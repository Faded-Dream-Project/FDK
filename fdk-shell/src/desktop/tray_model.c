/* tray_model.c — the StatusNotifier host-side model (T-B side 2).
 *
 * The org.kde.StatusNotifierWatcher watcher service, the item registry
 * (both registration dialects), per-item state (properties, pixmaps,
 * tooltip), and the com.canonical.dbusmenu client — data and events,
 * NO DRAWING. Extracted verbatim-in-protocol from the reference UI
 * (examples/trayhost.c); the UI keeps rendering and forwards
 * interactions here. The rig (tests/trayhost_rig.sh) proves the pair
 * end to end and is the behavior-preservation proof of this split.
 *
 * Posture notes carried from the reference implementation (D-007):
 *   - both registration dialects (bus-name KDE form, object-path
 *     Ayatana form), hostile-input grammar checks, idempotent
 *     re-registration;
 *   - per-item signal subscriptions scoped to the item's UNIQUE name
 *     (the bus rewrites well-known senders in the header);
 *   - vanished items dropped through NameOwnerChanged (and the
 *     well-known-name bridge's own watcher);
 *   - the dbusmenu two-out-arg GetLayout contract, AboutToShow,
 *     NO_REPLY_EXPECTED Events, the stale-safe mid-open discard, and
 *     the load-time menu prefetch (the first right-click must work);
 *   - the watcher name etiquette matrix (refuse / --replace / --wait)
 *     and the self-registration as host over the real bus path.
 */
#include "fdk-shell/fdk-shell-desktop.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- constants ---------------------------------------------------------- */

#define W_NAME "org.kde.StatusNotifierWatcher"
#define W_PATH "/StatusNotifierWatcher"
#define W_IFACE "org.kde.StatusNotifierWatcher"

#define ITEM_IFACE_KDE "org.kde.StatusNotifierItem"
#define ITEM_IFACE_AYATANA "org.ayatana.StatusNotifierItem"
#define ITEM_IFACE_FREEDESKTOP "org.freedesktop.StatusNotifierItem"
#define ITEM_DEFAULT_PATH "/StatusNotifierItem"

#define MENU_IFACE "com.canonical.dbusmenu"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"

#define MAX_ITEMS_DEFAULT 32
#define MAX_ITEM_NAME 256
#define MAX_TITLE_BYTES 512
#define MAX_TOOLTIP_BYTES 512
#define MAX_PIXMAP_DIM 256
#define MAX_PIXMAP_BYTES (4 * MAX_PIXMAP_DIM * MAX_PIXMAP_DIM)
#define MAX_PIXMAPS 16      /* a(iiay) entries accepted per array   */
#define MAX_MENU_NODES 200  /* GetLayout parse budget              */
#define MAX_MENU_DEPTH 8    /* cascade depth budget                */

/* ---- the model's types --------------------------------------------------- */

typedef struct {
    int width;
    int height;
    uint8_t *pixels; /* ARGB32 host order, owned                  */
} pixmap;

typedef struct {
    char *title;
    char *description;
} tooltip_info;

typedef enum {
    ITEM_STATUS_ACTIVE = 0,
    ITEM_STATUS_PASSIVE,
    ITEM_STATUS_NEEDS_ATTENTION,
} item_status;

typedef struct menu_node menu_node;

struct menu_node {
    menu_node *next;   /* sibling list                              */
    menu_node *child;  /* first child (when a submenu)              */
    int32_t id;
    char *label;       /* mnemonic-stripped, owned                  */
    int is_separator;
    int is_check;
    int is_radio;
    int toggle_state;  /* 0/1 (radio + check)                       */
    int enabled;
    char *icon_name;   /* dbusmenu "icon-name" property, owned      */
    menu_node *owner;  /* submenu rows point at their parent        */
};

/* The in-flight GetLayout's user data (embedded in the item — a
 * canceled pending never fires its reply, so a heap ctx would leak). */
typedef struct tray_item tray_item;

typedef struct {
    tray_item *item;
    int prefetch; /* a load-time prefetch, not an open              */
} menu_fetch_ctx;

typedef struct tray_item {
    struct tray_item *next;
    struct tray_item *prev;

    fdk_shell_tray_host *owner_host; /* every callback's way home   */

    uint32_t serial;    /* the stable identity events carry         */

    /* Identity (bus side). */
    char *reg_arg;       /* exactly what RegisterStatusNotifierItem
                          * carried — echoed in signals/property   */
    char *sender_uniq;   /* ":1.x" — the match-rule scope           */
    char *bus_name;      /* KDE-form bus name or NULL               */
    char *object_path;   /* the item's object path                  */
    fdk_dbus_name_watcher *name_watch; /* for well-known forms      */

    /* Properties (GetAll results; all owned strings). */
    char *id_str;
    char *title;
    item_status status;
    char *icon_name;       /* IconName                                */
    char *attn_icon_name;  /* AttentionIconName                       */
    pixmap *pixmaps;       /* IconPixmap list                         */
    size_t pixmap_count;
    pixmap *attn_pixmaps;  /* AttentionIconPixmap list               */
    size_t attn_pixmap_count;
    tooltip_info tooltip;
    int has_menu;          /* the Menu property arrived              */
    char *menu_path;       /* the dbusmenu object path               */
    int item_is_menu;      /* ItemIsMenu: clicks open the menu       */

    /* Live state. */
    fdk_dbus_subscription *item_subs[7]; /* New* signal subs        */
    fdk_dbus_pending *getall_p;  /* in-flight GetAll                */
    fdk_dbus_pending *layout_p; /* in-flight GetLayout             */
    int loading;         /* properties not fetched yet              */
    int failed;          /* un-usable item (kept until unregister)  */
    int menu_open;       /* the UI holds this item's menu open (the
                          * stale-safe discard's gate)              */

    /* The dbusmenu cache (owned; NULL until GetLayout answers). */
    menu_node *menu_cache;
    int32_t menu_revision;
    /* The in-flight GetLayout's user data — EMBEDDED (a canceled
     * pending never fires its reply, so a heap ctx would leak; the
     * embed makes cancellation free by construction). */
    menu_fetch_ctx menu_fetch;
} tray_item;

struct fdk_shell_tray_host {
    fdk_dbus_connection *bus;

    fdk_shell_tray_event_fn event_cb;
    void *event_user;
    int max_items;
    int opt_replace;
    int opt_wait;

    tray_item *head;  /* registration order                          */
    uint32_t serial_next;

    /* The NameOwnerChanged subscription (vanished items) — held for
     * the connection's lifetime; freeing it earlier REMOVES the
     * match rule (subscription_free issues RemoveMatch — the drop
     * path would silently never fire). */
    fdk_dbus_subscription *owner_sub;
    fdk_dbus_name_watcher *self_watch;

    int serving;
    int dead; /* destroy() ran; everything after is a no-op          */
    int refused; /* the incumbent refusal: DORMANT forever (never a
                  * queue-lurker that grabs the name when the
                  * incumbent vacates)                          */
};

/* ---- tracing ---------------------------------------------------------------
 *
 * The model logs its protocol transactions on the SAME channel the
 * reference UI opened (stderr under FDK_TRAYHOST_TRACE=1) — the rigs
 * grep these lines from the host process's log, so the split keeps
 * them. Logging is not drawing; the model stays renderer-free. */

static int g_trace = -1; /* -1 = unchecked, 0 = off, 1 = on        */

static void tracef(const char *fmt, ...) {
    if (g_trace < 0) {
        g_trace = getenv("FDK_TRAYHOST_TRACE") != NULL ? 1 : 0;
    }
    if (!g_trace) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fputs("trayhost[trace]: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* ---- small utilities ----------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u +
           (uint64_t)ts.tv_nsec / 1000000u;
}

static char *xstrdup_n(const char *s, size_t cap) {
    /* strdup clipped to cap bytes (always NUL-terminated). */
    if (s == NULL) {
        return NULL;
    }
    size_t len = strlen(s);
    if (len > cap) {
        len = cap;
    }
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static int valid_bus_name(const char *s) {
    /* The grammar the reference host accepted: unique names
     * (":1.x") and dot-separated well-known names, each element
     * 1..63 of [A-Za-z0-9_-], total <= 255. */
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    if (s[0] == ':') {
        s++;
        if (s[0] == '\0') {
            return 0;
        }
    }
    size_t elem = 0, total = 0;
    for (; s[total] != '\0'; total++) {
        char c = s[total];
        if (c == '.') {
            if (elem == 0 || elem > 63) {
                return 0;
            }
            elem = 0;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            elem++;
            continue;
        }
        return 0;
    }
    if (elem == 0 || elem > 63 || total > 255) {
        return 0;
    }
    return 1;
}

static int valid_object_path(const char *s) {
    if (s == NULL || s[0] != '/') {
        return 0;
    }
    if (s[1] == '\0') {
        return 1; /* the root path is legal                        */
    }
    /* Scan from index 1: the LEADING separator is not an element
     * boundary (the naive full-string walk refused every path at
     * its first character — the rig's Ayatana leg caught it). */
    size_t elem = 0, total = 1;
    for (; s[total] != '\0'; total++) {
        char c = s[total];
        if (c == '/') {
            if (elem == 0 || elem > 63) {
                return 0;
            }
            elem = 0;
            continue;
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') {
            elem++;
            continue;
        }
        return 0;
    }
    if (elem == 0 || elem > 63 || total > 255) {
        return 0;
    }
    return 1;
}

static char *strip_mnemonic(const char *in) {
    /* Removes '_' from labels, except a literal "__" which emits one
     * underscore. NULL in, NULL out; empty string is legal. */
    if (in == NULL) {
        return NULL;
    }
    size_t n = strlen(in);
    char *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '_') {
            if (i + 1 < n && in[i + 1] == '_') {
                out[o++] = '_';
                i++;
                continue;
            }
            continue; /* the mnemonic marker vanishes               */
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
    return out;
}

/* The coarse event delivery. */
static void tray_emit(fdk_shell_tray_host *h, fdk_shell_tray_event_type ev,
                      uint32_t serial) {
    if (h != NULL && !h->dead && h->event_cb != NULL) {
        h->event_cb(h, ev, serial, h->event_user);
    }
}

/* ---- D-Bus value helpers -------------------------------------------------
 *
 * dict-entry builders whose values are VARIANT-wrapped (the
 * PropertiesChanged / GetAll a{sv} shape). The array constructor
 * TAKES the entries it accepts — a failure means every entry is the
 * caller's to free (the ownership-consumption discipline the live
 * debugging pinned). */

static fdk_dbus_value *sv_string(const char *key, const char *val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_string(val);
    fdk_dbus_value *var =
        inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e =
        (k != NULL && var != NULL)
            ? fdk_dbus_value_new_dict_entry(k, var)
            : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
    }
    return e;
}

static fdk_dbus_value *sv_bool(const char *key, int val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_boolean(val);
    fdk_dbus_value *var =
        inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e =
        (k != NULL && var != NULL)
            ? fdk_dbus_value_new_dict_entry(k, var)
            : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
    }
    return e;
}

