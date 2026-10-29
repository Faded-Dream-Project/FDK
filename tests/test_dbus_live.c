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

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_theme.h"
#include "fdk/fdk_idle.h"
#include "fdk/fdk_dialog.h"

/* The scheme-engine seam (the mock-portal scenario drives the same
 * recheck a real portal's SettingChanged fires). */
#include "theme/theme_internal.h"
/* ctx->window_count for the portal-fallback leg's own-browser proof. */
#include "core/context_internal.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
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

/* The mock portal's method handler: ReadOne on
 * org.freedesktop.appearance/color-scheme answers the current test
 * value (g_portal_answer); anything else is UnknownMethod. */
static uint32_t g_portal_answer = 1; /* color-scheme: 1 = dark */
static void portal_method_handler(fdk_dbus_connection *c,
                                  fdk_dbus_message *call, void *user) {
    (void)user;
    const char *member = fdk_dbus_message_member(call);
    const char *iface = fdk_dbus_message_interface(call);
    if (member == NULL || iface == NULL ||
        strcmp(iface, "org.freedesktop.portal.Settings") != 0) {
        return;
    }
    if (strcmp(member, "ReadOne") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) != FDK_OK ||
            n < 2 || args == NULL) {
            fdk_dbus_values_free_array(args, n);
            return;
        }
        const char *ns = fdk_dbus_value_string(args[0]);
        const char *key = fdk_dbus_value_string(args[1]);
        if (ns != NULL && key != NULL &&
            strcmp(ns, "org.freedesktop.appearance") == 0 &&
            strcmp(key, "color-scheme") == 0) {
            fdk_dbus_message *reply =
                fdk_dbus_message_new_method_return(call);
            if (reply != NULL) {
                (void)fdk_dbus_message_append_variant_uint32(
                    reply, g_portal_answer);
                (void)fdk_dbus_connection_send(c, reply);
            }
        }
        fdk_dbus_values_free_array(args, n);
        return;
    }
    /* ReadAll and friends: UnknownMethod (the engine only uses
     * ReadOne + the SettingChanged subscription). */
}

/* ---- the mock org.freedesktop.ScreenSaver (S-3 idle inhibition) ----
 *
 * A becomes the desktop's screensaver service: Inhibit(app, reason)
 * answers a distinct cookie (or a refusal for the sentinel app
 * name), UnInhibit(cookie) records the release. The idle module's
 * full D-Bus half is pinned against it: the cookie round trip, the
 * DEFERRED release, and the error path. */
static int g_ss_inhibits, g_ss_uninhibits;
static uint32_t g_ss_last_cookie, g_ss_last_uninhibited;
static char g_ss_last_app[64], g_ss_last_reason[64];

static void screensaver_handler(fdk_dbus_connection *c,
                                fdk_dbus_message *call, void *user) {
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface == NULL ||
        strcmp(iface, "org.freedesktop.ScreenSaver") != 0) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (strcmp(member, "Inhibit") == 0) {
        if (fdk_dbus_message_read_args(call, &args, &n) != FDK_OK ||
            n != 2) {
            fdk_dbus_values_free_array(args, n);
            fdk_dbus_message *err = fdk_dbus_message_new_error(
                call, "org.fdktest.Error.BadArgs", "Inhibit(s,s)");
            if (err != NULL) {
                (void)fdk_dbus_connection_send(c, err);
            }
            return;
        }
        const char *app = fdk_dbus_value_string(args[0]);
        const char *reason = fdk_dbus_value_string(args[1]);
        snprintf(g_ss_last_app, sizeof g_ss_last_app, "%s",
                 app ? app : "");
        snprintf(g_ss_last_reason, sizeof g_ss_last_reason, "%s",
                 reason ? reason : "");
        if (app != NULL && strcmp(app, "please-fail") == 0) {
            fdk_dbus_values_free_array(args, n);
            fdk_dbus_message *err = fdk_dbus_message_new_error(
                call, "org.freedesktop.ScreenSaver.Error.Refused",
                "the sentinel app is refused");
            if (err != NULL) {
                (void)fdk_dbus_connection_send(c, err);
            }
            return;
        }
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL) {
            g_ss_inhibits++;
            g_ss_last_cookie = 1000u + (uint32_t)g_ss_inhibits;
            (void)fdk_dbus_message_append_uint32(reply, g_ss_last_cookie);
            (void)fdk_dbus_connection_send(c, reply);
        }
        fdk_dbus_values_free_array(args, n);
        return;
    }
    if (strcmp(member, "UnInhibit") == 0) {
        if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32) {
            g_ss_last_uninhibited = fdk_dbus_value_uint32(args[0]);
            g_ss_uninhibits++;
        }
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *reply = fdk_dbus_message_new_method_return(call);
        if (reply != NULL) {
            (void)fdk_dbus_connection_send(c, reply);
        }
        return;
    }
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

