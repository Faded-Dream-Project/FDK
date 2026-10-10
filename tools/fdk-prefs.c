/*
 * fdk-prefs.c — the generic settings face of the FDK command line
 * (1.4.13)
 *
 *     fdk-prefs list                  every key with its value
 *     fdk-prefs get theme.name        one value (exit 2 if absent)
 *     fdk-prefs set window.width 940  write one key, typed
 *     fdk-prefs remove window.width   drop one key
 *     fdk-prefs path                  where the store lives
 *
 * A preferences library without a way to inspect it from a shell is
 * a black box the moment something goes wrong ("what did it
 * actually save?"), and an application's settings are legitimately
 * its user's settings — the person who wants to peek or fix a value
 * without launching the app. gsettings plays exactly this role for
 * GTK; this is FDK's.
 *
 * By default the tool operates on the TOOLKIT's own store (the
 * reserved app id "fdk" — the file fdk-theme writes); -a/--app aims
 * it at any application's store instead, because the format is the
 * same and the file layout is the same. It never edits around the
 * library: every read and write goes through fdk_prefs, so the
 * grammar, the atomic temp-file rename, and the fail-soft posture
 * are the library's, not a reimplementation's.
 *
 * `set` types its argument the way the file format means it:
 * true/false -> bool, integers -> int, decimals -> double, anything
 * else -> string. The store canonicalizes ("1" never becomes
 * "1.000000"); values round-trip bit-for-bit, which the prefs test
 * suite already pins.
 *
 * exit codes: 0 ok; 1 usage or invalid key/app id; 2 no such key
 * (get); 3 the store cannot be saved (no config directory).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_prefs.h"

#include "toolutil.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_prog = "fdk-prefs";

static void usage(FILE *out) {
    fprintf(out,
        "usage: %s <command> [args] [options]\n"
        "\n"
        "  list               every key with its value, in file order\n"
        "  get <key>          print one value (exit 2 when absent;\n"
        "                     --default <str> prints <str> instead)\n"
        "  set <key> <value>  write one key (type auto-detected:\n"
        "                     bool, integer, decimal, else string)\n"
        "  remove <key>       drop one key (absent key: quiet no-op)\n"
        "  path               the store's file path\n"
        "  help               this text\n"
        "  version            tool and library version\n"
        "\n"
        "options:\n"
        "  -a, --app <id>     which application's store (default: fdk,\n"
        "                     the toolkit's own)\n"
        "      --default <s>  get: value to print when the key is absent\n"
        "\n"
        "store location: $FDK_PREFS_FILE, else\n"
        "$XDG_CONFIG_HOME/<app>.prefs, else ~/.config/<app>.prefs.\n"
        "docs/cli.md documents the file format.\n",
        g_prog);
}

/* ---- the store -------------------------------------------------------- */

static fdk_prefs *open_store(const char *app_id) {
    fdk_prefs *p = NULL;
    fdk_result r = fdk_prefs_open(app_id, &p);
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot open the '%s' store (%s)\n", g_prog,
                app_id, fdk_result_to_string(r));
        exit(1);
    }
    return p;
}

/* Same first-run policy as fdk-theme: create the config directory a
 * fresh system does not have yet, then save atomically. */
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

/* ---- typed `set` ------------------------------------------------------ */

/* A prefs key is 'section.name' (one dot, both halves non-empty —
 * fdk_prefs.h). The library's setters enforce the full grammar, but
 * its getters hold the never-an-error contract, so a malformed key
 * on get/remove would read as "absent" — a misleading answer. The
 * tool checks the SHAPE itself and says what is wrong. */
static bool key_shape_ok(const char *key) {
    if (key == NULL) {
        return false;
    }
    const char *dot = strchr(key, '.');
    if (dot == NULL || dot == key || dot[1] == '\0') {
        return false;
    }
    if (strchr(dot + 1, '.') != NULL) {
        return false; /* deeper nesting is not the prefs model */
    }
    return true;
}

static int key_shape_fail(const char *key) {
    fprintf(stderr,
            "%s: '%s' is not a prefs key — keys are 'section.name' "
            "(one dot, both halves non-empty; e.g. window.width)\n",
            g_prog, key);
    return 1;
}

static bool looks_bool(const char *v) {
    return strcmp(v, "true") == 0 || strcmp(v, "false") == 0;
}

static bool looks_int(const char *v) {
    if (*v == '\0') {
        return false;
    }
    const char *p = (*v == '-' || *v == '+') ? v + 1 : v;
    if (*p == '\0') {
        return false;
    }
    for (; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

static bool looks_double(const char *v) {
    if (*v == '\0') {
        return false;
    }
    char *end = NULL;
    (void)strtod(v, &end);
    if (end == NULL || *end != '\0') {
        return false;
    }
    /* strtod also accepts plain integers; only classify as double
     * when there is a decimal point or exponent — the value the user
     * typed, not just any parsable number. */
    return strpbrk(v, ".eE") != NULL;
}

/* ---- commands --------------------------------------------------------- */

static int cmd_list(fdk_prefs *p) {
    size_t n = fdk_prefs_count(p);
    for (size_t i = 0; i < n; i++) {
        printf("%s = %s\n", fdk_prefs_key_at(p, i),
               fdk_prefs_value_at(p, i));
    }
    return 0;
}

static int cmd_get(fdk_prefs *p, const char *key, const char *def) {
    if (!key_shape_ok(key)) {
        return key_shape_fail(key);
    }
    /* Present-but-unparsable values are the STORE's business; get
     * returns the raw canonical string either way (the "never an
     * error, always an answer" contract). */
    const char *v = fdk_prefs_get(p, key, NULL);
    if (v == NULL) {
        if (def != NULL) {
            printf("%s\n", def);
            return 0;
        }
        fprintf(stderr, "%s: no key '%s'\n", g_prog, key);
        return 2;
    }
    printf("%s\n", v);
    return 0;
}

static int cmd_set(fdk_prefs *p, const char *key, const char *value) {
    if (!key_shape_ok(key)) {
        return key_shape_fail(key);
    }
    fdk_result r;
    if (looks_bool(value)) {
        r = fdk_prefs_set_bool(p, key, strcmp(value, "true") == 0);
    } else if (looks_int(value)) {
        r = fdk_prefs_set_int(p, key, strtol(value, NULL, 10));
    } else if (looks_double(value)) {
        r = fdk_prefs_set_double(p, key, strtod(value, NULL));
    } else {
        r = fdk_prefs_set(p, key, value);
    }
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot set '%s' (%s)\n", g_prog, key,
                fdk_result_to_string(r));
        return 1;
    }
    return save_store(p); /* silent on success: scriptable */
}

