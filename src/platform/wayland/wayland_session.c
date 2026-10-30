/*
 * wayland_session.c — the xdg-session-management-v1 half of D-5 (1.1.0)
 *
 * The Wayland side of include/fdk/fdk_session.h: the compositor's
 * save/restore object lane. xdg_session_manager_v1 (staging,
 * wayland-protocols 1.48+) mints xdg_session_v1 objects keyed by a
 * client-chosen session id string; each window joins through
 * xdg_toplevel_session_v1 (restore_toplevel BEFORE the surface's
 * first commit when the compositor might remember it, add_toplevel
 * after), and the compositor keeps the window-management state
 * (workspace, stacking, size/position) updated on its own — there
 * is no "save now" cue on this side of the desktop split; the X11
 * WM_SAVE_YOURSELF lane is the whole of that half.
 *
 * The state machine is the protocol's own shape: PENDING from
 * open() until the compositor answers created (a NEW id — persist
 * it) or restored (prior state, at least partially, back);
 * replaced means another client took the session over and this
 * object is inert. The notify is the coarse D-014 callback
 * discipline: one function, re-query state/id.
 *
 * OWNERSHIP (the part the protocol leaves to clients): the tracker
 * owns the manager + session proxies, the id storage, and a JOIN
 * TABLE of the windows attached to it — every join record on a
 * window points back (session_wire for leave's remove_toplevel
 * request, session_tracker for the reverse sweep). Tracker
 * destroy/remove sweeps the table (the protocol makes the
 * toplevel-session objects inert; FDK retires the proxies so the
 * windows' records go clean); window destroy unregisters through
 * fdk__wayland_session_window_destroyed WITHOUT remove_toplevel —
 * a window dying is not the application asking the compositor to
 * forget its stored state (a relaunch may want it back).
 *
 * The honest platform truth at landing time: NO released
 * compositor advertises the protocol yet (checked live: sway
 * 1.10/wlroots 0.18 and sway 1.12/wlroots 0.20 both lack the
 * global; the protocol is weeks old in wayland-protocols 1.48).
 * open() therefore answers the documented NULL on every desktop a
 * rig can currently drive — the translation bodies are proven
 * through the test seam (the D-3 tablet discipline: the listeners
 * are the real bodies, the seam mints the tracker a real
 * compositor's events would reach), and the first compositor that
 * offers the global gets a working client.
 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "fdk/fdk_session.h" /* FDK_SESSION_* — the public enums, wire values */

#include "generated/xdg-session-management-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* The id/name caps: the protocol bounds session ids and toplevel
 * names by the Wayland message maximum (~4 KB); FDK stores a copy,
 * so it caps at the same order with headroom — a longer id is
 * refused at open (INVALID_ARGUMENT), not silently truncated into
 * a DIFFERENT session. */
#define FDK_WL_SESSION_ID_MAX 1024
#define FDK_WL_SESSION_NAME_MAX 512

struct fdk_platform_session {
    fdk_platform_connection *conn;
    struct xdg_session_manager_v1 *manager; /* bound at open, dies with it */
    struct xdg_session_v1 *session;         /* the protocol object itself  */
    fdk_platform_session_notify_fn on_notify;
    void *user;
    int state;      /* FDK_SESSION_STATE_* — PENDING until the answer  */
    char id[FDK_WL_SESSION_ID_MAX]; /* "" until created/restored        */
    int dead;       /* destroy()/remove() already ran — inert guard    */
    /* The join table: every window this session attached. The sweep
     * at tracker teardown retires each window's toplevel-session
     * proxy (inert per the protocol) so no record outlives its
     * factory; window destroy unregisters through the reverse
     * pointer so the table never holds a freed window. */
    fdk_platform_window **joined;
    size_t joined_count;
    size_t joined_cap;
};

/* ---- join-table helpers ------------------------------------------------- */

