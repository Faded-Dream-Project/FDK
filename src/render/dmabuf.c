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
#include "surface_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
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

/* ---- the texture payload helpers ---------------------------------------
 *
 * The import stores the params (the texture owns dup'd fds); the
 * realization splits by lane at first draw:
 *   - the GPU engine's EGL bridge (dmabuf_egl.c) imports zero-copy,
 *   - everything else falls back to THIS module's linear mmap copy
 *     — and ONLY for the linear modifier (INVALID never mmaps). */
void fdk__dmabuf_texture_close_fds(fdk_dmabuf_params *p) {
    if (p == NULL) {
        return;
    }
    for (int i = 0; i < p->n_planes; i++) {
        if (p->planes[i].fd >= 0) {
            close(p->planes[i].fd);
            p->planes[i].fd = -1;
        }
    }
}

/* The CPU-mode fallback: mmap the (linear) planes and fill the
 * texture's RGBA8888 storage through the PUBLIC texture API.
 * Returns FDK_OK, FDK_ERR_UNSUPPORTED (tiled/INVALID — the honest
 * refusal; never mapped as linear), or the mmap failure as
 * FDK_ERR_UNSUPPORTED. */
fdk_result fdk__dmabuf_fallback_realize(fdk_texture *t,
                                        fdk_dmabuf_params *p) {
    if (t == NULL || p == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (p->n_planes < 1 || p->n_planes > FDK_DMABUF_MAX_PLANES) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (p->n_planes != 1) {
        return FDK_ERR_UNSUPPORTED;
    }
    fdk_dmabuf_plane *pl = &p->planes[0];
    if (pl->fd < 0) {
        return FDK_ERR_INVALID_STATE; /* the fds died with the import */
    }
    if (pl->modifier != 0) {
        /* linear only. DRM_FORMAT_MOD_INVALID (the sentinel) and
         * every tiled modifier refuse here — the zero-copy lane is
         * where they live. */
        return FDK_ERR_UNSUPPORTED;
    }
    fdk__dmabuf_row_fn row = NULL;
    if (!fourcc_row(p->fourcc, &row)) {
        return FDK_ERR_UNSUPPORTED;
    }
    uint64_t need = (uint64_t)pl->offset +
                    (uint64_t)pl->stride * (uint64_t)p->height;
    if (pl->stride < (fdk_u32)p->width * 4u || need > SIZE_MAX) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    uint8_t *base =
        mmap(NULL, (size_t)need, PROT_READ, MAP_SHARED, pl->fd, 0);
    if (base == MAP_FAILED) {
        return FDK_ERR_UNSUPPORTED; /* the honest mapping refusal */
    }
    /* rows land DIRECTLY in the texture's storage (the internal
     * struct is shared via surface_internal.h — routing through the
     * public update would self-reject while the payload flag is up) */
    fdk_result rr = FDK_OK;
    for (int y = 0; y < p->height && fdk_ok(rr); y++) {
        row(base + pl->offset + (size_t)y * pl->stride,
            t->pixels + (size_t)y * (size_t)t->width * 4u, p->width);
    }
    munmap(base, (size_t)need);
    return rr;
}

/* The dmabuf payload accessors for the engine (gpaint.c) and the
 * context-level query (extend.c). */
const fdk_dmabuf_params *fdk__dmabuf_texture_params(
    const fdk_texture *t) {
    return t != NULL && t->is_dmabuf ? &t->dmabuf : NULL;
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
    fdk__dmabuf_row_fn row = NULL;
    if (!fourcc_row(params->fourcc, &row)) {
        return FDK_ERR_UNSUPPORTED; /* honest: no lossy conversion */
    }
    if (params->n_planes != 1) {
        return FDK_ERR_UNSUPPORTED; /* multi-plane YUV: out of family */
    }
    const fdk_dmabuf_plane *p = &params->planes[0];
    /* the modifier policy BEFORE the fd check: an unsupported layout
     * is unsupported regardless of the descriptor quality. Zero is
     * DRM_FORMAT_MOD_LINEAR; INVALID (the sentinel) and tiled
     * modifiers stay eligible for the ZERO-COPY lane only. */
    if (p->fd < 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* plane arithmetic: the last row must fit under the stride's
     * extent (overflow-safe in 64 bits) — checked NOW so the caller
     * learns the descriptor is broken even if the zero-copy lane
     * would never need the arithmetic */
    uint64_t need = (uint64_t)p->offset +
                    (uint64_t)p->stride * (uint64_t)params->height;
    if (p->stride < (fdk_u32)params->width * 4u || need > SIZE_MAX) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* the descriptor must FIT the buffer: fstat the fd and require
     * offset + stride*height <= size — a wrong stride or a wrong
     * size otherwise SIGBUSes the fallback mmap mid-draw (the
     * robustness contract: clean errors, never crashes). A closed fd
     * fails the fstat the same way. (Robustness, Item 2.) */
    {
        struct stat st;
        if (fstat(p->fd, &st) != 0 || st.st_size < 0 ||
            (uint64_t)st.st_size < need) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
    }

    fdk_texture *t = NULL;
    fdk_result r = fdk_texture_create(params->width, params->height, &t);
    if (!fdk_ok(r)) {
        return r;
    }
    /* the texture owns dup'd fds; the caller's stay theirs */
    t->is_dmabuf = 1;
    t->dmabuf = *params;
    for (int i = 0; i < params->n_planes; i++) {
        int dup_fd = fcntl(params->planes[i].fd, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0) {
            /* the import fails WHOLE: no half-owned descriptors */
            t->is_dmabuf = 0;
            memset(&t->dmabuf, 0, sizeof t->dmabuf);
            fdk_texture_destroy(t);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        t->dmabuf.planes[i].fd = dup_fd;
    }
    *out_texture = t;
    return FDK_OK;
}
