/*
 * emoji.c — color glyphs: COLRv0/v1 + CPAL, CBDT/CBLC, sbix (C-3 / B-02,
 * 1.1.0).
 *
 * FDK drew text as one alpha mask per glyph blended with a solid
 * color — color glyphs (emoji) had no path at all. This module gives
 * the glyph cache an RGBA flavor: a glyph that the FACE marks as
 * color rasterizes through here into a straight-alpha RGBA8 bitmap,
 * and the draw path composites it with the pixels' own colors
 * (fdk_surface_blend_rgba, src/render/surface.c) instead of tinting
 * an alpha mask. Everything else in the pipeline — shaping, the
 * advance math, the cache keying, the damage model — is unchanged:
 * a color glyph is just a glyph whose bitmap happens to carry its
 * own color.
 *
 * Three families are parsed, all natively from the public OpenType
 * specifications (field layouts pinned against fontTools, the
 * reference implementation, in tests/data/emoji/):
 *
 *   COLRv0 + CPAL — the layer-list format (Twemoji-COLR, older
 *     Noto builds). A base glyph maps to N layer records, each an
 *     outline glyph + a palette index; the layers composite
 *     source-over in order. Rendering is the EXISTING stb
 *     rasterizer per layer (identical coverage to mono text — one
 *     look), colorized from CPAL.
 *
 *   COLRv1 + CPAL — the paint-graph format (Noto COLRv1, the
 *     fonttools test faces). A base glyph maps to a paint graph:
 *     glyph-shaped clips, solid/linear/radial/sweep fills, affine
 *     transforms, and Porter-Duff + separable blend composition
 *     (PaintComposite). The engine evaluates the graph per pixel
 *     into canvas-sized scratch buffers. Variable (PaintVar*)
 *     formats decode at the DEFAULT instance — deltas are skipped
 *     — matching the toolkit's documented no-variable-fonts
 *     posture. Palette index 0xFFFF ("foreground") renders opaque
 *     black: the glyph cache has no per-draw color context to
 *     thread a foreground through (FreeType's own default; the
 *     palette-0-only posture is documented with it).
 *
 *   CBDT/CBLC — the bitmap-strike format (fonts-noto-color-emoji,
 *     THE Linux desktop default: Noto Color Emoji has no glyf table
 *     at all, 10.5 MB of PNG strikes). Strike selection by pixels-
 *     per-em, PNG decode through the same validated in-memory
 *     image path the clipboard rides (fdk_surface_create_from_
 *     image_bytes), and box-filter downscale / bilinear upscale to
 *     the requested size. sbix (Apple's sibling format) rides the
 *     same strike/decode/resample shape.
 *
 * Security posture: every table access is bounds-checked against
 * the file the loader already validated (validate_sfnt_face proved
 * table extents in-file); every count is capped before any
 * allocation it sizes; the paint recursion is depth-capped and
 * PaintColrGlyph-cycle-checked. A malformed color table degrades to
 * "no color glyphs" — the outline path answers instead — never to a
 * read outside the buffer.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Angle constants (the build's _POSIX_C_SOURCE discipline means
 * math.h's M_PI is not guaranteed — spell them). */
#define E_PI 3.14159265358979323846f

/* ---- 1. Bounded readers + the table directory ----------------------- */

static fdk_u16 e_be16(const fdk_u8 *p) {
    return (fdk_u16)(((fdk_u16)p[0] << 8) | (fdk_u16)p[1]);
}

static fdk_i16 e_bei16(const fdk_u8 *p) {
    return (fdk_i16)(int16_t)(((fdk_u16)p[0] << 8) | (fdk_u16)p[1]);
}

static fdk_u32 e_be32(const fdk_u8 *p) {
    return ((fdk_u32)p[0] << 24) | ((fdk_u32)p[1] << 16) |
           ((fdk_u32)p[2] << 8) | (fdk_u32)p[3];
}

/* uint24 — Offset24 fields in COLRv1. */
static fdk_u32 e_be24(const fdk_u8 *p) {
    return ((fdk_u32)p[0] << 16) | ((fdk_u32)p[1] << 8) | (fdk_u32)p[2];
}

/* Finds a table in the face's directory. Returns the (offset, length)
 * pair via the out pointers, false when absent or out-of-file. The
 * loader's validate_sfnt_face already proved every directory record's
 * extent is in-file; this re-checks cheaply anyway (defense in depth —
 * the emoji module is also reachable from the fabricated bitmap-only
 * path where this code walks the directory itself). */
static bool e_find_table(const fdk_u8 *file, size_t file_size,
                         size_t face_off, fdk_u32 tag, size_t *out_off,
                         size_t *out_len) {
    if (face_off + 12 > file_size) {
        return false;
    }
    size_t num_tables = e_be16(file + face_off + 4);
    if (face_off + 12 + num_tables * 16 > file_size) {
        return false;
    }
    const fdk_u8 *rec = file + face_off + 12;
    for (size_t i = 0; i < num_tables; i++, rec += 16) {
        if (e_be32(rec) != tag) {
            continue;
        }
        size_t off = e_be32(rec + 8);
        size_t len = e_be32(rec + 12);
        if (off > file_size || len > file_size - off) {
            return false;
        }
        if (out_off != NULL) {
            *out_off = off;
        }
        if (out_len != NULL) {
            *out_len = len;
        }
        return true;
    }
    return false;
}

/* ---- 2. CPAL ---------------------------------------------------------- */

/* The parsed CPAL view: palette 0's colors as straight RGBA bytes
 * (R,G,B,A order in memory — CPAL stores BGRA on disk). Entry count
 * is the palette's declared size; palette indices >= it (and the
 * 0xFFFF foreground sentinel) resolve at lookup time. CPAL v1's
 * label/type offsets are not parsed (labels are name-table strings —
 * presentation metadata, not rendering data). */
typedef struct {
    const fdk_u8 *entries; /* file bytes, borrowed                */
    size_t entry_count;    /* numPaletteEntries                    */
    size_t bytes_len;      /* the whole table's length, for caps  */
} e_cpal;

/* Parses CPAL. Returns false when absent or malformed (a CPAL without
 * at least one entry of palette 0 is useless). */
static bool e_cpal_parse(const fdk_u8 *file, size_t file_size,
                         size_t face_off, e_cpal *out) {
    memset(out, 0, sizeof *out);
    size_t off = 0, len = 0;
    if (!e_find_table(file, file_size, face_off, 0x4350414Cu /* 'CPAL' */,
                      &off, &len)) {
        return false;
    }
    if (len < 12) {
        return false;
    }
    const fdk_u8 *t = file + off;
    /* Header: u16 version, u16 numPaletteEntries, u16 numPalettes,
     * u16 numColorRecords, u32 colorRecordsArrayOffset — the field
     * order fontTools' own unpack(">HHHHL") pins. */
    fdk_u16 num_entries = e_be16(t + 2);
    fdk_u16 num_palettes = e_be16(t + 4);
    fdk_u16 num_records = e_be16(t + 6);
    size_t records_off = e_be32(t + 8);
    if (num_entries == 0 || num_palettes == 0 || num_records == 0) {
        return false;
    }
    /* The color-records array + the palette-index array must both be
     * in-file. Palette 0's index is the first u16 after the header
     * (v0 layout: indices directly follow; v1 adds its three offsets
     * after those — palette selection only reads the v0 part). */
    size_t index_array = 12;
    if (index_array + 2 * (size_t)num_palettes > len) {
        return false;
    }
    size_t palette0_start = e_be16(t + index_array);
    if ((size_t)num_entries > (size_t)num_records ||
        palette0_start + (size_t)num_entries > (size_t)num_records) {
        return false;
    }
    if (records_off + 4u * (size_t)num_records > len) {
        return false;
    }
    out->entries = t + records_off + 4u * palette0_start;
    out->entry_count = num_entries;
    out->bytes_len = len;
    return true;
}

/* Palette lookup → straight RGBA. 0xFFFF is the "foreground" sentinel:
 * opaque black (documented posture — see the file header). Out-of-range
 * indices are transparent (a malformed font must not crash, and an
 * invisible layer is the honest degradation). */
static void e_palette_color(const e_cpal *pal, fdk_u16 index, fdk_u8 *rgba) {
    rgba[0] = 0;
    rgba[1] = 0;
    rgba[2] = 0;
    rgba[3] = 0;
    if (index == 0xFFFFu) {
        rgba[3] = 255; /* foreground → opaque black */
        return;
    }
    if (pal->entries == NULL || (size_t)index >= pal->entry_count) {
        return;
    }
    const fdk_u8 *p = pal->entries + 4u * (size_t)index;
    rgba[0] = p[2]; /* disk order B,G,R,A */
    rgba[1] = p[1];
    rgba[2] = p[0];
    rgba[3] = p[3];
}

/* ---- 3. COLR parse (v0 + v1) ----------------------------------------- */

/* The parsed COLR view. v0: the base-glyph record array (glyph ->
 * layer slice). v1: the BaseGlyphList (glyph -> root paint offset,
 * from its OWN record start), the LayerList (paint offsets from the
 * LayerList start), and the ClipList. `file` is the whole font file
 * (borrowed); every stored pointer is inside it, so paint offsets —
 * which the spec makes relative to various sub-structures — resolve
 * to absolute FILE offsets at parse time and the paint walk never
 * does base arithmetic on hostile values. */
typedef struct {
    const fdk_u8 *file;      /* whole font file, borrowed   */
    size_t file_size;
    fdk_u16 version;

    /* v0 */
    const fdk_u8 *v0_base;   /* BaseGlyphRecord array       */
    size_t v0_base_count;
    const fdk_u8 *v0_layers; /* LayerRecord array           */
    size_t v0_layer_count;

    /* v1 */
    size_t colr_off;         /* COLR table start in file    */
    size_t colr_end;         /* one past the table          */
    const fdk_u8 *bgl;       /* BaseGlyphList               */
    size_t bgl_count;
    const fdk_u8 *layer_list; /* LayerList                  */
    size_t layer_list_count; /* its u32 paint-offset array  */
    const fdk_u8 *clip_list; /* ClipList (format 1) or NULL */
    size_t clip_count;
} e_colr;

/* The v0 base records are sorted by glyph id (the spec REQUIRES it —
 * the spec's own binary search note). Linear scan is fine at emoji
 * counts (Twemoji-COLR: ~3600); the rasterizer looks each glyph up
 * once, then the result is cached. */
static bool e_colr_v0_lookup(const e_colr *c, int glyph, size_t *first,
                             size_t *count) {
    for (size_t i = 0; i < c->v0_base_count; i++) {
        const fdk_u8 *r = c->v0_base + 6u * i;
        int g = (int)e_be16(r);
        if (g == glyph) {
            size_t f = e_be16(r + 2);
            size_t n = e_be16(r + 4);
            if (f + n > c->v0_layer_count) {
                return false; /* malformed slice */
            }
            *first = f;
            *count = n;
            return true;
        }
    }
    return false;
}

/* v1 BaseGlyphList lookup: the root paint's absolute FILE offset, or
 * false. Records are 6 bytes (glyph u16 + paint Offset32) and the
 * offset resolves from the BaseGlyphList table's OWN start (the u32
 * count's first byte — `bgl` points 4 bytes past it) — the base
 * fontTools' writer uses, pinned by dumping its output. */
static bool e_colr_v1_lookup(const e_colr *c, int glyph,
                             size_t *paint_off) {
    if (c->bgl == NULL) {
        return false;
    }
    size_t bgl_start = (size_t)(c->bgl - c->file) - 4u;
    for (size_t i = 0; i < c->bgl_count; i++) {
        const fdk_u8 *r = c->bgl + 6u * i;
        if ((int)e_be16(r) == glyph) {
            size_t rel = e_be32(r + 2);
            size_t paint = bgl_start + rel;
            if (paint >= c->colr_end) {
                return false; /* points outside the table */
            }
            *paint_off = paint;
            return true;
        }
    }
    return false;
}

/* Parses COLR. Returns false when absent or malformed beyond use.
 *
 * The v0 half: header u16 version, u16 numBaseGlyphRecords, u32
 * offsetBaseGlyphRecordArray, u32 offsetLayerRecordArray, u16
 * numLayerRecords. The v1 half appends three u32 offsets
 * (BaseGlyphList, LayerList, ClipList) + a VarIndexMap/VarStore pair
 * (both deliberately unparsed — the default-instance posture). */
static bool e_colr_parse(const fdk_u8 *file, size_t file_size,
                         size_t face_off, e_colr *out) {
    memset(out, 0, sizeof *out);
    size_t off = 0, len = 0;
    if (!e_find_table(file, file_size, face_off, 0x434F4C52u /* 'COLR' */,
                      &off, &len)) {
        return false;
    }
    if (len < 14) {
        return false;
    }
    const fdk_u8 *t = file + off;
    out->file = file;
    out->file_size = file_size;
    out->version = e_be16(t);
    out->colr_off = off;
    out->colr_end = off + len;

    /* ---- v0 ---- */
    size_t v0_base_off = e_be32(t + 4);
    size_t v0_base_count = e_be16(t + 2);
    size_t v0_layer_off = e_be32(t + 8);
    size_t v0_layer_count = e_be16(t + 12);
    if (v0_base_count > 0) {
        if (v0_base_off + 6u * v0_base_count > len) {
            return false;
        }
        out->v0_base = t + v0_base_off;
        out->v0_base_count = v0_base_count;
    }
    if (v0_layer_count > 0) {
        if (v0_layer_off + 4u * v0_layer_count > len) {
            return false;
        }
        out->v0_layers = t + v0_layer_off;
        out->v0_layer_count = v0_layer_count;
    }
    if (out->version > 1) {
        return out->v0_base_count > 0; /* unknown version: v0 half only */
    }
    if (out->version == 0) {
        return out->v0_base_count > 0 && out->v0_layer_count > 0;
    }

    /* ---- v1 additions ---- */
    if (len < 34) {
        return false;
    }
    size_t bgl_off = e_be32(t + 14);
    size_t layer_list_off = e_be32(t + 18);
    size_t clip_list_off = e_be32(t + 22);
    if (bgl_off == 0 || bgl_off + 4 > len) {
        return false;
    }
    size_t bgl_count = e_be32(t + bgl_off);
    /* Cap: each record is 6 bytes; a count that would run past the
     * table is malformed (also caps every allocation it sizes). */
    if (bgl_count > (len - bgl_off - 4) / 6) {
        return false;
    }
    out->bgl = t + bgl_off + 4;
    out->bgl_count = bgl_count;

    if (layer_list_off != 0 && layer_list_off + 4 <= len) {
        size_t ll_count = e_be32(t + layer_list_off);
        if (ll_count <= (len - layer_list_off - 4) / 4) {
            out->layer_list = t + layer_list_off + 4;
            out->layer_list_count = ll_count;
        }
    }
    if (clip_list_off != 0 && clip_list_off + 5 <= len &&
        t[clip_list_off] == 1) {
        size_t clip_count = e_be32(t + clip_list_off + 1);
        if (clip_count <= (len - clip_list_off - 5) / 8) {
            out->clip_list = t + clip_list_off;
            out->clip_count = clip_count;
        }
    }
    return true;
}

/* ---- 4. CBDT/CBLC ----------------------------------------------------- */

/* One strike, parsed: the CBLC bitmapSizeTable fields the rasterizer
 * needs, plus the located index-subtable array. Strike count is tiny
 * (Noto Color Emoji: 1; Apple sbix faces: ~5), so the parsed array is
 * a fixed small cap — a font declaring more is malformed. */
#define E_MAX_STRIKES 16
#define E_MAX_SUBTABLES 64

typedef struct {
    fdk_u32 index_sub_array_off; /* from CBLC start            */
    fdk_u32 n_sub_tables;
    fdk_u16 start_glyph, end_glyph;
    fdk_u16 ppem_x, ppem_y;
    fdk_u8 bit_depth;
    size_t sub_lo, sub_hi; /* this strike's slice of subs[] */
} e_strike;

typedef struct {
    /* One CBLC index subtable. first/last glyph + the resolved
     * FILE offset of the subtable header. */
    fdk_u16 first_glyph, last_glyph;
    size_t header_off; /* FILE offset of the indexSubHeader */
} e_index_sub;

typedef struct {
    const fdk_u8 *file;
    size_t file_size;
    size_t cbdt_off, cbdt_len; /* the CBDT table extent       */
    e_strike strikes[E_MAX_STRIKES];
    size_t n_strikes;
    e_index_sub subs[E_MAX_SUBTABLES];
    size_t n_subs;
} e_cbdt;

/* The parsed index subtable view (filled per glyph lookup): where the
 * glyph's image bytes live and which format describes them. */
typedef struct {
    fdk_u16 index_format; /* 1..5                          */
    fdk_u16 image_format; /* 17/18/19 (byte-aligned=PNG)   */
    size_t data_off;      /* FILE offset: image data base  */
} e_located;

