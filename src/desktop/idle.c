/*
 * idle.c — the idle/screensaver inhibition module (1.1.0, S-3 / B-11)
 *
 * Two mechanisms, one handle (include/fdk/fdk_idle.h carries the
 * contract and the lifecycle story):
 *
 *   - the COMPOSITOR path: the platform op
 *     window_set_idle_inhibit — Wayland's
 *     zwp_idle_inhibit_manager_v1, surface-scoped. Returns
 *     UNSUPPORTED when the compositor offers no manager (the
 *     fallback engages) or when the backend has no op at all (X11:
 *     the D-Bus service is the mechanism there). This half is owned
 *     by the WINDOW: the engagement flag rides the window (its
 *     destroy path disengages before the platform window dies) and
 *     the window keeps the inhibitor's back-reference (severed at
 *     destroy — a released-late inhibitor never touches a dead
 *     window).
 *   - the D-BUS path: org.freedesktop.ScreenSaver on the session
 *     bus — Inhibit(s, s) -> cookie (u), UnInhibit(u). The
 *     connection rides the fdk_dbus client's fd-source integration
 *     (the notify client's discipline: one connection, the ctx's
 *     pump drives it).
 *
 * LIFECYCLE (the part worth reading): the D-Bus half is async —
 * the cookie may arrive AFTER the application released the
 * inhibitor. Cancelling the pending Inhibit would orphan the
 * inhibition on the desktop (the call was already sent; only its
 * answer is dropped), so the release is instead DEFERRED: the
 * handle outlives the application's reference while the cookie is
 * in flight, and the reply callback finishes the job (UnInhibit +
 * teardown). A two-slot reference count owns that: the USER's
 * fdk_idle_uninhibit drops one, the pending call's callback drops
 * the other; the memory dies at zero. Error/NULL replies release
 * the inflight slot without an UnInhibit (there is no cookie to
 * un-inhibit — the desktop never engaged).
 */

#define FDK_LOG_TAG "idle"

#include "fdk/fdk_idle.h"
#include "fdk/fdk_dbus.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "window/window_internal.h"

#include <string.h>

#define IDLE_BUS_NAME "org.freedesktop.ScreenSaver"
#define IDLE_BUS_PATH "/org/freedesktop/ScreenSaver"
#define IDLE_BUS_IFACE "org.freedesktop.ScreenSaver"

/* The reference slots: (1) the USER's handle, (2) the BUS slot
 * (from fdk_dbus_connect until on_bus_ready), (3) the PENDING
 * slot (from send_inhibit until on_inhibit_reply). The handle's
 * memory dies when the last slot drops — the deferred-release
 * story in the header. */
struct fdk_idle_inhibitor {
    fdk_context *ctx;
    fdk_window *window;  /* borrowed; NULL once severed (dead or
                          * app-wide) — the compositor half's target */
    char *app_name;      /* owned copy (the deferred Inhibit needs it) */
    char *reason;        /* owned copy */
    int refs;            /* user handle + the inflight Inhibit call */
    int released;        /* the user called uninhibit */
    int wl_engaged;      /* the platform inhibitor is up (window owns
                          * the actual proxy) */
    int status;          /* fdk_idle_inhibit_status */
    fdk_dbus_connection *bus;   /* NULL until/unless the D-Bus path */
    fdk_dbus_pending *pending;  /* the Inhibit call in flight */
    uint32_t cookie;            /* 0 = none yet */
};

/* ---- internal (window.c calls these; window_internal.h declares) -------- */

/* The window is dying (or the inhibitor engages app-wide): sever the
 * compositor-half coupling. The window's own destroy path disengaged
 * the platform inhibitor already; the handle must never touch the
 * window again. */
void fdk__idle_inhibitor_sever(fdk_idle_inhibitor *inhib) {
    if (inhib != NULL) {
        inhib->window = NULL;
        inhib->wl_engaged = 0;
    }
}

static void inhib_unref(fdk_idle_inhibitor *inhib) {
    if (inhib == NULL) {
        return;
    }
    if (--inhib->refs > 0) {
        return;
    }
    /* Both slots dropped: the pending is done (canceled or fired)
     * and the user released. The only writes that could still be
     * queued on the bus are the fire-and-forget UnInhibit — closing
     * now is the notify client's teardown discipline. */
    if (inhib->bus != NULL) {
        fdk_dbus_connection_close(inhib->bus);
        inhib->bus = NULL;
    }
    fdk_free(inhib->app_name);
    fdk_free(inhib->reason);
    fdk_free(inhib);
}

