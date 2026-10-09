#define FDK_LOG_TAG "widgets"

/*
 * notebook.c — Notebook / TabView (Phase 9)
 *
 * A tab strip (labels only — close buttons are parked; drag
 * reordering ships since 1.4.4) over a page area. Pages are ordinary
 * widgets the notebook adopts (append_page reparents them in, like
 * ScrollView's content); exactly one page is visible at a time — the
 * notebook sets the others invisible, which makes them
 * input-transparent and skips their paint (the Phase 4 visible-flag
 * semantics doing exactly what a page switcher needs).
 *
 * Tab clicks switch pages; the switch callback fires after the
 * switch settles. Keyboard: the notebook is not focusable (the
 * pages' own focusables take keys); Ctrl+Tab-style cycling is
 * parked with the window-level key bindings it belongs to.
 *
 * Reorder (1.4.4): a press on a tab arms the gesture (and switches
 * pages, the existing behavior); motion past NB_REORDER_SLOP px
 * makes the tab FOLLOW THE POINTER. Swaps are LIVE on the model —
 * when the dragged tab's center crosses a neighbor slot's midpoint
 * the pages array swaps NOW, and drag_dx is corrected by the
 * neighbor's advance so the tab stays GLUED to the pointer (the
 * strip rolls underneath, the tab never jumps). The shown page
 * never changes during a drag: the current page is tracked by
 * POINTER IDENTITY and re-indexed after every swap. Release settles
 * the tab into its slot (drag_dx returns to 0 — the tab is already
 * there, model-wise) and fires on_page_reordered exactly once when
 * the order actually changed.
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
#define NB_REORDER_SLOP 4   /* px of travel before a press becomes a drag */

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
    /* ---- reorder drag (1.4.4) ---- */
    bool press_armed;     /* a tab press is down; motion may promote */
    bool dragging;        /* the promotion happened */
    size_t drag_index;    /* the dragged page's CURRENT slot */
    size_t drag_from;     /* its slot at press time */
    fdk_f32 press_x;      /* press position (strip-local) */
    fdk_i32 drag_dx;      /* pointer-following offset, swap-corrected */
    bool drag_moved;      /* at least one swap since press */
    fdk_notebook_reorder_fn on_reorder;
    void *on_reorder_data;
} fdk_notebook;

static fdk_notebook *nb_of(fdk_widget *w) {
    return (fdk_notebook *)(void *)w;
}

extern const fdk_widget_class fdk_notebook_class_def;

/* ---- geometry ---- */

static fdk_i32 nb_tab_width(const fdk_notebook *nb, size_t i) {
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(nb->font, nb->pages[i].label, &tw, &th);
    return tw + NB_TAB_PAD_X * 2;
}

/* The tab's slot origin (strip-local x). O(i), same walk the paint
 * and the a11y bounds already do. */
static fdk_i32 nb_tab_x(const fdk_notebook *nb, size_t i) {
    fdk_i32 x = 0;
    for (size_t k = 0; k < i; k++) {
        x += nb_tab_width(nb, k) + NB_TAB_GAP;
    }
    return x;
}

/* The strip's total advance (the clamp range's right wall). */
static fdk_i32 nb_strip_width(const fdk_notebook *nb) {
    if (nb->count == 0) {
        return 0;
    }
    return nb_tab_x(nb, nb->count - 1) + nb_tab_width(nb, nb->count - 1);
}

