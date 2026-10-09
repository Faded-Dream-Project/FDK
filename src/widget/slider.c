#define FDK_LOG_TAG "widgets"

/*
 * slider.c — Slider widget (Phase 9)
 *
 * A draggable value picker over [min, max] with a themed track and a
 * grabbable thumb. Pointer: press anywhere jumps the thumb there
 * (quantized to the value step) and the implicit grab drags it.
 * Keyboard (focused): arrows step, PageUp/PageDown step by 10% of
 * the range, Home/End go to the ends. The value-changed callback
 * fires on every settled value change (drag, click, keyboard,
 * programmatic set).
 *
 * Horizontal only in v1 (the roadmap says "Slider"; vertical is a
 * documented parked remainder).
 *
 * 1.4.2 — marks: gtk_scale_add_mark's ticks (and optional labels)
 * under the trough. Labels render in the lazy system default font
 * (the tooltip's precedent: loaded on first use, owned by the
 * slider, destroyed with it); a system without a loadable font
 * degrades labels to bare ticks — honest, not a blank. Marks widen
 * the natural height by one label line; the track centers in the
 * remaining extent so a marked slider keeps its thumb geometry.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stddef.h>
#include <math.h>

#define SLIDER_TRACK_H 6
#define SLIDER_THUMB_W 14
#define SLIDER_THUMB_H 20
#define SLIDER_MIN_W 40
#define SLIDER_MIN_H 24
#define SLIDER_KEY_PCT 10
#define SLIDER_TICK_H 6    /* 1.4.2: the tick stroke below the trough */

/* A mark: tick + optional label at a value (GTK's scale marks). */
typedef struct fdk_slider_mark {
    double value;
    char *label;    /* owned; NULL = tick only */
} fdk_slider_mark;

typedef struct fdk_slider {
    fdk_widget base;
    double min;
    double max;
    double value;
    double step;      /* quantization; 0 = continuous */
    bool dragging;
    fdk_slider_changed_fn on_changed;
    void *on_changed_data;
    fdk_slider_mark *marks;   /* 1.4.2, owned array */
    size_t mark_count;
    size_t mark_cap;
    fdk_font *label_font;     /* 1.4.2: lazy system default          */
    bool label_font_failed;   /* one honest attempt, then ticks only */
} fdk_slider;

static fdk_slider *slider_of(fdk_widget *w) {
    return (fdk_slider *)(void *)w;
}

extern const fdk_widget_class fdk_slider_class_def;

static double slider_clamp(fdk_slider *s, double v) {
    if (v < s->min) {
        v = s->min;
    }
    if (v > s->max) {
        v = s->max;
    }
    if (s->step > 0.0) {
        double q = (v - s->min) / s->step;
        q = (q >= 0.0) ? (double)(long long)(q + 0.5)
                       : -(double)(long long)(-q + 0.5);
        v = s->min + q * s->step;
        if (v < s->min) {
            v = s->min;
        }
        if (v > s->max) {
            v = s->max;
        }
    }
    return v;
}

static double slider_span(const fdk_slider *s) {
    return s->max - s->min;
}

/* 1.4.2: whether any mark carries a label (drives the height and
 * the track zone). */
static bool slider_has_labeled_mark(const fdk_slider *s) {
    for (size_t i = 0; i < s->mark_count; i++) {
        if (s->marks[i].label != NULL) {
            return true;
        }
    }
    return false;
}

/* The label font, loaded lazily on first use (the tooltip's
 * precedent). NULL when the system has no loadable font — labels
 * then degrade to ticks. */
static fdk_font *slider_label_font(fdk_slider *s) {
    if (s->label_font != NULL) {
        return s->label_font;
    }
    if (s->label_font_failed) {
        return NULL;
    }
    s->label_font = fdk_font_load_system_default(12);
    if (s->label_font == NULL) {
        s->label_font_failed = true; /* one honest attempt */
    }
    return s->label_font;
}

/* Thumb x (widget-local). */
static fdk_i32 slider_thumb_x(const fdk_slider *s) {
    fdk_i32 w = s->base.bounds.width;
    fdk_i32 inner = w - SLIDER_THUMB_W;
    if (inner <= 0) {
        return 0;
    }
    double frac = (slider_span(s) > 0.0)
        ? (s->value - s->min) / slider_span(s)
        : 0.0;
    return (fdk_i32)(frac * (double)inner + 0.5);
}

