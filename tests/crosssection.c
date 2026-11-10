/* crosssection.c — P1 close-out evidence: dump a 1px-wide CPU-vs-GPU
 * coverage cross-section for ONE circle edge and ONE rotated edge and
 * decide offset-vs-AA-model from the numbers.
 *
 * Method: paint the same shape into a CPU offscreen surface (the
 * reference rasterizer) and into a GPU window's surface, read both
 * back, take one scanline through the shape's edge, and print the
 * per-pixel "coverage" (alpha-distance from the flat interior color,
 * 0..255) side by side. Then measure, for each row, the shift s in
 * [-2, 2] that minimizes the sum of |cpu[x] - gpu[x+s]|: a half-pixel
 * or one-pixel geometry offset shows up as the best shift being
 * nonzero AND driving the residual to ~0 at that shift; a pure AA-model
 * difference (area coverage vs distance ramp) shows best shift 0 with
 * a small residual that does not vanish at any shift. */
#include "fdk/fdk.h"
#include "fdk/fdk_window.h"
#include "platform/x11/x11_platform.h" /* the readback seam */
#include "window/window_internal.h"    /* w->pwindow for the seam */
#include "render/gpaint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XW 640
#define XH 480
static fdk_u32 g_gpu_buf[XW * XH];

static const fdk_u32 *grab_gpu(fdk_window *w) {
    if (fdk_ok(fdk_x11_window_gpu_readback(w->pwindow, g_gpu_buf, XW,
                                           XH))) {
        return g_gpu_buf;
    }
    return NULL;
}

static double cov(fdk_u32 px, fdk_u32 flat) {
    /* max per-channel delta from the flat interior color, inverted to
     * a 0..255 "how far from interior" measure — the edge profile. */
    long d = 0;
    for (int ch = 0; ch < 3; ch++) {
        long dd = labs((long)((px >> (ch * 8)) & 0xff) -
                       (long)((flat >> (ch * 8)) & 0xff));
        if (dd > d) {
            d = dd;
        }
    }
    return (double)d;
}

static int g_cst;

static void dump_row(const char *name, const fdk_u32 *cpu,
                     const fdk_u32 *gpu, int w, int y, int x0, int x1,
                     fdk_u32 flat) {
    printf("== %s (row y=%d) ==\ncpu:", name, y);
    for (int x = x0; x <= x1; x++) {
        printf(" %5.0f", cov(cpu[y * g_cst + x], flat));
    }
    printf("\ngpu:");
    for (int x = x0; x <= x1; x++) {
        printf(" %5.0f", cov(gpu[y * w + x], flat));
    }
    printf("\n");
}

