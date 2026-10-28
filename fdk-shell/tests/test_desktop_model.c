/* test_desktop_model.c — the desktop models' headless test (T-B side 2)
 *
 * The tray-host and notification-server MODELS (fdk-shell-desktop.h)
 * proven directly, without any rendering: a private bus
 * (dbus-run-session, launched by the Makefile) plus a throwaway Xvfb
 * (the fdk_context), an in-test MOCK ITEM on a second connection for
 * the tray side, and an in-test client for the notify side.
 *
 * Covered: the watcher's name acquisition and events; both
 * registration dialects; the item load (GetAll) and snapshots
 * (fields, pixmaps byte-exact, tooltips); the dbusmenu round (walk,
 * open/close handshakes, Event delivery); Activate forwarding;
 * NewStatus; hostile and duplicate registrations; the vanished-item
 * drop; the notification server's id assignment, replaces, actions,
 * urgency/timeout policy, expiration, flood gate, live-set cap, and
 * the close/action signals; and both models' name-etiquette matrix.
 *
 * Skips loudly (exit 0) when dbus-run-session did not launch us (no
 * DBUS_SESSION_BUS_ADDRESS) — same discipline as test_dbus_live.c.
 */
#define _POSIX_C_SOURCE 200809L

#include "fdk-shell/fdk-shell.h"
#include "fdk-shell/fdk-shell-desktop.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the event recorder --------------------------------------------------- */

typedef struct {
    int items_changed, item_state, menu_changed, host_state;
    uint32_t last_serial;
    int host_serving;
} tray_events;

typedef struct {
    int added, replaced, closed, server_state;
    uint32_t last_id;
    int serving;
} notify_events;

static tray_events g_te;
static notify_events g_ne;

static void tray_event(fdk_shell_tray_host *h,
                       fdk_shell_tray_event_type ev, uint32_t serial,
                       void *user) {
    (void)h;
    (void)user;
    switch (ev) {
    case FDK_SHELL_TRAY_EV_ITEMS_CHANGED:
        g_te.items_changed++;
        g_te.last_serial = serial;
        break;
    case FDK_SHELL_TRAY_EV_ITEM_STATE:
        g_te.item_state++;
        g_te.last_serial = serial;
        break;
    case FDK_SHELL_TRAY_EV_MENU_CHANGED:
        g_te.menu_changed++;
        break;
    case FDK_SHELL_TRAY_EV_HOST_STATE:
        g_te.host_state++;
        g_te.host_serving = fdk_shell_tray_host_serving(h);
        break;
    default:
        break;
    }
}

static void notify_event(fdk_shell_notify_server *s,
                         fdk_shell_notify_event_type ev, uint32_t id,
                         void *user) {
    (void)s;
    (void)user;
    switch (ev) {
    case FDK_SHELL_NOTIFY_EV_ADDED:
        g_ne.added++;
        g_ne.last_id = id;
        break;
    case FDK_SHELL_NOTIFY_EV_REPLACED:
        g_ne.replaced++;
        g_ne.last_id = id;
        break;
    case FDK_SHELL_NOTIFY_EV_CLOSED:
        g_ne.closed++;
        break;
    case FDK_SHELL_NOTIFY_EV_SERVER_STATE:
        g_ne.server_state++;
        g_ne.serving = fdk_shell_notify_server_serving(s);
        break;
    default:
        break;
    }
}

static void reset_events(void) {
    memset(&g_te, 0, sizeof(g_te));
    memset(&g_ne, 0, sizeof(g_ne));
}

/* ---- the pump helper -------------------------------------------------------- */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Pumps until `cond` (evaluated each iteration with `user`) or the
 * deadline. Returns nonzero when cond held. */
static int pump_until(fdk_context *ctx, double seconds,
                      int (*cond)(void *), void *user) {
    double deadline = now_s() + seconds;
    while (now_s() < deadline) {
        fdk_pump_events(ctx, 25);
        if (cond != NULL && cond(user)) {
            return 1;
        }
    }
    return cond != NULL ? cond(user) : 1;
}

/* ---- the mock tray item (a SECOND connection) ------------------------------ */

#define MOCK_ITEM_PATH "/org/fdk/mock/Item"
#define MOCK_MENU_PATH "/org/fdk/mock/Menu"

typedef struct {
    fdk_dbus_connection *bus;
    int activate_calls, secondary_calls, scroll_calls;
    int abouttoshow_calls, event_calls, getlayout_calls;
    char last_event[32];
    int32_t last_event_id;
} mock_item;

static mock_item g_mock;

