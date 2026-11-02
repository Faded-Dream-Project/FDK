/*
 * test_list.c — headless tests for the Phase 9 List widget
 *
 * Synthetic events through the tree (clicks now carry the Phase 9
 * modifier state, so ctrl/shift multi-select is testable headless):
 *   - CRUD: append/insert/remove/clear/row_text/set_row_text
 *   - SINGLE mode: click selects exactly one, callback fires once
 *   - MULTIPLE: ctrl toggles, shift ranges, ctrl+shift additive,
 *     plain click resets
 *   - NONE: clicks do nothing, select() refused
 *   - keyboard: up/down/home/end/page, shift+arrows extend
 *   - scrolling: many rows + wheel through the internal scrollview
 *   - paint: selected row shows the accent band (pixel-diff)
 *   - argument safety
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include "../src/widget/widgets_internal.h" /* the test seams */

#include "widget/widget_internal.h" /* animation test seam (1.3.8) */

#include <assert.h>
#include <stdio.h>
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

static void click(fdk_widget *root, float x, float y, fdk_u32 mods) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y,
                                    mods);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y,
                                  mods);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

/* Pointer MOTION (the widget-level synthesis path uses the plain
 * pointer member; the union overlays the button member's position,
 * but writing it directly is the honest spelling). */
static fdk_event_data ev_motion_evt(float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_MOTION;
    e.pointer.position.x = x;
    e.pointer.position.y = y;
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

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 400},
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
static fdk_list_selection_bounds g_last_bounds;
static void on_changed_bounds(fdk_widget *w,
                              const fdk_list_selection_bounds *bounds,
                              void *user) {
    (void)w;
    (void)user;
    if (bounds != NULL) {
        g_last_bounds = *bounds;
    }
}

static void on_changed(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_changes++;
}

/* ---- CRUD ---- */

static void test_crud(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));

    size_t i0 = 999, i1 = 999;
    assert(fdk_ok(fdk_list_append(list, "alpha", &i0)));
    assert(fdk_ok(fdk_list_append(list, "gamma", &i1)));
    assert(fdk_ok(fdk_list_insert(list, 1, "beta")));
    assert(fdk_list_row_count(list) == 3);
    assert(i0 == 0 && i1 == 1); /* captured at append time, before
                                 * the insert shifts gamma to 2 */
    assert(strcmp(fdk_list_row_text(list, 1), "beta") == 0);

    /* set_row_text + measure follows width. */
    assert(fdk_ok(fdk_list_set_row_text(list, 2, "gamma-long-row")));
    fdk_size nat = { 0, 0 };
    fdk_widget_measure(list, &nat);
    assert(nat.width > 80);

    /* Out-of-range reads. */
    assert(fdk_list_row_text(list, 3) == NULL);
    assert(fdk_list_remove(list, 3) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_list_row_text(NULL, 0) == NULL);
    assert(fdk_list_row_count(NULL) == 0);

    /* Remove the middle. */
    assert(fdk_ok(fdk_list_remove(list, 1)));
    assert(fdk_list_row_count(list) == 2);
    assert(strcmp(fdk_list_row_text(list, 1), "gamma-long-row") == 0);

    fdk_list_clear(list);
    assert(fdk_list_row_count(list) == 0);

    fdk_widget_destroy(root);
    printf("[ok] list: CRUD (append/insert/remove/clear/text)\n");
}

/* ---- SINGLE selection ---- */

static void test_single(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_rect r = { 0, 0, 200, 210 }; /* 6 rows x 30px + headroom */
    fdk_widget_set_bounds(list, r);
    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "row %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    g_changes = 0;
    fdk_list_set_on_selection_changed(list, on_changed, NULL);

    /* 1.4.0: rows lay out at the theme's LIST_ROW_HEIGHT floor (30
     * with the default Modern theme; the font's own metrics when
     * taller) — every click y below is computed from the PUBLIC
     * row-height getter, not a hardcoded constant. */
    const fdk_i32 rh = fdk_list_get_row_height(list);
    assert(rh >= 16);

    /* Click row 2 (center of the row band). */
    click(root, 50, (float)(2 * rh + rh / 2), 0);
    assert(fdk_list_get_selected(list) == 2);
    assert(fdk_list_is_selected(list, 2));
    assert(!fdk_list_is_selected(list, 1));
    assert(g_changes == 1);

    /* Click row 4: exactly one selection moves. */
    click(root, 50, (float)(4 * rh + rh / 2), 0);
    assert(fdk_list_get_selected(list) == 4);
    assert(!fdk_list_is_selected(list, 2));
    assert(g_changes == 2);

    /* Shift-click in SINGLE mode: still a single selection. */
    click(root, 50, (float)(rh / 2), FDK_MOD_SHIFT);
    assert(fdk_list_get_selected(list) == 0);
    assert(fdk_list_selected_count(list) == 1);

    /* Programmatic select. */
    assert(fdk_ok(fdk_list_select(list, 5)));
    assert(fdk_list_get_selected(list) == 5);

    fdk_widget_destroy(root);
    printf("[ok] list: SINGLE selection (clicks move it, callback "
           "fires once per change, shift collapses)\n");
}

