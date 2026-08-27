#define FDK_LOG_TAG "render"

#include "fdk/fdk_surface.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "window/window_internal.h"

#include <math.h>
#include <string.h>

/* ---- clip-stack maintenance ---- */

/* Recomputes the effective pixel clip (clip_x0/y0/x1/y1, half-open)
 * from the top of the clip stack intersected with the framebuffer
 * bounds. Called after every push/pop AND after every framebuffer
 * (re)acquisition — the fb bounds are part of the intersection. An
 * empty result (x0 >= x1) simply makes every drawing helper a
 * no-op. */
static void recompute_clip(fdk_surface *surface) {
    if (surface->clip_depth == 0) {
        surface->clip_x0 = 0;
        surface->clip_y0 = 0;
        surface->clip_x1 = surface->fb.width;
        surface->clip_y1 = surface->fb.height;
        return;
    }

    fdk_rect c = surface->clip_stack[surface->clip_depth - 1];
    long long x0 = (long long)c.x;
    long long y0 = (long long)c.y;
    long long x1 = (long long)c.x + (long long)c.width;
    long long y1 = (long long)c.y + (long long)c.height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > surface->fb.width) x1 = surface->fb.width;
    if (y1 > surface->fb.height) y1 = surface->fb.height;

    surface->clip_x0 = (fdk_i32)x0;
    surface->clip_y0 = (fdk_i32)y0;
    surface->clip_x1 = (fdk_i32)x1;
    surface->clip_y1 = (fdk_i32)y1;
}

/* ---- damage bookkeeping ---- */

/* Adds a surface-local rect to the damage region, clamped to the
 * framebuffer. Fully-outside / empty rects are dropped. Overflow of
 * the bounded rect list degrades to full damage (documented in
 * fdk_surface.h). */
static void damage_add(fdk_surface *surface, fdk_rect rect) {
    if (surface->damage_full) {
        return; /* already "everything" */
    }
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }

    long long x0 = (long long)rect.x;
    long long y0 = (long long)rect.y;
    long long x1 = (long long)rect.x + (long long)rect.width;
    long long y1 = (long long)rect.y + (long long)rect.height;

    if (x1 <= 0 || y1 <= 0 || x0 >= surface->fb.width ||
        y0 >= surface->fb.height) {
        return; /* entirely outside */
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > surface->fb.width) x1 = surface->fb.width;
    if (y1 > surface->fb.height) y1 = surface->fb.height;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (surface->damage_count >= FDK_SURFACE_MAX_DAMAGE) {
        /* Bounded bookkeeping: degrade to "everything changed".
         * Correctness (present must cover every changed pixel) is
         * preserved; only the partial-redraw optimization is lost. */
        surface->damage_full = 1;
        surface->damage_count = 0;
        return;
    }

    surface->damage[surface->damage_count].x = (fdk_i32)x0;
    surface->damage[surface->damage_count].y = (fdk_i32)y0;
    surface->damage[surface->damage_count].width = (fdk_i32)(x1 - x0);
    surface->damage[surface->damage_count].height = (fdk_i32)(y1 - y0);
    surface->damage_count++;
}

/* ---- framebuffer acquisition ---- */

static fdk_result surface_acquire(fdk_surface *surface);

/* Acquires the backend framebuffer (window surfaces), or re-acquires
 * it if the previous one was invalidated by a present. Offscreen
 * surfaces own their pixels permanently and never take this path.
 * Fails with FDK_ERR_UNSUPPORTED if the backend provides no software
 * framebuffer — the ops fields are optional by design. */
static fdk_result surface_acquire(fdk_surface *surface) {
    if (surface->has_fb) {
        return FDK_OK;
    }
    if (surface->window->ops->window_get_framebuffer == NULL) {
        FDK_WARN("backend %s provides no software framebuffer",
                 surface->window->ops->name);
        return FDK_ERR_UNSUPPORTED;
    }
    fdk_platform_framebuffer fb;
    fdk_result r = surface->window->ops->window_get_framebuffer(
        surface->window->pwindow, &fb);
    if (!fdk_ok(r)) {
        return r;
    }

    /* A framebuffer at a NEW size means all-new (undefined) content:
     * reset the damage model to "everything changed" and note the
     * size damage is now recorded against. Same-size re-acquisition
     * keeps the accumulated damage — X11 returns the same image, and
     * the Wayland backend pre-fills fresh buffers with the visible
     * frame (see wayland_window.c), so in both cases the pixels
     * outside new damage genuinely still match the screen. */
    if (fb.width != surface->damage_w || fb.height != surface->damage_h) {
        surface->damage_full = 1;
        surface->damage_count = 0;
        surface->damage_w = fb.width;
        surface->damage_h = fb.height;
    }

    surface->fb = fb;
    surface->has_fb = 1;
    surface->ever_acquired = 1;
    recompute_clip(surface); /* fb bounds are part of the clip */
    return FDK_OK;
}

