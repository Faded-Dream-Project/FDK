#define FDK_LOG_TAG "widgets"

/*
 * entry.c — single-line text Entry (Phase 9)
 *
 * A UTF-8 text field with a byte-offset caret that is ALWAYS on a
 * codepoint boundary, a selection [anchor, caret), clipboard
 * integration (Ctrl+X/C/V through the owning window's context),
 * word-wise motion/selection, double/triple-click word/field select,
 * horizontal scrolling that keeps the caret visible, and the IME
 * GROUNDWORK surface: a preedit string API that renders inline at
 * the caret with an underline. (Real input-method integration —
 * XIM preedit callbacks on X11, zwp_text_input on Wayland — is
 * deliberately out of scope; the API here is what such a layer will
 * drive, and it is exercised by the tests.)
 *
 * Editing model: all mutations funnel through entry_set_text_internal
 * (one allocation strategy: grow-doubling, one 64 KiB cap per the
 * bounded-input rule in docs/security.md) and then normalize the
 * caret/anchor (clamped to [0, len], snapped to codepoint
 * boundaries), fire on_changed once, and re-scroll.
 */

#include "widgets_internal.h"
#include "text/text_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "window/window_internal.h"

#include "fdk/fdk_clipboard.h"

#include <time.h>

#define ENTRY_PAD_X 8
#define ENTRY_MIN_W 32
#define ENTRY_MIN_H 16
#define ENTRY_MAX_TEXT (64u * 1024u) /* bounded input (security.md) */
#define ENTRY_DBLCLICK_MS 400
#define ENTRY_DBLCLICK_SLOP 4

typedef struct fdk_entry {
    fdk_widget base;
    fdk_font *font;        /* borrowed */
    char *text;            /* owned, NUL-terminated UTF-8, never NULL */
    size_t len;            /* byte length of text */
    size_t cap;            /* allocation size (>= len + 1)           */
    size_t caret;          /* byte offset, codepoint boundary        */
    size_t anchor;         /* selection anchor (== caret: no sel)     */
    char *preedit;         /* owned, NULL when no preedit             */
    size_t preedit_len;
    fdk_entry_changed_fn on_changed;
    void *on_changed_data;
    fdk_entry_activate_fn on_activate;
    void *on_activate_data;
    fdk_i32 x_offset;      /* text scroll, pixels, >= 0              */
    int selecting;         /* pointer drag in progress               */
    /* Click-count tracking for double/triple selection (monotonic
     * clock, same discipline as the title bar's double-click). */
    fdk_i64 last_click_ms;
    fdk_f32 last_click_x, last_click_y;
    int click_count;       /* 1 = single, 2 = double, 3+ = triple   */
} fdk_entry;

static fdk_entry *entry_of(fdk_widget *w) {
    return (fdk_entry *)(void *)w;
}

/* ---- UTF-8 boundary helpers (stepping shares text.c's decoder) ---- */

