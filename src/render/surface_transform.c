/*
 * surface_transform.c — affine transforms and the transformed blit
 * (fdk_matrix / fdk_surface_blit_transformed; see fdk_surface.h).
 *
 * The primitive is inverse-mapped: for every DESTINATION pixel inside
 * the transformed source's bounding box (intersected with the clip),
 * the source coordinate is computed through the inverse matrix and
 * sampled bilinearly (edge-clamped). One destination pixel is visited
 * exactly once, so translucent sources never double-blend — the same
 * correctness rule the crisp primitives follow for shapes.
 *
 * Integer-exact fast paths (identity, whole-pixel translation, integer
 * uniform scale-up) route to nearest-neighbor sampling so common
 * operations never blur: scaling a pixel-art sprite 3x reproduces its
 * pixels 3x, not a filtered mush of them.
 *
 * Not part of the public API beyond the documented functions — never
 * installed.
 */

#define FDK_LOG_TAG "render"

#include "surface_internal.h"

#include "core/log_internal.h"

#include <math.h>
#include <string.h>

/* ---- matrix constructors ---------------------------------------------- */

fdk_matrix fdk_matrix_identity(void) {
    fdk_matrix m = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    return m;
}

fdk_matrix fdk_matrix_translate(float tx, float ty) {
    fdk_matrix m = { 1.0f, 0.0f, tx, 0.0f, 1.0f, ty };
    return m;
}

fdk_matrix fdk_matrix_scale(float factor) {
    fdk_matrix m = { factor, 0.0f, 0.0f, 0.0f, factor, 0.0f };
    return m;
}

fdk_matrix fdk_matrix_scale_xy(float sx, float sy) {
    fdk_matrix m = { sx, 0.0f, 0.0f, 0.0f, sy, 0.0f };
    return m;
}

fdk_matrix fdk_matrix_rotate(float radians) {
    float c = cosf(radians);
    float s = sinf(radians);
    fdk_matrix m = { c, -s, 0.0f, s, c, 0.0f };
    return m;
}

fdk_matrix fdk_matrix_mul(fdk_matrix first, fdk_matrix second) {
    /* Composition: apply `first`, then `second` (row-vector convention
     * — matches the header's "first a, then b" reading). */
    fdk_matrix r;
    r.m00 = second.m00 * first.m00 + second.m01 * first.m10;
    r.m01 = second.m00 * first.m01 + second.m01 * first.m11;
    r.tx = second.m00 * first.tx + second.m01 * first.ty + second.tx;
    r.m10 = second.m10 * first.m00 + second.m11 * first.m10;
    r.m11 = second.m10 * first.m01 + second.m11 * first.m11;
    r.ty = second.m10 * first.tx + second.m11 * first.ty + second.ty;
    return r;
}

fdk_matrix fdk_matrix_invert(fdk_matrix m) {
    float det = m.m00 * m.m11 - m.m01 * m.m10;
    if (det == 0.0f || !isfinite(det)) {
        return fdk_matrix_identity(); /* degenerate rule, see header */
    }
    float inv_det = 1.0f / det;
    fdk_matrix r;
    r.m00 = m.m11 * inv_det;
    r.m01 = -m.m01 * inv_det;
    r.m10 = -m.m10 * inv_det;
    r.m11 = m.m00 * inv_det;
    /* inv.t = -A^-1 * t (the affine inverse's translation). */
    r.tx = -(r.m00 * m.tx + r.m01 * m.ty);
    r.ty = -(r.m10 * m.tx + r.m11 * m.ty);
    return r;
}

/* ---- helpers ----------------------------------------------------------- */

/* True if the matrix is (numerically) the identity. */
static int matrix_is_identity(fdk_matrix m) {
    return m.m00 == 1.0f && m.m01 == 0.0f && m.m10 == 0.0f &&
           m.m11 == 1.0f && m.tx == 0.0f && m.ty == 0.0f;
}

/* True for a pure whole-pixel translation. */
static int matrix_is_int_translation(fdk_matrix m) {
    if (m.m00 != 1.0f || m.m01 != 0.0f || m.m10 != 0.0f || m.m11 != 1.0f) {
        return 0;
    }
    return m.tx == floorf(m.tx) && m.ty == floorf(m.ty);
}

/* True for an integer uniform scale-up (2x, 3x, ...) with whole-pixel
 * translation — the nearest-neighbor exact cases. */
