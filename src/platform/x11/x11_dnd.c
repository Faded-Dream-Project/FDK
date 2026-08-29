#define FDK_LOG_TAG "x11"

/*
 * x11_dnd.c — XDND drag and drop (1.2.0)
 *
 * RECEIVER: FDK windows carry the XdndAware property (version 5,
 * set at window creation). The four client messages a source sends
 * — XDndEnter (the source's type list, inline for <=3 types, else
 * the XdndTypeList property), XDndPosition (root x/y + the action),
 * XDndLeave, XDndDrop — are routed here from fdk_x11_dispatch_pending
 * before ordinary translation. Each Position is answered with an
 * XDndStatus whose accept bit is the intersection of the source's
 * offered types and the window's registered fdk_drag_format mask
 * (the cursor feedback the dragging client shows). A Drop triggers
 * the same bounded convert-and-wait the clipboard uses, but against
 * the XdndSelection with the DROP TARGET window as requestor, then
 * one FDK_EVENT_DRAG_DROP with the decoded payload (POSIX paths /
 * UTF-8 text), then the XDndFinished reply.
 *
 * SOURCE: fdk_drag_begin takes the XdndSelection on the clipboard
 * helper window, grabs the pointer with owner_events=True (so the
 * drag rides the ordinary dispatch — no nested loop, per FDK's
 * threading rules), and tracks the deepest window under the pointer
 * by walking XQueryTree at each motion: new target gets XDndEnter +
 * XDndPosition, dropped target gets XDndLeave, XDndStatus replies
 * (arriving on the helper) remember the target's accept state, and
 * the release either sends XDndDrop (accepted target; the subsequent
 * XDndFinished completes the drag) or cancels. SelectionRequests for
 * the XdndSelection are served from the drag payload (uri-list or
 * text) until Finished arrives — the fetch order the XDND spec
 * requires. A 2s watchdog retires a Finished that never comes (the
 * target died mid-handshake) as CANCELLED, honestly warned.
 */

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xatom.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

/* The widget layer's fdk__strdup is above the platform seam; the
 * backend does its own bounded copy with the shared allocator. */
static char *dnd_strdup(const char *s) {
    size_t len = strlen(s);
    char *out = fdk_alloc(len + 1);
    if (out != NULL) {
        memcpy(out, s, len + 1);
    }
    return out;
}

#define FDK_XDND_VERSION 5
#define FDK_XDND_DROP_TIMEOUT_MS 2000
#define FDK_XDND_WAIT_MS 250

/* ---- init / teardown (called from x11_connection.c) ---- */

fdk_result fdk_x11_dnd_init(fdk_platform_connection *conn) {
    conn->atom_xdnd_aware = XInternAtom(conn->display, "XdndAware", False);
    conn->atom_xdnd_enter = XInternAtom(conn->display, "XdndEnter", False);
    conn->atom_xdnd_position =
        XInternAtom(conn->display, "XdndPosition", False);
    conn->atom_xdnd_status = XInternAtom(conn->display, "XdndStatus", False);
    conn->atom_xdnd_leave = XInternAtom(conn->display, "XdndLeave", False);
    conn->atom_xdnd_drop = XInternAtom(conn->display, "XdndDrop", False);
    conn->atom_xdnd_finished =
        XInternAtom(conn->display, "XdndFinished", False);
    conn->atom_xdnd_action_copy =
        XInternAtom(conn->display, "XdndActionCopy", False);
    conn->atom_xdnd_type_list =
        XInternAtom(conn->display, "XdndTypeList", False);
    conn->atom_xdnd_selection =
        XInternAtom(conn->display, "XdndSelection", False);
    conn->atom_text_uri_list =
        XInternAtom(conn->display, "text/uri-list", False);
    conn->xdnd.source = None;
    conn->xdnd.hover = NULL;
    conn->xdnd_source.active = 0;
    return FDK_OK;
}

