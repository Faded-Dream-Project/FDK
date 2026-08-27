/*
 * surface_internal.h — internal definition of struct fdk_surface
 *
 * The surface object is the public "renderable drawing target" handle
 * (see include/fdk/fdk_surface.h) owned by exactly one fdk_window and
 * created lazily by fdk_window_get_surface(). It holds no pixel
 * memory itself: the pixels always belong to the backend framebuffer
 * (X11 XImage data / Wayland wl_shm mapping) acquired through the
 * owning context's fdk_platform_ops.
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_SURFACE_INTERNAL_H
#define FDK_SURFACE_INTERNAL_H

#include "fdk/fdk_surface.h"

#include "platform/platform_internal.h"

struct fdk_surface {
    fdk_window *window; /* owning window, not owned by us */

    /* Last backend framebuffer handed out by
     * window->ops->window_get_framebuffer(). `has_fb` tracks whether
     * it is still meaningful — cleared by fdk_surface_present(),
     * because the Wayland backend hands the buffer to the compositor
     * at present time (writing it afterwards would race the
     * compositor's read; the next acquisition returns a fresh buffer).
     * The X11 backend's XImage data stays writable across presents,
     * but uniformly re-acquiring keeps the surface layer
     * backend-agnostic and costs X11 only a size check. */
    fdk_platform_framebuffer fb;
    int has_fb;
};

/* Implemented in surface.c, called by fdk_window_destroy() (see
 * src/window/window.c) to release the window's lazily-created surface,
 * if any. Safe to call on a window whose surface was never created. */
void fdk_surface_detach_from_window(fdk_window *window);

#endif /* FDK_SURFACE_INTERNAL_H */
