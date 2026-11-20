/*
 * reduced_motion.c — the desktop's reduced-motion preference
 * (ACCESSIBILITY-AUDIT.md H2, first third: the READ side).
 *
 * The audit's finding: no reduced-motion setting existed anywhere
 * in the API. The fix follows the color-scheme module's shape
 * exactly (same channels, same precedence, same live-follow), so
 * the maintenance story is one pattern, not two.
 *
 * Channels, in precedence order:
 *
 *   1. $FDK_REDUCE_MOTION — the developer/test override. Spelled
 *      reduce (not reduced): "1"/"reduce" forces it on, "0"/"none"
 *      forces it off, anything else warns once and is ignored.
 *      When set, it IS the answer; the desktop is muted.
 *
 *   2. X11 XSETTINGS: Net/EnableAnimations (the freedesktop key
 *      GTK publishes; 0 = animations off = reduced motion) —
 *      read at the first getter call, then live via the settings
 *      daemon's PropertyNotify (the X11 backend routes it to this
 *      module's re-poll, same as color-scheme).
 *
 *   3. org.freedesktop.portal.Settings,
 *      ReadOne("org.freedesktop.appearance", "contrast-high")
 *      is NOT this module's key — the portal carries no
 *      animations key; the appearance namespace is contrast + color
 *      only. On Wayland the real source is the GNOME key
 *      org.gnome.desktop.interface's "enable-animations", read
 *      the same way ui_metrics.c reads "font-name": through the
 *      portal's ReadOne with that namespace. SettingChanged on
 *      the same key follows it live.
 *
 *   4. Default: NOT reduced (animations on).
 *
 * The public getter (fdk_theme.h):
 *   fdk_system_prefers_reduced_motion() — true when the desktop
 *   asked for fewer animations. The ANIMATION ENGINE does not
 *   gate itself on this (that changes widget timing behavior and
 *   needs its own decision); applications and FDK's own rig
 *   screens read it and adapt. This is deliberately the honest
 *   read side first — the audit's H2 named the preference as the
 *   gap; what consumers DO with it is their call.
 *
 * Layering: theme module, platform-free — the XSETTINGS reader is
 * declared in theme_internal.h and implemented in the X11 backend
 * (the icon engine's and color-scheme's precedent), the portal
 * client is public API. Threads: UI-thread-only, like everything
 * theme.
 */

#include "theme_internal.h"

#define FDK_LOG_TAG "theme"
#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_extend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- state ---------------------------------------------------------------- */

/* The cached answer (the getter never touches a socket). */
static bool g_reduced;

/* The env override: -1 = unset (parse once), 0 = no override,
 * 1 = forced on, 2 = forced off. */
static int g_env;

/* Whether the XSETTINGS channel ever produced a value. */
static bool g_xsettings_spoke;
static bool g_xsettings_polled;

/* The portal singleton. */
static fdk_dbus_connection *g_portal;
static bool g_portal_failed;
static uint64_t g_portal_last_try_ms;

/* Set at shutdown: no more portal activity. */
static bool g_shut_down;

/* ---- the env override ------------------------------------------------------ */

static void env_parse(void) {
    g_env = 0; /* default: no override */
    const char *v = getenv("FDK_REDUCE_MOTION");
    if (v == NULL || v[0] == '\0') {
        return;
    }
    if (strcmp(v, "1") == 0 || strcmp(v, "reduce") == 0 ||
        strcmp(v, "on") == 0 || strcmp(v, "true") == 0) {
        g_env = 1;
    } else if (strcmp(v, "0") == 0 || strcmp(v, "none") == 0 ||
               strcmp(v, "off") == 0 || strcmp(v, "false") == 0) {
        g_env = 2;
    } else {
        FDK_WARN("reduced-motion: $FDK_REDUCE_MOTION='%s' is not a "
                 "boolean — ignored",
                 v);
    }
}

bool fdk_system_prefers_reduced_motion(void) {
    if (g_env < 0) {
        env_parse();
    }
    if (g_env == 1) {
        return true;
    }
    if (g_env == 2) {
        return false;
    }
    if (!g_xsettings_polled && !g_shut_down) {
        /* The lazy first poll (the color-scheme posture). The flag
         * is set BEFORE the poll — the poll can trigger a recheck
         * that calls right back in here. */
        g_xsettings_polled = true;
        fdk__reduced_motion_poll_x11();
    }
    return g_reduced;
}

/* ---- the setter ------------------------------------------------------------ */

static void reduced_set(bool r) {
    if (g_env < 0) {
        env_parse();
    }
    if (g_env != 0 || g_shut_down) {
        return; /* the override wins; shutdown freezes the answer */
    }
    if (r == g_reduced) {
        return;
    }
    g_reduced = r;
    FDK_INFO("reduced-motion: the desktop %s animations",
             r ? "asks to reduce" : "allows");
    /* No theme re-resolution here: the theme faces do not encode
     * motion. Applications that adapt re-read the getter on their
     * next layout pass (the reference shells' live-follow shape).
     * When the animation engine grows its own gate, it hooks this
     * seam, not the theme engine's. */
}

