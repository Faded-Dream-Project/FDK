#define FDK_LOG_TAG "widgets"

/*
 * iconview.c — the item GRID (1.4.5)
 *
 * The canvas-based item layout the parity ledger parked since its
 * first draft: a scrolling field of fixed-size cells (glyph over
 * label), flowing left-to-right then top-to-bottom in as many
 * columns as the current width fits. The List's whole discipline
 * applies — ScrollView internals (bars, wheel, clipping), the
 * selection model (NONE/SINGLE/MULTIPLE with click / ctrl / shift
 * in reading order), activation by double-click (1.4.7 — the
 * header claimed it since 1.4.5; the cells now deliver it) or
 * Enter, the 1.4.7 band auto-scroll (BOTH axes — the grid scrolls
 * sideways too), batched bulk mutation, and the a11y LIST with the
 * cells as real child widgets.
 *
 * Geometry: cells are CELL_W x CELL_H (settable); the icon box is
 * the cell's top band (48 px default), the label the bottom band
 * (clipped to the cell width — no wrap, no ellipsis glyph). The
 * column count is derived at every arrange from the CURRENT width
 * (the same request/allocate rhythm the List applies to rows), and
 * the grid container's natural size follows the full grid so the
 * scrollview's bars and clamps are honest.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define IV_CELL_W 96
#define IV_CELL_H 84
#define IV_ICON_BOX 48
#define IV_GRID_GAP 4
#define IV_MIN_W 80
#define IV_MIN_H 60

typedef struct fdk_iv_item {
    fdk_widget *cell;    /* owned via the tree (class iconview-item) */
    char *label;         /* owned */
    fdk_row_icon icon;
    bool selected;
} fdk_iv_item;

typedef struct fdk_iconview {
    fdk_widget base;
    fdk_font *font;        /* borrowed */
    fdk_iv_item *items;
    size_t count;
    size_t capacity;
    fdk_list_selection_mode mode;
    size_t selected_count;
    size_t key_cursor;    /* (size_t)-1 = cold start */
    size_t anchor;        /* the shift-range's fixed end (the last
                           * plain/ctrl click — the List's rule) */
    fdk_i32 cell_w;
    fdk_i32 cell_h;
    /* Layout state (recomputed per arrange). */
    size_t columns;
    fdk_i32 grid_w;       /* the container's natural width  */
    fdk_i32 grid_h;       /* the container's natural height */
    /* Internals: scrollview child -> grid container. */
    fdk_widget *scroll;
    fdk_widget *grid;
    /* Batch (the List's contract). */
    int batch_depth;
    bool batch_dirty;
    /* Callbacks. */
    fdk_list_selection_fn on_selection_changed;
    void *on_selection_data;
    fdk_iconview_item_fn on_activate;
    void *on_activate_data;
    /* ---- the rubber-band sweep (1.4.6) ----
     *
     * The List's drag-select, grid-shaped: a left press on EMPTY
     * grid space sweeps a 2-D rect; every cell whose slot INTERSECTS
     * the rect selects live, plain replacing / ctrl unioning over
     * the press-time snapshot. The anchor is glued to CONTENT space
     * (captured at press with the offset) so the 1.4.7 auto-scroll
     * extension composes: the moving edge rides the offset, the
     * anchor does not, and the sweep GROWS as the view chases the
     * pointer. MULTIPLE mode only. */
    bool banding;
    bool band_ctrl;
    fdk_f32 band_x, band_y;        /* viewport-space press point  */
    fdk_f32 band_ax_cx, band_ay_cy; /* CONTENT-space anchor       */
    fdk_f32 band_now_x, band_now_y;
    bool *band_base;               /* ctrl-union snapshot         */
    size_t band_base_cap;
    /* ---- band auto-scroll (1.4.7) ----
     *
     * The List's edge-chasing timer, grid-shaped: while the sweep
     * pointer rests outside the viewport's edge zone on EITHER
     * axis, a repeating timer scrolls toward it and re-applies the
     * band. The band's coordinates are VIEWPORT-space (apply_band
     * adds the CURRENT offset when mapping cells), so a stationary
     * pointer needs no updates while the content slides under it —
     * the sweep simply GROWS as the view chases. Disarmed at the
     * release, on destruction, and when the pointer re-enters. */
    fdk_timer *band_scroll_timer;
    /* ---- double-click activation (1.4.7) ----
     * The List's predicate discipline: same item, same press
     * semantics (fdk__window_is_double_click; the second press is
     * on the same cell, so dx/dy are 0). A triple click re-arms
     * from zero. */
    bool have_last_click;
    size_t last_click_item;
    fdk_i64 last_click_ms;
} fdk_iconview;

static fdk_iconview *iv_of(fdk_widget *w) {
    return (fdk_iconview *)(void *)w;
}

static fdk_i64 iv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

extern const fdk_widget_class fdk_iconview_class_def;
static const fdk_widget_class fdk_iv_cell_class_def;

/* The band auto-scroll constants (1.4.7): the List's cadence, step,
 * and edge zone — applied to BOTH axes (the grid scrolls sideways). */
#define IV_BAND_SCROLL_MS 40
#define IV_BAND_SCROLL_STEP 24
#define IV_BAND_EDGE 16

/* ---- geometry ---- */

static size_t iv_columns_for(const fdk_iconview *iv, fdk_i32 width) {
    fdk_i32 step = iv->cell_w + IV_GRID_GAP;
    if (step <= 0 || width <= 0) {
        return 1;
    }
    size_t cols = (size_t)((width + IV_GRID_GAP) / step);
    return (cols > 0) ? cols : 1;
}

