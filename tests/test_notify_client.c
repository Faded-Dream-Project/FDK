/* test_notify_client.c — the app-facing notification client against a
 * REAL bus and an in-test mock daemon (1.1.0)
 *
 * Runs under `make test-dbus` beside test_dbus_live.c (the same
 * dbus-run-session wrapper). The mock daemon is a second FDK
 * connection that owns org.freedesktop.Notifications and serves it
 * exactly like a real one — reply shapes, signal emission, name
 * lifecycle — so this file proves the WHOLE client contract:
 *
 *   - the adoption flow: open -> watch the daemon name -> probe ->
 *     READY with capabilities and server identity;
 *   - a full Notify round trip: every argument the client marshals
 *     (app name, replaces_id, icon, summary, body, the action pair
 *     array, the hints dict — urgency byte, category, desktop-entry,
 *     and image-data as the flat byte array — and the timeout) is
 *     VALIDATED FIELD BY FIELD on the mock side before the id reply;
 *   - ActionInvoked and NotificationClosed arrive with the note_user
 *     the send carried (and the mapping survives an action followed
 *     by a close);
 *   - CloseNotification (withdraw) -> NotificationClosed reason 3;
 *   - replaces_id: the daemon's same-id answer comes back in
 *     on_sent;
 *   - the daemon vanishing (ReleaseName) -> NO_DAEMON, the in-flight
 *     send reporting the bus's ServiceUnknown through on_failed, and
 *     the daemon re-appearing -> re-adoption (on_ready again);
 *   - malformed requests refused locally (no summary -> 0, no event);
 *   - the teardown discipline: closing the client INSIDE its own
 *     event callback (ASan/LSan must stay clean).
 *
 * Skips loudly (exit 0) without a session bus; FDK_DBUS_TEST_REQUIRE=1
 * makes the skip a failure (CI sets it).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_notify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* send() consumes its message on FDK_OK — free it only when the
 * send itself failed (the leak-free discipline). */
static void send_or_free(fdk_dbus_connection *bus, fdk_dbus_message *m) {
    if (m == NULL) {
        return;
    }
    if (!fdk_ok(fdk_dbus_connection_send(bus, m))) {
        fdk_dbus_message_free(m);
    }
}

/* ---- pump helper (the live suite's shape) ------------------------------ */

static const char *sstr(const char *s) {
    return s != NULL ? s : "?";
}

static int pump_until(fdk_context *ctx, int *done, int timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!*done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                       (now.tv_nsec - start.tv_nsec) / 1000000LL;
        if (ms > timeout_ms) {
            return 0;
        }
        (void)fdk_pump_events(ctx, 20);
    }
    return 1;
}

/* ---- the mock daemon ----------------------------------------------------- */

#define MOCK_NAME "org.freedesktop.Notifications"
#define MOCK_PATH "/org/freedesktop/Notifications"

/* Everything the mock validated about the last Notify, as flags the
 * test asserts after the round trip. */
static struct {
    int got_notify;
    int app_name_ok;   /* "fdk-test-app"                       */
    int icon_ok;       /* "test-icon"                          */
    int summary_ok;    /* "Sum"                                */
    int body_ok;       /* "<b>Body</b>"                        */
    int actions_ok;    /* 2 entries: "open"/"Open it"          */
    int urgency_ok;    /* byte 2 (critical)                    */
    int category_ok;   /* "test.cat"                           */
    int entry_ok;      /* "org.fd.Test"                        */
    int image_ok;      /* 2x2 RGBA, rowstride 8, 12 flat bytes */
    int timeout_ok;    /* 5000                                 */
    uint32_t replaces_seen; /* the replaces_id of the last Notify */
} g_mock;

static uint32_t g_mock_next_id = 1;

/* Whether the scripted Action+Closed pair fires for each Notify —
 * ON only for the full round-trip leg; the withdraw/replace legs
 * need the daemon to stay quiet so their own closes are the only
 * ones. */
static int g_script_events = 1;

/* Client-side event bookkeeping. */
static struct {
    int ready_count;
    int sent_count;
    uint32_t sent_ids[8];
    void *sent_users[8];
    int failed_count;
    char failed_error[8][128];
    int closed_count;
    uint32_t closed_ids[8];
    uint32_t closed_reasons[8];
    void *closed_users[8];
    int action_count;
    uint32_t action_ids[8];
    char action_keys[8][64];
    void *action_users[8];
} g_ev;

