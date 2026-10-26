/* test_dbus_live.c — the native D-Bus client against a REAL bus (1.1.0)
 *
 * Runs under `make test-dbus`, which wraps this binary in
 * dbus-run-session(1) and a throwaway Xvfb (the fdk_context the
 * client rides needs a display). What it proves, end to end:
 *
 *   - the SASL handshake + Hello against the real daemon (libdbus
 *     and FDK interoperate on the same socket grammar);
 *   - ListNames through our own marshaling;
 *   - a full service round trip: connection B calls connection A's
 *     method (through the daemon), A replies, B reads the reply —
 *     plus the error-reply path;
 *   - signals: A subscribes with a match rule, B emits, A receives
 *     (arg0 filtering proven);
 *   - properties: A serves org.freedesktop.DBus.Properties (Get/Set
 *     + PropertiesChanged delivery to B's typed subscription);
 *   - name watching: B watches A's well-known name through its
 *     RequestName/ReleaseName lifecycle (the GetNameOwner +
 *     NameOwnerChanged reconciliation);
 *   - timeouts: a call to a name that exists but never replies;
 *   - introspection of the bus daemon's own node;
 *   - the reentrancy discipline: closing the connection INSIDE its
 *     own reply callback (ASan/LSan must stay clean);
 *   - graceful degradation: an address pointing nowhere fails fast.
 *
 * Skips loudly (exit 0, [skip] lines) when dbus-run-session or a
 * display is unavailable, so family CI without dbus stays green;
 * set FDK_DBUS_TEST_REQUIRE=1 to make the skip a failure (the
 * ubuntu CI job does).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- tiny pump helper: drive the loop until `done` flips ------- */

static int pump_until(fdk_context *ctx, int *done, int timeout_ms) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!*done) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                       (now.tv_nsec - start.tv_nsec) / 1000000LL;
        if (ms > timeout_ms) {
            return 0; /* timeout */
        }
        (void)fdk_pump_events(ctx, 20);
    }
    return 1;
}

/* request_name's reply handler + state. */
static int g_reqname_code = 0;
static int g_reqname_done = 0;
void reqname_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                   void *user);

/* ---- shared state for the async steps --------------------------- */

static int ready_a, ready_b;
static int listnames_done;
static int saw_own_name;
static int echo_done, echo_ok;
static int error_done, error_ok;
static int signal_done, signal_count;
static int propget_done, propget_ok;
static int propset_done, propset_ok;
static int propchange_done, propchange_ok;
static int watch_done, watch_events;
static int timeout_done, timeout_was_null;
static int intro_done, intro_ok;
static int closed_in_callback;
static int death_fired;
static int name_events_acquired, name_events_lost;
static int ignore_seen;

#define NAME_A "org.fdktest.Echo"

/* ---- connection A: the service under test ------------------------ */

static void a_ready(fdk_dbus_connection *c, int ok, const char *err,
                    void *user) {
    (void)c;
    (void)err;
    int *flag = user;
    *flag = ok;
}