/* The notebook draws the TABS ITSELF in its paint hook (hit-testing
 * via pointer events against the computed tab rects), and the page
 * widgets are plain application widgets laid out in the page area
 * with visibility switched. The page widgets' bounds are the page
 * area, not the tabs: only the CURRENT page ever gets bounds
 * (nb_sync_pages); inactive pages are invisible and keep whatever
 * bounds they had — they are input-transparent and skipped by the
 * paint walk until they become current. */

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
    fdk_rect drag_rect = {0, 0, 0, 0};
    bool dragging = nb->dragging;
    for (size_t i = 0; i < nb->count; i++) {
        fdk_i32 tw = nb_tab_width(nb, i);
        fdk_rect tr = { x, bounds.y, tw, NB_TAB_H };
        if (dragging && i == nb->drag_index) {
            /* The dragged tab paints LAST (top of the strip): stash
             * its rect at the pointer offset, skip here. */
            drag_rect = tr;
            drag_rect.x += nb->drag_dx;
            x += tw + NB_TAB_GAP;
            continue;
        }
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
    if (dragging && drag_rect.width > 0) {
        /* The dragged tab: pressed fill, a hairline edge (the
         * elevation cue — it rides OVER its neighbors), and the
         * accent underline when it carries the current page. */
        fdk_surface_fill_rect(surface, drag_rect,
                              fdk__pal_control_pressed());
        fdk_surface_draw_rect(surface, drag_rect, fdk__pal_border());
        if (nb->drag_index == nb->current) {
            fdk_rect bar = { drag_rect.x, drag_rect.y,
                             drag_rect.width, 2 };
            fdk_surface_fill_rect(surface, bar, fdk__pal_accent());
        }
        if (nb->font != NULL) {
            fdk_i32 baseline = fdk__center_baseline(
                nb->font, drag_rect.y, drag_rect.height);
            fdk__draw_text(surface, nb->font,
                           nb->pages[nb->drag_index].label,
                           fdk__pal_text(),
                           drag_rect.x + NB_TAB_PAD_X, baseline);
        }
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

/* ---- reorder (1.4.4) ---------------------------------------------
 */

/* Moves pages[from] to slot `to` (0 <= to < count) on the MODEL:
 * the array entry lifts out, the others close the gap, the entry
 * lands at `to`. The CURRENT page keeps its identity (its index is
 * re-derived from its widget pointer after the move — a reorder
 * never switches pages), and hover_tab follows the same arithmetic.
 * The caller owns invalidation and notifications. */
static void nb_reorder_move(fdk_notebook *nb, size_t from, size_t to) {
    if (from == to || from >= nb->count || to >= nb->count) {
        return;
    }
    fdk_widget *cur = (nb->count > 0) ? nb->pages[nb->current].widget
                                      : NULL;
    int hover = nb->hover_tab;
    fdk_notebook_page moved = nb->pages[from];
    if (from < to) {
        memmove(&nb->pages[from], &nb->pages[from + 1],
                (to - from) * sizeof(*nb->pages));
    } else {
        memmove(&nb->pages[to + 1], &nb->pages[to],
                (from - to) * sizeof(*nb->pages));
    }
    nb->pages[to] = moved;
    /* Re-index the current page by identity. */
    if (cur != NULL) {
        for (size_t i = 0; i < nb->count; i++) {
            if (nb->pages[i].widget == cur) {
                nb->current = i;
                break;
            }
        }
    }
    /* Hover follows the moved slot's arithmetic (it is -1 while a
     * drag runs; this branch serves the programmatic path). */
    if (hover >= 0) {
        if ((size_t)hover == from) {
            nb->hover_tab = (int)to;
        } else if (from < (size_t)hover && (size_t)hover <= to) {
            nb->hover_tab = hover - 1;
        } else if (to <= (size_t)hover && (size_t)hover < from) {
            nb->hover_tab = hover + 1;
        }
    }
}

/* One motion step while a reorder drag runs: run the swap ladder
 * with the RAW pointer delta (the swaps are pointer-driven — the
 * tab's center crossing neighbor midpoints), THEN clamp the tab
 * inside the strip (clamping first would freeze the tab in the slot
 * it started in, forbidding the very swaps the pointer asked for —
 * the bug the 1.4.4 drag test caught live). */
static void nb_drag_step(fdk_notebook *nb, fdk_f32 x) {
    if (nb->drag_index >= nb->count) {
        return; /* pages vanished mid-drag (programmatic removal) */
    }
    fdk_i32 tw = nb_tab_width(nb, nb->drag_index);
    fdk_i32 strip_w = nb_strip_width(nb);
    fdk_i32 dx = (fdk_i32)(x - nb->press_x);
    nb->drag_dx = dx;

    /* Swap right while the tab's center passes the right neighbor's
     * slot midpoint; each swap advances the slot, so the correction
     * subtracts the OLD right neighbor's advance. */
    for (;;) {
        if (nb->drag_index + 1 >= nb->count) {
            break;
        }
        fdk_i32 center = nb_tab_x(nb, nb->drag_index) + nb->drag_dx +
                         nb_tab_width(nb, nb->drag_index) / 2;
        size_t right = nb->drag_index + 1;
        fdk_i32 right_mid = nb_tab_x(nb, right) +
                            nb_tab_width(nb, right) / 2;
        if (center < right_mid) {
            break;
        }
        fdk_i32 advance = nb_tab_width(nb, right) + NB_TAB_GAP;
        nb_reorder_move(nb, nb->drag_index, right);
        nb->drag_index = right;
        nb->drag_dx -= advance;
        nb->drag_moved = true;
    }
    /* Swap left, symmetric. */
    for (;;) {
        if (nb->drag_index == 0) {
            break;
        }
        fdk_i32 center = nb_tab_x(nb, nb->drag_index) + nb->drag_dx +
                         nb_tab_width(nb, nb->drag_index) / 2;
        size_t left = nb->drag_index - 1;
        fdk_i32 left_mid = nb_tab_x(nb, left) +
                           nb_tab_width(nb, left) / 2;
        if (center > left_mid) {
            break;
        }
        fdk_i32 advance = nb_tab_width(nb, left) + NB_TAB_GAP;
        nb_reorder_move(nb, nb->drag_index, left);
        nb->drag_index = left;
        nb->drag_dx += advance;
        nb->drag_moved = true;
    }

    /* Clamp the settled tab inside the strip (a pointer far outside
     * pins the tab to the wall its swaps reached). */
    fdk_i32 slot = nb_tab_x(nb, nb->drag_index);
    if (slot + nb->drag_dx < 0) {
        nb->drag_dx = -slot;
    }
    if (slot + nb->drag_dx > strip_w - tw) {
        nb->drag_dx = strip_w - tw - slot;
    }
}

/* ---- events (tab clicks + the reorder drag) ---- */

static bool nb_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_notebook *nb = nb_of(w);
    if (ev->type == FDK_WIDGET_POINTER_MOTION) {
        if (nb->dragging) {
            /* The implicit grab routes post-press motion here even
             * outside the strip — the drag math clamps by x only. */
            nb_drag_step(nb, ev->position.x);
            fdk_widget_invalidate(w);
            return true;
        }
        int tab = nb_tab_at(nb, ev->position.x, ev->position.y);
        if (tab != nb->hover_tab) {
            nb->hover_tab = tab;
            fdk_widget_invalidate(w);
        }
        /* A press is down but under the slop: keep arming (the
         * promotion below reads the live position). */
        if (nb->press_armed && !nb->dragging) {
            fdk_f32 dx = ev->position.x - nb->press_x;
            if (dx < 0.0f) {
                dx = -dx;
            }
            if (dx > (fdk_f32)NB_REORDER_SLOP) {
                nb->dragging = true;
                nb->hover_tab = -1; /* declutter under the dragged tab */
                nb_drag_step(nb, ev->position.x);
                fdk_widget_invalidate(w);
                return true;
            }
        }
        return false; /* motion keeps bubbling */
    }
    if (ev->type == FDK_WIDGET_POINTER_UP) {
        if (nb->dragging) {
            nb->dragging = false;
            nb->press_armed = false;
            nb->drag_dx = 0; /* settle into the model slot */
            fdk_widget_invalidate(w);
            if (nb->drag_moved && nb->on_reorder != NULL) {
                nb->on_reorder(w, nb->drag_from, nb->drag_index,
                               nb->on_reorder_data);
            }
            if (nb->drag_moved) {
                fdk__a11y_notify(w, FDK_A11Y_CHILDREN_CHANGED, 0);
            }
            nb->drag_moved = false;
            return true;
        }
        if (nb->press_armed) {
            nb->press_armed = false; /* a plain click: already switched */
        }
        return false;
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
    /* Arm the reorder gesture (the implicit grab keeps MOTION/UP). */
    nb->press_armed = true;
    nb->press_x = ev->pointer.position.x;
    nb->drag_index = (size_t)tab;
    nb->drag_from = (size_t)tab;
    nb->drag_dx = 0;
    nb->drag_moved = false;
    return true;
}

static void nb_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = NB_MIN_W;
    out->height = NB_MIN_H;
}

static void nb_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    nb_sync_pages(nb_of(w));
}

