#define FDK_LOG_TAG "widgets"

/*
 * textview.c — the multi-line text editor (1.4.8)
 *
 * The last widget-family hole: a scrolling, wrapping text document
 * with the Entry's whole editing discipline. One UTF-8 buffer (the
 * Entry's splice/undo/clipboard contracts, nearly verbatim); visual
 * lines are DERIVED — fdk_font_break_lines_utf8 over the buffer at
 * the current wrap width, cached and invalidated on every text,
 * width, or wrap-mode change (the label's line-cache shape).
 *
 * The second dimension is the actual work:
 *
 *   - caret/selection stay BYTE offsets (the Entry's model); the
 *     visual mapping (line from y, offset from x) runs through the
 *     cached lines, and every keyboard motion that used to be
 *     1-D grows its 2-D twin: Up/Down step VISUAL lines keeping
 *     the caret's x (the classic goal-column), Home/End the visual
 *     line's ends, PageUp/PageDown the viewport's pages, Enter
 *     inserts '\n' (a hard break — the break engine already honors
 *     them), Backspace/Delete join lines (a plain splice).
 *   - triple-click selects the PARAGRAPH (the logical line, wraps
 *     included) where the Entry selected the whole field.
 *   - scroll-to-caret runs on BOTH axes.
 *
 * Topology: textview (focusable) -> scrollview -> page (the
 * internal class "textview-page" that paints the owner's lines,
 * selection rects, caret, and preedit — the IconView-cell owner
 * reach-through). The view paints the entry-style rounded field +
 * focus ring itself (the scrollview is transparent). The scrollview
 * self-sync rule (1.4.7's lesson, the List's since 1.2) applies:
 * relayout re-syncs the internals at the view's CURRENT bounds.
 *
 * The wrap cache is O(document) per rebreak and rebreaks on every
 * edit — v1 honesty (the label re-breaks per paint today). A
 * line-indexed document model is the recorded future optimization
 * once real editors push six-figure documents through it.
 */

#include "widgets_internal.h"
#include "text/text_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "core/undo_internal.h"

#include "fdk/fdk_clipboard.h"
#include "fdk/fdk_undo.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define TV_MAX_TEXT (256 * 1024) /* the editable cap, bytes        */
#define TV_DBLCLICK_MS 400       /* the Entry's double-click window*/
#define TV_DBLCLICK_SLOP 4
#define TV_MIN_W 80
#define TV_MIN_H 40

typedef struct fdk_textview {
    fdk_widget base;
    fdk_font *font;        /* borrowed */
    char *text;            /* owned, NUL-terminated UTF-8, never NULL */
    size_t len;            /* byte length of text */
    size_t cap;            /* allocation size (>= len + 1)           */
    size_t caret;          /* byte offset, codepoint boundary        */
    size_t anchor;         /* selection anchor (== caret: no sel)     */
    char *preedit;         /* owned, NULL when no preedit             */
    fdk_textview_wrap wrap;
    bool read_only;
    fdk_entry_changed_fn on_changed;
    void *on_changed_data;
    /* ---- the wrap cache ----
     * Rebuilt whenever text/wrap-mode/wrap-width changes. Lines are
     * the break engine's output over the WHOLE buffer; each carries
     * the doc-relative byte range and its own advance width. */
    fdk_text_line *lines;
    size_t line_count;
    size_t line_cap;
    fdk_i32 wrap_width;    /* the width the cache was broken at  */
    fdk_i32 doc_w;         /* the widest line + insets           */
    fdk_i32 doc_h;         /* line_count * line_h + insets       */
    fdk_i32 line_h;        /* ascent + descent + leading         */
    fdk_i32 pad;           /* the themed inset, both axes        */
    /* Internals: scrollview child -> page. */
    fdk_widget *scroll;
    fdk_widget *page;
    /* Pointer drag selecting; click-count machinery (the Entry's). */
    int selecting;
    fdk_i64 last_click_ms;
    fdk_f32 last_click_x, last_click_y;
    int click_count;
    /* The goal column: Up/Down through shorter lines remember where
     * the caret WANTED to be (GTK's rule — the classic editor feel). */
    fdk_i32 goal_x;
    bool have_goal;
    /* Caret blink (the Entry's timer discipline). */
    fdk_timer *blink_timer;
    bool caret_on;
    /* Undo (the Entry's closures, byte-for-byte). */
    fdk_undo_stack *undo;
    bool undo_applying;
    bool undo_group_pending;
    /* PRIMARY ownership politeness (the Entry's flag). */
    bool primary_pushed;
} fdk_textview;

static fdk_textview *tv_of(fdk_widget *w) {
    return (fdk_textview *)(void *)w;
}

extern const fdk_widget_class fdk_textview_class_def;
static const fdk_widget_class fdk_tv_page_class_def;

static fdk_i64 tv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

/* ---- UTF-8 helpers (the Entry's, verbatim) ---- */

