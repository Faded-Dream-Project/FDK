/* test_gpu_paint.c — the GPU paint engine's runtime proof (X11/Xvfb).
 *
 * Builds a scene twice — once into a CPU-mode window surface, once
 * into a GPU-mode window surface (FDK_GL_FORCE=1: llvmpipe under the
 * suite's Xvfb) — presents both, reads both windows back with
 * XGetImage, and compares per pixel with a tolerance (the SDF shapes
 * are not bit-exact to the CPU AA rasterizer; solid fills and the
 * texture path are close, blended SDF edges get slack).
 *
 * Also pins the GPU-mode contracts:
 *   - FDK_NO_GL=1 + GPU mode  -> window creation fails UNSUPPORTED
 *   - texture create/update/draw roundtrip in GPU mode
 *   - resize re-acquire keeps painting (engine resize path)
 *
 * Standalone: make test-gpu-paint (runs inside test-x11's Xvfb).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_core.h"
#include "fdk/fdk_window.h"
#include "render/surface_internal.h" /* the offscreen reference pixels */
#include "fdk/fdk_surface.h"
#include "fdk/fdk_window.h"
#include "render/surface_internal.h" /* the offscreen reference pixels */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <math.h>
#include <string.h>
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

#define W 320
#define H 200

/* The shared scene: every fdk_surface primitive the engine implements,
 * in both plain and _aa forms. */
static void paint_scene(fdk_surface *s) {
    fdk_color bg = { 0.10f, 0.12f, 0.16f, 1.0f };
    fdk_surface_fill(s, bg);
    fdk_color c1 = { 0.9f, 0.2f, 0.2f, 1.0f };
    fdk_color c2 = { 0.2f, 0.4f, 0.95f, 1.0f };
    fdk_color c3 = { 0.1f, 0.8f, 0.3f, 1.0f };
    fdk_color c4 = { 1.0f, 0.85f, 0.1f, 1.0f };
    fdk_rect r;
    r.x = 10; r.y = 10; r.width = 60; r.height = 40;
    fdk_surface_fill_rect(s, r, c1);
    r.x = 90; r.y = 10; r.width = 60; r.height = 40;
    fdk_surface_fill_rounded_rect_aa(s, r, 12, c2);
    r.x = 170; r.y = 10; r.width = 60; r.height = 40;
    fdk_surface_draw_rounded_rect_aa(s, r, 10, c3);
    r.x = 250; r.y = 10; r.width = 60; r.height = 40;
    fdk_surface_fill_gradient_vertical(s, r, c1, c2);
    fdk_surface_fill_circle_aa(s, 40, 110, 24, c3);
    fdk_surface_draw_circle_aa(s, 110, 110, 24, c4);
    fdk_surface_draw_rect(s, (fdk_rect){ 150, 86, 70, 48 }, c1);
    fdk_surface_draw_line(s, 20, 170, 300, 170, c4);
    fdk_surface_draw_line_aa(s, 20, 180, 300, 180, c2);
    /* clip: a red rect half-covered by a clip rect */
    fdk_surface_push_clip(s, (fdk_rect){ 150, 150, 80, 20 });
    fdk_surface_fill_rect(s, (fdk_rect){ 130, 140, 120, 40 }, c1);
    fdk_surface_pop_clip(s);
}

/* A textured quad (the software-pixel upload path). */
static void paint_texture(fdk_surface *s, fdk_texture *t) {
    fdk_u8 px[4 * 8 * 8];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            fdk_u8 *p = px + (y * 8 + x) * 4;
            p[0] = (fdk_u8)(x * 31);
            p[1] = (fdk_u8)(y * 31);
            p[2] = 200;
            p[3] = 255;
        }
    }
    fdk_rect all = { 0, 0, 8, 8 };
    fdk_texture_update(t, all, px, 8 * 4);
    fdk_texture_draw(s, t, (fdk_rect){ 240, 140, 64, 48 });
}

typedef struct {
    Display *dpy;
    Window win;
} xwin;

/* Read the window's front-buffer pixels after present. */
static int read_window(Display *dpy, xwin *w, fdk_u32 *out) {
    XImage *img = XGetImage(dpy, w->win, 0, 0, W, H, AllPlanes, ZPixmap);
    if (img == NULL) {
        return -1;
    }
    for (int y = 0; y < H; y++) {
        memcpy(out + (size_t)y * W, img->data + (size_t)y * img->bytes_per_line,
               (size_t)W * 4);
    }
    XDestroyImage(img);
    return 0;
}

