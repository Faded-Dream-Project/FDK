/* main.c — fdk-term: the application shell (windows, tabs, splits,
 * menu bar, shortcuts, session restore, the pump). */
#include "term_app.h"

#include "fdk/fdk_extend.h"
#include "fdk/fdk_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void twindow_focus_active(twindow *w);

void tview_regrid(tview *v);

/* tapp's prefs int read (fdk_prefs raw; no ta_app cast games). */
static int tapp_pref_int(tapp *app, const char *key, int def) {
    if (app->prefs == NULL) {
        return def;
    }
    const char *s = fdk_prefs_get(app->prefs, key, NULL);
    return (s != NULL && s[0] != '\0') ? atoi(s) : def;
}

/* ---- window geometry helpers ------------------------------------------ */

static void window_grid_math(twindow *w) {
    tapp *app = w->app;
    tview *v = w->tabs[0] != NULL && w->tabs[0]->nviews > 0
                   ? w->tabs[0]->views[0]
                   : NULL;
    if (v == NULL) {
        return;
    }
    /* the notebook + menubar + statusbar chrome eats ~64px */
    fdk_size ws = {0, 0};
    fdk_window_get_size(w->window, &ws);
    int avail_h = ws.height - 64;
    int avail_w = ws.width;
    int cols = avail_w / (v->cell_w > 0 ? v->cell_w : 8);
    int rows = avail_h / (v->cell_h > 0 ? v->cell_h : 17);
    if (cols < 2) {
        cols = 2;
    }
    if (rows < 2) {
        rows = 2;
    }
    w->cols = cols;
    w->rows = rows;
    (void)app;
}

/* ---- tabs ---------------------------------------------------------------- */

void tapp_new_tab(twindow *w, const char *cwd) {
    if (w->ntabs >= 16) {
        return;
    }
    tapp *app = w->app;
    ttab *tab = calloc(1, sizeof *tab);
    if (tab == NULL) {
        return;
    }
    tab->win = w;
    tab->id = app->next_id++;
    snprintf(tab->title, sizeof tab->title, "shell");

    fdk_widget *page = NULL;
    fdk_box_create(w->notebook, FDK_VERTICAL, &page); /* adopted */
    fdk_box_set_padding(page, 0);
    fdk_box_set_spacing(page, 0);
    tab->page = page;

    tview *v = calloc(1, sizeof *v);
    if (v == NULL || !tview_create(v, app, w, page, cwd)) {
        free(v);
        fdk_widget_destroy(page);
        free(tab);
        return;
    }
    fdk_widget_set_expand(v->canvas, true, true);
    tab->views[tab->nviews++] = v;

    fdk_notebook_append_page(w->notebook, page, tab->title);
    w->tabs[w->ntabs++] = tab;
    fdk_notebook_set_current_page(w->notebook,
                                  fdk_notebook_page_count(w->notebook) - 1);
    tview_focus(v);
    tapp_update_status(w);
}

void tapp_close_tab(twindow *w, int tab_index) {
    if (tab_index < 0 || tab_index >= w->ntabs) {
        return;
    }
    ttab *tab = w->tabs[tab_index];
    /* the views + canvases die with the page (parent-owns-children);
     * the model halves are ours */
    for (int i = 0; i < tab->nviews; i++) {
        tview_destroy(tab->views[i]);
        free(tab->views[i]);
    }
    size_t page_idx = fdk_notebook_page_index(w->notebook, tab->page);
    if (page_idx < fdk_notebook_page_count(w->notebook)) {
        (void)fdk_notebook_remove_page(w->notebook, page_idx);
    }
    for (int i = tab_index; i < w->ntabs - 1; i++) {
        w->tabs[i] = w->tabs[i + 1];
    }
    w->ntabs--;
    free(tab);
    if (w->ntabs == 0) {
        tapp_close_window(w);
    } else {
        tapp_update_status(w);
    }
}

