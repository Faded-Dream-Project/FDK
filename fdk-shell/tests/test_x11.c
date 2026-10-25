/* test_x11.c — fdk-shell's X11 backend tests (the EWMH half).
 *
 * Requires a reachable X11 display (make test starts a throwaway
 * Xvfb). Verifies against the SERVER, through a second connection,
 * exactly what the backend promises: the dock window type, the
 * _NET_WM_STRUT_PARTIAL reservation math (auto and explicit zones,
 * margins, every anchored edge), the anchor geometry (stretch,
 * corner, centered), keyboard EXCLUSIVE as a real XGrabKeyboard,
 * rendering through FDK's extension surface path, and popups on a
 * layer (positioning, dismissal, teardown).
 *
 * The test speaks the PUBLIC fdk-shell + FDK APIs; Xlib here is the
 * verification channel (reading back properties and geometry an FDK
 * cache cannot satisfy), the same role it plays in FDK's own
 * integration suite.
 */

#include <fdk-shell/fdk-shell.h>

/* Test harness only: the internals are the verification channel
 * (reading the ext-surface handles the assertions need) — the same
 * license FDK's own integration suite grants itself. The LIBRARY
 * never crosses its boundary; the test does, to prove things. */
#include "../src/shell_internal.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h> /* XGetImage / XGetPixel / XDestroyImage */
#include <X11/Xatom.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* nanosleep — the polling waits */
#include <unistd.h>

/* ---- verification helpers (second connection) ---------------------------- */

static Display *g_rb = NULL;

static void rb_open(void) {
    if (g_rb == NULL) {
        g_rb = XOpenDisplay(NULL);
        assert(g_rb != NULL);
    }
}

/* The native XID through the PUBLIC extension accessor (the ext
 * handle itself through the internal struct — verification only). */
static unsigned long layer_xid(fdk_shell_layer *layer) {
    fdk_ext_native_handle nh;
    assert(fdk_ok(fdk_ext_surface_native(layer->ext, &nh)));
    assert(nh.backend == FDK_EXT_BACKEND_X11);
    return (unsigned long)nh.xid;
}

static int prop_cardinals(Display *dpy, Window w, Atom prop, long *out,
                           int max_n) {
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, prop, 0, (long)max_n, False,
                           XA_CARDINAL, &actual, &fmt, &n, &after,
                           &data) != Success ||
        data == NULL) {
        return 0;
    }
    int count = (int)(n < (unsigned long)max_n ? n : (unsigned long)max_n);
    for (int i = 0; i < count; i++) {
        out[i] = (long)((unsigned long *)data)[i];
    }
    XFree(data);
    return count;
}

static int window_is_dock_typed(Display *dpy, Window w) {
    Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, type_atom, 0, 4, False, XA_ATOM,
                           &actual, &fmt, &n, &after, &data) != Success ||
        data == NULL) {
        return 0;
    }
    int ok = (n >= 1 && ((Atom *)data)[0] == dock);
    XFree(data);
    return ok;
}

static void geom_of(Display *dpy, Window w, int *x, int *y,
                    unsigned int *width, unsigned int *height) {
    Window root = None, parent = None;
    Window *kids = NULL;
    unsigned int nk = 0;
    assert(XGetGeometry(dpy, w, &root, x, y, width, height, &nk, &nk) != 0);
    (void)parent;
    (void)kids;
    (void)nk;
}

/* ---- shared event/pump plumbing ------------------------------------------- */

/* POSIX-2008 sleep (usleep is obsolescent and undeclared under the
 * strict feature set the package builds with). */
