/*
 * tray_internal.h — the shared shapes of the tray module (internal)
 *
 * The menu (dbusmenu_server.c) and the item (tray_item.c) share one
 * connection and one method dispatcher: the item owns the bus, the
 * menu rides on it at /MenuBar. Not installed, not public.
 */

#ifndef FDK_TRAY_INTERNAL_H
#define FDK_TRAY_INTERNAL_H

#define FDK_LOG_TAG "tray"
#include "core/log_internal.h"

#include "fdk/fdk_dbus.h"
#include "fdk/fdk_tray.h"

#include <stddef.h>
#include <stdint.h>

/* ---- the menu model ------------------------------------------------------- */

typedef struct menu_node {
    uint32_t parent; /* node id, 0 = under the root                    */
    int separator;
    char *label;     /* owned; NULL for separators                     */
    int enabled;     /* dbusmenu default is true                       */
    int visible;     /* dbusmenu default is true                       */
    int check_kind;  /* fdk_tray_menu_check                            */
    int toggle;      /* 0/1                                            */
    int dirty;       /* AboutToShow handshake (subtree changed)        */
} menu_node;

struct fdk_tray_menu {
    menu_node *nodes; /* nodes[i] has id i+1; order = sibling order   */
    size_t n, cap;
    uint32_t revision;
    uint32_t root_asked_rev; /* AboutToShow(0)'s dirty answer      */

    fdk_tray_menu_click_fn click_fn;
    void *click_user;
    fdk_tray_menu_show_fn show_fn;
    void *show_user;

    fdk_tray_item *item; /* the transport while attached             */
};

/* dbusmenu serving (dbusmenu_server.c). Returns 1 when the member
 * belonged to com.canonical.dbusmenu (answered or errored), 0 when
 * the caller should keep dispatching. */
int fdk__tray_menu_serve_method(fdk_tray_menu *menu,
                                fdk_dbus_connection *bus,
                                fdk_dbus_message *call);

/* Emit LayoutUpdated through the attached item (no-op detached). */
void fdk__tray_menu_emit_updated(fdk_tray_menu *menu);

/* The Introspect XML for /MenuBar. */
const char *fdk__tray_menu_introspect_xml(void);

/* ---- the item ------------------------------------------------------------- */

typedef struct {
    int32_t w, h;
    uint8_t *bytes; /* owned, ARGB32 network order               */
    size_t len;
} tray_pixmap;

struct fdk_tray_item {
    fdk_context *ctx;
    fdk_tray_item_callbacks cb;
    void *cb_user;

    char *id;       /* REQUIRED (options)                             */
    char *title;
    char *category; /* default "ApplicationStatus"                    */

    fdk_tray_status status;

    char *icon_name;    /* owned or NULL                              */
    tray_pixmap icon_px, att_px;
    char *att_icon_name;

    char *tt_icon, *tt_title, *tt_desc;

    fdk_tray_menu *menu;

    fdk_dbus_connection *bus; /* owned */
    fdk_dbus_name_watcher *watch_kde;
    fdk_dbus_name_watcher *watch_ayatana;
    char *kde_owner;      /* the watchers' current owner states        */
    char *ayatana_owner;
    const char *registered_to; /* which watcher answered              */
    int hosted;
    int registering;
    int tearing_down;

    /* The XEmbed fallback (1.0.0-rc1, round 48): when no SNI watcher
     * is on the bus and the platform offers the classic X11 systray
     * (System Tray v0.2), the item docks a real icon WINDOW the bar
     * embeds — the fallback that makes the tray work on ANY X11 bar.
     * SNI wins the moment a watcher appears. */
    void *xembed;      /* the platform systray handle, or NULL      */
    int xembed_active; /* nonzero while docked/embedding            */

    /* The click menu (round 50): an attached fdk_tray_menu is ALSO
     * the XEmbed fallback's menu — a click pops it locally through
     * the widget layer (the SNI host pops it on SNI bars). Lazily
     * created, released on item teardown; the adapter (and the font
     * it owns) dies with the popup's close callback. */
    void *popup_host;  /* fdk_window* — the hidden 1x1 popup anchor */
    void *popup;       /* fdk_tray_popup_adapter* while a popup is open */
};

/* ---- the XEmbed popup adapter (round 50) --------------------------------------- */

/* Forward decls matching the public headers' forms (identical
 * typedef redefinition is legal C11). */
typedef struct fdk_menu fdk_menu;
typedef struct fdk_menu_item fdk_menu_item;
typedef struct fdk_font fdk_font;

/* The tray menu mirrored into a widget-layer fdk_menu for the local
 * popup: same rows, same order, check/radio/separators, submenus as
 * borrowed models. Activations dispatch back as tray-menu clicks. */
typedef struct fdk_tray_popup_adapter {
    fdk_tray_menu *tm;     /* borrowed                              */
    fdk_tray_item *owner;  /* weak; NULLed when the item tears down
                            * (window destroys are deferred, so the
                            * closed callback must outlive the item) */
    fdk_menu *menu;        /* OWNED — destroyed with the adapter    */
    fdk_font *font;        /* OWNED — the system font at UI size    */
    fdk_menu **subs;       /* owned submenu models (flat list)      */
    size_t sub_n, sub_cap;
    fdk_menu_item **items; /* owned row handles (flat)              */
    uint32_t *ids;         /* the tray node id of items[i]          */
    size_t n, cap;
    void *popup_window;    /* the session's popup fdk_window* (weak;
                            * the item's close destroys it)         */
} fdk_tray_popup_adapter;

/* Snapshots the tray menu (after the app's about-to-show callback
 * ran, the AboutToShow parity) into a fresh adapter. The adapter
 * loads and owns its own system font. NULL when the menu is empty or
 * the mirror fails. */
fdk_tray_popup_adapter *fdk__tray_menu_build_popup(fdk_tray_menu *tm);
/* Destroys the adapter AND the mirrored models. */
void fdk__tray_menu_adapter_free(fdk_tray_popup_adapter *a);
/* The mirrored model (for the popup seam). */
fdk_menu *fdk__tray_adapter_menu(const fdk_tray_popup_adapter *a);
/* Test seam: the open adapter while a popup is showing, else NULL. */
fdk_tray_popup_adapter *fdk__tray_item_popup(const fdk_tray_item *it);

/* Shared string helper (owned copy, NULL-safe). */
char *fdk__tray_strdup(const char *s);

#endif /* FDK_TRAY_INTERNAL_H */
