/*
 * tray_item.c — the StatusNotifierItem export (1.1.0)
 *
 * The item half of the app-facing tray API: owns the bus
 * connection, exports /StatusNotifierItem (both the org.kde and
 * org.ayatana interface spellings) and /MenuBar (the attached
 * menu's com.canonical.dbusmenu), registers with whichever
 * StatusNotifierWatcher answers — the KDE name first, the ayatana
 * twin as the fallback — and re-registers through watcher
 * restarts. The public contract is include/fdk/fdk_tray.h.
 *
 * Emission discipline: every setter updates the state AND tells
 * the hosts BOTH ways the spec allows — the New* signal family
 * (on both interface spellings; hosts subscribe to one) AND
 * org.freedesktop.DBus.Properties.PropertiesChanged with the
 * org.kde interface as arg0. Hosts that follow either convention
 * see the change; hosts that follow neither re-read at their next
 * open.
 */

#include "desktop/tray_internal.h"

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

#include "core/alloc_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PIXMAP_DIM 512
#define MAX_LABEL_BYTES 4096

char *fdk__tray_strdup(const char *s) {
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

static const char *status_string(fdk_tray_status s) {
    switch (s) {
    case FDK_TRAY_STATUS_PASSIVE:
        return "Passive";
    case FDK_TRAY_STATUS_NEEDS_ATTENTION:
        return "NeedsAttention";
    case FDK_TRAY_STATUS_ACTIVE:
    default:
        return "Active";
    }
}

/* ---- property marshaling ----------------------------------------------------
 * One a{sv} entry with a VARIANT-wrapped value; new_variant clones
 * its child, new_dict_entry takes both sides. */

static fdk_dbus_value *pixmap_struct(int32_t w, int32_t h,
                                     const uint8_t *bytes, size_t len) {
    fdk_dbus_value *bw = fdk_dbus_value_new_int32(w);
    fdk_dbus_value *bh = fdk_dbus_value_new_int32(h);
    fdk_dbus_value *data = fdk_dbus_value_new_byte_array(bytes, len);
    if (bw == NULL || bh == NULL || data == NULL) {
        fdk_dbus_value_free(bw);
        fdk_dbus_value_free(bh);
        fdk_dbus_value_free(data);
        return NULL;
    }
    fdk_dbus_value *fields[3] = {bw, bh, data};
    fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 3);
    if (st == NULL) {
        fdk_dbus_value_free(bw);
        fdk_dbus_value_free(bh);
        fdk_dbus_value_free(data);
    }
    return st;
}

/* The a(iiay) sizes list for one pixmap (the single-size answer —
 * see fdk_tray.h's honest note). Ownership: new_array takes the
 * struct on SUCCESS only — the failure path frees it. */
static fdk_dbus_value *pixmap_array(int32_t w, int32_t h,
                                    const uint8_t *bytes, size_t len) {
    if (bytes == NULL) {
        return fdk_dbus_value_new_array("(iiay)", NULL, 0);
    }
    fdk_dbus_value *one = pixmap_struct(w, h, bytes, len);
    if (one == NULL) {
        return NULL;
    }
    fdk_dbus_value *items[1] = {one};
    fdk_dbus_value *arr = fdk_dbus_value_new_array("(iiay)", items, 1);
    if (arr == NULL) {
        fdk_dbus_value_free(one);
    }
    return arr;
}

/* The ToolTip value: (sa(iiay)(ss)ss) — icon name, pixmap list, the
 * legacy (title, description) struct, title, description (both
 * spellings carried; hosts read whichever they know). */
