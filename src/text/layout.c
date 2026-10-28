/*
 * layout.c — line breaking and ellipsis on top of the shaped
 * placement pipeline (Phase 6 completion; M-2 rework).
 *
 * Both passes ride the shaped result's per-codepoint LOGICAL pen
 * attribution (fdk__shape_range's cp_pen array — the cumulative
 * advance after each codepoint, kerning and ligature effects
 * attributed to the cluster's last cp), so a line's reported
 * advance is by construction the advance its bytes shape to. There
 * is still no second rounding rule anywhere in the text stack: the
 * pen floors per glyph at draw time and rounds once per reported
 * line width, exactly as before.
 *
 * The wrapper is the classic greedy algorithm, chosen deliberately
 * over Knuth-Plass-class optimizers: it is O(n) in codepoints, needs
 * no lookahead buffer, and its failures are local (one long-ish
 * line) rather than global (a reflowed paragraph). Word processing
 * this is not; labels and one-paragraph widget text it is.
 *
 * Shaping context does not cross a line boundary: every emitted line
 * restarts its own shape when it is drawn (the widgets draw line
 * ranges), and the attribution's context is the whole text — the
 * only place they can differ is kerning ACROSS a space at a line
 * break, which is ~0 in real fonts (kern pairs live inside words).
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h" /* the span wrappers free flattened runs */

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
    /* byte bookmarks (emission) and cp-index twins (pen math): the
     * per-codepoint attribution pen is TEXT-absolute, so every
     * line-relative value is a difference against the line-start
     * cp's pen. */
    size_t line_start;
    size_t line_start_k;
    size_t nonspace_end;
    size_t nonspace_end_k;
    fdk_f32 pen_after_nonspace;
    size_t committed_end;
    size_t committed_end_k;
    fdk_f32 pen_after_committed;
    size_t break_at;
} wrap_state;

