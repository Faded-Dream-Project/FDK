/* 02_rendering.c — the software renderer, end to end.
 *
 * Everything on screen here is drawn by the application through
 * fdk_surface — no widget tree, no toolkit painting. Four panels:
 *
 *   PRIMITIVES  (left, tall) — a vertical gradient, two overlapping
 *               translucent ARGB sprites (alpha ACCUMULATES where
 *               they overlap), the "FDK" block logo with drop
 *               shadows, a translucent rounded border, and a ball
 *               that demonstrates DAMAGE-TRACKED rendering: after
 *               the intro it moves with two small damage rects per
 *               frame (the console prints how much of the window
 *               each present actually carried)
 *   IMAGE       — examples/data/fdk_logo.png decoded by
 *               fdk_surface_create_from_image (vendored stb_image)
 *               and composited with fdk_surface_blit_blend: the
 *               PNG's 50%-alpha band actually blends over the
 *               panel background; a second copy is scaled 1.7x
 *               through the general (bilinear) path
 *   TRANSFORM   — the same image through fdk_surface_blit_transformed:
 *               exact 2x integer scale-up (nearest-neighbor block
 *               pixels) and a fixed 30-degree rotation (bilinear)
 *   AA SHAPES   — crisp vs antialiased variants side by side: lines,
 *               circle outlines, filled circles, rounded rects
 *
 * The ball animates for a short intro and then freezes: an idle FDK
 * app costs zero presents (nothing damaged -> nothing committed).
 * Set FDK_DEMO_ANIMATE=1 to keep it moving forever, or
 * FDK_DEMO_FRAMES=N to exit after N frames (the automation knobs
 * the test rigs use).
 *
 * Works identically on X11 and Wayland — no backend type appears
 * anywhere in this file. Close the window or press ESC to exit.
 *
 * INIT-tier helper user (see example_window.h): the demo owns its
 * window and loop because its subject IS the raw surface — a helper
 * content box would fight the direct framebuffer writes. The helper
 * still provides the uniform app_id (org.fdk.example02).
 */

#include "example_window.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Intro animation: ~4 s at 60 fps, then the scene freezes. */
#define ANIM_INTRO_FRAMES 240
#define FRAME_PUMP_MS 15

/* The PRIMITIVES panel (window coordinates). */
#define P0_X 8
#define P0_Y 8
#define P0_W 368
#define P0_H 648

/* ---- automation knobs (uniform across the example suite) ---- */

static int demo_frame_limit(void) {
    const char *s = getenv("FDK_DEMO_FRAMES");
    return (s != NULL) ? atoi(s) : 0; /* 0 = stay open until closed */
}

static bool demo_animate_forever(void) {
    const char *s = getenv("FDK_DEMO_ANIMATE");
    return s != NULL && s[0] != '\0' && strcmp(s, "0") != 0;
}

/* ---- 5x7 block font (bits = lit pixels), just the three letters ---- */
static const char *FONT_F[7] = {
    "#####", "#....", "#....", "####.", "#....", "#....", "#....",
};
static const char *FONT_D[7] = {
    "####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####.",
};
static const char *FONT_K[7] = {
    "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#",
};

typedef struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_surface *surface;
    fdk_surface *image;   /* decoded PNG (ARGB) */
    fdk_surface *sprite;  /* runtime-built ARGB sprite */
    int frame;
    int need_full_redraw;
    bool quit;
    /* Ball state (the damage demo). */
    float ball_x, ball_y, ball_vx, ball_vy, ball_r;
    /* Frozen gradient colors (row 0 / row h-1) for ball restores. */
    float grad_top[3], grad_bot[3];
} app_state;

static fdk_color panel_bg = { .r = 0.13f, .g = 0.14f, .b = 0.17f, .a = 1.0f };
static fdk_color ink = { .r = 0.92f, .g = 0.93f, .b = 0.96f, .a = 1.0f };
static fdk_color accent = { .r = 0.20f, .g = 0.78f, .b = 0.62f, .a = 1.0f };
static fdk_color warn = { .r = 0.95f, .g = 0.45f, .b = 0.25f, .a = 1.0f };

/* The left panel's gradient palette — fixed, so the frozen scene is
 * deterministic (the rig screenshots it after the intro). */
static void palette(float t, float out[3]) {
    out[0] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.0f));
    out[1] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.33f));
    out[2] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.66f));
}

