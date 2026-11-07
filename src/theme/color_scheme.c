#define FDK_LOG_TAG "theme"
/*
 * color_scheme.c — the desktop's light/dark preference (1.1.0)
 *
 * One process-wide answer to "does the desktop prefer dark or
 * light?", sourced the way each world actually states it:
 *
 *   1. $FDK_COLOR_SCHEME — the per-process override (the FDK_THEME
 *      precedent: theming one launch). "dark" / "light" / "none";
 *      anything else is ignored with one warning. When set, live
 *      following is OFF (the override is the answer).
 *
 *   2. XSETTINGS Gtk/ColorScheme (X11 sessions — and XWayland's
 *      xsettingsd under compositors that run one): the string
 *      "prefer-dark" / "prefer-light" / "" (no preference), read
 *      through the same bounded live-reader the icon engine uses.
 *      LIVE: the settings daemon rewrites the property and the
 *      X11 backend routes the PropertyNotify to this module's
 *      re-poll (x11_xsettings.c arms the watch at connect; a
 *      throttled re-arm catches daemon restarts).
 *
 *   3. org.freedesktop.portal.Settings (the Wayland world's
 *      channel — and the fallback on X11 sessions with no
 *      settings daemon): ReadOne("org.freedesktop.appearance",
 *      "color-scheme") at connect, SettingChanged afterwards. Rides
 *      the native D-Bus client (fdk_dbus.h) on the FDK event loop:
 *      the connection is created lazily by the first live context's
 *      pump (window.c's scheme tick), retried on a 30 s throttle
 *      (a session bus appearing late still gets followed), and
 *      dies quietly when no bus exists (one INFO line — the
 *      no-bus posture, never a retry storm).
 *
 * The answer feeds the theme engine's variant selection
 * (settings.c): a resolved theme named X prefers the sibling face
 * X-dark / X-light when it exists on the same search path. The
 * public getter (fdk_system_color_scheme) lets applications make
 * their OWN dark-mode decisions (choosing their own assets) with
 * the same truth the theme engine used.
 *
 * Layering: theme module, platform-free — the XSETTINGS reader is
 * declared in theme_internal.h and implemented in the X11 backend
 * (the icon engine's precedent), the portal client is public API.
 * Threads: like everything theme, UI-thread-only.
 */

#include "theme_internal.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_extend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- state ---------------------------------------------------------------- */

/* The cached answer (the getter never touches a socket). */
static fdk_color_scheme g_scheme = FDK_COLOR_SCHEME_NONE;

/* The env override: -1 = unset (parse once), 0 = "none" (explicit),
 * else the scheme it names. When set, XSETTINGS and the portal are
 * muted — the override IS the answer. */
static int g_env_scheme = -1;

/* Whether the XSETTINGS channel ever produced a value (the portal
 * is the fallback when it never speaks). */
static bool g_xsettings_spoke;

/* Whether the XSETTINGS channel has been POLLED at least once (the
 * first getter call polls lazily — a process whose theme resolution
 * runs before/after any boot ordering still sees the desktop's
 * current statement; later updates arrive as events). */
static bool g_xsettings_polled;

/* The portal singleton (Wayland / no-xsettingsd sessions). */
static fdk_dbus_connection *g_portal;
static bool g_portal_failed; /* last connect attempt failed        */
static uint64_t g_portal_last_try_ms;

/* Set at shutdown: no more portal activity. */
static bool g_shut_down;

/* ---- the env override -------------------------------------------------------- */

static void env_scheme_parse(void) {
    g_env_scheme = 0; /* default: no override                    */
    const char *v = getenv("FDK_COLOR_SCHEME");
    if (v == NULL || v[0] == '\0') {
        return;
    }
    if (strcmp(v, "dark") == 0 || strcmp(v, "1") == 0) {
        g_env_scheme = (int)FDK_COLOR_SCHEME_DARK;
    } else if (strcmp(v, "light") == 0 || strcmp(v, "2") == 0) {
        g_env_scheme = (int)FDK_COLOR_SCHEME_LIGHT;
    } else if (strcmp(v, "none") == 0 || strcmp(v, "0") == 0) {
        g_env_scheme = 0; /* explicit none */
    } else {
        FDK_WARN("color-scheme: $FDK_COLOR_SCHEME='%s' is not "
                 "dark/light/none — ignored",
                 v);
    }
}