void fdk_x11_dnd_shutdown(fdk_platform_connection *conn) {
    /* A source drag still active at shutdown: report it cancelled
     * (the app is going away; the callback may do nothing at all) and
     * release what we hold. The X server drops our grabs and
     * selection ownership with the connection. */
    if (conn->xdnd_source.active) {
        fdk_x11_dnd_source_finish(conn, FDK_DRAG_CANCELLED);
    }
    fdk_free(conn->xdnd_source.text);
    conn->xdnd_source.text = NULL;
    fdk_free(conn->xdnd_source.uri_payload);
    conn->xdnd_source.uri_payload = NULL;
}

/* Stamps the XdndAware property (version 5) on a new window — called
 * from window creation; without it sources never even begin a drag
 * over us. */
void fdk_x11_dnd_window_init(fdk_platform_window *pwindow) {
    Atom version = FDK_XDND_VERSION;
    XChangeProperty(pwindow->conn->display, pwindow->xwindow,
                    pwindow->conn->atom_xdnd_aware, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)&version, 1);
    pwindow->drop_formats = 0;
}

/* ---- shared format mapping ---- */

static int xdnd_types_to_formats(fdk_platform_connection *conn,
                                 const Atom *types, int count) {
    int formats = 0;
    for (int i = 0; i < count; i++) {
        if (types[i] == conn->utf8_string ||
            types[i] == conn->atom_text_plain ||
            types[i] == conn->atom_text) {
            formats |= FDK_DRAG_FORMAT_TEXT;
        } else if (types[i] == conn->atom_text_uri_list) {
            formats |= FDK_DRAG_FORMAT_URI_LIST;
        }
    }
    return formats;
}

/* ---- receiver ---- */

static void xdnd_send(fdk_platform_connection *conn, Window to,
                      Atom type, long l0, long l1, long l2, long l3,
                      long l4) {
    XClientMessageEvent msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = ClientMessage;
    msg.display = conn->display;
    msg.window = to;
    msg.message_type = type;
    msg.format = 32;
    msg.data.l[0] = l0;
    msg.data.l[1] = l1;
    msg.data.l[2] = l2;
    msg.data.l[3] = l3;
    msg.data.l[4] = l4;
    XSendEvent(conn->display, to, False, NoEventMask, (XEvent *)&msg);
    XFlush(conn->display);
}

static void xdnd_send_status(fdk_platform_connection *conn, Window source,
                             int accept) {
    /* Flags: bit0 = accept, bit1 = send position updates while in the
     * rectangle, bit2 = want position events anyway. We always want
     * motions (no rectangle), so bit2 is set — the default every
     * mainstream toolkit ships. */
    long flags = 0x4 | (accept ? 0x1 : 0x0);
    xdnd_send(conn, source, conn->atom_xdnd_status,
              (long)conn->xdnd.dest_window, flags, 0, 0, 0);
}

static void xdnd_dispatch_drag(fdk_platform_window *pwindow,
                               fdk_event_type type, fdk_f32 x, fdk_f32 y,
                               int offered, int accepted) {
    fdk_event_data ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.drag.offered_formats = offered;
    ev.drag.accepted_formats = accepted;
    ev.drag.position.x = x;
    ev.drag.position.y = y;
    pwindow->conn->dispatch(pwindow, &ev,
                            pwindow->conn->dispatch_user_data);
}

static void xdnd_fetch_and_drop(fdk_platform_connection *conn,
                                fdk_platform_window *pwindow,
                                unsigned long timestamp);

