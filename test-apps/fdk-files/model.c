/* model.c — see model.h. */
#include "model.h"
#include "fsops.h"
#include "mime.h"
#include "panes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

listing_model fmodel;

/* the filter's user data is the singleton itself */

static int tolower_f(int c);

static bool filter_pred(fdk_model *m, fdk_u64 id, void *user) {
    (void)m;
    (void)user;
    const file_info *fi = fmodel_info_of(id);
    if (fi == NULL) {
        return false;
    }
    if (!fmodel.include_hidden && fi->is_hidden) {
        return false;
    }
    if (fmodel.filter_text[0] != '\0') {
        /* case-insensitive ASCII substring, the honest scope (the
         * example's rule; UTF-8 case folding is a text-layer ask) */
        const char *hay = fi->name;
        const char *needle = fmodel.filter_text;
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

const file_info *fmodel_info_of(fdk_u64 id) {
    /* ids are the model's stable counter (not array indices), so
     * resolve id -> row on the STORE and read the PTR column — the
     * one true route the model API offers. */
    fdk_model_row row;
    if (!fdk_ok(fdk_model_row_resolve(fmodel.store, id, &row))) {
        return NULL;
    }
    void *ptr = NULL;
    if (!fdk_model_get_ptr(fmodel.store, row, FM_COL_PTR, &ptr)) {
        return NULL;
    }
    return ptr;
}

static void infos_clear(void) {
    for (size_t i = 0; i < fmodel.info_count; i++) {
        free(fmodel.infos[i]);
        fmodel.infos[i] = NULL;
    }
    fmodel.info_count = 0;
}

long fmodel_load(const char *dir, bool include_hidden) {
    fmodel_set_filter(""); /* a navigation clears the live filter */

    size_t n = 0;
    file_info *raw = fs_list_dir(dir, include_hidden, &n);
    if (raw == NULL && n == 0) {
        /* unreadable or empty: an empty store either way */
        fdk_model_clear(fmodel.store);
        infos_clear();
        return (raw == NULL) ? -1 : 0;
    }

    fdk_model_clear(fmodel.store);
    infos_clear();

    for (size_t i = 0; i < n; i++) {
        if (i == fmodel.info_cap) {
            fmodel.info_cap = fmodel.info_cap ? fmodel.info_cap * 2
                                              : 256;
            file_info **g = realloc(fmodel.infos,
                                    fmodel.info_cap *
                                        sizeof *fmodel.infos);
            if (g == NULL) {
                break;
            }
            fmodel.infos = g;
        }
        file_info *fi = malloc(sizeof *fi);
        if (fi == NULL) {
            break;
        }
        *fi = raw[i];
        fmodel.infos[i] = fi;

        fdk_model_row row =
            fdk_model_row_append(fmodel.store,
                                 FDK_MODEL_ROW_NONE);
        (void)fdk_model_set_text(fmodel.store, row, FM_COL_NAME,
                                 fi->name);
        (void)fdk_model_set_int(fmodel.store, row, FM_COL_SIZE,
                                (fdk_i64)fi->size);
        (void)fdk_model_set_int(fmodel.store, row, FM_COL_MTIME,
                                (fdk_i64)fi->mtime);
        char cat[32];
        snprintf(cat, sizeof cat, "%s",
                 fi->is_dir ? "Folder"
                            : mime_category(fi->mime));
        (void)fdk_model_set_text(fmodel.store, row, FM_COL_TYPE, cat);
        (void)fdk_model_set_ptr(fmodel.store, row, FM_COL_PTR, fi);
    }
    fmodel.info_count = n;
    free(raw);
    return (long)n;
}

void fmodel_set_sort(int col, bool desc) {
    fmodel.sort_col = col;
    fmodel.sort_desc = desc;
    fdk_model_sort_set_key(fmodel.sort, (size_t)col, desc, NULL,
                           NULL);
}

void fmodel_set_filter(const char *text) {
    snprintf(fmodel.filter_text, sizeof fmodel.filter_text, "%s",
             text != NULL ? text : "");
    fdk_model_filter_set_predicate(fmodel.filter, filter_pred,
                                   NULL);
}

void fmodel_set_hidden(bool include) {
    fmodel.include_hidden = include;
    fdk_model_filter_set_predicate(fmodel.filter, filter_pred,
                                   NULL);
}

void model_init(void) {
    static const fdk_model_value_type cols[FM_COL_COUNT] = {
        FDK_MODEL_TEXT, FDK_MODEL_INT, FDK_MODEL_INT, FDK_MODEL_TEXT,
        FDK_MODEL_PTR,
    };
    memset(&fmodel, 0, sizeof fmodel);
    fmodel.store = fdk_model_create(cols, FM_COL_COUNT);
    fmodel.filter = fdk_model_filter_new(fmodel.store);
    fmodel.sort =
        fdk_model_sort_new(fdk_model_filter_model(fmodel.filter));
    fmodel.view = fdk_model_sort_model(fmodel.sort);
    fmodel_set_sort(FM_COL_NAME, false);
}

void model_shutdown(void) {
    fdk_model_sort_destroy(fmodel.sort);
    fdk_model_filter_destroy(fmodel.filter);
    infos_clear();
    free(fmodel.infos);
    fmodel.infos = NULL;
    fmodel.info_cap = 0;
    fdk_model_destroy(fmodel.store);
    memset(&fmodel, 0, sizeof fmodel);
}

size_t fmodel_selected(struct pane *p, const file_info **out,
                       size_t cap) {
    tab *t = (p != NULL) ? active_tab(p) : NULL;
    if (t == NULL || t->list == NULL) {
        return 0;
    }
    fdk_u64 ids[512];
    size_t n = fdk_list_bound_selected_ids(t->list, ids, 512);
    size_t w = 0;
    for (size_t i = 0; i < n && w < cap; i++) {
        const file_info *fi = fmodel_info_of(ids[i]);
        if (fi != NULL) {
            out[w++] = fi;
        }
    }
    return w;
}
