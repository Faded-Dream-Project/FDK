/* main.c — fdk-monitor's boot: the shell, the menu bar, the
 * levelbars, the 250 ms sampling timer, the prefs, the shortcuts.
 * The README's contract: /proc + /sys parsing, canvas graphs with
 * animation, tray pixmaps, notifications with actions, 250 ms
 * timers — every leg on FDK's public API only. */
#include "monitor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the sampler timer -------------------------------------------- */

static fdk_timer *g_timer;

static void on_tick(fdk_timer *timer, void *user) {
    (void)timer;
    (void)user;
    mo_sample_all();
    /* the levelbars (0..1 fractions) */
    if (mapp.cpu_level != NULL) {
        fdk_levelbar_set_value(mapp.cpu_level,
                               mapp.cpu.load.latest / 100.0);
    }
    if (mapp.mem_level != NULL) {
        fdk_levelbar_set_value(mapp.mem_level,
                               mapp.mem.used_frac.latest);
    }
    if (mapp.swap_level != NULL && mapp.mem.swap_total_mb > 0) {
        fdk_levelbar_set_value(
            mapp.swap_level,
            mapp.mem.swap_used_mb / mapp.mem.swap_total_mb);
    }
    mo_graphs_tick();
    mo_tray_tick();
    mo_notify_check();
    /* the status bar is the numeric twin of the graphs            */
    char rx[32], tx[32];
    if (mapp.net.rx_rate > 1048576.0) {
        snprintf(rx, sizeof rx, "%.1f MiB/s",
                 mapp.net.rx_rate / 1048576.0);
    } else {
        snprintf(rx, sizeof rx, "%.0f KiB/s",
                 mapp.net.rx_rate / 1024.0);
    }
    if (mapp.net.tx_rate > 1048576.0) {
        snprintf(tx, sizeof tx, "%.1f MiB/s",
                 mapp.net.tx_rate / 1048576.0);
    } else {
        snprintf(tx, sizeof tx, "%.0f KiB/s",
                 mapp.net.tx_rate / 1024.0);
    }
    ta_status(&mapp.base,
              "CPU %.0f%%   RAM %.0f/%.0f MB   NET ↓%s ↑%s   "
              "(%u samples)",
              mapp.cpu.load.latest, mapp.mem.used_mb,
              mapp.mem.total_mb, rx, tx, mapp.samples);
}

void mo_set_running(bool run) {
    mapp.running = run;
    if (run && g_timer == NULL) {
        g_timer = fdk_timer_add(mapp.base.ctx, mapp.interval_ms,
                                true, on_tick, NULL);
    } else if (!run && g_timer != NULL) {
        fdk_timer_remove(g_timer);
        g_timer = NULL;
        ta_status(&mapp.base, "%s", ta_("Sampling paused"));
    }
}

/* ---- the menus ------------------------------------------------------ */

static void menu_quit(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    mapp.base.quit = true;
}

/* the tray menu's quit row (tray_notify.c forwards here) */
void mo_tray_on_quit(void) {
    mapp.base.quit = true;
}

static void menu_pause(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    mo_set_running(!mapp.running);
}

static void menu_test_note(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    mo_notify_test();
}

static void menu_interval(fdk_widget *mi, void *u);

static void menu_about(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("About fdk-monitor");
    o.text = "fdk-monitor — the FDK validation-suite system "
             "monitor.\n/proc sampling at 250 ms, canvas graphs,\n"
             "tray sparkline, threshold notifications.";
    (void)fdk_dialog_show_message(mapp.base.ctx, &o, NULL, NULL,
                                  NULL);
}

/* the item-activate trampoline (the shared one lives in fdk-files;
 * a test app keeps its build self-contained) */
typedef struct {
    void (*fn)(fdk_widget *, void *);
    void *arg;
} mo_menu_act;
static mo_menu_act g_acts[32];
static size_t g_act_count;

/* the menus are APP-OWNED (the menu contract): keep the handles,
 * destroy them after the bar dies at shutdown                      */
static fdk_menu *g_menus[8];
static size_t g_menu_count;

static void act_trampoline(fdk_menu_item *it, void *user) {
    (void)it;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx < g_act_count && g_acts[idx].fn != NULL) {
        g_acts[idx].fn(NULL, g_acts[idx].arg);
    }
}

static void menu_add(fdk_menu *m, const char *label,
                     const char *shortcut,
                     void (*fn)(fdk_widget *, void *)) {
    fdk_menu_item *it = NULL;
    if (fdk_menu_append(m, label, &it) == FDK_OK && it != NULL) {
        if (shortcut != NULL && shortcut[0] != '\0') {
            (void)fdk_menu_item_set_shortcut(it, shortcut);
        }
        if (fn != NULL && g_act_count < 32) {
            g_acts[g_act_count].fn = fn;
            g_acts[g_act_count].arg = NULL;
            fdk_menu_item_set_on_activate(
                it, act_trampoline, (void *)(uintptr_t)g_act_count);
            g_act_count++;
        }
    }
}

/* the interval items: one handler, the arg carries the ms */
static void menu_interval(fdk_widget *mi, void *u) {
    (void)mi;
    int ms = (int)(uintptr_t)u;
    mapp.interval_ms = (ms == 1000) ? 1000u : 250u;
    if (mapp.running) {
        mo_set_running(false);
        mo_set_running(true); /* re-arm at the new cadence        */
    }
    (void)fdk_prefs_set_int(mapp.base.prefs, "monitor.interval",
                            (int)mapp.interval_ms);
    ta_status(&mapp.base, "Sampling every %u ms", mapp.interval_ms);
}

