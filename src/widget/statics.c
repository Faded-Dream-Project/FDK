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

/* The Label's ellipsis run shares the text layer's single definition
 * (measured and drawn characters can never drift apart). Same
 * layering pattern as the layout back-edge in widgets_internal.h. */
#include "../text/text_internal.h"
#include "../theme/theme_internal.h"

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
    /* Either output may be NULL (queries that need only one of the
     * two — NULL-tolerant since Phase 9's menu code queries widths
     * alone heavily). */
    if (out_w != NULL) {
        *out_w = 0;
    }
    if (out_h != NULL) {
        *out_h = 0;
    }
    if (font == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_text_metrics tm;
    if (fdk_ok(fdk_font_measure_utf8(font, text, strlen(text), &tm))) {
        if (out_w != NULL) {
            *out_w = tm.advance_width;
        }
    }
    if (out_h != NULL) {
        *out_h = fm.ascent + fm.descent;
    }
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

/* ---- themed palette accessors (Phase 7) ----
 *
 * The Phase 6 v1 palette became the Phase 7 built-in default theme,
 * byte-for-byte (src/theme/theme.c). These accessors stay the
 * catalog's single color seam: each resolves against the CURRENT
 * default theme at paint time, so fdk_theme_set_default() repaints
 * the world with no cached colors to flush. */

fdk_color fdk__pal_text(void) {
    return fdk_theme_get_color(NULL, FDK_TK_TEXT);
}
fdk_color fdk__pal_text_disabled(void) {
    return fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED);
}
fdk_color fdk__pal_control(void) {
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
}
fdk_color fdk__pal_control_hover(void) {
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_HOVER);
}
fdk_color fdk__pal_control_pressed(void) {
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_PRESSED);
}
fdk_color fdk__pal_control_disabled(void) {
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_DISABLED);
}
fdk_color fdk__pal_accent(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ACCENT);
}
fdk_color fdk__pal_track(void) {
    return fdk_theme_get_color(NULL, FDK_TK_TRACK);
}
fdk_color fdk__pal_border(void) {
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER);
}

/* ---- Label ---- */

/* Display-cache plumbing: the label's text broken into the lines
 * that fit its current width. Grown geometrically; freed at destroy.
 * `built_width`/`lines_dirty` decide staleness — rebuild on arrange
 * (resize) and lazily at paint (covers manual set_bounds callers
 * that bypass fdk_widget_arrange). */
static bool label_reserve(fdk_label *l, size_t needed) {
    if (needed <= l->lines_cap) {
        return true;
    }
    size_t cap = l->lines_cap > 0 ? l->lines_cap : 4;
    while (cap < needed) {
        cap *= 2;
    }
    fdk_text_line *grown =
        fdk_realloc(l->lines, cap * sizeof(fdk_text_line));
    if (grown == NULL) {
        return false; /* keep the old cache; paint uses what's there */
    }
    l->lines = grown;
    l->lines_cap = cap;
    return true;
}

static void label_reset_cache(fdk_label *l) {
    l->line_count = 0;
    l->ellipsis_prefix = 0;
    l->ellipsis_x = 0;
    l->ellipsis_w = 0;
    l->ellipsized = false;
}

/* Rebuilds the display cache for `width` pixels. Pure toolkit code:
 * no callbacks run, allocation failures degrade to an empty cache
 * (a paint that draws nothing rather than a crash). */
