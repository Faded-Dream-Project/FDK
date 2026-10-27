/*
 * dbusmenu_server.c — the com.canonical.dbusmenu server model (1.1.0)
 *
 * The menu half of the app-facing tray API: the tree model the app
 * builds (fdk_tray.h) plus the server that answers the host's five
 * core calls — GetLayout (the two-out-arg nested struct), GetGroup-
 * Properties, GetProperty, AboutToShow (the dirty-flag handshake),
 * Event (the click dispatch) — and emits LayoutUpdated on every
 * mutation. The EventGroup/AboutToShowGroup pair is deliberately
 * refused with UnknownMethod: they are the specification's optional
 * bulk forms, FDK's own trayhost never calls them, and an honest
 * refusal beats a guessed signature.
 *
 * The node table: nodes[i] carries id i+1, and the array order IS
 * the sibling order (insertion; removal compacts without swap, so
 * order never jumps). The root is id 0 — implicit, always present,
 * and never in the table.
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

/* ---- the model ------------------------------------------------------------- */

static menu_node *node_by_id(fdk_tray_menu *m, uint32_t id,
                             size_t *index_out) {
    if (m == NULL || id == 0 || id > m->n) {
        return NULL;
    }
    if (index_out != NULL) {
        *index_out = (size_t)(id - 1);
    }
    return &m->nodes[id - 1];
}

static void mark_dirty_chain(fdk_tray_menu *m, uint32_t parent) {
    /* The parent's subtree changed: its AboutToShow answer flips. */
    menu_node *p = node_by_id(m, parent, NULL);
    if (p != NULL) {
        p->dirty = 1;
    }
    if (parent != 0) {
        mark_dirty_chain(m, p != NULL ? p->parent : 0);
    }
}

static void bump(fdk_tray_menu *m, uint32_t parent) {
    if (m == NULL) {
        return;
    }
    m->revision++;
    mark_dirty_chain(m, parent);
    fdk__tray_menu_emit_updated(m);
}

