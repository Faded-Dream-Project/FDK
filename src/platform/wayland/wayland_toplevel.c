/*
 * wayland_toplevel.c — the wlr-foreign-toplevel half of D-4 (1.1.0)
 *
 * The Wayland side of include/fdk/fdk_toplevel.h:
 * zwlr_foreign_toplevel_manager_v1 (the sway/labwc/wlroots taskbar
 * protocol) with each handle's title/app_id/state stream folded
 * into the coarse CHANGED model — every `done` delimiter fires one
 * change; the desktop module re-snapshots on top.
 *
 * Ownership: handles are COMPOSITOR-created; the records die with
 * `closed` events (or the manager's `finished`). The tracker holds
 * the manager proxy + the handle table; list close sends manager
 * .stop() and destroys every handle proxy (the protocol's own
 * teardown order).
 *
 * The rig proof: sway headless LISTS the suite's own windows — the
 * integration suite opens the list, maps a second window, and
 * watches it appear (then CLOSES it through the request path: the
 * compositor's close lands as the window's own close-request
 * event, the full round trip).
 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "fdk/fdk_toplevel.h" /* FDK_TOPLEVEL_* state bits */

#include "generated/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- the tracker + handle records --------------------------------------- */

#define FDK_WL_TOPLEVEL_MAX 64

struct fdk_wl_toplevel {
    struct zwlr_foreign_toplevel_handle_v1 *proxy;
    fdk_u64 id;         /* the proxy's pointer identity: stable for
                         * the handle's lifetime (Wayland proxies are
                         * unique per object)                       */
    char title[160];
    char app_id[160];
    fdk_u32 states;
    int live;           /* record in use                            */
};

struct fdk_platform_toplevels {
    fdk_platform_connection *conn;
    struct zwlr_foreign_toplevel_manager_v1 *manager;
    fdk_platform_toplevels_changed_fn on_change;
    void *user;
    struct fdk_wl_toplevel items[FDK_WL_TOPLEVEL_MAX];
    /* The single at() record — the snapshot the wrapper copies
     * from. NOT thread state: one call at a time (UI thread). */
    struct fdk_platform_toplevel snapshot;
};

/* ---- handle listeners ---------------------------------------------------- */

static void tl_title(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *handle,
                     const char *title) {
    struct fdk_platform_toplevels *t = data;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].proxy == handle) {
            snprintf(t->items[i].title, sizeof(t->items[i].title), "%s",
                     title != NULL ? title : "");
            return; /* the `done` that follows fires the change    */
        }
    }
}

static void tl_app_id(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      const char *app_id) {
    struct fdk_platform_toplevels *t = data;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].proxy == handle) {
            snprintf(t->items[i].app_id, sizeof(t->items[i].app_id), "%s",
                     app_id != NULL ? app_id : "");
            return;
        }
    }
}

static void tl_state(void *data,
                     struct zwlr_foreign_toplevel_handle_v1 *handle,
                     struct wl_array *state) {
    struct fdk_platform_toplevels *t = data;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].proxy == handle) {
            fdk_u32 bits = 0;
            uint32_t *s;
            wl_array_for_each(s, state) {
                switch (*s) {
                case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
                    bits |= FDK_TOPLEVEL_MAXIMIZED;
                    break;
                case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
                    bits |= FDK_TOPLEVEL_MINIMIZED;
                    break;
                case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
                    bits |= FDK_TOPLEVEL_FULLSCREEN;
                    break;
                case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
                    bits |= FDK_TOPLEVEL_ACTIVATED;
                    break;
                default:
                    break; /* future states: honestly ignored       */
                }
            }
            t->items[i].states = bits;
            return;
        }
    }
}

static void tl_done(void *data,
                    struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    struct fdk_platform_toplevels *t = data;
    if (t->on_change != NULL) {
        t->on_change(t->user); /* the batch delimiter = the change  */
    }
}

static void tl_closed(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct fdk_platform_toplevels *t = data;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].proxy == handle) {
            zwlr_foreign_toplevel_handle_v1_destroy(handle);
            t->items[i].proxy = NULL;
            t->items[i].live = 0;
            t->items[i].id = 0;
            if (t->on_change != NULL) {
                t->on_change(t->user);
            }
            return;
        }
    }
}