static void iv_cell_slot(const fdk_iconview *iv, size_t index,
                         fdk_i32 *out_x, fdk_i32 *out_y) {
    size_t cols = (iv->columns > 0) ? iv->columns : 1;
    size_t col = index % cols;
    size_t row = index / cols;
    *out_x = (fdk_i32)col * (iv->cell_w + IV_GRID_GAP);
    *out_y = (fdk_i32)row * (iv->cell_h + IV_GRID_GAP);
}

/* Places every cell at its grid slot and sizes the container's
 * natural size from the full grid (the scrollview's bars, clamps,
 * and measure all follow that). One relayout at the batch's end
 * covers bulk fills. */
static void iv_relayout(fdk_iconview *iv) {
    size_t cols = iv->columns;
    if (cols == 0) {
        cols = 1;
    }
    size_t rows = (iv->count + cols - 1) / cols;
    iv->grid_w = (fdk_i32)cols * iv->cell_w +
                 (fdk_i32)(cols - 1) * IV_GRID_GAP;
    iv->grid_h = (fdk_i32)rows * iv->cell_h +
                 (fdk_i32)(rows > 0 ? rows - 1 : 0) * IV_GRID_GAP;
    if (iv->grid_w < IV_MIN_W) {
        iv->grid_w = IV_MIN_W;
    }
    if (iv->grid_h < IV_MIN_H) {
        iv->grid_h = IV_MIN_H;
    }
    if (iv->grid != NULL) {
        fdk_widget_set_natural_size(iv->grid, iv->grid_w, iv->grid_h);
        fdk_i32 x = 0, y = 0;
        for (size_t i = 0; i < iv->count; i++) {
            iv_cell_slot(iv, i, &x, &y);
            fdk_widget_set_bounds(iv->items[i].cell,
                                  (fdk_rect){x, y, iv->cell_w,
                                             iv->cell_h});
        }
        /* Self-sync the internal scrollview to the view's CURRENT
         * bounds (the List's rule, found live by the 1.4.7 file
         * dialog integration): set_bounds does not run arrange
         * hooks — fdk_widget_arrange is the layout engine's entry —
         * and dialogs that position surfaces by hand (the file
         * dialog's body arrange) use set_bounds. Without this, the
         * scrollview stays at its creation 0x0 and nothing paints,
         * scrolls, or hit-tests. */
        if (iv->scroll != NULL && iv->base.bounds.width > 0 &&
            iv->base.bounds.height > 0) {
            fdk_rect inner = { 0, 0, iv->base.bounds.width,
                               iv->base.bounds.height };
            fdk_widget_set_bounds(iv->scroll, inner);
        }
        fdk__scrollview_layout_changed(iv->scroll);
    }
}

static void iv_measure(fdk_widget *w, fdk_size *out) {
    fdk_iconview *iv = iv_of(w);
    /* Natural = the viewport's column arrangement at the CURRENT
     * width: a narrow parent shows fewer columns and grows taller.
     * The grid container's natural carries the real content size;
     * the VIEW's natural is one honest screenful (the List's
     * approach: something smaller gets assigned and it scrolls). */
    size_t cols = iv_columns_for(iv, w->bounds.width);
    size_t rows = (iv->count + cols - 1) / cols;
    out->width = (fdk_i32)cols * iv->cell_w;
    if (out->width < IV_MIN_W) {
        out->width = IV_MIN_W;
    }
    out->height = (fdk_i32)rows * iv->cell_h;
    if (out->height < IV_MIN_H) {
        out->height = IV_MIN_H;
    }
}

static void iv_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_iconview *iv = iv_of(w);
    fdk_widget_set_bounds(w, assigned);
    if (iv->scroll != NULL) {
        /* INNER rect: the scrollview is the view's full-area child —
         * the List's pattern. (Passing `assigned` through verbatim
         * double-offseted the scrollview by the view's own x/y — a
         * latent 1.4.5 bug every arrange site at x=0 hid.) */
        fdk_rect inner = { 0, 0, assigned.width, assigned.height };
        fdk_widget_set_bounds(iv->scroll, inner);
        fdk_widget_child_layout_changed(iv->scroll);
    }
    /* Columns follow the CURRENT width; the cells re-slot. */
    size_t cols = iv_columns_for(iv, assigned.width);
    if (cols != iv->columns) {
        iv->columns = cols;
        iv_relayout(iv);
    }
}

/* ---- selection ---- */

static void iv_fire_selection_changed(fdk_iconview *iv) {
    if (iv->on_selection_changed != NULL) {
        iv->on_selection_changed(&iv->base, iv->on_selection_data);
    }
}

static void iv_clear_selection(fdk_iconview *iv) {
    bool any = (iv->selected_count > 0);
    for (size_t i = 0; i < iv->count; i++) {
        iv->items[i].selected = false;
    }
    iv->selected_count = 0;
    if (any) {
        iv_fire_selection_changed(iv);
    }
}

/* The keyboard cursor's landing: select per the mode + modifiers
 * (the List's rules, grid-shaped). */
