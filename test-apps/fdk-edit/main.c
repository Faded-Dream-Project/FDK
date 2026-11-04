/* main.c — fdk-edit's boot: the app shell, the session restore, the
 * menu bar, the unsaved-changes ask, the prefs dialog. */
#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>

/* doc.c's gutter re-arm (the View/prefs toggles) */
void ed_arm_gutter(ed_doc *d);

/* ui.c's Edit-menu twins (round 21's menu wiring) */
void ed_menu_undo(fdk_widget *mi, void *u);
void ed_menu_redo(fdk_widget *mi, void *u);
void ed_menu_find(fdk_widget *mi, void *u);
void ed_menu_replace(fdk_widget *mi, void *u);

/* ---- the menus ------------------------------------------------------ */

static void menu_new(fdk_widget *mi, void *u) { (void)mi; (void)u; ed_doc_new(); }
static void menu_open(fdk_widget *mi, void *u);
static void menu_save(fdk_widget *mi, void *u) { (void)mi; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) { (void)ed_doc_save(d, false); } }
static void menu_save_as(fdk_widget *mi, void *u) { (void)mi; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) { (void)ed_doc_save(d, true); } }
static void menu_close_tab(fdk_widget *mi, void *u) { (void)mi; (void)u;
    ed_doc_close(eapp.current); }
static void menu_quit(fdk_widget *mi, void *u) { (void)mi; (void)u;
    eapp.base.quit = true; }
static void menu_wrap(fdk_widget *mi, void *u) { (void)mi; (void)u;
    eapp.wrap = !eapp.wrap;
    for (size_t i = 0; i < eapp.doc_count; i++) {
        fdk_textview_set_wrap(eapp.docs[i].view,
                              eapp.wrap ? FDK_TEXTVIEW_WRAP_WORD
                                        : FDK_TEXTVIEW_WRAP_NONE);
    }
    ed_ui_status(); }
static void menu_highlight(fdk_widget *mi, void *u) { (void)mi; (void)u;
    eapp.highlight = !eapp.highlight;
    for (size_t i = 0; i < eapp.doc_count; i++) {
        ed_highlight(&eapp.docs[i]);
    } }
static void menu_gutter(fdk_widget *mi, void *u) { (void)mi; (void)u;
    eapp.show_gutter = !eapp.show_gutter;
    for (size_t i = 0; i < eapp.doc_count; i++) {
        if (eapp.show_gutter) {
            ed_arm_gutter(&eapp.docs[i]);
        } else {
            (void)fdk_textview_set_gutter(eapp.docs[i].view, 0,
                                          NULL, NULL);
        }
    } }
static void menu_about(fdk_widget *mi, void *u) { (void)mi; (void)u;
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("About fdk-edit");
    o.text = "fdk-edit — the FDK validation-suite text editor.\n"
             "Tabs, undo/redo, find/replace (plain + regex),\n"
             "syntax highlighting, UTF-8/BOM/UTF-16, CRLF/LF.";
    (void)fdk_dialog_show_message(eapp.base.ctx, &o, NULL, NULL,
                                  NULL); }

static void menu_recent(fdk_widget *mi, void *u) {
    (void)mi;
    ed_doc_open((const char *)u);
}


/* the item-activate trampoline: stores (fn, arg) pairs */
typedef struct {
    void (*fn)(fdk_widget *, void *);
    void *arg;
} ed_menu_act;
static ed_menu_act g_acts[64];
static size_t g_act_count;

/* the menus are APP-OWNED (the menu contract): the handles live
 * here and die after the bar at shutdown (round 21 — the smoke
 * LSan leg caught the leak)                                    */
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
                     void (*fn)(fdk_widget *, void *), void *arg) {
    fdk_menu_item *it = NULL;
    if (fdk_menu_append(m, label, &it) == FDK_OK && it != NULL) {
        if (shortcut != NULL && shortcut[0] != '\0') {
            (void)fdk_menu_item_set_shortcut(it, shortcut);
        }
        if (fn != NULL && g_act_count < 64) {
            g_acts[g_act_count].fn = fn;
            g_acts[g_act_count].arg = arg;
            fdk_menu_item_set_on_activate(
                it, act_trampoline,
                (void *)(uintptr_t)g_act_count);
            g_act_count++;
        }
    }
}