static fdk_dbus_value *tooltip_value(const fdk_tray_item *it) {
    fdk_dbus_value *f0 = fdk_dbus_value_new_string(
        it->tt_icon != NULL ? it->tt_icon : "");
    fdk_dbus_value *f1 = pixmap_array(0, 0, NULL, 0);
    fdk_dbus_value *t0 = fdk_dbus_value_new_string(
        it->tt_title != NULL ? it->tt_title : "");
    fdk_dbus_value *t1 = fdk_dbus_value_new_string(
        it->tt_desc != NULL ? it->tt_desc : "");
    fdk_dbus_value *mid_fields[2] = {t0, t1};
    fdk_dbus_value *mid = (t0 != NULL && t1 != NULL)
                              ? fdk_dbus_value_new_struct(mid_fields, 2)
                              : NULL;
    if (mid == NULL) {
        fdk_dbus_value_free(t0);
        fdk_dbus_value_free(t1);
    }
    fdk_dbus_value *f3 = fdk_dbus_value_new_string(
        it->tt_title != NULL ? it->tt_title : "");
    fdk_dbus_value *f4 = fdk_dbus_value_new_string(
        it->tt_desc != NULL ? it->tt_desc : "");
    fdk_dbus_value *ok = NULL;
    if (f0 != NULL && f1 != NULL && mid != NULL && f3 != NULL &&
        f4 != NULL) {
        fdk_dbus_value *fields[5] = {f0, f1, mid, f3, f4};
        ok = fdk_dbus_value_new_struct(fields, 5);
    }
    if (ok == NULL) {
        /* new_struct takes the fields ONLY on success. */
        fdk_dbus_value_free(f0);
        fdk_dbus_value_free(f1);
        fdk_dbus_value_free(mid);
        fdk_dbus_value_free(f3);
        fdk_dbus_value_free(f4);
    }
    return ok;
}

/* One property as its VARIANT value, by name (NULL = unknown). */
static fdk_dbus_value *item_property(const fdk_tray_item *it,
                                     const char *name) {
    if (strcmp(name, "Category") == 0) {
        fdk_dbus_value *v = fdk_dbus_value_new_string(
            it->category != NULL ? it->category : "ApplicationStatus");
        return v;
    }
    if (strcmp(name, "Id") == 0) {
        return fdk_dbus_value_new_string(it->id);
    }
    if (strcmp(name, "Title") == 0) {
        return fdk_dbus_value_new_string(
            it->title != NULL ? it->title : "");
    }
    if (strcmp(name, "Status") == 0) {
        return fdk_dbus_value_new_string(status_string(it->status));
    }
    if (strcmp(name, "WindowId") == 0) {
        return fdk_dbus_value_new_int32(0);
    }
    if (strcmp(name, "IconName") == 0) {
        return fdk_dbus_value_new_string(
            it->icon_name != NULL ? it->icon_name : "");
    }
    if (strcmp(name, "IconPixmap") == 0) {
        return pixmap_array(it->icon_px.w, it->icon_px.h,
                            it->icon_px.bytes, it->icon_px.len);
    }
    if (strcmp(name, "AttentionIconName") == 0) {
        return fdk_dbus_value_new_string(
            it->att_icon_name != NULL ? it->att_icon_name : "");
    }
    if (strcmp(name, "AttentionIconPixmap") == 0) {
        return pixmap_array(it->att_px.w, it->att_px.h,
                            it->att_px.bytes, it->att_px.len);
    }
    if (strcmp(name, "ToolTip") == 0) {
        return tooltip_value(it);
    }
    if (strcmp(name, "Menu") == 0) {
        return fdk_dbus_value_new_object_path(
            it->menu != NULL ? "/MenuBar" : "/");
    }
    if (strcmp(name, "ItemIsMenu") == 0) {
        return fdk_dbus_value_new_boolean(0);
    }
    return NULL;
}

