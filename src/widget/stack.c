/*
 * stack.c — the named page stack (1.4.2).
 *
 * GTK's GtkStack: a container of NAMED pages with exactly one
 * visible at a time — the structure every settings dialog and
 * master-detail pane is built on. The Notebook's adoption model
 * applies verbatim: fdk_stack_add reparents the page in (the
 * standard FDK parent-owns-children model), the stack sets every
 * non-current page FDK_WF_VISIBLE-off (input-transparent, skipped
 * by paints — the visible-flag semantics doing exactly what a page
 * switcher needs), and the current page gets the stack's full
 * bounds.
 *
 * Unlike the Notebook there is no drawn strip: the Stack is pure
 * structure, and the switching surface belongs to whoever the app
 * chooses — the StackSwitcher's pill row, a sidebar, or nothing
 * (programmatic page flips). Names are unique keys (the GTK rule:
 * a duplicate or NULL name is refused, not silently de-duplicated);
 * titles are display strings the StackSwitcher reads.
 *
 * Page switches are INSTANT by v1 policy: no crossfade (the paint
 * walk composites to a flat surface — the revealer's honesty note
 * covers why), no slide. What a switching animation needs is a
 * canvas-level compositor decision, parked with the HiDPI/fractional
 * family in the roadmap.
 *
 * The natural size is the MAX over pages' naturals (floored at
 * 40x40): a non-homogeneous stack sized for its largest page, so
 * page flips never resize the window. Pages are measured live —
 * stacks hold a handful of pages, and the measure must see a
 * page's CURRENT natural anyway.
 *
 * The a11y face: PANEL role, the current page's title as the name
 * (the StackSwitcher exposes the tab-list interface — the two
 * widgets split the a11y job the way GTK's do).
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"

#include <string.h>

#define STACK_MIN_W 40
#define STACK_MIN_H 40

static fdk_stack *stack_of_w(fdk_widget *w) {
    return (fdk_stack *)(void *)w;
}

/* Exactly one page visible; the current one owns the bounds. */
static void stack_sync_pages(fdk_stack *st) {
    fdk_rect area = {0, 0, st->base.bounds.width,
                     st->base.bounds.height};
    for (size_t i = 0; i < st->count; i++) {
        fdk_widget_set_visible(st->pages[i].widget, i == st->current);
        if (i == st->current) {
            /* ARRANGE, not set_bounds + the layout notifier: the
             * page's own container hook re-packs it at the new
             * bounds directly. The notifier would CLIMB from the
             * page back into the stack — whose hook re-syncs, whose
             * sync re-notifies, unbounded recursion. (The notebook's
             * identical sync survives only because no notifier entry
             * dispatches ON the notebook; the stack has one. Found
             * live by example 12, whose pages are BOXES — the
             * headless suite's plain pages never dispatched.) */
            fdk_widget_arrange(st->pages[i].widget, area);
        }
    }
}

static void stack_measure(fdk_widget *w, fdk_size *out) {
    fdk_stack *st = stack_of_w(w);
    fdk_i32 mw = STACK_MIN_W;
    fdk_i32 mh = STACK_MIN_H;
    for (size_t i = 0; i < st->count; i++) {
        fdk_widget *page = st->pages[i].widget;
        if ((page->flags & FDK_WF_DESTROYING) != 0) {
            continue;
        }
        /* Visibility-independent (the revealer/expander rule): the
         * stack owns page visibility; a hidden page still counts
         * toward the natural size or the first flip would resize
         * the window. */
        fdk_size pn;
        fdk_widget_measure(page, &pn);
        if (pn.width > mw) {
            mw = pn.width;
        }
        if (pn.height > mh) {
            mh = pn.height;
        }
    }
    out->width = mw;
    out->height = mh;
}

static void stack_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    stack_sync_pages(stack_of_w(w));
}

/* The layout notifier's hook (box.c): a page (or something inside
 * one) joined or re-measured — re-sync at the CURRENT bounds so the
 * current page re-arranges and the stack's natural stays honest. */
void fdk__stack_layout_changed(fdk_widget *w) {
    if (w == NULL || w->klass != &fdk_stack_class_def) {
        return;
    }
    if ((w->flags & FDK_WF_DESTROYING) != 0) {
        return;
    }
    stack_arrange(w, w->bounds);
}

