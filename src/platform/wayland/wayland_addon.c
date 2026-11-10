/* wayland_addon.c — P3: the add-on raw-event surface
 *
 * libwayland's queue mechanism, first-class: an add-on creates its
 * OWN event queue (fdk_wl_queue_create — the returned handle carries
 * a wrapped display + a private registry bound to that queue), binds
 * any advertised global onto it with fdk_wl_raw_bind (the raw-event
 * hook: the add-on sets listeners on the returned proxy and owns the
 * protocol conversation end to end), and either dispatches the queue
 * itself (fdk_wl_queue_dispatch) or attaches it to FDK's pump
 * (fdk_wl_queue_attach), which then dispatches the queue's pending
 * events after the main round each frame.
 *
 * Pump integration is safe by construction: dispatch goes to the
 * LISTENERS THE ADD-ON SET on its own proxies — FDK never sees those
 * objects, so there are no unknown opcodes to trip its handlers.
 * Read-side discipline stays with FDK's pump (prepare_read/
 * read_events route events to their proxies' queues at read time).
 */
#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"
#include "core/alloc_internal.h"

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <string.h>

static void queue_destroy_impl(struct fdk_wl_queue *q);
static void *queue_registry_impl(struct fdk_wl_queue *q);
static void *queue_raw_bind_impl(struct fdk_wl_queue *q, fdk_u32 name,
                                 const void *iface, fdk_u32 version);
static int queue_dispatch_impl(struct fdk_wl_queue *q);
static void queue_attach_impl(struct fdk_wl_queue *q, int on_pump);

/* wl_display_create_queue_with_name arrived in libwayland 1.23;
 * runners and distros on 1.22 (ubuntu-24.04, debian stable) lack it.
 * The weak ref is NULL there — the unnamed queue is the honest
 * fallback (the name is a debugging aid, not a protocol fact). */
__attribute__((weak)) struct wl_event_queue *
wl_display_create_queue_with_name(struct wl_display *display,
                                  const char *name);

void *fdk_wayland_queue_create(fdk_platform_connection *conn,
                               const char *name) {
    if (conn == NULL || conn->display == NULL) {
        return NULL;
    }
    struct fdk_wl_queue *q = fdk_alloc(sizeof *q);
    if (q == NULL) {
        return NULL;
    }
    memset(q, 0, sizeof *q);
    if (name != NULL && wl_display_create_queue_with_name != NULL) {
        q->q = wl_display_create_queue_with_name(conn->display, name);
    } else {
        q->q = wl_display_create_queue(conn->display);
    }
    if (q->q == NULL) {
        fdk_free(q);
        return NULL;
    }
    q->wrapped = wl_proxy_create_wrapper(conn->display);
    if (q->wrapped == NULL) {
        wl_event_queue_destroy(q->q);
        fdk_free(q);
        return NULL;
    }
    wl_proxy_set_queue((struct wl_proxy *)q->wrapped, q->q);
    q->registry = wl_display_get_registry(q->wrapped);
    if (q->registry == NULL) {
        wl_proxy_wrapper_destroy((struct wl_proxy *)q->wrapped);
        wl_event_queue_destroy(q->q);
        fdk_free(q);
        return NULL;
    }
    q->real = conn->display;
    q->conn = conn;
    q->do_destroy = queue_destroy_impl;
    q->get_registry = queue_registry_impl;
    q->do_raw_bind = queue_raw_bind_impl;
    q->do_dispatch = queue_dispatch_impl;
    q->do_attach = queue_attach_impl;
    q->next = conn->addon_queues;
    conn->addon_queues = q;
    return q;
}

void fdk_wayland_queue_destroy(fdk_platform_connection *conn,
                               void *handle) {
    struct fdk_wl_queue *q = handle;
    if (conn == NULL || q == NULL) {
        return;
    }
    struct fdk_wl_queue **p = &conn->addon_queues;
    while (*p != NULL && *p != q) {
        p = &(*p)->next;
    }
    if (*p == q) {
        *p = q->next;
    }
    /* the registry is a NORMAL proxy (created on the wrapper);
     * only the display wrapper takes wrapper_destroy */
    wl_proxy_destroy((struct wl_proxy *)q->registry);
    wl_proxy_wrapper_destroy((struct wl_proxy *)q->wrapped);
    wl_event_queue_destroy(q->q);
    fdk_free(q);
}