static void sleep_ms(int ms) {
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

static void pump(fdk_context *ctx, int ms) {
    int steps = ms / 50;
    for (int i = 0; i < steps; i++) {
        (void)fdk_pump_events(ctx, 50);
    }
}

/* ---- a minimal EWMH window manager (the desktop-switch rig) ---------------
 *
 * The sticky/ghost contracts need a WM: without one, top-level windows
 * map directly and nothing ever switches desktops. This rig implements
 * exactly the EWMH subset those contracts exercise — no more:
 *
 *   - root SubstructureRedirect ownership (MapRequest routing);
 *   - _NET_SUPPORTING_WM_CHECK + _NET_SUPPORTED advertising;
 *   - desktop tracking (_NET_NUMBER_OF_DESKTOPS = 2,
 *     _NET_CURRENT_DESKTOP), switched VIA THE ROOT CLIENT MESSAGE
 *     (the pager shape: switch = send _NET_CURRENT_DESKTOP to root);
 *   - per-window _NET_WM_DESKTOP (0xFFFFFFFF = sticky = never
 *     unmapped by a switch; others unmap when their desktop is not
 *     current and remap when it becomes current again);
 *   - _NET_WM_STATE / _NET_WM_DESKTOP client-message requests honored
 *     on mapped windows (the ADD shape rewrites the property).
 *
 * Deliberate realism choice: on MapRequest the WM DELETES the
 * window's _NET_WM_STATE property before mapping — the shape of a WM
 * that starts late or ignores pre-map state. The post-map client
 * messages (fdk-shell's belt-and-braces leg) must then be the thing
 * that restores STICKY/ABOVE, which is exactly what the assertions
 * check.
 */
typedef struct {
    Display *dpy;
    Window root;
    Window check_win; /* _NET_SUPPORTING_WM_CHECK       */
    int current_desktop;
    struct {
        Window w;
        unsigned long desktop;
    } clients[64];
    int n_clients;
    struct {
        Atom net_supported, net_supporting_wm_check;
        Atom net_number_of_desktops, net_current_desktop;
        Atom net_wm_desktop, net_wm_state;
        Atom net_wm_state_sticky, net_wm_state_above;
        Atom net_wm_state_skip_taskbar, net_wm_state_skip_pager;
    } a;
} test_wm;

static int wm_ignore_errors(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0; /* a dying client's stale window is not the rig's problem */
}

static unsigned long wm_window_desktop(test_wm *wm, Window w) {
    long v = 0;
    if (prop_cardinals(wm->dpy, w, wm->a.net_wm_desktop, &v, 1) != 1) {
        return 0; /* EWMH default: the first desktop */
    }
    /* Xlib sign-extends 32-bit CARDINALs into the long it hands
     * back (0xFFFFFFFF reads as -1): mask to the wire's true 32-bit
     * value before any comparison. */
    return ((unsigned long)v) & 0xFFFFFFFFUL;
}

static int wm_client_find(test_wm *wm, Window w) {
    for (int i = 0; i < wm->n_clients; i++) {
        if (wm->clients[i].w == w) {
            return i;
        }
    }
    return -1;
}

static void wm_set_root_cardinal(test_wm *wm, Atom prop, long value) {
    XChangeProperty(wm->dpy, wm->root, prop, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *)&value, 1);
}

static void wm_switch(test_wm *wm, int desk) {
    for (int i = 0; i < wm->n_clients; i++) {
        Window w = wm->clients[i].w;
        unsigned long d = wm->clients[i].desktop;
        if (d == 0xFFFFFFFFUL) {
            continue; /* sticky: the whole point — never touched */
        }
        XWindowAttributes wa;
        if (XGetWindowAttributes(wm->dpy, w, &wa) == 0) {
            continue; /* died */
        }
        if (d == (unsigned long)desk && wa.map_state != IsViewable) {
            XMapWindow(wm->dpy, w); /* its desktop became current */
        } else if (d != (unsigned long)desk && wa.map_state == IsViewable) {
            XUnmapWindow(wm->dpy, w); /* left for another desktop */
        }
    }
    wm->current_desktop = desk;
    wm_set_root_cardinal(wm, wm->a.net_current_desktop, desk);
    XSync(wm->dpy, False);
}

static void wm_state_add(test_wm *wm, Window w, Atom a1, Atom a2) {
    /* The EWMH ADD request on a mapped window: merge into the
     * property (idempotent). */
    Atom have[16];
    int n_have = 0;
    {
        Atom actual = None;
        int fmt = 0;
        unsigned long n = 0, after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(wm->dpy, w, wm->a.net_wm_state, 0, 16, False,
                               XA_ATOM, &actual, &fmt, &n, &after,
                               &data) == Success &&
            data != NULL) {
            for (unsigned long i = 0; i < n && n_have < 16; i++) {
                have[n_have++] = ((Atom *)data)[i];
            }
            XFree(data);
        }
    }
    Atom merged[20];
    int n_merged = 0;
    for (int i = 0; i < n_have; i++) {
        merged[n_merged++] = have[i];
    }
    const Atom add[2] = { a1, a2 };
    for (int k = 0; k < 2; k++) {
        if (add[k] == None) {
            continue;
        }
        int present = 0;
        for (int i = 0; i < n_merged; i++) {
            if (merged[i] == add[k]) {
                present = 1;
            }
        }
        if (!present && n_merged < 20) {
            merged[n_merged++] = add[k];
        }
    }
    if (n_merged > 0) {
        XChangeProperty(wm->dpy, w, wm->a.net_wm_state, XA_ATOM, 32,
                        PropModeReplace,
                        (const unsigned char *)merged, n_merged);
    }
}

