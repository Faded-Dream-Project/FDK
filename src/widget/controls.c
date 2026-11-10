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
#include "../window/window_internal.h"

#include "core/alloc_internal.h"
#include <stdio.h>
#include <stdint.h>

/* ---- shared geometry ---- */

/* Button text padding (over the measured text extent). */
#define BTN_PAD_X 16
#define BTN_PAD_Y 8
/* BTN_RADIUS moved to the theme: FDK_TM_BUTTON_CORNER_RADIUS,
 * built-in default 8 (Phase 7). The focus-ring inset/width moved to
 * the theme too: FDK_TM_FOCUS_RING_WIDTH, built-in default 2 (1.0.0). */

/* The hover fade flight (1.0.0): 120 ms of quad-out — hover is a
 * whisper, not a wave. Shared by Button, the check family (1.0.0),
 * the StackSwitcher's pills and the menu/combo rows (1.0.0). */
#define HOVER_FADE_MS 120

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

/* ---- 1.0.0: the hover fade (shared by Button + check family) ---- */

/* Linear channel blend, clamped — the paint-time hover ladder. */
static fdk_color blend_colors(fdk_color a, fdk_color b, fdk_f32 t) {
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return (fdk_color){a.r + (b.r - a.r) * t,
                       a.g + (b.g - a.g) * t,
                       a.b + (b.b - a.b) * t,
                       a.a + (b.a - a.a) * t};
}

/* Arms (or retargets) the fade toward 1 (entering) / 0 (leaving) —
 * the shared machinery in statics.c (1.0.0 promoted it so the
 * switcher pills and menu rows ride what the buttons ride): cancels
 * any running flight first, departing from the CURRENT blend,
 * never teleporting. */

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
    if (b->span_count > 0 && b->text != NULL && b->font != NULL) {
        /* Markup (1.0.0): styled widths — a bold word is BOLD-wide.
         * Height stays the font's pitch (synthesis never changes it). */
        fdk_text_metrics m;
        if (fdk_ok(fdk_font_measure_spans_utf8(
                b->font, b->text, strlen(b->text), b->spans,
                b->span_count, &m))) {
            tw = m.advance_width;
        }
        fdk_font_metrics fm;
        fdk_font_get_metrics(b->font, &fm);
        th = fm.ascent + fm.descent;
    } else {
        fdk__text_extent(b->font, b->text, &tw, &th);
    }
    out->width = tw + BTN_PAD_X * 2;
    out->height = th + BTN_PAD_Y * 2;
    if (out->width < 24) {
        out->width = 24; /* tiny hit area even with no text */
    }
    if (out->height < 16) {
        out->height = 16;
    }
    /* Hostile font metrics (a face declaring a giant ascent) could in
     * principle push tw/th past the fdk_i32 budget; the measure
     * contract only promises sane fonts, but saturate rather than
     * wrap — a wrapped negative would poison layout. */
    if (out->width < 0) {
        out->width = INT32_MAX;
    }
    if (out->height < 0) {
        out->height = INT32_MAX;
    }
}

/* ---- 1.0.0: the themed button SHAPE ----
 *
 * The effective corner radius for a button-shaped (w, h) box under
 * the current theme's FDK_TM_BUTTON_SHAPE: ROUNDED defers to the
 * button_corner_radius metric (the historical look), CIRCLE clips
 * to half the short side — a true circle on a square button, a pill
 * on a wide one — and SQUARE is sharp regardless of the metric. The
 * renderer clamps any radius to min(w,h)/2 itself, but the callers
 * want the number (focus-ring insets mirror it), so the arithmetic
 * lives here once instead of three times. */
fdk_i32 fdk__button_shape_radius(fdk_i32 w, fdk_i32 h) {
    switch (fdk_theme_get_metric(NULL, FDK_TM_BUTTON_SHAPE)) {
    case 1: /* CIRCLE */
        return (w < h ? w : h) / 2;
    case 2: /* SQUARE */
        return 0;
    default: /* ROUNDED: the metric, clamped to the box */
        {
            fdk_i32 r = fdk_theme_get_metric(NULL,
                                             FDK_TM_BUTTON_CORNER_RADIUS);
            fdk_i32 half = (w < h ? w : h) / 2;
            if (r < 0) {
                r = 0;
            }
            if (r > half) {
                r = half;
            }
            return r;
        }
    }
}

