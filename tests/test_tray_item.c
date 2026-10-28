/* test_tray_item.c — the app-facing tray item + menu server against a
 * REAL bus and an in-test mock watcher/host (1.1.0)
 *
 * Runs under `make test-dbus` beside test_dbus_live.c and
 * test_notify_client.c (one dbus-run-session wrapper). The mock is a
 * second FDK connection that plays BOTH roles the item meets on a
 * real desktop:
 *
 *   - the WATCHER (org.kde.StatusNotifierWatcher): answers
 *     RegisterStatusNotifierItem, tracks the registered service
 *     name, serves the watcher properties, and can vanish/return
 *     (ReleaseName / RequestName) to prove the re-registration
 *     lifecycle;
 *   - the HOST: GetAll on the item's unique name at
 *     /StatusNotifierItem (every property shape-checked — the
 *     IconPixmap a(iiay) byte content through the flat reader),
 *     Activate / SecondaryActivate / Scroll calls, the full menu
 *     round (GetLayout's two out-args walked to the leaves, the
 *     property filter, GetGroupProperties, GetProperty, the
 *     AboutToShow dirty handshake, Event(clicked)), the New* /
 *     PropertiesChanged signal receipts, and the live-update
 *     LayoutUpdated flow.
 *
 * Skips loudly (exit 0) without a session bus, without a display
 * backend, or when a REAL watcher already owns the bus name (a live
 * desktop tray — this test must never fight, or queue behind, the
 * user's own watcher); FDK_DBUS_TEST_REQUIRE=1 makes any skip a
 * failure (CI sets it — CI's dbus-run-session bus is always private
 * and empty).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_tray.h"

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

/* Forward declarations (defined later in this file). */
void tray_getall_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                       void *user);
void abouttoshow_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                       void *user);
extern int g_ats_dirty;

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

/* ---- the mock watcher + host's state ------------------------------------- */

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"

static struct {
    char registered_service[64]; /* what RegisterStatusNotifierItem said */
    int registrations;
    int ready;
    int reqname_done;
    /* Host-side receipts. */
    int got_getall;
    char title[64];
    char icon_name[64];
    char status[32];
    int pixmap_ok;      /* 2x2, the four ARGB bytes seen flat        */
    int tooltip_ok;     /* 5-field shape, title "T" at fields 3/4    */
    int menu_ok;        /* /MenuBar path in the Menu property        */
    int activate_count;
    int32_t act_x, act_y;
    int secondary_count;
    int scroll_count;
    int32_t scroll_dx, scroll_dy;
    /* Signal receipts (the host subscribes). */
    int new_title_count;
    int new_icon_count;
    int new_status_count;
    int props_changed_count;
    int layout_updated_count;
    uint32_t layout_revision;
} g_mock;

static fdk_dbus_subscription *g_sig_subs[8];
static size_t g_sig_sub_n;

/* ---- the mock's method handler -------------------------------------------- */

static void reply_simple(fdk_dbus_connection *bus,
                         const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        send_or_free(bus, rep);
    }
}

static void serve_register(fdk_dbus_connection *bus,
                           const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *svc = NULL;
    if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 1 &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
        svc = fdk_dbus_value_string(args[0]);
    }
    if (svc == NULL) {
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs", NULL);
        if (err != NULL) {
            send_or_free(bus, err);
        }
        return;
    }
    snprintf(g_mock.registered_service,
             sizeof(g_mock.registered_service), "%s", svc);
    g_mock.registrations++;
    fdk_dbus_values_free_array(args, n);
    reply_simple(bus, call);
    /* The watcher's signal: the host's cue to fetch the item. The
     * registered name is read from the COPY — `svc` itself died
     * with the args array above (the borrowed-string rule). */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        WATCHER_PATH, WATCHER_NAME, "StatusNotifierItemRegistered");
    if (sig != NULL &&
        fdk_ok(fdk_dbus_message_append_string(
            sig, g_mock.registered_service))) {
        send_or_free(bus, sig);
    } else if (sig != NULL) {
        fdk_dbus_message_free(sig);
    }
}