int fdk_x11_dnd_handle_client_message(fdk_platform_connection *conn,
                                      const XEvent *xevent) {
    if (xevent->type != ClientMessage) {
        return 0;
    }
    const XClientMessageEvent *msg = &xevent->xclient;
    fdk_platform_window *pwindow =
        fdk_x11_find_window(conn, msg->window);
    FDK_DEBUG("xdnd client message: type=%lu enter=%lu pos=%lu win=%lu pw=%p",
              (unsigned long)msg->message_type,
              (unsigned long)conn->atom_xdnd_enter,
              (unsigned long)conn->atom_xdnd_position,
              (unsigned long)msg->window, (void *)pwindow);

    if (msg->message_type == conn->atom_xdnd_enter && pwindow != NULL) {
        conn->xdnd.source = (Window)msg->data.l[0];
        conn->xdnd.version =
            (int)((msg->data.l[1] >> 24) & 0xFF);
        if (conn->xdnd.version > FDK_XDND_VERSION) {
            conn->xdnd.version = FDK_XDND_VERSION;
        }
        if (msg->data.l[1] & 1) {
            /* >3 types: read the source's XdndTypeList property. */
            Atom type = None;
            int fmt = 0;
            unsigned long n = 0, after = 0;
            unsigned char *data = NULL;
            conn->xdnd.offered = 0;
            if (XGetWindowProperty(conn->display, conn->xdnd.source,
                                   conn->atom_xdnd_type_list, 0, 64,
                                   False, XA_ATOM, &type, &fmt, &n,
                                   &after, &data) == Success &&
                data != NULL && fmt == 32) {
                conn->xdnd.offered = xdnd_types_to_formats(
                    conn, (const Atom *)data, (int)n);
                XFree(data);
            }
        } else {
            Atom three[3] = {
                (Atom)msg->data.l[2], (Atom)msg->data.l[3],
                (Atom)msg->data.l[4],
            };
            conn->xdnd.offered =
                xdnd_types_to_formats(conn, three, 3);
        }
        conn->xdnd.hover = NULL; /* ENTER arms; first Position fires */
        return 1;
    }

    if (msg->message_type == conn->atom_xdnd_position &&
        pwindow != NULL && conn->xdnd.source != None) {
        /* Root coordinates from the message; window-local from the
         * server (the honest translation — what a reparenting WM
         * reports, not arithmetic against a cached origin). */
        int rx = (int)((msg->data.l[2] >> 16) & 0xFFFF);
        int ry = (int)(msg->data.l[2] & 0xFFFF);
        int lx = 0, ly = 0;
        Window child = None;
        XTranslateCoordinates(conn->display, conn->root,
                              pwindow->xwindow, rx, ry, &lx, &ly,
                              &child);
        int accepted =
            conn->xdnd.offered & pwindow->drop_formats;
        conn->xdnd.dest_window = pwindow->xwindow;
        xdnd_send_status(conn, conn->xdnd.source, accepted != 0);
        if (accepted == 0) {
            /* Not acceptable here: whatever hover we had ends (the
             * drag may have moved from an acceptable region of a
             * multi-format app to a window that takes none). */
            if (conn->xdnd.hover == pwindow) {
                conn->xdnd.hover = NULL;
                xdnd_dispatch_drag(pwindow, FDK_EVENT_DRAG_LEAVE,
                                   (fdk_f32)lx, (fdk_f32)ly,
                                   conn->xdnd.offered, 0);
            }
            return 1;
        }
        if (conn->xdnd.hover != pwindow) {
            if (conn->xdnd.hover != NULL) {
                /* The source normally sends Leave itself, but a
                 * Position on a DIFFERENT window while we still track
                 * an old hover (proxy scenarios, missed leaves) gets
                 * the honest synthetic leave. */
                xdnd_dispatch_drag(conn->xdnd.hover,
                                   FDK_EVENT_DRAG_LEAVE, 0, 0,
                                   conn->xdnd.offered, 0);
            }
            conn->xdnd.hover = pwindow;
            xdnd_dispatch_drag(pwindow, FDK_EVENT_DRAG_ENTER,
                               (fdk_f32)lx, (fdk_f32)ly,
                               conn->xdnd.offered, accepted);
        } else {
            xdnd_dispatch_drag(pwindow, FDK_EVENT_DRAG_MOTION,
                               (fdk_f32)lx, (fdk_f32)ly,
                               conn->xdnd.offered, accepted);
        }
        return 1;
    }

    if (msg->message_type == conn->atom_xdnd_leave && pwindow != NULL) {
        if (conn->xdnd.hover == pwindow) {
            xdnd_dispatch_drag(pwindow, FDK_EVENT_DRAG_LEAVE, 0, 0,
                               conn->xdnd.offered, 0);
        }
        conn->xdnd.source = None;
        conn->xdnd.hover = NULL;
        return 1;
    }

    if (msg->message_type == conn->atom_xdnd_drop && pwindow != NULL &&
        conn->xdnd.source != None) {
        unsigned long timestamp = (unsigned long)msg->data.l[2];
        int accepted = conn->xdnd.offered & pwindow->drop_formats;
        if (accepted == 0 || conn->xdnd.hover != pwindow) {
            /* Dropped on a window that never entered acceptance: the
             * honest protocol answer is Finished(no-drop). */
            xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                      (long)pwindow->xwindow, 0, 0, 0, 0);
            if (conn->xdnd.hover == pwindow) {
                conn->xdnd.hover = NULL;
            }
            return 1;
        }
        conn->xdnd.hover = NULL;
        xdnd_fetch_and_drop(conn, pwindow, timestamp);
        return 1;
    }

    return 0;
}