/* ---- The mock FileChooser portal (1.1.0, C-1 / B-09) --------------------
 *
 * A becomes org.freedesktop.portal.Desktop serving
 * org.freedesktop.portal.FileChooser: OpenFile/SaveFile parse the
 * options dict (handle_token, directory, multiple, current_name,
 * current_folder, filters), answer a REQUEST PATH built exactly the
 * spec's way (/org/freedesktop/portal/desktop/request/<sender-with-
 * colons-and-dots-rewritten>/<token>), then emit the Response
 * signal on it with the chosen outcome (accepted-with-uris or
 * dismissed). FDK's portal client is pinned end to end against it:
 * the call shape, the subscription ordering, the response decode,
 * and the fallback path (a refusal hands the pick to the toolkit's
 * own browser). */
static int g_fc_calls;
static int g_fc_refuse; /* answer an error reply instead of a handle */
static struct {
    int done;
    fdk_file_dialog_outcome outcome;
    char paths[2][256];
    size_t count;
} g_fc_result;

static void fc_done_cb(const fdk_file_dialog_result *r, void *user) {
    (void)user;
    g_fc_result.done = 1;
    g_fc_result.outcome = r->outcome;
    g_fc_result.count = r->count;
    for (size_t i = 0; i < r->count && i < 2; i++) {
        snprintf(g_fc_result.paths[i], sizeof g_fc_result.paths[i],
                 "%s", r->paths[i]);
    }
}

static int g_fc_last_directory;
static int g_fc_last_multiple;
static char g_fc_last_title[128];
static char g_fc_last_current_name[128];
static char g_fc_last_current_folder[256];
static int g_fc_last_filters; /* count of filter structs seen */
static char g_fc_last_token[64];

