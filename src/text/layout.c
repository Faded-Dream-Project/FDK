/*
 * layout.c — line breaking and ellipsis on top of the run-level text
 * API (Phase 6 completion).
 *
 * Both passes ride the SAME shaping arithmetic as measure and draw
 * (decode -> kern against previous glyph -> advance), so a line's
 * reported advance is by construction the width its bytes paint at.
 * There is no second rounding rule anywhere in the text stack.
 *
 * The wrapper is the classic greedy algorithm, chosen deliberately
 * over Knuth-Plass-class optimizers: it is O(n) in shaped glyphs,
 * needs no lookahead buffer, and its failures are local (one long-ish
 * line) rather than global (a reflowed paragraph). Word processing
 * this is not; labels and one-paragraph widget text it is.
 *
 * Kerning never crosses a line boundary: every emitted line restarts
 * the walk at its own first byte with a fresh pen and a previous
 * glyph of "none" — exactly what fdk_surface_draw_utf8() does for
 * those bytes, so agreement is structural, not incidental.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h" /* the span wrappers free flattened runs */

/* stbtt pair kerning is called directly below; everything else rides
 * the cached-glyph walk from text.c. */

/* ---- shared shaped-glyph step ---- */

/* One shaped glyph, decoded at byte i (NOT consumed — the caller
 * advances i itself). Mirrors fdk_text_shape_step's arithmetic
 * exactly (same decode, same pair kerning, same advance) but also
 * reports the codepoint, which the layout passes need for their
 * whitespace and hard-break rules. Returns the cached glyph (never
 * NULL for a non-NULL font). */
static const fdk_glyph *shape_at(fdk_font *f, const char *utf8,
                                 size_t len, size_t i, int *io_prev_g,
                                 fdk_f32 *io_pen, fdk_u32 *out_cp,
                                 unsigned style_flags) {
    fdk_u32 cp = 0;
    (void)fdk_text_utf8_next(utf8, len, i, &cp);
    const fdk_glyph *glyph =
        fdk_text_glyph_for_styled(f, cp, style_flags);
    /* The cache key packs (glyph index, style variant, subpixel
     * phase); kerning wants the raw glyph id, so unpack it the same
     * way text.c does. */
    int g = fdk_text_key_glyph(glyph->key);

    if (*io_prev_g >= 0) {
        int kern = stbtt_GetGlyphKernAdvance(&f->info, *io_prev_g, g);
        if (kern != 0) {
            *io_pen += (fdk_f32)kern * f->scale;
        }
    }
    *io_pen += glyph->advance;
    *io_prev_g = g;
    *out_cp = cp;
    return glyph;
}

static bool is_wrap_space(fdk_u32 cp) {
    return cp == 0x20u || cp == 0x09u; /* space, tab */
}

/* A line's reported advance uses the same rounding as measure. */
static fdk_i32 pen_round(fdk_f32 pen) {
    return (fdk_i32)(pen + 0.5f);
}

/* ---- span-aware shaping cursor (1.4.11) ---------------------------
 *
 * The wrap/ellipsize engines are attribute-blind at their core; the
 * SPAN variants of both passes (the Label's markup modes) feed them
 * the effective style per byte through this cursor. Kerning resets
 * at style-run boundaries exactly as it does at line boundaries —
 * a bold word's last glyph never kerns against the regular text
 * after it (the two rasterizations are different slots; pretending
 * kerning applies would drift measure from paint). */
typedef struct style_cursor {
    const fdk__text_run *runs;
    size_t n_runs;
    unsigned prev_style; /* style of the last SHAPED glyph         */
    bool have_prev;      /* false at line starts / cursor resets    */
} style_cursor;

static void style_cursor_reset(style_cursor *sc) {
    sc->have_prev = false;
}

/* Shapes the glyph at byte i under the cursor's run style, resetting
 * the caller's previous-glyph id when the style changed (kerning
 * never crosses a style boundary). Returns the shaped glyph. */
static const fdk_glyph *shape_at_cursor(fdk_font *f, const char *utf8,
                                        size_t len, size_t i,
                                        style_cursor *sc, int *io_prev_g,
                                        fdk_f32 *io_pen, fdk_u32 *out_cp) {
    unsigned style = fdk__run_style_at(sc->runs, sc->n_runs, i,
                                       f->style);
    if (sc->have_prev && style != sc->prev_style) {
        *io_prev_g = -1; /* style boundary: no kerning across it */
    }
    const fdk_glyph *g = shape_at(f, utf8, len, i, io_prev_g, io_pen,
                                  out_cp, style);
    sc->prev_style = style;
    sc->have_prev = true;
    return g;
}

/* ---- line breaking ---- */

