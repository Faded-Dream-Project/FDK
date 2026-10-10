#define FDK_LOG_TAG "widgets"

/*
 * menubutton.c — MenuButton widget (1.4.3)
 *
 * GtkMenuButton's role: a button-shaped widget that pops up an
 * ATTACHED menu model when clicked — the hamburger, the toolbar
 * overflow, the "options" split-button. The model stays the
 * application's (borrowed for the popup only): after the chain
 * closes it is still valid, still reusable, still the app's to
 * destroy; nothing is cloned, nothing is retained.
 *
 * The popup is the menu machinery: fdk__menu_popup_open_full at the
 * button's bottom-left, min-width the button's width, with a
 * closed-hook that only clears this button's open state. The
 * combo's guard pattern applies unchanged — destroying the button
 * while its chain is open is safe: the guard's widget pointer is
 * NULLed by the destroy hook (which runs before the free), and the
 * closed hook then does bookkeeping only. The app-owned model is
 * never touched by the closed hook (there is nothing to free —
 * the model was never ours).
 *
 * The arrow (default DOWN) is the combo's chevron language: a
 * small vector chevron after the label. While the chain is open
 * the button paints PRESSED (the "the menu you see came from
 * here" read) and reports the a11y EXPANDED state.
 */

#include "widgets_internal.h"
#include "menu_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define MBTN_PAD_X 10
#define MBTN_PAD_Y 6
#define MBTN_ARROW_ZONE 16   /* the chevron's reserved run          */
#define MBTN_ARROW_GAP 4     /* label-to-chevron gap                */

typedef struct fdk_menu_button {
    fdk_widget base;
    fdk_font *font;          /* borrowed */
    char *text;              /* owned, may be NULL                  */
    fdk_menu *model;         /* borrowed, may be NULL (inert)       */
    fdk_menu_button_arrow arrow;
    bool open;               /* our chain is up                     */
    bool pressed;            /* pointer down inside                 */
    bool hovering;
    fdk_hover_fade fade;     /* the 1.4.1 hover paint blend        */
    struct mbtn_guard *guard; /* the live chain's lifetime guard    */
} fdk_menu_button;

static fdk_menu_button *mbtn_of(fdk_widget *w) {
    return (fdk_menu_button *)(void *)w;
}

extern const fdk_widget_class fdk_menu_button_class_def;

/* ---- the popup guard (the combo's pattern, model-free) ----
 *
 * The guard outlives the button while a chain is open; every
 * callback treats a NULL widget pointer as "button gone, do
 * bookkeeping only". Freed by the closed hook (every end path). */
typedef struct mbtn_guard {
    fdk_widget *button;      /* NULL once the button is destroyed */
} mbtn_guard;

typedef struct mbtn_ctx {
    mbtn_guard *guard;
} mbtn_ctx;

static void mbtn_popup_closed(void *user) {
    mbtn_ctx *ctx = user;
    fdk_widget *w = ctx->guard->button;
    if (w != NULL && w->klass == &fdk_menu_button_class_def) {
        fdk_menu_button *b = mbtn_of(w);
        b->open = false;
        b->guard = NULL;
        /* A11y: the EXPANDED state left. */
        fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_EXPANDED);
        fdk_widget_invalidate(w);
    }
    fdk_free(ctx->guard);
    fdk_free(ctx);
}

static void mbtn_open_popup(fdk_widget *w) {
    fdk_menu_button *b = mbtn_of(w);
    if (b->open || b->model == NULL) {
        return;
    }
    mbtn_ctx *ctx = fdk_alloc(sizeof(*ctx));
    mbtn_guard *guard = fdk_alloc(sizeof(*guard));
    if (ctx == NULL || guard == NULL) {
        fdk_free(ctx);
        fdk_free(guard);
        return;
    }
    guard->button = w;
    ctx->guard = guard;
    fdk_result r = fdk__menu_popup_open_full(
        b->model, w, 0, w->bounds.height, w->bounds.width,
        mbtn_popup_closed, ctx);
    if (!fdk_ok(r)) {
        fdk_free(guard);
        fdk_free(ctx);
        return;
    }
    b->open = true;
    b->guard = guard;
    /* A11y: the EXPANDED state arrived. */
    fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_EXPANDED);
    fdk_widget_invalidate(w);
}

/* ---- widget hooks ---- */

static void mbtn_measure(fdk_widget *w, fdk_size *out) {
    fdk_menu_button *b = mbtn_of(w);
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(b->font, b->text, &tw, &th);
    out->width = tw + MBTN_PAD_X * 2;
    out->height = th + MBTN_PAD_Y * 2;
    if (b->arrow != FDK_MENU_BUTTON_ARROW_NONE) {
        out->width += MBTN_ARROW_ZONE + MBTN_ARROW_GAP;
    }
    if (out->width < 24) {
        out->width = 24;
    }
    if (out->height < 16) {
        out->height = 16;
    }
}

