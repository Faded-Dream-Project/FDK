/*
 * test_clipboard.c — headless clipboard API surface (Phase 9)
 *
 * Everything protocol-shaped needs a display and lives in
 * test_x11_integration.c / test_wayland_integration.c. What CAN be
 * verified without one is the argument-safety contract from
 * fdk_clipboard.h: NULL contexts are rejected, never crash, and the
 * getters report NULL rather than inventing content.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_clipboard.h"

#include <stdio.h>

int main(void) {
    int checks = 0;

    /* set: NULL context is INVALID_ARGUMENT, not a crash. */
    if (fdk_clipboard_set_text(NULL, "x") != FDK_ERR_INVALID_ARGUMENT) {
        fprintf(stderr, "FAIL: set with NULL ctx must be INVALID_ARGUMENT\n");
        return 1;
    }
    checks++;

    /* get: NULL context yields NULL, not a crash. */
    if (fdk_clipboard_get_text(NULL) != NULL) {
        fprintf(stderr, "FAIL: get with NULL ctx must return NULL\n");
        return 1;
    }
    checks++;

    /* set: NULL text is the documented empty-string alias; without a
     * context it still routes to the same INVALID_ARGUMENT path. */
    if (fdk_clipboard_set_text(NULL, NULL) != FDK_ERR_INVALID_ARGUMENT) {
        fprintf(stderr, "FAIL: set(NULL, NULL) must be INVALID_ARGUMENT\n");
        return 1;
    }
    checks++;

    printf("[ok] clipboard argument safety (%d checks)\n", checks);
    return 0;
}