static void iv_cursor_select(fdk_iconview *iv, size_t index,
                             fdk_u32 modifiers) {
    if (index >= iv->count) {
        return;
    }
    iv->key_cursor = index;
    bool shift = (modifiers & FDK_MOD_SHIFT) != 0;
    bool ctrl = (modifiers & FDK_MOD_CTRL) != 0;
    if (iv->mode == FDK_LIST_SELECTION_NONE) {
        return;
    }
    if (iv->mode == FDK_LIST_SELECTION_SINGLE) {
        if (!iv->items[index].selected) {
            iv_clear_selection(iv);
            iv->items[index].selected = true;
            iv->selected_count = 1;
            iv->anchor = index;
            iv_fire_selection_changed(iv);
        }
        return;
    }
    /* MULTIPLE. */
    if (shift && iv->anchor != (size_t)-1) {
        size_t from = iv->anchor;
        size_t to = index;
        if (from > to) {
            size_t t = from;
            from = to;
            to = t;
        }
        if (!ctrl) {
            iv_clear_selection(iv);
        }
        for (size_t i = from; i <= to && i < iv->count; i++) {
            if (!iv->items[i].selected) {
                iv->items[i].selected = true;
                iv->selected_count++;
            }
        }
        iv_fire_selection_changed(iv);
        return;
    }
    if (ctrl) {
        if (iv->items[index].selected) {
            iv->items[index].selected = false;
            if (iv->selected_count > 0) {
                iv->selected_count--;
            }
        } else {
            iv->items[index].selected = true;
            iv->selected_count++;
        }
        /* NOTE: ctrl does NOT move the anchor — the next
         * shift-click still ranges from the last PLAIN click
         * (GTK's rule; the test pins it). */
        iv_fire_selection_changed(iv);
        return;
    }
    iv_clear_selection(iv);
    iv->items[index].selected = true;
    iv->selected_count = 1;
    iv->anchor = index;
    iv_fire_selection_changed(iv);
}

/* ---- the cell widget ---- */

typedef struct fdk_iv_cell {
    fdk_widget base;
    size_t index;         /* into the owning iconview's items */
} fdk_iv_cell;

static fdk_iv_cell *cell_of(fdk_widget *w) {
    return (fdk_iv_cell *)(void *)w;
}

static void iv_cell_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The cell's owner: grid -> scrollview -> iconview. */
    fdk_widget *grid = w->parent;
    fdk_widget *scroll = grid != NULL ? grid->parent : NULL;
    fdk_widget *iv_w = (scroll != NULL) ? scroll->parent : NULL;
    if (iv_w == NULL || iv_w->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iv_w);
    if (cell_of(w)->index >= iv->count) {
        return;
    }
    fdk_iv_item *item = &iv->items[cell_of(w)->index];

    /* Selected cells: the selection tint + a hairline accent
     * border; the focused view's cursor cell gets the ring. */
    if (item->selected) {
        fdk_surface_fill_rounded_rect(surface, bounds, 6,
                                      fdk__pal_selection());
        fdk_surface_draw_rounded_rect(surface, bounds, 6,
                                      fdk__pal_accent());
    }

    bool disabled = (iv_w->flags & FDK_WF_ENABLED) == 0;

    /* The glyph: centered in the icon band (the 16-px vector
     * language, centered honestly rather than stretched). */
    fdk_i32 icon_band = iv->cell_h - 24; /* label band below */
    if (icon_band < 16) {
        icon_band = 16;
    }
    fdk_i32 gx = bounds.x + (bounds.width - 16) / 2;
    fdk_i32 gy = bounds.y + (icon_band - 16) / 2;
    if (item->icon != FDK_ROW_ICON_NONE) {
        fdk__row_icon_paint(surface, item->icon, gx, gy, disabled);
    }

    /* The label: centered under the glyph, clipped to the cell
     * width (no wrap, no ellipsis — the honest clip). */
    if (iv->font != NULL && item->label != NULL) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(iv->font, item->label, &tw, &th);
        fdk_i32 text_x = bounds.x + (bounds.width - tw) / 2;
        if (text_x < bounds.x + 2) {
            text_x = bounds.x + 2;
        }
        fdk_i32 baseline = fdk__center_baseline(
            iv->font, bounds.y + icon_band, 20);
        fdk__draw_text(surface, iv->font, item->label,
                       disabled ? fdk__pal_text_disabled()
                                : fdk__pal_text(),
                       text_x, baseline);
    }
}

static bool iv_cell_handle_event(fdk_widget *w,
                                 const fdk_widget_event *ev) {
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        /* Resolve our owner (the paint path's chain). */
        fdk_widget *grid = w->parent;
        fdk_widget *scroll = grid != NULL ? grid->parent : NULL;
        fdk_widget *iv_w = (scroll != NULL) ? scroll->parent : NULL;
        if (iv_w == NULL || iv_w->klass != &fdk_iconview_class_def) {
            return false;
        }
        fdk_iconview *iv = iv_of(iv_w);
        size_t index = cell_of(w)->index;
        if (index >= iv->count) {
            return false;
        }
        if (!fdk_widget_has_focus(iv_w)) {
            (void)fdk_widget_focus(iv_w);
        }
        iv_cursor_select(iv, index, ev->pointer.modifiers);
        /* Double-click = activation (1.4.7 — the doc's claim since
         * 1.4.5, now delivered by the cells): the List's predicate,
         * the same press semantics (the second press is on the SAME
         * cell, so the slop dx/dy are 0). */
        fdk_i64 now = iv_now_ms();
        bool is_double =
            iv->have_last_click && iv->last_click_item == index &&
            fdk__window_is_double_click(now, iv->last_click_ms, 0, 0);
        if (is_double) {
            iv->have_last_click = false; /* a triple re-arms */
            if (iv->on_activate != NULL) {
                iv->on_activate(iv_w, index, iv->on_activate_data);
            }
        } else {
            iv->have_last_click = true;
            iv->last_click_item = index;
            iv->last_click_ms = now;
        }
        return true;
    }
    default:
        return false;
    }
}

