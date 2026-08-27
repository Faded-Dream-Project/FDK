/*
 * fdk_surface.h — Faded Dream ToolKit software rendering surfaces
 *
 * First slice of Phase 3 (Rendering). A surface is a window's
 * CPU-drawing target: you ask the window for its surface, obtain the
 * framebuffer's pixel pointer, draw (either with the fdk_surface_* fill
 * helpers below, or by writing pixels directly), then present the
 * surface to make the content visible on screen.
 *
 * Typical frame, in an application-driven loop built on
 * fdk_pump_events() (see fdk_core.h):
 *
 *     fdk_pump_events(ctx, 15);              // process input, etc.
 *     fdk_surface_get_info(surface, &info);  // (re)acquire framebuffer
 *     // ... draw using info.pixels / helpers ...
 *     fdk_surface_present(surface);
 *
 * The framebuffer is 32-bit XRGB8888: one fdk_u32 per pixel, red in
 * bits 23..16, green in 15..8, blue in 7..0, the top byte ignored
 * (treated as opaque). This matches what both current backends want
 * natively (X11 24-bit TrueColor with 0xFF0000/0xFF00/0xFF masks, and
 * Wayland's WL_SHM_FORMAT_XRGB8888, which the wl_shm spec guarantees
 * every compositor supports), so no per-pixel conversion happens
 * anywhere in FDK.
 *
 * No backend-specific (X11/Wayland) type ever appears here.
 *
 * Threading: like the rest of FDK's window API, surfaces are
 * UI-thread-only — see docs/threading.md.
 *
 * Phase 3 scope of this header: software framebuffer access, whole-
 * window presentation, and a small set of blending fill primitives
 * (solid fill, filled rect, 1px rect border, vertical gradient). The
 * full Phase 3 primitive set (lines, rounded rects, transforms,
 * clipping stacks, images, text — see docs/roadmap.md) builds on this
 * foundation in later slices.
 */

#ifndef FDK_SURFACE_H
#define FDK_SURFACE_H

#include "fdk_error.h"
#include "fdk_types.h"
#include "fdk_window.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel layout of the framebuffer. Kept as an enum (not a bare
 * #define) so future formats can be added without breaking ABI —
 * fdk_surface_info carries the value, and applications that only
 * accept a specific layout can check it. */
typedef enum fdk_surface_format {
    FDK_SURFACE_FORMAT_XRGB8888 = 1, /* fdk_u32, R<<16 | G<<8 | B */
} fdk_surface_format;

/* A snapshot of the surface's framebuffer. `pixels` points at the
 * top-left corner of the window's framebuffer; row y starts at
 * pixels + (size_t)y * stride. `stride` is in PIXELS (fdk_u32 units),
 * not bytes. The pointer and dimensions are valid only until the next
 * fdk_surface_get_info() or fdk_surface_present() call on this
 * surface (a present hands the buffer to the platform and the next
 * acquisition may return a different one), so reacquire every frame —
 * the typical loop above does exactly that. */
typedef struct fdk_surface_info {
    fdk_u32 *pixels;
    fdk_i32 width;
    fdk_i32 height;
    fdk_i32 stride; /* in fdk_u32 units, >= width */
    fdk_surface_format format;
} fdk_surface_info;

/* Returns (lazily creating on first call) the drawing surface of
 * `window`. One surface exists per window; repeated calls return the
 * same object. The surface is owned by the window and destroyed with
 * it — applications never free a surface themselves.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - window or out_surface is NULL
 *   FDK_ERR_NOT_INITIALIZED  - window's context has no connection
 *   FDK_ERR_OUT_OF_MEMORY    - allocation failure
 */
fdk_result fdk_window_get_surface(fdk_window *window,
                                  fdk_surface **out_surface);

/* Acquires (or re-acquires, handling resizes) the window's current
 * framebuffer and writes its description to *out_info. Call this once
 * per frame before drawing; see fdk_surface_info for the pointer's
 * validity rules.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or out_info is NULL
 *   FDK_ERR_UNSUPPORTED      - the active backend provides no software
 *                              framebuffer (none of the current
 *                              backends hit this, it exists for future
 *                              GPU-only backends)
 *   FDK_ERR_OUT_OF_MEMORY    - framebuffer allocation failed
 *   FDK_ERR_SURFACE_CREATE   - the platform rejected buffer creation
 *                              (logged at WARN level by the backend)
 */
fdk_result fdk_surface_get_info(fdk_surface *surface,
                                fdk_surface_info *out_info);

/* Makes everything drawn since the last acquisition visible on
 * screen. On X11 this blits the framebuffer to the window
 * (XPutImage); on Wayland the buffer is attached, damaged, and
 * committed to the compositor. With nothing acquired/drawn yet, this
 * is a documented no-op returning FDK_OK.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface is NULL
 *   FDK_ERR_UNSUPPORTED      - backend has no presentation path
 *   FDK_ERR_SURFACE_CREATE   - platform presentation failed (logged)
 */
fdk_result fdk_surface_present(fdk_surface *surface);

/* ---- Drawing primitives (first Phase 3 slice) ----
 *
 * All of these blend source-over into the framebuffer using the
 * straight (non-premultiplied) alpha of `color` — pass a = 1.0 for
 * opaque replacement, a < 1.0 to tint what is already there. All
 * coordinates are clipped to the framebuffer automatically; rects
 * may be partially or fully outside. Zero-size rects draw nothing.
 * If the surface has no acquired framebuffer yet, each helper
 * acquires one first (they never crash on an unpresented surface).
 *
 * These helpers are deliberately small building blocks, not a full
 * 2D toolkit — lines, transforms, and text are later Phase 3 work.
 */

/* Fills the entire framebuffer with `color`. */
void fdk_surface_fill(fdk_surface *surface, fdk_color color);

/* Fills `rect` with `color`. */
void fdk_surface_fill_rect(fdk_surface *surface, fdk_rect rect,
                           fdk_color color);

/* Draws a 1-pixel border around `rect` with `color`. */
void fdk_surface_draw_rect(fdk_surface *surface, fdk_rect rect,
                           fdk_color color);

/* Fills `rect` with a vertical linear gradient from `top` (at
 * rect.y) to `bottom` (at rect.y + rect.height). Both colors' alpha
 * channels participate in the per-row blend. */
void fdk_surface_fill_gradient_vertical(fdk_surface *surface,
                                        fdk_rect rect,
                                        fdk_color top, fdk_color bottom);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SURFACE_H */
