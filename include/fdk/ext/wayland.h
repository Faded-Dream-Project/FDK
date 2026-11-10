/*
 * fdk/ext/wayland.h — the OPT-IN Wayland escape hatch (1.0.0).
 *
 * This header is NOT included by fdk.h and includes native Wayland
 * client headers itself: including it IS the opt-in, and it requires
 * libwayland-client's dev headers at compile time. The "no backend
 * leakage" rule applies to the CORE public headers only — an add-on
 * that speaks Wayland protocols gets first-class, documented handles
 * here (the old "workarounds banned" rule is gone; the coexistence
 * rules below replace it).
 *
 * Coexistence (docs/EXTENDING.md §10): bind your own globals through
 * your OWN wl_display_get_registry() (FDK's listeners are
 * dispatch-safe, so the roundtrip is harmless at any time); integrate
 * your own event queue with the fd-source API (watch
 * wl_display_get_fd() through fdk_ext_loop_add_fd and
 * wl_display_prepare_read()/dispatch_pending() in the callback); raw
 * input objects are read-only truth — deliver synthetic input by
 * FDK's public APIs, never by injecting into FDK's seat handlers.
 */
#ifndef FDK_EXT_WAYLAND_H
#define FDK_EXT_WAYLAND_H

#include <wayland-client-core.h>
#include <xkbcommon/xkbcommon.h>

#include "../fdk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The connection FDK drives. Valid until fdk_shutdown. NULL when the
 * context has no live connection or runs on X11. */
struct wl_display *fdk_ext_wl_display(fdk_context *ctx);

/* FDK's own registry (binding your globals on it is legal and
 * dispatch-safe; you may also make your own). */
struct wl_registry *fdk_ext_wl_registry(fdk_context *ctx);

/* The seats FDK bound (the count is fixed after connect; order
 * stable). NULL beyond the count. */
size_t fdk_ext_wl_seat_count(fdk_context *ctx);
struct wl_seat *fdk_ext_wl_seat(fdk_context *ctx, size_t index);

/* The connection's bound wl_outputs (the same objects FDK's output
 * model drives — the scale/geometry listeners are FDK's, dispatching
 * on FDK's queues; an add-on may pass them to role protocols like
 * ext-session-lock's get_lock_surface). The count includes outputs
 * that disappeared (their registry global was retired); such entries
 * still return their (zombie) handle — a lock surface on a dead
 * output is the compositor's refusal, not FDK's. Available since
 * extension API v3 (append-only). */
size_t fdk_ext_wl_output_count(fdk_context *ctx);
struct wl_output *fdk_ext_wl_output(fdk_context *ctx, size_t index);

/* The data-device manager FDK bound (NULL when absent). */
struct wl_data_device_manager *fdk_ext_wl_data_device_manager(
    fdk_context *ctx);

/* FDK's xkb_context (for your own keymap compilation; do not free). */
struct xkb_context *fdk_ext_wl_xkb_context(fdk_context *ctx);

#ifdef __cplusplus
}
#endif
#endif /* FDK_EXT_WAYLAND_H */
