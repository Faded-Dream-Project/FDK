/*
 * grid.c — the GRID container (Phase 5 completion; see fdk_layout.h).
 *
 * Children occupy (col, row) cells and may span several. The
 * container's measure is the negotiated column widths + row heights
 * (single-span children take the maxima; multi-span children
 * distribute the deficit they introduce equally over their span —
 * the classic GTK algorithm, integer-safe); arrange assigns each
 * child its cell rect (margins honored, align hints applied inside
 * the cell), with expand-marked columns/rows sharing any EXTRA space
 * the container is given beyond its natural size.
 *
 * Placement is carried BY the child (grid_col/grid_row/... on the
 * widget, set by fdk_grid_attach) exactly like the box's margins/
 * expand/align hints — so child destruction needs no container-side
 * unlink and re-parenting into another container simply forgets the
 * placement. Track state (widths/heights/expand flags) lives in the
 * embedded subclass struct, like fdk_box.
 *
 * Like the box, the grid is a widget subclass running its policy
 * through the Phase 4 measure/arrange hooks — nothing in the object
 * model knows grids exist.
 */

#define FDK_LOG_TAG "layout"

#include "layout/layout_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdlib.h>

typedef struct fdk_grid {
    fdk_widget base;
    fdk_i32 rows, cols;
    fdk_i32 spacing;    /* both axes */
    fdk_i32 padding;
    bool homogeneous;
    /* Per-axis expand flags (setters) + the track-size cache the
     * measure pass computes and the arrange pass consumes (one
     * measure per layout — the same contract the box honors). */
    unsigned char *col_expand;
    unsigned char *row_expand;
    fdk_i32 *col_w;
    fdk_i32 *row_h;
    fdk_i32 track_cap; /* entries per axis array */
} fdk_grid;

/* Grid-ness is hook delegation, exactly like the box's rule: any
 * class running the grid hooks is a grid. */
static bool grid_class_of(const fdk_widget *w) {
    if (w == NULL || w->klass == NULL) {
        return false;
    }
    return w->klass->measure == fdk_grid_measure_hook ||
           w->klass->arrange == fdk_grid_arrange_hook;
}

static fdk_grid *grid_of(fdk_widget *w) {
    if (!grid_class_of(w)) {
        return NULL;
    }
    return (fdk_grid *)w;
}

/* Grows the per-axis arrays to fit `need` tracks (all three keep the
 * same capacity; growth is atomic enough — on failure the old arrays
 * stay valid and the caller refuses the growth). */
static bool grid_reserve(fdk_grid *g, fdk_i32 need) {
    if (need <= g->track_cap) {
        return true;
    }
    fdk_i32 cap = g->track_cap == 0 ? 8 : g->track_cap;
    while (cap < need) {
        cap *= 2;
    }
    unsigned char *ce = fdk_realloc(g->col_expand, (size_t)cap);
    if (ce == NULL) {
        return false;
    }
    g->col_expand = ce;
    unsigned char *re = fdk_realloc(g->row_expand, (size_t)cap);
    if (re == NULL) {
        return false;
    }
    g->row_expand = re;
    fdk_i32 *cw = fdk_realloc(g->col_w, (size_t)cap * sizeof(fdk_i32));
    if (cw == NULL) {
        return false;
    }
    g->col_w = cw;
    fdk_i32 *rh = fdk_realloc(g->row_h, (size_t)cap * sizeof(fdk_i32));
    if (rh == NULL) {
        return false;
    }
    g->row_h = rh;
    for (fdk_i32 i = g->track_cap; i < cap; i++) {
        g->col_expand[i] = 0;
        g->row_expand[i] = 0;
    }
    g->track_cap = cap;
    return true;
}

/* A child's margin-inclusive extent. */
static void grid_child_extent(fdk_widget *child, fdk_size *out) {
    fdk_widget_measure(child, out);
    out->width += child->margin_left + child->margin_right;
    out->height += child->margin_top + child->margin_bottom;
}

/* ---- measure ------------------------------------------------------------ */

