/* x11_randr.c — the X11 RandR output model (M-6 / B-05, 1.1.0)
 *
 * The multi-monitor substrate: per-CRTC geometry probed at connect
 * (XRRGetScreenResourcesCurrent — the cached, non-reprobing form)
 * and refreshed on RRNotify. INTERNAL ONLY by design — the core's
 * Wayland backend tracks outputs without an app-facing output API
 * too (the audit's parity note), and the add-on that needs output
 * EVENTS (fdk-shell) drives its OWN X connection for them (the
 * same own-channel posture as its own wl_registry on Wayland —
 * docs/DECISIONS.md D-015).
 *
 * The consumers here:
 *   - window placement: maximize/fullscreen target the monitor
 *     CONTAINING the window (was: the whole screen — the
 *     multi-monitor break the audit named);
 *   - nothing else yet: the scale detection stays whole-screen
 *     (X11 has no per-monitor scale protocol — the honest Xft.dpi
 *     posture documented in x11_connection.c).
 *
 * The fallback: no RandR extension (or a probe failure) = ONE
 * output, the whole screen — exactly the pre-RandR behavior, so
 * single-monitor correctness is a regression pin, not a new path.
 *
 * Indexing contract: outputs are sorted by (x, y) at probe time —
 * left-to-right reading order — so an index is stable across
 * refreshes while the layout stays the same, and a re-probe that
 * changes the layout re-sorts deterministically.
 */
#define FDK_LOG_TAG "x11"
#include "core/log_internal.h"
#include "x11_platform.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <stdio.h>
#include <string.h>

/* The table cap: 16 connected monitors is far past every real
 * X11 desktop (a 16-head wall is a video-wall, and it would be
 * running a real Xorg with its own limits). Probes past the cap
 * are dropped with a WARN — the honest bound. */
#define FDK_X11_MAX_OUTPUTS 16

void fdk__x11_randr_init(fdk_platform_connection *conn) {
    fdk_x11_randr *rr = &conn->randr;
    memset(rr, 0, sizeof(*rr));

    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(conn->display, &event_base, &error_base)) {
        FDK_INFO("x11: no RandR extension — one output (the screen), "
                "no hotplug");
        rr->present = 0;
        rr->event_base = -1;
        return;
    }
    int major = 0, minor = 0;
    XRRQueryVersion(conn->display, &major, &minor);
    rr->present = 1;
    rr->event_base = event_base;
    FDK_INFO("x11: RandR %d.%d present (event base %d)", major, minor,
            event_base);

    /* The hotplug/change masks on the root. RRScreenChangeNotify
     * (screen size changes) + CRTC/Output change notifications.
     * Selected on the CORE's connection: the events arrive in the
     * core's own event queue and x11_dispatch.c routes them to the
     * refresh. */
    XRRSelectInput(conn->display, conn->root,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask);

    fdk__x11_randr_refresh(conn);
}

void fdk__x11_randr_refresh(fdk_platform_connection *conn) {
    fdk_x11_randr *rr = &conn->randr;
    if (!rr->present) {
        return;
    }
    XRRScreenResources *res =
        XRRGetScreenResourcesCurrent(conn->display, conn->root);
    if (res == NULL) {
        /* The ancient-server form: some report resources only via
         * the reprobing call. */
        res = XRRGetScreenResources(conn->display, conn->root);
    }
    if (res == NULL) {
        FDK_WARN("x11: RandR resource probe failed — keeping the "
                 "last-known output table");
        return;
    }

    fdk_x11_output table[FDK_X11_MAX_OUTPUTS];
    int count = 0;
    for (int c = 0; c < res->ncrtc && count < FDK_X11_MAX_OUTPUTS; c++) {
        XRRCrtcInfo *crtc =
            XRRGetCrtcInfo(conn->display, res, res->crtcs[c]);
        if (crtc == NULL) {
            continue;
        }
        if (crtc->width == 0 || crtc->height == 0 || crtc->noutput == 0) {
            /* An disabled CRTC (no mode) or one with no output
             * drives nothing — not a monitor. */
            XRRFreeCrtcInfo(crtc);
            continue;
        }
        /* The CRTC's primary output carries the identity. */
        XRROutputInfo *out =
            XRRGetOutputInfo(conn->display, res, crtc->outputs[0]);
        if (out == NULL || out->connection != RR_Connected) {
            /* A disconnected output on a live CRTC (mirroring
             * setups) — not a placeable monitor. */
            XRRFreeOutputInfo(out);
            XRRFreeCrtcInfo(crtc);
            continue;
        }
        fdk_x11_output *o = &table[count++];
        o->x = (int)crtc->x;
        o->y = (int)crtc->y;
        o->width = (int)crtc->width;
        o->height = (int)crtc->height;
        o->mm_width = (int)out->mm_width;
        o->mm_height = (int)out->mm_height;
        snprintf(o->name, sizeof(o->name), "%s", out->name);
        o->connected = 1;
        XRRFreeOutputInfo(out);
        XRRFreeCrtcInfo(crtc);
    }
    XRRFreeScreenResources(res);

    if (count > FDK_X11_MAX_OUTPUTS) {
        FDK_WARN("x11: RandR reports more than %d outputs — the "
                 "rest are dropped",
                 FDK_X11_MAX_OUTPUTS);
        count = FDK_X11_MAX_OUTPUTS;
    }

    /* Reading order: sort by (x, y) so indices are stable while the
     * layout holds. */
    for (int i = 1; i < count; i++) {
        fdk_x11_output key = table[i];
        int j = i - 1;
        while (j >= 0 &&
               (table[j].x > key.x ||
                (table[j].x == key.x && table[j].y > key.y))) {
            table[j + 1] = table[j];
            j--;
        }
        table[j + 1] = key;
    }

    memcpy(rr->outputs, table, sizeof(table));
    rr->count = count;
    FDK_INFO("x11: RandR outputs: %d", count);
    for (int i = 0; i < count; i++) {
        FDK_INFO("x11:   [%d] \"%s\" %dx%d at +%d+%d (%dmm x %dmm)", i,
                rr->outputs[i].name, rr->outputs[i].width,
                rr->outputs[i].height, rr->outputs[i].x,
                rr->outputs[i].y, rr->outputs[i].mm_width,
                rr->outputs[i].mm_height);
    }
}

