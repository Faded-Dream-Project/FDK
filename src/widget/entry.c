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
#include "core/undo_internal.h"
#include "window/window_internal.h"

#include "fdk/fdk_clipboard.h"
#include "fdk/fdk_undo.h"

#include <time.h>

#define ENTRY_PAD_X 8
#define ENTRY_MIN_W 32
#define ENTRY_MIN_H 16
#define ENTRY_MAX_TEXT (64u * 1024u) /* bounded input (security.md) */
#define ENTRY_DBLCLICK_MS 400
#define ENTRY_DBLCLICK_SLOP 4
/* 1.4.2 search preset: the magnifier's glyph zone before the text
 * and the clear button's zone after it (16-px glyph box + 6-px gap,
 * the symbolic-row-icon geometry). */
#define ENTRY_SEARCH_GLYPH 16
#define ENTRY_SEARCH_GLYPH_GAP 6
#define ENTRY_SEARCH_CLEAR (ENTRY_SEARCH_GLYPH + ENTRY_SEARCH_GLYPH_GAP)
/* Caret blink cadence (1.3.1, timers): GTK's 530ms, the desktop
 * convention — two full blinks per second reads as "alive" without
 * distracting. The phase RESETS on every caret move/edit (the same
 * restart-on-activity rule GTK applies). */
#define ENTRY_BLINK_MS 530

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
    char *placeholder;     /* owned, NULL when unset (1.4.0)         */
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
    bool password;         /* render bullets, not glyphs            */
    bool read_only;        /* selection + copy yes, edits no        */
    size_t max_len;        /* editable cap in bytes; 0 = 64 KiB     */
    /* Caret blink (1.3.1): NULL when unfocused or detached from any
     * window (a detached tree has no context — the caret stays solid
     * there, which is exactly why the headless tests never see it
     * blink). */
    fdk_timer *blink_timer;
    bool caret_on;
    /* ---- Undo history (1.3.3) ----
     *
     * Lazily created on the FIRST recorded edit (display-only and
     * read-only entries never allocate one). undo_applying guards
     * entry_splice's recorder while an op's undo/redo closure is
     * re-driving the splice — those applications must fire
     * on_changed/a11y/repaint like any edit, but must not record.
     *
     * undo_group_pending marks "a selection-delete just happened and
     * the gesture it belonged to may not be finished": the typing
     * path deletes the selection and then inserts, and those two
     * splices must land on the stack as ONE replace op. The flag is
     * set by entry_delete_selection and consumed (read-and-clear)
     * by the very next record — a keystroke boundary or any other
     * record in between expires it naturally. */
    fdk_undo_stack *undo;
    bool undo_applying;
    bool undo_group_pending;
    /* ---- PRIMARY selection (1.3.4) ----
     *
     * True while THIS entry believes it last pushed a non-empty
     * text to the PRIMARY selection (the classic Unix "current
     * selection" buffer). The flag drives the polite-ownership rule:
     * collapsing an empty selection never GRABS PRIMARY (an FDK app
     * with entries must not steal the user's xterm selection at
     * startup), but a collapse after a real push empties it (the
     * classic observable: middle-click pastes nothing). Headless /
     * standalone trees never set it — no context, no pushes. */
    bool primary_pushed;
    /* ---- Search preset (1.4.2) ----
     *
     * The GtkSearchEntry face on a regular Entry: a magnifier glyph
     * before the text, a clear button (hover-pill + X) that exists
     * only while there is text, and the search Esc ladder (first
     * Escape clears, the next bubbles). Every other Entry API —
     * selection, undo, clipboard, preedit — applies unchanged. */
    bool search_mode;
    bool clear_hover;    /* the clear zone under the pointer */
} fdk_entry;

static fdk_entry *entry_of(fdk_widget *w) {
    return (fdk_entry *)(void *)w;
}

/* ---- caret blink (1.3.1) ---- */

static void entry_blink_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_entry *e = user;
    e->caret_on = !e->caret_on;
    fdk_widget_invalidate(&e->base);
}

/* Starts the blink when the entry gains focus; detached trees (no
 * window -> no context) keep the solid caret — the headless-test
 * contract stays pixel-stable. */
static void entry_blink_start(fdk_entry *e) {
    if (e->blink_timer != NULL) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return;
    }
    e->blink_timer =
        fdk_timer_add(ctx, ENTRY_BLINK_MS, true, entry_blink_tick, e);
    e->caret_on = true;
}

static void entry_blink_stop(fdk_entry *e) {
    if (e->blink_timer != NULL) {
        fdk_timer_remove(e->blink_timer);
        e->blink_timer = NULL;
    }
    e->caret_on = true; /* refocus shows the caret immediately */
}

/* Activity restart: every caret move / edit / focus event puts the
 * phase back to "visible" and re-arms the timer — the GTK rule. */