static void wm_pump(test_wm *wm) {
    while (XPending(wm->dpy) > 0) {
        XEvent ev;
        XNextEvent(wm->dpy, &ev);
        switch (ev.type) {
            case MapRequest: {
                Window w = ev.xmaprequest.window;
                unsigned long desk = wm_window_desktop(wm, w);
                int idx = wm_client_find(wm, w);
                if (idx < 0 && wm->n_clients < 64) {
                    idx = wm->n_clients++;
                    wm->clients[idx].w = w;
                }
                if (idx >= 0) {
                    wm->clients[idx].desktop = desk;
                }
                /* The "WM that missed the pre-map state" shape: the
                 * property leg is simulated away — only the post-map
                 * client messages can restore the window's state. */
                XDeleteProperty(wm->dpy, w, wm->a.net_wm_state);
                if (desk == 0xFFFFFFFFUL ||
                    desk == (unsigned long)wm->current_desktop) {
                    XMapWindow(wm->dpy, w);
                    XRaiseWindow(wm->dpy, w);
                }
                break;
            }
            case DestroyNotify: {
                int idx = wm_client_find(wm, ev.xdestroywindow.window);
                if (idx >= 0) {
                    wm->clients[idx] = wm->clients[--wm->n_clients];
                }
                break;
            }
            case ClientMessage: {
                if (ev.xclient.message_type == wm->a.net_current_desktop &&
                    ev.xclient.window == wm->root) {
                    wm_switch(wm, (int)ev.xclient.data.l[0]);
                } else if (ev.xclient.message_type == wm->a.net_wm_desktop) {
                    Window w = ev.xclient.window;
                    /* Same 32-bit masking: the wire value is what EWMH
                     * defines (0xFFFFFFFF = all desktops), regardless
                     * of Xlib's sign-extension of data.l[]. */
                    unsigned long d =
                        ((unsigned long)ev.xclient.data.l[0]) & 0xFFFFFFFFUL;
                    int idx = wm_client_find(wm, w);
                    if (idx >= 0) {
                        wm->clients[idx].desktop = d;
                        XWindowAttributes wa;
                        if (XGetWindowAttributes(wm->dpy, w, &wa) != 0) {
                            if (d == 0xFFFFFFFFUL ||
                                d == (unsigned long)wm->current_desktop) {
                                if (wa.map_state != IsViewable) {
                                    XMapWindow(wm->dpy, w);
                                }
                            } else if (wa.map_state == IsViewable) {
                                XUnmapWindow(wm->dpy, w);
                            }
                        }
                    }
                } else if (ev.xclient.message_type == wm->a.net_wm_state &&
                           ev.xclient.data.l[0] == 1 /* _NET_WM_STATE_ADD */) {
                    wm_state_add(wm, ev.xclient.window,
                                 (Atom)ev.xclient.data.l[1],
                                 (Atom)ev.xclient.data.l[2]);
                }
                break;
            }
            default:
                break; /* MapNotify/UnmapNotify/... — not needed */
        }
    }
    XFlush(wm->dpy);
}

static void wm_start(test_wm *wm, Display *dpy) {
    memset(wm, 0, sizeof(*wm));
    wm->dpy = dpy;
    wm->root = DefaultRootWindow(dpy);
    XSetErrorHandler(wm_ignore_errors);

    wm->a.net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
    wm->a.net_supporting_wm_check =
        XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    wm->a.net_number_of_desktops =
        XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    wm->a.net_current_desktop =
        XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    wm->a.net_wm_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    wm->a.net_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    wm->a.net_wm_state_sticky =
        XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    wm->a.net_wm_state_above =
        XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    wm->a.net_wm_state_skip_taskbar =
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    wm->a.net_wm_state_skip_pager =
        XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);

    /* Own the root: every later top-level MapRequest routes here. */
    XSelectInput(dpy, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);

    /* EWMH identity + the capability list (exactly what the rig
     * honors — an honest _NET_SUPPORTED, not a blanket one). */
    wm->check_win = XCreateSimpleWindow(dpy, wm->root, -10, -10, 1, 1, 0, 0,
                                        0);
    {
        const Atom caps[8] = {
            wm->a.net_supported,   /* self-listing is conventional */
            wm->a.net_current_desktop, wm->a.net_number_of_desktops,
            wm->a.net_wm_desktop,  wm->a.net_wm_state,
            wm->a.net_wm_state_sticky, wm->a.net_wm_state_above,
            wm->a.net_wm_state_skip_taskbar
        };
        XChangeProperty(dpy, wm->root, wm->a.net_supported, XA_ATOM, 32,
                        PropModeReplace, (const unsigned char *)caps, 8);
    }
    wm_set_root_cardinal(wm, wm->a.net_supporting_wm_check,
                         (long)wm->check_win);
    wm_set_root_cardinal(wm, wm->a.net_number_of_desktops, 2);
    wm_set_root_cardinal(wm, wm->a.net_current_desktop, 0);
    XSync(dpy, False);
}

static void wm_stop(test_wm *wm) {
    /* Closing the display releases the substructure redirect. */
    XSync(wm->dpy, False);
    XCloseDisplay(wm->dpy);
}

/* ---- pixel/property helpers over the readback connection ------------------ */

static unsigned long win_pixel(Display *dpy, Window w, int x, int y) {
    XImage *img = XGetImage(dpy, (Drawable)w, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(img != NULL);
    unsigned long p = (unsigned long)XGetPixel(img, 0, 0) & 0xFFFFFFUL;
    XDestroyImage(img);
    return p;
}

static int window_has_state_atom(Display *dpy, Window w, Atom state) {
    Atom actual = None;
    int fmt = 0;
    unsigned long n = 0, after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, XInternAtom(dpy, "_NET_WM_STATE", False),
                           0, 32, False, XA_ATOM, &actual, &fmt, &n, &after,
                           &data) != Success ||
        data == NULL) {
        return 0;
    }
    int found = 0;
    for (unsigned long i = 0; i < n; i++) {
        if (((Atom *)data)[i] == state) {
            found = 1;
        }
    }
    XFree(data);
    return found;
}

