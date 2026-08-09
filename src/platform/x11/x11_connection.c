#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h>
#include <unistd.h>

fdk_result fdk_x11_connect(fdk_platform_dispatch_fn dispatch,
                               void *dispatch_user_data,
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

    /* WM_DELETE_WINDOW: without registering for this via WM_PROTOCOLS,
     * the window manager would just kill our connection when the user
     * clicks the close button, giving the application no chance to
     * respond (see fdk_event.h, FDK_EVENT_WINDOW_CLOSE_REQUEST). This
     * is the standard ICCCM mechanism, not a desktop-specific hack. */
    conn->wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    conn->wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    conn->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    conn->utf8_string = XInternAtom(display, "UTF8_STRING", False);

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
