#define _GNU_SOURCE /* memfd_create, MFD_CLOEXEC under -std=c17 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/viewporter-client-protocol.h"
#include "generated/fractional-scale-v1-client-protocol.h"
#include "generated/xdg-toplevel-icon-v1-client-protocol.h"
#include "generated/idle-inhibit-unstable-v1-client-protocol.h"
#include "generated/xdg-activation-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "theme/theme_internal.h"

#include <stdint.h>
#include <stdlib.h> /* getenv — the GL lane's FDK_GL_FORCE gate */
#include <string.h> /* memcpy — the lane downgrade's frame salvage */
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
    /* FD LIFETIME (1.1.0-rc1, found live on sway/labwc headless — the
     * invisible-hover/w1 chase): libwayland resolves an fd argument at
     * FLUSH time, by number. Closing it before the create_pool request
     * has been written to the socket lets the kernel hand that number
     * to the NEXT memfd — and every pool created earlier in the same
     * un-flushed burst then aliases the newest memfd on the SERVER
     * side (the client keeps its own, now-private, mappings). The
     * compositors still accept everything — buffers created, commits
     * processed, releases and frame callbacks flowing — but the
     * server reads the WRONG pages, so whole paint cycles silently
     * never appear on screen (the demo bar's hover repaint being the
     * live report). Flushing HERE, while fd is still open, guarantees
     * the request that carries it goes out referring to THIS memfd.
     * EAGAIN (socket full) is retried by the next flush — but then fd
     * would close into a queued request again, so keep retrying inline
     * a bounded number of times; on persistent failure the pool is
     * unusable anyway and the caller degrades honestly. */
    {
        int tries = 0;
        while (wl_display_flush(conn->display) < 0 && errno == EAGAIN &&
               tries < 64) {
            /* the send buffer is full because the compositor is behind;
             * poll for writability, bounded, then retry */
            struct pollfd pfd = {
                .fd = wl_display_get_fd(conn->display),
                .events = POLLOUT,
            };
            poll(&pfd, 1, 10);
            tries++;
        }
    }
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

/* Defined below (GPU lane path, 1.2.0 / D-1): the EGL lane's
 * permanent downgrade + salvage. The deferred-first-frame flush above
 * needs it before its definition point. */
fdk_result fdk__wayland_surface_downgrade_from_lane(
    fdk_platform_window *pwindow);

/* THE deferred-first-frame commit, shared by xdg_surface_configure()
 * and the extension path (wayland_ext.c's resized op — the add-on's
 * role configure). Mirrors the 1.1.0 fix: an application that painted
 * BEFORE the first configure had its first present deferred (the
 * protocol forbids committing content before the ack) with the damage
 * already consumed — and every later present was a no-op against a
 * clean tree, so the pending buffer was never committed and the
 * surface never mapped. The commit the deferred present waits for
 * happens HERE, after the ack:
 *   - pending buffer still at the configured size -> commit as-is
 *     (full damage; the painted content is valid)
 *   - configure proposed a different size -> drop the stale pending
 *     at the next acquisition; dispatch EXPOSE so the tree
 *     re-invalidates and the app's next paint redraws and presents at
 *     the real size (same re-drive the X11 backend's ExposureMask
 *     provides on first map).
 * Returns 1 when the pending frame was committed, 0 otherwise. */
