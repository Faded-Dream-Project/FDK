/* 12_narrator.c — the embedded screen reader, live (1.1.0).
 *
 * GTK and Qt narrate through AT-SPI2 over D-Bus: a registry daemon,
 * a session bus, a bridge process. This demo is FDK's answer: the
 * narrator runs IN-PROCESS, subscribes to the same a11y
 * notifications any consumer sees, and speaks through a sink the
 * application wires — here, a subtitle label at the bottom of the
 * window plus a stdout line. No bus, no daemon, no TTS dependency;
 * a real app would point the same sink at its speech engine.
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
 * Escape or the window's close button ends the demo. Set
 * FDK_DEMO_FRAMES=N to exit after N frames instead (the automation
 * knob the screenshot battery uses).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_a11y.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    fdk_window *window;
    fdk_context *ctx;
    bool quit;
} app;

/* The visual sink: the subtitle bar + a stdout line. Runs inside
 * focus()/set-value call stacks — label text updates are ordinary
 * tree mutations, safe from any callback position (the a11y notify
 * walk is snapshot-based). */
static fdk_widget *subtitle = NULL;

static void speak(const char *utterance, void *user) {
    (void)user;
    printf("narrator: %s\n", utterance);
    if (subtitle != NULL) {
        (void)fdk_label_set_text(subtitle, utterance);
    }
}

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "12_narrator: init failed\n");
        return 1;
    }
    app.ctx = ctx;

    fdk_font *font = fdk_font_load_system_default(16);
    if (font == NULL) {
        fprintf(stderr,
                "12_narrator: no system font found — set FDK_FONT_FILE "
                "or FDK_FONT_DIRS (see docs/text.md)\n");
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_window_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.title = "FDK — the embedded narrator";
    opts.width = 460;
    opts.height = 400;
    if (!fdk_ok(fdk_window_create(ctx, &opts, &app.window))) {
        fprintf(stderr, "12_narrator: window failed (no display?)\n");
        fdk_font_destroy(font);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);
    fdk_widget_set_background(root, col(24, 26, 35));

    /* The form: a heading, a checkbox, a slider, a spin, buttons. */
    fdk_widget *title = NULL;
    (void)fdk_label_create(root, font, "Screen reader, no bus", &title);
    fdk_widget_set_bounds(title, (fdk_rect){16, 12, 428, 28});
    fdk_label_set_color(title, col(235, 238, 245));

    fdk_widget *hint = NULL;
    (void)fdk_label_create(
        root, font,
        "Tab / click / drag — every focus and value change is spoken",
        &hint);
    fdk_widget_set_bounds(hint, (fdk_rect){16, 40, 428, 20});
    fdk_label_set_color(hint, col(130, 139, 160));

    fdk_widget *bold = NULL;
    (void)fdk_checkbox_create(root, font, "Bold", &bold);
    fdk_widget_set_bounds(bold, (fdk_rect){20, 80, 160, 30});

    fdk_widget *volume = NULL;
    (void)fdk_slider_create(root, 0.0, 100.0, 30.0, &volume);
    fdk_widget_set_bounds(volume, (fdk_rect){200, 82, 240, 26});
    fdk_widget_set_accessible_name(volume, "Volume");

    fdk_widget *size = NULL;
    (void)fdk_spin_create(root, font, 6.0, 72.0, 12.0, &size);
    fdk_widget_set_bounds(size, (fdk_rect){20, 130, 160, 30});
    fdk_widget_set_accessible_name(size, "Font size");

    fdk_widget *ok = NULL;
    (void)fdk_button_create(root, font, "Apply", &ok);
    fdk_widget_set_bounds(ok, (fdk_rect){20, 190, 120, 36});

    fdk_widget *quit = NULL;
    (void)fdk_button_create(root, font, "Quit", &quit);
    fdk_widget_set_bounds(quit, (fdk_rect){150, 190, 120, 36});

    /* The subtitle bar: the sink's visual half. */
    (void)fdk_label_create(root, font, "narration: (idle)", &subtitle);
    fdk_widget_set_bounds(subtitle, (fdk_rect){12, 350, 436, 34});
    fdk_label_set_color(subtitle, col(120, 220, 160));

    /* The narrator: sink in, engine on. */
    fdk_a11y_set_speaker(speak, NULL);
    if (!fdk_ok(fdk_a11y_narrator_start())) {
        fprintf(stderr, "12_narrator: engine failed to start\n");
        fdk_window_destroy(app.window);
        fdk_font_destroy(font);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_a11y_announce("Narration on");

    fdk_window_show(app.window);
    (void)fdk_window_paint(app.window);

    /* The scripted tour: one scripted action every 24 frames
     * (~360 ms at the 15 ms pump). Then the demo goes interactive
     * until quit / close / the frame budget. */
    long frame_cap = -1;
    const char *cap_env = getenv("FDK_DEMO_FRAMES");
    if (cap_env != NULL) {
        frame_cap = atol(cap_env);
    }

    const int TOUR_PERIOD = 24;
    const int TOUR_STEPS = 7; /* focus+toggle, focus+value, 2 focuses,
                              * and the closing announcement */
    int tour_done = 0;

    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit || app.window == NULL) {
            break;
        }
        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        (void)fdk_window_paint(app.window);
        frames++;

        /* Tour driver: each action follows its focus, the way a
         * keyboard user drives the form — that is what the engine
         * narrates (toggle and value changes announce on the
         * FOCUSED widget). */
        if (tour_done < TOUR_STEPS && frames % TOUR_PERIOD == 0) {
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

        if (frame_cap >= 0 && frames >= frame_cap) {
            break;
        }
    }

    printf("12_narrator: exited cleanly after %d frames, %d tour steps\n",
           frames, tour_done);

    fdk_a11y_set_speaker(NULL, NULL); /* parks the engine */
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    return 0;
}