static void menus_build(void) {
    fdk_font *f = eapp.base.font_ui;
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(eapp.base.menubar_host, f, &bar);

    fdk_menu *m_file = NULL;
    (void)fdk_menu_create(f, &m_file);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_file;
    }
    menu_add(m_file, ta_("&New"), "Ctrl+N", menu_new, NULL);
    menu_add(m_file, ta_("&Open..."), "Ctrl+O", menu_open, NULL);
    for (size_t i = 0; i < eapp.recent_count && i < 5; i++) {
        menu_add(m_file, eapp.recent[i], NULL, menu_recent,
                 eapp.recent[i]);
    }
    menu_add(m_file, ta_("&Save"), "Ctrl+S", menu_save, NULL);
    menu_add(m_file, ta_("Save &As..."), "Ctrl+Shift+S",
             menu_save_as, NULL);
    menu_add(m_file, ta_("&Close Tab"), NULL, menu_close_tab, NULL);
    menu_add(m_file, ta_("&Quit"), "Ctrl+Q", menu_quit, NULL);
    (void)fdk_menu_bar_append(bar, ta_("&File"), m_file);

    fdk_menu *m_edit = NULL;
    (void)fdk_menu_create(f, &m_edit);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_edit;
    }
    /* round 21: the Edit menu was the one place the app's actions
     * were not wired (toolbar + shortcuts worked, menu said NULL —
     * the four-app standard says the menu is the discoverable twin
     * of every shortcut). ui.c owns the real action bodies. */
    menu_add(m_edit, ta_("&Undo"), "Ctrl+Z", ed_menu_undo, NULL);
    menu_add(m_edit, ta_("&Redo"), "Ctrl+Shift+Z", ed_menu_redo, NULL);
    menu_add(m_edit, ta_("&Find..."), "Ctrl+F", ed_menu_find, NULL);
    menu_add(m_edit, ta_("Rep&lace..."), "Ctrl+H", ed_menu_replace,
             NULL);
    menu_add(m_edit, ta_("&Go To Line..."), "Ctrl+G",
             ed_menu_goto_line, NULL);
    (void)fdk_menu_bar_append(bar, ta_("&Edit"), m_edit);

    fdk_menu *m_view = NULL;
    (void)fdk_menu_create(f, &m_view);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_view;
    }
    menu_add(m_view, ta_("&Word wrap"), NULL, menu_wrap, NULL);
    menu_add(m_view, ta_("&Syntax highlighting"), NULL,
             menu_highlight, NULL);
    menu_add(m_view, ta_("&Line numbers"), NULL, menu_gutter, NULL);
    (void)fdk_menu_bar_append(bar, ta_("&View"), m_view);

    fdk_menu *m_help = NULL;
    (void)fdk_menu_create(f, &m_help);
    if (g_menu_count < 8) {
        g_menus[g_menu_count++] = m_help;
    }
    menu_add(m_help, ta_("&About"), NULL, menu_about, NULL);
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

/* the open menu item (shares the toolbar's picker path) */
static void menu_open(fdk_widget *mi, void *u) {
    (void)mi; (void)u;
    extern void ed_open_dialog(void);
    ed_open_dialog();
}

/* ---- the unsaved ask (async) ----------------------------------------- */

typedef struct {
    size_t index; /* the doc being closed                    */
} unsaved_ask;

static void on_unsaved_answer(fdk_dialog_response response,
                              void *user) {
    unsaved_ask *a = user;
    if (response == FDK_DIALOG_OK || response == FDK_DIALOG_YES) {
        ed_doc *d = &eapp.docs[a->index];
        if (d->path[0] != '\0') {
            /* save then close (the plain write, no picker) */
            const char *t = fdk_textview_get_text(d->view);
            if (t != NULL && t != d->text) {
                free(d->text);
                d->text = strdup(t);
                d->text_len = strlen(d->text);
            }
            (void)ed_write_file(d->path, d->text, d->text_len,
                                d->crlf, d->bom);
        }
        ed_doc_close(a->index);
    } else if (response == FDK_DIALOG_NO) {
        ed_doc_close(a->index);
    } /* CANCEL / dismissed: keep the tab */
    free(a);
}

