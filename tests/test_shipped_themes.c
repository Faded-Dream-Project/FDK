/*
 * test_shipped_themes.c — the shipped face + the engine fixture
 * corpus (1.4.16; the 1.0.0 alignment, 2026-10-12)
 *
 * Two contracts in one suite:
 *
 * THE SHIPPED PRODUCT: the source tree's .FDKThemes/ folder
 * carries EXACTLY ONE face — faded-dream.fdk, the packaged copy
 * of the BUILT-IN default. The owner's 1.0.0 directive is
 * structural: `make install` pins that single file (the
 * SHIPPED_THEMES line in the Makefile), and this suite pins the
 * folder itself — a stray .fdk dropped into .FDKThemes/ fails
 * here long before it could ride into a release. The next faces
 * are .FDKThemes drop-ins the USER installs ($HOME/.FDKThemes,
 * the discovery slot), never package content.
 *
 * THE ENGINE FIXTURE CORPUS: the nine faces FDK grew between
 * 1.4.13 and 1.4.17, kept in tests/data/faces/ as parser and
 * read-through fixtures — never installed, never shipped. Each
 * is a real authored file exercising a different region of the
 * grammar and the read-through rules:
 *
 *   faded-dream      SHIPPED — every base token byte-equal to
 *                    the built-in, no titlebar keys (the calm
 *                    gray chrome is the fallback)
 *   mono-chromatic   black-and-white discipline; the titlebar
 *                    family opted in (near-black band, white
 *                    text); buttons are CIRCLES (shape = 1)
 *   pink-rave        the all-pink face; hot-pink accent AND a
 *                    hot-pink titlebar band over a plum window
 *   amoled-void      true-black world, electric-violet family,
 *                    SQUARE buttons (shape = 2)
 *   catppuccin pair  the official v0.4.5 palettes, mocha + latte
 *   nordic-frost     Nord's official sixteen, nord8 cyan accent
 *   solar-flare      midnight navy under amber energy
 *   daylight         the complete light theme (1.4.13) — its own
 *                    values stand alone
 *   matrix           the partial green-on-black theme (1.4.13) —
 *                    still partial (seven colors, one metric)
 *
 * Plus the enumeration-level facts: $FDK_THEME_DIR pointed at
 * the shipped folder lists exactly the one stem; pointed at the
 * corpus it lists all nine, alphabetically.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_theme.h"

#include "theme/theme_internal.h" /* fdk__theme_scan_reset_for_tests: the
                                     * discovery scan caches per environment */

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

static char g_folder[1024]; /* abs path to the repo's .FDKThemes/ */
static char g_corpus[1024]; /* abs path to tests/data/faces/ */

static char g_path[1100];
static const char *theme_file(const char *dir, const char *stem) {
    snprintf(g_path, sizeof g_path, "%s/%s.fdk", dir, stem);
    return g_path;
}

static fdk_theme *load_stem(const char *dir, const char *stem) {
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_load(theme_file(dir, stem), &r);
    if (t == NULL) {
        fprintf(stderr, "FAIL: cannot load %s.fdk (%s)\n", stem,
                fdk_result_to_string(r));
        assert(!"theme fixture failed to load");
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
        assert(!"theme fixture color drifted");
    }
}

/* ---- the shipped face -------------------------------------------------- */

static void test_faded_dream_is_the_default(void) {
    /* The contract file: every BASE token byte-equal to the
     * built-in, so `fdk-theme set faded-dream` is pixel-identical
     * to the default — and the titlebar family is ABSENT (the calm
     * gray chrome comes from the fallback, which this also pins:
     * the loaded theme's unset titlebar tokens must read through to
     * ITS tokens, which equal the built-in's). */
    fdk_theme *t = load_stem(g_folder, "faded-dream");
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
    printf("[ok] faded-dream.fdk (the one shipped face) mirrors the "
           "built-in default exactly (base tokens + fallback chrome)\n");
}

