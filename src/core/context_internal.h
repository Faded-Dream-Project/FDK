/*
 * context_internal.h — internal definition of struct fdk_context
 *
 * The public header (fdk_core.h) only ever exposes `fdk_context` as an
 * opaque forward-declared type (see fdk_types.h). Its real layout lives
 * here, internal to the library, so it can change freely between
 * releases without breaking ABI for applications that only ever hold
 * a pointer to it.
 */

#ifndef FDK_CONTEXT_INTERNAL_H
#define FDK_CONTEXT_INTERNAL_H

#include "fdk/fdk_core.h"

struct fdk_context {
    fdk_platform_backend backend;
    char *app_id;           /* owned, heap-allocated copy */

    int running;            /* nonzero while inside fdk_run() */
    int quit_requested;     /* set by fdk_quit() */

    /* Phase 2 will add: platform connection handle, window list,
     * timer queue, idle callback queue. Deliberately absent here —
     * Phase 1 does not fake a platform connection it doesn't have. */
};

#endif /* FDK_CONTEXT_INTERNAL_H */
