/* tray_notify.c — fdk-monitor's desktop half: the tray item whose
 * PIXMAP is a live CPU sparkline (redrawn from the same series the
 * canvas plots), the tray menu, and the threshold notification
 * with an action. */
#include "monitor.h"

#include <stdio.h>
#include <string.h>

static fdk_tray_item *g_tray;
static fdk_tray_menu *g_menu;
static fdk_notify *g_notify;
static bool g_alert_armed = true; /* re-arms when load falls back   */

#define TRAY_SZ 22

/* the tray pixmap: ARGB32 network byte order (the SNI contract) */
static uint8_t g_tray_px[TRAY_SZ * TRAY_SZ * 4];

static void tray_pixmap_render(void) {
    const mo_series *s = &mapp.cpu.load;
    fdk_color accent = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    fdk_color bg = {0, 0, 0, 0}; /* transparent corners             */
    if (accent.a == 0) {
        accent.r = 0x35;
        accent.g = 0x84;
        accent.b = 0xe4;
        accent.a = 255;
    }
    /* paint right-to-left: newest sample at the right edge         */
    for (size_t y = 0; y < TRAY_SZ; y++) {
        for (size_t x = 0; x < TRAY_SZ; x++) {
            uint8_t *px = &g_tray_px[(y * TRAY_SZ + x) * 4];
            /* ARGB32 network order: A R G B, channels 0..255      */
            px[0] = (uint8_t)(bg.a * 255.0f + 0.5f);
            px[1] = (uint8_t)(bg.r * 255.0f + 0.5f);
            px[2] = (uint8_t)(bg.g * 255.0f + 0.5f);
            px[3] = (uint8_t)(bg.b * 255.0f + 0.5f);
        }
    }
    if (s->count < 2) {
        return;
    }
    int avail = TRAY_SZ - 2;
    int n = (int)s->count;
    if (n > avail) {
        n = avail;
    }
    for (int i = 0; i < n; i++) {
        double v = s->hist[s->count - (size_t)n + (size_t)i];
        if (v > 100.0) {
            v = 100.0;
        }
        int h = (int)((v / 100.0) * (TRAY_SZ - 4)) + 1;
        size_t x = 1u + (size_t)i;
        for (int dy = 0; dy < h; dy++) {
            int py_i = TRAY_SZ - 2 - dy;
            if (py_i < 1) {
                break;
            }
            size_t py = (size_t)py_i;
            uint8_t *px = &g_tray_px[(py * TRAY_SZ + x) * 4];
            px[0] = (uint8_t)(accent.a * 255.0f + 0.5f);
            px[1] = (uint8_t)(accent.r * 255.0f + 0.5f);
            px[2] = (uint8_t)(accent.g * 255.0f + 0.5f);
            px[3] = (uint8_t)(accent.b * 255.0f + 0.5f);
        }
    }
}

static void tray_pixmap_push(void) {
    if (g_tray == NULL) {
        return;
    }
    fdk_tray_item_set_icon_pixmap(g_tray, TRAY_SZ, TRAY_SZ,
                                  g_tray_px, sizeof g_tray_px);
}

/* the menu's click sink: id 1 = pause/resume, 2 = notify test,
 * 3 = quit (mo_tray_on_quit wired by main.c) */
void mo_tray_on_quit(void);
static void on_menu_click(fdk_tray_menu *menu, uint32_t id,
                          void *user) {
    (void)menu;
    (void)user;
    extern void mo_set_running(bool run);
    if (id == 1) {
        mo_set_running(!mapp.running);
    } else if (id == 2) {
        mo_notify_test();
    } else if (id == 3) {
        mo_tray_on_quit();
    }
}

static void on_tray_activate(fdk_tray_item *item, int secondary,
                             int32_t x, int32_t y, void *user) {
    (void)item;
    (void)secondary;
    (void)x;
    (void)y;
    (void)user;
    /* a plain activation is the classic monitor toggle: pause or
     * resume the sampling                                   */
    extern void mo_set_running(bool run);
    mo_set_running(!mapp.running);
}

