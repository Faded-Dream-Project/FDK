/*
 * test_touch.c — the touch-slot engine's policy battery (1.1.0, M-3)
 *
 * Headless and pure: a bare engine, a recording sink, and a
 * controllable clock (fdk__touch_set_test_clock) — every rule the
 * engine owns gets pinned here without a display or compositor:
 *
 *   - TAP: inside the time+slop budgets synthesizes a full left
 *     click pair at the tracked position
 *   - LONG-PRESS: over FDK_TOUCH_TAP_MS -> no click
 *   - DRAG: over the slop radius -> no click (the tracked position,
 *     not the DOWN position, is what the verdict measures against)
 *   - TWO-FINGER SCROLL: centroid motion with natural-scrolling
 *     deltas (delta = -(travel)); entering two fingers cancels any
 *     pending tap; re-entry re-arms the centroid without a jump
 *   - TAP-AFTER-TWO-FINGER: the lifted-last finger of a multi
 *     gesture never clicks (the multi_ever latch)
 *   - CANCEL: wipes every slot, synthesizes nothing, and disarms
 *     pending taps
 *   - protocol noise: duplicate DOWN, UP for an unknown id, MOTION
 *     for an unknown id are quiet no-ops
 *
 * The X11/Wayland integration suites drive the SAME engine through
 * the real translate paths (fdk_x11_xi2_test_touch / the wayland
 * test_touch op) — this file pins the policy itself.
 */

#include "window/window_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- the recording sink + the controllable clock -----------------------
 *
 * ONE file-scope rig: the engine's clock indirection is global
 * (g_touch_clock), so the rig it reads must outlive every feed.
 * Each test resets it through rig_reset(). */

#define SINK_MAX 64
static struct {
    fdk_event_data events[SINK_MAX];
    int count;
    long long now_ms;
} g_rig;

static void rig_reset(void) {
    g_rig.count = 0;
    g_rig.now_ms = 0;
}

static void rig_emit(void *user, const fdk_event_data *event) {
    (void)user;
    assert(g_rig.count < SINK_MAX);
    g_rig.events[g_rig.count++] = *event;
}

static long long rig_clock(void *user) {
    (void)user;
    return g_rig.now_ms;
}

static fdk_event_data touch_evt(fdk_event_type type, fdk_u32 id,
                                float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.touch.id = id;
    e.touch.position.x = x;
    e.touch.position.y = y;
    return e;
}

static void rig_feed(fdk_touch_engine *eng, fdk_event_data e) {
    fdk__touch_engine_feed(eng, &e, rig_emit, NULL);
}

/* C-6: the engine now emits GESTURE events alongside the classic
 * synthesis — the finder/count helpers keep the assertions about
 * ONE stream honest (count scrolls, find the first pinch, ...). */
static int rig_count_of(fdk_event_type type) {
    int n = 0;
    for (int i = 0; i < g_rig.count; i++) {
        if (g_rig.events[i].type == type) {
            n++;
        }
    }
    return n;
}

static const fdk_event_data *rig_first(fdk_event_type type) {
    for (int i = 0; i < g_rig.count; i++) {
        if (g_rig.events[i].type == type) {
            return &g_rig.events[i];
        }
    }
    return NULL;
}

static void test_tap_synthesizes_click(void) {
    rig_reset(); g_rig.now_ms = 1000;
    fdk_touch_engine *eng = fdk__touch_engine_create();
    assert(eng != NULL);

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 5, 100.0f, 120.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 5, 103.0f, 118.0f));

    /* A full click pair: DOWN then UP, left button, at the tracked
     * (last-motion) position, no modifiers. */
    assert(g_rig.count == 2);
    assert(g_rig.events[0].type == FDK_EVENT_POINTER_BUTTON_DOWN);
    assert(g_rig.events[1].type == FDK_EVENT_POINTER_BUTTON_UP);
    assert(g_rig.events[0].pointer_button.button == FDK_POINTER_BUTTON_LEFT);
    assert(g_rig.events[1].pointer_button.button == FDK_POINTER_BUTTON_LEFT);
    assert(g_rig.events[0].pointer_button.modifiers == 0);
    /* The UP carried no coordinates (wl_touch shape); the engine used
     * the tracked DOWN position. */
    assert(g_rig.events[0].pointer_button.position.x == 100.0f);
    assert(g_rig.events[0].pointer_button.position.y == 120.0f);

    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: tap -> full left click pair at the tracked position\n");
}

