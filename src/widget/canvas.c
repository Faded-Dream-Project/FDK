#define FDK_LOG_TAG "widgets"

/*
 * canvas.c — Canvas widget (Phase 9)
 *
 * An application-drawable surface: the paint hook forwards the
 * surface, the widget's absolute bounds, and the effective clip to
 * the application's callback at paint time. Everything the paint
 * machinery already guarantees — clipping to the widget's bounds,
 * damage-driven repainting, idempotent paints — applies to the
 * callback's drawing too (it draws through the same surface
 * primitives, inside the same pushed clip).
 *
 * The callback runs DURING the paint walk: it must not destroy
 * widgets, mutate the tree, or call fdk_window_paint() re-entrantly
 * (the deferred-destroy guard tolerates accidents, but the contract
 * is "draw only", like every other paint hook). fdk_canvas_invalidate
 * is the correct way to request a repaint.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stddef.h>

#define CANVAS_MIN 8

typedef struct fdk_canvas {
    fdk_widget base;
    fdk_canvas_paint_fn on_paint;
    void *on_paint_data;
} fdk_canvas;

static fdk_canvas *canvas_of(fdk_widget *w) {
    return (fdk_canvas *)(void *)w;
}

extern const fdk_widget_class fdk_canvas_class_def;

static void canvas_paint(fdk_widget *w, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip) {
    fdk_canvas *c = canvas_of(w);
    if (c->on_paint != NULL) {
        c->on_paint(w, surface, bounds, clip, c->on_paint_data);
    }
}

static void canvas_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = CANVAS_MIN;
    out->height = CANVAS_MIN;
}

const fdk_widget_class fdk_canvas_class_def = {
    .size = sizeof(fdk_canvas),
    .name = "canvas",
    .handle_event = NULL, /* events bubble to ancestors by default */
    .paint = canvas_paint,
    .measure = canvas_measure,
    .arrange = NULL,
    .destroy = NULL,
};

fdk_result fdk_canvas_create(fdk_widget *parent,
                             fdk_canvas_paint_fn on_paint,
                             void *user_data, fdk_widget **out_canvas) {
    if (out_canvas == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_canvas_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_canvas *c = canvas_of(w);
    c->on_paint = on_paint;
    c->on_paint_data = user_data;
    fdk_widget_child_layout_changed(w->parent);
    *out_canvas = w;
    return FDK_OK;
}

void fdk_canvas_set_paint_callback(fdk_widget *canvas,
                                   fdk_canvas_paint_fn on_paint,
                                   void *user_data) {
    if (canvas == NULL || canvas->klass != &fdk_canvas_class_def) {
        return;
    }
    fdk_canvas *c = canvas_of(canvas);
    c->on_paint = on_paint;
    c->on_paint_data = user_data;
    fdk_widget_invalidate(canvas);
}

void fdk_canvas_invalidate(fdk_widget *canvas) {
    if (canvas == NULL || canvas->klass != &fdk_canvas_class_def) {
        return;
    }
    fdk_widget_invalidate(canvas);
}