static bool utf8_is_cont(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

/* Byte offset of the previous codepoint start before `i` (i on a
 * boundary). Scans back at most 4 bytes. */
static size_t utf8_prev(const char *s, size_t i) {
    if (i == 0) {
        return 0;
    }
    size_t j = i - 1;
    size_t back = 0;
    while (j > 0 && utf8_is_cont((unsigned char)s[j]) && back < 3) {
        j--;
        back++;
    }
    return j;
}

/* True when i is a codepoint boundary of s[0..len). */
static bool utf8_at_boundary(const char *s, size_t len, size_t i) {
    if (i >= len) {
        return i == len;
    }
    return !utf8_is_cont((unsigned char)s[i]);
}

/* Clamps *io_i to [0, len] and scans forward to a boundary. */
static size_t snap_boundary(const char *s, size_t len, size_t i) {
    if (len == 0) {
        return 0;
    }
    if (i > len) {
        i = len;
    }
    while (i < len && !utf8_at_boundary(s, len, i)) {
        i++;
    }
    return i;
}

/* Word boundary for double-click selection: a "word" is a maximal
 * run of codepoints that are all whitespace or all non-whitespace.
 * Returns the boundary offsets of the word containing byte offset
 * `i` (already a boundary). */
static void word_range(const char *s, size_t len, size_t i,
                       size_t *out_start, size_t *out_end) {
    if (len == 0 || i >= len) {
        *out_start = len;
        *out_end = len;
        return;
    }
    fdk_u32 cp = 0;
    (void)fdk_text_utf8_next(s, len, i, &cp);
    bool in_ws = (cp == ' ' || cp == '\t');
    size_t start = i;
    while (start > 0) {
        size_t prev = utf8_prev(s, start);
        fdk_u32 pcp = 0;
        (void)fdk_text_utf8_next(s, len, prev, &pcp);
        bool ws = (pcp == ' ' || pcp == '\t');
        if (ws != in_ws) {
            break;
        }
        start = prev;
    }
    size_t end = i;
    while (end < len) {
        fdk_u32 ecp = 0;
        int n = fdk_text_utf8_next(s, len, end, &ecp);
        bool ws = (ecp == ' ' || ecp == '\t');
        if (ws != in_ws) {
            break;
        }
        end += (size_t)n;
    }
    *out_start = start;
    *out_end = end;
}

/* ---- geometry ---- */

/* Cumulative advance width (px) of s[0..i) — an O(n) walk through
 * the glyph cache; n is bounded by the entry cap. Glyph advances
 * are subpixel floats; the entry rounds the SUM once (integer
 * caret geometry, float-consistent accumulation). */
static fdk_i32 text_width_to(fdk_font *font, const char *s, size_t len,
                             size_t i) {
    if (font == NULL || i == 0) {
        return 0;
    }
    fdk_f32 x = 0.0f;
    size_t at = 0;
    while (at < i) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(s, len, at, &cp);
        const fdk_glyph *g = fdk_text_glyph_for(font, cp);
        x += g->advance;
        at += (size_t)n;
    }
    return (fdk_i32)(x + 0.5f);
}

/* Hit-test: the boundary whose advance is closest to local x. */
static size_t offset_at_x(fdk_entry *e, fdk_f32 local_x) {
    if (e->font == NULL || e->len == 0) {
        return 0;
    }
    fdk_f32 x = local_x - (fdk_f32)ENTRY_PAD_X + (fdk_f32)e->x_offset;
    if (x <= 0.0f) {
        return 0;
    }
    fdk_f32 acc = 0.0f;
    size_t at = 0;
    while (at < e->len) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(e->text, e->len, at, &cp);
        const fdk_glyph *g = fdk_text_glyph_for(e->font, cp);
        fdk_f32 next_acc = acc + g->advance;
        if (x < acc + (next_acc - acc) * 0.5f) {
            return at; /* closer to this boundary than the next */
        }
        acc = next_acc;
        at += (size_t)n;
    }
    return e->len;
}

/* Scroll so the caret (or the preedit end, when active) is visible;
 * called after every caret move / edit / resize. */
static void entry_scroll_to_caret(fdk_entry *e) {
    fdk_i32 w = e->base.bounds.width;
    if (w <= 0) {
        return;
    }
    fdk_i32 view = w - ENTRY_PAD_X * 2;
    if (view <= 0) {
        e->x_offset = 0;
        return;
    }
    fdk_i32 caret_x = text_width_to(e->font, e->text, e->len, e->caret);
    fdk_i32 preedit_w = 0;
    if (e->preedit_len > 0 && e->font != NULL) {
        fdk_i32 pw = 0, ph = 0;
        fdk__text_extent(e->font, e->preedit, &pw, &ph);
        preedit_w = pw;
    }
    fdk_i32 vis_end = caret_x + preedit_w;
    if (caret_x - e->x_offset < 0) {
        e->x_offset = caret_x;
    } else if (vis_end - e->x_offset > view) {
        e->x_offset = vis_end - view;
    }
    if (e->x_offset < 0) {
        e->x_offset = 0;
    }
    /* Don't scroll past the end with big slack: if everything fits,
     * snap to 0. */
    fdk_i32 total = 0, th = 0;
    fdk__text_extent(e->font, e->text, &total, &th);
    if (total <= view) {
        e->x_offset = 0;
    } else if (e->x_offset > total - view) {
        e->x_offset = total - view;
    }
}