static void wrap_reset(wrap_state *ws, size_t start, size_t start_k) {
    ws->line_start = start;
    ws->line_start_k = start_k;
    ws->nonspace_end = start;
    ws->nonspace_end_k = start_k;
    ws->pen_after_nonspace = 0.0f;
    ws->committed_end = start;
    ws->committed_end_k = start_k;
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
 * Label's cached-runs paint path calls directly): the text is
 * shaped ONCE (runs included — the shaper segments by style), and
 * the greedy walk advances over the shaped result's codepoints with
 * the LOGICAL pen attribution. Same greedy algorithm as always —
 * one rounding rule, one whitespace definition. */
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

    fdk__shaped *sh = fdk__shape_range(f, utf8, byte_len, 0, byte_len,
                                       runs, n_runs);
    if (sh == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    const size_t n_cp = sh->n_cp;
    const fdk_u32 *cp = sh->cp;
    const size_t *cp_byte = sh->cp_byte;
    const fdk_f32 *cp_pen = sh->cp_pen;

    size_t count = 0;
    bool dropped = false;
    wrap_state ws;
    wrap_reset(&ws, 0, 0);

    size_t k = 0; /* the cp index being consumed */
    while (k < n_cp) {
        fdk_u32 c = cp[k];
        size_t byte = cp_byte[k];
        size_t next_byte =
            k + 1 < n_cp ? cp_byte[k + 1] : byte_len;
        /* LINE-RELATIVE pens: the attribution is text-absolute, the
         * greedy walk restarts at every line. */
        fdk_f32 pen = cp_pen[k + 1] - cp_pen[ws.line_start_k];
        fdk_f32 pen_before = cp_pen[k] - cp_pen[ws.line_start_k];
        bool space = is_wrap_space(c);

        /* Hard break: '\n', '\r', or a "\r\n" pair (one break). The
         * line ends at its last visible glyph (nonspace_end); a hard
         * break ALWAYS emits its line, empty or not ("a\n\nb" is
         * three lines). */
        if (c == 0x0Au || c == 0x0Du) {
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           ws.nonspace_end, ws.pen_after_nonspace)) {
                dropped = true;
            }
            if (c == 0x0Du && next_byte < byte_len &&
                utf8[next_byte] == '\n') {
                next_byte++; /* swallow the \n of a \r\n */
                k++;
            }
            k++; /* past the newline cp itself */
            /* the next line starts at the first cp at/after
             * next_byte (== k unless the bytes and cps disagree) */
            while (k < n_cp && cp_byte[k] < next_byte) {
                k++;
            }
            wrap_reset(&ws, next_byte, k);
            continue;
        }

        /* Word boundary: freeze the completed word, open the next
         * break opportunity just past this space. */
        if (space) {
            ws.committed_end = ws.nonspace_end;
            ws.committed_end_k = ws.nonspace_end_k;
            ws.pen_after_committed = ws.pen_after_nonspace;
            ws.break_at = next_byte;
        }

        /* Overflow? The pen is rounded exactly as a line's advance
         * would be reported. */
        if (pen_round(pen) <= max_width) {
            if (!space) {
                ws.nonspace_end = next_byte;
                ws.nonspace_end_k = k + 1;
                ws.pen_after_nonspace = pen;
            }
            k++;
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
            k++;
            wrap_reset(&ws, next_byte, k);
            continue;
        }

        if (ws.break_at > ws.line_start &&
            ws.committed_end > ws.line_start) {
            /* Word break: the overflowing word moves to the next
             * line WHOLE; this line ends at the last completed word
             * (trailing spaces trimmed by construction). Restart the
             * walk at the word's first cp. */
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           ws.committed_end,
                           ws.pen_after_committed)) {
                dropped = true;
            }
            {
                size_t target = ws.break_at;
                while (k > 0 && cp_byte[k - 1] >= target) {
                    k--;
                }
                wrap_reset(&ws, target, k);
            }
            continue;
        }

        if (ws.break_at > ws.line_start &&
            ws.committed_end == ws.line_start) {
            /* Only spaces (no visible word) before an overflowing
             * word: nothing to emit — the next line starts at the
             * word, the spaces vanish. */
            {
                size_t target = ws.break_at;
                while (k > 0 && cp_byte[k - 1] >= target) {
                    k--;
                }
                wrap_reset(&ws, target, k);
            }
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
            wrap_reset(&ws, byte, k);
            /* k stays: the overflowing cp re-shapes on the new line
             * (fresh pen — it may fit there, or overflow alone and
             * be emitted by the branch below). */
        } else {
            /* The line's first glyph overflows alone: emit it as a
             * one-glyph line and move PAST it. */
            if (!wrap_emit(out_lines, max_lines, &count, ws.line_start,
                           next_byte, pen)) {
                dropped = true;
            }
            k++;
            wrap_reset(&ws, next_byte, k);
        }
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
    fdk__shaped_free(sh);
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

/* The engine both public ellipsize entry points ride. The prefix
 * walk is the shaped result's per-cp logical pen (ligatures and
 * kerning attributed to the cluster's last cp — the prefix budget
 * therefore lands on cluster boundaries, never splitting one); the
 * ellipsis itself is measured at the FONT's own style — it is the
 * toolkit's truncation mark, not part of any run. */
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
    fdk__shaped *sh = fdk__shape_range(f, utf8, byte_len, 0, byte_len,
                                       runs, n_runs);
    if (sh == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t best = 0;
    for (size_t k = 0; k < sh->n_cp; k++) {
        fdk_f32 pen = sh->cp_pen[k + 1];
        size_t next_byte =
            k + 1 < sh->n_cp ? sh->cp_byte[k + 1] : byte_len;
        if (pen_round(pen) > budget) {
            break; /* this glyph pushed past the budget */
        }
        if (!is_wrap_space(sh->cp[k])) {
            best = next_byte;
        }
    }
    fdk__shaped_free(sh);

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
