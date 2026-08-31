#define _GNU_SOURCE /* memfd_create, MFD_CLOEXEC under -std=c17 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/viewporter-client-protocol.h"
#include "generated/fractional-scale-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "theme/theme_internal.h"

#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WAYLAND_DEFAULT_WIDTH  640
#define WAYLAND_DEFAULT_HEIGHT 480
#define WAYLAND_DEFAULT_TITLE  "FDK Application"

/* Background pixel (1.2.1): the theme's window-background token —
 * the same fill the root default background paints on the first
 * rendered frame (fdk_window_get_root), so the pre-first-frame
 * window and the first painted frame agree instead of flashing
 * white→dark. Matches the X11 backend's themed creation-time
 * background pixel (x11_window.c). Written as an XRGB8888 pixel:
 * little-endian memory layout is [B,G,R,X], i.e. R<<16 | G<<8 | B. */
static uint32_t wayland_theme_window_pixel(void) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    uint32_t r = (uint32_t)(c.r * 255.0f + 0.5f);
    uint32_t g = (uint32_t)(c.g * 255.0f + 0.5f);
    uint32_t b = (uint32_t)(c.b * 255.0f + 0.5f);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
}

/* Defined below — needed by xdg_surface_configure() above it. */
static fdk_result attach_background_buffer(fdk_platform_window *pwindow, fdk_size size);

/* HiDPI helpers defined below (the scale machinery block). */
static fdk_i32 physical_dim(fdk_i32 logical, int scale_x120);
fdk_result fdk_wayland_window_recompute_scale(fdk_platform_window *pwindow,
                                              struct wl_output *output,
                                              int entered);

/* Creates a WL_SHM_FORMAT_XRGB8888 wl_buffer of `size` backed by a
 * fresh memfd. Shared by the solid-background path and the software
 * render path (fdk_surface machinery); the wl_shm spec guarantees
 * every compositor supports this format, and destroying the pool
 * immediately after buffer creation is legal ("the mmapped memory
 * will be released when all buffers that have been created from this
 * pool are gone") — the server-side mapping outlives the pool object
 * and our fd. Returns the buffer plus the (still-mapped) pixels so
 * callers can either fill and drop the mapping (background) or hand
 * it to the application to draw into (render path). */
static fdk_result create_shm_buffer(fdk_platform_connection *conn,
                                     fdk_size size,
                                     struct wl_buffer **out_buffer,
                                     uint32_t **out_pixels,
                                     size_t *out_length) {
    if (size.width <= 0 || size.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    size_t stride = (size_t)size.width * 4u;
    size_t length = stride * (size_t)size.height;

    int fd = memfd_create("fdk-window-buffer", MFD_CLOEXEC);
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

    struct wl_shm_pool *pool = wl_shm_create_pool(conn->shm, fd, (int32_t)length);
    close(fd);
    if (pool == NULL) {
        munmap(pixels, length);
        FDK_WARN("wl_shm_create_pool failed");
        return FDK_ERR_OUT_OF_MEMORY;
    }

    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, size.width, size.height, (int32_t)stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    if (buffer == NULL) {
        munmap(pixels, length);
        FDK_WARN("wl_shm_pool_create_buffer failed");
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* 1.1.6: wl_buffer events ride the connection's DEDICATED queue,
     * so the slot-exhaustion wait (get_framebuffer below) can block
     * on wl_buffer::release alone — dispatching that queue never runs
     * unrelated listeners re-entrantly, which is what makes the
     * bounded wait safe to call from inside a listener-driven paint
     * (the dispatch-tail synchronous repaint). */
    if (conn->release_queue != NULL) {
        wl_proxy_set_queue((struct wl_proxy *)buffer, conn->release_queue);
    }

    *out_buffer = buffer;
    *out_pixels = pixels;
    *out_length = length;
    return FDK_OK;
}

/* Defined below (Phase 8 section), forward-declared for the window
 * create path, which must create the xdg-decoration object before
 * the surface's first buffer per protocol. */
static void toplevel_decoration_configure(void *data,
                                          struct zxdg_toplevel_decoration_v1 *deco,
                                          uint32_t mode);
static const struct zxdg_toplevel_decoration_v1_listener
    g_toplevel_decoration_listener;

/* Defined below (render-buffer path): commits an acquired render
 * buffer. The deferred-first-frame block in xdg_surface_configure()
 * needs it before its definition point. */
static fdk_result commit_render_pending(fdk_platform_window *pwindow,
                                        const fdk_platform_damage *damage);

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

    int size_changed = 0;
    if (pwindow->pending_size.width > 0 && pwindow->pending_size.height > 0) {
        if (pwindow->last_size.width != pwindow->pending_size.width ||
            pwindow->last_size.height != pwindow->pending_size.height) {
            /* A real resize: a fractional viewport's source rectangle
             * is size-derived, so it must re-apply before the next
             * commit (integer scales derive nothing and re-apply as a
             * cheap no-op of the same requests). */
            pwindow->scale_applied = 0;
            size_changed = 1;
        }
        pwindow->last_size = pwindow->pending_size;
    }

    /* Content follows the (possibly new) size: this is where the
     * first buffer gets committed after the ack above, and where a
     * resize's replacement buffer lands. Both are what make the
     * window actually appear on screen — Wayland maps nothing until
     * a buffer is committed.
     *
     * Applications rendering via fdk_surface are skipped: their
     * committed render buffers (or the pending one about to be
     * presented) own visibility, and re-committing a white background
     * here would flash over their content on every resize. */
    if ((!pwindow->buffer_attached ||
         pwindow->buffer_size.width != pwindow->last_size.width ||
         pwindow->buffer_size.height != pwindow->last_size.height) &&
        !pwindow->rendered_ever && pwindow->render_pending == NULL) {
        attach_background_buffer(
            pwindow, (fdk_size){
                         .width = physical_dim(
                             pwindow->last_size.width < 1
                                 ? 1
                                 : pwindow->last_size.width,
                             pwindow->scale_x120),
                         .height = physical_dim(
                             pwindow->last_size.height < 1
                                 ? 1
                                 : pwindow->last_size.height,
                             pwindow->scale_x120),
                     });
    }

    /* The deferred first frame (found live, 1.1.0): an application
     * that paints BEFORE the first configure — exactly what every
     * FDK example does (show, paint, then enter the pump loop) — had
     * its first present deferred above (xdg-shell forbids committing
     * content before ack_configure) with the damage already consumed
     * by the surface layer. With the tree clean and the surface's
     * damage empty, every later present was a no-op: the pending
     * buffer was NEVER committed and the window never mapped — the
     * app sat invisible forever. The commit the deferred present was
     * waiting for happens HERE, now that ack_configure has been sent:
     *
     *   - pending buffer still at the configured size -> commit it
     *     as-is (full damage; the content the app painted is valid)
     *   - configure proposed a different size -> the stale pending
     *     is dropped at the next acquisition; dispatch EXPOSE so the
     *     tree re-invalidates and the app's next paint redraws and
     *     presents at the real size (same re-drive the X11 backend's
     *     ExposureMask provides on first map) */
    if (pwindow->render_pending != NULL && !pwindow->rendered_ever) {
        fdk_size want = {
            .width = physical_dim(pwindow->last_size.width < 1
                                      ? 1
                                      : pwindow->last_size.width,
                                  pwindow->scale_x120),
            .height = physical_dim(pwindow->last_size.height < 1
                                       ? 1
                                       : pwindow->last_size.height,
                                   pwindow->scale_x120),
        };
        fdk_size have = { .width = 0, .height = 0 };
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer ==
                pwindow->render_pending) {
                have = pwindow->render_slots[i].size;
                break;
            }
        }
        if (have.width == want.width && have.height == want.height) {
            fdk_platform_damage full;
            memset(&full, 0, sizeof(full));
            full.full = 1;
            (void)commit_render_pending(pwindow, &full);
            FDK_DEBUG("deferred first frame committed at configure");
        } else {
            fdk_event_data event;
            memset(&event, 0, sizeof(event));
            event.type = FDK_EVENT_WINDOW_EXPOSE;
            pwindow->conn->dispatch(pwindow, &event,
                                    pwindow->conn->dispatch_user_data);
            FDK_DEBUG("configure resized past the deferred frame; "
                      "EXPOSE re-drives the first paint");
        }
    }

    /* The very first configure is required before the first
     * wl_surface_commit() (see fdk_wayland_window_show()) — but only
     * a first configure that KEPT our size may stay silent (the app
     * already knows its requested creation size). A first configure
     * that CHANGED the size must emit FDK_EVENT_WINDOW_CONFIGURE
     * like any later one: resize-at-map compositors (kiosk-shell
     * fullscreen, tiling WMs) propose their size right there, and
     * the window layer must learn it or it keeps laying out at the
     * creation size inside a full-compositor buffer — found live by
     * the 1.1.5 manager-less rig: 320x240 of UI islanded in the
     * top-left of a 1024x640 framebuffer, rest zero-black. */
    if (was_configured || size_changed) {
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

    /* Phase 8: the states array is the compositor's authoritative
     * window state. MAXIMIZED is tracked directly; ACTIVATED is what
     * clears the (request-optimistic) minimized flag — the protocol
     * has no minimized state and no unminimize request, but
     * compositors report activated when a window is brought back.
     * FULLSCREEN (1.1.5) is tracked as the resize gate only — kiosk
     * shells and fullscreen windows get their geometry from the
     * compositor, and a client buffer at any other size is a
     * protocol error there. */
    int maximized = 0, activated = 0, fullscreen = 0;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            maximized = 1;
        }
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) {
            fullscreen = 1;
        }
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
            activated = 1;
        }
    }
    pwindow->fullscreen = fullscreen;
    int minimized = pwindow->minimized;
    if (activated && minimized) {
        minimized = 0;
    }
    fdk_wayland_window_update_state(pwindow, maximized, minimized);
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    fdk_platform_window *pwindow = data;
    fdk_event_data event = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
    pwindow->conn->dispatch(pwindow, &event, pwindow->conn->dispatch_user_data);
}

