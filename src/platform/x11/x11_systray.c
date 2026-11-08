/*
 * x11_systray.c — the X11 system-tray fallback (1.0.0-rc1, round 48)
 *
 * WHY: the tray item speaks StatusNotifierItem (the KDE 2009 D-Bus
 * spec and its ayatana twin) — the protocol of waybar, KDE Plasma,
 * GNOME extensions, and tint2's SNI host. But the classic X11 bar
 * world (tint2's legacy systray, trayer, stalonetray, fvwm, the
 * xfce4-panel legacy mode) speaks the OLDER protocol: the
 * freedesktop System Tray protocol v0.2 — a manager selection
 * (_NET_SYSTEM_TRAY_S<screen>), a dock request via a client
 * message, and the icon as an XEMBED client window the tray
 * reparents and resizes. An SNI-only item is INVISIBLE to every
 * such bar (the field report, 2026-11-08: nm-applet docks on
 * tint2, FDK's item did not — nm-applet falls back to exactly this
 * protocol when no SNI watcher is on the bus). This file implements
 * that fallback so the tray item works on ANY bar with a systray,
 * X11 side — no per-bar code anywhere.
 *
 * THE SEQUENCE (System Tray v0.2 + XEMBED 0.0):
 *   1. Atom _NET_SYSTEM_TRAY_S<screen>; XGetSelectionOwner — the
 *      owner window is the manager. None: not available yet; the
 *      root window keeps ClientMessageMask (selected at connect)
 *      so a restarted bar's MANAGER announcement re-triggers the
 *      dock attempt.
 *   2. Icon window: InputOutput, the tray's advertised 32-bit ARGB
 *      visual when it publishes _NET_SYSTEM_TRAY_VISUAL, else the
 *      screen's default visual; the _XEMBED_INFO property (version
 *      0, XEMBED_MAPPED) must exist before the dock request.
 *   3. ClientMessage to the manager window: _NET_SYSTEM_TRAY_OPCODE
 *      with SYSTEM_TRAY_REQUEST_DOCK (0) and our icon window.
 *   4. The tray reparents the icon (ReparentNotify — THE docking
 *      contract this code reports), announces XEMBED_EMBEDDED_NOTIFY,
 *      and resizes it to the bar's icon size; every ConfigureNotify
 *      repaints the item's pixmap scaled (nearest neighbor — a tray
 *      icon is 16-32 px).
 *   5. Loss: StructureNotifyMask on the manager window — its
 *      DestroyNotify is the bar quitting/restarting; the item is
 *      reported unhosted and the next MANAGER announcement (root
 *      ClientMessage) re-docks.
 *
 * Endianness: the item's pixmaps are ARGB32 network order (big-
 * endian bytes A,R,G,B). The XImage is filled with byte_order
 * LSBFirst — per pixel B,G,R,A for depth 32, B,G,R,0 for depth 24 —
 * which on a little-endian host is the four source bytes reversed,
 * and Xlib applies byte_order on the wire for big-endian hosts.
 */

#define FDK_LOG_TAG "tray"
#include "x11_platform.h"
#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xatom.h> /* XA_VISUALID, XA_CARDINAL */
#include <X11/Xutil.h> /* XVisualInfo, XGetVisualInfo, XDestroyImage */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* XEMBED (the 2003 freedesktop XEmbed spec) constants we must speak. */
#define FDK_XEMBED_VERSION 0
#define FDK_XEMBED_MAPPED 1u
#define FDK_XEMBED_EMBEDDED_NOTIFY 0

struct fdk_x11_systray {
    fdk_platform_connection *conn; /* the owning connection         */
    Window icon_w;   /* the icon window the tray embeds (owned)   */
    Window mgr_w;    /* the tray's manager window (watched)       */
    Visual *visual;  /* the icon window's visual                  */
    int depth;

    uint8_t *argb;   /* the item's pixmap, ARGB32 network order (owned) */
    int32_t w, h;

    int docked;      /* the tray confirmed the embed              */

    void (*on_event)(void *user, int docked);
    void *user;

    struct fdk_x11_systray *next; /* the connection's handle list */
};

/* The atoms, interned once per process on first use. */
static struct {
    Atom selection;      /* _NET_SYSTEM_TRAY_S<screen>            */
    Atom opcode;         /* _NET_SYSTEM_TRAY_OPCODE               */
    Atom visual;         /* _NET_SYSTEM_TRAY_VISUAL               */
    Atom xembed_info;    /* _XEMBED_INFO                          */
    Atom manager;        /* MANAGER (the root announcement)       */
    Atom xembed;         /* XEMBED (the embedded notification)    */
    int ready;
} g_systray_atoms;

