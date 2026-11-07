/*
 * fdk_core.h — Faded Dream ToolKit core lifecycle
 *
 * Every FDK application follows this shape:
 *
 *     fdk_context *ctx = NULL;
 *     fdk_result r = fdk_init(&ctx, NULL);
 *     if (!fdk_ok(r)) { ... }
 *
 *     // create windows, widgets, run the event loop...
 *     fdk_run(ctx);
 *
 *     fdk_shutdown(ctx);
 *
 * Threading: the fdk_context and everything reachable from it (windows,
 * widgets, timers) must only be touched from the thread that called
 * fdk_init() — conventionally the "main" or "UI" thread. See
 * docs/threading.md for how to schedule work from other threads onto
 * the UI thread.
 */

#ifndef FDK_CORE_H
#define FDK_CORE_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which platform backend a context is using. FDK_PLATFORM_AUTO (the
 * default) picks Wayland if $WAYLAND_DISPLAY is set and reachable,
 * otherwise X11. Applications rarely need to force a specific backend;
 * this exists mainly for testing and for the (rare) app that has a
 * hard requirement one way or the other. */
typedef enum fdk_platform_backend {
    FDK_PLATFORM_AUTO   = 0,
    FDK_PLATFORM_X11    = 1,
    FDK_PLATFORM_WAYLAND= 2,
} fdk_platform_backend;

/* Initialization options for fdk_init(). Zero-initialize (or pass NULL
 * for defaults) to accept every default: FDK_PLATFORM_AUTO backend,
 * default log level, no explicit application id. */
typedef struct fdk_init_options {
    fdk_platform_backend backend;

    /* Reverse-DNS-style application identifier (e.g.
     * "org.example.myapp"), used where the platform wants one — Wayland
     * app-id, X11 WM_CLASS, etc. May be NULL to use a generated default. */
    const char *app_id;
} fdk_init_options;

/* Initializes FDK and creates a new toolkit context, writing it to
 * *out_ctx on success. `options` may be NULL to accept all defaults.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT  - out_ctx is NULL
 *   FDK_ERR_OUT_OF_MEMORY     - allocation failure
 *   FDK_ERR_PLATFORM_INIT     - platform backend failed to initialize
 *   FDK_ERR_NO_DISPLAY        - no X11/Wayland display reachable
 *
 * On any failure, *out_ctx is left unchanged (not partially initialized). */
fdk_result fdk_init(fdk_context **out_ctx, const fdk_init_options *options);

/* Runs the event loop until fdk_quit() is called or the last top-level
 * window is closed (whichever comes first). Blocks the calling thread.
 * Safe to call fdk_run() again after it returns, as long as shutdown()
 * has not been called.
 *
 * Applications that render animation frames (see fdk_surface.h) should
 * NOT use fdk_run() — it blocks indefinitely waiting for input between
 * events, which leaves no place to draw and present the next frame.
 * Drive the loop yourself with fdk_pump_events() instead. */
void fdk_run(fdk_context *ctx);

/* Waits for platform events for up to `timeout_ms` milliseconds, then
 * dispatches whatever arrived (invoking per-window event callbacks).
 * This is the building block applications use to own their event loop:
 *
 *     while (!done) {
 *         fdk_pump_events(ctx, 15);          // wait up to 15 ms
 *         // ... render frame, fdk_surface_present(surface) ...
 *     }
 *
 * timeout_ms semantics: 0 = poll for pending events without blocking;
 * negative = block indefinitely until something arrives. EINTR during
 * the wait is absorbed and reported as "0 events dispatched".
 *
 * Due timers fire inside this call (after the event drain, in
 * deadline order); their count adds to the return value, so the
 * canonical `pump; paint;` loop repaints timer-driven changes (caret
 * blinks, animations) without any extra plumbing. The wait itself is
 * capped at the next timer deadline: an indefinite timeout_ms (-1)
 * with live timers still wakes up to fire them.
 *
 * Returns the number of events dispatched plus timers fired (>= 0),
 * or a negative fdk_result code on failure — FDK_ERR_INVALID_ARGUMENT,
 * FDK_ERR_NOT_INITIALIZED (no platform connection), or the negative
 * fdk_result the backend's dispatch reported for an unrecoverable
 * connection failure (treat the connection as dead; fdk_run() stops
 * its loop on the same condition). */
