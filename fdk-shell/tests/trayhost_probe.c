/* trayhost_probe.c — the trayhost rig's helper client (S-1 / A-11).
 *
 * A SYNTHETIC StatusNotifierItem with everything a real one carries:
 *   - KDE-dialect registration (its unique bus name) or the Ayatana
 *     dialect (a bare object path) — the rig picks per leg;
 *   - the full property set through org.freedesktop.DBus.Properties:
 *     dual-resolution IconPixmap (16x16 + 32x32 a(iiay), ARGB32
 *     network byte order — the byte order real items ship), an
 *     AttentionIconPixmap, the (sa(iiay)(ss)ss) ToolTip struct, the
 *     Menu object path, ItemIsMenu false;
 *   - a complete com.canonical.dbusmenu tree at /MenuBar: the
 *     mnemonic label ("Fi_rst item"), a separator, a checked
 *     checkmark, a radio pair, a disabled row, and a submenu with
 *     two children — every shape the host's parser must survive;
 *   - a control interface, fdk.trayprobe.Control, the rig drives
 *     live state through: SetStatus(s) / Retitle(s) / FlipIcon()
 *     each emit the matching New* signal; Quit() ends the probe.
 *
 * Every bus interaction is logged to stdout, line-buffered, in a
 * greppable form ("Activate x=.. y=..", "Event id=1 clicked", ...).
 *
 * Built by `make -C fdk-shell test-tray` next to the rig; runs under
 * the rig's dbus-run-session (a display is present for the event
 * loop). Exit codes: 0 ok, 1 failure.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W_NAME "org.kde.StatusNotifierWatcher"
#define W_PATH "/StatusNotifierWatcher"
#define W_IFACE "org.kde.StatusNotifierWatcher"

#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define MENU_IFACE "com.canonical.dbusmenu"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define CONTROL_IFACE "fdk.trayprobe.Control"
#define PROBE_WKN "org.fdk.TrayProbe" /* the control channel's bus name */

/* The probe's object paths. */
#define ITEM_PATH_KDE "/StatusNotifierItem"
#define ITEM_PATH_AAT "/org/fdk/trayprobe/Item1"
#define MENU_PATH "/MenuBar"
#define CONTROL_PATH "/org/fdk/trayprobe/Control"

/* ---- state ---------------------------------------------------------------- */

/* Status storage: a STABLE buffer (a stack pointer here was the
 * use-after-return: serve_control's frame dies, serve_getall reads
 * the corpse on the next GetAll). */
static char g_status[64] = "Active";
static char g_title[128] = "Probe Item";
static int g_flip;          /* 0: blue 32x32; 1: green 32x32          */
static int g_stop;
static fdk_dbus_connection *g_bus;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

/* ---- pixmap builders -------------------------------------------------------- */

/* Fills a byte array value with a WxH solid RGBA pixmap. */
static fdk_dbus_value *pixmap_bytes(int w, int h, uint8_t r, uint8_t g,
                                    uint8_t b) {
    size_t n = (size_t)w * (size_t)h * 4u;
    fdk_dbus_value **bytes = calloc(n, sizeof(*bytes));
    if (bytes == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i += 4) {
        bytes[i + 0] = fdk_dbus_value_new_byte(0xFF); /* A first: the */
        bytes[i + 1] = fdk_dbus_value_new_byte(r);    /* ARGB32 net   */
        bytes[i + 2] = fdk_dbus_value_new_byte(g);    /* byte order   */
        bytes[i + 3] = fdk_dbus_value_new_byte(b);
    }
    fdk_dbus_value *arr = fdk_dbus_value_new_array("y", bytes, n);
    if (arr == NULL) {
        /* failure: the array took nothing — the bytes are ours    */
        for (size_t i = 0; i < n; i++) {
            fdk_dbus_value_free(bytes[i]);
        }
    }
    free(bytes);
    return arr;
}

