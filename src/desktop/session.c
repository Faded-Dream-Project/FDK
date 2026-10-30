/*
 * session.c — the session participation public half (1.1.0, D-5)
 *
 * The thin wrapper over the backend's session_* ops (see
 * platform_internal.h's D-5 block for the seam's contract):
 * validation, the objectLESS X11 lane's degradation posture, and
 * the id/state accessor copies — the toplevel.c discipline applied
 * to the save/restore desktop contract (include/fdk/fdk_session.h
 * is the contract's own prose).
 *
 * X11 needs no wrapper machinery at all: its session lane is the
 * automatic WM_COMMAND + WM_SAVE_YOURSELF half (fdk_window_set_
 * command + FDK_EVENT_SESSION_SAVE, both handled inside the X11
 * backend with no per-session state), so ops->session_open being
 * NULL there makes open() answer the documented NULL — the
 * session-absent branch, never an error. Wayland without the
 * xdg-session-management global degrades identically.
 *
 * The notify thunk: backends fire the coarse callback with THEIR
 * user data; the wrapper's own notify closure re-enters the public
 * callback with the public object — and survives the application
 * destroying the session inside it (the backend's dead-guard makes
 * the teardown safe, the wrapper's `closing` flag makes the second
 * entry a no-op: the same self-close discipline as the toplevel
 * list's).
 */

#define FDK_LOG_TAG "session"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "platform/platform_internal.h"
#include "window/window_internal.h" /* fdk_window: pwindow + ops (the join seam) */

#include "fdk/fdk_session.h"

#include <string.h>

struct fdk_session {
    fdk_context *ctx;
    struct fdk_platform_session *backend;
    fdk_session_notify_fn on_notify;
    void *on_notify_user;
    int closing; /* destroy-inside-notify deferral flag              */
};

/* The backend's coarse notify → the application's coarse notify.
 * Runs between events (the pump's callback discipline), so the
 * application may query state/id, act, or DESTROY the session —
 * the closing flag keeps a teardown mid-notification from
 * re-entering freed wrapper memory. */
static void session_backend_notify(void *user) {
    fdk_session *session = user;
    if (session == NULL || session->on_notify == NULL) {
        return;
    }
    if (session->closing) {
        return; /* torn down; the backend flushes with the tracker */
    }
    session->on_notify(session, session->on_notify_user);
}

fdk_session *fdk_session_open(fdk_context *ctx,
                              fdk_session_reason reason,
                              const char *restore_id,
                              fdk_session_notify_fn on_notify,
                              void *user) {
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return NULL;
    }
    if ((int)reason < FDK_SESSION_REASON_LAUNCH ||
        (int)reason > FDK_SESSION_REASON_RESTORE) {
        return NULL; /* the enum is wire values; anything else is a
                      * caller bug — NULL, the session-absent branch */
    }
    if (ctx->ops->session_open == NULL) {
        /* X11 always (the objectLESS WM_SAVE_YOURSELF lane needs no
         * session object); the public header documents the NULL. */
        return NULL;
    }
    fdk_session *session = fdk_alloc(sizeof(*session));
    if (session == NULL) {
        return NULL;
    }
    session->ctx = ctx;
    session->backend = NULL;
    session->on_notify = on_notify;
    session->on_notify_user = user;
    session->closing = 0;

    fdk_result r = ctx->ops->session_open(
        ctx->conn, (int)reason, restore_id, session_backend_notify,
        session, &session->backend);
    if (!fdk_ok(r)) {
        /* Wayland without the xdg-session-management global: the
         * honest degradation — NULL, not an error code (the caller's
         * NULL check IS the no-restore-on-this-desktop branch). */
        fdk_free(session);
        return NULL;
    }
    FDK_DEBUG("session opened (reason %d, restore %s)",
              (int)reason, restore_id != NULL ? "requested" : "fresh");
    return session;
}

void fdk_session_destroy(fdk_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->closing) {
        return; /* already tearing down (self-close discipline)     */
    }
    session->closing = 1;
    if (session->ctx != NULL && session->ctx->ops != NULL &&
        session->ctx->ops->session_destroy != NULL) {
        session->ctx->ops->session_destroy(session->backend);
    }
    fdk_free(session);
}

void fdk_session_remove(fdk_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->closing) {
        return;
    }
    session->closing = 1;
    if (session->ctx != NULL && session->ctx->ops != NULL &&
        session->ctx->ops->session_remove != NULL) {
        session->ctx->ops->session_remove(session->backend);
    }
    fdk_free(session);
}

const char *fdk_session_id(const fdk_session *session) {
    if (session == NULL || session->ctx == NULL ||
        session->ctx->ops == NULL || session->ctx->ops->session_id == NULL) {
        return NULL;
    }
    return session->ctx->ops->session_id(session->backend);
}

fdk_session_state fdk_session_get_state(const fdk_session *session) {
    if (session == NULL || session->ctx == NULL ||
        session->ctx->ops == NULL ||
        session->ctx->ops->session_state == NULL) {
        return FDK_SESSION_STATE_PENDING;
    }
    return (fdk_session_state)session->ctx->ops->session_state(
        session->backend);
}

fdk_result fdk_window_join_session(fdk_window *window,
                                   fdk_session *session,
                                   const char *name) {
    if (window == NULL || session == NULL || name == NULL ||
        name[0] == '\0') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (session->ctx == NULL || session->ctx->ops == NULL ||
        session->ctx->ops->window_join_session == NULL) {
        /* X11: the objectLESS lane — the header documents the
         * refusal (WM_COMMAND + WM_SAVE_YOURSELF need no join). */
        return FDK_ERR_UNSUPPORTED;
    }
    if (session->closing || session->backend == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    return session->ctx->ops->window_join_session(window->pwindow,
                                                  session->backend, name);
}

void fdk_window_leave_session(fdk_window *window) {
    if (window == NULL || window->ops == NULL ||
        window->ops->window_leave_session == NULL) {
        return;
    }
    window->ops->window_leave_session(window->pwindow);
}
