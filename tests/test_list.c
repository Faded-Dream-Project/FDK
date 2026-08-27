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

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

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
    fdk_rect r = { 0, 0, 200, 160 };
    fdk_widget_set_bounds(list, r);
    for (int i = 0; i < 6; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "row %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    g_changes = 0;
    fdk_list_set_on_selection_changed(list, on_changed, NULL);

    /* Click row 2 (rows are ~26px tall; click y=2.5*26=65). */
    click(root, 50, 65, 0);
    assert(fdk_list_get_selected(list) == 2);
    assert(fdk_list_is_selected(list, 2));
    assert(!fdk_list_is_selected(list, 1));
    assert(g_changes == 1);

    /* Click row 4: exactly one selection moves. */
    click(root, 50, 4 * 26 + 13, 0);
    assert(fdk_list_get_selected(list) == 4);
    assert(!fdk_list_is_selected(list, 2));
    assert(g_changes == 2);

    /* Shift-click in SINGLE mode: still a single selection. */
    click(root, 50, 13, FDK_MOD_SHIFT);
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
    fdk_rect r = { 0, 0, 200, 160 };
    fdk_widget_set_bounds(list, r);
    for (int i = 0; i < 8; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item %d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    g_changes = 0;
    fdk_list_set_on_selection_changed(list, on_changed, NULL);

    /* Plain click row 1 (anchor). */
    click(root, 50, 1 * 26 + 13, 0);
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_is_selected(list, 1));

    /* Shift-click row 4: range 1..4. */
    click(root, 50, 4 * 26 + 13, FDK_MOD_SHIFT);
    assert(fdk_list_selected_count(list) == 4);
    for (size_t i = 1; i <= 4; i++) {
        assert(fdk_list_is_selected(list, i));
    }

    /* Ctrl+click row 5 (the last visible one; viewport is 148px):
     * adds without clearing. */
    click(root, 50, 5 * 26 + 13, FDK_MOD_CTRL);
    assert(fdk_list_selected_count(list) == 5);
    assert(fdk_list_is_selected(list, 5));

    /* Ctrl+click row 2: TOGGLES OFF (it was in the range). */
    click(root, 50, 2 * 26 + 13, FDK_MOD_CTRL);
    assert(!fdk_list_is_selected(list, 2));
    assert(fdk_list_selected_count(list) == 4);

    /* Plain click row 5 (anchor 5, selection resets to {5}), then
     * ctrl+shift+click row 4: ADDITIVE range 4..5 — ctrl+shift never
     * clears, unlike plain shift. */
    click(root, 50, 5 * 26 + 13, 0);
    assert(fdk_list_selected_count(list) == 1);
    click(root, 50, 4 * 26 + 13, FDK_MOD_CTRL | FDK_MOD_SHIFT);
    assert(fdk_list_is_selected(list, 4));
    assert(fdk_list_is_selected(list, 5));
    assert(fdk_list_selected_count(list) == 2);
    assert(!fdk_list_is_selected(list, 3));
    assert(!fdk_list_is_selected(list, 1));

    /* Plain click: collapse to one. */
    click(root, 50, 3 * 26 + 13, 0);
    assert(fdk_list_selected_count(list) == 1);
    assert(fdk_list_get_selected(list) == 3);

    /* Enumerate in order. */
    click(root, 50, 5 * 26 + 13, FDK_MOD_CTRL);
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
     * the vbar steals from WIDTH, not height), row_h 26 -> 6 rows. */
    fdk_event_data pgup = ev_key(FDK_KEY_PAGE_UP, 0);
    assert(fdk_widget_tree_handle_event(root, &pgup));
    assert(fdk_list_get_selected(list) == 3);

    /* MULTIPLE + shift+arrows extend from the anchor. */
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    click(root, 50, 2 * 26 + 13, 0); /* anchor row 2 */
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
    /* 60 rows x 26px = 1560px of content in a 148px viewport. */
    fdk_event_data wheel = {
        .type = FDK_EVENT_POINTER_SCROLL,
    };
    wheel.scroll.position.x = 50;
    wheel.scroll.position.y = 80;
    wheel.scroll.delta_y = -1;
    (void)fdk_widget_tree_handle_event(root, &wheel);
    /* The internal scrollview did the work; observable: clicking a
     * row that is only reachable SCROLLED hits that row. */
    /* Scroll to the bottom via End-ish: many wheel notches. */
    for (int i = 0; i < 40; i++) {
        (void)fdk_widget_tree_handle_event(root, &wheel);
    }
    /* Row 59 is now at the bottom; its content y =
     * 59*26 + 13 - scroll. Click near the bottom of the viewport. */
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
    test_none();
    test_keyboard();
    test_scrolling();
    test_paint();

    fdk_font_destroy(g_font);
    printf("all list tests passed\n");
    return 0;
}