void fdk_grid_measure_hook(fdk_widget *w, fdk_size *out) {
    fdk_grid *g = grid_of(w);
    if (g == NULL) {
        *out = (fdk_size){0, 0};
        return;
    }

    if (!grid_reserve(g, g->cols > g->rows ? g->cols : g->rows)) {
        *out = (fdk_size){0, 0};
        return;
    }

    for (fdk_i32 c = 0; c < g->cols; c++) {
        g->col_w[c] = 0;
    }
    for (fdk_i32 r = 0; r < g->rows; r++) {
        g->row_h[r] = 0;
    }

    /* Pass 1: single-span maxima. */
    for (size_t i = 0; i < w->child_count; i++) {
        fdk_widget *child = w->children[i];
        if (!child->grid_attached ||
            (child->flags & FDK_WF_VISIBLE) == 0 ||
            (child->flags & FDK_WF_DESTROYING) != 0) {
            continue;
        }
        fdk_size ext;
        grid_child_extent(child, &ext);
        if (child->grid_colspan == 1 && child->grid_col >= 0 &&
            child->grid_col < g->cols && ext.width > g->col_w[child->grid_col]) {
            g->col_w[child->grid_col] = ext.width;
        }
        if (child->grid_rowspan == 1 && child->grid_row >= 0 &&
            child->grid_row < g->rows && ext.height > g->row_h[child->grid_row]) {
            g->row_h[child->grid_row] = ext.height;
        }
    }

    /* Pass 2: multi-span children distribute their deficit equally
     * over the spanned tracks (integer remainders to the earlier
     * tracks — deterministic). */
    for (size_t i = 0; i < w->child_count; i++) {
        fdk_widget *child = w->children[i];
        if (!child->grid_attached ||
            (child->flags & FDK_WF_VISIBLE) == 0 ||
            (child->flags & FDK_WF_DESTROYING) != 0) {
            continue;
        }
        fdk_size ext;
        grid_child_extent(child, &ext);

        if (child->grid_colspan > 1 && child->grid_col >= 0 &&
            child->grid_col + child->grid_colspan <= g->cols) {
            fdk_i64 spanned = (fdk_i64)(child->grid_colspan - 1) * g->spacing;
            for (fdk_i32 c = child->grid_col;
                 c < child->grid_col + child->grid_colspan; c++) {
                spanned += g->col_w[c];
            }
            if ((fdk_i64)ext.width > spanned) {
                fdk_i64 deficit = (fdk_i64)ext.width - spanned;
                fdk_i64 each = deficit / child->grid_colspan;
                fdk_i64 rem = deficit - each * child->grid_colspan;
                for (fdk_i32 c = child->grid_col;
                     c < child->grid_col + child->grid_colspan; c++) {
                    g->col_w[c] = (fdk_i32)(g->col_w[c] + each +
                                            (rem-- > 0 ? 1 : 0));
                }
            }
        }
        if (child->grid_rowspan > 1 && child->grid_row >= 0 &&
            child->grid_row + child->grid_rowspan <= g->rows) {
            fdk_i64 spanned = (fdk_i64)(child->grid_rowspan - 1) * g->spacing;
            for (fdk_i32 r = child->grid_row;
                 r < child->grid_row + child->grid_rowspan; r++) {
                spanned += g->row_h[r];
            }
            if ((fdk_i64)ext.height > spanned) {
                fdk_i64 deficit = (fdk_i64)ext.height - spanned;
                fdk_i64 each = deficit / child->grid_rowspan;
                fdk_i64 rem = deficit - each * child->grid_rowspan;
                for (fdk_i32 r = child->grid_row;
                     r < child->grid_row + child->grid_rowspan; r++) {
                    g->row_h[r] = (fdk_i32)(g->row_h[r] + each +
                                            (rem-- > 0 ? 1 : 0));
                }
            }
        }
    }

    /* Homogeneous: every track takes the axis maximum. */
    if (g->homogeneous) {
        fdk_i32 maxw = 0, maxh = 0;
        for (fdk_i32 c = 0; c < g->cols; c++) {
            if (g->col_w[c] > maxw) {
                maxw = g->col_w[c];
            }
        }
        for (fdk_i32 r = 0; r < g->rows; r++) {
            if (g->row_h[r] > maxh) {
                maxh = g->row_h[r];
            }
        }
        for (fdk_i32 c = 0; c < g->cols; c++) {
            g->col_w[c] = maxw;
        }
        for (fdk_i32 r = 0; r < g->rows; r++) {
            g->row_h[r] = maxh;
        }
    }

    fdk_i64 total_w = (fdk_i64)g->padding * 2;
    fdk_i64 total_h = (fdk_i64)g->padding * 2;
    for (fdk_i32 c = 0; c < g->cols; c++) {
        total_w += g->col_w[c];
    }
    if (g->cols > 1) {
        total_w += (fdk_i64)(g->cols - 1) * g->spacing;
    }
    for (fdk_i32 r = 0; r < g->rows; r++) {
        total_h += g->row_h[r];
    }
    if (g->rows > 1) {
        total_h += (fdk_i64)(g->rows - 1) * g->spacing;
    }
    *out = (fdk_size){ total_w > (fdk_i64)INT32_MAX / 2 ? INT32_MAX / 2
                                                         : (fdk_i32)total_w,
                       total_h > (fdk_i64)INT32_MAX / 2 ? INT32_MAX / 2
                                                         : (fdk_i32)total_h };
}

