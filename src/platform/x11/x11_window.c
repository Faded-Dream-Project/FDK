#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <string.h>

#define X11_DEFAULT_WIDTH  640
#define X11_DEFAULT_HEIGHT 480
#define X11_DEFAULT_TITLE  "FDK Application"

fdk_result fdk_x11_window_create(fdk_platform_connection *conn,
                                     const fdk_window_options *options,
                                     fdk_platform_window **out_pwindow) {
    fdk_i32 width = X11_DEFAULT_WIDTH;
    fdk_i32 height = X11_DEFAULT_HEIGHT;
    const char *title = X11_DEFAULT_TITLE;

    if (options != NULL) {
        if (options->width > 0)  width = options->width;
        if (options->height > 0) height = options->height;
        if (options->title != NULL) title = options->title;
    }

    unsigned long black = BlackPixel(conn->display, conn->screen);
    unsigned long white = WhitePixel(conn->display, conn->screen);

    Window xwindow = XCreateSimpleWindow(
        conn->display, conn->root,
        0, 0, (unsigned int)width, (unsigned int)height,
        0 /* border width */, black, white);

    if (xwindow == 0) {
        FDK_ERROR("XCreateSimpleWindow failed");
        return FDK_ERR_WINDOW_CREATE;
    }

    /* Select the input events FDK's event model in fdk_event.h can
     * translate. StructureNotifyMask gets us ConfigureNotify (resize).
     * ExposureMask gets us Expose → FDK_EVENT_WINDOW_EXPOSE so
     * rendered content (fdk_surface) can repaint regions the X
     * server no longer retains — see fdk_event.h. PropertyChangeMask
     * gets us PropertyNotify on OUR windows, which is how a real WM
     * talks state back: _NET_WM_STATE (maximized) and WM_STATE
     * (iconic) are properties the WM rewrites on our window, and
     * FDK re-reads them on change (see x11_events.c). Not selecting
     * VisibilityChangeMask/etc — those don't map to any event
     * fdk_event.h defines yet. */
    XSelectInput(conn->display, xwindow,
                 StructureNotifyMask | ExposureMask | FocusChangeMask |
                 KeyPressMask | KeyReleaseMask | PointerMotionMask |
                 ButtonPressMask | ButtonReleaseMask | EnterWindowMask |
                 LeaveWindowMask | PropertyChangeMask);

    /* Register for WM_DELETE_WINDOW so close requests arrive as an
     * event we can translate rather than killing the connection. */
    XSetWMProtocols(conn->display, xwindow, &conn->wm_delete_window, 1);

    XStoreName(conn->display, xwindow, title);
    /* Also set _NET_WM_NAME/UTF8_STRING for correct non-ASCII title
     * rendering under EWMH-compliant window managers; XStoreName above
     * is the ICCCM fallback for WMs that don't read _NET_WM_NAME. */
    XChangeProperty(conn->display, xwindow, conn->net_wm_name, conn->utf8_string,
                     8, PropModeReplace,
                     (const unsigned char *)title, (int)strlen(title));

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        XDestroyWindow(conn->display, xwindow);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    pwindow->conn = conn;
    pwindow->xwindow = xwindow;
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->has_saved = 0;
    pwindow->saved_x = 0;
    pwindow->saved_y = 0;
    pwindow->saved_w = 0;
    pwindow->saved_h = 0;
    pwindow->render_slots[0].image = NULL;
    pwindow->render_slots[1].image = NULL;
    pwindow->render_slots[0].malloc_data = NULL;
    pwindow->render_slots[1].malloc_data = NULL;
    pwindow->render_slots[0].shm_attached = 0;
    pwindow->render_slots[1].shm_attached = 0;
    pwindow->render_slots[0].in_flight = 0;
    pwindow->render_slots[1].in_flight = 0;
    pwindow->render_back = 0;
    pwindow->render_gc = NULL;
    pwindow->render_size.width = 0;
    pwindow->render_size.height = 0;

    fdk_result r = fdk_x11_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        XDestroyWindow(conn->display, xwindow);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("window created (0x%lx, %dx%d, \"%s\")", xwindow, width, height, title);

    *out_pwindow = pwindow;
    return FDK_OK;
}

