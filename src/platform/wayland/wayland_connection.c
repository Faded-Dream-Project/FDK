#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdlib.h>
#include <string.h>

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
    }
    /* Other globals (wl_output, wl_data_device_manager, etc.) are
     * intentionally not bound — out of Phase 2 scope, see
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
                                void *dispatch_user_data,
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
    conn->windows = NULL;
    conn->window_count = 0;
    conn->window_capacity = 0;
    conn->last_dispatch_errno = 0;

    conn->registry = wl_display_get_registry(display);
    if (conn->registry == NULL) {
        FDK_ERROR("wl_display_get_registry failed");
        wl_display_disconnect(display);
        fdk_free(conn);
        return FDK_ERR_PLATFORM_INIT;
    }
    wl_registry_add_listener(conn->registry, &g_registry_listener, conn);

    /* Roundtrip: blocks until the compositor has sent every initial
     * wl_registry::global event and we've processed them all via the
     * listener above. This is the standard Wayland client startup
     * pattern — without it, conn->compositor etc. would still be
     * NULL immediately after this function returns. */
    if (wl_display_roundtrip(display) < 0) {
        FDK_ERROR("initial registry roundtrip failed");
        wl_display_disconnect(display);
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
        wl_display_disconnect(display);
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

    fdk_wayland_teardown_seat(conn);

    if (conn->wm_base) xdg_wm_base_destroy(conn->wm_base);
    if (conn->shm) wl_shm_destroy(conn->shm);
    if (conn->compositor) wl_compositor_destroy(conn->compositor);
    if (conn->registry) wl_registry_destroy(conn->registry);

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
