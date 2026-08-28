#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

fdk_result fdk_x11_connect(fdk_platform_dispatch_fn dispatch,
                               void *dispatch_user_data, const char *app_id,
                               fdk_platform_connection **out_conn) {
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        FDK_INFO("XOpenDisplay failed (no X11 display reachable)");
        return FDK_ERR_NO_DISPLAY;
    }

    fdk_platform_connection *conn = fdk_alloc(sizeof(fdk_platform_connection));
    if (conn == NULL) {
        XCloseDisplay(display);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    conn->display = display;
    conn->screen = DefaultScreen(display);
    conn->root = RootWindow(display, conn->screen);
    conn->dispatch = dispatch;
    conn->dispatch_user_data = dispatch_user_data;
    conn->windows = NULL;
    conn->window_count = 0;
    conn->window_capacity = 0;
    conn->app_id = NULL;

    /* app_id (when set) rides along on the connection and becomes
     * every window's WM_CLASS — the X11 identity mechanism window
     * managers match for rules and grouping. */
    if (app_id != NULL && app_id[0] != '\0') {
        size_t len = strlen(app_id) + 1;
        conn->app_id = malloc(len); /* Xlib-free storage; freed with
                                     * free() at disconnect */
        if (conn->app_id != NULL) {
            memcpy(conn->app_id, app_id, len);
        }
    }

    /* MIT-SHM probe (Phase 3 completion): use the shared-memory fast
     * path for present() when the server supports the extension and
     * the environment has not opted out (FDK_NO_MIT_SHM=1 — escape
     * hatch for servers that implement it brokenly and for
     * debugging/measuring the copy path). Probing once here keeps
     * per-window acquisition cheap and makes the capability a
     * property of the CONNECTION, which is what the protocol says it
     * is. */
    conn->shm_ok = 0;
    conn->shm_event_base = 0;
    if (getenv("FDK_NO_MIT_SHM") == NULL) {
        int major = 0, minor = 0, pixmaps = 0;
        if (XShmQueryVersion(display, &major, &minor,
                             (Bool *)&pixmaps) == True) {
            conn->shm_ok = 1;
            conn->shm_event_base = XShmGetEventBase(display);
            FDK_DEBUG("MIT-SHM available (v%d.%d) — presentation uses "
                      "the shared-memory path", major, minor);
        }
    } else {
        FDK_DEBUG("FDK_NO_MIT_SHM set — presentation uses the copy path");
    }

    /* WM_DELETE_WINDOW: without registering for this via WM_PROTOCOLS,
     * the window manager would just kill our connection when the user
     * clicks the close button, giving the application no chance to
     * respond (see fdk_event.h, FDK_EVENT_WINDOW_CLOSE_REQUEST). This
     * is the standard ICCCM mechanism, not a desktop-specific hack. */
    conn->wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    conn->wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    conn->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    conn->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    conn->motif_wm_hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    conn->net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    conn->net_wm_state_maximized_vert =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    /* NOTE the spelling: the EWMH spec atom is ..._HORZ (not
     * ..._HORIZ). A misspelled name here does NOT fail loudly —
     * XInternAtom with only-if-exists=False CREATES a fresh atom
     * nobody else uses, so the _NET_SUPPORTED probe below never
     * matches it, every WM looks "maximize-incapable", and the
     * maximize path silently degrades to the bare-X fallback under
     * real window managers (exactly the 1.1.3 bug: window maximized
     * by the WM, FDK's flag disagreeing). tests/test_x11_integration.c
     * pins the spec spelling against this. */
    conn->net_wm_state_maximized_horiz =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    conn->net_wm_moveresize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    conn->wm_state = XInternAtom(display, "WM_STATE", False);
    conn->wm_change_state = XInternAtom(display, "WM_CHANGE_STATE", False);

    /* Probe the WM's EWMH capabilities ONCE, from the root's
     * _NET_SUPPORTED atom list — the EWMH-sanctioned capability
     * discovery. No list (or an empty one) means no EWMH WM is
     * running (bare X, Xvfb, or a pre-EWMH WM), which is exactly what
     * the bare-X fallback paths in x11_window.c key off. */
    {
        Atom net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(display, conn->root, net_supported,
                               0, 1024, False, XA_ATOM, &type, &format,
                               &nitems, &bytes_after, &prop) == Success &&
            type == XA_ATOM && format == 32 && nitems > 0) {
            conn->ewmh_wm = 1;
            Atom *atoms = (Atom *)prop;
            int have_vert = 0, have_horiz = 0;
            for (unsigned long i = 0; i < nitems; i++) {
                if (atoms[i] == conn->net_wm_state_maximized_vert) {
                    have_vert = 1;
                }
                if (atoms[i] == conn->net_wm_state_maximized_horiz) {
                    have_horiz = 1;
                }
            }
            conn->ewmh_state_ok = (have_vert && have_horiz) ? 1 : 0;
        }
        if (prop != NULL) {
            XFree(prop);
        }
    }
    FDK_INFO("EWMH window manager %s (maximize support: %s)",
             conn->ewmh_wm ? "detected" : "not detected",
             conn->ewmh_state_ok ? "yes" : "no");

    /* Clipboard helper + atoms (Phase 9). Failure is not fatal — the
     * clipboard ops simply report FDK_ERR_PLATFORM / NULL. */
    conn->clip_helper = None;
    conn->clip_owned_text = NULL;
    if (fdk_ok(fdk_x11_clipboard_init(conn))) {
        FDK_DEBUG("clipboard helper window ready (Phase 9)");
    }

    FDK_INFO("connected (screen %d, %dx%d root)", conn->screen,
             DisplayWidth(display, conn->screen),
             DisplayHeight(display, conn->screen));

    *out_conn = conn;
    return FDK_OK;
}

void fdk_x11_disconnect(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }

    if (conn->window_count > 0) {
        FDK_WARN("disconnecting with %zu window(s) still open — "
                 "force-destroying (caller should have destroyed them "
                 "explicitly; see fdk_shutdown() in fdk_core.h)",
                 conn->window_count);
        /* fdk_x11_window_destroy() mutates conn->windows via
         * fdk_x11_unregister_window(), so iterate defensively from
         * the end rather than assuming indices stay stable. */
        while (conn->window_count > 0) {
            fdk_x11_window_destroy(conn->windows[conn->window_count - 1]);
        }
    }
    fdk_free(conn->windows);

    fdk_x11_clipboard_shutdown(conn);

    free(conn->app_id);
    conn->app_id = NULL;

    FDK_INFO("disconnecting");
    XCloseDisplay(conn->display);
    fdk_free(conn);
}

int fdk_x11_get_event_fd(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return -1;
    }
    return ConnectionNumber(conn->display);
}
