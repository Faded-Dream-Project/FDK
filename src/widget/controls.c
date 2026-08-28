/*
 * controls.c — interactive catalog widgets (Button, Toggle,
 * Checkbox, RadioButton).
 *
 * Interaction model (see fdk_widgets.h): controls track pressed/
 * hover state from the pointer events the widget core routes to
 * them, activate on release INSIDE the widget (the Phase 4 implicit
 * grab delivers the release even if the pointer left meanwhile), and
 * on Space/Enter while focused. Disabled widgets never see events
 * (input-transparent since Phase 4), so the hooks only handle the
 * enabled path.
 *
 * The event hooks CONSUME the press/release/activation keys (return
 * true) — a button is the final consumer of its own click — but let
 * motion/enter/leave bubble (return false) so containers can still
 * react to hover.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include <stdio.h>

/* ---- shared geometry ---- */

/* Button text padding (over the measured text extent). */
#define BTN_PAD_X 16
#define BTN_PAD_Y 8
/* BTN_RADIUS moved to the theme: FDK_TM_BUTTON_CORNER_RADIUS,
 * built-in default 8 (Phase 7). */
#define BTN_FOCUS_INSET 2

/* Toggle: track 34x18, knob 12, gap 8 before optional label. */
#define TOGGLE_TRACK_W 34
#define TOGGLE_TRACK_H 18
#define TOGGLE_KNOB 12
#define TOGGLE_GAP 8

/* Checkbox: 16x16 box, 2-px check stroke, gap 8. */
#define CHECK_BOX 16
#define CHECK_GAP 8

/* Radio: 16x16 circle (r=8), inner dot r=4, gap 8. */
#define RADIO_EXTENT 16
#define RADIO_GAP 8

/* The check-family's shared indicator extent along X (drawn at the
 * left edge) and the gap that follows it. */
static fdk_i32 check_indicator_w(const fdk_widget *w) {
    if (w->klass == &fdk_toggle_class_def) {
        return TOGGLE_TRACK_W;
    }
    return CHECK_BOX; /* checkbox + radio share the extent */
}

/* ---- Button ---- */

static void button_measure(fdk_widget *w, fdk_size *out) {
    fdk_button *b = button_of(w);
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(b->font, b->text, &tw, &th);
    out->width = tw + BTN_PAD_X * 2;
    out->height = th + BTN_PAD_Y * 2;
    if (out->height < th + BTN_PAD_Y * 2) {
        out->height = th + BTN_PAD_Y * 2; /* no-shrink guard */
    }
    if (out->width < 24) {
        out->width = 24; /* tiny hit area even with no text */
    }
    if (out->height < 16) {
        out->height = 16;
    }
}

static void button_paint(fdk_widget *w, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_button *b = button_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    fdk_color fill;
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        fill = fdk__pal_control_disabled();
    } else if (b->pressed) {
        fill = fdk__pal_control_pressed();
    } else if (b->hovering) {
        fill = fdk__pal_control_hover();
    } else {
        fill = fdk__pal_control();
    }
    /* Themed corner radius (default 8 = the v1 BTN_RADIUS exactly).
     * Radius 0 = square corners - the renderer's rounded-rect treats
     * that as a plain fill. */
    fdk_i32 radius = fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, fill);

    /* Focus ring: a second rounded outline just inside the fill. */
    if ((w->flags & FDK_WF_FOCUSED) != 0) {
        fdk_rect ring = {bounds.x + BTN_FOCUS_INSET,
                         bounds.y + BTN_FOCUS_INSET,
                         bounds.width - BTN_FOCUS_INSET * 2,
                         bounds.height - BTN_FOCUS_INSET * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 ring_r = radius > BTN_FOCUS_INSET
                                 ? radius - BTN_FOCUS_INSET
                                 : 0;
            fdk_surface_draw_rounded_rect(surface, ring, ring_r,
                                          fdk__pal_accent());
        }
    }

    /* Centered text. */
    if (b->font != NULL && b->text != NULL) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(b->font, b->text, &tw, &th);
        fdk_i32 text_x = bounds.x + (bounds.width - tw) / 2;
        if (text_x < bounds.x) {
            text_x = bounds.x;
        }
        fdk_i32 baseline = fdk__center_baseline(b->font, bounds.y,
                                                bounds.height);
        fdk__draw_text(surface, b->font, b->text,
                       (w->flags & FDK_WF_ENABLED) != 0
                           ? fdk__pal_text()
                           : fdk__pal_text_disabled(),
                       text_x, baseline);
    }
}