fdk_result fdk_window_get_surface(fdk_window *window,
                                  fdk_surface **out_surface) {
    if (window == NULL || out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ctx == NULL || window->ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    if (window->surface != NULL) {
        *out_surface = window->surface;
        return FDK_OK;
    }

    fdk_surface *surface = fdk_alloc(sizeof(fdk_surface));
    if (surface == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    surface->window = window;
    surface->offscreen = 0;
    surface->has_fb = 0;
    surface->fb.pixels = NULL;
    surface->fb.width = 0;
    surface->fb.height = 0;
    surface->fb.stride = 0;
    surface->own_pixels = NULL;
    surface->own_length = 0;
    surface->clip_depth = 0;
    surface->clip_x0 = 0;
    surface->clip_y0 = 0;
    surface->clip_x1 = 0;
    surface->clip_y1 = 0;
    surface->damage_count = 0;
    surface->damage_full = 1; /* nothing drawn yet = all of it unknown */
    surface->damage_w = 0;
    surface->damage_h = 0;
    surface->ever_acquired = 0;
    recompute_clip(surface);

    window->surface = surface;
    FDK_DEBUG("surface created");

    *out_surface = surface;
    return FDK_OK;
}

fdk_result fdk_surface_get_info(fdk_surface *surface,
                                fdk_surface_info *out_info) {
    if (surface == NULL || out_info == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_result r = surface_acquire(surface);
    if (!fdk_ok(r)) {
        return r;
    }

    out_info->pixels = surface->fb.pixels;
    out_info->width = surface->fb.width;
    out_info->height = surface->fb.height;
    out_info->stride = surface->fb.stride;
    out_info->format = FDK_SURFACE_FORMAT_XRGB8888;
    return FDK_OK;
}

fdk_result fdk_surface_present(fdk_surface *surface) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Offscreen surfaces have no display to present to; presenting
     * just closes their frame (resets damage) so the draw -> damage
     * -> present cycle behaves uniformly for every surface type. */
    if (surface->offscreen) {
        surface->damage_count = 0;
        surface->damage_full = 0;
        return FDK_OK;
    }

    if (surface->window->ops->window_present == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }

    /* Documented no-ops: nothing ever drawn, or nothing drawn since
     * the last present. The second case is the payoff of damage
     * tracking — an unchanged frame costs nothing (on Wayland, not
     * even a commit). */
    if (!surface->ever_acquired) {
        return FDK_OK;
    }
    if (!surface->damage_full && surface->damage_count == 0) {
        return FDK_OK;
    }

    fdk_platform_damage damage;
    memset(&damage, 0, sizeof(damage));
    damage.full = surface->damage_full;
    damage.count = surface->damage_full ? 0 : surface->damage_count;
    if (damage.count > 0) {
        memcpy(damage.rects, surface->damage,
               sizeof(fdk_rect) * (size_t)damage.count);
    }

    fdk_result r = surface->window->ops->window_present(
        surface->window->pwindow, &damage);
    if (!fdk_ok(r)) {
        return r; /* damage retained — a retry re-presents it */
    }

    /* Damage consumed. The buffer has been handed to the platform —
     * force the next drawing call / get_info to re-acquire. On
     * Wayland the mapping now belongs to the compositor until
     * release; on X11 this is merely a cheap size-checked
     * re-acquire. See surface_internal.h. */
    surface->damage_count = 0;
    surface->damage_full = 0;
    surface->has_fb = 0;
    return FDK_OK;
}

void fdk_surface_detach_from_window(fdk_window *window) {
    if (window == NULL || window->surface == NULL) {
        return;
    }
    /* No backend resources are owned by the surface itself — the
     * framebuffer belongs to the backend's platform window and is
     * released by its window_destroy (wl_buffer/XImage cleanup). Only
     * the bookkeeping object goes away here. */
    fdk_free(window->surface);
    window->surface = NULL;
}

/* ---- offscreen surfaces ---- */

fdk_result fdk_surface_create(fdk_i32 width, fdk_i32 height,
                              fdk_surface **out_surface) {
    if (out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Stride padded to a 16-pixel multiple: deliberately NOT equal to
     * width in general, so every primitive and blit always runs the
     * stride-aware path that real backends (X11 pads to 4 bytes;
     * future formats may pad more) exercise. */
    fdk_i32 stride = (width + 15) & ~15;
    size_t length = (size_t)stride * (size_t)height * sizeof(fdk_u32);

    fdk_u32 *pixels = fdk_alloc(length);
    if (pixels == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    fdk_surface *surface = fdk_alloc(sizeof(fdk_surface));
    if (surface == NULL) {
        fdk_free(pixels);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    surface->window = NULL;
    surface->offscreen = 1;
    surface->own_pixels = pixels;
    surface->own_length = length;
    surface->fb.pixels = pixels;
    surface->fb.width = width;
    surface->fb.height = height;
    surface->fb.stride = stride;
    surface->has_fb = 1; /* offscreen pixels are always valid */
    surface->clip_depth = 0;
    surface->damage_count = 0;
    surface->damage_full = 1; /* freshly created = nothing drawn */
    surface->damage_w = width;
    surface->damage_h = height;
    surface->ever_acquired = 1;
    recompute_clip(surface);

    FDK_DEBUG("offscreen surface created (%dx%d, stride %d)", width, height,
              stride);

    *out_surface = surface;
    return FDK_OK;
}

void fdk_surface_destroy(fdk_surface *surface) {
    if (surface == NULL) {
        return;
    }
    if (!surface->offscreen) {
        /* Window surfaces belong to their window (fdk_surface.h) and
         * are destroyed by fdk_window_destroy(). Refuse loudly
         * instead of corrupting the window's bookkeeping. */
        FDK_WARN("fdk_surface_destroy() called on a window surface — "
                 "ignored (window surfaces are destroyed with their "
                 "window; see fdk_surface.h)");
        return;
    }
    fdk_free(surface->own_pixels);
    fdk_free(surface);
    FDK_DEBUG("offscreen surface destroyed");
}

/* ---- damage public API ---- */

void fdk_surface_invalidate(fdk_surface *surface, fdk_rect rect) {
    if (surface == NULL) {
        return;
    }
    /* Window surfaces need a live framebuffer to clamp against; an
     * unpresented/unacquired one records damage as "everything"
     * (there is no meaningful partial region yet). */
    if (!surface->offscreen && !surface->has_fb) {
        if (!fdk_ok(surface_acquire(surface))) {
            return; /* backend refused acquisition — nothing to damage */
        }
    }
    damage_add(surface, rect);
}

void fdk_surface_invalidate_all(fdk_surface *surface) {
    if (surface == NULL) {
        return;
    }
    surface->damage_full = 1;
    surface->damage_count = 0;
}

bool fdk_surface_get_damage_bounds(fdk_surface *surface,
                                   fdk_rect *out_bounds) {
    if (surface == NULL || out_bounds == NULL) {
        return false;
    }
    if (surface->damage_full) {
        out_bounds->x = 0;
        out_bounds->y = 0;
        out_bounds->width = surface->fb.width;
        out_bounds->height = surface->fb.height;
        return true;
    }
    if (surface->damage_count == 0) {
        return false;
    }

    long long x0 = surface->damage[0].x;
    long long y0 = surface->damage[0].y;
    long long x1 = (long long)surface->damage[0].x + surface->damage[0].width;
    long long y1 =
        (long long)surface->damage[0].y + surface->damage[0].height;
    for (int i = 1; i < surface->damage_count; i++) {
        fdk_rect *d = &surface->damage[i];
        if (d->x < x0) x0 = d->x;
        if (d->y < y0) y0 = d->y;
        long long rx = (long long)d->x + d->width;
        long long ry = (long long)d->y + d->height;
        if (rx > x1) x1 = rx;
        if (ry > y1) y1 = ry;
    }
    out_bounds->x = (fdk_i32)x0;
    out_bounds->y = (fdk_i32)y0;
    out_bounds->width = (fdk_i32)(x1 - x0);
    out_bounds->height = (fdk_i32)(y1 - y0);
    return true;
}

/* ---- clip stack public API ---- */

fdk_result fdk_surface_push_clip(fdk_surface *surface, fdk_rect clip) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (clip.width <= 0 || clip.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->clip_depth >= FDK_SURFACE_CLIP_DEPTH) {
        return FDK_ERR_INVALID_ARGUMENT; /* bounded stack, documented */
    }

    /* Intersect with the clip below (pushing never expands). The
     * result may be empty — that is pushed as-is; every drawing
     * helper then no-ops until the stack unwinds. */
    if (surface->clip_depth > 0) {
        fdk_rect cur = surface->clip_stack[surface->clip_depth - 1];
        long long x0 = clip.x > cur.x ? (long long)clip.x : (long long)cur.x;
        long long y0 = clip.y > cur.y ? (long long)clip.y : (long long)cur.y;
        long long x1 = (long long)clip.x + clip.width;
        long long y1 = (long long)clip.y + clip.height;
        long long cx1 = (long long)cur.x + cur.width;
        long long cy1 = (long long)cur.y + cur.height;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;

        clip.x = (fdk_i32)x0;
        clip.y = (fdk_i32)y0;
        clip.width = x1 > x0 ? (fdk_i32)(x1 - x0) : 0;
        clip.height = y1 > y0 ? (fdk_i32)(y1 - y0) : 0;
    }

    surface->clip_stack[surface->clip_depth] = clip;
    surface->clip_depth++;
    recompute_clip(surface);
    return FDK_OK;
}

void fdk_surface_pop_clip(fdk_surface *surface) {
    if (surface == NULL || surface->clip_depth == 0) {
        return; /* documented defensive no-op */
    }
    surface->clip_depth--;
    recompute_clip(surface);
}

fdk_rect fdk_surface_get_clip(fdk_surface *surface) {
    if (surface == NULL || surface->clip_depth == 0) {
        /* "No clip" = the infinite plane (documented idiom; NOT the
         * fb bounds — geometry-composition callers want their own
         * coordinate space back). */
        fdk_rect infinite = { .x = INT32_MIN, .y = INT32_MIN,
                              .width = INT32_MAX, .height = INT32_MAX };
        return infinite;
    }
    return surface->clip_stack[surface->clip_depth - 1];
}

/* ---- frame pacing ---- */

bool fdk_surface_frame_ready(fdk_surface *surface) {
    if (surface == NULL || surface->offscreen) {
        return true; /* offscreen: no display, no pacing */
    }
    if (surface->window == NULL || surface->window->ops == NULL ||
        surface->window->ops->window_frame_ready == NULL) {
        return true; /* backend without frame feedback — always ready */
    }
    return surface->window->ops->window_frame_ready(
               surface->window->pwindow) != 0;
}

/* ---- drawing primitives ---- */

/* Blends `color` (straight alpha) source-over the pixel at (x, y),
 * clipped to the effective clip (which includes the fb bounds).
 * Inline-hot; called by every helper. */
static inline void blend_pixel(fdk_surface *surface, int x, int y,
                               fdk_color color) {
    if (x < surface->clip_x0 || y < surface->clip_y0 ||
        x >= surface->clip_x1 || y >= surface->clip_y1) {
        return;
    }

    fdk_u32 dst = surface->fb.pixels[(size_t)y * (size_t)surface->fb.stride +
                                     (size_t)x];
    fdk_f32 da_r = (fdk_f32)((dst >> 16) & 0xFFu) / 255.0f;
    fdk_f32 da_g = (fdk_f32)((dst >> 8) & 0xFFu) / 255.0f;
    fdk_f32 da_b = (fdk_f32)(dst & 0xFFu) / 255.0f;

    fdk_f32 a = color.a;
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 1.0f) {
        a = 1.0f;
    }

    /* Source-over with straight alpha on a destination that is
     * implicitly opaque (XRGB target): out = src*a + dst*(1-a). */
    fdk_f32 r = color.r * a + da_r * (1.0f - a);
    fdk_f32 g = color.g * a + da_g * (1.0f - a);
    fdk_f32 b = color.b * a + da_b * (1.0f - a);

    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;

    fdk_u32 px = ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
                 ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
                 (fdk_u32)(b * 255.0f + 0.5f);
    surface->fb.pixels[(size_t)y * (size_t)surface->fb.stride +
                       (size_t)x] = px;
}

/* Packs a color to its XRGB8888 pixel value, clamping channels. */
static fdk_u32 pack_color(fdk_color color) {
    fdk_f32 r = color.r < 0.0f ? 0.0f : (color.r > 1.0f ? 1.0f : color.r);
    fdk_f32 g = color.g < 0.0f ? 0.0f : (color.g > 1.0f ? 1.0f : color.g);
    fdk_f32 b = color.b < 0.0f ? 0.0f : (color.b > 1.0f ? 1.0f : color.b);
    return ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
           ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
           (fdk_u32)(b * 255.0f + 0.5f);
}

/* Clips a rect against the EFFECTIVE clip (clip stack intersected
 * with the fb bounds); returns nonzero if any part is inside.
 * Outputs the clipped span as [x0, x1) x [y0, y1). */
static int clip_rect(fdk_surface *surface, fdk_rect rect,
                     int *out_x0, int *out_y0, int *out_x1, int *out_y1) {
    long long x0 = (long long)rect.x;
    long long y0 = (long long)rect.y;
    long long x1 = (long long)rect.x + (long long)rect.width;
    long long y1 = (long long)rect.y + (long long)rect.height;

    if (x0 < surface->clip_x0) x0 = surface->clip_x0;
    if (y0 < surface->clip_y0) y0 = surface->clip_y0;
    if (x1 > surface->clip_x1) x1 = surface->clip_x1;
    if (y1 > surface->clip_y1) y1 = surface->clip_y1;

    *out_x0 = (int)x0;
    *out_y0 = (int)y0;
    *out_x1 = (int)x1;
    *out_y1 = (int)y1;
    return (x0 < x1 && y0 < y1);
}

/* Clips and damages a rect span in one step — the common prologue of
 * every rect-shaped primitive: returns nonzero if drawing should
 * proceed. */
static int clip_and_damage(fdk_surface *surface, fdk_rect rect,
                           int *out_x0, int *out_y0, int *out_x1,
                           int *out_y1) {
    if (!clip_rect(surface, rect, out_x0, out_y0, out_x1, out_y1)) {
        return 0;
    }
    damage_add(surface,
               (fdk_rect){ .x = *out_x0, .y = *out_y0,
                           .width = *out_x1 - *out_x0,
                           .height = *out_y1 - *out_y0 });
    return 1;
}

void fdk_surface_fill(fdk_surface *surface, fdk_color color) {
    if (surface == NULL) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    int x0 = surface->clip_x0;
    int y0 = surface->clip_y0;
    int x1 = surface->clip_x1;
    int y1 = surface->clip_y1;
    if (x0 >= x1 || y0 >= y1) {
        return; /* effectively empty clip */
    }
    damage_add(surface, (fdk_rect){ .x = x0, .y = y0,
                                    .width = x1 - x0, .height = y1 - y0 });

    /* Opaque colors are the common case for fill() — a memset-fast
     * path avoids per-pixel float math for them. */
    if (color.a >= 1.0f) {
        fdk_u32 px = pack_color(color);
        for (int y = y0; y < y1; y++) {
            fdk_u32 *row =
                surface->fb.pixels + (size_t)y * (size_t)surface->fb.stride;
            for (int x = x0; x < x1; x++) {
                row[x] = px;
            }
        }
        return;
    }

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            blend_pixel(surface, x, y, color);
        }
    }
}

void fdk_surface_fill_rect(fdk_surface *surface, fdk_rect rect,
                           fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_and_damage(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            blend_pixel(surface, x, y, color);
        }
    }
}

