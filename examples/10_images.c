/*
 * 10_images.c — Phase 3 completion demo: images, alpha compositing,
 * transforms, and antialiased primitives.
 *
 * Four panels, all software-rendered through fdk_surface:
 *
 *   1. IMAGE      — examples/data/fdk_logo.png decoded by
 *                   fdk_surface_create_from_image (vendored stb_image)
 *                   and composited with fdk_surface_blit_blend: the
 *                   PNG's 50%-alpha band actually blends over the
 *                   panel background.
 *   2. TRANSFORM  — the same image through fdk_surface_blit_transformed:
 *                   exact 2x integer scale-up (nearest-neighbor block
 *                   pixels), a 30-degree rotation (bilinear), and a
 *                   fractional 2.5x scale (bilinear) in sequence.
 *   3. AA SHAPES  — crisp vs antialiased variants side by side: lines,
 *                   circle outlines, filled circles, rounded rects.
 *   4. ALPHA      — an ARGB offscreen surface built at runtime (a
 *                   translucent rounded rect + translucent circle),
 *                   blit_blend'ed twice — the accumulation is visible
 *                   where the two shapes overlap.
 *
 * RIG markers (stdout "RIG: x y w h label") let scripts find each
 * panel for pixel verification; ESC or window close exits.
 */

#include "fdk/fdk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_surface *image;   /* decoded PNG (ARGB) */
    fdk_surface *sprite;  /* runtime-built ARGB sprite (panel 4) */
    int frame;
    bool quit;
} app_state;

static void on_event(fdk_window *window, const fdk_event_data *event,
                     void *user_data) {
    app_state *app = user_data;
    (void)window;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        app->quit = true;
    } else if (event->type == FDK_EVENT_KEY_DOWN &&
               event->key.codepoint == 27 /* ESC */) {
        app->quit = true;
    }
}

static fdk_color palette_bg = { .r = 0.07f, .g = 0.08f, .b = 0.10f, .a = 1.0f };
static fdk_color panel_bg = { .r = 0.13f, .g = 0.14f, .b = 0.17f, .a = 1.0f };
static fdk_color ink = { .r = 0.92f, .g = 0.93f, .b = 0.96f, .a = 1.0f };
static fdk_color accent = { .r = 0.20f, .g = 0.78f, .b = 0.62f, .a = 1.0f };
static fdk_color warn = { .r = 0.95f, .g = 0.45f, .b = 0.25f, .a = 1.0f };

static void draw_panel_frame(fdk_surface *s, fdk_rect r) {
    fdk_surface_fill_rect(s, r, panel_bg);
    fdk_surface_draw_rect(s, r, (fdk_color){ .r = 0.25f, .g = 0.27f,
                                             .b = 0.32f, .a = 1.0f });
}

