/*
 * fdk_shell_x11.c — the X11 side of fdk-shell: an EWMH dock
 * window (override-redirect, _NET_WM_WINDOW_TYPE_DOCK, strut
 * hints) built on FDK's extension API.
 *
 * X11 has no layer-shell; the dock conventions stand in for it:
 *   - override-redirect keeps the WM out of the geometry and the
 *     stacking (a shell surface manages itself),
 *   - _NET_WM_WINDOW_TYPE_DOCK tells WMs what this is,
 *   - _NET_WM_STRUT_PARTIAL reserves the exclusive zone so normal
 *     windows never overlap the bar (the strut's four basic
 *     fields carry the reserved edge; _PARTIAL is set too, for
 *     WMs that read only one of the two),
 *   - _NET_WM_STATE_ABOVE stacks it with the other docks.
 *
 * Everything is set before the window is mapped (the extension
 * contract: create UNMAPPED, FDK maps at show()).
 */

#include "fdk_shell_internal.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdlib.h>
#include <string.h>

typedef struct x11_state x11_overlay;

/* ---- helpers -------------------------------------------------------------- */

static void set_atoms(Display *dpy, Window w, const char *name,
                      const unsigned char *data, int count, int format)
{
    Atom prop = XInternAtom(dpy, name, False);
    XChangeProperty(dpy, w, prop, XA_ATOM, format, PropModeReplace,
                    data, count);
}

/* The strut: reserve `size` logical px along the first anchored
 * edge (top > bottom > left > right precedence, matching how
 * single-edge struts are conventionally authored). */
static void set_strut(Display *dpy, Window w,
                      const fdk_shell_overlay_cfg *cfg)
{
    unsigned long strut[12] = {0};
    int size = cfg->exclusive > 0 ? cfg->exclusive
               : (cfg->size > 0 ? cfg->size : 32);
    if (cfg->anchor & FDK_SHELL_ANCHOR_TOP) {
        strut[2] = (unsigned long)(size + cfg->margin_top); /* top   */
        strut[9] = 1;  /* strut_start/end left/right are set below */
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_BOTTOM) {
        strut[3] = (unsigned long)(size + cfg->margin_bottom);
        strut[10] = 1;
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_LEFT) {
        strut[0] = (unsigned long)(size + cfg->margin_left);
        strut[4] = 1;
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_RIGHT) {
        strut[1] = (unsigned long)(size + cfg->margin_right);
        strut[6] = 1;
    }
    /* Full-width/height extent (start 1, end = screen extent):
     * the basic strut fields alone already reserve the edge for
     * every WM that implements either property. */
    Atom partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    Atom basic = XInternAtom(dpy, "_NET_WM_STRUT", False);
    XChangeProperty(dpy, w, partial,
                    XInternAtom(dpy, "CARDINAL", False), 32,
                    PropModeReplace, (const unsigned char *)strut, 12);
    XChangeProperty(dpy, w, basic,
                    XInternAtom(dpy, "CARDINAL", False), 32,
                    PropModeReplace, (const unsigned char *)strut, 4);
}

