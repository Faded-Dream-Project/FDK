/* 07_text_layout.c — the Phase 6 text-layout features, live: word
 * wrap, ellipsize, and line alignment in Labels, plus a radio group
 * whose selection the keyboard arrows own.
 *
 * Every text element here is one Label with a mode: the paragraph
 * re-wraps when the window narrows (phase 3 resizes it live), the
 * path-like line truncates with an ellipsis exactly at its right
 * edge, and the three alignment rows hug the left edge, the center,
 * and the right edge of the same width.
 *
 * The demo drives deterministically for the test rig: HOLD_A (full
 * width, "South" selected), HOLD_B (selection moved to "North" —
 * the same transition the arrow keys make), HOLD_C (window narrowed:
 * the paragraph re-wraps taller and the truncation point moves).
 * Escape or the close request ends it. Needs a system TrueType font
 * (exits with a notice otherwise).
 */

#include "fdk/fdk.h"

#include <stdio.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_font *font16 = NULL;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void set_status(fdk_widget *status, const char *text) {
    (void)fdk_label_set_text(status, text);
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

int main(void) {
    font16 = fdk_font_load_system_default(16);
    if (font16 == NULL) {
        fprintf(stderr, "07_text_layout: no system TrueType font "
                        "found — this demo needs one. Install a face "
                        "like DejaVu Sans or Noto Sans, or point "
                        "FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("07_text_layout: using font %s\n",
           fdk_font_get_file_path(font16));

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 07 — text layout",
        .width = 480,
        .height = 620,
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

    /* --- frame: a wrapping paragraph --- */
    fdk_widget *para_frame = NULL;
    (void)fdk_frame_create(content, font16, "Paragraph", &para_frame);
    fdk_widget_set_background(para_frame, col(26, 29, 40));
    fdk_widget *para = NULL;
    (void)fdk_label_create(
        para_frame, font16,
        "FDK wraps text with a greedy word-wrap that never splits a "
        "word unless the word alone is wider than the line. Lines are "
        "measured by the same shaping walk that paints them, so what "
        "you see is exactly what was measured. Narrow this window and "
        "the paragraph reflows taller.",
        &para);
    fdk_label_set_mode(para, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(para, 260, 0);
    fdk_widget_set_expand(para, true, false);

    /* --- frame: ellipsized truncation --- */
    fdk_widget *trunc_frame = NULL;
    (void)fdk_frame_create(content, font16, "Truncation", &trunc_frame);
    fdk_widget_set_background(trunc_frame, col(26, 29, 40));
    fdk_widget *trunc = NULL;
    (void)fdk_label_create(
        trunc_frame, font16,
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf runs out of "
        "room exactly where the ellipsis lands",
        &trunc);
    fdk_label_set_mode(trunc, FDK_LABEL_ELLIPSIZE);
    fdk_label_set_color(trunc, col(150, 158, 178));
    fdk_widget_set_expand(trunc, true, false);

    /* --- frame: the three alignments --- */
    fdk_widget *align_frame = NULL;
    (void)fdk_frame_create(content, font16, "Alignment", &align_frame);
    fdk_widget_set_background(align_frame, col(26, 29, 40));
    fdk_widget *a_start = NULL, *a_center = NULL, *a_end = NULL;
    (void)fdk_label_create(align_frame, font16, "<< start-aligned",
                           &a_start);
    (void)fdk_label_create(align_frame, font16, "centered line",
                           &a_center);
    (void)fdk_label_create(align_frame, font16, "right-aligned >>",
                           &a_end);
    fdk_widget_set_expand(a_start, true, false);
    fdk_widget_set_expand(a_center, true, false);
    fdk_widget_set_expand(a_end, true, false);
    fdk_label_set_alignment(a_center, FDK_ALIGN_CENTER);
    fdk_label_set_alignment(a_end, FDK_ALIGN_END);

    /* --- frame: keyboard-owned selection --- */
    fdk_widget *key_frame = NULL;
    (void)fdk_frame_create(content, font16, "Keyboard", &key_frame);
    fdk_widget_set_background(key_frame, col(26, 29, 40));
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    (void)fdk_radio_create(key_frame, font16, "North", &r1);
    (void)fdk_radio_create(key_frame, font16, "East", &r2);
    (void)fdk_radio_create(key_frame, font16, "South", &r3);
    fdk_widget *status = NULL;
    (void)fdk_label_create(key_frame, font16, "", &status);
    fdk_label_set_color(status, col(150, 158, 178));
    fdk_radio_set_checked(r3, true);
    set_status(status, "Selection: South — arrow keys move it");

    fdk_window_show(app.window);
    (void)fdk_window_paint(app.window);

    /* Deterministic drive for the rig: hold, move the selection
     * (the same transition the arrow keys make), hold, narrow the
     * window (live re-wrap + re-ellipsis), hold, exit. */
    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }

        if (frames == 40) {
            printf("HOLD_A\n");
            fflush(stdout);
        } else if (frames == 240) {
            fdk_radio_set_checked(r1, true);
            set_status(status, "Selection: North — moved by one key");
        } else if (frames == 280) {
            printf("HOLD_B\n");
            fflush(stdout);
        } else if (frames == 460) {
            fdk_window_resize(app.window, 340, 620);
        } else if (frames == 520) {
            printf("HOLD_C\n");
            fflush(stdout);
        } else if (frames >= 800) {
            app.quit = true;
        }
        frames++;

        if (fdk_widget_tree_has_damage(root)) {
            (void)fdk_window_paint(app.window);
        }
    }

    fdk_window_destroy(app.window);
    fdk_font_destroy(font16);
    fdk_shutdown(ctx);
    printf("07_text_layout: done\n");
    return 0;
}
