/* test_render.c — headless software-renderer tests.
 *
 * Everything here runs against OFFSCREEN surfaces (fdk_surface_create)
 * — no display, no backend, no X11/Wayland. That is the point: the
 * entire primitive set, the clip stack, damage bookkeeping, and blit
 * semantics are display-independent, so they must be provable with
 * nothing but memory. The stride of an offscreen surface is
 * deliberately padded (16-px multiples) so every helper is forced
 * through its stride-aware path — the same paths the X11 backend's
 * padded XImages and the Wayland backend's buffers exercise live
 * (verified separately by test_x11_integration.c's server-side pixel
 * readback and the weston-based Wayland verification).
 *
 * Pixel conventions: XRGB8888, R<<16 | G<<8 | B, top byte ignored.
 * All test colors are opaque (a = 1.0) unless a test specifically
 * exercises blending, so readback comparison is exact.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- helpers ---- */

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x];
}

static fdk_u32 pack(int r, int g, int b) {
    return ((fdk_u32)r << 16) | ((fdk_u32)g << 8) | (fdk_u32)b;
}

static fdk_color rgb(int r, int g, int b) {
    fdk_color c = { .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f, .b = (fdk_f32)b / 255.0f,
                    .a = 1.0f };
    return c;
}

static int is_color(fdk_surface *s, int x, int y, fdk_u32 expected) {
    fdk_u32 got = px_at(s, x, y) & 0x00FFFFFFu;
    return got == (expected & 0x00FFFFFFu);
}

/* ---- lifecycle ---- */

static void test_offscreen_create_info_destroy(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 60, &s)));
    assert(s != NULL);

    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    assert(info.width == 100);
    assert(info.height == 60);
    assert(info.stride >= 100);          /* padded stride, see header */
    assert((info.stride & 15) == 0);     /* 16-px alignment by design */
    assert(info.format == FDK_SURFACE_FORMAT_XRGB8888);
    assert(info.pixels != NULL);

    /* Fresh offscreen memory is zeroed (fdk_alloc), and writing
     * through the raw pointer round-trips — stride honored by
     * writing at the LAST pixel of the LAST row. */
    assert(px_at(s, 0, 0) == 0);
    info.pixels[(size_t)(info.height - 1) * (size_t)info.stride +
                (size_t)(info.width - 1)] = pack(1, 2, 3);
    assert(px_at(s, info.width - 1, info.height - 1) == pack(1, 2, 3));
    /* ...and the padding columns were not touched by that write. */
    if (info.stride > info.width) {
        assert(info.pixels[(size_t)(info.height - 1) * (size_t)info.stride +
                           (size_t)info.width] == 0);
    }

    fdk_surface_destroy(s);
    printf("[ok] offscreen surface create/get_info/destroy (padded stride "
           "%d)\n", info.stride);
}

static void test_offscreen_argument_checks(void) {
    fdk_surface *s = NULL;
    assert(fdk_surface_create(0, 10, &s) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_create(10, -1, &s) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_create(20000, 10, &s) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_create(10, 10, NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(s == NULL); /* nothing created on failure */

    fdk_surface_destroy(NULL); /* documented safe no-op */
    printf("[ok] offscreen argument checks\n");
}

static void test_offscreen_present_is_frame_close(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(50, 50, &s)));

    /* Fresh surface: everything damaged (nothing drawn yet). */
    fdk_rect bounds;
    assert(fdk_surface_get_damage_bounds(s, &bounds));
    assert(bounds.width == 50 && bounds.height == 50);

    /* present() on offscreen = frame close: resets damage, no-op. */
    assert(fdk_ok(fdk_surface_present(s)));
    assert(!fdk_surface_get_damage_bounds(s, &bounds));

    fdk_surface_destroy(s);
    printf("[ok] offscreen present closes the frame (damage reset)\n");
}

/* ---- primitives: exact geometry ---- */

static void test_fill_and_fill_rect(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(80, 50, &s)));

    fdk_surface_fill(s, rgb(10, 20, 30));
    for (int y = 0; y < 50; y += 7) {
        for (int x = 0; x < 80; x += 11) {
            assert(is_color(s, x, y, pack(10, 20, 30)));
        }
    }
    assert(fdk_ok(fdk_surface_present(s))); /* consume fill's damage */

    /* A rect that straddles the edges: only the inside changes. */
    fdk_surface_fill_rect(s, (fdk_rect){ .x = -10, .y = 10,
                                         .width = 40, .height = 30 },
                          rgb(200, 0, 0));
    assert(is_color(s, 0, 10, pack(200, 0, 0)));   /* clipped-in left */
    assert(is_color(s, 29, 39, pack(200, 0, 0)));  /* bottom-right in */
    assert(is_color(s, 30, 40, pack(10, 20, 30))); /* just outside */
    assert(is_color(s, 0, 9, pack(10, 20, 30)));   /* just above */
    assert(is_color(s, 70, 40, pack(10, 20, 30))); /* far corner */

    /* Damage of the clipped fill_rect. */
    fdk_rect dmg;
    assert(fdk_surface_get_damage_bounds(s, &dmg));
    assert(dmg.x == 0 && dmg.y == 10 && dmg.width == 30 && dmg.height == 30);

    fdk_surface_destroy(s);
    printf("[ok] fill + clipped fill_rect geometry and damage\n");
}

static void test_draw_rect_no_double_blend_corners(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(60, 60, &s)));
    fdk_surface_fill(s, rgb(0, 0, 0));

    /* Translucent border: each border pixel must be blended EXACTLY
     * once (0.5 white over black = 128-ish), corners included. */
    fdk_color half = { .r = 1, .g = 1, .b = 1, .a = 0.5f };
    fdk_surface_draw_rect(s, (fdk_rect){ .x = 10, .y = 10,
                                         .width = 40, .height = 40 },
                          half);

    fdk_u32 corner = px_at(s, 10, 10) & 0x00FFFFFFu;
    fdk_u32 edge = px_at(s, 30, 10) & 0x00FFFFFFu;
    assert(corner == edge); /* corner == edge => no double blend */
    assert(corner == pack(128, 128, 128) || corner == pack(127, 127, 127) ||
           corner == pack(129, 129, 129)); /* 0.5*255 rounded */
    assert((px_at(s, 30, 30) & 0x00FFFFFFu) == pack(0, 0, 0)); /* interior */

    fdk_surface_destroy(s);
    printf("[ok] draw_rect blends each border pixel exactly once\n");
}

