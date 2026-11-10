/*
 * dmabuf.c — dma-buf texture import (P4).
 *
 * The full validation-first importer: DRM fourcc table, modifier
 * policy, plane arithmetic, then the CPU mmap copy into the
 * texture's RGBA8888 storage. Every check runs before any mapping;
 * the errors are exact (INVALID_ARGUMENT for caller mistakes,
 * UNSUPPORTED for formats/modifiers FDK cannot convert honestly).
 *
 * The GPU-side zero-copy (EGLImage on the lane) is the documented
 * follow-up; the CPU copy here is complete and serves every render
 * mode — the imported texture is an ordinary fdk_texture.
 */
#define FDK_LOG_TAG "render"

#include "fdk/fdk_surface.h"
#include "core/alloc_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* DRM_FORMAT_MOD_* (linux/drm_fourcc.h values, spelled locally so no
 * libdrm header is needed — the constants are ABI-stable UAPI) */
#define FDK_DRM_FORMAT_MOD_INVALID (((uint64_t)1 << 56) - 1)
#define FDK_DRM_FORMAT_MOD_LINEAR 0

/* DRM fourccs (the four ASCII chars, little-endian packed) */
#define FDK_FOURCC(a, b, c, d)                                            \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |       \
     ((uint32_t)(d) << 24))
#define FDK_FOURCC_AR24 FDK_FOURCC('A', 'R', '2', '4') /* ABGR8888 */
#define FDK_FOURCC_AB24 FDK_FOURCC('A', 'B', '2', '4') /* ARGB8888 */
#define FDK_FOURCC_XR24 FDK_FOURCC('X', 'R', '2', '4') /* BGRX8888 */
#define FDK_FOURCC_BX24 FDK_FOURCC('B', 'X', '2', '4') /* RGBX8888 */
#define FDK_FOURCC_RA24 FDK_FOURCC('R', 'A', '2', '4') /* RGBA8888 */
#define FDK_FOURCC_BA24 FDK_FOURCC('B', 'A', '2', '4') /* BGRA8888 */

typedef void (*fdk__dmabuf_row_fn)(const uint8_t *src, uint8_t *dst,
                                   int width);

static void row_abgr8888(const uint8_t *src, uint8_t *dst, int width) {
    /* bytes B,G,R,A -> R,G,B,A */
    for (int x = 0; x < width; x++) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = src[x * 4 + 3];
    }
}

static void row_argb8888(const uint8_t *src, uint8_t *dst, int width) {
    /* bytes B,G,R,A (ARGB in LE word form) -> R,G,B,A */
    row_abgr8888(src, dst, width);
}

static void row_bgrx8888(const uint8_t *src, uint8_t *dst, int width) {
    /* bytes B,G,R,X -> R,G,B,A(255) */
    for (int x = 0; x < width; x++) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = 0xFF;
    }
}

static void row_rgbx8888(const uint8_t *src, uint8_t *dst, int width) {
    row_bgrx8888(src, dst, width);
}

static void row_rgba8888(const uint8_t *src, uint8_t *dst, int width) {
    memcpy(dst, src, (size_t)width * 4u);
}

static void row_bgra8888(const uint8_t *src, uint8_t *dst, int width) {
    row_abgr8888(src, dst, width);
}

static int fourcc_row(uint32_t fourcc, fdk__dmabuf_row_fn *out) {
    switch (fourcc) {
        case FDK_FOURCC_AR24:
        case FDK_FOURCC_AB24:
        case FDK_FOURCC_BA24:
            *out = row_abgr8888;
            return 1;
        case FDK_FOURCC_XR24:
        case FDK_FOURCC_BX24:
            *out = row_bgrx8888;
            return 1;
        case FDK_FOURCC_RA24:
            *out = row_rgba8888;
            return 1;
        default:
            return 0;
    }
}

/* One mmap per unique fd, shared by the planes that reference it. */
struct plane_map {
    int fd;
    uint8_t *addr;
    size_t len;
};

static uint8_t *map_for(struct plane_map *maps, int *n_maps, int fd,
                        size_t len) {
    for (int i = 0; i < *n_maps; i++) {
        if (maps[i].fd == fd) {
            return maps[i].addr;
        }
    }
    int slot = *n_maps;
    maps[slot].fd = fd;
    maps[slot].len = len;
    maps[slot].addr = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    *n_maps = slot + 1;
    return maps[slot].addr;
}