static void slider_set_value(fdk_slider *s, double v, bool fire) {
    double nv = slider_clamp(s, v);
    if (nv != s->value || (v != s->value && nv == s->value)) {
        s->value = nv;
        fdk_widget_invalidate(&s->base);
        /* A11y: the value interface moved (before the user callback,
         * which may destroy the widget). */
        fdk__a11y_notify(&s->base, FDK_A11Y_VALUE_CHANGED, 0);
        if (fire && s->on_changed != NULL) {
            s->on_changed(&s->base, s->on_changed_data);
        }
    } else {
        s->value = nv;
    }
}

static void slider_paint(fdk_widget *w, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_slider *s = slider_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* 1.4.2: with labeled marks the track zone is the bounds minus
     * the label line (+2 breathing room); the thumb/track geometry
     * is UNCHANGED (it centers in the zone). */
    fdk_i32 zone_h = bounds.height;
    fdk_font *lfont = NULL;
    if (s->mark_count > 0 && slider_has_labeled_mark(s)) {
        lfont = slider_label_font(s);
        if (lfont != NULL) {
            fdk_i32 lw = 0, lh = 0;
            fdk__text_extent(lfont, "Ag", &lw, &lh);
            zone_h = bounds.height - lh - 2;
            if (zone_h < SLIDER_MIN_H / 2) {
                zone_h = bounds.height / 2; /* squeezed: keep the
                                              * track honest */
            }
        }
    }
    fdk_i32 cy = bounds.y + zone_h / 2;
    /* Track. */
    fdk_rect track = { bounds.x + SLIDER_THUMB_W / 2,
                       cy - SLIDER_TRACK_H / 2,
                       bounds.width - SLIDER_THUMB_W, SLIDER_TRACK_H };
    if (track.width > 0) {
        fdk_surface_fill_rounded_rect(surface, track, SLIDER_TRACK_H / 2,
                                      fdk__pal_track());
    }
    /* Filled run from the minimum to the thumb. */
    fdk_i32 tx = slider_thumb_x(s);
    fdk_rect fill = { track.x, track.y,
                      (tx + SLIDER_THUMB_W / 2) - track.x, track.height };
    if (fill.width > 0) {
        fdk_surface_fill_rounded_rect(surface, fill, SLIDER_TRACK_H / 2,
                                      fdk__pal_accent());
    }
    /* 1.4.2 — the marks: ticks at each mark's thumb-center position
     * (a 1-px stroke below the trough), labels centered under their
     * ticks in the lazy system font. Labels clip at the widget's
     * bounds (no wrap, no ellipsis — the marks must not change the
     * slider's natural WIDTH). */
    if (s->mark_count > 0 && track.width > 0) {
        fdk_color tick_col = ((w->flags & FDK_WF_ENABLED) == 0)
            ? fdk__pal_control_disabled()
            : fdk__pal_border();
        fdk_i32 label_baseline = 0;
        if (lfont != NULL) {
            label_baseline = bounds.y + bounds.height - 2;
        }
        for (size_t i = 0; i < s->mark_count; i++) {
            double frac = (slider_span(s) > 0.0)
                ? (s->marks[i].value - s->min) / slider_span(s)
                : 0.0;
            if (frac < 0.0) {
                frac = 0.0;
            } else if (frac > 1.0) {
                frac = 1.0;
            }
            fdk_i32 mx = track.x +
                (fdk_i32)(frac * (double)track.width + 0.5);
            fdk_surface_draw_line(surface, mx, track.y + track.height + 1,
                                  mx, track.y + track.height +
                                  SLIDER_TICK_H, tick_col);
            if (lfont != NULL && s->marks[i].label != NULL) {
                fdk_i32 lw = 0, lh = 0;
                fdk__text_extent(lfont, s->marks[i].label, &lw, &lh);
                fdk_i32 lx = mx - lw / 2;
                if (lx < bounds.x) {
                    lx = bounds.x; /* left-edge label clamp */
                }
                fdk__draw_text(surface, lfont, s->marks[i].label,
                               tick_col, lx, label_baseline);
            }
        }
    }
    /* Thumb. */
    fdk_color thumb_col = ((w->flags & FDK_WF_ENABLED) == 0)
        ? fdk__pal_control_disabled()
        : (s->dragging ? fdk__pal_control_pressed()
                       : ((w->flags & FDK_WF_HOVERED) != 0
                              ? fdk__pal_control_hover()
                              : fdk__pal_control()));
    fdk_rect thumb = { bounds.x + tx, cy - SLIDER_THUMB_H / 2,
                       SLIDER_THUMB_W, SLIDER_THUMB_H };
    fdk_surface_fill_rounded_rect(surface, thumb, SLIDER_THUMB_W / 2,
                                  thumb_col);
    if ((w->flags & FDK_WF_FOCUSED) != 0) {
        fdk_rect ring = { thumb.x + 1, thumb.y + 1, thumb.width - 2,
                          thumb.height - 2 };
        if (ring.width > 0 && ring.height > 0) {
            fdk_surface_draw_rounded_rect(surface, ring,
                                          (SLIDER_THUMB_W / 2) - 1,
                                          fdk__pal_accent());
        }
    }
}

