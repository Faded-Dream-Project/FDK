/*
 * wayland_platform.h — Wayland backend internals
 *
 * Only included by files inside src/platform/wayland/. No file
 * outside this directory may include wayland-client.h or the
 * generated xdg-shell protocol headers — same "no backend leakage"
 * rule as x11_platform.h, see docs/architecture.md.
 */

#ifndef FDK_WAYLAND_PLATFORM_H
#define FDK_WAYLAND_PLATFORM_H

#include "platform/platform_internal.h"

#include "platform/wayland/generated/xdg-shell-client-protocol.h"

#include <stddef.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct fdk_platform_connection {
    struct wl_display *display;
    struct wl_registry *registry;

    /* Globals discovered during the initial registry roundtrip
     * (see wayland_registry.c). All required for Phase 2's
     * "create a window, get input, handle close/resize" scope —
     * none are optional the way e.g. a clipboard-manager global
     * would be. */
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm_base;

    /* Bound from the seat's capabilities (wayland_seat.c). May be
     * NULL if the seat genuinely has no keyboard/pointer — checked
     * before use, never assumed present. */
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;

    /* xkbcommon keymap state, populated from the compositor-supplied
     * keymap (wl_keyboard::keymap event) rather than FDK guessing at
     * a layout — this is the standard Wayland approach, in contrast
     * to X11 where XLookupString queries the X server's active
     * layout directly (see x11_events.c). */
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* Pointer focus tracking: Wayland delivers pointer button/motion
     * events against whichever surface last got a wl_pointer::enter,
     * not tagged per-event the way X11's XEvent carries .window. */
    fdk_platform_window *pointer_focus;
    fdk_platform_window *keyboard_focus;
    double pointer_x, pointer_y; /* last known position, surface-local */

    fdk_platform_dispatch_fn dispatch;
    void *dispatch_user_data;

    /* Same registry pattern as the X11 backend and fdk_context — see
     * x11_platform.h's struct fdk_platform_connection comment and
     * context_internal.h for the shared rationale. */
    fdk_platform_window **windows;
    size_t window_count;
    size_t window_capacity;

    int last_dispatch_errno; /* set by wl_display_dispatch() failure */
};

struct fdk_platform_window {
    fdk_platform_connection *conn;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    fdk_size last_size;
    fdk_size pending_size;   /* accumulated from configure until ack+commit */
    int configured;          /* nonzero once the first xdg_surface.configure
                                 has been acked — required before the first
                                 wl_surface.commit per xdg-shell protocol */
};

/* Registry helpers, implemented in wayland_registry.c. */
fdk_result fdk_wayland_register_window(fdk_platform_connection *conn,
                                        fdk_platform_window *pwindow);
void fdk_wayland_unregister_window(fdk_platform_connection *conn,
                                    fdk_platform_window *pwindow);

/* Seat/input listener setup, implemented in wayland_seat.c, called
 * once compositor/shm/seat/wm_base are all bound (wayland_registry.c). */
void fdk_wayland_bind_seat_listeners(fdk_platform_connection *conn);
void fdk_wayland_teardown_seat(fdk_platform_connection *conn);

/* Declared here, defined across wayland_connection.c, wayland_window.c
 * dispatch is wl_display_dispatch() itself (no separate translate step
 * the way X11 has — see wayland_dispatch.c's doc comment for why).
 * Assembled into the fdk_platform_ops vtable in wayland_ops.c. */
fdk_result fdk_wayland_connect(fdk_platform_dispatch_fn dispatch,
                                void *dispatch_user_data,
                                fdk_platform_connection **out_conn);
void fdk_wayland_disconnect(fdk_platform_connection *conn);
int fdk_wayland_get_event_fd(fdk_platform_connection *conn);
int fdk_wayland_dispatch_pending(fdk_platform_connection *conn);

fdk_result fdk_wayland_window_create(fdk_platform_connection *conn,
                                      const fdk_window_options *options,
                                      fdk_platform_window **out_pwindow);
void fdk_wayland_window_destroy(fdk_platform_window *pwindow);
void fdk_wayland_window_show(fdk_platform_window *pwindow);
void fdk_wayland_window_hide(fdk_platform_window *pwindow);
void fdk_wayland_window_set_title(fdk_platform_window *pwindow, const char *title);
void fdk_wayland_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height);
void fdk_wayland_window_set_size_limits(fdk_platform_window *pwindow,
                                         fdk_size min_size, fdk_size max_size);

#endif /* FDK_WAYLAND_PLATFORM_H */