/* ---- MULTIPLE selection ---- */

static void test_multiple(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_rect r = { 0, 0, 200, 260 }; /* 8 rows x 30px + headroom */
    fdk_widget_set_bounds(list, r);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    g_changes = 0;
    fdk_list_set_on_selection_changed(list, on_changed, NULL);
    const fdk_i32 rh = fdk_list_get_row_height(list);

    /* Plain click row 1 (anchor). */
    click(root, 50, (float)(1 * rh + rh / 2), 0);
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_is_selected(list, 1));

    /* Shift-click row 4: range 1..4. */
    click(root, 50, (float)(4 * rh + rh / 2), FDK_MOD_SHIFT);
    assert(fdk_list_selected_count(list) == 4);
    for (size_t i = 1; i <= 4; i++) {
        assert(fdk_list_is_selected(list, i));
    }

    /* Ctrl+click row 5: adds without clearing. */
    click(root, 50, (float)(5 * rh + rh / 2), FDK_MOD_CTRL);
    assert(fdk_list_selected_count(list) == 5);
    assert(fdk_list_is_selected(list, 5));

    /* Ctrl+click row 2: TOGGLES OFF (it was in the range). */
    click(root, 50, (float)(2 * rh + rh / 2), FDK_MOD_CTRL);
    assert(!fdk_list_is_selected(list, 2));
    assert(fdk_list_selected_count(list) == 4);

    /* Plain click row 5 (anchor 5, selection resets to {5}), then
     * ctrl+shift+click row 4: ADDITIVE range 4..5 — ctrl+shift never
     * clears, unlike plain shift. */
    click(root, 50, (float)(5 * rh + rh / 2), 0);
    assert(fdk_list_selected_count(list) == 1);
    click(root, 50, (float)(4 * rh + rh / 2), FDK_MOD_CTRL | FDK_MOD_SHIFT);
    assert(fdk_list_is_selected(list, 4));
    assert(fdk_list_is_selected(list, 5));
    assert(fdk_list_selected_count(list) == 2);
    assert(!fdk_list_is_selected(list, 3));
    assert(!fdk_list_is_selected(list, 1));

    /* Plain click: collapse to one. */
    click(root, 50, (float)(3 * rh + rh / 2), 0);
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_get_selected(list) == 3);

    /* Enumerate in order. */
    click(root, 50, (float)(5 * rh + rh / 2), FDK_MOD_CTRL);
    size_t row = 999;
    assert(fdk_ok(fdk_list_selected_at(list, 0, &row)));
    assert(row == 3);
    assert(fdk_ok(fdk_list_selected_at(list, 1, &row)));
    assert(row == 5);
    assert(fdk_list_selected_at(list, 2, &row) == FDK_ERR_NOT_FOUND);

    fdk_widget_destroy(root);
    printf("[ok] list: MULTIPLE selection (ctrl toggle, shift range, "
           "ctrl+shift additive, plain collapse, ordered enum)\n");
}

/* ---- NONE mode ---- */

/* §25/G-3: selection API completeness. A file manager's
 * Edit menu needs: deselect ONE row (invert-selection without the
 * rebuild-everything dance), select a RANGE programmatically (the
 * shift-click equivalent), and the changed signal must say WHICH
 * rows moved so a status bar can update without rescanning. The
 * §24 app worked around the absence by clearing + re-selecting
 * (N fires, and no range info at all).
 *
 * Written RED first: fdk_list_deselect / fdk_list_select_range /
 * fdk_list_selection_changed_bounds did not exist.
 */
