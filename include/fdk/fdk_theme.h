/*
 * fdk_theme.h — Faded Dream ToolKit: themes
 *
 * Phase 7 theme engine. A theme is the toolkit's central palette and
 * metric set: the named colors the widget catalog paints with (text,
 * control surfaces and their hover/pressed/disabled states, accent,
 * track, border) plus the handful of paint-time metrics (button corner
 * radius, separator thickness). The built-in default theme is the
 * Phase 6 "v1" palette exactly — switching to it changes no pixels.
 *
 * Design notes:
 *
 *  - Themes are standalone objects like fonts: no context, no window.
 *    Build one programmatically (fdk_theme_create_default +
 *    fdk_theme_set_color), parse one from memory, or load one from a
 *    `.fdk` file. The file format is specified in
 *    docs/fdk-theme-format.md and its parser is written to the
 *    security rules in docs/security.md (strict, bounded, no partial
 *    results).
 *
 *  - There is ONE current default theme per process. The widget
 *    catalog resolves colors through it at paint time — nothing is
 *    cached on widgets — so switching themes is:
 *
 *        fdk_theme_set_default(other);   // repaints every live tree
 *
 *    which invalidates every live widget tree (window-owned and
 *    standalone). Widgets that draw their own explicit colors (a
 *    Label with a set color, a plain widget's background fill) keep
 *    those colors; theming changes what widgets paint when they ask
 *    the theme for a token. The example 08 demo shows the documented
 *    re-theme pattern for app-owned styling.
 *
 *  - Partial themes are a feature: a `.fdk` file that overrides three
 *    colors inherits the built-in defaults for the rest. The parser
 *    rejects unknown keys and sections (typos must fail loudly) but
 *    never requires completeness.
 *
 *  - FDK does not force the theme onto anything that didn't ask for
 *    it: existing windows keep their X11/Wayland backend background,
 *    plain widgets keep their per-widget background style, and the
 *    `window_background` token is a recommendation applications opt
 *    into.
 *
 * Ownership and lifetime:
 *
 *  - fdk_theme_create_default/parse/load return a theme owned by the
 *    caller. Destroy with fdk_theme_destroy() — destroying the theme
 *    that is currently the default first reverts the default to the
 *    built-in theme (and repaints), so a dangling current theme is
 *    impossible.
 *
 *  - fdk_theme_set_default() BORROWS: the caller keeps ownership and
 *    may destroy the theme at any time (see above). NULL reverts to
 *    the built-in theme. Setting a theme that is already current is a
 *    no-op (no repaint storm).
 *
 *  - Themes are not thread-safe; as with the rest of FDK's object
 *    model, use them from one thread at a time.
 */

#ifndef FDK_THEME_H
#define FDK_THEME_H

#include "fdk_types.h"
#include "fdk_error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Theme object ---- */

/* Opaque theme handle: a palette + metric set with a name. */
typedef struct fdk_theme fdk_theme;

/* Color tokens. The set is exactly what the widget catalog consumes
 * today plus the window background recommendation; tokens grow as
 * widgets do (new tokens are new enum values at the end — ABI policy
 * keeps this enum append-only, never renumbered). */
typedef enum fdk_theme_token {
    FDK_TK_WINDOW_BACKGROUND         = 0, /* app surface (opt-in)    */
    FDK_TK_TEXT                      = 1, /* primary text            */
    FDK_TK_TEXT_DISABLED             = 2, /* text on disabled widgets*/
    FDK_TK_CONTROL_BACKGROUND        = 3, /* control resting fill    */
    FDK_TK_CONTROL_BACKGROUND_HOVER  = 4, /* control under pointer   */
    FDK_TK_CONTROL_BACKGROUND_PRESSED= 5, /* control while pressed   */
    FDK_TK_CONTROL_BACKGROUND_DISABLED=6, /* control when disabled   */
    FDK_TK_CONTROL_BORDER            = 7, /* separators, rules       */
    FDK_TK_ACCENT                    = 8, /* checked, progress, focus*/
    FDK_TK_TRACK                     = 9, /* progress/toggle track   */

    FDK_TK_COUNT = 10
} fdk_theme_token;

/* Integer paint metrics. Most are paint-time values only (they never
 * change a widget's natural size); the exceptions are layout metrics —
 * currently FDK_TM_TITLE_BAR_HEIGHT, which sizes the FDK-drawn title
 * band (a theme switch re-arranges decorated windows; see
 * fdk_window_set_decorated). */
