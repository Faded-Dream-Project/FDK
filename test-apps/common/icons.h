/* icons.h — the test-apps icon loader.
 *
 * The icon set (test-apps/icons/, original artwork) is line-stroke
 * SVG colored with `currentColor`. FDK's SVG rasterizer
 * (fdk_surface_create_from_image via nanosvg) does not substitute
 * `currentColor` — it renders literally — so the loader rewrites the
 * token to the CURRENT THEME'S TEXT INK before handing the bytes to
 * the rasterizer, and CACHES the result per (icon, size, ink) so the
 * rewrite+rasterize cost is paid once.
 *
 * Fallback ladder (the honest chain every app here follows):
 *   1. our bundled SVG, tinted with the theme text color
 *   2. the desktop's icon theme (fdk_icon_theme_surface) — a real
 *      Adwaita/breeze installation beats nothing
 *   3. FDK's built-in vector row glyphs (fdk_row_icon) — the floor
 *      that can never fail
 *
 * THEME CHANGES: the cache keys on the ink color, so a live re-theme
 * (light -> dark) simply misses the cache and re-rasterizes. Apps
 * call icons_theme_changed() from their theme-follow code path to
 * repaint whatever they drew icons into.
 *
 * This is deliberately NOT a framework: one loader, one fallback, no
 * widget classes.
 */
#ifndef TESTAPPS_ICONS_H
#define TESTAPPS_ICONS_H

#include "fdk/fdk_surface.h"
#include "fdk/fdk_theme.h"

#include <stddef.h>

/* The icon names available in test-apps/icons/ (NUL-separated in the
 * returned buffer; valid for the process lifetime). */
const char *icons_available(void);

/* Loads `name` (e.g. "back") at `size` px, tinted with the CURRENT
 * theme's text ink. Tries the bundled SVG, then the desktop icon
 * theme, then NULL (callers fall back to fdk_row_icon glyphs).
 * The result is OWNED BY THE CACHE — do not destroy it; it stays
 * valid until icons_shutdown(). Rasterizes at size * window scale
 * when told, for HiDPI crispness. */
fdk_surface *icons_get(const char *name, fdk_i32 size, fdk_f32 scale);

/* Drops every cached raster (a live re-theme: the next icons_get
 * re-tints with the new ink). */
void icons_theme_changed(void);

/* Releases the whole cache. */
void icons_shutdown(void);

#endif /* TESTAPPS_ICONS_H */
