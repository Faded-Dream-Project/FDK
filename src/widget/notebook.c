#define FDK_LOG_TAG "widgets"

/*
 * notebook.c — Notebook / TabView (Phase 9)
 *
 * A tab strip (labels only — close buttons and reorder are parked)
 * over a page area. Pages are ordinary widgets the notebook adopts
 * (append_page reparents them in, like ScrollView's content);
 * exactly one page is visible at a time — the notebook sets the
 * others invisible, which makes them input-transparent and skips
 * their paint (the Phase 4 visible-flag semantics doing exactly what
 * a page switcher needs).
 *
 * Tab clicks switch pages; the switch callback fires after the
 * switch settles. Keyboard: the notebook is not focusable (the
 * pages' own focusables take keys); Ctrl+Tab-style cycling is
 * parked with the window-level key bindings it belongs to.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define NB_TAB_PAD_X 14
#define NB_TAB_PAD_Y 7
#define NB_TAB_GAP 2
#define NB_TAB_H 30
#define NB_MIN_W 60
#define NB_MIN_H 60

typedef struct fdk_notebook_page {
    fdk_widget *widget;   /* owned via the tree */
    char *label;          /* owned */
} fdk_notebook_page;

typedef struct fdk_notebook {
    fdk_widget base;
    fdk_font *font;       /* borrowed */
    fdk_notebook_page *pages;
    size_t count;
    size_t capacity;
    size_t current;       /* index of the shown page */
    int hover_tab;        /* -1 when none (pages are invisible and
                           * never hover — the notebook tracks the
                           * strip's hover itself from MOTION) */
    fdk_notebook_switch_fn on_switch;
    void *on_switch_data;
} fdk_notebook;

static fdk_notebook *nb_of(fdk_widget *w) {
    return (fdk_notebook *)(void *)w;
}

extern const fdk_widget_class fdk_notebook_class_def;

/* ---- geometry ---- */

static fdk_i32 nb_tab_width(fdk_notebook *nb, size_t i) {
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(nb->font, nb->pages[i].label, &tw, &th);
    return tw + NB_TAB_PAD_X * 2;
}

static void nb_relayout(fdk_notebook *nb) {
    fdk_i32 w = nb->base.bounds.width;
    fdk_i32 h = nb->base.bounds.height;
    if (w <= 0 || h <= 0) {
        return;
    }
    fdk_i32 x = 0;
    for (size_t i = 0; i < nb->count; i++) {
        fdk_rect tr = { x, 0, nb_tab_width(nb, i), NB_TAB_H };
        fdk_widget_set_bounds(nb->pages[i].widget, tr);
        x += tr.width + NB_TAB_GAP;
    }
}

/* The tab rects are the PAGE WIDGETS' own bounds (a page widget IS
 * its tab while inactive; the page CONTENT is drawn by the page's
 * paint hook into the same widget). Simpler and more honest than
 * separate tab widgets: one widget per page, one paint hook that
 * switches on visibility.
 *
 * ...which means a page's paint hook must know whether it is being
 * drawn as a TAB (inactive, small rect at the top) or as a PAGE
 * (active, the area below the strip). That is an awkward contract
 * for applications.
 *
 * INSTEAD: the notebook draws the TABS ITSELF in its paint hook
 * (hit-testing via pointer events against the computed tab rects),
 * and the page widgets are plain application widgets laid out in the
 * page area with visibility switched. The page widgets' bounds are
 * the page area, not the tabs. */

/* Tab index at widget-local (x, y), -1 outside the strip. */
static int nb_tab_at(fdk_notebook *nb, fdk_f32 x, fdk_f32 y) {
    if (y < 0.0f || y >= (fdk_f32)NB_TAB_H) {
        return -1;
    }
    fdk_i32 cx = 0;
    for (size_t i = 0; i < nb->count; i++) {
        fdk_i32 tw = nb_tab_width(nb, i);
        if (x >= (fdk_f32)cx && x < (fdk_f32)(cx + tw)) {
            return (int)i;
        }
        cx += tw + NB_TAB_GAP;
    }
    return -1;
}

static fdk_rect nb_page_area(fdk_notebook *nb) {
    return (fdk_rect){ 0, NB_TAB_H, nb->base.bounds.width,
                       nb->base.bounds.height - NB_TAB_H };
}

static void nb_sync_pages(fdk_notebook *nb) {
    fdk_rect area = nb_page_area(nb);
    for (size_t i = 0; i < nb->count; i++) {
        fdk_widget_set_visible(nb->pages[i].widget, i == nb->current);
        if (i == nb->current) {
            fdk_widget_set_bounds(nb->pages[i].widget, area);
            fdk_widget_child_layout_changed(nb->pages[i].widget);
        }
    }
}

/* ---- paint (tabs) ---- */

