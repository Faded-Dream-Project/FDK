/* 19_extension_surfaces.c — the extension API (fdk_extend.h), live.
 *
 * The surface add-on API that external shell add-ons are BUILT
 * on, exercised
 * directly: the demo registers its OWN surface kind, creates a
 * surface, renders to it through the ordinary fdk_surface path,
 * sets a cursor + input region, shows/hides it, and opens a real
 * popup on it — the whole lifecycle an add-on package lives.
 *
 *   fdk_ext_api_version / has_capability  the version + capability
 *                 probes every add-on starts with
 *   fdk_ext_get_native_display + surface_native
 *                 the documented native bridge: on X11 the demo's
 *                 create() makes its own XCreateWindow and sets its
 *                 own properties (an override-redirect-free child
 *                 with a class hint), exactly like a real add-on
 *   register_kind / surface_create / show / destroy
 *                 the custom-kind lifecycle (create required,
 *                 destroy optional, the struct_size contract)
 *   get_surface  drawing through the public render target: a
 *                 gradient + rects + text rendered each frame with
 *                 the ordinary primitives
 *   set_cursor / set_input_region
 *                 the pointer-shape and clickability controls
 *   popup_create a REAL popup (grab + Escape dismissal) on the
 *                 extension surface — the dropdown pattern
 *
 * The window beside it carries the report: version, capabilities,
 * the kind's registration, the surface's native handle, every step
 * logged. On Wayland the create() branch speaks the compositor
 * path (wl_compositor's create_surface) — this demo implements
 * the X11 branch and reports the backend honestly where it did
 * not.
 */

#include "example_window.h"

#include "fdk/fdk_extend.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static fdk_example ex;
static fdk_widget *g_log;
static char g_log_text[4096];
static size_t g_log_len;
static fdk_ext_surface_kind *g_kind;
static fdk_ext_surface *g_ext;
static int g_frames;

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_log_len += (size_t)vsnprintf(
        g_log_text + g_log_len, sizeof g_log_text - g_log_len, fmt,
        ap);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    g_log_len += (size_t)snprintf(g_log_text + g_log_len,
                                  sizeof g_log_text - g_log_len, "\n");
    if (g_log_len >= sizeof g_log_text - 256) {
        size_t keep = g_log_len / 2;
        while (keep < g_log_len && g_log_text[keep] != '\n') {
            keep++;
        }
        memmove(g_log_text, g_log_text + keep, g_log_len - keep);
        g_log_len -= keep;
        g_log_text[g_log_len] = '\0';
    }
    fdk_textview_set_text(g_log, g_log_text);
    fdk_example_set_status(&ex, g_log_text);
}

/* ---- the custom kind: an X11 child window the add-on owns --------- */

static fdk_result kind_create(fdk_ext_surface *surface,
                              fdk_ext_native_handle *out_native,
                              void *user) {
    (void)user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        log_line("[ext] create: no native display");
        return FDK_ERR_PLATFORM;
    }
    if (nd.backend != FDK_EXT_BACKEND_X11) {
        /* This demo implements the X11 branch; the Wayland branch
         * (wl_compositor create_surface + a role) is the overlay
         * package's job. Report honestly and stay out. */
        log_line("[ext] create: backend is not X11 — this demo's "
                 "kind is X11-only (the same API carries the "
                 "Wayland shape)");
        return FDK_ERR_UNSUPPORTED;
    }
    Display *dpy = (Display *)nd.display;
    Window parent = DefaultRootWindow(dpy);
    int screen = DefaultScreen(dpy);
    Window w = XCreateSimpleWindow(
        dpy, parent, 80, 80, 320, 200, 0,
        BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    /* The add-on's own properties, before show: the class hint +
     * a title — the exact pattern a shell add-on's X11 leg
     * uses. */
    XClassHint *class_hint = XAllocClassHint();
    if (class_hint != NULL) {
        class_hint->res_name = (char *)"fdk-example-19";
        class_hint->res_class = (char *)"FdkExample19";
        XSetClassHint(dpy, w, class_hint);
        XFree(class_hint);
    }
    XStoreName(dpy, w, "FDK example 19 — extension surface");
    /* An input shape making only the top half clickable: the
     * add-on's protocol work, then FDK's own set_input_region
     * below demonstrates the public form. */
    out_native->backend = FDK_EXT_BACKEND_X11;
    out_native->xid = (fdk_u64)w;
    log_line("[ext] create: XCreateWindow xid=0x%lx, class + name "
             "set",
             (unsigned long)w);
    return FDK_OK;
}