static void serve_watcher_properties(fdk_dbus_connection *bus,
                                     const fdk_dbus_message *call) {
    /* GetAll on the watcher: RegisteredStatusNotifierItems (as of the
     * last registration), IsStatusNotifierHostRegistered (true),
     * ProtocolVersion (0), IconTheme ("default"). */
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        fdk_dbus_message_free(rep);
        return;
    }
    fdk_dbus_value *entries[4];
    size_t n = 0;
    struct {
        const char *key;
        fdk_dbus_type t;
        const char *s;
        int32_t i;
    } rows[] = {
        {"IsStatusNotifierHostRegistered", FDK_DBUS_BOOLEAN, NULL, 1},
        {"ProtocolVersion", FDK_DBUS_INT32, NULL, 0},
        {"IconTheme", FDK_DBUS_STRING, "default", 0},
    };
    fdk_dbus_value *reg_again = fdk_dbus_value_new_string(
        g_mock.registered_service[0] != '\0'
            ? g_mock.registered_service
            : "");
    fdk_dbus_value *var_reg =
        reg_again != NULL ? fdk_dbus_value_new_variant(reg_again) : NULL;
    fdk_dbus_value_free(reg_again);
    fdk_dbus_value *k_reg = var_reg != NULL
                                ? fdk_dbus_value_new_string(
                                      "RegisteredStatusNotifierItems")
                                : NULL;
    if (k_reg != NULL) {
        fdk_dbus_value *e =
            fdk_dbus_value_new_dict_entry(k_reg, var_reg);
        if (e != NULL) {
            entries[n++] = e;
        } else {
            fdk_dbus_value_free(k_reg);
            fdk_dbus_value_free(var_reg);
        }
    } else if (var_reg != NULL) {
        fdk_dbus_value_free(var_reg);
    }
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        fdk_dbus_value *inner =
            rows[r].t == FDK_DBUS_STRING
                ? fdk_dbus_value_new_string(rows[r].s)
                : (rows[r].t == FDK_DBUS_INT32
                       ? fdk_dbus_value_new_int32(rows[r].i)
                       : fdk_dbus_value_new_boolean(rows[r].i));
        fdk_dbus_value *var =
            inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
        fdk_dbus_value_free(inner);
        fdk_dbus_value *k =
            var != NULL ? fdk_dbus_value_new_string(rows[r].key) : NULL;
        if (k == NULL || var == NULL) {
            fdk_dbus_value_free(k);
            fdk_dbus_value_free(var);
            continue;
        }
        fdk_dbus_value *e = fdk_dbus_value_new_dict_entry(k, var);
        if (e == NULL) {
            fdk_dbus_value_free(k);
            fdk_dbus_value_free(var);
            continue;
        }
        entries[n++] = e;
    }
    fdk_dbus_value *dict = fdk_dbus_value_new_array("{sv}", entries, n);
    for (size_t i = 0; i < n; i++) {
        fdk_dbus_value_free(entries[i]);
    }
    if (dict != NULL && fdk_ok(fdk_dbus_message_append(rep, dict))) {
        send_or_free(bus, rep);
    } else {
        fdk_dbus_message_free(rep);
    }
    fdk_dbus_value_free(dict);
}

static void mock_method_handler(fdk_dbus_connection *c,
                                fdk_dbus_message *call, void *user) {
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface == NULL || member == NULL) {
        return;
    }
    if (strcmp(iface, WATCHER_NAME) == 0) {
        if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
            serve_register(c, call);
            return;
        }
        if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
            reply_simple(c, call);
            return;
        }
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.UnknownMethod", NULL);
        if (err != NULL) {
            send_or_free(c, err);
        }
        return;
    }
    if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        if (strcmp(member, "GetAll") == 0 &&
            strcmp(sstr(fdk_dbus_message_path(call)), WATCHER_PATH) == 0) {
            serve_watcher_properties(c, call);
            return;
        }
        reply_simple(c, call);
        return;
    }
}

static void mock_ready(fdk_dbus_connection *c, int ok, const char *err,
                       void *user) {
    (void)c;
    (void)err;
    int *flag = user;
    if (ok) {
        *flag = 1;
    }
}

static void generic_done(fdk_dbus_connection *c, fdk_dbus_message *reply,
                         void *user) {
    (void)c;
    (void)reply;
    int *flag = user;
    *flag = 1;
}

/* The live-test skip discipline (test_dbus_live.c's posture): exit 0
 * with a loud reason unless FDK_DBUS_TEST_REQUIRE turns it into a
 * failure. Returns the exit code main() should return. */
static int live_skip(const char *why) {
    printf("[skip] %s\n", why);
    if (getenv("FDK_DBUS_TEST_REQUIRE") != NULL) {
        fprintf(stderr, "FAIL: FDK_DBUS_TEST_REQUIRE set but %s\n", why);
        return 1;
    }
    return 0;
}

/* RequestName's reply code: 1 = primary owner, 2 = queued,
 * 3 = exists (another owner), 4 = already ours. Only 1 puts the mock
 * in the watcher seat — captured so a real desktop's tray can never
 * be fought or queued behind. */
static uint32_t g_reqname_code = 0;

static void reqname_code_done(fdk_dbus_connection *c,
                              fdk_dbus_message *reply, void *user) {
    (void)c;
    int *flag = user;
    *flag = 1;
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (reply != NULL &&
        fdk_ok(fdk_dbus_message_read_args(reply, &args, &n)) && n >= 1) {
        g_reqname_code = fdk_dbus_value_uint32(args[0]);
    }
    fdk_dbus_values_free_array(args, n);
}

/* ---- the mock host's client-side calls ------------------------------------- */

/* The item's unique bus name, once registered. */
static char g_item_bus[64];

static int g_getall_done;
static int g_call_done;

/* Call f and wait for the reply flag. */
static int call_and_wait(fdk_context *ctx, fdk_dbus_connection *bus,
                         fdk_dbus_message *call, int *done) {
    *done = 0;
    if (fdk_dbus_connection_call(bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 generic_done, done) == NULL) {
        fdk_dbus_message_free(call);
        return 0;
    }
    return pump_until(ctx, done, 5000);
}

