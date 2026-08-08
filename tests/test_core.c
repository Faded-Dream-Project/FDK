/* test_core.c — fdk_init/fdk_shutdown lifecycle, version, error strings */

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

static void test_init_default(void) {
    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    assert(fdk_ok(r));
    assert(ctx != NULL);
    fdk_shutdown(ctx);
    printf("[ok] fdk_init(NULL options) / fdk_shutdown\n");
}

static void test_init_with_options(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = {
        .backend = FDK_PLATFORM_AUTO,
        .app_id = "org.fdk.test",
    };
    fdk_result r = fdk_init(&ctx, &opts);
    assert(fdk_ok(r));
    assert(ctx != NULL);
    fdk_shutdown(ctx);
    printf("[ok] fdk_init(options with app_id)\n");
}

static void test_init_rejects_null_out(void) {
    fdk_result r = fdk_init(NULL, NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    printf("[ok] fdk_init(NULL, ...) rejected\n");
}

static void test_shutdown_null_is_safe(void) {
    fdk_shutdown(NULL); /* must not crash */
    printf("[ok] fdk_shutdown(NULL) no-op\n");
}

static void test_quit_before_run_is_safe(void) {
    fdk_context *ctx = NULL;
    assert(fdk_ok(fdk_init(&ctx, NULL)));
    fdk_quit(ctx);   /* must not crash even though fdk_run() never called */
    fdk_quit(NULL);  /* must not crash */
    fdk_shutdown(ctx);
    printf("[ok] fdk_quit() safety\n");
}

static void test_run_returns(void) {
    /* Phase 1 has no platform event source, so fdk_run() must return
     * promptly rather than blocking forever — this is what proves it. */
    fdk_context *ctx = NULL;
    assert(fdk_ok(fdk_init(&ctx, NULL)));
    fdk_run(ctx);
    fdk_shutdown(ctx);
    printf("[ok] fdk_run() returns (no platform layer yet)\n");
}

int main(void) {
    test_version();
    test_error_strings();
    test_init_default();
    test_init_with_options();
    test_init_rejects_null_out();
    test_shutdown_null_is_safe();
    test_quit_before_run_is_safe();
    test_run_returns();

    printf("\nall core tests passed\n");
    return 0;
}
