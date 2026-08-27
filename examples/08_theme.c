/* 08_theme.c — the Phase 7 theme engine, live.
 *
 * One panel of catalog widgets under three themes: the built-in
 * "FDK Dark" (the exact Phase 6 v1 palette), plus "Daylight" and
 * "Matrix" loaded from .fdk files in examples/data/. The "Next
 * theme" button cycles the default theme at runtime — FDK repaints
 * the whole tree (fills, text, accent, focus ring, separator
 * thickness, corner radius) and the demo re-applies its own themed
 * styling: root background and title color come from tokens, the
 * documented app-side re-theme pattern.
 *
 * For the test rig the demo prints two machine-readable lines:
 *   RIG: next <x> <y> <w> <h>   — the Next-theme button's absolute
 *                                bounds after the first layout
 *   RIG: quit <x> <y> <w> <h>   — same for Quit
 *   PHASE: <theme name>         — after every switch (and once at
 *                                startup)
 * Escape or the close request ends it. Needs a system TrueType font
 * and runs from the repository root (the theme files are relative).
 */

#include "fdk/fdk.h"

#include <stdio.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_font *font16 = NULL;
static fdk_widget *title = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static fdk_widget *root = NULL;
static fdk_widget *next_btn = NULL;

/* The cycle: built-in, then two parsed files. */
#define THEME_COUNT 3
static fdk_theme *themes[THEME_COUNT];
static int current_theme = 0;

static const char *FONT_CANDIDATES[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    NULL,
};

static const char *THEME_FILES[] = {
    NULL, /* index 0: the built-in theme */
    "examples/data/daylight.fdk",
    "examples/data/matrix.fdk",
};

/* Applies the app's own themed styling on top of the engine's
 * repaint: root surface color and title accent come from tokens of
 * the theme that is CURRENT now. */
static void apply_app_theming(void) {
    fdk_widget_set_background(
        root, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));
    fdk_label_set_color(title, fdk_theme_get_color(NULL, FDK_TK_ACCENT));
    (void)fdk_label_set_text(status, fdk_theme_name(NULL));
    printf("PHASE: %s\n", fdk_theme_name(NULL));
    fflush(stdout);
}

static void on_next_theme(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    current_theme = (current_theme + 1) % THEME_COUNT;
    fdk_theme_set_default(themes[current_theme]);
    apply_app_theming();
}

static void on_quit(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    app.quit = true;
}

static void print_rig_rect(const char *what, fdk_widget *w) {
    fdk_rect b = fdk_widget_get_absolute_bounds(w);
    printf("RIG: %s %d %d %d %d\n", what, b.x, b.y, b.width, b.height);
    fflush(stdout);
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
    const char *font_path = NULL;
    for (int i = 0; FONT_CANDIDATES[i] != NULL; i++) {
        FILE *f = fopen(FONT_CANDIDATES[i], "rb");
        if (f != NULL) {
            fclose(f);
            font_path = FONT_CANDIDATES[i];
            break;
        }
    }
    if (font_path == NULL) {
        fprintf(stderr, "08_theme: no system TrueType font found — "
                        "this demo needs one\n");
        return 1;
    }
    font16 = fdk_font_load(font_path, 16);
    if (font16 == NULL) {
        fprintf(stderr, "08_theme: font load failed\n");
        return 1;
    }

    /* Themes 1 and 2 come from .fdk files — the same parser a
     * downloaded theme would go through. */
    for (int i = 1; i < THEME_COUNT; i++) {
        fdk_result r = FDK_ERR_UNKNOWN;
        themes[i] = fdk_theme_load(THEME_FILES[i], &r);
        if (themes[i] == NULL) {
            fprintf(stderr, "08_theme: cannot load %s (%s) — run from "
                            "the repository root\n",
                    THEME_FILES[i], fdk_result_to_string(r));
            return 1;
        }
        printf("loaded theme: %s (from %s)\n", fdk_theme_name(themes[i]),
               THEME_FILES[i]);
    }
    themes[0] = NULL; /* the built-in is installed via set_default(NULL) */

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 08 — theme engine",
        .width = 460,
        .height = 330,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    (void)fdk_window_get_root(app.window, &root);

    fdk_widget *content = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &content);
    fdk_box_set_padding(content, 14);
    fdk_box_set_spacing(content, 10);
    fdk_window_set_content(app.window, content);

    title = NULL;
    (void)fdk_label_create(content, font16, "Theme engine", &title);

    status = NULL;
    (void)fdk_label_create(content, font16, "", &status);

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* The two buttons the rig drives. */
    fdk_widget *row = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 10);
    (void)fdk_button_create(row, font16, "Next theme", &next_btn);
    fdk_button_set_on_activate(next_btn, on_next_theme, NULL);
    fdk_widget *quit_btn = NULL;
    (void)fdk_button_create(row, font16, "Quit", &quit_btn);
    fdk_button_set_on_activate(quit_btn, on_quit, NULL);
    fdk_widget *filler = NULL;
    (void)fdk_widget_create(row, NULL, (fdk_rect){0, 0, 0, 1}, &filler);
    fdk_widget_set_expand(filler, true, false);

    /* A couple of stateful controls so themed checked/unchecked and
     * text colors are all on screen at once. */
    fdk_widget *greet = NULL;
    (void)fdk_toggle_create(content, font16, "Greeting", &greet);
    fdk_widget *cbs = NULL;
    (void)fdk_checkbox_create(content, font16, "Live preview", &cbs);
    fdk_checkbox_set_checked(cbs, true);

    (void)fdk_progress_create(content, &progress);
    fdk_widget_set_natural_size(progress, 0, 14);
    fdk_widget_set_expand(progress, true, false);

    fdk_window_show(app.window);
    apply_app_theming();
    (void)fdk_window_paint(app.window);

    /* Report button geometry to the rig AFTER the first layout. */
    print_rig_rect("next", next_btn);
    print_rig_rect("quit", quit_btn);

    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }

        /* The meter sweeps forever: every theme is seen mid-motion,
         * and the loop proves themed painting is not a one-shot. */
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

    printf("08_theme: exited cleanly after %d frames\n", frames);
    fdk_font_destroy(font16);
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    for (int i = 1; i < THEME_COUNT; i++) {
        fdk_theme_destroy(themes[i]);
    }
    fdk_shutdown(ctx);
    return 0;
}
