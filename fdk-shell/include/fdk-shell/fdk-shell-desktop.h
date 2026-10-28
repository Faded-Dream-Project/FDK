/* fdk-shell-desktop.h — the desktop host-side models (T-B side 2).
 *
 * Two no-drawing models extracted from the reference UIs
 * (examples/trayhost.c, examples/notifyd.c):
 *
 *   fdk_shell_tray_host      the org.kde.StatusNotifierWatcher host:
 *                            the watcher service, the item registry
 *                            (both registration dialects), per-item
 *                            state (properties, pixmaps, tooltip), and
 *                            the com.canonical.dbusmenu client.
 *
 *   fdk_shell_notify_server  the org.freedesktop.Notifications server:
 *                            the Notify/CloseNotification protocol,
 *                            the etiquette matrix, the flood gate, the
 *                            live-set cap, expiration, and the record
 *                            store.
 *
 * The contract for both: DATA AND EVENTS ONLY. A UI (bar, panel, OSD)
 * creates the model on an fdk_dbus_connection, receives coarse events,
 * snapshots the state through the read APIs, and forwards user
 * interactions through the action APIs. Nothing here touches surfaces,
 * fonts, or pixels — that is the renderer's half, and the reference
 * renderers remain the two examples above (their rigs prove the models
 * end to end).
 *
 * Includes: fdk/fdk.h (for fdk_dbus_connection, fdk_context).
 */
#ifndef FDK_SHELL_DESKTOP_H
#define FDK_SHELL_DESKTOP_H

#include <fdk/fdk.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * Part 1 — the tray host model
 * ========================================================================== */

typedef struct fdk_shell_tray_host fdk_shell_tray_host;

/* Item status (the SNI Status property's three values). */
typedef enum {
    FDK_SHELL_TRAY_ACTIVE = 0,
    FDK_SHELL_TRAY_PASSIVE = 1,
    FDK_SHELL_TRAY_NEEDS_ATTENTION = 2,
} fdk_shell_tray_item_status;

/* Why an event fired. ITEMS_CHANGED: the enumerable set changed (an
 * item registered, finished loading, failed, or was dropped). The UI
 * re-walks the item list. ITEM_STATE: one item's visible state
 * changed (title/status/icons/tooltip) — item_serial identifies it,
 * the UI re-snapshots. MENU_CHANGED: the item's cached dbusmenu tree
 * was replaced — the UI re-walks the menu (and, if it has the menu
 * open on that item, reopens it; the model itself keeps open-menu
 * refreshes stale-safe exactly like the reference UI did).
 * HOST_STATE: the well-known name's ownership changed — the event's
 * item_serial field carries the new state (1 = acquired/serving,
 * 0 = lost or refused; fdk_shell_tray_host_serving() is the query).
 */
typedef enum {
    FDK_SHELL_TRAY_EV_ITEMS_CHANGED = 1,
    FDK_SHELL_TRAY_EV_ITEM_STATE = 2,
    FDK_SHELL_TRAY_EV_MENU_CHANGED = 3,
    FDK_SHELL_TRAY_EV_HOST_STATE = 4,
} fdk_shell_tray_event_type;

typedef void (*fdk_shell_tray_event_fn)(fdk_shell_tray_host *host,
                                        fdk_shell_tray_event_type ev,
                                        uint32_t item_serial,
                                        void *user);

/* Creation description (struct_size discipline: zero-init, set
 * struct_size = sizeof(...), fill the fields you care about).
 *
 *   event_cb / user: the coarse change notification (required for a
 *                    useful host; NULL is legal — a pure probe).
 *   max_items:       0 = the default cap (32). Registrations beyond
 *                    the cap are refused with InvalidArgs the same
 *                    way malformed ones are.
 *   replace / wait:  the name-etiquette posture against an incumbent
 *                    watcher. Default (both 0): REFUSE — when
 *                    org.kde.StatusNotifierWatcher is already owned,
 *                    the host reports HOST_STATE(0) and stays
 *                    dormant. replace=1: take the name (flags allow
 *                    replacement). wait=1: queue behind the incumbent
 *                    and serve when it vacates.
 */
typedef struct {
    uint32_t struct_size;
    fdk_shell_tray_event_fn event_cb;
    void *user;
    int max_items;
    int replace;
    int wait;
} fdk_shell_tray_host_desc;

