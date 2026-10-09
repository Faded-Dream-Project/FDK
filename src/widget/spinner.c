/*
 * spinner.c — the busy indicator (1.4.1).
 *
 * GTK's GtkSpinner / Qt's busy QProgressBar, drawn as FDK can draw
 * it with the primitive set: a rotating arc synthesized from
 * antialiased line segments along a circle, with an alpha ramp
 * fading toward the tail — the classic "comet" activity indicator.
 * The head leads at full accent, the tail dissolves to nothing.
 *
 * The rotation rides the TIMER queue exactly like the indeterminate
 * ProgressBar's sweep (same tempo discipline, same detached-tree
 * honesty: no context, no clock, the arc parks at its phase). The
 * phase is kept on stop — restart continues the rotation rather
 * than resetting it (GTK semantics; a spinner that jumps back to
 * 12 o'clock on every restart looks like a glitch, not a choice).
 *
 * Indicator, not control: no focus, no events, the a11y face is
 * the BUSY state and a "busy"/"idle" value text.
 */

#define FDK_LOG_TAG "widgets"

#include "widgets_internal.h"
#include "../theme/theme_internal.h"

#include "core/alloc_internal.h"
#include "../window/window_internal.h" /* timer lifecycle via context */
#include <math.h>
#include <stdio.h>

/* Full turn in radians. Defined locally: the build's strict
 * _POSIX_C_SOURCE hides XSI's M_PI, and no other file uses it
 * (slider.c/spin.c write their own trig constants the same way). */
#define SPIN_TAU 6.28318530717958647692

/* Natural size (square; the arc scales with the bounds). */
#define SPINNER_SIZE 24

/* Rotation tempo: one tick per timer fire, 10 degrees per tick at
 * 40 ms = 360 degrees in 1.44 s — reads as brisk but calm, the
 * same ballpark as the progress sweep's ~1.9 s traversal. */
#define SPINNER_TICK_MS 40
#define SPINNER_STEP_DEG 10.0f

/* The arc's geometry: spans ~300 degrees of the ring (an open
 * comet, not a full circle), stroke inset so the whole ring sits
 * inside the bounds with a pixel of air. */
#define SPINNER_ARC_DEG 300.0f
#define SPINNER_INSET 3.0f

static void spinner_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = SPINNER_SIZE;
    out->height = SPINNER_SIZE;
}

static void spinner_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_spinner *sp = user;
    sp->phase += SPINNER_STEP_DEG * (float)(SPIN_TAU / 360.0);
    if (sp->phase >= (float)SPIN_TAU) {
        sp->phase -= (float)SPIN_TAU;
    }
    fdk_widget_invalidate(&sp->base);
}

/* Paint reads the phase through a helper so the phase's owner is
 * unambiguous (spinner_of on the paint-path widget). */
static fdk_f32 sp_phase(fdk_widget *w) {
    return spinner_of(w)->phase;
}

static void spinner_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    if (bounds.width <= SPINNER_INSET * 2 ||
        bounds.height <= SPINNER_INSET * 2) {
        return; /* too small to draw an honest ring */
    }
    const bool disabled = (w->flags & FDK_WF_ENABLED) == 0;

    /* The ring: centered, radius scaled to the smaller axis. The
     * stroke IS the ring (a 1-px line); at 24 px natural that reads
     * crisp, and scaled-up spiners read as thin arcs — consistent
     * with how FDK draws circle outlines everywhere else. */
    fdk_f32 cx = (fdk_f32)bounds.x + (fdk_f32)bounds.width * 0.5f;
    fdk_f32 cy = (fdk_f32)bounds.y + (fdk_f32)bounds.height * 0.5f;
    fdk_f32 radius = (fdk_f32)bounds.width;
    if ((fdk_f32)bounds.height < radius) {
        radius = (fdk_f32)bounds.height;
    }
    radius = radius * 0.5f - SPINNER_INSET;

    fdk_color head = disabled ? fdk__pal_control_disabled() : fdk__pal_accent();

    /* The comet: SEGS segments tracing ARC_DEG behind the head,
     * alpha ramping from full at the head to ~0 at the tail. Fewer
     * segments than the ring's pixel circumference is fine — the AA
     * lines interpolate the chords; more is waste. */
    enum { SEGS = 24 };
    const fdk_f32 arc = SPINNER_ARC_DEG * (float)(SPIN_TAU / 360.0);
    const fdk_f32 seg = arc / (fdk_f32)SEGS;
    const fdk_f32 phase = sp_phase(w);
    for (int i = 0; i < SEGS; i++) {
        /* Segment i covers [head - (i+1)*seg, head - i*seg] — the
         * ramp runs tail->head so the FIRST segment drawn is the
         * tail (alpha ~0, invisible) and the LAST is the head. */
        fdk_f32 a0 = phase - (arc - (fdk_f32)i * seg);
        fdk_f32 a1 = a0 + seg;
        fdk_f32 t = (fdk_f32)(i + 1) / (fdk_f32)SEGS; /* 0 tail, 1 head */
        /* Ease the ramp so the bright half of the comet dominates:
         * alpha = t^1.5 puts two thirds of the ink in the last
         * third of the arc. */
        fdk_f32 alpha = t * sqrtf(t);
        fdk_color c = {head.r, head.g, head.b, head.a * alpha};
        fdk_i32 x0 = (fdk_i32)(cx + cosf(a0) * radius + 0.5f);
        fdk_i32 y0 = (fdk_i32)(cy + sinf(a0) * radius + 0.5f);
        fdk_i32 x1 = (fdk_i32)(cx + cosf(a1) * radius + 0.5f);
        fdk_i32 y1 = (fdk_i32)(cy + sinf(a1) * radius + 0.5f);
        fdk_surface_draw_line_aa(surface, x0, y0, x1, y1, c);
    }
}