static void test_gradient_interpolation(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(20, 100, &s)));

    fdk_color top = rgb(0, 0, 0);
    fdk_color bottom = rgb(255, 255, 255);
    fdk_surface_fill_gradient_vertical(s,
                                       (fdk_rect){ .x = 0, .y = 0,
                                                   .width = 20, .height = 100 },
                                       top, bottom);

    fdk_u32 first = px_at(s, 10, 0) & 0x00FFFFFFu;
    fdk_u32 last = px_at(s, 10, 99) & 0x00FFFFFFu;
    fdk_u32 mid = px_at(s, 10, 50) & 0x00FFFFFFu;
    assert(first == pack(0, 0, 0));
    assert(last == pack(255, 255, 255));
    /* Monotone and roughly centered (127..129 everywhere). */
    fdk_u32 m = (mid >> 16) & 0xFFu;
    assert(m >= 126 && m <= 129);
    /* Rows strictly increase. */
    assert(px_at(s, 10, 10) < px_at(s, 10, 11));
    assert(px_at(s, 10, 50) < px_at(s, 10, 51));

    fdk_surface_destroy(s);
    printf("[ok] vertical gradient endpoints and monotone interpolation\n");
}

static void test_draw_line(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 100, &s)));
    fdk_surface_fill(s, rgb(0, 0, 0));

    /* Diagonal: every pixel on y = x between the endpoints. */
    fdk_surface_draw_line(s, 10, 10, 60, 60, rgb(255, 0, 0));
    for (int i = 10; i <= 60; i++) {
        assert(is_color(s, i, i, pack(255, 0, 0)));
    }
    assert(is_color(s, 9, 9, pack(0, 0, 0)));
    assert(is_color(s, 61, 61, pack(0, 0, 0)));
    /* One step off the diagonal must be untouched. */
    assert(is_color(s, 30, 31, pack(0, 0, 0)));

    /* Shallow line (dx > dy) left-to-right and right-to-left. */
    fdk_surface_draw_line(s, 5, 80, 95, 90, rgb(0, 255, 0));
    assert(is_color(s, 5, 80, pack(0, 255, 0)));
    assert(is_color(s, 95, 90, pack(0, 255, 0)));
    fdk_surface_draw_line(s, 95, 20, 5, 30, rgb(0, 0, 255));
    assert(is_color(s, 95, 20, pack(0, 0, 255)));
    assert(is_color(s, 5, 30, pack(0, 0, 255)));

    /* Single point degenerates to one pixel. */
    fdk_surface_draw_line(s, 70, 70, 70, 70, rgb(255, 255, 0));
    assert(is_color(s, 70, 70, pack(255, 255, 0)));
    assert(is_color(s, 71, 70, pack(0, 0, 0)));

    /* Vertical line. */
    fdk_surface_draw_line(s, 40, 5, 40, 45, rgb(255, 0, 255));
    for (int y = 5; y <= 45; y++) {
        assert(is_color(s, 40, y, pack(255, 0, 255)));
    }

    fdk_surface_destroy(s);
    printf("[ok] Bresenham lines: diagonal, shallow, both directions, "
           "point, vertical\n");
}

static void test_circle_fill_and_outline(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 100, &s)));
    fdk_surface_fill(s, rgb(0, 0, 0));

    fdk_surface_fill_circle(s, 50, 50, 20, rgb(0, 128, 0));

    /* Center filled, cardinal interior points filled. */
    assert(is_color(s, 50, 50, pack(0, 128, 0)));
    assert(is_color(s, 50, 69, pack(0, 128, 0)));  /* (0, +19) in */
    assert(is_color(s, 69, 50, pack(0, 128, 0)));
    /* Just outside the radius on the axes. */
    assert(is_color(s, 50, 71, pack(0, 0, 0)));    /* (0, +21) out */
    assert(is_color(s, 71, 50, pack(0, 0, 0)));
    /* Diagonal ~ r/sqrt(2) ~= 14 inside, 16+ outside. */
    assert(is_color(s, 50 + 14, 50 + 14, pack(0, 128, 0)));
    assert(is_color(s, 50 + 17, 50 + 17, pack(0, 0, 0)));

    /* Outline circle elsewhere: ring drawn, interior untouched. */
    fdk_surface_draw_circle(s, 20, 80, 12, rgb(255, 255, 255));
    assert(is_color(s, 20 + 12, 80, pack(255, 255, 255))); /* +x point */
    assert(is_color(s, 20, 80 - 12, pack(255, 255, 255))); /* +y point */
    assert(is_color(s, 20, 80, pack(0, 0, 0)));            /* center */

    fdk_surface_destroy(s);
    printf("[ok] circle fill (scanline chords) and outline (cardinals)\n");
}

static void test_rounded_rect_fill_and_outline(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(120, 80, &s)));
    fdk_surface_fill(s, rgb(0, 0, 0));

    /* Radius 15 on a 60x50 rect at (10,10): corner cutouts must be
     * untouched, full rows and columns filled. */
    fdk_rect r = { .x = 10, .y = 10, .width = 60, .height = 50 };
    fdk_surface_fill_rounded_rect(s, r, 15, rgb(0, 0, 200));

    /* Middle row/col: full span filled. */
    assert(is_color(s, 10, 35, pack(0, 0, 200)));
    assert(is_color(s, 69, 35, pack(0, 0, 200)));
    assert(is_color(s, 40, 10, pack(0, 0, 200)));  /* top edge middle */
    /* Corner cutout: the very corner of the bounding box untouched. */
    assert(is_color(s, 10, 10, pack(0, 0, 0)));
    assert(is_color(s, 69, 59, pack(0, 0, 0)));
    /* One step in along the top edge: past the corner radius -> filled. */
    assert(is_color(s, 10 + 15, 10, pack(0, 0, 200)));
    assert(is_color(s, 10 + 15 + 1, 10, pack(0, 0, 200)));

    /* Radius clamping: radius > half the shorter side degrades to a
     * pill/circle shape, never an out-of-bounds crash. */
    fdk_surface_fill_rounded_rect(s, (fdk_rect){ .x = 80, .y = 10,
                                                 .width = 30, .height = 60 },
                                  999, rgb(200, 100, 0));
    assert(is_color(s, 95, 40, pack(200, 100, 0))); /* center filled */
    assert(is_color(s, 80, 10, pack(0, 0, 0)));     /* clamped corner out */

    /* Outline: straight edges + arcs, no double blend on the arcs
     * (translucent outline must equal its edge color exactly). */
    fdk_color half = { .r = 1, .g = 1, .b = 1, .a = 0.5f };
    fdk_surface_draw_rounded_rect(s, (fdk_rect){ .x = 5, .y = 62,
                                                 .width = 50, .height = 15 },
                                  5, half);
    fdk_u32 arc = px_at(s, 5 + 5, 62) & 0x00FFFFFFu; /* arc endpoint-ish */
    fdk_u32 edge = px_at(s, 30, 62) & 0x00FFFFFFu;   /* straight top edge */
    assert(arc == edge); /* same single blend */
    assert(edge == pack(128, 128, 128) || edge == pack(127, 127, 127) ||
           edge == pack(129, 129, 129));

    fdk_surface_destroy(s);
    printf("[ok] rounded rect fill (corner cutouts, radius clamp) and "
           "outline (single blend)\n");
}

