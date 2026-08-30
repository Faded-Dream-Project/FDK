#define FDK_LOG_TAG "widgets"

/*
 * list.c — List widget (Phase 9)
 *
 * A vertical list of text rows with single / multiple / no selection,
 * built on the Phase 9 ScrollView: rows are widgets inside a plain
 * content container, so scrolling, clipping, and hit-testing are the
 * scrollview's problem (all already pixel-tested). Each row is a
 * "list-row" class widget painting its own text + selection state.
 *
 * Selection model (fdk_list_selection_mode):
 *   NONE     rows never select.
 *   SINGLE   click selects exactly one row (the classic list).
 *   MULTIPLE click selects; ctrl+click toggles one row without
 *            clearing others; shift+click selects the range from the
 *            anchor (the last plain-clicked row); ctrl+shift+click
 *            extends the anchor range additively. The anchor moves
 *            only on plain clicks and ctrl+clicks.
 *
 * Keyboard (when the list is focused — it is focusable by default):
 * Up/Down move the selection (extending with shift), Home/End go to
 * the ends, PageUp/PageDown move by the visible row count. In
 * MULTIPLE mode plain arrows collapse to a single selection (the
 * standard "keyboard resets multi" behavior every toolkit ships).
 *
 * The selection-changed callback fires once per user-visible change
 * (programmatic selects included), after the state is settled.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <time.h>
#include <string.h>

#define LIST_ROW_PAD_Y 5
#define LIST_ROW_PAD_X 10

typedef struct fdk_list_row {
    fdk_widget base;
    char *text;   /* owned */
    bool selected;
} fdk_list_row;

typedef struct fdk_list {
    fdk_widget base;
    fdk_font *font;      /* borrowed */
    fdk_list_selection_mode mode;
    fdk_widget *scroll;  /* internal ScrollView (child)          */
    fdk_widget *rows;    /* content container inside the scroll  */
    fdk_list_row **row_widgets; /* row widgets in row order       */
    size_t count;
    size_t capacity;
    size_t anchor;       /* shift-click anchor row               */
    size_t key_cursor;   /* the keyboard's moving end (shift+arrows) */
    fdk_list_selection_fn on_selection_changed;
    void *on_selection_data;
    /* ---- Row activation (1.2.0) ----
     *
     * Double-click and Enter both fire on_row_activate — the
     * "open this" gesture file managers and file dialogs are built
     * on. The click tracking uses the shared double-click predicate
     * (same window, same slop policy as the title bar). */
    fdk_list_row_activate_fn on_row_activate;
    void *on_row_activate_data;
    fdk_i64 last_click_ms;   /* last left-press, for dbl detection */
    size_t last_click_row;   /* the row that press selected        */
    bool have_last_click;
} fdk_list;

static fdk_list *list_of(fdk_widget *w) {
    return (fdk_list *)(void *)w;
}
static fdk_list_row *row_of(fdk_widget *w) {
    return (fdk_list_row *)(void *)w;
}

static const fdk_widget_class fdk_list_row_class_def;
extern const fdk_widget_class fdk_list_class_def;

/* ---- row geometry ---- */

static fdk_i32 list_row_height(const fdk_list *l) {
    if (l->font == NULL) {
        return 20; /* textless rows keep a sane hit area */
    }
    fdk_font_metrics m;
    fdk_font_get_metrics(l->font, &m);
    fdk_i32 h = m.ascent + m.descent + LIST_ROW_PAD_Y * 2;
    return (h < 16) ? 16 : h;
}

/* Re-places every row inside the rows container + refreshes the
 * container's natural size (so the scrollview's bars/clamps follow).
 * Called after any mutation. */