static void iv_cell_destroy(fdk_widget *w) {
    (void)w; /* the label is the iconview's, not the cell's */
}

/* ---- the cell's a11y (a LIST_ITEM over the owner's data) ---- */

static void iv_cell_a11y_describe(const fdk_widget *w,
                                  fdk_a11y_info *out) {
    const fdk_iv_cell *cell = (const fdk_iv_cell *)(const void *)w;
    /* Owner chain for the label/selected state. */
    const fdk_widget *grid = w->parent;
    const fdk_widget *scroll = grid != NULL ? grid->parent : NULL;
    const fdk_widget *iv_w = (scroll != NULL) ? scroll->parent : NULL;
    if (iv_w == NULL || iv_w->klass != &fdk_iconview_class_def ||
        cell->index >= ((const fdk_iconview *)(const void *)iv_w)->count) {
        return;
    }
    const fdk_iconview *iv = (const fdk_iconview *)(const void *)iv_w;
    const fdk_iv_item *item = &iv->items[cell->index];
    if (item->label != NULL) {
        out->name = fdk__strdup(item->label);
    }
    out->states = FDK_A11Y_VISIBLE | FDK_A11Y_SHOWING |
                  FDK_A11Y_ENABLED;
    if (item->selected) {
        out->states |= FDK_A11Y_SELECTED;
    }
}

static const fdk_a11y_class iv_cell_a11y = {
    .role = FDK_A11Y_ROLE_LIST_ITEM,
    .describe = iv_cell_a11y_describe,
};

static const fdk_widget_class fdk_iv_cell_class_def = {
    .size = sizeof(fdk_iv_cell),
    .name = "iconview-item",
    .handle_event = iv_cell_handle_event,
    .paint = iv_cell_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = iv_cell_destroy,
    .a11y = &iv_cell_a11y,
};

/* ---- the rubber-band sweep (1.4.6) ---- */

/* The sweep's selection pass: the CONTENT-space band rect vs every
 * cell's slot rect — plain intersection, ctrl unions the snapshot. */
static void iv_apply_band(fdk_iconview *iv) {
    if (iv->count == 0) {
        return;
    }
    fdk_i32 off_x = 0, off_y = 0;
    if (iv->scroll != NULL) {
        fdk_scrollview_get_scroll_offset(iv->scroll, &off_x, &off_y);
    }
    /* The moving edge is viewport-space + the CURRENT offset. */
    fdk_f32 now_cx = iv->band_now_x + (fdk_f32)off_x;
    fdk_f32 now_cy = iv->band_now_y + (fdk_f32)off_y;
    fdk_f32 x1 = (iv->band_ax_cx < now_cx) ? iv->band_ax_cx : now_cx;
    fdk_f32 x2 = (iv->band_ax_cx < now_cx) ? now_cx : iv->band_ax_cx;
    fdk_f32 y1 = (iv->band_ay_cy < now_cy) ? iv->band_ay_cy : now_cy;
    fdk_f32 y2 = (iv->band_ay_cy < now_cy) ? now_cy : iv->band_ay_cy;
    /* Widen a hair so an edge-aligned sweep still catches cells. */
    x1 -= 1.0f;
    y1 -= 1.0f;
    x2 += 1.0f;
    y2 += 1.0f;
    for (size_t i = 0; i < iv->count; i++) {
        fdk_i32 sx = 0, sy = 0;
        iv_cell_slot(iv, i, &sx, &sy);
        bool hit = (fdk_f32)sx < x2 &&
                   (fdk_f32)(sx + iv->cell_w) > x1 &&
                   (fdk_f32)sy < y2 &&
                   (fdk_f32)(sy + iv->cell_h) > y1;
        bool want = hit ||
                    (iv->band_ctrl && iv->band_base != NULL &&
                     i < iv->band_base_cap && iv->band_base[i]);
        if (iv->items[i].selected != want) {
            iv->items[i].selected = want;
            if (want) {
                iv->selected_count++;
            } else if (iv->selected_count > 0) {
                iv->selected_count--;
            }
        }
    }
    fdk_widget_invalidate(&iv->base);
}

/* ---- band auto-scroll (1.4.7, grid-shaped) ---- */

static void iv_band_scroll_disarm(fdk_iconview *iv);

/* The repeating tick: scrolls one step toward the pointer's side
 * of the viewport on whichever axis is pinned, then re-applies the
 * selection. The band's coords are viewport-space — the pointer
 * resting at the edge needs no update; the growing scroll offset
 * does the extending (the List's rule, both axes now). Disarms
 * itself when the scroll has nowhere further to go. */
