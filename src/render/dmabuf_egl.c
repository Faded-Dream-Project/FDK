/*
 * dmabuf_egl.c — the zero-copy dma-buf import bridge (P4, extension
 * API v3).
 *
 * A self-contained libEGL bridge, dlopen'd exactly like the lanes
 * resolve their GL libraries (no new build dependency). The engine
 * calls in with the LANE'S CONTEXT CURRENT (the first flush is the
 * documented bring-up point); the bridge reads the current EGL
 * display/context, checks the import extensions, and realizes
 * EGLImage-backed GL textures.
 *
 * Extension truth per display:
 *   EGL_EXT_image_dma_buf_import        — plain import (linear +
 *                                         implicit/INVALID)
 *   EGL_EXT_image_dma_buf_import_modifiers — explicit modifier
 *                                         attributes (tiled/vendor)
 * eglQueryDmaBufFormatsEXT / eglQueryDmaBufModifiersEXT (the
 * modifiers extension) report the driver's per-fourcc truth.
 *
 * INVALID modifier semantics: the modifier attributes are OMITTED
 * from the EGLImage attrib list — the driver's implicit-modifier
 * path. It is never passed as an explicit value and never mapped as
 * linear.
 */
#include "dmabuf_egl_internal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* EGL constants (ABI-stable UAPI values; spelled locally so no EGL
 * header is needed — same policy as the lanes' dlsym tables) */
#define EGL_EXTENSIONS 0x3055
typedef int EGLint;
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#define EGL_LINUX_DRM_FOURCC_EXT 0x3271
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT 0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT 0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT 0x327A
#define EGL_DMA_BUF_PLANE3_FD_EXT 0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT 0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT 0x3442
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT 0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT 0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT 0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT 0x344A
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601

#define DRM_FORMAT_MOD_INVALID (((uint64_t)1 << 56) - 1)

static struct fdk__dmabuf_egl_api g_api;
static int g_loaded = 0;

static void *db_egl_dlsym(void *lib, const char *name, int required,
                          int *ok) {
    void *sym = dlsym(lib, name);
    if (sym == NULL) {
        if (required) {
            *ok = 0;
        }
    }
    return sym;
}

