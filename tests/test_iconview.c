/*
 * test_iconview.c — headless tests for the 1.4.5 IconView
 *
 * Same discipline as the other catalog suites: standalone roots,
 * offscreen surfaces, synthetic window events through
 * fdk_widget_tree_handle_event, ASan+UBSan throughout. A system
 * font is needed for the label geometry; without one the suite
 * honestly skips.
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

static fdk_event_data ev_key(fdk_scancode sc, fdk_u32 mods) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    e.key.modifiers = mods;
    return e;
}

static void click(fdk_widget *root, float x, float y, fdk_u32 mods) {
    fdk_event_data down =
        ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y, mods);
    fdk_event_data up =
        ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y, mods);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

static void press_key(fdk_widget *root, fdk_scancode sc, fdk_u32 mods) {
    fdk_event_data e = ev_key(sc, mods);
    (void)fdk_widget_tree_handle_event(root, &e);
}

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static int g_sel_changes = 0;
static void on_sel(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_sel_changes++;
}

static int g_activates = 0;
static size_t g_last_activated = 999;
static void on_activate(fdk_widget *w, size_t index, void *user) {
    (void)w;
    (void)user;
    g_activates++;
    g_last_activated = index;
}

/* A fresh 7-item view arranged 200x200 (2 columns at 96+4 step). */
static fdk_widget *make_view(fdk_widget *root, fdk_widget **out_iv) {
    fdk_widget *iv = NULL;
    assert(fdk_ok(fdk_iconview_create(root, g_font, &iv)));
    fdk_iconview_begin_batch(iv);
    for (int i = 0; i < 7; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Item %d", i);
        (void)fdk_iconview_append(
            iv, label,
            (i % 3 == 0) ? FDK_ROW_ICON_FOLDER : FDK_ROW_ICON_FILE);
    }
    fdk_iconview_end_batch(iv);
    /* Tall enough to show all four rows (7 items, 2 columns). */
    fdk_widget_arrange(iv, (fdk_rect){0, 0, 200, 400});
    if (out_iv != NULL) {
        *out_iv = iv;
    }
    return iv;
}

/* ---- basics + geometry ---- */

static void test_basics_and_grid(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = NULL;
    assert(fdk_ok(fdk_iconview_create(root, g_font, &iv)));
    assert(fdk_iconview_item_count(iv) == 0);
    assert(fdk_iconview_append(root, "nope", FDK_ROW_ICON_FILE) == 0);
    assert(fdk_iconview_append(NULL, "x", FDK_ROW_ICON_FILE) == 0);
    assert(fdk_iconview_item_label(NULL, 0) == NULL);
    assert(fdk_iconview_item_icon(iv, 0) == FDK_ROW_ICON_NONE);

    size_t idx = fdk_iconview_append(iv, "Alpha", FDK_ROW_ICON_FOLDER);
    assert(idx == 0);
    idx = fdk_iconview_append(iv, "Beta", FDK_ROW_ICON_HOME);
    assert(idx == 1);
    assert(fdk_iconview_item_count(iv) == 2);
    assert(strcmp(fdk_iconview_item_label(iv, 0), "Alpha") == 0);
    assert(fdk_iconview_item_icon(iv, 1) == FDK_ROW_ICON_HOME);
    assert(fdk_iconview_set_item_label(iv, 1, "Beta Prime") == FDK_OK);
    assert(strcmp(fdk_iconview_item_label(iv, 1), "Beta Prime") == 0);
    assert(fdk_iconview_set_item_icon(iv, 1, FDK_ROW_ICON_DRIVE) ==
           FDK_OK);
    assert(fdk_iconview_item_icon(iv, 1) == FDK_ROW_ICON_DRIVE);
    assert(fdk_iconview_item_label(iv, 9) == NULL);
    assert(fdk_iconview_set_item_label(iv, 9, "x") ==
           FDK_ERR_INVALID_ARGUMENT);

    /* 200 wide, cell 96 + gap 4: TWO columns. Seven items -> four
     * rows; slot math: (col*100, row*88). The cells are the grid
     * container's children under iconview -> scrollview -> grid. */
    for (int i = 2; i < 7; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Item %d", i);
        (void)fdk_iconview_append(iv, label, FDK_ROW_ICON_FILE);
    }
    fdk_widget_arrange(iv, (fdk_rect){0, 0, 200, 400});
    fdk_widget *scroll = fdk_widget_child_at(iv, 0);
    fdk_widget *grid = fdk_widget_child_at(scroll, 0);
    assert(grid != NULL && fdk_widget_child_count(grid) == 7);
    fdk_rect c0 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 0));
    fdk_rect c1 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 1));
    fdk_rect c2 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 2));
    fdk_rect c6 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 6));
    assert(c0.x == 0 && c0.y == 0 && c0.width == 96 && c0.height == 84);
    assert(c1.x == 100 && c1.y == 0);   /* second column           */
    assert(c2.x == 0 && c2.y == 88);    /* wrapped to row 2        */
    assert(c6.x == 0 && c6.y == 3 * 88);   /* 7th: row 4, column 1  */

    /* Re-flow: a 310-wide slot fits THREE columns. */
    fdk_widget_arrange(iv, (fdk_rect){0, 0, 310, 400});
    c2 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 2));
    c6 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 6));
    assert(c2.x == 200 && c2.y == 0);   /* third column now        */
    assert(c6.x == 0 && c6.y == 2 * 88);

    /* set_item_size: bounds + validation. */
    assert(fdk_iconview_set_item_size(iv, 120, 100) == FDK_OK);
    c1 = fdk_widget_get_bounds(fdk_widget_child_at(grid, 1));
    assert(c1.width == 120 && c1.height == 100);
    assert(fdk_iconview_set_item_size(iv, 10, 100) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_iconview_set_item_size(NULL, 100, 100) ==
           FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    printf("[ok] iconview basics: append/label/icon getters and "
           "setters, 2-column slot math, re-flow to 3 columns, "
           "item_size + validation\n");
}

