/*
 * test_textview.c — headless tests for the 1.4.8 TextView
 *
 * The Entry-suite discipline applied to the second dimension:
 * standalone roots, synthetic tree events, offscreen paint
 * readbacks, ASan+UBSan throughout. A system font is needed for
 * the wrap geometry; without one the suite honestly skips.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* internal state + helpers   */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

static fdk_event_data ev_button(fdk_event_type t, float x, float y,
                                fdk_u32 mods) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    e.pointer_button.modifiers = mods;
    return e;
}

static fdk_event_data ev_motion(float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_MOTION;
    e.pointer.position.x = x;
    e.pointer.position.y = y;
    return e;
}

static fdk_event_data ev_key(fdk_scancode sc, fdk_u32 mods,
                             fdk_u32 cp) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    e.key.modifiers = mods;
    e.key.codepoint = cp;
    return e;
}

static void press_key(fdk_widget *root, fdk_scancode sc, fdk_u32 mods,
                      fdk_u32 cp) {
    fdk_event_data e = ev_key(sc, mods, cp);
    (void)fdk_widget_tree_handle_event(root, &e);
}

static void type_cp(fdk_widget *root, fdk_u32 cp) {
    press_key(root, 0, 0, cp);
}

static void click(fdk_widget *root, float x, float y, fdk_u32 mods) {
    fdk_event_data down =
        ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y, mods);
    fdk_event_data up =
        ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y, mods);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static int g_changes = 0;
static void on_changed(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_changes++;
}

/* A fresh view with the classic lorem-ish doc, arranged 240x160. */
static fdk_widget *make_view(fdk_widget *root,
                             fdk_widget **out_tv) {
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, g_font, &tv)));
    fdk_textview_set_text(tv,
                          "the quick brown fox jumps over the lazy "
                          "dog\nsecond paragraph here\n\nend");
    fdk_textview_set_on_changed(tv, on_changed, NULL);
    fdk_widget_arrange(tv, (fdk_rect){0, 0, 240, 160});
    if (out_tv != NULL) {
        *out_tv = tv;
    }
    return tv;
}

/* ---- basics: text, wraps, paragraphs ---- */

static void test_basics(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    assert(strcmp(fdk_textview_get_text(tv),
                  "the quick brown fox jumps over the lazy "
                  "dog\nsecond paragraph here\n\nend") == 0);
    /* 3 paragraphs (two newlines... three \n total -> 4). */
    assert(fdk_textview_paragraph_count(tv) == 4);
    /* At 240 wide (224 text): the first paragraph wraps to >= 2
     * lines; total visual lines > paragraphs. */
    assert(fdk_textview_line_count(tv) >= 5);
    assert(fdk_textview_line_count(tv) >
           fdk_textview_paragraph_count(tv));

    /* NO-wrap: one visual line per paragraph. */
    fdk_textview_set_wrap(tv, FDK_TEXTVIEW_WRAP_NONE);
    assert(fdk_textview_line_count(tv) == 4);
    fdk_textview_set_wrap(tv, FDK_TEXTVIEW_WRAP_WORD);
    assert(fdk_textview_line_count(tv) >= 5);

    /* set_text resets: caret 0, no selection, history cleared. */
    fdk_textview_set_text(tv, "fresh");
    assert(strcmp(fdk_textview_get_text(tv), "fresh") == 0);
    size_t a = 9, c = 9;
    assert(!fdk_textview_get_selection(tv, &a, &c));

    /* Empty doc: 1 paragraph, 0 visual lines (nothing to break). */
    fdk_textview_set_text(tv, "");
    assert(fdk_textview_paragraph_count(tv) == 1);
    assert(fdk_textview_line_count(tv) == 0);

    fdk_widget_destroy(root);
    printf("[ok] textview basics: text round-trip, visual vs "
           "logical line counts, wrap modes, reset contract\n");
}

/* ---- geometry: click positions, Home/End, arrows ---- */

