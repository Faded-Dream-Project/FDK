/*
 * theme.c — theme object lifecycle, the built-in default, the
 * current-default switch, and the global-settings boot (1.4.13)
 *
 * The built-in default is the 1.4.16 "Faded Dream" palette — the
 * gray-with-violet calm retune requested as the toolkit's home face
 * (FDK IS the Faded Dream ToolKit; the default finally says so on
 * screen). What changed from the 1.4.0 "Modern" palette and why:
 *
 *  - SURFACES went NEUTRAL: Modern carried a blue undertone in every
 *    gray (0.13/0.15/0.20); Faded Dream is a true gray ramp
 *    (#232329 window, #33333C controls) — calm means no hue pressure
 *    at rest. The 1.4.0 Modern values survive as a documented .fdk
 *    recipe in docs/fdk-theme-format.md, exactly as v1's did.
 *  - THE ACCENT family went VIOLET (#8F79D9, hover #A18DE3, pressed
 *    #7763C2): "purple, small parts around" — checked boxes, focus
 *    rings, progress fills, links; small areas, one hue, the single
 *    voice of color in the calm field.
 *  - SEMANTICS desaturated to the same calm register (success sage,
 *    warning muted amber, danger dusty rose) so an error banner never
 *    shouts over the gray world it sits in.
 *  - The titlebar family ships UNSET in the built-in (the fallback
 *    mask below): the band reads control_background/text as it
 *    always did, and only a theme that opts into a distinct chrome
 *    gets one.
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

/* ---- The built-in default (the 1.4.16 "Faded Dream" palette) ---- */

/* The built-in name lives in a static array so the struct needs no
 * const-dropping cast: the instance is never mutated or freed. */
static char g_builtin_name[] = "Faded Dream";

/* Helper: k/255 as a float, written once so the palette below reads
 * in 8-bit channel values (the .fdk file's hex, in decimals) and the
 * float/hex correspondence stays exact by construction. */
#define C8(k) ((fdk_f32)(k) / 255.0f)