/* GetAll(org.kde.StatusNotifierItem) on the item — the host's load. */
static int host_getall(fdk_context *ctx, fdk_dbus_connection *bus) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        g_item_bus, "/StatusNotifierItem", "org.freedesktop.DBus.Properties",
        "GetAll");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call,
                                               "org.kde.StatusNotifierItem"))) {
        fdk_dbus_message_free(call);
        return 0;
    }
    /* generic_done flips the flag; the VALIDATION happens in the
     * reply handler below — route through a dedicated one. */
    g_getall_done = 0;
    if (fdk_dbus_connection_call(bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 tray_getall_reply, NULL) == NULL) {
        fdk_dbus_message_free(call);
        return 0;
    }
    return pump_until(ctx, &g_getall_done, 5000);
}

/* ---- the callbacks (the app under test) ------------------------------------ */

static struct {
    int host_count;
    int hosted_now;
    int activate_count;
    int secondary;
    int32_t x, y;
    int32_t first_x, first_y;
    int scroll_count;
    int32_t dx, dy;
    uint32_t clicked_ids[8];
    int click_count;
    int show_count;
    uint32_t shown_parents[8];
} g_app;

static void app_on_host(fdk_tray_item *item, int hosted, void *user) {
    (void)item;
    (void)user;
    g_app.host_count++;
    g_app.hosted_now = hosted;
}

static void app_on_activate(fdk_tray_item *item, int secondary, int32_t x,
                            int32_t y, void *user) {
    (void)item;
    (void)user;
    g_app.activate_count++;
    if (g_app.activate_count == 1) {
        g_app.first_x = x; /* the plain Activate's coordinates       */
        g_app.first_y = y;
    }
    g_app.secondary = secondary;
    g_app.x = x;
    g_app.y = y;
}

static void app_on_scroll(fdk_tray_item *item, int32_t dx, int32_t dy,
                          void *user) {
    (void)item;
    (void)user;
    g_app.scroll_count++;
    g_app.dx = dx;
    g_app.dy = dy;
}

static void app_menu_click(fdk_tray_menu *menu, uint32_t id, void *user) {
    (void)menu;
    (void)user;
    if (g_app.click_count < 8) {
        g_app.clicked_ids[g_app.click_count] = id;
    }
    g_app.click_count++;
}

static void app_menu_show(fdk_tray_menu *menu, uint32_t parent, void *user) {
    (void)menu;
    (void)user;
    if (g_app.show_count < 8) {
        g_app.shown_parents[g_app.show_count] = parent;
    }
    g_app.show_count++;
}

/* ---- the GetAll reply validation (the host's property walk) ---------------- */

void tray_getall_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                       void *user) {
    (void)c;
    (void)user;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        g_getall_done = 1;
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(reply, &args, &n)) || n != 1) {
        fdk_dbus_values_free_array(args, n);
        g_getall_done = 1;
        return;
    }
    g_mock.got_getall = 1;
    const fdk_dbus_value *dict = args[0];
    const fdk_dbus_value *v;

    if ((v = fdk_dbus_value_dict_lookup(dict, "Title")) != NULL) {
        snprintf(g_mock.title, sizeof(g_mock.title), "%s",
                 sstr(fdk_dbus_value_string(fdk_dbus_value_variant_value(v))));
    }
    if ((v = fdk_dbus_value_dict_lookup(dict, "IconName")) != NULL) {
        snprintf(g_mock.icon_name, sizeof(g_mock.icon_name), "%s",
                 sstr(fdk_dbus_value_string(fdk_dbus_value_variant_value(v))));
    }
    if ((v = fdk_dbus_value_dict_lookup(dict, "Status")) != NULL) {
        snprintf(g_mock.status, sizeof(g_mock.status), "%s",
                 sstr(fdk_dbus_value_string(fdk_dbus_value_variant_value(v))));
    }
    if ((v = fdk_dbus_value_dict_lookup(dict, "IconPixmap")) != NULL) {
        const fdk_dbus_value *arr = fdk_dbus_value_variant_value(v);
        if (fdk_dbus_value_array_count(arr) == 1) {
            const fdk_dbus_value *one = fdk_dbus_value_array_item(arr, 0);
            if (fdk_dbus_value_struct_count(one) == 3) {
                int32_t w = fdk_dbus_value_int32(
                    fdk_dbus_value_struct_item(one, 0));
                int32_t h = fdk_dbus_value_int32(
                    fdk_dbus_value_struct_item(one, 1));
                const fdk_dbus_value *bytes =
                    fdk_dbus_value_struct_item(one, 2);
                size_t count = 0;
                const uint8_t *flat =
                    fdk_dbus_value_array_flat_bytes(bytes, &count);
                g_mock.pixmap_ok = w == 2 && h == 2 && flat != NULL &&
                                   count == 16 && flat[0] == 0x00 &&
                                   flat[1] == 0xFF && flat[2] == 0x00 &&
                                   flat[3] == 0xFF;
            }
        }
    }
    if ((v = fdk_dbus_value_dict_lookup(dict, "ToolTip")) != NULL) {
        const fdk_dbus_value *tt = fdk_dbus_value_variant_value(v);
        if (fdk_dbus_value_struct_count(tt) == 5) {
            const fdk_dbus_value *t3 = fdk_dbus_value_struct_item(tt, 3);
            const fdk_dbus_value *t4 = fdk_dbus_value_struct_item(tt, 4);
            g_mock.tooltip_ok =
                strcmp(sstr(fdk_dbus_value_string(t3)), "Tip title") == 0 &&
                strcmp(sstr(fdk_dbus_value_string(t4)), "Tip body") == 0;
        }
    }
    if ((v = fdk_dbus_value_dict_lookup(dict, "Menu")) != NULL) {
        g_mock.menu_ok = strcmp(sstr(fdk_dbus_value_string(
                                    fdk_dbus_value_variant_value(v))),
                                "/MenuBar") == 0;
    }
    fdk_dbus_values_free_array(args, n);
    g_getall_done = 1;
}