/* Creates the host on `bus` (which must be READY —
 * fdk_dbus_connection_ready() — create in the ready callback; a
 * not-yet-ready connection fails FDK_ERR_NOT_INITIALIZED) and starts
 * the well-known-name etiquette (watcher on the name, RequestName
 * with the posture above). The watcher's method surface
 * (RegisterStatusNotifierItem in both dialects,
 * RegisterStatusNotifierHost, Properties Get/GetAll/Set, Introspect,
 * Peer) is served from here — the model owns the connection's ONE
 * method handler and its NameAcquired/NameLost events: one tray host
 * per bus connection. The NameOwnerChanged monitoring that drops
 * vanished items is installed for the connection's lifetime.
 *
 * Name acquisition is ASYNCHRONOUS: create returns FDK_OK and the
 * HOST_STATE event (+ serving()) reports the outcome.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - bus or out is NULL, or replace and
 *                              wait are both set
 *   FDK_ERR_NOT_INITIALIZED  - the connection is not ready
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_shell_tray_host_create(
    fdk_dbus_connection *bus, const fdk_shell_tray_host_desc *desc,
    fdk_shell_tray_host **out);

/* Tears the host down: unregisters every item (the
 * StatusNotifierItemUnregistered announcements are sent), releases
 * the well-known name, drops the NameOwnerChanged subscription and
 * all per-item signal subscriptions and pending calls. The bus
 * connection itself is untouched. NULL is a safe no-op. */
void fdk_shell_tray_host_destroy(fdk_shell_tray_host *host);

/* Nonzero while this host owns the well-known name. */
int fdk_shell_tray_host_serving(const fdk_shell_tray_host *host);

/* ---- item snapshots --------------------------------------------------- */

/* The enumerable item count (loading and failed items included — a
 * loading item is real state a UI may want to reserve a cell for,
 * exactly like the reference bar does). */
uint32_t fdk_shell_tray_item_count(const fdk_shell_tray_host *host);

/* Item snapshot (struct_size discipline). `serial` is the stable
 * identity events carry (indices are positional and shift on drops;
 * the serial never shifts). Strings are copied into the caller's
 * buffers; NULL-able pointers mean "absent".
 */
typedef struct {
    uint32_t struct_size;
    uint32_t serial;      /* stable id, event correlation            */
    int loading;          /* properties still fetching               */
    int failed;           /* unusable (kept until unregister)        */
    fdk_shell_tray_item_status status;
    char *reg_arg;        /* exactly what RegisterStatusNotifierItem
                           * carried (bus name or object path); buffer
                           * of reg_arg_len bytes                    */
    size_t reg_arg_len;
    char *title;          /* the Title property, or NULL             */
    size_t title_len;
    char *icon_name;      /* IconName, or NULL                        */
    size_t icon_name_len;
    char *attention_icon_name; /* AttentionIconName, or NULL          */
    size_t attention_icon_name_len;
    char *tooltip_title;  /* ToolTip's title field, or NULL           */
    size_t tooltip_title_len;
    char *tooltip_description; /* ToolTip's description, or NULL      */
    size_t tooltip_description_len;
    uint32_t pixmap_count;        /* IconPixmap entries                */
    uint32_t attention_pixmap_count; /* AttentionIconPixmap entries    */
    int has_menu;         /* a Menu object path arrived              */
    int item_is_menu;     /* clicks should open the menu             */
} fdk_shell_tray_item_info;

/* Snapshots item `index` into *out. Pointers inside *out that are
 * non-NULL on entry are treated as caller buffers of the
 * corresponding *_len capacity (the model NUL-terminates and clips);
 * NULL pointers mean "don't copy this field". Returns
 * FDK_ERR_NOT_FOUND for an out-of-range index. */
fdk_result fdk_shell_tray_item_snapshot(const fdk_shell_tray_host *host,
                                        uint32_t index,
                                        fdk_shell_tray_item_info *out);

/* One pixmap's dimensions. attention != 0 selects the
 * AttentionIconPixmap list. FDK_ERR_NOT_FOUND for a bad pair. */
fdk_result fdk_shell_tray_item_pixmap_size(
    const fdk_shell_tray_host *host, uint32_t index, int attention,
    uint32_t pixmap_index, int *out_w, int *out_h);

/* Copies pixmap pixels (ARGB32, 4*w*h bytes, the model's host-order
 * a(iiay) decode) into buf. FDK_ERR_INVALID_ARGUMENT when cap is too
 * small (the required size is still written to *out_needed). */
fdk_result fdk_shell_tray_item_pixmap_pixels(
    const fdk_shell_tray_host *host, uint32_t index, int attention,
    uint32_t pixmap_index, uint8_t *buf, size_t cap, size_t *out_needed);

/* ---- interactions (the UI forwards user actions) ----------------------- */

/* Activate / SecondaryActivate / Scroll(int dx, int dy) on the item —
 * the SNI method calls, with the reference UI's posture (screen
 * coordinates passed through; Scroll's delta pair). */
fdk_result fdk_shell_tray_item_activate(fdk_shell_tray_host *host,
                                        uint32_t index, int x, int y);
fdk_result fdk_shell_tray_item_secondary_activate(
    fdk_shell_tray_host *host, uint32_t index, int x, int y);