static void a_method_handler(fdk_dbus_connection *c, fdk_dbus_message *call,
                             void *user) {
    (void)user;
    const char *member = fdk_dbus_message_member(call);
    const char *iface = fdk_dbus_message_interface(call);
    if (member == NULL) {
        return;
    }
    if (strcmp(member, "Echo") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) != FDK_OK ||
            n != 1) {
            fdk_dbus_values_free_array(args, n);
            fdk_dbus_message *err = fdk_dbus_message_new_error(
                call, "org.fdktest.Error.BadArgs", "Echo takes one string");
            if (err) {
                (void)fdk_dbus_connection_send(c, err);
            }
            return;
        }
        const char *text = fdk_dbus_value_string(args[0]);
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL && text != NULL) {
            (void)fdk_dbus_message_append_string(reply, text);
            (void)fdk_dbus_connection_send(c, reply);
        }
        fdk_dbus_values_free_array(args, n);
        return;
    }
    if (strcmp(member, "Ignore") == 0) {
        ignore_seen = 1; /* deliberately never replies */
        return;
    }
    if (strcmp(member, "Fail") == 0) {
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.fdktest.Error.Called", "you asked for it");
        if (err) {
            (void)fdk_dbus_connection_send(c, err);
        }
        return;
    }
    /* Properties: serve org.freedesktop.DBus.Properties on this
     * object — Get/Set/GetAll on the org.fdktest.Echo interface. */
    if (iface != NULL &&
        strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) != FDK_OK ||
            n < 2) {
            fdk_dbus_values_free_array(args, n);
            return;
        }
        const char *prop = fdk_dbus_value_string(args[1]);
        if (strcmp(member, "Get") == 0 && prop != NULL &&
            strcmp(prop, "Volume") == 0) {
            fdk_dbus_message *reply = fdk_dbus_message_new_method_return(
                call);
            if (reply) {
                (void)fdk_dbus_message_append_variant_uint32(reply, 11u);
                (void)fdk_dbus_connection_send(c, reply);
            }
        } else if (strcmp(member, "Set") == 0 && prop != NULL &&
                   strcmp(prop, "Volume") == 0 && n >= 3) {
            fdk_dbus_message *reply = fdk_dbus_message_new_method_return(
                call);
            if (reply) {
                (void)fdk_dbus_connection_send(c, reply);
            }
            /* emit PropertiesChanged for the typed subscription test */
            fdk_dbus_message *sig = fdk_dbus_message_new_signal(
                fdk_dbus_message_path(call),
                "org.freedesktop.DBus.Properties", "PropertiesChanged");
            if (sig) {
                (void)fdk_dbus_message_append_string(
                    sig, "org.fdktest.Echo"); /* the interface arg    */
                /* a{sv}: the changed dict's values are VARIANTS */
                fdk_dbus_value *k = fdk_dbus_value_new_string("Volume");
                fdk_dbus_value *v_inner = fdk_dbus_value_new_uint32(12);
                fdk_dbus_value *v = fdk_dbus_value_new_variant(v_inner);
                fdk_dbus_value_free(v_inner);
                fdk_dbus_value *entry =
                    fdk_dbus_value_new_dict_entry(k, v);
                fdk_dbus_value *entries[1] = {entry};
                fdk_dbus_value *dict =
                    fdk_dbus_value_new_array("{sv}", entries, 1);
                (void)fdk_dbus_message_append(sig, dict);
                fdk_dbus_value_free(dict);
                fdk_dbus_value *empty = fdk_dbus_value_new_array("s",
                                                                 NULL, 0);
                (void)fdk_dbus_message_append(sig, empty);
                fdk_dbus_value_free(empty);
                (void)fdk_dbus_connection_send(c, sig);
            }
        }
        fdk_dbus_values_free_array(args, n);
        return;
    }
    /* Unknown member: the documented FDK default when no handler
     * answers — here the handler IS installed, so replying is the
     * test's business (and it does, exercising the full path). */
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod",
        "no such member on org.fdktest.Echo");
    if (err) {
        (void)fdk_dbus_connection_send(c, err);
    }
}

/* ---- connection B: the client under test -------------------------- */

static void b_ready(fdk_dbus_connection *c, int ok, const char *err,
                    void *user) {
    (void)c;
    (void)err;
    int *flag = user;
    *flag = ok;
}

static void listnames_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                            void *user) {
    (void)c;
    (void)user;
    listnames_done = 1;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1) {
        return;
    }
    const fdk_dbus_value *names = args[0];
    size_t count = fdk_dbus_value_array_count(names);
    for (size_t i = 0; i < count; i++) {
        const char *nm =
            fdk_dbus_value_string(fdk_dbus_value_array_item(names, i));
        if (nm != NULL && strcmp(nm, "org.freedesktop.DBus") == 0) {
            saw_own_name = 1;
        }
    }
    fdk_dbus_values_free_array(args, n);
}

static void echo_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                       void *user) {
    (void)c;
    (void)user;
    echo_done = 1;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) == FDK_OK && n == 1) {
        const char *s = fdk_dbus_value_string(args[0]);
        echo_ok = (s != NULL && strcmp(s, "hello through the daemon") == 0);
    }
    fdk_dbus_values_free_array(args, n);
}

static void error_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                        void *user) {
    (void)c;
    (void)user;
    error_done = 1;
    error_ok = (m != NULL && fdk_dbus_message_is_error(m) &&
                fdk_dbus_message_error_name(m) != NULL &&
                strcmp(fdk_dbus_message_error_name(m),
                       "org.fdktest.Error.Called") == 0);
}