static int systray_atoms_init(fdk_platform_connection *conn) {
    if (g_systray_atoms.ready) {
        return 1;
    }
    char name[64];
    snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", conn->screen);
    g_systray_atoms.selection = XInternAtom(conn->display, name, False);
    g_systray_atoms.opcode =
        XInternAtom(conn->display, "_NET_SYSTEM_TRAY_OPCODE", False);
    g_systray_atoms.visual =
        XInternAtom(conn->display, "_NET_SYSTEM_TRAY_VISUAL", False);
    g_systray_atoms.xembed_info =
        XInternAtom(conn->display, "_XEMBED_INFO", False);
    g_systray_atoms.manager = XInternAtom(conn->display, "MANAGER", False);
    g_systray_atoms.xembed = XInternAtom(conn->display, "XEMBED", False);
    g_systray_atoms.ready = 1;
    return g_systray_atoms.selection != None;
}

/* ---- painting -------------------------------------------------------------- */

/* Nearest-neighbor scale of the stored ARGB32 pixmap into an
 * LSBFirst ZPixmap buffer for the icon window's depth. */
static uint8_t *systray_render(fdk_x11_systray *st, int tw, int th) {
    if (st->argb == NULL || st->w <= 0 || st->h <= 0 || tw <= 0 || th <= 0) {
        return NULL;
    }
    uint8_t *buf = fdk_alloc((size_t)tw * (size_t)th * 4);
    if (buf == NULL) {
        return NULL;
    }
    for (int y = 0; y < th; y++) {
        int sy = (int)(((int64_t)y * st->h) / th);
        const uint8_t *src = st->argb + (size_t)sy * (size_t)st->w * 4;
        uint8_t *dst = buf + (size_t)y * (size_t)tw * 4;
        for (int x = 0; x < tw; x++) {
            int sx = (int)(((int64_t)x * st->w) / tw);
            const uint8_t *px = src + (size_t)sx * 4;
            /* Source ARGB32 network order: [A,R,G,B]. LSBFirst
             * ZPixmap: [B,G,R,A] (depth 32) or [B,G,R,0] (24). */
            dst[0] = px[3];
            dst[1] = px[2];
            dst[2] = px[1];
            dst[3] = st->depth >= 32 ? px[0] : 0;
            dst += 4;
        }
    }
    return buf;
}

static void systray_paint(fdk_x11_systray *st, fdk_platform_connection *conn) {
    XWindowAttributes wa;
    if (st->icon_w == None || !XGetWindowAttributes(conn->display, st->icon_w, &wa)) {
        return;
    }
    if (wa.width <= 0 || wa.height <= 0) {
        return;
    }
    uint8_t *buf = systray_render(st, wa.width, wa.height);
    if (buf == NULL) {
        return;
    }
    XImage *img = XCreateImage(conn->display, wa.visual, (unsigned)wa.depth,
                               ZPixmap, 0, (char *)buf,
                               (unsigned)wa.width, (unsigned)wa.height,
                               32, 0);
    if (img == NULL) {
        fdk_free(buf);
        return;
    }
    img->byte_order = LSBFirst;
    GC gc = XCreateGC(conn->display, st->icon_w, 0, NULL);
    XPutImage(conn->display, st->icon_w, gc, img, 0, 0, 0, 0,
              (unsigned)wa.width, (unsigned)wa.height);
    XFreeGC(conn->display, gc);
    XDestroyImage(img); /* frees buf */
}

/* ---- the docking lifecycle -------------------------------------------------- */

static void systray_confirm(fdk_x11_systray *st, int docked) {
    if (st->docked == docked) {
        return;
    }
    st->docked = docked;
    FDK_INFO("tray: the XEmbed systray %s", docked ? "confirmed the dock"
                                                   : "went away");
    if (st->on_event != NULL) {
        st->on_event(st->user, docked);
    }
}