static void stack_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)surface;
    (void)bounds;
    (void)clip;
    (void)w;
    /* Nothing of its own: pages paint themselves; the app composes
     * surfaces it wants behind them. (The notebook paints a strip;
     * the stack deliberately does not.) */
}

static void stack_destroy(fdk_widget *w) {
    fdk_stack *st = stack_of_w(w);
    for (size_t i = 0; i < st->count; i++) {
        fdk_free(st->pages[i].name);
        fdk_free(st->pages[i].title);
    }
    fdk_free(st->pages);
}

/* ---- a11y ---- */

static void stack_a11y_describe(const fdk_widget *w,
                                fdk_a11y_info *out) {
    const fdk_stack *st = (const fdk_stack *)(const void *)w;
    if (st->count > 0 && st->current < st->count) {
        const fdk_stack_page *p = &st->pages[st->current];
        out->name = fdk__strdup(p->title != NULL ? p->title : p->name);
    }
}

static const fdk_a11y_class stack_a11y = {
    .role = FDK_A11Y_ROLE_PANEL,
    .describe = stack_a11y_describe,
    .actions = NULL,
    .perform = NULL,
};

const fdk_widget_class fdk_stack_class_def = {
    .size = sizeof(fdk_stack),
    .name = "stack",
    .handle_event = NULL,
    .paint = stack_paint,
    .measure = stack_measure,
    .arrange = stack_arrange,
    .destroy = stack_destroy,
    .a11y = &stack_a11y,
};

fdk_result fdk_stack_create(fdk_widget *parent,
                            fdk_widget **out_stack) {
    if (out_stack == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_stack_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_stack *st = stack_of_w(w);
    st->pages = NULL;
    st->count = 0;
    st->capacity = 0;
    st->current = 0;
    fdk_widget_child_layout_changed(w->parent);
    *out_stack = w;
    return FDK_OK;
}

fdk_result fdk_stack_add(fdk_widget *stack, fdk_widget *child,
                         const char *name, const char *title) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def ||
        child == NULL || name == NULL || name[0] == '\0') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_stack *st = stack_of_w(stack);
    /* Names are unique keys (the GTK rule). */
    for (size_t i = 0; i < st->count; i++) {
        if (strcmp(st->pages[i].name, name) == 0) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
    }
    if (st->count == st->capacity) {
        size_t cap = (st->capacity == 0) ? 4 : st->capacity * 2;
        if (cap < st->capacity || cap > SIZE_MAX / sizeof(*st->pages)) {
            return FDK_ERR_OUT_OF_MEMORY; /* refuse absurd growth */
        }
        fdk_stack_page *grown =
            fdk_realloc(st->pages, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        st->pages = grown;
        st->capacity = cap;
    }
    char *name_copy = fdk__strdup(name);
    char *title_copy = fdk__strdup(title != NULL ? title : name);
    if (name_copy == NULL || title_copy == NULL) {
        fdk_free(name_copy);
        fdk_free(title_copy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_result r = fdk_widget_reparent(child, stack);
    if (!fdk_ok(r)) {
        fdk_free(name_copy);
        fdk_free(title_copy);
        return r;
    }
    st->pages[st->count].widget = child;
    st->pages[st->count].name = name_copy;
    st->pages[st->count].title = title_copy;
    st->count++;
    if (st->count == 1) {
        st->current = 0;
    }
    stack_sync_pages(st);
    fdk_widget_invalidate(stack);
    fdk__a11y_notify(stack, FDK_A11Y_CHILDREN_CHANGED, 0);
    return FDK_OK;
}

size_t fdk_stack_page_count(fdk_widget *stack) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return 0;
    }
    return stack_of_w(stack)->count;
}

fdk_widget *fdk_stack_get_page(fdk_widget *stack, size_t index) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return NULL;
    }
    return st->pages[index].widget;
}

fdk_widget *fdk_stack_get_child_by_name(fdk_widget *stack,
                                        const char *name) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def ||
        name == NULL) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    for (size_t i = 0; i < st->count; i++) {
        if (strcmp(st->pages[i].name, name) == 0) {
            return st->pages[i].widget;
        }
    }
    return NULL;
}

const char *fdk_stack_page_name(fdk_widget *stack, size_t index) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return NULL;
    }
    return st->pages[index].name;
}

const char *fdk_stack_page_title(fdk_widget *stack, size_t index) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return NULL;
    }
    return st->pages[index].title;
}

