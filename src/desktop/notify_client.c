#define FDK_LOG_TAG "notify"
/*
 * notify_client.c — the app-facing notification client (1.1.0)
 *
 * The APP side of org.freedesktop.Notifications on the native
 * D-Bus client: Notify / CloseNotification / GetCapabilities /
 * GetServerInformation plus NotificationClosed and ActionInvoked,
 * delivered as C callbacks on the FDK event loop. The public
 * contract is include/fdk/fdk_notify.h — read that first; this
 * file is the machinery under it.
 *
 * The state machine, drawn once:
 *
 *   open ──connect──▶ CONNECTING ──ready──▶ watch the daemon name
 *                      │                        │            │
 *              (ok=0)  ▼                 owner appeared   owner NULL
 *                   NO_BUS ◀──death──  ┌────────────┐      ▼
 *                      │        probe  │ NO_DAEMON  │   (a send may
 *                      ▼        caps   └─────┬──────┘    still reach a
 *                (throttled              adopts│           bus-activated
 *                 retry on send)               ▼           daemon)
 *                                        READY ◀── caps reply ──┘
 *
 * Sends ride straight out whenever a CONNECTION exists (the bus
 * queues pre-ready messages itself; a NO_DAEMON send still goes
 * out because the bus may service-activate a daemon — the
 * desktop's intended flow). Only the NO_BUS window queues locally
 * (bounded, in order, flushed the moment a connection answers).
 *
 * Robustness against the daemon: every reply is shape-checked
 * before it is believed; events carry ids we map back to the
 * app's note_user or honestly NULL (the bounded map's eviction).
 * Nothing here can block: every socket operation is the event
 * loop's; every timeout is the pending-call machinery's.
 */

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_notify.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- constants ----------------------------------------------------------- */

#define RETRY_INTERVAL_MS 30000u
#define QUEUE_MAX 64
#define ID_MAP_MAX 256
#define MAX_IMAGE_DIM 512

/* ---- small helpers -------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static char *xstrdup_n(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *c = fdk_alloc(n);
    if (c != NULL) {
        memcpy(c, s, n);
    }
    return c;
}

/* The a{sv} building block: one dict entry with a VARIANT-wrapped
 * child. new_variant CLONES its child; new_dict_entry TAKES both
 * sides — free only the failure-path leftovers. */
static fdk_dbus_value *sv_wrap(const char *key,
                               fdk_dbus_value *inner_owned) {
    if (inner_owned == NULL) {
        return NULL;
    }
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *var = fdk_dbus_value_new_variant(inner_owned);
    fdk_dbus_value_free(inner_owned); /* the variant holds its clone */
    if (k == NULL || var == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
        return NULL;
    }
    fdk_dbus_value *entry = fdk_dbus_value_new_dict_entry(k, var);
    if (entry == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
        return NULL;
    }
    return entry;
}

/* ---- the queued send (the NO_BUS window) --------------------------------- */

typedef struct {
    fdk_notify_request req; /* every string deep-owned */
    void *note_user;
} queued_send;

static void request_clear(queued_send *q) {
    fdk_free((void *)(uintptr_t)q->req.app_name);
    fdk_free((void *)(uintptr_t)q->req.icon);
    fdk_free((void *)(uintptr_t)q->req.summary);
    fdk_free((void *)(uintptr_t)q->req.body);
    fdk_free((void *)(uintptr_t)q->req.category);
    fdk_free((void *)(uintptr_t)q->req.desktop_entry);
    if (q->req.actions != NULL) {
        for (size_t i = 0; q->req.actions[i] != NULL; i++) {
            fdk_free((void *)(uintptr_t)q->req.actions[i]);
        }
        fdk_free((void *)(uintptr_t)q->req.actions);
    }
    fdk_free((void *)(uintptr_t)q->req.image_data);
    memset(q, 0, sizeof(*q));
}

static char *dup_or_null(const char *s) {
    return xstrdup_n(s); /* NULL in, NULL out */
}