static void iv_band_scroll_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_iconview *iv = user;
    if (!iv->banding || iv->scroll == NULL) {
        iv->band_scroll_timer = NULL;
        return;
    }
    fdk_i32 off_x = 0, off_y = 0;
    fdk_scrollview_get_scroll_offset(iv->scroll, &off_x, &off_y);
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(iv->scroll, &vw, &vh);
    fdk_i32 dx = 0, dy = 0;
    if (iv->band_now_x < (fdk_f32)IV_BAND_EDGE) {
        dx = -IV_BAND_SCROLL_STEP;
    } else if (iv->band_now_x > (fdk_f32)(vw - IV_BAND_EDGE)) {
        dx = IV_BAND_SCROLL_STEP;
    }
    if (iv->band_now_y < (fdk_f32)IV_BAND_EDGE) {
        dy = -IV_BAND_SCROLL_STEP;
    } else if (iv->band_now_y > (fdk_f32)(vh - IV_BAND_EDGE)) {
        dy = IV_BAND_SCROLL_STEP;
    }
    if (dx == 0 && dy == 0) {
        iv_band_scroll_disarm(iv);
        return;
    }
    fdk_i32 want_x = off_x + dx;
    fdk_i32 want_y = off_y + dy;
    if (want_x < 0) {
        want_x = 0;
    }
    if (want_y < 0) {
        want_y = 0;
    }
    /* The scrollview clamps to its own max; read back the truth so
     * the band never claims a scroll that did not happen. */
    (void)fdk_scrollview_scroll_to(iv->scroll, want_x, want_y);
    fdk_i32 new_x = 0, new_y = 0;
    fdk_scrollview_get_scroll_offset(iv->scroll, &new_x, &new_y);
    if (new_x == off_x && new_y == off_y) {
        /* Clamped at the end on BOTH axes: nowhere further. */
        iv_band_scroll_disarm(iv);
        return;
    }
    iv_apply_band(iv);
    fdk_widget_invalidate(&iv->base);
}

/* Arms (once) the auto-scroll timer; a detached tree (no window
 * context) cannot arm it — the band then stays put, honestly. */
static void iv_band_scroll_arm(fdk_iconview *iv) {
    if (iv->band_scroll_timer != NULL || iv->scroll == NULL) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&iv->base));
    if (ctx == NULL) {
        return;
    }
    iv->band_scroll_timer =
        fdk_timer_add(ctx, IV_BAND_SCROLL_MS, true,
                      iv_band_scroll_tick, iv);
}

static void iv_band_scroll_disarm(fdk_iconview *iv) {
    if (iv->band_scroll_timer != NULL) {
        fdk_timer_remove(iv->band_scroll_timer);
        iv->band_scroll_timer = NULL;
    }
}

/* Re-evaluates after every motion: arm while the pointer rests
 * outside the visible band on EITHER axis, disarm when it comes
 * back inside. The band's own coords need no bookkeeping — they
 * are viewport-space already. */
static void iv_band_scroll_track(fdk_iconview *iv) {
    if (iv->scroll == NULL) {
        return;
    }
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(iv->scroll, &vw, &vh);
    if (iv->band_now_x < (fdk_f32)IV_BAND_EDGE ||
        iv->band_now_x > (fdk_f32)(vw - IV_BAND_EDGE) ||
        iv->band_now_y < (fdk_f32)IV_BAND_EDGE ||
        iv->band_now_y > (fdk_f32)(vh - IV_BAND_EDGE)) {
        iv_band_scroll_arm(iv);
    } else {
        iv_band_scroll_disarm(iv);
    }
}

/* ---- the view's keyboard ---- */