/* Locates glyph data in one index subtable. Returns false when the
 * glyph is out of the subtable's range or the record is malformed.
 * imageDataOffset (the header's u32) is relative to the CBDT table
 * start — the reference implementation's own resolution (fontTools
 * E_B_L_C_: locations = imageDataOffset + offsetArray[i]). */
static bool e_sub_locate(const e_cbdt *cb, const e_index_sub *sub,
                         int glyph, e_located *out) {
    if (glyph < (int)sub->first_glyph || glyph > (int)sub->last_glyph) {
        return false;
    }
    size_t cbdt_end = cb->cbdt_off + cb->cbdt_len;
    if (sub->header_off + 8 > cb->file_size) {
        return false;
    }
    const fdk_u8 *h = cb->file + sub->header_off;
    fdk_u16 index_format = e_be16(h);
    fdk_u16 image_format = e_be16(h + 2);
    fdk_u32 image_data_off = e_be32(h + 4);
    if (index_format < 1 || index_format > 5) {
        return false;
    }
    size_t base = cb->cbdt_off + image_data_off;
    const fdk_u8 *arr = h + 8; /* the offsets/pairs array    */
    size_t arr_avail = cb->file_size - (sub->header_off + 8);
    size_t n = (size_t)(sub->last_glyph - sub->first_glyph + 1);

    size_t glyph_rel = 0; /* byte offset from `base`     */
    switch (index_format) {
    case 1: { /* u32 offsets, glyphs first..last     */
        if (arr_avail < (n + 1) * 4) {
            return false;
        }
        size_t i = (size_t)(glyph - (int)sub->first_glyph);
        size_t o0 = e_be32(arr + i * 4);
        size_t o1 = e_be32(arr + i * 4 + 4);
        if (o1 < o0 || o1 - o0 > cb->cbdt_len) {
            return false;
        }
        glyph_rel = o0;
        out->data_off = base + o0;
        break;
    }
    case 2: { /* uniform size + big metrics          */
        if (arr_avail < 16) {
            return false;
        }
        fdk_u32 image_size = e_be32(arr);
        size_t i = (size_t)(glyph - (int)sub->first_glyph);
        out->data_off = base + (size_t)image_size * i;
        break;
    }
    case 3: { /* u16 offsets                         */
        if (arr_avail < (n + 1) * 2) {
            return false;
        }
        size_t i = (size_t)(glyph - (int)sub->first_glyph);
        size_t o0 = e_be16(arr + i * 2);
        size_t o1 = e_be16(arr + i * 2 + 2);
        if (o1 < o0) {
            return false;
        }
        out->data_off = base + o0;
        break;
    }
    case 4: { /* sparse (u16 glyphId, u16 offset) pairs */
        if (arr_avail < 4) {
            return false;
        }
        size_t num_glyphs = e_be32(arr);
        /* The array is numGlyphs+1 four-byte entries; the last is
         * the end-offset sentinel (glyphId ignored — the EBLC
         * spec's own layout; fontTools' writer emits exactly this
         * and its reader over-reads one pair harmlessly). */
        if (num_glyphs == 0 ||
            (num_glyphs + 1) * 4 > arr_avail - 4) {
            return false;
        }
        const fdk_u8 *p = arr + 4;
        bool found = false;
        for (size_t i = 0; i < num_glyphs; i++) {
            if ((int)e_be16(p + i * 4) == glyph) {
                size_t o0 = e_be16(p + i * 4 + 2);
                size_t o1 = e_be16(p + (i + 1) * 4 + 2);
                if (o1 < o0) {
                    return false;
                }
                out->data_off = base + o0;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        break;
    }
    case 5: { /* uniform size + big metrics + ids    */
        if (arr_avail < 16) {
            return false;
        }
        fdk_u32 image_size = e_be32(arr);
        size_t num = e_be32(arr + 12);
        if (num == 0 || num > (arr_avail - 16) / 2) {
            return false;
        }
        const fdk_u8 *ids = arr + 16;
        bool found = false;
        for (size_t i = 0; i < num; i++) {
            if ((int)e_be16(ids + i * 2) == glyph) {
                out->data_off = base + (size_t)image_size * i;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    if (out->data_off >= cbdt_end) {
        return false;
    }
    out->index_format = index_format;
    out->image_format = image_format;
    (void)glyph_rel;
    return true;
}

/* Locates a glyph across all parsed subtables. */
static bool e_cbdt_locate(const e_cbdt *cb, int glyph, e_located *out) {
    for (size_t i = 0; i < cb->n_subs; i++) {
        if (e_sub_locate(cb, &cb->subs[i], glyph, out)) {
            return true;
        }
    }
    return false;
}

/* Parses CBLC. Returns false when absent/unusable. */
static bool e_cbdt_parse(const fdk_u8 *file, size_t file_size,
                         size_t face_off, e_cbdt *out) {
    memset(out, 0, sizeof *out);
    size_t off = 0, len = 0, cbd_off = 0, cbd_len = 0;
    if (!e_find_table(file, file_size, face_off, 0x43424C43u /* 'CBLC' */,
                      &off, &len) ||
        !e_find_table(file, file_size, face_off, 0x43424454u /* 'CBDT' */,
                      &cbd_off, &cbd_len)) {
        return false;
    }
    if (len < 8) {
        return false;
    }
    const fdk_u8 *t = file + off;
    fdk_u32 num_sizes = e_be32(t + 4);
    if (num_sizes == 0 || num_sizes > E_MAX_STRIKES) {
        return false;
    }
    if (8u + 48u * (size_t)num_sizes > len) {
        return false;
    }
    out->file = file;
    out->file_size = file_size;
    out->cbdt_off = cbd_off;
    out->cbdt_len = cbd_len;

    for (size_t s = 0; s < num_sizes; s++) {
        const fdk_u8 *st = t + 8 + 48 * s;
        e_strike k;
        memset(&k, 0, sizeof k);
        k.index_sub_array_off = e_be32(st);
        k.n_sub_tables = e_be32(st + 8);
        k.start_glyph = e_be16(st + 40);
        k.end_glyph = e_be16(st + 42);
        k.ppem_x = st[44];
        k.ppem_y = st[45];
        k.bit_depth = st[46];
        if (k.ppem_x == 0) {
            continue; /* a strike with no size is unusable */
        }
        if (k.n_sub_tables == 0 || k.n_sub_tables > E_MAX_SUBTABLES) {
            continue;
        }
        /* The index-subtable array must be in-file; entries are 8
         * bytes (first, last, additionalOffset relative to the
         * array's own start). */
        size_t arr_off = off + (size_t)k.index_sub_array_off;
        if (arr_off + 8u * (size_t)k.n_sub_tables > file_size) {
            continue;
        }
        k.sub_lo = out->n_subs;
        k.sub_hi = out->n_subs;
        out->strikes[out->n_strikes] = k;
        for (fdk_u32 i = 0; i < k.n_sub_tables; i++) {
            const fdk_u8 *e = file + arr_off + 8u * (size_t)i;
            e_index_sub sub;
            sub.first_glyph = e_be16(e);
            sub.last_glyph = e_be16(e + 2);
            size_t addl = e_be32(e + 4);
            sub.header_off = arr_off + addl;
            if (sub.first_glyph > sub.last_glyph ||
                sub.header_off + 8 > file_size) {
                continue;
            }
            if (out->n_subs < E_MAX_SUBTABLES) {
                out->subs[out->n_subs++] = sub;
                out->strikes[out->n_strikes].sub_hi = out->n_subs;
            }
        }
        out->n_strikes++;
    }
    return out->n_strikes > 0 && out->n_subs > 0;
}

/* ---- 5. sbix (Apple's bitmap strikes — same shape, PNG payloads) ----- */

typedef struct {
    const fdk_u8 *file;
    size_t file_size;
    size_t strikes[E_MAX_STRIKES]; /* FILE offsets, one per strike */
    fdk_u16 ppem[E_MAX_STRIKES];
    size_t n_strikes;
} e_sbix;

static bool e_sbix_parse(const fdk_u8 *file, size_t file_size,
                         size_t face_off, e_sbix *out) {
    memset(out, 0, sizeof *out);
    size_t off = 0, len = 0;
    if (!e_find_table(file, file_size, face_off, 0x73626978u /* 'sbix' */,
                      &off, &len)) {
        return false;
    }
    if (len < 8) {
        return false;
    }
    const fdk_u8 *t = file + off;
    size_t num_strikes = e_be32(t + 4);
    if (num_strikes == 0 || num_strikes > E_MAX_STRIKES) {
        return false;
    }
    if (8u + 8u * num_strikes > len) {
        return false;
    }
    out->file = file;
    out->file_size = file_size;
    for (size_t i = 0; i < num_strikes; i++) {
        const fdk_u8 *r = t + 8 + 8 * i;
        fdk_u16 ppem = e_be16(r);
        size_t strike_off = e_be32(r + 4);
        if (ppem == 0 || strike_off == 0 || off + strike_off > file_size) {
            continue;
        }
        out->ppem[out->n_strikes] = ppem;
        out->strikes[out->n_strikes] = off + strike_off;
        out->n_strikes++;
    }
    return out->n_strikes > 0;
}

/* ---- 6. Bitmap resampling --------------------------------------------- */

/* Area-average (box) downscale: the correct answer for large
 * reductions — every destination pixel averages exactly the source
 * pixels it covers, weighted by fractional overlap (a mipmap-style
 * filter: no aliasing, no shimmer). Bilinear upscale for growth.
 * Straight RGBA in both cases; alpha resamples on its own channel
 * (straight, not premultiplied — the source is PNG straight alpha;
 * premultiplying would darken semi-transparent edges of downscaled
 * emoji against whatever the destination blend does with them).
 * In-place-safe when src == dst is FORBIDDEN (buffers differ). */
static fdk_u8 *e_resample(const fdk_u8 *src, int sw, int sh, int dw,
                          int dh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return NULL;
    }
    fdk_u8 *dst = fdk_alloc((size_t)dw * (size_t)dh * 4u);
    if (dst == NULL) {
        return NULL;
    }
    if (dw <= sw && dh <= sh) {
        /* Box filter with exact fractional coverage. */
        fdk_f32 sx = (fdk_f32)sw / (fdk_f32)dw;
        fdk_f32 sy = (fdk_f32)sh / (fdk_f32)dh;
        for (int y = 0; y < dh; y++) {
            fdk_f32 fy0 = (fdk_f32)y * sy;
            fdk_f32 fy1 = (fdk_f32)(y + 1) * sy;
            int iy0 = (int)fy0;
            int iy1 = (int)ceilf(fy1 - 1e-4f);
            if (iy1 > sh) {
                iy1 = sh;
            }
            for (int x = 0; x < dw; x++) {
                fdk_f32 fx0 = (fdk_f32)x * sx;
                fdk_f32 fx1 = (fdk_f32)(x + 1) * sx;
                int ix0 = (int)fx0;
                int ix1 = (int)ceilf(fx1 - 1e-4f);
                if (ix1 > sw) {
                    ix1 = sw;
                }
                fdk_f32 r = 0, g = 0, b = 0, a = 0, wsum = 0;
                for (int yy = iy0; yy < iy1; yy++) {
                    fdk_f32 wy = 1.0f;
                    if (yy < fy0) {
                        wy = (fdk_f32)yy + 1.0f - fy0;
                    }
                    if ((fdk_f32)(yy + 1) > fy1) {
                        wy = fy1 - (fdk_f32)yy;
                        if (wy < 0) {
                            wy = 0;
                        }
                    }
                    for (int xx = ix0; xx < ix1; xx++) {
                        fdk_f32 wx = 1.0f;
                        if (xx < fx0) {
                            wx = (fdk_f32)xx + 1.0f - fx0;
                        }
                        if ((fdk_f32)(xx + 1) > fx1) {
                            wx = fx1 - (fdk_f32)xx;
                            if (wx < 0) {
                                wx = 0;
                            }
                        }
                        fdk_f32 wgt = wx * wy;
                        if (wgt <= 0) {
                            continue;
                        }
                        const fdk_u8 *p =
                            src + ((size_t)yy * (size_t)sw + (size_t)xx) * 4u;
                        r += (fdk_f32)p[0] * wgt;
                        g += (fdk_f32)p[1] * wgt;
                        b += (fdk_f32)p[2] * wgt;
                        a += (fdk_f32)p[3] * wgt;
                        wsum += wgt;
                    }
                }
                fdk_u8 *q = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
                if (wsum > 0) {
                    q[0] = (fdk_u8)((r / wsum) + 0.5f);
                    q[1] = (fdk_u8)((g / wsum) + 0.5f);
                    q[2] = (fdk_u8)((b / wsum) + 0.5f);
                    q[3] = (fdk_u8)((a / wsum) + 0.5f);
                } else {
                    q[0] = q[1] = q[2] = q[3] = 0;
                }
            }
        }
    } else {
        /* Bilinear upscale (clamped edges). */
        for (int y = 0; y < dh; y++) {
            fdk_f32 fy = ((fdk_f32)y + 0.5f) * (fdk_f32)sh / (fdk_f32)dh -
                         0.5f;
            int y0 = (int)floorf(fy);
            int y1 = y0 + 1;
            fdk_f32 ty = fy - (fdk_f32)y0;
            if (y0 < 0) {
                y0 = 0;
                ty = 0;
            }
            if (y1 >= sh) {
                y1 = sh - 1;
            }
            for (int x = 0; x < dw; x++) {
                fdk_f32 fx =
                    ((fdk_f32)x + 0.5f) * (fdk_f32)sw / (fdk_f32)dw - 0.5f;
                int x0 = (int)floorf(fx);
                int x1 = x0 + 1;
                fdk_f32 tx = fx - (fdk_f32)x0;
                if (x0 < 0) {
                    x0 = 0;
                    tx = 0;
                }
                if (x1 >= sw) {
                    x1 = sw - 1;
                }
                const fdk_u8 *p00 = src + ((size_t)y0 * (size_t)sw +
                                           (size_t)x0) * 4u;
                const fdk_u8 *p10 = src + ((size_t)y0 * (size_t)sw +
                                           (size_t)x1) * 4u;
                const fdk_u8 *p01 = src + ((size_t)y1 * (size_t)sw +
                                           (size_t)x0) * 4u;
                const fdk_u8 *p11 = src + ((size_t)y1 * (size_t)sw +
                                           (size_t)x1) * 4u;
                fdk_u8 *q = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4u;
                for (int c = 0; c < 4; c++) {
                    fdk_f32 v =
                        (1.0f - ty) *
                            ((1.0f - tx) * (fdk_f32)p00[c] +
                             tx * (fdk_f32)p10[c]) +
                        ty * ((1.0f - tx) * (fdk_f32)p01[c] +
                              tx * (fdk_f32)p11[c]);
                    q[c] = (fdk_u8)(v + 0.5f);
                }
            }
        }
    }
    return dst;
}

/* ---- 7. COLRv0 rasterize --------------------------------------------- */

/* Renders one v0 color glyph: the layer bitmaps through the EXISTING
 * stb rasterizer (the same coverage mono text gets — one look), each
 * colorized from CPAL and composited source-over in order. Returns
 * the RGBA bitmap + the box; NULL when the glyph has no ink. */
static fdk_u8 *e_colr_v0_render(const e_colr *c, const e_cpal *pal,
                                const stbtt_fontinfo *info,
                                fdk_cff_font *cff, fdk_f32 scale,
                                int glyph, int *out_w, int *out_h,
                                fdk_i32 *out_xoff, fdk_i32 *out_yoff) {
    size_t first = 0, count = 0;
    if (!e_colr_v0_lookup(c, glyph, &first, &count) || count == 0) {
        return NULL;
    }

    /* Pass 1: the ink box — the union of every layer's glyph box,
     * scaled and floor/ceil'd exactly like the mono glyph path (the
     * same arithmetic cff_glyph_bitmap uses, so color and mono glyphs
     * of the same size land on the same pixel grid). */
    fdk_i64 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int any = 0;
    for (size_t i = 0; i < count; i++) {
        const fdk_u8 *lr = c->v0_layers + 4u * (first + i);
        int layer_glyph = (int)e_be16(lr);
        int gx0, gy0, gx1, gy1;
        bool have = cff != NULL
                        ? fdk__cff_glyph_box(cff, layer_glyph, &gx0, &gy0,
                                             &gx1, &gy1)
                        : (stbtt_GetGlyphBox(info, layer_glyph, &gx0, &gy0,
                                             &gx1, &gy1) != 0);
        if (!have) {
            continue;
        }
        fdk_i64 px0 = (fdk_i64)floorf((fdk_f32)gx0 * scale);
        fdk_i64 py0 = (fdk_i64)floorf(-(fdk_f32)gy1 * scale);
        fdk_i64 px1 = (fdk_i64)ceilf((fdk_f32)gx1 * scale);
        fdk_i64 py1 = (fdk_i64)ceilf(-(fdk_f32)gy0 * scale);
        if (!any) {
            bx0 = px0;
            by0 = py0;
            bx1 = px1;
            by1 = py1;
            any = 1;
        } else {
            if (px0 < bx0) bx0 = px0;
            if (py0 < by0) by0 = py0;
            if (px1 > bx1) bx1 = px1;
            if (py1 > by1) by1 = py1;
        }
    }
    if (!any || bx1 <= bx0 || by1 <= by0) {
        return NULL;
    }
    int w = (int)(bx1 - bx0);
    int h = (int)(by1 - by0);
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > 16u * 1024u * 1024u) {
        return NULL;
    }
    fdk_u8 *canvas = fdk_alloc((size_t)w * (size_t)h * 4u);
    if (canvas == NULL) {
        return NULL;
    }
    memset(canvas, 0, (size_t)w * (size_t)h * 4u);

    /* Pass 2: rasterize, colorize, composite. */
    for (size_t i = 0; i < count; i++) {
        const fdk_u8 *lr = c->v0_layers + 4u * (first + i);
        int layer_glyph = (int)e_be16(lr);
        fdk_u16 palette_index = e_be16(lr + 2);
        fdk_u8 color[4];
        e_palette_color(pal, palette_index, color);
        if (color[3] == 0) {
            continue; /* transparent layer: nothing to paint */
        }

        int lw = 0, lh = 0;
        fdk_i32 lx0 = 0, ly0 = 0;
        fdk_u8 *lbits = NULL;
        if (cff != NULL) {
            stbtt_vertex *verts = NULL;
            int nv = fdk__cff_glyph_shape(cff, layer_glyph, &verts);
            if (nv > 0 && verts != NULL) {
                int gx0 = verts[0].x, gy0 = verts[0].y;
                int gx1 = gx0, gy1 = gy0;
                for (int k = 0; k < nv; k++) {
                    if (verts[k].x < gx0) gx0 = verts[k].x;
                    if (verts[k].x > gx1) gx1 = verts[k].x;
                    if (verts[k].y < gy0) gy0 = verts[k].y;
                    if (verts[k].y > gy1) gy1 = verts[k].y;
                }
                lx0 = (int)floorf((fdk_f32)gx0 * scale);
                ly0 = (int)floorf(-(fdk_f32)gy1 * scale);
                int lx1 = (int)ceilf((fdk_f32)gx1 * scale);
                int ly1 = (int)ceilf(-(fdk_f32)gy0 * scale);
                lw = lx1 - lx0;
                lh = ly1 - ly0;
                if (lw > 0 && lh > 0) {
                    fdk_u8 *pixels =
                        fdk_alloc((size_t)lw * (size_t)lh);
                    if (pixels != NULL) {
                        stbtt__bitmap gbm;
                        gbm.w = lw;
                        gbm.h = lh;
                        gbm.stride = lw;
                        gbm.pixels = pixels;
                        stbtt_Rasterize(&gbm, 0.35f, verts, nv, scale,
                                        scale, 0.0f, 0.0f, lx0, ly0, 1,
                                        NULL);
                        lbits = pixels;
                    }
                }
            }
            fdk_free(verts);
        } else {
            lbits = stbtt_GetGlyphBitmapSubpixel(info, 0, scale, 0.0f,
                                                 0.0f, layer_glyph, &lw,
                                                 &lh, &lx0, &ly0);
        }
        if (lbits == NULL || lw <= 0 || lh <= 0) {
            fdk_free(lbits);
            continue;
        }
        /* Composite the colorized layer onto the canvas. The layer's
         * canvas position: (lx0 - bx0, ly0 - by0) — both are y-down
         * bitmap offsets from the origin, so the difference is the
         * canvas-space placement. */
        fdk_i32 ox = lx0 - (fdk_i32)bx0;
        fdk_i32 oy = ly0 - (fdk_i32)by0;
        for (int y = 0; y < lh; y++) {
            int cy = y + oy;
            if (cy < 0 || cy >= h) {
                continue;
            }
            for (int x = 0; x < lw; x++) {
                int cx = x + ox;
                if (cx < 0 || cx >= w) {
                    continue;
                }
                fdk_u8 cov = lbits[(size_t)y * (size_t)lw + (size_t)x];
                if (cov == 0) {
                    continue;
                }
                fdk_u8 *q =
                    canvas + ((size_t)cy * (size_t)w + (size_t)cx) * 4u;
                fdk_u32 sa = ((fdk_u32)color[3] * (fdk_u32)cov + 127u) /
                             255u;
                fdk_u32 da = (fdk_u32)q[3];
                /* Source-over with straight alpha, the integer form
                 * of blend_mask's mono path. */
                fdk_u32 out_a = sa + da * (255u - sa) / 255u;
                if (out_a == 0) {
                    q[0] = q[1] = q[2] = q[3] = 0;
                    continue;
                }
                for (int ch = 0; ch < 3; ch++) {
                    fdk_u32 sc = (fdk_u32)color[ch] * sa;
                    fdk_u32 dc = (fdk_u32)q[ch] * da * (255u - sa) / 255u;
                    q[ch] = (fdk_u8)((sc + dc) / out_a);
                }
                q[3] = (fdk_u8)out_a;
            }
        }
        fdk_free(lbits);
    }
    *out_w = w;
    *out_h = h;
    *out_xoff = (fdk_i32)bx0;
    *out_yoff = (fdk_i32)by0;
    return canvas;
}

/* ---- 8. COLRv1: the paint engine ------------------------------------- */

#include "stb_image.h" /* PNG decode for CBDT/sbix payloads (decls only;
                          the implementation lives in
                          src/render/stb_image_impl.c — one TU, one
                          allocator, same as the clipboard image path) */

/* 2x3 affine, Cairo field semantics: x' = a*x + c*y + e;
 * y' = b*x + d*y + f. PaintTransform's Affine2x3 serializes in
 * exactly this field order (xx, yx, xy, yy, dx, dy — pinned against
 * fontTools' getTransform mapping; the spec's own "xy/yx labels are
 * swapped" note reconciled there). */
typedef struct {
    fdk_f32 a, b, c, d, e, f;
} e_mat;

static e_mat e_mat_identity(void) {
    e_mat m = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    return m;
}

/* m ∘ t: t applies to points FIRST, then m (the child-to-parent
 * direction the paint graph needs — see the section header). */
static e_mat e_mat_mul(const e_mat *m, const e_mat *t) {
    e_mat r;
    r.a = m->a * t->a + m->c * t->b;
    r.b = m->b * t->a + m->d * t->b;
    r.c = m->a * t->c + m->c * t->d;
    r.d = m->b * t->c + m->d * t->d;
    r.e = m->a * t->e + m->c * t->f + m->e;
    r.f = m->b * t->e + m->d * t->f + m->f;
    return r;
}

static e_mat e_mat_translate(fdk_f32 dx, fdk_f32 dy) {
    e_mat m = {1.0f, 0.0f, 0.0f, 1.0f, dx, dy};
    return m;
}

static e_mat e_mat_scale(fdk_f32 sx, fdk_f32 sy) {
    e_mat m = {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
    return m;
}

static e_mat e_mat_rotate(fdk_f32 radians) {
    fdk_f32 c = cosf(radians);
    fdk_f32 s = sinf(radians);
    e_mat m = {c, s, -s, c, 0.0f, 0.0f};
    return m;
}

/* T_center ∘ inner ∘ T_-center: the fontTools chain
 * translate(c).OP().translate(-c) — every AroundCenter paint. */
static e_mat e_center_op(const e_mat *inner, fdk_f32 cx, fdk_f32 cy) {
    e_mat tc = e_mat_translate(cx, cy);
    e_mat tnc = e_mat_translate(-cx, -cy);
    e_mat mid = e_mat_mul(inner, &tnc);
    return e_mat_mul(&tc, &mid);
}

static bool e_mat_invert(const e_mat *m, e_mat *out) {
    fdk_f32 det = m->a * m->d - m->b * m->c;
    if (fabsf(det) < 1e-12f) {
        return false; /* degenerate: nothing can render */
    }
    fdk_f32 inv = 1.0f / det;
    out->a = m->d * inv;
    out->b = -m->b * inv;
    out->c = -m->c * inv;
    out->d = m->a * inv;
    out->e = (m->c * m->f - m->d * m->e) * inv;
    out->f = (m->b * m->e - m->a * m->f) * inv;
    return true;
}

static void e_mat_apply(const e_mat *m, fdk_f32 x, fdk_f32 y,
                        fdk_f32 *out_x, fdk_f32 *out_y) {
    *out_x = m->a * x + m->c * y + m->e;
    *out_y = m->b * x + m->d * y + m->f;
}

/* Angle encodings (pinned against fontTools' converters):
 * F2Dot14-of-half-circle → degrees; BiasedAngle adds a +1.0 bias so
 * +360° stays encodable. */
static fdk_f32 e_angle_deg(fdk_u16 raw) {
    return (fdk_f32)(fdk_i16)raw / 16384.0f * 180.0f;
}

static fdk_f32 e_biased_angle_deg(fdk_u16 raw) {
    return ((fdk_f32)(fdk_i16)raw / 16384.0f - 1.0f) * 180.0f;
}

static fdk_f32 e_f2dot14(fdk_u16 raw) {
    return (fdk_f32)(fdk_i16)raw / 16384.0f;
}

static fdk_f32 e_fixed(fdk_u32 raw) {
    return (fdk_f32)(fdk_i32)raw / 65536.0f;
}

/* The paint engine's working context. Scratch buffers are
 * depth-indexed and lazily allocated once per rasterization; the
 * pool dies with the context (one fdk_alloc'd array). */
#define E_PAINT_MAX_DEPTH 10
#define E_COLR_CYCLE_MAX 8

typedef struct {
    const e_colr *colr;
    const e_cpal *pal;
    const stbtt_fontinfo *info; /* outline source (glyf)         */
    fdk_cff_font *cff;          /* outline source (CFF)          */
    fdk_f32 scale;              /* font units -> pixels          */
    int w, h;                   /* canvas size                   */
    fdk_u8 *pool[E_PAINT_MAX_DEPTH];      /* RGBA scratch         */
    fdk_u8 *mask_pool[E_PAINT_MAX_DEPTH]; /* coverage scratch     */
    int visiting[E_COLR_CYCLE_MAX]; /* PaintColrGlyph cycle guard */
    int n_visiting;
    bool failed;                /* allocation failure anywhere   */
} e_ctx;

static void e_ctx_destroy(e_ctx *ctx) {
    for (int i = 0; i < E_PAINT_MAX_DEPTH; i++) {
        fdk_free(ctx->pool[i]);
        fdk_free(ctx->mask_pool[i]);
    }
}

/* A scratch buffer at `depth`; NULL (and ctx->failed) when the pool
 * is exhausted or the allocation fails. Buffers are recycled across
 * siblings at the same depth — clear before use. */
static fdk_u8 *e_scratch(e_ctx *ctx, int depth) {
    if (depth < 0 || depth >= E_PAINT_MAX_DEPTH) {
        return NULL;
    }
    if (ctx->pool[depth] == NULL) {
        ctx->pool[depth] =
            fdk_alloc((size_t)ctx->w * (size_t)ctx->h * 4u);
        if (ctx->pool[depth] == NULL) {
            ctx->failed = true;
            return NULL;
        }
    }
    memset(ctx->pool[depth], 0,
           (size_t)ctx->w * (size_t)ctx->h * 4u);
    return ctx->pool[depth];
}

static fdk_u8 *e_mask_scratch(e_ctx *ctx, int depth) {
    if (depth < 0 || depth >= E_PAINT_MAX_DEPTH) {
        return NULL;
    }
    if (ctx->mask_pool[depth] == NULL) {
        ctx->mask_pool[depth] = fdk_alloc((size_t)ctx->w * (size_t)ctx->h);
        if (ctx->mask_pool[depth] == NULL) {
            ctx->failed = true;
            return NULL;
        }
    }
    memset(ctx->mask_pool[depth], 0, (size_t)ctx->w * (size_t)ctx->h);
    return ctx->mask_pool[depth];
}

/* ---- 8a. Color lines ---- */

/* A parsed color line: stops normalized to offsets in [0, 1] with
 * straight RGBA. Extend rides the fill (pad/repeat/reflect). */
#define E_MAX_STOPS 64

typedef struct {
    fdk_f32 offset[E_MAX_STOPS];
    fdk_u8 rgba[E_MAX_STOPS][4];
    int n;
    fdk_u8 extend; /* 0 pad, 1 repeat, 2 reflect */
} e_colorline;

/* Parses a ColorLine (or VarColorLine — var deltas skipped) at the
 * given FILE offset. Returns false when malformed. */
static bool e_colorline_parse(const e_ctx *ctx, size_t off,
                              bool is_var, e_colorline *out) {
    memset(out, 0, sizeof *out);
    if (off + 3 > ctx->colr->colr_end) {
        return false;
    }
    const fdk_u8 *p = ctx->colr->file + off;
    out->extend = p[0];
    fdk_u16 n = e_be16(p + 1);
    if (out->extend > 2 || n == 0) {
        return false;
    }
    if (n > E_MAX_STOPS) {
        n = E_MAX_STOPS; /* degrade: first 64 stops */
    }
    size_t step = is_var ? 10u : 6u;
    if (off + 3 + (size_t)n * step > ctx->colr->colr_end) {
        return false;
    }
    for (int i = 0; i < (int)n; i++) {
        const fdk_u8 *s = p + 3 + (size_t)i * step;
        out->offset[i] = e_f2dot14(e_be16(s));
        e_palette_color(ctx->pal, e_be16(s + 2), out->rgba[i]);
        fdk_f32 alpha = e_f2dot14(e_be16(s + 4));
        if (alpha < 0) {
            alpha = 0;
        }
        if (alpha > 1) {
            alpha = 1;
        }
        out->rgba[i][3] =
            (fdk_u8)((fdk_f32)out->rgba[i][3] * alpha + 0.5f);
    }
    out->n = (int)n;
    return true;
}

/* The color at gradient position t in [0, 1] under the extend mode.
 * Straight RGBA lerp (FreeType's own interpolation choice). */
static void e_colorline_at(const e_colorline *cl, fdk_f32 t,
                           fdk_u8 *rgba) {
    if (cl->n == 0) {
        memset(rgba, 0, 4);
        return;
    }
    /* Apply the extend mode to t. */
    if (cl->extend == 1) { /* repeat */
        t = t - floorf(t);
        if (t < 0) {
            t = 0;
        }
    } else if (cl->extend == 2) { /* reflect */
        fdk_f32 two = 2.0f * (t - floorf(t)) - 1.0f; /* in [-1, 1) */
        if (two < 0) {
            two = -two;
        }
        t = two;
    } else { /* pad */
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
    }
    /* Bracketing stops. */
    if (t <= cl->offset[0]) {
        memcpy(rgba, cl->rgba[0], 4);
        return;
    }
    if (t >= cl->offset[cl->n - 1]) {
        memcpy(rgba, cl->rgba[cl->n - 1], 4);
        return;
    }
    for (int i = 0; i < cl->n - 1; i++) {
        if (t >= cl->offset[i] && t <= cl->offset[i + 1]) {
            fdk_f32 span = cl->offset[i + 1] - cl->offset[i];
            fdk_f32 k = span > 0 ? (t - cl->offset[i]) / span : 0.0f;
            for (int c = 0; c < 4; c++) {
                fdk_f32 v = (fdk_f32)cl->rgba[i][c] * (1.0f - k) +
                            (fdk_f32)cl->rgba[i + 1][c] * k;
                rgba[c] = (fdk_u8)(v + 0.5f);
            }
            return;
        }
    }
    memcpy(rgba, cl->rgba[cl->n - 1], 4);
}

/* ---- 8b. Buffer atoms (straight RGBA8) -------------------------------- */

/* Source-over one FULL buffer onto another (same geometry). */
static void e_src_over(fdk_u8 *dst, const fdk_u8 *src, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const fdk_u8 *s = src + i * 4u;
        fdk_u8 *d = dst + i * 4u;
        fdk_u32 sa = s[3];
        if (sa == 0) {
            continue;
        }
        if (sa == 255) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
            continue;
        }
        fdk_u32 da = d[3];
        fdk_u32 out_a = sa + da * (255u - sa) / 255u;
        if (out_a == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; c++) {
            fdk_u32 sc = (fdk_u32)s[c] * sa;
            fdk_u32 dc = (fdk_u32)d[c] * da * (255u - sa) / 255u;
            d[c] = (fdk_u8)((sc + dc) / out_a);
        }
        d[3] = (fdk_u8)out_a;
    }
}

/* Multiply a buffer's alpha by a coverage mask (PaintGlyph's clip). */
static void e_mask_alpha(fdk_u8 *buf, const fdk_u8 *mask, int w, int h) {
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        fdk_u8 m = mask[i];
        if (m == 255) {
            continue;
        }
        fdk_u8 *q = buf + i * 4u;
        if (m == 0) {
            q[0] = q[1] = q[2] = q[3] = 0;
            continue;
        }
        q[0] = (fdk_u8)(((fdk_u32)q[0] * m + 127u) / 255u);
        q[1] = (fdk_u8)(((fdk_u32)q[1] * m + 127u) / 255u);
        q[2] = (fdk_u8)(((fdk_u32)q[2] * m + 127u) / 255u);
        q[3] = (fdk_u8)(((fdk_u32)q[3] * m + 127u) / 255u);
    }
}

/* ---- 8c. The 28 blend modes (W3C compositing semantics) --------------- */

/* Channel blend helpers on UNpremultiplied 0..1 values. */
static fdk_f32 e_clamp01(fdk_f32 v) {
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static fdk_f32 e_sep_screen(fdk_f32 b, fdk_f32 s) {
    return b + s - b * s;
}

static fdk_f32 e_sep_hard_light(fdk_f32 b, fdk_f32 s) {
    return s <= 0.5f ? 2.0f * b * s : e_sep_screen(b, 2.0f * s - 1.0f);
}

static fdk_f32 e_sep_overlay(fdk_f32 b, fdk_f32 s) {
    return e_sep_hard_light(s, b);
}

static fdk_f32 e_sep_dodge(fdk_f32 b, fdk_f32 s) {
    return b <= 0 ? 0 : (s >= 1 ? 1 : b / (1.0f - s));
}

static fdk_f32 e_sep_burn(fdk_f32 b, fdk_f32 s) {
    return b >= 1 ? 1 : (s <= 0 ? 0 : (1.0f - b) / (1.0f - s) > 1.0f
                                        ? 1.0f
                                        : (1.0f - b) / (1.0f - s));
}

static fdk_f32 e_sep_soft_light(fdk_f32 b, fdk_f32 s) {
    if (s <= 0.5f) {
        return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    }
    fdk_f32 d = 0.0f;
    if (b <= 0.25f) {
        d = ((16.0f * b - 12.0f) * b + 4.0f) * b;
    } else {
        d = sqrtf(b);
    }
    return b + (2.0f * s - 1.0f) * (d - b);
}

static fdk_f32 e_lum(fdk_f32 r, fdk_f32 g, fdk_f32 b) {
    return 0.3f * r + 0.59f * g + 0.11f * b;
}

static void e_clip_color(fdk_f32 *r, fdk_f32 *g, fdk_f32 *b) {
    fdk_f32 l = e_lum(*r, *g, *b);
    fdk_f32 n = *r < *g ? (*r < *b ? *r : *b) : (*g < *b ? *g : *b);
    fdk_f32 x = *r > *g ? (*r > *b ? *r : *b) : (*g > *b ? *g : *b);
    if (n < 0) {
        fdk_f32 k = l / (l - n);
        *r = l + (*r - l) * k;
        *g = l + (*g - l) * k;
        *b = l + (*b - l) * k;
    }
    if (x > 1) {
        fdk_f32 k = (1.0f - l) / (x - l);
        *r = l + (*r - l) * k;
        *g = l + (*g - l) * k;
        *b = l + (*b - l) * k;
    }
    *r = e_clamp01(*r);
    *g = e_clamp01(*g);
    *b = e_clamp01(*b);
}

static void e_set_lum(fdk_f32 *r, fdk_f32 *g, fdk_f32 *b, fdk_f32 l) {
    *r += l - e_lum(*r, *g, *b);
    *g += l - e_lum(*r, *g, *b);
    *b += l - e_lum(*r, *g, *b);
    e_clip_color(r, g, b);
}

static fdk_f32 e_sat(fdk_f32 r, fdk_f32 g, fdk_f32 b) {
    fdk_f32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    fdk_f32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    return mx - mn;
}

/* SetSat: the channel ordering matters (the W3C spec's Cmin/Cmid/Cmax
 * procedure reassigns by rank, keeping WHICH channel was which). */
static void e_set_sat(fdk_f32 *r, fdk_f32 *g, fdk_f32 *b, fdk_f32 s) {
    fdk_f32 *cmn, *cmid, *cmx;
    /* rank the three channels */
    if (*r <= *g) {
        if (*g <= *b) {
            cmn = r;
            cmid = g;
            cmx = b;
        } else if (*r <= *b) {
            cmn = r;
            cmid = b;
            cmx = g;
        } else {
            cmn = b;
            cmid = r;
            cmx = g;
        }
    } else {
        if (*r <= *b) {
            cmn = g;
            cmid = r;
            cmx = b;
        } else if (*g <= *b) {
            cmn = g;
            cmid = b;
            cmx = r;
        } else {
            cmn = b;
            cmid = g;
            cmx = r;
        }
    }
    if (*cmx > *cmn) {
        *cmid = (*cmid - *cmn) * s / (*cmx - *cmn);
        *cmx = s;
    } else {
        *cmid = 0;
        *cmx = 0;
    }
    *cmn = 0;
}

/* One blend-mode application over full buffers: dst = dst (backdrop)
 * ⊙mode src. Straight RGBA in/out. The Porter-Duff set uses each
 * mode's own alpha/color equations (the W3C compositing spec's
 * table); the blend set (screen..HSL) uses the unified form
 *   αo = αs + αb(1-αs)
 *   Co = [(1-αb)αsCs + (1-αs)αbCb + αsαb·B(Cb,Cs)] / αo
 * with B the (unpremultiplied) blend function. */
static void e_composite(fdk_u8 *dst, const fdk_u8 *src, int w, int h,
                        fdk_u16 mode) {
    size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const fdk_u8 *s = src + i * 4u;
        fdk_u8 *d = dst + i * 4u;
        fdk_f32 as = (fdk_f32)s[3] / 255.0f;
        fdk_f32 ad = (fdk_f32)d[3] / 255.0f;
        fdk_f32 cs[3] = {(fdk_f32)s[0] / 255.0f, (fdk_f32)s[1] / 255.0f,
                         (fdk_f32)s[2] / 255.0f};
        fdk_f32 cb[3] = {(fdk_f32)d[0] / 255.0f, (fdk_f32)d[1] / 255.0f,
                         (fdk_f32)d[2] / 255.0f};
        fdk_f32 ao = 0.0f;
        fdk_f32 co[3] = {0, 0, 0};

        switch (mode) {
        case 0: /* Clear */
            d[0] = d[1] = d[2] = d[3] = 0;
            continue;
        case 1: /* Src */
            memcpy(d, s, 4);
            continue;
        case 2: /* Dest */
            continue;
        case 3: /* SrcOver */
            ao = as + ad * (1.0f - as);
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = (as * cs[c] + ad * cb[c] * (1.0f - as)) / ao;
                }
            }
            break;
        case 4: /* DestOver */
            ao = as * (1.0f - ad) + ad;
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = (as * cs[c] * (1.0f - ad) + ad * cb[c]) / ao;
                }
            }
            break;
        case 5: /* SrcIn */
            ao = as * ad;
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = cs[c];
                }
            }
            break;
        case 6: /* DestIn */
            ao = as * ad;
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = cb[c];
                }
            }
            break;
        case 7: /* SrcOut */
            ao = as * (1.0f - ad);
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = cs[c];
                }
            }
            break;
        case 8: /* DestOut */
            ao = ad * (1.0f - as);
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = cb[c];
                }
            }
            break;
        case 9: /* SrcAtop */
            ao = ad;
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = as * cs[c] + (1.0f - as) * cb[c];
                }
            }
            break;
        case 10: /* DestAtop */
            ao = as;
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = (1.0f - ad) * cs[c] + ad * cb[c];
                }
            }
            break;
        case 11: /* Xor */
            ao = as * (1.0f - ad) + ad * (1.0f - as);
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = (as * (1.0f - ad) * cs[c] +
                             ad * (1.0f - as) * cb[c]) / ao;
                }
            }
            break;
        case 12: /* Plus */
            ao = as + ad;
            if (ao > 1.0f) {
                ao = 1.0f;
            }
            if (ao > 0) {
                for (int c = 0; c < 3; c++) {
                    co[c] = (as * cs[c] + ad * cb[c]) / ao;
                }
            }
            break;
        default: { /* 13..27: the blend set */
            ao = as + ad * (1.0f - as);
            if (ao <= 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
                continue;
            }
            for (int c = 0; c < 3; c++) {
                fdk_f32 base = (1.0f - ad) * as * cs[c] +
                               (1.0f - as) * ad * cb[c];
                fdk_f32 b = 0;
                switch (mode) {
                case 13: b = e_sep_screen(cb[c], cs[c]); break;
                case 14: b = e_sep_overlay(cb[c], cs[c]); break;
                case 15: b = cb[c] < cs[c] ? cb[c] : cs[c]; break;
                case 16: b = cb[c] > cs[c] ? cb[c] : cs[c]; break;
                case 17: b = e_sep_dodge(cb[c], cs[c]); break;
                case 18: b = e_sep_burn(cb[c], cs[c]); break;
                case 19: b = e_sep_hard_light(cb[c], cs[c]); break;
                case 20: b = e_sep_soft_light(cb[c], cs[c]); break;
                case 21: b = fabsf(cb[c] - cs[c]); break;
                case 22: b = cb[c] + cs[c] - 2.0f * cb[c] * cs[c]; break;
                case 23: b = cb[c] * cs[c]; break;
                default: b = cs[c]; /* replaced below for HSL */
                }
                co[c] = (base + as * ad * b) / ao;
            }
            if (mode >= 24) {
                /* Non-separable: B operates on the whole RGB triple. */
                fdk_f32 ur = 0, ug = 0, ub = 0;
                switch (mode) {
                case 24: { /* HSL_Hue */
                    fdk_f32 sat_b = e_sat(cb[0], cb[1], cb[2]);
                    fdk_f32 r = cs[0], g = cs[1], bb = cs[2];
                    e_set_sat(&r, &g, &bb, sat_b);
                    e_set_lum(&r, &g, &bb, e_lum(cb[0], cb[1], cb[2]));
                    ur = r;
                    ug = g;
                    ub = bb;
                    break;
                }
                case 25: { /* HSL_Saturation */
                    fdk_f32 sat_s = e_sat(cs[0], cs[1], cs[2]);
                    fdk_f32 r = cb[0], g = cb[1], bb = cb[2];
                    e_set_sat(&r, &g, &bb, sat_s);
                    e_set_lum(&r, &g, &bb, e_lum(cb[0], cb[1], cb[2]));
                    ur = r;
                    ug = g;
                    ub = bb;
                    break;
                }
                case 26: { /* HSL_Color */
                    fdk_f32 sat_b = e_sat(cb[0], cb[1], cb[2]);
                    fdk_f32 r = cs[0], g = cs[1], bb = cs[2];
                    e_set_sat(&r, &g, &bb, sat_b);
                    e_set_lum(&r, &g, &bb, e_lum(cb[0], cb[1], cb[2]));
                    ur = r;
                    ug = g;
                    ub = bb;
                    break;
                }
                default: { /* 27 HSL_Luminosity */
                    fdk_f32 r = cb[0], g = cb[1], bb = cb[2];
                    e_set_lum(&r, &g, &bb, e_lum(cs[0], cs[1], cs[2]));
                    ur = r;
                    ug = g;
                    ub = bb;
                    break;
                }
                }
                fdk_f32 br[3] = {ur, ug, ub};
                for (int c = 0; c < 3; c++) {
                    fdk_f32 base = (1.0f - ad) * as * cs[c] +
                                   (1.0f - as) * ad * cb[c];
                    co[c] = (base + as * ad * br[c]) / ao;
                }
            }
            break;
        }
        }

        d[0] = (fdk_u8)(e_clamp01(co[0]) * 255.0f + 0.5f);
        d[1] = (fdk_u8)(e_clamp01(co[1]) * 255.0f + 0.5f);
        d[2] = (fdk_u8)(e_clamp01(co[2]) * 255.0f + 0.5f);
        d[3] = (fdk_u8)(e_clamp01(ao) * 255.0f + 0.5f);
    }
}