static void button_paint(fdk_widget *w, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_button *b = button_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The active-state ladder, per role (1.0.0; 1.0.0 fades the
     * HOVER rungs). NORMAL rides the control family as v1 did;
     * SUGGESTED is accent-filled with the accent's own hover/
     * pressed states and accent-text label; DESTRUCTIVE is
     * danger-filled, its hover/pressed derived by an inline shade
     * (no token pair — the family is one color wide); LINK is the
     * flat hyperlink — no fill, link-colored text, an underline
     * that fades in under the pointer (persistent when checked —
     * the "visited/active" read). Pressed/checked still snap: the
     * fade is a hover whisper, not input lag. */
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;
    const bool active = b->pressed || b->checked;
    const fdk_f32 ht = b->fade.t; /* the hover blend */
    fdk_color fill;
    fdk_color label_col;
    switch (b->role) {
    case FDK_BUTTON_ROLE_LINK:
        fill = (fdk_color){0.0f, 0.0f, 0.0f, 0.0f}; /* flat: no fill */
        label_col = disabled ? fdk__pal_text_disabled() : fdk__pal_link();
        break;
    case FDK_BUTTON_ROLE_SUGGESTED:
        fill = disabled ? fdk__pal_control_disabled()
                : active   ? fdk__pal_accent_pressed()
                : blend_colors(fdk__pal_accent(), fdk__pal_accent_hover(),
                               ht);
        label_col = disabled ? fdk__pal_text_disabled()
                              : fdk__pal_accent_text();
        break;
    case FDK_BUTTON_ROLE_DESTRUCTIVE: {
        fdk_color danger = fdk_theme_get_color(NULL, FDK_TK_DANGER);
        fdk_color hover = {danger.r * 0.90f, danger.g * 0.90f,
                           danger.b * 0.90f, danger.a};
        if (disabled) {
            fill = fdk__pal_control_disabled();
        } else if (active) {
            fill = (fdk_color){danger.r * 0.78f, danger.g * 0.78f,
                               danger.b * 0.78f, danger.a};
        } else {
            fill = blend_colors(danger, hover, ht);
        }
        label_col = disabled ? fdk__pal_text_disabled()
                              : fdk__pal_accent_text();
        break;
    }
    default:
        fill = disabled ? fdk__pal_control_disabled()
                : active ? fdk__pal_control_pressed()
                : blend_colors(fdk__pal_control(), fdk__pal_control_hover(),
                               ht);
        label_col = disabled ? fdk__pal_text_disabled() : fdk__pal_text();
        break;
    }
    /* Themed corner shape (1.0.0): ROUNDED rides the
     * button_corner_radius metric (default 8 = the v1 BTN_RADIUS
     * exactly; radius 0 = square corners — the renderer's
     * rounded-rect treats that as a plain fill), CIRCLE rounds to
     * half the short side, SQUARE forces 0. The focus-ring inset
     * below mirrors the same number so a ring follows its button's
     * silhouette at any shape. */
    fdk_i32 radius = fdk__button_shape_radius(bounds.width,
                                               bounds.height);
    if (fill.a > 0.0f) {
        fdk_surface_fill_rounded_rect_aa(surface, bounds, radius, fill);
    }

    /* Focus ring: a themed-color, themed-width rounded outline just
     * inside the fill (1.0.0: FOCUS_RING token + FOCUS_RING_WIDTH
     * metric, replacing the hardcoded accent hairline). LINK draws
     * it too — a flat button is still a keyboard citizen. */
    if ((w->flags & FDK_WF_FOCUSED) != 0 && !disabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2,
                         bounds.height - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 ring_r = radius > fw ? radius - fw : 0;
            fdk_surface_draw_rounded_rect_aa(surface, ring, ring_r,
                                          fdk__pal_focus_ring());
        }
    }

    /* Centered text. */
    if (b->font != NULL && b->text != NULL) {
        fdk_i32 tw = 0, th = 0;
        if (b->span_count > 0) {
            fdk_text_metrics m;
            if (fdk_ok(fdk_font_measure_spans_utf8(
                    b->font, b->text, strlen(b->text), b->spans,
                    b->span_count, &m))) {
                tw = m.advance_width;
            }
            fdk_font_metrics fm;
            fdk_font_get_metrics(b->font, &fm);
            th = fm.ascent + fm.descent;
        } else {
            fdk__text_extent(b->font, b->text, &tw, &th);
        }
        fdk_i32 text_x = bounds.x + (bounds.width - tw) / 2;
        if (text_x < bounds.x) {
            text_x = bounds.x;
        }
        fdk_i32 baseline = fdk__center_baseline(b->font, bounds.y,
                                                bounds.height);
        if (b->span_count > 0) {
            /* Markup: styled glyphs, run colors, decoration bars. */
            (void)fdk_surface_draw_spans_utf8(
                surface, b->font, b->text, strlen(b->text), b->spans,
                b->span_count, text_x, baseline, label_col);
        } else {
            fdk__draw_text(surface, b->font, b->text, label_col,
                           text_x, baseline);
        }
        /* LINK underline: under the whole text run, fading in with
         * the hover blend (or persistent when checked — the active
         * link read). Same baseline+2 rule as the mnemonics. */
        if (b->role == FDK_BUTTON_ROLE_LINK && !disabled) {
            fdk_f32 u = b->checked ? 1.0f : ht;
            if (u > 0.01f) {
                fdk_color lc = fdk__pal_link();
                fdk_color ul = {lc.r, lc.g, lc.b, lc.a * u};
                fdk_surface_fill_rect(
                    surface,
                    (fdk_rect){text_x, baseline + 2, tw, 1}, ul);
            }
        }
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

static void button_destroy(fdk_widget *w) {
    fdk_button *b = button_of(w);
    fdk_free(b->text);
    fdk_free(b->spans);
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
    .instance_finalize = button_destroy,
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
    /* Plain text clears any markup spans (the setters are mutually
     * exclusive by ownership). */
    fdk_free(b->spans);
    b->spans = NULL;
    b->span_count = 0;
    fdk_widget_invalidate(button);
    fdk_widget_child_layout_changed(button->parent);
    /* A11y: the label IS the accessible name. */
    fdk__a11y_notify(button, FDK_A11Y_NAME_CHANGED, 0);
    return FDK_OK;
}

/* fdk_button_set_markup (1.0.0): the Label's markup contract on a
 * button — tags parse to plain text + spans, get_text reports the
 * PLAIN label (what the a11y tree narrates), and measure/paint use
 * the styled widths. The role machinery (fills, focus ring, the
 * LINK underline) is markup-agnostic: the underline still bars the
 * whole run, under whatever the spans paint. */
fdk_result fdk_button_set_markup(fdk_widget *button, const char *markup) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_button *b = button_of(button);

    char *plain = NULL;
    fdk_span *spans = NULL;
    size_t span_count = 0;
    if (markup != NULL) {
        fdk_result r = fdk_markup_parse(markup, &plain, &spans,
                                        &span_count);
        if (!fdk_ok(r)) {
            return r; /* old content untouched (OOM discipline) */
        }
    }

    fdk_free(b->text);
    fdk_free(b->spans);
    b->text = plain;
    b->spans = spans;
    b->span_count = span_count;
    fdk_widget_invalidate(button);
    fdk_widget_child_layout_changed(button->parent);
    /* A11y: the PLAIN label is the accessible name. */
    fdk__a11y_notify(button, FDK_A11Y_NAME_CHANGED, 0);
    return FDK_OK;
}