static void test_tap_motion_inside_slop_uses_last_position(void) {
    rig_reset(); g_rig.now_ms = 5000;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 1, 50.0f, 50.0f));
    g_rig.now_ms = 5100;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 1, 54.0f, 55.0f));
    g_rig.now_ms = 5200;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 1, 0.0f, 0.0f));

    assert(g_rig.count == 2);
    /* The click lands at the LAST MOTION position (the tracked one),
     * travel measured from the DOWN: 4,5 px < slop 10. */
    assert(g_rig.events[0].pointer_button.position.x == 54.0f);
    assert(g_rig.events[0].pointer_button.position.y == 55.0f);

    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: tap with in-slop motion clicks at the last position\n");
}

static void test_long_press_is_not_a_tap(void) {
    rig_reset();
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 2, 20.0f, 20.0f));
    g_rig.now_ms = FDK_TOUCH_TAP_MS + 1; /* past the budget */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 2, 20.0f, 20.0f));

    assert(g_rig.count == 0);
    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: press longer than FDK_TOUCH_TAP_MS never clicks\n");
}

static void test_drag_is_not_a_tap(void) {
    rig_reset(); g_rig.now_ms = 100;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 3, 10.0f, 10.0f));
    g_rig.now_ms = 150;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 3, 40.0f, 80.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 3, 40.0f, 80.0f));

    assert(g_rig.count == 0);
    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: drag beyond the slop radius never clicks\n");
}

static void test_two_finger_scroll_natural_deltas(void) {
    rig_reset();
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 10, 100.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 11, 160.0f, 100.0f));
    /* Centroid = (130, 100). ONE finger moves by (+20, +40): the
     * centroid moves by (+10, +20) — natural-scrolling delta =
     * -(travel) = (-10, -20). (Platform backends deliver the two
     * fingers' updates as separate events; each contributes its
     * half to the centroid — the SUM of a hardware snapshot's two
     * scroll events is the full centroid travel, which is what a
     * delta-accumulating consumer like the scroll widget sees.) */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 10, 120.0f, 140.0f));

    /* (the motion ALSO changed the span 60 -> ~72: one PINCH rides
     * along — the coexistence C-6 documents. The scroll assertions
     * filter by type.) */
    assert(rig_count_of(FDK_EVENT_POINTER_SCROLL) == 1);
    const fdk_event_data *sc = rig_first(FDK_EVENT_POINTER_SCROLL);
    assert(sc != NULL);
    assert(sc->scroll.delta_x == -10.0f);
    assert(sc->scroll.delta_y == -20.0f);
    /* Scroll position = the centroid. */
    assert(sc->scroll.position.x == 140.0f);
    assert(sc->scroll.position.y == 120.0f);
    assert(rig_count_of(FDK_EVENT_GESTURE_PINCH) == 1);

    /* Re-arm without a jump: one finger lifts, comes back down, and
     * moves — the FIRST motion after re-entry is measured from the
     * re-entry centroid, not the stale one. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 11, 160.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 11, 160.0f, 100.0f));
    /* Centroid re-arms at ((120+160)/2, (140+100)/2) = (140, 120). */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 11, 161.0f, 101.0f));
    /* One finger by (1,1): centroid (+0.5, +0.5) -> delta (-0.5, -0.5).
     * (Plus the re-armed pinch — the span moved 40 -> ~41.4.) */
    assert(rig_count_of(FDK_EVENT_POINTER_SCROLL) == 2);
    const fdk_event_data *sc2 = NULL;
    for (int i = 0; i < g_rig.count; i++) {
        if (g_rig.events[i].type == FDK_EVENT_POINTER_SCROLL) {
            sc2 = &g_rig.events[i];
        }
    }
    assert(sc2 != NULL);
    assert(sc2->scroll.delta_x == -0.5f);
    assert(sc2->scroll.delta_y == -0.5f);

    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: two-finger scroll emits natural deltas, re-arm is jump-free\n");
}

static void test_pending_tap_cancelled_by_second_finger(void) {
    rig_reset();
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 20, 60.0f, 60.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 21, 90.0f, 60.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 20, 60.0f, 60.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 21, 90.0f, 60.0f));

    /* Neither finger taps (the multi_ever latch), and with no motion
     * there is no scroll: nothing at all is synthesized. */
    assert(g_rig.count == 0);
    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: a second finger cancels every pending tap\n");
}

