/*
 * fdk_window.h — Faded Dream ToolKit window API
 *
 * A top-level window backed by whichever platform backend the owning
 * fdk_context selected (X11 or Wayland — see fdk_core.h). No backend
 * type ever appears here; see docs/architecture.md, "no backend
 * leakage".
 *
 * Threading: window functions are UI-thread-only, like the rest of
 * FDK — see docs/threading.md.
 *
 * Phase 2 scope: creation, destruction, show/hide, title, size
 * (including min/max hints), close-request handling, and configure
 * (resize) notification. Phase 8 adds FDK-drawn window decorations
 * (see fdk_window_set_decorated) and window-state management
 * (maximize / minimize / restore / interactive resize, below).
 */

#ifndef FDK_WINDOW_H
#define FDK_WINDOW_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Options for fdk_window_create(). Zero-initialize (or pass NULL) for
 * sane defaults: title "FDK Application", 640x480, no min/max size
 * constraints. This is an "input struct" per docs/abi-policy.md — safe
 * to zero-init, fields only ever appended in the future. */
typedef struct fdk_window_options {
    const char *title;      /* copied internally; NULL = default title */
    fdk_i32 width;           /* <= 0 means "use default" (640) */
    fdk_i32 height;          /* <= 0 means "use default" (480) */
} fdk_window_options;

/* Creates a top-level window on `ctx`'s platform connection. Writes
 * the new window to *out_window on success.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx or out_window is NULL
 *   FDK_ERR_NOT_INITIALIZED  - ctx has no working platform connection
 *   FDK_ERR_OUT_OF_MEMORY    - allocation failure
 *   FDK_ERR_WINDOW_CREATE    - the platform backend refused/failed
 *
 * The window is NOT shown on the screen until fdk_window_show() is
 * called. On any failure, *out_window is left unchanged. */
fdk_result fdk_window_create(fdk_context *ctx,
                              const fdk_window_options *options,
                              fdk_window **out_window);

/* Maps the window (makes it visible). No-op if already shown. */
void fdk_window_show(fdk_window *window);

/* Unmaps the window (hides it without destroying it). No-op if
 * already hidden. The window's state (title, size) is preserved and
 * fdk_window_show() will restore visibility. */
void fdk_window_hide(fdk_window *window);

/* Destroys the window and releases its platform resources. `window`
 * must not be used after this call. Passing NULL is a safe no-op.
 * Does NOT call fdk_shutdown() on the owning context. */
void fdk_window_destroy(fdk_window *window);

/* Sets the window title. Safe to call before or after fdk_window_show().
 * `title` is copied internally; the caller retains ownership of the
 * string passed in. NULL is treated as an empty title. */
void fdk_window_set_title(fdk_window *window, const char *title);

/* Requests a resize to (width, height). This is a request, not a
 * guarantee — the platform/compositor may clamp, ignore, or adjust it
 * (e.g. Wayland only actually resizes a toplevel in response to a
 * compositor-driven configure; FDK sends the request and the actual
 * resulting size arrives via FDK_EVENT_WINDOW_CONFIGURE). Both width
 * and height must be > 0. */
void fdk_window_resize(fdk_window *window, fdk_i32 width, fdk_i32 height);

/* Returns the window's last-known size (from the most recent configure
 * event, or the creation size if none has arrived yet) via *out_size.
 * Returns FDK_ERR_INVALID_ARGUMENT if window or out_size is NULL. */
fdk_result fdk_window_get_size(const fdk_window *window, fdk_size *out_size);

/* Sets minimum/maximum size hints. Pass 0 for either dimension of
 * either struct to mean "no constraint" in that dimension. These are
 * hints handed to the platform (WM_NORMAL_HINTS on X11, xdg_toplevel
 * min/max on Wayland) — enforcement is the platform's responsibility,
 * not FDK's — with one exception: FDK's own resize-edge drags (see
 * fdk_window_set_resizable) also clamp to these limits, because on a
 * bare X server there is no window manager to do it. */
void fdk_window_set_size_limits(fdk_window *window, fdk_size min_size, fdk_size max_size);

/* ---- Window state (Phase 8) ----
 *
 * All of these are REQUESTS — the platform may clamp, ignore, or
 * adjust them, exactly like fdk_window_resize. FDK reports what
 * actually happened through FDK_EVENT_WINDOW_STATE (see fdk_event.h);
 * fdk_window_is_maximized()/is_minimized() read FDK's last-known
 * state, not the request. */

