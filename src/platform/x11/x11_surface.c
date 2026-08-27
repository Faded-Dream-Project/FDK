#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/log_internal.h"

#include <X11/Xutil.h> /* XDestroyImage macro, XCreateImage */
#include <stdlib.h>
#include <string.h>

/* Software framebuffer for the X11 backend — the machinery behind
 * fdk_surface (include/fdk/fdk_surface.h) on this platform.
 *
 * Phase 3 completion design (double buffering + MIT-SHM):
 *
 *  - TWO pixel slots per window ("front"/"back", swapped every
 *    present). The application always draws the back slot; present
 *    blits the back slot and swaps. With plain XPutImage (the copy
 *    path) this is belt-and-braces — XPutImage copies pixels into the
 *    request stream before returning — but with MIT-SHM it is a
 *    CORRECTNESS requirement: the server reads a shared segment
 *    asynchronously after XShmPutImage returns, so the segment must
 *    not be redrawn until the server's ShmCompletion event arrives.
 *    The app can therefore only ever alias a buffer the server is
 *    done with: acquire hands out the back slot, and a still-in-flight
 *    back slot is synced first (see wait_slot_completion).
 *
 *  - MIT-SHM (shared memory) is used when the server supports it
 *    (probed once per connection, see x11_connection.c) and the
 *    environment has not opted out with FDK_NO_MIT_SHM=1: present
 *    becomes XShmPutImage — the server reads the pixels through the
 *    shared segment instead of FDK memcpy-ing them into the socket.
 *    This is both the local-socket fast path AND the only sane path
 *    for remote X (forwarding a megabyte per frame through the ssh
 *    channel vs. a few bytes of request). Each slot owns one
 *    XShmSegmentInfo; segments use the attach-then-IPC_RMID pattern
 *    so a crashed client leaks nothing in /dev/shm.
 *
 *  - ShmCompletion routing: every present sends exactly ONE
 *    send_event=True put (the last damage rect of the batch — the
 *    server processes requests in order, so its completion implies
 *    the whole batch's). The completion event carries the slot's
 *    segment id; x11_dispatch.c clears in_flight. If the app draws
 *    before that arrives, wait_slot_completion() does one XSync +
 *    XCheckTypedEvent drain (extension events only — normal events
 *    stay queued for the pump; no application callbacks can run
 *    here).
 *
 *  - The visual check (standard 24-bit TrueColor only) and the
 *    damage-driven put policy (per-rect sub-image puts, >=75% switches
 *    to one whole-image put) are unchanged from the first slice; only
 *    WHERE the pixels live and HOW they travel changed.
 *
 *  - Non-SHM slot pixel buffers are allocated with plain malloc(),
 *    NOT fdk_alloc(), because XDestroyImage() releases them with
 *    Xlib's own free — mixing allocators here would be a real bug,
 *    not a style choice. (SHM slots' pixels live in a shmget segment,
 *    not the heap at all.)
 */

/* Native byte order in Xlib's LSBFirst/MSBFirst vocabulary, for
 * XImage.byte_order. The pixel values applications write are native
 * fdk_u32s (R in bits 23..16 etc), so the image must be told they are
 * in native order and Xlib converts to the wire format as needed. */
static int native_byte_order(void) {
    const uint16_t probe = 1;
    return (*(const unsigned char *)&probe == 1) ? LSBFirst : MSBFirst;
}

/* ---- slot lifecycle ----------------------------------------------------- */

/* Destroys one slot's XImage and shared segment (or malloc buffer).
 * Safe on a never-created slot (image == NULL). */
static void slot_destroy(fdk_platform_connection *conn,
                         fdk_platform_window *pwindow, int slot) {
    XImage *image = pwindow->render_slots[slot].image;
    if (image == NULL) {
        return;
    }

    if (pwindow->render_slots[slot].shm_attached) {
        /* Detach FIRST, then stop XDestroyImage from freeing the
         * shared-segment pointer (it would XFree() memory that came
         * from shmat — allocator mixing = heap corruption), then
         * unmap. The segment itself is already IPC_RMID'd (marked
         * dying at creation; it fully disappears once both the
         * server and this process detach — the crash-safe pattern). */
        XShmDetach(conn->display, &pwindow->render_slots[slot].shm);
        image->data = NULL;
        XDestroyImage(image);
        shmdt(pwindow->render_slots[slot].shm.shmaddr);
        pwindow->render_slots[slot].shm_attached = 0;
    } else if (pwindow->render_slots[slot].malloc_data != NULL) {
        XDestroyImage(image); /* frees malloc_data with Xlib's free */
        pwindow->render_slots[slot].malloc_data = NULL;
    } else {
        XDestroyImage(image); /* data == NULL: tolerates it */
    }
    pwindow->render_slots[slot].image = NULL;
    pwindow->render_slots[slot].in_flight = 0;
}

