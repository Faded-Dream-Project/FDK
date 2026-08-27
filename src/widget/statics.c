/*
 * statics.c — non-interactive catalog widgets (Label, ProgressBar,
 * Separator, Frame) plus the helpers and v1 palette shared with
 * controls.c.
 *
 * See include/fdk/fdk_widgets.h for the public contract and
 * src/widget/widgets_internal.h for the instance structs.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"

#include "core/alloc_internal.h"
#include <stdio.h>
#include "core/log_internal.h"

/* ---- shared helpers ---- */

char *fdk__strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = fdk_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

void fdk__text_extent(const fdk_font *font, const char *text,
                      fdk_i32 *out_w, fdk_i32 *out_h) {
    *out_w = 0;
    *out_h = 0;
    if (font == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_text_metrics tm;
    if (fdk_ok(fdk_font_measure_utf8(font, text, strlen(text), &tm))) {
        *out_w = tm.advance_width;
    }
    *out_h = fm.ascent + fm.descent;
}

void fdk__draw_text(fdk_surface *surface, fdk_font *font,
                    const char *text, fdk_color color, fdk_i32 x,
                    fdk_i32 baseline) {
    if (surface == NULL || font == NULL || text == NULL ||
        text[0] == '\0') {
        return;
    }
    (void)fdk_surface_draw_utf8(surface, font, text, strlen(text), x,
                                baseline, color);
}

fdk_i32 fdk__center_baseline(const fdk_font *font, fdk_i32 top,
                             fdk_i32 avail_h) {
    if (font == NULL) {
        return top;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_i32 text_h = fm.ascent + fm.descent;
    fdk_i32 pad = avail_h - text_h;
    if (pad < 0) {
        pad = 0;
    }
    return top + pad / 2 + fm.ascent;
}

/* ---- v1 palette ---- */

fdk_color fdk__pal_text(void) {
    return (fdk_color){0.92f, 0.93f, 0.96f, 1.0f};
}
fdk_color fdk__pal_text_disabled(void) {
    return (fdk_color){0.45f, 0.47f, 0.52f, 1.0f};
}
fdk_color fdk__pal_control(void) {
    return (fdk_color){0.16f, 0.18f, 0.26f, 1.0f};
}
fdk_color fdk__pal_control_hover(void) {
    return (fdk_color){0.22f, 0.25f, 0.36f, 1.0f};
}
fdk_color fdk__pal_control_pressed(void) {
    return (fdk_color){0.28f, 0.32f, 0.46f, 1.0f};
}
fdk_color fdk__pal_control_disabled(void) {
    return (fdk_color){0.12f, 0.13f, 0.18f, 1.0f};
}
fdk_color fdk__pal_accent(void) {
    return (fdk_color){0.35f, 0.65f, 0.95f, 1.0f};
}
fdk_color fdk__pal_track(void) {
    return (fdk_color){0.10f, 0.12f, 0.17f, 1.0f};
}
fdk_color fdk__pal_border(void) {
    return (fdk_color){0.30f, 0.33f, 0.44f, 1.0f};
}

/* ---- Label ---- */

static void label_measure(fdk_widget *w, fdk_size *out) {
    fdk_label *l = label_of(w);
    fdk__text_extent(l->font, l->text, &out->width, &out->height);
}

static void label_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_label *l = label_of(w);
    if (l->font == NULL || l->text == NULL) {
        return;
    }
    fdk_color color = l->color.a > 0.0f ? l->color
                                        : (((w->flags & FDK_WF_ENABLED) != 0) ? fdk__pal_text()
                                                      : fdk__pal_text_disabled());
    fdk_font_metrics fm;
    fdk_font_get_metrics(l->font, &fm);
    fdk__draw_text(surface, l->font, l->text, color, bounds.x,
                   bounds.y + fm.ascent);
}

static void label_destroy(fdk_widget *w) {
    fdk_free(label_of(w)->text);
}

const fdk_widget_class fdk_label_class_def = {
    .size = sizeof(fdk_label),
    .name = "label",
    .handle_event = NULL,
    .paint = label_paint,
    .measure = label_measure,
    .arrange = NULL,
    .destroy = label_destroy,
};

fdk_result fdk_label_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_label) {
    if (out_label == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_label_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_label *l = label_of(w);
    l->font = font;
    l->text = fdk__strdup(text);
    l->color = (fdk_color){0, 0, 0, 0}; /* -> palette default */
    if (text != NULL && l->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* Re-measure with real fields (the create-time notify ran before
     * the subclass constructor initialized them). */
    fdk_widget_child_layout_changed(w->parent);
    *out_label = w;
    return FDK_OK;
}

fdk_result fdk_label_set_text(fdk_widget *label, const char *text) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_label *l = label_of(label);
    char *copy = fdk__strdup(text);
    if (text != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(l->text);
    l->text = copy;
    fdk_widget_invalidate(label);
    fdk_widget_child_layout_changed(label->parent);
    return FDK_OK;
}

void fdk_label_set_color(fdk_widget *label, fdk_color color) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return;
    }
    label_of(label)->color = color;
    fdk_widget_invalidate(label);
}