static fdk_dbus_value *sv_int32(const char *key, int32_t val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_int32(val);
    fdk_dbus_value *var =
        inner != NULL ? fdk_dbus_value_new_variant(inner) : NULL;
    fdk_dbus_value_free(inner);
    fdk_dbus_value *e =
        (k != NULL && var != NULL)
            ? fdk_dbus_value_new_dict_entry(k, var)
            : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
    }
    return e;
}

static fdk_dbus_value *sv_str_array(const char *key,
                                    const char *const *vals, size_t count) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value **elems = NULL;
    fdk_dbus_value *arr = NULL;
    if (count > 0) {
        elems = calloc(count, sizeof(*elems));
        if (elems != NULL) {
            for (size_t i = 0; i < count; i++) {
                elems[i] = fdk_dbus_value_new_string(vals[i]);
            }
            arr = fdk_dbus_value_new_array("s", elems, count);
            if (arr == NULL) {
                for (size_t i = 0; i < count; i++) {
                    fdk_dbus_value_free(elems[i]);
                }
            }
            free(elems);
        }
    } else {
        arr = fdk_dbus_value_new_array("s", NULL, 0);
    }
    fdk_dbus_value *var =
        arr != NULL ? fdk_dbus_value_new_variant(arr) : NULL;
    fdk_dbus_value_free(arr);
    fdk_dbus_value *e =
        (k != NULL && var != NULL)
            ? fdk_dbus_value_new_dict_entry(k, var)
            : NULL;
    if (e == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(var);
    }
    return e;
}

/* ---- a(iiay) pixmap decode ---------------------------------------------- */

static void pixmap_free_list(pixmap *list, size_t count) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(list[i].pixels);
    }
    free(list);
}

/* A byte from an 'ay' value: the FLAT form first (every array that
 * arrived on the wire is flat since the value model grew the bulk
 * form — the tree form's per-index reads return nothing there, the
 * all-zero-icon bug this caught), the tree form second (hand-built
 * values). */
static uint8_t ay_byte(const fdk_dbus_value *ay, size_t index) {
    size_t n = 0;
    const uint8_t *flat = fdk_dbus_value_array_flat_bytes(ay, &n);
    if (flat != NULL) {
        return index < n ? flat[index] : 0;
    }
    const fdk_dbus_value *v = fdk_dbus_value_array_item(ay, index);
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_BYTE) {
        return 0;
    }
    return (uint8_t)fdk_dbus_value_byte(v);
}

static int pixmap_from_struct(const fdk_dbus_value *v, pixmap *out) {
    /* (iiay): width, height, bytes — network (big-endian) ARGB32. */
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_STRUCT ||
        fdk_dbus_value_struct_count(v) != 3) {
        return 0;
    }
    const fdk_dbus_value *wv = fdk_dbus_value_struct_item(v, 0);
    const fdk_dbus_value *hv = fdk_dbus_value_struct_item(v, 1);
    const fdk_dbus_value *av = fdk_dbus_value_struct_item(v, 2);
    if (fdk_dbus_value_type(wv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(hv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(av) != FDK_DBUS_ARRAY) {
        return 0;
    }
    int32_t w = fdk_dbus_value_int32(wv);
    int32_t h = fdk_dbus_value_int32(hv);
    if (w <= 0 || h <= 0 || w > MAX_PIXMAP_DIM || h > MAX_PIXMAP_DIM) {
        return 0;
    }
    /* The spec's array is BYTEs; int32 arrays (gdbus's text-form
     * inference) are refused — reading ints as bytes would silently
     * yield an all-zero (fully transparent) icon. */
    {
        const char *elem = fdk_dbus_value_array_signature(av);
        if (elem == NULL || elem[0] != 'y' || elem[1] != '\0') {
            return 0;
        }
    }
    size_t need = (size_t)w * (size_t)h * 4u;
    size_t have = fdk_dbus_value_array_count(av);
    if (have < need) {
        return 0; /* short pixel arrays are refused whole           */
    }
    uint8_t *px = malloc(need);
    if (px == NULL) {
        return 0;
    }
    /* Network byte order -> host order (each 4-byte pixel). */
    for (size_t i = 0; i < need; i += 4) {
        px[i + 0] = ay_byte(av, i + 3);
        px[i + 1] = ay_byte(av, i + 2);
        px[i + 2] = ay_byte(av, i + 1);
        px[i + 3] = ay_byte(av, i + 0);
    }
    out->width = (int)w;
    out->height = (int)h;
    out->pixels = px;
    return 1;
}

static size_t pixmap_list_from_array(const fdk_dbus_value *arr,
                                     pixmap **out_list) {
    if (arr == NULL || fdk_dbus_value_type(arr) != FDK_DBUS_ARRAY) {
        *out_list = NULL;
        return 0;
    }
    size_t count = fdk_dbus_value_array_count(arr);
    if (count > MAX_PIXMAPS) {
        count = MAX_PIXMAPS;
    }
    if (count == 0) {
        *out_list = NULL;
        return 0;
    }
    pixmap *list = calloc(count, sizeof(*list));
    if (list == NULL) {
        *out_list = NULL;
        return 0;
    }
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (!pixmap_from_struct(fdk_dbus_value_array_item(arr, i),
                                &list[kept])) {
            continue;
        }
        kept++;
    }
    if (kept == 0) {
        free(list);
        *out_list = NULL;
        return 0;
    }
    *out_list = list;
    return kept;
}

/* ---- item lifecycle ------------------------------------------------------ */

static void menu_node_free(menu_node *node) {
    while (node != NULL) {
        menu_node *next = node->next;
        menu_node_free(node->child);
        free(node->label);
        free(node->icon_name);
        free(node);
        node = next;
    }
}

static void item_free(tray_item *it) {
    /* Cancel pendings FIRST (their callbacks would touch a freed
     * item otherwise; cancel means the callback never fires). */
    fdk_dbus_pending_cancel(it->getall_p);
    fdk_dbus_pending_cancel(it->layout_p);
    for (size_t i = 0;
         i < sizeof(it->item_subs) / sizeof(*it->item_subs); i++) {
        fdk_dbus_subscription_free(it->item_subs[i]);
        it->item_subs[i] = NULL;
    }
    fdk_dbus_name_watcher_free(it->name_watch);
    it->name_watch = NULL;
    pixmap_free_list(it->pixmaps, it->pixmap_count);
    pixmap_free_list(it->attn_pixmaps, it->attn_pixmap_count);
    free(it->reg_arg);
    free(it->sender_uniq);
    free(it->bus_name);
    free(it->object_path);
    free(it->id_str);
    free(it->title);
    free(it->icon_name);
    free(it->attn_icon_name);
    free(it->tooltip.title);
    free(it->tooltip.description);
    free(it->menu_path);
    menu_node_free(it->menu_cache);
    free(it);
}

static tray_item *item_find_by_reg(fdk_shell_tray_host *h, const char *reg) {
    for (tray_item *it = h->head; it != NULL; it = it->next) {
        if (it->reg_arg != NULL && strcmp(it->reg_arg, reg) == 0) {
            return it;
        }
    }
    return NULL;
}

static tray_item *item_find_by_sender(fdk_shell_tray_host *h,
                                      const char *uniq) {
    for (tray_item *it = h->head; it != NULL; it = it->next) {
        if (it->sender_uniq != NULL && strcmp(it->sender_uniq, uniq) == 0) {
            return it;
        }
    }
    return NULL;
}

static tray_item *item_at(fdk_shell_tray_host *h, uint32_t index) {
    uint32_t i = 0;
    for (tray_item *it = h->head; it != NULL; it = it->next, i++) {
        if (i == index) {
            return it;
        }
    }
    return NULL;
}

/* The read-only twin (the snapshot APIs take a const host). */
static const tray_item *item_at_c(const fdk_shell_tray_host *h,
                                  uint32_t index) {
    uint32_t i = 0;
    for (const tray_item *it = h->head; it != NULL; it = it->next, i++) {
        if (i == index) {
            return it;
        }
    }
    return NULL;
}

/* The drop: unlink, announce, free, notify. */
static void item_drop(fdk_shell_tray_host *h, tray_item *it, int emit) {
    if (h->head == it) {
        h->head = it->next;
    } else if (it->prev != NULL) {
        it->prev->next = it->next;
    }
    if (it->next != NULL) {
        it->next->prev = it->prev;
    }
    char *reg = it->reg_arg != NULL ? strdup(it->reg_arg) : NULL;
    uint32_t serial = it->serial;
    item_free(it);

    /* The announcement (the spec's signal; sent while we still own
     * the name — a drop after losing the name stays silent). */
    if (h->serving && reg != NULL) {
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            W_PATH, W_IFACE, "StatusNotifierItemUnregistered");
        if (sig != NULL) {
            if (fdk_ok(fdk_dbus_message_append_string(sig, reg))) {
                fdk_dbus_connection_send(h->bus, sig);
            } else {
                fdk_dbus_message_free(sig);
            }
        }
    }
    free(reg);
    if (emit) {
        tray_emit(h, FDK_SHELL_TRAY_EV_ITEMS_CHANGED, serial);
    }
}

