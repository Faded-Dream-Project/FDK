#define FDK_LOG_TAG "window"

#include "fdk/fdk_window.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_widgets.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "theme/theme_internal.h"
#include "widget/widget_internal.h"
#include "window/window_internal.h"

#include <string.h>

/* ---- Phase 8: FDK-drawn decorations ----
 *
 * The title band is a widget with a private paint class (themed fill
 * + themed border rule, resolved at paint time so theme switches
 * repaint it for free), carrying a catalog Label (default color =
 * themed text) and a catalog Button wired to the close-request path.
 * Dragging the band moves the window through the backend's optional
 * move ops. */

#define DECO_TITLE_H 28

static void deco_bar_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)w;
    (void)clip;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_surface_fill_rect(
        surface, bounds,
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND));
    fdk_i32 t = fdk_theme_get_metric(NULL, FDK_TM_SEPARATOR_THICKNESS);
    if (t > bounds.height) {
        t = bounds.height;
    }
    fdk_surface_fill_rect(
        surface, (fdk_rect){bounds.x, bounds.y + bounds.height - t,
                            bounds.width, t},
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER));
}

static const fdk_widget_class deco_bar_class = {
    .size = sizeof(fdk_widget),
    .name = "fdk-deco-bar",
    .handle_event = NULL,
    .paint = deco_bar_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
};

/* Bar-local drag. Snap formulation: on each motion the window is
 * moved so the press anchor sits under the pointer again (origin +=
 * local_now - anchor). Because the move is flushed before the next
 * motion event is generated, each event's bar-local coordinates are
 * relative to the frame the previous move produced — the drag
 * converges instead of drifting. */
static bool deco_bar_event(fdk_widget *w, const fdk_widget_event *ev,
                           void *user) {
    fdk_window *window = user;
    if (window == NULL || w != window->deco_bar) {
        return false;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if (ev->pointer.button != 1 ||
            window->ops->window_get_position == NULL ||
            window->ops->window_move_to == NULL) {
            return false; /* non-primary button, or backend can't move */
        }
        if (!fdk_ok(window->ops->window_get_position(
                window->pwindow, &window->drag_origin_x,
                &window->drag_origin_y))) {
            return false;
        }
        window->drag_anchor = ev->pointer.position;
        window->dragging = true;
        return true;
    case FDK_WIDGET_POINTER_MOTION:
        if (!window->dragging) {
            return false;
        }
        window->ops->window_move_to(
            window->pwindow,
            window->drag_origin_x +
                (fdk_i32)(ev->position.x - window->drag_anchor.x),
            window->drag_origin_y +
                (fdk_i32)(ev->position.y - window->drag_anchor.y));
        /* The move above becomes the frame the NEXT motion event is
         * measured against; keep the origin current, anchor stays. */
        window->drag_origin_x += (fdk_i32)(ev->position.x -
                                           window->drag_anchor.x);
        window->drag_origin_y += (fdk_i32)(ev->position.y -
                                           window->drag_anchor.y);
        return true;
    case FDK_WIDGET_POINTER_UP:
        window->dragging = false;
        return true;
    default:
        return false;
    }
}

/* The close button delivers the SAME event the WM's delete would, so
 * application close semantics are identical either way. Nested into
 * fdk_window_dispatch_event — the same reentrancy protections as the
 * real path apply (a callback may destroy the window). */
static void deco_close_activate(fdk_widget *w, void *user) {
    (void)w;
    fdk_window *window = user;
    if (window == NULL) {
        return;
    }
    fdk_event_data ev;
    memset(&ev, 0, sizeof ev);
    ev.type = FDK_EVENT_WINDOW_CLOSE_REQUEST;
    fdk_window_dispatch_event(window, &ev);
}

/* Sizes the band and its children for the current window width; the
 * content widget is re-laid-out below the band by the caller. */
