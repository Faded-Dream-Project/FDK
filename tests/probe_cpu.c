/* probe_cpu.c — pin the CPU blit_transformed + texture_draw paths. */
#include "fdk/fdk.h"
#include "fdk/fdk_core.h"
#include "fdk/fdk_surface.h"
#include "render/surface_internal.h"

#include <math.h>
#include <stdio.h>

int main(void) {
    fdk_surface *dst = NULL, *card = NULL;
    fdk_surface_create(480, 300, &dst);
    fdk_surface_create(64, 48, &card);
    fdk_color bg = { 0.1f, 0.1f, 0.1f, 1.0f };
    fdk_surface_fill(dst, bg);
    fdk_color cc = { 0.95f, 0.95f, 0.95f, 1.0f };
    fdk_surface_fill(card, cc);

    float a = 3.14159265f / 4.0f;
    fdk_matrix rot = { cosf(a), -sinf(a), 0, sinf(a), cosf(a), 0 };
    fdk_matrix sc = { 0.8f, 0, 0, 0, 0.8f, 0 };
    fdk_matrix m = fdk_matrix_mul(sc, rot);
    fdk_matrix tr = { 1, 0, 273.0f, 0, 1, 101.0f };
    fdk_matrix m1 = m;
    printf("m1 = %.3f %.3f %.1f | %.3f %.3f %.1f\n", m1.m00, m1.m01,
           m1.tx, m1.m10, m1.m11, m1.ty);
    m = fdk_matrix_mul(m, tr);
    fdk_result r = fdk_surface_blit_transformed(dst, m, card);
    printf("blit_transformed: %d\n", (int)r);
    long changed = 0;
    for (size_t i = 0; i < (size_t)dst->fb.stride * dst->fb.height; i++) {
        if (dst->own_pixels[i] != 0x001a1a1au) {
            changed++;
        }
    }
    printf("changed pixels: %ld\n", changed);

    /* simplest case: pure translation */
    fdk_surface *dst2 = NULL;
    fdk_surface_create(480, 300, &dst2);
    fdk_surface_fill(dst2, bg);
    fdk_matrix tr2 = { 1, 0, 50, 0, 1, 40 };
    r = fdk_surface_blit_transformed(dst2, tr2, card);
    fdk_u32 p = dst2->own_pixels[(size_t)45 * dst2->fb.stride + 55];
    printf("translation blit: %d pixel@55,45=%08x (expect white-ish "
           "00f2f2f2-family)\n", (int)r, p);

    /* invert check */
    fdk_matrix inv = fdk_matrix_invert(m);
    printf("m   = %.3f %.3f %.1f | %.3f %.3f %.1f\n", m.m00, m.m01, m.tx,
           m.m10, m.m11, m.ty);
    printf("inv = %.3f %.3f %.1f | %.3f %.3f %.1f\n", inv.m00, inv.m01,
           inv.tx, inv.m10, inv.m11, inv.ty);
    /* sanity: inv(m) * m should be identity — apply m to a point, then
     * inv to the result */
    float px5 = m.m00 * 5.0f + m.m01 * 5.0f + m.tx;
    float py5 = m.m10 * 5.0f + m.m11 * 5.0f + m.ty;
    float bx = inv.m00 * px5 + inv.m01 * py5 + inv.tx;
    float by = inv.m10 * px5 + inv.m11 * py5 + inv.ty;
    printf("roundtrip (5,5) -> (%.1f,%.1f) -> (%.2f,%.2f)\n", px5, py5, bx,
           by);

    /* texture draw on the CPU path */
    fdk_texture *t = NULL;
    fdk_texture_create(8, 8, &t);
    fdk_u8 px[8 * 8 * 4];
    for (int i = 0; i < 8 * 8 * 4; i++) {
        px[i] = 200;
    }
    fdk_texture_update(t, (fdk_rect){ 0, 0, 8, 8 }, px, 32);
    r = fdk_texture_draw(dst, t, (fdk_rect){ 370, 120, 80, 60 });
    printf("texture_draw: %d\n", (int)r);
    changed = 0;
    for (int y = 125; y < 175; y++) {
        for (int x = 375; x < 445; x++) {
            fdk_u32 p = dst->own_pixels[(size_t)y * dst->fb.stride + x];
            if (p != 0x001a1a1au) {
                changed++;
            }
        }
    }
    printf("texture region changed: %ld\n", changed);
    fdk_u32 tp = dst->own_pixels[(size_t)150 * dst->fb.stride + 400];
    printf("pixel@400,150=%08x\n", tp);
    return 0;
}
