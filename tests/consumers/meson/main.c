/* The consumer-build test's single translation unit (shared shape
 * with the cmake twin's sibling): exercise the install surface exactly as a
 * downstream integrator would — the umbrella header, a headless
 * surface, a blit, and the version string. Any of these failing
 * (missing header, unresolved symbol, wrong soname) is the
 * failure this test exists to catch. */
#include <fdk/fdk.h>
#include <stdio.h>

int main(void) {
    printf("FDK version: %s\n", fdk_get_version_string());

    /* ARGB8888 explicitly: blit_blend's documented src
     * requirement (the plain create default is XRGB8888). */
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_surface_create_format(64, 64,
                                    FDK_SURFACE_FORMAT_ARGB8888, &s))) {
        fprintf(stderr, "consumer: surface create failed\n");
        return 1;
    }
    fdk_surface_fill(s, (fdk_color){ .r = 1.0f, .g = 0.0f,
                                     .b = 1.0f, .a = 1.0f });

    /* A second surface + blend: pulls in more than the trivial
     * link set (the compositing path, not just alloc). ARGB8888
     * explicitly (blit_blend's documented src requirement). */
    fdk_surface *dst = NULL;
    if (!fdk_ok(fdk_surface_create(128, 128, &dst))) {
        fprintf(stderr, "consumer: dst create failed\n");
        return 1;
    }
    fdk_rect src_rect = { .x = 0, .y = 0, .width = 64, .height = 64 };
    if (!fdk_ok(fdk_surface_blit_blend(dst, 32, 32, s, src_rect))) {
        fprintf(stderr, "consumer: blend failed\n");
        return 1;
    }
    fdk_surface_destroy(s);
    fdk_surface_destroy(dst);
    printf("meson consumer OK\n");
    return 0;
}