/* Screen-geometry placement for the anchored shape. */
static void place(const fdk_shell_overlay_cfg *cfg, Display *dpy,
                  int *x, int *y, unsigned *width, unsigned *height)
{
    int screen = DefaultScreen(dpy);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    int size = cfg->size > 0 ? cfg->size : 32;
    int horiz = (cfg->anchor & FDK_SHELL_ANCHOR_LEFT) &&
                (cfg->anchor & FDK_SHELL_ANCHOR_RIGHT);
    int vert = (cfg->anchor & FDK_SHELL_ANCHOR_TOP) &&
               (cfg->anchor & FDK_SHELL_ANCHOR_BOTTOM);
    int none = !(cfg->anchor & (FDK_SHELL_ANCHOR_TOP |
                                FDK_SHELL_ANCHOR_BOTTOM |
                                FDK_SHELL_ANCHOR_LEFT |
                                FDK_SHELL_ANCHOR_RIGHT));

    if (horiz && !vert) {                 /* horizontal bar       */
        *x = cfg->margin_left;
        *y = cfg->margin_top;
        *width = (unsigned)(sw - cfg->margin_left - cfg->margin_right);
        *height = (unsigned)size;
    } else if (vert && !horiz) {          /* vertical bar         */
        *x = cfg->margin_left;
        *y = cfg->margin_top;
        *width = (unsigned)size;
        *height = (unsigned)(sh - cfg->margin_top - cfg->margin_bottom);
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_TOP) {
        *x = cfg->margin_left;
        *y = cfg->margin_top;
        *width = (unsigned)(horiz ? sw : size);
        *height = (unsigned)(vert ? sh : size);
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_BOTTOM) {
        *x = cfg->margin_left;
        *y = sh - cfg->margin_bottom - size;
        *width = (unsigned)(horiz ? sw : size);
        *height = (unsigned)size;
    } else if (cfg->anchor & FDK_SHELL_ANCHOR_LEFT) {
        *x = cfg->margin_left;
        *y = cfg->margin_top;
        *width = (unsigned)size;
        *height = (unsigned)(vert ? sh : size);
    } else {                              /* right or floating    */
        *x = sw - cfg->margin_right - size;
        *y = cfg->margin_top;
        *width = (unsigned)size;
        *height = (unsigned)(vert ? sh : size);
    }
    if (none) {                           /* floating overlay     */
        *x = cfg->margin_left;
        *y = cfg->margin_top;
        *width = (unsigned)size;
        *height = (unsigned)size;
    }
}

/* ---- the kind ops --------------------------------------------------------- */

fdk_result fdk_shell_x11_create(fdk_ext_surface *surface,
                                fdk_ext_native_handle *out_native,
                                void *user)
{
    x11_overlay *ov = user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    fdk_shell_overlay_cfg *cfg = &ov->head.cfg;
    Display *dpy;
    int x, y;
    unsigned width, height;
    XSetWindowAttributes attrs;

    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)) ||
        nd.backend != FDK_EXT_BACKEND_X11)
        return FDK_ERR_NOT_INITIALIZED;
    dpy = (Display *)nd.display;

    ov->head.surface = surface;
    place(cfg, dpy, &x, &y, &width, &height);

    /* Override-redirect: the shell surface manages its own
     * geometry and stacking (no WM decorations, no WM moving). */
    attrs.override_redirect = True;
    attrs.background_pixel = BlackPixel(dpy, DefaultScreen(dpy));
    attrs.event_mask = 0; /* FDK installs its mask at adopt      */

    ov->window = XCreateWindow(dpy, DefaultRootWindow(dpy), x, y,
                               width, height, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel |
                                   CWEventMask,
                               &attrs);
    if (!ov->window)
        return FDK_ERR_WINDOW_CREATE;

    /* The dock identity + the exclusive zone, before any map. */
    Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, ov->window,
                    XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False),
                    XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&dock, 1);
    set_strut(dpy, ov->window, cfg);
    Atom above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    set_atoms(dpy, ov->window, "_NET_WM_STATE",
              (const unsigned char *)&above, 1, 32);
    {
        /* Class/res_name so WMs and pagers can identify the shell
         * (XSetClassHint takes mutable chars, not literals). */
        static char res_class[] = "fdk-shell";
        static char res_name[] = "fdk-shell";
        XClassHint hint = {res_name, res_class};
        XSetClassHint(dpy, ov->window, &hint);
    }

    out_native->backend = FDK_EXT_BACKEND_X11;
    out_native->xid = (fdk_u64)ov->window;
    return FDK_OK;
}

void fdk_shell_x11_destroy(fdk_ext_surface *surface, void *user)
{
    /* FDK destroys the window itself (the ownership contract);
     * the dock has no role objects to tear down beyond that. */
    (void)surface; (void)user;
}

void fdk_shell_x11_popup_attached(fdk_ext_surface *surface,
                                  void *native_popup, void *user)
{
    /* X11 parents popups through the window tree; never called. */
    (void)surface; (void)native_popup; (void)user;
}
