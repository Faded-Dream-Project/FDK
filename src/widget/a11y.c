/* a11y.c — the accessibility core (Phase 10).
 *
 * The tree IS the a11y tree: describe() snapshots a widget's role,
 * name, states, value, and bounds; subscribers observe changes;
 * perform() drives widgets through their own public semantics.
 *
 * Layer position: strictly widget-layer, backend-neutral, verified
 * headless. The consumers are the embedded narrator (see
 * a11y_narrator.c — in-process, per the no-bus policy) and
 * application-side test/automation drivers; every one of them is a
 * CONSUMER of this API — see the header for the design rationale.
 */

#include "fdk/fdk_a11y.h"

#include "core/alloc_internal.h"
#include "widgets_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- role names ---------------------------------------------------- */

static const struct {
    fdk_a11y_role role;
    const char *name;
} k_role_names[] = {
    {FDK_A11Y_ROLE_UNKNOWN, "unknown"},
    {FDK_A11Y_ROLE_WINDOW, "window"},
    {FDK_A11Y_ROLE_DIALOG, "dialog"},
    {FDK_A11Y_ROLE_APPLICATION, "application"},
    {FDK_A11Y_ROLE_PANEL, "panel"},
    {FDK_A11Y_ROLE_SCROLL_AREA, "scroll area"},
    {FDK_A11Y_ROLE_SCROLL_BAR, "scroll bar"},
    {FDK_A11Y_ROLE_LIST, "list"},
    {FDK_A11Y_ROLE_TREE, "tree"},
    {FDK_A11Y_ROLE_TOOLBAR, "toolbar"},
    {FDK_A11Y_ROLE_MENU_BAR, "menu bar"},
    {FDK_A11Y_ROLE_MENU, "menu"},
    {FDK_A11Y_ROLE_TAB_LIST, "tab list"},
    {FDK_A11Y_ROLE_GROUP, "group"},
    {FDK_A11Y_ROLE_LIST_ITEM, "list item"},
    {FDK_A11Y_ROLE_TREE_ITEM, "tree item"},
    {FDK_A11Y_ROLE_MENU_ITEM, "menu item"},
    {FDK_A11Y_ROLE_CHECK_MENU_ITEM, "check menu item"},
    {FDK_A11Y_ROLE_RADIO_MENU_ITEM, "radio menu item"},
    {FDK_A11Y_ROLE_TAB, "tab"},
    {FDK_A11Y_ROLE_OPTION, "option"},
    {FDK_A11Y_ROLE_BUTTON, "button"},
    {FDK_A11Y_ROLE_TOGGLE_BUTTON, "toggle button"},
    {FDK_A11Y_ROLE_CHECK_BOX, "check box"},
    {FDK_A11Y_ROLE_RADIO_BUTTON, "radio button"},
    {FDK_A11Y_ROLE_LABEL, "label"},
    {FDK_A11Y_ROLE_ENTRY, "entry"},
    {FDK_A11Y_ROLE_SLIDER, "slider"},
    {FDK_A11Y_ROLE_SPIN_BUTTON, "spin button"},
    {FDK_A11Y_ROLE_COMBO_BOX, "combo box"},
    {FDK_A11Y_ROLE_PROGRESS_BAR, "progress bar"},
    {FDK_A11Y_ROLE_SEPARATOR, "separator"},
    {FDK_A11Y_ROLE_CANVAS, "canvas"},
    {FDK_A11Y_ROLE_STATUS_BAR, "status bar"},
};

const char *fdk_a11y_role_name(fdk_a11y_role role) {
    for (size_t i = 0; i < sizeof(k_role_names) / sizeof(k_role_names[0]);
         i++) {
        if (k_role_names[i].role == role) {
            return k_role_names[i].name;
        }
    }
    return "unknown";
}

/* ---- info lifecycle -------------------------------------------------- */

void fdk_a11y_info_free(fdk_a11y_info *info) {
    if (info == NULL) {
        return;
    }
    fdk_free(info->name);
    fdk_free(info->description);
    fdk_free(info->value_text);
    memset(info, 0, sizeof(*info));
}

