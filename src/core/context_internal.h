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