/* ---- item property load (GetAll fallback chain) --------------------------- */

static void item_send_new_prefetch(tray_item *it);

static void apply_status(tray_item *it, const char *s) {
    if (s == NULL) {
        return;
    }
    if (strcmp(s, "Passive") == 0) {
        it->status = ITEM_STATUS_PASSIVE;
    } else if (strcmp(s, "NeedsAttention") == 0) {
        it->status = ITEM_STATUS_NEEDS_ATTENTION;
    } else {
        it->status = ITEM_STATUS_ACTIVE; /* the honest default       */
    }
}

/* The property dictionary applier — every value is type-checked and
 * size-capped; a malformed value is skipped, never fatal. Returns
 * nonzero when something the UI sees changed (drives ITEM_STATE). */
static int item_apply_props(tray_item *it, const fdk_dbus_value *dict) {
    int changed = 0;
    for (size_t i = 0;
         i < fdk_dbus_value_array_count(dict) && !it->failed; i++) {
        const fdk_dbus_value *entry =
            fdk_dbus_value_array_item(dict, i);
        const fdk_dbus_value *kv = fdk_dbus_value_dict_key(entry);
        const fdk_dbus_value *vv = fdk_dbus_value_dict_value(entry);
        if (kv == NULL || vv == NULL) {
            continue;
        }
        if (fdk_dbus_value_type(vv) == FDK_DBUS_VARIANT) {
            vv = fdk_dbus_value_variant_value(vv);
        }
        const char *key = fdk_dbus_value_string(kv);
        if (key == NULL) {
            continue;
        }
        if (strcmp(key, "Id") == 0 &&
            fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->id_str);
            it->id_str = xstrdup_n(fdk_dbus_value_string(vv),
                                   MAX_ITEM_NAME);
        } else if (strcmp(key, "Title") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->title);
            it->title = xstrdup_n(fdk_dbus_value_string(vv),
                                  MAX_TITLE_BYTES);
            changed = 1;
        } else if (strcmp(key, "Status") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            apply_status(it, fdk_dbus_value_string(vv));
            changed = 1;
        } else if (strcmp(key, "IconName") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->icon_name);
            it->icon_name = xstrdup_n(fdk_dbus_value_string(vv),
                                      MAX_ITEM_NAME);
            changed = 1;
        } else if (strcmp(key, "AttentionIconName") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->attn_icon_name);
            it->attn_icon_name = xstrdup_n(fdk_dbus_value_string(vv),
                                           MAX_ITEM_NAME);
            changed = 1;
        } else if (strcmp(key, "IconPixmap") == 0) {
            pixmap_free_list(it->pixmaps, it->pixmap_count);
            it->pixmaps = NULL;
            it->pixmap_count =
                pixmap_list_from_array(vv, &it->pixmaps);
            changed = 1;
        } else if (strcmp(key, "AttentionIconPixmap") == 0) {
            pixmap_free_list(it->attn_pixmaps, it->attn_pixmap_count);
            it->attn_pixmaps = NULL;
            it->attn_pixmap_count =
                pixmap_list_from_array(vv, &it->attn_pixmaps);
            changed = 1;
        } else if (strcmp(key, "ToolTip") == 0) {
            /* (sa(iiay)(ss)ss): icon name, pixmaps, a struct, title,
             * description — parsed by STRUCT COUNT with per-field
             * type checks (tolerant to the middle struct's shape). */
            if (fdk_dbus_value_type(vv) == FDK_DBUS_STRUCT &&
                fdk_dbus_value_struct_count(vv) == 5) {
                const fdk_dbus_value *f3 =
                    fdk_dbus_value_struct_item(vv, 3);
                const fdk_dbus_value *f4 =
                    fdk_dbus_value_struct_item(vv, 4);
                if (fdk_dbus_value_type(f3) == FDK_DBUS_STRING &&
                    fdk_dbus_value_type(f4) == FDK_DBUS_STRING) {
                    free(it->tooltip.title);
                    free(it->tooltip.description);
                    it->tooltip.title = xstrdup_n(
                        fdk_dbus_value_string(f3), MAX_TOOLTIP_BYTES);
                    it->tooltip.description = xstrdup_n(
                        fdk_dbus_value_string(f4), MAX_TOOLTIP_BYTES);
                    changed = 1;
                }
            }
        } else if (strcmp(key, "Menu") == 0) {
            if (fdk_dbus_value_type(vv) == FDK_DBUS_OBJECT_PATH) {
                free(it->menu_path);
                it->menu_path =
                    xstrdup_n(fdk_dbus_value_string(vv), 255);
                it->has_menu = it->menu_path != NULL;
                changed = 1;
            }
        } else if (strcmp(key, "ItemIsMenu") == 0) {
            if (fdk_dbus_value_type(vv) == FDK_DBUS_BOOLEAN) {
                it->item_is_menu = fdk_dbus_value_boolean(vv);
                changed = 1;
            }
        }
    }
    return changed;
}

static void item_getall_reply(fdk_dbus_connection *bus,
                              fdk_dbus_message *reply, void *user) {
    (void)bus;
    tray_item *it = user;
    fdk_shell_tray_host *h = it->owner_host;
    it->getall_p = NULL; /* FIRST: the item may be gone next line    */
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        tracef("GetAll failed for %s", it->reg_arg ? it->reg_arg : "?");
        it->failed = 1;
        tray_emit(h, FDK_SHELL_TRAY_EV_ITEMS_CHANGED, it->serial);
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(reply, &args, &n) != FDK_OK ||
        n != 1 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_ARRAY) {
        fdk_dbus_values_free_array(args, n);
        it->failed = 1;
        tray_emit(h, FDK_SHELL_TRAY_EV_ITEMS_CHANGED, it->serial);
        return;
    }
    int was_loading = it->loading;
    int changed = item_apply_props(it, args[0]);
    fdk_dbus_values_free_array(args, n);
    it->loading = 0;
    if (was_loading && !it->failed) {
        /* The load completed: the item joined the enumerable set. */
        tray_emit(h, FDK_SHELL_TRAY_EV_ITEMS_CHANGED, it->serial);
        item_send_new_prefetch(it);
    } else if (changed) {
        tray_emit(h, FDK_SHELL_TRAY_EV_ITEM_STATE, it->serial);
    }
}

/* Issues the GetAll with the interface fallback chain. */
static void item_fetch_props(tray_item *it) {
    static const char *const ifaces[] = {
        ITEM_IFACE_KDE, ITEM_IFACE_AYATANA, ITEM_IFACE_FREEDESKTOP,
    };
    fdk_shell_tray_host *h = it->owner_host;
    if (h->bus == NULL || it->sender_uniq == NULL ||
        it->object_path == NULL) {
        it->failed = 1;
        return;
    }
    for (size_t i = 0; i < sizeof(ifaces) / sizeof(*ifaces); i++) {
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            it->sender_uniq, it->object_path, PROPS_IFACE, "GetAll");
        if (call == NULL ||
            !fdk_ok(fdk_dbus_message_append_string(call, ifaces[i]))) {
            fdk_dbus_message_free(call);
            continue;
        }
        fdk_dbus_pending *p = fdk_dbus_connection_call(
            h->bus, call, 3000, item_getall_reply, it);
        if (p != NULL) {
            it->getall_p = p;
            tracef("GetAll(%s) on %s %s", ifaces[i], it->sender_uniq,
                   it->object_path);
            return; /* one in flight; the reply drives the chain     */
        }
    }
    it->failed = 1;
}

/* ---- the Watcher's served surface -----------------------------------------
 *
 * Properties (all read-only on this host):
 *   RegisteredStatusNotifierItems  as
 *   IsStatusNotifierHostRegistered b
 *   ProtocolVersion                i  (0 — the spec's number)
 *   IconTheme                      s  (the current icon theme name,
 *                                     else "default")
 */