/* ---- arrange ------------------------------------------------------------ */

/* Places a child inside its cell per its align hints, margins, and
 * expand flags. BASELINE inside a single cell behaves like END — the
 * honest per-cell approximation (cross-child baseline alignment is
 * the box's cross-axis feature; a cell has one child). */
static void place_in_cell(fdk_widget *child, fdk_rect cell) {
    fdk_size nat;
    fdk_widget_measure(child, &nat);

    fdk_i32 avail_w = cell.width - child->margin_left - child->margin_right;
    fdk_i32 avail_h = cell.height - child->margin_top - child->margin_bottom;
    if (avail_w < 0) {
        avail_w = 0;
    }
    if (avail_h < 0) {
        avail_h = 0;
    }

    fdk_i32 w = nat.width;
    fdk_i32 h = nat.height;
    fdk_i32 x = cell.x + child->margin_left;
    fdk_i32 y = cell.y + child->margin_top;

    if (child->align_h == FDK_ALIGN_FILL || child->expand_h) {
        w = avail_w;
    } else {
        switch (child->align_h) {
        case FDK_ALIGN_CENTER:
            x = cell.x + child->margin_left + (avail_w - w) / 2;
            break;
        case FDK_ALIGN_END:
            x = cell.x + child->margin_left + avail_w - w;
            break;
        case FDK_ALIGN_BASELINE: /* horizontal baseline reads as START */
        case FDK_ALIGN_START:
        default:
            break;
        }
    }

    if (child->align_v == FDK_ALIGN_FILL || child->expand_v) {
        h = avail_h;
    } else {
        switch (child->align_v) {
        case FDK_ALIGN_CENTER:
            y = cell.y + child->margin_top + (avail_h - h) / 2;
            break;
        case FDK_ALIGN_END:
        case FDK_ALIGN_BASELINE:
            y = cell.y + child->margin_top + avail_h - h;
            break;
        case FDK_ALIGN_START:
        default:
            break;
        }
    }

    fdk_widget_arrange(child, (fdk_rect){ .x = x, .y = y, .width = w,
                                          .height = h });
}

