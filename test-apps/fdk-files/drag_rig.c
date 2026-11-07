/* drag_rig.c — the §27/2b input-injection rig: REAL XTEST pointer
 * drags against a running fdk-files, verifying the §26/3a wiring
 * end-to-end.
 *
 * Legs (each prints PASS/FAIL; any FAIL exits 1):
 *   D1  drag-between-panes: a real XTEST press on a file row in
 *       pane 1, motion across to pane 2, release — the file must
 *       APPEAR in pane 2's directory (the drop handler copies).
 *   D2  drag-to-bookmark: press on a folder row, motion to the
 *       sidebar's bookmark list, release — the bookmark count in
 *       the app's config file must grow by one.
 *   D3  bookmark reorder: drag bookmark row 1 below row 3 in the
 *       sidebar's own list — the saved order must change.
 *   D4  drag-out: drag a file onto the EXTERNAL xdnd_sink window
 *       (scripts/xdnd_sink, a raw-Xlib XDND peer started by
 *       run_drig.sh) — the sink must print the URI payload.
 *
 * The app must run with FDK_FILES_DRIG=1 (a §27 gate in main.c that
 * arranges a deterministic layout: split view ON, both panes in
 * /tmp/fdk-drig with known filenames, one bookmark pre-seeded) and
 * FDK_TESTAPP_FRAMES high enough to stay alive for the rig.
 *
 * Coordinates (measured against the DRIG screenshot, 1000x700 at
 * 96dpi, window parked at (0,0) — run_drig.sh verifies the origin):
 *   toolbar           y  8..19
 *   sidebar           x  7..149  (bookmarks list)
 *   left pane rows    x 153..369; row 1 (alpha.txt) y 124..138,
 *                      beta-dir's band y 377..436
 *   right pane        x 520..950 (empty dir: the drop target)
 *   bookmark rows     ~30px apart from y 113 (row 0 = the seed)
 *
 * Usage: drag_rig <display> <app-window-xid-decimal> <geo-log>
 *
 * The geometry comes from the app itself: main.c's DRIG gate prints
 * DRIG-GEO lines (each pane's list bounds + row count, the bookmarks
 * bounds) in WINDOW coordinates; the rig parses <geo-log> and adds
 * the window origin. No more assumed pixels.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>

static Display *dpy;
static Window app;

/* parsed from the app's DRIG-GEO output (window coordinates) */
static int pane0_y, pane0_rows;
static int pane1_y;
static int book_y, book_h;

static void parse_geo(const char *log) {
    FILE *f = fopen(log, "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        int x, y, w, h, rows;
        if (sscanf(line, "DRIG-GEO pane0 list %d %d %d %d rows=%d",
                   &x, &y, &w, &h, &rows) == 5) {
            pane0_y = y;
            pane0_rows = rows;
        } else if (sscanf(line,
                          "DRIG-GEO pane1 list %d %d %d %d rows=%d",
                          &x, &y, &w, &h, &rows) == 5) {
            pane1_y = y;
        } else if (sscanf(line, "DRIG-GEO bookmarks %d %d %d %d",
                          &x, &y, &w, &h) == 4) {
            book_y = y;
            book_h = h;
        }
    }
    fclose(f);
}

static void msleep(int ms) {
    struct timespec ts = {(time_t)(ms / 1000),
                          (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void click(int x, int y) {
    XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
    XSync(dpy, False);
    msleep(60);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    msleep(40);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
    msleep(120);
}

static void drag(int x1, int y1, int x2, int y2) {
    XTestFakeMotionEvent(dpy, 0, x1, y1, CurrentTime);
    XSync(dpy, False);
    msleep(80);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    msleep(80);
    /* the motion path: enough intermediate steps that both the
     * source's drag-begin threshold and the target's enter fire */
    int steps = 12;
    for (int i = 1; i <= steps; i++) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
        XSync(dpy, False);
        msleep(30);
    }
    msleep(120);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
    msleep(400); /* the drop handler's worker copy starts */
}

static int dir_has(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    struct dirent *de;
    int found = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, name) == 0) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] != '#' && line[0] != '\n') {
            n++;
        }
    }
    fclose(f);
    return n;
}

/* the line at `index' (1-based among non-comment lines), or NULL */
static char *line_at(const char *path, int index) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return NULL;
    }
    static char buf[512];
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] != '#' && line[0] != '\n') {
            n++;
            if (n == index) {
                snprintf(buf, sizeof buf, "%s", line);
                /* trim */
                size_t l = strlen(buf);
                while (l > 0 && (buf[l - 1] == '\n' ||
                                 buf[l - 1] == ' ')) {
                    buf[--l] = '\0';
                }
                fclose(f);
                return buf;
            }
        }
    }
    fclose(f);
    return NULL;
}

