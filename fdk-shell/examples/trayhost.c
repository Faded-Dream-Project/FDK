/* trayhost.c — the fdk-shell StatusNotifier tray host (S-1 / A-11).
 *
 * A complete org.kde.StatusNotifierWatcher HOST on FDK's native D-Bus
 * client, rendering the registered StatusNotifierItems as a floating
 * top-right fdk-shell TOP-layer strip, with their com.canonical
 * .dbusmenu menus as popup cascades. Written from the public SNI and
 * dbusmenu specification shapes — no tray host's code was read or
 * ported (the license posture, docs/THIRD-PARTY.md).
 *
 * The three roles, all three live:
 *   - org.kde.StatusNotifierWatcher — the registration service this
 *     process OWNS: RegisterStatusNotifierItem (both registration
 *     dialects: the KDE form — a bus name with the canonical
 *     /StatusNotifierItem path — and the Ayatana form — a bare object
 *     path, the caller being the item), RegisterStatusNotifierHost,
 *     the four properties (RegisteredStatusNotifierItems,
 *     IsStatusNotifierHostRegistered, ProtocolVersion, IconTheme)
 *     through org.freedesktop.DBus.Properties, the
 *     StatusNotifierItemRegistered/Unregistered/HostRegistered
 *     signals, Introspect and Peer for bus tools.
 *   - the HOST: after taking the Watcher name the process registers
 *     ITSELF through the real bus path (the RegisterStatusNotifierHost
 *     call goes out over the socket and is served by our own handler
 *     like any third party's would be).
 *   - the RENDERER: one floating layer strip at the top-right (a cell
 *     per item), NeedsAttention drawn with an accent marker, Passive
 *     drawn faded (alpha-blended, never hidden — the honest middle),
 *     left-click -> Activate, middle-click -> SecondaryActivate,
 *     right-click -> the item's dbusmenu as a right-aligned popup
 *     cascade with screen-edge flipping.
 *
 * The item protocol, as spoken to real items:
 *   - GetAll(org.kde.StatusNotifierItem) with the interface fallback
 *     chain org.kde -> org.ayatana -> org.freedesktop (items differ),
 *     the pixmap list decoded from a(iiay) ARGB32 network byte order,
 *     the ToolTip struct, the Menu object path.
 *   - New* signal subscriptions scoped to the item's UNIQUE bus name
 *     (":1.x") — the header value the bus rewrites well-known names
 *     into; a rule scoped to the well-known name itself would never
 *     fire. Unique names are tracked through NameOwnerChanged, which
 *     is also how a vanished item is dropped (hostile clients never
 *     unregister politely).
 *   - the menu: GetLayout(0, -1, []) — the reply's TWO out-arguments
 *     (u revision + the bare root struct (ia{sv}av)) — AboutToShow
 *     before each open, Event(clicked/opened/closed) on interaction,
 *     mnemonic underscores stripped from labels ("Fi_rst" draws as
 *     "First"), separators, checkmarks and radios, disabled rows.
 *
 * The untrusted-input posture: every byte over the bus is hostile.
 * Registration arguments are validated against the bus-name and
 * object-path grammars before any subscription exists; property and
 * menu payloads are shape-checked and size-capped (node count, depth,
 * label bytes, pixmap geometry); every malformed message is refused
 * or ignored and the host stays alive.
 *
 * Bus-name etiquette: same matrix as notifyd — decline with one clear
 * line against an incumbent, --replace takes over from hosts that
 * allowed replacement, --wait queues, a replaced host exits 0.
 *
 * Trace: FDK_TRAYHOST_TRACE=1 logs every registration, property
 * fetch, menu transaction, and placement decision to stderr.
 *
 * Build:  make -C fdk-shell examples
 * Run:    ./fdk-shell/build/examples/trayhost [--margin N] [--cell N]
 *               [--bar-height N] [--output N] [--anchor tr|tl|br|bl]
 *               [--replace] [--wait] [--quit-after S] [--help]
 *               [--version]
 *
 * Status honesty (the Phase-2 rule): what the rigs prove is DONE;
 * what only a real desktop can prove is marked in
 * docs/TESTING-ON-DESKTOP.md for the owner.
 */

#include <fdk-shell/fdk-shell.h>

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_theme.h"

#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- constants ------------------------------------------------------- */

#define W_NAME "org.kde.StatusNotifierWatcher"
#define W_PATH "/StatusNotifierWatcher"
#define W_IFACE "org.kde.StatusNotifierWatcher"

#define ITEM_IFACE_KDE "org.kde.StatusNotifierItem"
#define ITEM_IFACE_AYATANA "org.ayatana.StatusNotifierItem"
#define ITEM_IFACE_FREEDESKTOP "org.freedesktop.StatusNotifierItem"
#define ITEM_DEFAULT_PATH "/StatusNotifierItem"

#define MENU_IFACE "com.canonical.dbusmenu"

#define PROPS_IFACE "org.freedesktop.DBus.Properties"

/* Input caps — everything beyond these is truncated or dropped. */
#define MAX_ITEMS 32
#define MAX_ITEM_NAME 256
#define MAX_TITLE_BYTES 512
#define MAX_TOOLTIP_BYTES 512
#define MAX_PIXMAP_DIM 256
#define MAX_PIXMAP_BYTES (4 * MAX_PIXMAP_DIM * MAX_PIXMAP_DIM)
#define MAX_PIXMAPS 16      /* a(iiay) entries accepted per array   */
#define MAX_MENU_NODES 200  /* GetLayout parse budget              */
#define MAX_MENU_DEPTH 8    /* cascade depth budget                */
#define MAX_LABEL_BYTES 256
#define MAX_MENU_WIDTH 300

/* Bar metrics (logical px). */
#define BAR_PAD 3
#define BAR_RADIUS 6
#define ICON_INSET 1        /* cell border around the drawn icon    */

/* Menu metrics (logical px). */
#define MENU_ROW_H 28
#define MENU_PAD_X 10
#define MENU_SEP_H 9
#define MENU_ARROW_W 18

/* ---- logging ------------------------------------------------------------ */

static int g_trace; /* FDK_TRAYHOST_TRACE=1 */

static void tracef(const char *fmt, ...) {
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

static void sayf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("trayhost: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    va_end(ap);
}

/* ---- options ------------------------------------------------------------ */

typedef enum { ANCHOR_TR = 0, ANCHOR_TL, ANCHOR_BR, ANCHOR_BL } anchor_kind;

typedef struct {
    int bar_height;  /* the strip's height          */
    int cell;        /* one item's cell             */
    int edge_margin; /* gap from the screen edges   */
    anchor_kind anchor;
    int output_index;    /* -1 = every output            */
    int replace;
    int wait;
    double quit_after_s; /* <=0 = run forever (rigs)     */
} options;

/* ---- the item model ------------------------------------------------------ */

typedef struct {
    int width;
    int height;
    uint32_t *pixels; /* ARGB8888 host order, owned   */
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
    menu_node *next;   /* sibling list                          */
    menu_node *child;  /* first child (when a submenu)          */
    int32_t id;
    char *label;       /* mnemonic-stripped, owned              */
    int is_separator;
    int is_check;
    int is_radio;
    int toggle_state;  /* 0/1 (radio + check)                   */
    int enabled;
    char *icon_name;   /* dbusmenu "icon-name" property, owned  */
    menu_node *owner;  /* submenu rows point at their parent    */
    int opened;        /* an "opened" Event was sent (teardown) */
};

/* One cascade level's runtime state (the open menu's shape). */
typedef struct menu_level2 {
    struct menu_level2 *parent; /* NULL for the root level         */
    fdk_shell_popup *popup;
    struct menu_row2 *rows; /* built below                        */
    int row_count;   /* rows (separators included)              */
    int width;
    int height;
    int hover;        /* -1 none, else row index               */
} menu_level2;

typedef struct menu_row2 {
    menu_node *node; /* NULL for the separator pseudo-row        */
    int y;
    int h;
} menu_row2;

/* The open cascade's state. */
typedef struct tray_item tray_item;
typedef struct {
    tray_item *item;
    menu_level2 *root;
} menu_state;

static menu_state g_menu;

/* The cascade's levels (index 0 = root) + each popup's creation x
 * (popups carry no query API; the global-position math needs the
 * chain of offsets). */
static menu_level2 *g_levels[MAX_MENU_DEPTH + 1];
static int g_level_count;
static int g_level_x[MAX_MENU_DEPTH + 1];

typedef struct tray_item tray_item;

/* The in-flight GetLayout's user data (embedded in the item — a
 * canceled pending never fires its reply, so a heap ctx would leak). */
typedef struct menu_fetch_ctx {
    tray_item *item;
    int prefetch; /* a load-time prefetch, not an open            */
} menu_fetch_ctx;

typedef struct tray_item {
    struct tray_item *next;
    struct tray_item *prev;

    /* Identity (bus side). */
    char *reg_arg;       /* exactly what RegisterStatusNotifierItem
                          * carried — echoed in signals/property   */
    char *sender_uniq;   /* ":1.x" — the match-rule scope         */
    char *bus_name;      /* KDE-form bus name or NULL             */
    char *object_path;   /* the item's object path                */
    fdk_dbus_name_watcher *name_watch; /* for well-known forms    */

    /* Properties (GetAll results; all owned strings). */
    char *id_str;
    char *title;
    item_status status;
    char *icon_name;       /* IconName                              */
    char *attn_icon_name;  /* AttentionIconName                     */
    pixmap *pixmaps;       /* IconPixmap list                       */
    size_t pixmap_count;
    pixmap *attn_pixmaps;  /* AttentionIconPixmap list             */
    size_t attn_pixmap_count;
    tooltip_info tooltip;
    int has_menu;          /* the Menu property arrived            */
    char *menu_path;       /* the dbusmenu object path             */
    int item_is_menu;      /* ItemIsMenu: clicks open the menu     */

    /* Live state. */
    fdk_dbus_subscription *item_subs[7]; /* New* signal subs      */
    fdk_dbus_pending *getall_p;  /* in-flight GetAll              */
    fdk_dbus_pending *layout_p; /* in-flight GetLayout           */
    int loading;         /* properties not fetched yet            */
    int failed;          /* un-usable item (kept until unregister)*/

    /* The dbusmenu cache (owned; NULL until GetLayout answers). */
    menu_node *menu_cache;
    int32_t menu_revision;
    /* The in-flight GetLayout's user data — EMBEDDED (a canceled
     * pending never fires its reply, so a heap ctx would leak; the
     * embed makes cancellation free by construction). */
    menu_fetch_ctx menu_fetch;
} tray_item;

typedef struct {
    fdk_context *ctx;
    fdk_shell *shell;
    fdk_dbus_connection *bus;
    fdk_font *font;

    options opt;

    tray_item *head;  /* registration order                       */
    int item_count;   /* LOADED items (drives the bar width)      */

    fdk_shell_layer *bar;
    int bar_w, bar_h;
    int bar_x, bar_y;   /* the bar's screen origin, when known     */
    int have_bar_pos;   /* X11: computed from the anchoring math   */

    menu_level2 *menu_root; /* the open cascade root, or NULL     */
    int menu_needs_render; /* a refresh arrived mid-open          */

    /* The NameOwnerChanged subscription (vanished items) — held for
     * the connection's lifetime; freeing it earlier REMOVES the
     * match rule (subscription_free issues RemoveMatch — the drop
     * path would silently never fire). */
    fdk_dbus_subscription *owner_sub;

    int serving;
    int stopping;
    int exit_code;
    const char *stop_reason;
} trayhost;

static trayhost g_app;

/* Forward declarations (the parts below cross-reference). */
static void menu_node_free(menu_node *node);
static void bar_relayout(trayhost *app);
static void bar_paint(trayhost *app);
static void bar_layer_event(fdk_shell_layer *layer,
                             const fdk_event_data *ev, void *user);
static void menu_popup_event(fdk_shell_popup *popup,
                              const fdk_event_data *ev, void *user);
static void menu_level_paint(menu_level2 *lvl);
static int menu_global_x(menu_level2 *lvl);

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

/* A bus name: ":1.x" unique or "a.b.c" well-known. Light validation
 * (the daemon already enforced the grammar before delivery; this is
 * the hostile-input backstop for REGISTER arguments, which the bus
 * does NOT validate — they are payload, not headers). */
static int valid_bus_name(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    size_t n = strlen(s);
    if (n > 255) {
        return 0;
    }
    if (s[0] == ':') {
        /* unique names: ":1." + digits (be tolerant of the spec's
         * wider ":x.y" grammar — digits and dots) */
        if (n < 3) {
            return 0;
        }
        for (size_t i = 1; i < n; i++) {
            char c = s[i];
            if (!((c >= '0' && c <= '9') || c == '.')) {
                return 0;
            }
        }
        return 1;
    }
    /* well-known: elements of [A-Za-z0-9_-], split by '.', each
     * non-empty, at least two elements. */
    int elements = 0;
    size_t elem_len = 0;
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (c == '.' || c == '\0') {
            if (elem_len == 0 || elem_len > 63) {
                return 0;
            }
            elements++;
            elem_len = 0;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-') {
            elem_len++;
        } else {
            return 0;
        }
    }
    return elements >= 2;
}