void fdk_grid_arrange_hook(fdk_widget *w, fdk_rect assigned) {
    fdk_grid *g = grid_of(w);
    if (g == NULL) {
        return;
    }

    fdk_widget_set_bounds(w, assigned);

    /* Measure fills the track cache; arrange consumes it (running
     * the measure here keeps arrange self-sufficient — the same
     * pattern the box's hooks follow). */
    fdk_size natural;
    fdk_grid_measure_hook(w, &natural);

    /* Extra space (beyond natural) goes to expand-marked tracks. */
    fdk_i64 extra_w = (fdk_i64)assigned.width - natural.width;
    fdk_i64 extra_h = (fdk_i64)assigned.height - natural.height;

    if (extra_w > 0) {
        fdk_i32 expand_cols = 0;
        for (fdk_i32 c = 0; c < g->cols; c++) {
            if (g->col_expand[c]) {
                expand_cols++;
            }
        }
        if (expand_cols > 0) {
            fdk_i64 each = extra_w / expand_cols;
            fdk_i64 rem = extra_w - each * expand_cols;
            for (fdk_i32 c = 0; c < g->cols; c++) {
                if (g->col_expand[c]) {
                    g->col_w[c] = (fdk_i32)(g->col_w[c] + each +
                                            (rem-- > 0 ? 1 : 0));
                }
            }
        }
    }
    if (extra_h > 0) {
        fdk_i32 expand_rows = 0;
        for (fdk_i32 r = 0; r < g->rows; r++) {
            if (g->row_expand[r]) {
                expand_rows++;
            }
        }
        if (expand_rows > 0) {
            fdk_i64 each = extra_h / expand_rows;
            fdk_i64 rem = extra_h - each * expand_rows;
            for (fdk_i32 r = 0; r < g->rows; r++) {
                if (g->row_expand[r]) {
                    g->row_h[r] = (fdk_i32)(g->row_h[r] + each +
                                            (rem-- > 0 ? 1 : 0));
                }
            }
        }
    }

    for (size_t i = 0; i < w->child_count; i++) {
        fdk_widget *child = w->children[i];
        if (!child->grid_attached ||
            (child->flags & FDK_WF_VISIBLE) == 0 ||
            (child->flags & FDK_WF_DESTROYING) != 0) {
            continue;
        }
        /* Defensive clamp of the attachment into the grid extent. */
        fdk_i32 c0 = child->grid_col < 0 ? 0 : child->grid_col;
        fdk_i32 r0 = child->grid_row < 0 ? 0 : child->grid_row;
        if (c0 >= g->cols || r0 >= g->rows) {
            continue;
        }
        fdk_i32 cs = child->grid_colspan;
        if (cs > g->cols - c0) {
            cs = g->cols - c0;
        }
        fdk_i32 rs = child->grid_rowspan;
        if (rs > g->rows - r0) {
            rs = g->rows - r0;
        }

        /* Children cells are GRID-RELATIVE (the core's
         * parent-relative contract): packing starts at the grid's
         * own padding origin — NOT at assigned.x/y (the grid's
         * position RELATIVE TO ITS PARENT), which paint_rec stacks
         * on top of the grid's absolute origin again. The exact bug
         * the box fixed in Phase 6 ("double-offsetting every child
         * of any box not at (0,0)"), caught here by the breathing
         * meter demo: a grid whose own position moves never re-damages
         * its children, and they stop painting entirely. */
        fdk_i32 x = g->padding;
        for (fdk_i32 c = 0; c < c0; c++) {
            x += g->col_w[c] + g->spacing;
        }
        fdk_i32 cw = -g->spacing;
        for (fdk_i32 c = c0; c < c0 + cs; c++) {
            cw += g->col_w[c] + g->spacing;
        }

        fdk_i32 y = g->padding;
        for (fdk_i32 r = 0; r < r0; r++) {
            y += g->row_h[r] + g->spacing;
        }
        fdk_i32 ch = -g->spacing;
        for (fdk_i32 r = r0; r < r0 + rs; r++) {
            ch += g->row_h[r] + g->spacing;
        }

        place_in_cell(child, (fdk_rect){ .x = x, .y = y, .width = cw,
                                         .height = ch });
    }
}

/* ---- subclass plumbing --------------------------------------------------- */

static void grid_destroy_hook(fdk_widget *w) {
    fdk_grid *g = grid_of(w);
    if (g == NULL) {
        return;
    }
    fdk_free(g->col_expand);
    fdk_free(g->row_expand);
    fdk_free(g->col_w);
    fdk_free(g->row_h);
}

const fdk_widget_class fdk_grid_class_def = {
    .size = sizeof(fdk_grid),
    .name = "grid",
    .handle_event = NULL,
    .paint = NULL, /* container: the tree walk paints the children */
    .measure = fdk_grid_measure_hook,
    .arrange = fdk_grid_arrange_hook,
    .destroy = grid_destroy_hook,
};

/* ---- public API ------------------------------------------------------------ */