/* The full a{sv} GetAll answer. */
static fdk_dbus_value *item_all_properties(const fdk_tray_item *it) {
    static const char *const keys[] = {
        "Category",         "Id",           "Title",
        "Status",           "WindowId",     "IconName",
        "IconPixmap",       "AttentionIconName",
        "AttentionIconPixmap", "ToolTip",   "Menu",
        "ItemIsMenu",
    };
    size_t nkeys = sizeof(keys) / sizeof(*keys);
    fdk_dbus_value *entries[12];
    size_t n = 0;
    for (size_t i = 0; i < nkeys; i++) {
        fdk_dbus_value *inner = item_property(it, keys[i]);
        if (inner == NULL) {
            continue;
        }
        fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
        fdk_dbus_value_free(inner);
        if (var == NULL) {
            continue;
        }
        fdk_dbus_value *k = fdk_dbus_value_new_string(keys[i]);
        if (k == NULL) {
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
    fdk_dbus_value *arr = fdk_dbus_value_new_array("{sv}", entries, n);
    if (arr == NULL) {
        for (size_t i = 0; i < n; i++) {
            fdk_dbus_value_free(entries[i]);
        }
    }
    return arr;
}

/* ---- signal emission --------------------------------------------------------- */

static void emit_new_signal(const fdk_tray_item *it, const char *member) {
    if (it->bus == NULL) {
        return;
    }
    /* Both interface spellings: hosts subscribe to one of them; a
     * second send is cheap and misses nobody. */
    static const char *const ifaces[2] = {
        "org.kde.StatusNotifierItem", "org.ayatana.StatusNotifierItem",
    };
    for (size_t i = 0; i < 2; i++) {
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            "/StatusNotifierItem", ifaces[i], member);
        if (sig == NULL) {
            continue;
        }
        send_or_free(it->bus, sig);
    }
}

static void emit_new_status(const fdk_tray_item *it) {
    if (it->bus == NULL) {
        return;
    }
    static const char *const ifaces[2] = {
        "org.kde.StatusNotifierItem", "org.ayatana.StatusNotifierItem",
    };
    for (size_t i = 0; i < 2; i++) {
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            "/StatusNotifierItem", ifaces[i], "NewStatus");
        if (sig == NULL) {
            continue;
        }
        if (fdk_ok(fdk_dbus_message_append_string(
                sig, status_string(it->status)))) {
            send_or_free(it->bus, sig);
        } else {
            fdk_dbus_message_free(sig);
        }
    }
}

/* PropertiesChanged for the changed keys (belt and braces beside the
 * New* family). */
static void emit_properties_changed(fdk_tray_item *it, const char *key) {
    if (it->bus == NULL) {
        return;
    }
    fdk_dbus_value *inner = item_property(it, key);
    if (inner == NULL) {
        return;
    }
    fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    if (var == NULL) {
        return;
    }
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    if (k == NULL) {
        fdk_dbus_value_free(var);
        return;
    }
    fdk_dbus_value *e = fdk_dbus_value_new_dict_entry(k, var);
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
        return;
    }
    fdk_dbus_value *entries[1] = {e};
    fdk_dbus_value *changed = fdk_dbus_value_new_array("{sv}", entries, 1);
    if (changed == NULL) {
        fdk_dbus_value_free(e);
        return;
    }
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        "/StatusNotifierItem", "org.freedesktop.DBus.Properties",
        "PropertiesChanged");
    if (sig != NULL &&
        fdk_ok(fdk_dbus_message_append_string(
            sig, "org.kde.StatusNotifierItem")) &&
        fdk_ok(fdk_dbus_message_append(sig, changed)) &&
        fdk_ok(fdk_dbus_message_append_string(sig, ""))) {
        send_or_free(it->bus, sig);
    } else if (sig != NULL) {
        fdk_dbus_message_free(sig);
    }
    fdk_dbus_value_free(changed);
}

/* ---- the method dispatcher ---------------------------------------------------- */

static const char *const SNI_INTROSPECT_XML =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\" "
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.kde.StatusNotifierItem\">\n"
    "    <method name=\"Activate\">\n"
    "      <arg name=\"x\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"y\" type=\"i\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"SecondaryActivate\">\n"
    "      <arg name=\"x\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"y\" type=\"i\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"Scroll\">\n"
    "      <arg name=\"delta\" type=\"i\" direction=\"in\"/>\n"
    "      <arg name=\"orientation\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <property name=\"Category\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Id\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Title\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"Status\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"WindowId\" type=\"u\" access=\"read\"/>\n"
    "    <property name=\"IconName\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"IconPixmap\" type=\"a(iiay)\" "
    "access=\"read\"/>\n"
    "    <property name=\"AttentionIconName\" type=\"s\" "
    "access=\"read\"/>\n"
    "    <property name=\"AttentionIconPixmap\" type=\"a(iiay)\" "
    "access=\"read\"/>\n"
    "    <property name=\"ToolTip\" type=\"(sa(iiay)(ss)ss)\" "
    "access=\"read\"/>\n"
    "    <property name=\"Menu\" type=\"o\" access=\"read\"/>\n"
    "    <property name=\"ItemIsMenu\" type=\"b\" access=\"read\"/>\n"
    "    <signal name=\"NewTitle\"/>\n"
    "    <signal name=\"NewIcon\"/>\n"
    "    <signal name=\"NewAttentionIcon\"/>\n"
    "    <signal name=\"NewToolTip\"/>\n"
    "    <signal name=\"NewMenu\"/>\n"
    "    <signal name=\"NewStatus\">\n"
    "      <arg name=\"status\" type=\"s\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\"/>\n"
    "</node>\n";