/* xdg_popup events (Phase 9): configure carries the final placement
 * (the compositor may move it per the constraint adjustments —
 * acknowledged through xdg_surface.ack_configure as usual);
 * popup_done is the compositor's dismissal (click elsewhere, focus
 * change) and becomes the window's close request. */
static void xdg_popup_configure(void *data, struct xdg_popup *popup,
                                int32_t x, int32_t y, int32_t width,
                                int32_t height) {
    (void)popup;
    fdk_platform_window *pwindow = data;
    pwindow->pending_size.width = width;
    pwindow->pending_size.height = height;
    FDK_DEBUG("xdg_popup configure at (%d, %d) %dx%d", x, y, width,
              height);
}

static void xdg_popup_done(void *data, struct xdg_popup *popup) {
    (void)popup;
    fdk_platform_window *pwindow = data;
    fdk_event_data event = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
    pwindow->conn->dispatch(pwindow, &event,
                            pwindow->conn->dispatch_user_data);
}

static const struct xdg_popup_listener g_xdg_popup_listener = {
    .configure = xdg_popup_configure,
    .popup_done = xdg_popup_done,
    .repositioned = NULL,
};

static const struct xdg_toplevel_listener g_xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

/* wl_surface enter/leave (core protocol, HiDPI — Phase 3
 * completion): the surface is being shown on (or removed from) this
 * output. The window's output-derived preferred scale is the MAXIMUM
 * scale of the outputs it currently occupies — Wayland's rule for
 * never rendering a window at less detail than the sharpest screen
 * it is visible on. (These are wl_surface events in the CORE
 * protocol, not xdg_toplevel ones — the classic confusion.) */
static void wl_surface_enter_output(void *data, struct wl_surface *surface,
                                    struct wl_output *output) {
    (void)surface;
    fdk_platform_window *pwindow = data;
    (void)fdk_wayland_window_recompute_scale(pwindow, output, 1);
}

static void wl_surface_leave_output(void *data, struct wl_surface *surface,
                                    struct wl_output *output) {
    (void)surface;
    fdk_platform_window *pwindow = data;
    (void)fdk_wayland_window_recompute_scale(pwindow, output, 0);
}

static const struct wl_surface_listener g_wl_surface_listener = {
    .enter = wl_surface_enter_output,
    .leave = wl_surface_leave_output,
};

/* wp_fractional_scale_v1::preferred_scale (HiDPI): the compositor's
 * authoritative per-window preference, in 120ths of a unit — this
 * OVERRIDES the output-derived integer maximum (it arrives from the
 * compositor that also owns enter/leave, with fresher knowledge of
 * e.g. a 150% desktop setting). */
static void fractional_preferred_scale(void *data,
                                       struct wp_fractional_scale_v1 *frac,
                                       uint32_t scale) {
    (void)frac;
    fdk_platform_window *pwindow = data;
    if (scale < 120) {
        scale = 120; /* protocol floor: 1x */
    }
    if ((int)scale != pwindow->scale_x120) {
        pwindow->scale_x120 = (int)scale;
        pwindow->scale_applied = 0; /* re-apply before the next commit */
        FDK_DEBUG("fractional preferred scale: %d/120", scale);
    }
}

static const struct wp_fractional_scale_v1_listener
    g_fractional_scale_listener = {
    .preferred_scale = fractional_preferred_scale,
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

    struct wl_buffer *buffer = NULL;
    uint32_t *pixels = NULL;
    size_t length = 0;
    fdk_result r = create_shm_buffer(conn, size, &buffer, &pixels, &length);
    if (!fdk_ok(r)) {
        return r;
    }

    uint32_t pixel = wayland_theme_window_pixel();
    for (size_t i = 0; i < length / 4u; i++) {
        pixels[i] = pixel;
    }
    munmap(pixels, length);

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

/* ---- Software rendering (fdk_surface machinery) ----
 *
 * Lifecycle (buffer-recycling design — see wayland_platform.h's
 * render_slots comment for the full rationale):
 *
 *   acquire  -> reuse the pending buffer if the size still matches;
 *               otherwise pick a RELEASED slot at the right size (or
 *               an empty slot, creating a fresh buffer), pre-fill it
 *               with a copy of the currently visible frame so pixels
 *               outside the app's next damage region keep matching
 *               the screen, and mark it `render_pending`.
 *   present  -> attach + damage + commit the pending buffer; it
 *               becomes the live buffer; request a frame callback.
 *   release  -> the compositor is done with a buffer: its slot is
 *               marked `released` (kept alive for reuse, NOT
 *               destroyed).
 *   destroy  -> all slots are destroyed unconditionally.
 */

static void render_buffer_release(void *data, struct wl_buffer *buffer) {
    fdk_platform_window *pwindow = data;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == buffer) {
            /* Keep buffer + mapping alive for recycling; only flip
             * the state. If it was the live buffer, the surface is
             * now effectively un-backed (e.g. superseded or the
             * surface was unmapped by fdk_window_hide()). */
            pwindow->render_slots[i].released = 1;
            if (pwindow->render_pending == buffer) {
                /* Defensive: a pending (never-attached) buffer should
                 * not be released; if a compositor does anyway, keep
                 * bookkeeping consistent rather than dangling. */
                pwindow->render_pending = NULL;
            }
            if (pwindow->buffer == buffer) {
                pwindow->buffer = NULL;
                pwindow->buffer_attached = 0;
            }
            FDK_DEBUG("render buffer released by compositor (slot kept "
                      "for reuse)");
            return;
        }
    }
    /* Not one of ours — nothing to do (should not happen; every
     * listener registration passes the owning pwindow). */
}

static const struct wl_buffer_listener g_render_buffer_listener = {
    .release = render_buffer_release,
};

/* Frame callback: the compositor finished presenting the last
 * committed frame — the green light for the next one. Destroys the
 * one-shot callback object (its only event has arrived). */
static void frame_callback_done(void *data, struct wl_callback *callback,
                                 uint32_t callback_data) {
    (void)callback_data;
    fdk_platform_window *pwindow = data;
    pwindow->frame_ack = 1;
    pwindow->frame_cb = NULL; /* about to die; don't double-destroy */
    wl_callback_destroy(callback);
    FDK_DEBUG("frame acknowledged by compositor");
}

static const struct wl_callback_listener g_frame_callback_listener = {
    .done = frame_callback_done,
};

/* Monotonic milliseconds — only used for the pacing starvation
 * guard, so absolute epoch correctness does not matter. */
static fdk_i64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

/* Copies the currently visible frame (the live buffer's mapping)
 * into `slot`, when one exists at the same size. This is the
 * correctness backbone of damage tracking on Wayland: the app's next
 * frame then differs from the screen ONLY in the region it draws,
 * which is exactly what the damage hints claim. A live buffer that
 * is a background buffer has no client mapping (background buffers
 * are filled and munmapped at attach) — but that case only occurs
 * before the first render frame, which is always fully damaged
 * anyway. */
