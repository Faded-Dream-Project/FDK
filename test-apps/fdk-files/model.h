/* model.h — the listing model: fdk_model + sort/filter proxies.
 * §29: PER-PANE (the singleton served every pane one store — a
 * navigation in pane A erased pane B's rows; the split view could
 * never show two directories, and the drag-between-panes leg was
 * testing an impossibility). */
#ifndef FILES_MODEL_H
#define FILES_MODEL_H

#include "files.h"

#include "fdk/fdk_model.h"

/* The columns of the listing model. */
enum {
    FM_COL_NAME = 0, /* TEXT: display name                      */
    FM_COL_SIZE,     /* INT: st_size (dirs: 0)                  */
    FM_COL_MTIME,    /* INT: st_mtime                            */
    FM_COL_TYPE,     /* TEXT: the category ("Folder", "Image")  */
    FM_COL_PTR,      /* PTR: the file_info* (app-owned)         */
    FM_COL_COUNT,
};

/* One chain per pane: store -> filter -> sort -> view. */
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

#define FILES_MAX_PANES 4

struct pane;

/* The per-pane chains (model.c owns them). model_active() is the
 * focused pane's chain — the menus and the status bar read it. */
listing_model *model_pane_of(const struct pane *p);
listing_model *model_active(void);

/* The pane's lifecycle (panes.c calls these at add/remove). */
void model_pane_init(struct pane *p);
void model_pane_shutdown(struct pane *p);

/* Reloads `dir' into PANE'S store (clear + fill in one batch). */
void fmodel_load(struct pane *p, const char *dir,
                 bool include_hidden);

/* the id -> payload lookups (against one store / the active pane) */
const file_info *fmodel_info_in(fdk_model *store, fdk_u64 id);
const file_info *fmodel_info_of(fdk_u64 id);

/* Sort/filter setters (each re-applies the ACTIVE pane's proxy). */
void fmodel_set_sort(int col, bool desc);
void fmodel_set_filter(const char *text);
void fmodel_set_hidden(bool include);

/* The selection's payloads (up to cap; returns the count). */
size_t fmodel_selected(struct pane *p, const file_info **out,
                       size_t cap);

#endif /* FILES_MODEL_H */
