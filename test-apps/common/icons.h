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
 * Fallback ladder (§31/PART-2: THE SYSTEM THEME FIRST — S3's root
 * cause was the old bundled-first order; the owner's machine has
 * real themes and the app ignored them):
 *   1. the desktop's icon theme (fdk_icon_theme_surface) — the
 *      freedesktop name resolved per the Icon Theme Specification
 *      (theme -> Inherits -> hicolor -> pixmaps), preferring the
 *      -symbolic variant where one exists (it comes back already
 *      tinted with the theme ink)
 *   2. our bundled SVG, tinted with the theme text color (EMBEDDED
 *      in the binary — no path, no cwd, no install prefix)
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

/* Loads `name` at `size` px. `name` is the FREDESKTOP STANDARD name
 * (go-previous, folder-new, user-trash, ...); the bundled set's
 * legacy short names ("back", "folder") are mapped to them. Tries
 * the SYSTEM THEME first (symbolic variant preferred), then the
 * bundled SVG tinted with the theme ink, then NULL (callers fall
 * back to fdk_row_icon glyphs). The tier that answered is available
 * through icons_last_tier() for --diag.
 * The result is OWNED BY THE CACHE — do not destroy it; it stays
 * valid until icons_shutdown(). Rasterizes at size * window scale
 * when told, for HiDPI crispness. */
fdk_surface *icons_get(const char *name, fdk_i32 size, fdk_f32 scale);

/* §31/owner-round-5: the FILE-ENTRY twin — a themed icon for a
 * filesystem entry (extension/type classification, folder chains),
 * cached by TYPE (directory / extension) so a 10k-row directory
 * costs a handful of resolutions, not 10k. Same ownership rule:
 * the cache owns the surface for the process lifetime; rows and
 * iconview items BORROW it — never destroy, never free on view
 * rebuilds. NULL = the theme serves nothing for that type; the
 * caller keeps its glyph floor. */
fdk_surface *icons_get_file(const char *dir, const char *name,
                            bool is_dir, fdk_i32 size);

/* §31/owner-round-3: the tier verdict for the shutdown log line —
 * how many lookups, how many were served by the system theme. */
long icons_system_hits(void);
long icons_lookups(void);

/* Which tier answered the LAST icons_get on this thread — the --diag
 * contract. Strings are static; NONE means "the caller's glyph". */
typedef enum icons_tier {
    ICONS_TIER_NONE = 0,
    ICONS_TIER_SYSTEM,   /* the desktop icon theme (path resolved) */
    ICONS_TIER_BUNDLED,  /* the embedded SVG, tinted              */
    ICONS_TIER_FALLBACK, /* NULL: the fdk_row_icon glyph floor     */
} icons_tier;

/* The tier of the last icons_get(name); also copies the resolved
 * system-theme FILE PATH when that tier answered (else ""). */
icons_tier icons_last_tier(char *out_path, size_t path_cap);

/* Drops every cached raster (a live re-theme: the next icons_get
 * re-tints with the new ink). */
void icons_theme_changed(void);

/* Releases the whole cache. */
void icons_shutdown(void);

#endif /* TESTAPPS_ICONS_H */
