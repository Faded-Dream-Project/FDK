/*
 * stackswitcher.c — the stack's pill row (1.4.2).
 *
 * GTK's GtkStackSwitcher: the horizontal row of rounded pills that
 * switches a Stack's pages — the modern face of the tab strip
 * (GNOME's Settings wears one). Active page = an accent-filled pill
 * with the accent's own text ink (a "checked" semantic: it snaps,
 * the 1.4.1 rule); inactive pills are flat text that blends the
 * soft ROW_HOVER fill in over the 120-ms hover fade — the exact
 * machinery the buttons ride.
 *
 * BINDING. set_stack binds loosely: the switcher keeps a persistent
 * reentrancy watch on the stack, and every paint/arrange/measure
 * re-checks it — a stack destroyed by another hand cleanly unbinds
 * (the switcher paints its empty row; a dangling pointer is a bug,
 * not a policy). The switcher never occupies the stack's single
 * on_changed slot: it reconciles at paint time instead (count,
 * titles, and the active pill are re-read every frame — pages are
 * few and titles short, and the alternative is an observer list the
 * API does not need yet).
 *
 * Clicks switch by NAME (the cached copy of the page key); a stale
 * cache (the page left between paints) returns NOT_FOUND up through
 * the stack's own API and the next paint re-syncs — honest, visible,
 * and self-healing.
 *
 * The a11y face is the notebook's split: the switcher is the TAB_LIST
 * with one VIRTUAL CHILD per pill (role TAB, the title as name,
 * SELECTED on the active page, ACTIVATE = switch).
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"

#include <string.h>

#define SWITCHER_PILL_H 30
#define SWITCHER_PILL_PAD_X 14
#define SWITCHER_PILL_GAP 4
#define SWITCHER_MIN_W 40

static fdk_stackswitcher *sw_of(fdk_widget *w) {
    return (fdk_stackswitcher *)(void *)w;
}

static void sw_drop_pills(fdk_stackswitcher *sw);

/* The live stack handle, or NULL once unbound/dead. */
static fdk_widget *sw_stack(fdk_stackswitcher *sw) {
    if (sw->stack == NULL) {
        return NULL;
    }
    if (!fdk__watch_alive(&sw->stack_watch)) {
        /* The stack died by another hand: unbind NOW (here is the
         * only place the dead watch is observed). */
        sw->stack = NULL;
        return NULL;
    }
    return sw->stack;
}

/* Const-safe peek for the a11y walk's describe/actions (read the
 * binding WITHOUT the unbind-on-dead side effect — the describe
 * path must not mutate). */
static const fdk_widget *sw_stack_peek(const fdk_stackswitcher *sw) {
    if (sw->stack == NULL || !fdk__watch_alive(&sw->stack_watch)) {
        return NULL;
    }
    return sw->stack;
}

/* Drops the pill cache (page set or titles changed, or unbinding):
 * cancels running flights BEFORE the arrays free — a live tick's
 * user pointer points into the fades array. */
static void sw_drop_pills(fdk_stackswitcher *sw) {
    for (size_t i = 0; i < sw->fade_count; i++) {
        if (sw->fades != NULL && sw->fades[i].anim != NULL) {
            fdk_animation_cancel(sw->fades[i].anim);
            sw->fades[i].anim = NULL;
        }
    }
    if (sw->titles != NULL) {
        for (size_t i = 0; i < sw->fade_count; i++) {
            fdk_free(sw->titles[i]);
        }
        fdk_free(sw->titles);
        sw->titles = NULL;
    }
    if (sw->names != NULL) {
        for (size_t i = 0; i < sw->fade_count; i++) {
            fdk_free(sw->names[i]);
        }
        fdk_free(sw->names);
        sw->names = NULL;
    }
    fdk_free(sw->fades);
    sw->fades = NULL;
    sw->fade_count = 0;
    sw->hover_pill = -1;
}

/* Drops the binding entirely (stack death, rebind, destroy). */
static void sw_unbind(fdk_stackswitcher *sw) {
    fdk__widget_unwatch(&sw->stack_watch);
    sw->stack = NULL;
    sw_drop_pills(sw);
}

