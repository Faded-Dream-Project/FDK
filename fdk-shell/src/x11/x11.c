/*
 * x11.c — fdk-shell's X11/XLibre backend (0.1.0)
 *
 * Dock semantics the EWMH world offers, behind the same API the
 * Wayland half implements:
 *
 *   - the layer surface is an ordinary X window typed
 *     _NET_WM_WINDOW_TYPE_DOCK (the property window managers use to
 *     exempt a window from normal decoration/focus policy and keep
 *     it above/below normal windows as docks deserve);
 *   - the EXCLUSIVE ZONE is _NET_WM_STRUT_PARTIAL (plus the legacy
 *     _NET_WM_STRUT for older WMs): the number of pixels along each
 *     screen edge other windows must not use — what a panel "reserves"
 *     so maximized windows stop under it;
 *   - STICKINESS: the layer follows the user across workspaces —
 *     _NET_WM_DESKTOP = 0xFFFFFFFF ("all desktops") and
 *     _NET_WM_STATE_STICKY, set as properties BEFORE mapping (the
 *     EWMH initial-state contract WMs honor at map time) AND
 *     re-asserted as root client messages AFTER mapping (for WMs
 *     that came late, missed the properties, or need the request
 *     shape). This mirrors what layer-shell gives for free: a
 *     zwlr_layer_surface is per-output, not per-workspace, and every
 *     wlroots compositor shows it on all of them.
 *     The sticky window is never unmapped across desktop switches —
 *     which also protects its pixels (an unmapped window's contents
 *     are undefined once remapped; that was the ghost-bar bug);
 *   - STACKING by layer level: TOP and OVERLAY carry
 *     _NET_WM_STATE_ABOVE (plus _NET_WM_STATE_SKIP_TASKBAR /
 *     _NET_WM_STATE_SKIP_PAGER for OVERLAY — the strongest EWMH
 *     stacking behavior available). BACKGROUND/BOTTOM get the plain
 *     dock type only: EWMH has NO below-normal-windows stacking
 *     state, and that delta is documented honestly in the README
 *     together with the fullscreen-over-docks limit and what a
 *     custom WM would need to honor a true overlay layer;
 *   - geometry follows the protocol's anchoring math (anchored axes
 *     stretch between margins; unanchored axes take the desired size
 *     and CENTER — "displayed centered with respect to its anchors",
 *     the wlr-layer-shell wording mirrored exactly);
 *   - keyboard EXCLUSIVE is an XGrabKeyboard while shown; ON_DEMAND
 *     degrades honestly to "focusable" (X11 has no per-surface
 *     on-demand focus protocol).
 *
 * Everything is built through the extension API's documented
 * accessors: the Display* comes from fdk_ext_get_native_display(),
 * the window is created here (unmapped, properties set), and FDK
 * adopts it into the full event/render/popup machinery. The layer
 * LEVEL differences (background/bottom/overlay vs top) are
 * Wayland-only truth — X11 docks stack as docks do; the README
 * documents the delta instead of faking it.
 */

#include "shell_internal.h"

#if defined(FDKSHELL_BUILD_X11)

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h> /* XSizeHints / XSetWMNormalHints (self-positioning) */
#include <X11/extensions/Xrandr.h> /* the output model (0.3.0, M-6) */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- atoms (interned once per shell) -------------------------------------- */

typedef struct {
    Atom net_wm_window_type;
    Atom net_wm_window_type_dock;
    Atom net_wm_strut;
    Atom net_wm_strut_partial;
    Atom net_wm_desktop;
    Atom net_wm_state;
    Atom net_wm_state_sticky;
    Atom net_wm_state_above;
    Atom net_wm_state_skip_taskbar;
    Atom net_wm_state_skip_pager;
    Atom fdk_wm_layer; /* _FDK_WM_LAYER: the private layer name    */
} x11_atoms;

static void x11_intern_atoms(Display *dpy, x11_atoms *a) {
    a->net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a->net_wm_window_type_dock =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a->net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    a->net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL",
                                          False);
    a->net_wm_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    a->net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    /* The private layer contract (0.2.0): _FDK_WM_LAYER carries the
     * layer NAME ("background"/"bottom"/"top"/"overlay") so a
     * coordinating WM can implement true layer stacking without
     * guessing from the EWMH type+state soup. Harmless on WMs that
     * do not know it (an unknown property is ignored). */
    a->fdk_wm_layer = XInternAtom(dpy, "_FDK_WM_LAYER", False);
    a->net_wm_state_sticky =
        XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    a->net_wm_state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    a->net_wm_state_skip_taskbar =
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    a->net_wm_state_skip_pager =
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
}

/* ---- per-shell state ------------------------------------------------------ */