void fdk_surface_draw_rect(fdk_surface *surface, fdk_rect rect,
                           fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    /* Top and bottom edges (full width). */
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x, .y = rect.y,
                                      .width = rect.width, .height = 1 },
                          color);
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x,
                                      .y = rect.y + rect.height - 1,
                                      .width = rect.width, .height = 1 },
                          color);
    /* Left and right edges — strictly BETWEEN the horizontal ones so
     * no corner pixel is blended twice (with translucent colors a
     * double blend is a visible artifact, not a style choice). */
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x, .y = rect.y + 1,
                                      .width = 1,
                                      .height = rect.height - 2 },
                          color);
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x + rect.width - 1,
                                      .y = rect.y + 1,
                                      .width = 1,
                                      .height = rect.height - 2 },
                          color);
}

void fdk_surface_fill_gradient_vertical(fdk_surface *surface,
                                        fdk_rect rect,
                                        fdk_color top, fdk_color bottom) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_and_damage(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }

    /* Interpolate in the ORIGINAL rect's coordinate space so clipping
     * doesn't skew the gradient. */
    int inner_h = y1 - y0;
    for (int y = y0; y < y1; y++) {
        fdk_f32 t = (inner_h > 1)
            ? (fdk_f32)(y - y0) / (fdk_f32)(inner_h - 1)
            : 0.0f;
        fdk_color c = {
            .r = top.r + (bottom.r - top.r) * t,
            .g = top.g + (bottom.g - top.g) * t,
            .b = top.b + (bottom.b - top.b) * t,
            .a = top.a + (bottom.a - top.a) * t,
        };
        for (int x = x0; x < x1; x++) {
            blend_pixel(surface, x, y, c);
        }
    }
}