static int request_copy(queued_send *q, const fdk_notify_request *req,
                        void *note_user) {
    memset(q, 0, sizeof(*q));
    q->req.urgency = req->urgency;
    q->req.replaces_id = req->replaces_id;
    q->req.timeout_ms = req->timeout_ms;
    q->req.image_width = req->image_width;
    q->req.image_height = req->image_height;
    q->req.image_rowstride = req->image_rowstride;
    q->req.image_has_alpha = req->image_has_alpha;
    q->note_user = note_user;
    q->req.app_name = dup_or_null(req->app_name);
    q->req.icon = dup_or_null(req->icon);
    q->req.summary = dup_or_null(req->summary);
    q->req.body = dup_or_null(req->body);
    q->req.category = dup_or_null(req->category);
    q->req.desktop_entry = dup_or_null(req->desktop_entry);
    if (q->req.summary == NULL && req->summary != NULL) {
        request_clear(q);
        return 0;
    }
    if (req->actions != NULL) {
        size_t n = 0;
        while (req->actions[n] != NULL) {
            n++;
        }
        const char **acts = fdk_alloc_array(n + 1, sizeof(char *));
        if (acts == NULL) {
            request_clear(q);
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            acts[i] = dup_or_null(req->actions[i]);
            if (acts[i] == NULL) {
                for (size_t k = 0; k < i; k++) {
                    fdk_free((void *)(uintptr_t)acts[k]);
                }
                fdk_free(acts);
                request_clear(q);
                return 0;
            }
        }
        acts[n] = NULL;
        q->req.actions = acts;
    }
    if (req->image_data != NULL && req->image_width > 0 &&
        req->image_height > 0) {
        int32_t ch = req->image_has_alpha ? 4 : 3;
        int32_t rs = req->image_rowstride > 0 ? req->image_rowstride
                                              : req->image_width * ch;
        size_t need = (size_t)rs * (size_t)(req->image_height - 1) +
                      (size_t)req->image_width * (size_t)ch;
        uint8_t *img = fdk_alloc_array(need, 1);
        if (img == NULL) {
            request_clear(q);
            return 0;
        }
        memcpy(img, req->image_data, need);
        q->req.image_data = img;
        q->req.image_rowstride = rs;
    } else {
        q->req.image_data = NULL;
    }
    return 1;
}

/* ---- the client ----------------------------------------------------------- */

typedef struct {
    fdk_notify *n;
    void *note_user;
} send_cell;

struct fdk_notify {
    fdk_context *ctx;
    fdk_notify_callbacks cb;

    fdk_dbus_connection *bus; /* owned; NULL in NO_BUS */
    fdk_notify_status status;
    int probing; /* capabilities/server-info in flight */
    fdk_dbus_name_watcher *watch;
    fdk_dbus_subscription *events;

    char **caps;         /* owned, NULL-terminated */
    char *server_name;   /* owned */
    char *server_vendor; /* owned */

    queued_send queue[QUEUE_MAX];
    size_t queue_len;

    struct {
        uint32_t id; /* 0 = empty slot (daemons never hand out 0) */
        void *note_user;
    } ids[ID_MAP_MAX];
    size_t ids_head; /* ring insertion point */
    size_t ids_len;

    uint64_t last_attempt;
    int tearing_down;
};

/* ---- id map --------------------------------------------------------------- */

static void id_map_put(fdk_notify *n, uint32_t id, void *note_user) {
    if (id == 0) {
        return;
    }
    /* Full: evict the OLDEST mapping (documented in the header —
     * its later events arrive with a NULL note_user). */
    if (n->ids_len == ID_MAP_MAX) {
        size_t oldest = (n->ids_head + 1) % ID_MAP_MAX;
        n->ids[oldest].id = 0;
        n->ids_len--;
    }
    n->ids[n->ids_head].id = id;
    n->ids[n->ids_head].note_user = note_user;
    n->ids_head = (n->ids_head + 1) % ID_MAP_MAX;
    n->ids_len++;
}