static void menus_build(void) {
    fdk_font *f = mapp.base.font_ui;
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(mapp.base.menubar_host, f, &bar);

    fdk_menu *m_mon = NULL;
    (void)fdk_menu_create(f, &m_mon);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_mon;
    }
    menu_add(m_mon, ta_("&Pause/resume sampling"), "Ctrl+P",
             menu_pause);
    menu_add(m_mon, ta_("&Test notification"), NULL, menu_test_note);
    menu_add(m_mon, ta_("&Quit"), "Ctrl+Q", menu_quit);
    (void)fdk_menu_bar_append(bar, ta_("&Monitor"), m_mon);

    fdk_menu *m_view = NULL;
    (void)fdk_menu_create(f, &m_view);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_view;
    }
    menu_add(m_view, ta_("Sample every &250 ms"), NULL,
             menu_interval);
    g_acts[g_act_count - 1].arg = (void *)(uintptr_t)250;
    menu_add(m_view, ta_("Sample every &1000 ms"), NULL,
             menu_interval);
    g_acts[g_act_count - 1].arg = (void *)(uintptr_t)1000;
    (void)fdk_menu_bar_append(bar, ta_("&View"), m_view);

    fdk_menu *m_help = NULL;
    (void)fdk_menu_create(f, &m_help);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_help;
    }
    menu_add(m_help, ta_("&About"), NULL, menu_about);
    (void)fdk_menu_bar_append(bar, ta_("&Help"), m_help);
}

static void menus_shutdown(void) {
    /* after ta_close tore the bar down, the models are ours to free */
    for (size_t i = 0; i < g_menu_count; i++) {
        if (g_menus[i] != NULL) {
            fdk_menu_destroy(g_menus[i]);
        }
    }
    g_menu_count = 0;
}

/* ---- the levelbar row ------------------------------------------------ */

static fdk_widget *level_row(fdk_widget *parent, const char *name,
                             fdk_widget **out) {
    fdk_widget *row = NULL;
    (void)fdk_widget_create(parent, NULL,
                            (fdk_rect){0, 0, 420, 26}, &row);
    fdk_widget *lbl = NULL;
    (void)fdk_label_create(row, mapp.base.font_ui, name, &lbl);
    fdk_widget_set_bounds(lbl, (fdk_rect){8, 4, 120, 20});
    fdk_widget_set_accessible_name(lbl, name);
    fdk_widget *lb = NULL;
    (void)fdk_levelbar_create(row, 0.0, 1.0, &lb);
    fdk_levelbar_set_mode(lb, FDK_LEVELBAR_CONTINUOUS);
    fdk_widget_set_bounds(lb, (fdk_rect){136, 3, 260, 20});
    fdk_widget_set_accessible_name(lb, name);
    *out = lb;
    return row;
}

/* ---- boot ------------------------------------------------------------ */

static void sh_pause(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    mo_set_running(!mapp.running);
}

int main(void) {
    memset(&mapp, 0, sizeof mapp);
    if (!ta_init(&mapp.base, "fdk-monitor")) {
        return 1;
    }
    (void)ta_i18n_load(&mapp.base, PO_DIR, "ui.language");
    mapp.interval_ms =
        (unsigned)ta_pref_int(&mapp.base, "monitor.interval", 250);
    if (mapp.interval_ms != 250 && mapp.interval_ms != 1000) {
        mapp.interval_ms = 250;
    }
    if (!ta_open(&mapp.base, "fdk-monitor — System Monitor", 980,
                 640)) {
        ta_close(&mapp.base);
        return 1;
    }

    /* the levelbar rows (CPU / RAM / swap) */
    fdk_widget *rows = NULL;
    (void)fdk_widget_create(mapp.base.content, NULL,
                            (fdk_rect){8, 6, 440, 96}, &rows);
    fdk_widget *r = level_row(rows, "CPU", &mapp.cpu_level);
    fdk_widget_set_bounds(r, (fdk_rect){0, 0, 436, 26});
    r = level_row(rows, "RAM", &mapp.mem_level);
    fdk_widget_set_bounds(r, (fdk_rect){0, 32, 436, 26});
    r = level_row(rows, "Swap", &mapp.swap_level);
    fdk_widget_set_bounds(r, (fdk_rect){0, 64, 436, 26});

    mo_graphs_build();
    menus_build();
    (void)fdk_window_add_shortcut(mapp.base.window, "Ctrl+P",
                                  sh_pause, NULL);

    mo_notify_boot();
    mo_tray_notify_boot();

    /* the first sample now (so the graphs never start empty), the
     * timer carries it from here                                 */
    mo_sample_all();
    mo_set_running(true);
    on_tick(NULL, NULL);

    ta_status(&mapp.base, "%s", ta_("Sampling"));
    ta_run(&mapp.base);

    mo_set_running(false); /* retire the sampler before the world  */
                           /* teardown (the pump's live-timer warn) */
    mo_tray_notify_shutdown();
    (void)fdk_prefs_set_int(mapp.base.prefs, "monitor.interval",
                            (int)mapp.interval_ms);
    ta_close(&mapp.base);
    menus_shutdown();
    return 0;
}