static bool iv_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_iconview *iv = iv_of(w);

    /* ---- the rubber-band sweep (1.4.6) ----
     *
     * A left press reaching the VIEW itself is by construction on
     * EMPTY space (cells consume their own presses; the scrollview
     * does not consume presses) — the band start. MULTIPLE mode
     * only; the implicit grab delivers motions + the release. */
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT ||
            iv->mode != FDK_LIST_SELECTION_MULTIPLE ||
            (w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        iv->banding = true;
        iv->band_ctrl =
            (ev->pointer.modifiers & FDK_MOD_CTRL) != 0;
        iv->band_x = iv->band_now_x = ev->pointer.position.x;
        iv->band_y = iv->band_now_y = ev->pointer.position.y;
        /* The anchor is glued to the CONTENT it was pressed on. */
        fdk_i32 ox = 0, oy = 0;
        if (iv->scroll != NULL) {
            fdk_scrollview_get_scroll_offset(iv->scroll, &ox, &oy);
        }
        iv->band_ax_cx = iv->band_x + (fdk_f32)ox;
        iv->band_ay_cy = iv->band_y + (fdk_f32)oy;
        /* Ctrl-union snapshot: the selection as the sweep found it. */
        if (iv->band_ctrl && iv->count > 0) {
            bool *snap = fdk_alloc_array(iv->count, sizeof(bool));
            if (snap != NULL) {
                for (size_t i = 0; i < iv->count; i++) {
                    snap[i] = iv->items[i].selected;
                }
                iv->band_base = snap;
                iv->band_base_cap = iv->count;
            }
        } else {
            fdk_free(iv->band_base);
            iv->band_base = NULL;
            iv->band_base_cap = 0;
        }
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        iv_apply_band(iv);
        /* A press that STARTS at the edge chases immediately (the
         * List's 1.4.3 rule, applied at the gesture's birth). */
        iv_band_scroll_track(iv);
        iv_fire_selection_changed(iv);
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION:
        if (!iv->banding) {
            return false;
        }
        iv->band_now_x = ev->position.x;
        iv->band_now_y = ev->position.y;
        iv_apply_band(iv);
        iv_band_scroll_track(iv);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (!iv->banding) {
            return false;
        }
        iv->banding = false;
        iv_band_scroll_disarm(iv);
        fdk_free(iv->band_base);
        iv->band_base = NULL;
        iv->band_base_cap = 0;
        /* The band rect disappears with the gesture. */
        fdk_widget_invalidate(w);
        /* One gesture: the callback fires here (and at the press —
         * motions never spam it). */
        iv_fire_selection_changed(iv);
        return true;
    default:
        break;
    }

    if (ev->type != FDK_WIDGET_KEY_DOWN) {
        return false;
    }
    if ((w->flags & FDK_WF_FOCUSED) == 0 || iv->count == 0) {
        return false;
    }
    fdk_u32 mods = ev->key.modifiers;
    bool shift = (mods & FDK_MOD_SHIFT) != 0;
    bool ctrl = (mods & FDK_MOD_CTRL) != 0;
    bool cold = (iv->key_cursor == (size_t)-1);
    size_t cursor = cold ? 0 : iv->key_cursor;
    size_t cols = (iv->columns > 0) ? iv->columns : 1;
    size_t next = cursor;
    switch (ev->key.scancode) {
    case FDK_KEY_LEFT:
        next = (cold || cursor > 0) ? (cold ? 0 : cursor - 1) : cursor;
        break;
    case FDK_KEY_RIGHT:
        next = (cold || cursor + 1 < iv->count) ? (cold ? 0 : cursor + 1)
                                                : cursor;
        break;
    case FDK_KEY_UP:
        next = (cold || cursor >= cols) ? (cold ? 0 : cursor - cols)
                                        : cursor;
        break;
    case FDK_KEY_DOWN:
        /* Cold start: the FIRST Down selects item 0 (the List's
         * rule — the keyboard's first landfall), not item 0+cols. */
        next = (cold || cursor + cols < iv->count)
                  ? (cold ? 0 : cursor + cols)
                  : cursor;
        break;
    case FDK_KEY_HOME:
        next = 0;
        break;
    case FDK_KEY_END:
        next = iv->count - 1;
        break;
    case FDK_KEY_ENTER:
        if (iv->on_activate != NULL && cursor < iv->count) {
            iv->on_activate(w, cursor, iv->on_activate_data);
        }
        return true;
    default:
        return false;
    }
    if (next != cursor || iv->key_cursor == (size_t)-1) {
        iv_cursor_select(iv, next, (shift ? FDK_MOD_SHIFT : 0u) |
                                       (ctrl ? FDK_MOD_CTRL : 0u));
        /* Keep the cursor visible: scroll the grid so the cell's
         * slot is in view (the List's discipline). */
        fdk_i32 x = 0, y = 0;
        iv_cell_slot(iv, next, &x, &y);
        fdk_i32 vw = 0, vh = 0;
        fdk__scrollview_viewport(iv->scroll, &vw, &vh);
        fdk_i32 off_x = 0, off_y = 0;
        fdk_scrollview_get_scroll_offset(iv->scroll, &off_x, &off_y);
        if (y < off_y) {
            off_y = y;
        } else if (y + iv->cell_h > off_y + vh) {
            off_y = y + iv->cell_h - vh;
        }
        if (x + iv->cell_w > off_x + vw) {
            off_x = x + iv->cell_w - vw;
        } else if (x < off_x) {
            off_x = x;
        }
        (void)fdk_scrollview_scroll_to(iv->scroll, off_x, off_y);
    }
    return true;
}

/* ---- the view class ---- */

static void iv_view_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_iconview *iv = iv_of(w);
    /* The rubber band (1.4.6), painted UNDER the cells (the view's
     * paint runs before its scrollview subtree): the List's accent
     * tint + border, clamped to the view's bounds. The anchor's
     * on-screen position tracks the CONTENT it is glued to. */
    if (iv->banding) {
        fdk_i32 axo = 0, ayo = 0;
        if (iv->scroll != NULL) {
            fdk_scrollview_get_scroll_offset(iv->scroll, &axo, &ayo);
        }
        fdk_f32 anchor_vx = iv->band_ax_cx - (fdk_f32)axo;
        fdk_f32 anchor_vy = iv->band_ay_cy - (fdk_f32)ayo;
        fdk_f32 x1 = (anchor_vx < iv->band_now_x) ? anchor_vx
                                                  : iv->band_now_x;
        fdk_f32 x2 = (anchor_vx < iv->band_now_x) ? iv->band_now_x
                                                  : anchor_vx;
        fdk_f32 y1 = (anchor_vy < iv->band_now_y) ? anchor_vy
                                                  : iv->band_now_y;
        fdk_f32 y2 = (anchor_vy < iv->band_now_y) ? iv->band_now_y
                                                  : anchor_vy;
        fdk_i32 bx1 = (fdk_i32)x1 + bounds.x;
        fdk_i32 by1 = (fdk_i32)y1 + bounds.y;
        fdk_i32 bx2 = (fdk_i32)x2 + bounds.x;
        fdk_i32 by2 = (fdk_i32)y2 + bounds.y;
        if (bx1 < bounds.x) {
            bx1 = bounds.x;
        }
        if (by1 < bounds.y) {
            by1 = bounds.y;
        }
        if (bx2 > bounds.x + bounds.width) {
            bx2 = bounds.x + bounds.width;
        }
        if (by2 > bounds.y + bounds.height) {
            by2 = bounds.y + bounds.height;
        }
        fdk_i32 bw = bx2 - bx1;
        fdk_i32 bh = by2 - by1;
        if (bw > 0 && bh > 0) {
            fdk_color accent = fdk__pal_accent();
            fdk_rect band = {bx1, by1, bw, bh};
            fdk_color tint = {accent.r, accent.g, accent.b,
                              accent.a * 0.16f};
            fdk_color edge = {accent.r, accent.g, accent.b,
                              accent.a * 0.55f};
            fdk_surface_fill_rect(surface, band, tint);
            fdk_surface_draw_rect(surface, band, edge);
        }
    }
}

