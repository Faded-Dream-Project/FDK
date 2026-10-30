/*
 * toplevel.c — the foreign toplevel list's public half (1.1.0, D-4)
 *
 * The thin wrapper over the backend's toplevels_* ops (see
 * platform_internal.h's D-4 block for the seam's contract):
 * validation, the fixed-buffer snapshot copies the tray-host
 * discipline demands, and the degradation posture (a backend
 * without the mechanism answers an empty list, never an error).
 *
 * The event model is deliberately COARSE: one changed callback,
 * re-snapshot semantics — a taskbar re-renders from the snapshot
 * whatever happened (D-014's tray-host shape, applied to the
 * compositor's window set instead of the bus's items).
 */

#define FDK_LOG_TAG "toplevel"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "platform/platform_internal.h"

#include "fdk/fdk_toplevel.h"

#include <stdio.h>
#include <string.h>

struct fdk_toplevel_list {
    fdk_context *ctx;
    struct fdk_platform_toplevels *backend;
    fdk_toplevel_changed_fn on_change;
    void *on_change_user;
    int closing; /* close-inside-callback deferral flag          */
};

/* The backend's coarse change → the application's coarse change.
 * Runs between events (the pump's callback discipline), so the
 * application may re-snapshot, act, or CLOSE the list — the close
 * defers through `closing` so the backend never calls into a freed
 * wrapper mid-notification. */
static void toplevels_backend_changed(void *user) {
    fdk_toplevel_list *list = user;
    if (list == NULL || list->on_change == NULL) {
        return;
    }
    if (list->closing) {
        return; /* torn down; the backend flushes with the tracker */
    }
    list->on_change(list, list->on_change_user);
}

fdk_toplevel_list *fdk_toplevel_list_open(fdk_context *ctx,
                                          fdk_toplevel_changed_fn on_change,
                                          void *user) {
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return NULL;
    }
    if (ctx->ops->toplevels_open == NULL) {
        /* The backend has no mechanism at all — the documented
         * degradation: NULL, not an error code (the caller's NULL
         * check IS the taskbar-absent branch). */
        return NULL;
    }
    fdk_toplevel_list *list = fdk_alloc(sizeof(*list));
    if (list == NULL) {
        return NULL;
    }
    list->ctx = ctx;
    list->backend = NULL;
    list->on_change = on_change;
    list->on_change_user = user;
    list->closing = 0;

    fdk_result r = ctx->ops->toplevels_open(
        ctx->conn, toplevels_backend_changed, list, &list->backend);
    if (!fdk_ok(r)) {
        /* The protocol absent (Wayland) or the root property
         * unreadable (a bare X server): the honest degradation. */
        fdk_free(list);
        return NULL;
    }
    FDK_DEBUG("foreign toplevel list opened");
    return list;
}

void fdk_toplevel_list_close(fdk_toplevel_list *list) {
    if (list == NULL) {
        return;
    }
    if (list->closing) {
        return; /* already tearing down (self-close discipline)   */
    }
    list->closing = 1;
    if (list->ctx != NULL && list->ctx->ops != NULL &&
        list->ctx->ops->toplevels_close != NULL) {
        list->ctx->ops->toplevels_close(list->backend);
    }
    fdk_free(list);
}

fdk_u32 fdk_toplevel_count(fdk_toplevel_list *list) {
    if (list == NULL || list->ctx == NULL || list->ctx->ops == NULL ||
        list->ctx->ops->toplevels_count == NULL) {
        return 0;
    }
    return list->ctx->ops->toplevels_count(list->backend);
}

fdk_result fdk_toplevel_get_info(fdk_toplevel_list *list, fdk_u32 index,
                                 fdk_toplevel_info *out) {
    if (list == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (out->struct_size != sizeof(*out)) {
        /* The append-only contract: the caller must pass EXACTLY
         * the struct it compiled with (the tray-host read APIs' rule
         * — a zero/short struct is a caller bug, loudly refused). */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (list->ctx == NULL || list->ctx->ops == NULL ||
        list->ctx->ops->toplevels_at == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    const struct fdk_platform_toplevel *item =
        list->ctx->ops->toplevels_at(list->backend, index);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* index out of the snapshot */
    }
    out->id = item->id;
    out->states = item->states;
    /* Fixed-buffer copies — the backend's strings live only until
     * its next change fires; the application's copy outlives it. */
    snprintf(out->title, sizeof(out->title), "%s",
             item->title != NULL ? item->title : "");
    snprintf(out->app_id, sizeof(out->app_id), "%s",
             item->app_id != NULL ? item->app_id : "");
    return FDK_OK;
}

fdk_result fdk_toplevel_activate(fdk_toplevel_list *list, fdk_u32 index) {
    if (list == NULL || list->ctx == NULL || list->ctx->ops == NULL ||
        list->ctx->ops->toplevels_activate == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return list->ctx->ops->toplevels_activate(list->backend, index);
}

fdk_result fdk_toplevel_close(fdk_toplevel_list *list, fdk_u32 index) {
    if (list == NULL || list->ctx == NULL || list->ctx->ops == NULL ||
        list->ctx->ops->toplevels_close_window == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return list->ctx->ops->toplevels_close_window(list->backend, index);
}