/* Bookmarks for the line under construction:
 *
 *   line_start         first byte of the current line
 *   nonspace_end       byte AFTER the last non-space glyph SHAPED SO
 *                      FAR (spaces never extend it), with
 *                      pen_after_nonspace the pen at that point
 *   committed_end      byte after the last COMPLETE word — frozen
 *                      each time a space run begins — with
 *                      pen_after_committed the pen there. When a word
 *                      overflows, the line ends here and the word
 *                      moves to the next line whole.
 *   break_at           where the next line may start: just past the
 *                      last space run (= the current word's first
 *                      byte while a word is being shaped) */
typedef struct wrap_state {
    size_t line_start;
    size_t nonspace_end;
    fdk_f32 pen_after_nonspace;
    size_t committed_end;
    fdk_f32 pen_after_committed;
    size_t break_at;
} wrap_state;

static void wrap_reset(wrap_state *ws, size_t start) {
    ws->line_start = start;
    ws->nonspace_end = start;
    ws->pen_after_nonspace = 0.0f;
    ws->committed_end = start;
    ws->pen_after_committed = 0.0f;
    ws->break_at = start;
}

/* Appends the line [line_start, end) at pen `advance`. Returns false
 * only when a line was DROPPED for capacity (max_lines > 0 and the
 * array is full) — the caller flags truncation. Count-only calls
 * (max_lines == 0) never drop. */
static bool wrap_emit(fdk_text_line *out_lines, size_t max_lines,
                      size_t *io_count, size_t line_start, size_t end,
                      fdk_f32 advance) {
    if (max_lines == 0) {
        *io_count += 1; /* counting only */
        return true;
    }
    if (*io_count >= max_lines) {
        return false;
    }
    fdk_text_line *l = &out_lines[*io_count];
    l->byte_offset = line_start;
    l->byte_len = end - line_start;
    l->advance_width = pen_round(advance);
    *io_count += 1;
    return true;
}

/* The engine the public break entry points ride (and the markup
 * Label's cached-runs paint path calls directly): runs == NULL (or
 * n_runs == 0) is the classic single-style walk; otherwise the style
 * cursor feeds each glyph its run's style and kerning resets at
 * style boundaries. Same greedy algorithm either way — one rounding
 * rule, one whitespace definition. */
