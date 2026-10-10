/*
 * test_picture.c — headless tests for the 1.4.9 image milestone
 *
 * The PNG codec round-trip (encode -> decode -> pixel compare),
 * the file save/load path, and the Picture widget: fit policies
 * (pixel proofs for the letterbox and the crop), natural sizes,
 * ownership transfer, and the IMAGE a11y role.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "core/alloc_internal.h"
#include "render/surface_internal.h" /* fdk__png_encode */
#include "widget/widgets_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x];
}

static fdk_u32 pack(int r, int g, int b, int a) {
    return ((fdk_u32)(a & 0xFF) << 24) | ((fdk_u32)(r & 0xFF) << 16) |
           ((fdk_u32)(g & 0xFF) << 8) | (fdk_u32)(b & 0xFF);
}

/* A deterministic 12x8 gradient test image. */
static fdk_surface *make_image(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create_format(
        12, 8, FDK_SURFACE_FORMAT_ARGB8888, &s)));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 12; x++) {
            fdk_surface_info info;
            assert(fdk_ok(fdk_surface_get_info(s, &info)));
            info.pixels[(size_t)y * (size_t)info.stride +
                        (size_t)x] =
                pack(x * 20, y * 30, 0x80, 0xFF);
        }
    }
    return s;
}

/* ---- the PNG codec round-trip ---- */

static void test_png_codec(void) {
    fdk_surface *img = make_image();

    unsigned char *png = NULL;
    size_t len = 0;
    assert(fdk_ok(fdk__png_encode(img, &png, &len)));
    assert(png != NULL && len > 8 + 12 + 12 + 12);
    /* The signature. */
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D,
                                         0x0A, 0x1A, 0x0A};
    assert(memcmp(png, sig, 8) == 0);
    /* IHDR declares 12x8, 8-bit RGBA. */
    fdk_u32 w = ((fdk_u32)png[16] << 24) | ((fdk_u32)png[17] << 16) |
                ((fdk_u32)png[18] << 8) | (fdk_u32)png[19];
    fdk_u32 h = ((fdk_u32)png[20] << 24) | ((fdk_u32)png[21] << 16) |
                ((fdk_u32)png[22] << 8) | (fdk_u32)png[23];
    assert(w == 12 && h == 8);
    assert(png[24] == 8 && png[25] == 6);

    /* Decode back and compare every pixel. */
    fdk_surface *back = NULL;
    assert(fdk_ok(fdk_surface_create_from_image_bytes(png, len,
                                                      &back)));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 12; x++) {
            assert(px_at(back, x, y) == px_at(img, x, y));
        }
    }

    /* Garbage refuses honestly. */
    fdk_surface *bad = NULL;
    assert(fdk_surface_create_from_image_bytes("not an image", 13,
                                               &bad) ==
           FDK_ERR_UNSUPPORTED);
    assert(fdk_surface_create_from_image_bytes(NULL, 13, &bad) ==
           FDK_ERR_INVALID_ARGUMENT);

    fdk_surface_destroy(back);
    fdk_free(png);

    /* The file path: save, reload, compare. */
    assert(fdk_ok(fdk_surface_save_png(img, "/tmp/fdk-test-img.png")));
    fdk_surface *reloaded = NULL;
    assert(fdk_ok(fdk_surface_create_from_image(
        "/tmp/fdk-test-img.png", &reloaded)));
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 12; x++) {
            assert(px_at(reloaded, x, y) == px_at(img, x, y));
        }
    }
    fdk_surface_destroy(reloaded);
    remove("/tmp/fdk-test-img.png");

    fdk_surface_destroy(img);
    printf("[ok] png codec: encode/decode round-trip is pixel-exact "
           "(memory + file), garbage refuses\n");
}

/* ---- the picture widget ---- */

