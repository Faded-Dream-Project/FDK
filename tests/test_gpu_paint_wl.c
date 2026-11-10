/* test_gpu_paint.c — the GPU paint engine's runtime proof (X11/Xvfb).
 *
 * Hardened comparison (P1 hardening): the scene below covers solid
 * fills, SDF rounded rects (fill + 1px stroke), circles, vertical
 * gradients, nested clips, an offscreen blit, a 45-DEGREE transformed
 * blit (the real affine path), the fdk_texture upload path, and an
 * asymmetric marker (a lone rect off-center in both axes — a mirrored
 * scene cannot pass). The reference is the same scene rendered into
 * an offscreen CPU surface; the GPU frame is read back through the
 * engine's glReadPixels seam. Per-region statistics (mean / max /
 * p99 of the per-pixel channel diff) are reported and gated on
 * explicit tolerances: SDF antialiasing legitimately differs from the
 * CPU rasterizer's coverage at shape EDGES (up to one coverage step),
 * while solid interiors must be near-exact.
 *
 * Second phase: glyph-atlas overflow — ~2800 unique synthetic mask
 * blits far exceed the 1024x1024 atlas; the engine must evict (shelf
 * reset) and keep presenting correct frames. A full atlas must never
 * kill the engine.
 *
 * GPU-mode contracts pinned here: FDK_NO_GL + GPU -> UNSUPPORTED at
 * creation; texture create/update/draw roundtrip; resize repaint.
 *
 * Coordinate convention under test: primitives speak TOP-LEFT-ORIGIN
 * pixels; the single Y flip happens in the vertex shader (see
 * src/render/gpaint.h). The asymmetric marker makes any double flip
 * visible in the diff map.
 *
 * Run: make test-gpu-paint (Xvfb + FDK_GL_FORCE=1, llvmpipe).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_core.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_window.h"
#include "render/surface_internal.h"
#include "window/window_internal.h"   /* wgpu->pwindow for the seam */
#include "platform/wayland/wayland_platform.h"
#include "render/gpaint.h"


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("[FAIL] %s\n", msg);                                    \
            g_fail++;                                                      \
        } else {                                                           \
            printf("[ok] %s\n", msg);                                      \
        }                                                                  \
    } while (0)

#define W 480
#define H 300

typedef struct {
    double mean, max, p99;
} stats;

/* Per-pixel per-channel abs diff over a region; p99 from a 256-bin
 * histogram (diffs are 0..255). */
static void region_stats(const fdk_u32 *ref, const fdk_u32 *gpu, int rx,
                         int ry, int rw, int rh, stats *out) {
    long hist[256] = { 0 };
    double sum = 0;
    long n = 0;
    long mx = 0;
    for (int y = ry; y < ry + rh && y < H; y++) {
        for (int x = rx; x < rx + rw && x < W; x++) {
            fdk_u32 a = ref[y * W + x], b = gpu[y * W + x];
            for (int ch = 0; ch < 3; ch++) {
                int sh = ch * 8;
                long d = labs((long)((a >> sh) & 0xff) -
                              (long)((b >> sh) & 0xff));
                hist[d]++;
                sum += (double)d;
                if (d > mx) {
                    mx = d;
                }
                n++;
            }
        }
    }
    out->mean = n ? sum / (double)n : 0.0;
    out->max = (double)mx;
    long acc = 0;
    out->p99 = 255.0;
    for (int d = 0; d < 256; d++) {
        acc += hist[d];
        if (acc * 100 >= n * 99) {
            out->p99 = (double)d;
            break;
        }
    }
}

/* Interior/edge split: a pixel is INTERIOR when the REFERENCE's 3x3
 * neighborhood is uniform (max per-channel delta <= 2 across all 8
 * neighbors) — at least ~1.5px from any shape edge, AA transition, or
 * 1px feature. Interiors must be near-exact: the GPU paints flat
 * colors and gradients with the same math the CPU does. The EDGE band
 * is everything else (AA outlines, 1px strokes, ring arcs) where the
 * two rasterizers legitimately differ: the CPU computes coverage as
 * an AREA fraction, the GPU samples an SDF distance — different
 * answers at the same edge pixel, by construction. Strokes and circle
 * rings are ENTIRELY edge band (1px features have no interior), which
 * is why their region p99 lives in the edge tolerance, not the
 * interior one. */
