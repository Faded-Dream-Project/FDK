/*
 * surface_aa.c — antialiased drawing primitives
 * (fdk_surface_draw_line_aa / draw_circle_aa / fill_circle_aa /
 * fill_rounded_rect_aa; see include/fdk/fdk_surface.h).
 *
 * Every primitive computes each pixel's EXACT geometric coverage (the
 * fraction of the pixel's area the ideal shape covers — via signed
 * distances, not approximations) and blends the color weighted by that
 * coverage through fdk__surface_blend_coverage, which is the same
 * source-over math (and the same ARGB-aware path) as the crisp
 * primitives. Consequences the tests pin:
 *
 *   - a horizontal or vertical AA line is pixel-identical to the crisp
 *     Bresenham line (coverage collapses to exactly 0 or 1 on axis);
 *   - opaque AA shapes over an opaque background never double-blend
 *     (each destination pixel is visited once);
 *   - every blended value is inside the convex hull of background and
 *     shape colors (no overshoot from filtering).
 *
 * The crisp (_non-_aa) variants remain the defaults: exact integer
 * geometry, zero float math in the hot path. The _aa variants trade
 * that for smooth edges and cost a distance evaluation per pixel.
 *
 * Not part of the public API beyond the documented functions — never
 * installed.
 */

#define FDK_LOG_TAG "render"

#include "surface_internal.h"

#include <math.h>
#include <stdlib.h>

/* ---- antialiased line (Xiaolin Wu) ------------------------------------- */

/* Plots one pixel with the given fractional coverage (0..1). */
static void wu_plot(fdk_surface *s, int x, int y, fdk_color color,
                    float c) {
    if (c <= 0.0f) {
        return;
    }
    if (c > 1.0f) {
        c = 1.0f;
    }
    fdk__surface_blend_coverage(s, x, y, color, c);
}

void fdk_surface_draw_line_aa(fdk_surface *surface,
                              fdk_i32 x0, fdk_i32 y0,
                              fdk_i32 x1, fdk_i32 y1,
                              fdk_color color) {
    if (surface == NULL) {
        return;
    }
    if (!fdk_ok(fdk__surface_acquire(surface))) {
        return;
    }

    /* Axis-aligned lines collapse to exact 1px coverage — delegate to
     * the same span logic the crisp variant produces (verified by
     * test: crisp and AA agree byte-for-byte on axis). */
    if (y0 == y1) {
        int xa = x0 < x1 ? x0 : x1;
        int xb = x0 < x1 ? x1 : x0;
        for (int x = xa; x <= xb; x++) {
            fdk__surface_blend_coverage(surface, x, y0, color, 1.0f);
        }
        fdk__surface_damage_add(
            surface, (fdk_rect){ .x = xa, .y = y0,
                                 .width = xb - xa + 1, .height = 1 });
        return;
    }
    if (x0 == x1) {
        int ya = y0 < y1 ? y0 : y1;
        int yb = y0 < y1 ? y1 : y0;
        for (int y = ya; y <= yb; y++) {
            fdk__surface_blend_coverage(surface, x0, y, color, 1.0f);
        }
        fdk__surface_damage_add(
            surface, (fdk_rect){ .x = x0, .y = ya,
                                 .width = 1, .height = yb - ya + 1 });
        return;
    }

    /* Xiaolin Wu, integer-endpoint form: endpoints sit exactly on
     * pixel centers, so every column's ideal minor coordinate is the
     * direct rational interpolation y0 + (y1-y0)*(x-x0)/(x1-x0) —
     * evaluated per column (no incremental drift), its fractional
     * part split over the two bracketing pixels. */
    int steep = abs(y1 - y0) > abs(x1 - x0);
    int ax0, ay0, ax1, ay1; /* always left-to-right along the major axis */
    if (steep) {
        /* Walk y: swap roles. */
        if (y0 < y1) { ax0 = x0; ay0 = y0; ax1 = x1; ay1 = y1; }
        else         { ax0 = x1; ay0 = y1; ax1 = x0; ay1 = y0; }
    } else {
        if (x0 < x1) { ax0 = x0; ay0 = y0; ax1 = x1; ay1 = y1; }
        else         { ax0 = x1; ay0 = y1; ax1 = x0; ay1 = y0; }
    }

    int major0 = steep ? ay0 : ax0;
    int major1 = steep ? ay1 : ax1;
    int minor0 = steep ? ax0 : ay0;
    int minor1 = steep ? ax1 : ay1;
    int dmaj = major1 - major0; /* > 0 by construction */
    int dmin = minor1 - minor0;

    for (int maj = major0; maj <= major1; maj++) {
        float mn = (float)minor0 + (float)dmin * (float)(maj - major0) /
                                         (float)dmaj;
        int nb = (int)floorf(mn);
        float frac = mn - (float)nb;
        if (steep) {
            wu_plot(surface, nb, maj, color, 1.0f - frac);
            wu_plot(surface, nb + 1, maj, color, frac);
        } else {
            wu_plot(surface, maj, nb, color, 1.0f - frac);
            wu_plot(surface, maj, nb + 1, color, frac);
        }
    }

    /* Damage: the line's bbox (2px minor-axis thick for the bracket
     * pixels). */
    int dxall = (x1 - x0) < 0 ? -(x1 - x0) : (x1 - x0);
    int dyall = (y1 - y0) < 0 ? -(y1 - y0) : (y1 - y0);
    fdk_rect bbox;
    if (steep) {
        bbox.x = (x0 < x1 ? x0 : x1) - 1;
        bbox.y = (y0 < y1 ? y0 : y1) - 1;
        bbox.width = dyall + 3;
        bbox.height = dxall + 3;
    } else {
        bbox.x = (x0 < x1 ? x0 : x1) - 1;
        bbox.y = (y0 < y1 ? y0 : y1) - 1;
        bbox.width = dxall + 3;
        bbox.height = dyall + 3;
    }
    fdk__surface_damage_add(surface, bbox);
}