static int matrix_is_int_scale_up(fdk_matrix m) {
    if (m.m01 != 0.0f || m.m10 != 0.0f) {
        return 0;
    }
    if (m.m00 != m.m11) {
        return 0;
    }
    float s = m.m00;
    if (s < 2.0f || s != floorf(s) || s > 64.0f) {
        return 0; /* 1x is translation; >64x would explode bounds */
    }
    return m.tx == floorf(m.tx) && m.ty == floorf(m.ty);
}

/* Applies m to (x, y). */
static void matrix_apply(fdk_matrix m, float x, float y, float *out_x,
                         float *out_y) {
    *out_x = m.m00 * x + m.m01 * y + m.tx;
    *out_y = m.m10 * x + m.m11 * y + m.ty;
}

/* Linear interpolation helper (file scope — C17 has no nested
 * functions). */
static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* Samples the source at (fx, fy) BILINEARLY, edge-clamped, writing the
 * four channels as 0..255 straight-alpha floats (alpha as 0..1). */
static void sample_bilinear(const fdk_surface *src, float fx, float fy,
                            float *out_r, float *out_g, float *out_b,
                            float *out_a) {
    /* Bilinear taps sit on pixel CENTERS; (fx, fy) is the continuous
     * sampling position in source coordinates. */
    float x = fx - 0.5f;
    float y = fy - 0.5f;
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > (float)src->fb.width - 1.0f) x = (float)src->fb.width - 1.0f;
    if (y > (float)src->fb.height - 1.0f) y = (float)src->fb.height - 1.0f;

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 > src->fb.width - 1) x1 = src->fb.width - 1;
    if (y1 > src->fb.height - 1) y1 = src->fb.height - 1;

    float wx = x - (float)x0;
    float wy = y - (float)y0;

    fdk_u32 p00 = src->fb.pixels[(size_t)y0 * (size_t)src->fb.stride +
                                 (size_t)x0];
    fdk_u32 p10 = src->fb.pixels[(size_t)y0 * (size_t)src->fb.stride +
                                 (size_t)x1];
    fdk_u32 p01 = src->fb.pixels[(size_t)y1 * (size_t)src->fb.stride +
                                 (size_t)x0];
    fdk_u32 p11 = src->fb.pixels[(size_t)y1 * (size_t)src->fb.stride +
                                 (size_t)x1];

    int src_opaque = (src->format != FDK_SURFACE_FORMAT_ARGB8888);

    float r00 = (float)((p00 >> 16) & 0xFFu);
    float r10 = (float)((p10 >> 16) & 0xFFu);
    float r01 = (float)((p01 >> 16) & 0xFFu);
    float r11 = (float)((p11 >> 16) & 0xFFu);
    float g00 = (float)((p00 >> 8) & 0xFFu);
    float g10 = (float)((p10 >> 8) & 0xFFu);
    float g01 = (float)((p01 >> 8) & 0xFFu);
    float g11 = (float)((p11 >> 8) & 0xFFu);
    float b00 = (float)(p00 & 0xFFu);
    float b10 = (float)(p10 & 0xFFu);
    float b01 = (float)(p01 & 0xFFu);
    float b11 = (float)(p11 & 0xFFu);
    float a00 = src_opaque ? 255.0f : (float)((p00 >> 24) & 0xFFu);
    float a10 = src_opaque ? 255.0f : (float)((p10 >> 24) & 0xFFu);
    float a01 = src_opaque ? 255.0f : (float)((p01 >> 24) & 0xFFu);
    float a11 = src_opaque ? 255.0f : (float)((p11 >> 24) & 0xFFu);

    *out_r = lerpf(lerpf(r00, r10, wx), lerpf(r01, r11, wx), wy);
    *out_g = lerpf(lerpf(g00, g10, wx), lerpf(g01, g11, wx), wy);
    *out_b = lerpf(lerpf(b00, b10, wx), lerpf(b01, b11, wx), wy);
    *out_a = lerpf(lerpf(a00, a10, wx), lerpf(a01, a11, wx), wy) / 255.0f;
}

/* Clamps a float channel into 0..255 in place. */
static void clampf(float *v) {
    if (*v < 0.0f) {
        *v = 0.0f;
    } else if (*v > 255.0f) {
        *v = 255.0f;
    }
}

/* Composites one straight-alpha source pixel over the destination
 * pixel at (x, y) — clip-checked here (the transformed blit's inner
 * loop runs over a pre-clipped bbox, but blend coverage must still
 * respect the exact clip bounds). */
