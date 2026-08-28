/* a11y.c — the accessibility core (Phase 10).
 *
 * The tree IS the a11y tree: describe() snapshots a widget's role,
 * name, states, value, and bounds; subscribers observe changes;
 * perform() drives widgets through their own public semantics.
 *
 * Layer position: strictly widget-layer, backend-neutral, verified
 * headless. The platform bridge (a future AT-SPI2 bridge, a screen
 * reader, a test driver) is a CONSUMER of this API — see the header
 * for the design rationale.
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
