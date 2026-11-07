#define FDK_LOG_TAG "dialog"

/*
 * portal_file_chooser.c — the xdg-desktop-portal FileChooser client
 * (1.1.0, C-1 / B-09).
 *
 * The desktop's own file picker, over the native D-Bus client: when a
 * portal service answers org.freedesktop.portal.FileChooser on the
 * session bus, fdk_dialog_open_file / fdk_dialog_save_file hand the
 * pick to IT (the sandbox-aware, system-styled chooser); any failure
 * anywhere along the way falls back to the toolkit's own browser
 * (file_dialog.c) so the application ALWAYS gets a picker — the GTK
 * posture, and the roadmap's D-002 decision (core work on the native
 * client, not a Flatpak-only nicety).
 *
 * The protocol dance (the portal spec, version 1):
 *
 *   1. OpenFile(parent_window, title, options{a(sv)}) -> o handle
 *      (the options dict carries handle_token — the request's identity
 *      in the returned handle's last path segment — plus the kind's
 *      choices: directory, multiple, the filters, the current-name /
 *      current-folder / current-file hints);
 *   2. subscribe to org.freedesktop.portal.Request::Response on the
 *      PREDICTABLE request path /org/freedesktop/portal/desktop/
 *      request/<SENDER>/<TOKEN> — the sender segment is the unique
 *      name with the ':' -> '_' rewrite, so the subscription can be
 *      armed BEFORE the OpenFile call goes out (the spec's own
 *      race-free ordering);
 *   3. the Response signal carries (u response, a(sa{sv}) results):
 *      0 = accepted (the results dict's "uris" string array is the
 *      pick), 1 = dismissed; everything else the portal documents as
 *      an error the caller treats as a dismissal.
 *
 * URIs decode to POSIX paths (file:// stripped, percent-decoding via
 * the shared dnd_uri codec — the same wire format XDND speaks).
 *
 * The ONE-FLIGHT discipline: one portal request at a time per
 * context (a second dialog while one flies answers UNSUPPORTED —
 * the same one-at-a-time honesty the own-browser path has never
 * needed, being window-scoped). All state hangs off the context in
 * a single static block; the fallback path frees it all.
 */

#include "fdk/fdk_dialog.h"
#include "fdk/fdk_dbus.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "../widget/dialog_portal_internal.h"

/* The fallback half's exported state (file_dialog.c owns the
 * storage): the saved options + the flight's callback/context. The
 * STRINGS in the saved options are re-pointed at THIS module's
 * copies before every fallback call (the application's originals
 * were borrowed and may be gone; the flight's copies die only after
 * the fallback returns). */
extern fdk_file_dialog_options fdk__portal_saved_options;
extern int fdk__portal_saved_valid;
extern fdk_context *g_flight_ctx;
extern fdk_file_dialog_done_fn g_flight_on_done;
extern void *g_flight_user;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Local copy (the widget layer's fdk__strdup is not exported to the
 * desktop modules; a 6-line helper beats a cross-layer dependency). */
static char *pfc_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = fdk_alloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

/* ---- constants (the portal spec's names) -------------------------------- */

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE_FILECHOOSER "org.freedesktop.portal.FileChooser"
#define PORTAL_IFACE_REQUEST "org.freedesktop.portal.Request"
#define PORTAL_MEMBER_RESPONSE "Response"
#define PORTAL_REQUEST_PREFIX \
    "/org/freedesktop/portal/desktop/request/"
#define PORTAL_TIMEOUT_MS 25000u /* the dialog has no deadline; the    \
                                    * CALL does — a hung portal falls  \
                                    * back instead of wedging the app  */

/* The one-flight state (see the file comment). */
static struct {
    fdk_context *ctx;
    fdk_file_dialog_done_fn on_done;
    void *user;
    fdk_file_dialog_kind kind;
    fdk_dbus_connection *bus;
    fdk_dbus_subscription *sub;
    char request_path[192]; /* the subscribed path (for teardown)   */
    char *title_copy;       /* argument copies for the retry-free   */
    char *start_dir_copy;
    char *start_name_copy;
    char *filters_copy;
    int armed;              /* subscription armed, call in flight   */
    int answered;           /* the response fired (or fallback ran) */
} g_flight;

/* ---- small helpers ------------------------------------------------------ */

