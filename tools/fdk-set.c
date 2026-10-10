/*
 * fdk-set.c — the per-application settings face of the FDK command
 * line (1.4.14)
 *
 *     fdk-set theme set "matrix"                 the global default
 *                                                 (same file fdk-theme
 *                                                 writes — the tools
 *                                                 are interoperable)
 *     fdk-set theme set "matrix" --app org.fdk.example09
 *                                               ONE application's own
 *                                               theme: it wins over
 *                                               the global setting
 *                                               for that app only
 *     fdk-set theme get [--app APP] [--verbose]  what that app (or a
 *                                                 new one) would use
 *     fdk-set theme reset [--app APP]            forget the override
 *     fdk-set theme list [--paths]               what is installed
 *
 * Why this tool exists: the 1.4.13 CLI made the DESKTOP rethemable
 * from the shell (fdk-theme set — every FDK app, launch or live);
 * this is the other half real desktops grow — per-application
 * preferences for the app whose brand wants one theme while the
 * desktop's default is another (Firefox keeping dark while the
 * desktop is light, the IDE keeping its own). The store is the
 * application's OWN <app_id>.prefs file — the exact file the app's
 * fdk_init identity resolves to — so the override is also readable
 * and editable by hand, and `fdk-set theme set X --app A` +
 * `fdk-set theme reset --app A` round-trip cleanly.
 *
 * It is a thin, honest tool like its siblings: every mechanism lives
 * in the library (the settings engine's resolution order — env, then
 * the app's store, then the global store, then the built-in — and
 * the inotify live follow that re-themes the running application the
 * moment the file lands). The tool only owns POLICY a library should
 * not: validating before writing (never store a name that cannot
 * load), creating the config directory on first run, and speaking
 * exit codes a script can branch on:
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

#define BUILTIN_NAME "FDK Modern"

/* The toolkit's own reserved store (fdk_prefs.h / docs/cli.md). */
#define GLOBAL_APP_ID "fdk"
#define THEME_KEY "theme.name"

static const char *g_prog = "fdk-set";

static void usage(FILE *out) {
    fprintf(out,
        "usage: %s <group> <command> [args]\n"
        "\n"
        "  theme set <name> [--app <app-id>]\n"
        "                     remember <name> globally, or just for the\n"
        "                     application <app-id> (its fdk_init identity;\n"
        "                     '%s' means the built-in)\n"
        "  theme get [--app <app-id>] [--verbose]\n"
        "                     what that app (or a new one) would use\n"
        "  theme reset [--app <app-id>]\n"
        "                     forget the global setting, or the app's\n"
        "  theme list [--paths]\n"
        "                     themes on the search path (the built-in first)\n"
        "  help               this text\n"
        "  version            tool and library version\n"
        "\n"
        "precedence: $FDK_THEME, then the app's own store, then the\n"
        "global store, then the built-in. Running FDK applications\n"
        "follow these files live (they watch them).\n"
        "\n"
        "search path: $FDK_THEME_DIR, $XDG_DATA_HOME/fdk/themes,\n"
        "the $XDG_DATA_DIRS entries' fdk/themes (default\n"
        "/usr/local/share:/usr/share). docs/cli.md has the details.\n"
        "\n"
        "exit codes: 0 ok; 1 usage or unusable theme; 2 not found;\n"
        "3 could not save settings.\n",
        g_prog, BUILTIN_NAME);
}

/* ---- the stores -------------------------------------------------------- */

/* Opens the store `app_id` names (the global "fdk" store, or an
 * application's own); dies with exit 1 when even that fails. */
static fdk_prefs *open_store(const char *app_id) {
    fdk_prefs *p = NULL;
    fdk_result r = fdk_prefs_open(app_id, &p);
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot open the settings for '%s' (%s)\n",
                g_prog, app_id, fdk_result_to_string(r));
        exit(1);
    }
    return p;
}

/* Saves `p`, creating the config directory first if the platform
 * layout does not have it yet (first run on a minimal system). */
