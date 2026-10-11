/*
 * icon_svg.c — the SVG policy layer for the icon-theme engine
 * (internal)
 *
 * Everything security- and policy-shaped about rasterizing a themed
 * SVG lives HERE; the vendored nanosvg (nanosvg_impl.c) is the pure
 * parser/rasterizer. The discipline mirrors surface_image.c's rules
 * for attacker-controlled images — an installed icon theme is data
 * the toolkit did not author:
 *
 *   - reject-first: stat the path BEFORE any parse; refuse
 *     non-regular files and anything over 16 MiB (real icon SVGs
 *     are kilobytes of path data; the bound exists so a hostile
 *     "theme" cannot make FDK read unbounded text);
 *   - bounded: the raster is allocated at FDK's own requested size
 *     (never the SVG's declared dimensions), so a viewBox of
 *     100000x100000 cannot allocate anything larger than the icon
 *     cell the engine asked for;
 *   - honest-miss on empty output: some real-world SVGs use
 *     features nanosvg deliberately does not implement (CSS class
 *     fills, <text>, filters, masks). When a parse produces no
 *     drawable coverage the icon is reported as a MISS so the
 *     lookup chain keeps searching — other sizes, other themes, the
 *     procedural glyph floor — instead of drawing a blank cell;
 *   - fdk_alloc discipline: the file buffer and raster buffer are
 *     fdk_alloc'd here and freed here on every path; nanosvg's own
 *     internal allocations are all released by nsvgDelete /
 *     nsvgDeleteRasterizer, which this file calls on every path
 *     too (see nanosvg_impl.c's allocator note).
 *
 * Aspect handling: the SVG's document box (nanosvg folds the
 * viewBox transform into the shape coordinates during parse, so
 * image->width/height are the post-viewBox document dims) is
 * aspect-fit into the requested square cell and centered — the same
 * geometry fdk__row_icon_paint_image applies to raster icons, so a
 * wide SVG and a wide PNG land identically.
 *
 * Not part of the public API — never installed.
 */

#define FDK_LOG_TAG "theme"

#include "theme_internal.h"
#include "fdk/fdk_surface.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

/* The vendored parser/rasterizer's public surface (implementation
 * compiled once in nanosvg_impl.c, same pattern as stb_image). */
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Hard ceiling on SVG input size. Real icon SVGs are single-digit
 * kilobytes; 16 MiB is a hostile-input bound, not a theme bound. */
#define FDK_SVG_MAX_FILE_BYTES (16u * 1024u * 1024u)

/* An icon whose raster covers fewer pixels than this is treated as
 * a parse miss: nanosvg leaves the raster untouched (all zero
 * alpha) when nothing drawable existed, and a legitimately usable
 * icon at 16 px always covers far more than this floor. */
#define FDK_SVG_MIN_COVERAGE 4

static fdk_result it_svg_read_all(const char *path, char **out_buf,
                                  size_t *out_len) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return FDK_ERR_NOT_FOUND;
    }
    if (!S_ISREG(st.st_mode)) {
        return FDK_ERR_NOT_A_FILE;
    }
    if (st.st_size <= 0) {
        return FDK_ERR_UNSUPPORTED;
    }
    if ((unsigned long long)st.st_size >
        (unsigned long long)FDK_SVG_MAX_FILE_BYTES) {
        FDK_WARN("icon SVG too large to raster (%lld bytes, max %u): %s",
                 (long long)st.st_size, FDK_SVG_MAX_FILE_BYTES, path);
        return FDK_ERR_UNSUPPORTED;
    }
    size_t len = (size_t)st.st_size;
    /* +1 so the buffer is always NUL-terminated for nsvgParse. */
    char *buf = fdk_alloc(len + 1);
    if (buf == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fdk_free(buf);
        return FDK_ERR_NOT_A_FILE;
    }
    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        fdk_free(buf);
        return FDK_ERR_UNSUPPORTED; /* shrank or errored mid-read */
    }
    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return FDK_OK;
}

