#define FDK_LOG_TAG "theme"

/*
 * settings.c — the theme-settings engine: resolution, the developer
 * whitelist, and the live follow (1.4.14)
 *
 * 1.4.13 gave FDK one toolkit-level preference — theme.name in the
 * reserved "fdk" store — applied ONCE per process, lazily, at the
 * first resolution of the current default theme. This file is that
 * idea grown into its final shape, driven by how real desktops
 * actually use it:
 *
 *   RESOLUTION (shared by the boot and every live recheck):
 *
 *     1. $FDK_THEME            per-process override (GTK_THEME
 *                              precedent — theming one launch)
 *     2. theme.name in the APPLICATION's own store
 *                              (the <app_id>.prefs file — the
 *                              per-app override `fdk-set theme set
 *                              NAME --app APP` writes; the app_id
 *                              comes from fdk_init's options)
 *     3. theme.name in the "fdk" store
 *                              (the global default `fdk-theme set`
 *                              and `fdk-set theme set NAME` write)
 *     4. the built-in theme, quietly
 *
 *   THE WHITELIST (the developer's side of the contract): an
 *   application that wants its brand — and only its brand — calls
 *   fdk_theme_set_allowed_themes() and every settings source above
 *   is clamped to the list: a resolved name outside the list falls
 *   back to the first allowed theme, and "nothing set anywhere"
 *   means the first allowed theme instead of the built-in. The
 *   restriction governs SETTINGS, not code: the developer's own
 *   fdk_theme_set_default() calls are always honored (and, as
 *   always, opt the process out of settings-following entirely).
 *
 *   THE LIVE FOLLOW: while a display context runs, the settings
 *   FILES are watched (inotify on their directories — atomic
 *   temp+rename saves surface as IN_MOVED_TO on the dir, hand edits
 *   as IN_CLOSE_WRITE). When a watched file changes, the resolution
 *   re-runs; a NEW theme installs through the same repaint-all path
 *   fdk_theme_set_default() uses, so every running FDK application
 *   re-themes itself the moment `fdk-theme set` lands. Applications
 *   that installed their own theme opted out and are left alone;
 *   the watch is torn down the first time that is detected.
 *
 * Failure posture everywhere: soft. A corrupt store, an unreadable
 * directory, a name that no longer resolves — one warning, the
 * current theme stays, the launch (and the recheck) continues. A
 * themed launch must never be a failed launch; neither must a
 * re-theme of a running one.
 *
 * Layering: this file sits in the theme module and speaks the public
 * prefs API (fdk_prefs_open/get) plus one core-internal path
 * resolver (fdk__prefs_resolve_path, prefs_internal.h) for the
 * watch. The context/window layers bridge to it through
 * window_internal.h declarations implemented in window.c (the
 * sanctioned core->window->theme path — see docs/architecture.md).
 */

#include "theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "core/prefs_internal.h"
#include "fdk/fdk_prefs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

/* ---- tiny path helper --------------------------------------------------- */

/* "a/b.prefs" -> ("a", "b.prefs"); "/x" -> ("/", "x"); "x" -> (".", "x").
 * Bounded copies; truncation is defensive only (paths here live in
 * PATH_MAX-ish buffers upstream). */
static void split_dir_base(const char *path, char *dir, size_t dir_cap,
                           char *base, size_t base_cap) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(dir, dir_cap, ".");
        snprintf(base, base_cap, "%s", path);
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0) {
        snprintf(dir, dir_cap, "/");
    } else {
        if (n >= dir_cap) {
            n = dir_cap - 1;
        }
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    snprintf(base, base_cap, "%s", slash + 1);
}

/* ---- state ---------------------------------------------------------------- */

/* Set from fdk_init (options->app_id): the per-app store's identity.
 * NULL until an application with an identity initializes — processes
 * without one (tools, tests) simply have no per-app leg. */
static char *g_app_id;

/* The theme the SETTINGS machinery installed (boot or recheck), owned
 * here for its lifetime or until superseded. NULL while the built-in
 * (or an app-owned theme) is current. */
static fdk_theme *g_installed;
static char g_installed_name[FDK_THEME_STRING_MAX];

/* Set once the boot has run (mirrors theme.c's g_booted guard). */
static bool g_boot_ran;

/* The developer whitelist (owned copies). Empty = no restriction. */
static char **g_allowed;
static size_t g_allowed_count;

/* The live watch (inotify on the settings files' directories). */
static int g_watch_fd = -1;
static char g_dir_global[4096];   /* dirname of the global store  */
static char g_dir_app[4096];      /* dirname of the app store     */
static char g_base_global[128];   /* "fdk.prefs"                  */
static char g_base_app[128];      /* "<app_id>.prefs"             */

/* ---- the developer whitelist (public API) -------------------------------- */

