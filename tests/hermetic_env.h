/*
 * hermetic_env.h — the suite's desktop-independence pin (1.1.0)
 *
 * WHY THIS EXISTS: the lazy resolvers (UI metrics, the theme boot,
 * the color-scheme read) answer from the ENVIRONMENT the first time
 * anything asks — and on a developer's real desktop that environment
 * is full of answers: ~/.config/fdk.prefs carrying theme.name /
 * ui.font_size / ui.scale, an exported $FDK_THEME, a dark color
 * scheme, XSETTINGS Gtk/FontName. A test run on such a machine is a
 * DIFFERENT test run than CI's, and it reds on assertions CI never
 * saw (the owner's test_progress: the global settings file resolved
 * a real theme face, the accent stopped matching is_accent(), and
 * "filled50 >= 96" failed with zero diagnostics). The suite must be
 * hermetic: same answers on CI, on the owner's desktop, in 2030.
 *
 * THE PIN (a constructor — it runs before main, therefore before
 * any lazy first-getter can fire, which a main()-time call cannot
 * promise: tests query palettes from helper functions long before
 * any explicit setup line, and many create no context at all):
 *
 *   FDK_UI_FONT_SIZE=14   the catalog's REFERENCE size — every
 *                         fdk__ui_derived_px(N) resolves to exactly
 *                         N (the arithmetic's 14.0 divisor)
 *   FDK_UI_SCALE=1        no desktop text-scaling leakage
 *   FDK_COLOR_SCHEME=none no dark/light variant flipping (the
 *                         getter's override wins even when the
 *                         XSETTINGS poll runs on a real display)
 *   $FDK_THEME unset      no developer-shell theme leaking in
 *   $FDK_PREFS_FILE unset no one-file override leaking in
 *   XDG_CONFIG_HOME -> build/tests/hermetic-home/config
 *                         an empty directory (created best-effort;
 *                         left nonexistent is equally hermetic) —
 *                         the owner's real fdk.prefs / app prefs
 *                         cannot leak a theme or ui.* keys in.
 *                         Tests with their own fixtures setenv their
 *                         own XDG_CONFIG_HOME in main(); theirs
 *                         lands later and wins. HOME is
 *                         deliberately NOT sandboxed: fontconfig
 *                         discovery is part of what the tests
 *                         exercise, and XDG_CONFIG_HOME set means
 *                         the HOME/.config prefs fallback never
 *                         runs anyway.
 *
 *   The env pins are also the "no portal influence" guarantee in
 *   disguise: with the override set, ui_metrics' resolve() never
 *   consults the settings store, XSETTINGS, or the portal, and the
 *   color-scheme getter returns NONE without polling — the desktop
 *   has no channel left to speak through.
 *
 * TESTS THAT TEST THE RESOLUTION ITSELF opt out of the metrics pin
 * (they are the subject, not a consumer):
 *
 *     #define FDK_TEST_NO_METRICS_PIN
 *     #include "hermetic_env.h"
 *
 * …and keep only the sandbox half (the XDG_CONFIG_HOME isolation,
 * FDK_THEME / FDK_PREFS_FILE unsets), which is exactly what a
 * resolution test needs to assert the DEFAULTS deterministically.
 *
 * THE CHECK MACROS: plain assert() prints the expression, never the
 * values — a red like `filled50 >= 96 && filled50 <= 104' says
 * nothing about WHICH channel moved or by how much. Every CHECK_*
 * below prints the actual value and the expected one (point,
 * bound, or range) before aborting, with file:line and a `what'
 * label naming the measurement. They abort() unconditionally —
 * never compiled out by NDEBUG; the suite never defines it, but a
 * check that can silently vanish is not a check.
 *
 * This header is header-only and dependency-free (stdlib/stdio); it
 * is include-order agnostic and safe to include twice.
 */
#ifndef FDK_TEST_HERMETIC_ENV_H
#define FDK_TEST_HERMETIC_ENV_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ---- the pin ----------------------------------------------------------- */

