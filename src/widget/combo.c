#define FDK_LOG_TAG "widgets"

/*
 * combo.c — ComboBox widget (Phase 9 completion)
 *
 * Non-editable: a field widget painting the active row's text plus a
 * chevron; the whole field opens the dropdown. Editable: an embedded
 * Entry (full text editing, selection, clipboard) over the field with
 * the chevron zone kept for the dropdown; typing goes "custom"
 * (active index cleared, on_changed fires with FDK_COMBO_NONE).
 *
 * The dropdown is the menu machinery: a temporary fdk_menu model
 * (rows as NORMAL items with per-item callbacks capturing this
 * combo) popped via fdk__menu_popup_open_full at the combo's
 * bottom-left, min-width the combo's width, with a closed-hook that
 * frees the temporary model when the chain ends (activation,
 * dismissal, or popup death — every path). No FDK window, popup, or
 * session knowledge leaks into the application: it pumps events, the
 * combo does the rest.
 */

#include "widgets_internal.h"
#include "menu_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define COMBO_CHEVRON 22    /* chevron zone width (right edge) */
#define COMBO_PAD_X 8       /* field text padding              */

typedef struct fdk_combo {
    fdk_widget base;
    fdk_font *font;         /* borrowed */
    char **rows;            /* owned row texts */
    size_t count;
    size_t cap;
    fdk_i64 active;         /* -1 = none */
    bool editable;
    fdk_widget *entry;      /* editable mode's embedded Entry    */
    bool entry_suppress;    /* programmatic buffer writes       */
    bool dropdown_open;
    fdk_menu *dropdown_model; /* temporary model while open     */
    fdk_combo_changed_fn on_changed;
    void *on_changed_user;
    bool hovering;
    bool pressed;
} fdk_combo;

static fdk_combo *combo_of(fdk_widget *w) {
    return (fdk_combo *)(void *)w;
}

extern const fdk_widget_class fdk_combo_class_def;

/* ---- helpers ---- */

static void combo_fire_changed(fdk_combo *c, fdk_i64 index) {
    if (c->on_changed != NULL) {
        c->on_changed(&c->base, (index >= 0) ? (size_t)index
                                             : FDK_COMBO_NONE,
                      c->on_changed_user);
    }
}

static const char *combo_row_text(const fdk_combo *c, fdk_i64 index) {
    if (c == NULL || index < 0 || (size_t)index >= c->count) {
        return NULL;
    }
    return c->rows[index];
}

/* The currently-displayed text (active row, else the entry buffer). */
static const char *combo_display_text(fdk_combo *c) {
    if (c->editable && c->entry != NULL) {
        return fdk_entry_get_text(c->entry);
    }
    const char *t = combo_row_text(c, c->active);
    return (t != NULL) ? t : "";
}

/* ---- dropdown (the menu machinery) ---- */

typedef struct combo_item_ctx {
    fdk_widget *combo;
    size_t index;
} combo_item_ctx;

typedef struct combo_dropdown_ctx {
    fdk_widget *combo;      /* class-checked at every use          */
    fdk_menu *model;        /* freed here                          */
    combo_item_ctx *items;  /* per-item closures array, freed here */
} combo_dropdown_ctx;

/* The closed hook: fired on EVERY end path (activation — after the
 * item callback returned — dismissal, popup death). Frees the
 * temporary model and the item closures. */
static void combo_dropdown_closed(void *user) {
    combo_dropdown_ctx *ctx = user;
    fdk_widget *w = ctx->combo;
    if (w != NULL && w->klass == &fdk_combo_class_def) {
        fdk_combo *c = combo_of(w);
        c->dropdown_open = false;
        c->dropdown_model = NULL;
        fdk_widget_invalidate(w);
    }
    fdk_menu_destroy(ctx->model);
    fdk_free(ctx->items);
    fdk_free(ctx);
}