/* ---- the tests ---------------------------------------------------------------- */

static void test_top_bar_auto_zone(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));

    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    /* X11's honest single output. */
    uint32_t outputs = 0;
    assert(fdk_ok(fdk_shell_output_count(shell, &outputs)));
    assert(outputs == 1);
    char name[64];
    assert(fdk_ok(fdk_shell_output_name(shell, 0, name, sizeof(name))));
    assert(strcmp(name, "screen-0") == 0);

    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    desc.exclusive_zone = -1; /* auto: the bar's own extent */

    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_show(layer);
    pump(ctx, 200);

    rb_open();
    Display *dpy = g_rb;
    unsigned long xid = layer_xid(layer);
    int screen_w = DisplayWidth(dpy, DefaultScreen(dpy));

    /* Geometry: full width at the very top, 32 tall. */
    int x = 0, y = 0;
    unsigned int w = 0, h = 0;
    geom_of(dpy, (Window)xid, &x, &y, &w, &h);
    assert(x == 0 && y == 0);
    assert(w == (unsigned int)screen_w);
    assert(h == 32);

    /* The dock type. */
    assert(window_is_dock_typed(dpy, (Window)xid));

    /* Struts: auto zone = 32 reserved at the top, full width. */
    long strut[12] = { 0 };
    Atom struts_partial =
        XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    assert(prop_cardinals(dpy, (Window)xid, struts_partial, strut, 12) ==
           12);
    assert(strut[0] == 0 && strut[1] == 0); /* left, right  */
    assert(strut[2] == 32);                 /* top          */
    assert(strut[3] == 0);                  /* bottom       */
    assert(strut[8] == 0);                  /* top_start_x  */
    assert(strut[9] == screen_w - 1);       /* top_end_x    */

    /* The legacy strut mirrors it. */
    long simple[4] = { 0 };
    Atom struts_atom = XInternAtom(dpy, "_NET_WM_STRUT", False);
    assert(prop_cardinals(dpy, (Window)xid, struts_atom, simple, 4) == 4);
    assert(simple[2] == 32 && simple[0] == 0 && simple[1] == 0 &&
           simple[3] == 0);

    /* Render through the public fdk_surface path; server-side pixel
     * proof at the bar's center. */
    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_shell_layer_get_surface(layer, &surface)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    assert(info.width == screen_w && info.height == 32);
    fdk_surface_fill(surface, (fdk_color){ .r = 0.2f, .g = 0.6f, .b = 0.9f,
                                           .a = 1.0f });
    assert(fdk_ok(fdk_surface_present(surface)));
    pump(ctx, 200);

    XImage *img = XGetImage(dpy, (Window)xid, screen_w / 2 - 8, 8, 16, 8,
                            AllPlanes, ZPixmap);
    assert(img != NULL);
    unsigned long px = (unsigned long)XGetPixel(img, 8, 4);
    unsigned long rgb = px & 0xFFFFFF;
    int blue = ((rgb)&0xFF) > 180 && ((rgb >> 16) & 0xFF) < 100;
    assert(blue);
    XDestroyImage(img);

    /* Size query follows the server's configure. */
    fdk_size sz;
    assert(fdk_ok(fdk_shell_layer_get_size(layer, &sz)));
    assert(sz.width == screen_w && sz.height == 32);

    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: top bar auto zone (dock type, "
           "strut math, geometry, render + readback, size)\n");
}

static void test_margins_and_bottom(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    int screen_w = 0, screen_h = 0;
    {
        rb_open();
        screen_w = DisplayWidth(g_rb, DefaultScreen(g_rb));
        screen_h = DisplayHeight(g_rb, DefaultScreen(g_rb));
    }

    /* A bottom panel with margins: geometry + the strut's bottom
     * extent (margin + height). */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 40;
    desc.margin_bottom = 6;
    desc.exclusive_zone = -1;

    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_show(layer);
    pump(ctx, 150);

    Display *dpy = g_rb;
    unsigned long xid = layer_xid(layer);
    int x = 0, y = 0;
    unsigned int w = 0, h = 0;
    geom_of(dpy, (Window)xid, &x, &y, &w, &h);
    assert(x == 0);
    assert(y == screen_h - 6 - 40);
    assert(w == (unsigned int)screen_w);
    assert(h == 40);

    long strut[12] = { 0 };
    Atom struts_partial =
        XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    assert(prop_cardinals(dpy, (Window)xid, struts_partial, strut, 12) ==
           12);
    assert(strut[3] == 46); /* bottom: height + margin */
    assert(strut[10] == 0 && strut[11] == screen_w - 1);

    /* An EXPLICIT zone overrides the extent. */
    fdk_shell_layer *layer2 = NULL;
    desc.exclusive_zone = 100;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer2)));
    fdk_shell_layer_show(layer2);
    pump(ctx, 150);
    unsigned long xid2 = layer_xid(layer2);
    long strut2[12] = { 0 };
    assert(prop_cardinals(dpy, (Window)xid2, struts_partial, strut2,
                          12) == 12);
    assert(strut2[3] == 100);

    fdk_shell_layer_destroy(layer2);
    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: bottom panel margins + explicit zone\n");
}

