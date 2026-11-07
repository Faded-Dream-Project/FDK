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

#include <X11/Xatom.h> /* XA_ATOM (the subsurface's XDndAware property) */
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

/* ---- Subsurfaces (1.1.0, D-2 / B-16) -------------------------------------
 *
 * The X11 half of fdk_ext_subsurface_create: a REAL CHILD WINDOW of
 * the parent's X window — the "X11 child-window compositing" the
 * roadmap's B-16 row named. Everything a subsurface means on X11 is
 * a window-tree fact, not a protocol negotiation:
 *
 *   - Clipping: the server clips children to the parent, always —
 *     a child hanging off the edge simply stops at the boundary.
 *   - Stacking: children stack among themselves WITHIN the parent;
 *     they always render over the parent's own pixels (the API's
 *     documented no-under-parent limit — X11 truth, kept uniform).
 *   - Input: pointer events find the child through the normal
 *     window tree (it is topmost where it is stacked); keyboard
 *     follows the focused top-level, which a child never is (the
 *     WM only manages root children — the documented contract).
 *   - Updates: immediate. There is no commit transaction, so the
 *     desync/sync vocabulary of the Wayland side is a no-op here
 *     (set_desync below — a mode hint, never an error).
 *
 * The child is created UNMAPPED with the themed background pixel
 * (the popup reasoning verbatim: a region that maps before its
 * first paint should read as a blank region, not undefined server
 * memory) and NorthWest bit gravity (resize retains pixels — the
 * top-level white-flash fix, same reasoning). FDK's standard event
 * mask is selected at CREATE — before any mapping — so no event
 * between map and select can be missed. WM chrome conveniences
 * (WM_DELETE protocols, titles, _NET_WM_NAME, startup ids) are
 * deliberately absent: no WM ever manages a subwindow; XDND
 * awareness IS set (a drop lands on the topmost window under the
 * pointer — the child must be findable by dragging clients).
 *
 * Coordinates: LOGICAL in the API (the parent's client space, the
 * same space every other FDK window op speaks), PHYSICAL on the
 * wire (x conn->scale) — the HiDPI rule x11_window.c states and
 * x11_events.c reverses for incoming events. */
fdk_result fdk_x11_subsurface_create(fdk_platform_connection *conn,
                                     fdk_platform_window *parent,
                                     fdk_i32 x, fdk_i32 y,
                                     fdk_i32 width, fdk_i32 height,
                                     fdk_platform_window **out_pwindow) {
    if (conn == NULL || parent == NULL || out_pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->display_dead) {
        return FDK_ERR_NO_DISPLAY;
    }

    fdk_i32 scale = conn->scale;
    fdk_i32 pw = width * scale;
    fdk_i32 ph = height * scale;

    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixel = fdk_x11_theme_window_pixel();
    attrs.bit_gravity = NorthWestGravity; /* retain bits top-left */
    attrs.border_pixel = BlackPixel(conn->display, conn->screen);
    attrs.event_mask = StructureNotifyMask | ExposureMask |
                       FocusChangeMask | KeyPressMask |
                       KeyReleaseMask | PointerMotionMask |
                       ButtonPressMask | ButtonReleaseMask |
                       EnterWindowMask | LeaveWindowMask |
                       PropertyChangeMask;
    Window xwindow = XCreateWindow(conn->display, parent->xwindow,
                                   x * scale, y * scale,
                                   (unsigned int)pw, (unsigned int)ph, 0,
                                   CopyFromParent, InputOutput,
                                   CopyFromParent,
                                   CWBackPixel | CWBitGravity |
                                       CWBorderPixel | CWEventMask,
                                   &attrs);
    if (xwindow == 0) {
        FDK_ERROR("subsurface XCreateWindow failed");
        return FDK_ERR_WINDOW_CREATE;
    }

    /* Touch (M-3 discipline): route XI_TouchBegin/Update/End for the
     * child too — a tap in the video region is still a touch. */
    fdk_x11_xi2_select_window(conn, (unsigned long)xwindow);

    /* XDND awareness: a drag source probing the window under the
     * pointer finds the CHILD wherever it is topmost; without the
     * property no dragging client ever begins a drag over it. */
    {
        Atom xdnd_version = 5;
        XChangeProperty(conn->display, xwindow, conn->atom_xdnd_aware,
                        XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)&xdnd_version, 1);
    }

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        XDestroyWindow(conn->display, xwindow);
        XSync(conn->display, False);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    pwindow->conn = conn;
    pwindow->xwindow = xwindow;
    pwindow->ext = 1;        /* routed like every ext surface     */
    pwindow->is_subsurface = 1; /* ...and a child (the two markers   */
                             /* exist for different gates)        */
    pwindow->popup = 0;
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
    pwindow->last_size.width = width; /* LOGICAL — the API's space */
    pwindow->last_size.height = height;
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
    /* XIC lazy (the X1 discipline): pointer events reach the child,
     * keyboard rarely does — the first FocusIn/KeyPress brings the
     * context up if the app ever focuses it explicitly. */
    pwindow->xic = NULL;
    pwindow->startup_id = NULL;
    pwindow->startup_remove_sent = 0;

    fdk_result r = fdk_x11_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        /* Ownership began at the successful XCreateWindow: the child
         * dies with this failure, and XSync pushes the destroy out
         * (the adopt-path pairing against Xvfb). */
        XDestroyWindow(conn->display, xwindow);
        XSync(conn->display, False);
        fdk_free(pwindow);
        return r;
    }

    /* The creation BARRIER (x11_window.c's X1 lesson): the XID is
     * about to become reachable through fdk_ext_surface_native, and
     * any second connection must be able to query the child the
     * moment this call returns. One round-trip per created child. */
    XSync(conn->display, False);

    FDK_DEBUG("subsurface child 0x%lx created under 0x%lx (%dx%d at %d,%d)",
              (unsigned long)xwindow, (unsigned long)parent->xwindow,
              width, height, x, y);

    *out_pwindow = pwindow;
    return FDK_OK;
}

