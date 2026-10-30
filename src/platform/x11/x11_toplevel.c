/*
 * x11_toplevel.c — the EWMH half of D-4 (1.1.0)
 *
 * The X11 side of include/fdk/fdk_toplevel.h: the taskbar substrate
 * as EWMH spells it — _NET_CLIENT_LIST on the root (the WM's
 * client roster), per-window _NET_WM_NAME/WM_NAME + WM_CLASS +
 * _NET_WM_STATE reads, _NET_ACTIVE_WINDOW for focus, and the
 * client-message requests (activate: the S-6 message shape pointed
 * at the foreign window; close: _NET_CLOSE_WINDOW — the polite way,
 * never XKillClient).
 *
 * Live updates ride the root's PropertyChangeMask (selected at
 * connect for the desktop-switch watch): membership churn
 * (_NET_CLIENT_LIST) and focus moves (_NET_ACTIVE_WINDOW) fire the
 * coarse change. THE HONEST X11 GAP (D-031): title edits on OTHER
 * clients' windows are not notified — clients own their PropertyNotify
 * selection, and a toolkit that selects it on foreign windows would
 * fight the WM for event masks. A retitle lands at the next
 * membership/focus churn; the snapshot re-reads titles every time.
 *
 * The bare-X posture: no WM answering (Xvfb without the fake-WM rig,
 * xinit sessions) reads an empty _NET_CLIENT_LIST — the documented
 * degradation, an empty list, never an error.
 */

#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include "fdk/fdk_toplevel.h"

#include <X11/Xatom.h>  /* XA_WINDOW / XA_ATOM / XA_WM_NAME        */
#include <X11/Xutil.h>  /* XClassHint / XGetClassHint              */

#include <stdio.h>
#include <string.h>

#define FDK_X11_TOPLEVEL_MAX 64

struct fdk_platform_toplevels {
    fdk_platform_connection *conn;
    fdk_platform_toplevels_changed_fn on_change;
    void *user;
    struct {
        fdk_u64 id; /* the Window XID: stable, unique, exactly the
                     * identity a taskbar needs                    */
        char title[160];
        char app_id[160];
        fdk_u32 states;
    } items[FDK_X11_TOPLEVEL_MAX];
    fdk_u32 count;
    struct fdk_platform_toplevel snapshot; /* the at() record     */
};

/* ---- snapshot readers ----------------------------------------------------- */

static void x11_read_string(fdk_platform_connection *conn, Window w,
                            Atom prop, Atom type, char *out, size_t cap) {
    out[0] = '\0';
    if (w == None || prop == None) {
        return;
    }
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, left = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, w, prop, 0, 160, False, type,
                           &actual, &fmt, &nitems, &left,
                           &data) != Success ||
        data == NULL || nitems == 0) {
        if (data != NULL) {
            XFree(data);
        }
        return;
    }
    /* NUL-safe copy into the fixed buffer. */
    size_t n = nitems < cap ? nitems : cap - 1;
    memcpy(out, data, n);
    out[n] = '\0';
    XFree(data);
}

static fdk_u32 x11_read_states(fdk_platform_connection *conn, Window w) {
    fdk_u32 bits = 0;
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, left = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, w, conn->net_wm_state, 0, 64,
                           False, XA_ATOM, &actual, &fmt, &nitems, &left,
                           &data) != Success ||
        data == NULL) {
        if (data != NULL) {
            XFree(data);
        }
        return 0;
    }
    Atom *atoms = (Atom *)data;
    for (unsigned long i = 0; i < nitems; i++) {
        if (atoms[i] == conn->net_wm_state_maximized_vert ||
            atoms[i] == conn->net_wm_state_maximized_horiz) {
            bits |= FDK_TOPLEVEL_MAXIMIZED;
        } else if (atoms[i] == conn->net_wm_state_fullscreen) {
            bits |= FDK_TOPLEVEL_FULLSCREEN;
        }
        /* MINIMIZED: _NET_WM_STATE_HIDDEN is the EWMH spelling
         * (withdrawn/iconic windows also skip _NET_CLIENT_LIST in
         * some WMs — the honest platform vocabulary). */
        else if (atoms[i] == conn->net_wm_state_hidden) {
            bits |= FDK_TOPLEVEL_MINIMIZED;
        }
    }
    XFree(data);
    return bits;
}

