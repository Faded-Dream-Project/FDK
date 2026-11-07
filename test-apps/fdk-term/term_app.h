/* app.h — fdk-term's application shell: the multi-window, multi-tab
 * app state around the views.
 *
 * Structure: tapp (the process) -> twindow (an OS window) -> tabs
 * (a notebook page per terminal; the page is a vertical box holding
 * OPTIONAL splits: one pane box, itself a Paned of views or a single
 * view). Splits nest via Paned widgets (vsplit adds a horizontal
 * Paned under the pane; hsplit a vertical one — terminology follows
 * the split LINE direction, the Konsole convention).
 *
 * Detach moves a tab's whole page into a NEW twindow. Session
 * restore: window count/sizes, per-window tab count, per-tab cwd +
 * (optionally) the command line — restored on start. */
#ifndef FDKTERM_APP_H
#define FDKTERM_APP_H

#include "profile.h"
#include "view.h"

#include "fdk/fdk.h"

typedef struct ttab {
    struct twindow *win;
    fdk_widget *page;       /* the notebook page (a box)           */
    fdk_widget *panes;      /* the split tree root (or the view's
                             * canvas directly)                    */
    tview *views[8];        /* the split members (1..8)            */
    int nviews;
    char title[128];        /* the live title (OSC 0/2)            */
    int id;
} ttab;

typedef struct twindow {
    struct tapp *app;
    fdk_window *window;
    fdk_widget *root, *content;
    fdk_widget *notebook;
    fdk_widget *statusbar;
    ttab *tabs[16];
    int ntabs;
    /* the menu MODELS listed by this window's bar (fdk_menu_bar_append
     * does not take ownership — the app destroys them, per the
     * fdk_widgets.h contract, after the bar is gone) */
    fdk_menu *menus[8];
    int nmenus;
    int id;
    /* the pending-resize grid math */
    int cols, rows;
    bool closing;
    bool focused;   /* the FOCUS event's record (bell unread marks) */
} twindow;

typedef struct tapp {
    int last_font_px;
    fdk_context *ctx;
    fdk_prefs *prefs;
    fdk_font *font;          /* the profile mono font (owned)       */
    fdk_font *font_ui;       /* the UI font (owned)                 */
    fdk_notify *notify;
    tprofile active;          /* the live profile                    */
    twindow *windows[8];
    int nwindows;
    int next_id;
    bool quit;
    long frames, frame_limit;
    char app_id[64];
    /* the prefs UI's window (one at a time) */
    struct tprefs *prefs_ui;
    /* the search bar owner (the window whose bar is open) */
    twindow *search_win;
} tapp;

/* main.c's pieces used across files: */
void tapp_new_tab(twindow *w, const char *cwd);
void tapp_detach_tab(twindow *w, int tab_index);
void tapp_close_tab(twindow *w, int tab_index);
void tapp_split(twindow *w, ttab *tab, bool vertical_line);
void tapp_close_window(twindow *w);
twindow *tapp_open_window(tapp *app, int cols, int rows);
void tapp_open_prefs(tapp *app);
void tapp_update_status(twindow *w);
void tapp_quit(tapp *app);
void tapp_zoom(tapp *app, int dir);      /* +1 in, -1 out, 0 reset */
void tapp_reload_font(tapp *app);        /* after a font_px change */
void tapp_build_menubar(twindow *w, fdk_widget *host);
tview *tapp_focused_view(twindow *w);

/* view.c's app-facing helpers: */
void tview_set_tab_title(tview *v, const char *title);
void tview_bell(tview *v);
void tview_child_died(tview *v);
void tview_open_context_menu(tview *v, int x, int y);
fdk_window *tview_owner_window(tview *v);

/* prefs_ui.c (the preferences window): */
void tprefs_run(tapp *app);

/* search.c (the find bar): */
void tsearch_toggle(twindow *w);
void tsearch_next(twindow *w);
void tsearch_close(twindow *w);

#endif /* FDKTERM_APP_H */