int fdk__dialog_portal_pref(void) {
    const char *v = getenv("FDK_FILE_DIALOG_PORTAL");
    if (v == NULL || v[0] == '\0') {
        return -1; /* auto */
    }
    if (strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
        strcasecmp(v, "no") == 0) {
        return 0;
    }
    return 1;
}

/* The request path for a token on OUR unique name (the spec's
 * sender-segment rule: ':1.42' -> '_1_42'). The buffer is 160 bytes
 * (the prefix is 41, the sender segment <= 95 rewritten, the token
 * bounded by the spec's [a-zA-Z0-9_]+ grammar and our own mint) —
 * a pathological unique name would truncate, the subscription then
 * would not match, and the flight times out into the fallback:
 * degraded, never fatal. */
#define PFC_REQUEST_PATH_CAP 192u
static void request_path_for(char *out,
                             const char *unique_name,
                             const char *token) {
    char sender[96];
    size_t o = 0;
    for (const char *p = unique_name;
         p != NULL && *p != '\0' && o + 1 < sizeof sender; p++) {
        if (*p == ':') {
            sender[o++] = '_';
        } else if (*p == '.') {
            sender[o++] = '_';
        } else {
            sender[o++] = *p;
        }
    }
    sender[o] = '\0';
    (void)snprintf(out, (size_t)PFC_REQUEST_PATH_CAP, "%s%s/%s",
                   PORTAL_REQUEST_PREFIX, sender, token);
}

/* One (sv) pair appended to the dict-in-progress. The VALUE is
 * wrapped in a VARIANT (the signature demands it — a bare value is
 * the mistake the array validator rejects). Takes ownership of the
 * value node; returns the pair or NULL (everything freed). */
static fdk_dbus_value *sv_pair(const char *key, fdk_dbus_value *val) {
    if (key == NULL || val == NULL) {
        fdk_dbus_value_free(val);
        return NULL;
    }
    fdk_dbus_value *var = fdk_dbus_value_new_variant(val);
    fdk_dbus_value_free(val); /* new_variant COPIES its child */
    if (var == NULL) {
        return NULL;
    }
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    if (k == NULL) {
        fdk_dbus_value_free(var);
        return NULL;
    }
    fdk_dbus_value *pair = fdk_dbus_value_new_struct(
        (fdk_dbus_value *[]){k, var}, 2);
    if (pair == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
    }
    return pair;
}

/* ---- the Response signal ------------------------------------------------ */

/* URI -> path (the dnd codec's decode rules: file:// stripped,
 * percent-decoding applied, non-file URIs pass through as-is).
 * Returns a fresh allocation or NULL. */
static char *uri_to_path(const char *uri) {
    if (uri == NULL) {
        return NULL;
    }
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
        /* strip the authority (an empty one is the common case; a
         * nonempty one is a remote file we present honestly) */
        const char *slash = strchr(p, '/');
        if (slash == NULL) {
            return NULL;
        }
        p = slash;
    }
    /* percent-decode in place into a fresh buffer */
    char *out = fdk_alloc(strlen(p) + 1);
    if (out == NULL) {
        return NULL;
    }
    char *o = out;
    for (const char *q = p; *q != '\0';) {
        if (q[0] == '%' && q[1] != '\0' && q[2] != '\0') {
            int hi = q[1];
            int lo = q[2];
            int h = (hi >= '0' && hi <= '9') ? hi - '0'
                    : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                    : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
            int l = (lo >= '0' && lo <= '9') ? lo - '0'
                    : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                    : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
            if (h >= 0 && l >= 0) {
                *o++ = (char)((h << 4) | l);
                q += 3;
                continue;
            }
        }
        *o++ = *q++;
    }
    *o = '\0';
    return out;
}

static void deliver_result(fdk_file_dialog_outcome outcome, char **paths,
                           size_t count) {
    fdk_file_dialog_result r = {.outcome = outcome, .paths = paths,
                                .count = count};
    if (g_flight.on_done != NULL) {
        g_flight.on_done(&r, g_flight.user);
    }
    /* paths ownership passes to no one: free them after the callback
     * (the own-browser contract — the result is valid for the call
     * only). */
    for (size_t i = 0; i < count; i++) {
        fdk_free(paths[i]);
    }
    fdk_free(paths);
}

