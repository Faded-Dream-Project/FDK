#define FDK_LOG_TAG "clipboard"

#include "fdk/fdk_clipboard.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "../render/surface_internal.h"
#include "core/log_internal.h"

/* Thin frontend: validate the context, then hand the call to the
 * backend's OPTIONAL clipboard ops. Everything protocol-shaped lives
 * below the platform seam (see docs/architecture.md). */

fdk_result fdk_clipboard_set_text(fdk_context *ctx, const char *text) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->conn == NULL || ctx->ops == NULL) {
        /* No platform connection (headless test contexts): report the
         * same condition fdk_init() would have. */
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops->clipboard_set_text == NULL) {
        FDK_WARN("clipboard: backend \"%s\" has no clipboard support",
                 ctx->ops->name);
        return FDK_ERR_UNSUPPORTED;
    }
    return ctx->ops->clipboard_set_text(ctx->conn, text);
}

char *fdk_clipboard_get_text(fdk_context *ctx) {
    if (ctx == NULL || ctx->conn == NULL || ctx->ops == NULL ||
        ctx->ops->clipboard_get_text == NULL) {
        /* Distinguish "no clipboard support" from "empty clipboard"
         * for the log line; the caller sees NULL either way, per the
         * documented contract. */
        if (ctx != NULL && ctx->conn != NULL && ctx->ops != NULL) {
            FDK_WARN("clipboard: backend \"%s\" has no clipboard support",
                     ctx->ops->name);
        }
        return NULL;
    }
    return ctx->ops->clipboard_get_text(ctx->conn);
}

/* ---- PRIMARY selection (1.3.4) ---- */

fdk_result fdk_clipboard_set_primary_text(fdk_context *ctx,
                                          const char *text) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->conn == NULL || ctx->ops == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops->clipboard_set_primary_text == NULL) {
        FDK_WARN("clipboard: backend \"%s\" has no PRIMARY selection "
                 "support",
                 ctx->ops->name);
        return FDK_ERR_UNSUPPORTED;
    }
    return ctx->ops->clipboard_set_primary_text(ctx->conn, text);
}

char *fdk_clipboard_get_primary_text(fdk_context *ctx) {
    if (ctx == NULL || ctx->conn == NULL || ctx->ops == NULL ||
        ctx->ops->clipboard_get_primary_text == NULL) {
        if (ctx != NULL && ctx->conn != NULL && ctx->ops != NULL) {
            FDK_WARN("clipboard: backend \"%s\" has no PRIMARY selection "
                     "support",
                     ctx->ops->name);
        }
        return NULL;
    }
    return ctx->ops->clipboard_get_primary_text(ctx->conn);
}

/* ---- clipboard images (1.4.9) ---- */

fdk_result fdk_clipboard_set_image(fdk_context *ctx,
                                   const fdk_surface *surface) {
    if (ctx == NULL || surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->conn == NULL || ctx->ops == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops->clipboard_set_image == NULL) {
        FDK_WARN("clipboard: backend \"%s\" has no image clipboard "
                 "support",
                 ctx->ops->name);
        return FDK_ERR_UNSUPPORTED;
    }
    /* The core owns the format: encode here, move bytes below the
     * seam, decode on the read side. */
    unsigned char *png = NULL;
    size_t len = 0;
    fdk_result r = fdk__png_encode(surface, &png, &len);
    if (!fdk_ok(r)) {
        return r;
    }
    r = ctx->ops->clipboard_set_image(ctx->conn, png, len);
    fdk_free(png);
    return r;
}

fdk_surface *fdk_clipboard_get_image(fdk_context *ctx) {
    if (ctx == NULL || ctx->conn == NULL || ctx->ops == NULL ||
        ctx->ops->clipboard_get_image == NULL) {
        if (ctx != NULL && ctx->conn != NULL && ctx->ops != NULL) {
            FDK_WARN("clipboard: backend \"%s\" has no image clipboard "
                     "support",
                     ctx->ops->name);
        }
        return NULL;
    }
    size_t len = 0;
    unsigned char *png = ctx->ops->clipboard_get_image(ctx->conn, &len);
    if (png == NULL || len == 0) {
        fdk_free(png);
        return NULL;
    }
    fdk_surface *surface = NULL;
    fdk_result r = fdk_surface_create_from_image_bytes(png, len,
                                                       &surface);
    fdk_free(png);
    if (!fdk_ok(r)) {
        return NULL; /* undecodable: reads as "no image"          */
    }
    return surface;
}
