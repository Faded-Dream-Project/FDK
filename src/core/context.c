#define FDK_LOG_TAG "core"

#include "fdk/fdk_core.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "window/window_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- small static helpers (forward-declared by being defined above
 * their use site; both are file-local to context.c) ---- */

static int errno_value(void) { return errno; }
static int errno_is_eintr(void) { return errno == EINTR; }

/* ---- string helper ----
 * Strdup is POSIX-200809 (we are in _POSIX_C_SOURCE=200809L per
 * Makefile) but the FDK allocator path already centralizes OOM
 * logging; using it here keeps allocation tracking consistent and
 * lets a future debug allocator hook see app_id allocation too. */
static char *dup_string(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = fdk_alloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* ---- timers (1.3.1) ----------------------------------------------------
 *
 * One queue on the context, fired from the pump between event
 * batches (deadline order, LATE allowed / never early). See
 * fdk_core.h for the public contract. */

static long long timers_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL +
           (long long)ts.tv_nsec / 1000000LL;
}

fdk_timer *fdk_timer_add(fdk_context *ctx, fdk_u32 interval_ms,
                         bool repeating, fdk_timer_fn fn,
                         void *user_data) {
    if (ctx == NULL || fn == NULL) {
        return NULL;
    }
    if (ctx->timer_count == ctx->timer_capacity) {
        size_t cap = (ctx->timer_capacity == 0) ? 8
                                                : ctx->timer_capacity * 2;
        if (cap < ctx->timer_capacity ||
            cap > SIZE_MAX / sizeof(fdk_timer *)) {
            return NULL; /* refuse absurd growth */
        }
        fdk_timer **grown =
            fdk_realloc(ctx->timers, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        ctx->timers = grown;
        ctx->timer_capacity = cap;
    }
    fdk_timer *t = fdk_alloc(sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->ctx = ctx;
    t->interval_ms = interval_ms;
    t->repeating = repeating;
    t->active = true;
    t->fn = fn;
    t->user_data = user_data;
    t->deadline_ms = timers_now_ms() + (long long)interval_ms;
    ctx->timers[ctx->timer_count++] = t;
    return t;
}

/* Unlinks a dead timer from the queue and frees it. Only called
 * when the record is NOT the currently-firing one (whose free the
 * fire loop defers — see the struct comment). */
static void timer_unlink_free(fdk_context *ctx, fdk_timer *t) {
    for (size_t i = 0; i < ctx->timer_count; i++) {
        if (ctx->timers[i] == t) {
            ctx->timers[i] = ctx->timers[--ctx->timer_count];
            break;
        }
    }
    fdk_free(t);
}

void fdk_timer_remove(fdk_timer *timer) {
    if (timer == NULL || !timer->active) {
        return;
    }
    timer->active = false; /* never fires again, even if already due */
    fdk_context *ctx = timer->ctx;
    if (ctx->timer_firing == timer) {
        /* Its own callback is removing it: the fire loop frees the
         * record after the callback returns (touching it here would
         * free the very record that loop still holds). */
        return;
    }
    timer_unlink_free(ctx, timer);
}

void fdk_timer_reset(fdk_timer *timer, fdk_u32 interval_ms) {
    if (timer == NULL || !timer->active) {
        return;
    }
    timer->interval_ms = interval_ms;
    timer->deadline_ms = timers_now_ms() + (long long)interval_ms;
}

/* Earliest live deadline; false when no live timers exist. */
static bool timers_next_deadline(fdk_context *ctx,
                                 long long *out_deadline_ms) {
    bool any = false;
    long long best = 0;
    for (size_t i = 0; i < ctx->timer_count; i++) {
        fdk_timer *t = ctx->timers[i];
        if (!any || t->deadline_ms < best) {
            best = t->deadline_ms;
            any = true;
        }
    }
    if (any && out_deadline_ms != NULL) {
        *out_deadline_ms = best;
    }
    return any;
}

/* Fires every due timer (deadline order), returns how many fired.
 * Snapshot + re-check discipline: a callback may remove later timers
 * (skipped), add timers (considered next pump — the documented
 * deferral idiom), or quit the loop. Repeating timers re-arm from
 * the FIRING time (missed beats are skipped, never caught up). */
static int timers_fire_due(fdk_context *ctx) {
    if (ctx->timer_count == 0) {
        return 0;
    }
    long long now = timers_now_ms();
    /* Collect the due set (indices move under removals, so snapshot
     * the pointers themselves). */
    fdk_timer **due = NULL;
    size_t due_n = 0;
    for (size_t i = 0; i < ctx->timer_count; i++) {
        if (ctx->timers[i]->deadline_ms <= now) {
            due_n++;
        }
    }
    if (due_n == 0) {
        return 0;
    }
    due = fdk_alloc_array(due_n, sizeof(*due));
    if (due == NULL) {
        return 0; /* OOM: skip this round — timers fire next pump */
    }
    size_t k = 0;
    for (size_t i = 0; i < ctx->timer_count; i++) {
        if (ctx->timers[i]->deadline_ms <= now) {
            due[k++] = ctx->timers[i];
        }
    }
    /* Deadline order (stable enough: ties fire in add order). */
    for (size_t i = 1; i < due_n; i++) {
        fdk_timer *key = due[i];
        size_t j = i;
        while (j > 0 && due[j - 1]->deadline_ms > key->deadline_ms) {
            due[j] = due[j - 1];
            j--;
        }
        due[j] = key;
    }
    int fired = 0;
    for (size_t i = 0; i < due_n; i++) {
        fdk_timer *t = due[i];
        if (!t->active || t->ctx != ctx) {
            continue; /* removed (or cross-context: impossible) */
        }
        if (t->repeating) {
            t->deadline_ms = now + (long long)t->interval_ms;
        } else {
            t->active = false; /* one-shot: dead from here on */
        }
        fdk_timer *prev_firing = ctx->timer_firing;
        ctx->timer_firing = t;
        t->fn(t, t->user_data);
        ctx->timer_firing = prev_firing;
        fired++;
        if (!t->active) {
            /* One-shot death or a removal from inside the callback:
             * the record is still allocated (remove deferred the free
             * to exactly here). */
            timer_unlink_free(ctx, t);
        }
    }
    fdk_free(due);
    return fired;
}

/* ---- dispatch glue ----
 * This is the function handed to a backend's connect() (see
 * fdk_platform_ops in platform_internal.h). When the backend has
 * translated a backend-specific event into an fdk_event_data, it calls
 * us with the opaque `fdk_platform_window *` it delivered the event
 * against; we look that pwindow up in the context's window registry to
 * find the owning public fdk_window, then call fdk_window_dispatch_event()
 * which caches configure sizes and invokes the application's
 * registered callback.
 *
 * `dispatch_user_data` is the fdk_context* itself (set by fdk_init()
 * below) — the same value the backend was handed at connect time. */
static void context_dispatch_event(fdk_platform_window *pwindow,
                                    const fdk_event_data *event,
                                    void *dispatch_user_data) {
    fdk_context *ctx = dispatch_user_data;
    if (ctx == NULL || pwindow == NULL || event == NULL) {
        return;
    }
    fdk_window *window = fdk_context_find_window_by_pwindow(ctx, pwindow);
    if (window == NULL) {
        /* Backend delivered an event for a pwindow FDK doesn't track.
         * This can happen during teardown (a window was destroyed but
         * the backend still had a queued event for it) — not an error,
         * just nothing to dispatch. */
        return;
    }
    fdk_window_dispatch_event(window, event);
}

/* ---- backend selection ----
 * FDK_PLATFORM_AUTO: try Wayland first if $WAYLAND_DISPLAY looks set
 * (existence-only check here; the actual connection attempt in
 * wayland_ops->connect() is what proves reachability — see
 * platform_internal.h's doc comment on
 * fdk_platform_wayland_display_present). If that fails, fall back to
 * X11. Each backend's connect() returns FDK_ERR_NO_DISPLAY if no
 * display of that kind is reachable.
 *
 * Explicit FDK_PLATFORM_X11 / FDK_PLATFORM_WAYLAND: try ONLY that
 * backend, no silent fallback (per fdk_core.h's documented contract
 * and the test_platform_no_display.c test that asserts this). */
static fdk_result select_and_connect(
    fdk_platform_backend requested,
    fdk_context *ctx,
    const fdk_platform_ops **out_ops,
    fdk_platform_connection **out_conn) {

    /* Build the candidate list in selection order. */
    const fdk_platform_ops *candidates[2];
    size_t candidate_count = 0;

    if (requested == FDK_PLATFORM_AUTO) {
        if (fdk_platform_wayland_display_present()) {
            const fdk_platform_ops *wl = fdk_platform_wayland_ops();
            if (wl != NULL) {
                candidates[candidate_count++] = wl;
            }
        }
        const fdk_platform_ops *x11 = fdk_platform_x11_ops();
        if (x11 != NULL) {
            candidates[candidate_count++] = x11;
        }
        /* If Wayland wasn't even detected at the env-var level (or was
         * compiled out), we just try X11 — X11's connect() returns
         * FDK_ERR_NO_DISPLAY if XOpenDisplay(NULL) finds nothing. */
    } else if (requested == FDK_PLATFORM_WAYLAND) {
        const fdk_platform_ops *wl = fdk_platform_wayland_ops();
        if (wl != NULL) {
            candidates[candidate_count++] = wl;
        }
    } else { /* FDK_PLATFORM_X11 */
        const fdk_platform_ops *x11 = fdk_platform_x11_ops();
        if (x11 != NULL) {
            candidates[candidate_count++] = x11;
        }
    }

    /* A backend that was compiled out (Wayland in a build with
     * FDK_DISABLE_WAYLAND=1, see Makefile) returns NULL from its
     * fdk_platform_*_ops() entry point — skip it. The check is what
     * makes "optional Wayland" possible without touching this code
     * path beyond what's above. */
    fdk_result last_failure = FDK_ERR_NO_DISPLAY;
    for (size_t i = 0; i < candidate_count; i++) {
        const fdk_platform_ops *ops = candidates[i];
        FDK_INFO("attempting %s backend", ops->name);
        fdk_platform_connection *conn = NULL;
        /* Pass `ctx` as dispatch_user_data — by this point in
         * fdk_init(), ctx is fully allocated and field-initialized
         * (only `ops`/`conn` are unset, which the dispatch callback
         * doesn't read). Backends store this value at connect time
         * and use it for every future dispatch invocation; doing it
         * this way avoids needing to peek into the opaque
         * fdk_platform_connection struct (which lives inside
         * src/platform/{x11,wayland}/ and is intentionally opaque to
         * the core layer — see docs/architecture.md's "no backend
         * leakage" rule). */
        fdk_result r = ops->connect(context_dispatch_event, ctx,
                                    ctx->app_id, &conn);
        if (fdk_ok(r)) {
            *out_ops = ops;
            *out_conn = conn;
            return FDK_OK;
        }
        last_failure = r;
        /* FDK_ERR_NO_DISPLAY means "this backend isn't reachable in
         * this environment" — for FDK_PLATFORM_AUTO we want to fall
         * through and try the next candidate. Other errors
         * (FDK_ERR_PLATFORM_INIT) mean the backend WAS reachable but
         * couldn't complete setup; for AUTO we still try the next
         * candidate, because the alternative is failing the whole
         * fdk_init() when the other backend might work fine. The
         * last_failure is preserved so if every candidate fails, the
         * reported error reflects the most informative non-NO_DISPLAY
         * failure (preferred) or NO_DISPLAY if all were no-display. */
        if (r != FDK_ERR_NO_DISPLAY) {
            FDK_WARN("%s backend failed to initialize: %s",
                     ops->name, fdk_result_to_string(r));
        }
    }

    *out_ops = NULL;
    *out_conn = NULL;
    return last_failure;
}

fdk_result fdk_init(fdk_context **out_ctx, const fdk_init_options *options) {
    if (out_ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_context *ctx = fdk_alloc(sizeof(fdk_context));
    if (ctx == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    ctx->backend = FDK_PLATFORM_AUTO;
    ctx->app_id = NULL;
    ctx->running = 0;
    ctx->quit_requested = 0;
    ctx->ops = NULL;
    ctx->conn = NULL;
    ctx->windows = NULL;
    ctx->window_count = 0;
    ctx->window_capacity = 0;
    ctx->timers = NULL;
    ctx->timer_count = 0;
    ctx->timer_capacity = 0;
    ctx->timer_firing = NULL;

    fdk_platform_backend requested = FDK_PLATFORM_AUTO;
    if (options != NULL) {
        ctx->backend = options->backend;
        requested = options->backend;
        if (options->app_id != NULL) {
            ctx->app_id = dup_string(options->app_id);
            if (ctx->app_id == NULL) {
                fdk_free(ctx);
                return FDK_ERR_OUT_OF_MEMORY;
            }
        }
    }

    /* Actually perform the platform connection. This is what turns
     * fdk_init() from a stub into a real Phase 2 entry point: a context
     * with no reachable display genuinely fails with
     * FDK_ERR_NO_DISPLAY (test_platform_no_display.c asserts exactly
     * this), and a context with one available genuinely connects.
     *
     * ctx is fully constructed by the time we hand it to
     * select_and_connect() (and thence to a backend's connect() as
     * dispatch_user_data) — every field except `ops`/`conn` is set,
     * and the dispatch callback never reads either of those from a
     * partially-constructed state. */
    const fdk_platform_ops *ops = NULL;
    fdk_platform_connection *conn = NULL;
    fdk_result r = select_and_connect(requested, ctx, &ops, &conn);
    if (!fdk_ok(r)) {
        /* On any connection failure, the context is fully torn down
         * — no half-initialized state escapes to the caller. */
        fdk_free(ctx->app_id);
        fdk_free(ctx->windows); /* empty but defensive */
        fdk_free(ctx);
        return r;
    }

    ctx->ops = ops;
    ctx->conn = conn;

    FDK_INFO("initialized (backend=%s, app_id=%s)",
             ops->name ? ops->name : "?",
             ctx->app_id ? ctx->app_id : "(none)");

    *out_ctx = ctx;
    return FDK_OK;
}

int fdk_pump_events(fdk_context *ctx, int timeout_ms) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->ops == NULL || ctx->conn == NULL) {
        /* Same "not initialized / already shut down" edge cases
         * fdk_run() documents; reported instead of warned because the
         * return value is available for it here. */
        return FDK_ERR_NOT_INITIALIZED;
    }

    int fd = ctx->ops->get_event_fd(ctx->conn);
    if (fd < 0) {
        FDK_ERROR("backend gave no event fd; cannot poll");
        return FDK_ERR_PLATFORM_INIT;
    }

    /* The wait loop. "Wait up to timeout_ms for events" means
     * APPLICATION events — the backends consume their own internal
     * traffic inside dispatch_pending (X11's MIT-SHM completion
     * notifications; Wayland's bookkeeping events) and report only
     * what the application can observe. When such internal traffic
     * is all that arrived, poll() has woken for nothing the caller
     * cares about, so the loop goes back to waiting for the
     * REMAINING time instead of returning early.
     *
     * Found live (Phase 5 completion, the text demo, now examples/03_text.c): every
     * XShmPutImage present makes the server send one ShmCompletion
     * ~1-4ms later, so a draw-per-frame loop with pump(15ms) pacing
     * was actually paced by its own completion events — 252fps
     * instead of ~65fps, burning the demo's whole animation budget in
     * a second. The timeout contract is now enforced against a
     * monotonic deadline regardless of what wakes poll().
     *
     * The pre-drain on every iteration matters too: client libraries
     * read socket data into an internal queue during ordinary calls
     * outside this loop — Xlib, for one, reads whatever is available
     * every time it flushes, and fdk_surface_present() flushes every
     * frame. An event that arrived while such a call was reading has
     * already LEFT the socket, so poll() on the connection fd will
     * never report it, and without this drain it would sit in the
     * client queue forever. (Found live: a WM_DELETE_WINDOW sent
     * mid-render was swallowed exactly this way; events landing
     * during the poll() wait were fine, events landing during
     * rendering were not.)
     *
     * Both backends' dispatch_pending are designed to be safely
     * callable in this position: X11's drains Xlib's queue via
     * XPending; Wayland's does its own non-blocking readability check
     * (see wayland_dispatch.c). */
    struct timespec deadline;
    const bool finite = timeout_ms >= 0;
    if (finite) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        long long ns = (long long)deadline.tv_nsec +
                       (long long)timeout_ms * 1000000LL;
        deadline.tv_sec += (time_t)(ns / 1000000000LL);
        deadline.tv_nsec = (long)(ns % 1000000000LL);
    }

    for (;;) {
        int buffered = ctx->ops->dispatch_pending(ctx->conn);
        if (buffered < 0) {
            FDK_ERROR("backend dispatch_pending failed (%d)", buffered);
            return buffered;
        }
        /* Due timers fire between batches (deadline order). A fired
         * timer counts toward the return value so the canonical
         * `pump; paint;` loop repaints timer-driven damage (caret
         * blinks, animation frames) with zero extra plumbing. */
        int fired = timers_fire_due(ctx);
        if (fired > 0) {
            fdk__window_flush_geo_repaints(ctx);
            return fired;
        }
        if (buffered > 0) {
            /* 1.2.2: the batch is drained — paint any geometry the
             * batch changed, ONCE per window at the batch's final
             * size (see window_internal.h; the resize backlog that
             * wedged the main thread at 100% CPU pre-1.2.2). */
            fdk__window_flush_geo_repaints(ctx);
            return buffered;
        }

        int wait_ms = -1;
        if (finite) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long rem_ns =
                (long long)(deadline.tv_sec - now.tv_sec) * 1000000000LL +
                (deadline.tv_nsec - now.tv_nsec);
            if (rem_ns <= 0) {
                return 0; /* timeout expired */
            }
            wait_ms = (int)((rem_ns + 999999LL) / 1000000LL); /* ceil */
        }
        /* Timers cap the wait: never sleep past the next deadline
         * (an indefinite timeout with live timers still wakes up to
         * fire them). */
        {
            long long next_deadline = 0;
            if (timers_next_deadline(ctx, &next_deadline)) {
                long long now_ms = 0;
                struct timespec nowts;
                clock_gettime(CLOCK_MONOTONIC, &nowts);
                now_ms = (long long)nowts.tv_sec * 1000LL +
                         (long long)nowts.tv_nsec / 1000000LL;
                long long to_next = next_deadline - now_ms;
                if (to_next < 0) {
                    to_next = 0; /* already due: fire next loop head */
                }
                if (wait_ms < 0 || to_next < (long long)wait_ms) {
                    wait_ms = (int)to_next;
                }
            }
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0) {
            if (errno_is_eintr()) {
                /* Signal arrived mid-poll; nothing dispatched, caller
                 * loops and re-checks its own exit conditions (a
                 * fdk_quit() from a signal handler is honored this way —
                 * same behavior fdk_run()'s loop has always had). */
                return 0;
            }
            FDK_ERROR("poll() failed (errno=%d)", errno_value());
            return FDK_ERR_UNKNOWN;
        }

        if (pfd.revents & (POLLERR | POLLNVAL)) {
            FDK_ERROR("poll() reported fd error condition");
            return FDK_ERR_PLATFORM_INIT;
        }

        if ((pfd.revents & (POLLIN | POLLHUP)) != 0) {
            int dispatched = ctx->ops->dispatch_pending(ctx->conn);
            if (dispatched < 0) {
                /* Backend returned a negative fdk_result cast to int —
                 * unrecoverable connection failure. Propagate it so the
                 * caller (fdk_run() or an application-owned loop) can
                 * treat the connection as dead. */
                FDK_ERROR("backend dispatch_pending failed (%d)", dispatched);
                return dispatched;
            }
            if (dispatched > 0) {
                /* 1.2.2: same batch-end flush as the loop head — the
                 * poll-visible batch just drained, paint its geometry
                 * fallout once before reporting the events up. */
                fdk__window_flush_geo_repaints(ctx);
                return dispatched;
            }
            /* Only backend-internal traffic arrived — timers due in
             * the meantime fire at the loop head; keep waiting the
             * remaining timeout (the deadline check returns 0 once
             * the budget is spent). */
        }
        /* Poll timeout or spurious wake: loop (the deadline check
         * returns 0 once the budget is spent). */
    }
}

