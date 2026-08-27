/*
 * theme.c — theme object lifecycle, the built-in default, and the
 * current-default switch
 *
 * The v1 palette lives on as the built-in default theme, byte-for-byte
 * the floats src/widget/statics.c painted with through Phase 6 —
 * installing the built-in theme (or never touching themes at all)
 * changes no pixels. Parsing lives in parse.c; this file owns the
 * object.
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

#include <string.h>

/* ---- The built-in default (the Phase 6 v1 palette) ---- */

/* The built-in name lives in a static array so the struct needs no
 * const-dropping cast: the instance is never mutated or freed. */
static char g_builtin_name[] = "FDK Dark";

static fdk_theme g_builtin = {
    .name = g_builtin_name,
    .author = NULL,
    .colors = {
        /* FDK_TK_WINDOW_BACKGROUND: new token, no v1 consumer; the
         * darkest surface color in the v1 family. */
        [FDK_TK_WINDOW_BACKGROUND] = {0.07f, 0.09f, 0.13f, 1.0f},
        [FDK_TK_TEXT] = {0.92f, 0.93f, 0.96f, 1.0f},
        [FDK_TK_TEXT_DISABLED] = {0.45f, 0.47f, 0.52f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND] = {0.16f, 0.18f, 0.26f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_HOVER] = {0.22f, 0.25f, 0.36f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_PRESSED] = {0.28f, 0.32f, 0.46f, 1.0f},
        [FDK_TK_CONTROL_BACKGROUND_DISABLED] = {0.12f, 0.13f, 0.18f, 1.0f},
        [FDK_TK_CONTROL_BORDER] = {0.30f, 0.33f, 0.44f, 1.0f},
        [FDK_TK_ACCENT] = {0.35f, 0.65f, 0.95f, 1.0f},
        [FDK_TK_TRACK] = {0.10f, 0.12f, 0.17f, 1.0f},
    },
    .metrics = {
        [FDK_TM_BUTTON_CORNER_RADIUS] = 8, /* was BTN_RADIUS */
        [FDK_TM_SEPARATOR_THICKNESS] = 1,
        [FDK_TM_TITLE_BAR_HEIGHT] = 28, /* was DECO_TITLE_H */
    },
};

/* The static instance's name/author are (char *) casts of literals:
 * the struct is never mutated or freed, so the non-const pointers are
 * inert. Every theme an application can touch is a heap copy. */

const fdk_theme *fdk__theme_builtin(void) {
    return &g_builtin;
}

/* ---- The current default ---- */

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

/* NULL means "the built-in theme". Only heap themes (or NULL) are ever
 * stored here — the built-in is referred to, never installed, so it
 * can never be destroyed out from under the process. */
static fdk_theme *g_current;

fdk_theme *fdk__theme_current(void) {
    return (g_current != NULL) ? g_current : &g_builtin;
}

void fdk_theme_set_default(fdk_theme *theme) {
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

fdk_theme *fdk_theme_get_default(void) {
    return fdk__theme_current();
}

/* ---- Lifecycle ---- */

/* Initializes `t` as a copy of the built-in default (name/author
 * become owned copies). Returns false only on allocation failure, in
 * which case nothing was allocated. */
static bool init_from_builtin(fdk_theme *t) {
    *t = g_builtin; /* struct copy; the static's pointers are
                     * immediately replaced with owned copies below */
    t->name = NULL;
    t->author = NULL;
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
    if (theme == g_current) {
        /* The current default must never dangle: revert first (this
         * also repaints, showing the built-in palette). */
        fdk_theme_set_default(NULL);
    }
    fdk_free(theme->name);
    fdk_free(theme->author);
    fdk_free(theme);
}

/* ---- Access ---- */

const char *fdk_theme_name(const fdk_theme *theme) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    return (t->name != NULL) ? t->name : "FDK Dark";
}

const char *fdk_theme_author(const fdk_theme *theme) {
    const fdk_theme *t =
        (theme != NULL) ? theme : fdk__theme_current();
    return t->author; /* NULL when unset (documented) */
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
        theme->name = fdk__theme_strdup("FDK Dark");
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