/* One (iiay) struct. Consumes nothing; returns a fresh value. */
static fdk_dbus_value *pixmap_struct(int w, int h, uint8_t r, uint8_t g,
                                     uint8_t b) {
    fdk_dbus_value *data = pixmap_bytes(w, h, r, g, b);
    if (data == NULL) {
        return NULL;
    }
    fdk_dbus_value *fields[3];
    fields[0] = fdk_dbus_value_new_int32(w);
    fields[1] = fdk_dbus_value_new_int32(h);
    fields[2] = data;
    fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 3);
    if (st == NULL) {
        /* fields (incl. data) are ours again on failure             */
        fdk_dbus_value_free(fields[0]);
        fdk_dbus_value_free(fields[1]);
        fdk_dbus_value_free(fields[2]);
    }
    return st;
}

/* The a(iiay) array for the current flip state. */
static fdk_dbus_value *icon_pixmap_array(void) {
    fdk_dbus_value *items[2];
    items[0] = pixmap_struct(16, 16, 255, 0, 0); /* the red 16       */
    items[1] = pixmap_struct(32, 32, g_flip ? 0 : 40, g_flip ? 190 : 90,
                             g_flip ? 60 : 210);
    if (items[0] == NULL || items[1] == NULL) {
        fdk_dbus_value_free(items[0]);
        fdk_dbus_value_free(items[1]);
        return NULL;
    }
    return fdk_dbus_value_new_array("(iiay)", items, 2);
}

/* The amber attention pixmap list. */
static fdk_dbus_value *attn_pixmap_array(void) {
    fdk_dbus_value *items[1];
    items[0] = pixmap_struct(32, 32, 230, 160, 20);
    if (items[0] == NULL) {
        return NULL;
    }
    return fdk_dbus_value_new_array("(iiay)", items, 1);
}

/* ---- a{sv} helpers (every value VARIANT-wrapped — the discipline) ---------- */

static fdk_dbus_value *sv_str(const char *key, const char *val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_string(val);
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = v != NULL ? fdk_dbus_value_new_dict_entry(k, v)
                                  : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
    }
    return e;
}

static fdk_dbus_value *sv_objpath(const char *key, const char *val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_object_path(val);
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = v != NULL ? fdk_dbus_value_new_dict_entry(k, v)
                                  : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
    }
    return e;
}

static fdk_dbus_value *sv_u32(const char *key, uint32_t val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_uint32(val);
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = v != NULL ? fdk_dbus_value_new_dict_entry(k, v)
                                  : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
    }
    return e;
}

static fdk_dbus_value *sv_bool(const char *key, int val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_boolean(val);
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = v != NULL ? fdk_dbus_value_new_dict_entry(k, v)
                                  : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
    }
    return e;
}

static fdk_dbus_value *sv_value(const char *key,
                                fdk_dbus_value *inner /* consumed */) {
    if (inner == NULL) {
        return NULL;
    }
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    if (k == NULL) {
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e = v != NULL ? fdk_dbus_value_new_dict_entry(k, v)
                                  : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
    }
    return e;
}

/* ---- the item's property set ---------------------------------------------- */