static void *id_map_take(fdk_notify *n, uint32_t id) {
    for (size_t i = 0; i < ID_MAP_MAX; i++) {
        if (n->ids[i].id == id) {
            void *u = n->ids[i].note_user;
            n->ids[i].id = 0;
            n->ids[i].note_user = NULL;
            n->ids_len--;
            return u;
        }
    }
    return NULL;
}

/* Non-destructive read: ActionInvoked must NOT drop the mapping —
 * the daemon still sends NotificationClosed afterwards. */
static void *id_map_get(const fdk_notify *n, uint32_t id) {
    for (size_t i = 0; i < ID_MAP_MAX; i++) {
        if (n->ids[i].id == id) {
            return n->ids[i].note_user;
        }
    }
    return NULL;
}

/* ---- daemon adoption / loss ----------------------------------------------- */

static void on_daemon_event(fdk_dbus_connection *bus,
                            fdk_dbus_message *sig, void *user);

static void drop_daemon(fdk_notify *n) {
    if (n->events != NULL) {
        fdk_dbus_subscription_free(n->events);
        n->events = NULL;
    }
    if (n->caps != NULL) {
        for (size_t i = 0; n->caps[i] != NULL; i++) {
            fdk_free(n->caps[i]);
        }
        fdk_free(n->caps);
        n->caps = NULL;
    }
    fdk_free(n->server_name);
    n->server_name = NULL;
    fdk_free(n->server_vendor);
    n->server_vendor = NULL;
    if (n->status == FDK_NOTIFY_STATUS_READY) {
        n->status = FDK_NOTIFY_STATUS_NO_DAEMON;
    }
}

static void on_caps_reply(fdk_dbus_connection *bus, fdk_dbus_message *reply,
                          void *user);

static void on_serverinfo_reply(fdk_dbus_connection *bus,
                                fdk_dbus_message *reply, void *user);

static void adopt_owner(fdk_notify *n, const char *owner) {
    if (n->bus == NULL) {
        return;
    }
    /* The event subscription: scoped to the daemon's UNIQUE name
     * (the header value the matcher sees), interface-wide, then
     * dispatched by member. */
    char *rule = fdk_dbus_match_rule("signal", owner,
                                     "org.freedesktop.Notifications", NULL,
                                     NULL, NULL);
    if (rule == NULL) {
        return;
    }
    n->events = fdk_dbus_connection_subscribe(n->bus, rule,
                                              on_daemon_event, n);
    free(rule);
    if (n->events == NULL) {
        FDK_WARN("notify: event subscription refused — closed/action "
                 "events will not arrive");
    }
    /* Probe: capabilities (drives READY) + server information. */
    n->probing = 2;
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "GetCapabilities");
    if (call != NULL) {
        if (fdk_dbus_connection_call(n->bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                     on_caps_reply, n) == NULL) {
            fdk_dbus_message_free(call);
            n->probing--;
        }
    } else {
        n->probing--;
    }
    call = fdk_dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "GetServerInformation");
    if (call != NULL) {
        if (fdk_dbus_connection_call(n->bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                     on_serverinfo_reply, n) == NULL) {
            fdk_dbus_message_free(call);
            n->probing--;
        }
    } else {
        n->probing--;
    }
}

static void on_owner_event(fdk_dbus_connection *bus, const char *name,
                           const char *owner, void *user) {
    (void)bus;
    (void)name;
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    if (owner == NULL) {
        FDK_INFO("notify: no notification daemon on the bus");
        drop_daemon(n);
        n->status = FDK_NOTIFY_STATUS_NO_DAEMON;
        return;
    }
    drop_daemon(n); /* a replacement daemon: start its adoption clean */
    FDK_INFO("notify: notification daemon on the bus (%s)", owner);
    n->status = FDK_NOTIFY_STATUS_CONNECTING; /* probing */
    adopt_owner(n, owner);
}

/* ---- daemon events --------------------------------------------------------- */

/* ---- connection lifecycle ------------------------------------------------- */