/* The button's current label (toolkit-owned copy; valid until the
 * next set_text/destroy). NULL when the button has no text — the
 * same convention as fdk_label_get_text (1.0.0: every text-bearing
 * widget now has its getter). */
const char *fdk_button_get_text(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return NULL;
    }
    return button_of(button)->text;
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

/* ---- 1.0.0: role + toggle-button state ---- */

void fdk_button_set_role(fdk_widget *button, fdk_button_role role) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return;
    }
    switch (role) {
    case FDK_BUTTON_ROLE_NORMAL:
    case FDK_BUTTON_ROLE_SUGGESTED:
    case FDK_BUTTON_ROLE_DESTRUCTIVE:
    case FDK_BUTTON_ROLE_LINK:
        break;
    default:
        return; /* unknown values ignored (documented) */
    }
    fdk_button *b = button_of(button);
    if (b->role == role) {
        return;
    }
    b->role = role;
    fdk_widget_invalidate(button);
}

fdk_button_role fdk_button_get_role(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return FDK_BUTTON_ROLE_NORMAL;
    }
    return button_of(button)->role;
}

void fdk_button_set_checked(fdk_widget *button, bool checked) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return;
    }
    fdk_button *b = button_of(button);
    checked = checked ? true : false; /* clamp to exactly true/false */
    if (b->checked == checked) {
        return;
    }
    b->checked = checked;
    fdk_widget_invalidate(button);
}

