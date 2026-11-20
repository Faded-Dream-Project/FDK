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
/* ---- Render modes (1.0.0) -------------------------------------------------
 *
 * How a context (and each window) drives the pixel path:
 *
 *   FDK_RENDER_AUTO    zero-init default; resolves to HYBRID semantics
 *                      (the CPU rasterizer paints, the GPU present lane
 *                      engages where the driver is real — FDK's
 *                      "presentation lane" design, docs/rendering.md).
 *   FDK_RENDER_CPU     the software path only. No EGL/GLX is ever
 *                      loaded: no dlopen, no driver contact, no lane.
 *   FDK_RENDER_HYBRID  explicit AUTO's meaning: lane where available
 *                      (hardware GL, size policy permitting), the
 *                      classic SHM/XImage path otherwise — including
 *                      the documented present-time downgrade.
 *   FDK_RENDER_GPU     the lane is REQUIRED. The classic framebuffer
 *                      presentation path is disabled for the context's
 *                      windows: if the GL stack cannot initialize, the
 *                      WINDOW CREATION FAILS (FDK_ERR_UNSUPPORTED, a
 *                      logged reason — never a silent fallback), and a
 *                      lane that dies mid-flight fails the present
 *                      instead of downgrading. Widgets keep painting
 *                      software pixels: the lane's texture upload IS
 *                      the explicit CPU-pixels-to-GPU path (there is
 *                      no hidden CPU presentation behind it).
 *
 * Selection: fdk_init_options.render_mode (nonzero wins), else the
 * FDK_RENDER environment variable (cpu|hybrid|gpu, case-insensitive),
 * else AUTO. Per window: fdk_window_options.render_mode (0 = inherit
 * the context). */
typedef enum fdk_render_mode {
    FDK_RENDER_AUTO   = 0,
    FDK_RENDER_CPU    = 1,
    FDK_RENDER_HYBRID = 2,
    FDK_RENDER_GPU    = 3,
} fdk_render_mode;

typedef struct fdk_init_options {
    fdk_platform_backend backend;

    /* Reverse-DNS-style application identifier (e.g.
     * "org.example.myapp"), used where the platform wants one — Wayland
     * app-id, X11 WM_CLASS, etc. May be NULL to use a generated default. */
    const char *app_id;

    /* 1.0.0 appended (zero-init = FDK_RENDER_AUTO): the render mode.
     * Nonzero OVERRIDES the FDK_RENDER environment variable. Values
     * outside the enum refuse fdk_init with FDK_ERR_INVALID_ARGUMENT. */
    int render_mode;

    /* 1.0.0-dev appended (zero-init = the built-in per-format budget):
     * the maximum pixels fdk_surface_create_from_image* accepts, in
     * ADDITION to (not instead of) the per-format policy — the value
     * here is the cap for the LARGEST format budget (PNG/JPEG); the
     * stricter GIF budget scales with it. 0 keeps the defaults
     * (see docs/quality-gates.md: 96 Mi pixels PNG/JPEG, 16 Mi GIF).
     * A value smaller than 1 Mi pixels refuses init with
     * FDK_ERR_INVALID_ARGUMENT (a budget that rejects a 1024x1024
     * icon cannot be intended). Append-only: existing zero-init
     * callers see no change. */
    unsigned int image_max_pixels;

    /* NO struct_size here, and none will ever be PREPENDED: that
     * would shift every existing field's offset and break the ABI
     * in one step (docs/abi-policy.md's classification makes this
     * struct a frozen-layout input). Fields keep APPENDING, and the
     * size contract lives at the CALL — see fdk_init_ex() below.
     * (Future fields follow this comment; the comment is the
     * contract, not a field.) */
} fdk_init_options;

/* The append-only CONTRACT for fdk_init_options, made explicit
 * (2026-11-20): the struct grew twice by appending (render_mode,
 * then image_max_pixels) with no way for FDK to know which layout
 * a binary was compiled against — a caller built against the
 * SMALLER struct passes a smaller allocation, and FDK reading the
 * newer fields is an immediate over-read.
 *
 * Why not a leading struct_size like fdk_toplevel_info /
 * fdk_widget_class / fdk_ext_surface_ops? Those types were BORN
 * with the field; fdk_init_options was not, so prepending it now
 * shifts backend/app_id/render_mode by eight bytes and breaks
 * every existing binary in one stroke — the exact damage the
 * contract exists to prevent. A TRAILING size field is unusable
 * for an INPUT struct: reading it at all is the over-read.
 *
 * The safest shape, in the house style: a separate entry point
 * that takes the caller-declared size.
 *
 *   fdk_init    (ctx, opts)                     — frozen forever
 *                as "opts points at the FULL current layout".
 *                New fields set through it MUST be written by
 *                callers rebuilt against the new header.
 *   fdk_init_ex (ctx, opts, opts_size)          — the versioned
 *                form. Pass sizeof(fdk_init_options) AS YOUR
 *                HEADER DEFINED IT (or 0 = same meaning as
 *                fdk_init). FDK reads exactly
 *                min(opts_size, sizeof(its own layout)) bytes,
 *                field-by-field — a caller compiled against an
 *                older, smaller struct can never be over-read,
 *                and newer fields it never heard of simply read
 *                as their zero-init defaults.
 *
 * Rules for every future appended field (enforced by review,
 * pinned by test_core.c's fdk_init_ex group):
 *   1. append at the end, zero-init must mean the old default;
 *   2. fdk_init_ex reads it ONLY if it lies wholly inside
 *      opts_size;
 *   3. fdk_init keeps reading it (full-layout form) — the two
 *      entry points differ ONLY in the size discipline. */
