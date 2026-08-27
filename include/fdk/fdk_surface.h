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
 * Phase 3 scope of this header: software framebuffer access,
 * presentation with damage tracking (only what changed is sent to
 * the display), a clip stack, offscreen surfaces, a small blending
 * primitive set (fills, rects, gradients, lines, circles, rounded
 * rects, surface-to-surface blit), and a frame-pacing query. The
 * remaining Phase 3 work (images, transforms, text — see
 * docs/roadmap.md and docs/rendering.md) builds on this foundation.
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

/* Makes everything drawn since the last present visible on screen,
 * sending only the damaged region (see "Damage tracking" below): on
 * X11 that is one sub-image blit per damaged rect; on Wayland the
 * buffer is attached, the damaged rects are sent as damage hints,
 * and the commit is made. With nothing drawn yet, or with an empty
 * damage region, this is a documented no-op returning FDK_OK — an
 * unchanged frame costs nothing (on Wayland not even a commit).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface is NULL
 *   FDK_ERR_UNSUPPORTED      - backend has no presentation path
 *   FDK_ERR_SURFACE_CREATE   - platform presentation failed (logged)
 */
fdk_result fdk_surface_present(fdk_surface *surface);

/* ---- Offscreen surfaces ----
 *
 * An offscreen surface is a windowless drawing target owned by the
 * application: create it, draw with the same primitives, read the
 * pixels via fdk_surface_get_info, and blit (fdk_surface_blit) it
 * onto window surfaces. Uses: caching expensive compositions,
 * sprites, and — for FDK itself — the widget layer's rendering.
 *
 * Offscreen surfaces work with NO display connection at all, which
 * also makes them the renderer's headless test surface. Their pixel
 * stride is deliberately padded (to a multiple of 16 pixels) so the
 * stride != width code paths that real backends produce are always
 * exercised.
 *
 * fdk_surface_present() on an offscreen surface is a no-op that
 * resets the damage region (see below) — there is no screen to
 * present to; fdk_surface_blit is how its content travels.
 *
 * Ownership: the caller owns the surface and releases it with
 * fdk_surface_destroy. Window surfaces (fdk_window_get_surface) are
 * the exception — those belong to their window and must NOT be
 * passed to fdk_surface_destroy (documented there).
 */

/* Creates a standalone offscreen surface of the given size.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - out_surface is NULL, or width/height
 *                              <= 0 or larger than 16384
 *   FDK_ERR_OUT_OF_MEMORY    - allocation failure
 */
fdk_result fdk_surface_create(fdk_i32 width, fdk_i32 height,
                             fdk_surface **out_surface);

/* Destroys an offscreen surface created by fdk_surface_create,
 * releasing its pixel memory. Passing a WINDOW surface here is an
 * application bug — window surfaces belong to their window; this
 * call detects and refuses it (FDK-side check, logged) rather than
 * corrupting the window. NULL is a safe no-op. */
void fdk_surface_destroy(fdk_surface *surface);

/* ---- Damage tracking ----
 *
 * "Damage" is the record of what changed on a surface since its
 * last fdk_surface_present(). Present sends only the damaged region
 * to the display (on X11: sub-image blits; on Wayland: per-rect
 * damage hints on the commit — and no commit at all when nothing
 * changed), which is what makes partial redraws cheap. The damage
 * region is cleared by a successful present.
 *
 * Every drawing helper below records the damage for what it drew
 * automatically. Applications writing pixels directly through
 * fdk_surface_get_info().pixels must call fdk_surface_invalidate()
 * for the region they touched — otherwise present() has no way to
 * know the content changed, and (documented behavior) will not send
 * it to the display.
 *
 * Damage accumulation is bounded: at most FDK_SURFACE_MAX_DAMAGE
 * rects (64) are tracked; further damage degrades to "everything
 * changed", preserving correctness while capping bookkeeping cost.
 * A resize (framebuffer re-acquisition at a new size) also resets
 * damage to "everything" — the new buffer's content is undefined
 * until redrawn. On Wayland every acquired buffer is pre-filled
 * with the currently visible frame, so partial redraw is correct
 * there too.
 */

#define FDK_SURFACE_MAX_DAMAGE 64

/* Adds `rect` to the surface's damage region. Clip-safe: parts
 * outside the surface are ignored; empty/garbage rects are no-ops.
 * Call this after writing pixels directly through the raw pointer. */
void fdk_surface_invalidate(fdk_surface *surface, fdk_rect rect);

/* Marks the entire surface as damaged ("everything changed"). */
void fdk_surface_invalidate_all(fdk_surface *surface);

/* Writes the bounding box of the current damage region to
 * *out_bounds and returns true; returns false (leaving *out_bounds
 * untouched) if nothing is damaged. This is what a partial-redraw
 * application checks to decide how much of a cached scene to
 * re-render. */
bool fdk_surface_get_damage_bounds(fdk_surface *surface,
                                   fdk_rect *out_bounds);

