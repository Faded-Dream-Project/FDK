/*
 * test_ime.c — the IME model's headless battery (1.1.0, M-1)
 *
 * Three seams, one file:
 *
 *   1. THE X11 COMPOSITION-BUFFER MATH (fdk_x11_ime_test_apply ->
 *      fdk__x11_preedit_apply): the character-index/byte-offset
 *      splice every XIM client historically corrupts — inserts,
 *      span replaces, deletions, end clips, refusals, and the
 *      multi-byte shapes (3-byte CJK, 4-byte emoji) where char
 *      counting and byte counting diverge.
 *
 *   2. THE WIDGET ROUTING: FDK_EVENT_IME_PREEDIT / IME_COMMIT fed
 *      through fdk_widget_tree_handle_event — the FOCUSED Entry or
 *      TextView consumes both (preedit rides set_preedit, commit
 *      splices at the caret through the single mutation primitive:
 *      undo, on_changed, a11y all ride it), while read-only /
 *      disabled / no-focus / non-text-focus shapes fall through
 *      UNCONSUMED — the application's window callback implements
 *      its own composition there (the documented contract).
 *
 *   3. THE SURROUNDING-TEXT CONTEXT QUERIES:
 *      fdk__widget_ime_context answers the focused text widget's
 *      (text, caret-bytes) for the window layer's
 *      ime_update_surrounding push; anything else answers 0.
 *
 * What this file deliberately does NOT pin: the Wayland end-to-end
 * (a real input-method-v2 client driving text-input-v3 through
 * sway) lives in the sway rig (tests/test_wayland_integration.c);
 * the X11 XIM-callback wiring (XIMPreeditCallbacks + the
 * Xutf8LookupString commit leg) is code-reviewed here and
 * owner-verified against a real fcitx5 desktop
 * (docs/TESTING-ON-DESKTOP.md) — this sandbox has no XIM server,
 * and the buffer math is the part that needs pinning headless.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

/* Internal seams (the suite's standing discipline — same include
 * style as test_touch.c/test_entry.c): the platform header exposes
 * the buffer-math seam, the widget internals expose the context
 * queries. */
#include "platform/x11/x11_platform.h"
#include "widget/widgets_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- 1. the X11 composition-buffer math -------------------------------- */

static void test_apply_insert_into_empty(void) {
    char buf[32] = "";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 0, "abc");
    assert(r == 3);
    assert(strcmp(buf, "abc") == 0);
}

static void test_apply_insert_middle(void) {
    char buf[32] = "hello";
    /* Insert at CHARACTER index 2, replacing nothing. */
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 2, 0, "-X-");
    assert(r == 8);
    assert(strcmp(buf, "he-X-llo") == 0);
}

static void test_apply_replace_span(void) {
    char buf[32] = "hello world";
    /* Replace the first five CHARACTERS ("hello") with "goodbye". */
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 5, "goodbye");
    assert(r == 13);
    assert(strcmp(buf, "goodbye world") == 0);
}

static void test_apply_delete_span(void) {
    char buf[32] = "hello";
    /* Delete three characters starting at index 1 ("ell"). */
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 1, 3, NULL);
    assert(r == 2);
    assert(strcmp(buf, "ho") == 0);
}

static void test_apply_cjk_char_indices(void) {
    /* The corruption classic: chg_first/chg_length count CHARACTERS
     * while the buffer is bytes. 你好世界 — four 3-byte characters.
     * Replace the two characters at indices 1..2 (好世) with 的. */
    char buf[32] = "\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 1, 2,
                                      "\xe7\x9a\x84");
    assert(r == 9); /* 3 + 3 + 3 bytes */
    assert(strcmp(buf,
           "\xe4\xbd\xa0\xe7\x9a\x84\xe7\x95\x8c") == 0); /* 你的界 */
}

static void test_apply_emoji_four_byte(void) {
    /* a😀b — a 1-byte, a 4-byte, a 1-byte character. Delete the
     * emoji (character index 1). */
    char buf[32] = "a\xf0\x9f\x98\x80" "b";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 1, 1, NULL);
    assert(r == 2);
    assert(strcmp(buf, "ab") == 0);
}

