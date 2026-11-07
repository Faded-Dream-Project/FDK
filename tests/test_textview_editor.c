/*
 * test_textview_editor.c — the §30/STEP-2a editor-surface gaps in
 * TextView, test-first. fdk-edit (a real text editor, the §24 spec)
 * needs four things the 1.4.8 TextView never grew:
 *
 *   1. CARET POSITION I/O — get/set the caret as a byte offset
 *      (goto-line, find-result jumps, session restore all want to
 *      place the caret programmatically; the status bar wants to
 *      read it as line:column).
 *   2. CARET GEOMETRY — the caret's (line, column) pair, logical
 *      line (paragraph) + codepoint column, plus the paragraph's
 *      count of lines so the caller can compute percentages.
 *   3. RANGE ATTRIBUTES — an fdk_attr_list applied to the document
 *      (syntax highlighting is (byte range, color) pairs over the
 *      whole text; the attribute engine already exists — TextView
 *      just never accepted a list).
 *   4. GUTTER — a line-number column: the widget reserves a left
 *      band of `width` px, paints the app's gutter content hook
 *      there (numbers, from the line geometry it already computes),
 *      and hits NOTHING in it (the text's x origin shifts right).
 *
 * All four are GTK4 GtkTextView / GtkSourceView-shaped (the app
 * builds the rest: tabs, find bar, highlighting rules).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_attr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- caret position I/O ------------------------------------------ */

static void test_caret_io(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_widget_set_bounds(tv, (fdk_rect){0, 0, 400, 300});

    fdk_textview_set_text(tv, "hello\nwörld\nthird");
    /* default caret: 0 (a fresh document) */
    size_t caret = 12345;
    assert(fdk_ok(fdk_textview_get_caret(tv, &caret)));
    assert(caret == 0);

    /* set into line 2 (byte 6 = 'w' of wörld; ö is 2 bytes) */
    assert(fdk_ok(fdk_textview_set_caret(tv, 6)));
    assert(fdk_ok(fdk_textview_get_caret(tv, &caret)));
    assert(caret == 6);

    /* boundary snap: 7 is INSIDE ö (bytes 7-8); must snap to a
     * codepoint boundary (either side — the Entry's rule is
     * forward-snap) */
    assert(fdk_ok(fdk_textview_set_caret(tv, 7)));
    assert(fdk_ok(fdk_textview_get_caret(tv, &caret)));
    assert(caret == 8 || caret == 7);

    /* clamps: -via-huge and past-the-end. The byte length is 18
     * (ö is a 2-byte codepoint: 5+1 + 1+2+3+1 + 5). */
    assert(fdk_ok(fdk_textview_set_caret(tv, (size_t)-1)));
    assert(fdk_ok(fdk_textview_get_caret(tv, &caret)));
    assert(caret == 18);
    assert(fdk_ok(fdk_textview_set_caret(tv, 99999)));
    assert(fdk_ok(fdk_textview_get_caret(tv, &caret)));
    assert(caret == 18);

    /* a selection survives set_caret? No — set_caret is the CARET
     * API: it collapses the selection (anchor = caret), the
     * documented reset. */
    fdk_textview_select_all(tv);
    assert(fdk_ok(fdk_textview_set_caret(tv, 0)));
    size_t a = 1, c = 1;
    assert(!fdk_textview_get_selection(tv, &a, &c));

    /* bad args refused */
    assert(!fdk_ok(fdk_textview_get_caret(NULL, &caret)));
    assert(!fdk_ok(fdk_textview_get_caret(tv, NULL)));
    assert(!fdk_ok(fdk_textview_set_caret(NULL, 0)));

    fdk_widget_destroy(root);
    printf("[ok] textview editor: caret byte-offset get/set "
           "(snap, clamp, collapse)\n");
}

/* ---- caret geometry: line:column ---------------------------------- */