static void on_event(fdk_window *window, const fdk_event_data *event,
                     void *user_data) {
    app_state *app = user_data;
    (void)window;

    switch (event->type) {
        case FDK_EVENT_WINDOW_CLOSE_REQUEST:
            app->quit = true;
            break;
        case FDK_EVENT_WINDOW_CONFIGURE:
        case FDK_EVENT_WINDOW_EXPOSE:
            /* New size = new framebuffer = damage resets to full; the
             * whole scene is repainted next frame. */
            app->need_full_redraw = 1;
            break;
        case FDK_EVENT_KEY_DOWN:
            if (event->key.codepoint == 27 || event->key.scancode == 1) {
                app->quit = true;
            }
            break;
        default:
            break;
    }
}

/* ---- raw-pixel drawing (the info.pixels level of the API) ---- */

/* Antialiased filled ball, blended source-over with per-pixel
 * coverage. Writes ONLY within [x0,x1)x[y0,y1) of `span`. */
static void draw_ball_span(fdk_surface_info *info, float cx, float cy,
                           float r, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > info->width) x1 = info->width;
    if (y1 > info->height) y1 = info->height;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d = dx * dx + dy * dy; /* squared distance */
            float inner = (r - 1.0f) * (r - 1.0f);
            float outer = (r + 1.0f) * (r + 1.0f);
            float cov;
            if (d <= inner) {
                cov = 1.0f;
            } else if (d >= outer) {
                continue;
            } else {
                cov = (outer - d) / (outer - inner);
            }
            /* White core with a violet rim for depth. */
            fdk_u32 src = (d >= r * r * 0.5625f) ? 0x00B48CFFu
                                                  : 0x00FFFFFFu;
            fdk_u32 *px =
                info->pixels + (size_t)y * (size_t)info->stride + (size_t)x;
            fdk_u32 dst = *px;
            unsigned sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu,
                     sb = src & 0xFFu;
            unsigned dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu,
                     db = dst & 0xFFu;
            dr = (unsigned)((float)sr * cov + (float)dr * (1.0f - cov));
            dg = (unsigned)((float)sg * cov + (float)dg * (1.0f - cov));
            db = (unsigned)((float)sb * cov + (float)db * (1.0f - cov));
            *px = (dr << 16) | (dg << 8) | db;
        }
    }
}

/* Restores the frozen vertical gradient inside a rect via raw writes
 * — and declares the damage explicitly, which is exactly what the
 * raw-access contract requires (helpers do this automatically). */
static void restore_gradient_span(fdk_surface *surface,
                                  fdk_surface_info *info,
                                  const app_state *app, int x0, int y0,
                                  int x1, int y1) {
    if (x0 < P0_X) x0 = P0_X;
    if (y0 < P0_Y) y0 = P0_Y;
    if (x1 > P0_X + P0_W) x1 = P0_X + P0_W;
    if (y1 > P0_Y + P0_H) y1 = P0_Y + P0_H;

    for (int y = y0; y < y1; y++) {
        float t = P0_H > 1
            ? (float)(y - P0_Y) / (float)(P0_H - 1)
            : 0.0f;
        unsigned r = (unsigned)(255.0f *
                     (app->grad_top[0] + (app->grad_bot[0] - app->grad_top[0]) * t));
        unsigned g = (unsigned)(255.0f *
                     (app->grad_top[1] + (app->grad_bot[1] - app->grad_top[1]) * t));
        unsigned b = (unsigned)(255.0f *
                     (app->grad_top[2] + (app->grad_bot[2] - app->grad_top[2]) * t));
        fdk_u32 px = ((fdk_u32)r << 16) | ((fdk_u32)g << 8) | (fdk_u32)b;
        fdk_u32 *row = info->pixels + (size_t)y * (size_t)info->stride;
        for (int x = x0; x < x1; x++) {
            row[x] = px;
        }
    }
    fdk_surface_invalidate(surface, (fdk_rect){ .x = x0, .y = y0,
                                                .width = x1 - x0,
                                                .height = y1 - y0 });
}

