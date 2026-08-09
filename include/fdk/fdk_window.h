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
 * (resize) notification. Custom window decorations (FDK-drawn title
 * bars) are NOT implemented yet — see docs/roadmap.md, Phase 7. What
 * you get today are whatever decorations the platform/compositor
 * draws (X11 window manager decorations, or Wayland xdg-decoration
 * server-side decorations where the compositor supports them).
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
 * not FDK's. */
void fdk_window_set_size_limits(fdk_window *window, fdk_size min_size, fdk_size max_size);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WINDOW_H */
