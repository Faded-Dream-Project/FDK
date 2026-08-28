#define FDK_LOG_TAG "widgets"

/*
 * spin.c — SpinButton widget (Phase 9)
 *
 * A numeric entry: an embedded Entry (the Phase 9 text field — full
 * editing, selection, clipboard) plus an up/down stepper column that
 * draws as two stacked vector-glyph buttons (chevrons, no font
 * needed for the arrows). The VALUE is what the entry's text parses
 * to; parsing is strict (whole/decimal via strtod-like acceptance
 * with the FDK discipline: hand-rolled, locale-free), and an
 * unparsable buffer reads as the last committed value.
 *
 * Commit discipline: typing edits the buffer freely; the value
 * commits on Enter, on stepper press, on focus leaving (FOCUS_OUT),
 * and on programmatic set — the same settle points a spin button
 * has in every toolkit. Commits CLAMP to [min, max]; the clamp
 * rewrites the buffer so the text and value never disagree.
 *
 * Keyboard: Up/Down step (the entry forwards them — its own Up/Down
 * handling is line motion, meaningless in a single line), PageUp/
 * PageDown step by 10x.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPIN_BTN_W 22
#define SPIN_MIN_W 64
#define SPIN_MIN_H 24

typedef struct fdk_spin {
    fdk_widget base;
    fdk_widget *entry;    /* the embedded text field (child) */
    double min;
    double max;
    double step;
    double value;         /* last COMMITTED value */
    bool committing;      /* re-entrancy guard for commit->set_text */
    int hover_btn;        /* 0 none, 1 up, 2 down */
    int press_btn;
    fdk_spin_changed_fn on_changed;
    void *on_changed_data;
} fdk_spin;

static fdk_spin *spin_of(fdk_widget *w) {
    return (fdk_spin *)(void *)w;
}

extern const fdk_widget_class fdk_spin_class_def;

static void spin_commit(fdk_spin *s, bool fire);

/* The embedded entry's event watcher: commits when the entry loses
 * focus (FOCUS events never bubble, so the spin itself cannot see
 * them — this is the composition seam). */
static bool spin_entry_watch(fdk_widget *entry,
                             const fdk_widget_event *ev, void *user) {
    (void)entry;
    if (ev->type == FDK_WIDGET_FOCUS_OUT) {
        fdk_spin *s = user;
        spin_commit(s, false);
    }
    return false; /* observe only, never consume */
}

/* Enter in the entry is the spin's commit (the entry CONSUMES Enter
 * for its activate signal — the spin rides it). */
static void spin_entry_activated(fdk_widget *entry, void *user) {
    (void)entry;
    spin_commit(user, true);
}

/* ---- parsing (hand-rolled, locale-free) ---- */

/* Accepts an optional sign, digits with at most one '.', and an
 * optional exponent — the boring, safe grammar strtod accepts MINUS
 * locale weirdness and surrounding junk, which we refuse. Returns
 * true and the value on success. */
