/* gen_app_icon.c — draws the fdk-files app icon with FDK itself and
 * saves it as PNG (§26/3b).
 *
 * 48x48 (the size the hicolor theme convention requires for the
 * apps/ dir; also 128 for the README/screenshots). The drawing is a
 * folder with a document + a small accent bar — recognizable at 48,
 * honest at 128 (no fake shadows; flat fills + the round-rect AA).
 *
 * Run from test-apps/: ./build/gen-fdk-files-icon out.png [size]
 */
#include "fdk/fdk.h"

#include <stdio.h>
#include <stdlib.h>

static fdk_color pal(int r, int g, int b, int a) {
    return (fdk_color){(fdk_f32)r / 255.0f, (fdk_f32)g / 255.0f,
                       (fdk_f32)b / 255.0f, (fdk_f32)a / 255.0f};
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s out.png [size]\n", argv[0]);
        return 2;
    }
    int size = (argc >= 3) ? atoi(argv[2]) : 48;
    if (size < 16 || size > 512) {
        fprintf(stderr, "size out of range: %d\n", size);
        return 2;
    }
    /* scale factor from the 48-grid */
    double k = (double)size / 48.0;

    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_surface_create_format(size, size,
                                          FDK_SURFACE_FORMAT_ARGB8888,
                                          &s)) ||
        s == NULL) {
        fprintf(stderr, "surface create failed\n");
        return 1;
    }
    /* transparent base */
    (void)fdk_surface_fill(s, pal(0, 0, 0, 0));

    /* the folder BACK: tab + body */
    fdk_rect tab = {.x = (fdk_i32)(4 * k), .y = (fdk_i32)(8 * k),
                    .width = (fdk_i32)(16 * k),
                    .height = (fdk_i32)(6 * k)};
    (void)fdk_surface_fill_rect(s, tab, pal(61, 106, 166, 255));
    fdk_rect body = {.x = (fdk_i32)(4 * k), .y = (fdk_i32)(12 * k),
                     .width = (fdk_i32)(40 * k),
                     .height = (fdk_i32)(26 * k)};
    (void)fdk_surface_fill_rounded_rect_aa(s, body,
                                           (fdk_i32)(2 * k),
                                           pal(70, 122, 192, 255));

    /* the folder FRONT (the classic open look): a lighter slab */
    fdk_rect front = {.x = (fdk_i32)(6 * k), .y = (fdk_i32)(18 * k),
                      .width = (fdk_i32)(36 * k),
                      .height = (fdk_i32)(18 * k)};
    (void)fdk_surface_fill_rounded_rect_aa(s, front,
                                           (fdk_i32)(2 * k),
                                           pal(94, 148, 217, 255));

    /* the DOCUMENT: white page, folded corner, three text lines */
    fdk_i32 px = (fdk_i32)(16 * k), py = (fdk_i32)(8 * k);
    fdk_i32 pw = (fdk_i32)(14 * k), ph = (fdk_i32)(18 * k);
    fdk_rect page = {.x = px, .y = py, .width = pw, .height = ph};
    (void)fdk_surface_fill_rounded_rect_aa(s, page, (fdk_i32)(k),
                                           pal(245, 247, 250, 255));
    /* the fold (a small triangle cut) */
    fdk_rect fold = {.x = px + pw - (fdk_i32)(4 * k),
                     .y = py, .width = (fdk_i32)(4 * k),
                     .height = (fdk_i32)(4 * k)};
    (void)fdk_surface_fill_rect(s, fold, pal(210, 216, 226, 255));
    /* text lines */
    for (int i = 0; i < 3; i++) {
        fdk_rect line = {.x = px + (fdk_i32)(2 * k),
                         .y = py + (fdk_i32)((6 + i * 4) * k),
                         .width = pw - (fdk_i32)((i == 2 ? 6 : 4) * k),
                         .height = (fdk_i32)(1.5 * k)};
        (void)fdk_surface_fill_rect(s, line,
                                    pal(120, 134, 156, 255));
    }

    /* the accent bar: the FDK selection blue at the folder base */
    fdk_rect bar = {.x = (fdk_i32)(10 * k), .y = (fdk_i32)(33 * k),
                    .width = (fdk_i32)(10 * k),
                    .height = (fdk_i32)(3 * k)};
    (void)fdk_surface_fill_rounded_rect_aa(s, bar, (fdk_i32)(k),
                                           pal(255, 194, 32, 255));

    fdk_result r = fdk_surface_save_png(s, argv[1]);
    fdk_surface_destroy(s);
    if (!fdk_ok(r)) {
        fprintf(stderr, "png save failed: %s\n",
                fdk_result_to_string(r));
        return 1;
    }
    printf("icon written: %s (%dx%d)\n", argv[1], size, size);
    return 0;
}