/* Bounded wait for the SelectionNotify answering our drop convert —
 * same discipline as the clipboard's wait (events that are not the
 * one we want stay queued; nothing is dispatched re-entrantly). */
static int xdnd_wait_notify(fdk_platform_connection *conn, Window requestor,
                            XEvent *out) {
    uint64_t deadline = (uint64_t)time(NULL) * 1000 + FDK_XDND_WAIT_MS;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    deadline = (uint64_t)ts.tv_sec * 1000u +
               (uint64_t)ts.tv_nsec / 1000000u + FDK_XDND_WAIT_MS;
    XFlush(conn->display);
    for (;;) {
        XEvent ev;
        if (XCheckTypedWindowEvent(conn->display, requestor,
                                   SelectionNotify, &ev)) {
            if (ev.xselection.selection ==
                conn->atom_xdnd_selection) {
                *out = ev;
                return 1;
            }
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u +
                       (uint64_t)ts.tv_nsec / 1000000u;
        if (now >= deadline) {
            return 0;
        }
        struct pollfd pfd = { ConnectionNumber(conn->display), POLLIN, 0 };
        int r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno != EINTR) {
            return 0;
        }
        XEventsQueued(conn->display, QueuedAfterReading);
    }
}

/* Latin-1 (XA_STRING) widening — same shape as the clipboard's. */
static char *xdnd_utf8_from_property(Atom type, const unsigned char *data,
                                     unsigned long len) {
    char *out = fdk_alloc(len * 2 + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (unsigned long i = 0; i < len; i++) {
        unsigned char c = data[i];
        if (type == XA_STRING && c >= 0x80) {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

static void xdnd_fetch_and_drop(fdk_platform_connection *conn,
                                fdk_platform_window *pwindow,
                                unsigned long timestamp) {
    int accepted = conn->xdnd.offered & pwindow->drop_formats;
    /* Prefer the richest format the intersection allows: files over
     * text (a file drop IS text, but the app registered URI_LIST
     * because it wants paths). */
    Atom target = None;
    if (accepted & FDK_DRAG_FORMAT_URI_LIST) {
        target = conn->atom_text_uri_list;
    } else if (accepted & FDK_DRAG_FORMAT_TEXT) {
        target = conn->utf8_string;
    } else {
        xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                  (long)pwindow->xwindow, 0, 0, 0, 0);
        return;
    }

    XConvertSelection(conn->display, conn->atom_xdnd_selection, target,
                      conn->atom_fdk_selection, pwindow->xwindow,
                      (Time)timestamp);
    XEvent notify;
    if (!xdnd_wait_notify(conn, pwindow->xwindow, &notify)) {
        FDK_WARN("dnd: drop source did not answer within %d ms",
                 FDK_XDND_WAIT_MS);
        xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                  (long)pwindow->xwindow, 0, 0, 0, 0);
        return;
    }
    if (notify.xselection.property == None) {
        xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                  (long)pwindow->xwindow, 0, 0, 0, 0);
        return;
    }

    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(conn->display, pwindow->xwindow,
                           conn->atom_fdk_selection, 0, 0x100000, True,
                           AnyPropertyType, &type, &fmt, &n, &after,
                           &data) != Success ||
        data == NULL || fmt != 8) {
        if (data != NULL) {
            XFree(data);
        }
        xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                  (long)pwindow->xwindow, 0, 0, 0, 0);
        return;
    }

    char *payload = xdnd_utf8_from_property(type, data, n);
    XFree(data);
    if (payload == NULL) {
        xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
                  (long)pwindow->xwindow, 0, 0, 0, 0);
        return;
    }

    /* Deliver the DROP with the decoded payload, FDK-owned, freed
     * after the dispatch returns. */
    char **uris = NULL;
    size_t uri_count = 0;
    const char *text = NULL;
    char *text_copy = NULL;
    if (target == conn->atom_text_uri_list) {
        (void)fdk__dnd_parse_uri_list(payload, strlen(payload), &uris,
                                      &uri_count);
        if (uris == NULL) {
            /* Empty uri-list: deliver as an empty text drop so the
             * app at least sees the gesture. */
            text_copy = dnd_strdup("");
            text = text_copy;
        }
    } else {
        text_copy = payload;
        payload = NULL;
        text = text_copy;
    }

    fdk_event_data ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = FDK_EVENT_DRAG_DROP;
    ev.drag.offered_formats = conn->xdnd.offered;
    ev.drag.accepted_formats = accepted;
    ev.drag.text = text;
    ev.drag.uris = uris;
    ev.drag.uri_count = uri_count;
    conn->dispatch(pwindow, &ev, conn->dispatch_user_data);

    fdk__dnd_free_uri_list(uris, uri_count);
    fdk_free(text_copy);
    fdk_free(payload);

    /* XDND v2+: l[1] bit0 says the drop was accepted — the source's
     * result reporting reads it. */
    xdnd_send(conn, conn->xdnd.source, conn->atom_xdnd_finished,
              (long)pwindow->xwindow, 1, 0, 0, 0);
    conn->xdnd.source = None;
}