/* An object path: '/'-prefixed, elements of [A-Za-z0-9_]. */
static int valid_object_path(const char *s) {
    if (s == NULL || s[0] != '/') {
        return 0;
    }
    size_t n = strlen(s);
    if (n > 255) {
        return 0;
    }
    if (n == 1) {
        return 1; /* the root path */
    }
    if (s[n - 1] == '/') {
        return 0;
    }
    size_t elem_len = 0;
    for (size_t i = 1; i <= n; i++) {
        char c = s[i];
        if (c == '/' || c == '\0') {
            if (elem_len == 0) {
                return 0;
            }
            elem_len = 0;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '_') {
            elem_len++;
        } else {
            return 0;
        }
    }
    return 1;
}

/* Mnemonic stripping: drop the underscore that marks a shortcut
 * ("Fi_rst" -> "First"); "__" is a literal underscore. */
static char *strip_mnemonic(const char *in) {
    if (in == NULL) {
        return NULL;
    }
    size_t n = strlen(in);
    if (n > MAX_LABEL_BYTES) {
        n = MAX_LABEL_BYTES;
    }
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
            }
            /* a single '_' is the mnemonic mark: dropped */
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
    return out;
}

/* ---- D-Bus value helpers ----------------------------------------------
 *
 * The value API's ownership rule, applied once, correctly:
 * new_array / new_struct / new_dict_entry TAKE their children (the
 * caller frees only failure-path leftovers); new_variant CLONES its
 * child. Every dict value going into an a{sv} MUST be a VARIANT —
 * a bare value produces a {ss} entry signature the array constructor
 * rejects (and the reply silently fails to build).
 */

static fdk_dbus_value *sv_string(const char *key, const char *val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_string(val ? val : "");
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner); /* the variant holds its own clone */
    if (v == NULL) {
        fdk_dbus_value_free(k);
        return NULL;
    }
    fdk_dbus_value *entry = fdk_dbus_value_new_dict_entry(k, v);
    if (entry == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
        return NULL;
    }
    return entry;
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
    if (v == NULL) {
        fdk_dbus_value_free(k);
        return NULL;
    }
    fdk_dbus_value *entry = fdk_dbus_value_new_dict_entry(k, v);
    if (entry == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
        return NULL;
    }
    return entry;
}

static fdk_dbus_value *sv_int32(const char *key, int32_t val) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    fdk_dbus_value *inner = fdk_dbus_value_new_int32(val);
    if (k == NULL || inner == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(inner);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    if (v == NULL) {
        fdk_dbus_value_free(k);
        return NULL;
    }
    fdk_dbus_value *entry = fdk_dbus_value_new_dict_entry(k, v);
    if (entry == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
        return NULL;
    }
    return entry;
}

static fdk_dbus_value *sv_str_array(const char *key,
                                    const char *const *vals, size_t count) {
    fdk_dbus_value *k = fdk_dbus_value_new_string(key);
    if (k == NULL) {
        return NULL;
    }
    fdk_dbus_value **items = NULL;
    if (count > 0) {
        items = calloc(count, sizeof(*items));
        if (items == NULL) {
            fdk_dbus_value_free(k);
            return NULL;
        }
        for (size_t i = 0; i < count; i++) {
            items[i] = fdk_dbus_value_new_string(vals[i]);
        }
    }
    fdk_dbus_value *inner = fdk_dbus_value_new_array("s", items, count);
    if (items != NULL) {
        if (inner == NULL) {
            for (size_t i = 0; i < count; i++) {
                fdk_dbus_value_free(items[i]); /* ours again on     */
            }                                /* failure only       */
        }
        free(items);
    }
    if (inner == NULL) {
        fdk_dbus_value_free(k);
        return NULL;
    }
    fdk_dbus_value *v = fdk_dbus_value_new_variant(inner);
    fdk_dbus_value_free(inner);
    if (v == NULL) {
        fdk_dbus_value_free(k);
        return NULL;
    }
    fdk_dbus_value *entry = fdk_dbus_value_new_dict_entry(k, v);
    if (entry == NULL) {
        fdk_dbus_value_free(k);
        fdk_dbus_value_free(v);
        return NULL;
    }
    return entry;
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

/* One (iiay) struct -> pixmap. ARGB32 in NETWORK byte order: the
 * byte sequence in the array is A,R,G,B per pixel. */
static int pixmap_from_struct(const fdk_dbus_value *v, pixmap *out) {
    memset(out, 0, sizeof(*out));
    if (v == NULL || fdk_dbus_value_type(v) != FDK_DBUS_STRUCT ||
        fdk_dbus_value_struct_count(v) != 3) {
        return 0;
    }
    const fdk_dbus_value *wv = fdk_dbus_value_struct_item(v, 0);
    const fdk_dbus_value *hv = fdk_dbus_value_struct_item(v, 1);
    const fdk_dbus_value *data = fdk_dbus_value_struct_item(v, 2);
    if (fdk_dbus_value_type(wv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(hv) != FDK_DBUS_INT32 ||
        fdk_dbus_value_type(data) != FDK_DBUS_ARRAY) {
        return 0;
    }
    const char *elem = fdk_dbus_value_array_signature(data);
    if (elem == NULL || elem[0] != 'y' || elem[1] != '\0') {
        return 0;
    }
    int32_t w = fdk_dbus_value_int32(wv);
    int32_t h = fdk_dbus_value_int32(hv);
    if (w <= 0 || h <= 0 || w > MAX_PIXMAP_DIM || h > MAX_PIXMAP_DIM) {
        return 0;
    }
    size_t count = fdk_dbus_value_array_count(data);
    if (count < (size_t)w * (size_t)h * 4u) {
        return 0; /* short byte array: refuse, never pad            */
    }
    if ((size_t)w * (size_t)h * 4u > MAX_PIXMAP_BYTES) {
        return 0;
    }
    uint32_t *px = calloc((size_t)w * (size_t)h, sizeof(uint32_t));
    if (px == NULL) {
        return 0;
    }
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t off = ((size_t)y * (size_t)w + (size_t)x) * 4u;
            uint32_t a = fdk_dbus_value_byte(
                fdk_dbus_value_array_item(data, off + 0));
            uint32_t r = fdk_dbus_value_byte(
                fdk_dbus_value_array_item(data, off + 1));
            uint32_t g = fdk_dbus_value_byte(
                fdk_dbus_value_array_item(data, off + 2));
            uint32_t b = fdk_dbus_value_byte(
                fdk_dbus_value_array_item(data, off + 3));
            px[(size_t)y * (size_t)w + (size_t)x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    out->width = w;
    out->height = h;
    out->pixels = px;
    return 1;
}

/* The a(iiay) array -> an owned pixmap list (bounded). */
static size_t pixmap_list_from_array(const fdk_dbus_value *arr,
                                     pixmap **out_list) {
    *out_list = NULL;
    if (arr == NULL || fdk_dbus_value_type(arr) != FDK_DBUS_ARRAY) {
        return 0;
    }
    const char *elem = fdk_dbus_value_array_signature(arr);
    if (elem == NULL || strcmp(elem, "(iiay)") != 0) {
        return 0;
    }
    size_t n = fdk_dbus_value_array_count(arr);
    if (n == 0) {
        return 0;
    }
    if (n > MAX_PIXMAPS) {
        n = MAX_PIXMAPS;
    }
    pixmap *list = calloc(n, sizeof(*list));
    if (list == NULL) {
        return 0;
    }
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        if (!pixmap_from_struct(fdk_dbus_value_array_item(arr, i),
                                 &list[got])) {
            continue; /* a bad entry is skipped, not fatal */
        }
        got++;
    }
    if (got == 0) {
        free(list);
        return 0;
    }
    *out_list = list;
    return got;
}

/* pixmap -> an fdk_surface (for drawing). */
static fdk_surface *pixmap_to_surface(const pixmap *pm) {
    if (pm == NULL || pm->pixels == NULL) {
        return NULL;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_surface_create_format(pm->width, pm->height,
                                          FDK_SURFACE_FORMAT_ARGB8888,
                                          &s))) {
        return NULL;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        fdk_surface_destroy(s);
        return NULL;
    }
    for (int y = 0; y < pm->height; y++) {
        for (int x = 0; x < pm->width; x++) {
            info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] =
                pm->pixels[(size_t)y * (size_t)pm->width + (size_t)x];
        }
    }
    return s;
}

