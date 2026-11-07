/* §31/PART-2 test: fdk_surface_create_from_svg_bytes — the embedded
 * icon tier's decoder (stbi cannot parse SVG; this API is why the
 * bundled tier works at all). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"

static const char *const EYE_SVG =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"24\" "
    "height=\"24\" viewBox=\"0 0 24 24\">"
    "<path d=\"M12 5C7 5 2.7 8.1 1 12c1.7 3.9 6 7 11 7s9.3-3.1 "
    "11-7c-1.7-3.9-6-7-11-7zm0 11.5A4.5 4.5 0 1 1 16.5 12 "
    "4.5 4.5 0 0 1 12 16.5z\" fill=\"#000000\"/></svg>";

int main(void) {
    fdk_surface *s = NULL;
    /* the rasterized embedded icon */
    assert(fdk_ok(fdk_surface_create_from_svg_bytes(EYE_SVG, 16, &s)));
    assert(s != NULL);
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    assert(info.width == 16 && info.height == 16);
    assert(info.format == FDK_SURFACE_FORMAT_ARGB8888);
    /* ink present: some pixel has alpha */
    size_t ink = 0;
    for (fdk_i32 y = 0; y < info.height; y++) {
        fdk_u32 *row = info.pixels + (size_t)y * (size_t)info.stride;
        for (fdk_i32 x = 0; x < info.width; x++) {
            if ((row[x] & 0xFF000000u) != 0u) ink++;
        }
    }
    assert(ink > 10);
    fdk_surface_destroy(s);
    /* the input document is NOT modified (the API copies) */
    const char before = EYE_SVG[0];
    fdk_surface *s2 = NULL;
    assert(fdk_ok(fdk_surface_create_from_svg_bytes(EYE_SVG, 32, &s2)));
    assert(s2 != NULL && EYE_SVG[0] == before);
    fdk_surface_destroy(s2);
    /* NULL discipline + garbage refusal */
    fdk_surface *s3 = (fdk_surface *)1;
    assert(fdk_surface_create_from_svg_bytes(NULL, 16, &s3) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_create_from_svg_bytes("not an svg at all", 16,
                                             &s3) ==
           FDK_ERR_UNSUPPORTED);
    assert(s3 == NULL);
    assert(fdk_surface_create_from_svg_bytes(EYE_SVG, 0, &s3) ==
           FDK_ERR_INVALID_ARGUMENT);
    printf("[ok] svg_bytes: rasterize, ink, immutability, refusal\n");
    return 0;
}
