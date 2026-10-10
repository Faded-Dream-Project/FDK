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

/* Style VARIANTS the cache keys apart (1.4.11): normal / bold /
 * italic / bold-italic. Rich-text runs flip styles per run; baking
 * the style into the KEY (instead of into the font object alone)
 * lets a bold word and the regular text around it share one font
 * object with BOTH rasterizations resident — no cache thrash, no
 * style-flip re-rasterization when a label repaints mixed runs.
 * fdk_font_set_style() still flushes (its documented contract), but
 * per-run styled lookups are variant-keyed and stable. */
#define FDK_TEXT_STYLE_VARIANTS 4

/* Full cache-key stride per glyph id: phases * style variants. Keys
 * are glyph_index * STRIDE + variant * PHASES + phase; TrueType
 * glyph ids are uint16, so the worst key is 65535*16+15 ≈ 1.05e6,
 * comfortably inside int. */
#define FDK_TEXT_KEY_STRIDE \
    (FDK_TEXT_SUBPIXEL_PHASES * FDK_TEXT_STYLE_VARIANTS)

/* A rasterized glyph, cached. `xoff`/`yoff` are stb's bitmap-box
 * offsets: where the bitmap's top-left sits relative to the pen
 * position (xoff) and the baseline (yoff; <= 0 means above it).
 * `advance` is the scaled horizontal advance in pixels (float —
 * rounding happens once per glyph placement, never inside the
 * accumulation). `bits` is the w*h alpha bitmap (row-major, 1 byte
 * per pixel, stride == w); NULL when the glyph has no ink (space,
 * combining marks with empty bitmaps) — such glyphs still advance. */
