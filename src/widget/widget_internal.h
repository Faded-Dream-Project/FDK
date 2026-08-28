/*
 * widget_internal.h — internal definition of struct fdk_widget
 *
 * Not part of the public API — never installed. The public header
 * (include/fdk/fdk_widget.h) keeps fdk_widget opaque; this layout is
 * visible only to the library's own translation units (and to FDK's
 * future widget subclasses, which live under src/ and embed
 * fdk_widget as their first member — see the class doc there).
 *
 * Tree topology notes:
 *
 *   - `parent` is NULL only for ROOT widgets. Roots carry the
 *     tree-global state (focus target, hover target, pointer grab,
 *     damage box, dispatch/paint reentrancy guards, deferred-destroy
 *     list). Every non-root widget reaches its root by walking
 *     `parent` — there is no back-pointer to maintain.
 *
 *   - `children` is a dynamic array in z-order: index 0 is the
 *     bottom-most (painted first, hit-tested last); the last index is
 *     the top-most. raise()/lower() move entries within it.
 *
 *   - a root is "window-owned" when it came from fdk_window_get_root()
 *     (the window owns and destroys it); standalone roots are the
 *     application's (or the test suite's) to destroy.
 */

#ifndef FDK_WIDGET_INTERNAL_H
#define FDK_WIDGET_INTERNAL_H

#include "fdk/fdk_layout.h"
#include "fdk/fdk_widget.h"

#include <stdbool.h>
#include <stddef.h>

/* Own-flag bits (widget->flags). Effective visibility/enabledness is
 * the AND of the flag over the whole parent chain, computed on demand
 * — chains are short, and state changes are rare relative to reads by
 * hit-testing during event storms. */
#define FDK_WF_VISIBLE   0x1u
#define FDK_WF_ENABLED   0x2u
#define FDK_WF_CAN_FOCUS 0x4u

/* Set the moment fdk_widget_destroy() unlinks a widget: no further
 * events, painting, focus, or reparenting reach it. Memory is freed
 * immediately (no tree activity) or at dispatch unwind (deferred
 * destroy from inside a callback). */
#define FDK_WF_DESTROYING 0x8u

/* Maintained on the widget itself (not just the root) for O(1)
 * fdk_widget_has_focus()/is_hovered(); the root's pointer remains
 * the source of truth — these bits are mirrors kept in sync by the
 * focus/hover setters. */
#define FDK_WF_FOCUSED  0x10u
#define FDK_WF_HOVERED  0x20u

/* Set on window-owned roots: fdk_widget_destroy() refuses them (the
 * owning window is the only legitimate destroyer). */
#define FDK_WF_WINDOW_ROOT 0x40u

/* Layout batching (Phase 11): the container is pending a batched
 * relayout — set by the layout notifier's marking mode, cleared by
 * the flush. Lives on the widget so a mark is one OR and a climb is
 * one TEST, with no side table to keep coherent. */
#define FDK_WF_LAYOUT_DIRTY 0x80u

struct fdk_widget {
    const fdk_widget_class *klass;   /* never NULL (base class at minimum) */
    fdk_widget *parent;              /* NULL for roots                    */
    fdk_widget **children;           /* z-order array, NULL when empty    */
    size_t child_count;
    size_t child_capacity;

    fdk_rect bounds;                 /* parent-relative                  */

    unsigned flags;                  /* FDK_WF_*                         */

    /* Per-instance event callback (independent of the class hook). */
    fdk_widget_event_fn event_callback;
    void *event_callback_user_data;

    /* Theme-change notification (internal; set via
     * fdk__widget_set_theme_hook). Called on a fdk_theme_set_default()
     * switch, on the same walk that invalidates every root, for every
     * live widget that has a hook. Used by layout-affecting theme
     * consumers — the FDK-drawn title band (its height is a theme
     * metric) re-arranges its window here. Runs BEFORE the damage
     * marking, so geometry a hook changes is covered by the same
     * repaint. A hook may destroy widgets (including itself) — the
     * walk is snapshot-based like every other tree walker. */
    void (*theme_hook)(fdk_widget *widget);

    void *user_data;
    char *name;                      /* owned copy, NULL when unset      */

    /* Accessibility overrides (Phase 10): when set, these beat the
     * class descriptor's computed name/description in
     * fdk_a11y_describe(). Owned copies, NULL when unset; freed by
     * teardown. */
    char *a11y_name;
    char *a11y_description;

    /* A11y: explicit relation edges (LABELLED_BY, CONTROLLER_FOR, ...)
     * stored as (type, target) pairs; NULL when the widget has none.
     * Owned by src/widget/a11y.c (struct fdk_a11y_edge — opaque here,
     * forward-declared); torn down with the widget, removing the
     * inverse edges from every target so dangling references cannot
     * exist. */
    struct fdk_a11y_edge *a11y_relations;
    size_t a11y_relation_count;
    size_t a11y_relation_cap;

    /* Base style (Phase 4 theme seed): background fill + corner
     * radius used by the default paint hook. */
    fdk_color background;            /* a == 0 -> no background          */
    fdk_i32 corner_radius;

    /* Per-child layout hints (Phase 5) — carried BY the child for
     * whatever container holds it. See fdk_layout.h. */
    fdk_i32 margin_left, margin_top, margin_right, margin_bottom;
    bool expand_h, expand_v;
    fdk_align align_h, align_v;

    /* Min/max size constraints (Phase 5 completion): clamped into
     * every measure result (fdk_widget_measure). 0 = unconstrained
     * in that dimension. */
    fdk_i32 min_w, min_h, max_w, max_h;