int main(int argc, char **argv) {
    app_state app = { 0 };
    const char *image_path = "examples/data/fdk_logo.png";
    if (argc > 1) {
        image_path = argv[1];
    }

    fdk_init_options opts = { .app_id = "org.fdk.demo.images" };
    fdk_result r = fdk_init(&app.ctx, &opts);
    if (!fdk_ok(r)) {
        fprintf(stderr, "10_images: no display (%d)\n", (int)r);
        return 1;
    }

    r = fdk_surface_create_from_image(image_path, &app.image);
    if (!fdk_ok(r)) {
        fprintf(stderr, "10_images: cannot decode %s (%d)\n", image_path,
                (int)r);
        fdk_shutdown(app.ctx);
        return 1;
    }

    /* The runtime-built ARGB sprite for panel 4. */
    fdk_surface *sprite = NULL;
    r = fdk_surface_create_format(120, 90, FDK_SURFACE_FORMAT_ARGB8888,
                                  &sprite);
    if (fdk_ok(r)) {
        fdk_color glass = { .r = 0.30f, .g = 0.75f, .b = 0.95f, .a = 0.45f };
        fdk_color rose = { .r = 0.95f, .g = 0.35f, .b = 0.50f, .a = 0.40f };
        fdk_surface_fill_rounded_rect(sprite,
                                      (fdk_rect){ .x = 8, .y = 12,
                                                  .width = 70, .height = 62 },
                                      16, glass);
        fdk_surface_fill_circle_aa(sprite, 82, 46, 26, rose);
        app.sprite = sprite;
    }

    fdk_window_options wopts = { .title = "FDK images, transforms & AA",
                                 .width = 760, .height = 420 };
    r = fdk_window_create(app.ctx, &wopts, &app.window);
    if (!fdk_ok(r)) {
        fprintf(stderr, "10_images: window create failed\n");
        fdk_surface_destroy(app.image);
        fdk_surface_destroy(app.sprite);
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, on_event, &app);
    fdk_window_show(app.window);

    /* RIG markers: the four panel rectangles (window coordinates). */
    printf("RIG: 8 8 368 196 image\n");
    printf("RIG: 384 8 368 196 transform\n");
    printf("RIG: 8 212 368 200 aa\n");
    printf("RIG: 384 212 368 200 alpha\n");
    fflush(stdout);

    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
        if (app.quit) {
            break;
        }

        fdk_surface *s = NULL;
        if (!fdk_ok(fdk_window_get_surface(app.window, &s))) {
            continue;
        }
        fdk_surface_info info;
        (void)fdk_surface_get_info(s, &info);
        int W = info.width, H = info.height;

        fdk_surface_fill(s, palette_bg);

        /* ---- Panel 1: image decode + alpha compositing ---- */
        fdk_rect p1 = { .x = 8, .y = 8, .width = 368, .height = 196 };
        draw_panel_frame(s, p1);
        if (app.image != NULL) {
            fdk_surface_info ii;
            (void)fdk_surface_get_info(app.image, &ii);
            fdk_i32 x = p1.x + 24, y = p1.y + 24;
            fdk_surface_blit_blend(s, x, y, app.image,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = ii.width,
                                               .height = ii.height });
            /* A second copy, scaled 1.7x through the general
             * (bilinear) path — visible enlargement beside the
             * original. Composition: scale FIRST, then translate. */
            fdk_matrix m2 = fdk_matrix_mul(
                fdk_matrix_scale(1.7f),
                fdk_matrix_translate((float)(x + ii.width + 24),
                                     (float)(y + 52)));
            fdk_surface_blit_transformed(s, m2, app.image);
            (void)W; (void)H;
        }

        /* ---- Panel 2: transforms ---- */
        fdk_rect p2 = { .x = 384, .y = 8, .width = 368, .height = 196 };
        draw_panel_frame(s, p2);
        if (app.image != NULL) {
            fdk_surface_info ii;
            (void)fdk_surface_get_info(app.image, &ii);

            /* 2x integer scale (exact nearest-neighbor block
             * pixels) at the left: scale FIRST, then translate. */
            fdk_matrix m2x =
                fdk_matrix_mul(fdk_matrix_scale(2.0f),
                               fdk_matrix_translate(396.0f, 20.0f));
            fdk_surface_blit_transformed(s, m2x, app.image);

            /* Rotated, breathing slowly — the bilinear path. The
             * origin-centered rotation swings the square; the
             * translate afterwards parks it in the panel's right
             * half (rotation is about the source origin, so the
             * visible center sits near translate + half-diagonal). */
            float ang = sinf((float)app.frame * 0.02f) * 0.45f;
            fdk_matrix rot = fdk_matrix_rotate(ang);
            fdk_matrix m = fdk_matrix_mul(
                fdk_matrix_mul(rot, fdk_matrix_scale(0.9f)),
                fdk_matrix_translate(640.0f, 96.0f));
            fdk_surface_blit_transformed(s, m, app.image);
        }

        /* ---- Panel 3: crisp vs AA ---- */
        fdk_rect p3 = { .x = 8, .y = 212, .width = 368, .height = 200 };
        draw_panel_frame(s, p3);
        {
            fdk_i32 cx = p3.x + 60, cy = p3.y + 70;
            fdk_surface_draw_line(s, cx - 40, cy + 55, cx + 30, cy - 45, ink);
            fdk_surface_draw_line_aa(s, cx + 44, cy + 55, cx + 114,
                                     cy - 45, accent);
            fdk_surface_draw_circle(s, cx, cy + 92, 22, warn);
            fdk_surface_draw_circle_aa(s, cx + 130, cy + 92, 22, accent);
            fdk_surface_fill_rounded_rect(
                s, (fdk_rect){ .x = p3.x + 190, .y = p3.y + 18,
                               .width = 80, .height = 80 }, 18, warn);
            fdk_surface_fill_rounded_rect_aa(
                s, (fdk_rect){ .x = p3.x + 280, .y = p3.y + 18,
                               .width = 80, .height = 80 }, 18, accent);
            fdk_surface_fill_circle_aa(
                s, p3.x + 230, p3.y + 150, 34,
                (fdk_color){ .r = 0.55f, .g = 0.55f, .b = 0.95f, .a = 1.0f });
        }

        /* ---- Panel 4: runtime ARGB sprite, blended twice ---- */
        fdk_rect p4 = { .x = 384, .y = 212, .width = 368, .height = 200 };
        draw_panel_frame(s, p4);
        if (app.sprite != NULL) {
            fdk_surface_info si;
            (void)fdk_surface_get_info(app.sprite, &si);
            fdk_surface_blit_blend(
                s, p4.x + 24, p4.y + 40, app.sprite,
                (fdk_rect){ .x = 0, .y = 0, .width = si.width,
                            .height = si.height });
            /* Overlapping second copy: alpha ACCUMULATES where they
             * overlap — visibly darker/more saturated. */
            fdk_surface_blit_blend(
                s, p4.x + 100, p4.y + 62, app.sprite,
                (fdk_rect){ .x = 0, .y = 0, .width = si.width,
                            .height = si.height });
        }

        (void)fdk_surface_present(s);
        app.frame++;
    }

    printf("10_images: exited cleanly after %d frames\n", app.frame);
    fdk_surface_destroy(app.sprite);
    fdk_surface_destroy(app.image);
    fdk_window_destroy(app.window);
    fdk_shutdown(app.ctx);
    return 0;
}
