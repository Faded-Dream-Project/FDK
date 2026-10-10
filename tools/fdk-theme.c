/*
 * fdk-theme.c — the theme face of the FDK command line (1.4.13)
 *
 *     fdk-theme list              what is installed, where
 *     fdk-theme get               what a new FDK app would use
 *     fdk-theme set "matrix"      make that the global theme
 *     fdk-theme reset             back to the built-in theme
 *     fdk-theme path [name]       where a theme file lives
 *
 * The reason this tool exists is the reason every toolkit grows a
 * settings CLI: most applications are never going to ship a theme
 * picker, and a desktop where retheming requires each app to have
 * built one is a desktop that cannot be rethemed. GTK answers with
 * gsettings set org.gnome.desktop.interface gtk-theme-name; FDK
 * answers with this — one line, one global settings file, every FDK
 * application that did not explicitly install its own theme picks
 * it up on its next launch.
 *
 * It is a thin, honest tool: every mechanism lives in the library
 * (fdk_theme_find / fdk_theme_available_* for discovery, fdk_prefs
 * for storage, the theme-module boot for applying it — see
 * docs/cli.md). The tool only owns POLICY a library should not:
 * validating before writing (never store a name that cannot load),
 * creating the config directory on first run, and speaking exit
 * codes a script can branch on:
 *
 *     0   success
 *     1   usage error, or a named theme that exists but is unusable
 *     2   theme not found
 *     3   the settings could not be saved
 *
 * Library log noise is folded to errors-only here (the tool's own
 * messages are the precise ones a user should read); the same
 * diagnostics stay fully on at INFO/WARN inside applications.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_prefs.h"
#include "fdk/fdk_theme.h"

#include "toolutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#define BUILTIN_NAME "Faded Dream"

/* The toolkit's own reserved store (fdk_prefs.h / docs/cli.md). */
#define GLOBAL_APP_ID "fdk"
#define THEME_KEY "theme.name"

static const char *g_prog = "fdk-theme";

static void usage(FILE *out) {
    fprintf(out,
        "usage: %s <command> [args]\n"
        "\n"
        "  list [--paths]     themes on the search path (the built-in first)\n"
        "  get [--verbose]    the effective theme: $FDK_THEME, else the\n"
        "                     global setting, else the built-in\n"
        "  set <name>         remember <name> in the global settings\n"
        "                     ('%s' means reset)\n"
        "  reset              forget the stored theme\n"
        "  path [<name>]      the file a theme lives in\n"
        "  help               this text\n"
        "  version            tool and library version\n"
        "\n"
        "search path: $FDK_THEME_DIR, $XDG_DATA_HOME/fdk/themes,\n"
        "the $XDG_DATA_DIRS entries' fdk/themes (default\n"
        "/usr/local/share:/usr/share). docs/cli.md has the details.\n"
        "\n"
        "exit codes: 0 ok; 1 usage or unusable theme; 2 not found;\n"
        "3 could not save settings.\n",
        g_prog, BUILTIN_NAME);
}

/* ---- the settings file ------------------------------------------------ */

/* Opens the global store; dies with exit 1 (never a crash) when even
 * that fails. A missing file is a clean first run — the library
 * says FDK_OK and every getter serves its default, which is exactly
 * the semantics this tool wants. */
static fdk_prefs *open_global(void) {
    fdk_prefs *p = NULL;
    fdk_result r = fdk_prefs_open(GLOBAL_APP_ID, &p);
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot open the global settings (%s)\n",
                g_prog, fdk_result_to_string(r));
        exit(1);
    }
    return p;
}

/* Saves `p`, creating the config directory first if the platform
 * layout does not have it yet (first run on a minimal system). */
static int save_global(fdk_prefs *p) {
    const char *path = fdk_prefs_path(p);
    if (path == NULL) {
        fprintf(stderr,
                "%s: no config directory resolves (set $XDG_CONFIG_HOME "
                "or $HOME, or $FDK_PREFS_FILE explicitly)\n",
                g_prog);
        return 3;
    }
    char dir[4096];
    tu_dirname(dir, sizeof dir, path);
    if (tu_mkdir_p(dir) != 0) {
        fprintf(stderr, "%s: cannot create %s: %s\n", g_prog, dir,
                strerror(errno));
        return 3;
    }
    fdk_result r = fdk_prefs_save(p);
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot save %s (%s)\n", g_prog, path,
                fdk_result_to_string(r));
        return 3;
    }
    return 0;
}