static void test_apply_insert_after_emoji(void) {
    /* Insert a 2-byte character after the emoji (character index 2),
     * before the 'b'. */
    char buf[32] = "a\xf0\x9f\x98\x80" "b";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 2, 0, "\xc3\xa9");
    assert(r == 8);
    assert(strcmp(buf, "a\xf0\x9f\x98\x80\xc3\xa9" "b") == 0);
}

static void test_apply_caret_move_is_noop(void) {
    /* text NULL + chg_length 0: a caret-only move — the buffer must
     * not change and the byte length returns unchanged. */
    char buf[32] = "hello";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 3, 0, NULL);
    assert(r == 5);
    assert(strcmp(buf, "hello") == 0);
}

static void test_apply_delete_past_end_clips(void) {
    /* chg_length may exceed the remainder ("the rest"): clipped. */
    char buf[32] = "abc";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 1, 99, NULL);
    assert(r == 1);
    assert(strcmp(buf, "a") == 0);
}

static void test_apply_append_at_end(void) {
    /* Insert at the character COUNT == the end: an append. */
    char buf[32] = "abc";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 3, 0, "def");
    assert(r == 6);
    assert(strcmp(buf, "abcdef") == 0);
}

static void test_apply_first_beyond_refused(void) {
    /* chg_first beyond the string is a protocol violation: refuse
     * (return -1) and leave the buffer untouched. */
    char buf[32] = "abc";
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 9, 0, "x");
    assert(r == (size_t)-1);
    assert(strcmp(buf, "abc") == 0);
}

static void test_apply_negative_indices_refused(void) {
    char buf[32] = "abc";
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), -1, 0, "x") ==
           (size_t)-1);
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), 0, -1, "x") ==
           (size_t)-1);
    assert(fdk_x11_ime_test_apply(NULL, sizeof(buf), 0, 0, "x") ==
           (size_t)-1);
    assert(strcmp(buf, "abc") == 0);
}

static void test_apply_overflow_refused(void) {
    /* Capacity is the caller's contract: a splice that would not
     * fit (with the NUL) is refused, buffer untouched. */
    char buf[8] = "abc"; /* 4 bytes used incl. NUL, cap 8 */
    size_t r = fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 0, "defghi");
    assert(r == (size_t)-1); /* 6 + 3 + 1 = 10 > 8 */
    assert(strcmp(buf, "abc") == 0);
    /* The boundary case: exactly fits (3 + 4 + 1 = 8). */
    r = fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 0, "defg");
    assert(r == 7);
    assert(strcmp(buf, "defgabc") == 0);
}

static void test_apply_cjk_full_composition_cycle(void) {
    /* The pinyin-style shape the module exists for: a composition
     * built draw-by-draw — n → ni → nih → 你好 — as pure
     * replaces of the whole (growing) buffer, then a commit. */
    char buf[64] = "";
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 0, "n") == 1);
    assert(strcmp(buf, "n") == 0);
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 1, "ni") == 2);
    assert(strcmp(buf, "ni") == 0);
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 2, "nihao") == 5);
    assert(strcmp(buf, "nihao") == 0);
    /* The IM replaces the whole composition with the candidates. */
    assert(fdk_x11_ime_test_apply(buf, sizeof(buf), 0, 5,
                                  "\xe4\xbd\xa0\xe5\xa5\xbd") == 6);
    assert(strcmp(buf, "\xe4\xbd\xa0\xe5\xa5\xbd") == 0); /* 你好 */
}

/* ---- 2. the widget routing ---------------------------------------------- */

static fdk_event_data ime_evt(fdk_event_type type, const char *text,
                              int cursor) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.ime.text = text;
    e.ime.cursor = cursor;
    return e;
}

