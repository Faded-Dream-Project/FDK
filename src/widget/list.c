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

/* Row icon geometry (1.4.1): a 16-px glyph box, 6-px gap to the
 * text — the sidebar-symbolic set. The glyph PAINTERS themselves
 * moved to statics.c in 1.4.2 (fdk__row_icon_advance /
 * fdk__row_icon_paint) so the Tree wears the same language. */
#define LIST_ICON 16
#define LIST_ICON_GAP 6

typedef struct fdk_list_row {
    fdk_widget base;
    bool drag_armed; /* 1.5.0: a selected-row press may arm a drag
                      * source (see fdk_list_row_set_drag_armed)   */
    char **cells;    /* 1.5.0: columns 1..N's text (owned, parallel
                      * to the list's column set; NULL when the list
                      * has no columns)                            */
    size_t cells_cap; /* slots in `cells' — the row frees its own
                       * cells at destroy WITHOUT the list (the
                       * parent chain is already unlinked by then) */
    /* 1.5.0: the pending-band press (the wall-to-wall-rows case).
     * The press on an unselected plain row is remembered, not
     * consumed; when the pointer moves past the slop the ROW
     * forwards the gesture to the LIST as a rubber band (the row
     * keeps the tree grab — it forwards motions, the release, and
     * any grab-broken). A release inside the slop was a CLICK: the
     * row consumes it as the classic select. */
    bool pending_band;
    fdk_f32 pending_x, pending_y;
    char *text;   /* owned */
    bool selected;
    fdk_row_icon icon; /* 1.4.1: the vector glyph before the text */
    fdk_surface *image; /* borrowed raster icon (the icon-theme
                         * engine's cache owns it; NULL = symbolic
                         * only). Paint order: image wins. */
} fdk_list_row;
/* The icon band a row reserves: the same 16+6 whenever EITHER the
 * symbolic glyph or the raster image is present (the raster twin
 * shares the geometry so switching surfaces never re-flows text). */
static fdk_i32 row_icon_band(const fdk_list_row *row) {
    if (row == NULL) {
        return 0;
    }
    return (row->icon != FDK_ROW_ICON_NONE || row->image != NULL)
               ? LIST_ICON + LIST_ICON_GAP
               : 0;
}


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
    size_t selected_count; /* O(1) selection queries + batch diffing */
    fdk_list_selection_fn on_selection_changed;
    void *on_selection_data;
    /* §25/G-3: the bounds-carrying twin. When set, it REPLACES the
     * plain callback (one signal slot). The span accumulates across
     * the multi-row mutations the API performs, so one fire reports
     * every row it touched. */
    fdk_list_selection_bounds_fn on_selection_bounds;
    void *on_selection_bounds_data;
    bool span_active;        /* a mutation is accumulating           */
    size_t span_first, span_last;
    /* ---- Row activation (1.2.0) ----
     *
     * Double-click and Enter both fire on_row_activate — the
     * "open this" gesture file managers and file dialogs are built
     * on. The click tracking uses the shared double-click predicate
     * (same window, same slop policy as the title bar).
     *
     * 1.4.4: SINGLE-CLICK activation mode (GtkListBox's
     * activate_on_single_click / GtkTreeView's setuptools): the
     * places-sidebar rhythm — one click navigates. The double-click
     * branch goes quiet in this mode (the first press already
     * fired; a doubled click must not fire twice). */
    fdk_list_row_activate_fn on_row_activate;
    void *on_row_activate_data;
    fdk_i64 last_click_ms;   /* last left-press, for dbl detection */
    size_t last_click_row;   /* the row that press selected        */
    bool have_last_click;
    bool single_activate;    /* 1.4.4: press fires activation      */
    /* ---- Bulk-mutation batching (1.3.0) ----
     *
     * Between begin_batch/end_batch, row mutations skip the
     * per-mutation O(N) relayout and the selection-changed fire —
     * end_batch runs ONE relayout and (if the selection actually
     * changed) fires ONCE. Bulk loads (a directory fill, a test
     * fixture) become O(N) instead of O(N^2). Depth-counted, so
     * nesting is safe; anything mutated without a batch keeps the
     * exact per-op settle semantics v1 shipped. */
    int batch_depth;
    bool batch_dirty;        /* a mutation awaits the batch relayout */
    size_t batch_sel_at_start; /* selected_count when the batch opened */
    /* Widest row's content width, cached so appends can place just
     * the new row (the common bulk-fill shape) instead of re-placing
     * the whole list. 80 = the v1 minimum content width. */
    fdk_i32 content_width;
    /* ---- List columns (1.5.0) ----------------------------------
     *
     * The file-manager list view: a header strip + per-row cells
     * right of the primary text, all PAINT-TIME geometry on the
     * existing row widgets (no per-cell children — the row count,
     * hit-testing, the implicit grab, and the rubber band are all
     * untouched). columns[0] is the PRIMARY column: a minimum width
     * (row text still widens it) that expands into leftover list
     * width; columns 1..N are fixed-width, right-aligned by
     * default. col_widths is resolved at set_columns time: the
     * declared width, widened to fit the header title, and widened
     * again per cell as rows fill (never narrower than declared).
     * columns_width is the sum of the resolved widths — the FLOOR
     * under content_width so the horizontal scroll reaches the last
     * column's end. */
    fdk_list_column *columns;   /* the set (titles borrowed from
                                  * col_titles — the public struct's
                                  * const field is read-only to
                                  * callers, owned here)            */
    size_t column_count;
    char **col_titles;          /* owned title copies, parallel      */
    fdk_i32 *col_widths;        /* resolved widths, parallel        */
    fdk_i32 columns_width;      /* sum(col_widths) + separators     */
    /* ---- 1.5.1: the ADAPTIVE column geometry (owner round 8) ----
     *
     * The round-7 layout summed widths from the LEFT — the fixed
     * columns clustered right after the shortest filename, nowhere
     * near the window's far end, and never moved on resize. The
     * desktop contract is the opposite: the fixed columns are
     * RIGHT-ANCHORED to the viewport's end, and the name column
     * absorbs all the slack between the names and them.
     *
     * name_nat caches the natural width of the name column's
     * content (widest row text + icon band + padding) so the
     * layout can re-run cheaply on every resize. The resolved
     * geometry lives in col_x[] / col_name_width, recomputed by
     * list_columns_layout() whenever the viewport width changes
     * (the arrange/sync/paint self-heal paths all call it), in one
     * of two modes:
     *
     *   ANCHORED — viewport >= the natural spread (name_nat +
     *   every fixed column + separators): the fixed columns hug
     *   the far end (minus the v-scrollbar's reserved strip), the
     *   name column stretches across the slack. No h-scroll.
     *
     *   CONTENT — viewport too narrow: everything lays out at its
     *   natural width in content space (names left, cells right of
     *   them, exactly one LIST_COL_SEP apart) and content_width
     *   floors at the spread so the horizontal scroll reaches the
     *   last column — narrow windows degrade into a scrollable
     *   pane instead of colliding with the name column.          */
    fdk_i32 name_nat;           /* the name column's natural width   */
    fdk_i32 *col_x;             /* resolved x per column (incl. 0)   */
    fdk_i32 col_name_width;     /* the name column's resolved width  */
    /* ---- Rubber-band drag-select (1.4.2) ----
     *
     * A press on the list's EMPTY space (below the rows / outside
     * the content) starts a sweep-selection band: the rect between
     * the press and the pointer selects every row it intersects,
     * LIVE, recomputed per motion. Plain sweeps REPLACE the
     * selection (an empty-space click clears it — correct list
     * behavior on its own); Ctrl-sweeps UNION over the pre-band
     * selection (the `band_base` snapshot, taken at press). The
     * on_changed callback fires at press and release, not per
     * motion — sweeps are one gesture. MULTIPLE mode only: SINGLE
     * and NONE have no multi-selection to sweep. */
    bool banding;
    bool band_ctrl;      /* the union modifier was held at press     */
    fdk_f32 band_x, band_y;      /* the anchor, list-local           */
    fdk_f32 band_now_x, band_now_y; /* the pointer, list-local      */
    fdk_f32 band_anchor_cy; /* the anchor's CONTENT-space y (1.4.3:
                              * captured at press, glued to the content
                              * — the auto-scroll extension rule: the
                              * moving edge rides the offset, the anchor
                              * does not, so the sweep GROWS as the view
                              * chases the pointer)                  */
    bool *band_base;     /* pre-band snapshot (ctrl) or NULL         */
    size_t band_base_cap;
    fdk_rect band_last_rect; /* round 9: the last painted band rect
                              * (damage-limited invalidation — the
                              * union of old+new repaints the sweep) */
    /* ---- Band auto-scroll (1.4.3) ----
     *
     * When the sweep's pointer leaves the viewport's visible band,
     * a repeating timer scrolls the scrollview toward it. The band
     * coordinates are VIEWPORT-space (apply_band adds the scroll
     * offset when mapping rows), so a stationary pointer needs NO
     * coordinate updates while the content slides under it — the
     * selection simply extends as the offset grows, which is exactly
     * how GTK's sweep chases the viewport edge. Disarmed at the
     * release, on destruction, and when the pointer re-enters the
     * visible band. */
    fdk_timer *band_scroll_timer;
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
    /* 1.4.0: the theme's LIST_ROW_HEIGHT is the FLOOR — the modern
     * airy default (30) wins when the font fits inside it; a font
     * taller than the metric still wins (rows must fit their text;
     * the metric's range tops out at 48). */
    fdk_i32 floor_h =
        fdk_theme_get_metric(NULL, FDK_TM_LIST_ROW_HEIGHT);
    if (h < floor_h) {
        h = floor_h;
    }
    return (h < 16) ? 16 : h;
}

