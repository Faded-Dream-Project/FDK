/* model.h — the listing store: fdk_model + sort/filter proxies. */
#ifndef FILES_MODEL_H
#define FILES_MODEL_H

#include "files.h"

#include "fdk/fdk_model.h"

/* The columns of the listing model. */
enum {
    FM_COL_NAME = 0, /* TEXT: display name                      */
    FM_COL_SIZE,     /* INT: st_size (dirs: 0)                  */
    FM_COL_MTIME,    /* INT: st_mtime                           */
    FM_COL_TYPE,     /* TEXT: the category ("Folder", "Image")  */
    FM_COL_PTR,      /* PTR: the file_info* (app-owned)         */
    FM_COL_COUNT,
};

/* The per-app singleton chain: store -> filter -> sort -> view. */
typedef struct listing_model {
    fdk_model *store;
    fdk_model_filter *filter;
    fdk_model_sort *sort;
    fdk_model *view; /* sort's face                              */

    file_info **infos;     /* the row payloads (row i owns
                            * infos[i], each heap-allocated)      */
    size_t info_count;
    size_t info_cap;

    int sort_col;          /* FM_COL_*                              */
    bool sort_desc;
    char filter_text[128]; /* live name filter                      */
    bool include_hidden;
} listing_model;

/* One global model serves every pane/tab (a navigation repopulates
 * it; the panes each bind their own list to the same view). */
extern listing_model fmodel;

/* Reloads `dir' into the store (clear + fill in one batch; rows
 * carry infos[i]). Returns the entry count; -1 when unreadable. */
long fmodel_load(const char *dir, bool include_hidden);

/* The row id -> payload lookup (NULL when the id is stale). */
const file_info *fmodel_info_of(fdk_u64 id);

/* Sort/filter setters (each re-applies the proxy). */
void fmodel_set_sort(int col, bool desc);
void fmodel_set_filter(const char *text);
void fmodel_set_hidden(bool include);

/* The selection's payloads (up to cap; returns the count). */
size_t fmodel_selected(struct pane *p, const file_info **out,
                       size_t cap);

#endif /* FILES_MODEL_H */
