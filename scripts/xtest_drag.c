/* xtest_drag.c — the field-report rig: drives the REAL example with
 * grab-respecting XTEST input (the SendEvent tests bypass the server
 * grab entirely — this driver does not). Sequence mirrors the user's
 * flow: warp to the scale button, click, warp onto the popup's
 * trough in motion steps, press, drag, release. Prints everything it
 * finds so the FDK log beside it tells the whole story. */
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Window find_window(Display *dpy, Window root, unsigned int w,
                          unsigned int h) {
    Window rw, pw, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &rw, &pw, &kids, &n)) {
        return 0;
    }
    Window found = 0;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes a;
        if (XGetWindowAttributes(dpy, kids[i], &a) &&
            a.map_state == IsViewable && (unsigned)a.width == w &&
            (unsigned)a.height == h) {
            found = kids[i];
            break;
        }
    }
    if (kids) {
        XFree(kids);
    }
    return found;
}

static void click(Display *dpy, int x, int y) {
    XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
    XSync(dpy, False);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        return 1;
    }
    Window root = DefaultRootWindow(dpy);
    int scr = DefaultScreen(dpy);

    /* The example's window: 380x190. Wait for it. */
    Window win = 0;
    for (int i = 0; i < 30 && win == 0; i++) {
        win = find_window(dpy, root, 380, 190);
        if (win == 0) {
            usleep(100000);
        }
    }
    if (win == 0) {
        printf("driver: no 380x190 window\n");
        return 1;
    }
    XWindowAttributes a;
    XGetWindowAttributes(dpy, win, &a);
    int wx = a.x, wy = a.y;
    {
        Window cw;
        XTranslateCoordinates(dpy, win, root, 0, 0, &wx, &wy, &cw);
    }
    printf("driver: main window 0x%lx at (%d,%d)\n",
           (unsigned long)win, wx, wy);

    /* The scale button sits in the content box: title band ~40px,
     * box padding 12 — aim at (34, 66) window-local. */
    click(dpy, wx + 34, wy + 66);
    usleep(300000);

    Window pop = find_window(dpy, root, 48, 176);
    if (pop == 0) {
        printf("driver: NO POPUP after the click\n");
        return 2;
    }
    int px, py;
    {
        Window cw;
        XTranslateCoordinates(dpy, pop, root, 0, 0, &px, &py, &cw);
    }
    printf("driver: popup 0x%lx at (%d,%d)\n", (unsigned long)pop, px,
           py);

    /* Approach in motion steps, press mid-trough, drag up, release. */
    XTestFakeMotionEvent(dpy, 0, px + 24, py + 76, CurrentTime);
    XSync(dpy, False);
    usleep(150000);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    usleep(150000);
    for (int s = 1; s <= 4; s++) {
        XTestFakeMotionEvent(dpy, 0, px + 24, py + 76 - s * 10,
                             CurrentTime);
        XSync(dpy, False);
        usleep(100000);
    }
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
    usleep(200000);

    pop = find_window(dpy, root, 48, 176);
    printf("driver: popup after drag: %s (0x%lx)\n",
           pop ? "STILL OPEN" : "GONE", (unsigned long)pop);
    /* Leave the mouse where a human's would be. */
    click(dpy, wx + 190, wy + 100);
    XCloseDisplay(dpy);
    return 0;
}