static void composite_over(fdk_surface *dst, int x, int y, float sr,
                           float sg, float sb, float sa) {
    if (x < dst->clip_x0 || y < dst->clip_y0 || x >= dst->clip_x1 ||
        y >= dst->clip_y1) {
        return;
    }
    if (sa <= 0.0f) {
        return;
    }

    fdk_u32 *px = dst->fb.pixels + (size_t)y * (size_t)dst->fb.stride +
                  (size_t)x;
    fdk_u32 d = *px;
    float dr = (float)((d >> 16) & 0xFFu);
    float dg = (float)((d >> 8) & 0xFFu);
    float db = (float)(d & 0xFFu);

    if (sa > 1.0f) {
        sa = 1.0f;
    }
    float inv = 1.0f - sa;

    if (dst->format == FDK_SURFACE_FORMAT_ARGB8888) {
        float da = (float)((d >> 24) & 0xFFu) / 255.0f;
        float out_a = sa + da * inv;
        float or_, og, ob, oa;
        if (out_a <= 0.0f) {
            or_ = og = ob = oa = 0.0f;
        } else {
            float w_s = sa / out_a;
            float w_d = da * inv / out_a;
            or_ = (sr * 255.0f * w_s + dr * w_d);
            og = (sg * 255.0f * w_s + dg * w_d);
            ob = (sb * 255.0f * w_s + db * w_d);
            oa = out_a * 255.0f;
        }
        clampf(&or_);
        clampf(&og);
        clampf(&ob);
        clampf(&oa);
        *px = ((fdk_u32)(oa + 0.5f) << 24) |
              ((fdk_u32)(or_ + 0.5f) << 16) |
              ((fdk_u32)(og + 0.5f) << 8) | (fdk_u32)(ob + 0.5f);
    } else {
        float or_ = sr * 255.0f * sa + dr * inv;
        float og = sg * 255.0f * sa + dg * inv;
        float ob = sb * 255.0f * sa + db * inv;
        clampf(&or_);
        clampf(&og);
        clampf(&ob);
        *px = ((fdk_u32)(or_ + 0.5f) << 16) |
              ((fdk_u32)(og + 0.5f) << 8) | (fdk_u32)(ob + 0.5f);
    }
}

/* ---- the transformed blit ---------------------------------------------- */