/* ---- the D-Bus half ------------------------------------------------------ */

static void on_inhibit_reply(fdk_dbus_connection *bus,
                             fdk_dbus_message *reply, void *user);

static void send_uninhibit(fdk_idle_inhibitor *inhib) {
    if (inhib->bus == NULL || inhib->cookie == 0) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        IDLE_BUS_NAME, IDLE_BUS_PATH, IDLE_BUS_IFACE, "UnInhibit");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, inhib->cookie))) {
        fdk_dbus_message_free(call);
        return;
    }
    /* Fire-and-forget: the reply carries nothing we need. */
    if (fdk_dbus_connection_call(inhib->bus, call,
                                 FDK_DBUS_TIMEOUT_DEFAULT,
                                 NULL, NULL) == NULL) {
        fdk_dbus_message_free(call);
    }
    inhib->cookie = 0;
}

static void send_inhibit(fdk_idle_inhibitor *inhib) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        IDLE_BUS_NAME, IDLE_BUS_PATH, IDLE_BUS_IFACE, "Inhibit");
    if (call == NULL) {
        inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
        return;
    }
    const char *app = (inhib->app_name != NULL && inhib->app_name[0])
                          ? inhib->app_name
                          : "FDK application";
    const char *reason =
        (inhib->reason != NULL) ? inhib->reason : "";
    if (!fdk_ok(fdk_dbus_message_append_string(call, app)) ||
        !fdk_ok(fdk_dbus_message_append_string(call, reason))) {
        fdk_dbus_message_free(call);
        inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
        return;
    }
    /* The inflight reference: on_inhibit_reply (or the teardown
     * cancel — it always runs at-most-once with a NULL reply)
     * drops it. */
    inhib->refs++;
    inhib->pending = fdk_dbus_connection_call(
        inhib->bus, call, FDK_DBUS_TIMEOUT_DEFAULT, on_inhibit_reply,
        inhib);
    if (inhib->pending == NULL) {
        fdk_dbus_message_free(call);
        inhib->refs--; /* never went out */
        inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
    } else {
        inhib->status = FDK_IDLE_INHIBIT_PENDING;
    }
}

static void on_inhibit_reply(fdk_dbus_connection *bus,
                             fdk_dbus_message *reply, void *user) {
    (void)bus;
    fdk_idle_inhibitor *inhib = user;
    inhib->pending = NULL;

    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t argc_ = 0;
        if (fdk_ok(fdk_dbus_message_read_args(reply, &args, &argc_)) &&
            argc_ == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
            fdk_dbus_value_uint32(args[0]) != 0) {
            inhib->cookie = fdk_dbus_value_uint32(args[0]);
            fdk_dbus_values_free_array(args, argc_);
            inhib->status = FDK_IDLE_INHIBIT_ACTIVE;
            if (inhib->released) {
                /* The user let go before the cookie landed: finish
                 * the release NOW (the deferred path — cancelling
                 * back then would have orphaned the inhibition). */
                send_uninhibit(inhib);
            }
            inhib_unref(inhib); /* the inflight slot */
            return;
        }
        fdk_dbus_values_free_array(args, argc_);
        FDK_INFO("idle: malformed ScreenSaver Inhibit reply");
    } else if (reply != NULL) {
        const char *name = fdk_dbus_message_error_name(reply);
        FDK_INFO("idle: ScreenSaver Inhibit refused (%s)",
                 name ? name : "error");
    } else {
        FDK_INFO("idle: ScreenSaver Inhibit no reply "
                 "(timeout or bus loss)");
    }
    /* No cookie: the desktop never engaged — nothing to
     * un-inhibit. */
    inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
    inhib_unref(inhib); /* the inflight slot */
}

static void on_bus_ready(fdk_dbus_connection *bus, int ok,
                         const char *error, void *user) {
    fdk_idle_inhibitor *inhib = user;
    if (inhib->bus != bus) {
        return; /* a stale ready from an abandoned connection */
    }
    if (!ok) {
        FDK_INFO("idle: session bus connect failed (%s)",
                 error ? error : "?");
        inhib->bus = NULL;
        inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
        /* Close FIRST (the notify client's sanctioned in-callback
         * close), then drop the bus slot — this may be the last
         * reference (the user released while the handshake ran). */
        fdk_dbus_connection_close(bus);
        inhib_unref(inhib);
        return;
    }
    /* Send FIRST (the pending adds its own reference), then drop
     * the bus slot — the ordering keeps the count never-zero between
     * the two. */
    send_inhibit(inhib);
    inhib_unref(inhib);
}