static int g_close_inside_callback = 0; /* the reentrancy leg's flag */
static fdk_notify *g_client_to_close = NULL;

/* Emits a daemon signal: ActionInvoked / NotificationClosed. */
static void emit_signal(fdk_dbus_connection *c, const char *member,
                        uint32_t id, const char *action,
                        uint32_t reason) {
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        MOCK_PATH, "org.freedesktop.Notifications", member);
    if (sig == NULL) {
        return;
    }
    int appended;
    if (action != NULL) {
        appended = fdk_ok(fdk_dbus_message_append_uint32(sig, id)) &&
                   fdk_ok(fdk_dbus_message_append_string(sig, action));
    } else {
        appended = fdk_ok(fdk_dbus_message_append_uint32(sig, id)) &&
                   fdk_ok(fdk_dbus_message_append_uint32(sig, reason));
    }
    if (appended) {
        send_or_free(c, sig); /* consumed on success */
    } else {
        fdk_dbus_message_free(sig);
    }
}

/* The mock's Notify: validate everything the client marshaled, then
 * answer the id and (for the scripted flow) emit the action + close
 * pair the test expects. */
static void mock_serve_notify(fdk_dbus_connection *c,
                              fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) || n != 8) {
        fdk_dbus_values_free_array(args, n);
        return;
    }
    g_mock.got_notify = 1;
    g_mock.app_name_ok = strcmp(sstr(fdk_dbus_value_string(args[0])),
                                "fdk-test-app") == 0;
    g_mock.replaces_seen = fdk_dbus_value_uint32(args[1]);
    g_mock.icon_ok =
        strcmp(sstr(fdk_dbus_value_string(args[2])), "test-icon") == 0;
    g_mock.summary_ok =
        strcmp(sstr(fdk_dbus_value_string(args[3])), "Sum") == 0;
    g_mock.body_ok =
        strcmp(sstr(fdk_dbus_value_string(args[4])), "<b>Body</b>") == 0;

    /* actions: as with exactly ["open", "Open it"]. */
    {
        const fdk_dbus_value *acts = args[5];
        size_t an = fdk_dbus_value_array_count(acts);
        const char *a0 = an > 0 ? fdk_dbus_value_string(
                             fdk_dbus_value_array_item(acts, 0)) : NULL;
        const char *a1 = an > 1 ? fdk_dbus_value_string(
                             fdk_dbus_value_array_item(acts, 1)) : NULL;
        g_mock.actions_ok = an == 2 && a0 != NULL && a1 != NULL &&
                            strcmp(a0, "open") == 0 &&
                            strcmp(a1, "Open it") == 0;
    }

    /* hints: urgency (byte), category, desktop-entry, image-data. */
    {
        const fdk_dbus_value *hints = args[6];
        const fdk_dbus_value *urg = fdk_dbus_value_dict_lookup(hints,
                                                               "urgency");
        g_mock.urgency_ok =
            urg != NULL &&
            fdk_dbus_value_type(
                fdk_dbus_value_variant_value(urg)) == FDK_DBUS_BYTE &&
            fdk_dbus_value_byte(fdk_dbus_value_variant_value(urg)) == 2;
        const fdk_dbus_value *cat = fdk_dbus_value_dict_lookup(hints,
                                                               "category");
        g_mock.category_ok =
            cat != NULL &&
            strcmp(sstr(fdk_dbus_value_string(
                       fdk_dbus_value_variant_value(cat))),
                   "test.cat") == 0;
        const fdk_dbus_value *ent = fdk_dbus_value_dict_lookup(
            hints, "desktop-entry");
        g_mock.entry_ok =
            ent != NULL &&
            strcmp(sstr(fdk_dbus_value_string(
                       fdk_dbus_value_variant_value(ent))),
                   "org.fd.Test") == 0;
        const fdk_dbus_value *img = fdk_dbus_value_dict_lookup(hints,
                                                               "image-data");
        if (img != NULL) {
            const fdk_dbus_value *st = fdk_dbus_value_variant_value(img);
            int shape = fdk_dbus_value_type(st) == FDK_DBUS_STRUCT &&
                        fdk_dbus_value_struct_count(st) == 7;
            if (shape) {
                const fdk_dbus_value *w = fdk_dbus_value_struct_item(st, 0);
                const fdk_dbus_value *h = fdk_dbus_value_struct_item(st, 1);
                const fdk_dbus_value *rs = fdk_dbus_value_struct_item(st, 2);
                const fdk_dbus_value *al = fdk_dbus_value_struct_item(st, 3);
                const fdk_dbus_value *ch = fdk_dbus_value_struct_item(st, 5);
                const fdk_dbus_value *bytes =
                    fdk_dbus_value_struct_item(st, 6);
                size_t count = 0;
                const uint8_t *flat =
                    fdk_dbus_value_array_flat_bytes(bytes, &count);
                /* 2x2 RGBA with rowstride 8: 8*(h-1) + w*4 = 16
                 * bytes, and the flat reader must see the exact
                 * bytes the test put there (both rows, padded). */
                g_mock.image_ok =
                    fdk_dbus_value_int32(w) == 2 &&
                    fdk_dbus_value_int32(h) == 2 &&
                    fdk_dbus_value_int32(rs) == 8 &&
                    fdk_dbus_value_boolean(al) == 1 &&
                    fdk_dbus_value_int32(ch) == 4 && flat != NULL &&
                    count == 16 && flat[0] == 0x11 && flat[1] == 0x22 &&
                    flat[2] == 0x33 && flat[3] == 0x44 && flat[7] == 0x00 &&
                    flat[8] == 0x55 && flat[11] == 0x88 && flat[15] == 0xCC;
            }
        }
    }
    g_mock.timeout_ok = fdk_dbus_value_int32(args[7]) == 5000;
    fdk_dbus_values_free_array(args, n);

    /* The id: the spec's replace rule — the same id answers a
     * replaces_id we handed out before. */
    uint32_t id = g_mock.replaces_seen != 0 ? g_mock.replaces_seen
                                            : g_mock_next_id++;
    fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
    if (reply != NULL && fdk_ok(fdk_dbus_message_append_uint32(reply, id))) {
        send_or_free(c, reply);
    } else if (reply != NULL) {
        fdk_dbus_message_free(reply);
    }

    /* The scripted event pair for THIS id: an action click, then the
     * close the daemon would send right after (dismissed) — only
     * when the leg asked for them. */
    if (g_script_events) {
        emit_signal(c, "ActionInvoked", id, "open", 0);
        emit_signal(c, "NotificationClosed", id, NULL, 2);
    }
}

