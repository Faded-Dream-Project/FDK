/*
 * gif.c — the animated GIF decoder + compositor (1.1.0 / C-4)
 *
 * A self-contained GIF87a/89a parser written from the published
 * specification: the logical screen, the global color table, the
 * extension blocks (Graphic Control's delay/disposal/transparency,
 * the NETSCAPE2.0 loop count), the image descriptors with local
 * color tables and interlacing, and the GIF LZW variant (code size
 * 3..12, clear/EOI, the KwKwK case). stb_image is NOT used here —
 * its public surface is first-frame-only, and threading its
 * internals into a multi-frame API would couple FDK to undocumented
 * structure. ~700 lines of spec, fully bounded, beats a dependency.
 *
 * Security posture (docs/security.md's attacker-data rules, the
 * same ones surface_image.c follows):
 *
 *   - reject-first: the file is statted before any decode; regular
 *     files only, 512 MiB cap;
 *   - bounded: canvas 1..16384 (the surface limits), frame count
 *     <= 4096, per-frame geometry must land inside the canvas, the
 *     LZW decode emits EXACTLY w*h indices and stops, sub-block
 *     concatenation is capped, color tables are sized by their
 *     2^(n+1) fields;
 *   - fdk_alloc discipline: every buffer is fdk_alloc'd, freed on
 *     every failure path, and nothing partial escapes (out_gif is
 *     untouched unless FDK_OK).
 *
 * The compositing model is the player's (fdk_gif_player): one
 * frame at a time onto an app-owned ARGB8888 surface, the previous
 * frame's disposal applied first. Disposal 2 ("restore to
 * background") clears the previous frame's rect to TRANSPARENT —
 * the interoperable reading (the spec's background-color reading
 * paints a solid rectangle over transparent-capable displays;
 * every browser and giflib-based tool implements the clear, and so
 * does FDK, documented). Disposal 3 restores the snapshot taken
 * before the disposal-3 frame drew.
 *
 * Not part of the public API beyond include/fdk/fdk_surface.h's
 * animated-image section — never installed.
 */

#define FDK_LOG_TAG "render"

#include "surface_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ---- bounds ---- */

#define GIF_MAX_FILE_BYTES (512u * 1024u * 1024u)
#define GIF_MAX_FRAMES 4096
#define GIF_MAX_DIM 16384
/* Sub-block concatenation cap: a single frame's LZW stream larger
 * than this is a hostile file (a 16384x16384 frame's indices are
 * 256 MiB — the cap sits above every legal geometry this decoder
 * accepts, and below unbounded growth). */
#define GIF_MAX_SUBBLOCKS (280u * 1024u * 1024u)

/* ---- byte reader ---- */

typedef struct gif_reader {
    const fdk_u8 *data;
    size_t size;
    size_t pos;
} gif_reader;

static bool rd_u8(gif_reader *r, fdk_u8 *out) {
    if (r->pos + 1 > r->size) {
        return false;
    }
    *out = r->data[r->pos++];
    return true;
}

static bool rd_u16(gif_reader *r, fdk_u16 *out) {
    if (r->pos + 2 > r->size) {
        return false;
    }
    *out = (fdk_u16)(r->data[r->pos] |
                     ((fdk_u16)r->data[r->pos + 1] << 8));
    r->pos += 2;
    return true;
}

static bool rd_skip(gif_reader *r, size_t n) {
    if (r->pos + n > r->size) {
        return false;
    }
    r->pos += n;
    return true;
}

/* Reads exactly n bytes into out (bounds-checked). */
static bool rd_bytes(gif_reader *r, fdk_u8 *out, size_t n) {
    if (r->pos + n > r->size) {
        return false;
    }
    memcpy(out, r->data + r->pos, n);
    r->pos += n;
    return true;
}

/* ---- sub-block stream ---- */

/* Concatenates a sub-block chain (len byte + data, until len 0)
 * into one fdk_alloc'd buffer. The cap keeps a lying length field
 * from allocating without bound. */