/* ---- selection ---- */

static void test_selection(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = make_view(root, NULL);
    fdk_widget *scroll = fdk_widget_child_at(iv, 0);
    fdk_widget *grid = fdk_widget_child_at(scroll, 0);
    g_sel_changes = 0;
    fdk_iconview_set_on_selection_changed(iv, on_sel, NULL);

    /* Cold start: nothing selected, get_selected -1. */
    assert(fdk_iconview_selected_count(iv) == 0);
    assert(fdk_iconview_get_selected(iv) == -1);

    /* Click item 2 (slot (0, 88)): plain select. */
    click(root, 48.0f, 88.0f + 42.0f, 0);
    assert(fdk_iconview_is_selected(iv, 2));
    assert(fdk_iconview_selected_count(iv) == 1);
    assert(fdk_iconview_get_selected(iv) == 2);
    assert(g_sel_changes == 1);

    /* MULTIPLE: ctrl+click item 4 (slot (0, 176)) unions. */
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_MULTIPLE);
    click(root, 48.0f, 176.0f + 42.0f, FDK_MOD_CTRL);
    assert(fdk_iconview_selected_count(iv) == 2);
    assert(fdk_iconview_is_selected(iv, 4));
    /* ctrl again toggles OFF. */
    click(root, 48.0f, 176.0f + 42.0f, FDK_MOD_CTRL);
    assert(fdk_iconview_selected_count(iv) == 1);

    /* shift+click item 5 (slot (100, 176)): range 2..5 replaces. */
    click(root, 148.0f, 176.0f + 42.0f, FDK_MOD_SHIFT);
    assert(fdk_iconview_selected_count(iv) == 4);
    assert(fdk_iconview_is_selected(iv, 2));
    assert(fdk_iconview_is_selected(iv, 3));
    assert(fdk_iconview_is_selected(iv, 4));
    assert(fdk_iconview_is_selected(iv, 5));
    /* Enumerate in order. */
    size_t at = 0;
    assert(fdk_iconview_selected_at(iv, 0, &at) == FDK_OK &&
           at == 2);
    assert(fdk_iconview_selected_at(iv, 3, &at) == FDK_OK &&
           at == 5);
    assert(fdk_iconview_selected_at(iv, 4, &at) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Programmatic select/clear. */
    assert(fdk_iconview_select(iv, 6) == FDK_OK);
    assert(fdk_iconview_selected_count(iv) == 1);
    assert(fdk_iconview_is_selected(iv, 6));
    fdk_iconview_clear_selection(iv);
    assert(fdk_iconview_selected_count(iv) == 0);
    assert(fdk_iconview_select(iv, 99) == FDK_ERR_INVALID_ARGUMENT);
    (void)grid;

    /* NONE mode: clicks never select. */
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_NONE);
    click(root, 48.0f, 42.0f, 0);
    assert(fdk_iconview_selected_count(iv) == 0);

    /* Programmatic select is the List's clear-then-one (even in
     * MULTIPLE — the family contract, the doc says so). */
    fdk_widget_destroy(root);
    root = fresh_root();
    iv = make_view(root, NULL);
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_MULTIPLE);
    assert(fdk_iconview_select(iv, 1) == FDK_OK);
    assert(fdk_iconview_select(iv, 3) == FDK_OK);
    assert(fdk_iconview_selected_count(iv) == 1);
    assert(fdk_iconview_get_selected(iv) == 3);

    /* SINGLE collapse from MULTIPLE keeps the first (built with
     * ctrl-clicks, the additive gesture). */
    click(root, 148.0f, 42.0f, FDK_MOD_CTRL); /* item 1 */
    click(root, 48.0f, 176.0f + 42.0f, FDK_MOD_CTRL); /* item 4 */
    assert(fdk_iconview_selected_count(iv) == 3); /* 3 stays from select */
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_SINGLE);
    assert(fdk_iconview_selected_count(iv) == 1);
    assert(fdk_iconview_get_selected(iv) == 1);

    fdk_widget_destroy(root);
    printf("[ok] iconview selection: plain/ctrl/shift in reading "
           "order, enumeration, programmatic select/clear, NONE, "
           "SINGLE collapse\n");
}

