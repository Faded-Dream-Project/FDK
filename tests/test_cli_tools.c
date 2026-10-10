/* test_cli_tools.c — end-to-end tests for the command-line face
 * (fdk-theme, fdk-prefs) (1.4.13).
 *
 * The tools are REAL subprocesses: popen() runs the binaries the
 * build produced (located relative to this test binary, so the
 * suite works from any cwd), inheriting this process's sandboxed
 * environment. Every assertion is on observable CLI contract —
 * stdout text and exit codes — exactly what a script would consume:
 *
 *   fdk-theme: list content (--paths), get across all three origins
 *   (nothing set / the settings file / $FDK_THEME), set + the file
 *   it wrote, reset, path, the exit codes for not-found (2) and
 *   unusable-theme (1), and the 'FDK Modern' == reset alias
 *
 *   fdk-prefs: typed set/get round trips (int/bool/double/string),
 *   list layout, remove, the missing-key exit (2) vs --default (0),
 *   the section.name shape rejection (1), -a store isolation, and
 *   path
 *
 *   both: the config-directory-creation policy (a store under a
 *   two-level-deep nonexistent directory succeeds on first run)
 *
 * The tools run under the same ASan/LSan build as everything else:
 * a leak in a tool is a failed exit code here, not a silent pass.
 */

#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- helpers ---------------------------------------------------------- */

static char g_root[256];    /* the mkdtemp sandbox directory */
static char g_sandbox[256]; /* the settings-file path inside it */
static char g_fixtures[1024];
static char g_theme_tool[1024];
static char g_prefs_tool[1024];

static const char *fixture(const char *name) {
    static char path[1100];
    snprintf(path, sizeof path, "%s/%s", g_fixtures, name);
    return path;
}

/* Runs `cmd`, capturing stdout into `out` (truncated to fit).
 * Returns the exit code, or -1 when the child died abnormally
 * (assert-heavy tools should never). */
