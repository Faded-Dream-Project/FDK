#define FDK_LOG_TAG "window"

#include "fdk/fdk_window.h"

#include "core/alloc_internal.h"
#include "core/context_windows_internal.h"
#include "core/log_internal.h"
#include "window/window_internal.h"

fdk_result fdk_window_create(fdk_context *ctx,
                              const fdk_window_options *options,
                              fdk_window **out_window) {
    if (ctx == NULL || out_window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    fdk_window *window = fdk_alloc(sizeof(fdk_window));
    if (window == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    window->ctx = ctx;
    window->ops = ctx->ops;
    window->event_callback = NULL;
    window->event_callback_user_data = NULL;

    fdk_result r = ctx->ops->window_create(ctx->conn, options, &window->pwindow);
    if (!fdk_ok(r)) {
        fdk_free(window);
        return r;
    }

    window->last_size.width = (options != NULL && options->width > 0) ? options->width : 640;
    window->last_size.height = (options != NULL && options->height > 0) ? options->height : 480;

    r = fdk_context_register_window(ctx, window);
    if (!fdk_ok(r)) {
        ctx->ops->window_destroy(window->pwindow);
        fdk_free(window);
        return r;
    }

    FDK_DEBUG("window created");

    *out_window = window;
    return FDK_OK;
}

void fdk_window_show(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    window->ops->window_show(window->pwindow);
}

void fdk_window_hide(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    window->ops->window_hide(window->pwindow);
}

void fdk_window_destroy(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    fdk_context_unregister_window(window->ctx, window);
    window->ops->window_destroy(window->pwindow);
    fdk_free(window);
}

void fdk_window_set_title(fdk_window *window, const char *title) {
    if (window == NULL) {
        return;
    }
    window->ops->window_set_title(window->pwindow, title);
}

void fdk_window_resize(fdk_window *window, fdk_i32 width, fdk_i32 height) {
    if (window == NULL || width <= 0 || height <= 0) {
        return;
    }
    window->ops->window_resize(window->pwindow, width, height);
}

fdk_result fdk_window_get_size(const fdk_window *window, fdk_size *out_size) {
    if (window == NULL || out_size == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_size = window->last_size;
    return FDK_OK;
}

void fdk_window_set_size_limits(fdk_window *window, fdk_size min_size, fdk_size max_size) {
    if (window == NULL) {
        return;
    }
    window->ops->window_set_size_limits(window->pwindow, min_size, max_size);
}

void fdk_window_set_event_callback(fdk_window *window,
                                    fdk_event_callback_fn callback,
                                    void *user_data) {
    if (window == NULL) {
        return;
    }
    window->event_callback = callback;
    window->event_callback_user_data = user_data;
}

void fdk_window_dispatch_event(fdk_window *window, const fdk_event_data *event) {
    /* Keep fdk_window_get_size() authoritative without requiring the
     * application to handle FDK_EVENT_WINDOW_CONFIGURE itself just to
     * keep FDK's own bookkeeping in sync. */
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        window->last_size = event->configure.size;
    }

    if (window->event_callback != NULL) {
        window->event_callback(window, event, window->event_callback_user_data);
    }
}
