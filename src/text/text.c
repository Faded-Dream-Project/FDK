/*
 * text.c — fonts, shaping, measurement, and glyph rendering (Phase 6
 * text foundation).
 *
 * Pipeline: UTF-8 bytes -> codepoints (strict-ish RFC 3629 decode,
 * U+FFFD for invalid bytes) -> glyph indices (stbtt, .notdef when
 * unmapped) -> per-glyph metrics from the LRU cache (rasterized on
 * first use) -> float pen advance with pair kerning -> integer glyph
 * placement -> alpha-mask blend through fdk_surface_blend_mask().
 *
 * measure and draw share the exact same shaping walk, so
 * fdk_font_measure_utf8()'s advance_width is by construction where
 * fdk_surface_draw_utf8()'s pen ends up: same decode, same kerning,
 * same rounding rule (each glyph is placed at round(pen), the total
 * is round(final pen)).
 *
 * Damage model: a run's ink boxes are unioned into ONE rect (already
 * intersected with the current clip), invalidated once after the
 * glyphs are blended — a 100-glyph line costs one damage rect, and
 * pixels outside the clip contribute nothing.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- UTF-8 decode ---- */

/* Continuation byte check: 10xxxxxx */
#define FDK_IS_CONT(b) (((b)&0xC0u) == 0x80u)

int fdk_text_utf8_next(const char *s, size_t len, size_t i,
                       fdk_u32 *out_cp) {
    fdk_u32 b0 = (fdk_u32)(unsigned char)s[i];

    /* 1 byte: ASCII (NUL included — it is a valid codepoint; fonts
     * answer it with .notdef or an empty glyph). */
    if (b0 < 0x80u) {
        *out_cp = b0;
        return 1;
    }

    /* 2 bytes: C2..DF */
    if (b0 >= 0xC2u && b0 <= 0xDFu && i + 1 < len &&
        FDK_IS_CONT((unsigned char)s[i + 1])) {
        *out_cp = ((b0 & 0x1Fu) << 6) |
                  (fdk_u32)(unsigned char)(s[i + 1] & 0x3Fu);
        return 2;
    }

    /* 3 bytes: E0 A0..BF / E1..EC / ED 80..9F (no surrogates) /
     * EE..EF, all with two continuations */
    if (i + 2 < len && FDK_IS_CONT((unsigned char)s[i + 1]) &&
        FDK_IS_CONT((unsigned char)s[i + 2])) {
        fdk_u32 b1 = (fdk_u32)(unsigned char)s[i + 1];
        fdk_u32 b2 = (fdk_u32)(unsigned char)s[i + 2];
        fdk_u32 cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) |
                     (b2 & 0x3Fu);
        if ((b0 == 0xE0u && b1 >= 0xA0u && b1 <= 0xBFu) ||
            (b0 >= 0xE1u && b0 <= 0xECu) ||
            (b0 == 0xEDu && b1 >= 0x80u && b1 <= 0x9Fu) ||
            (b0 >= 0xEEu && b0 <= 0xEFu)) {
            *out_cp = cp;
            return 3;
        }
    }

    /* 4 bytes: F0 90..BF / F1..F3 / F4 80..8F, three continuations */
    if (i + 3 < len && FDK_IS_CONT((unsigned char)s[i + 1]) &&
        FDK_IS_CONT((unsigned char)s[i + 2]) &&
        FDK_IS_CONT((unsigned char)s[i + 3])) {
        fdk_u32 b1 = (fdk_u32)(unsigned char)s[i + 1];
        fdk_u32 cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) |
                     (((fdk_u32)(unsigned char)s[i + 2] & 0x3Fu) << 6) |
                     (fdk_u32)(unsigned char)(s[i + 3] & 0x3Fu);
        if ((b0 == 0xF0u && b1 >= 0x90u && b1 <= 0xBFu) ||
            (b0 >= 0xF1u && b0 <= 0xF3u) ||
            (b0 == 0xF4u && b1 >= 0x80u && b1 <= 0x8Fu)) {
            *out_cp = cp;
            return 4;
        }
    }

    /* Anything else (truncated sequence at end of buffer, overlong,
     * surrogate, > U+10FFFF): replacement character, one byte. */
    *out_cp = 0xFFFDu;
    return 1;
}

