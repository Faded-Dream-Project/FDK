/*
 * touch.c — the core touch-slot engine (1.1.0, M-3)
 *
 * Backends translate raw touch contacts (wl_touch on Wayland, XI2
 * touch through dlopen'd libXi on X11) into FDK_EVENT_TOUCH_*
 * events and hand them to fdk_window_dispatch_event. THIS module
 * owns the gesture policy that runs on top of the raw slots —
 * identical on both backends because it lives here, once:
 *
 *   TAP      one slot, down-to-up inside FDK_TOUCH_TAP_MS and
 *            FDK_TOUCH_TAP_SLOP logical px of travel, with no other
 *            slot ever having been active in between, synthesizes a
 *            full left POINTER_BUTTON_DOWN + POINTER_BUTTON_UP pair
 *            at the UP position (the position the finger left, which
 *            is what the user aimed at).
 *
 *   SCROLL   while exactly two slots are active, the CENTROID's
 *            motion between consecutive events synthesizes
 *            FDK_EVENT_POINTER_SCROLL with natural-scrolling deltas
 *            (delta = -(centroid travel): fingers drag down, content
 *            follows). Entering two-slot mode cancels any pending
 *            tap; dropping back below two slots ends the scroll.
 *
 *   CANCEL   wipes every slot, synthesizes nothing, and disarms all
 *            pending taps (the compositor took the gesture over —
 *            a click would be a phantom).
 *
 * The engine is deliberately PURE with respect to delivery: it
 * never touches a window or a platform itself, it calls the `emit`
 * callback the window layer hands it. That is what makes the whole
 * policy unit-testable headless (tests/test_touch.c drives a bare
 * engine through every rule with a recording sink) while the
 * window glue (window.c) stays a two-line hookup: feed every
 * TOUCH_* event in, route emitted synthesis back through
 * fdk_window_dispatch_event so synthesized clicks behave EXACTLY
 * like real ones (widget routing, grabs, hover state, popup
 * stamps — the recursive-dispatch pattern the close-request
 * synthesis already uses).
 *
 * Numbers: slots are capped at FDK_TOUCH_ENGINE_SLOTS; a compositor
 * reporting more contacts merges the overflow into "too many —
 * gesture only" (no tap synthesis, scroll stops) which is the
 * honest degradation (5-finger palm grabs are not precision input).
 */

#define FDK_LOG_TAG "window"

#include "window/window_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>
#include <math.h>

#define FDK_TOUCH_ENGINE_SLOTS 10

typedef struct fdk_touch_slot {
    int active;
    fdk_u32 id;
    fdk_pointf pos;
    fdk_pointf start;
    long long start_ms;
    /* C-6: the finger's lift time (0 while down) — the swipe's
     * "every lift within the window" rule reads it at the LAST
     * finger's UP through the slots that are STILL DOWN (their
     * lifts are future); the LIFTED ones' times are folded into
     * first_lift_ms/lifts_count at their own UP. */
    long long lift_ms;
} fdk_touch_slot;

struct fdk_touch_engine {
    fdk_touch_slot slots[FDK_TOUCH_ENGINE_SLOTS];
    int active_count;
    /* Two-finger scroll state: nonzero while exactly two slots are
     * active (recomputed on every DOWN/UP/CANCEL). scroll_armed is
     * the centroid bookkeeping — valid only while two_finger. */
    int two_finger;
    int scroll_armed;
    fdk_pointf scroll_last;
    /* Gesture-level latch: ANY moment with two or more simultaneous
     * slots since the last fully-idle instant. A slot lifted last
     * after a two-finger gesture must NOT tap (the finger was part
     * of a multi-touch gesture a heartbeat ago — reading its lone
     * count at UP time as "solo" is the classic tap-after-scroll
     * bug). Cleared when the engine goes fully idle. */
    int multi_ever;

