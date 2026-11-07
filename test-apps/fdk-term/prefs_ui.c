/* prefs_ui.c — the preferences window: every profile knob as a real
 * widget (the brief's "a preferences window" for fdk-term), applied
 * LIVE to the running terminals. */
#include "term_app.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    tapp *app;
    fdk_window *win;
    fdk_widget *root;
    /* profile editors */
    fdk_widget *font_px;
    fdk_widget *scheme;
    fdk_widget *shell;
    fdk_widget *workdir;
    fdk_widget *login;
    fdk_widget *scrollback;
    fdk_widget *copy_sel;
    fdk_widget *bell_aud, *bell_vis, *bell_notif;
    fdk_widget *opacity;
    fdk_widget *blink;
} tprefs;

static tprefs g_prefs;

static void apply_and_reload(tprefs *p) {
    tapp *app = p->app;
    tprofile *pr = &app->active;

    pr->font_px = (int)fdk_spin_get_value(p->font_px);
    pr->login_shell = fdk_toggle_is_checked(p->login);
    pr->scrollback = (int)fdk_spin_get_value(p->scrollback);
    pr->copy_on_select = fdk_toggle_is_checked(p->copy_sel);
    pr->bell_audible = fdk_toggle_is_checked(p->bell_aud);
    pr->bell_visual = fdk_toggle_is_checked(p->bell_vis);
    pr->bell_notify = fdk_toggle_is_checked(p->bell_notif);
    pr->opacity = fdk_slider_get_value(p->opacity);
    pr->cursor_blink_ms = (int)fdk_spin_get_value(p->blink);

    const char *shell = fdk_entry_get_text(p->shell);
    snprintf(pr->shell, sizeof pr->shell, "%s", shell);
    const char *wd = fdk_entry_get_text(p->workdir);
    snprintf(pr->workdir, sizeof pr->workdir, "%s", wd);

    fdk_i64 scheme_idx = fdk_combo_get_active(p->scheme);
    pr->scheme = scheme_idx >= 0 ? (tscheme)scheme_idx : TSCHEME_FOLLOW;

    /* persist + apply live */
    profile_save(app, NULL, pr);
    tcolor_set_scheme(pr->scheme);

    /* re-font every view (the font object is app-owned) */
    if (pr->font_px != app->last_font_px) {
        tapp_reload_font(app);
    }
    for (int wi = 0; wi < app->nwindows; wi++) {
        twindow *w = app->windows[wi];
        (void)fdk_window_set_opacity(w->window, pr->opacity);
        for (int t = 0; t < w->ntabs; t++) {
            for (int k = 0; k < w->tabs[t]->nviews; k++) {
                tview_invalidate(w->tabs[t]->views[k]);
            }
        }
    }
}

static void pref_changed_toggle(fdk_widget *w, bool checked, void *user) {
    (void)w; (void)checked; (void)user;
    apply_and_reload(&g_prefs);
}
static void pref_changed_combo(fdk_widget *w, size_t index, void *user) {
    (void)w; (void)index; (void)user;
    apply_and_reload(&g_prefs);
}
static void pref_changed_spin(fdk_widget *w, void *user) {
    (void)w; (void)user;
    apply_and_reload(&g_prefs);
}
static void pref_changed_slider(fdk_widget *w, void *user) {
    (void)w; (void)user;
    apply_and_reload(&g_prefs);
}
static void pref_changed_entry(fdk_widget *w, void *user) {
    (void)w; (void)user;
    apply_and_reload(&g_prefs);
}

static void prefs_close_cb(fdk_window *win, const fdk_event_data *ev,
                           void *user) {
    tprefs *p = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(win);
        p->win = NULL;
        p->app->prefs_ui = NULL;
        memset(&g_prefs, 0, sizeof g_prefs);
    }
}