/* Helper-level drawing: "FDK" block letters with a drop shadow. */
static void draw_logo(fdk_surface *surface, int origin_x, int origin_y,
                      int scale) {
    const int glyph_w = 5 * scale;
    const int gap = 2 * scale;
    const char **letters[3] = { FONT_F, FONT_D, FONT_K };
    for (int li = 0; li < 3; li++) {
        const char **glyph = letters[li];
        int gx = origin_x + li * (glyph_w + gap);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (glyph[row][col] == '#') {
                    fdk_rect cell = {
                        .x = gx + col * scale,
                        .y = origin_y + row * scale,
                        .width = scale,
                        .height = scale,
                    };
                    /* Shadow first (offset), then the white face. */
                    fdk_rect shadow = cell;
                    shadow.x += 3;
                    shadow.y += 3;
                    fdk_surface_fill_rect(surface, shadow,
                                          (fdk_color){ .r = 0, .g = 0, .b = 0,
                                                       .a = 0.45f });
                    fdk_surface_fill_rect(surface, cell,
                                          (fdk_color){ .r = 1, .g = 1, .b = 1,
                                                       .a = 1 });
                }
            }
        }
    }
}

static void draw_panel_frame(fdk_surface *s, fdk_rect r) {
    fdk_surface_fill_rect(s, r, panel_bg);
    fdk_surface_draw_rect(s, r, (fdk_color){ .r = 0.25f, .g = 0.27f,
                                             .b = 0.32f, .a = 1.0f });
}

/* The full scene. Called on the first frame, after every resize, and
 * when the intro animation ends (to settle deterministically). */
static void render_full_frame(app_state *app) {
    fdk_surface *s = app->surface;
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }

    fdk_surface_fill(s, (fdk_color){ .r = 0.07f, .g = 0.08f, .b = 0.10f,
                                     .a = 1.0f });

    /* ---- left panel: primitives, sprites, logo, damage-demo ball ---- */
    {
        const fdk_rect p0 = { .x = P0_X, .y = P0_Y, .width = P0_W,
                              .height = P0_H };
        float top[3], bot[3];
        palette(0.13f, top);
        palette(0.63f, bot);
        for (int i = 0; i < 3; i++) {
            app->grad_top[i] = top[i];
            app->grad_bot[i] = bot[i];
        }
        fdk_surface_fill_gradient_vertical(
            s, p0,
            (fdk_color){ top[0], top[1], top[2], 1 },
            (fdk_color){ bot[0], bot[1], bot[2], 1 });

        /* Translucent sprites over the gradient: alpha accumulates
         * where the two copies overlap. */
        if (app->sprite != NULL) {
            fdk_surface_info si;
            (void)fdk_surface_get_info(app->sprite, &si);
            fdk_surface_blit_blend(
                s, P0_X + 20, P0_Y + 380, app->sprite,
                (fdk_rect){ .x = 0, .y = 0, .width = si.width,
                            .height = si.height });
            fdk_surface_blit_blend(
                s, P0_X + 96, P0_Y + 402, app->sprite,
                (fdk_rect){ .x = 0, .y = 0, .width = si.width,
                            .height = si.height });
        }

        /* The logo anchors the bottom of the panel. */
        draw_logo(s, P0_X + 34, P0_Y + P0_H - 7 * 7 - 40, 7);

        fdk_surface_draw_rounded_rect(
            s, (fdk_rect){ .x = P0_X + 4, .y = P0_Y + 4,
                           .width = P0_W - 8, .height = P0_H - 8 },
            10, (fdk_color){ .r = 1, .g = 1, .b = 1, .a = 0.35f });

        /* Ball on top, raw pixels + explicit damage. */
        draw_ball_span(&info, app->ball_x, app->ball_y, app->ball_r,
                       (int)(app->ball_x - app->ball_r) - 2,
                       (int)(app->ball_y - app->ball_r) - 2,
                       (int)(app->ball_x + app->ball_r) + 2,
                       (int)(app->ball_y + app->ball_r) + 2);
        fdk_surface_invalidate(
            s, (fdk_rect){ .x = (int)(app->ball_x - app->ball_r) - 2,
                           .y = (int)(app->ball_y - app->ball_r) - 2,
                           .width = (int)(app->ball_r * 2) + 4,
                           .height = (int)(app->ball_r * 2) + 4 });
    }

    /* ---- right column, top: image decode + alpha compositing ---- */
    const fdk_rect p1 = { .x = 384, .y = 8, .width = 368, .height = 216 };
    draw_panel_frame(s, p1);
    if (app->image != NULL) {
        fdk_surface_info ii;
        (void)fdk_surface_get_info(app->image, &ii);
        fdk_i32 x = p1.x + 24, y = p1.y + 24;
        fdk_surface_blit_blend(s, x, y, app->image,
                               (fdk_rect){ .x = 0, .y = 0,
                                           .width = ii.width,
                                           .height = ii.height });
        /* A second copy, scaled 1.7x through the general (bilinear)
         * path — visible enlargement beside the original. Composition:
         * scale FIRST, then translate. */
        fdk_matrix m2 = fdk_matrix_mul(
            fdk_matrix_scale(1.7f),
            fdk_matrix_translate((float)(x + ii.width + 24),
                                 (float)(y + 52)));
        fdk_surface_blit_transformed(s, m2, app->image);
    }

    /* ---- right column, middle: transforms ---- */
    const fdk_rect p2 = { .x = 384, .y = 232, .width = 368, .height = 216 };
    draw_panel_frame(s, p2);
    if (app->image != NULL) {
        /* 2x integer scale (exact nearest-neighbor block pixels) at
         * the left: scale FIRST, then translate. */
        fdk_matrix m2x =
            fdk_matrix_mul(fdk_matrix_scale(2.0f),
                           fdk_matrix_translate(396.0f, 244.0f));
        fdk_surface_blit_transformed(s, m2x, app->image);

        /* Fixed 30-degree rotation — the bilinear path. Rotation is
         * about the source origin; the translate afterwards parks it
         * in the panel's right half. */
        fdk_matrix rot = fdk_matrix_rotate(0.5236f);
        fdk_matrix m = fdk_matrix_mul(
            fdk_matrix_mul(rot, fdk_matrix_scale(0.9f)),
            fdk_matrix_translate(630.0f, 330.0f));
        fdk_surface_blit_transformed(s, m, app->image);
    }

    /* ---- right column, bottom: crisp vs AA ---- */
    const fdk_rect p3 = { .x = 384, .y = 456, .width = 368, .height = 200 };
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
}