/* ---- source ---- */

void fdk_x11_dnd_source_finish(fdk_platform_connection *conn, int status) {
    if (!conn->xdnd_source.active) {
        return;
    }
    conn->xdnd_source.active = 0;
    XUngrabPointer(conn->display, CurrentTime);
    XFlush(conn->display);
    if (conn->xdnd_source.target != None) {
        xdnd_send(conn, conn->xdnd_source.target, conn->atom_xdnd_leave,
                  (long)conn->clip_helper, 0, 0, 0, 0);
        conn->xdnd_source.target = None;
    }
    void (*on_done)(int, void *) = conn->xdnd_source.on_done;
    void *user = conn->xdnd_source.on_done_user;
    conn->xdnd_source.on_done = NULL;
    conn->xdnd_source.on_done_user = NULL;
    if (on_done != NULL) {
        on_done(status, user);
    }
}

/* Deepest window under the root coordinate (the drag target search):
 * descend the tree, testing containment via translation. Skips our
 * own origin window (a source dropping onto itself is the app's
 * call, not the toolkit's — XDND allows it, v1 does not). */
static Window xdnd_window_under(fdk_platform_connection *conn, int rx,
                                int ry) {
    Window cur = conn->root;
    for (;;) {
        Window root_ret, parent;
        Window *children = NULL;
        unsigned int nchildren = 0;
        if (!XQueryTree(conn->display, cur, &root_ret, &parent,
                        &children, &nchildren)) {
            return cur;
        }
        Window next = None;
        /* Top of the stack last: XQueryTree returns bottom-to-top, so
         * iterate REVERSED so the topmost wins containment ties. */
        for (int i = (int)nchildren - 1; i >= 0; i--) {
            Window child = children[i];
            if (child == conn->clip_helper) {
                continue;
            }
            int cx = 0, cy = 0;
            Window junk = None;
            if (XTranslateCoordinates(conn->display, conn->root, child,
                                      rx, ry, &cx, &cy, &junk)) {
                /* Containment test needs the child's size. */
                XWindowAttributes wa;
                if (XGetWindowAttributes(conn->display, child, &wa) &&
                    cx >= 0 && cy >= 0 && cx < wa.width &&
                    cy < wa.height) {
                    next = child;
                    break;
                }
            }
        }
        XFree(children);
        if (next == None) {
            return cur;
        }
        cur = next;
    }
}

