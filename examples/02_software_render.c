/*
 * 02_software_render.c — FDK software rendering: a real animated
 * frame in the window, drawn pixel by pixel through fdk_surface.
 *
 * This is the Phase 3 "first slice" example (see docs/roadmap.md):
 * it owns its event loop with fdk_pump_events() (the pattern
 * fdk_core.h documents for rendered apps — fdk_run() would block in
 * poll() between input events with no place to draw), then each
 * frame:
 *
 *   1. reacquires the framebuffer (fdk_surface_get_info — handles
 *      resizes transparently),
 *   2. draws an animated vertical gradient with the fdk_surface
 *      gradient helper,
 *   3. draws a bouncing antialiased ball by writing pixels DIRECTLY
 *      through info.pixels (the raw-access level of the API),
 *   4. draws "FDK" block letters with fdk_surface_fill_rect and a
 *      soft border with fdk_surface_draw_rect,
 *   5. presents the frame (fdk_surface_present).
 *
 * Exits on window close, ESC, or after ~10 seconds of frames (the
 * self-timeout exists so automated demos terminate cleanly; on a
 * desktop just close the window or press ESC sooner).
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

/* Self-terminate after this many frames (~10 s at 60 fps) so
 * automated/headless demos are bounded. */
#define MAX_FRAMES 600
#define FRAME_PUMP_MS 15 /* ~60 fps cadence */

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
    /* Ball state. */
    float ball_x, ball_y;
    float ball_vx, ball_vy;
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
            printf("resized to %dx%d — framebuffer follows next frame\n",
                   event->configure.size.width, event->configure.size.height);
            break;

        case FDK_EVENT_WINDOW_EXPOSE:
            /* On X11 the next loop iteration re-renders and re-presents,
             * which is the documented response; nothing extra needed
             * here beyond noting it. */
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
static fdk_color palette(float t, float a) {
    /* Three overlapping sine phases keep every frame colorful without
     * any of the channels ever saturating to flat white. */
    fdk_color c = {
        .r = 0.5f + 0.5f * sinf(6.28318f * (t + 0.0f)),
        .g = 0.5f + 0.5f * sinf(6.28318f * (t + 0.33f)),
        .b = 0.5f + 0.5f * sinf(6.28318f * (t + 0.66f)),
        .a = a,
    };
    return c;
}

/* Direct-pixel drawing: antialiased filled circle, the "raw access"
 * half of the surface API. */
static void draw_ball(fdk_surface_info *info, float cx, float cy, float r) {
    int x0 = (int)(cx - r - 1.0f);
    int x1 = (int)(cx + r + 1.0f);
    int y0 = (int)(cy - r - 1.0f);
    int y1 = (int)(cy + r + 1.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= info->width) x1 = info->width - 1;
    if (y1 >= info->height) y1 = info->height - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float d = dx * dx + dy * dy; /* squared distance */
            float rr = r * r;
            /* Coverage ramp across one pixel: fully inside at
             * d <= (r-1)^2, fully outside at d >= (r+1)^2 — a cheap
             * boxy but effective antialiasing edge. */
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
            /* White core with a subtle violet rim for depth. */
            fdk_u32 rim = 0x00B48CFFu; /* soft violet */
            fdk_u32 core = 0x00FFFFFFu; /* white */
            float rim_w = 0.25f;        /* rim occupies outer quarter */
            float t = (d >= rr * (1.0f - rim_w * 2.0f)) ? 1.0f : 0.0f;
            fdk_u32 src = t > 0.0f ? rim : core;

            fdk_u32 *px =
                info->pixels + (size_t)y * (size_t)info->stride + (size_t)x;
            fdk_u32 dst = *px;
            /* Blend src over dst with coverage `cov`. */
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

static void render_frame(fdk_surface *surface, app_state *app) {
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info))) {
        return;
    }

    float t = (float)app->frame / 240.0f; /* full palette cycle: 4 s */

    /* 1. Animated vertical gradient. */
    fdk_color top = palette(t, 1.0f);
    fdk_color bottom = palette(t + 0.5f, 1.0f);
    fdk_surface_fill_gradient_vertical(surface,
                                        (fdk_rect){ .x = 0, .y = 0,
                                                    .width = info.width,
                                                    .height = info.height },
                                        top, bottom);

    /* 2. Bouncing ball (raw pixels). */
    float r = 36.0f;
    app->ball_x += app->ball_vx;
    app->ball_y += app->ball_vy;
    if (app->ball_x < r) { app->ball_x = r; app->ball_vx = -app->ball_vx; }
    if (app->ball_x > (float)info.width - r) {
        app->ball_x = (float)info.width - r;
        app->ball_vx = -app->ball_vx;
    }
    if (app->ball_y < r) { app->ball_y = r; app->ball_vy = -app->ball_vy; }
    if (app->ball_y > (float)info.height - r) {
        app->ball_y = (float)info.height - r;
        app->ball_vy = -app->ball_vy;
    }
    draw_ball(&info, app->ball_x, app->ball_y, r);

    /* 3. Logo + soft border (helper primitives). */
    draw_logo(surface, info.width, info.height);
    fdk_surface_draw_rect(surface,
                          (fdk_rect){ .x = 4, .y = 4,
                                      .width = info.width - 8,
                                      .height = info.height - 8 },
                          (fdk_color){ .r = 1, .g = 1, .b = 1, .a = 0.35f });
}

int main(void) {
    printf("Faded Dream ToolKit %s — software rendering demo\n",
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
        .ball_x = 160.0f,
        .ball_y = 120.0f,
        .ball_vx = 3.1f,
        .ball_vy = 2.3f,
    };
    fdk_window_set_event_callback(window, on_event, &app);
    fdk_window_show(window);

    printf("rendering %d frames (~%.0f s) — close the window or press "
           "ESC to stop early\n",
           MAX_FRAMES, MAX_FRAMES * FRAME_PUMP_MS / 1000.0f);

    while (!app.done && app.frame < MAX_FRAMES) {
        fdk_pump_events(ctx, FRAME_PUMP_MS);
        if (app.done) {
            break; /* stop requested from a callback — don't draw again */
        }
        render_frame(surface, &app);
        if (!fdk_ok(fdk_surface_present(surface))) {
            fprintf(stderr, "present failed — stopping\n");
            break;
        }
        app.frame++;
        if (app.frame % 120 == 0) {
            printf("frame %d / %d\n", app.frame, MAX_FRAMES);
        }
    }

    printf("rendered %d frames, shutting down\n", app.frame);
    fdk_window_destroy(window);
    fdk_shutdown(ctx);
    return 0;
}