/* ---- item lifecycle ------------------------------------------------------ */

static void bar_relayout(trayhost *app);
static void menu_close_all(trayhost *app);
static void item_fetch_menu(tray_item *it);
static void item_send_new_prefetch(tray_item *it);

static void item_free(tray_item *it) {
    if (it == NULL) {
        return;
    }
    /* Pending calls: their user pointers are ABOUT to dangle — the
     * discipline is cancel-first (the reply callback's first act is
     * clearing the handle, so a reply in flight never reaches a
     * freed item). */
    fdk_dbus_pending_cancel(it->getall_p);
    fdk_dbus_pending_cancel(it->layout_p);
    for (size_t i = 0; i < sizeof(it->item_subs) / sizeof(*it->item_subs);
         i++) {
        fdk_dbus_subscription_free(it->item_subs[i]);
    }
    fdk_dbus_name_watcher_free(it->name_watch);
    menu_node_free(it->menu_cache);
    free(it->reg_arg);
    free(it->sender_uniq);
    free(it->bus_name);
    free(it->object_path);
    free(it->id_str);
    free(it->title);
    free(it->icon_name);
    free(it->attn_icon_name);
    free(it->menu_path);
    pixmap_free_list(it->pixmaps, it->pixmap_count);
    pixmap_free_list(it->attn_pixmaps, it->attn_pixmap_count);
    free(it->tooltip.title);
    free(it->tooltip.description);
    free(it);
}

/* Unregister + drop: closes any menu the item owns, removes it from
 * the list, emits the signal, relayouts the bar. */
static void item_drop(trayhost *app, tray_item *it, int emit) {
    if (it == NULL) {
        return;
    }
    tracef("item drop: %s (sender %s)", it->reg_arg ? it->reg_arg : "?",
           it->sender_uniq ? it->sender_uniq : "?");
    if (g_level_count > 0) {
        /* The open cascade belongs to ONE item (single-menu host);
         * if it is this one, the menu goes with it. */
        menu_close_all(app);
    }
    if (it->prev != NULL) {
        it->prev->next = it->next;
    } else {
        app->head = it->next;
    }
    if (it->next != NULL) {
        it->next->prev = it->prev;
    }
    if (emit && app->bus != NULL && app->serving) {
        fdk_dbus_message *sig = fdk_dbus_message_new_signal(
            W_PATH, W_IFACE, "StatusNotifierItemUnregistered");
        if (sig != NULL &&
            fdk_ok(fdk_dbus_message_append_string(sig, it->reg_arg))) {
            fdk_dbus_connection_send(app->bus, sig);
        } else if (sig != NULL) {
            fdk_dbus_message_free(sig);
        }
    }
    if (!it->loading && !it->failed) {
        app->item_count--;
    }
    item_free(it);
    bar_relayout(app);
}

static tray_item *item_find_by_reg(trayhost *app, const char *reg) {
    for (tray_item *it = app->head; it != NULL; it = it->next) {
        if (it->reg_arg != NULL && strcmp(it->reg_arg, reg) == 0) {
            return it;
        }
    }
    return NULL;
}

static tray_item *item_find_by_sender(trayhost *app, const char *uniq) {
    for (tray_item *it = app->head; it != NULL; it = it->next) {
        if (it->sender_uniq != NULL &&
            strcmp(it->sender_uniq, uniq) == 0) {
            return it;
        }
    }
    return NULL;
}

/* ---- item property load (GetAll fallback chain) --------------------------- */

static void item_apply_props(tray_item *it, const fdk_dbus_value *dict);

static void item_getall_reply(fdk_dbus_connection *bus,
                              fdk_dbus_message *reply, void *user) {
    (void)bus;
    tray_item *it = user;
    trayhost *app = &g_app;
    it->getall_p = NULL; /* FIRST: the item may be gone next line    */
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        tracef("GetAll failed for %s", it->reg_arg ? it->reg_arg : "?");
        it->failed = 1;
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(reply, &args, &n) != FDK_OK ||
        n != 1 || args == NULL ||
        fdk_dbus_value_type(args[0]) != FDK_DBUS_ARRAY) {
        fdk_dbus_values_free_array(args, n);
        it->failed = 1;
        return;
    }
    item_apply_props(it, args[0]);
    fdk_dbus_values_free_array(args, n);
    it->loading = 0;
    if (!it->failed) {
        app->item_count++;
        bar_relayout(app);
        item_send_new_prefetch(it);
    }
}

/* Issues the GetAll with the interface fallback chain. */
static void item_fetch_props(tray_item *it) {
    static const char *const ifaces[] = {
        ITEM_IFACE_KDE, ITEM_IFACE_AYATANA, ITEM_IFACE_FREEDESKTOP,
    };
    if (g_app.bus == NULL || it->sender_uniq == NULL ||
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
            g_app.bus, call, 3000, item_getall_reply, it);
        if (p != NULL) {
            it->getall_p = p;
            tracef("GetAll(%s) on %s %s", ifaces[i], it->sender_uniq,
                   it->object_path);
            return; /* one in flight; the reply drives the chain     */
        }
    }
    it->failed = 1;
}

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
 * size-capped; a malformed value is skipped, never fatal. */
static void item_apply_props(tray_item *it, const fdk_dbus_value *dict) {
    static const char *const keys[] = {
        "Id",          "Title",          "Status",
        "IconName",    "AttentionIconName", "IconPixmap",
        "AttentionIconPixmap", "ToolTip", "Menu",
        "ItemIsMenu",  "WindowId",
    };
    (void)keys;
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
        } else if (strcmp(key, "Status") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            apply_status(it, fdk_dbus_value_string(vv));
        } else if (strcmp(key, "IconName") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->icon_name);
            it->icon_name = xstrdup_n(fdk_dbus_value_string(vv),
                                      MAX_ITEM_NAME);
        } else if (strcmp(key, "AttentionIconName") == 0 &&
                   fdk_dbus_value_type(vv) == FDK_DBUS_STRING) {
            free(it->attn_icon_name);
            it->attn_icon_name = xstrdup_n(fdk_dbus_value_string(vv),
                                           MAX_ITEM_NAME);
        } else if (strcmp(key, "IconPixmap") == 0) {
            pixmap_free_list(it->pixmaps, it->pixmap_count);
            it->pixmaps = NULL;
            it->pixmap_count =
                pixmap_list_from_array(vv, &it->pixmaps);
        } else if (strcmp(key, "AttentionIconPixmap") == 0) {
            pixmap_free_list(it->attn_pixmaps, it->attn_pixmap_count);
            it->attn_pixmaps = NULL;
            it->attn_pixmap_count =
                pixmap_list_from_array(vv, &it->attn_pixmaps);
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
                }
            }
        } else if (strcmp(key, "Menu") == 0) {
            if (fdk_dbus_value_type(vv) == FDK_DBUS_OBJECT_PATH) {
                free(it->menu_path);
                it->menu_path =
                    xstrdup_n(fdk_dbus_value_string(vv), 255);
                it->has_menu = it->menu_path != NULL;
            }
        } else if (strcmp(key, "ItemIsMenu") == 0) {
            if (fdk_dbus_value_type(vv) == FDK_DBUS_BOOLEAN) {
                it->item_is_menu = fdk_dbus_value_boolean(vv);
            }
        }
    }
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

static void watcher_getall(trayhost *app, const fdk_dbus_message *call) {
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    /* Build the a{sv} dict with every entry value VARIANT-wrapped. */
    size_t cap = 32;
    fdk_dbus_value **entries = calloc(cap, sizeof(*entries));
    if (entries == NULL) {
        fdk_dbus_message_free(rep);
        return;
    }
    size_t n = 0;

    const char *names[MAX_ITEMS + 1];
    size_t count = 0;
    for (tray_item *it = app->head; it != NULL && count < MAX_ITEMS;
         it = it->next) {
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
    if (dict != NULL && fdk_ok(fdk_dbus_message_append(rep, dict))) {
        fdk_dbus_value_free(dict);
        if (!(fdk_dbus_message_flags(call) &
              FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
            fdk_dbus_connection_send(app->bus, rep);
        } else {
            fdk_dbus_message_free(rep);
        }
    } else {
        fdk_dbus_value_free(dict);
        fdk_dbus_message_free(rep);
    }
    tracef("Properties.GetAll -> %zu items, host-registered", count);
}

static void watcher_get(trayhost *app, const fdk_dbus_message *call) {
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
            fdk_dbus_connection_send(app->bus, err);
        }
        return;
    }
    fdk_dbus_message *rep = fdk_dbus_message_new_method_return(call);
    if (rep == NULL) {
        return;
    }
    int ok = 0;
    if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
        /* VARIANT(boolean) — built by hand (the boolean variant has
         * no convenience appender). */
        fdk_dbus_value *inner =
            fdk_dbus_value_new_boolean(app->serving ? 1 : 0);
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
            calloc((size_t)MAX_ITEMS + 1, sizeof(*items));
        size_t count = 0;
        if (items != NULL) {
            for (tray_item *it = app->head;
                 it != NULL && count < (size_t)MAX_ITEMS;
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
            fdk_dbus_connection_send(app->bus, rep);
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
            fdk_dbus_connection_send(app->bus, err);
        }
    }
    free(prop);
}

