#define FDK_LOG_TAG "theme"
/*
 * ui_metrics.c — the process's UI font size and scale (1.1.0)
 *
 * One answer, process-wide, to "what size is the UI text?" — the
 * same posture as color_scheme.c (which this file deliberately
 * mirrors): a cached resolution over four channels, refreshed by
 * the same live events (XSETTINGS rewrites, the portal's
 * SettingChanged, FDK settings-file changes), never a socket on
 * the getter path.
 *
 *   1. $FDK_UI_FONT_SIZE (px, 8..96) / $FDK_UI_SCALE (0.5..4.0) —
 *      the per-process override (the FDK_THEME precedent).
 *   2. FDK's own settings store: ui.font_size / ui.scale — the
 *      owner's explicit choice, overriding the desktop (the
 *      app-scoped file first, then the global "fdk" file, the
 *      theme resolution's precedence).
 *   3. The desktop's statement:
 *      - X11: XSETTINGS Gtk/FontName — "Family [Style] N", the
 *        trailing N parsed as POINTS and converted at the nominal
 *        96 dpi (pt * 4 / 3), the same arithmetic every toolkit
 *        applies when it has no better DPI statement.
 *      - the portal (Wayland / X11 without a settings daemon):
 *        org.gnome.desktop.interface's font-name (same parse) and
 *        text-scaling-factor (the scale channel).
 *   4. Defaults: 16 px, scale 1.0.
 *
 * The public getters (fdk_theme.h):
 *   fdk_ui_font_size()  the BASE size in px (channels 1-3, else 16)
 *   fdk_ui_scale()      the multiplier (1.0 default)
 *   fdk_ui_font_px()    round(base * scale), clamped to [8, 96] —
 *                       the one number a renderer should use
 *
 * LIVE FOLLOWING: the answer updates when the desktop restates it
 * (the XSETTINGS PropertyNotify re-poll, the portal's
 * SettingChanged) or FDK's settings file changes — unless the
 * environment or the settings store OWNS the answer, in which
 * case the desktop's statement is politely ignored. Consumers
 * that re-read per layout pass (the reference shells do) follow
 * it live; a consumer that caches the number itself gets the
 * next-launch value — the honest limit, stated here.
 *
 * Threads: like everything theme, UI-thread only.
 */

#include "theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "fdk/fdk_dbus.h"
#include "fdk/fdk_prefs.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- state ---------------------------------------------------------------- */

#define DEFAULT_FONT_PX 16
#define MIN_FONT_PX 8
#define MAX_FONT_PX 96

static int g_font_px = -1;       /* the resolved answer; -1 = unresolved  */
static double g_scale = -1.0;    /* the resolved scale; -1 = unresolved   */
static bool g_font_override;     /* env or the settings store own it      */
static bool g_scale_override;
static bool g_desktop_font_spoke; /* the desktop stated a font size       */
static int g_desktop_font_px;
static bool g_desktop_scale_spoke;
static double g_desktop_scale;
static bool g_shut_down;

/* The portal singleton (the color-scheme module's shape). */
static fdk_dbus_connection *g_portal;
static bool g_portal_failed;
static uint64_t g_portal_last_try_ms;

