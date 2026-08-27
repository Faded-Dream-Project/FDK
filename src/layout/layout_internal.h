/*
 * layout_internal.h — internal definitions for the layout engine.
 *
 * Not part of the public API — never installed. The layout engine is
 * built ENTIRELY from widget subclasses: a container is just a widget
 * whose measure/arrange hooks implement a layout policy (see
 * src/layout/box.c). Nothing here is needed outside src/layout/ except
 * the box class symbol, which src/widget/ never touches (the coupling
 * runs the other way: the widget core notifies the layout engine
 * through fdk_widget_child_layout_changed, declared in
 * widget_internal.h and defined in box.c).
 */

#ifndef FDK_LAYOUT_INTERNAL_H
#define FDK_LAYOUT_INTERNAL_H

#include "widget/widget_internal.h"

/* The box container's struct. Lives here (not in box.c) so container
 * SUBCLASSES — the widget catalog's Frame — can embed fdk_box and
 * reuse the packing hooks via fdk_box_class_def's measure/arrange
 * pointers. title_inset reserves a band at the top of a VERTICAL box
 * (Frame draws its title there); plain boxes leave it 0. */
typedef struct fdk_box {
    fdk_widget base;
    fdk_orientation orientation;
    fdk_i32 spacing;
    fdk_i32 padding;
    fdk_i32 title_inset; /* extra top inset, vertical boxes only */
    bool homogeneous;
} fdk_box;

/* The box container's class. Exposed (rather than static in box.c)
 * so later layout code and tests can identify boxes — and so
 * subclasses can delegate measure/arrange to the box packing. */
void fdk_box_measure_hook(fdk_widget *w, fdk_size *out);
void fdk_box_arrange_hook(fdk_widget *w, fdk_rect assigned);

extern const struct fdk_widget_class fdk_box_class_def;

/* The grid container (Phase 5 completion, src/layout/grid.c). Same
 * subclass pattern: state embedded in the widget allocation, policy
 * in the measure/arrange hooks. */
void fdk_grid_measure_hook(fdk_widget *w, fdk_size *out);
void fdk_grid_arrange_hook(fdk_widget *w, fdk_rect assigned);

extern const struct fdk_widget_class fdk_grid_class_def;

#endif /* FDK_LAYOUT_INTERNAL_H */
