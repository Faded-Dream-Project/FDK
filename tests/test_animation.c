/* test_animation.c — headless animation-engine tests (1.3.8).
 *
 * Everything runs on the synthetic clock: the engine's test seam
 * pins fdk_widget_animate()'s start stamping and the suite drives
 * fdk__animation_pump() at chosen times — the exact code path the
 * production ~16 ms ticker drives, with no display and no sleeps.
 * Widgets are standalone roots (the established discipline).
 *
 * What is proven here:
 *   - every easing curve: boundary identities e(0)=0 / e(1)=1,
 *     monotonicity where promised, the back_out overshoot (> 1
 *     mid-flight, settling exactly on 1), input clamping, and the
 *     unknown-kind-degrades-to-linear contract
 *   - animator lifecycle: nothing moves on the starting frame's
 *     event, exact eased values at chosen times, completion fires
 *     done(finished = true) exactly once
 *   - duration 0 completes on the first pump
 *   - cancel: done(finished = false) exactly once, the handle is
 *     dead afterwards, double-cancel is a quiet no-op
 *   - tick-cancels-itself: legal, the animation stops, the pump
 *     keeps walking its siblings (the captured-successor rule)
 *   - done-restarts: a new animation started from done() first
 *     ticks on the NEXT pump, never re-entrantly
 *   - widget destroy mid-flight: NO callbacks run against the dead
 *     widget (the use-after-free guard), the engine's list drains,
 *     and a sibling animation OUTSIDE the destroyed subtree survives
 *   - argument safety: NULL widget / NULL tick / NULL handle
 *   - fdk_animation_running / fdk_animation_widget queries, before
 *     and after every transition
 */

#include "fdk/fdk.h"
#include "fdk/fdk_animation.h"

#include "widget/widget_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- the synthetic clock ------------------------------------------------ */

static long long g_time;

static void at(long long t) {
    g_time = t;
    fdk__animation_set_test_clock(t);
    fdk__animation_pump(t);
}



/* ---- easing ------------------------------------------------------------ */

static void test_easing(void) {
    static const fdk_ease_kind kinds[] = {
        FDK_EASE_LINEAR,     FDK_EASE_SMOOTHSTEP, FDK_EASE_QUAD_IN,
        FDK_EASE_QUAD_OUT,   FDK_EASE_QUAD_IN_OUT, FDK_EASE_CUBIC_IN,
        FDK_EASE_CUBIC_OUT,  FDK_EASE_CUBIC_IN_OUT,
        FDK_EASE_SINE_IN_OUT, FDK_EASE_EXPO_OUT,
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        fdk_ease_kind k = kinds[i];
        /* Boundary identities: every curve starts at 0 and lands
         * exactly at 1 (bitwise — the landing must be exact, not
         * epsilon-close, or animations end at 0.9999 of the way). */
        assert(fdk_ease_apply(k, 0.0) == 0.0);
        assert(fdk_ease_apply(k, 1.0) == 1.0);
        /* Input clamping: outside [0,1] pins to the boundaries. */
        assert(fdk_ease_apply(k, -0.5) == 0.0);
        assert(fdk_ease_apply(k, 1.5) == 1.0);
        /* Monotonicity: a sample walk never goes backwards. */
        double prev = 0.0;
        for (int s = 1; s <= 20; s++) {
            double v = fdk_ease_apply(k, s / 20.0);
            assert(v >= prev - 1e-12);
            assert(v <= 1.0);
            prev = v;
        }
    }

    /* Spot values that pin the actual curves (regression guards
     * against silently swapping a formula). */
    assert(fdk_ease_apply(FDK_EASE_LINEAR, 0.25) == 0.25);
    assert(fdk_ease_apply(FDK_EASE_QUAD_IN, 0.5) == 0.25);
    assert(fdk_ease_apply(FDK_EASE_QUAD_OUT, 0.5) == 0.75);
    assert(fdk_ease_apply(FDK_EASE_CUBIC_OUT, 0.5) == 0.875);
    assert(fdk_ease_apply(FDK_EASE_SMOOTHSTEP, 0.5) == 0.5);
    /* Sine at the midpoint: mathematically exactly 0.5 (cos(pi/2)
     * = 0), within floating-point tolerance of the constant pi. */
    assert(fdk_ease_apply(FDK_EASE_SINE_IN_OUT, 0.5) > 0.499999 &&
           fdk_ease_apply(FDK_EASE_SINE_IN_OUT, 0.5) < 0.500001);

    /* back_out: the overshoot curve — strictly above 1 somewhere
     * mid-flight, back to EXACTLY 1 at the end. */
    double peak = 0.0;
    for (int s = 0; s <= 100; s++) {
        double v = fdk_ease_apply(FDK_EASE_BACK_OUT, s / 100.0);
        if (v > peak) {
            peak = v;
        }
    }
    assert(peak > 1.05 && peak < 1.2); /* ~1.098 for s = 1.70158 */
    assert(fdk_ease_apply(FDK_EASE_BACK_OUT, 1.0) == 1.0);

    /* Unknown kinds degrade to linear — the math function never
     * fails, it degenerates (the header's contract). */
    assert(fdk_ease_apply((fdk_ease_kind)999, 0.25) == 0.25);
    assert(fdk_ease_apply((fdk_ease_kind)-1, 0.75) == 0.75);

    printf("[ok] animation: easing table (boundary identities, "
           "monotonicity, clamping, the back_out overshoot, "
           "unknown-kind = linear)\n");
}

