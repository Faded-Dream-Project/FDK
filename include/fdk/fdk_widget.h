/*
 * fdk_widget.h — Faded Dream ToolKit widget foundation
 *
 * Phase 4: the retained-mode widget object model everything later
 * (layout, core widgets, theming) builds on. A widget is an opaque
 * object in a parent/child tree; one tree hangs off each window's
 * ROOT widget (fdk_window_get_root), but trees are also fully usable
 * detached from any window — which is exactly what makes the whole
 * widget layer testable headless (tests/test_widget.c drives geometry,
 * events, focus, and painting against plain offscreen surfaces).
 *
 * The shape of the model, briefly:
 *
 *   - Geometry: each widget's bounds are PARENT-RELATIVE (fdk_rect);
 *     absolute (window/root) bounds compose by summing up the chain.
 *     Children are not clipped by geometry changes — they are clipped
 *     at PAINT time by the surface clip stack, so a child may sit
 *     outside its parent's bounds and simply never becomes visible.
 *
 *   - Z-order: children paint above their parent, and later siblings
 *     above earlier ones (child order IS z-order; fdk_widget_raise /
 *     fdk_widget_lower reorder within the parent). Hit-testing uses
 *     the same order, topmost first.
 *
 *   - State: each widget owns `visible` and `enabled` flags; the
 *     EFFECTIVE state (fdk_widget_is_effectively_visible / _enabled)
 *     is the AND of the whole ancestor chain. Hidden or disabled
 *     widgets receive no pointer events — hit-testing passes through
 *     them to whatever is underneath.
 *
 *   - Events: window-level events (fdk_event.h) are injected into the
 *     tree with fdk_widget_tree_handle_event(); the tree hit-tests
 *     pointer events, routes keyboard events to the focused widget,
 *     synthesizes per-widget ENTER/LEAVE from motion, maintains an
 *     implicit pointer grab for press→release pairing, and bubbles
 *     unhandled events to ancestors. When a window has a root widget
 *     (fdk_window_get_root), FDK performs this routing automatically
 *     before the application's window event callback — events a widget
 *     handles never reach that callback (documented in
 *     fdk_window_get_root).
 *
 *   - Focus: one widget per tree holds keyboard focus
 *     (fdk_widget_focus / fdk_widget_tree_advance_focus, which
 *     implements Tab / Shift-Tab order as depth-first child order).
 *     Focus requires visible + enabled + can-focus.
 *
 *   - Invalidation: widgets mark themselves dirty with
 *     fdk_widget_invalidate(); the tree accumulates a damage bounding
 *     box, and fdk_widget_tree_paint() repaints only what intersects
 *     it, constrained by the surface clip stack — built directly on
 *     the Phase 3 renderer's damage and clip machinery.
 *
 *   - Subclassing: fdk_widget_class is a public vtable describing the
 *     hook contracts (events, paint, measure, arrange, destroy). The
 *     implementation pattern — embedding fdk_widget as the first
 *     member of a larger struct — lives INSIDE the library (src/,
 *     where the struct layout is visible via widget_internal.h) and
 *     is what FDK's own Phase 5/6 widget sets use. Because fdk_widget
 *     is opaque in the public API (see docs/abi-policy.md),
 *     APPLICATIONS cannot embed it today; they extend behavior with
 *     event callbacks, user data, and the base style setters, and
 *     application-embeddable subclassing is revisited at the ABI
 *     freeze. The class struct is still public so the hook contracts
 *     (which the Phase 5 layout engine drives) are documented API.
 *
 * Threading: UI-thread-only, like the rest of FDK (docs/threading.md).
 */

#ifndef FDK_WIDGET_H
#define FDK_WIDGET_H

#include "fdk_error.h"
#include "fdk_event.h"
#include "fdk_surface.h"
#include "fdk_types.h"
#include "fdk_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Widget events ---- */