static bool read_subblocks(gif_reader *r, fdk_u8 **out,
                           size_t *out_len) {
    fdk_u8 *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    for (;;) {
        fdk_u8 n = 0;
        if (!rd_u8(r, &n)) {
            fdk_free(buf);
            return false;
        }
        if (n == 0) {
            break;
        }
        if (r->pos + n > r->size) {
            fdk_free(buf);
            return false;
        }
        if (len + n > GIF_MAX_SUBBLOCKS) {
            fdk_free(buf);
            return false;
        }
        if (len + n > cap) {
            size_t ncap = (cap == 0) ? 256 : cap * 2;
            while (ncap < len + n) {
                ncap *= 2;
            }
            fdk_u8 *nb = fdk_realloc(buf, ncap);
            if (nb == NULL) {
                fdk_free(buf);
                return false;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, r->data + r->pos, n);
        len += n;
        r->pos += n;
    }
    *out = buf;
    *out_len = len;
    return true;
}

/* Skips a sub-block chain without storing it (comments, unknown
 * extensions). */
static bool skip_subblocks(gif_reader *r) {
    for (;;) {
        fdk_u8 n = 0;
        if (!rd_u8(r, &n)) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (!rd_skip(r, n)) {
            return false;
        }
    }
}

/* ---- the GIF LZW variant ----
 *
 * Variable code size min+1 .. 12, clear code resets the table, EOI
 * ends the stream, codes below the first free slot are literals,
 * and the KwKwK case (code == next free) emits the previous
 * sequence plus its own first byte. Output is bounded to out_cap:
 * the decoder stops filling but keeps consuming cleanly so a
 * short/lying stream cannot overflow. */

#define LZW_MAX_CODES 4096

typedef struct {
    fdk_u16 prefix[LZW_MAX_CODES];
    fdk_u8 suffix[LZW_MAX_CODES];
    fdk_u16 len[LZW_MAX_CODES]; /* bytes in the sequence */
} lzw_table;

static bool gif_lzw_decode(const fdk_u8 *data, size_t data_len,
                           fdk_u8 min_code_size, fdk_u8 *out,
                           size_t out_cap, size_t *out_written) {
    if (min_code_size < 2 || min_code_size > 11) {
        return false; /* spec: 2..8; 11 tolerates hostile files */
    }
    fdk_u16 clear = (fdk_u16)(1u << min_code_size);
    fdk_u16 eoi = (fdk_u16)(clear + 1);
    fdk_u16 next = (fdk_u16)(eoi + 1);
    fdk_u32 code_size = (fdk_u32)min_code_size + 1;

    lzw_table *tab = fdk_alloc(sizeof(*tab));
    if (tab == NULL) {
        return false;
    }
    for (fdk_u16 i = 0; i < clear; i++) {
        tab->prefix[i] = 0xFFFF;
        tab->suffix[i] = (fdk_u8)i;
        tab->len[i] = 1;
    }

    /* Bit reader over the concatenated sub-block bytes. */
    size_t byte_pos = 0;
    fdk_u32 bit_buf = 0;
    fdk_u32 bit_cnt = 0;

    fdk_u16 prev = 0xFFFF;
    size_t written = 0;
    fdk_u8 stack[LZW_MAX_CODES];
    bool ok = true;
    bool done = false;

    while (!done) {
        /* Pull the next code. */
        while (bit_cnt < code_size) {
            if (byte_pos >= data_len) {
                done = true; /* stream ended: accept if output full */
                ok = (written >= out_cap);
                goto finish;
            }
            bit_buf |= (fdk_u32)data[byte_pos++] << bit_cnt;
            bit_cnt += 8;
        }
        fdk_u16 code = (fdk_u16)(bit_buf & ((1u << code_size) - 1u));
        bit_buf >>= code_size;
        bit_cnt -= code_size;

        if (code == clear) {
            next = (fdk_u16)(eoi + 1);
            code_size = (fdk_u32)min_code_size + 1;
            prev = 0xFFFF;
            continue;
        }
        if (code == eoi) {
            break;
        }

        fdk_u16 emit = code;
        fdk_u8 first_byte = 0;
        bool kwkwk = false;
        if (code < next) {
            /* Existing entry: its first byte rides the chain. */
            fdk_u16 walk = code;
            while (walk != 0xFFFF) {
                first_byte = tab->suffix[walk];
                walk = tab->prefix[walk];
            }
        } else if (code == next && prev != 0xFFFF) {
            /* KwKwK: the code being defined right now — the sequence
             * is the PREVIOUS one plus its own first byte. */
            emit = prev;
            kwkwk = true;
            fdk_u16 walk = prev;
            while (walk != 0xFFFF) {
                first_byte = tab->suffix[walk];
                walk = tab->prefix[walk];
            }
        } else {
            /* Corrupt code: out of range. */
            ok = false;
            break;
        }

        /* Emit the sequence (reversed chain walk onto a stack). */
        fdk_u16 n = 0;
        fdk_u16 walk = emit;
        while (walk != 0xFFFF && n < LZW_MAX_CODES) {
            stack[n++] = tab->suffix[walk];
            walk = tab->prefix[walk];
        }
        if (walk != 0xFFFF) {
            ok = false; /* cycle/corruption guard */
            break;
        }
        while (n > 0 && written < out_cap) {
            out[written++] = stack[--n];
        }
        if (kwkwk && written < out_cap) {
            /* The KwKwK sequence's LAST byte: the previous
             * sequence's first byte, appended after its chain. */
            out[written++] = first_byte;
        }
        if (n > 0) {
            /* Output overrun: the frame's w*h was a lie. Tolerate by
             * discarding the rest of THIS code; the outer bound
             * check below fails the decode if the stream keeps
             * producing. */
            ok = false;
        }

        /* Add the new table entry (prev + first of current). */
        if (prev != 0xFFFF && next < LZW_MAX_CODES) {
            tab->prefix[next] = prev;
            tab->suffix[next] = first_byte;
            tab->len[next] = (fdk_u16)(tab->len[prev] + 1);
            next++;
            if (next == (fdk_u16)(1u << code_size) &&
                code_size < 12) {
                code_size++;
            }
        }
        prev = code;
    }

finish:
    if (written < out_cap) {
        /* Short frame: the geometry promised w*h indices and the
         * stream did not deliver. Fail honestly (the caller frees
         * the frame). */
        ok = false;
    }
    fdk_free(tab);
    *out_written = written;
    return ok;
}

/* ---- the frame + gif objects ---- */

typedef struct gif_frame {
    fdk_u16 left, top, w, h;
    fdk_u8 disposal;         /* the GCE method (0..7; 4..7 unreserved
                              * are treated as 0, the spec's advice) */
    bool has_transparent;
    fdk_u8 transparent_index;
    fdk_u16 delay_cs;
    fdk_u8 *indices;         /* w*h, de-interlaced, fdk_alloc'd     */
    fdk_u8 *table;           /* active color table (GCT copy ptr or
                              * LCT copy); 3*n entries; owned when
                              * table_owned                              */
    fdk_u16 table_n;
    bool table_owned;
} gif_frame;

struct fdk_gif {
    fdk_i32 width, height;
    fdk_u8 *gct;        /* owned; NULL when absent          */
    fdk_u16 gct_n;      /* entries                          */
    fdk_u8 bg_index;
    gif_frame *frames;
    fdk_u32 nframes;
    fdk_u32 loop_count;
};

/* ---- parsing ---- */

/* The GCE state pending the next image descriptor (GCE precedes its
 * frame in the stream). */
typedef struct {
    fdk_u8 disposal;
    bool has_transparent;
    fdk_u8 transparent_index;
    fdk_u16 delay_cs;
} gif_gce_pending;

static fdk_result gif_parse(const fdk_u8 *data, size_t size,
                            fdk_gif **out_gif) {
    if (data == NULL || size < 13) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (memcmp(data, "GIF87a", 6) != 0 &&
        memcmp(data, "GIF89a", 6) != 0) {
        return FDK_ERR_UNSUPPORTED;
    }
    gif_reader r = {data, size, 6};

    fdk_u16 w = 0, h = 0;
    fdk_u8 packed = 0;
    if (!rd_u16(&r, &w) || !rd_u16(&r, &h) || !rd_u8(&r, &packed)) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (w == 0 || h == 0 || w > GIF_MAX_DIM || h > GIF_MAX_DIM) {
        return FDK_ERR_LIMIT;
    }
    fdk_u8 bg = 0, aspect = 0;
    if (!rd_u8(&r, &bg) || !rd_u8(&r, &aspect)) {
        return FDK_ERR_UNSUPPORTED;
    }
    (void)aspect;

    fdk_gif *g = fdk_alloc(sizeof(*g));
    if (g == NULL) {
        return FDK_ERR_SURFACE_CREATE;
    }
    memset(g, 0, sizeof(*g));
    g->width = w;
    g->height = h;
    g->bg_index = bg;

    if (packed & 0x80) {
        fdk_u16 gct_n = (fdk_u16)(1u << ((packed & 0x07) + 1));
        if (!rd_skip(&r, (size_t)gct_n * 3)) {
            fdk_free(g);
            return FDK_ERR_UNSUPPORTED;
        }
        g->gct = fdk_alloc((size_t)gct_n * 3);
        if (g->gct == NULL) {
            fdk_free(g);
            return FDK_ERR_SURFACE_CREATE;
        }
        memcpy(g->gct, data + (r.pos - (size_t)gct_n * 3),
               (size_t)gct_n * 3);
        g->gct_n = gct_n;
    }

    gif_gce_pending gce = {0, false, 0, 0};
    fdk_u32 cap = 0;

    for (;;) {
        fdk_u8 block = 0;
        if (!rd_u8(&r, &block)) {
            fdk_gif_destroy(g);
            return FDK_ERR_UNSUPPORTED; /* no trailer */
        }
        if (block == 0x3B) {
            break; /* trailer */
        }
        if (block == 0x21) {
            /* Extension. */
            fdk_u8 label = 0;
            if (!rd_u8(&r, &label)) {
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }
            if (label == 0xF9) {
                /* Graphic Control: one sub-block, 4 bytes of payload
                 * (+ terminator handled by read_subblocks). */
                fdk_u8 *payload = NULL;
                size_t plen = 0;
                if (!read_subblocks(&r, &payload, &plen)) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_UNSUPPORTED;
                }
                if (plen >= 4) {
                    gce.disposal = (fdk_u8)((payload[0] >> 2) & 0x07);
                    gce.has_transparent = (payload[0] & 0x01) != 0;
                    gce.transparent_index = payload[3];
                    gce.delay_cs =
                        (fdk_u16)(payload[1] |
                                  ((fdk_u16)payload[2] << 8));
                }
                fdk_free(payload);
            } else if (label == 0xFF) {
                /* Application extension: NETSCAPE2.0 carries the
                 * loop count. */
                fdk_u8 *payload = NULL;
                size_t plen = 0;
                if (!read_subblocks(&r, &payload, &plen)) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_UNSUPPORTED;
                }
                if (plen >= 11 + 3 &&
                    memcmp(payload, "NETSCAPE2.0", 11) == 0 &&
                    payload[11] == 1) {
                    g->loop_count = (fdk_u32)payload[12] |
                                    ((fdk_u32)payload[13] << 8);
                }
                fdk_free(payload);
            } else {
                /* Comment (0xFE) / plain text (0x01) / unknown. */
                if (!skip_subblocks(&r)) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_UNSUPPORTED;
                }
            }
            continue;
        }
        if (block == 0x2C) {
            /* Image descriptor. */
            fdk_u16 left = 0, top = 0, iw = 0, ih = 0;
            if (!rd_u16(&r, &left) || !rd_u16(&r, &top) ||
                !rd_u16(&r, &iw) || !rd_u16(&r, &ih)) {
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }
            fdk_u8 pk = 0;
            if (!rd_u8(&r, &pk)) {
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }
            if (iw == 0 || ih == 0 ||
                (size_t)left + iw > (size_t)w ||
                (size_t)top + ih > (size_t)h) {
                fdk_gif_destroy(g);
                return FDK_ERR_LIMIT; /* frame outside the canvas */
            }
            if (g->nframes >= GIF_MAX_FRAMES) {
                fdk_gif_destroy(g);
                return FDK_ERR_LIMIT;
            }

            /* Local color table. */
            fdk_u8 *table = NULL;
            fdk_u16 table_n = 0;
            bool table_owned = false;
            if (pk & 0x80) {
                fdk_u16 lct_n = (fdk_u16)(1u << ((pk & 0x07) + 1));
                if (!rd_skip(&r, (size_t)lct_n * 3)) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_UNSUPPORTED;
                }
                table = fdk_alloc((size_t)lct_n * 3);
                if (table == NULL) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_SURFACE_CREATE;
                }
                memcpy(table, data + (r.pos - (size_t)lct_n * 3),
                       (size_t)lct_n * 3);
                table_n = lct_n;
                table_owned = true;
            } else if (g->gct == NULL || g->gct_n == 0) {
                /* No color table at all: unusable frame. */
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            } else {
                /* GCT-referencing frames OWN A COPY: an embedded
                 * data stream (the resync below) can REPLACE the
                 * global table mid-file, and a borrowed pointer
                 * would dangle for the earlier frames. */
                table = fdk_alloc((size_t)g->gct_n * 3);
                if (table == NULL) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_SURFACE_CREATE;
                }
                memcpy(table, g->gct, (size_t)g->gct_n * 3);
                table_n = g->gct_n;
                table_owned = true;
            }

            /* LZW min code size + data sub-blocks. */
            fdk_u8 min_code = 0;
            if (!rd_u8(&r, &min_code)) {
                if (table_owned) {
                    fdk_free(table);
                }
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }
            fdk_u8 *stream = NULL;
            size_t stream_len = 0;
            if (!read_subblocks(&r, &stream, &stream_len)) {
                if (table_owned) {
                    fdk_free(table);
                }
                fdk_free(stream);
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }

            fdk_u8 *indices = fdk_alloc((size_t)iw * (size_t)ih);
            if (indices == NULL) {
                fdk_free(stream);
                if (table_owned) {
                    fdk_free(table);
                }
                fdk_gif_destroy(g);
                return FDK_ERR_SURFACE_CREATE;
            }
            size_t written = 0;
            bool ok = gif_lzw_decode(stream, stream_len, min_code,
                                      indices, (size_t)iw * (size_t)ih,
                                      &written);
            fdk_free(stream);
            if (!ok) {
                FDK_WARN("gif: LZW decode failed at frame %u "
                         "(wrote %zu of %u)",
                         (unsigned)(g->nframes + 1), written,
                         (unsigned)((size_t)iw * (size_t)ih));
                fdk_free(indices);
                if (table_owned) {
                    fdk_free(table);
                }
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }

            /* De-interlace: the four passes write rows 0,8,16.. then
             * 4,12.. then 2,6,10.. then 1,3,5.. — expand into true
             * row-major order. */
            if (pk & 0x40) {
                fdk_u8 *linear = fdk_alloc((size_t)iw * (size_t)ih);
                if (linear != NULL) {
                    size_t src_row = 0;
                    static const fdk_u8 starts[4] = {0, 4, 2, 1};
                    static const fdk_u8 steps[4] = {8, 8, 4, 2};
                    for (int pass = 0; pass < 4; pass++) {
                        for (fdk_u16 row = starts[pass]; row < ih;
                             row += steps[pass]) {
                            memcpy(linear + (size_t)row * iw,
                                   indices + src_row * iw, iw);
                            src_row++;
                        }
                    }
                    fdk_free(indices);
                    indices = linear;
                }
                /* linear == NULL: the alloc failed but the frame is
                 * still legal without de-interlacing? No — silent
                 * wrong rows are worse than an honest failure. */
                else {
                    fdk_free(indices);
                    if (table_owned) {
                        fdk_free(table);
                    }
                    fdk_gif_destroy(g);
                    return FDK_ERR_SURFACE_CREATE;
                }
            }

            /* Append the frame. */
            if (g->nframes == cap) {
                fdk_u32 ncap = (cap == 0) ? 16 : cap * 2;
                gif_frame *nf =
                    fdk_realloc(g->frames, (size_t)ncap *
                                               sizeof(gif_frame));
                if (nf == NULL) {
                    fdk_free(indices);
                    if (table_owned) {
                        fdk_free(table);
                    }
                    fdk_gif_destroy(g);
                    return FDK_ERR_SURFACE_CREATE;
                }
                g->frames = nf;
                cap = ncap;
            }
            gif_frame *fr = &g->frames[g->nframes++];
            fr->left = left;
            fr->top = top;
            fr->w = iw;
            fr->h = ih;
            fr->disposal = (gce.disposal > 3) ? 0 : gce.disposal;
            fr->has_transparent = gce.has_transparent;
            fr->transparent_index = gce.transparent_index;
            fr->delay_cs = gce.delay_cs;
            fr->indices = indices;
            fr->table = table;
            fr->table_n = table_n;
            fr->table_owned = table_owned;

            /* The GCE applies to exactly one frame. */
            gce = (gif_gce_pending){0, false, 0, 0};
            continue;
        }
        /* Unknown block type — ONE tolerated shape: an EMBEDDED
         * DATA STREAM. PIL's save_all writes a second complete
         * "GIF89a" + logical screen + global table into the same
         * FILE without a trailer between them (no 0x3B — the
         * bytes just re-sync on the signature), and browsers eat
         * it. The resync: same canvas required, the new global
         * table replaces the old (frames keep their own copies —
         * see the table ownership note above), parsing continues. */
        if (r.pos - 1 + 6 <= r.size &&
            memcmp(data + (r.pos - 1), "GIF8", 4) == 0 &&
            (data[r.pos - 1 + 4] == '7' || data[r.pos - 1 + 4] == '9') &&
            data[r.pos - 1 + 5] == 'a') {
            r.pos -= 1; /* the signature starts at the unknown byte */
            (void)rd_skip(&r, 6);
            fdk_u16 w2 = 0, h2 = 0;
            fdk_u8 packed2 = 0, bg2 = 0, aspect2 = 0;
            if (!rd_u16(&r, &w2) || !rd_u16(&r, &h2) ||
                !rd_u8(&r, &packed2) || !rd_u8(&r, &bg2) ||
                !rd_u8(&r, &aspect2)) {
                fdk_gif_destroy(g);
                return FDK_ERR_UNSUPPORTED;
            }
            (void)bg2;
            (void)aspect2;
            if (w2 != w || h2 != h) {
                fdk_gif_destroy(g);
                return FDK_ERR_LIMIT; /* a second logical screen with
                                       * different geometry: refuse */
            }
            if (packed2 & 0x80) {
                fdk_u16 gct2_n = (fdk_u16)(1u << ((packed2 & 0x07) + 1));
                fdk_u8 *gct2 = fdk_alloc((size_t)gct2_n * 3);
                if (gct2 == NULL) {
                    fdk_gif_destroy(g);
                    return FDK_ERR_SURFACE_CREATE;
                }
                if (!rd_bytes(&r, gct2, (size_t)gct2_n * 3)) {
                    fdk_free(gct2);
                    fdk_gif_destroy(g);
                    return FDK_ERR_UNSUPPORTED;
                }
                fdk_free(g->gct);
                g->gct = gct2;
                g->gct_n = gct2_n;
            }
            continue;
        }
        /* Any other unknown byte: the stream is corrupt. */
        fdk_gif_destroy(g);
        return FDK_ERR_UNSUPPORTED;
    }

    if (g->nframes == 0) {
        fdk_gif_destroy(g);
        return FDK_ERR_UNSUPPORTED; /* trailer with no frames */
    }
    /* Trim the frame array to size. */
    if (cap > g->nframes) {
        gif_frame *nf = fdk_realloc(g->frames,
                                    (size_t)g->nframes *
                                        sizeof(gif_frame));
        if (nf != NULL) {
            g->frames = nf;
        }
        /* realloc failure keeps the larger array: harmless. */
    }

    *out_gif = g;
    return FDK_OK;
}