static void combo_item_activated(fdk_menu_item *item, void *user) {
    (void)item;
    combo_item_ctx *ictx = user;
    fdk_widget *w = ictx->combo;
    if (w == NULL || w->klass != &fdk_combo_class_def) {
        return;
    }
    fdk_combo *c = combo_of(w);
    fdk_i64 index = (fdk_i64)ictx->index;
    if (c->active != index) {
        c->active = index;
        if (c->editable && c->entry != NULL) {
            c->entry_suppress = true;
            (void)fdk_entry_set_text(c->entry,
                                     combo_row_text(c, index));
            c->entry_suppress = false;
        }
        fdk_widget_invalidate(w);
        combo_fire_changed(c, index);
    }
}

static void combo_open_dropdown(fdk_widget *w) {
    fdk_combo *c = combo_of(w);
    if (c->dropdown_open || c->count == 0) {
        return;
    }
    /* The temporary model: rows as NORMAL items, each carrying a
     * per-item closure (combo + row ordinal) into the items array.
     * Model, items array, and ctx are all freed by the closed hook —
     * which fires on every end path, AFTER the item callback has
     * returned (the session closes post-activation; see
     * view_activate in menu.c). */
    fdk_menu *model = NULL;
    if (!fdk_ok(fdk_menu_create(c->font, &model)) || model == NULL) {
        return;
    }
    combo_dropdown_ctx *ctx = fdk_alloc(sizeof(combo_dropdown_ctx));
    combo_item_ctx *items = fdk_alloc_array(c->count, sizeof(*items));
    if (ctx == NULL || items == NULL) {
        fdk_free(ctx);
        fdk_free(items);
        fdk_menu_destroy(model);
        return;
    }
    ctx->combo = w;
    ctx->model = model;
    ctx->items = items;

    bool ok = true;
    for (size_t i = 0; i < c->count; i++) {
        items[i].combo = w;
        items[i].index = i;
        fdk_menu_item *it = NULL;
        if (!fdk_ok(fdk_menu_append(model, c->rows[i], &it)) ||
            it == NULL) {
            ok = false;
            break;
        }
        fdk_menu_item_set_on_activate(it, combo_item_activated, &items[i]);
    }
    if (ok) {
        fdk_i32 width = w->bounds.width;
        fdk_result r = fdk__menu_popup_open_full(
            model, w, 0, w->bounds.height, width, combo_dropdown_closed,
            ctx);
        if (fdk_ok(r)) {
            c->dropdown_open = true;
            c->dropdown_model = model;
            fdk_widget_invalidate(w);
            return;
        }
    }
    fdk_free(items);
    fdk_free(ctx);
    fdk_menu_destroy(model);
}

/* ---- widget hooks ---- */

static void combo_measure(fdk_widget *w, fdk_size *out) {
    fdk_combo *c = combo_of(w);
    fdk_i32 max_w = 40;
    for (size_t i = 0; i < c->count; i++) {
        fdk_i32 tw = 0;
        fdk__text_extent(c->font, c->rows[i], &tw, NULL);
        if (tw > max_w) {
            max_w = tw;
        }
    }
    out->width = max_w + COMBO_PAD_X * 2 + COMBO_CHEVRON;
    out->height = fdk__menu_row_height(NULL);
}

static void combo_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_combo *c = combo_of(w);
    if (c->entry != NULL) {
        fdk_rect r = assigned;
        r.x = 0;
        r.y = 0;
        r.width -= COMBO_CHEVRON;
        if (r.width < 1) {
            r.width = 1;
        }
        fdk_widget_set_bounds(c->entry, r);
    }
}

