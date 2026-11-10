/* 11_ext_popup_attached.c — the kind hook that adopts Wayland popups.
 *
 * The audit's second gap: the popup_attached hook's Wayland
 * popup-parenting path was covered by the widget tests, but no
 * ext-kind example exercised it. This one registers a kind WITH the
 * hook, pops a popup off the kind surface, and asserts the hook's
 * documented behavior:
 *
 *   - Wayland: FDK creates the xdg_popup and hands the proxy to the
 *     hook — "native_popup is the struct xdg_popup* — cast it in the
 *     add-on, which is the only place wayland-client types may
 *     appear". The hook here does exactly that (records the pointer;
 *     a real role with its own get_popup request would adopt it
 *     there), and the example asserts the hook FIRED.
 *   - X11: the hook is never called ("X parents popups through the X
 *     window tree") — asserted too, as the documented negative.
 *
 * A role-less Wayland parent may also be refused outright
 * (FDK_ERR_WINDOW_CREATE) — a documented outcome, asserted as such.
 *
 * Build: cc -Iinclude examples/11_ext_popup_attached.c -lfdk ...
 * Run:   DISPLAY=... ./11...   (or WAYLAND_DISPLAY=...)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_error.h"

#include <X11/Xlib.h>
#include <wayland-client.h> /* the hook's cast + the own-registry walk */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The registry listener (a file-scope fn, the docs' shape). */
static void registry_global(void *data, struct wl_registry *reg,
                            uint32_t name, const char *iface,
                            uint32_t version) {
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        struct wl_compositor **out = data;
        *out = (struct wl_compositor *)wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    }
}

static int g_hook_fired = 0;
static void *g_hook_popup = NULL;

/* The documented adopt point: the ONLY place wayland-client types
 * may appear in an add-on. This example records the proxy; a real
 * layer-shell panel would call its role's get_popup here. */
static void panel_popup_attached(fdk_ext_surface *surface,
                                 void *native_popup, void *user) {
    (void)surface;
    (void)user;
    g_hook_fired++;
    g_hook_popup = native_popup;
}

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
            dpy, DefaultRootWindow(dpy), 100, 100, 240, 100, 0,
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
        /* The own-registry walk (the docs' shape; 09 carries the
         * annotated version). This parent is deliberately role-less:
         * the hook contract covers exactly what happens then —
         * the popup either serves with the grab alone or the
         * platform refuses. */
        struct wl_display *dpy = (struct wl_display *)nd.display;
        struct wl_registry *reg = wl_display_get_registry(dpy);
        if (reg == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        struct wl_compositor *comp = NULL;
        struct wl_registry_listener rl = {0};
        rl.global = registry_global;
        wl_registry_add_listener(reg, &rl, &comp);
        wl_display_roundtrip(dpy);
        wl_display_roundtrip(dpy);
        wl_registry_destroy(reg);
        if (comp == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        struct wl_surface *surf = wl_compositor_create_surface(comp);
        wl_compositor_destroy(comp); /* the surface keeps its own ref */
        if (surf == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        out_native->backend = FDK_EXT_BACKEND_WAYLAND;
        out_native->pointer = surf;
        return FDK_OK;
    }
    return FDK_ERR_UNSUPPORTED;
}

static void panel_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user;
}

int main(void) {
    fdk_init_options io;
    memset(&io, 0, sizeof io);
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, &io))) {
        printf("[attached] init failed\n");
        return 2;
    }
    fdk_ext_surface_ops ops = {0};
    ops.struct_size = sizeof(ops);
    ops.create = panel_create;
    ops.destroy = panel_destroy;
    ops.popup_attached = panel_popup_attached;
    fdk_ext_surface_kind *kind = NULL;
    if (!fdk_ok(fdk_ext_register_kind(ctx, "com.fadeddream.popup-attached",
                                      &ops, NULL, &kind))) {
        printf("[attached] kind registration failed\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface *parent = NULL;
    if (!fdk_ok(fdk_ext_surface_create(kind, &parent))) {
        printf("[attached] parent refused\n");
        fdk_ext_kind_destroy(kind);
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_ext_surface_show(parent);
    for (int i = 0; i < 10; i++) {
        fdk_pump_events(ctx, 16);
    }

    fdk_ext_surface *popup = NULL;
    fdk_result r = fdk_ext_popup_create(parent, 24, 24, 120, 60, &popup);
    printf("[attached] fdk_ext_popup_create: %s\n",
           fdk_result_to_string(r));
    if (fdk_ok(r)) {
        fdk_surface *sf = NULL;
        (void)fdk_ext_surface_get_surface(popup, &sf);
        fdk_surface_fill(sf, (fdk_color){0.1f, 0.3f, 0.15f, 1.0f});
        (void)fdk_surface_present(sf);
        for (int i = 0; i < 10; i++) {
            fdk_pump_events(ctx, 16);
        }
        const char *backend = getenv("WAYLAND_DISPLAY") != NULL
                                  ? "wayland"
                                  : "x11";
        if (strcmp(backend, "wayland") == 0) {
            /* THE assertion: the hook adopted the popup proxy. */
            if (g_hook_fired == 1 && g_hook_popup != NULL) {
                printf("[attached] PASS: popup_attached fired once and "
                       "handed over the xdg_popup proxy (%p)\n",
                       g_hook_popup);
            } else {
                printf("[attached] FAIL: the hook did not adopt the "
                       "popup (fired=%d, proxy=%p)\n",
                       g_hook_fired, g_hook_popup);
                return 2;
            }
        } else {
            /* The documented negative: X11 never calls the hook. */
            if (g_hook_fired == 0 && g_hook_popup == NULL) {
                printf("[attached] PASS: X11 popup served through the "
                       "window tree and the hook correctly never fired\n");
            } else {
                printf("[attached] FAIL: the hook fired on X11 (the "
                       "docs say it never does)\n");
                return 2;
            }
        }
        fdk_ext_surface_destroy(popup);
    } else if (r == FDK_ERR_WINDOW_CREATE) {
        /* The documented Wayland refusal for a role-less parent. */
        printf("[attached] PASS: the documented refusal (role-less "
               "Wayland parent) — the hook path needs a parenting "
               "role in production\n");
    } else {
        printf("[attached] unexpected failure: %s\n",
               fdk_result_to_string(r));
        return 2;
    }

    fdk_ext_surface_destroy(parent);
    fdk_ext_kind_destroy(kind);
    fdk_shutdown(ctx);
    printf("[attached] clean teardown\n");
    return 0;
}