fdk_tray_menu *fdk_tray_menu_create(void) {
    fdk_tray_menu *m = fdk_alloc(sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    return m;
}

static void menu_detach(fdk_tray_menu *m) {
    if (m != NULL && m->item != NULL) {
        fdk_tray_item *owner = m->item;
        m->item = NULL;
        if (owner->menu == m) {
            owner->menu = NULL;
        }
    }
}

void fdk_tray_menu_close(fdk_tray_menu *menu) {
    if (menu == NULL) {
        return;
    }
    menu_detach(menu);
    for (size_t i = 0; i < menu->n; i++) {
        fdk_free(menu->nodes[i].label);
    }
    menu->n = 0;
    fdk_free(menu->nodes);
    menu->nodes = NULL;
    menu->cap = 0;
    fdk_free(menu);
}

static uint32_t menu_append(fdk_tray_menu *m, uint32_t parent,
                            int separator, const char *label) {
    if (m == NULL) {
        return 0;
    }
    if (parent != 0 && node_by_id(m, parent, NULL) == NULL) {
        return 0; /* bad parent (0 IS the root: always valid) */
    }
    if (m->n == m->cap) {
        size_t cap = m->cap == 0 ? 8 : m->cap * 2;
        menu_node *grown = fdk_alloc_array(cap, sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        if (m->n > 0) {
            memcpy(grown, m->nodes, m->n * sizeof(*grown));
        }
        fdk_free(m->nodes);
        m->nodes = grown;
        m->cap = cap;
    }
    menu_node *nd = &m->nodes[m->n];
    memset(nd, 0, sizeof(*nd));
    nd->parent = parent;
    nd->separator = separator ? 1 : 0;
    nd->enabled = 1;
    nd->visible = 1;
    if (!separator) {
        nd->label = fdk__tray_strdup(label != NULL ? label : "");
        if (nd->label == NULL) {
            return 0;
        }
    }
    m->n++;
    uint32_t id = (uint32_t)m->n;
    bump(m, parent);
    return id;
}

uint32_t fdk_tray_menu_add(fdk_tray_menu *menu, uint32_t parent_id,
                           const char *label) {
    return menu_append(menu, parent_id, 0, label);
}

uint32_t fdk_tray_menu_add_separator(fdk_tray_menu *menu,
                                     uint32_t parent_id) {
    return menu_append(menu, parent_id, 1, NULL);
}

void fdk_tray_menu_remove(fdk_tray_menu *menu, uint32_t id) {
    if (menu == NULL || id == 0 || id > menu->n) {
        return;
    }
    menu_node *nd = &menu->nodes[id - 1];
    uint32_t parent = nd->parent;
    /* Collect the subtree (the node + descendants), then compact.
     * Marking then compacting in one pass keeps sibling order. */
    char *drop = fdk_alloc_array(menu->n, 1);
    if (drop == NULL) {
        return; /* honest refusal: no memory, nothing corrupted */
    }
    drop[id - 1] = 1;
    int changed = 1;
    while (changed) { /* fixpoint: children of dropped are dropped */
        changed = 0;
        for (size_t i = 0; i < menu->n; i++) {
            if (!drop[i] && menu->nodes[i].parent != 0 &&
                drop[menu->nodes[i].parent - 1]) {
                drop[i] = 1;
                changed = 1;
            }
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < menu->n; i++) {
        if (!drop[i]) {
            menu->nodes[w++] = menu->nodes[i];
        } else {
            fdk_free(menu->nodes[i].label);
        }
    }
    menu->n = w;
    fdk_free(drop);
    /* Ids above the removed range SHIFTED — that is the documented
     * consequence of a compacting table; hosts re-fetch the whole
     * tree on LayoutUpdated, so nothing on the wire goes stale. */
    bump(menu, parent);
}

void fdk_tray_menu_clear(fdk_tray_menu *menu) {
    if (menu == NULL) {
        return;
    }
    for (size_t i = 0; i < menu->n; i++) {
        fdk_free(menu->nodes[i].label);
    }
    menu->n = 0;
    bump(menu, 0);
}

void fdk_tray_menu_set_label(fdk_tray_menu *menu, uint32_t id,
                             const char *label) {
    menu_node *nd = node_by_id(menu, id, NULL);
    if (nd == NULL || label == NULL) {
        return;
    }
    char *copy = fdk__tray_strdup(label);
    if (copy == NULL) {
        return;
    }
    fdk_free(nd->label);
    nd->label = copy;
    bump(menu, nd->parent);
}

void fdk_tray_menu_set_enabled(fdk_tray_menu *menu, uint32_t id,
                               int enabled) {
    menu_node *nd = node_by_id(menu, id, NULL);
    if (nd == NULL) {
        return;
    }
    nd->enabled = enabled ? 1 : 0;
    bump(menu, nd->parent);
}

void fdk_tray_menu_set_check(fdk_tray_menu *menu, uint32_t id,
                             fdk_tray_menu_check kind, int state) {
    menu_node *nd = node_by_id(menu, id, NULL);
    if (nd == NULL) {
        return;
    }
    if (kind != FDK_TRAY_MENU_CHECK_NONE && state) {
        /* Radio semantics: checking one clears the RADIO siblings
         * of the same parent (the standard toolkit behavior). */
        if (kind == FDK_TRAY_MENU_CHECK_RADIO) {
            for (size_t i = 0; i < menu->n; i++) {
                if (i != (size_t)(id - 1) &&
                    menu->nodes[i].parent == nd->parent &&
                    menu->nodes[i].check_kind ==
                        FDK_TRAY_MENU_CHECK_RADIO) {
                    menu->nodes[i].toggle = 0;
                }
            }
        }
        nd->toggle = 1;
    } else {
        nd->toggle = kind == FDK_TRAY_MENU_CHECK_NONE ? 0 : (state ? 1 : 0);
    }
    nd->check_kind = (int)kind;
    bump(menu, nd->parent);
}

void fdk_tray_menu_on_click(fdk_tray_menu *menu,
                            fdk_tray_menu_click_fn fn, void *user) {
    if (menu != NULL) {
        menu->click_fn = fn;
        menu->click_user = user;
    }
}

void fdk_tray_menu_on_about_to_show(fdk_tray_menu *menu,
                                    fdk_tray_menu_show_fn fn, void *user) {
    if (menu != NULL) {
        menu->show_fn = fn;
        menu->show_user = user;
    }
}

uint32_t fdk_tray_menu_revision(const fdk_tray_menu *menu) {
    return menu != NULL ? menu->revision : 0;
}

size_t fdk_tray_menu_node_count(const fdk_tray_menu *menu) {
    return menu != NULL ? menu->n : 0;
}

/* ---- LayoutUpdated ---------------------------------------------------------- */

void fdk__tray_menu_emit_updated(fdk_tray_menu *menu) {
    if (menu == NULL || menu->item == NULL || menu->item->bus == NULL) {
        return;
    }
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        "/MenuBar", "com.canonical.dbusmenu", "LayoutUpdated");
    if (sig == NULL) {
        return;
    }
    if (fdk_ok(fdk_dbus_message_append_uint32(sig, menu->revision)) &&
        fdk_ok(fdk_dbus_message_append_uint32(sig, 0u))) {
        send_or_free(menu->item->bus, sig);
    } else {
        fdk_dbus_message_free(sig);
    }
}

/* ---- property marshaling ---------------------------------------------------- */

/* The property filter: NULL/empty = everything the node carries. */
static int wants(const char *const *names, size_t n, const char *key) {
    if (names == NULL || n == 0) {
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (names[i] != NULL && strcmp(names[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* One a{sv} node-props dict. Ownership: takes its children; on
 * failure every leftover is freed here. */
static fdk_dbus_value *node_props(fdk_tray_menu *m, size_t idx,
                                  const char *const *names, size_t nn) {
    menu_node *nd = &m->nodes[idx];
    fdk_dbus_value *entries[8];
    size_t n = 0;
    int failed = 0;

#define TRY(label, inner_expr)                                                \
    do {                                                                      \
        if (!failed && wants(names, nn, label)) {                              \
            fdk_dbus_value *inner = (inner_expr);                              \
            if (inner == NULL) {                                               \
                failed = 1;                                                    \
            } else {                                                           \
                fdk_dbus_value *k = fdk_dbus_value_new_string(label);          \
                fdk_dbus_value *var = fdk_dbus_value_new_variant(inner);       \
                fdk_dbus_value_free(inner); /* the variant clones */            \
                if (k == NULL || var == NULL) {                                \
                    fdk_dbus_value_free(k);                                    \
                    fdk_dbus_value_free(var);                                  \
                    failed = 1;                                                \
                } else {                                                       \
                    fdk_dbus_value *e =                                         \
                        fdk_dbus_value_new_dict_entry(k, var);                  \
                    if (e == NULL) {                                           \
                        fdk_dbus_value_free(k);                                \
                        fdk_dbus_value_free(var);                              \
                        failed = 1;                                            \
                    } else {                                                   \
                        entries[n++] = e;                                      \
                    }                                                          \
                }                                                              \
            }                                                                  \
        }                                                                      \
    } while (0)

    if (nd->separator) {
        TRY("type", fdk_dbus_value_new_string("separator"));
    } else {
        TRY("label",
            fdk_dbus_value_new_string(nd->label != NULL ? nd->label : ""));
        if (!nd->enabled) {
            TRY("enabled", fdk_dbus_value_new_boolean(0));
        }
        if (!nd->visible) {
            TRY("visible", fdk_dbus_value_new_boolean(0));
        }
        if (nd->check_kind == FDK_TRAY_MENU_CHECK_CHECK) {
            TRY("toggle-state", fdk_dbus_value_new_int32(nd->toggle));
        } else if (nd->check_kind == FDK_TRAY_MENU_CHECK_RADIO) {
            TRY("toggle-state", fdk_dbus_value_new_int32(nd->toggle));
            TRY("toggle-group",
                fdk_dbus_value_new_string("")); /* the group is positional */
        }
    }
    /* children-display when this node HAS children. */
    for (size_t i = 0; i < m->n && !failed; i++) {
        if (m->nodes[i].parent == idx + 1) {
            TRY("children-display", fdk_dbus_value_new_string("submenu"));
            break;
        }
    }
#undef TRY

    if (failed) {
        for (size_t i = 0; i < n; i++) {
            fdk_dbus_value_free(entries[i]);
        }
        return NULL;
    }
    return fdk_dbus_value_new_array("{sv}", entries, n);
}

/* One (ia{sv}av) node value. depth: -1 unlimited, 0 childless. */
static fdk_dbus_value *node_value(fdk_tray_menu *m, size_t idx, int depth,
                                  const char *const *names, size_t nn) {
    /* children (av): each child struct VARIANT-wrapped. */
    fdk_dbus_value **kids = NULL;
    size_t nk = 0;
    if (depth != 0) {
        kids = fdk_alloc_array(m->n, sizeof(*kids));
        if (kids == NULL && m->n > 0) {
            return NULL;
        }
        for (size_t i = 0; i < m->n; i++) {
            if (m->nodes[i].parent != idx + 1) {
                continue;
            }
            fdk_dbus_value *child =
                node_value(m, i, depth > 0 ? depth - 1 : depth, names, nn);
            if (child == NULL) {
                goto fail_kids;
            }
            fdk_dbus_value *wrapped = fdk_dbus_value_new_variant(child);
            fdk_dbus_value_free(child); /* the variant clones */
            if (wrapped == NULL) {
                goto fail_kids;
            }
            kids[nk++] = wrapped;
        }
    }
    {
        fdk_dbus_value *children =
            fdk_dbus_value_new_array("v", kids, nk);
        if (children == NULL) {
            goto fail_kids; /* the array left the kids with us */
        }
        fdk_dbus_value *props = node_props(m, idx, names, nn);
        if (props == NULL) {
            fdk_dbus_value_free(children);
            goto free_ptr; /* children already released the kids */
        }
        fdk_dbus_value *id_v = fdk_dbus_value_new_int32((int32_t)(idx + 1));
        if (id_v == NULL) {
            fdk_dbus_value_free(props);
            fdk_dbus_value_free(children);
            goto free_ptr;
        }
        fdk_dbus_value *fields[3] = {id_v, props, children};
        fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 3);
        if (st == NULL) {
            fdk_dbus_value_free(id_v);
            fdk_dbus_value_free(props);
            fdk_dbus_value_free(children);
            goto free_ptr;
        }
        /* Success: the struct owns id/props/children, and children
         * owns every kid — only the POINTER array was ours. */
        fdk_free(kids);
        return st;
    }
free_ptr:
    fdk_free(kids);
    return NULL;
fail_kids:
    for (size_t i = 0; i < nk; i++) {
        fdk_dbus_value_free(kids[i]);
    }
    fdk_free(kids);
    return NULL;
}

/* ---- the five served methods ------------------------------------------------ */

static void serve_get_layout(fdk_tray_menu *m, fdk_dbus_connection *bus,
                             const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    uint32_t parent = 0;
    int32_t depth = -1;
    /* The property-name filter: borrowed string pointers that live
     * in the args tree — everything is read before the args free. */
    const char **names = NULL;
    size_t nn = 0;
    if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 3 &&
        args != NULL && fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_INT32 &&
        fdk_dbus_value_type(args[2]) == FDK_DBUS_ARRAY) {
        parent = fdk_dbus_value_uint32(args[0]);
        depth = fdk_dbus_value_int32(args[1]);
        const fdk_dbus_value *as = args[2];
        nn = fdk_dbus_value_array_count(as);
        if (nn > 0) {
            names = fdk_alloc_array(nn, sizeof(char *));
            if (names != NULL) {
                for (size_t i = 0; i < nn; i++) {
                    names[i] = fdk_dbus_value_string(
                        fdk_dbus_value_array_item(as, i));
                }
            }
        }
    }
    fdk_dbus_message *err = NULL;
    if (args == NULL || n != 3) {
        err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "GetLayout expects (u i as)");
    } else if (nn > 0 && names == NULL) {
        err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.NoMemory", NULL);
    }
    if (err != NULL) {
        fdk_dbus_values_free_array(args, n);
        send_or_free(bus, err);
        return;
    }

    /* Build the root node value (parent 0 → the implicit root). */
    fdk_dbus_value *root = NULL;
    if (parent == 0) {
        /* The implicit root: id 0, empty props, all top-level rows
         * as children. Hand-built — it is not in the table. */
        fdk_dbus_value **kids = fdk_alloc_array(m->n > 0 ? m->n : 1,
                                                sizeof(*kids));
        size_t nk = 0;
        if (kids == NULL) {
            fdk_dbus_values_free_array(args, n);
            fdk_free(names);
            err = fdk_dbus_message_new_error(
                call, "org.freedesktop.DBus.Error.NoMemory", NULL);
            send_or_free(bus, err);
            return;
        }
        int kid_failed = 0;
        for (size_t i = 0; i < m->n; i++) {
            if (m->nodes[i].parent != 0) {
                continue;
            }
            fdk_dbus_value *child = node_value(m, i, depth, names, nn);
            if (child == NULL) {
                kid_failed = 1;
                break;
            }
            fdk_dbus_value *wrapped = fdk_dbus_value_new_variant(child);
            fdk_dbus_value_free(child);
            if (wrapped == NULL) {
                kid_failed = 1;
                break;
            }
            kids[nk++] = wrapped;
        }
        if (!kid_failed) {
            fdk_dbus_value *children =
                fdk_dbus_value_new_array("v", kids, nk);
            fdk_dbus_value *empty_props =
                fdk_dbus_value_new_array("{sv}", NULL, 0);
            fdk_dbus_value *id_v = fdk_dbus_value_new_int32(0);
            if (children != NULL && empty_props != NULL && id_v != NULL) {
                fdk_dbus_value *fields[3] = {id_v, empty_props, children};
                root = fdk_dbus_value_new_struct(fields, 3);
                if (root == NULL) {
                    fdk_dbus_value_free(id_v);
                    fdk_dbus_value_free(empty_props);
                    fdk_dbus_value_free(children);
                }
            } else {
                fdk_dbus_value_free(id_v);
                fdk_dbus_value_free(empty_props);
                fdk_dbus_value_free(children);
            }
        } else {
            for (size_t i = 0; i < nk; i++) {
                fdk_dbus_value_free(kids[i]); /* array not built: ours */
            }
        }
        fdk_free(kids);
    } else {
        size_t idx;
        if (node_by_id(m, parent, &idx) != NULL) {
            root = node_value(m, idx, depth, names, nn);
        }
    }
    fdk_dbus_values_free_array(args, n);
    fdk_free(names);

    if (root == NULL) {
        err = fdk_dbus_message_new_error(
            call, "com.canonical.dbusmenu.Error.NoSuchObject",
            "no such menu node");
        send_or_free(bus, err);
        return;
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL ||
        !fdk_ok(fdk_dbus_message_append_uint32(rep, m->revision)) ||
        !fdk_ok(fdk_dbus_message_append(rep, root))) {
        fdk_dbus_message_free(rep);
        fdk_dbus_value_free(root);
        return;
    }
    fdk_dbus_value_free(root);
    send_or_free(bus, rep);
}

static void serve_get_group_properties(fdk_tray_menu *m,
                                       fdk_dbus_connection *bus,
                                       const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) || n != 2) {
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "GetGroupProperties expects (au as)");
        send_or_free(bus, err);
        return;
    }
    const fdk_dbus_value *ids = args[0];
    size_t nid = fdk_dbus_value_array_count(ids);
    fdk_dbus_value **items = fdk_alloc_array(nid > 0 ? nid : 1,
                                             sizeof(*items));
    size_t ni = 0;
    if (items == NULL) {
        fdk_dbus_values_free_array(args, n);
        return;
    }
    for (size_t i = 0; i < nid; i++) {
        uint32_t id = fdk_dbus_value_uint32(
            fdk_dbus_value_array_item(ids, i));
        size_t idx;
        if (node_by_id(m, id, &idx) == NULL) {
            continue;
        }
        fdk_dbus_value *props = node_props(m, idx, NULL, 0);
        fdk_dbus_value *id_v = fdk_dbus_value_new_int32((int32_t)id);
        if (props == NULL || id_v == NULL) {
            fdk_dbus_value_free(props);
            fdk_dbus_value_free(id_v);
            continue;
        }
        fdk_dbus_value *fields[2] = {id_v, props};
        fdk_dbus_value *st = fdk_dbus_value_new_struct(fields, 2);
        if (st == NULL) {
            fdk_dbus_value_free(id_v);
            fdk_dbus_value_free(props);
            continue;
        }
        items[ni++] = st;
    }
    fdk_dbus_values_free_array(args, n);
    fdk_dbus_value *arr = fdk_dbus_value_new_array("(ia{sv})", items, ni);
    if (arr == NULL) {
        for (size_t i = 0; i < ni; i++) {
            fdk_dbus_value_free(items[i]);
        }
    }
    fdk_free(items);
    if (arr == NULL) {
        return;
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep != NULL && fdk_ok(fdk_dbus_message_append(rep, arr))) {
        send_or_free(bus, rep);
    } else {
        fdk_dbus_message_free(rep);
    }
    fdk_dbus_value_free(arr);
}

static void serve_get_property(fdk_tray_menu *m, fdk_dbus_connection *bus,
                               const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (!fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) || n != 2 ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_UINT32 ||
        fdk_dbus_value_type(args[1]) != FDK_DBUS_STRING) {
        fdk_dbus_values_free_array(args, n);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "GetProperty expects (u s)");
        send_or_free(bus, err);
        return;
    }
    uint32_t id = fdk_dbus_value_uint32(args[0]);
    char *name = fdk__tray_strdup(fdk_dbus_value_string(args[1]));
    fdk_dbus_values_free_array(args, n);
    size_t idx;
    if (name == NULL || node_by_id(m, id, &idx) == NULL) {
        fdk_free(name);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "com.canonical.dbusmenu.Error.NoSuchObject", NULL);
        send_or_free(bus, err);
        return;
    }
    /* Reuse node_props with a one-name filter. */
    const char *const names[1] = {name};
    fdk_dbus_value *props = node_props(m, idx, names, 1);
    fdk_free(name);
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        fdk_dbus_value_free(props);
        return;
    }
    if (props == NULL ||
        fdk_dbus_value_array_count(props) != 1) {
        fdk_dbus_value_free(props);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "com.canonical.dbusmenu.Error.NoSuchProperty",
            "no such property on this node");
        send_or_free(bus, err);
        fdk_dbus_message_free(rep);
        return;
    }
    /* The entry's variant value is the reply. */
    const fdk_dbus_value *entry = fdk_dbus_value_array_item(props, 0);
    const fdk_dbus_value *var = fdk_dbus_value_dict_value(entry);
    if (fdk_ok(fdk_dbus_message_append(rep, var))) {
        send_or_free(bus, rep);
    } else {
        fdk_dbus_message_free(rep);
    }
    fdk_dbus_value_free(props);
}