void ed_ask_unsaved(size_t index) {
    if (index >= eapp.doc_count) {
        return;
    }
    ed_doc *d = &eapp.docs[index];
    unsaved_ask *a = malloc(sizeof *a);
    if (a == NULL) {
        return;
    }
    a->index = index;
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("Unsaved changes");
    char msg[600];
    snprintf(msg, sizeof msg, ta_("Save changes to %s before closing?"),
             d->path[0] != '\0' ? d->path : ta_("Untitled"));
    o.text = msg;
    o.buttons = FDK_DIALOG_BUTTONS_YES_NO_CANCEL;
    o.modal = true;
    o.parent = eapp.base.window;
    if (!fdk_ok(fdk_dialog_show_message(eapp.base.ctx, &o,
                                        on_unsaved_answer, a,
                                        NULL))) {
        free(a);
    }
}

/* ---- the prefs dialog (PASS A: a summary; PASS B: live toggles) ----- */

void ed_prefs_dialog(void) {
    static char text[256];
    snprintf(text, sizeof text,
             ta_("Word wrap: %s\nSyntax highlighting: %s\n"
               "Line numbers: %s"),
             eapp.wrap ? ta_("on") : ta_("off"),
             eapp.highlight ? ta_("on") : ta_("off"),
             eapp.show_gutter ? ta_("on") : ta_("off"));
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("Preferences");
    o.text = text;
    o.buttons = FDK_DIALOG_BUTTONS_CLOSE;
    o.parent = eapp.base.window;
    (void)fdk_dialog_show_message(eapp.base.ctx, &o, NULL, NULL,
                                  NULL);
}

/* ---- boot ----------------------------------------------------------- */

int main(void) {
    memset(&eapp, 0, sizeof eapp);
    if (!ta_init(&eapp.base, "fdk-edit")) {
        return 1;
    }
    /* the prefs snapshot BEFORE the UI (the session booleans) */
    ed_session_load();
    /* round 21: the Romanian catalog — fdk-files had one, fdk-edit
     * booted catalog-less (the four-app standard: i18n-ready
     * strings are not optional furniture) */
    (void)ta_i18n_load(&eapp.base, PO_DIR, "ui.language");
    if (!ta_open(&eapp.base, "fdk-edit — Text Editor", 1000, 680)) {
        ta_close(&eapp.base);
        return 1;
    }
    ed_ui_build();
    menus_build();

    /* the session's tabs */
    int want_tabs = ta_pref_int(&eapp.base, "session.tabs", -1);
    int want_cur = ta_pref_int(&eapp.base, "session.current", 0);
    if (want_tabs > 0) {
        for (int i = 0;
             i < want_tabs &&
             eapp.doc_count <
                 sizeof eapp.docs / sizeof *eapp.docs;
             i++) {
            char key[48];
            snprintf(key, sizeof key, "tab.%d.path", i);
            char path[512];
            ta_pref_str(&eapp.base, key, "", path, sizeof path);
            if (path[0] != '\0') {
                struct stat st;
                if (stat(path, &st) == 0) {
                    ed_doc_open(path);
                    ed_doc *d = ed_doc_current();
                    if (d != NULL) {
                        snprintf(key, sizeof key,
                                 "tab.%d.caret", i);
                        int c = ta_pref_int(&eapp.base, key, 0);
                        if (c > 0) {
                            (void)fdk_textview_set_caret(
                                d->view, (size_t)c);
                        }
                    }
                }
            }
        }
    }
    if (eapp.doc_count == 0) {
        ed_doc_new(); /* never an empty editor */
    }
    if (want_cur >= 0 && (size_t)want_cur < eapp.doc_count) {
        eapp.current = (size_t)want_cur;
        ed_ui_tab_update();
    }

    ta_status(&eapp.base, "%s", ta_("Ready"));
    ta_run(&eapp.base);

    ed_session_save();
    ed_docs_shutdown();
    ta_close(&eapp.base);
    menus_shutdown();
    return 0;
}