static void join_register(struct fdk_platform_session *s,
                          fdk_platform_window *pwindow) {
    if (s->joined_count == s->joined_cap) {
        size_t cap = s->joined_cap ? s->joined_cap * 2 : 4;
        fdk_platform_window **grown =
            fdk_realloc(s->joined, cap * sizeof(*grown));
        if (grown == NULL) {
            /* The join still WORKS (proxy + record are set); only the
             * sweep loses this window — and the window's own destroy
             * path clears its record independently, so the failure is
             * a teardown-time leak at worst, never a stale pointer. */
            FDK_WARN("session join table full — sweep will miss a window");
            return;
        }
        s->joined = grown;
        s->joined_cap = cap;
    }
    s->joined[s->joined_count++] = pwindow;
}

static void join_unregister(struct fdk_platform_session *s,
                            fdk_platform_window *pwindow) {
    for (size_t i = 0; i < s->joined_count; i++) {
        if (s->joined[i] == pwindow) {
            s->joined[i] = s->joined[--s->joined_count];
            return;
        }
    }
}

/* Retires ONE window's join record (proxy destroyed, fields
 * cleared). `wire` is the still-live session object when the
 * removal should also TELL the compositor (leave), NULL when the
 * record simply dies (tracker teardown — inert objects take no
 * requests; window destroy — state must survive). */
static void join_retire(fdk_platform_window *pwindow,
                        struct xdg_session_v1 *wire) {
    if (pwindow == NULL || pwindow->session_tracker == NULL) {
        return;
    }
    if (wire != NULL && pwindow->session_name != NULL) {
        xdg_session_v1_remove_toplevel(wire, pwindow->session_name);
    }
    if (pwindow->session_proxy != NULL) {
        xdg_toplevel_session_v1_destroy(pwindow->session_proxy);
        pwindow->session_proxy = NULL;
    }
    fdk_free(pwindow->session_name);
    pwindow->session_name = NULL;
    pwindow->session_wire = NULL;
    pwindow->session_tracker = NULL;
}

/* ---- the session listener (the real translation body) ------------------ */

static void session_notify(struct fdk_platform_session *s) {
    if (s != NULL && s->on_notify != NULL && !s->dead) {
        s->on_notify(s->user);
    }
}

static void sess_created(void *data, struct xdg_session_v1 *session,
                         const char *session_id) {
    (void)session; /* the tracker is the listener's data — one object */
    struct fdk_platform_session *s = data;
    if (s == NULL) {
        return;
    }
    /* A NEW session: the id is the thing to persist. The protocol
     * says "at most once, some time after get_session" — arriving
     * after restored would be a compositor bug, but the LAST word
     * still wins (the honest resolution order). */
    snprintf(s->id, sizeof(s->id), "%s",
             session_id != NULL ? session_id : "");
    s->state = FDK_SESSION_STATE_CREATED;
    FDK_DEBUG("session created (id %.32s...)", s->id);
    session_notify(s);
}

static void sess_restored(void *data, struct xdg_session_v1 *session) {
    (void)session;
    struct fdk_platform_session *s = data;
    if (s == NULL) {
        return;
    }
    /* Prior state at least partially restored; the id we passed in
     * stays valid (it IS the same session). */
    s->state = FDK_SESSION_STATE_RESTORED;
    FDK_DEBUG("session restored (id %.32s...)", s->id);
    session_notify(s);
}

static void sess_replaced(void *data, struct xdg_session_v1 *session) {
    (void)session;
    struct fdk_platform_session *s = data;
    if (s == NULL) {
        return;
    }
    /* Another client took the session over: this object (and every
     * join record's proxy) is inert. The app destroys and may open a
     * fresh one; FDK does NOT auto-teardown app-owned records — the
     * normal destroy/leave paths are safe on inert objects. */
    s->state = FDK_SESSION_STATE_REPLACED;
    FDK_INFO("session replaced by another client — this one is inert");
    session_notify(s);
}