/* Re-places every row inside the rows container + refreshes the
 * container's natural size (so the scrollview's bars/clamps follow).
 * Called after any non-batched mutation (and once at end_batch). */
/* The column geometry (1.5.0). Separators double as the horizontal
 * gutters between columns: PAD on each side of the divider keeps
 * dense right-aligned cells off each other's pixels. */
#define LIST_COL_SEP 24
#define LIST_COL_MIN 48

/* The header strip's height: a text row's height, floored at the
 * row height so the band-aligned geometry stays honest. Paint-only
 * metric; computed per paint (the font can change). */
static fdk_i32 list_header_height(const fdk_list *l) {
    (void)l;
    fdk_i32 h = 0;
    if (l->font != NULL) {
        fdk_i32 tw = 0;
        fdk__text_extent(l->font, "Xg", &tw, &h);
    }
    if (h < 20) {
        h = 20;
    }
    return h + 6;
}

/* 1.5.1: the resolved column geometry — see the struct block. The
 * name column's natural width comes from the widest row (text +
 * icon band + padding, the v1 measure). Must be re-run whenever
 * col_widths, the row content, or the viewport width changes; every
 * such path funnels through here so the two modes are one rule. */
static void list_columns_layout(fdk_list *l) {
    if (l->column_count == 0 || l->col_x == NULL) {
        return;
    }
    /* The name column's natural width: widest row, v1 measure. */
    fdk_i32 name_nat = 80;
    for (size_t i = 0; i < l->count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->row_widgets[i]->text, &tw, &th);
        tw += row_icon_band(l->row_widgets[i]);
        if (tw + LIST_ROW_PAD_X * 2 > name_nat) {
            name_nat = tw + LIST_ROW_PAD_X * 2;
        }
    }
    l->name_nat = name_nat;

    /* The fixed columns' natural spread (their resolved widths +
     * separators). */
    fdk_i32 fixed_nat = 0;
    for (size_t c = 1; c < l->column_count; c++) {
        fixed_nat += LIST_COL_SEP + l->col_widths[c];
    }

    /* The viewport: the scrollview's content width — full width
     * under OVERLAY bars, minus the v-bar's reserved strip under
     * CLASSIC ones (the default). Anchoring INTO the reserved strip
     * would slide the last column under the scrollbar. */
    fdk_i32 vw = (l->scroll != NULL)
                     ? fdk__scrollview_viewport_width(l->scroll)
                     : l->base.bounds.width;
    if (vw <= 0) {
        vw = l->base.bounds.width; /* not arranged yet: the list's
                                    * own width is the best truth */
    }

    if (vw >= name_nat + fixed_nat) {
        /* ANCHORED: the last fixed column's RIGHT edge sits at the
         * viewport's end; the name column takes everything left of
         * the first fixed one. */
        fdk_i32 fixed_w = 0;
        for (size_t c = 1; c < l->column_count; c++) {
            fixed_w += LIST_COL_SEP + l->col_widths[c];
        }
        l->col_x[l->column_count - 1] = vw - l->col_widths[
            l->column_count - 1];
        for (size_t c = l->column_count - 1; c > 1; c--) {
            l->col_x[c - 1] = l->col_x[c] - LIST_COL_SEP -
                              l->col_widths[c - 1];
        }
        if (l->column_count > 1) {
            l->col_x[0] = 0;
            l->col_name_width = l->col_x[1] - LIST_COL_SEP;
        } else {
            l->col_x[0] = 0;
            l->col_name_width = vw;
        }
    } else {
        /* CONTENT: natural widths in content space; the name column
         * keeps its natural width (col_name_width), the fixed
         * columns sit one separator after it / each other. */
        l->col_x[0] = 0;
        l->col_name_width = name_nat;
        fdk_i32 x = name_nat;
        for (size_t c = 1; c < l->column_count; c++) {
            x += LIST_COL_SEP;
            l->col_x[c] = x;
            x += l->col_widths[c];
        }
    }
}

/* The x where column `col` starts, in CONTENT space (list-local +
 * the scroll offset applied by the caller). Column 0 starts at 0
 * (it IS the primary text column; its icon band and padding are the
 * row's own). 1.5.1: reads the RESOLVED geometry — anchored or
 * content mode, whatever the last list_columns_layout() decided. */
static fdk_i32 list_col_x(const fdk_list *l, size_t col) {
    if (col < l->column_count && l->col_x != NULL) {
        return l->col_x[col];
    }
    return 0;
}

/* The resolved width floor for a fixed column: its declared width,
 * widened to fit its header title (+ padding) — cell text widens it
 * further per row (see list_row_set_cell). */
static fdk_i32 list_col_title_width(const fdk_list *l, size_t col) {
    fdk_i32 w = l->columns[col].min_width;
    if (l->font != NULL && l->columns[col].title != NULL) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->columns[col].title, &tw, &th);
        if (tw + LIST_ROW_PAD_X * 2 > w) {
            w = tw + LIST_ROW_PAD_X * 2;
        }
    }
    return w;
}

/* Recomputes columns_width from the resolved widths. The PRIMARY
 * column contributes its current width floor — the row text can
 * exceed every declared width, and content_width carries that;
 * columns_width only has to FLOOR the scrollable content. */
static void list_columns_recompute(fdk_list *l) {
    fdk_i32 w = 0;
    for (size_t i = 0; i < l->column_count; i++) {
        if (i > 0) {
            w += LIST_COL_SEP;
        }
        w += l->col_widths[i];
    }
    l->columns_width = w;
}

/* The scrollview's inner bounds for the CURRENT list bounds: the
 * full list, or inset below the header strip when columns are set.
 * Every site that self-syncs the scrollview goes through here so
 * the header pinning is one rule, not four drifting copies. */
static void list_sync_scroll_bounds(fdk_list *l) {
    if (l->scroll == NULL || l->base.bounds.width <= 0 ||
        l->base.bounds.height <= 0) {
        return;
    }
    fdk_i32 top = 0;
    if (l->column_count > 0) {
        top = list_header_height(l);
        if (top >= l->base.bounds.height) {
            top = l->base.bounds.height - 1;
            if (top < 0) {
                top = 0;
            }
        }
    }
    fdk_rect inner = { 0, top, l->base.bounds.width,
                       l->base.bounds.height - top };
    fdk_widget_set_bounds(l->scroll, inner);
    /* 1.5.1: the viewport just moved (resize, or the list's first
     * arrangement) — the right-anchored columns re-resolve here.
     * This is the RESIZE ADAPTATION: every path that changes the
     * scrollview's bounds (arrange, the app's set_bounds, the
     * self-heal in paint) funnels through this sync, so the anchor
     * follows the window live. */
    if (l->column_count > 0) {
        list_columns_layout(l);
        fdk_i32 spread = l->col_x[l->column_count - 1] +
                         l->col_widths[l->column_count - 1];
        if (spread > l->content_width && l->rows != NULL) {
            l->content_width = spread;
            fdk_i32 rh = list_row_height(l);
            for (size_t i = 0; i < l->count; i++) {
                fdk_rect r = { 0, (fdk_i32)(i * (size_t)rh),
                               l->content_width, rh };
                fdk_widget_set_bounds(&l->row_widgets[i]->base, r);
            }
            fdk_widget_set_natural_size(
                l->rows, l->content_width,
                (fdk_i32)(l->count * (size_t)rh));
        }
    }
}

static void list_relayout(fdk_list *l) {
    if (l->rows == NULL) {
        return;
    }
    fdk_i32 rh = list_row_height(l);
    fdk_i32 w = 80;
    /* Width: the widest row's text (min 80) — rows stretch to it.
     * Iconed rows account for the glyph box + gap (1.4.1). */
    for (size_t i = 0; i < l->count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->row_widgets[i]->text, &tw, &th);
        tw += row_icon_band(l->row_widgets[i]);
        if (tw + LIST_ROW_PAD_X * 2 > w) {
            w = tw + LIST_ROW_PAD_X * 2;
        }
    }
    /* 1.5.1: the adaptive column geometry runs FIRST — the
     * content-mode floor below reads the resolved col_x[] tail. */
    if (l->column_count > 0) {
        list_columns_layout(l);
        fdk_i32 spread = l->col_x[l->column_count - 1] +
                         l->col_widths[l->column_count - 1];
        if (spread > w) {
            w = spread; /* content mode: the h-scroll must reach
                         * the last column's right edge */
        }
    }
    l->content_width = w;
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
    list_sync_scroll_bounds(l);
    fdk_widget_child_layout_changed(l->rows->parent);
}

/* The append fast path: place ONLY the new row. O(1) when the new
 * row is not wider than every row before it (the common bulk-fill
 * shape — filenames, log lines); a widening append re-places the
 * list once (widths stretch). The batched fill path never even gets
 * here (end_batch runs the single full relayout). */