void fdk_x11_subsurface_set_position(fdk_platform_window *pwindow,
                                     fdk_i32 x, fdk_i32 y) {
    if (pwindow == NULL || pwindow->conn == NULL) {
        return;
    }
    /* Server-side and immediate — X11 has no commit transaction; the
     * move is atomic with respect to the event stream. XFlush makes
     * "immediate" true for ANY connection (the API contract): a
     * second client's geometry query the moment this returns sees
     * the new position, not a request still sitting in Xlib's
     * write buffer (the same discipline window_show/hide carry). */
    XMoveWindow(pwindow->conn->display, pwindow->xwindow,
                x * pwindow->conn->scale, y * pwindow->conn->scale);
    XFlush(pwindow->conn->display);
}

void fdk_x11_subsurface_resize(fdk_platform_window *pwindow,
                               fdk_i32 width, fdk_i32 height) {
    if (pwindow == NULL || pwindow->conn == NULL || width <= 0 ||
        height <= 0) {
        return;
    }
    fdk_i32 scale = pwindow->conn->scale;
    XResizeWindow(pwindow->conn->display, pwindow->xwindow,
                  (unsigned int)(width * scale),
                  (unsigned int)(height * scale));
    /* Flush for the same immediate-contract reason as set_position
     * (a second connection's XGetGeometry must see the new size). */
    XFlush(pwindow->conn->display);
    /* The buffer-side size follows the same rule ext_surface_resized
     * records: the next framebuffer acquisition re-sizes the pair.
     * A ConfigureNotify from the server will ALSO arrive (we selected
     * StructureNotify) and dispatch the resize event through the
     * ordinary stream — the surface layer's own dispatch happens only
     * when no server configure will (the Wayland branch). */
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
}

void fdk_x11_subsurface_set_desync(fdk_platform_window *pwindow,
                                   int desync) {
    /* A mode hint, never an error: X11 child windows update on their
     * own always — the set_cursor contract for commit modes. The
     * parameter is deliberately unused. */
    (void)pwindow;
    (void)desync;
}

fdk_result fdk_x11_subsurface_place(fdk_platform_window *pwindow,
                                     fdk_platform_window *sibling,
                                     int above) {
    if (pwindow == NULL || sibling == NULL || pwindow->conn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* XConfigureWindow with an explicit sibling + stack mode is the
     * core-protocol restack: CWStackMode alone would only raise/lower
     * to the ends of the sibling stack. The window layer already
     * validated both are children of the SAME parent (X11's own rule:
     * a sibling must share the parent or the server answers
     * BadMatch). */
    XWindowChanges changes;
    memset(&changes, 0, sizeof(changes));
    changes.sibling = sibling->xwindow;
    changes.stack_mode = above ? Above : Below;
    XConfigureWindow(pwindow->conn->display, pwindow->xwindow,
                     CWSibling | CWStackMode, &changes);
    return FDK_OK;
}
