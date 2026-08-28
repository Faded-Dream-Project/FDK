/* 03_text.c — the text stack, end to end.
 *
 * Two layers of the same engine in one window:
 *
 *   TOP (a Canvas widget, painted by the app): raw text rendering —
 *     a 96px "FDK" wordmark, a size ladder (12 / 16 / 24 px of the
 *     same sentence), colored runs placed by MEASURED widths (each
 *     run starts where the previous one's advance ended), a wave
 *     line where every glyph rides its own baseline, and a live
 *     glyph-cache stats footer. Everything here is shaped, kerned,
 *     rasterized, cached, and alpha-blended by fdk_surface_draw_utf8
 *     driven straight from the application.
 *
 *   BOTTOM (ordinary widgets): text LAYOUT — the label modes.
 *     A wrapping paragraph (narrow the window and it reflows
 *     taller), an ellipsized path line that truncates exactly at its
 *     right edge, the three alignments on one width, and a radio
 *     group the keyboard arrows own.
 *
 * The wave is static (t = 0); an idle FDK app costs zero presents.
 * Set FDK_DEMO_ANIMATE=1 to see it undulate forever, or
 * FDK_DEMO_FRAMES=N to exit after N frames (the automation knobs
 * the test rigs use).
 *
 * Needs a system TrueType font; discovery is fdk_font_load_system
 * _default()'s job (fontconfig, $FDK_FONT_FILE / $FDK_FONT_DIRS,
 * then a ranked font-directory scan), and the demo exits with a
 * notice if the environment has none. Close the window or press
 * ESC to exit.
 */

#include "fdk/fdk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_font *f12 = NULL;
static fdk_font *f16 = NULL;
static fdk_font *f24 = NULL;
static fdk_font *f96 = NULL;

/* Animation clock (frozen at 0 unless FDK_DEMO_ANIMATE is set). */
static double g_wave_t = 0.0;

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

/* ---- the canvas: raw text rendering (no widgets involved) ---- */

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
 * the shaper uses. */
