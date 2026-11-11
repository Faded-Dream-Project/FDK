/* examples/g-dmabuf-video.c — P5 (g): the video-style dmabuf import.
 *
 * The video-player frame path: a decoder hands FDK one dma-buf per
 * frame (linux-dmabuf on Wayland, DRI3 on X11); FDK imports it into
 * a texture and the paint draws it — the same texture API as the
 * other examples, every render mode.
 *
 * Without a DRM device (or before the first frame arrives) the
 * example reports SKIP — never a fake PASS. On a DRM box with libgbm
 * it synthesizes "decoded frames" as linear GBM buffers and plays
 * them through the import.
 *
 * Wall check (P5): every wall this hits is an FDK bug to fix.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FOURCC(a, b, c, d)                                                   \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |          \
     ((uint32_t)(d) << 24))

static struct gbm_dev *g_gd = NULL;
static struct gbm_bo *g_bo = NULL;
static int g_drm_fd = -1;
static int g_buf_fd = -1;
static uint32_t g_stride = 0;

/* a "decoded frame": the video's current time as a color ramp */
static int make_frame(int t, int w, int h) {
    uint32_t stride = 0;
    void *map_data = NULL;
    uint32_t *px = NULL;
    /* POSIX dlsym returns void*; ISO C (pedantic) forbids the direct
     * conversion — the union idiom (the 2026-11-11 sweep). */
    union {
        void *obj;
        void *(*fn)(struct gbm_bo *, uint32_t, uint32_t, uint32_t,
                    uint32_t, uint32_t, uint32_t *, void **);
    } u_map;
    u_map.obj = dlsym(RTLD_DEFAULT, "gbm_bo_map");
    void *(*map)(struct gbm_bo *, uint32_t, uint32_t, uint32_t, uint32_t,
                 uint32_t, uint32_t *, void **) = u_map.fn;
    union {
        void *obj;
        void (*fn)(struct gbm_bo *, void *);
    } u_unmap;
    u_unmap.obj = dlsym(RTLD_DEFAULT, "gbm_bo_unmap");
    void (*unmap)(struct gbm_bo *, void *) = u_unmap.fn;
    if (map == NULL || unmap == NULL || g_bo == NULL) {
        return -1;
    }
    px = map(g_bo, 0, 0, (uint32_t)w, (uint32_t)h, 0x1, &stride,
             &map_data);
    if (px == NULL) {
        return -1;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t r = (uint32_t)(((unsigned)x * 255u / (unsigned)w +
                                     (unsigned)t * 7u) % 256u);
            uint32_t b = (uint32_t)((unsigned)y * 255u / (unsigned)h);
            px[(size_t)y * (stride / 4) + (size_t)x] =
                (0xFFu << 24) | (b << 16) | r;
        }
    }
    unmap(g_bo, map_data);
    g_stride = stride;
    return 0;
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    /* a DRM device is the honest SKIP: without one there is no
     * dma-buf to import and no video to play */
    char dev[64];
    for (int i = 0; i < 8 && g_drm_fd < 0; i++) {
        snprintf(dev, sizeof dev, "/dev/dri/renderD%d", 128 + i);
        g_drm_fd = open(dev, O_RDWR | O_CLOEXEC);
    }
    void *gbm = g_drm_fd >= 0 ? dlopen("libgbm.so.1", RTLD_NOW)
                              : NULL;
    union {
        void *obj;
        struct gbm_dev *(*fn)(int);
    } u_mkdev;
    u_mkdev.obj = gbm ? dlsym(gbm, "gbm_create_device") : NULL;
    struct gbm_dev *(*gbm_create_device)(int) = u_mkdev.fn;
    union {
        void *obj;
        struct gbm_bo *(*fn)(struct gbm_dev *, uint32_t, uint32_t,
                             uint32_t, uint32_t);
    } u_mkbo;
    u_mkbo.obj = gbm ? dlsym(gbm, "gbm_bo_create") : NULL;
    struct gbm_bo *(*gbm_bo_create)(struct gbm_dev *, uint32_t, uint32_t,
                                    uint32_t, uint32_t) = u_mkbo.fn;
    union {
        void *obj;
        int (*fn)(struct gbm_bo *);
    } u_getfd;
    u_getfd.obj = gbm ? dlsym(gbm, "gbm_bo_get_fd") : NULL;
    int (*gbm_bo_get_fd)(struct gbm_bo *) = u_getfd.fn;
    union {
        void *obj;
        void (*fn)(struct gbm_dev *);
    } u_devdestroy;
    u_devdestroy.obj = gbm ? dlsym(gbm, "gbm_device_destroy") : NULL;
    void (*gbm_device_destroy)(struct gbm_dev *) = u_devdestroy.fn;
    if (gbm_create_device == NULL || gbm_bo_create == NULL ||
        gbm_bo_get_fd == NULL || gbm_device_destroy == NULL) {
        fprintf(stderr,
                "SKIP: no DRM device / libgbm — dmabuf playback needs "
                "a DRM-equipped box (this is a SKIP, never a PASS)\n");
        fdk_shutdown(ctx);
        return 77;
    }
    g_gd = gbm_create_device(g_drm_fd);
    g_bo = gbm_bo_create(g_gd, 320, 180, FOURCC('A', 'R', '2', '4'),
                         1 << 6 /* GBM_BO_USE_LINEAR */);
    if (g_bo == NULL) {
        fprintf(stderr, "SKIP: GBM allocation failed\n");
        gbm_device_destroy(g_gd);
        fdk_shutdown(ctx);
        return 77;
    }
    g_buf_fd = gbm_bo_get_fd(g_bo);
    if (g_buf_fd < 0) {
        fprintf(stderr, "SKIP: gbm_bo_get_fd refused\n");
        gbm_device_destroy(g_gd);
        fdk_shutdown(ctx);
        return 77;
    }

    fdk_window *w = NULL;
    fdk_window_options o;
    memset(&o, 0, sizeof o);
    o.render_mode = FDK_RENDER_CPU;
    o.width = 320;
    o.height = 220;
    o.title = "fdk dmabuf video";
    if (!fdk_ok(fdk_window_create(ctx, &o, &w))) {
        fprintf(stderr, "no window\n");
        return 1;
    }
    fdk_window_show(w);

    for (int frame = 0; frame < 20; frame++) {
        fdk_pump_events(ctx, 33); /* ~30 fps */
        if (make_frame(frame, 320, 180) != 0) {
            fprintf(stderr, "SKIP: frame mapping failed\n");
            break;
        }
        fdk_dmabuf_params p;
        memset(&p, 0, sizeof p);
        p.struct_size = sizeof p;
        p.width = 320;
        p.height = 180;
        p.fourcc = FOURCC('A', 'R', '2', '4');
        p.n_planes = 1;
        p.planes[0].fd = g_buf_fd;
        p.planes[0].stride = g_stride;
        p.planes[0].modifier = 0; /* linear */
        fdk_texture *tex = NULL;
        fdk_result r = fdk_texture_import_dmabuf(&p, &tex);
        if (!fdk_ok(r)) {
            fprintf(stderr, "FAIL: import (%d)\n", (int)r);
            break;
        }
        fdk_surface *s = NULL;
        fdk_window_get_surface(w, &s);
        fdk_surface_fill(s, (fdk_color){ 0.05f, 0.05f, 0.08f, 1.0f });
        fdk_rect dst = { 0, 20, 320, 180 };
        fdk_surface_fill_rect(s, dst, (fdk_color){ 0, 0, 0, 1 });
        fdk_texture_draw(s, tex, dst);
        (void)fdk_surface_present(s);
        fdk_texture_destroy(tex); /* per-frame import: the video path */
    }
    printf("video: played 20 imported frames; clean exit\n");

    close(g_buf_fd);
    gbm_device_destroy(g_gd);
    close(g_drm_fd);
    fdk_shutdown(ctx);
    return 0;
}