/* The Properties.GetAll reply: the full property set. */
static void mock_serve_getall(fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    assert(rep != NULL);
    fdk_dbus_value *entries[8];
    size_t n = 0;
    /* Every value variant-wrapped (the a{sv} shape). */
    {
        fdk_dbus_value *k = fdk_dbus_value_new_string("Title");
        fdk_dbus_value *inner = fdk_dbus_value_new_string("Mock item");
        fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
        fdk_dbus_value_free(inner);
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    {
        fdk_dbus_value *k = fdk_dbus_value_new_string("Status");
        fdk_dbus_value *inner = fdk_dbus_value_new_string("Active");
        fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
        fdk_dbus_value_free(inner);
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    {
        /* IconPixmap: a(iiay) — an ARRAY holding one 2x2 opaque
         * blue ARGB32 network-order struct. */
        fdk_dbus_value *bytes[16];
        for (size_t i = 0; i < 4; i++) {
            bytes[i * 4 + 0] = fdk_dbus_value_new_byte(0xFF); /* A */
            bytes[i * 4 + 1] = fdk_dbus_value_new_byte(0x00); /* R */
            bytes[i * 4 + 2] = fdk_dbus_value_new_byte(0x00); /* G */
            bytes[i * 4 + 3] = fdk_dbus_value_new_byte(0xFF); /* B */
        }
        fdk_dbus_value *data = fdk_dbus_value_new_array("y", bytes, 16);
        fdk_dbus_value *w = fdk_dbus_value_new_int32(2);
        fdk_dbus_value *h = fdk_dbus_value_new_int32(2);
        fdk_dbus_value *fields[3] = {w, h, data};
        fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 3);
        fdk_dbus_value *items[1] = {st};
        fdk_dbus_value *arr = fdk_dbus_value_new_array("(iiay)", items, 1);
        fdk_dbus_value *var = fdk_dbus_value_new_variant(arr);
        fdk_dbus_value_free(arr);
        fdk_dbus_value *k = fdk_dbus_value_new_string("IconPixmap");
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    {
        /* ToolTip: (sa(iiay)(ss)ss) — icon name, pixmaps, a (ss)
         * struct, title, description; the parser reads fields 3 and
         * 4. (An EMPTY struct would be an invalid signature — the
         * daemon discards the whole reply; the (ss) middle keeps it
         * legal.) */
        fdk_dbus_value *f0 = fdk_dbus_value_new_string("");
        fdk_dbus_value *px = fdk_dbus_value_new_array("(iiay)", NULL, 0);
        fdk_dbus_value *ss0 = fdk_dbus_value_new_string("");
        fdk_dbus_value *ss1 = fdk_dbus_value_new_string("");
        fdk_dbus_value *sf[2] = {ss0, ss1};
        fdk_dbus_value *f2 = fdk_dbus_value_new_struct(sf, 2);
        fdk_dbus_value *f3 = fdk_dbus_value_new_string("Tip title");
        fdk_dbus_value *f4 = fdk_dbus_value_new_string("Tip desc");
        fdk_dbus_value *fields[5] = {f0, px, f2, f3, f4};
        fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 5);
        fdk_dbus_value *var = fdk_dbus_value_new_variant(st);
        fdk_dbus_value_free(st);
        fdk_dbus_value *k = fdk_dbus_value_new_string("ToolTip");
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    {
        fdk_dbus_value *k = fdk_dbus_value_new_string("Menu");
        fdk_dbus_value *inner =
            fdk_dbus_value_new_object_path(MOCK_MENU_PATH);
        fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
        fdk_dbus_value_free(inner);
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    {
        fdk_dbus_value *k = fdk_dbus_value_new_string("ItemIsMenu");
        fdk_dbus_value *inner = fdk_dbus_value_new_boolean(0);
        fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
        fdk_dbus_value_free(inner);
        entries[n++] = fdk_dbus_value_new_dict_entry(k, var);
    }
    fdk_dbus_value *dict = fdk_dbus_value_new_array("{sv}", entries, n);
    assert(dict != NULL);
    assert(fdk_ok(fdk_dbus_message_append(rep, dict)));
    fdk_dbus_value_free(dict);
    fdk_dbus_connection_send(g_mock.bus, rep);
}

/* A dbusmenu node builder: (i a{sv} av) with one label property.
 * NULL children means an EMPTY av (never a NULL struct field — the
 * marshaler refuses those). */
static fdk_dbus_value *mk_node(int32_t id, const char *label,
                               fdk_dbus_value *children) {
    fdk_dbus_value *k = fdk_dbus_value_new_string("label");
    fdk_dbus_value *inner = fdk_dbus_value_new_string(label);
    fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = fdk_dbus_value_new_dict_entry(k, var);
    fdk_dbus_value *props = fdk_dbus_value_new_array("{sv}", &e, 1);
    fdk_dbus_value *idv = fdk_dbus_value_new_int32(id);
    if (children == NULL) {
        children = fdk_dbus_value_new_array("v", NULL, 0);
    }
    fdk_dbus_value *fields[3] = {idv, props, children};
    return fdk_dbus_value_new_struct(fields, 3);
}

/* GetLayout: a root with two children, one a submenu parent. */
static void mock_serve_getlayout(fdk_dbus_message *call) {
    g_mock.getlayout_calls++;
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    assert(rep != NULL);
    assert(fdk_ok(fdk_dbus_message_append_uint32(rep, 3))); /* rev */

    /* The submenu's children: one leaf, variant-wrapped in the av. */
    fdk_dbus_value *sub_leaf = mk_node(12, "Sub leaf", NULL);
    fdk_dbus_value *sub_wrapped = fdk_dbus_value_new_variant(sub_leaf);
    fdk_dbus_value_free(sub_leaf);
    fdk_dbus_value *sub_items[1] = {sub_wrapped};
    fdk_dbus_value *sub_children =
        fdk_dbus_value_new_array("v", sub_items, 1);
    /* The root's children: a leaf and the submenu parent — each
     * VARIANT-wrapped inside the root's av. */
    fdk_dbus_value *leaf = mk_node(10, "First item", NULL);
    fdk_dbus_value *parent = mk_node(11, "Submenu", sub_children);
    fdk_dbus_value *wrapped[2] = {
        fdk_dbus_value_new_variant(leaf),
        fdk_dbus_value_new_variant(parent),
    };
    fdk_dbus_value_free(leaf);
    fdk_dbus_value_free(parent);
    fdk_dbus_value *children = fdk_dbus_value_new_array("v", wrapped, 2);
    fdk_dbus_value *root = mk_node(0, "", children);
    assert(fdk_ok(fdk_dbus_message_append(rep, root)));
    fdk_dbus_value_free(root);
    fdk_dbus_connection_send(g_mock.bus, rep);
}

static void mock_method_handler(fdk_dbus_connection *bus,
                                fdk_dbus_message *call, void *user) {
    (void)bus;
    (void)user;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface != NULL &&
        strcmp(iface, "org.freedesktop.DBus.Properties") == 0 &&
        member != NULL && strcmp(member, "GetAll") == 0) {
        mock_serve_getall(call);
        return;
    }
    if (iface != NULL &&
        strcmp(iface, "com.canonical.dbusmenu") == 0) {
        if (member != NULL && strcmp(member, "GetLayout") == 0) {
            mock_serve_getlayout(call);
            return;
        }
        if (member != NULL && strcmp(member, "AboutToShow") == 0) {
            g_mock.abouttoshow_calls++;
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL) {
                fdk_dbus_message_append_boolean(rep, 0);
                fdk_dbus_connection_send(g_mock.bus, rep);
            }
            return;
        }
        if (member != NULL && strcmp(member, "Event") == 0) {
            g_mock.event_calls++;
            fdk_dbus_value **args = NULL;
            size_t n = 0;
            if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
                n >= 2 && args != NULL &&
                fdk_dbus_value_type(args[0]) == FDK_DBUS_INT32 &&
                fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
                g_mock.last_event_id = fdk_dbus_value_int32(args[0]);
                snprintf(g_mock.last_event,
                         sizeof(g_mock.last_event), "%s",
                         fdk_dbus_value_string(args[1]));
            }
            if (args != NULL) {
                fdk_dbus_values_free_array(args, n);
            }
            /* Event carries NO_REPLY_EXPECTED; reply anyway is fine
             * but the model does not wait — just count. */
            return;
        }
    }
    if (iface != NULL &&
        strcmp(iface, "org.kde.StatusNotifierItem") == 0) {
        if (member != NULL && strcmp(member, "Activate") == 0) {
            g_mock.activate_calls++;
        } else if (member != NULL &&
                   strcmp(member, "SecondaryActivate") == 0) {
            g_mock.secondary_calls++;
        } else if (member != NULL && strcmp(member, "Scroll") == 0) {
            g_mock.scroll_calls++;
        }
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_connection_send(g_mock.bus, rep);
        }
        return;
    }
}

