/*
 * context_internal.h — internal definition of struct fdk_context
 *
 * The public header (fdk_core.h) only ever exposes `fdk_context` as an
 * opaque forward-declared type (see fdk_types.h). Its real layout lives
 * here, internal to the library, so it can change freely between
 * releases without breaking ABI for applications that only ever hold
 * a pointer to it.
 *
 * Phase 2 fields: the platform backend vtable + connection handle, and
 * a small dynamic array of windows so the dispatch callback handed to
 * the backend can resolve an opaque `fdk_platform_window *` back to the
 * owning `fdk_window *` for event delivery (see src/window/window.c).
 * Phase 1's "no platform connection yet" comment is gone — fdk_init()
 * now actually performs the connection (src/core/context.c).
 */

#ifndef FDK_CONTEXT_INTERNAL_H
#define FDK_CONTEXT_INTERNAL_H

#include "fdk/fdk_core.h"

#include "platform/platform_internal.h"

#include <stddef.h>

struct fdk_context {
    fdk_platform_backend backend;
    char *app_id;           /* owned, heap-allocated copy */

    int running;            /* nonzero while inside fdk_run() */
    int quit_requested;     /* set by fdk_quit() */

    /* Phase 2: the backend selected during fdk_init() (X11 or Wayland,
     * per fdk_platform_backend). NULL only between fdk_shutdown() and
     * the next fdk_init(). ops->connect() must have succeeded for ops
     * to be non-NULL — the two are set together, atomically, in
     * fdk_init(). */
    const fdk_platform_ops *ops;
    fdk_platform_connection *conn;

    /* Fatal-dispatch reporting (1.1.0-rc1, the labwc report
     * 2026-10-26): the FIRST dispatch_pending failure a pump/fdk_run
     * sees is logged ONCE here (the backend logs its own one-time
     * detail line — Wayland's connection_dead carries the protocol
     * error's interface/id/code); identical repeats log NOTHING and,
     * when the caller gave a timeout, the pump waits the remaining
     * budget before returning the error so an application loop that
     * ignores the fatal return cannot busy-spin the CPU (the report's
     * "backend dispatch_pending failed (-101)" line printed ~2.2
     * million times in three seconds). A DIFFERENT fatal code resets
     * the dedupe (a new failure mode gets its one line). */
    int fatal_dispatch_reported; /* 0 = none yet; else the code      */

    /* Top-level windows created against this context. Maintained by
     * fdk_context_register_window() / _unregister_window() (called from
     * fdk_window_create() / fdk_window_destroy() in src/window/window.c).
     * The platform backends deliver events as
     * `fdk_platform_window *` handles; the dispatch glue in
     * src/core/context.c looks one up in this array to find the owning
     * `fdk_window *` so it can call fdk_window_dispatch_event(). */
    fdk_window **windows;
    size_t window_count;
    size_t window_capacity;

    /* Timers (1.3.1): pointer array so public fdk_timer* handles stay
     * stable across growth. See fdk_core.h for the contract. */
    fdk_timer **timers;
    size_t timer_count;
    size_t timer_capacity;
    /* The timer whose callback is on the stack RIGHT NOW (NULL when
     * none): fdk_timer_remove on THIS handle only deactivates — the
     * fire loop performs the unlink+free after the callback returns,
     * so a self-removing callback cannot free the record the loop is
     * still about to touch. Saved/restored around each callback so
     * nested pumps nest cleanly. */
    fdk_timer *timer_firing;

    /* ---- Extension fd sources (1.1.0, include/fdk/fdk_extend.h) ----
     *
     * Pointer array (stable handles across growth, same discipline
     * as timers). extfd_firing is the source whose callback is on
     * the stack RIGHT NOW: fdk_ext_loop_remove_fd on THAT handle
     * only deactivates; the fire loop in src/core/extfd.c performs
     * the unlink+free after the callback returns. The pump polls
     * them alongside the backend fd (src/core/context.c). */
    struct fdk_ext_fd_source **extfds;
    size_t extfd_count;
    size_t extfd_capacity;
    struct fdk_ext_fd_source *extfd_firing;

    /* ---- Extension surface kinds (1.1.0, src/window/extend.c) ----
     *
     * Intrusive singly-linked list of the kinds registered on this
     * context (fdk_ext_register_kind). Add-on OWNED objects — FDK
     * only links them for the duplicate-name check, the diagnostic
     * sweep, and detaching them at shutdown (kinds outliving their
     * context must not keep pointing at freed memory). */
    struct fdk_ext_surface_kind *ext_kinds_head;

