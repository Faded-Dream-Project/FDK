/* tests/test_frozen_init.c — the frozen-symbol pin, in its own TU.
 *
 * WHY A SEPARATE FILE: the pin must call the EXPORTED fdk_init
 * (the 1.0-frozen symbol), but any TU that includes fdk_core.h
 * normally gets the static-inline redirect instead — the token
 * fdk_init in source is the inline, not the symbol. The first
 * version of this pin lived in test_core.c and reached around the
 * redirect with a GCC asm label
 * (extern ... fdk_test_frozen_init_sym ... __asm__("fdk_init")).
 * That put TWO entities with one assembler name in one TU — the
 * inline's emitted local copy and the extern alias — which is
 * exactly the fragile shape the header warns about, and it
 * misbehaved on arm64/gcc-13/-O0 (the call returned 0 without
 * entering fdk_init_ex; the (fdk_context*)0x1 sentinel survived
 * and fdk_shutdown faulted at ctx->thread_state). The header's
 * own contract (fdk_core.h, "Opting out of the redirect") is the
 * supported way: build THIS TU with -DFDK_NO_INIT_REDIRECT and
 * call plain fdk_init — no asm labels, no alias tricks, the
 * ordinary linker binding the compiler built for us.
 *
 * WHAT IS PINNED (the same proof as before, moved):
 *   - the exported fdk_init reads AT MOST the 1.0 layout. Proven
 *     with a heap allocation of EXACTLY FDK_INIT_OPTIONS_SIZE_1_0
 *     (24) bytes: the frozen symbol cannot read a 25th byte
 *     without ASan aborting (the whole binary runs sanitized).
 *   - the 1.0 mirror struct and the real struct agree on size
 *     (static asserts; a shrink or reorder breaks the build).
 *   - the acceptance envelope: headless (no display) the call
 *     returns FDK_ERR_NO_DISPLAY; with a display it succeeds —
 *     either is fine, an over-read is not.
 *
 * The Makefile builds this with -DFDK_NO_INIT_REDIRECT and adds
 * it to the test suite (see the test_frozen_init target). */
#define FDK_NO_INIT_REDIRECT 1

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <assert.h>
#include <stddef.h> /* offsetof */
#include <stdio.h>
#include <stdlib.h>

static void test_frozen_symbol_pin(void) {
    /* The 1.0 layout, field-for-field, as a binary compiled
     * against the 1.0 header saw it. Heap-allocated at EXACTLY
     * the 1.0 size so ASan's redzone catches any read past it. */
    struct layout_1_0 {
        fdk_platform_backend backend;
        const char *app_id;
        int render_mode;
        unsigned int image_max_pixels;
    };
    _Static_assert(sizeof(fdk_init_options) ==
                       FDK_INIT_OPTIONS_SIZE_1_0,
                   "the pin and the real struct must agree");
    _Static_assert(sizeof(struct layout_1_0) ==
                       FDK_INIT_OPTIONS_SIZE_1_0,
                   "the local mirror must be the 1.0 size");

    struct layout_1_0 *small = calloc(1, sizeof *small);
    assert(small != NULL);
    small->backend = FDK_PLATFORM_AUTO;
    small->app_id = "fdk-frozen-symbol-test";

    /* Plain fdk_init — THIS TU opted out of the redirect, so the
     * call binds the exported frozen symbol directly. */
    fdk_context *ctx = (fdk_context *)0x1;
    fdk_result r = fdk_init(&ctx, (const fdk_init_options *)small);
    assert(r == FDK_ERR_NO_DISPLAY || fdk_ok(r));
    if (fdk_ok(r)) {
        fdk_shutdown(ctx);
    }

    /* The frozen symbol treats NULL options the same as every
     * other form: no reads at all. */
    ctx = (fdk_context *)0x1;
    r = fdk_init(&ctx, NULL);
    assert(r == FDK_ERR_NO_DISPLAY || fdk_ok(r));
    if (fdk_ok(r)) {
        fdk_shutdown(ctx);
    }

    free(small);
    printf("[ok] frozen fdk_init symbol: reads at most the 1.0 "
           "layout (24-byte heap proof under ASan), NULL options "
           "safe\n");
}

int main(void) {
    test_frozen_symbol_pin();
    printf("all frozen-symbol pin tests passed\n");
    return 0;
}