static void test_centered_anchor(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    /* TOP-only anchoring: 300 wide, horizontally CENTERED (the
     * protocol's "centered with respect to its anchors"). */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_TOP;
    desc.width = 300;
    desc.height = 24;

    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_show(layer);
    pump(ctx, 150);

    rb_open();
    int screen_w = DisplayWidth(g_rb, DefaultScreen(g_rb));
    unsigned long xid = layer_xid(layer);
    int x = 0, y = 0;
    unsigned int w = 0, h = 0;
    geom_of(g_rb, (Window)xid, &x, &y, &w, &h);
    assert(x == (screen_w - 300) / 2);
    assert(y == 0);
    assert(w == 300 && h == 24);

    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: unanchored axis centers\n");
}

static void test_keyboard_exclusive_grab(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_OVERLAY;
    desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 10;
    desc.keyboard = FDK_SHELL_KBD_EXCLUSIVE;

    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_show(layer);
    pump(ctx, 200);

    /* A SECOND client's keyboard grab must fail while the layer
     * holds it (the server-side proof an EXCLUSIVE grab exists). */
    rb_open();
    Window root = DefaultRootWindow(g_rb);
    int foreign = XGrabKeyboard(g_rb, root, False, GrabModeAsync,
                                GrabModeAsync, CurrentTime);
    XUngrabKeyboard(g_rb, CurrentTime);
    XFlush(g_rb);
    assert(foreign == AlreadyGrabbed);

    /* Destroying the layer releases it. */
    fdk_shell_layer_destroy(layer);
    pump(ctx, 150);
    foreign = XGrabKeyboard(g_rb, root, False, GrabModeAsync,
                            GrabModeAsync, CurrentTime);
    XUngrabKeyboard(g_rb, CurrentTime);
    XFlush(g_rb);
    assert(foreign == GrabSuccess);

    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: EXCLUSIVE keyboard is a real "
           "server-side grab, released at destroy\n");
}

static int g_popup_closes = 0;
static void popup_event(fdk_shell_popup *popup, const fdk_event_data *ev,
                        void *user) {
    (void)popup;
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        g_popup_closes++;
    }
}

static void test_popup_on_layer(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_show(layer);
    pump(ctx, 150);

    /* The dropdown: parented at layer-local (20, 32) — right under
     * the bar's left corner. */
    g_popup_closes = 0;
    fdk_shell_popup *popup = NULL;
    assert(fdk_ok(fdk_shell_popup_create(layer, 20, 32, 120, 90, &popup)));
    fdk_shell_popup_set_event_callback(popup, popup_event, NULL);
    fdk_shell_popup_show(popup);
    pump(ctx, 200);

    rb_open();
    Display *dpy = g_rb;
    unsigned long layer_x = layer_xid(layer);

    /* The popup's native handle + geometry: positioned at the layer
     * origin + (20, 32), override-redirect, mapped. */
    fdk_ext_native_handle nh;
    assert(fdk_ok(fdk_ext_surface_native(popup->ext, &nh)));
    unsigned long pop_xid = (unsigned long)nh.xid;
    int px = 0, py = 0;
    unsigned int pw = 0, ph = 0;
    geom_of(dpy, (Window)pop_xid, &px, &py, &pw, &ph);
    Window child_ret = None;
    int lx = 0, ly = 0;
    assert(XTranslateCoordinates(dpy, (Window)layer_x,
                                 DefaultRootWindow(dpy), 0, 0, &lx, &ly,
                                 &child_ret));
    assert(px == lx + 20 && py == ly + 32);
    assert(pw == 120 && ph == 90);
    XWindowAttributes pattrs;
    assert(XGetWindowAttributes(dpy, (Window)pop_xid, &pattrs) != 0);
    assert(pattrs.override_redirect != 0);
    assert(pattrs.map_state == IsViewable);

    /* Render it through the shell's popup surface. */
    fdk_surface *ps = NULL;
    assert(fdk_ok(fdk_shell_popup_get_surface(popup, &ps)));
    fdk_surface_fill(ps, (fdk_color){ .r = 1.0f, .g = 0.8f, .b = 0.1f,
                                      .a = 1.0f });
    assert(fdk_ok(fdk_surface_present(ps)));
    pump(ctx, 200);
    XImage *img = XGetImage(dpy, (Window)pop_xid, 50, 40, 20, 10,
                            AllPlanes, ZPixmap);
    assert(img != NULL);
    unsigned long rgb = (unsigned long)XGetPixel(img, 10, 5) & 0xFFFFFF;
    assert(((rgb >> 16) & 0xFF) > 200);
    XDestroyImage(img);

    /* Outside click: out-of-bounds press (the grab shape) delivers
     * the close request. */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ButtonPress;
    ev.xbutton.window = (Window)pop_xid;
    ev.xbutton.x = -60;
    ev.xbutton.y = -60;
    ev.xbutton.button = 1;
    ev.xbutton.same_screen = True;
    assert(XSendEvent(dpy, (Window)pop_xid, False,
                      (long)(ButtonPressMask | ButtonReleaseMask), &ev));
    XFlush(dpy);
    pump(ctx, 200);
    assert(g_popup_closes == 1);

    /* Destroying the LAYER sweeps the popup with it (the documented
     * contract, enforced by the shell's own ordering). */
    fdk_shell_layer_destroy(layer);
    pump(ctx, 150);
    {
        Window root2, parent2;
        Window *kids = NULL;
        unsigned int nk = 0;
        assert(XQueryTree(dpy, DefaultRootWindow(dpy), &root2, &parent2,
                          &kids, &nk) != 0);
        int found = 0;
        for (unsigned int i = 0; i < nk; i++) {
            if (kids[i] == (Window)pop_xid) {
                found = 1;
            }
        }
        if (kids != NULL) {
            XFree(kids);
        }
        assert(!found);
    }

    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: popup on layer (position, render, "
           "outside-click dismissal, parent sweep)\n");
}

