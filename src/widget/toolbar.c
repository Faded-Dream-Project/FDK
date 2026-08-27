#define FDK_LOG_TAG "widgets"

/*
 * toolbar.c — Toolbar widget (Phase 9)
 *
 * A horizontal action bar: flat-styled buttons (hover/press tint, no
 * rounded chrome — the "toolbar button" look) alternating with
 * separators, laid out in a row at fixed control height. Buttons are
 * real catalog Buttons reparented in and restyled only by the
 * toolbar's OWN paint (which draws the bar background under them);
 * their interaction (click, hover, keyboard activation) is the
 * stock Button behavior — a toolbar is presentation + arrangement,
 * not a new control.
 *
 * Overflow (wrapping / chevron menus) is parked honestly: a toolbar
 * narrower than its content clips at the bar's bounds (the standard
 * Phase 4 clip rule).
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define TB_PAD 4
#define TB_BTN_H 30
#define TB_GAP 2
#define TB_MIN_W 24
#define TB_MIN_H TB_BTN_H + TB_PAD * 2

typedef struct fdk_toolbar {
    fdk_widget base;
    fdk_font *font;    /* borrowed, shared with the buttons */
} fdk_toolbar;

static fdk_toolbar *toolbar_of(fdk_widget *w) {
    return (fdk_toolbar *)(void *)w;
}

extern const fdk_widget_class fdk_toolbar_class_def;

static void toolbar_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = TB_MIN_W;
    out->height = TB_MIN_H;
}

/* Row layout at the CURRENT bounds — the arrange hook, the layout
 * notifier (button added), and bounds-sync all funnel here. */
static void toolbar_layout(fdk_widget *w) {
    fdk_i32 x = TB_PAD;
    fdk_i32 y = TB_PAD;
    fdk_i32 h = w->bounds.height - TB_PAD * 2;
    if (h > TB_BTN_H) {
        h = TB_BTN_H;
    }
    if (h < 1) {
        return; /* not laid out yet */
    }
    size_t n = fdk_widget_child_count(w);
    for (size_t i = 0; i < n; i++) {
        fdk_widget *c = fdk_widget_child_at(w, i);
        fdk_size nat = { 0, 0 };
        fdk_widget_measure(c, &nat);
        fdk_i32 cw = nat.width;
        if (cw < 1) {
            cw = 1;
        }
        fdk_rect r = { x, y, cw, h };
        fdk_widget_set_bounds(c, r);
        x += cw + TB_GAP;
    }
}

static void toolbar_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    toolbar_layout(w);
}

/* Internal: relayout hook for the layout notifier (box.c) — a button
 * or separator was added; re-run the row at current bounds. */
void fdk__toolbar_layout_changed(fdk_widget *w) {
    toolbar_layout(w);
}

static void toolbar_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)w;
    (void)clip;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The bar: a track-colored band with a hairline bottom rule.
     * Buttons (children) paint on top with their own hover/press
     * states; the toolbar paints only the chrome under them. */
    fdk_surface_fill_rect(surface, bounds, fdk__pal_track());
    fdk_rect rule = { bounds.x, bounds.y + bounds.height - 1,
                      bounds.width, 1 };
    fdk_color border = fdk__pal_border();
    fdk_surface_fill_rect(surface, rule, border);
}

const fdk_widget_class fdk_toolbar_class_def = {
    .size = sizeof(fdk_toolbar),
    .name = "toolbar",
    .handle_event = NULL,
    .paint = toolbar_paint,
    .measure = toolbar_measure,
    .arrange = toolbar_arrange,
    .destroy = NULL,
};

/* ---- public API ---- */

fdk_result fdk_toolbar_create(fdk_widget *parent, fdk_font *font,
                              fdk_widget **out_toolbar) {
    if (out_toolbar == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_toolbar_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    toolbar_of(w)->font = font;
    fdk_widget_child_layout_changed(w->parent);
    *out_toolbar = w;
    return FDK_OK;
}

fdk_result fdk_toolbar_add_button(fdk_widget *toolbar,
                                  const char *text,
                                  fdk_button_activate_fn on_activate,
                                  void *user_data,
                                  fdk_widget **out_button) {
    if (toolbar == NULL || toolbar->klass != &fdk_toolbar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_toolbar *tb = toolbar_of(toolbar);
    fdk_widget *btn = NULL;
    fdk_result r = fdk_button_create(toolbar, tb->font, text, &btn);
    if (!fdk_ok(r)) {
        return r;
    }
    if (on_activate != NULL) {
        fdk_button_set_on_activate(btn, on_activate, user_data);
    }
    if (out_button != NULL) {
        *out_button = btn;
    }
    return FDK_OK;
}

fdk_result fdk_toolbar_add_separator(fdk_widget *toolbar) {
    if (toolbar == NULL || toolbar->klass != &fdk_toolbar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *sep = NULL;
    fdk_result r = fdk_separator_create(toolbar, FDK_HORIZONTAL, &sep);
    if (!fdk_ok(r)) {
        return r;
    }
    /* Vertical rules read better in a horizontal bar. */
    fdk_widget_set_natural_size(sep, 1, TB_BTN_H);
    return FDK_OK;
}
