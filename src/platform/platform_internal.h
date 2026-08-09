/*
 * platform_internal.h — internal platform backend interface
 *
 * This is the seam described in docs/architecture.md ("no backend
 * leakage"): everything above this line (src/core, src/window, and
 * eventually src/widget etc.) talks only to `fdk_platform`, never to
 * Xlib/xcb/wayland-client types directly. Everything below this line
 * (src/platform/x11, src/platform/wayland) implements this interface
 * and is the ONLY place backend headers may be #included.
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_PLATFORM_INTERNAL_H
#define FDK_PLATFORM_INTERNAL_H

#include "fdk/fdk_core.h"
#include "fdk/fdk_error.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_types.h"
#include "fdk/fdk_window.h"

/* Opaque, backend-owned window handle. Cast to whatever the backend
 * actually needs internally (an X11 backend stores its Window ID +
 * Display pointer here; Wayland stores wl_surface / xdg_toplevel
 * pointers, etc.) — see
 * src/platform/x11/x11_platform.c and
 * src/platform/wayland/wayland_platform.c. The public fdk_window
 * (src/window/window_internal.h) holds one of these plus
 * backend-agnostic bookkeeping (event callback, cached size).
 */
typedef struct fdk_platform_window fdk_platform_window;

/* Opaque, backend-owned connection handle — one per fdk_context. */
typedef struct fdk_platform_connection fdk_platform_connection;

/* A platform backend delivers translated events by calling this
 * dispatcher, which src/window/ implements: it looks up the owning
 * fdk_window from the platform_window pointer and invokes that
 * window's registered fdk_event_callback_fn, if any. Backends never
 * call an application's callback directly. */
typedef void (*fdk_platform_dispatch_fn)(fdk_platform_window *pwindow,
                                          const fdk_event_data *event,
                                          void *dispatch_user_data);

/* One implementation of this struct exists per backend
 * (g_fdk_x11_backend, g_fdk_wayland_backend — see each backend's .c
 * file). fdk_init() (src/core/context.c) selects one based on
 * fdk_init_options.backend / FDK_PLATFORM_AUTO detection and stores
 * a pointer to it in the context; every other internal call site goes
 * through ctx->platform_ops rather than caring which backend it is. */
typedef struct fdk_platform_ops {
    /* Human-readable backend name for logging ("x11", "wayland"). */
    const char *name;

    /* Attempts to connect to the platform (X11 display / Wayland
     * compositor). On success, writes a new connection handle to
     * *out_conn and returns FDK_OK. On failure (no display reachable,
     * protocol setup failure, required global missing) returns
     * FDK_ERR_NO_DISPLAY or FDK_ERR_PLATFORM_INIT and leaves
     * *out_conn unchanged — this backend is then considered
     * unavailable and (for FDK_PLATFORM_AUTO) the other backend is
     * tried. `dispatch` and `dispatch_user_data` are stored by the
     * backend and used for all future event delivery on this
     * connection. */
    fdk_result (*connect)(fdk_platform_dispatch_fn dispatch,
                           void *dispatch_user_data,
                           fdk_platform_connection **out_conn);

    /* Releases the connection and everything still open on it (any
     * windows the caller failed to destroy first are force-destroyed,
     * with a warning logged — callers should not rely on this, see
     * fdk_shutdown()'s documented behavior in fdk_core.h). */
    void (*disconnect)(fdk_platform_connection *conn);

    /* Returns a pollable file descriptor for this connection's event
     * source (the X11 connection fd, or wl_display_get_fd()). Used by
     * the event loop (src/core/context.c) to block in poll()/epoll()
     * without busy-spinning. Returns -1 if the backend has no single
     * fd to offer (not expected for either current backend, but the
     * interface allows for it). */
    int (*get_event_fd)(fdk_platform_connection *conn);

    /* Processes whatever events are currently pending on the
     * connection (reads from the fd, translates, calls `dispatch` for
     * each). Does not block — call after poll()/epoll() indicates the
     * fd is readable, or opportunistically to drain a burst. Returns
     * the number of events processed (>= 0), or a negative fdk_result
     * value cast to int on unrecoverable connection failure (the
     * caller should treat the connection as dead and stop the loop —
     * see fdk_run()'s handling in src/core/context.c). */
    int (*dispatch_pending)(fdk_platform_connection *conn);

    /* --- Window operations --- */

    fdk_result (*window_create)(fdk_platform_connection *conn,
                                 const fdk_window_options *options,
                                 fdk_platform_window **out_pwindow);
    void (*window_destroy)(fdk_platform_window *pwindow);
    void (*window_show)(fdk_platform_window *pwindow);
    void (*window_hide)(fdk_platform_window *pwindow);
    void (*window_set_title)(fdk_platform_window *pwindow, const char *title);
    void (*window_resize)(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height);
    void (*window_set_size_limits)(fdk_platform_window *pwindow,
                                    fdk_size min_size, fdk_size max_size);
} fdk_platform_ops;

/* Backend entry points. Each returns NULL if that backend was not
 * compiled in (not expected currently — both are always compiled on
 * Linux per project requirements — but the check exists so a future
 * conditional build configuration has somewhere to hook in). */
const fdk_platform_ops *fdk_platform_x11_ops(void);
const fdk_platform_ops *fdk_platform_wayland_ops(void);

/* Auto-detection per fdk_core.h's documented FDK_PLATFORM_AUTO
 * contract: Wayland if $WAYLAND_DISPLAY is set (existence check only
 * here; the actual connection attempt in wayland_ops->connect() is
 * what proves reachability), otherwise X11. */
int fdk_platform_wayland_display_present(void);

#endif /* FDK_PLATFORM_INTERNAL_H */
