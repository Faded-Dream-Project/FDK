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

#include <stdio.h>

/* ---- per-shell state ------------------------------------------------------ */

typedef struct x11_shell_state {
    Display *dpy; /* borrowed: FDK's connection, its lifetime  */
    int screen;
} x11_shell_state;

/* ---- per-layer state ------------------------------------------------------ */

typedef struct x11_layer_state {
    Window xwindow; /* FDK-owned after adoption; cached for the
                     * strut/grab work below (it equals the ext
                     * surface's native handle) */
    int grabbed;    /* keyboard grab active (EXCLUSIVE mode) */
} x11_layer_state;

/* ---- atoms (interned once per shell) -------------------------------------- */

typedef struct {
    Atom net_wm_window_type;
    Atom net_wm_window_type_dock;
    Atom net_wm_strut;
    Atom net_wm_strut_partial;
} x11_atoms;

static void x11_intern_atoms(Display *dpy, x11_atoms *a) {
    a->net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    a->net_wm_window_type_dock =
        XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    a->net_wm_strut = XInternAtom(dpy, "_NET_WM_STRUT", False);
    a->net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL",
                                          False);
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
    shell->x11 = st;
    return FDK_OK;
}

void fdkshell__x11_shutdown(fdk_shell *shell) {
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

    x11_atoms atoms;
    x11_intern_atoms(dpy, &atoms);

    /* The DOCK type: the EWMH layering semantic. */
    XChangeProperty(dpy, xw, atoms.net_wm_window_type, XA_ATOM, 32,
                    PropModeReplace,
                    (const unsigned char *)&atoms.net_wm_window_type_dock, 1);

    /* The exclusive zone as struts. */
    x11_set_struts(dpy, &atoms, xw, desc, (int)x, (int)y, (int)w, (int)h,
                   screen_w, screen_h);

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