int fdk__dmabuf_egl_load(struct fdk__dmabuf_egl_api **out) {
    if (out == NULL) {
        return 0;
    }
    *out = NULL;
    if (g_loaded) {
        return g_api.lib != NULL;
    }
    g_loaded = 1;
    memset(&g_api, 0, sizeof g_api);
    static const char *sonames[] = {"libEGL.so.1", "libEGL.so", NULL};
    void *lib = NULL;
    for (int i = 0; sonames[i] != NULL && lib == NULL; i++) {
        lib = dlopen(sonames[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (lib == NULL) {
        return 0;
    }
    int ok = 1;
    g_api.lib = lib;
    g_api.eglGetCurrentDisplay =
        db_egl_dlsym(lib, "eglGetCurrentDisplay", 1, &ok);
    g_api.eglGetCurrentContext =
        db_egl_dlsym(lib, "eglGetCurrentContext", 1, &ok);
    g_api.eglQueryString = db_egl_dlsym(lib, "eglQueryString", 1, &ok);
    g_api.eglGetError = db_egl_dlsym(lib, "eglGetError", 1, &ok);
    g_api.eglCreateImageKHR =
        db_egl_dlsym(lib, "eglCreateImageKHR", 1, &ok);
    g_api.eglDestroyImageKHR =
        db_egl_dlsym(lib, "eglDestroyImageKHR", 1, &ok);
    /* the modifiers extension is OPTIONAL (its absence just narrows
     * what the driver can import — never a load failure) */
    g_api.eglQueryDmaBufFormatsEXT =
        db_egl_dlsym(lib, "eglQueryDmaBufFormatsEXT", 0, &ok);
    g_api.eglQueryDmaBufModifiersEXT =
        db_egl_dlsym(lib, "eglQueryDmaBufModifiersEXT", 0, &ok);
    g_api.eglGetProcAddress =
        db_egl_dlsym(lib, "eglGetProcAddress", 1, &ok);
    if (!ok) {
        g_api.lib = NULL;
        return 0;
    }
    g_api.glEGLImageTargetTexture2DOES =
        (void (*)(unsigned, void *))g_api.eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    if (g_api.glEGLImageTargetTexture2DOES == NULL) {
        g_api.lib = NULL;
        return 0;
    }
    /* the plain-GL texture calls ride the SAME library the engine
     * resolved (libGLESv2/libGL); resolve them from that library
     * via eglGetProcAddress too — portable across both flavors */
    g_api.glGenTextures =
        (void (*)(int, unsigned *))g_api.eglGetProcAddress(
            "glGenTextures");
    g_api.glBindTexture =
        (void (*)(unsigned, unsigned))g_api.eglGetProcAddress(
            "glBindTexture");
    g_api.glTexParameteri =
        (void (*)(unsigned, unsigned, unsigned))g_api.eglGetProcAddress(
            "glTexParameteri");
    g_api.glDeleteTextures =
        (void (*)(int, const unsigned *))g_api.eglGetProcAddress(
            "glDeleteTextures");
    if (g_api.glGenTextures == NULL || g_api.glBindTexture == NULL ||
        g_api.glTexParameteri == NULL || g_api.glDeleteTextures == NULL) {
        g_api.lib = NULL;
        return 0;
    }
    *out = &g_api;
    return 1;
}

static int ext_has(const char *exts, const char *name) {
    if (exts == NULL) {
        return 0;
    }
    size_t n = strlen(name);
    const char *p = exts;
    while ((p = strstr(p, name)) != NULL) {
        char before = p == exts ? ' ' : p[-1];
        char after = p[n];
        if ((before == ' ' || before == '\0') &&
            (after == ' ' || after == '\0')) {
            return 1;
        }
        p += n;
    }
    return 0;
}

int fdk__dmabuf_egl_dpy_supports(const struct fdk__dmabuf_egl_api *api,
                                 void *dpy, int *out_modifiers_ext) {
    if (api == NULL || dpy == NULL || api->eglQueryString == NULL) {
        return 0;
    }
    const char *disp =
        api->eglQueryString(dpy, EGL_EXTENSIONS);
    int plain = ext_has(disp, "EGL_EXT_image_dma_buf_import");
    int mods = ext_has(disp, "EGL_EXT_image_dma_buf_import_modifiers");
    if (out_modifiers_ext != NULL) {
        *out_modifiers_ext = mods;
    }
    return plain;
}

int fdk__dmabuf_egl_query_modifiers(
    const struct fdk__dmabuf_egl_api *api, void *dpy, fdk_u32 fourcc,
    fdk_u64 *out_modifiers, fdk_i32 max_modifiers, fdk_i32 *out_n,
    fdk_i32 *out_implicit) {
    if (api == NULL || dpy == NULL || out_n == NULL) {
        return 0;
    }
    *out_n = 0;
    if (out_implicit != NULL) {
        *out_implicit = 0;
    }
    if (api->eglQueryDmaBufFormatsEXT == NULL ||
        api->eglQueryDmaBufModifiersEXT == NULL) {
        return 0; /* the extension absent: no query, no explicit mods */
    }
    int mods_ext = 0;
    if (!fdk__dmabuf_egl_dpy_supports(api, dpy, &mods_ext) || !mods_ext) {
        return 0;
    }
    unsigned fmts[64];
    int n_fmts = 0;
    if (!api->eglQueryDmaBufFormatsEXT(dpy, 64, fmts, &n_fmts) ||
        n_fmts <= 0) {
        return 0;
    }
    int found = 0;
    for (int i = 0; i < n_fmts && !found; i++) {
        if (fmts[i] == (unsigned)fourcc) {
            found = 1;
        }
    }
    if (!found) {
        *out_n = 0;
        return 1; /* the driver answers queries but not this format */
    }
    unsigned long long mods[128];
    int reserved[128];
    int n_mods = 0;
    if (!api->eglQueryDmaBufModifiersEXT(dpy, (unsigned)fourcc, 128,
                                         mods, reserved, &n_mods)) {
        return 0;
    }
    int implicit = 0;
    int kept = 0;
    for (int i = 0; i < n_mods; i++) {
        if ((uint64_t)mods[i] == DRM_FORMAT_MOD_INVALID) {
            implicit = 1;
            continue;
        }
        if (kept < max_modifiers) {
            out_modifiers[kept++] = (fdk_u64)mods[i];
        }
    }
    *out_n = kept;
    if (out_implicit != NULL) {
        *out_implicit = implicit;
    }
    return 1;
}

unsigned fdk__dmabuf_egl_realize(const struct fdk__dmabuf_egl_api *api,
                                 void *dpy, const fdk_dmabuf_params *p) {
    if (api == NULL || dpy == NULL || p == NULL) {
        return 0;
    }
    int mods_ext = 0;
    if (!fdk__dmabuf_egl_dpy_supports(api, dpy, &mods_ext)) {
        return 0;
    }
    int have_mods = mods_ext && p->planes[0].modifier != DRM_FORMAT_MOD_INVALID;
    /* a tiled modifier WITHOUT the modifiers extension cannot be
     * described to the driver: unsupported, honestly */
    if (p->planes[0].modifier != 0 &&
        p->planes[0].modifier != DRM_FORMAT_MOD_INVALID && !mods_ext) {
        return 0;
    }
    EGLint attrs[64];
    int n = 0;
    attrs[n++] = EGL_LINUX_DMA_BUF_EXT;
    attrs[n++] = 0;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = (EGLint)p->fourcc;
    attrs[n++] = (EGLint)p->width;
    attrs[n++] = (EGLint)p->height;
    static const int fd_keys[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const int off_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const int pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const int lo_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const int hi_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    for (int i = 0; i < p->n_planes && n + 6 < 64; i++) {
        attrs[n++] = fd_keys[i];
        attrs[n++] = p->planes[i].fd;
        attrs[n++] = off_keys[i];
        attrs[n++] = (EGLint)p->planes[i].offset;
        attrs[n++] = pitch_keys[i];
        attrs[n++] = (EGLint)p->planes[i].stride;
        /* INVALID: OMIT the modifier attrs entirely (implicit
         * semantics — the documented rule). Explicit values only
         * when the driver has the modifiers extension. */
        if (have_mods) {
            uint64_t m = p->planes[i].modifier;
            attrs[n++] = lo_keys[i];
            attrs[n++] = (EGLint)(m & 0xFFFFFFFFu);
            attrs[n++] = hi_keys[i];
            attrs[n++] = (EGLint)(m >> 32);
        }
    }
    attrs[n++] = 0; /* EGL_NONE */
    void *image = api->eglCreateImageKHR(
        dpy, api->eglGetCurrentContext(), EGL_LINUX_DMA_BUF_EXT, NULL,
        attrs);
    if (image == NULL) {
        return 0; /* the driver refused: unsupported, honestly */
    }
    unsigned tex = 0;
    api->glGenTextures(1, &tex);
    if (tex == 0) {
        api->eglDestroyImageKHR(dpy, image);
        return 0;
    }
    api->glBindTexture(GL_TEXTURE_2D, tex);
    api->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    api->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    api->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    api->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    api->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    /* the image can go now — the texture holds the reference */
    api->eglDestroyImageKHR(dpy, image);
    return tex;
}