/* ---- public API ---- */

fdk_result fdk_gif_load_bytes(const void *data, size_t size,
                              fdk_gif **out_gif) {
    if (data == NULL || out_gif == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_gif = NULL;
    fdk_gif *g = NULL;
    fdk_result r = gif_parse((const fdk_u8 *)data, size, &g);
    if (!fdk_ok(r)) {
        return r;
    }
    *out_gif = g;
    return FDK_OK;
}

fdk_result fdk_gif_load(const char *path, fdk_gif **out_gif) {
    if (path == NULL || out_gif == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_gif = NULL;
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return FDK_ERR_NOT_FOUND;
        }
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!S_ISREG(st.st_mode)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if ((fdk_u64)st.st_size > GIF_MAX_FILE_BYTES) {
        return FDK_ERR_LIMIT;
    }
    if (st.st_size < 13) {
        return FDK_ERR_UNSUPPORTED;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return FDK_ERR_NOT_FOUND;
    }
    size_t size = (size_t)st.st_size;
    fdk_u8 *buf = fdk_alloc(size);
    if (buf == NULL) {
        fclose(f);
        return FDK_ERR_SURFACE_CREATE;
    }
    if (fread(buf, 1, size, f) != size) {
        fdk_free(buf);
        fclose(f);
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fclose(f);

    fdk_gif *g = NULL;
    fdk_result r = fdk_gif_load_bytes(buf, size, &g);
    fdk_free(buf);
    if (!fdk_ok(r)) {
        return r;
    }
    FDK_DEBUG("gif decoded (%dx%d, %u frames, loop %u)", g->width,
              g->height, (unsigned)g->nframes,
              (unsigned)g->loop_count);
    *out_gif = g;
    return FDK_OK;
}

void fdk_gif_destroy(fdk_gif *gif) {
    if (gif == NULL) {
        return;
    }
    for (fdk_u32 i = 0; i < gif->nframes; i++) {
        fdk_free(gif->frames[i].indices);
        if (gif->frames[i].table_owned) {
            fdk_free(gif->frames[i].table);
        }
    }
    fdk_free(gif->frames);
    fdk_free(gif->gct);
    fdk_free(gif);
}

fdk_u32 fdk_gif_frame_count(const fdk_gif *gif) {
    return (gif != NULL) ? gif->nframes : 0;
}

void fdk_gif_canvas_size(const fdk_gif *gif, fdk_i32 *out_w,
                         fdk_i32 *out_h) {
    if (out_w != NULL) {
        *out_w = 0;
    }
    if (out_h != NULL) {
        *out_h = 0;
    }
    if (gif == NULL) {
        return;
    }
    if (out_w != NULL) {
        *out_w = gif->width;
    }
    if (out_h != NULL) {
        *out_h = gif->height;
    }
}

fdk_u32 fdk_gif_loop_count(const fdk_gif *gif) {
    return (gif != NULL) ? gif->loop_count : 0;
}

fdk_u32 fdk_gif_frame_delay_ms(const fdk_gif *gif, fdk_u32 index) {
    if (gif == NULL || index >= gif->nframes) {
        return 0;
    }
    fdk_u32 ms = (fdk_u32)gif->frames[index].delay_cs * 10u;
    if (ms == 0 || ms < 20u) {
        ms = 100u; /* the compatibility floor */
    }
    return ms;
}

/* ---- the player ---- */

struct fdk_gif_player {
    const fdk_gif *gif;
    fdk_surface *dest;
    fdk_u32 index;      /* UINT32_MAX = nothing drawn */
    fdk_u32 *saved;     /* disposal-3 snapshot, canvas pixels */
    size_t saved_cap;
};

fdk_gif_player *fdk_gif_player_create(const fdk_gif *gif,
                                      fdk_surface *dest) {
    if (gif == NULL || dest == NULL) {
        return NULL;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(dest, &info))) {
        return NULL;
    }
    if (info.format != FDK_SURFACE_FORMAT_ARGB8888 ||
        info.width != gif->width || info.height != gif->height) {
        return NULL; /* the documented surface contract */
    }
    fdk_gif_player *p = fdk_alloc(sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    p->gif = gif;
    p->dest = dest;
    p->index = UINT32_MAX;
    p->saved = NULL;
    p->saved_cap = 0;
    return p;
}

void fdk_gif_player_destroy(fdk_gif_player *player) {
    if (player == NULL) {
        return;
    }
    fdk_free(player->saved);
    fdk_free(player);
}

void fdk_gif_player_reset(fdk_gif_player *player) {
    if (player == NULL) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(player->dest, &info))) {
        return;
    }
    for (fdk_i32 y = 0; y < info.height; y++) {
        fdk_u32 *row = info.pixels + (size_t)y * (size_t)info.stride;
        for (fdk_i32 x = 0; x < info.width; x++) {
            row[x] = 0u; /* fully transparent */
        }
    }
    player->index = UINT32_MAX;
}