/* ---- buffer management ---- */

static fdk_result entry_ensure_cap(fdk_entry *e, size_t need) {
    if (need <= e->cap) {
        return FDK_OK;
    }
    /* `need` includes the NUL terminator; the 64 KiB cap is on TEXT
     * bytes, so the allocation may legitimately reach MAX+1. */
    if (need > ENTRY_MAX_TEXT + 1) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    size_t cap = (e->cap == 0) ? 32 : e->cap;
    while (cap < need) {
        if (cap > ENTRY_MAX_TEXT + 1) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        cap *= 2;
    }
    char *grown = fdk_realloc(e->text, cap);
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    e->text = grown;
    e->cap = cap;
    return FDK_OK;
}

/* Replace [from, to) with insert (insert_len bytes, may be NULL/0).
 * Maintains every invariant and fires on_changed. Returns FDK_ERR_*,
 * leaves the entry untouched on failure. */
static fdk_result entry_splice(fdk_entry *e, size_t from, size_t to,
                               const char *insert, size_t insert_len) {
    if (from > to || to > e->len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (insert == NULL) {
        insert_len = 0;
    }
    size_t new_len = e->len - (to - from) + insert_len;
    if (new_len > ENTRY_MAX_TEXT) {
        FDK_WARN("entry: 64 KiB text limit reached; insert refused");
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = entry_ensure_cap(e, new_len + 1);
    if (!fdk_ok(r)) {
        return r;
    }
    /* Move the tail first (memmove handles overlap), then the insert. */
    memmove(e->text + from + insert_len, e->text + to, e->len - to);
    if (insert_len > 0) {
        memcpy(e->text + from, insert, insert_len);
    }
    e->text[new_len] = '\0';
    e->len = new_len;
    e->caret = from + insert_len; /* caret lands after the insert */
    e->anchor = e->caret;
    entry_scroll_to_caret(e);
    fdk_widget_invalidate(&e->base);
    fdk_widget_child_layout_changed(e->base.parent);
    if (e->on_changed != NULL) {
        e->on_changed(&e->base, e->on_changed_data);
    }
    return FDK_OK;
}

/* ---- clipboard ---- */

static void entry_clipboard_copy(fdk_entry *e) {
    if ((e->base.flags & FDK_WF_ENABLED) == 0) {
        return; /* disabled entries do not touch the clipboard */
    }
    if (e->caret == e->anchor) {
        return; /* copying an empty selection is a no-op */
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return; /* standalone tree: no clipboard to talk to */
    }
    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    char saved = e->text[hi];
    e->text[hi] = '\0';
    (void)fdk_clipboard_set_text(ctx, e->text + lo);
    e->text[hi] = saved;
}

static void entry_clipboard_cut(fdk_entry *e) {
    /* Cut = copy + delete, but ONLY when the copy actually went
     * somewhere: with no owning window (standalone trees) there is no
     * clipboard, and deleting unsaved text would be data loss — the
     * shortcut is inert instead. */
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return;
    }
    entry_clipboard_copy(e);
    if (e->caret != e->anchor) {
        size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
        size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
        (void)entry_splice(e, lo, hi, NULL, 0);
    }
}

static void entry_clipboard_paste(fdk_entry *e) {
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return;
    }
    char *clip = fdk_clipboard_get_text(ctx);
    if (clip == NULL) {
        return;
    }
    /* Replace the selection (if any) with the pasted text. */
    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    fdk_result r = entry_splice(e, lo, hi, clip, strlen(clip));
    fdk_free(clip);
    (void)r; /* oversized paste is refused with a warning; fine */
}

/* ---- selection helpers ---- */

static void entry_delete_selection(fdk_entry *e) {
    if (e->caret == e->anchor) {
        return;
    }
    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    (void)entry_splice(e, lo, hi, NULL, 0);
}

