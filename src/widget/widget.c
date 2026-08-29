#define FDK_LOG_TAG "widget"

#include "widget/widget_internal.h"
#include "widgets_internal.h" /* fdk__a11y_notify */

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- internal limits ----
 *
 * Tree depth is capped at create time: the paint walk, hit-testing,
 * and focus traversal all recurse over the tree, the bubble chain is
 * a fixed array, and the paint-time clip stack is bounded by
 * FDK_SURFACE_CLIP_DEPTH (32). 256 levels is far beyond any sane
 * interface tree and keeps every recursion bounded. */
#define FDK_WIDGET_MAX_DEPTH 256

/* Ancestor chain captured before user callbacks run (a callback may
 * destroy widgets — including ancestors of the widget being
 * dispatched — and the guard machinery keeps their memory alive but
 * unlinks them, so the pre-captured chain is the safe walk order). */
#define FDK_WIDGET_MAX_BUBBLE_DEPTH 64

/* ---- rect helpers (i64 intermediates: two intersecting rects in
 * window space can't overflow, but the SUMS in union/edge math can if
 * fed hostile values from a subclass's set_bounds — cheap insurance
 * under -Wconversion too). ---- */

static bool rect_empty(fdk_rect r) {
    return r.width <= 0 || r.height <= 0;
}

static bool rects_intersect(fdk_rect a, fdk_rect b) {
    return (int64_t)a.x < (int64_t)b.x + (int64_t)b.width &&
           (int64_t)b.x < (int64_t)a.x + (int64_t)a.width &&
           (int64_t)a.y < (int64_t)b.y + (int64_t)b.height &&
           (int64_t)b.y < (int64_t)a.y + (int64_t)a.height;
}

static fdk_rect rect_intersect(fdk_rect a, fdk_rect b) {
    int64_t x0 = (int64_t)a.x > (int64_t)b.x ? (int64_t)a.x : (int64_t)b.x;
    int64_t y0 = (int64_t)a.y > (int64_t)b.y ? (int64_t)a.y : (int64_t)b.y;
    int64_t x1 = (int64_t)a.x + a.width < (int64_t)b.x + b.width
                     ? (int64_t)a.x + a.width
                     : (int64_t)b.x + b.width;
    int64_t y1 = (int64_t)a.y + a.height < (int64_t)b.y + b.height
                     ? (int64_t)a.y + a.height
                     : (int64_t)b.y + b.height;
    if (x0 >= x1 || y0 >= y1) {
        return (fdk_rect){0, 0, 0, 0};
    }
    return (fdk_rect){(fdk_i32)x0, (fdk_i32)y0,
                      (fdk_i32)(x1 - x0), (fdk_i32)(y1 - y0)};
}

static fdk_rect rect_union(fdk_rect a, fdk_rect b) {
    if (rect_empty(a)) {
        return b;
    }
    if (rect_empty(b)) {
        return a;
    }
    int64_t x0 = (int64_t)a.x < (int64_t)b.x ? (int64_t)a.x : (int64_t)b.x;
    int64_t y0 = (int64_t)a.y < (int64_t)b.y ? (int64_t)a.y : (int64_t)b.y;
    int64_t ax1 = (int64_t)a.x + a.width;
    int64_t bx1 = (int64_t)b.x + b.width;
    int64_t ay1 = (int64_t)a.y + a.height;
    int64_t by1 = (int64_t)b.y + b.height;
    return (fdk_rect){(fdk_i32)x0, (fdk_i32)y0,
                      (fdk_i32)((ax1 > bx1 ? ax1 : bx1) - x0),
                      (fdk_i32)((ay1 > by1 ? ay1 : by1) - y0)};
}

/* ---- topology helpers ---- */

static fdk_widget *find_root(fdk_widget *w) {
    if (w == NULL) {
        return NULL;
    }
    while (w->parent != NULL) {
        w = w->parent;
    }
    return w;
}

static const fdk_widget *find_root_const(const fdk_widget *w) {
    if (w == NULL) {
        return NULL;
    }
    while (w->parent != NULL) {
        w = w->parent;
    }
    return w;
}

/* Absolute (root-local) origin of w's top-left corner. The ROOT's own
 * bounds.x/y are deliberately ignored — the root IS the coordinate
 * space, so root-local == window coordinates for window-attached
 * trees (the window glue keeps the root at (0, 0, w, h)). */
static fdk_point abs_pos(const fdk_widget *w) {
    fdk_i32 x = 0;
    fdk_i32 y = 0;
    for (const fdk_widget *cur = w; cur->parent != NULL; cur = cur->parent) {
        x += cur->bounds.x;
        y += cur->bounds.y;
    }
    return (fdk_point){x, y};
}

static fdk_rect absolute_bounds(const fdk_widget *w) {
    fdk_point p = abs_pos(w);
    return (fdk_rect){p.x, p.y, w->bounds.width, w->bounds.height};
}

static bool effective_visible(const fdk_widget *w) {
    for (const fdk_widget *cur = w; cur != NULL; cur = cur->parent) {
        if ((cur->flags & FDK_WF_VISIBLE) == 0) {
            return false;
        }
    }
    return true;
}

static bool effective_enabled(const fdk_widget *w) {
    for (const fdk_widget *cur = w; cur != NULL; cur = cur->parent) {
        if ((cur->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
    }
    return true;
}

static int widget_depth(const fdk_widget *w) {
    int depth = 0;
    for (const fdk_widget *cur = w; cur != NULL; cur = cur->parent) {
        depth++;
    }
    return depth;
}

/* Is `candidate` w itself or a descendant of w? (Subtree test — used
 * for focus/hover/grab bookkeeping when a subtree changes state.) */
static bool is_in_subtree(const fdk_widget *candidate, const fdk_widget *w) {
    for (const fdk_widget *cur = candidate; cur != NULL; cur = cur->parent) {
        if (cur == w) {
            return true;
        }
    }
    return false;
}

/* ---- child array (z-order) ---- */

static fdk_result child_append(fdk_widget *parent, fdk_widget *child) {
    if (parent->child_count == parent->child_capacity) {
        size_t new_capacity = (parent->child_capacity == 0)
            ? 4
            : parent->child_capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(fdk_widget *)) {
            FDK_ERROR("child array capacity overflow");
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_widget **grown = fdk_realloc(
            parent->children, new_capacity * sizeof(fdk_widget *));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        parent->children = grown;
        parent->child_capacity = new_capacity;
    }
    parent->children[parent->child_count++] = child;
    return FDK_OK;
}

/* Order-preserving removal (z-order of remaining siblings must not
 * scramble — swap-remove would silently reshuffle the stack). */
static void child_remove(fdk_widget *parent, fdk_widget *child) {
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            memmove(&parent->children[i], &parent->children[i + 1],
                    (parent->child_count - i - 1) * sizeof(fdk_widget *));
            parent->child_count--;
            return;
        }
    }
}

static void child_move_to_end(fdk_widget *parent, fdk_widget *child) {
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            memmove(&parent->children[i], &parent->children[i + 1],
                    (parent->child_count - i - 1) * sizeof(fdk_widget *));
            parent->children[parent->child_count - 1] = child;
            return;
        }
    }
}

static void child_move_to_front(fdk_widget *parent, fdk_widget *child) {
    for (size_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child) {
            memmove(&parent->children[1], &parent->children[0],
                    i * sizeof(fdk_widget *));
            parent->children[0] = child;
            return;
        }
    }
}

/* ---- base class ---- */

static void base_paint(fdk_widget *widget, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    if (widget->background.a <= 0.0f) {
        return; /* transparent default: paints nothing */
    }
    if (widget->corner_radius > 0) {
        fdk_surface_fill_rounded_rect(surface, bounds,
                                      widget->corner_radius,
                                      widget->background);
    } else {
        fdk_surface_fill_rect(surface, bounds, widget->background);
    }
}

static const fdk_widget_class fdk_widget_base_class_def = {
    .size = sizeof(fdk_widget),
    .name = "widget",
    .handle_event = NULL,
    .paint = base_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
};

