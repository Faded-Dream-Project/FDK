#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/log_internal.h"

#include <X11/Xutil.h> /* XDestroyImage macro, XCreateImage */
#include <stdlib.h>

/* Software framebuffer for the X11 backend — the machinery behind
 * fdk_surface (include/fdk/fdk_surface.h) on this platform.
 *
 * Design notes (first Phase 3 slice):
 *
 *  - The framebuffer is an XImage in ZPixmap format, 32 bits per
 *    pixel, with the window's visual masks. We only support the
 *    standard 24-bit TrueColor layout (red 0xFF0000 / green 0xFF00 /
 *    blue 0xFF) — which is what Xvfb -depth 24 and effectively every
 *    mainstream Linux desktop provide — and report
 *    FDK_ERR_UNSUPPORTED otherwise rather than silently mangling
 *    colors on exotic visuals (8-bit PseudoColor, unusual 30-bit
 *    layouts, etc).
 *
 *  - XPutImage copies the pixel data into the X request stream
 *    before returning, so the application may freely rewrite the
 *    framebuffer right after present() — no synchronous flush of the
 *    image contents is needed for correctness (the XFlush at the end
 *    of present() just pushes the request toward the server promptly
 *    instead of waiting for the next automatic flush).
 *
 *  - Deliberately NOT using MIT-SHM in this first slice: XPutImage
 *    over a local socket is a memcpy per frame, which the current
 *    demo workload (640x480@60) is nowhere near needing to optimize.
 *    The MIT-SHM fast path (shared segment + XShmPutImage) can be
 *    added later inside this file without changing a single line
 *    anywhere else — the ops interface hands out a plain pixel
 *    pointer either way. Recorded in docs/roadmap.md.
 *
 *  - The XImage data buffer is allocated with plain malloc(), NOT
 *    fdk_alloc(), because XDestroyImage() releases it with Xlib's
 *    own free — mixing allocators here would be a real bug, not a
 *    style choice.
 */

/* Native byte order in Xlib's LSBFirst/MSBFirst vocabulary, for
 * XImage.byte_order. The pixel values applications write are native
 * fdk_u32s (R in bits 23..16 etc), so the image must be told they are
 * in native order and Xlib converts to the wire format as needed. */
static int native_byte_order(void) {
    const uint16_t probe = 1;
    return (*(const unsigned char *)&probe == 1) ? LSBFirst : MSBFirst;
}

