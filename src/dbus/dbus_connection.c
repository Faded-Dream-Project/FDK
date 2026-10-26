/*
 * dbus_connection.c — the connection object (1.1.0)
 *
 * Everything above the wire: the async state machine (connect →
 * SASL → Hello → ready), the fd-source/pump integration, pending
 * calls with timeouts, deferred pre-ready sends, match rules with
 * refcounting and client-side evaluation, name watching and
 * ownership, the method-call serving path, properties, and
 * introspection.
 *
 * Reentrancy model (the discipline the whole file is written to —
 * the same shape the extfd/timer fire loops use, adapted):
 *   - Every APP callback fires between enter_cb()/leave_cb(). A
 *     callback may call any fdk_dbus function — including
 *     fdk_dbus_connection_close() on the very connection whose
 *     callback is running. close() tears the connection down (fd,
 *     sources, timers, buffers) but NEVER frees the struct from
 *     inside a callback: dead connections become zombies until the
 *     SWEEP reaps them (every public entry point, the end of our
 *     own io/timer callbacks, and fdk_shutdown). Every dispatch
 *     loop therefore checks c->dead after each callback and stops —
 *     the struct itself stays valid, so plain field reads are safe.
 *   - A pending/subscription/watcher whose callback is on the stack
 *     is marked `firing`; its free is deferred to the fire wrapper
 *     (pendings) or the dispatch sweep (subs/watchers) — never the
 *     app's free call, never teardown.
 *   - The extfd machinery's self-removal deferral covers the fd
 *     source record; fdk_timer_remove's firing-timer deferral
 *     covers the timeout records.
 *
 * Bus-side truth this file encodes: NameAcquired/NameLost arrive
 * UNICAST from the daemon (no match rule — routed before the
 * subscription engine); NameOwnerChanged needs one arg0-scoped rule
 * per watched name (refcounted); replies correlate by serial and
 * are delivered at most once; a reply to a canceled call is dropped
 * silently (the specification's own rule for late arrivals).
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* Connection states. */
enum {
    DBUS_ST_CONNECTING = 0, /* socket connect in flight (EINPROGRESS) */
    DBUS_ST_AUTH,           /* SASL lines                            */
    DBUS_ST_HELLO_WAIT,     /* Hello sent, awaiting the reply        */
    DBUS_ST_READY,
    DBUS_ST_DEAD,
};

static void conn_teardown(fdk_dbus_connection *c, int fire_callbacks);
static void conn_free(fdk_dbus_connection *c);
static void conn_die(fdk_dbus_connection *c, const char *why);
static int conn_flush(fdk_dbus_connection *c);
static void io_callback(fdk_ext_fd_source *src, uint32_t revents,
                        void *user);
static void hello_reply_cb(fdk_dbus_connection *c, fdk_dbus_message *m,
                           void *user);
static void sweep_dead_watchers(fdk_dbus_connection *c);

/* ---- small helpers ---------------------------------------------------------- */

