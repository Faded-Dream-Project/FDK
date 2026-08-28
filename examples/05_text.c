/* 05_text.c — the Phase 6 text foundation, live.
 *
 * Everything on screen here is shaped, kerned, rasterized, cached,
 * and alpha-blended text — drawn straight onto the window surface:
 *   - a 96px "FDK" wordmark
 *   - a size ladder (12 / 16 / 24 px of the same sentence)
 *   - colored runs placed by MEASURED widths (no hand-tuned offsets:
 *     each run starts where the previous one's advance ended)
 *   - an animated sine wave where every glyph rides its own baseline
 *   - a live glyph-cache stats footer
 *
 * After the animated phase the demo freezes into a deterministic
 * HOLD state (rig-friendly: screenshots race animated frames).
 * Escape or the close request ends it.
 *
 * Needs a system TrueType font; discovery is fdk_font_load_system
 * _default()'s job (fontconfig, $FDK_FONT_FILE / $FDK_FONT_DIRS,
 * then a ranked font-directory scan), and the demo exits with a
 * notice if the environment has none.
 */

#include "fdk/fdk.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (event->type == FDK_EVENT_KEY_DOWN &&
         event->key.scancode == FDK_KEY_ESC)) {
        app.quit = true;
    }
}

/* Draws one run of text and returns where the pen landed (so callers
 * can chain runs on one baseline without hand-tuned offsets). */
static int draw_run(fdk_surface *s, fdk_font *f, const char *text,
                    int pen_x, int baseline_y, fdk_color color) {
    size_t len = strlen(text);
    fdk_text_metrics m;
    (void)fdk_font_measure_utf8(f, text, len, &m);
    (void)fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline_y, color);
    return pen_x + m.advance_width;
}

/* The wave line: each glyph of `text` drawn with its own baseline
 * offset — per-glyph placement driven by the same measured advances
 * the shaper uses. `t` freezes at 0 in the HOLD phase. */
static void draw_wave(fdk_surface *s, fdk_font *f, const char *text,
                      int pen_x, int mid_baseline, double t) {
    size_t len = strlen(text);
    fdk_text_metrics run_m;
    (void)fdk_font_measure_utf8(f, text, len, &run_m);
    int start_x = pen_x;
    if (run_m.advance_width < 560) {
        start_x += (560 - run_m.advance_width) / 2;
    }

    int pen = start_x;
    for (size_t i = 0; i < len; i++) {
        fdk_text_metrics gm;
        (void)fdk_font_measure_utf8(f, text + i, 1, &gm);
        int dy = (int)(14.0 * sin(t + (double)i * 0.55));
        (void)fdk_surface_draw_utf8(s, f, text + i, 1, pen,
                                    mid_baseline + dy,
                                    col(130, 210, 255));
        pen += gm.advance_width;
    }
}

int main(void) {
    fdk_font *f16 = fdk_font_load_system_default(16);
    if (f16 == NULL) {
        fprintf(stderr,
                "05_text: no system TrueType font found — this demo "
                "needs one to shape. Install a face like DejaVu Sans "
                "or Noto Sans, or point FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("05_text: using font %s\n", fdk_font_get_file_path(f16));

    fdk_font *f12 = fdk_font_load(fdk_font_get_file_path(f16), 12);
    fdk_font *f24 = fdk_font_load(fdk_font_get_file_path(f16), 24);
    fdk_font *f48 = fdk_font_load(fdk_font_get_file_path(f16), 48);
    fdk_font *f96 = fdk_font_load(fdk_font_get_file_path(f16), 96);
    if (f96 == NULL || f48 == NULL || f24 == NULL || f16 == NULL ||
        f12 == NULL) {
        fprintf(stderr, "05_text: font load failed\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 05 — text",
        .width = 640,
        .height = 480,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);
    fdk_window_show(app.window);
    (void)fdk_pump_events(ctx, 100);

    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }

        fdk_surface *s = NULL;
        if (!fdk_ok(fdk_window_get_surface(app.window, &s))) {
            continue;
        }

        /* HOLD phase: freeze the wave's clock at 0 for deterministic
         * screenshots; animate it before that. */
        double t = (frames < 24 * 8) ? (double)frames / 24.0 * 3.0 : 0.0;

        fdk_surface_fill(s, col(18, 20, 28));

        /* wordmark */
        (void)fdk_surface_draw_utf8(s, f96, "FDK", 3, 40, 110,
                                    col(240, 240, 245));
        /* subtitle, right of the wordmark (two single-line runs — the
         * text API is strictly one line per call) */
        fdk_text_metrics wm;
        (void)fdk_font_measure_utf8(f96, "FDK", 3, &wm);
        int sub_x = 40 + wm.advance_width + 16;
        (void)draw_run(s, f16, "software text,", sub_x, 84,
                       col(140, 150, 170));
        (void)draw_run(s, f16, "kerned + cached", sub_x, 106,
                       col(140, 150, 170));

        /* size ladder */
        int y = 150;
        (void)draw_run(s, f12, "12 px — The quick brown fox jumps over the lazy dog",
                       40, y, col(120, 200, 160));
        y += 24;
        (void)draw_run(s, f16, "16 px — The quick brown fox jumps over the lazy dog",
                       40, y, col(150, 210, 170));
        y += 32;
        (void)draw_run(s, f24, "24 px — The quick brown fox jumps",
                       40, y, col(190, 230, 180));

        /* colored runs, chained by measured advance (no magic offsets) */
        y += 56;
        int pen = 40;
        pen = draw_run(s, f24, "red ", pen, y, col(235, 100, 100));
        pen = draw_run(s, f24, "green ", pen, y, col(110, 220, 130));
        pen = draw_run(s, f24, "blue", pen, y, col(110, 150, 235));
        /* translucent ghost text under the runs */
        (void)draw_run(s, f16, "runs placed by fdk_font_measure_utf8()",
                       40, y + 26, col(110, 120, 150));

        /* the wave */
        (void)draw_wave(s, f24, "FDK text rendering!", 40, 360, t);

        /* stats footer */
        fdk_font_cache_stats st;
        fdk_font_get_cache_stats(f24, &st);
        char footer[160];
        snprintf(footer, sizeof(footer),
                 "24px glyph cache: %d resident | %d hits | %d "
                 "rasterized | %d evicted | frame %d",
                 st.cached_glyphs, st.cache_hits, st.cache_misses,
                 st.evictions, frames);
        (void)draw_run(s, f12, footer, 40, 448, col(130, 140, 165));

        (void)fdk_surface_present(s);
        frames++;

        if (frames == 24 * 8) {
            printf("HOLD_A\n");
            fflush(stdout);
        } else if (frames >= 24 * 14) {
            /* HOLD_A lasts ~2.2s of steady frames — enough margin
             * for the test rig's marker poll + settle + capture
             * (found live: a 72-frame hold left the rig's grab only
             * ~0.24s before exit, one slow ffmpeg startup from
             * grabbing a dead screen). */
            app.quit = true;
        }
    }

    printf("05_text: exited cleanly after %d frames\n", frames);
    fdk_font_destroy(f96);
    fdk_font_destroy(f48);
    fdk_font_destroy(f24);
    fdk_font_destroy(f16);
    fdk_font_destroy(f12);
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_shutdown(ctx);
    return 0;
}
