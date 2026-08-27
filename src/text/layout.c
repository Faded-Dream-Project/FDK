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
                                 fdk_f32 *io_pen, fdk_u32 *out_cp) {
    fdk_u32 cp = 0;
    (void)fdk_text_utf8_next(utf8, len, i, &cp);
    const fdk_glyph *glyph = fdk_text_glyph_for(f, cp);
    int g = glyph->glyph_index;

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

fdk_result fdk_font_break_lines_utf8(const fdk_font *font,
                                     const char *utf8, size_t byte_len,
                                     fdk_i32 max_width,
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

    while (i < byte_len) {
        fdk_u32 cp = 0;
        fdk_f32 pen_before = pen;
        shape_at(f, utf8, byte_len, i, &prev_g, &pen, &cp);
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

/* ---- ellipsis ---- */

/* The ellipsis run: shared macro (see text_internal.h) — the pass
 * that measures it and the paint hook that draws it use one
 * definition. Theming the character (or the policy) belongs to the
 * theme engine, not the text layer. */

fdk_result fdk_font_ellipsize_utf8(const fdk_font *font,
                                   const char *utf8, size_t byte_len,
                                   fdk_i32 max_width,
                                   size_t *out_prefix_bytes,
                                   bool *out_fits) {
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
    if (!fdk_ok(fdk_font_measure_utf8(font, utf8, byte_len, &whole))) {
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
    while (i < byte_len) {
        fdk_u32 cp = 0;
        shape_at(f, utf8, byte_len, i, &prev_g, &pen, &cp);
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