/* ---- the host's signal subscription receipts -------------------------------- */

static void host_signal(fdk_dbus_connection *c, fdk_dbus_message *sig,
                        void *user) {
    (void)c;
    (void)user;
    const char *member = fdk_dbus_message_member(sig);
    const char *iface = fdk_dbus_message_interface(sig);
    if (member == NULL) {
        return;
    }
    if (iface != NULL &&
        strcmp(iface, "com.canonical.dbusmenu") == 0 &&
        strcmp(member, "LayoutUpdated") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_ok(fdk_dbus_message_read_args(sig, &args, &n)) && n == 2) {
            g_mock.layout_updated_count++;
            g_mock.layout_revision = fdk_dbus_value_uint32(args[0]);
        }
        fdk_dbus_values_free_array(args, n);
        return;
    }
    if (iface != NULL &&
        strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
        strcmp(member, "PropertiesChanged") == 0) {
        g_mock.props_changed_count++;
        return;
    }
    if (strcmp(member, "NewTitle") == 0) {
        g_mock.new_title_count++;
    } else if (strcmp(member, "NewIcon") == 0) {
        g_mock.new_icon_count++;
    } else if (strcmp(member, "NewStatus") == 0) {
        g_mock.new_status_count++;
    }
}

static void subscribe_host_signals(fdk_dbus_connection *bus) {
    static const char *const rules[] = {
        "type='signal',interface='com.canonical.dbusmenu',member='LayoutUpdated'",
        "type='signal',interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',path='/StatusNotifierItem'",
        "type='signal',interface='org.kde.StatusNotifierItem',member='NewTitle'",
        "type='signal',interface='org.kde.StatusNotifierItem',member='NewIcon'",
        "type='signal',interface='org.kde.StatusNotifierItem',member='NewStatus'",
    };
    for (size_t i = 0;
         i < sizeof(rules) / sizeof(rules[0]) && g_sig_sub_n < 8; i++) {
        g_sig_subs[g_sig_sub_n++] =
            fdk_dbus_connection_subscribe(bus, rules[i], host_signal, NULL);
    }
}

/* ---- the menu tree the host fetched (GetLayout walk) ------------------------- */

static struct {
    int walked;
    int root_children;      /* 4 rows                                   */
    int labels_ok;         /* First / Second / Thi_rd / (separator)   */
    int check_ok;          /* Second checked                           */
    int radio_pair_ok;     /* two radios, exactly one on               */
    int submenu_ok;        /* First carries Sub + SubSub (2-deep)     */
    int disabled_ok;       /* the submenu's disabled row              */
    uint32_t first_id, second_id, third_id, sep_id, sub_id, subsub_id;
    uint32_t radio_a, radio_b, disabled_id;
    int radio_a_toggle, radio_b_toggle;
    uint32_t revision;
} g_menu_mock;

/* Walk one (ia{sv}av) node. */
static void walk_node(const fdk_dbus_value *node, int depth);

static void walk_children(const fdk_dbus_value *children, int depth) {
    size_t n = fdk_dbus_value_array_count(children);
    for (size_t i = 0; i < n; i++) {
        const fdk_dbus_value *wrap = fdk_dbus_value_array_item(children, i);
        const fdk_dbus_value *node =
            fdk_dbus_value_variant_value(wrap);
        if (node != NULL) {
            walk_node(node, depth + 1);
        }
    }
}

static const fdk_dbus_value *node_prop(const fdk_dbus_value *node,
                                       const char *key) {
    const fdk_dbus_value *props = fdk_dbus_value_struct_item(node, 1);
    if (props == NULL) {
        return NULL;
    }
    return fdk_dbus_value_dict_lookup(props, key);
}