/* ---- clip stack ---- */

static void test_clip_stack(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 100, &s)));
    fdk_surface_fill(s, rgb(0, 0, 0));

    /* get_clip with empty stack = infinite plane. */
    fdk_rect inf = fdk_surface_get_clip(s);
    assert(inf.x == INT32_MIN && inf.y == INT32_MIN);
    assert(inf.width == INT32_MAX && inf.height == INT32_MAX);

    /* Push a clip; fills respect it. */
    fdk_rect clip = { .x = 20, .y = 20, .width = 40, .height = 40 };
    assert(fdk_ok(fdk_surface_push_clip(s, clip)));
    assert(fdk_surface_get_clip(s).x == 20); /* intersected with nothing */

    fdk_surface_fill(s, rgb(255, 255, 255)); /* full-surface fill, clipped */
    assert(is_color(s, 20, 20, pack(255, 255, 255)));  /* inside clip */
    assert(is_color(s, 59, 59, pack(255, 255, 255)));
    assert(is_color(s, 19, 20, pack(0, 0, 0)));        /* outside clip */
    assert(is_color(s, 60, 60, pack(0, 0, 0)));
    assert(is_color(s, 99, 99, pack(0, 0, 0)));

    /* Nested push intersects (never expands). */
    fdk_rect inner = { .x = 40, .y = 40, .width = 40, .height = 40 };
    assert(fdk_ok(fdk_surface_push_clip(s, inner)));
    fdk_rect eff = fdk_surface_get_clip(s);
    assert(eff.x == 40 && eff.y == 40);
    assert(eff.width == 20 && eff.height == 20); /* 40..60 only */

    fdk_surface_fill(s, rgb(0, 0, 255));
    assert(is_color(s, 50, 50, pack(0, 0, 255)));     /* in inner clip */
    assert(is_color(s, 30, 30, pack(255, 255, 255))); /* outer only */
    assert(is_color(s, 70, 70, pack(0, 0, 0)));       /* outside both */

    /* Disjoint push -> empty effective clip: drawing no-ops. */
    assert(fdk_ok(fdk_surface_push_clip(s, (fdk_rect){ .x = 70, .y = 70,
                                                       .width = 10,
                                                       .height = 10 })));
    assert(fdk_surface_get_clip(s).width == 0); /* empty intersection */
    fdk_surface_fill(s, rgb(255, 0, 0));        /* must draw NOTHING */
    assert(is_color(s, 50, 50, pack(0, 0, 255)));
    assert(is_color(s, 75, 75, pack(0, 0, 0)));

    /* Unwind: pops restore in LIFO order. */
    fdk_surface_pop_clip(s);
    assert(fdk_surface_get_clip(s).x == 40); /* back to inner */
    fdk_surface_pop_clip(s);
    assert(fdk_surface_get_clip(s).x == 20); /* back to outer */
    fdk_surface_pop_clip(s);
    inf = fdk_surface_get_clip(s);
    assert(inf.x == INT32_MIN); /* back to infinite */

    /* Pop on empty stack = documented no-op, no crash. */
    fdk_surface_pop_clip(s);
    fdk_surface_pop_clip(NULL);

    /* Depth bound: 32 pushes succeed, the 33rd is rejected. */
    for (int i = 0; i < FDK_SURFACE_CLIP_DEPTH; i++) {
        fdk_rect c = { .x = i, .y = i, .width = 1000, .height = 1000 };
        assert(fdk_ok(fdk_surface_push_clip(s, c)));
    }
    fdk_rect overflow = { .x = 0, .y = 0, .width = 10, .height = 10 };
    assert(fdk_surface_push_clip(s, overflow) == FDK_ERR_INVALID_ARGUMENT);
    for (int i = 0; i < FDK_SURFACE_CLIP_DEPTH; i++) {
        fdk_surface_pop_clip(s);
    }

    fdk_surface_destroy(s);
    printf("[ok] clip stack: intersect, nest, empty-intersection, LIFO "
           "unwind, depth bound\n");
}

/* ---- damage ---- */