typedef enum fdk_widget_event_type {
    /* Pointer entered / left the widget's bounds (synthesized by the
     * tree from window motion events — applications never synthesize
     * these). Positions are widget-local. */
    FDK_WIDGET_POINTER_ENTER = 1,
    FDK_WIDGET_POINTER_LEAVE = 2,

    FDK_WIDGET_POINTER_MOTION = 3,  /* .position: widget-local        */
    FDK_WIDGET_POINTER_DOWN  = 4,   /* .pointer: position + button    */
    FDK_WIDGET_POINTER_UP    = 5,   /* .pointer: position + button    */
    FDK_WIDGET_SCROLL        = 6,   /* .scroll: position + deltas     */

    FDK_WIDGET_KEY_DOWN      = 7,   /* .key: delivered to focus only  */
    FDK_WIDGET_KEY_UP        = 8,   /* .key: delivered to focus only  */

    /* The widget gained or lost the tree's keyboard focus. Delivered
     * both on explicit fdk_widget_focus() changes and when the window
     * itself loses/regains focus (the tree keeps its focus target
     * across window-unfocus; FOCUS_OUT is delivered on window blur and
     * FOCUS_IN again on window focus, matching toolkit convention). */
    FDK_WIDGET_FOCUS_IN      = 9,
    FDK_WIDGET_FOCUS_OUT     = 10,
} fdk_widget_event_type;

typedef struct fdk_widget_event {
    fdk_widget_event_type type;

    /* The widget the event is being delivered to (the hit target, the
     * focused widget, or — while bubbling — an ancestor). */
    fdk_widget *widget;

    union {
        fdk_pointf position; /* ENTER / LEAVE / MOTION: widget-local */
        struct { fdk_pointf position; fdk_u32 button; fdk_u32 modifiers; } pointer; /* DOWN / UP */
        struct { fdk_pointf position; fdk_f32 delta_x, delta_y; } scroll;
        fdk_key_event key; /* KEY_DOWN / KEY_UP                     */
    };
} fdk_widget_event;

/* Widget event callback. Return true if the event is HANDLED — a
 * handled event stops bubbling and (for events routed from a window)
 * is not re-delivered to the application's window event callback.
 * Returning false lets the event continue to ancestors. */
typedef bool (*fdk_widget_event_fn)(fdk_widget *widget,
                                    const fdk_widget_event *event,
                                    void *user_data);

/* ---- Widget class (subclassing vtable) ----
 *
 * Describes a widget implementation's hooks. FDK's own widget types
 * (the base class, and the label/button/... families of later phases)
 * live under src/, embed fdk_widget as the first member of their
 * struct, and fill one of these in — `size` makes the single
 * allocation big enough for the subclass, and the allocation beyond
 * the base fields is zero-initialized. The hook contracts themselves
 * are public API (the Phase 5 layout engine drives measure/arrange
 * through them), but the embedding pattern is internal until the ABI
 * freeze makes the struct layout embeddable for applications.
 *
 * Every hook is optional (NULL) except `size`; the defaults give you
 * a plain colored rectangle widget.
 */
typedef struct fdk_widget_class {
    /* sizeof the subclass struct (>= sizeof(fdk_widget)). Used for the
     * single allocation in fdk_widget_create(). */
    size_t size;

    /* Diagnostic name ("button", "my-panel", ...); shown in logs. May
     * be NULL. */
    const char *name;

    /* Event hook. Runs BEFORE the widget's user event callback for
     * every event delivered to this widget, and (like the user
     * callback) always sees the event — return value only decides
     * whether the event counts as handled (stopping bubbling). This
     * "always runs" rule is what lets a Button subclass track pressed
     * state from DOWN/UP even when the application also handles them.
     * NULL = no class-level handling. */
    bool (*handle_event)(fdk_widget *widget, const fdk_widget_event *event);

    /* Paint hook. Draws the widget (in ROOT/window coordinates — the
     * widget's absolute bounds are handed to you; use them, so paint
     * code never shifts when the widget moves) onto `surface`, clipped
     * by the surface's clip stack to the intersection of the tree's
     * damage region, this widget's bounds, and every ancestor's
     * bounds — hooks cannot paint outside their widget. `clip` is the
     * same intersection as a convenience rect for early-outs. Painting
     * must be idempotent: a widget may be repainted when neighboring
     * damage overlaps it. NULL = the base paint (background fill, see
     * fdk_widget_set_background). */
    void (*paint)(fdk_widget *widget, fdk_surface *surface,
                  fdk_rect bounds, fdk_rect clip);

    /* Measure hook (Phase 5 layout engine's entry point; the hook
     * contract is settled now so layout lands without touching the
     * object model). Write the widget's natural size to *out_size.
     * NULL = natural size is the widget's size request (create-time
     * bounds / fdk_widget_set_natural_size). */
    void (*measure)(fdk_widget *widget, fdk_size *out_size);

    /* Arrange hook — the layout engine's assignment callback (Phase
     * 5). Default (NULL) applies the assigned rect via
     * fdk_widget_set_bounds(). */
    void (*arrange)(fdk_widget *widget, fdk_rect assigned);

    /* Destroy hook: subclass cleanup (release owned strings, etc).
     * Called once, immediately before the widget's memory is freed.
     * NULL = nothing extra. */
    void (*destroy)(fdk_widget *widget);

    /* Accessibility class descriptor (Phase 10): role + dynamic
     * describe/action hooks for fdk_a11y_describe / fdk_a11y_perform.
     * Opaque here (defined in fdk_a11y.h — which includes this
     * header, so it cannot be included back). NULL = the widget is
     * an unknown-role generic. App-defined subclasses may point this
     * at their own static descriptor. Appended per the pre-1.0
     * safe-append ABI policy; designated initializers everywhere
     * mean existing class definitions compile unchanged (field
     * defaults to NULL). */
    const struct fdk_a11y_class *a11y;
} fdk_widget_class;