typedef struct fdk_glyph {
    int key;              /* cache key: glyph*STRIDE + variant*PHASES
                                 + phase (see FDK_TEXT_KEY_STRIDE) */
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

/* Unpacks the glyph id from a cache key (the kerning passes need
 * the raw stb glyph id; keys pack phase and style variant below
 * it). One definition — text.c and layout.c both use it. */
static inline int fdk_text_key_glyph(int key) {
    return key / FDK_TEXT_KEY_STRIDE;
}

/* Maps FDK_FONT_STYLE_* flags to the 0..3 variant index baked into
 * cache keys (bit 0 = bold, bit 1 = italic — the flag order). */
static inline int fdk_text_style_variant(unsigned style_flags) {
    int v = 0;
    if ((style_flags & FDK_FONT_STYLE_BOLD) != 0) {
        v |= 1;
    }
    if ((style_flags & FDK_FONT_STYLE_ITALIC) != 0) {
        v |= 2;
    }
    return v;
}

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
 * (space, empty bitmaps) — such glyphs still advance the pen.
 *
 * The styled twin (below) is the 1.4.11 core: it rasterizes with
 * the REQUESTED style flags (absolute — they replace the font's
 * current style for that glyph) under a variant-keyed cache entry.
 * This one keeps its Phase-6 semantics: the font object's own
 * fdk_font_set_style() style is what gets baked. */
const fdk_glyph *fdk_text_glyph_for(fdk_font *font, fdk_u32 codepoint);

/* The styled run core (1.4.11): same lookup/rasterize contract, but
 * the synthetic style comes from `style_flags` instead of the font's
 * current style. Regular and bold rasters of the same glyph coexist
 * in the cache under different keys. Unknown style bits are ignored
 * (masked to bold/italic), matching fdk_font_set_style. */
const fdk_glyph *fdk_text_glyph_for_styled(fdk_font *font,
                                           fdk_u32 codepoint,
                                           unsigned style_flags);

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
 * arithmetic.
 *
 * `style_flags` is the ABSOLUTE synthetic style for THIS glyph
 * (1.4.11): the single-style entry points pass the font's own
 * fdk_font_get_style(), the span-aware walks pass the effective
 * style of the byte being consumed. Callers that reset the pen
 * (line breaks, run boundaries) pass *io_prev_g = -1 so kerning
 * never crosses the boundary. */
int fdk_text_shape_step(fdk_font *font, const char *utf8, size_t len,
                        size_t *io_i, int *io_prev_g, fdk_f32 *io_pen,
                        unsigned style_flags,
                        const fdk_glyph **out_glyph, fdk_i32 *out_pen_x);

/* ---- Flattened attribute runs (1.4.11, src/text/span.c) -----------

 * The span-aware passes (measure/draw/break/ellipsize with attributes,
 * and the Label/Button/tooltip markup paint hooks) never walk the
 * caller's fdk_span array directly: spans may overlap and arrive in
 * any order, but the shaping passes need "the ONE effective attribute
 * set for byte i", monotonic and cheap. fdk__span_flatten() produces
 * exactly that: a sorted, non-overlapping, maximally-merged run
 * array over [0, byte_len). */
typedef struct fdk__text_run {
    size_t start;      /* first byte of the run (codepoint boundary) */
    size_t end;        /* one past the last byte                      */
    unsigned style;    /* ABSOLUTE FDK_FONT_STYLE_* flags             */
    bool color_set;    /* false: paint with the caller's base color   */
    fdk_color color;   /* valid only when color_set                   */
    bool underline;    /* decoration flags, drawn by the span passes  */
    bool strikethrough;
} fdk__text_run;

/* Flattens `n` spans (later array entries win wholesale on overlap —
 * the documented fdk_span contract) over the plain text
 * [0, byte_len) into *out_runs (fdk_alloc'd; free with fdk_free) and
 * *out_run_count. Spans are clamped to the text, empty/degenerate
 * ones dropped, boundaries snapped outward to codepoint boundaries
 * when the caller's spans land mid-codepoint (the markup parser
 * never does, but the public API allows it). Bytes no span covers
 * become gap runs at `default_style` (the font's own style) with no
 * color and no decorations — so the run array is CONTIGUOUS over the
 * whole text and every walk can treat run styles as absolute.
 *
 * Returns FDK_ERR_INVALID_ARGUMENT for NULL out pointers or a NULL
 * span array with n > 0; FDK_ERR_OUT_OF_MEMORY on allocation
 * failure (in which case *out_runs stays NULL). An empty span list
 * flattens to zero runs — the callers' plain-text path. */
fdk_result fdk__span_flatten(const char *utf8, size_t byte_len,
                             const fdk_span *spans, size_t n,
                             unsigned default_style,
                             fdk__text_run **out_runs,
                             size_t *out_run_count);

/* The effective style for byte i, or the font's own style when no
 * run covers it. Linear scan — runs are few (a markup label has
 * single digits) and correctness beats a premature index. */
unsigned fdk__run_style_at(const fdk__text_run *runs, size_t n,
                           size_t byte_i, unsigned fallback);

/* Runs-level measure (the engine under the public span entry point
 * and the ellipsize pass): same contract as fdk_font_measure_utf8,
 * but every glyph shapes at its run's style and kerning resets at
 * run boundaries. `limit` measures only [0, limit) bytes (limit <=
 * byte_len) — the ellipsized label's prefix advance. The advance is
 * ONE rounded total (the float pen accumulates across runs); ink
 * bounds are the union of the walked glyph boxes. */
fdk_result fdk__measure_runs(const fdk_font *font, const char *utf8,
                             size_t byte_len,
                             const fdk__text_run *runs, size_t n_runs,
                             size_t limit, fdk_text_metrics *out);

/* The wrap/ellipsize engines over a CACHED run array (the public
 * span entry points flatten and call these; the markup Label caches
 * its runs and calls them directly). Identical semantics to the
 * public span twins. */
fdk_result fdk__break_lines_runs(const fdk_font *font,
                                  const char *utf8, size_t byte_len,
                                  const fdk__text_run *runs,
                                  size_t n_runs, fdk_i32 max_width,
                                  fdk_text_line *out_lines,
                                  size_t max_lines,
                                  size_t *out_line_count,
                                  bool *out_truncated);

fdk_result fdk__ellipsize_runs(const fdk_font *font,
                                const char *utf8, size_t byte_len,
                                const fdk__text_run *runs, size_t n_runs,
                                fdk_i32 max_width,
                                size_t *out_prefix_bytes, bool *out_fits);

/* Runs-level paint: draws bytes [off, off+count) of the attributed
 * text with each glyph at its run's style/color, decorations
 * included (bars clipped to the visible range — a run half inside
 * the range bars only its visible half). base_color paints runs
 * that set no color. Damage/clip semantics are
 * fdk_surface_draw_utf8's, plus fill_rect's for the bars. The
 * widget layer's markup paint hooks (Label/Button/tooltip) call
 * this with their cached runs. */
fdk_result fdk__draw_runs(fdk_surface *surface, fdk_font *font,
                          const char *utf8, size_t byte_len,
                          const fdk__text_run *runs, size_t n_runs,
                          size_t off, size_t count,
                          fdk_i32 pen_x, fdk_i32 baseline_y,
                          fdk_color base_color);

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
