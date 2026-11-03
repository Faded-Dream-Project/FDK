/* model.c — see model.h. §29: the listing model is PER-PANE (the
 * §28 rig exposed the singleton's fatal flaw: one store serving
 * every pane means a navigation in pane A erases pane B's rows —
 * the split view could never show two directories at once, and the
 * drag-between-panes leg was testing an impossibility). */
#include "model.h"
#include "fsops.h"
#include "mime.h"
#include "panes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the per-pane chains (index by slot; panes.c registers/unregisters
 * through model_pane_init / model_pane_shutdown) */
static listing_model g_panes[FILES_MAX_PANES];
static struct pane *g_pane_keys[FILES_MAX_PANES];

static int tolower_f(int c);

listing_model *model_pane_of(const struct pane *p) {
    for (int i = 0; i < FILES_MAX_PANES; i++) {
        if (g_pane_keys[i] == p) {
            return &g_panes[i];
        }
    }
    return NULL;
}

listing_model *model_active(void) {
    extern files_app fapp;
    struct pane *p = (fapp.active_pane >= 0 &&
                      fapp.active_pane < fapp.pane_count)
                         ? fapp.panes[fapp.active_pane]
                         : NULL;
    return model_pane_of(p);
}

/* the filter's predicate runs against the chain's OWN store: the
 * proxy hands us the model the row came through, and the chain's
 * bottom (the store) is where the PTR column lives. */
static bool filter_pred(fdk_model *m, fdk_u64 id, void *user) {
    (void)user;
    fdk_model *store = fdk_model_store_of(m);
    const file_info *fi = fmodel_info_in(store, id);
    if (fi == NULL) {
        return false;
    }
    /* the settings live on the chain that owns this filter: find the
     * listing_model whose store this is (a linear scan over <= 4) */
    listing_model *lm = NULL;
    for (int i = 0; i < FILES_MAX_PANES && lm == NULL; i++) {
        if (g_pane_keys[i] != NULL && g_panes[i].store == store) {
            lm = &g_panes[i];
        }
    }
    if (lm == NULL) {
        lm = model_active();
    }
    if (lm == NULL) {
        return true;
    }
    if (!lm->include_hidden && fi->is_hidden) {
        return false;
    }
    if (lm->filter_text[0] != '\0') {
        /* case-insensitive ASCII substring, the honest scope (the
         * example's rule; UTF-8 case folding is a text-layer ask) */
        const char *hay = fi->name;
        const char *needle = lm->filter_text;
        size_t nl = strlen(needle);
        for (const char *scan = hay; *scan != '\0'; scan++) {
            size_t i = 0;
            while (i < nl && scan[i] != '\0' &&
                   tolower_f((unsigned char)scan[i]) ==
                       tolower_f((unsigned char)needle[i])) {
                i++;
            }
            if (i == nl) {
                return true;
            }
        }
        return false;
    }
    return true;
}