static void systray_attempt_dock(fdk_platform_connection *conn,
                                 fdk_x11_systray *st) {
    Display *dpy = conn->display;
    Window mgr = XGetSelectionOwner(dpy, g_systray_atoms.selection);
    if (mgr == None) {
        /* No bar yet: stay armed for the MANAGER announcement. */
        return;
    }
    if (st->icon_w == None) {
        /* The icon window: the tray's advertised 32-bit ARGB visual
         * when it publishes one (bars composite alpha), else the
         * screen default. */
        Visual *visual = DefaultVisual(dpy, conn->screen);
        int depth = DefaultDepth(dpy, conn->screen);
        unsigned long *ids = NULL;
        unsigned long n = 0;
        unsigned long bytes_after = 0;
        Atom actual = None;
        int fmt = 0;
        if (XGetWindowProperty(dpy, mgr, g_systray_atoms.visual, 0, 1,
                               False, XA_VISUALID, &actual, &fmt, &n,
                               &bytes_after,
                               (unsigned char **)&ids) == Success &&
            n == 1 && actual == XA_VISUALID) {
            XVisualInfo want;
            want.visualid = *ids;
            int n_match = 0;
            XVisualInfo *vi = XGetVisualInfo(dpy, VisualIDMask, &want,
                                             &n_match);
            if (vi != NULL && n_match > 0) {
                visual = vi[0].visual;
                depth = vi[0].depth;
            }
            if (vi != NULL) {
                XFree(vi);
            }
        }
        if (ids != NULL) {
            XFree(ids);
        }
        st->visual = visual;
        st->depth = depth;
        XSetWindowAttributes attr;
        memset(&attr, 0, sizeof(attr));
        attr.background_pixel = 0;
        attr.border_pixel = 0;
        attr.event_mask = ExposureMask | StructureNotifyMask;
        st->icon_w = XCreateWindow(
            dpy, conn->root, 0, 0,
            (unsigned)(st->w > 0 ? st->w : 24),
            (unsigned)(st->h > 0 ? st->h : 24), 0, depth, InputOutput,
            visual, CWBackPixel | CWBorderPixel | CWEventMask, &attr);
        /* Format-32 property data is an array of LONG on both sides
         * of Xlib's API (the wire units are 32-bit, the buffer is
         * not) — an int32 buffer here writes garbage half-words. */
        long info[2] = { FDK_XEMBED_VERSION, (long)FDK_XEMBED_MAPPED };
        XChangeProperty(dpy, st->icon_w, g_systray_atoms.xembed_info,
                        XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)info, 2);
    }
    st->mgr_w = mgr;
    XSelectInput(dpy, mgr, StructureNotifyMask);
    XClientMessageEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.window = mgr;
    ev.message_type = g_systray_atoms.opcode;
    ev.format = 32;
    ev.data.l[0] = CurrentTime;
    ev.data.l[1] = 0; /* SYSTEM_TRAY_REQUEST_DOCK */
    ev.data.l[2] = (long)st->icon_w;
    XSendEvent(dpy, mgr, False, NoEventMask, (XEvent *)&ev);
    FDK_INFO("tray: dock request sent to the X11 systray manager "
             "(0x%lx, icon 0x%lx)",
             (unsigned long)mgr, (unsigned long)st->icon_w);
}

/* ---- dispatch (x11_dispatch.c routes BEFORE the window table) ---------------- */

int fdk__x11_systray_handle(fdk_platform_connection *conn, XEvent *xev) {
    if (!g_systray_atoms.ready || conn->systray == NULL) {
        return 0;
    }
    fdk_x11_systray *st = conn->systray;
    if (xev->type == ClientMessage &&
        xev->xclient.message_type == g_systray_atoms.manager &&
        xev->xclient.format == 32) {
        /* A bar (re)started: the MANAGER announcement carries the
         * selection atom and the manager window. Re-dock every
         * handle that is not currently docked. */
        if ((Atom)xev->xclient.data.l[1] != g_systray_atoms.selection) {
            return 0; /* another manager's announcement (XSETTINGS...) */
        }
        for (fdk_x11_systray *it = st; it != NULL; it = it->next) {
            if (!it->docked) {
                systray_attempt_dock(conn, it);
            }
        }
        return 0; /* other MANAGER listeners may exist */
    }
    while (st != NULL) {
        if (xev->xany.window == st->icon_w) {
            switch (xev->type) {
            case ReparentNotify:
                if (xev->xreparent.parent == st->mgr_w) {
                    systray_confirm(st, 1);
                    systray_paint(st, conn);
                }
                break;
            case ConfigureNotify:
                systray_paint(st, conn);
                break;
            case Expose:
                systray_paint(st, conn);
                break;
            case ClientMessage:
                if (xev->xclient.message_type == g_systray_atoms.xembed &&
                    xev->xclient.format == 32 &&
                    (long)xev->xclient.data.l[1] ==
                        FDK_XEMBED_EMBEDDED_NOTIFY) {
                    systray_confirm(st, 1);
                    systray_paint(st, conn);
                }
                break;
            case DestroyNotify:
                /* The server destroyed the icon window — a dying bar
                 * takes its embedded children with it, so this rides
                 * the manager's death, and the handle must rebuild
                 * (icon_w = None) before the next dock attempt. */
                st->icon_w = None;
                if (st->mgr_w != None) {
                    systray_confirm(st, 0);
                }
                break;
            default:
                break;
            }
            return 1;
        }
        if (xev->type == DestroyNotify &&
            xev->xdestroywindow.window == st->mgr_w) {
            /* The bar quit: the server destroys the tray's windows
             * AND their embedded children (the icon died with it —
             * the DestroyNotify branch above may have seen it
             * first). Recreate on the next manager announcement. */
            st->mgr_w = None;
            st->icon_w = None;
            int was = st->docked;
            st->docked = 0;
            if (was && st->on_event != NULL) {
                st->on_event(st->user, 0);
            }
            return 1;
        }
        st = st->next;
    }
    return 0;
}