/* Partial frame: restore the frozen gradient under the ball's old
 * span, then draw it at the new spot. Two small damage rects — that
 * is the whole frame. The ball stays in the panel's upper region,
 * above the sprites and the logo, so its restore span never repaints
 * them. */
static void render_partial_frame(app_state *app) {
    fdk_surface *surface = app->surface;
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        return;
    }

    float r = app->ball_r;
    int pad = 2;

    /* Old span: restore the frozen gradient there. */
    restore_gradient_span(surface, &info, app,
                          (int)(app->ball_x - r) - pad,
                          (int)(app->ball_y - r) - pad,
                          (int)(app->ball_x + r) + pad,
                          (int)(app->ball_y + r) + pad);

    /* Move. */
    app->ball_x += app->ball_vx;
    app->ball_y += app->ball_vy;

    float min_x = P0_X + r + 12.0f;
    float max_x = P0_X + P0_W - r - 12.0f;
    float min_y = P0_Y + r + 12.0f;
    float max_y = P0_Y + 340.0f - r; /* above the sprite band (y 380) */
    if (max_y < min_y + 10.0f) {
        max_y = min_y + 10.0f; /* very short window: degrade to top band */
    }
    if (app->ball_x < min_x) { app->ball_x = min_x; app->ball_vx = -app->ball_vx; }
    if (app->ball_x > max_x) { app->ball_x = max_x; app->ball_vx = -app->ball_vx; }
    if (app->ball_y < min_y) { app->ball_y = min_y; app->ball_vy = -app->ball_vy; }
    if (app->ball_y > max_y) { app->ball_y = max_y; app->ball_vy = -app->ball_vy; }

    /* New span: draw the ball + declare its damage. */
    draw_ball_span(&info, app->ball_x, app->ball_y, r,
                   (int)(app->ball_x - r) - pad, (int)(app->ball_y - r) - pad,
                   (int)(app->ball_x + r) + pad, (int)(app->ball_y + r) + pad);
    fdk_surface_invalidate(surface,
                           (fdk_rect){ .x = (int)(app->ball_x - r) - pad,
                                       .y = (int)(app->ball_y - r) - pad,
                                       .width = (int)(r * 2) + 2 * pad,
                                       .height = (int)(r * 2) + 2 * pad });
}