static fdk_event_data key_cp(fdk_u32 codepoint, fdk_u32 mods) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = 0;
    e.key.codepoint = codepoint;
    e.key.modifiers = mods;
    return e;
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static void test_entry_preedit_rides_set_preedit(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    assert(fdk_widget_focus(entry));

    /* PREEDIT: consumed, the buffer is UNTOUCHED (the composition is
     * display-only until commit — the groundwork API's contract). */
    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "nihao", 2);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    /* A second PREEDIT revises it; an EMPTY preedit clears it — both
     * consumed, buffer still untouched. */
    e = ime_evt(FDK_EVENT_IME_PREEDIT, "ni hao", 3);
    assert(fdk_widget_tree_handle_event(root, &e));
    e = ime_evt(FDK_EVENT_IME_PREEDIT, "", 0);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    fdk_widget_destroy(root);
}

static void test_entry_commit_splices_at_caret(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    assert(fdk_widget_focus(entry));

    /* Park the caret between 'a' and 'b' (collapsed range = caret). */
    assert(fdk_ok(fdk_entry_select_range(entry, 1, 1)));

    /* A pending composition, then the commit: the composition
     * clears and the text splices at the caret. */
    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "X", 1);
    assert(fdk_widget_tree_handle_event(root, &e));
    e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "aXYbc") == 0);

    fdk_widget_destroy(root);
}

static void test_entry_commit_replaces_selection(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    assert(fdk_widget_focus(entry));

    /* Select "ab" (0..2); the commit REPLACES the selection. */
    assert(fdk_ok(fdk_entry_select_range(entry, 0, 2)));
    fdk_event_data e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "XYc") == 0);

    fdk_widget_destroy(root);
}

static void test_entry_commit_is_undoable(void) {
    /* The commit rides the single mutation primitive — undo must
     * revert it as one step (the 1.3.3 discipline). */
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    assert(fdk_widget_focus(entry));

    fdk_event_data e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abcXY") == 0);

    fdk_event_data z = key_cp('z', FDK_MOD_CTRL);
    assert(fdk_widget_tree_handle_event(root, &z));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    fdk_widget_destroy(root);
}

static void test_entry_read_only_refuses_commit(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    fdk_entry_set_read_only(entry, true);
    assert(fdk_widget_focus(entry));

    /* Read-only: the commit is NOT consumed — the application's
     * window callback gets it (custom surfaces decide what a commit
     * against a read-only field means). The preedit is still shown
     * (display-only) — consumed. */
    fdk_event_data e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(!fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    fdk_widget_destroy(root);
}

static void test_entry_disabled_refuses_both(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));
    assert(fdk_widget_focus(entry));

    /* Disabling DROPS the tree focus (the toolkit's own semantics —
     * a disabled widget never holds focus), so composition traffic
     * has nowhere to land: it falls through to the application's
     * window callback unconsumed. */
    fdk_widget_set_enabled(entry, false);
    assert(fdk_widget_tree_get_focused(root) != entry);

    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "XY", 1);
    assert(!fdk_widget_tree_handle_event(root, &e));
    e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(!fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    fdk_widget_destroy(root);
}

static void test_no_focus_falls_through(void) {
    /* Nothing focused: neither event is consumed — they belong to
     * the application's window callback. */
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "abc", &entry)));

    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "XY", 1);
    assert(!fdk_widget_tree_handle_event(root, &e));
    e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(!fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_entry_get_text(entry), "abc") == 0);

    fdk_widget_destroy(root);
}

static void test_non_text_focus_falls_through(void) {
    /* A focused BUTTON: keys route to it, IME traffic does not —
     * composition belongs to text surfaces only. */
    fdk_widget *root = fresh_root();
    fdk_widget *button = NULL;
    assert(fdk_ok(fdk_button_create(root, NULL, "go", &button)));
    assert(fdk_widget_focus(button));

    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "XY", 1);
    assert(!fdk_widget_tree_handle_event(root, &e));
    e = ime_evt(FDK_EVENT_IME_COMMIT, "XY", 0);
    assert(!fdk_widget_tree_handle_event(root, &e));

    fdk_widget_destroy(root);
}