/* One RandR output record (0.3.0, M-6/A-03): a connected output's
 * CRTC geometry, probed on the shell's OWN connection and refreshed
 * on RRNotify. GONE outputs keep their records (the enumeration
 * contract the Wayland side defines: stable slots, REMOVED events,
 * records until shutdown). */
typedef struct {
    int x, y, width, height;  /* root coordinates                 */
    int mm_width, mm_height;  /* the output's physical size        */
    char name[32];            /* the connector name               */
    int gone;                 /* was REMOVED; the record stays     */
} x11_output_rec;

typedef struct x11_shell_state {
    Display *dpy; /* borrowed: FDK's connection, its lifetime  */
    int screen;
    x11_atoms atoms; /* interned once per shell (EWMH names are
                      * server-global, but interning is a round trip
                      * per call — one set per shell keeps layer
                      * creation off that path) */

    /* The output model (0.3.0): the shell's OWN X connection —
     * the add-on's own channel, the same posture as the Wayland
     * side's own wl_registry (D-015). FDK's core connection
     * carries its own internal RandR table for window placement;
     * this one exists so output EVENTS reach the add-on without a
     * core API (the extension boundary stays clean). */
    Display *watch_dpy;   /* NULL when the own connection failed  */
    int randr_event_base; /* -1 when the extension is absent      */
    void *watch_src;      /* fdk_ext_fd_source*, the loop hook    */
    x11_output_rec outputs[16];
    int output_count;
} x11_shell_state;

/* The output table cap. */
#define X11_OUTPUT_CAP 16

/* ---- per-layer state ------------------------------------------------------ */

typedef struct x11_layer_state {
    Window xwindow; /* FDK-owned after adoption; cached for the
                     * strut/grab work below (it equals the ext
                     * surface's native handle) */
    int grabbed;    /* keyboard grab active (EXCLUSIVE mode) */
} x11_layer_state;

/* ---- the output model (0.3.0, M-6/A-03) ------------------------------------
 *
 * The shell's OWN X connection carries the RandR watch: XRRSelectInp
 * on the root + an fd source on the extension API's loop (the
 * documented add-on-socket pattern). The core's connection runs its
 * own internal table for window placement — the two probes are one
 * round trip each, and the extension boundary stays clean (no core
 * output API, matching the Wayland side's own-registry posture).
 *
 * Absent RandR (or a failed own connection): ONE output, the whole
 * screen, no events — the pre-RandR truth, kept as the fallback so
 * single-monitor correctness is a regression pin.
 */

/* Probes the CURRENT table into fresh (gone outputs preserved from
 * the old table by name — the enumeration contract). Returns the
 * number of LIVE (non-gone) records. */
static int x11_randr_probe(x11_shell_state *st) {
    Display *dpy = st->watch_dpy != NULL ? st->watch_dpy : st->dpy;
    Window root = RootWindow(dpy, st->screen);
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (res == NULL) {
        res = XRRGetScreenResources(dpy, root);
    }
    if (res == NULL) {
        return -1;
    }
    x11_output_rec fresh[X11_OUTPUT_CAP];
    int n = 0;
    for (int c = 0; c < res->ncrtc && n < X11_OUTPUT_CAP; c++) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(dpy, res, res->crtcs[c]);
        if (crtc == NULL) {
            continue;
        }
        if (crtc->width == 0 || crtc->height == 0 || crtc->noutput == 0) {
            XRRFreeCrtcInfo(crtc);
            continue;
        }
        XRROutputInfo *out = XRRGetOutputInfo(dpy, res, crtc->outputs[0]);
        if (out == NULL || out->connection != RR_Connected) {
            XRRFreeOutputInfo(out);
            XRRFreeCrtcInfo(crtc);
            continue;
        }
        x11_output_rec *o = &fresh[n++];
        o->x = (int)crtc->x;
        o->y = (int)crtc->y;
        o->width = (int)crtc->width;
        o->height = (int)crtc->height;
        o->mm_width = (int)out->mm_width;
        o->mm_height = (int)out->mm_height;
        snprintf(o->name, sizeof(o->name), "%s", out->name);
        o->gone = 0;
        XRRFreeOutputInfo(out);
        XRRFreeCrtcInfo(crtc);
    }
    XRRFreeScreenResources(res);

    /* Reading order (x, y) — the same rule the core's table uses. */
    for (int i = 1; i < n; i++) {
        x11_output_rec key = fresh[i];
        int j = i - 1;
        while (j >= 0 && (fresh[j].x > key.x ||
                          (fresh[j].x == key.x && fresh[j].y > key.y))) {
            fresh[j + 1] = fresh[j];
            j--;
        }
        fresh[j + 1] = key;
    }

    /* The DIFF: new table vs old, by name. GONE records persist
     * (appended when the cap allows) so their indices stay stable. */
    int live = 0;
    x11_output_rec merged[X11_OUTPUT_CAP];
    int merged_n = 0;
    /* every live old record: gone if absent from fresh */
    for (int i = 0; i < st->output_count && merged_n < X11_OUTPUT_CAP;
         i++) {
        int found = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(st->outputs[i].name, fresh[j].name) == 0) {
                found = j;
                break;
            }
        }
        x11_output_rec *m = &merged[merged_n++];
        if (found < 0) {
            *m = st->outputs[i];
            m->gone = 1;
        } else {
            *m = fresh[found];
            fresh[found].name[0] = '\0'; /* consumed */
        }
    }
    /* every unconsumed fresh record: a new output */
    for (int j = 0; j < n && merged_n < X11_OUTPUT_CAP; j++) {
        if (fresh[j].name[0] != '\0') {
            merged[merged_n++] = fresh[j];
        }
    }
    memcpy(st->outputs, merged, sizeof(merged));
    st->output_count = merged_n;
    for (int i = 0; i < merged_n; i++) {
        if (!st->outputs[i].gone) {
            live++;
        }
    }
    return live;
}