static void label_rebuild(fdk_label *l, fdk_i32 width) {
    label_reset_cache(l);
    l->lines_dirty = false;
    l->built_width = width;

    if (l->font == NULL || l->text == NULL || l->text[0] == '\0') {
        return; /* nothing to show: 0 lines */
    }
    size_t len = strlen(l->text);

    if (l->mode == FDK_LABEL_ELLIPSIZE && width > 0) {
        size_t prefix = len;
        bool fits = true;
        (void)fdk_font_ellipsize_utf8(l->font, l->text, len, width,
                                      &prefix, &fits);
        fdk_text_metrics pm;
        fdk_i32 prefix_adv = 0;
        if (prefix > 0 &&
            fdk_ok(fdk_font_measure_utf8(l->font, l->text, prefix,
                                         &pm))) {
            prefix_adv = pm.advance_width;
        }
        fdk_text_metrics em;
        fdk_i32 ell_w = 0;
        if (!fits &&
            fdk_ok(fdk_font_measure_utf8(l->font,
                                         FDK_TEXT_ELLIPSIS_UTF8,
                                         FDK_TEXT_ELLIPSIS_BYTES, &em))) {
            ell_w = em.advance_width;
        }
        if (!label_reserve(l, 1)) {
            return;
        }
        fdk_text_metrics whole;
        fdk_i32 full_adv = 0;
        if (fdk_ok(fdk_font_measure_utf8(l->font, l->text, len,
                                         &whole))) {
            full_adv = whole.advance_width;
        }
        l->lines[0].byte_offset = 0;
        l->lines[0].byte_len = fits ? len : prefix;
        l->lines[0].advance_width =
            fits ? full_adv : prefix_adv + ell_w;
        l->line_count = 1;
        l->ellipsized = !fits;
        l->ellipsis_prefix = prefix;
        l->ellipsis_x = prefix_adv;
        l->ellipsis_w = ell_w;
        return;
    }

    if (l->mode == FDK_LABEL_WRAP && width > 0) {
        size_t count = 0;
        if (!fdk_ok(fdk_font_break_lines_utf8(l->font, l->text, len,
                                              width, NULL, 0, &count,
                                              NULL)) ||
            count == 0 || !label_reserve(l, count)) {
            return; /* error or nothing that fits: empty cache */
        }
        size_t filled = 0;
        (void)fdk_font_break_lines_utf8(l->font, l->text, len, width,
                                        l->lines, l->lines_cap, &filled,
                                        NULL);
        l->line_count = filled;
        return;
    }

    /* NOWRAP — and the degenerate WRAP/ELLIPSIZE width-0 case: one
     * full line; the label's bounds clip whatever overflows. */
    fdk_text_metrics whole;
    fdk_i32 adv = 0;
    if (fdk_ok(fdk_font_measure_utf8(l->font, l->text, len, &whole))) {
        adv = whole.advance_width;
    }
    if (!label_reserve(l, 1)) {
        return;
    }
    l->lines[0].byte_offset = 0;
    l->lines[0].byte_len = len;
    l->lines[0].advance_width = adv;
    l->line_count = 1;
}

/* Rebuilds when stale: dirty flag set by text/mode setters, or the
 * width moved since the last build (resize without arrange — e.g. a
 * direct set_bounds). */
static void label_ensure(fdk_label *l, fdk_i32 width) {
    if (l->lines_dirty || width != l->built_width) {
        label_rebuild(l, width);
    }
}

