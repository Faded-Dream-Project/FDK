/* key_rig.c — round 24: press ONE key (or a short string) into the
 * focused window, with a controllable pause. Usage:
 *   key_rig <window-id> <key>     — key is "space", "enter", or a
 *                                   literal string typed with pauses
 * The window is activated + clicked at its middle first (focus).
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void msleep(int ms) { usleep((useconds_t)ms * 1000); }

static void press(Display *dpy, KeySym ks) {
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    if (kc == 0) {
        fprintf(stderr, "no keycode for keysym %lu\n",
                (unsigned long)ks);
        return;
    }
    XTestFakeKeyEvent(dpy, kc, True, CurrentTime);
    XSync(dpy, False);
    msleep(30);
    XTestFakeKeyEvent(dpy, kc, False, CurrentTime);
    XSync(dpy, False);
    msleep(30);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <window-id> <key|string>\n", argv[0]);
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

    /* activate + click the middle of the window (focus the editor)
     * — only when KEY_RIG_CLICK=1: re-clicking between keys would
     * land a textview pointer-down and yank the caret (round 24's
     * rig artifact). */
    Window r, c; int rx, ry, wx, wy; unsigned int mask;
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, target, &wa)) {
        fprintf(stderr, "attributes failed\n");
        return 1;
    }
    if (getenv("KEY_RIG_CLICK") != NULL &&
        XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy),
                              400, 300, &rx, &ry, &c)) {
        XTestFakeMotionEvent(dpy, 0, rx, ry, CurrentTime);
        XSync(dpy, False);
        msleep(60);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(40);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(250);
    } else {
        (void)r; (void)wx; (void)wy; (void)mask;
    }

    const char *k = argv[2];
    if (strncmp(k, "ctrl+", 5) == 0) {
        /* "ctrl+g": hold Ctrl, press the letter, release */
        KeyCode ctl = XKeysymToKeycode(dpy, XK_Control_L);
        XTestFakeKeyEvent(dpy, ctl, True, CurrentTime);
        XSync(dpy, False);
        msleep(20);
        char buf[2] = {k[5], 0};
        press(dpy, XStringToKeysym(buf));
        XTestFakeKeyEvent(dpy, ctl, False, CurrentTime);
        XSync(dpy, False);
        msleep(30);
    } else if (strcmp(k, "escape") == 0) {
        press(dpy, XK_Escape);
    } else if (strcmp(k, "space") == 0) {
        press(dpy, XK_space);
    } else if (strcmp(k, "enter") == 0) {
        press(dpy, XK_Return);
    } else if (strcmp(k, "left") == 0) {
        press(dpy, XK_Left);
    } else if (strcmp(k, "right") == 0) {
        press(dpy, XK_Right);
    } else {
        /* a literal string, 40ms between chars */
        for (const char *p = k; *p != '\0'; p++) {
            char buf[2] = {*p, 0};
            KeySym ks = XStringToKeysym(buf);
            if (ks == NoSymbol && *p == ' ') {
                ks = XK_space;
            }
            if (ks == NoSymbol) {
                continue;
            }
            Bool shift = (*p >= 'A' && *p <= 'Z');
            if (shift) {
                XTestFakeKeyEvent(
                    dpy, XKeysymToKeycode(dpy, XK_Shift_L),
                    True, CurrentTime);
                XSync(dpy, False);
                msleep(20);
            }
            press(dpy, ks);
            if (shift) {
                XTestFakeKeyEvent(
                    dpy, XKeysymToKeycode(dpy, XK_Shift_L),
                    False, CurrentTime);
                XSync(dpy, False);
                msleep(20);
            }
        }
    }
    XFlush(dpy);
    fprintf(stderr, "key_rig: sent '%s' to 0x%lx\n", k,
            (unsigned long)target);
    return 0;
}