/* local x -> value (thumb centered under the pointer). */
static double slider_value_at(fdk_slider *s, fdk_f32 local_x) {
    fdk_i32 inner = s->base.bounds.width - SLIDER_THUMB_W;
    if (inner <= 0) {
        return s->min;
    }
    double frac = ((double)local_x - SLIDER_THUMB_W / 2.0) /
                  (double)inner;
    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac > 1.0) {
        frac = 1.0;
    }
    return s->min + frac * slider_span(s);
}

static bool slider_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev) {
    fdk_slider *s = slider_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        s->dragging = true;
        slider_set_value(s, slider_value_at(s, ev->pointer.position.x),
                         true);
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_MOTION:
        if (s->dragging) {
            slider_set_value(s, slider_value_at(s, ev->position.x),
                             true);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_UP:
        s->dragging = false;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        double step = (s->step > 0.0)
            ? s->step
            : slider_span(s) / 100.0;
        if (step <= 0.0) {
            step = 1.0;
        }
        double big = slider_span(s) * SLIDER_KEY_PCT / 100.0;
        if (big <= 0.0) {
            big = step;
        }
        double want = s->value;
        switch (ev->key.scancode) {
        case FDK_KEY_LEFT:
        case FDK_KEY_DOWN:
            want -= step;
            break;
        case FDK_KEY_RIGHT:
        case FDK_KEY_UP:
            want += step;
            break;
        case FDK_KEY_PAGE_UP:
            want += big;
            break;
        case FDK_KEY_PAGE_DOWN:
            want -= big;
            break;
        case FDK_KEY_HOME:
            want = s->min;
            break;
        case FDK_KEY_END:
            want = s->max;
            break;
        default:
            return false;
        }
        slider_set_value(s, want, true);
        return true;
    }
    default:
        break;
    }
    return false;
}

static void slider_measure(fdk_widget *w, fdk_size *out) {
    fdk_slider *s = slider_of(w);
    out->width = SLIDER_MIN_W;
    out->height = SLIDER_MIN_H;
    /* Labeled marks widen the height by one label line — but only
     * when the label font can exist (a fontless system degrades to
     * ticks, and the height must match what paint actually draws). */
    if (s->mark_count > 0 && slider_has_labeled_mark(s) &&
        slider_label_font(s) != NULL) {
        fdk_i32 lw = 0, lh = 0;
        fdk__text_extent(s->label_font, "Ag", &lw, &lh);
        out->height += lh + 2;
    }
}

static void slider_destroy(fdk_widget *w) {
    fdk_slider *s = slider_of(w);
    if (s->marks != NULL) {
        for (size_t i = 0; i < s->mark_count; i++) {
            fdk_free(s->marks[i].label);
        }
        fdk_free(s->marks);
    }
    if (s->label_font != NULL) {
        fdk_font_destroy(s->label_font);
        s->label_font = NULL;
    }
}

/* ---- a11y ---- */

static void slider_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_slider *s = (const fdk_slider *)(const void *)w;
    out->has_value = true;
    out->value_min = s->min;
    out->value_max = s->max;
    out->value_current = s->value;
    out->value_text = fdk__a11y_valuef("%g", s->value);
}

static fdk_a11y_action_set slider_a11y_actions(const fdk_widget *w) {
    (void)w;
    return FDK_A11Y_ACTION_INCREMENT | FDK_A11Y_ACTION_DECREMENT |
           FDK_A11Y_ACTION_SET_VALUE;
}

static bool slider_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                double value) {
    fdk_slider *s = slider_of(w);
    /* The same quantized path the keyboard takes. */
    double step = (s->step > 0.0) ? s->step : slider_span(s) / 100.0;
    if (step <= 0.0) {
        step = 1.0;
    }
    switch (action) {
    case FDK_A11Y_ACTION_INCREMENT:
        slider_set_value(s, s->value + step, true);
        return true;
    case FDK_A11Y_ACTION_DECREMENT:
        slider_set_value(s, s->value - step, true);
        return true;
    case FDK_A11Y_ACTION_SET_VALUE:
        slider_set_value(s, value, true);
        return true;
    default:
        return false;
    }
}