static void label_measure(fdk_widget *w, fdk_size *out) {
    fdk_label *l = label_of(w);
    out->width = 0;
    out->height = 0;
    if (l->font == NULL || l->text == NULL || l->text[0] == '\0') {
        fdk__widget_set_baseline(w, -1); /* no text, no baseline */
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(l->font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;
    /* Baseline (Phase 5 completion): the label's text baseline is its
     * font's ascent from the top — what FDK_ALIGN_BASELINE aligns
     * rows of labels on. */
    fdk__widget_set_baseline(w, fm.ascent);

    if (l->mode != FDK_LABEL_WRAP) {
        /* NOWRAP and ELLIPSIZE: the natural size is the full text —
         * an ellipsized label shows everything it gets room for. */
        fdk__text_extent(l->font, l->text, &out->width, &out->height);
        return;
    }

    /* WRAP: width = the request when one is set, else the whole
     * advance (one line, until something constrains the width).
     * Height = lines at that width * pitch — computed with a local
     * pass, not the paint cache (measure must stay side-effect
     * free). v1 has no width-for-height layout; the header documents
     * the narrower-than-request clipping contract. */
    size_t len = strlen(l->text);
    fdk_text_metrics whole;
    fdk_i32 width = w->natural_w;
    if (width <= 0 &&
        fdk_ok(fdk_font_measure_utf8(l->font, l->text, len, &whole))) {
        width = whole.advance_width;
    }
    out->width = width > 0 ? width : 0;
    size_t count = 0;
    if (width > 0) {
        (void)fdk_font_break_lines_utf8(l->font, l->text, len, width,
                                        NULL, 0, &count, NULL);
    }
    out->height = (fdk_i32)count * pitch;
}

static void label_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_label *l = label_of(w);
    label_ensure(l, assigned.width);
}

static void label_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_label *l = label_of(w);
    label_ensure(l, bounds.width);
    if (l->line_count == 0 || l->font == NULL) {
        return;
    }
    fdk_color color = l->color.a > 0.0f ? l->color
                                        : (((w->flags & FDK_WF_ENABLED) != 0) ? fdk__pal_text()
                                                      : fdk__pal_text_disabled());
    fdk_font_metrics fm;
    fdk_font_get_metrics(l->font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;

    for (size_t i = 0; i < l->line_count; i++) {
        const fdk_text_line *line = &l->lines[i];
        fdk_i32 x = bounds.x;
        if (l->align == FDK_ALIGN_CENTER || l->align == FDK_ALIGN_END) {
            if (line->advance_width < bounds.width) {
                fdk_i32 slack = bounds.width - line->advance_width;
                x = bounds.x + (l->align == FDK_ALIGN_CENTER
                                    ? slack / 2
                                    : slack);
            }
        }
        fdk_i32 baseline = bounds.y + (fdk_i32)i * pitch + fm.ascent;

        if (line->byte_len > 0) {
            (void)fdk_surface_draw_utf8(surface, l->font,
                                        l->text + line->byte_offset,
                                        line->byte_len, x, baseline,
                                        color);
        }
        /* ELLIPSIZE: the ellipsis run lands exactly where the prefix
         * pen stopped (same rounding as the layout pass). */
        if (l->ellipsized && i == 0 && l->ellipsis_w > 0) {
            (void)fdk_surface_draw_utf8(surface, l->font,
                                        FDK_TEXT_ELLIPSIS_UTF8,
                                        FDK_TEXT_ELLIPSIS_BYTES,
                                        x + l->ellipsis_x, baseline,
                                        color);
        }
    }
}

static void label_destroy(fdk_widget *w) {
    fdk_label *l = label_of(w);
    fdk_free(l->text);
    fdk_free(l->lines);
}

/* ---- a11y ---- */

static void label_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_label *l = (const fdk_label *)(const void *)w;
    if (l->text != NULL) {
        out->name = fdk__strdup(l->text);
    }
}

static const fdk_a11y_class label_a11y = {
    .role = FDK_A11Y_ROLE_LABEL,
    .describe = label_a11y_describe,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_label_class_def = {
    .size = sizeof(fdk_label),
    .name = "label",
    .handle_event = NULL,
    .paint = label_paint,
    .measure = label_measure,
    .arrange = label_arrange,
    .destroy = label_destroy,
    .a11y = &label_a11y,
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
    l->mode = FDK_LABEL_NOWRAP;
    l->align = FDK_ALIGN_START;
    l->built_width = -1; /* nothing built yet */
    l->lines_dirty = true;
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
    l->lines_dirty = true;
    fdk_widget_invalidate(label);
    fdk_widget_child_layout_changed(label->parent);
    /* A11y: the label's text IS its accessible name. */
    fdk__a11y_notify(label, FDK_A11Y_NAME_CHANGED, 0);
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

void fdk_label_set_mode(fdk_widget *label, fdk_label_mode mode) {
    if (label == NULL || label->klass != &fdk_label_class_def ||
        (mode != FDK_LABEL_NOWRAP && mode != FDK_LABEL_WRAP &&
         mode != FDK_LABEL_ELLIPSIZE)) {
        return;
    }
    fdk_label *l = label_of(label);
    if (l->mode == mode) {
        return;
    }
    l->mode = mode;
    l->lines_dirty = true;
    fdk_widget_invalidate(label);
    fdk_widget_child_layout_changed(label->parent);
}

fdk_label_mode fdk_label_get_mode(fdk_widget *label) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return FDK_LABEL_NOWRAP;
    }
    return label_of(label)->mode;
}

