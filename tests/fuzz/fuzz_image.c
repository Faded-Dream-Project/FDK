/*
 * fuzz_image.c — libFuzzer harness for the vendored PNG/GIF
 * decoders (docs/fuzzing.md).
 *
 * Target: fdk_surface_create_from_image_bytes() — the public
 * decode-from-memory entry that dispatches to the vendored stb
 * image decoders (PNG, GIF, JPEG, BMP, PSD included in stb_image's
 * set; the toolkit's documented formats are PNG and GIF — the
 * dispatch itself decides, so every byte stream finds its lane).
 * Image files arrive from the internet (clipboard image reads,
 * theme icon PNGs, picture widgets), so the security policy's
 * "attacker controls every byte" clause applies in full.
 *
 * A NULL out-surface with FDK_ERR_UNSUPPORTED is the expected
 * outcome for garbage; anything else is the sanitizer's business.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* stb's own bounds: 1..1 MiB is where the interesting parse
     * lives; larger inputs mostly stress the length checks. */
    if (size == 0 || size > 4 * 1024 * 1024) {
        return 0;
    }
    fdk_surface *s = NULL;
    fdk_result r = fdk_surface_create_from_image_bytes(data, size, &s);
    if (fdk_ok(r)) {
        fdk_surface_destroy(s);
    }
    return 0;
}