static bool tv_utf8_is_cont(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

static size_t tv_utf8_prev(const char *s, size_t i) {
    size_t p = i - 1;
    while (p > 0 && tv_utf8_is_cont((unsigned char)s[p])) {
        p--;
    }
    return p;
}

/* The logical line (paragraph) containing byte offset i: the
 * [start, end) between the surrounding newlines. Wraps included —
 * a triple-click selects this whole range. */
static void tv_paragraph_range(const char *s, size_t len, size_t i,
                               size_t *out_start, size_t *out_end) {
    size_t start = i;
    while (start > 0 && s[start - 1] != '\n') {
        start = tv_utf8_prev(s, start);
    }
    size_t end = i;
    while (end < len && s[end] != '\n') {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(s, len, end, &cp);
        end += (size_t)(n > 0 ? n : 1);
    }
    if (end < len) {
        end++; /* the newline belongs to the paragraph */
    }
    *out_start = start;
    *out_end = end;
}

/* The word at byte offset i (the Entry's word_range discipline:
 * runs of word characters vs runs of separators). */
static void tv_word_range(const char *s, size_t len, size_t i,
                          size_t *out_start, size_t *out_end) {
    /* Codepoint classifier: letters/digits/underscore are "word",
     * everything else (including newline) is separator. */
    bool is_word_cp(fdk_u32 cp) {
        return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
               (cp >= '0' && cp <= '9') || cp == '_' ||
               (cp >= 0x80u && cp != 0x2026u /* treat text as word */);
    }
    /* Snap i to a codepoint boundary. */
    while (i < len && tv_utf8_is_cont((unsigned char)s[i])) {
        i++;
    }
    if (i >= len) {
        *out_start = *out_end = len;
        return;
    }
    fdk_u32 cp = 0;
    int n = fdk_text_utf8_next(s, len, i, &cp);
    bool word = is_word_cp(cp);
    size_t start = i;
    while (start > 0) {
        size_t p = tv_utf8_prev(s, start);
        fdk_u32 pc = 0;
        int pn = fdk_text_utf8_next(s, len, p, &pc);
        if (pn <= 0 || is_word_cp(pc) != word) {
            break;
        }
        start = p;
    }
    size_t end = i + (size_t)(n > 0 ? n : 1);
    while (end < len) {
        fdk_u32 nc = 0;
        int nn = fdk_text_utf8_next(s, len, end, &nc);
        if (nn <= 0 || is_word_cp(nc) != word) {
            break;
        }
        end += (size_t)nn;
    }
    *out_start = start;
    *out_end = end;
}

static size_t tv_word_forward(const char *s, size_t len, size_t i) {
    /* Skip the current run, then the separator run. */
    while (i < len) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(s, len, i, &cp);
        bool word = (cp >= 'a' && cp <= 'z') ||
                    (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
                    cp == '_' || cp >= 0x80u;
        if (!word) {
            break;
        }
        i += (size_t)(n > 0 ? n : 1);
    }
    while (i < len) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(s, len, i, &cp);
        bool word = (cp >= 'a' && cp <= 'z') ||
                    (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
                    cp == '_' || cp >= 0x80u;
        if (word) {
            break;
        }
        i += (size_t)(n > 0 ? n : 1);
    }
    return i;
}

static size_t tv_word_backward(const char *s, size_t len, size_t i) {
    while (i > 0) {
        size_t p = tv_utf8_prev(s, i);
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(s, len, p, &cp);
        bool word = (cp >= 'a' && cp <= 'z') ||
                    (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') ||
                    cp == '_' || cp >= 0x80u;
        if (word) {
            return p;
        }
        (void)n;
        i = p;
    }
    return 0;
}

/* ---- the wrap cache ---- */

static void tv_rebreak(fdk_textview *tv) {
    /* The width to break at: the viewport minus insets in WORD mode;
     * unbounded in NONE mode (one horizontal field). */
    fdk_i32 max_w = (tv->wrap == FDK_TEXTVIEW_WRAP_WORD)
                        ? tv->wrap_width - tv->pad * 2
                        : 1 << 20;
    if (max_w < 16) {
        max_w = 16;
    }
    /* Count first (the break engine's two-pass contract). */
    size_t count = 0;
    if (tv->font != NULL && tv->len > 0) {
        (void)fdk_font_break_lines_utf8(tv->font, tv->text, tv->len,
                                        max_w, NULL, 0, &count, NULL);
    }
    /* Grow the cache. */
    if (count > tv->line_cap) {
        size_t cap = (tv->line_cap == 0) ? 16 : tv->line_cap;
        while (cap < count) {
            cap *= 2;
        }
        fdk_text_line *grown =
            fdk_realloc(tv->lines, cap * sizeof(*tv->lines));
        if (grown == NULL) {
            count = (count < tv->line_cap) ? count : tv->line_cap;
        } else {
            tv->lines = grown;
            tv->line_cap = cap;
        }
    }
    if (count > tv->line_cap) {
        count = tv->line_cap; /* degraded: partial cache, honest */
    }
    if (tv->font != NULL && count > 0) {
        bool truncated = false;
        (void)fdk_font_break_lines_utf8(tv->font, tv->text, tv->len,
                                        max_w, tv->lines, count,
                                        &tv->line_count, &truncated);
    } else {
        tv->line_count = 0;
    }
    /* The document extent (the page's natural size). */
    fdk_i32 widest = 0;
    for (size_t i = 0; i < tv->line_count; i++) {
        if (tv->lines[i].advance_width > widest) {
            widest = tv->lines[i].advance_width;
        }
    }
    tv->doc_w = widest + tv->pad * 2;
    if (tv->doc_w < TV_MIN_W) {
        tv->doc_w = TV_MIN_W;
    }
    tv->doc_h = (fdk_i32)tv->line_count * tv->line_h + tv->pad * 2;
    if (tv->doc_h < TV_MIN_H) {
        tv->doc_h = TV_MIN_H;
    }
    if (tv->page != NULL) {
        fdk_widget_set_natural_size(tv->page, tv->doc_w, tv->doc_h);
    }
}

/* The full relayout: rebreak (when stale) + self-sync the
 * scrollview at the view's CURRENT bounds (the 1.4.7 rule —
 * set_bounds runs no hooks, dialogs place surfaces by hand) +
 * re-place the page. */
static void tv_relayout(fdk_textview *tv) {
    if (tv->font == NULL) {
        return;
    }
    fdk_font_metrics fm;
    fdk_font_get_metrics(tv->font, &fm);
    tv->line_h = fm.ascent + fm.descent + 2; /* +leading: the editor
                                                breathes more than a
                                                label line */
    fdk_i32 want_wrap = tv->base.bounds.width;
    if (want_wrap != tv->wrap_width) {
        tv->wrap_width = want_wrap;
        tv_rebreak(tv);
    }
    if (tv->scroll != NULL && tv->base.bounds.width > 0 &&
        tv->base.bounds.height > 0) {
        fdk_rect inner = { 0, 0, tv->base.bounds.width,
                           tv->base.bounds.height };
        fdk_widget_set_bounds(tv->scroll, inner);
    }
    if (tv->scroll != NULL) {
        fdk__scrollview_layout_changed(tv->scroll);
    }
}

/* Force a rebreak on the next relayout (text or wrap-mode change). */
static void tv_lines_invalid(fdk_textview *tv) {
    tv->wrap_width = -1; /* never matches a real width */
}

/* ---- geometry: visual line <-> byte offset ---- */

/* The visual line index whose byte range contains `off`. A caret at
 * a soft-break boundary (end of line i == start of line i+1)
 * belongs to line i — the caret paints at the END of the wrapped
 * line (GTK's rule; Enter then breaks there). The document end
 * belongs to the last line. */
static size_t tv_line_of_offset(const fdk_textview *tv, size_t off) {
    if (tv->line_count == 0) {
        return 0;
    }
    if (off >= tv->len) {
        return tv->line_count - 1;
    }
    for (size_t i = 0; i < tv->line_count; i++) {
        size_t start = tv->lines[i].byte_offset;
        size_t end = start + tv->lines[i].byte_len;
        if (off >= start && off <= end) {
            if (off == end && i + 1 < tv->line_count &&
                tv->lines[i + 1].byte_offset == end && off != start) {
                /* boundary caret: keep line i unless it is empty */
                if (tv->lines[i].byte_len > 0) {
                    return i;
                }
            }
            return i;
        }
        if (off < start) {
            return (i > 0) ? i - 1 : 0;
        }
    }
    return tv->line_count - 1;
}

/* Width of line `i`'s bytes [line_start, off) — the caret's x. */
static fdk_i32 tv_line_width_to(const fdk_textview *tv, size_t i,
                                size_t off) {
    if (tv->font == NULL || i >= tv->line_count) {
        return 0;
    }
    const fdk_text_line *line = &tv->lines[i];
    size_t rel = off - line->byte_offset;
    if (rel > line->byte_len) {
        rel = line->byte_len;
    }
    if (rel == 0) {
        return 0;
    }
    fdk_text_metrics m;
    if (!fdk_ok(fdk_font_measure_utf8(tv->font,
                                      tv->text + line->byte_offset,
                                      rel, &m))) {
        return 0;
    }
    return m.advance_width;
}

/* The byte offset at (line i, x-in-content): walks the line's
 * codepoints, landing the caret before the glyph the pen crosses.
 * Past the line's end = the line's end (a soft break's end — the
 * next Right steps into the next line; the Entry's discipline). */
static size_t tv_offset_at_x(const fdk_textview *tv, size_t i,
                             fdk_f32 x) {
    if (i >= tv->line_count || tv->font == NULL) {
        return 0;
    }
    const fdk_text_line *line = &tv->lines[i];
    if (line->byte_len == 0 || x <= 0.0f) {
        return line->byte_offset;
    }
    size_t off = line->byte_offset;
    fdk_f32 pen = (fdk_f32)tv->pad;
    while (off < line->byte_offset + line->byte_len) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(tv->text, tv->len, off, &cp);
        if (n <= 0) {
            break;
        }
        fdk_text_metrics m;
        if (!fdk_ok(fdk_font_measure_utf8(tv->font, tv->text + off,
                                          (size_t)n, &m))) {
            break;
        }
        if (pen + (fdk_f32)m.advance_width / 2.0f > x) {
            break; /* the glyph's midpoint passed: stop before it */
        }
        pen += (fdk_f32)m.advance_width;
        off += (size_t)n;
    }
    return off;
}

/* The byte offset at content (x, y): line from y, offset from x. */
static size_t tv_offset_at(const fdk_textview *tv, fdk_f32 x,
                           fdk_f32 y) {
    size_t line = 0;
    if (tv->line_count > 0) {
        fdk_f32 ly = (y - (fdk_f32)tv->pad) / (fdk_f32)tv->line_h;
        if (ly < 0.0f) {
            ly = 0.0f;
        }
        if (ly > (fdk_f32)(tv->line_count - 1)) {
            ly = (fdk_f32)(tv->line_count - 1);
        }
        line = (size_t)ly;
    }
    return tv_offset_at_x(tv, line, x);
}

/* ---- selection + caret ---- */

static void tv_set_selection(fdk_textview *tv, size_t anchor,
                             size_t caret);
static fdk_result tv_splice(fdk_textview *tv, size_t from, size_t to,
                            const char *insert, size_t insert_len);

static void tv_scroll_to_caret(fdk_textview *tv) {
    if (tv->scroll == NULL) {
        return;
    }
    size_t line = tv_line_of_offset(tv, tv->caret);
    fdk_i32 caret_x = tv->pad + tv_line_width_to(tv, line, tv->caret);
    fdk_i32 preedit_w = 0;
    if (tv->preedit != NULL && tv->preedit[0] != '\0' &&
        tv->font != NULL) {
        fdk_i32 pw = 0, ph = 0;
        fdk__text_extent(tv->font, tv->preedit, &pw, &ph);
        preedit_w = pw;
    }
    fdk_i32 caret_y = tv->pad + (fdk_i32)line * tv->line_h;
    fdk_i32 off_x = 0, off_y = 0;
    fdk_scrollview_get_scroll_offset(tv->scroll, &off_x, &off_y);
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(tv->scroll, &vw, &vh);
    fdk_i32 want_x = off_x;
    fdk_i32 want_y = off_y;
    if (caret_x - off_x < 8) {
        want_x = caret_x - 8;
    } else if (caret_x + preedit_w - off_x > vw - 8) {
        want_x = caret_x + preedit_w - vw + 8;
    }
    if (caret_y - off_y < tv->line_h) {
        want_y = caret_y - tv->line_h;
    } else if (caret_y + tv->line_h - off_y > vh) {
        want_y = caret_y + tv->line_h - vh;
    }
    if (want_x < 0) {
        want_x = 0;
    }
    if (want_y < 0) {
        want_y = 0;
    }
    if (want_x != off_x || want_y != off_y) {
        (void)fdk_scrollview_scroll_to(tv->scroll, want_x, want_y);
    }
}

static void tv_blink_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_textview *tv = user;
    tv->caret_on = !tv->caret_on;
    fdk_widget_invalidate(&tv->base);
}

