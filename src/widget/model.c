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

struct fdk_model {
    fdk_model_value_type *types;
    size_t columns;
    m_row *rows;        /* flat; order = insertion order within
                         * the parent                                */
    size_t n_rows, cap_rows;
    fdk_u64 next_id;    /* 1, 2, ... (0 = never)                     */

    fdk_model_change_fn on_change;
    void *change_user;

    /* The proxy back-faces: NULL for a plain store. */
    fdk_model *sort_face;
    fdk_model *filter_face;
    void *proxy_owner;
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
    if (m->on_change != NULL) {
        fdk_model_row r = {parent, index};
        m->on_change(m, change, r, m->change_user);
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
    if (m != NULL && is_proxy_face(m)) {
        return proxy_owner_of(m)->child;
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

/* The child's change signal handler (shared): rebuild + refire. */
static void child_changed(fdk_model *child, fdk_model_change change,
                          fdk_model_row row, void *user) {
    fdk_model_proxy *p = user;
    (void)child;
    proxy_rebuild(p);
    /* Refire on the FACE (the view bound to the proxy sees the
     * proxy's coordinates). */
    if (p->face.on_change != NULL) {
        p->face.on_change(&p->face, change, row,
                          p->face.change_user);
    }
}

static void proxy_init(fdk_model_proxy *p, fdk_model *child) {
    memset(p, 0, sizeof *p);
    p->child = child;
    fdk_model_set_on_change(child, child_changed, p);
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

static void proxy_rebuild(fdk_model_proxy *p) {
    p->n_perm = 0;
    /* Collect the child's top-level ids. */
    for (size_t i = 0; i < p->child->n_rows; i++) {
        if (p->child->rows[i].parent == FDK_MODEL_ROW_NONE) {
            proxy_perm_push(p, p->child->rows[i].id);
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
         * over the permutation (N is widget-scale). */
        for (size_t i = 1; i < p->n_perm; i++) {
            fdk_u64 id = p->perm[i];
            size_t j = i;
            while (j > 0) {
                long aj = row_find_id(p->child, p->perm[j - 1]);
                long ai = row_find_id(p->child, id);
                if (aj < 0 || ai < 0) {
                    break;
                }
                m_row *a = &p->child->rows[ai];
                m_row *b = &p->child->rows[aj];
                int cmp = 0;
                fdk_model_value_type t =
                    p->child->types[s->key_column];
                if (t == FDK_MODEL_INT) {
                    cmp = a->cells[s->key_column].v.i <
                                  b->cells[s->key_column].v.i
                              ? -1
                              : a->cells[s->key_column].v.i >
                                        b->cells[s->key_column].v.i
                                    ? 1
                                    : 0;
                } else if (t == FDK_MODEL_FLOAT) {
                    cmp = a->cells[s->key_column].v.f <
                                  b->cells[s->key_column].v.f
                              ? -1
                              : a->cells[s->key_column].v.f >
                                        b->cells[s->key_column].v.f
                                    ? 1
                                    : 0;
                } else if (t == FDK_MODEL_TEXT) {
                    const char *x = a->cells[s->key_column].v.text;
                    const char *y = b->cells[s->key_column].v.text;
                    cmp = (x == NULL || y == NULL)
                              ? 0
                              : strcmp(x, y);
                }
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

    /* The FILTER's rule runs in proxy_rebuild's filter path
     * (set_predicate); the sort rebuild above is the shared
     * change-triggered path. */
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
    fdk_model_set_on_change(sort->p.child, NULL, NULL);
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
    if (sort->p.face.on_change != NULL) {
        sort->p.face.on_change(&sort->p.face, FDK_MODEL_ROW_MOVED,
                               (fdk_model_row){
                                   FDK_MODEL_ROW_NONE, 0},
                               sort->p.face.change_user);
    }
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
    fdk_model_set_on_change(filter->p.child, NULL, NULL);
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
    /* Rebuild with the predicate applied. */
    filter->p.n_perm = 0;
    for (size_t i = 0; i < filter->p.child->n_rows; i++) {
        if (filter->p.child->rows[i].parent != FDK_MODEL_ROW_NONE) {
            continue;
        }
        if (fn == NULL ||
            fn(filter->p.child, filter->p.child->rows[i].id,
               user)) {
            proxy_perm_push(&filter->p,
                            filter->p.child->rows[i].id);
        }
    }
    if (filter->p.face.on_change != NULL) {
        filter->p.face.on_change(&filter->p.face,
                                 FDK_MODEL_ROW_MOVED,
                                 (fdk_model_row){
                                     FDK_MODEL_ROW_NONE, 0},
                                 filter->p.face.change_user);
    }
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
