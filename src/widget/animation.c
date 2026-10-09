#define FDK_LOG_TAG "widgets"

/*
 * animation.c — the easing library + the animator engine (1.3.8)
 *
 * One process-wide list of active animations, one shared ~16 ms
 * repeating timer (created when the first animation starts, removed
 * when the list empties — an idle toolkit costs nothing), and one
 * pump that walks the list. The pump is the ONLY place animations
 * are freed and the only place tick/done callbacks run outside the
 * public entry points — single-owner lifetime, the same discipline
 * as the deferred-destroy queue.
 *
 * THE TEST SEAM is the pump itself: fdk__animation_pump(now_ms) is
 * exactly what the production ticker calls (with the monotonic
 * clock), and the headless suite calls it with synthetic times —
 * the whole machine (cadence, easing application, completion,
 * cancel-inside-tick, widget-detach) runs identically under both
 * drivers. Production never calls the seam directly; tests never
 * need a display.
 *
 * WHY A GLOBAL LIST rather than per-widget storage: the destroy
 * sweep must find every animation in a dying subtree WITHOUT
 * walking the tree (teardown frees children bottom-up through
 * pointers that change under it) — a global list makes the sweep
 * one O(active) pass of parent-chain walks, and the common case
 * (no animations at all) is a NULL check.
 */

#include "widgets_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_animation.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>

/* ---- easing ------------------------------------------------------------ */

double fdk_ease_apply(fdk_ease_kind kind, double t) {
    if (t < 0.0) {
        t = 0.0;
    } else if (t > 1.0) {
        t = 1.0;
    }
    switch (kind) {
    case FDK_EASE_SMOOTHSTEP:
        /* hermite: 3t^2 - 2t^3 — the "does everything politely"
         * curve; the default recommendation. */
        return t * t * (3.0 - 2.0 * t);
    case FDK_EASE_QUAD_IN:
        return t * t;
    case FDK_EASE_QUAD_OUT:
        return 1.0 - (1.0 - t) * (1.0 - t);
    case FDK_EASE_QUAD_IN_OUT: {
        /* piecewise parabola, mirrored at the midpoint. */
        if (t < 0.5) {
            return 2.0 * t * t;
        }
        return 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    }
    case FDK_EASE_CUBIC_IN:
        return t * t * t;
    case FDK_EASE_CUBIC_OUT:
        return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    case FDK_EASE_CUBIC_IN_OUT: {
        if (t < 0.5) {
            return 4.0 * t * t * t;
        }
        double u = 1.0 - t;
        return 1.0 - 4.0 * u * u * u;
    }
    case FDK_EASE_SINE_IN_OUT:
        /* half a cosine dip: (1 - cos(pi t)) / 2. */
        return 0.5 - 0.5 * cos(3.14159265358979323846 * t);
    case FDK_EASE_EXPO_OUT: {
        /* fast rise, long tail: 1 - 2^(-10t), pinned to exactly 1
         * at the end (the formula only reaches ~0.999 there — the
         * boundary identity e(1)=1 is a contract, not a limit).
         * (The first draft inverted the exponent's argument and
         * returned ~0.999 at t=0 and 0 at t=1 — caught by the
         * easing suite's boundary identities the day it was
         * written.) */
        if (t >= 1.0) {
            return 1.0;
        }
        if (t <= 0.0) {
            return 0.0;
        }
        return 1.0 - exp2(-10.0 * t);
    }
    case FDK_EASE_BACK_OUT: {
        /* the reveal overshoot: overshoots to ~1.1 at t ~ 0.7,
         * settles at exactly 1. s = 1.70158 is the classic
         * "back" constant (the 10% overshoot). */
        const double s = 1.70158;
        double u = t - 1.0;
        return 1.0 + u * u * ((s + 1.0) * u + s);
    }
    case FDK_EASE_LINEAR:
    default:
        /* Unknown kinds degenerate to linear — a math function
         * never fails (the header's contract). */
        return t;
    }
}

/* ---- the engine --------------------------------------------------------- */

struct fdk_animation {
    fdk_widget *widget; /* NULLed by the destroy sweep */
    fdk_u32 duration_ms;
    fdk_ease_kind ease;
    fdk_animation_tick_fn tick;
    fdk_animation_done_fn done;
    void *user;
    long long start_ms; /* monotonic */
    bool finished;      /* completed, canceled, or detached */
    /* Zombie reaping (see the lifetime block below): the pump
     * number at which this finished handle may be freed. */
    unsigned long long reap_at;
    fdk_animation *prev, *next; /* the global active list */
};

static fdk_animation *g_list;      /* running animations */
static fdk_animation *g_zombies;  /* finished, still queryable */
static unsigned long long g_pumps; /* monotonic pump count */

