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

/* Glyph cache capacity. Subpixel positioning (below) keys each
 * (glyph, phase) pair separately, so four entries exist per distinct
 * glyph: 2048 keeps the same real-glyph coverage 512 gave at integer
 * positioning (a full Latin alphabet, digits, punctuation, and then
 * some permanently resident); runs that touch more distinct
 * (glyph,phase) pairs evict least-recently-used entries. */
#define FDK_TEXT_GLYPH_CACHE_MAX 2048

/* Subpixel positioning phases per axis (x only — y stays integer;
 * text lines sit on the baseline). Four phases (0, 1/4, 1/2, 3/4)
 * is the classic grayscale-subpixel granularity: the error is at
 * most 1/8 px, invisible next to the 1 px error integer positioning
 * makes. */
#define FDK_TEXT_SUBPIXEL_PHASES 4

/* A rasterized glyph, cached. `xoff`/`yoff` are stb's bitmap-box
 * offsets: where the bitmap's top-left sits relative to the pen
 * position (xoff) and the baseline (yoff; <= 0 means above it).
 * `advance` is the scaled horizontal advance in pixels (float —
 * rounding happens once per glyph placement, never inside the
 * accumulation). `bits` is the w*h alpha bitmap (row-major, 1 byte
 * per pixel, stride == w); NULL when the glyph has no ink (space,
 * combining marks with empty bitmaps) — such glyphs still advance. */
typedef struct fdk_glyph {
    int key;              /* cache key: glyph_index * PHASES + phase */
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
    unsigned style;             /* fdk_font_style flags (synthetic) */
    char *source_path;          /* fdk_alloc'd copy of the load path
                                   (fdk_font_get_file_path); NULL is
                                   non-fatal — see text.c */

    fdk_font_metrics metrics;

    /* Glyph cache: fixed-capacity open-addressing-free linear table
     * with LRU eviction. Lookup is a linear scan over live entries —
     * fine at 2048 entries for a software renderer (rasterization and
     * blending dominate); promoted to a hash when profiling says so.
     * Entries are keyed (glyph, subpixel phase); the synthetic style
     * is baked in at raster time and a style change flushes the
     * whole cache (fdk_text_flush_cache). */
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

/* The ellipsis run shared by the ellipsize pass (src/text/layout.c)
 * and the Label paint hook (src/widget/statics.c): U+2026, HORIZONTAL
 * ELLIPSIS. One definition so the measured and the drawn character
 * can never drift apart. */
#define FDK_TEXT_ELLIPSIS_UTF8 "\xE2\x80\xA6"
#define FDK_TEXT_ELLIPSIS_BYTES 3

/* One step of the shared left-to-right shaping walk (see text.c).
 * Consumes the next codepoint at *io_i (never past len), applies pair
 * kerning against *io_prev_g (pass -1 to start), advances the float
 * pen, and reports the glyph plus the FLOOR pen position its left
 * edge is drawn at (the subpixel phase is baked into the returned
 * glyph's rasterization — the caller never sees it). Returns 0 at
 * end of run, 1 on progress. The measure walk, the draw walk, AND
 * the line/ellipsis layout pass (src/text/layout.c) share this —
 * every width FDK ever reports or paints comes from the same
 * arithmetic. */
int fdk_text_shape_step(fdk_font *font, const char *utf8, size_t len,
                        size_t *io_i, int *io_prev_g, fdk_f32 *io_pen,
                        const fdk_glyph **out_glyph, fdk_i32 *out_pen_x);

/* Drops every cached (glyph, phase) rasterization — used when the
 * font's synthetic style changes (the style is baked in at raster
 * time; see fdk_font_set_style). The next walk re-rasterizes on
 * demand. */
void fdk_text_flush_cache(fdk_font *font);

/* The text layer's SINGLE const-laundering point (measure warms the
 * glyph cache, so const public signatures need mutable state; see
 * text.c). The layout pass uses it too — no other const-casting
 * exists in the text layer. */
fdk_font *fdk_text_font_mutable(const fdk_font *font);

/* ---- System font discovery (src/text/fontscan.c) ---- */

/* Resolves the system default UI font through the documented
 * priority chain: $FDK_FONT_FILE, $FDK_FONT_DIRS scan, fontconfig
 * (dlopen'd at run time), the known-path list, then a ranked scan of
 * the standard font directories. On success *out_path points at a
 * cached string (do not free) and *out_face receives the collection
 * face index fontconfig chose (0 otherwise). The cache is valid until
 * the next resolution — normally the process lifetime, but a failed
 * full load rejects the winner and forces a re-probe (see
 * fdk_text_font_discovery_reject), so copy the string if it must
 * outlive the call. Returns false when no usable font exists (already
 * logged with an actionable message). */
bool fdk_text_resolve_system_font(const char **out_path,
                                  long *out_face);

/* Loader shared by fdk_font_load() (face 0) and the system-default
 * resolver (fontconfig's FC_INDEX): `face_index` selects a face
 * inside a TrueType Collection. */
fdk_font *fdk_text_font_load_face(const char *path, long face_index,
                                  fdk_i32 pixel_size);

/* Clears the cached system-font resolution so the next
 * fdk_text_resolve_system_font() call re-probes. TEST SUITE ONLY —
 * exists because fontconfig state (and the environment) can change
 * between scenarios within one test process. Not part of the public
 * API; never declared in an installed header. */
void fdk_text_font_discovery_reset_for_tests(void);

/* Records that the candidate at `path` failed the loader's full
 * container validation and forces re-resolution, so the next-best
 * candidate wins instead of the discovery result poisoning every
 * subsequent call. Used by fdk_font_load_system_default()'s retry
 * loop. */
void fdk_text_font_discovery_reject(const char *path);

#endif /* FDK_TEXT_INTERNAL_H */