static void test_tap_after_two_finger_never_fires(void) {
    rig_reset(); g_rig.now_ms = 1000;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    /* Two-finger gesture that ends with one finger still down. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 30, 100.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 31, 130.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 31, 130.0f, 100.0f));
    /* Now the LAST finger lifts quickly, in-place — the classic
     * tap-after-scroll bug shape: it was part of a multi gesture a
     * heartbeat ago and must NOT click. */
    g_rig.now_ms = 1100;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 30, 100.0f, 100.0f));
    assert(g_rig.count == 0);

    /* After going fully idle, a FRESH contact taps normally again. */
    g_rig.now_ms = 2000;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 32, 70.0f, 70.0f));
    g_rig.now_ms = 2100;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 32, 70.0f, 70.0f));
    assert(g_rig.count == 2);
    assert(g_rig.events[0].type == FDK_EVENT_POINTER_BUTTON_DOWN);

    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: the lifted-last finger of a multi gesture never clicks; "
           "a fresh contact does\n");
}

static void test_cancel_wipes_and_disarms(void) {
    rig_reset();
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 40, 80.0f, 80.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_CANCEL, 40, 0, 0));
    assert(fdk__touch_engine_active(eng) == 0);
    /* The disarmed tap: the UP after a CANCEL synthesizes nothing. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 40, 80.0f, 80.0f));
    assert(g_rig.count == 0);

    /* And the engine is clean for the next gesture. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 41, 10.0f, 10.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 41, 10.0f, 10.0f));
    assert(g_rig.count == 2);

    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: CANCEL wipes slots and disarms the pending tap\n");
}

static void test_protocol_noise_is_quiet(void) {
    rig_reset();
    fdk_touch_engine *eng = fdk__touch_engine_create();

    /* UP for an id that never went down. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 99, 5.0f, 5.0f));
    /* MOTION for an unknown id. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 98, 5.0f, 5.0f));
    /* Duplicate DOWN for a live slot is treated as a position update. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 7, 30.0f, 30.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 7, 33.0f, 31.0f));
    assert(fdk__touch_engine_active(eng) == 1);
    /* A non-touch event is ignored entirely. */
    fdk_event_data key;
    memset(&key, 0, sizeof(key));
    key.type = FDK_EVENT_KEY_DOWN;
    fdk__touch_engine_feed(eng, &key, rig_emit, NULL);

    assert(g_rig.count == 0);
    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: duplicate DOWN / unknown UP+MOTION / non-touch feed are quiet\n");
}

static void test_engine_lifecycle(void) {
    fdk_touch_engine *eng = fdk__touch_engine_create();
    assert(eng != NULL);
    assert(fdk__touch_engine_active(eng) == 0);
    fdk__touch_engine_reset(eng);
    assert(fdk__touch_engine_active(eng) == 0);
    fdk__touch_engine_destroy(NULL); /* safe no-op */
    fdk__touch_engine_destroy(eng);
    printf("[ok] touch: engine create/reset/destroy lifecycle\n");
}

/* ---- 1.1.0, C-6: the gestures ------------------------------------------- */

/* PINCH: the span arithmetic. Two contacts 60 apart spread to 120
 * apart: ONE pinch with scale 2.0 (cumulative), delta 2.0 (the
 * first span change). A further motion to 90 apart: scale 1.5,
 * delta 0.75 (90/120). The focus is the centroid. */
static void test_pinch_span_arithmetic(void) {
    rig_reset(); g_rig.now_ms = 100;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 30, 100.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 31, 160.0f, 100.0f));
    /* spread: 100 -> 160 (span 60 -> 60: same distance, y equal) */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 30, 70.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 31, 190.0f, 100.0f));

    assert(rig_count_of(FDK_EVENT_GESTURE_PINCH) == 2);
    const fdk_event_data *p1 = rig_first(FDK_EVENT_GESTURE_PINCH);
    assert(p1 != NULL);
    /* the first motion (finger 30 to 70): span 60 -> 90 */
    assert(p1->gesture.scale > 1.49f && p1->gesture.scale < 1.51f);
    assert(p1->gesture.delta > 1.49f && p1->gesture.delta < 1.51f);
    assert(p1->gesture.fingers == 2);
    assert(p1->gesture.focus.x > 114.9f && p1->gesture.focus.x < 115.1f);
    assert(p1->gesture.focus.y == 100.0f);
    /* the second motion (finger 31 to 190): span 90 -> 120 */
    const fdk_event_data *p2 = NULL;
    for (int i = 0; i < g_rig.count; i++) {
        if (g_rig.events[i].type == FDK_EVENT_GESTURE_PINCH) {
            p2 = &g_rig.events[i];
        }
    }
    assert(p2 != NULL);
    assert(p2->gesture.scale > 1.99f && p2->gesture.scale < 2.01f);
    assert(p2->gesture.delta > 1.32f && p2->gesture.delta < 1.34f);
    /* The scroll coexisted (the centroid moved too). */
    assert(rig_count_of(FDK_EVENT_POINTER_SCROLL) >= 1);

    fdk__touch_engine_destroy(eng);
    printf("[ok] gesture: pinch span arithmetic (scale cumulative, "
           "delta incremental, centroid focus, scroll coexistence)\n");
}