static void test_selection_completeness(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_rect r = { 0, 0, 200, 260 };
    fdk_widget_set_bounds(list, r);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "item %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    g_changes = 0;
    fdk_list_set_on_selection_changed(list, on_changed, NULL);

    /* --- deselect one row (two rows selected first: additive
     * range select, then deselect one of them) --- */
    assert(fdk_ok(fdk_list_select_range(list, 0, 3, false)));
    assert(fdk_list_selected_count(list) == 4);
    assert(fdk_ok(fdk_list_deselect(list, 1)));
    assert(fdk_ok(fdk_list_deselect(list, 2)));
    assert(fdk_list_selected_count(list) == 2);
    assert(fdk_ok(fdk_list_deselect(list, 0)));
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_is_selected(list, 3));
    assert(!fdk_list_is_selected(list, 0));
    /* deselecting an unselected row: OK, no signal */
    int before = g_changes;
    assert(fdk_ok(fdk_list_deselect(list, 1)));
    assert(g_changes == before);
    /* bad index refused */
    assert(fdk_list_deselect(list, 99) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_list_deselect(NULL, 0) == FDK_ERR_INVALID_ARGUMENT);

    /* --- select_range: the programmatic shift-click --- */
    assert(fdk_ok(fdk_list_select_range(list, 2, 5, false)));
    assert(fdk_list_selected_count(list) == 4);
    assert(fdk_list_is_selected(list, 2));
    assert(fdk_list_is_selected(list, 5));
    assert(!fdk_list_is_selected(list, 1));
    /* additive keeps the old selection */
    assert(fdk_ok(fdk_list_select_range(list, 6, 7, true)));
    assert(fdk_list_selected_count(list) == 6);
    /* inverted span works (5,2) */
    assert(fdk_ok(fdk_list_select_range(list, 5, 2, false)));
    assert(fdk_list_selected_count(list) == 4);
    assert(fdk_list_is_selected(list, 3));
    /* out-of-range ends are clamped, not fatal (6..7 after the
     * clamp to the 8-row count) */
    assert(fdk_ok(fdk_list_select_range(list, 6, 60, false)));
    assert(fdk_list_is_selected(list, 6));
    assert(fdk_list_is_selected(list, 7));
    assert(fdk_list_selected_count(list) == 2);

    /* --- the changed signal carries the affected range --- */
    fdk_list_set_on_selection_changed_bounds(list,
                                         on_changed_bounds, NULL);
    /* replacing {6,7} with 0..3 touches rows 0..7 (the clear of 6,7
     * is part of the same settled change) */
    g_last_bounds.valid = false;
    assert(fdk_ok(fdk_list_select_range(list, 0, 3, false)));
    assert(g_last_bounds.valid);
    assert(g_last_bounds.first == 0 && g_last_bounds.last == 7);
    /* a deselect reports exactly its own row */
    g_last_bounds.valid = false;
    assert(fdk_ok(fdk_list_deselect(list, 2)));
    assert(g_last_bounds.valid);
    assert(g_last_bounds.first == 2 && g_last_bounds.last == 2);

    fdk_widget_destroy(root);
    printf("[ok] list: selection completeness (deselect one, "
           "select_range additive/clamped/inverted, changed signal "
           "carries the affected range)\n");
}

static void test_none(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 200, 160});
    assert(fdk_ok(fdk_list_append(list, "a", NULL)));
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_NONE);
    click(root, 50, 13, 0);
    assert(fdk_list_get_selected(list) == -1);
    assert(fdk_list_select(list, 0) == FDK_ERR_UNSUPPORTED);

    fdk_widget_destroy(root);
    printf("[ok] list: NONE mode refuses selection (click + API)\n");
}

/* ---- keyboard ---- */