/* Window-owned roots use this class: the base widget + the WINDOW
 * accessibility role, so the a11y tree's roots announce themselves.
 * Standalone (test) roots keep the plain base class. */
static const fdk_a11y_class window_root_a11y = {
    .role = FDK_A11Y_ROLE_WINDOW,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

static const fdk_widget_class window_root_class_def = {
    .size = sizeof(fdk_widget),
    .name = "window-root",
    .handle_event = NULL,
    .paint = base_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &window_root_a11y,
};

const fdk_widget_class *fdk__widget_window_root_class(void) {
    return &window_root_class_def;
}

const fdk_widget_class *fdk_widget_base_class(void) {
    return &fdk_widget_base_class_def;
}

/* ---- damage bookkeeping ---- */

static void damage_union(fdk_widget *root, fdk_rect r) {
    if (root == NULL || rect_empty(r)) {
        return;
    }
    if (!root->has_damage) {
        root->damage = r;
        root->has_damage = true;
    } else {
        root->damage = rect_union(root->damage, r);
    }
}

/* ---- reentrancy guards + deferred destroy ---- */

static void guard_enter(fdk_widget *root) {
    if (root != NULL) {
        root->dispatch_depth++;
    }
}

static void guard_leave(fdk_widget *root) {
    if (root == NULL) {
        return;
    }
    root->dispatch_depth--;
    fdk_widget_root_flush_deferred(root);
}

static fdk_result deferred_append(fdk_widget *root, fdk_widget *w) {
    if (root->deferred_count == root->deferred_capacity) {
        size_t new_capacity = (root->deferred_capacity == 0)
            ? 4
            : root->deferred_capacity * 2;
        if (new_capacity > SIZE_MAX / sizeof(fdk_widget *)) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_widget **grown = fdk_realloc(
            root->deferred_destroy, new_capacity * sizeof(fdk_widget *));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        root->deferred_destroy = grown;
        root->deferred_capacity = new_capacity;
    }
    root->deferred_destroy[root->deferred_count++] = w;
    return FDK_OK;
}

/* Recursively tears down a widget's subtree: subclass destroy hook
 * first (it may still reference its children), then the children,
 * then the arrays. Children are marked DESTROYING so a reentrant
 * fdk_widget_destroy() from any hook is a no-op rather than a double
 * free. */
static void teardown_free(fdk_widget *w) {
    if (w->klass->destroy != NULL) {
        w->klass->destroy(w);
    }
    while (w->child_count > 0) {
        fdk_widget *child = w->children[0];
        child_remove(w, child);
        child->parent = NULL;
        child->flags |= FDK_WF_DESTROYING;
        teardown_free(child);
    }
    fdk__layout_batch_forget(w); /* pending batch entries die here */
    fdk_free(w->children);
    fdk_free(w->name);
    fdk_free(w->a11y_name);
    fdk_free(w->a11y_description);
    /* A11y: drop every relation edge that touched this widget —
     * ours, and the inverse copies stored on the targets — so no
     * dangling relation target can outlive the widget. (The list is
     * NULL for the overwhelming majority of widgets; one branch.) */
    fdk__a11y_relations_destroyed(w);
    fdk_free(w);
}

void fdk_widget_root_flush_deferred(fdk_widget *root) {
    if (root == NULL) {
        return;
    }
    if (root->dispatch_depth > 0 || root->paint_depth > 0) {
        return; /* still inside dispatch/paint; the outermost frame flushes */
    }

    while (root->deferred_count > 0) {
        /* Detach the list BEFORE freeing anything: the root itself may
         * be one of the deferred widgets (destroyed from inside its
         * own event dispatch), and teardown hooks may destroy yet more
         * widgets — with both guards at zero those free immediately,
         * so the list can't regrow behind our back. */
        fdk_widget **list = root->deferred_destroy;
        size_t count = root->deferred_count;
        root->deferred_destroy = NULL;
        root->deferred_count = 0;
        root->deferred_capacity = 0;

        bool root_in_list = false;
        for (size_t i = 0; i < count; i++) {
            if (list[i] == root) {
                root_in_list = true;
                continue; /* freed last, below */
            }
            teardown_free(list[i]);
        }
        fdk_free(list);

        if (root_in_list) {
            /* Free the root itself. Its list was already detached
             * above; nothing touches root's fields after this. */
            teardown_free(root);
            return;
        }
    }
}

/* ---- event delivery ---- */

/* Rewrites an event's position fields from root coordinates into
 * `w`-local coordinates. Key/focus events carry no position. */
static void localize_event(fdk_widget_event *ev, const fdk_widget *w) {
    fdk_point origin = abs_pos(w);
    fdk_f32 ox = (fdk_f32)origin.x;
    fdk_f32 oy = (fdk_f32)origin.y;
    switch (ev->type) {
        case FDK_WIDGET_POINTER_ENTER:
        case FDK_WIDGET_POINTER_LEAVE:
        case FDK_WIDGET_POINTER_MOTION:
            ev->position.x -= ox;
            ev->position.y -= oy;
            break;
        case FDK_WIDGET_POINTER_DOWN:
        case FDK_WIDGET_POINTER_UP:
            ev->pointer.position.x -= ox;
            ev->pointer.position.y -= oy;
            break;
        case FDK_WIDGET_SCROLL:
            ev->scroll.position.x -= ox;
            ev->scroll.position.y -= oy;
            break;
        default:
            break;
    }
}

/* Delivers to ONE widget: class hook first, then the per-instance
 * callback — both always see the event (the class hook's return only
 * participates in the handled verdict). Returns the handled verdict. */
static bool deliver_to(fdk_widget *w, fdk_widget_event *ev) {
    bool handled = false;
    ev->widget = w;
    if (w->klass->handle_event != NULL) {
        handled = w->klass->handle_event(w, ev);
    }
    if (w->event_callback != NULL) {
        if (w->event_callback(w, ev, w->event_callback_user_data)) {
            handled = true;
        }
    }
    return handled;
}

/* Deliver to one widget (no bubbling), with position localized and
 * the reentrancy guard held. */
static bool deliver_single(fdk_widget *root, fdk_widget *target,
                           const fdk_widget_event *ev) {
    fdk_widget_event local = *ev;
    localize_event(&local, target);
    guard_enter(root);
    bool handled = deliver_to(target, &local);
    guard_leave(root);
    return handled;
}

/* Deliver to target and, while unhandled, to each ancestor (position
 * re-localized per widget). ENTER/LEAVE/FOCUS events never bubble —
 * callers use deliver_single for those. */
static bool deliver_bubbling(fdk_widget *root, fdk_widget *target,
                             const fdk_widget_event *ev) {
    fdk_widget *chain[FDK_WIDGET_MAX_BUBBLE_DEPTH];
    size_t depth = 0;
    for (fdk_widget *cur = target;
         cur != NULL && depth < FDK_WIDGET_MAX_BUBBLE_DEPTH;
         cur = cur->parent) {
        chain[depth++] = cur;
    }
    if (depth == FDK_WIDGET_MAX_BUBBLE_DEPTH && target->parent != NULL) {
        /* Chain longer than the fixed array: truncate. Trees this deep
         * are already far past FDK_WIDGET_MAX_DEPTH's intent; the
         * deepest 64 still receive the event. */
        FDK_WARN("bubble chain truncated at %d levels",
                 FDK_WIDGET_MAX_BUBBLE_DEPTH);
    }

    guard_enter(root);
    bool handled = false;
    for (size_t i = 0; i < depth; i++) {
        if ((chain[i]->flags & FDK_WF_DESTROYING) != 0) {
            continue; /* unlinked mid-bubble by an earlier handler */
        }
        fdk_widget_event local = *ev;
        localize_event(&local, chain[i]);
        if (deliver_to(chain[i], &local)) {
            handled = true;
            break;
        }
    }
    guard_leave(root);
    return handled;
}

/* ---- root bookkeeping: focus / hover / grab drops ---- */

static void drop_focus_inside(fdk_widget *root, fdk_widget *subtree) {
    if (root->focused == NULL) {
        return;
    }
    if (!is_in_subtree(root->focused, subtree)) {
        return;
    }
    fdk_widget_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = FDK_WIDGET_FOCUS_OUT;

    fdk_widget *old = root->focused;
    root->focused = NULL;
    if ((old->flags & FDK_WF_DESTROYING) == 0) {
        old->flags &= ~FDK_WF_FOCUSED;
        (void)deliver_single(root, old, &ev);
    }
}

static void drop_hover_inside(fdk_widget *root, fdk_widget *subtree) {
    if (root->hovered == NULL) {
        return;
    }
    if (!is_in_subtree(root->hovered, subtree)) {
        return;
    }
    fdk_widget_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = FDK_WIDGET_POINTER_LEAVE;

    fdk_widget *old = root->hovered;
    root->hovered = NULL;
    if ((old->flags & FDK_WF_DESTROYING) == 0) {
        old->flags &= ~FDK_WF_HOVERED;
        (void)deliver_single(root, old, &ev);
    }
}

static void drop_grab_inside(fdk_widget *root, fdk_widget *subtree) {
    if (root->grab != NULL && is_in_subtree(root->grab, subtree)) {
        root->grab = NULL; /* silent: grabs are positional, not owned */
    }
}

/* Sets the tree's hover target to `hit` (may be NULL), delivering
 * LEAVE/ENTER to the widgets that changed. Returns true if any of
 * those deliveries was handled. */
static bool set_hovered(fdk_widget *root, fdk_widget *hit) {
    if (root->hovered == hit) {
        return false;
    }
    bool handled = false;

    if (root->hovered != NULL && (root->hovered->flags & FDK_WF_DESTROYING) == 0) {
        fdk_widget_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = FDK_WIDGET_POINTER_LEAVE;
        fdk_widget *old = root->hovered;
        root->hovered = NULL;
        old->flags &= ~FDK_WF_HOVERED;
        if (deliver_single(root, old, &ev)) {
            handled = true;
        }
    }

    root->hovered = hit;
    if (hit != NULL) {
        hit->flags |= FDK_WF_HOVERED;
        fdk_widget_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = FDK_WIDGET_POINTER_ENTER;
        if (deliver_single(root, hit, &ev)) {
            handled = true;
        }
    }
    return handled;
}

/* ---- hit testing ---- */

/* `origin` is w's absolute position. Point-in-rect uses half-open
 * pixel bounds (pixel centers); float tolerance follows the event's
 * float precision, which is finer. */
static fdk_widget *hit_test_rec(fdk_widget *w, fdk_f32 origin_x,
                                fdk_f32 origin_y, fdk_pointf pos) {
    if ((w->flags & FDK_WF_VISIBLE) == 0 ||
        (w->flags & FDK_WF_ENABLED) == 0) {
        return NULL; /* hidden/disabled subtrees are input-transparent */
    }
    for (size_t i = w->child_count; i-- > 0;) {
        fdk_widget *child = w->children[i];
        fdk_f32 cx = origin_x + (fdk_f32)child->bounds.x;
        fdk_f32 cy = origin_y + (fdk_f32)child->bounds.y;
        if (pos.x >= cx && pos.x < cx + (fdk_f32)child->bounds.width &&
            pos.y >= cy && pos.y < cy + (fdk_f32)child->bounds.height) {
            fdk_widget *hit = hit_test_rec(child, cx, cy, pos);
            if (hit != NULL) {
                return hit;
            }
            /* An ineligible child (or its subtree) declined — keep
             * looking at lower siblings, then fall back to w. */
        }
    }
    return w; /* w itself is eligible (checked on entry) */
}

static fdk_widget *hit_test(fdk_widget *root, fdk_pointf pos) {
    if (pos.x < 0.0f || pos.y < 0.0f ||
        pos.x >= (fdk_f32)root->bounds.width ||
        pos.y >= (fdk_f32)root->bounds.height) {
        return NULL; /* outside the root == outside the window; nothing
                        can be hit (children can't paint there either —
                        the paint walk clips to the root's bounds) */
    }
    return hit_test_rec(root, 0.0f, 0.0f, pos);
}

/* ---- lifecycle ---- */

/* ---- Root registry ----
 *
 * Every live root (window-owned or standalone) is linked into one
 * global doubly-linked list. The theme engine walks it on a
 * default-theme switch to damage every tree (see
 * fdk__widget_roots_invalidate_all below). Roots can never be
 * reparented into a tree, so membership changes only here: create
 * (parent == NULL) links in, destroy unlinks at entry — before the
 * deferred-free machinery, so a root pending teardown is already
 * invisible to the walk. */
static fdk_widget *g_roots;

fdk_widget *fdk__widget_roots_head(void) {
    return g_roots;
}

static void root_registry_add(fdk_widget *root) {
    root->root_prev = NULL;
    root->root_next = g_roots;
    if (g_roots != NULL) {
        g_roots->root_prev = root;
    }
    g_roots = root;
}

static void root_registry_remove(fdk_widget *root) {
    if (root->root_prev != NULL) {
        root->root_prev->root_next = root->root_next;
    } else if (g_roots == root) {
        g_roots = root->root_next;
    }
    if (root->root_next != NULL) {
        root->root_next->root_prev = root->root_prev;
    }
    root->root_prev = NULL;
    root->root_next = NULL;
}

/* Runs every theme_hook in `w`'s subtree (including w), skipping
 * widgets being destroyed. Snapshot-based like the tree's other
 * walkers: a hook may destroy widgets (the walk re-validates each
 * child before descending). */
static void subtree_theme_notify(fdk_widget *w) {
    if (w == NULL || (w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (w->theme_hook != NULL) {
        w->theme_hook(w);
    }
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return; /* the hook destroyed this very widget */
    }
    for (size_t i = 0; i < w->child_count; i++) {
        /* child_append/destroy can shift the array mid-walk; re-check
         * bounds every iteration (children only ever shrink during a
         * notify walk — nothing creates widgets here). */
        if (i < w->child_count) {
            subtree_theme_notify(w->children[i]);
        }
    }
}

void fdk__widget_tree_cancel_grab(fdk_widget *any) {
    /* Called from the decoration-band press path (window.c) at the
     * moment the drag is handed to the WM/compositor. The tree is
     * mid-dispatch of that very BUTTON_DOWN (route_event set
     * root->grab BEFORE dispatching to the band handler), so this
     * runs under an active guard — clear the grab directly; the
     * guard's deferred flush at guard_leave handles anything a
     * handler destroyed in between. No release is synthesized: the
     * release belongs to the WM's grab now and will never arrive. */
    fdk_widget *root = find_root(any);
    if (root == NULL) {
        return;
    }
    root->grab = NULL;
}

void fdk__widget_roots_invalidate_all(void) {
    /* Pre-capture the walk order: damage_union is pure bookkeeping,
     * but the theme switch that reached us may itself be running
     * inside a widget callback — and any handler in this process
     * could destroy a root. Walking a captured list is the same
     * discipline the tree walkers use (see the top-of-file comment).
     * The worst case is damaging a just-destroyed root's old bounds:
     * unlinking happens at destroy ENTRY, so this is exactly as
     * stale-proof as the tree's own walks. */
    size_t count = 0;
    for (fdk_widget *r = g_roots; r != NULL; r = r->root_next) {
        count++;
    }
    if (count == 0) {
        return;
    }
    /* Bounded allocation: one pointer per live root. On OOM, damage
     * the roots one by one as we walk (no snapshot) — still correct,
     * just re-entrant into whatever destroy does mid-walk. */
    fdk_widget **snapshot = fdk_alloc(count * sizeof *snapshot);
    size_t n = 0;
    for (fdk_widget *r = g_roots; r != NULL && n < count; r = r->root_next) {
        if (snapshot != NULL) {
            snapshot[n] = r;
        }
        n++;
    }
    if (snapshot != NULL) {
        for (size_t i = 0; i < n; i++) {
            /* Layout-affecting theme consumers first (e.g. the title
             * band's height metric), then the whole-root damage that
             * covers whatever geometry they changed. */
            subtree_theme_notify(snapshot[i]);
            damage_union(snapshot[i], snapshot[i]->bounds);
        }
        fdk_free(snapshot);
    } else {
        for (fdk_widget *r = g_roots; r != NULL; ) {
            fdk_widget *next = r->root_next;
            subtree_theme_notify(r);
            damage_union(r, r->bounds);
            r = next;
        }
    }
}

fdk_result fdk_widget_create(fdk_widget *parent,
                             const fdk_widget_class *klass,
                             fdk_rect bounds,
                             fdk_widget **out_widget) {
    if (out_widget == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (klass != NULL && klass->size < sizeof(fdk_widget)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (parent != NULL && (parent->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT; /* attaching to a dying subtree */
    }
    if (parent != NULL && widget_depth(parent) + 1 > FDK_WIDGET_MAX_DEPTH) {
        FDK_WARN("widget tree deeper than %d levels; refusing create",
                 FDK_WIDGET_MAX_DEPTH);
        return FDK_ERR_INVALID_ARGUMENT;
    }

    const fdk_widget_class *k = (klass != NULL) ? klass
                                                : fdk_widget_base_class();
    fdk_widget *w = fdk_alloc(k->size);
    if (w == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(w, 0, k->size); /* subclass fields start zeroed (documented) */

    w->klass = k;
    w->parent = parent;
    if (bounds.width < 0) {
        bounds.width = 0;
    }
    if (bounds.height < 0) {
        bounds.height = 0;
    }
    w->bounds = bounds;
    w->natural_w = bounds.width;
    w->natural_h = bounds.height;
    w->align_h = FDK_ALIGN_FILL;
    w->align_v = FDK_ALIGN_FILL;
    w->min_w = 0;
    w->min_h = 0;
    w->max_w = 0;
    w->max_h = 0;
    w->baseline = -1;
    w->grid_col = 0;
    w->grid_row = 0;
    w->grid_colspan = 1;
    w->grid_rowspan = 1;
    w->grid_attached = false;
    w->flags = FDK_WF_VISIBLE | FDK_WF_ENABLED;

    if (parent != NULL) {
        fdk_result r = child_append(parent, w);
        if (!fdk_ok(r)) {
            fdk_free(w);
            return r;
        }
    } else {
        root_registry_add(w); /* roots join the global registry */
    }

    /* A freshly created widget has never painted — damage it so the
     * next tree paint actually draws it. */
    damage_union(find_root(w), absolute_bounds(w));

    /* Containers (e.g. boxes) relayout around the new child. */
    fdk_widget_child_layout_changed(parent);

    /* A11y: the subtree gained a node. */
    fdk__a11y_notify(parent, FDK_A11Y_CHILDREN_CHANGED, 0);

    *out_widget = w;
    return FDK_OK;
}

void fdk_widget_destroy(fdk_widget *widget) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if ((widget->flags & FDK_WF_WINDOW_ROOT) != 0) {
        FDK_WARN("refusing fdk_widget_destroy() on a window-owned root; "
                 "destroy the window instead");
        return;
    }

    fdk_widget *root = find_root(widget);
    widget->flags |= FDK_WF_DESTROYING;
    /* Layout batching: a dying widget must never receive a batched
     * flush (the notifier would relayout a detached subtree —
     * harmless — but a DEFERRED free makes the entry dangle — not
     * harmless). Forget it from the pending set now; children are
     * forgotten individually in teardown_free. */
    fdk__layout_batch_forget(widget);

    /* Leave the root registry at entry, before anything can defer: a
     * root pending teardown must already be invisible to the theme
     * engine's invalidate-all walk. (Non-roots are never registered —
     * the later `widget->parent = NULL` below does not affect it.) */
    if (widget->parent == NULL) {
        root_registry_remove(widget);
    }

    /* Damage the region BEFORE unlinking — the absolute bounds need
     * the intact parent chain, and whatever was under this widget
     * must repaint. */
    damage_union(root, absolute_bounds(widget));

    /* Unlink BEFORE any callback can run: no handler may observe the
     * dying widget as part of the tree. */
    if (widget->parent != NULL) {
        fdk_widget *old_parent = widget->parent;
        child_remove(old_parent, widget);
        widget->parent = NULL;
        /* The container the child left relayouts around the gap. */
        fdk_widget_child_layout_changed(old_parent);
        /* A11y: children changed — the subject is already detached,
         * so only scope-NULL (global) subscribers receive this one
         * (subtree scopes cannot contain a detached widget). */
        fdk__a11y_notify(old_parent, FDK_A11Y_CHILDREN_CHANGED, 0);
    }

    /* The guard spans the bookkeeping deliveries: their callbacks may
     * destroy anything (including this tree's root — deferred, since
     * the guard is held, so every pointer in this frame stays valid).
     * The root itself can never be among the deferrals: destroying it
     * again is a no-op (DESTROYING already set). */
    guard_enter(root);
    if (root != NULL) {
        drop_focus_inside(root, widget);
        drop_hover_inside(root, widget);
        drop_grab_inside(root, widget);
    }

    bool outer_activity = (root != NULL) &&
        (root->dispatch_depth > 1 || root->paint_depth > 0);
    if (outer_activity) {
        /* Inside an event dispatch or paint walk on this tree: defer
         * the free (the walker still holds pointers into the tree).
         * On OOM we LEAK rather than free early — a leak is
         * recoverable, a use-after-free is not. */
        if (!fdk_ok(deferred_append(root, widget))) {
            FDK_ERROR("out of memory deferring widget destroy — leaking "
                      "one detached widget");
        }
        guard_leave(root); /* outer frame is still on the stack: no
                            * flush happens here, by design */
    } else {
        /* Release the guard FIRST (settling anything the bookkeeping
         * deliveries deferred — the root cannot be in that set, so it
         * is still valid here), then tear down. Destroys triggered
         * from teardown hooks run with both guards at zero, so they
         * free immediately instead of deferring onto a dying root. */
        guard_leave(root);
        teardown_free(widget); /* widget may BE the root: nothing
                                * touches it after this line */
    }
}

void fdk_widget_set_event_callback(fdk_widget *widget,
                                   fdk_widget_event_fn callback,
                                   void *user_data) {
    if (widget == NULL) {
        return;
    }
    widget->event_callback = callback;
    widget->event_callback_user_data = user_data;
}

void fdk_widget_set_user_data(fdk_widget *widget, void *user_data) {
    if (widget == NULL) {
        return;
    }
    widget->user_data = user_data;
}

void fdk__widget_set_baseline(fdk_widget *widget, fdk_i32 y) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    widget->baseline = y < 0 ? -1 : y;
}

void fdk__widget_set_theme_hook(fdk_widget *widget,
                                void (*hook)(fdk_widget *widget)) {
    if (widget == NULL) {
        return;
    }
    widget->theme_hook = hook;
}

void *fdk_widget_get_user_data(const fdk_widget *widget) {
    return (widget != NULL) ? widget->user_data : NULL;
}

void fdk_widget_set_name(fdk_widget *widget, const char *name) {
    if (widget == NULL) {
        return;
    }
    fdk_free(widget->name);
    widget->name = NULL;
    if (name != NULL && name[0] != '\0') {
        size_t len = strlen(name) + 1;
        widget->name = fdk_alloc(len);
        if (widget->name == NULL) {
            return; /* OOM already logged; name stays unset */
        }
        memcpy(widget->name, name, len);
    }
}

const char *fdk_widget_get_name(const fdk_widget *widget) {
    if (widget == NULL) {
        return NULL;
    }
    if (widget->name != NULL) {
        return widget->name;
    }
    return widget->klass->name;
}

/* ---- hierarchy ---- */

fdk_widget *fdk_widget_parent(const fdk_widget *widget) {
    return (widget != NULL) ? widget->parent : NULL;
}

size_t fdk_widget_child_count(const fdk_widget *widget) {
    return (widget != NULL) ? widget->child_count : 0;
}

fdk_widget *fdk_widget_child_at(const fdk_widget *widget, size_t index) {
    if (widget == NULL || index >= widget->child_count) {
        return NULL;
    }
    return widget->children[index];
}

void fdk_widget_raise(fdk_widget *widget) {
    if (widget == NULL || widget->parent == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_widget *parent = widget->parent;
    if (parent->children[parent->child_count - 1] == widget) {
        return; /* already topmost */
    }
    child_move_to_end(parent, widget);
    fdk_widget_invalidate(widget);
}

void fdk_widget_lower(fdk_widget *widget) {
    if (widget == NULL || widget->parent == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    fdk_widget *parent = widget->parent;
    if (parent->children[0] == widget) {
        return; /* already bottom-most */
    }
    child_move_to_front(parent, widget);
    fdk_widget_invalidate(widget);
}

fdk_result fdk_widget_reparent(fdk_widget *widget, fdk_widget *new_parent) {
    if (widget == NULL || new_parent == NULL || widget == new_parent) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (widget->parent == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* roots can't be reparented */
    }
    if ((widget->flags & FDK_WF_DESTROYING) != 0 ||
        (new_parent->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (is_in_subtree(new_parent, widget)) {
        return FDK_ERR_INVALID_ARGUMENT; /* would create a cycle */
    }

    fdk_widget *old_root = find_root(widget);
    fdk_widget *new_root = find_root(new_parent);

    /* Damage the old region while the chain is intact. */
    damage_union(old_root, absolute_bounds(widget));

    fdk_widget *old_parent = widget->parent;
    child_remove(old_parent, widget);
    fdk_result r = child_append(new_parent, widget);
    if (!fdk_ok(r)) {
        /* Attach failed (OOM): re-attach to the old parent rather than
         * leaking an orphaned subtree. */
        if (!fdk_ok(child_append(old_parent, widget))) {
            FDK_ERROR("reparent rollback failed — subtree orphaned");
            widget->parent = NULL;
        }
        return r;
    }
    widget->parent = new_parent;

    /* Damage the new region BEFORE the drop deliveries below: they
     * are pure bookkeeping until the guards open, and after them a
     * handler may have destroyed parts of either tree. */
    damage_union(new_root, absolute_bounds(widget));

    /* Both containers' layouts changed shape. */
    fdk_widget_child_layout_changed(old_parent);
    fdk_widget_child_layout_changed(new_parent);

    /* A11y: both ends of the move. */
    if (old_parent != NULL) {
        fdk__a11y_notify(old_parent, FDK_A11Y_CHILDREN_CHANGED, 0);
    }
    fdk__a11y_notify(new_parent, FDK_A11Y_CHILDREN_CHANGED, 0);

    /* Cross-tree move: the old root's focus/hover/grab may point into
     * the moved subtree; drop them there (with events), the new root
     * starts clean. */
    if (old_root != new_root) {
        guard_enter(old_root);
        drop_focus_inside(old_root, widget);
        drop_hover_inside(old_root, widget);
        drop_grab_inside(old_root, widget);
        guard_leave(old_root);
    }
    return FDK_OK;
}

bool fdk_widget_is_root(const fdk_widget *widget) {
    return widget != NULL && widget->parent == NULL;
}

/* ---- geometry ---- */

fdk_rect fdk_widget_get_bounds(const fdk_widget *widget) {
    if (widget == NULL) {
        return (fdk_rect){0, 0, 0, 0};
    }
    return widget->bounds;
}

void fdk_widget_set_bounds(fdk_widget *widget, fdk_rect bounds) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (bounds.width < 0) {
        bounds.width = 0;
    }
    if (bounds.height < 0) {
        bounds.height = 0;
    }
    if (bounds.x == widget->bounds.x && bounds.y == widget->bounds.y &&
        bounds.width == widget->bounds.width &&
        bounds.height == widget->bounds.height) {
        return;
    }
    fdk_widget *root = find_root(widget);
    damage_union(root, absolute_bounds(widget)); /* old region */
    widget->bounds = bounds;
    damage_union(root, absolute_bounds(widget)); /* new region */
    /* A11y: geometry moved/resized. Fired after the mutation, before
     * anything can run user code that might destroy the widget. */
    fdk__a11y_notify(widget, FDK_A11Y_BOUNDS_CHANGED, 0);
}

fdk_rect fdk_widget_get_absolute_bounds(const fdk_widget *widget) {
    if (widget == NULL) {
        return (fdk_rect){0, 0, 0, 0};
    }
    return absolute_bounds(widget);
}

/* ---- state ---- */

void fdk_widget_set_visible(fdk_widget *widget, bool visible) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    bool was = (widget->flags & FDK_WF_VISIBLE) != 0;
    if (was == visible) {
        return;
    }
    if (visible) {
        widget->flags |= FDK_WF_VISIBLE;
    } else {
        widget->flags &= ~FDK_WF_VISIBLE;
    }
    /* Invalidate BEFORE any delivery runs: the damage bookkeeping is
     * pure (no callbacks), and once the drop deliveries below run, a
     * handler may destroy arbitrary parts of the tree — including
     * this widget's ancestors — so nothing may be touched after. */
    fdk_widget_invalidate(widget);
    /* A11y: visibility flipped. VISIBLE is the own flag; SHOWING is
     * computed at describe time, so this one notification covers the
     * whole visible-subtree change for observers. */
    fdk__a11y_notify(widget, FDK_A11Y_STATE_CHANGED, FDK_A11Y_VISIBLE);
    fdk_widget *root = find_root(widget);
    if (!visible && root != NULL) {
        guard_enter(root);
        drop_focus_inside(root, widget);
        drop_hover_inside(root, widget);
        drop_grab_inside(root, widget);
        guard_leave(root);
    }
}

bool fdk_widget_get_visible(const fdk_widget *widget) {
    return widget != NULL && (widget->flags & FDK_WF_VISIBLE) != 0;
}

bool fdk_widget_is_effectively_visible(const fdk_widget *widget) {
    return widget != NULL && effective_visible(widget);
}

void fdk_widget_set_enabled(fdk_widget *widget, bool enabled) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    bool was = (widget->flags & FDK_WF_ENABLED) != 0;
    if (was == enabled) {
        return;
    }
    if (enabled) {
        widget->flags |= FDK_WF_ENABLED;
    } else {
        widget->flags &= ~FDK_WF_ENABLED;
    }
    /* A11y: enabled-ness flipped (before the event deliveries below,
     * which may destroy tree parts). */
    fdk__a11y_notify(widget, FDK_A11Y_STATE_CHANGED, FDK_A11Y_ENABLED);
    fdk_widget *root = find_root(widget);
    if (!enabled && root != NULL) {
        /* Focus/hover/grab drops may deliver events whose handlers
         * destroy tree parts; nothing is touched after this block. */
        guard_enter(root);
        drop_focus_inside(root, widget);
        drop_hover_inside(root, widget);
        drop_grab_inside(root, widget);
        guard_leave(root);
    }
    /* Enabled-ness has no default visual (the base widget paints the
     * same background either way); interactive subclasses repaint
     * themselves from their event handlers. */
}

bool fdk_widget_get_enabled(const fdk_widget *widget) {
    return widget != NULL && (widget->flags & FDK_WF_ENABLED) != 0;
}

bool fdk_widget_is_effectively_enabled(const fdk_widget *widget) {
    return widget != NULL && effective_enabled(widget);
}

void fdk_widget_set_can_focus(fdk_widget *widget, bool can_focus) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (can_focus) {
        widget->flags |= FDK_WF_CAN_FOCUS;
        return;
    }
    widget->flags &= ~FDK_WF_CAN_FOCUS;
    fdk_widget *root = find_root(widget);
    if (root != NULL && root->focused == widget) {
        guard_enter(root);
        drop_focus_inside(root, widget);
        guard_leave(root);
    }
}

bool fdk_widget_get_can_focus(const fdk_widget *widget) {
    return widget != NULL && (widget->flags & FDK_WF_CAN_FOCUS) != 0;
}

bool fdk_widget_is_hovered(const fdk_widget *widget) {
    if (widget == NULL) {
        return false;
    }
    const fdk_widget *root = find_root_const(widget);
    return root != NULL && root->hovered == widget;
}

bool fdk_widget_has_focus(const fdk_widget *widget) {
    return widget != NULL && (widget->flags & FDK_WF_FOCUSED) != 0;
}

/* ---- focus ---- */

bool fdk_widget_focus(fdk_widget *widget) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return false;
    }
    if ((widget->flags & FDK_WF_CAN_FOCUS) == 0 ||
        !effective_visible(widget) || !effective_enabled(widget)) {
        return false;
    }
    fdk_widget *root = find_root(widget);
    if (root == NULL) {
        return false;
    }
    fdk_widget *old = root->focused;
    if (old == widget) {
        return true;
    }
    root->focused = widget;

    fdk_widget_event ev;
    memset(&ev, 0, sizeof(ev));

    guard_enter(root);
    if (old != NULL && (old->flags & FDK_WF_DESTROYING) == 0) {
        old->flags &= ~FDK_WF_FOCUSED;
        ev.type = FDK_WIDGET_FOCUS_OUT;
        (void)deliver_single(root, old, &ev);
    }
    widget->flags |= FDK_WF_FOCUSED;
    /* A11y: both ends of the focus move (after the flag flips, in
     * callback-safe positions — the guards are open but the walk is
     * snapshot-based). */
    if (old != NULL && (old->flags & FDK_WF_DESTROYING) == 0) {
        fdk__a11y_notify(old, FDK_A11Y_STATE_CHANGED, FDK_A11Y_FOCUSED);
    }
    fdk__a11y_notify(widget, FDK_A11Y_STATE_CHANGED, FDK_A11Y_FOCUSED);
    ev.type = FDK_WIDGET_FOCUS_IN;
    (void)deliver_single(root, widget, &ev);
    guard_leave(root);
    return true;
}

fdk_widget *fdk_widget_tree_get_focused(fdk_widget *any) {
    fdk_widget *root = find_root(any);
    return (root != NULL) ? root->focused : NULL;
}

void fdk_widget_tree_clear_focus(fdk_widget *any) {
    fdk_widget *root = find_root(any);
    if (root == NULL) {
        return;
    }
    guard_enter(root);
    drop_focus_inside(root, root); /* clears whatever is focused */
    guard_leave(root);
}

/* Depth-first eligible-widget collector for focus traversal (Tab
 * order = child order, depth-first — the order a screen reader walks
 * the tree, and the order users expect). */
typedef struct focus_collector {
    fdk_widget **items;
    size_t count;
    size_t capacity;
    bool oom;
} focus_collector;

static void collect_focusable(fdk_widget *w, focus_collector *col) {
    if ((w->flags & FDK_WF_VISIBLE) == 0 ||
        (w->flags & FDK_WF_ENABLED) == 0 ||
        (w->flags & FDK_WF_DESTROYING) != 0) {
        return; /* whole subtree pruned (own-flag check == effective) */
    }
    if ((w->flags & FDK_WF_CAN_FOCUS) != 0) {
        if (col->count == col->capacity) {
            size_t new_capacity = (col->capacity == 0) ? 8
                                                       : col->capacity * 2;
            fdk_widget **grown = fdk_realloc(
                col->items, new_capacity * sizeof(fdk_widget *));
            if (grown == NULL) {
                col->oom = true;
                return;
            }
            col->items = grown;
            col->capacity = new_capacity;
        }
        col->items[col->count++] = w;
    }
    for (size_t i = 0; i < w->child_count; i++) {
        collect_focusable(w->children[i], col);
    }
}

static fdk_widget *advance_focus(fdk_widget *root, bool backward) {
    focus_collector col = {NULL, 0, 0, false};
    collect_focusable(root, &col);
    if (col.oom) {
        fdk_free(col.items);
        FDK_ERROR("out of memory collecting focus order");
        return NULL;
    }

    fdk_widget *result = NULL;
    if (col.count == 0) {
        guard_enter(root);
        drop_focus_inside(root, root);
        guard_leave(root);
    } else {
        size_t current = col.count; /* "not found" sentinel */
        for (size_t i = 0; i < col.count; i++) {
            if (col.items[i] == root->focused) {
                current = i;
                break;
            }
        }
        size_t next;
        if (backward) {
            next = (current == 0 || current >= col.count)
                ? col.count - 1
                : current - 1;
        } else {
            next = (current >= col.count) ? 0 : (current + 1) % col.count;
        }
        result = col.items[next];
        (void)fdk_widget_focus(result);
    }
    fdk_free(col.items);
    return result;
}

fdk_widget *fdk_widget_tree_advance_focus(fdk_widget *any, bool backward) {
    fdk_widget *root = find_root(any);
    if (root == NULL) {
        return NULL;
    }
    return advance_focus(root, backward);
}

/* ---- invalidation & painting ---- */

void fdk_widget_invalidate(fdk_widget *widget) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    damage_union(find_root(widget), absolute_bounds(widget));
}

void fdk_widget_invalidate_all(fdk_widget *any) {
    fdk_widget *root = find_root(any);
    if (root == NULL) {
        return;
    }
    root->damage = (fdk_rect){0, 0, root->bounds.width,
                              root->bounds.height};
    root->has_damage = root->bounds.width > 0 && root->bounds.height > 0;
}

bool fdk_widget_tree_has_damage(fdk_widget *any) {
    fdk_widget *root = find_root(any);
    return root != NULL && root->has_damage;
}

static void paint_rec(fdk_widget *w, fdk_surface *surface,
                      fdk_f32 origin_x, fdk_f32 origin_y, fdk_rect damage) {
    if ((w->flags & FDK_WF_VISIBLE) == 0) {
        return;
    }
    fdk_rect abs = {
        (fdk_i32)origin_x + w->bounds.x,
        (fdk_i32)origin_y + w->bounds.y,
        w->bounds.width,
        w->bounds.height,
    };
    if (rect_empty(abs) || !rects_intersect(abs, damage)) {
        return;
    }

    /* Constrain this widget (and its children) to its own bounds on
     * the surface's clip stack. Beyond FDK_SURFACE_CLIP_DEPTH levels
     * the push fails: paint unconstrained rather than skip (the
     * primitives still clip to everything already on the stack and to
     * the surface — this only loosens parent-boundary enforcement for
     * absurdly deep trees). */
    bool pushed = fdk_ok(fdk_surface_push_clip(surface, abs));
    if (pushed) {
        fdk_rect clip = fdk_surface_get_clip(surface);
        if (!rect_empty(clip)) {
            void (*paint_hook)(fdk_widget *, fdk_surface *, fdk_rect,
                               fdk_rect) =
                (w->klass->paint != NULL) ? w->klass->paint : base_paint;
            paint_hook(w, surface, abs, clip);
        }
        fdk_f32 child_x = (fdk_f32)abs.x;
        fdk_f32 child_y = (fdk_f32)abs.y;
        for (size_t i = 0; i < w->child_count; i++) {
            paint_rec(w->children[i], surface, child_x, child_y, damage);
        }
        fdk_surface_pop_clip(surface);
    } else {
        FDK_WARN("clip stack exhausted at widget depth; painting "
                 "unconstrained");
        fdk_rect clip = fdk_surface_get_clip(surface);
        void (*paint_hook)(fdk_widget *, fdk_surface *, fdk_rect,
                           fdk_rect) =
            (w->klass->paint != NULL) ? w->klass->paint : base_paint;
        paint_hook(w, surface, abs, clip);
        fdk_f32 child_x = (fdk_f32)abs.x;
        fdk_f32 child_y = (fdk_f32)abs.y;
        for (size_t i = 0; i < w->child_count; i++) {
            paint_rec(w->children[i], surface, child_x, child_y, damage);
        }
    }
}

void fdk_widget_tree_paint(fdk_widget *any, fdk_surface *surface) {
    if (any == NULL || surface == NULL) {
        return;
    }
    fdk_widget *root = find_root(any);
    if (root == NULL || !root->has_damage) {
        return;
    }

    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        return; /* framebuffer unavailable; damage stays pending */
    }
    fdk_rect surface_bounds = {0, 0, info.width, info.height};
    fdk_rect damage = rect_intersect(root->damage, surface_bounds);
    if (rect_empty(damage)) {
        root->has_damage = false;
        return;
    }

    root->paint_depth++;
    if (fdk_ok(fdk_surface_push_clip(surface, damage))) {
        paint_rec(root, surface, 0.0f, 0.0f, damage);
        fdk_surface_pop_clip(surface);
    } else {
        paint_rec(root, surface, 0.0f, 0.0f, damage);
    }
    root->paint_depth--;
    root->has_damage = false;

    /* A paint hook may have destroyed widgets (deferred); free them
     * now that the walk is off the stack. */
    fdk_widget_root_flush_deferred(root);
}