static void list_place_appended(fdk_list *l) {
    if (l->rows == NULL || l->count == 0) {
        return;
    }
    fdk_i32 rh = list_row_height(l);
    fdk_list_row *row = l->row_widgets[l->count - 1];
    fdk_i32 tw = 0;
    fdk__text_extent(l->font, row->text, &tw, NULL);
    fdk_i32 want = tw + LIST_ROW_PAD_X * 2 + row_icon_band(row);
    if (want > l->content_width) {
        l->content_width = want;
        /* Widths stretch for every row: re-place the list. */
        for (size_t i = 0; i < l->count; i++) {
            fdk_rect r = { 0, (fdk_i32)(i * (size_t)rh),
                           l->content_width, rh };
            fdk_widget_set_bounds(&l->row_widgets[i]->base, r);
        }
    } else {
        fdk_rect r = { 0, (fdk_i32)((l->count - 1) * (size_t)rh),
                       l->content_width, rh };
        fdk_widget_set_bounds(&row->base, r);
    }
    /* 1.5.1: a widening append can flip content mode (the name
     * column's natural width grew) — re-resolve the anchor before
     * the natural size lands. */
    if (l->column_count > 0) {
        list_columns_layout(l);
        fdk_i32 spread = l->col_x[l->column_count - 1] +
                         l->col_widths[l->column_count - 1];
        if (spread > l->content_width) {
            l->content_width = spread;
            for (size_t i = 0; i < l->count; i++) {
                fdk_rect r = { 0, (fdk_i32)(i * (size_t)rh),
                               l->content_width, rh };
                fdk_widget_set_bounds(&l->row_widgets[i]->base, r);
            }
        }
    }
    fdk_widget_set_natural_size(l->rows, l->content_width,
                                (fdk_i32)(l->count * (size_t)rh));
    list_sync_scroll_bounds(l);
    fdk_widget_child_layout_changed(l->rows->parent);
}

/* ---- selection plumbing ---- */

static void list_fire_changed(fdk_list *l) {
    if (l->on_selection_bounds != NULL) {
        fdk_list_selection_bounds b;
        b.valid = l->span_active;
        b.first = l->span_active ? l->span_first : 0;
        b.last = l->span_active ? l->span_last : 0;
        l->on_selection_bounds(&l->base, &b,
                               l->on_selection_bounds_data);
    } else if (l->on_selection_changed != NULL) {
        l->on_selection_changed(&l->base, l->on_selection_data);
    }
    l->span_active = false; /* the span is per-fire                */
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
        if (selected) {
            l->selected_count++;
        } else {
            l->selected_count--;
        }
        fdk_widget_invalidate(&row->base);
        /* A11y: the row's selected state flipped. */
        fdk__a11y_notify(&row->base, FDK_A11Y_STATE_CHANGED,
                         FDK_A11Y_SELECTED);
        /* §25/G-3: widen the accumulating change span. */
        if (!l->span_active) {
            l->span_active = true;
            l->span_first = index;
            l->span_last = index;
        } else {
            if (index < l->span_first) {
                l->span_first = index;
            }
            if (index > l->span_last) {
                l->span_last = index;
            }
        }
    }
}

/* Clears every selected row. Reentrancy-safe: an a11y subscriber
 * may destroy the LIST mid-sweep (notifications run app code), so
 * the loop re-checks the watch every iteration. */
static void list_clear_selection(fdk_list *l) {
    if (l->selected_count == 0) {
        return;
    }
    fdk_widget_watch watch;
    fdk__widget_watch(&watch, &l->base);
    for (size_t i = 0; i < l->count; i++) {
        if (!fdk__watch_alive(&watch)) {
            break;
        }
        list_set_row(l, i, false);
    }
    fdk__widget_unwatch(&watch);
}

size_t fdk_list_selected_count(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return 0;
    }
    /* Maintained incrementally by list_set_row — O(1), and the
     * honest count even mid-batch. */
    return list_of(list)->selected_count;
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

fdk_result fdk_list_deselect(fdk_widget *list, size_t row) {
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
    if (l->row_widgets[row]->selected) {
        list_set_row(l, row, false);
        /* the anchor/cursor survive a deselect they do not point at;
         * one pointing AT the row moves to it conceptually (the next
         * shift-click re-anchors anyway) — no state to fix here */
        list_fire_changed(l);
    }
    return FDK_OK; /* the no-op contract: no signal when unchanged */
}

fdk_result fdk_list_select_range(fdk_widget *list, size_t first,
                                 size_t last, bool additive) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (l->mode == FDK_LIST_SELECTION_NONE) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (l->count == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* clamp + order */
    size_t lo = first, hi = last;
    if (lo > hi) {
        size_t t = lo;
        lo = hi;
        hi = t;
    }
    if (lo >= l->count) {
        lo = l->count - 1; /* both ends past the end: the last row */
    }
    if (hi >= l->count) {
        hi = l->count - 1;
    }
    if (!additive) {
        list_clear_selection(l);
    }
    for (size_t i = lo; i <= hi; i++) {
        list_set_row(l, i, true);
    }
    l->anchor = lo;
    l->key_cursor = hi;
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
    /* The owning list: rows -> scrollview -> list (the chain the
     * row's own DOWN handler walks). The old one-liner stopped at
     * the scrollview and cast IT — harmless while nothing read it
     * ((void)l), fatal the day the columns code did. */
    fdk_list *l = NULL;
    for (fdk_widget *cur = w->parent; cur != NULL;
         cur = cur->parent) {
        if (cur->klass == &fdk_list_class_def) {
            l = list_of(cur);
            break;
        }
    }

    fdk_color fill;
    if (row->selected) {
        /* The 1.3.2 SELECTION_BACKGROUND token, wired in 1.4.0 (the
         * hardcoded accent-with-0.45-alpha is gone). */
        fill = fdk__pal_selection();
        fdk_surface_fill_rect(surface, bounds, fill);
    } else if ((w->flags & FDK_WF_HOVERED) != 0 &&
               (w->flags & FDK_WF_ENABLED) != 0) {
        /* 1.4.0: the soft ROW_HOVER fill (subtler than a control
         * hover — dense lists must not strobe under the pointer). */
        fdk_surface_fill_rect(surface, bounds, fdk__pal_row_hover());
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
    fdk_i32 text_x = bounds.x + LIST_ROW_PAD_X;
    /* The symbolic glyph before the text (1.4.1): vertically
     * centered in its own 16-px box. */
    if (row->image != NULL || row->icon != FDK_ROW_ICON_NONE) {
        fdk_i32 gy = bounds.y + (bounds.height - LIST_ICON) / 2;
        if (gy < bounds.y) {
            gy = bounds.y;
        }
        if (row->image != NULL) {
            /* The icon-theme engine's raster icon, scaled into the
             * same 16-px band the symbolic glyphs occupy. */
            fdk__row_icon_paint_image(surface, row->image, text_x, gy,
                                      LIST_ICON);
        } else {
            fdk__row_icon_paint(surface, row->icon, text_x, gy,
                       (w->flags & FDK_WF_ENABLED) == 0);
        }
        text_x += LIST_ICON + LIST_ICON_GAP;
    }
    fdk__draw_text(surface, font, row->text, fdk__pal_text(),
                   text_x, baseline);

    /* 1.5.0 columns: the fixed cells right of the primary text,
     * drawn at the column offsets in the ROW's local space (the row
     * spans the full content width, so content x == row x). Cells
     * render in the muted text color — the primary text stays the
     * row's identity, the cells are metadata. */
    if (l != NULL && l->column_count > 1 && row->cells != NULL) {
        fdk_i32 off_x = 0, off_y = 0;
        if (l->scroll != NULL) {
            fdk_scrollview_get_scroll_offset(l->scroll, &off_x,
                                             &off_y);
        }
        for (size_t c = 1; c < l->column_count; c++) {
            const char *cell = row->cells[c - 1];
            if (cell == NULL || cell[0] == '\0') {
                continue;
            }
            fdk_i32 cx = list_col_x(l, c) - off_x;
            fdk_i32 cw = l->col_widths[c];
            fdk_i32 tw2 = 0, th2 = 0;
            fdk__text_extent(font, cell, &tw2, &th2);
            fdk_i32 cell_x;
            if (l->columns[c].align_left) {
                cell_x = bounds.x + cx;
            } else {
                cell_x = bounds.x + cx + (cw - tw2) - LIST_ROW_PAD_X;
                if (cell_x < bounds.x + cx) {
                    cell_x = bounds.x + cx;
                }
            }
            fdk__draw_text(surface, font, cell, fdk__pal_text_disabled(),
                           cell_x, baseline);
        }
    }
}

static bool list_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev);

/* 1.5.0: the row-press-to-band slop (px, root space) — a press on
 * an unselected row becomes a rubber band only past this distance;
 * inside it, the release is the classic click-select. */
#define BAND_SLOP 6

/* list-local absolute-origin twin (widget.c's abs_pos is static to
 * this translation unit's sibling) — the row-to-list re-localizer
 * below needs both widgets' absolute origins. */
static fdk_point row_abs_pos(const fdk_widget *w) {
    fdk_i32 x = 0, y = 0;
    for (const fdk_widget *cur = w; cur->parent != NULL;
         cur = cur->parent) {
        x += cur->bounds.x;
        y += cur->bounds.y;
    }
    return (fdk_point){x, y};
}

/* The row's INTERACTIVE band (the round-9 owner report): a row
 * counts as a row only over its CONTENT — the icon band plus the
 * name text plus the row's own padding. A press to the RIGHT of
 * that is EMPTY LIST SPACE: the row declines it, the hit-test walk
 * falls through to the list, and the rubber band starts there,
 * exactly like the space below the rows. (The row's hit rect stays
 * full-width for painting — selection/hover fills span the row and
 * the 1.5.x column cells ride the same expanse — but presses
 * outside the interactive band are not this row's.) */
