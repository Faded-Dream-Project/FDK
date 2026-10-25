/*
 * x11_ext.c — X11 extension-surface adoption (1.1.0)
 *
 * The X11 half of include/fdk/fdk_extend.h's ext_surface_adopt
 * platform op: an add-on built its own X window through the Display*
 * the extension API handed out (dock type, struts, override-redirect
 * — whatever its surface kind means), and FDK now wraps that window
 * so it participates in everything a normal FDK window gets: event
 * translation (x11_dispatch resolves the XID through the registry
 * this adopt joins), the XImage/XShm framebuffer pair, presents with
 * damage, popup parenting (the X tree parents children naturally),
 * and pointer/keyboard focus translation.
 *
 * Ownership (fdk_extend.h's lifetime block): from this call on, the
 * X window belongs to FDK — window_destroy XDestroyWindow's it. The
 * add-on's ROLE (its properties, its protocol meaning) is torn down
 * by the kind's destroy callback BEFORE that, against a live window.
 */

#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

fdk_result fdk_x11_ext_surface_adopt(fdk_platform_connection *conn,
                                     const fdk_ext_native_handle *native,
                                     fdk_platform_window **out_pwindow) {
    if (conn == NULL || native == NULL || out_pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (native->backend != FDK_EXT_BACKEND_X11 || native->xid == 0) {
        FDK_WARN("ext_surface_adopt: not an X11 window handle");
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->display_dead) {
        return FDK_ERR_NO_DISPLAY;
    }

    Window xwindow = (Window)native->xid;

    /* The window must exist and belong to this connection's screen.
     * XGetGeometry is the honest probe (a BadWindow would kill the
     * Display's default error path): it also hands back the window's
     * real geometry, which seeds last_size so the first framebuffer
     * acquisition is right-sized without waiting for a ConfigureNotify
     * that an unmapped window has not sent yet. */
    Window root = None;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;
    if (!XGetGeometry(conn->display, xwindow, &root, &x, &y, &width,
                      &height, &border, &depth)) {
        /* A handle that is no window on THIS display: nothing to own,
        * and destroying it would raise BadWindow through Xlib's
        * default error path (process exit) — refuse honestly and
        * touch nothing. (Ownership transfer begins below, once the
        * window proves real.) */
        FDK_WARN("ext_surface_adopt: XGetGeometry failed — foreign or "
                 "already-destroyed window 0x%lx",
                 (unsigned long)xwindow);
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Ownership transfers HERE (fdk_extend.h's lifetime block): from
     * this point every failure path destroys the X window before
     * returning, so the caller can treat any adopt failure as "the
     * native surface is gone" — after it called the kind's destroy
     * callback for the role teardown. */

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        XDestroyWindow(conn->display, xwindow);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    pwindow->conn = conn;
    pwindow->xwindow = xwindow;
    pwindow->ext = 1;
    pwindow->popup = 0; /* popups arrive through the normal
                         * window_create path, parenting to this
                         * wrapper's X window like any X child */
    pwindow->input_transparent = 0;
    pwindow->grabbed = 0;
    pwindow->modal_grab = 0;
    pwindow->drop_formats = 0;
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->fullscreen = 0;
    pwindow->has_saved = 0;
    pwindow->saved_x = 0;
    pwindow->saved_y = 0;
    pwindow->saved_w = 0;
    pwindow->saved_h = 0;
    pwindow->last_size.width = (fdk_i32)width;
    pwindow->last_size.height = (fdk_i32)height;
    pwindow->presented_ever = 0;
    pwindow->background_dropped = 0;
    pwindow->gl_win = NULL;
    pwindow->gl_downgraded = 0;
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

    /* FDK's standard input set — exactly what window_create selects
     * (the comment there applies verbatim: these are the events
     * fdk_event.h can translate). The add-on selected nothing when
     * it built the window; FDK owns event selection for every window
     * it routes, adopted or not. */
    XSelectInput(conn->display, xwindow,
                 StructureNotifyMask | ExposureMask | FocusChangeMask |
                     KeyPressMask | KeyReleaseMask | PointerMotionMask |
                     ButtonPressMask | ButtonReleaseMask | EnterWindowMask |
                     LeaveWindowMask | PropertyChangeMask);

    /* One XIC per window when the connection has an XIM (the same
     * rootless style window_create installs) — Xutf8LookupString in
     * the key translator then resolves the user's real layout. */
    pwindow->xic = NULL;
    if (conn->xim != NULL) {
        pwindow->xic = XCreateIC(conn->xim,
                                 XNInputStyle, XIMPreeditNothing |
                                                   XIMStatusNothing,
                                 XNClientWindow, xwindow,
                                 XNFocusWindow, xwindow,
                                 NULL);
        if (pwindow->xic == NULL) {
            FDK_WARN("XCreateIC failed — extension surface falls back "
                     "to ASCII text entry");
        }
    }

    fdk_result r = fdk_x11_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        if (pwindow->xic != NULL) {
            XDestroyIC(pwindow->xic);
        }
        /* Ownership was transferred above: the window dies with this
         * failure (XSync so the request is truly out — the same
         * destroy/XSync pairing window_destroy uses against Xvfb). */
        XDestroyWindow(conn->display, xwindow);
        XSync(conn->display, False);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("extension surface adopted (0x%lx, %ux%u)",
              (unsigned long)xwindow, width, height);

    *out_pwindow = pwindow;
    return FDK_OK;
}

/* The documented native-display accessor behind
 * fdk_ext_get_native_display() (1.1.0): the connection's Display*,
 * borrowed for the connection's lifetime. */
void *fdk_x11_native_display(fdk_platform_connection *conn) {
    return (conn != NULL) ? (void *)conn->display : NULL;
}

/* fdk_ext_surface_notify_configure()'s op: the buffer-side size
 * follows the reported one (the framebuffer pair re-creates at this
 * size on the next acquisition — the X WINDOW itself is untouched:
 * on X11 the add-on sizes its own window, and the server's own
 * ConfigureNotify path updates this same field through
 * x11_events.c). */
void fdk_x11_ext_surface_resized(fdk_platform_window *pwindow,
                                 fdk_i32 width, fdk_i32 height) {
    if (pwindow == NULL || width < 1 || height < 1) {
        return;
    }
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
}

/* fdk_ext_surface_native()'s op: the wrapped window's XID. */
fdk_result fdk_x11_ext_surface_native(fdk_platform_window *pwindow,
                                      fdk_ext_native_handle *out) {
    if (pwindow == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->backend = FDK_EXT_BACKEND_X11;
    out->xid = (fdk_u64)pwindow->xwindow;
    return FDK_OK;
}