static bool allowed_matches(const char *name) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < g_allowed_count; i++) {
        if (strcmp(g_allowed[i], name) == 0) {
            return true;
        }
    }
    return false;
}

size_t fdk_theme_allowed_count(void) {
    return g_allowed_count;
}

const char *fdk_theme_allowed_name(size_t index) {
    if (index >= g_allowed_count) {
        return NULL;
    }
    return g_allowed[index];
}

fdk_result fdk_theme_set_allowed_themes(const char *const *names,
                                        size_t count) {
    if (count == 0) {
        fdk_theme_clear_allowed_themes();
        return FDK_OK;
    }
    if (names == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char **copies = fdk_alloc_array(count, sizeof(char *));
    if (copies == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        if (names[i] == NULL || names[i][0] == '\0' ||
            strlen(names[i]) >= FDK_THEME_STRING_MAX) {
            fdk__theme_settings_free_list(copies, i);
            return FDK_ERR_INVALID_ARGUMENT;
        }
        copies[i] = fdk__theme_strdup(names[i]);
        if (copies[i] == NULL) {
            fdk__theme_settings_free_list(copies, i);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    fdk__theme_settings_free_list(g_allowed, g_allowed_count);
    g_allowed = copies;
    g_allowed_count = count;
    /* A restriction arriving after something was already applied is
     * honored immediately: re-run the clamp against the new list. */
    fdk__theme_settings_recheck();
    return FDK_OK;
}

void fdk_theme_clear_allowed_themes(void) {
    fdk__theme_settings_free_list(g_allowed, g_allowed_count);
    g_allowed = NULL;
    g_allowed_count = 0;
}

/* ---- resolution ------------------------------------------------------------ */

/* Reads theme.name out of the store `app_id` resolves to, WITHOUT
 * keeping the store open (the read-only peek the resolution and the
 * tools share). Empty/absent/corrupt all mean "no preference" — the
 * prefs resilience rule, inherited wholesale. */
static char *peek_store_theme(const char *app_id) {
    fdk_prefs *p = NULL;
    fdk_result r = fdk_prefs_open(app_id, &p);
    if (!fdk_ok(r) || p == NULL) {
        return NULL;
    }
    const char *v = fdk_prefs_get(p, "theme.name", NULL);
    char *copy = NULL;
    if (v != NULL && v[0] != '\0' &&
        strlen(v) < sizeof g_installed_name) {
        copy = fdk__theme_strdup(v);
    }
    fdk_prefs_destroy(p);
    return copy;
}

/* One resolution pass. Returns an OWNED theme to install, or NULL
 * when nothing should be (nothing set / unloadable / whitelist
 * fallback itself unloadable — all soft, all warned once). */
static fdk_theme *resolve_theme(char *name_out, size_t name_cap,
                                const char **origin_out) {
    name_out[0] = '\0';
    if (origin_out != NULL) {
        *origin_out = NULL;
    }

    const char *name = NULL;
    const char *origin = NULL;
    char *owned = NULL; /* the one allocation all store legs share */

    const char *env = getenv("FDK_THEME");
    if (env != NULL && env[0] != '\0') {
        name = env;
        origin = "environment ($FDK_THEME)";
    }
    if (name == NULL && g_app_id != NULL) {
        owned = peek_store_theme(g_app_id);
        if (owned != NULL) {
            name = owned;
            origin = "the application's settings";
        }
    }
    if (name == NULL) {
        owned = peek_store_theme("fdk");
        if (owned != NULL) {
            name = owned;
            origin = "the global settings file";
        }
    }

    /* The whitelist clamp: whatever settings said, the application's
     * declared brand wins. The match runs against the resolved NAME
     * and, when that fails, against the loaded theme's INTERNAL name
     * (a user setting the stem "daylight" of a theme the developer
     * allowed as "Daylight" must not be rejected for spelling it the
     * way `fdk-theme list` prints it). */
    if (g_allowed_count > 0) {
        if (name == NULL) {
            owned = fdk__theme_strdup(g_allowed[0]);
            name = owned;
            origin = "the application's theme whitelist";
        } else if (!allowed_matches(name)) {
            fdk_result pr = FDK_OK;
            fdk_theme *probe = fdk_theme_find(name, &pr);
            bool ok = false;
            if (probe != NULL) {
                ok = allowed_matches(fdk_theme_name(probe));
                fdk_theme_destroy(probe);
            }
            if (!ok) {
                FDK_INFO("theme: '%s' (%s) is not one of this "
                         "application's allowed themes; using '%s'",
                         name, origin, g_allowed[0]);
                fdk_free(owned);
                owned = fdk__theme_strdup(g_allowed[0]);
                name = owned;
                origin = "the application's theme whitelist";
            }
        }
    }

    if (name == NULL) {
        return NULL; /* no preference anywhere: the built-in stays */
    }

    fdk_result r = FDK_OK;
    fdk_theme *t = fdk_theme_find(name, &r);
    if (t == NULL) {
        if (r != FDK_ERR_NOT_FOUND) {
            FDK_WARN("theme: '%s' (%s) failed to load (error %d); "
                     "staying on the current theme",
                     name, origin, (int)r);
        } else {
            FDK_WARN("theme: '%s' (%s) is not on the theme search "
                     "path; staying on the current theme",
                     name, origin);
        }
        fdk_free(owned);
        return NULL;
    }
    snprintf(name_out, name_cap, "%s", fdk_theme_name(t));
    if (origin_out != NULL) {
        *origin_out = origin;
    }
    fdk_free(owned);
    return t;
}

/* ---- install / uninstall ---------------------------------------------------- */

/* Installs `t` as the settings-applied theme: current default via the
 * settings-aware path in theme.c (an install is not an app opt-out),
 * owned in g_installed, retiring whatever it replaces. */
static void install_theme(fdk_theme *t, const char *name,
                          const char *origin) {
    fdk_theme *old = g_installed;
    g_installed = t;
    snprintf(g_installed_name, sizeof g_installed_name, "%s",
             name != NULL ? name : "");
    fdk__theme_set_default_settings(t);
    if (old != NULL) {
        fdk_theme_destroy(old); /* no longer current: plain free */
    }
    FDK_INFO("theme: '%s' applied (%s)",
             g_installed_name, origin != NULL ? origin : "settings");
}

/* Reverts to the built-in (the user reset the setting while the app
 * ran). No-op when nothing was installed. */
static void uninstall_theme(void) {
    fdk_theme *old = g_installed;
    if (old == NULL) {
        return;
    }
    g_installed = NULL;
    g_installed_name[0] = '\0';
    fdk__theme_set_default_settings(NULL); /* built-in + repaint */
    fdk_theme_destroy(old);
    FDK_INFO("theme: setting cleared — back to the built-in theme");
}

/* The shared apply: resolve, short-circuit on "nothing changed",
 * install or uninstall. Safe to run any time; the boot and every
 * live recheck both land here. */
static void apply_resolved(void) {
    char name[FDK_THEME_STRING_MAX];
    const char *origin = NULL;
    fdk_theme *t = resolve_theme(name, sizeof name, &origin);
    if (t == NULL) {
        if (name[0] == '\0') {
            /* Nothing resolvable: only act when a previous install
             * must be retired (reset-to-default at runtime). */
            uninstall_theme();
        }
        return;
    }
    if (g_installed != NULL && strcmp(name, g_installed_name) == 0) {
        fdk_theme_destroy(t); /* same theme: no repaint storm */
        return;
    }
    install_theme(t, name, origin);
}

/* ---- the boot (theme.c's lazy one-shot lands here) -------------------------- */

void fdk__theme_settings_boot(void) {
    if (g_boot_ran) {
        return;
    }
    g_boot_ran = true;
    apply_resolved();
}

/* Re-run resolution NOW (the whitelist changed, or a watched file
 * changed — the pump's drain calls this). Respects the opt-out: an
 * application that installed its own theme is never overridden. */
static void watch_stop(void); /* defined with the watch, below */

void fdk__theme_settings_recheck(void) {
    if (fdk__theme_settings_app_owned()) {
        watch_stop();
        return;
    }
    apply_resolved();
}

/* ---- the live watch ----------------------------------------------------------- */

static void watch_stop(void) {
    if (g_watch_fd >= 0) {
        close(g_watch_fd);
        g_watch_fd = -1;
    }
}

/* (Re)arms the watch: one inotify fd, one add_watch per distinct
 * settings directory, basenames filtered at drain time. Directories
 * that do not exist yet are skipped with one INFO line — `fdk-theme
 * set` creates the config dir before saving, but a watch cannot be
 * placed on a directory that is not there; apps started before the
 * directory exists follow the setting at their next launch instead
 * (documented, honest). */
static void watch_start(void) {
    watch_stop();
    if (fdk__theme_settings_app_owned()) {
        return; /* the app owns the theme; settings can't touch it */
    }
    if (g_dir_global[0] == '\0' && g_dir_app[0] == '\0') {
        return; /* nothing resolvable to watch */
    }
    g_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (g_watch_fd < 0) {
        FDK_WARN("theme: could not start the settings watch (%s); "
                 "live re-theming is off for this process",
                 strerror(errno));
        return;
    }
    const char *dirs[2] = {g_dir_global, g_dir_app};
    int added = 0;
    for (int i = 0; i < 2; i++) {
        if (dirs[i][0] == '\0') {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(dirs[i], dirs[j]) == 0) {
                dup = true; /* the usual case: both files in ~/.config */
            }
        }
        if (dup) {
            continue;
        }
        if (inotify_add_watch(g_watch_fd, dirs[i],
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                                  IN_DELETE) < 0) {
            FDK_INFO("theme: not watching %s (%s)",
                     dirs[i], strerror(errno));
            continue;
        }
        added++;
    }
    if (added == 0) {
        watch_stop(); /* nothing watched; do not keep a dead fd */
    }
}

/* The poll-integration face: -1 when nothing is watched. */
int fdk__theme_settings_watch_fd(void) {
    return g_watch_fd;
}

/* Drains pending inotify events; returns true when a watched file
 * changed (the caller — the pump — treats that as activity worth
 * reporting). The recheck itself is coalesced: every event batch
 * resolves the file's CURRENT state exactly once, and an unchanged
 * theme name short-circuits before any repaint. */
bool fdk__theme_settings_watch_drain(void) {
    if (g_watch_fd < 0) {
        return false;
    }
    bool relevant = false;
    char buf[4096] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = read(g_watch_fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                FDK_WARN("theme: settings watch read failed (%s); "
                         "live re-theming is off for this process",
                         strerror(errno));
                watch_stop();
            }
            break;
        }
        ssize_t off = 0;
        while (off + (ssize_t)sizeof(struct inotify_event) <= n) {
            const struct inotify_event *ev =
                (const struct inotify_event *)(buf + off);
            off += (ssize_t)sizeof(struct inotify_event) + ev->len;
            if (ev->len == 0) {
                continue;
            }
            if ((g_base_global[0] != '\0' &&
                 strcmp(ev->name, g_base_global) == 0) ||
                (g_base_app[0] != '\0' &&
                 strcmp(ev->name, g_base_app) == 0)) {
                relevant = true;
            }
        }
    }
    if (!relevant) {
        return false;
    }
    if (fdk__theme_settings_app_owned()) {
        watch_stop(); /* the app opted out somewhere along the way */
        return false;
    }
    apply_resolved();
    return true;
}

