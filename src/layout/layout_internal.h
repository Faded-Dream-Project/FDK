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

/* The box container's class. Exposed (rather than static in box.c)
 * so later layout code and tests can identify boxes. */
extern const struct fdk_widget_class fdk_box_class_def;

#endif /* FDK_LAYOUT_INTERNAL_H */
