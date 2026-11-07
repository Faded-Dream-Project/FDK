/*
 * fdk_tray.h — Faded Dream ToolKit tray icon API (1.1.0)
 *
 * The APP side of the desktop tray: a StatusNotifierItem export
 * (the org.kde.StatusNotifierItem protocol) plus its DBusMenu
 * server (com.canonical.dbusmenu) — everything an FDK application
 * needs to LIVE in the tray of any host (trayhost, Waybar,
 * ashuffle-tray, every KDE/GNOME-extension implementation that
 * speaks the 2009 KDE spec and its ayatana twin) on FDK's own
 * native D-Bus client, with no new dependency of any kind.
 *
 * THE LIFECYCLE. fdk_tray_item_open() owns its OWN session-bus
 * connection and exports the item at the canonical path
 * /StatusNotifierItem (the menu at /MenuBar). It watches the
 * watcher — org.kde.StatusNotifierWatcher first, the
 * org.ayatana twin when the KDE name has no owner — and registers
 * the moment one appears, re-registers when a watcher restarts,
 * and reports HOSTED / UNHOSTED through on_host. A desktop with
 * no bus or no tray host is an ordinary state: the item keeps
 * working, every setter still updates the state a future host
 * will read, nothing spins.
 *
 * THE MENU. fdk_tray_menu is the tree the host renders: labels,
 * separators, check and radio items, submenus (any depth),
 * enabled/disabled rows, live updates. The app builds it with
 * add/remove/set calls; every mutation bumps the revision and
 * emits LayoutUpdated, and hosts re-fetch. Clicks come back
 * through on_click with the node id. Radio semantics are the
 * toolkit-standard ones: marking one radio CHECKED clears the
 * RADIO siblings under the same parent.
 *
 * THE GRACEFUL POSTURE. No bus, or a bus that dies mid-session:
 * reported (hosted == 0), never a crash, never a busy-loop — the
 * item then stays unhosted until the application recreates it (an
 * honest limit: unlike the notification client, whose sends give
 * it a natural retry point, a tray icon has no traffic to piggyback
 * a reconnect on). No watcher: same report, and the moment one
 * APPEARS the item registers with it. A watcher that vanishes
 * mid-session: the item flips to unhosted and waits — a new
 * watcher gets a fresh registration automatically.
 *
 * PIXMAPS are the spec's ARGB32 in NETWORK byte order (big-
 * endian A,R,G,B), one flat buffer per size — fdk surfaces are
 * host-order ARGB8888, so an app converting one repacks four
 * bytes per pixel (the EXTENDING.md example shows the loop).
 *
 * Documentation: docs/EXTENDING.md (the tray chapter),
 * docs/DECISIONS.md D-008. Wire references: the KDE
 * StatusNotifierItem specification and the canonical dbusmenu
 * specification (IMPLEMENTED from, not copied from — the license
 * posture is in docs/THIRD-PARTY.md).
 */

#ifndef FDK_TRAY_H
#define FDK_TRAY_H

#include "fdk_core.h"
#include "fdk_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * The menu (a com.canonical.dbusmenu server model)
 * ========================================================================*/

typedef struct fdk_tray_menu fdk_tray_menu;

/* Toggle kinds. NONE is a plain row; CHECK renders a checkbox;
 * RADIO renders a radio dot — and marking one RADIO checked
 * clears its RADIO siblings under the same parent (the standard
 * toolkit behavior; hosts only draw the state). */
typedef enum {
    FDK_TRAY_MENU_CHECK_NONE = 0,
    FDK_TRAY_MENU_CHECK_CHECK = 1,
    FDK_TRAY_MENU_CHECK_RADIO = 2,
} fdk_tray_menu_check;

/* A click (Event "clicked") on node `id`. */
typedef void (*fdk_tray_menu_click_fn)(fdk_tray_menu *menu, uint32_t id,
                                       void *user);
/* The host is about to show the submenu under `parent_id` (the
 * moment to refresh labels — mutations made here land before the
 * host's GetLayout). parent_id 0 is the root menu itself. */
typedef void (*fdk_tray_menu_show_fn)(fdk_tray_menu *menu,
                                      uint32_t parent_id, void *user);

/* Creates an empty menu (just the root, id 0). `ctx` may be NULL
 * until the menu is attached — a standalone menu is a pure model
 * (useful for tests and previews); it serves and emits only while
 * attached to a hosted item. */
fdk_tray_menu *fdk_tray_menu_create(void);
/* Tears the menu down and detaches it from its item (if any). */
void fdk_tray_menu_close(fdk_tray_menu *menu);

/* Appends a labeled row under `parent_id` (0 = the root level);
 * returns the new node id, or 0 on failure (bad parent, out of
 * memory). Labels may carry the "_" mnemonic convention — hosts
 * strip and underline it (FDK's own trayhost does). */
uint32_t fdk_tray_menu_add(fdk_tray_menu *menu, uint32_t parent_id,
                           const char *label);
/* Appends a separator row; returns the node id or 0. */
uint32_t fdk_tray_menu_add_separator(fdk_tray_menu *menu,
                                     uint32_t parent_id);
/* Removes a node and its whole subtree (the root, id 0, cannot be
 * removed — clear() resets the menu instead). */
void fdk_tray_menu_remove(fdk_tray_menu *menu, uint32_t id);
/* Drops every node (back to the bare root). */
void fdk_tray_menu_clear(fdk_tray_menu *menu);