/* The name a NEW FDK process would resolve, and where it came from.
 * Mirrors the library boot's precedence exactly (theme.c): env
 * first, then the stored setting, else the built-in. `out_origin`
 * and `out_path` (the settings FILE) are for --verbose diagnostics.
 * When the answer comes from the settings file it is copied into
 * `buf` (the store owns the original and is destroyed before the
 * caller uses the answer); every other return is a static string. */
static const char *effective_theme(char *buf, size_t buflen,
                                    const char **out_origin,
                                    char *settings_buf,
                                    size_t settings_buflen) {
    if (out_origin != NULL) {
        *out_origin = NULL;
    }
    const char *env = getenv("FDK_THEME");
    if (env != NULL && env[0] != '\0') {
        if (out_origin != NULL) {
            *out_origin = "environment ($FDK_THEME)";
        }
        return env;
    }
    fdk_prefs *p = open_global();
    const char *v = fdk_prefs_get(p, THEME_KEY, NULL);
    if (v == NULL || v[0] == '\0') {
        fdk_prefs_destroy(p);
        if (out_origin != NULL) {
            *out_origin = "built-in default (nothing set)";
        }
        return BUILTIN_NAME;
    }
    if (out_origin != NULL) {
        *out_origin = "global settings";
    }
    /* Copy BOTH strings while the store still owns them: the store
     * is destroyed below, and the caller prints them after. Prefs
     * values are bounded at 256 bytes and paths at 4096, so the
     * callers' buffers are comfortably exact. */
    snprintf(buf, buflen, "%s", v);
    if (settings_buf != NULL && settings_buflen > 0) {
        const char *sp = fdk_prefs_path(p);
        snprintf(settings_buf, settings_buflen, "%s",
                 (sp != NULL) ? sp : "");
    }
    fdk_prefs_destroy(p);
    return buf;
}

/* ---- commands --------------------------------------------------------- */

static int cmd_list(bool paths) {
    size_t count = fdk_theme_available_count();

    /* Column width: longest stem, the built-in line included. */
    size_t width = strlen(BUILTIN_NAME);
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(fdk_theme_available_name(i));
        if (n > width) {
            width = n;
        }
    }

    printf("%-*s", (int)width, BUILTIN_NAME);
    printf("%s\n", paths ? "    (built-in default, no file)"
                         : "    (built-in default)");
    for (size_t i = 0; i < count; i++) {
        const char *name = fdk_theme_available_name(i);
        if (paths) {
            printf("%-*s    %s\n", (int)width, name,
                   fdk_theme_available_path(i));
        } else {
            printf("%s\n", name);
        }
    }
    return 0;
}

static int cmd_get(bool verbose) {
    char name[512];
    char settings_path[4096];
    const char *origin = NULL;
    /* Use the RETURN VALUE: when nothing is set, effective_theme()
     * returns the static built-in name and never touches `name`. */
    const char *effective =
        effective_theme(name, sizeof name, &origin, settings_path,
                        sizeof settings_path);
    printf("%s\n", effective);
    if (verbose) {
        fprintf(stderr, "source: %s", origin);
        if (settings_path[0] != '\0') {
            fprintf(stderr, " (%s)", settings_path);
        }
        fprintf(stderr, "\n");
    }
    return 0;
}

static int cmd_set(const char *name) {
    /* The built-in has no file to point at; setting it IS resetting. */
    if (strcasecmp(name, BUILTIN_NAME) == 0) {
        fdk_prefs *p = open_global();
        fdk_prefs_remove(p, THEME_KEY);
        int rc = save_global(p);
        fdk_prefs_destroy(p);
        if (rc != 0) {
            return rc;
        }
        printf("cleared — %s is the built-in theme\n", BUILTIN_NAME);
        return 0;
    }

    /* Validate before writing: a stored name that cannot load would
     * silently no-op every future launch (the library's soft-fail
     * boot is right for apps, wrong for the tool whose whole job is
     * to say "this works"). */
    fdk_result r = FDK_OK;
    fdk_theme *t = fdk_theme_find(name, &r);
    if (t == NULL) {
        if (r == FDK_ERR_NOT_FOUND) {
            fprintf(stderr,
                    "%s: no theme named '%s' on the search path\n"
                    "run '%s list' to see what is installed\n",
                    g_prog, name, g_prog);
            return 2;
        }
        fprintf(stderr,
                "%s: '%s' exists but is not a usable theme (%s)\n",
                g_prog, name, fdk_result_to_string(r));
        return 1;
    }

    fdk_prefs *p = open_global();
    fdk_result wr = fdk_prefs_set(p, THEME_KEY, name);
    if (!fdk_ok(wr)) {
        fprintf(stderr, "%s: cannot store the theme name (%s)\n", g_prog,
                fdk_result_to_string(wr));
        fdk_theme_destroy(t);
        fdk_prefs_destroy(p);
        return 1;
    }
    int rc = save_global(p);
    fdk_prefs_destroy(p);
    if (rc != 0) {
        fdk_theme_destroy(t);
        return rc;
    }

    printf("theme set: %s (%s)\n", fdk_theme_name(t),
           fdk_theme_file_path(t) != NULL ? fdk_theme_file_path(t)
                                          : "(built-in)");
    printf("running FDK applications re-theme the moment this lands "
           "(1.4.14: they watch the settings file)\n");
    fdk_theme_destroy(t);
    return 0;
}