/* ---- lifecycle (bridged through window.c from core) --------------------------- */

/* Records the application identity (fdk_init) and arms the watch.
 * Called once per fdk_init; a second init (the test pattern) re-binds
 * the app id and re-arms — the boot itself stays once per process,
 * but the LIVE follow tracks the current context's identity. */
void fdk__theme_settings_set_app_id(const char *app_id) {
    fdk_free(g_app_id);
    g_app_id = NULL;
    g_dir_app[0] = '\0';
    g_base_app[0] = '\0';

    if (app_id != NULL && app_id[0] != '\0') {
        g_app_id = fdk__theme_strdup(app_id);
    }

    g_dir_global[0] = '\0';
    g_base_global[0] = '\0';
    char *global = fdk__prefs_resolve_path("fdk");
    if (global != NULL) {
        split_dir_base(global, g_dir_global, sizeof g_dir_global,
                       g_base_global, sizeof g_base_global);
        fdk_free(global);
    }
    if (g_app_id != NULL) {
        char *app = fdk__prefs_resolve_path(g_app_id);
        if (app != NULL) {
            split_dir_base(app, g_dir_app, sizeof g_dir_app,
                           g_base_app, sizeof g_base_app);
            fdk_free(app);
        }
    }
    watch_start();
}

/* fdk_shutdown: release the watch (the installed theme, if any, is
 * still the process's current default and stays valid — theme state
 * is process-wide, context lifetime is not its lifetime). */
