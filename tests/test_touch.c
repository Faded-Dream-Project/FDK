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

    assert(g_rig.count == 1);
    assert(g_rig.events[0].type == FDK_EVENT_POINTER_SCROLL);
    assert(g_rig.events[0].scroll.delta_x == -10.0f);
    assert(g_rig.events[0].scroll.delta_y == -20.0f);
    /* Scroll position = the centroid. */
    assert(g_rig.events[0].scroll.position.x == 140.0f);
    assert(g_rig.events[0].scroll.position.y == 120.0f);

    /* Re-arm without a jump: one finger lifts, comes back down, and
     * moves — the FIRST motion after re-entry is measured from the
     * re-entry centroid, not the stale one. */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_UP, 11, 160.0f, 100.0f));
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_DOWN, 11, 160.0f, 100.0f));
    /* Centroid re-arms at ((120+160)/2, (140+100)/2) = (140, 120). */
    rig_feed(eng, touch_evt(FDK_EVENT_TOUCH_MOTION, 11, 161.0f, 101.0f));
    /* One finger by (1,1): centroid (+0.5, +0.5) -> delta (-0.5, -0.5). */
    assert(g_rig.count == 2);
    assert(g_rig.events[1].type == FDK_EVENT_POINTER_SCROLL);
    assert(g_rig.events[1].scroll.delta_x == -0.5f);
    assert(g_rig.events[1].scroll.delta_y == -0.5f);

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

    /* Restore production time for any later user of this process. */
    fdk__touch_set_test_clock(NULL, NULL);

    printf("\nall touch-engine tests passed\n");
    return 0;
}
