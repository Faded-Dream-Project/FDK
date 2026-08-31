#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "theme/theme_internal.h"

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h> /* XC_* cursor glyph indices (1.1.4) */
#include <string.h>

/* The creation-time window fill (1.2.1): the theme's window-
 * background token instead of WhitePixel, so the frame a compositor
 * shows before the app's first paint matches the FIRST PAINTED frame
 * (the root default background, fdk_window_get_root) instead of
 * flashing white→dark. Same channel packing the renderer assumes
 * everywhere (R<<16 | G<<8 | B — the visual FDK requires). */
static unsigned long x11_theme_window_pixel(void) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    unsigned long r = (unsigned long)(c.r * 255.0f + 0.5f);
    unsigned long g = (unsigned long)(c.g * 255.0f + 0.5f);
    unsigned long b = (unsigned long)(c.b * 255.0f + 0.5f);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
}

#define X11_DEFAULT_WIDTH  640
#define X11_DEFAULT_HEIGHT 480
#define X11_DEFAULT_TITLE  "FDK Application"

fdk_result fdk_x11_window_create(fdk_platform_connection *conn,
                                     const fdk_window_options *options,
                                     fdk_platform_window *parent,
                                     fdk_platform_window **out_pwindow) {
    fdk_i32 width = X11_DEFAULT_WIDTH;
    fdk_i32 height = X11_DEFAULT_HEIGHT;
    const char *title = X11_DEFAULT_TITLE;
    int popup = 0;
    fdk_i32 pop_x = 0, pop_y = 0;

    if (options != NULL) {
        if (options->width > 0)  width = options->width;
        if (options->height > 0) height = options->height;
        if (options->title != NULL) title = options->title;
        popup = options->popup;
        pop_x = options->x;
        pop_y = options->y;
    }

    unsigned long black = BlackPixel(conn->display, conn->screen);
    unsigned long bg_pixel = x11_theme_window_pixel();

    /* Popups (Phase 9): override-redirect children positioned at the
     * parent's client-area origin + (x, y) in ROOT coordinates —
     * override-redirect windows place directly, no WM frame math.
     * Top-levels stay WM-managed at (0,0) as before. */
    fdk_i32 win_x = 0, win_y = 0;
    if (popup != 0) {
        if (parent != NULL) {
            Window child = None;
            if (!XTranslateCoordinates(conn->display, parent->xwindow,
                                       conn->root, 0, 0, &win_x, &win_y,
                                       &child)) {
                win_x = 0;
                win_y = 0;
            }
        }
        win_x += pop_x;
        win_y += pop_y;
    }

    Window xwindow;
    if (popup != 0) {
        XSetWindowAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        attrs.override_redirect = True;
        attrs.border_pixel = black;
        attrs.background_pixel = bg_pixel;
        attrs.event_mask = StructureNotifyMask | ExposureMask |
                           FocusChangeMask | KeyPressMask |
                           KeyReleaseMask | PointerMotionMask |
                           ButtonPressMask | ButtonReleaseMask |
                           EnterWindowMask | LeaveWindowMask |
                           PropertyChangeMask;
        xwindow = XCreateWindow(conn->display, conn->root,
                                win_x, win_y, (unsigned int)width,
                                (unsigned int)height, 0,
                                CopyFromParent, InputOutput,
                                CopyFromParent,
                                CWOverrideRedirect | CWBorderPixel |
                                    CWBackPixel | CWEventMask,
                                &attrs);
    } else {
        /* Top-level (WM-managed): white background PIXEL + NorthWest
         * bit gravity at creation; the background becomes None at the
         * first framebuffer acquisition (x11_surface.c). Together that
         * is the fix for the white-flash-on-resize found on real
         * compositing desktops (Cinnamon), without breaking the
         * documented never-painted-window contract:
         *
         * A window the app never renders into (01_hello_world, the
         * "no renderer yet" example) shows its background pixel —
         * the themed window fill on both backends, same as Wayland's
         * committed solid-color buffer (attach_background_buffer).
         * The moment
         * an app acquires the framebuffer it owns every pixel, and
         * the background flips to None: the server never CLEARS
         * again. A background clear on each resize step is precisely
         * the flash — the client only repaints after the configure
         * travels back through its event loop, so an interactive
         * resize showed a full frame of background per step (fast
         * white flashing over dark content).
         *
         * NorthWest bit gravity makes every resize RETAIN the
         * existing pixels anchored top-left instead of discarding
         * them (the default ForgetGravity treats the whole window as
         * newly exposed): retained content plus the synchronous
         * configure-time repaint (window.c's dispatch tail) means the
         * window goes straight from old-size content to new-size
         * content with nothing visible in between.
         *
         * Popups keep a background pixel always: they never resize,
         * so they have no resize-flash to fix, and a menu that maps
         * before its first paint should read as a blank menu, not as
         * undefined server memory. */
        XSetWindowAttributes attrs;
        memset(&attrs, 0, sizeof(attrs));
        attrs.background_pixel = bg_pixel; /* until the app paints */
        attrs.bit_gravity = NorthWestGravity; /* retain bits top-left */
        attrs.border_pixel = black;
        xwindow = XCreateWindow(conn->display, conn->root,
                                win_x, win_y, (unsigned int)width,
                                (unsigned int)height, 0,
                                CopyFromParent, InputOutput,
                                CopyFromParent,
                                CWBackPixel | CWBitGravity |
                                    CWBorderPixel,
                                &attrs);
    }

    if (xwindow == 0) {
        FDK_ERROR("XCreateWindow failed");
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

    /* XDND awareness (1.2.0): without the XdndAware property no
     * dragging client ever begins a drag over this window. */
    {
        Atom xdnd_version = 5;
        XChangeProperty(conn->display, xwindow, conn->atom_xdnd_aware,
                        XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)&xdnd_version, 1);
    }

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
    pwindow->popup = (popup != 0);
    pwindow->grabbed = 0;
    pwindow->drop_formats = 0; /* fdk_window_set_drop_formats fills it */
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->presented_ever = 0;
    pwindow->background_dropped = 0;
    pwindow->has_saved = 0;
    pwindow->saved_x = 0;
    pwindow->saved_y = 0;
    pwindow->saved_w = 0;
    pwindow->saved_h = 0;
    /* WM_CLASS from the connection's app_id (fdk_init_options):
     * res_class = the id itself, res_name = the id's last dot
     * segment — the ICCCM convention (class groups instances). */
    if (conn->app_id != NULL) {
        XClassHint class_hint = { 0 };
        class_hint.res_class = conn->app_id;
        /* strrchr on a char* returns char* in C — no const to shed
         * (XClassHint predates const, but the strings are mutable
         * connection-owned storage). res_name = the id's last dot
         * segment per the ICCCM instance/class convention. */
        char *last_dot = strrchr(conn->app_id, '.');
        class_hint.res_name =
            last_dot != NULL ? last_dot + 1 : conn->app_id;
        XSetClassHint(conn->display, pwindow->xwindow, &class_hint);
    }

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
    if (pwindow->popup) {
        fdk_x11_window_popup_ungrab(pwindow);
    }
    if (pwindow->modal_grab) {
        /* An active modal grab would outlive the dying grab window —
         * release it explicitly (dialog destruction ends modality). */
        XUngrabKeyboard(pwindow->conn->display, CurrentTime);
        XUngrabPointer(pwindow->conn->display, CurrentTime);
        pwindow->modal_grab = 0;
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

void fdk_x11_window_popup_grab(fdk_platform_window *pwindow) {
    if (pwindow == NULL || !pwindow->popup || pwindow->grabbed) {
        return;
    }
    Display *dpy = pwindow->conn->display;
    /* The popup's grab: pointer events ANYWHERE on the screen route
     * to our connection (owner_events False reports them against
     * the grab window); clicks outside the popup's bounds become a
     * dismissal in x11_events.c. The keyboard grab routes keys to
     * the popup regardless of the WM's focus (menus need arrows). */
    int pr = XGrabPointer(dpy, pwindow->xwindow, False,
                          ButtonPressMask | ButtonReleaseMask |
                              PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None,
                          CurrentTime);
    int kr = XGrabKeyboard(dpy, pwindow->xwindow, False,
                           GrabModeAsync, GrabModeAsync, CurrentTime);
    if (pr != GrabSuccess || kr != GrabSuccess) {
        FDK_WARN("popup grab: pointer=%d keyboard=%d (popup works, "
                 "but outside clicks/keys may escape)",
                 pr == GrabSuccess ? 1 : 0, kr == GrabSuccess ? 1 : 0);
    }
    pwindow->grabbed = 1;
}

void fdk_x11_window_popup_ungrab(fdk_platform_window *pwindow) {
    if (pwindow == NULL || !pwindow->grabbed) {
        return;
    }
    Display *dpy = pwindow->conn->display;
    /* Release both grabs unconditionally — a successful grab of
     * either kind must be undone even if the other failed. */
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);
    pwindow->grabbed = 0;
}

void fdk_x11_window_popup_regrab(fdk_platform_window *pwindow) {
    if (pwindow == NULL || !pwindow->popup || !pwindow->grabbed) {
        return;
    }
    /* Same grabs as popup_grab, re-issued on THIS window: a nested
     * popup's grab replaced ours, and closing the child did not give
     * it back (grabs do not stack) — the menu machinery calls this
     * when a submenu closes but the parent menu stays open. */
    Display *dpy = pwindow->conn->display;
    int pr = XGrabPointer(dpy, pwindow->xwindow, False,
                          ButtonPressMask | ButtonReleaseMask |
                              PointerMotionMask,
                          GrabModeAsync, GrabModeAsync, None, None,
                          CurrentTime);
    int kr = XGrabKeyboard(dpy, pwindow->xwindow, False,
                           GrabModeAsync, GrabModeAsync, CurrentTime);
    if (pr != GrabSuccess || kr != GrabSuccess) {
        FDK_WARN("popup regrab: pointer=%d keyboard=%d",
                 pr == GrabSuccess ? 1 : 0, kr == GrabSuccess ? 1 : 0);
    }
}

fdk_result fdk_x11_window_set_modal(fdk_platform_window *pwindow,
                                    bool modal) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    Display *dpy = pwindow->conn->display;
    if (modal && !pwindow->modal_grab) {
        /* The modal grab: identical X calls to the popup grab, but on
         * a NORMAL toplevel and without any dismissal semantics —
         * out-of-bounds presses on a non-popup window are ordinary
         * events that hit-test nothing and are ignored, which is
         * exactly "input waits for the dialog". The keyboard grab
         * also bypasses whatever focus the WM had, so the dialog's
         * focused widget keeps keys even before the WM re-focuses. */
        int pr = XGrabPointer(dpy, pwindow->xwindow, False,
                              ButtonPressMask | ButtonReleaseMask |
                                  PointerMotionMask,
                              GrabModeAsync, GrabModeAsync, None, None,
                              CurrentTime);
        int kr = XGrabKeyboard(dpy, pwindow->xwindow, False,
                               GrabModeAsync, GrabModeAsync,
                               CurrentTime);
        if (pr != GrabSuccess || kr != GrabSuccess) {
            FDK_WARN("modal grab: pointer=%d keyboard=%d (dialog "
                     "stays up, but other windows may take input)",
                     pr == GrabSuccess ? 1 : 0,
                     kr == GrabSuccess ? 1 : 0);
            return FDK_ERR_PLATFORM;
        }
        pwindow->modal_grab = 1;
    } else if (!modal && pwindow->modal_grab) {
        XUngrabKeyboard(dpy, CurrentTime);
        XUngrabPointer(dpy, CurrentTime);
        XFlush(dpy);
        pwindow->modal_grab = 0;
    }
    return FDK_OK;
}