static void serve_about_to_show(fdk_tray_menu *m, fdk_dbus_connection *bus,
                                const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    uint32_t id = 0;
    if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 1) {
        id = fdk_dbus_value_uint32(args[0]);
    }
    fdk_dbus_values_free_array(args, n);
    menu_node *nd = node_by_id(m, id, NULL);
    int dirty = 0;
    if (id == 0) {
        /* The root: dirty when the revision moved since the last
         * ask (mutations always reach the root's answer). */
        dirty = m->root_asked_rev != m->revision;
        m->root_asked_rev = m->revision;
    } else if (nd != NULL) {
        dirty = nd->dirty;
        nd->dirty = 0;
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep != NULL && fdk_ok(fdk_dbus_message_append_boolean(rep, dirty))) {
        send_or_free(bus, rep);
    } else {
        fdk_dbus_message_free(rep);
    }
    if (m->show_fn != NULL) {
        m->show_fn(m, id, m->show_user);
    }
}

static void serve_event(fdk_tray_menu *m, fdk_dbus_connection *bus,
                        const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    uint32_t id = 0;
    char *event_id = NULL;
    if (fdk_ok(fdk_dbus_message_read_args(call, &args, &n)) && n == 4 &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        id = fdk_dbus_value_uint32(args[0]);
        event_id = fdk__tray_strdup(fdk_dbus_value_string(args[1]));
    }
    fdk_dbus_values_free_array(args, n);
    if (!(fdk_dbus_message_flags(call) & FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            send_or_free(bus, rep);
        }
    }
    if (event_id != NULL && m->click_fn != NULL &&
        strcmp(event_id, "clicked") == 0 && id != 0 &&
        node_by_id(m, id, NULL) != NULL) {
        m->click_fn(m, id, m->click_user);
    }
    fdk_free(event_id);
}