void fdk_run(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->ops == NULL || ctx->conn == NULL) {
        /* Should not happen for a context that came out of a
         * successful fdk_init(); a NULL ops here means fdk_shutdown()
         * was already called, or the context was never initialized.
         * Match the no-op contract fdk_core.h documents for these
         * edge cases rather than crashing. */
        FDK_WARN("fdk_run() called on a context with no platform connection");
        return;
    }

    ctx->running = 1;
    ctx->quit_requested = 0;

    /* Drain anything the backend already queued during connect()'s
     * initial roundtrips (Wayland does at least one wl_display_roundtrip
     * in connect() for registry discovery; the resulting listener
     * callbacks fire here rather than inside connect() so they go
     * through the normal dispatch path. X11's XOpenDisplay doesn't
     * pre-queue anything to drain, so this is a near no-op for X11.) */
    (void)ctx->ops->dispatch_pending(ctx->conn);
    /* 1.2.2: the initial drain can carry a configure (Wayland's
     * first xdg configure lands here on resize-at-map compositors —
     * the 1.1.5 first-configure size fix); its repaint belongs to
     * the same batch. */
    fdk__window_flush_geo_repaints(ctx);

    /* Exit condition (per fdk_core.h): stop when fdk_quit() was called
     * OR when there are no top-level windows left. The "no windows"
     * case returns immediately even on the first iteration — that's
     * what test_run_returns_when_no_windows_open verifies.
     *
     * The loop body IS fdk_pump_events(): fdk_run() is now a thin
     * convenience wrapper around the pump primitive that applications
     * rendering animation frames use directly (see fdk_core.h). */
    while (!ctx->quit_requested && ctx->window_count > 0) {
        int r = fdk_pump_events(ctx, -1); /* block until something is readable */
        if (r < 0) {
            /* Connection failure (or fd error) — treat the connection
             * as dead and exit the loop, leaving cleanup to
             * fdk_shutdown(). */
            break;
        }
        /* POLLHUP (peer closed) doesn't necessarily mean an error for
         * our backends — X11 and Wayland both speak over a socket the
         * server owns, but a clean compositor shutdown is rare in
         * practice. We keep looping; the next poll() returns
         * immediately with the same POLLHUP and dispatch eventually
         * reports a negative result, which exits via the branch
         * above. */
    }

    ctx->running = 0;
}