/* ---- reconciliation ----
 *
 * Re-reads the bound stack: drops dead bindings, rebuilds the pill
 * cache when the page count or any title changed, clamps the hover.
 * Returns the live stack handle (NULL when unbound). Cheap and
 * idempotent — paint, arrange, measure, and the a11y walk all call
 * it first. */
static fdk_widget *sw_reconcile(fdk_widget *w) {
    fdk_stackswitcher *sw = sw_of(w);
    fdk_widget *stack = sw_stack(sw);
    if (stack == NULL) {
        if (sw->fade_count > 0) {
            sw_drop_pills(sw);
        }
        return NULL;
    }
    fdk_stack *st = (fdk_stack *)(void *)stack;
    /* Stale when the count changed or any title drifted (NULL
     * titles already read as the page's name — the add fallback). */
    bool stale = (sw->fade_count != st->count);
    if (!stale && st->count > 0) {
        for (size_t i = 0; i < st->count; i++) {
            const char *live = (st->pages[i].title != NULL)
                ? st->pages[i].title
                : st->pages[i].name;
            if (sw->titles == NULL || sw->titles[i] == NULL ||
                strcmp(sw->titles[i], live) != 0) {
                stale = true;
                break;
            }
        }
    }
    if (stale) {
        sw_drop_pills(sw);
        if (st->count > 0) {
            sw->fades = fdk_alloc_array(st->count, sizeof(*sw->fades));
            sw->titles = fdk_alloc_array(st->count, sizeof(char *));
            sw->names = fdk_alloc_array(st->count, sizeof(char *));
            if (sw->fades == NULL || sw->titles == NULL ||
                sw->names == NULL) {
                /* OOM: the empty row — honest, not a crash. */
                fdk_free(sw->fades);
                fdk_free(sw->titles);
                fdk_free(sw->names);
                sw->fades = NULL;
                sw->titles = NULL;
                sw->names = NULL;
                return stack;
            }
            sw->fade_count = st->count;
            for (size_t i = 0; i < st->count; i++) {
                const char *title = (st->pages[i].title != NULL)
                    ? st->pages[i].title
                    : st->pages[i].name;
                sw->titles[i] = fdk__strdup(title);
                sw->names[i] = fdk__strdup(st->pages[i].name);
                if (sw->titles[i] == NULL || sw->names[i] == NULL) {
                    sw->fade_count = i; /* truncate honestly */
                    break;
                }
            }
        }
        fdk_widget_invalidate(w);
    }
    return stack;
}

/* Pill i's width (text + padding; a fontless switcher sizes pills
 * by padding alone — still clickable, still honest). */
static fdk_i32 sw_pill_w(const fdk_stackswitcher *sw, size_t i) {
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(sw->font, sw->titles[i], &tw, &th);
    return tw + SWITCHER_PILL_PAD_X * 2;
}

/* Pill index at widget-local x, -1 between/outside. */
static int sw_pill_at(fdk_stackswitcher *sw, fdk_f32 x) {
    if (sw->titles == NULL) {
        return -1;
    }
    fdk_i32 cx = 0;
    for (size_t i = 0; i < sw->fade_count; i++) {
        fdk_i32 pw = sw_pill_w(sw, i);
        if (x >= (fdk_f32)cx && x < (fdk_f32)(cx + pw)) {
            return (int)i;
        }
        cx += pw + SWITCHER_PILL_GAP;
    }
    return -1;
}

/* ---- geometry / paint ---- */

static void sw_measure(fdk_widget *w, fdk_size *out) {
    fdk_stackswitcher *sw = sw_of(w);
    (void)sw_reconcile(w);
    fdk_i32 total = 0;
    for (size_t i = 0; i < sw->fade_count; i++) {
        total += sw_pill_w(sw, i) +
                 ((i + 1 < sw->fade_count) ? SWITCHER_PILL_GAP : 0);
    }
    out->width = (total < SWITCHER_MIN_W) ? SWITCHER_MIN_W : total;
    out->height = SWITCHER_PILL_H;
}