/* Creates one slot's XImage at (w, h): SHM-backed when conn->shm_ok,
 * malloc-backed otherwise. Returns 0 on success. */
static int slot_create(fdk_platform_connection *conn,
                       fdk_platform_window *pwindow, int slot, int w, int h) {
    int screen = conn->screen;
    Visual *visual = DefaultVisual(conn->display, screen);
    int depth = DefaultDepth(conn->display, screen);

    if (visual == NULL || visual->class != TrueColor ||
        visual->red_mask != 0xFF0000UL || visual->green_mask != 0xFF00UL ||
        visual->blue_mask != 0xFFUL || depth != 24) {
        /* See file header: only the standard 24-bit TrueColor layout
         * is supported. This is a property of the X server's visual
         * for our window, not of anything the application did wrong —
         * hence a WARN and a distinct error code, not a crash. */
        FDK_WARN("unsupported visual layout (class=%d, depth=%d, "
                 "masks=%06lx/%04lx/%02lx) for software rendering",
                 visual != NULL ? visual->class : -1, depth,
                 visual != NULL ? visual->red_mask : 0UL,
                 visual != NULL ? visual->green_mask : 0UL,
                 visual != NULL ? visual->blue_mask : 0UL);
        return -1;
    }

    XImage *image = NULL;

    if (conn->shm_ok) {
        XShmSegmentInfo *shm = &pwindow->render_slots[slot].shm;
        size_t length = (size_t)w * (size_t)h * 4u; /* 32bpp, ZPixmap */

        shm->shmid = shmget(IPC_PRIVATE, length, IPC_CREAT | 0777);
        if (shm->shmid < 0) {
            FDK_DEBUG("shmget failed (%zu bytes) — falling back to "
                      "the copy path for this slot",
                      length);
            goto copy_path;
        }
        shm->shmaddr = (char *)shmat(shm->shmid, NULL, 0);
        if (shm->shmaddr == (char *)-1) {
            shmctl(shm->shmid, IPC_RMID, NULL);
            FDK_DEBUG("shmat failed — falling back to the copy path");
            goto copy_path;
        }
        shm->readOnly = False;

        /* Attach the server, then mark the segment dying immediately:
         * it persists exactly until both sides detach, so a crashed
         * client (or server) leaks nothing in /dev/shm. */
        if (!XShmAttach(conn->display, shm)) {
            shmdt(shm->shmaddr);
            shmctl(shm->shmid, IPC_RMID, NULL);
            FDK_DEBUG("XShmAttach refused — falling back to the copy "
                      "path");
            goto copy_path;
        }
        shmctl(shm->shmid, IPC_RMID, NULL);

        image = XShmCreateImage(conn->display, visual, (unsigned int)depth,
                                ZPixmap, shm->shmaddr, shm,
                                (unsigned int)w, (unsigned int)h);
        if (image == NULL) {
            XShmDetach(conn->display, shm);
            shmdt(shm->shmaddr);
            FDK_WARN("XShmCreateImage failed — falling back to the "
                     "copy path");
            goto copy_path;
        }

        pwindow->render_slots[slot].shm_attached = 1;
        pwindow->render_slots[slot].malloc_data = NULL;
        pwindow->render_slots[slot].image = image;
        image->byte_order = (int)native_byte_order();
        image->bitmap_bit_order = (int)native_byte_order();
        return 0;
    }

copy_path:
    image = XCreateImage(conn->display, visual, (unsigned int)depth,
                         ZPixmap, 0, NULL, (unsigned int)w,
                         (unsigned int)h, 32 /* bitmap pad */,
                         0 /* auto stride */);
    if (image == NULL) {
        FDK_WARN("XCreateImage failed");
        return -1;
    }

    /* malloc'd buffer — XDestroyImage frees it with Xlib's free; see
     * file header for why fdk_alloc is wrong here. */
    image->data = malloc((size_t)image->bytes_per_line * (size_t)h);
    if (image->data == NULL) {
        FDK_WARN("framebuffer allocation failed (%dx%d)", w, h);
        XDestroyImage(image); /* tolerates NULL data */
        return -1;
    }

    image->byte_order = (int)native_byte_order();
    image->bitmap_bit_order = (int)native_byte_order();

    pwindow->render_slots[slot].shm_attached = 0;
    pwindow->render_slots[slot].malloc_data = image->data;
    pwindow->render_slots[slot].image = image;
    return 0;
}