/* ---- animator lifecycle ------------------------------------------------- */

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 100, 80},
                                    &root)));
    return root;
}

typedef struct {
    double last_e;
    int ticks;
    int done_true;
    int done_false;
} anim_log;

static void log_tick(fdk_animation *a, double e, void *user) {
    (void)a;
    anim_log *l = user;
    l->last_e = e;
    l->ticks++;
}

static void log_done(fdk_animation *a, bool finished, void *user) {
    (void)a;
    anim_log *l = user;
    if (finished) {
        l->done_true++;
    } else {
        l->done_false++;
    }
}

static void test_lifecycle(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();

    /* Nothing moves on the start call itself: the first tick is the
     * next frame (a teleporting "animation" is a bug, not a
     * feature). */
    anim_log l = { 0, 0, 0, 0 };
    fdk_animation *a = fdk_widget_animate(root, 100,
                                          FDK_EASE_CUBIC_OUT,
                                          log_tick, log_done, &l);
    assert(a != NULL);
    assert(fdk_animation_running(a));
    assert(fdk_animation_widget(a) == root);
    assert(l.ticks == 0);

    /* At exactly half a 100 ms cubic-out flight: e = 0.875. */
    at(50);
    assert(l.ticks == 1);
    assert(l.last_e > 0.874 && l.last_e < 0.876);
    assert(fdk_animation_running(a));

    /* Past the end: the last tick delivered e = 1 exactly, done
     * fired ONCE with finished = true, and the handle reports
     * stopped. */
    at(200);
    assert(l.last_e == 1.0);
    assert(l.done_true == 1);
    assert(l.done_false == 0);
    assert(!fdk_animation_running(a));
    /* Pumps after completion tick nothing and call nothing: the
     * empty-engine pump is a no-op (and the zombie handle still
     * answers queries safely — the grace window). */
    at(400);
    assert(l.ticks == 2); /* the half-flight tick + the final tick */
    at(600);
    assert(l.ticks == 2);
    assert(!fdk_animation_running(a)); /* zombie: queryable, dead */
    fdk_animation_cancel(a);           /* zombie: quiet no-op */
    assert(l.done_true == 1);          /* no second done */

    fdk_widget_destroy(root);
    printf("[ok] animation: lifecycle (no motion on start, exact "
           "eased values, done(true) exactly once, dead handles "
           "stay dead)\n");
}

static void test_duration_zero(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    anim_log l = { 0, 0, 0, 0 };
    fdk_animation *a = fdk_widget_animate(root, 0, FDK_EASE_LINEAR,
                                          log_tick, log_done, &l);
    assert(a != NULL);
    /* Duration 0: the very first pump completes it (e = 1). */
    at(16);
    assert(l.ticks == 1);
    assert(l.last_e == 1.0);
    assert(l.done_true == 1);
    assert(!fdk_animation_running(a));
    fdk_widget_destroy(root);
    printf("[ok] animation: duration 0 completes on the first "
           "pump\n");
}

static void test_cancel(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    anim_log l = { 0, 0, 0, 0 };
    fdk_animation *a = fdk_widget_animate(root, 100, FDK_EASE_LINEAR,
                                          log_tick, log_done, &l);
    at(20);
    assert(l.ticks == 1);
    fdk_animation_cancel(a);
    assert(l.done_false == 1);
    assert(!fdk_animation_running(a));
    /* Double-cancel: quiet no-op, done never fires twice. */
    fdk_animation_cancel(a);
    fdk_animation_cancel(NULL);
    assert(l.done_false == 1);
    /* The pump after a cancel runs NOTHING for the dead handle. */
    at(300);
    assert(l.ticks == 1);
    fdk_widget_destroy(root);
    printf("[ok] animation: cancel (done(false) once, double-cancel "
           "quiet, nothing after death)\n");
}

/* Tick cancels its own animation: legal, and the pump must keep
 * walking SIBLINGS started after it (the captured-successor rule —
 * the walk must not lose the rest of the list when a callback
 * mutates it). */
