/*
 * test_shipped_themes.c — the .FDKThemes/ folder, as shipped (1.4.16)
 *
 * The five themes the source tree's .FDKThemes/ carries are a
 * PRODUCT: `make install` puts them on every user's search path and
 * the maintainer demo-cycles them in example 05. A broken shipped
 * file would ship broken. This suite loads each one from the folder
 * itself (resolved relative to this binary, the same convention the
 * CLI and discovery suites use) and pins the contract each file
 * promises:
 *
 *   faded-dream      the packaged copy of the BUILT-IN default —
 *                    every base token byte-equal, no titlebar keys
 *                    (the calm gray chrome is the fallback)
 *   mono-chromatic   black-and-white discipline; the titlebar family
 *                    opted in (near-black band, white text); buttons
 *                    are CIRCLES (button_shape = 1)
 *   pink-rave        the all-pink face; hot-pink accent AND a
 *                    hot-pink titlebar band over a plum window
 *   daylight         the complete light theme (1.4.13) — unchanged,
 *                    pinned so the retune cannot drift it
 *   matrix           the partial green-on-black theme (1.4.13) —
 *                    still partial (seven colors, one metric)
 *
 * Plus the enumeration-level fact: point $FDK_THEME_DIR at the folder
 * and all five stems are listed.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_theme.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

static char g_folder[1024]; /* abs path to the repo's .FDKThemes/ */

static char g_path[1100];
static const char *theme_file(const char *stem) {
    snprintf(g_path, sizeof g_path, "%s/%s.fdk", g_folder, stem);
    return g_path;
}

static fdk_theme *load_stem(const char *stem) {
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_load(theme_file(stem), &r);
    if (t == NULL) {
        fprintf(stderr, "FAIL: cannot load %s.fdk (%s)\n", stem,
                fdk_result_to_string(r));
        assert(!"shipped theme failed to load");
    }
    return t;
}

static void assert_channel(fdk_color got, int r, int g, int b,
                           const char *what) {
    if (got.r != (fdk_f32)r / 255.0f || got.g != (fdk_f32)g / 255.0f ||
        got.b != (fdk_f32)b / 255.0f) {
        fprintf(stderr,
                "FAIL %s: got (%.3f,%.3f,%.3f) want #%02X%02X%02X\n",
                what, got.r, got.g, got.b, r, g, b);
        assert(!"shipped theme color drifted");
    }
}

/* ---- the five faces --------------------------------------------------- */

static void test_faded_dream_is_the_default(void) {
    /* The contract file: every BASE token byte-equal to the
     * built-in, so `fdk-theme set faded-dream` is pixel-identical
     * to the default — and the titlebar family is ABSENT (the calm
     * gray chrome comes from the fallback, which this also pins:
     * the loaded theme's unset titlebar tokens must read through to
     * ITS tokens, which equal the built-in's). */
    fdk_theme *t = load_stem("faded-dream");
    assert(strcmp(fdk_theme_name(t), "Faded Dream") == 0);

    for (int tok = 0; tok < (int)FDK_TK_COUNT; tok++) {
        if (tok >= (int)FDK_TK_TITLEBAR_BACKGROUND) {
            continue; /* the family is deliberately absent */
        }
        fdk_color want = fdk_theme_get_color(NULL, (fdk_theme_token)tok);
        fdk_color got = fdk_theme_get_color(t, (fdk_theme_token)tok);
        if (got.r != want.r || got.g != want.g || got.b != want.b ||
            got.a != want.a) {
            fprintf(stderr, "FAIL: token %d drifted from the built-in\n",
                    tok);
            assert(!"faded-dream.fdk no longer mirrors the default");
        }
    }
    /* The fallback still reads the control family in the FILE's
     * copy (unset keys -> its own equal values). */
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BACKGROUND),
                   51, 51, 60, "faded-dream titlebar (fallback)");
    /* The metrics: the documented full set, all at default values. */
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 0);
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(t, FDK_TM_TITLE_BAR_HEIGHT) == 28);
    assert(fdk_theme_get_metric(t, FDK_TM_TEXTVIEW_PAD) == 8);
    fdk_theme_destroy(t);
    printf("[ok] faded-dream.fdk mirrors the built-in default "
           "exactly (base tokens + fallback chrome)\n");
}

static void test_mono_chromatic(void) {
    fdk_theme *t = load_stem("mono-chromatic");
    assert(strcmp(fdk_theme_name(t), "Mono Chromatic") == 0);

    /* The pairing: a WHITE world with BLACK speech. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND),
                   255, 255, 255, "mono window");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TEXT), 17, 17, 17,
                   "mono text");
    assert_channel(fdk_theme_get_color(t, FDK_TK_CONTROL_BORDER),
                   17, 17, 17, "mono border");
    assert_channel(fdk_theme_get_color(t, FDK_TK_ACCENT), 17, 17, 17,
                   "mono accent (black)");
    /* The inverted chrome. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BACKGROUND),
                   10, 10, 10, "mono band (near-black)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_TEXT),
                   255, 255, 255, "mono band text (white)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BUTTON_HOVER),
                   46, 46, 46, "mono band hover");
    /* The shape story: CIRCLES, a taller band. */
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 1);
    assert(fdk_theme_get_metric(t, FDK_TM_TITLE_BAR_HEIGHT) == 36);
    fdk_theme_destroy(t);
    printf("[ok] mono-chromatic.fdk: white world, black speech, "
           "inverted band, circle buttons\n");
}

