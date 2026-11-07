/*
 * model.c — the item model + the sort/filter proxies (§23/B2e;
 * include/fdk/fdk_model.h).
 *
 * Rows carry a stable internal id (a monotonically increasing u64;
 * never reused) plus a (parent-id, index) identity that mutations
 * invalidate — the split GTK solves the same way. The store is a
 * flat array of row records with per-row cell arrays; the parent's
 * child order is the array order filtered by parent id, which
 * makes child_count/child O(N) but insertion O(1) — fine at
 * widget scale, and the proxies sit on top with their own
 * permutations.
 *
 * The views bind through the SAME fdk_model interface whether it
 * is a store or a proxy: the proxy types embed an fdk_model face
 * whose row addressing is the PROXY's, and whose change signals
 * fire when the permutation moves — the view cannot tell (GTK's
 *GtkTreeModelFilter trick, restated in C).
 */

#define FDK_LOG_TAG "widget"

#include "fdk/fdk_model.h"

#include "widgets_internal.h" /* §28/G-9: the listener decls     */
#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- the row record ------------------------------------------------------ */

typedef struct {
    fdk_u64 id;
    fdk_u64 parent;
    fdk_model_value *cells; /* [columns]                                */
} m_row;

/* §28/G-9: one subscribed change listener on a model. */
typedef struct {
    fdk_model_change_fn fn;
    void *user;
} m_listener;

struct fdk_model {
    fdk_model_value_type *types;
    size_t columns;
    m_row *rows;        /* flat; order = insertion order within
                         * the parent                                */
    size_t n_rows, cap_rows;
    fdk_u64 next_id;    /* 1, 2, ... (0 = never)                     */

    fdk_model_change_fn on_change;
    void *change_user;

    /* §28/G-9: N listeners per model. The single on_change slot
     * above stays the app's simple path; the binding layer and the
     * proxies subscribe through this list so any number of views
     * can mirror one model (the model/view contract's basic case:
     * two panes, one listing model). */
    m_listener *listeners;
    size_t n_listeners, cap_listeners;

    /* The proxy back-faces: NULL for a plain store. */
    fdk_model *sort_face;
    fdk_model *filter_face;
    void *proxy_owner;

    /* Round 12 — mutation batching. A store being rebuilt (a
     * directory reload's clear + N append + 4N cell sets) fired
     * every change as it happened; each fire walked the proxy
     * chain and re-mirrored a bound view (the O(N²) the nav rig
     * measured as a pegged core). Between begin/end_batch the
     * fires are HELD; end_batch releases exactly ONE
     * FDK_MODEL_CLEARED (the "everything changed" event a bound
     * view answers with one replay). Proxies forward the batch
     * discipline to their faces, so a batch on the STORE coalesces
     * the whole chain. Zero public API change beyond the two calls
     * (fdk_model_begin_batch / fdk_model_end_batch). */
    unsigned batch_depth;
    bool batch_pending;
};

/* ---- helpers --------------------------------------------------------------- */

static size_t row_count_of(fdk_model *m, fdk_u64 parent) {
    size_t n = 0;
    for (size_t i = 0; i < m->n_rows; i++) {
        if (m->rows[i].parent == parent) {
            n++;
        }
    }
    return n;
}

/* Finds the array index of (parent, index). -1 when absent. */
static long row_find(fdk_model *m, fdk_u64 parent, size_t index) {
    size_t seen = 0;
    for (size_t i = 0; i < m->n_rows; i++) {
        if (m->rows[i].parent == parent) {
            if (seen == index) {
                return (long)i;
            }
            seen++;
        }
    }
    return -1;
}

static long row_find_id(fdk_model *m, fdk_u64 id) {
    for (size_t i = 0; i < m->n_rows; i++) {
        if (m->rows[i].id == id) {
            return (long)i;
        }
    }
    return -1;
}

static void fire_change(fdk_model *m, fdk_model_change change,
                        fdk_u64 parent, size_t index) {
    if (m->batch_depth > 0) {
        /* Held: the batch's end fires once for everything. (The
         * suppressed events are not lost semantics for the only
         * current listener class — the view bindings — because the
         * batch's single CLEARED subsumes them: a replay reads the
         * model's final state.) */
        m->batch_pending = true;
        return;
    }
    fdk_model_row r = {parent, index};
    if (m->on_change != NULL) {
        m->on_change(m, change, r, m->change_user);
    }
    /* §28/G-9: the listener list fires AFTER the classic slot (the
     * app's own handler first, then every subscribed view). The
     * walk is by index over a STABLE copy: a listener may unbind
     * itself (or another) mid-flight — the compaction only shifts
     * LATER entries down, and n_listeners is re-read each step. */
    for (size_t i = 0; i < m->n_listeners; i++) {
        if (m->listeners[i].fn != NULL) {
            m->listeners[i].fn(m, change, r, m->listeners[i].user);
        }
    }
}

/* §28/G-9 — the internal listener subscribe/unsubscribe used by
 * model_bind.c (and available to anything inside the tree that
 * needs a second ear on a model). */