fdk_result fdk_x11_window_get_framebuffer(fdk_platform_window *pwindow,
                                           fdk_platform_framebuffer *out_fb) {
    if (pwindow == NULL || out_fb == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    Display *dpy = pwindow->conn->display;

    /* The live window size (kept current by ConfigureNotify
     * translation in x11_events.c). Defensively clamp degenerate
     * sizes to 1 rather than handing Xlib a zero-dimension image. */
    fdk_i32 w = pwindow->last_size.width;
    fdk_i32 h = pwindow->last_size.height;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    /* Recreate the image on any size change; XImages cannot be
     * resized in place. */
    if (pwindow->render_image != NULL &&
        (pwindow->render_size.width != w || pwindow->render_size.height != h)) {
        XDestroyImage(pwindow->render_image);
        pwindow->render_image = NULL;
    }

    if (pwindow->render_image == NULL) {
        int screen = pwindow->conn->screen;
        Visual *visual = DefaultVisual(dpy, screen);
        int depth = DefaultDepth(dpy, screen);

        if (visual == NULL || visual->class != TrueColor ||
            visual->red_mask != 0xFF0000UL || visual->green_mask != 0xFF00UL ||
            visual->blue_mask != 0xFFUL || depth != 24) {
            /* See file header: only the standard 24-bit TrueColor
             * layout is supported in this slice. This is a property
             * of the X server's visual for our window, not of
             * anything the application did wrong — hence a WARN and
             * a distinct error code, not a crash. */
            FDK_WARN("unsupported visual layout (class=%d, depth=%d, "
                     "masks=%06lx/%04lx/%02lx) for software rendering",
                     visual != NULL ? visual->class : -1, depth,
                     visual != NULL ? visual->red_mask : 0UL,
                     visual != NULL ? visual->green_mask : 0UL,
                     visual != NULL ? visual->blue_mask : 0UL);
            return FDK_ERR_UNSUPPORTED;
        }

        XImage *image = XCreateImage(dpy, visual, (unsigned int)depth,
                                     ZPixmap, 0, NULL,
                                     (unsigned int)w, (unsigned int)h,
                                     32 /* bitmap pad */, 0 /* auto stride */);
        if (image == NULL) {
            FDK_WARN("XCreateImage failed");
            return FDK_ERR_OUT_OF_MEMORY;
        }

        /* malloc'd buffer — XDestroyImage frees it with Xlib's free;
         * see file header for why fdk_alloc is wrong here. */
        image->data = malloc((size_t)image->bytes_per_line * (size_t)h);
        if (image->data == NULL) {
            FDK_WARN("framebuffer allocation failed (%dx%d)", w, h);
            XDestroyImage(image); /* tolerates NULL data */
            return FDK_ERR_OUT_OF_MEMORY;
        }

        image->byte_order = (int)native_byte_order();
        image->bitmap_bit_order = (int)native_byte_order();

        pwindow->render_image = image;
        pwindow->render_size.width = w;
        pwindow->render_size.height = h;
        FDK_DEBUG("framebuffer created (%dx%d, stride %d bytes)", w, h,
                  image->bytes_per_line);
    }

    XImage *img = pwindow->render_image;
    out_fb->pixels = (fdk_u32 *)img->data;
    out_fb->width = w;
    out_fb->height = h;
    out_fb->stride = (fdk_i32)(img->bytes_per_line / 4);
    return FDK_OK;
}

fdk_result fdk_x11_window_present(fdk_platform_window *pwindow,
                                 const fdk_platform_damage *damage) {
    if (pwindow == NULL || damage == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Documented no-op when nothing was ever acquired/drawn (the
     * window simply keeps its background pixel contents). */
    if (pwindow->render_image == NULL) {
        return FDK_OK;
    }

    /* Nothing changed since the last present — not even a request
     * is sent (the damage-tracking contract; see
     * platform_internal.h). */
    if (!damage->full && damage->count == 0) {
        return FDK_OK;
    }

    Display *dpy = pwindow->conn->display;

    if (pwindow->render_gc == NULL) {
        pwindow->render_gc = XCreateGC(dpy, pwindow->xwindow, 0, NULL);
        if (pwindow->render_gc == NULL) {
            FDK_WARN("XCreateGC failed");
            return FDK_ERR_SURFACE_CREATE;
        }
    }

    /* Damage-driven blit: only the changed sub-rectangles of the
     * XImage are copied into the request stream (each XPutImage is
     * one request — a 64-rect damage region is still far cheaper
     * than a full-frame blit for small updates). Two coarsenings
     * keep request count sane (both are pure policy on this side of
     * the ops seam; the surface layer's contract only demands that
     * every damaged pixel reaches the screen):
     *   - rects are clamped to the image (damage arrives unclamped);
     *   - if the damaged area reaches 3/4 of the surface, one
     *     whole-image put beats a burst of overlapping sub-puts. */
    fdk_i32 w = pwindow->render_size.width;
    fdk_i32 h = pwindow->render_size.height;

    if (damage->full) {
        XPutImage(dpy, pwindow->xwindow, pwindow->render_gc,
                  pwindow->render_image,
                  0, 0, 0, 0, (unsigned int)w, (unsigned int)h);
    } else {
        long long damaged_area = 0;
        for (int i = 0; i < damage->count; i++) {
            const fdk_rect *rc = &damage->rects[i];
            long long x0 = rc->x, y0 = rc->y;
            long long x1 = (long long)rc->x + rc->width;
            long long y1 = (long long)rc->y + rc->height;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            if (x1 > x0 && y1 > y0) {
                damaged_area += (x1 - x0) * (y1 - y0);
            }
        }

        if (damaged_area * 4 >= (long long)w * h * 3) {
            /* >= 75% damaged: one whole-image request. */
            XPutImage(dpy, pwindow->xwindow, pwindow->render_gc,
                      pwindow->render_image,
                      0, 0, 0, 0, (unsigned int)w, (unsigned int)h);
        } else {
            for (int i = 0; i < damage->count; i++) {
                const fdk_rect *rc = &damage->rects[i];
                long long x0 = rc->x, y0 = rc->y;
                long long x1 = (long long)rc->x + rc->width;
                long long y1 = (long long)rc->y + rc->height;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                if (x1 > x0 && y1 > y0) {
                    /* The image's origin maps to the window's origin,
                     * so source and destination offsets coincide. */
                    XPutImage(dpy, pwindow->xwindow, pwindow->render_gc,
                              pwindow->render_image,
                              (int)x0, (int)y0, (int)x0, (int)y0,
                              (unsigned int)(x1 - x0),
                              (unsigned int)(y1 - y0));
                }
            }
        }
    }

    XFlush(dpy);
    return FDK_OK;
}

/* Called from fdk_x11_window_destroy() — releases the image, its
 * pixel buffer, and the GC. */
void fdk_x11_surface_cleanup(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    if (pwindow->render_image != NULL) {
        XDestroyImage(pwindow->render_image);
        pwindow->render_image = NULL;
    }
    if (pwindow->render_gc != NULL) {
        XFreeGC(pwindow->conn->display, pwindow->render_gc);
        pwindow->render_gc = NULL;
    }
}
