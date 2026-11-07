/*
 * fdk_layout.h — Faded Dream ToolKit layout engine
 *
 * Phase 5. Layout drives geometry through the Phase 4 widget hooks:
 * a layout CONTAINER is a widget subclass whose measure hook reports
 * the natural size of its children laid out, and whose arrange hook
 * assigns each child its final rect (fdk_widget_arrange — so a child
 * that is itself a container relayouts recursively). No widget ever
 * needs to know what its parent is; layout is pure geometry
 * negotiation downward.
 *
 * The BOX is a linear container (horizontal or vertical) with
 * spacing, padding, and per-child layout hints; the GRID (Phase 5
 * completion) is the two-dimensional counterpart. Shared per-child
 * hints (margins, expand, align incl. BASELINE, min/max size
 * limits) apply to both:
 *
 *   - margins (per side, outside the child's bounds)
 *   - expand (share leftover space along the box's axis; fill the
 *     cross axis)
 *   - align (START / CENTER / END / FILL — how a child that does NOT
 *     expand sits in the cross axis)
 *
 * Natural sizes come from each child's measure hook (default: its
 * current bounds), so the whole tree measures bottom-up and arranges
 * top-down in two passes — the classic model.
 *
 * Window integration: fdk_window_set_content() makes one widget the
 * window's content — automatically arranged to fill the root on set
 * and on every configure (resize), so a box at the top of a window
 * reflows on window resizes with no application code.
 *
 * Threading: UI-thread-only, like the rest of FDK (docs/threading.md).
 */

#ifndef FDK_LAYOUT_H
#define FDK_LAYOUT_H

#include "fdk_error.h"
#include "fdk_types.h"
#include "fdk_widget.h"
#include "fdk_window.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum fdk_orientation {
    FDK_HORIZONTAL = 1,
    FDK_VERTICAL   = 2,
} fdk_orientation;

/* Cross-axis placement for children that do not expand. FILL makes
 * the child occupy the full cross axis (its natural cross size is
 * ignored); the others keep the child's natural cross size and
 * position it inside the available cross space. */
typedef enum fdk_align {
    FDK_ALIGN_FILL     = 0,
    FDK_ALIGN_START    = 1,
    FDK_ALIGN_CENTER   = 2,
    FDK_ALIGN_END      = 3,
    /* Phase 5 completion: align text baselines (boxes' cross axis,
     * grid cells' vertical axis). Children report their text baseline
     * (fdk_widget_get_baseline); children WITHOUT one use their
     * BOTTOM edge as the baseline. Only meaningful for the vertical
     * align hint. */
    FDK_ALIGN_BASELINE = 4,
} fdk_align;

/* ---- Box container ----
 *
 * A widget that lays its children out in a line. Children are
 * arranged when the box itself is arranged (fdk_widget_arrange —
 * which the window content glue, a parent container, or the
 * application drives) and re-measured when the box is measured.
 * Adding/removing/hinting a child relayouts the box immediately. */

fdk_result fdk_box_create(fdk_widget *parent, fdk_orientation orientation,
                          fdk_widget **out_box);

/* Re-runs the layout in the new direction. */
void fdk_box_set_orientation(fdk_widget *box, fdk_orientation orientation);
fdk_orientation fdk_box_get_orientation(const fdk_widget *box);

/* Space between adjacent children (>= 0). */
void fdk_box_set_spacing(fdk_widget *box, fdk_i32 spacing);

/* Uniform inset on all four sides (>= 0). Per-side insets around
 * individual children are the child's MARGINS (see below). */
void fdk_box_set_padding(fdk_widget *box, fdk_i32 padding);

/* Homogeneous mode: every child gets an EQUAL share of the along-axis
 * space (largest natural size ignored, expansion hints ignored along
 * the axis). Off by default. */
void fdk_box_set_homogeneous(fdk_widget *box, bool homogeneous);

/* ---- Per-child layout hints ----
 *
 * Stored ON the child (a child carries its own layout preferences to
 * whatever container ends up holding it). All hints relayout the
 * child's parent container immediately when the child is in one. */

/* Margins: space reserved OUTSIDE the child's bounds, inside its
 * slot. Negative values are clamped to 0. */
void fdk_widget_set_margin(fdk_widget *widget, fdk_i32 left, fdk_i32 top,
                           fdk_i32 right, fdk_i32 bottom);

/* Expand: claim a share of the container's leftover space along that
 * axis. In a horizontal box, horizontal-expand shares the width;
 * vertical-expand fills the cross axis (overriding the align hint).
 * Both default to false. */
void fdk_widget_set_expand(fdk_widget *widget, bool horizontal,
                           bool vertical);

/* Cross-axis placement when the child does not expand into it. */
void fdk_widget_set_align(fdk_widget *widget, fdk_align horizontal,
                          fdk_align vertical);

/* ---- Grid container (Phase 5 completion) ----
 *
 * A two-dimensional container: children occupy (col, row) cells, may
 * span multiple cells, and honor the same per-child hints (margins,
 * align within the cell, expand). Column widths / row heights are the
 * maximum natural size of the children in them (multi-span children
 * distribute any deficit they introduce equally over their span);
 * `fdk_grid_set_column_expand` / `fdk_grid_set_row_expand` mark
 * columns/rows that share the container's EXTRA space when arranged
 * larger than its natural size (window content, expanding parents).
 * Homogeneous mode sizes every column (row) to the largest one.
 *
 *     fdk_widget *grid;
 *     fdk_grid_create(root, 2, 2, &grid);   // rows, columns
 *     fdk_widget *a = fdk_widget_create(grid, NULL, (fdk_rect){0}, &a);
 *     fdk_grid_attach(grid, a, 0, 0, 1, 1); // col, row, colspan, rowspan
 *
 * The grid grows automatically when attach addresses a cell beyond
 * the current dimensions. */