/* Internal helper for class describe hooks: printf-render a value
 * text. Returns NULL on allocation failure (callers treat a NULL
 * value_text as "no rendering", which is lossy but safe). */
char *fdk__a11y_valuef(const char *fmt, double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), fmt, v);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return NULL;
    }
    return fdk__strdup(buf);
}

fdk_result fdk_a11y_describe(const fdk_widget *widget, fdk_a11y_info *out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (widget == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((widget->flags & FDK_WF_DESTROYING) != 0) {
        /* A dying widget has no a11y identity; bridges must not
         * announce it. Distinguishable from a live generic only by
         * the caller's own lifetime tracking, like everything else
         * in FDK's reentrancy discipline. */
        return FDK_ERR_INVALID_ARGUMENT;
    }

    const fdk_a11y_class *a = NULL;
    if (widget->klass != NULL) {
        a = widget->klass->a11y;
    }
    out->role = (a != NULL) ? a->role : FDK_A11Y_ROLE_UNKNOWN;

    /* Core states — computed, never cached, so a snapshot can never
     * disagree with the tree. */
    fdk_a11y_state_set st = 0;
    if ((widget->flags & FDK_WF_ENABLED) != 0) {
        st |= FDK_A11Y_ENABLED;
    }
    if ((widget->flags & FDK_WF_VISIBLE) != 0) {
        st |= FDK_A11Y_VISIBLE;
    }
    if ((widget->flags & FDK_WF_CAN_FOCUS) != 0) {
        st |= FDK_A11Y_FOCUSABLE;
    }
    if ((widget->flags & FDK_WF_FOCUSED) != 0) {
        st |= FDK_A11Y_FOCUSED;
    }
    if (fdk_widget_is_effectively_visible(widget)) {
        st |= FDK_A11Y_SHOWING;
    }
    out->states = st;

    out->bounds = fdk_widget_get_absolute_bounds(widget);

    /* Class dynamics (name, semantic states, value), then the
     * per-widget overrides. */
    if (a != NULL && a->describe != NULL) {
        a->describe(widget, out);
    }
    if (widget->a11y_name != NULL) {
        fdk_free(out->name);
        out->name = fdk__strdup(widget->a11y_name);
    }
    if (widget->a11y_description != NULL) {
        fdk_free(out->description);
        out->description = fdk__strdup(widget->a11y_description);
    }
    return FDK_OK;
}

/* ---- overrides -------------------------------------------------------- */

void fdk_widget_set_accessible_name(fdk_widget *widget, const char *name) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    char *copy = (name != NULL) ? fdk__strdup(name) : NULL;
    if (name != NULL && copy == NULL) {
        return; /* OOM: keep the old name rather than clearing it */
    }
    bool changed = ((widget->a11y_name == NULL) != (copy == NULL)) ||
                   (copy != NULL && widget->a11y_name != NULL &&
                    strcmp(copy, widget->a11y_name) != 0);
    fdk_free(widget->a11y_name);
    widget->a11y_name = copy;
    if (changed) {
        fdk__a11y_notify(widget, FDK_A11Y_NAME_CHANGED, 0);
    }
}

void fdk_widget_set_accessible_description(fdk_widget *widget,
                                           const char *description) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    char *copy = (description != NULL) ? fdk__strdup(description) : NULL;
    if (description != NULL && copy == NULL) {
        return;
    }
    bool changed = ((widget->a11y_description == NULL) != (copy == NULL)) ||
                   (copy != NULL && widget->a11y_description != NULL &&
                    strcmp(copy, widget->a11y_description) != 0);
    fdk_free(widget->a11y_description);
    widget->a11y_description = copy;
    if (changed) {
        fdk__a11y_notify(widget, FDK_A11Y_DESCRIPTION_CHANGED, 0);
    }
}

/* ---- subscribers ------------------------------------------------------ */

typedef struct {
    fdk_a11y_notify_fn fn;
    void *user;
    fdk_widget *scope; /* NULL = everything */
    bool active;
} a11y_sub;

static a11y_sub g_subs[FDK_A11Y_MAX_SUBSCRIBERS];