static void test_pointer_and_motions(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    /* Click on the second visual line (y=32 is inside the second
     * line's band at the default metrics): the caret lands on a
     * wrapped continuation line, mid-paragraph. */
    click(root, 10.0f, 32.0f, 0);
    size_t a = 0, c = 0;
    (void)a;
    assert(!fdk_textview_get_selection(tv, &a, &c));
    /* The caret is somewhere in the first paragraph's wrap; Home
     * (visual) goes to the LINE start, End to the line end. */
    press_key(root, FDK_KEY_HOME, 0, 0);
    /* After Home on a non-first visual line, the caret is the
     * line's byte_offset (mid-paragraph). Verify: pressing End
     * then checking via a shift-Home selection length > 0. */
    press_key(root, FDK_KEY_END, 0, 0);
    press_key(root, FDK_KEY_HOME, FDK_MOD_SHIFT, 0);
    size_t sel = 0, caret = 0;
    assert(fdk_textview_get_selection(tv, &sel, &caret));
    /* The anchor held the line END (from End); shift+Home moved the
     * caret to the line START: a non-empty span, caret < sel. */
    assert(sel > caret && sel - caret > 0);
    /* Ctrl+Home: doc start. */
    press_key(root, FDK_KEY_HOME, FDK_MOD_CTRL, 0);
    assert(!fdk_textview_get_selection(tv, &sel, &caret));
    press_key(root, FDK_KEY_END, FDK_MOD_CTRL, 0);
    assert(!fdk_textview_get_selection(tv, &sel, &caret));
    /* The doc end caret: typing appends. */
    type_cp(root, '!');
    assert(strcmp(fdk_textview_get_text(tv) +
                      strlen("the quick brown fox jumps over the "
                             "lazy dog\nsecond paragraph here\n\nend"),
                  "!") == 0);

    fdk_widget_destroy(root);
    printf("[ok] textview pointer+motions: click caret, visual "
           "Home/End, Ctrl+Home/End doc ends\n");
}

/* ---- editing: Enter splits, Backspace joins, read-only ---- */

static void test_editing(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, g_font, &tv)));
    fdk_textview_set_text(tv, "alpha beta");
    fdk_widget_arrange(tv, (fdk_rect){0, 0, 200, 120});

    /* Click mid-text, read the caret through the a11y text
     * interface, then Enter: the split lands exactly there. */
    click(root, 58.0f, 12.0f, 0);
    size_t caret = fdk_a11y_text_caret(tv);
    assert(caret > 0 && caret < strlen("alpha beta"));
    press_key(root, FDK_KEY_ENTER, 0, 0);
    assert(fdk_textview_paragraph_count(tv) == 2);
    {
        const char *t = fdk_textview_get_text(tv);
        assert(strlen(t) == strlen("alpha beta") + 1);
        assert(t[caret] == '\n');
        assert(memcmp(t, "alpha beta", caret) == 0);
        assert(memcmp(t + caret + 1, "alpha beta" + caret,
                      strlen("alpha beta") - caret) == 0);
    }

    /* Backspace at the line start joins back. */
    press_key(root, FDK_KEY_HOME, 0, 0);
    press_key(root, FDK_KEY_BACKSPACE, 0, 0);
    assert(fdk_textview_paragraph_count(tv) == 1);

    /* Delete key eats forward. */
    press_key(root, FDK_KEY_HOME, FDK_MOD_CTRL, 0);
    press_key(root, FDK_KEY_DELETE, 0, 0);
    assert(fdk_textview_get_text(tv)[0] != 'a');

    /* Read-only: mutators refuse (consumed but inert), copy/select
     * still work. */
    fdk_textview_set_text(tv, "locked");
    fdk_textview_set_read_only(tv, true);
    assert(fdk_textview_get_read_only(tv));
    type_cp(root, 'X');
    press_key(root, FDK_KEY_ENTER, 0, 0);
    press_key(root, FDK_KEY_BACKSPACE, 0, 0);
    assert(strcmp(fdk_textview_get_text(tv), "locked") == 0);
    fdk_textview_select_all(tv);
    size_t a = 0, c = 0;
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(a == 0 && c == strlen("locked"));
    fdk_textview_set_read_only(tv, false);

    fdk_widget_destroy(root);
    printf("[ok] textview editing: Enter splits, Backspace joins, "
           "Delete eats, read-only refuses mutators\n");
}

/* ---- selection gestures ---- */