static void prefetch_visible_frame(fdk_platform_window *pwindow,
                                    int slot) {
    if (pwindow->buffer == NULL) {
        return; /* nothing visible yet — first frame is full-damage */
    }
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == pwindow->buffer &&
            pwindow->render_slots[i].size.width ==
                pwindow->render_slots[slot].size.width &&
            pwindow->render_slots[i].size.height ==
                pwindow->render_slots[slot].size.height &&
            pwindow->render_slots[i].length ==
                pwindow->render_slots[slot].length) {
            memcpy(pwindow->render_slots[slot].pixels,
                   pwindow->render_slots[i].pixels,
                   pwindow->render_slots[slot].length);
            return;
        }
    }
    /* Live buffer has no reusable mapping (background buffer, or a
     * different length): leave the slot zeroed — the surface layer
     * resets damage to full on any size change, and the app's first
     * frame at a given size is expected to cover everything. */
}

/* ---- HiDPI scale machinery (Phase 3 completion) ------------------------ */

/* Physical (buffer) dimension for a logical one at the window's
 * current scale: ceil(logical * scale_x120 / 120). Exact for integer
 * factors; rounds UP fractionally so the viewport's source rectangle
 * (which may be fractional) is always fully backed by buffer pixels. */
static fdk_i32 physical_dim(fdk_i32 logical, int scale_x120) {
    long long v = (long long)logical * (long long)scale_x120;
    fdk_i32 p = (fdk_i32)((v + 119) / 120);
    return p < 1 ? 1 : p;
}

/* Updates the entered-outputs set and re-derives the preferred scale
 * from it — used when the fractional-scale protocol is unavailable
 * (with it, the compositor's preferred_scale event is authoritative
 * and arrives by itself; this path would only fight it). */