static void tv_blink_start(fdk_textview *tv) {
    if (tv->blink_timer != NULL) {
        return; /* already blinking */
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return; /* detached: the caret stays solid (headless rule) */
    }
    tv->caret_on = true;
    tv->blink_timer = fdk_timer_add(ctx, 530, true, tv_blink_tick, tv);
}

static void tv_blink_stop(fdk_textview *tv) {
    if (tv->blink_timer != NULL) {
        fdk_timer_remove(tv->blink_timer);
        tv->blink_timer = NULL;
    }
    tv->caret_on = false;
}

static void tv_blink_restart(fdk_textview *tv) {
    tv_blink_stop(tv);
    if ((tv->base.flags & FDK_WF_FOCUSED) != 0) {
        tv_blink_start(tv);
        tv->caret_on = true;
    }
}

/* ---- PRIMARY sync (the Entry's politeness, verbatim) ---- */

static void tv_primary_sync(fdk_textview *tv) {
    if ((tv->base.flags & FDK_WF_ENABLED) == 0) {
        return;
    }
    bool selected = tv->caret != tv->anchor;
    if (!selected && !tv->primary_pushed) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return;
    }
    if (!selected) {
        (void)fdk_clipboard_set_primary_text(ctx, "");
        tv->primary_pushed = false;
        return;
    }
    size_t lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
    size_t hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
    char saved = tv->text[hi];
    tv->text[hi] = '\0';
    fdk_result r = fdk_clipboard_set_primary_text(ctx, tv->text + lo);
    tv->text[hi] = saved;
    tv->primary_pushed = fdk_ok(r);
}

static void tv_set_selection(fdk_textview *tv, size_t anchor,
                             size_t caret) {
    if (anchor > tv->len) {
        anchor = tv->len;
    }
    if (caret > tv->len) {
        caret = tv->len;
    }
    if (anchor == tv->anchor && caret == tv->caret) {
        return;
    }
    tv->anchor = anchor;
    tv->caret = caret;
    tv->have_goal = false; /* an explicit selection clears the goal */
    tv_primary_sync(tv);
    fdk_widget_invalidate(&tv->base);
}

/* Caret move without selection change (motions): the Entry's
 * move_caret, plus the goal-column memory for Up/Down. */
static void tv_move_caret(fdk_textview *tv, size_t to, bool extend) {
    if (to > tv->len) {
        to = tv->len;
    }
    if (extend) {
        tv_set_selection(tv, tv->anchor, to);
    } else {
        tv_set_selection(tv, to, to);
    }
    tv->have_goal = false;
    tv_scroll_to_caret(tv);
}

/* ---- the buffer: splice + undo (the Entry's contracts) ---- */

typedef struct tv_undo_op {
    fdk_textview *tv;    /* borrowed — the stack is owned by it */
    size_t from;         /* splice start */
    size_t to;           /* splice end (pre-edit) */
    char *removed;       /* owned, the pre-edit text over [from,to) */
    char *inserted;      /* owned, what replaced it */
    size_t caret_before;
    size_t anchor_before;
    bool typed;          /* single-codepoint insert/delete: runs
                          * coalesce (the Entry's rule) */
} tv_undo_op;

static fdk_result tv_ensure_cap(fdk_textview *tv, size_t need) {
    if (need <= tv->cap) {
        return FDK_OK;
    }
    size_t cap = (tv->cap == 0) ? 64 : tv->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(char)) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    char *grown = fdk_realloc(tv->text, cap);
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    tv->text = grown;
    tv->cap = cap;
    return FDK_OK;
}

static void tv_undo_destroy(void *user) {
    tv_undo_op *op = user;
    fdk_free(op->removed);
    fdk_free(op->inserted);
    fdk_free(op);
}

static fdk_result tv_apply_op(fdk_textview *tv, size_t from, size_t to,
                              const char *insert, size_t insert_len) {
    tv->undo_applying = true;
    fdk_result r = tv_splice(tv, from, to, insert, insert_len);
    tv->undo_applying = false;
    return r;
}

static void tv_undo_apply(void *user) {
    tv_undo_op *op = user;
    fdk_textview *tv = op->tv;
    size_t ins_len = (op->inserted != NULL) ? strlen(op->inserted) : 0;
    size_t rem_len = (op->removed != NULL) ? strlen(op->removed) : 0;
    (void)tv_apply_op(tv, op->from, op->from + ins_len, op->removed,
                      rem_len);
    if (rem_len > 0) {
        tv_set_selection(tv, op->from, op->from + rem_len);
    } else {
        tv_set_selection(tv, op->caret_before, op->anchor_before);
    }
    tv_blink_restart(tv);
}

static void tv_redo_apply(void *user) {
    tv_undo_op *op = user;
    fdk_textview *tv = op->tv;
    size_t ins_len = (op->inserted != NULL) ? strlen(op->inserted) : 0;
    size_t rem_len = (op->removed != NULL) ? strlen(op->removed) : 0;
    (void)tv_apply_op(tv, op->from, op->from + rem_len, op->inserted,
                      ins_len);
    tv_blink_restart(tv);
}