/* Live updates — each bumps the revision, emits LayoutUpdated
 * (when attached), and marks the node's ancestors dirty for the
 * AboutToShow handshake. set_label(NULL) is a no-op. */
void fdk_tray_menu_set_label(fdk_tray_menu *menu, uint32_t id,
                             const char *label);
void fdk_tray_menu_set_enabled(fdk_tray_menu *menu, uint32_t id,
                               int enabled);
/* kind NONE clears the toggle; state nonzero marks checked. Radio
 * semantics clear the RADIO siblings of the same parent. */
void fdk_tray_menu_set_check(fdk_tray_menu *menu, uint32_t id,
                             fdk_tray_menu_check kind, int state);

/* The callbacks (both optional). */
void fdk_tray_menu_on_click(fdk_tray_menu *menu,
                            fdk_tray_menu_click_fn fn, void *user);
void fdk_tray_menu_on_about_to_show(fdk_tray_menu *menu,
                                    fdk_tray_menu_show_fn fn,
                                    void *user);

/* Diagnostics: the current revision (bumped by every mutation)
 * and the live node count (the root not included). */
uint32_t fdk_tray_menu_revision(const fdk_tray_menu *menu);
size_t fdk_tray_menu_node_count(const fdk_tray_menu *menu);

/* ==========================================================================
 * The item (a StatusNotifierItem export)
 * ========================================================================*/

typedef struct fdk_tray_item fdk_tray_item;

typedef enum {
    FDK_TRAY_STATUS_PASSIVE = 0,       /* hidden / inactive                      */
    FDK_TRAY_STATUS_ACTIVE = 1,        /* normal, visible                        */
    FDK_TRAY_STATUS_NEEDS_ATTENTION = 2, /* the attention icon + host emphasis   */
} fdk_tray_status;

typedef struct {
    const char *id;      /* REQUIRED: app-unique, stable (e.g. "myapp-net")  */
    const char *title;   /* NULL ok — the tooltip title fallback              */
    const char *category; /* NULL -> "ApplicationStatus" (the spec default)  */
} fdk_tray_item_options;

/* All optional. */
typedef struct {
    /* The watcher appeared (hosted 1) or vanished (hosted 0) — the
     * item is live in a tray or waiting for one. */
    void (*on_host)(fdk_tray_item *item, int hosted, void *user);
    /* The host activated the icon (left click); `secondary` marks
     * the right-click activation. Coordinates are the click's, in
     * the host's space (diagnostic for most apps). */
    void (*on_activate)(fdk_tray_item *item, int secondary, int32_t x,
                        int32_t y, void *user);
    /* The host scrolled over the icon (deltas in the spec's units). */
    void (*on_scroll)(fdk_tray_item *item, int32_t dx, int32_t dy,
                      void *user);
    void *user;
} fdk_tray_item_callbacks;

/* Opens the item: connection, watcher watch, registration when a
 * watcher answers. Never NULL except on allocation failure or a
 * missing `opts->id`. */
fdk_tray_item *fdk_tray_item_open(fdk_context *ctx,
                                  const fdk_tray_item_options *opts,
                                  const fdk_tray_item_callbacks *callbacks,
                                  void *user);
/* Detaches the menu (if attached), tears the item down. Safe from
 * inside its own callbacks (deferred, like every FDK teardown). */
void fdk_tray_item_close(fdk_tray_item *item);

/* Nonzero while a watcher holds our registration. */
int fdk_tray_item_hosted(const fdk_tray_item *item);

/* Attach the menu (the Menu property + the /MenuBar server). A
 * menu serves ONE item at a time — attaching a second item
 * detaches the first (logged). NULL detaches. */
void fdk_tray_item_set_menu(fdk_tray_item *item, fdk_tray_menu *menu);

/* State setters — each updates what hosts read (GetAll / the New*
 * signals / PropertiesChanged) whether hosted or not:
 *   icon:        IconName, or the IconPixmap sizes list
 *   attention:   AttentionIconName / AttentionIconPixmap
 *   tooltip:     ToolTip (icon name may be NULL; title REQUIRED)
 *   title:       Title
 *   status:      Status (+ the NewStatus signal's string)
 * Pixmap buffers are ARGB32 NETWORK byte order (see the header
 * banner), copied immediately. Multiple sizes: call set_icon_pixmap
 * repeatedly before... no — each call REPLACES the list with that
 * one size (the multi-size list is a host optimization FDK's own
 * trayhost never needed; the honest single-size answer). */
void fdk_tray_item_set_icon_name(fdk_tray_item *item, const char *name);
void fdk_tray_item_set_icon_pixmap(fdk_tray_item *item, int32_t width,
                                   int32_t height, const uint8_t *argb_be,
                                   size_t len);
void fdk_tray_item_set_attention_icon_name(fdk_tray_item *item,
                                           const char *name);
void fdk_tray_item_set_attention_icon_pixmap(fdk_tray_item *item,
                                             int32_t width,
                                             int32_t height,
                                             const uint8_t *argb_be,
                                             size_t len);
void fdk_tray_item_set_tooltip(fdk_tray_item *item, const char *icon_name,
                               const char *title,
                               const char *description);
void fdk_tray_item_set_title(fdk_tray_item *item, const char *title);
void fdk_tray_item_set_status(fdk_tray_item *item, fdk_tray_status status);

#ifdef __cplusplus
}
#endif

#endif /* FDK_TRAY_H */