static void serve_getall(fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    fdk_dbus_value *entries[12];
    size_t n = 0;
    entries[n++] = sv_str("Id", "trayprobe-1");
    entries[n++] = sv_str("Title", g_title);
    entries[n++] = sv_str("Status", g_status);
    entries[n++] = sv_str("Category", "ApplicationStatus");
    entries[n++] = sv_u32("WindowId", 0);
    entries[n++] = sv_str("IconName", "");
    entries[n++] = sv_value("IconPixmap", icon_pixmap_array());
    entries[n++] = sv_value("AttentionIconPixmap", attn_pixmap_array());
    entries[n++] = sv_objpath("Menu", MENU_PATH);
    entries[n++] = sv_bool("ItemIsMenu", 0);

    /* The ToolTip (sa(iiay)(ss)ss). */
    {
        fdk_dbus_value *tt_fields[5];
        tt_fields[0] = fdk_dbus_value_new_string("probe");
        tt_fields[1] = fdk_dbus_value_new_array("(iiay)", NULL, 0);
        fdk_dbus_value *ss_fields[2];
        ss_fields[0] = fdk_dbus_value_new_string("");
        ss_fields[1] = fdk_dbus_value_new_string("");
        tt_fields[2] =
            fdk_dbus_value_new_struct(ss_fields, 2);
        if (tt_fields[2] == NULL) {
            fdk_dbus_value_free(ss_fields[0]);
            fdk_dbus_value_free(ss_fields[1]);
        }
        tt_fields[3] = fdk_dbus_value_new_string(g_title);
        tt_fields[4] = fdk_dbus_value_new_string("the probe item");
        fdk_dbus_value *tt = fdk_dbus_value_new_struct(tt_fields, 5);
        if (tt == NULL) {
            for (size_t i = 0; i < 5; i++) {
                fdk_dbus_value_free(tt_fields[i]);
            }
        }
        entries[n++] = sv_value("ToolTip", tt);
    }

    int ok = 1;
    for (size_t i = 0; i < n; i++) {
        if (entries[i] == NULL) {
            ok = 0;
        }
    }
    fdk_dbus_value *dict = NULL;
    if (ok) {
        dict = fdk_dbus_value_new_array("{sv}", entries, n);
    }
    if (dict != NULL && fdk_ok(fdk_dbus_message_append(rep, dict))) {
        fdk_dbus_value_free(dict);
        fdk_dbus_connection_send(g_bus, rep);
    } else {
        fdk_dbus_value_free(dict);
        fdk_dbus_message_free(rep);
    }
    logf_("GetAll served (%s)", g_status);
}

/* ---- the dbusmenu tree ------------------------------------------------------ */

/* One node's (ia{sv}av) struct. */
static fdk_dbus_value *menu_node_value(int32_t id, const char *label,
                                       const char *type, int enabled,
                                       int toggle,
                                       fdk_dbus_value *children_av) {
    fdk_dbus_value *props[8];
    size_t pn = 0;
    if (label != NULL) {
        props[pn++] = sv_str("label", label);
    }
    if (type != NULL) {
        props[pn++] = sv_str("type", type);
    }
    if (!enabled) {
        props[pn++] = sv_bool("enabled", 0);
    }
    if (toggle >= 0) {
        props[pn++] = sv_value(
            "toggle-state",
            fdk_dbus_value_new_int32(toggle));
    }
    fdk_dbus_value *props_arr =
        fdk_dbus_value_new_array("{sv}", props, pn);
    if (props_arr == NULL) {
        for (size_t i = 0; i < pn; i++) {
            fdk_dbus_value_free(props[i]);
        }
    }

    fdk_dbus_value *fields[3];
    fields[0] = fdk_dbus_value_new_int32(id);
    fields[1] = props_arr;
    fields[2] = children_av != NULL
                    ? children_av
                    : fdk_dbus_value_new_array("v", NULL, 0);
    fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 3);
    if (st == NULL) {
        fdk_dbus_value_free(fields[0]);
        fdk_dbus_value_free(fields[1]);
        fdk_dbus_value_free(fields[2]);
    }
    return st;
}

