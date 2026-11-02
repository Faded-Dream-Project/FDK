/* ui.c — fdk-edit's furniture: the icon toolbar, the menus, the
 * status bar, the find bar, the shortcuts, drag-file-to-open. */
#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the toolbar actions ----------------------------------------- */

static void act_new(fdk_widget *b, void *u) { (void)b; (void)u; ed_doc_new(); }
static void act_open(fdk_widget *b, void *u);
static void act_save(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) {
        (void)ed_doc_save(d, false);
    }
}
static void act_save_as(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) {
        (void)ed_doc_save(d, true);
    }
}
static void act_undo(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) {
        (void)fdk_textview_undo(d->view);
        on_doc_changed_external(d);
    }
}
static void act_redo(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_doc *d = ed_doc_current();
    if (d != NULL) {
        (void)fdk_textview_redo(d->view);
        on_doc_changed_external(d);
    }
}
static void act_find(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_ui_findbar_toggle(false);
}
static void act_replace(fdk_widget *b, void *u) {
    (void)b; (void)u;
    ed_ui_findbar_toggle(true);
}
static void act_wrap(fdk_widget *b, void *u);
static void act_prefs(fdk_widget *b, void *u);

/* doc.c's changed path, callable for undo/redo resync */
void on_doc_changed_external(ed_doc *d) {
    const char *t = fdk_textview_get_text(d->view);
    if (t != d->text) {
        free(d->text);
        d->text = strdup(t != NULL ? t : "");
        d->text_len = strlen(d->text);
    }
    ed_highlight(d);
    ed_doc_mark_modified(d);
}

/* ---- open: the async picker --------------------------------------- */

static void on_open_picked(const fdk_file_dialog_result *r,
                           void *user) {
    (void)user;
    if (r == NULL || r->outcome != FDK_FILE_DIALOG_ACCEPTED) {
        return;
    }
    for (size_t i = 0; i < r->count; i++) {
        ed_doc_open(r->paths[i]);
    }
}

static void act_open(fdk_widget *b, void *u) {
    (void)b; (void)u;
    fdk_file_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("Open");
    o.kind = FDK_FILE_DIALOG_OPEN_FILE;
    o.parent = eapp.base.window;
    if (!fdk_ok(fdk_dialog_open_file(eapp.base.ctx, &o,
                                     on_open_picked, NULL, NULL))) {
        ta_status(&eapp.base, "%s", ta_("Cannot open the file dialog"));
    }
}

/* the shared open-picker entry (toolbar + menu) */
void ed_open_dialog(void) {
    fdk_file_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("Open");
    o.kind = FDK_FILE_DIALOG_OPEN_FILE;
    o.parent = eapp.base.window;
    if (!fdk_ok(fdk_dialog_open_file(eapp.base.ctx, &o,
                                     on_open_picked, NULL, NULL))) {
        ta_status(&eapp.base, "%s", ta_("Cannot open the file dialog"));
    }
}

/* ---- the wrap toggle (a toggle button) --------------------------- */

static bool g_wrap_on;
static void act_wrap(fdk_widget *b, void *u) {
    (void)b; (void)u;
    eapp.wrap = !eapp.wrap;
    g_wrap_on = eapp.wrap;
    for (size_t i = 0; i < eapp.doc_count; i++) {
        fdk_textview_set_wrap(
            eapp.docs[i].view,
            eapp.wrap ? FDK_TEXTVIEW_WRAP_WORD
                      : FDK_TEXTVIEW_WRAP_NONE);
    }
    ta_status(&eapp.base, "%s", eapp.wrap ? ta_("Word wrap on")
                                          : ta_("Word wrap off"));
}

/* ---- preferences (the wrap/highlight/gutter toggles) -------------- */


static void act_prefs(fdk_widget *b, void *u) {
    (void)b; (void)u;
    /* three stock check rows in one message-style dialog: the honest
     * scope for PASS A (a real prefs dialog is PASS B) — a small
     * toolkit-owned window with Check buttons */
    extern void ed_prefs_dialog(void);
    ed_prefs_dialog();
}

/* ---- the notebook switch ------------------------------------------ */

static void on_switch(fdk_widget *nb, size_t page, void *user) {
    (void)nb; (void)user;
    if (page < eapp.doc_count) {
        eapp.current = page;
        ed_ui_status();
    }
}

/* ---- drag-file-to-open -------------------------------------------- */

static void on_drop(fdk_widget *w, fdk_event_type type,
                    const fdk_drag_event *ev, void *user) {
    (void)w; (void)user;
    if (type == FDK_EVENT_DRAG_DROP && ev->uris != NULL) {
        for (size_t i = 0; i < ev->uri_count; i++) {
            const char *u = ev->uris[i];
            if (strncmp(u, "file://", 7) == 0) {
                ed_doc_open(u + 7);
            }
        }
    }
}

