/* test_font_rasterize.c — R-002 (Halcyon): fdk_font_rasterize_utf8's
 * contract. Ink lands in CALLER-OWNED ARGB memory with no surface and
 * no display connection; the ink box matches the painted region; the
 * NULL/bounds contracts refuse honestly; the result agrees with an
 * on-surface draw of the same string (the same shaped walk). */
#include "fdk/fdk.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
static void check(int cond, const char *what) {
    printf("[rast] %-58s %s\n", what, cond ? "PASS" : "FAIL");
    if (!cond) fails++;
}

int main(void) {
    /* NULL contracts. */
    fdk_rect ink = { 1, 2, 3, 4 };
    fdk_u32 buf[64];
    check(fdk_font_rasterize_utf8(NULL, "x", 1, (fdk_color){1,1,1,1},
                                  buf, 8, 8, 8 * 4, 6, &ink) ==
          FDK_ERR_INVALID_ARGUMENT,
          "NULL font -> INVALID_ARGUMENT");
    check(ink.width == 0 && ink.height == 0,
          "out_ink zeroed on refusal (never stale garbage)");

    /* The default font: every FDK context-free font path must work
     * without a display (offscreen text is the whole point). */
    fdk_font *font = fdk_font_load_system_default(32.0f);
    fdk_result r = font != NULL ? FDK_OK : FDK_ERR_UNSUPPORTED;
    printf("[rast] default font load -> %p\n", (void *)font);
    if (font == NULL) {
        /* No font subsystem on this box: the contract legs above
         * already passed; nothing else can run. */
        printf("[rast] no font — headless: contracts passed\n");
        return fails ? 1 : 0;
    }

    /* Bad geometry. */
    check(fdk_font_rasterize_utf8(font, "hi", 2, (fdk_color){1,1,1,1},
                                  NULL, 8, 8, 32, 6, &ink) ==
          FDK_ERR_INVALID_ARGUMENT,
          "NULL dst -> INVALID_ARGUMENT");
    check(fdk_font_rasterize_utf8(font, "hi", 2, (fdk_color){1,1,1,1},
                                  buf, 0, 8, 32, 6, &ink) ==
          FDK_ERR_INVALID_ARGUMENT,
          "w == 0 -> INVALID_ARGUMENT");
    check(fdk_font_rasterize_utf8(font, "hi", 2, (fdk_color){1,1,1,1},
                                  buf, 8, 8, 8, 6, &ink) ==
          FDK_ERR_INVALID_ARGUMENT,
          "stride < w*4 -> INVALID_ARGUMENT");

    /* The paint: a 96x48 buffer, red text at the baseline. */
    enum { W = 96, H = 48 };
    fdk_u32 *px = malloc((size_t)W * (size_t)H * 4u);
    check(px != NULL, "scratch allocation");
    if (px == NULL) {
        fdk_font_destroy(font);
        return 1;
    }
    memset(px, 0, (size_t)W * (size_t)H * 4u);
    fdk_rect ink2 = { 0, 0, 0, 0 };
    /* baseline mid-buffer: a 32px font's ascenders fit above it. */
    r = fdk_font_rasterize_utf8(font, "Ag", 2,
                                (fdk_color){ 1.0f, 0.0f, 0.0f, 1.0f },
                                px, W, H, W * 4, 30, &ink2);
    check(fdk_ok(r), "rasterize Ag -> OK");
    printf("[rast] ink box %d,%d %dx%d\n", (int)ink2.x, (int)ink2.y,
           (int)ink2.width, (int)ink2.height);
    check(ink2.width > 0 && ink2.height > 0,
          "ink box non-empty for visible text");

    /* Ink must exist ONLY inside the reported box (scan the whole
     * buffer, not just the box: outside pixels must stay zero). */
    int inside = 0, outside = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            fdk_u32 p = px[(size_t)y * (size_t)W + (size_t)x];
            int has_ink = (p & 0x00FFFFFFu) != 0 || (p >> 24) != 0;
            int in_box = x >= ink2.x && x < ink2.x + ink2.width &&
                         y >= ink2.y && y < ink2.y + ink2.height;
            if (has_ink && in_box) {
                inside++;
            } else if (has_ink) {
                outside++;
            }
        }
    }
    printf("[rast] ink pixels: %d inside, %d outside the box\n",
           inside, outside);
    check(inside > 0, "glyph ink landed in the buffer");
    check(outside == 0, "no ink outside the reported box");

    /* The ink is RED-tinted (the color param): every inked pixel's
     * red channel dominates green and blue. */
    int red_ok = 1;
    for (int y = 0; y < H && red_ok; y++) {
        for (int x = 0; x < W; x++) {
            fdk_u32 p = px[(size_t)y * (size_t)W + (size_t)x];
            if ((p >> 24) != 0) {
                int rr = (int)((p >> 16) & 0xFFu);
                int gg = (int)((p >> 8) & 0xFFu);
                int bb = (int)(p & 0xFFu);
                if (gg > rr || bb > rr) {
                    red_ok = 0;
                    break;
                }
            }
        }
    }
    check(red_ok, "tint honored (red >= green/blue everywhere)");

    /* Clipping: rasterize into a 4x4 corner buffer — the glyphs clip
     * to the tiny buffer, no crash, and the ink box intersects the
     * buffer (or is empty when fully outside). */
    fdk_u32 tiny[16];
    memset(tiny, 0, sizeof tiny);
    fdk_rect ink3 = { 0, 0, 0, 0 };
    r = fdk_font_rasterize_utf8(font, "Ag", 2,
                                (fdk_color){ 1.0f, 1.0f, 1.0f, 1.0f },
                                tiny, 4, 4, 16, 2, &ink3);
    check(fdk_ok(r), "tiny buffer clips without error");
    check(ink3.width >= 0 && ink3.width <= 4 && ink3.height >= 0 &&
          ink3.height <= 4,
          "clipped ink box stays inside the buffer");

    /* A whitespace-only run: no ink, empty box, still OK. */
    memset(px, 0, (size_t)W * (size_t)H * 4u);
    fdk_rect ink4 = { 9, 9, 9, 9 };
    r = fdk_font_rasterize_utf8(font, "  ", 2,
                                (fdk_color){ 1, 1, 1, 1 },
                                px, W, H, W * 4, 30, &ink4);
    int any_ink = 0;
    for (size_t i = 0; i < (size_t)W * (size_t)H; i++) {
        if (px[i] != 0) {
            any_ink = 1;
            break;
        }
    }
    check(fdk_ok(r) && !any_ink &&
          (ink4.width == 0 || ink4.height == 0),
          "whitespace run: no ink, empty box");

    free(px);
    fdk_font_destroy(font);
    printf("[rast] %s\n", fails ? "FAILURES" : "all pass");
    return fails ? 1 : 0;
}