/* ---- Glyph cache ---- */

/* Finds the cache slot for a glyph index, rasterizing on miss and
 * evicting LRU when full. Returns the entry (never NULL). */
static fdk_glyph *glyph_slot(fdk_font *font, int glyph_index) {
    /* Hit? */
    for (int i = 0; i < font->glyph_count; i++) {
        if (font->glyphs[i].glyph_index == glyph_index) {
            font->clock++;
            font->glyphs[i].last_used = font->clock;
            font->stats.cache_hits++;
            return &font->glyphs[i];
        }
    }

    /* Miss: find a slot — a free one if the table has room, else the
     * least recently used entry. */
    fdk_glyph *slot = NULL;
    if (font->glyph_count < FDK_TEXT_GLYPH_CACHE_MAX) {
        slot = &font->glyphs[font->glyph_count++];
    } else {
        fdk_u64 oldest = font->glyphs[0].last_used;
        slot = &font->glyphs[0];
        for (int i = 1; i < font->glyph_count; i++) {
            if (font->glyphs[i].last_used < oldest) {
                oldest = font->glyphs[i].last_used;
                slot = &font->glyphs[i];
            }
        }
        fdk_free(slot->bits);
        slot->bits = NULL;
        font->stats.evictions++;
    }

    /* Rasterize with stb at the font's baked-in scale. Integer
     * subpixel offset (frac = 0) — v1 places glyphs at rounded pen
     * positions. */
    int w = 0, h = 0;
    fdk_i32 xoff = 0, yoff = 0;
    fdk_u8 *bits = stbtt_GetGlyphBitmap(&font->info, 0, font->scale,
                                        glyph_index, &w, &h, &xoff,
                                        &yoff);
    int advance_raw = 0;
    stbtt_GetGlyphHMetrics(&font->info, glyph_index, &advance_raw, NULL);

    slot->glyph_index = glyph_index;
    slot->w = w;
    slot->h = h;
    slot->xoff = xoff;
    slot->yoff = yoff;
    slot->advance = (fdk_f32)advance_raw * font->scale;
    slot->bits = bits; /* stb malloc'd (routed to fdk_alloc); may be NULL */
    font->clock++;
    slot->last_used = font->clock;
    font->stats.cache_misses++;
    font->stats.cached_glyphs = font->glyph_count;
    return slot;
}

const fdk_glyph *fdk_text_glyph_for(fdk_font *font, fdk_u32 codepoint) {
    if (font == NULL) {
        return NULL;
    }
    /* FindGlyphIndex returns 0 for unmapped codepoints — and glyph 0
     * IS .notdef (the font's missing-glyph box, or an empty glyph),
     * so the fallback is free and its metrics stay valid. */
    int g = stbtt_FindGlyphIndex(&font->info, (int)codepoint);
    return glyph_slot(font, g);
}

/* ---- Font container validation ---- */

static fdk_u32 be32(const unsigned char *p) {
    return ((fdk_u32)p[0] << 24) | ((fdk_u32)p[1] << 16) |
           ((fdk_u32)p[2] << 8) | (fdk_u32)p[3];
}

static fdk_u32 be16(const unsigned char *p) {
    return ((fdk_u32)p[0] << 8) | (fdk_u32)p[1];
}

/* Validates the sfnt container enough that stbtt's table lookups
 * stay within the buffer: magic, table directory inside the file,
 * and every table record's [offset, offset+length) inside the file.
 * Returns the face offset to parse (0 for plain sfnt, the first
 * face's offset for a TrueType Collection), or -1 when the data is
 * not a trustworthy TrueType-flavored font.
 *
 * Why this exists: stb_truetype deliberately does no range checking
 * (see its header and third_party/stb/README.md) — it assumes a
 * trusted font. This gate keeps malformed files (truncated tables,
 * garbage bytes, bogus directories) from turning into out-of-bounds
 * reads inside stb. It is not a full security audit of table
 * CONTENTS: fonts should still come from sources the application
 * trusts, as fdk_text.h documents. */