/* ---- event routing ---- */

static bool dispatch_pointer(fdk_widget *root, fdk_widget *target,
                             fdk_widget_event_type type, fdk_pointf pos,
                             fdk_u32 button, fdk_f32 dx, fdk_f32 dy,
                             fdk_u32 modifiers) {
    fdk_widget_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    switch (type) {
        case FDK_WIDGET_POINTER_DOWN:
        case FDK_WIDGET_POINTER_UP:
            ev.pointer.position = pos;
            ev.pointer.button = button;
            ev.pointer.modifiers = modifiers;
            break;
        case FDK_WIDGET_SCROLL:
            ev.scroll.position = pos;
            ev.scroll.delta_x = dx;
            ev.scroll.delta_y = dy;
            break;
        default: /* MOTION */
            ev.position = pos;
            break;
    }
    return deliver_bubbling(root, target, &ev);
}

static bool dispatch_key(fdk_widget *root, fdk_widget *focused,
                         const fdk_key_event *key, bool down) {
    fdk_widget_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? FDK_WIDGET_KEY_DOWN : FDK_WIDGET_KEY_UP;
    ev.key = *key;

    guard_enter(root);
    bool handled = false;
    if (focused != NULL && (focused->flags & FDK_WF_DESTROYING) == 0) {
        handled = deliver_bubbling(root, focused, &ev);
    }
    if (!handled && down && key->scancode == FDK_KEY_TAB &&
        (key->modifiers & ~(fdk_u32)FDK_MOD_SHIFT) == 0) {
        /* Built-in focus traversal: an otherwise-unhandled Tab (or
         * Shift+Tab) moves the tree's focus — including from "nothing
         * focused" to the first (or last) focusable widget — and
         * consumes the key. */
        (void)advance_focus(root, (key->modifiers & FDK_MOD_SHIFT) != 0);
        handled = true;
    }
    guard_leave(root);
    return handled;
}