static void test_keyboard(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 200, 160});
    for (int i = 0; i < 10; i++) {
        assert(fdk_ok(fdk_list_append(list, "k", NULL)));
    }
    assert(fdk_widget_focus(list));

    /* Cold start: Down selects row 0. */
    fdk_event_data down = ev_key(FDK_KEY_DOWN, 0);
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_list_get_selected(list) == 0);

    /* Down thrice -> 3. */
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_list_get_selected(list) == 3);

    /* Up -> 2. Home -> 0. End -> 9 (clamped). */
    fdk_event_data up = ev_key(FDK_KEY_UP, 0);
    assert(fdk_widget_tree_handle_event(root, &up));
    assert(fdk_list_get_selected(list) == 2);
    fdk_event_data home = ev_key(FDK_KEY_HOME, 0);
    assert(fdk_widget_tree_handle_event(root, &home));
    assert(fdk_list_get_selected(list) == 0);
    fdk_event_data end = ev_key(FDK_KEY_END, 0);
    assert(fdk_widget_tree_handle_event(root, &end));
    assert(fdk_list_get_selected(list) == 9);

    /* PageUp from 9: viewport = 160 (rows are narrow -> no hbar;
     * the vbar steals from WIDTH, not height); page = vh/rh rows
     * (30px rows -> 5). The getter keeps this honest across theme
     * metric changes. */
    fdk_event_data pgup = ev_key(FDK_KEY_PAGE_UP, 0);
    assert(fdk_widget_tree_handle_event(root, &pgup));
    {
        fdk_i32 krh = fdk_list_get_row_height(list);
        fdk_i64 page = (krh > 0) ? 160 / krh : 10;
        if (page < 1) {
            page = 1;
        }
        assert(fdk_list_get_selected(list) == 9 - page);
    }

    /* MULTIPLE + shift+arrows extend from the anchor. */
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    {
        fdk_i32 krh = fdk_list_get_row_height(list);
        click(root, 50, (float)(2 * krh + krh / 2), 0); /* anchor row 2 */
    }
    fdk_event_data sdown = ev_key(FDK_KEY_DOWN, FDK_MOD_SHIFT);
    assert(fdk_widget_tree_handle_event(root, &sdown));
    assert(fdk_widget_tree_handle_event(root, &sdown));
    assert(fdk_list_selected_count(list) == 3); /* rows 2,3,4 */
    assert(fdk_list_is_selected(list, 4));

    fdk_widget_destroy(root);
    printf("[ok] list: keyboard nav (cold start, arrows, home/end, "
           "page, shift+arrow extension)\n");
}

/* ---- scrolling integration ---- */

static void test_scrolling(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 200, 160});
    for (int i = 0; i < 60; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "row %02d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    /* 60 rows x 30px = 1800px of content in a ~148px viewport. */
    fdk_event_data wheel = {
        .type = FDK_EVENT_POINTER_SCROLL,
    };
    wheel.scroll.position.x = 50;
    wheel.scroll.position.y = 80;
    wheel.scroll.delta_y = -1;
    (void)fdk_widget_tree_handle_event(root, &wheel);
    /* The internal scrollview did the work; observable: clicking a
     * row that is only reachable SCROLLED hits that row. */
    /* Scroll to the bottom via End-ish: many wheel notches. The
     * internal scrollview EASES to the accumulated target (1.3.8),
     * so the burst settles before the click: pin the engine's
     * synthetic clock, pump far past any flight. */
    fdk__animation_set_test_clock(0);
    for (int i = 0; i < 40; i++) {
        (void)fdk_widget_tree_handle_event(root, &wheel);
    }
    fdk__animation_pump(100000);
    fdk__animation_set_test_clock(-1);
    /* Row 59 is now at the bottom; its content y =
     * 59*rh + rh/2 - scroll. Click near the bottom of the viewport. */
    click(root, 50, 148, 0);
    assert(fdk_list_get_selected(list) >= 55);

    fdk_widget_destroy(root);
    printf("[ok] list: wheel scrolling through the internal "
           "scrollview reaches late rows\n");
}

/* ---- paint ---- */

static void test_paint(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 200, 160});
    assert(fdk_ok(fdk_list_append(list, "visible row", NULL)));

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 200, &s)));

    assert(fdk_ok(fdk_list_select(list, 0)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 selected_row[64];
    for (int i = 0; i < 64; i++) {
        selected_row[i] = px_at(s, 30 + i, 10 + 13);
    }

    /* Deselect: the band disappears. */
    click(root, 30, 10 + 13, 0);
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_NONE);
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_SINGLE);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int band = 0;
    for (int i = 0; i < 64; i++) {
        if (px_at(s, 30 + i, 10 + 13) != selected_row[i]) {
            band = 1;
            break;
        }
    }
    assert(band);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] list: selected row paints an accent band "
           "(pixel-diff verified)\n");
}