static void nb_paint(fdk_widget *w, fdk_surface *surface,
                     fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_notebook *nb = nb_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* Strip background. */
    fdk_rect strip = { bounds.x, bounds.y, bounds.width, NB_TAB_H };
    fdk_surface_fill_rect(surface, strip, fdk__pal_track());

    fdk_i32 x = bounds.x;
    for (size_t i = 0; i < nb->count; i++) {
        fdk_i32 tw = nb_tab_width(nb, i);
        fdk_rect tr = { x, bounds.y, tw, NB_TAB_H };
        fdk_color fill = (i == nb->current)
            ? fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND)
            : ((int)i == nb->hover_tab ? fdk__pal_control_hover()
                                       : fdk__pal_control());
        fdk_surface_fill_rect(surface, tr, fill);
        /* Active tab gets an accent underline on its top edge. */
        if (i == nb->current) {
            fdk_rect bar = { tr.x, tr.y, tr.width, 2 };
            fdk_surface_fill_rect(surface, bar, fdk__pal_accent());
        }
        if (nb->font != NULL) {
            fdk_i32 baseline = fdk__center_baseline(nb->font, tr.y,
                                                    tr.height);
            fdk__draw_text(surface, nb->font, nb->pages[i].label,
                           fdk__pal_text(),
                           tr.x + NB_TAB_PAD_X, baseline);
        }
        x += tw + NB_TAB_GAP;
    }
    /* Page area background (pages draw their own content on top). */
    fdk_rect area = { bounds.x, bounds.y + NB_TAB_H, bounds.width,
                      bounds.height - NB_TAB_H };
    if (area.width > 0 && area.height > 0) {
        fdk_surface_fill_rect(
            surface, area,
            fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));
    }
}

/* ---- events (tab clicks) ---- */

static bool nb_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_notebook *nb = nb_of(w);
    if (ev->type == FDK_WIDGET_POINTER_MOTION) {
        int tab = nb_tab_at(nb, ev->position.x, ev->position.y);
        if (tab != nb->hover_tab) {
            nb->hover_tab = tab;
            fdk_widget_invalidate(w);
        }
        return false; /* motion keeps bubbling */
    }
    if (ev->type != FDK_WIDGET_POINTER_DOWN ||
        ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
        return false;
    }
    int tab = nb_tab_at(nb, ev->pointer.position.x,
                        ev->pointer.position.y);
    if (tab < 0) {
        return false; /* below the strip: the page's territory */
    }
    if ((size_t)tab != nb->current) {
        nb->current = (size_t)tab;
        nb_sync_pages(nb);
        fdk_widget_invalidate(w);
        if (nb->on_switch != NULL) {
            nb->on_switch(w, nb->current, nb->on_switch_data);
        }
    }
    return true;
}

static void nb_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = NB_MIN_W;
    out->height = NB_MIN_H;
}

static void nb_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    nb_relayout(nb_of(w));
    nb_sync_pages(nb_of(w));
}

static void nb_destroy(fdk_widget *w) {
    fdk_notebook *nb = nb_of(w);
    for (size_t i = 0; i < nb->count; i++) {
        fdk_free(nb->pages[i].label);
    }
    fdk_free(nb->pages);
}

const fdk_widget_class fdk_notebook_class_def = {
    .size = sizeof(fdk_notebook),
    .name = "notebook",
    .handle_event = nb_handle_event,
    .paint = nb_paint,
    .measure = nb_measure,
    .arrange = nb_arrange,
    .destroy = nb_destroy,
};

/* ---- public API ---- */

fdk_result fdk_notebook_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_notebook) {
    if (out_notebook == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_notebook_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_notebook *nb = nb_of(w);
    nb->font = font;
    nb->hover_tab = -1;
    fdk_widget_child_layout_changed(w->parent);
    *out_notebook = w;
    return FDK_OK;
}

fdk_result fdk_notebook_append_page(fdk_widget *notebook,
                                    fdk_widget *page,
                                    const char *label) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def ||
        page == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (nb->count == nb->capacity) {
        size_t cap = (nb->capacity == 0) ? 4 : nb->capacity * 2;
        fdk_notebook_page *grown =
            fdk_realloc(nb->pages, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        nb->pages = grown;
        nb->capacity = cap;
    }
    char *copy = fdk__strdup(label != NULL ? label : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_result r = fdk_widget_reparent(page, notebook);
    if (!fdk_ok(r)) {
        fdk_free(copy);
        return r;
    }
    nb->pages[nb->count].widget = page;
    nb->pages[nb->count].label = copy;
    nb->count++;
    if (nb->count == 1) {
        nb->current = 0;
    }
    nb_sync_pages(nb);
    fdk_widget_invalidate(notebook);
    return FDK_OK;
}

size_t fdk_notebook_page_count(fdk_widget *notebook) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return 0;
    }
    return nb_of(notebook)->count;
}

fdk_result fdk_notebook_set_current_page(fdk_widget *notebook,
                                         size_t index) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (index >= nb->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (index != nb->current) {
        nb->current = index;
        nb_sync_pages(nb);
        fdk_widget_invalidate(notebook);
        if (nb->on_switch != NULL) {
            nb->on_switch(notebook, index, nb->on_switch_data);
        }
    }
    return FDK_OK;
}

size_t fdk_notebook_get_current_page(fdk_widget *notebook) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return 0;
    }
    fdk_notebook *nb = nb_of(notebook);
    return (nb->count == 0) ? 0 : nb->current;
}

fdk_widget *fdk_notebook_get_page(fdk_widget *notebook, size_t index) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return NULL;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (index >= nb->count) {
        return NULL;
    }
    return nb->pages[index].widget;
}

void fdk_notebook_set_on_switch(fdk_widget *notebook,
                                fdk_notebook_switch_fn fn,
                                void *user_data) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return;
    }
    fdk_notebook *nb = nb_of(notebook);
    nb->on_switch = fn;
    nb->on_switch_data = user_data;
}
