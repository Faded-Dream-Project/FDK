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

    fdk_drag_closure *c = fdk_alloc(sizeof(*c));
    if (c == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    c->on_done = on_done;
    c->user = user_data;

    fdk_result r = origin->ops->drag_begin(
        origin->pwindow, formats, text, uris, uri_count,
        fdk__drag_report_done, c);
    if (!fdk_ok(r)) {
        fdk_free(c);
    }
    return r;
}
