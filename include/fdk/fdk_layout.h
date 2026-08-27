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
 * First slice: the BOX — a linear container (horizontal or vertical)
 * with spacing, padding, and per-child layout hints:
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
    FDK_ALIGN_FILL   = 0,
    FDK_ALIGN_START  = 1,
    FDK_ALIGN_CENTER = 2,
    FDK_ALIGN_END    = 3,
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

#ifdef __cplusplus
}
#endif

#endif /* FDK_LAYOUT_H */