static void reply_simple(fdk_dbus_connection *bus,
                         const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        send_or_free(bus, rep);
    }
}

static void reply_error(fdk_dbus_connection *bus,
                        const fdk_dbus_message *call, const char *name,
                        const char *text) {
    fdk_dbus_message *err = fdk_dbus_message_new_error(call, name, text);
    if (err != NULL) {
        send_or_free(bus, err);
    }
}

static int is_sni_interface(const char *iface) {
    return iface != NULL && (strcmp(iface, "org.kde.StatusNotifierItem") == 0 ||
                             strcmp(iface,
                                    "org.ayatana.StatusNotifierItem") == 0);
}

static void serve_properties(fdk_tray_item *it, fdk_dbus_connection *bus,
                             const fdk_dbus_message *call) {
    const char *member = fdk_dbus_message_member(call);
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (strcmp(member, "Get") == 0) {
        if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 2 &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING &&
            fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
            /* Copy before the free (borrowed strings). */
            char *iface = fdk__tray_strdup(fdk_dbus_value_string(args[0]));
            char *name = fdk__tray_strdup(fdk_dbus_value_string(args[1]));
            fdk_dbus_values_free_array(args, n);
            if (iface == NULL || name == NULL) {
                fdk_free(iface);
                fdk_free(name);
                reply_error(bus, call,
                            "org.freedesktop.DBus.Error.NoMemory", NULL);
                return;
            }
            fdk_dbus_value *v = is_sni_interface(iface)
                                    ? item_property(it, name)
                                    : NULL;
            fdk_free(iface);
            if (v == NULL) {
                reply_error(bus, call,
                            "org.freedesktop.DBus.Error.InvalidArgs",
                            "no such property on this interface");
                fdk_free(name);
                return;
            }
            fdk_dbus_value *var = fdk_dbus_value_new_variant(v);
            fdk_dbus_value_free(v);
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL && var != NULL &&
                fdk_ok(fdk_dbus_message_append(rep, var))) {
                send_or_free(bus, rep);
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            fdk_dbus_value_free(var);
            fdk_free(name);
        } else {
            fdk_dbus_values_free_array(args, n);
            reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs",
                        "Get expects (s s)");
        }
        return;
    }
    if (strcmp(member, "GetAll") == 0) {
        if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 1 &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
            char *iface = fdk__tray_strdup(fdk_dbus_value_string(args[0]));
            fdk_dbus_values_free_array(args, n);
            if (iface == NULL) {
                reply_error(bus, call,
                            "org.freedesktop.DBus.Error.NoMemory", NULL);
                return;
            }
            int sni = is_sni_interface(iface);
            fdk_free(iface);
            if (!sni) {
                /* An empty dict: the honest answer for an interface
                 * we do not carry properties on. */
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                fdk_dbus_value *empty =
                    fdk_dbus_value_new_array("{sv}", NULL, 0);
                if (rep != NULL && empty != NULL &&
                    fdk_ok(fdk_dbus_message_append(rep, empty))) {
                    send_or_free(bus, rep);
                } else if (rep != NULL) {
                    fdk_dbus_message_free(rep);
                }
                fdk_dbus_value_free(empty);
                return;
            }
            fdk_dbus_value *all = item_all_properties(it);
            if (all == NULL) {
                reply_error(bus, call,
                            "org.freedesktop.DBus.Error.NoMemory", NULL);
                return;
            }
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL && fdk_ok(fdk_dbus_message_append(rep, all))) {
                send_or_free(bus, rep);
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            fdk_dbus_value_free(all);
        } else {
            fdk_dbus_values_free_array(args, n);
            reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs",
                        "GetAll expects (s)");
        }
        return;
    }
    if (strcmp(member, "Set") == 0) {
        fdk_dbus_values_free_array(args, n);
        reply_error(bus, call, "org.freedesktop.DBus.Error.PropertyReadOnly",
                    "StatusNotifierItem properties are read-only");
        return;
    }
    fdk_dbus_values_free_array(args, n);
}

