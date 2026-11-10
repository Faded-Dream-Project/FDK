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
#include <wayland-client-protocol.h> /* wl_compositor/wl_surface */

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

/* The configure listener for FDK-MANAGED lock surfaces (the
 * first-class kind): the compositor sizes the surface to the output;
 * FDK routes it through the ext-surface configure flow (the same
 * path an add-on's notify_configure drives), which resizes the
 * render machinery and delivers the configure event. */
struct managed_surface {
    struct ext_session_lock_surface_v1 *proxy;
    fdk_ext_surface *es;
};

static void ms_configure(void *data,
                         struct ext_session_lock_surface_v1 *proxy,
                         uint32_t serial, uint32_t width,
                         uint32_t height) {
    (void)proxy;
    struct managed_surface *ms = data;
    ext_session_lock_surface_v1_ack_configure(proxy, serial);
    if (ms != NULL && ms->es != NULL) {
        fdk_ext_surface_notify_configure(
            ms->es, (fdk_i32)width, (fdk_i32)height);
    }
}

static const struct ext_session_lock_surface_v1_listener ms_listener = {
    .configure = ms_configure,
};

/* The FDK-MANAGED lock surface: FDK creates the wl_surface, attaches
 * the role on the connection's FIRST bound output, installs the
 * configure listener, and hands the raw surface to FDK's ext-surface
 * adoption (out_native). A lock surface needs a ROLE-LESS surface —
 * the ext-surface native accessor's surfaces already carry the layer
 * role and CANNOT take this one (the compositor kills the
 * connection: protocol error 2, the wall this API exists for). */
fdk_result fdk_wayland_session_lock_surface_managed(
    fdk_platform_connection *conn, void *handle, void *es,
    void **out_native) {
    struct session_lock_state *st = handle;
    if (conn == NULL || st == NULL || st->lock == NULL ||
        es == NULL || out_native == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->compositor == NULL || conn->output_count < 1 ||
        conn->outputs[0].output == NULL) {
        return FDK_ERR_UNSUPPORTED; /* no compositor/output to lock */
    }
    struct managed_surface *ms = fdk_alloc(sizeof *ms);
    if (ms == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    ms->es = (fdk_ext_surface *)es;
    ms->proxy = NULL;
    struct wl_surface *surface =
        wl_compositor_create_surface(conn->compositor);
    if (surface == NULL) {
        fdk_free(ms);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    ms->proxy = ext_session_lock_v1_get_lock_surface(
        st->lock, surface, conn->outputs[0].output);
    if (ms->proxy == NULL) {
        wl_surface_destroy(surface);
        fdk_free(ms);
        return FDK_ERR_UNSUPPORTED;
    }
    ext_session_lock_surface_v1_add_listener(ms->proxy, &ms_listener,
                                             ms);
    *out_native = (void *)surface;
    return FDK_OK;
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