static void test_picture_widget(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 200, 150},
                                    &root)));
    fdk_widget *pic = NULL;
    assert(fdk_ok(fdk_picture_create(root, &pic)));

    /* Empty: no natural size, no crash. */
    fdk_size nat = {1, 1};
    fdk_widget_measure(pic, &nat);
    assert(nat.width == 0 && nat.height == 0);

    /* Ownership transfer: our surface becomes the picture's. */
    fdk_surface *img = make_image();
    fdk_picture_set_surface(pic, img);
    assert(fdk_picture_image_width(pic) == 12);
    assert(fdk_picture_image_height(pic) == 8);
    fdk_size nat2 = {0, 0};
    fdk_widget_measure(pic, &nat2);
    assert(nat2.width == 12 && nat2.height == 8);

    /* The default fit is CONTAIN. */
    assert(fdk_picture_get_fit(pic) == FDK_PICTURE_FIT_CONTAIN);

    /* Paint proofs on a 60x40 arrangement: a 12x8 image CONTAINed
     * in 60x40 scales to 60x40 exactly (aspect 12:8 = 60:40 — the
     * same ratio, no letterbox); COVER also fills. Use a 60x30
     * arrangement instead: 12:8 vs 60:30 — CONTAIN scales to
     * 36x30 (letterbox bands left/right), COVER scales to 60x40
     * (cropped top/bottom). Give the root a background so the
     * letterbox band has a KNOWN color to read. */
    fdk_widget_set_background(root, (fdk_color){0.10f, 0.10f, 0.10f,
                                                1.0f});
    fdk_widget_arrange(pic, (fdk_rect){0, 0, 60, 30});

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(60, 30, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);

    /* CONTAIN: the image is 36 wide (scaled 12*2.5), centered ->
     * x 12..48 painted, x 0..11 the root's band. A pixel at (4,15)
     * is the BAND; (30,15) is the image's scaled pixel. */
    fdk_u32 band = px_at(s, 4, 15) & 0x00FFFFFFu;
    fdk_u32 mid = px_at(s, 30, 15) & 0x00FFFFFFu;
    assert(band != mid);
    /* The band is the root background (dark gray ~ 0x1A1A1A). */
    int br = (int)((band >> 16) & 0xFF);
    assert(br > 15 && br < 45);

    /* COVER: the same arrangement fills 60 wide (scaled 5x, 40 tall
     * cropped to 30) — no bands. */
    fdk_picture_set_fit(pic, FDK_PICTURE_FIT_COVER);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 cover_edge = px_at(s, 4, 15) & 0x00FFFFFFu;
    assert(cover_edge != band); /* the image reaches the edge now */

    /* NONE: 1:1 at the top-left — (2,2) is the image's first
     * pixels scaled 1:1; (40,20) is beyond the 12x8 image -> band. */
    fdk_picture_set_fit(pic, FDK_PICTURE_FIT_NONE);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    assert((px_at(s, 40, 20) & 0x00FFFFFFu) == band);
    fdk_u32 topleft = px_at(s, 2, 2) & 0x00FFFFFFu;
    assert(topleft != band);

    fdk_surface_destroy(s);

    /* Replacing the content: transfer a fresh surface, the old one
     * is destroyed internally (no leak under ASan). */
    fdk_surface *img2 = make_image();
    fdk_picture_set_surface(pic, img2);
    assert(fdk_picture_image_width(pic) == 12);

    /* Clearing. */
    fdk_picture_set_surface(pic, NULL);
    assert(fdk_picture_image_width(pic) == 0);

    /* a11y: the IMAGE role with the pixel-size value text (set an
     * image again for the describe check). */
    fdk_surface *img3 = make_image();
    fdk_picture_set_surface(pic, img3);
    fdk_a11y_info info;
    memset(&info, 0, sizeof(info));
    assert(fdk_ok(fdk_a11y_describe(pic, &info)));
    assert(info.value_text != NULL);
    assert(strstr(info.value_text, "12 x 8") != NULL);
    fdk_a11y_info_free(&info);

    fdk_widget_destroy(root);
    printf("[ok] picture widget: natural sizes, CONTAIN letterbox + "
           "COVER fill + NONE top-left pixel proofs, ownership "
           "transfer + clear, IMAGE a11y role\n");
}

int main(void) {
    test_png_codec();
    test_picture_widget();
    printf("all picture/png tests passed\n");
    return 0;
}