static void list_relayout(fdk_list *l) {
    if (l->rows == NULL) {
        return;
    }
    fdk_i32 rh = list_row_height(l);
    fdk_i32 w = 0;
    /* Width: the widest row's text (min 80) — rows stretch to it. */
    for (size_t i = 0; i < l->count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->row_widgets[i]->text, &tw, &th);
        if (tw + LIST_ROW_PAD_X * 2 > w) {
            w = tw + LIST_ROW_PAD_X * 2;
        }
    }
    if (w < 80) {
        w = 80;
    }
    for (size_t i = 0; i < l->count; i++) {
        fdk_rect r = { 0, (fdk_i32)(i * (size_t)rh), w, rh };
        fdk_widget_set_bounds(&l->row_widgets[i]->base, r);
    }
    fdk_widget_set_natural_size(l->rows, w,
                                (fdk_i32)(l->count * (size_t)rh));
    /* Self-sync the internal scrollview to the list's CURRENT
     * bounds: covers both arrange-driven sizing and the direct
     * fdk_widget_set_bounds() path (set_bounds does not run arrange
     * hooks — fdk_widget_arrange is the layout engine's entry, and
     * applications positioning a list by hand use set_bounds). */
    if (l->scroll != NULL && l->base.bounds.width > 0 &&
        l->base.bounds.height > 0) {
        fdk_rect inner = { 0, 0, l->base.bounds.width,
                           l->base.bounds.height };
        fdk_widget_set_bounds(l->scroll, inner);
    }
    fdk_widget_child_layout_changed(l->rows->parent);
}

/* ---- selection plumbing ---- */

static void list_fire_changed(fdk_list *l) {
    if (l->on_selection_changed != NULL) {
        l->on_selection_changed(&l->base, l->on_selection_data);
    }
}

static fdk_i64 list_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

static void list_fire_activated(fdk_list *l, size_t row) {
    if (l->on_row_activate != NULL) {
        l->on_row_activate(&l->base, row, l->on_row_activate_data);
    }
}

/* Emits selection state changes on the affected rows. */
static void list_set_row(fdk_list *l, size_t index, bool selected) {
    if (index >= l->count) {
        return;
    }
    fdk_list_row *row = l->row_widgets[index];
    if (row->selected != selected) {
        row->selected = selected;
        fdk_widget_invalidate(&row->base);
        /* A11y: the row's selected state flipped. */
        fdk__a11y_notify(&row->base, FDK_A11Y_STATE_CHANGED,
                         FDK_A11Y_SELECTED);
    }
}

static void list_clear_selection(fdk_list *l) {
    for (size_t i = 0; i < l->count; i++) {
        list_set_row(l, i, false);
    }
}

size_t fdk_list_selected_count(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return 0;
    }
    fdk_list *l = list_of(list);
    size_t n = 0;
    for (size_t i = 0; i < l->count; i++) {
        if (l->row_widgets[i]->selected) {
            n++;
        }
    }
    return n;
}

fdk_result fdk_list_selected_at(fdk_widget *list, size_t position,
                                size_t *out_row) {
    if (list == NULL || list->klass != &fdk_list_class_def ||
        out_row == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    size_t seen = 0;
    for (size_t i = 0; i < l->count; i++) {
        if (l->row_widgets[i]->selected) {
            if (seen == position) {
                *out_row = i;
                return FDK_OK;
            }
            seen++;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

/* First selected row (the SINGLE-mode query), -1 when none. */
fdk_i64 fdk_list_get_selected(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return -1;
    }
    fdk_list *l = list_of(list);
    for (size_t i = 0; i < l->count; i++) {
        if (l->row_widgets[i]->selected) {
            return (fdk_i64)i;
        }
    }
    return -1;
}

bool fdk_list_is_selected(fdk_widget *list, size_t row) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return false;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return false;
    }
    return l->row_widgets[row]->selected;
}

fdk_result fdk_list_select(fdk_widget *list, size_t row) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (l->mode == FDK_LIST_SELECTION_NONE) {
        return FDK_ERR_UNSUPPORTED;
    }
    list_clear_selection(l);
    list_set_row(l, row, true);
    l->anchor = row;
    l->key_cursor = row;
    list_fire_changed(l);
    return FDK_OK;
}

