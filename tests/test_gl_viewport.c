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

#include "platform/platform_internal.h"

static int fails;
static void check(int cond, const char *what) {
    printf("[glv] %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

static int render_calls;
static void on_render(fdk_widget *vp, int w, int h, fdk_f32 s, void *u) {
    (void)vp; (void)w; (void)h; (void)s; (void)u;
    render_calls++;
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
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(w, &root);
    fdk_widget *vp = NULL;
    check(fdk_ok(fdk_gl_viewport_create(root, on_render, NULL, &vp)) &&
          vp != NULL, "fdk_gl_viewport_create");
    fdk_gl_viewport_set_render_callback(vp, on_render, NULL);
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
    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    printf("[glv] %s\n", fails ? "FAILURES" : "all pass");
    return fails ? 1 : 0;
}