/* ---- 8d. Gradient fills ------------------------------------------------ */

/* Fills the whole buffer with a linear gradient. The geometry (P0,
 * P1, P2) is in the PAINT's own font-unit space; each canvas pixel
 * maps back through m^-1 (so transforms apply to the gradient, not
 * the buffer). t is the projection of the point onto the P0->P1
 * basis measured parallel to P0->P2 (the spec's skewed gradient):
 * solve p - P0 = s*(P1-P0) + u*(P2-P0); s = cross(v1, p-P0) /
 * cross(v1, v0). */
static void e_fill_linear(e_ctx *ctx, fdk_u8 *buf, const e_mat *m,
                          const e_colorline *cl, fdk_f32 x0, fdk_f32 y0,
                          fdk_f32 x1, fdk_f32 y1, fdk_f32 x2, fdk_f32 y2) {
    e_mat inv;
    if (!e_mat_invert(m, &inv)) {
        memset(buf, 0, (size_t)ctx->w * (size_t)ctx->h * 4u);
        return;
    }
    fdk_f32 v0x = x1 - x0, v0y = y1 - y0;
    fdk_f32 v1x = x2 - x0, v1y = y2 - y0;
    fdk_f32 denom = v1x * v0y - v1y * v0x;
    if (fabsf(denom) < 1e-12f) {
        /* Degenerate line: the whole fill collapses to one stop. */
        fdk_u8 rgba[4];
        e_colorline_at(cl, 1.0f, rgba);
        for (size_t i = 0; i < (size_t)ctx->w * (size_t)ctx->h; i++) {
            memcpy(buf + i * 4u, rgba, 4);
        }
        return;
    }
    for (int py = 0; py < ctx->h; py++) {
        for (int px = 0; px < ctx->w; px++) {
            fdk_f32 fx, fy;
            e_mat_apply(&inv, (fdk_f32)px + 0.5f, (fdk_f32)py + 0.5f,
                        &fx, &fy);
            fdk_f32 dx = fx - x0, dy = fy - y0;
            fdk_f32 s = (v1x * dy - v1y * dx) / denom;
            fdk_u8 rgba[4];
            e_colorline_at(cl, s, rgba);
            fdk_u8 *q = buf + ((size_t)py * (size_t)ctx->w + (size_t)px) * 4u;
            memcpy(q, rgba, 4);
        }
    }
}

