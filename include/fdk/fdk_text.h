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

/* ---- Synthetic styles (Phase 6 completion) ----
 *
 * SYNTHESIS, not face selection: these flags widen (bold) or slant
 * (italic) whatever face the font object loaded. When a real bold or
 * italic face FILE exists, loading it with fdk_font_load() is the
 * better choice — a designed face beats a synthesized one. The flags
 * exist for the (common) case where only the regular file is
 * available.
 *
 *   BOLD   — stem dilation: strokes widen by pixel_size/24 px (min
 *            1), and the advance grows by the same amount, so text
 *            measures as wide as it paints.
 *   ITALIC — oblique shear: ink slants ~12 degrees, anchored at the
 *            baseline (ascenders lean right, descenders left). The
 *            advance is deliberately unchanged (CSS font-synthesis
 *            semantics): the slanted ink may overhang the next
 *            glyph's slot, which the damage model already covers.
 *
 * Changing the style flushes the glyph cache (rasterizations bake
 * the style in); the next measure/draw re-rasterizes on demand.
 * Measurement always reflects the current style — a bold run
 * measures wider than the same run regular. */

typedef enum fdk_font_style {
    FDK_FONT_STYLE_NORMAL = 0,
    FDK_FONT_STYLE_BOLD = 1u << 0,
    FDK_FONT_STYLE_ITALIC = 1u << 1,
} fdk_font_style;

/* Sets/clears style flags (OR of fdk_font_style values; unknown bits
 * are ignored). FDK_ERR_INVALID_ARGUMENT only for a NULL font. */
fdk_result fdk_font_set_style(fdk_font *font, unsigned style_flags);

/* The font's current style flags (0 = normal). NULL reads as 0. */
unsigned fdk_font_get_style(const fdk_font *font);

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

/* Ascent/descent/line-gap in pixels (see the struct above). */
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
 * number of rasterized entries held (bounded; least-recently-used
 * entries are evicted past the bound, which is currently 2048 —
 * entries are keyed (glyph, subpixel phase), so this is 512 distinct
 * glyphs' worth of coverage; large enough for entire alphabets plus
 * punctuation to stay resident). */
typedef struct fdk_font_cache_stats {
    fdk_i32 cached_glyphs;
    fdk_i32 cache_hits;
    fdk_i32 cache_misses; /* rasterizations performed */
    fdk_i32 evictions;
} fdk_font_cache_stats;

/* Glyph-cache counters (diagnostics). */
void fdk_font_get_cache_stats(const fdk_font *font,
                              fdk_font_cache_stats *out);

/* ---- Line layout: wrapping and ellipsis ---- */

/* One output line of fdk_font_break_lines_utf8().
 *
 * byte_offset/byte_len select the line's bytes within the SOURCE
 * string (the line never splits a codepoint); trailing spaces are
 * trimmed, so byte_len may end before the whitespace that caused the
 * break. advance_width is the line's rounded pen advance (kerning
 * within the line included) — exactly what fdk_surface_draw_utf8()
 * would paint for those bytes, so callers can align or center lines
 * without a draw pass. A line of pure whitespace reports
 * advance_width 0 (and byte_len 0); such lines exist only where the
 * source had an explicit newline. */
typedef struct fdk_text_line {
    size_t byte_offset;
    size_t byte_len;
    fdk_i32 advance_width;
} fdk_text_line;

/* Greedy word-wrap: breaks utf8[0..byte_len) into lines that each fit
 * max_width pixels.
 *
 * Rules (v1, left-to-right scripts only):
 *   - Lines break after space runs (the space stays out of the next
 *     line). A word longer than max_width breaks at glyph boundaries
 *     rather than overflowing.
 *   - '\n' is a hard line break; "\r\n" and a lone '\r' count as one.
 *     Lines they create may be empty (byte_len 0).
 *   - The FIRST line keeps the source's leading spaces; later lines
 *     start at their first non-space byte. A trailing all-space run
 *     produces no extra line.
 *   - Each line is an independent run: kerning never crosses lines.
 *
 * out_lines receives at most max_lines entries (in text order). Pass
 * max_lines = 0 with out_lines = NULL to COUNT first: *out_line_count
 * then holds the total line count and nothing is stored. When the
 * text needs more lines than max_lines, only the first max_lines are
 * stored and *out_truncated is true (remaining visible text exists);
 * with max_lines = 0, *out_truncated is always false (nothing was
 * dropped — the count is complete).
 *
 * Returns FDK_ERR_INVALID_ARGUMENT for NULL font/utf8/out_line_count,
 * out_lines == NULL with max_lines > 0, or max_width < 1. Empty text
 * yields 0 lines and FDK_OK. out_truncated may be NULL.
 */
fdk_result fdk_font_break_lines_utf8(const fdk_font *font,
                                     const char *utf8, size_t byte_len,
                                     fdk_i32 max_width,
                                     fdk_text_line *out_lines,
                                     size_t max_lines,
                                     size_t *out_line_count,
                                     bool *out_truncated);

/* Fits utf8[0..byte_len) into max_width pixels, truncating with an
 * ellipsis character (U+2026, HORIZONTAL ELLIPSIS) when it does not
 * fit.
 *
 * When the whole text fits: *out_prefix_bytes = byte_len and
 * *out_fits = true — draw the text as-is. Otherwise *out_fits is
 * false and *out_prefix_bytes is the longest byte prefix (always a
 * codepoint boundary, trailing spaces trimmed) such that
 * round(prefix advance) + (ellipsis advance) <= max_width. The caller
 * draws text[0..prefix) and then the ellipsis at
 * x + round(prefix advance). Kerning between the prefix's last glyph
 * and the ellipsis is deliberately not applied (the two runs are
 * independent); for most faces it is zero.
 *
 * Degenerate widths: max_width == 0 (or below the ellipsis alone)
 * yields prefix 0 — the caller draws just the ellipsis and lets the
 * clip stack hide the overflow. max_width < 0 is an argument error.
 *
 * Returns FDK_ERR_INVALID_ARGUMENT for NULL font/utf8/
 * out_prefix_bytes or negative max_width. out_fits may be NULL.
 */
fdk_result fdk_font_ellipsize_utf8(const fdk_font *font,
                                   const char *utf8, size_t byte_len,
                                   fdk_i32 max_width,
                                   size_t *out_prefix_bytes,
                                   bool *out_fits);

/* Loads a default UI font at `pixel_size` by probing the locations
 * FDK's own examples and test suite probe (DejaVu Sans, Noto Sans,
 * Liberation Sans, FreeSans — the common Linux desktop faces). The
 * first readable file wins; the found path is cached, so repeated
 * calls are cheap after the first hit. Returns NULL with an error
 * log when no candidate exists — FDK deliberately bundles NO font
 * (licensing posture, see docs/dependencies.md), so a stripped
 * container has no default; applications that must render text
 * should ship or require a font and load it by path.
 *
 * The window-decoration layer (fdk_window_set_decorated) uses this
 * for its title text; applications may prefer their own face via
 * fdk_font_load() — see fdk_window_set_decoration_font(). */
fdk_font *fdk_font_load_system_default(fdk_i32 pixel_size);

#ifdef __cplusplus
}
#endif

#endif /* FDK_TEXT_H */
