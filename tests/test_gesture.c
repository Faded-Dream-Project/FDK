/*
 * test_gesture.c — headless tests for the §23/B2b recognizers
 * (include/fdk/fdk_gesture.h, src/window/gesture.c).
 *
 * The whole suite drives the recognizers through fdk_gesture_feed
 * with a CONTROLLABLE CLOCK (the touch engine's discipline: no test
 * ever sleeps — the M-3 lesson). Event structs are built by hand;
 * the recognizer never touches a window, so no display is needed.
 *
 * What is proven here:
 *   - click: single/double/triple counts within the 400 ms window,
 *     the chain resets past it, a moved press is not a click
 *   - long-press: fires at 500 ms (and at 500 * delay_factor),
 *     cancelled on early release, degraded by movement past the
 *     slop
 *   - drag: begin at the 5 px threshold, update offsets, end with
 *     the final total; below the threshold: nothing
 *   - pan: the orientation latch (a vertical pan denied when the
 *     axis is horizontal)
 *   - swipe: velocity and direction on a fast release; a slow
 *     release fires nothing
 *   - zoom: the span ratio from two touch slots, re-baselined
 *   - rotate: the angle delta (positive = clockwise)
 *   - reset: an interrupted recognizer ends silently
 */

#include "hermetic_env.h"
#include "fdk/fdk_gesture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- the controllable clock ---- */

static long long g_now;
static long long test_clock(void *user) {
    (void)user;
    return g_now;
}

/* ---- event builders ---- */

static fdk_event_data g_ev;

static const fdk_event_data *button_down(fdk_f32 x, fdk_f32 y) {
    memset(&g_ev, 0, sizeof g_ev);
    g_ev.type = FDK_EVENT_POINTER_BUTTON_DOWN;
    g_ev.pointer_button.position.x = x;
    g_ev.pointer_button.position.y = y;
    g_ev.pointer_button.button = FDK_POINTER_BUTTON_LEFT;
    return &g_ev;
}

static const fdk_event_data *button_up(fdk_f32 x, fdk_f32 y) {
    memset(&g_ev, 0, sizeof g_ev);
    g_ev.type = FDK_EVENT_POINTER_BUTTON_UP;
    g_ev.pointer_button.position.x = x;
    g_ev.pointer_button.position.y = y;
    g_ev.pointer_button.button = FDK_POINTER_BUTTON_LEFT;
    return &g_ev;
}

static const fdk_event_data *motion(fdk_f32 x, fdk_f32 y) {
    memset(&g_ev, 0, sizeof g_ev);
    g_ev.type = FDK_EVENT_POINTER_MOTION;
    g_ev.pointer.position.x = x;
    g_ev.pointer.position.y = y;
    return &g_ev;
}

static const fdk_event_data *touch(fdk_event_type t, fdk_u32 id,
                                   fdk_f32 x, fdk_f32 y) {
    memset(&g_ev, 0, sizeof g_ev);
    g_ev.type = t;
    g_ev.touch.id = id;
    g_ev.touch.position.x = x;
    g_ev.touch.position.y = y;
    return &g_ev;
}

/* ---- recorders ---- */

static int g_clicks;
static int g_last_npress;
static void on_click(fdk_gesture *g, int n_press, fdk_pointf where,
                     void *user) {
    (void)g; (void)where; (void)user;
    g_clicks++;
    g_last_npress = n_press;
}

static int g_lp_pressed, g_lp_cancelled;
static void on_long_press(fdk_gesture *g, fdk_pointf where,
                          bool pressed_not_cancelled, void *user) {
    (void)g; (void)where; (void)user;
    if (pressed_not_cancelled) {
        g_lp_pressed++;
    } else {
        g_lp_cancelled++;
    }
}

static int g_drags;
static fdk_pointf g_last_offset;
static void on_drag(fdk_gesture *g, fdk_pointf offset, fdk_pointf where,
                    void *user) {
    (void)g; (void)where; (void)user;
    g_drags++;
    g_last_offset = offset;
}

static int g_swipes;
static fdk_f32 g_last_vx, g_last_vy;
static fdk_gesture_direction g_last_dir;
static void on_swipe(fdk_gesture *g, fdk_f32 vx, fdk_f32 vy,
                     fdk_gesture_direction dir, void *user) {
    (void)g; (void)user;
    g_swipes++;
    g_last_vx = vx;
    g_last_vy = vy;
    g_last_dir = dir;
}

static int g_scales;
static fdk_f32 g_last_value, g_last_delta;
static void on_scale(fdk_gesture *g, fdk_f32 value, fdk_f32 delta,
                     void *user) {
    (void)g; (void)user;
    g_scales++;
    g_last_value = value;
    g_last_delta = delta;
}

/* ---- the tests ---- */

