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
    fdk_color c = { .r = r / 255.0f, .g = g / 255.0f, .b = b / 255.0f,
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
    printf("all headless render tests passed\n");
    return 0;
}