static bool button_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev) {
    fdk_button *b = button_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
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
            ev->pointer.position.y < (fdk_f32)w->bounds.height &&
            b->on_activate != NULL) {
            b->on_activate(w, b->on_activate_data);
            /* The callback may have destroyed the widget — do not
             * touch w from here on. */
        }
        return true;
    }
    case FDK_WIDGET_KEY_DOWN:
        if (ev->key.scancode == FDK_KEY_SPACE ||
            ev->key.scancode == FDK_KEY_ENTER) {
            if (b->on_activate != NULL) {
                b->on_activate(w, b->on_activate_data);
            }
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        b->hovering = true;
        fdk_widget_invalidate(w);
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        b->hovering = false;
        fdk_widget_invalidate(w);
        return false;
    default:
        return false;
    }
}

static void button_destroy(fdk_widget *w) {
    fdk_free(button_of(w)->text);
}

/* ---- a11y ---- */

static void button_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_button *b = (const fdk_button *)(const void *)w;
    if (b->text != NULL) {
        out->name = fdk__strdup(b->text);
    }
}

static bool button_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_button *b = button_of(w);
    if (b->on_activate == NULL) {
        return false;
    }
    /* Exactly the Space/Enter path. The callback may destroy w. */
    b->on_activate(w, b->on_activate_data);
    return true;
}

static fdk_a11y_action_set button_a11y_actions(const fdk_widget *w) {
    return (((const fdk_button *)(const void *)w)->on_activate != NULL)
               ? (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE
               : 0;
}

static const fdk_a11y_class button_a11y = {
    .role = FDK_A11Y_ROLE_BUTTON,
    .describe = button_a11y_describe,
    .actions = button_a11y_actions, /* ACTIVATE only with a callback */
    .perform = button_a11y_perform,
};

const fdk_widget_class fdk_button_class_def = {
    .size = sizeof(fdk_button),
    .name = "button",
    .handle_event = button_handle_event,
    .paint = button_paint,
    .measure = button_measure,
    .arrange = NULL,
    .destroy = button_destroy,
    .a11y = &button_a11y,
};

fdk_result fdk_button_create(fdk_widget *parent, fdk_font *font,
                             const char *text, fdk_widget **out_button) {
    if (out_button == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_button_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_button *b = button_of(w);
    b->font = font;
    b->text = fdk__strdup(text);
    if (text != NULL && b->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_widget_set_can_focus(w, true);
    /* Re-measure with real fields (create's notify ran too early). */
    fdk_widget_child_layout_changed(w->parent);
    *out_button = w;
    return FDK_OK;
}

fdk_result fdk_button_set_text(fdk_widget *button, const char *text) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_button *b = button_of(button);
    char *copy = fdk__strdup(text);
    if (text != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(b->text);
    b->text = copy;
    fdk_widget_invalidate(button);
    fdk_widget_child_layout_changed(button->parent);
    /* A11y: the label IS the accessible name. */
    fdk__a11y_notify(button, FDK_A11Y_NAME_CHANGED, 0);
    return FDK_OK;
}

void fdk_button_set_on_activate(fdk_widget *button,
                                fdk_button_activate_fn on_activate,
                                void *user_data) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return;
    }
    fdk_button *b = button_of(button);
    b->on_activate = on_activate;
    b->on_activate_data = user_data;
}

/* ---- check family: shared logic ---- */

/* Fires the widget's on_change (if set). The callback may destroy
 * the widget; callers must not touch w afterwards. */
static void check_fire_change(fdk_widget *w, fdk_check_widget *c) {
    if (c->on_change != NULL) {
        c->on_change(w, c->checked, c->on_change_data);
    }
}

/* Press visual + toggle-on-release-inside, shared by toggle/checkbox/
 * radio. `activate` receives the widget after the state has flipped. */
static bool check_handle_event(fdk_widget *w,
                               const fdk_widget_event *ev,
                               void (*activate)(fdk_widget *w)) {
    fdk_check_widget *c = check_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        c->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP: {
        bool was_pressed = c->pressed;
        c->pressed = false;
        fdk_widget_invalidate(w);
        if (was_pressed && ev->pointer.position.x >= 0.0f &&
            ev->pointer.position.y >= 0.0f &&
            ev->pointer.position.x < (fdk_f32)w->bounds.width &&
            ev->pointer.position.y < (fdk_f32)w->bounds.height) {
            activate(w);
        }
        return true;
    }
    case FDK_WIDGET_KEY_DOWN:
        if (ev->key.scancode == FDK_KEY_SPACE ||
            ev->key.scancode == FDK_KEY_ENTER) {
            activate(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        c->hovering = true;
        fdk_widget_invalidate(w);
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        c->hovering = false;
        fdk_widget_invalidate(w);
        return false;
    default:
        return false;
    }
}

static void check_measure(fdk_widget *w, fdk_size *out) {
    fdk_check_widget *c = check_of(w);
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(c->font, c->text, &tw, &th);
    fdk_i32 ind_w = check_indicator_w(w);
    fdk_i32 ind_h = (w->klass == &fdk_toggle_class_def) ? TOGGLE_TRACK_H
                                                        : CHECK_BOX;
    out->width = ind_w + (tw > 0 ? TOGGLE_GAP + tw : 0);
    out->height = th > ind_h ? th : ind_h;
}

static void check_text_paint(fdk_widget *w, fdk_surface *surface,
                             fdk_rect bounds) {
    fdk_check_widget *c = check_of(w);
    if (c->font == NULL || c->text == NULL) {
        return;
    }
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(c->font, c->text, &tw, &th);
    fdk_i32 x = bounds.x + check_indicator_w(w) + TOGGLE_GAP;
    fdk_i32 baseline = fdk__center_baseline(c->font, bounds.y,
                                            bounds.height);
    fdk__draw_text(surface, c->font, c->text,
                   ((w->flags & FDK_WF_ENABLED) != 0) ? fdk__pal_text()
                              : fdk__pal_text_disabled(),
                   x, baseline);
}

static void check_destroy(fdk_widget *w) {
    fdk_free(check_of(w)->text);
}

/* Shared a11y for the check family: name = text, CHECKED/PRESSED
 * from the state, ACTIVATE = the same toggle the keyboard drives. */
static void check_a11y_describe(const fdk_widget *w, fdk_a11y_info *out,
                                 fdk_a11y_state_flag checked_flag) {
    const fdk_check_widget *c = (const fdk_check_widget *)(const void *)w;
    if (c->text != NULL) {
        out->name = fdk__strdup(c->text);
    }
    if (c->checked) {
        out->states |= checked_flag;
    }
}

static bool check_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                               double value,
                               void (*activate)(fdk_widget *)) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        return false;
    }
    activate(w); /* may run user callbacks; w may be destroyed */
    return true;
}