static void on_ready(fdk_dbus_connection *bus, int ok, const char *error,
                     void *user) {
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    if (!ok) {
        FDK_INFO("notify: session bus connect failed (%s)",
                 error ? error : "?");
        n->status = FDK_NOTIFY_STATUS_NO_BUS;
        n->bus = NULL; /* the handle frees itself below */
        fdk_dbus_connection_close(bus);
        return;
    }
    n->watch = fdk_dbus_connection_watch_name(
        bus, "org.freedesktop.Notifications", on_owner_event, n);
    if (n->watch == NULL) {
        FDK_WARN("notify: daemon name watch refused — assuming none");
        n->status = FDK_NOTIFY_STATUS_NO_DAEMON;
    }
    /* The watch fires immediately with the current owner state; the
     * status transition happens there. */
}

static void on_death(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    FDK_INFO("notify: the bus connection died — back to NO_BUS");
    n->bus = NULL; /* the deferred close frees the handle */
    n->watch = NULL;
    n->events = NULL;
    n->probing = 0;
    if (n->caps != NULL) {
        for (size_t i = 0; n->caps[i] != NULL; i++) {
            fdk_free(n->caps[i]);
        }
        fdk_free(n->caps);
        n->caps = NULL;
    }
    fdk_free(n->server_name);
    n->server_name = NULL;
    fdk_free(n->server_vendor);
    n->server_vendor = NULL;
    n->status = FDK_NOTIFY_STATUS_NO_BUS;
}

static void try_connect(fdk_notify *n) {
    if (n->bus != NULL || n->ctx == NULL) {
        return;
    }
    uint64_t now = now_ms();
    if (n->last_attempt != 0 && now < n->last_attempt + RETRY_INTERVAL_MS) {
        return; /* throttled — never a retry storm */
    }
    n->last_attempt = now;
    n->status = FDK_NOTIFY_STATUS_CONNECTING;
    n->bus = fdk_dbus_connect(n->ctx, FDK_DBUS_SESSION_BUS, on_ready, n);
    if (n->bus == NULL) {
        n->status = FDK_NOTIFY_STATUS_NO_BUS;
        return;
    }
    fdk_dbus_connection_set_death_handler(n->bus, on_death, n);
}

/* ---- Notify: building the call -------------------------------------------- */

static fdk_dbus_value *build_image_struct(const fdk_notify_request *req) {
    int32_t ch = req->image_has_alpha ? 4 : 3;
    int32_t rs = req->image_rowstride > 0 ? req->image_rowstride
                                          : req->image_width * ch;
    size_t need = (size_t)rs * (size_t)(req->image_height - 1) +
                  (size_t)req->image_width * (size_t)ch;
    fdk_dbus_value *bytes =
        fdk_dbus_value_new_byte_array(req->image_data, need);
    fdk_dbus_value *fields[6] = {NULL};
    size_t got = 0;
    fdk_dbus_value *st = NULL;
    if (bytes == NULL) {
        return NULL;
    }
    fields[0] = fdk_dbus_value_new_int32(req->image_width);
    fields[1] = fdk_dbus_value_new_int32(req->image_height);
    fields[2] = fdk_dbus_value_new_int32(rs);
    fields[3] = fdk_dbus_value_new_boolean(req->image_has_alpha ? 1 : 0);
    fields[4] = fdk_dbus_value_new_int32(8);
    fields[5] = fdk_dbus_value_new_int32(ch);
    for (size_t i = 0; i < 6; i++) {
        if (fields[i] == NULL) {
            goto fail;
        }
        got++;
    }
    /* new_struct TAKES its children — the byte array rides along. */
    fdk_dbus_value *seven[7];
    for (size_t i = 0; i < 6; i++) {
        seven[i] = fields[i];
    }
    seven[6] = bytes;
    st = fdk_dbus_value_new_struct(seven, 7);
    if (st == NULL) {
        goto fail;
    }
    return st;
fail:
    for (size_t i = 0; i < got; i++) {
        fdk_dbus_value_free(fields[i]);
    }
    fdk_dbus_value_free(bytes);
    return NULL;
}