static bool entry_has_selection(const fdk_entry *e) {
    return e->caret != e->anchor;
}

/* ---- events ---- */

static fdk_i64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

/* THE selection mutation: sets anchor+caret (both byte offsets,
 * snapped to codepoint boundaries), scrolls to keep the caret
 * visible, and invalidates when EITHER endpoint moved. All callers
 * route through here so programmatic selection changes (select_all,
 * select_range) can never silently skip a repaint — the exact bug
 * the entry paint tests caught in the first cut. */
static void entry_set_selection(fdk_entry *e, size_t anchor, size_t caret) {
    size_t old_caret = e->caret;
    size_t old_anchor = e->anchor;
    e->caret = snap_boundary(e->text, e->len, caret);
    e->anchor = snap_boundary(e->text, e->len, anchor);
    entry_scroll_to_caret(e);
    if (e->caret != old_caret || e->anchor != old_anchor) {
        fdk_widget_invalidate(&e->base);
    }
}

/* Caret motion: extend=true keeps the anchor (shift+arrows);
 * extend=false collapses to a caret. */
static void entry_move_caret(fdk_entry *e, size_t to, bool extend) {
    entry_set_selection(e, extend ? e->anchor : to, to);
}

static size_t word_motion_forward(const char *s, size_t len, size_t i) {
    size_t ws = 0, we = 0;
    word_range(s, len, (i < len) ? i : len, &ws, &we);
    if (i < we && we > i) {
        return we; /* jump to the end of the current word */
    }
    return (i < len) ? snap_boundary(s, len, i + 1) : len;
}

static size_t word_motion_backward(const char *s, size_t len, size_t i) {
    if (i == 0) {
        return 0;
    }
    size_t prev = utf8_prev(s, i);
    size_t ws = 0, we = 0;
    word_range(s, len, prev, &ws, &we);
    if (ws < i) {
        return ws;
    }
    return prev;
}