static void sw_paint(fdk_widget *w, fdk_surface *surface,
                     fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_stackswitcher *sw = sw_of(w);
    fdk_widget *stack = sw_reconcile(w);
    if (bounds.width <= 0 || bounds.height <= 0 ||
        sw->fade_count == 0 || sw->titles == NULL) {
        return;
    }
    size_t active = (size_t)-1;
    if (stack != NULL) {
        active = ((fdk_stack *)(void *)stack)->current;
    }
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;

    fdk_i32 x = bounds.x;
    fdk_i32 cy = bounds.y + (bounds.height - SWITCHER_PILL_H) / 2;
    for (size_t i = 0; i < sw->fade_count; i++) {
        fdk_i32 pw = sw_pill_w(sw, i);
        fdk_rect pill = {x, cy, pw, SWITCHER_PILL_H};
        if (i == active) {
            /* The checked semantic SNAPS (1.4.1 rule): accent fill,
             * accent ink, no fade on state. */
            fdk_color fill = disabled ? fdk__pal_control_disabled()
                                      : fdk__pal_accent();
            fdk_surface_fill_rounded_rect(surface, pill,
                                          SWITCHER_PILL_H / 2, fill);
        } else if (!disabled && sw->fades[i].t > 0.0f) {
            /* The hover whisper: ROW_HOVER blended by the fade. */
            fdk_color base = fdk__pal_row_hover();
            fdk_f32 t = sw->fades[i].t;
            fdk_color fill = {base.r, base.g, base.b, base.a * t};
            if (fill.a > 0.0f) {
                fdk_surface_fill_rounded_rect(surface, pill,
                                              SWITCHER_PILL_H / 2,
                                              fill);
            }
        }
        if (sw->font != NULL) {
            fdk_i32 baseline = fdk__center_baseline(
                sw->font, pill.y, pill.height);
            fdk_color ink = (i == active)
                ? (disabled ? fdk__pal_text_disabled()
                            : fdk__pal_accent_text())
                : (disabled ? fdk__pal_text_disabled()
                            : fdk__pal_text());
            fdk__draw_text(surface, sw->font, sw->titles[i], ink,
                           pill.x + SWITCHER_PILL_PAD_X, baseline);
        }
        x += pw + SWITCHER_PILL_GAP;
    }
}

/* ---- events ---- */

static bool sw_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_stackswitcher *sw = sw_of(w);
    fdk_widget *stack = sw_reconcile(w);
    if (stack == NULL || sw->titles == NULL) {
        return false;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        int pill = sw_pill_at(sw, ev->position.x);
        if (pill != sw->hover_pill) {
            if (sw->hover_pill >= 0 &&
                (size_t)sw->hover_pill < sw->fade_count) {
                fdk__hover_fade_arm(w, &sw->fades[sw->hover_pill],
                                    false);
            }
            if (pill >= 0 && (size_t)pill < sw->fade_count) {
                fdk__hover_fade_arm(w, &sw->fades[pill], true);
            }
            sw->hover_pill = pill;
        }
        return false; /* motion keeps bubbling */
    }
    case FDK_WIDGET_POINTER_LEAVE: {
        if (sw->hover_pill >= 0 &&
            (size_t)sw->hover_pill < sw->fade_count) {
            fdk__hover_fade_arm(w, &sw->fades[sw->hover_pill], false);
        }
        sw->hover_pill = -1;
        return false;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        int pill = sw_pill_at(sw, ev->pointer.position.x);
        if (pill < 0) {
            return false;
        }
        /* Switch by the cached name: a page that left between
         * paints is a NOT_FOUND the app can see; the next paint
         * re-syncs. */
        (void)fdk_stack_set_visible_name(stack, sw->names[pill]);
        return true;
    }
    default:
        return false;
    }
}

static void sw_destroy(fdk_widget *w) {
    /* sw_unbind drops the watch, the cache, and the fades — the
     * switcher owns nothing else. */
    sw_unbind(sw_of(w));
}

/* ---- a11y ---- */

static void sw_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    (void)w;
    (void)out;
    /* The tab list's own name comes from its virtual children; the
     * notebook pattern keeps the container's describe empty. */
}

static size_t sw_virtual_count(const fdk_widget *w) {
    const fdk_stackswitcher *sw =
        (const fdk_stackswitcher *)(const void *)w;
    return sw->fade_count;
}