/* Click semantics: plain / ctrl / shift / ctrl+shift. */
static void list_row_clicked(fdk_list *l, size_t index, fdk_u32 mods) {
    if (l->mode == FDK_LIST_SELECTION_NONE) {
        return;
    }
    bool ctrl = (mods & FDK_MOD_CTRL) != 0;
    bool shift = (mods & FDK_MOD_SHIFT) != 0;

    if (l->mode == FDK_LIST_SELECTION_SINGLE) {
        list_clear_selection(l);
        list_set_row(l, index, true);
        l->anchor = index;
        l->key_cursor = index;
        list_fire_changed(l);
        return;
    }

    /* MULTIPLE: */
    if (ctrl && shift) {
        /* Add the anchor range without clearing anything. */
        size_t lo = (l->anchor < index) ? l->anchor : index;
        size_t hi = (l->anchor < index) ? index : l->anchor;
        for (size_t i = lo; i <= hi && i < l->count; i++) {
            list_set_row(l, i, true);
        }
        /* Anchor unchanged (ctrl keeps it). */
    } else if (shift) {
        list_clear_selection(l);
        size_t lo = (l->anchor < index) ? l->anchor : index;
        size_t hi = (l->anchor < index) ? index : l->anchor;
        for (size_t i = lo; i <= hi && i < l->count; i++) {
            list_set_row(l, i, true);
        }
    } else if (ctrl) {
        list_set_row(l, index, !l->row_widgets[index]->selected);
        l->anchor = index;
        l->key_cursor = index;
    } else {
        list_clear_selection(l);
        list_set_row(l, index, true);
        l->anchor = index;
        l->key_cursor = index;
    }
    list_fire_changed(l);
}

/* ---- rows ---- */

static void row_paint(fdk_widget *w, fdk_surface *surface,
                      fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_list_row *row = row_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_list *l = list_of(w->parent && w->parent->parent
                              ? w->parent->parent
                              : NULL);
    (void)l;

    fdk_color fill;
    if (row->selected) {
        /* Accent-tinted selection band (the v1 way, matching the
         * Entry selection highlight). */
        fdk_color accent = fdk__pal_accent();
        fill = (fdk_color){accent.r, accent.g, accent.b, 0.45f};
        fdk_surface_fill_rect(surface, bounds, fill);
    } else if ((w->flags & FDK_WF_HOVERED) != 0 &&
               (w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_fill_rect(surface, bounds,
                              fdk__pal_control_hover());
    }

    /* The row's parent chain: rows container -> scrollview -> list.
     * The FONT lives on the list, two hops up. */
    fdk_font *font = NULL;
    if (w->parent != NULL && w->parent->parent != NULL) {
        fdk_widget *gp = w->parent->parent;
        /* scrollview's parent is the list */
        if (gp->parent != NULL && gp->parent->klass == &fdk_list_class_def) {
            font = list_of(gp->parent)->font;
        }
    }
    if (font == NULL) {
        return;
    }
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(font, row->text, &tw, &th);
    fdk_i32 baseline = fdk__center_baseline(font, bounds.y,
                                            bounds.height);
    fdk__draw_text(surface, font, row->text, fdk__pal_text(),
                   bounds.x + LIST_ROW_PAD_X, baseline);
}

static bool row_handle_event(fdk_widget *w,
                             const fdk_widget_event *ev) {
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        /* Find our index (the rows container holds only rows, in
         * order — a linear scan is honest and bounded by list size). */
        fdk_widget *rows = w->parent;
        fdk_widget *scroll = rows ? rows->parent : NULL;
        fdk_widget *list_w = scroll ? scroll->parent : NULL;
        if (list_w == NULL || list_w->klass != &fdk_list_class_def) {
            return false;
        }
        fdk_list *l = list_of(list_w);
        for (size_t i = 0; i < l->count; i++) {
            if (&l->row_widgets[i]->base == w) {
                list_row_clicked(l, i, ev->pointer.modifiers);
                /* Double-click = activation (1.2.0). Same row, same
                 * press semantics as the title bar's dblclick window
                 * (fdk__window_is_double_click; dx/dy are 0 because
                 * the SECOND click is on the same row widget — the
                 * row IS the slop region here). */
                fdk_i64 now = list_now_ms();
                if (l->have_last_click && l->last_click_row == i &&
                    fdk__window_is_double_click(now, l->last_click_ms,
                                                0, 0)) {
                    l->have_last_click = false; /* a triple click
                                                   re-arms from zero */
                    list_fire_activated(l, i);
                } else {
                    l->have_last_click = true;
                    l->last_click_row = i;
                    l->last_click_ms = now;
                }
                return true;
            }
        }
        return false;
    }
    default:
        break;
    }
    return false;
}

static void row_destroy(fdk_widget *w) {
    fdk_list_row *row = row_of(w);
    fdk_free(row->text);
}

/* ---- a11y ---- */

static void list_row_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_list_row *row = (const fdk_list_row *)(const void *)w;
    if (row->text != NULL) {
        out->name = fdk__strdup(row->text);
    }
    if (row->selected) {
        out->states |= FDK_A11Y_SELECTED;
    }
}

