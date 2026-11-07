/* xfocus.c — round 20 rig tool: pull the input focus OFF a window
 * (the "switched to a terminal" half of the owner's trigger, without
 * needing a second app). XSetInputFocus to the root window with
 * RevertToPointerRoot, exactly what a sloppy-focus WM does when the
 * pointer leaves. Build like xsnap.c (libX11 from /tmp/x11dev).
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <display>\n", argv[0]);
        return 2;
    }
    Display *d = XOpenDisplay(argv[1]);
    if (d == NULL) {
        fprintf(stderr, "xfocus: cannot open %s\n", argv[1]);
        return 2;
    }
    XSetInputFocus(d, DefaultRootWindow(d), RevertToPointerRoot,
                   CurrentTime);
    XFlush(d);
    XCloseDisplay(d);
    printf("xfocus: focus moved to root\n");
    return 0;
}
