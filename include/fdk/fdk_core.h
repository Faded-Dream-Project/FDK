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
 * has not been called. */
void fdk_run(fdk_context *ctx);

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