/* ---- columns (1.5.0) ---- */

static void test_columns(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    for (int i = 0; i < 4; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "file %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 400, 200});
    /* layout + the no-columns baseline */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(420, 240, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    assert(fdk_list_get_column_count(list) == 0);

    /* Declare: Name (primary, min 120) | Modified (110) | Size (80). */
    fdk_list_column cols[3] = {
        {"Name", 120, true},
        {"Modified", 110, false},
        {"Size", 80, false},
    };
    assert(fdk_ok(fdk_list_set_columns(list, cols, 3)));
    assert(fdk_list_get_column_count(list) == 3);
    /* bad args: NULL array with a count, cell on column 0, cell on
     * a column beyond the set, cell on an out-of-range row */
    assert(!fdk_ok(fdk_list_set_columns(list, NULL, 2)));
    assert(!fdk_ok(fdk_list_row_set_cell(list, 0, 0, "x")));
    assert(!fdk_ok(fdk_list_row_set_cell(list, 0, 3, "x")));
    assert(!fdk_ok(fdk_list_row_set_cell(list, 99, 1, "x")));

    /* cells land and a wide cell widens its column (never clips a
     * right-aligned tail) */
    assert(fdk_ok(fdk_list_row_set_cell(list, 0, 1,
                                       "Nov 12 2025, 18:20")));
    assert(fdk_ok(fdk_list_row_set_cell(list, 0, 2, "1.4 MB")));
    assert(fdk_ok(fdk_list_row_set_cell(list, 1, 2, "2 bytes")));

    /* geometry: the content width floors at the columns' sum; rows
     * keep full-width selection spans; the scrollview insets below
     * the header strip */
    fdk_widget_tree_paint(root, s);
    fdk_rect rb = fdk_widget_get_absolute_bounds(list);
    (void)rb;
    /* selection still works under columns (the row-press band from
     * round 6 must survive the new geometry): press row 0, sweep to
     * row 2, release */
    const fdk_i32 rh = fdk_list_get_row_height(list);
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    50.0f,
                                    (float)(10 + rh / 2), 0);
    fdk_event_data move = ev_motion_evt(
        50.0f, (float)(10 + rh * 2 + rh / 2));
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 50.0f,
                                  (float)(10 + rh * 2 + rh / 2), 0);
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_widget_tree_handle_event(root, &move));
    assert(fdk_widget_tree_handle_event(root, &up));
    assert(fdk_list_selected_count(list) == 3);

    /* ---- 1.5.1: the right-edge anchor (the owner's round-8
     * report: the columns clustered at the LEFT, must hug the far
     * end and follow resizes) ---- */
    assert(fdk_list_get_column_count(list) == 3);
    fdk_widget_tree_paint(root, s); /* the self-heal sync ran */
    fdk_i32 cxL = 0, cwL = 0, cwid = 0; /* the LAST column (idx 2) */
    fdk_i32 cx1 = 0, cw1 = 0;
    fdk__list_test_column_x(list, 2, &cxL, &cwL, &cwid);
    /* anchored: the LAST column's right edge == the viewport (the
     * list is 400 wide at x=10; 4 rows of "file N" fit far inside,
     * so this is the anchored mode) */
    assert(cxL + cwL == 400);
    /* the name column stretched across the slack: its width is the
     * first fixed column's x minus the separator */
    fdk__list_test_column_x(list, 1, &cx1, &cw1, NULL);
    fdk_i32 cnx = 0, cnw = 0;
    fdk__list_test_column_x(list, 0, &cnx, &cnw, NULL);
    assert(cnx == 0);
    assert(cnw == cx1 - 24); /* LIST_COL_SEP — the name column
                              * fills everything left of the first
                              * fixed column (the EXPANSION, made
                              * concrete: 116 px for "file N" rows
                              * whose natural width is ~60) */
    assert(cnw >= 80);       /* at least the v1 minimum          */

    /* resize: WIDER window — the anchor follows */
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 600, 200});
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk__list_test_column_x(list, 2, &cxL, &cwL, &cwid);
    assert(cxL + cwL == 600);
    fdk_i32 cnw_wide = 0;
    fdk__list_test_column_x(list, 0, &cnx, &cnw_wide, NULL);
    assert(cnw_wide > cnw); /* grew with the window             */
    assert(cnw_wide >= cnw + 190); /* ~the full delta (600-400,
                                     * minus nothing: the fixed
                                     * spread did not change)   */

    /* resize: NARROW window — content mode: natural widths, the
     * spread floors content_width, the name column keeps its
     * natural width */
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 160, 200});
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk__list_test_column_x(list, 2, &cxL, &cwL, &cwid);
    assert(cxL + cwL <= cwid); /* inside the scrolled content */
    assert(cwid > 160);        /* the floor: the spread          */
    fdk__list_test_column_x(list, 0, &cnx, &cnw, NULL);
    assert(cnw < 160); /* natural, not the squeezed viewport */

    /* and back wide — re-anchors */
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 500, 200});
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk__list_test_column_x(list, 2, &cxL, &cwL, &cwid);
    assert(cxL + cwL == 500);

    /* clearing the column set returns the plain v1 list */
    assert(fdk_ok(fdk_list_set_columns(list, NULL, 0)));
    assert(fdk_list_get_column_count(list) == 0);
    fdk_surface_destroy(s);
    printf("[ok] list: columns (set/cells, right-edge anchor + "
           "resize adaptation + narrow fallback, selection under "
           "columns, clear-to-plain)\n");
}

