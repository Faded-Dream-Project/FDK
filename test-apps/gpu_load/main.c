/* main.c — gpu_load, the "is the GPU ACTUALLY doing the rendering?"
 * app (the owner's 2026-11-12 question).
 *
 * The question, precisely: light apps don't stress a GPU, so a
 * working window proves nothing; and "hardware acceleration" means
 * FULL GPU — the entire draw stream executing on the device, the
 * CPU barely touching pixels. This app answers it three ways:
 *
 *   1. LOAD — an animated scene at a tunable primitive count
 *      (LOAD_PRIMS, default 800/frame): rounded rects with alpha,
 *      circles, gradient bars, text. Enough quads that a CPU
 *      rasterizer visibly sinks and a GPU does not.
 *
 *   2. MEASUREMENT — fps, CPU wall time per frame (the paint calls
 *      only, timed with CLOCK_MONOTONIC), and the engine's own
 *      counters via fdk_window_gpu_paint_stats(): quads and batches
 *      per frame, lifetime totals, atlas/texture uploads. In GPU
 *      mode the quads ARE the GPU's draw work — thousands of quads
 *      with near-zero CPU paint time is the signature of "the GPU
 *      is doing it". In CPU mode the same scene runs with the
 *      counters at zero and the paint time doing the talking.
 *
 *   3. MODE HONESTY — the app runs in FDK_RENDER_GPU (the mode
 *      where the paint engine owns every draw and there is NO CPU
 *      fallback: window creation FAILS if the lane cannot engage).
 *      The report line names the mode, backend, device — a CPU
 *      drop-back is impossible by construction, not by promise.
 *
 * Knobs (environment):
 *   LOAD_PRIMS  primitives per frame          (default 800)
 *   RUN_MS      measurement duration in ms    (default 3000)
 *   HOLD_MS     extra visible hold, repainting (default 0; for
 *               screen probes / cameras — the visual proof)
 *   W, H        window size                   (default 900x600)
 *
 * A/B the paths (same scene, same measurement):
 *   FDK_RENDER=gpu  ./gpu_load     (the full-GPU paint engine)
 *   FDK_RENDER=cpu  ./gpu_load     (software rasterizer)
 *
 * On a software-rig (llvmpipe/lavapipe) GPU mode needs
 * FDK_GL_FORCE=1 to engage; on hardware it engages on its own.
 *
 * Exit codes: 0 = the run completed (either path — the REPORT is
 * the deliverable); 1 = GPU mode refused (lane or engine would not
 * engage — the error says which); 2 = FDK failed before the run.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    return (v != NULL && *v != '\0') ? atoi(v) : fallback;
}

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

static const char *mode_name(fdk_render_mode m) {
    switch (m) {
    case FDK_RENDER_GPU:
        return "gpu";
    case FDK_RENDER_CPU:
        return "cpu";
    default:
        return "hybrid";
    }
}

/* The scene: deterministic pseudo-random (xorshift), so every run
 * paints the same shape stream and the numbers compare A/B. */