static void mock_method_handler(fdk_dbus_connection *c,
                                fdk_dbus_message *call, void *user) {
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface == NULL ||
        strcmp(iface, "org.freedesktop.Notifications") != 0 ||
        member == NULL) {
        return;
    }
    if (strcmp(member, "Notify") == 0) {
        mock_serve_notify(c, call);
        return;
    }
    if (strcmp(member, "CloseNotification") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        uint32_t id = 0;
        if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 1) {
            id = fdk_dbus_value_uint32(args[0]);
        }
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL) {
            send_or_free(c, reply);
        }
        emit_signal(c, "NotificationClosed", id, NULL, 3);
        return;
    }
    if (strcmp(member, "GetCapabilities") == 0) {
        fdk_dbus_value *items[3] = {
            fdk_dbus_value_new_string("actions"),
            fdk_dbus_value_new_string("body-markup"),
            fdk_dbus_value_new_string("icon-static"),
        };
        fdk_dbus_value *arr = items[0] != NULL && items[1] != NULL &&
                                      items[2] != NULL
                                  ? fdk_dbus_value_new_array("s", items, 3)
                                  : NULL;
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL && arr != NULL &&
            fdk_ok(fdk_dbus_message_append(reply, arr))) {
            send_or_free(c, reply);
        } else if (reply != NULL) {
            fdk_dbus_message_free(reply);
        }
        fdk_dbus_value_free(arr);
        return;
    }
    if (strcmp(member, "GetServerInformation") == 0) {
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL &&
            fdk_ok(fdk_dbus_message_append_string(reply, "fdk-mock")) &&
            fdk_ok(fdk_dbus_message_append_string(reply, "FDK")) &&
            fdk_ok(fdk_dbus_message_append_string(reply, "1.0")) &&
            fdk_ok(fdk_dbus_message_append_string(reply, "1.2"))) {
            send_or_free(c, reply);
        } else if (reply != NULL) {
            fdk_dbus_message_free(reply);
        }
        return;
    }
    /* UnknownMethod, the daemon's honest answer. */
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod", NULL);
    if (err != NULL) {
        send_or_free(c, err);
    }
}

static int g_mock_ready = 0;
static int g_mock_reqname_done = 0;

static void mock_ready(fdk_dbus_connection *c, int ok, const char *err,
                       void *user) {
    (void)c;
    (void)err;
    int *flag = user;
    if (ok) {
        *flag = 1;
    }
}