bool fdk_button_is_checked(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_button_class_def) {
        return false;
    }
    return button_of(button)->checked;
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
        fdk__hover_fade_arm(w, &c->fade, true);
        fdk_widget_invalidate(w);
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        c->hovering = false;
        fdk__hover_fade_arm(w, &c->fade, false);
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
    fdk_surface_fill_rounded_rect_aa(surface, track, r, track_col);

    /* Knob: left when off, right when on. */
    fdk_i32 knob_x = c->checked
                         ? track.x + TOGGLE_TRACK_W - TOGGLE_KNOB - 3
                         : track.x + 3;
    if (TOGGLE_TRACK_W - TOGGLE_KNOB - 6 < 0) {
        knob_x = track.x; /* degenerate tiny track */
    }
    fdk_i32 knob_y = track_y + (TOGGLE_TRACK_H - TOGGLE_KNOB) / 2;
    fdk_surface_fill_circle_aa(surface,
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
    .instance_finalize = check_destroy,
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

void fdk_toggle_set_on_changed(fdk_widget *toggle,
                              fdk_toggle_changed_fn on_change,
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
                                       : blend_colors(fdk__pal_control(),
                                                      fdk__pal_control_hover(),
                                                      c->fade.t));
    fdk_surface_fill_rounded_rect_aa(surface, box, 4, fill);

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
    .instance_finalize = check_destroy,
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

void fdk_checkbox_set_on_changed(fdk_widget *checkbox,
                                fdk_checkbox_changed_fn on_change,
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
    fdk_surface_fill_circle_aa(surface, extent.x + RADIO_EXTENT / 2,
                            extent.y + RADIO_EXTENT / 2,
                            RADIO_EXTENT / 2, ring);

    if (c->checked) {
        /* Inner dot, inset by a third of the extent. */
        fdk_i32 dot_r = RADIO_EXTENT / 2 - RADIO_EXTENT / 3;
        fdk_surface_fill_circle_aa(
            surface, extent.x + RADIO_EXTENT / 2,
            extent.y + RADIO_EXTENT / 2, dot_r,
            ((w->flags & FDK_WF_ENABLED) == 0 ? fdk__pal_text_disabled()
                                              : fdk__pal_accent()));
    }

    check_text_paint(w, surface, bounds);
}

/* Group rule: the radio's PARENT widget is its group. Checking one
 * unchecks every sibling radio (silently if they have no callback;
 * their on_change fires with false otherwise).
 *
 * Reentrancy: sibling on_change callbacks run OUTSIDE event dispatch
 * here — a callback may legally destroy radios (including ones later
 * in the loop), reparent them out of the group, or append new
 * children. The sweep is therefore snapshot + watch based: the child
 * list is copied once, every snapshot entry is watched, and an entry
 * is only touched when its watch still reports it alive AND it is
 * still parented to the same group (a reparented radio left the
 * group by definition). A destroyed parent frees the whole snapshot —
 * every watch reads dead, nothing is dereferenced. */
static void radio_uncheck_siblings(fdk_widget *radio) {
    fdk_widget *parent = radio->parent;
    if (parent == NULL || parent->child_count == 0) {
        return;
    }
    size_t n = parent->child_count;
    fdk_widget **snap = fdk_alloc(n * sizeof *snap);
    fdk_widget_watch *watches =
        fdk_alloc_array(n, sizeof *watches);
    if (snap == NULL || watches == NULL) {
        /* OOM: skip the sweep. The group rule is momentarily
         * unenforced (two checked radios) — a visual inconsistency,
         * never a memory-safety hazard; the next check self-heals. */
        fdk_free(snap);
        fdk_free(watches);
        return;
    }
    memcpy(snap, parent->children, n * sizeof *snap);
    for (size_t i = 0; i < n; i++) {
        fdk__widget_watch(&watches[i], snap[i]);
    }
    for (size_t i = 0; i < n; i++) {
        fdk_widget *sib = watches[i].target; /* NULL = died mid-sweep */
        if (sib == NULL || sib == radio || sib->parent != parent) {
            continue;
        }
        if (sib->klass != &fdk_radio_class_def) {
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
    for (size_t i = 0; i < n; i++) {
        fdk__widget_unwatch(&watches[i]);
    }
    fdk_free(snap);
    fdk_free(watches);
}

static void radio_set_checked_impl(fdk_widget *w, bool checked) {
    fdk_check_widget *c = check_of(w);
    if (checked && !c->checked) {
        /* State BEFORE callbacks, and a watch across them: a sibling's
         * on_change may destroy THIS radio (legal outside dispatch,
         * where destroy frees immediately) — the watch keeps the
         * post-callback fire from touching freed memory. */
        c->checked = true;
        fdk_widget_invalidate(w);
        /* A11y: this radio + the unchecked siblings all flipped. */
        fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_CHECKED);
        fdk_widget_watch watch;
        fdk__widget_watch(&watch, w);
        radio_uncheck_siblings(w);
        if (fdk__watch_alive(&watch)) {
            check_fire_change(w, c);
        }
        fdk__widget_unwatch(&watch);
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
    .instance_finalize = check_destroy,
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

void fdk_radio_set_on_changed(fdk_widget *radio,
                             fdk_radio_changed_fn on_change,
                             void *user_data) {
    if (radio == NULL || radio->klass != &fdk_radio_class_def) {
        return;
    }
    fdk_check_widget *c = check_of(radio);
    c->on_change = (void (*)(fdk_widget *, bool, void *))on_change;
    c->on_change_data = user_data;
}


/* ---- Scale button (1.0.0) --------------------------------------------
 *
 * The slider-in-a-popup control (the GtkScaleButton/GtkVolumeButton
 * row, FDK's own machinery): a compact button whose face is a value
 * meter; a press pops a small window carrying a vertical slider and
 * a live value label. See fdk_widgets.h for the contract. */

#define SB_W 44            /* natural width                          */
#define SB_H 28            /* natural height                         */
#define SB_METER_X 9       /* the meter's x inset inside the face    */
#define SB_METER_H 10      /* the meter trough height                */
#define SB_POP_W 48        /* the popup window's width               */
#define SB_POP_H 176       /* the popup window's height              */
#define SB_POP_PAD 8       /* the slider's inset inside the popup    */
#define SB_POP_LABEL_H 16  /* the value label's band at the bottom   */

/* The heap guard bridging the popup's callbacks and a button that
 * may die while its popup is open — the menubutton's pattern,
 * model-free. The guard outlives the button while the popup lives;
 * every callback treats a NULL widget pointer as "button gone, do
 * bookkeeping only". Freed exactly once, by the closed hook (every
 * end path). */
typedef struct sb_guard {
    fdk_widget *button;      /* NULL once the button is destroyed */
    fdk_window *popup;       /* the popup window (dies with it)    */
    fdk_widget *slider;      /* the popup's slider (lives with it) */
    fdk_widget *label;       /* the popup's value label            */
} sb_guard;

typedef struct fdk_scale_button {
    fdk_widget base;
    fdk_font *font;          /* borrowed; the popup's value label */
    double min, max, value;  /* the min < max invariant           */
    double step;             /* 0 = continuous                    */
    bool pressed;
    bool hovering;
    fdk_hover_fade fade;
    fdk_scale_button_changed_fn on_change;
    void *on_change_data;
    fdk_window *popup;       /* the open popup (NULL when closed) */
    sb_guard *guard;         /* shared with the popup callbacks   */
} fdk_scale_button;

static fdk_scale_button *sb_of(fdk_widget *w) {
    return (fdk_scale_button *)(void *)w;
}

extern const fdk_widget_class fdk_scale_button_class_def;

static const char *sb_value_text(const fdk_scale_button *sb, char *buf,
                                 size_t cap) {
    (void)snprintf(buf, cap, "%.10g", sb->value);
    return buf;
}

/* The closed hook: fired on EVERY end path (the close request, the
 * outside click, the destroy notify, the toggle-press teardown —
 * whatever destroys the popup runs this once). */
static void sb_popup_closed(void *user) {
    sb_guard *guard = user;
    fdk_widget *w = guard->button;
    if (w != NULL && w->klass == &fdk_scale_button_class_def) {
        fdk_scale_button *b = sb_of(w);
        if (b->guard == guard) {
            b->popup = NULL;
            b->guard = NULL;
            fdk_widget_invalidate(w);
        }
        /* Else: the button already reset its pointers (the toggle
         * teardown path) — the guard is still the one to free. */
    }
    fdk_free(guard);
}

static void sb_close(fdk_scale_button *b) {
    if (b->popup == NULL) {
        return;
    }
    /* Reset the button side first: the destroy notify runs the
     * closed hook, which must not clear already-reset pointers.
     * The notify runs SYNCHRONOUSLY (destroy_notify fires while
     * the window is still whole, before teardown), so it frees
     * the guard before this returns — nothing here may touch the
     * guard afterwards. */
    fdk_window *pop = b->popup;
    b->popup = NULL;
    b->guard = NULL;
    fdk_window_destroy(pop);
}

/* The close path FROM the popup's own callbacks (the guard holds
 * the window): the button side when it lives, the raw destroy when
 * it does not. The caller must not touch the guard after this —
 * the destroy notify frees it. */
static void sb_close_from_guard(sb_guard *guard) {
    fdk_widget *w = guard->button;
    if (w != NULL && w->klass == &fdk_scale_button_class_def) {
        sb_close(sb_of(w));
        return;
    }
    if (guard->popup != NULL) {
        fdk_window_destroy(guard->popup);
    }
}

static void sb_popup_destroyed(fdk_window *window, void *user) {
    (void)window; /* the window is still whole here; do NOT destroy */
    /* The popup died under us (the owner window closed, the WM
     * killed it, or sb_close destroyed it): run the closed hook. */
    sb_popup_closed(user);
}

static void sb_popup_event(fdk_window *window, const fdk_event_data *ev,
                           void *user) {
    (void)window;
    sb_guard *guard = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        sb_close_from_guard(guard);
        return;
    }
    if (ev->type == FDK_EVENT_POINTER_BUTTON_DOWN) {
        /* A press delivered while the grab routes it here: outside
         * the popup's bounds means the user clicked away — the
         * dismissal contract. */
        fdk_size wsz = {0, 0};
        (void)fdk_window_get_size(window, &wsz);
        if (ev->pointer_button.position.x < 0.0f ||
            ev->pointer_button.position.y < 0.0f ||
            ev->pointer_button.position.x >= (float)wsz.width ||
            ev->pointer_button.position.y >= (float)wsz.height) {
            sb_close_from_guard(guard);
        }
    }
}

static void sb_slider_changed(fdk_widget *slider, void *user) {
    sb_guard *guard = user;
    double value = fdk_slider_get_value(slider);
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%.10g", value);
    if (guard->label != NULL) {
        fdk_label_set_text(guard->label, buf);
        fdk_widget_invalidate(guard->label);
    }
    fdk_widget *w = guard->button;
    if (w == NULL || w->klass != &fdk_scale_button_class_def) {
        return; /* the button is gone: label bookkeeping only */
    }
    fdk_scale_button *b = sb_of(w);
    b->value = value;
    fdk_widget_invalidate(w);
    fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
    if (b->on_change != NULL) {
        /* The app callback may destroy the button (or the popup) —
         * nothing is touched after it returns. */
        b->on_change(w, value, b->on_change_data);
    }
}

static void sb_open_popup(fdk_widget *w) {
    fdk_scale_button *b = sb_of(w);
    if (b->popup != NULL) {
        sb_close(b);
        return; /* the second press is the toggle-off half */
    }
    fdk_window *owner = fdk__window_of_owner(fdk__widget_window_owner(w));
    fdk_context *ctx = fdk__window_context(fdk__widget_window_owner(w));
    if (owner == NULL || ctx == NULL) {
        return; /* detached tree: documented no-op */
    }
    /* Below the button, centered on it — the menu-anchor rule, no
     * owner-bounds clamping: a popup is a root-level window and
     * hangs BELOW its owner onto the desktop exactly like a menu
     * does (the first cut clamped into the owner's 190px frame,
     * slammed a 176px popup onto the title band, and the field
     * report was the proof nobody could reach it). */
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_i32 x = abs.x + (abs.width - SB_POP_W) / 2;
    fdk_i32 y = abs.y + abs.height + 2;

    sb_guard *guard = fdk_alloc(sizeof(*guard));
    if (guard == NULL) {
        return; /* OOM: no popup this press (honest) */
    }
    guard->button = w;
    guard->popup = NULL;
    guard->slider = NULL;
    guard->label = NULL;
    fdk_window *pop = NULL;
    fdk_result r = fdk_window_create_popup(ctx, owner, x, y, SB_POP_W,
                                           SB_POP_H, &pop);
    if (!fdk_ok(r)) {
        fdk_free(guard);
        return;
    }
    fdk__window_set_auto_paint(pop, true);
    fdk__window_set_destroy_notify(pop, sb_popup_destroyed, guard);
    fdk_window_set_event_callback(pop, sb_popup_event, guard);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(pop, &root);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return;
    }
    fdk_widget *slider = NULL;
    r = fdk_slider_create(root, b->min, b->max, b->value, &slider);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return;
    }
    fdk_slider_set_orientation(slider, FDK_SLIDER_VERTICAL);
    if (b->step > 0.0) {
        fdk_slider_set_step(slider, b->step);
    }
    fdk_widget_arrange(slider, (fdk_rect){SB_POP_PAD, SB_POP_PAD,
                                          SB_POP_W - SB_POP_PAD * 2,
                                          SB_POP_H - SB_POP_PAD * 2 -
                                              SB_POP_LABEL_H});
    fdk_slider_set_on_changed(slider, sb_slider_changed, guard);

    char buf[32];
    (void)snprintf(buf, sizeof(buf), "%.10g", b->value);
    fdk_widget *label = NULL;
    r = fdk_label_create(root, b->font, buf, &label);
    if (!fdk_ok(r)) {
        fdk_window_destroy(pop);
        return;
    }
    fdk_widget_arrange(label, (fdk_rect){0, SB_POP_H - SB_POP_PAD -
                                              SB_POP_LABEL_H,
                                         SB_POP_W, SB_POP_LABEL_H});
    guard->popup = pop;
    guard->slider = slider;
    guard->label = label;
    b->popup = pop;
    b->guard = guard;
    /* SHOW IT (the menu-session order: create, pack, show — the
     * grab happens on map, inside the platform). The first cut
     * packed the slider and label into a window that was never
     * mapped: invisible on every real desktop, while the SendEvent
     * GUI tests "passed" — XSendEvent delivers to unmapped windows,
     * bypassing mapping, grabs, and geometry entirely. The XTest
     * rig in the integration suite is the proof that maps. */
    fdk_window_show(pop);
    fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_EXPANDED);
}