/* ---- Lifecycle ---- */

/* Creates a widget and attaches it to `parent`'s child list (append =
 * topmost). Pass parent = NULL to create a standalone ROOT widget —
 * the top of a tree; trees not attached to a window are fully
 * functional for geometry/events/painting against offscreen surfaces
 * (this is the headless-test entry point). A window's root is
 * normally obtained via fdk_window_get_root() instead.
 *
 * `klass` selects the widget class; NULL means the base class (a
 * plain colored rectangle). Passing classes is how FDK's internal
 * widget types construct themselves; applications pass NULL and use
 * callbacks/user data/style setters.
 *
 * `bounds` are parent-relative (for a root, root-local — typically
 * (0, 0, width, height)).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - out_widget NULL, klass->size <
 *                              sizeof(fdk_widget), parent is being
 *                              destroyed, or the tree would exceed
 *                              256 levels of depth
 *   FDK_ERR_OUT_OF_MEMORY    - allocation failure
 */
fdk_result fdk_widget_create(fdk_widget *parent,
                             const fdk_widget_class *klass,
                             fdk_rect bounds,
                             fdk_widget **out_widget);

/* Destroys `widget` and its whole subtree, unlinking it from its
 * parent first. The widget becomes unusable the moment this returns:
 * it is detached from the tree immediately (no further events or
 * painting reach it), and the actual free happens either right here
 * or — if called from inside an event dispatch or paint walk on the
 * same tree — deferred to when that dispatch unwinds (a callback is
 * always allowed to destroy widgets, including itself or its
 * ancestors; FDK defers the free rather than freeing memory the
 * dispatcher is still walking).
 *
 * Destroying a window-owned root widget (from fdk_window_get_root) is
 * refused with a warning — the window owns that root and frees it
 * with itself. Destroying a widget that is mid-destruction (deferred
 * from inside a callback) is a safe no-op; destroying an
 * already-freed pointer is ordinary C undefined behavior, like a
 * double free(). NULL is a safe no-op. */
void fdk_widget_destroy(fdk_widget *widget);

/* Sets (or replaces, or removes with NULL) the widget's user event
 * callback — the per-instance, class-independent way to receive the
 * widget's events. The class hook (if any) always runs first; see
 * fdk_widget_class.handle_event for the ordering rules. */
void fdk_widget_set_event_callback(fdk_widget *widget,
                                   fdk_widget_event_fn callback,
                                   void *user_data);

/* ---- User data / naming ---- */

void  fdk_widget_set_user_data(fdk_widget *widget, void *user_data);
void *fdk_widget_get_user_data(const fdk_widget *widget);

/* Copies `name` (owned by the widget, freed at destroy). Purely
 * diagnostic — logs and future debug tooling. NULL clears it. */
void fdk_widget_set_name(fdk_widget *widget, const char *name);
const char *fdk_widget_get_name(const fdk_widget *widget);

/* ---- Hierarchy ---- */

fdk_widget *fdk_widget_parent(const fdk_widget *widget);

/* Number of children (z-order bottom .. top). */
size_t      fdk_widget_child_count(const fdk_widget *widget);

/* Child at `index` (0 = bottom-most). NULL if index >= child_count. */
fdk_widget *fdk_widget_child_at(const fdk_widget *widget, size_t index);

/* Moves the widget to its parent's topmost (paint/input front) or
 * bottom-most position. No-op for roots. Invalidates the widget's
 * region so both the uncovered and covering areas repaint. */
void fdk_widget_raise(fdk_widget *widget);
void fdk_widget_lower(fdk_widget *widget);