fdk_result fdk__model_add_listener(fdk_model *model,
                                   fdk_model_change_fn fn, void *user) {
    if (model == NULL || fn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < model->n_listeners; i++) {
        if (model->listeners[i].fn == fn &&
            model->listeners[i].user == user) {
            return FDK_OK; /* idempotent */
        }
    }
    if (model->n_listeners >= model->cap_listeners) {
        size_t cap = model->cap_listeners ? model->cap_listeners * 2
                                          : 4;
        m_listener *grown =
            fdk_realloc(model->listeners, cap * sizeof *grown);
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        model->listeners = grown;
        model->cap_listeners = cap;
    }
    model->listeners[model->n_listeners].fn = fn;
    model->listeners[model->n_listeners].user = user;
    model->n_listeners++;
    return FDK_OK;
}

void fdk__model_remove_listener(fdk_model *model,
                                fdk_model_change_fn fn, void *user) {
    if (model == NULL) {
        return;
    }
    for (size_t i = 0; i < model->n_listeners; i++) {
        if (model->listeners[i].fn == fn &&
            model->listeners[i].user == user) {
            memmove(&model->listeners[i],
                    &model->listeners[i + 1],
                    (model->n_listeners - i - 1) *
                        sizeof *model->listeners);
            model->n_listeners--;
            return;
        }
    }
}

/* ---- the proxy-face dispatch --------------------------------------------
 * A proxy's model FACE is the first member of its proxy struct; the
 * face's proxy_owner marks it (NULL = a real store). The read paths
 * below dispatch: a face's rows are the proxy's PERMUTATION of the
 * child's top level; row ids are the child's stable ids (so a
 * selection held by id survives a re-sort). Mutations refuse on a
 * face — proxies map, they do not store. */

/* Both proxies share the shape: a child model, a permutation of
 * the child's TOP-LEVEL rows (array of child row ids), and a model
 * FACE whose row addressing is the proxy's. The face is the proxy
 * struct's first member so casts are legal. */

typedef struct fdk_model_proxy {
    fdk_model face;         /* the proxy's own model face          */
    fdk_model *child;
    fdk_u64 *perm;          /* child row ids, proxy order          */
    size_t n_perm, cap_perm;
} fdk_model_proxy;

struct fdk_model_sort {
    fdk_model_proxy p;
    size_t key_column;
    bool descending;
    fdk_model_sort_fn fn;
    void *fn_user;
};

/* ---- the proxy-face dispatch --------------------------------------------
 * A proxy's model FACE is the first member of its proxy struct; the
 * face's proxy_owner marks it (NULL = a real store). The read paths
 * below dispatch: a face's rows are the proxy's PERMUTATION of the
 * child's top level; row ids are the child's stable ids (so a
 * selection held by id survives a re-sort). Mutations refuse on a
 * face — proxies map, they do not store. */

static bool is_proxy_face(const fdk_model *m) {
    return m != NULL && m->proxy_owner != NULL;
}

static fdk_model_proxy *proxy_owner_of(fdk_model *m) {
    /* both proxy structs start with the fdk_model_proxy head      */
    return (fdk_model_proxy *)m->proxy_owner;
}

fdk_model *fdk_model_store_of(fdk_model *model) {
    if (model == NULL) {
        return NULL;
    }
    if (is_proxy_face(model)) {
        return proxy_owner_of(model)->child;
    }
    return model;
}

static void free_cells(fdk_model *m, m_row *r) {
    for (size_t c = 0; c < m->columns; c++) {
        if (m->types[c] == FDK_MODEL_TEXT &&
            r->cells[c].v.text != NULL) {
            fdk_free(r->cells[c].v.text);
        }
    }
    fdk_free(r->cells);
}

