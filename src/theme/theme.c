/*
 * theme.c — theme object lifecycle, the built-in default, the
 * current-default switch, and the global-settings boot (1.4.13)
 *
 * The built-in default is the 1.4.0 "Modern" retune of the Phase 6
 * v1 palette: same token roles, flatter and calmer values. The v1
 * look survives exactly — every value it painted with is reproducible
 * as a .fdk theme file (docs/fdk-theme-format.md records the v1
 * palette for exactly that). What the retune changes and why:
 *
 *  - SURFACES: the window/control contrast was cut roughly in half
 *    (0.07/0.16 v1 -> 0.09/0.12 Modern) — modern apps read controls
 *    as shapes on a surface, not as raised chrome.
 *  - BORDERS: v1's 0.30 border read as heavy outlining at rest; the
 *    Modern border is barely-there at rest and does its work on hover
 *    and focus instead.
 *  - ACCENT: v1's sky blue (0.35/0.65/0.95) goes slightly indigo and
 *    brighter (0.42/0.62/1.00) — enough to pop against the darker
 *    control family while keeping white-accent-text contrast > 3.5:1.
 *  - NEW FAMILIES: sidebar/menu surfaces one step off the control
 *    fill, flat entries (window-tone fill + subtle border — the
 *    sunken-field convention replacing v1's raised-field entry),
 *    accent hover/pressed fills + accent-on-accent text, a link
 *    color, and a row-hover softer than the control hover.
 *
 * Parsing lives in parse.c; this file owns the object.
 *
 * The one deliberate internal cycle in FDK so far lives here: the
 * widget catalog resolves tokens through fdk__theme_current(), and
 * fdk_theme_set_default() calls back into the widget core to
 * invalidate every live tree. See docs/architecture.md.
 */

#define FDK_LOG_TAG "theme"

#include "theme_internal.h"
#include "../widget/widget_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- The built-in default (the 1.4.0 "Modern" palette) ---- */

/* The built-in name lives in a static array so the struct needs no
 * const-dropping cast: the instance is never mutated or freed. */
static char g_builtin_name[] = "FDK Modern";