static void watcher_getall(fdk_shell_tray_host *h,
                           const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    /* Build the a{sv} dict with every entry value VARIANT-wrapped. */
    fdk_dbus_value **entries = calloc(32, sizeof(*entries));
    if (entries == NULL) {
        fdk_dbus_message_free(rep);
        return;
    }
    size_t n = 0;

    const char *names[MAX_ITEMS_DEFAULT + 1];
    size_t count = 0;
    for (tray_item *it = h->head;
         it != NULL && count < (size_t)h->max_items; it = it->next) {
        names[count++] = it->reg_arg;
    }
    entries[n++] = sv_str_array("RegisteredStatusNotifierItems", names,
                                count);
    entries[n++] = sv_bool("IsStatusNotifierHostRegistered", 1);
    entries[n++] = sv_int32("ProtocolVersion", 0);
    /* Same truth as Get (D-007): the override channel or "default". */
    const char *env_theme = getenv("FDK_ICON_THEME");
    entries[n++] = sv_string(
        "IconTheme",
        env_theme != NULL && env_theme[0] != '\0' ? env_theme : "default");

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
    if (!ok || dict == NULL) {
        /* the array constructor took nothing — every entry is ours */
        for (size_t i = 0; i < n; i++) {
            fdk_dbus_value_free(entries[i]);
        }
    }
    free(entries); /* the pointer array itself (the leak the rig's
                    * LSan check caught: 256 bytes per GetAll)       */
    tracef("Properties.GetAll -> %zu items, host-registered", count);
    if (dict != NULL && fdk_ok(fdk_dbus_message_append(rep, dict))) {
        fdk_dbus_value_free(dict);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(h->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_value_free(dict);
        fdk_dbus_message_free(rep);
    }
}

static void watcher_get(fdk_shell_tray_host *h,
                        const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    char *prop = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
        n >= 2 && args != NULL &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        /* Borrowed string: copy BEFORE the values array frees (the
         * UAF class read_args' contract implies). */
        prop = xstrdup_n(fdk_dbus_value_string(args[1]), 255);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    if (prop == NULL) {
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "Get requires (interface, property) strings");
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        free(prop);
        return;
    }
    int ok = 0;
    if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
        /* VARIANT(boolean) — built by hand (the boolean variant has
         * no convenience appender). */
        fdk_dbus_value *inner =
            fdk_dbus_value_new_boolean(h->serving ? 1 : 0);
        fdk_dbus_value *var = inner != NULL
                                  ? fdk_dbus_value_new_variant(inner)
                                  : NULL;
        fdk_dbus_value_free(inner);
        if (var != NULL) {
            ok = fdk_ok(fdk_dbus_message_append(rep, var));
            fdk_dbus_value_free(var);
        }
    } else if (strcmp(prop, "ProtocolVersion") == 0) {
        /* VARIANT(int32) — the appenders cover string/uint32 only, so
         * the value tree is built by hand (new_variant CLONES). */
        fdk_dbus_value *inner = fdk_dbus_value_new_int32(0);
        fdk_dbus_value *var = inner != NULL
                                  ? fdk_dbus_value_new_variant(inner)
                                  : NULL;
        fdk_dbus_value_free(inner);
        if (var != NULL) {
            ok = fdk_ok(fdk_dbus_message_append(rep, var));
            fdk_dbus_value_free(var);
        }
    } else if (strcmp(prop, "IconTheme") == 0) {
        /* The one channel a public-API client can read truthfully:
         * the override the icon lookup itself honors first, else the
         * honest "default" (pixmaps are the primary channel for
         * real items anyway — D-007). */
        const char *env = getenv("FDK_ICON_THEME");
        ok = fdk_ok(fdk_dbus_message_append_variant_string(
            rep, env != NULL && env[0] != '\0' ? env : "default"));
    } else if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
        fdk_dbus_value **items =
            calloc((size_t)h->max_items + 1, sizeof(*items));
        size_t count = 0;
        if (items != NULL) {
            for (tray_item *it = h->head;
                 it != NULL && count < (size_t)h->max_items;
                 it = it->next) {
                items[count++] =
                    fdk_dbus_value_new_string(it->reg_arg);
            }
            fdk_dbus_value *arr =
                fdk_dbus_value_new_array("s", items, count);
            if (arr == NULL) {
                for (size_t i = 0; i < count; i++) {
                    fdk_dbus_value_free(items[i]);
                }
            }
            free(items);
            if (arr != NULL) {
                ok = fdk_ok(fdk_dbus_message_append(rep, arr));
                fdk_dbus_value_free(arr);
            }
        }
    }
    if (ok) {
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(h->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_message_free(rep);
        char text[160];
        snprintf(text, sizeof(text),
                 "No such property '%s'", prop);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs", text);
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
    }
    free(prop);
}

static void watcher_set(fdk_shell_tray_host *h,
                        const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    char *prop = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
        n >= 2 && args != NULL &&
        fdk_dbus_value_type(args[1]) == FDK_DBUS_STRING) {
        prop = xstrdup_n(fdk_dbus_value_string(args[1]), 255);
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    char text[160];
    snprintf(text, sizeof(text),
             "Property '%s' is read-only on this watcher",
             prop ? prop : "?");
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.PropertyReadOnly", text);
    if (err != NULL) {
        fdk_dbus_connection_send(h->bus, err);
    }
    tracef("Properties.Set refused: %s", prop ? prop : "?");
    free(prop);
    (void)h;
}

/* ---- properties-changed emission --------------------------------------- */

/* Emits PropertiesChanged for the Watcher's item-list property (the
 * bus-side contract: every a{sv} value is VARIANT-wrapped). */
static void emit_items_changed(fdk_shell_tray_host *h) {
    if (h->bus == NULL || !h->serving) {
        return;
    }
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        W_PATH, PROPS_IFACE, "PropertiesChanged");
    if (sig == NULL) {
        return;
    }
    fdk_dbus_value *entry = NULL;
    {
        fdk_dbus_value **items =
            calloc((size_t)h->max_items + 1, sizeof(*items));
        size_t count = 0;
        if (items != NULL) {
            for (tray_item *it = h->head;
                 it != NULL && count < (size_t)h->max_items;
                 it = it->next) {
                items[count++] = fdk_dbus_value_new_string(it->reg_arg);
            }
        }
        fdk_dbus_value *inner =
            items != NULL ? fdk_dbus_value_new_array("s", items, count)
                          : NULL;
        if (items != NULL) {
            if (inner == NULL) {
                for (size_t i = 0; i < count; i++) {
                    fdk_dbus_value_free(items[i]);
                }
            }
            free(items);
        }
        fdk_dbus_value *k = fdk_dbus_value_new_string(
            "RegisteredStatusNotifierItems");
        fdk_dbus_value *var = inner != NULL
                                  ? fdk_dbus_value_new_variant(inner)
                                  : NULL;
        fdk_dbus_value_free(inner); /* the variant cloned it        */
        if (k != NULL && var != NULL) {
            entry = fdk_dbus_value_new_dict_entry(k, var);
        }
        if (entry == NULL) {
            /* the entry took nothing — everything is still ours    */
            fdk_dbus_value_free(k);
            fdk_dbus_value_free(var);
        }
    }
    fdk_dbus_value *dict = NULL;
    if (entry != NULL) {
        fdk_dbus_value *entries[1] = {entry};
        dict = fdk_dbus_value_new_array("{sv}", entries, 1);
    }
    fdk_dbus_value *invalid = fdk_dbus_value_new_array("s", NULL, 0);
    int sent = 0;
    if (dict != NULL && invalid != NULL &&
        fdk_ok(fdk_dbus_message_append_string(sig, W_IFACE)) &&
        fdk_ok(fdk_dbus_message_append(sig, dict)) &&
        fdk_ok(fdk_dbus_message_append(sig, invalid))) {
        fdk_dbus_connection_send(h->bus, sig);
        sent = 1;
    } else if (dict == NULL || entry == NULL) {
        fdk_dbus_message_free(sig);
        sig = NULL;
    }
    fdk_dbus_value_free(dict);
    fdk_dbus_value_free(invalid);
    if (sig != NULL && !sent) {
        fdk_dbus_message_free(sig);
    }
    tracef("PropertiesChanged(RegisteredStatusNotifierItems)");
}

/* ---- item signal subscriptions ------------------------------------------- */

/* New* handling: the property refresh each signal implies. */
static void item_refresh_icon(tray_item *it) {
    /* The pixmap/icon name list is re-fetched with a fresh GetAll. */
    if (it->getall_p != NULL) {
        return; /* one in flight; the reply carries the newer data  */
    }
    it->loading = 0; /* a refresh, not a first load                 */
    item_fetch_props(it);
}

static void item_signal(fdk_dbus_connection *bus,
                        fdk_dbus_message *sig, void *user) {
    (void)bus;
    tray_item *it = user;
    fdk_shell_tray_host *h = it->owner_host;
    const char *member = fdk_dbus_message_member(sig);
    tracef("item signal %s from %s", member ? member : "?",
           it->reg_arg ? it->reg_arg : "?");
    if (member == NULL) {
        return;
    }
    if (strcmp(member, "NewTitle") == 0 || strcmp(member, "NewIcon") == 0 ||
        strcmp(member, "NewAttentionIcon") == 0 ||
        strcmp(member, "NewOverlayIcon") == 0 ||
        strcmp(member, "NewToolTip") == 0 ||
        strcmp(member, "NewMenu") == 0) {
        if (strcmp(member, "NewMenu") == 0 && it->menu_path != NULL) {
            /* The menu path may have changed: re-fetch the tree. */
            fdk_dbus_pending_cancel(it->layout_p);
            it->layout_p = NULL;
            menu_node_free(it->menu_cache);
            it->menu_cache = NULL;
            it->menu_fetch.item = it;
            it->menu_fetch.prefetch = 1;
            tray_emit(h, FDK_SHELL_TRAY_EV_MENU_CHANGED, it->serial);
        }
        item_refresh_icon(it);
    } else if (strcmp(member, "NewStatus") == 0) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(sig, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
            /* Borrowed string: copy before the values array frees. */
            char *s = xstrdup_n(fdk_dbus_value_string(args[0]), 64);
            fdk_dbus_values_free_array(args, n);
            if (s != NULL) {
                apply_status(it, s);
                free(s);
                tray_emit(h, FDK_SHELL_TRAY_EV_ITEM_STATE, it->serial);
            }
        } else if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    }
}

/* Subscribes the New* family scoped to the item's UNIQUE name — the
 * header value the bus rewrites well-known senders into; a rule on
 * the well-known name itself would never fire (the daemon rewrites
 * the header, so the match misses). */
static void item_subscribe_signals(tray_item *it) {
    fdk_shell_tray_host *h = it->owner_host;
    if (it->sender_uniq == NULL || it->object_path == NULL) {
        return;
    }
    static const char *const members[] = {
        "NewTitle",      "NewIcon",    "NewAttentionIcon",
        "NewOverlayIcon", "NewToolTip", "NewMenu", "NewStatus",
    };
    for (size_t i = 0;
         i < sizeof(members) / sizeof(*members) &&
         i < sizeof(it->item_subs) / sizeof(*it->item_subs);
         i++) {
        char *rule = fdk_dbus_match_rule(
            "signal", it->sender_uniq, NULL, members[i], it->object_path,
            NULL);
        if (rule == NULL) {
            continue;
        }
        it->item_subs[i] = fdk_dbus_connection_subscribe(
            h->bus, rule, item_signal, it);
        free(rule);
    }
}