static bool entry_handle_event(fdk_widget *w,
                               const fdk_widget_event *ev) {
    fdk_entry *e = entry_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        fdk_i64 now = now_ms();
        fdk_f32 dx = ev->pointer.position.x - e->last_click_x;
        fdk_f32 dy = ev->pointer.position.y - e->last_click_y;
        if (now - e->last_click_ms <= ENTRY_DBLCLICK_MS &&
            dx >= -ENTRY_DBLCLICK_SLOP && dx <= ENTRY_DBLCLICK_SLOP &&
            dy >= -ENTRY_DBLCLICK_SLOP && dy <= ENTRY_DBLCLICK_SLOP) {
            e->click_count++;
        } else {
            e->click_count = 1;
        }
        e->last_click_ms = now;
        e->last_click_x = ev->pointer.position.x;
        e->last_click_y = ev->pointer.position.y;

        size_t hit = offset_at_x(e, ev->pointer.position.x);
        if (e->click_count == 2) {
            /* Word select: the word bounds become the selection. */
            size_t ws = 0, we = 0;
            word_range(e->text, e->len, hit, &ws, &we);
            entry_set_selection(e, ws, we);
            e->selecting = 1;
        } else if (e->click_count >= 3) {
            /* Triple: select the whole field. */
            entry_set_selection(e, 0, e->len);
            e->selecting = 1;
        } else {
            /* Plain click: caret to hit; the anchor collapses (the
             * v1 pointer event carries no modifiers, so shift-click
             * EXTENSION is not expressible yet — documented in
             * fdk_widgets.h; shift+arrows does the extending). */
            entry_set_selection(e, hit, hit);
            e->selecting = 1;
        }
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        if (e->selecting) {
            size_t hit = offset_at_x(e, ev->position.x);
            entry_move_caret(e, hit, true);
            return true;
        }
        return false;
    }
    case FDK_WIDGET_POINTER_UP:
        e->selecting = 0;
        return true;
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        const fdk_key_event *key = &ev->key;
        fdk_u32 mods = key->modifiers;
        bool shift = (mods & FDK_MOD_SHIFT) != 0;
        bool ctrl = (mods & FDK_MOD_CTRL) != 0;

        /* Clipboard shortcuts. */
        if (ctrl && !shift) {
            if (key->codepoint == 'c' || key->codepoint == 'C') {
                entry_clipboard_copy(e);
                return true;
            }
            if (key->codepoint == 'x' || key->codepoint == 'X') {
                entry_clipboard_cut(e);
                return true;
            }
            if (key->codepoint == 'v' || key->codepoint == 'V') {
                entry_clipboard_paste(e);
                return true;
            }
            if (key->codepoint == 'a' || key->codepoint == 'A') {
                entry_set_selection(e, 0, e->len);
                return true;
            }
        }

        switch (key->scancode) {
        case FDK_KEY_LEFT:
            if (ctrl) {
                entry_move_caret(e, word_motion_backward(e->text, e->len,
                                                         e->caret),
                                 shift);
            } else {
                entry_move_caret(e, utf8_prev(e->text, e->caret), shift);
            }
            return true;
        case FDK_KEY_RIGHT:
            if (ctrl) {
                entry_move_caret(
                    e, word_motion_forward(e->text, e->len, e->caret),
                    shift);
            } else if (e->caret < e->len) {
                fdk_u32 cp = 0;
                int n = fdk_text_utf8_next(e->text, e->len, e->caret,
                                           &cp);
                entry_move_caret(e, e->caret + (size_t)n, shift);
            }
            return true;
        case FDK_KEY_HOME:
            entry_move_caret(e, 0, shift);
            return true;
        case FDK_KEY_END:
            entry_move_caret(e, e->len, shift);
            return true;
        case FDK_KEY_BACKSPACE:
            if (entry_has_selection(e)) {
                entry_delete_selection(e);
            } else if (e->caret > 0) {
                size_t prev = utf8_prev(e->text, e->caret);
                (void)entry_splice(e, prev, e->caret, NULL, 0);
            }
            return true;
        case FDK_KEY_DELETE:
            if (entry_has_selection(e)) {
                entry_delete_selection(e);
            } else if (e->caret < e->len) {
                fdk_u32 cp = 0;
                int n = fdk_text_utf8_next(e->text, e->len, e->caret,
                                           &cp);
                (void)entry_splice(e, e->caret, e->caret + (size_t)n,
                                   NULL, 0);
            }
            return true;
        case FDK_KEY_ENTER:
            if (e->on_activate != NULL) {
                e->on_activate(&e->base, e->on_activate_data);
            }
            return true;
        case FDK_KEY_ESC:
            /* Collapse the selection (classic cancel behavior). */
            entry_set_selection(e, e->caret, e->caret);
            return true;
        default:
            break;
        }

        /* Textual insert: any codepoint the platform resolved,
         * excluding control characters (they are not text). */
        if (key->codepoint >= 0x20u && !ctrl) {
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
            entry_delete_selection(e);
            (void)entry_splice(e, e->caret, e->caret, buf, n);
            return true;
        }
        return false;
    }
    default:
        break;
    }
    return false;
}

/* ---- paint ---- */