int main(int argc, char **argv) {
    printf("Faded Dream ToolKit %s — software rendering demo "
           "(damage-tracked, frame-paced)\n",
           fdk_get_version_string());

    const char *image_path = "examples/data/fdk_logo.png";
    if (argc > 1) {
        image_path = argv[1];
    }

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "02")) {
        return 1;
    }
    fdk_result r = FDK_OK;

    app_state app = { 0 };
    app.ctx = ctx;
    app.ball_x = 160.0f;
    app.ball_y = 200.0f;
    app.ball_vx = 3.1f;
    app.ball_vy = 2.3f;
    app.ball_r = 36.0f;
    app.need_full_redraw = 1;

    r = fdk_surface_create_from_image(image_path, &app.image);
    if (!fdk_ok(r)) {
        fprintf(stderr, "02_rendering: cannot decode %s (%s)\n", image_path,
                fdk_result_to_string(r));
        fprintf(stderr, "(run it from the repository root)\n");
        fdk_shutdown(ctx);
        return 1;
    }

    /* The runtime-built ARGB sprite for the PRIMITIVES panel. */
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

    fdk_window_options opts = {
        .title = "FDK rendering — primitives, images, transforms, AA",
        .width = 760,
        .height = 664,
    };
    r = fdk_window_create(ctx, &opts, &app.window);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_create failed: %s\n",
                fdk_result_to_string(r));
        fdk_surface_destroy(app.image);
        fdk_surface_destroy(app.sprite);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, on_event, &app);
    r = fdk_window_get_surface(app.window, &app.surface);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_get_surface failed: %s\n",
                fdk_result_to_string(r));
        fdk_window_destroy(app.window);
        fdk_surface_destroy(app.image);
        fdk_surface_destroy(app.sprite);
        fdk_shutdown(ctx);
        return 1;
    }

    /* RIG markers: the panel rectangles (window coordinates). */
    printf("RIG: %d %d %d %d primitives\n", P0_X, P0_Y, P0_W, P0_H);
    printf("RIG: 384 8 368 216 image\n");
    printf("RIG: 384 232 368 216 transform\n");
    printf("RIG: 384 456 368 200 aa\n");
    fflush(stdout);

    fdk_window_show(app.window);

    const bool animate = demo_animate_forever();
    const int frame_limit = demo_frame_limit();
    int full_frames = 0, partial_frames = 0;

    while (!app.quit) {
        (void)fdk_pump_events(ctx, FRAME_PUMP_MS);
        if (app.quit) {
            break;
        }

        /* Compositor-paced (Wayland): skip rendering until the last
         * frame has been acknowledged (or the guard interval fires).
         * On X11 this is always true. */
        if (!fdk_surface_frame_ready(app.surface)) {
            continue;
        }

        if (app.need_full_redraw) {
            render_full_frame(&app);
            app.need_full_redraw = 0;
            full_frames++;
        } else if (animate || app.frame < ANIM_INTRO_FRAMES) {
            render_partial_frame(&app);
            partial_frames++;
        } else if (app.frame == ANIM_INTRO_FRAMES) {
            /* The intro just ended: one final full frame settles the
             * scene deterministically (the rig screenshots now). */
            render_full_frame(&app);
            printf("HOLD\n");
            fflush(stdout);
        }

        /* Report the damage this present will carry. */
        fdk_rect dmg;
        if (fdk_surface_get_damage_bounds(app.surface, &dmg)) {
            fdk_surface_info info;
            long long area = (long long)dmg.width * dmg.height;
            long long total = 1;
            if (fdk_ok(fdk_surface_get_info(app.surface, &info))) {
                total = (long long)info.width * info.height;
            }
            int percent = (int)((area * 100) / (total > 0 ? total : 1));
            if (app.frame % 60 == 0) {
                printf("frame %3d damage %dx%d = %d%% of window\n",
                       app.frame, dmg.width, dmg.height, percent);
            }
        }

        if (!fdk_ok(fdk_surface_present(app.surface))) {
            fprintf(stderr, "present failed — stopping\n");
            break;
        }
        app.frame++;

        if (frame_limit > 0 && app.frame >= frame_limit) {
            break;
        }
    }

    printf("02_rendering: exited cleanly after %d frames "
           "(%d full-surface, %d partial)\n",
           app.frame, full_frames, partial_frames);
    fdk_surface_destroy(app.sprite);
    fdk_surface_destroy(app.image);
    fdk_window_destroy(app.window);
    fdk_shutdown(ctx);
    return 0;
}