fdk_color_scheme fdk_system_color_scheme(void) {
    if (g_env_scheme < 0) {
        env_scheme_parse();
    }
    if (g_env_scheme > 0) {
        return (fdk_color_scheme)g_env_scheme;
    }
    if (!g_xsettings_polled && !g_shut_down) {
        /* The lazy first poll (the icon engine's detection posture:
         * one live read at the first query, then events carry it).
         * The flag is set BEFORE the poll — the poll can trigger a
         * recheck that calls right back in here. */
        g_xsettings_polled = true;
        fdk__color_scheme_poll_x11();
    }
    return g_scheme;
}

/* ---- the shared setter: a real change re-runs theme resolution ------------- */

static void scheme_set(fdk_color_scheme s) {
    if (g_env_scheme < 0) {
        env_scheme_parse();
    }
    if (g_env_scheme > 0 || g_shut_down) {
        return; /* the override wins; shutdown freezes the answer  */
    }
    if (s == g_scheme) {
        return;
    }
    g_scheme = s;
    FDK_INFO("color-scheme: the desktop prefers %s",
             s == FDK_COLOR_SCHEME_DARK ? "dark"
                                        : (s == FDK_COLOR_SCHEME_LIGHT
                                               ? "light"
                                               : "no preference"));
    if (getenv("PP_NORECHECK") != NULL) {
        return; /* bisect gate: the value change without the recheck */
    }
    /* A preference change re-runs theme resolution: the variant
     * selection in settings.c may flip the installed face, and the
     * same repaint-all path `fdk-theme set` rides fires here. */
    fdk__theme_settings_recheck();
}

/* ---- XSETTINGS: Gtk/ColorScheme -------------------------------------------- */

/* GTK's spelling: "prefer-dark" / "prefer-light" / "" (absent also
 * means no preference). Older daemons used 0/1/2 integers; the
 * string channel is what GTK4/5 actually read — integer values are
 * accepted defensively (a daemon publishing them exists in the
 * wild) with the portal's numbering. */
static fdk_color_scheme parse_scheme_string(const char *v) {
    if (v == NULL) {
        return FDK_COLOR_SCHEME_NONE;
    }
    if (strcmp(v, "prefer-dark") == 0 || strcmp(v, "dark") == 0 ||
        strcmp(v, "1") == 0) {
        return FDK_COLOR_SCHEME_DARK;
    }
    if (strcmp(v, "prefer-light") == 0 || strcmp(v, "light") == 0 ||
        strcmp(v, "2") == 0) {
        return FDK_COLOR_SCHEME_LIGHT;
    }
    return FDK_COLOR_SCHEME_NONE;
}

void fdk__color_scheme_poll_x11(void) {
    if (g_env_scheme < 0) {
        env_scheme_parse();
    }
    g_xsettings_polled = true;
    if (g_env_scheme > 0 || g_shut_down) {
        return;
    }
    char val[32];
    if (fdk__x11_xsettings_string("Gtk/ColorScheme", val, sizeof val)) {
        g_xsettings_spoke = true;
        scheme_set(parse_scheme_string(val));
    } else {
        /* "" published explicitly ALSO counts as the daemon
         * speaking (a no-preference statement is a statement);
         * fdk__x11_xsettings_string cannot distinguish "" from
         * absent, so a daemon that publishes SOMETHING ELSE we
         * successfully read counts too. The honest limit: a daemon
         * publishing ONLY Gtk/ColorScheme="" reads as absent —
         * the portal fallback stays active; both channels agree
         * on "no preference" anyway. */
    }
}

bool fdk__color_scheme_x11_spoke(void) {
    return g_xsettings_spoke;
}

/* ---- the portal: org.freedesktop.portal.Settings ----------------------------- */

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.Settings"
#define APPEARANCE_NS "org.freedesktop.appearance"

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void portal_death(fdk_dbus_connection *c, void *user) {
    (void)user;
    /* The bus or the portal went away mid-session: drop the handle
     * (closed from inside its own callback — the deferred free the
     * D-Bus layer documents) and let the tick reconnect on its
     * throttle. */
    if (g_portal == c) {
        g_portal = NULL;
        fdk_dbus_connection_close(c);
    }
}

/* SettingChanged(namespace s, key s, value v) — the live channel. */
static void portal_setting_changed(fdk_dbus_connection *c,
                                   fdk_dbus_message *sig, void *user) {
    (void)c;
    (void)user;
    if (getenv("FDK_PORTAL_NOCB")) {
        return; /* bisect gate: the machinery without the engine work */
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(sig, &args, &n) != FDK_OK || n < 3 ||
        args == NULL) {
        return;
    }
    const char *ns = fdk_dbus_value_string(args[0]);
    const char *key = fdk_dbus_value_string(args[1]);
    if (ns != NULL && key != NULL &&
        strcmp(ns, APPEARANCE_NS) == 0 &&
        strcmp(key, "color-scheme") == 0) {
        const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[2]);
        uint32_t u = fdk_dbus_value_uint32(v);
        if (u <= 2) {
            scheme_set((fdk_color_scheme)u);
        }
    }
    fdk_dbus_values_free_array(args, n);
}

