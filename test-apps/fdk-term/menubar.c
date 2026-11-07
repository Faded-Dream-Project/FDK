/* menubar.c — the menu bar + keyboard accelerators + the toolbar. */
#include "term_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The per-window focus tracking (view.c's bell marks unread from it). */
void tapp_note_focus(twindow *w, bool focused);

/* ---- actions --------------------------------------------------------------- */

typedef struct {
    twindow *w;
} mact;

static void act_new_tab(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_new_tab(a->w, NULL);
}

static void act_close_tab(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_close_tab(a->w,
                   (int)fdk_notebook_get_current_page(a->w->notebook));
}

static void act_detach(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_detach_tab(a->w,
                    (int)fdk_notebook_get_current_page(a->w->notebook));
}

static void act_split_h(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    size_t cur = fdk_notebook_get_current_page(a->w->notebook);
    if (cur < (size_t)a->w->ntabs) {
        tapp_split(a->w, a->w->tabs[cur], true);
    }
}

static void act_split_v(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    size_t cur = fdk_notebook_get_current_page(a->w->notebook);
    if (cur < (size_t)a->w->ntabs) {
        tapp_split(a->w, a->w->tabs[cur], false);
    }
}

static void act_new_window(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    twindow *nw = tapp_open_window(a->w->app, 100, 30);
    if (nw != NULL) {
        tapp_new_tab(nw, NULL);
    }
}

static void act_close_window(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_close_window(a->w);
}

static void act_quit(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_quit(a->w->app);
}

static void act_copy(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tview *v = tapp_focused_view(a->w);
    if (v != NULL) {
        char *txt = term_selection_text(&v->t);
        if (txt != NULL) {
            (void)fdk_clipboard_set_text(a->w->app->ctx, txt);
            free(txt);
        }
    }
}

static void act_paste(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tview *v = tapp_focused_view(a->w);
    if (v != NULL) {
        char *txt = fdk_clipboard_get_text(a->w->app->ctx);
        if (txt != NULL) {
            vt_paste(v->parser, txt, strlen(txt));
            free(txt);
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
        }
    }
}

static void act_select_all(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tview *v = tapp_focused_view(a->w);
    if (v != NULL) {
        v->t.sel_active = true;
        v->t.sel_x0 = 0;
        v->t.sel_y0 = 0;
        v->t.sel_x1 = v->t.cols - 1;
        v->t.sel_y1 = tview_abs_y(v, v->t.rows - 1);
        tview_invalidate(v);
    }
}

static void act_search(fdk_menu_item *it, void *user) {
    (void)it;
    tsearch_toggle(user);
}

static void act_prefs(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_open_prefs(a->w->app);
}

static void act_zoom_in(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_zoom(a->w->app, +1);
}

static void act_zoom_out(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_zoom(a->w->app, -1);
}

static void act_reset_zoom(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tapp_zoom(a->w->app, 0);
}

static void act_clear(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    tview *v = tapp_focused_view(a->w);
    if (v != NULL) {
        vt_feed(v->parser, "\033[H\033[2J", 7, NULL);
        tview_invalidate(v);
    }
}

static void act_about(fdk_menu_item *it, void *user) {
    (void)it;
    mact *a = user;
    fdk_about_dialog_options ao = {0};
    ao.program_name = "fdk-term";
    ao.version = "1.0";
    ao.comments =
        "The FDK terminal emulator: a real xterm-compatible terminal "
        "built purely on the FDK toolkit — PTY job control, full "
        "CSI/OSC escape support, 256-color and truecolor, wide "
        "characters and emoji, scrollback with search, tabs, splits, "
        "and profiles.";
    ao.modal = true;
    ao.parent = a->w->window;
    fdk_window *about_w = NULL;
    (void)fdk_dialog_show_about(a->w->app->ctx, &ao, NULL, NULL, NULL,
                                &about_w);
}

/* ---- the bar ---------------------------------------------------------------- */