/* Shared set_checked for toggle/checkbox (no group semantics). */
static void simple_set_checked(fdk_widget *w, bool checked) {
    fdk_check_widget *c = check_of(w);
    if (c->checked == checked) {
        return;
    }
    c->checked = checked;
    fdk_widget_invalidate(w);
    /* A11y: the checked state flipped, before the user callback
     * (which may destroy w). */
    fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_CHECKED);
    check_fire_change(w, c);
}

/* ---- Toggle ---- */

static void toggle_paint(fdk_widget *w, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_check_widget *c = check_of(w);
    fdk_i32 track_y = bounds.y + (bounds.height - TOGGLE_TRACK_H) / 2;
    if (track_y < bounds.y) {
        track_y = bounds.y;
    }
    fdk_rect track = {bounds.x, track_y, TOGGLE_TRACK_W, TOGGLE_TRACK_H};
    fdk_i32 r = TOGGLE_TRACK_H / 2;

    fdk_color track_col = (w->flags & FDK_WF_ENABLED) == 0
                              ? fdk__pal_control_disabled()
                              : (c->checked ? fdk__pal_accent()
                                            : fdk__pal_track());
    fdk_surface_fill_rounded_rect(surface, track, r, track_col);

    /* Knob: left when off, right when on. */
    fdk_i32 knob_x = c->checked
                         ? track.x + TOGGLE_TRACK_W - TOGGLE_KNOB - 3
                         : track.x + 3;
    if (TOGGLE_TRACK_W - TOGGLE_KNOB - 6 < 0) {
        knob_x = track.x; /* degenerate tiny track */
    }
    fdk_i32 knob_y = track_y + (TOGGLE_TRACK_H - TOGGLE_KNOB) / 2;
    fdk_surface_fill_circle(surface,
                            knob_x + TOGGLE_KNOB / 2,
                            knob_y + TOGGLE_KNOB / 2, TOGGLE_KNOB / 2,
                            (fdk_color){0.92f, 0.93f, 0.96f, 1.0f});

    check_text_paint(w, surface, bounds);
}

static void toggle_activate(fdk_widget *w) {
    simple_set_checked(w, !check_of(w)->checked);
}