static fdk_theme g_builtin = {
    .name = g_builtin_name,
    .author = NULL,
    .colors = {
        /* Surfaces: calm, low-contrast layering. */
        [FDK_TK_WINDOW_BACKGROUND] = {0.09f, 0.10f, 0.14f, 1.0f},
        [FDK_TK_TEXT] = {0.93f, 0.94f, 0.97f, 1.0f},
        [FDK_TK_TEXT_DISABLED] = {0.44f, 0.46f, 0.52f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND] = {0.13f, 0.15f, 0.20f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_HOVER] = {0.17f, 0.20f, 0.27f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_PRESSED] = {0.21f, 0.25f, 0.33f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_DISABLED] = {0.11f, 0.12f, 0.16f, 1.0f},
        [FDK_TK_CONTROL_BORDER] = {0.20f, 0.23f, 0.30f, 1.0f},
        [FDK_TK_ACCENT] = {0.42f, 0.62f, 1.00f, 1.0f},
        [FDK_TK_TRACK] = {0.11f, 0.12f, 0.16f, 1.0f},
        /* 1.3.2 tokens, recalibrated against the Modern surfaces. */
        [FDK_TK_TOOLTIP_BACKGROUND] = {0.95f, 0.96f, 0.99f, 1.0f},
        [FDK_TK_TOOLTIP_TEXT] = {0.11f, 0.13f, 0.19f, 1.0f},
        [FDK_TK_TOOLTIP_BORDER] = {0.70f, 0.73f, 0.82f, 1.0f},
        [FDK_TK_SELECTION_BACKGROUND] = {0.42f, 0.62f, 1.00f, 0.38f},
        [FDK_TK_SELECTION_TEXT] = {0.95f, 0.96f, 0.99f, 1.0f},
        [FDK_TK_FOCUS_RING] = {0.42f, 0.62f, 1.00f, 0.90f},
        [FDK_TK_SUCCESS] = {0.38f, 0.74f, 0.45f, 1.0f},
        [FDK_TK_WARNING] = {0.91f, 0.69f, 0.27f, 1.0f},
        [FDK_TK_DANGER] = {0.91f, 0.36f, 0.38f, 1.0f},
        /* 1.4.0: the modern-face families — popup and sidebar
         * surfaces a half-step above the control fill; the accent's
         * own hover/pressed states and its on-fill text; the link
         * blue; the flat-entry pair; the soft row hover. */
        [FDK_TK_SIDEBAR_BACKGROUND] = {0.10f, 0.12f, 0.16f, 1.0f},
        [FDK_TK_MENU_BACKGROUND] = {0.15f, 0.17f, 0.23f, 1.0f},
        [FDK_TK_ACCENT_HOVER] = {0.50f, 0.69f, 1.00f, 1.0f},
        [FDK_TK_ACCENT_PRESSED] = {0.34f, 0.52f, 0.88f, 1.0f},
        [FDK_TK_ACCENT_TEXT] = {0.97f, 0.98f, 1.00f, 1.0f},
        [FDK_TK_LINK] = {0.55f, 0.73f, 1.00f, 1.0f},
        [FDK_TK_ENTRY_BACKGROUND] = {0.07f, 0.08f, 0.11f, 1.0f},
        [FDK_TK_ENTRY_BORDER] = {0.17f, 0.19f, 0.25f, 1.0f},
        [FDK_TK_ROW_HOVER] = {0.14f, 0.16f, 0.22f, 0.60f},
    },
    .metrics = {
        [FDK_TM_BUTTON_CORNER_RADIUS] = 8,
        [FDK_TM_SEPARATOR_THICKNESS] = 1,
        [FDK_TM_TITLE_BAR_HEIGHT] = 28,
        [FDK_TM_SCROLLBAR_WIDTH] = 12,
        [FDK_TM_MENU_ITEM_HEIGHT] = 26,
        [FDK_TM_TOOLTIP_CORNER_RADIUS] = 6,
        /* 1.4.0: fields round softer than buttons, menus round like
         * tooltips, rows breathe at 30px, rings stroke at 2px. */
        [FDK_TM_ENTRY_CORNER_RADIUS] = 6,
        [FDK_TM_MENU_CORNER_RADIUS] = 8,
        [FDK_TM_LIST_ROW_HEIGHT] = 30,
        [FDK_TM_SCROLLBAR_OVERLAY_WIDTH] = 6,
        [FDK_TM_ICONVIEW_CELL_WIDTH] = 96,
        [FDK_TM_ICONVIEW_CELL_HEIGHT] = 84,
        [FDK_TM_TEXTVIEW_PAD] = 8,
        [FDK_TM_FOCUS_RING_WIDTH] = 2,
    },
};

/* The static instance's name/author are (char *) casts of literals:
 * the struct is never mutated or freed, so the non-const pointers are
 * inert. Every theme an application can touch is a heap copy. */

const fdk_theme *fdk__theme_builtin(void) {
    return &g_builtin;
}

/* ---- The current default ---- */

/* The global-settings boot (1.4.13, reworked 1.4.14). The ONE-SHOT
 * guard lives here; the resolution itself — $FDK_THEME, then the
 * application's own store, then the global "fdk" store, clamped by
 * the developer whitelist — lives in settings.c, which also owns
 * the live follow (the inotify watch the pump polls) and the
 * installed theme's lifetime.
 *
 * The boot runs lazily at the FIRST resolution of the current
 * default (the first paint, fdk_theme_get_default(), any NULL-theme
 * accessor) — before which an application may pre-empt it by
 * calling fdk_theme_set_default() itself; that call sets the boot
 * flag AND the ownership flag below, and the process is thereafter
 * entirely the application's (the explicit-override-owns-the-choice
 * contract, fdk_theme.h).
 *
 * Every failure is soft, per the prefs resilience rule — settings.c
 * warns and keeps the current theme. A themed launch must never be
 * a failed launch. */
static bool g_booted;           /* the one-shot guard; set FIRST
                                 * inside the boot because the
                                 * set_default() it calls re-enters
                                 * via fdk__theme_current() */
static bool g_settings_installing; /* the settings engine is on the
                                 * stack of a set_default call: NOT
                                 * an application opt-out */
static bool g_app_owned;        /* application code set the default
                                 * itself — settings stand down
                                 * (checked by settings.c's watch) */

/* NULL means "the built-in theme". Only heap themes (or NULL) are ever
 * stored here — the built-in is referred to, never installed, so it
 * can never be destroyed out from under the process. */
static fdk_theme *g_current;

/* The opt-out flag's reader (theme_internal.h): settings.c's watch
 * and recheck stand down when the application owns the choice. */
