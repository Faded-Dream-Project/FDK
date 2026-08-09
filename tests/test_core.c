/* test_core.c — platform-independent core tests: version, error
 * strings, and argument validation that doesn't require an actual
 * platform connection to exercise. See docs/testing.md for why this
 * file is deliberately narrower than it was in Phase 1: fdk_init()
 * now really does connect to a display, so tests that need a live
 * connection moved to tests/test_x11_integration.c (run via
 * `make test-x11`, not plain `make test`) rather than staying here
 * and making ordinary `make test` require a desktop session. */

#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_version(void) {
    assert(fdk_get_version() == FDK_VERSION);
    assert(strcmp(fdk_get_version_string(), FDK_VERSION_STRING) == 0);
    printf("[ok] version: %s (%d)\n", fdk_get_version_string(), fdk_get_version());
}

static void test_error_strings(void) {
    assert(strcmp(fdk_result_to_string(FDK_OK), "ok") == 0);
    assert(fdk_ok(FDK_OK));
    assert(!fdk_ok(FDK_ERR_OUT_OF_MEMORY));

    /* Every declared error code must produce a non-generic string;
     * spot-check a representative sample from each subsystem range. */
    assert(strcmp(fdk_result_to_string(FDK_ERR_INVALID_ARGUMENT), "unknown error") != 0);
    assert(strcmp(fdk_result_to_string(FDK_ERR_NO_DISPLAY), "unknown error") != 0);
    assert(strcmp(fdk_result_to_string(FDK_ERR_THEME_PARSE), "unknown error") != 0);

    printf("[ok] error strings\n");
}

static void test_init_rejects_null_out(void) {
    /* No display needed: fdk_init() validates out_ctx before ever
     * attempting a platform connection (see src/core/context.c). */
    fdk_result r = fdk_init(NULL, NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    printf("[ok] fdk_init(NULL, ...) rejected\n");
}

static void test_shutdown_null_is_safe(void) {
    fdk_shutdown(NULL); /* must not crash */
    printf("[ok] fdk_shutdown(NULL) no-op\n");
}

static void test_quit_null_is_safe(void) {
    fdk_quit(NULL); /* must not crash */
    printf("[ok] fdk_quit(NULL) no-op\n");
}

int main(void) {
    test_version();
    test_error_strings();
    test_init_rejects_null_out();
    test_shutdown_null_is_safe();
    test_quit_null_is_safe();

    printf("\nall platform-independent core tests passed\n");
    return 0;
}
