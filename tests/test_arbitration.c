/* test_arbitration.c — P2 close-out: per-surface present arbitration.
 * One context, three windows with three different presentation paths:
 *   - an add-on-driven GL surface (a GPU window whose frame callback
 *     draws raw GL — FDK swaps it),
 *   - a CPU window (the software path),
 *   - a HYBRID window (lane where available, software elsewhere).
 * Presents interleave for several rounds; each surface must keep its
 * own content (no GL state bleed into the software paths, no stale
 * software pixels in the GL surface).
 */
#include "fdk/fdk.h"
#include "platform/x11/x11_platform.h"
#include "window/window_internal.h" /* w->pwindow + the readback seam */
#include "render/surface_internal.h" /* fb.pixels for the software surfaces */
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        printf("[%s] %s\n", (cond) ? "ok" : "FAIL", name);                   \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

static fdk_platform_window *wgl_pwindow = NULL;

static void gl_frame(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user;
    /* raw GL: a solid magenta clear, drawn with the lane context
     * already current (the documented frame-callback contract) */
    static void (*ClearColor)(float, float, float, float) = NULL;
    static void (*Clear)(unsigned) = NULL;
    static void (*Finish)(void) = NULL;
    if (Clear == NULL) {
        extern void *fdk_x11_gl_proc_for_test(fdk_platform_window *pw,
                                              const char *name);
        ClearColor = (void (*)(float, float, float, float))
            fdk_x11_gl_proc_for_test(wgl_pwindow, "glClearColor");
        Clear = (void (*)(unsigned))fdk_x11_gl_proc_for_test(
            wgl_pwindow, "glClear");
        Finish = (void (*)(void))fdk_x11_gl_proc_for_test(wgl_pwindow,
                                                          "glFinish");
    }
    if (ClearColor != NULL) {
        ClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    }
    if (Clear != NULL) {
        Clear(0x00004100u /* GL_COLOR_BUFFER_BIT */);
    }
    if (Finish != NULL) {
        Finish();
    }
}

static fdk_u32 g_gpu_px[64 * 64];

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("SKIP: no FDK context\n");
        return 77;
    }
    fdk_window *wgl = NULL, *wcpu = NULL, *why = NULL;
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.title = "arb-gl";
    o.width = 64;
    o.height = 64;
    o.render_mode = FDK_RENDER_GPU;
    if (!fdk_ok(fdk_window_create(ctx, &o, &wgl))) {
        printf("SKIP: no GPU lane\n");
        return 77;
    }
    o.render_mode = FDK_RENDER_CPU;
    o.title = "arb-cpu";
    if (!fdk_ok(fdk_window_create(ctx, &o, &wcpu))) {
        printf("SKIP: CPU window refused\n");
        return 77;
    }
    o.render_mode = FDK_RENDER_HYBRID;
    o.title = "arb-hybrid";
    if (!fdk_ok(fdk_window_create(ctx, &o, &why))) {
        printf("SKIP: HYBRID window refused\n");
        return 77;
    }
    fdk_window_show(wgl);
    fdk_window_show(wcpu);
    fdk_window_show(why);
    for (int i = 0; i < 3; i++) {
        fdk_pump_events(ctx, 16);
    }
    wgl_pwindow = wgl->pwindow;

    for (int round = 0; round < 2; round++) {
        /* interleave: GL, CPU, HYBRID, then reverse */
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgl, &s);
        fdk_surface_fill(s, (fdk_color){ 0.2f, 0.2f, 0.2f, 1.0f });
        CHECK(fdk_ok(fdk_surface_present(s)), "GL surface presents");
        fdk_window_get_surface(wcpu, &s);
        fdk_surface_fill(s, (fdk_color){ 0.9f, 0.5f, 0.1f, 1.0f });
        CHECK(fdk_ok(fdk_surface_present(s)), "CPU surface presents");
        fdk_window_get_surface(why, &s);
        fdk_surface_fill(s, (fdk_color){ 0.1f, 0.6f, 0.9f, 1.0f });
        CHECK(fdk_ok(fdk_surface_present(s)), "HYBRID surface presents");
        fdk_pump_events(ctx, 16);
    }

    /* the lane context attaches at the first present: NOW the add-on
     * claims the GL window's buffer with a frame callback (the
     * arbitration contract: from here the callback owns the buffer) */
    CHECK(fdk_ok(fdk_x11_window_gl_set_frame_cb(
              wgl->pwindow, (void (*)(void *, void *))gl_frame, NULL,
              NULL)),
          "frame callback set");
    for (int round = 0; round < 2; round++) {
        fdk_surface *s0 = NULL;
        fdk_window_get_surface(wgl, &s0);
        fdk_surface_fill(s0, (fdk_color){ 0.2f, 0.2f, 0.2f, 1.0f });
        CHECK(fdk_ok(fdk_surface_present(s0)), "GL surface presents");
        fdk_window_get_surface(wcpu, &s0);
        fdk_surface_fill(s0, (fdk_color){ 0.9f, 0.5f, 0.1f, 1.0f });
        (void)fdk_surface_present(s0);
        fdk_window_get_surface(why, &s0);
        fdk_surface_fill(s0, (fdk_color){ 0.1f, 0.6f, 0.9f, 1.0f });
        (void)fdk_surface_present(s0);
        fdk_pump_events(ctx, 16);
    }

    /* each surface kept its own content */
    memset(g_gpu_px, 0, sizeof(g_gpu_px));
    int gpu_ok = 0;
    if (fdk_ok(fdk_x11_window_gpu_readback(wgl->pwindow, g_gpu_px, 64,
                                           64))) {
        /* the frame callback's magenta clear (1.0, 0.0, 1.0) */
        fdk_u32 p = g_gpu_px[32 * 64 + 32];
        int rr = (int)((p >> 16) & 0xFF), gg = (int)((p >> 8) & 0xFF),
            bb = (int)(p & 0xFF);
        gpu_ok = rr > 200 && gg < 60 && bb > 200;
        if (!gpu_ok) {
            printf("[info] GL surface pixel r=%d g=%d b=%d\n", rr, gg, bb);
        }
    }
    CHECK(gpu_ok, "the add-on GL surface shows the raw GL content");

    fdk_surface *sc = NULL;
    fdk_window_get_surface(wcpu, &sc);
    fdk_u32 cp = sc->fb.pixels[32 * (size_t)sc->fb.stride + 32];
    int cr = (int)((cp >> 16) & 0xFF), cg = (int)((cp >> 8) & 0xFF);
    CHECK(cr > 200 && cg > 100 && cr > cg,
          "the CPU surface shows its own color");

    fdk_surface *sh = NULL;
    fdk_window_get_surface(why, &sh);
    fdk_u32 hp = sh->fb.pixels[32 * (size_t)sh->fb.stride + 32];
    int hr = (int)((hp >> 16) & 0xFF), hb = (int)(hp & 0xFF);
    CHECK(hb > 200 && hb > hr, "the HYBRID surface shows its own color");

    fdk_window_destroy(wgl);
    fdk_window_destroy(wcpu);
    fdk_window_destroy(why);
    fdk_shutdown(ctx);
    printf(g_fail ? "ARBITRATION: %d FAILURES\n"
                  : "ARBITRATION: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
