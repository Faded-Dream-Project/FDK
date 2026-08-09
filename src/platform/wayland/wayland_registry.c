#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

fdk_result fdk_wayland_register_window(fdk_platform_connection *conn,
                                        fdk_platform_window *pwindow) {
    if (conn->window_count == conn->window_capacity) {
        size_t new_capacity = (conn->window_capacity == 0) ? 4 : conn->window_capacity * 2;
        fdk_platform_window **new_array =
            fdk_realloc(conn->windows, new_capacity * sizeof(fdk_platform_window *));
        if (new_array == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        conn->windows = new_array;
        conn->window_capacity = new_capacity;
    }

    conn->windows[conn->window_count++] = pwindow;
    return FDK_OK;
}

void fdk_wayland_unregister_window(fdk_platform_connection *conn,
                                    fdk_platform_window *pwindow) {
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i] == pwindow) {
            conn->windows[i] = conn->windows[conn->window_count - 1];
            conn->window_count--;
            /* If this window currently held pointer/keyboard focus,
             * clear it so a stale pointer is never dereferenced by a
             * later input event delivered before the compositor sends
             * a fresh enter/leave for whatever's focused now. */
            if (conn->pointer_focus == pwindow) conn->pointer_focus = NULL;
            if (conn->keyboard_focus == pwindow) conn->keyboard_focus = NULL;
            return;
        }
    }
    FDK_WARN("unregister_window: window not found in registry (double free?)");
}