/* ---- public ---------------------------------------------------------------- */

fdk_result fdk_idle_inhibit(fdk_context *ctx, fdk_window *window,
                            const char *app_name, const char *reason,
                            fdk_idle_inhibitor **out) {
    if (ctx == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;

    fdk_idle_inhibitor *inhib = fdk_alloc(sizeof(*inhib));
    if (inhib == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(inhib, 0, sizeof(*inhib));
    inhib->ctx = ctx;
    inhib->refs = 1; /* the user's handle */
    inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
    if (app_name != NULL && app_name[0] != '\0') {
        inhib->app_name = fdk_alloc(strlen(app_name) + 1);
        if (inhib->app_name != NULL) {
            memcpy(inhib->app_name, app_name, strlen(app_name) + 1);
        }
    }
    if (reason != NULL && reason[0] != '\0') {
        inhib->reason = fdk_alloc(strlen(reason) + 1);
        if (inhib->reason != NULL) {
            memcpy(inhib->reason, reason, strlen(reason) + 1);
        }
    }

    /* The compositor path first (the authoritative mechanism
     * wherever it exists). One inhibitor per window. */
    if (window != NULL) {
        if (window->idle_inhib != NULL) {
            fdk_free(inhib->app_name);
            fdk_free(inhib->reason);
            fdk_free(inhib);
            return FDK_ERR_INVALID_STATE; /* release the live one */
        }
        if (ctx->ops != NULL &&
            ctx->ops->window_set_idle_inhibit != NULL) {
            fdk_result r = ctx->ops->window_set_idle_inhibit(
                window->pwindow, 1);
            if (fdk_ok(r)) {
                inhib->wl_engaged = 1;
                inhib->window = window;
                inhib->status = FDK_IDLE_INHIBIT_ACTIVE;
                window->idle_inhibited = 1;
                window->idle_inhib = inhib;
                *out = inhib;
                return FDK_OK;
            }
            if (r != FDK_ERR_UNSUPPORTED) {
                fdk_free(inhib->app_name);
                fdk_free(inhib->reason);
                fdk_free(inhib);
                return r; /* a real failure, not a missing manager */
            }
            /* UNSUPPORTED: no compositor manager — the D-Bus
             * fallback (the window stays unengaged; the inhibitor
             * rides the bus app-wide). */
        }
    }

    /* The D-Bus path (app-wide inhibits, X11, manager-less
     * compositors). The connection holds its own reference from
     * this moment: on_bus_ready drops it (a user release during
     * the handshake must not free the memory the callback is
     * about to touch). */
    inhib->refs++; /* the bus slot */
    inhib->bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS,
                                  on_bus_ready, inhib);
    if (inhib->bus == NULL) {
        inhib->refs--; /* the connection never existed */
        inhib->status = FDK_IDLE_INHIBIT_UNSUPPORTED;
        *out = inhib; /* inert handle; release still required */
        return FDK_OK;
    }
    inhib->status = FDK_IDLE_INHIBIT_PENDING;
    *out = inhib;
    return FDK_OK;
}

void fdk_idle_uninhibit(fdk_idle_inhibitor *inhib) {
    if (inhib == NULL) {
        return;
    }
    inhib->released = 1;

    if (inhib->wl_engaged && inhib->window != NULL) {
        fdk_window *w = inhib->window;
        if (w->idle_inhib == inhib) {
            w->idle_inhib = NULL;
            w->idle_inhibited = 0;
        }
        if (w->ctx != NULL && w->ctx->ops != NULL &&
            w->ctx->ops->window_set_idle_inhibit != NULL) {
            (void)w->ctx->ops->window_set_idle_inhibit(w->pwindow, 0);
        }
        inhib->window = NULL;
        inhib->wl_engaged = 0;
    }

    if (inhib->cookie != 0) {
        send_uninhibit(inhib);
    }
    /* Cookie still in flight: the deferred release — the reply
     * callback sends the UnInhibit and drops the inflight ref. */
    inhib_unref(inhib); /* the user's handle */
}

fdk_idle_inhibit_status fdk_idle_inhibitor_status(
    const fdk_idle_inhibitor *inhib) {
    return inhib != NULL
               ? (fdk_idle_inhibit_status)inhib->status
               : FDK_IDLE_INHIBIT_UNSUPPORTED;
}