static bool row_press_on_content(const fdk_list *l,
                                 const fdk_list_row *row,
                                 fdk_f32 local_x) {
    if (row == NULL) {
        return false;
    }
    fdk_i32 tw = 0;
    fdk_i32 th = 0;
    if (l->font != NULL && row->text != NULL) {
        fdk__text_extent(l->font, row->text, &tw, &th);
    }
    fdk_i32 band =
        LIST_ROW_PAD_X + row_icon_band(row) + tw + LIST_ROW_PAD_X;
    return local_x < (fdk_f32)band;
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
                /* The interactive band: icon + name text only. A
                 * press right of the content is empty list space —
                 * the row declines it, the hit-test walk falls to
                 * the list, and the rubber band starts there. */
                if (!row_press_on_content(l, l->row_widgets[i],
                                          ev->pointer.position.x)) {
                    return false;
                }
                /* MULTIPLE-mode press on an UNSELECTED row with no
                 * range modifier is a POTENTIAL rubber band (1.5.0):
                 * a full directory view is wall-to-wall rows — there
                 * is no empty space to start a sweep from, and the
                 * press users actually make is ON a row (the round-6
                 * report: the sweep was unreachable in practice).
                 * Remember it; a sweep past the slop hands the
                 * gesture to the list's band, a release inside the
                 * slop is the classic click-select. Selected rows
                 * keep the arm-a-drag press; modifier presses keep
                 * their range semantics. */
                fdk_list_row *row = l->row_widgets[i];
                if (l->mode == FDK_LIST_SELECTION_MULTIPLE &&
                    !row->selected && !row->drag_armed &&
                    (ev->pointer.modifiers &
                     (FDK_MOD_CTRL | FDK_MOD_SHIFT)) == 0) {
                    row->pending_band = true;
                    row->pending_x = ev->pointer.position.x;
                    row->pending_y = ev->pointer.position.y;
                    return true; /* consumed: the ROW owns the press
                                  * now (click or band, its call) */
                }
                list_row_clicked(l, i, ev->pointer.modifiers);
                /* Double-click = activation (1.2.0). Same row, same
                 * press semantics as the title bar's dblclick window
                 * (fdk__window_is_double_click; dx/dy are 0 because
                 * the SECOND click is on the same row widget — the
                 * row IS the slop region here). */
                fdk_i64 now = list_now_ms();
                bool is_double =
                    l->have_last_click && l->last_click_row == i &&
                    fdk__window_is_double_click(now, l->last_click_ms,
                                                0, 0);
                if (is_double) {
                    l->have_last_click = false; /* a triple click
                                                   re-arms from zero */
                    if (!l->single_activate) {
                        list_fire_activated(l, i);
                    }
                } else {
                    l->have_last_click = true;
                    l->last_click_row = i;
                    l->last_click_ms = now;
                    if (l->single_activate) {
                        /* The sidebar rhythm (1.4.4): ONE click
                         * navigates. */
                        list_fire_activated(l, i);
                    }
                }
                return true;
            }
        }
        return false;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        fdk_list *l = NULL;
        fdk_widget *list_w = NULL;
        for (fdk_widget *cur = w->parent; cur != NULL;
             cur = cur->parent) {
            if (cur->klass == &fdk_list_class_def) {
                list_w = cur;
                break;
            }
        }
        if (list_w != NULL) {
            l = list_of(list_w);
        }
        /* pending press + past the slop: the press becomes the
         * band's anchor. The band's coordinates are LIST-local; the
         * press's were row-local — the same absolute point, so
         * translating is just the row's origin within the list. */
        if (l != NULL && row_of(w)->pending_band) {
            fdk_point origin_row = row_abs_pos(w);
            fdk_point origin_list = row_abs_pos(list_w);
            fdk_f32 dx = (fdk_f32)(origin_row.x - origin_list.x);
            fdk_f32 dy = (fdk_f32)(origin_row.y - origin_list.y);
            fdk_f32 mx = ev->position.x + dx;
            fdk_f32 my = ev->position.y + dy;
            fdk_f32 px = row_of(w)->pending_x + dx;
            fdk_f32 py = row_of(w)->pending_y + dy;
            fdk_f32 ddx = mx - px, ddy = my - py;
                if (ddx * ddx + ddy * ddy > BAND_SLOP * BAND_SLOP) {
                row_of(w)->pending_band = false;
                /* synthesize the band's press at the LIST */
                fdk_widget_event press;
                memset(&press, 0, sizeof press);
                press.type = FDK_WIDGET_POINTER_DOWN;
                press.widget = list_w;
                press.pointer.button = FDK_POINTER_BUTTON_LEFT;
                press.pointer.position.x = px;
                press.pointer.position.y = py;
                (void)list_handle_event(list_w, &press);
                /* then the motion that tripped the slop */
                fdk_widget_event fwd = *ev;
                fwd.widget = list_w;
                fwd.position.x = mx;
                fwd.position.y = my;
                return list_handle_event(list_w, &fwd);
            }
            return true; /* inside the slop: still maybe a click */
        }
        /* a live band owned by the list: forward the motion */
        if (l != NULL && l->banding) {
            fdk_point origin_row = row_abs_pos(w);
            fdk_point origin_list = row_abs_pos(list_w);
            fdk_widget_event fwd = *ev;
            fwd.widget = list_w;
            fwd.position.x =
                ev->position.x + (fdk_f32)(origin_row.x - origin_list.x);
            fwd.position.y =
                ev->position.y + (fdk_f32)(origin_row.y - origin_list.y);
            return list_handle_event(list_w, &fwd);
        }
        return false;
    }
    case FDK_WIDGET_POINTER_UP: {
        fdk_widget *list_w = NULL;
        for (fdk_widget *cur = w->parent; cur != NULL;
             cur = cur->parent) {
            if (cur->klass == &fdk_list_class_def) {
                list_w = cur;
                break;
            }
        }
        fdk_list *l = (list_w != NULL) ? list_of(list_w) : NULL;
        /* pending press released inside the slop: it was a CLICK —
         * the classic select (the press was consumed by the row, so
         * the click logic runs NOW, at the release). */
        if (l != NULL && row_of(w)->pending_band) {
            row_of(w)->pending_band = false;
            fdk_list_row *row = row_of(w);
            for (size_t i = 0; i < l->count; i++) {
                if (l->row_widgets[i] == row) {
                    list_row_clicked(l, i, 0);
                    return true;
                }
            }
            return true;
        }
        /* a live band: forward the release (the gesture's end) */
        if (l != NULL && l->banding) {
            fdk_point origin_row = row_abs_pos(w);
            fdk_point origin_list = row_abs_pos(list_w);
            fdk_widget_event fwd = *ev;
            fwd.widget = list_w;
            fwd.pointer = ev->pointer;
            fwd.pointer.position.x +=
                (fdk_f32)(origin_row.x - origin_list.x);
            fwd.pointer.position.y +=
                (fdk_f32)(origin_row.y - origin_list.y);
            return list_handle_event(list_w, &fwd);
        }
        return false;
    }
    case FDK_WIDGET_GRAB_BROKEN: {
        fdk_widget *list_w = NULL;
        for (fdk_widget *cur = w->parent; cur != NULL;
             cur = cur->parent) {
            if (cur->klass == &fdk_list_class_def) {
                list_w = cur;
                break;
            }
        }
        fdk_list *l = (list_w != NULL) ? list_of(list_w) : NULL;
        row_of(w)->pending_band = false;
        if (l != NULL && l->banding) {
            fdk_widget_event fwd = *ev;
            fwd.widget = list_w;
            return list_handle_event(list_w, &fwd);
        }
        return true;
    }
    default:
        break;
    }
    return false;
}

static void row_destroy(fdk_widget *w) {
    fdk_list_row *row = row_of(w);
    fdk_free(row->text);
    if (row->cells != NULL) {
        /* The cells array is ALL NULL-terminated string slots —
         * freeing needs no column count: destroy unlinks the widget
         * from its parent AT ENTRY (the tree walker's rule), so the
         * parent-chain walk to the owning list is ALREADY BROKEN
         * here (the round-7 rig caught exactly that: cells leaked
         * on every rebuild). The array's allocation size is the
         * slot count; a NULL slot is the terminator-free marker —
         * walk until the allocation's end is unknown... it is NOT:
         * so free EVERY non-NULL slot by capacity, which
         * insert_internal recorded as column_count-1 at alloc time.
         * Since the list is unreachable, remember the capacity ON
         * THE ROW (cells_cap) — set at alloc and at set_columns. */
        for (size_t i = 0; i < row->cells_cap; i++) {
            fdk_free(row->cells[i]);
        }
        fdk_free(row->cells);
    }
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

/* ---- list-level events (keyboard + the 1.4.2 rubber band) ---- */

static void list_apply_band(fdk_list *l);
static void list_band_scroll_disarm(fdk_list *l);

/* The band auto-scroll constants (1.4.3): tick rate, step, and the
 * edge zone that arms it. */
#define LIST_BAND_SCROLL_MS 40
#define LIST_BAND_SCROLL_STEP 24
#define LIST_BAND_EDGE 16

/* The repeating tick: scrolls one step toward the pointer's side of
 * the viewport and re-applies the selection. The band's coords are
 * viewport-space — the pointer resting at the edge needs no update;
 * the growing scroll offset does the extending. Disarms itself when
 * the scroll has nowhere further to go. */
static void list_band_scroll_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_list *l = user;
    if (!l->banding || l->scroll == NULL) {
        l->band_scroll_timer = NULL;
        return;
    }
    fdk_i32 off_x = 0, off_y = 0;
    fdk_scrollview_get_scroll_offset(l->scroll, &off_x, &off_y);
    (void)off_x;
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(l->scroll, &vw, &vh);
    (void)vw;
    /* The pointer's viewport-space y decides the direction. */
    fdk_i32 dy = 0;
    if (l->band_now_y < (fdk_f32)LIST_BAND_EDGE) {
        dy = -LIST_BAND_SCROLL_STEP;
    } else if (l->band_now_y > (fdk_f32)(vh - LIST_BAND_EDGE)) {
        dy = LIST_BAND_SCROLL_STEP;
    }
    if (dy == 0) {
        list_band_scroll_disarm(l);
        return;
    }
    fdk_i32 want = off_y + dy;
    if (want < 0) {
        want = 0;
    }
    /* The scrollview clamps to its own max; read back the truth so
     * the band never claims a scroll that did not happen. */
    (void)fdk_scrollview_scroll_to(l->scroll, 0, want);
    fdk_i32 new_off_x = 0, new_off_y = 0;
    fdk_scrollview_get_scroll_offset(l->scroll, &new_off_x, &new_off_y);
    (void)new_off_x;
    if (new_off_y == off_y) {
        /* Clamped at the end: nowhere further — stop chasing. */
        list_band_scroll_disarm(l);
        return;
    }
    /* Viewport-space band + a bigger offset = a wider selection
     * sweep; the band rect itself never moves on screen. */
    list_apply_band(l);
    fdk_widget_invalidate(&l->base);
}