/* ---- the row-press band start (1.5.0, the round-6 wall-to-wall case) ---- */

static void test_row_press_band(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    /* Nine 30-px rows = 270 px of content in a 280-px field: 10 px
     * of empty space ONLY — the wall-to-wall shape of a real
     * directory view. The press must start on a ROW. */
    for (int i = 0; i < 9; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "row %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 200, 280});
    /* LAY THE TREE OUT (a paint pass runs measure/arrange): the
     * press must HIT a row, and hit-testing needs real bounds. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 400, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    const fdk_i32 rh = fdk_list_get_row_height(list);
    assert(rh > 0);

    /* Press row 0 (unselected, plain): the ROW holds the press as a
     * potential band; the sweep crosses rows 0..4; release. The
     * rows sit at list-y rh*i; the list itself is at root (10,10). */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    50.0f,
                                    (float)(10 + rh / 2), 0);
    fdk_event_data move = ev_motion_evt(
        50.0f, (float)(10 + rh * 4 + rh / 2));
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 50.0f,
                                  (float)(10 + rh * 4 + rh / 2), 0);
    int hd = fdk_widget_tree_handle_event(root, &down);
    assert(hd); /* the row consumed it (pending band) */
    int hm = fdk_widget_tree_handle_event(root, &move);
    assert(hm); /* the slop converted it: the band owns the sweep */
    int hu = fdk_widget_tree_handle_event(root, &up);
    assert(hu);
    fdk_surface_destroy(s); /* the layout probe surface */
    assert(fdk_list_selected_count(list) == 5);
    assert(fdk_list_is_selected(list, 0));
    assert(fdk_list_is_selected(list, 4));
    assert(!fdk_list_is_selected(list, 5));

    /* A drag-armed (selected) row keeps the classic press: the row
     * consumes it (no band, no re-selection churn). */
    (void)fdk_list_row_set_drag_armed(list, 2, true);
    fdk_event_data down2 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 85.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down2));
    /* still selected, still exactly the armed row's click semantics:
     * plain click on a selected+armed row re-selects just it */
    assert(fdk_list_is_selected(list, 2));

    /* GRAB_BROKEN mid-band: the auto-scroll timer and the banding
     * state must die with the gesture (the round-6 freeze). */
    fdk_event_data down3 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 45.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down3);
    fdk__widget_tree_grab_broken(root); /* the drag-begin path */
    /* the NEXT press is a fresh gesture (the frozen state is gone):
     * it starts its own band and lands where it lands — a stale
     * banding flag would have made this press a no-op (the round-6
     * freeze's exact symptom). */
    fdk_event_data down4 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 45.0f, 0);
    fdk_event_data up4 = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 100.0f,
                                   45.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down4);
    (void)fdk_widget_tree_handle_event(root, &up4);
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_is_selected(list, 1));
    printf("[ok] list: row-press band, armed rows, grab-broken\n");
}

/* ---- the rubber-band sweep (1.4.2) ---- */