    /* ---- C-6 gestures -------------------------------------------------
     * PINCH: the span (contact distance) at the moment the two-finger
     * posture began, and the span the last PINCH carried (the delta
     * denominator). span0 == 0 means "not armed". */
    float pinch_span0;
    float pinch_last_span;
    /* SWIPE: the gesture's peak membership, the first lift's time,
     * and the per-lift bookkeeping the verdict reads. lifts_count is
     * the number of UPs since the gesture began (>= 2 of them with
     * the last one closing the gesture = the swipe candidate). */
    int gesture_peak;
    long long first_lift_ms;
    int lifts_count;
    /* The swipe's per-finger travels (signed), captured at each UP:
     * the verdict reads them INDIVIDUALLY (every finger must have
     * flung — the aggregate alone would let two lazy fingers sum
     * into a "swipe") and as the aggregate (the dominant axis and
     * the agreement). Per-slot data dies with the slot, so the lifts
     * fold in here. */
    float swipe_travels[FDK_TOUCH_ENGINE_SLOTS][2];
    long long gesture_start_ms;
};

/* --- the engine's clock -------------------------------------------------
 *
 * The engine reads monotonic milliseconds through this indirection:
 * production rides fdk__window_now_ms(); the headless unit battery
 * (tests/test_touch.c) installs a CONTROLLABLE clock so the
 * tap-timeout and gesture-sequencing rules are testable without
 * real sleeps. Pure-function discipline: no clock calls anywhere
 * else in the engine. */
static long long (*g_touch_clock)(void *user);
static void *g_touch_clock_user;

static long long touch_now_ms(void) {
    if (g_touch_clock != NULL) {
        return g_touch_clock(g_touch_clock_user);
    }
    return fdk__window_now_ms();
}

void fdk__touch_set_test_clock(long long (*now)(void *user), void *user) {
    g_touch_clock = now;
    g_touch_clock_user = user;
}

static fdk_touch_slot *touch_find_slot(fdk_touch_engine *e, fdk_u32 id,
                                       int *out_index) {
    for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
        if (e->slots[i].active && e->slots[i].id == id) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return &e->slots[i];
        }
    }
    return NULL;
}

static fdk_touch_slot *touch_free_slot(fdk_touch_engine *e, int *out_index) {
    for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
        if (!e->slots[i].active) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return &e->slots[i];
        }
    }
    return NULL;
}

static int touch_count_active(const fdk_touch_engine *e) {
    int n = 0;
    for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
        if (e->slots[i].active) {
            n++;
        }
    }
    return n;
}

/* The current two-contact span (the pinch's substrate). 0 when the
 * posture is not exactly two slots (the caller checks). */
static float touch_span(const fdk_touch_engine *e) {
    fdk_pointf a = { 0.0f, 0.0f };
    fdk_pointf b = { 0.0f, 0.0f };
    int n = 0;
    for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
        if (e->slots[i].active) {
            if (n == 0) {
                a = e->slots[i].pos;
            } else if (n == 1) {
                b = e->slots[i].pos;
            }
            n++;
        }
    }
    if (n != 2) {
        return 0.0f;
    }
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    /* sqrt is the honest span; the ratio arithmetic downstream is
     * scale-free so the cost is the only concern — one sqrt per
     * two-finger MOTION is nothing. */
    return sqrtf(dx * dx + dy * dy);
}

/* (Re)compute the two-finger posture after any membership change.
 * Arming the scroll centroid only on the SECOND finger's arrival
 * (and on every re-entry) means the first motion after arming
 * produces a delta from the entry centroid — no phantom jump. */