/* Arms (once) the auto-scroll timer; a tree without a context (no
 * window) cannot arm it — the band then stays put, honestly. */
static void list_band_scroll_arm(fdk_list *l) {
    if (l->band_scroll_timer != NULL || l->scroll == NULL) {
        return;
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(&l->base));
    if (ctx == NULL) {
        return;
    }
    l->band_scroll_timer =
        fdk_timer_add(ctx, LIST_BAND_SCROLL_MS, true,
                      list_band_scroll_tick, l);
}

static void list_band_scroll_disarm(fdk_list *l) {
    if (l->band_scroll_timer != NULL) {
        fdk_timer_remove(l->band_scroll_timer);
        l->band_scroll_timer = NULL;
    }
}

/* Re-evaluates the auto-scroll after every motion: arm while the
 * pointer rests outside the visible band, disarm when it comes back
 * inside. The band's own coordinates need no bookkeeping — they are
 * viewport-space already. */
static void list_band_scroll_track(fdk_list *l) {
    if (l->scroll == NULL) {
        return;
    }
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(l->scroll, &vw, &vh);
    (void)vw;
    if (l->band_now_y < (fdk_f32)LIST_BAND_EDGE ||
        l->band_now_y > (fdk_f32)(vh - LIST_BAND_EDGE)) {
        list_band_scroll_arm(l);
    } else {
        list_band_scroll_disarm(l);
    }
}

static bool list_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_list *l = list_of(w);

    /* ---- the rubber-band sweep (1.4.2) ----
     *
     * A left press reaching the LIST itself is by construction on
     * EMPTY space (rows consume their own presses; the scrollview
     * does not consume presses) — the classic band start. MULTIPLE
     * mode only; the implicit grab (press returned true) delivers
     * the motions and the release to the list alone. */
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT ||
            l->mode != FDK_LIST_SELECTION_MULTIPLE ||
            (w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        l->banding = true;
        l->band_ctrl = (ev->pointer.modifiers & FDK_MOD_CTRL) != 0;
        l->band_x = l->band_now_x = ev->pointer.position.x;
        l->band_y = l->band_now_y = ev->pointer.position.y;
        /* The anchor is glued to the CONTENT it was pressed on: the
         * auto-scroll rule (the sweep grows as the view moves; the
         * anchor row stays the anchor). */
        {
            fdk_i32 ox = 0, oy = 0;
            if (l->scroll != NULL) {
                fdk_scrollview_get_scroll_offset(l->scroll, &ox, &oy);
            }
            l->band_anchor_cy = l->band_y + (fdk_f32)oy;
        }
        /* Ctrl-union snapshot: the selection as the sweep found it. */
        if (l->band_ctrl && l->count > 0) {
            bool *snap = fdk_alloc_array(l->count, sizeof(bool));
            if (snap != NULL) {
                for (size_t i = 0; i < l->count; i++) {
                    snap[i] = l->row_widgets[i]->selected;
                }
                l->band_base = snap;
                l->band_base_cap = l->count;
            }
        } else {
            fdk_free(l->band_base);
            l->band_base = NULL;
            l->band_base_cap = 0;
        }
        /* A zero-extent band selects nothing: the plain sweep clears
         * (an empty-space click deselecting — correct on its own);
         * the ctrl sweep keeps the snapshot. A press that STARTS at
         * the edge chases immediately (the motion path's rule,
         * applied at the gesture's birth). */
        l->band_last_rect = (fdk_rect){0, 0, 0, 0}; /* a fresh
                                                     * gesture owes
                                                     * no old-rect
                                                     * damage     */
        list_apply_band(l);
        list_band_scroll_track(l);
        list_fire_changed(l);
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION:
        if (!l->banding) {
            return false;
        }
        l->band_now_x = ev->position.x;
        l->band_now_y = ev->position.y;
        list_apply_band(l);
        list_band_scroll_track(l);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (!l->banding) {
            return false;
        }
        l->banding = false;
        list_band_scroll_disarm(l);
        fdk_free(l->band_base);
        l->band_base = NULL;
        l->band_base_cap = 0;
        /* The band rect disappears with the gesture: repaint. */
        fdk_widget_invalidate(w);
        /* The sweep is ONE gesture: the callback fires here (and it
         * fired at the press — motions never spam it). */
        list_fire_changed(l);
        return true;
    case FDK_WIDGET_GRAB_BROKEN:
        /* The pointer was handed to a backend gesture (a drag began
         * mid-sweep — the round-6 report): end the band NOW, or the
         * auto-scroll timer outlives the gesture and the stale
         * banding flag captures the next press (the frozen-window
         * symptom). Same cleanup as the release. */
        if (l->banding) {
            l->banding = false;
            list_band_scroll_disarm(l);
            fdk_free(l->band_base);
            l->band_base = NULL;
            l->band_base_cap = 0;
            fdk_widget_invalidate(w);
        }
        return true;
    default:
        break;
    }

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
    list_band_scroll_disarm(l);
    if (l->col_titles != NULL) {
        for (size_t i = 0; i < l->column_count; i++) {
            fdk_free(l->col_titles[i]);
        }
    }
    fdk_free(l->col_titles);
    l->col_titles = NULL;
    fdk_free(l->columns);
    l->columns = NULL;
    fdk_free(l->col_widths);
    l->col_widths = NULL;
    fdk_free(l->col_x);
    l->col_x = NULL;
    l->column_count = 0;
    fdk_free(l->row_widgets);
    l->row_widgets = NULL;
    l->count = 0;
    l->capacity = 0;
    fdk_free(l->band_base); /* a band interrupted by destruction */
    l->band_base = NULL;
    l->band_base_cap = 0;
    l->banding = false;
}

static void list_measure(fdk_widget *w, fdk_size *out) {
    fdk_list *l = list_of(w);
    /* Natural: up to 8 rows tall, wide enough for the widest row
     * (icons account for their glyph box + gap, 1.4.1). */
    fdk_i32 rh = list_row_height(l);
    fdk_i32 width = 80;
    for (size_t i = 0; i < l->count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, l->row_widgets[i]->text, &tw, &th);
        tw += row_icon_band(l->row_widgets[i]);
        if (tw + LIST_ROW_PAD_X * 2 > width) {
            width = tw + LIST_ROW_PAD_X * 2;
        }
    }
    size_t shown = (l->count < 8) ? l->count : 8;
    if (l->column_count > 0) {
        /* 1.5.1: the natural size is the CONTENT-mode spread — the
         * anchored mode's whole point is that it fits the viewport,
         * so the natural (unanchored) answer is the fallback one. */
        list_columns_layout(l);
        fdk_i32 spread = l->col_x[l->column_count - 1] +
                         l->col_widths[l->column_count - 1];
        if (spread > width) {
            width = spread;
        }
    }
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
        list_sync_scroll_bounds(l);
        fdk_widget_child_layout_changed(l->scroll);
    }
}

/* Applies the band's live selection: every row intersecting the
 * band's content-space extent (viewport y + scroll offset), unioned
 * with the pre-band snapshot when Ctrl armed the sweep. Rows flip
 * through list_set_row so the a11y notifications ride the same
 * path shift-range clicks take. */
/* The band's LIST-LOCAL rect (round 9): the same geometry the paint
 * path draws — x from the press/pointer pair, y from the anchor's
 * on-screen position (content-glued) and the pointer, clamped to the
 * list's bounds. A zero rect means "nothing to repaint". */
