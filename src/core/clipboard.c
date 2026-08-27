#define FDK_LOG_TAG "clipboard"

#include "fdk/fdk_clipboard.h"

#include "core/context_internal.h"
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
