/* test_theme_discovery.c — theme discovery + global-settings boot
 * tests (1.4.13).
 *
 * Everything is sandboxed through the environment the discovery
 * layer already reads: $FDK_THEME_DIR / $XDG_DATA_HOME /
 * $FDK_PREFS_FILE / $FDK_THEME point at mkdtemp sandboxes and
 * fixtures in tests/data/themes/ (alpha, beta, oddname, broken), so
 * the test never sees the machine's real themes or settings — and
 * never writes outside its sandbox either.
 *
 * What is proven here:
 *   - the search path: $FDK_THEME_DIR beats $XDG_DATA_HOME;
 *     same-stem shadowing resolves to the higher-priority file
 *   - fdk_theme_find: exact stem match; internal-name match
 *     (oddname.fdk's name "The Odd One"); case-sensitivity; the
 *     stem grammar gate ('../etc/passwd' style input can never
 *     become a path); NOT_FOUND when nothing matches; the broken
 *     neighbor skipped without hiding the healthy ones; the SAME
 *     broken file asked for BY NAME propagating its parse error
 *     (loud when chosen, quiet when merely present)
 *   - enumeration: sorted output, path strings, shadowed duplicates
 *     dropped, NULL/0 past the end, hidden and non-.fdk entries
 *     ignored
 *   - fdk_theme_file_path: set for found themes, NULL for
 *     create_default/parse
 *   - the one-shot settings boot (via fdk__theme_boot_reset_for_tests
 *     between scenarios): no preference -> built-in; theme.name in
 *     the global store -> applied at first resolution; $FDK_THEME
 *     beats the file; an unresolvable name fails SOFT (one warning,
 *     built-in stays); an explicit set_default() before the first
 *     resolution opts the process out; XDG_CONFIG_HOME resolution
 *     (not just $FDK_PREFS_FILE)
 *   - boot + discover under ASan/LSan: every scenario ends clean
 *     (the boot theme's ownership is the reset hook's business)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_prefs.h"
#include "fdk/fdk_theme.h"

#include "theme/theme_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

static char g_dir[256];      /* mkdtemp sandbox for the whole binary */
static char g_dir2[256];     /* second sandbox (the low-priority dir) */
static char g_fixtures[1024];/* abs path to tests/data/themes */

static const char *fixture(const char *name) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/%s", g_fixtures, name);
    return path;
}

static void use_env(const char *name, const char *value) {
    if (value != NULL) {
        assert(setenv(name, value, 1) == 0);
    } else {
        unsetenv(name);
    }
}

/* The global settings file under $FDK_PREFS_FILE (the sandbox), with
 * optional contents. Writing the file and pointing the override at
 * it are ONE operation — a file nobody reads is the exact bug the
 * first run of these tests caught. NULL clears both. */
static void settings_file(const char *contents) {
    char path[512];
    snprintf(path, sizeof path, "%s/fdk.prefs", g_dir);
    if (contents == NULL) {
        remove(path);
        use_env("FDK_PREFS_FILE", NULL);
        return;
    }
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(contents, 1, strlen(contents), f) == strlen(contents));
    fclose(f);
    use_env("FDK_PREFS_FILE", path);
}

/* Points the whole discovery stack at the fixtures (high priority)
 * and sandbox2 (low priority), with a clean settings file.
 * $XDG_DATA_DIRS is pinned to a sandbox WITHOUT an fdk/themes
 * subdir so the machine's real system themes can never leak into
 * a count assertion — hermetic on any host. */
static void discovery_env(void) {
    use_env("FDK_THEME_DIR", g_fixtures);
    use_env("XDG_DATA_HOME", g_dir2);
    use_env("XDG_DATA_DIRS", g_dir);
    use_env("FDK_THEME", NULL);
    settings_file(NULL); /* also clears $FDK_PREFS_FILE */
    fdk__theme_scan_reset_for_tests();
}