/* The layer-level stacking state, as SERVER-VISIBLE properties:
 * every layer is sticky (_NET_WM_DESKTOP = 0xFFFFFFFF "all desktops"
 * pre-map + _NET_WM_STATE_STICKY), TOP/OVERLAY carry
 * _NET_WM_STATE_ABOVE, and OVERLAY adds SKIP_TASKBAR/SKIP_PAGER —
 * the strongest EWMH stacking behavior. BACKGROUND/BOTTOM get no
 * ABOVE (EWMH cannot stack below normal windows; the documented
 * delta). */
static void test_layer_stack_state(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));
    rb_open();
    Display *dpy = g_rb;
    Atom a_desktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    Atom a_sticky = XInternAtom(dpy, "_NET_WM_STATE_STICKY", False);
    Atom a_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom a_skip_tb = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom a_skip_pg = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);

    const fdk_shell_layer_level levels[4] = {
        FDK_SHELL_LAYER_BACKGROUND, FDK_SHELL_LAYER_BOTTOM,
        FDK_SHELL_LAYER_TOP, FDK_SHELL_LAYER_OVERLAY
    };
    for (int i = 0; i < 4; i++) {
        fdk_shell_layer_desc desc;
        memset(&desc, 0, sizeof(desc));
        desc.struct_size = sizeof(desc);
        desc.layer = levels[i];
        desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT;
        desc.width = 100;
        desc.height = 10;

        fdk_shell_layer *layer = NULL;
        assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));

        /* BEFORE any show: the pre-map properties are the EWMH
         * initial-state contract — a WM reads them at map time.
         * (The 32-bit mask: Xlib sign-extends CARDINALs on readback;
         * the wire value is what a WM sees.) */
        unsigned long xid = layer_xid(layer);
        long v = 0;
        assert(prop_cardinals(dpy, (Window)xid, a_desktop, &v, 1) == 1);
        assert((((unsigned long)v) & 0xFFFFFFFFUL) == 0xFFFFFFFFUL);
        assert(window_has_state_atom(dpy, (Window)xid, a_sticky));

        int want_above = (levels[i] == FDK_SHELL_LAYER_TOP ||
                          levels[i] == FDK_SHELL_LAYER_OVERLAY);
        assert(window_has_state_atom(dpy, (Window)xid, a_above) ==
               want_above);
        if (levels[i] == FDK_SHELL_LAYER_OVERLAY) {
            assert(window_has_state_atom(dpy, (Window)xid, a_skip_tb));
            assert(window_has_state_atom(dpy, (Window)xid, a_skip_pg));
        }

        fdk_shell_layer_destroy(layer);
    }

    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell x11: layer stack state (sticky desktop + "
           "per-level ABOVE/SKIP_* properties, pre-map)\n");
}

/* The test's "application": repaints the layer on EXPOSE/CONFIGURE —
 * the demo bar's exact shape (and fdk_event.h's documented contract
 * for rendered surfaces). Counts repainting so the assertions can
 * prove the Expose actually drove one. */
static fdk_color g_ghost_color;
static int g_ghost_repaints = 0;

static void ghost_layer_event(fdk_shell_layer *layer,
                              const fdk_event_data *ev, void *user) {
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_EXPOSE ||
        ev->type == FDK_EVENT_WINDOW_CONFIGURE) {
        fdk_surface *s = NULL;
        if (fdk_ok(fdk_shell_layer_get_surface(layer, &s))) {
            fdk_surface_fill(s, g_ghost_color);
            (void)fdk_surface_present(s);
            g_ghost_repaints++;
        }
    }
}