/* ReadOne's reply: one argument, the value as a VARIANT (u). */
static void portal_readone_reply(fdk_dbus_connection *c,
                                 fdk_dbus_message *m, void *user) {
    (void)user;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return; /* portal without the appearance namespace: nothing */
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL) {
        return;
    }
    const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[0]);
    uint32_t u = fdk_dbus_value_uint32(v);
    if (u <= 2) {
        scheme_set((fdk_color_scheme)u);
    }
    fdk_dbus_values_free_array(args, n);
    (void)c;
}

static void portal_ready(fdk_dbus_connection *c, int ok, const char *err,
                         void *user) {
    (void)user;
    if (!ok) {
        FDK_INFO("color-scheme: portal connect failed (%s); the "
                 "desktop preference will be polled again later",
                 err ? err : "?");
        if (g_portal == c) {
            g_portal = NULL;
            fdk_dbus_connection_close(c);
        }
        return;
    }
    fdk_dbus_connection_set_death_handler(c, portal_death, NULL);

    /* The live subscription FIRST (no window between the initial
     * read and the watch). arg0 scoping on the namespace keeps the
     * rule precise; no sender (the well-known→unique rewrite). */
    char *rule = NULL;
    if (getenv("FDK_PORTAL_NOSUB") == NULL) {
        rule = fdk_dbus_match_rule("signal", NULL, PORTAL_IFACE,
                                   "SettingChanged", NULL,
                                   APPEARANCE_NS);
    }
    if (rule != NULL) {
        fdk_dbus_subscription *sub =
            fdk_dbus_connection_subscribe(c, rule, portal_setting_changed,
                                          NULL);
        if (sub != NULL) {
            /* the subscription lives as long as the connection:
             * leak-by-design singleton state, swept at shutdown */
        } else {
            FDK_INFO("color-scheme: could not subscribe to portal "
                     "SettingChanged; only the initial read applies");
        }
        free(rule);
    }

    /* The initial value. */
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE, "ReadOne");
    if (m != NULL &&
        fdk_dbus_message_append_string(m, APPEARANCE_NS) == FDK_OK &&
        fdk_dbus_message_append_string(m, "color-scheme") == FDK_OK) {
        (void)fdk_dbus_connection_call(c, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                       portal_readone_reply, NULL);
    } else {
        fdk_dbus_message_free(m);
    }
}

/* Called from the pump (window.c's scheme tick): attach the portal
 * singleton when the XSETTINGS channel has not spoken and the
 * session has a context to ride. Throttled: a session bus that
 * appears late still gets followed; a bus that stays absent costs
 * one INFO line per 30 s at most (in practice one, ever). */
void fdk__color_scheme_portal_tick(fdk_context *ctx) {
    if (g_shut_down || g_portal != NULL || ctx == NULL) {
        return;
    }
    if (g_env_scheme < 0) {
        env_scheme_parse();
    }
    if (g_env_scheme > 0 || g_xsettings_spoke) {
        return; /* the override or the native channel owns the answer */
    }
    if (g_portal_failed && now_ms() - g_portal_last_try_ms < 30000) {
        return;
    }
    g_portal_last_try_ms = now_ms();
    g_portal = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, portal_ready,
                                NULL);
    if (g_portal == NULL) {
        g_portal_failed = true;
    } else {
        g_portal_failed = false;
    }
}

void fdk__color_scheme_shutdown(void) {
    g_shut_down = true;
    if (g_portal != NULL) {
        fdk_dbus_connection_close(g_portal); /* outside callbacks: free */
        g_portal = NULL;
    }
}

/* Test seam: reset ALL module state (the headless suites exercise
 * the env parse and the parse helpers hermetically). */
void fdk__color_scheme_reset_for_tests(void) {
    g_scheme = FDK_COLOR_SCHEME_NONE;
    g_env_scheme = -1;
    g_xsettings_spoke = false;
    g_xsettings_polled = false;
    g_portal = NULL;
    g_portal_failed = false;
    g_portal_last_try_ms = 0;
    g_shut_down = false;
}