static void sb_toggle_popup(fdk_widget *w) {
    fdk_scale_button *b = sb_of(w);
    if (b->popup != NULL) {
        sb_close(b);
        return;
    }
    sb_open_popup(w);
}

static bool sb_handle_event(fdk_widget *w,
                            const fdk_widget_event *ev) {
    fdk_scale_button *b = sb_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if ((w->flags & FDK_WF_ENABLED) == 0 ||
            ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        b->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (b->pressed) {
            b->pressed = false;
            fdk_widget_invalidate(w);
            if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
                sb_open_popup(w); /* the toggle-off half closes */
            }
        }
        return true;
    case FDK_WIDGET_KEY_DOWN:
        if ((w->flags & FDK_WF_ENABLED) != 0 &&
            (ev->key.scancode == FDK_KEY_SPACE ||
             ev->key.scancode == FDK_KEY_ENTER)) {
            sb_toggle_popup(w);
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

static void sb_paint(fdk_widget *w, fdk_surface *surface,
                     fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_scale_button *b = sb_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The face: the control-family paint (hover blend, pressed
     * snapping, the disabled override). */
    fdk_color face = fdk__pal_control();
    if (b->fade.t > 0.0f) {
        fdk_color hover = fdk__pal_control_hover();
        face.r = face.r + (hover.r - face.r) * b->fade.t;
        face.g = face.g + (hover.g - face.g) * b->fade.t;
        face.b = face.b + (hover.b - face.b) * b->fade.t;
        face.a = face.a + (hover.a - face.a) * b->fade.t;
    }
    if (b->pressed) {
        face = fdk__pal_control_pressed();
    }
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        face = fdk__pal_control_disabled();
    }
    fdk_i32 radius =
        fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect_aa(surface, bounds, radius, face);
    if ((w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_draw_rounded_rect_aa(surface, bounds, radius,
                                      fdk__pal_border());
    }

    /* The meter: trough + accent fill proportional to the value —
     * the volume icon's honest stand-in. */
    fdk_i32 meter_y = bounds.y + (bounds.height - SB_METER_H) / 2;
    fdk_rect trough = {bounds.x + SB_METER_X, meter_y,
                       bounds.width - SB_METER_X * 2, SB_METER_H};
    if (trough.width <= 0) {
        trough.width = 1;
    }
    fdk_color track = fdk__pal_control_pressed();
    fdk_surface_fill_rounded_rect_aa(surface, trough, 4, track);
    double span = b->max - b->min;
    double frac = span > 0.0
                      ? (b->value - b->min) / span
                      : 0.0;
    if (frac < 0.0) {
        frac = 0.0;
    }
    if (frac > 1.0) {
        frac = 1.0;
    }
    fdk_i32 fill_w = (fdk_i32)((double)trough.width * frac + 0.5);
    if (fill_w > 0) {
        fdk_color fill = fdk__pal_accent();
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            fill.r = fill.r * 0.5f + face.r * 0.5f;
            fill.g = fill.g * 0.5f + face.g * 0.5f;
            fill.b = fill.b * 0.5f + face.b * 0.5f;
        }
        fdk_rect bar = {trough.x, trough.y, fill_w, trough.height};
        fdk_surface_fill_rounded_rect_aa(surface, bar, 4, fill);
    }
}

static void sb_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = SB_W;
    out->height = SB_H;
}

