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
#include "platform/wayland/generated/xdg-decoration-unstable-v1-client-protocol.h"

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

    /* OPTIONAL global (Phase 8): zxdg_decoration_manager_v1 — absent
     * means the compositor offers no xdg-decoration protocol, and
     * fdk_window_set_decorated() then honestly returns
     * FDK_ERR_UNSUPPORTED rather than stacking FDK's band over the
     * compositor's server-side decorations. */
    struct zxdg_decoration_manager_v1 *decoration_manager;

    /* --- HiDPI (Phase 3 completion) --------------------------------
     *
     * wl_output globals are bound (version >= 2 for the scale event)
     * and tracked so windows can derive their scale from the outputs
     * they are displayed on (xdg_toplevel enter/leave). Both optional
     * globals below extend scale support to FRACTIONAL factors:
     *   - wp_viewporter: maps an exact source rectangle of the buffer
     *     onto the surface (the mechanism fractional scaling needs);
     *   - wp_fractional_scale_manager_v1: the compositor's preferred
     *     scale in 120ths of a unit, per window.
     * When either is missing, FDK uses integer wl_surface buffer
     * scale only — correct on every compositor, fractional factors
     * just round to the nearest supported integer. */
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;

    /* Bound outputs (grown on demand; wl_output v2+ carries the
     * scale event). Kept for the window's lifetime of the
     * connection; outputs that disappear are left in place with
     * scale 0 = "gone" and skipped by the max-scale walk. */
    struct {
        struct wl_output *output;
        int scale; /* wl_output::scale, or 0 once the global is gone */
    } *outputs;
    size_t output_count;
    size_t output_capacity;

    /* Serial of the most recent pointer-button event (Phase 8):
     * xdg_toplevel.move/resize require the serial of the triggering
     * input event — the compositor validates it against its own
     * event history, so a stale serial makes the request a no-op. */
    uint32_t last_button_serial;

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

#define FDK_WL_RENDER_SLOTS 4

/* Frame-pacing guard: if the compositor stays silent this long after
 * a commit (ms), fdk_wayland_window_frame_ready() reports ready
 * anyway. Hidden surfaces legitimately never receive frame
 * callbacks; FDK's contract is to pace, never to starve. */
#define FDK_WL_FRAME_GUARD_MS 250

struct fdk_platform_window {
    fdk_platform_connection *conn;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    /* Solid background buffer — the Wayland equivalent of X11's
     * background pixel. Wayland compositors map nothing until a
     * client commits an actual buffer, so without this a Phase 2
     * window would be invisible (see attach_background_buffer() in
     * wayland_window.c). wl_shm_pool objects are destroyed at buffer
     * creation time — legal per the wl_shm spec, which keeps the
     * server-side mapping alive until the buffer itself is destroyed
     * — so only the buffer needs tracking here. The buffer is
     * destroyed from its wl_buffer::release listener, never reused:
     * content never changes, a resize simply makes a new one.
     *
     * Once an application renders via fdk_surface, this background
     * path is bypassed (rendered_ever / render_pending below) and the
     * committed render buffers take over visibility duty. */
    struct wl_buffer *buffer;   /* current background buffer, NULL when none */
    fdk_size buffer_size;       /* dimensions of `buffer` */
    int buffer_attached;        /* nonzero while `buffer` is committed to the surface */

    /* --- Software rendering (fdk_surface machinery) ---
     *
     * A small fixed pool of wl_shm buffers that are RECYCLED, not
     * destroyed: once the compositor releases a buffer
     * (wl_buffer::release), the buffer and its client-side mapping
     * stay alive in its slot, marked `released`, ready for the next
     * acquisition. This is what makes damage-tracked partial redraw
     * CORRECT and not merely fast: a recycled (or freshly created)
     * buffer is pre-filled with a copy of the currently visible
     * frame before being handed to the application, so every pixel
     * outside the newly drawn damage region matches the screen —
     * required, because compositors are allowed to ignore damage
     * hints and scan out the whole buffer. A buffer is only truly
     * destroyed on window destruction or a size change.
     *
     * Slot states: buffer == NULL -> free. buffer != NULL &&
     * buffer == render_pending -> acquired by the app, being drawn.
     * buffer != NULL && buffer == the surface's live `buffer` ->
     * currently committed/visible. Otherwise in flight (or released
     * and awaiting reuse). */
    struct {
        struct wl_buffer *buffer; /* NULL = slot free */
        uint32_t *pixels;         /* mmap mapping backing `buffer` */
        size_t length;            /* mapping size in bytes */
        fdk_size size;            /* buffer dimensions */
        int released;             /* compositor done -> reusable */
    } render_slots[FDK_WL_RENDER_SLOTS];
    struct wl_buffer *render_pending; /* acquired but not yet presented */
    int rendered_ever;                /* first render present() completed */

    /* --- HiDPI (Phase 3 completion) --------------------------------
     *
     * The window's scale in 120ths of a logical unit (240 = 2x,
     * 300 = 2.5x), exactly the fractional-scale-v1 unit — integer
     * factors are multiples of 120. Sources, in precedence order:
     * the compositor's wp_fractional_scale_v1::preferred_scale (when
     * the protocol pair is available), else the maximum wl_output
     * scale among the outputs the toplevel is currently shown on
     * (xdg_toplevel enter/leave), else 120 = 1x. buffer_scale is the
     * INTEGER wl_surface scale actually applied (exact for integer
     * factors; the viewport carries the fractional remainder — see
     * apply_window_scale()). Render buffers are PHYSICAL pixels:
     * logical size x buffer_scale (rounded up under a viewport).
     * scale_applied tracks that the protocol objects match the
     * current value (re-applied on change before the next commit). */
    int scale_x120;      /* preferred scale, 120ths (>= 120)          */
    int buffer_scale;    /* integer wl_surface buffer scale (>= 1)    */
    int scale_applied;   /* scale_x120 pushed to the protocol yet?    */
    struct wp_viewport *viewport;          /* NULL without viewporter */
    struct wp_fractional_scale_v1 *fractional; /* NULL without mgr    */