static void iv_view_destroy(fdk_widget *w) {
    fdk_iconview *iv = iv_of(w);
    iv_band_scroll_disarm(iv);
    for (size_t i = 0; i < iv->count; i++) {
        fdk_free(iv->items[i].label);
    }
    fdk_free(iv->items);
    fdk_free(iv->band_base);
    iv->band_base = NULL;
}

static void iv_view_a11y_describe(const fdk_widget *w,
                                  fdk_a11y_info *out) {
    const fdk_iconview *iv = (const fdk_iconview *)(const void *)w;
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%zu item%s", iv->count,
                   iv->count == 1 ? "" : "s");
    out->value_text = fdk__strdup(buf);
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = 0.0;
    out->value_current = 0.0;
}

static const fdk_a11y_class iv_view_a11y = {
    .role = FDK_A11Y_ROLE_LIST,
    .describe = iv_view_a11y_describe,
};

const fdk_widget_class fdk_iconview_class_def = {
    .size = sizeof(fdk_iconview),
    .name = "iconview",
    .handle_event = iv_handle_event,
    .paint = iv_view_paint,
    .measure = iv_measure,
    .arrange = iv_arrange,
    .destroy = iv_view_destroy,
    .a11y = &iv_view_a11y,
};

/* ---- mutation helpers ---- */

static fdk_result iv_items_reserve(fdk_iconview *iv, size_t need) {
    if (need <= iv->capacity) {
        return FDK_OK;
    }
    size_t cap = (iv->capacity == 0) ? 8 : iv->capacity;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(*iv->items)) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_iv_item *grown = fdk_realloc(iv->items, cap * sizeof(*grown));
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    iv->items = grown;
    iv->capacity = cap;
    return FDK_OK;
}

static void iv_settle(fdk_iconview *iv) {
    if (iv->batch_depth > 0) {
        iv->batch_dirty = true;
        return;
    }
    iv_relayout(iv);
    fdk_widget_invalidate(&iv->base);
}

/* ---- public API ---- */

fdk_result fdk_iconview_create(fdk_widget *parent, fdk_font *font,
                              fdk_widget **out_iconview) {
    if (out_iconview == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_iconview_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_iconview *iv = iv_of(w);
    iv->font = font;
    iv->mode = FDK_LIST_SELECTION_SINGLE;
    iv->key_cursor = (size_t)-1;
    iv->anchor = (size_t)-1;
    iv->selected_count = 0;
    iv->batch_depth = 0;
    iv->batch_dirty = false;
    /* The themed defaults (LAYOUT metrics — 1.4.6): a theme can
     * re-slot every grid; set_item_size overrides per-widget. */
    iv->cell_w = fdk_theme_get_metric(NULL, FDK_TM_ICONVIEW_CELL_WIDTH);
    iv->cell_h = fdk_theme_get_metric(NULL,
                                      FDK_TM_ICONVIEW_CELL_HEIGHT);
    iv->columns = 1;
    iv->banding = false;
    iv->band_base = NULL;
    iv->band_base_cap = 0;
    fdk_widget_set_can_focus(w, true);

    r = fdk_scrollview_create(w, &iv->scroll);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    r = fdk_widget_create(iv->scroll, NULL, (fdk_rect){0, 0, 0, 0},
                          &iv->grid);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    (void)fdk_scrollview_set_content(iv->scroll, iv->grid);

    fdk_widget_child_layout_changed(w->parent);
    *out_iconview = w;
    return FDK_OK;
}

size_t fdk_iconview_append(fdk_widget *iconview, const char *label,
                           fdk_row_icon icon) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return 0;
    }
    fdk_iconview *iv = iv_of(iconview);
    fdk_result r = iv_items_reserve(iv, iv->count + 1);
    if (!fdk_ok(r)) {
        return 0; /* OOM: append fails, count unchanged */
    }
    char *copy = fdk__strdup(label != NULL ? label : "");
    if (copy == NULL) {
        return 0;
    }
    fdk_widget *cell = NULL;
    r = fdk_widget_create(iv->grid, &fdk_iv_cell_class_def,
                          (fdk_rect){0, 0, iv->cell_w, iv->cell_h},
                          &cell);
    if (!fdk_ok(r)) {
        fdk_free(copy);
        return 0;
    }
    cell_of(cell)->index = iv->count;
    iv->items[iv->count].cell = cell;
    iv->items[iv->count].label = copy;
    iv->items[iv->count].icon = icon;
    iv->items[iv->count].selected = false;
    iv->count++;
    iv_settle(iv);
    fdk__a11y_notify(iconview, FDK_A11Y_CHILDREN_CHANGED, 0);
    return iv->count - 1;
}

size_t fdk_iconview_item_count(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return 0;
    }
    return iv_of(iconview)->count;
}

const char *fdk_iconview_item_label(fdk_widget *iconview, size_t index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return NULL;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return NULL;
    }
    return iv->items[index].label;
}

fdk_row_icon fdk_iconview_item_icon(fdk_widget *iconview, size_t index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return FDK_ROW_ICON_NONE;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return FDK_ROW_ICON_NONE;
    }
    return iv->items[index].icon;
}

fdk_result fdk_iconview_set_item_label(fdk_widget *iconview,
                                       size_t index, const char *label) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = fdk__strdup(label != NULL ? label : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(iv->items[index].label);
    iv->items[index].label = copy;
    iv_settle(iv);
    return FDK_OK;
}