static void sb_destroy(fdk_widget *w) {
    fdk_scale_button *b = sb_of(w);
    if (b->popup != NULL) {
        /* The popup outlives the button (the combo's abandonment
         * rule): the guard's button pointer NULLs, the closed hook
         * still frees it when the popup ends. */
        b->guard->button = NULL;
        b->popup = NULL;
        b->guard = NULL;
    }
}

/* ---- a11y: a BUTTON whose value is the number ---- */

static void sb_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_scale_button *sb = (const fdk_scale_button *)(const void *)w;
    char buf[32];
    out->value_text = fdk__strdup(sb_value_text(sb, buf, sizeof(buf)));
    out->has_value = out->value_text != NULL;
    out->value_min = sb->min;
    out->value_max = sb->max;
    out->value_current = sb->value;
}

static fdk_a11y_action_set sb_a11y_actions(const fdk_widget *w) {
    (void)w;
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static bool sb_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                            double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    sb_toggle_popup(w);
    return true;
}

static const fdk_a11y_class sb_a11y = {
    .role = FDK_A11Y_ROLE_BUTTON,
    .describe = sb_a11y_describe,
    .actions = sb_a11y_actions,
    .perform = sb_a11y_perform,
};

const fdk_widget_class fdk_scale_button_class_def = {
    .size = sizeof(fdk_scale_button),
    .name = "scalebutton",
    .handle_event = sb_handle_event,
    .paint = sb_paint,
    .measure = sb_measure,
    .arrange = NULL,
    .instance_finalize = sb_destroy,
    .a11y = &sb_a11y,
};