/* Radial: t solves |p - (c0 + t*(c1-c0))| = r0 + t*(r1-r0) — the
 * two-point conical family the spec defines. The quadratic's smaller
 * root is the gradient position (Skia's two-point conical discipline);
 * degenerate r0==r1 reduces to the linear projection. */
static void e_fill_radial(e_ctx *ctx, fdk_u8 *buf, const e_mat *m,
                          const e_colorline *cl, fdk_f32 x0, fdk_f32 y0,
                          fdk_f32 r0, fdk_f32 x1, fdk_f32 y1,
                          fdk_f32 r1) {
    e_mat inv;
    if (!e_mat_invert(m, &inv)) {
        memset(buf, 0, (size_t)ctx->w * (size_t)ctx->h * 4u);
        return;
    }
    fdk_f32 dx = x1 - x0, dy = y1 - y0;
    fdk_f32 dr = r1 - r0;
    fdk_f32 dd = dx * dx + dy * dy - dr * dr; /* quadratic A */
    for (int py = 0; py < ctx->h; py++) {
        for (int px = 0; px < ctx->w; px++) {
            fdk_f32 fx, fy;
            e_mat_apply(&inv, (fdk_f32)px + 0.5f, (fdk_f32)py + 0.5f,
                        &fx, &fy);
            fdk_f32 t;
            fdk_f32 px0 = fx - x0, py0 = fy - y0;
            if (fabsf(dr) < 1e-6f && fabsf(dd) < 1e-6f) {
                /* Concentric same-radius: constant. */
                t = 0.0f;
            } else if (fabsf(dr) < 1e-6f) {
                /* Equal radii: project onto the center line. */
                fdk_f32 len = dx * dx + dy * dy;
                t = len > 0 ? (px0 * dx + py0 * dy) / len : 0.0f;
            } else {
                fdk_f32 B = px0 * dx + py0 * dy - r0 * dr; /* -half? */
                fdk_f32 C = px0 * px0 + py0 * py0 - r0 * r0;
                /* Solve dd*t^2 + 2*B*t + C = 0 (the circle family). */
                if (fabsf(dd) < 1e-12f) {
                    t = -C / (2.0f * B);
                } else {
                    fdk_f32 disc = B * B - dd * C;
                    if (disc < 0) {
                        t = -B / dd; /* outside every circle: nearest */
                    } else {
                        fdk_f32 sq = sqrtf(disc);
                        fdk_f32 t1 = (-B - sq) / dd;
                        fdk_f32 t2 = (-B + sq) / dd;
                        t = t1; /* the smaller root */
                        (void)t2;
                    }
                }
            }
            fdk_u8 rgba[4];
            e_colorline_at(cl, t, rgba);
            fdk_u8 *q = buf + ((size_t)py * (size_t)ctx->w + (size_t)px) * 4u;
            memcpy(q, rgba, 4);
        }
    }
}

