/* occlude.c — round 19 rig tool: the REAL cover/uncover X path.
 *
 * Round 16's stage D tested XUnmapWindow (wm_hide): that regenerates
 * the window's backing on MAP, a different server path from
 * occlusion. The owner's trigger is a covering window (terminal on
 * top), which makes the server DISCARD the obscured window's pixels
 * (backing-store off), then deliver Expose events on uncover — the
 * exact sequence this tool drives:
 *
 *   1. create a full-geometry InputOutput occluder ABOVE the target
 *      (stack mode Above, sibling = target), opaque 32-bit plane,
 *      and map it -> the server retains the target's pixels only if
 *      backing store is on (Xvfb: off by default — same as most
 *      real compositors-off desktops);
 *   2. sleep (the "recording" window), then unmap the occluder
 *      -> the server sends Expose for every region it discarded;
 *   3. exit. The rig then snapshots the target and counts rows.
 *
 * Selection matches xsnap.c (-w 0x... / -t title-substring / largest
 * toplevel). Build like xsnap.c (libX11 from /tmp/x11dev).
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
            wa.map_state != IsViewable || wa.override_redirect) {
            continue;
        }
        if (title_sub != NULL) {
            XTextProperty tp;
            if (XGetWMName(d, kids[i], &tp) != 0 && tp.value != NULL &&
                tp.nitems > 0) {
                char name[256];
                size_t len = tp.nitems < sizeof name - 1
                                 ? tp.nitems : sizeof name - 1;
                memcpy(name, tp.value, len);
                name[len] = '\0';
                for (char *p = name; *p != '\0'; p++) {
                    *p = (char)tolower((unsigned char)*p);
                }
                char sub[256];
                snprintf(sub, sizeof sub, "%s", title_sub);
                for (char *p = sub; *p != '\0'; p++) {
                    *p = (char)tolower((unsigned char)*p);
                }
                XFree(tp.value);
                if (strstr(name, sub) == NULL) {
                    continue;
                }
            }
        }
        long area = (long)wa.width * (long)wa.height;
        if (area > best_area) {
            best_area = area;
            best = kids[i];
        }
    }
    if (kids != NULL) {
        XFree(kids);
    }
    return best;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        fprintf(stderr,
                "usage: %s <display> <cover-seconds> [-w 0xid | -t title]\n",
                argv[0]);
        return 2;
    }
    const char *dpy_name = argv[1];
    double secs = atof(argv[2]);
    Window want = None;
    const char *title_sub = NULL;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-w") == 0) {
            want = (Window)strtoul(argv[i + 1], NULL, 0);
        } else if (strcmp(argv[i], "-t") == 0) {
            title_sub = argv[i + 1];
        }
    }

    Display *d = XOpenDisplay(dpy_name);
    if (d == NULL) {
        fprintf(stderr, "occlude: cannot open %s\n", dpy_name);
        return 2;
    }
    Window target = find_window(d, title_sub, want);
    if (target == None) {
        fprintf(stderr, "occlude: no target window\n");
        XCloseDisplay(d);
        return 2;
    }

    XWindowAttributes wa;
    if (!XGetWindowAttributes(d, target, &wa)) {
        fprintf(stderr, "occlude: cannot query target\n");
        XCloseDisplay(d);
        return 2;
    }
    XSetWindowAttributes sa;
    sa.override_redirect = True;
    sa.background_pixel = BlackPixel(d, DefaultScreen(d));
    sa.event_mask = 0;
    /* Above the target, same geometry: full cover. override_redirect
     * keeps the (absent on Xvfb) WM out of the stacking war. */
    Window occ = XCreateWindow(
        d, wa.root, wa.x, wa.y, (unsigned)wa.width, (unsigned)wa.height,
        0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask, &sa);
    XWindowChanges xc;
    xc.stack_mode = Above;
    xc.sibling = target;
    XConfigureWindow(d, occ, CWStackMode | CWSibling, &xc);
    XMapWindow(d, occ);
    XFlush(d);
    printf("occlude: covering 0x%lx for %.1fs\n", (unsigned long)target,
           secs);
    fflush(stdout);

    usleep((useconds_t)(secs * 1e6));

    XUnmapWindow(d, occ);
    XDestroyWindow(d, occ);
    XFlush(d);
    printf("occlude: uncovered\n");
    XCloseDisplay(d);
    return 0;
}
