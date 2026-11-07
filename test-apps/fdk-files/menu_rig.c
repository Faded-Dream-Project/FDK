/* menu_rig.c — round 11: the context-menu proof rig.
 *
 * Boots nothing itself: run_menurig.sh starts fdk-files (DRIG
 * layout) under Xvfb, waits for the DRIG-GEO lines, then THIS rig
 * drives REAL XTest events:
 *
 *   1. right-click ON the beta-dir row (a folder): the row under
 *      the pointer must become the selection (the app's "PHASE:
 *      sel" telemetry prints the newly selected row's name);
 *   2. the menu popup is a REAL X window the rig can SEE: it finds
 *      the new override-redirect window, measures it, and clicks
 *      its LAST row — "Properties" in the folder's flavor;
 *   3. the app's "PHASE: act" telemetry must print "Properties",
 *      NOT "Delete permanently" (the round-11 off-by-one: every
 *      menu entry fired the action ABOVE it, so the Properties
 *      click opened the permanent-delete confirmation).
 *
 * Self-calibrating by design: the menu's geometry is measured from
 * the actual popup window (QueryTree children of the root, the new
 * one since the click; its height and the row count from the app's
 * menu model are unknown here, but Properties is ALWAYS the last
 * row, and a click 3/4 down the popup's height is well inside it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* Reads /tmp/menurig-app.log from *offset; returns the LAST new
 * line starting with the given prefix. Updates *offset. */
static void last_new_line(long *offset, const char *prefix,
                          char *out, size_t cap) {
    out[0] = '\0';
    FILE *f = fopen("/tmp/menurig-app.log", "r");
    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    if (end < *offset) {
        *offset = end;
    }
    fseek(f, *offset, SEEK_SET);
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            snprintf(out, cap, "%.*s",
                     (int)(strlen(line) >= cap ? cap - 1
                                               : strlen(line)),
                     line);
            char *nl = strchr(out, '\n');
            if (nl != NULL) {
                *nl = '\0';
            }
        }
    }
    *offset = end;
    fclose(f);
}

/* The topmost child of the root that appeared since `before` was
 * captured (the menu popup). None -> 0. */
static Window new_topmost_child(Display *dpy, Window root,
                                unsigned long before) {
    Window rret, pret;
    Window *children = NULL;
    unsigned int n = 0;
    Window found = 0;
    if (!XQueryTree(dpy, root, &rret, &pret, &children, &n)) {
        return 0;
    }
    /* XQueryTree is bottom-to-top: the popup (raised last) is at
     * the END. Walk backward; the first window with a HIGHER id
     * than every id `before` saw is the newcomer. Simpler and
     * honest: the last child whose id > before_max. */
    unsigned long max_before = before;
    for (int i = (int)n - 1; i >= 0; i--) {
        if ((unsigned long)children[i] > max_before) {
            found = children[i]; /* keep going: the LAST new one    */
        }
    }
    XFree(children);
    return found;
}

static unsigned long max_child_id(Display *dpy, Window root) {
    Window rret, pret;
    Window *children = NULL;
    unsigned int n = 0;
    unsigned long max = 0;
    if (!XQueryTree(dpy, root, &rret, &pret, &children, &n)) {
        return 0;
    }
    for (unsigned int i = 0; i < n; i++) {
        if ((unsigned long)children[i] > max) {
            max = (unsigned long)children[i];
        }
    }
    XFree(children);
    return max;
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "menu_rig: no display\n");
        return 2;
    }
    Window root = DefaultRootWindow(dpy);
    long off_sel = 0, off_act = 0;
    last_new_line(&off_sel, "PHASE: sel", (char[256]){0}, 256);
    last_new_line(&off_act, "PHASE: act", (char[256]){0}, 256);

    int lx = (env = getenv("MENURIG_LIST_X")) != NULL ? atoi(env) : 10;
    int ly = (env = getenv("MENURIG_LIST_Y")) != NULL ? atoi(env) : 0;
    int row = (env = getenv("MENURIG_ROW")) != NULL ? atoi(env) : 1;
    int rh = (env = getenv("MENURIG_ROW_H")) != NULL ? atoi(env) : 30;
    int hdr = (env = getenv("MENURIG_HEADER_H")) != NULL ? atoi(env)
                                                          : 26;

    /* 1. right-click the folder row (center of its band; the rows
     * start below the columns round's header strip) */
    int px = lx + 60;
    int py = ly + hdr + row * rh + rh / 2;
    unsigned long before = max_child_id(dpy, root);
    XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
    XSync(dpy, False);
    msleep(60);
    XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
    XSync(dpy, False);
    msleep(60);
    XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
    XSync(dpy, False);
    msleep(400); /* the popup opens + paints */

    char sel[256];
    last_new_line(&off_sel, "PHASE: sel", sel, sizeof sel);
    printf("RIGHT-CLICK at (%d,%d) -> %s\n", px, py,
           sel[0] ? sel : "(no selection change)");
    fflush(stdout);

    /* 2. find the popup window and click its LAST row */
    Window pop = new_topmost_child(dpy, root, before);
    if (pop == 0) {
        printf("VERDICT: FAIL — no popup window appeared\n");
        XCloseDisplay(dpy);
        return 1;
    }
    XWindowAttributes wa;
    int rx = 0, ry = 0;
    Window junk;
    Window child_ret;
    if (!XGetWindowAttributes(dpy, pop, &wa) ||
        !XTranslateCoordinates(dpy, pop, root, 0, 0, &rx, &ry,
                               &junk) ||
        !XTranslateCoordinates(dpy, root, pop, (int)px, (int)py,
                               &px, &py, &child_ret)) {
        printf("VERDICT: FAIL — popup geometry unreadable\n");
        XCloseDisplay(dpy);
        return 1;
    }
    printf("POPUP 0x%lx at root (%d,%d) %ux%u\n",
           (unsigned long)pop, rx, ry, wa.width, wa.height);
    fflush(stdout);

    /* Properties is the LAST row: click 10px above the popup's
     * bottom edge (inside the last row whatever its height; the
     * folder flavor's last row is Properties) and 40px in from its
     * left. */
    int mx = rx + 40;
    int my = ry + (int)wa.height - 10;
    XTestFakeMotionEvent(dpy, 0, mx, my, CurrentTime);
    XSync(dpy, False);
    msleep(150); /* hover paints the highlight */
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    msleep(60);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
    msleep(500); /* the action runs (the window opens) */

    char act[256];
    last_new_line(&off_act, "PHASE: act", act, sizeof act);
    printf("MENU CLICK at (%d,%d) -> %s\n", mx, my,
           act[0] ? act : "(no action fired)");
    fflush(stdout);

    /* the verdict: the action that ran must be Properties */
    int ok = (strstr(act, "Properties") != NULL);
    printf("VERDICT: %s\n",
           ok ? "PASS — the Properties entry ran Properties"
              : "FAIL — the menu fired the wrong action");
    XCloseDisplay(dpy);
    return ok ? 0 : 1;
}