static int cmd_reset(void) {
    fdk_prefs *p = open_global();
    fdk_prefs_remove(p, THEME_KEY);
    int rc = save_global(p);
    fdk_prefs_destroy(p);
    if (rc != 0) {
        return rc;
    }
    printf("theme setting cleared — back to %s (built-in)\n",
           BUILTIN_NAME);
    return 0;
}

static int cmd_path(const char *name_arg) {
    char name_buf[512];
    const char *name = name_arg;
    if (name == NULL) {
        name = effective_theme(name_buf, sizeof name_buf, NULL, NULL, 0);
    }
    if (strcasecmp(name, BUILTIN_NAME) == 0) {
        printf("(built-in)\n");
        return 0;
    }
    fdk_result r = FDK_OK;
    fdk_theme *t = fdk_theme_find(name, &r);
    if (t == NULL) {
        if (r == FDK_ERR_NOT_FOUND) {
            fprintf(stderr, "%s: no theme named '%s'\n", g_prog, name);
            return 2;
        }
        fprintf(stderr, "%s: '%s' is not a usable theme (%s)\n", g_prog,
                name, fdk_result_to_string(r));
        return 1;
    }
    const char *file = fdk_theme_file_path(t);
    if (file == NULL) {
        /* Unreachable today (find only returns loaded themes), kept
         * honest rather than crashing if that ever changes. */
        printf("(built-in)\n");
    } else {
        printf("%s\n", file);
    }
    fdk_theme_destroy(t);
    return 0;
}

int main(int argc, char **argv) {
    g_prog = (argc > 0 && argv[0] != NULL && argv[0][0] != '\0')
                 ? argv[0]
                 : "fdk-theme";
    const char *dash = strrchr(g_prog, '/');
    if (dash != NULL) {
        g_prog = dash + 1;
    }

    /* Tool output stays clean: the library's INFO/WARN lines become
     * this tool's own precise messages (they remain fully on inside
     * real applications). */
    fdk_log_set_level(FDK_LOG_ERROR);

    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-V") == 0) {
        printf("fdk-theme (FDK) %s\n", fdk_get_version_string());
        return 0;
    }

    if (strcmp(cmd, "list") == 0) {
        bool paths = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--paths") == 0) {
                paths = true;
            } else {
                fprintf(stderr, "%s: list takes no such argument: %s\n",
                        g_prog, argv[i]);
                return 1;
            }
        }
        return cmd_list(paths);
    }

    if (strcmp(cmd, "get") == 0) {
        bool verbose = false;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0 ||
                strcmp(argv[i], "-v") == 0) {
                verbose = true;
            } else {
                fprintf(stderr, "%s: get takes no such argument: %s\n",
                        g_prog, argv[i]);
                return 1;
            }
        }
        return cmd_get(verbose);
    }

    if (strcmp(cmd, "set") == 0) {
        if (argc != 3) {
            fprintf(stderr, "%s: set takes exactly one theme name\n",
                    g_prog);
            return 1;
        }
        return cmd_set(argv[2]);
    }

    if (strcmp(cmd, "reset") == 0) {
        if (argc != 2) {
            fprintf(stderr, "%s: reset takes no arguments\n", g_prog);
            return 1;
        }
        return cmd_reset();
    }

    if (strcmp(cmd, "path") == 0) {
        if (argc > 3) {
            fprintf(stderr, "%s: path takes at most one theme name\n",
                    g_prog);
            return 1;
        }
        return cmd_path((argc == 3) ? argv[2] : NULL);
    }

    fprintf(stderr, "%s: unknown command '%s'\n\n", g_prog, cmd);
    usage(stderr);
    return 1;
}