static char *dup_str(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = fdk_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* ---- the context registry ----------------------------------------------------- */

fdk_result fdk__dbus_conn_register(fdk_dbus_connection *c) {
    fdk_context *ctx = c->ctx;
    if (ctx->dbus_conn_count == ctx->dbus_conn_capacity) {
        size_t cap = ctx->dbus_conn_capacity ? ctx->dbus_conn_capacity * 2
                                             : 4;
        if (cap > SIZE_MAX / sizeof(fdk_dbus_connection *)) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_dbus_connection **grown =
            fdk_realloc(ctx->dbus_conns, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        ctx->dbus_conns = grown;
        ctx->dbus_conn_capacity = cap;
    }
    ctx->dbus_conns[ctx->dbus_conn_count++] = c;
    return FDK_OK;
}

void fdk__dbus_conn_unregister(fdk_dbus_connection *c) {
    fdk_context *ctx = c->ctx;
    if (ctx == NULL) {
        return;
    }
    for (size_t i = 0; i < ctx->dbus_conn_count; i++) {
        if (ctx->dbus_conns[i] == c) {
            ctx->dbus_conns[i] = ctx->dbus_conns[--ctx->dbus_conn_count];
            return;
        }
    }
}

void fdk__dbus_shutdown(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->dbus_conn_count > 0) {
        FDK_INFO("shutdown with %zu D-Bus connection(s) still live — "
                 "closing",
                 ctx->dbus_conn_count);
    }
    while (ctx->dbus_conn_count > 0) {
        fdk_dbus_connection *c = ctx->dbus_conns[ctx->dbus_conn_count - 1];
        conn_teardown(c, 0); /* teardown, not delivery */
        conn_free(c);
    }
    fdk_free(ctx->dbus_conns);
    ctx->dbus_conns = NULL;
    ctx->dbus_conn_capacity = 0;
}

/* ---- enter/leave: the callback reentrancy discipline ------------------------- */

static void enter_cb(fdk_dbus_connection *c) {
    c->in_callback++;
}

static void leave_cb(fdk_dbus_connection *c) {
    c->in_callback--;
    /* The struct is NEVER freed here — the sweep owns reaping, so
     * dispatch loops may keep reading c fields (they check dead). */
}

/* ---- pending calls -------------------------------------------------------------- */

static void pending_unlink_free(fdk_dbus_connection *c,
                                fdk_dbus_pending *p) {
    for (size_t i = 0; i < c->pending_count; i++) {
        if (c->pending[i] == p) {
            c->pending[i] = c->pending[--c->pending_count];
            break;
        }
    }
    if (p->timer != NULL) {
        fdk_timer_remove(p->timer); /* firing-safe: deactivates    */
        p->timer = NULL;
    }
    if (p->queued != NULL) {
        /* An unsent pre-ready message: the PENDING owns it, the
         * deferred array holds a non-owning pointer — remove it
         * there, then free it here. Exactly one free, ever. */
        for (size_t i = 0; i < c->deferred_count; i++) {
            if (c->deferred[i] == p->queued) {
                memmove(&c->deferred[i], &c->deferred[i + 1],
                        (c->deferred_count - i - 1) *
                            sizeof(*c->deferred));
                c->deferred_count--;
                break;
            }
        }
        fdk_dbus_message_free(p->queued);
        p->queued = NULL;
    }
    fdk_free(p);
}

/* Appends to the pre-ready send queue (FIFO). */
static int deferred_push(fdk_dbus_connection *c, fdk_dbus_message *m) {
    if (c->deferred_count == c->deferred_cap) {
        size_t cap = c->deferred_cap ? c->deferred_cap * 2 : 8;
        fdk_dbus_message **grown =
            fdk_realloc(c->deferred, cap * sizeof(*grown));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        c->deferred = grown;
        c->deferred_cap = cap;
    }
    c->deferred[c->deferred_count++] = m;
    return FDK_OK;
}

/* Fires one pending's callback, then frees it (deferred-free
 * discipline: nothing else frees a firing pending). A nested
 * cascade (death/close inside another pending's callback) may call
 * this on a pending that is ALREADY firing — its own wrapper owns
 * the completion, so this just marks it done. */
static void fire_pending(fdk_dbus_connection *c, fdk_dbus_pending *p,
                         fdk_dbus_message *reply) {
    if (p->firing) {
        p->done = 1;
        return;
    }
    p->firing = 1;
    fdk_dbus_reply_fn fn = p->fn;
    void *user = p->user;
    int already = p->done;
    p->done = 1;
    enter_cb(c);
    if (!already && fn != NULL) {
        fn(c, reply, user);
    }
    leave_cb(c);
    p->firing = 0;
    pending_unlink_free(c, p);
}

static void timeout_timer_cb(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_dbus_pending *p = user;
    fdk_dbus_connection *c = p->conn;
    fire_pending(c, p, NULL); /* NULL reply = timeout */
    /* The connection may have died (or been closed) inside the
     * callback. A DEAD handle stays valid for the app; only a
     * close_pending zombie (closed from inside its own callback)
     * is freed, and only once no callback of ours is on the stack. */
    if (c->in_callback == 0 && c->dead && c->close_pending) {
        conn_free(c);
    }
}

static fdk_dbus_pending *pending_new(fdk_dbus_connection *c,
                                     fdk_dbus_reply_fn fn, void *user) {
    fdk_dbus_pending *p = fdk_alloc(sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->conn = c;
    p->fn = fn;
    p->user = user;
    if (c->pending_count == c->pending_cap) {
        size_t cap = c->pending_cap ? c->pending_cap * 2 : 8;
        fdk_dbus_pending **grown =
            fdk_realloc(c->pending, cap * sizeof(*grown));
        if (grown == NULL) {
            fdk_free(p);
            return NULL;
        }
        c->pending = grown;
        c->pending_cap = cap;
    }
    c->pending[c->pending_count++] = p;
    return p;
}

static uint32_t next_serial(fdk_dbus_connection *c) {
    uint32_t s = c->next_serial++;
    if (s == 0 || c->next_serial == 0) {
        c->next_serial = 1;
        s = c->next_serial++;
    }
    return s;
}

/* Serializes + queues + flushes a built message, assigning its
 * serial. The message is consumed (freed) on success only. */
static int send_built(fdk_dbus_connection *c, fdk_dbus_message *m,
                      uint32_t *out_serial) {
    uint32_t serial = next_serial(c);
    m->serial = serial;
    fdk__dbuf out = {NULL, 0, 0, FDK_OK};
    int r = fdk__dbus_message_serialize(m, serial, &out);
    fdk_dbus_message_free(m);
    if (r != FDK_OK) {
        fdk__dbuf_free(&out);
        return r;
    }
    r = fdk__dbuf_put(&c->out, out.data, out.len);
    fdk__dbuf_free(&out);
    if (r != FDK_OK) {
        return r;
    }
    if (out_serial != NULL) {
        *out_serial = serial;
    }
    (void)conn_flush(c);
    return FDK_OK;
}

static uint32_t effective_timeout(uint32_t timeout_ms) {
    if (timeout_ms == FDK_DBUS_TIMEOUT_DEFAULT) {
        return 25000; /* libdbus's default, kept for parity */
    }
    return timeout_ms;
}

static int pending_arm_timer(fdk_dbus_connection *c, fdk_dbus_pending *p) {
    if (p->timeout_ms == FDK_DBUS_TIMEOUT_NONE) {
        return FDK_OK; /* wait forever */
    }
    p->timer = fdk_timer_add(c->ctx, p->timeout_ms, 0,
                             timeout_timer_cb, p);
    return (p->timer != NULL) ? FDK_OK : FDK_ERR_OUT_OF_MEMORY;
}

/* ---- writing / reading ----------------------------------------------------------- */

static void update_io_watch(fdk_dbus_connection *c) {
    if (c->io == NULL || c->dead) {
        return;
    }
    uint32_t want = FDK_EXT_FD_IN;
    if (c->state == DBUS_ST_CONNECTING || c->out.len > 0) {
        want |= FDK_EXT_FD_OUT;
    }
    (void)fdk_ext_loop_update_fd(c->io, want);
}

static int conn_flush(fdk_dbus_connection *c) {
    if (c->dead) {
        return FDK_ERR_INVALID_STATE;
    }
    while (c->out.len > 0) {
        ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_NOSIGNAL);
        if (n > 0) {
            memmove(c->out.data, c->out.data + n, c->out.len - (size_t)n);
            c->out.len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; /* buffer full: watch OUT */
        }
        conn_die(c, "socket write failed");
        return FDK_ERR_PLATFORM;
    }
    update_io_watch(c);
    return FDK_OK;
}

/* Grows the in-buffer for another read, enforcing the message cap. */
static int in_reserve(fdk_dbus_connection *c, size_t extra) {
    size_t cap_limit = c->max_message + 16 + FDK_DBUS_READ_CHUNK;
    if (c->in_len + extra > cap_limit) {
        return FDK_ERR_LIMIT;
    }
    if (c->in_len + extra <= c->in_cap) {
        return FDK_OK;
    }
    size_t want = c->in_cap ? c->in_cap : 4096;
    while (want < c->in_len + extra) {
        want *= 2;
        if (want > cap_limit) {
            want = cap_limit;
            break;
        }
    }
    if (want < c->in_len + extra) {
        return FDK_ERR_LIMIT;
    }
    uint8_t *grown = fdk_realloc(c->in, want);
    if (grown == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    c->in = grown;
    c->in_cap = want;
    return FDK_OK;
}

/* ---- the auth phase driver ----------------------------------------------------- */

static void flush_deferred(fdk_dbus_connection *c);

static int auth_feed(fdk_dbus_connection *c) {
    /* Feed complete lines to the state machine; queue its replies. */
    for (;;) {
        uint8_t *nl = memchr(c->in, '\n', c->in_len);
        if (nl == NULL) {
            if (c->in_len > FDK_DBUS_AUTH_LINE_MAX) {
                conn_die(c, "SASL reply line overlong");
                return -1;
            }
            return 0;
        }
        size_t line_len = (size_t)(nl - c->in) + 1; /* incl. \n */
        char line[FDK_DBUS_AUTH_LINE_MAX + 2];
        if (line_len > sizeof(line) - 1) {
            conn_die(c, "SASL reply line overlong");
            return -1;
        }
        memcpy(line, c->in, line_len);
        line[line_len] = '\0';
        memmove(c->in, c->in + line_len, c->in_len - line_len);
        c->in_len -= line_len;

        size_t reply_len = 0;
        char *reply = fdk__dbus_auth_step(&c->auth, line, &reply_len);
        if (reply != NULL) {
            int r = fdk__dbuf_put(&c->out, reply, reply_len);
            fdk_free(reply);
            if (r != FDK_OK) {
                conn_die(c, "out of memory during auth");
                return -1;
            }
        }
        if (c->auth.failed) {
            conn_die(c, "SASL handshake rejected");
            return -1;
        }
        if (c->auth.done) {
            /* BEGIN queued; send Hello and switch to messages. The
             * bus is LIVE from here (the deferred queue includes the
             * Hello itself — flush it now, not at ready). */
            c->state = DBUS_ST_HELLO_WAIT;
            fdk_dbus_message *hello = fdk_dbus_message_new_method_call(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "Hello");
            if (hello == NULL) {
                conn_die(c, "out of memory sending Hello");
                return -1;
            }
            if (fdk_dbus_connection_call(c, hello, 10000, hello_reply_cb,
                                         c) == NULL) {
                /* call() left the message with us on failure */
                fdk_dbus_message_free(hello);
                conn_die(c, "cannot send Hello");
                return -1;
            }
            flush_deferred(c);
            return 1; /* state changed */
        }
    }
}

/* ---- the Hello reply: ready, flush deferred ------------------------------------ */

static void flush_deferred(fdk_dbus_connection *c) {
    /* Sends the queue in order. Each entry leaves the array BEFORE
     * it is consumed (send_built frees the message whatever happens,
     * so an early death-return leaves only live, owned messages
     * behind — the ownership rules in pending_unlink_free and
     * conn_teardown keep exactly one free per message). */
    while (c->deferred_count > 0 && !c->dead) {
        fdk_dbus_message *m = c->deferred[0];
        memmove(&c->deferred[0], &c->deferred[1],
                (c->deferred_count - 1) * sizeof(*c->deferred));
        c->deferred_count--;
        /* A pending waiting on this message (a call made pre-ready)
         * hands ownership over: clear its marker, then send. */
        fdk_dbus_pending *owner = NULL;
        for (size_t k = 0; k < c->pending_count; k++) {
            if (c->pending[k]->queued == m) {
                owner = c->pending[k];
                owner->queued = NULL;
                break;
            }
        }
        uint32_t serial = 0;
        (void)send_built(c, m, owner ? &serial : NULL); /* consumes */
        if (owner != NULL) {
            owner->serial = serial;
        }
    }
}

static void hello_reply_cb(fdk_dbus_connection *c, fdk_dbus_message *m,
                           void *user) {
    (void)user;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        conn_die(c, m == NULL ? "Hello timed out" : "Hello error reply");
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL || fdk_dbus_value_type(args[0]) != FDK_DBUS_STRING) {
        fdk_dbus_values_free_array(args, n);
        conn_die(c, "malformed Hello reply");
        return;
    }
    fdk_free(c->unique_name);
    c->unique_name = dup_str(fdk_dbus_value_string(args[0]));
    fdk_dbus_values_free_array(args, n);
    if (c->unique_name == NULL) {
        conn_die(c, "out of memory on Hello reply");
        return;
    }
    c->state = DBUS_ST_READY;
    if (c->handshake_timer != NULL) {
        fdk_timer_remove(c->handshake_timer);
        c->handshake_timer = NULL;
    }
    FDK_DEBUG("D-Bus ready on %s as %s", c->address, c->unique_name);
    flush_deferred(c);
    if (c->dead) {
        return;
    }
    if (c->ready_fn != NULL) {
        enter_cb(c);
        c->ready_fn(c, 1, NULL, c->ready_user);
        leave_cb(c);
    }
}

/* ---- dispatch ---------------------------------------------------------------- */

static fdk_dbus_pending *find_pending_by_serial(fdk_dbus_connection *c,
                                                uint32_t serial) {
    for (size_t i = 0; i < c->pending_count; i++) {
        if (c->pending[i]->serial == serial && !c->pending[i]->done) {
            return c->pending[i];
        }
    }
    return NULL;
}

static void dispatch_return_or_error(fdk_dbus_connection *c,
                                     fdk_dbus_message *m) {
    fdk_dbus_pending *p = find_pending_by_serial(c, m->reply_serial);
    if (p == NULL) {
        return; /* late reply to a canceled call: drop silently */
    }
    fire_pending(c, p, m);
}

/* ---- name events ------------------------------------------------------------------ */

static void fire_name_event(fdk_dbus_connection *c, const char *name,
                            int owned) {
    if (c->name_fn == NULL || c->dead) {
        return;
    }
    enter_cb(c);
    c->name_fn(c, name, owned, c->name_user);
    leave_cb(c);
}

/* ---- watchers ---------------------------------------------------------------------- */

static void watcher_fire(fdk_dbus_connection *c, fdk_dbus_name_watcher *w,
                         const char *owner /* NULL = vanished */) {
    w->owner_known = 1;
    fdk_free(w->owner);
    w->owner = dup_str(owner);
    w->firing = 1;
    enter_cb(c);
    w->fn(c, w->name, w->owner, w->user);
    leave_cb(c);
    w->firing = 0;
    if (w->dead) {
        /* freed from inside its own callback: deferred to here */
        for (size_t i = 0; i < c->watcher_count; i++) {
            if (c->watchers[i] == w) {
                c->watchers[i] = c->watchers[--c->watcher_count];
                break;
            }
        }
        fdk_free(w->name);
        fdk_free(w->owner);
        fdk_free(w);
    }
}

/* Reconciles ownership transitions (fires only on real change). */
static void watcher_set_owner(fdk_dbus_connection *c,
                              fdk_dbus_name_watcher *w,
                              const char *owner) {
    if (w->dead) {
        return;
    }
    if (w->owner_known) {
        if (w->owner == NULL && owner == NULL) {
            return;
        }
        if (w->owner != NULL && owner != NULL &&
            strcmp(w->owner, owner) == 0) {
            return;
        }
    }
    watcher_fire(c, w, owner);
}

static void getowner_reply_cb(fdk_dbus_connection *c, fdk_dbus_message *m,
                              void *user) {
    fdk_dbus_name_watcher *w = user;
    w->initial = NULL; /* the pending is being freed by the fire loop */
    if (m == NULL) {
        return; /* timeout/death: stay silent until a change arrives */
    }
    if (fdk_dbus_message_is_error(m)) {
        /* NameHasNoOwner is the "vanished" answer — any error is,
         * for watching purposes (logged once at DEBUG). */
        FDK_DEBUG("GetNameOwner(%s) errored: %s", w->name,
                  fdk_dbus_message_error_name(m)
                      ? fdk_dbus_message_error_name(m)
                      : "?");
        watcher_set_owner(c, w, NULL);
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL) {
        return;
    }
    watcher_set_owner(c, w, fdk_dbus_value_string(args[0]));
    fdk_dbus_values_free_array(args, n);
}

static void handle_name_owner_changed(fdk_dbus_connection *c,
                                      fdk_dbus_message *m) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 3 ||
        args == NULL) {
        return;
    }
    const char *name = fdk_dbus_value_string(args[0]);
    const char *new_owner = fdk_dbus_value_string(args[2]);
    if (name == NULL) {
        fdk_dbus_values_free_array(args, n);
        return;
    }
    const char *owner = (new_owner == NULL || new_owner[0] == '\0')
                            ? NULL
                            : new_owner;
    for (size_t i = 0; i < c->watcher_count; i++) {
        fdk_dbus_name_watcher *w = c->watchers[i];
        if (w->dead) {
            continue;
        }
        if (strcmp(w->name, name) == 0) {
            watcher_set_owner(c, w, owner);
        }
        if (c->dead) {
            break; /* the connection died during a fire */
        }
    }
    fdk_dbus_values_free_array(args, n);
    sweep_dead_watchers(c);
}