static void tl_output_enter(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *handle,
                            struct wl_output *output) {
    (void)data; (void)handle; (void)output; /* per-output membership:
                                            * no fdk_event carries it */
}

static void tl_output_leave(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *handle,
                            struct wl_output *output) {
    (void)data; (void)handle; (void)output;
}

static void tl_parent(void *data,
                      struct zwlr_foreign_toplevel_handle_v1 *handle,
                      struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)handle; (void)parent; /* modality trees: not
                                             * modeled (the panel's
                                             * flat list)            */
}

static const struct zwlr_foreign_toplevel_handle_v1_listener
    g_toplevel_handle_listener = {
    .title = tl_title,
    .app_id = tl_app_id,
    .output_enter = tl_output_enter,
    .output_leave = tl_output_leave,
    .state = tl_state,
    .done = tl_done,
    .closed = tl_closed,
    .parent = tl_parent,
};

/* ---- manager listeners ---------------------------------------------------- */

static void mgr_toplevel(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *manager,
                         struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)manager;
    struct fdk_platform_toplevels *t = data;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (!t->items[i].live) {
            t->items[i].live = 1;
            t->items[i].proxy = handle;
            t->items[i].id = (fdk_u64)(uintptr_t)handle;
            t->items[i].title[0] = '\0';
            t->items[i].app_id[0] = '\0';
            t->items[i].states = 0;
            zwlr_foreign_toplevel_handle_v1_add_listener(
                handle, &g_toplevel_handle_listener, t);
            return; /* the identity burst + done will fire change  */
        }
    }
    /* Over the table: destroy the proxy (protocol-clean) and stay
     * quiet — a 65-toplevel desktop is someone else's problem. */
    FDK_INFO("foreign-toplevel: over the %d-item table — dropped",
             FDK_WL_TOPLEVEL_MAX);
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

static void mgr_finished(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *manager) {
    (void)manager;
    struct fdk_platform_toplevels *t = data;
    /* The compositor retired the protocol (a log-out race): clear
     * everything; the next change fires the app's re-snapshot to an
     * empty list — the honest degradation, not an error. */
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live) {
            zwlr_foreign_toplevel_handle_v1_destroy(t->items[i].proxy);
            t->items[i].proxy = NULL;
            t->items[i].live = 0;
            t->items[i].id = 0;
        }
    }
    if (t->on_change != NULL) {
        t->on_change(t->user);
    }
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
    g_toplevel_manager_listener = {
    .toplevel = mgr_toplevel,
    .finished = mgr_finished,
};

/* ---- the ops-table surface ------------------------------------------------ */