/* ---- public API ---- */

fdk_result fdk_scale_button_create(fdk_widget *parent, fdk_font *font,
                                   double min, double max, double value,
                                   fdk_widget **out_button) {
    if (out_button == NULL || parent == NULL || font == NULL ||
        min >= max) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_scale_button_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_scale_button *b = sb_of(w);
    b->font = font;
    b->min = min;
    b->max = max;
    b->value = (value < min) ? min : (value > max) ? max : value;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_button = w;
    return FDK_OK;
}

void fdk_scale_button_set_range(fdk_widget *button, double min,
                                double max) {
    if (button == NULL || button->klass != &fdk_scale_button_class_def ||
        min >= max) {
        return; /* a bad range is a documented no-op */
    }
    fdk_scale_button *b = sb_of(button);
    b->min = min;
    b->max = max;
    if (b->value < min) {
        b->value = min;
    }
    if (b->value > max) {
        b->value = max;
    }
    fdk_widget_invalidate(button);
    fdk__a11y_notify(button, FDK_A11Y_VALUE_CHANGED, 0);
}

void fdk_scale_button_set_value(fdk_widget *button, double value) {
    if (button == NULL || button->klass != &fdk_scale_button_class_def) {
        return;
    }
    fdk_scale_button *b = sb_of(button);
    if (value < b->min) {
        value = b->min;
    }
    if (value > b->max) {
        value = b->max;
    }
    if (value == b->value) {
        return; /* idempotent */
    }
    b->value = value;
    fdk_widget_invalidate(button);
    fdk__a11y_notify(button, FDK_A11Y_VALUE_CHANGED, 0);
}

double fdk_scale_button_get_value(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_scale_button_class_def) {
        return 0.0;
    }
    return sb_of(button)->value;
}

void fdk_scale_button_set_step(fdk_widget *button, double step) {
    if (button == NULL || button->klass != &fdk_scale_button_class_def) {
        return;
    }
    sb_of(button)->step = (step > 0.0) ? step : 0.0;
}

void fdk_scale_button_set_on_value_changed(
    fdk_widget *button, fdk_scale_button_changed_fn on_changed,
    void *user_data) {
    if (button == NULL || button->klass != &fdk_scale_button_class_def) {
        return;
    }
    fdk_scale_button *b = sb_of(button);
    b->on_change = on_changed;
    b->on_change_data = user_data;
}