void fdk__theme_settings_shutdown(void) {
    watch_stop();
    fdk_free(g_app_id);
    g_app_id = NULL;
    g_dir_app[0] = '\0';
    g_base_app[0] = '\0';
}

/* fdk_theme_destroy's ownership slot: forget `theme` if it is the one
 * this module installed (an application may legitimately destroy the
 * borrowed current-default pointer it holds). */
void fdk__theme_settings_forget(fdk_theme *theme) {
    if (theme != NULL && theme == g_installed) {
        g_installed = NULL;
        g_installed_name[0] = '\0';
    }
}

/* TEST-only: rewind boot + watch + whitelist state so one process can
 * run many scenarios (tests/test_theme_discovery.c). Destroys the
 * installed theme through the public API so any current-default
 * revert happens exactly as it would in an application. */
void fdk__theme_settings_reset_for_tests(void) {
    watch_stop();
    if (g_installed != NULL) {
        fdk_theme *t = g_installed;
        g_installed = NULL; /* forget FIRST: destroy must not re-enter */
        g_installed_name[0] = '\0';
        fdk_theme_destroy(t); /* reverts the current default if needed */
    }
    g_boot_ran = false;
    fdk_theme_clear_allowed_themes();
}

/* The whitelist's storage free (also used by set_allowed's rollback). */
void fdk__theme_settings_free_list(char **list, size_t count) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_free(list[i]);
    }
    fdk_free(list);
}