void tapp_detach_tab(twindow *w, int tab_index) {
    if (tab_index < 0 || tab_index >= w->ntabs || w->ntabs < 2) {
        return;
    }
    tapp *app = w->app;
    ttab *tab = w->tabs[tab_index];

    /* a fresh window sized to this one */
    twindow *nw = tapp_open_window(app, w->cols, w->rows);
    if (nw == NULL) {
        return;
    }
    /* move the tab struct: steal the views, leave the page widget
     * (the notebook destroys pages; we rebuild in the new window) */
    ttab *nt = calloc(1, sizeof *nt);
    if (nt == NULL) {
        return;
    }
    *nt = *tab;
    nt->win = nw;
    /* re-parent the views' models into the new window's context:
     * the views' canvases were owned by the old page; build a fresh
     * page in the new window and move the VIEWS by re-creating their
     * canvas only if needed. Simplest honest path: the page widget
     * can be re-parented directly. */
    fdk_result r = fdk_widget_reparent(nt->page, nw->notebook);
    if (!fdk_ok(r)) {
        /* fall back: destroy + rebuild the tab fresh */
        free(nt);
        tapp_new_tab(nw, tab->views[0] != NULL ? tab->views[0]->pt.cwd
                                               : NULL);
        tapp_close_tab(w, tab_index);
        return;
    }
    fdk_notebook_append_page(nw->notebook, nt->page, nt->title);
    nw->tabs[nw->ntabs++] = nt;
    /* fix the views' owner pointers */
    for (int i = 0; i < nt->nviews; i++) {
        nt->views[i]->owner = nw;
    }
    /* drop it from the old window */
    size_t page_idx = fdk_notebook_page_index(w->notebook, nt->page);
    if (page_idx == fdk_notebook_page_count(w->notebook)) {
        /* already re-parented away; just fix the arrays */
    }
    for (int i = tab_index; i < w->ntabs - 1; i++) {
        w->tabs[i] = w->tabs[i + 1];
    }
    w->ntabs--;
    free(tab);
    fdk_notebook_set_current_page(nw->notebook, 0);
    tapp_update_status(nw);
    tapp_update_status(w);
}

/* Splits: the tab's page hosts a Paned tree. Splitting replaces
 * the pane layout with a new Paned containing the old root + a
 * fresh view. vertical_line=true draws the divider vertically
 * (side-by-side panes); false stacks them. */
void tapp_split(twindow *w, ttab *tab, bool vertical_line) {
    if (tab->nviews >= 8) {
        return;
    }
    tapp *app = w->app;
    fdk_widget *old_root = tab->panes != NULL ? tab->panes
                            : (tab->nviews > 0 ? tab->views[0]->canvas
                                               : NULL);
    if (old_root == NULL) {
        return;
    }

    fdk_widget *paned = NULL;
    fdk_paned_create(tab->page,
                     vertical_line ? FDK_HORIZONTAL : FDK_VERTICAL,
                     &paned);
    fdk_widget_set_expand(paned, true, true);

    /* the new view is created FIRST (its parent must exist but the
     * canvas can be re-parented after) — simplest: create against
     * the page, then reparent both into the paned. */
    tview *v = calloc(1, sizeof *v);
    if (v == NULL) {
        return;
    }
    /* create the view with its canvas in the page temporarily */
    if (!tview_create(v, app, w, tab->page, NULL)) {
        free(v);
        return;
    }
    fdk_widget_set_expand(v->canvas, true, true);

    /* re-parent old root and new canvas into the paned (children of
     * a Paned = pane1, pane2). */
    fdk_result r1 = fdk_widget_reparent(old_root, paned);
    fdk_result r2 = fdk_widget_reparent(v->canvas, paned);
    if (!fdk_ok(r1) || !fdk_ok(r2)) {
        /* degrade honestly: keep the old layout, drop the new view */
        tview_destroy(v);
        free(v);
        fdk_widget_destroy(paned);
        return;
    }
    fdk_paned_unset_position(paned);
    fdk_widget_set_expand(old_root, true, true);
    tab->panes = paned;
    tab->views[tab->nviews++] = v;
    tview_focus(v);
    tapp_update_status(w);
}

