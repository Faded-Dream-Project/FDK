#define FDK_LOG_TAG "widgets"

/*
 * gl_viewport.c — the GL viewport widget (1.0.0, include/fdk/fdk_gl.h)
 *
 * The composition half of application-owned GL: a rectangular region
 * of an ordinary window rendered by the application's OpenGL code,
 * compositing with the widget tree in paint order. The canvas
 * widget's shape (canvas.c) carried to GL:
 *
 *   paint hook  -> engage the window's lane (lazily, first frame),
 *                  make the context current, run the render callback
 *                  (the callback sets its own viewport — FDK hands
 *                  it the logical size and scale), and let the
 *                  tree's present carry the frame. Later siblings
 *                  (menus, popups, tooltips) paint OVER the region
 *                  because the walk visits them later — the z-order
 *                  requirement, for free.
 *
 * FRAME MODEL: application-driven. The viewport renders only on
 * damage (resize, request_frame). No timer of its own — the animated
 * app asks from ITS timer (request_frame coalesces).
 *
 * CONTEXT LOSS: a lane downgrade between frames flips lane_ok off;
 * the context-lost callback fires once and the region falls back to
 * the themed background. The status query reports the reason.
 */

#include "widgets_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_gl.h"
#include "../theme/theme_internal.h"
#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stddef.h>
#include <string.h>

#define GLV_MIN 8

typedef struct fdk_gl_viewport {
    fdk_widget base;
    fdk_gl_render_fn on_render;
    void *on_render_data;
    fdk_gl_context_lost_fn on_lost;
    void *on_lost_data;
    int lane_ok;        /* 0 = lost (or never had it)             */
    int lost_reported;  /* the one-shot context-loss flag          */
    fdk_size last_region;
} fdk_gl_viewport;

static fdk_gl_viewport *glv_of(fdk_widget *w) {
    return (fdk_gl_viewport *)(void *)w;
}

extern const fdk_widget_class fdk_gl_viewport_class_def;

static void glv_paint(fdk_widget *w, fdk_surface *surface,
                      fdk_rect bounds, fdk_rect clip) {
    fdk_gl_viewport *v = glv_of(w);
    (void)clip;
    if (v->on_render == NULL || bounds.width <= 0 || bounds.height <= 0) {
        return; /* blank: nothing to render */
    }
    fdk_window *win = fdk__window_of_owner(fdk__widget_window_owner(w));
    if (win == NULL || win->ops == NULL ||
        win->ops->window_gl_make_current == NULL) {
        return;
    }
    /* Engage lazily: the first frame asks the lane to serve the
     * window (an explicit engage exists for apps that want it
     * earlier — fdk_window_gl_engage). */
    if (v->lane_ok == 0) {
        if (win->ops->window_gl_engage == NULL ||
            !fdk_ok(win->ops->window_gl_engage(win->pwindow, NULL))) {
            /* NOT_YET this frame is fine (a pre-configure Wayland
             * window, a mid-size-change beat); DRIVER/DOWNGRADED are
             * terminal — report once, fall back to the background. */
            int reason = FDK_PLATFORM_GL_REASON_NOT_YET;
            if (win->ops->window_gl_status != NULL) {
                win->ops->window_gl_status(win->pwindow, &reason, NULL);
            }
            if (reason != FDK_PLATFORM_GL_REASON_NOT_YET) {
                if (v->lost_reported == 0) {
                    v->lost_reported = 1;
                    FDK_INFO("gl_viewport: GL unavailable "
                             "(reason %d) — the region falls back to "
                             "the themed background", reason);
                    if (v->on_lost != NULL) {
                        v->on_lost(w, v->on_lost_data);
                    }
                }
                return;
            }
            return; /* try again next frame */
        }
        v->lane_ok = 1;
        v->lost_reported = 0;
    }

    fdk_f32 scale = 1.0f;
    (void)fdk_window_get_scale(win, &scale);
    const int pw = (int)bounds.width;
    const int ph = (int)bounds.height;

    if (!fdk_ok(win->ops->window_gl_make_current(win->pwindow))) {
        v->lane_ok = 0;
        return;
    }
    /* The callback owns the GL: it sets its viewport, draws with its
     * own shaders (depth test and all — the flavor fdk_gl.h reports
     * governs what it may use), and returns. FDK never draws GL on
     * the application's behalf; the region's presence in the frame
     * rides the window's present (the callback drew INTO the lane's
     * drawable, the same channel the ext frame-callback owns). */
    v->on_render(w, pw, ph, scale, v->on_render_data);

    /* The region's background box in the CPU surface: keeps the
     * damage bookkeeping and the tree's composite coherent while the
     * GL content rides the lane's present. */
    fdk_surface_fill_rect(surface, bounds,
                          (fdk_color){ 0.06f, 0.07f, 0.09f, 1.0f });
    v->last_region.width = bounds.width;
    v->last_region.height = bounds.height;
}

static void glv_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = GLV_MIN;
    out->height = GLV_MIN;
}

static const fdk_a11y_class glv_a11y = {
    .role = FDK_A11Y_ROLE_CANVAS,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_gl_viewport_class_def = {
    .size = sizeof(fdk_gl_viewport),
    .name = "gl_viewport",
    .handle_event = NULL,
    .paint = glv_paint,
    .measure = glv_measure,
    .arrange = NULL,
    .instance_finalize = NULL,
    .a11y = &glv_a11y,
};

fdk_result fdk_gl_viewport_create(fdk_widget *parent,
                                  fdk_gl_render_fn on_render,
                                  void *user_data,
                                  fdk_widget **out_viewport) {
    if (out_viewport == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_viewport = NULL;
    fdk_rect bounds = { 0, 0, GLV_MIN, GLV_MIN };
    fdk_result r = fdk_widget_create(parent, &fdk_gl_viewport_class_def,
                                     bounds, out_viewport);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_gl_viewport *v = glv_of(*out_viewport);
    v->on_render = on_render;
    v->on_render_data = user_data;
    v->on_lost = NULL;
    v->on_lost_data = NULL;
    v->lane_ok = 0;
    v->lost_reported = 0;
    v->last_region.width = 0;
    v->last_region.height = 0;
    return FDK_OK;
}

void fdk_gl_viewport_set_render_callback(fdk_widget *viewport,
                                         fdk_gl_render_fn on_render,
                                         void *user_data) {
    if (viewport == NULL || viewport->klass != &fdk_gl_viewport_class_def) {
        return;
    }
    fdk_gl_viewport *v = glv_of(viewport);
    v->on_render = on_render;
    v->on_render_data = user_data;
    fdk_widget_invalidate(viewport);
}

void fdk_gl_viewport_set_context_lost_callback(
    fdk_widget *viewport, fdk_gl_context_lost_fn on_lost, void *user_data) {
    if (viewport == NULL || viewport->klass != &fdk_gl_viewport_class_def) {
        return;
    }
    fdk_gl_viewport *v = glv_of(viewport);
    v->on_lost = on_lost;
    v->on_lost_data = user_data;
}

void fdk_gl_viewport_request_frame(fdk_widget *viewport) {
    if (viewport == NULL || viewport->klass != &fdk_gl_viewport_class_def) {
        return;
    }
    fdk_widget_invalidate(viewport);
}

fdk_result fdk_gl_viewport_status(fdk_widget *viewport,
                                  fdk_gl_status_data *out_status) {
    if (out_status == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->reason = FDK_GL_REASON_BACKEND;
    if (viewport == NULL || viewport->klass != &fdk_gl_viewport_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_window *win =
        fdk__window_of_owner(fdk__widget_window_owner(viewport));
    return fdk_gl_status(win, out_status);
}