/* The chevron rung at the right edge (x = the zone's center). */
static void mbtn_paint_chevron(fdk_surface *surface, fdk_rect bounds,
                               fdk_color col,
                               fdk_menu_button_arrow arrow) {
    fdk_i32 cx = bounds.x + bounds.width - MBTN_ARROW_ZONE / 2;
    fdk_i32 cy = bounds.y + bounds.height / 2;
    switch (arrow) {
    case FDK_MENU_BUTTON_ARROW_UP:
        fdk_surface_draw_line(surface, cx - 4, cy + 1, cx, cy - 3, col);
        fdk_surface_draw_line(surface, cx, cy - 3, cx + 4, cy + 1, col);
        break;
    case FDK_MENU_BUTTON_ARROW_LEFT:
        fdk_surface_draw_line(surface, cx + 1, cy - 4, cx - 3, cy, col);
        fdk_surface_draw_line(surface, cx - 3, cy, cx + 1, cy + 4, col);
        break;
    case FDK_MENU_BUTTON_ARROW_RIGHT:
        fdk_surface_draw_line(surface, cx - 1, cy - 4, cx + 3, cy, col);
        fdk_surface_draw_line(surface, cx + 3, cy, cx - 1, cy + 4, col);
        break;
    case FDK_MENU_BUTTON_ARROW_DOWN:
    default:
        fdk_surface_draw_line(surface, cx - 4, cy - 2, cx, cy + 2, col);
        fdk_surface_draw_line(surface, cx, cy + 2, cx + 4, cy - 2, col);
        break;
    }
}

static void mbtn_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_menu_button *b = mbtn_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;
    /* The button's NORMAL ladder: control fill, hover blend, pressed
     * when the chain is open TOO (the persistent "menu is up" read —
     * open beats a momentary press visual). */
    const bool active = b->pressed || b->open;
    const fdk_f32 ht = b->fade.t;
    fdk_color fill;
    fdk_color label_col;
    if (disabled) {
        fill = fdk__pal_control_disabled();
        label_col = fdk__pal_text_disabled();
    } else if (active) {
        fill = fdk__pal_control_pressed();
        label_col = fdk__pal_text();
    } else {
        /* blend_colors is controls.c's local; the fade's endpoints
         * are the control family's, computed inline here. */
        fdk_color a = fdk__pal_control();
        fdk_color hov = fdk__pal_control_hover();
        fill = (fdk_color){a.r + (hov.r - a.r) * ht,
                           a.g + (hov.g - a.g) * ht,
                           a.b + (hov.b - a.b) * ht,
                           a.a + (hov.a - a.a) * ht};
        label_col = fdk__pal_text();
    }
    /* Themed corner shape (1.4.16): the same shape story as the
     * catalog Button — ROUNDED/CIRCLE/SQUARE from the theme. */
    fdk_i32 radius = fdk__button_shape_radius(bounds.width,
                                              bounds.height);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);

    /* Focus ring (the button's themed rule). */
    if ((w->flags & FDK_WF_FOCUSED) != 0 && !disabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2,
                         bounds.height - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 ring_r = radius > fw ? radius - fw : 0;
            fdk_surface_draw_rounded_rect(surface, ring, ring_r,
                                          fdk__pal_focus_ring());
        }
    }

    /* Label: centered in the text zone (everything left of the
     * chevron run when one is shown). */
    if (b->font != NULL && b->text != NULL) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(b->font, b->text, &tw, &th);
        fdk_i32 zone_w = bounds.width;
        if (b->arrow != FDK_MENU_BUTTON_ARROW_NONE) {
            zone_w -= MBTN_ARROW_ZONE + MBTN_ARROW_GAP;
        }
        fdk_i32 text_x = bounds.x + (zone_w - tw) / 2;
        if (text_x < bounds.x) {
            text_x = bounds.x;
        }
        fdk_i32 baseline =
            fdk__center_baseline(b->font, bounds.y, bounds.height);
        fdk__draw_text(surface, b->font, b->text, label_col,
                       text_x, baseline);
    }

    if (b->arrow != FDK_MENU_BUTTON_ARROW_NONE) {
        mbtn_paint_chevron(surface, bounds, label_col, b->arrow);
    }
}

static void mbtn_destroy(fdk_widget *w) {
    fdk_menu_button *b = mbtn_of(w);
    fdk_free(b->text);
    b->text = NULL;
    /* An open chain outlives us safely (the combo's ordering story):
     * this hook runs before the memory is freed, so NULLing the
     * guard here makes every later closed-hook touch skip widget
     * access. The popup itself dies with the next event or the
     * window; the MODEL is the app's and needs nothing from us. */
    if (b->guard != NULL) {
        b->guard->button = NULL;
        b->guard = NULL;
    }
}

