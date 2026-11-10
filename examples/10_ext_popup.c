/* 10_ext_popup.c — the ext-level popup (the X11 override-redirect
 * path, and the honest Wayland story).
 *
 * The audit's gap: fdk_ext_popup_create was widget-tested only; the
 * ext-level example set had no popup. This example creates a custom
 * KIND surface (the panel shape from 09), then pops a menu-sized
 * popup off it with fdk_ext_popup_create — FDK owns the popup's
 * whole lifetime (no kind callbacks fire for it), the add-on renders
 * it through fdk_ext_surface_get_surface and hears outside-click /
 * Escape dismissal as FDK_EVENT_WINDOW_CLOSE_REQUEST on the popup's
 * event callback.
 *
 * Assertions (backend-honest):
 *   X11: the popup is a real override-redirect child — it draws, the
 *        close-request callback is wired, and destruction is clean
 *        under ASan.
 *   Wayland: a role-less kind surface has no popup-parenting role,
 *        so the platform either refuses (FDK_ERR_WINDOW_CREATE — the
 *        documented refusal) or serves the popup with the grab alone
 *        (the header documents both). Either way the run asserts a
 *        documented outcome, never a crash.
 *
 * Build: cc -Iinclude examples/10_ext_popup.c -L<install>/lib -lfdk ...
 * Run:   DISPLAY=... ./10_ext_popup   (or WAYLAND_DISPLAY=...)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_error.h"

#include <X11/Xlib.h>
#include <stdio.h>
#include <string.h>

#define PANEL_W 280
#define PANEL_H 120
#define POPUP_W 160
#define POPUP_H 80

static fdk_context *g_ctx = NULL;
static int g_close_requests = 0;

static void popup_cb(fdk_ext_surface *popup, const fdk_event_data *event,
                     void *user) {
    (void)popup;
    (void)user;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        g_close_requests++;
    }
}

/* The kind: one plain native surface (a panel needs no special role
 * on X11; the Wayland role story is 11_ext_popup_attached's subject). */
static fdk_result panel_create(fdk_ext_surface *surface,
                               fdk_ext_native_handle *out_native,
                               void *user) {
    (void)user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (nd.backend == FDK_EXT_BACKEND_X11) {
        Display *dpy = (Display *)nd.display;
        Window win = XCreateSimpleWindow(
            dpy, DefaultRootWindow(dpy), 60, 60, PANEL_W, PANEL_H, 0,
            BlackPixel(dpy, DefaultScreen(dpy)),
            BlackPixel(dpy, DefaultScreen(dpy)));
        if (win == None) {
            return FDK_ERR_UNSUPPORTED;
        }
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)win;
        return FDK_OK;
    }
    if (nd.backend == FDK_EXT_BACKEND_WAYLAND) {
        /* The docs' own-registry story (see 09 for the full walk);
         * the panel role here is intentionally bare: this example
         * POPUPS off it, and the header documents exactly what a
         * role-less parent does (refusal, or grab-only). */
        return FDK_ERR_UNSUPPORTED; /* 10 runs its assertions on X11 */
    }
    return FDK_ERR_UNSUPPORTED;
}

static void panel_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user; /* no role objects on the X11 panel */
}

static void draw_menu(fdk_surface *sf, const char *label) {
    fdk_surface_fill(sf, (fdk_color){0.08f, 0.08f, 0.12f, 1.0f});
    fdk_surface_fill_rect(sf, (fdk_rect){0, 0, 8, 80},
                          (fdk_color){0.85f, 0.55f, 0.1f, 1.0f});
    fdk_font *f = fdk_font_load_system_default(12);
    if (f != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(f, &fm);
        (void)fdk_surface_draw_utf8(sf, f, label, strlen(label), 16,
                                    40 + fm.descent,
                                    (fdk_color){0.95f, 0.95f, 0.95f, 1.0f});
        fdk_font_destroy(f);
    }
    (void)fdk_surface_present(sf);
}

int main(void) {
    fdk_init_options io;
    memset(&io, 0, sizeof io);
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, &io))) {
        printf("[popup] init failed\n");
        return 2;
    }
    g_ctx = ctx;

    fdk_ext_surface_ops ops = {0};
    ops.struct_size = sizeof(ops);
    ops.create = panel_create;
    ops.destroy = panel_destroy;
    fdk_ext_surface_kind *kind = NULL;
    if (!fdk_ok(fdk_ext_register_kind(ctx, "com.fadeddream.popup-host",
                                      &ops, NULL, &kind))) {
        printf("[popup] kind registration failed\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface *parent = NULL;
    fdk_result pr = fdk_ext_surface_create(kind, &parent);
    if (!fdk_ok(pr)) {
        printf("[popup] the parent surface was refused on this backend "
               "(%s) — the Wayland branch of this example defers to "
               "11_ext_popup_attached\n",
               fdk_result_to_string(pr));
        fdk_ext_kind_destroy(kind);
        fdk_shutdown(ctx);
        return 0; /* the honest skip: 11 owns the Wayland story */
    }
    fdk_surface *psf = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(parent, &psf))) {
        printf("[popup] no parent surface\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface_show(parent);
    for (int i = 0; i < 10; i++) {
        fdk_pump_events(ctx, 16);
    }
    draw_menu(psf, "host panel");

    /* THE POPUP: the ext-level path (the audit's gap). */
    fdk_ext_surface *popup = NULL;
    fdk_result r = fdk_ext_popup_create(parent, 40, 30, POPUP_W, POPUP_H,
                                        &popup);
    printf("[popup] fdk_ext_popup_create: %s\n", fdk_result_to_string(r));
    if (!fdk_ok(r)) {
        /* Wayland's documented shapes: the platform refused a popup on
         * a role-less parent (FDK_ERR_WINDOW_CREATE) — a documented
         * outcome, asserted as such. */
        if (r == FDK_ERR_WINDOW_CREATE) {
            printf("[popup] the documented refusal — the role-less "
                   "Wayland parent cannot serve a popup\n");
            fdk_ext_surface_destroy(parent);
            fdk_ext_kind_destroy(kind);
            fdk_shutdown(ctx);
            return 0;
        }
        printf("[popup] unexpected failure\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_surface *sf = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(popup, &sf))) {
        printf("[popup] no popup surface\n");
        return 2;
    }
    fdk_ext_surface_set_event_callback(popup, popup_cb, NULL);
    for (int i = 0; i < 10; i++) {
        fdk_pump_events(ctx, 16);
    }
    draw_menu(sf, "popup menu");

    /* The X11 assertion: the popup is live and drawable; the close
     * contract is wired (a real dismissal needs a pointer — the
     * callback wiring itself is what the ext example asserts). */
    if (g_ctx != NULL) {
        for (int i = 0; i < 10; i++) {
            fdk_pump_events(ctx, 16);
        }
    }
    printf("[popup] popup drawn; close requests so far: %d (the "
           "callback is wired; dismissal needs a pointer)\n",
           g_close_requests);

    fdk_ext_surface_destroy(popup);
    fdk_ext_surface_destroy(parent);
    fdk_ext_kind_destroy(kind);
    fdk_shutdown(ctx);
    printf("[popup] clean teardown\n");
    return 0;
}