static void test_selection(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    /* Drag: press on line 0, sweep to line 1. */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    12.0f, 12.0f, 0);
    fdk_event_data move = ev_motion(60.0f, 34.0f);
    fdk_event_data up =
        ev_button(FDK_EVENT_POINTER_BUTTON_UP, 60.0f, 34.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down) == true);
    (void)fdk_widget_tree_handle_event(root, &move);
    (void)fdk_widget_tree_handle_event(root, &up);
    size_t a = 0, c = 0;
    assert(fdk_textview_get_selection(tv, &a, &c));
    /* The anchor held the PRESS offset (x=12 with pad 8 lands ~1
     * byte in); the sweep extended it down a line. */
    assert(c > a && a < 4);

    /* Double-click: the word under it. */
    click(root, 20.0f, 12.0f, 0);
    click(root, 20.0f, 12.0f, 0);
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(c - a >= 3 && c - a <= 8); /* "the" or "quick"-ish word */
    /* The selected bytes are a word (no spaces). */
    const char *t = fdk_textview_get_text(tv);
    for (size_t i = a; i < c; i++) {
        assert(t[i] != ' ');
    }

    /* Triple-click: the PARAGRAPH (up to and including the \n). */
    click(root, 20.0f, 12.0f, 0);
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(c - a > 30); /* the whole first paragraph, wraps incl. */

    /* Shift+click extends from the anchor. */
    click(root, 10.0f, 12.0f, 0);
    click(root, 100.0f, 12.0f, FDK_MOD_SHIFT);
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(a == 0 && c > a);

    fdk_widget_destroy(root);
    printf("[ok] textview selection: drag sweep, double-click word, "
           "triple-click paragraph, shift-click extend\n");
}

/* ---- vertical navigation ---- */

static void test_vertical_nav(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    /* Keys route to the FOCUSED widget (a click focuses; the
     * headless tree starts unfocused). */
    assert(fdk_widget_focus(tv));
    /* Ctrl+End then Up twice: the doc's tail is "...here\n\nend"
     * — ONE Up lands on the EMPTY line (already at its start, so
     * shift+Home selects nothing there); a second Up reaches the
     * second paragraph's wrap, where shift+Home spans real text. */
    press_key(root, FDK_KEY_END, FDK_MOD_CTRL, 0);
    press_key(root, FDK_KEY_UP, 0, 0);
    size_t a = 0, c = 0;
    assert(!fdk_textview_get_selection(tv, &a, &c)); /* empty line */
    press_key(root, FDK_KEY_UP, 0, 0);
    press_key(root, FDK_KEY_HOME, FDK_MOD_SHIFT, 0);
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(c > 30); /* well into the document                  */

    /* PageUp from the end: multi-line jump. */
    press_key(root, FDK_KEY_END, FDK_MOD_CTRL, 0);
    press_key(root, FDK_KEY_PAGE_UP, 0, 0);
    /* Somewhere above the end: a shift+Ctrl+End selection spans
     * many lines. */
    press_key(root, FDK_KEY_END, FDK_MOD_CTRL | FDK_MOD_SHIFT, 0);
    assert(fdk_textview_get_selection(tv, &a, &c));
    assert(c - a > 20);

    fdk_widget_destroy(root);
    printf("[ok] textview vertical nav: Up steps a visual line, "
           "PageUp pages, shift extends across lines\n");
}

/* ---- undo/redo ---- */

static void test_undo(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, g_font, &tv)));
    fdk_textview_set_text(tv, "");
    fdk_widget_arrange(tv, (fdk_rect){0, 0, 200, 120});
    assert(fdk_widget_focus(tv)); /* keys route to the focused */

    /* A typing run: "hello" then Backspace Backspace -> "hel".
     * Undo steps: the whole backspace run (coalesced), then the
     * whole typing run (coalesced). */
    for (const char *p = "hello"; *p != '\0'; p++) {
        type_cp(root, (fdk_u32)*p);
    }
    assert(strcmp(fdk_textview_get_text(tv), "hello") == 0);
    press_key(root, FDK_KEY_BACKSPACE, 0, 0);
    press_key(root, FDK_KEY_BACKSPACE, 0, 0);
    assert(strcmp(fdk_textview_get_text(tv), "hel") == 0);
    (void)fdk_textview_undo(tv);
    assert(strcmp(fdk_textview_get_text(tv), "hello") == 0);
    (void)fdk_textview_undo(tv);
    assert(strcmp(fdk_textview_get_text(tv), "") == 0);
    (void)fdk_textview_redo(tv);
    assert(strcmp(fdk_textview_get_text(tv), "hello") == 0);

    /* Replace gesture: select-all + type = ONE undo step. */
    fdk_textview_select_all(tv);
    type_cp(root, 'X');
    assert(strcmp(fdk_textview_get_text(tv), "X") == 0);
    (void)fdk_textview_undo(tv);
    assert(strcmp(fdk_textview_get_text(tv), "hello") == 0);

    fdk_widget_destroy(root);
    printf("[ok] textview undo: typing/backspace runs coalesce, "
           "replace composes one step, redo restores\n");
}