fdk_model *fdk_model_create(const fdk_model_value_type *column_types,
                            size_t columns) {
    if (column_types == NULL || columns == 0 || columns > 256) {
        return NULL;
    }
    fdk_model *m = fdk_alloc(sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    memset(m, 0, sizeof *m);
    m->types = fdk_alloc_array(columns, sizeof *m->types);
    if (m->types == NULL) {
        fdk_free(m);
        return NULL;
    }
    memcpy(m->types, column_types, columns * sizeof *m->types);
    m->columns = columns;
    m->next_id = 1;
    return m;
}

void fdk_model_destroy(fdk_model *model) {
    if (model == NULL) {
        return;
    }
    for (size_t i = 0; i < model->n_rows; i++) {
        free_cells(model, &model->rows[i]);
    }
    fdk_free(model->rows);
    fdk_free(model->types);
    fdk_free(model->listeners);
    fdk_free(model);
}

void fdk_model_set_on_change(fdk_model *model,
                             fdk_model_change_fn fn, void *user) {
    if (model != NULL) {
        model->on_change = fn;
        model->change_user = user;
    }
}

/* ---- rows -------------------------------------------------------------------- */

static fdk_model_row row_at_index(fdk_model *m, long ai) {
    fdk_model_row r = {FDK_MODEL_ROW_NONE, 0};
    if (ai < 0) {
        return r;
    }
    r.parent = m->rows[ai].parent;
    /* the index within the parent */
    size_t seen = 0;
    for (long i = 0; i < ai; i++) {
        if (m->rows[i].parent == r.parent) {
            seen++;
        }
    }
    r.index = seen;
    return r;
}

fdk_model_row fdk_model_row_append(fdk_model *model, fdk_u64 parent) {
    fdk_model_row bad = {FDK_MODEL_ROW_NONE, 0};
    if (model == NULL) {
        return bad;
    }
    if (is_proxy_face(model)) {
        /* proxies map; they do not store — mutate the CHILD. */
        return bad;
    }
    if (parent != FDK_MODEL_ROW_NONE &&
        row_find_id(model, parent) < 0) {
        return bad; /* no such parent */
    }
    if (model->n_rows >= model->cap_rows) {
        size_t cap = model->cap_rows ? model->cap_rows * 2 : 16;
        m_row *grown = fdk_realloc(model->rows, cap * sizeof *grown);
        if (grown == NULL) {
            return bad;
        }
        model->rows = grown;
        model->cap_rows = cap;
    }
    m_row *r = &model->rows[model->n_rows];
    r->id = model->next_id++;
    r->parent = parent;
    r->cells = fdk_alloc_array(model->columns, sizeof *r->cells);
    if (r->cells == NULL) {
        model->next_id--; /* not consumed */
        return bad;
    }
    memset(r->cells, 0, model->columns * sizeof *r->cells);
    model->n_rows++;

    fdk_model_row out = row_at_index(model, (long)(model->n_rows - 1));
    fire_change(model, FDK_MODEL_ROW_INSERTED, out.parent, out.index);
    return out;
}

fdk_model_row fdk_model_row_insert(fdk_model *model, fdk_u64 parent,
                                   size_t index) {
    /* Insert-at-index on the flat store: append, then MOVE the
     * array slot into the parent's child order. */
    size_t count = model != NULL ? row_count_of(model, parent) : 0;
    if (model == NULL || index > count) {
        fdk_model_row bad = {FDK_MODEL_ROW_NONE, 0};
        return bad;
    }
    fdk_model_row appended = fdk_model_row_append(model, parent);
    if (appended.parent == FDK_MODEL_ROW_NONE && parent != 0 &&
        parent != FDK_MODEL_ROW_NONE) {
        return appended; /* append failed */
    }
    if (index == count) {
        return appended; /* the natural end */
    }
    /* Move the last array slot up to `index' within the parent. */
    long ai = row_find(model, appended.parent, appended.index);
    if (ai < 0) {
        return appended;
    }
    m_row moving = model->rows[ai];
    /* Shift the siblings after `index' down by one array slot. */
    long dst = ai;
    size_t seen = 0;
    for (long i = 0; i < ai; i++) {
        if (model->rows[i].parent == parent) {
            if (seen == index) {
                dst = i;
                break;
            }
            seen++;
        }
    }
    if (dst == ai) {
        return appended;
    }
    memmove(&model->rows[dst + 1], &model->rows[dst],
            (size_t)(ai - dst) * sizeof *model->rows);
    model->rows[dst] = moving;
    fdk_model_row out = row_at_index(model, dst);
    fire_change(model, FDK_MODEL_ROW_MOVED, out.parent, out.index);
    return out;
}

fdk_result fdk_model_row_remove(fdk_model *model, fdk_model_row row) {
    if (model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    long ai = row_find(model, row.parent, row.index);
    if (ai < 0) {
        return FDK_ERR_NOT_FOUND;
    }
    fdk_u64 id = model->rows[ai].id;
    /* Remove the subtree: this row and every descendant (a
     * descendant = whose parent chain reaches id; iterate to a
     * fixpoint over the flat array). */
    bool again = true;
    while (again) {
        again = false;
        for (size_t i = 0; i < model->n_rows; i++) {
            if (model->rows[i].parent == id) {
                id = model->rows[i].id; /* depth-first by re-entry */
                again = true;
                break;
            }
        }
        if (!again) {
            break;
        }
        /* remove the current id's row */
        long ri = row_find_id(model, id);
        if (ri < 0) {
            break;
        }
        free_cells(model, &model->rows[ri]);
        memmove(&model->rows[ri], &model->rows[ri + 1],
                (size_t)((long)model->n_rows - ri - 1) *
                    sizeof *model->rows);
        model->n_rows--;
        again = true;
    }
    /* remove the root of the subtree last */
    long ri = row_find(model, row.parent, row.index);
    if (ri >= 0) {
        free_cells(model, &model->rows[ri]);
        memmove(&model->rows[ri], &model->rows[ri + 1],
                (size_t)((long)model->n_rows - ri - 1) *
                    sizeof *model->rows);
        model->n_rows--;
    }
    fire_change(model, FDK_MODEL_ROW_REMOVED, row.parent, row.index);
    return FDK_OK;
}

fdk_result fdk_model_row_move(fdk_model *model, fdk_model_row row,
                              fdk_u64 new_parent, size_t new_index) {
    if (model == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    long ai = row_find(model, row.parent, row.index);
    if (ai < 0) {
        return FDK_ERR_NOT_FOUND;
    }
    if (new_parent != FDK_MODEL_ROW_NONE &&
        row_find_id(model, new_parent) < 0) {
        return FDK_ERR_NOT_FOUND;
    }
    /* Moving under one's own subtree is refused. */
    fdk_u64 p = new_parent;
    while (p != FDK_MODEL_ROW_NONE) {
        if (p == model->rows[ai].id) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        long pi = row_find_id(model, p);
        if (pi < 0) {
            break;
        }
        p = model->rows[pi].parent;
    }
    m_row moving = model->rows[ai];
    memmove(&model->rows[ai], &model->rows[ai + 1],
            (size_t)((long)model->n_rows - ai - 1) *
                sizeof *model->rows);
    model->n_rows--;

    /* Re-insert at the new parent's index: grow, find the slot. */
    if (model->n_rows >= model->cap_rows) {
        size_t cap = model->cap_rows ? model->cap_rows * 2 : 16;
        m_row *grown = fdk_realloc(model->rows, cap * sizeof *grown);
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        model->rows = grown;
        model->cap_rows = cap;
    }
    size_t count = row_count_of(model, new_parent);
    if (new_index > count) {
        new_index = count;
    }
    /* The array slot: after the (new_index-1)-th existing child of
     * new_parent; end of the parent's run (or the array end for
     * NONE, which is fine — NONE rows interleave legally since
     * identity is (parent,index), not contiguity). */
    size_t seen = 0;
    size_t slot = model->n_rows;
    for (size_t i = 0; i < model->n_rows; i++) {
        if (model->rows[i].parent == new_parent) {
            if (seen == new_index) {
                slot = i;
                break;
            }
            seen++;
        } else if (seen > new_index) {
            slot = i;
            break;
        }
    }
    memmove(&model->rows[slot + 1], &model->rows[slot],
            (model->n_rows - slot) * sizeof *model->rows);
    model->rows[slot] = moving;
    model->n_rows++;
    fire_change(model, FDK_MODEL_ROW_MOVED, new_parent, new_index);
    return FDK_OK;
}

size_t fdk_model_row_child_count(fdk_model *model, fdk_u64 parent) {
    if (model == NULL) {
        return 0;
    }
    if (is_proxy_face(model)) {
        fdk_model_proxy *p = proxy_owner_of(model);
        return parent == FDK_MODEL_ROW_NONE ? p->n_perm : 0;
    }
    return row_count_of(model, parent);
}

fdk_model_row fdk_model_row_child(fdk_model *model, fdk_u64 parent,
                                  size_t index) {
    fdk_model_row bad = {FDK_MODEL_ROW_NONE, 0};
    if (model == NULL) {
        return bad;
    }
    if (is_proxy_face(model)) {
        if (parent != FDK_MODEL_ROW_NONE ||
            index >= proxy_owner_of(model)->n_perm) {
            return bad;
        }
        fdk_model_row r = {FDK_MODEL_ROW_NONE, index};
        return r;
    }
    long ai = row_find(model, parent, index);
    return ai < 0 ? bad : row_at_index(model, ai);
}

fdk_u64 fdk_model_row_id(fdk_model *model, fdk_model_row row) {
    if (model == NULL) {
        return FDK_MODEL_ROW_NONE;
    }
    if (is_proxy_face(model)) {
        fdk_model_proxy *p = proxy_owner_of(model);
        if (row.parent != FDK_MODEL_ROW_NONE ||
            row.index >= p->n_perm) {
            return FDK_MODEL_ROW_NONE;
        }
        return p->perm[row.index]; /* the child's stable id        */
    }
    long ai = row_find(model, row.parent, row.index);
    return ai < 0 ? FDK_MODEL_ROW_NONE : model->rows[ai].id;
}

fdk_result fdk_model_row_resolve(fdk_model *model, fdk_u64 id,
                                 fdk_model_row *out_row) {
    if (model == NULL || out_row == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (is_proxy_face(model)) {
        fdk_model_proxy *p = proxy_owner_of(model);
        for (size_t i = 0; i < p->n_perm; i++) {
            if (p->perm[i] == id) {
                *out_row = (fdk_model_row){FDK_MODEL_ROW_NONE, i};
                return FDK_OK;
            }
        }
        return FDK_ERR_NOT_FOUND;
    }
    long ai = row_find_id(model, id);
    if (ai < 0) {
        return FDK_ERR_NOT_FOUND;
    }
    *out_row = row_at_index(model, ai);
    return FDK_OK;
}

void fdk_model_clear(fdk_model *model) {
    if (model == NULL) {
        return;
    }
    for (size_t i = 0; i < model->n_rows; i++) {
        free_cells(model, &model->rows[i]);
    }
    model->n_rows = 0;
    fire_change(model, FDK_MODEL_CLEARED, FDK_MODEL_ROW_NONE, 0);
}

/* Round 12 — mutation batching. begin/end_batch bracket a store
 * rebuild (a directory reload: clear + N appends + 4N cell sets).
 * Between the calls, change fires are HELD (fire_change sets
 * batch_pending and returns); end_batch releases exactly ONE
 * FDK_MODEL_CLEARED — the everything-changed event a bound view
 * answers with ONE full replay instead of N. Proxies forward the
 * discipline: batching the STORE holds the whole chain's chatter,
 * and the single CLEARED propagates through the faces at the end.
 * Nesting counts depth (only the outer end releases). A proxy face
 * passed to begin_batch batches ITS owner's child store — the
 * honest reading of "batch this model". */
void fdk_model_begin_batch(fdk_model *model) {
    if (model == NULL) {
        return;
    }
    if (is_proxy_face(model)) {
        fdk_model_begin_batch(proxy_owner_of(model)->child);
        return;
    }
    model->batch_depth++;
}

void fdk_model_end_batch(fdk_model *model) {
    if (model == NULL) {
        return;
    }
    if (is_proxy_face(model)) {
        fdk_model_end_batch(proxy_owner_of(model)->child);
        return;
    }
    if (model->batch_depth == 0) {
        return; /* unbalanced end: the quiet-setter style */
    }
    model->batch_depth--;
    if (model->batch_depth > 0) {
        return; /* the outermost end settles */
    }
    if (!model->batch_pending) {
        return; /* nothing was held */
    }
    model->batch_pending = false;
    /* The single release rides the NORMAL fire path: the store's
     * listeners include each attached proxy's child_changed, which
     * rebuilds the permutation and refires on the face — so the
     * whole chain sees exactly one CLEARED in its own
     * coordinates. */
    fire_change(model, FDK_MODEL_CLEARED, FDK_MODEL_ROW_NONE, 0);
}

/* ---- cells --------------------------------------------------------------------- */

static m_row *cell_row(fdk_model *m, fdk_model_row row) {
    if (m == NULL) {
        return NULL;
    }
    if (is_proxy_face(m)) {
        fdk_model_proxy *p = proxy_owner_of(m);
        if (row.parent != FDK_MODEL_ROW_NONE ||
            row.index >= p->n_perm) {
            return NULL;
        }
        fdk_model_row child;
        if (!fdk_ok(fdk_model_row_resolve(p->child,
                                          p->perm[row.index],
                                          &child))) {
            return NULL;
        }
        /* §28/G-9c: the child may itself be a proxy's face (a
         * filter over a sort) — recurse; row_find reads the raw
         * array, which a face does not have. */
        if (is_proxy_face(p->child)) {
            return cell_row(p->child, child);
        }
        long ai = row_find(p->child, child.parent, child.index);
        return ai < 0 ? NULL : &p->child->rows[ai];
    }
    long ai = row_find(m, row.parent, row.index);
    return ai < 0 ? NULL : &m->rows[ai];
}

/* A face's set paths must also WRITE through to the child (the
 * binding's reorder writes text cells through the face): cell_row
 * already resolves; the COLUMN type check reads the CHILD's
 * schema. The store set/get functions check model->types — on a
 * face that is the marker array. Give the set/get paths the REAL
 * schema through this helper. */
static fdk_model *schema_of(fdk_model *m) {
    /* §28/G-9c: walk to the STORE at the chain's bottom — a face's
     * child may itself be a face (filter over sort), and only the
     * store carries the real column schema. */
    while (m != NULL && is_proxy_face(m)) {
        m = proxy_owner_of(m)->child;
    }
    return m;
}

fdk_result fdk_model_set_text(fdk_model *model, fdk_model_row row,
                              size_t column, const char *text) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_TEXT) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = NULL;
    if (text != NULL) {
        copy = fdk_alloc(strlen(text) + 1);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        strcpy(copy, text);
    }
    if (r->cells[column].v.text != NULL) {
        fdk_free(r->cells[column].v.text);
    }
    r->cells[column].type = FDK_MODEL_TEXT;
    r->cells[column].v.text = copy;
    fire_change(model, FDK_MODEL_ROW_CHANGED, row.parent, row.index);
    return FDK_OK;
}

fdk_result fdk_model_set_int(fdk_model *model, fdk_model_row row,
                             size_t column, fdk_i64 value) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_INT) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    r->cells[column].type = FDK_MODEL_INT;
    r->cells[column].v.i = value;
    fire_change(model, FDK_MODEL_ROW_CHANGED, row.parent, row.index);
    return FDK_OK;
}