__attribute__((constructor)) static void fdk_test_hermetic_pin(void) {
#ifndef FDK_TEST_NO_METRICS_PIN
    /* TASK C's reference size: derived metrics resolve exactly. */
    (void)setenv("FDK_UI_FONT_SIZE", "14", 1);
    (void)setenv("FDK_UI_SCALE", "1", 1);
    /* The neutral scheme: no variant faces, no desktop following. */
    (void)setenv("FDK_COLOR_SCHEME", "none", 1);
#endif
    /* The settings store points at a directory that never holds a
     * file: mkdir best-effort (a read-only CWD leaves it
     * nonexistent — equally silent), never fatal. */
    (void)mkdir("build/tests/hermetic-home", 0755);
    (void)mkdir("build/tests/hermetic-home/config", 0755);
    (void)setenv("XDG_CONFIG_HOME", "build/tests/hermetic-home/config",
                 1);
    /* No theme / prefs-file overrides leaking in from the shell. */
    (void)unsetenv("FDK_THEME");
    (void)unsetenv("FDK_PREFS_FILE");
}

/* ---- the CHECK macros ----------------------------------------------------- */
/*
 * what: a short label naming the measurement (printed on failure).
 * All integer values promote to long long, so mixed int/size_t/
 * fdk_i32 comparisons print honestly. The trailing-underscore
 * locals (a_, e_, lo_, hi_) are chosen to not collide with test
 * locals (suite convention: plain names).
 */

#define CHECK_EQ(actual, expected, what)                                 \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(expected);                            \
        if (a_ != e_) {                                                  \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected %lld\n",                          \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_NE(actual, expected, what)                                 \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(expected);                            \
        if (a_ == e_) {                                                  \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected anything but %lld\n",             \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_GE(actual, min, what)                                      \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(min);                                 \
        if (a_ < e_) {                                                   \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected >= %lld\n",                       \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_GT(actual, min, what)                                      \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(min);                                 \
        if (a_ <= e_) {                                                  \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected > %lld\n",                        \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_LE(actual, max, what)                                      \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(max);                                 \
        if (a_ > e_) {                                                   \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected <= %lld\n",                       \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_LT(actual, max, what)                                      \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long e_ = (long long)(max);                                 \
        if (a_ >= e_) {                                                  \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected < %lld\n",                        \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

/* The [lo, hi] form — the pixel-count class (a ~100 of 200 fill
 * with AA edges is a RANGE, not an equality). */
#define CHECK_RANGE(actual, lo, hi, what)                                \
    do {                                                                 \
        long long a_ = (long long)(actual);                              \
        long long lo_ = (long long)(lo);                                 \
        long long hi_ = (long long)(hi);                                 \
        if (a_ < lo_ || a_ > hi_) {                                      \
            fprintf(stderr, "%s:%d: FAIL %s: actual %lld, "             \
                            "expected [%lld, %lld]\n",                  \
                    __FILE__, __LINE__, (what), a_, lo_, hi_);           \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

/* Floats: exact equality (the suite's convention for f32 fields)
 * and the tolerance form for arithmetic drift. */
#define CHECK_EQ_F(actual, expected, what)                               \
    do {                                                                 \
        double a_ = (double)(actual);                                    \
        double e_ = (double)(expected);                                  \
        if (a_ != e_) {                                                  \
            fprintf(stderr, "%s:%d: FAIL %s: actual %.9g, "             \
                            "expected %.9g\n",                          \
                    __FILE__, __LINE__, (what), a_, e_);                 \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, what)                          \
    do {                                                                 \
        double a_ = (double)(actual);                                    \
        double e_ = (double)(expected);                                  \
        double t_ = (double)(tol);                                       \
        double d_ = a_ - e_;                                             \
        if (d_ < 0.0) {                                                  \
            d_ = -d_;                                                    \
        }                                                                \
        if (!(d_ <= t_)) {                                               \
            fprintf(stderr, "%s:%d: FAIL %s: actual %.9g, "             \
                            "expected %.9g +/- %.9g\n",                 \
                    __FILE__, __LINE__, (what), a_, e_, t_);             \
            fflush(stderr);                                              \
            abort();                                                     \
        }                                                                \
    } while (0)

#endif /* FDK_TEST_HERMETIC_ENV_H */