/* ---- the well-known-name -> unique-name bridge --------------------------- */

/* For a KDE-form registration on a WELL-KNOWN name: the unique owner
 * is tracked (NameOwnerChanged under the hood); when it is known, the
 * item's signal subscriptions and calls go out against it. The
 * watcher's own initial state arrives synchronously. */
static void item_name_event(fdk_dbus_connection *bus, const char *name,
                            const char *owner, void *user) {
    (void)bus;
    tray_item *it = user;
    tracef("watch %s owner -> %s (item %s)", name ? name : "?",
           owner ? owner : "(none)", it->reg_arg ? it->reg_arg : "?");
    fdk_shell_tray_host *h = it->owner_host;
    if (owner == NULL) {
        /* The item's name lost its owner: drop it (this is ALSO the
         * path for a crashed client — hostile clients never
         * unregister politely). */
        item_drop(h, it, 1);
        return;
    }
    if (it->sender_uniq == NULL) {
        it->sender_uniq = xstrdup_n(owner, 255);
        if (it->sender_uniq != NULL) {
            item_subscribe_signals(it);
            item_fetch_props(it);
        }
    }
}

/* ---- registration -------------------------------------------------------- */

/* RegisterStatusNotifierItem(service): the two dialects.
 *   KDE form:     a bus name; the object path is the canonical
 *                 /StatusNotifierItem (the caller MAY be the owner).
 *   Ayatana form: an object path; the ITEM is the caller (sender).
 * The caller's own unique name (the message sender) is the scope for
 * subscriptions when the KDE name is the caller's own; for a foreign
 * well-known name the name watcher resolves the owner. */
static void serve_register_item(fdk_shell_tray_host *h,
                                const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *service = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK && n == 1 &&
        args != NULL && fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
        service = fdk_dbus_value_string(args[0]);
    }
    if (service == NULL) {
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "RegisterStatusNotifierItem requires one string argument");
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }
    /* Copy FIRST: read_args strings are borrowed from the values
     * array, which is about to be freed (the UAF class). */
    char *svc = xstrdup_n(service, 255);
    fdk_dbus_values_free_array(args, n);
    if (svc == NULL) {
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.NoMemory",
            "allocation failure");
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }

    const char *sender = fdk_dbus_message_sender(call);
    int kde_form = svc[0] != '/';
    if (kde_form ? !valid_bus_name(svc) : !valid_object_path(svc)) {
        tracef("RegisterStatusNotifierItem refused: '%s'", svc);
        char text[128];
        snprintf(text, sizeof(text),
                 "'%.60s' is neither a bus name nor an object path", svc);
        free(svc);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs", text);
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }

    if (item_find_by_reg(h, svc) != NULL) {
        /* Duplicate registration: the honest idempotent answer —
         * reply OK (items re-register after restarts). */
        tracef("re-registration of %s: accepted, no duplicate row",
               svc);
        free(svc);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL) {
                fdk_dbus_connection_send(h->bus, rep);
            }
        }
        return;
    }

    /* The cap (the same refusal class as malformed input). */
    uint32_t live = 0;
    for (tray_item *it = h->head; it != NULL; it = it->next) {
        live++;
    }
    if (live >= (uint32_t)h->max_items) {
        char text[128];
        snprintf(text, sizeof(text),
                 "this watcher hosts at most %d items", h->max_items);
        free(svc);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs", text);
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }

    tray_item *it = calloc(1, sizeof(*it));
    if (it == NULL) {
        free(svc);
        return;
    }
    it->owner_host = h;
    it->reg_arg = svc;
    it->status = ITEM_STATUS_ACTIVE;
    it->loading = 1;
    it->serial = h->serial_next != 0 ? h->serial_next : 1;
    h->serial_next = it->serial + 1;

    if (kde_form) {
        it->bus_name = xstrdup_n(svc, 255);
        it->object_path = xstrdup_n(ITEM_DEFAULT_PATH, 255);
        if (sender != NULL && sender[0] == ':') {
            /* The item registered its own unique name, or a
             * well-known name it owns — the sender IS reachable. */
            it->sender_uniq = xstrdup_n(sender, 255);
        }
        if (it->sender_uniq != NULL) {
            item_subscribe_signals(it);
            item_fetch_props(it);
        } else {
            /* A well-known name we have not resolved yet: bridge
             * through the name watcher (unique-owner tracking). */
            it->name_watch = fdk_dbus_connection_watch_name(
                h->bus, svc, item_name_event, it);
        }
    } else {
        /* Ayatana dialect: the object path IS the registration; the
         * item is the CALLER. */
        it->object_path = xstrdup_n(svc, 255);
        if (sender != NULL && sender[0] == ':') {
            it->sender_uniq = xstrdup_n(sender, 255);
        }
        if (it->sender_uniq != NULL) {
            item_subscribe_signals(it);
            item_fetch_props(it);
        } else {
            tracef("Ayatana registration without a sender — dropped");
            /* Unreachable without a sender: dropped. */
            item_free(it);
            return;
        }
    }

    /* Append to the list. */
    it->next = NULL;
    it->prev = NULL;
    if (h->head == NULL) {
        h->head = it;
    } else {
        tray_item *tail = h->head;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = it;
        it->prev = tail;
    }

    /* Reply OK (the spec has no return value). */
    if (!(fdk_dbus_message_flags(call) &
          FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_connection_send(h->bus, rep);
        }
    }

    /* The announcements. */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        W_PATH, W_IFACE, "StatusNotifierItemRegistered");
    if (sig != NULL &&
        fdk_ok(fdk_dbus_message_append_string(sig, svc))) {
        fdk_dbus_connection_send(h->bus, sig);
    } else if (sig != NULL) {
        fdk_dbus_message_free(sig);
    }
    emit_items_changed(h);

    /* A loading item changed the enumerable set too. */
    tray_emit(h, FDK_SHELL_TRAY_EV_ITEMS_CHANGED, it->serial);
}

static void serve_register_host(fdk_shell_tray_host *h,
                                const fdk_dbus_message *call) {
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    const char *service = NULL;
    if (fdk_dbus_message_read_args(call, &args, &n) == FDK_OK &&
        n == 1 && args != NULL &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_STRING) {
        service = fdk_dbus_value_string(args[0]);
    }
    char *svc = service != NULL ? xstrdup_n(service, 255) : NULL;
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
    if (svc == NULL || !valid_bus_name(svc)) {
        free(svc);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "RegisterStatusNotifierHost requires one bus name");
        if (err != NULL) {
            fdk_dbus_connection_send(h->bus, err);
        }
        return;
    }
    /* This host renders; a second host's registration is recorded in
     * the trace (the single-renderer posture is D-007's decision —
     * the LAST host to register wins the IsStatusNotifierHostRegistered
     * truth, which is this one while we serve). */
    tracef("host registered: %s (sender %s)", svc,
           fdk_dbus_message_sender(call) ? fdk_dbus_message_sender(call)
                                         : "?");
    free(svc);

    if (!(fdk_dbus_message_flags(call) &
          FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
        fdk_dbus_message *rep =
            fdk_dbus_message_new_method_return(call);
        if (rep != NULL) {
            fdk_dbus_connection_send(h->bus, rep);
        }
    }
    /* The spec's announcement, once per registration. */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        W_PATH, W_IFACE, "StatusNotifierHostRegistered");
    if (sig != NULL) {
        fdk_dbus_connection_send(h->bus, sig);
    }
}

/* ---- NameOwnerChanged: the vanished-item path --------------------------- */

static void owner_changed(fdk_dbus_connection *bus,
                          fdk_dbus_message *sig, void *user) {
    (void)bus;
    fdk_shell_tray_host *h = user;
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(sig, &args, &n) != FDK_OK || n != 3 ||
        args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[1]) != FDK_DBUS_STRING ||
        fdk_dbus_value_type(args[2]) != FDK_DBUS_STRING) {
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
        return;
    }
    char *name = xstrdup_n(fdk_dbus_value_string(args[0]), 255);
    char *old = xstrdup_n(fdk_dbus_value_string(args[1]), 255);
    char *neu = xstrdup_n(fdk_dbus_value_string(args[2]), 255);
    fdk_dbus_values_free_array(args, n);
    if (name == NULL) {
        free(old);
        free(neu);
        return;
    }
    if (old != NULL && old[0] != '\0' && (neu == NULL || neu[0] == '\0')) {
        /* An owner vanished: drop every item riding it. */
        tracef("NameOwnerChanged: %s lost (%s)", name, old);
        for (;;) {
            tray_item *hit = item_find_by_sender(h, old);
            if (hit == NULL) {
                break;
            }
            item_drop(h, hit, 1);
        }
    }
    free(name);
    free(old);
    free(neu);
}

/* ---- the dbusmenu client ---------------------------------------------------
 *
 * com.canonical.dbusmenu on the ITEM's Menu object path:
 *   GetLayout(parentId i, recursionDepth i, propertyNames as)
 *       -> (revision u, layout (ia{sv}av))    TWO out-arguments
 *   AboutToShow(id i) -> (b)
 *   Event(id i, eventId s, data v, timestamp u)
 *
 * The reply shape discipline (learned the hard way, pinned by the
 * rig): read_args returns BOTH out-arguments as separate top-level
 * values — args[0] the revision uint32, args[1] the BARE root
 * struct (NOT variant-wrapped). Each child inside the root's `av`
 * IS variant-wrapped.
 */

/* Reads one (ia{sv}av) struct value into a node (children appended
 * to ->child). Depth and total-node budgets are enforced. */