int fdk__x11_randr_handle(fdk_platform_connection *conn,
                          const XEvent *ev) {
    fdk_x11_randr *rr = &conn->randr;
    if (!rr->present || ev == NULL) {
        return 0;
    }
    if (ev->type == rr->event_base + RRScreenChangeNotify) {
        /* The screen geometry changed: update Xlib's cached screen
         * sizes first (the documented contract before re-probing),
         * then refresh the table. */
        XRRUpdateConfiguration((XEvent *)(uintptr_t)ev);
        FDK_INFO("x11: RandR screen change — re-probing outputs");
        fdk__x11_randr_refresh(conn);
        return 1;
    }
    if (ev->type == rr->event_base + RRNotify) {
        int sub = ((const XRRNotifyEvent *)ev)->subtype;
        if (sub == RRNotify_CrtcChange || sub == RRNotify_OutputChange ||
            sub == RRNotify_OutputProperty) {
            /* Coalesce: one refresh per event is fine (the probe is
             * one round trip); OutputProperty (dpi hints etc.) also
             * re-reads, keeping the mm fields honest. */
            fdk__x11_randr_refresh(conn);
            return 1;
        }
    }
    return 0;
}

int fdk__x11_randr_output_count(const fdk_platform_connection *conn) {
    if (conn == NULL) {
        return 0;
    }
    return conn->randr.count;
}

const fdk_x11_output *fdk__x11_randr_output_at(
    const fdk_platform_connection *conn, int index) {
    if (conn == NULL || index < 0 || index >= conn->randr.count) {
        return NULL;
    }
    return &conn->randr.outputs[index];
}

void fdk__x11_randr_monitor_containing(const fdk_platform_connection *conn,
                                       int x, int y, int *out_x, int *out_y,
                                       int *out_w, int *out_h) {
    /* The fallback answer: the whole screen (the pre-RandR truth). */
    int sx = 0, sy = 0;
    int sw = DisplayWidth(conn->display, conn->screen);
    int sh = DisplayHeight(conn->display, conn->screen);

    /* The monitor whose area contains the point; on a tie (point
     * on a shared edge) the first in reading order wins. A point
     * outside every monitor (gaps in the layout) takes the NEAREST
     * one by center distance — the honest answer for a window
     * placed in the void. */
    const fdk_x11_output *best = NULL;
    long best_d2 = -1;
    for (int i = 0; i < conn->randr.count; i++) {
        const fdk_x11_output *o = &conn->randr.outputs[i];
        if (x >= o->x && x < o->x + o->width && y >= o->y &&
            y < o->y + o->height) {
            best = o;
            break;
        }
        long dx = x - (o->x + o->width / 2);
        long dy = y - (o->y + o->height / 2);
        long d2 = dx * dx + dy * dy;
        if (best_d2 < 0 || d2 < best_d2) {
            best_d2 = d2;
            best = o;
        }
    }
    if (best != NULL) {
        sx = best->x;
        sy = best->y;
        sw = best->width;
        sh = best->height;
    }
    if (out_x != NULL) {
        *out_x = sx;
    }
    if (out_y != NULL) {
        *out_y = sy;
    }
    if (out_w != NULL) {
        *out_w = sw;
    }
    if (out_h != NULL) {
        *out_h = sh;
    }
}
