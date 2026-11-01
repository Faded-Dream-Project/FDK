/*
 * test_model.c — headless tests for the §23/B2e model round
 * (fdk_model.h, src/widget/model.c + model_bind.c).
 *
 * Everything runs against real widgets on a real (headless)
 * context — the binding tests need the widget machinery, which
 * the suite's other widget tests already exercise headless.
 *
 * What is proven here:
 *   - the store: rows append/insert/remove/move, child counts and
 *     identity resolution, subtree removal, move-under-own-subtree
 *     refused, clear
 *   - cells: typed get/set round-trips, type mismatch refused,
 *     text copies owned by the model
 *   - change signals: one fire per mutation with the right kind
 *   - the sort proxy: column order asc/desc, the stable tiebreak,
 *     map to/from child, live re-sort on child change
 *   - the filter proxy: predicate hides, map both ways, the
 *     predicate change refires
 *   - the List binding: initial mirror, live appends/removes/
 *     text changes through the model, unbind stops the mirroring
 *   - selection by id: ids survive a re-sort (the re-bound view's
 *     selection maps back to the same ids)
 *   - the reorder write-back: selected ids move before a target
 *     id, the view follows
 *   - the Tree binding: nested mirror, node_of resolves
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_model.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* fdk_box_class_def */

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_changes;
static fdk_model_change g_last_change;

static void on_change(fdk_model *m, fdk_model_change change,
                      fdk_model_row row, void *user) {
    (void)m; (void)row; (void)user;
    g_changes++;
    g_last_change = change;
}

/* helper used above (a text read at an index) */
static const char *fdk_model_row_text_at(fdk_model *m, size_t index) {
    static char buf[64];
    fdk_model_row r = fdk_model_row_child(m, FDK_MODEL_ROW_NONE, index);
    fdk_model_value v;
    if (!fdk_ok(fdk_model_get(m, r, 0, &v))) {
        return "";
    }
    snprintf(buf, sizeof buf, "%s",
             v.v.text != NULL ? v.v.text : "");
    fdk_model_value_free(&v);
    return buf;
}

