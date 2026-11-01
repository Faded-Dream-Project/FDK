/*
 * fdk_model.h — item models and proxies (the §23/B2e round,
 * 2026-11-01): the model/view split the List/Tree widgets' own
 * row APIs never had.
 *
 * The 1.x List and Tree are WIDGET-OWNED stores: you append rows
 * to the widget, it owns them. That shape is simple and stays —
 * nothing here replaces it. What was missing is the GtkListStore /
 * QAbstractItemModel half: a model OBJECT the application owns,
 * views BIND to, and proxies transform — so a directory listing, a
 * database cursor, or a million-row log can back a view without
 * the data being copied into widget memory, and so sorting and
 * filtering are the model layer's job, not hand-written loops in
 * every app.
 *
 * The shape:
 *
 *   fdk_model            a row store: N rows x M columns of
 *                        (text | i64 | f64 | pointer) cells with
 *                        change signals (row-inserted / removed /
 *                        moved / changed / cleared). One optional
 *                        TREE column: a parent index makes rows a
 *                        forest (the Tree view binds to that).
 *   fdk_model_sort       a proxy: reorders the child model by a
 *                        column/key, ascending or descending, with
 *                        a stable tiebreak; the app can also
 *                        supply its own comparator.
 *   fdk_model_filter     a proxy: hides rows failing a predicate
 *                        (the app's callback), keeps a
 *                        proxy->child mapping.
 *   fdk_list_bind_model  the VIEW half: a List widget mirrors the
 *                        model (or proxy) — the binding is live
 *                        (change signals update the view), the
 *                        selection maps back to model rows, and
 *                        the data is PULLED per visible row (the
 *                        widget never copies the model).
 *   fdk_tree_bind_model  the same for a Tree over the model's
 *                        parent column.
 *
 * Virtualization: the bound views render only their visible band
 * (the List's scrollview already clips; the binding asks the model
 * for exactly the rows in view, so a 1M-row model costs the
 * visible rows' memory — the point of the split).
 *
 * Keyboard navigation and drag-reorder ride the widgets' own
 * machinery (the List's arrow-key navigation and the reorder
 * handles); the binding translates them back into model moves
 * (fdk_list_set_reorderable on a model-bound list reorders the
 * MODEL, not just the view).
 */

#ifndef FDK_MODEL_H
#define FDK_MODEL_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The model ------------------------------------------------------------ */

typedef struct fdk_model fdk_model;

/* A stable row identity: the row's INDEX in its parent's child
 * order, plus the parent (FDK_MODEL_ROW_NONE for top level).
 * Indices are invalidated by mutations — read them, use them,
 * re-query after changes (the signal carries fresh indices). */
#define FDK_MODEL_ROW_NONE ((fdk_u64)0 - 1)

typedef struct fdk_model_row {
    fdk_u64 parent;   /* FDK_MODEL_ROW_NONE at top level          */
    size_t index;     /* within the parent's children             */
} fdk_model_row;

/* The per-cell value: one tagged union. */
typedef enum fdk_model_value_type {
    FDK_MODEL_TEXT = 1,   /* owned UTF-8, copied in and out       */
    FDK_MODEL_INT = 2,    /* i64                                   */
    FDK_MODEL_FLOAT = 3,  /* f64                                   */
    FDK_MODEL_PTR = 4,    /* the app's pointer (never dereferenced
                           * by FDK)                               */
} fdk_model_value_type;

typedef struct fdk_model_value {
    fdk_model_value_type type;
    union {
        char *text;
        fdk_i64 i;
        fdk_f64 f;
        void *ptr;
    } v;
} fdk_model_value;

/* Change signals. `row` is the AFFECTED row in the POST-change
 * coordinate space (for REMOVED, the row that now sits at the
 * removed slot). Fired after the model's state settled. */
typedef enum fdk_model_change {
    FDK_MODEL_ROW_INSERTED = 1,
    FDK_MODEL_ROW_REMOVED = 2,
    FDK_MODEL_ROW_CHANGED = 3,
    FDK_MODEL_ROW_MOVED = 4,
    FDK_MODEL_CLEARED = 5,
} fdk_model_change;

typedef void (*fdk_model_change_fn)(fdk_model *model,
                                    fdk_model_change change,
                                    fdk_model_row row, void *user);

