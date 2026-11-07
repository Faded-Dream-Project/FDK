/* type_rig.c — round 23: fake-keys a string into the focused
 * window via XTEST (the owner's "I can't see what I type" repro).
 * Usage: type_rig <window-id> <text> — activates the window,
 * clicks once inside its client area (top-left + 60,120) to focus
 * the textview, then types each character via its keysym. */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <window-id> <text>\n", argv[0]);
        return 2;
    }
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    int e, er, ma, mi;
    if (!XTestQueryExtension(dpy, &e, &er, &ma, &mi)) {
        fprintf(stderr, "no XTEST\n");
        return 1;
    }
    Window target = (Window)strtoul(argv[1], NULL, 0);

    /* activate + focus the target */
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, target, &wa)) {
        fprintf(stderr, "attributes failed\n");
        return 1;
    }
    Window r, c; int rx, ry, wx, wy; unsigned int mask;
    if (XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy),
                              400, 300, &rx, &ry, &c)) {
        XTestFakeMotionEvent(dpy, 0, rx, ry, CurrentTime);
        XSync(dpy, False);
        msleep(60);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(200);
    } else {
        (void)r; (void)rx; (void)ry; (void)wx; (void)wy; (void)mask;
    }

    /* type via XStringToKeysym: printable ASCII maps cleanly */
    for (const char *p = argv[2]; *p != '\0'; p++) {
        char buf[2] = {*p, 0};
        KeySym ks = XStringToKeysym(buf);
        if (ks == NoSymbol) {
            if (*p == ' ') {
                ks = XK_space;
            } else {
                continue;
            }
        }
        KeyCode kc = XKeysymToKeycode(dpy, ks);
        if (kc == 0) {
            continue;
        }
        Bool shift = (*p >= 'A' && *p <= 'Z');
        if (shift) {
            XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L),
                              True, CurrentTime);
            XSync(dpy, False);
            msleep(20);
        }
        XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
        XSync(dpy, False);
        msleep(25);
        XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
        XSync(dpy, False);
        msleep(25);
        if (shift) {
            XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, XK_Shift_L),
                              False, CurrentTime);
            XSync(dpy, False);
            msleep(20);
        }
    }
    XFlush(dpy);
    fprintf(stderr, "typed %zu chars into 0x%lx\n",
            strlen(argv[2]), (unsigned long)target);
    return 0;
}