/* LIFETIME, THE WHOLE CONTRACT IN ONE PLACE. The public header
 * promises "a handle past its animation is a query object, not a
 * dangling pointer" — honoring that literally (freeing at
 * completion) is a USE-AFTER-FREE factory: the pump frees inside
 * its own walk while callers hold the handle, and every
 * query-after-done pattern dies (found live by ASan the day this
 * engine was written: fdk_animation_running() on a completed handle
 * read freed memory). The working design is zombie grace:
 *
 *   - A completed / canceled / detached animation is UNLINKED from
 *     the active list and moved to the zombie list. It stays
 *     allocated: running() says false, widget() keeps answering
 *     (NULL after a destroy sweep), cancel() is a no-op.
 *   - Zombies are REAPED (freed) at pump exit once GRACE_PUMPS
 *     pump calls have passed since their animation ended — about a
 *     second at display cadence. Queries shortly after done() are
 *     safe; a handle is not a long-term storage object.
 *   - A zombie whose widget was destroyed underneath it (widget ==
 *     NULL) is reaped at the NEXT pump — nobody can be animating a
 *     dead widget, so nobody is querying for it either.
 *   - When the LAST widget root is destroyed, everything drains
 *     (no callbacks, frees both lists): no widget exists, so no
 *     animation can ever run again, and process exit is ASan-clean
 *     (the compositor-death rig checks exactly that).
 *
 * The pump is the ONLY caller of reaping, and it never frees
 * anything mid-walk — which is also why callbacks may cancel their
 * own or their neighbors' animations freely: the walk's captured
 * successors stay valid (zombies are still allocated). */
#define GRACE_PUMPS 64u /* ~1 s of display cadence */

/* The shared ticker. Attached to the context of the FIRST
 * animation's widget; a second context's animations ride the same
 * tick (the pump is global — it only needs SOMEONE pumping). One
 * timer for every animation also means one wakeup per frame no
 * matter how many things move, and every invalidation lands before
 * the app's next paint — coalesced damage, not interleaved paints. */
#define TICKER_INTERVAL_MS 16u
static fdk_timer *g_ticker;

static long long g_clock_override = -1; /* test seam; -1 = real clock */

