/*
 * x11_gl.h — the GPU presentation lane (1.4.18), X11/GLX side
 *
 * The lane's contract, in one paragraph: FDK rasterizes every pixel
 * on the CPU exactly as before (src/render/ is untouched — the same
 * rasterizer, the same bytes); the LANE replaces only the "pixels ->
 * screen" half of present(), uploading the damaged rectangles of the
 * window's CPU framebuffer into a GL texture and compositing one
 * fullscreen quad through a GLX context, so the final blit to the
 * visible window happens on the GPU. Output is byte-identical to the
 * MIT-SHM path by construction (1:1 NEAREST sampling of a
 * BGRA/UNSIGNED_BYTE texture whose bytes ARE the framebuffer's
 * little-endian XRGB8888 layout) — pinned by tests, not assumed.
 *
 * Why GLX loaded at RUNTIME (dlopen) instead of link-time: FDK's
 * documented build baseline is libX11 + libXext, everywhere libX11
 * exists; libGL is NOT part of that baseline (plenty of servers and
 * containers have no GL at all), and requiring it would break the
 * toolkit's zero-new-dependencies promise. dlopen("libGL.so.1") plus
 * hand-declared entry points (no GL headers needed — the API subset
 * is tiny and stable since 2001) keeps every existing build working
 * and makes the lane a pure runtime upgrade.
 *
 * Policy (see x11_gl.c for the decision table): the lane engages for
 * HARDWARE renderers; Mesa's software rasterizers (llvmpipe,
 * softpipe, swrast) are refused — the CPU/SHM path is faster there
 * and has fewer moving parts — unless FDK_GL_FORCE=1 asks for it
 * anyway (that is what the test suite uses to exercise the lane under
 * Xvfb's llvmpipe). FDK_NO_GL=1 opts out entirely, the same shape as
 * the FDK_NO_MIT_SHM escape hatch.
 *
 * FAILURE = DOWNGRADE, never a broken frame: any error while
 * presenting (GL error, lost context, poisoned present — see
 * FDK_GL_FAIL_AT, the test seam) makes x11_surface.c copy the current
 * staging buffer into the classic double-buffered slots and re-present
 * through the SHM path with full damage, permanently, for that window.
 * The first present through the lane also SELF-VERIFIES (glReadPixels
 * spot-check against the staging buffer) so a driver that renders
 * garbage downgrades before the user ever sees it.
 *
 * All GL types stay inside x11_gl.c (the "no backend leakage" rule:
 * this header declares only FDK/opaque types).
 */

#ifndef FDK_X11_GL_H
#define FDK_X11_GL_H

#include "platform/platform_internal.h"

/* Per-connection lane state: the dlopen'd libGL handle, the resolved
 * entry points, and the once-per-connection renderer policy verdict.
 * Created lazily by fdk_x11_gl_try_load() at the FIRST framebuffer
 * acquisition of ANY window on the connection (never at connect —
 * headless contexts and apps that open no window never load GL). */
struct fdk_x11_gl;

/* Per-window lane state: the GLX context, the window-sized texture,
 * and the CPU staging buffer the application draws into. NULL on the
 * fdk_platform_window until the lane engages for it. */
struct fdk_x11_gl_window;

/* Attempts to bring the lane up for this connection (once). Returns
 * NULL when unavailable — no libGL, no GLX extension, unresolved
 * entry points, or FDK_NO_GL=1 — after logging which. NULL is
 * STICKY: the caller caches "tried" so windows never retry. */
struct fdk_x11_gl *fdk_x11_gl_try_load(fdk_platform_connection *conn);

/* Connection teardown. Destroys the state; deliberately does NOT
 * dlclose libGL — see the pinning note in x11_gl.c's header comment. */
void fdk_x11_gl_shutdown(struct fdk_x11_gl *gl);

/* The renderer string the lane saw (for diagnostics/tests), or NULL
 * when the lane never got far enough to query one. */
const char *fdk_x11_gl_renderer(const struct fdk_x11_gl *gl);

/* Tries to engage the lane for one window: finds an FBConfig matching
 * the window's visual, creates the context, applies the renderer
 * policy, and initializes GL state. Returns NULL (and logs why) when
 * the window must stay on the CPU path. NULL is cached by the caller
 * only for POLICY refusals and hard failures — see x11_surface.c. */
struct fdk_x11_gl_window *fdk_x11_gl_window_try_init(
    struct fdk_x11_gl *gl, fdk_platform_window *pwindow);

/* Window teardown: destroys the context and texture, frees the
 * staging buffer, releases the current binding. No-op on NULL. */
void fdk_x11_gl_window_destroy(struct fdk_x11_gl *gl,
                               struct fdk_x11_gl_window *gwin);

/* Returns the staging buffer for drawing, (re)allocating it whenever
 * the size changed (a resize means all-new content per the surface
 * contract, so nothing is copied). The buffer is w*h fdk_u32s with
 * stride == w. Returns NULL only on allocation failure — the caller
 * treats that as the classic path's slot_create failure would be. */
fdk_u32 *fdk_x11_gl_window_staging(struct fdk_x11_gl_window *gwin,
                                   int w, int h);

/* Presents the damaged rectangles: uploads them to the texture,
 * composites the fullscreen quad, swaps. Returns 0 on success and
 * NONZERO on any failure — which is the caller's signal to perform
 * the permanent downgrade (the staging buffer still holds the
 * current frame, so the caller can salvage it into the SHM slots). */
int fdk_x11_gl_window_present(struct fdk_x11_gl_window *gwin,
                              const fdk_platform_damage *damage,
                              int w, int h);

/* TEST SEAM (not exported by the library's version script): clears
 * the process-wide self-check failure stamp — the verdict a poisoned
 * run declared. The test suite calls this after exercising the
 * REAL-failure path so later test groups (and real-hardware runs of
 * the suite, where the poison was an artifact, not a driver truth)
 * start from an honest UNVERIFIED state. */
void fdk__x11_gl_test_reset(void);

#endif /* FDK_X11_GL_H */