/* ---- parsing --------------------------------------------------------------- */

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Parses "Family [Style] N" (the XSETTINGS Gtk/FontName and the
 * portal's font-name shapes): the LAST whitespace-delimited token,
 * when it is a plain positive integer, is the size in POINTS.
 * Returns 0 when no size token exists. */
static int parse_font_points(const char *name) {
    if (name == NULL) {
        return 0;
    }
    const char *end = name + strlen(name);
    while (end > name && end[-1] == ' ') {
        end--;
    }
    const char *tok = end;
    while (tok > name && tok[-1] != ' ') {
        tok--;
    }
    if (tok == end) {
        return 0;
    }
    size_t len = (size_t)(end - tok);
    int pt = 0;
    for (size_t i = 0; i < len; i++) {
        if (tok[i] < '0' || tok[i] > '9') {
            return 0;
        }
        pt = pt * 10 + (tok[i] - '0');
        if (pt > 288) {
            return 0; /* absurd: refuse, not clamp (a broken string) */
        }
    }
    return pt;
}

/* Points to pixels at the nominal 96 dpi. */
static int points_to_px(int pt) {
    return (pt * 4 + 2) / 3; /* round(4/3 * pt) */
}

/* ---- resolution -------------------------------------------------------------- */

/* The FDK settings store leg: ui.font_size / ui.scale. */
static void settings_store_leg(int *font_out, double *scale_out) {
    const char *ids[2] = {fdk__theme_settings_app_id(), "fdk"};
    for (int i = 0; i < 2; i++) {
        if (ids[i] == NULL || ids[i][0] == '\0') {
            continue;
        }
        fdk_prefs *p = NULL;
        if (!fdk_ok(fdk_prefs_open(ids[i], &p)) || p == NULL) {
            continue;
        }
        long v = fdk_prefs_get_int(p, "ui.font_size", -1);
        if (v >= MIN_FONT_PX && v <= MAX_FONT_PX) {
            *font_out = (int)v;
        }
        double s = fdk_prefs_get_double(p, "ui.scale", 0.0);
        if (s >= 0.5 && s <= 4.0) {
            *scale_out = s;
        }
        fdk_prefs_destroy(p);
    }
}

/* The desktop's XSETTINGS font statement. */
static bool desktop_x11_font(int *out_px) {
    char name[256];
    if (fdk__x11_xsettings_string("Gtk/FontName", name, sizeof name)) {
        int pt = parse_font_points(name);
        if (pt > 0) {
            *out_px = points_to_px(pt);
            return true;
        }
    }
    return false;
}

/* One resolution pass (idempotent while cached). */
static void resolve(void) {
    if (g_font_px > 0 && g_scale > 0.0) {
        return; /* cached */
    }

    /* 1. the environment override — the strongest voice. */
    const char *env = getenv("FDK_UI_FONT_SIZE");
    if (g_font_px < 0 && env != NULL && env[0] != '\0') {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end != NULL && *end == '\0' && v >= MIN_FONT_PX &&
            v <= MAX_FONT_PX) {
            g_font_px = (int)v;
            g_font_override = true;
        } else {
            FDK_WARN("ui-metrics: $FDK_UI_FONT_SIZE='%s' is not an "
                     "integer in [%d, %d] — ignored",
                     env, MIN_FONT_PX, MAX_FONT_PX);
        }
    }
    env = getenv("FDK_UI_SCALE");
    if (g_scale < 0.0 && env != NULL && env[0] != '\0') {
        char *end = NULL;
        double v = strtod(env, &end);
        if (end != NULL && *end == '\0' && v >= 0.5 && v <= 4.0) {
            g_scale = v;
            g_scale_override = true;
        } else {
            FDK_WARN("ui-metrics: $FDK_UI_SCALE='%s' is not a number in "
                     "[0.5, 4.0] — ignored",
                     env);
        }
    }

    /* 2. FDK's settings store — the owner's explicit choice. */
    if (g_font_px < 0 || g_scale < 0.0) {
        int font = -1;
        double scale = -1.0;
        settings_store_leg(&font, &scale);
        if (g_font_px < 0 && font > 0) {
            g_font_px = font;
            g_font_override = true;
        }
        if (g_scale < 0.0 && scale > 0.0) {
            g_scale = scale;
            g_scale_override = true;
        }
    }

    /* 3. the desktop's statement (only when nobody overrode). */
    if (g_font_px < 0 && !g_font_override) {
        if (g_desktop_font_spoke) {
            g_font_px = g_desktop_font_px;
        } else {
            int px = 0;
            if (desktop_x11_font(&px)) {
                g_desktop_font_spoke = true;
                g_desktop_font_px = px;
                g_font_px = px;
            }
        }
    }
    if (g_scale < 0.0 && !g_scale_override && g_desktop_scale_spoke) {
        g_scale = g_desktop_scale;
    }

    /* 4. the defaults. */
    if (g_font_px < 0) {
        g_font_px = DEFAULT_FONT_PX;
    }
    if (g_scale < 0.0) {
        g_scale = 1.0;
    }
}

