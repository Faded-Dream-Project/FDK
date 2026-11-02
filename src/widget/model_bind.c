/*
 * model_bind.c — the model/view binding (§23/B2e; fdk_model.h's
 * view half).
 *
 * A bound List or Tree MIRRORS the model: the binding subscribes a
 * change listener on the model (or a proxy's face) and applies every
 * row change to the widget's own row CRUD — the widget stays the
 * presentation it always was, the model becomes the data's single
 * home, and sorting/filtering happen by REBINDING to a proxy (the
 * selection follows by row ID, so a re-sort keeps what was
 * selected selected).
 *
 * The binding owns a small id<->view-index map (the model's stable
 * row ids against the widget's row positions), rebuilt on every
 * change signal — the mapping is what selection-by-id and the
 * reorder write-back read.
 */

#define FDK_LOG_TAG "widget"

#include "fdk/fdk_model.h"
#include "fdk/fdk_widgets.h"

#include "widgets_internal.h" /* §28/G-9: the listener decls + the
                               * G-6 destroy-hook prototype        */
#include "core/alloc_internal.h"

#include <string.h>

/* ---- the binding object --------------------------------------------------- */

typedef struct {
    fdk_model *model;      /* borrowed; NULL = unbound             */
    size_t text_column;
    bool tree;             /* list or tree binding                  */

    /* id <-> view index (list) / id <-> node handle (tree)        */
    fdk_u64 *ids;          /* per view row: the model row id        */
    size_t n_ids, cap_ids;
    fdk_tree_node *nodes;  /* tree: per id slot, the node handle    */
} binding;

/* One binding per widget — stored in a small registry keyed by the
 * widget pointer (the widget structs are internal; a side table
 * avoids touching them — the extension-API discipline applied
 * inside the tree). */
#define BIND_MAX 32
static struct {
    fdk_widget *widget;
    binding b;
} g_bindings[BIND_MAX];

static binding *binding_of(fdk_widget *w) {
    if (w == NULL) {
        return NULL;
    }
    for (int i = 0; i < BIND_MAX; i++) {
        if (g_bindings[i].widget == w) {
            return &g_bindings[i].b;
        }
    }
    return NULL;
}

static binding *binding_alloc(fdk_widget *w) {
    /* reuse the slot if bound, else take a free one (or the first:
     * 32 simultaneous bound views is beyond any real UI). */
    int slot = -1;
    for (int i = 0; i < BIND_MAX; i++) {
        if (g_bindings[i].widget == w) {
            slot = i;
            break;
        }
        if (slot < 0 && g_bindings[i].widget == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    memset(&g_bindings[slot], 0, sizeof g_bindings[slot]);
    g_bindings[slot].widget = w;
    return &g_bindings[slot].b;
}

static void ids_push(binding *b, fdk_u64 id, fdk_tree_node node) {
    if (b->n_ids >= b->cap_ids) {
        size_t cap = b->cap_ids ? b->cap_ids * 2 : 16;
        fdk_u64 *grown = fdk_realloc(b->ids, cap * sizeof *grown);
        if (grown == NULL) {
            return;
        }
        b->ids = grown;
        if (b->tree) {
            fdk_tree_node *nodes =
                fdk_realloc(b->nodes, cap * sizeof *nodes);
            if (nodes == NULL) {
                return;
            }
            b->nodes = nodes;
        }
        b->cap_ids = cap;
    }
    b->ids[b->n_ids] = id;
    if (b->tree) {
        b->nodes[b->n_ids] = node;
    }
    b->n_ids++;
}

/* Rebuilds the widget from the model (the initial bind + CLEARED). */
static void rebuild_list(fdk_widget *list, binding *b) {
    fdk_list_begin_batch(list);
    fdk_list_clear(list);
    b->n_ids = 0;
    size_t n = fdk_model_row_child_count(b->model,
                                         FDK_MODEL_ROW_NONE);
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r = fdk_model_row_child(b->model,
                                              FDK_MODEL_ROW_NONE, i);
        fdk_model_value v;
        const char *text = "";
        if (fdk_ok(fdk_model_get(b->model, r, b->text_column, &v))) {
            text = v.v.text != NULL ? v.v.text : "";
        }
        size_t idx = 0;
        if (fdk_ok(fdk_list_append(list, text, &idx))) {
            ids_push(b, fdk_model_row_id(b->model, r),
                     FDK_TREE_NODE_NONE);
        }
        if (v.type == FDK_MODEL_TEXT) {
            fdk_model_value_free(&v);
        }
    }
    fdk_list_end_batch(list);
}

/* The tree rebuild: depth-first over the model's parent links. */
static void rebuild_tree_children(fdk_widget *tree, binding *b,
                                  fdk_u64 parent_id,
                                  fdk_tree_node parent_node) {
    size_t n = fdk_model_row_child_count(b->model, parent_id);
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r = fdk_model_row_child(b->model, parent_id, i);
        fdk_model_value v;
        const char *text = "";
        if (fdk_ok(fdk_model_get(b->model, r, b->text_column, &v))) {
            text = v.v.text != NULL ? v.v.text : "";
        }
        fdk_tree_node node = FDK_TREE_NODE_NONE;
        if (fdk_ok(fdk_tree_node_add(tree, parent_node, text,
                                     &node))) {
            ids_push(b, fdk_model_row_id(b->model, r), node);
            rebuild_tree_children(tree, b, fdk_model_row_id(b->model, r),
                                  node);
        }
        if (v.type == FDK_MODEL_TEXT) {
            fdk_model_value_free(&v);
        }
    }
}