/* ---- Clip stack ----
 *
 * Drawing helpers blend only inside the intersection of all pushed
 * clips (and the surface bounds). Pushing never expands: a pushed
 * rect is intersected with the clip below it. Used by the widget
 * layer to constrain children to parent bounds, and by
 * ScrollView-style widgets later; nothing backend-specific here.
 *
 * The stack is bounded (FDK_SURFACE_CLIP_DEPTH, 32); pushing deeper
 * returns FDK_ERR_INVALID_ARGUMENT without touching the stack.
 * Popping an empty stack is a documented no-op (defensive — a
 * pop/pop imbalance should not crash a rendering loop). The
 * effective clip (intersected, unclamped to the surface) is
 * queryable for tests and widget geometry math.
 */

#define FDK_SURFACE_CLIP_DEPTH 32

/* Pushes `clip` onto the clip stack, intersected with the current
 * effective clip.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or clip is empty/degenerate,
 *                              or the stack is already full
 */
fdk_result fdk_surface_push_clip(fdk_surface *surface, fdk_rect clip);

/* Pops the most recently pushed clip. No-op on an empty stack. */
void fdk_surface_pop_clip(fdk_surface *surface);

/* Returns the current effective clip (the intersection of all
 * pushed clips, NOT intersected with the surface bounds — callers
 * composing geometry usually want their own rects back). Returns
 * the full infinite plane rect (INT32_MIN..INT32_MAX) when no clip
 * is pushed. */
fdk_rect fdk_surface_get_clip(fdk_surface *surface);

/* ---- Drawing primitives ----
 *
 * All of these blend source-over into the framebuffer using the
 * straight (non-premultiplied) alpha of `color` — pass a = 1.0 for
 * opaque replacement, a < 1.0 to tint what is already there. All
 * coordinates are clipped to the framebuffer AND the current clip
 * stack automatically; shapes may be partially or fully outside.
 * Zero-size shapes draw nothing. If the surface has no acquired
 * framebuffer yet, each helper acquires one first (they never crash
 * on an unpresented surface). Every helper records damage for the
 * region it actually touched (the clipped bounding box).
 *
 * These helpers are deliberately small building blocks, not a full
 * 2D toolkit — images, transforms, and text are later Phase 3 work.
 */

/* Fills the entire (clip-visible) framebuffer with `color`. */
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

/* Draws a 1-pixel line from (x0, y0) to (x1, y1) inclusive, using
 * Bresenham's algorithm (crisp integer pixels, no smoothing —
 * antialiased lines are later rendering work). */
void fdk_surface_draw_line(fdk_surface *surface,
                           fdk_i32 x0, fdk_i32 y0,
                           fdk_i32 x1, fdk_i32 y1,
                           fdk_color color);

/* Draws the 1-pixel outline of the circle with center (cx, cy) and
 * radius `radius` (midpoint algorithm, 8-way symmetric). radius <= 0
 * draws nothing. */
void fdk_surface_draw_circle(fdk_surface *surface,
                             fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                             fdk_color color);

/* Fills the circle with center (cx, cy) and radius `radius` — every
 * pixel whose center lies within the radius. radius <= 0 draws
 * nothing. */
void fdk_surface_fill_circle(fdk_surface *surface,
                             fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                             fdk_color color);

/* Fills `rect` with `color`, rounding the four corners with the
 * given `corner_radius`. The radius is clamped to half the shorter
 * side, so any rect degrades gracefully to a pill/circle. */
void fdk_surface_fill_rounded_rect(fdk_surface *surface, fdk_rect rect,
                                   fdk_i32 corner_radius, fdk_color color);

/* Draws the 1-pixel outline of a rounded rectangle (straight edges
 * plus quarter-circle corners), with the same radius clamping as
 * fill_rounded_rect. */
void fdk_surface_draw_rounded_rect(fdk_surface *surface, fdk_rect rect,
                                   fdk_i32 corner_radius, fdk_color color);

/* Copies `src_rect` (in SOURCE surface coordinates) of `src` to
 * (dst_x, dst_y) on `dst`, opaque pixel copy, clipped against the
 * source bounds, the destination bounds, and the destination's clip
 * stack. Records damage on `dst` for the copied region. `src` may
 * be an offscreen surface (the typical sprite-cache pattern) or a
 * window surface. Blitting a surface onto ITSELF with overlapping
 * source and destination regions is undefined (no aliasing
 * protection); copy through an offscreen surface instead.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - dst/src NULL, or src_rect empty
 */
fdk_result fdk_surface_blit(fdk_surface *dst, fdk_i32 dst_x, fdk_i32 dst_y,
                            fdk_surface *src, fdk_rect src_rect);

/* ---- Frame pacing ----
 *
 * bool fdk_surface_frame_ready() answers "may I draw the next frame
 * now?". On Wayland it becomes false right after a present and true
 * again once the compositor has acknowledged that frame (its frame
 * callback arrives while the application pumps events), or after a
 * bounded guard interval if the compositor stays silent — a hidden
 * window never receives frame callbacks, and FDK's contract is to
 * pace, never to starve. On X11 (no compositor feedback in the
 * core protocol) and on offscreen surfaces it is always true.
 *
 * Typical paced loop:
 *
 *     for (;;) {
 *         fdk_pump_events(ctx, 15);
 *         if (!fdk_surface_frame_ready(surface)) continue;
 *         draw();
 *         fdk_surface_present(surface);
 *     }
 */
bool fdk_surface_frame_ready(fdk_surface *surface);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SURFACE_H */