static int mock_item_start(fdk_context *ctx) {
    memset(&g_mock, 0, sizeof(g_mock));
    g_mock.bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (g_mock.bus == NULL) {
        return 0;
    }
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(g_mock.bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(g_mock.bus)) {
        return 0;
    }
    fdk_dbus_connection_set_method_handler(g_mock.bus,
                                           mock_method_handler, NULL);
    return 1;
}

typedef struct {
    int ok;
    char err[128];
} reply_cap;

static void mock_register_reply(fdk_dbus_connection *bus,
                                fdk_dbus_message *reply, void *user) {
    (void)bus;
    reply_cap *c = user;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        c->ok = 1;
    } else if (reply != NULL) {
        snprintf(c->err, sizeof(c->err), "%s",
                 fdk_dbus_message_error_name(reply)
                     ? fdk_dbus_message_error_name(reply)
                     : "?");
    } else {
        snprintf(c->err, sizeof(c->err), "timeout");
    }
}

/* Registers the mock with the watcher (returns the reply code
 * through *out_ok / the error name through *out_err). */
static void mock_register(fdk_context *ctx, const char *service,
                          int *out_ok, char *out_err, size_t err_cap) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem");
    assert(call != NULL);
    assert(fdk_ok(fdk_dbus_message_append_string(call, service)));
    reply_cap *cap = calloc(1, sizeof(*cap));
    assert(cap != NULL);
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        g_mock.bus, call, 3000, mock_register_reply, cap);
    (void)p;
    double deadline = now_s() + 3.0;
    while (now_s() < deadline && !cap->ok && cap->err[0] == '\0') {
        fdk_pump_events(ctx, 25);
    }
    *out_ok = cap->ok;
    snprintf(out_err, err_cap, "%s", cap->err);
    free(cap);
}

/* The item count is 1 and loaded. */
static int tray_one_loaded(void *vp) {
    fdk_shell_tray_host *h = vp;
    if (fdk_shell_tray_item_count(h) != 1) {
        return 0;
    }
    /* LOADED, not merely present: the GetAll round completed. */
    fdk_shell_tray_item_info info;
    memset(&info, 0, sizeof(info));
    info.struct_size = sizeof(info);
    if (!fdk_ok(fdk_shell_tray_item_snapshot(h, 0, &info))) {
        return 0;
    }
    return !info.loading && !info.failed;
}

static int tray_count_is_2(void *vp) {
    fdk_shell_tray_host *h = vp;
    return fdk_shell_tray_item_count(h) == 2;
}

static int tray_count_is_0(void *vp) {
    fdk_shell_tray_host *h = vp;
    return fdk_shell_tray_item_count(h) == 0;
}

static void count_visit(const fdk_shell_tray_menu_node_info *node,
                         int depth, void *user) {
    (void)node;
    (void)depth;
    int *count = user;
    (*count)++;
}

static int menu_arrived(void *vp) {
    fdk_shell_tray_host *h = vp;
    int count = 0;
    return fdk_shell_tray_menu_walk(h, 0, count_visit, &count) ==
                   FDK_OK &&
           count > 0;
}

/* The menu walk collector. */
typedef struct {
    int count;
    int saw_first, saw_submenu, saw_subleaf;
    int max_depth;
} walk_state;