static fdk_rect list_band_rect(const fdk_list *l) {
    if (!l->banding) {
        return (fdk_rect){0, 0, 0, 0};
    }
    fdk_f32 x1 = (l->band_x < l->band_now_x) ? l->band_x
                                              : l->band_now_x;
    fdk_f32 x2 = (l->band_x < l->band_now_x) ? l->band_now_x
                                              : l->band_x;
    fdk_i32 ay_off = 0;
    if (l->scroll != NULL) {
        fdk_i32 axo = 0;
        fdk_scrollview_get_scroll_offset(l->scroll, &axo, &ay_off);
    }
    fdk_f32 anchor_vy = l->band_anchor_cy - (fdk_f32)ay_off;
    fdk_f32 y1 = (anchor_vy < l->band_now_y) ? anchor_vy
                                              : l->band_now_y;
    fdk_f32 y2 = (anchor_vy < l->band_now_y) ? l->band_now_y
                                              : anchor_vy;
    fdk_i32 bx1 = (fdk_i32)x1;
    fdk_i32 by1 = (fdk_i32)y1;
    fdk_i32 bx2 = (fdk_i32)x2;
    fdk_i32 by2 = (fdk_i32)y2;
    fdk_i32 w = l->base.bounds.width;
    fdk_i32 h = l->base.bounds.height;
    if (bx1 < 0) {
        bx1 = 0;
    }
    if (by1 < 0) {
        by1 = 0;
    }
    if (bx2 > w) {
        bx2 = w;
    }
    if (by2 > h) {
        by2 = h;
    }
    if (bx2 < bx1) {
        bx2 = bx1;
    }
    if (by2 < by1) {
        by2 = by1;
    }
    return (fdk_rect){bx1, by1, bx2 - bx1, by2 - by1};
}