static bool toggle_handle_event(fdk_widget *w,
                                const fdk_widget_event *ev) {
    return check_handle_event(w, ev, toggle_activate);
}

static void toggle_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    check_a11y_describe(w, out, FDK_A11Y_PRESSED);
}

static bool toggle_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                double value) {
    return check_a11y_perform(w, action, value, toggle_activate);
}

static fdk_a11y_action_set check_a11y_actions(const fdk_widget *w) {
    (void)w;
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static const fdk_a11y_class toggle_a11y = {
    .role = FDK_A11Y_ROLE_TOGGLE_BUTTON,
    .describe = toggle_a11y_describe,
    .actions = check_a11y_actions,
    .perform = toggle_a11y_perform,
};

const fdk_widget_class fdk_toggle_class_def = {
    .size = sizeof(fdk_check_widget),
    .name = "toggle",
    .handle_event = toggle_handle_event,
    .paint = toggle_paint,
    .measure = check_measure,
    .arrange = NULL,
    .destroy = check_destroy,
    .a11y = &toggle_a11y,
};

fdk_result fdk_toggle_create(fdk_widget *parent, fdk_font *font,
                             const char *text, fdk_widget **out_toggle) {
    if (out_toggle == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_toggle_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_check_widget *c = check_of(w);
    c->font = font;
    c->text = fdk__strdup(text);
    if (text != NULL && c->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_toggle = w;
    return FDK_OK;
}

void fdk_toggle_set_checked(fdk_widget *toggle, bool checked) {
    if (toggle == NULL || toggle->klass != &fdk_toggle_class_def) {
        return;
    }
    simple_set_checked(toggle, checked);
}

bool fdk_toggle_is_checked(fdk_widget *toggle) {
    if (toggle == NULL || toggle->klass != &fdk_toggle_class_def) {
        return false;
    }
    return check_of(toggle)->checked;
}

void fdk_toggle_set_on_change(fdk_widget *toggle,
                              fdk_toggle_change_fn on_change,
                              void *user_data) {
    if (toggle == NULL || toggle->klass != &fdk_toggle_class_def) {
        return;
    }
    fdk_check_widget *c = check_of(toggle);
    c->on_change = (void (*)(fdk_widget *, bool, void *))on_change;
    c->on_change_data = user_data;
}

/* ---- Checkbox ---- */

static void checkbox_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_check_widget *c = check_of(w);
    fdk_i32 box_y = bounds.y + (bounds.height - CHECK_BOX) / 2;
    if (box_y < bounds.y) {
        box_y = bounds.y;
    }
    fdk_rect box = {bounds.x, box_y, CHECK_BOX, CHECK_BOX};

    fdk_color fill = (w->flags & FDK_WF_ENABLED) == 0
                         ? fdk__pal_control_disabled()
                         : (c->checked ? fdk__pal_accent()
                                       : (c->hovering
                                              ? fdk__pal_control_hover()
                                              : fdk__pal_control()));
    fdk_surface_fill_rounded_rect(surface, box, 4, fill);

    if (c->checked) {
        /* Check mark: two strokes inside the box. */
        fdk_color mark = (fdk_color){0.10f, 0.12f, 0.17f, 1.0f};
        fdk_surface_draw_line(
            surface, box.x + 3, box.y + 8, box.x + 6, box.y + 12, mark);
        fdk_surface_draw_line(
            surface, box.x + 6, box.y + 12, box.x + 13, box.y + 4, mark);
    }

    check_text_paint(w, surface, bounds);
}

static void checkbox_activate(fdk_widget *w) {
    simple_set_checked(w, !check_of(w)->checked);
}

static bool checkbox_handle_event(fdk_widget *w,
                                  const fdk_widget_event *ev) {
    return check_handle_event(w, ev, checkbox_activate);
}

static void checkbox_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    check_a11y_describe(w, out, FDK_A11Y_CHECKED);
}

static bool checkbox_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                                  double value) {
    return check_a11y_perform(w, action, value, checkbox_activate);
}

static const fdk_a11y_class checkbox_a11y = {
    .role = FDK_A11Y_ROLE_CHECK_BOX,
    .describe = checkbox_a11y_describe,
    .actions = check_a11y_actions,
    .perform = checkbox_a11y_perform,
};

const fdk_widget_class fdk_checkbox_class_def = {
    .size = sizeof(fdk_check_widget),
    .name = "checkbox",
    .handle_event = checkbox_handle_event,
    .paint = checkbox_paint,
    .measure = check_measure,
    .arrange = NULL,
    .destroy = check_destroy,
    .a11y = &checkbox_a11y,
};