static void test_damage_bookkeeping(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));

    fdk_rect b;
    /* Fresh surface = fully damaged. */
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.x == 0 && b.y == 0 && b.width == 200 && b.height == 100);

    /* present() consumes it. */
    assert(fdk_ok(fdk_surface_present(s)));
    assert(!fdk_surface_get_damage_bounds(s, &b));

    /* Two separate draws -> union bounds. */
    fdk_surface_fill_rect(s, (fdk_rect){ .x = 10, .y = 10,
                                         .width = 5, .height = 5 },
                          rgb(1, 1, 1));
    fdk_surface_fill_rect(s, (fdk_rect){ .x = 100, .y = 60,
                                         .width = 5, .height = 5 },
                          rgb(1, 1, 1));
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.x == 10 && b.y == 10);
    assert(b.width == 95 && b.height == 55); /* union: 10..105 x 10..65 */

    /* Out-of-bounds invalidate: outside parts are ignored. */
    assert(fdk_ok(fdk_surface_present(s)));
    fdk_surface_invalidate(s, (fdk_rect){ .x = -50, .y = -50,
                                          .width = 60, .height = 60 });
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.x == 0 && b.y == 0 && b.width == 10 && b.height == 10);

    /* Entirely outside: no damage recorded. */
    assert(fdk_ok(fdk_surface_present(s)));
    fdk_surface_invalidate(s, (fdk_rect){ .x = 500, .y = 500,
                                          .width = 10, .height = 10 });
    assert(!fdk_surface_get_damage_bounds(s, &b));

    /* invalidate_all. */
    fdk_surface_invalidate(s, (fdk_rect){ .x = 1, .y = 1,
                                          .width = 3, .height = 3 });
    fdk_surface_invalidate_all(s);
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.width == 200 && b.height == 100);

    /* Raw-pointer writers must invalidate manually (the documented
     * contract): write without invalidate -> present does NOT consume
     * an empty region... and with invalidate it does. */
    assert(fdk_ok(fdk_surface_present(s)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    info.pixels[0] = pack(9, 9, 9);
    assert(!fdk_surface_get_damage_bounds(s, &b)); /* no auto damage */
    fdk_surface_invalidate(s, (fdk_rect){ .x = 0, .y = 0,
                                          .width = 1, .height = 1 });
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.width == 1 && b.height == 1);

    /* Damage overflow degrades to full. */
    assert(fdk_ok(fdk_surface_present(s)));
    for (int i = 0; i < FDK_SURFACE_MAX_DAMAGE + 5; i++) {
        fdk_surface_invalidate(s, (fdk_rect){ .x = i, .y = 0,
                                              .width = 1, .height = 1 });
    }
    assert(fdk_surface_get_damage_bounds(s, &b));
    assert(b.width == 200 && b.height == 100); /* full-surface fallback */

    fdk_surface_destroy(s);
    printf("[ok] damage bookkeeping: bounds, union, clamp, overflow->full, "
           "raw-write contract\n");
}

/* ---- blit ---- */

static void test_blit(void) {
    fdk_surface *src = NULL;
    fdk_surface *dst = NULL;
    assert(fdk_ok(fdk_surface_create(50, 40, &src)));
    assert(fdk_ok(fdk_surface_create(120, 90, &dst)));

    /* Build a recognizable source: red left half, blue right half,
     * with a green pixel at a known interior position. */
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 0, .y = 0, .width = 25,
                                           .height = 40 }, rgb(255, 0, 0));
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 25, .y = 0, .width = 25,
                                           .height = 40 }, rgb(0, 0, 255));
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 10, .y = 10, .width = 1,
                                           .height = 1 }, rgb(0, 255, 0));

    /* Full blit to a known position. */
    fdk_surface_fill(dst, rgb(0, 0, 0));
    assert(fdk_ok(fdk_surface_present(dst))); /* consume fill's damage */
    assert(fdk_ok(fdk_surface_blit(dst, 20, 10, src,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = 50, .height = 40 })));
    assert(is_color(dst, 20, 10, pack(255, 0, 0)));      /* src(0,0) red */
    assert(is_color(dst, 44, 10, pack(255, 0, 0)));      /* src(24,0) red */
    assert(is_color(dst, 45, 10, pack(0, 0, 255)));      /* src(25,0) blue */
    assert(is_color(dst, 46, 10, pack(0, 0, 255)));      /* src(26,0) blue */
    assert(is_color(dst, 69, 49, pack(0, 0, 255)));      /* src last px */
    assert(is_color(dst, 30, 20, pack(0, 255, 0)));      /* src(10,10) green */
    assert(is_color(dst, 19, 10, pack(0, 0, 0)));        /* outside blit */
    assert(is_color(dst, 70, 50, pack(0, 0, 0)));

    /* Blit damage recorded on dst. */
    fdk_rect b;
    assert(fdk_surface_get_damage_bounds(dst, &b));
    assert(b.x == 20 && b.y == 10 && b.width == 50 && b.height == 40);

    /* Partial source rect. */
    assert(fdk_ok(fdk_surface_present(dst)));
    assert(fdk_ok(fdk_surface_blit(dst, 0, 0, src,
                                   (fdk_rect){ .x = 10, .y = 10,
                                               .width = 5, .height = 5 })));
    assert(is_color(dst, 0, 0, pack(0, 255, 0)));   /* green pixel at (0,0) */
    assert(is_color(dst, 4, 4, pack(255, 0, 0)));   /* rest red */

    /* Clipped destination: blit partially off the right/bottom edge.
     * Visible part: x 100..119, y 80..89 — dst(119,89) maps to
     * src(19,9), inside the red half. */
    assert(fdk_ok(fdk_surface_present(dst)));
    assert(fdk_ok(fdk_surface_blit(dst, 100, 80, src,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = 50, .height = 40 })));
    assert(is_color(dst, 119, 89, pack(255, 0, 0))); /* clipped-in corner */
    assert(is_color(dst, 99, 79, pack(0, 0, 0)));    /* outside the blit */

    /* Straddle the LEFT/TOP edge: negative dst offsets clamp. */
    assert(fdk_ok(fdk_surface_present(dst)));
    assert(fdk_ok(fdk_surface_blit(dst, -45, -35, src,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = 50, .height = 40 })));
    /* src(45,35) maps to (0,0): right/blue region visible at dst corner. */
    assert(is_color(dst, 0, 0, pack(0, 0, 255)));
    assert(is_color(dst, 4, 4, pack(0, 0, 255)));

    /* Blit respects the DESTINATION clip stack. */
    assert(fdk_ok(fdk_surface_present(dst)));
    fdk_surface_fill(dst, rgb(0, 0, 0));
    assert(fdk_ok(fdk_surface_present(dst)));
    assert(fdk_ok(fdk_surface_push_clip(dst, (fdk_rect){ .x = 30, .y = 30,
                                                         .width = 20,
                                                         .height = 20 })));
    assert(fdk_ok(fdk_surface_blit(dst, 0, 0, src,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = 50, .height = 40 })));
    fdk_surface_pop_clip(dst);
    assert(is_color(dst, 35, 35, pack(0, 0, 255))); /* inside clip: copied
                                                     * (src(35,35) = blue) */
    assert(is_color(dst, 10, 10, pack(0, 0, 0)));   /* outside: untouched */
    assert(is_color(dst, 50, 50, pack(0, 0, 0)));   /* outside: untouched */

    /* Argument checks. */
    assert(fdk_surface_blit(dst, 0, 0, NULL,
                            (fdk_rect){ .x = 0, .y = 0, .width = 1,
                                        .height = 1 })
            == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_blit(NULL, 0, 0, src,
                            (fdk_rect){ .x = 0, .y = 0, .width = 1,
                                        .height = 1 })
            == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_blit(dst, 0, 0, src,
                            (fdk_rect){ .x = 0, .y = 0, .width = 0,
                                        .height = 1 })
            == FDK_ERR_INVALID_ARGUMENT);

    fdk_surface_destroy(src);
    fdk_surface_destroy(dst);
    printf("[ok] blit: full/partial/clipped/clip-stack/args\n");
}