/* Creates a model with `columns' columns, each of `type' (the
 * array is copied; column count is fixed for the model's life —
 * the append-only discipline applies to the API, not per-model
 * schemas). Destroy with fdk_model_destroy. */
fdk_model *fdk_model_create(const fdk_model_value_type *column_types,
                            size_t columns);
void fdk_model_destroy(fdk_model *model);

void fdk_model_set_on_change(fdk_model *model,
                             fdk_model_change_fn fn, void *user);

/* ---- Rows -------------------------------------------------------------------
 * Rows are addressed (parent, index). A tree grows by adding rows
 * with a non-NONE parent. The parent column convention: row R's
 * children are the rows whose parent == R (R addressed by its OWN
 * parent+index at add time; the model stores stable internal ids —
 * fdk_model_row_is_parent tells whether a row has children). */

/* Appends at the parent's end; returns the new row. */
fdk_model_row fdk_model_row_append(fdk_model *model, fdk_u64 parent);
/* Inserts at `index' within the parent (== child count appends). */
fdk_model_row fdk_model_row_insert(fdk_model *model, fdk_u64 parent,
                                   size_t index);
/* Removes the row and its whole subtree. */
fdk_result fdk_model_row_remove(fdk_model *model, fdk_model_row row);
/* Moves `row' (and subtree) to a new parent/index. */
fdk_result fdk_model_row_move(fdk_model *model, fdk_model_row row,
                              fdk_u64 new_parent, size_t new_index);
/* The number of the parent's direct children. */
size_t fdk_model_row_child_count(fdk_model *model, fdk_u64 parent);
/* The parent's `index'-th child's ROW (identity resolution). */
fdk_model_row fdk_model_row_child(fdk_model *model, fdk_u64 parent,
                                  size_t index);
/* The row's stable internal id (outlives index invalidation; the
 * value binds rows across mutations for drag-reorder etc.). */
fdk_u64 fdk_model_row_id(fdk_model *model, fdk_model_row row);
/* Resolves a stable id back to the current (parent, index). */
fdk_result fdk_model_row_resolve(fdk_model *model, fdk_u64 id,
                                 fdk_model_row *out_row);
/* Every row at once (a full clear). */
void fdk_model_clear(fdk_model *model);

/* ---- Cells ------------------------------------------------------------------ */

/* Sets a text cell (copied). */
fdk_result fdk_model_set_text(fdk_model *model, fdk_model_row row,
                              size_t column, const char *text);
/* Sets an int / float / pointer cell. */
fdk_result fdk_model_set_int(fdk_model *model, fdk_model_row row,
                             size_t column, fdk_i64 value);
fdk_result fdk_model_set_float(fdk_model *model, fdk_model_row row,
                               size_t column, fdk_f64 value);
fdk_result fdk_model_set_ptr(fdk_model *model, fdk_model_row row,
                             size_t column, void *value);

/* Reads a cell. TEXT: the fdk_alloc'd copy (release with
 * fdk_model_value_free) or NULL. The typed getters return false
 * on a type mismatch (the column's declared type rules). */
fdk_result fdk_model_get(fdk_model *model, fdk_model_row row,
                         size_t column, fdk_model_value *out);
void fdk_model_value_free(fdk_model_value *value);
bool fdk_model_get_int(fdk_model *model, fdk_model_row row,
                       size_t column, fdk_i64 *out);
bool fdk_model_get_float(fdk_model *model, fdk_model_row row,
                         size_t column, fdk_f64 *out);
bool fdk_model_get_ptr(fdk_model *model, fdk_model_row row,
                       size_t column, void **out);

/* ---- The sort proxy ----------------------------------------------------------- */

typedef struct fdk_model_sort fdk_model_sort;

typedef int (*fdk_model_sort_fn)(fdk_model *model, fdk_u64 a,
                                 fdk_u64 b, void *user);

/* Creates a sorting proxy over `child' (top-level rows only; tree
 * levels keep their parent order — GTK's GtkTreeModelSort shape).
 * The proxy keeps a permutation of the child's top level, updated
 * on every child change. Destroying the child first is a bug; the
 * proxy holds it borrowed. */
fdk_model_sort *fdk_model_sort_new(fdk_model *child);
void fdk_model_sort_destroy(fdk_model_sort *sort);

