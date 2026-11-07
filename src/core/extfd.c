/*
 * extfd.c — extension fd sources for the event loop (1.1.0)
 *
 * The core half of include/fdk/fdk_extend.h's "hook into the event
 * loop" section: fd sources an add-on registers with
 * fdk_ext_loop_add_fd() join the pump's poll() set (src/core/
 * context.c lays the backend fd and the theme-settings watch first,
 * then these), and their callbacks fire from the pump with exactly
 * the discipline fdk_timer callbacks get — snapshot the array AND
 * the reported revents before any callback runs, re-check liveness
 * before every callback, defer the unlink+free of a source that
 * removed ITSELF until its callback returns. The rules live in
 * fdk_extend.h's doc comments; this file is the machinery.
 *
 * Design note — why not epoll: the pump already runs poll() with a
 * small fixed set; extending the same call keeps one syscall, one
 * timeout budget, and one ordering story (backend events first,
 * theme-watch second, add-on fds after). A handful of add-on fds on
 * a linear array matches the registries' O(n)-is-fine stance.
 */

#define FDK_LOG_TAG "core"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"

#include <poll.h>
#include <string.h>

/* The public record. `active` is the removal flag, the timer
 * discipline: remove marks inactive (never fires again); the fire
 * loop performs the actual unlink+free — inside a callback that
 * means AFTER it returns. `internal` marks FDK's OWN module
 * sources (the D-Bus client's socket): their callbacks run like
 * any other's, but their firings are INTERNAL traffic — the pump
 * keeps waiting for the remaining timeout instead of returning
 * (the MIT-SHM completion rule, generalized to every FDK-owned
 * fd; an application's add-on sources keep the counted contract
 * fdk_extend.h documents). Without the distinction, one round of
 * bridge handshake traffic returned the pump early and the X
 * events still in flight missed their pump window — the CI-red
 * class the S-8 bus introduced. */
struct fdk_ext_fd_source {
    fdk_context *ctx;
    int fd;
    uint32_t flags; /* FDK_EXT_FD_* watch mask                    */
    fdk_ext_fd_fn fn;
    void *user_data;
    bool active;
    bool internal;
};

/* FDK_EXT_FD_* are numerically poll(2)'s bits (fdk_extend.h), so the
 * translation is a masked copy — asserted at compile time below so a
 * future divergence fails the build instead of silently miswatching. */
#define FDK_EXTFD_POLL_MASK (FDK_EXT_FD_IN | FDK_EXT_FD_OUT)
#if (FDK_EXT_FD_IN != POLLIN) || (FDK_EXT_FD_OUT != POLLOUT) || \
    (FDK_EXT_FD_ERR != POLLERR) || (FDK_EXT_FD_HUP != POLLHUP)
#error "FDK_EXT_FD_* bits have diverged from poll(2) — fix one of them"
#endif

