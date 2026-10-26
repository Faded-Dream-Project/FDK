/* test_color_scheme.c — the desktop color-scheme module (1.1.0)
 *
 * Headless, hermetic: the env override, the GTK string parse (via
 * the public getter under $FDK_COLOR_SCHEME), the scheme→variant
 * face selection through the settings engine (temp theme dirs,
 * the discovery env), and the NULL-ladder. The LIVE channels are
 * exercised in test_x11_integration.c (a fake XSETTINGS daemon
 * speaking the documented byte format, flipped live through
 * PropertyNotify) and test_dbus_live.c (a mock portal serving
 * ReadOne + SettingChanged over the native D-Bus client).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_theme.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* The internal seam for the parse helpers (hermetic value tests). */
#include "theme/theme_internal.h"

static char g_dir[512];

static void write_face(const char *stem, const char *name,
                       const char *accent) {
    char path[768];
    snprintf(path, sizeof path, "%s/%s.fdk", g_dir, stem);
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f,
            "version = 1\n"
            "name = \"%s\"\n"
            "author = \"color-scheme test\"\n"
            "\n"
            "[colors]\n"
            "accent = %s\n",
            name, accent);
    fclose(f);
}

static void test_env_override(void) {
    fdk__color_scheme_reset_for_tests();

    setenv("FDK_COLOR_SCHEME", "dark", 1);
    fdk__color_scheme_reset_for_tests();
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_DARK);
    unsetenv("FDK_COLOR_SCHEME");

    setenv("FDK_COLOR_SCHEME", "light", 1);
    fdk__color_scheme_reset_for_tests();
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_LIGHT);
    unsetenv("FDK_COLOR_SCHEME");

    setenv("FDK_COLOR_SCHEME", "none", 1);
    fdk__color_scheme_reset_for_tests();
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_NONE);
    unsetenv("FDK_COLOR_SCHEME");

    setenv("FDK_COLOR_SCHEME", "garbage", 1);
    fdk__color_scheme_reset_for_tests();
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_NONE);
    unsetenv("FDK_COLOR_SCHEME");

    /* "1"/"2" spellings */
    setenv("FDK_COLOR_SCHEME", "1", 1);
    fdk__color_scheme_reset_for_tests();
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_DARK);
    unsetenv("FDK_COLOR_SCHEME");

    printf("[ok] color-scheme env override parsing\n");
}

static void test_variant_selection(void) {
    fdk__color_scheme_reset_for_tests();
    unsetenv("FDK_THEME");

    /* A pair of faces: daylight + daylight-dark. Discovery rooted at
     * a temp dir through the theme path env. */
    snprintf(g_dir, sizeof g_dir, "/tmp/fdk-cs-%ld", (long)getpid());
    mkdir(g_dir, 0755);
    write_face("daylight", "Daylight", "#88ccff");
    write_face("daylight-dark", "Daylight-Dark", "#ffcc88");
    setenv("FDK_THEME_DIR", g_dir, 1);
    setenv("FDK_THEME", "daylight", 1);

    /* NOTE: the settings engine boots ONCE per process (the
     * g_boot_ran guard) and app-owned themes (fdk_theme_set_default)
     * opt it out — so this suite drives the engine through the same
     * recheck a live XSETTINGS/portal notification uses, never
     * calling set_default (the engine stays the owner throughout).
     * The boot itself ran above via the NONE case. */

    /* NONE: the base face resolves (the boot call). */
    fdk__color_scheme_reset_for_tests();
    fdk__theme_settings_boot(); /* first call runs; later ones no-op */
    assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                  "Daylight") == 0);

    /* DARK: the sibling face takes over (the recheck a scheme
     * change fires — exactly the live-flip path). */
    setenv("FDK_COLOR_SCHEME", "dark", 1);
    fdk__color_scheme_reset_for_tests();
    fdk__theme_settings_recheck();
    assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                  "Daylight-Dark") == 0);

    /* Back to NONE: the base face returns (the flip is live both
     * ways). */
    setenv("FDK_COLOR_SCHEME", "none", 1);
    fdk__color_scheme_reset_for_tests();
    fdk__theme_settings_recheck();
    assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                  "Daylight") == 0);

    /* LIGHT with no -light sibling: the base face stays. */
    setenv("FDK_COLOR_SCHEME", "light", 1);
    fdk__color_scheme_reset_for_tests();
    fdk__theme_settings_recheck();
    assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                  "Daylight") == 0);

    /* A theme with NO sibling at all: never any flip. */
    write_face("solo", "Solo", "#ffffff");
    setenv("FDK_THEME", "solo", 1);
    setenv("FDK_COLOR_SCHEME", "dark", 1);
    fdk__color_scheme_reset_for_tests();
    fdk__theme_settings_recheck();
    assert(strcmp(fdk_theme_name(fdk_theme_get_default()),
                  "Solo") == 0);

    unsetenv("FDK_COLOR_SCHEME");
    unsetenv("FDK_THEME");
    unsetenv("FDK_THEME_DIR");
    fdk__color_scheme_reset_for_tests();
    printf("[ok] color-scheme variant face selection (dark sibling, "
           "light miss, no sibling)\n");
}

static void test_getter_ladder(void) {
    assert(fdk_system_color_scheme() == FDK_COLOR_SCHEME_NONE ||
           fdk_system_color_scheme() == FDK_COLOR_SCHEME_DARK ||
           fdk_system_color_scheme() == FDK_COLOR_SCHEME_LIGHT);
    fdk__color_scheme_shutdown(); /* idempotent, no context needed */
    fdk__color_scheme_shutdown();
    printf("[ok] color-scheme getter + shutdown ladder\n");
}

int main(void) {
    unsetenv("DISPLAY"); /* hermetic: no XSETTINGS channel speaks */
    unsetenv("WAYLAND_DISPLAY");
    test_env_override();
    test_variant_selection();
    test_getter_ladder();
    printf("\nall color-scheme tests passed\n");
    return 0;
}
