/* miniwm.c — round 19 rig tool: the smallest honest reparenting WM.
 *
 * The cover/uncover disappearance could not be reproduced under bare
 * Xvfb (no WM) on either present lane — but the owner's session has
 * an EWMH window manager, and every real WM reparents toplevels into
 * frame windows. That changes the expose geometry (exposes arrive
 * for the frame AND the client), adds synthetic ConfigureNotify
 * traffic, and changes stacking semantics. This tool adds exactly
 * that delta and nothing else:
 *
 *   - substructure redirect on the root (the definition of a WM);
 *   - every MapRequest gets a frame (same geometry, client at 0,0 —
 *     zero decoration, so the rig's client-space coordinate math
 *     stays valid), client reparented in, both mapped;
 *   - ConfigureRequest passes through to the client and resize/moves
 *     the frame to match, with the synthetic ConfigureNotify a WM
 *     must send;
 *   - UnmapNotify/DestroyNotify tears the frame down;
 *   - no focus management, no compositing, no EWMH (the _NET atoms
 *     would need property scaffolding the bug doesn't care about).
 *
 * Build like xsnap.c (libX11 from /tmp/x11dev).
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Frame {
    Window frame;
    Window client;
    int x, y, w, h;
    struct Frame *next;
} Frame;

static Frame *frames = NULL;

/* The canonical WM-exists trap: selecting SubstructureRedirect on a
 * root another WM already owns raises BadAccess asynchronously —
 * the error handler flips this flag during the XSync round-trip. */
static int got_bad_access = 0;
static int wm_existing_error(Display *d, XErrorEvent *ev) {
    (void)d;
    if (ev->error_code == BadAccess) {
        got_bad_access = 1;
    }
    return 0;
}

static Frame *frame_of_client(Window c) {
    for (Frame *f = frames; f != NULL; f = f->next) {
        if (f->client == c) {
            return f;
        }
    }
    return NULL;
}