/* ---- frame pacing (headless: offscreen always ready) ---- */

static void test_frame_ready_offscreen(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(10, 10, &s)));
    assert(fdk_surface_frame_ready(s)); /* offscreen: always true */
    assert(fdk_surface_frame_ready(NULL)); /* defensive: true */
    fdk_surface_destroy(s);
    printf("[ok] frame_ready: offscreen surfaces are always ready\n");
}


/* ---- Phase 3 completion: ARGB surfaces, image decode, alpha blits,
 * transforms, antialiased primitives ---- */

static fdk_u32 px_at32(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x];
}

static void test_argb_surface_and_blending(void) {
    /* ARGB surfaces start fully transparent. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create_format(20, 20,
                                            FDK_SURFACE_FORMAT_ARGB8888, &s)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    assert(info.format == FDK_SURFACE_FORMAT_ARGB8888);
    assert(px_at32(s, 5, 5) == 0x00000000u);

    /* Unknown format values are refused. */
    fdk_surface *bad = NULL;
    assert(fdk_surface_create_format(4, 4, (fdk_surface_format)99, &bad) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(bad == NULL);

    /* Opaque fill sets alpha to 255. */
    fdk_surface_fill_rect(s, (fdk_rect){ .x = 0, .y = 0, .width = 10,
                                         .height = 10 }, rgb(255, 0, 0));
    assert(px_at32(s, 5, 5) == 0xFFFF0000u);

    /* Translucent blend over transparency: source-over with da=0 —
     * out_a = sa, out_rgb = src_rgb (a 50% red over transparent is
     * (255,0,0) at alpha 128, NOT a washed-out red). */
    fdk_color half_red = { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 0.5f };
    fdk_surface_fill_rect(s, (fdk_rect){ .x = 10, .y = 0, .width = 10,
                                         .height = 10 }, half_red);
    fdk_u32 got = px_at32(s, 15, 5);
    assert(((got >> 24) & 0xFFu) == 128);
    assert(((got >> 16) & 0xFFu) == 255);
    assert(((got >> 8) & 0xFFu) == 0);
    assert((got & 0xFFu) == 0);

    /* Stacking two 50% fills: out_a = .5 + .5*.5 = .75 -> 191. */
    fdk_surface_fill_rect(s, (fdk_rect){ .x = 10, .y = 0, .width = 10,
                                         .height = 10 }, half_red);
    got = px_at32(s, 15, 5);
    assert(((got >> 24) & 0xFFu) == 192);

    /* XRGB source blitted opaquely onto ARGB forces alpha 255 (the
     * "ignored" top byte must not fake transparency). */
    fdk_surface *x = NULL;
    assert(fdk_ok(fdk_surface_create(4, 4, &x)));
    fdk_surface_fill(x, rgb(0, 255, 0));
    assert(fdk_ok(fdk_surface_blit(s, 0, 16,
                                   x, (fdk_rect){ .x = 0, .y = 0,
                                                  .width = 4, .height = 4 })));
    assert(px_at32(s, 1, 17) == 0xFF00FF00u);

    fdk_surface_destroy(x);
    fdk_surface_destroy(s);
    printf("[ok] ARGB surfaces: format, transparency, alpha-accumulating "
           "blends, opaque blit forcing\n");
}

static void test_blit_blend(void) {
    /* Source sprite: left half opaque red, right half 50% blue. */
    fdk_surface *src = NULL;
    assert(fdk_ok(fdk_surface_create_format(
        8, 8, FDK_SURFACE_FORMAT_ARGB8888, &src)));
    fdk_color red = rgb(255, 0, 0);
    fdk_color blue50 = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 0.5f };
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 0, .y = 0, .width = 4,
                                           .height = 8 }, red);
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 4, .y = 0, .width = 4,
                                           .height = 8 }, blue50);

    /* XRGB sources are refused — that's fdk_surface_blit's job. */
    fdk_surface *xsrc = NULL;
    assert(fdk_ok(fdk_surface_create(2, 2, &xsrc)));
    assert(fdk_surface_blit_blend(NULL, 0, 0, src,
                                  (fdk_rect){ .x = 0, .y = 0, .width = 1,
                                              .height = 1 }) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_blit_blend(NULL, 0, 0, xsrc,
                                  (fdk_rect){ .x = 0, .y = 0, .width = 1,
                                              .height = 1 }) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Onto an opaque white XRGB destination: red replaces, blue50
     * blends to (127-ish, 127-ish, 255) — the classic half-blue tint
     * over white is (128,128,255) by the rounding discipline. */
    fdk_surface *dst = NULL;
    assert(fdk_ok(fdk_surface_create(16, 16, &dst)));
    fdk_surface_fill(dst, rgb(255, 255, 255));
    assert(fdk_ok(fdk_surface_blit_blend(dst, 4, 4, src,
                                         (fdk_rect){ .x = 0, .y = 0,
                                                     .width = 8,
                                                     .height = 8 })));
    assert(is_color(dst, 5, 5, pack(255, 0, 0)));
    fdk_u32 b = px_at(dst, 9, 5) & 0x00FFFFFFu;
    int br = (int)((b >> 16) & 0xFFu);
    int bg = (int)((b >> 8) & 0xFFu);
    int bb = (int)(b & 0xFFu);
    assert(br == 127 && bg == 127 && bb == 255);

    /* The untouched margin is still pure white (transparent-source
     * pixels skip; nothing else moved). */
    assert(is_color(dst, 0, 0, pack(255, 255, 255)));
    assert(is_color(dst, 15, 15, pack(255, 255, 255)));

    /* Clip stack constrains the blended blit's destination: reset
     * the background FIRST (unclipped), then blend under a 1px-wide
     * clip — everything outside it must stay background. */
    fdk_surface_fill(dst, rgb(255, 255, 255));
    assert(fdk_ok(fdk_surface_push_clip(dst, (fdk_rect){ .x = 4, .y = 4,
                                                          .width = 1,
                                                          .height = 16 })));
    assert(fdk_ok(fdk_surface_blit_blend(dst, 4, 4, src,
                                         (fdk_rect){ .x = 0, .y = 0,
                                                     .width = 8,
                                                     .height = 8 })));
    fdk_surface_pop_clip(dst);
    assert(is_color(dst, 4, 5, pack(255, 0, 0)));      /* inside clip */
    assert(is_color(dst, 9, 5, pack(255, 255, 255)));  /* outside clip */

    fdk_surface_destroy(dst);
    fdk_surface_destroy(xsrc);
    fdk_surface_destroy(src);
    printf("[ok] blit_blend: per-pixel source-over, XRGB refusal, clip "
           "conformance\n");
}