/* Fired with the post-change table to emit the event set. */
static void x11_randr_emit_diff(fdk_shell *shell, int old_live) {
    x11_shell_state *st = shell->x11;
    int live = 0;
    for (int i = 0; i < st->output_count; i++) {
        if (!st->outputs[i].gone) {
            live++;
        }
    }
    if (live > old_live) {
        /* Every still-live index is a candidate; the ADDED one is
         * the last live index (the merge appends new outputs). The
         * coarse event contract (the app re-enumerates) makes the
         * precise index informational. */
        for (int i = 0; i < st->output_count; i++) {
            if (!st->outputs[i].gone) {
                fdkshell__fire_output_event(
                    shell, FDK_SHELL_OUTPUT_ADDED, (uint32_t)i);
            }
        }
    } else if (live < old_live) {
        for (int i = 0; i < st->output_count; i++) {
            if (st->outputs[i].gone) {
                fdkshell__fire_output_event(
                    shell, FDK_SHELL_OUTPUT_REMOVED, (uint32_t)i);
            } else {
                fdkshell__fire_output_event(
                    shell, FDK_SHELL_OUTPUT_CHANGED, (uint32_t)i);
            }
        }
    } else {
        /* same count: geometry/renumber churn -> CHANGED per live
         * output (the app re-reads with get_info) */
        for (int i = 0; i < st->output_count; i++) {
            if (!st->outputs[i].gone) {
                fdkshell__fire_output_event(
                    shell, FDK_SHELL_OUTPUT_CHANGED, (uint32_t)i);
            }
        }
    }
}

/* ---- the output face (0.3.0: the RandR table) ------------------------------ */

static int x11_output_live_count(const x11_shell_state *st) {
    if (st->watch_dpy == NULL || st->output_count == 0) {
        return -1; /* the fallback posture: the screen */
    }
    int live = 0;
    for (int i = 0; i < st->output_count; i++) {
        if (!st->outputs[i].gone) {
            live++;
        }
    }
    return live;
}

/* The record for ENUMERATION index i (live outputs only, in table
 * order); NULL when the table is not in use. */
static const x11_output_rec *x11_output_live_at(const x11_shell_state *st,
                                                uint32_t index) {
    if (st->watch_dpy == NULL) {
        return NULL;
    }
    uint32_t live = 0;
    for (int i = 0; i < st->output_count; i++) {
        if (st->outputs[i].gone) {
            continue;
        }
        if (live == index) {
            return &st->outputs[i];
        }
        live++;
    }
    return NULL;
}

uint32_t fdkshell__x11_output_count(fdk_shell *shell) {
    x11_shell_state *st = shell->x11;
    if (st == NULL) {
        return 0;
    }
    int live = x11_output_live_count(st);
    if (live < 0) {
        return 1; /* the screen: the no-RandR fallback */
    }
    return (uint32_t)live;
}