int fdk__wayland_commit_deferred_first_frame(
    fdk_platform_window *pwindow) {
    if (pwindow == NULL ||
        pwindow->render_pending == NULL || pwindow->rendered_ever) {
        /* The GPU lane's deferred frame rides a different flag: the
         * lane path never sets render_pending. A lane present that
         * returned before the configure left its drawn pixels in the
         * STAGING buffer; flush them here (full damage) so a
         * single-present application still maps with its first
         * frame — the exact contract the CPU branch below gives a
         * deferred render_pending. */
        if (pwindow != NULL && pwindow->egl_win != NULL &&
            pwindow->lane_frame_deferred && !pwindow->rendered_ever) {
            pwindow->lane_frame_deferred = 0;
            fdk_platform_damage full;
            memset(&full, 0, sizeof(full));
            full.full = 1;
            if (fdk_wayland_egl_window_present(
                    pwindow->egl_win, &full,
                    pwindow->render_size.width,
                    pwindow->render_size.height) == 0) {
                pwindow->rendered_ever = 1;
                FDK_DEBUG("deferred EGL first frame committed at "
                          "configure");
                return 1;
            }
            return fdk__wayland_surface_downgrade_from_lane(pwindow) ==
                   FDK_OK;
        }
        return 0;
    }
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
        if (pwindow->render_slots[i].buffer == pwindow->render_pending) {
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
        return 1;
    }
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_WINDOW_EXPOSE;
    pwindow->conn->dispatch(pwindow, &event,
                            pwindow->conn->dispatch_user_data);
    FDK_DEBUG("configure resized past the deferred frame; "
              "EXPOSE re-drives the first paint");
    return 0;
}

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
     * waiting for happens HERE, now that ack_configure has been sent
     * (see fdk__wayland_commit_deferred_first_frame — shared with
     * the extension path, which hits the identical bug through the
     * add-on's role configure). */
    (void)fdk__wayland_commit_deferred_first_frame(pwindow);

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

    /* 1.3.2 — first-map EXPOSE: X11 parity for toolkit-owned popups.
     *
     * The X server sends Expose when a window maps, and THAT event is
     * what drives the auto-paint of menu/combo/tooltip popups whose
     * content damage has no input event of its own. Wayland has no
     * map event: the first xdg_surface.configure is the equivalent
     * moment. Without this dispatch, an input-TRANSPARENT popup (a
     * tooltip — the empty input region means it will NEVER receive
     * pointer events) stayed on its committed background buffer
     * forever: the canvas damage sat unpainted behind a dark box
     * (found live in the sway tooltip rig). Menus dodged the bug by
     * accident — their grab funnels a stream of pointer events into
     * the popup, and every dispatch tail re-ran the auto-paint. App
     * toplevels are unaffected either way: they render from their
     * own loop, and an EXPOSE they may additionally receive is the
     * documented contract fdk_event.h already describes. */
    if (!was_configured) {
        fdk_event_data event;
        memset(&event, 0, sizeof(event));
        event.type = FDK_EVENT_WINDOW_EXPOSE;
        pwindow->conn->dispatch(pwindow, &event,
                                pwindow->conn->dispatch_user_data);
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
    /* 1.4.10: fullscreen flows INTO update_state as a parameter —
     * the handler must NOT pre-write pwindow->fullscreen, or the
     * helper's change detection goes tautological for fullscreen-
     * only flips (found live by the sway suite: the unset configure
     * arrived, the flag never cleared, no event fired). update_state
     * owns the write; the resize-gate reads below run AFTER it. */
    int minimized = pwindow->minimized;
    if (activated && minimized) {
        minimized = 0;
    }
    fdk_wayland_window_update_state(pwindow, maximized, minimized,
                                    fullscreen);
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
    /* X3 (1.1.0-rc1): the compositor's dismissal carries its reason —
     * the single-slot channel window.c's dispatcher consumes when the
     * close-request lands on the popup (the trace then says
     * "compositor dismissal (outside click or focus loss)" instead of
     * "(unknown)"). */
    fdk__window_note_popup_close(FDK_POPUP_CLOSE_COMPOSITOR);
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

/* Non-static (1.1.0): wayland_ext.c's adoption path attaches the same
 * output-tracking listener to adopted extension surfaces — enter/leave
 * keeps the HiDPI scale machinery alive for them exactly like created
 * windows. */
const struct wl_surface_listener g_wl_surface_listener = {
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

/* Non-static (1.1.0): wayland_ext.c attaches the same preferred-scale
 * listener to adopted extension surfaces. */
const struct wp_fractional_scale_v1_listener
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
    /* 1.3.2: leave the pending set — the entry dies with the proxy
     * right below, and the window may already be gone (the zombie
     * path dispatched a queued release after window destroy). */
    for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
        if (pwindow->bg_pending[i] == buffer) {
            /* NOTE: unreachable once window destroy reaped the set —
             * it destroyed the proxies, and libwayland drops events
             * for destroyed objects. Mid-session releases land here. */
            pwindow->bg_pending[i] = NULL;
            break;
        }
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
    /* 1.3.2: track this buffer in the compositor-held set. A
     * REPLACED buffer stays tracked until its own wl_buffer::release
     * destroys it (the listener removes the entry) — destroying it
     * here instead raced the listener into a double destroy (found
     * live under ASan: the queued release still dispatched). */
    {
        int slot = -1;
        int oldest = -1;
        for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
            if (pwindow->bg_pending[i] == NULL && slot < 0) {
                slot = i;
            }
            if (pwindow->bg_pending[i] != NULL) {
                oldest = i; /* last non-empty = oldest (append order) */
            }
        }
        if (slot < 0) {
            /* Configure storm beyond the cap: destroy the oldest
             * pending proxy — its late release event is dropped by
             * libwayland's destroyed-object handling. */
            if (oldest >= 0) {
                FDK_WARN("wayland: background-buffer pending set "
                         "overflow; destroying the oldest unreleased "
                         "buffer");
                wl_buffer_destroy(pwindow->bg_pending[oldest]);
                if (pwindow->buffer == pwindow->bg_pending[oldest]) {
                    pwindow->buffer = NULL;
                    pwindow->buffer_attached = 0;
                }
                slot = oldest;
            } else {
                slot = 0; /* unreachable: cap > 0 */
            }
        }
        pwindow->bg_pending[slot] = buffer;
    }

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

/* Input region (1.1.0-rc2, S-4 / A-06): wl_surface.set_input_region
 * with a wl_compositor region built from the LOGICAL rects — the
 * protocol's surface-local space is exactly FDK's logical space (the
 * one pointer events deliver), so no scale conversion happens here
 * (unlike X11's physical XShape). The region OBJECT is destroyed
 * immediately: the request copies it into double-buffered surface
 * state. Committed at once when the surface is already configured
 * (so a region change takes effect without waiting for the app's
 * next present); a not-yet-configured surface carries the state on
 * its FIRST commit — committing here would be the
 * unconfigured-surface protocol error the ext-popup gate prevents. */
void fdk_wayland_window_set_input_region(fdk_platform_window *pwindow,
                                         const fdk_rect *rects,
                                         int count) {
    if (pwindow == NULL || pwindow->surface == NULL || count < 0 ||
        count > FDK_PLATFORM_INPUT_REGION_MAX) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (count > 0 && rects != NULL) {
        struct wl_region *region =
            wl_compositor_create_region(conn->compositor);
        if (region == NULL) {
            return; /* OOM: the previous region stays — honest */
        }
        for (int i = 0; i < count; i++) {
            wl_region_add(region, rects[i].x, rects[i].y,
                          rects[i].width, rects[i].height);
        }
        wl_surface_set_input_region(pwindow->surface, region);
        wl_region_destroy(region);
    } else {
        /* The clear: a NULL region is the protocol's "whole
         * surface" — full input again. */
        wl_surface_set_input_region(pwindow->surface, NULL);
    }
    if (pwindow->configured) {
        wl_surface_commit(pwindow->surface);
    }
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

    /* --- GPU present lane (1.2.0 / D-1, wayland_egl.c) ----------------
     *
     * The EGL mirror of x11_surface.c's engagement block, same
     * decisions: engage once per window (never after a downgrade),
     * run the SMALL-SURFACE policy BEFORE any dlopen (a process whose
     * windows are all small never loads the GL stack at all), draw
     * into the lane's STAGING buffer instead of a wl_shm slot, and
     * treat staging allocation failure as the classic path's own OOM.
     * FDK_GL_FORCE overrides the size gate exactly as on X11. */
    if (pwindow->egl_win == NULL && !pwindow->egl_downgraded) {
        int lane_eligible =
            ((fdk_i64)size.width * (fdk_i64)size.height >
                 FDK_WL_GL_MIN_PIXELS ||
             getenv("FDK_GL_FORCE") != NULL);
        if (!lane_eligible) {
            /* Sticky per window (the whole panel/popup class — same
             * rationale as x11_surface.c's copy of this policy); one
             * INFO per process. */
            pwindow->egl_downgraded = 1;
            static int small_policy_logged = 0;
            if (!small_policy_logged) {
                small_policy_logged = 1;
                FDK_INFO("GL lane: small surfaces (<= %d px) present on "
                         "the CPU path (FDK_GL_FORCE=1 overrides)",
                         FDK_WL_GL_MIN_PIXELS);
            }
        } else {
            if (pwindow->conn->egl == NULL && !pwindow->conn->egl_tried) {
                pwindow->conn->egl = fdk_wayland_egl_try_load(pwindow->conn);
                pwindow->conn->egl_tried = 1;
            }
            if (pwindow->conn->egl != NULL) {
                /* render_size feeds the lane's sizing (wl_egl_window
                 * needs the size at CREATION time; the SHM path derives
                 * sizes from the slots themselves). */
                pwindow->render_size = size;
                pwindow->egl_win = fdk_wayland_egl_window_try_init(
                    pwindow->conn->egl, pwindow);
            }
        }
    }
    if (pwindow->egl_win != NULL) {
        fdk_u32 *staging = fdk_wayland_egl_window_staging(
            pwindow->egl_win, size.width, size.height);
        if (staging != NULL) {
            pwindow->render_size = size;
            out_fb->pixels = staging;
            out_fb->width = size.width;
            out_fb->height = size.height;
            out_fb->stride = size.width;
            return FDK_OK;
        }
        FDK_WARN("GPU lane staging allocation failed — reverting to the "
                 "CPU present path");
        fdk_wayland_egl_window_destroy(pwindow->conn->egl, pwindow->egl_win);
        pwindow->egl_win = NULL;
        pwindow->egl_downgraded = 1;
    }

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
             *
             * 1.1.0-rc1 rework (the W1 report): the old wait used
             * wl_display_prepare_read() — the DEFAULT-queue variant,
             * which REFUSES while any default-queue event is pending.
             * Under heavy pointer motion that is always the case
             * (every motion is a default-queue event), so the wait
             * fell into its usleep(1000) fallback and degenerated
             * into a pure 100ms sleep: releases sitting on the socket
             * were never read, the wait always failed, every refused
             * acquire burned 100ms, the dispatch rate collapsed to
             * ~10 events/s against ~1000/s arriving, the backlog
             * grew without bound, and on wlroots the server's 4096-byte
             * per-client buffer overflows and the compositor kills the
             * connection outright ("Data too big for buffer").
             *
             * The fix is the QUEUE-SCOPED variant:
             * wl_display_prepare_read_queue(display, release_queue)
             * only fails when the RELEASE queue itself has pending
             * events — which we then dispatch (progress!), never
             * sleep. On success, poll+read_events reads EVERYTHING
             * waiting on the socket into its target queues: release
             * events land in the release queue (dispatched below),
             * while default-queue events (motions, configures) are
             * BUFFERED un-run for the outer loop — reading is safe
             * from inside a listener precisely because no listener
             * runs here. The event stream never stalls, so a release
             * that is already on the wire arrives within one poll
             * slice. */
            int remaining = (int)(wait_deadline - now_ms());
            if (remaining > 10) {
                remaining = 10;
            }
            if (wl_display_prepare_read_queue(
                    pwindow->conn->display,
                    pwindow->conn->release_queue) == 0) {
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
                /* The RELEASE queue has events waiting (the only
                 * reason the queue-scoped prepare refuses): dispatch
                 * them right now — one of them may be the release this
                 * wait exists for — and loop immediately. The 1.1.6
                 * code slept a millisecond here instead, which under a
                 * busy default queue turned the whole budget into a
                 * fixed sleep with zero events ever read (the W1
                 * freeze). */
                (void)fdk_wayland_release_queue_dispatch(pwindow->conn);
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
    /* Frame callback for pacing: fires when the compositor has
     * presented this commit (see frame_callback_done). Tracked in
     * pwindow->frame_cb so window_destroy can reap one that never
     * fires.
     *
     * ORDER (1.1.0-rc1, found live on sway/labwc headless rigs): the
     * frame request MUST be marshalled BEFORE the commit it paces.
     * wl_surface.frame's contract attaches the callback to the commit
     * it is submitted with; requesting it after the commit attaches it
     * to the NEXT frame cycle instead — and on wlroots the pending
     * callback is part of what schedules the output's repaint, so a
     * commit whose callback trails it can leave the compositor with
     * damage but no scheduled frame: the commit is processed (the old
     * buffer is even released), yet the output never re-renders and
     * wlr-screencopy keeps photographing the stale frame. Every
     * reference client (weston-simple-shm, GTK, Qt) requests frame
     * first; now FDK does too. */
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
    wl_surface_commit(pwindow->surface);

    pwindow->rendered_ever = 1;
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

    /* Flush after every commit (1.1.0-rc1, the W1 checklist): the
     * pump's dispatch tail flushes eventually, but a present from an
     * application paint loop (the demo bar, between pumps) would
     * otherwise sit in libwayland's output buffer until the next pump
     * pass — a full pump cadence of latency (100ms in the bar's loop)
     * before the compositor can even begin to present, frame, or
     * release. EPIPE here means the compositor is gone; the dispatch
     * layer's circuit breaker owns the reporting, so this is a quiet
     * mark-dead. EAGAIN (send buffer full) is not an error: the
     * request stays queued and rides the next flush. */
    if (wl_display_flush(pwindow->conn->display) < 0 && errno != EAGAIN) {
        pwindow->conn->display_dead = 1;
    }
    return FDK_OK;
}

fdk_result fdk_wayland_window_present(fdk_platform_window *pwindow,
                                      const fdk_platform_damage *damage) {
    if (pwindow == NULL || damage == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* --- GPU present lane (1.2.0 / D-1) ------------------------------
     *
     * Fast path: upload the damaged rects into the window's GL
     * texture, composite the fullscreen quad, eglSwapBuffers (the
     * swap's internal attach+commit IS the wl_surface commit —
     * libwayland-egl does it). A nonzero return is the lane's
     * PERMANENT downgrade verdict — handled by the shared downgrade
     * helper (salvage + wl_shm re-present; the window never shows a
     * broken or stale frame because of the downgrade itself).
     *
     * render_pending stays NULL on the lane path (no wl_shm buffer
     * was acquired), so the classic no-op checks below do not apply;
     * the "nothing changed" no-op DOES — a no-damage present through
     * the lane is equally free to skip. */
    if (pwindow->egl_win != NULL) {
        if (!damage->full && damage->count == 0) {
            return FDK_OK;
        }
        if (!pwindow->configured) {
            /* The xdg-shell first-commit gate applies to EGL buffers
             * exactly as it does to wl_shm ones (a commit is a
             * commit); defer to the configure path. The lane's
             * staging buffer holds the frame; the configure handler
             * flushes it (lane_frame_deferred — the symmetric branch
             * of the CPU path's deferred render_pending commit). */
            FDK_DEBUG("EGL present deferred until first configure");
            pwindow->lane_frame_deferred = 1;
            return FDK_OK;
        }
        fdk_i32 gw = pwindow->render_size.width;
        fdk_i32 gh = pwindow->render_size.height;
        if (fdk_wayland_egl_window_present(pwindow->egl_win, damage, gw,
                                           gh) == 0) {
            pwindow->rendered_ever = 1;
            return FDK_OK;
        }
        return fdk__wayland_surface_downgrade_from_lane(pwindow);
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
         * forever: with the tree clean it would be a no-op).
         *
         * EXTENSION surfaces (1.1.0) ride the SAME gate (1.1.0-rc1
         * fix, found live on labwc/sway 2026-10-26: "zwlr_layer_
         * surface_v1: error 2: layer_surface has never been
         * configured" killed the bar the moment it painted before
         * its role's first configure): their configured flag flips
         * in wayland_ext.c's resized op — the add-on reports its
         * role's configure through fdk_ext_surface_notify_configure()
         * AFTER acking it, and that op commits the deferred first
         * frame exactly like xdg_surface_configure does here. The
         * old design left the commit discipline to each add-on; one
         * that painted eagerly (the demo bar, right after show())
         * attached a buffer pre-configure and took down the whole
         * connection. The gate is the library's to own: no surface
         * FDK renders commits content before its role is configured. */
        FDK_DEBUG("present deferred until first configure (xdg or role)");
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

/* The render_path op (1.2.0 / D-1): 1 while this window presents
 * through the EGL lane, 0 on the classic wl_shm path (including
 * never-engaged and downgraded windows — the honest, current
 * answer, read live off the backend's own state). The exact mirror
 * of fdk_x11_window_render_path. */
int fdk_wayland_window_render_path(fdk_platform_window *pwindow) {
    return (pwindow != NULL && pwindow->egl_win != NULL) ? 1 : 0;
}

/* The GPU-lane downgrade path (1.2.0 / D-1) — the mirror of
 * fdk__x11_surface_downgrade_from_lane: salvage the staging frame
 * into a fresh wl_shm slot, destroy the lane state, mark the window
 * downgraded (sticky), and re-present through the classic path with
 * full damage so the salvaged frame goes out NOW. The staging
 * buffer holds exactly (last-presented + the drawing since), so the
 * window never shows a broken or stale frame because of the
 * downgrade itself. */
fdk_result fdk__wayland_surface_downgrade_from_lane(
    fdk_platform_window *pwindow) {
    if (pwindow == NULL || pwindow->egl_win == NULL) {
        return FDK_OK;
    }
    fdk_platform_connection *conn = pwindow->conn;
    fdk_i32 gw = pwindow->render_size.width;
    fdk_i32 gh = pwindow->render_size.height;

    FDK_WARN("GPU present lane failed — reverting this window to "
             "the CPU present path (permanent)");

    /* A classic slot at the current size, pre-filled from the
     * staging buffer: the same shape prefetch_visible_frame gives a
     * recycled slot, built directly because no render slot exists on
     * the lane path (render_pending was never set). */
    const fdk_u32 *src =
        fdk_wayland_egl_window_staging(pwindow->egl_win, gw, gh);
    if (src == NULL || gw < 1 || gh < 1) {
        /* No staging to salvage (degenerate sizes): plain teardown,
         * full-damage classic present, done. */
        fdk_wayland_egl_window_destroy(conn->egl, pwindow->egl_win);
        pwindow->egl_win = NULL;
        pwindow->egl_downgraded = 1;
        static const fdk_platform_damage fallback_full = {
            {{0, 0, 0, 0}}, 0, 1
        };
        return fdk_wayland_window_present(pwindow, &fallback_full);
    }

    /* Find (or make) a slot at this size. The classic path's
     * acquisition owns slot bookkeeping; the direct form here
     * reuses its primitives: take an empty slot, create the buffer,
     * copy the frame, set it pending. A full acquisition path runs
     * when the next present happens anyway — this present must go
     * out through the classic path NOW. */
    int slot = -1;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        if (pwindow->render_slots[i].buffer == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Reap an unreleased wrong-size slot (the destroy-order
         * argument: the lane path never allocated wl_shm slots, so
         * all four are either free or wrong-size strays from before
         * the lane engaged). */
        for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
            if (pwindow->render_slots[i].buffer != NULL &&
                pwindow->render_slots[i].buffer != pwindow->buffer) {
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
    }
    fdk_result rc = FDK_OK;
    if (slot >= 0) {
        struct wl_buffer *buffer = NULL;
        uint32_t *pixels = NULL;
        size_t length = 0;
        fdk_size size = { .width = gw, .height = gh };
        fdk_result r = create_shm_buffer(conn, size, &buffer, &pixels,
                                         &length);
        if (fdk_ok(r)) {
            wl_buffer_add_listener(buffer, &g_render_buffer_listener,
                                   pwindow);
            pwindow->render_slots[slot].buffer = buffer;
            pwindow->render_slots[slot].pixels = pixels;
            pwindow->render_slots[slot].length = length;
            pwindow->render_slots[slot].size = size;
            pwindow->render_slots[slot].released = 0;
            memcpy(pixels, src, sizeof(fdk_u32) * (size_t)gw * (size_t)gh);
            pwindow->render_pending = buffer;
        } else {
            rc = r; /* no classic slot: the damage stays set, the next
                     * present retries (acquisition's own story) */
        }
    }

    fdk_wayland_egl_window_destroy(conn->egl, pwindow->egl_win);
    pwindow->egl_win = NULL;
    pwindow->egl_downgraded = 1;

    if (!fdk_ok(rc)) {
        return rc;
    }

    /* The salvaged frame goes out through the classic path NOW, full
     * damage — by recursing into the (egl_win == NULL) present,
     * which owns the configure gate, damage conversion, the frame
     * callback, and live-buffer bookkeeping. */
    static const fdk_platform_damage fallback_full = {
        {{0, 0, 0, 0}}, 0, 1
    };
    return fdk_wayland_window_present(pwindow, &fallback_full);
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
    pwindow->toplevel_icon = NULL;          /* M-4: set_icon fills */
    pwindow->toplevel_icon_buffer = NULL;
    pwindow->toplevel_icon_buffer_size.width = 0;
    pwindow->toplevel_icon_buffer_size.height = 0;
    pwindow->icon_absent_logged = 0;
    pwindow->buffer = NULL;
    pwindow->buffer_size.width = 0;
    pwindow->buffer_size.height = 0;
    pwindow->buffer_attached = 0;
    pwindow->cursor_shape = 0; /* default arrow until shaped (W2) */
    for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
        pwindow->bg_pending[i] = NULL; /* 1.3.2: fdk_alloc not zeroed */
    }
    pwindow->render_pending = NULL;
    pwindow->rendered_ever = 0;
    pwindow->last_refuse_warn_ms = 0;
    /* GPU lane (1.2.0 / D-1): engaged at the first framebuffer
     * acquisition, never here (fdk_alloc does not zero). */
    pwindow->egl_win = NULL;
    pwindow->egl_downgraded = 0;
    pwindow->render_size.width = 0;
    pwindow->render_size.height = 0;
    pwindow->lane_frame_deferred = 0;
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
    pwindow->input_transparent =
        (options != NULL && options->input_transparent != 0);
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
         * sweep guarantees that order.
         *
         * 1.1.0 — EXTENSION-surface parents: the parent's wl_surface
         * carries the ADD-ON's role (a layer surface), not an
         * xdg_surface, so the xdg_popup is created with NO xdg
         * parent and the kind's popup_attached hook hands the popup
         * proxy to the add-on, which parents it to its role object
         * (zwlr_layer_surface_v1.get_popup — the waybar pattern).
         * The positioner's parent-relative coordinates apply against
         * the layer surface once attached. */
        if (parent == NULL) {
            FDK_ERROR("popup window needs a toplevel or popup parent");
            xdg_surface_destroy(pwindow->xdg_surface);
            wl_surface_destroy(pwindow->surface);
            fdk_free(pwindow);
            return FDK_ERR_INVALID_ARGUMENT;
        }
        const int parent_is_ext = (parent->ext != 0);
        if (!parent_is_ext && parent->xdg_toplevel == NULL &&
            parent->xdg_popup == NULL) {
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
        /* EXT parents carry no xdg_surface (the add-on's role owns
         * the surface): the popup starts xdg-parent-less and the
         * kind's popup_attached hook parents it to the role. */
        pwindow->xdg_popup = xdg_surface_get_popup(
            pwindow->xdg_surface,
            parent_is_ext ? NULL : parent->xdg_surface, pos);
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
         * rig's stderr); skip it cleanly instead.
         *
         * Input-TRANSPARENT popups (tooltips) skip the grab on
         * principle, not for want of a seat: a grabbed popup owns
         * the keyboard and dismisses on outside input — the exact
         * opposite of a hint that must never disturb the app it
         * explains. Their empty input region (below) makes the
         * compositor route pointer events to whatever is under
         * them. */
        if (conn->seat != NULL && !pwindow->input_transparent) {
            xdg_popup_grab(pwindow->xdg_popup, conn->seat,
                           conn->last_input_serial);
        }
        if (parent_is_ext) {
            /* Hand the fresh popup proxy to the add-on's kind so its
             * ROLE can adopt it (the zwlr_layer_surface_v1.get_popup
             * pattern). Window-layer hook (src/window/extend.c) —
             * silent no-op when the kind has no popup_attached
             * callback, in which case the popup stands on its grab
             * alone. ctx rides along as the connect()-time
             * dispatch_user_data (the one established bridge from a
             * connection back to the core — see platform_internal.h). */
            fdk__ext_popup_attached((fdk_context *)conn->dispatch_user_data,
                                    parent, pwindow->xdg_popup);
        }
        if (pwindow->input_transparent) {
            /* Click-through from the FIRST frame: input regions are
             * double-buffered surface state applied at the next
             * commit — this create path precedes any commit, so the
             * popup maps click-through. The region object may be
             * destroyed immediately (the surface keeps its own
             * reference). */
            if (conn->compositor != NULL) {
                struct wl_region *empty =
                    wl_compositor_create_region(conn->compositor);
                if (empty != NULL) {
                    wl_surface_set_input_region(pwindow->surface, empty);
                    wl_region_destroy(empty);
                } /* OOM on a 4-byte object: skip, degrade honestly */
            }
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
    /* GPU lane first (1.2.0 / D-1): the EGL surface wraps this
     * wl_surface, so it must die BEFORE wl_surface_destroy below —
     * the lane's own destroy path (context, surface, wl_egl_window,
     * staging) runs against a still-live surface. */
    if (pwindow->egl_win != NULL) {
        fdk_wayland_egl_window_destroy(pwindow->conn->egl, pwindow->egl_win);
        pwindow->egl_win = NULL;
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

    /* 1.3.2: reap every background buffer the compositor may still
     * hold — the current one (`buffer`, unless the render slots
     * above already consumed it) AND any replaced-but-unreleased
     * ones. Their proxies die here; any release event still queued
     * client-side for them is dropped by libwayland's
     * destroyed-object handling, so the release listener can never
     * run against this (about-to-be-freed) window. This closes the
     * use-after-free the clipboard interop rig hit when popup
     * content began presenting over the background fill. */
    for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
        if (pwindow->bg_pending[i] != NULL) {
            if (pwindow->buffer == pwindow->bg_pending[i]) {
                pwindow->buffer = NULL;
                pwindow->buffer_attached = 0;
            }
            wl_buffer_destroy(pwindow->bg_pending[i]);
            pwindow->bg_pending[i] = NULL;
        }
    }
    if (pwindow->buffer != NULL) {
        /* A non-background survivor (should not happen: render
         * buffers were consumed above) — still drop it rather than
         * leak the proxy. */
        wl_buffer_destroy(pwindow->buffer);
        pwindow->buffer = NULL;
        pwindow->buffer_attached = 0;
    }
    /* Toplevel icon pair (M-4): both die here, icon first (the
     * buffer must not outlive it), proxies only — the mapping is
     * already gone (munmap'd at set_icon). */
    if (pwindow->toplevel_icon != NULL) {
        xdg_toplevel_icon_v1_destroy(pwindow->toplevel_icon);
        pwindow->toplevel_icon = NULL;
    }
    if (pwindow->idle_inhibitor != NULL) {
        /* A window dying with the display still inhibited releases
         * it (the inhibitor proxy is surface-scoped and would be a
         * protocol error to keep after the surface dies). */
        zwp_idle_inhibitor_v1_destroy(pwindow->idle_inhibitor);
        pwindow->idle_inhibitor = NULL;
    }
    if (pwindow->toplevel_icon_buffer != NULL) {
        wl_buffer_destroy(pwindow->toplevel_icon_buffer);
        pwindow->toplevel_icon_buffer = NULL;
    }
    fdk_wayland_unregister_window(pwindow->conn, pwindow);
    /* IME focus guard (M-1): a window dying mid-composition leaves
     * the text-input's latched batch pointing at freed memory — the
     * IME module drops it (and marks the session stale for the next
     * enter's self-heal). */
    fdk_wayland_ime_window_destroyed(pwindow->conn, pwindow);
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
    if (pwindow->xdg_surface != NULL) {
        /* NULL for EXTENSION surfaces (1.1.0): an adopted surface's
         * role belongs to the add-on and was torn down by the kind's
         * destroy callback BEFORE this — there is no xdg object here
         * to destroy (found live by the fdk-shell sway rig: the NULL
         * proxy marshaled through wl_proxy_get_version and killed
         * the process). */
        xdg_surface_destroy(pwindow->xdg_surface);
        pwindow->xdg_surface = NULL;
    }
    wl_surface_destroy(pwindow->surface);
    fdk_free(pwindow);
}

void fdk_wayland_window_show(fdk_platform_window *pwindow) {
    /* Extension surfaces (1.1.0): no commit here. A role-carrying
     * surface (layer-shell above all) maps when its own protocol
     * handshake completes — committing before the role's initial
     * configure is a wlroots protocol ERROR that kills the whole
     * connection. fdk_extend.h documents show() as a Wayland no-op
     * for extension surfaces: the add-on's handshake + first present
     * do the mapping.
     *
     * EXTENSION POPUPS are the exception: they carry an FDK-OWNED
     * xdg role (created through the normal popup path), so the
     * initial empty commit below IS their handshake starter — the
     * gate keys on the popup flag, not ext alone (found live by the
     * fdk-shell sway rig: the popup's configure never arrived). */
    if (pwindow->ext && !pwindow->popup) {
        return;
    }
    /* xdg-shell requires an initial "commit with no buffer" to
     * trigger the first configure, then the client must wait for
     * that configure before attaching any actual content. The first
     * real buffer is therefore committed from xdg_surface_configure()
     * — see attach_background_buffer(). This commit just starts the
     * handshake; the window becomes visible a few events later. */
    wl_surface_commit(pwindow->surface);
}

void fdk_wayland_window_hide(fdk_platform_window *pwindow) {
    /* Extension surfaces (1.1.0): no NULL-buffer commit — unmapping
     * a role-carrying surface is the ROLE's operation (a layer
     * surface unmaps by destroying and re-creating its role object,
     * the add-on's business), not a surface-state commit. Symmetric
     * with show()'s no-op above and documented in fdk_extend.h. */
    if (pwindow->ext) {
        return;
    }
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
    /* Popups have no xdg_toplevel (their role object is xdg_popup) —
     * marshaling a toplevel request on the NULL proxy is a crash in
     * the generated inline (wl_proxy_get_version(NULL)). A popup's
     * "title" has no protocol meaning anyway: quietly ignore, the
     * same answer X11 gives override-redirect windows. (1.3.0: the
     * v1 code NULL-deref'd here — six public entry points shared
     * the bug, found by the production audit.) */
    if (pwindow == NULL || pwindow->xdg_toplevel == NULL) {
        return;
    }
    xdg_toplevel_set_title(pwindow->xdg_toplevel, title != NULL ? title : "");
}

/* Toplevel icon (1.1.0, M-4) — xdg-toplevel-icon-v1.
 *
 * The protocol's shape drives this implementation:
 *   - the buffer MUST be wl_shm-backed and SQUARE; a non-square
 *     source is centered on a transparent square (the letterbox an
 *     icon deserves, not a stretch);
 *   - add_buffer freezes the pool region — the buffer is written
 *     ONCE and never touched again, destroyed only with the icon;
 *   - state is double-buffered: applied at the next surface commit,
 *     so the icon lands together with whatever the window paints
 *     next (a window that never paints still gets its icon at the
 *     explicit commit this function sends — committing an empty
 *     surface state is legal and how icon-only updates are done);
 *   - compositor absence is the COMMON case today (wlroots < 0.19,
 *     GNOME): one INFO line, then a permanent quiet no-op per
 *     window (the warn-once stamp below).
 */
/* Idle inhibition (1.1.0, S-3): the per-surface inhibitor, engaged
 * and released by the idle module through the ops table. The proxy
 * rides the pwindow (the window destroy path releases a live one);
 * the inhibitor object itself carries NO events — the protocol is
 * create-to-inhibit, destroy-to-release. */
fdk_result fdk_wayland_window_set_idle_inhibit(
    fdk_platform_window *pwindow, int inhibit) {
    if (pwindow == NULL || pwindow->conn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->conn->idle_inhibit_manager == NULL) {
        return FDK_ERR_UNSUPPORTED; /* the D-Bus fallback engages */
    }
    if (inhibit) {
        if (pwindow->idle_inhibitor != NULL) {
            return FDK_OK; /* idempotent engage */
        }
        if (pwindow->surface == NULL) {
            return FDK_ERR_INVALID_STATE;
        }
        pwindow->idle_inhibitor =
            zwp_idle_inhibit_manager_v1_create_inhibitor(
                pwindow->conn->idle_inhibit_manager, pwindow->surface);
        return pwindow->idle_inhibitor != NULL ? FDK_OK
                                               : FDK_ERR_OUT_OF_MEMORY;
    }
    if (pwindow->idle_inhibitor != NULL) {
        zwp_idle_inhibitor_v1_destroy(pwindow->idle_inhibitor);
        pwindow->idle_inhibitor = NULL;
    }
    return FDK_OK; /* release without engagement: idempotent */
}

/* ---- xdg-activation (1.1.0, S-6) ------------------------------------------
 *
 * The focus-stealing-prevention protocol, both halves. The token
 * mint is the interesting part: the compositor answers commit()
 * with an async done(token) event, and the caller may be sitting
 * INSIDE an event handler (the launcher button that spawns a
 * child) — a plain wl_display_roundtrip would dispatch the main
 * queue and re-enter FDK's own handlers. So the token proxy is
 * moved to a PRIVATE event queue and caught with
 * wl_display_roundtrip_queue: the wait sees exactly one event —
 * the token's own done — and nothing else in the connection is
 * disturbed (the buffer-release-queue pattern, applied to a
 * protocol object for the first time).
 *
 * The requests attach everything the compositor weighs when
 * deciding whether a later activate() is honest: the serial of the
 * last user input FDK saw (set_serial — wlroots compositors refuse
 * tokens with no recent serial, which is exactly the point of the
 * protocol), the app_id (launch feedback identity), and the
 * REQUESTING surface (set_surface — this window vouches). */
typedef struct {
    char *token;
    int done;
} activation_wait;

static void activation_token_done(void *data,
                                  struct xdg_activation_token_v1 *tok,
                                  const char *token) {
    (void)tok;
    activation_wait *w = data;
    if (token != NULL && token[0] != '\0') {
        size_t len = strlen(token) + 1;
        w->token = fdk_alloc(len);
        if (w->token != NULL) {
            memcpy(w->token, token, len);
        }
    }
    w->done = 1;
}

static const struct xdg_activation_token_v1_listener
    activation_token_listener = {
        .done = activation_token_done,
};

/* Mints a compositor token bound to this window's surface. Returns
 * a heap copy (fdk_free by the caller) or NULL when the compositor
 * is absent, the window has no surface yet, or the exchange fails
 * (display error / done never arrived within the roundtrip — the
 * sync the roundtrip sends AFTER commit forces done out first, so
 * a live compositor always answers). */
static char *mint_activation_token(fdk_platform_window *pwindow) {
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->activation == NULL || pwindow->surface == NULL) {
        return NULL;
    }
    struct xdg_activation_token_v1 *tok =
        xdg_activation_v1_get_activation_token(conn->activation);
    if (tok == NULL) {
        return NULL;
    }
    struct wl_event_queue *q = wl_display_create_queue(conn->display);
    if (q == NULL) {
        xdg_activation_token_v1_destroy(tok);
        return NULL;
    }
    /* Route the token's events to the private queue: done will be
     * dispatched by roundtrip_queue below, never the main pump. */
    wl_proxy_set_queue((struct wl_proxy *)tok, q);

    activation_wait w;
    memset(&w, 0, sizeof w);
    xdg_activation_token_v1_add_listener(tok, &activation_token_listener,
                                         &w);
    if (conn->seat != NULL) {
        xdg_activation_token_v1_set_serial(
            tok, conn->last_input_serial, conn->seat);
    }
    if (conn->app_id != NULL && conn->app_id[0] != '\0') {
        xdg_activation_token_v1_set_app_id(tok, conn->app_id);
    }
    xdg_activation_token_v1_set_surface(tok, pwindow->surface);
    xdg_activation_token_v1_commit(tok);

    int rc = wl_display_roundtrip_queue(conn->display, q);
    /* Destroy only after the done event has been dispatched: the
     * destructor does not invalidate the received string, but
     * ordering here costs nothing and reads as the protocol's own
     * lifecycle. */
    xdg_activation_token_v1_destroy(tok);
    wl_event_queue_destroy(q);
    if (rc < 0) {
        fdk_free(w.token);
        return NULL;
    }
    return w.done ? w.token : (fdk_free(w.token), (char *)NULL);
}

fdk_result fdk_wayland_window_activate(fdk_platform_window *pwindow) {
    if (pwindow == NULL || pwindow->conn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->activation == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (pwindow->surface == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    /* The launch voucher: whatever launched THIS process left a
     * token in the environment (captured at connect, one shot).
     * Spending it on the first activation is the entire launcher
     * half of the protocol — the compositor then knows the user
     * themselves asked for this window (they clicked the launcher)
     * and focus-stealing-prevention steps aside. */
    if (conn->launch_token != NULL) {
        xdg_activation_v1_activate(conn->activation, conn->launch_token,
                                   pwindow->surface);
        fdk_free(conn->launch_token);
        conn->launch_token = NULL;
        return FDK_OK;
    }
    /* No voucher: mint fresh. The serial attached at mint time is
     * the best self-vouch a client can make (recent user input in
     * THIS window); compositors that demand more will ignore the
     * activation — the honest refusal, never an error. */
    char *tok = mint_activation_token(pwindow);
    if (tok == NULL) {
        return FDK_ERR_PLATFORM;
    }
    xdg_activation_v1_activate(conn->activation, tok, pwindow->surface);
    fdk_free(tok);
    return FDK_OK;
}

fdk_result fdk_wayland_window_activation_token(
    fdk_platform_window *pwindow, char *out, size_t cap) {
    if (pwindow == NULL || out == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->conn == NULL || pwindow->conn->activation == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    char *tok = mint_activation_token(pwindow);
    if (tok == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (strlen(tok) + 1 > cap) {
        fdk_free(tok);
        return FDK_ERR_INVALID_ARGUMENT; /* caller's buffer too small */
    }
    memcpy(out, tok, strlen(tok) + 1);
    fdk_free(tok);
    return FDK_OK;
}

void fdk_wayland_window_set_icon(fdk_platform_window *pwindow,
                                 const fdk_u32 *pixels,
                                 int width, int height) {
    if (pwindow == NULL) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;

    /* Clear path (and the popup no-op): destroy the pair, reset the
     * toplevel's icon to its default, commit. Manager-less composit
     * -ors never bound anything, so clear is always safe. */
    if (pwindow->toplevel_icon != NULL) {
        xdg_toplevel_icon_v1_destroy(pwindow->toplevel_icon);
        pwindow->toplevel_icon = NULL;
    }
    if (pwindow->toplevel_icon_buffer != NULL) {
        wl_buffer_destroy(pwindow->toplevel_icon_buffer);
        pwindow->toplevel_icon_buffer = NULL;
    }
    if (pixels == NULL || width <= 0 || height <= 0) {
        if (conn->toplevel_icon_manager != NULL &&
            pwindow->xdg_toplevel != NULL) {
            xdg_toplevel_icon_manager_v1_set_icon(
                conn->toplevel_icon_manager, pwindow->xdg_toplevel, NULL);
            /* Reset state applies at the next commit — send one only
             * if the surface is past its first configure (a state-only
             * commit before that is protocol-legal but pointless: the
             * deferred first-frame commit carries it anyway). */
            if (pwindow->configured && pwindow->surface != NULL) {
                wl_surface_commit(pwindow->surface);
            }
        }
        return;
    }
    if (conn->toplevel_icon_manager == NULL ||
        pwindow->xdg_toplevel == NULL) {
        /* Popups genuinely have no toplevel to icon (same as title);
         * manager absence on a toplevel is the honest-unsupported
         * case — one INFO line per window, never per call. */
        if (pwindow->xdg_toplevel != NULL && !pwindow->icon_absent_logged) {
            FDK_INFO("compositor offers no xdg-toplevel-icon-v1: "
                     "fdk_window_set_icon is a no-op here");
            pwindow->icon_absent_logged = 1;
        }
        return;
    }

    /* Build the SQUARE ARGB8888 buffer (centered letterbox for a
     * non-square source; stride = side*4; FDK's pixel packing is
     * already the wire format). */
    int side = width > height ? width : height;
    size_t stride = (size_t)side * 4u;
    size_t length = stride * (size_t)side;
    int fd = memfd_create("fdk-toplevel-icon", MFD_CLOEXEC);
    if (fd < 0) {
        FDK_WARN("toplevel icon: memfd_create failed (%s)", strerror(errno));
        return;
    }
    if (ftruncate(fd, (off_t)length) < 0) {
        FDK_WARN("toplevel icon: ftruncate failed (%s)", strerror(errno));
        close(fd);
        return;
    }
    uint32_t *buf = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, 0);
    if (buf == MAP_FAILED) {
        FDK_WARN("toplevel icon: mmap failed (%s)", strerror(errno));
        close(fd);
        return;
    }
    struct wl_shm_pool *pool =
        wl_shm_create_pool(conn->shm, fd, (int32_t)length);
    /* FD-lifetime flush, same rule as create_shm_buffer: the fd must
     * survive until the create_pool request carrying it is ON THE
     * WIRE, or the kernel may recycle its number first. */
    {
        int tries = 0;
        while (wl_display_flush(conn->display) < 0 && errno == EAGAIN &&
               tries < 64) {
            struct pollfd pfd = {
                .fd = wl_display_get_fd(conn->display),
                .events = POLLOUT,
            };
            poll(&pfd, 1, 10);
            tries++;
        }
    }
    close(fd);
    if (pool == NULL) {
        munmap(buf, length);
        FDK_WARN("toplevel icon: wl_shm_create_pool failed");
        return;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, side, side, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (buffer == NULL) {
        munmap(buf, length);
        FDK_WARN("toplevel icon: wl_shm_pool_create_buffer failed");
        return;
    }
    /* Keep the icon buffer OFF the release queue: wl_buffer::release
     * is documented UNUSED for this protocol, and the mapping is
     * frozen anyway — no listener, no queue, no re-entrancy. */

    /* Fill: transparent square, source centered. */
    memset(buf, 0, length);
    int off_x = (side - width) / 2;
    int off_y = (side - height) / 2;
    for (int y = 0; y < height; y++) {
        uint32_t *dst = buf + (size_t)(off_y + y) * (size_t)side +
                        (size_t)off_x;
        const fdk_u32 *src = pixels + (size_t)y * (size_t)width;
        memcpy(dst, src, (size_t)width * 4u);
    }
    /* The mapping can go NOW: wl_shm's contract keeps the
     * server-side mapping alive until the buffer is destroyed, and
     * the client may munmap once the requests are written (the pool
     * is already destroyed; the region is frozen from the client's
     * perspective the moment add_buffer is sent below). */
    munmap(buf, length);

    /* create icon -> add_buffer -> set_icon -> commit; keep the
     * pair on the window (see wayland_platform.h for the lifetime
     * reasoning). */
    struct xdg_toplevel_icon_v1 *icon =
        xdg_toplevel_icon_manager_v1_create_icon(conn->toplevel_icon_manager);
    if (icon == NULL) {
        wl_buffer_destroy(buffer);
        FDK_WARN("toplevel icon: create_icon failed");
        return;
    }
    xdg_toplevel_icon_v1_add_buffer(icon, buffer, 1);
    xdg_toplevel_icon_manager_v1_set_icon(conn->toplevel_icon_manager,
                                          pwindow->xdg_toplevel, icon);
    pwindow->toplevel_icon = icon;
    pwindow->toplevel_icon_buffer = buffer;
    pwindow->toplevel_icon_buffer_size.width = side;
    pwindow->toplevel_icon_buffer_size.height = side;
    /* The icon state is double-buffered: it applies at the surface's
     * next commit. Send one only past the first configure; before
     * that the deferred-first-frame commit (xdg_surface_configure's
     * handshake) carries the toplevel state to the compositor — a
     * pre-configure state commit is protocol-legal but would race
     * FDK's own gate discipline for zero benefit. */
    if (pwindow->configured) {
        wl_surface_commit(pwindow->surface);
    }
}

void fdk_wayland_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height) {
    /* Extension surfaces (1.1.0): the size is ROLE-driven (the
     * add-on reports it via fdk_ext_surface_notify_configure) — a
     * client-side resize request has no meaning for a role FDK does
     * not know, and the buffer-commit machinery below would fight
     * the role's own size negotiation. Quietly ignored, matching
     * fdk_extend.h's configure-flow documentation. */
    if (pwindow->ext) {
        return;
    }
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
    /* Popups: no toplevel to constrain (see set_title). */
    if (pwindow == NULL || pwindow->xdg_toplevel == NULL) {
        return;
    }
    xdg_toplevel_set_min_size(pwindow->xdg_toplevel, min_size.width, min_size.height);
    xdg_toplevel_set_max_size(pwindow->xdg_toplevel, max_size.width, max_size.height);
}

/* ---- Phase 8: window management (xdg-decoration + toplevel ops) ---- */

/* Compare-and-flip + FDK_EVENT_WINDOW_STATE dispatch (no-op when the
 * state didn't change). Called from xdg_toplevel_configure (the
 * compositor's authoritative states) and set_minimized (the
 * request-optimistic flip). */
void fdk_wayland_window_update_state(fdk_platform_window *pwindow,
                                     int maximized, int minimized,
                                     int fullscreen) {
    if (pwindow->maximized == maximized &&
        pwindow->minimized == minimized &&
        pwindow->fullscreen == fullscreen) {
        return;
    }
    pwindow->maximized = maximized;
    pwindow->minimized = minimized;
    pwindow->fullscreen = fullscreen;
    fdk_event_data event;
    memset(&event, 0, sizeof event);
    event.type = FDK_EVENT_WINDOW_STATE;
    event.state.maximized = maximized;
    event.state.minimized = minimized;
    event.state.fullscreen = fullscreen;
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
    /* Popups: no toplevel (see set_title) — refuse honestly rather
     * than dereferencing the NULL role object. */
    if (pwindow->xdg_toplevel == NULL) {
        return FDK_ERR_UNSUPPORTED;
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

fdk_result fdk_wayland_window_set_fullscreen(
    fdk_platform_window *pwindow, bool fullscreen) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Popups: no toplevel (see set_maximized) — refuse honestly. */
    if (pwindow->xdg_toplevel == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (fullscreen) {
        xdg_toplevel_set_fullscreen(pwindow->xdg_toplevel, NULL);
    } else {
        xdg_toplevel_unset_fullscreen(pwindow->xdg_toplevel);
    }
    /* No optimistic flip: the compositor answers with a configure
     * carrying FULLSCREEN in states (or not); update_state runs from
     * xdg_toplevel_configure either way (the set_maximized rule). */
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
    /* Popups: no toplevel (see set_title). */
    if (pwindow->xdg_toplevel == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    xdg_toplevel_set_minimized(pwindow->xdg_toplevel);
    /* Fire-and-forget request: no acknowledgement exists, so mark
     * optimistic (cleared on the next activated configure). */
    fdk_wayland_window_update_state(pwindow, pwindow->maximized,
                                    1, pwindow->fullscreen);
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
    if (pwindow->xdg_toplevel == NULL) {
        return FDK_ERR_UNSUPPORTED; /* popups: no toplevel (set_title) */
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
    if (pwindow->xdg_toplevel == NULL) {
        return FDK_ERR_UNSUPPORTED; /* popups: no toplevel (set_title) */
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