fdk_result fdk__break_lines_runs(const fdk_font *font,
                                  const char *utf8, size_t byte_len,
                                  const fdk__text_run *runs,
                                  size_t n_runs, fdk_i32 max_width,
                                  fdk_text_line *out_lines,
                                  size_t max_lines,
                                  size_t *out_line_count,
                                  bool *out_truncated) {
    fdk_font *f = fdk_text_font_mutable(font); /* cache-warming, like measure */
    if (f == NULL || utf8 == NULL || out_line_count == NULL ||
        (out_lines == NULL && max_lines > 0) || max_width < 1) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (out_truncated != NULL) {
        *out_truncated = false;
    }
    *out_line_count = 0;
    if (byte_len == 0) {
        return FDK_OK;
    }

    size_t count = 0;
    bool dropped = false;
    wrap_state ws;
    wrap_reset(&ws, 0);

    size_t i = 0;
    int prev_g = -1;
    fdk_f32 pen = 0.0f;
    style_cursor sc = {runs, n_runs, 0, false};

    while (i < byte_len) {
        fdk_u32 cp = 0;
        fdk_f32 pen_before = pen;
        shape_at_cursor(f, utf8, byte_len, i, &sc, &prev_g, &pen, &cp);
        size_t next = i + (size_t)fdk_text_utf8_next(utf8, byte_len, i,
                                                     &cp);
        /* `next` recomputes the consumed length; decoding twice is
         * intentional: shape_at keeps its own signature tight and
         * the decoder is a pure table walk over <= 4 bytes. */
        bool space = is_wrap_space(cp);

        /* Hard break: '\n', '\r', or a "\r\n" pair (one break). The
         * line ends at its last visible glyph (nonspace_end); a hard
         * break ALWAYS emits its line, empty or not ("a\n\nb" is
         * three lines). */
        if (cp == 0x0Au || cp == 0x0Du) {
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           ws.nonspace_end, ws.pen_after_nonspace)) {
                dropped = true;
            }
            if (cp == 0x0Du && next < byte_len && utf8[next] == '\n') {
                next++; /* swallow the \n of a \r\n */
            }
            wrap_reset(&ws, next);
            i = next;
            pen = 0.0f;
            prev_g = -1;
            style_cursor_reset(&sc);
            continue;
        }

        /* Word boundary: freeze the completed word, open the next
         * break opportunity just past this space. */
        if (space) {
            ws.committed_end = ws.nonspace_end;
            ws.pen_after_committed = ws.pen_after_nonspace;
            ws.break_at = next;
        }

        /* Overflow? The pen is rounded exactly as a line's advance
         * would be reported. */
        if (pen_round(pen) <= max_width) {
            if (!space) {
                ws.nonspace_end = next;
                ws.pen_after_nonspace = pen;
            }
            i = next;
            continue;
        }

        if (space) {
            /* A trailing space ran past the edge. The visible line
             * content already fits; emit it (or drop a spaces-only
             * line) and continue after this space. */
            if (ws.nonspace_end > ws.line_start) {
                if (!wrap_emit(out_lines, max_lines, &count,
                               ws.line_start, ws.nonspace_end,
                               ws.pen_after_nonspace)) {
                    dropped = true;
                }
            }
            wrap_reset(&ws, next);
            i = next;
            pen = 0.0f;
            prev_g = -1;
            style_cursor_reset(&sc);
            continue;
        }

        if (ws.break_at > ws.line_start &&
            ws.committed_end > ws.line_start) {
            /* Word break: the overflowing word moves to the next
             * line WHOLE; this line ends at the last completed word
             * (trailing spaces trimmed by construction). Restart the
             * walk at the word's first byte with a fresh pen. */
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           ws.committed_end,
                           ws.pen_after_committed)) {
                dropped = true;
            }
            wrap_reset(&ws, ws.break_at);
            i = ws.break_at;
            pen = 0.0f;
            prev_g = -1;
            style_cursor_reset(&sc);
            continue;
        }

        if (ws.break_at > ws.line_start &&
            ws.committed_end == ws.line_start) {
            /* Only spaces (no visible word) before an overflowing
             * word: nothing to emit — the next line starts at the
             * word, the spaces vanish. */
            wrap_reset(&ws, ws.break_at);
            i = ws.break_at;
            pen = 0.0f;
            prev_g = -1;
            style_cursor_reset(&sc);
            continue;
        }

        /* Single word wider than max_width: mid-word break. Glyphs
         * that already fit (if any) form the line; the overflowing
         * glyph re-shapes as the next line's first byte. A word whose
         * very first glyph overflows still emits that glyph alone —
         * guaranteed progress, no infinite loop. */
        if (ws.nonspace_end > ws.line_start) {
            /* pen_before is the pen at the boundary before this
             * glyph — exactly the fitted content's advance. */
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           ws.nonspace_end, pen_before)) {
                dropped = true;
            }
            wrap_reset(&ws, i);
            /* i stays: the overflowing glyph re-shapes on the new
             * line (fresh pen — it may fit there, or overflow alone
             * and be emitted by the branch below). */
        } else {
            /* The line's first glyph overflows alone: emit it as a
             * one-glyph line and move PAST it. */
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           next, pen)) {
                dropped = true;
            }
            wrap_reset(&ws, next);
            i = next;
        }
        pen = 0.0f;
        prev_g = -1;
        style_cursor_reset(&sc);
    }

    /* Final line: only if visible bytes remain — a trailing pure
     * space run produces no line. */
    if (ws.nonspace_end > ws.line_start) {
        if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                       ws.nonspace_end, ws.pen_after_nonspace)) {
            dropped = true;
        }
    }

    *out_line_count = count;
    if (out_truncated != NULL && max_lines > 0) {
        *out_truncated = dropped;
    }
    return FDK_OK;
}

fdk_result fdk_font_break_lines_utf8(const fdk_font *font,
                                     const char *utf8, size_t byte_len,
                                     fdk_i32 max_width,
                                     fdk_text_line *out_lines,
                                     size_t max_lines,
                                     size_t *out_line_count,
                                     bool *out_truncated) {
    return fdk__break_lines_runs(font, utf8, byte_len, NULL, 0,
                                  max_width, out_lines, max_lines,
                                  out_line_count, out_truncated);
}