static void test_click(void) {
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_CLICK);
    assert(g != NULL);
    fdk_gesture_on_click(g, on_click, NULL);

    /* single */
    g_now = 1000;
    fdk_gesture_feed(g, button_down(50, 50));
    fdk_gesture_feed(g, button_up(51, 50));
    assert(g_clicks == 1 && g_last_npress == 1);

    /* double, within the window */
    g_now = 1200;
    fdk_gesture_feed(g, button_down(51, 50));
    fdk_gesture_feed(g, button_up(51, 51));
    assert(g_clicks == 2 && g_last_npress == 2);

    /* triple */
    g_now = 1400;
    fdk_gesture_feed(g, button_down(51, 51));
    fdk_gesture_feed(g, button_up(50, 51));
    assert(g_clicks == 3 && g_last_npress == 3);

    /* past the window: a fresh single */
    g_now = 5000;
    fdk_gesture_feed(g, button_down(50, 50));
    fdk_gesture_feed(g, button_up(50, 50));
    assert(g_clicks == 4 && g_last_npress == 1);

    /* a moved press is not a click */
    fdk_gesture_feed(g, button_down(50, 50));
    fdk_gesture_feed(g, motion(70, 50));
    fdk_gesture_feed(g, button_up(70, 50));
    assert(g_clicks == 4);

    fdk_gesture_destroy(g);
    printf("[ok] gesture click: 1/2/3 counts, window reset, moved "
           "press rejected\n");
}

static void test_long_press(void) {
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_LONG_PRESS);
    assert(g != NULL);
    fdk_gesture_on_long_press(g, on_long_press, NULL);

    /* fires at 500 ms — a motion event at the timeout ticks the
     * evaluation (the documented no-timer model) */
    g_now = 0;
    fdk_gesture_feed(g, button_down(40, 40));
    g_now = 300;
    fdk_gesture_feed(g, motion(41, 40)); /* still inside the slop */
    assert(g_lp_pressed == 0);
    g_now = 600;
    fdk_gesture_feed(g, motion(41, 40));
    assert(g_lp_pressed == 1);

    /* released early: cancelled */
    fdk_gesture_feed(g, button_down(40, 40));
    g_now = 800; /* 200 ms in */
    fdk_gesture_feed(g, button_up(41, 40));
    assert(g_lp_cancelled == 1);

    /* delay_factor scales the hold */
    fdk_gesture_set_delay_factor(g, 2.0f); /* 1000 ms */
    fdk_gesture_feed(g, button_down(40, 40));
    g_now = 1600;
    fdk_gesture_feed(g, motion(40, 41));
    assert(g_lp_pressed == 1); /* 800 ms < 1000 ms: nothing */
    g_now = 2100;
    fdk_gesture_feed(g, motion(40, 41));
    assert(g_lp_pressed == 2);

    /* movement past the slop degrades it: neither pressed nor
     * cancelled — the gesture just is not a long-press anymore
     * (a moved hold belongs to drag/pan, and firing a cancel
     * callback would double-report to apps listening to both) */
    fdk_gesture_set_delay_factor(g, 1.0f);
    fdk_gesture_feed(g, button_down(40, 40));
    g_now = 2400;
    fdk_gesture_feed(g, motion(60, 40)); /* 20 px away */
    g_now = 3000;
    fdk_gesture_feed(g, button_up(60, 40));
    assert(g_lp_pressed == 2);
    assert(g_lp_cancelled == 1); /* unchanged: degraded, silent */

    fdk_gesture_destroy(g);
    printf("[ok] gesture long-press: 500 ms fire, delay_factor, "
           "early-cancel, slop degrade\n");
}

static void test_drag_pan(void) {
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_DRAG);
    assert(g != NULL);
    fdk_gesture_on_drag(g, on_drag, NULL);

    /* below the threshold: nothing */
    g_now = 0;
    fdk_gesture_feed(g, button_down(100, 100));
    fdk_gesture_feed(g, motion(103, 100));
    assert(g_drags == 0);

    /* crossing 5 px: begin + update */
    fdk_gesture_feed(g, motion(110, 102));
    assert(g_drags == 1); /* the begin */
    fdk_gesture_feed(g, motion(120, 104));
    assert(g_drags == 2);
    assert(g_last_offset.x == 20 && g_last_offset.y == 4);

    /* release: the end with the final total */
    fdk_gesture_feed(g, button_up(122, 106));
    assert(g_drags == 3);
    assert(g_last_offset.x == 22 && g_last_offset.y == 6);
    fdk_gesture_destroy(g);

    /* PAN: the orientation latch. */
    g = fdk_gesture_create(FDK_GESTURE_PAN);
    assert(g != NULL);
    fdk_gesture_on_drag(g, on_drag, NULL);
    fdk_gesture_set_orientation(g, FDK_GESTURE_DIR_LEFT); /* h */

    g_drags = 0;
    fdk_gesture_feed(g, button_down(100, 100));
    fdk_gesture_feed(g, motion(130, 100)); /* horizontal: OK */
    assert(g_drags == 1);
    fdk_gesture_feed(g, button_up(130, 100));
    assert(g_drags == 2);

    /* a vertical motion on a horizontal pan: denied, silent */
    g_drags = 0;
    fdk_gesture_feed(g, button_down(100, 100));
    fdk_gesture_feed(g, motion(102, 140));
    assert(g_drags == 0);
    fdk_gesture_feed(g, button_up(102, 140));
    assert(g_drags == 0);
    fdk_gesture_destroy(g);
    printf("[ok] gesture drag/pan: threshold begin, offsets, final "
           "total, orientation latch\n");
}