static fdk_dbus_value *build_hints(const fdk_notify_request *req) {
    fdk_dbus_value *entries[4];
    size_t n = 0;
    int failed = 0;

    { /* urgency always travels */
        fdk_dbus_value *u = fdk_dbus_value_new_byte((uint8_t)req->urgency);
        fdk_dbus_value *e = u != NULL ? sv_wrap("urgency", u) : NULL;
        if (e == NULL) {
            failed = 1;
        } else {
            entries[n++] = e;
        }
    }
    if (!failed && req->category != NULL) {
        fdk_dbus_value *c = fdk_dbus_value_new_string(req->category);
        fdk_dbus_value *e = c != NULL ? sv_wrap("category", c) : NULL;
        if (e == NULL) {
            failed = 1;
        } else {
            entries[n++] = e;
        }
    }
    if (!failed && req->desktop_entry != NULL) {
        fdk_dbus_value *d = fdk_dbus_value_new_string(req->desktop_entry);
        fdk_dbus_value *e = d != NULL ? sv_wrap("desktop-entry", d) : NULL;
        if (e == NULL) {
            failed = 1;
        } else {
            entries[n++] = e;
        }
    }
    if (!failed && req->image_data != NULL && req->image_width > 0 &&
        req->image_height > 0 && req->image_width <= MAX_IMAGE_DIM &&
        req->image_height <= MAX_IMAGE_DIM) {
        fdk_dbus_value *img = build_image_struct(req);
        fdk_dbus_value *e = img != NULL ? sv_wrap("image-data", img) : NULL;
        if (e == NULL) {
            failed = 1;
        } else {
            entries[n++] = e;
        }
    }
    if (failed) {
        for (size_t i = 0; i < n; i++) {
            fdk_dbus_value_free(entries[i]);
        }
        return NULL;
    }
    return fdk_dbus_value_new_array("{sv}", entries, n);
}

static fdk_dbus_value *build_actions(const fdk_notify_request *req) {
    if (req->actions == NULL) {
        return fdk_dbus_value_new_array("s", NULL, 0);
    }
    size_t n = 0;
    while (req->actions[n] != NULL) {
        n++;
    }
    if (n == 0) {
        return fdk_dbus_value_new_array("s", NULL, 0);
    }
    fdk_dbus_value **items = fdk_alloc_array(n, sizeof(*items));
    if (items == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        items[i] = fdk_dbus_value_new_string(req->actions[i]);
        if (items[i] == NULL) {
            for (size_t k = 0; k < i; k++) {
                fdk_dbus_value_free(items[k]);
            }
            fdk_free(items);
            return NULL;
        }
    }
    fdk_dbus_value *arr = fdk_dbus_value_new_array("s", items, n);
    /* new_array TAKES the values; the items ARRAY is ours. */
    fdk_free(items);
    return arr;
}

static void send_now(fdk_notify *n, const fdk_notify_request *req,
                     void *note_user);

static void flush_queue(fdk_notify *n) {
    while (n->queue_len > 0 && n->bus != NULL) {
        queued_send q = n->queue[0];
        memmove(n->queue, n->queue + 1,
                (n->queue_len - 1) * sizeof(*n->queue));
        n->queue_len--;
        send_now(n, &q.req, q.note_user);
        request_clear(&q);
    }
    if (n->queue_len > 0) {
        /* Still no connection: the sends stay queued; the caller's
         * next send retries the bus (throttled). */
        FDK_TRACE("notify: %zu sends still queued (no bus)",
                  n->queue_len);
    }
}

/* ---- replies --------------------------------------------------------------- */