/* ---- signals ------------------------------------------------------------------------ */

/* Client-side match-rule evaluation. Fail-open on keys this
 * evaluator does not implement (documented in the header). */
typedef struct rule_pair {
    char *key;
    char *value;
} rule_pair;

typedef struct rule_parsed {
    rule_pair *pairs;
    size_t n;
} rule_parsed;

static void rule_parse_free(rule_parsed *rp) {
    for (size_t i = 0; i < rp->n; i++) {
        fdk_free(rp->pairs[i].key);
        fdk_free(rp->pairs[i].value);
    }
    fdk_free(rp->pairs);
    rp->pairs = NULL;
    rp->n = 0;
}

static int rule_parse(const char *rule, rule_parsed *rp) {
    memset(rp, 0, sizeof(*rp));
    const char *p = rule;
    size_t cap = 0;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *eq = strchr(p, '=');
        const char *comma = strchr(p, ',');
        if (eq == NULL || (comma != NULL && eq > comma)) {
            rule_parse_free(rp);
            return 0;
        }
        size_t klen = (size_t)(eq - p);
        const char *vs = eq + 1;
        const char *ve;
        if (*vs == '\'') {
            vs++;
            const char *close = strchr(vs, '\'');
            if (close == NULL) {
                rule_parse_free(rp);
                return 0;
            }
            ve = close;
        } else {
            ve = comma ? comma : p + strlen(p);
        }
        char *key = fdk_alloc(klen + 1);
        char *value = fdk_alloc((size_t)(ve - vs) + 1);
        if (key == NULL || value == NULL) {
            fdk_free(key);
            fdk_free(value);
            rule_parse_free(rp);
            return 0;
        }
        memcpy(key, p, klen);
        key[klen] = '\0';
        memcpy(value, vs, (size_t)(ve - vs));
        value[ve - vs] = '\0';
        if (rp->n == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            rule_pair *grown =
                fdk_realloc(rp->pairs, ncap * sizeof(*grown));
            if (grown == NULL) {
                fdk_free(key);
                fdk_free(value);
                rule_parse_free(rp);
                return 0;
            }
            rp->pairs = grown;
            cap = ncap;
        }
        rp->pairs[rp->n].key = key;
        rp->pairs[rp->n].value = value;
        rp->n++;
        p = ve;
        if (*p == '\'') {
            p++;
        }
        if (*p == ',') {
            p++;
        } else if (*p != '\0') {
            rule_parse_free(rp);
            return 0;
        }
    }
    return 1;
}

/* Reads argument `idx` as a string into out (bounded). Returns 0
 * when the argument is absent or not string-typed (a correct
 * no-match for string literals). */
static int msg_arg_string(fdk_dbus_message *m, size_t idx, char *out,
                          size_t cap) {
    if (m == NULL || m->body == NULL || m->signature == NULL) {
        return 0;
    }
    fdk__dreader r = {m->body, m->body_len, 0, 0, m->endian, FDK_OK};
    const char *sig = m->signature;
    size_t pos = 0;
    size_t arg = 0;
    while (sig[pos] != '\0' && fdk__rd_ok(&r)) {
        size_t start = pos;
        if (fdk__dbus_sig_one_complete(sig, &pos, 0, 0, NULL) < 0) {
            return 0;
        }
        if (sig[start] != 's' && sig[start] != 'o' && sig[start] != 'g') {
            /* not string-typed: skip the value to keep walking */
            char one[64];
            size_t olen = pos - start;
            if (olen + 1 > sizeof(one)) {
                return 0;
            }
            memcpy(one, sig + start, olen);
            one[olen] = '\0';
            fdk_dbus_value *v = fdk__dbus_unmarshal_value(&r, one);
            if (v == NULL) {
                return 0;
            }
            fdk_dbus_value_free(v);
            if (!fdk__rd_ok(&r)) {
                return 0;
            }
        } else {
            if (arg == idx) {
                char *s = fdk__rd_string(&r);
                if (s == NULL || !fdk__rd_ok(&r)) {
                    fdk_free(s);
                    return 0;
                }
                int fits = (strlen(s) < cap);
                if (fits) {
                    snprintf(out, cap, "%s", s);
                }
                fdk_free(s);
                return fits;
            }
            char *s = fdk__rd_string(&r);
            fdk_free(s);
            if (!fdk__rd_ok(&r)) {
                return 0;
            }
        }
        arg++;
    }
    return 0;
}

static int rule_matches_parsed(const rule_parsed *rp, fdk_dbus_message *m) {
    char scratch[256];
    for (size_t i = 0; i < rp->n; i++) {
        const char *key = rp->pairs[i].key;
        const char *value = rp->pairs[i].value;
        if (strcmp(key, "type") == 0) {
            int ok = 0;
            if (strcmp(value, "signal") == 0) {
                ok = (m->kind == FDK_DBUS_MESSAGE_SIGNAL);
            } else if (strcmp(value, "method_call") == 0) {
                ok = (m->kind == FDK_DBUS_MESSAGE_METHOD_CALL);
            } else if (strcmp(value, "method_return") == 0) {
                ok = (m->kind == FDK_DBUS_MESSAGE_METHOD_RETURN);
            } else if (strcmp(value, "error") == 0) {
                ok = (m->kind == FDK_DBUS_MESSAGE_ERROR);
            }
            if (!ok) {
                return 0;
            }
        } else if (strcmp(key, "sender") == 0) {
            if (m->sender == NULL || strcmp(m->sender, value) != 0) {
                return 0;
            }
        } else if (strcmp(key, "destination") == 0) {
            if (m->destination == NULL ||
                strcmp(m->destination, value) != 0) {
                return 0;
            }
        } else if (strcmp(key, "interface") == 0) {
            if (m->interface == NULL ||
                strcmp(m->interface, value) != 0) {
                return 0;
            }
        } else if (strcmp(key, "member") == 0) {
            if (m->member == NULL || strcmp(m->member, value) != 0) {
                return 0;
            }
        } else if (strcmp(key, "path") == 0) {
            if (m->path == NULL || strcmp(m->path, value) != 0) {
                return 0;
            }
        } else if (strcmp(key, "path_namespace") == 0) {
            if (m->path == NULL) {
                return 0;
            }
            size_t vl = strlen(value);
            if (strncmp(m->path, value, vl) != 0) {
                return 0;
            }
            if (strlen(m->path) > vl && m->path[vl] != '/') {
                return 0;
            }
        } else if (strcmp(key, "arg0") == 0) {
            if (!msg_arg_string(m, 0, scratch, sizeof(scratch)) ||
                strcmp(scratch, value) != 0) {
                return 0;
            }
        } else if (strncmp(key, "arg", 3) == 0 && key[3] >= '1' &&
                   key[3] <= '9' && key[4] == '\0') {
            size_t idx = (size_t)(key[3] - '0');
            if (!msg_arg_string(m, idx, scratch, sizeof(scratch)) ||
                strcmp(scratch, value) != 0) {
                return 0;
            }
        }
        /* arg0path / arg0namespace / argNN (NN > 9) / unknown keys:
         * fail OPEN (deliver) — the server already filtered. */
    }
    return 1;
}

int fdk__dbus_rule_matches(const char *rule, fdk_dbus_message *m) {
    rule_parsed rp;
    if (!rule_parse(rule, &rp)) {
        return 0;
    }
    int match = rule_matches_parsed(&rp, m);
    rule_parse_free(&rp);
    return match;
}

/* The PropertiesChanged bridge: one internal signal callback. */
static void props_changed_signal_cb(fdk_dbus_connection *c,
                                    fdk_dbus_message *sig, void *user) {
    fdk_dbus_subscription *s = user;
    if (s->pc_fn == NULL) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(sig, &args, &n) != FDK_OK || n < 3 ||
        args == NULL) {
        return;
    }
    const char *iface = fdk_dbus_value_string(args[0]);
    const fdk_dbus_value *changed = args[1];
    /* invalidated: array of string → borrowed pointer table */
    const fdk_dbus_value *inv = args[2];
    size_t inv_n = fdk_dbus_value_array_count(inv);
    const char **inv_names = NULL;
    if (inv_n > 0) {
        inv_names = fdk_alloc_array(inv_n, sizeof(char *));
        if (inv_names != NULL) {
            for (size_t i = 0; i < inv_n; i++) {
                inv_names[i] =
                    fdk_dbus_value_string(fdk_dbus_value_array_item(inv, i));
            }
        }
    }
    enter_cb(c);
    s->pc_fn(c, iface, changed, inv_n, inv_names, s->pc_user);
    leave_cb(c);
    fdk_free(inv_names);
    fdk_dbus_values_free_array(args, n);
}

