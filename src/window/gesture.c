/*
 * gesture.c — the gesture recognizers (§23/B2b; include/fdk/
 * fdk_gesture.h).
 *
 * A pure event-fed state machine per recognizer kind, GTK4's
 * GtkGesture model in C: press opens a sequence, motion evaluates
 * the rules, release/timeout fires the callbacks. The touch
 * ENGINE (touch.c) stays the tap/scroll/pinch synthesizer it is —
 * this layer is what APPLICATION code attaches (the recognizer
 * side FDK lacked: long-press, multi-click counting, drag/pan
 * with begin/update/end, swipe velocity, zoom, rotate).
 *
 * Clock discipline: the recognizer's time is indirected exactly
 * like the touch engine's (tests drive a controllable clock; no
 * test sleeps — the M-3 lesson).
 *
 * Pointer AND touch: POINTER_BUTTON_DOWN/UP/MOTION feed the
 * single-point recognizers (click/long-press/drag/pan/swipe — a
 * mouse drag and a finger drag are the same gesture); TOUCH_*
 * slots feed zoom/rotate (two-point). This is GTK's split too
 * (GestureSingle rides pointer-equivalent sequences; Zoom/Rotate
 * ride the touchpad/touch contact pairs).
 */

#define FDK_LOG_TAG "window"

#include "fdk/fdk_gesture.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <math.h>
#include <string.h>
#include <time.h>

/* ---- defaults (GTK4's; see the header's sources) ----------------------- */

#define G_DEFAULT_THRESHOLD_PX 5.0f
#define G_DEFAULT_MULTI_CLICK_MS 400
#define G_DEFAULT_LONG_PRESS_MS 500
#define G_DEFAULT_SWIPE_VELOCITY 300.0f

/* ---- the clock ---------------------------------------------------------- */

static long long (*g_gesture_clock)(void *user);
static void *g_gesture_clock_user;

