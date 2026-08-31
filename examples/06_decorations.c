/* 06_decorations.c — the Phase 8 window decorations, live.
 *
 * One window running under FDK's OWN title bar: a themed band with
 * the window title and minimize / maximize-restore / close buttons
 * (vector glyphs — they render with or without fonts), the WM's
 * chrome asked away (_MOTIF_WM_HINTS on X11, xdg-decoration on
 * Wayland), the content laid out below the band, the band draggable
 * to move the window, double-click on the band toggling maximize,
 * and a 5px resize border around everything when FDK owns the
 * chrome. The "Toggle decorations" button flips between FDK-drawn
 * and platform decorations at runtime.
 *
 * For the test rig the demo prints:
 *   RIG: toggle <x> <y> <w> <h>  — the toggle button's bounds
 *   PHASE: on / PHASE: off       — after each toggle (and at start)
 *   WLSTATE: max=<0/1> min=<0/1> — every window-state change
 * With --wayland-auto the demo drives itself (no input devices
 * needed — weston headless kiosk-shell has no seat): a timed cycle
 * through decorate / maximize / unmaximize / minimize / undecorate,
 * printing the same markers, ending on decorated+maximized so the
 * screenshot shows the full package.
 *
 * Escape or the close request ends it. The content font comes from
 * fdk_font_load_system_default() — the same probe the title bar
 * uses. Needs a system TrueType font (the band buttons never do).
 *
 * INIT-tier helper user (see example_window.h): the demo owns its
 * window because the FDK decoration band IS the titlebar — a helper
 * header would duplicate it. The helper still provides the uniform
 * app_id (org.fdk.example06).
 */

#include "example_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_font *font16 = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static fdk_widget *root = NULL;

static void set_status(void) {
    if (status == NULL || app.window == NULL) {
        return; /* state events can precede widget creation */
    }
    char buf[128];
    if (fdk_window_get_decorated(app.window)) {
        snprintf(buf, sizeof buf,
                 "Decorations: ON%s - drag/double-click the band, "
                 "edges resize, buttons manage.",
                 fdk_window_is_maximized(app.window) ? " (maximized)"
                                                     : "");
    } else {
        snprintf(buf, sizeof buf,
                 "Decorations: OFF - the platform owns the chrome.");
    }
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
    } else if (event->type == FDK_EVENT_WINDOW_STATE) {
        printf("WLSTATE: max=%d min=%d\n",
               fdk_window_is_maximized(window) ? 1 : 0,
               fdk_window_is_minimized(window) ? 1 : 0);
        fflush(stdout);
        set_status();
    } else if (event->type == FDK_EVENT_WINDOW_DECORATION) {
        printf("WLDECO: compositor kept its own decorations\n");
        fflush(stdout);
        set_status();
    }
}

int main(int argc, char **argv) {
    bool wayland_auto = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wayland-auto") == 0) {
            wayland_auto = true;
        }
    }

    font16 = fdk_font_load_system_default(16);
    if (font16 == NULL) {
        fprintf(stderr, "06_decorations: no system TrueType font "
                        "found - this demo needs one\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "06")) {
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 06 - decorations",
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
        fprintf(stderr, "06_decorations: set_decorated failed (%s)\n",
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
    /* --wayland-auto: a self-driving cycle for compositors without
     * input devices (weston headless kiosk-shell has no seat). Each
     * step prints the same markers the input-driven rig reads. */
    int auto_step = 0;
    /* Time-based auto cycle (was frame-count-based): under HiDPI the
     * scaled framebuffer costs 4x the fill per frame under the
     * sanitized debug build, so frame counters made the rig's wall
     * clock budget flaky. Two seconds of real time per phase. */
    double auto_next_ms = 0.0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }
        struct timespec auto_now;
        clock_gettime(CLOCK_MONOTONIC, &auto_now);
        double auto_now_ms =
            (double)auto_now.tv_sec * 1000.0 + (double)auto_now.tv_nsec / 1e6;
        if (wayland_auto && auto_now_ms >= auto_next_ms) {
            switch (auto_step) {
            case 0:
                printf("AUTO: maximize\n");
                fflush(stdout);
                (void)fdk_window_maximize(app.window);
                auto_next_ms = auto_now_ms + 2000.0;
                break;
            case 1:
                printf("AUTO: unmaximize\n");
                fflush(stdout);
                (void)fdk_window_unmaximize(app.window);
                auto_next_ms = auto_now_ms + 2000.0;
                break;
            case 2:
                printf("AUTO: minimize\n");
                fflush(stdout);
                (void)fdk_window_minimize(app.window);
                auto_next_ms = auto_now_ms + 2000.0;
                break;
            case 3:
                printf("AUTO: undecorate\n");
                fflush(stdout);
                (void)fdk_window_set_decorated(app.window, false);
                printf("PHASE: off\n");
                fflush(stdout);
                auto_next_ms = auto_now_ms + 2000.0;
                break;
            case 4:
                /* End visible + decorated + maximized: the screenshot
                 * then shows the whole package. */
                printf("AUTO: redecorate + maximize (final state)\n");
                fflush(stdout);
                (void)fdk_window_set_decorated(app.window, true);
                printf("PHASE: on\n");
                fflush(stdout);
                (void)fdk_window_maximize(app.window);
                auto_next_ms = auto_now_ms + 2000.0;
                break;
            default:
                auto_next_ms = auto_now_ms + 2000.0; /* hold the final state */
                break;
            }
            auto_step++;
            set_status();
        }
        /* One startup sweep of the meter, then it holds (the rig can
         * keep it sweeping with FDK_DEMO_ANIMATE=1). */
        const char *anim = getenv("FDK_DEMO_ANIMATE");
        const bool keep_sweeping =
            (anim != NULL && anim[0] != '\0' && strcmp(anim, "0") != 0) ||
            wayland_auto;
        if (frames < 400 || keep_sweeping) {
            fdk_progress_set_fraction(progress,
                                      (fdk_f32)(frames % 200) / 199.0f);
        }

        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        if (fdk_widget_tree_has_damage(root)) {
            (void)fdk_window_paint(app.window);
        }
        frames++;
    }

    printf("06_decorations: exited cleanly after %d frames\n", frames);
    fdk_font_destroy(font16);
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_shutdown(ctx);
    return 0;
}