fdk_result fdkshell__x11_output_name(fdk_shell *shell, uint32_t index,
                                     char *buf, size_t buflen) {
    x11_shell_state *st = shell->x11;
    if (st == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    const x11_output_rec *o = x11_output_live_at(st, index);
    if (o != NULL) {
        snprintf(buf, buflen, "%s", o->name);
        return FDK_OK;
    }
    if (st->watch_dpy != NULL || index != 0) {
        return FDK_ERR_NOT_FOUND; /* the table is authoritative    */
    }
    snprintf(buf, buflen, "screen-0");
    return FDK_OK; /* the no-RandR fallback */
}

fdk_result fdkshell__x11_output_get_info(fdk_shell *shell, uint32_t index,
                                         fdk_shell_output_info *out) {
    x11_shell_state *st = shell->x11;
    if (st == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    const x11_output_rec *o = x11_output_live_at(st, index);
    if (o != NULL) {
        out->x = o->x;
        out->y = o->y;
        out->width = o->width;
        out->height = o->height;
        out->scale = 1; /* FDK's X11 scale applies INSIDE the
                         * window's paint path (logical x scale
                         * physical); the output's own geometry
                         * stays physical pixels */
        out->physical_width = o->mm_width;
        out->physical_height = o->mm_height;
        return FDK_OK;
    }
    if (st->watch_dpy != NULL || index != 0) {
        return FDK_ERR_NOT_FOUND; /* the table is authoritative    */
    }
    /* The no-RandR fallback: the whole screen. */
    out->x = 0;
    out->y = 0;
    out->width = DisplayWidth(st->dpy, st->screen);
    out->height = DisplayHeight(st->dpy, st->screen);
    out->scale = 1;
    out->physical_width = 0; /* the server's screen mm is at the
                              * DisplayWidthMM granularity and rarely
                              * meaningful under Xvfb/EMU — zeros are
                              * the honest no-statement */
    out->physical_height = 0;
    return FDK_OK;
}

/* ---- the test seam (internal linkage, tests/test_x11.c only) ---------------
 *
 * The multi-output ORIGIN math needs a multi-output server; the
 * headless rigs run on Xvfb, which reports ONE fixed output. The
 * seam injects a synthetic table (the same records the probe would
 * have written) so the per-output anchoring is proven end to end —
 * window geometry read back through the protocol — without a
 * second monitor. Not exported (the version script keeps it local
 * to the static library's test links). */

void fdkshell__x11_test_inject_output(fdk_shell *shell, int index,
                                      int x, int y, int w, int h) {
    x11_shell_state *st = shell->x11;
    if (st == NULL || index < 0 || index >= X11_OUTPUT_CAP) {
        return;
    }
    if (index >= st->output_count) {
        st->output_count = index + 1;
    }
    x11_output_rec *o = &st->outputs[index];
    memset(o, 0, sizeof(*o));
    o->x = x;
    o->y = y;
    o->width = w;
    o->height = h;
    snprintf(o->name, sizeof(o->name), "test-%d", index);
    o->gone = 0;
    /* The seam implies the watch exists (the fallback posture would
     * ignore the table). */
    if (st->watch_dpy == NULL) {
        st->watch_dpy = st->dpy; /* pretend: borrowed, never closed */
    }
}

void fdkshell__x11_test_clear_outputs(fdk_shell *shell) {
    x11_shell_state *st = shell->x11;
    if (st == NULL) {
        return;
    }
    if (st->watch_dpy == st->dpy) {
        st->watch_dpy = NULL; /* undo the seam's pretend          */
    }
    x11_randr_probe(st);
}

/* ---- the own-connection watch (the event half) ----------------------------- */

static void x11_watch_readable(fdk_ext_fd_source *source, uint32_t revents,
                               void *user) {
    (void)source;
    fdk_shell *shell = user;
    x11_shell_state *st = shell->x11;
    if (st == NULL || st->watch_dpy == NULL) {
        return;
    }
    if ((revents & (FDK_EXT_FD_ERR | FDK_EXT_FD_HUP)) != 0) {
        /* The own connection died (the server went away): the table
         * freezes at its last truth; the watch stops. The layers
         * ride FDK's (separate) connection. */
        return;
    }
    if ((revents & FDK_EXT_FD_IN) == 0) {
        return;
    }
    int old_live = x11_output_live_count(st);
    int fired = 0;
    while (XPending(st->watch_dpy) > 0) {
        XEvent ev;
        XNextEvent(st->watch_dpy, &ev);
        if (st->randr_event_base >= 0 &&
            (ev.type == st->randr_event_base + RRScreenChangeNotify ||
             ev.type == st->randr_event_base + RRNotify)) {
            if (ev.type == st->randr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
            }
            fired = 1;
        }
    }
    if (fired) {
        int new_live = x11_randr_probe(st);
        (void)new_live;
        x11_randr_emit_diff(shell, old_live);
    }
}

/* Starts the watch: the own connection + the probe + the fd source.
 * Failure at any step leaves the screen fallback in place (the
 * table stays empty and watch_dpy NULL). */
static void x11_output_watch_start(fdk_shell *shell) {
    x11_shell_state *st = shell->x11;
    const char *dpy_name = XDisplayString(st->dpy);
    Display *own = XOpenDisplay(dpy_name != NULL ? dpy_name : NULL);
    if (own == NULL) {
        /* The own channel is unavailable: the screen fallback. */
        return;
    }
    int event_base = 0, error_base = 0;
    if (!XRRQueryExtension(own, &event_base, &error_base)) {
        XCloseDisplay(own);
        return;
    }
    st->watch_dpy = own;
    st->randr_event_base = event_base;
    XRRSelectInput(own, RootWindow(own, st->screen),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask);
    XSync(own, False);
    if (x11_randr_probe(st) < 0) {
        /* The probe itself failed: keep the watch (events may still
         * arrive) but the table stays empty — count falls back to
         * the screen until the first refresh populates it. */
        st->output_count = 0;
    }
    st->watch_src = fdk_ext_loop_add_fd(
        shell->ctx, ConnectionNumber(own), FDK_EXT_FD_IN,
        x11_watch_readable, shell);
    if (st->watch_src == NULL) {
        /* No loop hook: close the own channel, keep the table's
         * initial probe (the app still sees correct outputs, just
         * no live events — the honest degradation). */
        XCloseDisplay(own);
        st->watch_dpy = NULL;
        st->randr_event_base = -1;
    }
}

/* The EWMH "all desktops" value for _NET_WM_DESKTOP. */
#define X11_NET_WM_DESKTOP_ALL 0xFFFFFFFFUL

/* The _NET_WM_STATE atom list a layer level maps to — the stacking
 * half of the layer semantics (see the file header). Populates
 * `out` (up to 4 entries) and returns the count. */
static int x11_layer_state_atoms(const x11_atoms *a,
                                  fdk_shell_layer_level layer, Atom *out) {
    int n = 0;
    out[n++] = a->net_wm_state_sticky; /* every layer is a desktop-
                                        * level surface: sticky */
    if (layer == FDK_SHELL_LAYER_TOP || layer == FDK_SHELL_LAYER_OVERLAY) {
        out[n++] = a->net_wm_state_above;
    }
    if (layer == FDK_SHELL_LAYER_OVERLAY) {
        /* The strongest EWMH stacking behavior: above normal windows
         * AND docks, out of the taskbar and the pager. */
        out[n++] = a->net_wm_state_skip_taskbar;
        out[n++] = a->net_wm_state_skip_pager;
    }
    return n;
}

/* ---- geometry: the anchoring math ------------------------------------------ */

static void x11_axis_geometry(int screen_len, int margin_lo, int margin_hi,
                              int anchor_lo, int anchor_hi, int desired,
                              int *out_pos, int *out_len) {
    if (anchor_lo && anchor_hi) {
        /* Opposite edges: stretch between the margins. */
        *out_pos = margin_lo;
        *out_len = screen_len - margin_lo - margin_hi;
        if (*out_len < 1) {
            *out_len = 1;
        }
    } else if (anchor_lo) {
        *out_pos = margin_lo;
        *out_len = desired > 0 ? desired : 1;
    } else if (anchor_hi) {
        *out_len = desired > 0 ? desired : 1;
        *out_pos = screen_len - margin_hi - *out_len;
    } else {
        /* Unanchored: desired size, centered (the protocol's
         * "centered with respect to its anchors"). */
        *out_len = desired > 0 ? desired : 1;
        *out_pos = (screen_len - *out_len) / 2;
    }
}

/* ---- struts: the exclusive zone -------------------------------------------- */

static void x11_set_struts(Display *dpy, const x11_atoms *atoms, Window w,
                           const fdk_shell_layer_desc *desc, int x, int y,
                           int width, int height, int screen_w,
                           int screen_h) {
    /* WHICH edges reserve space — the wlroots rule, mirrored: an
     * axis reserves when it is anchored to exactly ONE of its two
     * edges (a full-span axis — anchored to both — reserves
     * nothing: the top bar spanning the screen's width blocks the
     * TOP edge, not the left/right ones; a fullscreen layer blocks
     * nothing). A corner widget (one anchor on each axis) reserves
     * both — exactly what _NET_WM_STRUT_PARTIAL's per-edge ranges
     * exist to express. */
    const int a_top = (desc->anchors & FDK_SHELL_ANCHOR_TOP) != 0;
    const int a_bottom = (desc->anchors & FDK_SHELL_ANCHOR_BOTTOM) != 0;
    const int a_left = (desc->anchors & FDK_SHELL_ANCHOR_LEFT) != 0;
    const int a_right = (desc->anchors & FDK_SHELL_ANCHOR_RIGHT) != 0;

    /* The VALUE per reserved edge: explicit zone, else the layer's
     * own extent past its margin (auto). */
    int strut_left = 0, strut_right = 0, strut_top = 0, strut_bottom = 0;
    if (a_left && !a_right) {
        strut_left = (desc->exclusive_zone >= 0)
            ? desc->exclusive_zone
            : (x + width);
    }
    if (a_right && !a_left) {
        strut_right = (desc->exclusive_zone >= 0)
            ? desc->exclusive_zone
            : (screen_w - x);
    }
    if (a_top && !a_bottom) {
        strut_top = (desc->exclusive_zone >= 0)
            ? desc->exclusive_zone
            : (y + height);
    }
    if (a_bottom && !a_top) {
        strut_bottom = (desc->exclusive_zone >= 0)
            ? desc->exclusive_zone
            : (screen_h - y);
    }

    /* _NET_WM_STRUT_PARTIAL: the 12-card layout (left, right, top,
     * bottom + the two per-edge ranges the strut applies to). Each
     * edge's range is the layer's span along the perpendicular
     * axis — the partial-reservation semantics (a 300px centered
     * top bar reserves only its own 300px of the top edge). */
    long partial[12];
    memset(partial, 0, sizeof(partial));
    partial[0] = strut_left;
    partial[1] = strut_right;
    partial[2] = strut_top;
    partial[3] = strut_bottom;
    partial[4] = y;                 /* left_start_y   */
    partial[5] = y + height - 1;   /* left_end_y     */
    partial[6] = y;                 /* right_start_y  */
    partial[7] = y + height - 1;   /* right_end_y    */
    partial[8] = x;                 /* top_start_x    */
    partial[9] = x + width - 1;    /* top_end_x      */
    partial[10] = x;                /* bottom_start_x */
    partial[11] = x + width - 1;   /* bottom_end_x   */
    XChangeProperty(dpy, w, atoms->net_wm_strut_partial, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)partial, 12);

    /* The legacy 4-card strut for WMs that predate PARTIAL (full-edge
     * reservation only — the honest degradation for old WMs). */
    long simple[4] = { strut_left, strut_right, strut_top, strut_bottom };
    XChangeProperty(dpy, w, atoms->net_wm_strut, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)simple, 4);
}

