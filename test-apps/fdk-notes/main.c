/* main.c — fdk-notes' boot: the shell, the menus, the shortcuts,
 * the seed content, the shutdown. */
#include "notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../common/ta_buf.h"

void nt_quit(void) {
    napp.base.quit = true;
}

/* ---- the menus ------------------------------------------------------ */

static void menu_new_note(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    if (napp.book_count == 0) {
        (void)nt_book_add("Notebook 1");
    }
    if (nt_note_add(0, "Untitled note")) {
        nt_tree_refresh();
        nt_focus_select(napp.note_count - 1);
    }
}

static void menu_new_book(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    char name[32];
    snprintf(name, sizeof name, "Notebook %zu",
             napp.book_count + 1);
    if (nt_book_add(name)) {
        nt_tree_refresh();
    }
}

/* Round 22: Bold/Italic apply to the SELECTION (the per-range
 * rich-text editor; the whole-note flags are gone) */
static void menu_bold(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    if (!nt_format_selection(true, false)) {
        ta_status(&napp.base, "%s", ta_("Select text first"));
    }
}

static void menu_italic(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    if (!nt_format_selection(false, true)) {
        ta_status(&napp.base, "%s", ta_("Select text first"));
    }
}

static void menu_focus(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    nt_focus_toggle();
}

static void menu_remind_5(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    nt_note *n = nt_note_current();
    if (n != NULL) {
        n->reminder_at = (long)time(NULL) + 300;
        n->reminder_fired = false;
        ta_status(&napp.base, "%s", ta_("Reminder in 5 minutes"));
    }
}

static void menu_about(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("About fdk-notes");
    o.text = "fdk-notes — the FDK validation-suite notes app.\n"
             "Notebooks + notes tree, rich text, reminders,\n"
             "focus timer, a11y narration, tray.";
    (void)fdk_dialog_show_message(napp.base.ctx, &o, NULL, NULL,
                                  NULL);
}

static void menu_quit(fdk_widget *mi, void *u) {
    (void)mi;
    (void)u;
    nt_quit();
}

/* the item-activate trampoline */
typedef struct {
    void (*fn)(fdk_widget *, void *);
} nt_menu_act;
static nt_menu_act g_acts[32];
static size_t g_act_count;

static void act_trampoline(fdk_menu_item *it, void *user) {
    (void)it;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx < g_act_count && g_acts[idx].fn != NULL) {
        g_acts[idx].fn(NULL, NULL);
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
            fdk_menu_item_set_on_activate(
                it, act_trampoline, (void *)(uintptr_t)g_act_count);
            g_act_count++;
        }
    }
}

static fdk_menu *g_menus[8];
static size_t g_menu_count;

static void menus_build(void) {
    fdk_font *f = napp.base.font_ui;
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(napp.base.menubar_host, f, &bar);

    fdk_menu *m_file = NULL;
    (void)fdk_menu_create(f, &m_file);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_file;
    }
    menu_add(m_file, ta_("New &note"), "Ctrl+N", menu_new_note);
    menu_add(m_file, ta_("New note&book"), NULL, menu_new_book);
    menu_add(m_file, ta_("&Quit"), "Ctrl+Q", menu_quit);
    (void)fdk_menu_bar_append(bar, ta_("&File"), m_file);

    fdk_menu *m_fmt = NULL;
    (void)fdk_menu_create(f, &m_fmt);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_fmt;
    }
    menu_add(m_fmt, ta_("&Bold"), "Ctrl+B", menu_bold);
    menu_add(m_fmt, ta_("&Italic"), "Ctrl+I", menu_italic);
    (void)fdk_menu_bar_append(bar, ta_("For&mat"), m_fmt);

    fdk_menu *m_tools = NULL;
    (void)fdk_menu_create(f, &m_tools);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_tools;
    }
    menu_add(m_tools, ta_("&Focus timer (25 min)"), "Ctrl+D",
             menu_focus);
    menu_add(m_tools, ta_("Remind in &5 minutes"), NULL,
             menu_remind_5);
    (void)fdk_menu_bar_append(bar, ta_("&Tools"), m_tools);

    fdk_menu *m_help = NULL;
    (void)fdk_menu_create(f, &m_help);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_help;
    }
    menu_add(m_help, ta_("&About"), NULL, menu_about);
    (void)fdk_menu_bar_append(bar, ta_("&Help"), m_help);
}

static void menus_shutdown(void) {
    for (size_t i = 0; i < g_menu_count; i++) {
        if (g_menus[i] != NULL) {
            fdk_menu_destroy(g_menus[i]);
        }
    }
    g_menu_count = 0;
}

/* ---- shortcuts -------------------------------------------------------- */

static void sh_new(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    menu_new_note(NULL, NULL);
}
static void sh_bold(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    menu_bold(NULL, NULL);
}
static void sh_italic(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    menu_italic(NULL, NULL);
}
static void sh_focus(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    nt_focus_toggle();
}
static void sh_quit(fdk_window *w, void *u) {
    (void)w;
    (void)u;
    nt_quit();
}

/* ---- boot -------------------------------------------------------------- */

int main(void) {
    memset(&napp, 0, sizeof napp);
    if (!ta_init(&napp.base, "fdk-notes")) {
        return 1;
    }
    (void)ta_i18n_load(&napp.base, PO_DIR, "ui.language");
    nt_load_all();
    if (!ta_open(&napp.base, "fdk-notes — Notes", 960, 460)) {
        ta_close(&napp.base);
        return 1;
    }

    nt_ui_build();
    menus_build();
    (void)fdk_window_add_shortcut(napp.base.window, "Ctrl+N",
                                  sh_new, NULL);
    (void)fdk_window_add_shortcut(napp.base.window, "Ctrl+B",
                                  sh_bold, NULL);
    (void)fdk_window_add_shortcut(napp.base.window, "Ctrl+I",
                                  sh_italic, NULL);
    (void)fdk_window_add_shortcut(napp.base.window, "Ctrl+D",
                                  sh_focus, NULL);
    (void)fdk_window_add_shortcut(napp.base.window, "Ctrl+Q",
                                  sh_quit, NULL);

    /* the seed content on a first boot (a real user never opens an
     * empty notes app; the smoke rig gets something to paint)    */
    if (napp.note_count == 0) {
        (void)nt_book_add("Personal");
        (void)nt_note_add(0, "Welcome to fdk-notes");
        nt_note *n = nt_note_current();
        if (n != NULL) {
            const char *seed =
                "This note lives in the FDK validation app.\n\n"
                "Try: Ctrl+B bold, Ctrl+I italic, Ctrl+D the "
                "focus timer,\nTools > Remind in 5 minutes.";
            ta_copy(n->body, sizeof n->body, seed);
            n->body_len = strlen(n->body);
        }
    }
    nt_tree_refresh();
    nt_focus_select(0);

    nt_a11y_boot();
    nt_notify_boot();
    nt_tray_boot();

    ta_status(&napp.base, "%s", ta_("Ready"));
    ta_run(&napp.base);

    nt_save_all();
    nt_tray_shutdown();
    nt_notify_shutdown();
    nt_a11y_shutdown();
    if (napp.reminder_timer != NULL) {
        fdk_timer_remove(napp.reminder_timer);
        napp.reminder_timer = NULL;
    }
    if (napp.focus_timer != NULL) {
        fdk_timer_remove(napp.focus_timer);
        napp.focus_timer = NULL;
    }
    ta_close(&napp.base);
    menus_shutdown();
    return 0;
}
