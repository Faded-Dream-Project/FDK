/* wayland_session_lock.c — P3: ext-session-lock-v1 first-class.
 *
 * The lock-screen protocol: ext_session_lock_manager_v1.get_lock()
 * mints a lock object; the compositor answers finished (the lock was
 * taken by someone else — the add-on must destroy and give up) or
 * locked (exclusive access granted). While locked, the add-on maps
 * one lock surface PER OUTPUT (get_lock_surface on the output's
 * wl_surface); destroying the lock object unlocks.
 *
 * Events bridge into FDK via the callback below (locked/finished).
 * UNSUPPORTED paths: no manager global (cap false), and finished.
 */
#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"
#include "core/alloc_internal.h"

#include "generated/ext-session-lock-v1-client-protocol.h"

#include <string.h>

typedef void (*fdk_session_lock_event_fn)(void *user, int locked);

struct session_lock_state {
    struct ext_session_lock_v1 *lock;
    fdk_session_lock_event_fn on_event;
    void *user;
};

static void sl_locked(void *data, struct ext_session_lock_v1 *lock) {
    struct session_lock_state *st = data;
    (void)lock;
    if (st->on_event != NULL) {
        st->on_event(st->user, 1);
    }
}

static void sl_finished(void *data, struct ext_session_lock_v1 *lock) {
    struct session_lock_state *st = data;
    (void)lock;
    if (st->on_event != NULL) {
        st->on_event(st->user, 0);
    }
}

static const struct ext_session_lock_v1_listener sl_listener = {
    .locked = sl_locked,
    .finished = sl_finished,
};

void *fdk_wayland_session_lock(fdk_platform_connection *conn,
                               void (*on_event)(void *, int),
                               void *user) {
    if (conn == NULL || conn->session_lock_manager == NULL) {
        return NULL; /* the honest no-manager path */
    }
    struct session_lock_state *st = fdk_alloc(sizeof *st);
    if (st == NULL) {
        return NULL;
    }
    memset(st, 0, sizeof *st);
    st->on_event = on_event;
    st->user = user;
    st->lock = ext_session_lock_manager_v1_lock(
        conn->session_lock_manager);
    if (st->lock == NULL) {
        fdk_free(st);
        return NULL;
    }
    ext_session_lock_v1_add_listener(st->lock, &sl_listener, st);
    return st;
}

/* Maps the lock surface for one output: a zero-sized surface on the
 * output; the compositor configures it to the output's size (the
 * protocol: the client acks and commits a buffer of the given size). */
void *fdk_wayland_session_lock_surface(
    fdk_platform_connection *conn, void *handle, void *wl_surface,
    void *wl_output) {
    struct session_lock_state *st = handle;
    if (conn == NULL || st == NULL || st->lock == NULL ||
        wl_surface == NULL) {
        return NULL;
    }
    return (void *)ext_session_lock_v1_get_lock_surface(
        st->lock, (struct wl_surface *)wl_surface,
        (struct wl_output *)wl_output);
}

void fdk_wayland_session_lock_unlock(void *handle) {
    struct session_lock_state *st = handle;
    if (st == NULL || st->lock == NULL) {
        return;
    }
    ext_session_lock_v1_unlock_and_destroy(st->lock);
    st->lock = NULL;
    fdk_free(st);
}


/* the extend.c entry routes here (the conn rides the state) */
void fdk_session_lock_unlock_impl(void *handle) {
    fdk_wayland_session_lock_unlock(handle);
}