static void portal_response(fdk_dbus_connection *c,
                            fdk_dbus_message *signal, void *user) {
    (void)c;
    (void)user;
    if (!g_flight.armed || g_flight.answered) {
        return; /* stale signal after a fallback (the sub is freed) */
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(signal, &args, &n) != FDK_OK ||
        n < 2 || args == NULL) {
        fdk_dbus_values_free_array(args, n);
        g_flight.answered = 1;
        deliver_result(FDK_FILE_DIALOG_CANCELLED, NULL, 0);
        fdk__dialog_portal_flight_clear();
        return;
    }
    uint32_t response = fdk_dbus_value_uint32(args[0]);

    /* results: a(sa{sv}) — each section is (id, dict); the "uris"
     * key carries the pick. The dict_lookup helper is exactly the
     * a{sv} reader this (and every other portal protocol) leans on. */
    char **paths = NULL;
    size_t count = 0;
    if (response == 0 && fdk_dbus_value_type(args[1]) == FDK_DBUS_ARRAY) {
        size_t sections = fdk_dbus_value_array_count(args[1]);
        for (size_t s = 0; s < sections; s++) {
            const fdk_dbus_value *sec =
                fdk_dbus_value_array_item(args[1], s);
            if (sec == NULL ||
                fdk_dbus_value_struct_count(sec) < 2) {
                continue;
            }
            const fdk_dbus_value *dict =
                fdk_dbus_value_struct_item(sec, 1);
            const fdk_dbus_value *uris_v =
                fdk_dbus_value_dict_lookup(dict, "uris");
            if (uris_v == NULL ||
                fdk_dbus_value_type(uris_v) != FDK_DBUS_VARIANT) {
                continue;
            }
            /* The {sv} grammar: the entry's value is a VARIANT
             * wrapping the "as" array — unwrap before counting. */
            const fdk_dbus_value *uris_arr =
                fdk_dbus_value_variant_value(uris_v);
            if (uris_arr == NULL ||
                fdk_dbus_value_type(uris_arr) != FDK_DBUS_ARRAY) {
                continue;
            }
            size_t uris = fdk_dbus_value_array_count(uris_arr);
            for (size_t u = 0; u < uris; u++) {
                const fdk_dbus_value *uv =
                    fdk_dbus_value_array_item(uris_arr, u);
                const char *uri =
                    uv != NULL ? fdk_dbus_value_string(uv) : NULL;
                char *path = uri_to_path(uri);
                if (path == NULL) {
                    continue;
                }
                char **grown =
                    fdk_realloc(paths, (count + 1) * sizeof(*grown));
                if (grown == NULL) {
                    fdk_free(path);
                    continue;
                }
                paths = grown;
                paths[count++] = path;
            }
        }
    }
    fdk_dbus_values_free_array(args, n);

    g_flight.answered = 1;
    if (response == 0 && count > 0) {
        deliver_result(FDK_FILE_DIALOG_ACCEPTED, paths, count);
    } else {
        /* dismissed (or a zero-uri accept — the honest reading of a
         * pick with nothing picked is the same CANCELLED the
         * own-browser path delivers) */
        for (size_t i = 0; i < count; i++) {
            fdk_free(paths[i]);
        }
        fdk_free(paths);
        deliver_result(FDK_FILE_DIALOG_CANCELLED, NULL, 0);
    }
    fdk__dialog_portal_flight_clear();
}

/* ---- teardown / fallback ------------------------------------------------ */

/* Routes a failure: FORCED mode delivers the honest ERROR result
 * (the application asked portal-only); AUTO mode re-points the saved
 * options' strings at the flight's copies and hands the pick to the
 * toolkit's own browser. The flight clear ALWAYS follows (the
 * callers do it — this function must not, the fallback reads the
 * copies before clearing). */
static void portal_failed_async(void) {
    if (fdk__dialog_portal_pref() == 1) {
        /* forced: no fallback — the dialog ends in ERROR */
        g_flight.answered = 1;
        fdk_file_dialog_result r = {FDK_FILE_DIALOG_ERROR, NULL, 0};
        if (g_flight_on_done != NULL) {
            g_flight_on_done(&r, g_flight_user);
        }
        fdk__dialog_portal_flight_clear();
        return;
    }
    /* auto: the strings the fallback will read must be OUR copies
     * (the originals were borrowed and may be gone). */
    if (fdk__portal_saved_valid) {
        fdk__portal_saved_options.title = g_flight.title_copy;
        fdk__portal_saved_options.start_dir = g_flight.start_dir_copy;
        fdk__portal_saved_options.start_name = g_flight.start_name_copy;
        fdk__portal_saved_options.filters = g_flight.filters_copy;
    }
    fdk__dialog_fallback_own_browser();
    fdk__dialog_portal_flight_clear();
}

