#define FDK_LOG_TAG "dnd"

/*
 * dnd.c — drag and drop frontend (1.2.0)
 *
 * Thin validation + plumbing layer behind include/fdk/fdk_dnd.h, the
 * same shape as core/clipboard.c: check the context/window, hand the
 * call to the backend's OPTIONAL DnD ops. Everything protocol-shaped
 * (XDND, wl_data_device drags) lives below the platform seam. The
 * shared uri-list codec (parse + percent-decode on receive, escape +
 * file:// encoding on send) lives in src/window/dnd_uri.c so both
 * backends and the headless tests use ONE implementation.
 */

#include "fdk/fdk_dnd.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "window_internal.h"

fdk_result fdk_window_set_drop_formats(fdk_window *window, int formats) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (formats & ~(FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    window->drop_formats = formats;
    if (window->ops != NULL && window->ops->window_set_drop_formats != NULL) {
        window->ops->window_set_drop_formats(window->pwindow, formats);
    }
    /* No backend op (headless): the mask is still stored on the
     * fdk_window so the query contract holds; no drag can ever arrive
     * on a connection that cannot receive them. */
    return FDK_OK;
}

int fdk_window_get_drop_formats(const fdk_window *window) {
    return (window != NULL) ? window->drop_formats : 0;
}

/* Backends call this when a drag they started concludes. The void*
 * user is the fdk_drag_closure allocated by fdk_drag_begin. */
void fdk__drag_report_done(int status, void *user) {
    fdk_drag_closure *c = user;
    if (c == NULL) {
        return;
    }
    if (c->on_done != NULL) {
        c->on_done((fdk_drag_status)status, c->user);
    }
    fdk_free(c);
}

fdk_result fdk_drag_begin(fdk_window *origin, int formats,
                          const char *text,
                          const char *const *uris, size_t uri_count,
                          fdk_drag_done_fn on_done, void *user_data) {
    return fdk_drag_begin_ex(origin, formats, text, uris, uri_count,
                             NULL, 0, 0, on_done, user_data);
}

fdk_result fdk_drag_begin_ex(fdk_window *origin, int formats,
                             const char *text,
                             const char *const *uris, size_t uri_count,
                             const fdk_u32 *icon, fdk_i32 icon_w,
                             fdk_i32 icon_h,
                             fdk_drag_done_fn on_done, void *user_data) {
    if (origin == NULL || formats == 0 ||
        (formats & ~(FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST))) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((formats & FDK_DRAG_FORMAT_URI_LIST) &&
        (uris == NULL || uri_count == 0)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (origin->ctx == NULL || origin->ops == NULL ||
        origin->ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (origin->ops->drag_begin == NULL) {
        FDK_WARN("dnd: backend \"%s\" cannot start drags",
                 origin->ops->name);
        return FDK_ERR_UNSUPPORTED;
    }

    /* The icon, clamped and scaled HERE so every backend receives the
     * same contract (<= 256 x 256, ready-to-copy ARGB8888): an icon
     * is drag feedback, not a second framebuffer. Nearest-neighbor
     * downscale preserves hard edges (the pixel-art case) and costs
     * one pass over the destination — never the source. */
    fdk_u32 *icon_copy = NULL;
    int iw = 0, ih = 0;
    if (icon != NULL && icon_w > 0 && icon_h > 0) {
        iw = (icon_w > 256) ? 256 : (int)icon_w;
        ih = (icon_h > 256) ? 256 : (int)icon_h;
        icon_copy = fdk_alloc(sizeof(fdk_u32) * (size_t)iw * (size_t)ih);
        if (icon_copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        for (int y = 0; y < ih; y++) {
            fdk_i32 sy = (fdk_i32)((int64_t)y * icon_h / ih);
            for (int x = 0; x < iw; x++) {
                fdk_i32 sx = (fdk_i32)((int64_t)x * icon_w / iw);
                icon_copy[(size_t)y * (size_t)iw + (size_t)x] =
                    icon[(size_t)sy * (size_t)icon_w + (size_t)sx];
            }
        }
    }

    fdk_drag_closure *c = fdk_alloc(sizeof(*c));
    if (c == NULL) {
        fdk_free(icon_copy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    c->on_done = on_done;
    c->user = user_data;

    fdk_result r = origin->ops->drag_begin(
        origin->pwindow, formats, text, uris, uri_count,
        icon_copy, iw, ih, fdk__drag_report_done, c);
    fdk_free(icon_copy); /* the backend copied what it needs (or failed) */
    if (!fdk_ok(r)) {
        fdk_free(c);
    }
    return r;
}