/* PINCH: the re-arm baseline. A finger swap mid-gesture re-baselines
 * the scale to the NEW posture's span at re-entry — the numbers
 * prove it: posture 1 spans 60 (100..160), spreads to 80 (scale
 * 4/3); the finger swaps at 80; the spread continues to 160 — the
 * scale is 2.0 (160/80, the NEW baseline), NOT 8/3 (160/60, the
 * stale one). No phantom jump. */
static void test_pinch_rebaseline_on_reentry(void) {
    rig_reset(); g_rig.now_ms = 200;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 40, 100.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 41, 160.0f, 100.0f));
    /* spread 60 -> 80: one pinch, scale 4/3 */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 41, 180.0f, 100.0f));
    assert(rig_count_of(FDK_EVENT_GESTURE_PINCH) == 1);
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 41, 180.0f, 100.0f));
    /* the replacement finger lands at the SAME place (span 80) */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 42, 180.0f, 100.0f));
    /* spread 80 -> 160: the re-baselined scale is 2.0 */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 42, 260.0f, 100.0f));
    assert(rig_count_of(FDK_EVENT_GESTURE_PINCH) == 2);
    const fdk_event_data *last = NULL;
    for (int i = 0; i < g_rig.count; i++) {
        if (g_rig.events[i].type == FDK_EVENT_GESTURE_PINCH) {
            last = &g_rig.events[i];
        }
    }
    assert(last != NULL);
    assert(last->gesture.scale > 1.99f && last->gesture.scale < 2.01f);
    assert(last->gesture.delta > 1.99f && last->gesture.delta < 2.01f);

    fdk__touch_engine_destroy(eng);
    printf("[ok] gesture: pinch re-baselines on posture re-entry "
           "(the numbers: 2.0, not 8/3)\n");
}

/* SWIPE: the accept case. Two fingers land, BOTH travel +100px on x
 * (right), both lift within the window: one SWIPE(RIGHT, 2). */
static void test_swipe_two_finger_right(void) {
    rig_reset(); g_rig.now_ms = 1000;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 50, 100.0f, 200.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 51, 120.0f, 300.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 50, 180.0f, 200.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 51, 200.0f, 300.0f));
    g_rig.now_ms = 1100;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 50, 200.0f, 200.0f));
    g_rig.now_ms = 1150;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 51, 200.0f, 300.0f));

    assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 1);
    const fdk_event_data *sw = rig_first(FDK_EVENT_GESTURE_SWIPE);
    assert(sw != NULL);
    assert(sw->gesture.direction == FDK_SWIPE_RIGHT);
    assert(sw->gesture.fingers == 2);

    fdk__touch_engine_destroy(eng);
    printf("[ok] gesture: two-finger horizontal travel -> "
           "SWIPE(RIGHT, 2)\n");
}

/* SWIPE: three fingers, vertical (UP — negative y travel). */
static void test_swipe_three_finger_up(void) {
    rig_reset(); g_rig.now_ms = 5000;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 60, 100.0f, 400.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 61, 150.0f, 400.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 62, 200.0f, 400.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 60, 100.0f, 300.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 61, 150.0f, 290.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 62, 200.0f, 300.0f));
    g_rig.now_ms = 5100;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 60, 100.0f, 300.0f));
    g_rig.now_ms = 5150;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 61, 150.0f, 290.0f));
    g_rig.now_ms = 5200;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 62, 200.0f, 300.0f));

    assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 1);
    const fdk_event_data *sw = rig_first(FDK_EVENT_GESTURE_SWIPE);
    assert(sw != NULL);
    assert(sw->gesture.direction == FDK_SWIPE_UP);
    assert(sw->gesture.fingers == 3);

    fdk__touch_engine_destroy(eng);
    printf("[ok] gesture: three-finger vertical travel -> "
           "SWIPE(UP, 3)\n");
}

/* SWIPE: the rejects. (a) too-slow lifts (the lift window); (b) the
 * travel under the minimum; (c) ONE finger only (never a swipe);
 * (d) opposing travels on the dominant axis (disagreement). */