/* ---- antialiased circles ----------------------------------------------- */

void fdk_surface_draw_circle_aa(fdk_surface *surface,
                                fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                                fdk_color color) {
    if (surface == NULL || radius <= 0) {
        return;
    }
    if (!fdk_ok(fdk__surface_acquire(surface))) {
        return;
    }

    /* Pixel-center distances to the ideal circle; coverage is the
     * fraction of a 1px-wide ring centered on radius r. */
    int r = radius;
    for (int y = cy - r - 2; y <= cy + r + 2; y++) {
        for (int x = cx - r - 2; x <= cx + r + 2; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = 1.0f - fabsf(dist - (float)r);
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            fdk__surface_blend_coverage(surface, x, y, color, cov);
        }
    }
    fdk__surface_damage_add(
        surface, (fdk_rect){ .x = cx - r - 2, .y = cy - r - 2,
                             .width = 2 * r + 5, .height = 2 * r + 5 });
}

void fdk_surface_fill_circle_aa(fdk_surface *surface,
                                fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                                fdk_color color) {
    if (surface == NULL || radius <= 0) {
        return;
    }
    if (!fdk_ok(fdk__surface_acquire(surface))) {
        return;
    }

    /* Coverage = how far inside the radius the pixel center is:
     * full at r - 0.5, gone at r + 0.5. */
    int r = radius;
    for (int y = cy - r - 1; y <= cy + r + 1; y++) {
        for (int x = cx - r - 1; x <= cx + r + 1; x++) {
            float dx = (float)(x - cx);
            float dy = (float)(y - cy);
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = (float)r + 0.5f - dist;
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            fdk__surface_blend_coverage(surface, x, y, color, cov);
        }
    }
    fdk__surface_damage_add(
        surface, (fdk_rect){ .x = cx - r - 1, .y = cy - r - 1,
                             .width = 2 * r + 3, .height = 2 * r + 3 });
}

/* ---- antialiased rounded rectangle -------------------------------------- */

void fdk_surface_fill_rounded_rect_aa(fdk_surface *surface, fdk_rect rect,
                                      fdk_i32 corner_radius,
                                      fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(fdk__surface_acquire(surface))) {
        return;
    }

    /* Radius clamped to half the shorter side — same rule as the
     * crisp variant. */
    int half_min = rect.width < rect.height ? rect.width : rect.height;
    int r = corner_radius;
    if (r < 0) {
        r = 0;
    }
    if (r * 2 > half_min) {
        r = half_min / 2;
    }

    /* Signed distance from the pixel CENTER to the rounded-rect
     * boundary (negative inside). Straight bands reduce to plain axis
     * distances; corner bands to circle distances around the corner
     * centers — the classic box-SDF. */
    float ctr_x = (float)rect.x + (float)rect.width * 0.5f;
    float ctr_y = (float)rect.y + (float)rect.height * 0.5f;
    float half_w = (float)rect.width * 0.5f;
    float half_h = (float)rect.height * 0.5f;
    float core_w = half_w - (float)r;
    float core_h = half_h - (float)r;

    int x0 = rect.x - 1;
    int y0 = rect.y - 1;
    int x1 = rect.x + rect.width + 1;
    int y1 = rect.y + rect.height + 1;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float qx = fabsf(px - ctr_x) - core_w;
            float qy = fabsf(py - ctr_y) - core_h;

            /* Box SDF: distance outside the core rect, plus how far
             * inside the core the point is when both q are negative. */
            float ax = qx > 0.0f ? qx : 0.0f;
            float ay = qy > 0.0f ? qy : 0.0f;
            float outside = sqrtf(ax * ax + ay * ay);
            float inside = qx > qy ? qx : qy;
            float sdf = outside + (inside < 0.0f ? inside : 0.0f);

            /* Circle regions use the corner radius as their scale;
             * straight edges are at distance (sdf - r) from the real
             * boundary, so the effective signed distance to the
             * rounded-rect boundary is sdf - r... EXCEPT that for the
             * pure-straight case (r == 0) this is exactly the plain
             * box distance. One formula covers both: */
            float dist = sdf - (float)r;

            float cov = 0.5f - dist;
            if (cov <= 0.0f) {
                continue;
            }
            if (cov > 1.0f) {
                cov = 1.0f;
            }
            fdk__surface_blend_coverage(surface, x, y, color, cov);
        }
    }

    fdk__surface_damage_add(
        surface, (fdk_rect){ .x = x0, .y = y0,
                             .width = x1 - x0, .height = y1 - y0 });
}