typedef struct {
    fdk_animation *self;
    anim_log *sibling_log;
} self_cancel_ctx;

static void self_cancel_tick(fdk_animation *a, double e, void *user) {
    self_cancel_ctx *c = user;
    (void)e;
    fdk_animation_cancel(a);
    c->self = NULL;
}

static void test_tick_cancels_self(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    fdk_widget *child = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 40, 20},
                                    &child)));

    self_cancel_ctx ctx = { NULL, NULL };
    fdk_animation *a = fdk_widget_animate(root, 100, FDK_EASE_LINEAR,
                                          self_cancel_tick, NULL,
                                          &ctx);
    anim_log sib = { 0, 0, 0, 0 };
    /* The sibling is appended AFTER a: cancelling a inside its own
     * tick must not lose the sibling from the walk. */
    fdk_animation *b = fdk_widget_animate(child, 100, FDK_EASE_LINEAR,
                                          log_tick, log_done, &sib);
    (void)b;
    at(50);
    /* a canceled itself on its first tick; the sibling still
     * ticked at 50 and will complete at 100. */
    assert(!fdk_animation_running(a));
    assert(sib.ticks >= 1);
    at(300);
    assert(sib.done_true == 1);
    fdk_widget_destroy(root);
    printf("[ok] animation: tick-cancels-itself (legal; the walk "
           "keeps the siblings started after it)\n");
}

/* done() restarting: the new animation first ticks on the NEXT
 * pump, never re-entrantly inside the completing callback. The
 * chain restarts itself twice (every animation in the chain carries
 * the same restarting done callback — a real ping-pong pattern),
 * and every tick of every member is counted together. */
typedef struct {
    int restarts;    /* animations STARTED from done() */
    int completions; /* finished = true deliveries */
    int ticks;       /* every tick of every chain member */
} restart_ctx;

static void chain_tick(fdk_animation *a, double e, void *user) {
    (void)a;
    (void)e;
    restart_ctx *rc = user;
    rc->ticks++;
}

static void chain_done(fdk_animation *a, bool finished, void *user) {
    restart_ctx *rc = user;
    if (finished) {
        rc->completions++;
        if (rc->restarts < 2) {
            rc->restarts++;
            int before = rc->ticks;
            fdk_animation *na = fdk_widget_animate(
                fdk_animation_widget(a), 50, FDK_EASE_LINEAR,
                chain_tick, chain_done, rc);
            assert(na != NULL);
            /* Re-entrancy guard: the restart must NOT tick
             * immediately (the pump is mid-walk on the stack). */
            assert(rc->ticks == before);
        }
    }
}

static void test_restart_from_done(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    restart_ctx rc = { 0, 0, 0 };
    fdk_animation *a = fdk_widget_animate(root, 40, FDK_EASE_LINEAR,
                                          chain_tick, chain_done,
                                          &rc);
    (void)a;
    at(100); /* the original completes; restart #1 starts (50 ms) */
    assert(rc.completions == 1);
    assert(rc.restarts == 1);
    assert(rc.ticks == 1); /* the original's completing tick — the
                            * restart itself did NOT tick this pump */
    at(120); /* restart #1 mid-flight */
    assert(rc.ticks == 2);
    at(200); /* restart #1 done -> restart #2 (50 ms) */
    assert(rc.completions == 2);
    assert(rc.restarts == 2);
    at(220); /* restart #2 mid-flight */
    assert(rc.ticks == 4);
    at(400); /* restart #2 done; the cap means no restart #3 */
    assert(rc.completions == 3);
    assert(rc.restarts == 2);
    assert(rc.ticks == 5);
    fdk_widget_destroy(root);
    printf("[ok] animation: restart-from-done (chained restarts, "
           "next-pump rule, never re-entrant)\n");
}

/* Widget destroy mid-flight: no callback runs against the dying
 * widget (the use-after-free guard), and a sibling OUTSIDE the
 * destroyed subtree keeps animating. */