static bool route_event(fdk_widget *root, const fdk_event_data *event);

bool fdk_widget_tree_handle_event(fdk_widget *any,
                                  const fdk_event_data *event) {
    if (any == NULL || event == NULL) {
        return false;
    }
    fdk_widget *root = find_root(any);
    if (root == NULL) {
        return false;
    }

    /* ONE guard spans the entire routing: hover transitions, hit
     * targets, and the grab may all be invalidated by handlers
     * destroying widgets mid-route, and nothing here may trigger the
     * deferred-free flush while this frame still walks the tree. The
     * flush runs at guard_leave below — after which this function
     * touches nothing but its own locals. */
    guard_enter(root);
    bool handled = route_event(root, event);
    guard_leave(root);
    return handled;
}

static bool route_event(fdk_widget *root, const fdk_event_data *event) {
    switch (event->type) {
        case FDK_EVENT_POINTER_MOTION: {
            fdk_pointf pos = event->pointer.position;
            if (root->grab != NULL) {
                if ((root->grab->flags & FDK_WF_DESTROYING) == 0) {
                    /* While a button is held, motion goes to the
                     * grabbed widget (implicit grab); hover state is
                     * frozen until release. */
                    return dispatch_pointer(root, root->grab,
                                            FDK_WIDGET_POINTER_MOTION,
                                            pos, 0, 0.0f, 0.0f, 0);
                }
                root->grab = NULL; /* destroyed mid-grab */
            }
            fdk_widget *hit = hit_test(root, pos);
            (void)set_hovered(root, hit); /* LEAVE/ENTER deliveries
                                           * don't consume the motion */
            if (hit == NULL) {
                return false;
            }
            return dispatch_pointer(root, hit, FDK_WIDGET_POINTER_MOTION,
                                    pos, 0, 0.0f, 0.0f, 0);
        }

        case FDK_EVENT_POINTER_BUTTON_DOWN: {
            fdk_pointf pos = event->pointer_button.position;
            fdk_widget *target =
                (root->grab != NULL &&
                 (root->grab->flags & FDK_WF_DESTROYING) == 0)
                    ? root->grab
                    : hit_test(root, pos);
            if (target == NULL) {
                return false;
            }
            root->grab = target; /* implicit grab until release */
            return dispatch_pointer(root, target,
                                    FDK_WIDGET_POINTER_DOWN, pos,
                                    event->pointer_button.button,
                                    0.0f, 0.0f,
                                    event->pointer_button.modifiers);
        }

        case FDK_EVENT_POINTER_BUTTON_UP: {
            fdk_pointf pos = event->pointer_button.position;
            fdk_widget *target = root->grab;
            if (target == NULL ||
                (target->flags & FDK_WF_DESTROYING) != 0) {
                target = hit_test(root, pos);
            }
            bool handled = false;
            if (target != NULL) {
                handled = dispatch_pointer(root, target,
                                           FDK_WIDGET_POINTER_UP, pos,
                                           event->pointer_button.button,
                                           0.0f, 0.0f,
                                           event->pointer_button.modifiers);
            }
            root->grab = NULL; /* release always ends the grab */
            return handled;
        }

        case FDK_EVENT_POINTER_SCROLL: {
            fdk_pointf pos = event->scroll.position;
            fdk_widget *target =
                (root->grab != NULL &&
                 (root->grab->flags & FDK_WF_DESTROYING) == 0)
                    ? root->grab
                    : hit_test(root, pos);
            if (target == NULL) {
                return false;
            }
            return dispatch_pointer(root, target, FDK_WIDGET_SCROLL, pos,
                                    0, event->scroll.delta_x,
                                    event->scroll.delta_y, 0);
        }

        case FDK_EVENT_POINTER_ENTER: {
            fdk_pointf pos = event->pointer.position;
            fdk_widget *hit = hit_test(root, pos);
            return set_hovered(root, hit);
        }

        case FDK_EVENT_POINTER_LEAVE: {
            if (root->hovered == NULL) {
                return false;
            }
            fdk_widget *old = root->hovered;
            fdk_widget_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = FDK_WIDGET_POINTER_LEAVE;
            root->hovered = NULL;
            if ((old->flags & FDK_WF_DESTROYING) != 0) {
                return false;
            }
            old->flags &= ~FDK_WF_HOVERED;
            return deliver_single(root, old, &ev);
        }

        case FDK_EVENT_KEY_DOWN:
        case FDK_EVENT_KEY_UP: {
            /* focused may be NULL (nothing focused): dispatch_key
             * still runs, so a Tab can focus the first focusable
             * widget from a cold start. */
            return dispatch_key(root, root->focused, &event->key,
                                event->type == FDK_EVENT_KEY_DOWN);
        }

        case FDK_EVENT_WINDOW_FOCUS: {
            fdk_widget *focused = root->focused;
            if (focused == NULL ||
                (focused->flags & FDK_WF_DESTROYING) != 0) {
                return false; /* not consumed: the app still sees it */
            }
            fdk_widget_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = event->focus.focused ? FDK_WIDGET_FOCUS_IN
                                           : FDK_WIDGET_FOCUS_OUT;
            (void)deliver_single(root, focused, &ev);
            return false; /* window-level focus is never consumed by
                           * widgets — the app callback also receives
                           * it (documented) */
        }

        default:
            /* CONFIGURE / EXPOSE / CLOSE_REQUEST are window-level;
             * the window glue (fdk_window_dispatch_event) handles
             * root-resize and invalidate-all around this call. */
            return false;
    }
}