static void touch_recompute_two_finger(fdk_touch_engine *e) {
    e->active_count = touch_count_active(e);
    if (e->active_count >= 2) {
        e->multi_ever = 1;
    } else if (e->active_count == 0) {
        /* Fully idle: the gesture is over — a fresh contact starts a
         * fresh gesture with a clean tap latch. */
        e->multi_ever = 0;
    }
    if (e->active_count == 2) {
        e->two_finger = 1;
        if (!e->scroll_armed) {
            /* arm: centroid of the two members */
            fdk_pointf c = { 0.0f, 0.0f };
            for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
                if (e->slots[i].active) {
                    c.x += e->slots[i].pos.x;
                    c.y += e->slots[i].pos.y;
                }
            }
            e->scroll_last.x = c.x * 0.5f;
            e->scroll_last.y = c.y * 0.5f;
            e->scroll_armed = 1;
            /* C-6: the pinch arms on the same posture's arrival — the
             * span baseline set at the POSTURE's start (re-arming on
             * re-entry: a finger swap mid-gesture re-baselines, the
             * only non-surprising choice). */
            e->pinch_span0 = touch_span(e);
            e->pinch_last_span = e->pinch_span0;
        }
        /* the gesture's peak membership (the swipe's finger count) */
        if (e->active_count > e->gesture_peak) {
            e->gesture_peak = e->active_count;
        }
    } else {
        e->two_finger = 0;
        e->scroll_armed = 0;
        if (e->active_count < 2) {
            e->pinch_span0 = 0.0f; /* posture over: disarm */
        }
    }
    if (e->active_count > e->gesture_peak) {
        e->gesture_peak = e->active_count;
    }
}

fdk_touch_engine *fdk__touch_engine_create(void) {
    fdk_touch_engine *e = fdk_alloc(sizeof(fdk_touch_engine));
    if (e != NULL) {
        memset(e, 0, sizeof(*e));
    }
    return e;
}

void fdk__touch_engine_destroy(fdk_touch_engine *e) {
    fdk_free(e);
}

void fdk__touch_engine_reset(fdk_touch_engine *e) {
    if (e != NULL) {
        memset(e, 0, sizeof(*e));
    }
}

int fdk__touch_engine_active(const fdk_touch_engine *e) {
    return (e != NULL) ? e->active_count : 0;
}