static void dispatch_signal(fdk_dbus_connection *c, fdk_dbus_message *m) {
    /* Bus-daemon unicast events first. */
    if (m->sender != NULL &&
        strcmp(m->sender, "org.freedesktop.DBus") == 0 &&
        m->interface != NULL &&
        strcmp(m->interface, "org.freedesktop.DBus") == 0 &&
        m->member != NULL) {
        if (strcmp(m->member, "NameAcquired") == 0 ||
            strcmp(m->member, "NameLost") == 0) {
            fdk_dbus_value **args = NULL;
            size_t n = 0;
            if (fdk_dbus_message_read_args(m, &args, &n) == FDK_OK &&
                n >= 1 && args != NULL) {
                const char *name = fdk_dbus_value_string(args[0]);
                if (name != NULL) {
                    fire_name_event(c, name,
                                    strcmp(m->member, "NameAcquired") == 0);
                }
            }
            fdk_dbus_values_free_array(args, n);
            return;
        }
        if (strcmp(m->member, "NameOwnerChanged") == 0) {
            handle_name_owner_changed(c, m);
            if (c->dead) {
                return;
            }
            /* deliberate fall-through: an app that subscribed to
             * NameOwnerChanged explicitly ALSO gets it (documented) */
        }
    }
    /* Subscriptions, in registration order. */
    for (size_t i = 0; i < c->sub_count; i++) {
        fdk_dbus_subscription *s = c->subs[i];
        if (s->dead || (s->fn == NULL && s->pc_fn == NULL)) {
            continue;
        }
        rule_parsed rp;
        if (!rule_parse(s->rule, &rp)) {
            continue; /* unparsable rule matched nothing server-side */
        }
        int match = rule_matches_parsed(&rp, m);
        rule_parse_free(&rp);
        if (match) {
            s->firing = 1;
            if (s->pc_fn != NULL) {
                props_changed_signal_cb(c, m, s);
            } else {
                enter_cb(c);
                s->fn(c, m, s->user);
                leave_cb(c);
            }
            s->firing = 0;
            if (s->dead) {
                /* freed from inside its own callback: deferred free */
                for (size_t k = 0; k < c->sub_count; k++) {
                    if (c->subs[k] == s) {
                        c->subs[k] = c->subs[--c->sub_count];
                        break;
                    }
                }
                fdk_free(s->rule);
                fdk_free(s->pc_destination);
                fdk_free(s->pc_interface);
                fdk_free(s);
            }
        }
        if (c->dead) {
            return; /* died during a fire */
        }
    }
}

/* ---- incoming method calls ----------------------------------------------------------- */

static void dispatch_method_call(fdk_dbus_connection *c,
                                 fdk_dbus_message *m) {
    if (c->method_fn == NULL) {
        if ((m->flags & FDK_DBUS_FLAG_NO_REPLY_EXPECTED) == 0) {
            fdk_dbus_message *err = fdk_dbus_message_new_error(
                m, "org.freedesktop.DBus.Error.UnknownMethod",
                "no method handler is installed on this connection");
            if (err != NULL) {
                (void)fdk_dbus_connection_send(c, err);
            }
        }
        return;
    }
    enter_cb(c);
    c->method_fn(c, m, c->method_user);
    leave_cb(c);
}

static void dispatch_message(fdk_dbus_connection *c, fdk_dbus_message *m) {
    switch (m->kind) {
    case FDK_DBUS_MESSAGE_METHOD_RETURN:
    case FDK_DBUS_MESSAGE_ERROR:
        dispatch_return_or_error(c, m);
        break;
    case FDK_DBUS_MESSAGE_SIGNAL:
        dispatch_signal(c, m);
        break;
    case FDK_DBUS_MESSAGE_METHOD_CALL:
        dispatch_method_call(c, m);
        break;
    default:
        break; /* the parser rejects other kinds */
    }
}

/* ---- the read path -------------------------------------------------------------------- */

static void process_buffer(fdk_dbus_connection *c) {
    if (c->state == DBUS_ST_AUTH) {
        if (auth_feed(c) != 0 || c->dead) {
            return;
        }
    }
    if (c->state != DBUS_ST_HELLO_WAIT && c->state != DBUS_ST_READY) {
        return;
    }
    size_t parsed = 0;
    size_t offset = 0;
    while (offset < c->in_len) {
        size_t consumed = 0;
        int oom = 0;
        fdk_dbus_message *m = fdk__dbus_message_parse(
            c->in + offset, c->in_len - offset, &consumed, &oom);
        if (m == NULL) {
            if (oom) {
                conn_die(c, "out of memory parsing a message");
            } else if (consumed == (size_t)-1) {
                conn_die(c, "protocol violation in message framing");
            }
            break; /* (or need more bytes) */
        }
        offset += consumed;
        parsed++;
        dispatch_message(c, m);
        fdk_dbus_message_free(m);
        if (c->dead) {
            break; /* died (or was closed) during dispatch */
        }
        if (parsed >= FDK_DBUS_DISPATCH_BUDGET) {
            break; /* fairness: continue on the next pump */
        }
    }
    if (offset > 0 && offset <= c->in_len) {
        memmove(c->in, c->in + offset, c->in_len - offset);
        c->in_len -= offset;
    }
}

static void read_socket(fdk_dbus_connection *c) {
    for (;;) {
        int r = in_reserve(c, FDK_DBUS_READ_CHUNK);
        if (r == FDK_ERR_LIMIT) {
            conn_die(c, "input exceeded the message size cap");
            return;
        }
        if (r != FDK_OK) {
            return; /* OOM: retry on the next readable round */
        }
        unsigned char ctrl[CMSG_SPACE(sizeof(int) * 16)];
        struct iovec iov = {c->in + c->in_len, c->in_cap - c->in_len};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl;
        msg.msg_controllen = sizeof(ctrl);
        ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT);
        if (n > 0) {
            c->in_len += (size_t)n;
            /* Close any SCM_RIGHTS descriptors immediately: fd
             * passing was never negotiated, so their only possible
             * origin is a hostile peer — an unclosed fd is a
             * resource leak and a privilege hazard. */
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != NULL;
                 cm = CMSG_NXTHDR(&msg, cm)) {
                if (cm->cmsg_level == SOL_SOCKET &&
                    cm->cmsg_type == SCM_RIGHTS) {
                    int nf =
                        (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    int *fds = (int *)CMSG_DATA(cm);
                    for (int k = 0; k < nf; k++) {
                        close(fds[k]);
                    }
                    FDK_WARN("D-Bus peer sent %d unix fd(s) uninvited — "
                             "closed",
                             nf);
                }
            }
            process_buffer(c);
            if (c->dead) {
                return;
            }
            if ((size_t)n < iov.iov_len) {
                return; /* drained */
            }
            continue;
        }
        if (n == 0) {
            conn_die(c, "bus closed the connection");
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* drained */
        }
        conn_die(c, "socket read failed");
        return;
    }
}

/* ---- the io callback -------------------------------------------------------------------- */

static void io_callback(fdk_ext_fd_source *src, uint32_t revents,
                        void *user) {
    fdk_dbus_connection *c = user;
    (void)src;
    enter_cb(c);
    if (!c->dead) {
        if (c->state == DBUS_ST_CONNECTING) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
                err != 0) {
                conn_die(c, "socket connect failed");
            } else {
                /* Nonce first (nonce-tcp), then the SASL opening. */
                if (c->endpoint.has_nonce) {
                    unsigned char nonce[64];
                    int nn = fdk__dbus_read_nonce(&c->endpoint, nonce,
                                                  sizeof(nonce));
                    if (nn > 0) {
                        (void)fdk__dbuf_put(&c->out, nonce, (size_t)nn);
                    } else if (nn < 0) {
                        conn_die(c, "nonce file unreadable");
                    }
                }
                if (!c->dead) {
                    c->state = DBUS_ST_AUTH;
                    size_t start_len = 0;
                    char *start =
                        fdk__dbus_auth_start((uint32_t)getuid(), &start_len);
                    if (start == NULL) {
                        conn_die(c, "out of memory starting auth");
                    } else {
                        int r = fdk__dbuf_put(&c->out, start, start_len);
                        fdk_free(start);
                        if (r != FDK_OK) {
                            conn_die(c, "out of memory starting auth");
                        }
                    }
                    (void)conn_flush(c);
                }
            }
        }
        if (!c->dead && (revents & FDK_EXT_FD_OUT) != 0) {
            (void)conn_flush(c);
        }
        if (!c->dead &&
            (revents & (FDK_EXT_FD_IN | FDK_EXT_FD_HUP | 0x020)) != 0) {
            read_socket(c);
        }
    }
    leave_cb(c);
    if (c->in_callback == 0 && c->dead && c->close_pending) {
        conn_free(c); /* closed from inside one of the callbacks above */
    }
}

/* ---- death / teardown / free --------------------------------------------------------------- */

static void handshake_timeout_cb(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_dbus_connection *c = user;
    enter_cb(c);
    if (!c->dead && c->state != DBUS_ST_READY) {
        conn_die(c, "handshake timeout");
    }
    leave_cb(c);
    if (c->in_callback == 0 && c->dead && c->close_pending) {
        conn_free(c);
    }
}

static void sweep_dead_watchers(fdk_dbus_connection *c) {
    for (size_t i = c->watcher_count; i > 0; i--) {
        fdk_dbus_name_watcher *w = c->watchers[i - 1];
        if (w->dead && !w->firing) {
            c->watchers[i - 1] = c->watchers[--c->watcher_count];
            fdk_free(w->name);
            fdk_free(w->owner);
            fdk_free(w);
        }
    }
}

static void conn_die(fdk_dbus_connection *c, const char *why) {
    if (c->state == DBUS_ST_DEAD) {
        return;
    }
    if (!c->reported_dead) {
        FDK_INFO("D-Bus connection to %s died: %s",
                 c->address ? c->address : "(unknown)", why);
        c->reported_dead = 1;
    }
    int was_ready = (c->state == DBUS_ST_READY);
    c->state = DBUS_ST_DEAD;
    c->dead = 1;

    /* Deliver failure to the app: pending NULLs first. Firing
     * pendings are skipped (their own wrappers complete them). */
    for (size_t i = 0; i < c->pending_count;) {
        fdk_dbus_pending *p = c->pending[i];
        if (p->firing) {
            i++;
            continue;
        }
        fire_pending(c, p, NULL); /* swap-removes p; do not i++ */
    }
    if (!was_ready && c->ready_fn != NULL) {
        enter_cb(c);
        c->ready_fn(c, 0, "connection failed", c->ready_user);
        leave_cb(c);
    }
    if (c->death_fn != NULL) {
        enter_cb(c);
        c->death_fn(c, c->death_user);
        leave_cb(c);
    }
    /* Watchers learn the world vanished (only those with a known
     * state — an unknown-state watcher never announced anything). */
    for (size_t i = 0; i < c->watcher_count; i++) {
        fdk_dbus_name_watcher *w = c->watchers[i];
        if (!w->dead && w->owner_known) {
            watcher_set_owner(c, w, NULL);
        }
    }
    sweep_dead_watchers(c);
    conn_teardown(c, 0);
    /* Reaping is the sweep's job (this may be mid-callback). */
}