static int run_capture(const char *cmd, char *out, size_t outsz) {
    FILE *p = popen(cmd, "r");
    assert(p != NULL);
    size_t n = 0;
    out[0] = '\0';
    int c;
    while ((c = fgetc(p)) != EOF) {
        if (n + 1 < outsz) {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    int st = pclose(p);
    if (!WIFEXITED(st)) {
        return -1;
    }
    return WEXITSTATUS(st);
}

/* `tool args` with the single-quoted tool path prepended; the caller
 * keeps the returned buffer valid until the next call. */
static const char *cmdln(const char *tool, const char *args) {
    static char buf[4096];
    snprintf(buf, sizeof buf, "'%s' %s", tool, args);
    return buf;
}

static bool has_line(const char *out, const char *line) {
    /* Line-exact membership (a bare strstr would let "alpha" match
     * "alphabet"). */
    const char *p = out;
    size_t len = strlen(line);
    while ((p = strstr(p, line)) != NULL) {
        bool bol = (p == out) || (p[-1] == '\n');
        bool eol = (p[len] == '\n') || (p[len] == '\0');
        if (bol && eol) {
            return true;
        }
        p += len;
    }
    return false;
}

static bool file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static void sandbox_env(void) {
    assert(setenv("FDK_PREFS_FILE", g_sandbox, 1) == 0);
    remove(g_sandbox);
    assert(setenv("FDK_THEME_DIR", g_fixtures, 1) == 0);
    assert(setenv("XDG_DATA_HOME", g_root, 1) == 0);
    assert(setenv("XDG_DATA_DIRS", g_root, 1) == 0);
    assert(unsetenv("FDK_THEME") == 0);
    assert(unsetenv("XDG_CONFIG_HOME") == 0);
}

/* ---- fdk-theme -------------------------------------------------------- */

static void test_theme_help_version(void) {
    char out[8192];
    assert(run_capture(cmdln(g_theme_tool, "--help"), out, sizeof out) == 0);
    assert(strstr(out, "usage:") != NULL);
    assert(run_capture(cmdln(g_theme_tool, "version"), out, sizeof out) ==
           0);
    assert(strstr(out, "fdk-theme (FDK)") != NULL);
    assert(run_capture(cmdln(g_theme_tool, ""), out, sizeof out) == 1);
}

static void test_theme_list(void) {
    char out[8192];
    sandbox_env();
    int rc = run_capture(cmdln(g_theme_tool, "list"), out, sizeof out);
    assert(rc == 0);
    assert(has_line(out, "FDK Modern    (built-in default)"));
    assert(has_line(out, "alpha"));
    assert(has_line(out, "beta"));
    assert(has_line(out, "broken"));
    assert(has_line(out, "oddname"));
    assert(!has_line(out, "README"));

    rc = run_capture(cmdln(g_theme_tool, "list --paths"), out, sizeof out);
    assert(rc == 0);
    assert(strstr(out, fixture("alpha.fdk")) != NULL);
    assert(strstr(out, fixture("oddname.fdk")) != NULL);
}

static void test_theme_get_set_reset(void) {
    char out[8192];
    sandbox_env();

    /* Nothing set: the built-in, exactly. */
    assert(run_capture(cmdln(g_theme_tool, "get"), out, sizeof out) == 0);
    assert(has_line(out, "FDK Modern"));

    /* set: validated, stored, reported with the internal name and
     * the file it came from. */
    int rc = run_capture(cmdln(g_theme_tool, "set alpha"), out, sizeof out);
    assert(rc == 0);
    assert(strstr(out, "Alpha Test") != NULL);
    assert(strstr(out, fixture("alpha.fdk")) != NULL);
    assert(file_contains(g_sandbox, "name = alpha"));

    /* get: the stored handle. */
    assert(run_capture(cmdln(g_theme_tool, "get"), out, sizeof out) == 0);
    assert(has_line(out, "alpha"));

    /* path with and without an argument. */
    assert(run_capture(cmdln(g_theme_tool, "path"), out, sizeof out) == 0);
    assert(has_line(out, fixture("alpha.fdk")));
    assert(run_capture(cmdln(g_theme_tool, "path beta"), out,
                        sizeof out) == 0);
    assert(has_line(out, fixture("beta.fdk")));

    /* $FDK_THEME beats the file. */
    static char withenv[4096];
    snprintf(withenv, sizeof withenv, "FDK_THEME=beta '%s' get",
             g_theme_tool);
    assert(run_capture(withenv, out, sizeof out) == 0);
    assert(has_line(out, "beta"));

    /* reset: back to the built-in, key gone. */
    assert(run_capture(cmdln(g_theme_tool, "reset"), out, sizeof out) == 0);
    assert(run_capture(cmdln(g_theme_tool, "get"), out, sizeof out) == 0);
    assert(has_line(out, "FDK Modern"));
    assert(!file_contains(g_sandbox, "name ="));

    /* 'FDK Modern' as the set argument is the reset alias. */
    assert(run_capture(cmdln(g_theme_tool, "set alpha"), out,
                       sizeof out) == 0);
    assert(run_capture(cmdln(g_theme_tool, "set \"FDK Modern\""), out,
                       sizeof out) == 0);
    assert(run_capture(cmdln(g_theme_tool, "get"), out, sizeof out) == 0);
    assert(has_line(out, "FDK Modern"));
}

static void test_theme_exit_codes(void) {
    char out[8192];
    sandbox_env();
    /* Not found: 2, with the list hint. */
    assert(run_capture(cmdln(g_theme_tool, "set nope"), out,
                       sizeof out) == 2);
    /* Exists but unusable: 1. */
    assert(run_capture(cmdln(g_theme_tool, "set broken"), out,
                       sizeof out) == 1);
    /* Nothing was stored by either failure. */
    assert(!file_contains(g_sandbox, "name ="));
    /* path on a missing theme: 2. */
    assert(run_capture(cmdln(g_theme_tool, "path nope"), out,
                       sizeof out) == 2);
    /* Usage errors: 1. */
    assert(run_capture(cmdln(g_theme_tool, "set"), out, sizeof out) == 1);
    assert(run_capture(cmdln(g_theme_tool, "bogus"), out, sizeof out) == 1);
}

static void test_theme_internal_name_set(void) {
    char out[8192];
    sandbox_env();
    int rc = run_capture(cmdln(g_theme_tool, "set \"The Odd One\""), out,
                         sizeof out);
    assert(rc == 0);
    assert(file_contains(g_sandbox, "name = The Odd One"));
    assert(run_capture(cmdln(g_theme_tool, "get"), out, sizeof out) == 0);
    assert(has_line(out, "The Odd One"));
}

/* ---- fdk-prefs -------------------------------------------------------- */

static void test_prefs_help_version(void) {
    char out[8192];
    assert(run_capture(cmdln(g_prefs_tool, "--help"), out, sizeof out) == 0);
    assert(strstr(out, "usage:") != NULL);
    assert(run_capture(cmdln(g_prefs_tool, "version"), out, sizeof out) ==
           0);
    assert(strstr(out, "fdk-prefs (FDK)") != NULL);
}

static void test_prefs_roundtrip(void) {
    char out[8192];
    sandbox_env();
    remove(g_sandbox);

    assert(run_capture(cmdln(g_prefs_tool, "set window.width 940"), out,
                       sizeof out) == 0);
    assert(run_capture(cmdln(g_prefs_tool, "set window.maximized true"),
                       out, sizeof out) == 0);
    assert(run_capture(cmdln(g_prefs_tool, "set ui.scale 1.25"), out,
                       sizeof out) == 0);
    assert(run_capture(cmdln(g_prefs_tool, "set app.note \"hello world\""),
                       out, sizeof out) == 0);

    assert(run_capture(cmdln(g_prefs_tool, "get window.width"), out,
                       sizeof out) == 0);
    assert(has_line(out, "940"));
    assert(run_capture(cmdln(g_prefs_tool, "get window.maximized"), out,
                       sizeof out) == 0);
    assert(has_line(out, "true"));
    assert(run_capture(cmdln(g_prefs_tool, "get ui.scale"), out,
                       sizeof out) == 0);
    assert(has_line(out, "1.25"));
    assert(run_capture(cmdln(g_prefs_tool, "get app.note"), out,
                       sizeof out) == 0);
    assert(has_line(out, "hello world"));

    /* The typed canonical forms land in the file (set_int/bool/double
     * formatting, not the shell's spelling). */
    assert(file_contains(g_sandbox, "width = 940"));
    assert(file_contains(g_sandbox, "maximized = true"));
    assert(file_contains(g_sandbox, "scale = 1.25"));

    /* list: every key, file order. */
    assert(run_capture(cmdln(g_prefs_tool, "list"), out, sizeof out) == 0);
    assert(has_line(out, "window.width = 940"));
    assert(has_line(out, "window.maximized = true"));
    assert(has_line(out, "ui.scale = 1.25"));
    assert(has_line(out, "app.note = hello world"));

    /* remove: quiet, gone after. */
    assert(run_capture(cmdln(g_prefs_tool, "remove window.width"), out,
                       sizeof out) == 0);
    assert(run_capture(cmdln(g_prefs_tool, "get window.width"), out,
                       sizeof out) == 2);
    assert(!file_contains(g_sandbox, "width"));
}

static void test_prefs_missing_and_defaults(void) {
    char out[8192];
    sandbox_env();
    remove(g_sandbox);
    assert(run_capture(cmdln(g_prefs_tool, "get absent.key"), out,
                       sizeof out) == 2);
    assert(run_capture(cmdln(g_prefs_tool,
                             "get absent.key --default fallback"),
                       out, sizeof out) == 0);
    assert(has_line(out, "fallback"));
}

static void test_prefs_key_shape(void) {
    char out[8192];
    sandbox_env();
    /* The shape hint is a DIAGNOSTIC: it goes to stderr, so these
     * two probes merge stderr into the capture (2>&1) to assert the
     * message text; the rest of the suite asserts exit codes only. */
    static char witherr[4096];
    snprintf(witherr, sizeof witherr, "'%s' set flag true 2>&1",
             g_prefs_tool);
    assert(run_capture(witherr, out, sizeof out) == 1);
    assert(strstr(out, "section.name") != NULL);
    assert(run_capture(cmdln(g_prefs_tool, "get flag"), out,
                       sizeof out) == 1);
    assert(run_capture(cmdln(g_prefs_tool, "remove a..b"), out,
                       sizeof out) == 1);
}

static void test_prefs_app_isolation(void) {
    char out[8192];
    sandbox_env();
    remove(g_sandbox);
    assert(run_capture(cmdln(g_prefs_tool, "set theme.name alpha"), out,
                       sizeof out) == 0);

    /* A different app_id is a different store: same override
     * mechanism, exercised through a second file in the sandbox. */
    static char other[4096];
    snprintf(other, sizeof other, "FDK_PREFS_FILE=%s/other.prefs '%s' "
                                  "-a other-app set flag.on true",
             g_root, g_prefs_tool);
    assert(run_capture(other, out, sizeof out) == 0);

    snprintf(other, sizeof other, "FDK_PREFS_FILE=%s/other.prefs '%s' "
                                  "-a other-app list",
             g_root, g_prefs_tool);
    assert(run_capture(other, out, sizeof out) == 0);
    assert(has_line(out, "flag.on = true"));

    /* And the main store never saw it. */
    assert(run_capture(cmdln(g_prefs_tool, "list"), out, sizeof out) == 0);
    assert(!strstr(out, "flag.on"));
}

static void test_prefs_path(void) {
    char out[8192];
    sandbox_env();
    assert(run_capture(cmdln(g_prefs_tool, "path"), out, sizeof out) == 0);
    assert(has_line(out, g_sandbox));
}

/* ---- shared policy: first-run directory creation --------------------- */

static void test_first_run_creates_config_dir(void) {
    char out[8192];
    char deep[1024];
    snprintf(deep, sizeof deep, "%s/deep/config/fdk.prefs", g_sandbox);
    assert(setenv("FDK_PREFS_FILE", deep, 1) == 0);

    int rc = run_capture(cmdln(g_theme_tool, "set alpha"), out, sizeof out);
    assert(rc == 0);
    assert(file_contains(deep, "name = alpha"));

    rc = run_capture(cmdln(g_prefs_tool, "set window.width 100"), out,
                     sizeof out);
    assert(rc == 0);
    assert(file_contains(deep, "width = 100"));
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    char tmpl[] = "/tmp/fdk-cli-XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    snprintf(g_root, sizeof g_root, "%s", tmpl);
    snprintf(g_sandbox, sizeof g_sandbox, "%s/settings.prefs", tmpl);

    /* Tools live beside this binary: build/tests/ -> build/tools/. */
    char self[512];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    assert(n > 0);
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    assert(slash != NULL);
    *slash = '\0';
    snprintf(g_theme_tool, sizeof g_theme_tool, "%s/../tools/fdk-theme",
             self);
    snprintf(g_prefs_tool, sizeof g_prefs_tool, "%s/../tools/fdk-prefs",
             self);
    snprintf(g_fixtures, sizeof g_fixtures,
             "%s/../../tests/data/themes", self);

    struct stat st;
    assert(stat(g_theme_tool, &st) == 0);
    assert(stat(g_prefs_tool, &st) == 0);
    assert(stat(fixture("alpha.fdk"), &st) == 0);

    assert(setenv("HOME", tmpl, 1) == 0);

    test_theme_help_version();
    test_theme_list();
    test_theme_get_set_reset();
    test_theme_exit_codes();
    test_theme_internal_name_set();

    test_prefs_help_version();
    test_prefs_roundtrip();
    test_prefs_missing_and_defaults();
    test_prefs_key_shape();
    test_prefs_app_isolation();
    test_prefs_path();

    test_first_run_creates_config_dir();

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", tmpl);
    assert(system(cmd) == 0);

    printf("test_cli_tools: all groups passed\n");
    return 0;
}