const char *fdk_label_get_text(fdk_widget *label) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return NULL;
    }
    return label_of(label)->text;
}

/* ---- ProgressBar ---- */

#define PROGRESS_MIN_TRACK_H 4

static void progress_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_progress *p = progress_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_i32 r = bounds.height / 2;
    if (r > 8) {
        r = 8;
    }
    fdk_surface_fill_rounded_rect(surface, bounds, r,
                                  ((w->flags & FDK_WF_ENABLED) != 0) ? fdk__pal_track()
                                             : fdk__pal_control_disabled());
    fdk_i32 fill_w =
        (fdk_i32)((fdk_f32)bounds.width * p->fraction + 0.5f);
    if (fill_w > bounds.width) {
        fill_w = bounds.width;
    }
    if (fill_w < PROGRESS_MIN_TRACK_H) {
        return; /* nothing filled yet (or 0) */
    }
    fdk_rect fill = {bounds.x, bounds.y, fill_w, bounds.height};
    fdk_surface_fill_rounded_rect(surface, fill, r < fill_w ? r : fill_w,
                                  fdk__pal_accent());
}

const fdk_widget_class fdk_progress_class_def = {
    .size = sizeof(fdk_progress),
    .name = "progress",
    .handle_event = NULL,
    .paint = progress_paint,
    .measure = NULL, /* natural size = the size request */
    .arrange = NULL,
    .destroy = NULL,
};