/* Wraps a node struct in a VARIANT (the av element shape). */
static fdk_dbus_value *menu_node_variant(fdk_dbus_value *node) {
    if (node == NULL) {
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(node);
    fdk_dbus_value_free(node); /* the variant keeps a clone         */
    return v;
}

static void serve_getlayout(fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }

    /* The submenu's children (as av variants). */
    fdk_dbus_value *sub_items[2];
    sub_items[0] = menu_node_variant(
        menu_node_value(8, "Nested one", NULL, 1, -1, NULL));
    sub_items[1] = menu_node_variant(
        menu_node_value(9, "Nested two", NULL, 1, -1, NULL));
    fdk_dbus_value *sub_av =
        fdk_dbus_value_new_array("v", sub_items, 2);
    if (sub_av == NULL) {
        fdk_dbus_value_free(sub_items[0]);
        fdk_dbus_value_free(sub_items[1]);
    }

    /* The root's children. */
    fdk_dbus_value *items[7];
    items[0] = menu_node_variant(
        menu_node_value(1, "Fi_rst item", NULL, 1, -1, NULL));
    items[1] = menu_node_variant(
        menu_node_value(2, NULL, "separator", 1, -1, NULL));
    items[2] = menu_node_variant(
        menu_node_value(3, "Check me", "checkmark", 1, 1, NULL));
    items[3] = menu_node_variant(
        menu_node_value(4, "Radio A", "radio", 1, 1, NULL));
    items[4] = menu_node_variant(
        menu_node_value(5, "Radio B", "radio", 1, 0, NULL));
    items[5] = menu_node_variant(
        menu_node_value(6, "Disabled", NULL, 0, -1, NULL));
    items[6] = menu_node_variant(
        menu_node_value(7, "Submenu", NULL, 1, -1, sub_av));
    /* sub_av was CONSUMED by the node's struct (or freed on that
     * failure path) — nothing left to free here. */
    fdk_dbus_value *root_av = fdk_dbus_value_new_array("v", items, 7);
    if (root_av == NULL) {
        for (size_t i = 0; i < 7; i++) {
            fdk_dbus_value_free(items[i]);
        }
    }

    /* The root node (id 0, no properties). */
    fdk_dbus_value *root = menu_node_value(0, NULL, NULL, 1, -1, root_av);
    /* root_av was CONSUMED by the root struct (or freed on failure). */

    /* TWO out-arguments: u revision, then the BARE struct. */
    if (root != NULL &&
        fdk_ok(fdk_dbus_message_append_uint32(rep, 3)) &&
        fdk_ok(fdk_dbus_message_append(rep, root))) {
        fdk_dbus_value_free(root);
        fdk_dbus_connection_send(g_bus, rep);
    } else {
        fdk_dbus_value_free(root);
        fdk_dbus_message_free(rep);
    }
    logf_("GetLayout served rev=3");
}

static void serve_abouttoshow(fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    int32_t id = 0;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n == 1 &&
        args != NULL && fdk_dbus_value_type(args[0]) == FDK_DBUS_INT32) {
        id = fdk_dbus_value_int32(args[0]);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep != NULL &&
        fdk_ok(fdk_dbus_message_append_boolean(rep, 0))) {
        fdk_dbus_connection_send(g_bus, rep);
    } else if (rep != NULL) {
        fdk_dbus_message_free(rep);
    }
    logf_("AboutToShow id=%d", id);
}

static void serve_event(fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    int32_t id = 0;
    const char *event = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n >= 2 &&
        args != NULL &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_INT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        id = fdk_dbus_value_int32(args[0]);
        event = fdk_dbus_value_string(args[1]);
    }
    char ev[64] = "?";
    if (event != NULL) {
        snprintf(ev, sizeof(ev), "%s", event);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    logf_("Event id=%d %s", id, ev);
    /* NO_REPLY_EXPECTED is set by the caller; no reply either way. */
}

/* ---- item activation --------------------------------------------------------- */

static void serve_activate(fdk_dbus_message *call, const char *what) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    int32_t x = 0, y = 0;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n == 2 &&
        args != NULL &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_INT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_INT32) {
        x = fdk_dbus_value_int32(args[0]);
        y = fdk_dbus_value_int32(args[1]);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    logf_("%s x=%d y=%d", what, x, y);
    if (!(fdk_dbus_message_flags(call) & FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_connection_send(g_bus, rep);
        }
    }
}

/* ---- the control interface --------------------------------------------------- */