static void filechooser_handler(fdk_dbus_connection *c,
                                fdk_dbus_message *call, void *user) {
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface == NULL ||
        strcmp(iface, "org.freedesktop.portal.FileChooser") != 0 ||
        (strcmp(member, "OpenFile") != 0 &&
         strcmp(member, "SaveFile") != 0)) {
        return;
    }
    g_fc_calls++;

    /* Args: (parent_window, title, options a(sv)). Read them and
     * the options dict. */
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    char token[64] = "default";
    int directory = 0, multiple = 0;
    const char *cur_name = NULL;
    const uint8_t *cur_folder = NULL;
    size_t cur_folder_len = 0;
    int filters_seen = 0;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n >= 3) {
        const char *title = fdk_dbus_value_string(args[1]);
        snprintf(g_fc_last_title, sizeof g_fc_last_title, "%s",
                 title != NULL ? title : "");
        if (fdk_dbus_value_type(args[2]) == FDK_DBUS_ARRAY) {
            size_t entries = fdk_dbus_value_array_count(args[2]);
            for (size_t e = 0; e < entries; e++) {
                const fdk_dbus_value *entry =
                    fdk_dbus_value_array_item(args[2], e);
                if (entry == NULL ||
                    fdk_dbus_value_struct_count(entry) < 2) {
                    continue;
                }
                const char *key = fdk_dbus_value_string(
                    fdk_dbus_value_struct_item(entry, 0));
                const fdk_dbus_value *val =
                    fdk_dbus_value_struct_item(entry, 1);
                if (key == NULL || val == NULL) {
                    continue;
                }
                if (strcmp(key, "handle_token") == 0) {
                    const char *t = fdk_dbus_value_string(
                        fdk_dbus_value_variant_value(val));
                    snprintf(token, sizeof token, "%s",
                             t != NULL ? t : "default");
                } else if (strcmp(key, "directory") == 0) {
                    directory = fdk_dbus_value_boolean(
                        fdk_dbus_value_variant_value(val));
                } else if (strcmp(key, "multiple") == 0) {
                    multiple = fdk_dbus_value_boolean(
                        fdk_dbus_value_variant_value(val));
                } else if (strcmp(key, "current_name") == 0) {
                    cur_name = fdk_dbus_value_string(
                        fdk_dbus_value_variant_value(val));
                } else if (strcmp(key, "current_folder") == 0) {
                    cur_folder = fdk_dbus_value_array_flat_bytes(
                        fdk_dbus_value_variant_value(val),
                        &cur_folder_len);
                } else if (strcmp(key, "filters") == 0) {
                    const fdk_dbus_value *arr =
                        fdk_dbus_value_variant_value(val);
                    if (arr != NULL &&
                        fdk_dbus_value_type(arr) == FDK_DBUS_ARRAY) {
                        filters_seen =
                            (int)fdk_dbus_value_array_count(arr);
                    }
                }
            }
        }
    }
    /* Copy the borrowed strings OUT first — every one of them points
     * into the args tree that the free below releases (the UAF the
     * first cut of this handler hit at exactly the memcpy). */
    g_fc_last_directory = directory;
    g_fc_last_multiple = multiple;
    g_fc_last_filters = filters_seen;
    snprintf(g_fc_last_current_name, sizeof g_fc_last_current_name,
             "%s", cur_name != NULL ? cur_name : "");
    if (cur_folder != NULL && cur_folder_len > 0) {
        size_t take = cur_folder_len < 255 ? cur_folder_len : 255;
        memcpy(g_fc_last_current_folder, cur_folder, take);
        g_fc_last_current_folder[take] = '\0';
    } else {
        g_fc_last_current_folder[0] = '\0';
    }
    snprintf(g_fc_last_token, sizeof g_fc_last_token, "%s", token);
    fdk_dbus_values_free_array(args, n);

    /* The request path (the spec's sender rule: ':1.5' -> '_1_5'). */
    const char *unique = fdk_dbus_message_sender(call);
    char sender_seg[96] = "_";
    if (unique != NULL) {
        size_t o = 0;
        for (const char *q = unique;
             *q != '\0' && o + 1 < sizeof sender_seg; q++) {
            sender_seg[o++] = (*q == ':' || *q == '.') ? '_' : *q;
        }
        sender_seg[o] = '\0';
    }
    char request_path[192];
    snprintf(request_path, sizeof request_path,
             "/org/freedesktop/portal/desktop/request/%s/%s",
             sender_seg, token);

    /* The refusal leg: an error reply INSTEAD of the handle — the
     * portal-missing failure mode every client must survive. */
    if (g_fc_refuse) {
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.portal.Error.NotFound",
            "no file chooser backend");
        if (err != NULL) {
            (void)fdk_dbus_connection_send(c, err);
        }
        return;
    }

    /* Reply the handle... */
    fdk_dbus_message *reply =
        fdk_dbus_message_new_method_return(call);
    if (reply == NULL) {
        return;
    }
    (void)fdk_dbus_message_append_object_path(reply, request_path);
    (void)fdk_dbus_connection_send(c, reply);

    /* ...then the Response signal on it: response=1 for the
     * "fdk-dismiss" sentinel (the cancelled leg), response=0 with
     * the uris otherwise. */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        request_path, "org.freedesktop.portal.Request", "Response");
    if (sig == NULL) {
        return;
    }
    (void)fdk_dbus_message_append_uint32(
        sig, strcmp(g_fc_last_title, "fdk-dismiss") == 0 ? 1u : 0u);
    /* results: one section with the uris array */
    fdk_dbus_value *uris[2];
    uris[0] = fdk_dbus_value_new_string(
        "file:///etc/hostname");
    uris[1] = fdk_dbus_value_new_string(
        "file:///etc/os%2Drelease"); /* percent-encoded '-' */
    fdk_dbus_value *uri_arr =
        fdk_dbus_value_new_array("s", uris, 2);
    /* The results: a(sa{sv}) — one section ("", {"uris": [...]}).
     * The dict's VALUE must be a VARIANT wrapping the uri array (the
     * {sv} grammar; the tray-hint precedent's shape). */
    fdk_dbus_value *uris_var = fdk_dbus_value_new_variant(uri_arr);
    fdk_dbus_value_free(uri_arr); /* new_variant copies its child */
    fdk_dbus_value *uris_entry = fdk_dbus_value_new_dict_entry(
        fdk_dbus_value_new_string("uris"), uris_var);
    fdk_dbus_value *dict =
        fdk_dbus_value_new_array("{sv}",
                                  (fdk_dbus_value *[]){uris_entry}, 1);
    fdk_dbus_value *section_items[2];
    section_items[0] = fdk_dbus_value_new_string("");
    section_items[1] = dict;
    fdk_dbus_value *section = fdk_dbus_value_new_struct(
        section_items, 2);
    fdk_dbus_value *sections[1] = {section};
    fdk_dbus_value *results_arr =
        fdk_dbus_value_new_array("(sa{sv})", sections, 1);
    if (results_arr != NULL) {
        (void)fdk_dbus_message_append(sig, results_arr);
        fdk_dbus_value_free(results_arr);
    }
    (void)fdk_dbus_connection_send(c, sig);
}

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

    /* ---- the cross-implementation leg (ported from the alt
     * client's live battery, 2026-10-27): dbus-send — libdbus on
     * the other end — calls the service A serves, and libdbus's own
     * parser must accept FDK's reply bytes and print the value.
     * Two independent D-Bus implementations agreeing on the wire is
     * the strongest headless proof of marshal correctness there is.
     * Self-skips (disclosed) when dbus-send is absent. The polling
     * shape is the pclose-deadlock lesson the alt client paid for:
     * the child cannot finish before A answers, and A can only
     * answer while THIS loop pumps the bus. */
    {
        if (system("command -v dbus-send >/dev/null 2>&1") != 0) {
            printf("[skip] no dbus-send on PATH — the libdbus "
                   "cross-implementation reply leg\n");
        } else {
            FILE *out = popen(
                "dbus-send --print-reply --reply-timeout=8000 "
                "--dest=org.fdktest.Echo /org/fdktest/echo "
                "org.fdktest.Echo.Echo string:'cross-implementation "
                "hello' 2>&1",
                "r");
            assert(out != NULL);
            char xbuf[4096];
            size_t xgot = 0;
            int xdone = 0;
            int xms = 0;
            struct pollfd xpf = {.fd = fileno(out), .events = POLLIN};
            while (!xdone && xgot < sizeof xbuf - 1) {
                int pr = poll(&xpf, 1, 50);
                if (pr > 0) {
                    size_t n = fread(xbuf + xgot, 1,
                                     sizeof xbuf - 1 - xgot, out);
                    if (n == 0) {
                        xdone = 1; /* EOF: the child exited */
                    } else {
                        xgot += n;
                    }
                } else if (pr == 0) {
                    xms += 50;
                    assert(xms < 15000); /* bounded even if hung */
                    (void)fdk_pump_events(ctx, 20);
                } else {
                    assert(errno == EINTR);
                }
            }
            int xrc = pclose(out);
            xbuf[xgot] = '\0';
            if (xrc != 0 || strstr(xbuf, "cross-implementation hello") == NULL) {
                fprintf(stderr, "cross-leg diagnostics rc=%d:\n%s\n", xrc, xbuf);
            }
            assert(xrc == 0); /* dbus-send itself succeeded */
            assert(strstr(xbuf, "cross-implementation hello") !=
                   NULL); /* libdbus parsed and printed FDK's bytes */
            assert(strstr(xbuf, "string") != NULL); /* typed output */
            printf("[ok] cross-implementation: libdbus (dbus-send) "
                   "parsed FDK's served reply bytes and printed them "
                   "back\n");
        }
    }

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
        /* NameLost is a SEPARATE unicast to A that may trail the
         * broadcast NameOwnerChanged by a dispatch batch — wait it
         * out (bounded) instead of racing it. */
        for (int i = 0; i < 100 && name_events_lost < 1; i++) {
            (void)fdk_pump_events(ctx, 20);
        }
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

    /* ---- the mock portal: the color-scheme D-Bus channel -----------------
     *
     * Connection A BECOMES org.freedesktop.portal.Desktop (no real
     * portal runs under dbus-run-session), serves ReadOne, and emits
     * SettingChanged — the exact surface the theme engine's portal
     * singleton consumes. The engine attaches through the pump's
     * scheme tick (no XSETTINGS daemon exists in this container, so
     * the portal is the active source); the scheme value and the
     * theme re-check are asserted end to end. */
    {
        /* Hermetic theme fixtures: a face pair on a private path. */
        static char theme_root[] = "/tmp/fdk-portal-themes-XXXXXX";
        assert(mkdtemp(theme_root) != NULL);
        char path[768];
        const char *faces[2][3] = {
            {"daylight", "Daylight", "#88ccff"},
            {"daylight-dark", "Daylight-Dark", "#ffcc88"},
        };
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s/%s.fdk", theme_root,
                     faces[i][0]);
            FILE *f = fopen(path, "w");
            assert(f != NULL);
            fprintf(f,
                    "version = 1\n"
                    "name = \"%s\"\n"
                    "author = \"portal test\"\n"
                    "\n"
                    "[colors]\n"
                    "accent = %s\n",
                    faces[i][1], faces[i][2]);
            fclose(f);
        }
        const char *saved_theme = getenv("FDK_THEME");
        const char *saved_dir = getenv("FDK_THEME_DIR");
        setenv("FDK_THEME", "daylight", 1);
        setenv("FDK_THEME_DIR", theme_root, 1);
        unsetenv("FDK_COLOR_SCHEME");

        /* Reset the engines; the XSETTINGS poll finds no daemon (no
         * DISPLAY-side manager in this rig), so the portal is the
         * active source once the pump ticks. */
        fdk__theme_boot_reset_for_tests();
        fdk__color_scheme_reset_for_tests();

        /* A serves the portal: RequestName first. */
        static int portal_owned = 0;
        fdk_dbus_pending *pp = fdk_dbus_connection_request_name(
            a, "org.freedesktop.portal.Desktop",
            FDK_DBUS_NAME_REPLACE_EXISTING | FDK_DBUS_NAME_DO_NOT_QUEUE,
            reqname_reply, NULL);
        assert(pp != NULL);
        assert(pump_until(ctx, &g_reqname_done, 10000));
        assert(g_reqname_code == FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER);
        portal_owned = 1;
        (void)portal_owned;

        /* The portal method handler: ReadOne(color-scheme) -> (v). */
        fdk_dbus_connection_set_method_handler(a, portal_method_handler,
                                               NULL);

        /* Pump: the theme engine's scheme tick attaches its own
         * connection to the (mock) portal, ReadOne lands, the
         * recheck installs the DARK variant face. */
        int portal_scheme_dark = 0;
        for (int i = 0; i < 100 && !portal_scheme_dark; i++) {
            (void)fdk_pump_events(ctx, 20);
            portal_scheme_dark =
                (fdk_system_color_scheme() == FDK_COLOR_SCHEME_DARK);
        }
        assert(portal_scheme_dark);
        assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                      "Daylight-Dark") == 0);
        printf("[ok] portal color-scheme: ReadOne through the mock "
               "portal — dark, variant face installed\n");

        /* LIVE: the portal emits SettingChanged(color-scheme=light);
         * the engine's subscription must flip the scheme and the
         * theme back to the base face. */
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.portal.Settings", "SettingChanged");
        assert(sig != NULL);
        (void)fdk_dbus_message_append_string(sig,
                                             "org.freedesktop.appearance");
        (void)fdk_dbus_message_append_string(sig, "color-scheme");
        (void)fdk_dbus_message_append_variant_uint32(sig, 2);
        assert(fdk_dbus_connection_send(a, sig) == FDK_OK);
        int portal_scheme_light = 0;
        for (int i = 0; i < 100 && !portal_scheme_light; i++) {
            (void)fdk_pump_events(ctx, 20);
            portal_scheme_light =
                (fdk_system_color_scheme() == FDK_COLOR_SCHEME_LIGHT);
        }
        assert(portal_scheme_light);
        assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                      "Daylight") == 0);
        printf("[ok] portal color-scheme: SettingChanged flips the "
               "scheme + theme live\n");

        /* Restore: the engine's portal connection is closed by the
         * scheme shutdown below; env back; fixtures removed. */
        fdk__theme_boot_reset_for_tests(); /* closes the installed
                                            * theme cleanly */
        fdk__color_scheme_reset_for_tests();
        if (saved_theme != NULL) {
            setenv("FDK_THEME", saved_theme, 1);
        } else {
            unsetenv("FDK_THEME");
        }
        if (saved_dir != NULL) {
            setenv("FDK_THEME_DIR", saved_dir, 1);
        } else {
            unsetenv("FDK_THEME_DIR");
        }
        for (int i = 0; i < 2; i++) {
            snprintf(path, sizeof path, "%s/%s.fdk", theme_root,
                     faces[i][0]);
            remove(path);
        }
        remove(theme_root);
    }

    /* ---- Idle inhibition vs the mock ScreenSaver (1.1.0, S-3) ---- */
    {
        g_reqname_done = 0;
        fdk_dbus_pending *sp = fdk_dbus_connection_request_name(
            a, "org.freedesktop.ScreenSaver",
            FDK_DBUS_NAME_REPLACE_EXISTING | FDK_DBUS_NAME_DO_NOT_QUEUE,
            reqname_reply, NULL);
        assert(sp != NULL);
        assert(pump_until(ctx, &g_reqname_done, 10000));
        assert(g_reqname_code == FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER);
        fdk_dbus_connection_set_method_handler(a, screensaver_handler,
                                               NULL);
        printf("[ok] idle: mock ScreenSaver service owns the name\n");

        /* LEG 1: the cookie round trip. */
        g_ss_inhibits = 0;
        g_ss_uninhibits = 0;
        fdk_idle_inhibitor *inh = NULL;
        assert(fdk_ok(fdk_idle_inhibit(ctx, NULL, "fdk-tests",
                                       "playing a video", &inh)));
        assert(inh != NULL);
        int active = 0;
        for (int i = 0; i < 150 && !active; i++) {
            (void)fdk_pump_events(ctx, 20);
            active = fdk_idle_inhibitor_status(inh) ==
                     FDK_IDLE_INHIBIT_ACTIVE;
        }
        assert(active);
        assert(g_ss_inhibits == 1);
        assert(strcmp(g_ss_last_app, "fdk-tests") == 0);
        assert(strcmp(g_ss_last_reason, "playing a video") == 0);
        fdk_idle_uninhibit(inh);
        for (int i = 0; i < 150 && g_ss_uninhibits == 0; i++) {
            (void)fdk_pump_events(ctx, 20);
        }
        assert(g_ss_uninhibits == 1);
        assert(g_ss_last_uninhibited == g_ss_last_cookie);
        printf("[ok] idle: inhibit -> cookie -> ACTIVE; uninhibit "
               "sent the matching cookie\n");

        /* LEG 2: the DEFERRED release — the handle is dropped BEFORE
         * the cookie lands (no pump between the calls); the desktop
         * must still see the matching UnInhibit (cancelling would
         * orphan the inhibition). */
        fdk_idle_inhibitor *inh2 = NULL;
        assert(fdk_ok(fdk_idle_inhibit(ctx, NULL, "fdk-tests",
                                       "early release", &inh2)));
        fdk_idle_uninhibit(inh2); /* cookie still in flight */
        int settled = 0;
        for (int i = 0; i < 200 && !settled; i++) {
            (void)fdk_pump_events(ctx, 20);
            settled = (g_ss_inhibits == 2 && g_ss_uninhibits == 2);
        }
        assert(settled);
        assert(g_ss_last_uninhibited == g_ss_last_cookie);
        printf("[ok] idle: release-before-cookie still un-inhibits "
               "(the deferred release, no orphan)\n");

        /* LEG 3: the error path — the service refuses the sentinel
         * app; the inhibitor degrades to UNSUPPORTED and releases
         * cleanly. */
        fdk_idle_inhibitor *inh3 = NULL;
        assert(fdk_ok(fdk_idle_inhibit(ctx, NULL, "please-fail",
                                       "x", &inh3)));
        int unsup = 0;
        for (int i = 0; i < 150 && !unsup; i++) {
            (void)fdk_pump_events(ctx, 20);
            unsup = fdk_idle_inhibitor_status(inh3) ==
                    FDK_IDLE_INHIBIT_UNSUPPORTED;
        }
        assert(unsup);
        fdk_idle_uninhibit(inh3);
        assert(g_ss_inhibits == 2); /* the refusal issued no cookie */
        printf("[ok] idle: service refusal degrades to UNSUPPORTED\n");
    }

    /* ---- The FileChooser portal (1.1.0, C-1 / B-09) ----------------------
     *
     * A already owns org.freedesktop.portal.Desktop (the color-scheme
     * section took it; one name, two interfaces — exactly how the
     * real portal multiplexes). The handler swaps to the FileChooser
     * one and the legs run:
     *
     *   LEG 1 (the happy path, forced): OPEN_FILES with title /
     *        start_dir / filters — the mock asserts the call's OPTION
     *        SHAPE (directory/multiple off, the current_folder hint,
     *        two filter structs) and answers a handle + Response(0,
     *        uris[hostname, os%2Drelease]); FDK delivers ACCEPTED
     *        with the two POSIX paths (the percent-decode proven).
     *   LEG 2 (dismissed): response=1 -> CANCELLED, no paths.
     *   LEG 3 (OPEN_FOLDER): the directory choice ON in the call the
     *        mock sees.
     *   LEG 4 (auto-mode fallback): FDK_FILE_DIALOG_PORTAL unset and
     *        the mock REFUSING (an error reply) — the pick falls back
     *        to the toolkit's own browser (which under this bare
     *        harness creates its real window: assert the fallback
     *        produced an FDK window / a browser-side result, i.e. the
     *        callback fired from the OWN dialog, by watching the
     *        window count through a pumped idle).
     */
    {
        fdk_dbus_connection_set_method_handler(
            a, filechooser_handler, NULL);
        setenv("FDK_FILE_DIALOG_PORTAL", "1", 1);

        /* LEG 1 */
        memset(&g_fc_result, 0, sizeof g_fc_result);
        g_fc_calls = 0;
        fdk_file_dialog_options fcopts = {0};
        fcopts.title = "Pick files";
        fcopts.start_dir = "/etc";
        fcopts.filters = "*.conf;*.cfg";
        fcopts.kind = FDK_FILE_DIALOG_OPEN_FILES;
        assert(fdk_ok(fdk_dialog_open_file(ctx, &fcopts, fc_done_cb,
                                           NULL, NULL)));
        int got = 0;
        for (int i = 0; i < 200 && !got; i++) {
            (void)fdk_pump_events(ctx, 20);
            got = (g_fc_result.done != 0);
        }
        assert(got);
        assert(g_fc_result.outcome == FDK_FILE_DIALOG_ACCEPTED);
        assert(g_fc_result.count == 2);
        assert(strcmp(g_fc_result.paths[0], "/etc/hostname") == 0);
        assert(strcmp(g_fc_result.paths[1], "/etc/os-release") == 0);
        /* The call shape the mock saw. */
        assert(g_fc_calls == 1);
        assert(strcmp(g_fc_last_title, "Pick files") == 0);
        assert(g_fc_last_directory == 0);
        assert(g_fc_last_multiple == 1);
        assert(strcmp(g_fc_last_current_folder, "/etc") == 0);
        assert(g_fc_last_filters == 2);
        assert(g_fc_last_token[0] != '\0');
        printf("[ok] portal file chooser: OPEN_FILES -> options shape "
               "(multiple on, folder hint, 2 filters), Response "
               "decoded, percent-decoded POSIX paths\n");

        /* LEG 2: dismissed. */
        memset(&g_fc_result, 0, sizeof g_fc_result);
        g_fc_calls = 0;
        fdk_file_dialog_options dfopts = {0};
        dfopts.title = "fdk-dismiss";
        dfopts.kind = FDK_FILE_DIALOG_OPEN_FILE;
        assert(fdk_ok(fdk_dialog_open_file(ctx, &dfopts, fc_done_cb,
                                           NULL, NULL)));
        got = 0;
        for (int i = 0; i < 200 && !got; i++) {
            (void)fdk_pump_events(ctx, 20);
            got = (g_fc_result.done != 0);
        }
        assert(got);
        assert(g_fc_result.outcome == FDK_FILE_DIALOG_CANCELLED);
        assert(g_fc_result.count == 0);
        printf("[ok] portal file chooser: Response(1) -> CANCELLED, "
               "no paths\n");

        /* LEG 3: the folder kind sets the directory choice. */
        memset(&g_fc_result, 0, sizeof g_fc_result);
        g_fc_calls = 0;
        fdk_file_dialog_options folopts = {0};
        folopts.title = "Pick a folder";
        folopts.kind = FDK_FILE_DIALOG_OPEN_FOLDER;
        assert(fdk_ok(fdk_dialog_open_file(ctx, &folopts, fc_done_cb,
                                           NULL, NULL)));
        got = 0;
        for (int i = 0; i < 200 && !got; i++) {
            (void)fdk_pump_events(ctx, 20);
            got = (g_fc_result.done != 0);
        }
        assert(got);
        assert(g_fc_last_directory == 1);
        assert(g_fc_last_multiple == 0);
        printf("[ok] portal file chooser: OPEN_FOLDER -> the "
               "directory choice in the call\n");

        /* LEG 4: auto mode + the portal REFUSING (error reply) — the
         * fallback hands the pick to the toolkit's own browser. The
         * own browser under this harness really runs (it creates a
         * window and browses /); the assert is that the callback
         * fires AT ALL with a result whose origin is the own
         * browser: the mock saw the call, refused it, and the dialog
         * machinery produced a REAL FDK window (fdk_window_count > 0
         * while the browser is up, before any answer). */
        unsetenv("FDK_FILE_DIALOG_PORTAL");
        g_fc_refuse = 1; /* the mock answers an error reply */
        memset(&g_fc_result, 0, sizeof g_fc_result);
        g_fc_calls = 0;
        fdk_file_dialog_options rfopts = {0};
        rfopts.title = "Refuse me";
        rfopts.start_dir = "/etc";
        rfopts.kind = FDK_FILE_DIALOG_OPEN_FILE;
        assert(fdk_ok(fdk_dialog_open_file(ctx, &rfopts, fc_done_cb,
                                           NULL, NULL)));
        int browser_up = 0;
        int answered = 0;
        for (int i = 0; i < 400 && !answered; i++) {
            (void)fdk_pump_events(ctx, 20);
            if (!browser_up && ctx->window_count > 0) {
                browser_up = 1;
            }
            answered = (g_fc_result.done != 0);
        }
        assert(g_fc_calls == 1); /* the portal WAS consulted first */
        assert(browser_up); /* the own browser took over */
        printf("[ok] portal file chooser: refusal in auto mode -> the "
               "toolkit's own browser took the pick\n");
        g_fc_refuse = 0;

        /* The browser window from LEG 4 is still up (nobody
         * answered it): destroy it directly — the dialog's window
         * dies, the done callback fires CANCELLED (the early-destroy
         * contract). */
        while (ctx->window_count > 0) {
            fdk_window_destroy(ctx->windows[0]);
        }
        for (int i = 0; i < 100 && g_fc_result.done == 0; i++) {
            (void)fdk_pump_events(ctx, 20);
        }
        /* (LEG 4's callback was already satisfied by the destroy;
         * LEG 5 resets the state — no assert needed here beyond the
         * clean window count.) */

        /* LEG 5 (the forced-mode refusal): FDK_FILE_DIALOG_PORTAL=1
         * and the portal refusing — the dialog must FAIL the call
         * (no fallback), and the async refusal delivers the ERROR
         * outcome (the callback contract). */
        setenv("FDK_FILE_DIALOG_PORTAL", "1", 1);
        g_fc_refuse = 1;
        memset(&g_fc_result, 0, sizeof g_fc_result);
        g_fc_calls = 0;
        fdk_file_dialog_options fropts = {0};
        fropts.title = "Refuse forced";
        fropts.start_dir = "/etc";
        fropts.kind = FDK_FILE_DIALOG_OPEN_FILE;
        assert(fdk_ok(fdk_dialog_open_file(ctx, &fropts, fc_done_cb,
                                           NULL, NULL)));
        got = 0;
        for (int i = 0; i < 200 && !got; i++) {
            (void)fdk_pump_events(ctx, 20);
            got = (g_fc_result.done != 0);
        }
        assert(got);
        assert(g_fc_result.outcome == FDK_FILE_DIALOG_ERROR);
        assert(ctx->window_count == 0); /* no fallback window */
        g_fc_refuse = 0;
        unsetenv("FDK_FILE_DIALOG_PORTAL");
        printf("[ok] portal file chooser: forced-mode refusal -> the "
               "ERROR outcome, no fallback\n");
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