static void nb_destroy(fdk_widget *w) {
    fdk_notebook *nb = nb_of(w);
    for (size_t i = 0; i < nb->count; i++) {
        fdk_free(nb->pages[i].label);
    }
    fdk_free(nb->pages);
}

/* ---- a11y ---- */
/* The notebook is ONE widget with painted tabs: the a11y tree exposes
 * the notebook as the tab list with the CURRENT page as its value
 * interface (SET_VALUE switches pages — the same code path tab clicks
 * take) AND one VIRTUAL CHILD per tab (role TAB, the page label as
 * its name, SELECTED on the current page, ACTIVATE = switch). */
static void nb_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_notebook *nb = (const fdk_notebook *)(const void *)w;
    if (nb->count > 0 && nb->current < nb->count &&
        nb->pages[nb->current].label != NULL) {
        out->name = fdk__strdup(nb->pages[nb->current].label);
    }
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = (double)((nb->count > 0) ? nb->count - 1 : 0);
    out->value_current = (double)nb->current;
}

static fdk_a11y_action_set nb_a11y_actions(const fdk_widget *w) {
    const fdk_notebook *nb = (const fdk_notebook *)(const void *)w;
    return (nb->count > 1) ? (fdk_a11y_action_set)FDK_A11Y_ACTION_SET_VALUE
                           : 0;
}