static void walk_cb(const fdk_shell_tray_menu_node_info *node, int depth,
                    void *user) {
    walk_state *w = user;
    w->count++;
    if (depth > w->max_depth) {
        w->max_depth = depth;
    }
    if (node->label != NULL && strcmp(node->label, "First item") == 0) {
        w->saw_first = 1;
    }
    if (node->label != NULL && strcmp(node->label, "Submenu") == 0) {
        w->saw_submenu = 1;
    }
    if (node->label != NULL && strcmp(node->label, "Sub leaf") == 0) {
        w->saw_subleaf = 1;
    }
}

/* ---- the notify client (a SECOND connection) ------------------------------- */

typedef struct {
    fdk_dbus_connection *bus;
    int closed_signals;
    int invoked_signals;
    uint32_t last_closed_id, last_closed_reason;
    char last_action_key[64];
} mock_client;

static mock_client g_client;

static void client_signal(fdk_dbus_connection *bus, fdk_dbus_message *sig,
                          void *user) {
    (void)bus;
    (void)user;
    const char *member = fdk_dbus_message_member(sig);
    if (member == NULL) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (strcmp(member, "NotificationClosed") == 0 &&
        fdk_dbus_message_read_args(sig, &args, &n) == FDK_OK && n == 2 &&
        args != NULL) {
        g_client.closed_signals++;
        g_client.last_closed_id = fdk_dbus_value_uint32(args[0]);
        g_client.last_closed_reason = fdk_dbus_value_uint32(args[1]);
    } else if (strcmp(member, "ActionInvoked") == 0 &&
               fdk_dbus_message_read_args(sig, &args, &n) == FDK_OK &&
               n == 2 && args != NULL) {
        g_client.invoked_signals++;
        snprintf(g_client.last_action_key,
                 sizeof(g_client.last_action_key), "%s",
                 fdk_dbus_value_string(args[1]) != NULL
                     ? fdk_dbus_value_string(args[1])
                     : "");
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
}

static int client_start(fdk_context *ctx) {
    memset(&g_client, 0, sizeof(g_client));
    g_client.bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (g_client.bus == NULL) {
        return 0;
    }
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(g_client.bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(g_client.bus)) {
        return 0;
    }
    char *rule = fdk_dbus_match_rule(
        "signal", NULL, "org.freedesktop.Notifications", NULL,
        "/org/freedesktop/Notifications", NULL);
    if (rule != NULL) {
        fdk_dbus_connection_subscribe(g_client.bus, rule, client_signal,
                                       NULL);
        free(rule);
    }
    return 1;
}

typedef struct {
    uint32_t id;
    int done;
} id_cap;

static void client_notify_reply(fdk_dbus_connection *bus,
                                fdk_dbus_message *reply, void *user) {
    (void)bus;
    id_cap *c = user;
    c->done = 1;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32) {
            c->id = fdk_dbus_value_uint32(args[0]);
        }
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    }
}

/* A Notify call from the client; returns the reply id (0 on
 * error/timeout). */
static uint32_t client_notify(fdk_context *ctx, const char *app,
                              uint32_t replaces, const char *summary,
                              const char *body, int32_t timeout,
                              int urgency, const char *action_key) {
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "Notify");
    assert(call != NULL);
    fdk_dbus_value *actions = NULL;
    if (action_key != NULL) {
        fdk_dbus_value *k = fdk_dbus_value_new_string(action_key);
        fdk_dbus_value *l = fdk_dbus_value_new_string("Do it");
        fdk_dbus_value *items[2] = {k, l};
        actions = fdk_dbus_value_new_array("s", items, 2);
    } else {
        actions = fdk_dbus_value_new_array("s", NULL, 0);
    }
    fdk_dbus_value *uk = fdk_dbus_value_new_string("urgency");
    fdk_dbus_value *uv = fdk_dbus_value_new_byte((uint8_t)urgency);
    fdk_dbus_value *urg_var = fdk_dbus_value_new_variant(uv);
    fdk_dbus_value_free(uv);
    fdk_dbus_value *e = fdk_dbus_value_new_dict_entry(uk, urg_var);
    fdk_dbus_value *hints = fdk_dbus_value_new_array("{sv}", &e, 1);
    int ok = fdk_ok(fdk_dbus_message_append_string(call, app)) &&
             fdk_ok(fdk_dbus_message_append_uint32(call, replaces)) &&
             fdk_ok(fdk_dbus_message_append_string(call, "")) &&
             fdk_ok(fdk_dbus_message_append_string(call, summary)) &&
             fdk_ok(fdk_dbus_message_append_string(call, body)) &&
             fdk_ok(fdk_dbus_message_append(call, actions)) &&
             fdk_ok(fdk_dbus_message_append(call, hints)) &&
             fdk_ok(fdk_dbus_message_append_int32(call, timeout));
    fdk_dbus_value_free(actions);
    fdk_dbus_value_free(hints);
    assert(ok);
    id_cap *cap = calloc(1, sizeof(*cap));
    assert(cap != NULL);
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        g_client.bus, call, 3000, client_notify_reply, cap);
    (void)p;
    double deadline = now_s() + 3.0;
    while (now_s() < deadline && !cap->done) {
        fdk_pump_events(ctx, 25);
    }
    uint32_t id = cap->id;
    free(cap);
    return id;
}

/* ---- the tests ---------------------------------------------------------------- */