/* ---- layout hooks ---- */

void fdk_widget_measure(fdk_widget *widget, fdk_size *out_size) {
    if (out_size == NULL) {
        return;
    }
    if (widget == NULL) {
        *out_size = (fdk_size){0, 0};
        return;
    }
    if (widget->klass->measure != NULL) {
        widget->klass->measure(widget, out_size);
    } else {
        /* Default natural size = the widget's size REQUEST (create
         * bounds / set_natural_size), NOT its current bounds: layout
         * assigns bounds, and the request must survive that. */
        *out_size = (fdk_size){widget->natural_w, widget->natural_h};
    }

    /* Min/max constraints (Phase 5 completion): clamped into EVERY
     * measure result, so any container's negotiation respects them —
     * 0 means unconstrained in that dimension. Applied after the
     * hook (never before): the hook's value is the request being
     * clamped, not a hint about the clamps themselves. */
    if (widget->min_w > 0 && out_size->width < widget->min_w) {
        out_size->width = widget->min_w;
    }
    if (widget->min_h > 0 && out_size->height < widget->min_h) {
        out_size->height = widget->min_h;
    }
    if (widget->max_w > 0 && out_size->width > widget->max_w) {
        out_size->width = widget->max_w;
    }
    if (widget->max_h > 0 && out_size->height > widget->max_h) {
        out_size->height = widget->max_h;
    }
}

