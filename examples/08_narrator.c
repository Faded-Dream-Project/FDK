/* 08_narrator.c — the embedded screen reader, live (1.1.0).
 *
 * GTK and Qt narrate through AT-SPI2 over D-Bus: a registry daemon,
 * a session bus, a bridge process. This demo is FDK's answer: the
 * narrator runs IN-PROCESS, subscribes to the same a11y
 * notifications any consumer sees, and speaks through a sink the
 * application wires — here, the helper's status line (via
 * fdk_example_set_status) plus a stdout line. No bus, no daemon,
 * no TTS dependency; a real app would point the same sink at its
 * speech engine.
 *
 * What you should see (and read on stdout):
 *   - a scripted tour walks focus through the form; each move is
 *     narrated ("Bold, check box")
 *   - the checkbox toggles -> "..., checked"
 *   - the slider moves -> the compact value utterance ("Volume, 64")
 *   - a forced status announcement ("Settings saved")
 *   - then the narrator keeps running interactively: Tab through
 *     the form, click the checkbox, drag the slider — every focus
 *     move, toggle, and value change is spoken
 *
 * The window comes from the shared example helper (see
 * example_window.h): the standard header/status chrome, quit on
 * close/Escape, damage-gated frame-paced pump. The narrator is
 * parked (speaker NULL) before the helper's teardown order runs.
 *
 * Set FDK_DEMO_FRAMES=N to exit after N frames instead (the
 * automation knob the screenshot battery uses).
 */

#include "example_window.h"
#include "fdk/fdk_a11y.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_example *g_ex = NULL;

static void on_quit_clicked(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    if (g_ex != NULL) {
        g_ex->quit = true;
    }
}

/* The visual sink: the helper's status line + a stdout line. Runs
 * inside focus()/set-value call stacks — label text updates are
 * ordinary tree mutations, safe from any callback position (the
 * a11y notify walk is snapshot-based). */
static void speak(const char *utterance, void *user) {
    fdk_example *ex = user;
    printf("narrator: %s\n", utterance);
    if (ex != NULL) {
        fdk_example_set_status(ex, utterance);
    }
}

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "08")) {
        return 1;
    }

    fdk_font *font = fdk_font_load_system_default(16);
    if (font == NULL) {
        fprintf(stderr,
                "08_narrator: no system font found — set FDK_FONT_FILE "
                "or FDK_FONT_DIRS (see docs/text.md)\n");
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "08", "narrator", 460, 430)) {
        fdk_font_destroy(font);
        fdk_shutdown(ctx);
        return 1;
    }
    g_ex = &ex;
    fdk_widget_set_background(ex.root, col(24, 26, 35));
    fdk_widget *content = ex.content;

    /* The form: a heading, a checkbox, a slider, a spin, buttons —
     * all inside the helper's content box, so Tab order follows
     * creation order (the tour's "each action follows its focus"). */
    fdk_widget *title = NULL;
    (void)fdk_label_create(content, font, "Screen reader, no bus", &title);
    fdk_label_set_color(title, col(235, 238, 245));

    fdk_widget *hint = NULL;
    (void)fdk_label_create(
        content, font,
        "Tab / click / drag — every focus and value change is spoken",
        &hint);
    fdk_label_set_color(hint, col(130, 139, 160));

    fdk_widget *row1 = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row1);
    fdk_box_set_spacing(row1, 12);
    fdk_widget *bold = NULL;
    (void)fdk_checkbox_create(row1, font, "Bold", &bold);
    fdk_widget *volume = NULL;
    (void)fdk_slider_create(row1, 0.0, 100.0, 30.0, &volume);
    fdk_widget_set_expand(volume, true, false);
    fdk_widget_set_accessible_name(volume, "Volume");

    fdk_widget *row2 = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row2);
    fdk_box_set_spacing(row2, 12);
    fdk_widget *size = NULL;
    (void)fdk_spin_create(row2, font, 6.0, 72.0, 12.0, &size);
    fdk_widget_set_accessible_name(size, "Font size");
    fdk_widget *ok = NULL;
    (void)fdk_button_create(row2, font, "Apply", &ok);
    fdk_widget *quit = NULL;
    (void)fdk_button_create(row2, font, "Quit", &quit);
    fdk_button_set_on_activate(quit, on_quit_clicked, NULL);

    fdk_example_set_status(&ex, "narration: (idle)");

    /* The narrator: sink in, engine on. The sink receives &ex so
     * every utterance lands in the helper's status line. */
    fdk_a11y_set_speaker(speak, &ex);
    if (!fdk_ok(fdk_a11y_narrator_start())) {
        fprintf(stderr, "08_narrator: engine failed to start\n");
        fdk_example_close(&ex);
        fdk_font_destroy(font);
        return 1;
    }
    fdk_a11y_announce("Narration on");

    /* The scripted tour: one scripted action every 24 frames
     * (~360 ms at the 15 ms pump). Then the demo goes interactive
     * until quit / close / the frame budget. */
    const int TOUR_PERIOD = 24;
    const int TOUR_STEPS = 7; /* focus+toggle, focus+value, 2 focuses,
                              * and the closing announcement */
    int tour_done = 0;

    while (fdk_example_pump(&ex)) {
        /* Tour driver: each action follows its focus, the way a
         * keyboard user drives the form — that is what the engine
         * narrates (toggle and value changes announce on the
         * FOCUSED widget). */
        if (tour_done < TOUR_STEPS && ex.frames % TOUR_PERIOD == 0) {
            switch (tour_done) {
            case 0:
                fdk_widget_focus(bold);
                break;
            case 1:
                fdk_checkbox_set_checked(bold, true);
                break;
            case 2:
                fdk_widget_focus(volume);
                break;
            case 3:
                fdk_slider_set_value(volume, 64.0);
                break;
            case 4:
                fdk_widget_focus(size);
                break;
            case 5:
                fdk_widget_focus(ok);
                break;
            case 6:
                fdk_a11y_announce("Settings saved");
                break;
            default:
                break;
            }
            tour_done++;
        }
    }

    printf("08_narrator: %d tour steps\n", tour_done);

    /* Park the engine BEFORE the helper tears the window down. */
    fdk_a11y_set_speaker(NULL, NULL);
    fdk_font_destroy(font);
    fdk_example_close(&ex);
    return 0;
}
