#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#define WAYLAND_DEFAULT_WIDTH  640
#define WAYLAND_DEFAULT_HEIGHT 480
#define WAYLAND_DEFAULT_TITLE  "FDK Application"

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                   uint32_t serial) {
    fdk_platform_window *pwindow = data;
    /* Must ack every configure, even the first one before any content
     * has been committed — this is what tells the compositor "I've
     * seen this configure and applied whatever it implies." Skipping
     * this ack is a common Wayland-client bug that stalls the surface. */
    xdg_surface_ack_configure(xdg_surface, serial);

    int was_configured = pwindow->configured;
    pwindow->configured = 1;

    if (pwindow->pending_size.width > 0 && pwindow->pending_size.height > 0) {
        pwindow->last_size = pwindow->pending_size;
    }

    /* The very first configure is required before the first
     * wl_surface_commit() (see fdk_wayland_window_show()) — don't
     * emit a redundant FDK_EVENT_WINDOW_CONFIGURE for it if no real
     * size was proposed; the application already knows its requested
     * creation size. */
    if (was_configured) {
        fdk_event_data event = { .type = FDK_EVENT_WINDOW_CONFIGURE };
        event.configure.size = pwindow->last_size;
        pwindow->conn->dispatch(pwindow, &event, pwindow->conn->dispatch_user_data);
    }
}

static const struct xdg_surface_listener g_xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height, struct wl_array *states) {
    (void)toplevel;
    (void)states;
    fdk_platform_window *pwindow = data;

    /* width/height == 0 means "compositor has no opinion, keep your
     * current/requested size" per the xdg-shell spec — not an error,
     * and not "resize to zero". */
    if (width > 0 && height > 0) {
        pwindow->pending_size.width = width;
        pwindow->pending_size.height = height;
    } else {
        pwindow->pending_size = pwindow->last_size;
    }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    fdk_platform_window *pwindow = data;
    fdk_event_data event = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
    pwindow->conn->dispatch(pwindow, &event, pwindow->conn->dispatch_user_data);
}

static const struct xdg_toplevel_listener g_xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

fdk_result fdk_wayland_window_create(fdk_platform_connection *conn,
                                      const fdk_window_options *options,
                                      fdk_platform_window **out_pwindow) {
    fdk_i32 width = WAYLAND_DEFAULT_WIDTH;
    fdk_i32 height = WAYLAND_DEFAULT_HEIGHT;
    const char *title = WAYLAND_DEFAULT_TITLE;

    if (options != NULL) {
        if (options->width > 0)  width = options->width;
        if (options->height > 0) height = options->height;
        if (options->title != NULL) title = options->title;
    }

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    pwindow->conn = conn;
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->pending_size = pwindow->last_size;
    pwindow->configured = 0;

    pwindow->surface = wl_compositor_create_surface(conn->compositor);
    if (pwindow->surface == NULL) {
        FDK_ERROR("wl_compositor_create_surface failed");
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }

    pwindow->xdg_surface = xdg_wm_base_get_xdg_surface(conn->wm_base, pwindow->surface);
    if (pwindow->xdg_surface == NULL) {
        FDK_ERROR("xdg_wm_base_get_xdg_surface failed");
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }
    xdg_surface_add_listener(pwindow->xdg_surface, &g_xdg_surface_listener, pwindow);

    pwindow->xdg_toplevel = xdg_surface_get_toplevel(pwindow->xdg_surface);
    if (pwindow->xdg_toplevel == NULL) {
        FDK_ERROR("xdg_surface_get_toplevel failed");
        xdg_surface_destroy(pwindow->xdg_surface);
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }
    xdg_toplevel_add_listener(pwindow->xdg_toplevel, &g_xdg_toplevel_listener, pwindow);
    xdg_toplevel_set_title(pwindow->xdg_toplevel, title);

    fdk_result r = fdk_wayland_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        xdg_toplevel_destroy(pwindow->xdg_toplevel);
        xdg_surface_destroy(pwindow->xdg_surface);
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("window created (%dx%d, \"%s\")", width, height, title);

    *out_pwindow = pwindow;
    return FDK_OK;
}

void fdk_wayland_window_destroy(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    fdk_wayland_unregister_window(pwindow->conn, pwindow);
    xdg_toplevel_destroy(pwindow->xdg_toplevel);
    xdg_surface_destroy(pwindow->xdg_surface);
    wl_surface_destroy(pwindow->surface);
    fdk_free(pwindow);
}

void fdk_wayland_window_show(fdk_platform_window *pwindow) {
    /* xdg-shell requires an initial "commit with no buffer" to
     * trigger the first configure, then the client must wait for
     * that configure (handled asynchronously by
     * xdg_surface_configure() above) before attaching any actual
     * content and committing again. Phase 2 has no renderer yet (see
     * docs/roadmap.md, Phase 3), so there's no buffer to attach —
     * this commit is enough to make the surface exist and get the
     * xdg-shell handshake moving; the window will show as soon as
     * Phase 3's renderer attaches a buffer, following the same
     * pattern documented here. */
    wl_surface_commit(pwindow->surface);
}

void fdk_wayland_window_hide(fdk_platform_window *pwindow) {
    /* No true Wayland equivalent of X11's unmap that preserves state
     * — the idiomatic approach is attaching a NULL buffer, which
     * again needs the renderer (Phase 3). Documented gap rather than
     * a fake no-op success. */
    (void)pwindow;
    FDK_WARN("fdk_window_hide() is a no-op on Wayland until Phase 3 "
             "(attaching a NULL buffer requires the renderer)");
}

void fdk_wayland_window_set_title(fdk_platform_window *pwindow, const char *title) {
    xdg_toplevel_set_title(pwindow->xdg_toplevel, title != NULL ? title : "");
}

void fdk_wayland_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height) {
    /* Wayland gives clients no direct "resize me" request — resizing
     * a toplevel is compositor-driven (interactive resize, or a
     * compositor policy). A client can only update its own idea of
     * its size and commit, which needs the renderer. Documented
     * limitation, matching fdk_window_resize()'s own doc comment in
     * fdk_window.h about requests not being guarantees. */
    (void)pwindow;
    (void)width;
    (void)height;
    FDK_WARN("fdk_window_resize() is a no-op on Wayland — toplevel size "
             "is compositor-driven, not client-requested (see fdk_window.h)");
}

void fdk_wayland_window_set_size_limits(fdk_platform_window *pwindow,
                                         fdk_size min_size, fdk_size max_size) {
    xdg_toplevel_set_min_size(pwindow->xdg_toplevel, min_size.width, min_size.height);
    xdg_toplevel_set_max_size(pwindow->xdg_toplevel, max_size.width, max_size.height);
}
