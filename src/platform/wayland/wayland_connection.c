#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/viewporter-client-protocol.h"
#include "generated/fractional-scale-v1-client-protocol.h"
#include "generated/primary-selection-unstable-v1-client-protocol.h"
#include "generated/cursor-shape-v1-client-protocol.h"
#include "generated/xdg-toplevel-icon-v1-client-protocol.h"
#include "generated/text-input-unstable-v3-client-protocol.h"
#include "generated/idle-inhibit-unstable-v1-client-protocol.h"
#include "generated/xdg-activation-v1-client-protocol.h"
#include "generated/tablet-unstable-v2-client-protocol.h"
#include "generated/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "generated/xdg-session-management-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version) {
    fdk_platform_connection *conn = data;
    /* Bind-version discipline (1.3.1): bind at min(wanted,
     * advertised). Requesting a version HIGHER than the compositor
     * advertised is a client protocol error that KILLS the
     * connection — v1 hardcoded its wanted versions, which worked
     * on the rigs it was tested against (sway/weston advertise
     * plenty) and would have died on any older compositor. */
    uint32_t v = version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (v > 4) v = 4;
        conn->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, v);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        if (v > 1) v = 1;
        conn->shm = wl_registry_bind(registry, name, &wl_shm_interface, v);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        if (v > 5) v = 5;
        conn->seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        if (v > 1) v = 1;
        conn->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, v);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        /* OPTIONAL global (Phase 8): its absence is not an error and
         * costs nothing client-side — a compositor without this
         * protocol never draws chrome itself (client-side is the
         * xdg-shell default), so FDK's own band still comes up; only
         * the platform-chrome direction (set_decorated(false))
         * reports FDK_ERR_UNSUPPORTED then. */
        uint32_t dv = version < 1 ? version : 1;
        conn->decoration_manager =
            wl_registry_bind(registry, name,
                             &zxdg_decoration_manager_v1_interface, dv);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        /* OPTIONAL global (Phase 9): wl_data_device_manager is the
         * clipboard (and drag-and-drop) factory. Absent -> the
         * clipboard ops honestly report FDK_ERR_UNSUPPORTED instead
         * of faking a local-only clipboard. Version 3 is enough for
         * selections (drag-and-drop's data_source actions arrive with
         * v3 and are ignored — FDK does no DnD). */
        conn->data_device_manager =
            wl_registry_bind(registry, name,
                             &wl_data_device_manager_interface,
                             version < 3 ? version : 3);
        fdk_wayland_clipboard_device_ready(conn);
    } else if (strcmp(interface,
                      zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        /* OPTIONAL global (1.3.4): the PRIMARY selection protocol —
         * the classic Unix "whatever is selected" buffer, the one
         * middle-click pastes. The wlroots family (sway, labwc,
         * river) offers it; compositors that don't leave it NULL and
         * the primary ops report FDK_ERR_UNSUPPORTED instead of
         * faking a local-only buffer. Version 1 is the whole
         * protocol. The device itself materializes once the SEAT
         * also exists (clipboard_device_ready creates both). */
        uint32_t pv = version < 1 ? version : 1;
        conn->primary_manager =
            wl_registry_bind(registry, name,
                             &zwp_primary_selection_device_manager_v1_interface,
                             pv);
        fdk_wayland_clipboard_device_ready(conn);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        /* HiDPI (Phase 3 completion): bind every output at the
         * highest version both sides support, capped at 3 — v2 adds
         * the scale + done events (the minimum FDK needs), v3 only
         * adds name/description strings FDK has no use for. The
         * registry NAME rides along so global_remove can retire the
         * right record (1.3.1). */
        uint32_t ov = version < 2 ? version : (version > 3 ? 3 : version);
        struct wl_output *output =
            wl_registry_bind(registry, name, &wl_output_interface, ov);
        if (output != NULL) {
            fdk_wayland_track_output(conn, output, name);
        }
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        /* OPTIONAL (HiDPI): the source-rectangle mechanism fractional
         * scaling rides on. Absent -> integer buffer scale only. */
        uint32_t vv = version < 1 ? version : 1;
        conn->viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, vv);
    } else if (strcmp(interface,
                      wp_fractional_scale_manager_v1_interface.name) == 0) {
        /* OPTIONAL (HiDPI): per-window preferred scale in 120ths.
         * Absent -> integer buffer scale only. */
        uint32_t fv = version < 1 ? version : 1;
        conn->fractional_manager =
            wl_registry_bind(registry, name,
                             &wp_fractional_scale_manager_v1_interface, fv);
    } else if (strcmp(interface,
                      wp_cursor_shape_manager_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0-rc1, W2): cursor-shape-v1 — the compositor
         * renders enumerated cursor shapes itself. Preferred over the
         * client-side XCursor-theme path whenever offered: no theme
         * files to hunt, and the shape follows the compositor's own
         * cursor pipeline. Version 1 carries everything FDK uses
         * (set_shape + the shape enum); v2 adds no request we need,
         * so the bind stays at the offered version capped at 1. The
         * per-pointer device is created when the seat's pointer
         * materializes (wayland_seat.c). */
        uint32_t cv = version < 1 ? version : 1;
        conn->cursor_shape_manager =
            wl_registry_bind(registry, name,
                             &wp_cursor_shape_manager_v1_interface, cv);
    } else if (strcmp(interface,
                      xdg_toplevel_icon_manager_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0, M-4): xdg-toplevel-icon-v1 — the compositor
         * accepts per-toplevel window icons (taskbars, docks,
         * switchers). KDE Plasma 6.3+ and adopters of the staging
         * protocol; absent on wlroots < 0.19 / GNOME at bind time.
         * v1 is the only released version; the advisory icon_size
         * /done events are not listened to (FDK ships raster pixels
         * and lets the compositor scale — see wayland_platform.h). */
        uint32_t iv = version < 1 ? version : 1;
        conn->toplevel_icon_manager =
            wl_registry_bind(registry, name,
                             &xdg_toplevel_icon_manager_v1_interface, iv);
    } else if (strcmp(interface,
                      zwp_text_input_manager_v3_interface.name) == 0) {
        /* OPTIONAL (1.1.0, M-1): text-input-v3 — the IME composition
         * protocol. Absent (no compositor IME support) means no
         * composition on Wayland; keys keep flowing through
         * wl_keyboard. The per-seat text input object is created
         * once the seat binds (fdk_wayland_ime_seat_ready — the
         * manager may bind before or after the seat). */
        uint32_t tv = version < 1 ? version : 1;
        conn->text_input_manager =
            wl_registry_bind(registry, name,
                             &zwp_text_input_manager_v3_interface, tv);
        fdk_wayland_ime_seat_ready(conn);
    } else if (strcmp(interface,
                      zwp_idle_inhibit_manager_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0, S-3): idle-inhibit — absent means the
         * window op answers UNSUPPORTED and the D-Bus ScreenSaver
         * fallback carries idle inhibition instead. */
        conn->idle_inhibit_manager =
            wl_registry_bind(registry, name,
                             &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (strcmp(interface,
                      zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0, D-4): wlr-foreign-toplevel-management —
         * the taskbar substrate's factory. Only the NAME is taken
         * here: the manager is bound at fdk_toplevel_list_open
         * (wayland_toplevel.c) so the protocol's initial toplevel
         * burst meets an attached listener instead of the void.
         * Absent (GNOME, KDE pre-5.x, kiosk shells) = open answers
         * NULL — the honest degradation, never an error. */
        conn->foreign_toplevel_name = name;
    } else if (strcmp(interface,
                      xdg_session_manager_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0, D-5): xdg-session-management-v1 — the
         * save/restore object lane's factory (wayland_session.c).
         * NAME-only, manager bound lazily at fdk_session_open:
         * unlike foreign-toplevel there is no bind-time burst to
         * lose (get_session is client-initiated), but the lazy
         * shape keeps the feature entirely off the wire for the
         * apps that never open a session. Absent (every wlroots
         * compositor at the time of writing — the protocol is
         * weeks old in wayland-protocols 1.48) = open answers NULL,
         * the documented degradation. Version 1 is the whole
         * protocol. */
        conn->session_manager_name = name;
    } else if (strcmp(interface,
                      zwp_tablet_manager_v2_interface.name) == 0) {
        /* OPTIONAL (1.1.0, D-3): the tablet family's factory — the
         * manager is the bind point; the per-seat tablet seat (and
         * with it every tool stream) materializes once the SEAT also
         * exists (fdk_wayland_tablet_seat_ready, called from the
         * seat's own bind and its capabilities round). Absent
         * manager (GNOME before 42-class compositors, kiosk shells)
         * means no tablet events — never an error. Version 1 is the
         * whole protocol. */
        uint32_t tv = version < 1 ? version : 1;
        conn->tablet_manager =
            wl_registry_bind(registry, name,
                             &zwp_tablet_manager_v2_interface, tv);
        fdk_wayland_tablet_seat_ready(conn);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        /* OPTIONAL (1.1.0, D-2): wl_subcompositor — the subsurface
         * factory, CORE protocol (ships with libwayland itself, no
         * generated code needed) but still a per-compositor global.
         * Every compositor that offers wl_compositor offers this one
         * too; absent (never seen in practice) means
         * fdk_ext_subsurface_create answers FDK_ERR_UNSUPPORTED.
         * Version 1 is the whole protocol. */
        uint32_t subv = version < 1 ? version : 1;
        conn->subcompositor =
            wl_registry_bind(registry, name, &wl_subcompositor_interface,
                             subv);
    } else if (strcmp(interface,
                      xdg_activation_v1_interface.name) == 0) {
        /* OPTIONAL (1.1.0, S-6): xdg-activation — the
         * focus-stealing-prevention protocol. Absent (rare: sway,
         * KDE and GNOME all ship it) means activate() and token
         * minting answer UNSUPPORTED. Version 1 is the whole
         * protocol. */
        conn->activation =
            wl_registry_bind(registry, name,
                             &xdg_activation_v1_interface, 1);
    }
    /* Other globals (wl_data_device_manager, etc.) are
     * intentionally not bound — out of current scope, see
     * docs/roadmap.md. Ignoring an unrecognized global is the correct
     * behavior per the registry protocol, not a gap. */
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                    uint32_t name) {
    (void)registry;
    fdk_platform_connection *conn = data;
    /* Output hot-unplug (1.3.1): retire the matching record — scale
     * 0 = "gone", exactly what the max-scale walk already skips (the
     * contract wayland_platform.h always documented but never
     * implemented). A seat/global going away mid-session is logged
     * honestly: correctly detaching live input state bound to a dead
     * global is a real feature, not a one-line fix, and no compositor
     * a sane person runs removes a seat out from under its clients. */
    fdk_wayland_forget_output(conn, name);
    FDK_WARN("registry global %u removed", name);
}

static const struct wl_registry_listener g_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial) {
    (void)data;
    /* Required by the xdg-shell protocol: a client that doesn't
     * respond to pings within a compositor-defined timeout is
     * considered unresponsive (and may get a "not responding"
     * overlay). Must pong immediately, not deferred to app logic. */
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener g_wm_base_listener = {
    .ping = wm_base_ping,
};

fdk_result fdk_wayland_connect(fdk_platform_dispatch_fn dispatch,
                                void *dispatch_user_data, const char *app_id,
                                fdk_platform_connection **out_conn) {
    fdk__startup_trace_begin("wayland:connect complete");
    struct wl_display *display = wl_display_connect(NULL);
    if (display == NULL) {
        FDK_INFO("wl_display_connect failed (no Wayland compositor reachable)");
        return FDK_ERR_NO_DISPLAY;
    }

    fdk_platform_connection *conn = fdk_alloc(sizeof(fdk_platform_connection));
    if (conn == NULL) {
        wl_display_disconnect(display);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    conn->display = display;
    conn->compositor = NULL;
    conn->shm = NULL;
    conn->seat = NULL;
    conn->wm_base = NULL;
    conn->decoration_manager = NULL;
    conn->last_button_serial = 0;
    conn->keyboard = NULL;
    conn->pointer = NULL;
    conn->xkb_context = NULL;
    conn->xkb_keymap = NULL;
    conn->xkb_state = NULL;
    conn->pointer_focus = NULL;
    conn->keyboard_focus = NULL;
    conn->pointer_x = 0.0;
    conn->pointer_y = 0.0;
    conn->dispatch = dispatch;
    conn->dispatch_user_data = dispatch_user_data;
    conn->cursor_surface = NULL;
    conn->cursor_cache = NULL;
    conn->cursor_cache_count = 0;
    conn->cursor_cache_capacity = 0;
    conn->cursor_theme_state = 0;
    conn->app_id = NULL;
    if (app_id != NULL && app_id[0] != '\0') {
        size_t len = strlen(app_id) + 1;
        conn->app_id = fdk_alloc(len);
        if (conn->app_id != NULL) {
            memcpy(conn->app_id, app_id, len);
        }
    }
    if (conn->app_id == NULL) {
        /* 1.1.6: no app-provided identity — derive one from
         * /proc/self/cmdline's first token (argv[0]), basename only.
         * Demos then show up in taskbars/compositor rules as
         * "06_decorations" instead of the generic "fdk.app"; the
         * core log line stays honest (it reports the OPTION, which
         * was unset). Linux-only, best-effort: unreadable /proc or a
         * weird argv[0] just keeps the generic fallback. */
        char argv0[256];
        ssize_t n = -1;
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd >= 0) {
            n = read(fd, argv0, sizeof argv0 - 1);
            close(fd);
        }
        if (n > 0) {
            argv0[n] = '\0'; /* first token is NUL-terminated in the file */
            const char *base = strrchr(argv0, '/');
            base = (base != NULL) ? base + 1 : argv0;
            if (base[0] != '\0') {
                size_t len = strlen(base) + 1;
                conn->app_id = fdk_alloc(len);
                if (conn->app_id != NULL) {
                    memcpy(conn->app_id, base, len);
                    FDK_DEBUG("app_id derived from argv[0]: %s", conn->app_id);
                }
            }
        }
    }
    conn->windows = NULL;
    conn->window_count = 0;
    conn->window_capacity = 0;
    conn->last_dispatch_errno = 0;
    /* GPU lane (1.2.0 / D-1): lazy — created at the first framebuffer
     * acquisition, never here (no-window apps never touch EGL). */
    conn->egl = NULL;
    conn->egl_tried = 0;
    conn->activation = NULL;
    conn->launch_token = NULL;
    /* S-6: the launch token a LAUNCHER vouched for us with — read
     * once and UNSET immediately (the protocol's own hygiene rule:
     * never propagate a spent/stale token to children we spawn).
     * Spent by the first fdk_window_activate() (wayland_window.c). */
    {
        const char *tok = getenv("XDG_ACTIVATION_TOKEN");
        if (tok != NULL && tok[0] != '\0') {
            size_t len = strlen(tok) + 1;
            conn->launch_token = fdk_alloc(len);
            if (conn->launch_token != NULL) {
                memcpy(conn->launch_token, tok, len);
                FDK_INFO("xdg-activation: launch token captured "
                         "(spent by the first activate)");
            }
        }
        unsetenv("XDG_ACTIVATION_TOKEN");
    }

    /* Dedicated wl_buffer event queue (1.1.6) — created before the
     * first wl_buffer can exist (windows come later), so
     * create_shm_buffer's wl_proxy_set_queue never sees NULL. */
    conn->release_queue = wl_display_create_queue(display);
    if (conn->release_queue == NULL) {
        FDK_ERROR("wl_display_create_queue failed");
        wl_display_disconnect(display);
        fdk_free(conn->app_id);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }

    conn->registry = wl_display_get_registry(display);
    if (conn->registry == NULL) {
        FDK_ERROR("wl_display_get_registry failed");
        wl_event_queue_destroy(conn->release_queue);
        wl_display_disconnect(display);
        fdk_free(conn->app_id);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }
    wl_registry_add_listener(conn->registry, &g_registry_listener, conn);

    /* Roundtrip 1: blocks until the compositor has sent every initial
     * wl_registry::global event and we've processed them all via the
     * listener above. This is the standard Wayland client startup
     * pattern — without it, conn->compositor etc. would still be
     * NULL immediately after this function returns. */
    if (wl_display_roundtrip(display) < 0) {
        FDK_ERROR("initial registry roundtrip failed");
        wl_event_queue_destroy(conn->release_queue);
        wl_display_disconnect(display);
        fdk_free(conn->app_id);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }

    if (conn->compositor == NULL || conn->shm == NULL || conn->wm_base == NULL) {
        FDK_ERROR("compositor missing a required global (compositor=%p shm=%p wm_base=%p)",
                  (void *)conn->compositor, (void *)conn->shm, (void *)conn->wm_base);
        if (conn->wm_base) xdg_wm_base_destroy(conn->wm_base);
        if (conn->seat) wl_seat_destroy(conn->seat);
        if (conn->shm) wl_shm_destroy(conn->shm);
        if (conn->subcompositor) {
            wl_subcompositor_destroy(conn->subcompositor);
        }
        if (conn->compositor) wl_compositor_destroy(conn->compositor);
        wl_registry_destroy(conn->registry);
        wl_event_queue_destroy(conn->release_queue);
        wl_display_disconnect(display);
        fdk_free(conn->app_id);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }
    /* conn->seat may legitimately be NULL (a compositor with no input
     * seat at all is unusual but not protocol-invalid) — input setup
     * below handles that by simply not binding keyboard/pointer. */

    xdg_wm_base_add_listener(conn->wm_base, &g_wm_base_listener, conn);

    if (conn->seat != NULL) {
        fdk_wayland_bind_seat_listeners(conn);
    } else {
        FDK_WARN("compositor has no wl_seat — no keyboard/pointer input available");
    }

    /* Roundtrip 2 (1.2.4): the seat's name/capabilities events were
     * generated by the BIND in roundtrip 1 and are still sitting in
     * the queue — draining them HERE means everything that reacts to
     * seat arrival (fdk_wayland_clipboard_device_ready creating the
     * wl_data_device, the keyboard/pointer proxy binds) is live
     * before fdk_init returns. Found live by the 1.2.4 interop rig:
     * a clipboard set immediately after init answered UNSUPPORTED on
     * a compositor whose seat global arrives AFTER the
     * wl_data_device_manager global — the data device only material
     * ized when the first pump dispatched capabilities, so every
     * pre-pump clipboard call failed. The two-roundtrip startup is
     * the standard client pattern; roundtrip 1 collects globals,
     * roundtrip 2 collects the binds' replies. */
    if (conn->seat != NULL && wl_display_roundtrip(display) < 0) {
        FDK_ERROR("post-bind roundtrip failed");
        wl_registry_destroy(conn->registry);
        wl_event_queue_destroy(conn->release_queue);
        wl_display_disconnect(display);
        fdk_free(conn->app_id);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }

    fdk__startup_trace("wayland:connect complete");
    FDK_INFO("connected (compositor, shm, xdg_wm_base bound; seat=%s)",
             conn->seat ? "yes" : "no");

    *out_conn = conn;
    return FDK_OK;
}

void fdk_wayland_disconnect(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }

    if (conn->window_count > 0) {
        FDK_WARN("disconnecting with %zu window(s) still open — force-destroying",
                 conn->window_count);
        while (conn->window_count > 0) {
            fdk_wayland_window_destroy(conn->windows[conn->window_count - 1]);
        }
    }
    fdk_free(conn->windows);

    fdk_wayland_clipboard_teardown(conn);

    /* GPU lane (1.2.0 / D-1): windows were force-destroyed above, so
     * their per-window lane state is gone; the shared state dies
     * here (libraries stay pinned — wayland_egl.h's note). */
    if (conn->egl != NULL) {
        fdk_wayland_egl_shutdown(conn->egl);
        conn->egl = NULL;
    }

    fdk_wayland_teardown_seat(conn);

    fdk_wayland_cursor_teardown(conn);

    if (conn->decoration_manager) {
        zxdg_decoration_manager_v1_destroy(conn->decoration_manager);
    }
    if (conn->toplevel_icon_manager) {
        xdg_toplevel_icon_manager_v1_destroy(conn->toplevel_icon_manager);
        conn->toplevel_icon_manager = NULL;
    }
    fdk_wayland_ime_teardown(conn);
    if (conn->idle_inhibit_manager != NULL) {
        zwp_idle_inhibit_manager_v1_destroy(conn->idle_inhibit_manager);
        conn->idle_inhibit_manager = NULL;
    }
    if (conn->activation != NULL) {
        xdg_activation_v1_destroy(conn->activation);
        conn->activation = NULL;
    }
    /* Tablets (1.1.0, D-3): the tool records + the tablet seat die
     * with the connection; the manager proxy below. Idempotent. */
    fdk_wayland_tablet_shutdown(conn);
    if (conn->tablet_manager != NULL) {
        zwp_tablet_manager_v2_destroy(conn->tablet_manager);
        conn->tablet_manager = NULL;
    }
    fdk_free(conn->launch_token);
    conn->launch_token = NULL;
    if (conn->fractional_manager) {
        wp_fractional_scale_manager_v1_destroy(conn->fractional_manager);
    }
    if (conn->viewporter) {
        wp_viewporter_destroy(conn->viewporter);
    }
    fdk_wayland_destroy_outputs(conn);
    if (conn->wm_base) xdg_wm_base_destroy(conn->wm_base);
    if (conn->shm) wl_shm_destroy(conn->shm);
    if (conn->subcompositor) {
        wl_subcompositor_destroy(conn->subcompositor);
    }
    if (conn->foreign_toplevel_manager) {
        zwlr_foreign_toplevel_manager_v1_destroy(
            conn->foreign_toplevel_manager);
        conn->foreign_toplevel_manager = NULL;
    }
    conn->foreign_toplevel_name = 0;
    if (conn->compositor) wl_compositor_destroy(conn->compositor);
    if (conn->registry) wl_registry_destroy(conn->registry);

    fdk_free(conn->app_id);
    conn->app_id = NULL;

    /* After every wl_buffer is gone its event queue can die too
     * (libwayland requires the queue to outlive proxies assigned to
     * it — windows were force-destroyed above). */
    if (conn->release_queue != NULL) {
        wl_event_queue_destroy(conn->release_queue);
        conn->release_queue = NULL;
    }

    FDK_INFO("disconnecting");
    wl_display_disconnect(conn->display);
    fdk_free(conn);
}

int fdk_wayland_get_event_fd(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return -1;
    }
    return wl_display_get_fd(conn->display);
}

int fdk_platform_wayland_display_present(void) {
    const char *wd = getenv("WAYLAND_DISPLAY");
    return wd != NULL && wd[0] != '\0';
}