static long long anim_now_ms(void) {
    if (g_clock_override >= 0) {
        return g_clock_override;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL +
           (long long)ts.tv_nsec / 1000000LL;
}

static void ticker_fn(fdk_timer *timer, void *user) {
    (void)timer;
    (void)user;
    fdk__animation_pump(anim_now_ms());
}

static void ticker_start(fdk_widget *widget) {
    if (g_ticker != NULL) {
        return; /* already ticking */
    }
    void *owner = fdk__widget_window_owner(widget);
    if (owner == NULL) {
        return; /* standalone tree: the test pump drives it */
    }
    fdk_context *ctx = fdk__window_context(owner);
    if (ctx == NULL) {
        return;
    }
    g_ticker = fdk_timer_add(ctx, TICKER_INTERVAL_MS, true,
                             ticker_fn, NULL);
}

static void ticker_stop(void) {
    if (g_ticker != NULL) {
        fdk_timer_remove(g_ticker);
        g_ticker = NULL;
    }
}

/* Unlinks from the active list (when still linked) and pushes onto
 * the zombie list. The single post-animation transition. */
static void anim_zombify(fdk_animation *a) {
    if (a->prev != NULL || g_list == a) {
        if (a->prev != NULL) {
            a->prev->next = a->next;
        } else {
            g_list = a->next;
        }
        if (a->next != NULL) {
            a->next->prev = a->prev;
        }
        a->prev = NULL;
        a->next = NULL;
    }
    a->reap_at = g_pumps + GRACE_PUMPS;
    a->next = g_zombies;
    g_zombies = a;
}



fdk_animation *fdk_widget_animate(fdk_widget *widget,
                                  fdk_u32 duration_ms,
                                  fdk_ease_kind ease,
                                  fdk_animation_tick_fn on_tick,
                                  fdk_animation_done_fn on_done,
                                  void *user_data) {
    if (widget == NULL || on_tick == NULL) {
        return NULL;
    }
    fdk_animation *a = fdk_alloc(sizeof *a);
    if (a == NULL) {
        return NULL;
    }
    a->widget = widget;
    a->duration_ms = duration_ms;
    a->ease = ease;
    a->tick = on_tick;
    a->done = on_done;
    a->user = user_data;
    a->start_ms = anim_now_ms();
    a->finished = false;
    a->prev = NULL;
    a->next = g_list;
    if (g_list != NULL) {
        g_list->prev = a;
    }
    g_list = a;
    ticker_start(widget);
    return a;
}

void fdk_animation_cancel(fdk_animation *anim) {
    if (anim == NULL || anim->finished) {
        return;
    }
    anim->finished = true;
    if (anim->done != NULL) {
        anim->done(anim, false, anim->user);
    }
    anim_zombify(anim);
}

fdk_widget *fdk_animation_widget(const fdk_animation *anim) {
    return (anim != NULL) ? anim->widget : NULL;
}

bool fdk_animation_running(const fdk_animation *anim) {
    return anim != NULL && !anim->finished && anim->widget != NULL;
}

/* ---- the pump ------------------------------------------------------------
 * The single owner of frame delivery and reaping. Walks the active
 * list with the cursor's successor captured BEFORE any callback (a
 * tick may cancel itself, its neighbors, or start new animations —
 * all of which only ever prepend, so the captured successor is
 * always still valid and the walk terminates; zombified entries
 * stay allocated, which is exactly why the walk is safe to read
 * `a->finished` after the callback returns). */

void fdk__animation_pump(long long now_ms) {
    g_pumps++;

    fdk_animation *a = g_list;
    while (a != NULL) {
        fdk_animation *next = a->next;
        double t = 1.0;
        if (a->duration_ms > 0) {
            t = (double)(now_ms - a->start_ms) /
                (double)a->duration_ms;
            if (t < 0.0) {
                t = 0.0; /* clock went backwards; clamp */
            } else if (t > 1.0) {
                t = 1.0;
            }
        }
        double e = fdk_ease_apply(a->ease, t);
        a->tick(a, e, a->user);
        if (!a->finished && a->widget != NULL) {
            /* The damage lands AFTER the callback moved whatever it
             * moves — one region per frame, coalesced by the
             * tracker. (tick may have canceled a: finished, skip.) */
            fdk_widget_invalidate(a->widget);
            if (t >= 1.0) {
                a->finished = true;
                if (a->done != NULL) {
                    a->done(a, true, a->user);
                }
                anim_zombify(a);
            }
        }
        a = next;
    }

    /* Reaping: zombies whose grace has passed, and every zombie
     * whose widget died (nothing can be querying for a dead
     * widget's animation — the app was told the handle is a query
     * object, not a keepsake). */
    fdk_animation **link = &g_zombies;
    while (*link != NULL) {
        fdk_animation *z = *link;
        if (z->reap_at <= g_pumps || z->widget == NULL) {
            *link = z->next;
            fdk_free(z);
        } else {
            link = &z->next;
        }
    }

    /* The ticker sleeps when nothing is RUNNING. Zombies are inert
     * memory — they need no ticks, only eventual reaping, which the
     * app's own next pump performs (a live loop pumps continuously,
     * so the documented "~1 s of animation traffic" handle grace is
     * unchanged in practice; a quiet app simply keeps its zombie a
     * little longer — never shorter). Keeping the ticker alive
     * through the grace would leave a 16 ms repeating timer firing
     * ~64 times after the last animation ends, and every fire wakes
     * the app's pump at the timer deadline — a 50 ms pump budget
     * becomes ~16 ms of wall clock. Found live in 1.4.1: the hover
     * fade (the first animation the Wayland suite ever ran) armed
     * the ticker, and the pacing test's "400 ms" of pumping shrank
     * to ~128 ms — not enough for the compositor's frame callbacks
     * to come back. */
    if (g_list == NULL) {
        ticker_stop();
    }
}

/* The full drain: no callbacks, everything freed. Called when the
 * LAST widget root is destroyed (no widget exists -> no animation
 * can ever run again -> the engine's state is dead weight that
 * would otherwise show up as leaks at process exit, which the
 * compositor-death rig checks) and from the test suites. */
void fdk__animation_drain_all(void) {
    while (g_list != NULL) {
        fdk_animation *a = g_list;
        g_list = a->next;
        fdk_free(a);
    }
    while (g_zombies != NULL) {
        fdk_animation *z = g_zombies;
        g_zombies = z->next;
        fdk_free(z);
    }
    ticker_stop();
}

/* The destroy sweep: every animation whose widget is INSIDE `w`'s
 * subtree is dropped WITHOUT callbacks (a dying widget must not be
 * invalidated by an animator mid-funeral — the use-after-free
 * guard the header documents). Called from teardown_free, which
 * every destroy path funnels through, BEFORE the subtree is
 * freed. The membership test walks the PARENT CHAIN (the one
 * truth — bounds rects can coincide for overlapping siblings, a
 * detached widget, or a second standalone root at the same
 * coordinates; parent pointers cannot lie, and they are still
 * intact at this point in the teardown). */
void fdk__animation_detach_subtree(fdk_widget *w) {
    if (g_list == NULL || w == NULL) {
        return;
    }
    fdk_animation *a = g_list;
    while (a != NULL) {
        fdk_animation *next = a->next;
        bool inside = false;
        for (fdk_widget *p = a->widget; p != NULL; p = p->parent) {
            if (p == w) {
                inside = true;
                break;
            }
        }
        if (inside) {
            a->widget = NULL;
            a->finished = true;
            anim_zombify(a); /* reaped at the next pump: dead widget */
        }
        a = next;
    }
}

/* ---- internal test seam exposure -------------------------------------- */

/* The headless suite's clock: while an override is set,
 * fdk_widget_animate() stamps starts from it and the suite drives
 * fdk__animation_pump() with matching synthetic times — the whole
 * machine runs on one clock with no display and no sleeps.
 * Production never calls either function (the ticker uses the real
 * clock; the override is -1). */
void fdk__animation_set_test_clock(long long fixed_now_ms) {
    g_clock_override = fixed_now_ms;
}
