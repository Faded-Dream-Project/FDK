/*
 * gpaint.h — the GPU paint engine (internal; see the .c header comment
 * for the design contract). Everything here is src/render-internal: the
 * surface primitives (surface.c) call these instead of the rasterizer
 * when the target surface carries a gpu batch, and the platform lanes
 * (wayland_egl.c / x11_gl.c) drive flush+swap from their present path.
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_GPAINT_H
#define FDK_GPAINT_H

#include "surface_internal.h"

/* One-time GL proc resolution (dlopen libGLESv2.so.2 / libGL.so.1,
 * the lanes' zero-new-dependency posture). Returns 1 when the engine
 * can run in this process. */
int fdk__gpu_procs_init(void);

/* Batch state lifecycle — one per GPU-mode WINDOW surface. */
struct fdk_gpu_state;
int fdk__gpu_surface_init(struct fdk_surface *s, int width, int height);
void fdk__gpu_surface_destroy(struct fdk_surface *s);
void fdk__gpu_surface_resize(struct fdk_surface *s, int width, int height);

/* Paint primitives — exact semantic twins of the fdk_surface methods
 * of the same names (straight-alpha blending, integer pixel rects,
 * clip stack applied). Coordinates are top-left-origin framebuffer
 * pixels; the engine flips to GL coordinates itself. */
void fdk__gpu_fill(struct fdk_surface *s, fdk_color c);
void fdk__gpu_fill_rect(struct fdk_surface *s, fdk_rect r, fdk_color c);
void fdk__gpu_draw_rect(struct fdk_surface *s, fdk_rect r, fdk_color c,
                        fdk_i32 thickness);
void fdk__gpu_fill_rounded_rect(struct fdk_surface *s, fdk_rect r,
                                fdk_i32 radius, fdk_color c, int aa);
void fdk__gpu_draw_rounded_rect(struct fdk_surface *s, fdk_rect r,
                                fdk_i32 radius, fdk_color c,
                                fdk_i32 thickness, int aa);
void fdk__gpu_fill_circle(struct fdk_surface *s, fdk_f32 cx, fdk_f32 cy,
                          fdk_f32 radius, fdk_color c, int aa);
void fdk__gpu_draw_circle(struct fdk_surface *s, fdk_f32 cx, fdk_f32 cy,
                          fdk_f32 radius, fdk_color c, fdk_f32 thickness,
                          int aa);
void fdk__gpu_draw_line(struct fdk_surface *s, fdk_f32 x0, fdk_f32 y0,
                        fdk_f32 x1, fdk_f32 y1, fdk_color c, fdk_f32 width,
                        int aa);
void fdk__gpu_fill_gradient_vertical(struct fdk_surface *s, fdk_rect r,
                                     fdk_color c0, fdk_color c1);
void fdk__gpu_blend_mask(struct fdk_surface *s, fdk_rect dst,
                         const unsigned char *coverage, fdk_i32 stride,
                         fdk_color color, fdk_f32 global_alpha);
void fdk__gpu_blend_rgba(struct fdk_surface *s, fdk_rect dst,
                         const unsigned char *rgba, fdk_i32 stride,
                         fdk_f32 global_alpha);
void fdk__gpu_blit(struct fdk_surface *s, fdk_rect dst,
                   const struct fdk_surface *src, fdk_rect src_rect);
void fdk__gpu_sync_clip(struct fdk_surface *s);
void fdk__gpu_texture_drop(struct fdk_surface *s, void *key);

/* Draw one texture quad immediately (texture quads bypass the batch:
 * the batch flush binds the glyph atlas). uvs are 0..1. */
void fdk__gpu_draw_texture(struct fdk_surface *s, void *key,
                           fdk_rect dst, float u0, float v0, float u1,
                           float v1, float global_alpha);

/* pwindow -> surface registry: platform-internal re-present paths
 * (the frame pump's fallbacks) know only the platform window; the
 * engine maps it back to the GPU-mode surface. Bounded table; entries
 * die with the surface (unregister in detach). */
void fdk__gpu_register_window(void *pwindow, struct fdk_surface *s);
void fdk__gpu_unregister_window(void *pwindow);
struct fdk_surface *fdk__gpu_surface_for_window(void *pwindow);

/* The affine twin of blit_transformed (any invertible affine —
 * rotation/shear included; corners baked through fdk_matrix m). */
void fdk__gpu_blit_transformed(struct fdk_surface *s, fdk_matrix m,
                               const struct fdk_surface *src);

/* Test seam: readback of the current framebuffer as top-down
 * RGBA8888. The caller makes the lane context current. */
int fdk__gpu_engine_readback(struct fdk_surface *s, fdk_u32 *out,
                             int width, int height);

/* Issue the pending batch. Runs with the window's lane context CURRENT
 * (the caller owns makeCurrent); the caller swaps/presents after. */
void fdk__gpu_flush(struct fdk_surface *s);

/* Texture registry shared with the public fdk_texture API: creates or
 * returns the GL texture name for `t` in THIS engine (per-context
 * namespace; the lane's shared-context chain makes names cross). */
unsigned fdk__gpu_texture_name(struct fdk_surface *s, void *key,
                               int width, int height, int format_rgba,
                               const unsigned char *pixels, fdk_i32 stride,
                               int *created);

#endif /* FDK_GPAINT_H */