static void combo_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_combo *c = combo_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The field (the Entry paints itself above it in editable mode —
     * but the field chrome still frames it). */
    fdk_color fill = fdk__pal_control();
    if (c->hovering && fdk_widget_is_effectively_enabled(w)) {
        fill = fdk__pal_control_hover();
    }
    if (c->pressed && fdk_widget_is_effectively_enabled(w)) {
        fill = fdk__pal_control_pressed();
    }
    fdk_surface_fill_rect(surface, bounds, fill);
    fdk_surface_draw_rect(surface, bounds, fdk__pal_border());

    /* Chevron zone: a subtle inset panel with a vector chevron. */
    fdk_rect cz = {bounds.x + bounds.width - COMBO_CHEVRON, bounds.y,
                   COMBO_CHEVRON, bounds.height};
    fdk_color col = fdk_widget_is_effectively_enabled(w)
                        ? fdk__pal_text()
                        : fdk__pal_text_disabled();
    fdk_i32 cx = cz.x + cz.width / 2;
    fdk_i32 cy = cz.y + cz.height / 2;
    fdk_surface_draw_line(surface, cx - 4, cy - 2, cx, cy + 2, col);
    fdk_surface_draw_line(surface, cx, cy + 2, cx + 4, cy - 2, col);

    /* Non-editable: the active row's text. */
    if (!c->editable) {
        const char *t = combo_display_text(c);
        if (c->font != NULL && t != NULL && t[0] != '\0') {
            fdk_i32 baseline =
                fdk__center_baseline(c->font, bounds.y, bounds.height);
            fdk__draw_text(surface, c->font, t,
                           fdk_widget_is_effectively_enabled(w)
                               ? fdk__pal_text()
                               : fdk__pal_text_disabled(),
                           bounds.x + COMBO_PAD_X, baseline);
        }
    }
}

static void combo_destroy(fdk_widget *w) {
    fdk_combo *c = combo_of(w);
    for (size_t i = 0; i < c->count; i++) {
        fdk_free(c->rows[i]);
    }
    fdk_free(c->rows);
    c->rows = NULL;
    c->count = 0;
    /* An open dropdown outlives us harmlessly: its temporary model
     * is freed by the closed hook (fired when the popups die with
     * the parent window or the chain ends); the combo pointer it
     * captures is guarded by the class check in the callbacks. */
}

static void combo_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_combo *c = (const fdk_combo *)(const void *)w;
    /* Name: the active row's text (or the custom/entry text). */
    const char *name = combo_row_text(c, c->active);
    if (name != NULL) {
        out->name = fdk__strdup(name);
    } else if (c->editable && c->entry != NULL) {
        const char *t = fdk_entry_get_text(c->entry);
        if (t != NULL && t[0] != '\0') {
            out->name = fdk__strdup(t);
        }
    }
    if (c->dropdown_open) {
        out->states |= FDK_A11Y_HAS_POPUP | FDK_A11Y_EXPANDED;
    } else {
        out->states |= FDK_A11Y_HAS_POPUP;
    }
    out->has_value = true;
    out->value_min = -1.0;
    out->value_max = (double)((c->count > 0) ? (fdk_i64)c->count - 1 : -1);
    out->value_current = (double)c->active;
}

static fdk_a11y_action_set combo_a11y_actions(const fdk_widget *w) {
    (void)w;
    return FDK_A11Y_ACTION_ACTIVATE | FDK_A11Y_ACTION_SET_VALUE;
}

static bool combo_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                               double value) {
    switch (action) {
    case FDK_A11Y_ACTION_ACTIVATE:
        /* Open/close the dropdown — the same path the field click
         * takes. */
        combo_open_dropdown(w);
        return true;
    case FDK_A11Y_ACTION_SET_VALUE:
        return fdk_ok(fdk_combo_set_active(w, (fdk_i64)value));
    default:
        return false;
    }
}

static const fdk_a11y_class combo_a11y = {
    .role = FDK_A11Y_ROLE_COMBO_BOX,
    .describe = combo_a11y_describe,
    .actions = combo_a11y_actions,
    .perform = combo_a11y_perform,
};

const fdk_widget_class fdk_combo_class_def = {
    .size = sizeof(fdk_combo),
    .name = "combo",
    .handle_event = NULL, /* the per-instance callback does it */
    .paint = combo_paint,
    .measure = combo_measure,
    .arrange = combo_arrange,
    .destroy = combo_destroy,
    .a11y = &combo_a11y,
};

/* Per-instance event callback (set at create): the class hook is
 * reserved; this runs after it. */