typedef enum fdk_theme_metric {
    FDK_TM_BUTTON_CORNER_RADIUS = 0, /* Button fill/focus-ring corners, 0..32 */
    FDK_TM_SEPARATOR_THICKNESS = 1, /* Separator band thickness,    1..8  */
    FDK_TM_TITLE_BAR_HEIGHT    = 2, /* FDK-drawn title band height, 12..64 */
    FDK_TM_SCROLLBAR_WIDTH     = 3, /* ScrollView bar thickness,    6..24  */

    FDK_TM_COUNT = 4
} fdk_theme_metric;

/* ---- Lifecycle ---- */

/* A fresh copy of the built-in default theme (the Phase 6 v1 palette
 * and metrics). Modify it with the setters below and install it with
 * fdk_theme_set_default(). Returns NULL only on allocation failure. */
fdk_theme *fdk_theme_create_default(void);

/* Parses a theme from memory. `text` need not be NUL-terminated —
 * exactly `length` bytes are read. On failure returns NULL and, when
 * `out_error` is non-NULL, writes one of FDK_ERR_THEME_PARSE,
 * FDK_ERR_THEME_VERSION, FDK_ERR_OUT_OF_MEMORY, or
 * FDK_ERR_INVALID_ARGUMENT (NULL text / length 0 / length above the
 * 1 MiB input cap). On success writes FDK_OK. See
 * docs/fdk-theme-format.md for the grammar. */
fdk_theme *fdk_theme_parse(const char *text, size_t length,
                           fdk_result *out_error);

/* Loads and parses a `.fdk` theme file. Same error contract as
 * fdk_theme_parse(), plus FDK_ERR_THEME_IO when the file cannot be
 * opened or read, or is larger than the 1 MiB cap. */
fdk_theme *fdk_theme_load(const char *path, fdk_result *out_error);

/* Destroys a theme. If it is the current default, the default first
 * reverts to the built-in theme (repainting every live widget tree),
 * so the current default can never dangle. NULL is a no-op. */
void fdk_theme_destroy(fdk_theme *theme);

/* ---- Access ---- */

/* The theme's display name ("FDK Dark" for the built-in theme; the
 * `name` key for parsed themes). Never NULL; valid until the theme is
 * destroyed or renamed. A NULL theme means the current default. */
const char *fdk_theme_name(const fdk_theme *theme);

/* The theme's author string, or NULL when unset. A NULL theme means
 * the current default. */
const char *fdk_theme_author(const fdk_theme *theme);

/* A token's color. A NULL theme means the current default. An out-of-
 * range token logs a warning and returns opaque black. */
fdk_color fdk_theme_get_color(const fdk_theme *theme,
                               fdk_theme_token token);

/* A metric's value. A NULL theme means the current default. An
 * out-of-range metric logs a warning and returns 0. */
fdk_i32 fdk_theme_get_metric(const fdk_theme *theme,
                            fdk_theme_metric metric);

/* ---- Programmatic modification ---- */

/* Overrides a token's color on an owned theme. Does NOT repaint
 * anything: install the result with fdk_theme_set_default(), or
 * invalidate manually, when you want the change on screen. Returns
 * FDK_ERR_INVALID_ARGUMENT for a NULL theme or out-of-range token. */
fdk_result fdk_theme_set_color(fdk_theme *theme, fdk_theme_token token,
                               fdk_color color);

/* Overrides a metric on an owned theme, range-checked per metric
 * (button_corner_radius 0..32, separator_thickness 1..8). Same
 * no-repaint contract as fdk_theme_set_color(). */
fdk_result fdk_theme_set_metric(fdk_theme *theme, fdk_theme_metric metric,
                                fdk_i32 value);

/* Renames a theme (copied; capped at 127 bytes, longer names are an
 * FDK_ERR_INVALID_ARGUMENT). NULL name restores "FDK Dark". */
fdk_result fdk_theme_set_name(fdk_theme *theme, const char *name);

/* ---- The current default theme ---- */

/* Makes `theme` the current default theme — the one the widget
 * catalog paints with — and invalidates every live widget tree
 * (window-owned and standalone), so the next paint of each shows the
 * new colors. NULL reverts to the built-in theme. The theme is
 * borrowed; setting the already-current theme is a no-op. */
void fdk_theme_set_default(fdk_theme *theme);

/* The current default theme. Never NULL. The built-in theme when no
 * other has been set (or the last one set was destroyed/NULLed);
 * otherwise a borrowed pointer to whatever fdk_theme_set_default()
 * installed. */
fdk_theme *fdk_theme_get_default(void);

#ifdef __cplusplus
}
#endif

#endif /* FDK_THEME_H */