/* ---- the dispatcher --------------------------------------------------------- */

const char *fdk__tray_menu_introspect_xml(void) {
    return "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
           "Introspection 1.0//EN\" "
           "\"http://www.freedesktop.org/standards/dbus/1.0/"
           "introspect.dtd\">\n"
           "<node>\n"
           "  <interface name=\"com.canonical.dbusmenu\">\n"
           "    <method name=\"GetLayout\">\n"
           "      <arg name=\"parentId\" type=\"u\" direction=\"in\"/>\n"
           "      <arg name=\"recursionDepth\" type=\"i\" "
           "direction=\"in\"/>\n"
           "      <arg name=\"propertyNames\" type=\"as\" "
           "direction=\"in\"/>\n"
           "      <arg name=\"revision\" type=\"u\" direction=\"out\"/>\n"
           "      <arg name=\"layout\" type=\"(ia{sv}av)\" "
           "direction=\"out\"/>\n"
           "    </method>\n"
           "    <method name=\"GetGroupProperties\">\n"
           "      <arg name=\"ids\" type=\"au\" direction=\"in\"/>\n"
           "      <arg name=\"propertyNames\" type=\"as\" "
           "direction=\"in\"/>\n"
           "      <arg name=\"properties\" type=\"a(ia{sv})\" "
           "direction=\"out\"/>\n"
           "    </method>\n"
           "    <method name=\"GetProperty\">\n"
           "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
           "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
           "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
           "    </method>\n"
           "    <method name=\"AboutToShow\">\n"
           "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
           "      <arg name=\"needsclose\" type=\"b\" "
           "direction=\"out\"/>\n"
           "    </method>\n"
           "    <method name=\"Event\">\n"
           "      <arg name=\"id\" type=\"u\" direction=\"in\"/>\n"
           "      <arg name=\"eventId\" type=\"s\" direction=\"in\"/>\n"
           "      <arg name=\"data\" type=\"v\" direction=\"in\"/>\n"
           "      <arg name=\"timestamp\" type=\"u\" direction=\"in\"/>\n"
           "    </method>\n"
           "    <signal name=\"LayoutUpdated\">\n"
           "      <arg name=\"revision\" type=\"u\"/>\n"
           "      <arg name=\"parent\" type=\"u\"/>\n"
           "    </signal>\n"
           "  </interface>\n"
           "</node>\n";
}