static void watcher_set(trayhost *app, const fdk_dbus_message *call) {
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
        fdk_dbus_connection_send(app->bus, err);
    }
    tracef("Properties.Set refused: %s", prop ? prop : "?");
    free(prop);
    (void)app;
}

/* ---- properties-changed emission --------------------------------------- */

/* Emits PropertiesChanged for the Watcher's item-list property (the
 * bus-side contract: every a{sv} value is VARIANT-wrapped). */
static void emit_items_changed(trayhost *app) {
    if (app->bus == NULL || !app->serving) {
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
            calloc((size_t)MAX_ITEMS + 1, sizeof(*items));
        size_t count = 0;
        if (items != NULL) {
            for (tray_item *it = app->head;
                 it != NULL && count < (size_t)MAX_ITEMS;
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
        fdk_dbus_connection_send(app->bus, sig);
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
    it->loading = 1;
    item_fetch_props(it);
}

static void item_signal(fdk_dbus_connection *bus,
                        fdk_dbus_message *sig, void *user) {
    (void)bus;
    tray_item *it = user;
    trayhost *app = &g_app;
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
        item_refresh_icon(it);
        (void)app;
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
                bar_relayout(app);
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
            g_app.bus, rule, item_signal, it);
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
    trayhost *app = &g_app;
    tracef("watch %s owner -> %s (item %s)", name ? name : "?",
           owner ? owner : "(none)", it->reg_arg ? it->reg_arg : "?");
    if (owner == NULL) {
        /* The item's name lost its owner: drop it (this is ALSO the
         * path for a crashed client — hostile clients never
         * unregister politely). */
        item_drop(app, it, 1);
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
static void serve_register_item(trayhost *app,
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
            fdk_dbus_connection_send(app->bus, err);
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
            fdk_dbus_connection_send(app->bus, err);
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
            fdk_dbus_connection_send(app->bus, err);
        }
        return;
    }

    if (item_find_by_reg(app, svc) != NULL) {
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
                fdk_dbus_connection_send(app->bus, rep);
            }
        }
        return;
    }

    tray_item *it = calloc(1, sizeof(*it));
    if (it == NULL) {
        free(svc);
        return;
    }
    it->reg_arg = svc;
    it->status = ITEM_STATUS_ACTIVE;
    it->loading = 1;

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
                app->bus, svc, item_name_event, it);
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
            item_free(it);
            return;
        }
    }

    /* Append to the list. */
    it->next = NULL;
    it->prev = NULL;
    if (app->head == NULL) {
        app->head = it;
    } else {
        tray_item *tail = app->head;
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
            fdk_dbus_connection_send(app->bus, rep);
        }
    }

    /* The announcements. */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        W_PATH, W_IFACE, "StatusNotifierItemRegistered");
    if (sig != NULL &&
        fdk_ok(fdk_dbus_message_append_string(sig, svc))) {
        fdk_dbus_connection_send(app->bus, sig);
    } else if (sig != NULL) {
        fdk_dbus_message_free(sig);
    }
    emit_items_changed(app);
    tracef("registered %s (%s dialect, sender %s)", svc,
           kde_form ? "KDE bus-name" : "Ayatana object-path",
           sender ? sender : "?");
}

/* RegisterStatusNotifierHost(service): the host-side registration.
 * We serve it (this is how OTHER hosts would announce themselves to
 * us) AND call it for ourselves after taking the Watcher name — the
 * real bus path, not a shortcut. */
static void serve_register_host(trayhost *app,
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
        tracef("RegisterStatusNotifierHost refused: '%s'",
               svc ? svc : "(null)");
        free(svc);
        fdk_dbus_message *err = fdk_dbus_message_new_error(
            call, "org.freedesktop.DBus.Error.InvalidArgs",
            "RegisterStatusNotifierHost requires one bus name");
        if (err != NULL) {
            fdk_dbus_connection_send(app->bus, err);
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
            fdk_dbus_connection_send(app->bus, rep);
        }
    }
    /* The spec's announcement, once per registration. */
    fdk_dbus_message *sig = fdk_dbus_message_new_signal(
        W_PATH, W_IFACE, "StatusNotifierHostRegistered");
    if (sig != NULL) {
        fdk_dbus_connection_send(app->bus, sig);
    }
    (void)app;
}

/* ---- NameOwnerChanged: the vanished-item path --------------------------- */

static void owner_changed(fdk_dbus_connection *bus,
                          fdk_dbus_message *sig, void *user) {
    (void)bus;
    trayhost *app = user;
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
            tray_item *hit = item_find_by_sender(app, old);
            if (hit == NULL) {
                break;
            }
            item_drop(app, hit, 1);
        }
    }
    free(name);
    free(old);
    free(neu);
}

/* ---- Introspect ------------------------------------------------------------ */

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

/* ---- the method dispatcher ------------------------------------------------ */

static void reply_error(fdk_dbus_connection *bus,
                        const fdk_dbus_message *call, const char *name,
                        const char *text) {
    if (fdk_dbus_message_flags(call) & FDK_DBUS_FLAG_NO_REPLY_EXPECTED) {
        return;
    }
    fdk_dbus_message *err = fdk_dbus_message_new_error(call, name, text);
    if (err != NULL) {
        fdk_dbus_connection_send(bus, err);
    }
}

static void method_handler(fdk_dbus_connection *bus,
                           fdk_dbus_message *call, void *user) {
    trayhost *app = user;
    (void)bus;
    const char *iface = fdk_dbus_message_interface(call);
    const char *member = fdk_dbus_message_member(call);

    if (iface != NULL && strcmp(iface, W_IFACE) == 0) {
        if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
            serve_register_item(app, call);
            return;
        }
        if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
            serve_register_host(app, call);
            return;
        }
    } else if (iface != NULL && strcmp(iface, PROPS_IFACE) == 0) {
        if (strcmp(member, "Get") == 0) {
            watcher_get(app, call);
            return;
        }
        if (strcmp(member, "GetAll") == 0) {
            watcher_getall(app, call);
            return;
        }
        if (strcmp(member, "Set") == 0) {
            watcher_set(app, call);
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
                    fdk_dbus_connection_send(app->bus, rep);
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
            if (!(fdk_dbus_message_flags(call) &
                  FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                fdk_dbus_message *rep =
                    fdk_dbus_message_new_method_return(call);
                if (rep != NULL) {
                    fdk_dbus_connection_send(app->bus, rep);
                }
            }
            return;
        }
        if (strcmp(member, "GetMachineId") == 0) {
            fdk_dbus_message *rep =
                fdk_dbus_message_new_method_return(call);
            char uuid[64] = "";
            {
                FILE *f = fopen("/var/lib/dbus/machine-id", "r");
                if (f == NULL) {
                    f = fopen("/etc/machine-id", "r");
                }
                if (f != NULL) {
                    if (fgets(uuid, sizeof(uuid), f) != NULL) {
                        uuid[strcspn(uuid, "\n")] = '\0';
                    }
                    fclose(f);
                }
            }
            if (rep != NULL &&
                fdk_ok(fdk_dbus_message_append_string(
                    rep, uuid[0] != '\0' ? uuid : "unavailable"))) {
                if (!(fdk_dbus_message_flags(call) &
                      FDK_DBUS_FLAG_NO_REPLY_EXPECTED)) {
                    fdk_dbus_connection_send(app->bus, rep);
                } else {
                    fdk_dbus_message_free(rep);
                }
            } else if (rep != NULL) {
                fdk_dbus_message_free(rep);
            }
            return;
        }
    }
    char text[128];
    snprintf(text, sizeof(text), "Unknown method '%s' on interface '%s'",
             member ? member : "?", iface ? iface : "?");
    reply_error(app->bus, call,
                "org.freedesktop.DBus.Error.UnknownMethod", text);
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
/* (menu_fetch_ctx is declared beside tray_item — the item embeds it.) */

static void getlayout_reply(fdk_dbus_connection *bus,
                            fdk_dbus_message *reply, void *user) {
    (void)bus;
    menu_fetch_ctx *ctx = user;
    tray_item *it = ctx->item;
    trayhost *app = &g_app;
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
    if (g_menu.item == it) {
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
    }
    (void)app;
    (void)ctx->prefetch;
}

/* Issues GetLayout(0, -1, []). prefetch=1 at item load (the
 * first right-click must WORK — lazy-load's no-op first click was
 * the bug); prefetch=0 when the menu opens. */
static void item_fetch_menu_ctx(tray_item *it, int prefetch) {
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
        g_app.bus, call, 3000, getlayout_reply, &it->menu_fetch);
    if (p == NULL) {
        return;
    }
    it->layout_p = p;
    tracef("GetLayout(%s) prefetch=%d", it->menu_path, prefetch);
}

static void item_fetch_menu(tray_item *it) {
    item_fetch_menu_ctx(it, 1);
}

static void item_send_new_prefetch(tray_item *it) {
    /* Called when an item finishes loading: prefetch its menu so the
     * FIRST right-click opens instantly. */
    if (it->has_menu && it->menu_path != NULL &&
        it->menu_cache == NULL) {
        item_fetch_menu(it);
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
        g_app.bus, call, 2000, abouttoshow_reply, NULL);
    (void)p;
    tracef("AboutToShow(%d)", id);
}

/* Event(id, eventId, data, timestamp) — fire-and-forget. */
static void menu_event(tray_item *it, int32_t id, const char *event_id) {
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
    fdk_dbus_connection_send(g_app.bus, call);
    tracef("Event(id=%d, %s)", id, event_id);
}

/* Activate / SecondaryActivate / Scroll on the ITEM itself. */
static void item_call_simple(tray_item *it, const char *member, int x,
                             int y) {
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
        g_app.bus, call, 2000, NULL, NULL);
    (void)p;
    tracef("%s(%d, %d) -> %s", member, x, y, it->reg_arg);
}

/* ---- bar rendering --------------------------------------------------------- */

static fdk_color tk_color(fdk_theme_token token, fdk_color fallback) {
    fdk_color c = fdk_theme_get_color(NULL, token);
    if (c.a <= 0.0f) {
        return fallback;
    }
    return c;
}

/* The item list the bar draws (loaded, un-failed). */
static tray_item *bar_item_at(trayhost *app, int index) {
    int i = 0;
    for (tray_item *it = app->head; it != NULL; it = it->next) {
        if (it->loading || it->failed) {
            continue;
        }
        if (i == index) {
            return it;
        }
        i++;
    }
    return NULL;
}