void mo_tray_notify_boot(void) {
    fdk_tray_item_options opts;
    memset(&opts, 0, sizeof opts);
    opts.id = "fdk-monitor-cpu";
    opts.title = "fdk-monitor";
    opts.category = "System";
    fdk_tray_item_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.on_activate = on_tray_activate;
    g_tray = fdk_tray_item_open(mapp.base.ctx, &opts, &cb, NULL);
    if (g_tray == NULL) {
        return; /* no bus: the app still runs, tray-less (logged)   */
    }
    g_menu = fdk_tray_menu_create();
    (void)fdk_tray_menu_add(g_menu, 0, "_Pause/Resume sampling");
    (void)fdk_tray_menu_add(g_menu, 0, "_Test notification");
    (void)fdk_tray_menu_add_separator(g_menu, 0);
    (void)fdk_tray_menu_add(g_menu, 0, "_Quit");
    fdk_tray_menu_on_click(g_menu, on_menu_click, NULL);
    fdk_tray_item_set_menu(g_tray, g_menu);
    fdk_tray_item_set_tooltip(g_tray, NULL, "fdk-monitor",
                              "CPU / memory / network sampler");
    fdk_tray_item_set_status(g_tray, FDK_TRAY_STATUS_ACTIVE);
    tray_pixmap_render();
    tray_pixmap_push();
}

void mo_tray_tick(void) {
    if (g_tray == NULL) {
        return;
    }
    tray_pixmap_render();
    tray_pixmap_push();
    char tip[160];
    snprintf(tip, sizeof tip, "CPU %.0f%%  RAM %.0f/%.0f MB",
             mapp.cpu.load.latest, mapp.mem.used_mb,
             mapp.mem.total_mb);
    fdk_tray_item_set_tooltip(g_tray, NULL, "fdk-monitor", tip);
}

void mo_tray_notify_shutdown(void) {
    if (g_tray != NULL) {
        fdk_tray_item_close(g_tray);
        g_tray = NULL;
    }
    if (g_menu != NULL) {
        fdk_tray_menu_close(g_menu);
        g_menu = NULL;
    }
    if (g_notify != NULL) {
        fdk_notify_close(g_notify);
        g_notify = NULL;
    }
}

/* ---- notifications ---------------------------------------------------- */

static void on_note_action(fdk_notify *n, uint32_t id, const char *action,
                           void *note_user, void *user) {
    (void)n;
    (void)id;
    (void)note_user;
    (void)user;
    if (action != NULL && strcmp(action, "mute") == 0) {
        g_alert_armed = false; /* stay quiet until load recovers    */
    }
}

static void on_note_failed(fdk_notify *n, void *note_user,
                           const char *error, void *user) {
    (void)n;
    (void)note_user;
    (void)user;
    (void)error; /* a failed send is not fatal: the tooltip carries
                    the numbers                                 */
}

void mo_notify_boot(void) {
    fdk_notify_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.on_action = on_note_action;
    cb.on_failed = on_note_failed;
    g_notify = fdk_notify_open(mapp.base.ctx, &cb);
}

void mo_notify_test(void) {
    if (g_notify == NULL) {
        return;
    }
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "fdk-monitor";
    req.summary = "fdk-monitor";
    req.body = "The notification lane works.";
    req.timeout_ms = 4000;
    static const char *actions[] = {"mute", "Mute alerts", NULL};
    req.actions = actions;
    (void)fdk_notify_send(g_notify, &req, NULL);
}

/* the threshold alert: one notification per load excursion (the
 * mute action or a recovery below 70% re-arms it) */
void mo_notify_check(void) {
    if (g_notify == NULL || !g_alert_armed) {
        return;
    }
    if (mapp.cpu.load.latest >= 90.0 && mapp.samples > 4) {
        char body[160];
        snprintf(body, sizeof body,
                 "CPU load %.0f%% — sustained saturation.",
                 mapp.cpu.load.latest);
        fdk_notify_request req;
        memset(&req, 0, sizeof req);
        req.app_name = "fdk-monitor";
        req.summary = "CPU load high";
        req.body = body;
        req.urgency = FDK_NOTIFY_URGENCY_CRITICAL;
        req.timeout_ms = 6000;
        static const char *actions[] = {"mute", "Mute alerts", NULL};
        req.actions = actions;
        (void)fdk_notify_send(g_notify, &req, NULL);
        g_alert_armed = false;
    } else if (mapp.cpu.load.latest < 70.0) {
        g_alert_armed = true;
    }
}
