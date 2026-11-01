/* app.h — the tiny shared test-app shell.
 *
 * What every test-app repeats: init with its app_id, a decorated
 * window, the damage-gated pump, prefs, i18n catalogs (EN + RO),
 * an icon-capable toolbar button factory, a menu builder, theme
 * following (light/dark live), a quit path that is LSan-clean.
 *
 * NOT a framework: no widget subclasses, no event system, no CSS.
 * Static helpers, like examples/example_window.h.
 */
#ifndef TESTAPPS_APP_H
#define TESTAPPS_APP_H

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_window.h"
#include "fdk/fdk_prefs.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct ta_app {
    fdk_context *ctx;
    fdk_window *window;
    fdk_widget *root;
    fdk_widget *menubar_host; /* vertical box under the title band  */
    fdk_widget *content;      /* the app's own content box          */
    fdk_widget *statusbar;
    fdk_prefs *prefs;
    fdk_font *font_ui;   /* the UI font at fdk_ui_font_px()         */
    fdk_font *font_mono; /* monospace where an app wants it         */
    const char *app_id;
    char label[96];
    bool quit;
    long frames, frame_limit; /* FDK_TESTAPP_FRAMES=N exits early   */
    /* The app fills this in before ta_open: the current theme name
     * ("Faded Dream" default) and whether we follow the desktop. */
    void (*on_theme_changed)(struct ta_app *app, void *user);
    void *theme_user;
} ta_app;

/* fdk_init with `app_id` ("org.fdk.testapps.<name>"). */
bool ta_init(ta_app *app, const char *name);

/* The standard window: decorated, 1000x680 default, menu bar host at
 * the top, the app's content box in the middle, a statusbar at the
 * bottom. Sets Esc-to-quit OFF by default (apps register their own
 * bindings; fdk-edit wants Esc for other things). */
bool ta_open(ta_app *app, const char *display_name, fdk_i32 w, fdk_i32 h);

/* Statusbar helper: pushes context "main". */
void ta_status(ta_app *app, const char *fmt, ...);

/* One loop pass: pump, damage-gated paint, frame budget. */
bool ta_pump(ta_app *app);

/* Pumps until quit. */
void ta_run(ta_app *app);

/* Ordered teardown. */
void ta_close(ta_app *app);

/* ---- the icon toolbar --------------------------------------------
 *
 * FDK's Toolbar takes text buttons. A real app wants ICON buttons
 * with tooltips. This factory builds one: a catalog Button whose
 * paint callback blits the themed icon (16px) centered, plus a
 * tooltip with the keyboard shortcut in parentheses. `name` is an
 * icons/ stem or an fdk_row_icon fallback. */
typedef void (*ta_iconbtn_fn)(fdk_widget *button, void *user);

typedef struct ta_icon_button {
    const char *icon;   /* icons/ stem (may be NULL with glyph set) */
    fdk_row_icon glyph; /* fallback when the SVG does not resolve   */
    const char *tooltip;
    const char *shortcut; /* shown in the tooltip, may be NULL     */
    bool toggle;          /* paint the checked state when active   */
    ta_iconbtn_fn on_activate;
    void *user;
} ta_icon_button;

/* Appends an icon button to `bar` (any box works; toolbar too). */
bool ta_icon_button_add(fdk_widget *bar, ta_app *app,
                        const ta_icon_button *spec);

/* Re-pins every icon button's canvas overlay to its button's current
 * bounds — call from the window-configure handler (plain-parented
 * children are laid out by nobody but us). */
void ta_icon_buttons_relayout(void);

/* Frees the tracked icon-button payloads (after window destroy, at
 * app close; the Canvas class has no destroy hook of its own). */
void ta_icon_buttons_free(void);

/* A themed icon painted on any plain widget (the toolbar button
 * factory uses this; apps can use it for sidebar rows etc.). */
void ta_icon_widget_paint(fdk_widget *w, const char *icon,
                          fdk_row_icon glyph, ta_app *app);

/* ---- i18n ---------------------------------------------------------
 * Loads po/en.tacatalog and po/ro.tacatalog from `dir`. The active
 * language is a prefs key ("ui.language": "" / "en" / "ro"); empty
 * means English (the msgid language). Returns the ACTIVE catalog
 * (NULL = English). `app->font_ui` may be reloaded when the
 * language changes (CJK would want another face — not needed for
 * EN/RO, both Latin). */
struct fdk_catalog;
struct fdk_catalog *ta_i18n_load(ta_app *app, const char *dir,
                                 const char *pref_key);
/* The translate function every app uses: _("..."). */
const char *ta_(const char *msgid);

/* ---- prefs -------------------------------------------------------- */
/* Typed getters with defaults, so each app reads one-liners. */
bool ta_pref_bool(ta_app *app, const char *key, bool def);
int ta_pref_int(ta_app *app, const char *key, int def);
double ta_pref_double(ta_app *app, const char *key, double def);
/* Reads a string pref into `out` (cap bytes); returns false when
 * unset, in which case `def` is copied. */
bool ta_pref_str(ta_app *app, const char *key, const char *def,
                 char *out, size_t cap);

#endif /* TESTAPPS_APP_H */