/* Sorts by a column (the column's type's natural order for
 * INT/FLOAT; TEXT lexicographic) with a stable tiebreak on the row
 * id. NULL fn = the column order. */
void fdk_model_sort_set_key(fdk_model_sort *sort, size_t column,
                            bool descending, fdk_model_sort_fn fn,
                            void *user);

/* Proxy->child and child->proxy row mapping. */
fdk_result fdk_model_sort_map_to_child(fdk_model_sort *sort,
                                       fdk_model_row proxy_row,
                                       fdk_model_row *out_child);
fdk_result fdk_model_sort_map_from_child(fdk_model_sort *sort,
                                         fdk_model_row child_row,
                                         fdk_model_row *out_proxy);

/* The proxy's OWN model face (what a view binds): the same row
 * addressing, its own change signals. */
fdk_model *fdk_model_sort_model(fdk_model_sort *sort);

/* ---- The filter proxy ----------------------------------------------------------- */

typedef struct fdk_model_filter fdk_model_filter;

typedef bool (*fdk_model_filter_fn)(fdk_model *model, fdk_u64 id,
                                    void *user);

/* Creates a filtering proxy over `child' (top level). Rows the
 * predicate rejects are hidden; the mapping is recomputed on
 * every child change or predicate change. NULL fn shows all. */
fdk_model_filter *fdk_model_filter_new(fdk_model *child);
void fdk_model_filter_destroy(fdk_model_filter *filter);

void fdk_model_filter_set_predicate(fdk_model_filter *filter,
                                    fdk_model_filter_fn fn,
                                    void *user);

fdk_result fdk_model_filter_map_to_child(fdk_model_filter *filter,
                                         fdk_model_row proxy_row,
                                         fdk_model_row *out_child);
fdk_result fdk_model_filter_map_from_child(fdk_model_filter *filter,
                                           fdk_model_row child_row,
                                           fdk_model_row *out_proxy);

fdk_model *fdk_model_filter_model(fdk_model_filter *filter);

/* ---- The view bindings (src/widget/model_bind.c) ---------------------------
 * A bound List/Tree MIRRORS the model: row changes apply to the
 * widget live (the binding installs the model's change handler).
 * The List materializes its row widgets as ever (the widget's
 * architecture); the model + proxies are the scalable half. The
 * selection mapping is BY ROW ID: an app holding ids keeps its
 * selection across a re-sort or re-filter. */

/* Binds the List to the model's TOP-LEVEL rows, mirroring
 * `text_column' into each row. Rebinding the same widget replaces
 * the previous binding. (fdk_widget/fdk_tree_node come from
 * fdk_widgets.h — include it before this header.) */
fdk_result fdk_list_bind_model(struct fdk_widget *list, fdk_model *model,
                               size_t text_column);
/* Detaches (the widget keeps its current rows; the model's changes
 * stop applying). */
void fdk_list_unbind_model(struct fdk_widget *list);

/* The model row id behind a view row / the view row behind an id. */
fdk_u64 fdk_list_bound_row_id(struct fdk_widget *list, size_t view_row);
fdk_result fdk_list_bound_index_of(struct fdk_widget *list, fdk_u64 id,
                                   size_t *out_view_row);
/* Every selected row's model id, view order; writes at most cap,
 * returns the selected count. */
size_t fdk_list_bound_selected_ids(struct fdk_widget *list,
                                   fdk_u64 *out_ids, size_t cap);
/* The reorder write-back: moves the selected MODEL rows before
 * `target_id' (NONE = the end). Returns the moved count. */
size_t fdk_list_bound_reorder_selected(struct fdk_widget *list,
                                       fdk_u64 target_id);

/* The TREE binding lives in fdk_widgets.h (it speaks
 * fdk_tree_node): fdk_tree_bind_model / fdk_tree_unbind_model /
 * fdk_tree_bound_node_of. */

/* The underlying STORE behind a model: the model itself when it is
 * a store, the CHILD store when it is a proxy's face. Mutations
 * (row_add/move/remove, cell sets that must signal through the
 * proxy) belong on what this returns... cells may be set through a
 * face (the write lands in the store, the face's listeners hear
 * it); ROW moves on a face are refused — use the store. */
fdk_model *fdk_model_store_of(fdk_model *model);

#ifdef __cplusplus
}
#endif

#endif /* FDK_MODEL_H */