static void on_notify_reply(fdk_dbus_connection *bus,
                            fdk_dbus_message *reply, void *user) {
    (void)bus;
    send_cell *cell = user;
    fdk_notify *n = cell->n;
    void *note_user = cell->note_user;
    fdk_free(cell);

    if (n->tearing_down) {
        return;
    }
    if (reply == NULL) {
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "no reply (timeout or bus loss)",
                            n->cb.user);
        }
        return;
    }
    if (fdk_dbus_message_is_error(reply)) {
        if (n->status == FDK_NOTIFY_STATUS_READY) {
            /* A READY daemon answered with an error: report it,
             * keep the status (the daemon is real). */
        }
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user,
                            fdk_dbus_message_error_name(reply),
                            n->cb.user);
        }
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(reply, &args, &argc_)) ||
        argc_ != 1 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_UINT32) {
        fdk_dbus_values_free_array(args, argc_);
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "malformed Notify reply",
                            n->cb.user);
        }
        return;
    }
    uint32_t id = fdk_dbus_value_uint32(args[0]);
    fdk_dbus_values_free_array(args, argc_);
    id_map_put(n, id, note_user);
    if (n->cb.on_sent != NULL) {
        n->cb.on_sent(n, id, note_user, n->cb.user);
    }
}

static void on_caps_reply(fdk_dbus_connection *bus, fdk_dbus_message *reply,
                          void *user) {
    (void)bus;
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    n->probing = n->probing > 0 ? n->probing - 1 : 0;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        if (n->probing == 0 && n->status != FDK_NOTIFY_STATUS_READY) {
            n->status = FDK_NOTIFY_STATUS_NO_DAEMON;
        }
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(reply, &args, &argc_)) ||
        argc_ != 1 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_ARRAY) {
        fdk_dbus_values_free_array(args, argc_);
        if (n->probing == 0 && n->status != FDK_NOTIFY_STATUS_READY) {
            n->status = FDK_NOTIFY_STATUS_NO_DAEMON;
        }
        return;
    }
    const fdk_dbus_value *arr = args[0];
    size_t count = fdk_dbus_value_array_count(arr);
    char **caps = fdk_alloc_array(count + 1, sizeof(char *));
    if (caps == NULL) {
        fdk_dbus_values_free_array(args, argc_);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const char *s = fdk_dbus_value_string(
            fdk_dbus_value_array_item(arr, i));
        caps[i] = xstrdup_n(s != NULL ? s : "");
        if (caps[i] == NULL) {
            for (size_t k = 0; k < i; k++) {
                fdk_free(caps[k]);
            }
            fdk_free(caps);
            fdk_dbus_values_free_array(args, argc_);
            return;
        }
    }
    caps[count] = NULL;
    fdk_dbus_values_free_array(args, argc_);
    if (n->caps != NULL) {
        for (size_t i = 0; n->caps[i] != NULL; i++) {
            fdk_free(n->caps[i]);
        }
        fdk_free(n->caps);
    }
    n->caps = caps;
    if (n->status != FDK_NOTIFY_STATUS_READY) {
        n->status = FDK_NOTIFY_STATUS_READY;
        FDK_INFO("notify: daemon ready (%zu capabilities)", count);
        if (n->cb.on_ready != NULL) {
            n->cb.on_ready(n, n->cb.user);
        }
    }
}

static void on_serverinfo_reply(fdk_dbus_connection *bus,
                                fdk_dbus_message *reply, void *user) {
    (void)bus;
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    n->probing = n->probing > 0 ? n->probing - 1 : 0;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(reply, &args, &argc_)) ||
        argc_ != 4 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[1]) != FDK_DBUS_STRING) {
        fdk_dbus_values_free_array(args, argc_);
        return;
    }
    /* Borrowed strings die with the args array — copy first. */
    char *name = xstrdup_n(fdk_dbus_value_string(args[0]));
    char *vendor = xstrdup_n(fdk_dbus_value_string(args[1]));
    fdk_dbus_values_free_array(args, argc_);
    if (name == NULL || vendor == NULL) {
        fdk_free(name);
        fdk_free(vendor);
        return;
    }
    fdk_free(n->server_name);
    fdk_free(n->server_vendor);
    n->server_name = name;
    n->server_vendor = vendor;
    FDK_INFO("notify: daemon '%s' by '%s'", name, vendor);
}

/* ---- daemon events --------------------------------------------------------- */

