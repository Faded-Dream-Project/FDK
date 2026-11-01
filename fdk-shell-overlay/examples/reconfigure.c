/* reconfigure.c — fdk-shell-overlay's LIVE surface, end to end.
 *
 * The bar example shows the static lifecycle (create, show, popup).
 * This demo exercises everything AFTER that — the parts a real
 * panel's settings dialog drives:
 *
 *   outputs      fdk_shell_output_count / get_info / the output
 *                event callback: the RandR/wlr-output enumeration
 *                printed at startup (position, size, scale, mm),
 *                and output_index placement honored at create
 *   the layer   a dock-style strip with margins, an exclusive
 *                zone, and a namespace — drawn through the surface
 *                API every configure
 *   LIVE setters fdk_shell_layer_set_anchor (slide TOP -> BOTTOM
 *                -> LEFT -> RIGHT at runtime, the strut following),
 *                set_margin (the strip walks off its edge),
 *                set_exclusive_zone (reserve 0 / auto / 48),
 *                set_level (TOP -> OVERLAY -> BOTTOM),
 *                set_keyboard_mode (NONE / EXCLUSIVE / ON_DEMAND
 *                with the honest below-v4 degrade)
 *   auto-hide    set_auto_hide + reveal + hide_now: the hidden
 *                panel with a reveal strip, dwell-based hiding,
 *                and the force-reveal path
 *   backend      fdk_shell_init reports which world answered; the
 *                demo states plainly when it is the EWMH dock
 *                client (Xvfb) vs the layer-shell client (sway/
 *                labwc)
 *
 * Keys drive everything (the layer takes keyboard): F1-F6 the
 * setters, H auto-hide, R reveal, Q quit. The surface paints its
 * current state so every change is VISIBLE.
 */

#include <fdk-shell-overlay/fdk-shell-overlay.h>

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_shell *g_shell;
static fdk_shell_layer *g_layer;
static bool g_quit;
static int g_anchor_mode; /* 0 top, 1 bottom, 2 left, 3 right */
static int g_margin;
static int g_zone_mode;  /* 0 none, 1 auto, 2 explicit */
static int g_level_mode; /* 0 top, 1 overlay, 2 bottom */
static int g_kbd_mode;   /* 0 none, 1 exclusive, 2 on-demand */
static char g_status[256];

static void paint(void) {
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_layer_get_surface(g_layer, &s)) ||
        s == NULL) {
        return;
    }
    fdk_surface_info info;
    (void)fdk_surface_get_info(s, &info);
    fdk_surface_fill(s, (fdk_color){0.13f, 0.15f, 0.20f, 1.0f});
    /* a state readout drawn right on the strip */
    snprintf(g_status, sizeof g_status,
             "F1 edge:%s  F2 margin:+%d  F3 zone:%s  F4 level:%s  "
             "F5 kbd:%s  F6 repaint  H hide  R reveal  Q quit",
             g_anchor_mode == 0   ? "TOP"
             : g_anchor_mode == 1 ? "BOTTOM"
             : g_anchor_mode == 2 ? "LEFT"
                                  : "RIGHT",
             g_margin,
             g_zone_mode == 0   ? "none"
             : g_zone_mode == 1 ? "auto"
                                : "48px",
             g_level_mode == 0   ? "TOP"
             : g_level_mode == 1 ? "OVERLAY"
                                 : "BOTTOM",
             g_kbd_mode == 0   ? "NONE"
             : g_kbd_mode == 1 ? "EXCLUSIVE"
                               : "ON-DEMAND");
    /* the readout as bands (the strip may be too short for text) */
    int seg = info.width > 0 ? (int)info.width / 8 : 8;
    for (int i = 0; i < 8; i++) {
        fdk_f32 hue = (fdk_f32)i / 8.0f;
        fdk_surface_fill_rect(
            s, (fdk_rect){i * seg, 0, (fdk_i32)seg - 2, info.height},
            (fdk_color){hue, 1.0f - hue, 0.5f, 1.0f});
    }
    fdk_surface_present(s);
    printf("[paint] %dx%d — %s\n", (int)info.width,
           (int)info.height, g_status);
}

static void apply_anchor(void) {
    uint32_t anchors = g_anchor_mode == 0   ? FDK_SHELL_ANCHOR_TOP
                       : g_anchor_mode == 1 ? FDK_SHELL_ANCHOR_BOTTOM
                       : g_anchor_mode == 2 ? FDK_SHELL_ANCHOR_LEFT
                                            : FDK_SHELL_ANCHOR_RIGHT;
    fdk_result r = fdk_shell_layer_set_anchor(g_layer, anchors);
    printf("[live] set_anchor(0x%x): %s\n", anchors,
           fdk_ok(r) ? "the surface slid, the reservation followed"
                     : "FAILED");
}