static int save_store(fdk_prefs *p) {
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

/* Validates a theme name the way fdk-theme does (never store a name
 * that cannot load), returning 0 ok / 1 unusable / 2 not found. */
static int validate_theme(const char *name, fdk_theme **out_keep) {
    fdk_result r = FDK_OK;
    fdk_theme *t = fdk_theme_find(name, &r);
    if (t == NULL) {
        if (r == FDK_ERR_NOT_FOUND) {
            fprintf(stderr,
                    "%s: no theme named '%s' on the search path\n"
                    "run '%s theme list' to see what is installed\n",
                    g_prog, name, g_prog);
            return 2;
        }
        fprintf(stderr,
                "%s: '%s' exists but is not a usable theme (%s)\n",
                g_prog, name, fdk_result_to_string(r));
        return 1;
    }
    *out_keep = t;
    return 0;
}

/* The name the STORE `app_id` resolves for its process — env first
 * (the one override no store can beat), then the app's own store,
 * then the global store, else the built-in. Mirrors the library's
 * resolution order exactly (theme/settings.c); `get` is the tool that
 * TELLS the user what a running app sees. When the answer comes from
 * a store it is copied into `buf` (the store is closed before the
 * caller prints); every other return is a static string. */
static const char *effective_for(char *buf, size_t buflen,
                                 const char *app_id,
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
    if (app_id != NULL) {
        fdk_prefs *p = open_store(app_id);
        const char *v = fdk_prefs_get(p, THEME_KEY, NULL);
        if (v != NULL && v[0] != '\0') {
            snprintf(buf, buflen, "%s", v);
            if (settings_buf != NULL && settings_buflen > 0) {
                const char *sp = fdk_prefs_path(p);
                snprintf(settings_buf, settings_buflen, "%s",
                         (sp != NULL) ? sp : "");
            }
            fdk_prefs_destroy(p);
            if (out_origin != NULL) {
                *out_origin = "the application's settings";
            }
            return buf;
        }
        fdk_prefs_destroy(p);
    }
    fdk_prefs *p = open_store(GLOBAL_APP_ID);
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
    snprintf(buf, buflen, "%s", v);
    if (settings_buf != NULL && settings_buflen > 0) {
        const char *sp = fdk_prefs_path(p);
        snprintf(settings_buf, settings_buflen, "%s",
                 (sp != NULL) ? sp : "");
    }
    fdk_prefs_destroy(p);
    return buf;
}

/* ---- the theme group --------------------------------------------------- */

static int cmd_theme_list(bool paths) {
    size_t count = fdk_theme_available_count();
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

static int cmd_theme_get(const char *app_id, bool verbose) {
    char name[512];
    char settings_path[4096];
    const char *origin = NULL;
    const char *effective =
        effective_for(name, sizeof name, app_id, &origin, settings_path,
                      sizeof settings_path);
    if (app_id != NULL) {
        printf("%s\n", effective);
    } else {
        printf("%s\n", effective);
    }
    if (verbose) {
        fprintf(stderr, "source: %s", origin);
        if (settings_path[0] != '\0') {
            fprintf(stderr, " (%s)", settings_path);
        }
        if (app_id != NULL) {
            fprintf(stderr, " [app %s]", app_id);
        }
        fprintf(stderr, "\n");
    }
    return 0;
}

static int cmd_theme_set(const char *name, const char *app_id) {
    const char *store_id = (app_id != NULL) ? app_id : GLOBAL_APP_ID;

    /* The built-in has no file to point at; setting it IS resetting. */
    if (strcasecmp(name, BUILTIN_NAME) == 0) {
        fdk_prefs *p = open_store(store_id);
        fdk_prefs_remove(p, THEME_KEY);
        int rc = save_store(p);
        fdk_prefs_destroy(p);
        if (rc != 0) {
            return rc;
        }
        printf("cleared — %s is the built-in theme%s\n", BUILTIN_NAME,
               (app_id != NULL) ? " (app override removed)" : "");
        return 0;
    }

    /* Validate before writing (the fdk-theme policy: a stored name
     * that cannot load would silently no-op every future launch). */
    fdk_theme *t = NULL;
    int vrc = validate_theme(name, &t);
    if (vrc != 0) {
        return vrc;
    }

    fdk_prefs *p = open_store(store_id);
    fdk_result wr = fdk_prefs_set(p, THEME_KEY, name);
    if (!fdk_ok(wr)) {
        fprintf(stderr, "%s: cannot store the theme name (%s)\n", g_prog,
                fdk_result_to_string(wr));
        fdk_theme_destroy(t);
        fdk_prefs_destroy(p);
        return 1;
    }
    int rc = save_store(p);
    fdk_prefs_destroy(p);
    if (rc != 0) {
        fdk_theme_destroy(t);
        return rc;
    }

    printf("theme set: %s (%s)\n", fdk_theme_name(t),
           fdk_theme_file_path(t) != NULL ? fdk_theme_file_path(t)
                                          : "(built-in)");
    if (app_id != NULL) {
        printf("%s re-themes the moment this lands (it watches its "
               "own settings file)\n",
               app_id);
    } else {
        printf("running FDK applications re-theme the moment this "
               "lands (they watch the settings file)\n");
    }
    fdk_theme_destroy(t);
    return 0;
}

static int cmd_theme_reset(const char *app_id) {
    const char *store_id = (app_id != NULL) ? app_id : GLOBAL_APP_ID;
    fdk_prefs *p = open_store(store_id);
    fdk_prefs_remove(p, THEME_KEY);
    int rc = save_store(p);
    fdk_prefs_destroy(p);
    if (rc != 0) {
        return rc;
    }
    if (app_id != NULL) {
        printf("app theme setting cleared — %s now follows the global "
               "setting\n",
               app_id);
    } else {
        printf("theme setting cleared — back to %s (built-in)\n",
               BUILTIN_NAME);
    }
    return 0;
}

/* ---- argument parsing --------------------------------------------------- */

/* Parsed shape of one invocation. Exactly one of the command enums is
 * set; app_id is the --app value or NULL. */
typedef enum {
    CMD_NONE = 0,
    CMD_LIST,
    CMD_GET,
    CMD_SET,
    CMD_RESET,
} cmd_kind;

int main(int argc, char **argv) {
    g_prog = (argc > 0 && argv[0] != NULL && argv[0][0] != '\0')
                 ? argv[0]
                 : "fdk-set";
    const char *dash = strrchr(g_prog, '/');
    if (dash != NULL) {
        g_prog = dash + 1;
    }

    fdk_log_set_level(FDK_LOG_ERROR);

    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    const char *group = argv[1];
    if (strcmp(group, "help") == 0 || strcmp(group, "--help") == 0 ||
        strcmp(group, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(group, "version") == 0 || strcmp(group, "--version") == 0 ||
        strcmp(group, "-V") == 0) {
        printf("fdk-set (FDK) %s\n", fdk_get_version_string());
        return 0;
    }
    if (strcmp(group, "theme") != 0) {
        fprintf(stderr, "%s: unknown group '%s' (groups: theme)\n\n",
                g_prog, group);
        usage(stderr);
        return 1;
    }
    if (argc < 3) {
        fprintf(stderr, "%s: the 'theme' group needs a command "
                        "(set/get/reset/list)\n\n",
                g_prog);
        usage(stderr);
        return 1;
    }

    const char *cmd = argv[2];
    cmd_kind kind = CMD_NONE;
    const char *name = NULL;
    const char *app_id = NULL;
    bool paths = false;
    bool verbose = false;
    bool bad = false;

    if (strcmp(cmd, "list") == 0) {
        kind = CMD_LIST;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--paths") == 0) {
                paths = true;
            } else {
                fprintf(stderr, "%s: list takes no such argument: %s\n",
                        g_prog, argv[i]);
                bad = true;
            }
        }
    } else if (strcmp(cmd, "get") == 0) {
        kind = CMD_GET;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0 ||
                strcmp(argv[i], "-v") == 0) {
                verbose = true;
            } else if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
                app_id = argv[++i];
            } else {
                fprintf(stderr, "%s: get takes no such argument: %s\n",
                        g_prog, argv[i]);
                bad = true;
            }
        }
    } else if (strcmp(cmd, "set") == 0) {
        kind = CMD_SET;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
                app_id = argv[++i];
            } else if (name == NULL) {
                name = argv[i];
            } else {
                fprintf(stderr, "%s: set takes exactly one theme name\n",
                        g_prog);
                bad = true;
            }
        }
        if (name == NULL) {
            fprintf(stderr, "%s: set needs a theme name\n", g_prog);
            bad = true;
        }
    } else if (strcmp(cmd, "reset") == 0) {
        kind = CMD_RESET;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--app") == 0 && i + 1 < argc) {
                app_id = argv[++i];
            } else {
                fprintf(stderr, "%s: reset takes no such argument: %s\n",
                        g_prog, argv[i]);
                bad = true;
            }
        }
    } else {
        fprintf(stderr, "%s: unknown theme command '%s'\n\n", g_prog,
                cmd);
        usage(stderr);
        return 1;
    }

    if (bad) {
        return 1;
    }
    if (app_id != NULL && app_id[0] == '\0') {
        fprintf(stderr, "%s: --app needs a non-empty application id\n",
                g_prog);
        return 1;
    }

    switch (kind) {
    case CMD_LIST:
        return cmd_theme_list(paths);
    case CMD_GET:
        return cmd_theme_get(app_id, verbose);
    case CMD_SET:
        return cmd_theme_set(name, app_id);
    case CMD_RESET:
        return cmd_theme_reset(app_id);
    default:
        usage(stderr);
        return 1;
    }
}