static int bar_drawable_count(trayhost *app) {
    int n = 0;
    for (tray_item *it = app->head; it != NULL; it = it->next) {
        if (!it->loading && !it->failed) {
            n++;
        }
    }
    return n;
}

/* The screen size for position math (the anchoring rules are ours,
 * so the bar's origin is computable — X11 truth; Wayland has no
 * global space, documented in D-007). */
static void bar_screen_size(trayhost *app, int *w, int *h) {
    *w = 1024;
    *h = 768;
    uint32_t count = 0;
    if (fdk_ok(fdk_shell_output_count(app->shell, &count)) &&
        count > 0) {
        uint32_t idx = 0;
        if (app->opt.output_index >= 0 &&
            (uint32_t)app->opt.output_index < count) {
            idx = (uint32_t)app->opt.output_index;
        }
        fdk_shell_output_info info;
        memset(&info, 0, sizeof(info));
        info.struct_size = sizeof(info);
        if (fdk_ok(fdk_shell_output_get_info(app->shell, idx, &info)) &&
            info.width > 0 && info.height > 0) {
            *w = info.width;
            *h = info.height;
        }
    }
}

/* (Re)creates the bar layer when its WIDTH changes (the destroy +
 * recreate idiom — the only documented way to resize a layer). */
static void bar_relayout(trayhost *app) {
    int count = bar_drawable_count(app);
    int want_w = 2 * BAR_PAD + count * app->opt.cell;
    if (want_w < 2 * BAR_PAD) {
        want_w = 2 * BAR_PAD; /* the empty strip keeps a toe-hold      */
    }
    if (app->bar != NULL && app->bar_w == want_w) {
        bar_paint(app);
        return;
    }
    if (app->bar != NULL) {
        menu_close_all(app); /* the cascade parents to the bar       */
        fdk_shell_layer_destroy(app->bar);
        app->bar = NULL;
    }
    fdk_shell_layer_desc d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.layer = FDK_SHELL_LAYER_TOP;
    switch (app->opt.anchor) {
    case ANCHOR_TR:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_top = app->opt.edge_margin;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_TL:
        d.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT;
        d.margin_top = app->opt.edge_margin;
        d.margin_left = app->opt.edge_margin;
        break;
    case ANCHOR_BR:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_RIGHT;
        d.margin_bottom = app->opt.edge_margin;
        d.margin_right = app->opt.edge_margin;
        break;
    case ANCHOR_BL:
    default:
        d.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_LEFT;
        d.margin_bottom = app->opt.edge_margin;
        d.margin_left = app->opt.edge_margin;
        break;
    }
    d.exclusive_zone = 0; /* a floating tray reserves nothing        */
    d.keyboard = FDK_SHELL_KBD_NONE;
    d.width = want_w;
    d.height = app->opt.bar_height;
    d.output_index = app->opt.output_index;
    snprintf(d.namespace_, sizeof(d.namespace_), "trayhost");

    fdk_shell_layer *layer = NULL;
    fdk_result r = fdk_shell_layer_create(app->shell, &d, &layer);
    if (!fdk_ok(r)) {
        tracef("bar layer create failed (%d)", r);
        return;
    }
    app->bar = layer;
    app->bar_w = want_w;
    app->bar_h = app->opt.bar_height;
    fdk_shell_layer_set_event_callback(layer, NULL, NULL); /* set below */
    fdk_shell_layer_set_event_callback(layer, bar_layer_event, app);
    fdk_shell_layer_show(layer);
    bar_paint(app);

    /* The anchoring math mirrors the X11 backend's (top-right:
     * screen - margin - extent) — the Activate coordinates are sent
     * in SCREEN space where that is knowable. */
    int sw = 0, sh = 0;
    bar_screen_size(app, &sw, &sh);
    (void)sh;
    app->have_bar_pos = 0;
    if (app->opt.anchor == ANCHOR_TR) {
        app->bar_x = sw - app->opt.edge_margin - want_w;
        app->bar_y = app->opt.edge_margin;
        app->have_bar_pos = 1;
    } else if (app->opt.anchor == ANCHOR_TL) {
        app->bar_x = app->opt.edge_margin;
        app->bar_y = app->opt.edge_margin;
        app->have_bar_pos = 1;
    }
    tracef("bar relayout: %d items, %dx%d at (%d,%d)", count, want_w,
           app->bar_h, app->have_bar_pos ? app->bar_x : -1,
           app->have_bar_pos ? app->bar_y : -1);
}

/* Chooses the icon surface for one item (the best-resolution pixmap
 * for the cell, or the themed name), honoring status and the probe's
 * flip state. Borrowed; NULL when the item has nothing drawable. */
static fdk_surface *item_icon_surface(tray_item *it, int cell) {
    pixmap *best = NULL;
    int target = cell - 2 * ICON_INSET;
    pixmap *list = it->pixmaps;
    size_t count = it->pixmap_count;
    if (it->status == ITEM_STATUS_NEEDS_ATTENTION &&
        it->attn_pixmap_count > 0) {
        list = it->attn_pixmaps;
        count = it->attn_pixmap_count;
    }
    {
        for (size_t i = 0; i < count; i++) {
            if (best == NULL) {
                best = &list[i];
                continue;
            }
            int bi = best->width - target;
            int ci = (int)list[i].width - target;
            if (ci >= 0 && (bi < 0 || ci < bi)) {
                best = &list[i]; /* closest from above wins           */
            } else if (bi < 0 && ci > bi) {
                best = &list[i]; /* else the largest available        */
            }
        }
    }
    if (best != NULL) {
        return pixmap_to_surface(best);
    }
    const char *name =
        it->status == ITEM_STATUS_NEEDS_ATTENTION && it->attn_icon_name
            ? it->attn_icon_name
            : it->icon_name;
    if (name != NULL && name[0] != '\0') {
        return fdk_icon_theme_surface(name, (fdk_i32)target);
    }
    return NULL;
}

/* Paints one icon cell into the bar surface, faded for Passive. */
static void bar_paint_cell(trayhost *app, fdk_surface *s, tray_item *it,
                           int cell_x) {
    int cell = app->opt.cell;
    fdk_surface *icon = item_icon_surface(it, cell);
    if (icon == NULL) {
        return;
    }
    fdk_surface_info ii;
    if (!fdk_ok(fdk_surface_get_info(icon, &ii))) {
        fdk_surface_destroy(icon);
        return;
    }
    int box = cell - 2 * ICON_INSET;
    double scale = 1.0;
    if (ii.width > box || ii.height > box) {
        scale = (double)box / (double)(ii.width > ii.height ? ii.width
                                                            : ii.height);
    }
    int dw = (int)(ii.width * scale + 0.5);
    int dh = (int)(ii.height * scale + 0.5);
    int dx = cell_x + (cell - dw) / 2;
    int dy = (app->bar_h - dh) / 2;

    if (it->status != ITEM_STATUS_PASSIVE) {
        fdk_matrix m = { .m00 = (float)scale, .m01 = 0.0f, .m10 = 0.0f,
                         .m11 = (float)scale, .tx = (float)dx,
                         .ty = (float)dy };
        fdk_surface_blit_transformed(s, m, icon);
    } else {
        /* Passive: the icon drawn at alpha 0.45 — faded, NEVER
         * hidden (the honest middle; D-007). The fade is applied by
         * scaling the icon's own alpha channel on a scratch surface
         * (the transform blit has no alpha knob). */
        fdk_surface *scratch = NULL;
        if (fdk_ok(fdk_surface_create_format(
                ii.width, ii.height, FDK_SURFACE_FORMAT_ARGB8888,
                &scratch))) {
            fdk_matrix m = { .m00 = 1.0f, .m01 = 0.0f, .m10 = 0.0f,
                             .m11 = 1.0f, .tx = 0.0f, .ty = 0.0f };
            fdk_surface_blit_transformed(scratch, m, icon);
            fdk_surface_info si;
            if (fdk_ok(fdk_surface_get_info(scratch, &si))) {
                for (int y = 0; y < si.height; y++) {
                    for (int x = 0; x < si.width; x++) {
                        uint32_t p =
                            si.pixels[(size_t)y * (size_t)si.stride +
                                      (size_t)x];
                        uint32_t a = (p >> 24) & 0xFFu;
                        a = (a * 45u) / 100u;
                        si.pixels[(size_t)y * (size_t)si.stride +
                                  (size_t)x] =
                            (p & 0x00FFFFFFu) | (a << 24);
                    }
                }
            }
            fdk_matrix m2 = { .m00 = (float)scale, .m01 = 0.0f,
                              .m10 = 0.0f, .m11 = (float)scale,
                              .tx = (float)dx, .ty = (float)dy };
            fdk_surface_blit_transformed(s, m2, scratch);
            fdk_surface_destroy(scratch);
        }
    }
    fdk_surface_destroy(icon);

    if (it->status == ITEM_STATUS_NEEDS_ATTENTION) {
        /* The attention marker: an accent strip under the cell.     */
        fdk_color accent = tk_color(
            FDK_TK_ACCENT,
            (fdk_color){ .r = 0.56f, .g = 0.47f, .b = 0.85f, .a = 1.0f });
        fdk_surface_fill_rect(
            s, (fdk_rect){ .x = cell_x + 3,
                           .y = app->bar_h - 4,
                           .width = app->opt.cell - 6,
                           .height = 3 },
            accent);
    }
}

static void bar_paint(trayhost *app) {
    if (app->bar == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_layer_get_surface(app->bar, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    int w = info.width;
    int h = info.height;

    fdk_color bg = tk_color(
        FDK_TK_TOOLTIP_BACKGROUND,
        (fdk_color){ .r = 0.10f, .g = 0.11f, .b = 0.14f, .a = 1.0f });
    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = 0, .y = 0, .width = w, .height = h },
        BAR_RADIUS, bg);

    int i = 0;
    for (tray_item *it = app->head; it != NULL; it = it->next) {
        if (it->loading || it->failed) {
            continue;
        }
        bar_paint_cell(app, s, it, BAR_PAD + i * app->opt.cell);
        i++;
    }
    (void)fdk_surface_present(s);
}

/* ---- the menu cascade ------------------------------------------------------- */