/* Moves `widget` (with its subtree) from its current parent to the
 * end of `new_parent`'s child list. Order-preserving for the old
 * parent's remaining children. Refused (FDK_ERR_INVALID_ARGUMENT):
 * widget is a root, new_parent is NULL, widget is an ancestor of
 * new_parent (cycle), or either widget is being destroyed.
 * Invalidates both the old and new absolute regions. */
fdk_result fdk_widget_reparent(fdk_widget *widget, fdk_widget *new_parent);

bool fdk_widget_is_root(const fdk_widget *widget);

/* ---- Geometry ---- */

/* The widget's bounds, parent-relative. */
fdk_rect fdk_widget_get_bounds(const fdk_widget *widget);

/* Sets parent-relative bounds. Invalidates the widget's old AND new
 * absolute regions (a move, resize, or both) — the damage machinery
 * repaints exactly what changed. Children keep their parent-relative
 * bounds and therefore move/resize with the parent. */
void fdk_widget_set_bounds(fdk_widget *widget, fdk_rect bounds);

/* Absolute (root/window) bounds — the parent-relative chain summed.
 * For widgets in a window's tree these are window coordinates,
 * matching the surface's pixel space. */
fdk_rect fdk_widget_get_absolute_bounds(const fdk_widget *widget);

/* ---- Visibility / enabled / focusability ---- */

void fdk_widget_set_visible(fdk_widget *widget, bool visible);
bool fdk_widget_get_visible(const fdk_widget *widget);       /* own flag   */
bool fdk_widget_is_effectively_visible(const fdk_widget *);  /* AND chain  */

void fdk_widget_set_enabled(fdk_widget *widget, bool enabled);
bool fdk_widget_get_enabled(const fdk_widget *widget);       /* own flag   */
bool fdk_widget_is_effectively_enabled(const fdk_widget *);  /* AND chain  */

/* Hiding or disabling a focused widget drops focus (with FOCUS_OUT
 * delivered), and a widget under the pointer stops receiving motion
 * events — hit-testing only ever lands on visible+enabled widgets. */

void fdk_widget_set_can_focus(fdk_widget *widget, bool can_focus);
bool fdk_widget_get_can_focus(const fdk_widget *widget);

/* ---- Pointer / keyboard state queries ---- */

bool fdk_widget_is_hovered(const fdk_widget *widget); /* pointer inside   */
bool fdk_widget_has_focus(const fdk_widget *widget);  /* tree focus       */

/* ---- Focus ---- */

/* Gives the tree's keyboard focus to `widget`. Fails (returns false,
 * no events delivered) unless the widget is effectively visible,
 * effectively enabled, can-focus, and not being destroyed. On
 * success the previously focused widget (if any) receives
 * FDK_WIDGET_FOCUS_OUT and this one FDK_WIDGET_FOCUS_IN. */
bool fdk_widget_focus(fdk_widget *widget);

/* The tree's currently focused widget, or NULL. `any` may be any
 * widget in the tree (the root is resolved internally). */
fdk_widget *fdk_widget_tree_get_focused(fdk_widget *any);

/* Removes focus from the tree (FOCUS_OUT delivered if something was
 * focused). */
void fdk_widget_tree_clear_focus(fdk_widget *any);

/* Moves focus to the next (or previous, with backward = true)
 * focusable widget in depth-first child order, wrapping around. This
 * is the tree's built-in Tab / Shift-Tab behavior — it is what runs
 * automatically when a FDK_KEY_TAB key-down reaches the tree
 * unhandled by any widget. Returns the newly focused widget, or NULL
 * if nothing in the tree is focusable (focus is cleared then). */
fdk_widget *fdk_widget_tree_advance_focus(fdk_widget *any, bool backward);

/* ---- Invalidation & painting ---- */

/* Marks the widget's absolute bounds damaged. Repaint of the region
 * happens at the next fdk_widget_tree_paint() (or fdk_window_paint()
 * for window-attached trees). Called automatically by set_bounds /
 * set_visible / raise / lower / reparent; widget paint hooks call it
 * when their drawn state changes. */
void fdk_widget_invalidate(fdk_widget *widget);

/* Damages the ENTIRE tree (all of the root's bounds) — the
 * repaint-everything escape hatch, used by the window glue on EXPOSE
 * and resize. */
void fdk_widget_invalidate_all(fdk_widget *any);

/* True if the tree has accumulated damage (something is pending
 * repaint). Apps pacing their loop use this to skip painting (and
 * presenting) untouched frames. */
bool fdk_widget_tree_has_damage(fdk_widget *any);

