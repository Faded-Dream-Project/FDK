/* thread_internal.h — the internal face of src/core/thread.c.
 *
 * The public contract lives in include/fdk/fdk_thread.h; this file
 * is what the context/pump glue includes (it needs the hooks, not
 * the application-facing documentation). */
#ifndef FDK_CORE_THREAD_INTERNAL_H
#define FDK_CORE_THREAD_INTERNAL_H

#include "fdk/fdk_core.h"

struct fdk_context;

fdk_result fdk__thread_init(struct fdk_context *ctx);
void fdk__thread_shutdown(struct fdk_context *ctx);
int fdk__thread_wake_fd(const struct fdk_context *ctx);
int fdk__thread_drain(struct fdk_context *ctx);
bool fdk__on_main_thread(const struct fdk_context *ctx);
fdk_result fdk__wrong_thread_refused(const char *what);

/* The debug-build guard. Compiles to nothing in release builds (the
 * historical undefined-if-you-race behavior stands there); in debug
 * builds every guarded entry point refuses cross-thread callers. */
#if defined(FDK_DEBUG_THREAD_CHECKS)
#define FDK_MAIN_THREAD_GUARD(ctx, what) \
    do { \
        if (!fdk__on_main_thread((ctx))) { \
            return fdk__wrong_thread_refused((what)); \
        } \
    } while (0)
#else
#define FDK_MAIN_THREAD_GUARD(ctx, what) (void)(ctx)
#endif

#endif /* FDK_CORE_THREAD_INTERNAL_H */