void fdk_quit(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->quit_requested = 1;
}

void fdk_shutdown(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }

    FDK_INFO("shutting down");

    /* Destroy any windows the application leaked — mirrors each
     * backend's own disconnect() safety net but at the FDK window
     * level so application callbacks aren't bypassed. Iterating from
     * the end because fdk_window_destroy() calls
     * fdk_context_unregister_window() which swap-removes (mutating
     * indices). */
    while (ctx->window_count > 0) {
        FDK_WARN("shutdown with %zu window(s) still open — destroying",
                 ctx->window_count);
        fdk_window_destroy(ctx->windows[ctx->window_count - 1]);
    }

    if (ctx->ops != NULL && ctx->conn != NULL) {
        ctx->ops->disconnect(ctx->conn);
    }
    /* Leaked timers are freed WITHOUT firing (shutdown is teardown,
    * not delivery — the same policy the leaked-window sweep uses). */
    while (ctx->timer_count > 0) {
        FDK_WARN("shutdown with %zu timer(s) still live — freeing",
                 ctx->timer_count);
        fdk_timer *t = ctx->timers[ctx->timer_count - 1];
        t->active = false;
        timer_unlink_free(ctx, t);
    }
    fdk_free(ctx->timers);
    fdk_free(ctx->windows);
    fdk_free(ctx->app_id);
    fdk_free(ctx);
}