    /* ---- D-Bus connections (1.1.0, src/dbus/) ----------------------
     *
     * Every live fdk_dbus_connection registers here at creation and
     * unregisters when freed. The registry exists so fdk_shutdown()
     * can close survivors BEFORE the timer/extfd sweeps (a dbus
     * connection owns ext fd sources and timers; sweeping those
     * first would leave the connection touching freed records), and
     * so zombie connections (closed from inside their own callback —
     * the deferred-free discipline) can be reaped by the next public
     * fdk_dbus entry point. The array itself lives in
     * dbus_connection.c's registry code. */
    struct fdk_dbus_connection **dbus_conns;
    size_t dbus_conn_count;
    size_t dbus_conn_capacity;
};

/* ---- Extension fd sources (1.1.0) — src/core/extfd.c ---------------
 *
 * The pump-facing face of fdk_ext_loop_add_fd/remove_fd (public API
 * in fdk_extend.h): the poll-set builder and the reentrancy-safe
 * fire loop the pump calls after poll() reports activity. */
struct pollfd; /* <poll.h> lives in the .c files */

/* Writes this context's ext fd sources' poll entries at
 * pfds[base .. base+count) — the pump lays its own fds (backend,
 * theme watch) at [0 .. base) and calls this with that base.
 * FDK_EXT_FD_* flags translate to poll(2) event bits. Returns the
 * number of entries written (== fdk__extfd_count). */
size_t fdk__extfd_fill_poll(fdk_context *ctx, struct pollfd *pfds,
                            size_t base);

/* After poll(): for each source whose revents intersect its watched
 * flags (plus ERR/HUP, always reported like poll(2)), the callback
 * fires — snapshot + active re-check + deferred free, the timer fire
 * loop's discipline (a callback may remove any source including its
 * own, add sources that first fire on a LATER pump, destroy windows,
 * or quit). Reads the entries pfds[base .. base+n) — the SAME array
 * positions fill_poll wrote. Returns how many callbacks fired. */
int fdk__extfd_dispatch(fdk_context *ctx, const struct pollfd *pfds,
                        size_t base, size_t n);

/* How many ext fd sources are live (the poll array's sizing input). */
size_t fdk__extfd_count(fdk_context *ctx);

/* Shutdown sweep: frees every surviving source WITHOUT firing (the
 * same policy the leaked-timer sweep uses — teardown, not delivery).
 * Called from fdk_shutdown(). */
void fdk__extfd_shutdown(fdk_context *ctx);

/* The timer record itself. `active` is the removal flag: the fire
 * loop snapshots the pointer array, but re-checks active before
 * every callback (a prior callback may have removed a later timer)
 * — the same discipline the a11y registry snapshot uses. */
struct fdk_timer {
    fdk_context *ctx;
    fdk_u32 interval_ms;
    bool repeating;
    bool active;
    fdk_timer_fn fn;
    void *user_data;
    long long deadline_ms; /* CLOCK_MONOTONIC, same clock as the pump */
};

/* Called by fdk_window_create() (src/window/window.c) after a window is
 * successfully constructed. Adds it to ctx->windows so the dispatch
 * callback can find it. Returns FDK_ERR_OUT_OF_MEMORY if the windows
 * array can't grow. On failure, the caller is responsible for
 * destroying the platform window and the partially-built fdk_window —
 * the context is left untouched. */
fdk_result fdk_context_register_window(fdk_context *ctx, fdk_window *window);

/* Called by fdk_window_destroy() (src/window/window.c) before the
 * platform window is destroyed. Swap-removes (order does not matter
 * for event dispatch). Logs a warning if `window` is not in the
 * registry (double-destroy or destroy of an unregistered window). */
void fdk_context_unregister_window(fdk_context *ctx, fdk_window *window);

/* Used by the dispatch glue (src/core/context.c) to resolve a backend's
 * opaque `fdk_platform_window *` to the owning `fdk_window *`. Linear
 * scan: the number of top-level windows a typical application holds
 * open simultaneously is small, and matching pwindow pointers is O(n)
 * trivially cache-friendly — a hash table here would be premature
 * (see project principle against premature optimization). Returns NULL
 * if no registered fdk_window owns `pwindow` (event for a window FDK
 * already destroyed, or for one a backend delivered by mistake). */
fdk_window *fdk_context_find_window_by_pwindow(fdk_context *ctx,
                                                fdk_platform_window *pwindow);

#endif /* FDK_CONTEXT_INTERNAL_H */