fdk_result fdk_wayland_toplevels_open(
    fdk_platform_connection *conn,
    fdk_platform_toplevels_changed_fn on_change, void *user,
    struct fdk_platform_toplevels **out) {
    if (conn == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->foreign_toplevel_name == 0) {
        /* Not offered (GNOME, KDE pre-5.x, minimal compositors):
         * the documented degradation. */
        return FDK_ERR_UNSUPPORTED;
    }
    if (conn->foreign_toplevels_live) {
        /* ONE list per connection: the manager proxy carries ONE
         * listener in libwayland (the last add_listener wins), so a
         * second tracker would silently orphan the first. The
         * desktop module answers NULL — the documented
         * one-substrate-per-context contract. */
        return FDK_ERR_INVALID_STATE;
    }
    struct fdk_platform_toplevels *t = fdk_alloc(sizeof(*t));
    if (t == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* LAZY BIND (the sway rig's first lesson, D-031): the manager
     * sends its initial toplevel burst the moment the object
     * exists — the listener must be attached BEFORE the bind's
     * roundtrip can deliver it. Registry-time binding would have
     * sent the whole set into a listenerless proxy (lost events). */
    t->manager = wl_registry_bind(
        conn->registry, conn->foreign_toplevel_name,
        &zwlr_foreign_toplevel_manager_v1_interface, 1);
    if (t->manager == NULL) {
        fdk_free(t);
        return FDK_ERR_UNSUPPORTED;
    }
    t->conn = conn;
    t->on_change = on_change;
    t->user = user;
    memset(t->items, 0, sizeof(t->items));
    memset(&t->snapshot, 0, sizeof(t->snapshot));
    conn->foreign_toplevels_live = 1;
    conn->foreign_toplevel_manager = t->manager; /* teardown finds it */
    zwlr_foreign_toplevel_manager_v1_add_listener(
        t->manager, &g_toplevel_manager_listener, t);
    /* The initial change (the header's "may fire during open"): the
     * protocol's initial burst rides the NEXT roundtrip — but an
     * EMPTY set sends nothing at all (no handles, no done, no
     * event), so the coarse model's "re-snapshot now" signal must
     * come from HERE or an empty desktop would never fire one (the
     * rig caught both shapes: a fresh connection's empty roster and
     * a one-window roster differ only in whether wlroots has
     * anything to emit). */
    if (t->on_change != NULL) {
        t->on_change(t->user);
    }
    *out = t;
    return FDK_OK;
}

void fdk_wayland_toplevels_close(struct fdk_platform_toplevels *t) {
    if (t == NULL) {
        return;
    }
    if (t->conn != NULL) {
        t->conn->foreign_toplevels_live = 0; /* a new open may follow */
        t->conn->foreign_toplevel_manager = NULL; /* ours now        */
    }
    if (t->manager != NULL) {
        /* .stop() asks for no NEW handles; the live ones die here
         * (handle destroy is legal any time — the compositor drops
         * its side). */
        zwlr_foreign_toplevel_manager_v1_stop(t->manager);
        for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
            if (t->items[i].live && t->items[i].proxy != NULL) {
                zwlr_foreign_toplevel_handle_v1_destroy(t->items[i].proxy);
            }
        }
        zwlr_foreign_toplevel_manager_v1_destroy(t->manager);
    }
    fdk_free(t);
}

fdk_u32 fdk_wayland_toplevels_count(struct fdk_platform_toplevels *t) {
    if (t == NULL) {
        return 0;
    }
    fdk_u32 n = 0;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live) {
            n++;
        }
    }
    return n;
}

const struct fdk_platform_toplevel *fdk_wayland_toplevels_at(
    struct fdk_platform_toplevels *t, fdk_u32 index) {
    if (t == NULL) {
        return NULL;
    }
    fdk_u32 n = 0;
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live) {
            if (n == index) {
                t->snapshot.id = t->items[i].id;
                t->snapshot.states = t->items[i].states;
                t->snapshot.title = t->items[i].title;
                t->snapshot.app_id = t->items[i].app_id;
                return &t->snapshot;
            }
            n++;
        }
    }
    return NULL;
}

fdk_result fdk_wayland_toplevels_activate(
    struct fdk_platform_toplevels *t, fdk_u32 index) {
    if (t == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const struct fdk_platform_toplevel *item =
        fdk_wayland_toplevels_at(t, index);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (t->conn->seat == NULL) {
        /* activate REQUIRES a seat object; a seat-less compositor
         * (kiosk-shell) honestly cannot. */
        return FDK_ERR_UNSUPPORTED;
    }
    /* Map the snapshot id back to the handle. */
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].id == item->id) {
            zwlr_foreign_toplevel_handle_v1_activate(t->items[i].proxy,
                                                     t->conn->seat);
            return FDK_OK;
        }
    }
    return FDK_ERR_INVALID_ARGUMENT;
}

fdk_result fdk_wayland_toplevels_close_window(
    struct fdk_platform_toplevels *t, fdk_u32 index) {
    if (t == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const struct fdk_platform_toplevel *item =
        fdk_wayland_toplevels_at(t, index);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (int i = 0; i < FDK_WL_TOPLEVEL_MAX; i++) {
        if (t->items[i].live && t->items[i].id == item->id) {
            zwlr_foreign_toplevel_handle_v1_close(t->items[i].proxy);
            return FDK_OK;
        }
    }
    return FDK_ERR_INVALID_ARGUMENT;
}