static void on_daemon_event(fdk_dbus_connection *bus,
                            fdk_dbus_message *sig, void *user) {
    (void)bus;
    fdk_notify *n = user;
    if (n->tearing_down) {
        return;
    }
    const char *member = fdk_dbus_message_member(sig);
    if (member == NULL) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t argc_ = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(sig, &args, &argc_)) ||
        args == NULL || argc_ < 2) {
        fdk_dbus_values_free_array(args, argc_);
        return;
    }
    if (strcmp(member, "NotificationClosed") == 0 &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_UINT32) {
        uint32_t id = fdk_dbus_value_uint32(args[0]);
        uint32_t reason = fdk_dbus_value_uint32(args[1]);
        void *note_user = id_map_take(n, id);
        fdk_dbus_values_free_array(args, argc_);
        if (n->cb.on_closed != NULL) {
            n->cb.on_closed(n, id, reason, note_user, n->cb.user);
        }
        return;
    }
    if (strcmp(member, "ActionInvoked") == 0 &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        uint32_t id = fdk_dbus_value_uint32(args[0]);
        char *action = xstrdup_n(fdk_dbus_value_string(args[1]));
        fdk_dbus_values_free_array(args, argc_);
        if (action == NULL) {
            return;
        }
        void *note_user = id_map_get(n, id);
        if (n->cb.on_action != NULL) {
            n->cb.on_action(n, id, action, note_user, n->cb.user);
        }
        fdk_free(action);
        return;
    }
    fdk_dbus_values_free_array(args, argc_);
}

/* ---- the send path --------------------------------------------------------- */

static void send_now(fdk_notify *n, const fdk_notify_request *req,
                     void *note_user) {
    if (n->bus == NULL) {
        /* Queue for the NO_BUS window (bounded; in order). */
        if (n->queue_len == QUEUE_MAX) {
            FDK_WARN("notify: send queue full (%d) — oldest send failed",
                     QUEUE_MAX);
            queued_send q = n->queue[0];
            memmove(n->queue, n->queue + 1,
                    (n->queue_len - 1) * sizeof(*n->queue));
            n->queue_len--;
            if (n->cb.on_failed != NULL && !n->tearing_down) {
                n->cb.on_failed(n, q.note_user, "overflow", n->cb.user);
            }
            request_clear(&q);
        }
        if (!request_copy(&n->queue[n->queue_len], req, note_user)) {
            if (n->cb.on_failed != NULL && !n->tearing_down) {
                n->cb.on_failed(n, note_user, "allocation failed",
                                n->cb.user);
            }
            return;
        }
        n->queue_len++;
        return;
    }

    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    if (call == NULL) {
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "allocation failed",
                            n->cb.user);
        }
        return;
    }
    int ok = fdk_ok(fdk_dbus_message_append_string(
        call, req->app_name != NULL ? req->app_name
                                    : "FDK application"));
    ok = ok && fdk_ok(fdk_dbus_message_append_uint32(call,
                                                     req->replaces_id));
    ok = ok && fdk_ok(fdk_dbus_message_append_string(
        call, req->icon != NULL ? req->icon : ""));
    ok = ok && fdk_ok(fdk_dbus_message_append_string(call, req->summary));
    ok = ok && fdk_ok(fdk_dbus_message_append_string(
        call, req->body != NULL ? req->body : ""));

    fdk_dbus_value *actions = build_actions(req);
    fdk_dbus_value *hints = build_hints(req);
    if (actions == NULL || hints == NULL) {
        ok = 0;
    } else {
        ok = ok && fdk_ok(fdk_dbus_message_append(call, actions));
        ok = ok && fdk_ok(fdk_dbus_message_append(call, hints));
    }
    fdk_dbus_value_free(actions);
    fdk_dbus_value_free(hints);
    int32_t timeout = req->timeout_ms;
    ok = ok && fdk_ok(fdk_dbus_message_append_int32(call, timeout));

    if (!ok) {
        fdk_dbus_message_free(call);
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "build failed", n->cb.user);
        }
        return;
    }

    send_cell *cell = fdk_alloc(sizeof(*cell));
    if (cell == NULL) {
        fdk_dbus_message_free(call);
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "allocation failed",
                            n->cb.user);
        }
        return;
    }
    cell->n = n;
    cell->note_user = note_user;
    if (fdk_dbus_connection_call(n->bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 on_notify_reply, cell) == NULL) {
        fdk_dbus_message_free(call);
        fdk_free(cell);
        if (n->cb.on_failed != NULL) {
            n->cb.on_failed(n, note_user, "not connected", n->cb.user);
        }
    }
}