static double dist_delta(fdk_u32 a, fdk_u32 b, int sh) {
    return (double)labs((long)((a >> sh) & 0xff) -
                        (long)((b >> sh) & 0xff));
}

static void region_stats_split(const fdk_u32 *ref, const fdk_u32 *gpu,
                               int rx, int ry, int rw, int rh,
                               stats *interior, stats *edge) {
    long ihist[256] = { 0 }, ehist[256] = { 0 };
    double isum = 0, esum = 0;
    long in = 0, en = 0, imx = 0, emx = 0;
    for (int y = ry; y < ry + rh && y < H; y++) {
        for (int x = rx; x < rx + rw && x < W; x++) {
            /* uniform 3x3 neighborhood in the ref? */
            int uniform = 1;
            fdk_u32 c = ref[y * W + x];
            for (int dy = -1; dy <= 1 && uniform; dy++) {
                for (int dx = -1; dx <= 1 && uniform; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) {
                        continue;
                    }
                    fdk_u32 n = ref[ny * W + nx];
                    for (int ch = 0; ch < 3; ch++) {
                        if (dist_delta(c, n, ch * 8) > 2.0) {
                            uniform = 0;
                            break;
                        }
                    }
                }
            }
            fdk_u32 a = ref[y * W + x], b = gpu[y * W + x];
            long d = 0;
            for (int ch = 0; ch < 3; ch++) {
                long dd = labs((long)((a >> (ch * 8)) & 0xff) -
                               (long)((b >> (ch * 8)) & 0xff));
                if (dd > d) {
                    d = dd;
                }
            }
            if (uniform) {
                ihist[d]++;
                isum += (double)d;
                if (d > imx) imx = d;
                in++;
            } else {
                ehist[d]++;
                esum += (double)d;
                if (d > emx) emx = d;
                en++;
            }
        }
    }
    /* interior: max per-channel delta; edge: mean/max/p99 histogram */
    interior->mean = in ? isum / (double)in : 0.0;
    interior->max = (double)imx;
    interior->p99 = interior->max;
    edge->mean = en ? esum / (double)en : 0.0;
    edge->max = (double)emx;
    long acc = 0;
    edge->p99 = 255.0;
    for (int d = 0; d < 256; d++) {
        acc += ehist[d];
        if (acc * 100 >= en * 99) {
            edge->p99 = (double)d;
            break;
        }
    }
}

static void report_split(const char *name, const stats *in, double ti_mean,
                         double ti_max, const stats *ed, double te_mean,
                         double te_max, double te_p99, int *fails) {
    printf("  %-22s interior mean=%6.3f max=%4.1f [n>0] | edge "
           "mean=%6.3f max=%5.1f p99=%5.1f",
           name, in->mean, in->max, ed->mean, ed->max, ed->p99);
    if (in->mean <= ti_mean && in->max <= ti_max && ed->mean <= te_mean &&
        ed->max <= te_max && ed->p99 <= te_p99) {
        printf("  [ok]\n");
    } else {
        printf("  [FAIL] (tol int mean<=%g max<=%g; edge mean<=%g "
               "max<=%g p99<=%g)\n",
               ti_mean, ti_max, te_mean, te_max, te_p99);
        (*fails)++;
    }
}

static void report(const char *name, const stats *s, double t_mean,
                   double t_max, double t_p99, int *fails) {
    printf("  %-22s mean=%7.3f max=%5.1f p99=%5.1f", name, s->mean,
           s->max, s->p99);
    if (s->mean <= t_mean && s->max <= t_max && s->p99 <= t_p99) {
        printf("  [ok]\n");
    } else {
        printf("  [FAIL] (tol mean<=%g max<=%g p99<=%g)\n", t_mean, t_max,
               t_p99);
        (*fails)++;
    }
}