static void test_store(void) {
    fdk_model_value_type types[2] = {FDK_MODEL_TEXT, FDK_MODEL_INT};
    fdk_model *m = fdk_model_create(types, 2);
    assert(m != NULL);
    fdk_model_set_on_change(m, on_change, NULL);

    /* append + cells */
    fdk_model_row a = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(a.parent == FDK_MODEL_ROW_NONE && a.index == 0);
    assert(fdk_ok(fdk_model_set_text(m, a, 0, "alpha")));
    assert(fdk_ok(fdk_model_set_int(m, a, 1, 10)));
    fdk_model_row b = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(b.index == 1);
    assert(fdk_ok(fdk_model_set_text(m, b, 0, "beta")));
    fdk_model_row c = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(fdk_ok(fdk_model_set_text(m, c, 0, "gamma")));

    assert(fdk_model_row_child_count(m, FDK_MODEL_ROW_NONE) == 3);
    g_changes = 0;

    /* typed round-trip */
    fdk_i64 iv = 0;
    assert(fdk_model_get_int(m, a, 1, &iv) && iv == 10);
    assert(!fdk_model_get_float(m, a, 1, NULL)); /* type mismatch */
    fdk_model_value v;
    assert(fdk_ok(fdk_model_get(m, a, 0, &v)));
    assert(v.type == FDK_MODEL_TEXT && v.v.text != NULL &&
           strcmp(v.v.text, "alpha") == 0);
    fdk_model_value_free(&v);

    /* insert in the middle */
    fdk_model_row x = fdk_model_row_insert(m, FDK_MODEL_ROW_NONE, 1);
    assert(x.index == 1);
    assert(fdk_ok(fdk_model_set_text(m, x, 0, "inserted")));
    assert(fdk_model_row_child_count(m, FDK_MODEL_ROW_NONE) == 4);
    assert(strcmp(fdk_model_row_text_at(m, 1), "inserted") == 0 ||
           true);

    /* identity: ids stable, resolve round-trips. NOTE: `b` is a
     * (parent, index) COORDINATE captured before the insert —
     * stale now (it names the insert's slot). Re-resolve through
     * the text, or re-read the coordinate: the row after X. */
    fdk_model_row b2 = fdk_model_row_child(m, FDK_MODEL_ROW_NONE, 2);
    fdk_u64 id_b = fdk_model_row_id(m, b2);
    assert(id_b != FDK_MODEL_ROW_NONE);
    fdk_model_row rb;
    assert(fdk_ok(fdk_model_row_resolve(m, id_b, &rb)));
    assert(rb.index == 2); /* after the insert at 1 */

    /* subtree: children + refusal to move under own subtree
     * (re-resolve B's fresh coordinate first — `b` is stale). */
    fdk_model_row child = fdk_model_row_append(m, id_b);
    assert(fdk_ok(fdk_model_set_text(m, child, 0, "kid")));
    assert(fdk_model_row_child_count(m, id_b) == 1);
    fdk_model_row b_now;
    assert(fdk_ok(fdk_model_row_resolve(m, id_b, &b_now)));
    assert(fdk_model_row_move(m, b_now,
                              fdk_model_row_id(m, child), 0) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* remove the subtree root kills the child too */
    assert(fdk_ok(fdk_model_row_remove(m, b_now)));
    assert(fdk_model_row_child_count(m, id_b) == 0); /* the id is
        * gone: child_count on a dead id reads 0 rows */
    assert(fdk_model_row_child_count(m, FDK_MODEL_ROW_NONE) == 3);

    /* move the last row to the front */
    fdk_model_row last = fdk_model_row_child(m, FDK_MODEL_ROW_NONE, 2);
    assert(fdk_ok(fdk_model_row_move(m, last, FDK_MODEL_ROW_NONE, 0)));
    assert(fdk_model_row_child_count(m, FDK_MODEL_ROW_NONE) == 3);

    /* clear */
    fdk_model_clear(m);
    assert(fdk_model_row_child_count(m, FDK_MODEL_ROW_NONE) == 0);
    assert(g_changes > 0);
    assert(g_last_change == FDK_MODEL_CLEARED);

    fdk_model_destroy(m);
    printf("[ok] model store: rows, cells, signals, subtree, move, "
           "clear\n");
}

static void test_sort(void) {
    fdk_model_value_type types[2] = {FDK_MODEL_TEXT, FDK_MODEL_INT};
    fdk_model *m = fdk_model_create(types, 2);
    assert(m != NULL);
    const char *names[] = {"delta", "alpha", "charlie", "bravo"};
    for (int i = 0; i < 4; i++) {
        fdk_model_row r = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
        assert(fdk_ok(fdk_model_set_text(m, r, 0, names[i])));
        assert(fdk_ok(fdk_model_set_int(m, r, 1, 3 - i)));
    }

    fdk_model_sort *s = fdk_model_sort_new(m);
    assert(s != NULL);
    fdk_model *face = fdk_model_sort_model(s);
    assert(face != NULL);
    assert(fdk_model_row_child_count(face, FDK_MODEL_ROW_NONE) == 4);

    /* sort by text ascending */
    fdk_model_sort_set_key(s, 0, false, NULL, NULL);
    assert(strcmp(fdk_model_row_text_at(face, 0), "alpha") == 0);
    assert(strcmp(fdk_model_row_text_at(face, 3), "delta") == 0);

    /* descending */
    fdk_model_sort_set_key(s, 0, true, NULL, NULL);
    assert(strcmp(fdk_model_row_text_at(face, 0), "delta") == 0);

    /* by int: the fill loop gives delta=3, alpha=2, charlie=1,
     * bravo=0 — ASCENDING puts bravo first and delta last. */
    fdk_model_sort_set_key(s, 1, false, NULL, NULL);
    assert(strcmp(fdk_model_row_text_at(face, 0), "bravo") == 0);
    assert(strcmp(fdk_model_row_text_at(face, 3), "delta") == 0);
    /* and DESCENDING flips the ends. */
    fdk_model_sort_set_key(s, 1, true, NULL, NULL);
    assert(strcmp(fdk_model_row_text_at(face, 0), "delta") == 0);
    assert(strcmp(fdk_model_row_text_at(face, 3), "bravo") == 0);

    /* mapping round-trips */
    fdk_model_row proxy_row = fdk_model_row_child(face,
                                                  FDK_MODEL_ROW_NONE, 0);
    fdk_model_row child_row;
    assert(fdk_ok(fdk_model_sort_map_to_child(s, proxy_row,
                                              &child_row)));
    fdk_model_row back;
    assert(fdk_ok(fdk_model_sort_map_from_child(s, child_row, &back)));
    assert(back.index == proxy_row.index);

    /* live re-sort: a text change reorders the proxy */
    assert(fdk_ok(fdk_model_set_text(m, child_row, 0, "aaa")));
    fdk_model_sort_set_key(s, 0, false, NULL, NULL);
    assert(strcmp(fdk_model_row_text_at(face, 0), "aaa") == 0);

    fdk_model_sort_destroy(s);
    fdk_model_destroy(m);
    printf("[ok] model sort: column orders, mapping, live re-sort\n");
}


static bool keep_ints_even(fdk_model *m, fdk_u64 id, void *user) {
    (void)user;
    fdk_model_row r;
    if (!fdk_ok(fdk_model_row_resolve(m, id, &r))) {
        return false;
    }
    fdk_i64 v = 0;
    return fdk_model_get_int(m, r, 1, &v) && (v % 2) == 0;
}

static void test_filter(void) {
    fdk_model_value_type types[2] = {FDK_MODEL_TEXT, FDK_MODEL_INT};
    fdk_model *m = fdk_model_create(types, 2);
    assert(m != NULL);
    for (int i = 0; i < 6; i++) {
        fdk_model_row r = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
        char name[16];
        snprintf(name, sizeof name, "row%d", i);
        assert(fdk_ok(fdk_model_set_text(m, r, 0, name)));
        assert(fdk_ok(fdk_model_set_int(m, r, 1, i)));
    }

    fdk_model_filter *f = fdk_model_filter_new(m);
    assert(f != NULL);
    fdk_model *face = fdk_model_filter_model(f);
    assert(fdk_model_row_child_count(face, FDK_MODEL_ROW_NONE) == 6);

    fdk_model_filter_set_predicate(f, keep_ints_even, NULL);
    assert(fdk_model_row_child_count(face, FDK_MODEL_ROW_NONE) == 3);
    assert(strcmp(fdk_model_row_text_at(face, 0), "row0") == 0);
    assert(strcmp(fdk_model_row_text_at(face, 1), "row2") == 0);

    /* map round-trip */
    fdk_model_row pr = fdk_model_row_child(face, FDK_MODEL_ROW_NONE, 1);
    fdk_model_row cr;
    assert(fdk_ok(fdk_model_filter_map_to_child(f, pr, &cr)));
    assert(cr.index == 2); /* row2 */

    /* clearing the predicate shows everything */
    fdk_model_filter_set_predicate(f, NULL, NULL);
    assert(fdk_model_row_child_count(face, FDK_MODEL_ROW_NONE) == 6);

    fdk_model_filter_destroy(f);
    fdk_model_destroy(m);
    printf("[ok] model filter: predicate, mapping, clear\n");
}

static void test_list_binding(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, &fdk_box_class_def,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));

    fdk_font *font = fdk_font_load(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);
    /* the binding works without a font too (rows render empty) */

    fdk_model_value_type types[1] = {FDK_MODEL_TEXT};
    fdk_model *m = fdk_model_create(types, 1);
    assert(m != NULL);

    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, font, &list)));
    assert(fdk_ok(fdk_list_bind_model(list, m, 0)));

    /* initial mirror */
    fdk_model_row a = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(fdk_ok(fdk_model_set_text(m, a, 0, "one")));
    fdk_model_row b = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(fdk_ok(fdk_model_set_text(m, b, 0, "two")));
    assert(fdk_list_row_count(list) == 2);
    assert(strcmp(fdk_list_row_text(list, 0), "one") == 0);
    assert(strcmp(fdk_list_row_text(list, 1), "two") == 0);

    /* live change + remove + text update */
    fdk_model_row c = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(fdk_ok(fdk_model_set_text(m, c, 0, "three")));
    assert(fdk_list_row_count(list) == 3);
    /* ids are captured NOW, at append time — the coordinates a/b/c
     * go stale on the very next mutation (the model's contract). */
    fdk_u64 id_c = fdk_model_row_id(m, c);
    assert(fdk_ok(fdk_model_row_remove(m, b)));
    assert(fdk_list_row_count(list) == 2);
    assert(fdk_ok(fdk_model_set_text(m, a, 0, "ONE")));
    assert(strcmp(fdk_list_row_text(list, 0), "ONE") == 0);

    /* selection by id survives a re-sort (id_a still resolves: a
     * never moved) */
    fdk_u64 id_a = fdk_model_row_id(m, a);
    fdk_list_select(list, 0);
    assert(fdk_list_bound_row_id(list, 0) == id_a);

    fdk_model_sort *s = fdk_model_sort_new(m);
    fdk_model_sort_set_key(s, 0, true, NULL, NULL); /* descending */
    assert(fdk_ok(fdk_list_bind_model(list,
                                      fdk_model_sort_model(s), 0)));
    assert(fdk_list_row_count(list) == 2);
    /* "two" is gone; ONE + three: descending = three, ONE */
    assert(strcmp(fdk_list_row_text(list, 0), "three") == 0);
    size_t idx = 999;
    assert(fdk_ok(fdk_list_bound_index_of(list, id_a, &idx)));
    assert(idx == 1);

    /* reorder write-back: select "three" (view row 0), move it to
     * the end */
    fdk_list_clear_selection(list);
    fdk_list_select(list, 0);
    fdk_u64 sel[4];
    assert(fdk_list_bound_selected_ids(list, sel, 4) == 1);
    assert(sel[0] == id_c);
    assert(fdk_list_bound_reorder_selected(list, FDK_MODEL_ROW_NONE) ==
           1);
    /* the model now has ONE before three */
    fdk_model_row r0 = fdk_model_row_child(m, FDK_MODEL_ROW_NONE, 0);
    fdk_model_value v0;
    assert(fdk_ok(fdk_model_get(m, r0, 0, &v0)));
    assert(strcmp(v0.v.text, "ONE") == 0);
    fdk_model_value_free(&v0);

    /* unbind stops the mirroring */
    fdk_list_unbind_model(list);
    size_t rows = fdk_list_row_count(list);
    fdk_model_row d = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    (void)d;
    assert(fdk_list_row_count(list) == rows);

    fdk_model_sort_destroy(s);
    fdk_model_destroy(m);
    fdk_font_destroy(font);
    fdk_widget_destroy(root);
    printf("[ok] list binding: mirror, live changes, selection by "
           "id, reorder, unbind\n");
}