static void xdnd_source_send_enter(fdk_platform_connection *conn,
                                   Window target) {
    /* <=3 types inline: uri-list and text cover FDK's two formats. */
    long flags = FDK_XDND_VERSION << 24;
    long t2 = 0, t3 = 0, t4 = 0;
    int n = 0;
    if (conn->xdnd_source.formats & FDK_DRAG_FORMAT_URI_LIST) {
        if (n == 0) t2 = (long)conn->atom_text_uri_list;
        n++;
    }
    if (conn->xdnd_source.formats & FDK_DRAG_FORMAT_TEXT) {
        if (n == 0) t2 = (long)conn->utf8_string;
        else if (n == 1) t3 = (long)conn->utf8_string;
        n++;
    }
    xdnd_send(conn, target, conn->atom_xdnd_enter, (long)conn->clip_helper,
              flags, t2, t3, t4);
}

static void xdnd_source_position(fdk_platform_connection *conn, int rx,
                                 int ry, unsigned long time) {
    xdnd_send(conn, conn->xdnd_source.target, conn->atom_xdnd_position,
              (long)conn->clip_helper, 0,
              (long)((rx << 16) | (ry & 0xFFFF)),
              (long)time, (long)conn->atom_xdnd_action_copy);
}

/* Routes events while a drag we started is active. Returns 1 when
 * the event was consumed by the drag. Called FIRST in the dispatch
 * loop. */
int fdk_x11_dnd_source_handle_event(fdk_platform_connection *conn,
                                    XEvent *xevent) {
    if (!conn->xdnd_source.active) {
        return 0;
    }

    if (xevent->type == MotionNotify) {
        FDK_DEBUG("dnd-src motion: state=%u root=%d,%d",
                  xevent->xmotion.state, xevent->xmotion.x_root,
                  xevent->xmotion.y_root);
        if (!(xevent->xmotion.state & Button1Mask)) {
            return 0; /* not our drag anymore (button went away) */
        }
        int rx = xevent->xmotion.x_root;
        int ry = xevent->xmotion.y_root;
        Window target = xdnd_window_under(conn, rx, ry);
        FDK_DEBUG("dnd-src retarget: 0x%lx (old 0x%lx)",
                  (unsigned long)target,
                  (unsigned long)conn->xdnd_source.target);
        if (target == conn->root) {
            target = None;
        }
        if (target != conn->xdnd_source.target) {
            if (conn->xdnd_source.target != None) {
                xdnd_send(conn, conn->xdnd_source.target,
                          conn->atom_xdnd_leave,
                          (long)conn->clip_helper, 0, 0, 0, 0);
            }
            conn->xdnd_source.target = target;
            conn->xdnd_source.target_accepts = 0;
            if (target != None) {
                xdnd_source_send_enter(conn, target);
                xdnd_source_position(conn, rx, ry,
                                     xevent->xmotion.time);
            }
        } else if (target != None) {
            xdnd_source_position(conn, rx, ry, xevent->xmotion.time);
        }
        return 1;
    }

    if (xevent->type == ButtonRelease) {
        if (conn->xdnd_source.target != None &&
            conn->xdnd_source.target_accepts) {
            xdnd_send(conn, conn->xdnd_source.target,
                      conn->atom_xdnd_drop, (long)conn->clip_helper, 0,
                      (long)xevent->xbutton.time, 0, 0);
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            conn->xdnd_source.drop_sent_ms =
                (uint64_t)ts.tv_sec * 1000u +
                (uint64_t)ts.tv_nsec / 1000000u;
            conn->xdnd_source.drop_pending = 1;
            /* Stay grabbed (and active) until XDndFinished — the
             * target will fetch the selection under our feet. */
        } else {
            fdk_x11_dnd_source_finish(conn, FDK_DRAG_CANCELLED);
        }
        return 1;
    }

    if (xevent->type == KeyPress &&
        xevent->xkey.keycode == 9 /* ESC keycode: evdev 1 + 8 */) {
        fdk_x11_dnd_source_finish(conn, FDK_DRAG_CANCELLED);
        return 1;
    }

    /* Everything else during the drag (enters, leaves, exposes) is
     * inert to the drag; let it flow normally. */
    return 0;
}