/* ---- the backend contract --------------------------------------------------- */

int fdkshell__x11_supported(fdk_context *ctx) {
    return fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_X11;
}

fdk_result fdkshell__x11_init(fdk_shell *shell) {
    x11_shell_state *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(shell->ctx, &nd)) ||
        nd.display == NULL) {
        free(st);
        return FDK_ERR_NOT_INITIALIZED;
    }
    st->dpy = (Display *)nd.display;
    st->screen = DefaultScreen(st->dpy);
    st->randr_event_base = -1;
    x11_intern_atoms(st->dpy, &st->atoms);
    shell->x11 = st;

    /* The output model (0.3.0): the own-connection RandR watch. Any
     * failure inside leaves the whole-screen fallback. */
    x11_output_watch_start(shell);
    return FDK_OK;
}

void fdkshell__x11_shutdown(fdk_shell *shell) {
    x11_shell_state *st = shell->x11;
    if (st != NULL) {
        if (st->watch_src != NULL) {
            fdk_ext_loop_remove_fd(st->watch_src);
            st->watch_src = NULL;
        }
        if (st->watch_dpy != NULL) {
            XCloseDisplay(st->watch_dpy);
            st->watch_dpy = NULL;
        }
    }
    free(shell->x11);
    shell->x11 = NULL;
}