static void conn_teardown(fdk_dbus_connection *c, int fire_callbacks) {
    if (c->fd < 0 && c->io == NULL) {
        return; /* already torn down */
    }
    c->dead = 1;
    c->state = DBUS_ST_DEAD;
    if (fire_callbacks) {
        for (size_t i = 0; i < c->pending_count;) {
            fdk_dbus_pending *p = c->pending[i];
            if (p->firing) {
                i++;
                continue;
            }
            fire_pending(c, p, NULL); /* swap-removes p */
        }
    } else {
        /* Teardown, not delivery: pendings die silently. The FIRING
         * one (if any — teardown from inside a reply callback) is
         * freed by its own fire loop; the rest here. */
        for (size_t i = c->pending_count; i > 0; i--) {
            fdk_dbus_pending *p = c->pending[i - 1];
            if (p->firing) {
                p->done = 1;
                continue; /* its fire wrapper frees it */
            }
            pending_unlink_free(c, p);
        }
    }
    if (c->io != NULL) {
        fdk_ext_loop_remove_fd(c->io); /* deferred-safe in-callback */
        c->io = NULL;
    }
    if (c->handshake_timer != NULL) {
        fdk_timer_remove(c->handshake_timer);
        c->handshake_timer = NULL;
    }
    for (size_t i = c->watcher_count; i > 0; i--) {
        fdk_dbus_name_watcher *w = c->watchers[i - 1];
        if (w->firing) {
            w->dead = 1; /* its fire wrapper frees it */
            continue;
        }
        if (w->initial != NULL) {
            fdk_dbus_pending_cancel(w->initial);
        }
        fdk_free(w->name);
        fdk_free(w->owner);
        fdk_free(w);
    }
    c->watcher_count = 0;
    fdk_free(c->watchers);
    c->watchers = NULL;
    c->watcher_cap = 0;
    for (size_t i = c->sub_count; i > 0; i--) {
        fdk_dbus_subscription *s = c->subs[i - 1];
        if (s->firing) {
            s->dead = 1; /* its dispatch loop frees it */
            continue;
        }
        fdk_free(s->rule);
        fdk_free(s->pc_destination);
        fdk_free(s->pc_interface);
        fdk_free(s);
    }
    c->sub_count = 0;
    fdk_free(c->subs);
    c->subs = NULL;
    c->sub_cap = 0;
    /* Deferred messages: the array frees them, but a pending's
     * `queued` marker may point at one — NULL every marker FIRST so
     * the pending frees below (and any in-flight fire wrapper) see
     * NULL and do not double-free. */
    for (size_t i = c->deferred_count; i > 0; i--) {
        fdk_dbus_message *m = c->deferred[i - 1];
        for (size_t k = 0; k < c->pending_count; k++) {
            if (c->pending[k]->queued == m) {
                c->pending[k]->queued = NULL;
            }
        }
        fdk_dbus_message_free(m);
    }
    c->deferred_count = 0;
    fdk_free(c->deferred);
    c->deferred = NULL;
    c->deferred_cap = 0;
    for (size_t i = c->match_count; i > 0; i--) {
        fdk_free(c->matches[i - 1].rule);
    }
    fdk_free(c->matches);
    c->matches = NULL;
    c->match_count = 0;
    c->match_cap = 0;
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    fdk__dbuf_free(&c->out);
}

static void conn_free(fdk_dbus_connection *c) {
    conn_teardown(c, 0);
    for (size_t i = c->pending_count; i > 0; i--) {
        fdk_dbus_pending *p = c->pending[i - 1];
        if (!p->firing) { /* defensive; firing can't happen here */
            pending_unlink_free(c, p);
        }
    }
    fdk_free(c->pending);
    fdk_free(c->in);
    fdk__dbus_conn_unregister(c);
    fdk_free(c->address);
    fdk_free(c->unique_name);
    fdk_free(c);
}

/* ==========================================================================
 * Public API — connections
 * ========================================================================*/

static fdk_dbus_connection *connect_common(fdk_context *ctx,
                                           char *address_owned,
                                           fdk_dbus_ready_fn on_ready,
                                           void *user) {
    if (ctx == NULL || address_owned == NULL) {
        fdk_free(address_owned);
        return NULL;
    }
    fdk__dbus_addr addrs[8];
    int n_addrs = fdk__dbus_resolve_addresses(address_owned, addrs, 8);
    if (n_addrs <= 0) {
        FDK_INFO("no usable D-Bus endpoint in '%s'", address_owned);
        fdk_free(address_owned);
        return NULL;
    }
    struct fdk__dbus_addr chosen;
    int fd = fdk__dbus_connect_first(addrs, n_addrs, &chosen);
    if (fd < 0) {
        /* One line, once — the no-bus posture every feature above
         * this layer keys off. */
        FDK_INFO("D-Bus unavailable (no endpoint accepted a connection "
                 "for '%s')",
                 address_owned);
        fdk_free(address_owned);
        return NULL;
    }
    fdk_dbus_connection *c = fdk_alloc(sizeof(*c));
    if (c == NULL) {
        close(fd);
        fdk_free(address_owned);
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->ctx = ctx;
    c->address = address_owned;
    c->fd = fd;
    c->state = DBUS_ST_CONNECTING;
    c->next_serial = 1;
    c->max_message = FDK_DBUS_MAX_MESSAGE_DEFAULT;
    c->endpoint = chosen;
    c->ready_fn = on_ready;
    c->ready_user = user;
    if (fdk__dbus_conn_register(c) != FDK_OK) {
        close(fd);
        fdk_free(address_owned);
        fdk_free(c);
        return NULL;
    }
    c->io = fdk_ext_loop_add_fd(ctx, fd, FDK_EXT_FD_IN | FDK_EXT_FD_OUT,
                                io_callback, c);
    if (c->io == NULL) {
        conn_free(c);
        return NULL;
    }
    c->handshake_timer = fdk_timer_add(ctx, 15000, 0,
                                       handshake_timeout_cb, c);
    if (c->handshake_timer == NULL) {
        conn_free(c);
        return NULL;
    }
    /* A local socket usually completes connect() synchronously: if
     * it already did, kick the state machine now instead of waiting
     * for a (never-arriving) writability edge... poll() on a
     * connected socket reports POLLOUT immediately, so the next
     * pump fires it either way. Nothing to do here. */
    return c;
}

fdk_dbus_connection *fdk_dbus_connect(fdk_context *ctx, fdk_dbus_bus bus,
                                      fdk_dbus_ready_fn on_ready,
                                      void *user) {
    if (ctx == NULL) {
        return NULL;
    }
    char *address = fdk__dbus_bus_address(bus);
    if (address == NULL) {
        FDK_INFO("no D-Bus address resolves for bus %d "
                 "(no DBUS_SESSION_BUS_ADDRESS / XDG_RUNTIME_DIR)",
                 (int)bus);
        return NULL;
    }
    return connect_common(ctx, address, on_ready, user);
}

fdk_dbus_connection *fdk_dbus_connect_address(fdk_context *ctx,
                                              const char *address,
                                              fdk_dbus_ready_fn on_ready,
                                              void *user) {
    if (ctx == NULL || address == NULL || address[0] == '\0') {
        return NULL;
    }
    return connect_common(ctx, dup_str(address), on_ready, user);
}

void fdk_dbus_connection_close(fdk_dbus_connection *connection) {
    if (connection == NULL) {
        return;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead) {
        /* A DEAD handle stays allocated until the app closes it:
         * free it now (or, from inside a callback, mark it for the
         * outermost wrapper — nothing may free a connection while
         * its own callbacks are on the stack). */
        if (c->in_callback == 0) {
            conn_free(c);
        } else {
            c->close_pending = 1;
        }
        return;
    }
    conn_teardown(c, 1); /* pendings see NULL replies (documented) */
    if (c->in_callback == 0) {
        conn_free(c);
    } else {
        c->close_pending = 1;
    }
}

fdk_context *fdk_dbus_connection_context(fdk_dbus_connection *connection) {
    return (connection != NULL) ? connection->ctx : NULL;
}

int fdk_dbus_connection_ready(const fdk_dbus_connection *connection) {
    return (connection != NULL && !connection->dead &&
            connection->state == DBUS_ST_READY)
               ? 1
               : 0;
}

const char *fdk_dbus_connection_unique_name(
    const fdk_dbus_connection *connection) {
    return (connection != NULL) ? connection->unique_name : NULL;
}

const char *fdk_dbus_connection_address(
    const fdk_dbus_connection *connection) {
    return (connection != NULL) ? connection->address : NULL;
}

void fdk_dbus_connection_set_method_handler(fdk_dbus_connection *connection,
                                            fdk_dbus_method_fn fn,
                                            void *user) {
    if (connection != NULL) {
        connection->method_fn = fn;
        connection->method_user = user;
    }
}

void fdk_dbus_connection_set_name_events(fdk_dbus_connection *connection,
                                         fdk_dbus_owned_name_fn fn,
                                         void *user) {
    if (connection != NULL) {
        connection->name_fn = fn;
        connection->name_user = user;
    }
}

void fdk_dbus_connection_set_death_handler(fdk_dbus_connection *connection,
                                           fdk_dbus_death_fn fn,
                                           void *user) {
    if (connection != NULL) {
        connection->death_fn = fn;
        connection->death_user = user;
    }
}

/* ==========================================================================
 * Public API — method calls
 * ========================================================================*/

fdk_dbus_pending *fdk_dbus_connection_call(fdk_dbus_connection *connection,
                                          fdk_dbus_message *call,
                                          uint32_t timeout_ms,
                                          fdk_dbus_reply_fn on_reply,
                                          void *user) {
    if (connection == NULL || call == NULL) {
        return NULL;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead || c->state == DBUS_ST_DEAD) {
        return NULL; /* message stays with the caller */
    }
    if (call->kind != FDK_DBUS_MESSAGE_METHOD_CALL || call->serial != 0) {
        return NULL;
    }
    fdk_dbus_pending *p = pending_new(c, on_reply, user);
    if (p == NULL) {
        return NULL;
    }
    p->timeout_ms = effective_timeout(timeout_ms);
    if (pending_arm_timer(c, p) != FDK_OK) {
        pending_unlink_free(c, p);
        return NULL;
    }
    if (c->state == DBUS_ST_READY) {
        uint32_t serial = 0;
        int r = send_built(c, call, &serial);
        if (r != FDK_OK) {
            pending_unlink_free(c, p);
            return NULL; /* message consumed only on success */
        }
        p->serial = serial;
    } else {
        /* Pre-ready: the message rides the deferred queue (the
         * pending's marker is the owning reference); the first
         * flush at HELLO_WAIT/READY sends it in order. */
        p->queued = call;
        if (deferred_push(c, call) != FDK_OK) {
            p->queued = NULL;
            pending_unlink_free(c, p);
            return NULL; /* message stays with the caller */
        }
    }
    return p;
}

fdk_result fdk_dbus_connection_send(fdk_dbus_connection *connection,
                                    fdk_dbus_message *message) {
    if (connection == NULL || message == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead || c->state == DBUS_ST_DEAD) {
        return FDK_ERR_INVALID_STATE;
    }
    if (message->serial != 0) {
        return FDK_ERR_INVALID_STATE; /* already sent */
    }
    if (c->state == DBUS_ST_READY) {
        return send_built(c, message, NULL);
    }
    /* Defer behind the handshake (this message has no pending —
     * the array itself is its only reference). */
    return deferred_push(c, message);
}

void fdk_dbus_pending_cancel(fdk_dbus_pending *pending) {
    if (pending == NULL) {
        return;
    }
    fdk_dbus_pending *p = pending;
    p->done = 1; /* its reply (if any) will be dropped */
    if (p->firing) {
        return; /* freed by the fire loop momentarily */
    }
    pending_unlink_free(p->conn, p);
}

uint32_t fdk_dbus_pending_serial(const fdk_dbus_pending *pending) {
    return (pending != NULL) ? pending->serial : 0;
}

/* ==========================================================================
 * Public API — match rules and subscriptions
 * ========================================================================*/

char *fdk_dbus_match_rule(const char *type, const char *sender,
                          const char *interface, const char *member,
                          const char *path, const char *arg0) {
    const char *keys[6] = {"type", "sender", "interface", "member",
                           "path", "arg0"};
    const char *vals[6] = {type, sender, interface, member, path, arg0};
    size_t cap = 1;
    for (int i = 0; i < 6; i++) {
        const char *v = vals[i];
        if (i == 0 && (v == NULL || v[0] == '\0')) {
            v = "signal"; /* type is always emitted, defaulted */
        }
        if (v != NULL && v[0] != '\0') {
            cap += strlen(keys[i]) + strlen(v) + 5;
        }
    }
    char *out = fdk_alloc(cap);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    int first = 1;
    for (int i = 0; i < 6; i++) {
        const char *v = vals[i];
        if (v == NULL || v[0] == '\0') {
            if (i == 0) {
                v = "signal"; /* type is always emitted */
            } else {
                continue;
            }
        }
        if (!first) {
            w += (size_t)snprintf(out + w, cap - w, ",");
        }
        first = 0;
        w += (size_t)snprintf(out + w, cap - w, "%s='%s'", keys[i], v);
    }
    return out;
}

static void match_reply_cb(fdk_dbus_connection *c, fdk_dbus_message *m,
                           void *user) {
    (void)user;
    if (m != NULL && fdk_dbus_message_is_error(m)) {
        FDK_WARN("bus rejected a match rule: %s",
                 fdk_dbus_message_error_name(m)
                     ? fdk_dbus_message_error_name(m)
                     : "(unnamed)");
    }
    (void)c;
}

static int match_acquire(fdk_dbus_connection *c, const char *rule) {
    for (size_t i = 0; i < c->match_count; i++) {
        if (strcmp(c->matches[i].rule, rule) == 0) {
            c->matches[i].refs++;
            return FDK_OK;
        }
    }
    if (c->match_count == c->match_cap) {
        size_t cap = c->match_cap ? c->match_cap * 2 : 8;
        struct fdk_dbus_match_ref *grown =
            fdk_realloc(c->matches, cap * sizeof(*c->matches));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        c->matches = grown;
        c->match_cap = cap;
    }
    char *copy = dup_str(rule);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    c->matches[c->match_count].rule = copy;
    c->matches[c->match_count].refs = 1;
    c->match_count++;
    /* Server-side install (best effort; errors logged once). */
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "AddMatch");
    if (m != NULL &&
        fdk_dbus_message_append_string(m, rule) == FDK_OK) {
        (void)fdk_dbus_connection_call(c, m, 10000, match_reply_cb, NULL);
    } else {
        fdk_dbus_message_free(m);
    }
    return FDK_OK;
}

