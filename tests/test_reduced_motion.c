/*
 * test_reduced_motion.c — the reduced-motion preference's
 * hermetic value tests (make test; no display, no bus needed).
 *
 * The getter ladder exercised through the env override, the same
 * posture test_color_scheme.c takes: $FDK_REDUCE_MOTION owns the
 * answer when set, so the parse table and the precedence are
 * testable without any desktop. The XSETTINGS and portal channels
 * degrade to "not reduced" headlessly — which the default test
 * pins too.
 */

#include "hermetic_env.h"
#include "fdk/fdk_theme.h"

#include "theme/theme_internal.h" /* the reset seam */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_env_override(void) {
    fdk__reduced_motion_reset_for_tests();

    /* The truth spellings. */
    setenv("FDK_REDUCE_MOTION", "1", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "reduce", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "on", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "true", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    /* The false spellings. */
    setenv("FDK_REDUCE_MOTION", "0", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "none", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "off", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    setenv("FDK_REDUCE_MOTION", "false", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    /* Garbage warns once and is IGNORED — the desktop (headless
     * here: the default) answers instead. */
    setenv("FDK_REDUCE_MOTION", "garbage", 1);
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    unsetenv("FDK_REDUCE_MOTION");

    printf("[ok] reduced-motion env override parsing\n");
}

/* The headless default: no override, no XSETTINGS daemon, no
 * portal — the documented default is NOT reduced. */
static void test_default_not_reduced(void) {
    fdk__reduced_motion_reset_for_tests();
    assert(!fdk_system_prefers_reduced_motion());
    /* The getter is idempotent across calls (the cached answer,
     * the lazy poll armed exactly once). */
    assert(!fdk_system_prefers_reduced_motion());
    printf("[ok] reduced-motion headless default: not reduced\n");
}

/* The override WINS over whatever the desktop might say: with the
 * env set, the XSETTINGS poll and the portal tick are both no-ops
 * (the spoke getters stay false — the channels were muted, which
 * is the precedence the module promises). */
static void test_override_mutes_channels(void) {
    fdk__reduced_motion_reset_for_tests();
    setenv("FDK_REDUCE_MOTION", "1", 1);

    /* Directly poke the channel entry points the way the X11
     * notify would: with the override set they must not change
     * the answer and must not mark themselves as having spoken. */
    fdk__reduced_motion_poll_x11();
    assert(fdk_system_prefers_reduced_motion());
    assert(!fdk__reduced_motion_x11_spoke());

    unsetenv("FDK_REDUCE_MOTION");
    fdk__reduced_motion_reset_for_tests();
    printf("[ok] reduced-motion override mutes desktop channels\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_env_override();
    test_default_not_reduced();
    test_override_mutes_channels();
    printf("\nall reduced-motion tests passed\n");
    return 0;
}