void fdk_widget_set_size_limits(fdk_widget *widget, fdk_i32 min_w,
                                fdk_i32 min_h, fdk_i32 max_w, fdk_i32 max_h) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    /* Normalize: negatives read as 0 (unconstrained); a max below its
     * min is a contradiction — min wins, clamped into the max. */
    if (min_w < 0) min_w = 0;
    if (min_h < 0) min_h = 0;
    if (max_w < 0) max_w = 0;
    if (max_h < 0) max_h = 0;
    if (max_w > 0 && max_w < min_w) max_w = min_w;
    if (max_h > 0 && max_h < min_h) max_h = min_h;
    if (widget->min_w == min_w && widget->min_h == min_h &&
        widget->max_w == max_w && widget->max_h == max_h) {
        return; /* unchanged — no relayout storm */
    }
    widget->min_w = min_w;
    widget->min_h = min_h;
    widget->max_w = max_w;
    widget->max_h = max_h;
    /* The widget's measured size may have changed -> every ancestor
     * container must re-run its layout (same path as the other
     * hint setters). */
    fdk_widget_child_layout_changed(widget);
}

void fdk_widget_get_size_limits(const fdk_widget *widget, fdk_i32 *out_min_w,
                                fdk_i32 *out_min_h, fdk_i32 *out_max_w,
                                fdk_i32 *out_max_h) {
    fdk_i32 sink = 0;
    if (out_min_w == NULL) out_min_w = &sink;
    if (out_min_h == NULL) out_min_h = &sink;
    if (out_max_w == NULL) out_max_w = &sink;
    if (out_max_h == NULL) out_max_h = &sink;
    if (widget == NULL) {
        *out_min_w = 0;
        *out_min_h = 0;
        *out_max_w = 0;
        *out_max_h = 0;
        return;
    }
    *out_min_w = widget->min_w;
    *out_min_h = widget->min_h;
    *out_max_w = widget->max_w;
    *out_max_h = widget->max_h;
}

