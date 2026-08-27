#define FDK_LOG_TAG "window"

#include "fdk/fdk_window.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "widget/widget_internal.h"
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
    window->surface = NULL;
    window->root = NULL;
    window->content = NULL;

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
    if (window->root != NULL) {
        /* The window owns its root; drop the ownership marker so the
         * widget layer lets us destroy it, then tear the tree down
         * (subclass destroy hooks run, deferred destroys settle). */
        window->root->flags &= ~FDK_WF_WINDOW_ROOT;
        fdk_widget_destroy(window->root);
        window->root = NULL;
    }
    fdk_surface_detach_from_window(window);
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
        if (window->root != NULL) {
            /* The root tracks the window's client size; a resize is a
             * full repaint on both backends (fresh framebuffer). */
            fdk_widget_root_resized(window->root, event->configure.size);
        }
        if (window->content != NULL) {
            /* Phase 5: the content widget reflows with the window. */
            fdk_window_layout(window);
        }
    } else if (event->type == FDK_EVENT_WINDOW_EXPOSE) {
        if (window->root != NULL) {
            fdk_widget_invalidate_all(window->root);
        }
    }

    /* Widget trees get first claim on input events (pointer, keys,
     * window focus). Events a widget handles are consumed here and
     * never reach the application's window callback — that is the
     * documented contract of fdk_window_get_root() (see
     * include/fdk/fdk_widget.h). Window-level events (configure,
     * expose, close-request) are never consumed by widgets.
     *
     * A widget handler is allowed to destroy the window (the classic
     * quit button) — which frees this very fdk_window. Cache what the
     * post-routing code needs and re-verify registration before
     * touching `window` again. */
    fdk_context *ctx = window->ctx;
    fdk_platform_window *pwindow = window->pwindow;
    bool handled_by_tree = false;
    if (window->root != NULL) {
        handled_by_tree = fdk_widget_tree_handle_event(window->root, event);
    }

    if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
        return; /* destroyed by a widget handler: nothing left to do */
    }

    if (!handled_by_tree && window->event_callback != NULL) {
        window->event_callback(window, event, window->event_callback_user_data);
    }
}

fdk_result fdk_window_get_root(fdk_window *window, fdk_widget **out_root) {
    if (window == NULL || out_root == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->root == NULL) {
        fdk_rect bounds = {0, 0, window->last_size.width,
                           window->last_size.height};
        fdk_result r = fdk_widget_create(NULL, NULL, bounds, &window->root);
        if (!fdk_ok(r)) {
            return r;
        }
        window->root->flags |= FDK_WF_WINDOW_ROOT;
        FDK_DEBUG("window root widget created (%dx%d)", bounds.width,
                  bounds.height);
    }
    *out_root = window->root;
    return FDK_OK;
}

fdk_result fdk_window_paint(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->root == NULL) {
        return FDK_OK; /* no tree: the app drives the surface itself */
    }
    fdk_surface *surface = NULL;
    fdk_result r = fdk_window_get_surface(window, &surface);
    if (!fdk_ok(r)) {
        return r;
    }
    /* A paint hook may destroy the window (freeing the surface with
     * it); cache the context/pwindow and re-verify before presenting. */
    fdk_context *ctx = window->ctx;
    fdk_platform_window *pwindow = window->pwindow;
    fdk_widget_tree_paint(window->root, surface);
    if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
        return FDK_OK; /* window destroyed mid-paint; the tree went
                        * with it, nothing to present */
    }
    return fdk_surface_present(surface);
}

/* Is `widget` still a live descendant of `root`? (The content
 * pointer is weak: a destroyed content must silently deactivate.) */
static bool widget_in_tree(fdk_widget *root, fdk_widget *widget) {
    for (fdk_widget *cur = widget; cur != NULL; cur = cur->parent) {
        if (cur == root) {
            return true;
        }
    }
    return false;
}

void fdk_window_set_content(fdk_window *window, fdk_widget *content) {
    if (window == NULL) {
        return;
    }
    if (content != NULL) {
        fdk_widget *root = NULL;
        if (!fdk_ok(fdk_window_get_root(window, &root)) ||
            !widget_in_tree(root, content)) {
            FDK_WARN("set_content: widget is not in the window's tree");
            return;
        }
    }
    window->content = content;
    if (content != NULL) {
        fdk_window_layout(window);
    }
}

void fdk_window_layout(fdk_window *window) {
    if (window == NULL || window->content == NULL ||
        window->root == NULL) {
        return;
    }
    if (!widget_in_tree(window->root, window->content)) {
        /* The content widget was destroyed or reparented away —
         * deactivate the association rather than arrange a stray. */
        window->content = NULL;
        return;
    }
    fdk_rect full = fdk_widget_get_bounds(window->root);
    fdk_widget_arrange(window->content, full);
}