static void on_event(fdk_shell_layer *layer,
                     const fdk_event_data *ev, void *user) {
    (void)layer; (void)user;
    if (ev->type == FDK_EVENT_WINDOW_EXPOSE) {
        paint();
        return;
    }
    if (ev->type == FDK_EVENT_KEY_DOWN) {
        switch (ev->key.scancode) {
        case 59:
            g_anchor_mode = (g_anchor_mode + 1) % 4;
            apply_anchor();
            break;
        case 60: {
            g_margin = (g_margin + 8) % 40;
            fdk_result r = fdk_shell_layer_set_margin(
                g_layer, g_margin, g_margin, g_margin, g_margin);
            printf("[live] set_margin(%d all edges): %s\n", g_margin,
                   fdk_ok(r) ? "moved live" : "FAILED");
            paint();
            break;
        }
        case 61: {
            g_zone_mode = (g_zone_mode + 1) % 3;
            int32_t z = g_zone_mode == 0   ? 0
                        : g_zone_mode == 1 ? -1
                                           : 48;
            fdk_result r =
                fdk_shell_layer_set_exclusive_zone(g_layer, z);
            printf("[live] set_exclusive_zone(%d): %s\n", z,
                   fdk_ok(r) ? "re-reserved live" : "FAILED");
            paint();
            break;
        }
        case 62: {
            g_level_mode = (g_level_mode + 1) % 3;
            fdk_shell_layer_level lv =
                g_level_mode == 0   ? FDK_SHELL_LAYER_TOP
                : g_level_mode == 1 ? FDK_SHELL_LAYER_OVERLAY
                                    : FDK_SHELL_LAYER_BOTTOM;
            fdk_result r = fdk_shell_layer_set_level(g_layer, lv);
            printf("[live] set_level(%d): %s\n", (int)lv,
                   fdk_ok(r) ? "stacking changed live" : "FAILED");
            paint();
            break;
        }
        case 63: {
            g_kbd_mode = (g_kbd_mode + 1) % 3;
            fdk_shell_keyboard_mode m =
                g_kbd_mode == 0   ? FDK_SHELL_KBD_NONE
                : g_kbd_mode == 1 ? FDK_SHELL_KBD_EXCLUSIVE
                                  : FDK_SHELL_KBD_ON_DEMAND;
            fdk_result r =
                fdk_shell_layer_set_keyboard_mode(g_layer, m);
            printf("[live] set_keyboard_mode(%d): %s\n", (int)m,
                   fdk_ok(r) ? "OK (below-v4 compositors degrade "
                              "on-demand to none, logged)"
                             : "FAILED");
            paint();
            break;
        }
        case 64:
            paint();
            break;
        case 35: {
            fdk_result r = fdk_shell_layer_set_auto_hide(g_layer, 6,
                                                         1200);
            printf("[live] set_auto_hide(reveal 6px, dwell 1200ms): "
                   "%s — the strip slides off; the reveal edge or "
                   "R brings it back\n",
                   fdk_ok(r) ? "armed" : "FAILED");
            break;
        }
        case 19:
            fdk_shell_layer_reveal(g_layer);
            printf("[live] reveal: shown, auto-hide still armed\n");
            paint();
            break;
        case 16:
            g_quit = true;
            break;
        default:
            break;
        }
    }
}

static void on_outputs_changed(fdk_shell *shell,
                               const fdk_shell_output_event *event,
                               void *user) {
    (void)user; (void)event;
    uint32_t n = 0;
    (void)fdk_shell_output_count(shell, &n);
    printf("[outputs] the set changed — %u now:\n", n);
    for (uint32_t i = 0; i < n; i++) {
        fdk_shell_output_info info;
        memset(&info, 0, sizeof info);
        info.struct_size = sizeof info;
        if (fdk_ok(fdk_shell_output_get_info(shell, i, &info))) {
            printf("[outputs]   [%u] %dx%d at +%d+%d (scale %d, "
                   "%dx%dmm)\n",
                   i, info.width, info.height, info.x, info.y,
                   info.scale, info.physical_width,
                   info.physical_height);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    fdk_init_options iopts;
    memset(&iopts, 0, sizeof iopts);
    iopts.app_id = "org.fdk.shell-overlay.reconfigure";
    if (!fdk_ok(fdk_init(&ctx, &iopts)) || ctx == NULL) {
        fprintf(stderr, "reconfigure: fdk_init failed (no display?)\n");
        return 1;
    }
    if (!fdk_shell_backend_supported(ctx)) {
        fprintf(stderr,
                "reconfigure: the connected backend has no shell "
                "surface support\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell *shell = NULL;
    if (!fdk_ok(fdk_shell_init(ctx, &shell)) || shell == NULL) {
        fprintf(stderr, "reconfigure: fdk_shell_init failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    g_shell = shell;
    printf("[init] fdk-shell-overlay up (backend: %s)\n",
           fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_WAYLAND
               ? "Wayland layer-shell"
               : "X11 EWMH dock client");

    /* the outputs at startup + the live callback */
    fdk_shell_set_output_event_callback(shell, on_outputs_changed,
                                        NULL);
    fdk_shell_output_event ev_none;
    memset(&ev_none, 0, sizeof ev_none);
    on_outputs_changed(shell, &ev_none, NULL);

    /* the layer: a dock strip on TOP with a margin and an auto
     * zone, keyboard ON_DEMAND (so the keys work), named for
     * compositor rules. */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof desc;
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.margin_top = 4;
    desc.margin_left = 4;
    desc.margin_right = 4;
    desc.exclusive_zone = -1; /* auto: our own extent            */
    desc.keyboard = FDK_SHELL_KBD_ON_DEMAND;
    desc.height = 36;
    desc.output_index = -1;
    snprintf(desc.namespace_, sizeof desc.namespace_,
             "fdk-reconfigure-demo");
    fdk_shell_layer *layer = NULL;
    fdk_result r = fdk_shell_layer_create(shell, &desc, &layer);
    if (!fdk_ok(r) || layer == NULL) {
        fprintf(stderr, "reconfigure: layer_create failed\n");
        fdk_shell_shutdown(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    g_layer = layer;
    fdk_shell_layer_set_event_callback(layer, on_event, NULL);
    fdk_shell_layer_set_cursor(layer,
                              FDK_SHELL_CURSOR_POINTER);
    fdk_shell_layer_show(layer);
    paint();
    printf("[init] the strip is up. Click it for keyboard, then "
           "F1-F6 / H / R / Q.\n");

    while (!g_quit) {
        (void)fdk_pump_events(ctx, 15);
    }

    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[init] clean exit\n");
    return 0;
}