fdk_result fdk_iconview_set_item_icon(fdk_widget *iconview,
                                      size_t index, fdk_row_icon icon) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    iv->items[index].icon = icon;
    iv_settle(iv);
    return FDK_OK;
}

fdk_result fdk_iconview_remove_item(fdk_widget *iconview, size_t index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    bool was_selected = iv->items[index].selected;
    fdk_widget *cell = iv->items[index].cell;
    fdk_free(iv->items[index].label);
    memmove(&iv->items[index], &iv->items[index + 1],
            (iv->count - index - 1) * sizeof(*iv->items));
    iv->count--;
    if (was_selected && iv->selected_count > 0) {
        iv->selected_count--;
    }
    /* The cells after the removed one re-index (their owners are
     * looked up by index at paint/event time). */
    for (size_t i = index; i < iv->count; i++) {
        cell_of(iv->items[i].cell)->index = i;
    }
    if (iv->key_cursor != (size_t)-1) {
        if (iv->key_cursor >= iv->count) {
            iv->key_cursor = (iv->count > 0) ? iv->count - 1
                                             : (size_t)-1;
        } else if (iv->key_cursor > index) {
            iv->key_cursor--;
        }
    }
    fdk_widget_destroy(cell); /* the grid's own child */
    iv_settle(iv);
    fdk__a11y_notify(iconview, FDK_A11Y_CHILDREN_CHANGED, 0);
    return FDK_OK;
}

void fdk_iconview_clear(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    for (size_t i = 0; i < iv->count; i++) {
        fdk_free(iv->items[i].label);
        fdk_widget_destroy(iv->items[i].cell);
    }
    iv->count = 0;
    iv->selected_count = 0;
    iv->key_cursor = (size_t)-1;
    iv_settle(iv);
    fdk__a11y_notify(iconview, FDK_A11Y_CHILDREN_CHANGED, 0);
}

fdk_result fdk_iconview_set_item_size(fdk_widget *iconview,
                                      fdk_i32 width, fdk_i32 height) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def ||
        width < 40 || height < 40 || width > 512 || height > 512) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    iv->cell_w = width;
    iv->cell_h = height;
    iv_settle(iv);
    fdk_widget_child_layout_changed(iconview->parent);
    return FDK_OK;
}

void fdk_iconview_set_selection_mode(fdk_widget *iconview,
                                     fdk_list_selection_mode mode) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    iv->mode = mode;
    if (mode == FDK_LIST_SELECTION_NONE) {
        iv_clear_selection(iv);
    } else if (mode == FDK_LIST_SELECTION_SINGLE &&
               iv->selected_count > 1) {
        /* Keep the first only (the List's honest collapse). */
        bool kept = false;
        for (size_t i = 0; i < iv->count; i++) {
            if (iv->items[i].selected) {
                if (kept) {
                    iv->items[i].selected = false;
                    iv->selected_count--;
                } else {
                    kept = true;
                }
            }
        }
        iv_fire_selection_changed(iv);
    }
    fdk_widget_invalidate(iconview);
}

fdk_i64 fdk_iconview_get_selected(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return -1;
    }
    fdk_iconview *iv = iv_of(iconview);
    for (size_t i = 0; i < iv->count; i++) {
        if (iv->items[i].selected) {
            return (fdk_i64)i;
        }
    }
    return -1;
}

bool fdk_iconview_is_selected(fdk_widget *iconview, size_t index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return false;
    }
    fdk_iconview *iv = iv_of(iconview);
    return index < iv->count && iv->items[index].selected;
}

size_t fdk_iconview_selected_count(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return 0;
    }
    return iv_of(iconview)->selected_count;
}

fdk_result fdk_iconview_selected_at(fdk_widget *iconview, size_t position,
                                    size_t *out_index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def ||
        out_index == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    size_t seen = 0;
    for (size_t i = 0; i < iv->count; i++) {
        if (iv->items[i].selected) {
            if (seen == position) {
                *out_index = i;
                return FDK_OK;
            }
            seen++;
        }
    }
    return FDK_ERR_INVALID_ARGUMENT;
}

fdk_result fdk_iconview_select(fdk_widget *iconview, size_t index) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (index >= iv->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    iv_cursor_select(iv, index, 0);
    fdk_widget_invalidate(iconview);
    return FDK_OK;
}

void fdk_iconview_clear_selection(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    iv_clear_selection(iv);
    fdk_widget_invalidate(iconview);
}

void fdk_iconview_set_on_selection_changed(fdk_widget *iconview,
                                           fdk_list_selection_fn fn,
                                           void *user_data) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    iv->on_selection_changed = fn;
    iv->on_selection_data = user_data;
}

void fdk_iconview_set_on_item_activate(fdk_widget *iconview,
                                       fdk_iconview_item_fn fn,
                                       void *user_data) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    iv->on_activate = fn;
    iv->on_activate_data = user_data;
}

void fdk_iconview_begin_batch(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    iv_of(iconview)->batch_depth++;
}

void fdk_iconview_end_batch(fdk_widget *iconview) {
    if (iconview == NULL || iconview->klass != &fdk_iconview_class_def) {
        return;
    }
    fdk_iconview *iv = iv_of(iconview);
    if (iv->batch_depth > 0) {
        iv->batch_depth--;
    }
    if (iv->batch_depth == 0 && iv->batch_dirty) {
        iv->batch_dirty = false;
        iv_relayout(iv);
        fdk_widget_invalidate(iconview);
    }
}