static bool sub_in_scope(const a11y_sub *s, const fdk_widget *w) {
    if (s->scope == NULL) {
        return true;
    }
    for (const fdk_widget *cur = w; cur != NULL; cur = cur->parent) {
        if (cur == s->scope) {
            return true;
        }
    }
    /* Removed-child events: the widget is already detached, so scope
     * membership cannot be walked. Deliver only when the scope is
     * global — subtree scopes honestly miss detach events of widgets
     * removed FROM them (the widget no longer belongs to the subtree).
     * Bridges that need detach events subscribe globally. */
    return false;
}

fdk_result fdk_a11y_subscribe(fdk_widget *scope, fdk_a11y_notify_fn fn,
                              void *user_data) {
    if (fn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].active && g_subs[i].fn == fn &&
            g_subs[i].user == user_data && g_subs[i].scope == scope) {
            return FDK_OK; /* duplicate: no-op, documented */
        }
    }
    for (size_t i = 0; i < FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        if (!g_subs[i].active) {
            g_subs[i].fn = fn;
            g_subs[i].user = user_data;
            g_subs[i].scope = scope;
            g_subs[i].active = true;
            return FDK_OK;
        }
    }
    return FDK_ERR_LIMIT;
}

fdk_result fdk_a11y_unsubscribe(fdk_widget *scope, fdk_a11y_notify_fn fn,
                                void *user_data) {
    if (fn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        if (g_subs[i].active && g_subs[i].fn == fn &&
            g_subs[i].user == user_data && g_subs[i].scope == scope) {
            g_subs[i].active = false;
            return FDK_OK;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

/* Snapshot discipline: a callback may unsubscribe (or subscribe)
 * mid-notification, and may destroy widgets other than the subject.
 * We walk a copy of the registry and re-check liveness per call. */
void fdk__a11y_notify(fdk_widget *widget, fdk_a11y_event_kind kind,
                      fdk_a11y_state_flag state_flag) {
    if (widget == NULL) {
        return;
    }
    a11y_sub snapshot[FDK_A11Y_MAX_SUBSCRIBERS];
    memcpy(snapshot, g_subs, sizeof(g_subs));
    fdk_a11y_event ev;
    ev.kind = kind;
    ev.widget = widget;
    ev.state_flag = state_flag;
    for (size_t i = 0; i < FDK_A11Y_MAX_SUBSCRIBERS; i++) {
        if (!snapshot[i].active) {
            continue;
        }
        /* Still subscribed right now? (a previous callback may have
         * removed it.) */
        if (!g_subs[i].active || g_subs[i].fn != snapshot[i].fn ||
            g_subs[i].user != snapshot[i].user ||
            g_subs[i].scope != snapshot[i].scope) {
            continue;
        }
        if (!sub_in_scope(&snapshot[i], widget)) {
            continue;
        }
        snapshot[i].fn(&ev, snapshot[i].user);
        /* The subject may have been destroyed by that callback —
         * later subscribers still get the event (the registry
         * snapshot is plain data), which matches the "always runs"
         * rule for the class/user event callbacks: an announcement
         * of a change that just happened is never wrong. */
    }
}

/* ---- actions ---------------------------------------------------------- */

fdk_a11y_action_set fdk_a11y_actions_of(const fdk_widget *widget) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return 0;
    }
    const fdk_a11y_class *a =
        (widget->klass != NULL) ? widget->klass->a11y : NULL;
    if (a == NULL) {
        return 0;
    }
    if (a->actions != NULL) {
        return a->actions(widget);
    }
    return 0;
}

fdk_result fdk_a11y_perform(fdk_widget *widget, fdk_a11y_action action,
                            double value) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (action == 0 ||
        (action & (action - 1)) != 0) { /* exactly one bit */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_a11y_class *a =
        (widget->klass != NULL) ? widget->klass->a11y : NULL;
    if (a == NULL || a->perform == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* FOCUS is universal over focusable widgets — handled here so
     * every class does not re-implement it. */
    if (action == FDK_A11Y_ACTION_FOCUS) {
        if ((widget->flags & FDK_WF_CAN_FOCUS) == 0) {
            return FDK_ERR_UNSUPPORTED;
        }
        return fdk_widget_focus(widget) ? FDK_OK : FDK_ERR_UNSUPPORTED;
    }
    return a->perform(widget, action, value) ? FDK_OK : FDK_ERR_UNSUPPORTED;
}

/* ---- virtual children (painted-row containers) ------------------------ */

static const fdk_a11y_class *a11y_of(const fdk_widget *widget) {
    if (widget == NULL || widget->klass == NULL) {
        return NULL;
    }
    return widget->klass->a11y;
}

size_t fdk_a11y_virtual_count(const fdk_widget *container) {
    if (container == NULL || (container->flags & FDK_WF_DESTROYING) != 0) {
        return 0;
    }
    const fdk_a11y_class *a = a11y_of(container);
    if (a == NULL || a->virtual_count == NULL ||
        a->virtual_describe == NULL) {
        return 0;
    }
    return a->virtual_count(container);
}

fdk_result fdk_a11y_virtual_describe(const fdk_widget *container,
                                     size_t index, fdk_a11y_info *out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (container == NULL || (container->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_a11y_class *a = a11y_of(container);
    if (a == NULL || a->virtual_describe == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (a->virtual_count == NULL || index >= a->virtual_count(container)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    a->virtual_describe(container, index, out);
    return FDK_OK;
}

fdk_a11y_action_set fdk_a11y_virtual_actions(const fdk_widget *container,
                                             size_t index) {
    const fdk_a11y_class *a = a11y_of(container);
    if (a == NULL || a->virtual_actions == NULL ||
        a->virtual_describe == NULL || a->virtual_count == NULL) {
        return 0;
    }
    if (index >= a->virtual_count(container)) {
        return 0;
    }
    return a->virtual_actions(container, index);
}

fdk_result fdk_a11y_virtual_perform(fdk_widget *container, size_t index,
                                    fdk_a11y_action action, double value) {
    if (container == NULL || (container->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (action == 0 || (action & (action - 1)) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_a11y_class *a = a11y_of(container);
    if (a == NULL || a->virtual_perform == NULL ||
        a->virtual_describe == NULL || a->virtual_count == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (index >= a->virtual_count(container)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return a->virtual_perform(container, index, action, value)
               ? FDK_OK
               : FDK_ERR_UNSUPPORTED;
}

/* ---- text interface ---------------------------------------------------- */

bool fdk_a11y_has_text_interface(const fdk_widget *widget) {
    const fdk_a11y_class *a = a11y_of(widget);
    return a != NULL && a->text_length != NULL && a->text_at != NULL;
}

size_t fdk_a11y_text_length(const fdk_widget *widget) {
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_length == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return 0;
    }
    return a->text_length(widget);
}

size_t fdk_a11y_text_caret(const fdk_widget *widget) {
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_caret == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return 0;
    }
    return a->text_caret(widget);
}

bool fdk_a11y_text_selection(const fdk_widget *widget, size_t *anchor,
                             size_t *caret) {
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_selection == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return false;
    }
    return a->text_selection(widget, anchor, caret);
}

fdk_result fdk_a11y_text_at_offset(const fdk_widget *widget,
                                   size_t offset,
                                   fdk_a11y_text_granularity granularity,
                                   char *buf, size_t cap,
                                   size_t *out_start, size_t *out_end) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    if (out_start != NULL) {
        *out_start = 0;
    }
    if (out_end != NULL) {
        *out_end = 0;
    }
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_at == NULL ||
        (widget->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_UNSUPPORTED;
    }
    size_t start = 0;
    size_t end = 0;
    if (!a->text_at(widget, offset, granularity, buf, cap, &start,
                    &end)) {
        buf[0] = '\0';
        return FDK_ERR_UNSUPPORTED;
    }
    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
    return FDK_OK;
}

fdk_result fdk_a11y_text_set_caret(fdk_widget *widget, size_t offset) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_set_caret == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return a->text_set_caret(widget, offset) ? FDK_OK
                                             : FDK_ERR_INVALID_ARGUMENT;
}

fdk_result fdk_a11y_text_set_selection(fdk_widget *widget, size_t anchor,
                                       size_t caret) {
    if (widget == NULL || (widget->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_a11y_class *a = a11y_of(widget);
    if (a == NULL || a->text_set_selection == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return a->text_set_selection(widget, anchor, caret)
               ? FDK_OK
               : FDK_ERR_INVALID_ARGUMENT;
}

/* ---- relationships ----------------------------------------------------
 *
 * Storage: a per-widget array of (type, target) edges. The three
 * paired types are stored symmetrically — adding one direction
 * inserts the mirrored edge on the target — so a query from either
 * end works with no graph reconstruction. All edges touching a
 * destroyed widget are dropped in teardown (widget.c calls
 * fdk__a11y_relations_destroyed). */

struct fdk_a11y_edge {
    fdk_a11y_relation_type type;
    fdk_widget *target;
};

static fdk_a11y_relation_type relation_inverse(fdk_a11y_relation_type t) {
    switch (t) {
    case FDK_A11Y_RELATION_LABEL_FOR:
        return FDK_A11Y_RELATION_LABELLED_BY;
    case FDK_A11Y_RELATION_LABELLED_BY:
        return FDK_A11Y_RELATION_LABEL_FOR;
    case FDK_A11Y_RELATION_DESCRIPTION_FOR:
        return FDK_A11Y_RELATION_DESCRIBED_BY;
    case FDK_A11Y_RELATION_DESCRIBED_BY:
        return FDK_A11Y_RELATION_DESCRIPTION_FOR;
    case FDK_A11Y_RELATION_CONTROLLER_FOR:
        return FDK_A11Y_RELATION_CONTROLLED_BY;
    case FDK_A11Y_RELATION_CONTROLLED_BY:
        return FDK_A11Y_RELATION_CONTROLLER_FOR;
    default:
        return (fdk_a11y_relation_type)0;
    }
}

static bool relation_type_valid(fdk_a11y_relation_type t) {
    return t >= FDK_A11Y_RELATION_LABEL_FOR &&
           t <= FDK_A11Y_RELATION_CONTROLLED_BY;
}

static bool edge_list_contains(const fdk_widget *w,
                               fdk_a11y_relation_type type,
                               const fdk_widget *target) {
    for (size_t i = 0; i < w->a11y_relation_count; i++) {
        if (w->a11y_relations[i].type == type &&
            w->a11y_relations[i].target == target) {
            return true;
        }
    }
    return false;
}

static fdk_result edge_list_add(fdk_widget *w, fdk_a11y_relation_type type,
                                fdk_widget *target) {
    if (w->a11y_relation_count >= FDK_A11Y_MAX_RELATIONS) {
        return FDK_ERR_LIMIT;
    }
    if (w->a11y_relation_count == w->a11y_relation_cap) {
        size_t cap = (w->a11y_relation_cap == 0)
                         ? 4
                         : w->a11y_relation_cap * 2;
        if (cap > FDK_A11Y_MAX_RELATIONS) {
            cap = FDK_A11Y_MAX_RELATIONS;
        }
        struct fdk_a11y_edge *grown = fdk_realloc(
            w->a11y_relations, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        w->a11y_relations = grown;
        w->a11y_relation_cap = cap;
    }
    w->a11y_relations[w->a11y_relation_count].type = type;
    w->a11y_relations[w->a11y_relation_count].target = target;
    w->a11y_relation_count++;
    return FDK_OK;
}

static void edge_list_remove(fdk_widget *w, fdk_a11y_relation_type type,
                             const fdk_widget *target) {
    for (size_t i = 0; i < w->a11y_relation_count; i++) {
        if (w->a11y_relations[i].type == type &&
            w->a11y_relations[i].target == target) {
            memmove(&w->a11y_relations[i],
                    &w->a11y_relations[i + 1],
                    (w->a11y_relation_count - i - 1) *
                        sizeof(*w->a11y_relations));
            w->a11y_relation_count--;
            return;
        }
    }
}

fdk_result fdk_a11y_add_relation(fdk_widget *from,
                                 fdk_a11y_relation_type type,
                                 fdk_widget *to) {
    if (from == NULL || to == NULL || !relation_type_valid(type)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((from->flags & FDK_WF_DESTROYING) != 0 ||
        (to->flags & FDK_WF_DESTROYING) != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (from == to) {
        return FDK_ERR_INVALID_ARGUMENT; /* self-relations are noise */
    }
    if (edge_list_contains(from, type, to)) {
        return FDK_OK; /* duplicate: a no-op, like duplicate sets */
    }
    /* Both sides must have room (the inverse counts against the
     * target's budget) — add both or neither. */
    if (from->a11y_relation_count >= FDK_A11Y_MAX_RELATIONS ||
        to->a11y_relation_count >= FDK_A11Y_MAX_RELATIONS) {
        return FDK_ERR_LIMIT;
    }
    fdk_result r = edge_list_add(from, type, to);
    if (!fdk_ok(r)) {
        return r;
    }
    r = edge_list_add(to, relation_inverse(type), from);
    if (!fdk_ok(r)) {
        /* Roll the first edge back — the pair is atomic. */
        edge_list_remove(from, type, to);
        return r;
    }
    fdk__a11y_notify(from, FDK_A11Y_RELATIONS_CHANGED, 0);
    fdk__a11y_notify(to, FDK_A11Y_RELATIONS_CHANGED, 0);
    return FDK_OK;
}

fdk_result fdk_a11y_remove_relation(fdk_widget *from,
                                    fdk_a11y_relation_type type,
                                    fdk_widget *to) {
    if (from == NULL || to == NULL || !relation_type_valid(type)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!edge_list_contains(from, type, to)) {
        return FDK_ERR_NOT_FOUND;
    }
    edge_list_remove(from, type, to);
    edge_list_remove(to, relation_inverse(type), from);
    if ((from->flags & FDK_WF_DESTROYING) == 0) {
        fdk__a11y_notify(from, FDK_A11Y_RELATIONS_CHANGED, 0);
    }
    if ((to->flags & FDK_WF_DESTROYING) == 0) {
        fdk__a11y_notify(to, FDK_A11Y_RELATIONS_CHANGED, 0);
    }
    return FDK_OK;
}

size_t fdk_a11y_relation_count(const fdk_widget *from,
                               fdk_a11y_relation_type type) {
    if (from == NULL || !relation_type_valid(type)) {
        return 0;
    }
    size_t n = 0;
    for (size_t i = 0; i < from->a11y_relation_count; i++) {
        if (from->a11y_relations[i].type == type) {
            n++;
        }
    }
    return n;
}

fdk_result fdk_a11y_relation_at(const fdk_widget *from,
                                fdk_a11y_relation_type type,
                                size_t index, fdk_widget **out_to) {
    if (from == NULL || out_to == NULL || !relation_type_valid(type)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    size_t seen = 0;
    for (size_t i = 0; i < from->a11y_relation_count; i++) {
        if (from->a11y_relations[i].type == type) {
            if (seen == index) {
                *out_to = from->a11y_relations[i].target;
                return FDK_OK;
            }
            seen++;
        }
    }
    return FDK_ERR_NOT_FOUND;
}

bool fdk_a11y_has_relation(const fdk_widget *from,
                           fdk_a11y_relation_type type,
                           const fdk_widget *to) {
    if (from == NULL || to == NULL) {
        return false;
    }
    return edge_list_contains(from, type, to);
}

void fdk__a11y_relations_destroyed(fdk_widget *widget) {
    if (widget == NULL) {
        return;
    }
    if (widget->a11y_relations != NULL) {
        /* Remove the inverse copies stored on our targets first
         * (while our own list is still readable), then free ours. */
        for (size_t i = 0; i < widget->a11y_relation_count; i++) {
            fdk_widget *target = widget->a11y_relations[i].target;
            if (target == NULL ||
                (target->flags & FDK_WF_DESTROYING) != 0) {
                continue;
            }
            edge_list_remove(target,
                             relation_inverse(widget->a11y_relations[i].type),
                             widget);
            fdk__a11y_notify(target, FDK_A11Y_RELATIONS_CHANGED, 0);
        }
    }
    fdk_free(widget->a11y_relations);
    widget->a11y_relations = NULL;
    widget->a11y_relation_count = 0;
    widget->a11y_relation_cap = 0;
}