static void list_apply_band(fdk_list *l) {
    if (l->count == 0) {
        return;
    }
    fdk_i32 rh = list_row_height(l);
    fdk_i32 off_y = 0;
    if (l->scroll != NULL) {
        fdk_i32 ox = 0;
        fdk_scrollview_get_scroll_offset(l->scroll, &ox, &off_y);
    }
    /* The ANCHOR is content-space (captured at press); the moving
     * edge is the viewport-space pointer + the CURRENT offset —
     * under auto-scroll the sweep grows, the anchor never moves. */
    fdk_f32 now_cy = l->band_now_y + (fdk_f32)off_y;
    fdk_f32 y1 = (l->band_anchor_cy < now_cy) ? l->band_anchor_cy
                                               : now_cy;
    fdk_f32 y2 = (l->band_anchor_cy < now_cy) ? now_cy
                                               : l->band_anchor_cy;
    fdk_i64 lo = (fdk_i64)y1 - 1; /* widen one px */
    fdk_i64 hi = (fdk_i64)y2 + 1;
    /* Row indices intersecting [lo, hi]: floor/ceil on the rh grid. */
    fdk_i64 r_lo = lo / rh;
    if (r_lo < 0) {
        r_lo = 0;
    }
    fdk_i64 r_hi = (hi + rh - 1) / rh; /* exclusive */
    if (r_hi > (fdk_i64)l->count) {
        r_hi = (fdk_i64)l->count;
    }
    /* Round 9 (the lag report): rows that keep their state are NOT
     * invalidated — list_set_row already damages only the rows that
     * FLIP. The whole-list invalidate that used to run here repainted
     * every row (full-width selection fills, header strip and all) on
     * every single motion pixel, which on a large directory pegged a
     * core. What still must repaint every motion: the band TINT rect
     * itself — the OLD band's rect and the NEW one. */
    fdk_rect band = list_band_rect(l);
    if (band.width > 0 && band.height > 0) {
        fdk_rect prev = l->band_last_rect;
        if (prev.width > 0 && prev.height > 0) {
            if (prev.x < band.x) {
                band.x = prev.x;
            }
            if (prev.y < band.y) {
                band.y = prev.y;
            }
            if (prev.x + prev.width > band.x + band.width) {
                band.width = prev.x + prev.width - band.x;
            }
            if (prev.y + prev.height > band.y + band.height) {
                band.height = prev.y + prev.height - band.y;
            }
        }
        l->band_last_rect = band;
        fdk__widget_invalidate_rect(&l->base, band);
    }
    for (size_t i = 0; i < l->count; i++) {
        bool want = ((fdk_i64)i >= r_lo && (fdk_i64)i < r_hi) ||
                    (l->band_ctrl && l->band_base != NULL &&
                     i < l->band_base_cap && l->band_base[i]);
        list_set_row(l, i, want); /* damages only flips */
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
 * merely schedules one more (identical) frame — it converges.
 *
 * 1.4.2: the rubber band draws FIRST (under the rows — the accent
 * tint + border; selection fills land over it). */
static void list_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_list *l = list_of(w);
    /* 1.5.0: the header strip. Painted FIRST (under everything the
     * scrollview draws over it — the strip is pinned above the
     * viewport, and the scrollview's inset below keeps the rows out
     * of its pixels; the pinning is the arrangement in
     * list_arrange/list_relayout, this is just the pixels). The
     * titles ride the horizontal scroll (off_x) exactly like the
     * rows do, so header and cells stay aligned; the strip itself
     * never scrolls vertically. */
    if (l->column_count > 0 && l->font != NULL &&
        bounds.width > 0 && bounds.height > 0) {
        fdk_i32 hh = list_header_height(l);
        fdk_i32 off_x = 0, off_y = 0;
        if (l->scroll != NULL) {
            fdk_scrollview_get_scroll_offset(l->scroll, &off_x,
                                             &off_y);
        }
        fdk_rect strip = {bounds.x, bounds.y, bounds.width, hh};
        fdk_surface_fill_rect(surface, strip, fdk__pal_sidebar());
        fdk_color line = fdk__pal_border();
        fdk_rect rule = {bounds.x, bounds.y + hh - 1, bounds.width, 1};
        fdk_surface_fill_rect(surface, rule, line);
        fdk_i32 baseline = fdk__center_baseline(
            l->font, bounds.y, hh);
        for (size_t c = 0; c < l->column_count; c++) {
            const char *title = l->columns[c].title;
            if (title == NULL || title[0] == '\0') {
                continue;
            }
            fdk_i32 cx = list_col_x(l, c) - off_x;
            if (cx >= bounds.width) {
                break; /* fully scrolled out */
            }
            fdk_i32 cw = l->col_widths[c];
            if (c == 0) {
                /* the primary column: left, beside the row padding */
                fdk__draw_text(surface, l->font, title,
                               fdk__pal_text_disabled(),
                               bounds.x + cx + LIST_ROW_PAD_X,
                               baseline);
            } else {
                fdk_i32 tw = 0, th = 0;
                fdk__text_extent(l->font, title, &tw, &th);
                fdk_i32 tx = bounds.x + cx;
                if (!l->columns[c].align_left) {
                    tx += (cw - tw) - LIST_ROW_PAD_X;
                    if (tx < bounds.x + cx) {
                        tx = bounds.x + cx;
                    }
                }
                fdk__draw_text(surface, l->font, title,
                               fdk__pal_text_disabled(), tx,
                               baseline);
                /* the column divider: a vertical hair the strip's
                 * height, at the separator's center — the desktop
                 * pane-header look (1.5.1) */
                fdk_i32 dx = bounds.x + cx - LIST_COL_SEP / 2 - off_x;
                if (dx >= bounds.x && dx < bounds.x + bounds.width) {
                    fdk_rect div = { dx, bounds.y, 1, hh };
                    fdk_surface_fill_rect(surface, div,
                                          fdk__pal_border());
                }
            }
        }
    }
    /* The rubber band (1.4.2), painted UNDER the rows (the list's
     * paint runs before its scrollview subtree): selection fills
     * draw over the tint, which reads exactly as GTK's translucent
     * band does once row selections land in it. Accent-derived:
     * a soft fill + a firmer border. */
    if (l->banding) {
        fdk_f32 x1 = (l->band_x < l->band_now_x) ? l->band_x
                                                  : l->band_now_x;
        fdk_f32 x2 = (l->band_x < l->band_now_x) ? l->band_now_x
                                                  : l->band_x;
        /* The anchor's on-screen y tracks the CONTENT it is glued
         * to (auto-scroll slides it off the top as the sweep grows
         * downward); the moving edge is where the pointer is. */
        fdk_i32 ay_off = 0;
        if (l->scroll != NULL) {
            fdk_i32 axo = 0;
            fdk_scrollview_get_scroll_offset(l->scroll, &axo, &ay_off);
        }
        fdk_f32 anchor_vy = l->band_anchor_cy - (fdk_f32)ay_off;
        fdk_f32 y1 = (anchor_vy < l->band_now_y) ? anchor_vy
                                                 : l->band_now_y;
        fdk_f32 y2 = (anchor_vy < l->band_now_y) ? l->band_now_y
                                                 : anchor_vy;
        fdk_i32 bx1 = (fdk_i32)x1 + bounds.x;
        fdk_i32 by1 = (fdk_i32)y1 + bounds.y;
        fdk_i32 bx2 = (fdk_i32)x2 + bounds.x;
        fdk_i32 by2 = (fdk_i32)y2 + bounds.y;
        /* Clamp to the bounds (the grab delivers out-of-widget
         * coordinates while the pointer is dragged past the edge). */
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
    if (l->scroll == NULL || w->bounds.width <= 0 ||
        w->bounds.height <= 0) {
        return;
    }
    fdk_i32 top = (l->column_count > 0) ? list_header_height(l) : 0;
    if (top < l->scroll->bounds.y ||
        l->scroll->bounds.x != 0 ||
        l->scroll->bounds.width != w->bounds.width ||
        l->scroll->bounds.height != w->bounds.height - top) {
        list_sync_scroll_bounds(l); /* 1.5.1: the shared sync — the
                                     * column anchor re-resolves
                                     * here too (this is the paint
                                     * self-heal path a direct
                                     * set_bounds relies on) */
        fdk_widget_child_layout_changed(l->scroll);
    }
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
    l->selected_count = 0;
    l->batch_depth = 0;
    l->batch_dirty = false;
    l->batch_sel_at_start = 0;
    l->content_width = 80; /* the v1 minimum, until a row widens it */
    l->columns = NULL;      /* 1.5.0: the plain list until set_columns */
    l->column_count = 0;
    l->col_titles = NULL;
    l->col_widths = NULL;
    l->name_nat = 80;       /* 1.5.1: until rows/callers widen it  */
    l->col_x = NULL;
    l->col_name_width = 80;
    l->columns_width = 0;
    l->banding = false;
    l->band_ctrl = false;
    l->band_x = l->band_y = 0.0f;
    l->band_now_x = l->band_now_y = 0.0f;
    l->band_base = NULL;
    l->band_base_cap = 0;
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

/* ---- columns (1.5.0) ---- */

/* TEST SEAM (1.5.1): the resolved column geometry — col_x[c] and
 * the width of column c — so the rig can assert the right-edge
 * anchor and the resize adaptation without pixel-divining. Also
 * hands back content_width (the h-scroll floor). Never called by
 * the library itself. */
void fdk__list_test_column_x(fdk_widget *list, size_t col,
                             fdk_i32 *out_x, fdk_i32 *out_w,
                             fdk_i32 *out_content_width) {
    if (out_x != NULL) {
        *out_x = 0;
    }
    if (out_w != NULL) {
        *out_w = 0;
    }
    if (out_content_width != NULL) {
        *out_content_width = 0;
    }
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    if (col < l->column_count) {
        if (out_x != NULL) {
            *out_x = l->col_x != NULL ? l->col_x[col] : 0;
        }
        if (out_w != NULL) {
            *out_w = (col == 0) ? l->col_name_width
                                : l->col_widths[col];
        }
    }
    if (out_content_width != NULL) {
        *out_content_width = l->content_width;
    }
}

fdk_result fdk_list_set_columns(fdk_widget *list,
                                const fdk_list_column *columns,
                                size_t count) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (count > 0 && columns == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Build the owned copies + resolved widths first (OOM leaves
     * the old column set untouched). */
    fdk_list_column *cols = NULL;
    char **titles = NULL;
    fdk_i32 *widths = NULL;
    fdk_i32 *xs = NULL;
    if (count > 0) {
        cols = fdk_alloc_array(count, sizeof(*cols));
        if (cols == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        titles = fdk_alloc_array(count, sizeof(*titles));
        if (titles == NULL) {
            fdk_free(cols);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        widths = fdk_alloc_array(count, sizeof(*widths));
        if (widths == NULL) {
            fdk_free(cols);
            fdk_free(titles);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        xs = fdk_alloc_array(count, sizeof(*xs));
        if (xs == NULL) {
            fdk_free(cols);
            fdk_free(titles);
            fdk_free(widths);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        for (size_t i = 0; i < count; i++) {
            if (columns[i].title != NULL) {
                titles[i] = fdk__strdup(columns[i].title);
                if (titles[i] == NULL) {
                    for (size_t j = 0; j < i; j++) {
                        fdk_free(titles[j]);
                    }
                    fdk_free(titles);
                    fdk_free(cols);
                    fdk_free(widths);
                    fdk_free(xs);
                    return FDK_ERR_OUT_OF_MEMORY;
                }
            } else {
                titles[i] = NULL;
            }
            cols[i].title = titles[i]; /* borrowed (col_titles owns) */
            cols[i].align_left = columns[i].align_left;
            fdk_i32 w = columns[i].min_width;
            if (i == 0) {
                if (w < 80) {
                    w = 80; /* the primary column keeps the v1
                             * minimum — the plain-list contract
                             * must survive under columns */
                }
            } else if (w < LIST_COL_MIN) {
                w = LIST_COL_MIN;
            }
            widths[i] = w;
        }
    }

    /* Swap in. Existing rows keep their cell TEXTS where the new
     * set still has the column (index-parity, the same rule the
     * row array uses); cells beyond the new count are dropped, and
     * a newly-widened set gets fresh (empty) cell slots. */
    if (l->col_titles != NULL) {
        for (size_t i = 0; i < l->column_count; i++) {
            fdk_free(l->col_titles[i]);
        }
    }
    fdk_free(l->col_titles);
    fdk_free(l->columns);
    fdk_free(l->col_widths);
    fdk_free(l->col_x);
    l->columns = cols;
    l->col_titles = titles;
    l->col_widths = widths;
    l->col_x = xs;
    size_t old_count = l->column_count;
    l->column_count = count;

    /* Resolve the fixed columns' title floors now that the set is
     * live (list_col_title_width reads the list's font + set). */
    for (size_t i = 1; i < count; i++) {
        fdk_i32 w = list_col_title_width(l, i);
        if (w > l->col_widths[i]) {
            l->col_widths[i] = w;
        }
    }
    /* Re-measure every existing row's cells into the new widths —
     * a rebuilt column set on a populated list (the navigate case)
     * must land with the widths the data demands. */
    for (size_t r = 0; r < l->count; r++) {
        fdk_list_row *row = l->row_widgets[r];
        if (row->cells == NULL && count > 1) {
            row->cells = fdk_alloc_array(count - 1, sizeof(char *));
            if (row->cells == NULL) {
                continue; /* the row renders without cells */
            }
            row->cells_cap = count - 1;
        } else if (row->cells != NULL && count <= 1) {
            for (size_t c = 0; c < row->cells_cap; c++) {
                fdk_free(row->cells[c]);
            }
            fdk_free(row->cells);
            row->cells = NULL;
            row->cells_cap = 0;
            continue;
        } else if (row->cells != NULL && count < old_count) {
            for (size_t c = count - 1; c + 1 < old_count; c++) {
                fdk_free(row->cells[c]);
                row->cells[c] = NULL;
            }
        }
        /* a WIDENED set: grow the array (existing texts keep their
         * slots; the new tail is NULL) and refresh the capacity */
        if (row->cells != NULL && row->cells_cap < count - 1) {
            char **grown = fdk_alloc_array(count - 1,
                                            sizeof(char *));
            if (grown != NULL) {
                for (size_t c = 0; c < row->cells_cap; c++) {
                    grown[c] = row->cells[c];
                }
                for (size_t c = row->cells_cap; c + 1 < count; c++) {
                    grown[c] = NULL;
                }
                fdk_free(row->cells);
                row->cells = grown;
                row->cells_cap = count - 1;
            }
        }
        if (row->cells == NULL) {
            continue;
        }
        for (size_t c = 1; c < count; c++) {
            const char *cell = row->cells[c - 1];
            if (cell != NULL && cell[0] != '\0' &&
                l->font != NULL) {
                fdk_i32 tw = 0, th = 0;
                fdk__text_extent(l->font, cell, &tw, &th);
                fdk_i32 want = tw + LIST_ROW_PAD_X * 2;
                if (want > l->col_widths[c]) {
                    l->col_widths[c] = want;
                }
            }
        }
    }
    list_columns_recompute(l);
    list_relayout(l);
    fdk_widget_invalidate(list);
    return FDK_OK;
}

size_t fdk_list_get_column_count(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return 0;
    }
    return list_of(list)->column_count;
}

fdk_result fdk_list_row_set_cell(fdk_widget *list, size_t row,
                                 size_t col, const char *text) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count || col == 0 || col >= l->column_count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list_row *r = l->row_widgets[row];
    if (r->cells == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* set_columns raced a clear */
    }
    char *copy = NULL;
    if (text != NULL && text[0] != '\0') {
        copy = fdk__strdup(text);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    fdk_free(r->cells[col - 1]);
    r->cells[col - 1] = copy;
    /* Widen the column to fit (never narrower than declared): a
     * right-aligned cell must not clip its own tail. */
    if (copy != NULL && l->font != NULL) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(l->font, copy, &tw, &th);
        fdk_i32 want = tw + LIST_ROW_PAD_X * 2;
        if (want > l->col_widths[col]) {
            l->col_widths[col] = want;
            list_columns_recompute(l);
            list_relayout(l);
        }
    }
    fdk_widget_invalidate(&r->base);
    return FDK_OK;
}

bool fdk_list_row_set_drag_armed(fdk_widget *list, size_t row,
                                 bool armed) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return false;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return false;
    }
    bool was = l->row_widgets[row]->drag_armed;
    l->row_widgets[row]->drag_armed = armed;
    return was;
}

void fdk_list_set_selection_mode(fdk_widget *list,
                                 fdk_list_selection_mode mode) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    l->mode = mode;
    /* 1.5.0: MULTIPLE owns the plain press (the rubber band — any
     * press that reaches the list can start one, rows included), so
     * the widget vetoes drag-source arming for its own presses. A
     * drag from list content then begins only from a row the app
     * armed (fdk_list_row_set_drag_armed — a SELECTED row), which
     * is the desktop-standard gesture. */
    fdk_widget_set_drag_veto(list, mode == FDK_LIST_SELECTION_MULTIPLE);
    if (mode == FDK_LIST_SELECTION_NONE) {
        list_clear_selection(l);
        list_fire_changed(l);
    }
}

/* 1.4.0 — the row height this list actually lays rows out at (the
 * theme's LIST_ROW_HEIGHT floor, the font's metrics, whichever is
 * taller; 20 for a fontless list). Apps use it to map mouse coords
 * to row indices over a list, and to predict how many rows a fixed
 * height will show. 0 for a non-list. */
fdk_i32 fdk_list_get_row_height(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return 0;
    }
    return list_row_height(list_of(list));
}

static fdk_result list_grow(fdk_list *l) {
    if (l->count < l->capacity) {
        return FDK_OK;
    }
    size_t cap = (l->capacity == 0) ? 8 : l->capacity * 2;
    if (cap < l->capacity || /* wrapped */
        cap > SIZE_MAX / sizeof(fdk_list_row *)) {
        return FDK_ERR_OUT_OF_MEMORY; /* refuse absurd growth */
    }
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
    /* 1.5.0: the row's cell array parallels the list's column set
     * (NULL when there are no fixed columns — the common v1 list). */
    if (l->column_count > 1) {
        row->cells = fdk_alloc_array(l->column_count - 1,
                                     sizeof(char *));
        if (row->cells == NULL) {
            fdk_widget_destroy(row_w);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        row->cells_cap = l->column_count - 1;
    }
    /* Shift the widget array (widget z-order follows row order). */
    memmove(&l->row_widgets[index + 1], &l->row_widgets[index],
            (l->count - index) * sizeof(*l->row_widgets));
    l->row_widgets[index] = row;
    l->count++;
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return FDK_OK; /* one relayout at end_batch */
    }
    if (index == l->count - 1) {
        list_place_appended(l); /* the O(1) common case */
    } else {
        list_relayout(l); /* interior insert shifts every later row */
    }
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
    bool was_selected = l->row_widgets[index]->selected;
    /* Remove from the array first so relayout doesn't see a row that
     * is mid-destruction. */
    memmove(&l->row_widgets[index], &l->row_widgets[index + 1],
            (l->count - index - 1) * sizeof(*l->row_widgets));
    l->count--;
    if (was_selected) {
        /* The destroy below fires no a11y state change for the row
         * (CHILDREN_CHANGED on the container covers it); keep the
         * incremental count honest by hand. */
        l->selected_count--;
    }
    fdk_widget_destroy(row_w);
    if (l->anchor >= l->count && l->count > 0) {
        l->anchor = l->count - 1;
    }
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return FDK_OK; /* relayout + one fire at end_batch */
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
    bool had_selected = (l->selected_count != 0);
    /* One pass: destroy every row, then reset. The v1 loop removed
     * from the tail one row at a time — an O(N^2) relayout and an
     * N+1 selection-changed callback storm for one user-visible
     * change; both were wrong, not just slow. */
    for (size_t i = 0; i < l->count; i++) {
        fdk_widget_destroy(&l->row_widgets[i]->base);
    }
    l->count = 0;
    l->selected_count = 0;
    l->anchor = 0;
    l->key_cursor = (size_t)-1;
    l->have_last_click = false;
    l->content_width = 80;
    /* 1.5.0: the fixed columns' cell-driven widening resets with
     * the rows (the widths' floors are the declared + title
     * minimums — recompute from the set). */
    if (l->column_count > 0) {
        for (size_t c = 1; c < l->column_count; c++) {
            l->col_widths[c] = list_col_title_width(l, c);
        }
        list_columns_recompute(l);
        l->name_nat = 80; /* rows gone: the name column resets */
    }
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return; /* relayout + one fire at end_batch */
    }
    list_relayout(l);
    if (had_selected) {
        list_fire_changed(l);
    }
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
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return FDK_OK;
    }
    list_relayout(l);
    fdk_widget_invalidate(&l->row_widgets[row]->base);
    return FDK_OK;
}