/* ---- the ops the tray item calls --------------------------------------------- */

static fdk_result fdk_x11_systray_open(fdk_platform_connection *conn,
                                       const uint8_t *argb32, int32_t w,
                                       int32_t h, void *user,
                                       void (*on_event)(void *user, int docked),
                                       void **out_handle) {
    if (conn == NULL || out_handle == NULL || !systray_atoms_init(conn)) {
        return FDK_ERR_PLATFORM_INIT;
    }
    fdk_x11_systray *st = fdk_alloc(sizeof(*st));
    if (st == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(st, 0, sizeof(*st));
    if (argb32 != NULL && w > 0 && h > 0) {
        st->argb = fdk_alloc((size_t)w * (size_t)h * 4);
        if (st->argb == NULL) {
            fdk_free(st);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        memcpy(st->argb, argb32, (size_t)w * (size_t)h * 4);
        st->w = w;
        st->h = h;
    }
    st->on_event = on_event;
    st->user = user;
    st->conn = conn;
    st->next = conn->systray;
    conn->systray = st;
    *out_handle = st;
    systray_attempt_dock(conn, st);
    if (st->docked) {
        systray_paint(st, conn);
    }
    return FDK_OK;
}

static fdk_result fdk_x11_systray_set_icon(void *handle,
                                           const uint8_t *argb32, int32_t w,
                                           int32_t h) {
    fdk_x11_systray *st = handle;
    if (st == NULL || argb32 == NULL || w <= 0 || h <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    uint8_t *copy = fdk_alloc((size_t)w * (size_t)h * 4);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, argb32, (size_t)w * (size_t)h * 4);
    fdk_free(st->argb);
    st->argb = copy;
    st->w = w;
    st->h = h;
    /* The stored pointer rides the connection for repaints, so the
     * caller needs the connection: the paint goes through the next
     * expose/configure; a docked icon gets one immediate repaint
     * via the handle's connection pointer stashed below. */
    if (st->docked && st->conn != NULL) {
        systray_paint(st, st->conn);
    }
    return FDK_OK;
}

static void fdk_x11_systray_close(void *handle) {
    fdk_x11_systray *st = handle;
    if (st == NULL) {
        return;
    }
    /* Remove from the connection's list first (the XDestroyWindow
     * below generates events this dispatcher would otherwise see). */
    fdk_platform_connection *conn = st->conn;
    if (conn != NULL && conn->systray == st) {
        conn->systray = st->next;
    } else if (conn != NULL) {
        for (fdk_x11_systray *prev = conn->systray; prev != NULL;
             prev = prev->next) {
            if (prev->next == st) {
                prev->next = st->next;
                break;
            }
        }
    }
    if (conn != NULL && st->icon_w != None) {
        /* Destroying the icon window is the legal XEmbed exit: the
         * tray notices the structure change and unembeds. */
        XDestroyWindow(conn->display, st->icon_w);
    }
    fdk_free(st->argb);
    fdk_free(st);
}

void fdk__x11_systray_disconnect(fdk_platform_connection *conn) {
    while (conn->systray != NULL) {
        fdk_x11_systray *st = conn->systray;
        conn->systray = st->next;
        if (st->icon_w != None) {
            XDestroyWindow(conn->display, st->icon_w);
        }
        fdk_free(st->argb);
        fdk_free(st);
    }
    memset(&g_systray_atoms, 0, sizeof(g_systray_atoms));
}

/* The ops table wired into g_x11_ops (x11_ops.c); NULL on backends
 * without the protocol (Wayland). */
const fdk__platform_systray_ops fdk__x11_systray_ops = {
    .open = fdk_x11_systray_open,
    .set_icon = fdk_x11_systray_set_icon,
    .close = fdk_x11_systray_close,
};
