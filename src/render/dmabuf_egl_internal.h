/* dmabuf_egl_internal.h — the zero-copy dma-buf bridge's private
 * contract (dmabuf_egl.c ↔ the GPU engine + the lanes). Internal
 * header; nothing here is public API. */
#ifndef FDK_DMABUF_EGL_INTERNAL_H
#define FDK_DMABUF_EGL_INTERNAL_H

#include "fdk/fdk_surface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The dlopen'd libEGL bridge. Resolved once per process. */
struct fdk__dmabuf_egl_api {
    void *lib;
    void *(*eglGetCurrentDisplay)(void);
    void *(*eglGetCurrentContext)(void);
    const char *(*eglQueryString)(void *, int);
    unsigned (*eglGetError)(void);
    void *(*eglCreateImageKHR)(void *, void *, unsigned, void *,
                               const int *);
    unsigned char (*eglDestroyImageKHR)(void *, void *);
    unsigned char (*eglQueryDmaBufFormatsEXT)(void *, int, unsigned *,
                                              int *);
    unsigned char (*eglQueryDmaBufModifiersEXT)(void *, unsigned, int,
                                                unsigned long long *, int *,
                                                int *);
    void *(*eglGetProcAddress)(const char *);
    void (*glEGLImageTargetTexture2DOES)(unsigned, void *);
    void (*glGenTextures)(int, unsigned *);
    void (*glBindTexture)(unsigned, unsigned);
    void (*glTexParameteri)(unsigned, unsigned, unsigned);
    void (*glDeleteTextures)(int, const unsigned *);
};

/* Loads the bridge (dlopen libEGL + resolve). Returns 0 when no
 * libEGL or the required symbols are missing — the caller then
 * serves imports through the CPU-mode linear fallback only. */
int fdk__dmabuf_egl_load(struct fdk__dmabuf_egl_api **out);

/* The display's import truth: 1 when EGL_EXT_image_dma_buf_import is
 * in the display's extension string. *out_modifiers_ext (optional)
 * reports the _import_modifiers extension. */
int fdk__dmabuf_egl_dpy_supports(const struct fdk__dmabuf_egl_api *api,
                                 void *dpy, int *out_modifiers_ext);

/* The driver's modifier list for a fourcc. Returns 1 when the query
 * ran (the extension present); 0 when it cannot answer (no query
 * extension — the caller treats the modifier truth as unknown). */
int fdk__dmabuf_egl_query_modifiers(
    const struct fdk__dmabuf_egl_api *api, void *dpy, fdk_u32 fourcc,
    fdk_u64 *out_modifiers, fdk_i32 max_modifiers, fdk_i32 *out_n,
    fdk_i32 *out_implicit);

/* Realizes the GL texture for the params: eglCreateImageKHR
 * (EGL_LINUX_DMA_BUF_EXT) + glEGLImageTargetTexture2DOES. Requires
 * the lane's context CURRENT (the engine's contract at flush). The
 * EGLImage is destroyed before return (the texture holds the
 * reference). Returns the GL texture name, or 0 = the driver/lane
 * refused (the caller's fallback rules apply). */
/* The X11 side door: a cached platform-x11 EGL display for the GLX
 * lane's Display* (mesa shares the driver across the two APIs; see
 * the comment in dmabuf_egl.c). NULL when libEGL or the display
 * cannot serve it. */
void *fdk__dmabuf_egl_x11_display(void *x11_display);

/* The X11 cap half: the platform-x11 display has
 * EGL_EXT_image_dma_buf_import. The SAMPLING half (GL_OES_EGL_image
 * in the GLX context) is a draw-time truth and degrades to per-draw
 * UNSUPPORTED when absent. */
int fdk__dmabuf_egl_x11_available(void *x11_display);

/* A bridge view whose GL entry points resolve from `lib` (libGL for
 * the GLX lane — the desktop GL_OES_EGL_image entry). Borrows lib. */
struct fdk__dmabuf_egl_api *fdk__dmabuf_egl_api_with_gl_lib(void *lib);

/* The process's own X display (XOpenDisplay(NULL), cached) for the
 * GLX lane's side door; NULL without X11. */
void *fdk__dmabuf_egl_x11_process_display(void);

unsigned fdk__dmabuf_egl_realize(const struct fdk__dmabuf_egl_api *api,
                                 void *dpy, const fdk_dmabuf_params *p);

#ifdef __cplusplus
}
#endif

#endif /* FDK_DMABUF_EGL_INTERNAL_H */