bool fdk__theme_settings_app_owned(void) {
    return g_app_owned;
}

/* The settings engine's install path: same switch, no opt-out. */
void fdk__theme_set_default_settings(fdk_theme *theme) {
    g_settings_installing = true;
    fdk_theme_set_default(theme);
    g_settings_installing = false;
}

/* The switch's shared core. `explicit_call` distinguishes an
 * application's fdk_theme_set_default() (sets the ownership flag —
 * the process's theme is now the application's, and the settings
 * engine stands down) from the settings engine's own installs and
 * the destroy-path's revert (which must NOT flip ownership: an
 * app destroying a borrowed pointer must not accidentally opt the
 * process back INTO settings-following). */
static void set_default_impl(fdk_theme *theme, bool explicit_call) {
    /* An explicit set is an opt-out: the application that installs
     * its own theme BEFORE anything resolves the current default
     * owns the process's choice, and the global setting never gets
     * a chance to override it. Inside the boot itself the flag is
     * already set, so this line is a no-op there. */
    g_booted = true;
    if (explicit_call) {
        g_app_owned = true;
    }

    fdk_theme *next = (theme != NULL) ? theme : &g_builtin;
    if (next == fdk__theme_current()) {
        return; /* already current: no repaint storm */
    }
    g_current = (next == &g_builtin) ? NULL : next;

    /* Every live tree repaints on its next paint: paint hooks resolve
     * tokens at paint time, so a full damage mark per root is the
     * whole switch. */
    fdk__widget_roots_invalidate_all();
}

void fdk_theme_set_default(fdk_theme *theme) {
    set_default_impl(theme, !g_settings_installing);
}

/* fdk_alloc'd copy (NULL -> NULL). See theme_internal.h for why this
 * is not the widget layer's fdk__strdup. */
char *fdk__theme_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = fdk_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

fdk_theme *fdk__theme_current(void) {
    if (!g_booted) {
        /* One-shot even on soft failure: mark FIRST (the settings
         * boot's own installs re-enter this function through the
         * switch below, and a resolve-nothing boot must not rerun
         * on every paint either). */
        g_booted = true;
        fdk__theme_settings_boot();
    }
    return (g_current != NULL) ? g_current : &g_builtin;
}

/* Internal, TEST-only: rewind the one-shot boot (and the settings
 * engine's installed theme + whitelist with it) so the next current
 * resolution re-runs it — each scenario in
 * tests/test_theme_discovery.c starts from a clean slate. Not part
 * of the public API, never installed. */
void fdk__theme_boot_reset_for_tests(void) {
    fdk__theme_settings_reset_for_tests(); /* destroys the install,
                                            * reverting current first */
    g_booted = false;
    g_app_owned = false;
    g_settings_installing = false;
}

fdk_theme *fdk_theme_get_default(void) {
    return fdk__theme_current();
}

/* ---- Lifecycle ---- */

/* Initializes `t` as a copy of the built-in default (name/author
 * become owned copies). Returns false only on allocation failure, in
 * which case nothing was allocated. */
static bool init_from_builtin(fdk_theme *t) {
    *t = g_builtin; /* struct copy; the static's pointers are
                     * immediately replaced with owned copies below.
                     * path copies as NULL — only fdk_theme_load()
                     * ever sets it, and create_default/parse themes
                     * are genuinely pathless. */
    t->name = NULL;
    t->author = NULL;
    t->path = NULL;
    t->name = fdk__theme_strdup(g_builtin.name);
    if (t->name == NULL) {
        return false;
    }
    return true;
}

fdk_theme *fdk_theme_create_default(void) {
    fdk_theme *t = fdk_alloc(sizeof *t);
    if (t == NULL) {
        FDK_ERROR("fdk_theme_create_default: out of memory");
        return NULL;
    }
    if (!init_from_builtin(t)) {
        fdk_free(t);
        FDK_ERROR("fdk_theme_create_default: out of memory");
        return NULL;
    }
    return t;
}

void fdk_theme_destroy(fdk_theme *theme) {
    if (theme == NULL) {
        return;
    }
    /* The settings engine's ownership slot must not dangle if this
     * is the theme it installed (an application may legitimately
     * destroy the borrowed current-default pointer it holds). */
    fdk__theme_settings_forget(theme);
    if (theme == g_current) {
        /* The current default must never dangle: revert first (this
         * also repaints, showing the built-in palette). The revert
         * is NOT an application call — no ownership flip. */
        set_default_impl(NULL, false);
    }
    fdk_free(theme->name);
    fdk_free(theme->author);
    fdk_free(theme->path);
    fdk_free(theme);
}

