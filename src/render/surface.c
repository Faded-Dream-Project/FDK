#define FDK_LOG_TAG "render"

#include "fdk/fdk_surface.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "window/window_internal.h"

/* ---- framebuffer acquisition ---- */

/* Acquires the backend framebuffer, or re-acquires it if the previous
 * one was invalidated by a present (see struct fdk_surface). Fails
 * with FDK_ERR_UNSUPPORTED if this backend provides no software
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
    surface->fb = fb;
    surface->has_fb = 1;
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
    surface->has_fb = 0;
    surface->fb.pixels = NULL;
    surface->fb.width = 0;
    surface->fb.height = 0;
    surface->fb.stride = 0;

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
    if (surface->window->ops->window_present == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_result r = surface->window->ops->window_present(
        surface->window->pwindow);
    if (!fdk_ok(r)) {
        return r;
    }

    /* The buffer has been handed to the platform — force the next
     * drawing call / get_info to re-acquire. On Wayland the mapping
     * now belongs to the compositor until release; on X11 this is
     * merely a cheap size-checked re-acquire. See
     * surface_internal.h. */
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

/* ---- drawing primitives ---- */

/* Blends `color` (straight alpha) source-over the pixel at (x, y),
 * clipped to the framebuffer. Inline-hot; called by every helper. */
static inline void blend_pixel(fdk_surface *surface, int x, int y,
                               fdk_color color) {
    if (x < 0 || y < 0 || x >= surface->fb.width ||
        y >= surface->fb.height) {
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

/* Clips a rect against the framebuffer; returns nonzero if any part
 * is inside. Outputs the clipped span as [x0, x1) x [y0, y1). */
static int clip_rect(fdk_surface *surface, fdk_rect rect,
                     int *out_x0, int *out_y0, int *out_x1, int *out_y1) {
    long long x0 = (long long)rect.x;
    long long y0 = (long long)rect.y;
    long long x1 = (long long)rect.x + (long long)rect.width;
    long long y1 = (long long)rect.y + (long long)rect.height;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > surface->fb.width) x1 = surface->fb.width;
    if (y1 > surface->fb.height) y1 = surface->fb.height;

    *out_x0 = (int)x0;
    *out_y0 = (int)y0;
    *out_x1 = (int)x1;
    *out_y1 = (int)y1;
    return (x0 < x1 && y0 < y1);
}

void fdk_surface_fill(fdk_surface *surface, fdk_color color) {
    if (surface == NULL) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }

    /* Opaque colors are the common case for fill() — a memset-fast
     * path avoids per-pixel float math for them. */
    if (color.a >= 1.0f) {
        fdk_f32 r = color.r < 0.0f ? 0.0f : (color.r > 1.0f ? 1.0f : color.r);
        fdk_f32 g = color.g < 0.0f ? 0.0f : (color.g > 1.0f ? 1.0f : color.g);
        fdk_f32 b = color.b < 0.0f ? 0.0f : (color.b > 1.0f ? 1.0f : color.b);
        fdk_u32 px = ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
                     ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
                     (fdk_u32)(b * 255.0f + 0.5f);
        for (int y = 0; y < surface->fb.height; y++) {
            fdk_u32 *row =
                surface->fb.pixels + (size_t)y * (size_t)surface->fb.stride;
            for (int x = 0; x < surface->fb.width; x++) {
                row[x] = px;
            }
        }
        return;
    }

    for (int y = 0; y < surface->fb.height; y++) {
        for (int x = 0; x < surface->fb.width; x++) {
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
    if (!clip_rect(surface, rect, &x0, &y0, &x1, &y1)) {
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

    /* Top and bottom edges. */
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x, .y = rect.y,
                                      .width = rect.width, .height = 1 },
                          color);
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x,
                                      .y = rect.y + rect.height - 1,
                                      .width = rect.width, .height = 1 },
                          color);
    /* Left and right edges. */
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x, .y = rect.y,
                                      .width = 1, .height = rect.height },
                          color);
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = rect.x + rect.width - 1,
                                      .y = rect.y,
                                      .width = 1, .height = rect.height },
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
    if (!clip_rect(surface, rect, &x0, &y0, &x1, &y1)) {
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
