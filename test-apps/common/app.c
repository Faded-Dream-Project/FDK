/* app.c — see app.h. */
#include "app.h"
#include "icons.h"

#include "fdk/fdk_i18n.h"
#include "fdk/fdk_theme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_u32 ta_status_ctx(ta_app *app) {
    return fdk_statusbar_get_context_id(app->statusbar, "main");
}

bool ta_init(ta_app *app, const char *name) {
    memset(app, 0, sizeof *app);
    static char id[64];
    snprintf(id, sizeof id, "org.fdk.testapps.%s", name);
    app->app_id = id;

    fdk_init_options opts = {0};
    opts.app_id = id;
    if (!fdk_ok(fdk_init(&app->ctx, &opts))) {
        fprintf(stderr, "test-app %s: fdk_init failed (no display?)\n",
                name);
        return false;
    }
    fdk_prefs_open(id, &app->prefs); /* fail-soft: in-memory only */
    return true;
}

static void ta_window_event(fdk_window *w, const fdk_event_data *ev,
                            void *user) {
    (void)w;
    ta_app *app = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        app->quit = true;
    }
}

bool ta_open(ta_app *app, const char *display_name, fdk_i32 w,
             fdk_i32 h) {
    snprintf(app->label, sizeof app->label, "%s", display_name);
    const char *limit_s = getenv("FDK_TESTAPP_FRAMES");
    app->frame_limit = (limit_s != NULL) ? atol(limit_s) : 0;

    fdk_window_options wopts = {0};
    wopts.title = display_name;
    wopts.width = w > 0 ? w : 1000;
    wopts.height = h > 0 ? h : 680;
    if (!fdk_ok(fdk_window_create(app->ctx, &wopts, &app->window))) {
        fprintf(stderr, "test-app %s: window create failed\n",
                app->label);
        return false;
    }
    (void)fdk_window_set_decorated(app->window, true);
    fdk_window_set_event_callback(app->window, ta_window_event, app);
    if (!fdk_ok(fdk_window_get_root(app->window, &app->root))) {
        fdk_window_destroy(app->window);
        app->window = NULL;
        return false;
    }

    int ui_px = fdk_ui_font_px();
    app->font_ui = fdk_font_load_system_default(ui_px > 0 ? ui_px : 15);

    /* root box: [menubar host][content][statusbar] */
    fdk_widget *frame = NULL;
    if (!fdk_ok(fdk_box_create(app->root, FDK_VERTICAL, &frame))) {
        return false;
    }
    fdk_box_set_padding(frame, 0);
    fdk_box_set_spacing(frame, 0);
    fdk_window_set_content(app->window, frame);

    if (!fdk_ok(fdk_box_create(frame, FDK_VERTICAL,
                               &app->menubar_host))) {
        return false;
    }
    fdk_box_set_padding(app->menubar_host, 0);
    fdk_box_set_spacing(app->menubar_host, 0);

    if (!fdk_ok(fdk_box_create(frame, FDK_VERTICAL, &app->content))) {
        return false;
    }
    fdk_box_set_padding(app->content, 0);
    fdk_box_set_spacing(app->content, 0);
    fdk_widget_set_expand(app->content, false, true);

    if (!fdk_ok(fdk_statusbar_create(frame, app->font_ui,
                                     &app->statusbar))) {
        return false;
    }

    fdk_window_show(app->window);
    (void)fdk_window_paint(app->window);
    return true;
}

void ta_status(ta_app *app, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (void)fdk_statusbar_push(app->statusbar, ta_status_ctx(app), buf);
}

bool ta_pump(ta_app *app) {
    if (app->quit) {
        return false;
    }
    int pumped = fdk_pump_events(app->ctx, 15);
    if (pumped < 0) {
        app->quit = true;
        fprintf(stderr, "%s: platform connection lost\n", app->label);
        return false;
    }
    if (app->quit) {
        return false;
    }
    fdk_surface *surface = NULL;
    if (fdk_ok(fdk_window_get_surface(app->window, &surface)) &&
        !fdk_surface_frame_ready(surface)) {
        return true;
    }
    if (fdk_widget_tree_has_damage(app->root)) {
        (void)fdk_window_paint(app->window);
    }
    app->frames++;
    if (app->frame_limit > 0 && app->frames >= app->frame_limit) {
        return false;
    }
    return true;
}

void ta_run(ta_app *app) {
    while (ta_pump(app)) {
    }
}

void ta_close(ta_app *app) {
    printf("%s: exited cleanly after %ld frames\n", app->label,
           app->frames);
    if (app->window != NULL) {
        fdk_window_destroy(app->window); /* frees the whole tree    */
        app->window = NULL;
    }
    ta_icon_buttons_free(); /* our tracked paint payloads           */
    if (app->font_ui != NULL) {
        fdk_font_destroy(app->font_ui);
        app->font_ui = NULL;
    }
    if (app->font_mono != NULL) {
        fdk_font_destroy(app->font_mono);
        app->font_mono = NULL;
    }
    if (app->prefs != NULL) {
        fdk_prefs_save(app->prefs);
        fdk_prefs_destroy(app->prefs);
        app->prefs = NULL;
    }
    icons_shutdown();
    if (app->ctx != NULL) {
        fdk_shutdown(app->ctx);
        app->ctx = NULL;
    }
}