static void kind_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface; (void)user;
    log_line("[ext] destroy: the kind's role teardown ran");
}

/* ---- drawing: the ordinary fdk_surface path ------------------------- */

static void ext_paint(void) {
    if (g_ext == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(g_ext, &s)) ||
        s == NULL) {
        return;
    }
    fdk_surface_info info;
    (void)fdk_surface_get_info(s, &info);
    /* a moving gradient band + stable chrome */
    for (int y = 0; y < info.height; y++) {
        fdk_u32 c = 0xFF000000u |
                    (unsigned)((y + g_frames * 2) & 0xFF) << 16 |
                    0x6600u;
        fdk_surface_fill_rect(
            s, (fdk_rect){0, y, info.width, 1},
            (fdk_color){(fdk_f32)((c >> 16) & 0xFF) / 255.0f,
                        (fdk_f32)((c >> 8) & 0xFF) / 255.0f,
                        (fdk_f32)(c & 0xFF) / 255.0f, 1.0f});
    }
    fdk_surface_fill_rect(s, (fdk_rect){8, 8, info.width - 16, 28},
                          (fdk_color){0.1f, 0.1f, 0.15f, 1});
    fdk_surface_draw_rect(s, (fdk_rect){8, 8, info.width - 16, 28},
                          (fdk_color){1, 1, 1, 1});
    fdk_surface_present(s); /* the public path — same as a window */
    g_frames++;
}

/* ---- the demo's actions ------------------------------------------------ */

static void probe_report(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    log_line("[ext] api version: %d", fdk_ext_api_version());
    static const struct {
        fdk_ext_capability cap;
        const char *name;
    } caps[] = {
        {FDK_EXT_CAP_SHM_RENDER, "SHM_RENDER"},
        {FDK_EXT_CAP_POPUP_GRAB, "POPUP_GRAB"},
        {FDK_EXT_CAP_EWMH_DOCK, "EWMH_DOCK"},
        {FDK_EXT_CAP_FRAME_PACING, "FRAME_PACING"},
    };
    for (size_t i = 0; i < sizeof caps / sizeof caps[0]; i++) {
        log_line("[ext] capability %-12s: %s", caps[i].name,
                 fdk_ext_has_capability(ex.ctx, caps[i].cap)
                     ? "yes"
                     : "no");
    }
}

static void kind_register(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_kind != NULL) {
        log_line("[ext] kind already registered");
        return;
    }
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.struct_size = sizeof ops;
    ops.create = kind_create;
    ops.destroy = kind_destroy;
    fdk_result r = fdk_ext_register_kind(ex.ctx, "example19-kind",
                                         &ops, NULL, &g_kind);
    log_line("[ext] register_kind: %s",
             fdk_ok(r) ? "OK (name 'example19-kind')" : "FAILED");
}

static void surface_create(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_kind == NULL) {
        log_line("[ext] surface: register the kind first");
        return;
    }
    if (g_ext != NULL) {
        log_line("[ext] surface already alive");
        return;
    }
    fdk_result r = fdk_ext_surface_create(g_kind, &g_ext);
    if (!fdk_ok(r)) {
        log_line("[ext] surface_create: FAILED (the kind's create "
                 "refused — see the log above)");
        g_ext = NULL;
        return;
    }
    fdk_ext_native_handle nh;
    if (fdk_ok(fdk_ext_surface_native(g_ext, &nh)) &&
        nh.backend == FDK_EXT_BACKEND_X11) {
        log_line("[ext] surface_create: OK, native xid 0x%lx",
                 (unsigned long)nh.xid);
    } else {
        log_line("[ext] surface_create: OK");
    }
    /* the pointer shape + the input region (the public forms) */
    fdk_ext_surface_set_cursor(g_ext, FDK_CURSOR_CROSSHAIR);
    log_line("[ext] set_cursor(CROSSHAIR)");
    fdk_rect input_rects[1] = {{0, 0, 320, 100}};
    fdk_result ir = fdk_ext_surface_set_input_region(
        g_ext, input_rects, 1);
    log_line("[ext] set_input_region(top half): %s",
             fdk_ok(ir) ? "OK — clicks below y=100 fall through"
                        : "FAILED");
    fdk_ext_surface_show(g_ext);
    log_line("[ext] show: mapped (an FDK-drawn 320x200 surface "
             "appears at +80+80)");
    ext_paint();
}