void fdk__touch_engine_feed(fdk_touch_engine *e, const fdk_event_data *event,
                            fdk_touch_emit_fn emit, void *user) {
    if (e == NULL || event == NULL || emit == NULL) {
        return;
    }

    switch (event->type) {
    case FDK_EVENT_TOUCH_DOWN: {
        fdk_u32 id = event->touch.id;
        fdk_pointf pos = event->touch.position;
        long long now = touch_now_ms();
        /* Duplicate DOWN for a live id: a protocol violation on the
         * platform side — treat as a MOTION (position update) rather
         * than corrupting the slot table. */
        fdk_touch_slot *slot = touch_find_slot(e, id, NULL);
        if (slot != NULL) {
            slot->pos = pos;
            return;
        }
        int idx = -1;
        slot = touch_free_slot(e, &idx);
        if (slot == NULL) {
            /* More contacts than the cap: this is a palm-grab scale
             * gesture — remember we're over cap so no tap can fire,
             * and the scroll path already needs exactly two members
             * (it keeps working: the first two slots are still live).
             * Honestly logged once per overflow event. */
            FDK_INFO("touch: %d+ contacts — over the %d-slot cap, "
                     "tap synthesis suspended for this gesture",
                     FDK_TOUCH_ENGINE_SLOTS + 1, FDK_TOUCH_ENGINE_SLOTS);
            return;
        }
        slot->active = 1;
        slot->id = id;
        slot->lift_ms = 0;
        slot->pos = pos;
        slot->start = pos;
        slot->start_ms = now;
        if (e->active_count == 0) {
            /* the gesture clock: the FIRST contact of a fresh
             * gesture (the swipe's overall-duration budget reads it) */
            e->gesture_start_ms = now;
            e->first_lift_ms = 0;
            e->lifts_count = 0;
            memset(e->swipe_travels, 0, sizeof(e->swipe_travels));
        }
        touch_recompute_two_finger(e);
        return;
    }

    case FDK_EVENT_TOUCH_MOTION: {
        fdk_touch_slot *slot = touch_find_slot(e, event->touch.id, NULL);
        if (slot == NULL) {
            return; /* MOTION for an unknown id: protocol noise */
        }
        slot->pos = event->touch.position;
        if (e->two_finger && e->scroll_armed) {
            /* Centroid of the current two members; the delta since
             * the last arm/motion becomes a scroll (natural: content
             * follows the fingers — delta = -(travel)). */
            fdk_pointf c = { 0.0f, 0.0f };
            for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
                if (e->slots[i].active) {
                    c.x += e->slots[i].pos.x;
                    c.y += e->slots[i].pos.y;
                }
            }
            c.x *= 0.5f;
            c.y *= 0.5f;
            fdk_event_data scroll = { .type = FDK_EVENT_POINTER_SCROLL };
            scroll.scroll.position = c;
            scroll.scroll.delta_x = -(c.x - e->scroll_last.x);
            scroll.scroll.delta_y = -(c.y - e->scroll_last.y);
            if (scroll.scroll.delta_x != 0.0f ||
                scroll.scroll.delta_y != 0.0f) {
                emit(user, &scroll);
            }
            e->scroll_last = c;
        }
        /* C-6: the pinch rides the SAME posture's span (coexists
         * with the scroll — centroid vs span). scale is cumulative
         * against the posture's start; delta is the incremental
         * ratio; both 1.0 when the span did not move. The focus is
         * the posture's centroid (recomputed — the scroll block's
         * local is out of scope here). */
        if (e->two_finger && e->pinch_span0 > 0.0f) {
            float span = touch_span(e);
            if (span > 0.0f) {
                float scale = span / e->pinch_span0;
                float delta =
                    (e->pinch_last_span > 0.0f)
                        ? span / e->pinch_last_span
                        : 1.0f;
                if (delta != 1.0f) {
                    fdk_pointf c2 = { 0.0f, 0.0f };
                    for (int i = 0; i < FDK_TOUCH_ENGINE_SLOTS; i++) {
                        if (e->slots[i].active) {
                            c2.x += e->slots[i].pos.x;
                            c2.y += e->slots[i].pos.y;
                        }
                    }
                    c2.x *= 0.5f;
                    c2.y *= 0.5f;
                    fdk_event_data pinch = {
                        .type = FDK_EVENT_GESTURE_PINCH,
                    };
                    pinch.gesture.focus = c2;
                    pinch.gesture.scale = scale;
                    pinch.gesture.delta = delta;
                    pinch.gesture.fingers = 2;
                    pinch.gesture.direction = 0;
                    emit(user, &pinch);
                }
                e->pinch_last_span = span;
            }
        }
        return;
    }

    case FDK_EVENT_TOUCH_UP: {
        fdk_u32 id = event->touch.id;
        int gesture_was_multi = e->multi_ever;
        fdk_touch_slot *slot = touch_find_slot(e, id, NULL);
        if (slot == NULL) {
            return;
        }
        /* The tap verdict and the click POSITION come from the
         * slot's tracked state, not the event: wl_touch's up carries
         * NO coordinates (the protocol omits them), so the last
         * MOTION position is the only honest tap position on
         * Wayland — and on X11 the XI_TouchEnd coords equal the last
         * update anyway. */
        long long now = touch_now_ms();
        long long elapsed = now - slot->start_ms;
        fdk_pointf pos = slot->pos;
        fdk_f32 dx = pos.x - slot->start.x;
        fdk_f32 dy = pos.y - slot->start.y;
        fdk_f32 travel = dx * dx + dy * dy; /* squared — no sqrt need */

        /* C-6: fold this finger's lift into the swipe bookkeeping
         * (the travels accumulate; the lift window latches at the
         * FIRST lift). */
        int gesture_peak_at_up = e->gesture_peak;
        if (e->lifts_count == 0) {
            e->first_lift_ms = now;
        }
        if (e->lifts_count < FDK_TOUCH_ENGINE_SLOTS) {
            e->swipe_travels[e->lifts_count][0] = dx;
            e->swipe_travels[e->lifts_count][1] = dy;
        }
        e->lifts_count++;
        slot->lift_ms = now;

        slot->active = 0;
        touch_recompute_two_finger(e);

        /* TAP: a single-slot gesture — the multi_ever latch proves no
         * second finger intervened at ANY point (the lifted-last
         * finger of a two-finger gesture must not click), inside the
         * time and slop budgets. The click lands at the tracked
         * position. */
        if (!gesture_was_multi &&
            elapsed >= 0 && elapsed <= FDK_TOUCH_TAP_MS &&
            travel <= FDK_TOUCH_TAP_SLOP * FDK_TOUCH_TAP_SLOP) {
            fdk_event_data down = { .type = FDK_EVENT_POINTER_BUTTON_DOWN };
            down.pointer_button.position = pos;
            down.pointer_button.button = FDK_POINTER_BUTTON_LEFT;
            down.pointer_button.modifiers = 0;
            emit(user, &down);
            fdk_event_data up = { .type = FDK_EVENT_POINTER_BUTTON_UP };
            up.pointer_button.position = pos;
            up.pointer_button.button = FDK_POINTER_BUTTON_LEFT;
            up.pointer_button.modifiers = 0;
            emit(user, &up);
        }

        /* C-6: the SWIPE verdict — this UP closed the gesture (no
         * slots remain). The gesture must have been multi-finger at
         * its peak, every lift inside the lift window, each finger's
         * AGGREGATE travel beyond the minimum, and the travel must
         * have a dominant axis (the direction). A fast fling whose
         * span also changed has already fired its PINCHes — both
         * firing is by design (consumers pick by intent). */
        if (e->active_count == 0 && gesture_peak_at_up >= 2) {
            long long lift_window = now - e->first_lift_ms;
            long long gesture_time = now - e->gesture_start_ms;
            /* The aggregate (direction + agreement) and the per-finger
             * minimum (EVERY finger must have flung — two lazy
             * fingers do not sum into a swipe). */
            float sx = 0.0f, sy = 0.0f;
            int n_lifts =
                e->lifts_count < FDK_TOUCH_ENGINE_SLOTS
                    ? e->lifts_count
                    : FDK_TOUCH_ENGINE_SLOTS;
            for (int i = 0; i < n_lifts; i++) {
                sx += e->swipe_travels[i][0];
                sy += e->swipe_travels[i][1];
            }
            float mag_x = sx < 0.0f ? -sx : sx;
            float mag_y = sy < 0.0f ? -sy : sy;
            int horizontal = mag_x > mag_y;
            float dominant = horizontal ? mag_x : mag_y;
            int direction = 0;
            if (dominant >= FDK_GESTURE_SWIPE_MIN_PX) {
                direction = horizontal
                                ? ((sx < 0.0f) ? FDK_SWIPE_LEFT
                                               : FDK_SWIPE_RIGHT)
                                : ((sy < 0.0f) ? FDK_SWIPE_UP
                                               : FDK_SWIPE_DOWN);
                /* per-finger minimum on the dominant axis */
                float min_axis = 1e30f;
                for (int i = 0; i < n_lifts; i++) {
                    float t = horizontal ? e->swipe_travels[i][0]
                                         : e->swipe_travels[i][1];
                    if (t < 0.0f) {
                        t = -t;
                    }
                    if (t < min_axis) {
                        min_axis = t;
                    }
                }
                if (min_axis >= FDK_GESTURE_SWIPE_MIN_PX &&
                    lift_window <= FDK_GESTURE_SWIPE_MS &&
                    gesture_time <= 2 * FDK_GESTURE_SWIPE_MS) {
                    fdk_event_data swipe = {
                        .type = FDK_EVENT_GESTURE_SWIPE,
                    };
                    swipe.gesture.focus = pos;
                    swipe.gesture.scale = 1.0f;
                    swipe.gesture.delta = 1.0f;
                    swipe.gesture.fingers = gesture_peak_at_up;
                    swipe.gesture.direction = direction;
                    emit(user, &swipe);
                }
            }
            /* the gesture is over: the bookkeeping resets with the
             * next DOWN (active_count == 0 branch) — but multi_ever
             * clears only at the recompute above's idle path, and
             * gesture_peak must not leak into the next gesture. */
            e->gesture_peak = 0;
            e->lifts_count = 0;
            e->first_lift_ms = 0;
        }
        return;
    }

    case FDK_EVENT_TOUCH_CANCEL:
        /* The compositor revoked the whole gesture: every slot dies,
         * no synthesis (a click now would be a phantom press). */
        fdk__touch_engine_reset(e);
        return;

    default:
        return; /* not a touch event — window.c only feeds TOUCH_* */
    }
}