/* Client messages on the helper while a drag is active: XDndStatus
 * (target's accept state) and XDndFinished (the handshake's end). */
int fdk_x11_dnd_source_handle_helper_message(fdk_platform_connection *conn,
                                             const XEvent *xevent) {
    if (xevent->type != ClientMessage ||
        xevent->xclient.window != conn->clip_helper) {
        return 0;
    }
    const XClientMessageEvent *msg = &xevent->xclient;

    if (msg->message_type == conn->atom_xdnd_status &&
        conn->xdnd_source.active &&
        (Window)msg->data.l[0] == conn->xdnd_source.target) {
        conn->xdnd_source.target_accepts =
            (msg->data.l[1] & 0x1) != 0;
        return 1;
    }

    if (msg->message_type == conn->atom_xdnd_finished &&
        conn->xdnd_source.active) {
        int success =
            (conn->xdnd_source.version >= 2)
                ? ((msg->data.l[1] & 0x1) != 0)
                : conn->xdnd_source.drop_pending;
        conn->xdnd_source.drop_pending = 0;
        fdk_x11_dnd_source_finish(conn, success ? FDK_DRAG_SUCCEEDED
                                                : FDK_DRAG_CANCELLED);
        return 1;
    }
    return 0;
}

/* Watchdog + late-finish retire, called from dispatch_pending. */
void fdk_x11_dnd_source_tick(fdk_platform_connection *conn) {
    if (!conn->xdnd_source.active || !conn->xdnd_source.drop_pending) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000u +
                   (uint64_t)ts.tv_nsec / 1000000u;
    if (now - conn->xdnd_source.drop_sent_ms >
        FDK_XDND_DROP_TIMEOUT_MS) {
        FDK_WARN("dnd: target never sent XDndFinished — retiring the "
                 "drag as cancelled");
        conn->xdnd_source.drop_pending = 0;
        fdk_x11_dnd_source_finish(conn, FDK_DRAG_CANCELLED);
    }
}

/* Serves a SelectionRequest for the XdndSelection from the drag
 * payload (uri-list / UTF-8 text / TARGETS). Called from dispatch
 * when the request names the XdndSelection — before the clipboard's
 * generic serving path, which would otherwise refuse it. */