static void window_arrange_deco(fdk_window *window) {
    if (!window->decorated || window->root == NULL ||
        window->deco_bar == NULL) {
        return;
    }
    fdk_i32 w = window->last_size.width;
    fdk_widget_set_bounds(window->deco_bar,
                          (fdk_rect){0, 0, w, DECO_TITLE_H});
    fdk_i32 th = 0;
    if (window->deco_font != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(window->deco_font, &fm);
        th = fm.ascent + fm.descent;
    }
    fdk_i32 ly = (DECO_TITLE_H - th) / 2;
    if (ly < 0) {
        ly = 0;
    }
    if (window->deco_title != NULL) {
        fdk_widget_set_bounds(
            window->deco_title,
            (fdk_rect){10, ly, w > 50 ? w - 50 : 0, th > 0 ? th : 1});
    }
    if (window->deco_close != NULL) {
        fdk_i32 bh = DECO_TITLE_H - 8;
        fdk_widget_set_bounds(window->deco_close,
                              (fdk_rect){w - 28, 4, 22, bh});
    }
}

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
    window->decorated = false;
    window->deco_bar = NULL;
    window->deco_title = NULL;
    window->deco_close = NULL;
    window->deco_font = NULL;
    window->deco_font_owned = NULL;
    window->title = NULL;
    window->dragging = false;
    window->drag_anchor = (fdk_pointf){0.0f, 0.0f};
    window->drag_origin_x = 0;
    window->drag_origin_y = 0;

    if (options != NULL && options->title != NULL) {
        size_t n = strlen(options->title) + 1;
        window->title = fdk_alloc(n);
        if (window->title != NULL) {
            memcpy(window->title, options->title, n);
        }
    }

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
         * (subclass destroy hooks run, deferred destroys settle).
         * The decoration band is a subtree of the root and dies with
         * it. */
        window->root->flags &= ~FDK_WF_WINDOW_ROOT;
        fdk_widget_destroy(window->root);
        window->root = NULL;
    }
    window->deco_bar = NULL;
    window->deco_title = NULL;
    window->deco_close = NULL;
    if (window->deco_font_owned != NULL) {
        fdk_font_destroy(window->deco_font_owned);
        window->deco_font_owned = NULL;
    }
    window->deco_font = NULL;
    fdk_free(window->title);
    window->title = NULL;
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

    /* Keep our own copy so a later set_decorated(true) can label the
     * band without asking the backend to read it back. */
    fdk_free(window->title);
    window->title = NULL;
    if (title != NULL) {
        size_t n = strlen(title) + 1;
        window->title = fdk_alloc(n);
        if (window->title != NULL) {
            memcpy(window->title, title, n);
        }
    }
    if (window->decorated && window->deco_title != NULL) {
        (void)fdk_label_set_text(window->deco_title, window->title);
    }
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
        if (window->decorated) {
            /* The decoration band spans the new width. */
            window_arrange_deco(window);
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

/* ---- FDK-drawn decorations (public API) ---- */

static void deco_load_font(fdk_window *window) {
    if (window->deco_font != NULL) {
        return; /* app font borrowed, or already loaded */
    }
    window->deco_font_owned = fdk_font_load_system_default(14);
    window->deco_font = window->deco_font_owned;
    /* NULL (no system font) is legal: the band renders without title
     * text; the loader already warned once. */
}

fdk_result fdk_window_set_decorated(fdk_window *window, bool decorated) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (decorated == window->decorated) {
        return FDK_OK; /* idempotent */
    }

    if (decorated) {
        if (window->ops->window_set_wm_decorations == NULL) {
            FDK_WARN("set_decorated: this backend cannot drop its own "
                     "decorations; refusing to stack FDK's on top");
            return FDK_ERR_UNSUPPORTED;
        }
        fdk_result r = window->ops->window_set_wm_decorations(
            window->pwindow, false);
        if (!fdk_ok(r)) {
            return r;
        }

        fdk_widget *root = NULL;
        r = fdk_window_get_root(window, &root);
        if (!fdk_ok(r)) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }

        deco_load_font(window);

        r = fdk_widget_create(root, &deco_bar_class,
                              (fdk_rect){0, 0, window->last_size.width,
                                         DECO_TITLE_H},
                              &window->deco_bar);
        if (!fdk_ok(r)) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }
        r = fdk_label_create(window->deco_bar, window->deco_font,
                             window->title, &window->deco_title);
        if (fdk_ok(r)) {
            r = fdk_button_create(window->deco_bar, window->deco_font,
                                  "\xC3\x97", &window->deco_close);
        }
        if (!fdk_ok(r)) {
            /* Partial band is worse than none: roll back cleanly. */
            fdk_widget_destroy(window->deco_bar);
            window->deco_bar = NULL;
            window->deco_title = NULL;
            window->deco_close = NULL;
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }
        fdk_widget_set_event_callback(window->deco_bar, deco_bar_event,
                                      window);
        fdk_button_set_on_activate(window->deco_close,
                                   deco_close_activate, window);
        window->decorated = true;
        window_arrange_deco(window);
        fdk_window_layout(window);
        fdk_widget_invalidate_all(window->root);
        FDK_DEBUG("FDK decorations enabled (WM chrome dropped)");
    } else {
        window->decorated = false;
        window->dragging = false;
        if (window->deco_bar != NULL) {
            fdk_widget_destroy(window->deco_bar);
            window->deco_bar = NULL;
            window->deco_title = NULL;
            window->deco_close = NULL;
        }
        if (window->ops->window_set_wm_decorations != NULL) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
        }
        if (window->root != NULL) {
            fdk_window_layout(window);
            fdk_widget_invalidate_all(window->root);
        }
        FDK_DEBUG("FDK decorations disabled (WM chrome restored)");
    }
    return FDK_OK;
}

bool fdk_window_get_decorated(const fdk_window *window) {
    return window != NULL && window->decorated;
}

fdk_result fdk_window_set_decoration_font(fdk_window *window,
                                          fdk_font *font) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Swap the effective font; the owned system default (if any) is
     * kept for a later NULL revert — it is one small face. */
    window->deco_font = font;
    if (window->decorated && window->deco_title != NULL) {
        /* Re-create the label's text layout under the new face by
         * re-setting the text (a no-op change still re-measures) and
         * re-arranging the band geometry. */
        (void)fdk_label_set_text(window->deco_title, window->title);
        window_arrange_deco(window);
    }
    return FDK_OK;
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
    if (window->decorated && window->deco_bar != NULL) {
        full.y = DECO_TITLE_H;
        full.height -= DECO_TITLE_H;
        if (full.height < 0) {
            full.height = 0;
        }
    }
    fdk_widget_arrange(window->content, full);
}