static void test_swipe_rejects(void) {
    /* (a) the lift window */
    {
        rig_reset(); g_rig.now_ms = 1000;
        fdk_touch_engine *eng = fdk__touch_engine_create();
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 70, 100.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 71, 120.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 70, 200.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 71, 220.0f, 100.0f));
        g_rig.now_ms = 1100;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 70, 200.0f, 100.0f));
        g_rig.now_ms = 1100 + FDK_GESTURE_SWIPE_MS + 1;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 71, 220.0f, 100.0f));
        assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 0);
        fdk__touch_engine_destroy(eng);
    }
    /* (b) under the travel minimum */
    {
        rig_reset(); g_rig.now_ms = 100;
        fdk_touch_engine *eng = fdk__touch_engine_create();
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 72, 100.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 73, 120.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 72, 130.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 73, 140.0f, 100.0f));
        g_rig.now_ms = 150;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 72, 130.0f, 100.0f));
        g_rig.now_ms = 180;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 73, 140.0f, 100.0f));
        assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 0);
        fdk__touch_engine_destroy(eng);
    }
    /* (c) one finger: a fast fling is NOT a swipe (fingers >= 2) */
    {
        rig_reset(); g_rig.now_ms = 100;
        fdk_touch_engine *eng = fdk__touch_engine_create();
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 74, 100.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 74, 300.0f, 100.0f));
        g_rig.now_ms = 200;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 74, 300.0f, 100.0f));
        assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 0);
        fdk__touch_engine_destroy(eng);
    }
    /* (d) disagreement: the fingers travel OPPOSITE ways on x */
    {
        rig_reset(); g_rig.now_ms = 100;
        fdk_touch_engine *eng = fdk__touch_engine_create();
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 75, 100.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 76, 300.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 75, 200.0f, 100.0f));
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 76, 200.0f, 100.0f));
        g_rig.now_ms = 200;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 75, 200.0f, 100.0f));
        g_rig.now_ms = 250;
        rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 76, 200.0f, 100.0f));
        /* aggregate x travel: +100 - 100 = 0 -> no dominant axis */
        assert(rig_count_of(FDK_EVENT_GESTURE_SWIPE) == 0);
        fdk__touch_engine_destroy(eng);
    }
    printf("[ok] gesture: swipe rejects (slow lift window, short "
           "travel, single finger, disagreement)\n");
}

/* CANCEL: the gestures die with the slots (a revoked gesture
 * synthesizes nothing — pinch, swipe, scroll, click). */
static void test_cancel_kills_gestures(void) {
    rig_reset(); g_rig.now_ms = 100;
    fdk_touch_engine *eng = fdk__touch_engine_create();

    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 80, 100.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 81, 160.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 80, 70.0f, 100.0f));
    fdk_event_data cancel = { .type = FDK_EVENT_TOUCH_CANCEL };
    rig_feed(eng, cancel);
    /* anything after the cancel is a fresh gesture; nothing from the
     * revoked one ever fires. (The pre-cancel motion legitimately
     * synthesized BOTH a scroll and a pinch — the coexistence; the
     * post-cancel UP synthesized NOTHING: no swipe, no click.) */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 81, 190.0f, 100.0f));
    /* the UPs after the cancel synthesize nothing (no swipe, no
     * click — the slots are gone; the engine never saw these ids) */
    int pre_cancel = g_rig.count;
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 80, 190.0f, 100.0f));
    assert(g_rig.count == pre_cancel);
    assert(pre_cancel == 2); /* the scroll + the pinch */
    assert(g_rig.events[0].type == FDK_EVENT_POINTER_SCROLL);
    assert(g_rig.events[1].type == FDK_EVENT_GESTURE_PINCH);

    fdk__touch_engine_destroy(eng);
    printf("[ok] gesture: CANCEL wipes the gesture state — no swipe "
           "from a revoked gesture\n");
}

int main(void) {
    /* The clock reads the file-scope rig; binding once is enough. */
    fdk__touch_set_test_clock(rig_clock, NULL);

    test_tap_synthesizes_click();
    test_tap_motion_inside_slop_uses_last_position();
    test_long_press_is_not_a_tap();
    test_drag_is_not_a_tap();
    test_two_finger_scroll_natural_deltas();
    test_pending_tap_cancelled_by_second_finger();
    test_tap_after_two_finger_never_fires();
    test_cancel_wipes_and_disarms();
    test_protocol_noise_is_quiet();
    test_engine_lifecycle();
    test_pinch_span_arithmetic();
    test_pinch_rebaseline_on_reentry();
    test_swipe_two_finger_right();
    test_swipe_three_finger_up();
    test_swipe_rejects();
    test_cancel_kills_gestures();

    /* Restore production time for any later user of this process. */
    fdk__touch_set_test_clock(NULL, NULL);

    printf("\nall touch-engine tests passed\n");
    return 0;
}