fdk_result fdk__icon_svg_raster(const char *path, fdk_i32 size,
                                fdk_surface **out_surface) {
    if (path == NULL || out_surface == NULL || size <= 0 ||
        size > 16384) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_surface = NULL;

    char *buf = NULL;
    size_t len = 0;
    fdk_result r = it_svg_read_all(path, &buf, &len);
    if (r != FDK_OK) {
        return r;
    }

    /* Parse. nsvgParse modifies the buffer in place — fine, it is
     * ours. units="px" with dpi 96 keeps document coordinates in
     * CSS pixels (the unit icon themes author in). */
    NSVGimage *image = nsvgParse(buf, "px", 96.0f);
    fdk_free(buf);
    if (image == NULL || image->width <= 0.0f || image->height <= 0.0f) {
        if (image != NULL) {
            nsvgDelete(image);
        }
        return FDK_ERR_UNSUPPORTED;
    }

    /* Aspect-fit the document box into the requested square cell. */
    float doc_w = image->width;
    float doc_h = image->height;
    float scale = (float)size / (doc_w > doc_h ? doc_w : doc_h);
    float draw_w = doc_w * scale;
    float draw_h = doc_h * scale;
    float tx = ((float)size - draw_w) * 0.5f;
    float ty = ((float)size - draw_h) * 0.5f;

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (rast == NULL) {
        nsvgDelete(image);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    unsigned char *rgba = fdk_alloc((size_t)size * (size_t)size * 4u);
    if (rgba == NULL) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    nsvgRasterize(rast, image, tx, ty, scale, rgba, size, size,
                  (int)size * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);

    /* Coverage gate: nanosvg leaves the raster untouched when
     * nothing was drawable (unsupported-feature SVGs). Fewer than
     * FDK_SVG_MIN_COVERAGE covered pixels = the honest miss — the
     * caller keeps searching the chain rather than showing blank. */
    size_t covered = 0;
    for (size_t i = 3; i < (size_t)size * (size_t)size * 4u; i += 4) {
        if (rgba[i] != 0) {
            covered++;
        }
    }
    if (covered < FDK_SVG_MIN_COVERAGE) {
        FDK_INFO("icon SVG has no drawable coverage (unsupported "
                 "features?): treated as a miss: %s",
                 path);
        fdk_free(rgba);
        return FDK_ERR_UNSUPPORTED;
    }

    /* RGBA8 (straight alpha) -> FDK's ARGB8888 u32 surface. The
     * _format variant is load-bearing: plain fdk_surface_create
     * yields XRGB8888 (opaque), and an icon cell needs the
     * transparent surround ARGB8888 starts with. */
    fdk_surface *surf = NULL;
    if (fdk_surface_create_format(size, size,
                                  FDK_SURFACE_FORMAT_ARGB8888,
                                  &surf) != FDK_OK) {
        fdk_free(rgba);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_surface_info info;
    if (fdk_surface_get_info(surf, &info) != FDK_OK ||
        info.format != FDK_SURFACE_FORMAT_ARGB8888) {
        fdk_surface_destroy(surf);
        fdk_free(rgba);
        return FDK_ERR_UNSUPPORTED;
    }
    for (fdk_i32 y = 0; y < size; y++) {
        fdk_u32 *row = info.pixels + (size_t)y * (size_t)info.stride;
        const unsigned char *src =
            rgba + (size_t)y * (size_t)size * 4u;
        for (fdk_i32 x = 0; x < size; x++) {
            fdk_u32 a = (fdk_u32)src[(size_t)x * 4u + 3u];
            fdk_u32 rr = (fdk_u32)src[(size_t)x * 4u + 0u];
            fdk_u32 gg = (fdk_u32)src[(size_t)x * 4u + 1u];
            fdk_u32 bb = (fdk_u32)src[(size_t)x * 4u + 2u];
            row[x] = (a << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }
    fdk_free(rgba);
    *out_surface = surf;
    return FDK_OK;
}