/* ---- the find bar --------------------------------------------------
 * An Entry row (find), a conditional Entry (replace), toggle regex,
 * prev/next buttons, replace-one/replace-all. */

static void findbar_run(int dir, bool replace_all);

static void on_find_activate(fdk_widget *e, void *u) {
    (void)e; (void)u;
    findbar_run(1, false);
}

static void on_replace_all(fdk_widget *b, void *u) {
    (void)b; (void)u;
    findbar_run(1, true);
}

static void on_regex_toggle2(fdk_widget *b, bool checked, void *u) {
    (void)b; (void)u;
    eapp.regex_mode = checked;
}

void ed_ui_findbar_toggle(bool with_replace) {
    eapp.replace_visible = with_replace;
    fdk_widget_set_visible(eapp.replace_entry, with_replace);
    fdk_widget_set_visible(eapp.findbar, true);
    (void)fdk_widget_focus(eapp.find_entry);
}

static void findbar_run(int dir, bool replace_all) {
    ed_doc *d = ed_doc_current();
    if (d == NULL) {
        return;
    }
    char needle[256];
    ta_copy(needle, sizeof needle,
            fdk_entry_get_text(eapp.find_entry));
    if (needle[0] == '\0') {
        return;
    }
    if (replace_all) {
        char repl[256];
        ta_copy(repl, sizeof repl,
                fdk_entry_get_text(eapp.replace_entry));
        size_t n = 0;
        if (ed_replace_all(d, needle, repl, eapp.regex_mode,
                           true, &n)) {
            ta_status(&eapp.base, ta_("%zu replaced"), n);
        }
        return;
    }
    size_t caret = 0;
    (void)fdk_textview_get_caret(d->view, &caret);
    size_t ml = 0;
    const char *m = ed_find_next(d->text, needle, eapp.regex_mode,
                                 true, caret, &ml);
    if (m == NULL) {
        /* wrap around once */
        m = ed_find_next(d->text, needle, eapp.regex_mode, true, 0,
                         &ml);
    }
    if (m != NULL) {
        size_t at = (size_t)(m - d->text);
        (void)fdk_textview_set_caret(d->view, at);
        /* select the match (set selection through select_all +
         * caret? the public selection API is select_all only — the
         * honest PASS A scope: caret to the match, the status says
         * where) */
        ta_status(&eapp.base, ta_("match at byte %zu"), at);
    } else {
        ta_status(&eapp.base, "%s", ta_("No matches"));
    }
    (void)dir;
}

/* ---- the status bar ------------------------------------------------ */

void ed_ui_status(void) {
    ed_doc *d = ed_doc_current();
    if (d == NULL) {
        ta_status(&eapp.base, "%s", ta_("No documents"));
        return;
    }
    size_t line = 0, col = 0;
    (void)fdk_textview_caret_line_col(d->view, &line, &col);
    ta_status(&eapp.base,
              ta_("Ln %zu, Col %zu   %s   %s   %s"),
              line + 1, col + 1, d->encoding,
              d->crlf ? "CRLF" : "LF", ed_lang_name(d->lang));
}

void ed_ui_tab_update(void) {
    if (eapp.notebook != NULL &&
        eapp.current < fdk_notebook_page_count(eapp.notebook)) {
        (void)fdk_notebook_set_current_page(eapp.notebook,
                                            eapp.current);
    }
    ed_ui_status();
}

/* ---- the build ------------------------------------------------------ */

/* the shortcut shims (declared before the build that binds them) */
static void sh_new(fdk_window *w, void *u);
static void sh_open(fdk_window *w, void *u);
static void sh_save(fdk_window *w, void *u);
static void sh_find(fdk_window *w, void *u);
static void sh_replace(fdk_window *w, void *u);
void quit_from_shortcut(fdk_window *w, void *user);