static void rebuild_tree(fdk_widget *tree, binding *b) {
    fdk_tree_clear(tree);
    b->n_ids = 0;
    rebuild_tree_children(tree, b, FDK_MODEL_ROW_NONE,
                          FDK_TREE_NODE_NONE);
}

/* The change handler: applies one change to the mirror. The
 * simplest CORRECT policy for the proxy reorder/move cases is a
 * full rebuild inside a batch (the widget's own end_batch settles
 * once — the O(N) mirror is the binding's contract; the model
 * stays the scalable half). */
static void binding_changed(fdk_model *model, fdk_model_change change,
                            fdk_model_row row, void *user) {
    fdk_widget *w = user;
    binding *b = binding_of(w);
    if (b == NULL || b->model != model) {
        return; /* stale handler after unbind */
    }
    switch (change) {
    case FDK_MODEL_ROW_CHANGED: {
        /* Update the one row's text in place when the index still
         * maps; else rebuild. */
        if (!b->tree && row.parent == FDK_MODEL_ROW_NONE &&
            row.index < b->n_ids) {
            fdk_model_value v;
            if (fdk_ok(fdk_model_get(model, row, b->text_column,
                                     &v))) {
                const char *text =
                    v.v.text != NULL ? v.v.text : "";
                (void)fdk_list_set_row_text(w, row.index, text);
                if (v.type == FDK_MODEL_TEXT) {
                    fdk_model_value_free(&v);
                }
                return;
            }
        }
        if (b->tree && row.parent != FDK_MODEL_ROW_NONE) {
            /* a nested cell change: rebuild the tree (rare) */
            rebuild_tree(w, b);
            return;
        }
        if (b->tree) {
            rebuild_tree(w, b);
            return;
        }
        rebuild_list(w, b);
        return;
    }
    case FDK_MODEL_ROW_INSERTED:
    case FDK_MODEL_ROW_REMOVED:
    case FDK_MODEL_ROW_MOVED:
    case FDK_MODEL_CLEARED:
    default:
        if (b->tree) {
            rebuild_tree(w, b);
        } else {
            rebuild_list(w, b);
        }
        return;
    }
}

/* ---- public: the List binding ---------------------------------------------- */