static void entry_blink_restart(fdk_entry *e) {
    if (e->blink_timer == NULL) {
        return;
    }
    if (!e->caret_on) {
        e->caret_on = true;
        fdk_widget_invalidate(&e->base);
    }
    fdk_timer_reset(e->blink_timer, ENTRY_BLINK_MS);
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

/* ---- search preset geometry (1.4.2) ----
 *
 * The text origin's inset (the magnifier zone) and the right
 * reserve (the clear button, present only while text exists — the
 * view width grows the moment the button does, which is exactly
 * when the user can see it). Hit-testing, scrolling, and paint all
 * route through these two, so the caret and the glyph never fight. */
static fdk_i32 entry_text_inset(const fdk_entry *e) {
    return ENTRY_PAD_X +
           (e->search_mode
                ? ENTRY_SEARCH_GLYPH + ENTRY_SEARCH_GLYPH_GAP
                : 0);
}

static fdk_i32 entry_right_inset(const fdk_entry *e) {
    return ENTRY_PAD_X +
           ((e->search_mode && e->len > 0) ? ENTRY_SEARCH_CLEAR : 0);
}

/* The clear zone's x extent in widget-local coordinates (the box
 * + its gap; empty when the button does not exist). */
static fdk_i32 entry_clear_zone_w(const fdk_entry *e) {
    return (e->search_mode && e->len > 0) ? ENTRY_SEARCH_CLEAR : 0;
}

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

/* ---- password mode geometry ----
 *
 * Bullets advance by ONE glyph per CLUSTER: the buffer, the caret,
 * the selection, and the hit-testing all keep working in byte/
 * cluster space — only the rendering (and its geometry) changes. */

#define ENTRY_BULLET_UTF8 "\xE2\x80\xA2" /* U+2022 BULLET */

static fdk_i32 bullet_advance(const fdk_entry *e) {
    if (e->font != NULL) {
        fdk_i32 w = 0, h = 0;
        fdk__text_extent(e->font, ENTRY_BULLET_UTF8, &w, &h);
        if (w > 0) {
            return w;
        }
    }
    return 6; /* fontless fallback bullet width */
}

/* Clusters in [0, offset). */
static size_t cluster_count(const fdk_entry *e, size_t offset) {
    size_t n = 0, i = 0;
    while (i < offset) {
        fdk_u32 cp = 0;
        int step = fdk_text_utf8_next(e->text, e->len, i, &cp);
        if (step <= 0) {
            break;
        }
        i += (size_t)step;
        n++;
    }
    return n;
}

/* Password-aware width of [0, offset). */
static fdk_i32 entry_width_to(const fdk_entry *e, size_t offset) {
    if (!e->password) {
        return text_width_to(e->font, e->text, e->len, offset);
    }
    return (fdk_i32)cluster_count(e, offset) * bullet_advance(e);
}

/* Hit-test: the boundary whose advance is closest to local x. */
static size_t offset_at_x(fdk_entry *e, fdk_f32 local_x) {
    if (e->font == NULL || e->len == 0) {
        return 0;
    }
    fdk_f32 x = local_x - (fdk_f32)entry_text_inset(e) +
                (fdk_f32)e->x_offset;
    if (x <= 0.0f) {
        return 0;
    }
    fdk_f32 acc = 0.0f;
    size_t at = 0;
    fdk_f32 bullet_adv = e->password ? (fdk_f32)bullet_advance(e) : 0.0f;
    while (at < e->len) {
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(e->text, e->len, at, &cp);
        const fdk_glyph *g = e->password ? NULL
                                         : fdk_text_glyph_for(e->font, cp);
        fdk_f32 next_acc = acc + (e->password ? bullet_adv : g->advance);
        if (x < acc + (next_acc - acc) * 0.5f) {
            return at; /* closer to this boundary than the next */
        }
        acc = next_acc;
        at += (size_t)n;
    }
    return e->len;
}

/* Scroll so the caret (or the preedit end, when active) is visible;
 * called after every caret move / edit / resize — which makes it the
 * single chokepoint for the blink phase restart as well. */
static void entry_scroll_to_caret(fdk_entry *e) {
    entry_blink_restart(e);
    fdk_i32 w = e->base.bounds.width;
    if (w <= 0) {
        return;
    }
    fdk_i32 view = w - entry_text_inset(e) - entry_right_inset(e);
    if (view <= 0) {
        e->x_offset = 0;
        return;
    }
    fdk_i32 caret_x = entry_width_to(e, e->caret);
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

/* ---- undo history (1.3.3) ---------------------------------------------
 *
 * Every mutation routes through entry_splice, so the recorder lives
 * there and sees one uniform op shape: "range [from,to) was
 * replaced by `inserted`". Pre-edit caret/selection ride along so
 * undo restores exactly where the user was.
 *
 * COALESCING — the typing rule, stated precisely: a new op merges
 * into the top of the stack when both are TYPED ops (single
 * codepoint each — pastes, cuts, selection-deletes, and replacements
 * are atomic and never merge) and:
 *
 *   insert+insert: the new insert lands exactly at the end of the
 *                  top op's inserted text (the advancing frontier).
 *                  Type "ab", arrow around, resume typing elsewhere
 *                  -> different frontier -> separate undo steps.
 *   backspace run: the new delete's range ends exactly where the
 *                  top op's range begins (contiguous backward).
 *   delete run:    the new delete's range begins exactly where the
 *                  top op's range ended (contiguous forward).
 *
 * No time component: the rule is deterministic and testable. Undoing
 * a coalesced op removes the whole run in one step, which is the
 * native feel on every platform.
 *
 * APPLICATION: undo re-splices the old text and restores the saved
 * caret/anchor (a restored deletion reselects the restored text —
 * the GTK behavior, ready to re-delete or copy); redo re-splices
 * the new text (caret lands after it, as any edit does). Both route
 * through entry_splice with undo_applying set, so on_changed, a11y,
 * and repaints fire identically to user edits.
 */

typedef struct entry_undo_op {
    fdk_entry *e;             /* the entry this op belongs to */
    size_t from, to;          /* the range that was replaced */
    char *removed;            /* owned; old text (NULL = deleted nothing) */
    char *inserted;           /* owned; new text (NULL = inserted nothing) */
    size_t caret_before;      /* pre-edit selection, restored on undo */
    size_t anchor_before;
    bool typed;               /* single-codepoint user keystroke (coalescible) */
} entry_undo_op;

/* Defined below (after the undo machinery): the mutation primitive
 * every edit — and every undo/redo application — routes through. */
static fdk_result entry_splice(fdk_entry *e, size_t from, size_t to,
                               const char *insert, size_t insert_len);
/* Also below: THE selection setter the undo paths restore through. */
static void entry_set_selection(fdk_entry *e, size_t anchor, size_t caret);

static void entry_undo_destroy(void *user) {
    entry_undo_op *op = user;
    fdk_free(op->removed);
    fdk_free(op->inserted);
    fdk_free(op);
}

/* Pure-edit application core: splice without recording. Returns the
 * splice result; the caller fixes up the selection afterwards. */
static fdk_result entry_apply_op(fdk_entry *e, size_t from, size_t to,
                                 const char *insert, size_t insert_len) {
    e->undo_applying = true;
    fdk_result r = entry_splice(e, from, to, insert, insert_len);
    e->undo_applying = false;
    return r;
}

static void entry_undo_apply(void *user) {
    entry_undo_op *op = user;
    fdk_entry *e = op->e;
    size_t ins_len = (op->inserted != NULL) ? strlen(op->inserted) : 0;
    size_t rem_len = (op->removed != NULL) ? strlen(op->removed) : 0;
    (void)entry_apply_op(e, op->from, op->from + ins_len, op->removed,
                         rem_len);
    if (rem_len > 0) {
        /* A restored deletion reselects its text (GTK behavior). */
        entry_set_selection(e, op->from, op->from + rem_len);
    } else {
        entry_set_selection(e, op->caret_before, op->anchor_before);
    }
    entry_blink_restart(e);
}

static void entry_redo_apply(void *user) {
    entry_undo_op *op = user;
    fdk_entry *e = op->e;
    size_t ins_len = (op->inserted != NULL) ? strlen(op->inserted) : 0;
    size_t rem_len = (op->removed != NULL) ? strlen(op->removed) : 0;
    /* Symmetric with entry_undo_apply: at redo time the current text
     * holds the RESTORED (removed) text over [from, from+rem_len) —
     * the redo replaces exactly that with the inserted text. (For a
     * pure insert rem_len is 0: the insert lands at `from`.) */
    (void)entry_apply_op(e, op->from, op->from + rem_len,
                         op->inserted, ins_len);
    entry_blink_restart(e);
}

/* Is `s[0..n)` exactly one UTF-8 codepoint? (the "typed" test) */
static bool utf8_single_cp(const char *s, size_t n) {
    if (s == NULL || n == 0) {
        return false;
    }
    fdk_u32 cp = 0;
    int step = fdk_text_utf8_next(s, n, 0, &cp);
    return step > 0 && (size_t)step == n;
}

/* Pushes (or coalesces into) the stack. `removed_copy` and
 * `insert_copy` are already-owned exact copies; on any allocation
 * failure the op is simply not recorded (the edit itself succeeded —
 * a degraded history beats a failed keystroke; a warning logs it). */
static void entry_undo_record(fdk_entry *e, size_t from, size_t to,
                              char *removed_copy, char *insert_copy,
                              size_t caret_before, size_t anchor_before) {
    if (e->undo == NULL) {
        if (!fdk_ok(fdk_undo_stack_create(0, &e->undo))) {
            e->undo = NULL;
            fdk_free(removed_copy);
            fdk_free(insert_copy);
            FDK_WARN("entry: undo stack unavailable; edits are not "
                     "recorded");
            return;
        }
    }
    bool typed = utf8_single_cp(insert_copy,
                                (insert_copy != NULL)
                                    ? strlen(insert_copy) : 0) &&
                 removed_copy == NULL;
    /* A single-codepoint DELETE is also a typed op (backspace/delete
     * runs coalesce); typed inserts have no removed text by
     * construction, and typed deletes have no inserted text. */
    if (insert_copy == NULL && removed_copy != NULL) {
        typed = utf8_single_cp(removed_copy, strlen(removed_copy));
    }

    /* ---- gesture grouping: select-then-type is ONE replace op ----
     *
     * entry_delete_selection set undo_group_pending after recording
     * the selection's delete; if THIS record is the insert of that
     * same keystroke (a pure insert landing exactly at the deleted
     * slot), the two ops compose into a single replace — the undo
     * step then matches the user's gesture (GTK/Qt behavior).
     * Read-and-clear: any other record shape expires the pending. */
    bool group = e->undo_group_pending;
    e->undo_group_pending = false;
    if (group && insert_copy != NULL && removed_copy == NULL &&
        fdk_undo_stack_can_undo(e->undo)) {
        const fdk_undo_op *gtop = NULL;
        if (fdk_ok(fdk_undo_stack_top(e->undo, &gtop)) && gtop != NULL) {
            entry_undo_op *gop = gtop->user_data;
            if (gop != NULL && gop->inserted == NULL &&
                gop->removed != NULL && from == gop->from) {
                entry_undo_op *merged = fdk_alloc(sizeof(*merged));
                if (merged != NULL) {
                    merged->e = e;
                    merged->from = gop->from;
                    merged->to = gop->to;
                    merged->removed = gop->removed; /* steal below */
                    merged->inserted = insert_copy;
                    merged->caret_before = gop->caret_before;
                    merged->anchor_before = gop->anchor_before;
                    merged->typed = false; /* a replace is atomic */
                    gop->removed = NULL;   /* stolen by `merged` */
                    fdk_undo_op uop = {
                        .user_data = merged,
                        .undo = entry_undo_apply,
                        .redo = entry_redo_apply,
                        .destroy = entry_undo_destroy,
                    };
                    if (fdk_ok(fdk__undo_stack_replace_top(e->undo,
                                                           &uop))) {
                        return; /* gesture composed */
                    }
                    /* Replace refused (should not happen outside
                     * reentrancy): give the strings back and fall
                     * through to a normal push of the insert. */
                    gop->removed = merged->removed;
                    fdk_free(merged);
                }
            }
        }
    }

    if (fdk_undo_stack_can_undo(e->undo)) {
        /* Peek at the top op to try coalescing — the sanctioned
         * inspection API (fdk_undo_stack_top) hands back the exact
         * op struct the application pushed; the type is ours. */
        const fdk_undo_op *top = NULL;
        if (fdk_ok(fdk_undo_stack_top(e->undo, &top)) && top != NULL) {
            entry_undo_op *top_op = top->user_data;
            if (top_op != NULL && typed && top_op->typed) {
                size_t top_ins = (top_op->inserted != NULL)
                                     ? strlen(top_op->inserted) : 0;
                size_t top_rem = (top_op->removed != NULL)
                                     ? strlen(top_op->removed) : 0;
                bool merged = false;
                if (insert_copy != NULL && top_op->inserted != NULL &&
                    removed_copy == NULL &&
                    from == top_op->from + top_ins) {
                    /* typing run: append to the top's inserted text */
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
                    /* Pure-delete runs. Two geometries extend a run:
                     *
                     * BACKSPACE (caret moves LEFT with each delete):
                     *   new.to == top.from — prepend the new text,
                     *   from moves back. "wxyz": z, y, x merge to
                     *   [1,4) "xyz".
                     *
                     * DELETE KEY (caret stays PUT, text flows left):
                     *   new.from == top.from — append; each delete
                     *   took the same slot the previous one left.
                     *   "abcd" + Del Del: a, b merge to [0,2) "ab".
                     *
                     *   The advancing-caret variant (new.from ==
                     *   top.to, successive selection deletes) is the
                     *   same append geometry. */
                    if (to == top_op->from) {
                        /* backspace run: prepend (extends backward) */
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
                        /* delete-key run: append (extends forward) */
                        size_t a = top_rem;
                        size_t b = strlen(removed_copy);
                        char *grown =
                            fdk_realloc(top_op->removed, a + b + 1);
                        if (grown != NULL) {
                            memcpy(grown + a, removed_copy, b + 1);
                            top_op->removed = grown;
                            top_op->to = top_op->to + b;
                            merged = true;
                        }
                    }
                }
                if (merged) {
                    fdk_free(removed_copy);
                    fdk_free(insert_copy);
                    return;
                }
            }
        }
    }
    entry_undo_op *op = fdk_alloc(sizeof(*op));
    if (op == NULL) {
        fdk_free(removed_copy);
        fdk_free(insert_copy);
        FDK_WARN("entry: undo op not recorded (out of memory)");
        return;
    }
    op->e = e;
    op->from = from;
    op->to = to;
    op->removed = removed_copy;
    op->inserted = insert_copy;
    op->caret_before = caret_before;
    op->anchor_before = anchor_before;
    op->typed = typed;
    fdk_undo_op uop = {
        .user_data = op,
        .undo = entry_undo_apply,
        .redo = entry_redo_apply,
        .destroy = entry_undo_destroy,
    };
    if (!fdk_ok(fdk_undo_stack_push(e->undo, &uop))) {
        entry_undo_destroy(op);
        FDK_WARN("entry: undo op not recorded (push refused)");
    }
}

/* Replace [from, to) with insert (insert_len bytes, may be NULL/0).
 * Maintains every invariant and fires on_changed. Returns FDK_ERR_*,
 * leaves the entry untouched on failure.
 *
 * 1.3.3: this is the single mutation primitive, so it is also the
 * undo recorder. Applications driven by an op's undo/redo closure
 * set undo_applying (see entry_apply_op) and skip the recording.
 * Copies for the record are taken BEFORE the text moves; if the
 * copy allocation fails the edit still succeeds (degraded history,
 * logged) — a keystroke must never fail because the history could
 * not be paid for. */
static fdk_result entry_splice(fdk_entry *e, size_t from, size_t to,
                               const char *insert, size_t insert_len) {
    if (from > to || to > e->len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (insert == NULL) {
        insert_len = 0;
    }
    size_t new_len = e->len - (to - from) + insert_len;
    size_t cap = (e->max_len > 0) ? e->max_len : ENTRY_MAX_TEXT;
    if (new_len > cap) {
        FDK_WARN("entry: %zu-byte limit reached; insert refused", cap);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = entry_ensure_cap(e, new_len + 1);
    if (!fdk_ok(r)) {
        return r;
    }
    /* ---- undo capture (pre-mutation state) ---- */
    char *removed_copy = NULL;
    char *insert_copy = NULL;
    size_t caret_before = e->caret;
    size_t anchor_before = e->anchor;
    if (!e->undo_applying) {
        if (to > from) {
            removed_copy = fdk_alloc(to - from + 1);
            if (removed_copy != NULL) {
                memcpy(removed_copy, e->text + from, to - from);
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
    /* A11y: the text (the entry's value interface) changed. */
    fdk__a11y_notify(&e->base, FDK_A11Y_VALUE_CHANGED, 0);
    if (e->on_changed != NULL) {
        e->on_changed(&e->base, e->on_changed_data);
    }
    /* ---- undo record (post-success; skipped while applying) ----
     *
     * All-or-nothing: if either needed copy failed allocation, the
     * whole record is dropped — recording a half-op (an insert
     * without its removed text) would make undo restore WRONG text.
     * The edit itself stands; the warning says the history lost a
     * step. */
    if (!e->undo_applying) {
        bool need_removed = (to > from);
        bool need_insert = (insert_len > 0);
        bool removed_ok = !need_removed || removed_copy != NULL;
        bool insert_ok = !need_insert || insert_copy != NULL;
        if ((need_removed || need_insert) && removed_ok && insert_ok) {
            entry_undo_record(e, from, to, removed_copy, insert_copy,
                              caret_before, anchor_before);
        } else {
            if (need_removed || need_insert) {
                FDK_WARN("entry: edit not recorded (history copy "
                         "failed)");
            }
            fdk_free(removed_copy);
            fdk_free(insert_copy);
        }
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

/* ---- PRIMARY selection (1.3.4) ---- */

/* The classic Unix "current selection" buffer, distinct from the
 * copy/paste CLIPBOARD: text becomes PRIMARY by being SELECTED, and
 * the middle button pastes it. Called from entry_set_selection (THE
 * selection mutation) whenever the endpoints actually changed.
 *
 * Politeness rules, in order:
 *   - disabled entries never touch it;
 *   - an entry that never pushed (empty selection all along, or a
 *     standalone headless tree) never GRABS ownership — an FDK app
 *     must not steal the user's xterm selection at startup;
 *   - a collapse AFTER a push empties the buffer (one set; the
 *     classic observable is that middle-click then pastes nothing);
 *   - edits and pastes do NOT re-sync (splice collapses the
 *     selection as a side effect, and PRIMARY surviving a paste is
 *     what makes repeated middle-click pastes work — the xterm
 *     property).
 *
 * Sets are best-effort by the platform contract (no round-trip):
 * drag-selects fire this at pointer rate, which is exactly the
 * classic model — every real selection change re-owns. */
static void entry_primary_sync(fdk_entry *e) {
    if ((e->base.flags & FDK_WF_ENABLED) == 0) {
        return; /* disabled entries do not touch the selection buffer */
    }
    bool selected = e->caret != e->anchor;
    if (!selected && !e->primary_pushed) {
        return; /* empty and never owned: nothing to do, nothing grabbed */
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return; /* standalone tree: no platform connection to talk to */
    }
    if (!selected) {
        (void)fdk_clipboard_set_primary_text(ctx, "");
        e->primary_pushed = false;
        return;
    }
    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    char saved = e->text[hi];
    e->text[hi] = '\0';
    fdk_result r = fdk_clipboard_set_primary_text(ctx, e->text + lo);
    e->text[hi] = saved;
    e->primary_pushed = fdk_ok(r);
}

/* Middle-click: paste PRIMARY at the click position (classic Unix).
 * Pure INSERT at the point — the existing selection is not replaced
 * (xterm semantics; GTK's replace-on-overlap is a different school).
 * Read-first ordering matters: if THIS entry owns PRIMARY, collapsing
 * its selection in entry_set_selection would empty the buffer before
 * the read — classic self-paste (select, middle-click elsewhere in
 * the same field) depends on reading before that. */
static void entry_primary_paste(fdk_entry *e, fdk_f32 x) {
    if (e->read_only) {
        return; /* the reader contract: mutators refuse when read-only */
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&e->base));
    if (ctx == NULL) {
        return;
    }
    char *primary = fdk_clipboard_get_primary_text(ctx);
    size_t hit = offset_at_x(e, x);
    if (primary == NULL) {
        entry_set_selection(e, hit, hit); /* plain caret move */
        return;
    }
    (void)entry_splice(e, hit, hit, primary, strlen(primary));
    fdk_free(primary);
}

/* ---- selection helpers ---- */

static void entry_delete_selection(fdk_entry *e) {
    if (e->caret == e->anchor) {
        return;
    }
    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    /* 1.3.3 gesture grouping: typing over a selection runs this
     * delete and then an insert in the SAME keystroke — the user
     * performed ONE "replace" and undo must step it as one. The
     * flag is set AFTER the delete's own record (the delete record
     * must not consume it), and read-and-cleared by the NEXT record:
     * only an insert landing exactly at the deleted slot composes;
     * anything else — a keystroke boundary, a caret move, another
     * delete — expires it. */
    fdk_result r = entry_splice(e, lo, hi, NULL, 0);
    if (fdk_ok(r)) {
        e->undo_group_pending = true;
    }
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
 * the entry paint tests caught in the first cut.
 *
 * 1.3.4: this is also THE PRIMARY-selection sync point — the classic
 * Unix model is "the selection owns PRIMARY", so every real change
 * re-owns it (best-effort, no round-trip: drag-selects fire this at
 * pointer rate). See entry_primary_sync for the politeness rules. */
static void entry_set_selection(fdk_entry *e, size_t anchor, size_t caret) {
    size_t old_caret = e->caret;
    size_t old_anchor = e->anchor;
    e->caret = snap_boundary(e->text, e->len, caret);
    e->anchor = snap_boundary(e->text, e->len, anchor);
    entry_scroll_to_caret(e);
    if (e->caret != old_caret || e->anchor != old_anchor) {
        fdk_widget_invalidate(&e->base);
        entry_primary_sync(e);
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
        /* 1.4.2 — the search preset's clear button: a press in the
         * reserved right zone empties the field through the honest
         * edit path (undo records, on_changed fires). It is NOT a
         * caret gesture: the click-count machinery never sees it. */
        if (e->search_mode && e->len > 0 && !e->read_only &&
            ev->pointer.button == FDK_POINTER_BUTTON_LEFT &&
            ev->pointer.position.x >
                (fdk_f32)(e->base.bounds.width -
                          entry_clear_zone_w(e))) {
            if (!fdk_widget_has_focus(w)) {
                (void)fdk_widget_focus(w);
            }
            e->clear_hover = false;
            (void)entry_splice(e, 0, e->len, NULL, 0);
            return true;
        }
        if (ev->pointer.button == FDK_POINTER_BUTTON_MIDDLE) {
            /* Classic Unix middle-click: PRIMARY pastes at the click.
             * Deliberately outside the click-count machinery — a
             * middle press is not a selection gesture. */
            if (!fdk_widget_has_focus(w)) {
                (void)fdk_widget_focus(w);
            }
            entry_primary_paste(e, ev->pointer.position.x);
            return true;
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
            /* Plain click: caret to hit. Shift extends the current
             * selection from its anchor instead of collapsing (the
             * Phase 9 pointer modifiers make this expressible). */
            bool shift = (ev->pointer.modifiers & FDK_MOD_SHIFT) != 0;
            if (shift) {
                entry_set_selection(e, e->anchor, hit);
            } else {
                entry_set_selection(e, hit, hit);
            }
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
        /* 1.4.2 — the clear button's hover pill (MOTION carries real
         * coordinates; synthesized ENTER/LEAVE do not — the paned's
         * 1.4.1 lesson). */
        if (e->search_mode) {
            bool in_clear =
                e->len > 0 &&
                ev->position.x > (fdk_f32)(e->base.bounds.width -
                                           entry_clear_zone_w(e));
            if (in_clear != e->clear_hover) {
                e->clear_hover = in_clear;
                fdk_widget_invalidate(w);
            }
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

        /* Clipboard shortcuts. Read-only entries keep COPY and
         * SELECT-ALL (the reader contract) and drop the mutators. */
        if (ctrl && !shift) {
            if (key->codepoint == 'c' || key->codepoint == 'C') {
                entry_clipboard_copy(e);
                return true;
            }
            if (!e->read_only &&
                (key->codepoint == 'x' || key->codepoint == 'X')) {
                entry_clipboard_cut(e);
                return true;
            }
            if (!e->read_only &&
                (key->codepoint == 'v' || key->codepoint == 'V')) {
                entry_clipboard_paste(e);
                return true;
            }
            if (key->codepoint == 'a' || key->codepoint == 'A') {
                entry_set_selection(e, 0, e->len);
                return true;
            }
        }

        /* Undo/redo (1.3.3): Ctrl+Z undo, Ctrl+Shift+Z or Ctrl+Y
         * redo — the two spellings every desktop ships. Read-only
         * entries refuse (the mutator contract above) and BUBBLE the
         * key: a dialog's Escape handling must not be shadowed. */
        if (ctrl && !e->read_only &&
            (key->codepoint == 'z' || key->codepoint == 'Z')) {
            if (shift) {
                (void)fdk_entry_redo(w);
            } else {
                (void)fdk_entry_undo(w);
            }
            return true;
        }
        if (ctrl && !shift && !e->read_only &&
            (key->codepoint == 'y' || key->codepoint == 'Y')) {
            (void)fdk_entry_redo(w);
            return true;
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
            if (e->read_only) {
                return true; /* consumed, not edited */
            }
            if (entry_has_selection(e)) {
                entry_delete_selection(e);
            } else if (e->caret > 0) {
                size_t prev = utf8_prev(e->text, e->caret);
                (void)entry_splice(e, prev, e->caret, NULL, 0);
            }
            return true;
        case FDK_KEY_DELETE:
            if (e->read_only) {
                return true; /* consumed, not edited */
            }
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
            /* The search preset's Esc ladder (1.4.2): with text on
             * board, the FIRST Escape clears it (GtkSearchEntry
             * semantics — consumed); an empty search field falls
             * through to the selection-collapse rule below, and a
             * quiescent one bubbles so a dialog's Cancel still
             * works. */
            if (e->search_mode && e->len > 0 && !e->read_only) {
                (void)entry_splice(e, 0, e->len, NULL, 0);
                return true;
            }
            /* Collapse the selection (classic cancel behavior). With
             * nothing selected there is nothing to collapse: bubble
             * the Escape instead of eating it — a prompt dialog's
             * Cancel rides the window layer's Escape handling, and a
             * lone Entry swallowing it would wedge that (1.2.1). */
            if (e->anchor != e->caret) {
                entry_set_selection(e, e->caret, e->caret);
                return true;
            }
            break;
        default:
            break;
        }

        /* Textual insert: any codepoint the platform resolved,
         * excluding control characters (they are not text). */
        if (key->codepoint >= 0x20u && !ctrl && !e->read_only) {
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
    case FDK_WIDGET_FOCUS_IN:
        entry_blink_start(entry_of(w));
        return false; /* focus events keep bubbling (containers may
                         * track the chain for their own borders) */
    case FDK_WIDGET_FOCUS_OUT:
        entry_blink_stop(entry_of(w));
        return false;
    default:
        break;
    }
    return false;
}

static void entry_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_entry *e = entry_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    /* The modern field (1.4.0): a FLAT, slightly sunken surface —
     * ENTRY_BACKGROUND fill with a 1px ENTRY_BORDER outline — instead
     * of v1's raised control fill. Focus reads as the themed ring
     * (FOCUS_RING token, FOCUS_RING_WIDTH metric) drawn over the
     * border, so a focused field is outlined in accent, not filled
     * differently; disabled fields keep the control-disabled fill. */
    const bool focused = (w->flags & FDK_WF_FOCUSED) != 0;
    fdk_color fill = ((w->flags & FDK_WF_ENABLED) == 0)
        ? fdk__pal_control_disabled()
        : fdk__pal_entry();
    fdk_i32 radius = fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);

    if ((w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_draw_rounded_rect(
            surface, bounds, radius,
            focused ? fdk__pal_focus_ring() : fdk__pal_entry_border());
        if (focused) {
            fdk_i32 fw =
                fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
            /* Wider rings stack outlines inward (1px = 2px total).
             * The 1px border underneath is simply overdrawn. */
            for (fdk_i32 i = 1; i < fw; i++) {
                fdk_rect inset = {bounds.x + i, bounds.y + i,
                                  bounds.width - i * 2,
                                  bounds.height - i * 2};
                if (inset.width > 0 && inset.height > 0) {
                    fdk_i32 rr = radius > i ? radius - i : 0;
                    fdk_surface_draw_rounded_rect(
                        surface, inset, rr, fdk__pal_focus_ring());
                }
            }
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
    fdk_i32 text_x = bounds.x + entry_text_inset(e) - e->x_offset;

    /* 1.4.2 — the magnifier glyph in the reserved left zone: a
     * stroked lens + handle in the neutral ink (the symbolic-glyph
     * discipline: no fills that fight theme changes). */
    if (e->search_mode) {
        fdk_i32 gx = bounds.x + ENTRY_PAD_X - 2;
        fdk_i32 gy = bounds.y + (bounds.height - ENTRY_SEARCH_GLYPH) / 2;
        if (gy < bounds.y) {
            gy = bounds.y;
        }
        fdk_color ink = ((w->flags & FDK_WF_ENABLED) == 0)
            ? fdk__pal_text_disabled()
            : fdk__pal_text();
        fdk_surface_draw_circle_aa(surface, gx + 6, gy + 7, 4, ink);
        fdk_surface_draw_line_aa(surface, gx + 9, gy + 10, gx + 13,
                                 gy + 14, ink);
    }

    size_t lo = (e->anchor < e->caret) ? e->anchor : e->caret;
    size_t hi = (e->anchor < e->caret) ? e->caret : e->anchor;
    bool has_sel = (lo != hi) && (w->flags & FDK_WF_ENABLED) != 0;

    /* Selection highlight behind the selected segment. */
    if (has_sel) {
        fdk_i32 sel_x = text_x + entry_width_to(e, lo);
        fdk_i32 sel_w = entry_width_to(e, hi) - entry_width_to(e, lo);
        fdk_rect sel = {sel_x, bounds.y + 2, sel_w,
                        bounds.height - 4};
        if (sel_w > 0 && sel.height > 0) {
            /* The 1.3.2 SELECTION_BACKGROUND token, wired in 1.4.0
             * (the hardcoded accent-alpha is gone). */
            fdk_surface_fill_rect(surface, sel, fdk__pal_selection());
        }
    }

    /* Password mode: one bullet per CLUSTER, the same three-segment
     * geometry (the widths above are already bullet-aware — the
     * buffer, caret, selection, and hit-testing never change). */
    if (e->password && e->len > 0) {
        fdk_i32 bw = bullet_advance(e);
        fdk_i32 w_lo = entry_width_to(e, lo);
        fdk_i32 w_hi = entry_width_to(e, hi);
        size_t n_lo = cluster_count(e, lo);
        size_t n_hi = cluster_count(e, hi);
        size_t n_all = cluster_count(e, e->len);
        for (size_t i = 0; i < n_lo; i++) {
            fdk__draw_text(surface, e->font, ENTRY_BULLET_UTF8, text_col,
                           text_x + (fdk_i32)i * bw, baseline);
        }
        for (size_t i = 0; i < n_hi - n_lo; i++) {
            fdk__draw_text(surface, e->font, ENTRY_BULLET_UTF8, text_col,
                           text_x + w_lo + (fdk_i32)i * bw, baseline);
        }
        for (size_t i = 0; i < n_all - n_hi; i++) {
            fdk__draw_text(surface, e->font, ENTRY_BULLET_UTF8, text_col,
                           text_x + w_hi + (fdk_i32)i * bw, baseline);
        }
    } else if (e->len > 0) {
    /* Text in three segments — [0,lo) [lo,hi) [hi,len) — drawn by
     * temporarily NUL-terminating at each boundary. The selected
     * middle keeps the plain text color: the accent highlight rect
     * already says "selected" (no inverted text in the v1 theme). */
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

    /* Placeholder (1.4.0): shown only when there is nothing else to
     * draw — empty buffer, no preedit — in the disabled-text color.
     * It never joins selection/caret geometry: hit-testing, the caret
     * position, and copy all see a genuinely empty field. Overlong
     * placeholders clip at the field edge (no wrap, no ellipsis —
     * the placeholder must not change the entry's natural size). */
    if (e->len == 0 && e->preedit_len == 0 && e->placeholder != NULL) {
        fdk__draw_text(surface, e->font, e->placeholder,
                       fdk__pal_text_disabled(), text_x, baseline);
    }

    /* Preedit (IME groundwork): rendered AT the caret, underlined,
     * with the visual caret parked at its end while active. */
    fdk_i32 caret_x = text_x + entry_width_to(e, e->caret);
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

    /* Caret: 1px vertical bar, drawn only when enabled and focused
     * (and not in the blink's dark phase — unfocused/detached entries
     * have no timer and caret_on stays true, preserving the v1 look
     * everywhere the blink machinery cannot run). */
    if ((w->flags & FDK_WF_ENABLED) != 0 &&
        (w->flags & FDK_WF_FOCUSED) != 0 && e->caret_on) {
        fdk_rect bar = {caret_x, bounds.y + 3, 1, bounds.height - 6};
        if (bar.height > 0) {
            fdk_surface_fill_rect(surface, bar, text_col);
        }
    }

    /* 1.4.2 — the clear button in the reserved right zone (exists
     * only while there is text): a soft hover pill + the X stroke.
     * The pill is the ONLY hover feedback (no fade — the button
     * appearing with the first character is feedback enough). */
    if (entry_clear_zone_w(e) > 0) {
        fdk_i32 zone = entry_clear_zone_w(e);
        fdk_i32 bx = bounds.x + bounds.width - zone - 2;
        fdk_i32 by = bounds.y + (bounds.height - ENTRY_SEARCH_GLYPH) / 2;
        if (by < bounds.y) {
            by = bounds.y;
        }
        if (e->clear_hover && (w->flags & FDK_WF_ENABLED) != 0) {
            fdk_rect pill = {bx - 2, by - 1, ENTRY_SEARCH_GLYPH + 4,
                             ENTRY_SEARCH_GLYPH + 2};
            fdk_surface_fill_rounded_rect(surface, pill, 8,
                                          fdk__pal_row_hover());
        }
        fdk_color ink = ((w->flags & FDK_WF_ENABLED) == 0)
            ? fdk__pal_text_disabled()
            : fdk__pal_text();
        fdk_surface_draw_line_aa(surface, bx + 4, by + 4, bx + 12,
                                 by + 12, ink);
        fdk_surface_draw_line_aa(surface, bx + 12, by + 4, bx + 4,
                                 by + 12, ink);
    }
}

/* ---- measure / destroy ---- */

static void entry_measure(fdk_widget *w, fdk_size *out) {
    fdk_entry *e = entry_of(w);
    fdk_i32 tw = 0, th = 0;
    if (e->password) {
        /* Bullet-run width: the font's line height, bullet advances. */
        fdk__text_extent(e->font, ENTRY_BULLET_UTF8, &tw, &th);
        tw = (fdk_i32)cluster_count(e, e->len) * (tw > 0 ? tw : 6);
    } else {
        fdk__text_extent(e->font, e->text, &tw, &th);
    }
    out->width = tw + entry_text_inset(e) + entry_right_inset(e);
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
    entry_blink_stop(e);
    /* The undo stack owns the op structs (with their text copies);
     * destroying it runs every op's destroy — nothing dangles. */
    fdk_undo_stack_destroy(e->undo);
    e->undo = NULL;
    fdk_free(e->text);
    fdk_free(e->preedit);
    fdk_free(e->placeholder);
}

/* ---- a11y ---- */

static void entry_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_entry *e = (const fdk_entry *)(const void *)w;
    if (e->read_only) {
        out->states |= FDK_A11Y_READ_ONLY;
    } else {
        out->states |= FDK_A11Y_EDITABLE;
    }
    /* The text is the value interface (value_text); password fields
     * expose it too — masking is a BRIDGE decision (screen readers
     * must be able to echo what the user types), not the toolkit's. */
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = (double)e->len;
    out->value_current = (double)e->len;
    out->value_text = fdk__strdup(e->text);
}

/* ---- a11y text interface ---- */

static size_t entry_text_length(const fdk_widget *w) {
    const fdk_entry *e = (const fdk_entry *)(const void *)w;
    return e->len;
}

static size_t entry_text_caret(const fdk_widget *w) {
    const fdk_entry *e = (const fdk_entry *)(const void *)w;
    return e->caret;
}

static bool entry_text_selection(const fdk_widget *w, size_t *anchor,
                                 size_t *caret) {
    const fdk_entry *e = (const fdk_entry *)(const void *)w;
    if (e->anchor == e->caret) {
        return false;
    }
    if (anchor != NULL) {
        *anchor = e->anchor;
    }
    if (caret != NULL) {
        *caret = e->caret;
    }
    return true;
}

static bool entry_text_at(const fdk_widget *w, size_t offset,
                          fdk_a11y_text_granularity granularity,
                          char *buf, size_t cap, size_t *out_start,
                          size_t *out_end) {
    const fdk_entry *e = (const fdk_entry *)(const void *)w;
    if (buf == NULL || cap == 0) {
        return false;
    }
    buf[0] = '\0';
    if (e->len == 0) {
        if (out_start != NULL) {
            *out_start = 0;
        }
        if (out_end != NULL) {
            *out_end = 0;
        }
        return true; /* empty text: an empty run at 0 */
    }

    size_t start = 0;
    size_t end = e->len;
    if (offset > e->len) {
        offset = e->len;
    }
    offset = snap_boundary(e->text, e->len, offset);

    switch (granularity) {
    case FDK_A11Y_TEXT_CHAR: {
        /* The single codepoint at (or just before) the offset. */
        if (offset == e->len) {
            offset = utf8_prev(e->text, e->len);
        }
        fdk_u32 cp = 0;
        int n = fdk_text_utf8_next(e->text, e->len, offset, &cp);
        (void)cp;
        start = offset;
        end = (n > 0) ? offset + (size_t)n : offset + 1;
        break;
    }
    case FDK_A11Y_TEXT_WORD: {
        /* Whitespace-delimited words, the same definition the
         * double-click selection uses (word_range). */
        if (offset == e->len) {
            offset = utf8_prev(e->text, e->len);
        }
        word_range(e->text, e->len, offset, &start, &end);
        break;
    }
    case FDK_A11Y_TEXT_LINE:
    default:
        /* Entry is single-line: the run is the whole text. */
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
        n = cap - 1; /* truncated display; the range is still full */
    }
    memcpy(buf, e->text + start, n);
    buf[n] = '\0';
    return true;
}

static bool entry_text_set_caret(fdk_widget *w, size_t offset) {
    return fdk_ok(fdk_entry_set_cursor(w, offset));
}

static bool entry_text_set_selection(fdk_widget *w, size_t anchor,
                                     size_t caret) {
    return fdk_ok(fdk_entry_select_range(w, anchor, caret));
}

static const fdk_a11y_class entry_a11y = {
    .role = FDK_A11Y_ROLE_ENTRY,
    .describe = entry_a11y_describe,
    .actions = NULL,
    .perform = NULL,
    .text_length = entry_text_length,
    .text_caret = entry_text_caret,
    .text_selection = entry_text_selection,
    .text_at = entry_text_at,
    .text_set_caret = entry_text_set_caret,
    .text_set_selection = entry_text_set_selection,
};

static const fdk_widget_class fdk_entry_class_def = {
    .size = sizeof(fdk_entry),
    .name = "entry",
    .handle_event = entry_handle_event,
    .paint = entry_paint,
    .measure = entry_measure,
    .arrange = NULL,
    .destroy = entry_destroy,
    .a11y = &entry_a11y,
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
    e->blink_timer = NULL;
    e->caret_on = true;
    e->undo = NULL;       /* lazy: first recorded edit allocates it */
    e->undo_applying = false;
    e->search_mode = false; /* the plain entry (1.4.2 preset below) */
    e->clear_hover = false;
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

/* ---- search preset (1.4.2) ---- */

fdk_result fdk_search_entry_create(fdk_widget *parent, fdk_font *font,
                                   fdk_widget **out_entry) {
    fdk_result r = fdk_entry_create(parent, font, "", out_entry);
    if (!fdk_ok(r)) {
        return r;
    }
    entry_of(*out_entry)->search_mode = true;
    fdk_widget_invalidate(*out_entry);
    fdk_widget_child_layout_changed((*out_entry)->parent);
    return FDK_OK;
}

bool fdk_entry_is_search(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return false;
    }
    return entry_of(entry)->search_mode;
}

fdk_result fdk_entry_set_text(fdk_widget *entry, const char *text) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    const char *set = (text != NULL) ? text : "";
    size_t len = strlen(set);
    size_t cap = (e->max_len > 0) ? e->max_len : ENTRY_MAX_TEXT;
    if (len > cap) {
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
    /* 1.3.3: a programmatic overwrite is a mode change, not an edit —
     * the old text's history cannot meaningfully undo INTO it (the
     * ops' byte offsets reference a buffer that no longer exists),
     * so the history is cleared. set_text stays the "reset the
     * field" API; typed/spliced edits stay the undoable ones. */
    if (e->undo != NULL) {
        (void)fdk_undo_stack_clear(e->undo);
    }
    entry_scroll_to_caret(e);
    fdk_widget_invalidate(entry);
    fdk_widget_child_layout_changed(entry->parent);
    /* A11y: the text (the entry's value interface) changed. */
    fdk__a11y_notify(entry, FDK_A11Y_VALUE_CHANGED, 0);
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

void fdk_entry_set_placeholder(fdk_widget *entry, const char *text) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    if (text == NULL || text[0] == '\0') {
        /* NULL and "" both clear (a placeholder is decoration; an
         * empty one is nothing to paint). */
        if (e->placeholder == NULL) {
            return;
        }
        fdk_free(e->placeholder);
        e->placeholder = NULL;
    } else {
        size_t len = strlen(text);
        if (len > ENTRY_MAX_TEXT) {
            return; /* same cap as text; silently ignored, setter is void */
        }
        char *copy = fdk_alloc(len + 1);
        if (copy == NULL) {
            return; /* keep the old placeholder on OOM (fail-soft) */
        }
        memcpy(copy, text, len + 1);
        fdk_free(e->placeholder);
        e->placeholder = copy;
    }
    /* Only an EMPTY entry's paint depends on it — invalidate
     * unconditionally anyway: cheap, and correct across set_text
     * races the caller may have pending. */
    fdk_widget_invalidate(entry);
}

const char *fdk_entry_get_placeholder(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return NULL;
    }
    return entry_of(entry)->placeholder;
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

/* ---- password / read-only / max-length ---- */

void fdk_entry_set_password(fdk_widget *entry, bool password) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    if (e->password == password) {
        return;
    }
    e->password = password;
    /* Geometry may change (bullets advance differently than the
     * glyphs): re-measure and re-anchor the scroll. */
    entry_scroll_to_caret(e);
    fdk_widget_invalidate(entry);
    fdk_widget_child_layout_changed(entry->parent);
}

bool fdk_entry_is_password(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return false;
    }
    return entry_of(entry)->password;
}

void fdk_entry_set_read_only(fdk_widget *entry, bool read_only) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    fdk_entry *e = entry_of(entry);
    if (e->read_only == read_only) {
        return;
    }
    e->read_only = read_only;
    /* A11y: the editable/read-only state flipped. */
    fdk__a11y_notify(entry, FDK_A11Y_STATE_CHANGED, FDK_A11Y_READ_ONLY);
    fdk_widget_invalidate(entry);
}

bool fdk_entry_is_read_only(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return false;
    }
    return entry_of(entry)->read_only;
}

void fdk_entry_set_max_length(fdk_widget *entry, size_t max_bytes) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return;
    }
    if (max_bytes > ENTRY_MAX_TEXT) {
        max_bytes = ENTRY_MAX_TEXT;
    }
    entry_of(entry)->max_len = max_bytes;
}

size_t fdk_entry_get_max_length(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return 0;
    }
    fdk_entry *e = entry_of(entry);
    return (e->max_len > 0) ? e->max_len : ENTRY_MAX_TEXT;
}

/* ---- undo/redo (1.3.3 public API) ---- */

bool fdk_entry_can_undo(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return false;
    }
    fdk_entry *e = entry_of(entry);
    return !e->read_only && e->undo != NULL &&
           fdk_undo_stack_can_undo(e->undo);
}

bool fdk_entry_can_redo(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return false;
    }
    fdk_entry *e = entry_of(entry);
    return !e->read_only && e->undo != NULL &&
           fdk_undo_stack_can_redo(e->undo);
}

fdk_result fdk_entry_undo(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    /* Programmatic calls report honestly: INVALID_STATE when there
     * is nothing to undo (the KEYBOARD binding is the blind no-op
     * consumer — it never looks). */
    if (e->read_only || e->undo == NULL ||
        !fdk_undo_stack_can_undo(e->undo)) {
        return FDK_ERR_INVALID_STATE;
    }
    return fdk_undo_stack_undo(e->undo);
}

fdk_result fdk_entry_redo(fdk_widget *entry) {
    if (entry == NULL || entry->klass != &fdk_entry_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_entry *e = entry_of(entry);
    if (e->read_only || e->undo == NULL ||
        !fdk_undo_stack_can_redo(e->undo)) {
        return FDK_ERR_INVALID_STATE;
    }
    return fdk_undo_stack_redo(e->undo);
}
