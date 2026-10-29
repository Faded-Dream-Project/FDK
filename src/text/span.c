/*
 * span.c — the attribute-run layer over the shaping engine (1.4.11,
 * the markup milestone).
 *
 * Rich text in FDK is NOT a second text engine: the shaping walk,
 * the rounding rules, the subpixel phases, and the damage model are
 * text.c's, untouched. What this file adds is the ATTRIBUTE side:
 *
 *   fdk__span_flatten     caller spans (any order, overlapping,
 *                         later-wins) -> one sorted, contiguous,
 *                         maximally-merged run array over the whole
 *                         text, gaps filled at the font's own style
 *   fdk__measure_runs     the runs-level measure twin of
 *                         fdk_font_measure_utf8 (per-run styled
 *                         shaping, kerning resets at run boundaries,
 *                         ONE rounded total)
 *   fdk_surface_draw_spans_utf8
 *                         the paint twin: per-run styled glyphs at
 *                         per-run colors, plus the decoration bars
 *                         (underline / strikethrough) the single-
 *                         style surface never had
 *
 * The line-breaking and ellipsis engines stay in layout.c — they
 * ride the same flattened runs through the shared style cursor, so
 * a wrapped or ellipsized markup label measures, breaks, and paints
 * through ONE arithmetic. There is still no second rounding rule
 * anywhere in the text stack.
 *
 * Decoration geometry (metrics-derived, no theme dependency — text
 * metrics are the font's truth, not a style's): thickness is
 * pixel_size/16 clamped to >= 1; the underline bar sits just below
 * the descent's midpoint; the strikethrough bar crosses at roughly
 * 30% of the ascent (the optical middle of a lowercase x-height).
 * Both bars span the run's full advance, in the run's color.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "render/surface_internal.h"

#include <string.h>

/* ---- flattening ---- */

/* Snaps a byte offset DOWN to a codepoint boundary (the start of the
 * codepoint that contains it). */
static size_t snap_down(const char *s, size_t len, size_t i) {
    if (i >= len) {
        return len;
    }
    while (i > 0 && ((((unsigned char)s[i]) & 0xC0u) == 0x80u)) {
        i--;
    }
    return i;
}

/* Snaps a byte offset UP to the next codepoint boundary (>= i). */
static size_t snap_up(const char *s, size_t len, size_t i) {
    if (i >= len) {
        return len;
    }
    while (i < len && ((((unsigned char)s[i]) & 0xC0u) == 0x80u)) {
        i++;
    }
    return i;
}

static bool run_attrs_equal(const fdk__text_run *a,
                            const fdk__text_run *b) {
    return a->style == b->style && a->color_set == b->color_set &&
           (a->color_set == false ||
            (a->color.r == b->color.r && a->color.g == b->color.g &&
             a->color.b == b->color.b && a->color.a == b->color.a)) &&
           a->underline == b->underline &&
           a->strikethrough == b->strikethrough;
}

