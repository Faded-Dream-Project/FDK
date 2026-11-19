/* test_gl_viewport.c — 1.0.0 fdk_gl.h smoke: the reason enum's honest
 * answers, explicit engage on a SMALL window (the size-gate override),
 * the identity query, and the widget's place in a tree. Headless-safe:
 * without a display the NULL-contract legs still run and pass.
 * Under Xvfb+FDK_GL_FORCE the engage succeeds and the identity reads.
 * Also pins the public-vs-platform reason enum equality (ABI). */
#include "fdk/fdk.h"
#include "fdk/fdk_gl.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

#include "platform/platform_internal.h"

static int fails;
static void check(int cond, const char *what) {
    printf("[glv] %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

static int render_calls;
/* R-003's pixel proof: the callback clears to a color far from every
 * FDK theme/CPU-fill tone (the widget's fallback box is 0.06/0.07/
 * 0.09 dark blue-gray -> 0x0f1217). If the composite works, this
 * color MUST be readable from the window's CPU surface after a
 * present. GL entry points resolve through dlopen, the examples'
 * pattern (gl_viewport_3d.c): no GL headers, no link dependency —
 * the lane context is current when the callback runs. */
#define GLV_TEST_R 0.20f
#define GLV_TEST_G 0.80f
#define GLV_TEST_B 0.30f
static void (*t_glClearColor)(float, float, float, float);
static void (*t_glClear)(unsigned);
static void *t_gl_handle;
static void on_render(fdk_widget *vp, int w, int h, fdk_f32 s, void *u) {
    (void)vp; (void)w; (void)h; (void)s; (void)u;
    render_calls++;
    if (t_glClearColor == NULL) {
        t_gl_handle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (t_gl_handle == NULL) {
            t_gl_handle = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
        }
        if (t_gl_handle != NULL) {
            /* dlsym's void* -> function pointer: the union carry
             * (ISO-pedantic-safe, the codebase's standing idiom). */
            union { void *obj; void (*fn)(float, float, float, float); } u_cc;
            union { void *obj; void (*fn)(unsigned); } u_cl;
            u_cc.obj = dlsym(t_gl_handle, "glClearColor");
            u_cl.obj = dlsym(t_gl_handle, "glClear");
            t_glClearColor = u_cc.fn;
            t_glClear = u_cl.fn;
        }
    }
    if (t_glClearColor != NULL && t_glClear != NULL) {
        t_glClearColor(GLV_TEST_R, GLV_TEST_G, GLV_TEST_B, 1.0f);
        t_glClear(0x00004000u /* GL_COLOR_BUFFER_BIT */);
    }
}

int main(void) {
    /* ABI pin: the public enum and the platform mirror agree. */
    check((int)FDK_GL_REASON_OK == (int)FDK_PLATFORM_GL_REASON_OK &&
          (int)FDK_GL_REASON_NOT_YET == (int)FDK_PLATFORM_GL_REASON_NOT_YET &&
          (int)FDK_GL_REASON_SIZE_GATE == (int)FDK_PLATFORM_GL_REASON_SIZE_GATE &&
          (int)FDK_GL_REASON_CPU_MODE == (int)FDK_PLATFORM_GL_REASON_CPU_MODE &&
          (int)FDK_GL_REASON_DRIVER == (int)FDK_PLATFORM_GL_REASON_DRIVER &&
          (int)FDK_GL_REASON_BACKEND == (int)FDK_PLATFORM_GL_REASON_BACKEND &&
          (int)FDK_GL_REASON_DOWNGRADED == (int)FDK_PLATFORM_GL_REASON_DOWNGRADED,
          "reason enum: public == platform mirror (ABI pin)");

    /* NULL-window contracts. */
    fdk_gl_status_data st;
    check(fdk_ok(fdk_gl_status(NULL, &st)) && st.reason == FDK_GL_REASON_BACKEND,
          "fdk_gl_status(NULL) -> BACKEND, no crash");
    fdk_gl_info info;
    check(fdk_ok(fdk_window_gl_info(NULL, &info)) &&
          strcmp(info.flavor, "none") == 0,
          "fdk_window_gl_info(NULL) -> flavor none");
    check(fdk_window_gl_engage(NULL) == FDK_ERR_INVALID_ARGUMENT,
          "fdk_window_gl_engage(NULL) -> INVALID_ARGUMENT");

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        /* Headless: the NULL-contract legs above already passed; the
         * backend legs are covered by the Xvfb run (test-examples
         * pattern — a bare `make test` box has no display). */
        printf("[glv] no context — headless: NULL contracts passed\n");
        return fails ? 1 : 0;
    }
    fdk_window_options o;
    memset(&o, 0, sizeof o);
    o.title = "glv-test";
    o.width = 300;   /* SMALL: below the 131072-px gate on purpose */
    o.height = 180;
    fdk_window *w = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &o, &w)) || w == NULL) {
        printf("[glv] no window — headless: NULL contracts passed\n");
        fdk_shutdown(ctx);
        return fails ? 1 : 0;
    }
    fdk_window_show(w); /* map + expose -> the paint cycle runs */
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(w, &root);
    fdk_widget *vp = NULL;
    check(fdk_ok(fdk_gl_viewport_create(root, on_render, NULL, &vp)) &&
          vp != NULL, "fdk_gl_viewport_create");
    fdk_gl_viewport_set_render_callback(vp, on_render, NULL);
    /* Give the viewport a real region: the root spans the window
     * content; direct bounds keep the test independent of any
     * container layout policy. */
    fdk_widget_set_bounds(vp, (fdk_rect){ 10, 10, 200, 120 });
    fdk_gl_viewport_request_frame(vp);
    check(fdk_gl_viewport_status(NULL, NULL) == FDK_ERR_INVALID_ARGUMENT,
          "viewport_status(NULL) -> INVALID_ARGUMENT");
    fdk_gl_status_data vs;
    check(fdk_ok(fdk_gl_viewport_status(vp, &vs)),
          "viewport_status on a live viewport");

    /* The status BEFORE engage: NOT_YET (or a terminal reason on a
     * GL-less box) — never a crash, always named. */
    fdk_gl_status_data st2;
    (void)fdk_gl_status(w, &st2);
    printf("[glv] pre-engage reason = %d (area %lld gate %lld force %d)\n",
           (int)st2.reason, (long long)st2.pixel_area,
           (long long)st2.size_gate, st2.force_override);
    check(st2.reason == FDK_GL_REASON_NOT_YET ||
          st2.reason == FDK_GL_REASON_SIZE_GATE ||
          st2.reason == FDK_GL_REASON_DRIVER ||
          st2.reason == FDK_GL_REASON_BACKEND ||
          st2.reason == FDK_GL_REASON_CPU_MODE,
          "pre-engage status names a reason (the old gap: bare -6)");

    /* Explicit engage: allowed to fail with a REASON; succeeds under
     * Xvfb with FDK_GL_FORCE=1 (the size gate is overridden — the
     * window is small ON PURPOSE). */
    fdk_result r = fdk_window_gl_engage(w);
    fdk_gl_status_data st3;
    (void)fdk_gl_status(w, &st3);
    printf("[glv] engage -> %d, reason now %d\n", (int)r, (int)st3.reason);
    check(fdk_ok(r) ? st3.reason == FDK_GL_REASON_OK : st3.reason != FDK_GL_REASON_OK,
          "engage result and status agree");
    if (fdk_ok(r)) {
        fdk_gl_info gi;
        (void)fdk_window_gl_info(w, &gi);
        printf("[glv] flavor=%s %d.%d vendor=%s renderer=%s\n",
               gi.flavor, gi.version_major, gi.version_minor,
               gi.vendor, gi.renderer);
        check(gi.version_major >= 3 ||
              strcmp(gi.flavor, "compat") == 0 ||
              strcmp(gi.flavor, "gles2") == 0,
              "identity sane (core>=3 or honest compat/gles2)");
    }
    /* R-003 (Halcyon): the composite pixel proof. The 1.0.0 build
     * drew the GL content into the back buffer where the present's
     * fullscreen CPU quad covered it — the widget was NEVER visible.
     * Now the region is read back and blitted in paint order: after
     * one presented frame the window's CPU surface must carry the
     * callback's clear color inside the region (and the CPU fill
     * color OUTSIDE it when a lane is engaged). Runs only when the
     * lane engaged AND a present actually happened; headless boxes
     * keep passing through the legs above. */
    if (fdk_ok(r)) {
        /* Drive real paint cycles first: the expose/map handshake
         * and the widget walk both happen inside the pump (the
         * request_frame flag is consumed by the NEXT paint). A few
         * rounds ride Xvfb's map timing. */
        for (int i = 0; i < 6; i++) {
            (void)fdk_pump_events(ctx, 100);
        }
        check(render_calls > 0, "R-003 leg: the render callback ran "
              "(paint cycle reached the viewport)");
        fdk_surface *sf = NULL;
        if (fdk_ok(fdk_window_get_surface(w, &sf)) && sf != NULL) {
            fdk_gl_viewport_request_frame(vp);
            for (int i = 0; i < 3; i++) {
                (void)fdk_pump_events(ctx, 100);
            }
            fdk_surface_info si;
            if (fdk_ok(fdk_surface_get_info(sf, &si)) &&
                si.pixels != NULL) {
                fdk_surface_present(sf);
                /* Re-acquire (the present may rotate buffers) and
                 * inspect the region's center. */
                if (fdk_ok(fdk_surface_get_info(sf, &si)) &&
                    si.pixels != NULL && si.width > 4 && si.height > 4) {
                    const fdk_u32 *px = si.pixels +
                        (size_t)(si.height / 2) * (size_t)si.stride +
                        (size_t)(si.width / 2);
                    fdk_u32 got = *px & 0x00FFFFFFu;
                    fdk_u32 want = ((fdk_u32)(GLV_TEST_R * 255.0f + 0.5f)
                                    << 16) |
                                   ((fdk_u32)(GLV_TEST_G * 255.0f + 0.5f)
                                    << 8) |
                                   (fdk_u32)(GLV_TEST_B * 255.0f + 0.5f);
                    printf("[glv] composite center pixel: got 0x%06x "
                           "want 0x%06x (render_calls=%d)\n",
                           (unsigned)got, (unsigned)want, render_calls);
                    /* +-1 per channel: the float->byte rounding of the
                     * clear and the readback's channel repack may
                     * differ by one ULP; anything more is a wrong
                     * composite (the CPU fill is 0x0f1217, an
                     * entirely different color). */
                    int dr = (int)(got >> 16 & 0xFF) -
                             (int)(want >> 16 & 0xFF);
                    int dg = (int)(got >> 8 & 0xFF) -
                             (int)(want >> 8 & 0xFF);
                    int db = (int)(got & 0xFF) - (int)(want & 0xFF);
                    check(dr >= -1 && dr <= 1 && dg >= -1 && dg <= 1 &&
                          db >= -1 && db <= 1,
                          "R-003: GL region composites into the frame "
                          "(paint order)");
                }
            }
        }
    }

    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    printf("[glv] %s\n", fails ? "FAILURES" : "all pass");
    return fails ? 1 : 0;
}