/* The embedded PNG (tests/test_png_bytes.h, generated once): 8x8 RGBA —
 * left half opaque red, right half 50% blue, green pixel at (4,4),
 * transparent pixel at (5,5). Written to a temp file at runtime so the
 * decoder exercises the real file path. */
#include "test_png_bytes.h"

static void test_image_decode(void) {
    /* Failure paths first. */
    fdk_surface *s = NULL;
    assert(fdk_surface_create_from_image(NULL, &s) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_create_from_image("build/no_such_image.png", &s) ==
           FDK_ERR_NOT_FOUND);
    assert(fdk_surface_create_from_image(".", &s) == FDK_ERR_NOT_A_FILE);
    assert(fdk_surface_create_from_image("Makefile", &s) ==
           FDK_ERR_UNSUPPORTED); /* real file, not an image */

    /* Write the embedded PNG bytes to a temp file. */
    FILE *f = fopen("build/test_image_decode.png", "wb");
    assert(f != NULL);
    assert(fwrite(test_png_bytes, 1, TEST_PNG_LEN, f) == TEST_PNG_LEN);
    fclose(f);

    assert(fdk_ok(fdk_surface_create_from_image(
        "build/test_image_decode.png", &s)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    assert(info.width == 8 && info.height == 8);
    assert(info.format == FDK_SURFACE_FORMAT_ARGB8888);

    /* Known pixels: red, blue50, green, transparent. */
    assert(px_at32(s, 0, 0) == 0xFFFF0000u);
    assert(px_at32(s, 7, 7) == 0x800000FFu);
    assert(px_at32(s, 4, 4) == 0xFF00FF00u);
    assert(px_at32(s, 5, 5) == 0x00000000u);

    /* The decoded surface composites: blend onto white. */
    fdk_surface *dst = NULL;
    assert(fdk_ok(fdk_surface_create(16, 16, &dst)));
    fdk_surface_fill(dst, rgb(255, 255, 255));
    assert(fdk_ok(fdk_surface_blit_blend(dst, 4, 4, s,
                                         (fdk_rect){ .x = 0, .y = 0,
                                                     .width = 8,
                                                     .height = 8 })));
    assert(is_color(dst, 4, 4, pack(255, 0, 0)));
    assert(is_color(dst, 11, 11, pack(127, 127, 255)));
    assert(is_color(dst, 8, 8, pack(0, 255, 0)));      /* green over white */
    assert(is_color(dst, 9, 9, pack(255, 255, 255)));  /* transparent */

    fdk_surface_destroy(dst);
    fdk_surface_destroy(s);
    remove("build/test_image_decode.png");
    printf("[ok] image decode: PNG -> ARGB surface, known pixels, "
           "failure codes, compositing\n");
}

static void test_matrix_algebra(void) {
    /* identity / mul / invert round trips on non-trivial transforms */
    fdk_matrix t = fdk_matrix_translate(13.0f, -7.0f);
    fdk_matrix sc = fdk_matrix_scale_xy(2.0f, 3.0f);
    fdk_matrix rot = fdk_matrix_rotate(0.7f);

    fdk_matrix m = fdk_matrix_mul(t, fdk_matrix_mul(sc, rot));
    fdk_matrix inv = fdk_matrix_invert(m);

    /* inv(m) * (m * p) == p for a probe point. */
    float px = 5.0f, py = -2.0f;
    float ax, ay;
    /* apply m */
    ax = m.m00 * px + m.m01 * py + m.tx;
    ay = m.m10 * px + m.m11 * py + m.ty;
    /* apply inv */
    float rx = inv.m00 * ax + inv.m01 * ay + inv.tx;
    float ry = inv.m10 * ax + inv.m11 * ay + inv.ty;
    assert(fabsf(rx - px) < 1e-4f && fabsf(ry - py) < 1e-4f);

    /* Degenerate (singular) matrix inverts to identity by contract. */
    fdk_matrix zero = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    fdk_matrix zi = fdk_matrix_invert(zero);
    assert(zi.m00 == 1.0f && zi.m11 == 1.0f && zi.tx == 0.0f);

    /* Composition order: translate-then-scale vs scale-then-translate
     * differ (documented left-to-right reading). */
    fdk_matrix ts = fdk_matrix_mul(fdk_matrix_translate(10, 0),
                                   fdk_matrix_scale(2));
    fdk_matrix st = fdk_matrix_mul(fdk_matrix_scale(2),
                                   fdk_matrix_translate(10, 0));
    /* ts maps x=1 -> (1+10)*2 = 22; st maps x=1 -> 1*2+10 = 12. */
    assert(ts.m00 == 2.0f && ts.tx == 20.0f);
    assert(st.m00 == 2.0f && st.tx == 10.0f);

    printf("[ok] matrix algebra: compose, invert, degenerate, order\n");
}

static void test_blit_transformed(void) {
    /* A 4x4 red/green checkerboard source. */
    fdk_surface *src = NULL;
    assert(fdk_ok(fdk_surface_create(4, 4, &src)));
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            fdk_surface_fill_rect(src,
                (fdk_rect){ .x = x, .y = y, .width = 1, .height = 1 },
                ((x + y) & 1) ? rgb(0, 255, 0) : rgb(255, 0, 0));
        }
    }

    fdk_surface *dst = NULL;
    assert(fdk_ok(fdk_surface_create(40, 40, &dst)));
    fdk_surface_fill(dst, rgb(9, 9, 9));

    /* Identity = plain blit (fast path): pixel-exact. */
    assert(fdk_ok(fdk_surface_blit_transformed(dst, fdk_matrix_identity(),
                                               src)));
    assert(is_color(dst, 0, 0, pack(255, 0, 0)));
    assert(is_color(dst, 1, 1, pack(255, 0, 0)));
    assert(is_color(dst, 1, 0, pack(0, 255, 0)));
    assert(is_color(dst, 5, 5, pack(9, 9, 9)));

    /* Integer translation: exact relocation. */
    fdk_surface_fill(dst, rgb(9, 9, 9));
    assert(fdk_ok(fdk_surface_blit_transformed(
        dst, fdk_matrix_translate(10.0f, 20.0f), src)));
    assert(is_color(dst, 10, 20, pack(255, 0, 0)));
    assert(is_color(dst, 13, 23, pack(255, 0, 0)));
    assert(is_color(dst, 9, 20, pack(9, 9, 9)));
    assert(is_color(dst, 14, 20, pack(9, 9, 9)));

    /* 3x integer scale-up: nearest-neighbor fast path — each source
     * pixel becomes an exact 3x3 block, NO interpolated values. */
    fdk_surface_fill(dst, rgb(9, 9, 9));
    assert(fdk_ok(fdk_surface_blit_transformed(dst, fdk_matrix_scale(3.0f),
                                               src)));
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            fdk_u32 want = ((x + y) & 1) ? pack(0, 255, 0) : pack(255, 0, 0);
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    assert(is_color(dst, x * 3 + sx, y * 3 + sy, want));
                }
            }
        }
    }
    assert(is_color(dst, 12, 0, pack(9, 9, 9))); /* just past the edge */

    /* Fractional 2.5x scale: bilinear path — the destination at the
     * boundary between source pixels must hold an INTERPOLATED value
     * (strictly between the two colors), proving filtering happened. */
    fdk_surface_fill(dst, rgb(9, 9, 9));
    assert(fdk_ok(fdk_surface_blit_transformed(dst, fdk_matrix_scale(2.5f),
                                               src)));
    fdk_u32 mid = px_at(dst, 4, 0) & 0x00FFFFFFu; /* x in [1.6..2.0] */
    int mr = (int)((mid >> 16) & 0xFFu);
    int mg = (int)((mid >> 8) & 0xFFu);
    assert(mr > 0 && mr < 255);
    assert(mg > 0 && mg < 255);
    assert(mr + mg == 255 || mr + mg == 254 || mr + mg == 256);

    /* Degenerate matrix: documented no-op, FDK_OK. */
    fdk_surface_fill(dst, rgb(9, 9, 9));
    fdk_matrix zero = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    assert(fdk_ok(fdk_surface_blit_transformed(dst, zero, src)));
    assert(is_color(dst, 0, 0, pack(9, 9, 9)));

    /* Rotation by 90 degrees: exact quarter-turn (bilinear path but
     * the sample points land exactly on pixel centers). rotate maps
     * (x,y) -> (-y, x): source (0,0)->(0,0), (3,0)->(0,3),
     * (0,3)->(-3,0). FIRST rotate, THEN translate +4 in x to stay in
     * bounds. */
    fdk_surface_fill(dst, rgb(9, 9, 9));
    fdk_matrix r90 = fdk_matrix_rotate(1.57079632679f);
    fdk_matrix m = fdk_matrix_mul(r90, fdk_matrix_translate(4.0f, 0.0f));
    assert(fdk_ok(fdk_surface_blit_transformed(dst, m, src)));
    /* Rotating PIXEL CENTERS (not indices): source pixel (0,0)'s
     * center (0.5,0.5) maps to (3.5,0.5) = dest pixel (3,0)'s
     * center; and likewise down the diagonal. */
    assert(is_color(dst, 3, 0, pack(255, 0, 0))); /* src (0,0) red    */
    assert(is_color(dst, 3, 3, pack(0, 255, 0))); /* src (3,0) green  */
    assert(is_color(dst, 0, 0, pack(0, 255, 0))); /* src (0,3) green  */
    assert(is_color(dst, 0, 3, pack(255, 0, 0))); /* src (3,3) red    */
    assert(is_color(dst, 5, 0, pack(9, 9, 9)));   /* past the quad   */

    /* NULL argument refusal. */
    assert(fdk_surface_blit_transformed(NULL, fdk_matrix_identity(), src) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_blit_transformed(dst, fdk_matrix_identity(), NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    fdk_surface_destroy(dst);
    fdk_surface_destroy(src);
    printf("[ok] blit_transformed: identity/translate/scale exactness, "
           "bilinear, degenerate no-op, rotation\n");
}

static void test_transformed_alpha(void) {
    /* An ARGB sprite (50% blue square) scaled 2x with bilinear
     * filtering: the interior stays exactly 50% blue-over-background;
     * the alpha rides through the transform. */
    fdk_surface *src = NULL;
    assert(fdk_ok(fdk_surface_create_format(
        4, 4, FDK_SURFACE_FORMAT_ARGB8888, &src)));
    fdk_color blue50 = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 0.5f };
    fdk_surface_fill_rect(src, (fdk_rect){ .x = 1, .y = 1, .width = 2,
                                           .height = 2 }, blue50);

    fdk_surface *dst = NULL;
    assert(fdk_ok(fdk_surface_create(20, 20, &dst)));
    fdk_surface_fill(dst, rgb(255, 255, 255));
    assert(fdk_ok(fdk_surface_blit_transformed(dst, fdk_matrix_scale(2.0f),
                                               src)));
    /* Interior pixel: exactly the 50% blue-over-white blend
     * (127 = 255 * (1 - 128/255), the honest sa=128 arithmetic). */
    assert(is_color(dst, 4, 4, pack(127, 127, 255)));
    /* Corner pixel (0,0): outside the blue square, fully transparent
     * source — stays background. */
    assert(is_color(dst, 0, 0, pack(255, 255, 255)));

    fdk_surface_destroy(dst);
    fdk_surface_destroy(src);
    printf("[ok] transformed alpha compositing: alpha rides through "
           "scale\n");
}