static void match_release(fdk_dbus_connection *c, const char *rule) {
    for (size_t i = 0; i < c->match_count; i++) {
        if (strcmp(c->matches[i].rule, rule) == 0) {
            if (--c->matches[i].refs > 0) {
                return;
            }
            fdk_free(c->matches[i].rule);
            c->matches[i] = c->matches[--c->match_count];
            /* Server-side remove (best effort; a dead connection
             * just drops it). */
            fdk_dbus_message *m = fdk_dbus_message_new_method_call(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "RemoveMatch");
            if (m != NULL &&
                fdk_dbus_message_append_string(m, rule) == FDK_OK) {
                m->flags = FDK_DBUS_FLAG_NO_REPLY_EXPECTED;
                (void)fdk_dbus_connection_send(c, m);
            } else {
                fdk_dbus_message_free(m);
            }
            return;
        }
    }
}

fdk_dbus_subscription *fdk_dbus_connection_subscribe(
    fdk_dbus_connection *connection, const char *match_rule,
    fdk_dbus_signal_fn on_signal, void *user) {
    if (connection == NULL || match_rule == NULL || match_rule[0] == '\0') {
        return NULL;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead || c->state != DBUS_ST_READY) {
        return NULL; /* documented: subscribe once ready */
    }
    if (match_acquire(c, match_rule) != FDK_OK) {
        return NULL;
    }
    if (c->sub_count == c->sub_cap) {
        size_t cap = c->sub_cap ? c->sub_cap * 2 : 8;
        fdk_dbus_subscription **grown =
            fdk_realloc(c->subs, cap * sizeof(*grown));
        if (grown == NULL) {
            match_release(c, match_rule);
            return NULL;
        }
        c->subs = grown;
        c->sub_cap = cap;
    }
    fdk_dbus_subscription *s = fdk_alloc(sizeof(*s));
    if (s == NULL) {
        match_release(c, match_rule);
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->conn = c;
    s->rule = dup_str(match_rule);
    s->fn = on_signal;
    s->user = user;
    if (s->rule == NULL) {
        fdk_free(s);
        match_release(c, match_rule);
        return NULL;
    }
    c->subs[c->sub_count++] = s;
    return s;
}

void fdk_dbus_subscription_free(fdk_dbus_subscription *subscription) {
    if (subscription == NULL) {
        return;
    }
    fdk_dbus_subscription *s = subscription;
    if (s->dead) {
        return; /* idempotent */
    }
    s->dead = 1;
    if (!s->firing && !s->conn->dead) {
        match_release(s->conn, s->rule);
    }
    if (s->firing) {
        return; /* the dispatch loop frees it after the callback */
    }
    if (s->conn->dead) {
        /* teardown already swept the arrays... it hasn't freed this
         * one if it was firing; if we're here it wasn't, so free. */
    }
    for (size_t i = 0; i < s->conn->sub_count; i++) {
        if (s->conn->subs[i] == s) {
            s->conn->subs[i] = s->conn->subs[--s->conn->sub_count];
            break;
        }
    }
    fdk_free(s->rule);
    fdk_free(s->pc_destination);
    fdk_free(s->pc_interface);
    fdk_free(s);
}

/* ==========================================================================
 * Public API — name ownership and watching
 * ========================================================================*/

fdk_dbus_pending *fdk_dbus_connection_request_name(
    fdk_dbus_connection *connection, const char *name, uint32_t flags,
    fdk_dbus_reply_fn on_reply, void *user) {
    if (connection == NULL || name == NULL ||
        !fdk__dbus_valid_bus_name(name)) {
        return NULL;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead || c->state != DBUS_ST_READY) {
        return NULL;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (m == NULL) {
        return NULL;
    }
    if (fdk_dbus_message_append_string(m, name) != FDK_OK ||
        fdk_dbus_message_append_uint32(m, flags) != FDK_OK) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    return fdk_dbus_connection_call(c, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                    on_reply, user);
}

fdk_dbus_name_watcher *fdk_dbus_connection_watch_name(
    fdk_dbus_connection *connection, const char *well_known_name,
    fdk_dbus_name_event_fn on_event, void *user) {
    if (connection == NULL || well_known_name == NULL ||
        well_known_name[0] == '\0' || on_event == NULL) {
        return NULL;
    }
    fdk_dbus_connection *c = connection;
    if (c->dead || c->state != DBUS_ST_READY) {
        return NULL;
    }
    char *rule = fdk_dbus_match_rule(
        "signal", "org.freedesktop.DBus", "org.freedesktop.DBus",
        "NameOwnerChanged", NULL, well_known_name);
    if (rule == NULL) {
        return NULL;
    }
    if (match_acquire(c, rule) != FDK_OK) {
        fdk_free(rule);
        return NULL;
    }
    fdk_free(rule);
    if (c->watcher_count == c->watcher_cap) {
        size_t cap = c->watcher_cap ? c->watcher_cap * 2 : 8;
        fdk_dbus_name_watcher **grown =
            fdk_realloc(c->watchers, cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        c->watchers = grown;
        c->watcher_cap = cap;
    }
    fdk_dbus_name_watcher *w = fdk_alloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    w->conn = c;
    w->name = dup_str(well_known_name);
    w->fn = on_event;
    w->user = user;
    if (w->name == NULL) {
        fdk_free(w);
        return NULL;
    }
    c->watchers[c->watcher_count++] = w;
    /* The rule text is re-derivable; release needs the same string. */
    /* Initial state probe. */
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner");
    if (m == NULL || fdk_dbus_message_append_string(m, w->name) != FDK_OK) {
        fdk_dbus_message_free(m);
        return w; /* watcher exists; the probe can be retried by
                     freeing and re-creating it */
    }
    w->initial = fdk_dbus_connection_call(c, m, 10000, getowner_reply_cb,
                                          w);
    if (w->initial == NULL) {
        /* call() left the message with us on failure */
        fdk_dbus_message_free(m);
    }
    return w;
}

void fdk_dbus_name_watcher_free(fdk_dbus_name_watcher *watcher) {
    if (watcher == NULL) {
        return;
    }
    fdk_dbus_name_watcher *w = watcher;
    if (w->dead) {
        return; /* idempotent */
    }
    w->dead = 1;
    if (w->initial != NULL) {
        fdk_dbus_pending_cancel(w->initial);
        w->initial = NULL;
    }
    if (!w->firing && !w->conn->dead) {
        char *rule = fdk_dbus_match_rule(
            "signal", "org.freedesktop.DBus", "org.freedesktop.DBus",
            "NameOwnerChanged", NULL, w->name);
        if (rule != NULL) {
            match_release(w->conn, rule);
            fdk_free(rule);
        }
    }
    if (w->firing) {
        return; /* the fire wrapper frees it after the callback */
    }
    for (size_t i = 0; i < w->conn->watcher_count; i++) {
        if (w->conn->watchers[i] == w) {
            w->conn->watchers[i] = w->conn->watchers[--w->conn->watcher_count];
            break;
        }
    }
    fdk_free(w->name);
    fdk_free(w->owner);
    fdk_free(w);
}

/* ==========================================================================
 * Public API — properties
 * ========================================================================*/

fdk_dbus_pending *fdk_dbus_connection_get_property(
    fdk_dbus_connection *connection, const char *destination,
    const char *path, const char *interface, const char *property,
    fdk_dbus_reply_fn on_reply, void *user) {
    if (connection == NULL || destination == NULL || path == NULL ||
        interface == NULL || property == NULL) {
        return NULL;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        destination, path, "org.freedesktop.DBus.Properties", "Get");
    if (m == NULL) {
        return NULL;
    }
    if (fdk_dbus_message_append_string(m, interface) != FDK_OK ||
        fdk_dbus_message_append_string(m, property) != FDK_OK) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    return fdk_dbus_connection_call(connection, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                    on_reply, user);
}

fdk_dbus_pending *fdk_dbus_connection_set_property(
    fdk_dbus_connection *connection, const char *destination,
    const char *path, const char *interface, const char *property,
    const fdk_dbus_value *value, fdk_dbus_reply_fn on_reply, void *user) {
    if (connection == NULL || destination == NULL || path == NULL ||
        interface == NULL || property == NULL || value == NULL) {
        return NULL;
    }
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        destination, path, "org.freedesktop.DBus.Properties", "Set");
    if (m == NULL) {
        return NULL;
    }
    /* Set takes (ssv): the value argument is a VARIANT — the app
     * hands the INNER value and the wrapping happens here. */
    fdk_dbus_value *wrapped = fdk_dbus_value_new_variant(value);
    if (wrapped == NULL) {
        fdk_dbus_message_free(m);
        return NULL;
    }
    if (fdk_dbus_message_append_string(m, interface) != FDK_OK ||
        fdk_dbus_message_append_string(m, property) != FDK_OK ||
        fdk_dbus_message_append(m, wrapped) != FDK_OK) {
        fdk_dbus_value_free(wrapped);
        fdk_dbus_message_free(m);
        return NULL;
    }
    fdk_dbus_value_free(wrapped);
    return fdk_dbus_connection_call(connection, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                    on_reply, user);
}

fdk_dbus_subscription *fdk_dbus_connection_subscribe_properties_changed(
    fdk_dbus_connection *connection, const char *destination,
    const char *interface, fdk_dbus_props_changed_fn on_changed,
    void *user) {
    if (connection == NULL || destination == NULL || interface == NULL ||
        on_changed == NULL) {
        return NULL;
    }
    /* Sender scoping: the bus rewrites a well-known sender into
     * its UNIQUE name in the message header, so a sender='<well
     * known>' rule never matches. Scope by the signal's own shape
     * (arg0 = the interface whose properties changed) instead; a
     * unique-name destination survives the rewrite and scopes the
     * sender too. */
    char *rule = fdk_dbus_match_rule(
        "signal", (destination[0] == ':') ? destination : NULL,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL,
        interface);
    if (rule == NULL) {
        return NULL;
    }
    /* Route through a plain subscription with the bridge fields. */
    fdk_dbus_subscription *s =
        fdk_dbus_connection_subscribe(connection, rule, NULL, NULL);
    fdk_free(rule);
    if (s == NULL) {
        return NULL;
    }
    s->pc_fn = on_changed;
    s->pc_user = user;
    return s;
}

/* ==========================================================================
 * Public API — introspection
 * ========================================================================*/

typedef struct fdk_dbus_introspection {
    char *node_name;
    struct fdk_dbus_ispec {
        char *name;
        char **methods;
        size_t method_n;
        char **signals;
        size_t signal_n;
        char **props;
        char **prop_access;
        size_t prop_n;
    } *ifaces;
    size_t iface_n;
    char **children;
    size_t child_n;
} fdk_dbus_introspection;

/* ---- a small lenient XML tag scanner ------------------------------------- */

typedef struct {
    const char *p;
} xml_scan;

static int xml_skip_ws(xml_scan *s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' ||
           *s->p == '\r') {
        s->p++;
    }
    return *s->p != '\0';
}

/* Reads the next tag. Returns: 1 open tag, 2 close tag, 0 eof/-1 bad.
 * name/out_attrs are heap copies (attrs = raw attr block, parsed by
 * the caller); self_closing reports "<tag/>". */
static int xml_next_tag(xml_scan *s, char **name, char **attrs,
                        int *self_closing) {
    *name = NULL;
    *attrs = NULL;
    *self_closing = 0;
    if (!xml_skip_ws(s)) {
        return 0;
    }
    if (*s->p != '<') {
        return -1;
    }
    s->p++;
    int close = 0;
    if (*s->p == '/') {
        close = 1;
        s->p++;
    }
    const char *nstart = s->p;
    while (*s->p != '\0' && *s->p != '>' && *s->p != ' ' &&
           *s->p != '/' && *s->p != '\t' && *s->p != '\n' && *s->p != '\r') {
        s->p++;
    }
    size_t nlen = (size_t)(s->p - nstart);
    if (nlen == 0) {
        return -1;
    }
    *name = fdk_alloc(nlen + 1);
    if (*name == NULL) {
        return -1;
    }
    memcpy(*name, nstart, nlen);
    (*name)[nlen] = '\0';
    /* attributes until '>' (a '/' directly before '>' = self-close) */
    const char *astart = s->p;
    while (*s->p != '\0' && *s->p != '>') {
        s->p++;
    }
    if (*s->p == '\0') {
        fdk_free(*name);
        *name = NULL;
        return -1;
    }
    size_t alen = (size_t)(s->p - astart);
    if (alen > 0 && astart[alen - 1] == '/') {
        alen--;
        *self_closing = 1;
    }
    /* also trim trailing whitespace of the attr block */
    while (alen > 0 && (astart[alen - 1] == ' ' || astart[alen - 1] == '\t' ||
                        astart[alen - 1] == '\n' || astart[alen - 1] == '\r')) {
        alen--;
    }
    if (alen > 0) {
        *attrs = fdk_alloc(alen + 1);
        if (*attrs == NULL) {
            fdk_free(*name);
            *name = NULL;
            return -1;
        }
        memcpy(*attrs, astart, alen);
        (*attrs)[alen] = '\0';
    }
    s->p++; /* past '>' */
    return close ? 2 : 1;
}

/* Finds attr `key` in the raw attr block; decodes the five XML
 * entities; returns a heap copy or NULL. */
static char *xml_attr(char *attrs, const char *key) {
    if (attrs == NULL) {
        return NULL;
    }
    size_t klen = strlen(key);
    const char *p = attrs;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        const char *eq = strchr(p, '=');
        if (eq == NULL || (size_t)(eq - p) != klen ||
            strncmp(p, key, klen) != 0) {
            /* skip to next attr (past value) */
            const char *sp = strchr(p, ' ');
            if (sp == NULL) {
                break;
            }
            p = sp + 1;
            continue;
        }
        const char *v = eq + 1;
        if (*v != '"') {
            break;
        }
        v++;
        const char *ve = strchr(v, '"');
        if (ve == NULL) {
            break;
        }
        size_t raw_n = (size_t)(ve - v);
        char *out = fdk_alloc(raw_n * 6 + 1); /* entity worst case */
        if (out == NULL) {
            return NULL;
        }
        size_t w = 0;
        for (size_t i = 0; i < raw_n; i++) {
            if (v[i] == '&') {
                if (strncmp(v + i, "&amp;", 5) == 0) {
                    out[w++] = '&';
                    i += 4;
                } else if (strncmp(v + i, "&lt;", 4) == 0) {
                    out[w++] = '<';
                    i += 3;
                } else if (strncmp(v + i, "&gt;", 4) == 0) {
                    out[w++] = '>';
                    i += 3;
                } else if (strncmp(v + i, "&quot;", 6) == 0) {
                    out[w++] = '"';
                    i += 5;
                } else if (strncmp(v + i, "&apos;", 6) == 0) {
                    out[w++] = '\'';
                    i += 5;
                } else {
                    out[w++] = '&';
                }
            } else {
                out[w++] = v[i];
            }
        }
        out[w] = '\0';
        return out;
    }
    return NULL;
}