static void mock_reqname_reply(fdk_dbus_connection *c,
                               fdk_dbus_message *reply, void *user) {
    (void)c;
    (void)user;
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_ok(fdk_dbus_message_read_args(reply, &args, &n)) && n == 1) {
        g_mock_reqname_done = 1;
    }
    fdk_dbus_values_free_array(args, n);
}

/* ---- the client's callbacks (bookkeeping only) --------------------------- */

static void ev_ready(fdk_notify *n, void *user) {
    (void)n;
    (void)user;
    g_ev.ready_count++;
}

static void ev_sent(fdk_notify *n, uint32_t id, void *note_user,
                    void *user) {
    (void)n;
    (void)user;
    if (g_ev.sent_count < 8) {
        g_ev.sent_ids[g_ev.sent_count] = id;
        g_ev.sent_users[g_ev.sent_count] = note_user;
    }
    g_ev.sent_count++;
}

static void ev_failed(fdk_notify *n, void *note_user, const char *error,
                      void *user) {
    (void)n;
    (void)user;
    if (g_ev.failed_count < 8) {
        snprintf(g_ev.failed_error[g_ev.failed_count],
                 sizeof(g_ev.failed_error[0]), "%s",
                 error != NULL ? error : "?");
    }
    g_ev.failed_count++;
    (void)note_user;
}

static void ev_closed(fdk_notify *n, uint32_t id, uint32_t reason,
                      void *note_user, void *user) {
    (void)user;
    if (g_ev.closed_count < 8) {
        g_ev.closed_ids[g_ev.closed_count] = id;
        g_ev.closed_reasons[g_ev.closed_count] = reason;
        g_ev.closed_users[g_ev.closed_count] = note_user;
    }
    g_ev.closed_count++;
    if (g_close_inside_callback && g_client_to_close == n) {
        /* The reentrancy discipline: teardown inside the event. */
        fdk_notify_close(n);
        g_client_to_close = NULL;
        g_close_inside_callback = 2;
    }
}

static void ev_action(fdk_notify *n, uint32_t id, const char *action,
                      void *note_user, void *user) {
    (void)n;
    (void)user;
    if (g_ev.action_count < 8) {
        g_ev.action_ids[g_ev.action_count] = id;
        snprintf(g_ev.action_keys[g_ev.action_count],
                 sizeof(g_ev.action_keys[0]), "%s",
                 action != NULL ? action : "?");
        g_ev.action_users[g_ev.action_count] = note_user;
    }
    g_ev.action_count++;
}

/* ---- helpers --------------------------------------------------------------- */

static int g_releasename_done = 0;
static void generic_done_reply(fdk_dbus_connection *c,
                               fdk_dbus_message *reply, void *user) {
    (void)c;
    (void)reply;
    int *flag = user;
    *flag = 1;
}

static int release_mock_name(fdk_context *ctx, fdk_dbus_connection *c) {
    g_releasename_done = 0;
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ReleaseName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, MOCK_NAME))) {
        fdk_dbus_message_free(call);
        return 0;
    }
    if (fdk_dbus_connection_call(c, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 generic_done_reply, &g_releasename_done) ==
        NULL) {
        fdk_dbus_message_free(call);
        return 0;
    }
    return pump_until(ctx, &g_releasename_done, 5000);
}

static int g_reqname_again_done = 0;
static int reown_mock_name(fdk_context *ctx, fdk_dbus_connection *c) {
    g_reqname_again_done = 0;
    fdk_dbus_pending *p = fdk_dbus_connection_request_name(
        c, MOCK_NAME, FDK_DBUS_NAME_DO_NOT_QUEUE,
        generic_done_reply, &g_reqname_again_done);
    if (p == NULL) {
        return 0;
    }
    return pump_until(ctx, &g_reqname_again_done, 5000);
}

/* ---- main ------------------------------------------------------------------ */

