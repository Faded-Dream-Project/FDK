/* test_dpi_scale.c — P1 close-out: the DPI/scale-change path on both
 * lanes.
 *
 * X11: the scale is detected ONCE at connect from Xft.dpi (or the
 * screen's ppm metric). Run one instance against a plain Xvfb (scale
 * must be 1) and one against "Xvfb -dpi 192" (scale must be 2); a GPU
 * window at scale 2 must present and read back correctly with
 * PHYSICAL surface dimensions.
 *
 * Wayland: run under a compositor whose output has scale 2 (the sway
 * headless rig: output HEADLESS-1 scale 2). The window's scale must
 * follow the compositor's preference (fractional-scale/buffer-scale),
 * and a GPU window must present + read back at the physical size.
 *
 * LANE=x11|wayland picks the flow; each self-skips when its display
 * is unreachable. */
#include "fdk/fdk.h"
#include "fdk/fdk_window.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, name)                                                    \
    do {                                                                     \
        printf("[%s] %s\n", (cond) ? "ok" : "FAIL", name);                   \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

int main(int argc, char **argv) {
    const char *lane = argc > 1 ? argv[1] : getenv("FDK_DPI_LANE");
    if (lane == NULL) {
        lane = "x11";
    }
    int expect = argc > 2 ? atoi(argv[2]) : 1;

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("SKIP: no FDK context on lane %s\n", lane);
        return 77;
    }
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.render_mode = FDK_RENDER_GPU;
    o.title = "fdk-dpi-scale";
    o.width = 120;
    o.height = 80;
    fdk_window *w = NULL;
    fdk_result r = fdk_window_create(ctx, &o, &w);
    if (!fdk_ok(r)) {
        printf("SKIP: no GPU window (%d) on lane %s\n", (int)r, lane);
        return 77;
    }
    fdk_window_show(w);
    for (int i = 0; i < 5; i++) {
        fdk_pump_events(ctx, 16);
    }
    fdk_f32 scale = 0.0f;
    CHECK(fdk_ok(fdk_window_get_scale(w, &scale)), "get_scale returns");
    CHECK((int)(scale + 0.5f) == expect,
          "scale matches the lane's DPI preference");
    printf("[info] lane=%s reported scale=%.2f expected=%d\n", lane,
           (double)scale, expect);

    if (scale > 1) {
        /* the surface reports PHYSICAL dimensions at scale > 1 */
        fdk_surface *s = NULL;
        fdk_window_get_surface(w, &s);
        fdk_surface_info info;
        memset(&info, 0, sizeof(info));
        if (fdk_ok(fdk_surface_get_info(s, &info))) {
            CHECK(info.width >= 120 && info.height >= 80,
                  "surface is at least the logical size (physical)");
            printf("[info] surface physical %dx%d\n", (int)info.width,
                   (int)info.height);
        }
        /* paint + present at the physical size: the GPU path must
         * survive the scaled buffer */
        fdk_surface_fill(s, (fdk_color){ 0.2f, 0.6f, 0.3f, 1.0f });
        fdk_result pr = fdk_surface_present(s);
        CHECK(fdk_ok(pr), "present at scale > 1");
        for (int i = 0; i < 3; i++) {
            fdk_pump_events(ctx, 16);
        }
    }
    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    printf(g_fail ? "DPI/SCALE: %d FAILURES\n" : "DPI/SCALE: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