fdk_result fdk_model_set_float(fdk_model *model, fdk_model_row row,
                               size_t column, fdk_f64 value) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_FLOAT) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    r->cells[column].type = FDK_MODEL_FLOAT;
    r->cells[column].v.f = value;
    fire_change(model, FDK_MODEL_ROW_CHANGED, row.parent, row.index);
    return FDK_OK;
}

fdk_result fdk_model_set_ptr(fdk_model *model, fdk_model_row row,
                             size_t column, void *value) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_PTR) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    r->cells[column].type = FDK_MODEL_PTR;
    r->cells[column].v.ptr = value;
    fire_change(model, FDK_MODEL_ROW_CHANGED, row.parent, row.index);
    return FDK_OK;
}

fdk_result fdk_model_get(fdk_model *model, fdk_model_row row,
                         size_t column, fdk_model_value *out) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    out->type = schema_of(model)->types[column];
    switch (out->type) {
    case FDK_MODEL_TEXT:
        if (r->cells[column].v.text != NULL) {
            out->v.text = fdk_alloc(
                strlen(r->cells[column].v.text) + 1);
            if (out->v.text == NULL) {
                return FDK_ERR_OUT_OF_MEMORY;
            }
            strcpy(out->v.text, r->cells[column].v.text);
        } else {
            out->v.text = NULL;
        }
        break;
    case FDK_MODEL_INT:
        out->v.i = r->cells[column].v.i;
        break;
    case FDK_MODEL_FLOAT:
        out->v.f = r->cells[column].v.f;
        break;
    case FDK_MODEL_PTR:
        out->v.ptr = r->cells[column].v.ptr;
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return FDK_OK;
}