void fdk_surface_draw_line(fdk_surface *surface,
                           fdk_i32 x0, fdk_i32 y0,
                           fdk_i32 x1, fdk_i32 y1,
                           fdk_color color) {
    if (surface == NULL) {
        return;
    }
    if (x0 == x1 && y0 == y1) {
        fdk_surface_fill_rect(surface,
                              (fdk_rect){ .x = x0, .y = y0,
                                          .width = 1, .height = 1 },
                              color);
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    /* Damage = the line's bounding box clipped to the effective clip
     * (a line is convex, so this is exactly the drawn pixels' bbox). */
    fdk_rect bbox;
    bbox.x = x0 < x1 ? x0 : x1;
    bbox.y = y0 < y1 ? y0 : y1;
    bbox.width = (x0 < x1 ? x1 - x0 : x0 - x1) + 1;
    bbox.height = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    int cx0, cy0, cx1, cy1;
    if (!clip_rect(surface, bbox, &cx0, &cy0, &cx1, &cy1)) {
        return;
    }
    damage_add(surface, (fdk_rect){ .x = cx0, .y = cy0,
                                    .width = cx1 - cx0,
                                    .height = cy1 - cy0 });

    /* Bresenham over the major axis. */
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = x1 >= x0 ? 1 : -1;
    int sy = y1 >= y0 ? 1 : -1;
    int err = dx - dy;
    int x = x0;
    int y = y0;
    for (;;) {
        blend_pixel(surface, x, y, color);
        if (x == x1 && y == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

/* Integer square root rounded to nearest, exact for our magnitudes
 * (r <= 16384, so r*r <= 2^28 — comfortably inside f64 and i64
 * exactness). The float sqrt is corrected by +/-1 checks so
 * cross-platform float behavior can never produce a wrong integer. */
static fdk_i32 isqrt_round(fdk_i64 v) {
    if (v <= 0) {
        return 0;
    }
    fdk_i32 r = (fdk_i32)(sqrt((fdk_f64)v) + 0.5);
    while ((fdk_i64)r * r > v) {
        r--;
    }
    while ((fdk_i64)(r + 1) * (r + 1) <= v) {
        r++;
    }
    return r;
}

/* Clipped bbox of a circle/circle-banded shape, shared by the circle
 * helpers for damage; returns nonzero if any part is visible. */
static int circle_span(fdk_surface *surface, fdk_i32 cx, fdk_i32 cy,
                       fdk_i32 radius, fdk_rect *out_clipped_bbox) {
    fdk_rect bbox = { .x = cx - radius, .y = cy - radius,
                      .width = 2 * radius + 1, .height = 2 * radius + 1 };
    int x0, y0, x1, y1;
    if (!clip_rect(surface, bbox, &x0, &y0, &x1, &y1)) {
        return 0;
    }
    out_clipped_bbox->x = x0;
    out_clipped_bbox->y = y0;
    out_clipped_bbox->width = x1 - x0;
    out_clipped_bbox->height = y1 - y0;
    return 1;
}

void fdk_surface_draw_circle(fdk_surface *surface,
                             fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                             fdk_color color) {
    if (surface == NULL || radius <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    fdk_rect damaged;
    if (!circle_span(surface, cx, cy, radius, &damaged)) {
        return;
    }
    damage_add(surface, damaged);

    /* Midpoint circle via per-row chord ends, 8-way symmetric: for
     * each dy in [0, r], the chord half-length is sqrt(r^2 - dy^2);
     * plotting (+-dx, +-dy) and (+-dy, +-dx) covers every octant.
     * Cardinal points get plotted twice — harmless for the same
     * opaque color; for translucent colors the four axis pixels end
     * one step stronger, a known crisp-1px-outline tradeoff (full
     * geometric accuracy needs per-octant dedup; revisit with
     * antialiased outlines). */
    for (fdk_i32 dy = 0; dy <= radius; dy++) {
        fdk_i32 dx = isqrt_round((fdk_i64)radius * radius -
                                 (fdk_i64)dy * dy);
        blend_pixel(surface, cx + dx, cy + dy, color);
        blend_pixel(surface, cx - dx, cy + dy, color);
        blend_pixel(surface, cx + dx, cy - dy, color);
        blend_pixel(surface, cx - dx, cy - dy, color);
        if (dx != dy) { /* skip the diagonal duplicates */
            blend_pixel(surface, cx + dy, cy + dx, color);
            blend_pixel(surface, cx - dy, cy + dx, color);
            blend_pixel(surface, cx + dy, cy - dx, color);
            blend_pixel(surface, cx - dy, cy - dx, color);
        }
    }
}

void fdk_surface_fill_circle(fdk_surface *surface,
                             fdk_i32 cx, fdk_i32 cy, fdk_i32 radius,
                             fdk_color color) {
    if (surface == NULL || radius <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    fdk_rect damaged;
    if (!circle_span(surface, cx, cy, radius, &damaged)) {
        return;
    }
    damage_add(surface, damaged);

    /* Scanline fill: every row's horizontal chord [cx-half, cx+half]
     * where half = sqrt(r^2 - dy^2). blend_pixel enforces the clip
     * per pixel; rows are short-circuited only when the whole chord
     * is outside the clip's x range. */
    for (fdk_i32 dy = -radius; dy <= radius; dy++) {
        fdk_i32 half =
            isqrt_round((fdk_i64)radius * radius - (fdk_i64)dy * dy);
        for (fdk_i32 dx = -half; dx <= half; dx++) {
            blend_pixel(surface, cx + dx, cy + dy, color);
        }
    }
}

void fdk_surface_fill_rounded_rect(fdk_surface *surface, fdk_rect rect,
                                   fdk_i32 corner_radius, fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    /* Degenerate cases: no radius -> plain rect. */
    fdk_i32 max_radius =
        (rect.width < rect.height ? rect.width : rect.height) / 2;
    fdk_i32 r = corner_radius;
    if (r < 0) r = 0;
    if (r > max_radius) r = max_radius;
    if (r == 0) {
        fdk_surface_fill_rect(surface, rect, color);
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_and_damage(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }

    /* Corner centers: the top-left arc pivots around
     * (rect.x + r, rect.y + r), etc. Row spans:
     *   - middle rows (cyt <= y <= cyb): the FULL rect width;
     *   - corner-band rows: [cxl - half, cxr + half] with
     *     half = sqrt(r^2 - dy^2), dy = distance from the corner
     *     center row — half is 0 at the outermost row (the arc's
     *     tip, where the straight edge begins) and r at the corner
     *     center row, widening continuously to full width. */
    fdk_i32 cxl = rect.x + r;
    fdk_i32 cxr = rect.x + rect.width - 1 - r;
    fdk_i32 cyt = rect.y + r;
    fdk_i32 cyb = rect.y + rect.height - 1 - r;

    for (fdk_i32 y = rect.y; y < rect.y + rect.height; y++) {
        fdk_i32 half = r; /* middle rows: full width */
        if (y < cyt) {
            fdk_i32 dy = cyt - y;
            half = isqrt_round((fdk_i64)r * r - (fdk_i64)dy * dy);
        } else if (y > cyb) {
            fdk_i32 dy = y - cyb;
            half = isqrt_round((fdk_i64)r * r - (fdk_i64)dy * dy);
        }
        for (fdk_i32 x = cxl - half; x <= cxr + half; x++) {
            blend_pixel(surface, x, y, color);
        }
    }
}

void fdk_surface_draw_rounded_rect(fdk_surface *surface, fdk_rect rect,
                                   fdk_i32 corner_radius, fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    fdk_i32 max_radius =
        (rect.width < rect.height ? rect.width : rect.height) / 2;
    fdk_i32 r = corner_radius;
    if (r < 0) r = 0;
    if (r > max_radius) r = max_radius;
    if (r == 0) {
        fdk_surface_draw_rect(surface, rect, color);
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_and_damage(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }

    fdk_i32 cxl = rect.x + r;
    fdk_i32 cxr = rect.x + rect.width - 1 - r;
    fdk_i32 cyt = rect.y + r;
    fdk_i32 cyb = rect.y + rect.height - 1 - r;

    /* Straight edges, strictly between the arc endpoints so no pixel
     * is blended twice (see draw_rect for why that matters with
     * translucent colors). */
    if (rect.width > 2 * r) {
        for (fdk_i32 x = cxl + 1; x <= cxr - 1; x++) {
            blend_pixel(surface, x, rect.y, color);
            blend_pixel(surface, x, rect.y + rect.height - 1, color);
        }
    }
    if (rect.height > 2 * r) {
        for (fdk_i32 y = cyt + 1; y <= cyb - 1; y++) {
            blend_pixel(surface, rect.x, y, color);
            blend_pixel(surface, rect.x + rect.width - 1, y, color);
        }
    }

    /* Four quarter arcs. For corner center (cx, cy) the arc runs
     * from (cx +- r, cy) to (cx, cy +- r); plotting (cx -+ dx, cy -+
     * dy) for dy in [0, r] with dx = sqrt(r^2 - dy^2) walks it exactly
     * once — no double-blended pixels, unlike the full-circle
     * outline. */
    for (fdk_i32 dy = 0; dy <= r; dy++) {
        fdk_i32 dx = isqrt_round((fdk_i64)r * r - (fdk_i64)dy * dy);
        blend_pixel(surface, cxl - dx, cyt - dy, color); /* top-left  */
        blend_pixel(surface, cxr + dx, cyt - dy, color); /* top-right */
        blend_pixel(surface, cxl - dx, cyb + dy, color); /* bot-left  */
        blend_pixel(surface, cxr + dx, cyb + dy, color); /* bot-right */
    }
}

fdk_result fdk_surface_blit(fdk_surface *dst, fdk_i32 dst_x, fdk_i32 dst_y,
                            fdk_surface *src, fdk_rect src_rect) {
    if (dst == NULL || src == NULL || src_rect.width <= 0 ||
        src_rect.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Both surfaces need live framebuffers. Acquiring the source may
     * allocate a backend buffer (Wayland) — which is why the public
     * parameter is deliberately non-const despite the read-only use. */
    fdk_result r = surface_acquire(dst);
    if (!fdk_ok(r)) {
        return r;
    }
    r = surface_acquire(src);
    if (!fdk_ok(r)) {
        return r;
    }

    /* Clip the source rect to the source bounds first. */
    long long sx0 = src_rect.x;
    long long sy0 = src_rect.y;
    long long sx1 = (long long)src_rect.x + src_rect.width;
    long long sy1 = (long long)src_rect.y + src_rect.height;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > src->fb.width) sx1 = src->fb.width;
    if (sy1 > src->fb.height) sy1 = src->fb.height;
    if (sx0 >= sx1 || sy0 >= sy1) {
        return FDK_OK; /* source region entirely outside — nothing to do */
    }

    /* Map onto the destination and clip to the destination's
     * effective clip (bounds + clip stack). */
    long long dx0 = (long long)dst_x + (sx0 - src_rect.x);
    long long dy0 = (long long)dst_y + (sy0 - src_rect.y);
    long long dx1 = dx0 + (sx1 - sx0);
    long long dy1 = dy0 + (sy1 - sy0);

    if (dx1 <= dst->clip_x0 || dy1 <= dst->clip_y0 ||
        dx0 >= dst->clip_x1 || dy0 >= dst->clip_y1) {
        return FDK_OK; /* destination region entirely clipped away */
    }
    if (dx0 < dst->clip_x0) {
        sx0 += dst->clip_x0 - dx0;
        dx0 = dst->clip_x0;
    }
    if (dy0 < dst->clip_y0) {
        sy0 += dst->clip_y0 - dy0;
        dy0 = dst->clip_y0;
    }
    if (dx1 > dst->clip_x1) dx1 = dst->clip_x1;
    if (dy1 > dst->clip_y1) dy1 = dst->clip_y1;
    if (dx0 >= dx1 || dy0 >= dy1) {
        return FDK_OK;
    }

    /* Opaque row copies (documented: blit is an opaque pixel copy). */
    size_t copy_len = (size_t)(dx1 - dx0);
    for (long long row = 0; row < dy1 - dy0; row++) {
        fdk_u32 *dst_row =
            dst->fb.pixels +
            (size_t)(dy0 + row) * (size_t)dst->fb.stride + (size_t)dx0;
        const fdk_u32 *src_row =
            src->fb.pixels +
            (size_t)(sy0 + row) * (size_t)src->fb.stride + (size_t)sx0;
        memcpy(dst_row, src_row, copy_len * sizeof(fdk_u32));
    }

    damage_add(dst, (fdk_rect){ .x = (fdk_i32)dx0, .y = (fdk_i32)dy0,
                                .width = (fdk_i32)(dx1 - dx0),
                                .height = (fdk_i32)(dy1 - dy0) });
    return FDK_OK;
}