fdk_result fdk_stack_set_page_title(fdk_widget *stack, size_t index,
                                    const char *title) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* NULL/empty restores the name (the documented fallback the add
     * path installs). */
    const char *fallback = st->pages[index].name;
    char *copy = fdk__strdup(
        (title != NULL && title[0] != '\0') ? title : fallback);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(st->pages[index].title);
    st->pages[index].title = copy;
    fdk__a11y_notify(stack, FDK_A11Y_NAME_CHANGED, 0);
    /* A bound StackSwitcher re-reads titles at its next paint —
     * nothing to chase here. */
    return FDK_OK;
}

/* The switch path shared by every entry point. */
static fdk_result stack_set_index(fdk_widget *stack, size_t index,
                                  bool by_name) {
    (void)by_name;
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (index != st->current) {
        st->current = index;
        stack_sync_pages(st);
        fdk_widget_invalidate(stack);
        fdk__a11y_notify(stack, FDK_A11Y_NAME_CHANGED, 0);
        if (st->on_changed != NULL) {
            fdk_widget_watch watch;
            fdk__widget_watch(&watch, stack);
            st->on_changed(stack, index, st->pages[index].name,
                           st->on_changed_data);
            /* The callback may have destroyed the stack. */
            (void)fdk__watch_alive(&watch);
            fdk__widget_unwatch(&watch);
        }
    }
    return FDK_OK;
}

fdk_result fdk_stack_set_visible_name(fdk_widget *stack,
                                      const char *name) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def ||
        name == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_stack *st = stack_of_w(stack);
    for (size_t i = 0; i < st->count; i++) {
        if (strcmp(st->pages[i].name, name) == 0) {
            return stack_set_index(stack, i, true);
        }
    }
    return FDK_ERR_NOT_FOUND;
}

fdk_result fdk_stack_set_visible_child(fdk_widget *stack,
                                       fdk_widget *child) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def ||
        child == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_stack *st = stack_of_w(stack);
    for (size_t i = 0; i < st->count; i++) {
        if (st->pages[i].widget == child) {
            return stack_set_index(stack, i, false);
        }
    }
    return FDK_ERR_NOT_FOUND;
}

const char *fdk_stack_get_visible_name(fdk_widget *stack) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    return (st->count > 0) ? st->pages[st->current].name : NULL;
}

fdk_widget *fdk_stack_get_visible_child(fdk_widget *stack) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return NULL;
    }
    fdk_stack *st = stack_of_w(stack);
    return (st->count > 0) ? st->pages[st->current].widget : NULL;
}

fdk_result fdk_stack_remove_page(fdk_widget *stack, size_t index) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_stack *st = stack_of_w(stack);
    if (index >= st->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *page = st->pages[index].widget;
    bool was_current = (index == st->current);
    fdk_free(st->pages[index].name);
    fdk_free(st->pages[index].title);
    memmove(&st->pages[index], &st->pages[index + 1],
            (st->count - index - 1) * sizeof(*st->pages));
    st->count--;
    /* The notebook's survivor rule: removing the current page shows
     * the page that shifted into its slot (clamped); removing an
     * earlier page keeps showing the same page (index shifted). */
    if (st->count == 0) {
        st->current = 0;
    } else if (was_current) {
        st->current = (index < st->count) ? index : st->count - 1;
    } else if (index < st->current) {
        st->current--;
    }
    /* The stack owns its pages (add reparented them in — the
     * standard parent-owns-children model): removing a page
     * destroys the widget, exactly like the notebook's. */
    fdk_widget_destroy(page);
    stack_sync_pages(st);
    fdk_widget_invalidate(stack);
    fdk__a11y_notify(stack, FDK_A11Y_CHILDREN_CHANGED, 0);
    if (was_current && st->count > 0 && st->on_changed != NULL) {
        fdk_widget_watch watch;
        fdk__widget_watch(&watch, stack);
        st->on_changed(stack, st->current,
                       st->pages[st->current].name, st->on_changed_data);
        fdk__widget_unwatch(&watch);
    }
    return FDK_OK;
}

void fdk_stack_set_on_changed(fdk_widget *stack,
                              fdk_stack_changed_fn on_changed,
                              void *user_data) {
    if (stack == NULL || stack->klass != &fdk_stack_class_def) {
        return;
    }
    fdk_stack *st = stack_of_w(stack);
    st->on_changed = on_changed;
    st->on_changed_data = user_data;
}