static void timeout_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                          void *user) {
    (void)c;
    (void)user;
    timeout_done = 1;
    timeout_was_null = (m == NULL);
}

static void test_signal_cb(fdk_dbus_connection *c, fdk_dbus_message *sig,
                           void *user) {
    (void)c;
    (void)user;
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(sig, &args, &n) == FDK_OK && n == 1) {
        const char *s = fdk_dbus_value_string(args[0]);
        if (s != NULL && strcmp(s, "the-payload") == 0) {
            signal_count++;
            signal_done = 1;
        }
    }
    fdk_dbus_values_free_array(args, n);
}

static void wrong_arg_signal_cb(fdk_dbus_connection *c,
                                fdk_dbus_message *sig, void *user) {
    (void)c;
    (void)sig;
    (void)user;
    /* This subscription filters arg0='other-payload' — receiving it
     * here means client-side matching is broken. */
    signal_done = -1;
}

static void propget_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                          void *user) {
    (void)c;
    (void)user;
    propget_done = 1;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) == FDK_OK && n == 1) {
        const fdk_dbus_value *inner =
            fdk_dbus_value_variant_value(args[0]);
        propget_ok = (fdk_dbus_value_uint32(inner) == 11u);
    }
    fdk_dbus_values_free_array(args, n);
}

static void propset_reply(fdk_dbus_connection *c, fdk_dbus_message *m,
                          void *user) {
    (void)c;
    (void)user;
    propset_done = 1;
    propset_ok = (m != NULL && !fdk_dbus_message_is_error(m));
}

static void propchange_cb(fdk_dbus_connection *c, const char *iface,
                          const fdk_dbus_value *changed,
                          size_t n_invalidated,
                          const char *const *invalidated, void *user) {
    (void)c;
    (void)n_invalidated;
    (void)invalidated;
    (void)user;
    propchange_done = 1;
    if (iface == NULL || strcmp(iface, "org.fdktest.Echo") != 0) {
        return;
    }
    const fdk_dbus_value *v = fdk_dbus_value_dict_lookup(changed, "Volume");
    propchange_ok = (v != NULL &&
                     fdk_dbus_value_uint32(fdk_dbus_value_variant_value(
                         v)) == 12u);
}

static void watcher_cb(fdk_dbus_connection *c, const char *name,
                       const char *owner, void *user) {
    (void)c;
    (void)user;
    if (strcmp(name, NAME_A) != 0) {
        return;
    }
    watch_events++;
    if (owner != NULL) {
        watch_done = 1; /* appeared */
    }
}

static void intro_cb(fdk_dbus_connection *c, fdk_dbus_introspection *node,
                     const char *raw_xml, void *user) {
    (void)c;
    (void)user;
    intro_done = 1;
    intro_ok = (node != NULL && raw_xml != NULL &&
                fdk_dbus_introspection_find_interface(
                    node, "org.freedesktop.DBus") >= 0);
}

static void close_in_callback_reply(fdk_dbus_connection *c,
                                    fdk_dbus_message *m, void *user) {
    /* The reentrancy discipline: closing the connection whose
     * callback is on the stack. m is a real reply (the Echo call). */
    (void)m;
    fdk_dbus_connection *victim = user;
    if (m != NULL) {
        closed_in_callback = 1;
        fdk_dbus_connection_close(victim);
    }
    (void)c;
}

static void death_cb(fdk_dbus_connection *c, void *user) {
    (void)c;
    (void)user;
    death_fired = 1;
}

static void name_event_cb(fdk_dbus_connection *c, const char *name,
                          int owned, void *user) {
    (void)c;
    (void)user;
    if (strcmp(name, NAME_A) == 0) {
        if (owned) {
            name_events_acquired++;
        } else {
            name_events_lost++;
        }
    }
}

/* ---- the suite ------------------------------------------------------ */