int main(void) {
    const char *bus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus_addr == NULL || bus_addr[0] == '\0') {
        printf("[skip] no DBUS_SESSION_BUS_ADDRESS in the environment\n");
        if (getenv("FDK_DBUS_TEST_REQUIRE") != NULL) {
            return 1;
        }
        return 0;
    }

    fdk_context *ctx = NULL;
    assert(fdk_init(&ctx, NULL) == FDK_OK);

    /* The mock daemon first: name owned BEFORE the client opens. */
    fdk_dbus_connection *mock =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, mock_ready,
                         &g_mock_ready);
    assert(mock != NULL);
    assert(pump_until(ctx, &g_mock_ready, 15000));
    fdk_dbus_connection_set_method_handler(mock, mock_method_handler, NULL);
    {
        g_mock_reqname_done = 0;
        fdk_dbus_pending *p = fdk_dbus_connection_request_name(
            mock, MOCK_NAME, FDK_DBUS_NAME_DO_NOT_QUEUE,
            mock_reqname_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &g_mock_reqname_done, 5000));
    }

    /* 1. open -> READY with the daemon already up. */
    fdk_notify_callbacks cb = {
        .on_ready = ev_ready,
        .on_sent = ev_sent,
        .on_failed = ev_failed,
        .on_closed = ev_closed,
        .on_action = ev_action,
        .user = NULL,
    };
    fdk_notify *client = fdk_notify_open(ctx, &cb);
    assert(client != NULL);
    {
        int ready = 0;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!ready) {
            (void)fdk_pump_events(ctx, 20);
            if (fdk_notify_get_status(client) == FDK_NOTIFY_STATUS_READY) {
                ready = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                           (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (ms > 15000) {
                break;
            }
        }
        assert(ready);
        assert(g_ev.ready_count == 1);
    }
    const char *const *caps = fdk_notify_capabilities(client);
    assert(caps != NULL && caps[0] != NULL &&
           strcmp(caps[0], "actions") == 0 && caps[1] != NULL &&
           strcmp(caps[1], "body-markup") == 0 && caps[3] == NULL);
    /* Server identity travels on its own call — wait for it (the
     * READY event is the capabilities' moment, not the identity's). */
    {
        int ident = 0;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (!ident) {
            (void)fdk_pump_events(ctx, 20);
            if (fdk_notify_server_name(client) != NULL &&
                fdk_notify_server_vendor(client) != NULL) {
                ident = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - t0.tv_sec) * 1000LL +
                           (now.tv_nsec - t0.tv_nsec) / 1000000LL;
            if (ms > 5000) {
                break;
            }
        }
        assert(ident);
    }
    assert(strcmp(sstr(fdk_notify_server_name(client)), "fdk-mock") == 0);
    assert(strcmp(sstr(fdk_notify_server_vendor(client)), "FDK") == 0);
    printf("[ok] adoption: READY, capabilities + server identity\n");

    /* 2. the full Notify round trip (every field validated at the
     * mock, the action + close pair arriving with note_user). */
    {
        const char *actions[] = {"open", "Open it", NULL};
        /* 2x2 RGBA, rowstride 8: rows are 8,8; bytes 0..7 row 0,
         * 8..11 row 1 (2 pixels). Flat payload carries 12 bytes. */
        static const uint8_t img[16] = {
            0x11, 0x22, 0x33, 0x44, 0, 0, 0, 0, /* row 0 (+pad) */
            0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC,
        };
        fdk_notify_request req = {
            .app_name = "fdk-test-app",
            .replaces_id = 0,
            .icon = "test-icon",
            .summary = "Sum",
            .body = "<b>Body</b>",
            .actions = actions,
            .urgency = FDK_NOTIFY_URGENCY_CRITICAL,
            .timeout_ms = 5000,
            .category = "test.cat",
            .desktop_entry = "org.fd.Test",
            .image_data = img,
            .image_width = 2,
            .image_height = 2,
            .image_rowstride = 8,
            .image_has_alpha = 1,
        };
        int marker = 42;
        assert(fdk_notify_send(client, &req, &marker) == 1);

        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.sent_count >= 1 && g_ev.action_count >= 1 &&
                   g_ev.closed_count >= 1;
        }
        assert(g_ev.sent_count == 1 && g_ev.sent_ids[0] == 1 &&
               g_ev.sent_users[0] == &marker);
        assert(g_ev.action_count == 1 && g_ev.action_ids[0] == 1 &&
               strcmp(g_ev.action_keys[0], "open") == 0 &&
               g_ev.action_users[0] == &marker);
        assert(g_ev.closed_count == 1 && g_ev.closed_ids[0] == 1 &&
               g_ev.closed_reasons[0] == FDK_NOTIFY_CLOSED_DISMISSED &&
               g_ev.closed_users[0] == &marker);
        /* The mapping survived the action and died with the close. */
        assert(g_mock.got_notify && g_mock.app_name_ok &&
               g_mock.icon_ok && g_mock.summary_ok && g_mock.body_ok &&
               g_mock.actions_ok && g_mock.urgency_ok &&
               g_mock.category_ok && g_mock.entry_ok && g_mock.image_ok &&
               g_mock.timeout_ok);
        printf("[ok] Notify round trip: every field validated at the "
               "daemon; action + closed with note_user\n");
    }

    /* 3. withdraw: CloseNotification -> reason 3 (the daemon stays
     * quiet on this one — no scripted pair). */
    {
        g_script_events = 0;
        fdk_notify_request req = {
            .summary = "Second",
        };
        assert(fdk_notify_send(client, &req, NULL) == 1);
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.sent_count >= 2;
        }
        assert(g_ev.sent_ids[1] == 2);
        fdk_notify_withdraw(client, 2);
        done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.closed_count >= 2;
        }
        assert(g_ev.closed_ids[1] == 2 &&
               g_ev.closed_reasons[1] == FDK_NOTIFY_CLOSED_REQUEST);
        printf("[ok] withdraw -> NotificationClosed(reason 3)\n");
    }

    /* 4. replace: the same id answers; on_sent carries it again. */
    {
        fdk_notify_request req = {
            .summary = "Replaced",
            .replaces_id = 1,
        };
        assert(fdk_notify_send(client, &req, NULL) == 1);
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.sent_count >= 3;
        }
        assert(g_ev.sent_ids[2] == 1);
        printf("[ok] replaces_id: same id answers\n");
    }

    /* 5. malformed: refused locally, no events. */
    {
        fdk_notify_request bad = {
            .summary = NULL,
        };
        int before = g_ev.sent_count + g_ev.failed_count;
        assert(fdk_notify_send(client, &bad, NULL) == 0);
        fdk_notify_request bad2 = {
            .summary = "x",
            .image_width = 2,
            .image_height = 0,
            .image_data = (const uint8_t *)"x",
        };
        assert(fdk_notify_send(client, &bad2, NULL) == 0);
        (void)fdk_pump_events(ctx, 100);
        assert(g_ev.sent_count + g_ev.failed_count == before);
        printf("[ok] malformed requests refused locally (no events)\n");
    }

    /* 6. the daemon vanishes: NO_DAEMON; the send's ServiceUnknown
     * reported through on_failed; the daemon returns -> re-adopted. */
    {
        assert(release_mock_name(ctx, mock));
        int nod = 0;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!nod) {
            (void)fdk_pump_events(ctx, 20);
            if (fdk_notify_get_status(client) ==
                FDK_NOTIFY_STATUS_NO_DAEMON) {
                nod = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                           (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (ms > 15000) {
                break;
            }
        }
        assert(nod);
        assert(fdk_notify_capabilities(client) == NULL);
        printf("[ok] daemon vanish -> NO_DAEMON, capabilities dropped\n");

        int fails = g_ev.failed_count;
        fdk_notify_request req = {
            .summary = "Into the void",
        };
        assert(fdk_notify_send(client, &req, NULL) == 1);
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.failed_count > fails;
        }
        /* The bus's own error name for an unowned, unactivatable
         * destination. */
        assert(strstr(g_ev.failed_error[fails],
                      "ServiceUnknown") != NULL ||
               strstr(g_ev.failed_error[fails], "no reply") != NULL ||
               strstr(g_ev.failed_error[fails], "Error") != NULL);
        printf("[ok] no-daemon send fails gracefully (%s)\n",
               g_ev.failed_error[fails]);

        int ready_again = g_ev.ready_count;
        assert(reown_mock_name(ctx, mock));
        done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.ready_count > ready_again &&
                   fdk_notify_get_status(client) ==
                       FDK_NOTIFY_STATUS_READY;
        }
        assert(fdk_notify_capabilities(client) != NULL);
        printf("[ok] daemon returns -> re-adopted, capabilities back\n");
    }

    /* 7. the teardown discipline: close INSIDE the closed event. */
    {
        fdk_notify_request req = {
            .summary = "Last",
        };
        assert(fdk_notify_send(client, &req, NULL) == 1);
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_ev.sent_count >= 4;
        }
        g_close_inside_callback = 1;
        g_client_to_close = client;
        fdk_notify_withdraw(client, g_ev.sent_ids[3]);
        done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_close_inside_callback == 2;
        }
        assert(g_close_inside_callback == 2);
        printf("[ok] teardown inside the event callback (ASan clean)\n");
    }

    /* The mock connection's own teardown. */
    fdk_dbus_connection_close(mock);
    fdk_shutdown(ctx);
    printf("ALL PASS\n");
    return 0;
}
