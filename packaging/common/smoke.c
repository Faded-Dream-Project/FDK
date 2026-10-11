/*
 * smoke.c — FDK binary-package smoke test (public API only).
 *
 * Compiled and run by the packaging install-tests against an
 * INSTALLED FDK (found through pkg-config, loaded from the system
 * library paths, themed from the system data dir). It exercises:
 *
 *   - shared-library load + runtime version agreement
 *   - display connection (a real X11 server under Xvfb)
 *   - window creation, root box, label, theme-driven paint
 *   - the event pump, render-path introspection
 *   - clean teardown in the documented order
 *
 * Usage: fdk_smoke [expected-version]
 * Exit status 0 = pass. Any failure exits nonzero with a message on
 * stderr, naming the stage that failed.
 */

#include <stdio.h>
#include <string.h>

#include "fdk/fdk.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"
#include "fdk/fdk_layout.h"
#include "fdk/fdk_widgets.h"

int main(int argc, char **argv) {
    const char *expected = argc > 1 ? argv[1] : NULL;

    /* 1. Library + version agreement: catches a payload/soname mix-up
     * (e.g. an old libfdk.so.1 still installed under the new package). */
    const char *v = fdk_get_version_string();
    printf("SMOKE version=%s\n", v);
    if (expected && strcmp(v, expected) != 0) {
        fprintf(stderr, "SMOKE FAIL: runtime version %s != expected %s\n",
                v, expected);
        return 1;
    }

    /* 2. Real display connection. */
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "SMOKE FAIL: fdk_init (no usable display?)\n");
        return 1;
    }

    /* 3. Window + layout + label: drives theme load (title band +
     * widget paint) and font discovery in one shot. */
    fdk_window *window = NULL;
    fdk_window_options opts = {0};
    opts.title = "FDK package smoke test";
    opts.width = 420;
    opts.height = 280;
    if (!fdk_ok(fdk_window_create(ctx, &opts, &window))) {
        fprintf(stderr, "SMOKE FAIL: fdk_window_create\n");
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_widget *root = NULL;
    fdk_widget *box = NULL;
    fdk_widget *label = NULL;
    int staged = 0;
    if (!fdk_ok(fdk_window_get_root(window, &root))) {
        fprintf(stderr, "SMOKE FAIL: fdk_window_get_root\n");
        goto teardown;
    }
    if (!fdk_ok(fdk_box_create(root, FDK_VERTICAL, &box))) {
        fprintf(stderr, "SMOKE FAIL: fdk_box_create\n");
        goto teardown;
    }
    fdk_window_set_content(window, box);
    {
        char text[96];
        snprintf(text, sizeof text, "FDK %s — package smoke test", v);
        if (!fdk_ok(fdk_label_create(box, NULL, text, &label))) {
            fprintf(stderr, "SMOKE FAIL: fdk_label_create (font discovery?)\n");
            goto teardown;
        }
    }
    staged = 1;

    /* 4. Map + pump a few frames: every pump renders and presents the
     * window through the installed backend. */
    fdk_window_show(window);
    for (int i = 0; i < 25; i++) {
        fdk_pump_events(ctx, 20);
    }

    /* 5. Render-path introspection (observation only). */
    {
        fdk_render_path path = FDK_RENDER_PATH_CPU;
        if (fdk_ok(fdk_window_get_render_path(window, &path))) {
            printf("SMOKE render=%s\n",
                   path == FDK_RENDER_PATH_GL ? "gl" : "cpu");
        }
    }

teardown:
    /* 6. Documented teardown order: window, then context. */
    if (window) {
        fdk_window_destroy(window);
    }
    fdk_shutdown(ctx);
    if (!staged) {
        return 1;
    }
    printf("SMOKE PASS\n");
    return 0;
}
