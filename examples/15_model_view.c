/* 15_model_view.c — the §23/B2e model/view split, live.
 *
 * One window, four rows:
 *   top      a toolbar: Sort A-Z / Z-A / by size, a filter entry
 *            (rows whose text contains it), an Add button, and a
 *            Remove-selected button
 *   upper    a List BOUND to a sort>filter proxy chain over one
 *            model (the app never touches the widget's row API)
 *   lower    a SECOND List bound to the SAME proxy face — the
 *            §28/G-9 point: one model, N views, all mirroring
 *            (what a split view is)
 *   bottom   a status label mirroring the selection BY MODEL ID
 *
 * The point being demonstrated: the data lives in the MODEL (+
 * proxies); the views are mirrors. Sorting re-keys the proxy (the
 * selection follows by id); typing in the filter re-predicates;
 * Add/Remove mutate the model and every view updates through the
 * change signals alone. The reorder button moves the SELECTED
 * model rows (of the first view) to the end — through the model,
 * all views follow.
 */

#include "example_window.h"

#include "fdk/fdk_model.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_widgets.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static fdk_example app;

static struct {
    fdk_model *store;       /* the data's one home              */
    fdk_model_sort *sort;   /* the sort proxy                   */
    fdk_model_filter *filter; /* the filter proxy              */
    fdk_widget *list;
    fdk_widget *list2;      /* the second view (§28/G-9)        */
    fdk_widget *filter_entry;
    fdk_font *font;
    int sort_dir;           /* 0 = insertion, 1 = asc, 2 = desc */
} g;

static void refresh_binding(void) {
    /* Re-bind BOTH lists to the filter's FACE (the head of the
     * proxy chain). The rebuild reads through both proxies; the
     * listener list keeps both mirrors live. */
    fdk_list_bind_model(g.list,
                        fdk_model_filter_model(g.filter), 0);
    fdk_list_bind_model(g.list2,
                        fdk_model_filter_model(g.filter), 0);
}

static void status_selection(void) {
    fdk_u64 ids[32];
    size_t n = fdk_list_bound_selected_ids(g.list, ids, 32);
    if (n == 0) {
        fdk_example_set_status(&app, "nothing selected");
        return;
    }
    char buf[160];
    int off = snprintf(buf, sizeof buf, "%zu selected (ids:", n);
    for (size_t i = 0; i < n && i < 4; i++) {
        off += snprintf(buf + off, sizeof buf - (size_t)off,
                        " %llu", (unsigned long long)ids[i]);
    }
    if (n > 4) {
        snprintf(buf + off, sizeof buf - (size_t)off, " ...)");
    } else {
        snprintf(buf + off, sizeof buf - (size_t)off, ")");
    }
    fdk_example_set_status(&app, buf);
}

static void on_selection(fdk_widget *list, void *user) {
    (void)list; (void)user;
    status_selection();
}

static bool filter_contains(fdk_model *m, fdk_u64 id, void *user) {
    (void)user;
    const char *needle = fdk_entry_get_text(g.filter_entry);
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    fdk_model_row r;
    if (!fdk_ok(fdk_model_row_resolve(m, id, &r))) {
        return false;
    }
    fdk_model_value v;
    bool keep = false;
    if (fdk_ok(fdk_model_get(m, r, 0, &v))) {
        keep = v.v.text != NULL &&
               strstr(v.v.text, needle) != NULL;
        fdk_model_value_free(&v);
    }
    return keep;
}

static void on_filter_changed(fdk_widget *entry, void *user) {
    (void)entry; (void)user;
    fdk_model_filter_set_predicate(g.filter, filter_contains,
                                   NULL);
    refresh_binding();
    status_selection();
}

static void set_sort(int dir); /* fwd */

static void set_sort_wrapper_asc(fdk_widget *b, void *u) {
    (void)b; (void)u; set_sort(1);
}
static void set_sort_wrapper_desc(fdk_widget *b, void *u) {
    (void)b; (void)u; set_sort(2);
}
static void set_sort_wrapper_ins(fdk_widget *b, void *u) {
    (void)b; (void)u; set_sort(0);
}

static void set_sort(int dir) {
    g.sort_dir = dir;
    if (dir == 0) {
        fdk_model_sort_set_key(g.sort, 0, false, NULL, NULL);
        /* insertion order: un-sort by the row id (monotonic) */
        fdk_model_sort_set_key(g.sort, (size_t)-1, false, NULL,
                               NULL);
    } else {
        fdk_model_sort_set_key(g.sort, 0, dir == 2, NULL, NULL);
    }
    refresh_binding();
    status_selection();
}