/* waits for `path' to contain a line with `needle'; up to secs */
static int wait_line_contains(const char *path, const char *needle,
                              int secs) {
    for (int i = 0; i < secs * 4; i++) {
        FILE *f = fopen(path, "r");
        if (f != NULL) {
            char line[512];
            while (fgets(line, sizeof line, f) != NULL) {
                if (strstr(line, needle) != NULL) {
                    fclose(f);
                    return 1;
                }
            }
            fclose(f);
        }
        msleep(250);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <display> <app-xid> <geo-log>\n",
                argv[0]);
        return 2;
    }
    dpy = XOpenDisplay(argv[1]);
    if (dpy == NULL) {
        fprintf(stderr, "FAIL: cannot open display %s\n", argv[1]);
        return 1;
    }
    app = (Window)strtoul(argv[2], NULL, 0);
    parse_geo(argv[3]);
    if (pane0_y == 0 || book_y == 0) {
        fprintf(stderr, "FAIL: no DRIG-GEO geometry in %s\n",
                argv[3]);
        return 1;
    }

    /* the window's REAL position (WM-less Xvfb places it wherever
     * the create request said; ask the server, don't assume) */
    Window root;
    int px, py;
    unsigned int pw, ph, bd, dep;
    if (!XGetGeometry(dpy, app, &root, &px, &py, &pw, &ph, &bd, &dep)) {
        fprintf(stderr, "FAIL: XGetGeometry on 0x%lx\n", app);
        return 1;
    }
    Window child;
    int tx, ty;
    XTranslateCoordinates(dpy, app, root, 0, 0, &tx, &ty, &child);
    int win_x = tx, win_y = ty;
    printf("window at (%d,%d) %ux%u\n", win_x, win_y, pw, ph);

    /* the app-reported geometry (DRIG-GEO): the panes are stacked
     * vertically; each list starts below its ~28px column header,
     * rows step ~30px. The bookmarks rows start at the list top. */
    int row1_x = win_x + 300; /* safely inside the pane's list    */
    int row1_y = win_y + pane0_y + 42; /* row 0 below the header   */
    int folder_x = win_x + 300;
    int folder_y = win_y + pane0_y + 72; /* row 1 = beta-dir       */
    int pane2_x = win_x + 300; /* pane1's list (the lower pane)    */
    int pane2_y = win_y + pane1_y + 42;
    int bookmark_x = win_x + 60;
    int bookmark_y = win_y + book_y + 15; /* row 0 = the seed      */
    (void)pane0_rows; (void)book_h;

    int rc = 0;

    /* ---- D1: drag-between-panes ---------------------------------- */
    /* the drag source arms only on a press over a SELECTED row:
     * click-select alpha.txt first, then press-and-move. */
    click(row1_x, row1_y);
    int before = dir_has("/tmp/fdk-drig/right", "alpha.txt");
    drag(row1_x, row1_y, pane2_x, pane2_y);
    int tries = 0;
    int after = 0;
    while (tries++ < 40) { /* the drop's copy may ride a worker */
        after = dir_has("/tmp/fdk-drig/right", "alpha.txt");
        if (after && !before) {
            break;
        }
        msleep(250);
    }
    if (!before && after) {
        printf("PASS D1: drag-between-panes copied alpha.txt to the "
               "right pane's dir\n");
    } else {
        printf("FAIL D1: alpha.txt not in /tmp/fdk-drig/right "
               "(before=%d after=%d)\n", before, after);
        rc = 1;
    }

    /* ---- D2: drag-to-bookmark ------------------------------------ */
    int marks_before = count_lines("/tmp/fdk-drig-bookmarks");
    drag(folder_x, folder_y, bookmark_x, bookmark_y);
    msleep(800);
    int marks_after = count_lines("/tmp/fdk-drig-bookmarks");
    if (marks_after == marks_before + 1) {
        printf("PASS D2: drag-to-bookmark added a row (bookmarks "
               "%d -> %d)\n", marks_before, marks_after);
    } else {
        printf("FAIL D2: bookmark count %d -> %d\n",
               marks_before, marks_after);
        rc = 1;
    }

    /* ---- D3: bookmark reorder ------------------------------------ */
    /* the sidebar's own list drag: press bookmark row 0 (the seed
     * /tmp/fdk-drig), drag onto row 2's band (below the just-added
     * bookmark), release. The saved file's line order must change:
     * /tmp/fdk-drig was line 1; afterwards it must NOT be line 1. */
    char *first_before = line_at("/tmp/fdk-drig-bookmarks", 1);
    int n_marks = count_lines("/tmp/fdk-drig-bookmarks");
    if (n_marks >= 2) {
        /* re-select row 0 (a drag from the bookmarks list needs the
         * row selected first, same arming rule) */
        click(bookmark_x, bookmark_y);
        drag(bookmark_x, bookmark_y, bookmark_x,
             bookmark_y + 2 * 30); /* row 2's band */
        msleep(800);
        char *first_after = line_at("/tmp/fdk-drig-bookmarks", 1);
        if (first_before != NULL && first_after != NULL &&
            strcmp(first_before, first_after) != 0) {
            printf("PASS D3: bookmark reorder moved '%s' off the "
                   "head (now '%s')\n", first_before, first_after);
        } else {
            printf("FAIL D3: first bookmark still '%s' (was '%s')\n",
                   first_after ? first_after : "(none)",
                   first_before ? first_before : "(none)");
            rc = 1;
        }
    } else {
        printf("FAIL D3: need >=2 bookmarks to reorder, have %d\n",
               n_marks);
        rc = 1;
    }

    /* ---- D4: drag out to an external application ------------------ */
    /* run_drig.sh starts scripts/xdnd_sink at (1020,100) 200x200 and
     * tees its stdout to /tmp/drig-sink.log; drag alpha.txt onto it
     * and the sink must print the URI payload. */
    click(row1_x, row1_y); /* re-select after the earlier gestures */
    drag(row1_x, row1_y, 1100, 180);
    if (wait_line_contains("/tmp/drig-sink.log", "payload uri-list",
                           20)) {
        printf("PASS D4: drag-out delivered the URI list to the "
               "external xdnd_sink window\n");
    } else {
        printf("FAIL D4: xdnd_sink never reported a payload "
               "(see /tmp/drig-sink.log)\n");
        rc = 1;
    }

    XCloseDisplay(dpy);
    if (rc == 0) {
        printf("drag rig: ALL PASS\n");
    }
    return rc;
}