bool fdk_widget_get_baseline(const fdk_widget *widget, fdk_i32 *out_y) {
    if (out_y == NULL) {
        return false;
    }
    if (widget == NULL || widget->baseline < 0) {
        *out_y = 0;
        return false;
    }
    *out_y = widget->baseline;
    return true;
}

void fdk_widget_arrange(fdk_widget *widget, fdk_rect assigned) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (widget->klass->arrange != NULL) {
        widget->klass->arrange(widget, assigned);
    } else {
        fdk_widget_set_bounds(widget, assigned);
    }
}

/* ---- base style ---- */

void fdk_widget_set_background(fdk_widget *widget, fdk_color color) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    /* An explicit background is an OVERRIDE: from here on the widget
     * (window roots included) owns this color outright — theme
     * switches no longer re-default it (1.2.1; see the root default
     * in fdk_window_get_root). */
    widget->flags &= ~(unsigned)FDK_WF_ROOT_BG_DEFAULT;
    widget->background = color;
    fdk_widget_invalidate(widget);
}

void fdk_widget_set_corner_radius(fdk_widget *widget, fdk_i32 radius) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (radius < 0) {
        radius = 0;
    }
    widget->corner_radius = radius;
    fdk_widget_invalidate(widget);
}

/* ---- per-child layout hints (fdk_layout.h API; the state lives on
 * the widget, the containers consuming it live in src/layout/) ---- */