/* ---- Row icons (1.4.1) ---- */

fdk_result fdk_list_row_set_icon(fdk_widget *list, size_t row,
                                 fdk_row_icon icon) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    switch (icon) {
    case FDK_ROW_ICON_NONE:
    case FDK_ROW_ICON_FOLDER:
    case FDK_ROW_ICON_HOME:
    case FDK_ROW_ICON_DRIVE:
    case FDK_ROW_ICON_FILE:
    case FDK_ROW_ICON_RECENT: /* 1.4.4's glyph was silently dropped
                               * here (read as NONE) — the Recent
                               * place lost its clock face; fixed. */
        break;
    default:
        icon = FDK_ROW_ICON_NONE; /* unknown values read as NONE */
        break;
    }
    fdk_list_row *r = l->row_widgets[row];
    if (r->icon == icon) {
        return FDK_OK;
    }
    r->icon = icon;
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return FDK_OK;
    }
    /* The glyph changes the row's content width — the same settle
     * path a text change takes. */
    list_relayout(l);
    fdk_widget_invalidate(&r->base);
    return FDK_OK;
}

fdk_row_icon fdk_list_row_get_icon(fdk_widget *list, size_t row) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ROW_ICON_NONE;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return FDK_ROW_ICON_NONE;
    }
    return l->row_widgets[row]->icon;
}

fdk_result fdk__list_row_set_icon_image(fdk_widget *list, size_t row,
                                        fdk_surface *image) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list *l = list_of(list);
    if (row >= l->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_list_row *r = l->row_widgets[row];
    if (r->image == image) {
        return FDK_OK;
    }
    r->image = image; /* borrowed: the icon-theme cache owns it */
    if (l->batch_depth > 0) {
        l->batch_dirty = true;
        return FDK_OK;
    }
    /* Presence (not content) changes the row's content width — the
     * same settle path the symbolic setter takes. */
    list_relayout(l);
    fdk_widget_invalidate(&r->base);
    return FDK_OK;
}

fdk_result fdk_list_row_set_icon_surface(fdk_widget *list, size_t row,
                                         fdk_surface *image) {
    return fdk__list_row_set_icon_image(list, row, image);
}

void fdk_list_set_on_selection_changed_bounds(
    fdk_widget *list, fdk_list_selection_bounds_fn fn,
    void *user_data) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    l->on_selection_bounds = fn;
    l->on_selection_bounds_data = user_data;
    /* one signal slot: setting the bounds twin retires the plain
     * callback (the documented replacement) */
    l->on_selection_changed = NULL;
    l->on_selection_data = NULL;
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

void fdk_list_set_activate_on_single_click(fdk_widget *list,
                                           bool single) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    list_of(list)->single_activate = single;
}

bool fdk_list_get_activate_on_single_click(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return false;
    }
    return list_of(list)->single_activate;
}

/* ---- Bulk-mutation batching (1.3.0) ---- */

void fdk_list_begin_batch(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    if (l->batch_depth == 0) {
        l->batch_dirty = false;
        l->batch_sel_at_start = l->selected_count;
    }
    l->batch_depth++;
}

void fdk_list_end_batch(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    if (l->batch_depth == 0) {
        return; /* unbalanced: ignore, matching the quiet setter style */
    }
    l->batch_depth--;
    if (l->batch_depth > 0) {
        return; /* the outermost batch settles */
    }
    if (l->batch_dirty) {
        l->batch_dirty = false;
        list_relayout(l);
        /* Virtual children (the rows) changed for a11y consumers. */
        fdk__a11y_notify(&l->base, FDK_A11Y_CHILDREN_CHANGED, 0);
    }
    if (l->selected_count != l->batch_sel_at_start) {
        list_fire_changed(l); /* exactly ONE fire for the whole batch */
    }
}

/* The selection-clear the siblings have had since v1 (tree: select
 * NODE_NONE; combo: set_active -1). Clears every selected row in
 * any mode, firing on_selection_changed once when anything was
 * actually selected. */
void fdk_list_clear_selection(fdk_widget *list) {
    if (list == NULL || list->klass != &fdk_list_class_def) {
        return;
    }
    fdk_list *l = list_of(list);
    if (l->selected_count == 0) {
        return;
    }
    list_clear_selection(l);
    list_fire_changed(l);
}

static void fdk__list_cancel_bands_subtree(fdk_widget *w);

/* The gesture-cancel sweep (round 9): see widgets_internal.h. Walks
 * every root, every widget; a list with a live band ends it exactly
 * as a POINTER_UP would (minus the fire — nothing changed since the
 * last apply; the callback already ran at the press/motions). */
void fdk__list_cancel_bands_all_roots(void) {
    for (fdk_widget *root = fdk__widget_roots_head(); root != NULL;
         root = root->root_next) {
        fdk__list_cancel_bands_subtree(root);
    }
}

/* Recursive half: pre-order. A list's band lives on the list itself,
 * and menus can be nowhere inside a list (popups are separate
 * windows), so there is no ordering constraint with the subtree
 * walk. The pending_band flags (1.5.0's row-born gestures) are
 * cleared too: their POINTER_UP was swallowed with the popup's, and
 * a stale armed row would synthesize a phantom band from stale
 * coordinates on the next motion. */
static void fdk__list_cancel_bands_subtree(fdk_widget *w) {
    if (w == NULL || (w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (w->klass == &fdk_list_class_def) {
        fdk_list *l = list_of(w);
        if (l->banding) {
            l->banding = false;
            list_band_scroll_disarm(l);
            fdk_free(l->band_base);
            l->band_base = NULL;
            l->band_base_cap = 0;
            fdk_widget_invalidate(w); /* the band rect disappears */
        }
    }
    if (w->klass == &fdk_list_row_class_def) {
        row_of(w)->pending_band = false; /* the swallowed release's
                                          * arm dies with the chain */
    }
    for (size_t i = 0; i < w->child_count; i++) {
        fdk__list_cancel_bands_subtree(w->children[i]);
    }
}