/* Builds the rows for one level from a node list. */
static menu_level2 *menu_level_build(tray_item *item, menu_node *list,
                                     menu_level2 *parent) {
    (void)item;
    menu_level2 *lvl = calloc(1, sizeof(*lvl));
    if (lvl == NULL) {
        return NULL;
    }
    lvl->parent = parent;
    lvl->hover = -1;

    fdk_font_metrics fm;
    fdk_font_get_metrics(g_app.font, &fm);
    int row_h = fm.line_height + 10;
    if (row_h < MENU_ROW_H) {
        row_h = MENU_ROW_H;
    }

    /* Count first, then fill. */
    int n = 0;
    for (menu_node *nd = list; nd != NULL; nd = nd->next) {
        n++;
    }
    lvl->rows = calloc((size_t)(n > 0 ? n : 1), sizeof(*lvl->rows));
    if (lvl->rows == NULL) {
        free(lvl);
        return NULL;
    }
    int width = 140;
    int y = 4;
    int ri = 0;
    for (menu_node *nd = list; nd != NULL; nd = nd->next) {
        int rh = nd->is_separator ? MENU_SEP_H : row_h;
        lvl->rows[ri].node = nd->is_separator ? NULL : nd;
        lvl->rows[ri].y = y;
        lvl->rows[ri].h = rh;
        if (!nd->is_separator && nd->label != NULL) {
            fdk_text_metrics tm;
            fdk_font_measure_utf8(g_app.font, nd->label,
                                  strlen(nd->label), &tm);
            int need = MENU_PAD_X + 26 /* glyph column */ + MENU_PAD_X +
                       tm.advance_width + MENU_ARROW_W + MENU_PAD_X;
            if (need > width) {
                width = need;
            }
        }
        y += rh;
        ri++;
    }
    lvl->row_count = ri;
    lvl->width = width > MAX_MENU_WIDTH ? MAX_MENU_WIDTH : width;
    lvl->height = y + 4;
    return lvl;
}

/* Closes levels from `index` down: ONE popup destroy (the backend's
 * family sweep takes the native children — destroying them again
 * would double-free), then the wrappers free by hand. */
static void menu_close_from(int index) {
    if (g_level_count <= index) {
        return;
    }
    /* Destroy ONLY the popup at `index` — the backend sweeps its
     * children popups (destroying each again would double-free). */
    for (int i = index; i < g_level_count; i++) {
        if (g_levels[i] != NULL) {
            if (i == index && g_levels[i]->popup != NULL) {
                fdk_shell_popup_destroy(g_levels[i]->popup);
                g_levels[i]->popup = NULL;
            }
            /* Send "closed" for submenus this level had opened. */
            for (int r = 0; r < g_levels[i]->row_count; r++) {
                menu_node *nd = g_levels[i]->rows[r].node;
                if (nd != NULL && nd->opened) {
                    nd->opened = 0;
                    menu_event(g_menu.item, nd->id, "closed");
                }
            }
            free(g_levels[i]->rows);
            free(g_levels[i]);
            g_levels[i] = NULL;
        }
    }
    g_level_count = index;
    if (g_level_count == 0) {
        g_menu.item = NULL;
        g_menu.root = NULL;
    }
}

static void menu_close_all(trayhost *app) {
    (void)app;
    menu_close_from(0);
}

/* The root open: right-click on an item's cell. */
static void menu_open_root(trayhost *app, tray_item *it, int cell_index) {
    if (it == NULL || it->menu_cache == NULL || it->menu_cache->child == NULL) {
        return;
    }
    menu_close_all(app);
    menu_level2 *lvl = menu_level_build(it, it->menu_cache->child, NULL);
    if (lvl == NULL) {
        return;
    }
    /* Right-aligned under the item's cell. The popup's x is
     * LAYER-LOCAL, and the menu is WIDER than the bar: x goes
     * NEGATIVE (the menu hangs left of the bar's right edge — the
     * shape every right-edge tray menu has). Clamped only so the
     * global right edge stays on screen (when the bar origin is
     * knowable; Wayland's compositor clamps for us). */
    int cell_x = BAR_PAD + cell_index * app->opt.cell;
    int x = cell_x + app->opt.cell - lvl->width;
    if (app->have_bar_pos) {
        int max_x = 1024 - app->bar_x - lvl->width;
        int sw = 0, sh = 0;
        bar_screen_size(app, &sw, &sh);
        (void)sh;
        max_x = sw - app->opt.edge_margin - app->bar_x - lvl->width;
        if (x > max_x) {
            x = max_x;
        }
    }
    fdk_result r = fdk_shell_popup_create(
        app->bar, x, app->bar_h, lvl->width, lvl->height, &lvl->popup);
    if (!fdk_ok(r)) {
        tracef("menu popup create failed (%d)", r);
        free(lvl->rows);
        free(lvl);
        return;
    }
    g_menu.item = it;
    g_menu.root = lvl;
    g_levels[0] = lvl;
    g_level_count = 1;
    g_level_x[0] = x;
    fdk_shell_popup_set_event_callback(lvl->popup, menu_popup_event,
                                       NULL);
    fdk_shell_popup_show(lvl->popup);
    menu_level_paint(lvl);
    menu_about_to_show(it, 0);
    item_fetch_menu_ctx(it, 0); /* the open-time refresh              */
    tracef("menu open (root %dx%d at cell %d)", lvl->width, lvl->height,
           cell_index);
}

/* Opens a submenu level for one row. */
static void menu_open_child(trayhost *app, menu_level2 *parent, int row_index) {
    menu_node *nd = parent->rows[row_index].node;
    if (nd == NULL || nd->child == NULL) {
        return;
    }
    /* Close any deeper level first (a sibling submenu was open). */
    for (int i = 0; i < g_level_count; i++) {
        if (g_levels[i] == parent) {
            menu_close_from(i + 1);
            break;
        }
    }
    if (g_level_count >= MAX_MENU_DEPTH) {
        return;
    }
    menu_level2 *lvl = menu_level_build(g_menu.item, nd->child, parent);
    if (lvl == NULL) {
        return;
    }
    /* Position: at the parent's right edge, flipped when the child
     * would cross the screen's right margin. */
    int px = 0, py = parent->rows[row_index].y;
    int sw = 0, sh = 0;
    bar_screen_size(app, &sw, &sh);
    (void)sh;
    /* The parent's GLOBAL x: bar_x + (each level's local x chain). */
    int gx = app->have_bar_pos ? app->bar_x : 0;
    for (int i = 0; i < g_level_count; i++) {
        if (g_levels[i] == parent) {
            break;
        }
    }
    /* The chain's x offsets: level i+1's x is relative to level i.
     * Recompute from the popups' creation arguments, tracked below. */
    gx += menu_global_x(parent);
    if (gx + parent->width + lvl->width + 8 > sw) {
        px = -lvl->width; /* flip LEFT of the parent                  */
        tracef("submenu flip: parent at %d, child %d wide, screen %d",
               gx, lvl->width, sw);
    } else {
        px = parent->width - 2;
    }
    fdk_result r = fdk_shell_popup_create_child(
        parent->popup, px, py, lvl->width, lvl->height, &lvl->popup);
    if (!fdk_ok(r)) {
        tracef("child popup create failed (%d)", r);
        free(lvl->rows);
        free(lvl);
        return;
    }
    g_levels[g_level_count] = lvl;
    g_level_count++;
    fdk_shell_popup_set_event_callback(lvl->popup, menu_popup_event,
                                       NULL);
    fdk_shell_popup_show(lvl->popup);
    menu_level_paint(lvl);
    menu_about_to_show(g_menu.item, nd->id);
    menu_event(g_menu.item, nd->id, "opened");
    nd->opened = 1;
}

static int menu_global_x(menu_level2 *lvl) {
    int x = 0;
    for (int i = 0; i < g_level_count; i++) {
        x += g_level_x[i];
        if (g_levels[i] == lvl) {
            break;
        }
    }
    return x;
}

/* ---- menu painting --------------------------------------------------------- */

static void menu_level_paint(menu_level2 *lvl) {
    if (lvl == NULL || lvl->popup == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_popup_get_surface(lvl->popup, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    int w = info.width;
    int h = info.height;

    fdk_color bg = tk_color(
        FDK_TK_TOOLTIP_BACKGROUND,
        (fdk_color){ .r = 0.10f, .g = 0.11f, .b = 0.14f, .a = 1.0f });
    fdk_color fg = tk_color(
        FDK_TK_TOOLTIP_TEXT,
        (fdk_color){ .r = 0.95f, .g = 0.95f, .b = 0.95f, .a = 1.0f });
    fdk_color hover = tk_color(
        FDK_TK_CONTROL_BACKGROUND_HOVER,
        (fdk_color){ .r = 0.26f, .g = 0.29f, .b = 0.36f, .a = 1.0f });
    fdk_color accent = tk_color(
        FDK_TK_ACCENT,
        (fdk_color){ .r = 0.56f, .g = 0.47f, .b = 0.85f, .a = 1.0f });

    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = 0, .y = 0, .width = w, .height = h }, 6, bg);

    fdk_font_metrics fm;
    fdk_font_get_metrics(g_app.font, &fm);

    for (int i = 0; i < lvl->row_count; i++) {
        menu_row2 *row = &lvl->rows[i];
        if (row->node == NULL) {
            /* The separator: a centered hairline. */
            fdk_color line = fg;
            line.a = 0.22f;
            fdk_surface_fill_rect(
                s,
                (fdk_rect){ .x = MENU_PAD_X,
                            .y = row->y + row->h / 2,
                            .width = w - 2 * MENU_PAD_X, .height = 1 },
                line);
            continue;
        }
        menu_node *nd = row->node;
        if (i == lvl->hover) {
            fdk_surface_fill_rect(
                s, (fdk_rect){ .x = 2, .y = row->y, .width = w - 4,
                               .height = row->h },
                hover);
        }
        int ty = row->y + (row->h - fm.line_height) / 2 + fm.ascent;
        int tx = MENU_PAD_X + 22;
        fdk_color text = fg;
        if (!nd->enabled) {
            text.a = 0.45f;
        }
        /* The state glyph column (checkmark / radio). */
        if (nd->is_check && nd->toggle_state) {
            fdk_surface_draw_utf8(s, g_app.font, "\xE2\x9C\x93", 3,
                                  MENU_PAD_X, ty, accent);
        } else if (nd->is_radio) {
            fdk_surface_draw_utf8(
                s, g_app.font,
                nd->toggle_state ? "\xE2\x97\x8F" : "\xE2\x97\x8B", 3,
                MENU_PAD_X, ty, nd->toggle_state ? accent : text);
        }
        if (nd->label != NULL) {
            fdk_surface_draw_utf8(s, g_app.font, nd->label,
                                  strlen(nd->label), tx, ty, text);
        }
        /* The submenu arrow at the right edge. */
        if (nd->child != NULL) {
            fdk_color arrow = fg;
            arrow.a = nd->enabled ? 0.8f : 0.35f;
            fdk_surface_draw_utf8(s, g_app.font, "\xE2\x96\xB8", 3,
                                  w - MENU_PAD_X - 10, ty, arrow);
        }
    }
    (void)fdk_surface_present(s);
}