static void test_folder_ships_exactly_one(void) {
    /* The 1.0.0 alignment, pinned structurally: opendir the
     * .FDKThemes/ folder itself and count .fdk entries — exactly
     * one, and it is faded-dream.fdk. A second face (a stray
     * drop, a half-finished experiment) fails HERE, in the test
     * battery, instead of riding into a release through a
     * wildcard install. */
    DIR *d = opendir(g_folder);
    assert(d != NULL);
    int count = 0;
    char only[256] = "";
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (n < 5 || strcmp(ent->d_name + n - 4, ".fdk") != 0) {
            continue; /* not a theme file */
        }
        count++;
        snprintf(only, sizeof only, "%s", ent->d_name);
    }
    closedir(d);
    if (count != 1 || strcmp(only, "faded-dream.fdk") != 0) {
        fprintf(stderr,
                "FAIL: .FDKThemes/ carries %d .fdk file(s) (first: %s) "
                "— the 1.0.0 contract is EXACTLY ONE: "
                "faded-dream.fdk\n",
                count, count > 0 ? only : "(none)");
        assert(!"the shipped folder re-bloated past the one face");
    }

    /* Enumeration agrees: pointed at the shipped folder with a
     * clean environment, exactly the one stem lists. */
    fdk__theme_scan_reset_for_tests();
    assert(setenv("FDK_THEME_DIR", g_folder, 1) == 0);
    assert(setenv("XDG_DATA_HOME", "/nonexistent-fdk-shipped", 1) == 0);
    assert(setenv("XDG_DATA_DIRS", "/nonexistent-fdk-shipped", 1) == 0);
    assert(setenv("HOME", "/nonexistent-fdk-shipped", 1) == 0);
    assert(fdk_theme_available_count() == 1);
    assert(strcmp(fdk_theme_available_name(0), "faded-dream") == 0);
    assert(strcmp(fdk_theme_available_path(0),
                  theme_file(g_folder, "faded-dream")) == 0);

    /* The quoted name of the one shipped face resolves. */
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("Faded Dream", &r);
    assert(t != NULL && r == FDK_OK);
    assert(strcmp(fdk_theme_name(t), "Faded Dream") == 0);
    fdk_theme_destroy(t);

    assert(unsetenv("FDK_THEME_DIR") == 0);
    printf("[ok] the .FDKThemes folder ships EXACTLY ONE face "
           "(faded-dream.fdk) — folder scan, enumeration, and "
           "quoted-name lookup all agree\n");
}

/* ---- the engine fixture corpus ---------------------------------------- */

static void test_mono_chromatic(void) {
    fdk_theme *t = load_stem(g_corpus, "mono-chromatic");
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
    printf("[ok] fixture mono-chromatic.fdk: white world, black "
           "speech, inverted band, circle buttons\n");
}

static void test_pink_rave(void) {
    fdk_theme *t = load_stem(g_corpus, "pink-rave");
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
    printf("[ok] fixture pink-rave.fdk: plum floor, rose surfaces, "
           "hot-pink accent and band\n");
}

static void test_amoled_void(void) {
    fdk_theme *t = load_stem(g_corpus, "amoled-void");
    assert(strcmp(fdk_theme_name(t), "AMOLED Void") == 0);

    /* The pure-black world on a whisper-violet ramp. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND),
                   0, 0, 0, "void window (true black)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND),
                   14, 13, 18, "void control (whisper violet)");
    assert_channel(fdk_theme_get_color(t, FDK_TK_ACCENT),
                   124, 58, 237, "void accent (electric violet)");
    /* The hairline band with the violet rule. */
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BACKGROUND),
                   5, 5, 10, "void band");
    assert_channel(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BORDER),
                   91, 54, 184, "void band rule (violet)");
    /* Square corners, slim band. */
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 2);
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 0);
    assert(fdk_theme_get_metric(t, FDK_TM_TITLE_BAR_HEIGHT) == 32);
    fdk_theme_destroy(t);
    printf("[ok] fixture amoled-void.fdk: true-black world, "
           "electric-violet family, square corners\n");
}