int fdk_pump_events(fdk_context *ctx, int timeout_ms);

/* ---- Timers (1.3.1) ----
 *
 * The event loop's clock: callbacks fired between event batches, in
 * deadline order, on the UI thread, from fdk_pump_events() (and
 * therefore fdk_run()). Timers are what "repaint every 16ms",
 * "blink the caret every 530ms", "poll the sensor every second",
 * and "dismiss the toast after 4s" all look like — one primitive
 * instead of four hand-rolled pump-loop shapes.
 *
 * Clock: CLOCK_MONOTONIC, like the pump's own timeout budget. A
 * timer may fire LATE (the loop was busy, the caller's timeout
 * quantum rounded up) but never early. Repeating timers re-arm from
 * the FIRING time (now + interval), not the scheduled deadline — a
 * missed beat is skipped, never caught up in a burst.
 *
 * Reentrancy: a callback may add timers, remove any timer (including
 * itself — removal inside its own callback is safe), call fdk_quit(),
 * destroy windows and widgets. Removal is synchronous: a removed
 * timer never fires again, even if it was already due. Timers added
 * from inside a firing callback are first considered on the NEXT
 * pump call (a 0ms timer added in a callback fires as soon as the
 * caller pumps again — use that for "run this after the current
 * batch settles" deferrals).
 *
 * Detached trees (no window) have no context and no timers; timers
 * belong to the fdk_context, and the widget-internal consumers
 * (caret blink, indeterminate progress) resolve their window's
 * context the same way the Entry resolves the clipboard.
 */

typedef struct fdk_timer fdk_timer;

/* Timer callback. `timer` is the firing timer (reset/remove are both
 * legal inside); `user_data` is what fdk_timer_add received. */
typedef void (*fdk_timer_fn)(fdk_timer *timer, void *user_data);

/* Adds a timer to `ctx` that will fire after `interval_ms`
 * milliseconds. `repeating` = false: fires ONCE, then removes and
 * FREES itself — the handle is dangling from that moment, exactly
 * like any other freed C pointer: do not pass it to any timer
 * function afterwards. `repeating` = true: fires every interval_ms
 * until removed. interval_ms 0 is legal (fires on every pump call;
 * the deferred-callback idiom above). Returns NULL on invalid
 * arguments or allocation failure. */
fdk_timer *fdk_timer_add(fdk_context *ctx, fdk_u32 interval_ms,
                         bool repeating, fdk_timer_fn fn,
                         void *user_data);

/* Removes the timer immediately (no further firings, including a
 * firing already due this pump call) and frees it — the handle is
 * dangling afterwards. Legal from inside the timer's own callback
 * (the free is deferred until the callback returns). NULL is a safe
 * no-op. */
void fdk_timer_remove(fdk_timer *timer);

/* Re-arms a timer: the next firing is interval_ms from NOW (and for
 * repeating timers, the cadence continues from there). Legal on both
 * live and one-shot-pending timers; a no-op on dead/NULL handles. */
void fdk_timer_reset(fdk_timer *timer, fdk_u32 interval_ms);

/* Requests that the running fdk_run() event loop stop and return.
 * Safe to call from within an event callback. Has no effect if the
 * loop is not currently running. */
void fdk_quit(fdk_context *ctx);

/* Tears down the context: destroys any windows still open, releases
 * the platform connection, and frees `ctx`. `ctx` must not be used
 * after this call. Passing NULL is a safe no-op. */
void fdk_shutdown(fdk_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FDK_CORE_H */