void fdk_x11_window_destroy(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    fdk_x11_surface_cleanup(pwindow);
    fdk_x11_unregister_window(pwindow->conn, pwindow);
    XDestroyWindow(pwindow->conn->display, pwindow->xwindow);
    /* XSync (not just XFlush) ensures the destroy request has been
     * sent AND acknowledged by the server before this function
     * returns, rather than leaving it sitting in Xlib's client-side
     * write buffer. Without this, a caller that destroys a window and
     * then immediately calls fdk_shutdown() (which calls
     * fdk_x11_disconnect() -> XCloseDisplay()) can race: XCloseDisplay
     * closing the socket before the destroy request was actually
     * flushed corrupts the teardown from the server's point of view.
     * Reproduced directly against Xvfb (which is more sensitive to
     * this than a full Xorg server tends to be in practice) — see
     * docs/testing.md, "Known Xvfb flakiness", for the investigation
     * that found this. */
    XSync(pwindow->conn->display, False);
    fdk_free(pwindow);
}

void fdk_x11_window_show(fdk_platform_window *pwindow) {
    XMapWindow(pwindow->conn->display, pwindow->xwindow);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_hide(fdk_platform_window *pwindow) {
    XUnmapWindow(pwindow->conn->display, pwindow->xwindow);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_set_title(fdk_platform_window *pwindow, const char *title) {
    if (title == NULL) {
        title = "";
    }
    XStoreName(pwindow->conn->display, pwindow->xwindow, title);
    XChangeProperty(pwindow->conn->display, pwindow->xwindow,
                     pwindow->conn->net_wm_name, pwindow->conn->utf8_string,
                     8, PropModeReplace,
                     (const unsigned char *)title, (int)strlen(title));
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    XResizeWindow(pwindow->conn->display, pwindow->xwindow,
                  (unsigned int)width, (unsigned int)height);
    XFlush(pwindow->conn->display);
}

/* _MOTIF_WM_HINTS (MwmHints): the ICCCM-adjacent mechanism every
 * major X WM honors for turning server-side chrome off (and back
 * on). layout: {flags, functions, decorations, input_mode, status},
 * all CARD32. We only ever touch DECORATIONS. "On" deletes the
 * property entirely so the WM returns to its default treatment. */
#define X11_MWM_HINTS_DECORATIONS (1L << 1)

fdk_result fdk_x11_window_set_wm_decorations(fdk_platform_window *pwindow,
                                             bool on) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    Display *dpy = pwindow->conn->display;
    if (on) {
        XDeleteProperty(dpy, pwindow->xwindow,
                        pwindow->conn->motif_wm_hints);
    } else {
        long hints[5] = {
            X11_MWM_HINTS_DECORATIONS, /* flags: we set decorations */
            0,                          /* functions: leave alone    */
            0,                          /* decorations: NONE         */
            0,                          /* input mode: unchanged     */
            0,                          /* status                    */
        };
        XChangeProperty(dpy, pwindow->xwindow,
                        pwindow->conn->motif_wm_hints,
                        pwindow->conn->motif_wm_hints, 32, PropModeReplace,
                        (const unsigned char *)hints, 5);
    }
    XFlush(dpy);
    return FDK_OK;
}

fdk_result fdk_x11_window_get_position(fdk_platform_window *pwindow,
                                       fdk_i32 *out_x, fdk_i32 *out_y) {
    if (pwindow == NULL || out_x == NULL || out_y == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    Window child = 0;
    int x = 0, y = 0;
    if (!XTranslateCoordinates(pwindow->conn->display, pwindow->xwindow,
                               pwindow->conn->root, 0, 0, &x, &y,
                               &child)) {
        return FDK_ERR_PLATFORM_INIT; /* same-display failure */
    }
    *out_x = x;
    *out_y = y;
    return FDK_OK;
}

void fdk_x11_window_move_to(fdk_platform_window *pwindow, fdk_i32 x,
                            fdk_i32 y) {
    if (pwindow == NULL) {
        return;
    }
    XMoveWindow(pwindow->conn->display, pwindow->xwindow, x, y);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_set_size_limits(fdk_platform_window *pwindow,
                                        fdk_size min_size, fdk_size max_size) {
    XSizeHints *hints = XAllocSizeHints();
    if (hints == NULL) {
        FDK_ERROR("XAllocSizeHints failed");
        return;
    }

    hints->flags = 0;
    if (min_size.width > 0 || min_size.height > 0) {
        hints->flags |= PMinSize;
        hints->min_width = min_size.width > 0 ? min_size.width : 1;
        hints->min_height = min_size.height > 0 ? min_size.height : 1;
    }
    if (max_size.width > 0 || max_size.height > 0) {
        hints->flags |= PMaxSize;
        /* 0 in only one dimension of max_size means "no max in this
         * dimension"; approximate "unbounded" with a very large value
         * since XSizeHints has no explicit "no limit" sentinel once
         * PMaxSize is set at all. */
        hints->max_width = max_size.width > 0 ? max_size.width : 100000;
        hints->max_height = max_size.height > 0 ? max_size.height : 100000;
    }

    XSetWMNormalHints(pwindow->conn->display, pwindow->xwindow, hints);
    XFree(hints);
    XFlush(pwindow->conn->display);
}

/* ---- Phase 8: window-state management ----
 *
 * Two worlds, keyed off the connect-time EWMH probe:
 *
 *  - An EWMH WM is running: everything is a root-window ClientMessage
 *    request (_NET_WM_STATE for maximize, _NET_WM_MOVERESIZE for
 *    interactive move/resize, XIconifyWindow's WM_CHANGE_STATE for
 *    minimize) and the WM owns the outcome — it rewrites the
 *    _NET_WM_STATE / WM_STATE properties on our window, the
 *    PropertyNotify lands in x11_events.c, and ONLY the resulting
 *    property-derived state flips dispatch FDK_EVENT_WINDOW_STATE.
 *    A WM that ignores a request produces no event, and
 *    fdk_window_is_maximized() keeps telling the truth.
 *
 *  - Bare X (no WM — Xvfb, embedded kiosks): nobody is listening on
 *    the root, so FDK performs the action itself — move+resize to the
 *    full screen (geometry saved for restore) or unmap for minimize —
 *    and dispatches the state event directly, because in this world
 *    FDK's action IS the outcome.
 */

/* Compares the given state against the cached flags; on an actual
 * FLIP, updates the cache and dispatches FDK_EVENT_WINDOW_STATE.
 * Shared by the fallback paths here (where FDK's own action IS the
 * outcome) and the PropertyNotify translation in x11_events.c (where
 * the WM's property rewrite is the outcome). */
void fdk_x11_window_update_state(fdk_platform_window *pwindow,
                                 int maximized, int minimized) {
    if (pwindow->maximized == maximized &&
        pwindow->minimized == minimized) {
        return;
    }
    pwindow->maximized = maximized;
    pwindow->minimized = minimized;
    fdk_event_data event;
    memset(&event, 0, sizeof event);
    event.type = FDK_EVENT_WINDOW_STATE;
    event.state.maximized = maximized;
    event.state.minimized = minimized;
    pwindow->conn->dispatch(pwindow, &event,
                            pwindow->conn->dispatch_user_data);
}

/* EWMH ClientMessage to the root window — the sanctioned way clients
 * ask a WM to act. Event mask per the EWMH spec
 * (SubstructureNotifyMask | SubstructureRedirectMask). */
static void send_root_message(fdk_platform_window *pwindow, Atom type,
                              long l0, long l1, long l2, long l3) {
    XEvent msg;
    memset(&msg, 0, sizeof msg);
    msg.type = ClientMessage;
    msg.xclient.window = pwindow->xwindow;
    msg.xclient.message_type = type;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = l0;
    msg.xclient.data.l[1] = l1;
    msg.xclient.data.l[2] = l2;
    msg.xclient.data.l[3] = l3;
    msg.xclient.data.l[4] = 0;
    XSendEvent(pwindow->conn->display, pwindow->conn->root, False,
               (long)(SubstructureNotifyMask | SubstructureRedirectMask),
               &msg);
    XFlush(pwindow->conn->display);
}

/* Reads the maximized bits out of the window's _NET_WM_STATE property
 * (a list of state atoms). Returns -1 when the property is absent or
 * unreadable (not maximized as far as anyone can tell). */
int fdk_x11_window_net_state_maximized(fdk_platform_window *pwindow) {
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    int maximized = 0;
    if (XGetWindowProperty(pwindow->conn->display, pwindow->xwindow,
                           pwindow->conn->net_wm_state,
                           0, 64, False, XA_ATOM, &type, &format,
                           &nitems, &bytes_after, &prop) == Success &&
        type == XA_ATOM && format == 32) {
        Atom *atoms = (Atom *)prop;
        int vert = 0, horiz = 0;
        for (unsigned long i = 0; i < nitems; i++) {
            if (atoms[i] == pwindow->conn->net_wm_state_maximized_vert) {
                vert = 1;
            }
            if (atoms[i] == pwindow->conn->net_wm_state_maximized_horiz) {
                horiz = 1;
            }
        }
        /* "Maximized" means BOTH axes — VERT alone is a half-maximize
         * (drag-to-edge snap), which FDK does not model as maximized. */
        maximized = (vert && horiz) ? 1 : 0;
    }
    if (prop != NULL) {
        XFree(prop);
    }
    return maximized;
}

/* Reads WM_STATE's first CARD32 (NormalState=1 / IconicState=3) —
 * the ICCCM's own iconic tracking, maintained by every WM that
 * implements iconification. Returns -1 when unreadable. */
int fdk_x11_window_wm_state_iconic(fdk_platform_window *pwindow) {
    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = NULL;
    int iconic = -1;
    if (XGetWindowProperty(pwindow->conn->display, pwindow->xwindow,
                           pwindow->conn->wm_state,
                           0, 2, False, AnyPropertyType, &type, &format,
                           &nitems, &bytes_after, &prop) == Success &&
        prop != NULL && nitems >= 1 &&
        (type == XA_INTEGER || type == pwindow->conn->wm_state)) {
        /* WM_STATE is {CARD32 state, WINDOW icon}; the first long is
         * the state. Xlib hands format-32 data back as longs. */
        long state = *(long *)(void *)prop;
        iconic = (state == 3 /* IconicState */) ? 1 : 0;
    }
    if (prop != NULL) {
        XFree(prop);
    }
    return iconic;
}

fdk_result fdk_x11_window_set_maximized(fdk_platform_window *pwindow,
                                        bool maximized) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int want = maximized ? 1 : 0;
    if (want == pwindow->maximized) {
        return FDK_OK; /* idempotent request */
    }

    Display *dpy = pwindow->conn->display;
    if (pwindow->conn->ewmh_wm && pwindow->conn->ewmh_state_ok) {
        /* _NET_WM_STATE add/remove of the two maximized atoms in one
         * message; data.l[3] = 1 (source indication: application). */
        send_root_message(pwindow, pwindow->conn->net_wm_state,
                          want ? 1L /* _NET_WM_STATE_ADD */
                               : 0L /* _NET_WM_STATE_REMOVE */,
                          (long)pwindow->conn->net_wm_state_maximized_vert,
                          (long)pwindow->conn->net_wm_state_maximized_horiz,
                          1L);
        return FDK_OK; /* state change, if any, arrives via
                         PropertyNotify -> dispatch_state */
    }

    /* Bare X: nobody is listening on root, so FDK is the WM. */
    if (want) {
        Window child = 0;
        int x = 0, y = 0;
        if (!XTranslateCoordinates(dpy, pwindow->xwindow,
                                   pwindow->conn->root, 0, 0, &x, &y,
                                   &child)) {
            return FDK_ERR_PLATFORM_INIT;
        }
        pwindow->saved_x = x;
        pwindow->saved_y = y;
        pwindow->saved_w = pwindow->last_size.width;
        pwindow->saved_h = pwindow->last_size.height;
        pwindow->has_saved = 1;
        XMoveResizeWindow(dpy, pwindow->xwindow, 0, 0,
                          (unsigned int)DisplayWidth(dpy, pwindow->conn->screen),
                          (unsigned int)DisplayHeight(dpy, pwindow->conn->screen));
    } else {
        if (!pwindow->has_saved) {
            return FDK_OK; /* nothing to restore to; leave as-is */
        }
        XMoveResizeWindow(dpy, pwindow->xwindow,
                          pwindow->saved_x, pwindow->saved_y,
                          (unsigned int)pwindow->saved_w,
                          (unsigned int)pwindow->saved_h);
    }
    XFlush(dpy);
    fdk_x11_window_update_state(pwindow, want, pwindow->minimized);
    return FDK_OK;
}

fdk_result fdk_x11_window_set_minimized(fdk_platform_window *pwindow,
                                        bool minimized) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int want = minimized ? 1 : 0;
    if (want == pwindow->minimized) {
        return FDK_OK;
    }

    Display *dpy = pwindow->conn->display;
    if (want) {
        if (pwindow->conn->ewmh_wm) {
            /* XIconifyWindow sends the ICCCM WM_CHANGE_STATE(Iconic)
             * client message to the root — honored by every WM, EWMH
             * or not. The WM then maintains WM_STATE on our window;
             * the PropertyNotify keeps our flag honest (a WM that
             * refuses to iconify never touches it). */
            if (!XIconifyWindow(dpy, pwindow->xwindow,
                                pwindow->conn->screen)) {
                return FDK_ERR_PLATFORM_INIT;
            }
            XFlush(dpy);
        } else {
            /* Bare X: no WM to manage icons — unmap is the honest
             * equivalent (the window leaves the screen; restore maps
             * it back). */
            XUnmapWindow(dpy, pwindow->xwindow);
            XFlush(dpy);
        }
    } else {
        XMapWindow(dpy, pwindow->xwindow);
        XFlush(dpy);
    }
    /* Optimistic flip under a WM (WM_STATE's PropertyNotify will
     * agree — no second event — or correct us); the actual outcome
     * under bare X. */
    fdk_x11_window_update_state(pwindow, pwindow->maximized, want);
    return FDK_OK;
}

void fdk_x11_window_move_resize_to(fdk_platform_window *pwindow,
                                   fdk_i32 x, fdk_i32 y,
                                   fdk_i32 width, fdk_i32 height) {
    if (pwindow == NULL || width <= 0 || height <= 0) {
        return;
    }
    XMoveResizeWindow(pwindow->conn->display, pwindow->xwindow, x, y,
                      (unsigned int)width, (unsigned int)height);
    XFlush(pwindow->conn->display);
}

/* _NET_WM_MOVERESIZE direction codes, clockwise from top-left; FDK's
 * compass enum shares the clockwise-from-N ordering but starts at N,
 * so this is a rotation, not an identity map. */
static int moveresize_direction(int edge) {
    switch (edge) {
    case 1 /* FDK_WRES_N  */: return 1; /* _NET_WM_MOVERESIZE_SIZE_TOP     */
    case 2 /* FDK_WRES_NE */: return 2; /* _NET_WM_MOVERESIZE_SIZE_TOPRIGHT*/
    case 3 /* FDK_WRES_E  */: return 3; /* _NET_WM_MOVERESIZE_SIZE_RIGHT   */
    case 4 /* FDK_WRES_SE */: return 4; /* _NET_WM_MOVERESIZE_SIZE_BOTTOMRIGHT */
    case 5 /* FDK_WRES_S  */: return 5; /* _NET_WM_MOVERESIZE_SIZE_BOTTOM  */
    case 6 /* FDK_WRES_SW */: return 6; /* _NET_WM_MOVERESIZE_SIZE_BOTTOMLEFT */
    case 7 /* FDK_WRES_W  */: return 7; /* _NET_WM_MOVERESIZE_SIZE_LEFT    */
    case 8 /* FDK_WRES_NW */: return 0; /* _NET_WM_MOVERESIZE_SIZE_TOPLEFT */
    default: return -1;
    }
}

fdk_result fdk_x11_window_begin_move(fdk_platform_window *pwindow,
                                     fdk_i32 local_x, fdk_i32 local_y) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!pwindow->conn->ewmh_wm) {
        return FDK_ERR_UNSUPPORTED; /* caller runs its own drag */
    }
    Window child = 0;
    int rx = 0, ry = 0;
    if (!XTranslateCoordinates(pwindow->conn->display, pwindow->xwindow,
                               pwindow->conn->root, local_x, local_y,
                               &rx, &ry, &child)) {
        return FDK_ERR_PLATFORM_INIT;
    }
    /* data.l = {x_root, y_root, direction(_NET_WM_MOVERESIZE_MOVE=8),
     * button, source indication(1=application)}. The WM takes the
     * pointer grab from here; we see only the resulting configures. */
    send_root_message(pwindow, pwindow->conn->net_wm_moveresize,
                      rx, ry, 8, 1);
    return FDK_OK;
}

fdk_result fdk_x11_window_begin_resize(fdk_platform_window *pwindow,
                                       int edge, fdk_i32 local_x,
                                       fdk_i32 local_y) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int dir = moveresize_direction(edge);
    if (dir < 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!pwindow->conn->ewmh_wm) {
        return FDK_ERR_UNSUPPORTED; /* caller runs its own drag */
    }
    Window child = 0;
    int rx = 0, ry = 0;
    if (!XTranslateCoordinates(pwindow->conn->display, pwindow->xwindow,
                               pwindow->conn->root, local_x, local_y,
                               &rx, &ry, &child)) {
        return FDK_ERR_PLATFORM_INIT;
    }
    send_root_message(pwindow, pwindow->conn->net_wm_moveresize,
                      rx, ry, dir, 1);
    return FDK_OK;
}
