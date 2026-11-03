/* xexpose.c — round 14 rig tool: force a REAL XExpose on a window
 * (XClearArea with exposures=True). The blit-check's repaint twin:
 * after a blit-scrolled frame, one full-window expose forces the
 * app through its full-repaint path without changing any state —
 * the two frames must be pixel-identical. Window selection matches
 * xsnap.c (-w 0x... / -t title-substring / largest toplevel).
 * Build: like xsnap.c (libX11 from /tmp/x11dev).
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h> /* XGetPixel/XDestroyImage/fetch */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Window find_window(Display *d, const char *title_sub, Window want) {
    if (want != None) {
        return want;
    }
    Window root = DefaultRootWindow(d);
    Window dummy, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(d, root, &dummy, &dummy, &kids, &n) || kids == NULL) {
        return None;
    }
    Window best = None;
    long best_area = -1;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(d, kids[i], &wa) ||
            wa.map_state != IsViewable || wa.width < 8 || wa.height < 8) {
            continue;
        }
        if (title_sub != NULL) {
            char *name = NULL;
            if (XFetchName(d, kids[i], &name) && name != NULL) {
                char a[256], b[256];
                size_t j;
                for (j = 0; name[j] != '\0' && j < sizeof a - 1; j++) {
                    a[j] = (char)tolower((unsigned char)name[j]);
                }
                a[j] = '\0';
                for (j = 0; title_sub[j] != '\0' && j < sizeof b - 1; j++) {
                    b[j] = (char)tolower((unsigned char)title_sub[j]);
                }
                b[j] = '\0';
                XFree(name);
                if (strstr(a, b) != NULL) {
                    best = kids[i];
                    break;
                }
            } else if (name != NULL) {
                XFree(name);
            }
            continue;
        }
        long area = (long)wa.width * (long)wa.height;
        if (area > best_area) {
            best_area = area;
            best = kids[i];
        }
    }
    XFree(kids);
    return best;
}

int main(int argc, char **argv) {
    const char *dpy_name = NULL, *title_sub = NULL;
    Window want = None;
    int rx = 0, ry = 0, rw = 0, rh = 0; /* 0s = whole window */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dpy_name = argv[++i];
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            title_sub = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            want = (Window)strtoul(argv[++i], NULL, 0);
        } else {
            /* up to 4 positional: x y w h */
            int vals[4] = {0, 0, 0, 0};
            int got = 0;
            for (; i < argc && got < 4; i++, got++) {
                vals[got] = atoi(argv[i]);
            }
            rx = vals[0]; ry = vals[1]; rw = vals[2]; rh = vals[3];
            break;
        }
    }
    Display *d = XOpenDisplay(dpy_name);
    if (d == NULL) {
        fprintf(stderr, "xexpose: cannot open display\n");
        return 2;
    }
    Window w = find_window(d, title_sub, want);
    if (w == None) {
        fprintf(stderr, "xexpose: no window found\n");
        return 2;
    }
    if (rw <= 0 || rh <= 0) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(d, w, &wa)) {
            fprintf(stderr, "xexpose: attributes failed\n");
            return 2;
        }
        rx = 0; ry = 0; rw = wa.width; rh = wa.height;
    }
    fprintf(stderr, "xexpose: clearing 0x%lx rect %d,%d %dx%d\n",
            (unsigned long)w, rx, ry, rw, rh);
    XClearArea(d, w, rx, ry, (unsigned)rw, (unsigned)rh, True);
    XFlush(d);
    XCloseDisplay(d);
    return 0;
}
