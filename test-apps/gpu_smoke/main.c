/* main.c — gpu_smoke, the GPU-backend smoke app.
 *
 * The owner's ask: "an app that will immediately close like a test
 * app or example" proving the GPU present path works end to end.
 *
 * What it does, in order:
 *   1. connects (X11 or Wayland, whatever the session provides),
 *   2. creates a window (HYBRID render mode — the lane engages
 *      whenever it can; FDK_GPU_BACKEND / FDK_GL_VULKAN /
 *      FDK_GL_OPENGL / FDK_GL_FORCE steer WHICH lane, exactly as in
 *      the library),
 *   3. paints ONE frame (a background, two solid shapes — enough
 *      surface area to clear the small-window size gate),
 *   4. presents it,
 *   5. prints ONE line — backend + device + window size — for the
 *      rig to grep,
 *   6. destroys the window and exits 0 immediately.
 *
 * rc 0 = the app ran; the PRINTED backend is the verdict (a CPU
 * answer here is honest, not a failure — gpu_strict is the app that
 * refuses it).
 *
 * Everything on the public API only, like every test-app here.
 *
 * Run:  ./build/gpu_smoke            (AUTO: Vulkan, then OpenGL)
 *       FDK_GPU_BACKEND=vulkan ./build/gpu_smoke
 *       FDK_GL_OPENGL=1        ./build/gpu_smoke
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *backend_name(fdk_render_backend b) {
    switch (b) {
    case FDK_RENDER_BACKEND_VULKAN:
        return "vulkan";
    case FDK_RENDER_BACKEND_OPENGL:
        return "opengl";
    default:
        return "cpu";
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "gpu_smoke: fdk_init failed\n");
        return 1;
    }

    fdk_window_options wopts;
    memset(&wopts, 0, sizeof(wopts));
    wopts.title = "fdk-gpu-smoke";
    wopts.width = 480;
    wopts.height = 320;
    /* HYBRID: the lane is preferred, CPU is the fallback — the smoke
     * observes what actually engaged. (GPU mode would REFUSE the
     * window when no lane can; that verdict belongs to gpu_strict.) */
    wopts.render_mode = FDK_RENDER_HYBRID;

    fdk_window *win = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &win))) {
        fprintf(stderr, "gpu_smoke: window create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_window_show(win);

    /* One frame: enough pixels to clear the small-surface gate, two
     * solid shapes so the damage carries content, present, done. */
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_window_get_surface(win, &s)) && s != NULL) {
        fdk_surface_fill(s, (fdk_color){ 0.10f, 0.12f, 0.16f, 1.0f });
        fdk_surface_fill_rect(s, (fdk_rect){ 40, 40, 160, 120 },
                              (fdk_color){ 0.90f, 0.25f, 0.20f, 1.0f });
        fdk_surface_fill_rect(s, (fdk_rect){ 240, 120, 200, 160 },
                              (fdk_color){ 0.20f, 0.55f, 0.95f, 1.0f });
        (void)fdk_surface_present(s);
    }
    /* Let the backend commit the frame (Wayland's configure arrives
     * asynchronously; a couple of pump passes flush it). */
    for (int i = 0; i < 8; i++) {
        fdk_pump_events(ctx, 16);
    }

    /* Optional hold (the pixel-verification rig): HOLD_MS=<n> keeps
     * the window alive repainting for n milliseconds so a screen
     * probe can capture a live frame. */
    {
        const char *hold = getenv("HOLD_MS");
        int ms = hold != NULL ? atoi(hold) : 0;
        for (int t = 0; ms > 0 && t < ms; t += 33) {
            fdk_pump_events(ctx, 33);
        }
    }

    fdk_render_backend backend = FDK_RENDER_BACKEND_CPU;
    (void)fdk_window_get_render_backend(win, &backend);
    const char *device = fdk_window_render_device(win);

    printf("gpu_smoke: backend=%s device=\"%s\" window=%dx%d\n",
           backend_name(backend), device, (int)wopts.width,
           (int)wopts.height);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    return 0;
}