static void emit_item_signal(const char *member, const char *arg) {
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        ITEM_PATH_KDE, ITEM_IFACE, member);
    if (sig == NULL) {
        return;
    }
    int ok = 1;
    if (arg != NULL) {
        ok = fdk_ok(fdk_dbus_message_append_string(sig, arg));
    }
    if (ok) {
        fdk_dbus_connection_send(g_bus, sig);
    } else {
        fdk_dbus_message_free(sig);
    }
    logf_("signal %s%s%s", member, arg ? " " : "", arg ? arg : "");
}

static void serve_control(fdk_dbus_message *call) {
    const char *member = fdk_dbus_message_member(call);
    if (strcmp(member, "SetStatus") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
            /* copy BEFORE the values array frees (borrowed string) */
            snprintf(g_status, sizeof(g_status), "%s",
                     fdk_dbus_value_string(args[0]));
            fdk_dbus_values_free_array(args, n);
            emit_item_signal("NewStatus", g_status);
        } else if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    } else if (strcmp(member, "Retitle") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
            snprintf(g_title, sizeof(g_title), "%s",
                     fdk_dbus_value_string(args[0]));
            fdk_dbus_values_free_array(args, n);
            emit_item_signal("NewTitle", NULL);
        } else if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    } else if (strcmp(member, "FlipIcon") == 0) {
        g_flip = !g_flip;
        emit_item_signal("NewIcon", NULL);
    } else if (strcmp(member, "Quit") == 0) {
        g_stop = 1;
    }
    if (!(fdk_dbus_message_flags(call) &
          FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_connection_send(g_bus, rep);
        }
    }
}

/* ---- the dispatcher ----------------------------------------------------------- */

static void probe_method(fdk_dbus_connection *bus, fdk_dbus_message *call,
                         void *user) {
    (void)bus;
    (void)user;
    const char *path = fdk_dbus_message_path(call);
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (path == NULL || iface == NULL || member == NULL) {
        return;
    }

    if (strcmp(path, ITEM_PATH_KDE) == 0 ||
        strcmp(path, ITEM_PATH_AAT) == 0) {
        if (strcmp(iface, PROPS_IFACE) == 0 &&
            strcmp(member, "GetAll") == 0) {
            serve_getall(call);
            return;
        }
        if (strcmp(iface, ITEM_IFACE) == 0) {
            if (strcmp(member, "Activate") == 0) {
                serve_activate(call, "Activate");
                return;
            }
            if (strcmp(member, "SecondaryActivate") == 0) {
                serve_activate(call, "SecondaryActivate");
                return;
            }
            if (strcmp(member, "Scroll") == 0) {
                logf_("Scroll");
                return;
            }
        }
    }
    if (strcmp(path, MENU_PATH) == 0 &&
        strcmp(iface, MENU_IFACE) == 0) {
        if (strcmp(member, "GetLayout") == 0) {
            serve_getlayout(call);
            return;
        }
        if (strcmp(member, "AboutToShow") == 0) {
            serve_abouttoshow(call);
            return;
        }
        if (strcmp(member, "Event") == 0) {
            serve_event(call);
            return;
        }
    }
    if (strcmp(path, CONTROL_PATH) == 0 &&
        strcmp(iface, CONTROL_IFACE) == 0) {
        serve_control(call);
        return;
    }
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod",
        "the probe does not serve this");
    if (err != NULL) {
        fdk_dbus_connection_send(g_bus, err);
    }
}

/* ---- registration ------------------------------------------------------------ */

static void register_reply(fdk_dbus_connection *bus,
                           fdk_dbus_message *reply, void *user) {
    (void)bus;
    (void)user;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        logf_("register FAILED");
        g_stop = 1;
        return;
    }
    logf_("registered");
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int g_squat_stop;

static void squat_name_event(fdk_dbus_connection *bus, const char *name,
                             int owned, void *user) {
    (void)user;
    if (owned) {
        logf_("squatting %s as %s", name,
              fdk_dbus_connection_unique_name(bus));
    } else {
        logf_("lost %s", name);
        g_squat_stop = 1;
    }
}

/* squat [seconds] — holds org.kde.StatusNotifierWatcher with flags 0
 * (NO allow-replacement): the etiquette legs' refusing incumbent. */