/* Activate / SecondaryActivate — both (i i). */
static void serve_activation(fdk_tray_item *it, fdk_dbus_connection *bus,
                             const fdk_dbus_message *call) {
    const char *member = fdk_dbus_message_member(call);
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    int32_t a = 0, b = 0;
    int ok = fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 2;
    if (ok) {
        a = fdk_dbus_value_int32(args[0]);
        b = fdk_dbus_value_int32(args[1]);
    }
    fdk_dbus_values_free_array(args, n);
    if (!ok) {
        reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs",
                    "expected (i i)");
        return;
    }
    reply_simple(bus, call);
    if (it->tearing_down) {
        return;
    }
    if (strcmp(member, "Activate") == 0) {
        if (it->cb.on_activate != NULL) {
            it->cb.on_activate(it, 0, a, b, it->cb_user);
        }
    } else if (strcmp(member, "SecondaryActivate") == 0) {
        if (it->cb.on_activate != NULL) {
            it->cb.on_activate(it, 1, a, b, it->cb_user);
        }
    }
}

/* The SNI Scroll actually carries (i, s). The dispatcher routes it
 * here, not through serve_activation. */
static void serve_scroll(fdk_tray_item *it, fdk_dbus_connection *bus,
                         const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    int32_t delta = 0;
    char *orientation = NULL;
    if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 2 &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_INT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        delta = fdk_dbus_value_int32(args[0]);
        orientation = fdk__tray_strdup(fdk_dbus_value_string(args[1]));
    }
    fdk_dbus_values_free_array(args, n);
    if (orientation == NULL) {
        reply_error(bus, call, "org.freedesktop.DBus.Error.InvalidArgs",
                    "Scroll expects (i s)");
        return;
    }
    reply_simple(bus, call);
    if (!it->tearing_down && it->cb.on_scroll != NULL) {
        int vertical = strcmp(orientation, "vertical") == 0;
        it->cb.on_scroll(it, vertical ? 0 : delta,
                         vertical ? delta : 0, it->cb_user);
    }
    fdk_free(orientation);
}

static void item_method_handler(fdk_dbus_connection *bus,
                                fdk_dbus_message *call, void *user) {
    fdk_tray_item *it = user;
    const char *path = fdk_dbus_message_path(call);
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (path == NULL || iface == NULL || member == NULL) {
        return;
    }

    /* /MenuBar: the dbusmenu server (plus the standard interfaces). */
    if (strcmp(path, "/MenuBar") == 0) {
        if (strcmp(iface, "com.canonical.dbusmenu") == 0) {
            if (fdk__tray_menu_serve_method(it->menu, bus, call)) {
                return;
            }
        } else if (strcmp(iface, "org.freedesktop.DBus.Introspectable") ==
                   0) {
            if (strcmp(member, "Introspect") == 0) {
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                if (rep != NULL &&
                    fdk_ok(fdk_dbus_message_append_string(
                        rep, fdk__tray_menu_introspect_xml()))) {
                    send_or_free(bus, rep);
                } else if (rep != NULL) {
                    fdk_dbus_message_free(rep);
                }
                return;
            }
        } else if (strcmp(iface, "org.freedesktop.DBus.Peer") == 0) {
            if (strcmp(member, "Ping") == 0) {
                reply_simple(bus, call);
                return;
            }
            if (strcmp(member, "GetMachineId") == 0) {
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                /* The machine id this process would report for the
                 * Peer interface: the D-Bus daemon's own value is
                 * peer-specific; an empty string is the honest
                 * per-process answer (nobody consumes an item's
                 * machine id). */
                if (rep != NULL &&
                    fdk_ok(fdk_dbus_message_append_string(rep, ""))) {
                    send_or_free(bus, rep);
                } else if (rep != NULL) {
                    fdk_dbus_message_free(rep);
                }
                return;
            }
        }
        reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownMethod",
                    "no such method on /MenuBar");
        return;
    }

    if (strcmp(path, "/StatusNotifierItem") != 0) {
        reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownMethod",
                    "no such object");
        return;
    }

    /* The standard interfaces first. */
    if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
        serve_properties(it, bus, call);
        return;
    }
    if (strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0) {
        if (strcmp(member, "Introspect") == 0) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL && fdk_ok(fdk_dbus_message_append_string(
                                    rep, SNI_INTROSPECT_XML))) {
                send_or_free(bus, rep);
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
        }
        return;
    }
    if (strcmp(iface, "org.freedesktop.DBus.Peer") == 0) {
        if (strcmp(member, "Ping") == 0) {
            reply_simple(bus, call);
        }
        return;
    }

    /* The SNI methods (either spelling). */
    if (is_sni_interface(iface)) {
        if (strcmp(member, "Activate") == 0 ||
            strcmp(member, "SecondaryActivate") == 0) {
            serve_activation(it, bus, call);
            return;
        }
        if (strcmp(member, "Scroll") == 0) {
            serve_scroll(it, bus, call);
            return;
        }
        reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownMethod",
                    "no such StatusNotifierItem method");
        return;
    }

    reply_error(bus, call, "org.freedesktop.DBus.Error.UnknownMethod",
                "no such interface on this object");
}