fdk_result fdk_font_break_lines_spans_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count, fdk_i32 max_width,
    fdk_text_line *out_lines, size_t max_lines,
    size_t *out_line_count, bool *out_truncated) {
    if (spans == NULL && span_count > 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (spans == NULL || span_count == 0) {
        /* No attributes: exactly the classic walk (no flatten cost). */
        return fdk__break_lines_runs(font, utf8, byte_len, NULL, 0,
                                      max_width, out_lines, max_lines,
                                      out_line_count, out_truncated);
    }
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__span_flatten(utf8, byte_len, spans, span_count,
                                     fdk_font_get_style(font), &runs,
                                     &n_runs);
    if (!fdk_ok(r)) {
        return r;
    }
    r = fdk__break_lines_runs(font, utf8, byte_len, runs, n_runs,
                              max_width, out_lines, max_lines,
                              out_line_count, out_truncated);
    fdk_free(runs);
    return r;
}

/* ---- ellipsis ---- */

/* The ellipsis run: shared macro (see text_internal.h) — the pass
 * that measures it and the paint hook that draws it use one
 * definition. Theming the character (or the policy) belongs to the
 * theme engine, not the text layer. */

/* The engine both public ellipsize entry points ride. The span walk
 * shapes each glyph at its run's style (kerning resets at style
 * boundaries through the shared cursor); the ellipsis itself is
 * measured at the FONT's own style — it is the toolkit's truncation
 * mark, not part of any run. */
fdk_result fdk__ellipsize_runs(const fdk_font *font,
                                const char *utf8, size_t byte_len,
                                const fdk__text_run *runs, size_t n_runs,
                                fdk_i32 max_width,
                                size_t *out_prefix_bytes, bool *out_fits) {
    fdk_font *f = fdk_text_font_mutable(font); /* cache-warming, like measure */
    if (f == NULL || utf8 == NULL || out_prefix_bytes == NULL ||
        max_width < 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (out_fits != NULL) {
        *out_fits = true;
    }
    *out_prefix_bytes = byte_len;
    if (byte_len == 0) {
        return FDK_OK; /* empty text always fits */
    }

    fdk_text_metrics whole;
    fdk_result mr = (runs != NULL)
        ? fdk__measure_runs(font, utf8, byte_len, runs, n_runs,
                            byte_len, &whole)
        : fdk_font_measure_utf8(font, utf8, byte_len, &whole);
    if (!fdk_ok(mr)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (whole.advance_width <= max_width) {
        return FDK_OK; /* fits: prefix = everything */
    }
    if (out_fits != NULL) {
        *out_fits = false;
    }

    fdk_text_metrics ell;
    if (!fdk_ok(fdk_font_measure_utf8(font, FDK_TEXT_ELLIPSIS_UTF8,
                                      FDK_TEXT_ELLIPSIS_BYTES, &ell))) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Even the ellipsis alone does not fit: prefix 0. The caller
     * draws the ellipsis anyway and lets the clip stack hide the
     * overflow — consistent, predictable, and never a crash. */
    if (ell.advance_width > max_width) {
        *out_prefix_bytes = 0;
        return FDK_OK;
    }
    fdk_i32 budget = max_width - ell.advance_width;

    /* Longest codepoint-boundary prefix whose rounded advance stays
     * within the budget, trailing spaces trimmed: one pass, freezing
     * `best` only on non-space glyphs. `best` 0 means not even one
     * visible glyph fits beside the ellipsis. */
    size_t best = 0;
    size_t i = 0;
    int prev_g = -1;
    fdk_f32 pen = 0.0f;
    style_cursor sc = {runs, n_runs, 0, false};
    while (i < byte_len) {
        fdk_u32 cp = 0;
        shape_at_cursor(f, utf8, byte_len, i, &sc, &prev_g, &pen, &cp);
        size_t next = i + (size_t)fdk_text_utf8_next(utf8, byte_len,
                                                     i, &cp);
        if (pen_round(pen) > budget) {
            break; /* this glyph pushed past the budget */
        }
        if (!is_wrap_space(cp)) {
            best = next;
        }
        i = next;
    }

    *out_prefix_bytes = best;
    return FDK_OK;
}

fdk_result fdk_font_ellipsize_utf8(const fdk_font *font,
                                   const char *utf8, size_t byte_len,
                                   fdk_i32 max_width,
                                   size_t *out_prefix_bytes,
                                   bool *out_fits) {
    return fdk__ellipsize_runs(font, utf8, byte_len, NULL, 0, max_width,
                               out_prefix_bytes, out_fits);
}

fdk_result fdk_font_ellipsize_spans_utf8(
    const fdk_font *font, const char *utf8, size_t byte_len,
    const fdk_span *spans, size_t span_count, fdk_i32 max_width,
    size_t *out_prefix_bytes, bool *out_fits) {
    if (spans == NULL && span_count > 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (spans == NULL || span_count == 0) {
        return fdk__ellipsize_runs(font, utf8, byte_len, NULL, 0,
                                   max_width, out_prefix_bytes,
                                   out_fits);
    }
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    fdk_result r = fdk__span_flatten(utf8, byte_len, spans, span_count,
                                     fdk_font_get_style(font), &runs,
                                     &n_runs);
    if (!fdk_ok(r)) {
        return r;
    }
    r = fdk__ellipsize_runs(font, utf8, byte_len, runs, n_runs,
                            max_width, out_prefix_bytes, out_fits);
    fdk_free(runs);
    return r;
}