static bool nb_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                            double value) {
    if (action != FDK_A11Y_ACTION_SET_VALUE) {
        return false;
    }
    if (value < 0.0) {
        value = 0.0;
    }
    return fdk_ok(fdk_notebook_set_current_page(w, (size_t)(value + 0.5)));
}

/* ---- a11y: tabs as virtual children ---- */

static fdk_i32 nb_tab_width(const fdk_notebook *nb, size_t i);

static size_t nb_virtual_count(const fdk_widget *w) {
    const fdk_notebook *nb = (const fdk_notebook *)(const void *)w;
    return nb->count;
}

static void nb_virtual_describe(const fdk_widget *w, size_t index,
                                fdk_a11y_info *out) {
    const fdk_notebook *nb = (const fdk_notebook *)(const void *)w;
    out->role = FDK_A11Y_ROLE_TAB;
    if (nb->pages[index].label != NULL) {
        out->name = fdk__strdup(nb->pages[index].label);
    }
    out->states = FDK_A11Y_VISIBLE | FDK_A11Y_SHOWING |
                  FDK_A11Y_ENABLED;
    if (index == nb->current) {
        out->states |= FDK_A11Y_SELECTED;
    }
    /* Bounds: the tab's strip rect in the notebook's root-absolute
     * space (the strip layout nb_relayout computes). */
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_i32 x = 0;
    for (size_t i = 0; i < index; i++) {
        x += nb_tab_width(nb, i) + NB_TAB_GAP;
    }
    out->bounds = (fdk_rect){abs.x + x, abs.y,
                             nb_tab_width(nb, index), NB_TAB_H};
}