fdk_result fdk_init_ex(fdk_context **out_ctx,
                       const fdk_init_options *options, size_t opts_size);

/* Reports the context's ACTIVE render mode and, when it differs from
 * what was requested, a human-readable reason (a static string, valid
 * for the process lifetime; NULL when active == requested). *active
 * or *reason may be NULL to skip. AUTO reports as HYBRID with the
 * resolution named — AUTO is a request shape, HYBRID is what runs. */
void fdk_context_render_mode(const fdk_context *ctx,
                             fdk_render_mode *active,
                             const char **reason);

/* Initializes FDK and creates a new toolkit context, writing it to
 * *out_ctx on success. `options` may be NULL to accept all defaults.
 *
 * THE SIZE CONTRACT, SECOND PASS (2026-11-20). fdk_init_ex (below)
 * protects callers who opt in; the hole was the LEGACY symbol: a
 * caller compiled against a SMALLER fdk_init_options passes a
 * smaller allocation, and once a field is appended, the exported
 * fdk_init reading sizeof(its own layout) over-reads it.
 *
 * The fix has two halves:
 *
 *   1. THE REDIRECT (compile time). Source-level fdk_init(...) is
 *      macro-redirected to this header's static inline, which calls
 *      fdk_init_ex with sizeof(fdk_init_options) AS THE CALLER'S
 *      HEADER DEFINED IT. A recompiled app can never be over-read,
 *      no matter how far the struct grows. The inline keeps the
 *      exact calling convention, so &fdk_init still works (its
 *      address type is the same; through the redirect it binds the
 *      inline, which forwards the caller's own sizeof — the only
 *      behavior change vs the old symbol, and the one the contract
 *      wants).
 *
 *   2. THE FROZEN SYMBOL (link time). The exported fdk_init is
 *      pinned to the 1.0 layout — it reads AT MOST
 *      FDK_INIT_OPTIONS_SIZE_1_0 bytes ({backend, app_id,
 *      render_mode, image_max_pixels}; 24 on every ABI this
 *      library has ever shipped). A binary that did NOT rebuild
 *      against this header (dlsym("fdk_init"), a non-C binding, a
 *      hand-written declaration) keeps working against future
 *      libraries exactly as it did against 1.0: no future append
 *      can make the library read past its 24 bytes. All new fields
 *      reach the library ONLY through fdk_init_ex.
 *
 * Opting out of the redirect (rare): define FDK_NO_INIT_REDIRECT
 * before including this header — the plain prototype comes back,
 * the call binds the frozen symbol, and the caller asserts by
 * doing so that it only passes 1.0-layout structs. */
#define FDK_INIT_OPTIONS_SIZE_1_0 24

#ifdef FDK_NO_INIT_REDIRECT
fdk_result fdk_init(fdk_context **out_ctx,
                    const fdk_init_options *options);
#else
/* The redirect: source-level fdk_init calls the static inline at
 * the BOTTOM of this file (after fdk_init_ex's prototype — C needs
 * the full declaration before the call). NO prototype here: the
 * inline IS the declaration; a prototype plus a later static
 * definition is a hard error. */
#endif

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

/* ---- Timers (1.0.0) ----
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

/* THE REDIRECT (the size contract's compile-time half; see the
 * fdk_init comment above for the full story). Defined AFTER the
 * extern-"C" block closes so the static inline gets C++ linkage
 * rules right in mixed translation units (the body only calls an
 * extern "C" function, so the inline itself needs no mangling).
 * FDK_NO_INIT_REDIRECT (defined before including this header)
 * removes it: the plain call binds the frozen symbol. */
#ifndef FDK_NO_INIT_REDIRECT
/* C++ note: this inline deliberately has C++ (not extern "C")
 * linkage — 'extern "C" static' is ill-formed, and the body only
 * CALLS an extern "C" function, so no mangling concern arises.
 * Its address differs from the frozen symbol's; callers needing
 * the stable exported address (dlopen'd plugin tables) should
 * take &fdk_init from a TU compiled with FDK_NO_INIT_REDIRECT,
 * or dlsym("fdk_init"). */
static inline fdk_result fdk_init(fdk_context **out_ctx,
                                  const fdk_init_options *options) {
    return fdk_init_ex(out_ctx, options,
                       options != NULL ? sizeof(*options) : (size_t)0);
}
#endif /* FDK_NO_INIT_REDIRECT */

#endif /* FDK_CORE_H */
