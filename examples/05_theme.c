/* 05_theme.c — the Phase 7 theme engine, live.
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

#include "example_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *font16 = NULL;
static fdk_widget *title = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static fdk_widget *root = NULL;
static fdk_widget *next_btn = NULL;
static fdk_example *g_ex = NULL;

/* The cycle: built-in, then two parsed files. */
#define THEME_COUNT 3
static fdk_theme *themes[THEME_COUNT];
static int current_theme = 0;

static const char *THEME_FILES[] = {
    NULL, /* index 0: the built-in theme */
    "examples/data/daylight.fdk",
    "examples/data/matrix.fdk",
};

/* Applies the app's own themed styling on top of the engine's
 * repaint: root surface color and title accent come from tokens of
 * the theme that is CURRENT now — and the example header (the
 * helper's chrome) re-themes with it, the documented app-side
 * re-theme pattern extended to every label the window owns. */
static void apply_app_theming(void) {
    fdk_widget_set_background(
        root, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));
    fdk_label_set_color(title, fdk_theme_get_color(NULL, FDK_TK_ACCENT));
    if (g_ex != NULL) {
        fdk_label_set_color(g_ex->header_label,
                            fdk_theme_get_color(NULL, FDK_TK_TEXT));
        fdk_label_set_color(
            g_ex->header_hint,
            fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED));
    }
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
    if (g_ex != NULL) {
        g_ex->quit = true;
    }
}

static void print_rig_rect(const char *what, fdk_widget *w) {
    fdk_rect b = fdk_widget_get_absolute_bounds(w);
    printf("RIG: %s %d %d %d %d\n", what, b.x, b.y, b.width, b.height);
    fflush(stdout);
}

int main(void) {
    font16 = fdk_font_load_system_default(16);
    if (font16 == NULL) {
        fprintf(stderr, "05_theme: no system TrueType font found — "
                        "this demo needs one. Install a face like "
                        "DejaVu Sans or Noto Sans, or point "
                        "FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("05_theme: using font %s\n",
           fdk_font_get_file_path(font16));

    /* Themes 1 and 2 come from .fdk files — the same parser a
     * downloaded theme would go through. */
    for (int i = 1; i < THEME_COUNT; i++) {
        fdk_result r = FDK_ERR_UNKNOWN;
        themes[i] = fdk_theme_load(THEME_FILES[i], &r);
        if (themes[i] == NULL) {
            fprintf(stderr, "05_theme: cannot load %s (%s) — run from "
                            "the repository root\n",
                    THEME_FILES[i], fdk_result_to_string(r));
            return 1;
        }
        printf("loaded theme: %s (from %s)\n", fdk_theme_name(themes[i]),
               THEME_FILES[i]);
    }
    themes[0] = NULL; /* the built-in is installed via set_default(NULL) */

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "05")) {
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "05", "theme engine", 460, 395)) {
        fdk_shutdown(ctx);
        return 1;
    }
    g_ex = &ex;
    root = ex.root;
    fdk_widget *content = ex.content;

    title = NULL;
    (void)fdk_label_create(content, font16, "Theme engine", &title);

    /* The demo's status line IS the helper's (bottom of the frame). */
    status = ex.status;

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

    /* fdk_example_open already showed + painted the frame; the
     * theming pass re-styles it and the first pump repaints. */
    apply_app_theming();
    /* Report button geometry to the rig AFTER the first layout. */
    print_rig_rect("next", next_btn);
    print_rig_rect("quit", quit_btn);

    const char *anim = getenv("FDK_DEMO_ANIMATE");
    const bool animate =
        anim != NULL && anim[0] != '\0' && strcmp(anim, "0") != 0;

    while (fdk_example_pump(&ex)) {
        /* One startup sweep (every theme is seen mid-motion), then
         * the meter holds — an idle app presents nothing. The rig can
         * keep it sweeping with FDK_DEMO_ANIMATE=1. */
        if (ex.frames < 400 || animate) {
            fdk_progress_set_fraction(progress,
                                      (fdk_f32)(ex.frames % 200) / 199.0f);
        }
    }

    /* Owned resources go first (nothing paints after the loop);
     * fdk_example_close handles window → helper font → context. */
    fdk_font_destroy(font16);
    for (int i = 1; i < THEME_COUNT; i++) {
        fdk_theme_destroy(themes[i]);
    }
    fdk_example_close(&ex);
    return 0;
}