/* ---- XSETTINGS: Net/EnableAnimations -------------------------------------- */

/* 0 = animations off (reduced motion); anything else = on. The
 * integer channel is what xsettingsd and xfsettingsd publish. */
void fdk__reduced_motion_poll_x11(void) {
    if (g_env < 0) {
        env_parse();
    }
    g_xsettings_polled = true;
    if (g_env != 0 || g_shut_down) {
        return;
    }
    /* Net/EnableAnimations is an INTEGER setting (xsettings type
     * 0) — the int-typed seam beside the string one. */
    int val = 0;
    if (fdk__x11_xsettings_int("Net/EnableAnimations", &val)) {
        g_xsettings_spoke = true;
        reduced_set(val == 0);
    }
}

bool fdk__reduced_motion_x11_spoke(void) {
    return g_xsettings_spoke;
}

/* ---- the portal: org.gnome.desktop.interface / enable-animations ----------- */

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.Settings"
#define GNOME_NS "org.gnome.desktop.interface"

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void portal_death(fdk_dbus_connection *c, void *user) {
    (void)user;
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
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(sig, &args, &n) != FDK_OK || n < 3 ||
        args == NULL) {
        return;
    }
    const char *ns = fdk_dbus_value_string(args[0]);
    const char *key = fdk_dbus_value_string(args[1]);
    if (ns != NULL && key != NULL && strcmp(ns, GNOME_NS) == 0 &&
        strcmp(key, "enable-animations") == 0) {
        const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[2]);
        uint32_t u = fdk_dbus_value_uint32(v);
        reduced_set(u == 0);
    }
    fdk_dbus_values_free_array(args, n);
}

/* ReadOne's reply: one argument, the value as a VARIANT (b in the
 * GNOME spelling; uint32 defensively — both map through the
 * boolean-value reader, and a 0 is a 0). */
static void portal_readone_reply(fdk_dbus_connection *c,
                                 fdk_dbus_message *m, void *user) {
    (void)user;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return; /* portal without the key: nothing */
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL) {
        return;
    }
    const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[0]);
    if (fdk_dbus_value_type(v) == FDK_DBUS_BOOLEAN) {
        reduced_set(!fdk_dbus_value_boolean(v));
    } else {
        uint32_t u = fdk_dbus_value_uint32(v);
        reduced_set(u == 0);
    }
    fdk_dbus_values_free_array(args, n);
    (void)c;
}

static void portal_ready(fdk_dbus_connection *c, int ok, const char *err,
                         void *user) {
    (void)user;
    if (!ok) {
        FDK_INFO("reduced-motion: portal connect failed (%s); the "
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
     * read and the watch), scoped to the namespace. */
    char *rule = fdk_dbus_match_rule("signal", NULL, PORTAL_IFACE,
                                     "SettingChanged", NULL, GNOME_NS);
    if (rule != NULL) {
        fdk_dbus_subscription *sub =
            fdk_dbus_connection_subscribe(c, rule, portal_setting_changed,
                                          NULL);
        if (sub != NULL) {
            /* lives with the connection; swept at shutdown */
        } else {
            FDK_INFO("reduced-motion: could not subscribe to portal "
                     "SettingChanged; only the initial read applies");
        }
        free(rule);
    }

    /* The initial value. */
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE, "ReadOne");
    if (m != NULL &&
        fdk_dbus_message_append_string(m, GNOME_NS) == FDK_OK &&
        fdk_dbus_message_append_string(m, "enable-animations") ==
            FDK_OK) {
        (void)fdk_dbus_connection_call(c, m, FDK_DBUS_TIMEOUT_DEFAULT,
                                       portal_readone_reply, NULL);
    } else {
        fdk_dbus_message_free(m);
    }
}

/* Called from the pump (window.c's scheme tick): attach the portal
 * singleton when the XSETTINGS channel has not spoken. Same
 * 30 s failure throttle as color-scheme. */
void fdk__reduced_motion_portal_tick(fdk_context *ctx) {
    if (g_shut_down || g_portal != NULL || ctx == NULL) {
        return;
    }
    if (g_env < 0) {
        env_parse();
    }
    if (g_env != 0 || g_xsettings_spoke) {
        return;
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

void fdk__reduced_motion_shutdown(void) {
    g_shut_down = true;
    if (g_portal != NULL) {
        fdk_dbus_connection_close(g_portal);
        g_portal = NULL;
    }
}

/* Test seam: reset ALL module state (the headless suites exercise
 * the env parse hermetically). */
void fdk__reduced_motion_reset_for_tests(void) {
    g_reduced = false;
    g_env = -1;
    g_xsettings_spoke = false;
    g_xsettings_polled = false;
    g_portal = NULL;
    g_portal_failed = false;
    g_portal_last_try_ms = 0;
    g_shut_down = false;
}