static Window x11_active_window(fdk_platform_connection *conn) {
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, left = 0;
    unsigned char *data = NULL;
    Window active = None;
    if (XGetWindowProperty(conn->display, conn->root,
                           conn->net_active_window, 0, 1, False,
                           XA_WINDOW, &actual, &fmt, &nitems, &left,
                           &data) == Success &&
        data != NULL && nitems >= 1) {
        active = ((Window *)data)[0];
    }
    if (data != NULL) {
        XFree(data);
    }
    return active;
}

/* THE snapshot: the client list + each window's strings/state. */
static void x11_toplevels_snapshot(struct fdk_platform_toplevels *t) {
    fdk_platform_connection *conn = t->conn;
    t->count = 0;
    Atom actual = None;
    int fmt = 0;
    unsigned long nitems = 0, left = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, conn->root, conn->net_client_list,
                           0, FDK_X11_TOPLEVEL_MAX, False, XA_WINDOW,
                           &actual, &fmt, &nitems, &left,
                           &data) != Success ||
        data == NULL) {
        if (data != NULL) {
            XFree(data);
        }
        return; /* no WM answering: the empty list               */
    }
    Window active = x11_active_window(conn);
    Window *windows = (Window *)data;
    for (unsigned long i = 0; i < nitems && t->count < FDK_X11_TOPLEVEL_MAX;
         i++) {
        Window w = windows[i];
        t->items[t->count].id = (fdk_u64)w;
        /* _NET_WM_NAME (UTF-8) with the WM_NAME fallback (the
         * pre-EWMH spelling still common for xterm-class apps). */
        x11_read_string(conn, w, conn->net_wm_name, conn->utf8_string,
                        t->items[t->count].title,
                        sizeof(t->items[t->count].title));
        if (t->items[t->count].title[0] == '\0') {
            x11_read_string(conn, w, XA_WM_NAME, XA_STRING,
                            t->items[t->count].title,
                            sizeof(t->items[t->count].title));
        }
        /* app_id: EWMH has none — WM_CLASS's res_class is the
         * analog every taskbar actually shows. */
        {
            XClassHint hint;
            memset(&hint, 0, sizeof hint);
            t->items[t->count].app_id[0] = '\0';
            if (XGetClassHint(conn->display, w, &hint)) {
                snprintf(t->items[t->count].app_id,
                         sizeof(t->items[t->count].app_id), "%s",
                         hint.res_class != NULL ? hint.res_class : "");
                if (hint.res_name != NULL) {
                    XFree(hint.res_name);
                }
                if (hint.res_class != NULL) {
                    XFree(hint.res_class);
                }
            }
        }
        t->items[t->count].states = x11_read_states(conn, w);
        if (w == active) {
            t->items[t->count].states |= FDK_TOPLEVEL_ACTIVATED;
        }
        t->count++;
    }
    XFree(data);
}

/* The root-PropertyNotify hook (x11_dispatch.c calls this BEFORE the
 * window-table lookup): membership or focus churn re-snapshots and
 * fires the coarse change. Returns nonzero when consumed. */
int fdk__x11_toplevels_root_notify(fdk_platform_connection *conn,
                                   const XPropertyEvent *ev) {
    if (conn == NULL || ev == NULL || conn->x11_toplevels == NULL) {
        return 0;
    }
    if (ev->window != conn->root ||
        (ev->atom != conn->net_client_list &&
         ev->atom != conn->net_active_window)) {
        return 0;
    }
    struct fdk_platform_toplevels *t = conn->x11_toplevels;
    x11_toplevels_snapshot(t);
    if (t->on_change != NULL) {
        t->on_change(t->user);
    }
    return 1;
}

