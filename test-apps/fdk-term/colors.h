/* colors.h — the terminal palette: the 16 ANSI named colors, the
 * 256-color cube, and truecolor, resolved against the ACTIVE scheme
 * (theme-following: the 8/16 base slots read the FDK theme so a dark
 * terminal follows a dark desktop). */
#ifndef FDKTERM_COLORS_H
#define FDKTERM_COLORS_H

#include "fdk/fdk_types.h"

#include <stdint.h>
#include <stddef.h>

/* A terminal color: the DEFERRED pair xterm actually speaks. When
 * index >= 0 the cell references palette[index] (0..255), and the
 * RGB is resolved against the CURRENT scheme at PAINT time (a live
 * scheme switch never needs a grid rewrite). index == -1 means
 * truecolor (rgb directly) or unset (rgb is then the default). */
typedef struct {
    int index;    /* -1 = truecolor/unset                  */
    uint8_t r, g, b;
} tcolor;

#define TCOLOR_UNSET ((tcolor){ .index = -1, .r = 0, .g = 0, .b = 0 })

/* The named schemes shipped (per-profile, prefs key "scheme"):
 * default (theme-following), plus the classics every terminal
 * offers. "solarized-dark"/"solarized-light" (Ethan Schoonover's
 * palette, widely reproduced), "gruvbox-dark" (Pavel Pertsev's
 * palette), and a high-contrast black/white. */
typedef enum {
    TSCHEME_FOLLOW = 0,  /* the FDK theme decides the 16 slots   */
    TSCHEME_SOLARIZED_DARK,
    TSCHEME_SOLARIZED_LIGHT,
    TSCHEME_GRUVBOX_DARK,
    TSCHEME_CONTRAST,
    TSCHEME_COUNT
} tscheme;

const char *tscheme_name(tscheme s);       /* stable prefs string */
bool tscheme_parse(const char *name, tscheme *out);

/* Resolves `c` (a foreground or background reference) against the
 * active scheme into concrete RGB. `is_bg` picks the theme-derived
 * default when the reference is unset. */
fdk_color tcolor_resolve(tcolor c, bool is_bg);

/* Index 0..255 -> the concrete color (the 16 slots scheme-aware,
 * 16..231 the 6x6x6 cube, 232..255 the grayscale ramp). */
fdk_color tcolor_index(int index, bool is_bg);

/* The scheme's default foreground/background. */
fdk_color tcolor_default_fg(void);
fdk_color tcolor_default_bg(void);

/* Re-reads the theme when scheme == FOLLOW (call on theme change). */
void tcolor_theme_changed(void);

/* Selects the active scheme (TSCHEME_FOLLOW reads the FDK theme). */
void tcolor_set_scheme(tscheme s);

#endif /* FDKTERM_COLORS_H */