static bool combo_event(fdk_widget *w, const fdk_widget_event *ev,
                        void *user) {
    (void)user;
    fdk_combo *c = combo_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION:
        if (!c->hovering) {
            c->hovering = true;
            fdk_widget_invalidate(w);
        }
        return true;
    case FDK_WIDGET_POINTER_LEAVE:
        if (c->hovering) {
            c->hovering = false;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        c->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (c->pressed && !c->dropdown_open) {
            combo_open_dropdown(w);
        }
        c->pressed = false;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_KEY_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        /* Non-editable combos (which hold the focus) open on
         * Enter/Space/Down/Up. Editable ones route keys to the Entry
         * (which holds the focus instead) — the chevron is the only
         * opener there, documented. */
        if (!c->editable &&
            (ev->key.scancode == FDK_KEY_ENTER ||
             ev->key.scancode == FDK_KEY_SPACE ||
             ev->key.scancode == FDK_KEY_DOWN ||
             ev->key.scancode == FDK_KEY_UP)) {
            if (!c->dropdown_open) {
                combo_open_dropdown(w);
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* The editable Entry's on_changed: typing goes custom. */
static void combo_entry_changed(fdk_widget *entry, void *user) {
    (void)entry;
    fdk_widget *w = user;
    fdk_combo *c = combo_of(w);
    if (c->entry_suppress) {
        return;
    }
    if (c->active >= 0) {
        /* Did the user edit away from the active row's text? */
        const char *row = combo_row_text(c, c->active);
        const char *buf = (c->entry != NULL) ? fdk_entry_get_text(c->entry)
                                             : "";
        if (row == NULL || strcmp(row, buf) != 0) {
            c->active = -1;
            fdk_widget_invalidate(w);
            combo_fire_changed(c, -1);
        }
    } else {
        /* Already custom: nothing to re-fire (the Entry has its own
         * consumers; the combo reports index transitions only). */
    }
}

/* ---- public API ---- */

fdk_result fdk_combo_create(fdk_widget *parent, fdk_font *font,
                            fdk_widget **out_combo) {
    if (out_combo == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_combo_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_combo *c = combo_of(w);
    c->font = font;
    c->rows = NULL;
    c->count = 0;
    c->cap = 0;
    c->active = -1;
    c->editable = false;
    c->entry = NULL;
    c->entry_suppress = false;
    c->dropdown_open = false;
    c->dropdown_model = NULL;
    c->on_changed = NULL;
    c->on_changed_user = NULL;
    c->hovering = false;
    c->pressed = false;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_set_event_callback(w, combo_event, NULL);
    fdk_widget_child_layout_changed(w->parent);
    *out_combo = w;
    return FDK_OK;
}

static fdk_result combo_row_insert(fdk_combo *c, size_t index,
                                   const char *text) {
    if (c->count == c->cap) {
        size_t ncap = c->cap * 2 + 4;
        char **nr = fdk_realloc(c->rows, ncap * sizeof(char *));
        if (nr == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        c->rows = nr;
        c->cap = ncap;
    }
    char *copy = fdk__strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memmove(&c->rows[index + 1], &c->rows[index],
            (c->count - index) * sizeof(char *));
    c->rows[index] = copy;
    c->count++;
    return FDK_OK;
}

fdk_result fdk_combo_append(fdk_widget *combo, const char *text,
                            size_t *out_index) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_combo *c = combo_of(combo);
    fdk_result r = combo_row_insert(c, c->count, text);
    if (!fdk_ok(r)) {
        return r;
    }
    if (out_index != NULL) {
        *out_index = c->count - 1;
    }
    fdk_widget_invalidate(combo);
    return FDK_OK;
}

fdk_result fdk_combo_remove(fdk_widget *combo, size_t index) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_combo *c = combo_of(combo);
    if (index >= c->count) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_free(c->rows[index]);
    memmove(&c->rows[index], &c->rows[index + 1],
            (c->count - index - 1) * sizeof(char *));
    c->count--;
    /* The active row's death clears the selection (documented: no
     * on_changed for data-model changes). */
    if ((fdk_i64)index == c->active) {
        c->active = -1;
        if (c->editable && c->entry != NULL) {
            c->entry_suppress = true;
            (void)fdk_entry_set_text(c->entry, "");
            c->entry_suppress = false;
        }
    } else if ((fdk_i64)index < c->active) {
        c->active--;
    }
    fdk_widget_invalidate(combo);
    return FDK_OK;
}

void fdk_combo_clear(fdk_widget *combo) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return;
    }
    fdk_combo *c = combo_of(combo);
    for (size_t i = 0; i < c->count; i++) {
        fdk_free(c->rows[i]);
    }
    c->count = 0;
    c->active = -1;
    if (c->editable && c->entry != NULL) {
        c->entry_suppress = true;
        (void)fdk_entry_set_text(c->entry, "");
        c->entry_suppress = false;
    }
    fdk_widget_invalidate(combo);
}

size_t fdk_combo_count(fdk_widget *combo) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return 0;
    }
    return combo_of(combo)->count;
}