static long validate_sfnt(const unsigned char *data, size_t size) {
    if (data == NULL || size < 12) {
        return -1;
    }

    long face = 0;
    fdk_u32 tag = be32(data);
    if (tag == 0x74746366u) { /* 'ttcf' — collection */
        if (size < 20) {
            return -1; /* header + at least one face offset */
        }
        fdk_u32 first = be32(data + 12); /* face 0's offset */
        if (first < 12 || (size_t)first + 12 > size) {
            return -1;
        }
        face = (long)first;
        data += first;
        size -= (size_t)first;
        tag = be32(data);
    }

    /* Only TrueType-flavored sfnt (glyf outlines). CFF-flavored
     * OpenType ('OTTO') parses but cannot rasterize in stb — reject
     * it explicitly rather than shipping silent tofu. */
    if (tag != 0x00010000u) {
        return -1;
    }

    fdk_u32 num_tables = be16(data + 4);
    if ((size_t)num_tables > (size - 12) / 16) {
        return -1; /* directory itself would run past the buffer */
    }
    const unsigned char *rec = data + 12;
    for (fdk_u32 i = 0; i < num_tables; i++, rec += 16) {
        fdk_u32 off = be32(rec + 8);
        fdk_u32 len = be32(rec + 12);
        if (off > size || len > size - off) {
            return -1; /* table extent outside the file */
        }
    }
    return face;
}

/* ---- Font lifecycle ---- */

