/* test_dmabuf.c — P4: the dma-buf import.
 *
 * Everything that can run without a DRM device runs everywhere:
 * the full validation matrix (struct contract, geometry, fourcc,
 * modifier, plane arithmetic) and the exact error codes.
 * The REAL round-trip (a GBM-generated linear buffer through the
 * texture path, pixels verified) reports SKIP — never PASS — when
 * no DRM device exists; run it on a DRM-equipped box (or CI runner).
 */
#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include "render/surface_internal.h"

static int g_fail = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        printf("[%s] %s\n", (cond) ? "ok" : "FAIL", name);                   \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

#define FOURCC(a, b, c, d)                                                   \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |          \
     ((uint32_t)(d) << 24))

static fdk_dmabuf_params base_params(void) {
    fdk_dmabuf_params p;
    memset(&p, 0, sizeof p);
    p.struct_size = sizeof p;
    p.width = 4;
    p.height = 4;
    p.fourcc = FOURCC('R', 'A', '2', '4'); /* RGBA8888 */
    p.n_planes = 1;
    return p;
}

int main(void) {
    fdk_texture *t = NULL;

    /* ---- the validation matrix --------------------------------------- */
    CHECK(fdk_texture_import_dmabuf(NULL, &t) == FDK_ERR_INVALID_ARGUMENT,
          "NULL params rejected");
    fdk_dmabuf_params p = base_params();
    p.struct_size = 8;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "wrong struct_size rejected");
    p = base_params();
    p.width = 0;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "width 0 rejected");
    p = base_params();
    p.height = -3;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "negative height rejected");
    p = base_params();
    p.width = 20000;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "oversize rejected");
    p = base_params();
    p.flags = 1;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "reserved flags rejected");
    p = base_params();
    p.n_planes = 0;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "0 planes rejected");
    p = base_params();
    p.n_planes = 5;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "5 planes rejected");
    p = base_params();
    p.fourcc = FOURCC('N', 'V', '1', '2');
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_UNSUPPORTED,
          "NV12 (YUV, multi-plane) honestly unsupported");
    p = base_params();
    p.fourcc = FOURCC('X', 'Y', 'Z', 'W');
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_UNSUPPORTED,
          "unknown fourcc honestly unsupported");
    p = base_params();
    p.planes[0].modifier = ((uint64_t)1 << 56) - 1; /* INVALID sentinel:
                                                       legal */
    p.planes[0].fd = -1;                           /* invalid fd */
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "negative fd rejected (with the INVALID modifier accepted)");
    p = base_params();
    p.planes[0].modifier = 0x0200000000000002ull; /* a tiled modifier */
    p.planes[0].fd = -1;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_UNSUPPORTED,
          "tiled modifier honestly unsupported (the EGLImage path)");
    p = base_params();
    p.planes[0].stride = 4; /* < width*4 */
    p.planes[0].fd = -1;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "stride < width*bpp rejected");
    CHECK(t == NULL, "no texture leaked by any rejection");

    /* ---- the real round-trip: SKIP without a DRM device --------------- */
    void *gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    int drm_fd = -1;
    char dev[64];
    for (int i = 0; i < 8 && drm_fd < 0; i++) {
        snprintf(dev, sizeof dev, "/dev/dri/renderD%d", 128 + i);
        drm_fd = open(dev, O_RDWR | O_CLOEXEC);
    }
    if (gbm == NULL || drm_fd < 0) {
        printf("SKIP: no DRM device or libgbm — the round-trip needs a "
               "DRM-equipped box (reports SKIP, never PASS, here)\n");
        printf(g_fail ? "DMABUF: %d FAILURES\n" : "DMABUF: all green\n",
               g_fail);
        return g_fail ? 1 : 0;
    }

    /* GBM: allocate a linear ARGB8888 bo, map it, paint a pattern,
     * export the dmabuf fd, import through FDK, verify the pixels */
    struct gbm_dev *(*gbm_create_device)(int) =
        dlsym(gbm, "gbm_create_device");
    struct gbm_bo *(*gbm_bo_create)(struct gbm_dev *, uint32_t, uint32_t,
                                    uint32_t, uint32_t) =
        dlsym(gbm, "gbm_bo_create");
    void *(*gbm_bo_map)(struct gbm_bo *, uint32_t, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t *, void **) =
        dlsym(gbm, "gbm_bo_map");
    void (*gbm_bo_unmap)(struct gbm_bo *, void *) =
        dlsym(gbm, "gbm_bo_unmap");
    int (*gbm_bo_get_fd)(struct gbm_bo *) = dlsym(gbm, "gbm_bo_get_fd");
    uint32_t (*gbm_bo_get_stride)(struct gbm_bo *) =
        dlsym(gbm, "gbm_bo_get_stride");
    void (*gbm_device_destroy)(struct gbm_dev *) =
        dlsym(gbm, "gbm_device_destroy");
    if (!gbm_create_device || !gbm_bo_create || !gbm_bo_map ||
        !gbm_bo_unmap || !gbm_bo_get_fd || !gbm_bo_get_stride ||
        !gbm_device_destroy) {
        printf("SKIP: libgbm symbols unavailable\n");
        close(drm_fd);
        printf(g_fail ? "DMABUF: %d FAILURES\n" : "DMABUF: all green\n",
               g_fail);
        return g_fail ? 1 : 0;
    }
    struct gbm_dev *gd = gbm_create_device(drm_fd);
    struct gbm_bo *bo = gbm_bo_create(gd, 8, 8,
                                      FOURCC('A', 'R', '2', '4'),
                                      1 << 6 /* GBM_BO_USE_LINEAR */);
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *px = gbm_bo_map(bo, 0, 0, 8, 8, 0x1 /* WRITE */,
                              &stride, &map_data);
    if (px == NULL || bo == NULL) {
        printf("SKIP: GBM bo allocation/map failed\n");
        if (bo != NULL) {
            gbm_device_destroy(gd);
        }
        close(drm_fd);
        printf(g_fail ? "DMABUF: %d FAILURES\n" : "DMABUF: all green\n",
               g_fail);
        return g_fail ? 1 : 0;
    }
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            /* bytes B,G,R,A in ARGB8888-LE: red ramp + full alpha */
            uint32_t v = (0xFFu << 24) | ((uint32_t)(x * 28) << 16) |
                         (uint32_t)(y * 28);
            px[y * (stride / 4) + x] = v;
        }
    }
    gbm_bo_unmap(bo, map_data);
    int buf_fd = gbm_bo_get_fd(bo);
    if (buf_fd < 0) {
        printf("SKIP: gbm_bo_get_fd refused\n");
        gbm_device_destroy(gd);
        close(drm_fd);
        printf(g_fail ? "DMABUF: %d FAILURES\n" : "DMABUF: all green\n",
               g_fail);
        return g_fail ? 1 : 0;
    }
    fdk_dmabuf_params rp = base_params();
    rp.width = 8;
    rp.height = 8;
    rp.fourcc = FOURCC('A', 'R', '2', '4');
    rp.planes[0].fd = buf_fd;
    rp.planes[0].stride = stride;
    rp.planes[0].modifier = 0; /* linear */
    fdk_result r = fdk_texture_import_dmabuf(&rp, &t);
    CHECK(fdk_ok(r), "the GBM buffer imports");
    if (fdk_ok(r)) {
        /* verify via draw + readback on a CPU surface: the texture
         * draws opaque; sample through fdk_surface + own pixels */
        fdk_surface *s = NULL;
        if (fdk_ok(fdk_surface_create(8, 8, &s))) {
            fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
            fdk_rect dst = { 0, 0, 8, 8 };
            CHECK(fdk_ok(fdk_texture_draw(s, t, dst)), "draws");
            /* the suite's internal readback (the fb struct) */
            fdk_u8 *pix = (fdk_u8 *)s->fb.pixels;
            int ok = 1;
            for (int i = 0; i < 8; i++) {
                const fdk_u8 *p4 = pix + (size_t)i * 8 * 4;
                if (p4[2] < 200 || p4[3] < 250) {
                    ok = 0;
                }
            }
            CHECK(ok, "the imported pixels round-trip");
            fdk_surface_destroy(s);
        }
        fdk_texture_destroy(t);
    }
    close(buf_fd);
    gbm_device_destroy(gd);
    close(drm_fd);

    printf(g_fail ? "DMABUF: %d FAILURES\n" : "DMABUF: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
