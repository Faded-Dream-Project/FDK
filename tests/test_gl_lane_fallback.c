/* test_gl_lane_fallback.c — the §19 upload-contract fallback shapes,
 * verified WITHOUT any GL driver (1.1.0-rc2, the re-land of the lost
 * 1.0.0-era NVIDIA fix).
 *
 * The Wayland GPU lane's upload contract has four shapes, decided per
 * connection off the ES2 extension string: BGRA+ROW_LENGTH (the fast
 * path), BGRA with packed sub-rects, an RGBA twin with ROW_LENGTH,
 * and the twin with packed sub-rects (pure core GLES2 — the NVIDIA
 * proprietary-driver shape; GL_BGRA and GL_UNPACK_ROW_LENGTH are NOT
 * core GLES2 and a strict ES2 context rejects them). The GL side of
 * those shapes needs a driver this test refuses to require; the CPU
 * side — the R<->B twin swizzle and the tight row packing — is pure
 * byte math, and THIS is where it is pinned. Where a Wayland EGL
 * driver DOES exist, test_wayland_integration.c's FDK_GL_CORE_ONLY=1
 * leg drives the full lane through the same fallback shapes
 * (FDK_GL_CORE_ONLY pins has_bgra_ext/has_rowlen_ext to 0 on any
 * driver).
 *
 * The byte contract being pinned (little-endian fdk_u32s): a staging
 * pixel is [B,G,R,X] — the XRGB8888/wl_shm layout; the twin is
 * [R,G,B,X] — the core-legal RGBA form. GL reading the ORIGINAL as
 * BGRA/UNSIGNED_BYTE and the TWIN as RGBA/UNSIGNED_BYTE produces the
 * SAME texel (r=R, g=G, b=B) — the twin is ink-identical by
 * construction, which is what makes the fallback invisible to the
 * self-check and the equivalence rigs.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "platform/wayland/wayland_egl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_twin_swizzle(void) {
    /* Known bytes: B=0x11, G=0x22, R=0x33, X=0x44 -> u32 0x44332211;
     * the twin must be [R,G,B,X] -> 0x44112233. */
    fdk_u32 src[4] = { 0x44332211u, 0xFF0000FFu, 0x00FF00FFu,
                       0x12345678u };
    fdk_u32 dst[4] = { 0, 0, 0, 0 };
    fdk__wayland_egl_test_swizzle(dst, src, 4);
    assert(dst[0] == 0x44112233u);
    assert(dst[1] == 0xFFFF0000u);  /* B=FF,R=00 -> R=FF,B=00 */
    assert(dst[2] == 0x00FF00FFu);  /* B=FF,R=FF: the swap is a
                                     * fixed point */
    /* 0x12345678: B=0x78 G=0x56 R=0x34 X=0x12 -> [0x34,0x56,0x78,0x12]
     * = 0x12785634 */
    assert(dst[3] == 0x12785634u);

    /* The swizzle is an involution: back again is the original. */
    fdk_u32 back[4] = { 0, 0, 0, 0 };
    fdk__wayland_egl_test_swizzle(back, dst, 4);
    assert(back[0] == src[0] && back[1] == src[1] && back[2] == src[2] &&
           back[3] == src[3]);

    /* The G and X channels never move (the mask keeps them). */
    assert((dst[0] & 0xFF00FF00u) == (src[0] & 0xFF00FF00u));

    printf("[ok] gl lane fallback: RGBA-twin swizzle byte math "
           "(known pixels, involution, G/X channels pinned)\n");
}

static void test_row_packing(void) {
    /* A 8x4 stride-8 buffer; pixel (x, y) = 0x80000000 | y * 8 + x so
     * every position is distinguishable. */
    fdk_u32 src[32];
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 8; x++) {
            src[y * 8 + x] = 0x80000000u | (fdk_u32)(y * 8 + x);
        }
    }
    /* Rect (2, 1) size 3x2: rows 1 and 2, columns 2..4, TIGHT. */
    fdk_u32 dst[6];
    memset(dst, 0xAA, sizeof dst);
    fdk__wayland_egl_test_pack_rows(dst, src, 8, 2, 1, 3, 2);
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 3; i++) {
            assert(dst[j * 3 + i] == src[(1 + j) * 8 + (2 + i)]);
        }
    }
    /* Exactly six pixels written — the trailing sentinel survives. */
    fdk_u32 sentinel[8];
    memset(sentinel, 0xAA, sizeof sentinel);
    fdk__wayland_egl_test_pack_rows(sentinel, src, 8, 2, 1, 3, 2);
    assert(sentinel[6] == 0xAAAAAAAAu && sentinel[7] == 0xAAAAAAAAu);

    /* Full-width rect: packing is the identity layout. */
    fdk_u32 full[8];
    fdk__wayland_egl_test_pack_rows(full, src, 8, 0, 2, 8, 1);
    assert(memcmp(full, src + 16, sizeof full) == 0);

    /* Single pixel. */
    fdk_u32 one = 0;
    fdk__wayland_egl_test_pack_rows(&one, src, 8, 5, 3, 1, 1);
    assert(one == src[3 * 8 + 5]);

    printf("[ok] gl lane fallback: tight row packing (sub-rect, "
           "full-width, single pixel; stride honored, nothing extra "
           "written)\n");
}

int main(void) {
    test_twin_swizzle();
    test_row_packing();
    printf("\nall gl lane fallback tests passed\n");
    return 0;
}