/* ---- the icon button ---------------------------------------------
 *
 * FDK's catalog Button paints its own chrome and label; children of
 * a non-container widget keep their OWN bounds and paint after the
 * parent's chrome (verified in src/widget/widget.c paint_rec). So an
 * icon button is: a Button + a Canvas child pinned to the button's
 * bounds. The Canvas class has no destroy hook and no user-data
 * owner, so every payload is tracked here and freed at app close
 * (ta_icon_buttons_free). Bounds are re-pinned on every window
 * configure (ta_icon_buttons_relayout) — a plain-parented child is
 * nobody's layout responsibility but ours.
 */

typedef struct iconbtn {
    struct iconbtn *next;
    fdk_widget *button; /* the catalog Button                      */
    fdk_widget *canvas; /* the icon overlay child                  */
    char *icon;         /* icons/ stem (NULL = glyph only)          */
    fdk_row_icon glyph; /* floor when the SVG does not resolve      */
} iconbtn;

static iconbtn *g_icon_buttons;

void ta_icon_buttons_free(void) {
    iconbtn *ib = g_icon_buttons;
    while (ib != NULL) {
        iconbtn *next = ib->next;
        free(ib->icon);
        free(ib);
        ib = next;
    }
    g_icon_buttons = NULL;
}

void ta_icon_buttons_relayout(void) {
    for (iconbtn *ib = g_icon_buttons; ib != NULL; ib = ib->next) {
        fdk_rect b = fdk_widget_get_bounds(ib->button);
        /* the canvas rides AT the button's origin, filling it; the
         * button's own text (empty) still centers underneath */
        fdk_rect cv = {0, 0, b.width, b.height};
        fdk_widget_set_bounds(ib->canvas, cv);
    }
}

static void iconbtn_paint(fdk_widget *w, fdk_surface *surface,
                          fdk_rect bounds, fdk_rect clip, void *user) {
    iconbtn *ib = user;
    (void)w;
    (void)clip;
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    if (!fdk_widget_get_enabled(ib->button)) {
        ink = fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED);
    }

    fdk_surface *icon = NULL;
    if (ib->icon != NULL) {
        icon = icons_get(ib->icon, 16, 1.0f);
    }
    if (icon != NULL) {
        fdk_surface_info info;
        if (fdk_ok(fdk_surface_get_info(icon, &info))) {
            fdk_i32 dx = bounds.x + (bounds.width - info.width) / 2;
            fdk_i32 dy = bounds.y + (bounds.height - info.height) / 2;
            fdk_surface_blit_blend(
                surface, dx, dy, icon,
                (fdk_rect){0, 0, info.width, info.height});
        }
        return;
    }

    /* the floor: FDK's own vector glyph language, stroked by hand
     * with the theme ink (the same shapes fdk_row_icon draws). */
    fdk_i32 x = bounds.x + (bounds.width - 16) / 2;
    fdk_i32 y = bounds.y + (bounds.height - 16) / 2;    switch (ib->glyph) {
    case FDK_ROW_ICON_HOME:
        fdk_surface_draw_line_aa(surface, x + 2, y + 9, x + 8, y + 3,
                                 ink);
        fdk_surface_draw_line_aa(surface, x + 8, y + 3, x + 14, y + 9,
                                 ink);
        fdk_surface_draw_rect(surface,
                              (fdk_rect){x + 4, y + 9, 8, 6}, ink);
        break;
    case FDK_ROW_ICON_FOLDER:
        fdk_surface_draw_rounded_rect_aa(
            surface, (fdk_rect){x, y + 3, 16, 11}, 2, ink);
        fdk_surface_draw_line_aa(surface, x, y + 6, x + 6, y + 6, ink);
        fdk_surface_draw_line_aa(surface, x + 6, y + 6, x + 8, y + 3,
                                 ink);
        break;
    case FDK_ROW_ICON_DRIVE:
        fdk_surface_draw_rounded_rect_aa(
            surface, (fdk_rect){x + 1, y + 5, 14, 7}, 1, ink);
        fdk_surface_fill_circle(surface, x + 12, y + 8, 1, ink);
        break;
    case FDK_ROW_ICON_RECENT:
        fdk_surface_draw_circle_aa(surface, x + 8, y + 8, 6, ink);
        fdk_surface_draw_line_aa(surface, x + 8, y + 5, x + 8, y + 8,
                                 ink);
        fdk_surface_draw_line_aa(surface, x + 8, y + 8, x + 10, y + 10,
                                 ink);
        break;
    case FDK_ROW_ICON_FILE:
    default:
        fdk_surface_draw_rect(surface, (fdk_rect){x + 3, y + 1, 10,
                                                  14},
                              ink);
        fdk_surface_draw_line_aa(surface, x + 3, y + 5, x + 9, y + 5,
                                 ink);
        break;
    }
}