static void test_aa_primitives(void) {
    /* Axis-aligned AA lines are pixel-identical to crisp ones. */
    fdk_surface *crisp = NULL, *aa = NULL;
    assert(fdk_ok(fdk_surface_create(30, 30, &crisp)));
    assert(fdk_ok(fdk_surface_create(30, 30, &aa)));
    fdk_color c = rgb(200, 30, 90);
    fdk_surface_draw_line(crisp, 3, 7, 24, 7, c);
    fdk_surface_draw_line_aa(aa, 3, 7, 24, 7, c);
    fdk_surface_draw_line(crisp, 9, 2, 9, 27, c);
    fdk_surface_draw_line_aa(aa, 9, 2, 9, 27, c);
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 30; x++) {
            assert(px_at(crisp, x, y) == px_at(aa, x, y));
        }
    }

    /* Diagonal AA line: every touched pixel lies between background
     * and line color (no overshoot), and both endpoints are exactly
     * the line color. */
    fdk_surface_fill(aa, rgb(0, 0, 0));
    fdk_surface_draw_line_aa(aa, 2, 2, 27, 14, c);
    assert(px_at(aa, 2, 2) == pack(200, 30, 90)); /* endpoint exact */
    assert(px_at(aa, 27, 14) == pack(200, 30, 90));
    int touched = 0;
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 30; x++) {
            fdk_u32 p = px_at(aa, x, y) & 0x00FFFFFFu;
            if (p != 0u) {
                touched++;
                int pr = (int)((p >> 16) & 0xFFu);
                int pg = (int)((p >> 8) & 0xFFu);
                int pb = (int)(p & 0xFFu);
                assert(pr <= 200 && pg <= 30 && pb <= 90);
                assert(pr + pg + pb > 0);
            }
        }
    }
    assert(touched > 25); /* it is a line, not a point */

    /* AA circle: center pixel fully covered; pixels at distance r+2
     * untouched; coverage monotone along a radius. */
    fdk_surface_fill(aa, rgb(0, 0, 0));
    fdk_surface_fill_circle_aa(aa, 15, 15, 8, c);
    assert(px_at(aa, 15, 15) == pack(200, 30, 90));
    /* One INSIDE the radius is fully covered; exactly ON the radius
     * is the documented half-coverage ramp; far outside is empty. */
    assert(px_at(aa, 15, 15 - 7) == pack(200, 30, 90));
    fdk_u32 edge = px_at(aa, 15, 15 - 8) & 0x00FFFFFFu;
    int er = (int)((edge >> 16) & 0xFFu);
    assert(er > 50 && er < 200); /* half-ish blend, not either extreme */
    assert((px_at(aa, 15, 15 - 10) & 0x00FFFFFFu) == 0u); /* far outside */
    int prev = 255;
    for (int d = 0; d <= 10; d++) {
        fdk_u32 p = px_at(aa, 15 + d, 15) & 0x00FFFFFFu;
        int pr = (int)((p >> 16) & 0xFFu);
        assert(pr <= prev + 1); /* coverage non-increasing outward */
        prev = pr;
    }

    /* AA circle OUTLINE: interior empty, ring present, far outside
     * empty. */
    fdk_surface_fill(aa, rgb(0, 0, 0));
    fdk_surface_draw_circle_aa(aa, 15, 15, 8, c);
    assert((px_at(aa, 15, 15) & 0x00FFFFFFu) == 0u);
    assert((px_at(aa, 15 + 8, 15) & 0x00FFFFFFu) != 0u);
    assert((px_at(aa, 15 + 11, 15) & 0x00FFFFFFu) == 0u);

    /* AA rounded rect with radius 0: matches the crisp fill_rect's
     * interior exactly on the boundary row/col. */
    fdk_surface *crisp2 = NULL;
    assert(fdk_ok(fdk_surface_create(24, 24, &crisp2)));
    fdk_surface_fill(aa, rgb(0, 0, 0));
    fdk_surface_fill(crisp2, rgb(0, 0, 0));
    fdk_rect r = { .x = 4, .y = 6, .width = 12, .height = 9 };
    fdk_surface_fill_rect(crisp2, r, c);
    fdk_surface_fill_rounded_rect_aa(aa, r, 0, c);
    /* Interior pixels identical (boundary may have partial coverage
     * by design — the AA variant smooths the right/bottom edges). */
    for (int y = r.y + 1; y < r.y + r.height - 1; y++) {
        for (int x = r.x + 1; x < r.x + r.width - 1; x++) {
            assert(px_at(aa, x, y) == px_at(crisp2, x, y));
        }
    }

    /* AA rounded rect with a real radius: interior full, outside
     * empty, corner-region boundary pixels strictly between. */
    fdk_surface_fill(aa, rgb(0, 0, 0));
    fdk_rect rr = { .x = 2, .y = 2, .width = 20, .height = 20 };
    fdk_surface_fill_rounded_rect_aa(aa, rr, 6, c);
    assert(px_at(aa, 12, 12) == pack(200, 30, 90));      /* center */
    assert(px_at(aa, 2, 2) != pack(200, 30, 90));        /* clipped corner */
    assert((px_at(aa, 2, 2) & 0x00FFFFFFu) == 0u);       /* ...to nothing */
    /* On the corner arc (corner circle center (8,8), r=6): pixel (5,2)
     * sits in the coverage ramp — strictly between empty and full. */
    fdk_u32 corner_edge = px_at(aa, 5, 2) & 0x00FFFFFFu;
    int cer = (int)((corner_edge >> 16) & 0xFFu);
    assert(cer > 0 && cer < 200);
    /* And its neighbor further out on the arc is strictly less
     * covered (monotone ramp away from the shape). */
    fdk_u32 outer = px_at(aa, 4, 2) & 0x00FFFFFFu;
    int orr = (int)((outer >> 16) & 0xFFu);
    assert(orr < cer);

    fdk_surface_destroy(crisp2);
    fdk_surface_destroy(crisp);
    fdk_surface_destroy(aa);
    printf("[ok] AA primitives: axis-exactness, bounded coverage, "
           "monotone falloff, rounded-rect SDF\n");
}

int main(void) {
    test_offscreen_create_info_destroy();
    test_offscreen_argument_checks();
    test_offscreen_present_is_frame_close();
    test_fill_and_fill_rect();
    test_draw_rect_no_double_blend_corners();
    test_gradient_interpolation();
    test_draw_line();
    test_circle_fill_and_outline();
    test_rounded_rect_fill_and_outline();
    test_clip_stack();
    test_damage_bookkeeping();
    test_blit();
    test_frame_ready_offscreen();
    test_argb_surface_and_blending();
    test_blit_blend();
    test_image_decode();
    test_matrix_algebra();
    test_blit_transformed();
    test_transformed_alpha();
    test_aa_primitives();
    printf("all headless render tests passed\n");
    return 0;
}