/* Sweep: t from the angle around the center, normalized to the
 * start..end angular range (the spec's CW-positive-in-y-up space;
 * canvas y is flipped only inside m). Angles in degrees, encoded as
 * BiasedAngle on disk (handled by the caller). */
static void e_fill_sweep(e_ctx *ctx, fdk_u8 *buf, const e_mat *m,
                         const e_colorline *cl, fdk_f32 cx, fdk_f32 cy,
                         fdk_f32 start_deg, fdk_f32 end_deg) {
    e_mat inv;
    if (!e_mat_invert(m, &inv)) {
        memset(buf, 0, (size_t)ctx->w * (size_t)ctx->h * 4u);
        return;
    }
    fdk_f32 span = end_deg - start_deg;
    if (fabsf(span) < 1e-6f) {
        /* Degenerate sweep: constant at the last stop's edge. */
        fdk_u8 rgba[4];
        e_colorline_at(cl, 1.0f, rgba);
        for (size_t i = 0; i < (size_t)ctx->w * (size_t)ctx->h; i++) {
            memcpy(buf + i * 4u, rgba, 4);
        }
        return;
    }
    for (int py = 0; py < ctx->h; py++) {
        for (int px = 0; px < ctx->w; px++) {
            fdk_f32 fx, fy;
            e_mat_apply(&inv, (fdk_f32)px + 0.5f, (fdk_f32)py + 0.5f,
                        &fx, &fy);
            fdk_f32 ang =
                atan2f(fy - cy, fx - cx) * (180.0f / E_PI);
            fdk_f32 t = (ang - start_deg) / span;
            fdk_u8 rgba[4];
            e_colorline_at(cl, t, rgba);
            fdk_u8 *q = buf + ((size_t)py * (size_t)ctx->w + (size_t)px) * 4u;
            memcpy(q, rgba, 4);
        }
    }
}

/* ---- 8e. Coverage rasterization ---------------------------------------- */

/* Rasterizes a glyph outline to a canvas-sized coverage mask under
 * the accumulated transform m.
 *
 * Axis-aligned m (the overwhelming case: the root scale, maybe
 * translations): the EXISTING stbtt_Rasterize path with the same
 * box arithmetic the mono glyph path uses — color and mono coverage
 * are pixel-identical by construction.
 *
 * General affine m (rotate/skew paints): a supersampled scanline
 * rasterizer — 16 sub-scanlines per pixel row, exact fractional
 * horizontal coverage per sub-scanline, nonzero winding. Straight
 * edges make vertical coverage converge; 16x sub-sampling is
 * visually indistinguishable from stb's own 5-bit precision.
 *
 * Returns false on allocation failure or a degenerate outline. */
static bool e_glyph_coverage(e_ctx *ctx, int glyph, const e_mat *m,
                             fdk_u8 *mask) {
    /* Shape the outline: stb for glyf faces, the native interpreter
     * for CFF (the same split text.c's glyph path makes). */
    stbtt_vertex *verts = NULL;
    int nv = 0;
    if (ctx->cff != NULL) {
        nv = fdk__cff_glyph_shape(ctx->cff, glyph, &verts);
    } else {
        nv = stbtt_GetGlyphShape(ctx->info, glyph, &verts);
    }
    if (nv <= 0 || verts == NULL) {
        fdk_free(verts);
        return false; /* no ink */
    }

    bool axis_aligned = fabsf(m->b) < 1e-6f && fabsf(m->c) < 1e-6f &&
                        m->a > 0.0f && fabsf(m->d) > 1e-9f;
    bool ok = false;
    if (axis_aligned) {
        /* stb path. Coordinate contract (pinned against stb's own
         * GetGlyphBitmapSubpixel):
         *   bitmap_x = fx*scale_x + shift_x - off_x
         *   bitmap_y = -fy*scale_y + shift_y - off_y   (invert = 1)
         * We need canvas coords cx = a*fx + e, cy = d*fy + f — so
         * scale_x = a, shift_x = e, off_x = the box floor; for d < 0
         * (the normal root flip) scale_y = -d, shift_y = f, invert =
         * 1; for d > 0 scale_y = d, invert = 0. Mirrored x (a < 0)
         * routes through the affine path below. */
        int gx0 = verts[0].x, gy0 = verts[0].y;
        int gx1 = gx0, gy1 = gy0;
        for (int i = 0; i < nv; i++) {
            if (verts[i].x < gx0) gx0 = verts[i].x;
            if (verts[i].x > gx1) gx1 = verts[i].x;
            if (verts[i].y < gy0) gy0 = verts[i].y;
            if (verts[i].y > gy1) gy1 = verts[i].y;
        }
        fdk_f32 cx0 = (fdk_f32)gx0 * m->a + m->e;
        fdk_f32 cx1 = (fdk_f32)gx1 * m->a + m->e;
        fdk_f32 cy0 = m->d < 0 ? (fdk_f32)gy1 * m->d + m->f
                               : (fdk_f32)gy0 * m->d + m->f;
        fdk_f32 cy1 = m->d < 0 ? (fdk_f32)gy0 * m->d + m->f
                               : (fdk_f32)gy1 * m->d + m->f;
        int px0 = (int)floorf(cx0);
        int py0 = (int)floorf(cy0);
        int px1 = (int)ceilf(cx1);
        int py1 = (int)ceilf(cy1);
        int gw = px1 - px0;
        int gh = py1 - py0;
        ok = true;
        if (gw > 0 && gh > 0 && gw <= ctx->w && gh <= ctx->h) {
            fdk_u8 *bits = fdk_alloc((size_t)gw * (size_t)gh);
            if (bits != NULL) {
                stbtt__bitmap gbm;
                gbm.w = gw;
                gbm.h = gh;
                gbm.stride = gw;
                gbm.pixels = bits;
                int invert = m->d < 0 ? 1 : 0;
                fdk_f32 scale_y = invert ? -m->d : m->d;
                stbtt_Rasterize(&gbm, 0.35f, verts, nv, m->a, scale_y,
                                m->e, m->f, px0, py0, invert, NULL);
                for (int y = 0; y < gh; y++) {
                    int cy = y + py0;
                    if (cy < 0 || cy >= ctx->h) {
                        continue;
                    }
                    for (int x = 0; x < gw; x++) {
                        int cx = x + px0;
                        if (cx < 0 || cx >= ctx->w) {
                            continue;
                        }
                        mask[(size_t)cy * (size_t)ctx->w + (size_t)cx] =
                            bits[(size_t)y * (size_t)gw + (size_t)x];
                    }
                }
                fdk_free(bits);
            } else {
                ok = false;
            }
        }
        /* A glyph box falling entirely off-canvas or degenerate
         * leaves the mask zeroed — PaintGlyph clips to nothing,
         * which is the honest answer. */
        } else {
        /* General affine: supersampled nonzero-winding scanlines. */
        typedef struct {
            fdk_f32 x0, y0, x1, y1;
        } e_edge;
        /* Curves flatten to <= 8 segments, so the worst edge count is
         * nv + nv/2ish — allocate nv*9/8 + 2 and bound the walk. */
        size_t max_edges = (size_t)nv * 9u / 8u + 4u;
        e_edge *edges = fdk_alloc(sizeof(e_edge) * max_edges);
        fdk_f32 *xbuf = fdk_alloc(sizeof(fdk_f32) * max_edges);
        int *xdir = fdk_alloc(sizeof(int) * max_edges);
        fdk_f32 *cov = fdk_alloc(sizeof(fdk_f32) * (size_t)ctx->w);
        if (edges == NULL || xbuf == NULL || xdir == NULL || cov == NULL) {
            fdk_free(edges);
            fdk_free(xbuf);
            fdk_free(xdir);
            fdk_free(cov);
            fdk_free(verts);
            return false;
        }
        size_t ne = 0;
        fdk_f32 cur_x = 0, cur_y = 0;
        fdk_f32 start_x = 0, start_y = 0;
        bool have_prev = false;
        for (int i = 0; i < nv; i++) {
            switch (verts[i].type) {
            case STBTT_vmove:
                cur_x = (fdk_f32)verts[i].x;
                cur_y = (fdk_f32)verts[i].y;
                start_x = cur_x;
                start_y = cur_y;
                /* The move point IS the previous point: the first
                 * segment after it connects from it (dropping it
                 * loses the contour's first edge — a rectangle would
                 * lose its bottom side and the winding collapses). */
                have_prev = true;
                break;
            case STBTT_vline: {
                fdk_f32 nx = (fdk_f32)verts[i].x;
                fdk_f32 ny = (fdk_f32)verts[i].y;
                if (have_prev && ne < max_edges) {
                    e_mat_apply(m, cur_x, cur_y, &edges[ne].x0,
                                &edges[ne].y0);
                    e_mat_apply(m, nx, ny, &edges[ne].x1,
                                &edges[ne].y1);
                    ne++;
                }
                cur_x = nx;
                cur_y = ny;
                have_prev = true;
                break;
            }
            case STBTT_vcurve:
            case STBTT_vcubic: {
                fdk_f32 x_from = cur_x, y_from = cur_y;
                fdk_f32 c1x = (fdk_f32)verts[i].cx;
                fdk_f32 c1y = (fdk_f32)verts[i].cy;
                fdk_f32 c2x = (fdk_f32)verts[i].cx1;
                fdk_f32 c2y = (fdk_f32)verts[i].cy1;
                fdk_f32 x_to = (fdk_f32)verts[i].x;
                fdk_f32 y_to = (fdk_f32)verts[i].y;
                int steps = 8;
                fdk_f32 fx = x_from, fy = y_from;
                for (int st = 1; st <= steps; st++) {
                    fdk_f32 u = (fdk_f32)st / (fdk_f32)steps;
                    fdk_f32 mu = 1.0f - u;
                    fdk_f32 wx, wy;
                    if (verts[i].type == STBTT_vcurve) {
                        wx = mu * mu * x_from + 2.0f * mu * u * c1x +
                             u * u * x_to;
                        wy = mu * mu * y_from + 2.0f * mu * u * c1y +
                             u * u * y_to;
                    } else {
                        wx = mu * mu * mu * x_from +
                             3.0f * mu * mu * u * c1x +
                             3.0f * mu * u * u * c2x + u * u * u * x_to;
                        wy = mu * mu * mu * y_from +
                             3.0f * mu * mu * u * c1y +
                             3.0f * mu * u * u * c2y + u * u * u * y_to;
                    }
                    if (ne < max_edges) {
                        e_mat_apply(m, fx, fy, &edges[ne].x0,
                                    &edges[ne].y0);
                        e_mat_apply(m, wx, wy, &edges[ne].x1,
                                    &edges[ne].y1);
                        ne++;
                    }
                    fx = wx;
                    fy = wy;
                }
                cur_x = x_to;
                cur_y = y_to;
                have_prev = true;
                break;
            }
            default:
                break;
            }
        }
        /* Close the final contour. */
        if (have_prev && ne < max_edges) {
            e_mat_apply(m, cur_x, cur_y, &edges[ne].x0, &edges[ne].y0);
            e_mat_apply(m, start_x, start_y, &edges[ne].x1,
                        &edges[ne].y1);
            ne++;
        }

        /* 16 sub-scanlines per pixel row; per-sub-scanline horizontal
         * coverage is exact (fractional at both span ends). */
        const int SS = 16;
        for (int row = 0; row < ctx->h; row++) {
            memset(cov, 0, sizeof(fdk_f32) * (size_t)ctx->w);
            for (int sub = 0; sub < SS; sub++) {
                fdk_f32 sy = (fdk_f32)row +
                             ((fdk_f32)sub + 0.5f) / (fdk_f32)SS;
                int nx = 0;
                for (size_t e = 0; e < ne && nx < (int)max_edges; e++) {
                    fdk_f32 y0 = edges[e].y0, y1 = edges[e].y1;
                    if (y0 == y1) {
                        continue; /* horizontal edges carry no winding */
                    }
                    /* Normalize: (xl, yl) is the LOWER endpoint,
                     * (xh, yh) the higher; the crossing x interpolates
                     * from the low endpoint with t in [0, 1). */
                    fdk_f32 xl, yl, xh, yh;
                    int dir;
                    if (y0 < y1) {
                        xl = edges[e].x0;
                        yl = y0;
                        xh = edges[e].x1;
                        yh = y1;
                        dir = 1; /* edge runs downward (canvas y+) */
                    } else {
                        xl = edges[e].x1;
                        yl = y1;
                        xh = edges[e].x0;
                        yh = y0;
                        dir = -1; /* edge runs upward */
                    }
                    if (sy < yl || sy >= yh) {
                        continue;
                    }
                    fdk_f32 t = (sy - yl) / (yh - yl);
                    fdk_f32 xs = xl + t * (xh - xl);
                    xbuf[nx] = xs;
                    xdir[nx] = dir;
                    nx++;
                }
                if (nx == 0) {
                    continue;
                }
                /* sort by x (insertion sort — nx is small) */
                for (int a = 1; a < nx; a++) {
                    fdk_f32 xv = xbuf[a];
                    int dv = xdir[a];
                    int b = a - 1;
                    while (b >= 0 && xbuf[b] > xv) {
                        xbuf[b + 1] = xbuf[b];
                        xdir[b + 1] = xdir[b];
                        b--;
                    }
                    xbuf[b + 1] = xv;
                    xdir[b + 1] = dv;
                }
                /* Nonzero-winding spans: the span LEFT of crossing
                 * a carries the winding accumulated over crossings
                 * [0, a); fill it iff that is nonzero. */
                int wind = 0;
                for (int a = 0; a < nx; a++) {
                    if (a > 0 && wind != 0 && xbuf[a - 1] < (fdk_f32)ctx->w &&
                        xbuf[a] > 0.0f) {
                        fdk_f32 xa = xbuf[a - 1];
                        fdk_f32 xb = xbuf[a];
                        int pxa = xa < 0 ? 0 : (int)xa;
                        int pxb = xb > (fdk_f32)ctx->w ? ctx->w : (int)xb;
                        for (int p = pxa; p < pxb; p++) {
                            fdk_f32 lo = (fdk_f32)p;
                            if (lo < xa) {
                                lo = xa;
                            }
                            fdk_f32 hi = (fdk_f32)(p + 1);
                            if (hi > xb) {
                                hi = xb;
                            }
                            fdk_f32 cover = hi - lo;
                            if (cover > 0) {
                                cov[p] += cover;
                            }
                        }
                    }
                    wind += xdir[a];
                }
            }
            for (int p = 0; p < ctx->w; p++) {
                fdk_f32 c = cov[p] / (fdk_f32)SS;
                if (c > 1.0f) {
                    c = 1.0f;
                }
                mask[(size_t)row * (size_t)ctx->w + (size_t)p] =
                    (fdk_u8)(c * 255.0f + 0.5f);
            }
        }
        fdk_free(edges);
        fdk_free(xbuf);
        fdk_free(xdir);
        fdk_free(cov);
        ok = true;
    }
    fdk_free(verts);
    return ok;
}

/* ---- 8f. The paint walk ------------------------------------------------ */

/* Reads one paint table at `off` (a FILE offset) and composites its
 * evaluation into `out` (a ctx-sized scratch or the root canvas)
 * under transform m. Depth-capped; allocation failures set
 * ctx->failed and quietly zero the contribution (a glyph is cached
 * once — a transient OOM must not take the process down). */