static bool streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static bool contains(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

/* ---- discovery: find() ------------------------------------------------ */

static void test_find_stem(void) {
    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("alpha", &r);
    assert(t != NULL);
    assert(r == FDK_OK);
    assert(streq(fdk_theme_name(t), "Alpha Test"));
    assert(streq(fdk_theme_author(t), "FDK tests"));
    const char *fp = fdk_theme_file_path(t);
    assert(fp != NULL && streq(fp, fixture("alpha.fdk")));
    fdk_theme_destroy(t);
}

static void test_find_internal_name(void) {
    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    /* The stem does not match any file; the internal name does. */
    fdk_theme *t = fdk_theme_find("The Odd One", &r);
    assert(t != NULL);
    assert(r == FDK_OK);
    assert(streq(fdk_theme_file_path(t), fixture("oddname.fdk")));
    fdk_theme_destroy(t);
}

static void test_find_case_sensitive(void) {
    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    assert(fdk_theme_find("ALPHA", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
}

static void test_find_not_found(void) {
    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    assert(fdk_theme_find("nope", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
    /* The internal-name pass runs on a miss and must not find
     * anything either ("Beta" IS an internal name — present; but
     * "beta extra" is not). */
    assert(fdk_theme_find("beta extra", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
}

static void test_find_argument_safety(void) {
    discovery_env();
    fdk_result r = FDK_OK;
    assert(fdk_theme_find(NULL, &r) == NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    r = FDK_OK;
    assert(fdk_theme_find("", &r) == NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    r = FDK_OK;
    /* Path-shaped and traversal-shaped names: outside the stem
     * grammar, so they can only reach the internal-name pass, which
     * builds paths from directory ENTRIES, never from this string.
     * Not found, never a crash, never a stat outside the sandbox. */
    assert(fdk_theme_find("../../etc/passwd", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
    assert(fdk_theme_find("a/b", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
    assert(fdk_theme_find(".", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
    assert(fdk_theme_find("..", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
}

static void test_find_broken_neighbor_skipped(void) {
    discovery_env();
    /* broken.fdk fails to parse; asking for a HEALTHY neighbor by
     * internal name forces the scan pass over broken.fdk, which
     must be skipped, not fatal. */
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("The Odd One", &r);
    assert(t != NULL);
    assert(streq(fdk_theme_name(t), "The Odd One"));
    fdk_theme_destroy(t);
}

static void test_find_broken_direct_is_loud(void) {
    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    /* The same file asked for BY NAME: the error propagates. */
    assert(fdk_theme_find("broken", &r) == NULL);
    assert(r == FDK_ERR_THEME_PARSE);
}

static void test_find_priority(void) {
    /* Put a DIFFERENT alpha.fdk in the lower-priority dir: the
     * fixture (in $FDK_THEME_DIR) must win. */
    char low[512];
    snprintf(low, sizeof low, "%s/alpha.fdk", g_dir2);
    FILE *f = fopen(low, "wb");
    assert(f != NULL);
    const char *low_theme =
        "version = 1\nname = \"Alpha Low\"\n[colors]\naccent = #111111\n";
    assert(fwrite(low_theme, 1, strlen(low_theme), f) == strlen(low_theme));
    fclose(f);

    discovery_env();
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_find("alpha", &r);
    assert(t != NULL);
    assert(streq(fdk_theme_name(t), "Alpha Test")); /* NOT "Alpha Low" */
    fdk_theme_destroy(t);
    remove(low);
}

static void test_find_no_dirs(void) {
    use_env("FDK_THEME_DIR", "");
    use_env("XDG_DATA_HOME", NULL);
    use_env("HOME", g_dir); /* exists but has no theme dirs */
    use_env("XDG_DATA_DIRS", g_dir); /* no fdk/themes inside */
    use_env("FDK_PREFS_FILE", NULL);
    fdk__theme_scan_reset_for_tests();
    fdk_result r = FDK_ERR_UNKNOWN;
    assert(fdk_theme_find("alpha", &r) == NULL);
    assert(r == FDK_ERR_NOT_FOUND);
    assert(fdk_theme_available_count() == 0);
    assert(fdk_theme_available_name(0) == NULL);
    assert(fdk_theme_available_path(0) == NULL);
}

/* ---- discovery: enumeration ------------------------------------------ */

static void test_enumeration(void) {
    discovery_env();
    /* Sandbox2 also gets a shadowed duplicate of alpha + a stray
     * non-theme file + a dotfile, to pin dedupe and filtering. */
    char low[512];
    snprintf(low, sizeof low, "%s/alpha.fdk", g_dir2);
    FILE *f = fopen(low, "wb");
    assert(f != NULL);
    const char *low_theme =
        "version = 1\nname = \"Alpha Low\"\n[colors]\naccent = #111111\n";
    assert(fwrite(low_theme, 1, strlen(low_theme), f) == strlen(low_theme));
    fclose(f);
    snprintf(low, sizeof low, "%s/README.txt", g_dir2);
    f = fopen(low, "wb");
    assert(f != NULL);
    assert(fwrite("not a theme\n", 1, 12, f) == 12);
    fclose(f);
    snprintf(low, sizeof low, "%s/.hidden.fdk", g_dir2);
    f = fopen(low, "wb");
    assert(f != NULL);
    assert(fwrite("version = 1\n", 1, 12, f) == 12);
    fclose(f);

    size_t n = fdk_theme_available_count();
    /* alpha, beta, broken, oddname — sorted; the sandbox2 alpha is
     * shadowed, README and dotfile filtered out. broken IS listed
     * (inventory, not validation). */
    assert(n == 4);
    assert(streq(fdk_theme_available_name(0), "alpha"));
    assert(streq(fdk_theme_available_name(1), "beta"));
    assert(streq(fdk_theme_available_name(2), "broken"));
    assert(streq(fdk_theme_available_name(3), "oddname"));
    /* The high-priority alpha's path won. */
    assert(streq(fdk_theme_available_path(0), fixture("alpha.fdk")));
    for (size_t i = 0; i < n; i++) {
        assert(contains(fdk_theme_available_path(i), ".fdk"));
    }
    assert(fdk_theme_available_name(4) == NULL);
    assert(fdk_theme_available_path(4) == NULL);

    remove(low);
    snprintf(low, sizeof low, "%s/alpha.fdk", g_dir2);
    remove(low);
    snprintf(low, sizeof low, "%s/README.txt", g_dir2);
    remove(low);
}

static void test_file_path_origins(void) {
    /* create_default/parse themes are pathless; only loaded ones
     * know where they came from. */
    fdk_theme *t = fdk_theme_create_default();
    assert(t != NULL);
    assert(fdk_theme_file_path(t) == NULL);
    fdk_theme_destroy(t);

    const char *text = "version = 1\nname = \"From Memory\"\n";
    fdk_theme *p = fdk_theme_parse(text, strlen(text), NULL);
    assert(p != NULL);
    assert(fdk_theme_file_path(p) == NULL);
    fdk_theme_destroy(p);
}

/* ---- the one-shot settings boot --------------------------------------- */

static void boot_scenario_start(void) {
    fdk__theme_boot_reset_for_tests();
}

static void test_boot_no_preference(void) {
    boot_scenario_start();
    discovery_env();
    settings_file(NULL);
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "FDK Modern"));
}

static void test_boot_from_settings(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Alpha Test"));
    /* The installed default's file path survives (it is the boot's
     * owned theme, not a dangling stack object). */
    assert(fdk_theme_file_path(t) != NULL);
    assert(streq(fdk_theme_file_path(t), fixture("alpha.fdk")));
}

static void test_boot_env_beats_settings(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    use_env("FDK_THEME", "beta");
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Beta"));
}

static void test_boot_unresolvable_is_soft(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = no-such-theme\n");
    fdk_theme *t = fdk_theme_get_default();
    /* Soft failure: one warning (visible in the log output of the
     * binary), built-in stays, nothing crashes. */
    assert(streq(fdk_theme_name(t), "FDK Modern"));
}

static void test_boot_explicit_set_opts_out(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    /* The application installs its own theme FIRST: the global
     * setting must never override it. */
    fdk_theme *own = fdk_theme_create_default();
    assert(own != NULL);
    assert(fdk_ok(fdk_theme_set_name(own, "Explicitly Mine")));
    fdk_theme_set_default(own);
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Explicitly Mine"));
    fdk_theme_destroy(own);
    fdk_theme_set_default(NULL);
}

static void test_boot_xdg_resolution(void) {
    boot_scenario_start();
    discovery_env();
    /* Not $FDK_PREFS_FILE this time: the XDG_CONFIG_HOME path a real
     * desktop uses (~/.config/fdk.prefs). */
    use_env("FDK_PREFS_FILE", NULL);
    char xdg[512];
    snprintf(xdg, sizeof xdg, "%s/xdgconf", g_dir);
    assert(mkdir(xdg, 0777) == 0);
    use_env("XDG_CONFIG_HOME", xdg);
    char prefs[600];
    snprintf(prefs, sizeof prefs, "%s/fdk.prefs", xdg);
    FILE *f = fopen(prefs, "wb");
    assert(f != NULL);
    const char *body = "[theme]\nname = beta\n";
    assert(fwrite(body, 1, strlen(body), f) == strlen(body));
    fclose(f);

    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Beta"));
    remove(prefs);
    rmdir(xdg);
}

static void test_boot_runs_once(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Alpha Test"));
    /* Change the file: the boot already ran, the process keeps its
     * theme — one-shot is the documented contract. */
    settings_file("[theme]\nname = beta\n");
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Alpha Test"));
}

static void test_boot_empty_env_is_unset(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    use_env("FDK_THEME", ""); /* exported-empty behaves as unset */
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Alpha Test"));
}

/* ---- 1.4.14: the per-app leg + the whitelist + the live recheck -------- */

/* The app prefs file under the same sandbox dir as the global one —
 * what `fdk-set theme set NAME --app APP` writes for the app whose
 * fdk_init identity is APP. */
static void app_settings_file(const char *app_id, const char *contents) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.prefs", g_dir, app_id);
    if (contents == NULL) {
        remove(path);
        return;
    }
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(contents, 1, strlen(contents), f) == strlen(contents));
    fclose(f);
}

/* Binds the process to an application identity (what fdk_init does;
 * the test drives the settings engine directly through the internal
 * header it already includes). */
static void bind_app(const char *app_id) {
    fdk__theme_settings_set_app_id(app_id);
}

/* The XDG sandbox layout: $FDK_PREFS_FILE is a SINGLE-file override
 * (one absolute path for every store — the rig variable), so the
 * per-app leg is invisible under it. Per-app resolution needs the
 * real desktop layout: XDG_CONFIG_HOME/<store>.prefs per store,
 * which is exactly what this variant arranges. */
static void xdg_env(void) {
    discovery_env(); /* theme dirs + a clean FDK_THEME */
    use_env("FDK_PREFS_FILE", NULL);
    use_env("XDG_CONFIG_HOME", g_dir);
    fdk__theme_scan_reset_for_tests();
}

/* The global store in the XDG layout (g_dir/fdk.prefs). */
static void xdg_global_file(const char *contents) {
    char path[512];
    snprintf(path, sizeof path, "%s/fdk.prefs", g_dir);
    if (contents == NULL) {
        remove(path);
        return;
    }
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(contents, 1, strlen(contents), f) == strlen(contents));
    fclose(f);
}

static void xdg_cleanup(void) {
    xdg_global_file(NULL);
    app_settings_file("org.fdk.example99", NULL);
    use_env("XDG_CONFIG_HOME", NULL);
}

static void test_boot_app_store_beats_global(void) {
    boot_scenario_start();
    xdg_env();
    bind_app("org.fdk.example99");
    xdg_global_file("[theme]\nname = alpha\n");
    app_settings_file("org.fdk.example99", "[theme]\nname = beta\n");
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Beta"));
    /* The per-app override wins over the global setting. */
    fdk__theme_settings_reset_for_tests();
    bind_app(NULL);
    xdg_cleanup();
}

static void test_boot_global_when_app_store_empty(void) {
    boot_scenario_start();
    xdg_env();
    bind_app("org.fdk.example99");
    xdg_global_file("[theme]\nname = alpha\n");
    app_settings_file("org.fdk.example99", NULL);
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Alpha Test"));
    fdk__theme_settings_reset_for_tests();
    bind_app(NULL);
    xdg_cleanup();
}

static void test_boot_env_beats_app_store(void) {
    boot_scenario_start();
    xdg_env();
    bind_app("org.fdk.example99");
    xdg_global_file(NULL);
    app_settings_file("org.fdk.example99", "[theme]\nname = beta\n");
    use_env("FDK_THEME", "alpha");
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Alpha Test"));
    use_env("FDK_THEME", NULL);
    fdk__theme_settings_reset_for_tests();
    bind_app(NULL);
    xdg_cleanup();
}

static void test_whitelist_argument_safety(void) {
    boot_scenario_start();
    discovery_env();
    /* NULL list with a count is rejected; empty entries are rejected;
     * oversize entries are rejected; count 0 clears. */
    assert(!fdk_ok(fdk_theme_set_allowed_themes(NULL, 2)));
    const char *bad_empty[2] = {"alpha", ""};
    assert(!fdk_ok(fdk_theme_set_allowed_themes(bad_empty, 2)));
    const char *bad_huge[1] = {"0123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789"
                               "0123456789012345678901234567890123456789"
                               "0123456789"};
    assert(!fdk_ok(fdk_theme_set_allowed_themes(bad_huge, 1)));
    assert(fdk_theme_allowed_count() == 0);
    assert(fdk_theme_allowed_name(0) == NULL);
    assert(fdk_ok(fdk_theme_set_allowed_themes(NULL, 0))); /* clear */
}

static void test_whitelist_clamps_boot(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = beta\n");
    /* The user set 'beta'; the developer allows only alpha. */
    const char *only[1] = {"alpha"};
    assert(fdk_ok(fdk_theme_set_allowed_themes(only, 1)));
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Alpha Test"));
    assert(fdk_theme_allowed_count() == 1);
    assert(streq(fdk_theme_allowed_name(0), "alpha"));
    fdk_theme_clear_allowed_themes();
}

static void test_whitelist_no_setting_means_first_allowed(void) {
    boot_scenario_start();
    discovery_env();
    settings_file(NULL);
    const char *curated[2] = {"beta", "alpha"};
    assert(fdk_ok(fdk_theme_set_allowed_themes(curated, 2)));
    fdk_theme *t = fdk_theme_get_default();
    /* Nothing set anywhere: the first allowed theme, NOT the built-in. */
    assert(streq(fdk_theme_name(t), "Beta"));
    fdk_theme_clear_allowed_themes();
}

static void test_whitelist_internal_name_spelling(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = beta\n");
    /* oddname.fdk's internal name is "The Odd One": allowing the
     * DISPLAY name accepts a user setting the STEM. */
    const char *only[1] = {"The Odd One"};
    assert(fdk_ok(fdk_theme_set_allowed_themes(only, 1)));
    /* 'beta' is NOT allowed — the clamp falls back to the only entry,
     * which must resolve through the internal-name path. */
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "The Odd One"));
    fdk_theme_clear_allowed_themes();
}

static void test_whitelist_allows_the_set_theme(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = beta\n");
    const char *curated[2] = {"alpha", "beta"};
    assert(fdk_ok(fdk_theme_set_allowed_themes(curated, 2)));
    fdk_theme *t = fdk_theme_get_default();
    assert(streq(fdk_theme_name(t), "Beta")); /* allowed: unchanged */
    fdk_theme_clear_allowed_themes();
}

static void test_whitelist_reclamps_a_running_app(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = beta\n");
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Beta"));
    /* The restriction arrives AFTER the boot installed beta. */
    const char *only[1] = {"alpha"};
    assert(fdk_ok(fdk_theme_set_allowed_themes(only, 1)));
    assert(streq(fdk_theme_name(fdk_theme_get_default()),
                 "Alpha Test"));
    fdk_theme_clear_allowed_themes();
}

static void test_recheck_follows_file_changes(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    assert(streq(fdk_theme_name(fdk_theme_get_default()),
                 "Alpha Test"));
    /* The live follow's core (what the inotify drain lands in): a
     * settings change re-themes the RUNNING process. */
    settings_file("[theme]\nname = beta\n");
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Beta"));
    /* Same value again: no-op, current survives. */
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Beta"));
    /* Cleared at runtime: revert to the built-in. */
    settings_file(NULL);
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "FDK Modern"));
    /* Unresolvable at runtime: soft, current survives. */
    settings_file("[theme]\nname = no-such-theme\n");
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "FDK Modern"));
}

static void test_recheck_app_store_change(void) {
    boot_scenario_start();
    xdg_env();
    bind_app("org.fdk.example99");
    xdg_global_file("[theme]\nname = alpha\n");
    app_settings_file("org.fdk.example99", NULL);
    assert(streq(fdk_theme_name(fdk_theme_get_default()),
                 "Alpha Test"));
    /* The per-app override lands while the app runs. */
    app_settings_file("org.fdk.example99", "[theme]\nname = beta\n");
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "Beta"));
    /* And is forgotten live. */
    app_settings_file("org.fdk.example99", NULL);
    fdk__theme_settings_recheck();
    assert(streq(fdk_theme_name(fdk_theme_get_default()),
                 "Alpha Test"));
    fdk__theme_settings_reset_for_tests();
    bind_app(NULL);
    xdg_cleanup();
}

static void test_recheck_opted_out_app_is_left_alone(void) {
    boot_scenario_start();
    discovery_env();
    settings_file("[theme]\nname = alpha\n");
    /* The app installs its own theme: ownership flips. */
    fdk_theme *own = fdk_theme_create_default();
    assert(own != NULL);
    assert(fdk_ok(fdk_theme_set_name(own, "Explicitly Mine")));
    fdk_theme_set_default(own);
    settings_file("[theme]\nname = beta\n");
    fdk__theme_settings_recheck(); /* must stand down */
    assert(streq(fdk_theme_name(fdk_theme_get_default()),
                 "Explicitly Mine"));
    fdk_theme_destroy(own);
    /* Destroy reverts to the built-in (not to the setting — the
     * process stays opted out). */
    assert(streq(fdk_theme_name(fdk_theme_get_default()), "FDK Modern"));
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    char tmpl[] = "/tmp/fdk-discover-XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    snprintf(g_dir, sizeof g_dir, "%s", tmpl);
    char tmpl2[] = "/tmp/fdk-discover2-XXXXXX";
    assert(mkdtemp(tmpl2) != NULL);
    snprintf(g_dir2, sizeof g_dir2, "%s", tmpl2);

    /* Locate tests/data/themes relative to this binary
     * (build/tests/ -> ../../tests/data/themes). The path is already
     * absolute (from /proc/self/exe) and keeps its ".." components —
     * opendir/stat resolve them, and every comparison in this file
     * builds expectations from the same string, so canonicalization
     * would add nothing. */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(n > 0);
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    assert(slash != NULL);
    *slash = '\0'; /* dirname */
    snprintf(g_fixtures, sizeof g_fixtures, "%s/../../tests/data/themes",
             self);
    struct stat st;
    assert(stat(fixture("alpha.fdk"), &st) == 0);

    test_find_stem();
    test_find_internal_name();
    test_find_case_sensitive();
    test_find_not_found();
    test_find_argument_safety();
    test_find_broken_neighbor_skipped();
    test_find_broken_direct_is_loud();
    test_find_priority();
    test_find_no_dirs();
    test_enumeration();
    test_file_path_origins();

    test_boot_no_preference();
    test_boot_from_settings();
    test_boot_env_beats_settings();
    test_boot_unresolvable_is_soft();
    test_boot_explicit_set_opts_out();
    test_boot_xdg_resolution();
    test_boot_runs_once();
    test_boot_empty_env_is_unset();

    test_boot_app_store_beats_global();
    test_boot_global_when_app_store_empty();
    test_boot_env_beats_app_store();
    test_whitelist_argument_safety();
    test_whitelist_clamps_boot();
    test_whitelist_no_setting_means_first_allowed();
    test_whitelist_internal_name_spelling();
    test_whitelist_allows_the_set_theme();
    test_whitelist_reclamps_a_running_app();
    test_recheck_follows_file_changes();
    test_recheck_app_store_change();
    test_recheck_opted_out_app_is_left_alone();

    /* Leave the boot in the pristine state for anything that
     * follows in this process (nothing does today, but the reset
     * also frees the last scenario's boot theme under LSan). */
    fdk__theme_boot_reset_for_tests();
    bind_app(NULL); /* release the settings engine's app-id copy */

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", g_dir, g_dir2);
    assert(system(cmd) == 0);

    printf("test_theme_discovery: all groups passed\n");
    return 0;
}