/* ---- public ---------------------------------------------------------------- */

fdk_notify *fdk_notify_open(fdk_context *ctx,
                            const fdk_notify_callbacks *callbacks) {
    if (ctx == NULL) {
        return NULL;
    }
    fdk_notify *n = fdk_alloc(sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->ctx = ctx;
    if (callbacks != NULL) {
        n->cb = *callbacks;
    }
    n->status = FDK_NOTIFY_STATUS_NO_BUS;
    try_connect(n);
    return n;
}

void fdk_notify_close(fdk_notify *notify) {
    if (notify == NULL) {
        return;
    }
    notify->tearing_down = 1;
    if (notify->bus != NULL) {
        fdk_dbus_connection_close(notify->bus);
        notify->bus = NULL;
    }
    notify->watch = NULL;
    notify->events = NULL;
    for (size_t i = 0; i < notify->queue_len; i++) {
        request_clear(&notify->queue[i]);
    }
    notify->queue_len = 0;
    if (notify->caps != NULL) {
        for (size_t i = 0; notify->caps[i] != NULL; i++) {
            fdk_free(notify->caps[i]);
        }
        fdk_free(notify->caps);
        notify->caps = NULL;
    }
    fdk_free(notify->server_name);
    fdk_free(notify->server_vendor);
    fdk_free(notify);
}

fdk_notify_status fdk_notify_get_status(const fdk_notify *notify) {
    return notify != NULL ? notify->status : FDK_NOTIFY_STATUS_NO_BUS;
}

const char *const *fdk_notify_capabilities(const fdk_notify *notify) {
    return notify != NULL ? (const char *const *)notify->caps : NULL;
}

const char *fdk_notify_server_name(const fdk_notify *notify) {
    return notify != NULL ? notify->server_name : NULL;
}

const char *fdk_notify_server_vendor(const fdk_notify *notify) {
    return notify != NULL ? notify->server_vendor : NULL;
}

int fdk_notify_send(fdk_notify *notify, const fdk_notify_request *req,
                    void *note_user) {
    if (notify == NULL || req == NULL || req->summary == NULL ||
        req->summary[0] == '\0') {
        return 0;
    }
    if (req->image_data != NULL &&
        (req->image_width <= 0 || req->image_height <= 0 ||
         req->image_width > MAX_IMAGE_DIM ||
         req->image_height > MAX_IMAGE_DIM)) {
        FDK_WARN("notify: image %dx%d refused (cap %d)",
                 req->image_width, req->image_height, MAX_IMAGE_DIM);
        return 0;
    }
    if (req->image_data != NULL) {
        int32_t ch = req->image_has_alpha ? 4 : 3;
        if (req->image_rowstride > 0 &&
            req->image_rowstride < req->image_width * ch) {
            FDK_WARN("notify: image rowstride %d < width*%d — refused",
                     req->image_rowstride, ch);
            return 0;
        }
    }
    if (notify->bus == NULL) {
        try_connect(notify); /* throttled; a late bus gets used */
    }
    send_now(notify, req, note_user);
    if (notify->bus != NULL && notify->queue_len > 0) {
        /* A connection materialized (it may still be handshaking);
         * move the queue onto it — the bus queues pre-ready
         * messages itself, in order. */
        flush_queue(notify);
    }
    return 1;
}

void fdk_notify_withdraw(fdk_notify *notify, uint32_t id) {
    if (notify == NULL || notify->bus == NULL || id == 0) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "CloseNotification");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, id))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        notify->bus, call, FDK_DBUS_TIMEOUT_DEFAULT, NULL, NULL);
    if (p == NULL) {
        fdk_dbus_message_free(call);
    }
}