/* ---- keyboard ---- */

static void test_keyboard(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = make_view(root, NULL);
    g_activates = 0;
    fdk_iconview_set_on_item_activate(iv, on_activate, NULL);
    (void)fdk_widget_focus(iv);

    /* Cold start: Down selects item 0. Right steps +1; Down steps a
     * whole ROW (columns == 2 in a 200-wide slot). */
    press_key(root, FDK_KEY_DOWN, 0);
    assert(fdk_iconview_get_selected(iv) == 0);
    press_key(root, FDK_KEY_RIGHT, 0);
    assert(fdk_iconview_get_selected(iv) == 1);
    press_key(root, FDK_KEY_DOWN, 0);
    assert(fdk_iconview_get_selected(iv) == 3); /* 1 + 2 columns   */
    press_key(root, FDK_KEY_LEFT, 0);
    assert(fdk_iconview_get_selected(iv) == 2);
    press_key(root, FDK_KEY_UP, 0);
    assert(fdk_iconview_get_selected(iv) == 0);
    press_key(root, FDK_KEY_END, 0);
    assert(fdk_iconview_get_selected(iv) == 6);
    press_key(root, FDK_KEY_HOME, 0);
    assert(fdk_iconview_get_selected(iv) == 0);

    /* Shift+Right extends the selection in MULTIPLE mode. */
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_MULTIPLE);
    press_key(root, FDK_KEY_RIGHT, FDK_MOD_SHIFT);
    press_key(root, FDK_KEY_RIGHT, FDK_MOD_SHIFT);
    assert(fdk_iconview_selected_count(iv) == 3);
    assert(fdk_iconview_is_selected(iv, 0));
    assert(fdk_iconview_is_selected(iv, 1));
    assert(fdk_iconview_is_selected(iv, 2));

    /* Enter activates the cursor. */
    press_key(root, FDK_KEY_ENTER, 0);
    assert(g_activates == 1 && g_last_activated == 2);

    fdk_widget_destroy(root);
    printf("[ok] iconview keyboard: grid arrows (Down = one ROW), "
           "Home/End, shift-extend, Enter activation\n");
}

/* ---- mutation ---- */