fdk_result fdkshell__x11_layer_create(fdk_shell *shell,
                                       fdk_shell_layer *layer,
                                       fdk_ext_native_handle *out) {
    x11_shell_state *st = shell->x11;
    const fdk_shell_layer_desc *desc = &layer->desc;

    Display *dpy = st->dpy;
    int screen_w = DisplayWidth(dpy, st->screen);
    int screen_h = DisplayHeight(dpy, st->screen);

    /* Per-output anchoring (0.3.0, M-6): output_index >= 0 anchors
     * against THAT output's geometry and offsets the result into
     * root coordinates (X11 windows live in the root's space);
     * -1 (every output) keeps the whole-screen math — the
     * historical behavior. The screen fallback posture applies when
     * the index is out of range (the honest degradation: an app
     * holding a stale index across a removal gets the screen, not
     * a crash). */
    int out_x = 0, out_y = 0;
    if (desc->output_index >= 0) {
        const x11_output_rec *o =
            x11_output_live_at(st, (uint32_t)desc->output_index);
        if (o != NULL) {
            out_x = o->x;
            out_y = o->y;
            screen_w = o->width;
            screen_h = o->height;
        }
    }

    /* Geometry from the anchoring math. */
    int x = 0, y = 0, w = 0, h = 0;
    x11_axis_geometry(screen_w, desc->margin_left, desc->margin_right,
                      desc->anchors & FDK_SHELL_ANCHOR_LEFT,
                      desc->anchors & FDK_SHELL_ANCHOR_RIGHT, desc->width,
                      &x, &w);
    x11_axis_geometry(screen_h, desc->margin_top, desc->margin_bottom,
                      desc->anchors & FDK_SHELL_ANCHOR_TOP,
                      desc->anchors & FDK_SHELL_ANCHOR_BOTTOM,
                      desc->height, &y, &h);
    x += out_x; /* output-local -> root coordinates               */
    y += out_y;

    /* The native surface: an UNMAPPED dock-typed window (the adopt
     * contract — properties before the map, FDK installs the event
     * mask and registers it). */
    Window xw = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                    (int)x, (int)y, (unsigned)w,
                                    (unsigned)h, 0,
                                    BlackPixel(dpy, st->screen),
                                    WhitePixel(dpy, st->screen));
    if (xw == None) {
        return FDK_ERR_WINDOW_CREATE;
    }

    const x11_atoms *atoms = &st->atoms;

    /* The DOCK type: the EWMH layering semantic. */
    XChangeProperty(dpy, xw, atoms->net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace,
                    (const unsigned char *)&atoms->net_wm_window_type_dock, 1);

    /* _FDK_WM_LAYER (0.2.0): the private layer-name property — the
     * README's custom-WM contract, option 2, now actually set. */
    {
        static const char *layer_names[] = {
            "background", "bottom", "top", "overlay",
        };
        const char *lname =
            (desc->layer <= FDK_SHELL_LAYER_OVERLAY)
                ? layer_names[desc->layer]
                : "top";
        XChangeProperty(dpy, xw, atoms->fdk_wm_layer, XA_STRING, 8,
                        PropModeReplace, (const unsigned char *)lname,
                        (int)strlen(lname));
    }

    /* The namespace -> WM_CLASS res_class mapping (0.2.0): the same
     * rule-matching role the Wayland namespace plays — a WM matches
     * res_class for its window rules. FDK set WM_CLASS from the
     * context's app_id at adoption; a namespace in the desc wins
     * over the generic class (res_name keeps FDK's instance). */
    if (desc->struct_size >=
            offsetof(fdk_shell_layer_desc, namespace_) +
                sizeof(desc->namespace_) &&
        desc->namespace_[0] != '\0') {
        XClassHint hint;
        hint.res_class = (char *)desc->namespace_;
        char res_name[32];
        snprintf(res_name, sizeof res_name, "fdk-shell:%u",
                 (unsigned)(uintptr_t)layer);
        hint.res_name = res_name;
        XSetClassHint(dpy, xw, &hint);
    }

    /* STICKY (the workspace-following contract): _NET_WM_DESKTOP =
     * "all desktops" as the pre-map property (the EWMH initial-state
     * contract a WM honors when it maps the window) — so the layer
     * is never unmapped by a desktop switch and its pixels survive
     * (an unmapped window's contents are undefined on remap). The
     * _NET_WM_STATE list below carries _NET_WM_STATE_STICKY for WMs
     * that key off the state instead of the desktop hint. */
    unsigned long all_desktops = X11_NET_WM_DESKTOP_ALL;
    XChangeProperty(dpy, xw, atoms->net_wm_desktop, XA_CARDINAL, 32,
                    PropModeReplace,
                    (const unsigned char *)&all_desktops, 1);

    /* The layer's stacking state (STICKY + per-level ABOVE/SKIP_*),
     * also pre-map. */
    Atom state_atoms[4];
    int n_state = x11_layer_state_atoms(atoms, desc->layer, state_atoms);
    XChangeProperty(dpy, xw, atoms->net_wm_state, XA_ATOM, 32,
                    PropModeReplace,
                    (const unsigned char *)state_atoms,
                    (int)n_state);

    /* The exclusive zone as struts. */
    x11_set_struts(dpy, atoms, xw, desc, (int)x, (int)y, (int)w, (int)h,
                   screen_w, screen_h);

    /* SELF-POSITIONING (the EWMH contract for dock windows, found
     * live against Openbox 2026-10-26): a dock-typed window with NO
     * position hints is "unplaced" in the WM's eyes — Openbox runs
     * its normal placement policy and CENTERS the panel mid-screen
     * (its debug log says so verbatim: "placed window ... at 312,
     * 370"). The anchoring math above computed the panel's real
     * position; USPosition ("user specified") is the ICCCM bit that
     * makes a WM honor it verbatim — the same thing every real
     * panel sets. PPosition would do for most WMs; USPosition is
     * the stronger claim (never overridden by placement) and is
     * what the EWMH dock examples use. */
    {
        XSizeHints hints;
        memset(&hints, 0, sizeof(hints));
        hints.flags = USPosition | USSize | PPosition | PSize;
        hints.x = (int)x;
        hints.y = (int)y;
        hints.width = (int)w;
        hints.height = (int)h;
        XSetWMNormalHints(dpy, xw, &hints);
    }

    /* Make sure the requests are visible before FDK's adopt probe
     * (XGetGeometry syncs anyway, but the flush keeps the window
     * server-side state coherent for the properties too). */
    XFlush(dpy);

    x11_layer_state *lst = calloc(1, sizeof(*lst));
    if (lst == NULL) {
        XDestroyWindow(dpy, xw);
        XFlush(dpy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    lst->xwindow = xw;
    lst->grabbed = 0;
    layer->backend = lst;
    layer->size.width = (fdk_i32)w;
    layer->size.height = (fdk_i32)h;

    out->backend = FDK_EXT_BACKEND_X11;
    out->xid = (fdk_u64)xw;
    return FDK_OK;
}

void fdkshell__x11_layer_destroy(fdk_shell *shell,
                                  fdk_shell_layer *layer) {
    x11_layer_state *lst = layer->backend;
    if (lst == NULL) {
        return;
    }
    /* Role teardown: release the keyboard grab if we hold one (the
     * X window itself is FDK's — destroyed right after this). */
    if (lst->grabbed) {
        x11_shell_state *st = shell->x11;
        if (st != NULL) {
            XUngrabKeyboard(st->dpy, CurrentTime);
            XFlush(st->dpy);
        }
        lst->grabbed = 0;
    }
    free(lst);
    layer->backend = NULL;
}

void fdkshell__x11_layer_show(fdk_shell_layer *layer) {
    fdk_shell *shell = layer->shell;
    x11_shell_state *st = shell->x11;
    x11_layer_state *lst = layer->backend;
    if (st == NULL || lst == NULL) {
        return;
    }
    /* The layer window is mapped by the fdk_ext_surface_show that
     * ALWAYS runs before this backend hook (shell.c's ordering).
     * Re-assert the EWMH state through the REQUEST shapes now that
     * the window is mapped — belt and braces for WMs that started
     * after the pre-map properties were set, or that only move
     * already-mapped windows through client messages:
     *   _NET_WM_DESKTOP  -> move to "all desktops"
     *   _NET_WM_STATE    -> ADD each of the layer's state atoms
     * (EWMH allows two atoms per _NET_WM_STATE message; a 4-atom
     * overlay layer sends the pair [STICKY, ABOVE] then [SKIP_TB,
     * SKIP_PAGER].) */
    {
        Display *dpy = st->dpy;
        Window root = DefaultRootWindow(dpy);

        XEvent msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = ClientMessage;
        msg.xclient.window = lst->xwindow;
        msg.xclient.message_type = st->atoms.net_wm_desktop;
        msg.xclient.format = 32;
        msg.xclient.data.l[0] = (long)X11_NET_WM_DESKTOP_ALL;
        msg.xclient.data.l[1] = 0; /* timestamp: CurrentTime — the
                                    * EWMH request shape; WMs that
                                    * require a real timestamp still
                                    * honored the pre-map property */
        XSendEvent(dpy, root, False,
                   (long)SubstructureNotifyMask | SubstructureRedirectMask,
                   &msg);

        Atom state_atoms[4];
        int n_state =
            x11_layer_state_atoms(&st->atoms, layer->desc.layer,
                                  state_atoms);
        for (int i = 0; i < n_state; i += 2) {
            memset(&msg, 0, sizeof(msg));
            msg.type = ClientMessage;
            msg.xclient.window = lst->xwindow;
            msg.xclient.message_type = st->atoms.net_wm_state;
            msg.xclient.format = 32;
            msg.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
            msg.xclient.data.l[1] = (long)state_atoms[i];
            msg.xclient.data.l[2] =
                (i + 1 < n_state) ? (long)state_atoms[i + 1] : 0;
            msg.xclient.data.l[3] = 1; /* source indication:
                                        * application (EWMH) */
            msg.xclient.data.l[4] = 0;
            XSendEvent(dpy, root, False,
                       (long)SubstructureNotifyMask |
                           SubstructureRedirectMask,
                       &msg);
        }
        XFlush(dpy);
    }

    /* EXCLUSIVE keyboard: the grab rides the mapped window (X11's
     * answer to zwlr_layer_surface_v1's exclusive interactivity). */
    if (layer->desc.keyboard == FDK_SHELL_KBD_EXCLUSIVE && !lst->grabbed) {
        if (XGrabKeyboard(st->dpy, lst->xwindow, True, GrabModeAsync,
                          GrabModeAsync, CurrentTime) == GrabSuccess) {
            lst->grabbed = 1;
        } else {
            FDKS_WARN("XGrabKeyboard refused — the layer shows without "
                      "exclusive keyboard focus");
        }
    }
}

void fdkshell__x11_layer_hide(fdk_shell_layer *layer) {
    fdk_shell *shell = layer->shell;
    x11_shell_state *st = shell->x11;
    x11_layer_state *lst = layer->backend;
    if (st == NULL || lst == NULL) {
        return;
    }
    if (lst->grabbed) {
        XUngrabKeyboard(st->dpy, CurrentTime);
        XFlush(st->dpy);
        lst->grabbed = 0;
    }
}

#endif /* FDKSHELL_BUILD_X11 */