    /* Grid placement (Phase 5 completion), carried BY the child like
     * the other container hints: valid only while grid_attached and
     * the parent is a grid (fdk_grid_attach sets both together, the
     * grid reads them while iterating its children). Storing
     * placement on the child — rather than a side table on the
     * container — means child destruction needs no unlink hook. */
    fdk_i32 grid_col, grid_row, grid_colspan, grid_rowspan;
    bool grid_attached;

    /* Baseline (Phase 5 completion): the y offset (from the widget's
     * top) of its text baseline, or -1 when the widget has no text
     * and no meaningful baseline (containers fall back to the bottom
     * edge for baseline alignment). Set by text-bearing widgets at
     * measure time via fdk__widget_set_baseline. */
    fdk_i32 baseline;

    /* The widget's natural (requested) size: its create-time bounds,
    * adjustable via fdk_widget_set_natural_size. This is what the
    * default measure hook reports — deliberately NOT the current
    * (allocated) bounds, so a container's layout can never destroy
    * the child's size request (the classic request/allocate split). */
    fdk_i32 natural_w, natural_h;

    /* ---- ROOT-ONLY fields (parent == NULL) ---- */

    /* Keyboard focus target. Mirrored by FDK_WF_FOCUSED on the widget. */
    fdk_widget *focused;

    /* Widget currently under the pointer (mirrored by FDK_WF_HOVERED),
     * used to synthesize ENTER/LEAVE from window motion events. */
    fdk_widget *hovered;

    /* Implicit pointer grab: the widget that received the last button
     * press receives all motion and the release until then, even when
     * the pointer leaves its bounds. NULL when no button is held. */
    fdk_widget *grab;

    /* Damage bounding box since the last tree paint, in root coords.
     * `has_damage` false means "nothing pending". A single bounding
     * box (not a rect list): the Phase 3 surface already narrows what
     * actually goes over the wire per-primitive, so tree bookkeeping
     * only needs to bound the repaint walk. */
    fdk_rect damage;
    bool has_damage;

    /* Reentrancy guards: while an event dispatch or paint walk is on
     * the stack, fdk_widget_destroy() defers the actual free into
     * `deferred_destroy` (unlinks still happen immediately). */
    int dispatch_depth;
    int paint_depth;
    fdk_widget **deferred_destroy;
    size_t deferred_count;
    size_t deferred_capacity;

    /* Root registry linkage (root-only, maintained by widget.c): a
     * doubly-linked list of every live root — window-owned and
     * standalone — so the theme engine can invalidate them all on a
     * fdk_theme_set_default() without knowing about windows. Roots
     * can never be reparented into a tree, so membership only changes
     * at create and destroy. NULL-terminated at both ends. */
    fdk_widget *root_prev;
    fdk_widget *root_next;

    /* Root-only, set by the window glue (src/window/window.c): the
     * owning fdk_window as an OPAQUE pointer. The widget layer never
     * dereferences it — Phase 9's sanctioned back-edge is resolved by
     * the window module (fdk__window_context in window_internal.h),
     * the same one-way-opaque discipline the theme engine's root
     * registry uses. NULL on standalone roots. */
    void *window_owner;
};

/* Marks every live root's full bounds damaged, so each tree fully
 * repaints on its next paint walk. Used by the theme engine on a
 * default-theme switch (src/theme/theme.c). */
void fdk__widget_roots_invalidate_all(void);

/* Internal theme-change notification (see struct fdk_widget's
 * theme_hook field). NULL clears. Safe with NULL widget. */
void fdk__widget_set_theme_hook(fdk_widget *widget,
                                void (*hook)(fdk_widget *widget));

/* Internal baseline setter for text-bearing widgets (Phase 5
 * completion): y offset of the text baseline from the widget's top,
 * or -1 for "none". Measured by the public fdk_widget_get_baseline. */
void fdk__widget_set_baseline(fdk_widget *widget, fdk_i32 y);

/* The base widget class (what fdk_widget_create's klass == NULL gives
 * you, and what subclasses that don't override `paint` fall back to):
 * paints the background fill, no event handling, natural size = the
 * current bounds. Exposed here for the window glue and tests. */
const fdk_widget_class *fdk_widget_base_class(void);

/* Root bookkeeping, called by the window glue (src/window/window.c):
 * resize the root (and damage everything) when a configure arrives. */
void fdk_widget_root_resized(fdk_widget *root, fdk_size new_size);

/* Root-only opaque owner (the fdk_window that owns the root, set by
 * the window glue; NULL for standalone roots). Walks to `any`'s root
 * first — safe for any widget in any tree, returns NULL for
 * detached/standalone trees. */
void *fdk__widget_window_owner(fdk_widget *any);

/* Run deferred destroys if the last dispatch/paint unwound. Called by
 * fdk_widget_tree_handle_event / _paint on exit. */
void fdk_widget_root_flush_deferred(fdk_widget *root);

/* The root registry's head (widget.c): the layout batch flush walks
 * every live root's tree in overflow mode. NULL when no roots exist.
 * Iterate with root->root_next. */
fdk_widget *fdk__widget_roots_head(void);

/* Notifies the layout engine that `parent`'s child set (or a child's
 * layout hints) changed — called by the widget core from create /
 * destroy / the hint setters. Implemented in src/layout/box.c;
 * containers relayout, non-containers ignore it. Safe with NULL. */
void fdk_widget_child_layout_changed(fdk_widget *parent);

/* Layout batching (box.c): drop `widget` from the pending-batch set
 * — called by the widget core when a widget is unlinked for
 * destruction, and per-widget in teardown, so a batched flush can
 * never reach freed memory. */
void fdk__layout_batch_forget(fdk_widget *widget);

#endif /* FDK_WIDGET_INTERNAL_H */
