/* panel-layer-shell.c — P5(a): a layer-shell panel built ONLY on
 * FDK's public API (this is the FDK layer-shell example, NOT the
 * separate fdk-shell-overlay project). Anchors a BOTTOM panel with an
 * exclusive zone, paints it, and exits after a few configured frames.
 * Requires a Wayland compositor with wlr-layer-shell (the sway rig).
 *
 * Build (installed FDK, no internal include paths):
 *   cc -I$PREFIX/include panel-layer-shell.c -L$PREFIX/lib -lfdk
 */
#include <fdk/fdk.h>
#include <fdk/fdk_extend.h>
#include <stdio.h>
#include <string.h>

static int g_frames = 0;

static void on_event(fdk_ext_surface *surface,
                     const fdk_event_data *event, void *user) {
    (void)surface;
    fdk_ext_surface *panel = (fdk_ext_surface *)user;
    if (event->type != FDK_EVENT_WINDOW_CONFIGURE) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(panel, &s))) {
        return;
    }
    fdk_size sz;
    if (!fdk_ok(fdk_ext_surface_get_size(panel, &sz))) {
        return;
    }
    fdk_surface_fill(s, (fdk_color){ 0.08f, 0.08f, 0.11f, 1.0f });
    fdk_surface_fill_rect(s, (fdk_rect){ 0, 0, sz.width, 2 },
                          (fdk_color){ 0.9f, 0.4f, 0.2f, 1.0f });
    fdk_surface_fill_rect(s, (fdk_rect){ 8, 8, 120, 20 },
                          (fdk_color){ 0.2f, 0.5f, 0.9f, 1.0f });
    fdk_surface_present(s);
    g_frames++;
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("panel: no context (need a Wayland session)\n");
        return 77;
    }
    fdk_ext_layer_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.layer = FDK_LAYER_BOTTOM;
    opts.anchor = FDK_LAYER_ANCHOR_BOTTOM;
    opts.exclusive_zone = 36;
    opts.namespace_ = "fdk-example-panel";
    fdk_ext_surface *panel = NULL;
    fdk_result r = fdk_ext_layer_surface_create(ctx, &opts, &panel);
    if (!fdk_ok(r)) {
        printf("panel: layer surface refused (%d) — is the compositor "
               "wlr-layer-shell capable?\n",
               (int)r);
        fdk_shutdown(ctx);
        return 77;
    }
    fdk_result ar = fdk_ext_layer_surface_set_anchor(
        panel, FDK_LAYER_ANCHOR_BOTTOM);
    printf("panel: set_anchor -> %d\n", (int)ar);
    fdk_result zr = fdk_ext_layer_surface_set_exclusive_zone(panel, 36);
    printf("panel: set_exclusive_zone -> %d\n", (int)zr);
    /* client-chosen height; the width follows the LEFT|RIGHT anchors */
    fdk_result sr = fdk_ext_layer_surface_set_size(panel, 0, 36);
    printf("panel: set_size -> %d\n", (int)sr);
    fdk_ext_surface_set_event_callback(panel, on_event, panel);
    printf("panel: surface created\n");

    for (int i = 0; i < 90 && g_frames < 3; i++) {
        fdk_pump_events(ctx, 16);
    }
    printf("panel: %d configured frame(s) painted\n", g_frames);
    fdk_ext_surface_destroy(panel);
    fdk_shutdown(ctx);
    return g_frames > 0 ? 0 : 1;
}