fdk_result fdk_progress_create(fdk_widget *parent,
                               fdk_widget **out_progress) {
    if (out_progress == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_progress_class_def,
                                     (fdk_rect){0, 0, 0, 12}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    progress_of(w)->fraction = 0.0f;
    *out_progress = w;
    return FDK_OK;
}

void fdk_progress_set_fraction(fdk_widget *progress, fdk_f32 fraction) {
    if (progress == NULL || progress->klass != &fdk_progress_class_def) {
        return;
    }
    if (fraction < 0.0f) {
        fraction = 0.0f;
    } else if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    fdk_progress *p = progress_of(progress);
    if (p->fraction == fraction) {
        return;
    }
    p->fraction = fraction;
    fdk_widget_invalidate(progress);
}

fdk_f32 fdk_progress_get_fraction(fdk_widget *progress) {
    if (progress == NULL || progress->klass != &fdk_progress_class_def) {
        return 0.0f;
    }
    return progress_of(progress)->fraction;
}

/* ---- Separator ---- */

static void separator_paint(fdk_widget *w, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_separator *sep = separator_of(w);
    fdk_color c = ((w->flags & FDK_WF_ENABLED) != 0) ? fdk__pal_border()
                             : fdk__pal_control_disabled();
    if (sep->orientation == FDK_HORIZONTAL) {
        fdk_i32 y = bounds.y + bounds.height / 2;
        fdk_surface_draw_rect(surface,
                              (fdk_rect){bounds.x, y, bounds.width, 1},
                              c);
    } else {
        fdk_i32 x = bounds.x + bounds.width / 2;
        fdk_surface_draw_rect(surface,
                              (fdk_rect){x, bounds.y, 1, bounds.height},
                              c);
    }
}

const fdk_widget_class fdk_separator_class_def = {
    .size = sizeof(fdk_separator),
    .name = "separator",
    .handle_event = NULL,
    .paint = separator_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
};

fdk_result fdk_separator_create(fdk_widget *parent,
                                fdk_orientation orientation,
                                fdk_widget **out_separator) {
    if (out_separator == NULL || (orientation != FDK_HORIZONTAL &&
                                  orientation != FDK_VERTICAL)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_separator_class_def,
                                     (fdk_rect){0, 0, 1, 1}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    separator_of(w)->orientation = orientation;
    *out_separator = w;
    return FDK_OK;
}

/* ---- Frame ---- */

/* Title band height: the text line plus breathing room. Reserved
 * whenever the frame has a font (deterministic layout regardless of
 * whether a title string is currently set). */
static fdk_i32 frame_title_band(const fdk_frame *f) {
    if (f->font == NULL) {
        return 0;
    }
    fdk_i32 w = 0, h = 0;
    fdk__text_extent(f->font, "Ag", &w, &h);
    return h + 8;
}

static void frame_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_frame *f = frame_of(w);

    /* Background (if set), then border, then the title in its band.
     * Children paint on top afterwards (normal tree walk). */
    if (w->background.a > 0.0f) {
        if (w->corner_radius > 0) {
            fdk_surface_fill_rounded_rect(surface, bounds,
                                          w->corner_radius,
                                          w->background);
        } else {
            fdk_surface_fill_rect(surface, bounds, w->background);
        }
    }
    fdk_surface_draw_rounded_rect(surface, bounds, 8, fdk__pal_border());

    if (f->font != NULL && f->title != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(f->font, &fm);
        /* Vertically centered in the title band (band = text_h + 8,
         * so 4 px above the cap line, 4 below the baseline). */
        fdk__draw_text(surface, f->font, f->title, fdk__pal_text(),
                       bounds.x + f->base.padding + 2,
                       bounds.y + f->base.padding + 4 + fm.ascent);
    }
}

static void frame_destroy(fdk_widget *w) {
    fdk_free(frame_of(w)->title);
}

const fdk_widget_class fdk_frame_class_def = {
    .size = sizeof(fdk_frame),
    .name = "frame",
    .handle_event = NULL,
    .paint = frame_paint,
    /* Box packing: measure/arrange come from the box class so the
     * frame lays its children out exactly like a vertical box (with
     * the title band reserved via title_inset). */
    .measure = fdk_box_measure_hook,
    .arrange = fdk_box_arrange_hook,
    .destroy = frame_destroy,
};

fdk_result fdk_frame_create(fdk_widget *parent, fdk_font *font,
                            const char *title, fdk_widget **out_frame) {
    if (out_frame == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_frame_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_frame *f = frame_of(w);
    f->base.orientation = FDK_VERTICAL;
    f->base.spacing = 8;
    f->base.padding = 10;
    f->font = font;
    f->base.title_inset = frame_title_band(f);
    f->title = fdk__strdup(title);
    if (title != NULL && f->title == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* Box fields + title band are real now: re-notify. */
    fdk_widget_child_layout_changed(w->parent);
    *out_frame = w;
    return FDK_OK;
}

fdk_result fdk_frame_set_title(fdk_widget *frame, const char *title) {
    if (frame == NULL || frame->klass != &fdk_frame_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_frame *f = frame_of(frame);
    char *copy = fdk__strdup(title);
    if (title != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(f->title);
    f->title = copy;
    fdk_widget_invalidate(frame);
    return FDK_OK;
}