/* ---- registration -------------------------------------------------------------- */

static void on_register_reply(fdk_dbus_connection *bus,
                              fdk_dbus_message *reply, void *user) {
    (void)bus;
    fdk_tray_item *it = user;
    it->registering = 0;
    if (it->tearing_down) {
        return;
    }
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        FDK_INFO("tray: registration refused (%s)",
                 reply != NULL ? fdk_dbus_message_error_name(reply)
                               : "no reply");
        it->registered_to = NULL;
        return;
    }
    it->hosted = 1;
    FDK_INFO("tray: registered with the watcher (%s)",
             it->registered_to != NULL ? it->registered_to : "?");
    if (it->cb.on_host != NULL) {
        it->cb.on_host(it, 1, it->cb_user);
    }
}

static const char *watcher_with_owner(const fdk_tray_item *it) {
    /* KDE first, ayatana as the fallback; the owner states were
     * cached by watcher_event (both watchers fire their initial
     * state on install, so the cache is never stale). */
    return it->kde_owner != NULL
               ? "org.kde.StatusNotifierWatcher"
               : (it->ayatana_owner != NULL
                      ? "org.ayatana.StatusNotifierWatcher"
                      : NULL);
}

static void register_with(fdk_tray_item *it, const char *watcher) {
    if (it->bus == NULL || it->registering || watcher == NULL) {
        return;
    }
    const char *unique = fdk_dbus_connection_unique_name(it->bus);
    if (unique == NULL) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        watcher, "/StatusNotifierWatcher", watcher,
        "RegisterStatusNotifierItem");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, unique))) {
        fdk_dbus_message_free(call);
        return;
    }
    it->registering = 1;
    if (fdk_dbus_connection_call(it->bus, call, FDK_DBUS_TIMEOUT_DEFAULT,
                                 on_register_reply, (void *)it) == NULL) {
        fdk_dbus_message_free(call);
        it->registering = 0;
        return;
    }
    it->registered_to = watcher;
}

static void watcher_event(fdk_dbus_connection *bus, const char *name,
                          const char *owner, void *user);

/* ---- connection lifecycle -------------------------------------------------------- */

static void on_ready(fdk_dbus_connection *bus, int ok, const char *error,
                     void *user) {
    (void)error;
    fdk_tray_item *it = user;
    if (it->tearing_down) {
        return;
    }
    if (!ok) {
        FDK_INFO("tray: session bus connect failed — waiting for a "
                 "setter or reopen");
        it->bus = NULL;
        fdk_dbus_connection_close(bus);
        return;
    }
    fdk_dbus_connection_set_method_handler(bus, item_method_handler, it);
    it->watch_kde = fdk_dbus_connection_watch_name(
        bus, "org.kde.StatusNotifierWatcher", watcher_event, it);
    it->watch_ayatana = fdk_dbus_connection_watch_name(
        bus, "org.ayatana.StatusNotifierWatcher", watcher_event, it);
}