static void *queue_raw_bind_impl(struct fdk_wl_queue *q, fdk_u32 name,
                                 const void *iface, fdk_u32 version) {
    if (q == NULL || q->registry == NULL || iface == NULL) {
        return NULL;
    }
    /* the private registry sits on the add-on's queue, so the new
     * proxy (and every event it ever emits) lands there too */
    return wl_registry_bind(q->registry, name,
                            (const struct wl_interface *)iface, version);
}

static void *queue_registry_impl(struct fdk_wl_queue *q) {
    return q != NULL ? (void *)q->registry : NULL;
}

static int queue_dispatch_impl(struct fdk_wl_queue *q) {
    if (q == NULL || q->q == NULL) {
        return 0;
    }
    /* the add-on's listeners only; non-blocking */
    return wl_display_dispatch_queue_pending(q->real, q->q);
}

static void queue_attach_impl(struct fdk_wl_queue *q, int on_pump) {
    if (q != NULL) {
        q->on_pump = on_pump ? 1 : 0;
    }
}

static void queue_destroy_impl(struct fdk_wl_queue *q) {
    if (q == NULL || q->conn == NULL) {
        return;
    }
    struct fdk_wl_queue **p = &q->conn->addon_queues;
    while (*p != NULL && *p != q) {
        p = &(*p)->next;
    }
    if (*p == q) {
        *p = q->next;
    }
    /* the registry is a NORMAL proxy (created on the wrapper);
     * only the display wrapper takes wrapper_destroy */
    wl_proxy_destroy((struct wl_proxy *)q->registry);
    wl_proxy_wrapper_destroy((struct wl_proxy *)q->wrapped);
    wl_event_queue_destroy(q->q);
    fdk_free(q);
}

/* the public entry points (extend.c) call the impl through the queue */
void fdk_wayland_queue_destroy_pub(struct fdk_wl_queue *q) {
    if (q != NULL && q->do_destroy != NULL) {
        q->do_destroy(q);
    }
}

/* The pump's tail: dispatch every pump-attached add-on queue once. */
void fdk_wayland_addon_queues_dispatch(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    for (struct fdk_wl_queue *q = conn->addon_queues; q != NULL;
         q = q->next) {
        if (q->on_pump) {
            (void)wl_display_dispatch_queue_pending(conn->display, q->q);
        }
    }
}


/* ---- the public entry points (declared in fdk/fdk_extend.h) -----------
 * Defined HERE because the queue's type is internal to the wayland
 * layer; wayland_disabled.c provides the stubs for the other build. */
fdk_wl_queue *fdk_wl_queue_create_pub(fdk_platform_connection *conn,
                                      const char *name) {
    return (fdk_wl_queue *)fdk_wayland_queue_create(conn, name);
}

void fdk_wl_queue_destroy(fdk_wl_queue *queue) {
    if (queue != NULL && queue->do_destroy != NULL) {
        queue->do_destroy(queue);
    }
}

void *fdk_wl_queue_registry(fdk_wl_queue *queue) {
    return queue != NULL && queue->get_registry != NULL
               ? queue->get_registry(queue)
               : NULL;
}

void *fdk_wl_raw_bind(fdk_wl_queue *queue, fdk_u32 name,
                      const void *wl_interface, fdk_u32 version) {
    return queue != NULL && queue->do_raw_bind != NULL
               ? queue->do_raw_bind(queue, name, wl_interface, version)
               : NULL;
}

int fdk_wl_queue_dispatch(fdk_wl_queue *queue) {
    return queue != NULL && queue->do_dispatch != NULL
               ? queue->do_dispatch(queue)
               : 0;
}

void fdk_wl_queue_attach(fdk_wl_queue *queue, int on_pump) {
    if (queue != NULL && queue->do_attach != NULL) {
        queue->do_attach(queue, on_pump);
    }
}