static void test_destroy_detach(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    fdk_widget *doomed = NULL;
    fdk_widget *outsider = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 40, 20},
                                    &doomed)));
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 20, 40, 20},
                                    &outsider)));

    anim_log dl = { 0, 0, 0, 0 };
    anim_log ol = { 0, 0, 0, 0 };
    fdk_animation *da = fdk_widget_animate(doomed, 100,
                                           FDK_EASE_LINEAR,
                                           log_tick, log_done, &dl);
    fdk_animation *oa = fdk_widget_animate(outsider, 100,
                                           FDK_EASE_LINEAR,
                                           log_tick, log_done, &ol);
    at(30);
    assert(dl.ticks == 1 && ol.ticks == 1);

    /* Kill the doomed SUBTREE (a plain child, destroyed directly).
     * Neither tick nor done may run for it afterwards. */
    fdk_widget_destroy(doomed);
    assert(!fdk_animation_running(da));
    assert(fdk_animation_widget(da) == NULL);
    int dl_ticks_after_destroy = dl.ticks;
    at(200);
    assert(dl.ticks == dl_ticks_after_destroy);
    assert(dl.done_true == 0 && dl.done_false == 0);
    /* The outsider was never inside the destroyed subtree: it
     * completed normally. */
    assert(ol.done_true == 1);
    (void)oa;

    /* Destroying the whole ROOT while animations on its children
     * are airborne: the teardown sweep drops them without any
     * callback (observable: done_true stays 0 after further pumps),
     * and destroying the LAST root shuts the engine down entirely —
     * handles are dead from that point (the shutdown rule the
     * header documents), so the observable is the callback log, not
     * the handle. */
    anim_log rl = { 0, 0, 0, 0 };
    at(1000); /* a fresh clock base for the flight under test */
    fdk_animation *ra = fdk_widget_animate(outsider, 100,
                                           FDK_EASE_LINEAR,
                                           log_tick, log_done, &rl);
    (void)ra;
    at(1030);
    assert(rl.ticks == 1);
    fdk_widget_destroy(root);
    at(1200);
    assert(rl.ticks == 1);      /* no tick against the dead tree */
    assert(rl.done_true == 0);  /* and no done either */

    printf("[ok] animation: destroy mid-flight (no callbacks against "
           "the dead widget; outsiders and root-destroy sweeps "
           "covered)\n");
}

static void test_argument_safety(void) {
    fdk_animation *a = fdk_widget_animate(NULL, 100, FDK_EASE_LINEAR,
                                          log_tick, NULL, NULL);
    assert(a == NULL);
    fdk_widget *root = fresh_root();
    a = fdk_widget_animate(root, 100, FDK_EASE_LINEAR, NULL, NULL,
                           NULL);
    assert(a == NULL); /* an animation that observes nothing is a
                        * timer — and FDK has timers */
    assert(!fdk_animation_running(NULL));
    assert(fdk_animation_widget(NULL) == NULL);
    fdk_animation_cancel(NULL); /* documented no-op */
    fdk__animation_pump(0); /* empty engine: safe */
    fdk__animation_detach_subtree(NULL); /* safe */
    fdk__animation_detach_subtree(root); /* no animations: safe */
    fdk_widget_destroy(root);
    printf("[ok] animation: argument safety (NULL widget/tick/"
           "handle; empty-engine pump)\n");
}

/* Multiple concurrent animations on different widgets, interleaved
 * starts and finishes — the one-ticker-many-animations shape. */
static void test_concurrent(void) {
    g_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    fdk_widget *w1 = NULL, *w2 = NULL, *w3 = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 40, 20}, &w1)));
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 20, 40, 20}, &w2)));
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 40, 40, 20}, &w3)));

    anim_log l1 = { 0, 0, 0, 0 }, l2 = { 0, 0, 0, 0 },
             l3 = { 0, 0, 0, 0 };
    fdk_animation *a1 = fdk_widget_animate(w1, 100, FDK_EASE_LINEAR,
                                            log_tick, log_done, &l1);
    fdk_animation *a2 = fdk_widget_animate(w2, 200, FDK_EASE_LINEAR,
                                           log_tick, log_done, &l2);
    (void)a1;
    (void)a2;
    at(50); /* a1 half, a2 quarter */
    assert(l1.ticks == 1 && l2.ticks == 1);
    assert(l1.last_e == 0.5);
    assert(l2.last_e == 0.25);
    /* a3 joins late. */
    fdk_animation *a3 = fdk_widget_animate(w3, 50, FDK_EASE_LINEAR,
                                           log_tick, log_done, &l3);
    (void)a3;
    at(100); /* a1 completes (e=1), a2 half, a3 completes (started
              * at 50, 50 ms long). */
    assert(l1.done_true == 1);
    assert(l2.last_e == 0.5);
    assert(l3.done_true == 1);
    at(300); /* a2 completes. */
    assert(l2.done_true == 1);
    fdk_widget_destroy(root);
    printf("[ok] animation: concurrent flights (interleaved starts, "
           "independent completions, one pump for all)\n");
}

int main(void) {
    test_easing();
    test_lifecycle();
    test_duration_zero();
    test_cancel();
    test_tick_cancels_self();
    test_restart_from_done();
    test_destroy_detach();
    test_argument_safety();
    test_concurrent();
    fdk__animation_set_test_clock(-1); /* restore the real clock */
    printf("all animation tests passed\n");
    return 0;
}
