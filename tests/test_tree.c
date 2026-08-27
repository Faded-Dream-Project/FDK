/*
 * test_tree.c — headless tests for the Phase 9 Tree widget
 *
 * Model used everywhere:
 *   root-a (parent)
 *     a-1 (leaf)
 *     a-2 (parent)
 *       a-2-x (leaf, deep)
 *   root-b (leaf)
 *
 *   - model: add/get text, child counts, root-level count
 *   - visibility: collapsed parents hide subtrees; expand shows
 *     them in pre-order; visible_count tracks exactly
 *   - selection: row click selects the node; callback fires
 *   - expander: clicking the triangle zone toggles without
 *     selecting; clicking the text selects without toggling
 *   - keyboard: Up/Down walk visible rows across collapse
 *     boundaries; Left collapses a parent then jumps to its parent;
 *     Right expands then enters; Home/End
 *   - scrolling: deep expansion + wheel reaches deep nodes
 *   - argument safety
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static fdk_font *g_font = NULL;

static fdk_event_data ev_button(fdk_event_type t, float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    return e;
}

static void click(fdk_widget *root, float x, float y) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

static fdk_event_data ev_key(fdk_scancode sc) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    return e;
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 400},
                                    &root)));
    return root;
}

/* Builds the standard model; writes the four handles out. */
static fdk_widget *build_tree(fdk_widget *parent_root,
                              fdk_tree_node *a, fdk_tree_node *a1,
                              fdk_tree_node *a2, fdk_tree_node *a2x,
                              fdk_tree_node *b) {
    fdk_widget *tree = NULL;
    assert(fdk_ok(fdk_tree_create(parent_root, g_font, &tree)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE, "root-a",
                                    a)));
    assert(fdk_ok(fdk_tree_node_add(tree, *a, "a-1", a1)));
    assert(fdk_ok(fdk_tree_node_add(tree, *a, "a-2", a2)));
    assert(fdk_ok(fdk_tree_node_add(tree, *a2, "a-2-x", a2x)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE, "root-b",
                                    b)));
    return tree;
}

static int g_changes = 0;
static void on_changed(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_changes++;
}

/* ---- model ---- */

static void test_model(void) {
    fdk_widget *root = fresh_root();
    fdk_tree_node a, a1, a2, a2x, b;
    fdk_widget *tree = build_tree(root, &a, &a1, &a2, &a2x, &b);

    assert(strcmp(fdk_tree_node_text(tree, a), "root-a") == 0);
    assert(strcmp(fdk_tree_node_text(tree, a2x), "a-2-x") == 0);
    assert(fdk_tree_node_child_count(tree, a) == 2);
    assert(fdk_tree_node_child_count(tree, a2) == 1);
    assert(fdk_tree_node_child_count(tree, a1) == 0);
    assert(fdk_tree_node_child_count(tree, FDK_TREE_NODE_NONE) == 2);
    assert(fdk_tree_node_text(tree, 999) == NULL);
    assert(fdk_tree_node_text(NULL, 0) == NULL);
    assert(fdk_tree_node_child_count(NULL, 0) == 0);

    /* set_text. */
    assert(fdk_ok(fdk_tree_node_set_text(tree, b, "root-bee")));
    assert(strcmp(fdk_tree_node_text(tree, b), "root-bee") == 0);

    fdk_widget_destroy(root);
    printf("[ok] tree: model (add/text/child counts incl. roots)\n");
}

/* ---- visibility ---- */