fdk_result fdk_grid_create(fdk_widget *parent, fdk_i32 rows, fdk_i32 columns,
                           fdk_widget **out_grid);

/* Places `child` (which must be a direct child of the grid — the
 * usual pattern creates it with the grid as parent) at (column, row),
 * spanning `colspan` columns and `rowspan` rows (both clamped to >=
 * 1). The grid grows to contain the attachment. Re-attaching a child
 * moves it. */
fdk_result fdk_grid_attach(fdk_widget *grid, fdk_widget *child, fdk_i32 column,
                           fdk_i32 row, fdk_i32 colspan, fdk_i32 rowspan);

/* Uniform gap between columns/rows (>= 0). */
void fdk_grid_set_spacing(fdk_widget *grid, fdk_i32 spacing);

/* Uniform inset on all four sides (>= 0), like the box's. */
void fdk_grid_set_padding(fdk_widget *grid, fdk_i32 padding);

/* Homogeneous mode: all columns share the widest column's width and
 * all rows the tallest row's height (per-axis natural maxima). */
void fdk_grid_set_homogeneous(fdk_widget *grid, bool homogeneous);

/* Marks a column / row as sharing the container's extra along-axis
 * space when arranged larger than natural. Off by default. */
void fdk_grid_set_column_expand(fdk_widget *grid, fdk_i32 column, bool expand);
/* Row expand flag (columns have the column twin). */
void fdk_grid_set_row_expand(fdk_widget *grid, fdk_i32 row, bool expand);

/* ---- Min/max size constraints (Phase 5 completion) ----
 *
 * Clamped into the widget's every MEASURE result (see
 * fdk_widget_measure), so every container — box, grid, whatever
 * arranges next — negotiates within the limits without knowing about
 * them. 0 in a dimension means unconstrained. A max below its min is
 * normalized (min wins). Changing limits relayouts the parent
 * container immediately, like the other hint setters. */

void fdk_widget_set_size_limits(fdk_widget *widget, fdk_i32 min_width,
                                fdk_i32 min_height, fdk_i32 max_width,
                                fdk_i32 max_height);
void fdk_widget_get_size_limits(const fdk_widget *widget, fdk_i32 *out_min_w,
                                fdk_i32 *out_min_h, fdk_i32 *out_max_w,
                                fdk_i32 *out_max_h);

/* ---- Baseline (Phase 5 completion) ----
 *
 * Text-bearing widgets report the y offset of their text baseline
 * from their top; fdk_widget_get_baseline returns false for widgets
 * without one (their bottom edge is used as the baseline when a
 * container aligns FDK_ALIGN_BASELINE). */

bool fdk_widget_get_baseline(const fdk_widget *widget, fdk_i32 *out_y);

/* ---- Window content integration ----
 *
 * Makes `content` the window's single content widget: it is arranged
 * to exactly fill the window's root on every configure, starting
 * immediately. `content` must be (or become) a descendant of the
 * window's root — normally its direct child:
 *
 *     fdk_widget *root = fdk_window_get_root(...);
 *     fdk_widget *box  = fdk_box_create(root, FDK_VERTICAL, &box);
 *     fdk_window_set_content(window, box);
 *
 * The window holds a weak reference: destroying the content widget
 * simply clears the association (detected on the next use — safe).
 * Replacing the content re-arranges the new one. NULL clears it.
 */
void fdk_window_set_content(fdk_window *window, fdk_widget *content);

/* Arranges the window's content (if any) to the root's current
 * bounds. Public because applications driving layout manually (no
 * set_content) can call it for their own top container; for
 * set_content users it happens automatically. */
void fdk_window_layout(fdk_window *window);

/* ---- Layout batching (performance) ------------------------------------
 *
 * Building a large tree widget-by-widget is quadratic-ish without
 * help: every create relayouts the parent AND every ancestor
 * container (the eager contract that keeps geometry correct right
 * after each mutation — the property every other FDK API and test
 * relies on). For bulk construction, wrap the build in a batch:
 *
 *     fdk_layout_begin_batch();
 *     build_my_dialog(root);       // hundreds of creates/sets
 *     fdk_layout_end_batch();      // ONE relayout per dirty chain
 *
 * Inside a batch, layout invalidations MARK the affected containers
 * instead of relayouting them; end_batch relayouts each marked
 * chain's topmost container once (its arrange cascade refreshes
 * every descendant below it). The final geometry is identical to
 * the eager path — the bench suite pins this — only the
 * intermediate work disappears. Batches nest (only the outermost
 * end flushes); an unbalanced end is a harmless no-op; widgets
 * destroyed inside a batch are removed from the pending set, so
 * batches may span widget lifetimes.
 *
 * Reading geometry INSIDE a batch sees pre-batch values (nothing
 * has relayouted yet) — batch around bulk construction, not around
 * interaction with the results. */

void fdk_layout_begin_batch(void);
/* Ends a batch: flushes pending layout (outermost only). */
void fdk_layout_end_batch(void);

#ifdef __cplusplus
}
#endif

#endif /* FDK_LAYOUT_H */