fdk_result fdk_grid_create(fdk_widget *parent, fdk_i32 rows, fdk_i32 columns,
                           fdk_widget **out_grid) {
    if (out_grid == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (rows < 0) {
        rows = 0;
    }
    if (columns < 0) {
        columns = 0;
    }

    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_grid_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_grid *g = (fdk_grid *)w;
    g->rows = rows;
    g->cols = columns;
    g->spacing = 0;
    g->padding = 0;
    g->homogeneous = false;
    g->col_expand = NULL;
    g->row_expand = NULL;
    g->col_w = NULL;
    g->row_h = NULL;
    g->track_cap = 0;
    if (!grid_reserve(g, rows > columns ? rows : columns)) {
        fdk_widget_destroy(w); /* destroy hook frees what exists */
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* fdk_widget_create relayouted the parent against a zeroed grid;
     * re-notify now that the fields are real (the box does the
     * same — the create-time notification measures a still-zeroed
     * subclass). */
    fdk_widget_child_layout_changed(w->parent);

    *out_grid = w;
    return FDK_OK;
}

fdk_result fdk_grid_attach(fdk_widget *grid, fdk_widget *child, fdk_i32 column,
                           fdk_i32 row, fdk_i32 colspan, fdk_i32 rowspan) {
    fdk_grid *g = grid_of(grid);
    if (g == NULL || child == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (child->parent != grid) {
        FDK_WARN("fdk_grid_attach: child is not a direct child of the grid");
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (column < 0 || row < 0 || colspan < 1 || rowspan < 1) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* The grid grows to contain the attachment. */
    fdk_i32 need_cols = column + colspan;
    fdk_i32 need_rows = row + rowspan;
    if (need_cols > g->cols || need_rows > g->rows) {
        if (need_cols > g->cols) {
            g->cols = need_cols;
        }
        if (need_rows > g->rows) {
            g->rows = need_rows;
        }
        if (!grid_reserve(g, g->cols > g->rows ? g->cols : g->rows)) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }

    child->grid_col = column;
    child->grid_row = row;
    child->grid_colspan = colspan;
    child->grid_rowspan = rowspan;
    child->grid_attached = true;

    fdk_widget_child_layout_changed(grid);
    return FDK_OK;
}

static fdk_grid *as_grid(fdk_widget *w) {
    fdk_grid *g = grid_of(w);
    if (g == NULL) {
        FDK_WARN("grid setter on a non-grid widget — ignored");
    }
    return g;
}

void fdk_grid_set_spacing(fdk_widget *grid, fdk_i32 spacing) {
    fdk_grid *g = as_grid(grid);
    if (g == NULL) {
        return;
    }
    if (spacing < 0) {
        spacing = 0;
    }
    if (g->spacing == spacing) {
        return;
    }
    g->spacing = spacing;
    fdk_widget_child_layout_changed(grid);
}

void fdk_grid_set_padding(fdk_widget *grid, fdk_i32 padding) {
    fdk_grid *g = as_grid(grid);
    if (g == NULL) {
        return;
    }
    if (padding < 0) {
        padding = 0;
    }
    if (g->padding == padding) {
        return;
    }
    g->padding = padding;
    fdk_widget_child_layout_changed(grid);
}

void fdk_grid_set_homogeneous(fdk_widget *grid, bool homogeneous) {
    fdk_grid *g = as_grid(grid);
    if (g == NULL) {
        return;
    }
    if (g->homogeneous == homogeneous) {
        return;
    }
    g->homogeneous = homogeneous;
    fdk_widget_child_layout_changed(grid);
}

void fdk_grid_set_column_expand(fdk_widget *grid, fdk_i32 column, bool expand) {
    fdk_grid *g = as_grid(grid);
    if (g == NULL) {
        return;
    }
    if (column < 0 || column >= g->cols) {
        return; /* out of range: silent — the grid may grow later */
    }
    if (!!g->col_expand[column] == !!expand) {
        return;
    }
    g->col_expand[column] = expand ? 1 : 0;
    fdk_widget_child_layout_changed(grid);
}

void fdk_grid_set_row_expand(fdk_widget *grid, fdk_i32 row, bool expand) {
    fdk_grid *g = as_grid(grid);
    if (g == NULL) {
        return;
    }
    if (row < 0 || row >= g->rows) {
        return;
    }
    if (!!g->row_expand[row] == !!expand) {
        return;
    }
    g->row_expand[row] = expand ? 1 : 0;
    fdk_widget_child_layout_changed(grid);
}