/* Asks for the window to fill the screen (EWMH _NET_WM_STATE
 * maximized under a conforming X11 WM; xdg_toplevel.set_maximized on
 * Wayland; on a bare X server FDK itself moves+resizes the window to
 * the full screen, remembering the prior geometry for unmaximize).
 * Returns FDK_ERR_UNSUPPORTED when the backend cannot maximize at
 * all; FDK_OK means the request was sent/applied — watch
 * FDK_EVENT_WINDOW_STATE for the confirmed state. */
fdk_result fdk_window_maximize(fdk_window *window);

/* Returns a maximized window to its remembered (or compositor-
 * chosen) geometry. Same request/confirmation contract as
 * fdk_window_maximize(). */
fdk_result fdk_window_unmaximize(fdk_window *window);

/* Asks for the window to be minimized/iconified. On X11 this is the
 * ICCCM iconify request (any window manager honors it; without one,
 * FDK unmaps the window itself). On Wayland this is
 * xdg_toplevel.set_minimized — a fire-and-forget request: the
 * protocol has no un-minimize request and no acknowledgement, so
 * FDK marks the state optimistically and clears it when the
 * compositor next reports the window activated. */
fdk_result fdk_window_minimize(fdk_window *window);

/* Restores a minimized window (un-iconifies it). Unsupported on
 * Wayland (no protocol request exists — compositors unminimize via
 * activation, e.g. a taskbar click); returns FDK_ERR_UNSUPPORTED
 * there without changing anything. */
fdk_result fdk_window_restore(fdk_window *window);

/* FDK's last-known maximized/minimized state (from
 * FDK_EVENT_WINDOW_STATE). Note this is state-as-reported, not
 * request-as-sent — see the request/confirmation contract above. */
bool fdk_window_is_maximized(const fdk_window *window);
bool fdk_window_is_minimized(const fdk_window *window);

/* ---- Interactive resize (Phase 8) ----
 *
 * When enabled, a border-wide zone around the window's edges and
 * corners captures pointer drags and resizes the window — the CSD
 * (client-side decorations) answer to "the WM frame is gone, so who
 * draws the resize handles?". Drags are handed to the WM/compositor
 * where the platform supports it (EWMH _NET_WM_MOVERESIZE on X11,
 * xdg_toplevel.resize on Wayland); otherwise FDK drives the resize
 * itself, clamping to fdk_window_set_size_limits.
 *
 * The zone overlays the outermost pixels of the window content —
 * widgets living under it don't see presses there (the same
 * trade-off every CSD toolkit makes). fdk_window_set_decorated(true)
 * enables it automatically: a window with FDK's own title bar has no
 * WM frame left to resize it. Call set_resizable(false) AFTER
 * decorating for a fixed-size decorated window. */
void fdk_window_set_resizable(fdk_window *window, bool resizable);
bool fdk_window_get_resizable(const fdk_window *window);

/* ---- FDK-drawn decorations (Phase 8) ----
 *
 * decorated == true: FDK draws its own title bar INSIDE the client
 * area — a themed band with the window title on the left and a close
 * button on the right — and asks the backend to drop the window
 * manager's own chrome (X11: _MOTIF_WM_HINTS). Dragging the band
 * moves the window where the backend allows it (X11). The close
 * button delivers a normal FDK_EVENT_WINDOW_CLOSE_REQUEST, so the
 * application's close semantics are identical to the WM's.
 *
 * The title bar is a widget subtree under the window's root: it is
 * themed like every catalog widget (fdk_theme_set_default repaints
 * it), fdk_window_set_title keeps its text in sync, and the content
 * widget (fdk_window_set_content) is laid out BELOW the band.
 *
 * Returns FDK_ERR_UNSUPPORTED when the backend cannot drop its own
 * decorations (Wayland until xdg-decoration lands) — in that case
 * nothing changes: drawing FDK's bar over the compositor's would
 * give every window two title bars.
 */
fdk_result fdk_window_set_decorated(fdk_window *window, bool decorated);

/* Whether FDK decorations are currently on. */
bool fdk_window_get_decorated(const fdk_window *window);

/* The title bar's font. NULL (the default) uses
 * fdk_font_load_system_default() — FDK bundles no font, so a system
 * with none renders a bar without title text (the close button and
 * drag still work; a warning is logged once). The font is BORROWED:
 * keep it alive until the window is destroyed or another font is
 * set. */
fdk_result fdk_window_set_decoration_font(fdk_window *window,
                                          fdk_font *font);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WINDOW_H */