static void test_visibility(void) {
    fdk_widget *root = fresh_root();
    fdk_tree_node a, a1, a2, a2x, b;
    fdk_widget *tree = build_tree(root, &a, &a1, &a2, &a2x, &b);
    fdk_widget_set_bounds(tree, (fdk_rect){0, 0, 200, 300});

    /* Collapsed: root-a, root-b visible. */
    assert(fdk_tree_visible_count(tree) == 2);

    /* Expand root-a: a, a-1, a-2, root-b (a-2 still collapsed). */
    assert(fdk_ok(fdk_tree_node_expand(tree, a, true)));
    assert(fdk_tree_visible_count(tree) == 4);

    /* Expand a-2: deep child appears, pre-order. */
    assert(fdk_ok(fdk_tree_node_expand(tree, a2, true)));
    assert(fdk_tree_visible_count(tree) == 5);

    /* Collapse root-a again: back to 2. */
    assert(fdk_ok(fdk_tree_node_expand(tree, a, false)));
    assert(fdk_tree_visible_count(tree) == 2);

    /* Expanding a leaf is refused. */
    assert(fdk_tree_node_expand(tree, a1, true) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(!fdk_tree_node_is_expanded(tree, a1));

    fdk_widget_destroy(root);
    printf("[ok] tree: visibility follows expand/collapse exactly "
           "(2 -> 4 -> 5 -> 2); leaf expand refused\n");
}

/* ---- selection + expander clicks ---- */

static void test_clicks(void) {
    fdk_widget *root = fresh_root();
    fdk_tree_node a, a1, a2, a2x, b;
    fdk_widget *tree = build_tree(root, &a, &a1, &a2, &a2x, &b);
    fdk_widget_set_bounds(tree, (fdk_rect){0, 0, 220, 300});
    assert(fdk_ok(fdk_tree_node_expand(tree, a, true)));
    assert(fdk_ok(fdk_tree_node_expand(tree, a2, true)));
    g_changes = 0;
    fdk_tree_set_on_selection_changed(tree, on_changed, NULL);

    /* Rows (26px each): 0 root-a, 1 a-1, 2 a-2, 3 a-2-x, 4 root-b.
     * Click row 2's TEXT (x=60, past the depth-1 expander zone). */
    click(root, 60, 2 * 26 + 13);
    assert(fdk_tree_get_selected(tree) == a2);
    assert(g_changes == 1);

    /* Click row 0's EXPANDER zone (depth 0: zone x in [2, 20), the
     * glyph centered at x=7): the node collapses but the SELECTION
     * does not move. */
    click(root, 7, 13);
    assert(!fdk_tree_node_is_expanded(tree, a));
    assert(fdk_tree_get_selected(tree) == a2);
    assert(g_changes == 1); /* no selection change */
    assert(fdk_tree_visible_count(tree) == 2);

    /* Click row 0's text (x=60): selects root-a, expands nothing. */
    click(root, 60, 13);
    assert(fdk_tree_get_selected(tree) == a);
    assert(g_changes == 2);

    /* Programmatic select. */
    assert(fdk_ok(fdk_tree_select(tree, a2x)));
    assert(fdk_tree_get_selected(tree) == a2x);
    assert(g_changes == 3);
    assert(fdk_tree_select(tree, 999) == FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    printf("[ok] tree: text clicks select, expander clicks toggle "
           "without selecting, callback counts exactly\n");
}

/* ---- keyboard ---- */

static void test_keyboard(void) {
    fdk_widget *root = fresh_root();
    fdk_tree_node a, a1, a2, a2x, b;
    fdk_widget *tree = build_tree(root, &a, &a1, &a2, &a2x, &b);
    fdk_widget_set_bounds(tree, (fdk_rect){0, 0, 220, 300});
    assert(fdk_ok(fdk_tree_node_expand(tree, a, true)));
    assert(fdk_ok(fdk_tree_node_expand(tree, a2, true)));
    assert(fdk_widget_focus(tree));

    /* Cold Down -> row 0 (root-a). */
    assert(fdk_widget_tree_handle_event(root, &(fdk_event_data){
        .type = FDK_EVENT_KEY_DOWN, .key.scancode = FDK_KEY_DOWN }));
    assert(fdk_tree_get_selected(tree) == a);

    /* Down x3 -> a-2-x. */
    fdk_event_data down = ev_key(FDK_KEY_DOWN);
    for (int i = 0; i < 3; i++) {
        assert(fdk_widget_tree_handle_event(root, &down));
    }
    assert(fdk_tree_get_selected(tree) == a2x);

    /* Left on a deep LEAF jumps to its parent (a-2). */
    fdk_event_data left = ev_key(FDK_KEY_LEFT);
    assert(fdk_widget_tree_handle_event(root, &left));
    assert(fdk_tree_get_selected(tree) == a2);

    /* Left on an EXPANDED parent collapses it (selection stays). */
    assert(fdk_widget_tree_handle_event(root, &left));
    assert(!fdk_tree_node_is_expanded(tree, a2));
    assert(fdk_tree_get_selected(tree) == a2);
    assert(fdk_tree_visible_count(tree) == 4);

    /* Right re-expands. */
    fdk_event_data right = ev_key(FDK_KEY_RIGHT);
    assert(fdk_widget_tree_handle_event(root, &right));
    assert(fdk_tree_node_is_expanded(tree, a2));
    assert(fdk_tree_visible_count(tree) == 5);

    /* Right again enters the first child. */
    assert(fdk_widget_tree_handle_event(root, &right));
    assert(fdk_tree_get_selected(tree) == a2x);

    /* End -> last visible (root-b), Home -> first (root-a). */
    fdk_event_data end = ev_key(FDK_KEY_END);
    assert(fdk_widget_tree_handle_event(root, &end));
    assert(fdk_tree_get_selected(tree) == b);
    fdk_event_data home = ev_key(FDK_KEY_HOME);
    assert(fdk_widget_tree_handle_event(root, &home));
    assert(fdk_tree_get_selected(tree) == a);

    /* Left on a collapsed ROOT is a no-op (selection stays). */
    assert(fdk_widget_tree_handle_event(root, &left));
    assert(fdk_tree_get_selected(tree) == a);

    fdk_widget_destroy(root);
    printf("[ok] tree: keyboard (arrows across collapse boundaries, "
           "left collapse/parent jump, right expand/enter, home/end)\n");
}

/* ---- deep scrolling ---- */

static void test_scrolling(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tree = NULL;
    assert(fdk_ok(fdk_tree_create(root, g_font, &tree)));
    fdk_widget_set_bounds(tree, (fdk_rect){0, 0, 200, 150});

    /* A chain of 40 nested nodes; expansion happens in a SECOND
     * pass (a node is a leaf until its child exists). */
    fdk_tree_node handles[40];
    fdk_tree_node parent = FDK_TREE_NODE_NONE;
    for (int i = 0; i < 40; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "n%02d", i);
        assert(fdk_ok(fdk_tree_node_add(tree, parent, buf, &handles[i])));
        parent = handles[i];
    }
    for (int i = 0; i < 39; i++) { /* the last stays a leaf */
        assert(fdk_ok(fdk_tree_node_expand(tree, handles[i], true)));
    }
    assert(fdk_tree_visible_count(tree) == 40);

    fdk_event_data wheel = {
        .type = FDK_EVENT_POINTER_SCROLL,
    };
    wheel.scroll.position.x = 50;
    wheel.scroll.position.y = 80;
    wheel.scroll.delta_y = -1;
    for (int i = 0; i < 40; i++) {
        (void)fdk_widget_tree_handle_event(root, &wheel);
    }
    /* Click near the bottom of the viewport: with scroll clamped
     * near the end (content 40x26=1040, viewport ~138), viewport
     * y=120 maps to content y~1022 — inside the LAST row (the
     * deepest node). */
    click(root, 60, 120);
    assert(fdk_tree_get_selected(tree) != FDK_TREE_NODE_NONE);
    /* The deepest node (the last added). */
    assert(fdk_tree_get_selected(tree) == handles[39]);

    fdk_widget_destroy(root);
    printf("[ok] tree: deep chains scroll; late nodes selectable "
           "after wheeling\n");
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
        printf("[skip] no system TrueType font found — tree row "
               "geometry needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_model();
    test_visibility();
    test_clicks();
    test_keyboard();
    test_scrolling();

    fdk_font_destroy(g_font);
    printf("all tree tests passed\n");
    return 0;
}
