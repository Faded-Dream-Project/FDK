/* XTEST input driver for the WM test rigs (xdotool's motion is broken
 * in this environment; direct XTestFake* calls work fine).
 *
 * Usage: xtest_driver <window-id> <command>...
 *   Commands (space-separated):
 *     m:<x>,<y>   move pointer to ROOT coords
 *     mw:<x>,<y>  move pointer to WINDOW-relative coords
 *     d           button 1 press
 *     u           button 1 release
 *     w:<ms>      sleep
 * Between other commands a default 120ms pause is inserted.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern Bool XTestQueryExtension(Display *, int *, int *, int *, int *);
extern Bool XTestFakeMotionEvent(Display *, int, int, int, unsigned long);
extern Bool XTestFakeButtonEvent(Display *, unsigned int, Bool, unsigned long);

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <window-id> <cmd...>\n", argv[0]);
        return 2;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    int e, er, ma, mi;
    if (!XTestQueryExtension(dpy, &e, &er, &ma, &mi)) {
        fprintf(stderr, "no XTEST\n"); return 1;
    }
    Window target = (Window)strtoul(argv[1], NULL, 0);

    /* self-check: fake one motion and read it back. OPT-IN (the
     * XTEST_SELFCHECK env var): the check MOVES the pointer, which
     * poisons any multi-invocation drag sequence — each call would
     * reset the pointer to (11,22) between the caller's own steps
     * (the §29 drag-rig mystery: every leg left its target and
     * cancelled). */
    if (getenv("XTEST_SELFCHECK") != NULL) {
        XTestFakeMotionEvent(dpy, 0, 11, 22, CurrentTime);
        XFlush(dpy);
        Window r, c; int rx, ry, wx, wy; unsigned int mask;
        XQueryPointer(dpy, DefaultRootWindow(dpy), &r, &c, &rx, &ry,
                      &wx, &wy, &mask);
        fprintf(stderr, "selfcheck: pointer at %d,%d (want 11,22)\n", rx, ry);
        if (rx != 11 || ry != 22) {
            fprintf(stderr, "XTEST motion NOT working\n");
            return 1;
        }
    }

    for (int i = 2; i < argc; i++) {
        const char *c = argv[i];
        if (strncmp(c, "m:", 2) == 0) {
            int x = 0, y = 0;
            sscanf(c + 2, "%d,%d", &x, &y);
            XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
            XFlush(dpy);
        } else if (strncmp(c, "mw:", 3) == 0) {
            int x = 0, y = 0;
            sscanf(c + 3, "%d,%d", &x, &y);
            Window child;
            int rx = 0, ry = 0;
            if (XTranslateCoordinates(dpy, target,
                                       DefaultRootWindow(dpy),
                                       x, y, &rx, &ry, &child)) {
                XTestFakeMotionEvent(dpy, 0, rx, ry, CurrentTime);
                XFlush(dpy);
            } else {
                fprintf(stderr, "translate failed\n");
            }
        } else if (strcmp(c, "d") == 0) {
            XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
            XFlush(dpy);
        } else if (strcmp(c, "u") == 0) {
            XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
            XFlush(dpy);
        } else if (strncmp(c, "w:", 2) == 0) {
            usleep((useconds_t)atoi(c + 2) * 1000);
            continue; /* explicit wait: no extra pause */
        } else {
            fprintf(stderr, "unknown cmd: %s\n", c);
            return 2;
        }
        usleep(120 * 1000);
    }
    return 0;
}