static menu_node *menu_node_parse(const fdk_dbus_value *v, int depth,
                                  int *budget) {
    if (v == NULL || depth > MAX_MENU_DEPTH || *budget <= 0) {
        return NULL;
    }
    if (fdk_dbus_value_type(v) == FDK_DBUS_VARIANT) {
        v = fdk_dbus_value_variant_value(v);
    }
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_STRUCT ||
        fdk_dbus_value_struct_count(v) != 3) {
        return NULL;
    }
    const fdk_dbus_value *idv = fdk_dbus_value_struct_item(v, 0);
    const fdk_dbus_value *props = fdk_dbus_value_struct_item(v, 1);
    const fdk_dbus_value *children = fdk_dbus_value_struct_item(v, 2);
    if (fdk_dbus_value_type(idv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(props) != FDK_DBUS_ARRAY ||
        fdk_dbus_value_type(children) != FDK_DBUS_ARRAY) {
        return NULL;
    }
    menu_node *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    (*budget)--;
    node->id = fdk_dbus_value_int32(idv);
    node->enabled = 1;

    /* The properties dict. */
    size_t pc = fdk_dbus_value_array_count(props);
    for (size_t i = 0; i < pc; i++) {
        const fdk_dbus_value *entry =
            fdk_dbus_value_array_item(props, i);
        const fdk_dbus_value *kv = fdk_dbus_value_dict_key(entry);
        const fdk_dbus_value *vv = fdk_dbus_value_dict_value(entry);
        if (kv == NULL || vv == NULL) {
            continue;
        }
        if (fdk_dbus_value_type(vv) == FDK_DBUS_VARIANT) {
            vv = fdk_dbus_value_variant_value(vv);
        }
        const char *key = fdk_dbus_value_string(kv);
        if (key == NULL) {
            continue;
        }
        if (strcmp(key, "label") == 0 &&
            fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(node->label);
            node->label =
                strip_mnemonic(fdk_dbus_value_string(vv));
            tracef("menu node %d label '%s'", node->id,
                   node->label ? node->label : "");
        } else if (strcmp(key, "type") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            const char *t = fdk_dbus_value_string(vv);
            if (strcmp(t, "separator") == 0) {
                node->is_separator = 1;
            } else if (strcmp(t, "checkmark") == 0) {
                node->is_check = 1;
            } else if (strcmp(t, "radio") == 0) {
                node->is_radio = 1;
            }
        } else if (strcmp(key, "enabled") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_BOOLEAN) {
            node->enabled = fdk_dbus_value_boolean(vv);
        } else if (strcmp(key, "visible") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_BOOLEAN) {
            if (!fdk_dbus_value_boolean(vv)) {
                node->enabled = 0; /* invisible rows are not drawn   */
            }
        } else if (strcmp(key, "toggle-state") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_INT32) {
            node->toggle_state = fdk_dbus_value_int32(vv) != 0;
        } else if (strcmp(key, "icon-name") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(node->icon_name);
            node->icon_name =
                xstrdup_n(fdk_dbus_value_string(vv), MAX_ITEM_NAME);
        }
    }

    /* Children: each av element is a VARIANT wrapping the struct. */
    menu_node **tail = &node->child;
    size_t cc = fdk_dbus_value_array_count(children);
    for (size_t i = 0; i < cc && *budget > 0; i++) {
        menu_node *kid = menu_node_parse(
            fdk_dbus_value_array_item(children, i), depth + 1, budget);
        if (kid == NULL) {
            continue;
        }
        kid->owner = node;
        *tail = kid;
        tail = &kid->next;
    }
    return node;
}

/* GetLayout's reply handler: replaces the item's cached tree, then
 * (if this was a refresh of an OPEN menu) the open cascade repaints
 * by teardown+reopen at the next interaction — the honest simple
 * contract; mid-open relayouts are rare (the rig never needs one). */

static void getlayout_reply(fdk_dbus_connection *bus,
                            fdk_dbus_message *reply, void *user) {
    (void)bus;
    menu_fetch_ctx *ctx = user;
    tray_item *it = ctx->item;
    fdk_shell_tray_host *h = it->owner_host;
    it->layout_p = NULL; /* FIRST: item may be gone next line       */
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        tracef("GetLayout failed for %s", it->reg_arg ? it->reg_arg : "?");
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(reply, &args, &n) != FDK_OK ||
        n != 2 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_UINT32 ||
        fdk_dbus_value_type(args[1]) != FDK_DBUS_STRUCT) {
        fdk_dbus_values_free_array(args, n);
        return;
    }
    uint32_t revision = fdk_dbus_value_uint32(args[0]);
    int budget = MAX_MENU_NODES;
    menu_node *tree = menu_node_parse(args[1], 0, &budget);
    fdk_dbus_values_free_array(args, n);
    if (tree == NULL) {
        tracef("GetLayout: unparseable tree for %s",
               it->reg_arg ? it->reg_arg : "?");
        return;
    }
    tracef("GetLayout rev=%u nodes<=%d for %s", revision,
           MAX_MENU_NODES - budget, it->reg_arg ? it->reg_arg : "?");
    if (it->menu_open) {
        /* The menu is OPEN on this item: the open levels' rows point
         * INTO the old tree — replacing it now would dangle every
         * row. The refresh is discarded (the tree the menu was built
         * from stays until the next open). */
        tracef("GetLayout reply discarded: menu open (stale-safe)");
        menu_node_free(tree);
    } else {
        menu_node *root_old = it->menu_cache;
        it->menu_revision = (int32_t)revision;
        it->menu_cache = tree;
        if (root_old != NULL && root_old != tree) {
            menu_node_free(root_old);
        }
        tray_emit(h, FDK_SHELL_TRAY_EV_MENU_CHANGED, it->serial);
    }
}

/* Issues GetLayout(0, -1, []). prefetch=1 at item load (the
 * first right-click must WORK — lazy-load's no-op first click was
 * the bug); prefetch=0 when the menu opens. */
static void item_fetch_menu_ctx(tray_item *it, int prefetch) {
    fdk_shell_tray_host *h = it->owner_host;
    if (it->sender_uniq == NULL || it->menu_path == NULL) {
        return;
    }
    if (it->layout_p != NULL) {
        return; /* one in flight                             */
    }
    it->menu_fetch.item = it;
    it->menu_fetch.prefetch = prefetch;
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        it->sender_uniq, it->menu_path, MENU_IFACE, "GetLayout");
    fdk_dbus_value *empty = fdk_dbus_value_new_array("s", NULL, 0);
    int ok = call != NULL && empty != NULL &&
             fdk_ok(fdk_dbus_message_append_int32(call, 0)) &&
             fdk_ok(fdk_dbus_message_append_int32(call, -1)) &&
             fdk_ok(fdk_dbus_message_append(call, empty));
    fdk_dbus_value_free(empty);
    if (!ok) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        h->bus, call, 3000, getlayout_reply, &it->menu_fetch);
    if (p == NULL) {
        return;
    }
    it->layout_p = p;
    tracef("GetLayout(%s) prefetch=%d", it->menu_path, prefetch);
}

static void item_send_new_prefetch(tray_item *it) {
    /* Called when an item finishes loading: prefetch its menu so the
     * FIRST right-click opens instantly. */
    if (it->has_menu && it->menu_path != NULL &&
        it->menu_cache == NULL) {
        item_fetch_menu_ctx(it, 1);
    }
}

/* AboutToShow(id) — the open-path handshake. */
static void abouttoshow_reply(fdk_dbus_connection *bus,
                              fdk_dbus_message *reply, void *user) {
    (void)bus;
    (void)reply;
    (void)user; /* the refresh decision rides on GetLayout anyway */
}

static void menu_about_to_show(tray_item *it, int32_t id) {
    fdk_shell_tray_host *h = it->owner_host;
    if (it->sender_uniq == NULL || it->menu_path == NULL) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        it->sender_uniq, it->menu_path, MENU_IFACE, "AboutToShow");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_int32(call, id))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        h->bus, call, 2000, abouttoshow_reply, NULL);
    (void)p;
    tracef("AboutToShow(%d)", id);
}

/* Event(id, eventId, data, timestamp) — fire-and-forget. */
static void menu_event(tray_item *it, int32_t id, const char *event_id) {
    fdk_shell_tray_host *h = it->owner_host;
    if (it->sender_uniq == NULL || it->menu_path == NULL) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        it->sender_uniq, it->menu_path, MENU_IFACE, "Event");
    fdk_dbus_value *data_inner = fdk_dbus_value_new_int32(0);
    fdk_dbus_value *data_var = data_inner != NULL
                                   ? fdk_dbus_value_new_variant(data_inner)
                                   : NULL;
    fdk_dbus_value_free(data_inner);
    int ok = call != NULL && data_var != NULL &&
             fdk_ok(fdk_dbus_message_append_int32(call, id)) &&
             fdk_ok(fdk_dbus_message_append_string(call, event_id)) &&
             fdk_ok(fdk_dbus_message_append(call, data_var)) &&
             fdk_ok(fdk_dbus_message_append_uint32(
                 call, (uint32_t)(now_ms() & 0xFFFFFFFFu)));
    fdk_dbus_value_free(data_var);
    if (!ok) {
        fdk_dbus_message_free(call);
        return;
    }
    /* The item protocol's Event carries NO_REPLY_EXPECTED in every
     * real implementation — matched here (the probe logs it). */
    fdk_dbus_message_set_flags(call, FDK_DBUS_FLAG_NO_REPLY_EXPECTED);
    fdk_dbus_connection_send(h->bus, call);
    tracef("Event(id=%d, %s)", id, event_id);
}

/* Activate / SecondaryActivate / Scroll on the ITEM itself. */
static void item_call_simple(tray_item *it, const char *member, int x,
                             int y) {
    fdk_shell_tray_host *h = it->owner_host;
    if (it->sender_uniq == NULL || it->object_path == NULL) {
        return;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        it->sender_uniq, it->object_path, ITEM_IFACE_KDE, member);
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_int32(call, x)) ||
        !fdk_ok(fdk_dbus_message_append_int32(call, y))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        h->bus, call, 2000, NULL, NULL);
    (void)p;
    tracef("%s(%d, %d) -> %s", member, x, y, it->reg_arg);
}