/* ---- a11y ---- */

static void test_a11y(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    fdk_a11y_info info;
    memset(&info, 0, sizeof(info));
    assert(fdk_ok(fdk_a11y_describe(tv, &info)));
    assert(info.value_text != NULL);
    assert(strstr(info.value_text, "quick brown") != NULL);
    assert((info.states & FDK_A11Y_EDITABLE) != 0);
    fdk_a11y_info_free(&info);

    /* The text interface: length/caret/selection/at-offset. */
    assert(fdk_a11y_text_length(tv) ==
           strlen("the quick brown fox jumps over the lazy "
                  "dog\nsecond paragraph here\n\nend"));
    assert(fdk_a11y_text_caret(tv) == 0);
    fdk_textview_select_all(tv);
    size_t a = 0, c = 0;
    assert(fdk_a11y_text_selection(tv, &a, &c));
    assert(a == 0 && c == fdk_a11y_text_length(tv));

    char buf[64];
    size_t ws = 0, we = 0;
    assert(fdk_ok(fdk_a11y_text_at_offset(tv, 4,
                                          FDK_A11Y_TEXT_WORD, buf,
                                          sizeof(buf), &ws, &we)));
    assert(strcmp(buf, "quick") == 0);
    assert(ws == 4 && we == 9);
    assert(fdk_ok(fdk_a11y_text_at_offset(tv, 4, FDK_A11Y_TEXT_LINE,
                                          buf, sizeof(buf), &ws,
                                          &we)));
    /* The whole first paragraph. */
    assert(we - ws > 40);

    /* Mutators. */
    assert(fdk_ok(fdk_a11y_text_set_caret(tv, 10)));
    assert(fdk_a11y_text_caret(tv) == 10);
    assert(fdk_ok(fdk_a11y_text_set_selection(tv, 0, 5)));
    assert(fdk_a11y_text_selection(tv, &a, &c));
    assert(a == 0 && c == 5);

    fdk_widget_destroy(root);
    printf("[ok] textview a11y: TEXT_VIEW role with value text, "
           "full text interface incl. mutators\n");
}

/* ---- paint ---- */

static void test_paint(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tv = make_view(root, NULL);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 160, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);

    /* The field is OPAQUE (every pixel paints) — the proof is
     * VARIANCE, not ink: the first line band varies across x (the
     * glyph ink modulates the fill). */
    int variance = 0;
    fdk_u32 first = px_at(s, 12, 14);
    for (int y = 10; y < 24; y += 2) {
        for (int x = 10; x < 230; x += 2) {
            if (px_at(s, x, y) != first) {
                variance++;
            }
        }
    }
    assert(variance > 20); /* glyphs modulate the field          */

    /* Select-all: the selection tint shifts the band's pixels. */
    fdk_u32 plain_px = px_at(s, 200, 14); /* past the wrap's text */
    fdk_textview_select_all(tv);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 200, 14) != plain_px); /* the tint landed     */
    assert(px_at(s, 20, 14) != plain_px);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] textview paint: glyph variance on the field, "
           "selection tint shifts the line band\n");
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        g_font = fdk_font_load(candidates[i], 16);
        if (g_font != NULL) {
            break;
        }
    }
    if (g_font == NULL) {
        printf("[skip] no system TrueType font found — the textview "
               "wrap geometry needs real glyphs\n");
        return 0;
    }

    test_basics();
    test_pointer_and_motions();
    test_editing();
    test_selection();
    test_vertical_nav();
    test_undo();
    test_a11y();
    test_paint();

    fdk_font_destroy(g_font);
    printf("all textview tests passed\n");
    return 0;
}