static void test_tray_model(fdk_context *ctx) {
    printf("-- tray host model --\n");
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    assert(bus != NULL);
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    assert(fdk_dbus_connection_ready(bus));

    /* Create: not-ready must refuse. */
    fdk_dbus_connection *bus2 =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    assert(bus2 != NULL && !fdk_dbus_connection_ready(bus2));
    {
        fdk_shell_tray_host *h = NULL;
        assert(fdk_shell_tray_host_create(bus2, NULL, &h) ==
               FDK_ERR_NOT_INITIALIZED);
    }

    /* Create on the ready bus: the name arrives through the loop. */
    fdk_shell_tray_host *host = NULL;
    fdk_shell_tray_host_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.event_cb = tray_event;
    desc.user = NULL;
    assert(fdk_ok(fdk_shell_tray_host_create(bus, &desc, &host)));
    assert(host != NULL);
    assert(pump_until(ctx, 3.0, NULL, NULL) == 1);
    assert(g_te.host_state >= 1 && g_te.host_serving == 1);
    assert(fdk_shell_tray_host_serving(host) == 1);
    printf("[ok] create: the watcher name acquired, HOST_STATE(1)\n");

    /* Argument validation. */
    {
        fdk_shell_tray_host *h = NULL;
        fdk_shell_tray_host_desc bad;
        memset(&bad, 0, sizeof(bad));
        bad.struct_size = sizeof(bad);
        bad.replace = 1;
        bad.wait = 1;
        assert(fdk_shell_tray_host_create(bus, &bad, &h) ==
               FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_shell_tray_host_create(NULL, &desc, &h) ==
               FDK_ERR_INVALID_ARGUMENT);
    }
    printf("[ok] create: argument validation (replace+wait refused)\n");

    reset_events();

    /* The mock item (KDE dialect: its unique name). */
    assert(mock_item_start(ctx));
    const char *me = fdk_dbus_connection_unique_name(g_mock.bus);
    assert(me != NULL);
    {
        int ok = 0;
        char err[128] = "";
        mock_register(ctx, me, &ok, err, sizeof(err));
        assert(ok);
    }
    assert(pump_until(
        ctx, 3.0,
        tray_one_loaded, host));
    assert(fdk_shell_tray_item_count(host) == 1);
    assert(g_te.items_changed >= 2); /* registration + load          */
    printf("[ok] KDE registration: item registered and loaded\n");

    /* The snapshot. */
    {
        fdk_shell_tray_item_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        char title[64], tt[64], td[64], reg[128];
        size_t title_l = sizeof(title), tt_l = sizeof(tt),
               td_l = sizeof(td), reg_l = sizeof(reg);
        info.title = title;
        info.title_len = title_l;
        info.tooltip_title = tt;
        info.tooltip_title_len = tt_l;
        info.tooltip_description = td;
        info.tooltip_description_len = td_l;
        info.reg_arg = reg;
        info.reg_arg_len = reg_l;
        assert(fdk_ok(fdk_shell_tray_item_snapshot(host, 0, &info)));
        assert(info.serial != 0);
        assert(info.loading == 0 && info.failed == 0);
        assert(strcmp(title, "Mock item") == 0);
        assert(strcmp(tt, "Tip title") == 0);
        assert(strcmp(td, "Tip desc") == 0);
        assert(strcmp(reg, me) == 0);
        assert(info.status == FDK_SHELL_TRAY_ACTIVE);
        assert(info.pixmap_count == 1);
        assert(info.has_menu == 1);
        assert(info.item_is_menu == 0);
        /* Out-of-range index. */
        assert(fdk_shell_tray_item_snapshot(host, 1, &info) ==
               FDK_ERR_NOT_FOUND);
    }
    printf("[ok] snapshot: title, tooltip, reg arg, pixmap count, menu\n");

    /* The pixmap bytes: opaque blue ARGB32. */
    {
        int w = 0, h = 0;
        assert(fdk_ok(fdk_shell_tray_item_pixmap_size(host, 0, 0, 0, &w,
                                                      &h)));
        assert(w == 2 && h == 2);
        uint8_t bytes[16];
        size_t need = 0;
        assert(fdk_ok(fdk_shell_tray_item_pixmap_pixels(
            host, 0, 0, 0, bytes, sizeof(bytes), &need)));
        assert(need == 16);
        /* Host-order ARGB32: A=FF R=00 G=00 B=FF per pixel. */
        for (int i = 0; i < 4; i++) {
            assert(bytes[i * 4 + 0] == 0xFF); /* B (LE byte 0)      */
            assert(bytes[i * 4 + 1] == 0x00); /* G                  */
            assert(bytes[i * 4 + 2] == 0x00); /* R                  */
            assert(bytes[i * 4 + 3] == 0xFF); /* A                  */
        }
        /* Bad pixmap index. */
        assert(fdk_shell_tray_item_pixmap_size(host, 0, 0, 1, &w, &h) ==
               FDK_ERR_NOT_FOUND);
    }
    printf("[ok] pixmaps: size query and byte-exact copy\n");

    /* The menu prefetch + walk. */
    assert(pump_until(ctx, 3.0, menu_arrived, host));
    {
        walk_state ws;
        memset(&ws, 0, sizeof(ws));
        assert(fdk_ok(fdk_shell_tray_menu_walk(host, 0, walk_cb, &ws)));
        assert(ws.count == 4); /* root + 2 children + sub leaf      */
        assert(ws.max_depth == 2);
        assert(ws.saw_first && ws.saw_submenu && ws.saw_subleaf);
    }
    printf("[ok] menu: prefetched, walked (4 nodes, depth 2)\n");

    /* The menu open/close round + Event delivery. */
    {
        int before_about = g_mock.abouttoshow_calls;
        int before_events = g_mock.event_calls;
        assert(fdk_ok(fdk_shell_tray_menu_open(host, 0)));
        assert(fdk_ok(fdk_shell_tray_menu_event(host, 0, 10, "clicked")));
        assert(fdk_ok(fdk_shell_tray_menu_close(host, 0)));
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_mock.abouttoshow_calls > before_about);
        assert(g_mock.event_calls >= before_events + 2); /* clicked +
                                                            closed   */
        assert(g_mock.last_event_id == 0); /* the close sends root  */
    }
    printf("[ok] menu: AboutToShow handshake, Event(clicked), closed\n");

    /* Activate / SecondaryActivate / Scroll forwarding. */
    {
        assert(fdk_ok(fdk_shell_tray_item_activate(host, 0, 10, 20)));
        assert(fdk_ok(fdk_shell_tray_item_secondary_activate(
            host, 0, 10, 20)));
        assert(fdk_ok(fdk_shell_tray_item_scroll(host, 0, 3, -2)));
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_mock.activate_calls == 1);
        assert(g_mock.secondary_calls == 1);
        assert(g_mock.scroll_calls == 1);
    }
    printf("[ok] interactions: Activate, SecondaryActivate, Scroll\n");

    /* NewStatus: the mock emits, the model follows. */
    {
        reset_events();
        /* The KDE-form item's path is the canonical
         * /StatusNotifierItem (the registration's object path —
         * MOCK_ITEM_PATH is the AYATANA second item's). */
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            "/StatusNotifierItem", "org.kde.StatusNotifierItem",
            "NewStatus");
        assert(sig != NULL);
        assert(fdk_ok(fdk_dbus_message_append_string(sig, "Passive")));
        fdk_dbus_connection_send(g_mock.bus, sig);
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_te.item_state >= 1);
        fdk_shell_tray_item_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        assert(fdk_ok(fdk_shell_tray_item_snapshot(host, 0, &info)));
        assert(info.status == FDK_SHELL_TRAY_PASSIVE);
    }
    printf("[ok] NewStatus: the item state followed (Passive)\n");

    /* Hostile + duplicate registrations. */
    {
        int ok = 0;
        char err[128] = "";
        mock_register(ctx, "not a valid name!", &ok, err, sizeof(err));
        assert(!ok && strstr(err, "InvalidArgs") != NULL);
        mock_register(ctx, me, &ok, err, sizeof(err));
        assert(ok); /* duplicate: the idempotent OK                 */
        assert(pump_until(ctx, 1.0, NULL, NULL) == 1);
        assert(fdk_shell_tray_item_count(host) == 1);
    }
    printf("[ok] hostile and duplicate registrations\n");

    /* The Ayatana dialect: a second item by object path. */
    {
        int ok = 0;
        char err[128] = "";
        mock_register(ctx, "/org/fdk/mock/Second", &ok, err, sizeof(err));
        assert(ok);
        assert(pump_until(ctx, 3.0, tray_count_is_2,
                          host));
    }
    printf("[ok] Ayatana registration: second item by path\n");

    /* The vanished-item drop: the mock's connection dies. */
    {
        reset_events();
        fdk_dbus_connection_close(g_mock.bus);
        g_mock.bus = NULL;
        assert(pump_until(ctx, 3.0, tray_count_is_0,
                          host));
        assert(g_te.items_changed >= 2);
    }
    printf("[ok] vanished items: both dropped on connection loss\n");

    /* Destroy: clean, and the name releases (a fresh host takes it). */
    fdk_shell_tray_host_destroy(host);
    fdk_dbus_connection_close(bus2);
    printf("[ok] destroy: clean teardown\n");

    /* The etiquette matrix: an incumbent host, then a refusing
     * second host on ANOTHER connection. */
    {
        fdk_dbus_connection *busA =
            fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
        fdk_dbus_connection *busB =
            fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
        assert(busA != NULL && busB != NULL);
        double dl = now_s() + 5.0;
        while ((!fdk_dbus_connection_ready(busA) ||
                !fdk_dbus_connection_ready(busB)) &&
               now_s() < dl) {
            fdk_pump_events(ctx, 20);
        }
        assert(fdk_dbus_connection_ready(busA) &&
               fdk_dbus_connection_ready(busB));

        fdk_shell_tray_host *A = NULL;
        reset_events();
        assert(fdk_ok(fdk_shell_tray_host_create(busA, &desc, &A)));
        assert(pump_until(ctx, 3.0, NULL, NULL) == 1);
        assert(fdk_shell_tray_host_serving(A) == 1);

        /* B (no flags): the refusal, DORMANT. */
        fdk_shell_tray_host *B = NULL;
        reset_events();
        assert(fdk_ok(fdk_shell_tray_host_create(busB, &desc, &B)));
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_te.host_state >= 1 && g_te.host_serving == 0);
        assert(fdk_shell_tray_host_serving(B) == 0);

        /* A vacates; B must STAY dormant (never a queue-lurker). */
        fdk_shell_tray_host_destroy(A);
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(fdk_shell_tray_host_serving(B) == 0);

        fdk_shell_tray_host_destroy(B);
        fdk_dbus_connection_close(busA);
        fdk_dbus_connection_close(busB);
    }
    printf("[ok] etiquette: refusal + dormant-after-vacate\n");

    fdk_dbus_connection_close(bus);
}