static void on_death(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    fdk_tray_item *it = user;
    if (it->tearing_down) {
        return;
    }
    FDK_INFO("tray: the bus connection died — unhosted");
    it->bus = NULL;
    it->watch_kde = NULL;
    it->watch_ayatana = NULL;
    it->registered_to = NULL;
    if (it->hosted) {
        it->hosted = 0;
        if (it->cb.on_host != NULL) {
            it->cb.on_host(it, 0, it->cb_user);
        }
    }
}

static void watcher_event(fdk_dbus_connection *bus, const char *name,
                          const char *owner, void *user) {
    (void)bus;
    fdk_tray_item *it = user;
    if (it->tearing_down) {
        return;
    }
    /* Cache the owner states; the KDE name wins when both exist. */
    if (strcmp(name, "org.kde.StatusNotifierWatcher") == 0) {
        fdk_free(it->kde_owner);
        it->kde_owner = fdk__tray_strdup(owner);
    } else {
        fdk_free(it->ayatana_owner);
        it->ayatana_owner = fdk__tray_strdup(owner);
    }
    const char *watcher = watcher_with_owner(it);

    if (watcher == NULL) {
        /* No watcher anywhere: unhosted (once). */
        if (it->hosted || it->registered_to != NULL) {
            it->hosted = 0;
            it->registered_to = NULL;
            FDK_INFO("tray: no StatusNotifierWatcher on the bus");
            if (it->cb.on_host != NULL) {
                it->cb.on_host(it, 0, it->cb_user);
            }
        }
        return;
    }
    if (it->hosted) {
        /* Still hosted by SOME watcher: nothing to do until that
         * one vanishes (the watcher == NULL path above). */
        return;
    }
    if (it->registered_to != NULL && strcmp(it->registered_to, watcher) == 0 &&
        it->registering) {
        return; /* already in flight with this one */
    }
    FDK_INFO("tray: a watcher is on the bus (%s) — registering", watcher);
    register_with(it, watcher);
}

/* ---- public ----------------------------------------------------------------------- */

fdk_tray_item *fdk_tray_item_open(fdk_context *ctx,
                                  const fdk_tray_item_options *opts,
                                  const fdk_tray_item_callbacks *callbacks,
                                  void *user) {
    if (ctx == NULL || opts == NULL || opts->id == NULL ||
        opts->id[0] == '\0') {
        return NULL;
    }
    fdk_tray_item *it = fdk_alloc(sizeof(*it));
    if (it == NULL) {
        return NULL;
    }
    memset(it, 0, sizeof(*it));
    it->ctx = ctx;
    if (callbacks != NULL) {
        it->cb = *callbacks;
    }
    it->cb_user = user;
    it->id = fdk__tray_strdup(opts->id);
    it->title = fdk__tray_strdup(opts->title);
    it->category = fdk__tray_strdup(
        opts->category != NULL ? opts->category : "ApplicationStatus");
    if (it->id == NULL) {
        fdk_tray_item_close(it);
        return NULL;
    }
    it->status = FDK_TRAY_STATUS_ACTIVE;
    it->bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, on_ready, it);
    if (it->bus == NULL) {
        FDK_INFO("tray: no session bus — the item waits unhosted");
    } else {
        fdk_dbus_connection_set_death_handler(it->bus, on_death, it);
    }
    return it;
}

void fdk_tray_item_close(fdk_tray_item *item) {
    if (item == NULL) {
        return;
    }
    item->tearing_down = 1;
    if (item->menu != NULL) {
        item->menu->item = NULL;
        item->menu = NULL;
    }
    if (item->bus != NULL) {
        fdk_dbus_connection_close(item->bus);
        item->bus = NULL;
    }
    item->watch_kde = NULL;
    item->watch_ayatana = NULL;
    fdk_free(item->kde_owner);
    fdk_free(item->ayatana_owner);
    fdk_free(item->id);
    fdk_free(item->title);
    fdk_free(item->category);
    fdk_free(item->icon_name);
    fdk_free(item->att_icon_name);
    fdk_free(item->icon_px.bytes);
    fdk_free(item->att_px.bytes);
    fdk_free(item->tt_icon);
    fdk_free(item->tt_title);
    fdk_free(item->tt_desc);
    fdk_free(item);
}

int fdk_tray_item_hosted(const fdk_tray_item *item) {
    return item != NULL ? item->hosted : 0;
}

