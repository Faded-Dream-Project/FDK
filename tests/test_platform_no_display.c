/* test_platform_no_display.c — platform-independent: verifies FDK's
 * behavior when NO display of either kind is reachable. Deliberately
 * unsets DISPLAY/WAYLAND_DISPLAY so this test is meaningful and
 * consistent whether or not the environment running it happens to
 * have a desktop session — this is what makes it safe for ordinary
 * `make test` (see docs/testing.md's headless-by-default policy). Do
 * NOT run this under Xvfb/a real compositor without unsetting those
 * variables first, or it will fail for the wrong reason. */

#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    assert(r == FDK_ERR_NO_DISPLAY);
    assert(ctx == NULL); /* fdk_init() must leave *out_ctx untouched on failure */
    printf("[ok] fdk_init() with no display reachable returns FDK_ERR_NO_DISPLAY\n");

    /* Explicit-backend requests get the same treatment, no silent
     * fallback to a backend the caller didn't ask for. */
    fdk_init_options x11_opts = { .backend = FDK_PLATFORM_X11 };
    r = fdk_init(&ctx, &x11_opts);
    assert(r == FDK_ERR_NO_DISPLAY);
    printf("[ok] explicit FDK_PLATFORM_X11 with no display reachable fails cleanly\n");

    fdk_init_options wl_opts = { .backend = FDK_PLATFORM_WAYLAND };
    r = fdk_init(&ctx, &wl_opts);
    assert(r == FDK_ERR_NO_DISPLAY);
    printf("[ok] explicit FDK_PLATFORM_WAYLAND with no display reachable fails cleanly\n");

    printf("\nall no-display platform tests passed\n");
    return 0;
}