static long long gesture_now_ms(void) {
    if (g_gesture_clock != NULL) {
        return g_gesture_clock(g_gesture_clock_user);
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void fdk_gesture_set_clock(long long (*now_ms)(void *user), void *user) {
    g_gesture_clock = now_ms;
    g_gesture_clock_user = user;
}

/* ---- the recognizer ------------------------------------------------------ */

typedef struct {
    fdk_pointf pos;
    long long ms;
} g_point;

struct fdk_gesture {
    fdk_gesture_kind kind;

    /* configuration */
    fdk_f32 threshold_px;
    long long multi_click_ms;
    long long long_press_ms;
    fdk_f32 delay_factor;
    int pan_axis; /* 0 both, 'h', 'v' */

    /* callbacks */
    fdk_gesture_click_fn on_click;
    void *click_user;
    fdk_gesture_long_press_fn on_long_press;
    void *long_press_user;
    fdk_gesture_drag_fn on_drag;
    void *drag_user;
    fdk_gesture_swipe_fn on_swipe;
    void *swipe_user;
    fdk_gesture_scale_fn on_scale;
    void *scale_user;

    /* sequence state (single-point kinds) */
    bool pressed;         /* a press is being tracked            */
    int press_button;     /* which pointer button                */
    g_point press;        /* where/when it went down             */
    g_point last;         /* the most recent motion              */
    bool claimed;         /* this recognizer claimed the seq     */
    bool denied;          /* another recognizer claimed it       */
    bool long_press_fired;
    bool drag_begun;

    /* multi-click (click kind) */
    int click_count;      /* consecutive clicks so far           */
    long long last_click_ms;

    /* two-point state (zoom/rotate) */
    bool two_active;
    bool a_used, b_used;  /* first/second contact seen           */
    g_point a0, b0;       /* the posture's start                 */
    g_point a, b;         /* current                             */
    fdk_f32 base_value;   /* 1.0 zoom / 0.0 rotate at start      */
    fdk_f32 last_value;
    /* per-slot ids */
    fdk_u32 id_a, id_b;

    /* swipe bookkeeping */
    g_point release;
};

static fdk_f32 dist(fdk_pointf a, fdk_pointf b) {
    fdk_f32 dx = a.x - b.x;
    fdk_f32 dy = a.y - b.y;
    return sqrtf(dx * dx + dy * dy);
}

fdk_gesture *fdk_gesture_create(fdk_gesture_kind kind) {
    fdk_gesture *g = fdk_alloc(sizeof *g);
    if (g == NULL) {
        return NULL;
    }
    memset(g, 0, sizeof *g);
    g->kind = kind;
    g->threshold_px = G_DEFAULT_THRESHOLD_PX;
    g->multi_click_ms = G_DEFAULT_MULTI_CLICK_MS;
    g->long_press_ms = G_DEFAULT_LONG_PRESS_MS;
    g->delay_factor = 1.0f;
    return g;
}

void fdk_gesture_destroy(fdk_gesture *g) {
    fdk_free(g);
}

void fdk_gesture_set_threshold_px(fdk_gesture *g, fdk_f32 px) {
    if (g != NULL && px >= 0.0f) {
        g->threshold_px = px;
    }
}
void fdk_gesture_set_multi_click_ms(fdk_gesture *g, long long ms) {
    if (g != NULL && ms > 0) {
        g->multi_click_ms = ms;
    }
}
void fdk_gesture_set_long_press_ms(fdk_gesture *g, long long ms) {
    if (g != NULL && ms > 0) {
        g->long_press_ms = ms;
    }
}
void fdk_gesture_set_delay_factor(fdk_gesture *g, fdk_f32 factor) {
    if (g != NULL && factor > 0.0f) {
        g->delay_factor = factor;
    }
}
void fdk_gesture_set_orientation(fdk_gesture *g,
                                 fdk_gesture_direction axis) {
    if (g == NULL) {
        return;
    }
    g->pan_axis = (axis == FDK_GESTURE_DIR_LEFT ||
                   axis == FDK_GESTURE_DIR_RIGHT)
                      ? 'h'
                      : (axis == FDK_GESTURE_DIR_UP ||
                         axis == FDK_GESTURE_DIR_DOWN)
                            ? 'v'
                            : 0;
}

void fdk_gesture_on_click(fdk_gesture *g, fdk_gesture_click_fn fn,
                          void *user) {
    if (g != NULL) {
        g->on_click = fn;
        g->click_user = user;
    }
}
void fdk_gesture_on_long_press(fdk_gesture *g,
                               fdk_gesture_long_press_fn fn,
                               void *user) {
    if (g != NULL) {
        g->on_long_press = fn;
        g->long_press_user = user;
    }
}
void fdk_gesture_on_drag(fdk_gesture *g, fdk_gesture_drag_fn fn,
                         void *user) {
    if (g != NULL) {
        g->on_drag = fn;
        g->drag_user = user;
    }
}
void fdk_gesture_on_swipe(fdk_gesture *g, fdk_gesture_swipe_fn fn,
                          void *user) {
    if (g != NULL) {
        g->on_swipe = fn;
        g->swipe_user = user;
    }
}
void fdk_gesture_on_scale(fdk_gesture *g, fdk_gesture_scale_fn fn,
                          void *user) {
    if (g != NULL) {
        g->on_scale = fn;
        g->scale_user = user;
    }
}

void fdk_gesture_claim(fdk_gesture *g) {
    if (g != NULL && g->pressed) {
        g->claimed = true;
    }
}

void fdk_gesture_reset(fdk_gesture *g) {
    if (g == NULL) {
        return;
    }
    /* The multi-click count survives a reset ONLY when the reset
     * came from unmap (no press in flight); simplest honest rule:
     * a reset also forgets the click chain. GTK's reset does the
     * same. */
    g->pressed = false;
    g->claimed = false;
    g->denied = false;
    g->long_press_fired = false;
    g->drag_begun = false;
    g->two_active = false;
    g->click_count = 0;
}

/* ---- the single-point evaluation ---------------------------------------- */

static void gesture_press(fdk_gesture *g, fdk_pointf where,
                          fdk_u32 button) {
    long long now = gesture_now_ms();
    g->pressed = true;
    g->press_button = (int)button;
    g->press.pos = where;
    g->press.ms = now;
    g->last = g->press;
    g->long_press_fired = false;
    g->drag_begun = false;
    g->claimed = false;
    g->denied = false;
}

/* Long-press: fired when the hold elapses with no release/motion.
 * Evaluated from MOTION (a ticking heartbeat is not FDK's model —
 * the recognizer has no timer thread; a stationary press gets its
 * verdict at the NEXT event, which for a real hold is the release
 * — so the verdict also runs there. The dedicated long-press test
 * drives feed(TOUCH/POINTER MOTION at the same point) to observe
 * the fire at the timeout, the same trick tests/test_touch.c uses
 * for the tap timeout.) */
static void evaluate_long_press(fdk_gesture *g, long long now) {
    if (g->kind != FDK_GESTURE_LONG_PRESS || !g->pressed ||
        g->long_press_fired || g->denied) {
        return;
    }
    long long hold =
        (long long)((fdk_f32)g->long_press_ms * g->delay_factor);
    if (now - g->press.ms >= hold &&
        dist(g->last.pos, g->press.pos) <= g->threshold_px) {
        g->long_press_fired = true;
        g->claimed = true;
        if (g->on_long_press != NULL) {
            g->on_long_press(g, g->press.pos, true,
                             g->long_press_user);
        }
    }
}

static void gesture_motion(fdk_gesture *g, fdk_pointf where) {
    if (!g->pressed || g->denied) {
        return;
    }
    long long now = gesture_now_ms();
    fdk_pointf offset = {where.x - g->press.pos.x,
                         where.y - g->press.pos.y};
    fdk_f32 travel = dist(where, g->press.pos);

    evaluate_long_press(g, now);

    switch (g->kind) {
    case FDK_GESTURE_DRAG:
    case FDK_GESTURE_PAN: {
        if (!g->drag_begun && travel < g->threshold_px) {
            break; /* below the activation slop */
        }
        if (!g->drag_begun) {
            /* PAN's orientation latch: the axis locks at begin. */
            if (g->kind == FDK_GESTURE_PAN && g->pan_axis != 0) {
                fdk_f32 ax = fabsf(offset.x);
                fdk_f32 ay = fabsf(offset.y);
                if ((g->pan_axis == 'h' && ay > ax * 1.5f) ||
                    (g->pan_axis == 'v' && ax > ay * 1.5f)) {
                    /* dominant on the wrong axis: deny self, let
                     * others run (the vertical pan over a
                     * horizontal scroller). */
                    g->denied = true;
                    break;
                }
            }
            g->drag_begun = true;
            g->claimed = true;
        }
        if (g->on_drag != NULL) {
            g->on_drag(g, offset, where, g->drag_user);
        }
        break;
    }
    default:
        break;
    }
    g->last.pos = where;
    g->last.ms = now;
}

static void gesture_release(fdk_gesture *g, fdk_pointf where) {
    if (!g->pressed) {
        return;
    }
    long long now = gesture_now_ms();
    fdk_pointf offset = {where.x - g->press.pos.x,
                         where.y - g->press.pos.y};
    fdk_f32 travel = dist(where, g->press.pos);
    long long duration = now - g->press.ms;

    switch (g->kind) {
    case FDK_GESTURE_CLICK: {
        if (g->denied || travel > g->threshold_px) {
            break; /* moved: not a click */
        }
        /* Multi-click chain: within the window AND near the
         * previous click. */
        if (g->click_count > 0 &&
            (now - g->last_click_ms > g->multi_click_ms ||
             dist(where, g->press.pos) >
                 2 * g->threshold_px + 5.0f)) {
            g->click_count = 0;
        }
        g->click_count++;
        if (g->click_count > 3) {
            g->click_count = 1; /* quadruple = a fresh single */
        }
        g->last_click_ms = now;
        if (g->on_click != NULL) {
            g->on_click(g, g->click_count, where, g->click_user);
        }
        break;
    }
    case FDK_GESTURE_LONG_PRESS: {
        if (!g->long_press_fired && !g->denied &&
            g->on_long_press != NULL &&
            travel <= g->threshold_px) {
            /* Released before the hold: the CANCELLED callback
             * (GTK's contract: cancelled, not silence). */
            g->on_long_press(g, g->press.pos, false,
                             g->long_press_user);
        }
        break;
    }
    case FDK_GESTURE_DRAG:
    case FDK_GESTURE_PAN: {
        if (g->drag_begun && g->on_drag != NULL) {
            g->on_drag(g, offset, where, g->drag_user); /* the END
            * report: offset == the final total */
        }
        break;
    }
    case FDK_GESTURE_SWIPE: {
        if (g->denied || duration <= 0) {
            break;
        }
        fdk_f32 vx = (where.x - g->press.pos.x) * 1000.0f /
                     (fdk_f32)duration;
        fdk_f32 vy = (where.y - g->press.pos.y) * 1000.0f /
                     (fdk_f32)duration;
        fdk_f32 speed = sqrtf(vx * vx + vy * vy);
        if (speed < G_DEFAULT_SWIPE_VELOCITY) {
            break; /* a slow release is not a swipe */
        }
        fdk_gesture_direction dir;
        if (fabsf(vx) >= fabsf(vy)) {
            dir = vx > 0 ? FDK_GESTURE_DIR_RIGHT : FDK_GESTURE_DIR_LEFT;
        } else {
            dir = vy > 0 ? FDK_GESTURE_DIR_DOWN : FDK_GESTURE_DIR_UP;
        }
        if (g->on_swipe != NULL) {
            g->on_swipe(g, vx, vy, dir, g->swipe_user);
        }
        break;
    }
    default:
        break;
    }
    g->pressed = false;
    g->release.pos = where;
    g->release.ms = now;
}

/* ---- the two-point evaluation (zoom/rotate) ------------------------------ */

static void two_point_update(fdk_gesture *g) {
    fdk_f32 value;
    if (g->kind == FDK_GESTURE_ZOOM) {
        fdk_f32 d0 = dist(g->a0.pos, g->b0.pos);
        fdk_f32 d1 = dist(g->a.pos, g->b.pos);
        value = d0 > 0.01f ? d1 / d0 : 1.0f;
    } else { /* ROTATE */
        fdk_f32 a0 = atan2f(g->b0.pos.y - g->a0.pos.y,
                            g->b0.pos.x - g->a0.pos.x);
        fdk_f32 a1 = atan2f(g->b.pos.y - g->a.pos.y,
                            g->b.pos.x - g->a.pos.x);
        value = a1 - a0;
    }
    fdk_f32 delta = value - g->last_value;
    g->last_value = value;
    if (g->on_scale != NULL) {
        g->on_scale(g, value, delta, g->scale_user);
    }
}

static void touch_two_track(fdk_gesture *g, fdk_u32 id, fdk_pointf pos) {
    if (!g->two_active) {
        return;
    }
    if (id == g->id_a) {
        g->a.pos = pos;
    } else if (id == g->id_b) {
        g->b.pos = pos;
    } else {
        return;
    }
    two_point_update(g);
}

static void touch_down(fdk_gesture *g, fdk_u32 id, fdk_pointf pos) {
    if (!g->two_active) {
        if (!g->a_used) {
            g->id_a = id;
            g->a.pos = pos;
            g->a.ms = gesture_now_ms();
            g->a_used = true;
        } else if (!g->b_used && id != g->id_a) {
            g->id_b = id;
            g->b.pos = pos;
            g->b.ms = gesture_now_ms();
            g->b_used = true;
            /* The posture begins. */
            g->two_active = true;
            g->a0 = g->a;
            g->b0 = g->b;
            g->base_value = g->kind == FDK_GESTURE_ZOOM ? 1.0f : 0.0f;
            g->last_value = g->base_value;
        }
        return;
    }
    /* A third finger: the posture degrades (reset the pair). */
    if (id != g->id_a && id != g->id_b) {
        fdk_gesture_reset(g);
        g->a_used = false;
        g->b_used = false;
        g->id_a = 0;
        g->id_b = 0;
    }
}

static void touch_up(fdk_gesture *g, fdk_u32 id) {
    if (!g->two_active) {
        if (id == g->id_a) {
            g->id_a = 0;
            g->a_used = false;
        } else if (id == g->id_b) {
            g->id_b = 0;
            g->b_used = false;
        }
        return;
    }
    if (id == g->id_a || id == g->id_b) {
        /* Either finger lifting ends the posture. */
        g->two_active = false;
        g->a_used = false;
        g->b_used = false;
        g->id_a = 0;
        g->id_b = 0;
    }
}

/* ---- the feed ------------------------------------------------------------ */

bool fdk_gesture_feed(fdk_gesture *g, const fdk_event_data *event) {
    if (g == NULL || event == NULL) {
        return false;
    }
    switch (event->type) {
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        gesture_press(g, event->pointer_button.position,
                      event->pointer_button.button);
        return false;
    case FDK_EVENT_POINTER_BUTTON_UP:
        gesture_release(g, event->pointer_button.position);
        return g->claimed;
    case FDK_EVENT_POINTER_MOTION:
        gesture_motion(g, event->pointer.position);
        return false;
    case FDK_EVENT_TOUCH_DOWN:
        if (g->kind == FDK_GESTURE_ZOOM ||
            g->kind == FDK_GESTURE_ROTATE) {
            touch_down(g, event->touch.id, event->touch.position);
        }
        return false;
    case FDK_EVENT_TOUCH_MOTION:
        if (g->kind == FDK_GESTURE_ZOOM ||
            g->kind == FDK_GESTURE_ROTATE) {
            touch_two_track(g, event->touch.id, event->touch.position);
        }
        return false;
    case FDK_EVENT_TOUCH_UP:
    case FDK_EVENT_TOUCH_CANCEL:
        if (g->kind == FDK_GESTURE_ZOOM ||
            g->kind == FDK_GESTURE_ROTATE) {
            touch_up(g, event->touch.id);
        } else if (event->type == FDK_EVENT_TOUCH_CANCEL) {
            fdk_gesture_reset(g); /* the compositor took it: every
                                   * recognizer ends silently */
        }
        return false;
    default:
        return false;
    }
}