static void entry_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_entry *e = entry_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    fdk_color fill;
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        fill = fdk__pal_control_disabled();
    } else if ((w->flags & FDK_WF_FOCUSED) != 0) {
        /* Focused entries read as "active": the window background
         * token, the field convention the v1 palette supports. */
        fill = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    } else {
        fill = fdk__pal_control();
    }
    fdk_i32 radius = fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);

    if ((w->flags & FDK_WF_FOCUSED) != 0) {
        fdk_rect ring = {bounds.x + 2, bounds.y + 2,
                         bounds.width - 4, bounds.height - 4};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 rr = radius > 2 ? radius - 2 : 0;
            fdk_surface_draw_rounded_rect(surface, ring, rr,
                                          fdk__pal_accent());
        }
    }

    if (e->font == NULL) {
        return; /* textless (matches Label's no-font degradation) */
    }

    fdk_color text_col = ((w->flags & FDK_WF_ENABLED) == 0)
        ? fdk__pal_text_disabled()
        : fdk__pal_text();
    fdk_i32 baseline = fdk__center_baseline(e->font, bounds.y,
                                            bounds.height);
    fdk_i32 text_x = bounds.x + ENTRY_PAD_X - e->x_offset;

    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    bool has_sel = (lo != hi) && (w->flags & FDK_WF_ENABLED) != 0;

    /* Selection highlight behind the selected segment. */
    if (has_sel) {
        fdk_i32 sel_x = text_x + text_width_to(e->font, e->text, e->len,
                                               lo);
        fdk_i32 sel_w = text_width_to(e->font, e->text, e->len, hi) -
                        text_width_to(e->font, e->text, e->len, lo);
        fdk_rect sel = {sel_x, bounds.y + 2, sel_w,
                        bounds.height - 4};
        if (sel_w > 0 && sel.height > 0) {
            /* Accent at low alpha over the field fill: the v1 way to
             * say "selected" without a dedicated token. */
            fdk_color accent = fdk__pal_accent();
            fdk_color hl = {accent.r, accent.g, accent.b, 0.45f};
            fdk_surface_fill_rect(surface, sel, hl);
        }
    }

    /* Text in three segments — [0,lo) [lo,hi) [hi,len) — drawn by
     * temporarily NUL-terminating at each boundary. The selected
     * middle keeps the plain text color: the accent highlight rect
     * already says "selected" (no inverted text in the v1 theme). */
    if (e->len > 0) {
        char saved_lo = e->text[lo];
        char saved_hi = e->text[hi];
        fdk_i32 w_lo = text_width_to(e->font, e->text, e->len, lo);
        fdk_i32 w_hi = text_width_to(e->font, e->text, e->len, hi);
        e->text[lo] = '\0';
        fdk__draw_text(surface, e->font, e->text, text_col, text_x,
                       baseline);
        e->text[lo] = saved_lo;
        e->text[hi] = '\0';
        fdk__draw_text(surface, e->font, e->text + lo, text_col,
                       text_x + w_lo, baseline);
        e->text[hi] = saved_hi;
        fdk__draw_text(surface, e->font, e->text + hi, text_col,
                       text_x + w_hi, baseline);
    }

    /* Preedit (IME groundwork): rendered AT the caret, underlined,
     * with the visual caret parked at its end while active. */
    fdk_i32 caret_x = text_x + text_width_to(e->font, e->text, e->len,
                                             e->caret);
    if (e->preedit_len > 0) {
        fdk_i32 pw = 0, ph = 0;
        fdk__text_extent(e->font, e->preedit, &pw, &ph);
        fdk__draw_text(surface, e->font, e->preedit, text_col, caret_x,
                       baseline);
        /* Underline: a 1px accent band under the preedit run. */
        fdk_rect ul = {caret_x,
                       baseline + 2,
                       pw, 1};
        if (ul.width > 0) {
            fdk_surface_fill_rect(surface, ul, fdk__pal_accent());
        }
        caret_x += pw;
    }

    /* Caret: 1px vertical bar, drawn only when enabled and focused. */
    if ((w->flags & FDK_WF_ENABLED) != 0 &&
        (w->flags & FDK_WF_FOCUSED) != 0) {
        fdk_rect bar = {caret_x, bounds.y + 3, 1, bounds.height - 6};
        if (bar.height > 0) {
            fdk_surface_fill_rect(surface, bar, text_col);
        }
    }
}

/* ---- measure / destroy ---- */

static void entry_measure(fdk_widget *w, fdk_size *out) {
    fdk_entry *e = entry_of(w);
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(e->font, e->text, &tw, &th);
    out->width = tw + ENTRY_PAD_X * 2;
    out->height = th + ENTRY_PAD_X; /* tighter vertically */
    if (out->width < ENTRY_MIN_W) {
        out->width = ENTRY_MIN_W;
    }
    if (out->height < ENTRY_MIN_H) {
        out->height = ENTRY_MIN_H;
    }
}