void fdk__dialog_portal_flight_clear(void) {
    if (g_flight.sub != NULL) {
        fdk_dbus_subscription_free(g_flight.sub);
        g_flight.sub = NULL;
    }
    if (g_flight.bus != NULL) {
        fdk_dbus_connection_close(g_flight.bus);
        g_flight.bus = NULL;
    }
    fdk_free(g_flight.title_copy);
    g_flight.title_copy = NULL;
    fdk_free(g_flight.start_dir_copy);
    g_flight.start_dir_copy = NULL;
    fdk_free(g_flight.start_name_copy);
    g_flight.start_name_copy = NULL;
    fdk_free(g_flight.filters_copy);
    g_flight.filters_copy = NULL;
    g_flight.armed = 0;
    g_flight.ctx = NULL;
    g_flight.on_done = NULL;
    g_flight.user = NULL;
}

/* The OpenFile/SaveFile reply: the handle, or the failure that
 * falls back. */
static void openfile_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                           void *user) {
    (void)user;
    if (!g_flight.armed || g_flight.answered) {
        return;
    }
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        /* refused / timeout / dead: the routed failure — the own
         * browser takes over in auto mode, ERROR in forced mode. */
        portal_failed_async();
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *handle = NULL;
    if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK && n >= 1 &&
        args != NULL) {
        handle = fdk_dbus_value_string(args[0]); /* covers OBJECT_PATH */
    }
    if (handle == NULL) {
        fdk_dbus_values_free_array(args, n);
        portal_failed_async();
        return;
    }
    /* The handle IS the request path we subscribed to (the spec's
     * token contract). If the portal chose its own token anyway, the
     * subscription would be wrong — re-subscribe on the REAL handle
     * before the signal can fire (the same ordering rule: subscribe
     * before the call that can trigger; the call is done, so a
     * moment of subscription here is the documented degraded path). */
    if (strcmp(handle, g_flight.request_path) != 0) {
        if (g_flight.sub != NULL) {
            fdk_dbus_subscription_free(g_flight.sub);
            g_flight.sub = NULL;
        }
        char rule[320];
        snprintf(rule, sizeof rule,
                 "type='signal',interface='%s',member='%s',path='%s'",
                 PORTAL_IFACE_REQUEST, PORTAL_MEMBER_RESPONSE, handle);
        g_flight.sub =
            fdk_dbus_connection_subscribe(c, rule, portal_response, NULL);
        if (g_flight.sub == NULL) {
            fdk_dbus_values_free_array(args, n);
            portal_failed_async();
            return;
        }
        snprintf(g_flight.request_path, sizeof g_flight.request_path,
                 "%s", handle);
    }
    fdk_dbus_values_free_array(args, n);
    /* Armed and waiting: the Response signal (or the call timeout
     * through the subscription-less death path) completes the pick.
     * Nothing else to do here. */
}

/* ---- building the call --------------------------------------------------- */

/* The filters option (a(sa(us))) from the ";"-separated glob list:
 * each pattern becomes one portal filter ("Foo files" label from the
 * pattern itself). Returns the array node or NULL (no filters). */