static Frame *frame_of_frame(Window w) {
    for (Frame *f = frames; f != NULL; f = f->next) {
        if (f->frame == w) {
            return f;
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <display>\n", argv[0]);
        return 2;
    }
    Display *d = XOpenDisplay(argv[1]);
    if (d == NULL) {
        fprintf(stderr, "miniwm: cannot open %s\n", argv[1]);
        return 2;
    }
    Window root = DefaultRootWindow(d);
    int (*old_handler)(Display *, XErrorEvent *) = XSetErrorHandler(
        wm_existing_error);
    XSelectInput(d, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(d, False);
    XSetErrorHandler(old_handler);
    if (got_bad_access) {
        fprintf(stderr, "miniwm: another WM is running\n");
        XCloseDisplay(d);
        return 2;
    }
    printf("miniwm: managing root 0x%lx\n", (unsigned long)root);
    fflush(stdout);

    XEvent ev;
    while (XPending(d) >= 0) {
        if (XPending(d) == 0) {
            usleep(2000);
            continue;
        }
        XNextEvent(d, &ev);
        if (ev.type == MapRequest) {
            Window c = ev.xmaprequest.window;
            if (frame_of_client(c) != NULL) {
                continue;
            }
            XWindowAttributes wa;
            if (!XGetWindowAttributes(d, c, &wa) ||
                wa.override_redirect) {
                XMapWindow(d, c);
                continue;
            }
            Frame *f = calloc(1, sizeof *f);
            if (f == NULL) {
                continue;
            }
            f->client = c;
            f->x = wa.x;
            f->y = wa.y;
            f->w = wa.width;
            f->h = wa.height;
            f->frame = XCreateSimpleWindow(
                d, root, f->x, f->y, (unsigned)f->w, (unsigned)f->h, 0,
                BlackPixel(d, DefaultScreen(d)),
                WhitePixel(d, DefaultScreen(d)));
            XSelectInput(d, f->frame, ExposureMask);
            XAddToSaveSet(d, c);
            XReparentWindow(d, c, f->frame, 0, 0);
            XMapWindow(d, f->frame);
            XMapWindow(d, c);
            f->next = frames;
            frames = f;
            printf("miniwm: framed 0x%lx -> 0x%lx %dx%d+%d+%d\n",
                   (unsigned long)c, (unsigned long)f->frame, f->w, f->h,
                   f->x, f->y);
            fflush(stdout);
        } else if (ev.type == ConfigureRequest) {
            Window c = ev.xconfigurerequest.window;
            Frame *f = frame_of_client(c);
            unsigned long vm = ev.xconfigurerequest.value_mask;
            XWindowChanges wc;
            wc.x = (vm & CWX) ? ev.xconfigurerequest.x : 0;
            wc.y = (vm & CWY) ? ev.xconfigurerequest.y : 0;
            wc.width = (vm & CWWidth) ? ev.xconfigurerequest.width : 0;
            wc.height = (vm & CWHeight) ? ev.xconfigurerequest.height : 0;
            wc.sibling = (vm & CWSibling) ? ev.xconfigurerequest.above : 0;
            wc.stack_mode =
                (vm & CWStackMode) ? ev.xconfigurerequest.detail : 0;
            if (f != NULL) {
                if (vm & CWX) f->x = wc.x;
                if (vm & CWY) f->y = wc.y;
                if (vm & CWWidth) f->w = wc.width;
                if (vm & CWHeight) f->h = wc.height;
                XConfigureWindow(d, c, vm & ~CWSibling, &wc);
                XMoveResizeWindow(d, f->frame, f->x, f->y,
                                  (unsigned)f->w, (unsigned)f->h);
            } else {
                XConfigureWindow(d, c, vm, &wc);
            }
            /* the synthetic ConfigureNotify every WM owes the client */
            XEvent cn;
            memset(&cn, 0, sizeof cn);
            cn.type = ConfigureNotify;
            cn.xconfigure.display = d;
            cn.xconfigure.event = c;
            cn.xconfigure.window = c;
            if (f != NULL) {
                cn.xconfigure.x = f->x;
                cn.xconfigure.y = f->y;
                cn.xconfigure.width = f->w;
                cn.xconfigure.height = f->h;
            } else {
                cn.xconfigure.x = wc.x;
                cn.xconfigure.y = wc.y;
                cn.xconfigure.width = wc.width;
                cn.xconfigure.height = wc.height;
            }
            cn.xconfigure.border_width = 0;
            cn.xconfigure.above = (f != NULL) ? f->frame : None;
            cn.xconfigure.override_redirect = False;
            XSendEvent(d, c, False, StructureNotifyMask, &cn);
        } else if (ev.type == UnmapNotify &&
                   ev.xunmap.event == ev.xunmap.window) {
            /* a client unmapped itself: reparent home, drop the frame */
            Window c = ev.xunmap.window;
            Frame *f = frame_of_client(c);
            if (f != NULL) {
                XReparentWindow(d, c, root, f->x, f->y);
                XDestroyWindow(d, f->frame);
                /* unlink */
                Frame **pp = &frames;
                while (*pp != NULL && *pp != f) {
                    pp = &(*pp)->next;
                }
                if (*pp != NULL) {
                    *pp = f->next;
                }
                free(f);
            }
        } else if (ev.type == DestroyNotify) {
            Window c = ev.xdestroywindow.window;
            Frame *f = frame_of_client(c);
            if (f != NULL) {
                XDestroyWindow(d, f->frame);
                Frame **pp = &frames;
                while (*pp != NULL && *pp != f) {
                    pp = &(*pp)->next;
                }
                if (*pp != NULL) {
                    *pp = f->next;
                }
                free(f);
            }
        } else if (ev.type == Expose) {
            /* frame decoration: a flat fill (the rig only cares that
             * the frame consumes its own exposes like a real WM) */
            Frame *f = frame_of_frame(ev.xexpose.window);
            if (f != NULL && ev.xexpose.count == 0) {
                /* nothing to draw — the white background pixel stands */
                (void)f;
            }
        }
        XFlush(d);
    }
    XCloseDisplay(d);
    return 0;
}