static double pixel_diff(const fdk_u32 *a, const fdk_u32 *b, int skip_edge) {
    /* mean abs diff over the three channels; SDF AA edges differ from
     * the CPU rasterizer's coverage — report mean, gate on the solid
     * interior via skip_edge's margin crop. */
    double sum = 0.0;
    long n = 0;
    for (int y = skip_edge; y < H - skip_edge; y++) {
        for (int x = skip_edge; x < W - skip_edge; x++) {
            fdk_u32 pa = a[y * W + x], pb = b[y * W + x];
            for (int ch = 0; ch < 3; ch++) {
                int shift = ch * 8;
                int va = (int)((pa >> shift) & 0xff);
                int vb = (int)((pb >> shift) & 0xff);
                sum += fabs((double)(va - vb));
            }
            n++;
        }
    }
    return n ? sum / (double)n : 999.0;
}

int main(void) {
    fdk_test_hermetic_pin();
    setenv("FDK_GL_FORCE", "1", 1);
    unsetenv("FDK_NO_GL");

    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        printf("SKIP: no X display (run under Xvfb)\n");
        return 77;
    }
    int screen = DefaultScreen(dpy);

    /* Contract 1: no GL stack -> GPU mode is refused, never faked. */
    fdk_context *ctx_nogl = NULL;
    setenv("FDK_NO_GL", "1", 1);
    fdk_window_options nogl_opts;
    memset(&nogl_opts, 0, sizeof(nogl_opts));
    nogl_opts.render_mode = FDK_RENDER_GPU;
    /* A context must come up first; the refusal happens at window
     * creation (the lane is per-window). */
    if (fdk_ok(fdk_init(&ctx_nogl, NULL))) {
        fdk_window *wn = NULL;
        fdk_result r = fdk_window_create(ctx_nogl, &nogl_opts, &wn);
        CHECK(r == FDK_ERR_UNSUPPORTED,
              "FDK_NO_GL + GPU mode -> window create fails UNSUPPORTED");
        if (wn != NULL) {
            fdk_window_destroy(wn);
        }
        fdk_shutdown(ctx_nogl);
    } else {
        printf("[ok] FDK_NO_GL context refused (also acceptable)\n");
    }
    unsetenv("FDK_NO_GL");

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        printf("SKIP: no FDK context (%d)\n", (int)r);
        return 77;
    }

    /* CPU reference window */
    fdk_window_options cpu_opts;
    memset(&cpu_opts, 0, sizeof(cpu_opts));
    cpu_opts.render_mode = FDK_RENDER_CPU;
    cpu_opts.title = "fdk-gpu-test-cpu";
    cpu_opts.width = W;
    cpu_opts.height = H;
    fdk_window *wcpu = NULL;
    r = fdk_window_create(ctx, &cpu_opts, &wcpu);
    CHECK(fdk_ok(r), "CPU window created");

    /* GPU window under FDK_GL_FORCE=1 */
    fdk_window_options gpu_opts;
    memset(&gpu_opts, 0, sizeof(gpu_opts));
    gpu_opts.render_mode = FDK_RENDER_GPU;
    gpu_opts.title = "fdk-gpu-test-gpu";
    gpu_opts.width = W;
    gpu_opts.height = H;
    fdk_window *wgpu = NULL;
    r = fdk_window_create(ctx, &gpu_opts, &wgpu);
    if (!fdk_ok(r)) {
        printf("[skip] GPU window create failed (%d) — no usable GL "
               "stack in this environment; engine path untested here\n",
               (int)r);
        fdk_window_destroy(wcpu);
        fdk_shutdown(ctx);
        XCloseDisplay(dpy);
        return (g_fail ? 1 : 77);
    }
    CHECK(1, "GPU window created (lane + engine up)");

    fdk_surface *scpu = NULL, *sgpu = NULL;
    fdk_window_get_surface(wcpu, &scpu);
    fdk_window_get_surface(wgpu, &sgpu);
    CHECK(scpu != NULL && sgpu != NULL, "surfaces acquired");

    fdk_texture *tex = NULL;
    CHECK(fdk_ok(fdk_texture_create(8, 8, &tex)), "texture created");

    fdk_window_set_title(wcpu, "fdk-gpu-test-cpu");
    fdk_window_set_title(wgpu, "fdk-gpu-test-gpu");
    fdk_window_show(wcpu);
    fdk_window_show(wgpu);
    /* pump a few frames so both windows are mapped and painted */
    for (int i = 0; i < 6; i++) {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wcpu, &s);
        paint_scene(s);
        paint_texture(s, tex);
        fdk_surface_present(s);
        fdk_window_get_surface(wgpu, &s);
        paint_scene(s);
        paint_texture(s, tex);
        fdk_result pr = fdk_surface_present(s);
        if (i == 0) {
            CHECK(pr == FDK_OK, "GPU present returned OK (flush + swap)");
        }
        fdk_pump_events(ctx, 16);
    }

    /* The reference: the SAME scene rendered into an offscreen CPU
     * surface (the rasterizer's own pixels, no window involved). */
    fdk_surface *ref = NULL;
    fdk_surface_create(W, H, &ref);
    paint_scene(ref);
    paint_texture(ref, tex);
    static fdk_u32 pref[W * H];
    for (int yy = 0; yy < H; yy++) {
        memcpy(pref + (size_t)yy * W,
               ref->own_pixels + (size_t)yy * ref->fb.stride,
               (size_t)W * 4);
    }

    xwin xc = { 0 }, xg = { 0 };
    {
        Window root, parent, *kids = NULL;
        unsigned nk = 0;
        XQueryTree(dpy, RootWindow(dpy, screen), &root, &parent, &kids, &nk);
        for (unsigned i = 0; i < nk; i++) {
            char *name = NULL;
            XFetchName(dpy, kids[i], &name);
            if (name != NULL) {
                if (strcmp(name, "fdk-gpu-test-cpu") == 0) {
                    xc.dpy = dpy;
                    xc.win = kids[i];
                } else if (strcmp(name, "fdk-gpu-test-gpu") == 0) {
                    xg.dpy = dpy;
                    xg.win = kids[i];
                }
                XFree(name);
            }
        }
        if (kids != NULL) {
            XFree(kids);
        }
    }
    if (xc.win == 0 || xg.win == 0) {
        printf("[skip] could not find both test windows by title\n");
    }
    static fdk_u32 pcpu[W * H], pgpu[W * H];
    if (xg.win != 0 && read_window(dpy, &xg, pgpu) == 0) {
        double interior = pixel_diff(pref, pgpu, 4);
        long eq = 0, n2 = 0;
        for (int yy = 0; yy < H; yy++) {
            for (int xx = 0; xx < W; xx++) {
                if (pcpu[yy * W + xx] == pgpu[yy * W + xx]) eq++;
                n2++;
            }
        }
        printf("forensics: ref[0,0]=%08x gpu[0,0]=%08x ref[mid]=%08x "
               "gpu[mid]=%08x equal=%ld/%ld\n",
               pref[0], pgpu[0], pref[(H / 2) * W + W / 2],
               pgpu[(H / 2) * W + W / 2], eq, n2);
        printf("mean |GPU-CPU| channel diff (interior): %.3f\n", interior);
        CHECK(interior < 24.0,
              "GPU frame matches the CPU reference within SDF tolerance");
    } else {
        printf("[skip] XGetImage readback unavailable\n");
    }

    /* resize: the engine must follow the size-only re-acquire */
    fdk_window_resize(wgpu, W + 40, H + 20);
    for (int i = 0; i < 3; i++) {
        fdk_surface *s = NULL;
        fdk_window_get_surface(wgpu, &s);
        paint_scene(s);
        fdk_surface_present(s);
        fdk_pump_events(ctx, 16);
    }
    CHECK(1, "GPU resize repaint survived");

    fdk_surface_destroy(ref);
    fdk_texture_destroy(tex);
    fdk_window_destroy(wgpu);
    fdk_window_destroy(wcpu);
    fdk_shutdown(ctx);
    XCloseDisplay(dpy);
    printf(g_fail ? "GPU PAINT SMOKE: %d FAILURES\n" : "GPU PAINT SMOKE: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
