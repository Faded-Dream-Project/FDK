/* 05_theme.c — the theme engine, live (Phase 7; 1.4.16: the three
 * faces).
 *
 * One panel of catalog widgets cycling the toolkit's whole shipped
 * face collection: the built-in "Faded Dream" (the 1.4.16 default —
 * gray with violet accents), then five .fdk files — "Daylight" and
 * "Matrix" from examples/data/, and the 1.4.16 trio from the
 * source tree's .FDKThemes/ folder: "Mono Chromatic" (black/white,
 * CIRCLE buttons, black band), "Pink Rave" (all-pink, hot-pink
 * band), and "Faded Dream" (the packaged copy of the default).
 * The "Next theme" button cycles the default theme at runtime —
 * FDK repaints the whole tree (fills, text, accent, focus ring,
 * separator thickness, corner radius, BUTTON SHAPE, and the FDK
 * title band's own chrome — the titlebar family) and the demo
 * re-applies its own themed styling: the title color comes from a
 * token, the documented app-side re-theme pattern.
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
static fdk_widget *next_btn = NULL;
static fdk_example *g_ex = NULL;

/* The cycle: the built-in, then five parsed files — the two
 * examples/data/ fixtures and the three 1.4.16 faces from the
 * source tree's .FDKThemes/ folder (the same files `make install`
 * ships, so the demo doubles as a smoke test of the custom folder). */
#define THEME_COUNT 6
static fdk_theme *themes[THEME_COUNT];
static int current_theme = 0;

static const char *THEME_FILES[] = {
    NULL, /* index 0: the built-in theme */
    "examples/data/daylight.fdk",
    "examples/data/matrix.fdk",
    ".FDKThemes/mono-chromatic.fdk",
    ".FDKThemes/pink-rave.fdk",
    ".FDKThemes/faded-dream.fdk",
};

/* Applies the app's own themed styling on top of the engine's
 * repaint: the title accent comes from a token of the theme that is
 * CURRENT now. 1.4.14: the example's chrome — the FDK title band
 * the window now wears — themes ITSELF through the same tokens
 * (that is the toolkit's chrome doing what it preaches); 1.4.16:
 * the root surface rides the window_background TOKEN (the new
 * token-following background — no re-set needed on a switch), so
 * the only app-owned restyle left is this title label. */
static void apply_app_theming(void) {
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

    /* Themes 1..5 come from .fdk files — the same parser a
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