static void entry_destroy(fdk_widget *w) {
    fdk_entry *e = entry_of(w);
    fdk_free(e->text);
    fdk_free(e->preedit);
}

static const fdk_widget_class fdk_entry_class_def = {
    .size = sizeof(fdk_entry),
    .name = "entry",
    .handle_event = entry_handle_event,
    .paint = entry_paint,
    .measure = entry_measure,
    .arrange = NULL,
    .destroy = entry_destroy,
};

/* ---- public API ---- */

fdk_result fdk_entry_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_entry) {
    if (out_entry == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_entry_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_entry *e = entry_of(w);
    e->font = font;
    const char *init = (text != NULL) ? text : "";
    size_t len = strlen(init);
    if (len > ENTRY_MAX_TEXT) {
        fdk_widget_destroy(w);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    r = entry_ensure_cap(e, len + 1);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    memcpy(e->text, init, len + 1);
    e->len = len;
    e->caret = len;
    e->anchor = len;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_entry = w;
    return FDK_OK;
}

const char *fdk_entry_get_text(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return NULL;
    }
    return entry_of(entry)->text;
}

fdk_result fdk_entry_set_text(fdk_widget *entry, const char *text) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    const char *set = (text != NULL) ? text : "";
    size_t len = strlen(set);
    if (len > ENTRY_MAX_TEXT) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = entry_ensure_cap(e, len + 1);
    if (!fdk_ok(r)) {
        return r;
    }
    memcpy(e->text, set, len + 1);
    e->len = len;
    e->caret = len;
    e->anchor = len;
    entry_scroll_to_caret(e);
    fdk_widget_invalidate(entry);
    fdk_widget_child_layout_changed(entry->parent);
    if (e->on_changed != NULL) {
        e->on_changed(entry, e->on_changed_data);
    }
    return FDK_OK;
}

size_t fdk_entry_get_cursor(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return 0;
    }
    return entry_of(entry)->caret;
}

fdk_result fdk_entry_set_cursor(fdk_widget *entry, size_t byte_offset) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    if (byte_offset > e->len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!utf8_at_boundary(e->text, e->len, byte_offset)) {
        return FDK_ERR_INVALID_ARGUMENT; /* mid-codepoint offsets are
                                            refused, not snapped */
    }
    entry_move_caret(e, byte_offset, false);
    return FDK_OK;
}

fdk_result fdk_entry_get_selection(fdk_widget *entry, size_t *anchor,
                                   size_t *caret) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def ||
        anchor == NULL || caret == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    *anchor = e->anchor;
    *caret = e->caret;
    return FDK_OK;
}

fdk_result fdk_entry_select_range(fdk_widget *entry, size_t anchor,
                                  size_t caret) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    if (anchor > e->len || caret > e->len ||
        !utf8_at_boundary(e->text, e->len, anchor) ||
        !utf8_at_boundary(e->text, e->len, caret)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    entry_set_selection(e, anchor, caret);
    return FDK_OK;
}

void fdk_entry_select_all(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    entry_set_selection(e, 0, e->len);
}

fdk_result fdk_entry_set_preedit(fdk_widget *entry, const char *preedit) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    char *copy = NULL;
    if (preedit != NULL && preedit[0] != '\0') {
        size_t len = strlen(preedit);
        if (len > ENTRY_MAX_TEXT) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        copy = fdk_alloc(len + 1);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, preedit, len + 1);
    }
    fdk_free(e->preedit);
    e->preedit = copy;
    e->preedit_len = (copy != NULL) ? strlen(copy) : 0;
    entry_scroll_to_caret(e);
    fdk_widget_invalidate(entry);
    return FDK_OK;
}

void fdk_entry_set_on_changed(fdk_widget *entry,
                              fdk_entry_changed_fn on_changed,
                              void *user_data) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    e->on_changed = on_changed;
    e->on_changed_data = user_data;
}

void fdk_entry_set_on_activate(fdk_widget *entry,
                               fdk_entry_activate_fn on_activate,
                               void *user_data) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    e->on_activate = on_activate;
    e->on_activate_data = user_data;
}