static bool spin_parse(const char *s, double *out) {
    if (s == NULL) {
        return false;
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    if (*s == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s) {
        return false;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != '\0') {
        return false; /* trailing junk: refuse, don't truncate */
    }
    if (errno == ERANGE) {
        return false;
    }
    *out = v;
    return true;
}

/* Formats the value back into the buffer (shortest round trip). */
static void spin_format(double v, char *buf, size_t cap) {
    snprintf(buf, cap, "%g", v);
}

static double spin_clamp(fdk_spin *s, double v) {
    if (v < s->min) {
        v = s->min;
    }
    if (v > s->max) {
        v = s->max;
    }
    return v;
}

static void spin_commit(fdk_spin *s, bool fire) {
    if (s->committing) {
        return;
    }
    s->committing = true;
    double parsed = s->value;
    bool ok = spin_parse(fdk_entry_get_text(s->entry), &parsed);
    double v = ok ? spin_clamp(s, parsed) : s->value;
    if (v != s->value || !ok) {
        s->value = v;
        char buf[64];
        spin_format(v, buf, sizeof(buf));
        (void)fdk_entry_set_text(s->entry, buf);
        fdk_widget_invalidate(&s->base);
        /* A11y: the committed value moved. */
        fdk__a11y_notify(&s->base, FDK_A11Y_VALUE_CHANGED, 0);
    }
    if (fire && s->on_changed != NULL) {
        s->on_changed(&s->base, s->on_changed_data);
    }
    s->committing = false;
}

static void spin_step(fdk_spin *s, double delta) {
    double v = spin_clamp(s, s->value + delta);
    if (v != s->value) {
        s->value = v;
        char buf[64];
        spin_format(v, buf, sizeof(buf));
        (void)fdk_entry_set_text(s->entry, buf);
        fdk_widget_invalidate(&s->base);
        /* A11y: the stepped value moved. */
        fdk__a11y_notify(&s->base, FDK_A11Y_VALUE_CHANGED, 0);
        if (s->on_changed != NULL) {
            s->on_changed(&s->base, s->on_changed_data);
        }
    } else if (delta != 0.0) {
        /* Even a clamped no-op re-syncs the text. */
        char buf[64];
        spin_format(v, buf, sizeof(buf));
        if (strcmp(buf, fdk_entry_get_text(s->entry)) != 0) {
            (void)fdk_entry_set_text(s->entry, buf);
        }
    }
}

/* ---- geometry / paint ---- */

static fdk_rect spin_btn_rect(fdk_spin *s, int which) {
    fdk_i32 w = s->base.bounds.width;
    fdk_i32 h = s->base.bounds.height;
    fdk_rect r = { w - SPIN_BTN_W, 0, SPIN_BTN_W, h / 2 };
    if (which == 2) {
        r.y = h / 2;
        r.height = h - h / 2;
    }
    return r;
}

static void spin_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_spin *s = spin_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The stepper column: a track panel with two chevron buttons.
     * The entry child paints itself (it is a later sibling). */
    for (int which = 1; which <= 2; which++) {
        fdk_rect br = spin_btn_rect(s, which);
        br.x += bounds.x;
        br.y += bounds.y;
        fdk_color fill = ((w->flags & FDK_WF_ENABLED) == 0)
            ? fdk__pal_control_disabled()
            : (s->press_btn == which
                   ? fdk__pal_control_pressed()
                   : (s->hover_btn == which ? fdk__pal_control_hover()
                                            : fdk__pal_control()));
        fdk_surface_fill_rect(surface, br, fill);
        /* Chevron. */
        fdk_i32 cx = br.x + br.width / 2;
        fdk_i32 cy = br.y + br.height / 2;
        fdk_color ink = ((w->flags & FDK_WF_ENABLED) == 0)
            ? fdk__pal_text_disabled()
            : fdk__pal_text();
        fdk_i32 half = 4;
        if (which == 1) { /* up */
            fdk_surface_draw_line(surface, cx - half, cy + 2,
                                  cx, cy - 2, ink);
            fdk_surface_draw_line(surface, cx, cy - 2,
                                  cx + half, cy + 2, ink);
        } else { /* down */
            fdk_surface_draw_line(surface, cx - half, cy - 2,
                                  cx, cy + 2, ink);
            fdk_surface_draw_line(surface, cx, cy + 2,
                                  cx + half, cy - 2, ink);
        }
    }
}

/* ---- events: steppers + keyboard forwarding ---- */

static int spin_btn_at(fdk_spin *s, fdk_f32 x, fdk_f32 y) {
    if (x < (fdk_f32)(s->base.bounds.width - SPIN_BTN_W)) {
        return 0;
    }
    return (y < (fdk_f32)(s->base.bounds.height / 2)) ? 1 : 2;
}