fdk_result fdk_wayland_window_recompute_scale(fdk_platform_window *pwindow,
                                              struct wl_output *output,
                                              int entered) {
    if (pwindow == NULL || output == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    if (entered) {
        int known = 0;
        for (size_t i = 0; i < pwindow->entered_count; i++) {
            if (pwindow->entered_outputs[i] == output) {
                known = 1;
                break;
            }
        }
        if (!known) {
            if (pwindow->entered_count == pwindow->entered_capacity) {
                size_t cap =
                    pwindow->entered_capacity == 0 ? 4
                                                   : pwindow->entered_capacity * 2;
                if (cap > SIZE_MAX / sizeof(struct wl_output *)) {
                    return FDK_ERR_OUT_OF_MEMORY;
                }
                struct wl_output **grown =
                    fdk_realloc(pwindow->entered_outputs,
                                cap * sizeof(struct wl_output *));
                if (grown == NULL) {
                    return FDK_ERR_OUT_OF_MEMORY;
                }
                pwindow->entered_outputs = grown;
                pwindow->entered_capacity = cap;
            }
            pwindow->entered_outputs[pwindow->entered_count++] = output;
        }
    } else {
        for (size_t i = 0; i < pwindow->entered_count; i++) {
            if (pwindow->entered_outputs[i] == output) {
                pwindow->entered_outputs[i] =
                    pwindow->entered_outputs[pwindow->entered_count - 1];
                pwindow->entered_count--;
                break;
            }
        }
    }

    if (pwindow->fractional != NULL) {
        return FDK_OK; /* preferred_scale events own the value */
    }

    /* Max scale of the outputs we occupy (scale 0 = output gone:
     * skipped), floor 1 — the Wayland "never under-detail" rule. */
    int best = 1;
    fdk_platform_connection *conn = pwindow->conn;
    for (size_t i = 0; i < pwindow->entered_count; i++) {
        for (size_t j = 0; j < conn->output_count; j++) {
            if (conn->outputs[j].output == pwindow->entered_outputs[i] &&
                conn->outputs[j].scale > best) {
                best = conn->outputs[j].scale;
            }
        }
    }
    int want = best * 120;
    if (want != pwindow->scale_x120) {
        pwindow->scale_x120 = want;
        pwindow->scale_applied = 0; /* re-apply before the next commit */
        FDK_DEBUG("output-derived scale -> %d/120 (%d output(s))", want,
                  (int)pwindow->entered_count);
    }
    return FDK_OK;
}

/* Pushes the current scale to the protocol, before a commit. Integer
 * factors: wl_surface.set_buffer_scale(k) (and a stale viewport is
 * removed — viewport state supersedes buffer scale, so leaving one
 * behind would silently override k). Fractional factors (needs the
 * viewporter): buffer scale stays 1 and the viewport maps the exact
 * fractional source rectangle onto the logical surface size. */
static void apply_window_scale(fdk_platform_window *pwindow) {
    if (pwindow->scale_applied || pwindow->surface == NULL) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;

    int integer = (pwindow->scale_x120 % 120) == 0;
    if (integer || conn->viewporter == NULL) {
        int k = pwindow->scale_x120 / 120;
        if (k < 1) {
            k = 1;
        }
        if (pwindow->viewport != NULL) {
            /* A stale fractional viewport from a previous scale —
             * viewport state supersedes buffer scale, so it must go
             * before set_buffer_scale means anything. The fractional
             * LISTENER object stays: preferred_scale events keep
             * arriving (a later fractional switch re-creates the
             * viewport lazily below). */
            wp_viewport_destroy(pwindow->viewport);
            pwindow->viewport = NULL;
        }
        wl_surface_set_buffer_scale(pwindow->surface, k);
        pwindow->buffer_scale = k;
    } else {
        /* Fractional factor: the viewport is created HERE, never at
         * window-create — an unconfigured viewport (empty source/
         * destination) means "never display this surface" per the
         * viewporter protocol, and the first commit carrying one
         * kept sway from EVER mapping the window (found live: FDK
         * windows were absent from sway's tree while a control
         * client mapped fine; the suite's self-consistent checks —
         * own-framebuffer pixels + protocol counts — never saw it).
         * Creating it in the same batch as its configuration means
         * a viewport is never seen empty. */
        if (pwindow->viewport == NULL) {
            pwindow->viewport =
                wp_viewporter_get_viewport(conn->viewporter,
                                           pwindow->surface);
            if (pwindow->viewport == NULL) {
                FDK_WARN("wp_viewporter_get_viewport failed; falling "
                         "back to integer scale");
                int k = pwindow->scale_x120 / 120;
                if (k < 1) {
                    k = 1;
                }
                wl_surface_set_buffer_scale(pwindow->surface, k);
                pwindow->buffer_scale = k;
                pwindow->scale_applied = 1;
                return;
            }
        }
        wl_surface_set_buffer_scale(pwindow->surface, 1);
        pwindow->buffer_scale = 1;
        fdk_i32 lw = pwindow->last_size.width;
        fdk_i32 lh = pwindow->last_size.height;
        if (lw <= 0) lw = 1;
        if (lh <= 0) lh = 1;
        /* Source rectangle: logical size scaled by exactly
         * scale_x120/120 (wl_fixed precision is 1/256 — far finer
         * than the 1/120 quantum, so this is lossless). */
        wl_fixed_t sw = wl_fixed_from_double(
            (double)lw * (double)pwindow->scale_x120 / 120.0);
        wl_fixed_t sh = wl_fixed_from_double(
            (double)lh * (double)pwindow->scale_x120 / 120.0);
        wp_viewport_set_source(pwindow->viewport, wl_fixed_from_int(0),
                               wl_fixed_from_int(0), sw, sh);
        wp_viewport_set_destination(pwindow->viewport, lw, lh);
    }
    pwindow->scale_applied = 1;
}

fdk_result fdk_wayland_window_get_scale(fdk_platform_window *pwindow,
                                        fdk_f32 *out_scale) {
    if (pwindow == NULL || out_scale == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_scale = (fdk_f32)pwindow->scale_x120 / 120.0f;
    return FDK_OK;
}

fdk_result fdk_wayland_window_get_framebuffer(fdk_platform_window *pwindow,
                                               fdk_platform_framebuffer *out_fb) {
    if (pwindow == NULL || out_fb == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* PHYSICAL buffer size: the window's logical size at the current
     * scale (ceil for fractional factors). A scale change therefore
     * changes the fb size, which the surface layer treats exactly
     * like a resize — damage resets to full and the app redraws. */
    fdk_size size;
    size.width = physical_dim(pwindow->last_size.width < 1
                                  ? 1
                                  : pwindow->last_size.width,
                              pwindow->scale_x120);
    size.height = physical_dim(pwindow->last_size.height < 1
                                   ? 1
                                   : pwindow->last_size.height,
                               pwindow->scale_x120);

    /* Reuse the pending buffer when the size still matches — apps
     * that draw in several passes (or call get_info more than once
     * per frame) don't allocate a buffer per call. */
    if (pwindow->render_pending != NULL) {
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer == pwindow->render_pending) {
                if (pwindow->render_slots[i].size.width == size.width &&
                    pwindow->render_slots[i].size.height == size.height) {
                    out_fb->pixels = pwindow->render_slots[i].pixels;
                    out_fb->width = size.width;
                    out_fb->height = size.height;
                    out_fb->stride = size.width;
                    return FDK_OK;
                }
                /* Stale pending (window resized since acquisition):
                 * drop it and make a fresh one below. */
                wl_buffer_destroy(pwindow->render_slots[i].buffer);
                munmap(pwindow->render_slots[i].pixels,
                       pwindow->render_slots[i].length);
                pwindow->render_slots[i].buffer = NULL;
                pwindow->render_slots[i].pixels = NULL;
                pwindow->render_slots[i].length = 0;
                pwindow->render_slots[i].size.width = 0;
                pwindow->render_slots[i].size.height = 0;
                pwindow->render_slots[i].released = 0;
                pwindow->render_pending = NULL;
                break;
            }
        }
    }

    /* Reap released slots at the wrong size — they can never serve a
     * future acquisition and only eat capacity. */
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer != NULL &&
            pwindow->render_slots[i].released &&
            pwindow->render_slots[i].buffer != pwindow->buffer &&
            (pwindow->render_slots[i].size.width != size.width ||
             pwindow->render_slots[i].size.height != size.height)) {
            wl_buffer_destroy(pwindow->render_slots[i].buffer);
            munmap(pwindow->render_slots[i].pixels,
                   pwindow->render_slots[i].length);
            pwindow->render_slots[i].buffer = NULL;
            pwindow->render_slots[i].pixels = NULL;
            pwindow->render_slots[i].length = 0;
            pwindow->render_slots[i].size.width = 0;
            pwindow->render_slots[i].size.height = 0;
            pwindow->render_slots[i].released = 0;
        }
    }

    /* Prefer recycling a released slot at the current size. */
    int slot = -1;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer != NULL &&
            pwindow->render_slots[i].released &&
            pwindow->render_slots[i].size.width == size.width &&
            pwindow->render_slots[i].size.height == size.height) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Otherwise take an empty slot and create a buffer in it. */
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer == NULL) {
                slot = i;
                break;
            }
        }
    }
    /* Release-wait budget, classified below (full vs churn); the
     * wrong-size reaper after the wait cites the same number. */
    int wait_budget = FDK_WL_RELEASE_WAIT_MS;
    if (slot < 0) {
        /* 1.1.6: every slot is busy — WAIT, don't drop the frame.
         *
         * The pre-1.1.6 behavior (refuse immediately, loudly) was
         * built on the assumption that a compositor holding all four
         * buffers for long is misbehaving. The live report that
         * broke it: Muffin's experimental Wayland session on older
         * hardware — perfectly allowed to take >250ms per present
         * under load — turned every guard-expired repaint attempt
         * into a refusal, ~70 per interaction burst, and the dropped
         * frames read as "Wayland feels slower than X11".
         *
         * So: flush our committed requests (the compositor cannot
         * release what it has not received), then dispatch whatever
         * release events already arrived, then poll the display fd
         * for the wait budget in 10ms steps. Releases ride the
         * dedicated release_queue, so this wait ONLY ever runs
         * wl_buffer listeners — safe even though acquisition can be
         * reached from inside a listener (the dispatch tail's
         * synchronous repaint).
         *
         * 1.1.7: the budget is classified, not flat. The 1.1.6 wait
         * exited ONLY on a release at the CURRENT size — during an
         * interactive resize every busy slot is at a WRONG size (each
         * configure step changes the target), so no release could
         * ever match and the full 100ms burned per frame before the
         * wrong-size reaper below fired anyway: ~10fps resizing with
         * an idle CPU (the "still laggy when resize / CPU not being
         * fully used" live report). Now: pure hoarding (every busy
         * slot at the current size) keeps the full 1.1.6 budget — a
         * release there is recyclable and worth waiting for — while
         * churn (any busy slot at a wrong size) gets ONE bounded
         * poll slice, because every exit from a churn wait ends in
         * the reaper or a fresh allocation regardless. Any release
         * is progress in here: right-size recycles, wrong-size is
         * reaped on the spot and its slot reused. */
        int churn = 0;
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer != NULL &&
                !pwindow->render_slots[i].released &&
                (pwindow->render_slots[i].size.width != size.width ||
                 pwindow->render_slots[i].size.height != size.height)) {
                churn = 1;
                break;
            }
        }
        wait_budget = churn ? FDK_WL_CHURN_WAIT_MS
                             : FDK_WL_RELEASE_WAIT_MS;
        fdk_i64 wait_deadline = now_ms() + wait_budget;
        while (slot < 0 && now_ms() < wait_deadline) {
            (void)wl_display_flush(pwindow->conn->display);
            (void)fdk_wayland_release_queue_dispatch(pwindow->conn);
            for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
                if (pwindow->render_slots[i].buffer != NULL &&
                    pwindow->render_slots[i].released &&
                    pwindow->render_slots[i].buffer != pwindow->buffer) {
                    if (pwindow->render_slots[i].size.width == size.width &&
                        pwindow->render_slots[i].size.height == size.height) {
                        slot = i; /* a release we can recycle */
                        break;
                    }
                    /* A release at the wrong size: progress, not a
                     * miss — reap it now and reuse the slot (the
                     * top-of-function reaper cannot run inside this
                     * loop). */
                    wl_buffer_destroy(pwindow->render_slots[i].buffer);
                    munmap(pwindow->render_slots[i].pixels,
                           pwindow->render_slots[i].length);
                    pwindow->render_slots[i].buffer = NULL;
                    pwindow->render_slots[i].pixels = NULL;
                    pwindow->render_slots[i].length = 0;
                    pwindow->render_slots[i].size.width = 0;
                    pwindow->render_slots[i].size.height = 0;
                    pwindow->render_slots[i].released = 0;
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                break;
            }
            /* Nothing buffered — READ from the display fd, bounded.
             * The prepare_read/poll/read_events dance is the same
             * libwayland idiom wayland_dispatch.c uses (and Mesa's
             * swapchain waits): read_events routes every event to its
             * target queue, then the release-queue dispatch above
             * credits only wl_buffer listeners — a configure read
             * here stays buffered for the main loop, un-run. Safe to
             * nest inside a listener: single-threaded, and the outer
             * dispatch_pending holds no read marker while listeners
             * execute. */
            int remaining = (int)(wait_deadline - now_ms());
            if (remaining > 10) {
                remaining = 10;
            }
            if (wl_display_prepare_read(pwindow->conn->display) == 0) {
                struct pollfd pfd = {
                    .fd = wl_display_get_fd(pwindow->conn->display),
                    .events = POLLIN,
                };
                int pr = poll(&pfd, 1, remaining);
                if (pr > 0 && (pfd.revents & POLLIN)) {
                    if (wl_display_read_events(pwindow->conn->display) < 0) {
                        break; /* connection error: stop waiting (the
                                    read marker is consumed even on
                                    failure — do NOT cancel_read) */
                    }
                    (void)fdk_wayland_release_queue_dispatch(pwindow->conn);
                } else {
                    wl_display_cancel_read(pwindow->conn->display);
                }
            } else {
                /* Events already queued somewhere: dispatch_pending
                 * above will have consumed ours next iteration; do
                 * not spin the CPU waiting for the compositor. */
                usleep(1000);
            }
        }
    }

    if (slot < 0) {
        /* Still nothing at the right size. LAST RESORT before
         * refusing: reap an unreleased slot at the WRONG size. Each
         * buffer owns its memfd pool, and destroying a wl_shm
         * wl_buffer after commit is legal — the compositor keeps its
         * server-side mapping — so this only costs the churn of a
         * fresh allocation. Interactive resizes need exactly this:
         * every configure step changes the size, so the pool fills
         * with wrong-size slots the compositor has not released yet,
         * and refusing there stalled the whole maximize cycle on
         * slow compositors (the 1.1.5 live report's burst of 40+
         * refusals during maximize toggling). Since 1.1.7 the churn
         * budget reaches this reaper after ONE poll slice instead of
         * the full release wait — the wait could never satisfy a
         * current-size recycle while every busy slot is wrong-size
         * anyway. */
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer != NULL &&
                pwindow->render_slots[i].buffer != pwindow->buffer &&
                (pwindow->render_slots[i].size.width != size.width ||
                 pwindow->render_slots[i].size.height != size.height)) {
                wl_buffer_destroy(pwindow->render_slots[i].buffer);
                munmap(pwindow->render_slots[i].pixels,
                       pwindow->render_slots[i].length);
                pwindow->render_slots[i].buffer = NULL;
                pwindow->render_slots[i].pixels = NULL;
                pwindow->render_slots[i].length = 0;
                pwindow->render_slots[i].size.width = 0;
                pwindow->render_slots[i].size.height = 0;
                pwindow->render_slots[i].released = 0;
                slot = i;
                FDK_DEBUG("reaped an unreleased wrong-size render slot "
                          "after a %dms release wait (interactive resize)",
                          wait_budget);
                break;
            }
        }
    }

    if (slot < 0) {
        /* Genuine refusal: every slot holds a buffer at the CURRENT
         * size the compositor has not released — it is hoarding
         * frames it asked for. Rate-limit the warning (one per stall
         * episode; the 1.1.5 live report counted ~80 identical lines
         * in a single session) and hand the caller an honest error;
         * the damage stays set and the frame goes out on the next
         * successful acquisition. */
        fdk_i64 now = now_ms();
        if (pwindow->last_refuse_warn_ms == 0 ||
            now - pwindow->last_refuse_warn_ms >
                FDK_WL_REFUSE_WARN_INTERVAL_MS) {
            pwindow->last_refuse_warn_ms = now;
            FDK_WARN("all %d render buffers in flight after a %dms "
                     "release wait — compositor not releasing; "
                     "deferring this frame",
                     FDK_WL_RENDER_SLOTS, FDK_WL_RELEASE_WAIT_MS);
        } else {
            FDK_DEBUG("render acquisition still refused (rate-limited "
                      "repeat of the episode's WARN)");
        }
        return FDK_ERR_SURFACE_CREATE;
    }

    if (pwindow->render_slots[slot].buffer == NULL) {
        struct wl_buffer *buffer = NULL;
        uint32_t *pixels = NULL;
        size_t length = 0;
        fdk_result r = create_shm_buffer(pwindow->conn, size, &buffer,
                                         &pixels, &length);
        if (!fdk_ok(r)) {
            return r;
        }
        wl_buffer_add_listener(buffer, &g_render_buffer_listener, pwindow);
        pwindow->render_slots[slot].buffer = buffer;
        pwindow->render_slots[slot].pixels = pixels;
        pwindow->render_slots[slot].length = length;
        pwindow->render_slots[slot].size = size;
        pwindow->render_slots[slot].released = 0;
    }

    /* Pre-fill with the visible frame (see prefetch_visible_frame):
     * the surface layer's damage model then holds exactly. */
    prefetch_visible_frame(pwindow, slot);

    pwindow->render_pending = pwindow->render_slots[slot].buffer;

    FDK_DEBUG("render framebuffer acquired (%dx%d, slot %d, %s)",
              size.width, size.height, slot,
              pwindow->render_slots[slot].released ? "recycled" : "fresh");

    out_fb->pixels = pwindow->render_slots[slot].pixels;
    out_fb->width = size.width;
    out_fb->height = size.height;
    out_fb->stride = size.width;
    return FDK_OK;
}

