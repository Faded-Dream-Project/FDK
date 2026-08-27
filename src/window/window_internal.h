/*
 * window_internal.h — internal definition of struct fdk_window
 *
 * Bridges the public fdk_window API (fdk_window.h, fdk_event.h) to a
 * backend's fdk_platform_window via the fdk_platform_ops the owning
 * context selected. See docs/architecture.md's layering diagram —
 * this file is the "FDK Window API" layer.
 */

#ifndef FDK_WINDOW_INTERNAL_H
#define FDK_WINDOW_INTERNAL_H

#include "fdk/fdk_core.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"

#include "platform/platform_internal.h"
#include "widget/widget_internal.h"

struct fdk_window {
    fdk_context *ctx;                 /* owning context, not owned by us */
    const fdk_platform_ops *ops;      /* same as ctx's ops, cached for convenience */
    fdk_platform_window *pwindow;     /* backend-owned handle */

    fdk_size last_size;

    fdk_event_callback_fn event_callback;
    void *event_callback_user_data;

    /* Lazily created by fdk_window_get_surface() (src/render/surface.c),
     * destroyed by fdk_window_destroy(). NULL until the application
     * first asks to render into this window. */
    struct fdk_surface *surface;

    /* Lazily created by fdk_window_get_root() (src/widget/widget.c's
     * window glue below), destroyed with the window. NULL until the
     * application builds a widget tree on this window. While set,
     * fdk_window_dispatch_event routes pointer/key events through the
     * tree before the application callback, and fdk_window_paint()
     * repaints+ presents the tree. */
    struct fdk_widget *root;

    /* The window's content widget (fdk_window_set_content, Phase 5
     * layout): weak reference — must be a descendant of root while
     * set. Auto-arranged to the root's full bounds on every
     * configure. Validated (still in the tree?) at each use, so a
     * destroyed content just clears the association. */
    struct fdk_widget *content;
};

/* Called by the context's platform dispatch callback (see
 * src/core/context.c) once it has resolved a raw fdk_platform_window*
 * back to the fdk_window* that owns it. Not part of the public API. */
void fdk_window_dispatch_event(fdk_window *window, const fdk_event_data *event);

#endif /* FDK_WINDOW_INTERNAL_H */