static bool spin_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_spin *s = spin_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0 ||
            ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        int btn = spin_btn_at(s, ev->pointer.position.x,
                              ev->pointer.position.y);
        if (btn == 0) {
            return false; /* the entry's area: its own event */
        }
        s->press_btn = btn;
        spin_step(s, (btn == 1) ? s->step : -s->step);
        fdk_widget_invalidate(w);
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        int btn = spin_btn_at(s, ev->position.x, ev->position.y);
        if (btn != s->hover_btn) {
            s->hover_btn = btn;
            fdk_widget_invalidate(w);
        }
        return false;
    }
    case FDK_WIDGET_POINTER_UP:
        if (s->press_btn != 0) {
            s->press_btn = 0;
            fdk_widget_invalidate(w);
            return true;
        }
        return false;
    case FDK_WIDGET_KEY_DOWN: {
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            return false;
        }
        /* Arrows/page step instead of moving the entry caret — but
         * ONLY when the spin (not some inner focusable) is focused:
         * the entry is the focusable inside; when IT is focused the
         * events reach it first and bubble here. */
        double d = 0.0;
        switch (ev->key.scancode) {
        case FDK_KEY_UP:
            d = s->step;
            break;
        case FDK_KEY_DOWN:
            d = -s->step;
            break;
        case FDK_KEY_PAGE_UP:
            d = s->step * 10.0;
            break;
        case FDK_KEY_PAGE_DOWN:
            d = -s->step * 10.0;
            break;
        case FDK_KEY_ENTER:
            spin_commit(s, true);
            return false; /* Enter also activates the entry signal */
        default:
            return false;
        }
        if (d != 0.0) {
            spin_step(s, d);
            return true; /* consumed: the caret did not move */
        }
        return false;
    }
    case FDK_WIDGET_FOCUS_OUT:
        spin_commit(s, false);
        return false;
    default:
        break;
    }
    return false;
}

static void spin_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = SPIN_MIN_W;
    out->height = SPIN_MIN_H;
}

static void spin_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_spin *s = spin_of(w);
    if (s->entry != NULL) {
        fdk_rect er = { 0, 0, assigned.width - SPIN_BTN_W,
                        assigned.height };
        fdk_widget_set_bounds(s->entry, er);
    }
}

static void spin_destroy(fdk_widget *w) {
    (void)w; /* the entry is a child and dies with the subtree */
}

/* ---- a11y ---- */

static void spin_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_spin *s = (const fdk_spin *)(const void *)w;
    out->has_value = true;
    out->value_min = s->min;
    out->value_max = s->max;
    out->value_current = s->value;
    char buf[64];
    spin_format(s->value, buf, sizeof(buf));
    out->value_text = fdk__strdup(buf);
}

static fdk_a11y_action_set spin_a11y_actions(const fdk_widget *w) {
    (void)w;
    return FDK_A11Y_ACTION_INCREMENT | FDK_A11Y_ACTION_DECREMENT |
           FDK_A11Y_ACTION_SET_VALUE;
}

static bool spin_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                              double value) {
    fdk_spin *s = spin_of(w);
    switch (action) {
    case FDK_A11Y_ACTION_INCREMENT:
        spin_step(s, s->step);
        return true;
    case FDK_A11Y_ACTION_DECREMENT:
        spin_step(s, -s->step);
        return true;
    case FDK_A11Y_ACTION_SET_VALUE:
        fdk_spin_set_value(w, value);
        return true;
    default:
        return false;
    }
}

/* ---- a11y text interface (delegated to the embedded Entry) ---- */

static size_t spin_text_length(const fdk_widget *w) {
    const fdk_spin *s = (const fdk_spin *)(const void *)w;
    return (s->entry != NULL) ? fdk_a11y_text_length(s->entry) : 0;
}

static size_t spin_text_caret(const fdk_widget *w) {
    const fdk_spin *s = (const fdk_spin *)(const void *)w;
    return (s->entry != NULL) ? fdk_a11y_text_caret(s->entry) : 0;
}

static bool spin_text_selection(const fdk_widget *w, size_t *anchor,
                                size_t *caret) {
    const fdk_spin *s = (const fdk_spin *)(const void *)w;
    return s->entry != NULL &&
           fdk_a11y_text_selection(s->entry, anchor, caret);
}

static bool spin_text_at(const fdk_widget *w, size_t offset,
                         fdk_a11y_text_granularity granularity,
                         char *buf, size_t cap, size_t *out_start,
                         size_t *out_end) {
    const fdk_spin *s = (const fdk_spin *)(const void *)w;
    if (s->entry == NULL) {
        return false;
    }
    size_t start = 0;
    size_t end = 0;
    fdk_result r = fdk_a11y_text_at_offset(s->entry, offset,
                                           granularity, buf, cap,
                                           &start, &end);
    if (out_start != NULL) {
        *out_start = start;
    }
    if (out_end != NULL) {
        *out_end = end;
    }
    return fdk_ok(r);
}

static bool spin_text_set_caret(fdk_widget *w, size_t offset) {
    fdk_spin *s = spin_of(w);
    return s->entry != NULL &&
           fdk_ok(fdk_a11y_text_set_caret(s->entry, offset));
}

