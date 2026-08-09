#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/log_internal.h"

int fdk_x11_dispatch_pending(fdk_platform_connection *conn) {
    int processed = 0;

    while (XPending(conn->display) > 0) {
        XEvent xevent;
        XNextEvent(conn->display, &xevent);

        Window xwindow = xevent.xany.window;
        fdk_platform_window *pwindow = fdk_x11_find_window(conn, xwindow);
        if (pwindow == NULL) {
            /* Event for a window FDK doesn't own/track (root window
             * notifications some WMs send us, or a window we already
             * destroyed but X still had queued events for). Not an
             * error — just nothing to dispatch. */
            continue;
        }

        /* WM_DELETE_WINDOW arrives as a ClientMessage, not a distinct
         * X event type — handled here rather than in
         * fdk_x11_translate_event() since it needs conn->wm_protocols/
         * wm_delete_window to identify, which that function doesn't
         * have access to (by design — it only sees the XEvent). */
        if (xevent.type == ClientMessage &&
            xevent.xclient.message_type == conn->wm_protocols &&
            (Atom)xevent.xclient.data.l[0] == conn->wm_delete_window) {
            fdk_event_data event = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
            conn->dispatch(pwindow, &event, conn->dispatch_user_data);
            processed++;
            continue;
        }

        fdk_event_data event;
        if (fdk_x11_translate_event(pwindow, &xevent, &event)) {
            conn->dispatch(pwindow, &event, conn->dispatch_user_data);
            processed++;
        }
    }

    return processed;
}