void fdk_label_set_alignment(fdk_widget *label, fdk_align alignment) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return;
    }
    label_of(label)->align = alignment;
    fdk_widget_invalidate(label);
}

fdk_align fdk_label_get_alignment(fdk_widget *label) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return FDK_ALIGN_START;
    }
    return label_of(label)->align;
}

size_t fdk_label_get_line_count(fdk_widget *label) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return 0;
    }
    fdk_label *l = label_of(label);
    label_ensure(l, label->bounds.width);
    return l->line_count;
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

/* ---- a11y ---- */

static void progress_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = 1.0;
    out->value_current =
        (double)((const fdk_progress *)(const void *)w)->fraction;
    out->value_text = fdk__a11y_valuef("%.0f%%",
                                       out->value_current * 100.0);
}

static const fdk_a11y_class progress_a11y = {
    .role = FDK_A11Y_ROLE_PROGRESS_BAR,
    .describe = progress_a11y_describe,
    .actions = NULL, /* an indicator, not a control */
    .perform = NULL,
};

const fdk_widget_class fdk_progress_class_def = {
    .size = sizeof(fdk_progress),
    .name = "progress",
    .handle_event = NULL,
    .paint = progress_paint,
    .measure = NULL, /* natural size = the size request */
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &progress_a11y,
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
    /* A11y: the fraction IS the value interface. */
    fdk__a11y_notify(progress, FDK_A11Y_VALUE_CHANGED, 0);
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

    /* Themed band thickness (default 1 = the v1 rule exactly: same
     * center line, draw_rect/fill_rect agree at 1px). Paint-time only
     * - the size request is the application's (docs/fdk-theme-format
     * .md). */
    fdk_i32 t = fdk_theme_get_metric(NULL, FDK_TM_SEPARATOR_THICKNESS);
    if (sep->orientation == FDK_HORIZONTAL) {
        if (t > bounds.height) {
            t = bounds.height;
        }
        fdk_i32 y = bounds.y + bounds.height / 2 - (t - 1) / 2;
        fdk_surface_fill_rect(surface,
                              (fdk_rect){bounds.x, y, bounds.width, t},
                              c);
    } else {
        if (t > bounds.width) {
            t = bounds.width;
        }
        fdk_i32 x = bounds.x + bounds.width / 2 - (t - 1) / 2;
        fdk_surface_fill_rect(surface,
                              (fdk_rect){x, bounds.y, t, bounds.height},
                              c);
    }
}

static const fdk_a11y_class separator_a11y = {
    .role = FDK_A11Y_ROLE_SEPARATOR,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_separator_class_def = {
    .size = sizeof(fdk_separator),
    .name = "separator",
    .handle_event = NULL,
    .paint = separator_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &separator_a11y,
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

/* ---- a11y ---- */

static void frame_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_frame *f = (const fdk_frame *)(const void *)w;
    if (f->title != NULL) {
        out->name = fdk__strdup(f->title);
    }
}

static const fdk_a11y_class frame_a11y = {
    .role = FDK_A11Y_ROLE_GROUP, /* a labeled container */
    .describe = frame_a11y_describe,
    .actions = NULL,
    .perform = NULL,
};

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
    .a11y = &frame_a11y,
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
    /* A11y: the title IS the group's accessible name. */
    fdk__a11y_notify(frame, FDK_A11Y_NAME_CHANGED, 0);
    return FDK_OK;
}