/* ---- ShmCompletion routing ---------------------------------------------- */

/* Clears the in-flight flag of whichever slot owns `shmseg` on this
 * window. Called from the dispatch loop (x11_dispatch.c) and from
 * wait_slot_completion below — the two places ShmCompletion events
 * are consumed. */
void fdk_x11_surface_shm_completion(fdk_platform_window *pwindow,
                                    unsigned long shmseg) {
    for (int i = 0; i < 2; i++) {
        if (pwindow->render_slots[i].shm_attached &&
            (unsigned long)pwindow->render_slots[i].shm.shmseg == shmseg &&
            pwindow->render_slots[i].in_flight) {
            pwindow->render_slots[i].in_flight = 0;
            return;
        }
    }
    /* A completion for a slot we already destroyed (resize raced the
     * server) — nothing to clear, and not an error. */
}

/* Waits for the slot's outstanding SHM put to complete. One XSync
 * round trip guarantees the server has finished processing the put
 * (requests are processed in order), which means the completion event
 * is already in our local queue; XCheckTypedEvent then pulls ONLY
 * extension completion events out (normal input events stay queued
 * for the pump — nothing here can run application code). If the flag
 * somehow survives, the caller falls back to the other slot — the
 * next completion will clear the stale one. */
static void wait_slot_completion(fdk_platform_connection *conn,
                                 fdk_platform_window *pwindow, int slot) {
    if (!pwindow->render_slots[slot].in_flight) {
        return;
    }

    XSync(conn->display, False);
    int evtype = conn->shm_event_base + ShmCompletion;
    for (int guard = 0; guard < 64 && pwindow->render_slots[slot].in_flight;
         guard++) {
        XEvent ev;
        if (!XCheckTypedEvent(conn->display, evtype, &ev)) {
            break;
        }
        fdk_platform_window *owner =
            fdk_x11_find_window(conn, ev.xany.window);
        if (owner != NULL) {
            fdk_x11_surface_shm_completion(
                    owner,
                    (unsigned long)((const XShmCompletionEvent *)&ev)->shmseg);
        }
    }
}

/* ---- the ops ------------------------------------------------------------- */

