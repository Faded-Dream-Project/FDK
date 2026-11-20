/*
 * test_high_contrast.c — the high-contrast accessibility theme
 * (ACCESSIBILITY-AUDIT.md H2, final third; make test, headless).
 *
 * Three groups:
 *   1. the preference's env parse (hermetic, the color-scheme and
 *      reduced-motion posture);
 *   2. the sibling-face selection: with the preference on, the
 *      theme engine prefers faded-dream-highcontrast; with it off,
 *      the base face stays (and the default is NEVER high-contrast);
 *   3. the WCAG gate: every text/background token pair of the
 *      high-contrast face clears 7:1 (AAA), and a REPORT (printf,
 *      not assert) of the pairs below 4.5:1 in the OTHER shipped
 *      faces — the honest accounting of the calm default's
 *      restfulness trade.
 */

#include "hermetic_env.h"
#include "fdk/fdk_theme.h"

#include "theme/theme_internal.h" /* the reset seams */

#include <sys/stat.h>
#include <unistd.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The staged faces dir: $FDK_THEME_DIR points here so discovery
 * finds the SHIPPED faces (base + the new sibling) exactly as an
 * installed tree would, hermetically. */
static char g_theme_dir[256];

/* ---- WCAG relative luminance / contrast ------------------------------- */

static double srgb_to_lin(double v) {
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double luminance(fdk_color c) {
    double r = srgb_to_lin((double)c.r);
    double g = srgb_to_lin((double)c.g);
    double b = srgb_to_lin((double)c.b);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

static double contrast_ratio(fdk_color a, fdk_color b) {
    double l1 = luminance(a);
    double l2 = luminance(b);
    if (l1 < l2) {
        double t = l1;
        l1 = l2;
        l2 = t;
    }
    return (l1 + 0.05) / (l2 + 0.05);
}

/* ---- group 1: the env parse -------------------------------------------- */

static void test_env_override(void) {
    fdk__high_contrast_reset_for_tests();

    setenv("FDK_HIGH_CONTRAST", "1", 1);
    fdk__high_contrast_reset_for_tests();
    assert(fdk_system_prefers_high_contrast());
    unsetenv("FDK_HIGH_CONTRAST");

    setenv("FDK_HIGH_CONTRAST", "high", 1);
    fdk__high_contrast_reset_for_tests();
    assert(fdk_system_prefers_high_contrast());
    unsetenv("FDK_HIGH_CONTRAST");

    setenv("FDK_HIGH_CONTRAST", "0", 1);
    fdk__high_contrast_reset_for_tests();
    assert(!fdk_system_prefers_high_contrast());
    unsetenv("FDK_HIGH_CONTRAST");

    setenv("FDK_HIGH_CONTRAST", "none", 1);
    fdk__high_contrast_reset_for_tests();
    assert(!fdk_system_prefers_high_contrast());
    unsetenv("FDK_HIGH_CONTRAST");

    setenv("FDK_HIGH_CONTRAST", "garbage", 1);
    fdk__high_contrast_reset_for_tests();
    assert(!fdk_system_prefers_high_contrast());
    unsetenv("FDK_HIGH_CONTRAST");

    printf("[ok] high-contrast env override parsing\n");
}

/* ---- group 2: the sibling-face selection -------------------------------- */

/* The hermetic theme dir: hermetic_env.h builds one per test
 * process with the SHIPPED faces copied in (the suite's standard
 * posture — see test_shipped_themes.c); fdk_theme_find resolves
 * through it. */
static void test_face_selection(void) {
    /* OFF (the default): the resolved default theme must NOT be
     * the high-contrast face. */
    fdk__high_contrast_reset_for_tests();
    fdk__theme_boot_reset_for_tests(); /* re-runs the boot pass where the variant passes live */
    const fdk_theme *t = fdk_theme_get_default();
    assert(t != NULL);
    assert(strstr(fdk_theme_name(t), "High Contrast") == NULL);
    fdk_color bg = fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND);
    assert(bg.r < 0.5 && bg.g < 0.5 && bg.b < 0.5); /* the calm dark */

    /* ON through the env override: re-resolution flips the face. */
    setenv("FDK_HIGH_CONTRAST", "1", 1);
    fdk__high_contrast_reset_for_tests();
    fdk__theme_boot_reset_for_tests(); /* re-runs the boot pass where the variant passes live */
    t = fdk_theme_get_default();
    assert(t != NULL);
    const char *n = fdk_theme_name(t);
    printf("     (resolved face under the override: %s)\n", n);
    assert(strstr(n, "High Contrast") != NULL);
    bg = fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND);
    /* black field, white text */
    assert(bg.r < 0.05 && bg.g < 0.05 && bg.b < 0.05);
    fdk_color tx = fdk_theme_get_color(t, FDK_TK_TEXT);
    assert(tx.r > 0.9 && tx.g > 0.9 && tx.b > 0.9);

    unsetenv("FDK_HIGH_CONTRAST");
    fdk__high_contrast_reset_for_tests();
    fdk__theme_boot_reset_for_tests(); /* re-runs the boot pass where the variant passes live */
    printf("[ok] high-contrast face selection: off = base, on = the "
           "sibling; never the default\n");
}