static bool tv_utf8_single_cp(const char *s, size_t n) {
    if (s == NULL || n == 0) {
        return false;
    }
    fdk_u32 cp = 0;
    int step = fdk_text_utf8_next(s, n, 0, &cp);
    return step > 0 && (size_t)step == n;
}

static void tv_undo_record(fdk_textview *tv, size_t from, size_t to,
                           char *removed_copy, char *insert_copy,
                           size_t caret_before, size_t anchor_before) {
    if (tv->undo == NULL) {
        if (!fdk_ok(fdk_undo_stack_create(0, &tv->undo))) {
            tv->undo = NULL;
            fdk_free(removed_copy);
            fdk_free(insert_copy);
            FDK_WARN("textview: undo stack unavailable; edits are "
                     "not recorded");
            return;
        }
    }
    bool typed = tv_utf8_single_cp(insert_copy,
                                   (insert_copy != NULL)
                                       ? strlen(insert_copy)
                                       : 0) &&
                 removed_copy == NULL;
    bool del_typed = removed_copy != NULL && insert_copy == NULL &&
                     tv_utf8_single_cp(removed_copy,
                                       strlen(removed_copy));
    typed = typed || del_typed;

    /* Gesture grouping (the Entry's rule): a selection-delete
     * followed by the insert that lands in its slot composes into
     * ONE replace op. Read-and-clear; anything else expires it. */
    bool group = tv->undo_group_pending;
    tv->undo_group_pending = false;
    if (group && insert_copy != NULL && removed_copy == NULL &&
        fdk_undo_stack_can_undo(tv->undo)) {
        const fdk_undo_op *gtop = NULL;
        if (fdk_ok(fdk_undo_stack_top(tv->undo, &gtop)) &&
            gtop != NULL) {
            tv_undo_op *gop = gtop->user_data;
            if (gop != NULL && gop->tv == tv && gop->inserted == NULL &&
                gop->removed != NULL && from == gop->from) {
                tv_undo_op *merged = fdk_alloc(sizeof(*merged));
                if (merged != NULL) {
                    merged->tv = tv;
                    merged->from = gop->from;
                    merged->to = gop->to;
                    merged->removed = gop->removed; /* stolen below */
                    merged->inserted = insert_copy;
                    merged->caret_before = gop->caret_before;
                    merged->anchor_before = gop->anchor_before;
                    merged->typed = false; /* a replace is atomic */
                    gop->removed = NULL;
                    fdk_undo_op uop = {
                        .user_data = merged,
                        .undo = tv_undo_apply,
                        .redo = tv_redo_apply,
                        .destroy = tv_undo_destroy,
                    };
                    if (fdk_ok(fdk__undo_stack_replace_top(tv->undo,
                                                           &uop))) {
                        return; /* the gesture composed */
                    }
                    gop->removed = merged->removed;
                    fdk_free(merged);
                }
            }
        }
    }

    /* Typing-run coalescing (the Entry's rule, both directions):
     * consecutive single-codepoint edits extend the top op in place
     * instead of stacking one op per keystroke. */
    if (typed && fdk_undo_stack_can_undo(tv->undo)) {
        const fdk_undo_op *top = NULL;
        if (fdk_ok(fdk_undo_stack_top(tv->undo, &top)) &&
            top != NULL) {
            tv_undo_op *top_op = top->user_data;
            if (top_op != NULL && top_op->tv == tv && top_op->typed) {
                size_t top_ins = (top_op->inserted != NULL)
                                     ? strlen(top_op->inserted) : 0;
                size_t top_rem = (top_op->removed != NULL)
                                     ? strlen(top_op->removed) : 0;
                bool merged = false;
                if (insert_copy != NULL && top_op->inserted != NULL &&
                    removed_copy == NULL &&
                    from == top_op->from + top_ins) {
                    /* typing run: append */
                    size_t a = top_ins;
                    size_t b = strlen(insert_copy);
                    char *grown =
                        fdk_realloc(top_op->inserted, a + b + 1);
                    if (grown != NULL) {
                        memcpy(grown + a, insert_copy, b + 1);
                        top_op->inserted = grown;
                        merged = true;
                    }
                } else if (removed_copy != NULL && insert_copy == NULL &&
                           top_op->removed != NULL &&
                           top_op->inserted == NULL) {
                    if (to == top_op->from) {
                        /* backspace run: prepend, from moves back */
                        size_t b = strlen(removed_copy);
                        size_t a = top_rem;
                        char *grown =
                            fdk_realloc(top_op->removed, a + b + 1);
                        if (grown != NULL) {
                            memmove(grown + b, grown, a + 1);
                            memcpy(grown, removed_copy, b);
                            top_op->removed = grown;
                            top_op->from = from;
                            merged = true;
                        }
                    } else if (from == top_op->from ||
                               from == top_op->to) {
                        /* delete-key run: append */
                        size_t a = top_rem;
                        size_t b = strlen(removed_copy);
                        char *grown =
                            fdk_realloc(top_op->removed, a + b + 1);
                        if (grown != NULL) {
                            memcpy(grown + a, removed_copy, b + 1);
                            top_op->removed = grown;
                            top_op->to = to;
                            merged = true;
                        }
                    }
                }
                if (merged) {
                    fdk_free(removed_copy);
                    fdk_free(insert_copy);
                    return; /* coalesced into the top op */
                }
            }
        }
    }

    tv_undo_op *op = fdk_alloc(sizeof(*op));
    if (op == NULL) {
        fdk_free(removed_copy);
        fdk_free(insert_copy);
        FDK_WARN("textview: edit not recorded (op alloc failed)");
        return;
    }
    op->tv = tv;
    op->from = from;
    op->to = to;
    op->removed = removed_copy;
    op->inserted = insert_copy;
    op->caret_before = caret_before;
    op->anchor_before = anchor_before;
    op->typed = typed;
    fdk_undo_op uop = {
        .user_data = op,
        .undo = tv_undo_apply,
        .redo = tv_redo_apply,
        .destroy = tv_undo_destroy,
    };
    if (!fdk_ok(fdk_undo_stack_push(tv->undo, &uop))) {
        FDK_WARN("textview: edit not recorded (push failed)");
        fdk_free(op->removed);
        fdk_free(op->inserted);
        fdk_free(op);
    }
}