static bool mbtn_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_menu_button *b = mbtn_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        b->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP: {
        bool was_pressed = b->pressed;
        b->pressed = false;
        fdk_widget_invalidate(w);
        if (was_pressed && ev->pointer.position.x >= 0.0f &&
            ev->pointer.position.y >= 0.0f &&
            ev->pointer.position.x < (fdk_f32)w->bounds.width &&
            ev->pointer.position.y < (fdk_f32)w->bounds.height) {
            mbtn_open_popup(w);
        }
        return true;
    }
    case FDK_WIDGET_KEY_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        if (ev->key.scancode == FDK_KEY_SPACE ||
            ev->key.scancode == FDK_KEY_ENTER ||
            ev->key.scancode == FDK_KEY_DOWN) {
            mbtn_open_popup(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        b->hovering = true;
        fdk__hover_fade_arm(w, &b->fade, true);
        fdk_widget_invalidate(w);
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        b->hovering = false;
        fdk__hover_fade_arm(w, &b->fade, false);
        fdk_widget_invalidate(w);
        return false;
    default:
        return false;
    }
}

/* ---- a11y ---- */

static void mbtn_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_menu_button *b = (const fdk_menu_button *)(const void *)w;
    if (b->text != NULL) {
        out->name = fdk__strdup(b->text);
    }
    out->states |= FDK_A11Y_HAS_POPUP;
    if (b->open) {
        out->states |= FDK_A11Y_EXPANDED;
    }
}

static bool mbtn_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                              double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    /* The click path; an already-open chain is a no-op (opening
     * twice is not a thing — dismissal is the menu's own input). */
    mbtn_open_popup(w);
    return true;
}

static fdk_a11y_action_set mbtn_a11y_actions(const fdk_widget *w) {
    (void)w;
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static const fdk_a11y_class mbtn_a11y = {
    .role = FDK_A11Y_ROLE_MENU_BUTTON,
    .describe = mbtn_a11y_describe,
    .actions = mbtn_a11y_actions,
    .perform = mbtn_a11y_perform,
};

const fdk_widget_class fdk_menu_button_class_def = {
    .size = sizeof(fdk_menu_button),
    .name = "menu-button",
    .handle_event = mbtn_handle_event,
    .paint = mbtn_paint,
    .measure = mbtn_measure,
    .arrange = NULL,
    .destroy = mbtn_destroy,
    .a11y = &mbtn_a11y,
};

/* ---- public API ---- */

fdk_result fdk_menu_button_create(fdk_widget *parent, fdk_font *font,
                                  const char *label,
                                  fdk_widget **out_button) {
    if (out_button == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_menu_button_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_menu_button *b = mbtn_of(w);
    b->font = font;
    b->text = fdk__strdup(label);
    if (label != NULL && b->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    b->model = NULL;
    b->arrow = FDK_MENU_BUTTON_ARROW_DOWN;
    b->open = false;
    b->guard = NULL;
    b->pressed = false;
    b->hovering = false;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_button = w;
    return FDK_OK;
}

fdk_result fdk_menu_button_set_menu(fdk_widget *button, fdk_menu *model) {
    if (button == NULL || button->klass != &fdk_menu_button_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_menu_button *b = mbtn_of(button);
    if (b->open) {
        /* The live chain's model is fixed at open time; swapping it
         * under a popup the user is reading would be a lie. */
        return FDK_ERR_UNSUPPORTED;
    }
    b->model = model;
    fdk_widget_invalidate(button);
    return FDK_OK;
}

fdk_menu *fdk_menu_button_get_menu(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_menu_button_class_def) {
        return NULL;
    }
    return mbtn_of(button)->model;
}

void fdk_menu_button_set_arrow(fdk_widget *button,
                               fdk_menu_button_arrow arrow) {
    if (button == NULL || button->klass != &fdk_menu_button_class_def) {
        return;
    }
    switch (arrow) {
    case FDK_MENU_BUTTON_ARROW_NONE:
    case FDK_MENU_BUTTON_ARROW_DOWN:
    case FDK_MENU_BUTTON_ARROW_UP:
    case FDK_MENU_BUTTON_ARROW_LEFT:
    case FDK_MENU_BUTTON_ARROW_RIGHT:
        break;
    default:
        return; /* unknown values ignored (documented) */
    }
    fdk_menu_button *b = mbtn_of(button);
    if (b->arrow == arrow) {
        return;
    }
    b->arrow = arrow;
    fdk_widget_invalidate(button);
    fdk_widget_child_layout_changed(button->parent);
}

bool fdk_menu_button_is_open(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_menu_button_class_def) {
        return false;
    }
    return mbtn_of(button)->open;
}