static void analyze(const char *name, const fdk_u32 *cpu,
                    const fdk_u32 *gpu, int w, int y, int x0, int x1,
                    fdk_u32 flat) {
    (void)w;
    double best[5];
    for (int s = -2; s <= 2; s++) {
        double sum = 0;
        int n = 0;
        for (int x = x0; x <= x1; x++) {
            int xs = x + s;
            if (xs < 0 || xs >= w) {
                continue;
            }
            double cc = cov(cpu[y * g_cst + x], flat);
            double gg = cov(gpu[y * w + xs], flat);
            sum += cc > gg ? cc - gg : gg - cc;
            n++;
        }
        best[s + 2] = n ? sum / n : 0;
    }
    int bs = 0;
    for (int i = 1; i < 5; i++) {
        if (best[i] < best[bs + 2]) {
            bs = i - 2;
        }
    }
    printf("== %s shift analysis ==\n", name);
    for (int s = -2; s <= 2; s++) {
        printf("  shift %+d: mean |cpu-gpu| = %6.2f\n", s, best[s + 2]);
    }
    printf("  verdict: best shift %+d (residual %.2f vs %.2f at 0) -> %s\n",
           bs, best[bs + 2], best[2],
           bs == 0
               ? "NO GEOMETRY OFFSET — the difference is the AA model"
               : "GEOMETRY OFFSET DETECTED — fix the emitter/shader");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("no context\n");
        return 77;
    }
    /* GPU window: the lane decides GLX vs EGL; both were proven. */
    fdk_window_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.render_mode = FDK_RENDER_GPU;
    opts.title = "fdk-crosssection";
    opts.width = 640;
    opts.height = 480;
    fdk_window *w = NULL;
    fdk_result r = fdk_window_create(ctx, &opts, &w);
    if (!fdk_ok(r)) {
        printf("SKIP: no GPU lane (%d)\n", (int)r);
        return 77;
    }
    fdk_window_show(w);
    for (int i = 0; i < 3; i++) {
        fdk_pump_events(ctx, 16);
    }

    /* Scene 1: one circle, center (320,240), r=120 — scanline y=240.
     * Scene 2: one rotated quad (the blit_transformed emitter, 45deg)
     * whose diamond edge crosses y=240 near x=450 — scanline y=240. */
    fdk_surface *s = NULL;
    fdk_window_get_surface(w, &s);
    fdk_surface_fill(s, (fdk_color){ 1, 1, 1, 1 });
    fdk_color red = { 0.8f, 0.1f, 0.1f, 1.0f };
    /* isolation probes: a rect (the proven path) and a green circle;
     * plus 60 padding quads to force the normal batch drain path */
    for (int i = 0; i < 60; i++) {
        fdk_surface_fill_rect(s, (fdk_rect){ 560 + (i % 10) * 6,
                                             10 + (i / 10) * 6, 4, 4 },
                              (fdk_color){ 0.5f, 0.5f, 0.5f, 1.0f });
    }
    fdk_surface_fill_rect(s, (fdk_rect){ 20, 20, 40, 40 }, red);
    fdk_surface_fill_circle_aa(s, 100, 40, 16,
                               (fdk_color){ 0.1f, 0.8f, 0.3f, 1.0f });
    fdk_surface_fill_circle_aa(s, 320, 240, 120, red);
    /* the rotated edge: a 64x48 checker card, 45deg, center (450,240) */
    fdk_surface *t = NULL;
    fdk_surface_create(64, 48, &t);
    for (int yy = 0; yy < 48; yy += 8) {
        for (int xx = 0; xx < 64; xx += 8) {
            fdk_color c = ((xx / 8 + yy / 8) & 1)
                              ? (fdk_color){ 0.63f, 0.13f, 0.13f, 1.0f }
                              : (fdk_color){ 0.63f, 0.63f, 0.63f, 1.0f };
            fdk_surface_fill_rect(t, (fdk_rect){ xx, yy, 8, 8 }, c);
        }
    }
    float a = 3.14159265f / 4.0f;
    fdk_matrix rot = { 0.7071f, -0.7071f, 0, 0.7071f, 0.7071f, 0 };
    fdk_matrix tr = { 1, 0, 450.0f - 32.0f, 0, 1, 240.0f - 24.0f };
    fdk_matrix m = fdk_matrix_mul(rot, tr);
    fdk_surface_blit_transformed(s, m, t);
    fdk_surface_present(s);
    for (int i = 0; i < 2; i++) {
        fdk_pump_events(ctx, 16);
    }
    const fdk_u32 *gpu = grab_gpu(w);
    if (gpu == NULL) {
        printf("SKIP: readback seam unavailable\n");
        return 77;
    }

    /* the CPU reference: same shapes into an offscreen surface */
    fdk_surface *off = NULL;
    fdk_surface_create(640, 480, &off);
    fdk_surface_fill(off, (fdk_color){ 1, 1, 1, 1 });
    fdk_surface_fill_circle_aa(off, 320, 240, 120, red);
    fdk_surface_blit_transformed(off, m, t);
    static fdk_u32 cref[640 * 480];
    for (int y = 0; y < 480; y++) {
        const fdk_u32 *row = off->own_pixels + (size_t)y * off->fb.stride;
        for (int x = 0; x < 640; x++) {
            fdk_u32 p = row[x];
            cref[y * 640 + x] = 0xff000000u | ((p & 0xffu) << 16) |
                                (p & 0xff00u) | ((p >> 16) & 0xffu);
        }
    }
    const int cst = 640; /* cref is packed 640-wide */
    g_cst = cst;
    fdk_u32 flat = cref[240 * cst + 320]; /* interior */

    printf("probe rect:  cpu=(%u,%u,%u,%u) gpu=(%u,%u,%u,%u)\n",
           cref[40 * cst + 40] >> 16 & 0xff,
           cref[40 * cst + 40] >> 8 & 0xff,
           cref[40 * cst + 40] & 0xff, 0,
           gpu[40 * 640 + 40] >> 16 & 0xff, gpu[40 * 640 + 40] >> 8 & 0xff,
           gpu[40 * 640 + 40] & 0xff, 0);
    printf("probe circle: cpu=(%u,%u,%u,%u) gpu=(%u,%u,%u,%u)\n",
           cref[40 * cst + 100] >> 16 & 0xff,
           cref[40 * cst + 100] >> 8 & 0xff,
           cref[40 * cst + 100] & 0xff, 0,
           gpu[40 * 640 + 100] >> 16 & 0xff, gpu[40 * 640 + 100] >> 8 & 0xff,
           gpu[40 * 640 + 100] & 0xff, 0);
    printf("raw: cpu=(%u,%u,%u,%u) gpu=(%u,%u,%u,%u)\n",
           cref[240 * cst + 300] >> 16 & 0xff,
           cref[240 * cst + 300] >> 8 & 0xff,
           cref[240 * cst + 300] & 0xff,
           cref[240 * cst + 300] >> 24,
           gpu[240 * 640 + 300] >> 16 & 0xff,
           gpu[240 * 640 + 300] >> 8 & 0xff, gpu[240 * 640 + 300] & 0xff,
           gpu[240 * 640 + 300] >> 24);
    dump_row("CIRCLE cross-section x=195..215 (r=120 edge at x=200)",
             cref, gpu, 640, 240, 195, 215, flat);
    analyze("CIRCLE", cref, gpu, 640, 240, 190, 220, flat);
    /* the diamond's left edge near x=450-64/sqrt(2)... find the edge by
     * scanning for the first nonzero cpu coverage on the row */
    int ex = 380;
    for (int x = 380; x < 520; x++) {
        if (cov(cref[240 * cst + x], 0x00ffffffu) > 0) {
            ex = x;
            break;
        }
    }
        /* the 45-degree arc: edge at center + r/sqrt(2) up-right */
    {
        int ay = 240 - 85;
        int ax = 320 + 85;
        printf("== CIRCLE 45deg arc y=%d, x=%d..%d ==\n", ay, ax - 8,
               ax + 8);
        printf("cpu:");
        for (int x = ax - 8; x <= ax + 8; x++) {
            printf(" %5.0f", cov(cref[ay * 640 + x], flat));
        }
        printf("\ngpu:");
        for (int x = ax - 8; x <= ax + 8; x++) {
            printf(" %5.0f", cov(gpu[ay * 640 + x], flat));
        }
        printf("\n");
        analyze("CIRCLE 45deg arc", cref, gpu, 640, ay, ax - 10, ax + 10,
                flat);
    }
    printf("== ROTATED cross-section x=%d..%d (edge at x=%d) ==\n", ex - 2,
           ex + 18, ex);
    printf("cpu:");
    for (int x = ex - 2; x <= ex + 18; x++) {
        printf(" %5.0f", cov(cref[240 * cst + x], 0x00ffffffu));
    }
    printf("\ngpu:");
    for (int x = ex - 2; x <= ex + 18; x++) {
        printf(" %5.0f", cov(gpu[240 * 640 + x], 0x00ffffffu));
    }
    printf("\n");
    analyze("ROTATED", cref, gpu, 640, 240, ex - 4, ex + 20,
            0x00ffffffu);
    return 0;
}