static fdk_list *list_of_row(const fdk_widget *row) {
    /* rows -> content box -> scrollview -> list */
    fdk_widget *cur = row->parent;
    while (cur != NULL && cur->klass != &fdk_list_class_def) {
        cur = cur->parent;
    }
    return (cur != NULL) ? list_of(cur) : NULL;
}

static bool list_row_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                  double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    /* Plain-click semantics through the same code path. */
    fdk_list *l = list_of_row(w);
    if (l == NULL || l->mode == FDK_LIST_SELECTION_NONE) {
        return false;
    }
    for (size_t i = 0; i < l->count; i++) {
        if (l->row_widgets[i] == (fdk_list_row *)(void *)w) {
            list_row_clicked(l, i, 0);
            return true;
        }
    }
    return false;
}

static fdk_a11y_action_set list_row_a11y_actions(const fdk_widget *w) {
    fdk_list *l = list_of_row(w);
    return (l != NULL && l->mode != FDK_LIST_SELECTION_NONE)
               ? (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE
               : 0;
}

static const fdk_a11y_class list_row_a11y = {
    .role = FDK_A11Y_ROLE_LIST_ITEM,
    .describe = list_row_a11y_describe,
    .actions = list_row_a11y_actions,
    .perform = list_row_a11y_perform,
};

static const fdk_widget_class fdk_list_row_class_def = {
    .size = sizeof(fdk_list_row),
    .name = "list-row",
    .handle_event = row_handle_event,
    .paint = row_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = row_destroy,
    .a11y = &list_row_a11y,
};

/* ---- list-level events (keyboard) ---- */

static bool list_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_list *l = list_of(w);
    if (ev->type != FDK_WIDGET_KEY_DOWN ||
        (w->flags & FDK_WF_FOCUSED) == 0 ||
        l->mode == FDK_LIST_SELECTION_NONE ||
        l->count == 0) {
        return false;
    }
    /* Enter activates the keyboard cursor's row (1.2.0) — the
     * keyboard's "open this", same callback as the double-click. */
    if (ev->key.scancode == FDK_KEY_ENTER) {
        if (l->key_cursor != (size_t)-1 && l->key_cursor < l->count) {
            list_fire_activated(l, l->key_cursor);
            return true;
        }
        return false;
    }
    bool shift = (ev->key.modifiers & FDK_MOD_SHIFT) != 0;
    /* The keyboard's moving end is key_cursor (the last row the
     * keyboard/selection landed on) — NOT the first selected row,
     * which in a shift-range is the far end from the anchor. */
    fdk_i64 cur = (l->key_cursor == (size_t)-1 ||
                   l->key_cursor >= l->count)
                      ? -1
                      : (fdk_i64)l->key_cursor;
    fdk_i64 next;
    switch (ev->key.scancode) {
    case FDK_KEY_UP:
        next = (cur < 0) ? (fdk_i64)l->count - 1 : cur - 1;
        break;
    case FDK_KEY_DOWN:
        next = (cur < 0) ? 0 : cur + 1;
        break;
    case FDK_KEY_HOME:
        next = 0;
        break;
    case FDK_KEY_END:
        next = (fdk_i64)l->count - 1;
        break;
    case FDK_KEY_PAGE_UP:
    case FDK_KEY_PAGE_DOWN: {
        /* Rows visible in the scroll viewport (bar-adjusted; bounded
         * guess when not laid out yet). */
        fdk_i32 vh = 160;
        if (l->scroll != NULL) {
            fdk_i32 vw2 = 0;
            fdk__scrollview_viewport(l->scroll, &vw2, &vh);
        }
        fdk_i32 rh = list_row_height(l);
        fdk_i64 page = (rh > 0) ? (fdk_i64)(vh / rh) : 10;
        if (page < 1) {
            page = 1;
        }
        next = (cur < 0) ? 0
                         : (ev->key.scancode == FDK_KEY_PAGE_UP)
                               ? cur - page
                               : cur + page;
        break;
    }
    default:
        return false;
    }
    if (next < 0) {
        next = 0;
    }
    if (next >= (fdk_i64)l->count) {
        next = (fdk_i64)l->count - 1;
    }

    if (l->mode == FDK_LIST_SELECTION_MULTIPLE && shift && cur >= 0) {
        /* Shift+arrows extend the anchor range (visual mode, the
         * common list behavior): recompute from the anchor. */
        list_clear_selection(l);
        size_t target = (size_t)next;
        size_t lo = (l->anchor < target) ? l->anchor : target;
        size_t hi = (l->anchor < target) ? target : l->anchor;
        for (size_t i = lo; i <= hi && i < l->count; i++) {
            list_set_row(l, i, true);
        }
        l->key_cursor = (size_t)next;
        list_fire_changed(l);
    } else {
        l->key_cursor = (size_t)next;
        (void)fdk_list_select(w, (size_t)next);
    }
    return true;
}