/* ---- public snapshot APIs ------------------------------------------------ */

/* Field copy helper: *ptr is a caller buffer of *len capacity on
 * entry; on return *len is the bytes copied (excluding NUL). */
static void copy_field(char *ptr, size_t *len, const char *src) {
    if (ptr == NULL || len == NULL) {
        return;
    }
    size_t cap = *len;
    size_t n = 0;
    if (src != NULL) {
        n = strlen(src);
        if (n > cap) {
            n = cap;
        }
        memcpy(ptr, src, n);
    }
    if (cap > 0) {
        ptr[n] = '\0';
    }
    *len = n;
}

uint32_t fdk_shell_tray_item_count(const fdk_shell_tray_host *host) {
    if (host == NULL) {
        return 0;
    }
    uint32_t n = 0;
    for (tray_item *it = host->head; it != NULL; it = it->next) {
        n++;
    }
    return n;
}

fdk_result fdk_shell_tray_item_snapshot(const fdk_shell_tray_host *host,
                                        uint32_t index,
                                        fdk_shell_tray_item_info *out) {
    if (host == NULL || out == NULL ||
        out->struct_size != sizeof(*out)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const tray_item *it = item_at_c(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    out->serial = it->serial;
    out->loading = it->loading;
    out->failed = it->failed;
    out->status = (fdk_shell_tray_item_status)it->status;
    copy_field(out->reg_arg, &out->reg_arg_len, it->reg_arg);
    copy_field(out->title, &out->title_len, it->title);
    copy_field(out->icon_name, &out->icon_name_len, it->icon_name);
    copy_field(out->attention_icon_name, &out->attention_icon_name_len,
               it->attn_icon_name);
    copy_field(out->tooltip_title, &out->tooltip_title_len,
               it->tooltip.title);
    copy_field(out->tooltip_description, &out->tooltip_description_len,
               it->tooltip.description);
    out->pixmap_count = (uint32_t)it->pixmap_count;
    out->attention_pixmap_count = (uint32_t)it->attn_pixmap_count;
    out->has_menu = it->has_menu;
    out->item_is_menu = it->item_is_menu;
    return FDK_OK;
}

static const pixmap *pixmap_pick(const tray_item *it, int attention,
                                 uint32_t pixmap_index) {
    const pixmap *list = attention ? it->attn_pixmaps : it->pixmaps;
    size_t count = attention ? it->attn_pixmap_count : it->pixmap_count;
    if (list == NULL || pixmap_index >= count) {
        return NULL;
    }
    return &list[pixmap_index];
}

fdk_result fdk_shell_tray_item_pixmap_size(
    const fdk_shell_tray_host *host, uint32_t index, int attention,
    uint32_t pixmap_index, int *out_w, int *out_h) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const tray_item *it = item_at_c(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    const pixmap *pm = pixmap_pick(it, attention, pixmap_index);
    if (pm == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (out_w != NULL) {
        *out_w = pm->width;
    }
    if (out_h != NULL) {
        *out_h = pm->height;
    }
    return FDK_OK;
}

fdk_result fdk_shell_tray_item_pixmap_pixels(
    const fdk_shell_tray_host *host, uint32_t index, int attention,
    uint32_t pixmap_index, uint8_t *buf, size_t cap, size_t *out_needed) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const tray_item *it = item_at_c(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    const pixmap *pm = pixmap_pick(it, attention, pixmap_index);
    if (pm == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    size_t need = (size_t)pm->width * (size_t)pm->height * 4u;
    if (out_needed != NULL) {
        *out_needed = need;
    }
    if (buf == NULL) {
        return FDK_OK; /* the documented probe form (dimensions +
                        * byte count only)                        */
    }
    if (cap < need) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memcpy(buf, pm->pixels, need);
    return FDK_OK;
}

/* ---- public interaction APIs ---------------------------------------------- */

fdk_result fdk_shell_tray_item_activate(fdk_shell_tray_host *host,
                                        uint32_t index, int x, int y) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    item_call_simple(it, "Activate", x, y);
    return FDK_OK;
}

fdk_result fdk_shell_tray_item_secondary_activate(
    fdk_shell_tray_host *host, uint32_t index, int x, int y) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    item_call_simple(it, "SecondaryActivate", x, y);
    return FDK_OK;
}

fdk_result fdk_shell_tray_item_scroll(fdk_shell_tray_host *host,
                                      uint32_t index, int dx, int dy) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    item_call_simple(it, "Scroll", dx, dy);
    return FDK_OK;
}

/* ---- public menu APIs ------------------------------------------------------ */

/* The preorder walk (root first, then children). */
static void menu_walk_nodes(const menu_node *node, int depth,
                            fdk_shell_tray_menu_visit_fn visit,
                            void *user) {
    for (; node != NULL; node = node->next) {
        fdk_shell_tray_menu_node_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        info.id = node->id;
        info.label = node->label;
        info.is_separator = node->is_separator;
        info.is_check = node->is_check;
        info.is_radio = node->is_radio;
        info.toggle_state = node->toggle_state;
        info.enabled = node->enabled;
        info.icon_name = node->icon_name;
        info.has_children = node->child != NULL;
        visit(&info, depth, user);
        if (node->child != NULL) {
            menu_walk_nodes(node->child, depth + 1, visit, user);
        }
    }
}

fdk_result fdk_shell_tray_menu_walk(const fdk_shell_tray_host *host,
                                    uint32_t index,
                                    fdk_shell_tray_menu_visit_fn visit,
                                    void *user) {
    if (host == NULL || visit == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const tray_item *it = item_at_c(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (it->menu_cache == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    menu_walk_nodes(it->menu_cache, 0, visit, user);
    return FDK_OK;
}

fdk_result fdk_shell_tray_menu_open(fdk_shell_tray_host *host,
                                    uint32_t index) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    it->menu_open = 1;
    menu_about_to_show(it, 0);
    /* The open-path refresh (the reference UI's posture: a re-fetch
     * rides every open; while open, replies stay stale-safe). */
    item_fetch_menu_ctx(it, 0);
    return FDK_OK;
}

fdk_result fdk_shell_tray_menu_close(fdk_shell_tray_host *host,
                                     uint32_t index) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (it->menu_open) {
        it->menu_open = 0;
        menu_event(it, 0, "closed");
    }
    return FDK_OK;
}

fdk_result fdk_shell_tray_menu_event(fdk_shell_tray_host *host,
                                     uint32_t index, int32_t node_id,
                                     const char *event_id) {
    if (host == NULL || event_id == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    menu_event(it, node_id, event_id);
    return FDK_OK;
}

fdk_result fdk_shell_tray_menu_refresh(fdk_shell_tray_host *host,
                                       uint32_t index) {
    if (host == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    tray_item *it = item_at(host, index);
    if (it == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    if (it->menu_open) {
        /* A refresh while open is deferred to the next open (the
         * stale-safe contract). */
        return FDK_OK;
    }
    item_fetch_menu_ctx(it, 0);
    return FDK_OK;
}

/* ---- the method dispatcher ------------------------------------------------- */

static const char *k_introspect_xml =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
    "Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect."
    "dtd\">\n"
    "<node>\n"
    "  <interface name=\"org.kde.StatusNotifierWatcher\">\n"
    "    <method name=\"RegisterStatusNotifierItem\">\n"
    "      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"RegisterStatusNotifierHost\">\n"
    "      <arg name=\"service\" type=\"s\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <property name=\"RegisteredStatusNotifierItems\" "
    "type=\"as\" access=\"read\"/>\n"
    "    <property name=\"IsStatusNotifierHostRegistered\" "
    "type=\"b\" access=\"read\"/>\n"
    "    <property name=\"ProtocolVersion\" type=\"i\" "
    "access=\"read\"/>\n"
    "    <property name=\"IconTheme\" type=\"s\" access=\"read\"/>\n"
    "    <signal name=\"StatusNotifierItemRegistered\">\n"
    "      <arg name=\"service\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"StatusNotifierItemUnregistered\">\n"
    "      <arg name=\"service\" type=\"s\"/>\n"
    "    </signal>\n"
    "    <signal name=\"StatusNotifierHostRegistered\"/>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
    "    </method>\n"
    "    <method name=\"Set\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"name\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"value\" type=\"v\" direction=\"in\"/>\n"
    "    </method>\n"
    "    <method name=\"GetAll\">\n"
    "      <arg name=\"interface\" type=\"s\" direction=\"in\"/>\n"
    "      <arg name=\"properties\" type=\"a{sv}\" "
    "direction=\"out\"/>\n"
    "    </method>\n"
    "    <signal name=\"PropertiesChanged\">\n"
    "      <arg name=\"interface\" type=\"s\"/>\n"
    "      <arg name=\"changed_properties\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated_properties\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\">\n"
    "      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "    <method name=\"GetMachineId\">\n"
    "      <arg name=\"machine_uuid\" type=\"s\" direction=\"out\"/>\n"
    "    </method>\n"
    "  </interface>\n"
    "</node>\n";

static void serve_ping(fdk_shell_tray_host *h,
                       const fdk_dbus_message *call) {
    fdk_dbus_message *rep =
        fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(h->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    }
}

static void serve_get_machine_id(fdk_shell_tray_host *h,
                                 const fdk_dbus_message *call) {
    char id[64] = "";
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (f != NULL) {
        if (fgets(id, sizeof(id), f) == NULL) {
            id[0] = '\0';
        }
        fclose(f);
        /* strip a trailing newline if present */
        size_t n = strlen(id);
        while (n > 0 && (id[n - 1] == '\n' || id[n - 1] == '\r')) {
            id[--n] = '\0';
        }
    }
    fdk_dbus_message *rep =
        fdk_dbus_message_new_method_return(call);
    if (rep != NULL) {
        if (fdk_ok(fdk_dbus_message_append_string(rep, id))) {
            if (!(fdk_dbus_message_flags(call) &
                  FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                fdk_dbus_connection_send(h->bus, rep);
            } else {
                fdk_dbus_message_free(rep);
            }
        } else {
            fdk_dbus_message_free(rep);
        }
    }
}

static void method_handler(fdk_dbus_connection *bus,
                           fdk_dbus_message *call, void *user) {
    fdk_shell_tray_host *h = user;
    (void)bus;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);

    if (iface != NULL && strcmp(iface, W_IFACE) == 0) {
        if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
            serve_register_item(h, call);
            return;
        }
        if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
            serve_register_host(h, call);
            return;
        }
    } else if (iface != NULL && strcmp(iface, PROPS_IFACE) == 0) {
        if (strcmp(member, "Get") == 0) {
            watcher_get(h, call);
            return;
        }
        if (strcmp(member, "GetAll") == 0) {
            watcher_getall(h, call);
            return;
        }
        if (strcmp(member, "Set") == 0) {
            watcher_set(h, call);
            return;
        }
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0) {
        if (strcmp(member, "Introspect") == 0) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_string(rep,
                                                      k_introspect_xml))) {
                if (!(fdk_dbus_message_flags(call) &
                      FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                    fdk_dbus_connection_send(h->bus, rep);
                } else {
                    fdk_dbus_message_free(rep);
                }
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            return;
        }
    } else if (iface != NULL &&
               strcmp(iface, "org.freedesktop.DBus.Peer") == 0) {
        if (strcmp(member, "Ping") == 0) {
            serve_ping(h, call);
            return;
        }
        if (strcmp(member, "GetMachineId") == 0) {
            serve_get_machine_id(h, call);
            return;
        }
    }
    /* Unknown method: the honest error (a caller polling us must
     * never burn a timeout on silence). */
    fdk_dbus_message *err = fdk_dbus_message_new_error(
        call, "org.freedesktop.DBus.Error.UnknownMethod",
        "The tray host does not implement this method");
    if (err != NULL) {
        fdk_dbus_connection_send(h->bus, err);
    }
}

/* ---- the name etiquette (the reference matrix, verbatim) ----------------- */

/* The self-registration: after the name is ours, the HOST registers
 * with the WATCHER over the real bus path (our own handler serves
 * the call — exactly as a third party's would be). */
static void self_register_host(fdk_shell_tray_host *h) {
    const char *me = fdk_dbus_connection_unique_name(h->bus);
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        W_NAME, W_PATH, W_IFACE, "RegisterStatusNotifierHost");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(
            call, me != NULL ? me : ""))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        h->bus, call, 2000, NULL, NULL);
    (void)p;
    tracef("self-registered as host: %s", me != NULL ? me : "");
}