int fdk_x11_dnd_serve_selection(fdk_platform_connection *conn,
                                const XSelectionRequestEvent *req) {
    if (req->selection != conn->atom_xdnd_selection) {
        return 0;
    }
    Atom property = req->property != None ? req->property : req->target;

    const char *bytes = NULL;
    size_t len = 0;
    Atom type = None;
    if (req->target == conn->atom_targets) {
        Atom targets[2];
        int n = 0;
        if (conn->xdnd_source.uri_payload != NULL) {
            targets[n++] = conn->atom_text_uri_list;
        }
        if (conn->xdnd_source.text != NULL) {
            targets[n++] = conn->utf8_string;
        }
        XChangeProperty(conn->display, req->requestor, property, XA_ATOM,
                        32, PropModeReplace,
                        (const unsigned char *)targets, n);
    } else if (req->target == conn->atom_text_uri_list &&
               conn->xdnd_source.uri_payload != NULL) {
        bytes = conn->xdnd_source.uri_payload;
        len = strlen(bytes);
        type = conn->atom_text_uri_list;
    } else if ((req->target == conn->utf8_string ||
                req->target == conn->atom_text_plain) &&
               conn->xdnd_source.text != NULL) {
        bytes = conn->xdnd_source.text;
        len = strlen(bytes);
        type = conn->utf8_string;
    } else {
        property = None;
    }
    if (bytes != NULL) {
        XChangeProperty(conn->display, req->requestor, property, type,
                        8, PropModeReplace,
                        (const unsigned char *)bytes, (int)len);
    }

    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = SelectionNotify;
    reply.display = req->display;
    reply.requestor = req->requestor;
    reply.selection = req->selection;
    reply.target = req->target;
    reply.property = property;
    reply.time = req->time;
    XSendEvent(conn->display, req->requestor, False, 0,
               (XEvent *)&reply);
    XFlush(conn->display);
    return 1;
}

/* ---- the two ops ---- */

void fdk_x11_window_set_drop_formats(fdk_platform_window *pwindow,
                                     int formats) {
    pwindow->drop_formats = formats;
    /* No protocol traffic: the next XDndPosition reads it live (the
     * accept/reject status reply is the runtime negotiation). */
}

fdk_result fdk_x11_drag_begin(fdk_platform_window *origin, int formats,
                              const char *text,
                              const char *const *uris, size_t uri_count,
                              void (*on_done)(int, void *), void *user) {
    fdk_platform_connection *conn = origin->conn;
    if (conn->xdnd_source.active) {
        return FDK_ERR_PLATFORM; /* one drag at a time, honestly */
    }

    /* Copy the payload first; only act on the server when we hold
     * everything the drag promises. */
    char *text_copy = NULL;
    char *uri_payload = NULL;
    if (formats & FDK_DRAG_FORMAT_TEXT) {
        text_copy = dnd_strdup(text != NULL ? text : "");
        if (text_copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    if (formats & FDK_DRAG_FORMAT_URI_LIST) {
        uri_payload = fdk__dnd_build_uri_list(uris, uri_count);
        if (uri_payload == NULL) {
            fdk_free(text_copy);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }

    if (XSetSelectionOwner(conn->display, conn->atom_xdnd_selection,
                           conn->clip_helper,
                           CurrentTime) == 0 ||
        XGetSelectionOwner(conn->display, conn->atom_xdnd_selection) !=
            conn->clip_helper) {
        fdk_free(text_copy);
        fdk_free(uri_payload);
        FDK_WARN("dnd: could not take the XdndSelection ownership");
        return FDK_ERR_PLATFORM;
    }

    int grab = XGrabPointer(conn->display, origin->xwindow, True,
                            ButtonPressMask | ButtonReleaseMask |
                                PointerMotionMask,
                            GrabModeAsync, GrabModeAsync, None, None,
                            CurrentTime);
    if (grab != GrabSuccess) {
        XSetSelectionOwner(conn->display, conn->atom_xdnd_selection,
                           None, CurrentTime);
        fdk_free(text_copy);
        fdk_free(uri_payload);
        FDK_WARN("dnd: pointer grab refused (%d)", grab);
        return FDK_ERR_PLATFORM;
    }

    conn->xdnd_source.active = 1;
    conn->xdnd_source.version = FDK_XDND_VERSION;
    conn->xdnd_source.formats = formats;
    conn->xdnd_source.text = text_copy;
    conn->xdnd_source.uri_payload = uri_payload;
    conn->xdnd_source.target = None;
    conn->xdnd_source.target_accepts = 0;
    conn->xdnd_source.drop_pending = 0;
    conn->xdnd_source.on_done = on_done;
    conn->xdnd_source.on_done_user = user;
    /* The first motion under the grab starts the handshake; nothing
     * sends Enter until the pointer actually moves. */
    return FDK_OK;
}