static void list_destroy(fdk_widget *w) {
    fdk_list *l = list_of(w);
    fdk_free(l->row_widgets);
    l->row_widgets = NULL;
    l->count = 0;
    l->capacity = 0;
}

static void list_measure(fdk_widget *w, fdk_size *out) {
    fdk_list *l = list_of(w);
    /* Natural: up to 8 rows tall, wide enough for the widest row. */
    fdk_i32 rh = list_row_height(l);
    fdk_i32 width = 80;
    for (size_t i = 0; i < l->count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->row_widgets[i]->text, &tw, &th);
        if (tw + LIST_ROW_PAD_X * 2 > width) {
            width = tw + LIST_ROW_PAD_X * 2;
        }
    }
    size_t shown = (l->count < 8) ? l->count : 8;
    out->width = width;
    out->height = (fdk_i32)(shown * (size_t)rh);
    if (out->height == 0) {
        out->height = rh; /* empty lists still show a bar of height */
    }
}

static void list_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_list *l = list_of(w);
    if (l->scroll != NULL) {
        fdk_rect inner = { 0, 0, assigned.width, assigned.height };
        fdk_widget_set_bounds(l->scroll, inner);
        fdk_widget_child_layout_changed(l->scroll);
    }
}

/* The lazy-sync half of the scrollview bookkeeping. fdk_widget_
 * set_bounds() is pure geometry — it does NOT run arrange hooks —
 * and both FDK's own dialogs and ordinary applications position
 * lists by hand with it. The append path (list_relayout) syncs the
 * internal scrollview to the list's then-current bounds, so a list
 * sized AFTER its last append (or appended-to while still 0x0)
 * kept a 0x0/stale scrollview: the paint walk skips empty children,
 * so every row stayed invisible forever — found live by the 1.2.3
 * dialog rig (places sidebar). Syncing here, in the paint hook
 * that runs before the subtree is walked, heals any staleness with
 * one compare; the set_bounds inside damages the region, which
 * merely schedules one more (identical) frame — it converges. */
static void list_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)surface;
    (void)bounds;
    (void)clip;
    fdk_list *l = list_of(w);
    if (l->scroll == NULL || w->bounds.width <= 0 ||
        w->bounds.height <= 0) {
        return;
    }
    if (l->scroll->bounds.x == 0 && l->scroll->bounds.y == 0 &&
        l->scroll->bounds.width == w->bounds.width &&
        l->scroll->bounds.height == w->bounds.height) {
        return; /* in sync */
    }
    fdk_rect inner = { 0, 0, w->bounds.width, w->bounds.height };
    fdk_widget_set_bounds(l->scroll, inner);
    fdk_widget_child_layout_changed(l->scroll);
}

static void list_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_list *l = (const fdk_list *)(const void *)w;
    if (l->mode == FDK_LIST_SELECTION_MULTIPLE) {
        out->states |= FDK_A11Y_MULTI_SELECTABLE;
    }
}

