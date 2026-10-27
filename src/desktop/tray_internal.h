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
};

/* Shared string helper (owned copy, NULL-safe). */
char *fdk__tray_strdup(const char *s);

#endif /* FDK_TRAY_INTERNAL_H */
