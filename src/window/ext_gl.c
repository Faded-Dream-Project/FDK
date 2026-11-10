/* ext_gl.c — GL for add-ons as a real API (fdk_extend.h's fdk_ext_gl_*).
 * Routing only: the per-backend work lives in the lanes (wayland_egl.c /
 * x11_gl.c) which own their context machinery. The frame callback rides
 * the platform window (per-backend fields) and is invoked by each
 * backend's present path right before the swap — present arbitration:
 * a window with a registered callback presents ITS GL frame there while
 * every other window keeps its own render mode. Never installed. */

#include "window_internal.h"
#include "fdk/fdk_extend.h"

fdk_window *fdk__ext_surface_window(fdk_ext_surface *surface);

/* Backend routing (implemented per backend; declared here so this file
 * needs neither backend's platform header — the two headers both
 * complete struct fdk_platform_window and cannot coexist in one TU). */
static fdk_platform_window *ext_pw(fdk_ext_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    fdk_window *own = fdk__ext_surface_window(surface);
    return own != NULL ? own->pwindow : NULL;
}

fdk_result fdk_ext_gl_make_current(fdk_ext_surface *surface) {
    fdk_platform_window *pw = ext_pw(surface);
    if (pw == NULL || fdk__ext_surface_window(surface)->ops == NULL ||
        fdk__ext_surface_window(surface)->ops->window_gl_make_current ==
            NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return fdk__ext_surface_window(surface)->ops->window_gl_make_current(pw);
}

fdk_result fdk_ext_gl_set_frame_callback(fdk_ext_surface *surface,
                                         fdk_ext_gl_frame_fn fn,
                                         void *user) {
    fdk_platform_window *pw = ext_pw(surface);
    if (pw == NULL || fdk__ext_surface_window(surface)->ops == NULL ||
        fdk__ext_surface_window(surface)->ops->window_gl_set_frame_cb ==
            NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return fdk__ext_surface_window(surface)->ops->window_gl_set_frame_cb(
        pw, (void (*)(void *, void *))fn, user, surface);
}

fdk_result fdk_ext_gl_swap(fdk_ext_surface *surface) {
    fdk_platform_window *pw = ext_pw(surface);
    if (pw == NULL || fdk__ext_surface_window(surface)->ops == NULL ||
        fdk__ext_surface_window(surface)->ops->window_gl_swap == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return fdk__ext_surface_window(surface)->ops->window_gl_swap(pw);
}

fdk_result fdk_ext_gl_fence_insert(fdk_ext_surface *surface,
                                   fdk_u64 *out_fence) {
    (void)surface;
    (void)out_fence;
    /* The lane proc tables do not resolve EGLSync/GLFence yet; the
     * honest answer until they do is UNSUPPORTED, never a fake fence. */
    return FDK_ERR_UNSUPPORTED;
}

fdk_result fdk_ext_gl_fence_wait(fdk_ext_surface *surface, fdk_u64 fence) {
    (void)surface;
    (void)fence;
    return FDK_ERR_UNSUPPORTED;
}
