/*
 * wayland_egl.h — the GPU presentation lane (1.2.0 / D-1), Wayland/EGL side
 *
 * The exact contract of the X11/GLX lane (x11_gl.h), carried over to
 * Wayland with the platform's own mechanism: FDK rasterizes every
 * pixel on the CPU exactly as before (src/render/ untouched — the
 * same rasterizer, the same bytes); the LANE replaces only the
 * "pixels -> compositor" half of present(), uploading the damaged
 * rectangles of the window's CPU framebuffer into a GL texture and
 * compositing one fullscreen quad through an EGL window surface
 * backed by wl_egl_window (libwayland-egl), so the frame the
 * compositor latches is produced by the GPU. Output is
 * byte-identical to the wl_shm path by construction (1:1 NEAREST
 * sampling of a BGRA/UNSIGNED_BYTE texture whose bytes ARE the
 * framebuffer's little-endian XRGB8888 layout, the X byte's alpha
 * role pinned to opaque: the quad shader forces alpha 1.0 and the
 * config prefers an alpha-less swap buffer) — pinned by tests,
 * not assumed.
 *
 * Why EGL loaded at RUNTIME (dlopen) instead of link-time: FDK's
 * documented build baseline is libwayland-client + libxkbcommon;
 * libEGL and libwayland-egl are NOT part of that baseline (plenty
 * of servers and containers have no GL at all), and requiring them
 * would break the toolkit's zero-new-dependencies promise.
 * dlopen("libEGL.so.1") + dlopen("libwayland-egl.so.1") plus
 * hand-declared entry points (no EGL headers needed — the API
 * subset is tiny and stable since 2009) keeps every existing build
 * working and makes the lane a pure runtime upgrade. The same
 * posture as the GLX lane and the HarfBuzz/FriBidi/RENDER lanes.
 *
 * Policy (mirrors x11_gl.c's table): the lane engages for HARDWARE
 * renderers; Mesa's software rasterizers (llvmpipe, softpipe,
 * swrast) are refused — the CPU/wl_shm path is faster there and
 * has fewer moving parts — unless FDK_GL_FORCE=1 asks for it anyway
 * (that is what the test suite uses to exercise the lane under
 * sway headless's llvmpipe). FDK_NO_GL=1 opts out entirely, the
 * same shape as the X11 side and FDK_NO_MIT_SHM.
 *
 * FAILURE = DOWNGRADE, never a broken frame: any error while
 * presenting (GL error, lost context, poisoned present) makes
 * wayland_window.c copy the current staging buffer into the classic
 * wl_shm slots and re-present through the SHM path with full
 * damage, permanently, for that window. The first present through
 * the lane also SELF-VERIFIES (glReadPixels spot-check against the
 * staging buffer) so a driver that renders garbage downgrades
 * before the user ever sees it.
 *
 * EGL surface sizing: wl_egl_window is resized (wl_egl_window_resize)
 * whenever the window's PHYSICAL buffer size changes — the EGL
 * surface then reallocates its back buffer on the next
 * eglSwapBuffers, exactly the GLX lane's resize model. The lane's
 * staging buffer is allocated at the PHYSICAL size (the same size
 * wl_shm buffers take), so HiDPI scale factors ride the same path.
 *
 * All EGL types stay inside wayland_egl.c (the "no backend leakage"
 * rule: this header declares only FDK/opaque types).
 */

#ifndef FDK_WAYLAND_EGL_H
#define FDK_WAYLAND_EGL_H

#include "platform/platform_internal.h"

/* Per-connection lane state: the dlopen'd libEGL + libwayland-egl
 * handles, the resolved entry points, the EGLDisplay for the
 * connection's wl_display, and the once-per-connection renderer
 * policy verdict. Created lazily by fdk_wayland_egl_try_load() at
 * the FIRST framebuffer acquisition of ANY window on the connection
 * (never at connect — headless contexts and apps that open no
 * window never load EGL). */
struct fdk_wayland_egl;

/* Per-window lane state: the EGL context + surface pair, the
 * wl_egl_window wrapper, the window-sized texture, and the CPU
 * staging buffer the application draws into. NULL on the
 * fdk_platform_window until the lane engages for it. */
struct fdk_wayland_egl_window;

/* Attempts to bring the lane up for this connection (once).
 * Returns NULL when unavailable — no libEGL, no libwayland-egl,
 * eglGetPlatformDisplay/eglInitialize failure, unresolved entry
 * points, or FDK_NO_GL=1 — after logging which. NULL is STICKY:
 * the caller caches "tried" so windows never retry. */
struct fdk_wayland_egl *fdk_wayland_egl_try_load(
    fdk_platform_connection *conn);

/* Connection teardown. Destroys the state; deliberately does NOT
 * dlclose the libraries — the GLX lane's pinning note applies
 * verbatim (Mesa registers process-global state that does not
 * survive unload cleanly). */
void fdk_wayland_egl_shutdown(struct fdk_wayland_egl *egl);

/* Tries to engage the lane for one window: creates the
 * wl_egl_window + EGL surface + context, applies the renderer
 * policy, initializes GL state. Returns NULL (and logs why) when
 * the window must stay on the CPU path. The caller caches NULL for
 * POLICY refusals and hard failures — see wayland_window.c. */
struct fdk_wayland_egl_window *fdk_wayland_egl_window_try_init(
    struct fdk_wayland_egl *egl, fdk_platform_window *pwindow);

/* Window teardown: destroys the surface/context/wl_egl_window and
 * texture, frees the staging buffer, releases the current binding.
 * No-op on NULL. */
void fdk_wayland_egl_window_destroy(struct fdk_wayland_egl *egl,
                                    struct fdk_wayland_egl_window *ewin);

/* Returns the staging buffer for drawing, (re)allocating it
 * whenever the size changed (and resizing the wl_egl_window). The
 * buffer is w*h fdk_u32s with stride == w. Returns NULL only on
 * allocation failure — the caller treats that the way the classic
 * path treats a slot_create failure. */
fdk_u32 *fdk_wayland_egl_window_staging(struct fdk_wayland_egl_window *ewin,
                                        int w, int h);

/* Presents the damaged rectangles: uploads them to the texture,
 * composites the fullscreen quad, eglSwapBuffers. Returns 0 on
 * success and NONZERO on any failure — which is the caller's
 * signal to perform the permanent downgrade (the staging buffer
 * still holds the current frame, so the caller can salvage it into
 * the wl_shm slots). */
int fdk_wayland_egl_window_present(struct fdk_wayland_egl_window *ewin,
                                   const fdk_platform_damage *damage,
                                   int w, int h);

/* TEST SEAM (the fdk__x11_gl_test_reset mirror): clears the
 * process-wide lane verdicts (policy cache + self-check failure
 * stamp). Test binaries call it between poisoned legs so a later
 * group starts from an honest UNVERIFIED state; the library itself
 * never calls it. */
void fdk__wayland_egl_test_reset(void);

#endif /* FDK_WAYLAND_EGL_H */
