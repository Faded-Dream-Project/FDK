/*
 * surface_internal.h — internal definition of struct fdk_surface
 *
 * The surface object is the public "renderable drawing target" handle
 * (see include/fdk/fdk_surface.h) in one of two forms:
 *
 *  - a WINDOW surface, owned by exactly one fdk_window, created
 *    lazily by fdk_window_get_surface(), holding no pixel memory of
 *    its own (pixels belong to the backend framebuffer acquired
 *    through the owning context's fdk_platform_ops);
 *
 *  - an OFFSCREEN surface, created by the public fdk_surface_create()
 *    and owned by the application, holding its own malloc'd pixel
 *    buffer (stride padded to 16 px so stride != width paths are
 *    always exercised — see the header doc).
 *
 * Both forms share the drawing-primitive implementation in surface.c:
 * damage bookkeeping, the clip stack, and the blend helpers operate
 * on the `fb` fields identically. Only present() and the acquisition
 * path differ.
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_SURFACE_INTERNAL_H
#define FDK_SURFACE_INTERNAL_H

#include "fdk/fdk_surface.h"

#include "platform/platform_internal.h"

/* Clip stack and damage-region capacity are public (fdk_surface.h);
 * both live inline in the struct — no dynamic allocation, fixed
 * bounds, overflow degrades to full damage. */
struct fdk_surface {
    /* Owning window, NULL for offscreen surfaces. Not owned by us. */
    fdk_window *window;

    /* 1 = application-owned offscreen surface (own_pixels below). */
    int offscreen;

    /* Last framebuffer handed out (backend framebuffer for window
     * surfaces, own_pixels for offscreen). `has_fb` tracks whether it
     * is still meaningful — cleared by a real fdk_surface_present()
     * on a window surface, because the Wayland backend hands the
     * buffer to the compositor at present time (writing it afterwards
     * would race the compositor's read; the next acquisition returns
     * a fresh buffer pre-filled with the visible frame). Offscreen
     * surfaces keep has_fb permanently set — nothing ever takes the
     * pixels away. */
    fdk_platform_framebuffer fb;
    int has_fb;

    /* Offscreen pixel memory (offscreen surfaces only). Stride is
     * fb.stride * 4 bytes; allocated zeroed with fdk_alloc. */
    fdk_u32 *own_pixels;
    size_t own_length; /* bytes, for fdk_free symmetry */

    /* Effective clip as [x0, y0) x (x1, y1) half-open pixel bounds
     * intersected with the framebuffer bounds; recomputed on every
     * push/pop so the per-pixel hot path is a single compare per
     * axis. clip_stack[0 .. clip_depth) holds the pushed rects
     * (already intersected with their predecessors, in push order). */
    fdk_rect clip_stack[FDK_SURFACE_CLIP_DEPTH];
    int clip_depth;
    fdk_i32 clip_x0, clip_y0, clip_x1, clip_y1;

    /* Damage region (surface coordinates, half-open rects) since the
     * last successful present. `damage_full` short-circuits the rect
     * list ("everything changed"). Rects are stored UNCLAMPED to the
     * current fb (invalidate() drops fully-outside ones, keeps
     * straddling ones); backends clamp at present time. */
    fdk_rect damage[FDK_SURFACE_MAX_DAMAGE];
    int damage_count;
    int damage_full;

    /* Size the damage region was recorded against. A framebuffer
     * re-acquisition at a different size invalidates the damage
     * model (new buffer = all-new content) and resets to full. */
    fdk_i32 damage_w, damage_h;

    /* Set once any framebuffer has been acquired; distinguishes
     * "presented nothing ever" (present = documented no-op) from
     * "acquired but drew nothing" (present = cheap no-op skip). */
    int ever_acquired;
};

/* Implemented in surface.c, called by fdk_window_destroy() (see
 * src/window/window.c) to release the window's lazily-created surface,
 * if any. Safe to call on a window whose surface was never created. */
void fdk_surface_detach_from_window(fdk_window *window);

/* Alpha-mask blit (internal) — composites a 1-byte-per-pixel coverage
 * mask with a solid color, source-over, clip-stack-honoring, across
 * `rect` (also the mask's coordinate frame; `mask_stride` in bytes).
 * Does NOT record damage — callers own damage bookkeeping (the text
 * layer unions a whole run's glyph boxes into one damage rect).
 * Implemented in surface.c; used by src/text/text.c. */
void fdk_surface_blend_mask(fdk_surface *surface, fdk_rect rect,
                            const fdk_u8 *mask, fdk_i32 mask_stride,
                            fdk_color color);

#endif /* FDK_SURFACE_INTERNAL_H */