static void test_mutation(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = make_view(root, NULL);
    fdk_widget *scroll = fdk_widget_child_at(iv, 0);
    fdk_widget *grid = fdk_widget_child_at(scroll, 0);

    /* Remove item 2: survivors shift; the cells re-index. */
    assert(fdk_iconview_select(iv, 4) == FDK_OK);
    assert(fdk_iconview_remove_item(iv, 2) == FDK_OK);
    assert(fdk_iconview_item_count(iv) == 6);
    assert(strcmp(fdk_iconview_item_label(iv, 2), "Item 3") == 0);
    assert(strcmp(fdk_iconview_item_label(iv, 5), "Item 6") == 0);
    /* The selection followed the shift (old 4 -> now index 3). */
    assert(fdk_iconview_get_selected(iv) == 3);
    assert(fdk_iconview_remove_item(iv, 99) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* The cells' paint/event indices track (item 2's cell now owns
     * "Item 3" — verified through the a11y describe of grid child
     * 2). */
    fdk_a11y_info info;
    memset(&info, 0, sizeof(info));
    assert(fdk_ok(fdk_a11y_describe(fdk_widget_child_at(grid, 2),
                                    &info)));
    assert(info.name != NULL && strcmp(info.name, "Item 3") == 0);
    fdk_a11y_info_free(&info);

    /* clear: everything goes. */
    fdk_iconview_clear(iv);
    assert(fdk_iconview_item_count(iv) == 0);
    assert(fdk_widget_child_count(grid) == 0);
    assert(fdk_iconview_selected_count(iv) == 0);

    /* Batch: 50 appends, one settle. */
    fdk_iconview_begin_batch(iv);
    for (int i = 0; i < 50; i++) {
        char label[32];
        snprintf(label, sizeof(label), "Batch %d", i);
        (void)fdk_iconview_append(iv, label, FDK_ROW_ICON_FILE);
    }
    fdk_iconview_end_batch(iv);
    assert(fdk_iconview_item_count(iv) == 50);
    assert(fdk_widget_child_count(grid) == 50);

    fdk_widget_destroy(root);
    printf("[ok] iconview mutation: remove (survivors, selection "
           "shift, cell re-index), clear, 50-item batch fill\n");
}

/* ---- paint + a11y smoke ---- */

static void test_paint_and_a11y(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = make_view(root, NULL);
    fdk_widget *scroll = fdk_widget_child_at(iv, 0);
    fdk_widget *grid = fdk_widget_child_at(scroll, 0);

    assert(fdk_iconview_select(iv, 1) == FDK_OK);

    /* Paint: the selected cell inks its tint (a scan of the cell's
     * region differs from the unselected item 0's). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 420, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int sel_ink = 0, unsel_ink = 0;
    for (int y = 4; y < 80; y += 2) {
        for (int x = 4; x < 92; x += 2) {
            if (px_at(s, x, y) != 0) {
                unsel_ink++;
            }
            if (px_at(s, 104 + x - 4, y) != 0) {
                sel_ink++;
            }
        }
    }
    assert(sel_ink > unsel_ink); /* the tint + border + glyph     */

    /* a11y: the view is a LIST with a count value text; item 1's
     * cell carries its label + SELECTED. */
    fdk_a11y_info vinfo;
    memset(&vinfo, 0, sizeof(vinfo));
    assert(fdk_ok(fdk_a11y_describe(iv, &vinfo)));
    assert(vinfo.value_text != NULL &&
           strstr(vinfo.value_text, "7 item") != NULL);
    fdk_a11y_info_free(&vinfo);
    fdk_a11y_info iinfo;
    memset(&iinfo, 0, sizeof(iinfo));
    assert(fdk_ok(fdk_a11y_describe(fdk_widget_child_at(grid, 1),
                                    &iinfo)));
    assert(iinfo.name != NULL && strcmp(iinfo.name, "Item 1") == 0);
    assert((iinfo.states & FDK_A11Y_SELECTED) != 0);
    fdk_a11y_info_free(&iinfo);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] iconview paint+a11y: selected-cell tint pixels, "
           "LIST role with item-count value text, item cells as "
           "LIST_ITEM children with SELECTED state\n");
}

/* ---- the rubber-band sweep (1.4.6) ---- */