int fdk__tray_menu_serve_method(fdk_tray_menu *m,
                                fdk_dbus_connection *bus,
                                fdk_dbus_message *call) {
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);
    if (iface == NULL || member == NULL ||
        strcmp(iface, "com.canonical.dbusmenu") != 0) {
        return 0;
    }
    if (m == NULL) {
        /* No menu attached: an honest UnknownMethod, not silence. */
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.UnknownMethod",
            "no menu is attached to this item");
        if (err != NULL) {
            send_or_free(bus, err);
        }
        return 1;
    }
    if (strcmp(member, "GetLayout") == 0) {
        serve_get_layout(m, bus, call);
        return 1;
    }
    if (strcmp(member, "GetGroupProperties") == 0) {
        serve_get_group_properties(m, bus, call);
        return 1;
    }
    if (strcmp(member, "GetProperty") == 0) {
        serve_get_property(m, bus, call);
        return 1;
    }
    if (strcmp(member, "AboutToShow") == 0) {
        serve_about_to_show(m, bus, call);
        return 1;
    }
    if (strcmp(member, "Event") == 0) {
        serve_event(m, bus, call);
        return 1;
    }
    /* EventGroup / AboutToShowGroup: the optional bulk forms —
     * refused honestly (documented; FDK's own host never calls
     * them, and a guessed signature is worse than a refusal). */
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod",
        "the group forms are not served (the five core calls are)");
    if (err != NULL) {
        send_or_free(bus, err);
    }
    return 1;
}