/* Repaints the tree's damaged region onto `surface` and clears the
 * damage. `any` locates the tree (any widget in it; painting always
 * starts at the root — damage is tree-global). Widgets intersecting
 * the damage box are painted in z-order (parents before children,
 * earlier siblings first), each constrained by the clip stack to its
 * own bounds; widgets not intersecting are skipped entirely. With no
 * damage this is a no-op — an unchanged frame costs nothing.
 *
 * Presenting is the caller's business: on a window surface call
 * fdk_surface_present() afterwards (fdk_window_paint() wraps exactly
 * this pair). */
void fdk_widget_tree_paint(fdk_widget *any, fdk_surface *surface);

/* ---- Event injection ----
 *
 * Routes a window-level event (fdk_event.h shapes — the exact structs
 * a window event callback receives) into the tree: pointer events are
 * hit-tested (topmost-first, visible+enabled only) with per-widget
 * ENTER/LEAVE synthesis and an implicit grab between button down and
 * up; key events go to the focused widget; window focus events are
 * translated to the focused widget's FOCUS_IN/FOCUS_OUT; a Tab
 * key-down not handled by any widget advances focus automatically.
 * Unhandled events bubble from the target to its ancestors until
 * handled.
 *
 * Returns true if any widget handled the event. This is the same
 * entry point FDK's window glue uses when a tree is attached to a
 * window — which makes it also the GUI-test-harness injection point
 * of record for widget interaction.
 */
bool fdk_widget_tree_handle_event(fdk_widget *any, const fdk_event_data *event);

/* ---- Layout hooks (Phase 5 builds the engine on these) ---- */

/* The widget's natural size: its measure hook's answer, or — with no
 * hook — the widget's size REQUEST (its create-time bounds, or the
 * last fdk_widget_set_natural_size), deliberately independent of the
 * CURRENT bounds so a container's layout can never destroy a child's
 * request (the classic request/allocate split). */
void fdk_widget_measure(fdk_widget *widget, fdk_size *out_size);

/* Sets the widget's size request (what the default measure hook
 * reports to containers). Clamped to >= 0. Relayouts the parent
 * container. Widgets whose class provides a measure hook ignore
 * this (their hook computes the request). */
void fdk_widget_set_natural_size(fdk_widget *widget, fdk_i32 width,
                                 fdk_i32 height);

/* Assigns the widget's bounds through its arrange hook (default:
 * plain set_bounds). The Phase 5 layout engine's single entry point
 * into geometry assignment. */
void fdk_widget_arrange(fdk_widget *widget, fdk_rect assigned);

/* ---- Base style (the Phase 4 seed of the theme layer) ----
 *
 * The base paint fills the widget's bounds with its background color
 * (plain rect, or rounded when a corner radius is set). A fully
 * transparent background (alpha 0) is the default and paints nothing.
 * Real theme lookups replace these fields in the theme phase; they
 * exist now so trees render recognizably without any subclassing. */

void fdk_widget_set_background(fdk_widget *widget, fdk_color color);
void fdk_widget_set_corner_radius(fdk_widget *widget, fdk_i32 radius);

/* ---- Window integration ---- */

/* Returns (lazily creating) `window`'s root widget — the top of the
 * window's widget tree, sized to the window and kept in sync with
 * every configure. Build your interface by adding children to it.
 *
 * While a root exists, FDK routes the window's pointer, keyboard,
 * and focus events into the tree before your window event callback:
 * events a widget handles are NOT re-delivered to that callback.
 * Window-level events (configure, expose, close-request, and the
 * window focus event itself) always reach the callback — widgets
 * never consume those. EXPOSE and configure automatically invalidate
 * the tree, so a pump loop of
 *
 *     fdk_pump_events(ctx, 15);
 *     fdk_window_paint(window);
 *
 * is a complete rendered widget application (paint is a no-op when
 * nothing is damaged; add fdk_surface_frame_ready() gating for
 * Wayland pacing). */
fdk_result fdk_window_get_root(fdk_window *window, fdk_widget **out_root);

/* Repaints the window's damaged widget tree (if any) and presents the
 * window's surface. Without a root widget this is a no-op returning
 * FDK_OK — applications driving fdk_surface directly keep doing that.
 * The tree itself is created by fdk_window_get_root().
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT  - window or out_root NULL (get_root only)
 *   FDK_ERR_OUT_OF_MEMORY     - root allocation failed (get_root only)
 *   plus whatever fdk_window_get_surface() / fdk_surface_present()
 *   report (FDK_ERR_SURFACE_CREATE, ...) for paint.
 */
fdk_result fdk_window_paint(fdk_window *window);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WIDGET_H */
