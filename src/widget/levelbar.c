/*
 * levelbar.c — the segmented value readout (1.4.2).
 *
 * GTK's GtkLevelBar / the classic battery-and-volume meter: a value
 * in [min, max] displayed as a row of discrete blocks (DISCRETE mode,
 * the default) or as one continuous bar (CONTINUOUS mode). It is a
 * READOUT, not a control — no focus, no events, no drag: the value
 * arrives by fdk_levelbar_set_value from the application's own state
 * (the battery poller, the volume observer, the progress of a
 * transfer with no known total).
 *
 * DISCRETE mode: `segments` blocks (default 5), 10 px wide with 6-px
 * gaps, 6 px tall, rounded 3. Blocks strictly below the value's
 * fraction of the range fill with the accent; the rest stay on the
 * track color. At value == max every block is lit (the boundary rule:
 * a full bar must read as full, so the fill count is clamped from
 * BOTH sides).
 *
 * CONTINUOUS mode: the progress bar's geometry without its label
 * traffic — a rounded 6-px track, an accent run covering the value's
 * fraction.
 *
 * The a11y face is the LEVEL_BAR role with the value interface
 * (min/max/current) and a "%g" value text — a meter, announced as
 * one.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"

#define LEVELBAR_BLOCK_W 10
#define LEVELBAR_BLOCK_GAP 6
#define LEVELBAR_BLOCK_H 6
#define LEVELBAR_MIN_W 40
#define LEVELBAR_MIN_H 8

static fdk_f32 levelbar_frac(const fdk_levelbar *lb) {
    if (lb->max <= lb->min) {
        return (lb->value >= lb->max) ? 1.0f : 0.0f;
    }
    fdk_f32 f = (fdk_f32)((lb->value - lb->min) / (lb->max - lb->min));
    if (f < 0.0f) {
        f = 0.0f;
    } else if (f > 1.0f) {
        f = 1.0f;
    }
    return f;
}

static void levelbar_measure(fdk_widget *w, fdk_size *out) {
    fdk_levelbar *lb = levelbar_of(w);
    if (lb->mode == FDK_LEVELBAR_CONTINUOUS) {
        out->width = LEVELBAR_MIN_W;
        out->height = LEVELBAR_MIN_H;
        return;
    }
    size_t n = (lb->segments > 0) ? lb->segments : 1;
    out->width = (fdk_i32)(n * LEVELBAR_BLOCK_W +
                           (n - 1) * LEVELBAR_BLOCK_GAP);
    if (out->width < LEVELBAR_MIN_W) {
        out->width = LEVELBAR_MIN_W;
    }
    out->height = LEVELBAR_MIN_H;
}

static void levelbar_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_levelbar *lb = levelbar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;
    fdk_color on = disabled ? fdk__pal_control_disabled() : fdk__pal_accent();
    fdk_color off = fdk__pal_track();
    fdk_f32 frac = levelbar_frac(lb);

    if (lb->mode == FDK_LEVELBAR_CONTINUOUS) {
        /* One continuous bar: track + the accent run. */
        fdk_i32 h = (bounds.height < LEVELBAR_BLOCK_H) ? bounds.height
                                                       : LEVELBAR_BLOCK_H;
        fdk_rect track = {bounds.x,
                          bounds.y + (bounds.height - h) / 2,
                          bounds.width, h};
        if (track.width > 0 && track.height > 0) {
            fdk_surface_fill_rounded_rect(surface, track, h / 2, off);
            fdk_rect run = track;
            run.width = (fdk_i32)((fdk_f32)track.width * frac + 0.5f);
            if (run.width > 0) {
                fdk_surface_fill_rounded_rect(surface, run, h / 2, on);
            }
        }
        return;
    }

    /* DISCRETE: n blocks centered in the bounds, lit below frac. */
    size_t n = (lb->segments > 0) ? lb->segments : 1;
    fdk_i32 total = (fdk_i32)(n * LEVELBAR_BLOCK_W +
                              (n - 1) * LEVELBAR_BLOCK_GAP);
    fdk_i32 x = bounds.x + (bounds.width - total) / 2;
    if (x < bounds.x) {
        x = bounds.x;
    }
    fdk_i32 y = bounds.y + (bounds.height - LEVELBAR_BLOCK_H) / 2;
    if (y < bounds.y) {
        y = bounds.y;
    }
    /* The boundary rule: floor-of-fraction in the middle, exact at
     * both ends (a full bar reads full; an empty one reads empty). */
    size_t lit = (size_t)((fdk_f32)n * frac + 1e-4f);
    if (frac <= 0.0f) {
        lit = 0;
    } else if (frac >= 1.0f) {
        lit = n;
    }
    for (size_t i = 0; i < n; i++) {
        fdk_rect block = {x + (fdk_i32)(i * (LEVELBAR_BLOCK_W +
                                             LEVELBAR_BLOCK_GAP)),
                          y, LEVELBAR_BLOCK_W, LEVELBAR_BLOCK_H};
        if (block.x + block.width > bounds.x + bounds.width) {
            break; /* squeezed bounds: draw what honestly fits */
        }
        fdk_surface_fill_rounded_rect(surface, block,
                                      LEVELBAR_BLOCK_H / 2,
                                      (i < lit) ? on : off);
    }
}

