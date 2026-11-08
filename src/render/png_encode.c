#define FDK_LOG_TAG "render"

/*
 * png_encode.c — the PNG encoder (1.4.9)
 *
 * A complete, honest PNG writer in ~150 lines over zlib: the
 * signature, IHDR (8-bit RGBA), one IDAT (zlib compress2 of
 * filter-0 scanlines), IEND — chunk CRCs through zlib's crc32().
 * Row filters are deliberately NONE (filter byte 0 every scanline):
 * the compression ratio loss is real but small next to correctness,
 * and Paeth is a recorded future optimization, not a v1 need.
 *
 * The clipboard's image/png target and the public
 * fdk_surface_save_png ride this encoder; nothing else in the
 * toolkit writes PNGs.
 */

#include "surface_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <zlib.h>
#include <stdio.h>
#include <string.h>

/* One PNG chunk: [len BE32][type 4][data][crc32 BE32]. */
static void put_be32(unsigned char *p, fdk_u32 v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)(v);
}

static fdk_u32 chunk_crc(const unsigned char *type,
                         const unsigned char *data, size_t len) {
    uLong crc = crc32(0, NULL, 0);
    crc = crc32(crc, type, 4);
    crc = crc32(crc, data, (uInt)len);
    return (fdk_u32)crc;
}

/* Appends a chunk into a growing byte vector. */
static bool append_chunk(unsigned char **buf, size_t *len,
                         size_t *cap, const unsigned char *type,
                         const unsigned char *data, size_t data_len) {
    size_t need = *len + 12 + data_len;
    if (need > *cap) {
        size_t cap2 = (*cap == 0) ? 256 : *cap;
        while (cap2 < need) {
            cap2 *= 2;
        }
        unsigned char *grown = fdk_realloc(*buf, cap2);
        if (grown == NULL) {
            return false;
        }
        *buf = grown;
        *cap = cap2;
    }
    unsigned char *at = *buf + *len;
    put_be32(at, (fdk_u32)data_len);
    memcpy(at + 4, type, 4);
    if (data_len > 0) {
        memcpy(at + 8, data, data_len);
    }
    put_be32(at + 8 + data_len,
             chunk_crc(type, data, data_len));
    *len = need;
    return true;
}

/* Encodes `surface` as a complete PNG image stream. The output
 * buffer is heap-owned (*out freed with fdk_free). Fails with
 * FDK_ERR_OUT_OF_MEMORY on any allocation, FDK_ERR_INVALID_ARGUMENT
 * on NULLs. Empty surfaces are refused (a 0-dimension PNG is
 * meaningless). */
fdk_result fdk__png_encode(const fdk_surface *surface,
                           unsigned char **out, size_t *out_len) {
    if (surface == NULL || out == NULL || out_len == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_surface_info info;
    fdk_result r = fdk_surface_get_info(
        (fdk_surface *)(uintptr_t)surface, &info);
    if (!fdk_ok(r)) {
        return r;
    }
    if (info.width <= 0 || info.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* The raw filter-0 scanline buffer: 1 + w*4 bytes per row. */
    size_t w = (size_t)info.width;
    size_t h = (size_t)info.height;
    if (w > (SIZE_MAX - 1) / 4 / (h > 0 ? h : 1)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    size_t raw_len = (w * 4 + 1) * h;
    unsigned char *raw = fdk_alloc(raw_len);
    if (raw == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    for (size_t y = 0; y < h; y++) {
        unsigned char *row = raw + y * (w * 4 + 1);
        row[0] = 0; /* filter: None */
        const fdk_u32 *src = info.pixels + y * (size_t)info.stride;
        for (size_t x = 0; x < w; x++) {
            fdk_u32 px = src[x];
            row[1 + x * 4 + 0] = (unsigned char)((px >> 16) & 0xFFu);
            row[1 + x * 4 + 1] = (unsigned char)((px >> 8) & 0xFFu);
            row[1 + x * 4 + 2] = (unsigned char)(px & 0xFFu);
            row[1 + x * 4 + 3] = (unsigned char)((px >> 24) & 0xFFu);
        }
    }
    /* Compress the scanlines (zlib stream: exactly the IDAT body). */
    uLongf comp_cap = compressBound((uLong)raw_len);
    unsigned char *comp = fdk_alloc(comp_cap);
    if (comp == NULL) {
        fdk_free(raw);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    uLongf comp_len = comp_cap;
    int zr = compress2(comp, &comp_len, raw, (uLong)raw_len,
                       Z_BEST_SPEED);
    fdk_free(raw);
    if (zr != Z_OK) {
        fdk_free(comp);
        FDK_WARN("png: zlib compress failed (%d)", zr);
        return FDK_ERR_PLATFORM;
    }

    unsigned char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D,
                                         0x0A, 0x1A, 0x0A};
    /* Signature. */
    if (cap < 8) {
        cap = 256;
        buf = fdk_alloc(cap);
        if (buf == NULL) {
            fdk_free(comp);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    memcpy(buf, sig, 8);
    len = 8;

    /* IHDR. */
    unsigned char ihdr[13];
    put_be32(ihdr + 0, (fdk_u32)w);
    put_be32(ihdr + 4, (fdk_u32)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* color type: RGBA */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter: adaptive (per-scanline byte) */
    ihdr[12] = 0; /* interlace: none */
    if (!append_chunk(&buf, &len, &cap, (const unsigned char *)"IHDR",
                      ihdr, sizeof(ihdr))) {
        fdk_free(buf);
        fdk_free(comp);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* IDAT (one chunk — readers handle multi-chunk, one is legal). */
    if (!append_chunk(&buf, &len, &cap, (const unsigned char *)"IDAT",
                      comp, comp_len)) {
        fdk_free(buf);
        fdk_free(comp);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(comp);
    /* IEND. */
    if (!append_chunk(&buf, &len, &cap, (const unsigned char *)"IEND",
                      NULL, 0)) {
        fdk_free(buf);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    *out = buf;
    *out_len = len;
    return FDK_OK;
}

/* ---- public surface ---- */

fdk_result fdk_surface_save_png(const fdk_surface *surface,
                                const char *path) {
    if (path == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    unsigned char *bytes = NULL;
    size_t len = 0;
    fdk_result r = fdk__png_encode(surface, &bytes, &len);
    if (!fdk_ok(r)) {
        return r;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fdk_free(bytes);
        FDK_WARN("png: cannot open %s for writing", path);
        return FDK_ERR_PLATFORM;
    }
    size_t wrote = fwrite(bytes, 1, len, f);
    int ferr = ferror(f);
    fclose(f);
    fdk_free(bytes);
    if (wrote != len || ferr) {
        FDK_WARN("png: short write to %s", path);
        return FDK_ERR_PLATFORM;
    }
    return FDK_OK;
}
