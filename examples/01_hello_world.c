/*
 * 01_hello_world.c — the smallest possible FDK program.
 *
 * This milestone (Phase 1: Foundation) does not yet have a platform
 * layer or windowing, so there is no window to show. What this example
 * demonstrates honestly is the full init/shutdown lifecycle every FDK
 * program follows, plus version and error-handling conventions. Once
 * Phase 2 (platform layer) lands, this same skeleton grows a
 * fdk_window_create() call between init and run.
 *
 * Build: make examples
 * Run:   ./build/examples/01_hello_world
 */

#include "fdk/fdk.h"

#include <stdio.h>

int main(void) {
    printf("Faded Dream ToolKit %s\n", fdk_get_version_string());

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_init failed: %s\n", fdk_result_to_string(r));
        return 1;
    }

    /* Phase 2+ will add window creation here. */
    fdk_run(ctx);

    fdk_shutdown(ctx);
    return 0;
}
