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
 *   LOAD_PRIMS  primitives per frame          (default 800; the
 *               sweep's FIRST rung)
 *   RUN_MS      measurement duration in ms    (default 3000; per
 *               rung in sweep mode)
 *   HOLD_MS     extra visible hold, repainting (default 0; for
 *               screen probes / cameras — the visual proof; in
 *               sweep mode the hold paints at the TOP rung, so a
 *               meter watching during the hold sees peak load)
 *   W, H        window size                   (default 900x600)
 *   SWEEP       1 = ladder mode (default), 0 = the single-load run
 *   SWEEP_STEPS rungs in the ladder           (default 4, max 8)
 *   SWEEP_MAX_PRIMS  clamp rungs above this   (default 0 = none;
 *               set it on the CPU leg or a slow rig — a rung's
 *               cost is at least one full frame)
 *
 * SWEEP mode (2026-11-12, the owner's hardware report: CPU leg
 * pegged a core, GPU utilization +10%): the utilization meter only
 * moves when the load is big enough, so the sweep CLIMBS — x4
 * primitives per rung (800 → 3.2k → 12.8k → 51.2k at defaults),
 * reporting per rung: fps, CPU-ms/frame, quads/frame and ktris/s
 * from the engine's own counters. On hardware the GPU run should
 * hold fps while quads/frame climbs 4x per rung — and the meter
 * finally moves. Low utilization at LOW rungs is expected and
 * honest: a light scene is light work; the ladder exists to find
 * where "light" stops. Watch the meter (nvidia-smi / the task
 * manager) during SWEEP + HOLD_MS=10000: the climb, then the hold.
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

/* One rung: run the scene at `prims` for `run_ms`, return the
 * timing; the ENGINE-side numbers come back through `stats` (a
 * lifetime snapshot — the caller diffs against the previous rung to
 * make each row self-contained). Present refusals are counted: a
 * GPU-mode window that stops presenting must say so, not vanish
 * into a silent stall (the Round 69 honesty contract). */
typedef struct rung_out {
    double elapsed_ms;
    double paint_ms;
    int frames;
    int refused;
} rung_out;

static void run_rung(fdk_context *ctx, fdk_surface *s, fdk_font *font,
                     int w, int h, int prims, int run_ms, int warm,
                     rung_out *out) {
    memset(out, 0, sizeof(*out));
    if (warm) {
        /* One uncounted frame at this rung's load: the atlas/texture
         * counters and the batch-fullness path settle before the
         * clock starts (the same reasoning as the single-run
         * warm-up, applied per rung). */
        paint_frame(s, font, 0, w, h, prims);
        if (!fdk_ok(fdk_surface_present(s))) {
            out->refused = 1;
            return;
        }
    }
    double t0 = now_ms();
    for (;;) {
        double elapsed = now_ms() - t0;
        if (elapsed >= (double)run_ms) {
            out->elapsed_ms = elapsed;
            break;
        }
        double p0 = now_ms();
        paint_frame(s, font, out->frames + 1, w, h, prims);
        if (!fdk_ok(fdk_surface_present(s))) {
            out->refused++;
        }
        out->paint_ms += now_ms() - p0;
        out->frames++;
        fdk_pump_events(ctx, 1);
    }
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

    /* The ladder (default ON): x4 primitives per rung. SWEEP=0 keeps
     * the original single-load run for quick A/B comparisons. */
    int sweep = env_int("SWEEP", 1);
    int steps = env_int("SWEEP_STEPS", 4);
    int sweep_max = env_int("SWEEP_MAX_PRIMS", 0);
    if (steps < 1) steps = 1;
    if (steps > 8) steps = 8;
    if (prims < 1) prims = 1;

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

    /* Warm-up at the FIRST rung's load: one frame before any clock
     * starts (first frame pays atlas builds, texture realizes, lane
     * bring-up — not steady state; measuring it would flatter BOTH
     * paths). The present result is CHECKED: GPU mode refuses loudly
     * when its lane cannot engage, and this app's whole point is
     * refusing a silent CPU substitute — the check is the contract. */
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

    /* The report preamble: mode, backend, device — once. */
    fdk_render_backend backend = FDK_RENDER_BACKEND_CPU;
    (void)fdk_window_get_render_backend(win, &backend);
    const char *device = fdk_window_render_device(win);
    printf("gpu_load: mode=%s backend=%s device=\"%s\"\n",
           mode_name(fdk_window_render_mode(win)), backend_name(backend),
           device);
    if (sweep) {
        printf("gpu_load: sweep %d rung%s x %d ms (x4 prims per rung)\n",
               steps, steps == 1 ? "" : "s", run_ms);
    }

    /* The measured run(s): one load, or the ladder. */
    fdk_gpu_paint_stats gs;
    memset(&gs, 0, sizeof(gs));
    (void)fdk_window_gpu_paint_stats(win, &gs);
    unsigned long long prev_quads = gs.total_quads;
    unsigned long long prev_batches = gs.total_batches;
    unsigned long long prev_frames = gs.total_frames;
    int nrungs = sweep ? steps : 1;
    int rung_prims = prims;
    int top_prims = prims;
    int refused_total = 0;
    int first_fps = 0, last_fps = 0;
    double first_cpu_ms = 0.0;

    for (int rung = 0; rung < nrungs; rung++) {
        rung_out out;
        run_rung(ctx, s, font, w, h, rung_prims, run_ms, rung > 0,
                 &out);
        refused_total += out.refused;
        (void)fdk_window_gpu_paint_stats(win, &gs);
        unsigned long long dquads = gs.total_quads - prev_quads;
        unsigned long long dbatches = gs.total_batches - prev_batches;
        unsigned long long dframes = gs.total_frames - prev_frames;
        prev_quads = gs.total_quads;
        prev_batches = gs.total_batches;
        prev_frames = gs.total_frames;

        double fps = out.elapsed_ms > 0.0
                         ? out.frames * 1000.0 / out.elapsed_ms
                         : 0.0;
        double cpu_ms = out.frames > 0
                            ? out.paint_ms / (double)out.frames
                            : 0.0;
        double quads_pf =
            dframes > 0 ? (double)dquads / (double)dframes : 0.0;
        /* Every quad is 2 triangles of SDF fragment work. */
        double ktris_s = out.elapsed_ms > 0.0
                             ? (double)dquads * 2.0 /
                                   (out.elapsed_ms / 1000.0) / 1000.0
                             : 0.0;
        if (rung == 0) {
            first_fps = (int)(fps + 0.5);
            first_cpu_ms = cpu_ms;
        }
        last_fps = (int)(fps + 0.5);
        top_prims = rung_prims;

        printf("gpu_load: rung %d/%d %6d prims — %3d fps, %6.2f "
               "CPU-ms/frame, %7.0f quads/frame (%1.0f ktri/s, %llu "
               "batches)%s\n",
               rung + 1, nrungs, rung_prims, (int)(fps + 0.5), cpu_ms,
               quads_pf, ktris_s, dbatches,
               out.refused > 0 ? "  PRESENTS REFUSED" : "");

        /* Climb: x4, honoring the rig's clamp. */
        if (rung + 1 < nrungs) {
            long next = (long)rung_prims * 4;
            if (sweep_max > 0 && next > sweep_max) {
                next = sweep_max;
            }
            if (next <= rung_prims) {
                printf("gpu_load: sweep clamped at %d prims "
                       "(SWEEP_MAX_PRIMS) — ladder stops\n",
                       rung_prims);
                break;
            }
            rung_prims = (int)next;
        }
    }

    /* Verdict (from the engine's own lifetime numbers). */
    double quads_pf = gs.total_frames > 0
                          ? (double)gs.total_quads /
                                (double)gs.total_frames
                          : 0.0;
    printf("gpu_load: GPU paint engine: active=%u dead=%u — %.0f "
           "quads/frame avg, %llu batches lifetime, %llu atlas + %llu "
           "texture uploads\n",
           gs.active, gs.dead, quads_pf, gs.total_batches,
           gs.atlas_uploads, gs.texture_uploads);
    if (gs.active && !gs.dead && gs.total_frames > 0 &&
        refused_total == 0) {
        printf("gpu_load: VERDICT — the GPU paint engine carried every "
               "frame (%llu presents); CPU work per frame was vertex "
               "emission + the present call, no rasterization.\n",
               gs.total_frames);
    } else if (refused_total > 0) {
        printf("gpu_load: VERDICT — %d present(s) REFUSED — the lane "
               "stopped serving frames; see the log above.\n",
               refused_total);
    } else if (gs.active && gs.dead) {
        printf("gpu_load: VERDICT — the engine DIED mid-run (the lane "
               "downgrade); see the log above.\n");
    } else {
        printf("gpu_load: VERDICT — no GPU paint engine: pixels were "
               "CPU-rasterized (this is the FDK_RENDER=cpu behavior; "
               "compare its CPU-ms/frame against the GPU run).\n");
    }
    if (sweep && nrungs > 1 && first_fps > 0) {
        /* The scaling check, spelled out: in GPU mode the same fps
         * with 4x (or 16x) the primitives means the DEVICE absorbed
         * the load — the CPU certainly could not (its cost scales
         * with pixel coverage, and it was already measured at the
         * first rung). This is the line to read against the
         * utilization meter: watch it during the top rungs / the
         * hold; low utilization at the FIRST rung is expected and
         * honest — light scenes are light work. */
        printf("gpu_load: scale check — %d prims @ %d fps vs %d prims "
               "@ %d fps%s (%.2f CPU-ms at rung 1)\n",
               prims, first_fps, top_prims, last_fps,
               last_fps >= first_fps
                   ? ": fps HELD while the load climbed — the device "
                     "is absorbing it (watch the GPU meter now)"
                   : ": fps sank as the load climbed — the ceiling "
                     "approached",
               first_cpu_ms);
    }

    /* Optional visible hold for screen probes (at the TOP rung's
     * load — the meter-watcher's moment). */
    for (int t = 0; hold_ms > 0 && t < hold_ms; t += 33) {
        paint_frame(s, font, t / 33, w, h, top_prims);
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