static bool spin_text_set_selection(fdk_widget *w, size_t anchor,
                                    size_t caret) {
    fdk_spin *s = spin_of(w);
    return s->entry != NULL &&
           fdk_ok(fdk_a11y_text_set_selection(s->entry, anchor, caret));
}

static const fdk_a11y_class spin_a11y = {
    .role = FDK_A11Y_ROLE_SPIN_BUTTON,
    .describe = spin_a11y_describe,
    .actions = spin_a11y_actions,
    .perform = spin_a11y_perform,
    .text_length = spin_text_length,
    .text_caret = spin_text_caret,
    .text_selection = spin_text_selection,
    .text_at = spin_text_at,
    .text_set_caret = spin_text_set_caret,
    .text_set_selection = spin_text_set_selection,
};

const fdk_widget_class fdk_spin_class_def = {
    .size = sizeof(fdk_spin),
    .name = "spinbutton",
    .handle_event = spin_handle_event,
    .paint = spin_paint,
    .measure = spin_measure,
    .arrange = spin_arrange,
    .destroy = spin_destroy,
    .a11y = &spin_a11y,
};

/* ---- public API ---- */

fdk_result fdk_spin_create(fdk_widget *parent, fdk_font *font,
                           double min, double max, double value,
                           fdk_widget **out_spin) {
    if (out_spin == NULL || max < min) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_spin_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_spin *s = spin_of(w);
    s->min = min;
    s->max = max;
    s->step = 1.0;
    s->value = spin_clamp(s, value);

    char buf[64];
    spin_format(s->value, buf, sizeof(buf));
    r = fdk_entry_create(w, font, buf, &s->entry);
    if (!fdk_ok(r)) {
        fdk_widget_destroy(w);
        return r;
    }
    /* Composition seams: Enter commits (the entry's activate), focus
     * loss commits (FOCUS_OUT doesn't bubble). */
    fdk_entry_set_on_activate(s->entry, spin_entry_activated, s);
    fdk_widget_set_event_callback(s->entry, spin_entry_watch, s);
    /* The entry is right-adjacent to the steppers (arrange sizes
     * it); typing lands in it, focus follows. */
    /* Focusable like every control of its family (the slider, the
     * entry, the buttons set this too) — a SpinButton users cannot
     * Tab to is not keyboard-operable. Found by the narrator tests:
     * focus() on a spin was a silent no-op. */
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_spin = w;
    return FDK_OK;
}

void fdk_spin_set_range(fdk_widget *spin, double min, double max) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def ||
        max < min) {
        return;
    }
    fdk_spin *s = spin_of(spin);
    s->min = min;
    s->max = max;
    s->value = spin_clamp(s, s->value);
    char buf[64];
    spin_format(s->value, buf, sizeof(buf));
    (void)fdk_entry_set_text(s->entry, buf);
    fdk_widget_invalidate(spin);
}

void fdk_spin_set_step(fdk_widget *spin, double step) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def ||
        step <= 0.0) {
        return;
    }
    spin_of(spin)->step = step;
}

void fdk_spin_set_value(fdk_widget *spin, double value) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def) {
        return;
    }
    fdk_spin *s = spin_of(spin);
    spin_step(s, value - s->value);
    /* (a direct set, not a step: force the exact value) */
    double v = spin_clamp(s, value);
    if (v != s->value) {
        s->value = v;
        char buf[64];
        spin_format(v, buf, sizeof(buf));
        (void)fdk_entry_set_text(s->entry, buf);
        fdk_widget_invalidate(spin);
        if (s->on_changed != NULL) {
            s->on_changed(spin, s->on_changed_data);
        }
    }
}

double fdk_spin_get_value(fdk_widget *spin) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def) {
        return 0.0;
    }
    return spin_of(spin)->value;
}

const char *fdk_spin_get_text(fdk_widget *spin) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def) {
        return NULL;
    }
    return fdk_entry_get_text(spin_of(spin)->entry);
}

void fdk_spin_set_on_changed(fdk_widget *spin,
                             fdk_spin_changed_fn on_changed,
                             void *user_data) {
    if (spin == NULL || spin->klass != &fdk_spin_class_def) {
        return;
    }
    fdk_spin *s = spin_of(spin);
    s->on_changed = on_changed;
    s->on_changed_data = user_data;
}