/* Commits the acquired render_pending buffer: attach, damage hints,
 * commit, frame-callback registration, live-buffer bookkeeping.
 * Shared by fdk_wayland_window_present() and the deferred-first-frame
 * path in xdg_surface_configure() (which commits the buffer a present
 * deferred BEFORE the first configure was ever able to). */
static fdk_result commit_render_pending(fdk_platform_window *pwindow,
                                        const fdk_platform_damage *damage) {
    fdk_size size = { .width = 0, .height = 0 };
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == pwindow->render_pending) {
            size = pwindow->render_slots[i].size;
            pwindow->render_slots[i].released = 0; /* becomes live */
            break;
        }
    }

    /* HiDPI: push the current scale to the protocol before this
     * commit (set_buffer_scale / viewport — see apply_window_scale),
     * so the freshly attached physical buffer is interpreted at the
     * right factor. */
    apply_window_scale(pwindow);

    wl_surface_attach(pwindow->surface, pwindow->render_pending, 0, 0);

    /* Damage hints. full == 1 (or the defensive count == 0 with
     * full == 0) uses the whole-surface idiom; otherwise each rect
     * is clamped to the buffer and sent as its own hint — the
     * compositor can then limit its own repaint to what actually
     * changed. Compositors are allowed to ignore hints entirely,
     * which is why get_framebuffer pre-fills buffers with the
     * visible frame (see prefetch_visible_frame).
     *
     * Damage rects arrive in PHYSICAL buffer pixels; wl_surface_damage
     * takes SURFACE-LOCAL LOGICAL coordinates, so each edge converts
     * by 120/scale_x120, rounding OUTWARD (the hinted region must
     * cover every damaged physical pixel — over-hinting is always
     * safe, under-hinting is not). */
    if (damage->full || damage->count == 0) {
        wl_surface_damage(pwindow->surface, 0, 0, INT32_MAX, INT32_MAX);
    } else {
        for (int i = 0; i < damage->count; i++) {
            const fdk_rect *rc = &damage->rects[i];
            long long x0 = rc->x, y0 = rc->y;
            long long x1 = (long long)rc->x + rc->width;
            long long y1 = (long long)rc->y + rc->height;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > size.width) x1 = size.width;
            if (y1 > size.height) y1 = size.height;
            if (x1 > x0 && y1 > y0) {
                long long lx0 = x0 * 120 / pwindow->scale_x120;
                long long ly0 = y0 * 120 / pwindow->scale_x120;
                long long lx1 =
                    (x1 * 120 + pwindow->scale_x120 - 1) /
                    pwindow->scale_x120;
                long long ly1 =
                    (y1 * 120 + pwindow->scale_x120 - 1) /
                    pwindow->scale_x120;
                wl_surface_damage(pwindow->surface, (int32_t)lx0,
                                  (int32_t)ly0, (int32_t)(lx1 - lx0),
                                  (int32_t)(ly1 - ly0));
            }
        }
    }
    wl_surface_commit(pwindow->surface);

    pwindow->rendered_ever = 1;

    /* Frame callback for pacing: fires when the compositor has
     * presented this commit (see frame_callback_done). Tracked in
     * pwindow->frame_cb so window_destroy can reap one that never
     * fires. */
    struct wl_callback *cb = wl_surface_frame(pwindow->surface);
    if (cb != NULL) {
        /* A previous callback that never fired (hidden/minimized
         * surface, or a compositor that simply hasn't presented yet)
         * must be destroyed BEFORE the pointer is replaced — the
         * leaked wl_callback proxy found by the HiDPI test's second
         * paint. Its frame_ack stays 0; the pacing guard covers it. */
        if (pwindow->frame_cb != NULL) {
            wl_callback_destroy(pwindow->frame_cb);
        }
        wl_callback_add_listener(cb, &g_frame_callback_listener, pwindow);
        pwindow->frame_cb = cb;
    }
    pwindow->frame_ack = 0;
    pwindow->frame_commit_ms = now_ms();

    /* The pending buffer becomes the surface's live buffer; the
     * PREVIOUS live buffer (background or earlier render frame) gets
     * released by the compositor after this commit supersedes it,
     * which flips its slot to `released` for recycling. */
    pwindow->buffer = pwindow->render_pending;
    pwindow->buffer_size = size;
    pwindow->buffer_attached = 1;
    pwindow->render_pending = NULL;

    FDK_DEBUG("render buffer presented (%dx%d, %d damage rects)",
              size.width, size.height,
              damage->full ? -1 : damage->count);
    return FDK_OK;
}