void fdk_x11_window_show(fdk_platform_window *pwindow) {
    if (pwindow->popup) {
        /* Map FIRST, then grab: grabbing before the window exists on
         * screen fails outright under some servers. */
        XMapWindow(pwindow->conn->display, pwindow->xwindow);
        XFlush(pwindow->conn->display);
        fdk_x11_window_popup_grab(pwindow);
        return;
    }
    XMapWindow(pwindow->conn->display, pwindow->xwindow);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_hide(fdk_platform_window *pwindow) {
    if (pwindow->popup) {
        fdk_x11_window_popup_ungrab(pwindow);
    }
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
    if (pwindow->conn->ewmh_wm) {
        /* An EWMH WM is running: ALWAYS the client-message request,
         * never the direct geometry stomp below. Two reasons:
         *
         * 1. It's the sanctioned path — the WM owns maximization,
         *    rewrites _NET_WM_STATE, and the PropertyNotify keeps
         *    FDK's flag honest (a WM that refuses changes nothing,
         *    and fdk_window_is_maximized() keeps telling the truth).
         *
         * 2. The direct XMoveResizeWindow fallback was written for
         *    BARE X and LIES under a real WM: it optimistically
         *    dispatches the state flip as if FDK's own action were
         *    the outcome, but a WM may clamp or reinterpret the
         *    request (Metacity-family WMs treat a request for exactly
         *    the monitor size as maximization, then REFUSE the
         *    restore request while they consider the window
         *    maximized). Result: the window stays maximized while
         *    FDK's flag — and the title-bar button glyph driven by
         *    it — flip back (the 1.1.3 user report).
         *
         * A WM that doesn't advertise the two maximized atoms in
         * _NET_SUPPORTED simply ignores the message; the honest
         * outcome is "still not maximized", which the property
         * read agrees with. */
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
    /* Release the implicit pointer grab created by the button press
     * BEFORE handing the drag to the WM. The press that triggered
     * this call left the server's pointer actively grabbed by THIS
     * window (the X protocol's implicit grab: it ends at button
     * release). A WM that drives the interactive move with its own
     * XGrabPointer (openbox, Metacity-family, most others) gets
     * AlreadyGrabbed while our implicit grab is held — its move op
     * never sees a single motion event, and the window does not
     * move even though the message was sent and accepted (verified
     * empirically under openbox: with the ungrab the window follows
     * the drag exactly; without it, nothing moves). Releasing our
     * grab costs nothing — we were about to hand the drag over and
     * stop wanting pointer events for it anyway. */
    XUngrabPointer(pwindow->conn->display, CurrentTime);
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
    /* Same implicit-grab release as begin_move: the WM's interactive
     * resize needs to take the pointer grab our press is holding. */
    XUngrabPointer(pwindow->conn->display, CurrentTime);
    send_root_message(pwindow, pwindow->conn->net_wm_moveresize,
                      rx, ry, dir, 1);
    return FDK_OK;
}

/* ---- Pointer introspection + cursor shaping (1.1.4) ---- */

int fdk_x11_window_query_pointer(fdk_platform_window *pwindow,
                                 fdk_i32 *out_x, fdk_i32 *out_y) {
    if (pwindow == NULL || out_x == NULL || out_y == NULL) {
        return 0;
    }
    Window root_ret = None, child_ret = None;
    int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
    unsigned int mask = 0;
    /* XQueryPointer against OUR window returns the pointer position
     * already translated to window-local coordinates (win_x/win_y),
     * including positions OUTSIDE the window while crossing to it —
     * the bounds check below is what makes the "inside" answer
     * honest. Returns False when the pointer is on a different
     * screen, which is simply "not over this window".
     *
     * UNIFIED CONTRACT (1.2.5, same words as the Wayland side):
     * nonzero only when the pointer is within the window's CURRENT
     * geometry — last_size is the most recent ConfigureNotify size,
     * in the same window-local space win_x/win_y come in. The
     * Wayland query op runs the identical check against its last
     * acked size, so window_revalidate_pointer's motion-vs-leave
     * decision behaves the same on both backends. */
    if (!XQueryPointer(pwindow->conn->display, pwindow->xwindow,
                       &root_ret, &child_ret, &root_x, &root_y,
                       &win_x, &win_y, &mask)) {
        return 0;
    }
    *out_x = win_x;
    *out_y = win_y;
    return win_x >= 0 && win_y >= 0 &&
           win_x < (int)pwindow->last_size.width &&
           win_y < (int)pwindow->last_size.height;
}

/* Glyph indices in the server's built-in cursor font for each
 * fdk_window_resize_edge compass value (index 0 is unused — 0 means
 * "default arrow"). Corners get corner cursors, edges get arrows. */
static const unsigned int resize_cursor_glyphs[9] = {
    0,                          /* FDK_WRES_NONE — default */
    XC_top_side,                /* N  */
    XC_top_right_corner,        /* NE */
    XC_right_side,              /* E  */
    XC_bottom_right_corner,     /* SE */
    XC_bottom_side,             /* S  */
    XC_bottom_left_corner,      /* SW */
    XC_left_side,               /* W  */
    XC_top_left_corner,         /* NW */
};

void fdk_x11_window_set_cursor(fdk_platform_window *pwindow, int edge) {
    if (pwindow == NULL || edge < 0 || edge > 8) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;
    Cursor cursor = None;
    if (edge != 0) {
        if (conn->resize_cursors[edge] == None) {
            /* XCreateFontCursor reads the "cursor" font every X
             * server ships in core — no libXcursor, no theme lookup.
             * Failure leaves None, and defining cursor None below
             * then simply keeps the default arrow: honest
             * degradation, not a lying shape. */
            conn->resize_cursors[edge] = XCreateFontCursor(
                conn->display, resize_cursor_glyphs[edge]);
        }
        cursor = conn->resize_cursors[edge];
    }
    /* Cursor None (edge 0 or creation failure) reverts the window to
     * its parent's cursor — the standard arrow on any desktop. */
    XDefineCursor(conn->display, pwindow->xwindow, cursor);
}

void fdk_x11_cursor_shutdown(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    for (int i = 0; i < 9; i++) {
        if (conn->resize_cursors[i] != None) {
            XFreeCursor(conn->display, conn->resize_cursors[i]);
            conn->resize_cursors[i] = None;
        }
    }
}
