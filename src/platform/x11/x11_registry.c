#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

fdk_result fdk_x11_register_window(fdk_platform_connection *conn,
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

void fdk_x11_unregister_window(fdk_platform_connection *conn,
                                fdk_platform_window *pwindow) {
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i] == pwindow) {
            /* Swap-remove: order doesn't matter for this registry. */
            conn->windows[i] = conn->windows[conn->window_count - 1];
            conn->window_count--;
            return;
        }
    }
    FDK_WARN("unregister_window: window not found in registry (double free?)");
}

fdk_platform_window *fdk_x11_find_window(fdk_platform_connection *conn,
                                          Window xwindow) {
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->xwindow == xwindow) {
            return conn->windows[i];
        }
    }
    return NULL;
}