void tapp_build_menubar(twindow *w, fdk_widget *host) {
    tapp *app = w->app;
    mact *a = calloc(1, sizeof *a);
    if (a == NULL) {
        return;
    }
    a->w = w;
    w->menu_ctx = a;

    fdk_widget *bar = NULL;
    fdk_menu_bar_create(host, app->font_ui, &bar);

    /* File */
    fdk_menu *mfile = NULL;
    (void)fdk_menu_create(app->font_ui, &mfile);
    fdk_menu_item *it;
    (void)fdk_menu_append(mfile, "&New Tab", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+T");
    fdk_menu_item_set_on_activate(it, act_new_tab, a);
    (void)fdk_menu_append(mfile, "Close &Tab", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+W");
    fdk_menu_item_set_on_activate(it, act_close_tab, a);
    (void)fdk_menu_append(mfile, "&Detach Tab to New Window", &it);
    fdk_menu_item_set_on_activate(it, act_detach, a);
    (void)fdk_menu_append_separator(mfile);
    (void)fdk_menu_append(mfile, "&Split Horizontal", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+O");
    fdk_menu_item_set_on_activate(it, act_split_h, a);
    (void)fdk_menu_append(mfile, "S&plit Vertical", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+E");
    fdk_menu_item_set_on_activate(it, act_split_v, a);
    (void)fdk_menu_append_separator(mfile);
    (void)fdk_menu_append(mfile, "New &Window", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+N");
    fdk_menu_item_set_on_activate(it, act_new_window, a);
    (void)fdk_menu_append(mfile, "&Close Window", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+Q");
    fdk_menu_item_set_on_activate(it, act_close_window, a);
    (void)fdk_menu_append(mfile, "&Quit", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Q");
    fdk_menu_item_set_on_activate(it, act_quit, a);
    (void)fdk_menu_bar_append(bar, "&File", mfile);
    w->menus[w->nmenus++] = mfile;

    /* Edit */
    fdk_menu *medit = NULL;
    (void)fdk_menu_create(app->font_ui, &medit);
    (void)fdk_menu_append(medit, "&Copy", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+C");
    fdk_menu_item_set_on_activate(it, act_copy, a);
    (void)fdk_menu_append(medit, "&Paste", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+V");
    fdk_menu_item_set_on_activate(it, act_paste, a);
    (void)fdk_menu_append(medit, "Select &All", &it);
    fdk_menu_item_set_on_activate(it, act_select_all, a);
    (void)fdk_menu_append_separator(medit);
    (void)fdk_menu_append(medit, "&Find in Scrollback...", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+F");
    fdk_menu_item_set_on_activate(it, act_search, w);
    (void)fdk_menu_append_separator(medit);
    (void)fdk_menu_append(medit, "Pr&eferences...", &it);
    fdk_menu_item_set_on_activate(it, act_prefs, a);
    (void)fdk_menu_bar_append(bar, "&Edit", medit);
    w->menus[w->nmenus++] = medit;

    /* View */
    fdk_menu *mview = NULL;
    (void)fdk_menu_create(app->font_ui, &mview);
    (void)fdk_menu_append(mview, "Zoom &In", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+plus");
    fdk_menu_item_set_on_activate(it, act_zoom_in, a);
    (void)fdk_menu_append(mview, "Zoom &Out", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+minus");
    fdk_menu_item_set_on_activate(it, act_zoom_out, a);
    (void)fdk_menu_append(mview, "&Reset Zoom", &it);
    fdk_menu_item_set_on_activate(it, act_reset_zoom, a);
    (void)fdk_menu_append_separator(mview);
    (void)fdk_menu_append(mview, "&Clear Scrollback + Screen", &it);
    fdk_menu_item_set_on_activate(it, act_clear, a);
    (void)fdk_menu_bar_append(bar, "&View", mview);
    w->menus[w->nmenus++] = mview;

    /* Help */
    fdk_menu *mhelp = NULL;
    (void)fdk_menu_create(app->font_ui, &mhelp);
    (void)fdk_menu_append(mhelp, "&About fdk-term", &it);
    fdk_menu_item_set_on_activate(it, act_about, a);
    (void)fdk_menu_bar_append(bar, "&Help", mhelp);
    w->menus[w->nmenus++] = mhelp;
}

/* The focused view of a window: the notebook's current tab's LAST
 * split member that had focus (tracked via the canvas focus events;
 * the simple contract: the first view of the current tab). */
tview *tapp_focused_view(twindow *w) {
    if (w == NULL || w->ntabs == 0) {
        return NULL;
    }
    size_t cur = fdk_notebook_get_current_page(w->notebook);
    if (cur >= (size_t)w->ntabs) {
        cur = 0;
    }
    ttab *tab = w->tabs[cur];
    if (tab->nviews == 0) {
        return NULL;
    }
    return tab->views[tab->nviews - 1];
}