static const struct xdg_session_v1_listener g_session_listener = {
    .created = sess_created,
    .restored = sess_restored,
    .replaced = sess_replaced,
};

/* ---- the toplevel-session listener (the per-window body) ---------------
 *
 * The listener's user data is the WINDOW's platform record — the
 * join is per-window state (pwindow->session_proxy), and the
 * restored event dispatches straight to that window's callback,
 * BEFORE the first configure (the protocol's own ordering). */

static void toplevel_session_restored(void *data,
                                      struct xdg_toplevel_session_v1 *tsp) {
    (void)tsp;
    fdk_platform_window *pwindow = data;
    if (pwindow == NULL || pwindow->session_proxy == NULL) {
        return; /* stale event after leave/destroy — inert, per spec */
    }
    fdk_event_data event = { .type = FDK_EVENT_SESSION_RESTORED };
    pwindow->conn->dispatch(pwindow, &event, pwindow->conn->dispatch_user_data);
}

static const struct xdg_toplevel_session_v1_listener
    g_toplevel_session_listener = {
    .restored = toplevel_session_restored,
};

/* ---- the ops (platform_internal.h's D-5 block) -------------------------- */

fdk_result fdk_wayland_session_open(
    fdk_platform_connection *conn, int reason, const char *restore_id,
    fdk_platform_session_notify_fn on_notify, void *user,
    struct fdk_platform_session **out) {
    if (conn == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->session_manager_name == 0) {
        /* Not offered (every wlroots compositor at landing time,
         * GNOME, kiosk shells): the documented degradation — the
         * public open answers NULL. */
        return FDK_ERR_UNSUPPORTED;
    }
    if (reason < FDK_SESSION_REASON_LAUNCH ||
        reason > FDK_SESSION_REASON_RESTORE) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (restore_id != NULL && strlen(restore_id) >= FDK_WL_SESSION_ID_MAX) {
        /* Would arrive at the compositor as a DIFFERENT session after
         * any truncation — refuse rather than silently fork state. */
        return FDK_ERR_INVALID_ARGUMENT;
    }

    struct fdk_platform_session *s = fdk_alloc(sizeof(*s));
    if (s == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* LAZY bind (the D-4 shape): get_session is client-initiated, so
     * there is no bind-time burst to lose — but binding here (not at
     * registry time) keeps the protocol off the wire for apps that
     * never open a session. Version 1 is the whole protocol. */
    s->manager = wl_registry_bind(
        conn->registry, conn->session_manager_name,
        &xdg_session_manager_v1_interface, 1);
    if (s->manager == NULL) {
        fdk_free(s);
        return FDK_ERR_UNSUPPORTED;
    }
    s->conn = conn;
    s->on_notify = on_notify;
    s->user = user;
    s->state = FDK_SESSION_STATE_PENDING;
    s->id[0] = '\0';
    if (restore_id != NULL) {
        snprintf(s->id, sizeof(s->id), "%s", restore_id);
    }
    s->dead = 0;
    s->joined = NULL;
    s->joined_count = 0;
    s->joined_cap = 0;

    /* The session object + listener BEFORE any roundtrip can answer
     * (the listener-on-proxy rule; created/restored arrive "some
     * time after get_session", i.e. the next pump's dispatch). NULL
     * id = mint fresh; a passed id either restores or — the
     * protocol's own degradation — is treated as NULL by the
     * compositor (a `created` event follows instead). */
    s->session = xdg_session_manager_v1_get_session(
        s->manager, (uint32_t)reason,
        (restore_id != NULL && restore_id[0] != '\0') ? restore_id : NULL);
    if (s->session == NULL) {
        xdg_session_manager_v1_destroy(s->manager);
        fdk_free(s);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    xdg_session_v1_add_listener(s->session, &g_session_listener, s);
    *out = s;
    return FDK_OK;
}

/* The tracker teardown core shared by destroy (keep state) and
 * remove (forget state): retire every join record — the protocol
 * makes the toplevel-session objects inert with the session — then
 * retire the session itself. */
static void session_teardown(struct fdk_platform_session *s, int forget) {
    if (s == NULL) {
        return;
    }
    s->dead = 1; /* in-notify destroy guard (the notify is sync) */
    while (s->joined_count > 0) {
        fdk_platform_window *pwindow = s->joined[s->joined_count - 1];
        join_retire(pwindow, NULL);
        join_unregister(s, pwindow);
    }
    fdk_free(s->joined);
    s->joined = NULL;
    s->joined_count = 0;
    s->joined_cap = 0;
    if (s->session != NULL) {
        if (forget) {
            /* .remove: the compositor deletes the stored state — the
             * "do not restore me next time" call. */
            xdg_session_v1_remove(s->session);
        } else {
            /* .destroy: PRESERVES the stored state — the relaunch
             * path (the whole point of the id round trip). */
            xdg_session_v1_destroy(s->session);
        }
        s->session = NULL;
    }
    if (s->manager != NULL) {
        xdg_session_manager_v1_destroy(s->manager);
        s->manager = NULL;
    }
    fdk_free(s);
}

void fdk_wayland_session_destroy(struct fdk_platform_session *s) {
    session_teardown(s, 0);
}

void fdk_wayland_session_remove(struct fdk_platform_session *s) {
    session_teardown(s, 1);
}

const char *fdk_wayland_session_id(const struct fdk_platform_session *s) {
    if (s == NULL || s->id[0] == '\0') {
        return NULL;
    }
    return s->id;
}

int fdk_wayland_session_state(const struct fdk_platform_session *s) {
    if (s == NULL) {
        return FDK_SESSION_STATE_PENDING;
    }
    return s->state;
}

fdk_result fdk_wayland_window_join_session(
    fdk_platform_window *pwindow, struct fdk_platform_session *session,
    const char *name) {
    if (pwindow == NULL || session == NULL || name == NULL ||
        name[0] == '\0') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->conn != session->conn) {
        /* Cross-connection join: never legal (the proxy would name a
         * toplevel the other connection's compositor never saw). */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->xdg_toplevel == NULL) {
        /* Popups (xdg_popup role), subsurfaces (no role), ext kind
         * surfaces (layer-shell &c): no xdg_toplevel, no join — the
         * public header documents the refusal. A window that can
         * NEVER join is an argument problem, checked before the
         * session's own liveness (a dead session does not make a
         * popup joinable — the rig caught the order). */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (pwindow->session_proxy != NULL) {
        /* Already joined (this or any other session) — the protocol
         * would raise already_added on the second join; FDK refuses
         * before the wire. */
        return FDK_ERR_INVALID_STATE;
    }
    if (session->dead || session->session == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    if (strlen(name) >= FDK_WL_SESSION_NAME_MAX) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* The join record: name copy (remove_toplevel names windows by
     * STRING, not object) + the proxy with its listener attached
     * before any answer can arrive. */
    char *name_copy = fdk_alloc(strlen(name) + 1);
    if (name_copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(name_copy, name, strlen(name) + 1);

    struct xdg_toplevel_session_v1 *tsp;
    if (!pwindow->xdg_shown) {
        /* Pre-first-commit: the RESTORE path. The compositor applies
         * remembered window-management state with the first
         * configure; if it knows the name, xdg_toplevel_session_v1's
         * `restored` event precedes that configure and dispatches
         * FDK_EVENT_SESSION_RESTORED; if it does not, the protocol
         * says this behaves exactly like add_toplevel (no event). */
        tsp = xdg_session_v1_restore_toplevel(session->session,
                                              pwindow->xdg_toplevel, name);
    } else {
        /* Post-show (already committed once): the fresh-add path —
         * restore_toplevel here would be the already_mapped protocol
         * ERROR and kill the connection. State is recorded from now
         * on; this window cannot restore its own prior state this
         * run (the header documents the INFO posture). */
        FDK_INFO("session join after show: the fresh-add path (this "
                 "window's prior state, if any, is not restored this "
                 "run)");
        tsp = xdg_session_v1_add_toplevel(session->session,
                                          pwindow->xdg_toplevel, name);
    }
    if (tsp == NULL) {
        fdk_free(name_copy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    xdg_toplevel_session_v1_add_listener(tsp, &g_toplevel_session_listener,
                                         pwindow);
    pwindow->session_proxy = tsp;
    pwindow->session_name = name_copy;
    pwindow->session_wire = session->session;
    pwindow->session_tracker = session;
    join_register(session, pwindow);
    return FDK_OK;
}

void fdk_wayland_window_leave_session(fdk_platform_window *pwindow) {
    if (pwindow == NULL || pwindow->session_tracker == NULL) {
        return;
    }
    /* remove_toplevel by NAME (the request's own signature) — the
     * compositor forgets this window's stored state; the proxy goes
     * inert and is destroyed after the request. The session object
     * itself stays live — other windows keep their state. */
    struct fdk_platform_session *s = pwindow->session_tracker;
    struct xdg_session_v1 *wire = pwindow->session_wire;
    if (s->dead) {
        /* The tracker was torn down but the sweep missed this window
         * (the join-table growth warning, or an in-notify leave):
         * the wire object is gone with it — retire the record only. */
        wire = NULL;
    }
    join_retire(pwindow, wire);
    join_unregister(s, pwindow);
}

void fdk__wayland_session_window_destroyed(fdk_platform_window *pwindow) {
    if (pwindow == NULL || pwindow->session_tracker == NULL) {
        return;
    }
    /* The window is dying: retire its record WITHOUT remove_toplevel
     * (dying is not the app asking the compositor to FORGET the
     * stored state — a relaunch may want it back) and unregister so
     * the tracker's own sweep never meets a freed window. */
    struct fdk_platform_session *s = pwindow->session_tracker;
    join_retire(pwindow, NULL);
    join_unregister(s, pwindow);
}

/* ---- the test seam (the D-3 discipline: real bodies, rig-minted state) --
 *
 * No released compositor offers the protocol yet, so the suite
 * drives the REAL listener bodies on a rig-minted tracker: the
 * same struct a live compositor's events would reach, the same
 * functions the proxy dispatch would call. When a compositor
 * adopts the protocol the live legs replace these without the
 * bodies changing (that is the point of the seam). The mint gives
 * the suite a tracker in exactly the post-open state on a real
 * connection (conn wired, PENDING, no id); destroy retires it
 * through the REAL teardown (no proxies to sweep, a clean free). */
struct fdk_platform_session *fdk__wayland_session_test_mint(
    fdk_platform_connection *conn,
    fdk_platform_session_notify_fn on_notify, void *user) {
    struct fdk_platform_session *s = fdk_alloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->conn = conn;
    s->manager = NULL;
    s->session = NULL;
    s->on_notify = on_notify;
    s->user = user;
    s->state = FDK_SESSION_STATE_PENDING;
    s->id[0] = '\0';
    s->dead = 0;
    s->joined = NULL;
    s->joined_count = 0;
    s->joined_cap = 0;
    return s;
}

void fdk__wayland_session_test_created(struct fdk_platform_session *s,
                                       const char *id) {
    sess_created(s, NULL, id);
}

void fdk__wayland_session_test_restored(struct fdk_platform_session *s) {
    sess_restored(s, NULL);
}

void fdk__wayland_session_test_replaced(struct fdk_platform_session *s) {
    sess_replaced(s, NULL);
}

void fdk__wayland_session_test_toplevel_restored(
    fdk_platform_window *pwindow) {
    toplevel_session_restored(pwindow, NULL);
}
