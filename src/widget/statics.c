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
#include "../window/window_internal.h" /* timer-driven progress pulse */

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

/* 1.4.0 modern-face accessors — same single-seam contract as the
 * family above: resolve against the CURRENT default at paint time.
 * The selection/focus-ring pair finally wires the 1.3.2 tokens into
 * the catalog (they were theme-file-only until now; the hardcoded
 * accent-alpha call sites are gone). */
fdk_color fdk__pal_selection(void) {
    return fdk_theme_get_color(NULL, FDK_TK_SELECTION_BACKGROUND);
}
fdk_color fdk__pal_focus_ring(void) {
    return fdk_theme_get_color(NULL, FDK_TK_FOCUS_RING);
}
fdk_color fdk__pal_sidebar(void) {
    return fdk_theme_get_color(NULL, FDK_TK_SIDEBAR_BACKGROUND);
}
fdk_color fdk__pal_menu_bg(void) {
    return fdk_theme_get_color(NULL, FDK_TK_MENU_BACKGROUND);
}
fdk_color fdk__pal_accent_hover(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ACCENT_HOVER);
}
fdk_color fdk__pal_accent_pressed(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ACCENT_PRESSED);
}
fdk_color fdk__pal_accent_text(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ACCENT_TEXT);
}
fdk_color fdk__pal_link(void) {
    return fdk_theme_get_color(NULL, FDK_TK_LINK);
}
fdk_color fdk__pal_entry(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ENTRY_BACKGROUND);
}
fdk_color fdk__pal_entry_border(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ENTRY_BORDER);
}
fdk_color fdk__pal_row_hover(void) {
    return fdk_theme_get_color(NULL, FDK_TK_ROW_HOVER);
}

/* ---- 1.4.1: the shared symbolic row glyphs ----------------------
 *
 * Promoted from list.c's statics in 1.4.2 so the Tree wears the same
 * vector language. Font-independent strokes in the same idiom as the
 * title-bar and disclosure glyphs: neutral foreground ink, no fills
 * that would fight theme changes (the folder's tab keeps a soft
 * control fill so it reads at 16 px without a fill-rate-heavy
 * interior). Geometry: a 16-px glyph box, 6-px gap to the text —
 * the sidebar-symbolic set. */

#define LIST_ICON 16
#define LIST_ICON_GAP 6

fdk_i32 fdk__row_icon_advance(fdk_row_icon icon) {
    return (icon == FDK_ROW_ICON_NONE) ? 0
                                        : LIST_ICON + LIST_ICON_GAP;
}

void fdk__row_icon_paint(fdk_surface *surface, fdk_row_icon icon,
                         fdk_i32 x, fdk_i32 cy, bool disabled) {
    /* (x, cy) = the glyph box's top-left corner; the box is
     * LIST_ICON square. Ink: text color (disabled dims). */
    fdk_color ink = disabled ? fdk__pal_text_disabled() : fdk__pal_text();
    fdk_color soft = disabled ? fdk__pal_control_disabled()
                              : fdk__pal_control();
    switch (icon) {
    case FDK_ROW_ICON_FOLDER: {
        /* Tabbed folder: the tab spans the top-left, the body below
         * — outline + soft fill, rounded 2. */
        fdk_rect body = {x + 1, cy + 4, 14, 10};
        fdk_surface_fill_rounded_rect(surface, body, 2, soft);
        fdk_surface_draw_rounded_rect(surface, body, 2, ink);
        fdk_rect tab = {x + 1, cy + 2, 6, 2};
        fdk_surface_fill_rect(surface, tab, ink);
        break;
    }
    case FDK_ROW_ICON_HOME: {
        /* House: roof strokes + door. */
        fdk_surface_draw_line_aa(surface, x + 1, cy + 7, x + 8, cy + 2,
                                 ink);
        fdk_surface_draw_line_aa(surface, x + 8, cy + 2, x + 15, cy + 7,
                                 ink);
        fdk_rect wall = {x + 3, cy + 7, 10, 7};
        fdk_surface_draw_rect(surface, wall, ink);
        fdk_rect door = {x + 7, cy + 10, 2, 4};
        fdk_surface_fill_rect(surface, door, ink);
        break;
    }
    case FDK_ROW_ICON_DRIVE: {
        /* Slab: rounded outline + soft fill + the activity LED. */
        fdk_rect slab = {x + 2, cy + 4, 12, 8};
        fdk_surface_fill_rounded_rect(surface, slab, 3, soft);
        fdk_surface_draw_rounded_rect(surface, slab, 3, ink);
        fdk_surface_fill_circle(surface, x + 11, cy + 8, 1, ink);
        break;
    }
    case FDK_ROW_ICON_FILE: {
        /* Page: outline with a folded top-right corner. */
        fdk_rect page = {x + 3, cy + 2, 10, 12};
        fdk_surface_draw_rect(surface, page, ink);
        /* The fold: a diagonal + the corner notch. */
        fdk_surface_draw_line_aa(surface, x + 9, cy + 2, x + 13, cy + 6,
                                 ink);
        fdk_surface_fill_rect(surface,
                              (fdk_rect){x + 10, cy + 3, 3, 3}, soft);
        break;
    }
    case FDK_ROW_ICON_RECENT: {
        /* Clock (1.4.4, the recents place): a circled face with the
         * hands at ten-past-ten — the classic recents glyph. */
        fdk_surface_draw_circle_aa(surface, x + 8, cy + 8, 6, ink);
        fdk_surface_draw_line_aa(surface, x + 8, cy + 8, x + 8, cy + 3,
                                 ink);
        fdk_surface_draw_line_aa(surface, x + 8, cy + 8, x + 12, cy + 10,
                                 ink);
        break;
    }
    default:
        break; /* NONE: the caller did not reserve the box */
    }
}