/* ---- the XSETTINGS re-poll (the backend's PropertyNotify hook) --------------- */

void fdk__ui_metrics_poll_x11(void) {
    if (g_shut_down) {
        return;
    }
    int px = 0;
    if (desktop_x11_font(&px)) {
        if (!g_desktop_font_spoke || px != g_desktop_font_px) {
            FDK_INFO("ui-metrics: the desktop font statement changed to "
                     "%d px",
                     px);
        }
        g_desktop_font_spoke = true;
        g_desktop_font_px = px;
        if (!g_font_override) {
            g_font_px = px; /* the desktop owns the answer: follow */
        }
    } else if (g_desktop_font_spoke) {
        /* The manager went away: the default chain re-owns it. */
        g_desktop_font_spoke = false;
        if (!g_font_override) {
            g_font_px = DEFAULT_FONT_PX;
        }
    }
}

/* ---- the portal: org.freedesktop.portal.Settings ------------------------------ */

#define PORTAL_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_IFACE "org.freedesktop.portal.Settings"
#define INTERFACE_NS "org.gnome.desktop.interface"

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
    if (ns != NULL && key != NULL && strcmp(ns, INTERFACE_NS) == 0) {
        const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[2]);
        if (strcmp(key, "font-name") == 0) {
            int pt = parse_font_points(fdk_dbus_value_string(v));
            if (pt > 0) {
                int px = points_to_px(pt);
                g_desktop_font_spoke = true;
                g_desktop_font_px = px;
                if (!g_font_override) {
                    g_font_px = px;
                }
            }
        } else if (strcmp(key, "text-scaling-factor") == 0) {
            double s = fdk_dbus_value_double(v);
            if (s >= 0.5 && s <= 4.0) {
                g_desktop_scale_spoke = true;
                g_desktop_scale = s;
                if (!g_scale_override) {
                    g_scale = s;
                }
            }
        }
    }
    fdk_dbus_values_free_array(args, n);
}

/* ReadOne's reply: one argument, the value as a VARIANT. */
static void portal_readone_font_reply(fdk_dbus_connection *c,
                                      fdk_dbus_message *m, void *user) {
    (void)c;
    (void)user;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return; /* this portal does not carry the interface namespace */
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL) {
        return;
    }
    const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[0]);
    int pt = parse_font_points(fdk_dbus_value_string(v));
    if (pt > 0) {
        int px = points_to_px(pt);
        g_desktop_font_spoke = true;
        g_desktop_font_px = px;
        if (!g_font_override) {
            g_font_px = px;
        }
        FDK_INFO("ui-metrics: the portal's font statement is %d pt "
                 "(%d px)",
                 pt, px);
    }
    fdk_dbus_values_free_array(args, n);
}

static void portal_readone_scale_reply(fdk_dbus_connection *c,
                                       fdk_dbus_message *m, void *user) {
    (void)c;
    (void)user;
    if (m == NULL || fdk_dbus_message_is_error(m)) {
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(m, &args, &n) != FDK_OK || n < 1 ||
        args == NULL) {
        return;
    }
    const fdk_dbus_value *v = fdk_dbus_value_variant_value(args[0]);
    double s = fdk_dbus_value_double(v);
    if (s >= 0.5 && s <= 4.0) {
        g_desktop_scale_spoke = true;
        g_desktop_scale = s;
        if (!g_scale_override) {
            g_scale = s;
        }
        FDK_INFO("ui-metrics: the desktop's text scale is %.2f", s);
    }
    fdk_dbus_values_free_array(args, n);
}