void tprefs_run(tapp *app) {
    if (g_prefs.win != NULL) {
        return; /* one at a time */
    }
    tprofile *pr = &app->active;
    tprefs *p = &g_prefs;
    p->app = app;

    fdk_window_options wo = {0};
    wo.title = "fdk-term Preferences";
    wo.width = 460;
    wo.height = 560;
    if (!fdk_ok(fdk_window_create(app->ctx, &wo, &p->win))) {
        return;
    }
    (void)fdk_window_set_decorated(p->win, true);
    fdk_window_set_event_callback(p->win, prefs_close_cb, p);

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(p->win, &root);
    p->root = root;
    fdk_widget *sv = NULL;
    fdk_scrollview_create(root, &sv);
    fdk_scrollview_set_vertical_only(sv, true);
    fdk_widget_set_bounds(sv, (fdk_rect){0, 0, 460, 540});

    fdk_widget *box = NULL;
    fdk_box_create(sv, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 16);
    fdk_box_set_spacing(box, 10);
    fdk_scrollview_set_content(sv, box);

    /* the row helper: a flat [label | control] pair */
    #define ROW_START(lbl)                                                \
        fdk_widget *_row = NULL;                                          \
        fdk_box_create(box, FDK_HORIZONTAL, &_row);                       \
        fdk_box_set_padding(_row, 0);                                     \
        fdk_box_set_spacing(_row, 8);                                     \
        fdk_widget *_lab = NULL;                                          \
        fdk_label_create(_row, app->font_ui, lbl, &_lab);                 \
        fdk_widget_set_natural_size(_lab, 150, 26)

    /* --- Appearance --- */
    fdk_widget *f1 = NULL;
    fdk_frame_create(box, app->font_ui, "Appearance", &f1);
    {
        ROW_START("Font size (px)");
        fdk_spin_create(_row, app->font_ui, 8, 32, pr->font_px, &p->font_px);
        fdk_spin_set_on_changed(p->font_px, pref_changed_spin, NULL);
    }
    {
        ROW_START("Color scheme");
        fdk_combo_create(_row, app->font_ui, &p->scheme);
        for (int i = 0; i < TSCHEME_COUNT; i++) {
            fdk_combo_append(p->scheme, tscheme_name((tscheme)i), NULL);
        }
        fdk_combo_set_active(p->scheme, (fdk_i64)pr->scheme);
        fdk_combo_set_on_changed(p->scheme, pref_changed_combo, NULL);
    }
    {
        ROW_START("Background opacity");
        fdk_slider_create(_row, 0.05, 1.0, pr->opacity, &p->opacity);
        fdk_widget_set_expand(p->opacity, true, false);
        fdk_slider_set_on_changed(p->opacity, pref_changed_slider, NULL);
    }
    {
        ROW_START("Cursor blink (ms, 0=off)");
        fdk_spin_create(_row, app->font_ui, 0, 2000, pr->cursor_blink_ms,
                        &p->blink);
        fdk_spin_set_on_changed(p->blink, pref_changed_spin, NULL);
    }

    /* --- Shell --- */
    fdk_widget *f2 = NULL;
    fdk_frame_create(box, app->font_ui, "Command", &f2);
    {
        ROW_START("Shell");
        fdk_entry_create(_row, app->font_ui, pr->shell, &p->shell);
        fdk_widget_set_expand(p->shell, true, false);
        fdk_entry_set_placeholder(p->shell, "$SHELL");
        fdk_entry_set_on_changed(p->shell, pref_changed_entry, NULL);
    }
    {
        ROW_START("Working directory");
        fdk_entry_create(_row, app->font_ui, pr->workdir, &p->workdir);
        fdk_widget_set_expand(p->workdir, true, false);
        fdk_entry_set_placeholder(p->workdir, "default: home");
        fdk_entry_set_on_changed(p->workdir, pref_changed_entry, NULL);
    }
    {
        ROW_START("Login shell");
        fdk_toggle_create(_row, app->font_ui, "", &p->login);
        fdk_toggle_set_checked(p->login, pr->login_shell);
        fdk_toggle_set_on_changed(p->login, pref_changed_toggle, NULL);
    }

    /* --- Behavior --- */
    fdk_widget *f3 = NULL;
    fdk_frame_create(box, app->font_ui, "Behavior", &f3);
    {
        ROW_START("Scrollback lines");
        fdk_spin_create(_row, app->font_ui, 0, 1000000, pr->scrollback,
                        &p->scrollback);
        fdk_spin_set_on_changed(p->scrollback, pref_changed_spin, NULL);
    }
    {
        ROW_START("Copy on select");
        fdk_toggle_create(_row, app->font_ui, "", &p->copy_sel);
        fdk_toggle_set_checked(p->copy_sel, pr->copy_on_select);
        fdk_toggle_set_on_changed(p->copy_sel, pref_changed_toggle, NULL);
    }
    {
        ROW_START("Audible bell");
        fdk_toggle_create(_row, app->font_ui, "", &p->bell_aud);
        fdk_toggle_set_checked(p->bell_aud, pr->bell_audible);
        fdk_toggle_set_on_changed(p->bell_aud, pref_changed_toggle, NULL);
    }
    {
        ROW_START("Visual bell");
        fdk_toggle_create(_row, app->font_ui, "", &p->bell_vis);
        fdk_toggle_set_checked(p->bell_vis, pr->bell_visual);
        fdk_toggle_set_on_changed(p->bell_vis, pref_changed_toggle, NULL);
    }
    {
        ROW_START("Background-finish notify");
        fdk_toggle_create(_row, app->font_ui, "", &p->bell_notif);
        fdk_toggle_set_checked(p->bell_notif, pr->bell_notify);
        fdk_toggle_set_on_changed(p->bell_notif, pref_changed_toggle, NULL);
    }

    fdk_window_show(p->win);
    (void)fdk_window_paint(p->win);
    app->prefs_ui = (struct tprefs *)p;
}