/* Saves the canvas (before a disposal-3 frame draws). */
static bool player_save_canvas(fdk_gif_player *p, fdk_surface_info *info) {
    size_t need = (size_t)info->width * (size_t)info->height;
    if (p->saved == NULL || p->saved_cap < need) {
        fdk_u32 *ns = fdk_realloc(p->saved, need * sizeof(fdk_u32));
        if (ns == NULL) {
            return false;
        }
        p->saved = ns;
        p->saved_cap = need;
    }
    for (fdk_i32 y = 0; y < info->height; y++) {
        memcpy(p->saved + (size_t)y * (size_t)info->width,
               info->pixels + (size_t)y * (size_t)info->stride,
               (size_t)info->width * sizeof(fdk_u32));
    }
    return true;
}

/* Draws one frame's indices onto the canvas (transparency honored). */
static void player_draw_frame(fdk_gif_player *p,
                              const gif_frame *fr,
                              fdk_surface_info *info) {
    for (fdk_u16 fy = 0; fy < fr->h; fy++) {
        fdk_u32 *drow = info->pixels +
                        (size_t)(fr->top + fy) * (size_t)info->stride;
        const fdk_u8 *srow =
            fr->indices + (size_t)fy * (size_t)fr->w;
        for (fdk_u16 fx = 0; fx < fr->w; fx++) {
            fdk_u8 idx = srow[fx];
            if (fr->has_transparent && idx == fr->transparent_index) {
                continue; /* leave the canvas pixel */
            }
            if ((size_t)idx >= (size_t)fr->table_n) {
                /* Index beyond the table: transparent (the
                 * interoperable reading; a hostile index draws
                 * nothing rather than garbage). */
                continue;
            }
            const fdk_u8 *rgb = fr->table + (size_t)idx * 3;
            drow[fr->left + fx] =
                0xFF000000u | ((fdk_u32)rgb[0] << 16) |
                ((fdk_u32)rgb[1] << 8) | (fdk_u32)rgb[2];
        }
    }
    (void)p;
}

