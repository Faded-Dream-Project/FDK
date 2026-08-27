/*
 * fdk_text.h — Faded Dream ToolKit: fonts and text rendering
 *
 * Phase 6 text foundation. A font is a TrueType/OpenType file loaded
 * from disk at a fixed pixel size; text is UTF-8, shaped left-to-right
 * with kerning, rasterized anti-aliased by the vendored stb_truetype
 * (see third_party/stb/README.md), cached per glyph, and blended onto
 * any fdk_surface through the renderer's clip stack and damage
 * tracking.
 *
 * Design notes:
 *
 *  - Fonts are standalone objects: no fdk_context, no window. Like
 *    offscreen surfaces and standalone widget trees, they exist so
 *    applications (and the test suite) can measure and render text
 *    headlessly.
 *
 *  - One font object per (file, pixel size). Ask for 12 px and 16 px
 *    of the same face and you own two fdk_font objects, each with its
 *    own glyph cache. This keeps the shaping math (scale baked in at
 *    load) trivial and the cache hot.
 *
 *  - Layout is single-line and left-to-right. Line breaking, bidi,
 *    and complex shaping (ligatures, Arabic joining, Indic reordering)
 *    are out of scope for v1 and tracked on the roadmap; what v1 does
 *    promise is: Unicode codepoint coverage via the font's cmap,
 *    pair kerning, and metrics that agree exactly with what
 *    fdk_surface_draw_utf8() paints.
 *
 *  - Missing glyphs (codepoints absent from the font) render as the
 *    font's own .notdef glyph — usually a hollow box — and measure at
 *    that glyph's advance. Invalid UTF-8 decodes to U+FFFD
 *    (REPLACEMENT CHARACTER), one bad byte at a time, never reading
 *    past byte_len.
 *
 *  - Text rendering is fully software and backend-neutral: it draws
 *    into the surface's framebuffer, so it works identically on
 *    window surfaces (both backends) and offscreen surfaces.
 *
 * Ownership and lifetime:
 *
 *  - fdk_font_load() returns a font owned by the caller. Destroy it
 *    with fdk_font_destroy(). Glyph pixels are copied out at draw
 *    time; nothing drawn on a surface keeps a reference to the font,
 *    so fonts may be destroyed in any order relative to surfaces.
 *
 *  - Fonts are not thread-safe. As with the rest of FDK's object
 *    model, use a font from one thread at a time.
 */

#ifndef FDK_TEXT_H
#define FDK_TEXT_H

#include "fdk_types.h"
#include "fdk_error.h"
#include "fdk_surface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Font object ---- */

/* Opaque font handle: one TrueType/OpenType face at one pixel size. */
typedef struct fdk_font fdk_font;

/* Loads a font file (TrueType `.ttf` / OpenType `.ttf`-flavored) at
 * `pixel_size` pixels of em height.
 *
 * The whole file is read into memory up front (the parser needs the
 * complete byte stream). For TrueType Collection (`.ttc`) files the
 * FIRST face is used; selecting other faces is not supported yet.
 *
 * pixel_size must be in [1, 512]. Sizes above ~200 px will produce
 * very large glyph bitmaps in the cache; that is allowed, just be
 * aware of the memory cost.
 *
 * Returns the font, or NULL on failure:
 *   FDK_ERR_NOT_FOUND       — file missing / unreadable
 *   FDK_ERR_INVALID_ARGUMENT— NULL path or out-of-range size
 *   FDK_ERR_OUT_OF_MEMORY   — allocation failed
 *   FDK_ERR_FONT_LOAD       — file is not a usable font (stbtt InitFont
 *                             rejected it: bad magic/tables)
 */
fdk_font *fdk_font_load(const char *path, fdk_i32 pixel_size);

/* Destroys a font and its glyph cache. NULL is a safe no-op. */
void fdk_font_destroy(fdk_font *font);

/* Vertical metrics in pixels, from the font's hhea table, scaled to
 * the font's pixel size. ascent is the distance above the baseline,
 * descent the distance below it (reported as a positive number),
 * line_gap the recommended gap between lines; line_height is the
 * convenient sum of the three. */
typedef struct fdk_font_metrics {
    fdk_i32 ascent;
    fdk_i32 descent;     /* positive, distance BELOW the baseline */
    fdk_i32 line_gap;
    fdk_i32 line_height; /* ascent + descent + line_gap */
} fdk_font_metrics;

void fdk_font_get_metrics(const fdk_font *font, fdk_font_metrics *out);

/* ---- Measurement ---- */

/* Horizontal metrics for a single line of UTF-8 text, in pixels.
 *
 * advance_width is the rounded total pen advance: where the pen
 * lands after the whole run, kerning included. ink_top/ink_bottom
 * bound the union of the glyphs' outline boxes: ink_top is the
 * offset from the baseline to the topmost ink (negative = above the
 * baseline), ink_bottom to the lowest ink (positive = below the
 * baseline, e.g. descenders of g/j/p). Runs whose glyphs have no
 * outlines at all (e.g. pure spaces) report ink_top = ink_bottom = 0.
 *
 * These are the numbers fdk_surface_draw_utf8() paints with, so a
 * caller can position and size text runs without a draw pass.
 */
typedef struct fdk_text_metrics {
    fdk_i32 advance_width;
    fdk_i32 ink_top;    /* <= 0: topmost ink above the baseline */
    fdk_i32 ink_bottom; /* >= 0: lowest ink below the baseline */
} fdk_text_metrics;

/* Measures up to byte_len bytes of utf8 (the string need not be
 * NUL-terminated). Empty text measures {0, 0, 0}. */
fdk_result fdk_font_measure_utf8(const fdk_font *font,
                                 const char *utf8, size_t byte_len,
                                 fdk_text_metrics *out);

/* ---- Drawing ---- */

/* Draws one line of UTF-8 text onto `surface`.
 *
 * (pen_x, baseline_y) is the pen start: the left edge of the first
 * glyph's advance, on the text baseline. Color may be translucent —
 * glyphs are alpha-blended source-over, honoring the surface's clip
 * stack exactly like every other primitive.
 *
 * Each glyph's ink box that intersects the clip adds to the surface's
 * damage; the whole run is merged into a single damage rectangle (the
 * union of the run's ink boxes), so painting text costs one damage
 * rect regardless of length. A run with no ink (pure whitespace)
 * touches no pixels and adds no damage.
 *
 * Glyph bitmaps are cached per font (see fdk_font_get_cache_stats):
 * the first draw of a glyph rasterizes it; later draws reuse it.
 *
 * Returns FDK_ERR_INVALID_ARGUMENT for NULL surface/font/utf8, else
 * FDK_OK (text that falls entirely outside the clip is a successful
 * no-op).
 */
fdk_result fdk_surface_draw_utf8(fdk_surface *surface, fdk_font *font,
                                 const char *utf8, size_t byte_len,
                                 fdk_i32 pen_x, fdk_i32 baseline_y,
                                 fdk_color color);

/* ---- Glyph cache introspection ---- */

/* Glyph-cache counters since font load. cached_glyphs is the current
 * number of rasterized glyphs held (bounded; least-recently-used
 * glyphs are evicted past the bound, which is currently 512 — large
 * enough for entire alphabets plus punctuation to stay resident). */
typedef struct fdk_font_cache_stats {
    fdk_i32 cached_glyphs;
    fdk_i32 cache_hits;
    fdk_i32 cache_misses; /* rasterizations performed */
    fdk_i32 evictions;
} fdk_font_cache_stats;

void fdk_font_get_cache_stats(const fdk_font *font,
                              fdk_font_cache_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* FDK_TEXT_H */