fdk_result fdk_checkbox_create(fdk_widget *parent, fdk_font *font,
                               const char *text,
                               fdk_widget **out_checkbox) {
    if (out_checkbox == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_checkbox_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_check_widget *c = check_of(w);
    c->font = font;
    c->text = fdk__strdup(text);
    if (text != NULL && c->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_checkbox = w;
    return FDK_OK;
}

void fdk_checkbox_set_checked(fdk_widget *checkbox, bool checked) {
    if (checkbox == NULL || checkbox->klass != &fdk_checkbox_class_def) {
        return;
    }
    simple_set_checked(checkbox, checked);
}

bool fdk_checkbox_is_checked(fdk_widget *checkbox) {
    if (checkbox == NULL || checkbox->klass != &fdk_checkbox_class_def) {
        return false;
    }
    return check_of(checkbox)->checked;
}

void fdk_checkbox_set_on_change(fdk_widget *checkbox,
                                fdk_checkbox_change_fn on_change,
                                void *user_data) {
    if (checkbox == NULL || checkbox->klass != &fdk_checkbox_class_def) {
        return;
    }
    fdk_check_widget *c = check_of(checkbox);
    c->on_change = (void (*)(fdk_widget *, bool, void *))on_change;
    c->on_change_data = user_data;
}

/* ---- RadioButton ---- */

static void radio_paint(fdk_widget *w, fdk_surface *surface,
                        fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_check_widget *c = check_of(w);
    fdk_i32 cy = bounds.y + (bounds.height - RADIO_EXTENT) / 2;
    if (cy < bounds.y) {
        cy = bounds.y;
    }
    fdk_rect extent = {bounds.x, cy, RADIO_EXTENT, RADIO_EXTENT};

    fdk_color ring = (w->flags & FDK_WF_ENABLED) == 0
                         ? fdk__pal_control_disabled()
                         : (c->hovering ? fdk__pal_control_hover()
                                        : fdk__pal_control());
    fdk_surface_fill_circle(surface, extent.x + RADIO_EXTENT / 2,
                            extent.y + RADIO_EXTENT / 2,
                            RADIO_EXTENT / 2, ring);

    if (c->checked) {
        /* Inner dot, inset by a third of the extent. */
        fdk_i32 dot_r = RADIO_EXTENT / 2 - RADIO_EXTENT / 3;
        fdk_surface_fill_circle(
            surface, extent.x + RADIO_EXTENT / 2,
            extent.y + RADIO_EXTENT / 2, dot_r,
            ((w->flags & FDK_WF_ENABLED) == 0 ? fdk__pal_text_disabled()
                                              : fdk__pal_accent()));
    }

    check_text_paint(w, surface, bounds);
}

/* Group rule: the radio's PARENT widget is its group. Checking one
 * unchecks every sibling radio (silently if they have no callback;
 * their on_change fires with false otherwise). */
static void radio_uncheck_siblings(fdk_widget *radio) {
    fdk_widget *parent = radio->parent;
    if (parent == NULL) {
        return;
    }
    for (size_t i = 0; i < parent->child_count; i++) {
        fdk_widget *sib = parent->children[i];
        if (sib == radio || sib->klass != &fdk_radio_class_def) {
            continue;
        }
        fdk_check_widget *sc = check_of(sib);
        if (sc->checked) {
            sc->checked = false;
            fdk_widget_invalidate(sib);
            /* A11y: the sibling's checked state flipped to false. */
            fdk__a11y_notify(sib, FDK_A11Y_STATE_CHANGED,
                             FDK_A11Y_CHECKED);
            if (sc->on_change != NULL) {
                sc->on_change(sib, false, sc->on_change_data);
            }
        }
    }
}

static void radio_set_checked_impl(fdk_widget *w, bool checked) {
    fdk_check_widget *c = check_of(w);
    if (checked && !c->checked) {
        radio_uncheck_siblings(w);
        c->checked = true;
        fdk_widget_invalidate(w);
        /* A11y: this radio + the unchecked siblings all flipped. */
        fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_CHECKED);
        check_fire_change(w, c);
    } else if (!checked && c->checked) {
        c->checked = false;
        fdk_widget_invalidate(w);
        fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_CHECKED);
        check_fire_change(w, c);
    }
}

static void radio_activate(fdk_widget *w) {
    radio_set_checked_impl(w, true);
}