fdk_result fdk_list_bind_model(fdk_widget *list, fdk_model *model,
                               size_t text_column) {
    if (list == NULL || model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* unbind any previous binding on this widget (frees its id
     * arrays — the rebind-leak the model suite's LSan leg caught)
     */
    if (binding_of(list) != NULL) {
        fdk_list_unbind_model(list);
    }
    binding *b = binding_alloc(list);
    b->model = model;
    b->text_column = text_column;
    b->tree = false;
    /* §28/G-9: subscribe through the model's LISTENER LIST (not
     * the single public on_change slot) so a second bound view
     * cannot overwrite the first's handler — N views on one model
     * is the model/view contract's basic case. */
    fdk_result rc = fdk__model_add_listener(model, binding_changed,
                                            list);
    if (!fdk_ok(rc)) {
        fdk_list_unbind_model(list);
        return rc;
    }
    rebuild_list(list, b);
    return FDK_OK;
}

void fdk_list_unbind_model(fdk_widget *list) {
    binding *b = binding_of(list);
    if (b == NULL) {
        return;
    }
    if (b->model != NULL) {
        /* remove THIS widget's listener only — the model's other
         * bound views (and the app's own on_change) stay live */
        fdk__model_remove_listener(b->model, binding_changed, list);
    }
    fdk_free(b->ids);
    fdk_free(b->nodes);
    for (int i = 0; i < BIND_MAX; i++) {
        if (g_bindings[i].widget == list) {
            g_bindings[i].widget = NULL;
        }
    }
}

/* §26/G-6 — the destroy hook. teardown_free (widget.c) calls this
 * for every dying widget; a bound one releases its slot HERE so the
 * registry never holds a stale pointer.
 *
 * Deliberately NOT the public unbind's full path: touching the
 * model is unsafe at this point because the MODEL may already be
 * freed (an application that shuts its models down before its
 * window — fdk-files does exactly that). The model's listener list
 * may keep pointing at binding_changed with this dead widget as
 * user data; that call is already guarded (binding_of(w) returns
 * NULL for a cleared slot — the mirror no-ops) and the listener
 * dies with the model or is removed by a later unbind of a view
 * that shares it. */
void fdk__model_bind_widget_destroyed(fdk_widget *w) {
    if (w == NULL) {
        return;
    }
    for (int i = 0; i < BIND_MAX; i++) {
        if (g_bindings[i].widget == w) {
            binding *b = &g_bindings[i].b;
            fdk_free(b->ids);
            fdk_free(b->nodes);
            g_bindings[i].widget = NULL;
            return;
        }
    }
}

/* ---- public: the Tree binding ---------------------------------------------- */

fdk_result fdk_tree_bind_model(fdk_widget *tree, fdk_model *model,
                               size_t text_column) {
    if (tree == NULL || model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (binding_of(tree) != NULL) {
        fdk_list_unbind_model(tree); /* the shared registry slot */
    }
    binding *b = binding_alloc(tree);
    b->model = model;
    b->text_column = text_column;
    b->tree = true;
    fdk_result rc = fdk__model_add_listener(model, binding_changed,
                                            tree);
    if (!fdk_ok(rc)) {
        fdk_list_unbind_model(tree);
        return rc;
    }
    rebuild_tree(tree, b);
    return FDK_OK;
}

void fdk_tree_unbind_model(fdk_widget *tree) {
    fdk_list_unbind_model(tree); /* the same registry slot shape */
}

/* ---- public: selection + reorder mapping ------------------------------------ */

/* The model row id behind a view row (the selection-by-id bridge:
 * an app holds ids; a re-sort moves view rows, ids stay). */
fdk_u64 fdk_list_bound_row_id(fdk_widget *list, size_t view_row) {
    binding *b = binding_of(list);
    if (b == NULL || b->tree || view_row >= b->n_ids) {
        return FDK_MODEL_ROW_NONE;
    }
    return b->ids[view_row];
}

fdk_result fdk_list_bound_index_of(fdk_widget *list, fdk_u64 id,
                                   size_t *out_view_row) {
    binding *b = binding_of(list);
    if (b == NULL || b->tree || out_view_row == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < b->n_ids; i++) {
        if (b->ids[i] == id) {
            *out_view_row = i;
            return FDK_OK;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

/* Every selected row's model id (in view order). Writes at most
 * cap; returns the total selected count (like snprintf). */
size_t fdk_list_bound_selected_ids(fdk_widget *list, fdk_u64 *out_ids,
                                   size_t cap) {
    binding *b = binding_of(list);
    if (b == NULL || b->tree) {
        return 0;
    }
    size_t n = fdk_list_selected_count(list);
    size_t written = 0;
    for (size_t pos = 0; pos < n; pos++) {
        size_t row = 0;
        if (!fdk_ok(fdk_list_selected_at(list, pos, &row))) {
            continue;
        }
        if (out_ids != NULL && written < cap && row < b->n_ids) {
            out_ids[written] = b->ids[row];
        }
        written++;
    }
    return written;
}

/* The tree twin: the node handle behind a model id. */
fdk_tree_node fdk_tree_bound_node_of(fdk_widget *tree, fdk_u64 id) {
    binding *b = binding_of(tree);
    if (b == NULL || !b->tree) {
        return FDK_TREE_NODE_NONE;
    }
    for (size_t i = 0; i < b->n_ids; i++) {
        if (b->ids[i] == id) {
            return b->nodes[i];
        }
    }
    return FDK_TREE_NODE_NONE;
}

/* The reorder write-back: moves the selected model rows to just
 * before `target_id` (FDK_MODEL_ROW_NONE = the end). The app calls
 * this from its own drag handler (or a key binding) — the move
 * lands in the MODEL, the change signal rebuilds the view, and
 * the selection (held by id) follows. Returns the number of rows
 * actually moved. */
size_t fdk_list_bound_reorder_selected(fdk_widget *list,
                                       fdk_u64 target_id) {
    binding *b = binding_of(list);
    if (b == NULL || b->tree) {
        return 0;
    }
    fdk_u64 ids[64];
    size_t n = fdk_list_bound_selected_ids(list, ids, 64);
    if (n == 0) {
        return 0;
    }
    /* Row moves belong on the underlying STORE (a proxy face maps,
     * it does not mutate row structure — fdk_model_store_of). */
    fdk_model *store = fdk_model_store_of(b->model);
    fdk_model_row target;
    bool have_target = false;
    if (target_id != FDK_MODEL_ROW_NONE) {
        have_target = fdk_ok(fdk_model_row_resolve(store, target_id,
                                                   &target));
    }
    size_t moved = 0;
    size_t insert_at = 0;
    if (have_target) {
        insert_at = target.index;
    } else {
        insert_at = fdk_model_row_child_count(store,
                                              FDK_MODEL_ROW_NONE);
    }
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r;
        if (!fdk_ok(fdk_model_row_resolve(store, ids[i], &r))) {
            continue;
        }
        /* A row moving DOWN passes its own slot: compensate. */
        size_t at = insert_at;
        if (r.index < insert_at) {
            /* moving later — after the removal the target shifts */
        } else {
            at = insert_at > 0 && moved > 0 ? insert_at : insert_at;
        }
        if (fdk_ok(fdk_model_row_move(store, r,
                                      FDK_MODEL_ROW_NONE, at))) {
            moved++;
            insert_at = at + 1;
        }
    }
    return moved;
}