static int cmd_remove(fdk_prefs *p, const char *key) {
    if (!key_shape_ok(key)) {
        return key_shape_fail(key);
    }
    fdk_result r = fdk_prefs_remove(p, key);
    if (!fdk_ok(r)) {
        fprintf(stderr, "%s: cannot remove '%s' (%s)\n", g_prog, key,
                fdk_result_to_string(r));
        return 1;
    }
    return save_store(p);
}

static int cmd_path(fdk_prefs *p) {
    const char *path = fdk_prefs_path(p);
    if (path == NULL) {
        printf("(memory-only: no config directory resolves)\n");
        return 3;
    }
    printf("%s\n", path);
    return 0;
}

int main(int argc, char **argv) {
    g_prog = (argc > 0 && argv[0] != NULL && argv[0][0] != '\0')
                 ? argv[0]
                 : "fdk-prefs";
    const char *dash = strrchr(g_prog, '/');
    if (dash != NULL) {
        g_prog = dash + 1;
    }

    fdk_log_set_level(FDK_LOG_ERROR);

    const char *app_id = "fdk";
    const char *def = NULL;
    const char *cmd = NULL;
    const char *positional[2] = {NULL, NULL};
    int npos = 0;
    bool want_cmd = true;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (want_cmd && arg[0] != '-') {
            cmd = arg;
            want_cmd = false;
            continue;
        }
        /* Help/version may appear where a command would (before any
         * command word): dispatch them as pseudo-commands instead of
         * falling through to "unexpected argument". */
        if (cmd == NULL && (strcmp(arg, "--help") == 0 ||
                            strcmp(arg, "-h") == 0)) {
            cmd = "help";
            want_cmd = false;
            continue;
        }
        if (cmd == NULL && (strcmp(arg, "--version") == 0 ||
                            strcmp(arg, "-V") == 0)) {
            cmd = "version";
            want_cmd = false;
            continue;
        }
        if (strcmp(arg, "-a") == 0 || strcmp(arg, "--app") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: %s needs an application id\n", g_prog,
                        arg);
                return 1;
            }
            app_id = argv[++i];
            continue;
        }
        if (strcmp(arg, "--default") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --default needs a value\n", g_prog);
                return 1;
            }
            def = argv[++i];
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            /* Everything after -- is positional (values that look
             * like flags: a string pref of "-a", a negative number
             * already handled above only when leading). */
            for (i++; i < argc; i++) {
                if (npos < 2) {
                    positional[npos++] = argv[i];
                } else {
                    fprintf(stderr, "%s: too many arguments\n", g_prog);
                    return 1;
                }
            }
            break;
        }
        if (cmd != NULL && npos < 2) {
            positional[npos++] = arg;
            continue;
        }
        fprintf(stderr, "%s: unexpected argument '%s'\n", g_prog, arg);
        return 1;
    }

    if (cmd == NULL) {
        usage(stderr);
        return 1;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 ||
        strcmp(cmd, "-V") == 0) {
        printf("fdk-prefs (FDK) %s\n", fdk_get_version_string());
        return 0;
    }

    if (app_id == NULL || app_id[0] == '\0') {
        fprintf(stderr, "%s: the application id must not be empty\n",
                g_prog);
        return 1;
    }

    fdk_prefs *p = open_store(app_id);
    int rc;
    if (strcmp(cmd, "list") == 0) {
        if (npos != 0) {
            fprintf(stderr, "%s: list takes no arguments\n", g_prog);
            rc = 1;
        } else {
            rc = cmd_list(p);
        }
    } else if (strcmp(cmd, "get") == 0) {
        if (npos != 1) {
            fprintf(stderr, "%s: get takes exactly one key\n", g_prog);
            rc = 1;
        } else {
            rc = cmd_get(p, positional[0], def);
        }
    } else if (strcmp(cmd, "set") == 0) {
        if (npos != 2) {
            fprintf(stderr, "%s: set takes a key and a value\n", g_prog);
            rc = 1;
        } else {
            rc = cmd_set(p, positional[0], positional[1]);
        }
    } else if (strcmp(cmd, "remove") == 0) {
        if (npos != 1) {
            fprintf(stderr, "%s: remove takes exactly one key\n", g_prog);
            rc = 1;
        } else {
            rc = cmd_remove(p, positional[0]);
        }
    } else if (strcmp(cmd, "path") == 0) {
        if (npos != 0) {
            fprintf(stderr, "%s: path takes no arguments\n", g_prog);
            rc = 1;
        } else {
            rc = cmd_path(p);
        }
    } else {
        fprintf(stderr, "%s: unknown command '%s'\n\n", g_prog, cmd);
        usage(stderr);
        rc = 1;
    }
    fdk_prefs_destroy(p);
    return rc;
}