static void test_textview_round(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_textview_set_text(tv, "line one");
    assert(fdk_widget_focus(tv));

    /* Preedit: consumed, buffer untouched. */
    fdk_event_data e = ime_evt(FDK_EVENT_IME_PREEDIT, "draft", 2);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_textview_get_text(tv), "line one") == 0);

    /* Commit at the home caret (set_text resets the caret to the
     * document start — the programmatic-reset contract). */
    e = ime_evt(FDK_EVENT_IME_COMMIT, "!", 0);
    assert(fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_textview_get_text(tv), "!line one") == 0);

    /* Read-only: refuses the commit (falls through). */
    fdk_textview_set_read_only(tv, true);
    e = ime_evt(FDK_EVENT_IME_COMMIT, "?", 0);
    assert(!fdk_widget_tree_handle_event(root, &e));
    assert(strcmp(fdk_textview_get_text(tv), "!line one") == 0);

    fdk_widget_destroy(root);
}

/* ---- 3. the surrounding-text context queries ---------------------------- */

static void test_ime_context_queries(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *entry = NULL;
    fdk_widget *button = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL, "hola", &entry)));
    assert(fdk_ok(fdk_button_create(root, NULL, "go", &button)));

    /* NULL focus answers 0. */
    const char *text = NULL;
    int cursor = -1;
    assert(fdk__widget_ime_context(NULL, &text, &cursor) == 0);

    /* A focused entry answers its buffer + caret (bytes). */
    assert(fdk_widget_focus(entry));
    assert(fdk_entry_select_range(entry, 2, 2) == FDK_OK);
    assert(fdk__widget_ime_context(entry, &text, &cursor) == 1);
    assert(text != NULL && strcmp(text, "hola") == 0);
    assert(cursor == 2);

    /* A non-text widget answers 0. */
    assert(fdk_widget_focus(button));
    assert(fdk__widget_ime_context(button, &text, &cursor) == 0);

    /* A textview answers through the same dispatcher. */
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_textview_set_text(tv, "abcdef");
    assert(fdk_widget_focus(tv));
    assert(fdk__widget_ime_context(tv, &text, &cursor) == 1);
    assert(strcmp(text, "abcdef") == 0);
    assert(cursor == 0); /* set_text resets the textview caret home */

    /* Multi-byte carets ride BYTE offsets (the API contract). */
    fdk_widget *entry2 = NULL;
    assert(fdk_ok(fdk_entry_create(root, NULL,
                                   "\xe4\xbd\xa0\xe5\xa5\xbd", &entry2)));
    assert(fdk_widget_focus(entry2));
    assert(fdk_entry_select_range(entry2, 3, 3) == FDK_OK);
    assert(fdk__widget_ime_context(entry2, &text, &cursor) == 1);
    assert(cursor == 3); /* after 你, a codepoint boundary in bytes */

    fdk_widget_destroy(root);
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    printf("== test_ime: X11 composition buffer math ==\n");
    test_apply_insert_into_empty();
    test_apply_insert_middle();
    test_apply_replace_span();
    test_apply_delete_span();
    test_apply_cjk_char_indices();
    test_apply_emoji_four_byte();
    test_apply_insert_after_emoji();
    test_apply_caret_move_is_noop();
    test_apply_delete_past_end_clips();
    test_apply_append_at_end();
    test_apply_first_beyond_refused();
    test_apply_negative_indices_refused();
    test_apply_overflow_refused();
    test_apply_cjk_full_composition_cycle();
    printf("[ok] 14 buffer-math shapes (insert/replace/delete/clip/"
           "refuse, CJK+emoji char-vs-byte, the pinyin cycle)\n");

    printf("== test_ime: widget routing ==\n");
    test_entry_preedit_rides_set_preedit();
    test_entry_commit_splices_at_caret();
    test_entry_commit_replaces_selection();
    test_entry_commit_is_undoable();
    test_entry_read_only_refuses_commit();
    test_entry_disabled_refuses_both();
    test_no_focus_falls_through();
    test_non_text_focus_falls_through();
    test_textview_round();
    printf("[ok] entry/textview consume PREEDIT+COMMIT (caret splice,"
           " selection replace, undoable); read-only/disabled/"
           "no-focus/non-text fall through\n");

    printf("== test_ime: surrounding-text context ==\n");
    test_ime_context_queries();
    printf("[ok] fdk__widget_ime_context: entry/textview answer"
           " (text, caret-bytes); button/NULL answer 0\n");

    printf("all IME tests passed\n");
    return 0;
}