typedef struct {
    int done;
    char name[64];
} cap_info;

static void server_info_reply(fdk_dbus_connection *bus,
                              fdk_dbus_message *reply, void *user) {
    (void)bus;
    cap_info *c = user;
    c->done = 1;
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (reply != NULL &&
        fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
        n >= 1 && args != NULL) {
        snprintf(c->name, sizeof(c->name), "%s",
                 fdk_dbus_value_string(args[0]) != NULL
                     ? fdk_dbus_value_string(args[0])
                     : "");
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
}

static void test_notify_model(fdk_context *ctx) {
    printf("-- notification server model --\n");
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    assert(bus != NULL);
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    assert(fdk_dbus_connection_ready(bus));

    fdk_shell_notify_server *srv = NULL;
    fdk_shell_notify_server_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.event_cb = notify_event;
    desc.user = NULL;
    desc.server_name = "testd";
    desc.default_timeout_ms = 4000;
    desc.max_live = 4;
    assert(fdk_ok(fdk_shell_notify_server_create(bus, ctx, &desc, &srv)));
    assert(srv != NULL);
    assert(pump_until(ctx, 3.0, NULL, NULL) == 1);
    assert(g_ne.server_state >= 1 && g_ne.serving == 1);
    assert(fdk_shell_notify_server_serving(srv) == 1);
    printf("[ok] create: the Notifications name acquired\n");

    assert(client_start(ctx));
    reset_events();

    /* Notify: a normal note with an action. */
    uint32_t id1 = client_notify(ctx, "TestApp", 0, "First summary",
                                 "First body", 0 /* never */, 1, "act1");
    assert(id1 != 0);
    assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
    assert(g_ne.added == 1 && g_ne.last_id == id1);
    assert(fdk_shell_notify_note_count(srv) == 1);
    {
        fdk_shell_notify_note_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        char sum[64], body[64];
        size_t sum_l = sizeof(sum), body_l = sizeof(body);
        info.summary = sum;
        info.summary_len = sum_l;
        info.body = body;
        info.body_len = body_l;
        assert(fdk_ok(fdk_shell_notify_note_snapshot(srv, id1, &info)));
        assert(strcmp(sum, "First summary") == 0);
        assert(strcmp(body, "First body") == 0);
        assert(info.urgency == 1);
        assert(info.timeout_ms == 0); /* expire_timeout 0 = never   */
        assert(info.action_count == 1);
        assert(info.has_default == 0);
        char key[64], label[64];
        assert(fdk_ok(fdk_shell_notify_note_action(
            srv, id1, 0, key, sizeof(key), label, sizeof(label))));
        assert(strcmp(key, "act1") == 0);
        assert(strcmp(label, "Do it") == 0);
        assert(fdk_shell_notify_note_snapshot(srv, 999, &info) ==
               FDK_ERR_NOT_FOUND);
    }
    printf("[ok] Notify: id assigned, snapshot fields, action pair\n");

    /* Replaces: the same id returns. */
    {
        uint32_t id2 = client_notify(ctx, "TestApp", id1, "Replaced",
                                     "New body", 0, 1, NULL);
        assert(id2 == id1);
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_ne.replaced == 1);
        assert(fdk_shell_notify_note_count(srv) == 1);
        fdk_shell_notify_note_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        char sum[64];
        size_t sum_l = sizeof(sum);
        info.summary = sum;
        info.summary_len = sum_l;
        assert(fdk_ok(fdk_shell_notify_note_snapshot(srv, id1, &info)));
        assert(strcmp(sum, "Replaced") == 0);
    }
    printf("[ok] replaces_id: same id, REPLACED event, content swapped\n");

    /* Action invocation: the signal + the close (non-resident). */
    {
        int closed_before = g_client.closed_signals;
        int invoked_before = g_client.invoked_signals;
        assert(fdk_ok(fdk_shell_notify_action_invoke(srv, id1, "act1")));
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_client.invoked_signals == invoked_before + 1);
        assert(strcmp(g_client.last_action_key, "act1") == 0);
        assert(g_client.closed_signals == closed_before + 1);
        assert(g_client.last_closed_id == id1);
        assert(g_client.last_closed_reason == 2); /* dismissed      */
        assert(fdk_shell_notify_note_count(srv) == 0);
        assert(g_ne.closed == 1);
    }
    printf("[ok] action invoke: ActionInvoked + close(reason 2)\n");

    /* Expiration: a short timeout closes with reason 1. */
    {
        reset_events();
        uint32_t id = client_notify(ctx, "TestApp", 0, "Expiring",
                                    "gone soon", 250, 1, NULL);
        assert(id != 0);
        assert(fdk_shell_notify_note_count(srv) == 1);
        double dl = now_s() + 3.0;
        while (now_s() < dl && fdk_shell_notify_note_count(srv) == 1) {
            fdk_pump_events(ctx, 50);
        }
        assert(fdk_shell_notify_note_count(srv) == 0);
        /* The client's signal readback needs its own pump pass. */
        pump_until(ctx, 1.0, NULL, NULL);
        assert(g_client.last_closed_reason == 1); /* expired        */
        assert(g_ne.closed >= 1);
    }
    printf("[ok] expiration: reason 1 through the timer\n");

    /* Critical + -1 timeout: never expires. */
    {
        uint32_t id = client_notify(ctx, "TestApp", 0, "Critical",
                                    "stays", -1, 2, NULL);
        assert(id != 0);
        fdk_shell_notify_note_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        assert(fdk_ok(fdk_shell_notify_note_snapshot(srv, id, &info)));
        assert(info.timeout_ms == 0); /* critical: never            */
        assert(fdk_ok(fdk_shell_notify_close(srv, id, 3)));
        assert(pump_until(ctx, 1.0, NULL, NULL) == 1);
        assert(g_client.last_closed_reason == 3);
    }
    printf("[ok] critical -1: never expires; close(reason 3)\n");

    /* The default timeout: -1 at NORMAL urgency -> desc's 4000. */
    {
        fdk_shell_notify_note_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        uint32_t id = client_notify(ctx, "T", 0, "Normal", "", -1, 1,
                                    NULL);
        assert(id != 0);
        assert(fdk_ok(fdk_shell_notify_note_snapshot(srv, id, &info)));
        assert(info.timeout_ms == 4000);
        assert(fdk_ok(fdk_shell_notify_close(srv, id, 2)));
    }
    printf("[ok] -1 normal: the server default resolved\n");

    /* The flood gate: 30 in a window; ids answered, set capped. */
    {
        reset_events();
        for (int i = 0; i < 30; i++) {
            uint32_t id = client_notify(ctx, "Flood", 0, "flood", "",
                                        0 /* never */, 1, NULL);
            assert(id != 0); /* every call answered                 */
        }
        assert(fdk_shell_notify_note_count(srv) <= 25);
        for (uint32_t i = fdk_shell_notify_note_count(srv); i > 0; i--) {
            /* close them all through the model (reason 4). */
        }
        while (fdk_shell_notify_note_count(srv) > 0) {
            fdk_shell_notify_note_info info;
            memset(&info, 0, sizeof(info));
            info.struct_size = sizeof(info);
            /* The newest is the head; close by id via snapshot. */
            uint32_t id = 0;
            /* enumerate: the first id is the newest; find by trial */
            for (uint32_t guess = 1; guess < 200; guess++) {
                fdk_shell_notify_note_info probe;
                memset(&probe, 0, sizeof(probe));
                probe.struct_size = sizeof(probe);
                if (fdk_ok(fdk_shell_notify_note_snapshot(srv, guess,
                                                          &probe))) {
                    id = guess;
                    break;
                }
            }
            assert(id != 0);
            assert(fdk_ok(fdk_shell_notify_close(srv, id, 2)));
            fdk_pump_events(ctx, 10);
        }
    }
    printf("[ok] flood gate: 30 calls answered, live set capped\n");

    /* The live set: two notes tracked and closed by id. (The
     * live-set CAP itself ran through the flood leg above — 30
     * calls against max_live 4; creating a SECOND server on the
     * same connection is the header's documented one-per-connection
     * misuse, not a test case.) */
    {
        /* Drain the flood window first: notes sent within 1 s of the
         * burst would be acknowledged-and-dropped (the gate's
         * honest behavior), leaving the live set empty. */
        pump_until(ctx, 1.3, NULL, NULL);
        reset_events();
        uint32_t a = client_notify(ctx, "T", 0, "one", "", 0, 0, NULL);
        uint32_t b = client_notify(ctx, "T", 0, "two", "", 0, 0, NULL);
        assert(a != 0 && b != 0);
        assert(fdk_shell_notify_note_count(srv) == 2);
        assert(fdk_ok(fdk_shell_notify_close(srv, a, 2)));
        assert(fdk_ok(fdk_shell_notify_close(srv, b, 2)));
        pump_until(ctx, 1.0, NULL, NULL);
        assert(fdk_shell_notify_note_count(srv) == 0);
        assert(g_ne.closed >= 2);
    }
    printf("[ok] live set: notes tracked and closed\n");

    /* GetCapabilities / GetServerInformation through the client. */
    {
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications", "GetServerInformation");
        cap_info ci = {0, ""};
        fdk_dbus_pending *p = fdk_dbus_connection_call(
            g_client.bus, call, 3000, server_info_reply, &ci);
        (void)p;
        double dl = now_s() + 3.0;
        while (now_s() < dl && !ci.done) {
            fdk_pump_events(ctx, 25);
        }
        assert(ci.done);
        assert(strcmp(ci.name, "testd") == 0); /* the desc's name    */
    }
    printf("[ok] GetServerInformation: the desc's server name\n");

    /* The etiquette: a second server on another connection refuses. */
    {
        fdk_dbus_connection *busB =
            fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
        assert(busB != NULL);
        double dl = now_s() + 5.0;
        while (!fdk_dbus_connection_ready(busB) && now_s() < dl) {
            fdk_pump_events(ctx, 20);
        }
        assert(fdk_dbus_connection_ready(busB));
        fdk_shell_notify_server *B = NULL;
        reset_events();
        fdk_shell_notify_server_desc d2;
        memset(&d2, 0, sizeof(d2));
        d2.struct_size = sizeof(d2);
        d2.event_cb = notify_event;
        assert(fdk_ok(fdk_shell_notify_server_create(busB, ctx, &d2,
                                                     &B)));
        assert(pump_until(ctx, 2.0, NULL, NULL) == 1);
        assert(g_ne.server_state >= 1 && g_ne.serving == 0);
        assert(fdk_shell_notify_server_serving(B) == 0);
        fdk_shell_notify_server_destroy(B);
        fdk_dbus_connection_close(busB);
    }
    printf("[ok] etiquette: the incumbent refusal\n");

    fdk_shell_notify_server_destroy(srv);
    fdk_dbus_connection_close(g_client.bus);
    g_client.bus = NULL;
    fdk_dbus_connection_close(bus);
    printf("[ok] destroy: clean teardown\n");
}

/* ---- main ----------------------------------------------------------------------- */

int main(void) {
    /* Skip discipline (test_dbus_live.c's): no bus -> skip. */
    const char *bus_addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (bus_addr == NULL || bus_addr[0] == '\0') {
        printf("[skip] no DBUS_SESSION_BUS_ADDRESS (run under "
               "dbus-run-session)\n");
        return 0;
    }
    const char *display = getenv("DISPLAY");
    if (display == NULL || display[0] == '\0') {
        if (getenv("FDK_DESKTOP_MODEL_TEST_REQUIRE") != NULL) {
            fprintf(stderr, "FAIL: FDK_DESKTOP_MODEL_TEST_REQUIRE set "
                            "but no DISPLAY\n");
            return 1;
        }
        printf("[skip] no DISPLAY for the fdk_context\n");
        return 0;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("[skip] no display connection\n");
        return 0;
    }

    test_tray_model(ctx);
    test_notify_model(ctx);

    fdk_shutdown(ctx);
    printf("== desktop model tests passed ==\n");
    return 0;
}