static void test_rubber_band(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *iv = make_view(root, NULL);
    g_sel_changes = 0;
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_MULTIPLE);
    fdk_iconview_set_on_selection_changed(iv, on_sel, NULL);

    /* A press on EMPTY grid space (the gap right of item 1's cell:
     * cells span x 0..96 and 100..196; x=198 is between-columns
     * gap... at 2 columns x 198 is past both — inside the view's
     * 200px width, on the grid's empty right margin) starts the
     * band; sweeping to (10, 100) covers items 0..3's area. */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    198.0f, 20.0f, 0);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP,
                                  10.0f, 100.0f, 0);
    /* Motion events speak the `pointer` member (a UNION with
     * pointer_button — filling one then memsetting the other wipes
     * both; the dedicated builder avoids the trap). */
    fdk_event_data move;
    memset(&move, 0, sizeof(move));
    move.type = FDK_EVENT_POINTER_MOTION;
    move.pointer.position.x = 10.0f;
    move.pointer.position.y = 100.0f;

    assert(fdk_widget_tree_handle_event(root, &down) == true);
    assert(g_sel_changes == 1); /* the press fired (empty sweep)  */
    (void)fdk_widget_tree_handle_event(root, &move);
    /* The swept rect (content space): x [9,199], y [19,101] —
     * items 0..3 (rows 0-1 of the 2-column grid). */
    assert(fdk_iconview_selected_count(iv) == 4);
    assert(fdk_iconview_is_selected(iv, 0));
    assert(fdk_iconview_is_selected(iv, 1));
    assert(fdk_iconview_is_selected(iv, 2));
    assert(fdk_iconview_is_selected(iv, 3));
    assert(!fdk_iconview_is_selected(iv, 4));
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_sel_changes == 2); /* the release fired once         */
    assert(fdk_iconview_selected_count(iv) == 4); /* settled      */

    /* Ctrl-union: an existing selection survives a zero-extent
     * ctrl sweep (a plain one clears). */
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 198.0f, 20.0f,
                     FDK_MOD_CTRL);
    up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 198.0f, 20.0f,
                   FDK_MOD_CTRL);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(fdk_iconview_selected_count(iv) == 4); /* snapshot kept */
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 198.0f, 20.0f, 0);
    up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 198.0f, 20.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(fdk_iconview_selected_count(iv) == 0); /* plain cleared */

    /* SINGLE mode refuses the band (the press is not consumed). */
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_SINGLE);
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 198.0f, 20.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down) == false);

    /* Band pixels: while the sweep is LIVE, the view paints the
     * accent-tinted rect; released, the gap returns to the root's
     * fill. The root gets an explicit background here so every
     * repaint COVERS the gap (a background-less root never paints,
     * and a test surface is not cleared between frames). */
    fdk_widget_set_background(root, (fdk_color){0.06f, 0.07f, 0.10f, 1});
    fdk_iconview_set_selection_mode(iv, FDK_LIST_SELECTION_MULTIPLE);
    /* The press point must be EMPTY grid space: 198 is past the
     * second column's 196 edge, inside the view's 200 width. */
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 198.0f, 10.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down) == true);
    (void)fdk_widget_tree_handle_event(root, &move);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 420, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    /* The inter-column GAP strip (x 97..99) carries no cell ink:
     * live it shows the band's tint over the window background;
     * released it is exactly the bare background again. The
     * reference pixel comes from outside the view (x=205), where
     * only the root's themed fill reaches. */
    unsigned long bg = px_at(s, 205, 40);
    int band_gap_ink = 0;
    for (int y = 24; y < 84; y += 2) {
        for (int x = 97; x < 100; x++) {
            if (px_at(s, x, y) != bg) {
                band_gap_ink++;
            }
        }
    }
    assert(band_gap_ink > 20); /* the live tint fills the gap     */
    (void)fdk_widget_tree_handle_event(root, &up);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int after_gap_ink = 0;
    for (int y = 24; y < 84; y += 2) {
        for (int x = 97; x < 100; x++) {
            if (px_at(s, x, y) != bg) {
                after_gap_ink++;
            }
        }
    }
    assert(after_gap_ink == 0); /* the band rect left with the
                                   gesture — the selection stays  */
    fdk_surface_destroy(s);

    fdk_widget_destroy(root);
    printf("[ok] iconview rubber band: live 2-D sweep selects the "
           "intersecting cells, press/release callback cadence, "
           "ctrl-union vs plain-clear, SINGLE refuses, band pixels "
           "present live and gone after release\n");
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
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
        printf("[skip] no system TrueType font found — the iconview "
               "label geometry needs real glyphs; see "
               "docs/testing.md\n");
        return 0;
    }

    test_basics_and_grid();
    test_selection();
    test_keyboard();
    test_mutation();
    test_paint_and_a11y();
    test_rubber_band();

    fdk_font_destroy(g_font);
    printf("all iconview tests passed\n");
    return 0;
}
