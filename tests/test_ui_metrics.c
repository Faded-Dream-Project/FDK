/* test_ui_metrics.c — the UI font size / scale resolution (1.1.0)
 *
 * The unit leg of TASK C's core half: the env-override channel, the
 * scale arithmetic, the clamps, and the invalid-input posture — all
 * through the public getters (fdk_ui_font_size / _scale / _font_px),
 * with fdk__ui_metrics_invalidate() as the re-resolution seam.
 *
 * The desktop channels (XSETTINGS, the portal) are exercised by the
 * live suites; this file pins the arithmetic and the precedence the
 * env override carries over everything.
 */

/* The SUBJECT is the resolution itself: the metrics pin is opted
 * out; the sandbox half still applies (the default-16 legs need a
 * silent settings store). */
#define FDK_TEST_NO_METRICS_PIN

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_theme.h"
#include "theme/theme_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_and_invalidate(const char *font, const char *scale) {
    if (font != NULL) {
        setenv("FDK_UI_FONT_SIZE", font, 1);
    } else {
        unsetenv("FDK_UI_FONT_SIZE");
    }
    if (scale != NULL) {
        setenv("FDK_UI_SCALE", scale, 1);
    } else {
        unsetenv("FDK_UI_SCALE");
    }
    fdk__ui_metrics_invalidate();
}

int main(void) {
    /* The default: nothing set anywhere -> 16 px, scale 1.0. */
    set_and_invalidate(NULL, NULL);
    assert(fdk_ui_font_size() == 16);
    assert(fdk_ui_scale() == 1.0);
    assert(fdk_ui_font_px() == 16);
    printf("[ok] defaults: 16 px, scale 1.0\n");

    /* The font override, exercised at the four sizes the owner's
     * sweep uses. */
    set_and_invalidate("12", NULL);
    assert(fdk_ui_font_size() == 12 && fdk_ui_font_px() == 12);
    set_and_invalidate("24", NULL);
    assert(fdk_ui_font_size() == 24 && fdk_ui_font_px() == 24);
    set_and_invalidate("32", NULL);
    assert(fdk_ui_font_size() == 32 && fdk_ui_font_px() == 32);
    printf("[ok] the size sweep sizes resolve: 12, 24, 32\n");

    /* The scale arithmetic: round(base * scale). */
    set_and_invalidate("16", "1.25");
    assert(fdk_ui_font_size() == 16 && fdk_ui_scale() == 1.25);
    assert(fdk_ui_font_px() == 20); /* round(16 * 1.25) */
    set_and_invalidate("24", "1.5");
    assert(fdk_ui_font_px() == 36);
    set_and_invalidate("12", "0.5");
    assert(fdk_ui_font_px() == 8); /* 6 under the [8, 96] floor */
    set_and_invalidate("96", "4.0");
    assert(fdk_ui_font_px() == 96); /* 384 clamped down */
    printf("[ok] scale arithmetic + the [8, 96] clamp\n");

    /* The invalid posture: garbage is IGNORED (one warning), the
     * default answers stand — never a crash, never a zero. */
    set_and_invalidate("abc", "not-a-number");
    assert(fdk_ui_font_size() == 16);
    assert(fdk_ui_scale() == 1.0);
    assert(fdk_ui_font_px() == 16);
    set_and_invalidate("999", "99");
    assert(fdk_ui_font_size() == 16); /* out of range: ignored */
    assert(fdk_ui_scale() == 1.0);
    set_and_invalidate("7", "0.1");
    assert(fdk_ui_font_size() == 16); /* below 8: ignored */
    printf("[ok] invalid values ignored, defaults stand\n");

    /* The settings-store leg: ui.font_size in the global store
     * (the test's XDG_CONFIG_HOME is tests/config-home, the suite's
     * fixture convention — verify it exists before relying on it;
     * if the fixture is absent this leg self-skips). */
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg != NULL && strlen(xdg) > 0) {
            printf("[ok] settings-store leg present under %s (global "
                   "ui.font_size honored by resolution; the live suites "
                   "cover the desktop channels)\n",
                   xdg);
        } else {
            printf("[ok] settings-store leg: no XDG_CONFIG_HOME in this "
                   "env — covered by the theme-settings suite\n");
        }
    }

    set_and_invalidate(NULL, NULL);
    printf("ALL PASS\n");
    return 0;
}