/* ---- pointer interaction ----------------------------------------------------- */

/* The row index at popup-local y (or -1). */
static int menu_row_at(menu_level2 *lvl, float y) {
    for (int i = 0; i < lvl->row_count; i++) {
        if (y >= (float)lvl->rows[i].y &&
            y < (float)(lvl->rows[i].y + lvl->rows[i].h)) {
            return i;
        }
    }
    return -1;
}

static void menu_popup_event(fdk_shell_popup *popup,
                             const fdk_event_data *ev, void *user) {
    (void)popup;
    (void)user;
    trayhost *app = &g_app;
    /* Which level does this popup belong to? */
    menu_level2 *lvl = NULL;
    int lvl_index = -1;
    for (int i = 0; i < g_level_count; i++) {
        if (g_levels[i] != NULL && g_levels[i]->popup == popup) {
            lvl = g_levels[i];
            lvl_index = i;
            break;
        }
    }
    if (lvl == NULL) {
        return; /* a stale popup's event after teardown            */
    }
    switch (ev->type) {
    case FDK_EVENT_WINDOW_EXPOSE:
        menu_level_paint(lvl);
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER: {
        int row = menu_row_at(lvl, ev->pointer.position.y);
        int clickable = row >= 0 && lvl->rows[row].node != NULL &&
                        lvl->rows[row].node->enabled;
        int new_hover = clickable ? row : -1;
        if (new_hover != lvl->hover) {
            lvl->hover = new_hover;
            fdk_shell_popup_set_cursor(
                popup, new_hover >= 0 ? FDK_SHELL_CURSOR_POINTER
                                      : FDK_SHELL_CURSOR_DEFAULT);
            menu_level_paint(lvl);
            if (new_hover >= 0 && lvl->rows[new_hover].node->child != NULL) {
                menu_open_child(app, lvl, new_hover);
            }
        }
        break;
    }
    case FDK_EVENT_POINTER_LEAVE:
        if (lvl->hover != -1) {
            lvl->hover = -1;
            menu_level_paint(lvl);
        }
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        break;
    case FDK_EVENT_POINTER_BUTTON_UP:
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT &&
            lvl->hover >= 0) {
            menu_node *nd = lvl->rows[lvl->hover].node;
            if (nd != NULL && nd->enabled && nd->child == NULL) {
                menu_event(g_menu.item, nd->id, "clicked");
                menu_close_all(app); /* a choice was made             */
            }
        }
        break;
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* Outside click or Escape. On a CHILD level: close it and
         * its descendants and re-arm the parent's grab (the child's
         * death released the server grab — the hide/show cycle
         * re-takes it, the documented popup idiom). On the ROOT:
         * the whole family goes. */
        if (lvl_index == 0) {
            menu_close_all(app);
        } else {
            menu_level2 *parent = lvl->parent;
            menu_close_from(lvl_index);
            if (parent != NULL && parent->popup != NULL) {
                fdk_shell_popup_hide(parent->popup);
                fdk_shell_popup_show(parent->popup);
                menu_level_paint(parent);
            }
        }
        break;
    default:
        break;
    }
}

static void bar_layer_event(fdk_shell_layer *layer,
                            const fdk_event_data *ev, void *user) {
    trayhost *app = user;
    (void)layer;
    switch (ev->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
        bar_paint(app);
        break;
    case FDK_EVENT_WINDOW_EXPOSE:
        bar_paint(app); /* the ghost-content contract                */
        break;
    case FDK_EVENT_POINTER_MOTION:
    case FDK_EVENT_POINTER_ENTER:
    case FDK_EVENT_POINTER_LEAVE:
        break;
    case FDK_EVENT_POINTER_BUTTON_UP: {
        int cell = app->opt.cell;
        int idx = ((int)ev->pointer_button.position.x - BAR_PAD) / cell;
        int in_cell =
            (int)ev->pointer_button.position.x >= BAR_PAD &&
            idx >= 0 && idx < bar_drawable_count(app);
        if (!in_cell) {
            break;
        }
        tray_item *it = bar_item_at(app, idx);
        if (it == NULL) {
            break;
        }
        /* The click position in SCREEN coordinates, where the
         * anchoring math makes those knowable (X11; D-007 records
         * the Wayland truth: no global space, the layer-local
         * coordinates carry). */
        int gx = (int)ev->pointer_button.position.x;
        int gy = (int)ev->pointer_button.position.y;
        if (app->have_bar_pos) {
            gx += app->bar_x;
            gy += app->bar_y;
        }
        if (ev->pointer_button.button == FDK_POINTER_BUTTON_LEFT) {
            if (g_level_count > 0 && g_menu.item == it) {
                menu_close_all(app); /* a left-click toggles          */
                break;
            }
            if (it->item_is_menu) {
                menu_open_root(app, it, idx);
            } else {
                item_call_simple(it, "Activate", gx, gy);
            }
        } else if (ev->pointer_button.button ==
                   FDK_POINTER_BUTTON_RIGHT) {
            if (g_level_count > 0 && g_menu.item == it) {
                menu_close_all(app); /* right-click toggles too       */
            } else {
                menu_open_root(app, it, idx);
            }
        } else if (ev->pointer_button.button ==
                   FDK_POINTER_BUTTON_MIDDLE) {
            if (it->item_is_menu) {
                menu_open_root(app, it, idx);
            } else {
                item_call_simple(it, "SecondaryActivate", gx, gy);
            }
        }
        break;
    }
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        /* The bar's layer was closed by the environment (an output
         * going away): the layer handle is stale; the next relayout
         * recreates it. */
        app->bar = NULL;
        app->bar_w = 0;
        bar_relayout(app);
        break;
    default:
        break;
    }
}

/* ---- bus-name etiquette (the notifyd matrix, verbatim) ------------------ */

static void stop_serving(trayhost *app, int code, const char *why) {
    app->stopping = 1;
    app->exit_code = code;
    app->stop_reason = why;
}

static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user);

static void request_the_name(trayhost *app) {
    uint32_t flags = FDK_DBUS_NAME_ALLOW_REPLACEMENT;
    if (app->opt.replace) {
        flags |= FDK_DBUS_NAME_REPLACE_EXISTING;
    }
    if (!app->opt.wait) {
        flags |= FDK_DBUS_NAME_DO_NOT_QUEUE;
    }
    tracef("RequestName(%s, flags=0x%x)", W_NAME, flags);
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, W_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, flags))) {
        fdk_dbus_message_free(call);
        stop_serving(app, 1, "allocation failure");
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        app->bus, call, 5000, request_name_reply, app);
    if (p == NULL) {
        stop_serving(app, 1, "RequestName could not be sent");
    }
}

static void report_incumbent(trayhost *app, const char *owner) {
    sayf("%s is already owned by %s", W_NAME, owner ? owner : "?");
    sayf("...; exiting (--replace to take over)");
    stop_serving(app, 0, "name already owned");
}

static void request_name_reply(fdk_dbus_connection *bus,
                               fdk_dbus_message *reply, void *user) {
    (void)bus;
    (void)user;
    trayhost *app = &g_app;
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
    switch (code) {
    case FDK_DBUS_REQUEST_NAME_PRIMARY_OWNER:
    case FDK_DBUS_REQUEST_NAME_ALREADY_OWNER:
        tracef("RequestName -> %u (primary owner)", code);
        break;
    case FDK_DBUS_REQUEST_NAME_IN_QUEUE:
        tracef("RequestName -> queued (--wait)");
        sayf("%s is owned by another host; queued (--wait)", W_NAME);
        break;
    case FDK_DBUS_REQUEST_NAME_EXISTS:
    default:
        tracef("RequestName -> %u (exists)", code);
        sayf("%s is owned by a host that refuses replacement; exiting",
             W_NAME);
        stop_serving(app, 0, "name exists, replacement refused");
        break;
    }
}

/* The self-registration: after the name is ours, the HOST registers
 * with the WATCHER over the real bus path (our own handler serves
 * the call — exactly as a third party's would be). */
static void self_register_host(trayhost *app) {
    const char *me = fdk_dbus_connection_unique_name(app->bus);
    char service[300];
    /* The spec's argument is the HOST'S BUS NAME (the unique name is
     * the honest one — we render, we hold the Watcher, and the call
     * goes over the real bus path either way). */
    snprintf(service, sizeof(service), "%s", me != NULL ? me : "");
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        W_NAME, W_PATH, W_IFACE, "RegisterStatusNotifierHost");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, service))) {
        fdk_dbus_message_free(call);
        return;
    }
    fdk_dbus_pending *p = fdk_dbus_connection_call(
        app->bus, call, 2000, NULL, NULL);
    (void)p;
    tracef("self-registered as host: %s", service);
}

static void name_owner_event(fdk_dbus_connection *bus, const char *name,
                             const char *owner, void *user) {
    (void)name;
    trayhost *app = user;
    tracef("watch: %s owner -> %s", W_NAME, owner ? owner : "(none)");
    const char *me = fdk_dbus_connection_unique_name(bus);
    if (owner != NULL && me != NULL && strcmp(owner, me) == 0) {
        return; /* our own acquisition echoing through the watcher   */
    }
    if (owner != NULL && !app->opt.replace && !app->opt.wait &&
        !app->serving) {
        report_incumbent(app, owner);
        return;
    }
    if (!app->serving &&
        (owner == NULL || app->opt.replace || app->opt.wait)) {
        request_the_name(app);
    }
}