static int tolower_f(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* the id -> payload lookup against ONE store (NULL when stale). */
const file_info *fmodel_info_in(fdk_model *store, fdk_u64 id) {
    if (store == NULL) {
        return NULL;
    }
    /* ids are the model's stable counter (not array indices), so
     * resolve id -> row on the STORE and read the PTR column — the
     * one true route the model API offers. */
    fdk_model_row row;
    if (!fdk_ok(fdk_model_row_resolve(store, id, &row))) {
        return NULL;
    }
    void *ptr = NULL;
    if (!fdk_model_get_ptr(store, row, FM_COL_PTR, &ptr)) {
        return NULL;
    }
    return ptr;
}

const file_info *fmodel_info_of(fdk_u64 id) {
    /* the ACTIVE pane's store (the menus' context, the app's
     * status bar) */
    listing_model *lm = model_active();
    return (lm != NULL) ? fmodel_info_in(lm->store, id) : NULL;
}

static void infos_clear(listing_model *lm) {
    for (size_t i = 0; i < lm->info_count; i++) {
        free(lm->infos[i]);
        lm->infos[i] = NULL;
    }
    lm->info_count = 0;
}

void fmodel_load(struct pane *p, const char *dir,
                 bool include_hidden) {
    listing_model *lm = model_pane_of(p);
    if (lm == NULL) {
        return;
    }
    lm->include_hidden = include_hidden;
    lm->filter_text[0] = '\0'; /* a navigation clears the filter */

    size_t n = 0;
    file_info *raw = fs_list_dir(dir, include_hidden, &n);
    if (raw == NULL && n == 0) {
        /* unreadable or empty: an empty store either way */
        fdk_model_clear(lm->store);
        infos_clear(lm);
        return;
    }

    /* Round 12: ONE held batch around the whole rebuild — every
     * append/cell-set fire below is coalesced into a single
     * CLEARED the bound view answers with one replay (the
     * navigation-storm's O(N²) lived exactly here). */
    fdk_model_begin_batch(lm->store);
    fdk_model_clear(lm->store);
    infos_clear(lm);

    for (size_t i = 0; i < n; i++) {
        if (i == lm->info_cap) {
            lm->info_cap = lm->info_cap ? lm->info_cap * 2 : 256;
            file_info **g = realloc(lm->infos,
                                    lm->info_cap * sizeof *lm->infos);
            if (g == NULL) {
                break;
            }
            lm->infos = g;
        }
        file_info *fi = malloc(sizeof *fi);
        if (fi == NULL) {
            break;
        }
        *fi = raw[i];
        lm->infos[i] = fi;

        fdk_model_row row =
            fdk_model_row_append(lm->store, FDK_MODEL_ROW_NONE);
        (void)fdk_model_set_text(lm->store, row, FM_COL_NAME,
                                 fi->name);
        (void)fdk_model_set_int(lm->store, row, FM_COL_SIZE,
                                (fdk_i64)fi->size);
        (void)fdk_model_set_int(lm->store, row, FM_COL_MTIME,
                                (fdk_i64)fi->mtime);
        char cat[32];
        snprintf(cat, sizeof cat, "%s",
                 fi->is_dir ? "Folder" : mime_category(fi->mime));
        (void)fdk_model_set_text(lm->store, row, FM_COL_TYPE, cat);
        (void)fdk_model_set_ptr(lm->store, row, FM_COL_PTR, fi);
    }
    lm->info_count = n;
    free(raw);
    fdk_model_end_batch(lm->store);
}

void fmodel_set_sort(int col, bool desc) {
    listing_model *lm = model_active();
    if (lm == NULL) {
        return;
    }
    lm->sort_col = col;
    lm->sort_desc = desc;
    fdk_model_sort_set_key(lm->sort, (size_t)col, desc, NULL, NULL);
}

void fmodel_set_filter(const char *text) {
    listing_model *lm = model_active();
    if (lm == NULL) {
        return;
    }
    snprintf(lm->filter_text, sizeof lm->filter_text, "%s",
             text != NULL ? text : "");
    fdk_model_filter_set_predicate(lm->filter, filter_pred, NULL);
}

void fmodel_set_hidden(bool include) {
    listing_model *lm = model_active();
    if (lm == NULL) {
        return;
    }
    lm->include_hidden = include;
    fdk_model_filter_set_predicate(lm->filter, filter_pred, NULL);
}

void model_pane_init(struct pane *p) {
    static const fdk_model_value_type cols[FM_COL_COUNT] = {
        FDK_MODEL_TEXT, FDK_MODEL_INT, FDK_MODEL_INT, FDK_MODEL_TEXT,
        FDK_MODEL_PTR,
    };
    int slot = -1;
    for (int i = 0; i < FILES_MAX_PANES; i++) {
        if (g_pane_keys[i] == p) {
            slot = i; /* re-init of a known pane                */
            break;
        }
        if (slot < 0 && g_pane_keys[i] == NULL) {
            slot = i; /* first free slot                        */
        }
    }
    if (slot < 0) {
        return; /* registry full (2 panes is the app's ceiling) */
    }
    listing_model *lm = &g_panes[slot];
    memset(lm, 0, sizeof *lm);
    lm->store = fdk_model_create(cols, FM_COL_COUNT);
    lm->filter = fdk_model_filter_new(lm->store);
    lm->sort =
        fdk_model_sort_new(fdk_model_filter_model(lm->filter));
    lm->view = fdk_model_sort_model(lm->sort);
    lm->include_hidden = false;
    lm->sort_col = FM_COL_NAME;
    lm->sort_desc = false;
    fdk_model_sort_set_key(lm->sort, FM_COL_NAME, false, NULL,
                           NULL);
    fdk_model_filter_set_predicate(lm->filter, filter_pred, NULL);
    g_pane_keys[slot] = p;
}

void model_pane_shutdown(struct pane *p) {
    for (int i = 0; i < FILES_MAX_PANES; i++) {
        if (g_pane_keys[i] != p) {
            continue;
        }
        listing_model *lm = &g_panes[i];
        fdk_model_sort_destroy(lm->sort);
        fdk_model_filter_destroy(lm->filter);
        infos_clear(lm);
        free(lm->infos);
        lm->infos = NULL;
        lm->info_cap = 0;
        fdk_model_destroy(lm->store);
        memset(lm, 0, sizeof *lm);
        g_pane_keys[i] = NULL;
        return;
    }
}

size_t fmodel_selected(struct pane *p, const file_info **out,
                       size_t cap) {
    tab *t = (p != NULL) ? active_tab(p) : NULL;
    if (t == NULL || t->list == NULL) {
        return 0;
    }
    fdk_u64 ids[512];
    size_t n = fdk_list_bound_selected_ids(t->list, ids, 512);
    listing_model *lm = model_pane_of(p);
    if (lm == NULL) {
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < n && w < cap; i++) {
        const file_info *fi = fmodel_info_in(lm->store, ids[i]);
        if (fi != NULL) {
            out[w++] = fi;
        }
    }
    return w;
}