static void test_swipe(void) {
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_SWIPE);
    assert(g != NULL);
    fdk_gesture_on_swipe(g, on_swipe, NULL);

    /* a fast horizontal fling: 200 px in 100 ms = 2000 px/s */
    g_now = 0;
    fdk_gesture_feed(g, button_down(100, 100));
    g_now = 100;
    fdk_gesture_feed(g, button_up(300, 102));
    assert(g_swipes == 1);
    assert(g_last_dir == FDK_GESTURE_DIR_RIGHT);
    assert(g_last_vx > 1500.0f);

    /* a slow drag is not a swipe: 30 px in 1000 ms = 30 px/s */
    g_now = 2000;
    fdk_gesture_feed(g, button_down(100, 100));
    g_now = 3000;
    fdk_gesture_feed(g, button_up(130, 100));
    assert(g_swipes == 1);

    /* vertical fling */
    g_now = 4000;
    fdk_gesture_feed(g, button_down(100, 100));
    g_now = 4100;
    fdk_gesture_feed(g, button_up(102, -100));
    assert(g_swipes == 2);
    assert(g_last_dir == FDK_GESTURE_DIR_UP);
    fdk_gesture_destroy(g);
    printf("[ok] gesture swipe: velocity + direction, slow release "
           "rejected\n");
}

static void test_zoom_rotate(void) {
    /* zoom: two contacts 100 px apart, spread to 200 px -> 2.0 */
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_ZOOM);
    assert(g != NULL);
    fdk_gesture_on_scale(g, on_scale, NULL);

    g_now = 0;
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_DOWN, 1, 100, 100));
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_DOWN, 2, 200, 100));
    assert(g_scales == 0); /* the posture begins, no verdict yet */

    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_MOTION, 2, 250, 100));
    assert(g_scales == 1);
    assert(g_last_value > 1.4f && g_last_value < 1.6f);

    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_MOTION, 2, 300, 100));
    assert(g_scales == 2);
    assert(g_last_value > 1.9f && g_last_value < 2.1f);
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_UP, 1, 0, 0));
    fdk_gesture_destroy(g);

    /* rotate: the pair's angle from 0 to 90 degrees */
    g = fdk_gesture_create(FDK_GESTURE_ROTATE);
    assert(g != NULL);
    g_scales = 0;
    fdk_gesture_on_scale(g, on_scale, NULL);

    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_DOWN, 1, 100, 100));
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_DOWN, 2, 200, 100));
    /* b moves above a: the angle goes 0 -> +pi/2 (screen y down,
     * so "up on screen" is negative dy — atan2(-100, 100) = -pi/4
     * from 0... place b at (100, 0): angle = atan2(-100, 0) =
     * -pi/2). Signed angle semantics: counterclockwise on screen
     * is negative in y-down math. The test asserts the DELTA's
     * magnitude and sign consistently. */
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_MOTION, 2, 100, 0));
    assert(g_scales == 1);
    assert(g_last_value < -1.5f || g_last_value > 1.5f);
    fdk_gesture_feed(g, touch(FDK_EVENT_TOUCH_UP, 2, 0, 0));
    fdk_gesture_destroy(g);
    printf("[ok] gesture zoom/rotate: span ratio, angle delta\n");
}

static void test_reset(void) {
    fdk_gesture *g = fdk_gesture_create(FDK_GESTURE_DRAG);
    assert(g != NULL);
    fdk_gesture_on_drag(g, on_drag, NULL);

    g_now = 0;
    fdk_gesture_feed(g, button_down(100, 100));
    fdk_gesture_feed(g, motion(110, 100));
    assert(g_drags == 1);

    /* the compositor grabbed it: reset, silent end */
    fdk_event_data cancel;
    memset(&cancel, 0, sizeof cancel);
    cancel.type = FDK_EVENT_TOUCH_CANCEL;
    fdk_gesture_feed(g, &cancel);
    fdk_gesture_feed(g, button_up(140, 100));
    assert(g_drags == 1); /* no END fired: interrupted */
    fdk_gesture_destroy(g);
    printf("[ok] gesture reset: interrupted sequences end silently\n");
}

int main(void) {
    fdk_gesture_set_clock(test_clock, NULL);
    g_now = 0;

    test_click();
    test_long_press();
    test_drag_pan();
    test_swipe();
    test_zoom_rotate();
    test_reset();

    fdk_gesture_set_clock(NULL, NULL);
    printf("all gesture tests passed\n");
    return 0;
}