fdk_result fdk_surface_blit_transformed(fdk_surface *dst, fdk_matrix m,
                                        fdk_surface *src) {
    if (dst == NULL || src == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_result r = fdk__surface_acquire(dst);
    if (!fdk_ok(r)) {
        return r;
    }
    r = fdk__surface_acquire(src);
    if (!fdk_ok(r)) {
        return r;
    }

    if (src->fb.width <= 0 || src->fb.height <= 0) {
        return FDK_OK;
    }

    /* Fast paths that reduce to the crisp, exact blit. */
    if (matrix_is_identity(m)) {
        return fdk_surface_blit(dst, 0, 0, src,
                                (fdk_rect){ .x = 0, .y = 0,
                                            .width = src->fb.width,
                                            .height = src->fb.height });
    }
    if (matrix_is_int_translation(m)) {
        return fdk_surface_blit(dst, (fdk_i32)m.tx, (fdk_i32)m.ty, src,
                                (fdk_rect){ .x = 0, .y = 0,
                                            .width = src->fb.width,
                                            .height = src->fb.height });
    }

    /* Integer scale-up: nearest neighbor over the scaled bbox (exact
     * pixels, no filtering, no float compositing — reads source
     * pixels and copies them). */
    if (matrix_is_int_scale_up(m) && dst->format != FDK_SURFACE_FORMAT_ARGB8888 &&
        src->format != FDK_SURFACE_FORMAT_ARGB8888) {
        int s = (int)m.m00;
        long long dx0 = (long long)m.tx;
        long long dy0 = (long long)m.ty;
        long long dx1 = dx0 + (long long)src->fb.width * s;
        long long dy1 = dy0 + (long long)src->fb.height * s;
        if (dx1 <= dst->clip_x0 || dy1 <= dst->clip_y0 ||
            dx0 >= dst->clip_x1 || dy0 >= dst->clip_y1) {
            return FDK_OK;
        }
        long long cx0 = dx0 < dst->clip_x0 ? dst->clip_x0 : dx0;
        long long cy0 = dy0 < dst->clip_y0 ? dst->clip_y0 : dy0;
        long long cx1 = dx1 > dst->clip_x1 ? dst->clip_x1 : dx1;
        long long cy1 = dy1 > dst->clip_y1 ? dst->clip_y1 : dy1;
        for (long long y = cy0; y < cy1; y++) {
            long long sy = (y - dy0) / s;
            fdk_u32 *drow =
                dst->fb.pixels + (size_t)y * (size_t)dst->fb.stride;
            const fdk_u32 *srow =
                src->fb.pixels + (size_t)sy * (size_t)src->fb.stride;
            for (long long x = cx0; x < cx1; x++) {
                drow[x] = srow[(x - dx0) / s];
            }
        }
        fdk__surface_damage_add(dst, (fdk_rect){ .x = (fdk_i32)cx0, .y = (fdk_i32)cy0,
                                    .width = (fdk_i32)(cx1 - cx0),
                                    .height = (fdk_i32)(cy1 - cy0) });
        return FDK_OK;
    }

    /* General path: invert, walk the destination bbox. */
    fdk_matrix inv = fdk_matrix_invert(m);
    float det = m.m00 * m.m11 - m.m01 * m.m10;
    if (det == 0.0f || !isfinite(det)) {
        return FDK_OK; /* degenerate — documented no-op */
    }

    /* Destination bbox = bbox of the four transformed corners. */
    float corners[4][2] = {
        { 0.0f, 0.0f },
        { (float)src->fb.width, 0.0f },
        { 0.0f, (float)src->fb.height },
        { (float)src->fb.width, (float)src->fb.height },
    };
    float min_x = 0.0f, max_x = 0.0f, min_y = 0.0f, max_y = 0.0f;
    for (int i = 0; i < 4; i++) {
        float cx, cy;
        matrix_apply(m, corners[i][0], corners[i][1], &cx, &cy);
        if (!isfinite(cx) || !isfinite(cy)) {
            return FDK_OK; /* overflowed coordinates — nothing sane */
        }
        if (i == 0 || cx < min_x) min_x = cx;
        if (i == 0 || cx > max_x) max_x = cx;
        if (i == 0 || cy < min_y) min_y = cy;
        if (i == 0 || cy > max_y) max_y = cy;
    }

    long long bx0 = (long long)floorf(min_x);
    long long by0 = (long long)floorf(min_y);
    long long bx1 = (long long)ceilf(max_x);
    long long by1 = (long long)ceilf(max_y);

    /* Clip to the destination's effective clip (bounds + stack). */
    if (bx1 <= dst->clip_x0 || by1 <= dst->clip_y0 || bx0 >= dst->clip_x1 ||
        by0 >= dst->clip_y1) {
        return FDK_OK;
    }
    if (bx0 < dst->clip_x0) bx0 = dst->clip_x0;
    if (by0 < dst->clip_y0) by0 = dst->clip_y0;
    if (bx1 > dst->clip_x1) bx1 = dst->clip_x1;
    if (by1 > dst->clip_y1) by1 = dst->clip_y1;
    if (bx0 >= bx1 || by0 >= by1) {
        return FDK_OK;
    }

    fdk__surface_damage_add(dst, (fdk_rect){ .x = (fdk_i32)bx0, .y = (fdk_i32)by0,
                                .width = (fdk_i32)(bx1 - bx0),
                                .height = (fdk_i32)(by1 - by0) });

    for (long long y = by0; y < by1; y++) {
        for (long long x = bx0; x < bx1; x++) {
            /* Destination pixel center, inverse-mapped to source. */
            float fx, fy;
            matrix_apply(inv, (float)x + 0.5f, (float)y + 0.5f, &fx, &fy);
            if (fx < -0.5f || fy < -0.5f ||
                fx > (float)src->fb.width + 0.5f ||
                fy > (float)src->fb.height + 0.5f) {
                continue; /* outside the source, even edge-clamped */
            }
            float sr, sg, sb, sa;
            /* Bilinear for every source (XRGB sources sample as fully
             * opaque — sample_bilinear reads no alpha for them);
             * integer-exact cases never reach this path (fast paths
             * above). */
            sample_bilinear(src, fx, fy, &sr, &sg, &sb, &sa);
            composite_over(dst, (int)x, (int)y, sr / 255.0f, sg / 255.0f,
                           sb / 255.0f, sa);
        }
    }

    return FDK_OK;
}