static fdk_result tv_splice(fdk_textview *tv, size_t from, size_t to,
                            const char *insert, size_t insert_len) {
    if (from > to || to > tv->len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (insert == NULL) {
        insert_len = 0;
    }
    size_t new_len = tv->len - (to - from) + insert_len;
    if (new_len > TV_MAX_TEXT) {
        FDK_WARN("textview: %d-byte limit reached; insert refused",
                 TV_MAX_TEXT);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = tv_ensure_cap(tv, new_len + 1);
    if (!fdk_ok(r)) {
        return r;
    }
    /* ---- undo capture (pre-mutation state) ---- */
    char *removed_copy = NULL;
    char *insert_copy = NULL;
    size_t caret_before = tv->caret;
    size_t anchor_before = tv->anchor;
    if (!tv->undo_applying) {
        if (to > from) {
            removed_copy = fdk_alloc(to - from + 1);
            if (removed_copy != NULL) {
                memcpy(removed_copy, tv->text + from, to - from);
                removed_copy[to - from] = '\0';
            }
        }
        if (insert_len > 0) {
            insert_copy = fdk_alloc(insert_len + 1);
            if (insert_copy != NULL) {
                memcpy(insert_copy, insert, insert_len);
                insert_copy[insert_len] = '\0';
            }
        }
    }
    memmove(tv->text + from + insert_len, tv->text + to, tv->len - to);
    if (insert_len > 0) {
        memcpy(tv->text + from, insert, insert_len);
    }
    tv->text[new_len] = '\0';
    tv->len = new_len;
    tv->caret = from + insert_len;
    tv->anchor = tv->caret;
    tv_lines_invalid(tv);
    tv_relayout(tv);
    tv_scroll_to_caret(tv);
    fdk_widget_invalidate(&tv->base);
    fdk__a11y_notify(&tv->base, FDK_A11Y_VALUE_CHANGED, 0);
    if (tv->on_changed != NULL) {
        tv->on_changed(&tv->base, tv->on_changed_data);
    }
    /* ---- undo record (post-success; all-or-nothing) ---- */
    if (!tv->undo_applying) {
        bool need_removed = (to > from);
        bool need_insert = (insert_len > 0);
        bool removed_ok = !need_removed || removed_copy != NULL;
        bool insert_ok = !need_insert || insert_copy != NULL;
        if ((need_removed || need_insert) && removed_ok && insert_ok) {
            tv_undo_record(tv, from, to, removed_copy, insert_copy,
                           caret_before, anchor_before);
        } else {
            if (need_removed || need_insert) {
                FDK_WARN("textview: edit not recorded (history copy "
                         "failed)");
            }
            fdk_free(removed_copy);
            fdk_free(insert_copy);
        }
    }
    return FDK_OK;
}

/* ---- clipboard (the Entry's contracts) ---- */

static void tv_clipboard_copy(fdk_textview *tv) {
    if ((tv->base.flags & FDK_WF_ENABLED) == 0 || tv->caret == tv->anchor) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return;
    }
    size_t lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
    size_t hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
    char saved = tv->text[hi];
    tv->text[hi] = '\0';
    (void)fdk_clipboard_set_text(ctx, tv->text + lo);
    tv->text[hi] = saved;
}

static void tv_clipboard_cut(fdk_textview *tv) {
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return;
    }
    tv_clipboard_copy(tv);
    if (tv->caret != tv->anchor) {
        size_t lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
        size_t hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
        (void)tv_splice(tv, lo, hi, NULL, 0);
    }
}

static void tv_clipboard_paste(fdk_textview *tv) {
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return;
    }
    char *clip = fdk_clipboard_get_text(ctx);
    if (clip == NULL) {
        return;
    }
    size_t lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
    size_t hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
    /* Sanitize: the classic CRLF/CR normalization on the way in. */
    {
        char *norm = fdk_alloc(strlen(clip) + 1);
        if (norm != NULL) {
            size_t o = 0;
            for (size_t i = 0; clip[i] != '\0'; i++) {
                if (clip[i] == '\r') {
                    if (clip[i + 1] == '\n') {
                        i++; /* CRLF -> one \n */
                    }
                    norm[o++] = '\n';
                } else {
                    norm[o++] = clip[i];
                }
            }
            norm[o] = '\0';
            fdk_free(clip);
            clip = norm;
        }
    }
    fdk_result r = tv_splice(tv, lo, hi, clip, strlen(clip));
    fdk_free(clip);
    (void)r;
}

static void tv_delete_selection(fdk_textview *tv) {
    if (tv->caret == tv->anchor) {
        return;
    }
    size_t lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
    size_t hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
    fdk_result r = tv_splice(tv, lo, hi, NULL, 0);
    if (fdk_ok(r)) {
        tv->undo_group_pending = true;
    }
}

/* Middle-click: PRIMARY pastes at the click (read-first, the Entry's
 * self-paste ordering). */
static void tv_primary_paste(fdk_textview *tv, fdk_f32 x, fdk_f32 y) {
    if (tv->read_only) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&tv->base));
    if (ctx == NULL) {
        return;
    }
    char *primary = fdk_clipboard_get_primary_text(ctx);
    size_t hit = tv_offset_at(tv, x, y);
    if (primary == NULL) {
        tv_set_selection(tv, hit, hit);
        return;
    }
    (void)tv_splice(tv, hit, hit, primary, strlen(primary));
    fdk_free(primary);
}

/* ---- events ---- */

/* Up/Down through the wrap cache with the goal column. */
static void tv_move_vertical(fdk_textview *tv, int delta,
                             bool extend) {
    if (tv->line_count == 0) {
        return;
    }
    size_t line = tv_line_of_offset(tv, tv->caret);
    if (!tv->have_goal) {
        tv->goal_x = tv->pad + tv_line_width_to(tv, line, tv->caret);
        tv->have_goal = true;
    }
    /* Signed arithmetic through the whole hop: a page is many
     * lines, and (size_t)(-page) would wrap instead of climb. */
    fdk_i64 target = (fdk_i64)line + (fdk_i64)delta;
    if (target < 0) {
        /* GTK: Up past the first line goes to offset 0. */
        tv_move_caret(tv, 0, extend);
        return;
    }
    size_t next = (size_t)target;
    if (next >= tv->line_count) {
        next = tv->line_count - 1;
    }
    size_t off = tv_offset_at_x(tv, next, (fdk_f32)tv->goal_x);
    tv_set_selection(tv, extend ? tv->anchor : off, off);
    /* The goal survives for the NEXT vertical step (GTK's rule). */
    tv->have_goal = true;
    tv_scroll_to_caret(tv);
}

/* Page motions: a viewport's worth of lines. */
static void tv_move_page(fdk_textview *tv, int delta, bool extend) {
    fdk_i32 vh = 4 * tv->line_h;
    if (tv->scroll != NULL) {
        fdk_i32 vw = 0;
        fdk__scrollview_viewport(tv->scroll, &vw, &vh);
    }
    int page = (int)(vh / (tv->line_h > 0 ? tv->line_h : 1));
    if (page < 1) {
        page = 1;
    }
    tv_move_vertical(tv, delta * page, extend);
}