static fdk_dbus_value *build_filters(const char *filters) {
    if (filters == NULL || filters[0] == '\0') {
        return NULL;
    }
    fdk_dbus_value *items[64];
    size_t n = 0;
    const char *p = filters;
    while (*p != '\0' && n < 64) {
        const char *semi = strchr(p, ';');
        size_t len = semi != NULL ? (size_t)(semi - p) : strlen(p);
        if (len == 0) {
            p = semi != NULL ? semi + 1 : p + len;
            continue;
        }
        char pat[128];
        if (len >= sizeof pat) {
            len = sizeof pat - 1;
        }
        memcpy(pat, p, len);
        pat[len] = '\0';

        /* filter: (s label, a(us) patterns) — one pattern, weight 0 */
        char label[160];
        snprintf(label, sizeof label, "%s files", pat);
        fdk_dbus_value *pat_struct_items[2];
        pat_struct_items[0] = fdk_dbus_value_new_uint32(0);
        pat_struct_items[1] = fdk_dbus_value_new_string(pat);
        fdk_dbus_value *pat_struct = fdk_dbus_value_new_struct(
            pat_struct_items, 2);
        if (pat_struct == NULL) {
            fdk_dbus_value_free(pat_struct_items[0]);
            fdk_dbus_value_free(pat_struct_items[1]);
        }
        fdk_dbus_value *pat_array = NULL;
        if (pat_struct != NULL) {
            pat_array = fdk_dbus_value_new_array(
                "(us)", (fdk_dbus_value *[]){pat_struct}, 1);
            if (pat_array == NULL) {
                fdk_dbus_value_free(pat_struct);
            }
        }
        fdk_dbus_value *label_v = fdk_dbus_value_new_string(label);
        fdk_dbus_value *filter = NULL;
        if (pat_array != NULL && label_v != NULL) {
            filter = fdk_dbus_value_new_struct(
                (fdk_dbus_value *[]){label_v, pat_array}, 2);
        }
        if (filter == NULL) {
            fdk_dbus_value_free(label_v);
            fdk_dbus_value_free(pat_array);
        } else {
            items[n++] = filter;
        }
        p = semi != NULL ? semi + 1 : p + len;
    }
    if (n == 0) {
        return NULL;
    }
    fdk_dbus_value *arr =
        fdk_dbus_value_new_array("(sa(us))", items, n);
    if (arr == NULL) {
        fdk_dbus_values_free(items, n);
    }
    /* items' nodes are owned by arr on success */
    return arr;
}

/* Builds and sends the OpenFile/SaveFile call. Returns FDK_OK when
 * the call is on the wire (the flight is armed); anything else means
 * the caller should fall back. */