/* ---- windows --------------------------------------------------------------- */

static void window_event(fdk_window *win, const fdk_event_data *ev,
                         void *user) {
    twindow *w = user;
    tapp *app = w->app;
    switch (ev->type) {
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        tapp_close_window(w);
        break;
    case FDK_EVENT_WINDOW_CONFIGURE: {
        window_grid_math(w);
        for (int t = 0; t < w->ntabs; t++) {
            for (int k = 0; k < w->tabs[t]->nviews; k++) {
                tview_regrid(w->tabs[t]->views[k]);
            }
        }
        break;
    }
    case FDK_EVENT_WINDOW_FOCUS: {
        w->focused = ev->focus.focused != 0;
        if (ev->focus.focused) {
            /* the WM can focus the window with no click: make sure
             * the terminal canvas actually holds widget focus */
            twindow_focus_active(w);
            /* clear the unread marks */
            for (int t = 0; t < w->ntabs; t++) {
                for (int k = 0; k < w->tabs[t]->nviews; k++) {
                    w->tabs[t]->views[k]->has_unread = false;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    (void)win;
    (void)app;
}

/* Put keyboard focus on the active terminal view. fdk_widget_focus
 * rejects widgets in a not-yet-visible window, so the focus request
 * made while building tabs (tview_focus in tapp_new_tab) dies
 * silently — the window was still unshown. Without this, NOTHING
 * ever holds keyboard focus (a WM focuses the window on map without
 * any click) and every keystroke is routed to no widget: the
 * "can't type anything" report. Runs after fdk_window_show and on
 * every WINDOW_FOCUS gain. */
static void twindow_focus_active(twindow *w) {
    size_t cur = fdk_notebook_get_current_page(w->notebook);
    if (cur < (size_t)w->ntabs && w->tabs[cur]->nviews > 0) {
        tview_focus(w->tabs[cur]->views[0]);
    }
}

twindow *tapp_open_window(tapp *app, int cols, int rows) {
    if (app->nwindows >= 8) {
        return NULL;
    }
    twindow *w = calloc(1, sizeof *w);
    if (w == NULL) {
        return NULL;
    }
    w->app = app;
    w->id = app->next_id++;

    fdk_window_options wopts = {0};
    wopts.title = "fdk-term";
    wopts.width = (cols > 0 ? cols : 100) * 8 + 24;
    wopts.height = (rows > 0 ? rows : 30) * 17 + 64;
    if (!fdk_ok(fdk_window_create(app->ctx, &wopts, &w->window))) {
        free(w);
        return NULL;
    }
    (void)fdk_window_set_decorated(w->window, true);
    fdk_window_set_event_callback(w->window, window_event, w);
    fdk_window_set_role(w->window, "term");

    if (!fdk_ok(fdk_window_get_root(w->window, &w->root))) {
        fdk_window_destroy(w->window);
        free(w);
        return NULL;
    }
    fdk_widget *frame = NULL;
    fdk_box_create(w->root, FDK_VERTICAL, &frame);
    fdk_box_set_padding(frame, 0);
    fdk_box_set_spacing(frame, 0);
    fdk_window_set_content(w->window, frame);

    /* menubar host + notebook + statusbar */
    fdk_widget *menubar_host = NULL;
    fdk_box_create(frame, FDK_VERTICAL, &menubar_host);
    fdk_box_set_padding(menubar_host, 0);

    fdk_notebook_create(frame, app->font_ui, &w->notebook);
    fdk_widget_set_expand(w->notebook, true, true);

    fdk_statusbar_create(frame, app->font_ui, &w->statusbar);

    w->cols = cols > 0 ? cols : 100;
    w->rows = rows > 0 ? rows : 30;

    /* the menu bar */
    tapp_build_menubar(w, menubar_host);

    fdk_window_show(w->window);
    (void)fdk_window_paint(w->window);
    twindow_focus_active(w); /* the pre-show focus request died */

    app->windows[app->nwindows++] = w;
    return w;
}

void tapp_close_window(twindow *w) {
    tapp *app = w->app;
    if (w->closing) {
        return;
    }
    w->closing = true;
    /* views first (their fd watches + PTYs), then the tree */
    for (int t = 0; t < w->ntabs; t++) {
        for (int k = 0; k < w->tabs[t]->nviews; k++) {
            tview_destroy(w->tabs[t]->views[k]);
            free(w->tabs[t]->views[k]);
        }
        free(w->tabs[t]);
    }
    int idx = -1;
    for (int i = 0; i < app->nwindows; i++) {
        if (app->windows[i] == w) {
            idx = i;
        }
    }
    if (idx >= 0) {
        for (int i = idx; i < app->nwindows - 1; i++) {
            app->windows[i] = app->windows[i + 1];
        }
        app->nwindows--;
    }
    if (app->search_win == w) {
        tsearch_close(w);
    }
    fdk_window_destroy(w->window); /* frees the tree */
    /* the bar is gone (bar_destroy unattached the models): now the
     * app-side menu models go — the fdk_widgets.h ownership contract
     * (ASan counted every one of these as leaked at exit) */
    for (int i = 0; i < w->nmenus; i++) {
        fdk_menu_destroy(w->menus[i]);
    }
    free(w->menu_ctx);
    free(w);
    if (app->nwindows == 0) {
        app->quit = true;
    }
}

void tapp_update_status(twindow *w) {
    if (w == NULL || w->statusbar == NULL || w->ntabs == 0) {
        return;
    }
    fdk_u32 ctx = fdk_statusbar_get_context_id(w->statusbar, "main");
    ttab *tab = w->tabs[fdk_notebook_get_current_page(w->notebook) <
                                (size_t)w->ntabs
                            ? (int)fdk_notebook_get_current_page(
                                  w->notebook)
                            : 0];
    if (tab == NULL || tab->nviews == 0) {
        return;
    }
    tview *v = tab->views[0];
    char msg[256];
    const char *cwd = v->pt.cwd;
    snprintf(msg, sizeof msg, "%d x %d   %s   %s%s", v->t.cols, v->t.rows,
             cwd != NULL ? cwd : "~",
             v->t.on_alt ? "[alt] " : "",
             v->pt.master < 0 ? "  (process exited)" : "");
    (void)fdk_statusbar_push(w->statusbar, ctx, msg);
}

void tapp_quit(tapp *app) {
    /* close every window (the cascade sets quit when the last goes) */
    while (app->nwindows > 0) {
        tapp_close_window(app->windows[0]);
    }
}

/* normalized family key: lowercase, spaces and dashes dropped —
 * "JetBrainsMono Nerd Font" must match a "JetBrains Mono" request */
static void norm_family(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (const char *p = in; *p != '\0' && o + 1 < cap; p++) {
        if (*p == ' ' || *p == '-') {
            continue;
        }
        out[o++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    }
    out[o] = '\0';
}

/* the Nerd-Font-patched face of the requested mono families: the
 * enumerate list only ever matched the plain names, so a box whose
 * mono face IS its Nerd Font (the owner's Artix setup — their other
 * terminals draw the prompt icons from it directly) fell through to
 * the system default and the icons landed in .notdef */
static fdk_font *load_nerd_variant(const fdk_font_info *infos,
                                   size_t n, int px) {
    fdk_font *f = NULL;
    for (size_t i = 0; f == NULL && i < n; i++) {
        char nf[128];
        norm_family(infos[i].family, nf, sizeof nf);
        size_t len = strlen(nf);
        if (len < 8 || strcmp(nf + len - 8, "nerdfont") != 0) {
            continue;
        }
        f = fdk_font_load(infos[i].path, px);
        if (f != NULL && !fdk_font_has_glyph(f, 'A')) {
            /* icon-only faces (Symbols Nerd Font) cannot be the
             * terminal font — reject them like a failed load */
            fdk_font_destroy(f);
            f = NULL;
        }
    }
    return f;
}

/* ---- main -------------------------------------------------------------------- */

static void load_fonts(tapp *app) {
    /* the mono font at the profile's px */
    int px = app->active.font_px;
    fdk_font_info *infos = NULL;
    size_t n = 0;
    static const char *const families[] = {
        "DejaVu Sans Mono", "Liberation Mono", "Nimbus Mono PS",
        "Noto Sans Mono", "JetBrains Mono", "monospace",
    };
    app->font = NULL;
    if (fdk_ok(fdk_font_enumerate(&infos, &n))) {
        for (size_t f = 0; app->font == NULL && f < 6; f++) {
            for (size_t i = 0; i < n; i++) {
                if (strcmp(infos[i].family, families[f]) == 0) {
                    app->font = fdk_font_load(infos[i].path, (int)px);
                    if (app->font != NULL &&
                        !fdk_font_has_glyph(app->font, 'A')) {
                        /* a face that loads but cannot draw Latin
                         * (broken instance, symbol-only pick) would
                         * render the whole terminal blank — skip it
                         * like a failed load and try the next
                         * family */
                        fdk_font_destroy(app->font);
                        app->font = NULL;
                    }
                    break;
                }
            }
        }
        fdk_font_info_list_free(infos, n);
    }
    if (app->font == NULL) {
        /* no plain family matched — the box may only carry the
         * Nerd-Font-patched faces; they cover Latin just fine and
         * the owner's other terminals draw the prompt icons from
         * them directly */
        app->font = load_nerd_variant(infos, n, (int)px);
    }
    if (app->font == NULL) {
        app->font = fdk_font_load_system_default(px);
    }
    int ui_px = fdk_ui_font_px();
    app->font_ui = fdk_font_load_system_default(ui_px > 0 ? ui_px : 15);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    tapp app;
    memset(&app, 0, sizeof app);
    snprintf(app.app_id, sizeof app.app_id, "org.fdk.testapps.fdk-term");

    /* --smoke: run N frames and quit (the harness contract) */
    const char *frames = getenv("FDK_TESTAPP_FRAMES");
    app.frame_limit = frames != NULL ? atol(frames) : 0;

    fdk_init_options opts = {0};
    opts.app_id = app.app_id;
    if (!fdk_ok(fdk_init(&app.ctx, &opts))) {
        fprintf(stderr, "fdk-term: no display\n");
        return 1;
    }
    fdk_prefs_open(app.app_id, &app.prefs); /* fail-soft */
    profile_load(&app, NULL, &app.active);

    load_fonts(&app);
    tcolor_set_scheme(app.active.scheme);

    /* session restore: window count + grid sizes */
    int nwins = 1;
    if (app.prefs != NULL) {
        nwins = tapp_pref_int(&app, "session.windows", 1);
        if (nwins < 1) {
            nwins = 1;
        }
        if (nwins > 8) {
            nwins = 8;
        }
    }
    for (int i = 0; i < nwins; i++) {
        char key[32];
        snprintf(key, sizeof key, "session.win%d.cols", i);
        int cols = tapp_pref_int(&app, key, 100);
        snprintf(key, sizeof key, "session.win%d.rows", i);
        int rows = tapp_pref_int(&app, key, 30);
        twindow *w = tapp_open_window(&app, cols, rows);
        if (w == NULL) {
            break;
        }
        tapp_new_tab(w, NULL);
        (void)fdk_window_set_opacity(w->window, app.active.opacity);
    }

    /* the pump */
    while (!app.quit) {
        int pumped = fdk_pump_events(app.ctx, 15);
        if (pumped < 0) {
            break;
        }
        for (int i = 0; i < app.nwindows; i++) {
            twindow *w = app.windows[i];
            if (w == NULL || w->closing) {
                continue;
            }
            fdk_surface *s = NULL;
            if (fdk_ok(fdk_window_get_surface(w->window, &s)) &&
                !fdk_surface_frame_ready(s)) {
                continue;
            }
            if (fdk_widget_tree_has_damage(w->root)) {
                (void)fdk_window_paint(w->window);
            }
        }
        app.frames++;
        if (app.frame_limit > 0 && app.frames >= app.frame_limit) {
            /* the rig hook: dump the first view's visible text
             * BEFORE the windows close */
            {
                extern void tview_dump_grid(const tview *v, FILE *out);
                const char *dump = getenv("FDK_TERM_DUMP");
                if (dump != NULL && app.nwindows > 0 &&
                    app.windows[0]->ntabs > 0 &&
                    app.windows[0]->tabs[0]->nviews > 0) {
                    FILE *out = fopen(dump, "w");
                    if (out != NULL) {
                        tview_dump_grid(
                            app.windows[0]->tabs[0]->views[0], out);
                        fclose(out);
                    }
                }
            }
            tapp_quit(&app);
        }
    }

    /* save the session */
    if (app.prefs != NULL) {
        (void)fdk_prefs_set_int(app.prefs, "session.windows", app.nwindows);
        for (int i = 0; i < app.nwindows; i++) {
            twindow *w = app.windows[i];
            char key[32];
            snprintf(key, sizeof key, "session.win%d.cols", i);
            (void)fdk_prefs_set_int(app.prefs, key, w->cols);
            snprintf(key, sizeof key, "session.win%d.rows", i);
            (void)fdk_prefs_set_int(app.prefs, key, w->rows);
        }
    }

    while (app.nwindows > 0) {
        tapp_close_window(app.windows[0]);
    }
    if (app.notify != NULL) {
        fdk_notify_close(app.notify);
    }
    if (app.font != NULL) {
        fdk_font_destroy(app.font);
    }
    if (app.font_ui != NULL) {
        fdk_font_destroy(app.font_ui);
    }
    if (app.prefs != NULL) {
        fdk_prefs_save(app.prefs);
        fdk_prefs_destroy(app.prefs);
    }
    fdk_shutdown(app.ctx);
    printf("fdk-term: exited cleanly after %ld frames\n", app.frames);
    (void)argc;
    (void)argv;
    return 0;
}

/* ---- zoom + font reload + prefs entry ------------------------------------ */

void tapp_zoom(tapp *app, int dir) {
    tprofile *pr = &app->active;
    if (dir > 0) {
        pr->font_px += 1;
    } else if (dir < 0) {
        pr->font_px -= 1;
    } else {
        pr->font_px = 13;
    }
    if (pr->font_px < 8) {
        pr->font_px = 8;
    }
    if (pr->font_px > 32) {
        pr->font_px = 32;
    }
    profile_save(app, NULL, pr);
    tapp_reload_font(app);
}

void tapp_reload_font(tapp *app) {
    if (app->font != NULL) {
        fdk_font_destroy(app->font);
    }
    app->font = NULL;
    int px = app->active.font_px;
    fdk_font_info *infos = NULL;
    size_t n = 0;
    static const char *const families[] = {
        "DejaVu Sans Mono", "Liberation Mono", "Nimbus Mono PS",
        "Noto Sans Mono", "monospace",
    };
    if (fdk_ok(fdk_font_enumerate(&infos, &n))) {
        for (size_t f = 0; app->font == NULL && f < 5; f++) {
            for (size_t i = 0; i < n; i++) {
                if (strcmp(infos[i].family, families[f]) == 0) {
                    app->font = fdk_font_load(infos[i].path, (int)px);
                    break;
                }
            }
        }
        fdk_font_info_list_free(infos, n);
    }
    if (app->font == NULL) {
        app->font = fdk_font_load_system_default(px);
    }
    for (int wi = 0; wi < app->nwindows; wi++) {
        twindow *w = app->windows[wi];
        for (int t = 0; t < w->ntabs; t++) {
            for (int k = 0; k < w->tabs[t]->nviews; k++) {
                tview_refont(w->tabs[t]->views[k], app->font);
            }
        }
    }
}

void tapp_open_prefs(tapp *app) {
    tprefs_run(app);
}
