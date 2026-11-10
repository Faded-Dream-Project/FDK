/* gl-canvas.c — P5(b): a GL canvas in GPU mode, public API only. The
 * window runs the GPU render mode (the GL lane); the frame paints
 * through the fdk_surface path and presents — proving an application
 * can drive the GL lane end-to-end without touching FDK internals.
 * Exits 77 with the lane's reason if no GL is available.
 */
#include <fdk/fdk.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("gl-canvas: no context\n");
        return 77;
    }
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.render_mode = FDK_RENDER_GPU;
    o.title = "fdk-gl-canvas";
    o.width = 480;
    o.height = 300;
    fdk_window *w = NULL;
    fdk_result r = fdk_window_create(ctx, &o, &w);
    if (!fdk_ok(r)) {
        printf("gl-canvas: WALL — GPU window refused (%d)\n", (int)r);
        fdk_shutdown(ctx);
        return 77;
    }
    fdk_window_show(w);
    for (int f = 0; f < 5; f++) {
        fdk_pump_events(ctx, 16);
        fdk_surface *s = NULL;
        fdk_window_get_surface(w, &s);
        fdk_surface_fill(s, (fdk_color){ 0.10f, 0.12f, 0.16f, 1.0f });
        for (int i = 0; i < 6; i++) {
            fdk_color c = { 0.2f + 0.13f * (float)i, 0.5f,
                            0.9f - 0.12f * (float)i, 1.0f };
            fdk_surface_fill_rect(
                s, (fdk_rect){ 20 + i * 70, 60, 50, 50 }, c);
        }
        fdk_surface_fill_circle_aa(s, 240, 200, 70,
                                   (fdk_color){ 0.9f, 0.3f, 0.1f, 1.0f });
        r = fdk_surface_present(s);
        if (!fdk_ok(r)) {
            printf("gl-canvas: WALL — present failed (%d)\n", (int)r);
            fdk_window_destroy(w);
            fdk_shutdown(ctx);
            return 1;
        }
    }
    printf("gl-canvas: 5 GPU frames presented\n");
    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    return 0;
}
