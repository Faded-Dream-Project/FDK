#define _GNU_SOURCE /* memfd_create, MFD_CLOEXEC under -std=c17 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define WAYLAND_DEFAULT_WIDTH  640
#define WAYLAND_DEFAULT_HEIGHT 480
#define WAYLAND_DEFAULT_TITLE  "FDK Application"

/* Background pixel: pure white, matching the X11 backend's window
 * background (see x11_window.c's XCreateSimpleWindow call) so both
 * backends present the same Phase 2 appearance — a real, visible,
 * event-capable window with no renderer yet. Written as an
 * XRGB8888 pixel: memory layout on little-endian is [B,G,R,X], and
 * 0xFFFFFFFF sets every byte. */
#define WAYLAND_BG_PIXEL 0xFFFFFFFFu

/* Defined below — needed by xdg_surface_configure() above it. */
static fdk_result attach_background_buffer(fdk_platform_window *pwindow, fdk_size size);

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

    /* Content follows the (possibly new) size: this is where the
     * first buffer gets committed after the ack above, and where a
     * resize's replacement buffer lands. Both are what make the
     * window actually appear on screen — Wayland maps nothing until
     * a buffer is committed. */
    if (!pwindow->buffer_attached ||
        pwindow->buffer_size.width != pwindow->last_size.width ||
        pwindow->buffer_size.height != pwindow->last_size.height) {
        attach_background_buffer(pwindow, pwindow->last_size);
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

/* Compositor is done reading a background buffer — the only safe
 * moment to destroy it (per wl_buffer::release). If it is still the
 * current buffer (e.g. released because the surface was unmapped by
 * fdk_window_hide()), stop claiming it is attached so the next
 * configure re-attaches. */
static void background_buffer_release(void *data, struct wl_buffer *buffer) {
    fdk_platform_window *pwindow = data;
    if (pwindow->buffer == buffer) {
        pwindow->buffer = NULL;
        pwindow->buffer_attached = 0;
    }
    wl_buffer_destroy(buffer);
    FDK_DEBUG("background buffer released by compositor");
}

static const struct wl_buffer_listener g_background_buffer_listener = {
    .release = background_buffer_release,
};

/* Commits a fresh solid-color buffer at `size`, making the window
 * actually visible on screen — the Wayland counterpart of X11's
 * background pixel. Called on first configure (xdg-shell requires
 * acking the first configure before the first real commit), on
 * resizes, and from fdk_window_resize().
 *
 * Protocol-correctness notes (checked against the wl_shm spec):
 *  - The pool is destroyed immediately after creating the buffer:
 *    "the mmapped memory will be released when all buffers that have
 *    been created from this pool are gone", so the server-side
 *    mapping outlives the pool object.
 *  - The client-side mapping and fd are dropped right after fill +
 *    commit: the compositor holds its own fd/mapping received over
 *    the socket, independent of ours.
 *  - The OLD buffer (if any) is left alive here; it will receive
 *    wl_buffer::release once the new commit supersedes it, and the
 *    listener above destroys it then.
 *  - WL_SHM_FORMAT_XRGB8888 needs no format-event negotiation: the
 *    wl_shm spec guarantees every compositor supports it.
 * Returns FDK_OK, or an error code logged at WARN level (the caller
 * degrades to "window stays invisible", never crashes). */
static fdk_result attach_background_buffer(fdk_platform_window *pwindow, fdk_size size) {
    fdk_platform_connection *conn = pwindow->conn;

    if (size.width <= 0 || size.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    size_t stride = (size_t)size.width * 4u;
    size_t length = stride * (size_t)size.height;

    int fd = memfd_create("fdk-window-background", MFD_CLOEXEC);
    if (fd < 0) {
        FDK_WARN("memfd_create failed (%s)", strerror(errno));
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (ftruncate(fd, (off_t)length) < 0) {
        FDK_WARN("ftruncate failed (%s)", strerror(errno));
        close(fd);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    uint32_t *pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        FDK_WARN("mmap failed (%s)", strerror(errno));
        close(fd);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    uint32_t pixel = WAYLAND_BG_PIXEL;
    for (size_t i = 0; i < length / 4u; i++) {
        pixels[i] = pixel;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(conn->shm, fd, (int32_t)length);
    munmap(pixels, length);
    close(fd);
    if (pool == NULL) {
        FDK_WARN("wl_shm_create_pool failed");
        return FDK_ERR_OUT_OF_MEMORY;
    }

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, size.width, size.height, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    if (buffer == NULL) {
        FDK_WARN("wl_shm_pool_create_buffer failed");
        return FDK_ERR_OUT_OF_MEMORY;
    }

    wl_buffer_add_listener(buffer, &g_background_buffer_listener, pwindow);
    wl_surface_attach(pwindow->surface, buffer, 0, 0);
    /* Damage is mandatory: compositors schedule repaints from the
     * damage region, and a committed-but-undamaged surface is simply
     * never drawn — the buffer stays latched and invisible. Mark the
     * whole buffer damaged (INT32_MAX x INT32_MAX is the idiom for
     * "everything changed", robust to any buffer size/scale). */
    wl_surface_damage(pwindow->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(pwindow->surface);

    pwindow->buffer = buffer;
    pwindow->buffer_size = size;
    pwindow->buffer_attached = 1;

    FDK_DEBUG("background buffer %dx%d attached", size.width, size.height);
    return FDK_OK;
}

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
    pwindow->buffer = NULL;
    pwindow->buffer_size.width = 0;
    pwindow->buffer_size.height = 0;
    pwindow->buffer_attached = 0;

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
    /* If a background buffer is still attached here (it usually was
     * already released and destroyed by its release listener), drop
     * our reference before tearing down the surface. After
     * wl_surface_destroy the compositor discards pending state, and
     * the connection is about to go away anyway, so waiting for a
     * final wl_buffer::release would be pointless. */
    if (pwindow->buffer != NULL) {
        wl_buffer_destroy(pwindow->buffer);
        pwindow->buffer = NULL;
        pwindow->buffer_attached = 0;
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
     * that configure before attaching any actual content. The first
     * real buffer is therefore committed from xdg_surface_configure()
     * — see attach_background_buffer(). This commit just starts the
     * handshake; the window becomes visible a few events later. */
    wl_surface_commit(pwindow->surface);
}

void fdk_wayland_window_hide(fdk_platform_window *pwindow) {
    /* Unmap the surface by committing a NULL buffer — the documented
     * Wayland equivalent of X11's unmap. The previously committed
     * buffer gets wl_buffer::release'd by the compositor, which
     * destroys it via the release listener; the next
     * fdk_window_show() → configure cycle re-attaches a fresh one.
     * Title, size limits and all other window state survive. */
    if (pwindow->buffer_attached) {
        wl_surface_attach(pwindow->surface, NULL, 0, 0);
        wl_surface_commit(pwindow->surface);
        pwindow->buffer_attached = 0;
    }
}

void fdk_wayland_window_set_title(fdk_platform_window *pwindow, const char *title) {
    xdg_toplevel_set_title(pwindow->xdg_toplevel, title != NULL ? title : "");
}

void fdk_wayland_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height) {
    /* Wayland gives clients no direct "resize me" request — the
     * toplevel's size is ultimately compositor-driven (interactive
     * resize, maximization, or compositor policy). What a client CAN
     * do — and what this now does, instead of the old no-op — is
     * update its own idea of its size and commit a buffer at that
     * size: a floating toplevel's on-screen size follows the last
     * committed buffer. Whether the compositor honors the new size
     * is its call; if it disagrees, the resulting size arrives via
     * FDK_EVENT_WINDOW_CONFIGURE (matching fdk_window_resize()'s doc
     * comment in fdk_window.h about requests not being guarantees). */
    if (width <= 0 || height <= 0) {
        FDK_WARN("fdk_window_resize() ignored non-positive size %dx%d", width, height);
        return;
    }
    if (!pwindow->configured || !pwindow->buffer_attached) {
        /* No committed buffer yet — just remember the intended size;
        it becomes the size of the first buffer at first configure. */
        pwindow->last_size.width = width;
        pwindow->last_size.height = height;
        pwindow->pending_size = pwindow->last_size;
        return;
    }
    if (pwindow->buffer_size.width == width && pwindow->buffer_size.height == height) {
        return; /* already at that size — avoid a pointless new buffer */
    }
    fdk_size new_size = { .width = width, .height = height };
    if (!fdk_ok(attach_background_buffer(pwindow, new_size))) {
        return; /* attach_background_buffer already logged the reason */
    }
    pwindow->last_size = new_size;
}

void fdk_wayland_window_set_size_limits(fdk_platform_window *pwindow,
                                         fdk_size min_size, fdk_size max_size) {
    xdg_toplevel_set_min_size(pwindow->xdg_toplevel, min_size.width, min_size.height);
    xdg_toplevel_set_max_size(pwindow->xdg_toplevel, max_size.width, max_size.height);
}