static void add_row(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    static int counter = 7;
    char text[48];
    snprintf(text, sizeof text, "item %d", counter++);
    fdk_model_row r = fdk_model_row_append(
        fdk_model_store_of(fdk_model_filter_model(g.filter)),
        FDK_MODEL_ROW_NONE);
    (void)fdk_model_set_text(g.store, r, 0, text);
    /* append bypasses the face; signal through the store is
     * already live — but the proxy chain must rebuild, which the
     * store's own change fires through the filter's child hook */
    status_selection();
}

static void remove_selected(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    fdk_u64 ids[32];
    size_t n = fdk_list_bound_selected_ids(g.list, ids, 32);
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r;
        if (fdk_ok(fdk_model_row_resolve(g.store, ids[i], &r))) {
            (void)fdk_model_row_remove(g.store, r);
        }
    }
    status_selection();
}

static void reorder_selected(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    (void)fdk_list_bound_reorder_selected(g.list,
                                          FDK_MODEL_ROW_NONE);
    status_selection();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!fdk_example_init(&app.ctx, "15")) {
        return 1;
    }

    g.font = fdk_font_load_system_default(14);
    if (g.font == NULL) {
        g.font = fdk_font_load(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);
    }

    fdk_window_options wopts = {
        .title = "FDK 15 — model/view",
        .width = 560,
        .height = 420,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fdk_shutdown(app.ctx);
        return 1;
    }
    (void)fdk_window_set_decorated(app.window, true);
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(app.window, &root)));

    /* the model + proxy chain */
    fdk_model_value_type types[1] = {FDK_MODEL_TEXT};
    g.store = fdk_model_create(types, 1);
    g.sort = fdk_model_sort_new(g.store);
    g.filter = fdk_model_filter_new(fdk_model_sort_model(g.sort));
    const char *seed[] = {"pear",   "apple",     "orange",
                          "banana", "cherry",    "grape",
                          "peach"};
    for (int i = 0; i < 7; i++) {
        fdk_model_row r = fdk_model_row_append(g.store,
                                               FDK_MODEL_ROW_NONE);
        (void)fdk_model_set_text(g.store, r, 0, seed[i]);
    }

    /* the toolbar */
    fdk_widget *bar = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &bar)));
    fdk_widget *b_asc = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Sort A-Z",
                                    &b_asc)));
    fdk_button_set_on_activate(b_asc, set_sort_wrapper_asc,
                              NULL);
    fdk_widget *b_desc = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Sort Z-A",
                                    &b_desc)));
    fdk_button_set_on_activate(b_desc, set_sort_wrapper_desc,
                              NULL);
    fdk_widget *b_ins = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Insertion order",
                                    &b_ins)));
    fdk_button_set_on_activate(b_ins, set_sort_wrapper_ins,
                              NULL);
    fdk_widget *b_add = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Add", &b_add)));
    fdk_button_set_on_activate(b_add, add_row, NULL);
    fdk_widget *b_del = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Remove selected",
                                    &b_del)));
    fdk_button_set_on_activate(b_del, remove_selected, NULL);
    fdk_widget *b_re = NULL;
    assert(fdk_ok(fdk_button_create(bar, g.font, "Move to end",
                                    &b_re)));
    fdk_button_set_on_activate(b_re, reorder_selected, NULL);

    /* the filter entry */
    g.filter_entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, g.font, NULL,
                                 &g.filter_entry)));
    fdk_entry_set_placeholder(g.filter_entry, "filter contains...");
    fdk_entry_set_on_changed(g.filter_entry, on_filter_changed,
                             NULL);

    /* the two bound views (the same model, the same proxy face —
     * the split-view shape) */
    g.list = NULL;
    assert(fdk_ok(fdk_list_create(root, g.font, &g.list)));
    fdk_list_set_selection_mode(g.list, FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_selection_changed(g.list, on_selection, NULL);
    g.list2 = NULL;
    assert(fdk_ok(fdk_list_create(root, g.font, &g.list2)));
    fdk_list_set_selection_mode(g.list2, FDK_LIST_SELECTION_MULTIPLE);
    fdk_widget_set_expand(g.list2, true, true);
    refresh_binding();

    fdk_window_show(app.window);
    fdk_example_run(&app);

    fdk_list_unbind_model(g.list);
    fdk_list_unbind_model(g.list2);
    fdk_model_filter_destroy(g.filter);
    fdk_model_sort_destroy(g.sort);
    fdk_model_destroy(g.store);
    fdk_font_destroy(g.font);
    fdk_example_close(&app);
    return 0;
}
