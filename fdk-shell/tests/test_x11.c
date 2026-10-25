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

static void pump(fdk_context *ctx, int ms) {
    int steps = ms / 50;
    for (int i = 0; i < steps; i++) {
        (void)fdk_pump_events(ctx, 50);
    }
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

int main(void) {
    test_top_bar_auto_zone();
    test_margins_and_bottom();
    test_centered_anchor();
    test_keyboard_exclusive_grab();
    test_popup_on_layer();
    if (g_rb != NULL) {
        XCloseDisplay(g_rb);
    }
    printf("all fdk-shell X11 tests passed\n");
    return 0;
}

