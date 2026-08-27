/*
 * text_internal.h — internal definition of struct fdk_font and the
 * glyph cache.
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_text.h.
 */

#ifndef FDK_TEXT_INTERNAL_H
#define FDK_TEXT_INTERNAL_H

#include "fdk/fdk_text.h"

#include "stb_truetype.h"

/* Glyph cache capacity. 512 keeps a full Latin alphabet (upper+lower),
 * digits, punctuation, and then some permanently resident; runs that
 * touch more distinct glyphs evict least-recently-used entries. */
#define FDK_TEXT_GLYPH_CACHE_MAX 512

/* A rasterized glyph, cached. `xoff`/`yoff` are stb's bitmap-box
 * offsets: where the bitmap's top-left sits relative to the pen
 * position (xoff) and the baseline (yoff; <= 0 means above it).
 * `advance` is the scaled horizontal advance in pixels (float —
 * rounding happens once per glyph placement, never inside the
 * accumulation). `bits` is the w*h alpha bitmap (row-major, 1 byte
 * per pixel, stride == w); NULL when the glyph has no ink (space,
 * combining marks with empty bitmaps) — such glyphs still advance. */
typedef struct fdk_glyph {
    int glyph_index;      /* stb glyph id (cache key) */
    fdk_i32 w, h;
    fdk_i32 xoff, yoff;
    fdk_f32 advance;
    fdk_u8 *bits;         /* fdk_alloc'd; NULL iff w*h == 0 */
    fdk_u64 last_used;    /* LRU clock; bumped on every hit */
} fdk_glyph;

struct fdk_font {
    unsigned char *file_data;   /* whole font file, fdk_alloc'd */
    stbtt_fontinfo info;
    fdk_f32 scale;              /* font units -> pixels (pixel size) */
    fdk_i32 pixel_size;

    fdk_font_metrics metrics;

    /* Glyph cache: fixed-capacity open-addressing-free linear table
     * with LRU eviction. Lookup is a linear scan over live entries —
     * fine at 512 entries for a software renderer (rasterization and
     * blending dominate); promoted to a hash when profiling says so. */
    fdk_glyph glyphs[FDK_TEXT_GLYPH_CACHE_MAX];
    int glyph_count;            /* live entries in [0, glyph_count) */
    fdk_u64 clock;              /* monotonically bumped per use */

    fdk_font_cache_stats stats;
};

/* Decodes the codepoint at s[i] (i < len). Writes the codepoint to
 * *out_cp and returns the number of bytes consumed (>= 1) — invalid
 * sequences yield U+FFFD and consume exactly one byte, never reading
 * past len. `glyph_index_for()` then maps the codepoint (possibly 0 =
 * .notdef for unmapped codepoints). */
int fdk_text_utf8_next(const char *s, size_t len, size_t i,
                       fdk_u32 *out_cp);

/* Cache lookup with rasterize-on-miss. ALWAYS returns a valid glyph
 * entry (never NULL): unmapped codepoints resolve to glyph 0
 * (.notdef). The entry's metrics (advance/xoff/yoff/w/h) are always
 * populated; entry->bits is NULL exactly when the glyph has no ink
 * (space, empty bitmaps) — such glyphs still advance the pen. */
const fdk_glyph *fdk_text_glyph_for(fdk_font *font, fdk_u32 codepoint);

#endif /* FDK_TEXT_INTERNAL_H */
