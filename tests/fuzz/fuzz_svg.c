/*
 * fuzz_svg.c — libFuzzer harness for the vendored SVG rasterizer
 * (docs/fuzzing.md).
 *
 * Target: fdk_surface_create_from_svg_bytes() — the public
 * decode-from-memory entry that runs the vendored nanosvg parse
 * and rasterization. SVG icons are theme content (downloaded
 * .FDKThemes carry them), so a hostile document is in scope for
 * the security policy.
 *
 * The size argument is fixed at 64: the fuzzed surface is the
 * DOCUMENT, not the raster size (nanosvg's scaling math is
 * exercised through the document's own width/height/viewBox
 * declarations).
 *
 * nanosvg is a lenient parser by design — it degrades to
 * "nothing drawn", which the coverage gate then reports as
 * FDK_ERR_UNSUPPORTED. That is an expected, non-failing outcome.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The entry takes a C string; the harness treats the input as
     * one (libFuzzer inputs may contain NULs — the parser sees a
     * truncated document, which is a valid degraded case). Skip
     * the oversized: nanosvg walks the whole string. */
    if (size == 0 || size > 512 * 1024) {
        return 0;
    }
    /* Ensure NUL termination for the string contract. */
    char *doc = (char *)malloc(size + 1);
    if (doc == NULL) {
        return 0;
    }
    memcpy(doc, data, size);
    doc[size] = '\0';

    fdk_surface *s = NULL;
    fdk_result r = fdk_surface_create_from_svg_bytes(doc, 64, &s);
    if (fdk_ok(r)) {
        fdk_surface_destroy(s);
    }
    free(doc);
    return 0;
}