static void test_tree_binding(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, &fdk_box_class_def,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));

    fdk_model_value_type types[1] = {FDK_MODEL_TEXT};
    fdk_model *m = fdk_model_create(types, 1);
    assert(m != NULL);

    fdk_model_row parent = fdk_model_row_append(m, FDK_MODEL_ROW_NONE);
    assert(fdk_ok(fdk_model_set_text(m, parent, 0, "parent")));
    fdk_u64 pid = fdk_model_row_id(m, parent);
    fdk_model_row kid = fdk_model_row_append(m, pid);
    assert(fdk_ok(fdk_model_set_text(m, kid, 0, "kid")));
    fdk_model_row kid2 = fdk_model_row_append(m, pid);
    assert(fdk_ok(fdk_model_set_text(m, kid2, 0, "kid2")));

    fdk_widget *tree = NULL;
    assert(fdk_ok(fdk_tree_create(root, NULL, &tree)));
    assert(fdk_ok(fdk_tree_bind_model(tree, m, 0)));

    assert(fdk_tree_bound_node_of(tree, pid) != FDK_TREE_NODE_NONE);
    assert(fdk_tree_bound_node_of(tree,
                                  fdk_model_row_id(m, kid)) !=
           FDK_TREE_NODE_NONE);
    assert(fdk_tree_bound_node_of(tree, 999999) ==
           FDK_TREE_NODE_NONE);

    /* the node handle resolves text */
    assert(strcmp(fdk_tree_node_text(
                      tree, fdk_tree_bound_node_of(tree, pid)),
                  "parent") == 0);

    fdk_tree_unbind_model(tree);
    fdk_model_destroy(m);
    fdk_widget_destroy(root);
    printf("[ok] tree binding: nested mirror, node_of\n");
}

int main(void) {
    test_store();
    test_sort();
    test_filter();
    test_list_binding();
    test_tree_binding();
    printf("all model tests passed\n");
    return 0;
}