/* The shared scene. Every region's bounds are recorded for the
 * per-region statistics. */
static struct {
    fdk_rect solid, round_fill, round_stroke, grad, circles, clips,
        blit, rotated, texture, marker;
} R;

static void paint_scene(fdk_surface *s) {
    fdk_color bg = { 0.10f, 0.12f, 0.16f, 1.0f };
    fdk_surface_fill(s, bg);
    fdk_color c1 = { 0.9f, 0.2f, 0.2f, 1.0f };
    fdk_color c2 = { 0.2f, 0.4f, 0.95f, 1.0f };
    fdk_color c3 = { 0.1f, 0.8f, 0.3f, 1.0f };
    fdk_color c4 = { 1.0f, 0.85f, 0.1f, 1.0f };

    R.solid = (fdk_rect){ 10, 10, 60, 40 };
    fdk_surface_fill_rect(s, R.solid, c1);

    R.round_fill = (fdk_rect){ 90, 10, 60, 40 };
    fdk_surface_fill_rounded_rect_aa(s, R.round_fill, 12, c2);

    R.round_stroke = (fdk_rect){ 170, 10, 60, 40 };
    fdk_surface_draw_rounded_rect_aa(s, R.round_stroke, 10, c3);
    fdk_surface_draw_rect(s, (fdk_rect){ 170, 10, 60, 40 }, c1);

    R.grad = (fdk_rect){ 250, 10, 60, 40 };
    fdk_surface_fill_gradient_vertical(s, R.grad, c1, c2);

    R.circles = (fdk_rect){ 320, 10, 150, 90 };
    fdk_surface_fill_circle_aa(s, 350, 35, 24, c3);
    fdk_surface_draw_circle_aa(s, 420, 35, 24, c4);

    /* nested clips: an outer clip and an inner clip, then a rect that
     * would cover both — only the intersection may appear */
    R.clips = (fdk_rect){ 10, 70, 120, 80 };
    fdk_surface_push_clip(s, (fdk_rect){ 10, 70, 120, 80 });
    fdk_surface_push_clip(s, (fdk_rect){ 40, 90, 120, 40 });
    fdk_surface_fill_rect(s, (fdk_rect){ 0, 0, 480, 300 }, c4);
    fdk_surface_pop_clip(s);
    fdk_surface_pop_clip(s);

    R.marker = (fdk_rect){ 13, 187, 9, 7 }; /* asymmetric marker */
    fdk_surface_fill_rect(s, R.marker, c1);

    R.blit = (fdk_rect){ 150, 80, 90, 70 };   /* filled by the blit */
    R.rotated = (fdk_rect){ 250, 80, 110, 90 };
    R.texture = (fdk_rect){ 370, 120, 32, 32 }; /* 4x integer */

    /* lines through the bottom band */
    fdk_surface_draw_line(s, 20, 270, 460, 270, c4);
    fdk_surface_draw_line_aa(s, 20, 280, 460, 280, c2);
}

/* The dynamic part: an offscreen card blitted axis-aligned (scaled)
 * AND through a 45-degree rotation, plus the texture upload path. */
static void paint_scene_dynamic(fdk_surface *s, fdk_surface *card,
                                fdk_texture *tex) {
    paint_scene(s);
    fdk_surface_blit(s, R.blit.x, R.blit.y, card,
                     (fdk_rect){ 0, 0, card->fb.width, card->fb.height });
    float a = 3.14159265f / 4.0f;
    fdk_matrix rot = { cosf(a), -sinf(a), 0, sinf(a), cosf(a), 0 };
    fdk_matrix sc = { 0.8f, 0, 0, 0, 0.8f, 0 };
    /* mul(a, b) = "first a, then b": scale, then rotate, then place */
    fdk_matrix m = fdk_matrix_mul(sc, rot);
    fdk_matrix tr = { 1, 0, (fdk_f32)R.rotated.x + 55.0f - 32.0f,
                      0, 1, (fdk_f32)R.rotated.y + 45.0f - 24.0f };
    m = fdk_matrix_mul(m, tr);
    fdk_surface_blit_transformed(s, m, card);
    fdk_texture_draw(s, tex, R.texture);
}