/* ---- a11y ---- */

static void levelbar_a11y_describe(const fdk_widget *w,
                                   fdk_a11y_info *out) {
    const fdk_levelbar *lb = (const fdk_levelbar *)(const void *)w;
    out->has_value = true;
    out->value_min = lb->min;
    out->value_max = lb->max;
    out->value_current = lb->value;
    out->value_text = fdk__a11y_valuef("%g", lb->value);
}

static const fdk_a11y_class levelbar_a11y = {
    .role = FDK_A11Y_ROLE_LEVEL_BAR,
    .describe = levelbar_a11y_describe,
    .actions = NULL, /* a readout, not a control */
    .perform = NULL,
};

const fdk_widget_class fdk_levelbar_class_def = {
    .size = sizeof(fdk_levelbar),
    .name = "levelbar",
    .handle_event = NULL,
    .paint = levelbar_paint,
    .measure = levelbar_measure,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &levelbar_a11y,
    .max_children = 0,
};

fdk_result fdk_levelbar_create(fdk_widget *parent, double min,
                               double max, fdk_widget **out_levelbar) {
    if (out_levelbar == NULL || max < min) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_levelbar_class_def,
                                     (fdk_rect){0, 0, LEVELBAR_MIN_W,
                                                LEVELBAR_MIN_H},
                                     &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_levelbar *lb = levelbar_of(w);
    lb->min = min;
    lb->max = max;
    lb->value = min;
    lb->mode = FDK_LEVELBAR_DISCRETE;
    lb->segments = 5;
    *out_levelbar = w;
    return FDK_OK;
}

void fdk_levelbar_set_value(fdk_widget *levelbar, double value) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return;
    }
    fdk_levelbar *lb = levelbar_of(levelbar);
    if (value < lb->min) {
        value = lb->min;
    } else if (value > lb->max) {
        value = lb->max;
    }
    if (value == lb->value) {
        return;
    }
    lb->value = value;
    fdk_widget_invalidate(levelbar);
    fdk__a11y_notify(levelbar, FDK_A11Y_VALUE_CHANGED, 0);
}

double fdk_levelbar_get_value(fdk_widget *levelbar) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return 0.0;
    }
    return levelbar_of(levelbar)->value;
}

void fdk_levelbar_set_range(fdk_widget *levelbar, double min,
                            double max) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return;
    }
    if (max < min) {
        return; /* refuse quietly: create() documented this input */
    }
    fdk_levelbar *lb = levelbar_of(levelbar);
    lb->min = min;
    lb->max = max;
    fdk_levelbar_set_value(levelbar, lb->value); /* re-clamps */
    fdk_widget_invalidate(levelbar);
}

void fdk_levelbar_set_mode(fdk_widget *levelbar,
                           fdk_levelbar_mode mode) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return;
    }
    if (mode != FDK_LEVELBAR_DISCRETE &&
        mode != FDK_LEVELBAR_CONTINUOUS) {
        return;
    }
    fdk_levelbar *lb = levelbar_of(levelbar);
    if (lb->mode == mode) {
        return;
    }
    lb->mode = mode;
    /* The natural width differs between modes: relayout. */
    fdk_widget_invalidate(levelbar);
    fdk_widget_child_layout_changed(levelbar->parent);
}

fdk_levelbar_mode fdk_levelbar_get_mode(fdk_widget *levelbar) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return FDK_LEVELBAR_DISCRETE;
    }
    return levelbar_of(levelbar)->mode;
}

void fdk_levelbar_set_segments(fdk_widget *levelbar, size_t segments) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return;
    }
    if (segments < 2 || segments > 32) {
        return; /* one block is a progress bar; 32 is a wall of dots */
    }
    fdk_levelbar *lb = levelbar_of(levelbar);
    if (lb->segments == segments) {
        return;
    }
    lb->segments = segments;
    fdk_widget_invalidate(levelbar);
    fdk_widget_child_layout_changed(levelbar->parent);
}

size_t fdk_levelbar_get_segments(fdk_widget *levelbar) {
    if (levelbar == NULL || levelbar->klass != &fdk_levelbar_class_def) {
        return 0;
    }
    return levelbar_of(levelbar)->segments;
}