fdk_result fdk_wayland_window_present(fdk_platform_window *pwindow,
                                      const fdk_platform_damage *damage) {
    if (pwindow == NULL || damage == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Documented no-op when nothing was ever acquired/drawn. */
    if (pwindow->render_pending == NULL) {
        return FDK_OK;
    }

    /* Nothing changed — no attach, no damage, no commit, and the
     * acquired buffer stays pending for the next frame (the
     * damage-tracking contract; a no-op frame costs zero protocol
     * traffic). */
    if (!damage->full && damage->count == 0) {
        return FDK_OK;
    }

    if (!pwindow->configured) {
        /* xdg-shell forbids the first real commit before
         * ack_configure; the buffer stays pending and is committed
         * by xdg_surface_configure() the moment the first configure
         * is acked (see the deferred-first-frame block there — the
         * surface layer has already consumed this frame's damage, so
         * waiting for "the application's next present" would wait
         * forever: with the tree clean it would be a no-op). */
        FDK_DEBUG("present deferred until first xdg configure");
        return FDK_OK;
    }

    return commit_render_pending(pwindow, damage);
}

int fdk_wayland_window_frame_ready(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return 1;
    }
    if (pwindow->frame_ack) {
        return 1;
    }
    if (!pwindow->rendered_ever) {
        return 1; /* nothing presented yet — always allowed */
    }
    /* 1.1.6 capacity gate: the starvation guard below exists so a
     * hidden surface (compositor never sends frame callbacks) does
     * not starve. But "ready" while every render slot is in flight
     * is a LIE the pre-1.1.6 code told ~70 times per interaction on
     * Muffin's experimental Wayland session: the guard expired, the
     * app painted, acquisition refused, the frame dropped, repeat
     * every loop pass. Gating on capacity means the app's paint
     * loop waits (its prerogative) instead of spinning through
     * refusals — while hidden surfaces still pass the guard and get
     * their one honest frame when a slot frees (the wrong-size
     * reaper in get_framebuffer guarantees progress on resizes). */
    int capacity = 0;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == NULL ||
            pwindow->render_slots[i].released) {
            capacity = 1;
            break;
        }
    }
    if (!capacity) {
        return 0;
    }
    /* Starvation guard: hidden surfaces never get frame callbacks,
     * and a wedged compositor must not wedge every FDK app. Pacing
     * degrades to a floor rate instead of stopping. */
    return (now_ms() - pwindow->frame_commit_ms) > FDK_WL_FRAME_GUARD_MS;
}

int fdk_wayland_window_ever_presented(fdk_platform_window *pwindow) {
    return pwindow != NULL && pwindow->rendered_ever != 0;
}

