/* memfd_create is GNU; the test wants it before any include */
#define _GNU_SOURCE

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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include "render/surface_internal.h"

static fdk_context *g_ctx_dbg = NULL;
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
    p.planes[0].stride = 4; /* < width*4 */
    p.planes[0].fd = -1;
    CHECK(fdk_texture_import_dmabuf(&p, &t) == FDK_ERR_INVALID_ARGUMENT,
          "stride < width*bpp rejected");
    CHECK(t == NULL, "no texture leaked by any rejection");

    /* ---- the lazy-split semantics (no DRM needed: a memfd is
     * mmap-able exactly like a linear dmabuf, so the CPU fallback
     * path is REALLY testable here) -------------------------------- */
    int mfd = memfd_create("fdk-dmabuf-test", 0);
    int memfd_ok = mfd >= 0;
    CHECK(memfd_ok, "memfd for the fallback tests");
    if (mfd >= 0) {
        /* an 8x8 linear RA24 buffer: gradient + alpha */
        const int W = 8, H = 8;
        uint8_t buf[W * H * 4];
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint8_t *px = buf + (size_t)y * W * 4 + (size_t)x * 4;
                /* RA24 = RGBA8888: the bytes are R,G,B,A in memory */
                px[0] = (uint8_t)(y * 30);  /* R: the vertical ramp */
                px[1] = 10;                 /* G */
                px[2] = (uint8_t)(x * 30);  /* B */
                px[3] = 0xEE;               /* A */
            }
        }
        CHECK(write(mfd, buf, sizeof buf) == (ssize_t)sizeof buf,
              "the linear buffer written");

        /* LINEAR + the CPU fallback: import stores, draw realizes
         * through the mmap path and the pixels land */
        fdk_dmabuf_params lp = base_params();
        lp.width = W;
        lp.height = H;
        lp.fourcc = FOURCC('R', 'A', '2', '4');
        lp.planes[0].fd = mfd;
        lp.planes[0].stride = W * 4;
        lp.planes[0].modifier = 0; /* LINEAR */
        fdk_texture *lt = NULL;
        fdk_result lr = fdk_texture_import_dmabuf(&lp, &lt);
        CHECK(fdk_ok(lr), "the linear import stores (no mmap yet)");
        if (fdk_ok(lr)) {
            /* update is refused on a dmabuf texture (BEFORE the
             * draw — the fallback claims the storage after it) */
            fdk_rect one = {0, 0, 1, 1};
            uint8_t px4[4] = {1, 2, 3, 4};
            CHECK(fdk_texture_update(lt, one, px4, 4) ==
                      FDK_ERR_INVALID_STATE,
                  "texture_update refuses the dmabuf payload");
            fdk_surface *s2 = NULL;
            if (fdk_ok(fdk_surface_create(W, H, &s2))) {
                fdk_surface_fill(s2, (fdk_color){0, 0, 0, 1});
                fdk_rect dst = {0, 0, W, H};
                CHECK(fdk_ok(fdk_texture_draw(s2, lt, dst)),
                      "the CPU fallback draws");
                const uint8_t *pix =
                    (const uint8_t *)s2->fb.pixels;
                /* the fb is the surface's opaque BGRX form: alpha is
                 * the fill's — verify R and G only */
                /* the fb stride is in fdk_u32 PIXEL units (the
                 * documented fdk_surface_info contract) */
                const size_t stride_b = (size_t)s2->fb.stride * 4u;
                /* the draw's NEAREST sampling rounds at the edge:
                 * row 7 lands on 209, not 210 — tolerance 2 */
                int ok = pix[2] == 0 /* y=0,x=0: R=0 */ &&
                         pix[1] == 10 &&
                         pix[(size_t)(H - 1) * stride_b + 2] >=
                             (uint8_t)((H - 1) * 30 - 2);
                CHECK(ok, "the fallback pixels round-trip");
                fdk_surface_destroy(s2);
            }
            fdk_texture_destroy(lt);
        }

        /* INVALID modifier: import stores; the draw must answer
         * UNSUPPORTED — NEVER map it as linear */
        if (lseek(mfd, 0, SEEK_SET) == 0) {
            CHECK(write(mfd, buf, sizeof buf) == (ssize_t)sizeof buf,
                  "the buffer rewritten");
        }
        fdk_dmabuf_params ip = lp;
        ip.planes[0].modifier = ((uint64_t)1 << 56) - 1;
        fdk_texture *it = NULL;
        fdk_result ir = fdk_texture_import_dmabuf(&ip, &it);
        CHECK(fdk_ok(ir), "the INVALID import stores");
        if (fdk_ok(ir)) {
            fdk_surface *s3 = NULL;
            if (fdk_ok(fdk_surface_create(W, H, &s3))) {
                fdk_rect dst = {0, 0, W, H};
                CHECK(fdk_texture_draw(s3, it, dst) ==
                          FDK_ERR_UNSUPPORTED,
                      "INVALID never mmaps (draw refuses)");
                fdk_surface_destroy(s3);
            }
            fdk_texture_destroy(it);
        }
        /* the tiled modifier: the import STORES (the zero-copy lane
         * may serve it); the draw refuses without the EGL path */
        fdk_dmabuf_params tp = lp;
        tp.planes[0].modifier = 0x0200000000000002ull;
        fdk_texture *tt = NULL;
        if (fdk_ok(fdk_texture_import_dmabuf(&tp, &tt))) {
            fdk_surface *s4 = NULL;
            if (fdk_ok(fdk_surface_create(W, H, &s4))) {
                fdk_rect dst = {0, 0, W, H};
                CHECK(fdk_texture_draw(s4, tt, dst) ==
                          FDK_ERR_UNSUPPORTED,
                      "tiled draws UNSUPPORTED without the EGL path");
                fdk_surface_destroy(s4);
            }
            fdk_texture_destroy(tt);
        }
        close(mfd);
    }

    /* ---- the robustness matrix (Item 2): clean errors, never a
     * crash — a wrong stride, a wrong size, a closed fd ------------- */
    {
        int rfd = memfd_create("fdk-dmabuf-robust", 0);
        CHECK(rfd >= 0, "robustness memfd");
        if (rfd >= 0) {
            /* a SMALL buffer: 4x4 */
            uint8_t small[4 * 4 * 4];
            memset(small, 0x7F, sizeof small);
            CHECK(write(rfd, small, sizeof small) ==
                      (ssize_t)sizeof small,
                  "the small buffer written");

            /* wrong stride/size vs the REAL buffer: measure it, then
             * demand a descriptor that overruns it 4x (the gate is
             * honest about the actual size — never a fixed guess) */
            struct stat st_;
            fstat(rfd, &st_);
            fdk_i64 real_size = (fdk_i64)st_.st_size;
            fdk_texture *wt = NULL;
            fdk_dmabuf_params wp = base_params();
            wp.width = (fdk_i32)(real_size / 4); /* 1 row fits exactly */
            wp.height = 4;                       /* 4 rows: 4x overrun */
            wp.fourcc = FOURCC('R', 'A', '2', '4');
            wp.planes[0].fd = rfd;
            wp.planes[0].stride = (fdk_u32)wp.width * 4u;
            CHECK(fdk_texture_import_dmabuf(&wp, &wt) ==
                      FDK_ERR_INVALID_ARGUMENT,
                  "wrong stride vs the buffer size: clean refusal");
            CHECK(wt == NULL, "no texture on the stride refusal");

            /* wrong size: 4x40 (beyond the buffer end) */
            fdk_dmabuf_params hp = base_params();
            hp.width = 4;
            hp.height = 40;
            hp.fourcc = FOURCC('R', 'A', '2', '4');
            hp.planes[0].fd = rfd;
            hp.planes[0].stride = 4 * 4;
            CHECK(fdk_texture_import_dmabuf(&hp, &wt) ==
                      FDK_ERR_INVALID_ARGUMENT,
                  "wrong size vs the buffer size: clean refusal");

            /* closed fd: a positive number the kernel rejects */
            int dead = dup(rfd);
            close(dead);
            fdk_dmabuf_params dp = base_params();
            dp.width = 4;
            dp.height = 4;
            dp.fourcc = FOURCC('R', 'A', '2', '4');
            dp.planes[0].fd = dead;
            dp.planes[0].stride = 4 * 4;
            CHECK(fdk_texture_import_dmabuf(&dp, &wt) ==
                      FDK_ERR_INVALID_ARGUMENT,
                  "closed fd: clean refusal (fstat fails)");

            /* the honest control: the SAME buffer with the matching
             * descriptor imports fine */
            fdk_dmabuf_params okp = base_params();
            okp.width = 4;
            okp.height = 4;
            okp.fourcc = FOURCC('R', 'A', '2', '4');
            okp.planes[0].fd = rfd;
            okp.planes[0].stride = 4 * 4;
            CHECK(fdk_ok(fdk_texture_import_dmabuf(&okp, &wt)),
                  "the matching descriptor still imports");
            if (wt != NULL) {
                fdk_texture_destroy(wt);
            }
            close(rfd);
        }
    }

    /* ---- the driver's modifier query: only where an EGL lane
     * exists (the sandbox usually has none — SKIP honestly) ------- */
    if (!fdk_ok(fdk_init(&g_ctx_dbg, NULL))) {
        /* no context at all: the query test is done */
    } else {
        fdk_u64 mods[64];
        fdk_i32 n = 0, implicit = 0;
        fdk_result qr = fdk_texture_dmabuf_query(
            g_ctx_dbg, FOURCC('A', 'R', '2', '4'), mods, 64, &n,
            &implicit);
        if (fdk_ok(qr)) {
            printf("dmabuf query: %d modifiers for AR24 (implicit=%d)\n",
                   (int)n, (int)implicit);
            CHECK(n >= 0, "the modifier query ran");
        } else {
            printf("SKIP: no EGL lane for the modifier query (%d)\n",
                   (int)qr);
        }
        fdk_shutdown(g_ctx_dbg);
        g_ctx_dbg = NULL;
    }

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