/* ---- window registry ---- */

fdk_result fdk_context_register_window(fdk_context *ctx, fdk_window *window) {
    if (ctx == NULL || window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->window_count == ctx->window_capacity) {
        size_t new_capacity = (ctx->window_capacity == 0)
            ? 4
            : ctx->window_capacity * 2;
        /* Overflow check on the array size multiplication. */
        if (new_capacity > (SIZE_MAX / sizeof(fdk_window *))) {
            FDK_ERROR("window registry capacity overflow");
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_window **new_array = fdk_realloc(
            ctx->windows, new_capacity * sizeof(fdk_window *));
        if (new_array == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        ctx->windows = new_array;
        ctx->window_capacity = new_capacity;
    }
    ctx->windows[ctx->window_count++] = window;
    return FDK_OK;
}

void fdk_context_unregister_window(fdk_context *ctx, fdk_window *window) {
    if (ctx == NULL || window == NULL) {
        return;
    }
    for (size_t i = 0; i < ctx->window_count; i++) {
        if (ctx->windows[i] == window) {
            /* Swap-remove: dispatch order doesn't matter, only
             * membership. */
            ctx->windows[i] = ctx->windows[ctx->window_count - 1];
            ctx->window_count--;
            return;
        }
    }
    FDK_WARN("unregister_window: window not in registry (double destroy?)");
}

fdk_window *fdk_context_find_window_by_pwindow(fdk_context *ctx,
                                                fdk_platform_window *pwindow) {
    if (ctx == NULL || pwindow == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->window_count; i++) {
        /* fdk_window's `pwindow` field is private to the library —
         * this function lives in the same internal compilation unit
         * boundary as window_internal.h, which exposes the struct
         * layout to context.c (already included above). */
        if (ctx->windows[i]->pwindow == pwindow) {
            return ctx->windows[i];
        }
    }
    return NULL;
}
