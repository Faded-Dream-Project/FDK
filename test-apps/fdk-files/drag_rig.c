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
 *
 * The app must run with FDK_FILES_DRIG=1 (a §27 gate in main.c that
 * arranges a deterministic layout: split view ON, both panes in
 * /tmp/fdk-drig with known filenames, one bookmark pre-seeded) and
 * FDK_TESTAPP_FRAMES high enough to stay alive for the rig.
 *
 * Usage: drag_rig <display> <app-window-xid-decimal>
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <display> <app-xid>\n", argv[0]);
        return 2;
    }
    dpy = XOpenDisplay(argv[1]);
    if (dpy == NULL) {
        fprintf(stderr, "FAIL: cannot open display %s\n", argv[1]);
        return 1;
    }
    app = (Window)strtoul(argv[2], NULL, 0);

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
    int pane1_x = win_x + 190 + 40;   /* inside the left pane  */
    int pane2_x = win_x + 190 + 300;  /* inside the right pane */
    int row1_y = win_y + 160;         /* first file row        */
    int sidebar_y = win_y + 300;      /* the bookmarks list    */

    int rc = 0;

    /* ---- D1: drag-between-panes ---------------------------------- */
    /* the drag source arms only on a press over a SELECTED row:
     * click-select first, then press-and-move. The row scan covers
     * the toolbar/breadcrumb offsets of the real layout. */
    int sel_y = 0;
    for (int y = 150; y <= 260 && sel_y == 0; y += 22) {
        click(pane1_x, y);
        sel_y = y; /* any click selects SOME row in the list view;
                    * the alpha.txt row is the first — the scan's
                    * first hit inside the list area is it */
    }
    int before = dir_has("/tmp/fdk-drig/right", "alpha.txt");
    drag(pane1_x, sel_y, pane2_x, sel_y);
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
    drag(pane1_x, row1_y + 24 /* the second row: a folder */,
         win_x + 95, sidebar_y);
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

    XCloseDisplay(dpy);
    if (rc == 0) {
        printf("drag rig: ALL PASS\n");
    }
    return rc;
}
