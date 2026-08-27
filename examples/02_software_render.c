/*
 * 02_software_render.c — FDK software rendering: a real animated
 * frame in the window, drawn pixel by pixel through fdk_surface —
 * now DAMAGE-TRACKED and FRAME-PACED.
 *
 * The demo runs in two phases to exercise both rendering modes:
 *
 *   Phase 1 (~2 s): the full frame is animated (hue-cycling
 *   gradient), so every present is a full-surface damage update —
 *   the "everything changed" path.
 *
 *   Phase 2 (rest): the background freezes and only a ball moves.
 *   Each frame redraws just the ball's old rectangle (restoring the
 *   frozen gradient through raw pixel writes + explicit
 *   fdk_surface_invalidate) and the ball's new position — two small
 *   damage rects per frame instead of a whole-window redraw. The
 *   console prints the live damage bounds and what fraction of the
 *   window they cover, so the partial-present savings are visible.
 *
 * The loop also consults fdk_surface_frame_ready(): on Wayland the
 * compositor's frame callbacks pace the loop (never rendering ahead
 * of presentation, never starving); on X11 it is always true. Exits
 * on window close, ESC, or after ~10 s of frames.
 *
 * API levels demonstrated:
 *   - helpers: fill_gradient_vertical, fill_rect, draw_rect,
 *     fill_circle, draw_rounded_rect (background + logo)
 *   - raw access: info.pixels ball + gradient restore, each followed
 *     by fdk_surface_invalidate (the documented contract for raw
 *     writes)
 *   - damage query: fdk_surface_get_damage_bounds before present
 *
 * Works identically on X11 and Wayland — no backend type appears
 * anywhere in this file.
 *
 * Build: make examples
 * Run:   ./build/examples/02_software_render
 */

#include "fdk/fdk.h"

#include <math.h>
#include <stdio.h>

/* Self-terminate after this many frames so automated/headless demos
 * are bounded (~10 s at 60 fps). */
#define MAX_FRAMES 600
#define FRAME_PUMP_MS 15 /* ~60 fps cadence when not compositor-paced */
#define FULL_DANCE_FRAMES 120 /* phase 1 length (~2 s) */

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
    int done;
    int frame;
    int need_full_redraw; /* set at start and on every resize */
    /* Ball state. */
    float ball_x, ball_y;
    float ball_vx, ball_vy;
    float ball_r;
    /* Frozen phase-2 background colors (row 0 / row h-1). */
    float grad_top[3], grad_bot[3];
    /* Stats. */
    int full_frames, partial_frames;
} app_state;

static void on_event(fdk_window *window, const fdk_event_data *event,
                     void *user_data) {
    app_state *app = user_data;
    (void)window; /* close is deferred to after the loop — see below */

    switch (event->type) {
        case FDK_EVENT_WINDOW_CLOSE_REQUEST:
            printf("close requested, shutting down\n");
            /* Destroying here would be legal (01_hello_world does it),
             * but this app must not touch `surface` after the window
             * dies — the surface is owned by the window (fdk_surface.h).
             * So: stop the loop now, destroy after it. */
            app->done = 1;
            break;

        case FDK_EVENT_WINDOW_CONFIGURE:
            /* New size = new framebuffer = damage resets to full;
             * the app must repaint the whole scene next frame. */
            app->need_full_redraw = 1;
            printf("resized to %dx%d — full repaint next frame\n",
                   event->configure.size.width,
                   event->configure.size.height);
            break;

        case FDK_EVENT_WINDOW_EXPOSE:
            /* The next loop iteration re-renders and re-presents,
             * which is the documented response. */
            app->need_full_redraw = 1;
            break;

        case FDK_EVENT_KEY_DOWN:
            /* ESC via codepoint (27) or scancode (evdev 1) — see
             * fdk_event.h for the two identity systems. */
            if (event->key.codepoint == 27 || event->key.scancode == 1) {
                printf("ESC pressed, shutting down\n");
                app->done = 1;
            }
            break;
        default:
            break;
    }
}