/* ---- Access ---- */

const char *fdk_theme_name(const fdk_theme *theme) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    return (t->name != NULL) ? t->name : "FDK Modern";
}

const char *fdk_theme_author(const fdk_theme *theme) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    return t->author; /* NULL when unset (documented) */
}

/* The file this theme was loaded from (fdk_theme_load — and so
 * fdk_theme_find, which loads what it finds). NULL for themes that
 * came from anywhere else: the built-in default, create_default(),
 * parse-from-memory. The string is owned by the theme and valid
 * until destroy; a NULL theme means the current default. Purely
 * informational — the mirror of fdk_font_get_file_path(). */
const char *fdk_theme_file_path(const fdk_theme *theme) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    return t->path;
}

fdk_color fdk_theme_get_color(const fdk_theme *theme,
                                fdk_theme_token token) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    if ((unsigned)token >= (unsigned)FDK_TK_COUNT) {
        FDK_WARN("fdk_theme_get_color: token %d out of range",
                 (int)token);
        return (fdk_color){0.0f, 0.0f, 0.0f, 1.0f};
    }
    return t->colors[token];
}

fdk_i32 fdk_theme_get_metric(const fdk_theme *theme,
                               fdk_theme_metric metric) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    if ((unsigned)metric >= (unsigned)FDK_TM_COUNT) {
        FDK_WARN("fdk_theme_get_metric: metric %d out of range",
                  (int)metric);
        return 0;
    }
    return t->metrics[metric];
}

/* ---- Programmatic modification ---- */

fdk_result fdk_theme_set_color(fdk_theme *theme, fdk_theme_token token,
                               fdk_color color) {
    if (theme == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((unsigned)token >= (unsigned)FDK_TK_COUNT) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    theme->colors[token] = color;
    return FDK_OK;
}

fdk_result fdk_theme_set_metric(fdk_theme *theme, fdk_theme_metric metric,
                                fdk_i32 value) {
    if (theme == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_i32 lo = 0, hi = 0;
    switch (metric) {
    case FDK_TM_BUTTON_CORNER_RADIUS:
        lo = 0;
        hi = 32;
        break;
    case FDK_TM_SEPARATOR_THICKNESS:
        lo = 1;
        hi = 8;
        break;
    case FDK_TM_TITLE_BAR_HEIGHT:
        lo = 12;
        hi = 64;
        break;
    case FDK_TM_SCROLLBAR_WIDTH:
        lo = 6;
        hi = 24;
        break;
    case FDK_TM_MENU_ITEM_HEIGHT:
        lo = 16;
        hi = 48;
        break;
    case FDK_TM_TOOLTIP_CORNER_RADIUS:
        lo = 0;
        hi = 16;
        break;
    case FDK_TM_ENTRY_CORNER_RADIUS:
        lo = 0;
        hi = 16;
        break;
    case FDK_TM_MENU_CORNER_RADIUS:
        lo = 0;
        hi = 16;
        break;
    case FDK_TM_LIST_ROW_HEIGHT:
        lo = 16;
        hi = 48;
        break;
    case FDK_TM_FOCUS_RING_WIDTH:
        lo = 1;
        hi = 4;
        break;
    case FDK_TM_SCROLLBAR_OVERLAY_WIDTH:
        lo = 4;
        hi = 12;
        break;
    case FDK_TM_ICONVIEW_CELL_WIDTH:
        lo = 64;
        hi = 192;
        break;
    case FDK_TM_ICONVIEW_CELL_HEIGHT:
        lo = 64;
        hi = 224;
        break;
    case FDK_TM_TEXTVIEW_PAD:
        lo = 0;
        hi = 32;
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (value < lo || value > hi) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    theme->metrics[metric] = value;
    return FDK_OK;
}

fdk_result fdk_theme_set_name(fdk_theme *theme, const char *name) {
    if (theme == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (name == NULL) {
        fdk_free(theme->name);
        theme->name = fdk__theme_strdup("FDK Modern");
        return (theme->name != NULL) ? FDK_OK : FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n = strlen(name);
    if (n > FDK_THEME_STRING_MAX - 1) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    char *copy = fdk__theme_strdup(name);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(theme->name);
    theme->name = copy;
    return FDK_OK;
}