static void draw_wave(fdk_surface *s, fdk_font *f, const char *text,
                      int pen_x, int mid_baseline, double t) {
    size_t len = strlen(text);
    fdk_text_metrics run_m;
    (void)fdk_font_measure_utf8(f, text, len, &run_m);
    int start_x = pen_x;

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

static void canvas_paint(fdk_widget *canvas, fdk_surface *s,
                         fdk_rect bounds, fdk_rect clip, void *user) {
    (void)canvas;
    (void)bounds;
    (void)clip;
    (void)user;

    fdk_surface_fill(s, col(18, 20, 28));

    /* wordmark */
    (void)fdk_surface_draw_utf8(s, f96, "FDK", 3, 40, 100,
                                col(240, 240, 245));
    /* subtitle, right of the wordmark (two single-line runs — the
     * text API is strictly one line per call) */
    fdk_text_metrics wm;
    (void)fdk_font_measure_utf8(f96, "FDK", 3, &wm);
    int sub_x = 40 + wm.advance_width + 16;
    (void)draw_run(s, f16, "software text,", sub_x, 74,
                   col(140, 150, 170));
    (void)draw_run(s, f16, "kerned + cached", sub_x, 96,
                   col(140, 150, 170));

    /* size ladder */
    (void)draw_run(s, f12, "12 px — The quick brown fox jumps over the lazy dog",
                   40, 140, col(120, 200, 160));
    (void)draw_run(s, f16, "16 px — The quick brown fox jumps over the lazy dog",
                   40, 164, col(150, 210, 170));
    (void)draw_run(s, f24, "24 px — The quick brown fox jumps",
                   40, 196, col(190, 230, 180));

    /* colored runs, chained by measured advance (no magic offsets) */
    int y = 252;
    int pen = 40;
    pen = draw_run(s, f24, "red ", pen, y, col(235, 100, 100));
    pen = draw_run(s, f24, "green ", pen, y, col(110, 220, 130));
    pen = draw_run(s, f24, "blue", pen, y, col(110, 150, 235));
    /* translucent ghost text under the runs */
    (void)draw_run(s, f16, "runs placed by fdk_font_measure_utf8()",
                   40, y + 26, col(110, 120, 150));

    /* the wave (static unless FDK_DEMO_ANIMATE) */
    (void)draw_wave(s, f24, "FDK text rendering!", 40, 330, g_wave_t);

    /* stats footer */
    fdk_font_cache_stats st;
    fdk_font_get_cache_stats(f24, &st);
    char footer[160];
    snprintf(footer, sizeof(footer),
             "24px glyph cache: %d resident | %d hits | %d "
             "rasterized | %d evicted",
             st.cached_glyphs, st.cache_hits, st.cache_misses,
             st.evictions);
    (void)draw_run(s, f12, footer, 40, 368, col(130, 140, 165));
}

/* ---- the widget layer: label modes ---- */

static void set_status(fdk_widget *status, const char *text) {
    (void)fdk_label_set_text(status, text);
}

int main(void) {
    f16 = fdk_font_load_system_default(16);
    if (f16 == NULL) {
        fprintf(stderr, "03_text: no system TrueType font found — this "
                        "demo needs one to shape. Install a face like "
                        "DejaVu Sans or Noto Sans, or point FDK_FONT_FILE "
                        "at a .ttf/.ttc\n");
        return 1;
    }
    printf("03_text: using font %s\n", fdk_font_get_file_path(f16));

    const char *anim = getenv("FDK_DEMO_ANIMATE");
    const bool animate =
        anim != NULL && anim[0] != '\0' && strcmp(anim, "0") != 0;
    (void)animate; /* g_wave_t stays 0 unless the rig animates it */

    f12 = fdk_font_load(fdk_font_get_file_path(f16), 12);
    f24 = fdk_font_load(fdk_font_get_file_path(f16), 24);
    f96 = fdk_font_load(fdk_font_get_file_path(f16), 96);
    if (f96 == NULL || f24 == NULL || f12 == NULL) {
        fprintf(stderr, "03_text: font load failed\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 03 — text",
        .width = 640,
        .height = 700,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);
    fdk_widget_set_background(root, col(18, 20, 28));

    fdk_widget *content = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &content);
    fdk_box_set_padding(content, 14);
    fdk_box_set_spacing(content, 12);
    fdk_window_set_content(app.window, content);

    /* --- canvas: the raw-rendering gallery (fixed height) --- */
    fdk_widget *canvas = NULL;
    (void)fdk_canvas_create(content, canvas_paint, NULL, &canvas);
    fdk_widget_set_natural_size(canvas, 0, 380);
    fdk_widget_set_expand(canvas, true, false);

    /* --- frame: a wrapping paragraph --- */
    fdk_widget *para_frame = NULL;
    (void)fdk_frame_create(content, f16, "Paragraph", &para_frame);
    fdk_widget_set_background(para_frame, col(26, 29, 40));
    fdk_widget *para = NULL;
    (void)fdk_label_create(
        para_frame, f16,
        "FDK wraps text with a greedy word-wrap that never splits a "
        "word unless the word alone is wider than the line. Lines are "
        "measured by the same shaping walk that paints them, so what "
        "you see is exactly what was measured. Narrow this window and "
        "the paragraph reflows taller.",
        &para);
    fdk_label_set_mode(para, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(para, 300, 0);
    fdk_widget_set_expand(para, true, false);

    /* --- frame: ellipsized truncation --- */
    fdk_widget *trunc_frame = NULL;
    (void)fdk_frame_create(content, f16, "Truncation", &trunc_frame);
    fdk_widget_set_background(trunc_frame, col(26, 29, 40));
    fdk_widget *trunc = NULL;
    (void)fdk_label_create(
        trunc_frame, f16,
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf runs out of "
        "room exactly where the ellipsis lands",
        &trunc);
    fdk_label_set_mode(trunc, FDK_LABEL_ELLIPSIZE);
    fdk_label_set_color(trunc, col(150, 158, 178));
    fdk_widget_set_expand(trunc, true, false);

    /* --- frame: the three alignments --- */
    fdk_widget *align_frame = NULL;
    (void)fdk_frame_create(content, f16, "Alignment", &align_frame);
    fdk_widget_set_background(align_frame, col(26, 29, 40));
    fdk_widget *a_start = NULL, *a_center = NULL, *a_end = NULL;
    (void)fdk_label_create(align_frame, f16, "<< start-aligned",
                           &a_start);
    (void)fdk_label_create(align_frame, f16, "centered line",
                           &a_center);
    (void)fdk_label_create(align_frame, f16, "right-aligned >>",
                           &a_end);
    fdk_widget_set_expand(a_start, true, false);
    fdk_widget_set_expand(a_center, true, false);
    fdk_widget_set_expand(a_end, true, false);
    fdk_label_set_alignment(a_center, FDK_ALIGN_CENTER);
    fdk_label_set_alignment(a_end, FDK_ALIGN_END);

    /* --- frame: keyboard-owned selection --- */
    fdk_widget *key_frame = NULL;
    (void)fdk_frame_create(content, f16, "Keyboard", &key_frame);
    fdk_widget_set_background(key_frame, col(26, 29, 40));
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    (void)fdk_radio_create(key_frame, f16, "North", &r1);
    (void)fdk_radio_create(key_frame, f16, "East", &r2);
    (void)fdk_radio_create(key_frame, f16, "South", &r3);
    fdk_widget *status = NULL;
    (void)fdk_label_create(key_frame, f16, "", &status);
    fdk_label_set_color(status, col(150, 158, 178));
    fdk_radio_set_checked(r3, true);
    set_status(status, "Selection: South — arrow keys move it");

    fdk_window_show(app.window);
    (void)fdk_window_paint(app.window);

    const char *limit_s = getenv("FDK_DEMO_FRAMES");
    const int frame_limit = (limit_s != NULL) ? atoi(limit_s) : 0;
    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }

        if (animate) {
            /* The rig's animated mode: the wave undulates — the whole
             * canvas repaints per frame (its damage), nothing else. */
            g_wave_t = (double)frames / 24.0 * 3.0;
            fdk_canvas_invalidate(canvas);
        }

        /* Damage-gated painting: an idle text demo presents nothing. */
        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        if (fdk_widget_tree_has_damage(root)) {
            (void)fdk_window_paint(app.window);
        }
        frames++;

        if (frame_limit > 0 && frames >= frame_limit) {
            break;
        }
    }

    printf("03_text: exited cleanly after %d frames\n", frames);
    fdk_window_destroy(app.window);
    fdk_font_destroy(f96);
    fdk_font_destroy(f24);
    fdk_font_destroy(f16);
    fdk_font_destroy(f12);
    fdk_shutdown(ctx);
    return 0;
}
