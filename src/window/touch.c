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

#define FDK_TOUCH_ENGINE_SLOTS 10

typedef struct fdk_touch_slot {
    int active;
    fdk_u32 id;
    fdk_pointf pos;
    fdk_pointf start;
    long long start_ms;
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
        }
    } else {
        e->two_finger = 0;
        e->scroll_armed = 0;
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
        slot->pos = pos;
        slot->start = pos;
        slot->start_ms = now;
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