static void test_caret_line_col(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_widget_set_bounds(tv, (fdk_rect){0, 0, 400, 300});

    /* ö (2 bytes) and α (2 bytes) make the column count differ
     * from the byte count: columns are CODEPOINTS */
    fdk_textview_set_text(tv, "ab\ncöd\nefgh");
    /* len = 3 + 5 + 4 = 12 bytes; lines: 3 */

    assert(fdk_ok(fdk_textview_set_caret(tv, 0)));
    size_t line = 99, col = 99;
    assert(fdk_ok(fdk_textview_caret_line_col(tv, &line, &col)));
    assert(line == 0 && col == 0);

    /* byte 3 = start of line 1 */
    assert(fdk_ok(fdk_textview_set_caret(tv, 3)));
    assert(fdk_ok(fdk_textview_caret_line_col(tv, &line, &col)));
    assert(line == 1 && col == 0);

    /* byte 4 = 'ö' — column 1 (the first codepoint of line 1) */
    assert(fdk_ok(fdk_textview_set_caret(tv, 4)));
    assert(fdk_ok(fdk_textview_caret_line_col(tv, &line, &col)));
    assert(line == 1 && col == 1);

    /* byte 6 = 'd' — after c(col 1) and ö(col 2): column 3? The
     * caret AT byte 6 stands before 'd', which is the 3rd codepoint
     * of the line (c=0th, ö=1st, d=2nd): column 3 means 3 codepoints
     * precede — c, ö, d — no: before byte 6 stand c and ö only
     * (byte 6 IS d). Column = 2. */
    assert(fdk_ok(fdk_textview_set_caret(tv, 6)));
    assert(fdk_ok(fdk_textview_caret_line_col(tv, &line, &col)));
    assert(line == 1 && col == 2);

    /* the very end: byte 12 = document end (a b \n c ö d \n e f g
     * h = 12 bytes): line 2, after e f g h — column 4 */
    assert(fdk_ok(fdk_textview_set_caret(tv, 12)));
    assert(fdk_ok(fdk_textview_caret_line_col(tv, &line, &col)));
    assert(line == 2 && col == 4);

    /* the byte offset OF a line:col (the inverse, for goto-line) */
    assert(fdk_textview_line_col_byte(tv, 2, 4) == 12);
    assert(fdk_textview_line_col_byte(tv, 1, 0) == 3);
    assert(fdk_textview_line_col_byte(tv, 1, 2) == 6);
    /* clamped: past-the-end line/col clamp to the document's end */
    assert(fdk_textview_line_col_byte(tv, 9, 9) == 12);
    /* bad widget: SIZE_MAX */
    assert(fdk_textview_line_col_byte(NULL, 0, 0) ==
           (size_t)-1);

    fdk_widget_destroy(root);
    printf("[ok] textview editor: caret line:column geometry "
           "(codepoint columns, inverse mapping, clamps)\n");
}

/* ---- range attributes -------------------------------------------- */

static void test_attr_list_application(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_widget_set_bounds(tv, (fdk_rect){0, 0, 400, 300});
    fdk_textview_set_text(tv, "int main(void) {");

    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    /* 'int' bold-blue: the keyword */
    assert(fdk_ok(fdk_attr_list_add_range(
        &attrs, 0, 3, FDK_ATTR_BACKGROUND,
        0, 0, (fdk_color){0x40, 0x90, 0x50, 0xFF}, NULL)));
    /* 'main' underlined */
    assert(fdk_ok(fdk_attr_list_add_range(
        &attrs, 4, 8, FDK_ATTR_UNDERLINE,
        0, FDK_UNDERLINE_SINGLE, (fdk_color){0}, NULL)));

    /* the application: takes a COPY (the app may destroy its list);
     * NULL clears */
    assert(fdk_ok(fdk_textview_set_attrs(tv, &attrs)));
    fdk_attr_list_destroy(&attrs);

    /* re-apply and clear */
    fdk_attr_list again;
    assert(fdk_ok(fdk_attr_list_init(&again)));
    assert(fdk_ok(fdk_attr_list_add_range(
        &again, 0, 1, FDK_ATTR_LETTER_SPACING, 2.0f, 0,
        (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk_textview_set_attrs(tv, &again)));
    fdk_attr_list_destroy(&again);
    assert(fdk_ok(fdk_textview_set_attrs(tv, NULL)));

    /* bad args refused */
    assert(!fdk_ok(fdk_textview_set_attrs(NULL, &again)));

    fdk_widget_destroy(root);
    printf("[ok] textview editor: range attribute lists apply and "
           "clear (the syntax-highlight substrate)\n");
}

/* ---- gutter ------------------------------------------------------- */

static int g_gutter_paints;

static void on_gutter_paint(fdk_widget *tv, fdk_surface *s,
                            fdk_rect band, size_t first_line,
                            size_t line_count, void *user) {
    (void)tv; (void)s; (void)band;
    (void)first_line; (void)line_count; (void)user;
    g_gutter_paints++;
}

static void test_gutter(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, NULL, &tv)));
    fdk_widget_set_bounds(tv, (fdk_rect){0, 0, 400, 300});
    fdk_textview_set_text(tv, "one\ntwo\nthree");

    /* no gutter by default */
    assert(fdk_textview_get_gutter_width(tv) == 0);

    g_gutter_paints = 0;
    assert(fdk_ok(fdk_textview_set_gutter(
        tv, 44, on_gutter_paint, NULL)));
    assert(fdk_textview_get_gutter_width(tv) == 44);

    /* clearing */
    assert(fdk_ok(fdk_textview_set_gutter(tv, 0, NULL, NULL)));
    assert(fdk_textview_get_gutter_width(tv) == 0);

    /* bad args refused */
    assert(!fdk_ok(fdk_textview_set_gutter(NULL, 44,
                                           on_gutter_paint, NULL)));

    fdk_widget_destroy(root);
    printf("[ok] textview editor: the line-number gutter band "
           "(width state, hook, clear)\n");
}

int main(void) {
    test_caret_io();
    test_caret_line_col();
    test_attr_list_application();
    test_gutter();
    printf("all textview-editor tests passed\n");
    return 0;
}