fdk_result fdk__span_flatten(const char *utf8, size_t byte_len,
                             const fdk_span *spans, size_t n,
                             unsigned default_style,
                             fdk__text_run **out_runs,
                             size_t *out_run_count) {
    if (out_runs == NULL || out_run_count == NULL ||
        (spans == NULL && n > 0) || (utf8 == NULL && byte_len > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_runs = NULL;
    *out_run_count = 0;
    if (spans == NULL || n == 0 || byte_len == 0) {
        return FDK_OK;
    }

    /* Boundary points: 0, len, and every (clamped, snapped) span
     * edge. At most 2n + 2 of them; a span contributing a duplicate
     * edge is fine (dedupe sorts it out). Starts snap DOWN (outward),
     * ends snap UP; clamping happens first. */
    size_t max_bounds = 2 * n + 2;
    size_t *bounds = fdk_alloc_array(max_bounds, sizeof *bounds);
    if (bounds == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n_bounds = 0;
    bounds[n_bounds++] = 0;
    bounds[n_bounds++] = byte_len;
    for (size_t k = 0; k < n; k++) {
        size_t a = spans[k].byte_start;
        size_t b = spans[k].byte_end;
        if (a > byte_len) {
            a = byte_len;
        }
        if (b > byte_len) {
            b = byte_len;
        }
        if (a > b) {
            /* Inverted range: the contract says half-open [start,
             * end); treat it as empty rather than swapping (the
             * markup parser can never produce one). */
            continue;
        }
        a = snap_down(utf8, byte_len, a);
        b = snap_up(utf8, byte_len, b);
        bounds[n_bounds++] = a;
        bounds[n_bounds++] = b;
    }

    /* Sort + dedupe (insertion sort: n is single-digit in practice,
     * and the array is tiny). */
    for (size_t i = 1; i < n_bounds; i++) {
        size_t v = bounds[i];
        size_t j = i;
        while (j > 0 && bounds[j - 1] > v) {
            bounds[j] = bounds[j - 1];
            j--;
        }
        bounds[j] = v;
    }
    size_t uniq = 0;
    for (size_t i = 0; i < n_bounds; i++) {
        if (i == 0 || bounds[i] != bounds[i - 1]) {
            bounds[uniq++] = bounds[i];
        }
    }

    /* Resolve each segment's effective attributes: the LAST array
     * entry covering the segment wins wholesale (documented). */
    fdk__text_run *runs = fdk_alloc_array(uniq > 0 ? uniq - 1 : 1,
                                          sizeof *runs);
    if (runs == NULL) {
        fdk_free(bounds);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n_runs = 0;
    for (size_t i = 0; i + 1 < uniq; i++) {
        size_t seg_start = bounds[i];
        size_t seg_end = bounds[i + 1];
        const fdk_span *winner = NULL;
        for (size_t k = 0; k < n; k++) {
            if (spans[k].byte_start <= seg_start &&
                spans[k].byte_end >= seg_end &&
                spans[k].byte_start < spans[k].byte_end) {
                winner = &spans[k]; /* later entries overwrite */
            }
        }
        fdk__text_run r;
        r.start = seg_start;
        r.end = seg_end;
        if (winner != NULL) {
            r.style = winner->style;
            r.color_set = winner->color_set;
            r.color = winner->color_set ? winner->color
                                        : (fdk_color){0, 0, 0, 0};
            r.underline = winner->underline;
            r.strikethrough = winner->strikethrough;
        } else {
            /* A gap segment: the font's own style, no color, no
             * decorations — exactly what classic single-style text
             * would paint for those bytes. */
            r.style = default_style;
            r.color_set = false;
            r.color = (fdk_color){0, 0, 0, 0};
            r.underline = false;
            r.strikethrough = false;
        }
        /* Merge with the previous run when nothing differs. */
        if (n_runs > 0 && run_attrs_equal(&runs[n_runs - 1], &r)) {
            runs[n_runs - 1].end = r.end;
        } else {
            runs[n_runs++] = r;
        }
    }

    fdk_free(bounds);
    *out_runs = runs;
    *out_run_count = n_runs;
    return FDK_OK;
}

unsigned fdk__run_style_at(const fdk__text_run *runs, size_t n,
                           size_t byte_i, unsigned fallback) {
    for (size_t k = 0; k < n; k++) {
        if (byte_i >= runs[k].start && byte_i < runs[k].end) {
            /* Style 0 in a SPANNED run is absolute normal; the
             * fallback applies only to bytes no run covers (gaps
             * between spans in a sparse run array). */
            return runs[k].style;
        }
    }
    return fallback;
}

/* ---- runs-level measure ---- */

fdk_result fdk__measure_runs(const fdk_font *font, const char *utf8,
                             size_t byte_len,
                             const fdk__text_run *runs, size_t n_runs,
                             size_t limit, fdk_text_metrics *out) {
    fdk_font *f = fdk_text_font_mutable(font);
    if (f == NULL || utf8 == NULL || out == NULL || limit > byte_len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    if (runs == NULL || n_runs == 0) {
        /* No attributes at all: byte-for-byte the classic walk over
         * [0, limit). */
        return fdk_font_measure_utf8(font, utf8, limit, out);
    }

    /* M-2: the shaped placement pipeline under the flattened runs —
     * per-run styles, kerning context resetting at run boundaries,
     * the full complex-script behavior for whatever the runs cover. */
    fdk__shaped *s = fdk__shape_range(f, utf8, byte_len, 0, limit, runs,
                                      n_runs);
    if (s == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_text_metrics m = {0, 0, 0};
    m.advance_width = (fdk_i32)(s->total + 0.5f);
    if (s->has_ink) {
        m.ink_top = (fdk_i32)s->ink_top;
        m.ink_bottom = (fdk_i32)s->ink_bottom;
    }
    *out = m;
    fdk__shaped_free(s);
    return FDK_OK;
}

/* ---- public span entry points ---- */

fdk_result fdk_font_measure_spans_utf8(const fdk_font *font,
                                       const char *utf8, size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       fdk_text_metrics *out) {
    if (font == NULL || utf8 == NULL || out == NULL ||
        (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (spans == NULL || span_count == 0) {
        return fdk_font_measure_utf8(font, utf8, byte_len, out);
    }

    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__span_flatten(utf8, byte_len, spans, span_count,
                                     fdk_font_get_style(font), &runs,
                                     &n_runs);
    if (fdk_ok(r)) {
        r = fdk__measure_runs(font, utf8, byte_len, runs, n_runs,
                              byte_len, out);
    }
    fdk_free(runs);
    return r;
}

/* Decoration geometry (see the file header). */

static fdk_i32 deco_thickness(fdk_i32 pixel_size) {
    fdk_i32 t = pixel_size / 16;
    return t < 1 ? 1 : t;
}

static fdk_i32 underline_y(const fdk_font *font, fdk_i32 baseline) {
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_i32 off = fm.descent / 2;
    if (off < 1) {
        off = 1;
    }
    return baseline + off;
}

static fdk_i32 strike_y(const fdk_font *font, fdk_i32 baseline) {
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    return baseline - (fm.ascent * 3) / 10;
}

/* ---- runs-level paint (the engine under the public span draw) ---- */

/* The logical pen at a byte boundary (the shaped result's per-cp
 * attribution): cp_pen[k] when the byte is cp k's own boundary, the
 * cp's END pen when it lands mid-codepoint, the total past the range.
 * The decoration bars' extents come from here. */
static fdk_f32 shaped_pen_at_byte(const fdk__shaped *s, size_t byte) {
    if (s == NULL || s->cp_pen == NULL || s->n_cp == 0) {
        return 0.0f;
    }
    if (byte >= s->range_end) {
        return s->cp_pen[s->n_cp];
    }
    for (size_t k = 0; k < s->n_cp; k++) {
        size_t next =
            k + 1 < s->n_cp ? s->cp_byte[k + 1] : s->range_end;
        if (byte >= s->cp_byte[k] && byte < next) {
            return byte == s->cp_byte[k] ? s->cp_pen[k]
                                         : s->cp_pen[k + 1];
        }
    }
    return 0.0f;
}

fdk_result fdk__draw_runs(fdk_surface *surface, fdk_font *font,
                          const char *utf8, size_t byte_len,
                          const fdk__text_run *runs, size_t n_runs,
                          size_t off, size_t count,
                          fdk_i32 pen_x, fdk_i32 baseline_y,
                          fdk_color base_color) {
    if (surface == NULL || font == NULL || utf8 == NULL ||
        off > byte_len || count > byte_len - off) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return FDK_OK;
    }
    if (runs == NULL || n_runs == 0) {
        return fdk_surface_draw_utf8(surface, font, utf8 + off, count,
                                     pen_x, baseline_y, base_color);
    }

    /* The paint walk (M-2): the shaped placements for [off,
     * off+count) under the runs — per-run styles and colors, the
     * decoration bars over each run's visible extent. Damage: the
     * union of every glyph box in the range, one rect, clipped
     * exactly like text.c does it (blend_mask's reachable pixels);
     * decoration bars ride fill_rect's own clip+damage bookkeeping. */
    fdk_i64 bx0 = (fdk_i64)surface->clip_x0;
    fdk_i64 by0 = (fdk_i64)surface->clip_y0;
    fdk_i64 bx1 = (fdk_i64)surface->clip_x1;
    fdk_i64 by1 = (fdk_i64)surface->clip_y1;

    fdk__shaped *s = fdk__shape_range(font, utf8, byte_len, off, count,
                                      runs, n_runs);
    if (s == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    const size_t end = off + count;
    fdk_i64 ux0 = 0, uy0 = 0, ux1 = -1, uy1 = -1;
    int any = 0;
    fdk_i32 thick = deco_thickness(font->pixel_size);
    fdk_i32 uly = underline_y(font, baseline_y);
    fdk_i32 sty = strike_y(font, baseline_y);

    for (size_t i = 0; i < s->n_places; i++) {
        const fdk__placement *p = &s->places[i];
        const fdk_glyph *glyph = p->raster;
        if (glyph == NULL || glyph->bits == NULL || glyph->w <= 0 ||
            glyph->h <= 0) {
            continue;
        }
        /* The run's color: the placement's cluster byte selects the
         * run (the byte's effective attributes), exactly as the
         * per-run walk did. */
        fdk_color color = base_color;
        for (size_t k = 0; k < n_runs; k++) {
            if (p->cluster >= runs[k].start && p->cluster < runs[k].end) {
                color = runs[k].color_set ? runs[k].color : base_color;
                break;
            }
        }
        fdk_rect dst = {
            pen_x + p->x + glyph->xoff,
            baseline_y + glyph->yoff + p->y_shift,
            glyph->w,
            glyph->h,
        };
        if (glyph->color) {
            /* C-3: color glyphs composite their own RGBA — the run's
             * color/decoration logic still ran for the bar pass, but
             * the bitmap itself needs no tint. */
            fdk_surface_blend_rgba(surface, dst, glyph->bits,
                                   glyph->w * 4);
        } else {
            fdk_surface_blend_mask(surface, dst, glyph->bits,
                                   glyph->w, color);
        }
        if (!any) {
            ux0 = dst.x;
            uy0 = dst.y;
            ux1 = (fdk_i64)dst.x + dst.width;
            uy1 = (fdk_i64)dst.y + dst.height;
            any = 1;
        } else {
            if (dst.x < ux0) {
                ux0 = dst.x;
            }
            if (dst.y < uy0) {
                uy0 = dst.y;
            }
            if ((fdk_i64)dst.x + dst.width > ux1) {
                ux1 = (fdk_i64)dst.x + dst.width;
            }
            if ((fdk_i64)dst.y + dst.height > uy1) {
                uy1 = (fdk_i64)dst.y + dst.height;
            }
        }
    }

    /* The decoration bars: per run, over the run's VISIBLE extent
     * (a run half outside the range bars only its inside half). The
     * run's visible advance is its cp_pen span — the logical
     * attribution the shaped result carries. */
    for (size_t k = 0; k < n_runs; k++) {
        size_t r_start = runs[k].start > off ? runs[k].start : off;
        size_t r_end = runs[k].end < end ? runs[k].end : end;
        if (r_start >= r_end) {
            continue;
        }
        if (!runs[k].underline && !runs[k].strikethrough) {
            continue;
        }
        fdk_color color = runs[k].color_set ? runs[k].color : base_color;
        /* the run's [r_start, r_end) advance from the logical pen */
        fdk_f32 x0f = shaped_pen_at_byte(s, r_start);
        fdk_f32 x1f = shaped_pen_at_byte(s, r_end);
        fdk_i32 x = pen_x + (fdk_i32)(x0f + 0.5f);
        fdk_i32 w = (fdk_i32)(x1f + 0.5f) - (fdk_i32)(x0f + 0.5f);
        if (runs[k].underline && w > 0) {
            fdk_surface_fill_rect(surface, (fdk_rect){x, uly, w, thick},
                                  color);
        }
        if (runs[k].strikethrough && w > 0) {
            fdk_surface_fill_rect(surface, (fdk_rect){x, sty, w, thick},
                                  color);
        }
    }
    fdk__shaped_free(s);

    if (any) {
        fdk_i64 dx0 = ux0 > bx0 ? ux0 : bx0;
        fdk_i64 dy0 = uy0 > by0 ? uy0 : by0;
        fdk_i64 dx1 = ux1 < bx1 ? ux1 : bx1;
        fdk_i64 dy1 = uy1 < by1 ? uy1 : by1;
        if (dx1 > dx0 && dy1 > dy0 &&
            dx0 <= (fdk_i64)INT32_MAX && dx1 >= (fdk_i64)INT32_MIN &&
            dy0 <= (fdk_i64)INT32_MAX && dy1 >= (fdk_i64)INT32_MIN) {
            fdk_surface_invalidate(surface,
                                   (fdk_rect){
                                       (fdk_i32)dx0, (fdk_i32)dy0,
                                       (fdk_i32)(dx1 - dx0),
                                       (fdk_i32)(dy1 - dy0),
                                   });
        }
    }
    return FDK_OK;
}

fdk_result fdk_surface_draw_spans_utf8(fdk_surface *surface,
                                       fdk_font *font, const char *utf8,
                                       size_t byte_len,
                                       const fdk_span *spans,
                                       size_t span_count,
                                       fdk_i32 pen_x, fdk_i32 baseline_y,
                                       fdk_color base_color) {
    if (surface == NULL || font == NULL || utf8 == NULL ||
        (spans == NULL && span_count > 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (spans == NULL || span_count == 0) {
        return fdk_surface_draw_utf8(surface, font, utf8, byte_len,
                                     pen_x, baseline_y, base_color);
    }

    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__span_flatten(utf8, byte_len, spans, span_count,
                                     fdk_font_get_style(font), &runs,
                                     &n_runs);
    if (!fdk_ok(r)) {
        return r;
    }
    r = fdk__draw_runs(surface, font, utf8, byte_len, runs, n_runs, 0,
                       byte_len, pen_x, baseline_y, base_color);
    fdk_free(runs);
    return r;
}