fdk_result fdk_wayland_window_create(fdk_platform_connection *conn,
                                      const fdk_window_options *options,
                                      fdk_platform_window *parent,
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
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->toplevel_decoration = NULL;
    pwindow->deco_client_side = 0;
    pwindow->buffer = NULL;
    pwindow->buffer_size.width = 0;
    pwindow->buffer_size.height = 0;
    pwindow->buffer_attached = 0;
    pwindow->render_pending = NULL;
    pwindow->rendered_ever = 0;
    pwindow->last_refuse_warn_ms = 0;
    pwindow->scale_x120 = 120;    /* 1x until an output says otherwise */
    pwindow->buffer_scale = 1;
    pwindow->scale_applied = 0;   /* applied on the first commit      */
    pwindow->viewport = NULL;
    pwindow->fractional = NULL;
    pwindow->entered_outputs = NULL;
    pwindow->entered_count = 0;
    pwindow->entered_capacity = 0;
    pwindow->frame_ack = 1;       /* nothing presented yet = ready */
    pwindow->frame_commit_ms = 0;
    pwindow->frame_cb = NULL;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        pwindow->render_slots[i].buffer = NULL;
        pwindow->render_slots[i].pixels = NULL;
        pwindow->render_slots[i].length = 0;
        pwindow->render_slots[i].size.width = 0;
        pwindow->render_slots[i].size.height = 0;
        pwindow->render_slots[i].released = 0;
    }

    pwindow->surface = wl_compositor_create_surface(conn->compositor);
    if (pwindow->surface == NULL) {
        FDK_ERROR("wl_compositor_create_surface failed");
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }

    wl_surface_add_listener(pwindow->surface, &g_wl_surface_listener,
                             pwindow);
    pwindow->xdg_surface = xdg_wm_base_get_xdg_surface(conn->wm_base, pwindow->surface);
    if (pwindow->xdg_surface == NULL) {
        FDK_ERROR("xdg_wm_base_get_xdg_surface failed");
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }
    xdg_surface_add_listener(pwindow->xdg_surface, &g_xdg_surface_listener, pwindow);

    int is_popup = (options != NULL && options->popup != 0);
    pwindow->popup = is_popup;
    pwindow->drop_formats = 0; /* fdk_window_set_drop_formats fills it */
    pwindow->xdg_popup = NULL;

    if (is_popup) {
        /* Phase 9 popup: xdg_positioner anchored at the parent-
         * relative point, xdg_popup on the PARENT's xdg_surface.
         * The grab cites the newest input serial (compositors may
         * refuse serial 0 — documented in fdk_window.h); popup_done
         * (the compositor's dismissal) translates to a close request
         * in the popup listener below.
         *
         * Phase 9 completion — NESTED popups (menu submenus): the
         * parent may be another popup's xdg_surface, exactly as the
         * protocol prescribes ("the parent of a grabbing popup must
         * either be an xdg_toplevel surface or another xdg_popup
         * with an explicit grab"). The grab auto-returns to the
         * parent when the topmost popup is destroyed, so no re-grab
         * request exists here (unlike X11). Nested popups MUST be
         * destroyed topmost-first — the window layer's popup-family
         * sweep guarantees that order. */
        if (parent == NULL || (parent->xdg_toplevel == NULL &&
                               parent->xdg_popup == NULL)) {
            FDK_ERROR("popup window needs a toplevel or popup parent");
            xdg_surface_destroy(pwindow->xdg_surface);
            wl_surface_destroy(pwindow->surface);
            fdk_free(pwindow);
            return FDK_ERR_INVALID_ARGUMENT;
        }
        struct xdg_positioner *pos =
            xdg_wm_base_create_positioner(conn->wm_base);
        if (pos == NULL) {
            FDK_ERROR("xdg_wm_base_create_positioner failed");
            xdg_surface_destroy(pwindow->xdg_surface);
            wl_surface_destroy(pwindow->surface);
            fdk_free(pwindow);
            return FDK_ERR_WINDOW_CREATE;
        }
        fdk_i32 px = (options != NULL) ? options->x : 0;
        fdk_i32 py = (options != NULL) ? options->y : 0;
        xdg_positioner_set_anchor_rect(pos, px, py, 1, 1);
        xdg_positioner_set_size(pos, width, height);
        xdg_positioner_set_anchor(
            pos, XDG_POSITIONER_ANCHOR_BOTTOM_LEFT);
        xdg_positioner_set_gravity(
            pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        xdg_positioner_set_constraint_adjustment(
            pos, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_X |
                     XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y);
        pwindow->xdg_popup = xdg_surface_get_popup(
            pwindow->xdg_surface, parent->xdg_surface, pos);
        xdg_positioner_destroy(pos);
        if (pwindow->xdg_popup == NULL) {
            FDK_ERROR("xdg_surface_get_popup failed");
            xdg_surface_destroy(pwindow->xdg_surface);
            wl_surface_destroy(pwindow->surface);
            fdk_free(pwindow);
            return FDK_ERR_WINDOW_CREATE;
        }
        xdg_popup_add_listener(pwindow->xdg_popup,
                               &g_xdg_popup_listener, pwindow);
        /* Grab needs a REAL seat with a REAL input serial — a popup
         * without a grab is still valid protocol (compositors may
         * dismiss it on focus loss instead). On a seat-less
         * compositor (kiosk-shell weston) the grab would marshal a
         * NULL object: libwayland logs an argument error and drops
         * the whole request (1.1.5, seen live in the manager-less
         * rig's stderr); skip it cleanly instead. */
        if (conn->seat != NULL) {
            xdg_popup_grab(pwindow->xdg_popup, conn->seat,
                           conn->last_input_serial);
        }
        pwindow->xdg_toplevel = NULL;
        /* Popups commit a buffer on show like any window; the
         * configure handshake (xdg_surface.configure) drives the
         * first commit, same as toplevels. */
    } else {
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
    /* The application id (fdk_init_options.app_id): the compositor
     * identity window rules and taskbars match. Best-effort NULL —
     * the protocol allows omitting it (compositors then fall back to
     * the title). */
    xdg_toplevel_set_app_id(pwindow->xdg_toplevel,
                            conn->app_id != NULL ? conn->app_id : "fdk.app");
    }

    /* xdg-decoration object creation must happen HERE, before any
     * buffer is ever attached: the protocol is explicit that a
     * toplevel decoration created after the surface has a buffer is
     * a fatal protocol error (caught live by sway: "xdg_toplevel_
     * decoration must not have a buffer at creation"). set_mode may
     * then be called at any time — fdk_window_set_decorated() does
     * that part. Created only when the compositor advertised the
     * manager global; with the global absent there is simply no
     * server-side chrome to negotiate away — client-side is the
     * xdg-shell default (set_wm_decorations splits the semantics).
     *
     * TOPLEVELS ONLY: popups have no xdg_toplevel (passing the NULL
     * here marshals an error that poisons the whole connection —
     * every later flush fails with EINVAL; found live against sway
     * by the Phase 9 menu tests, the first real popup user). */
    if (conn->decoration_manager != NULL &&
        pwindow->xdg_toplevel != NULL) {
        pwindow->toplevel_decoration =
            zxdg_decoration_manager_v1_get_toplevel_decoration(
                conn->decoration_manager, pwindow->xdg_toplevel);
        if (pwindow->toplevel_decoration == NULL) {
            FDK_WARN("zxdg_decoration_manager_v1_get_toplevel_decoration "
                     "failed; decorations stay compositor-side");
        } else {
            zxdg_toplevel_decoration_v1_add_listener(
                pwindow->toplevel_decoration,
                &g_toplevel_decoration_listener, pwindow);
        }
    }

    /* HiDPI (Phase 3 completion): the fractional-scale LISTENER is
     * created at WINDOW-CREATE time — surface state with no
     * buffer-ordering constraint, and binding early means the
     * compositor's preferred_scale event (which arrives around the
     * first configure) is never missed. The VIEWPORT itself is NOT
     * created here (see apply_window_scale): an unconfigured
     * viewport means "never display" in the viewporter protocol,
     * and the first commit carrying one kept compositors from ever
     * mapping the window. It is born lazily, configured in the same
     * commit batch as its first use. */
    if (conn->fractional_manager != NULL) {
        pwindow->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            conn->fractional_manager, pwindow->surface);
        if (pwindow->fractional == NULL) {
            FDK_WARN("get_fractional_scale failed; output scale only "
                     "for this window");
        } else {
            wp_fractional_scale_v1_add_listener(
                pwindow->fractional, &g_fractional_scale_listener, pwindow);        }
    }

    fdk_result r = fdk_wayland_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        if (pwindow->fractional != NULL) {
            wp_fractional_scale_v1_destroy(pwindow->fractional);
        }
        if (pwindow->viewport != NULL) {
            wp_viewport_destroy(pwindow->viewport);
        }
        if (pwindow->toplevel_decoration != NULL) {
            zxdg_toplevel_decoration_v1_destroy(
                pwindow->toplevel_decoration);
        }
        if (pwindow->xdg_popup != NULL) {
            xdg_popup_destroy(pwindow->xdg_popup);
        }
        if (pwindow->xdg_toplevel != NULL) {
            xdg_toplevel_destroy(pwindow->xdg_toplevel);
        }
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
    /* Render buffers first: this may destroy the live buffer if it is
     * a render one (clearing pwindow->buffer), so the background
     * cleanup below never double-destroys. After wl_surface_destroy
     * the compositor discards pending state and the connection is
     * about to go away anyway, so waiting for wl_buffer::release here
     * would be pointless — same reasoning as the background buffer
     * below. */
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer != NULL) {
            if (pwindow->buffer == pwindow->render_slots[i].buffer) {
                pwindow->buffer = NULL;
                pwindow->buffer_attached = 0;
            }
            wl_buffer_destroy(pwindow->render_slots[i].buffer);
            munmap(pwindow->render_slots[i].pixels,
                   pwindow->render_slots[i].length);
            pwindow->render_slots[i].buffer = NULL;
            pwindow->render_slots[i].pixels = NULL;
            pwindow->render_slots[i].length = 0;
            pwindow->render_slots[i].released = 0;
        }
    }
    pwindow->render_pending = NULL;

    /* HiDPI objects die with the window (both wrap this surface). */
    if (pwindow->fractional != NULL) {
        wp_fractional_scale_v1_destroy(pwindow->fractional);
        pwindow->fractional = NULL;
    }
    if (pwindow->viewport != NULL) {
        wp_viewport_destroy(pwindow->viewport);
        pwindow->viewport = NULL;
    }
    fdk_free(pwindow->entered_outputs);
    pwindow->entered_outputs = NULL;
    pwindow->entered_count = 0;
    pwindow->entered_capacity = 0;

    /* If a background buffer is still attached here (it usually was
     * already released and destroyed by its release listener), drop
     * our reference before tearing down the surface. */
    if (pwindow->buffer != NULL) {
        wl_buffer_destroy(pwindow->buffer);
        pwindow->buffer = NULL;
        pwindow->buffer_attached = 0;
    }
    fdk_wayland_unregister_window(pwindow->conn, pwindow);
    /* A frame callback still awaiting `done` when the window dies
     * will never fire — reap its proxy (leak found by the sway
     * headless test). */
    if (pwindow->frame_cb != NULL) {
        wl_callback_destroy(pwindow->frame_cb);
        pwindow->frame_cb = NULL;
    }
    /* The decoration object (if any) must die before the toplevel it
     * wraps — xdg-decoration objects are inert after the toplevel is
     * gone, and destroying in the wrong order is a protocol error. */
    if (pwindow->toplevel_decoration != NULL) {
        zxdg_toplevel_decoration_v1_destroy(pwindow->toplevel_decoration);
        pwindow->toplevel_decoration = NULL;
    }
    if (pwindow->xdg_popup != NULL) {
        xdg_popup_destroy(pwindow->xdg_popup);
        pwindow->xdg_popup = NULL;
    }
    if (pwindow->xdg_toplevel != NULL) {
        xdg_toplevel_destroy(pwindow->xdg_toplevel);
        pwindow->xdg_toplevel = NULL;
    }
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
    /* Compositor-owned geometry (1.1.5): while the toplevel is
     * maximized or fullscreen — kiosk-shell weston configures EVERY
     * window fullscreen — xdg-shell requires the client to commit
     * the CONFIGURED size. A client-driven buffer at another size
     * is a protocol error on strict compositors: weston kills the
     * whole connection ("xdg_surface geometry (512 x 760) is larger
     * than the configured fullscreen state (1024 x 640)" — found
     * live by the 1.1.5 manager-less rig; the suite kept "passing"
     * on the dead connection afterwards). Refuse honestly and keep
     * the compositor's size; a resize after restore works normally. */
    if (pwindow->maximized || pwindow->fullscreen) {
        FDK_WARN("fdk_window_resize() refused while the window is %s "
                 "(the compositor owns the geometry; resize after "
                 "restoring)",
                 pwindow->fullscreen ? "fullscreen" : "maximized");
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
    if (pwindow->rendered_ever || pwindow->render_pending != NULL) {
        /* The application renders via fdk_surface: it will acquire a
         * framebuffer at the new size and present it, and the
         * toplevel's on-screen size follows that commit — committing
         * a solid background buffer here would flash white over its
         * content. Just record the new size. */
        pwindow->last_size.width = width;
        pwindow->last_size.height = height;
        pwindow->pending_size = pwindow->last_size;
        /* ...and notify the frontend the way a compositor configure
         * would. Without this, nothing ever invalidates the widget
         * tree (the tree's own damage tracking says "clean"; no
         * compositor configure arrives because the compositor only
         * reacts to a commit we never make) — the resize deadlocks:
         * no repaint, no commit, no confirmation, the window stays
         * at its old size forever. Found by the Phase 5 Wayland
         * reflow test. The event is optimistic in the same sense
         * resize itself is: a compositor that disagrees answers with
         * a real configure that corrects the bookkeeping. */
        fdk_event_data event;
        memset(&event, 0, sizeof event);
        event.type = FDK_EVENT_WINDOW_CONFIGURE;
        event.configure.size = pwindow->last_size;
        pwindow->conn->dispatch(pwindow, &event,
                                pwindow->conn->dispatch_user_data);
        return;
    }
    if (pwindow->buffer_size.width == width && pwindow->buffer_size.height == height) {
        return; /* already at that size — avoid a pointless new buffer */
    }
    /* Background path resizes in PHYSICAL pixels too (the buffer
     * must cover the scaled surface; a logical-sized solid buffer
     * would display at 1/scale its size). */
    fdk_size new_size = {
        .width = physical_dim(width < 1 ? 1 : width, pwindow->scale_x120),
        .height = physical_dim(height < 1 ? 1 : height, pwindow->scale_x120),
    };
    if (!fdk_ok(attach_background_buffer(pwindow, new_size))) {
        return; /* attach_background_buffer already logged the reason */
    }
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
}

void fdk_wayland_window_set_size_limits(fdk_platform_window *pwindow,
                                         fdk_size min_size, fdk_size max_size) {
    xdg_toplevel_set_min_size(pwindow->xdg_toplevel, min_size.width, min_size.height);
    xdg_toplevel_set_max_size(pwindow->xdg_toplevel, max_size.width, max_size.height);
}

/* ---- Phase 8: window management (xdg-decoration + toplevel ops) ---- */