static bool tv_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_textview *tv = tv_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        if (ev->pointer.button == FDK_POINTER_BUTTON_MIDDLE) {
            if (!fdk_widget_has_focus(w)) {
                (void)fdk_widget_focus(w);
            }
            tv_primary_paste(tv, ev->pointer.position.x,
                             ev->pointer.position.y);
            return true;
        }
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        /* Click-count machinery (the Entry's). */
        fdk_i64 now = tv_now_ms();
        fdk_f32 dx = ev->pointer.position.x - tv->last_click_x;
        fdk_f32 dy = ev->pointer.position.y - tv->last_click_y;
        if (now - tv->last_click_ms <= TV_DBLCLICK_MS &&
            dx >= -TV_DBLCLICK_SLOP && dx <= TV_DBLCLICK_SLOP &&
            dy >= -TV_DBLCLICK_SLOP && dy <= TV_DBLCLICK_SLOP) {
            tv->click_count++;
        } else {
            tv->click_count = 1;
        }
        tv->last_click_ms = now;
        tv->last_click_x = ev->pointer.position.x;
        tv->last_click_y = ev->pointer.position.y;

        size_t hit = tv_offset_at(tv, ev->pointer.position.x,
                                  ev->pointer.position.y);
        if (tv->click_count == 2) {
            size_t ws = 0, we = 0;
            tv_word_range(tv->text, tv->len, hit, &ws, &we);
            tv_set_selection(tv, ws, we);
            tv->selecting = 1;
        } else if (tv->click_count >= 3) {
            /* Triple: the PARAGRAPH (the logical line, wraps
             * included) — the multi-line twin of the Entry's
             * whole-field triple. */
            size_t ps = 0, pe = 0;
            tv_paragraph_range(tv->text, tv->len, hit, &ps, &pe);
            tv_set_selection(tv, ps, pe);
            tv->selecting = 1;
        } else {
            bool shift = (ev->pointer.modifiers & FDK_MOD_SHIFT) != 0;
            if (shift) {
                tv_set_selection(tv, tv->anchor, hit);
            } else {
                tv_set_selection(tv, hit, hit);
            }
            tv->selecting = 1;
        }
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION:
        if (tv->selecting) {
            size_t hit = tv_offset_at(tv, ev->position.x, ev->position.y);
            tv_set_selection(tv, tv->anchor, hit);
            tv->have_goal = false;
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_UP:
        tv->selecting = 0;
        return true;
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        const fdk_key_event *key = &ev->key;
        fdk_u32 mods = key->modifiers;
        bool shift = (mods & FDK_MOD_SHIFT) != 0;
        bool ctrl = (mods & FDK_MOD_CTRL) != 0;

        /* Clipboard shortcuts (the reader contract when
         * read-only: copy + select-all stay, mutators drop). */
        if (ctrl && !shift) {
            if (key->codepoint == 'c' || key->codepoint == 'C') {
                tv_clipboard_copy(tv);
                return true;
            }
            if (!tv->read_only &&
                (key->codepoint == 'x' || key->codepoint == 'X')) {
                tv_clipboard_cut(tv);
                return true;
            }
            if (!tv->read_only &&
                (key->codepoint == 'v' || key->codepoint == 'V')) {
                tv_clipboard_paste(tv);
                return true;
            }
            if (key->codepoint == 'a' || key->codepoint == 'A') {
                tv_set_selection(tv, 0, tv->len);
                return true;
            }
        }

        /* Undo/redo spellings (the Entry's). */
        if (ctrl && !tv->read_only &&
            (key->codepoint == 'z' || key->codepoint == 'Z')) {
            if (shift) {
                (void)fdk_textview_redo(w);
            } else {
                (void)fdk_textview_undo(w);
            }
            return true;
        }
        if (ctrl && !shift && !tv->read_only &&
            (key->codepoint == 'y' || key->codepoint == 'Y')) {
            (void)fdk_textview_redo(w);
            return true;
        }

        switch (key->scancode) {
        case FDK_KEY_LEFT:
            if (ctrl) {
                tv_move_caret(tv, tv_word_backward(tv->text, tv->len,
                                                   tv->caret), shift);
            } else {
                tv_move_caret(tv, tv_utf8_prev(tv->text, tv->caret),
                              shift);
            }
            return true;
        case FDK_KEY_RIGHT:
            if (ctrl) {
                tv_move_caret(tv, tv_word_forward(tv->text, tv->len,
                                                  tv->caret), shift);
            } else if (tv->caret < tv->len) {
                fdk_u32 cp = 0;
                int n = fdk_text_utf8_next(tv->text, tv->len, tv->caret,
                                           &cp);
                tv_move_caret(tv, tv->caret + (size_t)n, shift);
            }
            return true;
        case FDK_KEY_UP:
            tv_move_vertical(tv, -1, shift);
            return true;
        case FDK_KEY_DOWN:
            tv_move_vertical(tv, 1, shift);
            return true;
        case FDK_KEY_HOME:
            if (ctrl) {
                tv_move_caret(tv, 0, shift);
            } else {
                size_t line =
                    tv_line_of_offset(tv, tv->caret);
                tv_move_caret(tv, tv->lines[line].byte_offset, shift);
            }
            return true;
        case FDK_KEY_END:
            if (ctrl) {
                tv_move_caret(tv, tv->len, shift);
            } else {
                size_t line = tv_line_of_offset(tv, tv->caret);
                const fdk_text_line *l = &tv->lines[line];
                /* Past a trailing soft-break space run: the break
                 * engine trims; the caret sits at the trimmed end. */
                size_t end = l->byte_offset + l->byte_len;
                tv_move_caret(tv, end, shift);
            }
            return true;
        case FDK_KEY_PAGE_UP:
            tv_move_page(tv, -1, shift);
            return true;
        case FDK_KEY_PAGE_DOWN:
            tv_move_page(tv, 1, shift);
            return true;
        case FDK_KEY_BACKSPACE:
            if (tv->read_only) {
                return true; /* consumed, not edited */
            }
            if (tv->caret != tv->anchor) {
                tv_delete_selection(tv);
            } else if (tv->caret > 0) {
                size_t prev = tv_utf8_prev(tv->text, tv->caret);
                (void)tv_splice(tv, prev, tv->caret, NULL, 0);
            }
            return true;
        case FDK_KEY_DELETE:
            if (tv->read_only) {
                return true; /* consumed, not edited */
            }
            if (tv->caret != tv->anchor) {
                tv_delete_selection(tv);
            } else if (tv->caret < tv->len) {
                fdk_u32 cp = 0;
                int n = fdk_text_utf8_next(tv->text, tv->len, tv->caret,
                                           &cp);
                (void)tv_splice(tv, tv->caret, tv->caret + (size_t)n,
                                NULL, 0);
            }
            return true;
        case FDK_KEY_ENTER:
            if (tv->read_only) {
                return true; /* consumed, not edited */
            }
            tv_delete_selection(tv);
            (void)tv_splice(tv, tv->caret, tv->caret, "\n", 1);
            return true;
        case FDK_KEY_ESC:
            /* Collapse the selection; a quiescent view bubbles (the
             * Entry's 1.2.1 rule — a dialog's Cancel must live). */
            if (tv->anchor != tv->caret) {
                tv_set_selection(tv, tv->caret, tv->caret);
                return true;
            }
            break;
        default:
            break;
        }

        /* Textual insert (the Entry's filter). */
        if (key->codepoint >= 0x20u && !ctrl && !tv->read_only) {
            char buf[4];
            fdk_u32 cp = key->codepoint;
            size_t n = 0;
            if (cp < 0x80u) {
                buf[n++] = (char)cp;
            } else if (cp < 0x800u) {
                buf[n++] = (char)(0xC0u | (cp >> 6));
                buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else if (cp < 0x10000u) {
                buf[n++] = (char)(0xE0u | (cp >> 12));
                buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            } else {
                buf[n++] = (char)(0xF0u | (cp >> 18));
                buf[n++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                buf[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                buf[n++] = (char)(0x80u | (cp & 0x3Fu));
            }
            tv_delete_selection(tv);
            (void)tv_splice(tv, tv->caret, tv->caret, buf, n);
            return true;
        }
        return false;
    }
    case FDK_WIDGET_FOCUS_IN:
        tv_blink_start(tv);
        return false; /* focus keeps bubbling */
    case FDK_WIDGET_FOCUS_OUT:
        tv_blink_stop(tv);
        return false;
    default:
        break;
    }
    return false;
}

/* ---- the page (the owner's lines, painted) ---- */

typedef struct fdk_tv_page {
    fdk_widget base;
} fdk_tv_page;

static void tv_page_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip);

static const fdk_widget_class fdk_tv_page_class_def = {
    .size = sizeof(fdk_tv_page),
    .name = "textview-page",
    .handle_event = NULL, /* the view owns all input */
    .paint = tv_page_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
};

/* The selection fill + text + caret + preedit for every line that
 * intersects the clip. Content-space math (page-relative); the
 * scrollview already translated the page. */