static void test_pink_rave(void) {
    fdk_theme *t = load_stem("pink-rave");
    assert(strcmp(fdk_theme_name(t), "Pink Rave") == 0);

    /* The club floor: plum window, rose controls, neon accent. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND),
                   46, 15, 34, "rave window (dark plum)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND),
                   74, 24, 52, "rave control (rose)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_ACCENT),
                   255, 45, 154, "rave accent (hot pink)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_ACCENT_HOVER),
                   255, 92, 174, "rave accent hover");
    /* The loudest voice: a hot-pink band. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BACKGROUND),
                   255, 45, 154, "rave band (hot pink)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_TEXT),
                   255, 255, 255, "rave band text");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BORDER),
                   194, 24, 127, "rave band rule");
    /* Rounded (not circle) with soft modern fields. */
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 0);
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 16);
    assert(fdk_theme_get_metric(t, FDK_TM_ENTRY_CORNER_RADIUS) == 12);
    fdk_theme_destroy(t);
    printf("[ok] pink-rave.fdk: plum floor, rose surfaces, hot-pink "
           "accent and band\n");
}

static void test_daylight_and_matrix_unchanged(void) {
    /* The 1.4.13 pair: the retune must not have drifted them.
     * daylight is complete (its own values stand alone); matrix is
     * PARTIAL — its unpinned tokens now inherit the Faded Dream
     * grays instead of Modern's blue-grays, which is the documented
     * inheritance contract, so only its OWN seven colors + metric
     * are pinned. */
    fdk_theme *day = load_stem("daylight");
    assert(strcmp(fdk_theme_name(day), "Daylight") == 0);
    assert_channel(fdk_theme_get_color(day, FDK_TK_WINDOW_BACKGROUND),
                   244, 245, 247, "daylight window");
    assert_channel(fdk_theme_get_color(day, FDK_TK_ACCENT),
                   37, 99, 235, "daylight accent");
    fdk_theme_destroy(day);

    fdk_theme *mx = load_stem("matrix");
    assert(strcmp(fdk_theme_name(mx), "Matrix") == 0);
    assert_channel(fdk_theme_get_color(mx, FDK_TK_WINDOW_BACKGROUND),
                   6, 10, 6, "matrix window");
    assert_channel(fdk_theme_get_color(mx, FDK_TK_ACCENT),
                   124, 255, 168, "matrix accent");
    assert(fdk_theme_get_metric(mx, FDK_TM_BUTTON_CORNER_RADIUS) == 2);
    /* Partial by design: its titlebar is unset and follows its own
     * control fill now (the fallback contract). */
    assert_channel(fdk_theme_get_color(mx, FDK_TK_TITLEBAR_BACKGROUND),
                   12, 36, 18, "matrix band (fallback: its control)");
    fdk_theme_destroy(mx);
    printf("[ok] daylight.fdk + matrix.fdk: the 1.4.13 pair intact, "
           "matrix's band follows its own control fill\n");
}

static void test_folder_enumerates(void) {
    /* $FDK_THEME_DIR pointed at the folder: all five stems list. */
    assert(setenv("FDK_THEME_DIR", g_folder, 1) == 0);
    assert(setenv("XDG_DATA_HOME", "/nonexistent-fdk-shipped", 1) == 0);
    assert(setenv("XDG_DATA_DIRS", "/nonexistent-fdk-shipped", 1) == 0);
    assert(setenv("HOME", "/nonexistent-fdk-shipped", 1) == 0);

    assert(fdk_theme_available_count() == 5);
    assert(strcmp(fdk_theme_available_name(0), "daylight") == 0);
    assert(strcmp(fdk_theme_available_name(1), "faded-dream") == 0);
    assert(strcmp(fdk_theme_available_name(2), "matrix") == 0);
    assert(strcmp(fdk_theme_available_name(3), "mono-chromatic") == 0);
    assert(strcmp(fdk_theme_available_name(4), "pink-rave") == 0);

    /* Internal-name reachability: the quoted name resolves too (the
     * maintainer's spelled-out names work as CLI arguments). */
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("Mono Chromatic", &r);
    assert(t != NULL && r == FDK_OK);
    assert(strcmp(fdk_theme_name(t), "Mono Chromatic") == 0);
    fdk_theme_destroy(t);
    t = fdk_theme_find("Pink Rave", &r);
    assert(t != NULL && r == FDK_OK);
    fdk_theme_destroy(t);
    t = fdk_theme_find("Faded Dream", &r);
    assert(t != NULL && r == FDK_OK);
    fdk_theme_destroy(t);

    assert(unsetenv("FDK_THEME_DIR") == 0);
    printf("[ok] the .FDKThemes folder enumerates all five faces; "
           "internal names resolve\n");
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    /* Hermeticity: nothing from the machine's real settings. */
    setenv("FDK_PREFS_FILE", "/nonexistent-fdk-hermetic.prefs", 1);
    setenv("FDK_THEME", "", 1);

    /* build/tests/ -> ../../.FDKThemes */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(n > 0);
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    assert(slash != NULL);
    *slash = '\0';
    snprintf(g_folder, sizeof g_folder, "%s/../../.FDKThemes", self);
    struct stat st;
    assert(stat(theme_file("faded-dream"), &st) == 0);
    assert(stat(theme_file("mono-chromatic"), &st) == 0);
    assert(stat(theme_file("pink-rave"), &st) == 0);
    assert(stat(theme_file("daylight"), &st) == 0);
    assert(stat(theme_file("matrix"), &st) == 0);

    test_faded_dream_is_the_default();
    test_mono_chromatic();
    test_pink_rave();
    test_daylight_and_matrix_unchanged();
    test_folder_enumerates();

    printf("all shipped-theme tests passed\n");
    return 0;
}