fdk_result fdk_shell_tray_item_scroll(fdk_shell_tray_host *host,
                                      uint32_t index, int dx, int dy);

/* ---- the dbusmenu model ------------------------------------------------ */

/* One node's snapshot, taken during a walk. The strings point into
 * the model's owned tree and are valid ONLY inside the callback that
 * received them (the tree is replaced atomically between events). */
typedef struct fdk_shell_tray_menu_node_info {
    uint32_t struct_size;
    int32_t id;
    const char *label;    /* mnemonic-stripped; NULL for separators   */
    int is_separator;
    int is_check;
    int is_radio;
    int toggle_state;     /* 0/1 (check + radio)                      */
    int enabled;          /* 0 = greyed (invisible rows never walk)   */
    const char *icon_name; /* the "icon-name" property, or NULL       */
    int has_children;     /* a submenu hangs below this node          */
} fdk_shell_tray_menu_node_info;

typedef void (*fdk_shell_tray_menu_visit_fn)(
    const fdk_shell_tray_menu_node_info *node, int depth, void *user);

/* Walks the item's CACHED menu tree in preorder (root first, then
 * children depth-first). FDK_ERR_NOT_FOUND when the item has no menu
 * cached yet (first open after registration races the prefetch — the
 * honest answer is "empty", the UI draws nothing and the next event
 * brings the tree). */
fdk_result fdk_shell_tray_menu_walk(const fdk_shell_tray_host *host,
                                    uint32_t index,
                                    fdk_shell_tray_menu_visit_fn visit,
                                    void *user);

/* The menu open/close handshakes, exactly as the reference UI drove
 * them: open sends AboutToShow(root) and (when a cached tree exists)
 * a re-fetch of GetLayout; close sends Event(root, "closed") — the
 * UI is expected to also Event(opened)/Event(clicked) the nodes it
 * interacts with through fdk_shell_tray_menu_event. */
fdk_result fdk_shell_tray_menu_open(fdk_shell_tray_host *host,
                                    uint32_t index);
fdk_result fdk_shell_tray_menu_close(fdk_shell_tray_host *host,
                                     uint32_t index);

/* Event(id, eventId, data v, timestamp u) on the item's dbusmenu —
 * NO_REPLY_EXPECTED, the every-real-implementation posture. eventId
 * is the UI's choice ("clicked", "opened", "closed", ...). */
fdk_result fdk_shell_tray_menu_event(fdk_shell_tray_host *host,
                                     uint32_t index, int32_t node_id,
                                     const char *event_id);

/* Forces a GetLayout re-fetch (a NewMenu signal's implied refresh;
 * also the mid-open "the tree went stale" answer). */
fdk_result fdk_shell_tray_menu_refresh(fdk_shell_tray_host *host,
                                       uint32_t index);

/* ==========================================================================
 * Part 2 — the notification server model
 * ========================================================================== */

typedef struct fdk_shell_notify_server fdk_shell_notify_server;

typedef enum {
    FDK_SHELL_NOTIFY_EV_ADDED = 1,    /* a notification joined the set */
    FDK_SHELL_NOTIFY_EV_REPLACED = 2, /* same id, new content         */
    FDK_SHELL_NOTIFY_EV_CLOSED = 3,   /* gone; snapshot for the reason */
    FDK_SHELL_NOTIFY_EV_SERVER_STATE = 4, /* name acquired(1)/lost(0)  */
} fdk_shell_notify_event_type;

typedef void (*fdk_shell_notify_event_fn)(
    fdk_shell_notify_server *server, fdk_shell_notify_event_type ev,
    uint32_t note_id, void *user);

/* Creation description (struct_size discipline).
 *
 *   bus / ctx:      the connection to serve on; the context owns the
 *                   expiration timers (both required).
 *   event_cb/user:  the coarse change notification.
 *   server_name:    the GetServerInformation identity (copied at
 *                   create; NULL = "fdk-shell").
 *   default_timeout_ms: expire_timeout == -1 resolves here
 *                       (0 = the 5000 default).
 *   max_live:       the live-set cap; 0 = 32 (the reference posture).
 *   replace / wait: the name-etiquette posture against an incumbent
 *                   daemon. Default (both 0): REFUSE — creating on a
 *                   bus where org.freedesktop.Notifications is owned
 *                   fails FDK_ERR_EXISTS. replace=1: take the name
 *                   anyway. wait=1: queue behind the incumbent and
 *                   serve when it vacates.
 */
typedef struct {
    uint32_t struct_size;
    fdk_shell_notify_event_fn event_cb;
    void *user;
    const char *server_name;
    uint32_t default_timeout_ms;
    int max_live;
    int replace;
    int wait;
} fdk_shell_notify_server_desc;