static fdk_a11y_action_set nb_virtual_actions(const fdk_widget *w,
                                              size_t index) {
    const fdk_notebook *nb = (const fdk_notebook *)(const void *)w;
    return (index != nb->current) ? (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE
                                  : 0;
}

static bool nb_virtual_perform(fdk_widget *w, size_t index,
                               fdk_a11y_action action, double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    return fdk_ok(fdk_notebook_set_current_page(w, index));
}

static const fdk_a11y_class nb_a11y = {
    .role = FDK_A11Y_ROLE_TAB_LIST,
    .describe = nb_a11y_describe,
    .actions = nb_a11y_actions,
    .perform = nb_a11y_perform,
    .virtual_count = nb_virtual_count,
    .virtual_describe = nb_virtual_describe,
    .virtual_actions = nb_virtual_actions,
    .virtual_perform = nb_virtual_perform,
};

const fdk_widget_class fdk_notebook_class_def = {
    .size = sizeof(fdk_notebook),
    .name = "notebook",
    .handle_event = nb_handle_event,
    .paint = nb_paint,
    .measure = nb_measure,
    .arrange = nb_arrange,
    .destroy = nb_destroy,
    .a11y = &nb_a11y,
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
        if (cap < nb->capacity ||
            cap > SIZE_MAX / sizeof(*nb->pages)) {
            return FDK_ERR_OUT_OF_MEMORY; /* refuse absurd growth */
        }
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
    /* Virtual children (the tabs) changed — the page widget itself
     * already fired its own CHILDREN_CHANGED through reparent. */
    fdk__a11y_notify(notebook, FDK_A11Y_CHILDREN_CHANGED, 0);
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

const char *fdk_notebook_page_label(fdk_widget *notebook, size_t index) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return NULL;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (index >= nb->count) {
        return NULL;
    }
    return nb->pages[index].label;
}

fdk_result fdk_notebook_remove_page(fdk_widget *notebook, size_t index) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (index >= nb->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *page = nb->pages[index].widget;
    bool was_current = (index == nb->current);
    fdk_free(nb->pages[index].label);
    memmove(&nb->pages[index], &nb->pages[index + 1],
            (nb->count - index - 1) * sizeof(*nb->pages));
    nb->count--;
    /* Which page shows afterwards: removing the current page shows
     * the page that shifted into its slot (index, clamped); removing
     * an earlier page keeps showing the same page (its index
     * shifted down). */
    if (nb->count == 0) {
        nb->current = 0;
    } else if (was_current) {
        nb->current = (index < nb->count) ? index : nb->count - 1;
    } else if (index < nb->current) {
        nb->current--;
    }
    if (nb->hover_tab >= (int)nb->count) {
        nb->hover_tab = -1;
    }
    /* The notebook owns its pages (append_page reparented them in —
     * the standard FDK parent-owns-children model): removing a page
     * destroys the widget, exactly like fdk_list_remove and
     * fdk_scrollview_set_content's replacement. */
    fdk_widget_destroy(page);
    nb_sync_pages(nb);
    fdk_widget_invalidate(notebook);
    fdk__a11y_notify(notebook, FDK_A11Y_CHILDREN_CHANGED, 0);
    if (was_current && nb->on_switch != NULL && nb->count > 0) {
        fdk_widget_watch watch;
        fdk__widget_watch(&watch, notebook);
        nb->on_switch(notebook, nb->current, nb->on_switch_data);
        fdk__widget_unwatch(&watch);
    }
    return FDK_OK;
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

/* ---- public reordering API (1.4.4) ---- */

fdk_result fdk_notebook_reorder_page(fdk_widget *notebook,
                                     size_t index, size_t new_index) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_notebook *nb = nb_of(notebook);
    if (index >= nb->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (new_index >= nb->count) {
        new_index = nb->count - 1; /* clamp, like GTK's position */
    }
    if (index == new_index) {
        return FDK_OK; /* explicit no-op */
    }
    nb_reorder_move(nb, index, new_index);
    fdk_widget_invalidate(notebook);
    fdk__a11y_notify(notebook, FDK_A11Y_CHILDREN_CHANGED, 0);
    if (nb->on_reorder != NULL) {
        fdk_widget_watch watch;
        fdk__widget_watch(&watch, notebook);
        nb->on_reorder(notebook, index, new_index, nb->on_reorder_data);
        fdk__widget_unwatch(&watch);
    }
    return FDK_OK;
}

size_t fdk_notebook_page_index(fdk_widget *notebook,
                               const fdk_widget *page) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def ||
        page == NULL) {
        return 0;
    }
    fdk_notebook *nb = nb_of(notebook);
    for (size_t i = 0; i < nb->count; i++) {
        if (nb->pages[i].widget == page) {
            return i;
        }
    }
    return nb->count; /* absent/foreign: the documented sentinel */
}

void fdk_notebook_set_on_page_reordered(fdk_widget *notebook,
                                        fdk_notebook_reorder_fn fn,
                                        void *user_data) {
    if (notebook == NULL || notebook->klass != &fdk_notebook_class_def) {
        return;
    }
    fdk_notebook *nb = nb_of(notebook);
    nb->on_reorder = fn;
    nb->on_reorder_data = user_data;
}
