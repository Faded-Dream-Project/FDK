#define FDK_LOG_TAG "core"

#include "fdk/fdk_core.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"

#include <string.h>

static char *dup_string(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = fdk_alloc(len);
    if (copy != NULL) {
        memcpy(copy, s, len);
    }
    return copy;
}

fdk_result fdk_init(fdk_context **out_ctx, const fdk_init_options *options) {
    if (out_ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_context *ctx = fdk_alloc(sizeof(fdk_context));
    if (ctx == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    ctx->backend = FDK_PLATFORM_AUTO;
    ctx->app_id = NULL;
    ctx->running = 0;
    ctx->quit_requested = 0;

    if (options != NULL) {
        ctx->backend = options->backend;
        if (options->app_id != NULL) {
            ctx->app_id = dup_string(options->app_id);
            if (ctx->app_id == NULL) {
                fdk_free(ctx);
                return FDK_ERR_OUT_OF_MEMORY;
            }
        }
    }

    /* NOTE: Phase 1 deliberately does not attempt an X11/Wayland
     * connection — that is the platform layer, built in Phase 2 (see
     * src/platform/). A context is fully constructed and usable for
     * core-level bookkeeping (logging, version queries, future timers)
     * without one. Once the platform layer lands, fdk_init() will
     * attempt that connection here and return FDK_ERR_NO_DISPLAY /
     * FDK_ERR_PLATFORM_INIT on failure, per the documented contract in
     * fdk_core.h — that contract is written now so the header does not
     * need to change shape later. */

    FDK_INFO("initialized (app_id=%s)", ctx->app_id ? ctx->app_id : "(none)");

    *out_ctx = ctx;
    return FDK_OK;
}

void fdk_run(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }

    ctx->running = 1;
    ctx->quit_requested = 0;

    FDK_WARN("fdk_run() called but no platform event source exists yet "
             "(Phase 2) — returning immediately");

    /* Phase 2 replaces this with a real poll/dispatch loop over the
     * platform connection's event fd, timers, and idle queue. */

    ctx->running = 0;
}

void fdk_quit(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->quit_requested = 1;
}

void fdk_shutdown(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }

    FDK_INFO("shutting down");

    fdk_free(ctx->app_id);
    fdk_free(ctx);
}