void fdk_model_value_free(fdk_model_value *value) {
    if (value != NULL && value->type == FDK_MODEL_TEXT &&
        value->v.text != NULL) {
        fdk_free(value->v.text);
    }
    if (value != NULL) {
        value->v.text = NULL;
    }
}

bool fdk_model_get_int(fdk_model *model, fdk_model_row row,
                       size_t column, fdk_i64 *out) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_INT ||
        out == NULL) {
        return false;
    }
    *out = r->cells[column].v.i;
    return true;
}

bool fdk_model_get_float(fdk_model *model, fdk_model_row row,
                         size_t column, fdk_f64 *out) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_FLOAT ||
        out == NULL) {
        return false;
    }
    *out = r->cells[column].v.f;
    return true;
}

bool fdk_model_get_ptr(fdk_model *model, fdk_model_row row,
                       size_t column, void **out) {
    m_row *r = cell_row(model, row);
    if (r == NULL || column >= schema_of(model)->columns ||
        schema_of(model)->types[column] != FDK_MODEL_PTR ||
        out == NULL) {
        return false;
    }
    *out = r->cells[column].v.ptr;
    return true;
}

/* ---- the proxies ---------------------------------------------------------------- */


struct fdk_model_filter {
    fdk_model_proxy p;
    fdk_model_filter_fn fn;
    void *fn_user;
};