static void sw_virtual_describe(const fdk_widget *w, size_t index,
                                fdk_a11y_info *out) {
    const fdk_stackswitcher *sw =
        (const fdk_stackswitcher *)(const void *)w;
    out->role = FDK_A11Y_ROLE_TAB;
    if (sw->titles != NULL && sw->titles[index] != NULL) {
        out->name = fdk__strdup(sw->titles[index]);
    }
    out->states = FDK_A11Y_VISIBLE | FDK_A11Y_SHOWING |
                  FDK_A11Y_ENABLED;
    const fdk_widget *stack = sw_stack_peek(sw);
    if (stack != NULL &&
        index == ((const fdk_stack *)(const void *)stack)->current) {
        out->states |= FDK_A11Y_SELECTED;
    }
    /* Bounds: the pill's rect in the switcher's root-absolute
     * space. */
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_i32 x = 0;
    for (size_t i = 0; i < index; i++) {
        x += sw_pill_w(sw, i) + SWITCHER_PILL_GAP;
    }
    out->bounds = (fdk_rect){abs.x + x, abs.y,
                             sw_pill_w(sw, index), SWITCHER_PILL_H};
}

static fdk_a11y_action_set sw_virtual_actions(const fdk_widget *w,
                                              size_t index) {
    const fdk_stackswitcher *sw =
        (const fdk_stackswitcher *)(const void *)w;
    const fdk_widget *stack = sw_stack_peek(sw);
    if (stack == NULL ||
        index == ((const fdk_stack *)(const void *)stack)->current) {
        return 0;
    }
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static bool sw_virtual_perform(fdk_widget *w, size_t index,
                               fdk_a11y_action action, double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_stackswitcher *sw = sw_of(w);
    fdk_widget *stack = sw_stack(sw);
    if (stack == NULL || sw->names == NULL ||
        index >= sw->fade_count) {
        return false;
    }
    return fdk_ok(fdk_stack_set_visible_name(stack, sw->names[index]));
}

static const fdk_a11y_class sw_a11y = {
    .role = FDK_A11Y_ROLE_TAB_LIST,
    .describe = sw_a11y_describe,
    .actions = NULL,
    .perform = NULL,
    .virtual_count = sw_virtual_count,
    .virtual_describe = sw_virtual_describe,
    .virtual_actions = sw_virtual_actions,
    .virtual_perform = sw_virtual_perform,
};

const fdk_widget_class fdk_stackswitcher_class_def = {
    .size = sizeof(fdk_stackswitcher),
    .name = "stackswitcher",
    .handle_event = sw_handle_event,
    .paint = sw_paint,
    .measure = sw_measure,
    .arrange = NULL,
    .destroy = sw_destroy,
    .a11y = &sw_a11y,
    .max_children = 0,
};

fdk_result fdk_stackswitcher_create(fdk_widget *parent, fdk_font *font,
                                    fdk_widget **out_switcher) {
    if (out_switcher == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(
        parent, &fdk_stackswitcher_class_def,
        (fdk_rect){0, 0, SWITCHER_MIN_W, SWITCHER_PILL_H}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_stackswitcher *sw = sw_of(w);
    sw->font = font;
    sw->stack = NULL;
    sw->stack_watch.target = NULL;
    sw->hover_pill = -1;
    sw->fades = NULL;
    sw->fade_count = 0;
    sw->titles = NULL;
    sw->names = NULL;
    *out_switcher = w;
    return FDK_OK;
}

void fdk_stackswitcher_set_stack(fdk_widget *switcher,
                                 fdk_widget *stack) {
    if (switcher == NULL ||
        switcher->klass != &fdk_stackswitcher_class_def) {
        return;
    }
    fdk_stackswitcher *sw = sw_of(switcher);
    /* Rebind: drop the old binding entirely first. */
    sw_unbind(sw);
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return; /* NULL unbinds; a non-stack is refused */
    }
    sw->stack = stack;
    fdk__widget_watch(&sw->stack_watch, stack);
    /* Reconcile immediately so the next measure/paint sees pills. */
    sw_reconcile(switcher);
    fdk_widget_invalidate(switcher);
    fdk_widget_child_layout_changed(switcher->parent);
}

fdk_widget *fdk_stackswitcher_get_stack(fdk_widget *switcher) {
    if (switcher == NULL ||
        switcher->klass != &fdk_stackswitcher_class_def) {
        return NULL;
    }
    fdk_stackswitcher *sw = sw_of(switcher);
    return sw_stack(sw); /* NULL once dead/unbound */
}