/* The pager-shaped desktop switch: a _NET_CURRENT_DESKTOP client
 * message to the root (exactly how a panel's workspace switcher asks
 * a WM to switch), then the WM's own processing of it. */
static void wm_switch_via_message(Display *client_dpy, test_wm *wm,
                                  int desk) {
    XEvent msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = ClientMessage;
    msg.xclient.window = wm->root;
    msg.xclient.message_type = wm->a.net_current_desktop;
    msg.xclient.format = 32;
    msg.xclient.data.l[0] = desk;
    XSendEvent(client_dpy, wm->root, False,
               (long)SubstructureNotifyMask | SubstructureRedirectMask,
               &msg);
    /* XSync, not XFlush: the round trip guarantees the SERVER has
     * processed (and dispatched to the WM's socket) the SendEvent
     * before the WM below asks its own connection for events — a
     * flush alone races the server's delivery against the WM's read
     * (found live: the second switch's message arrived after the
     * pump had already looked). */
    XSync(client_dpy, False);
    wm_pump(wm);
}

/* THE desktop-switch contract, against the in-test EWMH WM:
 *
 *   1. STICKY: switching _NET_CURRENT_DESKTOP never unmaps the layer
 *      (it stays IsViewable through the whole round trip) while a
 *      non-sticky window on another desktop IS unmapped/remapped by
 *      the same switches (the control that proves the WM actually
 *      switches).
 *   2. THE GHOST FIX's mechanism: on the real desktop this was filed
 *      from (X11, no sticky support in the bar yet), switching away
 *      UNMAPPED the bar; the WM repainted the region with the other
 *      workspace's content, and the remapped bar (background None
 *      after the first acquire — the server never clears it again)
 *      showed that stale content in its own region: "a copy of
 *      whatever was on screen underneath, but only where the bar
 *      should be". Two independent fixes close it: sticky (an EWMH
 *      WM never unmaps the layer across switches — assertion 1) and
 *      the EXPOSE repaint (whatever content IS lost — a desktop
 *      switch on a WM without sticky support, a fullscreen window
 *      closing over the bar — the layer's event callback repaints,
 *      the demo bar's exact shape). The rig proves the second leg
 *      end-to-end: the switch-back's unocclusion EXPOSE arrives at
 *      the layer, the callback repaints + presents (counted), and
 *      the region reads the bar's own color again. (Xvfb, unlike a
 *      bare non-composited Xorg, also restores retained content
 *      server-side — so the LITERAL stale pixels are a real-desktop
 *      artifact; what is pinned here is the fix's mechanism, which
 *      is the only restorer on servers that don't.)
 *   3. The post-map CLIENT MESSAGES: the WM (which deletes
 *      _NET_WM_STATE at map time — the late-WM shape) must end with
 *      STICKY/ABOVE back on the window: only the _NET_WM_STATE ADD
 *      messages fdk-shell sends after mapping can have restored them.
 *      (The WM runs from BEFORE the layer exists, so the map — and
 *      the deletion — genuinely route through it.)
 */