/* Arrow-key traversal target within the radio's group (its parent):
 * the previous (Up/Left) or next (Down/Right) sibling radio in child
 * order, wrapping around. Skips non-radios and radios that could not
 * take focus (hidden, disabled, or non-focusable) — selection never
 * lands somewhere the keyboard cannot be. Returns NULL when the
 * group has no other traversable member (a lone radio lets the
 * arrows bubble to ancestors). */
static fdk_widget *radio_arrow_target(fdk_widget *w, bool backward) {
    fdk_widget *parent = w->parent;
    if (parent == NULL || parent->child_count == 0) {
        return NULL;
    }
    size_t count = parent->child_count;
    size_t me = count;
    for (size_t i = 0; i < count; i++) {
        if (parent->children[i] == w) {
            me = i;
            break;
        }
    }
    if (me == count) {
        return NULL; /* not in its parent's list (should not happen) */
    }
    for (size_t step = 1; step <= count; step++) {
        size_t idx = backward ? (me + count - step) % count
                              : (me + step) % count;
        fdk_widget *cand = parent->children[idx];
        if (cand == w) {
            break; /* wrapped all the way around: no other member */
        }
        if (cand->klass != &fdk_radio_class_def ||
            !fdk_widget_is_effectively_visible(cand) ||
            !fdk_widget_is_effectively_enabled(cand) ||
            !fdk_widget_get_can_focus(cand)) {
            continue;
        }
        return cand;
    }
    return NULL;
}

static bool radio_handle_event(fdk_widget *w,
                               const fdk_widget_event *ev) {
    /* Arrow keys move selection within the group (and focus with
     * it): Up/Left previous, Down/Right next, wrapping. Standard
     * radio-group behavior — the arrows belong to the group, not the
     * window, whenever the group has another member. */
    if (ev->type == FDK_WIDGET_KEY_DOWN) {
        fdk_scancode sc = ev->key.scancode;
        if (sc == FDK_KEY_UP || sc == FDK_KEY_DOWN ||
            sc == FDK_KEY_LEFT || sc == FDK_KEY_RIGHT) {
            bool backward = (sc == FDK_KEY_UP || sc == FDK_KEY_LEFT);
            fdk_widget *target = radio_arrow_target(w, backward);
            if (target == NULL) {
                return false; /* lone radio: arrows bubble */
            }
            fdk_widget_focus(target);
            radio_set_checked_impl(target, true);
            return true;
        }
    }
    return check_handle_event(w, ev, radio_activate);
}

static void radio_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    check_a11y_describe(w, out, FDK_A11Y_CHECKED);
}

static bool radio_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                               double value) {
    return check_a11y_perform(w, action, value, radio_activate);
}

static const fdk_a11y_class radio_a11y = {
    .role = FDK_A11Y_ROLE_RADIO_BUTTON,
    .describe = radio_a11y_describe,
    .actions = check_a11y_actions,
    .perform = radio_a11y_perform,
};

const fdk_widget_class fdk_radio_class_def = {
    .size = sizeof(fdk_check_widget),
    .name = "radio",
    .handle_event = radio_handle_event,
    .paint = radio_paint,
    .measure = check_measure,
    .arrange = NULL,
    .destroy = check_destroy,
    .a11y = &radio_a11y,
};

fdk_result fdk_radio_create(fdk_widget *parent, fdk_font *font,
                            const char *text, fdk_widget **out_radio) {
    if (out_radio == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_radio_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_check_widget *c = check_of(w);
    c->font = font;
    c->text = fdk__strdup(text);
    if (text != NULL && c->text == NULL) {
        fdk_widget_destroy(w);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_radio = w;
    return FDK_OK;
}

void fdk_radio_set_checked(fdk_widget *radio, bool checked) {
    if (radio == NULL || radio->klass != &fdk_radio_class_def) {
        return;
    }
    radio_set_checked_impl(radio, checked);
}

bool fdk_radio_is_checked(fdk_widget *radio) {
    if (radio == NULL || radio->klass != &fdk_radio_class_def) {
        return false;
    }
    return check_of(radio)->checked;
}

void fdk_radio_set_on_change(fdk_widget *radio,
                             fdk_radio_change_fn on_change,
                             void *user_data) {
    if (radio == NULL || radio->klass != &fdk_radio_class_def) {
        return;
    }
    fdk_check_widget *c = check_of(radio);
    c->on_change = (void (*)(fdk_widget *, bool, void *))on_change;
    c->on_change_data = user_data;
}
