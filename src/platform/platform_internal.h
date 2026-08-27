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
/* A platform framebuffer handed out by a backend's
 * window_get_framebuffer op (the machinery behind fdk_surface —
 * see include/fdk/fdk_surface.h). `stride` is in fdk_u32 PIXEL units
 * (not bytes), >= width; rows start at pixels + (size_t)y * stride.
 * The layout is XRGB8888 (R<<16|G<<8|B, top byte ignored) on both
 * current backends. The pixels pointer's lifetime is governed by the
 * backend: valid until the next get_framebuffer call on that window
 * and — for Wayland, whose buffers are handed to the compositor at
 * present — until window_present, after which a fresh acquisition
 * must be made. */
typedef struct fdk_platform_framebuffer {
    fdk_u32 *pixels;
    fdk_i32 width;
    fdk_i32 height;
    fdk_i32 stride; /* in fdk_u32 units */
} fdk_platform_framebuffer;

/* Damage region handed to window_present (the machinery behind
 * fdk_surface_present's partial-redraw contract — see
 * include/fdk/fdk_surface.h, "Damage tracking"). Rects are in
 * surface-local pixel coordinates, half-open [x, x+w) x [y, y+h),
 * NOT clamped to the framebuffer (backends clamp — they know the
 * real bounds). `full` set means "the whole surface changed" and
 * supersedes the rect list; count == 0 with full == 0 means
 * "nothing changed" (present should no-op). The struct is
 * value-copied / read-only from the backend's perspective. */
#define FDK_PLATFORM_DAMAGE_RECTS 64
typedef struct fdk_platform_damage {
    fdk_rect rects[FDK_PLATFORM_DAMAGE_RECTS];
    int count; /* valid rects, <= FDK_PLATFORM_DAMAGE_RECTS */
    int full;  /* 1 = whole surface damaged */
} fdk_platform_damage;

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

    /* OPTIONAL (Phase 8 decorations): turn the window manager's /
     * compositor's own decorations for this window on or off. X11
     * implements this via _MOTIF_WM_HINTS (respected by all major
     * WMs; harmless where ignored). Wayland leaves it NULL until
     * xdg-decoration lands — callers treat NULL/UNAVAILABLE as "this
     * backend cannot drop its chrome" and must NOT draw their own
     * title bar over the compositor's. */
    fdk_result (*window_set_wm_decorations)(fdk_platform_window *pwindow,
                                            bool on);

    /* OPTIONAL: the window's top-left position in root (screen)
     * coordinates. NULL = the backend cannot know it (Wayland:
     * clients are not told their position). */
    fdk_result (*window_get_position)(fdk_platform_window *pwindow,
                                      fdk_i32 *out_x, fdk_i32 *out_y);

    /* OPTIONAL: move the window to root coordinates. NULL = the
     * backend cannot (Wayland: only the compositor moves windows).
     * Under a reparenting WM this moves the client window; WMs that
     * reparent may or may not carry the frame along — v1 documents
     * this as exact under bare/non-reparenting X servers. */
    void (*window_move_to)(fdk_platform_window *pwindow,
                           fdk_i32 x, fdk_i32 y);

    /* --- Software rendering ---
     *
     * Both render ops are OPTIONAL: a backend that cannot provide a
     * software framebuffer (e.g. a future GPU-only backend) leaves
     * them NULL and the surface layer (src/render/surface.c) reports
     * FDK_ERR_UNSUPPORTED to the application.
     *
     * window_get_framebuffer returns the window's current CPU-drawing
     * buffer in *out_fb, (re)creating it if the window was resized
     * since the last call. A present-less no-draw call is legal.
     *
     * window_present makes the damaged parts of the current buffer
     * visible on screen. `damage` is never NULL and follows the
     * fdk_platform_damage contract above: full=1 -> whole surface;
     * count==0 -> nothing changed (backends treat it as a successful
     * no-op); otherwise exactly the listed rects changed (backends
     * may coarsen granularity but must not present garbage outside
     * the damage as new content — a fresh buffer must therefore
     * carry valid content outside damage, e.g. by copying the
     * previous frame into it, because compositors are allowed to
     * ignore damage hints entirely). With no buffer ever acquired it
     * is a documented no-op returning FDK_OK. See x11_surface.c and
     * wayland_window.c for per-backend notes. */
    fdk_result (*window_get_framebuffer)(fdk_platform_window *pwindow,
                                          fdk_platform_framebuffer *out_fb);
    fdk_result (*window_present)(fdk_platform_window *pwindow,
                                 const fdk_platform_damage *damage);

    /* OPTIONAL frame-pacing query behind
     * fdk_surface_frame_ready(): returns nonzero when the next frame
     * may be drawn/presented now (compositor acknowledged the last
     * frame, or no pacing applies). NULL means "always ready" (X11,
     * and any backend without display-driven frame feedback). Must
     * never block and never busy-wait — it is a state query. */
    int (*window_frame_ready)(fdk_platform_window *pwindow);
} fdk_platform_ops;

/* Backend entry points. Each returns NULL if that backend was not
 * compiled in (Wayland in a build with FDK_DISABLE_WAYLAND=1 — see
 * the Makefile's optional-Wayland logic and docs/build.md). */
const fdk_platform_ops *fdk_platform_x11_ops(void);
const fdk_platform_ops *fdk_platform_wayland_ops(void);

/* Auto-detection per fdk_core.h's documented FDK_PLATFORM_AUTO
 * contract: Wayland if $WAYLAND_DISPLAY is set (existence check only
 * here; the actual connection attempt in wayland_ops->connect() is
 * what proves reachability), otherwise X11. */
int fdk_platform_wayland_display_present(void);

#endif /* FDK_PLATFORM_INTERNAL_H */