    /* Outputs this toplevel currently occupies (xdg_toplevel
     * enter/leave), driving the output-derived scale when the
     * fractional-scale protocol is unavailable. Small dynamic array
     * of borrowed wl_output pointers; capacity grows on demand. */
    struct wl_output **entered_outputs;
    size_t entered_count;
    size_t entered_capacity;

    /* --- Frame pacing (wl_surface.frame) ---
     *
     * Every render present requests a frame callback after its
     * commit; the callback (frame_callback_done below) sets
     * frame_ack, which fdk_wayland_window_frame_ready() reports —
     * together with the FDK_WL_FRAME_GUARD_MS starvation guard and
     * the "never presented yet" case. Callbacks can only arrive
     * while the application pumps events; that contract is
     * documented in fdk_surface.h's frame-pacing section. */
    int frame_ack;        /* compositor acknowledged the last frame */
    fdk_i64 frame_commit_ms; /* monotonic ms of the last render commit */

    /* The pending wl_surface.frame callback, destroyed when `done`
     * arrives (frame_callback_done) OR at window destruction — a
     * callback whose window dies first would otherwise leak its
     * proxy (a real leak the sway-headless test caught: present ->
     * destroy before the compositor answers). */
    struct wl_callback *frame_cb;

    fdk_size last_size;
    fdk_size pending_size;   /* accumulated from configure until ack+commit */
    int configured;          /* nonzero once the first xdg_surface.configure
                                 has been acked — required before the first
                                 wl_surface.commit per xdg-shell protocol */

    /* --- Phase 8: window-state + decoration bookkeeping ---
     *
     * maximized is derived from xdg_toplevel::configure's states
     * array (the compositor's word, not our request); minimized is
     * request-optimistic — the protocol has no minimized state in
     * configure and no acknowledgement of set_minimized, so it is
     * cleared on the next activated configure (compositors send
     * activated when a window is un-minimized into focus). Flips
     * dispatch FDK_EVENT_WINDOW_STATE.
     *
     * toplevel_decoration is the per-window xdg-decoration object,
     * created on the first set_wm_decorations() call and alive until
     * window destruction; deco_client_side caches the compositor's
     * last confirmed mode (its configure event) — when it answers
     * SERVER_SIDE against our CLIENT_SIDE request, FDK emits
     * FDK_EVENT_WINDOW_DECORATION so the window layer drops its band
     * instead of double-decorating. */
    int maximized;
    int minimized;
    struct zxdg_toplevel_decoration_v1 *toplevel_decoration;
    int deco_client_side;
};

/* Registry helpers, implemented in wayland_registry.c. */
/* HiDPI output tracking (wayland_registry.c). track_output binds
 * listener state for a freshly bound wl_output; destroy_outputs
 * releases everything at disconnect. */
void fdk_wayland_track_output(fdk_platform_connection *conn,
                              struct wl_output *output);
void fdk_wayland_destroy_outputs(fdk_platform_connection *conn);

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

/* Phase 8 window management (see platform_internal.h for the vtable
 * contract of each): */
fdk_result fdk_wayland_window_set_wm_decorations(fdk_platform_window *pwindow,
                                                 bool on);
fdk_result fdk_wayland_window_set_maximized(fdk_platform_window *pwindow,
                                            bool maximized);
fdk_result fdk_wayland_window_set_minimized(fdk_platform_window *pwindow,
                                            bool minimized);
fdk_result fdk_wayland_window_begin_move(fdk_platform_window *pwindow,
                                         fdk_i32 local_x, fdk_i32 local_y);
fdk_result fdk_wayland_window_begin_resize(fdk_platform_window *pwindow,
                                           int edge, fdk_i32 local_x,
                                           fdk_i32 local_y);
void fdk_wayland_window_update_state(fdk_platform_window *pwindow,
                                     int maximized, int minimized);

/* Software rendering (fdk_surface machinery) — see the render_slots
 * comment in struct fdk_platform_window above for the recycling
 * lifecycle. */
fdk_result fdk_wayland_window_get_framebuffer(fdk_platform_window *pwindow,
                                               fdk_platform_framebuffer *out_fb);

/* HiDPI (Phase 3 completion): recompute the window's scale from the
 * enter/leave output set (wayland_window.c), and the
 * fdk_window_get_scale() query op. */
fdk_result fdk_wayland_window_recompute_scale(
    fdk_platform_window *pwindow, struct wl_output *output, int entered);
fdk_result fdk_wayland_window_get_scale(fdk_platform_window *pwindow,
                                        fdk_f32 *out_scale);
fdk_result fdk_wayland_window_present(fdk_platform_window *pwindow,
                                      const fdk_platform_damage *damage);
int fdk_wayland_window_frame_ready(fdk_platform_window *pwindow);

#endif /* FDK_WAYLAND_PLATFORM_H */