void fdk_tray_item_set_menu(fdk_tray_item *item, fdk_tray_menu *menu) {
    if (item == NULL) {
        return;
    }
    if (item->menu == menu) {
        return;
    }
    if (item->menu != NULL) {
        item->menu->item = NULL;
    }
    if (menu != NULL && menu->item != NULL) {
        /* A menu serves ONE item: detach the previous owner. */
        FDK_INFO("tray: the menu was re-attached (one item per menu)");
        if (menu->item->menu == menu) {
            menu->item->menu = NULL;
        }
    }
    item->menu = menu;
    if (menu != NULL) {
        menu->item = item;
        fdk__tray_menu_emit_updated(menu);
    }
    emit_new_signal(item, "NewMenu");
    emit_properties_changed(item, "Menu");
}

/* ---- the setters -------------------------------------------------------------------- */

static void set_string(char **slot, const char *value) {
    char *copy = fdk__tray_strdup(value);
    if (copy == NULL && value != NULL) {
        return; /* no memory: keep the old truth */
    }
    fdk_free(*slot);
    *slot = copy;
}

void fdk_tray_item_set_icon_name(fdk_tray_item *item, const char *name) {
    if (item == NULL) {
        return;
    }
    set_string(&item->icon_name, name);
    emit_new_signal(item, "NewIcon");
    emit_properties_changed(item, "IconName");
}

static void set_pixmap(fdk_tray_item *item, int32_t width,
                       int32_t height, const uint8_t *argb_be, size_t len,
                       int attention) {
    tray_pixmap *px = attention ? &item->att_px : &item->icon_px;
    if (argb_be == NULL || width <= 0 || height <= 0 ||
        width > MAX_PIXMAP_DIM || height > MAX_PIXMAP_DIM ||
        len < (size_t)width * (size_t)height * 4u) {
        FDK_WARN("tray: pixmap %dx%d (%zu bytes) refused",
                 width, height, len);
        return;
    }
    uint8_t *copy = fdk_alloc_array(len, 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, argb_be, len);
    fdk_free(px->bytes);
    px->bytes = copy;
    px->w = width;
    px->h = height;
    px->len = len;
    emit_new_signal(item, attention ? "NewAttentionIcon" : "NewIcon");
    emit_properties_changed(item,
                            attention ? "AttentionIconPixmap" : "IconPixmap");
}

void fdk_tray_item_set_icon_pixmap(fdk_tray_item *item, int32_t width,
                                   int32_t height, const uint8_t *argb_be,
                                   size_t len) {
    if (item == NULL) {
        return;
    }
    set_pixmap(item, width, height, argb_be, len, 0);
}

void fdk_tray_item_set_attention_icon_name(fdk_tray_item *item,
                                           const char *name) {
    if (item == NULL) {
        return;
    }
    set_string(&item->att_icon_name, name);
    emit_new_signal(item, "NewAttentionIcon");
    emit_properties_changed(item, "AttentionIconName");
}

void fdk_tray_item_set_attention_icon_pixmap(fdk_tray_item *item,
                                             int32_t width, int32_t height,
                                             const uint8_t *argb_be,
                                             size_t len) {
    if (item == NULL) {
        return;
    }
    set_pixmap(item, width, height, argb_be, len, 1);
}

void fdk_tray_item_set_tooltip(fdk_tray_item *item, const char *icon_name,
                               const char *title,
                               const char *description) {
    if (item == NULL) {
        return;
    }
    set_string(&item->tt_icon, icon_name);
    set_string(&item->tt_title, title);
    set_string(&item->tt_desc, description);
    emit_new_signal(item, "NewToolTip");
    emit_properties_changed(item, "ToolTip");
}

void fdk_tray_item_set_title(fdk_tray_item *item, const char *title) {
    if (item == NULL) {
        return;
    }
    set_string(&item->title, title);
    emit_new_signal(item, "NewTitle");
    emit_properties_changed(item, "Title");
}

void fdk_tray_item_set_status(fdk_tray_item *item, fdk_tray_status status) {
    if (item == NULL || (int)status < 0 || (int)status > 2) {
        return;
    }
    if (item->status == status) {
        return;
    }
    item->status = status;
    emit_new_status(item);
    emit_properties_changed(item, "Status");
}