static void tv_page_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)bounds;
    /* Owner: page -> scrollview -> textview. */
    fdk_widget *scroll = w->parent;
    fdk_widget *tv_w = (scroll != NULL) ? scroll->parent : NULL;
    if (tv_w == NULL || tv_w->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(tv_w);
    if (tv->font == NULL || tv->line_count == 0) {
        return;
    }
    bool disabled = (tv_w->flags & FDK_WF_ENABLED) == 0;
    fdk_color text_col = disabled ? fdk__pal_text_disabled()
                                  : fdk__pal_text();

    size_t sel_lo = 0, sel_hi = 0;
    bool has_sel = tv->caret != tv->anchor;
    if (has_sel) {
        sel_lo = (tv->anchor < tv->caret) ? tv->anchor : tv->caret;
        sel_hi = (tv->anchor < tv->caret) ? tv->caret : tv->anchor;
    }

    /* First/last line intersecting the clip (content space). */
    fdk_i32 first = (clip.y - tv->pad) / (tv->line_h > 0 ? tv->line_h : 1);
    if (first < 0) {
        first = 0;
    }
    fdk_i32 last = (clip.y + clip.height - tv->pad - 1) /
                   (tv->line_h > 0 ? tv->line_h : 1);
    if (last >= (fdk_i32)tv->line_count) {
        last = (fdk_i32)tv->line_count - 1;
    }

    for (fdk_i32 i = first; i <= last; i++) {
        const fdk_text_line *line = &tv->lines[i];
        fdk_i32 baseline = tv->pad + i * tv->line_h;
        /* The metrics are cached per font; ascent comes with the
         * pitch the relayout already computed. */
        fdk_font_metrics fm;
        fdk_font_get_metrics(tv->font, &fm);
        baseline += fm.ascent;

        /* Selection fill on this line: [sel_lo, sel_hi) intersected
         * with the line's byte range. A selection spanning the line
         * fills its full width (the classic full-row highlight for
         * wrapped lines). */
        if (has_sel) {
            size_t lstart = line->byte_offset;
            size_t lend = line->byte_offset + line->byte_len;
            if (sel_lo < lend && sel_hi > lstart) {
                fdk_i32 x1 = tv->pad;
                fdk_i32 x2 = tv->pad + line->advance_width;
                if (sel_lo > lstart) {
                    x1 += tv_line_width_to(tv, (size_t)i, sel_lo);
                }
                if (sel_hi < lend) {
                    x2 = tv->pad + tv_line_width_to(tv, (size_t)i,
                                                    sel_hi);
                }
                if (sel_hi >= lend && sel_lo <= lstart) {
                    /* full line: extend past the text for the classic
                     * editor look (a hair past the advance) */
                    x2 += 4;
                }
                if (x2 > x1) {
                    fdk_rect r = {x1, tv->pad + i * tv->line_h,
                                  x2 - x1, tv->line_h};
                    fdk_surface_fill_rect(surface, r,
                                          fdk__pal_selection());
                }
            }
        }

        if (line->byte_len > 0) {
            (void)fdk_surface_draw_utf8(surface, tv->font,
                                        tv->text + line->byte_offset,
                                        line->byte_len, tv->pad,
                                        baseline, text_col);
        }

        /* The caret: on the caret's line, at its x, when blinking
         * on (focus + timer discipline). */
        if (tv->caret_on &&
            tv_line_of_offset(tv, tv->caret) == (size_t)i) {
            fdk_i32 cx = tv->pad +
                         tv_line_width_to(tv, (size_t)i, tv->caret);
            fdk_rect c = {cx, tv->pad + i * tv->line_h, 1,
                          tv->line_h};
            fdk_surface_fill_rect(surface, c, text_col);
            /* The preedit: inline AFTER the caret, underlined (the
             * Entry's display-only contract; it never edits). */
            if (tv->preedit != NULL && tv->preedit[0] != '\0') {
                fdk_i32 pw = 0, ph = 0;
                fdk__text_extent(tv->font, tv->preedit, &pw, &ph);
                (void)fdk_surface_draw_utf8(
                    surface, tv->font, tv->preedit,
                    strlen(tv->preedit), cx + 1, baseline, text_col);
                fdk_rect u = {cx + 1,
                              tv->pad + i * tv->line_h + tv->line_h - 2,
                              pw, 1};
                fdk_surface_fill_rect(surface, u, fdk__pal_accent());
            }
        }
    }
}

/* ---- the view class ---- */

/* The entry-style rounded field + focus ring (the scrollview and
 * its page paint above it). */
static void tv_view_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    /* The Entry's field discipline: disabled keeps the control
     * fill; focus reads as the themed ring over the border. */
    const bool focused = (w->flags & FDK_WF_FOCUSED) != 0;
    fdk_color fill = ((w->flags & FDK_WF_ENABLED) == 0)
        ? fdk__pal_control_disabled()
        : fdk__pal_entry();
    fdk_i32 radius =
        fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);
    if ((w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_draw_rounded_rect(
            surface, bounds, radius,
            focused ? fdk__pal_focus_ring()
                    : fdk__pal_entry_border());
    }
}

static void tv_view_measure(fdk_widget *w, fdk_size *out) {
    fdk_textview *tv = tv_of(w);
    /* Natural: one honest screenful tall, the wrap-width wide (the
     * IconView's "something smaller gets assigned and it scrolls"
     * contract). */
    out->width = tv->doc_w;
    if (out->width < TV_MIN_W) {
        out->width = TV_MIN_W;
    }
    out->height = (tv->line_h > 0)
                      ? tv->line_h * 6 + tv->pad * 2
                      : TV_MIN_H;
    if (out->height < TV_MIN_H) {
        out->height = TV_MIN_H;
    }
}

static void tv_view_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_textview *tv = tv_of(w);
    fdk_widget_set_bounds(w, assigned);
    if (tv->scroll != NULL) {
        fdk_rect inner = {0, 0, assigned.width, assigned.height};
        fdk_widget_set_bounds(tv->scroll, inner);
        fdk_widget_child_layout_changed(tv->scroll);
    }
    tv_relayout(tv);
}

static void tv_view_destroy(fdk_widget *w) {
    fdk_textview *tv = tv_of(w);
    tv_blink_stop(tv);
    if (tv->undo != NULL) {
        fdk_undo_stack_destroy(tv->undo);
    }
    fdk_free(tv->lines);
    fdk_free(tv->text);
    fdk_free(tv->preedit);
}

/* ---- a11y ---- */

static void tv_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_textview *tv =
        (const fdk_textview *)(const void *)w;
    if (tv->read_only) {
        out->states |= FDK_A11Y_READ_ONLY;
    } else {
        out->states |= FDK_A11Y_EDITABLE;
    }
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = (double)tv->len;
    out->value_current = (double)tv->len;
    out->value_text = fdk__strdup(tv->text);
}

static size_t tv_text_length(const fdk_widget *w) {
    const fdk_textview *tv =
        (const fdk_textview *)(const void *)w;
    return tv->len;
}

static size_t tv_text_caret(const fdk_widget *w) {
    const fdk_textview *tv =
        (const fdk_textview *)(const void *)w;
    return tv->caret;
}

static bool tv_text_selection(const fdk_widget *w, size_t *anchor,
                              size_t *caret) {
    const fdk_textview *tv =
        (const fdk_textview *)(const void *)w;
    if (tv->anchor == tv->caret) {
        return false;
    }
    *anchor = tv->anchor;
    *caret = tv->caret;
    return true;
}