static void portal_readone_string(fdk_dbus_connection *c, const char *key,
                                  fdk_dbus_reply_fn fn) {
    fdk_dbus_message *m = fdk_dbus_message_new_method_call(
        PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE, "ReadOne");
    if (m != NULL &&
        fdk_dbus_message_append_string(m, INTERFACE_NS) == FDK_OK &&
        fdk_dbus_message_append_string(m, key) == FDK_OK) {
        (void)fdk_dbus_connection_call(c, m, FDK_DBUS_TIMEOUT_DEFAULT, fn,
                                       NULL);
    } else {
        fdk_dbus_message_free(m);
    }
}

static void portal_ready(fdk_dbus_connection *c, int ok, const char *err,
                         void *user) {
    (void)user;
    if (!ok) {
        FDK_INFO("ui-metrics: portal connect failed (%s); the desktop "
                 "font statement will be retried later",
                 err ? err : "?");
        if (g_portal == c) {
            g_portal = NULL;
            fdk_dbus_connection_close(c);
        }
        return;
    }
    fdk_dbus_connection_set_death_handler(c, portal_death, NULL);

    /* The live subscription FIRST (no window before the watch). */
    char *rule = fdk_dbus_match_rule("signal", NULL, PORTAL_IFACE,
                                     "SettingChanged", NULL, INTERFACE_NS);
    if (rule != NULL) {
        fdk_dbus_subscription *sub =
            fdk_dbus_connection_subscribe(c, rule, portal_setting_changed,
                                          NULL);
        if (sub == NULL) {
            FDK_INFO("ui-metrics: could not subscribe to the portal's "
                     "SettingChanged; only the initial read applies");
        }
        free(rule);
    }

    portal_readone_string(c, "font-name", portal_readone_font_reply);
    portal_readone_string(c, "text-scaling-factor",
                          portal_readone_scale_reply);
}

/* The pump's tick (window.c's scheme tick calls this): attach the
 * portal when the XSETTINGS channel has not spoken. Throttled like
 * the color-scheme module's (30 s between attempts; a session bus
 * appearing late still gets followed). */
void fdk__ui_metrics_portal_tick(fdk_context *ctx) {
    if (g_shut_down || g_portal != NULL || ctx == NULL) {
        return;
    }
    if (g_desktop_font_spoke && g_desktop_scale_spoke) {
        return; /* XSETTINGS owns the answers on this session */
    }
    uint64_t now = now_ms();
    if (g_portal_failed && g_portal_last_try_ms != 0 &&
        now < g_portal_last_try_ms + 30000u) {
        return;
    }
    g_portal_last_try_ms = now;
    g_portal = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, portal_ready,
                                NULL);
    if (g_portal == NULL) {
        g_portal_failed = true;
    }
}

void fdk__ui_metrics_shutdown(void) {
    g_shut_down = true;
    if (g_portal != NULL) {
        fdk_dbus_connection_close(g_portal);
        g_portal = NULL;
    }
}

/* The settings-file change hook: the store leg may have changed. */
void fdk__ui_metrics_invalidate(void) {
    if (g_shut_down) {
        return;
    }
    g_font_px = -1;
    g_scale = -1.0;
    g_font_override = false;
    g_scale_override = false;
    resolve();
}

/* ---- the public getters --------------------------------------------------------- */

int fdk_ui_font_size(void) {
    resolve();
    return g_font_px;
}

double fdk_ui_scale(void) {
    resolve();
    return g_scale;
}

int fdk__ui_derived_px(int reference_px) {
    if (reference_px <= 0) {
        return MIN_FONT_PX;
    }
    double d = (double)reference_px * (double)fdk_ui_font_px() / 14.0;
    int px = (int)(d + 0.5);
    if (px < MIN_FONT_PX) {
        px = MIN_FONT_PX;
    }
    if (px > MAX_FONT_PX) {
        px = MAX_FONT_PX;
    }
    return px;
}

int fdk_ui_font_px(void) {
    resolve();
    int px = (int)((double)g_font_px * g_scale + 0.5);
    if (px < MIN_FONT_PX) {
        px = MIN_FONT_PX;
    }
    if (px > MAX_FONT_PX) {
        px = MAX_FONT_PX;
    }
    return px;
}