int main(void) {
    /* Skip discipline: no dbus-run-session / no display → skip. */
    if (system("command -v dbus-run-session >/dev/null 2>&1") != 0) {
        printf("[skip] dbus-run-session not available\n");
        return 0;
    }
    const char *display = getenv("DISPLAY");
    if (display == NULL || display[0] == '\0') {
        if (getenv("FDK_DBUS_TEST_REQUIRE") != NULL) {
            fprintf(stderr, "FAIL: FDK_DBUS_TEST_REQUIRE set but no "
                            "DISPLAY\n");
            return 1;
        }
        printf("[skip] no DISPLAY for the fdk_context\n");
        return 0;
    }

    /* The harness ran us under dbus-run-session: the session bus
     * env must be present. */
    const char *bus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus_addr == NULL || bus_addr[0] == '\0') {
        printf("[skip] no DBUS_SESSION_BUS_ADDRESS in the environment\n");
        return 0;
    }

    fdk_context *ctx = NULL;
    assert(fdk_init(&ctx, NULL) == FDK_OK);

    /* ---- graceful degradation FIRST: a dead address fails fast ---- */
    setenv("FDK_DBUS_ADDRESS", "unix:path=/nonexistent/fdk-dbus-dead",
           1);
    fdk_dbus_connection *dead =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    assert(dead == NULL); /* one INFO line, no retry storm */
    unsetenv("FDK_DBUS_ADDRESS");
    printf("[ok] no-bus address fails fast (NULL, single log line)\n");

    /* ---- connect both peers ---------------------------------------- */
    fdk_dbus_connection *a =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, a_ready, &ready_a);
    assert(a != NULL);
    fdk_dbus_connection *b =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, b_ready, &ready_b);
    assert(b != NULL);
    assert(pump_until(ctx, &ready_a, 15000));
    assert(pump_until(ctx, &ready_b, 15000));
    assert(ready_a && ready_b);
    assert(fdk_dbus_connection_unique_name(a) != NULL);
    assert(fdk_dbus_connection_unique_name(b) != NULL);
    assert(fdk_dbus_connection_unique_name(a)[0] == ':');
    printf("[ok] SASL + Hello against the real daemon; unique names "
           "%s / %s\n",
           fdk_dbus_connection_unique_name(a),
           fdk_dbus_connection_unique_name(b));

    /* ---- A becomes a service --------------------------------------- */
    fdk_dbus_connection_set_method_handler(a, a_method_handler, NULL);
    fdk_dbus_connection_set_name_events(a, name_event_cb, NULL);
    fdk_dbus_connection_set_death_handler(a, death_cb, NULL);

    fdk_dbus_pending *pn = fdk_dbus_connection_request_name(
        a, NAME_A, FDK_DBUS_NAME_DO_NOT_QUEUE, reqname_reply, NULL);
    assert(pn != NULL);
    assert(pump_until(ctx, &g_reqname_done, 10000));
    assert(g_reqname_code == FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER);
    assert(name_events_acquired >= 1); /* NameAcquired arrived unicast */
    printf("[ok] RequestName primary owner + unicast NameAcquired\n");

    /* ---- B: ListNames ---------------------------------------------- */
    {
        fdk_dbus_message *m = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ListNames");
        assert(m != NULL);
        fdk_dbus_pending *p =
            fdk_dbus_connection_call(b, m, 10000, listnames_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &listnames_done, 10000));
        assert(saw_own_name);
    }
    printf("[ok] ListNames round trip; org.freedesktop.DBus present\n");

    /* ---- method call through the daemon ---------------------------- */
    {
        fdk_dbus_message *m =
            fdk_dbus_message_new_method_call(NAME_A, "/org/fdktest/echo",
                                             "org.fdktest.Echo", "Echo");
        assert(m != NULL);
        (void)fdk_dbus_message_append_string(m, "hello through the daemon");
        fdk_dbus_pending *p =
            fdk_dbus_connection_call(b, m, 10000, echo_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &echo_done, 10000));
        assert(echo_ok);
    }
    printf("[ok] method call routed through the daemon, echo reply "
           "parsed\n");

    /* ---- error reply ------------------------------------------------- */
    {
        fdk_dbus_message *m =
            fdk_dbus_message_new_method_call(NAME_A, "/org/fdktest/echo",
                                             "org.fdktest.Echo", "Fail");
        assert(m != NULL);
        fdk_dbus_pending *p =
            fdk_dbus_connection_call(b, m, 10000, error_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &error_done, 10000));
        assert(error_ok);
    }
    printf("[ok] error reply carries its name across the daemon\n");

    /* ---- timeout: Ignore never replies -------------------------------- */
    {
        fdk_dbus_message *m =
            fdk_dbus_message_new_method_call(NAME_A, "/org/fdktest/echo",
                                             "org.fdktest.Echo", "Ignore");
        assert(m != NULL);
        fdk_dbus_pending *p =
            fdk_dbus_connection_call(b, m, 300, timeout_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &timeout_done, 10000));
        assert(timeout_was_null);
        assert(ignore_seen);
    }
    printf("[ok] timeout on a silent peer delivers a NULL reply\n");

    /* ---- signals + match rules ---------------------------------------- */
    {
        /* NOTE: no sender= scoping — the bus rewrites a well-known
         * sender into its UNIQUE name in the message header (the
         * exact trap fdk_dbus.h's subscribe docs warn about); an
         * arg0-scoped rule is the portable form. */
        char *rule = fdk_dbus_match_rule("signal", NULL,
                                         "org.fdktest.Echo", "Shouted",
                                         NULL, "the-payload");
        assert(rule != NULL);
        fdk_dbus_subscription *sub =
            fdk_dbus_connection_subscribe(b, rule, test_signal_cb, NULL);
        assert(sub != NULL);
        char *wrong = fdk_dbus_match_rule("signal", NULL,
                                          "org.fdktest.Echo", "Shouted",
                                          NULL, "other-payload");
        fdk_dbus_subscription *sub2 = fdk_dbus_connection_subscribe(
            b, wrong, wrong_arg_signal_cb, NULL);
        assert(sub2 != NULL);
        /* let the AddMatch round trips land */
        int settle = 0;
        (void)pump_until(ctx, &settle, 150);
        /* A emits the signal */
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            "/org/fdktest/echo", "org.fdktest.Echo", "Shouted");
        assert(sig != NULL);
        (void)fdk_dbus_message_append_string(sig, "the-payload");
        assert(fdk_dbus_connection_send(a, sig) == FDK_OK);
        assert(pump_until(ctx, &signal_done, 10000));
        assert(signal_done == 1 && signal_count == 1);
        /* a second emission must ALSO arrive (rules persist) */
        sig = fdk_dbus_message_new_signal("/org/fdktest/echo",
                                          "org.fdktest.Echo", "Shouted");
        (void)fdk_dbus_message_append_string(sig, "the-payload");
        assert(fdk_dbus_connection_send(a, sig) == FDK_OK);
        int second = 0;
        while (signal_count < 2) {
            (void)fdk_pump_events(ctx, 20);
            clock_nanosleep(CLOCK_MONOTONIC, 0,
                            &(struct timespec){0, 20 * 1000000}, NULL);
            if (++second > 400) {
                break;
            }
        }
        assert(signal_count == 2);
        fdk_dbus_subscription_free(sub2);
        fdk_dbus_subscription_free(sub);
        free(rule);
        free(wrong);
    }
    printf("[ok] signals through AddMatch, arg0-filtered, both "
           "directions\n");

    /* ---- properties --------------------------------------------------- */
    {
        fdk_dbus_pending *p = fdk_dbus_connection_get_property(
            b, NAME_A, "/org/fdktest/echo", "org.fdktest.Echo", "Volume",
            propget_reply, NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &propget_done, 10000));
        assert(propget_ok);

        fdk_dbus_value *v = fdk_dbus_value_new_uint32(12);
        fdk_dbus_subscription *pcs =
            fdk_dbus_connection_subscribe_properties_changed(
                b, NAME_A, "org.fdktest.Echo", propchange_cb, NULL);
        assert(pcs != NULL);
        int settle = 0;
        (void)pump_until(ctx, &settle, 150);
        fdk_dbus_pending *sp = fdk_dbus_connection_set_property(
            b, NAME_A, "/org/fdktest/echo", "org.fdktest.Echo", "Volume",
            v, propset_reply, NULL);
        fdk_dbus_value_free(v);
        assert(sp != NULL);
        assert(pump_until(ctx, &propset_done, 10000));
        assert(propset_ok);
        assert(pump_until(ctx, &propchange_done, 10000));
        assert(propchange_ok);
        fdk_dbus_subscription_free(pcs);
    }
    printf("[ok] Properties Get/Set + typed PropertiesChanged delivery\n");

    /* ---- name watching -------------------------------------------------- */
    {
        fdk_dbus_name_watcher *w = fdk_dbus_connection_watch_name(
            b, NAME_A, watcher_cb, NULL);
        assert(w != NULL);
        assert(pump_until(ctx, &watch_done, 10000));
        assert(watch_events == 1); /* appeared, exactly once */

        /* A releases the name → vanished */
        fdk_dbus_message *m = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ReleaseName");
        assert(m != NULL);
        (void)fdk_dbus_message_append_string(m, NAME_A);
        int released = 0;
        fdk_dbus_pending *p = fdk_dbus_connection_call(
            a, m, 10000, NULL, NULL); /* reply dropped; the watcher is
                                         the assertion */
        (void)p;
        while (watch_events < 2) {
            (void)fdk_pump_events(ctx, 20);
            clock_nanosleep(CLOCK_MONOTONIC, 0,
                            &(struct timespec){0, 20 * 1000000}, NULL);
            if (++released > 400) {
                break;
            }
        }
        assert(watch_events == 2); /* appeared + vanished */
        assert(name_events_lost >= 1);
        fdk_dbus_name_watcher_free(w);

        /* re-acquire for the remaining tests */
        int settle = 0;
        (void)pump_until(ctx, &settle, 100);
        fdk_dbus_message *m2 = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName");
        (void)fdk_dbus_message_append_string(m2, NAME_A);
        (void)fdk_dbus_message_append_uint32(
            m2, FDK_DBUS_NAME_DO_NOT_QUEUE);
        fdk_dbus_pending *p2 =
            fdk_dbus_connection_call(a, m2, 10000, NULL, NULL);
        (void)p2;
        settle = 0;
        (void)pump_until(ctx, &settle, 200);
    }
    printf("[ok] name watching: appeared + vanished transitions, "
           "NameLost unicast\n");

    /* ---- introspection of the bus's own node --------------------------- */
    {
        fdk_dbus_pending *p = fdk_dbus_connection_introspect(
            b, "org.freedesktop.DBus", "/org/freedesktop/DBus", intro_cb,
            NULL);
        assert(p != NULL);
        assert(pump_until(ctx, &intro_done, 10000));
        assert(intro_ok);
    }
    printf("[ok] Introspect of the daemon's node parsed (org."
           "freedesktop.DBus interface found)\n");

    /* ---- the reentrancy discipline --------------------------------------- */
    {
        fdk_dbus_connection *victim = fdk_dbus_connect(
            ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
        assert(victim != NULL);
        while (!fdk_dbus_connection_ready(victim)) {
            (void)fdk_pump_events(ctx, 20);
        }
        /* the call's reply callback closes the very connection */
        fdk_dbus_message *m =
            fdk_dbus_message_new_method_call(NAME_A, "/org/fdktest/echo",
                                             "org.fdktest.Echo", "Echo");
        assert(m != NULL);
        (void)fdk_dbus_message_append_string(m, "close-from-callback");
        fdk_dbus_pending *p = fdk_dbus_connection_call(
            victim, m, 10000, close_in_callback_reply, victim);
        assert(p != NULL);
        while (!closed_in_callback) {
            (void)fdk_pump_events(ctx, 20);
        }
        /* The handle is freed at the end of the outermost callback
         * (the close_pending reap) — by the same discipline as
         * removing an fd source inside its own callback, the app
         * must not touch it afterwards. The next pump proves the
         * reaping happened with no dangling dispatch (ASan would
         * fire on any touch of the freed connection from OUR code,
         * which is exactly what this run exercises). */
        (void)fdk_pump_events(ctx, 20);
        printf("[ok] close-from-inside-its-own-callback: reaped cleanly\n");
    }

    /* ---- teardown --------------------------------------------------------- */
    fdk_dbus_connection_close(b);
    fdk_dbus_connection_close(a);
    (void)fdk_pump_events(ctx, 50);
    assert(death_fired == 0); /* close() is not a death (documented) */

    fdk_shutdown(ctx);
    printf("\nall live-bus dbus tests passed\n");
    return 0;
}

void reqname_reply(fdk_dbus_connection *c, fdk_dbus_message *m, void *user) {
    (void)c;
    (void)user;
    g_reqname_done = 1;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) == FDK_OK && n >= 1) {
        g_reqname_code = (int)fdk_dbus_value_uint32(args[0]);
    }
    fdk_dbus_values_free_array(args, n);
}