/* Compare-and-flip + FDK_EVENT_WINDOW_STATE dispatch (no-op when the
 * state didn't change). Called from xdg_toplevel_configure (the
 * compositor's authoritative states) and set_minimized (the
 * request-optimistic flip). */
void fdk_wayland_window_update_state(fdk_platform_window *pwindow,
                                     int maximized, int minimized) {
    if (pwindow->maximized == maximized && pwindow->minimized == minimized) {
        return;
    }
    pwindow->maximized = maximized;
    pwindow->minimized = minimized;
    fdk_event_data event;
    memset(&event, 0, sizeof event);
    event.type = FDK_EVENT_WINDOW_STATE;
    event.state.maximized = maximized;
    event.state.minimized = minimized;
    pwindow->conn->dispatch(pwindow, &event, pwindow->conn->dispatch_user_data);
}

/* zxdg_toplevel_decoration_v1::configure — the compositor's answer to
 * our set_mode request. When it confirms CLIENT_SIDE we simply record
 * it (the band fdk_window_set_decorated() already drew is the correct
 * outcome). When it answers SERVER_SIDE against our CLIENT request,
 * FDK must NOT draw a band over the compositor's decorations: emit
 * FDK_EVENT_WINDOW_DECORATION{client_side=0}; the window layer tears
 * its band down on receipt (see window.c's dispatch handling). */
static void toplevel_decoration_configure(void *data,
                                          struct zxdg_toplevel_decoration_v1 *deco,
                                          uint32_t mode) {
    (void)deco;
    fdk_platform_window *pwindow = data;
    int client_side = (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE) ? 1 : 0;
    if (pwindow->deco_client_side == client_side) {
        pwindow->deco_client_side = client_side; /* first-time bookkeeping */
        return;
    }
    pwindow->deco_client_side = client_side;
    if (!client_side) {
        fdk_event_data event;
        memset(&event, 0, sizeof event);
        event.type = FDK_EVENT_WINDOW_DECORATION;
        event.decoration.client_side = 0;
        pwindow->conn->dispatch(pwindow, &event,
                                pwindow->conn->dispatch_user_data);
    }
}

static const struct zxdg_toplevel_decoration_v1_listener
    g_toplevel_decoration_listener = {
    .configure = toplevel_decoration_configure,
};

fdk_result fdk_wayland_window_set_wm_decorations(fdk_platform_window *pwindow,
                                                 bool on) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* The decoration object exists only when (a) the compositor
     * advertised zxdg_decoration_manager_v1 and (b) it was created at
     * window-create time, before the first buffer (a protocol
     * requirement — see wayland_window_create). No object splits by
     * direction:
     *  - on=false (FDK draws its own band): a compositor that does
     *    NOT advertise zxdg_decoration_manager_v1 never draws chrome
     *    itself — client-side is the xdg-shell default — so FDK's
     *    band is the only chrome that will ever exist. Succeed
     *    without a protocol request; there is nothing to stack on.
     *    (weston kiosk-shell, Muffin's experimental Wayland session,
     *    and most tiling WMs are exactly this.)
     *  - on=true (compositor chrome wanted): genuinely unsupported —
     *    there is no protocol way to ask for server-side decorations,
     *    and the window layer must keep FDK's band so the window
     *    stays usable (the old blanket UNSUPPORTED here used to kill
     *    set_decorated(true) too, which made the decorations demo
     *    exit before it ever mapped a window on such compositors). */
    if (pwindow->toplevel_decoration == NULL) {
        return on ? FDK_ERR_UNSUPPORTED : FDK_OK;
    }
    /* on = the compositor draws chrome -> SERVER_SIDE;
     * off = FDK draws its own band  -> CLIENT_SIDE. The compositor's
     * answer arrives asynchronously in toplevel_decoration_configure. */
    zxdg_toplevel_decoration_v1_set_mode(
        pwindow->toplevel_decoration,
        on ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
           : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    return FDK_OK;
}

fdk_result fdk_wayland_window_set_maximized(fdk_platform_window *pwindow,
                                            bool maximized) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (maximized) {
        xdg_toplevel_set_maximized(pwindow->xdg_toplevel);
    } else {
        xdg_toplevel_unset_maximized(pwindow->xdg_toplevel);
    }
    /* No optimistic flip: the compositor answers with a configure
     * carrying MAXIMIZED in states (or not); update_state runs from
     * xdg_toplevel_configure either way. */
    return FDK_OK;
}

fdk_result fdk_wayland_window_set_minimized(fdk_platform_window *pwindow,
                                            bool minimized) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!minimized) {
        /* xdg-shell has no unminimize request — compositors unminimize
         * via activation. Tell the caller honestly instead of faking
         * a restore that cannot work. */
        return FDK_ERR_UNSUPPORTED;
    }
    xdg_toplevel_set_minimized(pwindow->xdg_toplevel);
    /* Fire-and-forget request: no acknowledgement exists, so mark
     * optimistic (cleared on the next activated configure). */
    fdk_wayland_window_update_state(pwindow, pwindow->maximized, 1);
    return FDK_OK;
}

/* xdg_toplevel resize edges — the protocol's own edge enum; FDK's
 * compass values are a straight enum-map (same order). */
static uint32_t toplevel_resize_edges(int edge) {
    switch (edge) {
    case 1: return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case 2: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case 3: return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case 4: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    case 5: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case 6: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case 7: return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case 8: return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    default: return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

fdk_result fdk_wayland_window_begin_move(fdk_platform_window *pwindow,
                                         fdk_i32 local_x, fdk_i32 local_y) {
    (void)local_x; /* the compositor tracks the pointer itself; the
                      protocol takes no coordinates, only the serial */
    (void)local_y;
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->conn->pointer == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    xdg_toplevel_move(pwindow->xdg_toplevel, pwindow->conn->seat,
                      pwindow->conn->last_button_serial);
    return FDK_OK;
}

fdk_result fdk_wayland_window_begin_resize(fdk_platform_window *pwindow,
                                           int edge, fdk_i32 local_x,
                                           fdk_i32 local_y) {
    (void)local_x;
    (void)local_y;
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->conn->pointer == NULL || edge <= 0) {
        return FDK_ERR_UNSUPPORTED;
    }
    xdg_toplevel_resize(pwindow->xdg_toplevel, pwindow->conn->seat,
                        pwindow->conn->last_button_serial,
                        toplevel_resize_edges(edge));
    return FDK_OK;
}

/* The optional pointer-introspection op (1.1.4): Wayland has no
 * query-the-server equivalent of XQueryPointer, but the seat's
 * listener state is the same truth — wl_pointer::enter/motion maintain
 * pointer_focus + surface-local pointer_x/y, and a compositor keeps
 * sending motion/leave events when surfaces move under the pointer,
 * so the cache is never stale while the pointer is over this surface.
 *
 * UNIFIED CONTRACT (1.2.5, same words as the X11 side): nonzero
 * only when the pointer is within the window's CURRENT geometry —
 * the bounds check runs against last_size, the last acked+committed
 * size, in the same logical surface space wl_pointer delivers. The
 * check matters exactly when the window shrank under a stationary
 * pointer (the unmaximize case window_revalidate_pointer exists
 * for): until the compositor's own leave arrives, the seat cache
 * still holds pointer_focus with coordinates that are now OUTSIDE
 * the surface — reporting "inside" there would route the
 * revalidation as a synthetic MOTION with out-of-bounds coordinates
 * instead of the honest LEAVE the X11 backend delivers, and every
 * consumer would have to tolerate out-of-bounds motion just for
 * this backend. With the check, both backends answer the question
 * "is the pointer inside the window's current geometry?" the same
 * way, and the leave synthesis (hover clear, cursor reset) is
 * backend-independent.
 *
 * (Cursor SHAPING: the XCursor-theme loader lives in
 * wayland_cursor.c since 1.1.6, behind the same window_set_cursor
 * op the X11 cursor-font glyphs use.) */
int fdk_wayland_window_query_pointer(fdk_platform_window *pwindow,
                                     fdk_i32 *out_x, fdk_i32 *out_y) {
    if (pwindow == NULL || out_x == NULL || out_y == NULL) {
        return 0;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->pointer == NULL || conn->pointer_focus != pwindow) {
        return 0;
    }
    *out_x = (fdk_i32)conn->pointer_x;
    *out_y = (fdk_i32)conn->pointer_y;
    return *out_x >= 0 && *out_y >= 0 &&
           *out_x < (fdk_i32)pwindow->last_size.width &&
           *out_y < (fdk_i32)pwindow->last_size.height;
}