/* ---- a11y ---- */

static void spinner_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_spinner *sp = (const fdk_spinner *)(const void *)w;
    if (sp->spinning) {
        out->states |= FDK_A11Y_BUSY;
        out->has_value = false;
        out->value_text = fdk__strdup("busy");
    } else {
        out->has_value = false;
        out->value_text = fdk__strdup("idle");
    }
}

static const fdk_a11y_class spinner_a11y = {
    .role = FDK_A11Y_ROLE_SPINNER,
    .describe = spinner_a11y_describe,
    .actions = NULL, /* an indicator, not a control */
    .perform = NULL,
};

const fdk_widget_class fdk_spinner_class_def = {
    .size = sizeof(fdk_spinner),
    .name = "spinner",
    .handle_event = NULL,
    .paint = spinner_paint,
    .measure = spinner_measure,
    .arrange = NULL,
    .destroy = NULL,
    .a11y = &spinner_a11y,
    .max_children = 0,
};

fdk_result fdk_spinner_create(fdk_widget *parent,
                              fdk_widget **out_spinner) {
    if (out_spinner == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_spinner_class_def,
                                     (fdk_rect){0, 0, SPINNER_SIZE,
                                                SPINNER_SIZE},
                                     &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_spinner *sp = spinner_of(w);
    sp->spinning = false;
    sp->tick_timer = NULL;
    sp->phase = 0.0f;
    *out_spinner = w;
    return FDK_OK;
}

void fdk_spinner_start(fdk_widget *spinner) {
    if (spinner == NULL || spinner->klass != &fdk_spinner_class_def) {
        return;
    }
    fdk_spinner *sp = spinner_of(spinner);
    if (sp->spinning) {
        return; /* idempotent */
    }
    sp->spinning = true;
    /* Detached trees have no context — the arc parks at its phase
     * (the progress pulse's honesty rule, verbatim). */
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(spinner));
    if (ctx != NULL && sp->tick_timer == NULL) {
        sp->tick_timer = fdk_timer_add(ctx, SPINNER_TICK_MS, true,
                                       spinner_tick, sp);
    }
    /* A11y: the busy state turns on. */
    fdk__a11y_notify(spinner, FDK_A11Y_STATE_CHANGED, FDK_A11Y_BUSY);
    fdk_widget_invalidate(spinner);
}

void fdk_spinner_stop(fdk_widget *spinner) {
    if (spinner == NULL || spinner->klass != &fdk_spinner_class_def) {
        return;
    }
    fdk_spinner *sp = spinner_of(spinner);
    if (!sp->spinning) {
        return; /* idempotent */
    }
    sp->spinning = false;
    if (sp->tick_timer != NULL) {
        fdk_timer_remove(sp->tick_timer);
        sp->tick_timer = NULL;
    }
    /* The phase is NOT reset: restart continues from the parked
     * angle (GTK semantics — documented in fdk_widgets.h). */
    fdk__a11y_notify(spinner, FDK_A11Y_STATE_CHANGED, FDK_A11Y_BUSY);
    fdk_widget_invalidate(spinner);
}

bool fdk_spinner_is_spinning(fdk_widget *spinner) {
    if (spinner == NULL || spinner->klass != &fdk_spinner_class_def) {
        return false;
    }
    return spinner_of(spinner)->spinning;
}