/* ---- group 3: the WCAG gate --------------------------------------------- */

/* The text/background pairs the token set defines. Tokens with an
 * explicit *_text sibling pair with it; the general text tokens
 * pair with the surfaces they sit on. */
struct pair_spec {
    const char *label;
    fdk_theme_token fg;
    fdk_theme_token bg;
};
static const struct pair_spec k_pairs[] = {
    {"text/window_background", FDK_TK_TEXT, FDK_TK_WINDOW_BACKGROUND},
    {"text/control_background", FDK_TK_TEXT, FDK_TK_CONTROL_BACKGROUND},
    {"text_disabled/control_background_disabled",
     FDK_TK_TEXT_DISABLED, FDK_TK_CONTROL_BACKGROUND_DISABLED},
    {"tooltip_text/tooltip_background",
     FDK_TK_TOOLTIP_TEXT, FDK_TK_TOOLTIP_BACKGROUND},
};

static void check_face(const fdk_theme *t, const char *face,
                       double floor, int assert_it) {
    int checked = 0;
    for (size_t i = 0; i < sizeof k_pairs / sizeof k_pairs[0]; i++) {
        fdk_color fg = fdk_theme_get_color(t, k_pairs[i].fg);
        fdk_color bg = fdk_theme_get_color(t, k_pairs[i].bg);
        double r = contrast_ratio(fg, bg);
        checked++;
        if (assert_it) {
            assert(r >= floor);
        } else if (r < floor) {
            printf("     CONTRAST REPORT %s: %s = %.2f:1 (below "
                   "%.1f:1)\n", face, k_pairs[i].label, r, floor);
        }
    }
    printf("     %s: %d text/background pairs checked against "
           "%.1f:1\n", face, checked, floor);
}

static void test_wcag_gate(void) {
    /* The high-contrast face: AAA (7:1) is a HARD assertion. */
    setenv("FDK_HIGH_CONTRAST", "1", 1);
    fdk__high_contrast_reset_for_tests();
    fdk__theme_boot_reset_for_tests(); /* re-runs the boot pass where the variant passes live */
    const fdk_theme *hc = fdk_theme_get_default();
    assert(hc != NULL);
    assert(strstr(fdk_theme_name(hc), "High Contrast") != NULL);
    check_face(hc, "faded-dream-highcontrast", 7.0, 1);
    unsetenv("FDK_HIGH_CONTRAST");
    fdk__high_contrast_reset_for_tests();
    fdk__theme_boot_reset_for_tests(); /* re-runs the boot pass where the variant passes live */

    /* The other shipped face(s): the honest report — pairs below
     * 4.5:1 are PRINTED, not asserted (the calm default's trade
     * is a design decision, documented, not a defect). */
    const fdk_theme *base = fdk_theme_get_default();
    assert(base != NULL);
    check_face(base, "faded-dream (the default)", 4.5, 0);

    printf("[ok] WCAG gate: high-contrast face AAA (>=7:1) pinned; "
           "other faces' sub-4.5:1 pairs reported above\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Stage the two shipped faces into the hermetic theme dir. */
    /* $FDK_THEME_DIR must be ABSOLUTE (discover.c ignores a
     * relative path with a warning) — build it from getcwd. */
    char cwd[512];
    assert(getcwd(cwd, sizeof cwd) != NULL);
    int n = snprintf(g_theme_dir, sizeof g_theme_dir,
                     "%s/build/tests/high-contrast-faces", cwd);
    assert(n > 0 && (size_t)n < sizeof g_theme_dir);
    mkdir("build/tests", 0755);
    mkdir(g_theme_dir, 0755);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "cp .FDKThemes/faded-dream.fdk "
             ".FDKThemes/faded-dream-highcontrast.fdk %s/", g_theme_dir);
    if (system(cmd) != 0) {
        printf("[skip] high-contrast: faces not present\n");
        return 0;
    }
    assert(setenv("FDK_THEME_DIR", g_theme_dir, 1) == 0);

    test_env_override();
    test_face_selection();
    test_wcag_gate();
    printf("\nall high-contrast tests passed\n");
    return 0;
}