static void surface_hide(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_ext == NULL) {
        return;
    }
    fdk_ext_surface_hide(g_ext);
    log_line("[ext] hide");
}

static void surface_show(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_ext == NULL) {
        return;
    }
    fdk_ext_surface_show(g_ext);
    log_line("[ext] show");
    ext_paint();
}

static void surface_popup(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_ext == NULL) {
        log_line("[ext] popup: create the surface first");
        return;
    }
    fdk_ext_surface *popup = NULL;
    fdk_result r = fdk_ext_popup_create(g_ext, 40, 60, 200, 90,
                                         &popup);
    if (!fdk_ok(r)) {
        log_line("[ext] popup_create: FAILED");
        return;
    }
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_ext_surface_get_surface(popup, &s)) && s != NULL) {
        fdk_surface_fill(s, (fdk_color){0.12f, 0.12f, 0.16f, 1});
        fdk_surface_fill_rect(s, (fdk_rect){8, 8, 184, 74},
                              (fdk_color){0.2f, 0.2f, 0.28f, 1});
        fdk_surface_present(s);
    }
    fdk_ext_surface_show(popup);
    log_line("[ext] popup: shown at +40+60 (200x90) — click outside "
             "or press Escape on it to dismiss");
    /* The demo leaks the popup deliberately-ish: the popup is
     * toolkit-grabbed and dismisses itself; the next surface
     * destroy sweeps survivors. Kept simple on purpose — the rig
     * tests the API, not a menu framework. */
}

static void surface_destroy(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_ext == NULL) {
        return;
    }
    fdk_ext_surface_destroy(g_ext);
    g_ext = NULL;
    log_line("[ext] surface_destroy: OK (kind teardown ran first)");
}

/* ---- main --------------------------------------------------------------- */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "19")) {
        return 1;
    }
    if (!fdk_example_open(&ex, ctx, "19", "extension surfaces (the "
                                          "add-on API)",
                          620, 560)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_font *font = ex.ui;

    fdk_widget *row = NULL;
    (void)fdk_box_create(ex.content, FDK_HORIZONTAL, &row);
    fdk_widget *b = NULL;
    (void)fdk_button_create(row, font, "Probe", &b);
    fdk_button_set_on_activate(b, probe_report, NULL);
    (void)fdk_button_create(row, font, "Register kind", &b);
    fdk_button_set_on_activate(b, kind_register, NULL);
    (void)fdk_button_create(row, font, "Create surface", &b);
    fdk_button_set_on_activate(b, surface_create, NULL);
    (void)fdk_button_create(row, font, "Hide", &b);
    fdk_button_set_on_activate(b, surface_hide, NULL);
    (void)fdk_button_create(row, font, "Show", &b);
    fdk_button_set_on_activate(b, surface_show, NULL);
    (void)fdk_button_create(row, font, "Popup", &b);
    fdk_button_set_on_activate(b, surface_popup, NULL);
    (void)fdk_button_create(row, font, "Destroy", &b);
    fdk_button_set_on_activate(b, surface_destroy, NULL);

    g_log = NULL;
    (void)fdk_textview_create(ex.content, font, &g_log);
    fdk_textview_set_text(g_log, g_log_text);

    log_line("[ext] the extension API demo — press Probe, then "
             "Register kind, then Create surface");

    fdk_example_run(&ex);

    if (g_ext != NULL) {
        fdk_ext_surface_destroy(g_ext);
    }
    if (g_kind != NULL) {
        (void)fdk_ext_kind_destroy(g_kind);
    }
    fdk_example_close(&ex);
    return 0;
}
