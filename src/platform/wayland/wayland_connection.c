#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/viewporter-client-protocol.h"
#include "generated/fractional-scale-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void registry_global(void *data, struct wl_registry *registry,
                             uint32_t name, const char *interface, uint32_t version) {
    fdk_platform_connection *conn = data;
    (void)version;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        conn->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        conn->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        conn->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        conn->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        /* OPTIONAL global (Phase 8): its absence is not an error and
         * costs nothing client-side — a compositor without this
         * protocol never draws chrome itself (client-side is the
         * xdg-shell default), so FDK's own band still comes up; only
         * the platform-chrome direction (set_decorated(false))
         * reports FDK_ERR_UNSUPPORTED then. */
        conn->decoration_manager =
            wl_registry_bind(registry, name,
                             &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        /* OPTIONAL global (Phase 9): wl_data_device_manager is the
         * clipboard (and drag-and-drop) factory. Absent -> the
         * clipboard ops honestly report FDK_ERR_UNSUPPORTED instead
         * of faking a local-only clipboard. Version 3 is enough for
         * selections (drag-and-drop's data_source actions arrive with
         * v3 and are ignored — FDK does no DnD). */
        conn->data_device_manager =
            wl_registry_bind(registry, name,
                             &wl_data_device_manager_interface, 3);
        fdk_wayland_clipboard_device_ready(conn);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        /* HiDPI (Phase 3 completion): bind every output at the
         * highest version both sides support, capped at 3 — v2 adds
         * the scale + done events (the minimum FDK needs), v3 only
         * adds name/description strings FDK has no use for. */
        uint32_t v = version < 2 ? version : (version > 3 ? 3 : version);
        struct wl_output *output =
            wl_registry_bind(registry, name, &wl_output_interface, v);
        if (output != NULL) {
            fdk_wayland_track_output(conn, output);
        }
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        /* OPTIONAL (HiDPI): the source-rectangle mechanism fractional
         * scaling rides on. Absent -> integer buffer scale only. */
        conn->viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface,
                      wp_fractional_scale_manager_v1_interface.name) == 0) {
        /* OPTIONAL (HiDPI): per-window preferred scale in 120ths.
         * Absent -> integer buffer scale only. */
        conn->fractional_manager =
            wl_registry_bind(registry, name,
                             &wp_fractional_scale_manager_v1_interface, 1);
    }
    /* Other globals (wl_data_device_manager, etc.) are
     * intentionally not bound — out of current scope, see
     * docs/roadmap.md. Ignoring an unrecognized global is the correct
     * behavior per the registry protocol, not a gap. */
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                    uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
    /* Global removal (e.g. a seat unplugged) isn't handled in Phase
     * 2 — logged if it ever matters in practice, not silently eaten,
     * but not acted on. Acting on it correctly means detaching any
     * windows/input state bound to that global, which is a real
     * feature to design, not a one-line fix; deferred honestly rather
     * than half-implemented. */
    FDK_WARN("registry global %u removed (not handled in Phase 2)", name);
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

    fdk_wayland_teardown_seat(conn);

    fdk_wayland_cursor_teardown(conn);

    if (conn->decoration_manager) {
        zxdg_decoration_manager_v1_destroy(conn->decoration_manager);
    }
    if (conn->fractional_manager) {
        wp_fractional_scale_manager_v1_destroy(conn->fractional_manager);
    }
    if (conn->viewporter) {
        wp_viewporter_destroy(conn->viewporter);
    }
    fdk_wayland_destroy_outputs(conn);
    if (conn->wm_base) xdg_wm_base_destroy(conn->wm_base);
    if (conn->shm) wl_shm_destroy(conn->shm);
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