static void test_catppuccin_pair(void) {
    /* Mocha: the official v0.4.5 palette — crust chrome, surface0
     * controls, mauve accent. */
    fdk_theme *mo = load_stem(g_corpus, "catppuccin-mocha");
    assert(strcmp(fdk_theme_name(mo), "Catppuccin Mocha") == 0);
    assert_channel(fdk_theme_get_color(mo, FDK_TK_WINDOW_BACKGROUND),
                   30, 30, 46, "mocha window (base #1E1E2E)");
    assert_channel(fdk_theme_get_color(mo, FDK_TK_CONTROL_BACKGROUND),
                   49, 50, 68, "mocha control (surface0)");
    assert_channel(fdk_theme_get_color(mo, FDK_TK_ACCENT),
                   203, 166, 247, "mocha accent (mauve)");
    assert_channel(fdk_theme_get_color(mo, FDK_TK_TITLEBAR_BACKGROUND),
                   17, 17, 27, "mocha band (crust)");
    assert_channel(fdk_theme_get_color(mo, FDK_TK_TITLEBAR_BORDER),
                   49, 50, 68, "mocha band rule (surface0)");
    assert(fdk_theme_get_metric(mo, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    fdk_theme_destroy(mo);

    /* Latte: the light twin — mantle chrome, base controls, the
     * same mauve accent voice. */
    fdk_theme *la = load_stem(g_corpus, "catppuccin-latte");
    assert(strcmp(fdk_theme_name(la), "Catppuccin Latte") == 0);
    assert_channel(fdk_theme_get_color(la, FDK_TK_WINDOW_BACKGROUND),
                   230, 233, 239, "latte window (#E6E9EF)");
    assert_channel(fdk_theme_get_color(la, FDK_TK_CONTROL_BACKGROUND),
                   239, 241, 245, "latte control (base)");
    assert_channel(fdk_theme_get_color(la, FDK_TK_ACCENT),
                   136, 57, 239, "latte accent (mauve)");
    assert_channel(fdk_theme_get_color(la, FDK_TK_TITLEBAR_BACKGROUND),
                   220, 224, 232, "latte band (mantle)");
    assert_channel(fdk_theme_get_color(la, FDK_TK_TITLEBAR_BORDER),
                   188, 192, 204, "latte band rule");
    assert(fdk_theme_get_metric(la, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    fdk_theme_destroy(la);
    printf("[ok] fixtures catppuccin-mocha.fdk + catppuccin-latte.fdk: "
           "the official pair, dark crust chrome and light mantle "
           "chrome\n");
}

static void test_nordic_frost_and_solar_flare(void) {
    /* Nord: the official sixteen, nord8 cyan as the only voice. */
    fdk_theme *nf = load_stem(g_corpus, "nordic-frost");
    assert(strcmp(fdk_theme_name(nf), "Nordic Frost") == 0);
    assert_channel(fdk_theme_get_color(nf, FDK_TK_WINDOW_BACKGROUND),
                   46, 52, 64, "nord window (#2E3440)");
    assert_channel(fdk_theme_get_color(nf, FDK_TK_CONTROL_BACKGROUND),
                   59, 66, 82, "nord control (#3B4252)");
    assert_channel(fdk_theme_get_color(nf, FDK_TK_ACCENT),
                   136, 192, 208, "nord accent (nord8 frost cyan)");
    assert_channel(fdk_theme_get_color(nf, FDK_TK_TITLEBAR_BORDER),
                   67, 76, 94, "nord band rule (#434C5E)");
    assert(fdk_theme_get_metric(nf, FDK_TM_BUTTON_CORNER_RADIUS) == 4);
    assert(fdk_theme_get_metric(nf, FDK_TM_TITLE_BAR_HEIGHT) == 28);
    fdk_theme_destroy(nf);

    /* Solar Flare: midnight navy under amber energy. */
    fdk_theme *sf = load_stem(g_corpus, "solar-flare");
    assert(strcmp(fdk_theme_name(sf), "Solar Flare") == 0);
    assert_channel(fdk_theme_get_color(sf, FDK_TK_WINDOW_BACKGROUND),
                   11, 16, 32, "flare window (#0B1020)");
    assert_channel(fdk_theme_get_color(sf, FDK_TK_CONTROL_BACKGROUND),
                   21, 29, 49, "flare control (#151D31)");
    assert_channel(fdk_theme_get_color(sf, FDK_TK_ACCENT),
                   255, 176, 32, "flare accent (amber #FFB020)");
    assert_channel(fdk_theme_get_color(sf, FDK_TK_TITLEBAR_BORDER),
                   255, 176, 32, "flare band rule (solid amber)");
    assert(fdk_theme_get_metric(sf, FDK_TM_BUTTON_CORNER_RADIUS) == 10);
    fdk_theme_destroy(sf);
    printf("[ok] fixtures nordic-frost.fdk + solar-flare.fdk: Nord's "
           "cyan restraint and the amber flare\n");
}

static void test_daylight_and_matrix_unchanged(void) {
    /* The 1.4.13 pair: the retune must not have drifted them.
     * daylight is complete (its own values stand alone); matrix is
     * PARTIAL — its unpinned tokens inherit the Faded Dream
     * grays instead of Modern's blue-grays, which is the documented
     * inheritance contract, so only its OWN seven colors + metric
     * are pinned. */
    fdk_theme *day = load_stem(g_corpus, "daylight");
    assert(strcmp(fdk_theme_name(day), "Daylight") == 0);
    assert_channel(fdk_theme_get_color(day, FDK_TK_WINDOW_BACKGROUND),
                   244, 245, 247, "daylight window");
    assert_channel(fdk_theme_get_color(day, FDK_TK_ACCENT),
                   37, 99, 235, "daylight accent");
    fdk_theme_destroy(day);

    fdk_theme *mx = load_stem(g_corpus, "matrix");
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
    printf("[ok] fixtures daylight.fdk + matrix.fdk: the 1.4.13 pair "
           "intact, matrix's band follows its own control fill\n");
}

static void test_corpus_enumerates(void) {
    /* $FDK_THEME_DIR pointed at the corpus: all nine fixture stems
     * list, alphabetically — the discovery layer's enumeration
     * contract over a multi-face folder (the shipped folder's
     * one-stem case is pinned above; this keeps the many-stem
     * case covered too). */
    assert(setenv("FDK_THEME_DIR", g_corpus, 1) == 0);
    fdk__theme_scan_reset_for_tests();
    assert(setenv("XDG_DATA_HOME", "/nonexistent-fdk-corpus", 1) == 0);
    assert(setenv("XDG_DATA_DIRS", "/nonexistent-fdk-corpus", 1) == 0);
    assert(setenv("HOME", "/nonexistent-fdk-corpus", 1) == 0);

    assert(fdk_theme_available_count() == 9);
    assert(strcmp(fdk_theme_available_name(0), "amoled-void") == 0);
    assert(strcmp(fdk_theme_available_name(1), "catppuccin-latte") == 0);
    assert(strcmp(fdk_theme_available_name(2), "catppuccin-mocha") == 0);
    assert(strcmp(fdk_theme_available_name(3), "daylight") == 0);
    assert(strcmp(fdk_theme_available_name(4), "matrix") == 0);
    assert(strcmp(fdk_theme_available_name(5), "mono-chromatic") == 0);
    assert(strcmp(fdk_theme_available_name(6), "nordic-frost") == 0);
    assert(strcmp(fdk_theme_available_name(7), "pink-rave") == 0);
    assert(strcmp(fdk_theme_available_name(8), "solar-flare") == 0);

    /* Internal-name reachability: the quoted names resolve too (the
     * maintainer's spelled-out names work as CLI arguments). */
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("Mono Chromatic", &r);
    assert(t != NULL && r == FDK_OK);
    assert(strcmp(fdk_theme_name(t), "Mono Chromatic") == 0);
    fdk_theme_destroy(t);
    t = fdk_theme_find("Pink Rave", &r);
    assert(t != NULL && r == FDK_OK);
    fdk_theme_destroy(t);
    t = fdk_theme_find("Catppuccin Mocha", &r);
    assert(t != NULL && r == FDK_OK);
    fdk_theme_destroy(t);
    t = fdk_theme_find("Solar Flare", &r);
    assert(t != NULL && r == FDK_OK);
    fdk_theme_destroy(t);

    assert(unsetenv("FDK_THEME_DIR") == 0);
    printf("[ok] the fixture corpus enumerates all nine faces; "
           "internal names resolve\n");
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    /* Hermeticity: nothing from the machine's real settings. */
    setenv("FDK_PREFS_FILE", "/nonexistent-fdk-hermetic.prefs", 1);
    setenv("FDK_THEME", "", 1);

    /* build/tests/ -> ../../.FDKThemes and ../../tests/data/faces */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(n > 0);
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    assert(slash != NULL);
    *slash = '\0';
    snprintf(g_folder, sizeof g_folder, "%s/../../.FDKThemes", self);
    snprintf(g_corpus, sizeof g_corpus, "%s/../../tests/data/faces", self);
    struct stat st;
    assert(stat(theme_file(g_folder, "faded-dream"), &st) == 0);
    assert(stat(theme_file(g_corpus, "mono-chromatic"), &st) == 0);
    assert(stat(theme_file(g_corpus, "pink-rave"), &st) == 0);
    assert(stat(theme_file(g_corpus, "daylight"), &st) == 0);
    assert(stat(theme_file(g_corpus, "matrix"), &st) == 0);
    assert(stat(theme_file(g_corpus, "amoled-void"), &st) == 0);
    assert(stat(theme_file(g_corpus, "catppuccin-latte"), &st) == 0);
    assert(stat(theme_file(g_corpus, "catppuccin-mocha"), &st) == 0);
    assert(stat(theme_file(g_corpus, "nordic-frost"), &st) == 0);
    assert(stat(theme_file(g_corpus, "solar-flare"), &st) == 0);

    test_faded_dream_is_the_default();
    test_folder_ships_exactly_one();
    test_mono_chromatic();
    test_pink_rave();
    test_amoled_void();
    test_catppuccin_pair();
    test_nordic_frost_and_solar_flare();
    test_daylight_and_matrix_unchanged();
    test_corpus_enumerates();

    printf("all shipped-theme tests passed\n");
    return 0;
}