/* Disposes the CURRENT frame before the next draws. */
static void player_dispose(fdk_gif_player *p, const gif_frame *fr,
                           fdk_surface_info *info) {
    switch (fr->disposal) {
    case 2: {
        /* Restore to background = clear the rect to transparent. */
        for (fdk_u16 fy = 0; fy < fr->h; fy++) {
            fdk_u32 *row = info->pixels +
                           (size_t)(fr->top + fy) *
                               (size_t)info->stride;
            for (fdk_u16 fx = 0; fx < fr->w; fx++) {
                row[fr->left + fx] = 0u;
            }
        }
        break;
    }
    case 3: {
        /* Restore to the snapshot taken before this frame drew. */
        if (p->saved != NULL) {
            for (fdk_i32 y = 0; y < info->height; y++) {
                memcpy(info->pixels + (size_t)y * (size_t)info->stride,
                       p->saved + (size_t)y * (size_t)info->width,
                       (size_t)info->width * sizeof(fdk_u32));
            }
        }
        break;
    }
    default:
        /* 0 (unspecified) / 1 (do not dispose): the canvas keeps
         * this frame's pixels; the next frame composites over. */
        break;
    }
}

fdk_u32 fdk_gif_player_next(fdk_gif_player *player) {
    if (player == NULL || player->gif->nframes == 0) {
        return UINT32_MAX;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(player->dest, &info))) {
        return UINT32_MAX;
    }
    if (player->index == UINT32_MAX) {
        fdk_gif_player_reset(player);
        if (!fdk_ok(fdk_surface_get_info(player->dest, &info))) {
            return UINT32_MAX;
        }
    } else {
        /* Dispose the current frame. Disposal 3 needs the snapshot
         * taken BEFORE it drew — which the draw path saved. */
        player_dispose(player, &player->gif->frames[player->index],
                       &info);
    }

    fdk_u32 next = (player->index + 1) % player->gif->nframes;
    gif_frame *fr = &player->gif->frames[next];
    /* Snapshot before drawing when THIS frame will want restore-to-
     * previous on its way out. */
    if (fr->disposal == 3 && !player_save_canvas(player, &info)) {
        /* Alloc failure: degrade to disposal 0 (documented honesty:
         * the frame draws, its restore silently becomes keep). */
        fr->disposal = 0;
    }
    player_draw_frame(player, fr, &info);
    player->index = next;
    return next;
}

fdk_u32 fdk_gif_player_frame(const fdk_gif_player *player) {
    return (player != NULL) ? player->index : UINT32_MAX;
}
