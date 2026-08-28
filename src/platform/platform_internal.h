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
                           void *dispatch_user_data, const char *app_id,
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

    /* Creates a window. `parent` is NULL for top-levels; for popup
     * windows (options->popup, Phase 9) it is the parent platform
     * window (X11: position reference for the override-redirect
     * child; Wayland: the xdg_surface the xdg_popup attaches to). */
    fdk_result (*window_create)(fdk_platform_connection *conn,
                                 const fdk_window_options *options,
                                 fdk_platform_window *parent,
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

    /* OPTIONAL: atomically move AND resize (the FDK-driven resize-edge
     * path; a separate move+resize pair would tear on the wire under a
     * real WM). NULL = the caller falls back to window_move_to +
     * window_resize. */
    void (*window_move_resize_to)(fdk_platform_window *pwindow,
                                  fdk_i32 x, fdk_i32 y,
                                  fdk_i32 width, fdk_i32 height);

    /* ---- OPTIONAL window-state operations (Phase 8) ----
     *
     * All of these are REQUESTS; the platform may ignore them (see
     * each op). State changes the platform acknowledges are reported
     * back as FDK_EVENT_WINDOW_STATE through the normal dispatch path,
     * never assumed from the request alone — except where a backend
     * performs the action itself (the bare-X fallbacks below), where
     * it dispatches the event directly. */

    /* Request maximized/unmaximized. X11: _NET_WM_STATE message under
     * an EWMH WM (state arrives via PropertyNotify); without a WM the
     * backend maximizes itself (fullscreen move+resize, geometry
     * saved for restore) and dispatches the state event. Wayland:
     * xdg_toplevel.set_maximized/unset; state arrives via configure.
     * NULL = backend cannot maximize at all. */
    fdk_result (*window_set_maximized)(fdk_platform_window *pwindow,
                                       bool maximized);

    /* Request minimized (iconic)/restored. X11: XIconifyWindow (any
     * WM) / XUnmapWindow + flag (bare X); restore is XMapWindow.
     * State tracked via the WM_STATE property where a WM maintains
     * it. Wayland: xdg_toplevel.set_minimized — the protocol gives NO
     * acknowledgement (no unminimize request either; compositors
     * unminimize via activation), so the backend reports the REQUEST
     * optimistically and clears it on the next activated configure;
     * window_set_minimized(false) returns FDK_ERR_UNSUPPORTED there.
     * NULL = backend cannot minimize. */
    fdk_result (*window_set_minimized)(fdk_platform_window *pwindow,
                                       bool minimized);

    /* OPTIONAL interactive-move/resize starters: hand the drag to the
     * WM/compositor (EWMH _NET_WM_MOVERESIZE on X11, xdg_toplevel
     * .move/.resize on Wayland), which then drives it with its own
     * pointer grab — FDK sees only the resulting configures. The
     * press point is window-local; backends translate as their
     * protocol needs. `edge` is an fdk_window_resize_edge compass
     * value (see window_internal.h; plain int here so this header
     * needn't depend on it); only begin_resize uses it. Return
     * FDK_OK when the platform took the drag, FDK_ERR_UNSUPPORTED
     * (or NULL op) when the caller should run its own fallback drag
     * via window_move_to/move_resize_to. */
    fdk_result (*window_begin_move)(fdk_platform_window *pwindow,
                                    fdk_i32 local_x, fdk_i32 local_y);
    fdk_result (*window_begin_resize)(fdk_platform_window *pwindow,
                                      int edge, fdk_i32 local_x,
                                      fdk_i32 local_y);

    /* --- OPTIONAL pointer introspection + cursor shaping (1.1.4) ---
     *
     * Both exist for the same class of bug: FDK's chrome has to
     * REACT to pointer position at moments the platform generates no
     * event for, and to ADVERTISE affordances a WM frame's borders
     * give for free.
     *
     * window_query_pointer writes the pointer's current position in
     * WINDOW-LOCAL coordinates to out_x/out_y and returns nonzero
     * when the pointer is inside this window's bounds right now. The
     * window layer calls it after geometry changes (configure,
     * window-state flips): when a window moves/resizes under a
     * STATIONARY pointer the platform delivers no motion event, so
     * hover state computed against the OLD geometry sticks (the
     * classic highlight-that-never-clears on a maximize button).
     * NULL = unsupported; revalidation is skipped, nothing else
     * changes.
     *
     * window_set_cursor selects the cursor shape shown over this
     * window. `edge` is 0 (default arrow) or an
     * fdk_window_resize_edge compass value (plain int here so this
     * header needn't depend on window_internal.h): FDK's resize-edge
     * zones use it to show the directional resize cursor BEFORE any
     * button is held — the affordance users expect from a resizable
     * window. Called only on shape transitions (the window layer
     * caches the applied shape). NULL = the backend cannot shape
     * cursors; the platform default applies (documented per-backend —
     * honest unsupported, no lying fallback). */
    int (*window_query_pointer)(fdk_platform_window *pwindow,
                                fdk_i32 *out_x, fdk_i32 *out_y);
    void (*window_set_cursor)(fdk_platform_window *pwindow, int edge);

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

    /* OPTIONAL first-commit query, test/diagnostic seam behind the
     * internal fdk__window_ever_presented(): returns nonzero once a
     * frame has actually reached the screen (a real commit on
     * Wayland, an XPutImage on X11), 0 before that. NULL means
     * "unknown" (-1 to the caller). Exists because Wayland's
     * configure handshake can leave an app's first present DEFERRED
     * (see the deferred-first-frame handling in wayland_window.c) —
     * the regression test for that path needs an honest, observable
     * answer to "is this window actually on screen yet". */
    int (*window_ever_presented)(fdk_platform_window *pwindow);

    /* OPTIONAL HiDPI scale query behind fdk_window_get_scale()
     * (Phase 3 completion): writes the window's current scale factor
     * (1.0 = one buffer pixel per logical unit) to *out_scale.
     * NULL means "always 1.0" — the honest answer for X11, whose
     * core protocol has no scale concept (X11 windows are their
     * pixels; HiDPI there is a font-size/DPI convention outside
     * FDK's scope, see docs/rendering.md). The Wayland backend
     * reports its live wl_surface buffer scale / fractional
     * preferred scale. */
    fdk_result (*window_get_scale)(fdk_platform_window *pwindow,
                                   fdk_f32 *out_scale);

    /* ---- OPTIONAL clipboard operations (Phase 9) ----
     *
     * Context-wide (not per-window) text clipboard behind
     * include/fdk/fdk_clipboard.h. NULL = the backend has no
     * clipboard support; the frontend reports FDK_ERR_UNSUPPORTED /
     * NULL with a warning rather than failing silently.
     *
     * clipboard_set_text makes the backend the clipboard owner and
     * keeps a copy to serve later requests (X11 SelectionRequest /
     * Wayland wl_data_source.send). `text` is NUL-terminated and
     * never NULL; the backend copies it. Replaces any previous
     * content.
     *
     * clipboard_get_text returns a freshly allocated NUL-terminated
     * UTF-8 string owned by the caller (fdk_free), or NULL when the
     * clipboard is empty/unreadable. Implementations must be
     * synchronous from the caller's perspective and internally
     * bounded (X11's convert-and-wait may not hang on a stuck
     * owner) and must not re-enter dispatch re-entrantly. */
    fdk_result (*clipboard_set_text)(fdk_platform_connection *conn,
                                     const char *text);
    char *(*clipboard_get_text)(fdk_platform_connection *conn);

    /* ---- OPTIONAL popup/dialog grab operations (Phase 9
     * completion — the menu machinery) ----
     *
     * window_popup_regrab re-asserts an existing popup's input grab
     * after a popup stacked ABOVE it closed: server/compositor grabs
     * do not stack (a nested popup's grab replaces its parent's, and
     * closing the child does not restore the parent's). Called only
     * on popup windows that grabbed at show. NULL = the backend
     * cannot re-grab (keyboard nav after submenu dismissal then
     * degrades to the pre-grab focus, documented).
     *
     * window_set_modal takes (modal=true) or releases (false) a
     * POINTER+KEYBOARD grab on a TOPLEVEL window for modal dialogs:
     * while grabbed, no other window on the connection receives
     * input. Unlike popup grabs, presses outside the window are NOT
     * dismissals — they arrive as ordinary out-of-bounds events and
     * are ignored (the modal contract is "input waits for the
     * dialog", not "click away closes it"). NULL or
     * FDK_ERR_UNSUPPORTED = the backend cannot do modal grabs
     * (Wayland: no toplevel-grab protocol exists — dialogs there
     * are non-modal, documented honestly). */
    void (*window_popup_regrab)(fdk_platform_window *pwindow);
    fdk_result (*window_set_modal)(fdk_platform_window *pwindow,
                                   bool modal);
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