static fdk_theme g_builtin = {
    .name = g_builtin_name,
    .author = NULL,
    .colors = {
        /* The gray ramp: calm, hueless layering. */
        [FDK_TK_WINDOW_BACKGROUND] = {C8(35), C8(35), C8(41), 1.0f},
        [FDK_TK_TEXT] = {C8(230), C8(230), C8(236), 1.0f},
        [FDK_TK_TEXT_DISABLED] = {C8(139), C8(139), C8(150), 1.0f},
        [FDK_TK_CONTROL_BACKGROUND] = {C8(51), C8(51), C8(60), 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_HOVER] = {C8(62), C8(62), C8(73), 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_PRESSED] = {C8(74), C8(74), C8(87), 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_DISABLED] = {C8(43), C8(43), C8(51), 1.0f},
        [FDK_TK_CONTROL_BORDER] = {C8(70), C8(70), C8(83), 1.0f},
        /* The one voice of color: violet, kept to small parts. */
        [FDK_TK_ACCENT] = {C8(143), C8(121), C8(217), 1.0f},
        [FDK_TK_TRACK] = {C8(42), C8(42), C8(49), 1.0f},
        /* 1.3.2 tokens, recalibrated for the gray field: tooltips go
         * near-white (the calm popup), selection is the violet at a
         * whisper, semantics desaturated to the same register. */
        [FDK_TK_TOOLTIP_BACKGROUND] = {C8(240), C8(239), C8(245), 1.0f},
        [FDK_TK_TOOLTIP_TEXT] = {C8(38), C8(38), C8(46), 1.0f},
        [FDK_TK_TOOLTIP_BORDER] = {C8(185), C8(183), C8(201), 1.0f},
        [FDK_TK_SELECTION_BACKGROUND] = {C8(143), C8(121), C8(217),
                                    C8(97)},  /* 0x61 */
        [FDK_TK_SELECTION_TEXT] = {C8(242), C8(241), C8(248), 1.0f},
        [FDK_TK_FOCUS_RING] = {C8(143), C8(121), C8(217), C8(230)}, /* 0xE6 */
        [FDK_TK_SUCCESS] = {C8(123), C8(185), C8(138), 1.0f},
        [FDK_TK_WARNING] = {C8(217), C8(178), C8(95), 1.0f},
        [FDK_TK_DANGER] = {C8(217), C8(124), C8(140), 1.0f},
        /* 1.4.0: the modern-face families, retuned onto the gray ramp
         * — sidebar/menu a step off the control fill, the violet's
         * own hover/pressed, flat entries sunk one step below the
         * window, the soft row hover. */
        [FDK_TK_SIDEBAR_BACKGROUND] = {C8(41), C8(41), C8(49), 1.0f},
        [FDK_TK_MENU_BACKGROUND] = {C8(55), C8(55), C8(66), 1.0f},
        [FDK_TK_ACCENT_HOVER] = {C8(161), C8(141), C8(227), 1.0f},
        [FDK_TK_ACCENT_PRESSED] = {C8(119), C8(99), C8(194), 1.0f},
        [FDK_TK_ACCENT_TEXT] = {C8(245), C8(243), C8(251), 1.0f},
        [FDK_TK_LINK] = {C8(169), C8(146), C8(232), 1.0f},
        [FDK_TK_ENTRY_BACKGROUND] = {C8(30), C8(30), C8(36), 1.0f},
        [FDK_TK_ENTRY_BORDER] = {C8(59), C8(59), C8(71), 1.0f},
        [FDK_TK_ROW_HOVER] = {C8(58), C8(58), C8(70), C8(89)}, /* 0x59 */
        /* 1.4.16: the titlebar family — deliberately UNSET here (see
         * colors_set below): the band falls back to the control/text/
         * border tokens above, pixel-identical to 1.4.15, and a theme
         * opts into a distinct chrome by setting the keys. The values
         * in this initializer are inert placeholders the fallback
         * never reads (create_default copies them but the mask says
         * unset, and get_color resolves through the fallback first). */
        [FDK_TK_TITLEBAR_BACKGROUND] = {0.0f, 0.0f, 0.0f, 0.0f},
        [FDK_TK_TITLEBAR_TEXT] = {0.0f, 0.0f, 0.0f, 0.0f},
        [FDK_TK_TITLEBAR_BORDER] = {0.0f, 0.0f, 0.0f, 0.0f},
        [FDK_TK_TITLEBAR_BUTTON_HOVER] = {0.0f, 0.0f, 0.0f, 0.0f},
        [FDK_TK_TITLEBAR_BUTTON_PRESSED] = {0.0f, 0.0f, 0.0f, 0.0f},
    },
    /* The override mask: every base token ships SET (a theme that
     * overrides nothing inherits exactly these values); every
     * titlebar token ships UNSET (the fallback family). init_from_
     * builtin copies this array along with the colors, so the mask
     * semantics survive create_default() and parse-into. Written one
     * designator per token (no GNU range extensions — the build is
     * -std=c17 -Wpedantic). */
    .colors_set = {
        [FDK_TK_WINDOW_BACKGROUND] = true,
        [FDK_TK_TEXT] = true,
        [FDK_TK_TEXT_DISABLED] = true,
        [FDK_TK_CONTROL_BACKGROUND] = true,
        [FDK_TK_CONTROL_BACKGROUND_HOVER] = true,
        [FDK_TK_CONTROL_BACKGROUND_PRESSED] = true,
        [FDK_TK_CONTROL_BACKGROUND_DISABLED] = true,
        [FDK_TK_CONTROL_BORDER] = true,
        [FDK_TK_ACCENT] = true,
        [FDK_TK_TRACK] = true,
        [FDK_TK_TOOLTIP_BACKGROUND] = true,
        [FDK_TK_TOOLTIP_TEXT] = true,
        [FDK_TK_TOOLTIP_BORDER] = true,
        [FDK_TK_SELECTION_BACKGROUND] = true,
        [FDK_TK_SELECTION_TEXT] = true,
        [FDK_TK_FOCUS_RING] = true,
        [FDK_TK_SUCCESS] = true,
        [FDK_TK_WARNING] = true,
        [FDK_TK_DANGER] = true,
        [FDK_TK_SIDEBAR_BACKGROUND] = true,
        [FDK_TK_MENU_BACKGROUND] = true,
        [FDK_TK_ACCENT_HOVER] = true,
        [FDK_TK_ACCENT_PRESSED] = true,
        [FDK_TK_ACCENT_TEXT] = true,
        [FDK_TK_LINK] = true,
        [FDK_TK_ENTRY_BACKGROUND] = true,
        [FDK_TK_ENTRY_BORDER] = true,
        [FDK_TK_ROW_HOVER] = true,
        /* FDK_TK_TITLEBAR_* deliberately stay false. */
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
        /* 1.4.16: ROUNDED — the calm default; the shape story is a
         * per-theme decision (Mono Chromatic ships CIRCLE). */
        [FDK_TM_BUTTON_SHAPE] = 0,
    },
};

/* The static instance's name/author are (char *) casts of literals:
 * the struct is never mutated or freed, so the non-const pointers are
 * inert. Every theme an application can touch is a heap copy. */

const fdk_theme *fdk__theme_builtin(void) {
    return &g_builtin;
}

/* ---- The fallback map (1.4.16) ---------------------------------------- */

/* One step of the titlebar family's read-through. The chain is a
 * single level deep by construction (every fallback target is a base
 * token), so no recursion guard is needed; the default arm is the
 * function's contract for every base token. */
fdk_theme_token fdk__token_fallback(fdk_theme_token token) {
    switch (token) {
    case FDK_TK_TITLEBAR_BACKGROUND:
        return FDK_TK_CONTROL_BACKGROUND;
    case FDK_TK_TITLEBAR_TEXT:
        return FDK_TK_TEXT;
    case FDK_TK_TITLEBAR_BORDER:
        return FDK_TK_CONTROL_BORDER;
    case FDK_TK_TITLEBAR_BUTTON_HOVER:
        return FDK_TK_CONTROL_BACKGROUND_HOVER;
    case FDK_TK_TITLEBAR_BUTTON_PRESSED:
        return FDK_TK_CONTROL_BACKGROUND_PRESSED;
    default:
        return token;
    }
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
    return (t->name != NULL) ? t->name : "Faded Dream";
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
    /* The fallback family (theme_internal.h): a titlebar token the
     * theme never overrode reads through to its base token — which
     * may itself be overridden (a partial theme that sets only
     * control_background re-themes the band, exactly as every theme
     * before 1.4.16 did). */
    if (!t->colors_set[token]) {
        token = fdk__token_fallback(token);
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
    theme->colors_set[token] = true; /* an override stops the fallback */
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
    case FDK_TM_BUTTON_SHAPE:
        lo = 0;
        hi = 2;
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
        theme->name = fdk__theme_strdup("Faded Dream");
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