static unsigned rng_state = 0x9E3779B9u;
static unsigned rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void paint_frame(fdk_surface *s, fdk_font *font, int frame,
                        int w, int h, int prims) {
    fdk_surface_fill(s, (fdk_color){ 0.05f, 0.06f, 0.09f, 1.0f });
    for (int i = 0; i < prims; i++) {
        int kind = (int)(rng() % 4);
        fdk_i32 x = (fdk_i32)(rng() % (unsigned)(w > 160 ? w - 160 : 1));
        fdk_i32 y = (fdk_i32)(rng() % (unsigned)(h > 120 ? h - 120 : 1));
        float a = 0.25f + (float)(rng() % 100) / 160.0f;
        fdk_color c = { (float)(rng() % 100) / 140.0f + 0.15f,
                        (float)(rng() % 100) / 160.0f + 0.15f,
                        (float)(rng() % 100) / 120.0f + 0.30f, a };
        switch (kind) {
        case 0:
            fdk_surface_fill_rounded_rect(
                s, (fdk_rect){ x, y, 60 + (int)(rng() % 100),
                               24 + (int)(rng() % 60) },
                (fdk_i32)(4 + (int)(rng() % 18)), c);
            break;
        case 1:
            fdk_surface_fill_circle(
                s, x + 40, y + 30, 6 + (int)(rng() % 34), c);
            break;
        case 2:
            fdk_surface_fill_gradient_vertical(
                s, (fdk_rect){ x, y, (fdk_i32)140, (fdk_i32)18 },
                (fdk_color){ c.r, c.g, c.b, 1.0f },
                (fdk_color){ c.b, c.r, c.g, 1.0f });
            break;
        default:
            fdk_surface_draw_rect(s, (fdk_rect){ x, y, 90, 50 }, c);
            break;
        }
    }
    /* A moving marker strip: the frame counter as text plus a
     * scrolling bar — visible proof the scene animates (and enough
     * glyph quads to exercise the atlas path every frame). */
    fdk_surface_fill_rect(s, (fdk_rect){ 0, h - 34, w, 34 },
                          (fdk_color){ 0.10f, 0.12f, 0.16f, 0.92f });
    char line[96];
    snprintf(line, sizeof(line), "frame %d  —  %d prims/frame", frame,
             prims);
    fdk_surface_draw_utf8(s, font, line, strlen(line), 12, h - 12,
                          (fdk_color){ 0.85f, 0.88f, 0.94f, 1.0f });
    fdk_i32 mx = (fdk_i32)((long)frame * 7 % (w > 40 ? w - 40 : 1));
    fdk_surface_fill_rect(s, (fdk_rect){ mx, h - 32, 28, 4 },
                          (fdk_color){ 0.95f, 0.55f, 0.20f, 1.0f });
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int prims = env_int("LOAD_PRIMS", 800);
    int run_ms = env_int("RUN_MS", 3000);
    int hold_ms = env_int("HOLD_MS", 0);
    int w = env_int("W", 900);
    int h = env_int("H", 600);
    if (w < 200) w = 200;
    if (h < 160) h = 160;

    fdk_init_options opts;
    memset(&opts, 0, sizeof(opts));
    /* THE POINT of this app: GPU mode. The paint engine owns every
     * draw call; there is no CPU rasterization fallback (a lane
     * that will not engage fails the window, loudly).
     *
     * The A/B escape: FDK_RENDER=cpu|hybrid in the environment runs
     * the SAME scene on the other path (options beat the env, so
     * the app must yield to it explicitly — this is the one app
     * where the option exists purely to be overridable). FDK_RENDER
     * unset means gpu. */
    fdk_render_mode want = FDK_RENDER_GPU;
    {
        const char *v = getenv("FDK_RENDER");
        if (v != NULL && strcmp(v, "cpu") == 0) {
            want = FDK_RENDER_CPU;
        } else if (v != NULL && strcmp(v, "hybrid") == 0) {
            want = FDK_RENDER_HYBRID;
        }
    }
    opts.render_mode = want;

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, &opts);
    if (!fdk_ok(r)) {
        fprintf(stderr, "gpu_load: fdk_init(FDK_RENDER_GPU) failed: %s\n",
                fdk_result_to_string(r));
        return 2;
    }

    fdk_window_options wopts;
    memset(&wopts, 0, sizeof(wopts));
    wopts.title = "fdk-gpu-load";
    wopts.width = w;
    wopts.height = h;
    wopts.render_mode = want;

    fdk_window *win = NULL;
    r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "gpu_load: REFUSED — render mode %s could not engage "
                "(%s). On a software rig add FDK_GL_FORCE=1; on "
                "hardware check the driver.\n",
                mode_name(want), fdk_result_to_string(r));
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_show(win);

    fdk_font *font = fdk_font_load_system_default(15);

    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_window_get_surface(win, &s)) || s == NULL) {
        fprintf(stderr, "gpu_load: surface acquire failed\n");
        fdk_window_destroy(win);
        fdk_shutdown(ctx);
        return 2;
    }

    /* Warm-up: one frame before the clock starts (first frame pays
     * atlas builds, texture realizes, lane bring-up — not steady
     * state; measuring it would flatter BOTH paths). The present
     * result is CHECKED: GPU mode refuses loudly when its lane
     * cannot engage, and this app's whole point is refusing a
     * silent CPU substitute — the check is the contract. */
    paint_frame(s, font, 0, w, h, prims);
    r = fdk_surface_present(s);
    if (!fdk_ok(r)) {
        fprintf(stderr,
                "gpu_load: REFUSED — the first present failed (%s); "
                "nothing reached a GPU lane and no CPU fallback "
                "exists in GPU mode. On a software rig add "
                "FDK_GL_FORCE=1.\n",
                fdk_result_to_string(r));
        if (font != NULL) {
            fdk_font_destroy(font);
        }
        fdk_window_destroy(win);
        fdk_shutdown(ctx);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        fdk_pump_events(ctx, 16);
    }

    /* The measured run. */
    double t0 = now_ms();
    double paint_ms = 0.0;
    int frames = 0;
    double elapsed = 0.0;
    for (; elapsed < (double)run_ms; elapsed = now_ms() - t0) {
        double p0 = now_ms();
        paint_frame(s, font, frames + 1, w, h, prims);
        (void)fdk_surface_present(s);
        paint_ms += now_ms() - p0;
        frames++;
        fdk_pump_events(ctx, 1);
    }

    /* The report: mode, backend, device, timing, engine counters. */
    fdk_render_backend backend = FDK_RENDER_BACKEND_CPU;
    (void)fdk_window_get_render_backend(win, &backend);
    const char *device = fdk_window_render_device(win);
    fdk_gpu_paint_stats gs;
    memset(&gs, 0, sizeof(gs));
    (void)fdk_window_gpu_paint_stats(win, &gs);

    double fps = elapsed > 0.0 ? frames * 1000.0 / elapsed : 0.0;
    double cpu_ms = frames > 0 ? paint_ms / (double)frames : 0.0;
    double quads_pf = frames > 0 ? (double)gs.total_quads / frames : 0.0;

    printf("gpu_load: mode=%s backend=%s device=\"%s\"\n",
           mode_name(fdk_window_render_mode(win)), backend_name(backend),
           device);
    printf("gpu_load: %d frames in %.0f ms — %.1f fps, %.2f CPU-ms/frame "
           "(paint+present, app thread)\n",
           frames, elapsed, fps, cpu_ms);
    printf("gpu_load: GPU paint engine: active=%u dead=%u — %.0f "
           "quads/frame, %llu batches lifetime, %llu atlas + %llu "
           "texture uploads\n",
           gs.active, gs.dead, quads_pf, gs.total_batches,
           gs.atlas_uploads, gs.texture_uploads);
    if (gs.active && !gs.dead && gs.total_frames > 0) {
        printf("gpu_load: VERDICT — the GPU paint engine carried every "
               "frame (%llu presents, %.0f quads each); CPU work per "
               "frame was vertex emission + the present call, no "
               "rasterization.\n",
               gs.total_frames, quads_pf);
    } else if (gs.active && gs.dead) {
        printf("gpu_load: VERDICT — the engine DIED mid-run (the lane "
               "downgrade); see the log above.\n");
    } else {
        printf("gpu_load: VERDICT — no GPU paint engine: pixels were "
               "CPU-rasterized (this is the FDK_RENDER=cpu behavior; "
               "compare its CPU-ms/frame against the GPU run).\n");
    }

    /* Optional visible hold for screen probes. */
    for (int t = 0; hold_ms > 0 && t < hold_ms; t += 33) {
        paint_frame(s, font, frames + 1 + t / 33, w, h, prims);
        (void)fdk_surface_present(s);
        fdk_pump_events(ctx, 33);
    }

    if (font != NULL) {
        fdk_font_destroy(font);
    }
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    return 0;
}