/* ---- 1.4.2: the shared hover-fade flight (promoted from
 * controls.c) ------------------------------------------------------
 *
 * The 1.4.1 Button/check-family machinery, shared verbatim by the
 * StackSwitcher's pills and the menu rows: 120 ms of quad-out, one
 * flight at a time per fade state, retarget-from-live (the
 * animator's compose rule). The SEMANTIC hover flag still flips
 * instantly wherever the consumer keeps one — only the PAINT
 * blends. */

#define HOVER_FADE_MS 120

static void fdk__hover_fade_tick(fdk_animation *anim, double eased,
                                 void *user) {
    (void)anim;
    fdk_hover_fade *f = user;
    f->t = f->from + (f->target - f->from) * (fdk_f32)eased;
    /* The animator invalidates the attached widget after this
     * returns — no manual invalidate needed (and none wanted:
     * this tick owns nothing but the blend). */
}

void fdk__hover_fade_arm(fdk_widget *w, fdk_hover_fade *f,
                         bool entering) {
    if (f->anim != NULL) {
        fdk_animation_cancel(f->anim);
        f->anim = NULL;
    }
    f->target = entering ? 1.0f : 0.0f;
    if (f->t == f->target) {
        return; /* already there; nothing to fly */
    }
    f->from = f->t;
    f->anim = fdk_widget_animate(w, HOVER_FADE_MS, FDK_EASE_QUAD_OUT,
                                 fdk__hover_fade_tick, NULL, f);
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
    fdk_color color;
    if (l->color_set) {
        color = l->color; /* an explicit color wins, even transparent */
    } else if ((w->flags & FDK_WF_ENABLED) != 0) {
        color = fdk__pal_text();
    } else {
        color = fdk__pal_text_disabled();
    }
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

/* ---- a11y text interface (read-only) ----
 *
 * A Label's text is not editable: length and at-offset queries work,
 * caret/selection report nothing. The "word" and "line" granularities
 * use the same whitespace-run definition as the Entry. */

static size_t label_text_length(const fdk_widget *w) {
    const fdk_label *l = (const fdk_label *)(const void *)w;
    return (l->text != NULL) ? strlen(l->text) : 0;
}

/* Whitespace-delimited word boundaries around byte offset i (the
 * Entry's word_range twin, local because it is static there). */
static void label_word_range(const char *s, size_t len, size_t i,
                             size_t *out_start, size_t *out_end) {
    if (len == 0 || i >= len) {
        *out_start = len;
        *out_end = len;
        return;
    }
    bool in_ws = (s[i] == ' ' || s[i] == '\t');
    size_t start = i;
    while (start > 0 &&
           ((s[start - 1] == ' ' || s[start - 1] == '\t') == in_ws)) {
        start--;
    }
    size_t end = i;
    while (end < len && ((s[end] == ' ' || s[end] == '\t') == in_ws)) {
        end++;
    }
    *out_start = start;
    *out_end = end;
}

static size_t label_utf8_prev(const char *s, size_t i) {
    size_t p = i - 1;
    while (p > 0 && ((s[p] & 0xC0) == 0x80)) {
        p--;
    }
    return p;
}

static bool label_text_at(const fdk_widget *w, size_t offset,
                          fdk_a11y_text_granularity granularity,
                          char *buf, size_t cap, size_t *out_start,
                          size_t *out_end) {
    const fdk_label *l = (const fdk_label *)(const void *)w;
    if (buf == NULL || cap == 0) {
        return false;
    }
    buf[0] = '\0';
    const char *text = (l->text != NULL) ? l->text : "";
    size_t len = strlen(text);
    if (len == 0) {
        if (out_start != NULL) {
            *out_start = 0;
        }
        if (out_end != NULL) {
            *out_end = 0;
        }
        return true;
    }
    if (offset > len) {
        offset = len;
    }

    size_t start = 0;
    size_t end = len;
    switch (granularity) {
    case FDK_A11Y_TEXT_CHAR: {
        /* Snap to a codepoint boundary (labels have no caret, so
         * mid-cluster offsets just round down). */
        while (offset < len && ((text[offset] & 0xC0) == 0x80)) {
            offset++;
        }
        if (offset >= len) {
            offset = label_utf8_prev(text, len);
        }
        start = offset;
        end = offset + 1;
        while (end < len && ((text[end] & 0xC0) == 0x80)) {
            end++;
        }
        break;
    }
    case FDK_A11Y_TEXT_WORD: {
        if (offset == len) {
            offset = len - 1;
        }
        label_word_range(text, len, offset, &start, &end);
        break;
    }
    case FDK_A11Y_TEXT_LINE:
    default:
        /* A Label may wrap at paint time, but v1 reports the whole
         * text as one line (visual line runs need the display cache
         * rebuilt; documented in the roadmap). */
        break;
    }

    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
    size_t n = end - start;
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(buf, text + start, n);
    buf[n] = '\0';
    return true;
}

static const fdk_a11y_class label_a11y = {
    .role = FDK_A11Y_ROLE_LABEL,
    .describe = label_a11y_describe,
    .actions = NULL,
    .perform = NULL,
    .text_length = label_text_length,
    .text_caret = NULL,
    .text_selection = NULL,
    .text_at = label_text_at,
    .text_set_caret = NULL,
    .text_set_selection = NULL,
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
    l->color = (fdk_color){0, 0, 0, 0};
    l->color_set = false; /* -> palette default at paint time */
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
    fdk_label *l = label_of(label);
    l->color = color;
    l->color_set = true; /* explicit color, even a transparent one */
    fdk_widget_invalidate(label);
}

/* The label's text color (the theme's text color when never set —
 * 1.3.0 getter symmetry with set_color). */
fdk_color fdk_label_get_color(fdk_widget *label) {
    if (label == NULL || label->klass != &fdk_label_class_def) {
        return fdk__pal_text();
    }
    fdk_label *l = label_of(label);
    if (l->color_set) {
        return l->color;
    }
    return fdk__pal_text();
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

/* Natural track height (the create-time request when the app gives
 * no explicit bounds; layout stretches widths via expand). */
#define PROGRESS_TRACK_H 12
/* Indeterminate mode: the sweeping block's width, as a fraction of
 * the track; the sweep cadence, in ms per timer tick and phase step
 * per tick (a full traversal every ~1.9s — the same visual tempo
 * GTK's activity mode reads as). */
#define PROGRESS_BLOCK 0.25f
#define PROGRESS_PULSE_MS 40
#define PROGRESS_PULSE_STEP 0.013f

static void progress_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = 0;                  /* meaningless without a slot */
    out->height = PROGRESS_TRACK_H; /* the visible track extent    */
}

static void progress_pulse_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_progress *p = user;
    p->pulse_phase += PROGRESS_PULSE_STEP;
    if (p->pulse_phase >= 1.0f + PROGRESS_BLOCK) {
        p->pulse_phase = 0.0f;
    }
    fdk_widget_invalidate(&p->base);
}

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
    if (p->indeterminate) {
        /* The sweeping block: phase in [0, 1 + BLOCK), so the block
         * enters from the left, traverses, and exits right before
         * wrapping — the classic activity-mode read. */
        fdk_f32 bw_f = PROGRESS_BLOCK * (fdk_f32)bounds.width;
        fdk_i32 bw = (fdk_i32)(bw_f + 0.5f);
        if (bw < 2) {
            bw = 2;
        }
        fdk_f32 span = (fdk_f32)bounds.width + (fdk_f32)bw;
        fdk_i32 x = (fdk_i32)(p->pulse_phase * span - (fdk_f32)bw + 0.5f);
        fdk_i32 bx0 = bounds.x + x;
        if (bx0 < bounds.x) {
            bx0 = bounds.x; /* clip the entering block */
        }
        fdk_i32 bx1 = bounds.x + x + bw;
        if (bx1 > bounds.x + bounds.width) {
            bx1 = bounds.x + bounds.width; /* and the exiting one */
        }
        if (bx1 > bx0) {
            fdk_rect block = {bx0, bounds.y, bx1 - bx0, bounds.height};
            fdk_i32 br = r;
            if (br > (bx1 - bx0) / 2) {
                br = (bx1 - bx0) / 2;
            }
            fdk_surface_fill_rounded_rect(surface, block, br,
                                          fdk__pal_accent());
        }
        return;
    }
    fdk_i32 fill_w =
        (fdk_i32)((fdk_f32)bounds.width * p->fraction + 0.5f);
    if (fill_w > bounds.width) {
        fill_w = bounds.width;
    }
    if (fill_w < 0) {
        fill_w = 0;
    }
    /* A tiny nonzero fraction must still be VISIBLE: without this a
     * 200-px track shows nothing until 2% — "the bar starts empty"
     * reads as "the bar is broken". Two device pixels is the smallest
     * sliver the rounded fill can render honestly. */
    if (p->fraction > 0.0f && fill_w < 2) {
        fill_w = 2;
        if (fill_w > bounds.width) {
            fill_w = bounds.width;
        }
    }
    if (fill_w <= 0) {
        return; /* fraction 0 (or a zero-width track): track only */
    }
    fdk_rect fill = {bounds.x, bounds.y, fill_w, bounds.height};
    fdk_i32 fill_r = r;
    if (fill_r > fill_w / 2) {
        fill_r = fill_w / 2;
    }
    fdk_surface_fill_rounded_rect(surface, fill, fill_r, fdk__pal_accent());
}