bool ta_icon_button_add(fdk_widget *bar, ta_app *app,
                        const ta_icon_button *spec) {
    if (bar == NULL || spec == NULL) {
        return false;
    }
    fdk_widget *btn = NULL;
    if (!fdk_ok(fdk_button_create(bar, app->font_ui, "", &btn))) {
        return false;
    }
    fdk_widget_set_natural_size(btn, 32, 28);

    iconbtn *ib = calloc(1, sizeof *ib);
    if (ib == NULL) {
        return false;
    }
    ib->button = btn;
    ib->glyph = spec->glyph;
    if (spec->icon != NULL) {
        ib->icon = strdup(spec->icon);
    }

    if (!fdk_ok(fdk_canvas_create(btn, iconbtn_paint, ib, &ib->canvas))) {
        free(ib->icon);
        free(ib);
        return false;
    }
    ib->next = g_icon_buttons;
    g_icon_buttons = ib;

    char tip[256];
    if (spec->shortcut != NULL && spec->shortcut[0] != '\0') {
        snprintf(tip, sizeof tip, "%s (%s)", spec->tooltip,
                 spec->shortcut);
    } else {
        snprintf(tip, sizeof tip, "%s", spec->tooltip);
    }
    (void)fdk_widget_set_tooltip(btn, tip);
    fdk_widget_set_accessible_name(btn, spec->tooltip);
    if (spec->toggle) {
        fdk_button_set_checked(btn, false);
    }
    if (spec->on_activate != NULL) {
        /* the payload the callback wants is the whole iconbtn, so it
         * can reach the button (checked state) and the app */
        fdk_button_set_on_activate(
            btn, (fdk_button_activate_fn)spec->on_activate, spec->user);
    }
    return true;
}

void ta_icon_widget_paint(fdk_widget *w, const char *icon,
                          fdk_row_icon glyph, ta_app *app) {
    (void)w;
    (void)icon;
    (void)glyph;
    (void)app;
    /* reserved for sidebar rows; the button path covers toolbars. */
}

/* ---- i18n ---------------------------------------------------------- */

static struct fdk_catalog *g_catalog;

struct fdk_catalog *ta_i18n_load(ta_app *app, const char *dir,
                                 const char *pref_key) {
    char lang[8] = {0};
    ta_pref_str(app, pref_key, "", lang, sizeof lang);
    if (lang[0] == '\0') {
        const char *env = getenv("LC_ALL");
        if (env == NULL || env[0] == '\0') {
            env = getenv("LANG");
        }
        if (env != NULL && strncmp(env, "ro", 2) == 0) {
            snprintf(lang, sizeof lang, "ro");
        }
    }
    if (g_catalog != NULL) {
        fdk_catalog_destroy(g_catalog);
        g_catalog = NULL;
    }
    if (strcmp(lang, "ro") == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s/ro.fmo", dir);
        (void)fdk_catalog_load(path, &g_catalog); /* fail-soft: EN */
    }
    return g_catalog;
}

const char *ta_(const char *msgid) {
    if (g_catalog != NULL && fdk_catalog_has(g_catalog, msgid)) {
        return fdk_catalog_get(g_catalog, msgid);
    }
    return msgid;
}

/* ---- prefs ---------------------------------------------------------- */

bool ta_pref_bool(ta_app *app, const char *key, bool def) {
    bool v = def;
    if (app->prefs != NULL) {
        v = fdk_prefs_get_bool(app->prefs, key, def);
    }
    return v;
}

int ta_pref_int(ta_app *app, const char *key, int def) {
    int v = def;
    if (app->prefs != NULL) {
        const char *s = fdk_prefs_get(app->prefs, key, NULL);
        if (s != NULL && s[0] != '\0') {
            v = atoi(s);
        }
    }
    return v;
}

double ta_pref_double(ta_app *app, const char *key, double def) {
    double v = def;
    if (app->prefs != NULL) {
        const char *s = fdk_prefs_get(app->prefs, key, NULL);
        if (s != NULL && s[0] != '\0') {
            v = atof(s);
        }
    }
    return v;
}

bool ta_pref_str(ta_app *app, const char *key, const char *def,
                 char *out, size_t cap) {
    if (app->prefs != NULL) {
        const char *s = fdk_prefs_get(app->prefs, key, NULL);
        if (s != NULL) {
            snprintf(out, cap, "%s", s);
            return true;
        }
    }
    if (def != NULL) {
        snprintf(out, cap, "%s", def);
    } else {
        out[0] = '\0';
    }
    return false;
}