fdk_result fdk_texture_import_dmabuf(const fdk_dmabuf_params *params,
                                     fdk_texture **out_texture) {
    if (params == NULL || out_texture == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (params->struct_size != sizeof(fdk_dmabuf_params)) {
        return FDK_ERR_INVALID_ARGUMENT; /* the input-struct contract */
    }
    if (params->width <= 0 || params->height <= 0 ||
        params->width > 16384 || params->height > 16384) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (params->flags != 0) {
        return FDK_ERR_INVALID_ARGUMENT; /* reserved must be 0 */
    }
    if (params->n_planes < 1 || params->n_planes > FDK_DMABUF_MAX_PLANES) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* single-plane formats only in the supported table (the 8-bit
     * RGB family is always single-plane) */
    fdk__dmabuf_row_fn row = NULL;
    if (!fourcc_row(params->fourcc, &row)) {
        return FDK_ERR_UNSUPPORTED; /* honest: no lossy conversion */
    }
    if (params->n_planes != 1) {
        return FDK_ERR_UNSUPPORTED; /* multi-plane YUV: out of family */
    }
    const fdk_dmabuf_plane *p = &params->planes[0];
    /* the modifier policy BEFORE the fd check: an unsupported layout
     * is unsupported regardless of the descriptor quality */
    if (p->modifier != FDK_DRM_FORMAT_MOD_LINEAR &&
        p->modifier != FDK_DRM_FORMAT_MOD_INVALID) {
        return FDK_ERR_UNSUPPORTED; /* tiled memory: the EGLImage path */
    }
    if (p->fd < 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* plane arithmetic: the last row must fit under the stride's
     * extent (overflow-safe in 64 bits) */
    uint64_t need = (uint64_t)p->offset +
                    (uint64_t)p->stride * (uint64_t)params->height;
    if (p->stride < (fdk_u32)params->width * 4u || need > SIZE_MAX) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_texture *t = NULL;
    fdk_result r = fdk_texture_create(params->width, params->height, &t);
    if (!fdk_ok(r)) {
        return r;
    }
    struct plane_map maps[FDK_DMABUF_MAX_PLANES];
    int n_maps = 0;
    uint8_t *base = map_for(maps, &n_maps, p->fd, (size_t)need);
    if (base == MAP_FAILED) {
        /* the runtime lacks the mapping right (an anonymous fd, a
         * sealed memfd, a device that will not mmap) — the honest
         * unsupported, not a guess */
        int e = errno;
        fdk_texture_destroy(t);
        for (int i = 0; i < n_maps; i++) {
            if (maps[i].addr != MAP_FAILED) {
                munmap(maps[i].addr, maps[i].len);
            }
        }
        (void)e;
        return FDK_ERR_UNSUPPORTED;
    }
    /* fill through the PUBLIC texture API (one row at a time — no
     * texture internals needed here) */
    uint8_t *rowbuf = fdk_alloc((size_t)params->width * 4u);
    if (rowbuf == NULL) {
        fdk_texture_destroy(t);
        for (int i = 0; i < n_maps; i++) {
            if (maps[i].addr != MAP_FAILED) {
                munmap(maps[i].addr, maps[i].len);
            }
        }
        return FDK_ERR_OUT_OF_MEMORY;
    }
    for (int y = 0; y < params->height; y++) {
        row(base + p->offset + (size_t)y * p->stride, rowbuf,
            params->width);
        fdk_rect rrow = { 0, y, params->width, 1 };
        fdk_result rr = fdk_texture_update(t, rrow, rowbuf,
                                           params->width * 4);
        if (!fdk_ok(rr)) {
            fdk_free(rowbuf);
            fdk_texture_destroy(t);
            for (int i = 0; i < n_maps; i++) {
                if (maps[i].addr != MAP_FAILED) {
                    munmap(maps[i].addr, maps[i].len);
                }
            }
            return rr;
        }
    }
    fdk_free(rowbuf);
    for (int i = 0; i < n_maps; i++) {
        if (maps[i].addr != MAP_FAILED) {
            munmap(maps[i].addr, maps[i].len);
        }
    }
    *out_texture = t;
    return FDK_OK;
}