static void name_list_push(char ***list, size_t *n, const char *name) {
    char **grown = fdk_realloc(*list, (*n + 1) * sizeof(char *));
    if (grown == NULL) {
        return;
    }
    *list = grown;
    char *copy = dup_str(name);
    if (copy == NULL) {
        return;
    }
    (*list)[(*n)++] = copy;
}

fdk_dbus_introspection *fdk_dbus_introspection_parse(const char *xml) {
    if (xml == NULL) {
        return NULL;
    }
    fdk_dbus_introspection *node = fdk_alloc(sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    xml_scan s = {xml};
    struct fdk_dbus_ispec *cur = NULL;
    int depth = 0;
    int seen_root = 0;
    (void)0;
    for (;;) {
        char *name = NULL;
        char *attrs = NULL;
        int self_closing = 0;
        int kind = xml_next_tag(&s, &name, &attrs, &self_closing);
        if (kind <= 0) {
            break;
        }
        if (name[0] == '!') {
            /* DOCTYPE (every real introspection reply carries the
             * standard one) and comments: no depth, no state. */
            fdk_free(name);
            fdk_free(attrs);
            continue;
        }
        if (kind == 2) { /* close */
            if (depth > 0) {
                depth--;
            }
            if (strcmp(name, "interface") == 0) {
                cur = NULL;
            }
            fdk_free(name);
            fdk_free(attrs);
            continue;
        }
        if (!self_closing) {
            depth++;
        }
        if (strcmp(name, "node") == 0) {
            char *n = xml_attr(attrs, "name");
            if (n != NULL) {
                if (!seen_root) {
                    fdk_free(node->node_name);
                    node->node_name = n;
                    n = NULL;
                } else if (depth == 1) {
                    /* a direct child of the root node */
                    name_list_push(&node->children, &node->child_n, n);
                    fdk_free(n);
                    n = NULL;
                } else {
                    fdk_free(n); /* deeper nested node: not a child */
                }
            }
            if (!seen_root && !self_closing) {
                seen_root = 1;
            }
        } else if (strcmp(name, "interface") == 0) {
            char *n = xml_attr(attrs, "name");
            struct fdk_dbus_ispec *grown = fdk_realloc(
                node->ifaces, (node->iface_n + 1) * sizeof(*grown));
            if (grown != NULL && n != NULL) {
                node->ifaces = grown;
                cur = &node->ifaces[node->iface_n++];
                memset(cur, 0, sizeof(*cur));
                cur->name = n;
            } else {
                fdk_free(n);
            }
        } else if (cur != NULL && strcmp(name, "method") == 0) {
            char *n = xml_attr(attrs, "name");
            if (n != NULL) {
                name_list_push(&cur->methods, &cur->method_n, n);
                fdk_free(n);
            }
        } else if (cur != NULL && strcmp(name, "signal") == 0) {
            char *n = xml_attr(attrs, "name");
            if (n != NULL) {
                name_list_push(&cur->signals, &cur->signal_n, n);
                fdk_free(n);
            }
        } else if (cur != NULL && strcmp(name, "property") == 0) {
            char *n = xml_attr(attrs, "name");
            char *acc = xml_attr(attrs, "access");
            if (n != NULL) {
                char **pg = fdk_realloc(cur->props,
                                        (cur->prop_n + 1) * sizeof(char *));
                char **ag = fdk_realloc(
                    cur->prop_access, (cur->prop_n + 1) * sizeof(char *));
                if (pg != NULL && ag != NULL) {
                    cur->props = pg;
                    cur->prop_access = ag;
                    cur->props[cur->prop_n] = n;
                    cur->prop_access[cur->prop_n] =
                        acc ? acc : dup_str("read");
                    cur->prop_n++;
                    if (acc == NULL) {
                        /* default already set */
                    }
                    n = NULL; /* ownership moved */
                } else {
                    fdk_free(n);
                    fdk_free(acc);
                }
            } else {
                fdk_free(acc);
            }
        }
        /* <arg>, <annotation>, unknown tags: skipped by design */
        fdk_free(name);
        fdk_free(attrs);
        (void)self_closing;
    }
    return node;
}

void fdk_dbus_introspection_free(fdk_dbus_introspection *node) {
    if (node == NULL) {
        return;
    }
    for (size_t i = 0; i < node->iface_n; i++) {
        struct fdk_dbus_ispec *is = &node->ifaces[i];
        fdk_free(is->name);
        for (size_t k = 0; k < is->method_n; k++) {
            fdk_free(is->methods[k]);
        }
        fdk_free(is->methods);
        for (size_t k = 0; k < is->signal_n; k++) {
            fdk_free(is->signals[k]);
        }
        fdk_free(is->signals);
        for (size_t k = 0; k < is->prop_n; k++) {
            fdk_free(is->props[k]);
            fdk_free(is->prop_access[k]);
        }
        fdk_free(is->props);
        fdk_free(is->prop_access);
    }
    fdk_free(node->ifaces);
    for (size_t i = 0; i < node->child_n; i++) {
        fdk_free(node->children[i]);
    }
    fdk_free(node->children);
    fdk_free(node->node_name);
    fdk_free(node);
}

const char *fdk_dbus_introspection_node_name(
    const fdk_dbus_introspection *node) {
    return (node != NULL) ? node->node_name : NULL;
}

size_t fdk_dbus_introspection_interface_count(
    const fdk_dbus_introspection *node) {
    return (node != NULL) ? node->iface_n : 0;
}

const char *fdk_dbus_introspection_interface_name(
    const fdk_dbus_introspection *node, size_t index) {
    if (node == NULL || index >= node->iface_n) {
        return NULL;
    }
    return node->ifaces[index].name;
}

int fdk_dbus_introspection_find_interface(
    const fdk_dbus_introspection *node, const char *interface_name) {
    if (node == NULL || interface_name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < node->iface_n; i++) {
        if (strcmp(node->ifaces[i].name, interface_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

size_t fdk_dbus_introspection_method_count(
    const fdk_dbus_introspection *node, int interface_index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n) {
        return 0;
    }
    return node->ifaces[interface_index].method_n;
}

const char *fdk_dbus_introspection_method_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n ||
        index >= node->ifaces[interface_index].method_n) {
        return NULL;
    }
    return node->ifaces[interface_index].methods[index];
}

size_t fdk_dbus_introspection_signal_count(
    const fdk_dbus_introspection *node, int interface_index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n) {
        return 0;
    }
    return node->ifaces[interface_index].signal_n;
}

const char *fdk_dbus_introspection_signal_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n ||
        index >= node->ifaces[interface_index].signal_n) {
        return NULL;
    }
    return node->ifaces[interface_index].signals[index];
}