static int run_squat(fdk_context *ctx, double lifetime) {
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (bus == NULL) {
        fprintf(stderr, "probe: no session bus\n");
        return 1;
    }
    g_bus = bus;
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(bus)) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    fdk_dbus_connection_set_name_events(bus, squat_name_event, NULL);
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, W_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, 0))) {
        fdk_dbus_message_free(call);
        fdk_dbus_connection_close(bus);
        return 1;
    }
    if (fdk_dbus_connection_call(bus, call, 3000, NULL, NULL) == NULL) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    double until = now_s() + lifetime;
    while (!g_squat_stop && now_s() < until) {
        fdk_pump_events(ctx, 100);
    }
    logf_("probe: squat done");
    fdk_dbus_connection_close(bus);
    return 0;
}

int main(int argc, char **argv) {
    /* usage: probe [kde|ayatana] [seconds] | squat [seconds] */
    if (argc > 1 && strcmp(argv[1], "squat") == 0) {
        fdk_context *ctx = NULL;
        if (!fdk_ok(fdk_init(&ctx, NULL))) {
            fprintf(stderr, "probe: no display\n");
            return 1;
        }
        int rc = run_squat(ctx, argc > 2 ? strtod(argv[2], NULL) : 20.0);
        fdk_shutdown(ctx);
        return rc;
    }
    int ayatana = 0;
    double lifetime = 30.0;
    if (argc > 1 && strcmp(argv[1], "ayatana") == 0) {
        ayatana = 1;
    }
    if (argc > 2) {
        lifetime = strtod(argv[2], NULL);
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "probe: no display\n");
        return 1;
    }
    g_bus = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (g_bus == NULL) {
        fprintf(stderr, "probe: no session bus\n");
        fdk_shutdown(ctx);
        return 1;
    }
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(g_bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(g_bus)) {
        fprintf(stderr, "probe: bus never became ready\n");
        fdk_dbus_connection_close(g_bus);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_dbus_connection_set_method_handler(g_bus, probe_method, NULL);

    /* The control channel's well-known name: the rig's gdbus calls
     * need a DESTINATION, and the unique name is unknowable outside
     * this process. Plain ownership (flags 0); the rig is the only
     * caller that ever targets it. */
    {
        fdk_dbus_message *wnc = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName");
        if (wnc != NULL &&
            fdk_ok(fdk_dbus_message_append_string(wnc, PROBE_WKN)) &&
            fdk_ok(fdk_dbus_message_append_uint32(wnc, 0))) {
            fdk_dbus_pending *wp =
                fdk_dbus_connection_call(g_bus, wnc, 3000, NULL, NULL);
            (void)wp;
        } else {
            fdk_dbus_message_free(wnc);
        }
    }

    /* The registration (the dialect the rig asked for). */
    const char *me = fdk_dbus_connection_unique_name(g_bus);
    char service[300];
    if (ayatana) {
        snprintf(service, sizeof(service), "%s", ITEM_PATH_AAT);
    } else {
        snprintf(service, sizeof(service), "%s", me);
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        W_NAME, W_PATH, W_IFACE, "RegisterStatusNotifierItem");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, service))) {
        fdk_dbus_message_free(call);
        fdk_dbus_connection_close(g_bus);
        fdk_shutdown(ctx);
        return 1;
    }
    logf_("probe: registering as %s (%s dialect)", service,
          ayatana ? "ayatana" : "kde");
    if (fdk_dbus_connection_call(g_bus, call, 3000, register_reply,
                                 NULL) == NULL) {
        fdk_dbus_connection_close(g_bus);
        fdk_shutdown(ctx);
        return 1;
    }

    double until = now_s() + lifetime;
    while (!g_stop && now_s() < until) {
        fdk_pump_events(ctx, 100);
    }
    logf_("probe: done");
    fdk_dbus_connection_close(g_bus);
    fdk_shutdown(ctx);
    return 0;
}