static const fdk_a11y_class slider_a11y = {
    .role = FDK_A11Y_ROLE_SLIDER,
    .describe = slider_a11y_describe,
    .actions = slider_a11y_actions,
    .perform = slider_a11y_perform,
};

const fdk_widget_class fdk_slider_class_def = {
    .size = sizeof(fdk_slider),
    .name = "slider",
    .handle_event = slider_handle_event,
    .paint = slider_paint,
    .measure = slider_measure,
    .arrange = NULL,
    .destroy = slider_destroy,
    .a11y = &slider_a11y,
};

/* ---- public API ---- */

fdk_result fdk_slider_create(fdk_widget *parent, double min,
                             double max, double value,
                             fdk_widget **out_slider) {
    if (out_slider == NULL || max < min) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_slider_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_slider *s = slider_of(w);
    s->min = min;
    s->max = max;
    s->value = min;
    s->step = 0.0;
    s->marks = NULL;
    s->mark_count = 0;
    s->mark_cap = 0;
    s->label_font = NULL;
    s->label_font_failed = false;
    fdk_widget_set_can_focus(w, true);
    slider_set_value(s, value, false);
    fdk_widget_child_layout_changed(w->parent);
    *out_slider = w;
    return FDK_OK;
}

void fdk_slider_set_range(fdk_widget *slider, double min, double max) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return;
    }
    if (max < min || isnan(min) || isnan(max)) {
        /* create() refuses this input with FDK_ERR_INVALID_ARGUMENT;
         * the setter is void by convention, so it WARNs instead of
         * silently swallowing (a silent ignore here made
         * debugging "why didn't my range change" needlessly hard).
         * NaN is refused outright — it would poison every clamp. */
        FDK_WARN("fdk_slider_set_range: invalid range [%g, %g] ignored",
                 min, max);
        return;
    }
    fdk_slider *s = slider_of(slider);
    s->min = min;
    s->max = max;
    slider_set_value(s, s->value, false);
    fdk_widget_invalidate(slider);
}

void fdk_slider_set_step(fdk_widget *slider, double step) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def ||
        step < 0.0) {
        return;
    }
    slider_of(slider)->step = step;
}

void fdk_slider_set_value(fdk_widget *slider, double value) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return;
    }
    slider_set_value(slider_of(slider), value, true);
}

double fdk_slider_get_value(fdk_widget *slider) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return 0.0;
    }
    return slider_of(slider)->value;
}

void fdk_slider_set_on_changed(fdk_widget *slider,
                               fdk_slider_changed_fn on_changed,
                               void *user_data) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return;
    }
    fdk_slider *s = slider_of(slider);
    s->on_changed = on_changed;
    s->on_changed_data = user_data;
}

/* ---- marks (1.4.2) ---- */

fdk_result fdk_slider_add_mark(fdk_widget *slider, double value,
                               const char *label) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_slider *s = slider_of(slider);
    if (isnan(value)) {
        return FDK_ERR_INVALID_ARGUMENT; /* would poison the frac */
    }
    /* Clamp into the range (GTK's documented coercion — a mark at
     * 11 on a 0..10 scale reads as "at the end"). */
    if (value < s->min) {
        value = s->min;
    } else if (value > s->max) {
        value = s->max;
    }
    if (s->mark_count == s->mark_cap) {
        size_t cap = (s->mark_cap == 0) ? 4 : s->mark_cap * 2;
        if (cap < s->mark_cap ||
            cap > SIZE_MAX / sizeof(*s->marks)) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_slider_mark *grown =
            fdk_realloc(s->marks, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        s->marks = grown;
        s->mark_cap = cap;
    }
    char *copy = NULL;
    if (label != NULL && label[0] != '\0') {
        copy = fdk__strdup(label);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    s->marks[s->mark_count].value = value;
    s->marks[s->mark_count].label = copy;
    s->mark_count++;
    /* A first label (or the first mark at all) may change the
     * natural height: relayout. */
    fdk_widget_invalidate(slider);
    fdk_widget_child_layout_changed(slider->parent);
    return FDK_OK;
}

void fdk_slider_clear_marks(fdk_widget *slider) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return;
    }
    fdk_slider *s = slider_of(slider);
    if (s->mark_count == 0) {
        return;
    }
    for (size_t i = 0; i < s->mark_count; i++) {
        fdk_free(s->marks[i].label);
    }
    s->mark_count = 0;
    fdk_widget_invalidate(slider);
    fdk_widget_child_layout_changed(slider->parent);
}

size_t fdk_slider_mark_count(fdk_widget *slider) {
    if (slider == NULL || slider->klass != &fdk_slider_class_def) {
        return 0;
    }
    return slider_of(slider)->mark_count;
}