static const fdk_a11y_class list_a11y = {
    .role = FDK_A11Y_ROLE_LIST,
    .describe = list_a11y_describe,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_list_class_def = {
    .size = sizeof(fdk_list),
    .name = "list",
    .handle_event = list_handle_event,
    .paint = list_paint,
    .measure = list_measure,
    .arrange = list_arrange,
    .destroy = list_destroy, /* the row POINTER array; the rows
                                themselves die with the subtree */
    .a11y = &list_a11y,
};

/* ---- public API ---- */

fdk_result fdk_list_create(fdk_widget *parent, fdk_font *font,
                           fdk_widget **out_list) {
    if (out_list == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_list_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_list *l = list_of(w);
    l->font = font;
    l->mode = FDK_LIST_SELECTION_SINGLE;
    l->key_cursor = (size_t)-1; /* cold start: Down selects row 0 */
    fdk_widget_set_can_focus(w, true);

    /* Internals: scrollview child -> rows container. */
    r = fdk_scrollview_create(w, &l->scroll);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    r = fdk_widget_create(l->scroll, NULL, (fdk_rect){0, 0, 0, 0},
                          &l->rows);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    (void)fdk_scrollview_set_content(l->scroll, l->rows);

    fdk_widget_child_layout_changed(w->parent);
    *out_list = w;
    return FDK_OK;
}

void fdk_list_set_selection_mode(fdk_widget *list,
                                 fdk_list_selection_mode mode) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    l->mode = mode;
    if (mode == FDK_LIST_SELECTION_NONE) {
        list_clear_selection(l);
        list_fire_changed(l);
    }
}

static fdk_result list_grow(fdk_list *l) {
    if (l->count < l->capacity) {
        return FDK_OK;
    }
    size_t cap = (l->capacity == 0) ? 8 : l->capacity * 2;
    fdk_list_row **grown =
        fdk_realloc(l->row_widgets, cap * sizeof(*grown));
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    l->row_widgets = grown;
    l->capacity = cap;
    return FDK_OK;
}

static fdk_result list_insert_internal(fdk_list *l, size_t index,
                                       const char *text) {
    if (index > l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = list_grow(l);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_widget *row_w = NULL;
    r = fdk_widget_create(l->rows, &fdk_list_row_class_def,
                          (fdk_rect){0, 0, 0, 0}, &row_w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_list_row *row = row_of(row_w);
    row->text = fdk__strdup(text != NULL ? text : "");
    if (row->text == NULL) {
        fdk_widget_destroy(row_w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* Shift the widget array (widget z-order follows row order). */
    memmove(&l->row_widgets[index + 1], &l->row_widgets[index],
            (l->count - index) * sizeof(*l->row_widgets));
    l->row_widgets[index] = row;
    l->count++;
    list_relayout(l);
    return FDK_OK;
}

fdk_result fdk_list_append(fdk_widget *list, const char *text,
                           size_t *out_index) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    fdk_result r = list_insert_internal(l, l->count, text);
    if (fdk_ok(r) && out_index != NULL) {
        *out_index = l->count - 1;
    }
    return r;
}

fdk_result fdk_list_insert(fdk_widget *list, size_t index,
                           const char *text) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return list_insert_internal(list_of(list), index, text);
}

fdk_result fdk_list_remove(fdk_widget *list, size_t index) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (index >= l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *row_w = &l->row_widgets[index]->base;
    /* Remove from the array first so relayout doesn't see a row that
     * is mid-destruction. */
    memmove(&l->row_widgets[index], &l->row_widgets[index + 1],
            (l->count - index - 1) * sizeof(*l->row_widgets));
    l->count--;
    fdk_widget_destroy(row_w);
    if (l->anchor >= l->count && l->count > 0) {
        l->anchor = l->count - 1;
    }
    list_relayout(l);
    list_fire_changed(l);
    return FDK_OK;
}

void fdk_list_clear(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    while (l->count > 0) {
        fdk_result r = fdk_list_remove(list, l->count - 1);
        if (!fdk_ok(r)) {
            break;
        }
    }
    list_fire_changed(l);
}

size_t fdk_list_row_count(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return 0;
    }
    return list_of(list)->count;
}

const char *fdk_list_row_text(fdk_widget *list, size_t row) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return NULL;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return NULL;
    }
    return l->row_widgets[row]->text;
}

fdk_result fdk_list_set_row_text(fdk_widget *list, size_t row,
                                 const char *text) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = fdk__strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(l->row_widgets[row]->text);
    l->row_widgets[row]->text = copy;
    list_relayout(l);
    fdk_widget_invalidate(&l->row_widgets[row]->base);
    return FDK_OK;
}

void fdk_list_set_on_selection_changed(fdk_widget *list,
                                       fdk_list_selection_fn fn,
                                       void *user_data) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    l->on_selection_changed = fn;
    l->on_selection_data = user_data;
}

void fdk_list_set_on_row_activate(fdk_widget *list,
                                  fdk_list_row_activate_fn fn,
                                  void *user_data) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    l->on_row_activate = fn;
    l->on_row_activate_data = user_data;
}