/* Creates the server on `bus` (READY — see the tray host's note;
 * FDK_ERR_NOT_INITIALIZED otherwise) and starts the name etiquette.
 * The whole method surface is served from here: Notify (with the
 * flood gate and the live-set cap), CloseNotification,
 * GetCapabilities, GetServerInformation, Properties, Introspect,
 * Peer — the model owns the connection's ONE method handler: one
 * notification server per bus connection. Name acquisition is
 * asynchronous: SERVER_STATE + serving() report the outcome.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - bus/ctx/out NULL, or replace and wait
 *                              both set
 *   FDK_ERR_NOT_INITIALIZED  - the connection is not ready
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_shell_notify_server_create(
    fdk_dbus_connection *bus, fdk_context *ctx,
    const fdk_shell_notify_server_desc *desc,
    fdk_shell_notify_server **out);

/* Tears the server down: closes every live notification (the
 * NotificationClosed signals are NOT emitted — teardown, not
 * delivery, the fdk_shell_shutdown discipline), releases the name.
 * NULL is a safe no-op. */
void fdk_shell_notify_server_destroy(fdk_shell_notify_server *server);

int fdk_shell_notify_server_serving(const fdk_shell_notify_server *server);

/* ---- note snapshots ---------------------------------------------------- */

/* The live note count (newest first is the enumeration order, the
 * reference stack's order). */
uint32_t fdk_shell_notify_note_count(const fdk_shell_notify_server *server);

/* A note snapshot (struct_size discipline; by stable id — ids are
 * unique and never reused within a server's life). The summary and
 * body are the SANITIZED RAW STRINGS (size-capped UTF-8, markup
 * untranslated — the reference notification markup is a rendering
 * concern and stays with the UI). The actions are the (key, label)
 * pairs the client sent, minus the "default" pair (has_default says
 * whether one was present).
 */
typedef struct {
    uint32_t struct_size;
    uint32_t id;
    uint32_t urgency;      /* 0 low, 1 normal, 2 critical            */
    int transient_hint;
    int resident_hint;
    int has_default;       /* a "default" action key was supplied    */
    uint32_t action_count; /* the non-default pairs, in order        */
    char *summary;         /* sanitized raw, or NULL                 */
    size_t summary_len;
    char *body;            /* sanitized raw, or NULL                 */
    size_t body_len;
    char *app_icon_name;   /* the app_icon string arg, or NULL       */
    size_t app_icon_name_len;
    char *image_path;      /* the image-path hint string, or NULL    */
    size_t image_path_len;
    uint32_t timeout_ms;   /* the resolved expiration (0 = never)    */
} fdk_shell_notify_note_info;

/* Snapshots note `id` into *out (same buffer conventions as the tray
 * item snapshot). FDK_ERR_NOT_FOUND when the id is not live. */
fdk_result fdk_shell_notify_note_snapshot(
    const fdk_shell_notify_server *server, uint32_t id,
    fdk_shell_notify_note_info *out);

/* One action pair (index < action_count). FDK_ERR_NOT_FOUND for an
 * out-of-range index. */
fdk_result fdk_shell_notify_note_action(
    const fdk_shell_notify_server *server, uint32_t id,
    uint32_t index, char *key_buf, size_t key_cap, char *label_buf,
    size_t label_cap);

/* The notification's icon BYTES when the client supplied a raw
 * pixmap hint: ARGB32 network-order decoded to host order. `which`
 * selects the source — 0 the "image-data" hint (the modern name,
 * with "image_data" accepted as its spelling variant), 1 the
 * deprecated "icon_data" hint — so a UI can honor the spec's
 * priority order (image-data > image-path > app_icon > icon_data)
 * against its own file/theme lookups. Writes the dimensions to
 * out_w / out_h, the byte count to *out_needed, and copies into buf
 * when cap is large enough (buf may be NULL to probe). Returns
 * FDK_ERR_NOT_FOUND when the note has no such pixmap (the UI falls
 * back to the next source in the priority order). */
fdk_result fdk_shell_notify_note_icon(
    const fdk_shell_notify_server *server, uint32_t id, int which,
    int *out_w, int *out_h, uint8_t *buf, size_t cap, size_t *out_needed);

/* ---- interactions ------------------------------------------------------ */

/* The user activated an action: emits ActionInvocation(id, action_key)
 * with the key verbatim ("default" for the default action), then —
 * unless the notification is resident — closes the note with reason
 * 2 (dismissed). */
fdk_result fdk_shell_notify_action_invoke(
    fdk_shell_notify_server *server, uint32_t id, const char *action_key);

/* Closes the note: removes it, emits NotificationClosed(id, reason)
 * (the spec's reasons: 1 expired, 2 dismissed, 3 the CloseNotification
 * call, 4 unspecified). Reason 0 is internal-only (replacement). */
fdk_result fdk_shell_notify_close(fdk_shell_notify_server *server,
                                  uint32_t id, uint32_t reason);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SHELL_DESKTOP_H */