size_t fdk_dbus_introspection_property_count(
    const fdk_dbus_introspection *node, int interface_index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n) {
        return 0;
    }
    return node->ifaces[interface_index].prop_n;
}

const char *fdk_dbus_introspection_property_name(
    const fdk_dbus_introspection *node, int interface_index, size_t index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n ||
        index >= node->ifaces[interface_index].prop_n) {
        return NULL;
    }
    return node->ifaces[interface_index].props[index];
}

const char *fdk_dbus_introspection_property_access(
    const fdk_dbus_introspection *node, int interface_index, size_t index) {
    if (node == NULL || interface_index < 0 ||
        (size_t)interface_index >= node->iface_n ||
        index >= node->ifaces[interface_index].prop_n) {
        return NULL;
    }
    return node->ifaces[interface_index].prop_access[index];
}

size_t fdk_dbus_introspection_child_count(
    const fdk_dbus_introspection *node) {
    return (node != NULL) ? node->child_n : 0;
}

const char *fdk_dbus_introspection_child_name(
    const fdk_dbus_introspection *node, size_t index) {
    if (node == NULL || index >= node->child_n) {
        return NULL;
    }
    return node->children[index];
}

/* The Introspect call with its typed reply bridge. */
static void introspect_reply_cb(fdk_dbus_connection *c, fdk_dbus_message *m,
                                void *user) {
    /* user packs the app callback + user_data (freed here). */
    struct {
        void (*fn)(fdk_dbus_connection *, fdk_dbus_introspection *,
                   const char *, void *);
        void *ud;
    } *bridge = user;
    void (*fn)(fdk_dbus_connection *, fdk_dbus_introspection *, const char *,
               void *) = bridge->fn;
    void *ud = bridge->ud;
    fdk_dbus_introspection *parsed = NULL;
    const char *xml = NULL;
    char *xml_owned = NULL;
    if (m != NULL && !fdk_dbus_message_is_error(m)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(m, &args, &n) == FDK_OK && n >= 1 &&
            args != NULL) {
            xml_owned = dup_str(fdk_dbus_value_string(args[0]));
            fdk_dbus_values_free_array(args, n);
        }
    }
    if (xml_owned != NULL) {
        xml = xml_owned;
        parsed = fdk_dbus_introspection_parse(xml);
    }
    enter_cb(c);
    fn(c, parsed, xml, ud);
    leave_cb(c);
    fdk_dbus_introspection_free(parsed);
    fdk_free(xml_owned);
    fdk_free(bridge);
}

fdk_dbus_pending *fdk_dbus_connection_introspect(
    fdk_dbus_connection *connection, const char *destination,
    const char *path,
    void (*on_reply)(fdk_dbus_connection *connection,
                     fdk_dbus_introspection *node, const char *raw_xml,
                     void *user),
    void *user) {
    if (connection == NULL || destination == NULL || path == NULL ||
        on_reply == NULL) {
        return NULL;
    }
    struct {
        void (*fn)(fdk_dbus_connection *, fdk_dbus_introspection *,
                   const char *, void *);
        void *ud;
    } *bridge = fdk_alloc(sizeof(*bridge));
    if (bridge == NULL) {
        return NULL;
    }
    bridge->fn = on_reply;
    bridge->ud = user;
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        destination, path, "org.freedesktop.DBus.Introspectable",
        "Introspect");
    if (m == NULL) {
        fdk_free(bridge);
        return NULL;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        connection, m, FDK_DBUS_TIMEOUT_DEFAULT, introspect_reply_cb,
        bridge);
    if (p == NULL) {
        fdk_free(bridge);
    }
    return p;
}