/* Hue-shift helper: a cheap smooth palette sweep over t in [0,1). */
static void palette(float t, float out[3]) {
    /* Three overlapping sine phases keep every frame colorful without
     * any of the channels ever saturating to flat white. */
    out[0] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.0f));
    out[1] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.33f));
    out[2] = 0.5f + 0.5f * sinf(6.28318f * (t + 0.66f));
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
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > info->width) x1 = info->width;
    if (y1 > info->height) y1 = info->height;

    for (int y = y0; y < y1; y++) {
        float t = info->height > 1
            ? (float)y / (float)(info->height - 1)
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
static void draw_logo(fdk_surface *surface, int win_w, int win_h) {
    const int scale = 8;
    const int glyph_w = 5 * scale;
    const int glyph_h = 7 * scale;
    const int gap = 2 * scale;
    const int total_w = 3 * glyph_w + 2 * gap;
    int origin_x = (win_w - total_w) / 2;
    int origin_y = win_h - glyph_h - 40;
    if (origin_x < 0) origin_x = 0;
    if (origin_y < 0) origin_y = 0;

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

/* Full-scene frame (phase 1 and post-resize): animated gradient,
 * logo, border, and the ball on top. */
static void render_full_frame(fdk_surface *surface, app_state *app) {
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        return;
    }

    float t = (float)app->frame / 240.0f; /* full palette cycle: 4 s */
    float top[3], bot[3];
    palette(t, top);
    palette(t + 0.5f, bot);

    /* Remember the phase-2 frozen colors (the gradient the ball
     * restores through when it moves). */
    for (int i = 0; i < 3; i++) {
        app->grad_top[i] = top[i];
        app->grad_bot[i] = bot[i];
    }

    fdk_surface_fill_gradient_vertical(surface,
                                       (fdk_rect){ .x = 0, .y = 0,
                                                   .width = info.width,
                                                   .height = info.height },
                                       (fdk_color){ top[0], top[1], top[2], 1 },
                                       (fdk_color){ bot[0], bot[1], bot[2], 1 });

    draw_logo(surface, info.width, info.height);
    fdk_surface_draw_rounded_rect(surface,
                                  (fdk_rect){ .x = 4, .y = 4,
                                              .width = info.width - 8,
                                              .height = info.height - 8 },
                                  10,
                                  (fdk_color){ .r = 1, .g = 1, .b = 1, .a = 0.35f });

    /* Ball on top, raw pixels + explicit damage. */
    draw_ball_span(&info, app->ball_x, app->ball_y, app->ball_r,
                   (int)(app->ball_x - app->ball_r - 2),
                   (int)(app->ball_y - app->ball_r - 2),
                   (int)(app->ball_x + app->ball_r + 2),
                   (int)(app->ball_y + app->ball_r + 2));
    fdk_surface_invalidate(surface,
                           (fdk_rect){ .x = (int)(app->ball_x - app->ball_r) - 2,
                                       .y = (int)(app->ball_y - app->ball_r) - 2,
                                       .width = (int)(app->ball_r * 2) + 4,
                                       .height = (int)(app->ball_r * 2) + 4 });
}

/* Partial frame (phase 2): restore the frozen gradient under the
 * ball's old span, then draw it at the new spot. Two small damage
 * rects — that is the whole frame. */
static void render_partial_frame(fdk_surface *surface, app_state *app) {
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        return;
    }

    float r = app->ball_r;
    int pad = 2;

    /* Old span: restore the frozen gradient there. */
    int ox0 = (int)(app->ball_x - r) - pad;
    int oy0 = (int)(app->ball_y - r) - pad;
    restore_gradient_span(surface, &info, app, ox0, oy0,
                          (int)(app->ball_x + r) + pad,
                          (int)(app->ball_y + r) + pad);

    /* Move. */
    app->ball_x += app->ball_vx;
    app->ball_y += app->ball_vy;

    /* Keep the ball above the logo band and inside the border, so
     * its restore span never has to repaint logo pixels. */
    float logo_top = (float)(info.height - 7 * 8 - 40) - 6.0f;
    float min_x = r + 12.0f, max_x = (float)info.width - r - 12.0f;
    float min_y = r + 12.0f;
    float max_y = logo_top - r;
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

int main(void) {
    printf("Faded Dream ToolKit %s — software rendering demo "
           "(damage-tracked, frame-paced)\n",
           fdk_get_version_string());

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_init failed: %s\n", fdk_result_to_string(r));
        fprintf(stderr, "(this example needs a real X11 or Wayland display)\n");
        return 1;
    }

    fdk_window *window = NULL;
    fdk_window_options opts = {
        .title = "FDK Software Rendering",
        .width = 640,
        .height = 480,
    };
    r = fdk_window_create(ctx, &opts, &window);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_create failed: %s\n", fdk_result_to_string(r));
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_surface *surface = NULL;
    r = fdk_window_get_surface(window, &surface);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_get_surface failed: %s\n",
                fdk_result_to_string(r));
        fdk_window_destroy(window);
        fdk_shutdown(ctx);
        return 1;
    }

    app_state app = {
        .ctx = ctx,
        .done = 0,
        .frame = 0,
        .need_full_redraw = 1,
        .ball_x = 160.0f,
        .ball_y = 120.0f,
        .ball_vx = 3.1f,
        .ball_vy = 2.3f,
        .ball_r = 36.0f,
        .grad_top = { 0, 0, 0 },
        .grad_bot = { 0, 0, 0 },
        .full_frames = 0,
        .partial_frames = 0,
    };
    fdk_window_set_event_callback(window, on_event, &app);
    fdk_window_show(window);

    printf("phase 1: animated full-frame gradient (~%d frames), then "
           "partial redraws\n", FULL_DANCE_FRAMES);
    printf("rendering %d frames (~%.0f s) — close the window or press "
           "ESC to stop early\n",
           MAX_FRAMES, MAX_FRAMES * FRAME_PUMP_MS / 1000.0f);

    while (!app.done && app.frame < MAX_FRAMES) {
        fdk_pump_events(ctx, FRAME_PUMP_MS);
        if (app.done) {
            break; /* stop requested from a callback — don't draw again */
        }

        /* Compositor-paced (Wayland): skip rendering until the last
         * frame has been acknowledged (or the guard interval fires).
         * On X11 this is always true. */
        if (!fdk_surface_frame_ready(surface)) {
            continue;
        }

        if (app.need_full_redraw) {
            render_full_frame(surface, &app);
            app.need_full_redraw = 0;
        } else if (app.frame < FULL_DANCE_FRAMES) {
            render_full_frame(surface, &app);
        } else {
            render_partial_frame(surface, &app);
        }

        /* Report the damage this present will carry. */
        fdk_rect dmg;
        if (fdk_surface_get_damage_bounds(surface, &dmg)) {
            fdk_surface_info info;
            long long area = (long long)dmg.width * dmg.height;
            long long total = 1;
            if (fdk_ok(fdk_surface_get_info(surface, &info))) {
                total = (long long)info.width * info.height;
            }
            int percent = (int)((area * 100) / (total > 0 ? total : 1));
            if (app.frame < FULL_DANCE_FRAMES) {
                app.full_frames++;
                if (app.frame % 30 == 0) {
                    printf("frame %3d [full ] damage %dx%d = %d%% of "
                           "window\n", app.frame, dmg.width, dmg.height,
                           percent);
                }
            } else {
                app.partial_frames++;
                if (app.frame % 60 == 0) {
                    printf("frame %3d [part ] damage %dx%d at (%d,%d) = "
                           "%d%% of window\n", app.frame, dmg.width,
                           dmg.height, dmg.x, dmg.y, percent);
                }
            }
        }

        if (!fdk_ok(fdk_surface_present(surface))) {
            fprintf(stderr, "present failed — stopping\n");
            break;
        }
        app.frame++;
    }

    printf("rendered %d frames (%d full-surface, %d partial), shutting "
           "down\n", app.frame, app.full_frames, app.partial_frames);
    fdk_window_destroy(window);
    fdk_shutdown(ctx);
    return 0;
}