static void clamp_zero(fdk_i32 *v) {
    if (*v < 0) {
        *v = 0;
    }
}

void fdk_widget_set_margin(fdk_widget *widget, fdk_i32 left, fdk_i32 top,
                           fdk_i32 right, fdk_i32 bottom) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    clamp_zero(&left);
    clamp_zero(&top);
    clamp_zero(&right);
    clamp_zero(&bottom);
    if (widget->margin_left == left && widget->margin_top == top &&
        widget->margin_right == right && widget->margin_bottom == bottom) {
        return;
    }
    widget->margin_left = left;
    widget->margin_top = top;
    widget->margin_right = right;
    widget->margin_bottom = bottom;
    fdk_widget_child_layout_changed(widget->parent);
}

void fdk_widget_set_expand(fdk_widget *widget, bool horizontal,
                           bool vertical) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (widget->expand_h == horizontal && widget->expand_v == vertical) {
        return;
    }
    widget->expand_h = horizontal;
    widget->expand_v = vertical;
    fdk_widget_child_layout_changed(widget->parent);
}

void fdk_widget_set_align(fdk_widget *widget, fdk_align horizontal,
                          fdk_align vertical) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (widget->align_h == horizontal && widget->align_v == vertical) {
        return;
    }
    widget->align_h = horizontal;
    widget->align_v = vertical;
    fdk_widget_child_layout_changed(widget->parent);
}

void fdk_widget_set_natural_size(fdk_widget *widget, fdk_i32 width,
                                 fdk_i32 height) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    if (width < 0) {
        width = 0;
    }
    if (height < 0) {
        height = 0;
    }
    if (widget->natural_w == width && widget->natural_h == height) {
        return;
    }
    widget->natural_w = width;
    widget->natural_h = height;
    fdk_widget_child_layout_changed(widget->parent);
}

/* ---- root bookkeeping for the window glue ---- */

void fdk_widget_root_resized(fdk_widget *root, fdk_size new_size) {
    if (root == NULL || root->parent != NULL) {
        return;
    }
    fdk_rect bounds = {0, 0, new_size.width, new_size.height};
    fdk_widget_set_bounds(root, bounds);
    /* A resize means a fresh framebuffer at a new size on both
     * backends (content undefined) — everything repaints. */
    fdk_widget_invalidate_all(root);
}

void *fdk__widget_window_owner(fdk_widget *any) {
    fdk_widget *root = (any != NULL) ? find_root(any) : NULL;
    return (root != NULL) ? root->window_owner : NULL;
}