static fdk_result send_open_call(fdk_dbus_connection *c, const char *member,
                                 const char *parent_window) {
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE_FILECHOOSER, member);
    if (m == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (fdk_dbus_message_append_string(m, parent_window) != FDK_OK ||
        fdk_dbus_message_append_string(
            m, g_flight.title_copy != NULL ? g_flight.title_copy
                                           : "Open") != FDK_OK) {
        fdk_dbus_message_free(m);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* options: a(sv) */
    fdk_dbus_value *pairs[16];
    size_t np = 0;

    /* handle_token: the request's identity (spec-mandated practice) */
    char token[32];
    snprintf(token, sizeof token, "fdk%lx", (unsigned long)(size_t)m & 0xffffffffu);
    pairs[np++] =
        sv_pair("handle_token", fdk_dbus_value_new_string(token));

    /* the kind's choices */
    bool dir = g_flight.kind == FDK_FILE_DIALOG_OPEN_FOLDER ||
               g_flight.kind == FDK_FILE_DIALOG_OPEN_FOLDERS;
    bool multi = g_flight.kind == FDK_FILE_DIALOG_OPEN_FILES ||
                 g_flight.kind == FDK_FILE_DIALOG_OPEN_FOLDERS;
    if (dir) {
        pairs[np++] = sv_pair("directory", fdk_dbus_value_new_boolean(1));
    }
    if (multi) {
        pairs[np++] = sv_pair("multiple", fdk_dbus_value_new_boolean(1));
    }

    /* the hints */
    if (g_flight.kind == FDK_FILE_DIALOG_SAVE_FILE &&
        g_flight.start_name_copy != NULL) {
        pairs[np++] =
            sv_pair("current_name", fdk_dbus_value_new_string(
                                        g_flight.start_name_copy));
    }
    if (g_flight.start_dir_copy != NULL) {
        pairs[np++] =
            sv_pair("current_folder",
                    fdk_dbus_value_new_byte_array(
                        (const uint8_t *)g_flight.start_dir_copy,
                        strlen(g_flight.start_dir_copy)));
    }

    /* the filters */
    fdk_dbus_value *filters = build_filters(g_flight.filters_copy);
    if (filters != NULL && np < 16) {
        pairs[np++] = sv_pair("filters", filters);
    } else {
        fdk_dbus_value_free(filters);
    }

    /* the options array of pairs */
    fdk_dbus_value *opts =
        fdk_dbus_value_new_array("(sv)", pairs, np);
    if (opts == NULL) {
        fdk_dbus_values_free(pairs, np);
        fdk_dbus_message_free(m);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* pairs' nodes are owned by opts now */

    if (fdk_dbus_message_append(m, opts) != FDK_OK) {
        fdk_dbus_value_free(opts);
        fdk_dbus_message_free(m);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_dbus_value_free(opts);

    /* Subscribe FIRST (the race-free ordering): the request path is
     * predictable from OUR unique name + the token. */
    const char *unique = fdk_dbus_connection_unique_name(c);
    if (unique == NULL) {
        fdk_dbus_message_free(m);
        return FDK_ERR_PLATFORM; /* not ready — treat as fallback */
    }
    request_path_for(g_flight.request_path, unique, token);
    char rule[320];
    snprintf(rule, sizeof rule,
             "type='signal',interface='%s',member='%s',path='%s'",
             PORTAL_IFACE_REQUEST, PORTAL_MEMBER_RESPONSE,
             g_flight.request_path);
    g_flight.sub =
        fdk_dbus_connection_subscribe(c, rule, portal_response, NULL);
    if (g_flight.sub == NULL) {
        fdk_dbus_message_free(m);
        return FDK_ERR_PLATFORM;
    }

    fdk_dbus_pending *p = fdk_dbus_connection_call(
        c, m, PORTAL_TIMEOUT_MS, openfile_reply, NULL);
    if (p == NULL) {
        /* the message was NOT consumed on failure */
        fdk_dbus_subscription_free(g_flight.sub);
        g_flight.sub = NULL;
        fdk_dbus_message_free(m);
        return FDK_ERR_PLATFORM;
    }
    g_flight.armed = 1;
    return FDK_OK;
}

/* ---- the bus ready + entry ---------------------------------------------- */

static void portal_bus_ready(fdk_dbus_connection *c, int ok,
                             const char *err, void *user) {
    (void)user;
    (void)err; /* the INFO log line at connect time says enough */
    if (!g_flight.armed && !g_flight.ctx) {
        /* a stale flight (fallback already ran): drop the bus */
        fdk_dbus_connection_close(c);
        return;
    }
    if (!ok) {
        portal_failed_async();
        return;
    }
    const char *member =
        (g_flight.kind == FDK_FILE_DIALOG_SAVE_FILE) ? "SaveFile"
                                                     : "OpenFile";
    fdk_result r = send_open_call(c, member, "");
    if (!fdk_ok(r)) {
        portal_failed_async();
    } else {
        g_flight.bus = c; /* the flight owns the connection now */
    }
}

/* The entry point file_dialog.c calls BEFORE building its own
 * browser. Returns FDK_OK when the portal path took the pick (the
 * caller does nothing more); FDK_ERR_UNSUPPORTED when the portal is
 * unavailable/refused (the caller proceeds with the own browser);
 * any other error propagates as the dialog's own failure. */
fdk_result fdk__dialog_portal_try(fdk_context *ctx,
                                  const fdk_file_dialog_options *options,
                                  fdk_file_dialog_done_fn on_done,
                                  void *user_data) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int pref = fdk__dialog_portal_pref();
    if (pref == 0) {
        return FDK_ERR_UNSUPPORTED; /* explicitly off */
    }
    if (g_flight.armed) {
        return FDK_ERR_UNSUPPORTED; /* one flight at a time */
    }

    /* Copy what the flight needs (the options struct is borrowed). */
    memset(&g_flight, 0, sizeof g_flight);
    g_flight.ctx = ctx;
    g_flight.on_done = on_done;
    g_flight.user = user_data;
    g_flight.kind = (options != NULL)
                        ? options->kind
                        : FDK_FILE_DIALOG_OPEN_FILE;
    if (options != NULL) {
        if (options->title != NULL) {
            g_flight.title_copy = pfc_strdup(options->title);
        }
        if (options->start_dir != NULL) {
            g_flight.start_dir_copy = pfc_strdup(options->start_dir);
        }
        if (options->start_name != NULL) {
            g_flight.start_name_copy = pfc_strdup(options->start_name);
        }
        if (options->filters != NULL) {
            g_flight.filters_copy = pfc_strdup(options->filters);
        }
    }

    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, portal_bus_ready, NULL);
    if (bus == NULL) {
        /* No session bus at all. Forced mode: the honest failure the
         * application asked for (FDK_ERR_PLATFORM — UNSUPPORTED is
         * the internal fall-through signal the own-browser path
         * keys off). Auto mode: the own browser proceeds. */
        fdk__dialog_portal_flight_clear();
        return (pref == 1) ? FDK_ERR_PLATFORM : FDK_ERR_UNSUPPORTED;
    }
    g_flight.bus = bus; /* owned until ready decides */
    return FDK_OK;
}