static void walk_node(const fdk_dbus_value *node, int depth) {
    if (fdk_dbus_value_struct_count(node) != 3) {
        return;
    }
    uint32_t id = (uint32_t)fdk_dbus_value_int32(
        fdk_dbus_value_struct_item(node, 0));
    const fdk_dbus_value *type_v = node_prop(node, "type");
    const char *label = NULL;
    const fdk_dbus_value *label_v = node_prop(node, "label");
    if (label_v != NULL) {
        label = fdk_dbus_value_string(fdk_dbus_value_variant_value(label_v));
    }
    if (depth == 1) {
        g_menu_mock.root_children++;
        if (type_v != NULL &&
            strcmp(sstr(fdk_dbus_value_string(
                       fdk_dbus_value_variant_value(type_v))),
                   "separator") == 0) {
            g_menu_mock.sep_id = id;
        } else if (label != NULL && strcmp(label, "First") == 0) {
            g_menu_mock.first_id = id;
        } else if (label != NULL && strcmp(label, "Second") == 0) {
            g_menu_mock.second_id = id;
            const fdk_dbus_value *ts = node_prop(node, "toggle-state");
            g_menu_mock.check_ok =
                ts != NULL &&
                fdk_dbus_value_int32(fdk_dbus_value_variant_value(ts)) == 1;
        } else if (label != NULL && strcmp(label, "Thi_rd") == 0) {
            g_menu_mock.third_id = id;
        } else if (label != NULL && strcmp(label, "Radio A") == 0) {
            g_menu_mock.radio_a = id;
            const fdk_dbus_value *ts = node_prop(node, "toggle-state");
            g_menu_mock.radio_a_toggle =
                ts != NULL
                    ? fdk_dbus_value_int32(
                          fdk_dbus_value_variant_value(ts))
                    : -1;
        } else if (label != NULL && strcmp(label, "Radio B") == 0) {
            g_menu_mock.radio_b = id;
            const fdk_dbus_value *ts = node_prop(node, "toggle-state");
            g_menu_mock.radio_b_toggle =
                ts != NULL
                    ? fdk_dbus_value_int32(
                          fdk_dbus_value_variant_value(ts))
                    : -1;
        }
    } else if (depth == 2) {
        if (label != NULL && strcmp(label, "Sub") == 0) {
            g_menu_mock.sub_id = id;
            const fdk_dbus_value *cd = node_prop(node, "children-display");
            g_menu_mock.submenu_ok =
                cd != NULL &&
                strcmp(sstr(fdk_dbus_value_string(
                           fdk_dbus_value_variant_value(cd))),
                       "submenu") == 0;
        }
    } else if (depth == 3) {
        if (label != NULL && strcmp(label, "SubSub") == 0) {
            g_menu_mock.subsub_id = id;
        } else if (label != NULL && strcmp(label, "Disabled row") == 0) {
            g_menu_mock.disabled_id = id;
            const fdk_dbus_value *en = node_prop(node, "enabled");
            g_menu_mock.disabled_ok =
                en != NULL &&
                fdk_dbus_value_boolean(fdk_dbus_value_variant_value(en)) ==
                    0;
        }
    }
    const fdk_dbus_value *children = fdk_dbus_value_struct_item(node, 2);
    if (children != NULL && fdk_dbus_value_array_count(children) > 0) {
        walk_children(children, depth);
    }
}

static int g_getlayout_done;

static void getlayout_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                           void *user) {
    (void)c;
    (void)user;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        g_getlayout_done = 1;
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(reply, &args, &n)) || n != 2) {
        fdk_dbus_values_free_array(args, n);
        g_getlayout_done = 1;
        return;
    }
    g_menu_mock.revision = fdk_dbus_value_uint32(args[0]);
    g_menu_mock.walked = 1;
    walk_node(args[1], 0);
    fdk_dbus_values_free_array(args, n);
    g_getlayout_done = 1;
}

static int host_getlayout(fdk_context *ctx, fdk_dbus_connection *bus) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        g_item_bus, "/MenuBar", "com.canonical.dbusmenu", "GetLayout");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, 0)) ||
        !fdk_ok(fdk_dbus_message_append_int32(call, -1))) {
        fdk_dbus_message_free(call);
        return 0;
    }
    fdk_dbus_value *names = fdk_dbus_value_new_array("s", NULL, 0);
    if (names == NULL ||
        !fdk_ok(fdk_dbus_message_append(call, names))) {
        fdk_dbus_value_free(names);
        fdk_dbus_message_free(call);
        return 0;
    }
    fdk_dbus_value_free(names);
    g_getlayout_done = 0;
    if (fdk_dbus_connection_call(bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 getlayout_reply, NULL) == NULL) {
        fdk_dbus_message_free(call);
        return 0;
    }
    return pump_until(ctx, &g_getlayout_done, 5000);
}

/* Event(clicked) with NO_REPLY_EXPECTED — fire and pump. */
static void host_event(fdk_context *ctx, fdk_dbus_connection *bus,
                       uint32_t id, const char *event_id) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        g_item_bus, "/MenuBar", "com.canonical.dbusmenu", "Event");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, id)) ||
        !fdk_ok(fdk_dbus_message_append_string(call, event_id))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_message_set_flags(call, FDK_DBUS_FLAG_NO_REPLY_EXPECTED);
    if (!fdk_ok(fdk_dbus_message_append_variant_string(call, "")) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, 0))) {
        fdk_dbus_message_free(call);
        return;
    }
    if (!fdk_ok(fdk_dbus_connection_send(bus, call))) {
        fdk_dbus_message_free(call);
    }
    (void)fdk_pump_events(ctx, 50);
}