const char *fdk_combo_text(fdk_widget *combo, size_t index) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return NULL;
    }
    return combo_row_text(combo_of(combo), (fdk_i64)index);
}

fdk_i64 fdk_combo_get_active(fdk_widget *combo) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return -1;
    }
    return combo_of(combo)->active;
}

fdk_result fdk_combo_set_active(fdk_widget *combo, fdk_i64 index) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_combo *c = combo_of(combo);
    if (index >= (fdk_i64)c->count || index < -1) {
        return FDK_ERR_INVALID_ARGUMENT; /* -1 is the only "none" */
    }
    if (index < 0) {
        if (c->active != -1) {
            c->active = -1;
            fdk_widget_invalidate(combo);
            combo_fire_changed(c, -1);
        }
        return FDK_OK;
    }
    if (c->active == index) {
        return FDK_OK; /* no-op: no on_changed */
    }
    c->active = index;
    if (c->editable && c->entry != NULL) {
        c->entry_suppress = true;
        (void)fdk_entry_set_text(c->entry, combo_row_text(c, index));
        c->entry_suppress = false;
    }
    fdk_widget_invalidate(combo);
    combo_fire_changed(c, index);
    return FDK_OK;
}

const char *fdk_combo_active_text(fdk_widget *combo) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return "";
    }
    fdk_combo *c = combo_of(combo);
    const char *t = combo_display_text(c);
    return (t != NULL) ? t : "";
}

void fdk_combo_set_editable(fdk_widget *combo, bool editable) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return;
    }
    fdk_combo *c = combo_of(combo);
    if (c->editable == editable) {
        return;
    }
    c->editable = editable;
    if (editable) {
        if (c->entry == NULL) {
            fdk_widget *e = NULL;
            if (fdk_ok(fdk_entry_create(combo, c->font, "", &e))) {
                c->entry = e;
                fdk_entry_set_on_changed(e, combo_entry_changed, combo);
            }
        }
        if (c->entry != NULL) {
            const char *row = combo_row_text(c, c->active);
            c->entry_suppress = true;
            (void)fdk_entry_set_text(c->entry, row != NULL ? row : "");
            c->entry_suppress = false;
            fdk_widget_set_can_focus(combo, false);
            combo_arrange(combo, fdk_widget_get_bounds(combo));
        }
    } else {
        if (c->entry != NULL) {
            fdk_widget_destroy(c->entry);
            c->entry = NULL;
        }
        fdk_widget_set_can_focus(combo, true);
    }
    fdk_widget_invalidate(combo);
}

void fdk_combo_set_on_changed(fdk_widget *combo,
                              fdk_combo_changed_fn on_changed,
                              void *user_data) {
    if (combo == NULL || combo->klass != &fdk_combo_class_def) {
        return;
    }
    fdk_combo *c = combo_of(combo);
    c->on_changed = on_changed;
    c->on_changed_user = user_data;
}