static void test_sticky_across_desktops(void) {
    const fdk_color BAR_TEAL = { .r = 0.10f, .g = 0.80f, .b = 0.75f,
                                 .a = 1.0f };
    /* fdk_surface_fill packs XRGB8888 exactly this way (the same
     * channel math the FDK suite's byte-exact readbacks pin). */
    const unsigned long TEAL = 0x001ACCBFUL;
    const unsigned long RED = 0x00FF0000UL;

    rb_open();
    Display *rb = g_rb;
    int screen_w = DisplayWidth(rb, DefaultScreen(rb));

    /* The WM FIRST — every later map routes through it. */
    Display *wm_dpy = XOpenDisplay(NULL);
    assert(wm_dpy != NULL);
    test_wm wm;
    wm_start(&wm, wm_dpy);

    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(fdk_init(&ctx, &iopts)));
    fdk_shell *shell = NULL;
    assert(fdk_ok(fdk_shell_init(ctx, &shell)));

    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors =
        FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT | FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    desc.exclusive_zone = -1;

    g_ghost_color = BAR_TEAL;
    g_ghost_repaints = 0;

    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_set_event_callback(layer, ghost_layer_event, NULL);
    fdk_shell_layer_show(layer);
    /* The map request + post-map client messages travel on FDK's
     * connection; the WM sees them asynchronously. Bounded polling
     * until the bar is viewable THROUGH the WM (the map happened)
     * — then one more round for the state messages to land. */
    for (int i = 0; i < 50; i++) {
        wm_pump(&wm);
        XWindowAttributes wa;
        if (XGetWindowAttributes(rb, (Window)layer_xid(layer), &wa) != 0 &&
            wa.map_state == IsViewable) {
            break;
        }
        sleep_ms(10);
    }
    pump(ctx, 150); /* the first-map Expose repaints through the callback */

    unsigned long xid = layer_xid(layer);
    XWindowAttributes wa;
    assert(XGetWindowAttributes(rb, (Window)xid, &wa) != 0);
    assert(wa.map_state == IsViewable); /* the WM mapped the sticky window */

    /* Paint the bar TEAL (deterministic, even though the callback's
     * first-map repaint already did) and verify server-side. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_shell_layer_get_surface(layer, &s)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    fdk_surface_fill(s, BAR_TEAL);
    assert(fdk_ok(fdk_surface_present(s)));
    pump(ctx, 150);
    const int probe_x = screen_w / 2, probe_y = 16;
    assert(win_pixel(rb, (Window)xid, probe_x, probe_y) == TEAL);

    /* The decoy: a RED window that lives on desktop 1 (the "other
     * workspace"), created through the readback connection like any
     * application window, tall enough to cover the bar's region. */
    Window decoy = XCreateSimpleWindow(rb, DefaultRootWindow(rb), 0, 0,
                                       (unsigned)screen_w, 200, 0, RED, RED);
    long one = 1;
    XChangeProperty(rb, decoy, XInternAtom(rb, "_NET_WM_DESKTOP", False),
                    XA_CARDINAL, 32, PropModeReplace,
                    (const unsigned char *)&one, 1);
    XMapWindow(rb, decoy);
    /* XSync before the WM looks: the round trip guarantees the server
     * processed (and dispatched to the WM) the MapRequest. */
    XSync(rb, False);
    wm_pump(&wm); /* the WM sees the MapRequest: desktop 1 is not
                     current — the decoy stays unmapped */
    assert(XGetWindowAttributes(rb, decoy, &wa) != 0);
    assert(wa.map_state == IsUnmapped); /* control: the WM's desktop
                                           model works */

    /* Switch TO desktop 1 (the pager shape) — the decoy maps and
     * covers the bar's region (the "other workspace" content over
     * the bar). */
    wm_switch_via_message(rb, &wm, 1);
    pump(ctx, 100);
    assert(XGetWindowAttributes(rb, decoy, &wa) != 0);
    assert(wa.map_state == IsViewable); /* control */
    assert(XGetWindowAttributes(rb, (Window)xid, &wa) != 0);
    assert(wa.map_state == IsViewable); /* STICKY: never unmapped, even
                                           while covered */

    /* Switch BACK to desktop 0: the decoy unmaps (the bar's region
     * loses its cover), the bar receives EXPOSE, the layer callback
     * repaints + presents, and the region reads the bar's own color
     * again — on a server that does not restore retained content
     * (the real desktop this was filed from), this repaint is the
     * ONLY restorer: the ghost fix. */
    int repaints_before = g_ghost_repaints;
    wm_switch_via_message(rb, &wm, 0);
    pump(ctx, 200); /* the Expose drives the repaint */
    assert(XGetWindowAttributes(rb, (Window)xid, &wa) != 0);
    assert(wa.map_state == IsViewable); /* still never unmapped */
    assert(XGetWindowAttributes(rb, decoy, &wa) != 0);
    assert(wa.map_state == IsUnmapped); /* control: the switch-back
                                           really happened */
    assert(g_ghost_repaints > repaints_before); /* EXPOSE arrived and
                                                   the callback
                                                   repainted */
    assert(win_pixel(rb, (Window)xid, probe_x, probe_y) == TEAL);

    /* The post-map client messages restored the state the WM deleted
     * at map time: STICKY and ABOVE are back on the window — only the
     * _NET_WM_STATE ADD messages can have done that. (Bounded poll:
     * the messages travel on FDK's connection, asynchronously.) */
    {
        Atom a_sticky = XInternAtom(rb, "_NET_WM_STATE_STICKY", False);
        Atom a_above = XInternAtom(rb, "_NET_WM_STATE_ABOVE", False);
        int got = 0;
        for (int i = 0; i < 50 && !got; i++) {
            wm_pump(&wm);
            got = window_has_state_atom(rb, (Window)xid, a_sticky) &&
                  window_has_state_atom(rb, (Window)xid, a_above);
            if (!got) {
                sleep_ms(10);
            }
        }
        assert(got);
    }

    XDestroyWindow(rb, decoy);
    XFlush(rb);
    wm_pump(&wm);

    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    wm_stop(&wm);
    printf("[ok] fdk-shell x11: sticky across desktops (never unmapped, "
           "control window switches) + ghost-fix mechanism (expose "
           "arrives, the layer repaints, pixels correct after the "
           "round-trip) + post-map EWMH state messages\n");
}

int main(void) {
    test_top_bar_auto_zone();
    test_margins_and_bottom();
    test_centered_anchor();
    test_keyboard_exclusive_grab();
    test_popup_on_layer();
    test_layer_stack_state();
    test_sticky_across_desktops();
    if (g_rb != NULL) {
        XCloseDisplay(g_rb);
    }
    printf("all fdk-shell X11 tests passed\n");
    return 0;
}