static void e_paint_eval(e_ctx *ctx, size_t off, const e_mat *m,
                         fdk_u8 *out, int depth) {
    const e_colr *c = ctx->colr;
    if (ctx->failed || depth >= E_PAINT_MAX_DEPTH ||
        off >= c->colr_end) {
        return;
    }
    const fdk_u8 *p = c->file + off;
    if (off + 3 > c->colr_end) {
        return;
    }
    fdk_u8 fmt = p[0];

    /* A helper to resolve an Offset24 (relative to the paint table's
     * own start) to a FILE offset, bounds-checked. */
#define E_SUB(field_off)                                                     \
    ((off + (size_t)(field_off) + 3 <= c->colr_end)                          \
         ? (off + (size_t)e_be24(p + (field_off)))                           \
         : (size_t)0)

    switch (fmt) {
    case 1: { /* PaintColrLayers */
        if (off + 6 > c->colr_end || c->layer_list == NULL) {
            return;
        }
        fdk_u8 num = p[1];
        fdk_u32 first = e_be32(p + 2);
        if (first + (fdk_u32)num > c->layer_list_count) {
            return;
        }
        fdk_u8 *tmp = e_scratch(ctx, depth + 1);
        if (tmp == NULL) {
            return;
        }
        /* `layer_list` points at the u32 offset ARRAY; the offsets
         * themselves resolve from the LayerList table's own start
         * (4 bytes earlier — the count), the base fontTools' writer
         * uses. */
        size_t ll_start = (size_t)(c->layer_list - c->file) - 4u;
        for (fdk_u32 i = 0; i < num; i++) {
            size_t lloff = (size_t)(c->layer_list - c->file) +
                           4u * (size_t)(first + i);
            if (lloff + 4 > c->colr_end) {
                break;
            }
            size_t rel = e_be32(c->file + lloff);
            size_t paint_off = ll_start + rel;
            if (paint_off >= c->colr_end) {
                continue;
            }
            e_paint_eval(ctx, paint_off, m, tmp, depth + 1);
            e_src_over(out, tmp, ctx->w, ctx->h);
            memset(tmp, 0, (size_t)ctx->w * (size_t)ctx->h * 4u);
        }
        return;
    }
    case 2:   /* PaintSolid */
    case 3: { /* PaintVarSolid (deltas skipped — default instance) */
        if (off + 5 > c->colr_end) {
            return;
        }
        fdk_u16 pal = e_be16(p + 1);
        fdk_f32 alpha = e_f2dot14(e_be16(p + 3));
        if (alpha < 0) {
            alpha = 0;
        }
        if (alpha > 1) {
            alpha = 1;
        }
        fdk_u8 rgba[4];
        e_palette_color(ctx->pal, pal, rgba);
        rgba[3] = (fdk_u8)((fdk_f32)rgba[3] * alpha + 0.5f);
        size_t n = (size_t)ctx->w * (size_t)ctx->h;
        for (size_t i = 0; i < n; i++) {
            memcpy(out + i * 4u, rgba, 4);
        }
        return;
    }
    case 4:  /* PaintLinearGradient */
    case 5: { /* PaintVarLinearGradient */
        if (off + 16 > c->colr_end) {
            return;
        }
        size_t cl_off = off + (size_t)e_be24(p + 1);
        e_colorline cl;
        if (!e_colorline_parse(ctx, cl_off, fmt == 5, &cl)) {
            return;
        }
        e_fill_linear(ctx, out, m, &cl, (fdk_f32)e_bei16(p + 4),
                      (fdk_f32)e_bei16(p + 6), (fdk_f32)e_bei16(p + 8),
                      (fdk_f32)e_bei16(p + 10), (fdk_f32)e_bei16(p + 12),
                      (fdk_f32)e_bei16(p + 14));
        return;
    }
    case 6:  /* PaintRadialGradient */
    case 7: { /* PaintVarRadialGradient */
        if (off + 16 > c->colr_end) {
            return;
        }
        size_t cl_off = off + (size_t)e_be24(p + 1);
        e_colorline cl;
        if (!e_colorline_parse(ctx, cl_off, fmt == 7, &cl)) {
            return;
        }
        e_fill_radial(ctx, out, m, &cl, (fdk_f32)e_bei16(p + 4),
                      (fdk_f32)e_bei16(p + 6), (fdk_f32)e_be16(p + 8),
                      (fdk_f32)e_bei16(p + 10), (fdk_f32)e_bei16(p + 12),
                      (fdk_f32)e_be16(p + 14));
        return;
    }
    case 8:  /* PaintSweepGradient */
    case 9: { /* PaintVarSweepGradient */
        if (off + 12 > c->colr_end) {
            return;
        }
        size_t cl_off = off + (size_t)e_be24(p + 1);
        e_colorline cl;
        if (!e_colorline_parse(ctx, cl_off, fmt == 9, &cl)) {
            return;
        }
        e_fill_sweep(ctx, out, m, &cl, (fdk_f32)e_bei16(p + 4),
                     (fdk_f32)e_bei16(p + 6),
                     e_biased_angle_deg(e_be16(p + 8)),
                     e_biased_angle_deg(e_be16(p + 10)));
        return;
    }
    case 10: { /* PaintGlyph */
        if (off + 7 > c->colr_end) {
            fprintf(stderr, "DBG PaintGlyph: off+7 > colr_end\n");
            return;
        }
        size_t child = E_SUB(1);
        int glyph = (int)e_be16(p + 4);
        if (child == 0 || child >= c->colr_end) {
            return;
        }
        fdk_u8 *mask = e_mask_scratch(ctx, depth);
        fdk_u8 *tmp = e_scratch(ctx, depth + 1);
        if (mask == NULL || tmp == NULL) {
            return;
        }
        if (e_glyph_coverage(ctx, glyph, m, mask)) {
            e_paint_eval(ctx, child, m, tmp, depth + 1);
            e_mask_alpha(tmp, mask, ctx->w, ctx->h);
            e_src_over(out, tmp, ctx->w, ctx->h);
        }
        return;
    }
    case 11: { /* PaintColrGlyph */
        if (off + 3 > c->colr_end) {
            return;
        }
        int glyph = (int)e_be16(p + 1);
        /* Cycle guard: a graph that reaches its own root through
         * PaintColrGlyph would recurse forever on a hostile font. */
        for (int i = 0; i < ctx->n_visiting; i++) {
            if (ctx->visiting[i] == glyph) {
                return;
            }
        }
        size_t root = 0;
        if (!e_colr_v1_lookup(c, glyph, &root)) {
            return;
        }
        if (ctx->n_visiting < E_COLR_CYCLE_MAX) {
            ctx->visiting[ctx->n_visiting++] = glyph;
        }
        e_paint_eval(ctx, root, m, out, depth + 1);
        if (ctx->n_visiting > 0) {
            ctx->n_visiting--;
        }
        return;
    }
    case 12:  /* PaintTransform */
    case 13: { /* PaintVarTransform */
        if (off + 7 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        size_t t_off = E_SUB(4);
        if (child == 0 || t_off == 0 || t_off + 24 > c->colr_end) {
            return;
        }
        const fdk_u8 *t = c->file + t_off;
        e_mat tm;
        tm.a = e_fixed(e_be32(t));
        tm.b = e_fixed(e_be32(t + 4));
        tm.c = e_fixed(e_be32(t + 8));
        tm.d = e_fixed(e_be32(t + 12));
        tm.e = e_fixed(e_be32(t + 16));
        tm.f = e_fixed(e_be32(t + 20));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 14:  /* PaintTranslate */
    case 15: { /* PaintVarTranslate */
        if (off + 9 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat tm = e_mat_translate((fdk_f32)e_bei16(p + 4),
                                   (fdk_f32)e_bei16(p + 6));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 16:  /* PaintScale */
    case 17: { /* PaintVarScale */
        if (off + 8 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat tm = e_mat_scale(e_f2dot14(e_be16(p + 4)),
                               e_f2dot14(e_be16(p + 6)));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 18:  /* PaintScaleAroundCenter */
    case 19: { /* PaintVarScaleAroundCenter */
        if (off + 12 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat inner = e_mat_scale(e_f2dot14(e_be16(p + 4)),
                                  e_f2dot14(e_be16(p + 6)));
        e_mat tm = e_center_op(&inner, (fdk_f32)e_bei16(p + 8),
                               (fdk_f32)e_bei16(p + 10));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 20:  /* PaintScaleUniform */
    case 21: { /* PaintVarScaleUniform */
        if (off + 6 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        fdk_f32 s = e_f2dot14(e_be16(p + 4));
        e_mat tm = e_mat_scale(s, s);
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 22:  /* PaintScaleUniformAroundCenter */
    case 23: { /* PaintVarScaleUniformAroundCenter */
        if (off + 10 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        fdk_f32 s = e_f2dot14(e_be16(p + 4));
        e_mat inner = e_mat_scale(s, s);
        e_mat tm = e_center_op(&inner, (fdk_f32)e_bei16(p + 6),
                               (fdk_f32)e_bei16(p + 8));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 24:  /* PaintRotate */
    case 25: { /* PaintVarRotate */
        if (off + 6 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat tm = e_mat_rotate(e_angle_deg(e_be16(p + 4)) *
                                (E_PI / 180.0f));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 26:  /* PaintRotateAroundCenter */
    case 27: { /* PaintVarRotateAroundCenter */
        if (off + 10 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat rot = e_mat_rotate(e_angle_deg(e_be16(p + 4)) *
                                 (E_PI / 180.0f));
        e_mat tm = e_center_op(&rot, (fdk_f32)e_bei16(p + 6),
                               (fdk_f32)e_bei16(p + 8));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 28:  /* PaintSkew */
    case 29: { /* PaintVarSkew */
        if (off + 8 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        /* Pinned against fontTools' getTransform: the matrix is
         * skew(radians(-xSkew), radians(ySkew)) in fontTools terms,
         * which in this struct is b = tan(ySkew), c = tan(-xSkew). */
        e_mat tm = {1.0f, tanf(e_angle_deg(e_be16(p + 6)) *
                               (E_PI / 180.0f)),
                    tanf(-e_angle_deg(e_be16(p + 4)) *
                         (E_PI / 180.0f)),
                    1.0f, 0.0f, 0.0f};
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 30:  /* PaintSkewAroundCenter */
    case 31: { /* PaintVarSkewAroundCenter */
        if (off + 12 > c->colr_end) {
            return;
        }
        size_t child = E_SUB(1);
        if (child == 0) {
            return;
        }
        e_mat sk = {1.0f, tanf(e_angle_deg(e_be16(p + 6)) *
                               (E_PI / 180.0f)),
                    tanf(-e_angle_deg(e_be16(p + 4)) *
                         (E_PI / 180.0f)),
                    1.0f, 0.0f, 0.0f};
        e_mat tm = e_center_op(&sk, (fdk_f32)e_bei16(p + 8),
                               (fdk_f32)e_bei16(p + 10));
        e_mat composed = e_mat_mul(m, &tm);
        e_paint_eval(ctx, child, &composed, out, depth + 1);
        return;
    }
    case 32: { /* PaintComposite */
        if (off + 8 > c->colr_end) {
            return;
        }
        size_t source = E_SUB(1);
        fdk_u16 mode = p[4];
        size_t backdrop = E_SUB(5);
        if (source == 0 || backdrop == 0) {
            return;
        }
        /* Backdrop goes into out first, then source composites onto
         * it with the mode. */
        e_paint_eval(ctx, backdrop, m, out, depth + 1);
        fdk_u8 *tmp = e_scratch(ctx, depth + 1);
        if (tmp == NULL) {
            return;
        }
        e_paint_eval(ctx, source, m, tmp, depth + 1);
        e_composite(out, tmp, ctx->w, ctx->h, mode);
        return;
    }
    default:
        return; /* unknown format: no contribution */
    }
#undef E_SUB
}

/* ---- 8g. v1 bounds + render entry -------------------------------------- */

/* The paint-graph ink-box walk (no ClipList): the union of every
 * PaintGlyph's glyph box under the accumulated transform — the same
 * discipline fontTools' computeClipBox uses to author clip boxes. */
static void e_paint_bounds(const e_colr *c, const stbtt_fontinfo *info,
                           fdk_cff_font *cff, size_t off, const e_mat *m,
                           fdk_f32 *bx0, fdk_f32 *by0, fdk_f32 *bx1,
                           fdk_f32 *by1, int depth, int *any,
                           int *visiting, int n_visiting) {
    if (depth >= E_PAINT_MAX_DEPTH || off >= c->colr_end ||
        off + 3 > c->colr_end) {
        return;
    }
    const fdk_u8 *p = c->file + off;
    fdk_u8 fmt = p[0];

    switch (fmt) {
    case 1: { /* PaintColrLayers */
        if (off + 6 > c->colr_end || c->layer_list == NULL) {
            return;
        }
        fdk_u8 num = p[1];
        fdk_u32 first = e_be32(p + 2);
        if (first + (fdk_u32)num > c->layer_list_count) {
            return;
        }
        size_t ll_start = (size_t)(c->layer_list - c->file) - 4u;
        for (fdk_u32 i = 0; i < num; i++) {
            size_t lloff = (size_t)(c->layer_list - c->file) +
                           4u * (size_t)(first + i);
            if (lloff + 4 > c->colr_end) {
                break;
            }
            size_t paint_off = ll_start + e_be32(c->file + lloff);
            if (paint_off < c->colr_end) {
                e_paint_bounds(c, info, cff, paint_off, m, bx0, by0,
                               bx1, by1, depth + 1, any, visiting,
                               n_visiting);
            }
        }
        return;
    }
    case 10: { /* PaintGlyph */
        if (off + 7 > c->colr_end) {
            return;
        }
        size_t child = off + (size_t)e_be24(p + 1);
        int glyph = (int)e_be16(p + 4);
        int gx0, gy0, gx1, gy1;
        bool have = cff != NULL
                        ? fdk__cff_glyph_box(cff, glyph, &gx0, &gy0,
                                             &gx1, &gy1)
                        : (stbtt_GetGlyphBox(info, glyph, &gx0, &gy0,
                                             &gx1, &gy1) != 0);
        if (have) {
            fdk_f32 corners[4][2] = {{(fdk_f32)gx0, (fdk_f32)gy0},
                                     {(fdk_f32)gx1, (fdk_f32)gy0},
                                     {(fdk_f32)gx0, (fdk_f32)gy1},
                                     {(fdk_f32)gx1, (fdk_f32)gy1}};
            for (int k = 0; k < 4; k++) {
                fdk_f32 tx, ty;
                e_mat_apply(m, corners[k][0], corners[k][1], &tx, &ty);
                if (!*any) {
                    *bx0 = *bx1 = tx;
                    *by0 = *by1 = ty;
                    *any = 1;
                } else {
                    if (tx < *bx0) *bx0 = tx;
                    if (tx > *bx1) *bx1 = tx;
                    if (ty < *by0) *by0 = ty;
                    if (ty > *by1) *by1 = ty;
                }
            }
        }
        if (child < c->colr_end) {
            e_paint_bounds(c, info, cff, child, m, bx0, by0, bx1, by1,
                           depth + 1, any, visiting, n_visiting);
        }
        return;
    }
    case 11: { /* PaintColrGlyph */
        if (off + 3 > c->colr_end) {
            return;
        }
        int glyph = (int)e_be16(p + 1);
        for (int i = 0; i < n_visiting; i++) {
            if (visiting[i] == glyph) {
                return;
            }
        }
        size_t root = 0;
        if (!e_colr_v1_lookup(c, glyph, &root)) {
            return;
        }
        if (n_visiting < E_COLR_CYCLE_MAX) {
            visiting[n_visiting++] = glyph;
        }
        e_paint_bounds(c, info, cff, root, m, bx0, by0, bx1, by1,
                       depth + 1, any, visiting, n_visiting);
        return;
    }
    default: {
        /* Transform-family paints: compose and recurse. The layouts
         * mirror e_paint_eval's (the E_SUB discipline). */
        size_t child = 0;
        e_mat tm = e_mat_identity();
        switch (fmt) {
        case 12:
        case 13: {
            if (off + 7 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            size_t t_off = off + (size_t)e_be24(p + 4);
            if (t_off + 24 > c->colr_end) {
                return;
            }
            const fdk_u8 *t = c->file + t_off;
            tm.a = e_fixed(e_be32(t));
            tm.b = e_fixed(e_be32(t + 4));
            tm.c = e_fixed(e_be32(t + 8));
            tm.d = e_fixed(e_be32(t + 12));
            tm.e = e_fixed(e_be32(t + 16));
            tm.f = e_fixed(e_be32(t + 20));
            break;
        }
        case 14:
        case 15: {
            if (off + 9 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            tm = e_mat_translate((fdk_f32)e_bei16(p + 4),
                                 (fdk_f32)e_bei16(p + 6));
            break;
        }
        case 16:
        case 17: {
            if (off + 8 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            tm = e_mat_scale(e_f2dot14(e_be16(p + 4)),
                             e_f2dot14(e_be16(p + 6)));
            break;
        }
        case 18:
        case 19: {
            if (off + 12 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            e_mat inner = e_mat_scale(e_f2dot14(e_be16(p + 4)),
                                      e_f2dot14(e_be16(p + 6)));
            tm = e_center_op(&inner, (fdk_f32)e_bei16(p + 8),
                             (fdk_f32)e_bei16(p + 10));
            break;
        }
        case 20:
        case 21: {
            if (off + 6 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            tm = e_mat_scale(e_f2dot14(e_be16(p + 4)),
                             e_f2dot14(e_be16(p + 4)));
            break;
        }
        case 22:
        case 23: {
            if (off + 10 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            fdk_f32 s = e_f2dot14(e_be16(p + 4));
            e_mat inner = e_mat_scale(s, s);
            tm = e_center_op(&inner, (fdk_f32)e_bei16(p + 6),
                             (fdk_f32)e_bei16(p + 8));
            break;
        }
        case 24:
        case 25: {
            if (off + 6 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            tm = e_mat_rotate(e_angle_deg(e_be16(p + 4)) *
                              (E_PI / 180.0f));
            break;
        }
        case 26:
        case 27: {
            if (off + 10 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            e_mat rot = e_mat_rotate(e_angle_deg(e_be16(p + 4)) *
                                     (E_PI / 180.0f));
            tm = e_center_op(&rot, (fdk_f32)e_bei16(p + 6),
                             (fdk_f32)e_bei16(p + 8));
            break;
        }
        case 28:
        case 29: {
            if (off + 8 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            tm.a = 1.0f;
            tm.b = tanf(e_angle_deg(e_be16(p + 6)) *
                        (E_PI / 180.0f));
            tm.c = tanf(-e_angle_deg(e_be16(p + 4)) *
                        (E_PI / 180.0f));
            tm.d = 1.0f;
            break;
        }
        case 30:
        case 31: {
            if (off + 12 > c->colr_end) {
                return;
            }
            child = off + (size_t)e_be24(p + 1);
            e_mat sk = {1.0f,
                        tanf(e_angle_deg(e_be16(p + 6)) *
                             (E_PI / 180.0f)),
                        tanf(-e_angle_deg(e_be16(p + 4)) *
                             (E_PI / 180.0f)),
                        1.0f, 0.0f, 0.0f};
            tm = e_center_op(&sk, (fdk_f32)e_bei16(p + 8),
                             (fdk_f32)e_bei16(p + 10));
            break;
        }
        case 32: { /* PaintComposite: both operands */
            if (off + 8 > c->colr_end) {
                return;
            }
            e_paint_bounds(c, info, cff, off + (size_t)e_be24(p + 1), m,
                           bx0, by0, bx1, by1, depth + 1, any, visiting,
                           n_visiting);
            e_paint_bounds(c, info, cff, off + (size_t)e_be24(p + 5), m,
                           bx0, by0, bx1, by1, depth + 1, any, visiting,
                           n_visiting);
            return;
        }
        default:
            return; /* fills contribute no geometry of their own */
        }
        e_mat composed = e_mat_mul(m, &tm);
        if (child != 0 && child < c->colr_end) {
            e_paint_bounds(c, info, cff, child, &composed, bx0, by0,
                           bx1, by1, depth + 1, any, visiting,
                           n_visiting);
        }
        return;
    }
    }
}

/* Renders one v1 color glyph into an RGBA bitmap + its box. */
static fdk_u8 *e_colr_v1_render(const e_colr *c, const e_cpal *pal,
                                const stbtt_fontinfo *info,
                                fdk_cff_font *cff, fdk_f32 scale,
                                int glyph, int *out_w, int *out_h,
                                fdk_i32 *out_xoff, fdk_i32 *out_yoff) {
    size_t root = 0;
    if (!e_colr_v1_lookup(c, glyph, &root)) {
        return NULL;
    }

    /* The ink box: the ClipList's authored box when present (the
     * author's declared drawing extent — FreeType clips to it), else
     * the graph walk's union of transformed PaintGlyph boxes. */
    fdk_f32 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    int any = 0;
    if (c->clip_list != NULL) {
        /* ClipList format 1: records sorted by glyph id, each a glyph
         * range + an Offset24 to a ClipBox (from the record start). */
        for (size_t i = 0; i < c->clip_count; i++) {
            const fdk_u8 *r = c->clip_list + 5 + 8 * i;
            fdk_u16 g0 = e_be16(r);
            fdk_u16 g1 = e_be16(r + 2);
            if (glyph >= (int)g0 && glyph <= (int)g1) {
                size_t box_off = (size_t)(r - c->file) + e_be24(r + 4);
                if (box_off + 9 <= c->colr_end &&
                    c->file[box_off] == 1) {
                    const fdk_u8 *b = c->file + box_off;
                    bx0 = (fdk_f32)e_bei16(b + 1);
                    by0 = (fdk_f32)e_bei16(b + 3);
                    bx1 = (fdk_f32)e_bei16(b + 5);
                    by1 = (fdk_f32)e_bei16(b + 7);
                    any = 1;
                }
                break;
            }
        }
    }
    if (!any) {
        e_mat m0 = e_mat_identity();
        int visiting[E_COLR_CYCLE_MAX];
        e_paint_bounds(c, info, cff, root, &m0, &bx0, &by0, &bx1, &by1,
                       0, &any, visiting, 0);
        /* The root walk runs in FONT units (identity m); scale into
         * pixels via the root transform below. A graph with no
         * PaintGlyph anywhere (a bare fill at the root) has no box —
         * the base glyph's own box is the fallback. */
        if (!any) {
            int gx0, gy0, gx1, gy1;
            bool have = cff != NULL
                            ? fdk__cff_glyph_box(cff, glyph, &gx0, &gy0,
                                                 &gx1, &gy1)
                            : (stbtt_GetGlyphBox(info, glyph, &gx0, &gy0,
                                                 &gx1, &gy1) != 0);
            if (!have) {
                return NULL;
            }
            bx0 = (fdk_f32)gx0;
            by0 = (fdk_f32)gy0;
            bx1 = (fdk_f32)gx1;
            by1 = (fdk_f32)gy1;
            any = 1;
        }
    }
    if (!any || bx1 <= bx0 || by1 <= by0) {
        return NULL;
    }

    /* Canvas: the box under the root transform (scale + y-flip),
     * floored/ceiled, with a 1px pad for AA edges. */
    fdk_f32 px0f = floorf(bx0 * scale) - 1.0f;
    fdk_f32 py0f = floorf(-by1 * scale) - 1.0f;
    fdk_f32 px1f = ceilf(bx1 * scale) + 1.0f;
    fdk_f32 py1f = ceilf(-by0 * scale) + 1.0f;
    if (px0f < -1.0e6f || py0f < -1.0e6f || px1f > 1.0e6f ||
        py1f > 1.0e6f) {
        return NULL; /* pathological transforms */
    }
    int w = (int)(px1f - px0f);
    int h = (int)(py1f - py0f);
    if (w <= 0 || h <= 0 || (size_t)w * (size_t)h > 16u * 1024u * 1024u) {
        return NULL;
    }

    fdk_u8 *canvas = fdk_alloc((size_t)w * (size_t)h * 4u);
    if (canvas == NULL) {
        return NULL;
    }
    memset(canvas, 0, (size_t)w * (size_t)h * 4u);

    e_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.colr = c;
    ctx.pal = pal;
    ctx.info = info;
    ctx.cff = cff;
    ctx.scale = scale;
    ctx.w = w;
    ctx.h = h;

    /* Root transform: font units (y-up) -> canvas pixels (y-down):
     * cx = fx*scale - px0f, cy = -fy*scale - py0f. */
    e_mat m0 = {scale, 0.0f, 0.0f, -scale, -px0f, -py0f};
    e_paint_eval(&ctx, root, &m0, canvas, 0);
    e_ctx_destroy(&ctx);
    if (ctx.failed) {
        fdk_free(canvas);
        return NULL;
    }

    *out_w = w;
    *out_h = h;
    *out_xoff = (fdk_i32)px0f;
    *out_yoff = (fdk_i32)py0f;
    return canvas;
}

/* ---- 9. CBDT / sbix rasterize ----------------------------------------- */

/* Decodes a PNG payload to a straight-RGBA8 buffer through stb_image
 * (the same decoder the clipboard's image/png path rides — one
 * decoder, one validation posture). Returns NULL on any failure. */
static fdk_u8 *e_decode_png(const fdk_u8 *data, size_t len, int *out_w,
                            int *out_h) {
    if (data == NULL || len == 0 || len > (512u << 20)) {
        return NULL;
    }
    int w = 0, h = 0, channels = 0;
    unsigned char *px = stbi_load_from_memory(
        (const stbi_uc *)data, (int)len, &w, &h, &channels, 4);
    if (px == NULL || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        stbi_image_free(px);
        return NULL;
    }
    fdk_u8 *buf = fdk_alloc((size_t)w * (size_t)h * 4u);
    if (buf == NULL) {
        stbi_image_free(px);
        return NULL;
    }
    /* stb with 4 requested channels delivers R,G,B,A byte order —
     * exactly this module's buffer format. */
    memcpy(buf, px, (size_t)w * (size_t)h * 4u);
    stbi_image_free(px);
    *out_w = w;
    *out_h = h;
    return buf;
}

/* One decoded bitmap-strike glyph: the RGBA pixels at strike
 * resolution, its placement (bearingX right of the pen, bearingY
 * above the baseline, strike pixels), and the strike's ppem. */
typedef struct {
    fdk_u8 *px;
    int w, h;
    fdk_i32 bearing_x, bearing_y; /* strike pixels               */
    fdk_u16 ppem;
} e_bitmap_glyph;

/* Chooses the strike whose ppem is closest in log-ratio to the
 * requested em-pixels — the same |log2| metric FreeType's bitmap
 * strike selection uses. */
static double e_strike_score(double want, double have) {
    return fabs(log(want / (double)have));
}

/* Decodes one CBDT glyph across all strikes (best strike wins).
 * Returns false when the glyph has no bitmap any strike can serve. */
static bool e_cbdt_render(const e_cbdt *cb, int glyph, double em_px,
                          e_bitmap_glyph *out) {
    memset(out, 0, sizeof *out);
    double best_score = 1.0e30;
    size_t best_strike = (size_t)-1;
    /* Which strikes serve this glyph at all? (Locate per strike.) */
    for (size_t s = 0; s < cb->n_strikes; s++) {
        const e_strike *k = &cb->strikes[s];
        if (glyph < (int)k->start_glyph || glyph > (int)k->end_glyph) {
            continue;
        }
        for (size_t i = k->sub_lo; i < k->sub_hi; i++) {
            e_located loc;
            if (e_sub_locate(cb, &cb->subs[i], glyph, &loc)) {
                double score =
                    e_strike_score(em_px, (double)k->ppem_x);
                if (score < best_score) {
                    best_score = score;
                    best_strike = s;
                }
                break; /* one subtable per strike serves it */
            }
        }
    }
    if (best_strike == (size_t)-1) {
        return false;
    }
    const e_strike *k = &cb->strikes[best_strike];
    e_located loc;
    bool found = false;
    for (size_t i = k->sub_lo; i < k->sub_hi && !found; i++) {
        found = e_sub_locate(cb, &cb->subs[i], glyph, &loc);
    }
    if (!found || loc.image_format < 17 || loc.image_format > 19) {
        return false;
    }

    const fdk_u8 *d = cb->file + loc.data_off;
    size_t avail = cb->file_size - loc.data_off;
    fdk_i32 bearing_x = 0, bearing_y = 0;
    const fdk_u8 *png = NULL;
    size_t png_len = 0;
    if (loc.image_format == 17) {
        if (avail < 9) {
            return false;
        }
        bearing_x = (fdk_i32)(fdk_i8)d[2];
        bearing_y = (fdk_i32)(fdk_i8)d[3];
        fdk_u32 data_len = e_be32(d + 5);
        if (data_len > avail - 9) {
            return false;
        }
        png = d + 9;
        png_len = data_len;
    } else if (loc.image_format == 18) {
        if (avail < 12) {
            return false;
        }
        bearing_x = (fdk_i32)(fdk_i8)d[2];
        bearing_y = (fdk_i32)(fdk_i8)d[3];
        fdk_u32 data_len = e_be32(d + 8);
        if (data_len > avail - 12) {
            return false;
        }
        png = d + 12;
        png_len = data_len;
    } else { /* 19: metrics from the subtable's shared BigGlyphMetrics */
        if (avail < 4) {
            return false;
        }
        /* The index subtable header (format 2/5) carries the metrics
         * 4 bytes past the offsets/ids preamble — re-read from the
         * located header. BigGlyphMetrics: height, width, horiBearingX,
         * horiBearingY, horiAdvance, vertBearingX, vertBearingY,
         * vertAdvance (8 bytes). */
        size_t hdr = 0;
        for (size_t i = k->sub_lo; i < k->sub_hi; i++) {
            if ((int)cb->subs[i].first_glyph <= glyph &&
                (int)cb->subs[i].last_glyph >= glyph) {
                hdr = cb->subs[i].header_off;
                break;
            }
        }
        if (hdr == 0 || hdr + 8 + 16 > cb->file_size) {
            return false;
        }
        const fdk_u8 *bgm = cb->file + hdr + 8 + 4; /* after imageSize */
        bearing_x = (fdk_i32)(fdk_i8)bgm[2];
        bearing_y = (fdk_i32)(fdk_i8)bgm[3];
        fdk_u32 data_len = e_be32(d);
        if (data_len > avail - 4) {
            return false;
        }
        png = d + 4;
        png_len = data_len;
    }
    int pw = 0, ph = 0;
    fdk_u8 *px = e_decode_png(png, png_len, &pw, &ph);
    if (px == NULL) {
        return false;
    }
    out->px = px;
    out->w = pw;
    out->h = ph;
    out->bearing_x = bearing_x;
    out->bearing_y = bearing_y;
    out->ppem = k->ppem_x;
    return true;
}

/* sbix twin: strike selection over the strike records, then the
 * per-glyph directory entry. Entries: originX i16, originY i16,
 * graphicType ('png '), dataOffset u32 from the entry's own start;
 * the per-glyph offsets array sits at the strike start. */
static bool e_sbix_render(const e_sbix *sb, int glyph, int num_glyphs,
                          double em_px, e_bitmap_glyph *out) {
    memset(out, 0, sizeof *out);
    if (glyph < 0 || glyph >= num_glyphs) {
        return false;
    }
    double best = 1.0e30;
    size_t best_i = (size_t)-1;
    for (size_t i = 0; i < sb->n_strikes; i++) {
        double score = e_strike_score(em_px, (double)sb->ppem[i]);
        if (score < best) {
            best = score;
            best_i = i;
        }
    }
    if (best_i == (size_t)-1) {
        return false;
    }
    size_t strike = sb->strikes[best_i];
    /* The offsets array: numGlyphs u32 entries at the strike start;
     * glyph offsets are relative to the strike start. */
    if (strike + 4u * (size_t)(glyph + 1) > sb->file_size) {
        return false;
    }
    size_t entry = strike + e_be32(sb->file + strike + 4u * (size_t)glyph);
    if (entry == strike || entry + 12 > sb->file_size) {
        return false; /* 0 = no bitmap for this glyph */
    }
    const fdk_u8 *e = sb->file + entry;
    if (e[4] != 'p' || e[5] != 'n' || e[6] != 'g' || e[7] != ' ') {
        return false; /* only PNG payloads are handled */
    }
    fdk_i32 origin_x = e_bei16(e);
    fdk_i32 origin_y = e_bei16(e + 2);
    size_t data_off = entry + e_be32(e + 8);
    if (data_off >= sb->file_size) {
        return false;
    }
    int pw = 0, ph = 0;
    fdk_u8 *px = e_decode_png(sb->file + data_off,
                              sb->file_size - data_off, &pw, &ph);
    if (px == NULL) {
        return false;
    }
    out->px = px;
    out->w = pw;
    out->h = ph;
    out->bearing_x = origin_x;
    out->bearing_y = origin_y;
    out->ppem = sb->ppem[best_i];
    return true;
}

/* ---- 10. The public seams --------------------------------------------- */

/* The parsed color-table view attached to an fdk_font. Borrows the
 * font's file bytes; dies with them (fdk_font_destroy calls
 * fdk__emoji_destroy BEFORE file_data goes). */
struct fdk_emoji_font {
    const fdk_u8 *file;
    size_t file_size;
    bool has_colr;
    e_colr colr;
    bool has_cpal;
    e_cpal cpal;
    bool has_cbdt;
    e_cbdt cbdt;
    bool has_sbix;
    e_sbix sbix;
    bool bitmap_only; /* no glyf AND no CFF: strikes are the ink  */
};

fdk_emoji_font *fdk__emoji_parse(const fdk_u8 *file, size_t file_size,
                                 size_t face_off) {
    if (file == NULL || face_off + 12 > file_size) {
        return NULL;
    }
    /* $FDK_NO_COLOR_GLYPHS=1 disables the color path for OUTLINE
     * faces (the escape hatch); bitmap-only faces keep parsing —
     * their strikes are the only ink there is, and refusing to parse
     * would refuse the whole font. */
    const char *no_color = getenv("FDK_NO_COLOR_GLYPHS");
    bool outline_face = e_find_table(file, file_size, face_off,
                                     0x676C7966u /* 'glyf' */, NULL,
                                     NULL) ||
                        e_find_table(file, file_size, face_off,
                                     0x43464620u /* 'CFF ' */, NULL,
                                     NULL);
    bool disabled = no_color != NULL && strcmp(no_color, "1") == 0 &&
                    outline_face;

    fdk_emoji_font *e = fdk_alloc(sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    memset(e, 0, sizeof *e);
    e->file = file;
    e->file_size = file_size;
    e->bitmap_only = !outline_face;

    bool any = false;
    if (e_cpal_parse(file, file_size, face_off, &e->cpal)) {
        e->has_cpal = true;
    }
    if (e_colr_parse(file, file_size, face_off, &e->colr)) {
        e->has_colr = true;
        any = true;
    }

    if (e_cbdt_parse(file, file_size, face_off, &e->cbdt)) {
        e->has_cbdt = true;
        any = true;
    }
    if (e_sbix_parse(file, file_size, face_off, &e->sbix)) {
        e->has_sbix = true;
        any = true;
    }
    if (!any || (e->has_colr && !e->has_cpal)) {
        /* Nothing usable (or a COLR without a CPAL — the spec pairs
         * them; a CPAL-less COLR is malformed). */
        if (e->bitmap_only) {
            /* A bitmap-only face with unusable tables still needs the
             * handle for fdk__emoji_bitmap_only_face's gate — but
             * nothing will render; keep it empty rather than NULL so
             * the loader's destruction path stays uniform. */
        } else {
            fdk_free(e);
            return NULL;
        }
    }
    if (disabled) {
        /* Parsed (so the font loads identically) but inert: every
         * is_color query answers false. */
        e->has_colr = false;
        e->has_cbdt = false;
        e->has_sbix = false;
    }
    return e;
}

void fdk__emoji_destroy(fdk_emoji_font *e) {
    /* All views borrow the font's file bytes — nothing else to free. */
    fdk_free(e);
}

bool fdk__emoji_is_color_glyph(const fdk_emoji_font *e, int glyph) {
    if (e == NULL || glyph < 0) {
        return false;
    }
    if (e->has_colr) {
        if (e->colr.version == 0) {
            size_t first, count;
            if (e_colr_v0_lookup(&e->colr, glyph, &first, &count)) {
                return true;
            }
        } else {
            size_t paint;
            if (e_colr_v1_lookup(&e->colr, glyph, &paint)) {
                return true;
            }
            /* A v1 table can ALSO carry the v0 half for legacy
             * clients (the spec's "both halves present" case). */
            if (e->colr.v0_base_count > 0) {
                size_t first, count;
                if (e_colr_v0_lookup(&e->colr, glyph, &first, &count)) {
                    return true;
                }
            }
        }
    }
    if (e->has_cbdt) {
        e_located loc;
        if (e_cbdt_locate(&e->cbdt, glyph, &loc)) {
            return true;
        }
    }
    if (e->has_sbix) {
        /* A glyph is color when its directory offset is non-zero;
         * the offsets array lives at the strike start (verified in
         * e_sbix_render; here a cheap presence probe). */
        for (size_t i = 0; i < e->sbix.n_strikes; i++) {
            size_t strike = e->sbix.strikes[i];
            if (strike + 4u * (size_t)(glyph + 1) <= e->file_size &&
                e_be32(e->file + strike + 4u * (size_t)glyph) != 0) {
                return true;
            }
        }
    }
    return false;
}

/* The pixels-per-em the requested size implies. font->scale maps
 * font units -> pixels with pixel_size measured over (ascent -
 * descent), so em-pixels = scale * unitsPerEm. */
static double e_em_pixels(const stbtt_fontinfo *info, fdk_f32 scale) {
    int units = 1000;
    if (info->head != 0) {
        units = (int)e_be16(info->data + info->head + 18);
    }
    if (units <= 0) {
        units = 1000;
    }
    return (double)scale * (double)units;
}

fdk_u8 *fdk__emoji_rasterize(const fdk_emoji_font *e,
                             const stbtt_fontinfo *info,
                             fdk_cff_font *cff, int glyph, fdk_f32 scale,
                             int *out_w, int *out_h, fdk_i32 *out_xoff,
                             fdk_i32 *out_yoff) {
    if (e == NULL || info == NULL || glyph < 0 || scale <= 0) {
        return NULL;
    }
    *out_w = 0;
    *out_h = 0;
    *out_xoff = 0;
    *out_yoff = 0;

    /* COLR first (v1 then v0): outline-native color. */
    if (e->has_colr && e->has_cpal) {
        if (e->colr.version >= 1) {
            size_t paint;
            if (e_colr_v1_lookup(&e->colr, glyph, &paint)) {
                fdk_u8 *px = e_colr_v1_render(&e->colr, &e->cpal, info,
                                              cff, scale, glyph, out_w,
                                              out_h, out_xoff, out_yoff);
                if (px != NULL) {
                    return px;
                }
            }
        }
        size_t first, count;
        if (e_colr_v0_lookup(&e->colr, glyph, &first, &count)) {
            return e_colr_v0_render(&e->colr, &e->cpal, info, cff,
                                    scale, glyph, out_w, out_h, out_xoff,
                                    out_yoff);
        }
    }

    /* Bitmap strikes: CBDT, then sbix. */
    if (e->has_cbdt) {
        e_bitmap_glyph bg;
        if (e_cbdt_render(&e->cbdt, glyph, e_em_pixels(info, scale),
                          &bg)) {
            fdk_f32 s = (fdk_f32)(e_em_pixels(info, scale) /
                                  (double)bg.ppem);
            int dw = (int)((fdk_f32)bg.w * s + 0.5f);
            int dh = (int)((fdk_f32)bg.h * s + 0.5f);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            fdk_u8 *scaled = e_resample(bg.px, bg.w, bg.h, dw, dh);
            fdk_free(bg.px);
            if (scaled != NULL) {
                *out_w = dw;
                *out_h = dh;
                *out_xoff = (fdk_i32)floorf((fdk_f32)bg.bearing_x * s);
                *out_yoff =
                    (fdk_i32)floorf(-(fdk_f32)bg.bearing_y * s);
                return scaled;
            }
        }
    }
    if (e->has_sbix) {
        int num_glyphs = info->numGlyphs;
        e_bitmap_glyph bg;
        if (e_sbix_render(&e->sbix, glyph, num_glyphs,
                          e_em_pixels(info, scale), &bg)) {
            fdk_f32 s = (fdk_f32)(e_em_pixels(info, scale) /
                                  (double)bg.ppem);
            int dw = (int)((fdk_f32)bg.w * s + 0.5f);
            int dh = (int)((fdk_f32)bg.h * s + 0.5f);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            fdk_u8 *scaled = e_resample(bg.px, bg.w, bg.h, dw, dh);
            fdk_free(bg.px);
            if (scaled != NULL) {
                *out_w = dw;
                *out_h = dh;
                *out_xoff = (fdk_i32)floorf((fdk_f32)bg.bearing_x * s);
                *out_yoff =
                    (fdk_i32)floorf(-(fdk_f32)bg.bearing_y * s);
                return scaled;
            }
        }
    }
    return NULL;
}

bool fdk__emoji_glyph_box(const fdk_emoji_font *e,
                          const stbtt_fontinfo *info, fdk_cff_font *cff,
                          int glyph, int *x0, int *y0, int *x1,
                          int *y1) {
    if (e == NULL || info == NULL || glyph < 0) {
        return false;
    }
    /* COLR: the ink box in font units (the same box the render
     * entry computes, unscaled). */
    if (e->has_colr && e->has_cpal) {
        if (e->colr.version >= 1) {
            size_t paint;
            if (e_colr_v1_lookup(&e->colr, glyph, &paint)) {
                if (e->colr.clip_list != NULL) {
                    for (size_t i = 0; i < e->colr.clip_count; i++) {
                        const fdk_u8 *r =
                            e->colr.clip_list + 5 + 8 * i;
                        fdk_u16 g0 = e_be16(r);
                        fdk_u16 g1 = e_be16(r + 2);
                        if (glyph >= (int)g0 && glyph <= (int)g1) {
                            size_t box_off = (size_t)(r - e->file) +
                                             e_be24(r + 4);
                            if (box_off + 9 <= e->colr.colr_end &&
                                e->file[box_off] == 1) {
                                const fdk_u8 *b = e->file + box_off;
                                *x0 = e_bei16(b + 1);
                                *y0 = e_bei16(b + 3);
                                *x1 = e_bei16(b + 5);
                                *y1 = e_bei16(b + 7);
                                return true;
                            }
                            break;
                        }
                    }
                }
                e_mat m0 = e_mat_identity();
                fdk_f32 bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
                int any = 0;
                int visiting[E_COLR_CYCLE_MAX];
                e_paint_bounds(&e->colr, info, cff, paint, &m0, &bx0,
                               &by0, &bx1, &by1, 0, &any, visiting, 0);
                if (any) {
                    *x0 = (int)floorf(bx0);
                    *y0 = (int)floorf(by0);
                    *x1 = (int)ceilf(bx1);
                    *y1 = (int)ceilf(by1);
                    return true;
                }
            }
        }
        size_t first, count;
        if (e_colr_v0_lookup(&e->colr, glyph, &first, &count)) {
            /* Union of the layer boxes. */
            bool any = false;
            for (size_t i = 0; i < count; i++) {
                const fdk_u8 *lr =
                    e->colr.v0_layers + 4u * (first + i);
                int layer_glyph = (int)e_be16(lr);
                int gx0, gy0, gx1, gy1;
                bool have =
                    cff != NULL
                        ? fdk__cff_glyph_box(cff, layer_glyph, &gx0,
                                             &gy0, &gx1, &gy1)
                        : (stbtt_GetGlyphBox(info, layer_glyph, &gx0,
                                             &gy0, &gx1, &gy1) != 0);
                if (!have) {
                    continue;
                }
                if (!any) {
                    *x0 = gx0;
                    *y0 = gy0;
                    *x1 = gx1;
                    *y1 = gy1;
                    any = true;
                } else {
                    if (gx0 < *x0) *x0 = gx0;
                    if (gy0 < *y0) *y0 = gy0;
                    if (gx1 > *x1) *x1 = gx1;
                    if (gy1 > *y1) *y1 = gy1;
                }
            }
            if (any) {
                return true;
            }
        }
    }
    /* Bitmap strikes: the box in font units. */
    double upem = 1.0;
    if (info->head != 0) {
        int units = (int)e_be16(info->data + info->head + 18);
        if (units > 0) {
            upem = (double)units;
        }
    }
    if (e->has_cbdt) {
        e_bitmap_glyph bg;
        if (e_cbdt_render(&e->cbdt, glyph, upem, &bg)) {
            /* at strike scale (em_px == ppem): pixels are font units
             * * ppem/upem — invert. */
            double k = upem / (double)bg.ppem;
            *x0 = (int)floorf((fdk_f32)bg.bearing_x * (fdk_f32)k);
            *y1 = (int)ceilf((fdk_f32)bg.bearing_y * (fdk_f32)k);
            *x1 = (int)ceilf(((fdk_f32)bg.bearing_x + (fdk_f32)bg.w) *
                             (fdk_f32)k);
            *y0 = (int)floorf(((fdk_f32)bg.bearing_y - (fdk_f32)bg.h) *
                              (fdk_f32)k);
            fdk_free(bg.px);
            return true;
        }
    }
    if (e->has_sbix) {
        e_bitmap_glyph bg;
        if (e_sbix_render(&e->sbix, glyph, info->numGlyphs, upem,
                          &bg)) {
            double k = upem / (double)bg.ppem;
            *x0 = (int)floorf((fdk_f32)bg.bearing_x * (fdk_f32)k);
            *y1 = (int)ceilf((fdk_f32)bg.bearing_y * (fdk_f32)k);
            *x1 = (int)ceilf(((fdk_f32)bg.bearing_x + (fdk_f32)bg.w) *
                             (fdk_f32)k);
            *y0 = (int)floorf(((fdk_f32)bg.bearing_y - (fdk_f32)bg.h) *
                              (fdk_f32)k);
            fdk_free(bg.px);
            return true;
        }
    }
    return false;
}

bool fdk__emoji_bitmap_only_face(const fdk_u8 *file, size_t file_size,
                                 size_t face_off) {
    if (file == NULL || face_off + 12 > file_size) {
        return false;
    }
    /* The gate: no glyf, no CFF — but a real CBDT/CBLC pair. (sbix
     * faces ship glyf in practice; CBDT-only is the Noto Color Emoji
     * posture this gate exists for.) */
    if (e_find_table(file, file_size, face_off, 0x676C7966u, NULL, NULL)) {
        return false;
    }
    if (e_find_table(file, file_size, face_off, 0x43464620u, NULL, NULL)) {
        return false;
    }
    return e_find_table(file, file_size, face_off, 0x43424C43u, NULL,
                        NULL) &&
           e_find_table(file, file_size, face_off, 0x43424454u, NULL,
                        NULL);
}

/* Fills a stbtt_fontinfo for a bitmap-only face (no glyf, no CFF) so
 * the cmap/hmtx/hhea/head machinery stb exposes keeps working: the
 * exact table walk stbtt_InitFont_internal does, minus the outline
 * requirement. The CFF bufs stay zeroed (an empty stbtt__buf — the
 * same value stbtt__new_buf(NULL, 0) produces), and loca/glyf stay
 * 0: no outline operation may be issued against this info (the
 * callers guard with font->bitmap_only). */
bool fdk__emoji_fill_info(stbtt_fontinfo *info, const fdk_u8 *file,
                          size_t file_size, size_t face_off) {
    memset(info, 0, sizeof *info);
    if (file == NULL || face_off + 12 > file_size) {
        return false;
    }
    size_t off = 0, len = 0;
    if (!e_find_table(file, file_size, face_off, 0x636D6170u /* 'cmap' */,
                      &off, &len) ||
        !e_find_table(file, file_size, face_off, 0x68656164u /* 'head' */,
                      NULL, NULL) ||
        !e_find_table(file, file_size, face_off, 0x68686561u /* 'hhea' */,
                      NULL, NULL) ||
        !e_find_table(file, file_size, face_off, 0x686D7478u /* 'hmtx' */,
                      NULL, NULL)) {
        return false; /* the format-agnostic tables are required */
    }
    if (e_find_table(file, file_size, face_off, 0x6D617870u /* 'maxp' */,
                     &off, &len)) {
        if (off + 6 > file_size) {
            return false;
        }
        info->numGlyphs = (int)e_be16(file + off + 4);
    } else {
        info->numGlyphs = 0xffff;
    }

    size_t cmap_off = 0, cmap_len = 0;
    if (!e_find_table(file, file_size, face_off, 0x636D6170u, &cmap_off,
                      &cmap_len) ||
        cmap_off + 4 > file_size) {
        return false;
    }
    size_t num_tables = e_be16(file + cmap_off + 2);
    if (cmap_off + 4 + 8 * num_tables > file_size) {
        return false;
    }
    /* The same encoding preference stbtt_InitFont applies: Microsoft
     * 3/1 or 3/10, then platform 0. Later records win, matching
     * stb's loop. */
    size_t index_map = 0;
    for (size_t i = 0; i < num_tables; i++) {
        size_t rec = cmap_off + 4 + 8 * i;
        fdk_u16 platform = e_be16(file + rec);
        fdk_u16 encoding = e_be16(file + rec + 2);
        bool take = false;
        if (platform == 3) {
            take = (encoding == 1 || encoding == 10);
        } else if (platform == 0) {
            take = true;
        }
        if (take && rec + 8 <= file_size) {
            size_t sub = cmap_off + e_be32(file + rec + 4);
            if (sub < file_size) {
                index_map = sub;
            }
        }
    }
    if (index_map == 0) {
        return false;
    }

    size_t t = 0;
    if (e_find_table(file, file_size, face_off, 0x68656164u, &t, NULL)) {
        info->head = (int)t;
    }
    if (e_find_table(file, file_size, face_off, 0x68686561u, &t, NULL)) {
        info->hhea = (int)t;
    }
    if (e_find_table(file, file_size, face_off, 0x686D7478u, &t, NULL)) {
        info->hmtx = (int)t;
    }
    if (e_find_table(file, file_size, face_off, 0x6B65726Eu, &t, NULL)) {
        info->kern = (int)t;
    }
    info->data = (unsigned char *)(uintptr_t)file; /* cast away const —
                                                      stb's struct
                                                      predates const */
    info->fontstart = (int)face_off;
    info->index_map = (int)index_map;
    info->indexToLocFormat = 0;
    info->loca = 0;
    info->glyf = 0;
    info->gpos = 0;
    info->svg = -1;
    return true;
}