/* Rebuilds a proxy's permutation from the child's current top
 * level + the proxy's own rule. */
static void proxy_rebuild(fdk_model_proxy *p);

/* §28/G-9c: a proxy FACE's full signal path — the classic slot,
 * then every listener. The face is not a store, so it has no
 * fire_change of its own; the proxies' three refire points
 * (child_changed, sort_set_key, filter_set_predicate) all go
 * through here so a face can sit at any depth of a chain: an
 * upstream proxy subscribed to it as its child hears the refire
 * like any listener. */
static void fire_face(fdk_model *face, fdk_model_change change,
                      fdk_model_row row) {
    if (face->on_change != NULL) {
        face->on_change(face, change, row, face->change_user);
    }
    for (size_t i = 0; i < face->n_listeners; i++) {
        if (face->listeners[i].fn != NULL) {
            face->listeners[i].fn(face, change, row,
                                  face->listeners[i].user);
        }
    }
}

/* The child's change signal handler (shared): rebuild + refire. */
static void child_changed(fdk_model *child, fdk_model_change change,
                          fdk_model_row row, void *user) {
    fdk_model_proxy *p = user;
    (void)child;
    proxy_rebuild(p);
    /* Refire on the FACE (the view bound to the proxy sees the
     * proxy's coordinates). §28/G-9: fan out through the face's
     * FULL signal path — the classic slot, then every listener —
     * so a second bound pane AND an upstream proxy (a filter whose
     * child is this face) all hear it. */
    fire_face(&p->face, change, row);
}

static void proxy_init(fdk_model_proxy *p, fdk_model *child) {
    memset(p, 0, sizeof *p);
    p->child = child;
    fdk__model_add_listener(child, child_changed, p);
}

static void proxy_perm_push(fdk_model_proxy *p, fdk_u64 id) {
    if (p->n_perm >= p->cap_perm) {
        size_t cap = p->cap_perm ? p->cap_perm * 2 : 16;
        fdk_u64 *grown = fdk_realloc(p->perm, cap * sizeof *grown);
        if (grown == NULL) {
            return;
        }
        p->perm = grown;
        p->cap_perm = cap;
    }
    p->perm[p->n_perm++] = id;
}

/* §28/G-9c: compares two child ids by `column', typed by the
 * schema, through the PUBLIC get (works when the child is a proxy
 * face — its rows map through its permutation). Absent ids/NULL
 * texts compare equal (stable). */
static int proxy_cmp_keys(fdk_model *child, fdk_u64 a_id,
                          fdk_u64 b_id, size_t column) {
    fdk_model_row ra, rb;
    if (!fdk_ok(fdk_model_row_resolve(child, a_id, &ra)) ||
        !fdk_ok(fdk_model_row_resolve(child, b_id, &rb))) {
        return 0;
    }
    fdk_model_value va, vb;
    int cmp = 0;
    if (fdk_ok(fdk_model_get(child, ra, column, &va)) &&
        fdk_ok(fdk_model_get(child, rb, column, &vb))) {
        if (va.type == FDK_MODEL_INT) {
            cmp = va.v.i < vb.v.i ? -1 : va.v.i > vb.v.i ? 1 : 0;
        } else if (va.type == FDK_MODEL_FLOAT) {
            cmp = va.v.f < vb.v.f ? -1 : va.v.f > vb.v.f ? 1 : 0;
        } else if (va.type == FDK_MODEL_TEXT) {
            const char *x = va.v.text;
            const char *y = vb.v.text;
            cmp = (x == NULL || y == NULL) ? 0 : strcmp(x, y);
        }
    }
    fdk_model_value_free(&va);
    fdk_model_value_free(&vb);
    return cmp;
}