/* ---- the ops-table surface ------------------------------------------------ */

fdk_result fdk_x11_toplevels_open(
    fdk_platform_connection *conn,
    fdk_platform_toplevels_changed_fn on_change, void *user,
    struct fdk_platform_toplevels **out) {
    if (conn == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->x11_toplevels != NULL) {
        /* ONE list per connection (the root hook is single-slot);
         * the desktop module answers NULL on a second open. */
        return FDK_ERR_INVALID_STATE;
    }
    struct fdk_platform_toplevels *t = fdk_alloc(sizeof(*t));
    if (t == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    t->conn = conn;
    t->on_change = on_change;
    t->user = user;
    t->count = 0;
    memset(t->items, 0, sizeof(t->items));
    memset(&t->snapshot, 0, sizeof(t->snapshot));
    conn->x11_toplevels = t;
    x11_toplevels_snapshot(t);
    /* The initial set lands as one change (the coarse model's
     * "the callback may fire during open itself"). */
    if (t->on_change != NULL) {
        t->on_change(t->user);
    }
    *out = t;
    return FDK_OK;
}

void fdk_x11_toplevels_close(struct fdk_platform_toplevels *t) {
    if (t == NULL) {
        return;
    }
    if (t->conn != NULL) {
        t->conn->x11_toplevels = NULL; /* the root hook goes quiet  */
    }
    fdk_free(t);
}

fdk_u32 fdk_x11_toplevels_count(struct fdk_platform_toplevels *t) {
    return (t != NULL) ? t->count : 0;
}

const struct fdk_platform_toplevel *fdk_x11_toplevels_at(
    struct fdk_platform_toplevels *t, fdk_u32 index) {
    if (t == NULL || index >= t->count) {
        return NULL;
    }
    t->snapshot.id = t->items[index].id;
    t->snapshot.states = t->items[index].states;
    t->snapshot.title = t->items[index].title;
    t->snapshot.app_id = t->items[index].app_id;
    return &t->snapshot;
}

/* The EWMH request sender: a 32-bit client message to the root. */
static void x11_send_root_message(fdk_platform_connection *conn,
                                  Atom type, Window target, long l0,
                                  long l1) {
    XEvent msg;
    memset(&msg, 0, sizeof msg);
    msg.type = ClientMessage;
    msg.xclient.window = target;
    msg.xclient.message_type = type;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = l0;
    msg.xclient.data.l[1] = l1;
    msg.xclient.data.l[2] = 0;
    msg.xclient.data.l[3] = 0;
    msg.xclient.data.l[4] = 0;
    XSendEvent(conn->display, conn->root, False,
               (long)(SubstructureNotifyMask | SubstructureRedirectMask),
               &msg);
    XFlush(conn->display);
}

fdk_result fdk_x11_toplevels_activate(struct fdk_platform_toplevels *t,
                                      fdk_u32 index) {
    if (t == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const struct fdk_platform_toplevel *item = fdk_x11_toplevels_at(t, index);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (t->conn->net_active_window == None) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* The S-6 message shape (source=1 application, the last user
     * timestamp), pointed at the FOREIGN window. */
    x11_send_root_message(t->conn, t->conn->net_active_window,
                          (Window)item->id, 1,
                          (long)t->conn->last_event_time);
    return FDK_OK;
}

fdk_result fdk_x11_toplevels_close_window(
    struct fdk_platform_toplevels *t, fdk_u32 index) {
    if (t == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const struct fdk_platform_toplevel *item = fdk_x11_toplevels_at(t, index);
    if (item == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (t->conn->net_close_window == None) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* _NET_CLOSE_WINDOW: the WM asks the client to close (data.l[0]
     * the timestamp) — the polite path, never XKillClient. */
    x11_send_root_message(t->conn, t->conn->net_close_window,
                          (Window)item->id, (long)t->conn->last_event_time,
                          0);
    return FDK_OK;
}