fdk_result fdk_x11_window_get_framebuffer(fdk_platform_window *pwindow,
                                           fdk_platform_framebuffer *out_fb) {
    if (pwindow == NULL || out_fb == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_platform_connection *conn = pwindow->conn;

    /* The live window size (kept current by ConfigureNotify
     * translation in x11_events.c). Defensively clamp degenerate
     * sizes to 1 rather than handing Xlib a zero-dimension image. */
    fdk_i32 w = pwindow->last_size.width;
    fdk_i32 h = pwindow->last_size.height;
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;

    /* Recreate BOTH slots on any size change; XImages cannot be
     * resized in place. A resize while a put is in flight is the
     * documented race double-buffering exists to close: sync the
     * in-flight slot before tearing its segment out from under the
     * server. */
    if (pwindow->render_slots[0].image != NULL &&
        (pwindow->render_size.width != w || pwindow->render_size.height != h)) {
        if (pwindow->render_slots[0].in_flight) {
            wait_slot_completion(conn, pwindow, 0);
        }
        if (pwindow->render_slots[1].in_flight) {
            wait_slot_completion(conn, pwindow, 1);
        }
        slot_destroy(conn, pwindow, 0);
        slot_destroy(conn, pwindow, 1);
        pwindow->render_back = 0;
    }

    if (pwindow->render_slots[0].image == NULL) {
        if (slot_create(conn, pwindow, 0, w, h) != 0) {
            return FDK_ERR_UNSUPPORTED; /* visual check failed, logged */
        }
        if (slot_create(conn, pwindow, 1, w, h) != 0) {
            slot_destroy(conn, pwindow, 0);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        pwindow->render_back = 0;
        pwindow->render_size.width = w;
        pwindow->render_size.height = h;
        FDK_DEBUG("framebuffer pair created (%dx%d, %s path)", w, h,
                  conn->shm_ok ? "MIT-SHM" : "copy");
    }

    /* Hand out the back slot. If the previous frame's SHM put is
     * still being read by the server, wait for it; if the wait failed
     * (should not happen — XSync guarantees ordering), draw into the
     * front slot instead: better one frame of aliasing risk than a
     * deadlock, and the stale completion clears the flag later. */
    int slot = pwindow->render_back;
    if (pwindow->render_slots[slot].in_flight) {
        wait_slot_completion(conn, pwindow, slot);
        if (pwindow->render_slots[slot].in_flight) {
            /* Should be unreachable (XSync guarantees the server
             * processed the put). Draw into the other slot rather
             * than block; the overdue completion clears the stale
             * flag whenever it lands. */
            slot = 1 - slot;
            pwindow->render_back = slot;
            if (pwindow->render_slots[slot].in_flight) {
                wait_slot_completion(conn, pwindow, slot);
            }
            FDK_WARN("SHM completion overdue; drawing into the other "
                     "slot this frame");
        }
    }

    XImage *img = pwindow->render_slots[slot].image;
    out_fb->pixels = (fdk_u32 *)img->data;
    out_fb->width = w;
    out_fb->height = h;
    out_fb->stride = (fdk_i32)(img->bytes_per_line / 4);
    return FDK_OK;
}

/* One put of (x0,y0)-(x1,y1) of the slot's image onto the window.
 * shm_event = true on the LAST put of a present so exactly one
 * ShmCompletion comes back (requests are processed in order). */
static void put_region(fdk_platform_window *pwindow, XImage *img,
                       int shm_attached, long long x0, long long y0,
                       long long x1, long long y1, int shm_event) {
    fdk_platform_connection *conn = pwindow->conn;
    if (shm_attached) {
        XShmPutImage(conn->display, pwindow->xwindow, pwindow->render_gc, img,
                     (int)x0, (int)y0, (int)x0, (int)y0,
                     (unsigned int)(x1 - x0), (unsigned int)(y1 - y0),
                     shm_event ? True : False);
    } else {
        XPutImage(conn->display, pwindow->xwindow, pwindow->render_gc, img,
                  (int)x0, (int)y0, (int)x0, (int)y0,
                  (unsigned int)(x1 - x0), (unsigned int)(y1 - y0));
    }
}

fdk_result fdk_x11_window_present(fdk_platform_window *pwindow,
                                 const fdk_platform_damage *damage) {
    if (pwindow == NULL || damage == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Documented no-op when nothing was ever acquired/drawn (the
     * window simply keeps its background pixel contents). */
    if (pwindow->render_slots[0].image == NULL) {
        return FDK_OK;
    }

    /* Nothing changed since the last present — not even a request
     * is sent (the damage-tracking contract; see
     * platform_internal.h). */
    if (!damage->full && damage->count == 0) {
        return FDK_OK;
    }

    fdk_platform_connection *conn = pwindow->conn;

    if (pwindow->render_gc == NULL) {
        pwindow->render_gc = XCreateGC(conn->display, pwindow->xwindow, 0, NULL);
        if (pwindow->render_gc == NULL) {
            FDK_WARN("XCreateGC failed");
            return FDK_ERR_SURFACE_CREATE;
        }
    }

    /* Damage-driven blit: only the changed sub-rectangles of the
     * back slot are sent (each put is one request). Two coarsenings
     * keep request count sane (both are pure policy on this side of
     * the ops seam; the surface layer's contract only demands that
     * every damaged pixel reaches the screen):
     *   - rects are clamped to the image (damage arrives unclamped);
     *   - if the damaged area reaches 3/4 of the surface, one
     *     whole-image put beats a burst of overlapping sub-puts. */
    fdk_i32 w = pwindow->render_size.width;
    fdk_i32 h = pwindow->render_size.height;
    int slot = pwindow->render_back;
    XImage *img = pwindow->render_slots[slot].image;

    /* Clamp the damage rects once, up front, and decide the
     * whole-image coarsening (>= 75% of the surface damaged: one put
     * beats a burst of overlapping sub-puts). */
    long long clamp_x0[FDK_PLATFORM_DAMAGE_RECTS];
    long long clamp_y0[FDK_PLATFORM_DAMAGE_RECTS];
    long long clamp_x1[FDK_PLATFORM_DAMAGE_RECTS];
    long long clamp_y1[FDK_PLATFORM_DAMAGE_RECTS];
    int live = 0;
    long long damaged_area = 0;

    if (!damage->full) {
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
                clamp_x0[live] = x0;
                clamp_y0[live] = y0;
                clamp_x1[live] = x1;
                clamp_y1[live] = y1;
                live++;
                damaged_area += (x1 - x0) * (y1 - y0);
            }
        }
    }

    int whole = damage->full ||
                damaged_area * 4 >= (long long)w * h * 3;
    int shm_attached = pwindow->render_slots[slot].shm_attached;
    int total_puts = whole ? 1 : live;

    if (whole) {
        put_region(pwindow, img, shm_attached, 0, 0, w, h, 1);
    } else {
        for (int i = 0; i < live; i++) {
            /* The image's origin maps to the window's origin, so
             * source and destination offsets coincide; the LAST put
             * requests the SHM completion event. */
            put_region(pwindow, img, shm_attached, clamp_x0[i], clamp_y0[i],
                       clamp_x1[i], clamp_y1[i], i + 1 == total_puts);
        }
    }

    if (shm_attached && total_puts > 0) {
        pwindow->render_slots[slot].in_flight = 1;
    }

    XFlush(conn->display);

    /* Swap: the presented slot becomes front (its contents are now
     * what the server shows / is reading); the app draws the other
     * one next frame. */
    pwindow->render_back = 1 - slot;

    /* Slot synchronization — the correctness requirement of
     * damage-tracked DOUBLE buffering (found live by the layout
     * demo's breathing meter): the surface layer's damage model
     * assumes the drawing buffer matches the SCREEN outside the new
     * damage. With alternating slots that only holds if each slot is
     * brought up to date with what was just presented: without this
     * copy, frame N+1's partial damage ships rects from a slot whose
     * contents are from TWO presents ago — stale wherever frame N's
     * damage overlapped, which the screen shows as flickering
     * previous-century content (the layout demo alternated whole
     * correct frames with storm-era leftovers). Copy the presented
     * region front -> back so the slots differ only by future
     * drawing. The copy is bounded by the shipped bytes (a whole-
     * image present syncs the whole image — same traffic again, CPU
     * memcpy between the slot buffers, no extra X requests). */
    {
        int back = 1 - slot;
        XImage *src_img = pwindow->render_slots[slot].image;
        XImage *dst_img = pwindow->render_slots[back].image;
        if (src_img != NULL && dst_img != NULL &&
            src_img->data != dst_img->data &&
            src_img->width == dst_img->width &&
            src_img->height == dst_img->height) {
            if (whole) {
                size_t row_bytes = (size_t)w * sizeof(fdk_u32);
                for (int y = 0; y < h; y++) {
                    memcpy(dst_img->data +
                               (size_t)y * (size_t)dst_img->bytes_per_line,
                           src_img->data +
                               (size_t)y * (size_t)src_img->bytes_per_line,
                           row_bytes);
                }
            } else {
                for (int i = 0; i < live; i++) {
                    int ry0 = (int)clamp_y0[i];
                    int ry1 = (int)clamp_y1[i];
                    size_t row_bytes =
                        (size_t)(clamp_x1[i] - clamp_x0[i]) * sizeof(fdk_u32);
                    for (int y = ry0; y < ry1; y++) {
                        memcpy(dst_img->data +
                                   (size_t)y *
                                       (size_t)dst_img->bytes_per_line +
                                   (size_t)clamp_x0[i] * sizeof(fdk_u32),
                               src_img->data +
                                   (size_t)y *
                                       (size_t)src_img->bytes_per_line +
                                   (size_t)clamp_x0[i] * sizeof(fdk_u32),
                               row_bytes);
                    }
                }
            }
        }
    }
    return FDK_OK;
}

/* Called from fdk_x11_window_destroy() — releases both slots, their
 * pixel storage / shared segments, and the GC. */
void fdk_x11_surface_cleanup(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    /* If a put is still in flight, let the server finish reading
     * before tearing the segment away (XSync guarantees completion). */
    if (pwindow->conn->shm_ok) {
        if (pwindow->render_slots[0].in_flight) {
            wait_slot_completion(pwindow->conn, pwindow, 0);
        }
        if (pwindow->render_slots[1].in_flight) {
            wait_slot_completion(pwindow->conn, pwindow, 1);
        }
    }
    slot_destroy(pwindow->conn, pwindow, 0);
    slot_destroy(pwindow->conn, pwindow, 1);
    if (pwindow->render_gc != NULL) {
        XFreeGC(pwindow->conn->display, pwindow->render_gc);
        pwindow->render_gc = NULL;
    }
}