/* ---- a11y ---- */

static void progress_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_progress *p = (const fdk_progress *)(const void *)w;
    if (p->indeterminate) {
        /* Busy: no fraction exists to report — say THAT instead of
         * lying with a number. */
        out->has_value = false;
        out->value_text = fdk__strdup("busy");
        return;
    }
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = 1.0;
    out->value_current = (double)p->fraction;
    out->value_text = fdk__a11y_valuef("%.0f%%",
                                       out->value_current * 100.0);
}

static void progress_destroy(fdk_widget *w) {
    fdk_progress *p = progress_of(w);
    if (p->pulse_timer != NULL) {
        fdk_timer_remove(p->pulse_timer);
        p->pulse_timer = NULL;
    }
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
    .measure = progress_measure,
    .arrange = NULL,
    .destroy = progress_destroy,
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
    fdk_progress *p = progress_of(w);
    p->fraction = 0.0f;
    p->indeterminate = false;
    p->pulse_timer = NULL;
    p->pulse_phase = 0.0f;
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
    if (p->indeterminate) {
        /* A fraction implies knowledge: set_fraction leaves activity
         * mode (the GTK contract — the app learned how far along it
         * is). */
        fdk_progress_set_indeterminate(progress, false);
    }
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

void fdk_progress_set_indeterminate(fdk_widget *progress,
                                    bool indeterminate) {
    if (progress == NULL || progress->klass != &fdk_progress_class_def) {
        return;
    }
    fdk_progress *p = progress_of(progress);
    if (p->indeterminate == indeterminate) {
        return;
    }
    p->indeterminate = indeterminate;
    if (indeterminate) {
        p->fraction = 0.0f;
        p->pulse_phase = 0.0f;
        /* Detached trees have no context — the block parks at phase
         * 0 (a static block, honest about being busy without the
         * animation the headless world has no clock for). */
        fdk_context *ctx =
            fdk__window_context(fdk__widget_window_owner(&p->base));
        if (ctx != NULL && p->pulse_timer == NULL) {
            p->pulse_timer = fdk_timer_add(ctx, PROGRESS_PULSE_MS, true,
                                           progress_pulse_tick, p);
        }
        /* A11y: busy state — the value interface stops claiming a
         * fraction it does not have. */
        fdk__a11y_notify(progress, FDK_A11Y_STATE_CHANGED, 0);
    } else {
        if (p->pulse_timer != NULL) {
            fdk_timer_remove(p->pulse_timer);
            p->pulse_timer = NULL;
        }
        fdk__a11y_notify(progress, FDK_A11Y_STATE_CHANGED, 0);
    }
    fdk_widget_invalidate(progress);
}

bool fdk_progress_is_indeterminate(fdk_widget *progress) {
    if (progress == NULL || progress->klass != &fdk_progress_class_def) {
        return false;
    }
    return progress_of(progress)->indeterminate;
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

/* The frame's current title (toolkit-owned; valid until the next
 * set_title/destroy). NULL when the frame has no title (1.3.0
 * getter symmetry with set_title). */
const char *fdk_frame_get_title(fdk_widget *frame) {
    if (frame == NULL || frame->klass != &fdk_frame_class_def) {
        return NULL;
    }
    return frame_of(frame)->title;
}