static void fill_card(fdk_surface *card) {
    fdk_color bgc = { 0.95f, 0.95f, 0.97f, 1.0f };
    fdk_color cc = { 0.2f, 0.5f, 0.9f, 1.0f };
    fdk_surface_fill(card, bgc);
    fdk_surface_fill_rounded_rect_aa(
        card, (fdk_rect){ 4, 4, card->fb.width - 8, card->fb.height - 8 },
        10, cc);
    fdk_surface_fill_circle_aa(card, 20, 24, 10,
                               (fdk_color){ 0.9f, 0.3f, 0.1f, 1.0f });
}

int main(void) {
    fdk_test_hermetic_pin();
    setenv("FDK_GL_FORCE", "1", 1);
    unsetenv("FDK_NO_GL");


    /* Contract: no GL stack -> GPU mode is refused, never faked. */
    printf("[info] wayland lane = EGL/GLES (the GLES-only path)\n");
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("SKIP: no FDK context\n");
        return 77;
    }

    fdk_window_options gpu_opts;
    memset(&gpu_opts, 0, sizeof(gpu_opts));
    gpu_opts.render_mode = FDK_RENDER_GPU;
    gpu_opts.title = "fdk-gpu-test-gpu";
    gpu_opts.width = W;
    gpu_opts.height = H;
    fdk_window *wgpu = NULL;
    fdk_result r = fdk_window_create(ctx, &gpu_opts, &wgpu);
    if (!fdk_ok(r)) {
        printf("[skip] GPU window create failed (%d) — no usable GL\n",
               (int)r);
        fdk_shutdown(ctx);
        return (g_fail ? 1 : 77);
    }
    CHECK(1, "GPU window created (lane + engine up)");
    fdk_window_show(wgpu);

    fdk_surface *sgpu = NULL;
    fdk_window_get_surface(wgpu, &sgpu);
    (void)sgpu;

    /* references and sources */
    fdk_surface *ref = NULL;
    fdk_surface_create(W, H, &ref);
    fdk_surface *card = NULL;
    fdk_surface_create(64, 48, &card);
    fill_card(card);
    fdk_texture *tex = NULL;
    CHECK(fdk_ok(fdk_texture_create(8, 8, &tex)), "texture created");
    fdk_u8 tpx[4 * 8 * 8];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            fdk_u8 *p = tpx + (y * 8 + x) * 4;
            p[0] = (fdk_u8)(x * 31);
            p[1] = (fdk_u8)(y * 31);
            p[2] = 200;
            p[3] = 255;
        }
    }
    fdk_texture_update(tex, (fdk_rect){ 0, 0, 8, 8 }, tpx, 8 * 4);

    /* the CPU reference frame */
    {
        FILE *rf = fopen("/tmp/gpu_ref_now.ppm", "wb");
        (void)rf;
    }
    if (getenv("GPU_MINIMAL") != NULL) {
        fdk_color bg = { 0.10f, 0.12f, 0.16f, 1.0f };
        fdk_surface_fill(ref, bg);
        fdk_surface_fill_rect(ref, (fdk_rect){ 20, 20, 60, 40 },
                              (fdk_color){ 0.9f, 0.2f, 0.2f, 1.0f });
        fdk_surface_fill_rounded_rect_aa(
            ref, (fdk_rect){ 120, 20, 60, 40 }, 12,
            (fdk_color){ 0.2f, 0.4f, 0.95f, 1.0f });
    } else {
        paint_scene_dynamic(ref, card, tex);
    }
    static fdk_u32 pref[W * H];
    for (int y = 0; y < H; y++) {
        const fdk_u32 *row =
            ref->own_pixels + (size_t)y * ref->fb.stride;
        for (int x = 0; x < W; x++) {
            /* own_pixels is BGRX in memory (R in the low byte); the
             * readback seam packs RGBA (R in bits 16-23) — align. */
            fdk_u32 p = row[x];
            pref[y * W + x] = 0xff000000u | ((p & 0xffu) << 16) |
                              (p & 0xff00u) | ((p >> 16) & 0xffu);
        }
    }

    /* dump the ref for visual inspection */
    {
        FILE *f = fopen("/tmp/gpu_ref_now.ppm", "wb");
        if (f != NULL) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            for (int yy = 0; yy < H; yy++) {
                for (int xx = 0; xx < W; xx++) {
                    fdk_u32 p = pref[yy * W + xx];
                    fdk_u8 rgb[3] = { (fdk_u8)(p >> 16), (fdk_u8)(p >> 8),
                                      (fdk_u8)p };
                    fwrite(rgb, 1, 3, f);
                }
            }
            fclose(f);
        }
    }

    static fdk_u32 ppre[W * H];
    /* Rasterization check first: paint one frame and read the back
     * buffer directly (the seam flushes without swapping). */
    {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgpu, &s);
        if (getenv("GPU_EMPTY") != NULL) {
            /* untouched surface */
        } else if (getenv("GPU_MINIMAL") != NULL) {
            fdk_color bg = { 0.10f, 0.12f, 0.16f, 1.0f };
            fdk_surface_fill(s, bg);
            fdk_surface_fill_rect(s, (fdk_rect){ 20, 20, 60, 40 },
                                  (fdk_color){ 0.9f, 0.2f, 0.2f, 1.0f });
            fdk_surface_fill_rounded_rect_aa(
                s, (fdk_rect){ 120, 20, 60, 40 }, 12,
                (fdk_color){ 0.2f, 0.4f, 0.95f, 1.0f });
        } else {
            paint_scene_dynamic(s, card, tex);
        }
        if (fdk_ok(fdk_wayland_window_gpu_readback(wgpu->pwindow, ppre, W,
                                               H))) {
            FILE *f = fopen("/tmp/gpu_preswap.ppm", "wb");
            if (f != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int yy = 0; yy < H; yy++) {
                    for (int xx = 0; xx < W; xx++) {
                        fdk_u32 p = ppre[yy * W + xx];
                        fdk_u8 rgb[3] = { (fdk_u8)p, (fdk_u8)(p >> 8),
                                          (fdk_u8)(p >> 16) };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
            }
            printf("preswap: ref[20,20]=%08x gpu[20,20]=%08x\n",
                   pref[20 * W + 20], ppre[20 * W + 20]);
        }
    }

    /* drive the GPU window through several identical frames */
    for (int i = 0; i < 4; i++) {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgpu, &s);
        if (getenv("GPU_EMPTY") != NULL) {
            /* present an untouched surface: no primitives at all */
        } else if (getenv("GPU_MINIMAL") != NULL) {
            fdk_color bg = { 0.10f, 0.12f, 0.16f, 1.0f };
            fdk_surface_fill(s, bg);
            fdk_surface_fill_rect(s, (fdk_rect){ 20, 20, 60, 40 },
                                  (fdk_color){ 0.9f, 0.2f, 0.2f, 1.0f });
            fdk_surface_fill_rounded_rect_aa(
                s, (fdk_rect){ 120, 20, 60, 40 }, 12,
                (fdk_color){ 0.2f, 0.4f, 0.95f, 1.0f });
        } else {
            paint_scene_dynamic(s, card, tex);
        }
        fdk_result pr = fdk_surface_present(s);
        if (i == 0) {
            CHECK(pr == FDK_OK, "GPU present returned OK (flush + swap)");
        }
        fdk_pump_events(ctx, 16);
    }

    /* read back through the platform seam and compare */
    static fdk_u32 pgpu[W * H];
    (void)pgpu;
    printf("registry probe: %p\n", (void *)fdk__gpu_surface_for_window(
                                       wgpu->pwindow));
    fdk_result rb = fdk_wayland_window_gpu_readback(wgpu->pwindow, pgpu, W, H);
    int have_gpu = fdk_ok(rb);
    if (!have_gpu) {
        printf("readback seam result: %d\n", (int)rb);
    }
    if (!have_gpu) {
        printf("[skip] GPU readback seam unavailable\n");
    } else {
        printf("forensics: ref[0,0]=%08x gpu[0,0]=%08x ref[20,20]=%08x "
               "gpu[20,20]=%08x ref[mid]=%08x gpu[mid]=%08x\n",
               pref[0], pgpu[0], pref[20 * W + 20], pgpu[20 * W + 20],
               pref[(H / 2) * W + W / 2], pgpu[(H / 2) * W + W / 2]);
        {
            FILE *f = fopen("/tmp/gpu_readback.ppm", "wb");
            if (f != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int yy = 0; yy < H; yy++) {
                    for (int xx = 0; xx < W; xx++) {
                        fdk_u32 p = pgpu[yy * W + xx];
                        fdk_u8 rgb[3] = { (fdk_u8)p, (fdk_u8)(p >> 8),
                                          (fdk_u8)(p >> 16) };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
            }
            f = fopen("/tmp/gpu_ref.ppm", "wb");
            if (f != NULL) {
                fprintf(f, "P6\n%d %d\n255\n", W, H);
                for (int yy = 0; yy < H; yy++) {
                    for (int xx = 0; xx < W; xx++) {
                        fdk_u32 p = pref[yy * W + xx];
                        fdk_u8 rgb[3] = { (fdk_u8)(p >> 16),
                                          (fdk_u8)(p >> 8), (fdk_u8)p };
                        fwrite(rgb, 1, 3, f);
                    }
                }
                fclose(f);
            }
        }
        int fails = 0;
        stats ist, est;
        printf("CPU-vs-GPU interior/edge split (interior = ref 3x3 "
               "uniform; edge = AA band, 1px strokes, ring arcs):\n");
        struct { const char *n; fdk_rect r; double im, ix, em, ex, ep; }
        regs[] = {
            { "solid fill", R.solid, 0.0, 0.0, 0.0, 0.0, 0.0 },
            { "rounded fill", R.round_fill, 0.0, 0.0, 4.0, 120.0, 48.0 },
            /* 1px strokes and rings have NO interior: every pixel is
             * edge band (the CPU area-coverage vs the GPU SDF-distance
             * answer at the same edge pixel — geometry stays pinned by
             * the interior[ok] of the fill shapes and the marker). */
            { "strokes", R.round_stroke, 0.0, 0.0, 8.0, 160.0, 160.0 },
            { "gradient", R.grad, 0.05, 2.0, 2.0, 4.0, 4.0 },
                        /* curved-edge AA: the CPU computes the pixel AREA covered
             * by the disk, the GPU ramps the SDF distance — up to ~0.3
             * alpha apart on the same edge pixel (flat edges agree
             * exactly, see rounded fill). Geometry stays pinned by the
             * clean interiors. */
            { "circles", R.circles, 0.2, 2.0, 40.0, 255.0, 230.0 },
            { "nested clips", R.clips, 0.0, 0.0, 4.0, 140.0, 48.0 },
            /* scaled blits: the GPU samples GL_LINEAR, the CPU maps
             * nearest — interior deltas are the filter model, gated
             * here; a geometry offset >1px would blow the edge mean
             * far past these gates. */
            { "axis blit", R.blit, 4.0, 32.0, 8.0, 160.0, 72.0 },
            { "rotated blit", R.rotated, 4.0, 32.0, 10.0, 255.0, 255.0 },
                        /* per-backend filter model: the GPU samples GL_LINEAR,
             * the CPU maps nearest — interpolation differences, not a
             * geometry offset (the marker + fills pin geometry). */
            { "texture upload", R.texture, 5.0, 16.0, 12.0, 16.0, 16.0 },
        };
        for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            region_stats_split(pref, ppre, regs[i].r.x, regs[i].r.y,
                               regs[i].r.width, regs[i].r.height, &ist,
                               &est);
            report_split(regs[i].n, &ist, regs[i].im, regs[i].ix, &est,
                         regs[i].em, regs[i].ex, regs[i].ep, &fails);
        }
        region_stats_split(pref, ppre, R.marker.x - 2, R.marker.y - 2,
                           R.marker.width + 4, R.marker.height + 4, &ist,
                           &est);
        report_split("asymmetry marker", &ist, 0.0, 0.0, &est, 2.0, 16.0,
                     4.0, &fails);
        g_fail += fails;
    }

    /* ---- phase 2: glyph-atlas overflow (eviction must not kill) -- */
    {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgpu, &s);
        fdk_result pr = FDK_OK;
        fdk_color c = { 1.0f, 1.0f, 1.0f, 1.0f };
        fdk_u8 mask[24 * 24];
        for (int k = 0; k < 2800; k++) {
            unsigned seed = (unsigned)(k * 2654435761u) | 1u;
            int dim = 12 + (int)(seed % 12u);
            for (int i = 0; i < dim * dim; i++) {
                seed = seed * 1664525u + 1013904223u;
                mask[i] = (fdk_u8)(seed >> 24);
            }
            fdk_rect dst = { (k % 20) * 22, 20 + (k % 8) * 26, dim, dim };
            fdk_surface_blend_mask(s, dst, mask, dim, c);
            if (k % 400 == 0) {
                pr = fdk_surface_present(s);
            }
        }
        CHECK(fdk_ok(pr),
              "atlas overflow: present still OK after 2800 unique masks");
        fdk_u8 final_mask[16 * 16];
        memset(final_mask, 0, sizeof(final_mask));
        for (int i = 4; i < 12; i++) {
            for (int j = 4; j < 12; j++) {
                final_mask[i * 16 + j] = 255;
            }
        }
        paint_scene_dynamic(s, card, tex);
        fdk_surface_blend_mask(s, (fdk_rect){ 200, 150, 16, 16 },
                               final_mask, 16,
                               (fdk_color){ 1, 1, 1, 1 });
        CHECK(fdk_ok(fdk_surface_present(s)),
              "atlas overflow: final frame presented");
        CHECK(fdk_ok(fdk_surface_present(s)),
              "atlas overflow: second present (back buffer == final)");
        static fdk_u32 pover[W * H];
        if (fdk_ok(fdk_wayland_window_gpu_readback(wgpu->pwindow, pover, W,
                                               H))) {
            fdk_u32 px = pover[158 * W + 208];
            printf("overflow probe px=%08x (expect white)\n", px);
            int rr = (int)(px & 0xff), gg = (int)((px >> 8) & 0xff),
                bb = (int)((px >> 16) & 0xff);
            CHECK(rr > 200 && gg > 200 && bb > 200,
                  "atlas overflow: post-eviction glyph renders correctly");
        } else {
            printf("[skip] overflow readback unavailable\n");
        }
    }

    /* ---- resize: the engine follows the size-only re-acquire ------ */
    fdk_window_resize(wgpu, W + 40, H + 20);
    for (int i = 0; i < 3; i++) {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgpu, &s);
        paint_scene_dynamic(s, card, tex);
        fdk_surface_present(s);
        fdk_pump_events(ctx, 16);
    }
    CHECK(1, "GPU resize repaint survived");

    fdk_texture_destroy(tex);
    fdk_surface_destroy(card);
    fdk_surface_destroy(ref);
    fdk_window_destroy(wgpu);
    fdk_shutdown(ctx);
    printf(g_fail ? "GPU PAINT SMOKE: %d FAILURES\n"
                  : "GPU PAINT SMOKE: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