void ed_ui_build(void) {
    ta_app *a = &eapp.base;

    /* the toolbar */
    (void)fdk_toolbar_create(a->content, a->font_ui, &eapp.toolbar);
    const ta_icon_button specs[] = {
        {"document-new", FDK_ROW_ICON_FILE, "New", "Ctrl+N", false,
         act_new, NULL},
        {"document-open", FDK_ROW_ICON_FOLDER, "Open", "Ctrl+O", false,
         act_open, NULL},
        {"document-save", FDK_ROW_ICON_FILE, "Save", "Ctrl+S", false,
         act_save, NULL},
        {NULL, FDK_ROW_ICON_FILE, "Save As", "Ctrl+Shift+S", false,
         act_save_as, NULL},
        {NULL, FDK_ROW_ICON_FILE, "Undo", "Ctrl+Z", false, act_undo,
         NULL},
        {NULL, FDK_ROW_ICON_FILE, "Redo", "Ctrl+Shift+Z", false,
         act_redo, NULL},
        {NULL, FDK_ROW_ICON_FILE, "Find", "Ctrl+F", false, act_find,
         NULL},
        {NULL, FDK_ROW_ICON_FILE, "Replace", "Ctrl+H", false,
         act_replace, NULL},
        {NULL, FDK_ROW_ICON_FILE, "Word wrap", NULL, true, act_wrap,
         NULL},
        {NULL, FDK_ROW_ICON_FILE, "Preferences", "Ctrl+,", false,
         act_prefs, NULL},
    };
    for (size_t i = 0; i < sizeof specs / sizeof *specs; i++) {
        (void)ta_icon_button_add(eapp.toolbar, a, &specs[i]);
    }

    /* the find bar (hidden until Ctrl+F) */
    (void)fdk_widget_create(a->content, NULL,
                            (fdk_rect){0, 0, 100, 36}, &eapp.findbar);
    fdk_widget *fe = NULL;
    (void)fdk_entry_create(eapp.findbar, a->font_ui, "", &fe);
    eapp.find_entry = fe;
    fdk_widget_set_bounds(fe, (fdk_rect){8, 4, 260, 28});
    fdk_entry_set_placeholder(fe, ta_("Find"));
    fdk_entry_set_on_activate(fe, on_find_activate, NULL);
    (void)fdk_widget_create(eapp.findbar, NULL,
                            (fdk_rect){0, 0, 100, 36},
                            &eapp.replace_entry);
    fdk_widget *re = NULL;
    (void)fdk_entry_create(eapp.findbar, a->font_ui, "", &re);
    fdk_widget_set_bounds(re, (fdk_rect){276, 4, 260, 28});
    fdk_entry_set_placeholder(re, ta_("Replace with"));
    fdk_widget_set_visible(eapp.replace_entry, false);
    /* the regex check + the buttons */
    fdk_widget *rx = NULL;
    (void)fdk_toggle_create(eapp.findbar, a->font_ui,
                            ta_("Regular expression"), &rx);
    fdk_widget_set_bounds(rx, (fdk_rect){544, 6, 170, 24});
    fdk_toggle_set_on_changed(rx, on_regex_toggle2, NULL);
    fdk_widget *ra = NULL;
    (void)fdk_button_create(eapp.findbar, a->font_ui,
                            ta_("Replace all"), &ra);
    fdk_widget_set_bounds(ra, (fdk_rect){712, 4, 120, 28});
    fdk_button_set_on_activate(ra, on_replace_all, NULL);
    fdk_widget_set_visible(eapp.findbar, false);

    /* the notebook */
    (void)fdk_notebook_create(a->content, a->font_ui,
                              &eapp.notebook);
    fdk_notebook_set_on_switch(eapp.notebook, on_switch, NULL);

    /* drag-file-to-open: the whole content is the target */
    (void)fdk_widget_set_drop_target(a->content,
                                     FDK_DRAG_FORMAT_URI_LIST);
    (void)fdk_widget_set_drag_callback(a->content, on_drop, NULL);

    /* the shortcuts (the toolbar actions' keyboard twins; the
     * signature is the window shortcut fn) */
    (void)fdk_window_add_shortcut(a->window, "Ctrl+N",
                                  sh_new, NULL);
    (void)fdk_window_add_shortcut(a->window, "Ctrl+O", sh_open,
                                  NULL);
    (void)fdk_window_add_shortcut(a->window, "Ctrl+S", sh_save,
                                  NULL);
    (void)fdk_window_add_shortcut(a->window, "Ctrl+F", sh_find,
                                  NULL);
    (void)fdk_window_add_shortcut(a->window, "Ctrl+H",
                                  sh_replace, NULL);
    (void)fdk_window_add_shortcut(a->window, "Ctrl+Q",
                                  quit_from_shortcut, NULL);
}

/* the shortcut shims' bodies */
static void sh_new(fdk_window *w, void *u) { (void)w; (void)u; ed_doc_new(); }
static void sh_open(fdk_window *w, void *u) { (void)w; (void)u; act_open(NULL, NULL); }
static void sh_save(fdk_window *w, void *u) { (void)w; (void)u; act_save(NULL, NULL); }
static void sh_find(fdk_window *w, void *u) { (void)w; (void)u; act_find(NULL, NULL); }
static void sh_replace(fdk_window *w, void *u) { (void)w; (void)u; act_replace(NULL, NULL); }

/* the quit shortcut (bound in ed_ui_build) */
void quit_from_shortcut(fdk_window *w, void *user) {
    (void)w; (void)user;
    eapp.base.quit = true;
}