static void proxy_rebuild(fdk_model_proxy *p) {
    p->n_perm = 0;
    /* Collect the child's top-level ids. §28/G-9c: through the
     * PUBLIC read API, not the child's row array — the child may
     * itself be a proxy's FACE (fdk-files' sort sits on the filter's
     * face; example 15 chains the other way), and a face's rows[]
     * is empty (its rows live in the permutation). Reading the
     * array directly made any proxy-on-face chain see 0 rows —
     * the whole chain went blank, single-slot bind or not. */
    size_t n = fdk_model_row_child_count(p->child,
                                         FDK_MODEL_ROW_NONE);
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r = fdk_model_row_child(p->child,
                                              FDK_MODEL_ROW_NONE, i);
        fdk_u64 id = fdk_model_row_id(p->child, r);
        if (id != FDK_MODEL_ROW_NONE) {
            proxy_perm_push(p, id);
        }
    }
    /* The sort's rule. The face marker separates the proxies:
     * columns == 0 marks a SORT face, 1 a FILTER face (set at
     * creation) — the two structs differ in size, so the sort
     * fields may ONLY be read through a sort owner. */
    fdk_model_sort *s = (fdk_model_sort *)p;
    bool is_sort_face = p->face.proxy_owner != NULL &&
                        p->face.columns == 0;
    if (is_sort_face && s->fn == NULL &&
        s->key_column != (size_t)-1) {
        /* column sort with the stable tiebreak: an insertion sort
         * over the permutation (N is widget-scale). Keys are read
         * through fdk_model_get (§28/G-9c): the child may itself be
         * a proxy's FACE, whose cells must map through its own
         * permutation — the raw row array read made any
         * proxy-on-face chain sort on garbage. */
        for (size_t i = 1; i < p->n_perm; i++) {
            fdk_u64 id = p->perm[i];
            size_t j = i;
            while (j > 0) {
                int cmp = proxy_cmp_keys(p->child, id,
                                         p->perm[j - 1],
                                         s->key_column);
                if (s->descending) {
                    cmp = -cmp;
                }
                if (cmp < 0) {
                    p->perm[j] = p->perm[j - 1];
                    j--;
                } else {
                    break;
                }
            }
            p->perm[j] = id;
        }
    } else if (is_sort_face && s->fn != NULL) {
        for (size_t i = 1; i < p->n_perm; i++) {
            fdk_u64 id = p->perm[i];
            size_t j = i;
            while (j > 0 &&
                   s->fn(p->child, id, p->perm[j - 1], s->fn_user) <
                       0) {
                p->perm[j] = p->perm[j - 1];
                j--;
            }
            p->perm[j] = id;
        }
    }

    /* §28/G-9c: the FILTER's rule re-applies here too. It used to
     * run only in set_predicate, so a store mutation arriving
     * through child_changed rebuilt the permutation UNFILTERED —
     * a bound pane showed rows the predicate had hidden. */
    bool is_filter_face = p->face.proxy_owner != NULL &&
                          p->face.columns == 1;
    if (is_filter_face) {
        fdk_model_filter *fl = (fdk_model_filter *)p;
        if (fl->fn != NULL) {
            size_t w = 0;
            for (size_t i = 0; i < p->n_perm; i++) {
                if (fl->fn(p->child, p->perm[i], fl->fn_user)) {
                    p->perm[w++] = p->perm[i];
                }
            }
            p->n_perm = w;
        }
    }
}

/* face implementation: the proxy's rows are the permutation's. */

static fdk_model_row face_row_of_perm(fdk_model_proxy *p, size_t i) {
    (void)p; /* the proxy identity is the index alone            */
    fdk_model_row r = {FDK_MODEL_ROW_NONE, i};
    return r;
}

/* The proxies' model-face overrides are implemented through the
 * SAME fdk_model functions: the face carries proxy_owner, and the
 * store functions check it first. To keep this file small the
 * faces dispatch through the functions below. */

static fdk_model *face_borrow(fdk_model *face) {
    return face; /* the face IS an fdk_model (first member) */
}

/* face row -> child row */
static bool proxy_map(fdk_model_proxy *p, fdk_model_row face_row,
                      fdk_model_row *out_child) {
    if (face_row.parent != FDK_MODEL_ROW_NONE ||
        face_row.index >= p->n_perm) {
        return false;
    }
    fdk_u64 id = p->perm[face_row.index];
    return fdk_ok(fdk_model_row_resolve(p->child, id, out_child));
}

/* ---- the sort proxy ---------------------------------------------------------- */