static bool tv_text_at(const fdk_widget *w, size_t offset,
                       fdk_a11y_text_granularity granularity,
                       char *buf, size_t cap, size_t *out_start,
                       size_t *out_end) {
    const fdk_textview *tv =
        (const fdk_textview *)(const void *)w;
    if (tv->len == 0 || offset > tv->len) {
        return false;
    }
    if (tv->len == 0) {
        return false;
    }
    size_t start = 0, end = tv->len;
    switch (granularity) {
    case FDK_A11Y_TEXT_CHAR: {
        /* The codepoint containing (or ending at) offset. */
        size_t o = (offset < tv->len) ? offset : tv->len - 1;
        while (o > 0 &&
               tv_utf8_is_cont((unsigned char)tv->text[o])) {
            o--;
        }
        start = o;
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(tv->text, tv->len, o, &cp);
        end = o + (size_t)(n > 0 ? n : 1);
        break;
    }
    case FDK_A11Y_TEXT_WORD:
        tv_word_range(tv->text, tv->len, offset, &start, &end);
        break;
    case FDK_A11Y_TEXT_LINE:
        tv_paragraph_range(tv->text, tv->len, offset, &start, &end);
        break;
    default:
        return false;
    }
    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
    if (buf != NULL && cap > 0) {
        size_t n = end - start;
        if (n > cap - 1) {
            n = cap - 1;
        }
        memcpy(buf, tv->text + start, n);
        buf[n] = '\0';
    }
    return true;
}

static bool tv_text_set_caret(fdk_widget *w, size_t offset) {
    fdk_textview *tv = (fdk_textview *)(void *)w;
    if (offset > tv->len) {
        return false;
    }
    tv_set_selection(tv, offset, offset);
    tv_scroll_to_caret(tv);
    return true;
}

static bool tv_text_set_selection(fdk_widget *w, size_t anchor,
                                  size_t caret) {
    fdk_textview *tv = (fdk_textview *)(void *)w;
    if (anchor > tv->len || caret > tv->len) {
        return false;
    }
    tv_set_selection(tv, anchor, caret);
    tv_scroll_to_caret(tv);
    return true;
}

static const fdk_a11y_class tv_a11y = {
    .role = FDK_A11Y_ROLE_TEXT_VIEW,
    .describe = tv_a11y_describe,
    .text_length = tv_text_length,
    .text_caret = tv_text_caret,
    .text_selection = tv_text_selection,
    .text_at = tv_text_at,
    .text_set_caret = tv_text_set_caret,
    .text_set_selection = tv_text_set_selection,
};

const fdk_widget_class fdk_textview_class_def = {
    .size = sizeof(fdk_textview),
    .name = "textview",
    .handle_event = tv_handle_event,
    .paint = tv_view_paint,
    .measure = tv_view_measure,
    .arrange = tv_view_arrange,
    .destroy = tv_view_destroy,
    .a11y = &tv_a11y,
};

/* ---- public API ---- */

fdk_result fdk_textview_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_textview) {
    if (out_textview == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_textview_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_textview *tv = tv_of(w);
    tv->font = font;
    tv->text = fdk_alloc(64);
    if (tv->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    tv->text[0] = '\0';
    tv->cap = 64;
    tv->caret = 0;
    tv->anchor = 0;
    tv->wrap = FDK_TEXTVIEW_WRAP_WORD;
    tv->pad = fdk_theme_get_metric(NULL, FDK_TM_TEXTVIEW_PAD);
    tv->wrap_width = -1;
    tv->line_h = 20;
    tv->doc_w = TV_MIN_W;
    tv->doc_h = TV_MIN_H;
    fdk_widget_set_can_focus(w, true);

    r = fdk_scrollview_create(w, &tv->scroll);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    r = fdk_widget_create(tv->scroll, &fdk_tv_page_class_def,
                          (fdk_rect){0, 0, 0, 0}, &tv->page);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    (void)fdk_scrollview_set_content(tv->scroll, tv->page);

    fdk_widget_child_layout_changed(w->parent);
    *out_textview = w;
    return FDK_OK;
}

void fdk_textview_set_text(fdk_widget *textview, const char *utf8) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(textview);
    size_t n = (utf8 != NULL) ? strlen(utf8) : 0;
    if (n > TV_MAX_TEXT) {
        n = TV_MAX_TEXT; /* the programmatic reset is honest, not
                          * undoable — clamping beats refusing here */
    }
    if (!fdk_ok(tv_ensure_cap(tv, n + 1))) {
        return;
    }
    if (n > 0) {
        memcpy(tv->text, utf8, n);
    }
    tv->text[n] = '\0';
    tv->len = n;
    tv->caret = 0;
    tv->anchor = 0;
    /* The reset clears the history (the Entry's contract: the
     * programmatic text swap is not a user gesture). */
    if (tv->undo != NULL) {
        fdk_undo_stack_destroy(tv->undo);
        tv->undo = NULL;
    }
    tv->undo_group_pending = false;
    tv_lines_invalid(tv);
    tv_relayout(tv);
    fdk_widget_invalidate(textview);
    fdk__a11y_notify(textview, FDK_A11Y_VALUE_CHANGED, 0);
    if (tv->on_changed != NULL) {
        tv->on_changed(textview, tv->on_changed_data);
    }
}

const char *fdk_textview_get_text(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return "";
    }
    return tv_of(textview)->text;
}

size_t fdk_textview_line_count(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return 0;
    }
    return tv_of(textview)->line_count;
}

size_t fdk_textview_paragraph_count(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return 0;
    }
    fdk_textview *tv = tv_of(textview);
    size_t n = 1;
    for (size_t i = 0; i < tv->len; i++) {
        if (tv->text[i] == '\n') {
            n++;
        }
    }
    return n;
}

void fdk_textview_set_wrap(fdk_widget *textview,
                           fdk_textview_wrap wrap) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(textview);
    if (tv->wrap == wrap) {
        return;
    }
    tv->wrap = wrap;
    tv_lines_invalid(tv);
    tv_relayout(tv);
    fdk_widget_invalidate(textview);
}

void fdk_textview_set_read_only(fdk_widget *textview, bool read_only) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(textview);
    tv->read_only = read_only;
    fdk_widget_invalidate(textview);
}

bool fdk_textview_get_read_only(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return false;
    }
    return tv_of(textview)->read_only;
}

bool fdk_textview_get_selection(fdk_widget *textview,
                                size_t *anchor, size_t *caret) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return false;
    }
    fdk_textview *tv = tv_of(textview);
    if (tv->anchor == tv->caret) {
        return false;
    }
    if (anchor != NULL) {
        *anchor = tv->anchor;
    }
    if (caret != NULL) {
        *caret = tv->caret;
    }
    return true;
}

void fdk_textview_select_all(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(textview);
    tv_set_selection(tv, 0, tv->len);
}

void fdk_textview_set_on_changed(fdk_widget *textview,
                                 fdk_entry_changed_fn fn,
                                 void *user_data) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return;
    }
    fdk_textview *tv = tv_of(textview);
    tv->on_changed = fn;
    tv->on_changed_data = user_data;
}

fdk_result fdk_textview_set_preedit(fdk_widget *textview,
                                    const char *preedit) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_textview *tv = tv_of(textview);
    if (preedit == NULL || preedit[0] == '\0') {
        fdk_free(tv->preedit);
        tv->preedit = NULL;
    } else {
        char *copy = fdk__strdup(preedit);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_free(tv->preedit);
        tv->preedit = copy;
    }
    tv_scroll_to_caret(tv);
    fdk_widget_invalidate(textview);
    return FDK_OK;
}

fdk_result fdk_textview_undo(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_textview *tv = tv_of(textview);
    if (tv->undo == NULL) {
        return FDK_OK; /* nothing ever recorded */
    }
    return fdk_undo_stack_undo(tv->undo);
}

fdk_result fdk_textview_redo(fdk_widget *textview) {
    if (textview == NULL ||
        textview->klass != &fdk_textview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_textview *tv = tv_of(textview);
    if (tv->undo == NULL) {
        return FDK_OK;
    }
    return fdk_undo_stack_redo(tv->undo);
}