/* The RequestName reply: EXISTS is the refusal that must reach the
 * host app (a --replace against a stubborn incumbent watcher) — the
 * event is HOST_STATE(0); the app decides to exit. IN_QUEUE is the
 * --wait posture (serving when the incumbent releases; the
 * NameAcquired path completes it). */
static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user) {
    (void)bus;
    fdk_shell_tray_host *h = user;
    uint32_t code = 0;
    if (reply != NULL && !fdk_dbus_message_is_error(reply)) {
        fdk_dbus_value **args = NULL;
        size_t n = 0;
        if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
            n == 1 && args != NULL &&
            fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32) {
            code = fdk_dbus_value_uint32(args[0]);
        }
        if (args != NULL) {
            fdk_dbus_values_free_array(args, n);
        }
    }
    if (code == FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER ||
        code == FDK_DBUS_REQUEST_NAME_ALREADY_OWNER) {
        return; /* NameAcquired usually beat the reply here        */
    }
    if (code == FDK_DBUS_REQUEST_NAME_IN_QUEUE) {
        return; /* --wait: serving when the incumbent releases     */
    }
    /* EXISTS (or an error reply): the refusal. */
    if (!h->serving) {
        h->refused = 1;
        tray_emit(h, FDK_SHELL_TRAY_EV_HOST_STATE, 0);
    }
}

static void request_the_name(fdk_shell_tray_host *h) {
    uint32_t flags = FDK_DBUS_NAME_ALLOW_REPLACEMENT;
    if (h->opt_replace) {
        flags |= FDK_DBUS_NAME_REPLACE_EXISTING;
    }
    if (!h->opt_wait) {
        flags |= FDK_DBUS_NAME_DO_NOT_QUEUE;
    }
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, W_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, flags))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        h->bus, call, 5000, request_name_reply, h);
    (void)p;
}

static void name_owner_event(fdk_dbus_connection *bus, const char *name,
                             const char *owner, void *user) {
    (void)name;
    fdk_shell_tray_host *h = user;
    const char *me = fdk_dbus_connection_unique_name(bus);
    if (owner != NULL && me != NULL && strcmp(owner, me) == 0) {
        return; /* our own acquisition echoing through the watcher   */
    }
    if (owner != NULL && !h->opt_replace && !h->opt_wait &&
        !h->serving) {
        /* The incumbent refusal: HOST_STATE(0), stay dormant. */
        if (!h->refused) {
            h->refused = 1;
            tray_emit(h, FDK_SHELL_TRAY_EV_HOST_STATE, 0);
        }
        return;
    }
    if (!h->refused && !h->serving &&
        (owner == NULL || h->opt_replace || h->opt_wait)) {
        request_the_name(h);
    }
}

static void name_event(fdk_dbus_connection *bus, const char *name,
                       int owned, void *user) {
    (void)bus;
    fdk_shell_tray_host *h = user;
    if (name == NULL || strcmp(name, W_NAME) != 0) {
        return; /* the unique-name NameAcquired is not ours to serve */
    }
    if (owned) {
        if (!h->serving) {
            h->serving = 1;
            self_register_host(h);
            tray_emit(h, FDK_SHELL_TRAY_EV_HOST_STATE, 1);
        }
    } else if (h->serving) {
        h->serving = 0;
        tray_emit(h, FDK_SHELL_TRAY_EV_HOST_STATE, 0);
    }
}

/* ---- lifecycle --------------------------------------------------------------- */

fdk_result fdk_shell_tray_host_create(
    fdk_dbus_connection *bus, const fdk_shell_tray_host_desc *desc,
    fdk_shell_tray_host **out) {
    if (bus == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc != NULL && desc->struct_size != 0 &&
        desc->struct_size != sizeof(*desc)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    int replace = desc != NULL ? desc->replace : 0;
    int wait = desc != NULL ? desc->wait : 0;
    if (replace && wait) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!fdk_dbus_connection_ready(bus)) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    fdk_shell_tray_host *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    h->bus = bus;
    h->event_cb = desc != NULL ? desc->event_cb : NULL;
    h->event_user = desc != NULL ? desc->user : NULL;
    h->max_items = desc != NULL && desc->max_items > 0
                       ? desc->max_items
                       : MAX_ITEMS_DEFAULT;
    h->opt_replace = replace;
    h->opt_wait = wait;

    /* The watcher's served surface: our handler owns the
     * connection's method dispatch from here on. */
    fdk_dbus_connection_set_method_handler(bus, method_handler, h);

    /* NameAcquired/NameLost -> the serving transitions. */
    fdk_dbus_connection_set_name_events(bus, name_event, h);

    /* The NameOwnerChanged subscription: vanished items drop here.
     * The subscription is HELD until teardown (freeing it issues
     * RemoveMatch — the vanished-item path would never fire). */
    char *rule = fdk_dbus_match_rule(
        "signal", "org.freedesktop.DBus", NULL, "NameOwnerChanged", NULL,
        NULL);
    if (rule != NULL) {
        h->owner_sub = fdk_dbus_connection_subscribe(bus, rule,
                                                     owner_changed, h);
        free(rule);
    }

    /* The etiquette: watch the well-known name; the watcher's
     * immediate state drives request-vs-refuse. */
    h->self_watch = fdk_dbus_connection_watch_name(
        bus, W_NAME, name_owner_event, h);
    if (h->self_watch == NULL) {
        fdk_dbus_subscription_free(h->owner_sub);
        fdk_dbus_connection_set_method_handler(bus, NULL, NULL);
        fdk_dbus_connection_set_name_events(bus, NULL, NULL);
        free(h);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    *out = h;
    return FDK_OK;
}

void fdk_shell_tray_host_destroy(fdk_shell_tray_host *host) {
    if (host == NULL || host->dead) {
        return;
    }
    host->dead = 1; /* the events must not fire into a dying host   */

    /* Drop every item (announcements while we still own the name). */
    while (host->head != NULL) {
        item_drop(host, host->head, 0);
    }

    fdk_dbus_name_watcher_free(host->self_watch);
    fdk_dbus_subscription_free(host->owner_sub);

    /* Release the well-known name (queued messages flush at close
     * or on the next RequestName elsewhere; the release is fire-and-
     * forget exactly like the reference). */
    if (host->serving && host->bus != NULL) {
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "ReleaseName");
        if (call != NULL &&
            fdk_ok(fdk_dbus_message_append_string(call, W_NAME))) {
            fdk_dbus_pending *p =
                fdk_dbus_connection_call(host->bus, call, 2000,
                                         NULL, NULL);
            (void)p;
        } else if (call != NULL) {
            fdk_dbus_message_free(call);
        }
    }

    /* Detach from the connection (the app owns the connection's
     * life; a fresh model may install itself next). */
    if (host->bus != NULL) {
        fdk_dbus_connection_set_method_handler(host->bus, NULL, NULL);
        fdk_dbus_connection_set_name_events(host->bus, NULL, NULL);
    }

    free(host);
}

int fdk_shell_tray_host_serving(const fdk_shell_tray_host *host) {
    return host != NULL ? host->serving : 0;
}