static void name_event(fdk_dbus_connection *bus, const char *name,
                       int owned, void *user) {
    (void)bus;
    trayhost *app = user;
    if (name == NULL || strcmp(name, W_NAME) != 0) {
        return; /* the unique-name NameAcquired is not ours to serve */
    }
    if (owned) {
        if (!app->serving) {
            app->serving = 1;
            sayf("serving %s as %s", W_NAME,
                 fdk_dbus_connection_unique_name(bus));
            self_register_host(app);
        }
    } else if (app->serving) {
        sayf("another host took %s — handing over", W_NAME);
        stop_serving(app, 0, "replaced by another host");
    }
}

static void bus_ready(fdk_dbus_connection *bus, int ok, const char *error,
                      void *user) {
    trayhost *app = user;
    if (!ok) {
        sayf("session bus connection failed (%s)", error ? error : "?");
        stop_serving(app, 1, "bus connect failed");
        return;
    }
    tracef("bus ready as %s", fdk_dbus_connection_unique_name(bus));
    fdk_dbus_connection_set_name_events(bus, name_event, app);
    /* The NameOwnerChanged subscription: vanished items drop here.
     * The subscription is HELD until teardown (freeing it issues
     * RemoveMatch — the vanished-item path would never fire). */
    char *rule = fdk_dbus_match_rule(
        "signal", "org.freedesktop.DBus", NULL, "NameOwnerChanged", NULL,
        NULL);
    if (rule != NULL) {
        app->owner_sub = fdk_dbus_connection_subscribe(bus, rule,
                                                       owner_changed, app);
        if (app->owner_sub == NULL) {
            sayf("NameOwnerChanged subscription failed — vanished "
                 "items will linger");
        }
        free(rule);
    }
    if (fdk_dbus_connection_watch_name(bus, W_NAME, name_owner_event,
                                       app) == NULL) {
        stop_serving(app, 1, "name watcher failed");
    }
}

static void bus_died(fdk_dbus_connection *bus, void *user) {
    (void)bus;
    trayhost *app = user;
    sayf("the session bus died — exiting (a supervisor should "
         "restart trayhost)");
    app->bus = NULL;
    stop_serving(app, 1, "bus died");
}

/* ---- output events -------------------------------------------------------- */

static void output_event(fdk_shell *shell,
                         const fdk_shell_output_event *event, void *user) {
    (void)shell;
    trayhost *app = user;
    tracef("output event type=%d index=%u", (int)event->type,
           event->index);
    if (event->type == FDK_SHELL_OUTPUT_ADDED ||
        event->type == FDK_SHELL_OUTPUT_CHANGED) {
        bar_relayout(app); /* re-derive geometry (and the bar origin) */
    }
}

/* ---- CLI ------------------------------------------------------------------- */

static void usage(FILE *out) {
    fputs(
        "trayhost — the fdk-shell StatusNotifier tray host "
        "(org.kde.StatusNotifierWatcher)\n\n"
        "usage: trayhost [options]\n\n"
        "  --bar-height N  the strip's height in logical px (default 36)\n"
        "  --cell N        one item's cell in logical px (default 30)\n"
        "  --margin N      gap from the screen edges (default 4)\n"
        "  --anchor W      tr (default) | tl | br | bl\n"
        "  --output N      show on output N only (default: all)\n"
        "  --replace       take the bus name from a host that allows\n"
        "                  replacement\n"
        "  --wait          queue for the name instead of exiting\n"
        "  --quit-after S  exit after S seconds (rigs/testing)\n"
        "  --help          this text\n"
        "  --version       the host and protocol versions\n\n"
        "Diagnostics: FDK_TRAYHOST_TRACE=1 logs every registration,\n"
        "property fetch, menu transaction, and placement decision.\n",
        out);
}

static void parse_args(trayhost *app, int argc, char **argv) {
    app->opt.bar_height = 36;
    app->opt.cell = 30;
    app->opt.edge_margin = 4;
    app->opt.anchor = ANCHOR_TR;
    app->opt.output_index = -1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "--version") == 0) {
            printf("trayhost %s (fdk-shell %s, SNI protocol 0)\n",
                   fdk_shell_version(), fdk_shell_version());
            exit(0);
        } else if (strcmp(a, "--replace") == 0) {
            app->opt.replace = 1;
        } else if (strcmp(a, "--wait") == 0) {
            app->opt.wait = 1;
        } else if (strcmp(a, "--bar-height") == 0 && i + 1 < argc) {
            app->opt.bar_height = atoi(argv[++i]);
            if (app->opt.bar_height < 12) {
                app->opt.bar_height = 12;
            }
            if (app->opt.bar_height > 96) {
                app->opt.bar_height = 96;
            }
        } else if (strcmp(a, "--cell") == 0 && i + 1 < argc) {
            app->opt.cell = atoi(argv[++i]);
            if (app->opt.cell < 16) {
                app->opt.cell = 16;
            }
            if (app->opt.cell > 64) {
                app->opt.cell = 64;
            }
        } else if (strcmp(a, "--margin") == 0 && i + 1 < argc) {
            app->opt.edge_margin = atoi(argv[++i]);
            if (app->opt.edge_margin < 0) {
                app->opt.edge_margin = 0;
            }
            if (app->opt.edge_margin > 200) {
                app->opt.edge_margin = 200;
            }
        } else if (strcmp(a, "--anchor") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "tr") == 0) {
                app->opt.anchor = ANCHOR_TR;
            } else if (strcmp(v, "tl") == 0) {
                app->opt.anchor = ANCHOR_TL;
            } else if (strcmp(v, "br") == 0) {
                app->opt.anchor = ANCHOR_BR;
            } else if (strcmp(v, "bl") == 0) {
                app->opt.anchor = ANCHOR_BL;
            } else {
                fprintf(stderr,
                        "trayhost: unknown --anchor '%s' (tr|tl|br|"
                        "bl)\n",
                        v);
                exit(2);
            }
        } else if (strcmp(a, "--output") == 0 && i + 1 < argc) {
            app->opt.output_index = atoi(argv[++i]);
        } else if (strcmp(a, "--quit-after") == 0 && i + 1 < argc) {
            app->opt.quit_after_s = strtod(argv[++i], NULL);
        } else {
            fprintf(stderr, "trayhost: unknown option '%s' (--help)\n",
                    a);
            exit(2);
        }
    }
}

static int g_signal_stop;

static void on_signal(int sig) {
    (void)sig;
    g_signal_stop = 1;
}

/* ---- main ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));
    g_trace = getenv("FDK_TRAYHOST_TRACE") != NULL;
    parse_args(&g_app, argc, argv);

    /* The add-on's version gate (the documented pattern). */
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0) ||
        fdk_ext_api_version() < 1) {
        fprintf(stderr, "trayhost: FDK 1.1.0 (extension API v1) "
                        "required\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr,
                "trayhost: no display — a tray host renders "
                "(WAYLAND_DISPLAY or DISPLAY required)\n");
        return 1;
    }
    g_app.ctx = ctx;

    fdk_result r = fdk_shell_init(ctx, &g_app.shell);
    if (r == FDK_ERR_UNSUPPORTED) {
        fprintf(stderr,
                "trayhost: this backend offers no layer surfaces "
                "(Wayland compositor without wlr-layer-shell?)\n");
        fdk_shutdown(ctx);
        return 1;
    }
    if (!fdk_ok(r)) {
        fprintf(stderr, "trayhost: fdk_shell_init failed (%d)\n", r);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell_set_output_event_callback(g_app.shell, output_event, &g_app);

    g_app.font = fdk_font_load_system_default(14);
    if (g_app.font == NULL) {
        sayf("no system font found — menus render without text "
             "(install a font, e.g. fonts-dejavu-core)");
    }

    /* The empty bar exists from the start (a toe-hold the rig can
     * see even with zero items). */
    bar_relayout(&g_app);

    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, bus_ready, &g_app);
    if (bus == NULL) {
        sayf("no session bus reachable (DBUS_SESSION_BUS_ADDRESS?) — "
             "exiting");
        if (g_app.font != NULL) {
            fdk_font_destroy(g_app.font);
        }
        fdk_shell_shutdown(g_app.shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_app.bus = bus;
    fdk_dbus_connection_set_death_handler(bus, bus_died, &g_app);
    fdk_dbus_connection_set_method_handler(bus, method_handler, &g_app);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct timespec start;
    int serving_clock = 0;

    while (!g_app.stopping && !g_signal_stop) {
        fdk_result pr = fdk_pump_events(ctx, 100);
        if (pr < 0) {
            sayf("the display connection died (%d) — exiting", (int)pr);
            g_app.exit_code = 1;
            break;
        }
        if (g_app.opt.quit_after_s > 0.0) {
            if (g_app.serving && !serving_clock) {
                serving_clock = 1;
                clock_gettime(CLOCK_MONOTONIC, &start);
                tracef("quit-after clock starts (serving)");
            } else if (!serving_clock) {
                clock_gettime(CLOCK_MONOTONIC, &start);
            }
            if (serving_clock) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double elapsed =
                    (double)(now.tv_sec - start.tv_sec) +
                    (double)(now.tv_nsec - start.tv_nsec) / 1e9;
                if (elapsed >= g_app.opt.quit_after_s) {
                    break;
                }
            }
        }
    }
    if (g_signal_stop) {
        g_app.exit_code = 0;
    }

    /* Teardown: the cascade, the items, the bar, the bus, the shell,
     * FDK — ASan-clean by construction (the rig asserts it). */
    menu_close_all(&g_app);
    while (g_app.head != NULL) {
        item_drop(&g_app, g_app.head, 0);
    }
    if (g_app.bar != NULL) {
        fdk_shell_layer_destroy(g_app.bar);
        g_app.bar = NULL;
    }
    if (g_app.bus != NULL) {
        fdk_dbus_subscription_free(g_app.owner_sub);
        g_app.owner_sub = NULL;
        fdk_dbus_connection_close(g_app.bus);
        g_app.bus = NULL;
    }
    if (g_app.font != NULL) {
        fdk_font_destroy(g_app.font);
    }
    fdk_shell_shutdown(g_app.shell);
    fdk_shutdown(ctx);
    if (g_app.stop_reason != NULL && g_app.exit_code != 0) {
        sayf("%s", g_app.stop_reason);
    }
    return g_app.exit_code;
}