fdk_model_sort *fdk_model_sort_new(fdk_model *child) {
    if (child == NULL) {
        return NULL;
    }
    fdk_model_sort *s = fdk_alloc(sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    proxy_init(&s->p, child);
    s->key_column = (size_t)-1;
    s->p.face.proxy_owner = s;
    /* The sort flag: mark through the face's columns field (the
     * face is never used as a real store; columns == 0 marks a
     * proxy face for the dispatchers below). */
    s->p.face.columns = 0;
    proxy_rebuild(&s->p);
    return s;
}

void fdk_model_sort_destroy(fdk_model_sort *sort) {
    if (sort == NULL) {
        return;
    }
    fdk__model_remove_listener(sort->p.child, child_changed, sort);
    fdk_free(sort->p.face.listeners);
    fdk_free(sort->p.perm);
    fdk_free(sort);
}

void fdk_model_sort_set_key(fdk_model_sort *sort, size_t column,
                            bool descending, fdk_model_sort_fn fn,
                            void *user) {
    if (sort == NULL) {
        return;
    }
    sort->key_column = column;
    sort->descending = descending;
    sort->fn = fn;
    sort->fn_user = user;
    proxy_rebuild(&sort->p);
    fire_face(&sort->p.face, FDK_MODEL_ROW_MOVED,
              (fdk_model_row){FDK_MODEL_ROW_NONE, 0});
}

fdk_result fdk_model_sort_map_to_child(fdk_model_sort *sort,
                                       fdk_model_row proxy_row,
                                       fdk_model_row *out_child) {
    if (sort == NULL || out_child == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return proxy_map(&sort->p, proxy_row, out_child)
               ? FDK_OK
               : FDK_ERR_NOT_FOUND;
}

fdk_result fdk_model_sort_map_from_child(fdk_model_sort *sort,
                                         fdk_model_row child_row,
                                         fdk_model_row *out_proxy) {
    if (sort == NULL || out_proxy == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_u64 id = fdk_model_row_id(sort->p.child, child_row);
    for (size_t i = 0; i < sort->p.n_perm; i++) {
        if (sort->p.perm[i] == id) {
            *out_proxy = face_row_of_perm(&sort->p, i);
            return FDK_OK;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

fdk_model *fdk_model_sort_model(fdk_model_sort *sort) {
    return sort != NULL ? face_borrow(&sort->p.face) : NULL;
}

/* ---- the filter proxy ----------------------------------------------------------- */

fdk_model_filter *fdk_model_filter_new(fdk_model *child) {
    if (child == NULL) {
        return NULL;
    }
    fdk_model_filter *f = fdk_alloc(sizeof *f);
    if (f == NULL) {
        return NULL;
    }
    proxy_init(&f->p, child);
    f->p.face.proxy_owner = f;
    f->p.face.columns = 1; /* the filter marker (nonzero, distinct
                            * from the sort's 0) */
    proxy_rebuild(&f->p);
    return f;
}

void fdk_model_filter_destroy(fdk_model_filter *filter) {
    if (filter == NULL) {
        return;
    }
    fdk__model_remove_listener(filter->p.child, child_changed, filter);
    fdk_free(filter->p.face.listeners);
    fdk_free(filter->p.perm);
    fdk_free(filter);
}

void fdk_model_filter_set_predicate(fdk_model_filter *filter,
                                    fdk_model_filter_fn fn,
                                    void *user) {
    if (filter == NULL) {
        return;
    }
    filter->fn = fn;
    filter->fn_user = user;
    /* Rebuild with the predicate applied. §28/G-9c: the walk is
     * through the child's PUBLIC row API (the child may be a
     * proxy's face — its rows live in the permutation, not the
     * array). */
    filter->p.n_perm = 0;
    size_t n = fdk_model_row_child_count(filter->p.child,
                                         FDK_MODEL_ROW_NONE);
    for (size_t i = 0; i < n; i++) {
        fdk_model_row r = fdk_model_row_child(filter->p.child,
                                              FDK_MODEL_ROW_NONE,
                                              i);
        fdk_u64 id = fdk_model_row_id(filter->p.child, r);
        if (id == FDK_MODEL_ROW_NONE) {
            continue;
        }
        if (fn == NULL ||
            fn(filter->p.child, id, user)) {
            proxy_perm_push(&filter->p, id);
        }
    }
    fire_face(&filter->p.face, FDK_MODEL_ROW_MOVED,
              (fdk_model_row){FDK_MODEL_ROW_NONE, 0});
}

fdk_result fdk_model_filter_map_to_child(fdk_model_filter *filter,
                                         fdk_model_row proxy_row,
                                         fdk_model_row *out_child) {
    if (filter == NULL || out_child == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return proxy_map(&filter->p, proxy_row, out_child)
               ? FDK_OK
               : FDK_ERR_NOT_FOUND;
}

fdk_result fdk_model_filter_map_from_child(fdk_model_filter *filter,
                                           fdk_model_row child_row,
                                           fdk_model_row *out_proxy) {
    if (filter == NULL || out_proxy == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_u64 id = fdk_model_row_id(filter->p.child, child_row);
    for (size_t i = 0; i < filter->p.n_perm; i++) {
        if (filter->p.perm[i] == id) {
            *out_proxy = face_row_of_perm(&filter->p, i);
            return FDK_OK;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

fdk_model *fdk_model_filter_model(fdk_model_filter *filter) {
    return filter != NULL ? face_borrow(&filter->p.face) : NULL;
}