fdk_font *fdk_font_load(const char *path, fdk_i32 pixel_size) {
    if (path == NULL || pixel_size < 1 || pixel_size > 512) {
        FDK_ERROR("fdk_font_load: path=%s size=%d (size must be 1..512)",
                  path == NULL ? "(null)" : path, pixel_size);
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        FDK_ERROR("fdk_font_load: cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long length = ftell(f);
    if (length <= 0 || length > (64L * 1024L * 1024L)) {
        FDK_ERROR("fdk_font_load: %s has implausible size %ld", path,
                  length);
        fclose(f);
        return NULL;
    }
    rewind(f);

    size_t size = (size_t)length;
    unsigned char *data = fdk_alloc(size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, size, f) != size) {
        fdk_free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    fdk_font *font = fdk_alloc(sizeof(fdk_font));
    if (font == NULL) {
        fdk_free(data);
        return NULL;
    }
    memset(font, 0, sizeof(*font));

    /* Container gate BEFORE stb sees the bytes (see validate_sfnt). */
    long face_offset = validate_sfnt(data, size);
    if (face_offset < 0) {
        FDK_ERROR("fdk_font_load: %s is not a usable TrueType font "
                  "(container validation failed)",
                  path);
        fdk_free(font);
        fdk_free(data);
        return NULL;
    }

    if (!stbtt_InitFont(&font->info, data, (int)face_offset)) {
        FDK_ERROR("fdk_font_load: %s rejected by the font parser",
                  path);
        fdk_free(font);
        fdk_free(data);
        return NULL;
    }

    font->file_data = data;
    font->pixel_size = pixel_size;
    font->scale = stbtt_ScaleForPixelHeight(&font->info, (float)pixel_size);

    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    /* Round to nearest pixel — these are layout-facing numbers, and
     * a 14.85px ascent truncating to 14 skews every line box. */
    font->metrics.ascent = (fdk_i32)((fdk_f32)ascent * font->scale +
                                     0.5f);
    font->metrics.descent = (fdk_i32)((fdk_f32)(-descent) * font->scale +
                                      0.5f);
    font->metrics.line_gap = (fdk_i32)((fdk_f32)line_gap * font->scale +
                                       0.5f);
    font->metrics.line_height = font->metrics.ascent +
                                font->metrics.descent +
                                font->metrics.line_gap;

    FDK_INFO("loaded %s @ %dpx (ascent %d descent %d line %d)", path,
             pixel_size, font->metrics.ascent, font->metrics.descent,
             font->metrics.line_height);
    return font;
}

void fdk_font_destroy(fdk_font *font) {
    if (font == NULL) {
        return;
    }
    for (int i = 0; i < font->glyph_count; i++) {
        fdk_free(font->glyphs[i].bits);
    }
    fdk_free(font->file_data);
    fdk_free(font);
}

void fdk_font_get_metrics(const fdk_font *font, fdk_font_metrics *out) {
    if (font == NULL || out == NULL) {
        return;
    }
    *out = font->metrics;
}

void fdk_font_get_cache_stats(const fdk_font *font,
                              fdk_font_cache_stats *out) {
    if (font == NULL || out == NULL) {
        return;
    }
    *out = font->stats;
}

/* ---- Shaping walk (shared by measure and draw) ---- */

/* Measure deliberately warms the glyph cache (documented in the
 * public header): the LRU clock and stats counters live inside the
 * font, so measurement needs mutable state behind a const public
 * signature. This union launder concentrates that one unavoidable
 * pointer-laundering into a single documented place — shared with
 * the layout pass (src/text/layout.c); no other const-casting
 * exists in the text layer. */
fdk_font *fdk_text_font_mutable(const fdk_font *font) {
    union {
        const fdk_font *in;
        fdk_font *out;
    } launder = {font};
    return launder.out;
}

/* One step of the left-to-right shaping walk: decodes the next
 * codepoint at *io_i, fetches its (cached) glyph, applies kerning
 * against *io_prev_g, and advances the float pen. Writes the glyph,
 * the rounded pen position it should be drawn at, and the new
 * previous-glyph id. Returns 0 when the walk is done.
 *
 * Shared by measure, draw, AND the line/ellipsis layout pass — see
 * text_internal.h. */
int fdk_text_shape_step(fdk_font *font, const char *utf8, size_t len,
                        size_t *io_i, int *io_prev_g, fdk_f32 *io_pen,
                        const fdk_glyph **out_glyph, fdk_i32 *out_pen_x) {
    if (*io_i >= len) {
        return 0;
    }
    fdk_u32 cp = 0;
    int consumed = fdk_text_utf8_next(utf8, len, *io_i, &cp);
    int g = stbtt_FindGlyphIndex(&font->info, (int)cp);

    /* Kerning against the previous glyph (scaled). */
    if (*io_prev_g >= 0) {
        int kern = stbtt_GetGlyphKernAdvance(&font->info, *io_prev_g, g);
        if (kern != 0) {
            *io_pen += (fdk_f32)kern * font->scale;
        }
    }

    const fdk_glyph *glyph = glyph_slot(font, g);
    *io_pen += glyph->advance;
    *io_i += (size_t)consumed;
    *io_prev_g = g;

    /* Placement x is where the pen was BEFORE this glyph's advance —
     * the left edge of its slot (kerning already applied). */
    *out_glyph = glyph;
    *out_pen_x = (fdk_i32)(*io_pen - glyph->advance + 0.5f);
    return 1;
}

fdk_result fdk_font_measure_utf8(const fdk_font *font,
                                 const char *utf8, size_t byte_len,
                                 fdk_text_metrics *out) {
    fdk_font *f = fdk_text_font_mutable(font);
    if (f == NULL || utf8 == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_text_metrics m = {0, 0, 0};
    size_t i = 0;
    int prev_g = -1;
    fdk_f32 pen = 0.0f;
    fdk_f32 ink_top = 0.0f;    /* min y-offset above baseline (<= 0) */
    fdk_f32 ink_bottom = 0.0f; /* max y-offset below baseline (>= 0) */
    int has_ink = 0;

    for (;;) {
        const fdk_glyph *glyph = NULL;
        fdk_i32 pen_x = 0;
        if (!fdk_text_shape_step(f, utf8, byte_len, &i, &prev_g, &pen,
                                 &glyph, &pen_x)) {
            break;
        }
        if (glyph->w > 0 && glyph->h > 0) {
            /* Vector outline box would need another stb call; the
             * rasterized box is already cached and is what draw()
             * actually paints — use it. */
            if ((fdk_f32)glyph->yoff < ink_top) {
                ink_top = (fdk_f32)glyph->yoff;
            }
            fdk_f32 bottom = (fdk_f32)(glyph->yoff + glyph->h);
            if (bottom > ink_bottom) {
                ink_bottom = bottom;
            }
            has_ink = 1;
        }
    }

    m.advance_width = (fdk_i32)(pen + 0.5f);
    if (has_ink) {
        m.ink_top = (fdk_i32)ink_top;
        m.ink_bottom = (fdk_i32)ink_bottom;
    }
    *out = m;
    return FDK_OK;
}

/* Rect union on i64 coordinates (glyph boxes can poke far outside a
 * small surface when fonts are big; the clip intersect afterwards
 * brings them back into range). */
static void rect_union_i64(fdk_i64 *x0, fdk_i64 *y0, fdk_i64 *x1,
                           fdk_i64 *y1, fdk_i64 rx0, fdk_i64 ry0,
                           fdk_i64 rx1, fdk_i64 ry1) {
    if (rx0 < *x0) *x0 = rx0;
    if (ry0 < *y0) *y0 = ry0;
    if (rx1 > *x1) *x1 = rx1;
    if (ry1 > *y1) *y1 = ry1;
}

fdk_result fdk_surface_draw_utf8(fdk_surface *surface, fdk_font *font,
                                 const char *utf8, size_t byte_len,
                                 fdk_i32 pen_x, fdk_i32 baseline_y,
                                 fdk_color color) {
    if (surface == NULL || font == NULL || utf8 == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Damage bookkeeping needs the pixels this run could touch: the
     * renderer's LIVE effective clip (clip stack intersected with the
     * framebuffer bounds, half-open). Those are exactly the pixels
     * blend_mask below can write, so the damage rect is precise by
     * construction. (fdk_surface_get_clip()'s empty-stack idiom is
     * the rect {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}, whose
     * x+width OVERFLOWS the plane — never do window math on it.) */
    fdk_i64 bx0 = (fdk_i64)surface->clip_x0;
    fdk_i64 by0 = (fdk_i64)surface->clip_y0;
    fdk_i64 bx1 = (fdk_i64)surface->clip_x1;
    fdk_i64 by1 = (fdk_i64)surface->clip_y1;

    size_t i = 0;
    int prev_g = -1;
    fdk_f32 pen = 0.0f;
    fdk_i64 ux0 = 0, uy0 = 0, ux1 = -1, uy1 = -1; /* empty union */
    int any = 0;

    for (;;) {
        const fdk_glyph *glyph = NULL;
        fdk_i32 gx = 0;
        if (!fdk_text_shape_step(font, utf8, byte_len, &i, &prev_g, &pen,
                                 &glyph, &gx)) {
            break;
        }
        if (glyph->bits == NULL || glyph->w <= 0 || glyph->h <= 0) {
            continue;
        }
        fdk_rect dst = {
            pen_x + gx + glyph->xoff,
            baseline_y + glyph->yoff,
            glyph->w,
            glyph->h,
        };
        fdk_surface_blend_mask(surface, dst, glyph->bits, glyph->w,
                               color);
        if (!any) {
            ux0 = dst.x;
            uy0 = dst.y;
            ux1 = (fdk_i64)dst.x + dst.width;
            uy1 = (fdk_i64)dst.y + dst.height;
            any = 1;
        } else {
            rect_union_i64(&ux0, &uy0, &ux1, &uy1, dst.x, dst.y,
                           (fdk_i64)dst.x + dst.width,
                           (fdk_i64)dst.y + dst.height);
        }
    }

    /* One damage rect per run, clipped to the pixels actually
     * reachable. Nothing drawn (all-whitespace run, empty string) ->
     * no damage at all. */
    if (any) {
        fdk_i64 dx0 = ux0 > bx0 ? ux0 : bx0;
        fdk_i64 dy0 = uy0 > by0 ? uy0 : by0;
        fdk_i64 dx1 = ux1 < bx1 ? ux1 : bx1;
        fdk_i64 dy1 = uy1 < by1 ? uy1 : by1;
        if (dx1 > dx0 && dy1 > dy0 &&
            dx0 <= (fdk_i64)INT32_MAX && dx1 >= (fdk_i64)INT32_MIN &&
            dy0 <= (fdk_i64)INT32_MAX && dy1 >= (fdk_i64)INT32_MIN) {
            fdk_surface_invalidate(surface,
                                   (fdk_rect){
                                       (fdk_i32)dx0, (fdk_i32)dy0,
                                       (fdk_i32)(dx1 - dx0),
                                       (fdk_i32)(dy1 - dy0),
                                   });
        }
    }
    return FDK_OK;
}
