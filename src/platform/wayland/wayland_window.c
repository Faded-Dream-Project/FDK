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
#include <time.h>
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
     * compositors report activated when a window is brought back. */
    int maximized = 0, activated = 0;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) {
            maximized = 1;
        }
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) {
            activated = 1;
        }
    }
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

    struct wl_buffer *buffer = NULL;
    uint32_t *pixels = NULL;
    size_t length = 0;
    fdk_result r = create_shm_buffer(conn, size, &buffer, &pixels, &length);
    if (!fdk_ok(r)) {
        return r;
    }

    uint32_t pixel = WAYLAND_BG_PIXEL;
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

fdk_result fdk_wayland_window_get_framebuffer(fdk_platform_window *pwindow,
                                               fdk_platform_framebuffer *out_fb) {
    if (pwindow == NULL || out_fb == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_size size = pwindow->last_size;
    if (size.width <= 0) size.width = 1;
    if (size.height <= 0) size.height = 1;

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
    if (slot < 0) {
        /* Every slot holds a buffer the compositor has not released
         * yet. A conforming compositor releases each superseded
         * buffer at the next commit, so four in flight means it is
         * hoarding them; refusing (loudly) is safer than reusing a
         * buffer the compositor may still be scanning out. */
        FDK_WARN("all %d render buffers in flight (compositor not "
                 "releasing?) — refusing new acquisition",
                 FDK_WL_RENDER_SLOTS);
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
         * ack_configure; the buffer stays pending and the
         * application's next present (after the configure event has
         * been pumped and acked) succeeds instead. */
        FDK_DEBUG("present deferred until first xdg configure");
        return FDK_OK;
    }

    fdk_size size = { .width = 0, .height = 0 };
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == pwindow->render_pending) {
            size = pwindow->render_slots[i].size;
            pwindow->render_slots[i].released = 0; /* becomes live */
            break;
        }
    }

    wl_surface_attach(pwindow->surface, pwindow->render_pending, 0, 0);

    /* Damage hints. full == 1 (or the defensive count == 0 with
     * full == 0) uses the whole-surface idiom; otherwise each rect
     * is clamped to the buffer and sent as its own hint — the
     * compositor can then limit its own repaint to what actually
     * changed. Compositors are allowed to ignore hints entirely,
     * which is why get_framebuffer pre-fills buffers with the
     * visible frame (see prefetch_visible_frame). */
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
                wl_surface_damage(pwindow->surface, (int32_t)x0,
                                  (int32_t)y0, (int32_t)(x1 - x0),
                                  (int32_t)(y1 - y0));
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
    /* Starvation guard: hidden surfaces never get frame callbacks,
     * and a wedged compositor must not wedge every FDK app. Pacing
     * degrades to a floor rate instead of stopping. */
    return (now_ms() - pwindow->frame_commit_ms) > FDK_WL_FRAME_GUARD_MS;
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

    /* xdg-decoration object creation must happen HERE, before any
     * buffer is ever attached: the protocol is explicit that a
     * toplevel decoration created after the surface has a buffer is
     * a fatal protocol error (caught live by sway: "xdg_toplevel_
     * decoration must not have a buffer at creation"). set_mode may
     * then be called at any time — fdk_window_set_decorated() does
     * that part. Created only when the compositor advertised the
     * manager global; absent global = set_decorated honestly
     * reports FDK_ERR_UNSUPPORTED later. */
    if (conn->decoration_manager != NULL) {
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

    fdk_result r = fdk_wayland_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        if (pwindow->toplevel_decoration != NULL) {
            zxdg_toplevel_decoration_v1_destroy(
                pwindow->toplevel_decoration);
        }
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
    if (pwindow->rendered_ever || pwindow->render_pending != NULL) {
        /* The application renders via fdk_surface: it will acquire a
         * framebuffer at the new size and present it, and the
         * toplevel's on-screen size follows that commit — committing
         * a solid background buffer here would flash white over its
         * content. Just record the new size. */
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
     * requirement — see wayland_window_create). No object -> this
     * compositor offers no protocol way to drop its decorations; the
     * caller must NOT draw its own (that would stack two title bars). */
    if (pwindow->toplevel_decoration == NULL) {
        return FDK_ERR_UNSUPPORTED;
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
