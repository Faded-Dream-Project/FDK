/* 09_decorations.c — the Phase 8 window decorations, live.
 *
 * One window running under FDK's OWN title bar: a themed band with
 * the window title and a working close button, the WM's chrome asked
 * away via _MOTIF_WM_HINTS, the content laid out below the band, and
 * the band draggable to move the whole window. The "Toggle
 * decorations" button flips between FDK-drawn and WM decorations at
 * runtime.
 *
 * For the test rig the demo prints:
 *   RIG: toggle <x> <y> <w> <h>  — the toggle button's bounds
 *   PHASE: on / PHASE: off       — after each toggle (and at start)
 * Escape or the close request ends it. The content font comes from
 * fdk_font_load_system_default() — the same probe the title bar
 * uses. Needs a system TrueType font.
 */

#include "fdk/fdk.h"

#include <stdio.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_font *font16 = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static fdk_widget *root = NULL;

static void set_status(void) {
    char buf[96];
    snprintf(buf, sizeof buf,
             app.window != NULL && fdk_window_get_decorated(app.window)
                 ? "Decorations: ON - drag the band, x closes."
                 : "Decorations: OFF - the WM owns the chrome.");
    (void)fdk_label_set_text(status, buf);
}

static void on_toggle(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    bool now = !fdk_window_get_decorated(app.window);
    fdk_result r = fdk_window_set_decorated(app.window, now);
    if (fdk_ok(r)) {
        printf("PHASE: %s\n", now ? "on" : "off");
        fflush(stdout);
        /* The button MOVES when the band appears/disappears (the
         * content reflows) - re-report so the rig clicks where the
         * button actually is now. */
        fdk_rect tr = fdk_widget_get_absolute_bounds(w);
        printf("RIG: toggle %d %d %d %d\n", tr.x, tr.y, tr.width,
               tr.height);
        fflush(stdout);
    } else {
        (void)fdk_label_set_text(status, "This backend cannot drop "
                                         "its own decorations.");
    }
    set_status();
}

static void on_quit(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    app.quit = true;
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
        fprintf(stderr, "09_decorations: no system TrueType font "
                        "found - this demo needs one\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 09 - decorations",
        .width = 460,
        .height = 300,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    (void)fdk_window_get_root(app.window, &root);
    fdk_widget_set_background(
        root, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    fdk_widget *content = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &content);
    fdk_box_set_padding(content, 14);
    fdk_box_set_spacing(content, 10);
    fdk_window_set_content(app.window, content);

    fdk_widget *title = NULL;
    (void)fdk_label_create(content, font16, "FDK-drawn decorations",
                           &title);
    fdk_label_set_color(title, fdk_theme_get_color(NULL, FDK_TK_ACCENT));

    status = NULL;
    (void)fdk_label_create(content, font16, "", &status);

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    fdk_widget *row = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 10);
    fdk_widget *toggle = NULL;
    (void)fdk_button_create(row, font16, "Toggle decorations",
                            &toggle);
    fdk_button_set_on_activate(toggle, on_toggle, NULL);
    fdk_widget *quit_btn = NULL;
    (void)fdk_button_create(row, font16, "Quit", &quit_btn);
    fdk_button_set_on_activate(quit_btn, on_quit, NULL);
    fdk_widget *filler = NULL;
    (void)fdk_widget_create(row, NULL, (fdk_rect){0, 0, 0, 1},
                            &filler);
    fdk_widget_set_expand(filler, true, false);

    (void)fdk_progress_create(content, &progress);
    fdk_widget_set_natural_size(progress, 0, 14);
    fdk_widget_set_expand(progress, true, false);

    /* Decorate BEFORE the first show: the window appears with the
     * FDK band already in place. */
    fdk_result r = fdk_window_set_decorated(app.window, true);
    if (!fdk_ok(r)) {
        fprintf(stderr, "09_decorations: set_decorated failed (%s)\n",
                fdk_result_to_string(r));
        return 1;
    }

    fdk_window_show(app.window);
    set_status();
    printf("PHASE: on\n");
    fflush(stdout);
    (void)fdk_window_paint(app.window);

    fdk_rect tr = fdk_widget_get_absolute_bounds(toggle);
    printf("RIG: toggle %d %d %d %d\n", tr.x, tr.y, tr.width,
           tr.height);
    fflush(stdout);

    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }
        fdk_progress_set_fraction(progress,
                                  (fdk_f32)(frames % 200) / 199.0f);

        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        (void)fdk_window_paint(app.window);
        frames++;
    }

    printf("09_decorations: exited cleanly after %d frames\n", frames);
    fdk_font_destroy(font16);
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_shutdown(ctx);
    return 0;
}