static fdk_ext_fd_source *extfd_add(fdk_context *ctx, int fd,
                                    uint32_t flags, fdk_ext_fd_fn fn,
                                    void *user_data, bool internal) {
    if (ctx == NULL || fd < 0 || fn == NULL) {
        return NULL;
    }
    /* Only IN/OUT are watchable (ERR/HUP are reported regardless,
     * exactly like poll(2) — any other bits are rejected rather than
     * silently dropped). */
    if ((flags & ~FDK_EXTFD_POLL_MASK) != 0 || flags == 0) {
        FDK_WARN("fdk_ext_loop_add_fd: bad flags 0x%x (IN|OUT only)",
                 flags);
        return NULL;
    }

    if (ctx->extfd_count == ctx->extfd_capacity) {
        size_t cap = (ctx->extfd_capacity == 0)
            ? 8
            : ctx->extfd_capacity * 2;
        if (cap < ctx->extfd_capacity ||
            cap > SIZE_MAX / sizeof(fdk_ext_fd_source *)) {
            return NULL; /* refuse absurd growth */
        }
        fdk_ext_fd_source **grown =
            fdk_realloc(ctx->extfds, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        ctx->extfds = grown;
        ctx->extfd_capacity = cap;
    }

    fdk_ext_fd_source *src = fdk_alloc(sizeof(*src));
    if (src == NULL) {
        return NULL;
    }
    src->ctx = ctx;
    src->fd = fd;
    src->flags = flags;
    src->fn = fn;
    src->user_data = user_data;
    src->active = true;
    src->internal = internal;
    ctx->extfds[ctx->extfd_count++] = src;
    return src;
}

fdk_ext_fd_source *fdk_ext_loop_add_fd(fdk_context *ctx, int fd,
                                       uint32_t flags, fdk_ext_fd_fn fn,
                                       void *user_data) {
    return extfd_add(ctx, fd, flags, fn, user_data, false);
}

/* The internal twin (context_internal.h): same machinery, one
 * semantic difference — the firings do not count toward the pump's
 * return value. FDK's own D-Bus sockets ride this; an add-on's
 * sources ride the public one. */
fdk_ext_fd_source *fdk__extfd_add_internal(fdk_context *ctx, int fd,
                                           uint32_t flags,
                                           fdk_ext_fd_fn fn,
                                           void *user_data) {
    return extfd_add(ctx, fd, flags, fn, user_data, true);
}

/* Unlinks a dead source from the array and frees it. Only called
 * when the record is NOT the currently-firing one (whose free the
 * fire loop defers — see the struct comment). */
static void extfd_unlink_free(fdk_context *ctx, fdk_ext_fd_source *src) {
    for (size_t i = 0; i < ctx->extfd_count; i++) {
        if (ctx->extfds[i] == src) {
            ctx->extfds[i] = ctx->extfds[--ctx->extfd_count];
            break;
        }
    }
    fdk_free(src);
}

void fdk_ext_loop_remove_fd(fdk_ext_fd_source *source) {
    if (source == NULL || !source->active) {
        return;
    }
    source->active = false; /* never fires again, even if readable */
    fdk_context *ctx = source->ctx;
    if (ctx->extfd_firing == source) {
        /* Its own callback is removing it: the fire loop frees the
         * record after the callback returns — freeing here would
         * pull the record out from under the loop. */
        return;
    }
    extfd_unlink_free(ctx, source);
}

int fdk_ext_fd_source_fd(const fdk_ext_fd_source *source) {
    return (source != NULL) ? source->fd : -1;
}

fdk_result fdk_ext_loop_update_fd(fdk_ext_fd_source *source,
                                  uint32_t flags) {
    if (source == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!source->active) {
        return FDK_OK; /* mid-removal: it will never fire again anyway */
    }
    if ((flags & ~FDK_EXTFD_POLL_MASK) != 0 || flags == 0) {
        FDK_WARN("fdk_ext_loop_update_fd: bad flags 0x%x (IN|OUT only)",
                 flags);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    source->flags = flags;
    return FDK_OK;
}

/* ---- the pump-facing face (context_internal.h) ---- */

size_t fdk__extfd_count(fdk_context *ctx) {
    return (ctx != NULL) ? ctx->extfd_count : 0;
}

/* The pump's poll array is a bounded stack buffer (context.c); the
 * fill stops at this many entries and warns once — an add-on with
 * more sources than this is misusing the loop (epoll is that way). */
#define FDK_EXTFD_POLL_MAX 256

size_t fdk__extfd_fill_poll(fdk_context *ctx, struct pollfd *pfds,
                            size_t base) {
    if (ctx == NULL) {
        return 0;
    }
    size_t k = base;
    int warned = 0;
    for (size_t i = 0; i < ctx->extfd_count; i++) {
        fdk_ext_fd_source *src = ctx->extfds[i];
        if (!src->active) {
            continue; /* mid-removal (its own callback): skip the fd */
        }
        if (k - base >= FDK_EXTFD_POLL_MAX) {
            if (!warned) {
                FDK_WARN("more than %d extension fd sources — the "
                         "rest are not polled this round",
                         FDK_EXTFD_POLL_MAX);
                warned = 1;
            }
            break;
        }
        pfds[k].fd = src->fd;
        pfds[k].events = (short)(src->flags & FDK_EXTFD_POLL_MASK);
        pfds[k].revents = 0;
        k++;
    }
    return k - base;
}

int fdk__extfd_dispatch(fdk_context *ctx, const struct pollfd *pfds,
                        size_t base, size_t n) {
    if (ctx == NULL || n == 0) {
        return 0;
    }
    /* Snapshot (source, revents) pairs FIRST, before any callback
     * runs: a callback may remove earlier sources, which shifts the
     * live array — but the pfds layout is frozen at fill time, so
     * revents read AFTER callbacks would map to the wrong entries.
     * Caching the pairs here (and re-checking `active` before each
     * callback) is the timer fire loop's exact discipline, adapted
     * for poll entries. */
    struct due_entry {
        fdk_ext_fd_source *src;
        uint32_t revents;
    } *due = NULL;
    size_t due_n = 0;
    size_t k = base;
    for (size_t i = 0; i < ctx->extfd_count; i++) {
        fdk_ext_fd_source *src = ctx->extfds[i];
        if (!src->active) {
            continue;
        }
        const struct pollfd *p = &pfds[k];
        k++;
        /* ERR/HUP are reported even when unwatched (poll(2)'s rule);
         * IN/OUT only when watched. */
        uint32_t revents = (uint32_t)(p->revents &
                                      ((short)FDK_EXTFD_POLL_MASK |
                                       POLLERR | POLLHUP | POLLNVAL));
        if (revents != 0) {
            due_n++;
        }
    }
    if (due_n == 0) {
        return 0;
    }
    due = fdk_alloc_array(due_n, sizeof(*due));
    if (due == NULL) {
        return 0; /* OOM: skip this round — sources fire next pump */
    }
    size_t w = 0;
    k = base;
    for (size_t i = 0; i < ctx->extfd_count; i++) {
        fdk_ext_fd_source *src = ctx->extfds[i];
        if (!src->active) {
            continue;
        }
        const struct pollfd *p = &pfds[k];
        k++;
        uint32_t revents = (uint32_t)(p->revents &
                                      ((short)FDK_EXTFD_POLL_MASK |
                                       POLLERR | POLLHUP | POLLNVAL));
        if (revents == 0) {
            continue;
        }
        due[w].src = src;
        due[w].revents = revents;
        w++;
    }
    int fired = 0;
    for (size_t i = 0; i < due_n; i++) {
        fdk_ext_fd_source *src = due[i].src;
        if (!src->active || src->ctx != ctx) {
            continue; /* removed by a prior callback */
        }
        fdk_ext_fd_source *prev_firing = ctx->extfd_firing;
        ctx->extfd_firing = src;
        src->fn(src, due[i].revents, src->user_data);
        ctx->extfd_firing = prev_firing;
        if (!src->internal) {
            fired++; /* application sources count; FDK-internal ones
                      * are the pump's keep-waiting class */
        }
        if (!src->active) {
            /* Self-removal inside its callback (or remove called on
             * the firing handle): the record is still allocated —
             * the free is deferred to exactly here. */
            extfd_unlink_free(ctx, src);
        }
    }
    fdk_free(due);
    return fired;
}

void fdk__extfd_shutdown(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    for (size_t i = ctx->extfd_count; i > 0; i--) {
        fdk_ext_fd_source *src = ctx->extfds[i - 1];
        if (src == ctx->extfd_firing) {
            /* Teardown reached from INSIDE a firing callback (quit ->
             * shutdown from the callback): the fire loop still holds
             * this record and frees it after the callback returns —
             * deactivate only. Defensive: the sanctioned pattern is
             * to quit and shut down from the pump's caller. */
            src->active = false;
            continue;
        }
        src->active = false;
        extfd_unlink_free(ctx, src);
    }
    fdk_free(ctx->extfds);
    ctx->extfds = NULL;
    ctx->extfd_capacity = 0;
}