static int g_band_changes;

static void on_band_changed(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_band_changes++;
}

static void test_rubber_band(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_selection_changed(list, on_band_changed, NULL);
    g_band_changes = 0;
    /* Six 30-px rows = 180 px of content in a 260-px field: 80 px of
     * empty space below the rows (the band's natural habitat). */
    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "row %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_widget_set_bounds(list, (fdk_rect){10, 10, 200, 260});

    /* A plain sweep from the empty space up over the rows: rows
     * 2..5 end up selected (the band covers local y [80, 200]). */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    100.0f, 210.0f, 0);
    fdk_event_data move = ev_motion_evt(150.0f, 90.0f);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 150.0f,
                                  90.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down)); /* the grab  */
    assert(fdk_list_selected_count(list) == 0);  /* zero-extent band */
    assert(g_band_changes == 1);                 /* the press fired */
    /* Live selection DURING the sweep (before the release). */
    (void)fdk_widget_tree_handle_event(root, &move);
    assert(fdk_list_selected_count(list) == 4);
    assert(fdk_list_is_selected(list, 2));
    assert(fdk_list_is_selected(list, 5));
    assert(!fdk_list_is_selected(list, 1));
    assert(!fdk_list_is_selected(list, 0));
    assert(g_band_changes == 1); /* motions never spam the callback */
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_band_changes == 2); /* the release is the gesture's end */
    assert(fdk_list_selected_count(list) == 4); /* selection persists */

    /* The band RECT paints during the gesture and is gone after:
     * sample inside the swept empty space (root y=205, x=135 — the
     * band's interior, clear of the bottom edge and of every row).
     * The root needs a background so the tint has something to be
     * repainted AWAY to (a standalone root paints nothing). */
    fdk_widget_set_background(root,
                              (fdk_color){0.1f, 0.1f, 0.1f, 1.0f});
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 300, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 after_release = px_at(s, 135, 205);
    fdk_event_data down2 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 210.0f, 0);
    fdk_event_data move2 = ev_motion_evt(150.0f, 90.0f);
    (void)fdk_widget_tree_handle_event(root, &down2);
    (void)fdk_widget_tree_handle_event(root, &move2);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 mid_gesture = px_at(s, 135, 205);
    assert(mid_gesture != after_release); /* the tint               */
    fdk_event_data up2 = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 150.0f,
                                   90.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &up2);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 135, 205) == after_release); /* gone            */

    /* A ctrl sweep UNIONS over the pre-band selection. */
    assert(fdk_ok(fdk_list_select(list, 0)));
    g_band_changes = 0;
    fdk_event_data down3 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 210.0f, FDK_MOD_CTRL);
    fdk_event_data move3 = ev_motion_evt(150.0f, 90.0f);
    fdk_event_data up3 = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 150.0f,
                                   90.0f, FDK_MOD_CTRL);
    (void)fdk_widget_tree_handle_event(root, &down3);
    (void)fdk_widget_tree_handle_event(root, &move3);
    (void)fdk_widget_tree_handle_event(root, &up3);
    assert(fdk_list_selected_count(list) == 5); /* row 0 + rows 2..5 */
    assert(fdk_list_is_selected(list, 0));

    /* An empty-space CLICK (no motion) clears a plain selection. */
    g_band_changes = 0;
    click(root, 100.0f, 230.0f, 0);
    assert(fdk_list_selected_count(list) == 0);
    assert(g_band_changes == 2); /* press + release, one gesture    */

    /* SINGLE mode refuses the band: the press is not consumed. */
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_SINGLE);
    fdk_event_data down4 = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                     100.0f, 210.0f, 0);
    assert(!fdk_widget_tree_handle_event(root, &down4));

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] list: rubber band — live sweep selection, plain "
           "replace vs ctrl union, band tint paints mid-gesture only, "
           "empty-space click clears, one callback pair per gesture, "
           "SINGLE refuses\n");
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
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
        printf("[skip] no system TrueType font found — list row "
               "geometry needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_crud();
    test_single();
    test_multiple();
    test_selection_completeness();
    test_none();
    test_keyboard();
    test_scrolling();
    test_paint();
    test_columns();
    test_row_press_band();
    test_rubber_band();

    fdk_font_destroy(g_font);
    printf("all list tests passed\n");
    return 0;
}