/* ---- main -------------------------------------------------------------------- */

int main(void) {
    /* Skip discipline (test_dbus_live.c's posture, kept identical):
     * no display backend → skip; no session bus → skip. A user's real
     * desktop has both, but an SSH session, a service context, or a
     * bare build VM may not — those must never see an abort. */
    const char *display = getenv("DISPLAY");
    const char *wl_display = getenv("WAYLAND_DISPLAY");
    if ((display == NULL || display[0] == '\0') &&
        (wl_display == NULL || wl_display[0] == '\0')) {
        return live_skip("no DISPLAY / WAYLAND_DISPLAY — the tray item "
                         "needs a live fdk_context (run: make test-dbus)");
    }
    const char *bus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus_addr == NULL || bus_addr[0] == '\0') {
        return live_skip("no DBUS_SESSION_BUS_ADDRESS in the environment");
    }

    fdk_context *ctx = NULL;
    if (fdk_init(&ctx, NULL) != FDK_OK) {
        /* DISPLAY set but dead (stale SSH forward, crashed Xvfb): a
         * skip, not an abort — the environment, not the code, failed. */
        return live_skip("fdk_init found no usable display backend — "
                         "stale DISPLAY? (run: make test-dbus)");
    }

    /* The mock watcher/host first. */
    fdk_dbus_connection *mock =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, mock_ready, &g_mock.ready);
    if (mock == NULL) {
        fdk_shutdown(ctx);
        return live_skip("session bus address unreachable (stale "
                         "DBUS_SESSION_BUS_ADDRESS?) — run: make test-dbus");
    }
    if (!pump_until(ctx, &g_mock.ready, 15000)) {
        fdk_dbus_connection_close(mock);
        fdk_shutdown(ctx);
        return live_skip("session bus never greeted us (stale address?)");
    }
    fdk_dbus_connection_set_method_handler(mock, mock_method_handler, NULL);
    {
        g_mock.reqname_done = 0;
        g_reqname_code = 0;
        /* DO_NOT_QUEUE is the safety rail: on a user's real desktop a
         * REAL watcher may own this name — the mock must lose the
         * race CLEANLY, not queue up to steal the name when the real
         * watcher later exits. */
        fdk_dbus_pending *p = fdk_dbus_connection_request_name(
            mock, WATCHER_NAME,
            FDK_DBUS_NAME_ALLOW_REPLACEMENT | FDK_DBUS_NAME_DO_NOT_QUEUE,
            reqname_code_done, &g_mock.reqname_done);
        assert(p != NULL);
        assert(pump_until(ctx, &g_mock.reqname_done, 5000));
        if (g_reqname_code != 1) {
            fdk_dbus_connection_close(mock);
            fdk_shutdown(ctx);
            return live_skip(
                "a real StatusNotifierWatcher owns this bus (a live tray) "
                "— run under a private bus: make test-dbus");
        }
    }
    subscribe_host_signals(mock);

    /* 1. open the item with the full state pre-set: it registers and
     * reports hosted. */
    fdk_tray_item_options opts = {
        .id = "fdk-tray-test",
        .title = "Original",
        .category = "Hardware",
    };
    fdk_tray_item_callbacks cb = {
        .on_host = app_on_host,
        .on_activate = app_on_activate,
        .on_scroll = app_on_scroll,
        .user = NULL,
    };
    fdk_tray_item *item = fdk_tray_item_open(ctx, &opts, &cb, NULL);
    assert(item != NULL);
    {
        int hosted = 0;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!hosted) {
            (void)fdk_pump_events(ctx, 20);
            if (fdk_tray_item_hosted(item)) {
                hosted = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                           (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (ms > 15000) {
                break;
            }
        }
        assert(hosted);
        assert(g_app.host_count == 1 && g_app.hosted_now == 1);
        assert(g_mock.registrations == 1);
        /* The registration argument is the item's unique bus name. */
        snprintf(g_item_bus, sizeof(g_item_bus), "%s",
                 g_mock.registered_service);
        assert(g_item_bus[0] == ':');
        printf("[ok] registration with the watcher (unique name, "
               "on_host(1))\n");
    }

    /* 2. state BEFORE the host's GetAll — icons, tooltip, status. */
    static const uint8_t px[16] = {
        /* 2x2 ARGB32 network order: opaque green-ish, 4 px * 4 B. */
        0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
        0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
    };
    fdk_tray_item_set_icon_name(item, "test-tray-icon");
    fdk_tray_item_set_icon_pixmap(item, 2, 2, px, sizeof(px));
    fdk_tray_item_set_tooltip(item, "tip-icon", "Tip title", "Tip body");
    fdk_tray_item_set_status(item, FDK_TRAY_STATUS_NEEDS_ATTENTION);

    /* 3. the host's load: GetAll — every property validated. */
    assert(host_getall(ctx, mock));
    assert(g_mock.got_getall);
    assert(strcmp(g_mock.title, "Original") == 0);
    assert(strcmp(g_mock.icon_name, "test-tray-icon") == 0);
    assert(strcmp(g_mock.status, "NeedsAttention") == 0);
    assert(g_mock.pixmap_ok);
    assert(g_mock.tooltip_ok);
    printf("[ok] GetAll: every property shape-checked (pixmap bytes "
           "flat-read, tooltip 5-field)\n");

    /* 4. the menu: build the full tree, attach, host fetches. */
    fdk_tray_menu *menu = fdk_tray_menu_create();
    assert(menu != NULL);
    uint32_t first = fdk_tray_menu_add(menu, 0, "First");
    uint32_t second = fdk_tray_menu_add(menu, 0, "Second");
    uint32_t sep = fdk_tray_menu_add_separator(menu, 0);
    uint32_t third = fdk_tray_menu_add(menu, 0, "Thi_rd");
    uint32_t ra = fdk_tray_menu_add(menu, 0, "Radio A");
    uint32_t rb = fdk_tray_menu_add(menu, 0, "Radio B");
    assert(first && second && sep && third && ra && rb);
    uint32_t sub = fdk_tray_menu_add(menu, first, "Sub");
    uint32_t disabled = fdk_tray_menu_add(menu, sub, "Disabled row");
    uint32_t subsub = fdk_tray_menu_add(menu, sub, "SubSub");
    assert(sub && disabled && subsub);
    fdk_tray_menu_set_check(menu, second, FDK_TRAY_MENU_CHECK_CHECK, 1);
    fdk_tray_menu_set_check(menu, ra, FDK_TRAY_MENU_CHECK_RADIO, 1);
    fdk_tray_menu_set_check(menu, rb, FDK_TRAY_MENU_CHECK_RADIO, 0);
    fdk_tray_menu_set_enabled(menu, disabled, 0);
    fdk_tray_menu_on_click(menu, app_menu_click, NULL);
    fdk_tray_menu_on_about_to_show(menu, app_menu_show, NULL);

    fdk_tray_item_set_menu(item, menu);
    assert(host_getlayout(ctx, mock));
    assert(g_menu_mock.walked);
    assert(g_menu_mock.root_children == 6); /* 4 rows + sep + 2 radios */
    assert(g_menu_mock.first_id == first && g_menu_mock.second_id == second &&
           g_menu_mock.third_id == third && g_menu_mock.sep_id == sep &&
           g_menu_mock.sub_id == sub && g_menu_mock.subsub_id == subsub);
    assert(g_menu_mock.check_ok);
    assert(g_menu_mock.submenu_ok);
    assert(g_menu_mock.disabled_ok);
    /* Radio semantics: exactly one of the pair carries toggle 1. */
    assert(g_menu_mock.radio_a_toggle + g_menu_mock.radio_b_toggle == 1);
    printf("[ok] GetLayout: the full tree walked — labels, mnemonic, "
           "separator, check, radio pair, 2-deep submenu, disabled\n");

    /* 5. Event(clicked) arrives as the app callback. */
    host_event(ctx, mock, second, "clicked");
    {
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_app.click_count >= 1;
        }
        assert(g_app.clicked_ids[0] == second);
        printf("[ok] Event(clicked) -> the app's menu callback\n");
    }

    /* 6. AboutToShow + the live update: a set_label bumps the
     * revision, LayoutUpdated arrives, the next AboutToShow flips. */
    {
        int before = g_mock.layout_updated_count;
        fdk_tray_menu_set_label(menu, third, "Third!");
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_mock.layout_updated_count > before;
        }
        assert(g_mock.layout_revision == fdk_tray_menu_revision(menu));

        /* The host's AboutToShow(0): dirty true (the revision moved). */
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            g_item_bus, "/MenuBar", "com.canonical.dbusmenu",
            "AboutToShow");
        assert(call != NULL &&
               fdk_ok(fdk_dbus_message_append_uint32(call, 0)));
        g_call_done = 0;
        g_ats_dirty = -1;
        assert(fdk_dbus_connection_call(mock, call,
                                         FDK_DBUS_TIMEOUT_DEFAULT,
                                         abouttoshow_reply, NULL) != NULL);
        assert(pump_until(ctx, &g_call_done, 5000));
        assert(g_ats_dirty == 1);
        /* And again: now clean (nothing moved since). */
        g_ats_dirty = -1;
        call = fdk_dbus_message_new_method_call(
            g_item_bus, "/MenuBar", "com.canonical.dbusmenu",
            "AboutToShow");
        assert(call != NULL &&
               fdk_ok(fdk_dbus_message_append_uint32(call, 0)));
        g_call_done = 0;
        assert(fdk_dbus_connection_call(mock, call,
                                         FDK_DBUS_TIMEOUT_DEFAULT,
                                         abouttoshow_reply, NULL) != NULL);
        assert(pump_until(ctx, &g_call_done, 5000));
        assert(g_ats_dirty == 0);
        assert(g_app.show_count >= 2); /* the show callbacks fired */
        printf("[ok] LayoutUpdated + the AboutToShow dirty handshake "
               "(dirty, then clean)\n");
    }

    /* 7. live property changes: the New* + PropertiesChanged receipts. */
    {
        fdk_tray_item_set_title(item, "Renamed");
        fdk_tray_item_set_icon_name(item, "other-icon");
        fdk_tray_item_set_status(item, FDK_TRAY_STATUS_ACTIVE);
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_mock.new_title_count >= 1 &&
                   g_mock.new_icon_count >= 1 &&
                   g_mock.new_status_count >= 1 &&
                   g_mock.props_changed_count >= 3;
        }
        printf("[ok] NewTitle/NewIcon/NewStatus + PropertiesChanged "
               "received by the host\n");
    }

    /* 8. Activate / SecondaryActivate / Scroll. */
    {
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            g_item_bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
            "Activate");
        assert(call != NULL && fdk_ok(fdk_dbus_message_append_int32(call, 7)) &&
               fdk_ok(fdk_dbus_message_append_int32(call, 9)));
        g_call_done = 0;
        assert(call_and_wait(ctx, mock, call, &g_call_done));
        call = fdk_dbus_message_new_method_call(
            g_item_bus, "/StatusNotifierItem",
            "org.kde.StatusNotifierItem", "SecondaryActivate");
        assert(call != NULL && fdk_ok(fdk_dbus_message_append_int32(call, 1)) &&
               fdk_ok(fdk_dbus_message_append_int32(call, 2)));
        g_call_done = 0;
        assert(call_and_wait(ctx, mock, call, &g_call_done));
        call = fdk_dbus_message_new_method_call(
            g_item_bus, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
            "Scroll");
        assert(call != NULL &&
               fdk_ok(fdk_dbus_message_append_int32(call, 5)) &&
               fdk_ok(fdk_dbus_message_append_string(call, "horizontal")));
        g_call_done = 0;
        assert(call_and_wait(ctx, mock, call, &g_call_done));
        int done = 0;
        while (!done) {
            (void)fdk_pump_events(ctx, 20);
            done = g_app.activate_count >= 2 && g_app.scroll_count >= 1;
        }
        assert(g_app.activate_count == 2);
        assert(g_app.secondary == 1);
        assert(g_app.first_x == 7 && g_app.first_y == 9);
        assert(g_app.x == 1 && g_app.y == 2);
        assert(g_app.scroll_count == 1 && g_app.dx == 5 && g_app.dy == 0);
        printf("[ok] Activate / SecondaryActivate / Scroll delivered\n");
    }

    /* 9. the watcher restart: unhosted, then re-registered. */
    {
        int released = 0;
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ReleaseName");
        assert(call != NULL &&
               fdk_ok(fdk_dbus_message_append_string(call, WATCHER_NAME)));
        assert(call_and_wait(ctx, mock, call, &released));
        int unhosted = 0;
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!unhosted) {
            (void)fdk_pump_events(ctx, 20);
            if (!fdk_tray_item_hosted(item)) {
                unhosted = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                           (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (ms > 15000) {
                break;
            }
        }
        assert(unhosted && g_app.host_count == 2);

        g_mock.reqname_done = 0;
        g_reqname_code = 0;
        fdk_dbus_pending *p = fdk_dbus_connection_request_name(
            mock, WATCHER_NAME,
            FDK_DBUS_NAME_ALLOW_REPLACEMENT | FDK_DBUS_NAME_DO_NOT_QUEUE,
            reqname_code_done, &g_mock.reqname_done);
        assert(p != NULL);
        assert(pump_until(ctx, &g_mock.reqname_done, 5000));
        assert(g_reqname_code == 1); /* we just released it: ours again */
        int rehosted = 0;
        clock_gettime(CLOCK_MONOTONIC, &start);
        while (!rehosted) {
            (void)fdk_pump_events(ctx, 20);
            if (fdk_tray_item_hosted(item)) {
                rehosted = 1;
            }
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long ms = (now.tv_sec - start.tv_sec) * 1000LL +
                           (now.tv_nsec - start.tv_nsec) / 1000000LL;
            if (ms > 15000) {
                break;
            }
        }
        assert(rehosted && g_app.host_count == 3 &&
               g_mock.registrations == 2);
        printf("[ok] watcher vanish -> unhosted; watcher return -> "
               "re-registered\n");
    }

    /* 10. teardown (menu first, then the item). */
    fdk_tray_menu_close(menu);
    fdk_tray_item_close(item);
    for (size_t i = 0; i < g_sig_sub_n; i++) {
        fdk_dbus_subscription_free(g_sig_subs[i]);
    }
    fdk_dbus_connection_close(mock);
    fdk_shutdown(ctx);
    printf("ALL PASS\n");
    return 0;
}

/* ---- the AboutToShow reply capture ------------------------------------------- */

int g_ats_dirty = -1;

void abouttoshow_reply(fdk_dbus_connection *c, fdk_dbus_message *reply,
                       void *user) {
    (void)c;
    (void)user;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_ok(fdk_dbus_message_read_args(reply, &args, &n)) && n == 1) {
            g_ats_dirty = fdk_dbus_value_boolean(args[0]) ? 1 : 0;
        }
        fdk_dbus_values_free_array(args, n);
    }
    g_call_done = 1;
}
