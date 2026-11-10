#define FDK_LOG_TAG "render"

#include "fdk/fdk_surface.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "gpaint.h"
#include "surface_internal.h"
#include "window/window_internal.h"

#include <math.h>
#include <stdio.h> /* printf (FDK_PRESENT_TRACE, round 14) */
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
void fdk__surface_damage_add(fdk_surface *surface, fdk_rect rect) {
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

    /* Round 22 (the owner's present-trace report: 3.29M px damaged
     * on a 2.07M window — 59% double-counting): MERGE the rect into
     * any existing entry it overlaps or touches, instead of
     * appending it. Overlapping rects made the present path ship
     * the same pixels twice (two XPutImage sub-puts covering one
     * region) and tripped the >=75% whole-image coarsening on
     * inflated area. Merging keeps the list overlap-free; adjacency
     * counts as overlap (a merged rect is always <= the two puts
     * it replaces). The merge re-scans once — a merged rect can
     * now overlap ANOTHER entry — bounded by the list size (64). */
    for (int i = 0; i < surface->damage_count; i++) {
        long long ex0 = surface->damage[i].x;
        long long ey0 = surface->damage[i].y;
        long long ex1 = (long long)surface->damage[i].x +
                        surface->damage[i].width;
        long long ey1 = (long long)surface->damage[i].y +
                        surface->damage[i].height;
        if (x0 <= ex1 && ex0 <= x1 && y0 <= ey1 && ey0 <= y1) {
            /* union in place; <=/>= keep degenerate empty sides
             * out (x0 < x1, y0 < y1 are already established)     */
            if (ex0 > x0) surface->damage[i].x = (fdk_i32)x0;
            if (ey0 > y0) surface->damage[i].y = (fdk_i32)y0;
            if (ex1 < x1) ex1 = x1;
            if (ey1 < y1) ey1 = y1;
            surface->damage[i].width =
                (fdk_i32)(ex1 - surface->damage[i].x);
            surface->damage[i].height =
                (fdk_i32)(ey1 - surface->damage[i].y);
            /* the widened rect may now swallow neighbors: one
             * compaction pass removes any entry it contains      */
            for (int j = i + 1; j < surface->damage_count; j++) {
                long long nx0 = surface->damage[j].x;
                long long ny0 = surface->damage[j].y;
                long long nx1 = (long long)surface->damage[j].x +
                                surface->damage[j].width;
                long long ny1 = (long long)surface->damage[j].y +
                                surface->damage[j].height;
                if (nx0 >= surface->damage[i].x &&
                    ny0 >= surface->damage[i].y &&
                    nx1 <= ex1 && ny1 <= ey1) {
                    memmove(&surface->damage[j],
                            &surface->damage[j + 1],
                            sizeof(fdk_rect) *
                                (size_t)(surface->damage_count -
                                         j - 1));
                    surface->damage_count--;
                    j--; /* re-examine the shifted-in entry        */
                }
            }
            return;
        }
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

/* Both renamed to fdk__* (shared across the render TUs, see
 * surface_internal.h); the original names stay as macros so the many
 * call sites in this file read unchanged. */
#define surface_acquire fdk__surface_acquire
#define damage_add fdk__surface_damage_add

/* Acquires the backend framebuffer (window surfaces), or re-acquires
 * it if the previous one was invalidated by a present. Offscreen
 * surfaces own their pixels permanently and never take this path.
 * Fails with FDK_ERR_UNSUPPORTED if the backend provides no software
 * framebuffer — the ops fields are optional by design. */
fdk_result fdk__surface_acquire(fdk_surface *surface) {
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

    /* GPU render mode arrives here as a SIZE-ONLY framebuffer
     * (pixels == NULL — the backend's GPU-mode contract). Bring the
     * paint engine up on first acquire and track resizes; failure
     * marks the surface gpu_failed (GPU mode has no CPU fallback by
     * contract — present will report the error, not fake a frame). */
    if (surface->fb.pixels == NULL) {
        if (surface->gpu != NULL) {
            fdk__gpu_surface_resize(surface, surface->fb.width,
                                    surface->fb.height);
        } else if (!surface->gpu_failed) {
            if (fdk__gpu_surface_init(surface, surface->fb.width,
                                      surface->fb.height)) {
                fdk__gpu_register_window(surface->window->pwindow,
                                         surface);
            } else {
                surface->gpu_failed = 1;
                FDK_ERROR("GPU paint engine init failed; GPU mode has "
                          "no CPU fallback by contract\n");
            }
        }
    }

    return FDK_OK;
}

void fdk__surface_drop_framebuffer(fdk_surface *surface) {
    if (surface == NULL || surface->offscreen) {
        return; /* offscreen pixels are permanent */
    }
    /* Keep damage bookkeeping as-is: the next acquire resets to full
     * damage if the re-fetched framebuffer's size differs (see
     * fdk__surface_acquire) and preserves it when the size is
     * unchanged, in which case the pixels are still valid on both
     * backends (X11 returns the same image; Wayland pre-fills fresh
     * buffers with the visible frame). The clip re-derives from the
     * re-acquired fb bounds — every drawing helper runs the acquire
     * prologue first. */
    surface->has_fb = 0;
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
    surface->format = FDK_SURFACE_FORMAT_XRGB8888; /* backends hand out XRGB */
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
    out_info->format = surface->format;
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

    /* GPU render mode: the batch IS the frame. Flush the engine with
     * the lane context current and swap — no CPU framebuffer exists
     * (the size-only fb contract), so damage bookkeeping never
     * applies. Nothing-pending still swaps: re-presents the last
     * frame cheaply, which matches the CPU path's visible behavior. */
    if (surface->gpu != NULL || surface->gpu_failed) {
        if (surface->gpu_failed ||
            surface->window->ops->window_gpu_flush == NULL ||
            fdk__gpu_engine_dead(surface)) {
            /* A context-lost engine reports UNSUPPORTED: no stale
             * frame, no fake OK (the same honest answer as init
             * failure — GPU mode has no CPU fallback by contract). */
            return FDK_ERR_UNSUPPORTED;
        }
        return surface->window->ops->window_gpu_flush(
            surface->window->pwindow, surface);
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

    /* Popup latency trace (1.0.0-rc1, X2): THE convergence point of
     * every present — the widget-tree auto-paint tail and the
     * application-driven presents of extension-API popups (the demo
     * bar paints its popup surface directly) both land here. A motion
     * stamped by the dispatcher (window.c) that is still unpresented
     * when this present completes is exactly the "hover highlight
     * trails the pointer" latency the X2 report asked to measure;
     * one line per motion-to-present pair (the stamp resets). */
    if (surface->window->is_popup && fdk__popup_trace_enabled() &&
        surface->window->popup_motion_ms != 0) {
        FDK_INFO("[popup] motion->present %lld ms",
                 (long long)(fdk__window_now_ms() -
                             surface->window->popup_motion_ms));
        surface->window->popup_motion_ms = 0;
    }

    /* Damage consumed. The buffer has been handed to the platform —
     * force the next drawing call / get_info to re-acquire. On
     * Wayland the mapping now belongs to the compositor until
     * release; on X11 this is merely a cheap size-checked
     * re-acquire. See surface_internal.h. */
    surface->damage_count = 0;
    surface->damage_full = 0;
    surface->has_fb = 0;

    /* Round 14 (the scroll-perf rigs): one line per real present.
     * full=1 means whole-surface (or degraded bookkeeping); rects
     * reports the bounded list the platform actually shipped. The
     * damaged-area sum is post-clamp (what really went over the
     * wire). Off by default — see fdk__present_trace_enabled. */
    if (fdk__present_trace_enabled()) {
        long long area = 0;
        if (!damage.full) {
            for (int i = 0; i < damage.count; i++) {
                area += (long long)damage.rects[i].width *
                        (long long)damage.rects[i].height;
            }
        } else {
            area = (long long)surface->fb.width *
                   (long long)surface->fb.height;
        }
        printf("PRESENT-TRACE %lld %s %lld %lld %d\n",
               fdk__window_now_ms(), damage.full ? "full" : "rects",
               area, (long long)surface->fb.width *
                         (long long)surface->fb.height,
               damage.full ? 0 : damage.count);
        fflush(stdout);
    }
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
    fdk__gpu_unregister_window(window->pwindow);
    fdk__gpu_surface_destroy(window->surface);
    fdk_free(window->surface);
    window->surface = NULL;
}

/* ---- offscreen surfaces ---- */

/* Shared body of fdk_surface_create / fdk_surface_create_format. */
static fdk_result surface_create_common(fdk_i32 width, fdk_i32 height,
                                        fdk_surface_format format,
                                        fdk_surface **out_surface) {
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
    surface->format = format;
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

    FDK_DEBUG("offscreen surface created (%dx%d, stride %d, format %d)",
              width, height, stride, (int)format);

    *out_surface = surface;
    return FDK_OK;
}

fdk_result fdk_surface_create(fdk_i32 width, fdk_i32 height,
                              fdk_surface **out_surface) {
    if (out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return surface_create_common(width, height, FDK_SURFACE_FORMAT_XRGB8888,
                                 out_surface);
}

fdk_result fdk_surface_create_format(fdk_i32 width, fdk_i32 height,
                                     fdk_surface_format format,
                                     fdk_surface **out_surface) {
    if (out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (format != FDK_SURFACE_FORMAT_XRGB8888 &&
        format != FDK_SURFACE_FORMAT_ARGB8888) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return surface_create_common(width, height, format, out_surface);
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

/* Round 22 test seam: the damage-list entry count (the overlap-
 * merge test's assertion point). Not exported through fdk_surface.h
 * — damage internals stay private; the test declares it extern. */
fdk_u32 fdk__test_damage_count(const fdk_surface *surface);
fdk_u32 fdk__test_damage_count(const fdk_surface *surface) {
    if (surface == NULL || surface->damage_full) {
        return (surface != NULL && surface->damage_full)
                   ? FDK_SURFACE_MAX_DAMAGE + 1u
                   : 0u;
    }
    return (fdk_u32)surface->damage_count;
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
    if (surface->gpu != NULL) {
        fdk__gpu_sync_clip(surface);
    }
    return FDK_OK;
}

void fdk_surface_pop_clip(fdk_surface *surface) {
    if (surface == NULL || surface->clip_depth == 0) {
        return; /* documented defensive no-op */
    }
    surface->clip_depth--;
    recompute_clip(surface);
    if (surface->gpu != NULL) {
        fdk__gpu_sync_clip(surface);
    }
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
 * Inline-hot; called by every helper.
 *
 * XRGB destinations (every window surface, plain offscreen surfaces):
 * the destination is implicitly opaque, so out = src*a + dst*(1-a).
 *
 * ARGB destinations: the full straight-alpha source-over, including
 * the alpha channel — out_a = sa + da*(1-sa), out_rgb =
 * (src_rgb*sa + dst_rgb*da*(1-sa)) / out_a — so translucent drawing
 * onto a partially transparent sprite accumulates correctly when the
 * sprite is later composited by blit_blend/blit_transformed. */
static inline void blend_pixel(fdk_surface *surface, int x, int y,
                               fdk_color color) {
    if (x < surface->clip_x0 || y < surface->clip_y0 ||
        x >= surface->clip_x1 || y >= surface->clip_y1) {
        return;
    }

    fdk_u32 *px = surface->fb.pixels + (size_t)y * (size_t)surface->fb.stride +
                   (size_t)x;
    fdk_u32 dst = *px;
    fdk_f32 da_r = (fdk_f32)((dst >> 16) & 0xFFu) / 255.0f;
    fdk_f32 da_g = (fdk_f32)((dst >> 8) & 0xFFu) / 255.0f;
    fdk_f32 da_b = (fdk_f32)(dst & 0xFFu) / 255.0f;

    fdk_f32 a = color.a;
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 1.0f) {
        a = 1.0f;
    }

    fdk_f32 r, g, b, out_a;
    if (surface->format == FDK_SURFACE_FORMAT_ARGB8888) {
        fdk_f32 da = (fdk_f32)((dst >> 24) & 0xFFu) / 255.0f;
        fdk_f32 inv = 1.0f - a;
        out_a = a + da * inv;
        if (out_a <= 0.0f) {
            r = g = b = 0.0f;
        } else {
            /* Source-over with a NON-opaque destination (see comment
             * above); da == 1 reduces exactly to the XRGB formula. */
            fdk_f32 w_s = a / out_a;
            fdk_f32 w_d = da * inv / out_a;
            r = color.r * w_s + da_r * w_d;
            g = color.g * w_s + da_g * w_d;
            b = color.b * w_s + da_b * w_d;
        }
    } else {
        out_a = 1.0f;
        r = color.r * a + da_r * (1.0f - a);
        g = color.g * a + da_g * (1.0f - a);
        b = color.b * a + da_b * (1.0f - a);
    }

    if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;

    fdk_u32 packed = ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
                     ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
                     (fdk_u32)(b * 255.0f + 0.5f);
    if (surface->format == FDK_SURFACE_FORMAT_ARGB8888) {
        if (out_a < 0.0f) out_a = 0.0f;
        if (out_a > 1.0f) out_a = 1.0f;
        packed |= (fdk_u32)(out_a * 255.0f + 0.5f) << 24;
    }
    *px = packed;
}

/* Packs a color to its XRGB8888 pixel value, clamping channels. The
 * top byte is 0 — meaningless for XRGB surfaces (ignored), and only
 * correct for OPAQUE ARGB fills when OR'd with 0xFF000000 (see
 * pack_opaque_argb below). */
static fdk_u32 pack_color(fdk_color color) {
    fdk_f32 r = color.r < 0.0f ? 0.0f : (color.r > 1.0f ? 1.0f : color.r);
    fdk_f32 g = color.g < 0.0f ? 0.0f : (color.g > 1.0f ? 1.0f : color.g);
    fdk_f32 b = color.b < 0.0f ? 0.0f : (color.b > 1.0f ? 1.0f : color.b);
    return ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
           ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
           (fdk_u32)(b * 255.0f + 0.5f);
}

/* pack_color | full alpha — the opaque-fill value for ARGB surfaces. */
static fdk_u32 pack_opaque_argb(fdk_color color) {
    return pack_color(color) | 0xFF000000u;
}

/* Coverage-weighted blend (internal) — the antialiased primitives'
 * atom: blends `color` with alpha scaled by `coverage` (0..1) over the
 * pixel at (x, y), clip-checked and format-aware exactly like
 * blend_pixel (which is what it runs underneath). Used by
 * surface_aa.c. */
void fdk__surface_blend_coverage(fdk_surface *surface, int x, int y,
                                 fdk_color color, fdk_f32 coverage) {
    if (coverage <= 0.0f) {
        return;
    }
    if (coverage >= 1.0f) {
        blend_pixel(surface, x, y, color);
        return;
    }
    fdk_color c = color;
    c.a = color.a * coverage;
    blend_pixel(surface, x, y, c);
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

static void fill_rect_translucent(fdk_surface *surface, int x0, int y0,
                                  int x1, int y1, fdk_color color);

#ifdef FDK_R17_BENCH
#include <stdio.h>
#include <time.h>
/* Reference: the OLD per-pixel path (blend_pixel inlined by hand,
 * same math) — the exactness oracle for the fast loop. */
static void ref_fill(fdk_surface *s, fdk_rect rect, fdk_color color) {
    for (int y = rect.y; y < rect.y + rect.height; y++) {
        for (int x = rect.x; x < rect.x + rect.width; x++) {
            blend_pixel(s, x, y, color);
        }
    }
}
void fdk__r17_bench(void) {
    fdk_surface *a = NULL, *b = NULL;
    if (!fdk_ok(fdk_surface_create_format(400, 400,
            FDK_SURFACE_FORMAT_XRGB8888, &a)) ||
        !fdk_ok(fdk_surface_create_format(400, 400,
            FDK_SURFACE_FORMAT_ARGB8888, &b))) {
        printf("BENCH: alloc failed\n");
        return;
    }
    fdk_color base = { .r = 0.2f, .g = 0.5f, .b = 0.8f, .a = 1.0f };
    fdk_color over = { .r = 0.9f, .g = 0.4f, .b = 0.1f, .a = 97.0f / 255.0f };
    /* Exactness: every dst byte value x every channel — exhaustive
     * 256-step sweep on a 256x256 patch pair, both formats. */
    for (int fmt = 0; fmt < 2; fmt++) {
        fdk_surface *s1 = NULL, *s2 = NULL;
        fdk_surface_create_format(256, 256, fmt ? FDK_SURFACE_FORMAT_ARGB8888
                                                : FDK_SURFACE_FORMAT_XRGB8888, &s1);
        fdk_surface_create_format(256, 256, fmt ? FDK_SURFACE_FORMAT_ARGB8888
                                                : FDK_SURFACE_FORMAT_XRGB8888, &s2);
        fdk_surface_fill(s1, base);
        fdk_surface_fill(s2, base);
        /* vary dst: paint 256 gray levels, one per row */
        for (int i = 0; i < 256; i++) {
            fdk_color g = { .r = i / 255.0f, .g = i / 255.0f,
                            .b = (255 - i) / 255.0f, .a = 1.0f };
            fdk_rect rr = { 0, i, 256, 1 };
            fdk_surface_fill_rect(s1, rr, g);
            fdk_surface_fill_rect(s2, rr, g);
        }
        ref_fill(s1, (fdk_rect){0, 0, 256, 256}, over);
        fill_rect_translucent(s2, 0, 0, 256, 256, over);
        fdk_u32 *p1 = s1->fb.pixels, *p2 = s2->fb.pixels;
        int bad = 0;
        for (int i = 0; i < 256 * 256; i++) {
            if (p1[i] != p2[i]) { bad++; }
        }
        printf("BENCH exactness fmt=%s: %s (%d differ)\n",
               fmt ? "ARGB" : "XRGB", bad ? "FAIL" : "EXACT", bad);
        fdk_surface_destroy(s1);
        fdk_surface_destroy(s2);
    }
    /* Speed 1: uniform-background fill (the row-highlight shape:
     * one constant destination under the whole rect), XRGB. */
    fdk_surface_fill(a, base);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 2000; i++) {
        fill_rect_translucent(a, 0, 0, 400, 400, over);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double fast = (t1.tv_sec - t0.tv_sec) * 1e3 +
                  (t1.tv_nsec - t0.tv_nsec) / 1e6;
    fdk_surface_fill(a, base);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 2000; i++) {
        ref_fill(a, (fdk_rect){0, 0, 400, 400}, over);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double slow = (t1.tv_sec - t0.tv_sec) * 1e3 +
                  (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("BENCH uniform 400x400x2000: old=%.1fms new=%.1fms "
           "speedup=%.2fx\n", slow, fast, slow / fast);
    /* Speed 2: mixed destination (the anti-run worst case: every
     * pixel differs — the run detector collapses nothing). */
    for (int i = 0; i < 400; i++) {
        for (int j = 0; j < 400; j++) {
            fdk_u32 *px = a->fb.pixels +
                          (size_t)i * (size_t)a->fb.stride + j;
            *px = (fdk_u32)((i * 7 + j * 13) & 0xFFFFFF);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 2000; i++) {
        fill_rect_translucent(a, 0, 0, 400, 400, over);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double fast2 = (t1.tv_sec - t0.tv_sec) * 1e3 +
                   (t1.tv_nsec - t0.tv_nsec) / 1e6;
    for (int i = 0; i < 400; i++) {
        for (int j = 0; j < 400; j++) {
            fdk_u32 *px = a->fb.pixels +
                          (size_t)i * (size_t)a->fb.stride + j;
            *px = (fdk_u32)((i * 7 + j * 13) & 0xFFFFFF);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 2000; i++) {
        ref_fill(a, (fdk_rect){0, 0, 400, 400}, over);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double slow2 = (t1.tv_sec - t0.tv_sec) * 1e3 +
                   (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("BENCH mixed  400x400x2000: old=%.1fms new=%.1fms "
           "speedup=%.2fx\n", slow2, fast2, slow2 / fast2);
    fdk_surface_destroy(a);
    fdk_surface_destroy(b);
}
#endif

void fdk_surface_fill(fdk_surface *surface, fdk_color color) {
    if (surface == NULL) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }
    if (surface->gpu != NULL) {
        fdk__gpu_fill(surface, color);
        return;
    }
    if (surface->gpu_failed) {
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
     * path avoids per-pixel float math for them. A 0-alpha fill on
     * an ARGB surface is an exact CLEAR (source-over with a == 0
     * leaves dst untouched, but the CLEAR intent is 0x00000000 —
     * the HiDPI intermediate's per-frame reset, which previously
     * blended nothing and left stale pixels; see window.c). */
    if (color.a <= 0.0f &&
        surface->format == FDK_SURFACE_FORMAT_ARGB8888) {
        for (int y = y0; y < y1; y++) {
            fdk_u32 *row =
                surface->fb.pixels + (size_t)y * (size_t)surface->fb.stride;
            memset(row + x0, 0, (size_t)(x1 - x0) * sizeof(fdk_u32));
        }
        return;
    }
    if (color.a >= 1.0f) {
        fdk_u32 px = (surface->format == FDK_SURFACE_FORMAT_ARGB8888)
                         ? pack_opaque_argb(color)
                         : pack_color(color);
        for (int y = y0; y < y1; y++) {
            fdk_u32 *row =
                surface->fb.pixels + (size_t)y * (size_t)surface->fb.stride;
            for (int x = x0; x < x1; x++) {
                row[x] = px;
            }
        }
        return;
    }

    fill_rect_translucent(surface, x0, y0, x1, y1, color);
}

/* The translucent-fill inner loop (round 17): blend_pixel's exact
 * float math with everything PER-PIXEL-INVARIANT hoisted out — the
 * clip test (done by the caller's rect math), the format branch,
 * the alpha clamp, the channel unpack of the constant SOURCE, and
 * the constant weight products. What remains per pixel: unpack the
 * destination, three multiply-adds, repack, store. Byte-identical
 * to blend_pixel by construction (same expressions, same rounding),
 * so the pixel-identity gates hold for both surface formats. */
static void fill_rect_translucent(fdk_surface *surface, int x0, int y0,
                                  int x1, int y1, fdk_color color) {
    fdk_f32 a = color.a;
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 1.0f) {
        a = 1.0f;
    }
    if (a <= 0.0f) {
        return; /* fully transparent: nothing changes */
    }
    /* The clamped 0..1 source channels, exactly as blend_pixel
     * clamps them before use. */
    fdk_f32 sr = color.r < 0.0f ? 0.0f
                : (color.r > 1.0f ? 1.0f : color.r);
    fdk_f32 sg = color.g < 0.0f ? 0.0f
                : (color.g > 1.0f ? 1.0f : color.g);
    fdk_f32 sb = color.b < 0.0f ? 0.0f
                : (color.b > 1.0f ? 1.0f : color.b);
    int argb = surface->format == FDK_SURFACE_FORMAT_ARGB8888;
    fdk_f32 inv = 1.0f - a;

    /* Round 18, the XRGB table pass. Window surfaces are XRGB
     * everywhere (fdk_window_get_surface), so THIS is the drag-
     * highlight path on real screens. Two structural wins, both
     * BIT-EXACT because they only MOVE where the same float
     * expressions are evaluated:
     *
     *  1. Per-fill tables: for XRGB the blended byte depends on
     *     the destination only through each channel's own 0..255
     *     value, so out = T_c[dst_c] where T_c[c] is EXACTLY the
     *     old expression for destination channel value c — the
     *     same float ops, same order, same rounding, evaluated
     *     256 times per fill instead of once per pixel. The
     *     per-pixel work collapses to three table loads, three
     *     shifts, and two ORs — no float at all.
     *  2. Uniform runs: a row over a constant destination is one
     *     constant output word — the fill becomes an unrolled
     *     store loop (the opaque path's shape). Selection over
     *     the plain list background hits this for nearly every
     *     pixel of every row.
     *
     * The 4-wide ARGB unroll below is unchanged: alpha-weighted
     * blending has no per-channel table (out depends on the
     * DESTINATION ALPHA too), so it keeps the hoisted-float form. */
    if (!argb) {
        fdk_u32 tr[256], tg[256], tb[256];
        for (int c = 0; c < 256; c++) {
            fdk_f32 dc = (fdk_f32)c / 255.0f;
            fdk_f32 r = sr * a + dc * inv;
            fdk_f32 g = sg * a + dc * inv;
            fdk_f32 b = sb * a + dc * inv;
            if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
            if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
            if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
            tr[c] = (fdk_u32)(r * 255.0f + 0.5f) << 16;
            tg[c] = (fdk_u32)(g * 255.0f + 0.5f) << 8;
            tb[c] = (fdk_u32)(b * 255.0f + 0.5f);
        }
        for (int y = y0; y < y1; y++) {
            fdk_u32 *row = surface->fb.pixels +
                           (size_t)y * (size_t)surface->fb.stride;
            int x = x0;
            while (x < x1) {
                /* run of one destination word */
                fdk_u32 d = row[x];
                int run = x + 1;
                while (run < x1 && row[run] == d) {
                    run++;
                }
                if (run - x >= 4) {
                    fdk_u32 px = tr[(d >> 16) & 0xFFu] |
                                 tg[(d >> 8) & 0xFFu] |
                                 tb[d & 0xFFu];
                    fdk_u32 *dst = row + x;
                    size_t n = (size_t)(run - x);
                    size_t i = 0;
                    for (; i + 4 <= n; i += 4) {
                        dst[i] = px;
                        dst[i + 1] = px;
                        dst[i + 2] = px;
                        dst[i + 3] = px;
                    }
                    for (; i < n; i++) {
                        dst[i] = px;
                    }
                    x = run;
                } else {
                    for (; x < run; x++) {
                        fdk_u32 dd = row[x];
                        row[x] = tr[(dd >> 16) & 0xFFu] |
                                 tg[(dd >> 8) & 0xFFu] |
                                 tb[dd & 0xFFu];
                    }
                }
            }
        }
        return;
    }

    for (int y = y0; y < y1; y++) {
        fdk_u32 *row = surface->fb.pixels +
                       (size_t)y * (size_t)surface->fb.stride;
        int x = x0;
        /* 4-wide unroll: four INDEPENDENT float chains (no
         * horizontal mixing), which the compiler may vectorize —
         * the rounding of each chain is unchanged, so byte-exact
         * vs the scalar form (the exactness sweep verifies). */
        for (; x + 4 <= x1; x += 4) {
            fdk_u32 dst0 = row[x], dst1 = row[x + 1];
            fdk_u32 dst2 = row[x + 2], dst3 = row[x + 3];
            fdk_u32 out[4];
            fdk_u32 srcs[4] = { dst0, dst1, dst2, dst3 };
            for (int k = 0; k < 4; k++) {
                fdk_u32 dst = srcs[k];
                fdk_f32 da_r =
                    (fdk_f32)((dst >> 16) & 0xFFu) / 255.0f;
                fdk_f32 da_g =
                    (fdk_f32)((dst >> 8) & 0xFFu) / 255.0f;
                fdk_f32 da_b = (fdk_f32)(dst & 0xFFu) / 255.0f;
                fdk_f32 r, g, b, out_a;
                {
                    fdk_f32 da =
                        (fdk_f32)((dst >> 24) & 0xFFu) / 255.0f;
                    out_a = a + da * inv;
                    if (out_a <= 0.0f) {
                        r = g = b = 0.0f;
                    } else {
                        fdk_f32 w_s = a / out_a;
                        fdk_f32 w_d = da * inv / out_a;
                        r = sr * w_s + da_r * w_d;
                        g = sg * w_s + da_g * w_d;
                        b = sb * w_s + da_b * w_d;
                    }
                }
                if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
                if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
                if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
                fdk_u32 packed =
                    ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
                    ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
                    (fdk_u32)(b * 255.0f + 0.5f);
                {
                    if (out_a < 0.0f) out_a = 0.0f;
                    if (out_a > 1.0f) out_a = 1.0f;
                    packed |= (fdk_u32)(out_a * 255.0f + 0.5f) << 24;
                }
                out[k] = packed;
            }
            row[x] = out[0];
            row[x + 1] = out[1];
            row[x + 2] = out[2];
            row[x + 3] = out[3];
        }
        for (; x < x1; x++) {
            fdk_u32 dst = row[x];
            fdk_f32 da_r = (fdk_f32)((dst >> 16) & 0xFFu) / 255.0f;
            fdk_f32 da_g = (fdk_f32)((dst >> 8) & 0xFFu) / 255.0f;
            fdk_f32 da_b = (fdk_f32)(dst & 0xFFu) / 255.0f;
            fdk_f32 r, g, b, out_a;
            if (argb) {
                fdk_f32 da = (fdk_f32)((dst >> 24) & 0xFFu) / 255.0f;
                out_a = a + da * inv;
                if (out_a <= 0.0f) {
                    r = g = b = 0.0f;
                } else {
                    fdk_f32 w_s = a / out_a;
                    fdk_f32 w_d = da * inv / out_a;
                    r = sr * w_s + da_r * w_d;
                    g = sg * w_s + da_g * w_d;
                    b = sb * w_s + da_b * w_d;
                }
            } else {
                out_a = 1.0f;
                r = sr * a + da_r * inv;
                g = sg * a + da_g * inv;
                b = sb * a + da_b * inv;
            }
            if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
            if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
            if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
            fdk_u32 packed =
                ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
                ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
                (fdk_u32)(b * 255.0f + 0.5f);
            if (argb) {
                if (out_a < 0.0f) out_a = 0.0f;
                if (out_a > 1.0f) out_a = 1.0f;
                packed |= (fdk_u32)(out_a * 255.0f + 0.5f) << 24;
            }
            row[x] = packed;
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
    if (surface->gpu != NULL) {
        fdk__gpu_fill_rect(surface, rect, color);
        return;
    }
    if (surface->gpu_failed) {
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_and_damage(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }
    /* Opaque fast path (1.0.0): a fully-opaque fill replaces the
     * destination outright — one packed write per pixel, no float
     * blend math. This is THE hot path of the widget layer (every
     * control background, every track, every panel is an opaque
     * fill_rect); the v1 float-per-pixel loop cost ~15 ops/pixel
     * for what is a memcpy-shaped operation. A row-word memset via
     * a small unrolled loop keeps the code portable and obvious. */
    if (color.a >= 1.0f) {
        fdk_u32 px =
            (surface->format == FDK_SURFACE_FORMAT_ARGB8888)
                ? pack_opaque_argb(color)
                : pack_color(color);
        for (int y = y0; y < y1; y++) {
            fdk_u32 *row = surface->fb.pixels +
                           (size_t)y * (size_t)surface->fb.stride;
            size_t n = (size_t)(x1 - x0);
            fdk_u32 *dst = row + (size_t)x0;
            size_t i = 0;
            for (; i + 4 <= n; i += 4) {
                dst[i] = px;
                dst[i + 1] = px;
                dst[i + 2] = px;
                dst[i + 3] = px;
            }
            for (; i < n; i++) {
                dst[i] = px;
            }
        }
        return;
    }
    /* Round 17: the per-pixel blend_pixel call (branch + call
     * overhead + re-derived constants every pixel) is 53% of a
     * drag-highlight profile; the hoisted loop above is
     * byte-identical and several times faster. */
    fill_rect_translucent(surface, x0, y0, x1, y1, color);
}

void fdk_surface_draw_rect(fdk_surface *surface, fdk_rect rect,
                           fdk_color color) {
    if (surface == NULL || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }
    if (surface->gpu != NULL) {
        fdk__gpu_draw_rect(surface, rect, color, 1);
        return;
    }
    if (surface->gpu_failed) {
        return;
    }

    /* Degenerate heights: top and bottom edges COINCIDE — drawing
     * both would double-blend the same row (a visible artifact with
     * translucent colors, and exactly what the no-double-blend rule
     * below exists to prevent). A 1-px-high rect is one row; a 1-px
     * WIDE rect similarly has its two vertical edges on one column.
     */
    if (rect.height == 1) {
        fdk_surface_fill_rect(surface,
                              (fdk_rect){ .x = rect.x, .y = rect.y,
                                          .width = rect.width,
                                          .height = 1 },
                              color);
        return; /* the row IS the whole outline */
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
    if (rect.width == 1) {
        /* One column, strictly between the two rows already drawn. */
        fdk_surface_fill_rect(surface,
                              (fdk_rect){ .x = rect.x, .y = rect.y + 1,
                                          .width = 1,
                                          .height = rect.height - 2 },
                              color);
        return;
    }
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
    if (surface->gpu != NULL) {
        fdk__gpu_fill_gradient_vertical(surface, rect, top, bottom);
        return;
    }
    if (surface->gpu_failed) {
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
    if (surface->gpu != NULL) {
        fdk__gpu_draw_line(surface, (fdk_f32)x0, (fdk_f32)y0,
                            (fdk_f32)x1, (fdk_f32)y1, color, 1.0f, 0);
        return;
    }
    if (surface->gpu_failed) {
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
    if (surface->gpu != NULL) {
        fdk__gpu_draw_circle(surface, (fdk_f32)cx, (fdk_f32)cy,
                              (fdk_f32)radius, color, 1.0f, 0);
        return;
    }
    if (surface->gpu_failed) {
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
    if (surface->gpu != NULL) {
        fdk__gpu_fill_circle(surface, (fdk_f32)cx, (fdk_f32)cy,
                              (fdk_f32)radius, color, 0);
        return;
    }
    if (surface->gpu_failed) {
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
    if (surface->gpu != NULL) {
        fdk__gpu_fill_rounded_rect(surface, rect, corner_radius, color, 0);
        return;
    }
    if (surface->gpu_failed) {
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

    /* Opaque fast path (1.0.0): with a fully-opaque color the blend
     * is the identity — write the packed pixel directly (the same
     * optimization fill_rect got; buttons and every rounded control
     * background ride this path on every paint). */
    const bool opaque = (color.a >= 1.0f);
    fdk_u32 opaque_px = 0;
    if (opaque) {
        opaque_px = (surface->format == FDK_SURFACE_FORMAT_ARGB8888)
                        ? pack_opaque_argb(color)
                        : pack_color(color);
    }

    for (fdk_i32 y = rect.y; y < rect.y + rect.height; y++) {
        fdk_i32 half = r; /* middle rows: full width */
        if (y < cyt) {
            fdk_i32 dy = cyt - y;
            half = isqrt_round((fdk_i64)r * r - (fdk_i64)dy * dy);
        } else if (y > cyb) {
            fdk_i32 dy = y - cyb;
            half = isqrt_round((fdk_i64)r * r - (fdk_i64)dy * dy);
        }
        fdk_i32 sx = cxl - half;
        fdk_i32 ex = cxr + half;
        if (opaque) {
            fdk_u32 *row = surface->fb.pixels +
                           (size_t)y * (size_t)surface->fb.stride;
            for (fdk_i32 x = sx; x <= ex; x++) {
                if (x >= surface->clip_x0 && x < surface->clip_x1 &&
                    y >= surface->clip_y0 && y < surface->clip_y1) {
                    row[x] = opaque_px;
                }
            }
        } else {
            for (fdk_i32 x = sx; x <= ex; x++) {
                blend_pixel(surface, x, y, color);
            }
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
    if (surface->gpu != NULL) {
        fdk__gpu_draw_rounded_rect(surface, rect, corner_radius, color, 1, 0);
        return;
    }
    if (surface->gpu_failed) {
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

/* Alpha-mask blit — the glyph compositing primitive (internal).
 *
 * Blends a 1-byte-per-pixel coverage mask (row stride `mask_stride`
 * in BYTES) with a solid straight-alpha color, source-over, across
 * `rect` on the surface. `rect` is both the destination rectangle and
 * the mask's coordinate frame: mask pixel (0,0) lands at rect's
 * top-left. The mask is sampled relative to the clipped span, so a
 * partially clipped rect simply reads the matching sub-rectangle.
 *
 * Honors the clip stack (via the effective clip) exactly like every
 * other primitive, but deliberately does NOT record damage: callers
 * own damage bookkeeping so a text run can union its glyph ink boxes
 * into a single damage rectangle instead of one per glyph (see
 * src/text/text.c). The caller is responsible for invalidating the
 * union of the rects it actually blended.
 */
void fdk_surface_blend_mask(fdk_surface *surface, fdk_rect rect,
                            const fdk_u8 *mask, fdk_i32 mask_stride,
                            fdk_color color) {
    if (surface == NULL || mask == NULL || rect.width <= 0 ||
        rect.height <= 0 || mask_stride < rect.width) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }
    if (surface->gpu != NULL) {
        fdk__gpu_blend_mask(surface, rect, mask, mask_stride, color, 1.0f);
        return;
    }
    if (surface->gpu_failed) {
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_rect(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }

    /* Integer source-over with a rounded blend, mirroring blend_pixel's
     * float math at glyph speeds: precombine the color's alpha with
     * each mask byte, then lerp the three channels. Fully transparent
     * pixels skip the write entirely (the common case in sparse AA
     * bitmaps — glyph interiors are the only dense part). */
    fdk_f32 af = color.a < 0.0f ? 0.0f : (color.a > 1.0f ? 1.0f : color.a);
    fdk_f32 rf = color.r < 0.0f ? 0.0f : (color.r > 1.0f ? 1.0f : color.r);
    fdk_f32 gf = color.g < 0.0f ? 0.0f : (color.g > 1.0f ? 1.0f : color.g);
    fdk_f32 bf = color.b < 0.0f ? 0.0f : (color.b > 1.0f ? 1.0f : color.b);
    int ca = (int)(af * 255.0f + 0.5f);
    int sr = (int)(rf * 255.0f + 0.5f);
    int sg = (int)(gf * 255.0f + 0.5f);
    int sb = (int)(bf * 255.0f + 0.5f);

    for (int y = y0; y < y1; y++) {
        const fdk_u8 *mrow =
            mask + (size_t)(y - rect.y) * (size_t)mask_stride +
            (size_t)(x0 - rect.x);
        fdk_u32 *drow = surface->fb.pixels +
                        (size_t)y * (size_t)surface->fb.stride +
                        (size_t)x0;
        int n = x1 - x0;
        for (int i = 0; i < n; i++) {
            int am = (ca * mrow[i] + 127) / 255;
            if (am <= 0) {
                continue;
            }
            if (surface->format == FDK_SURFACE_FORMAT_ARGB8888) {
                /* Full straight source-over including the alpha
                 * channel (see blend_pixel's ARGB branch). Text into
                 * transparent sprites is not the hot path — the
                 * float form keeps the math provably identical to
                 * blend_pixel instead of a second integer rounding.
                 */
                fdk_color cov_color = { .r = rf, .g = gf, .b = bf,
                                        .a = af * (fdk_f32)mrow[i] / 255.0f };
                blend_pixel(surface, x0 + i, y, cov_color);
                continue;
            }
            fdk_u32 d = drow[i];
            int dr = (int)((d >> 16) & 0xFFu);
            int dg = (int)((d >> 8) & 0xFFu);
            int db = (int)d & 0xFF;
            int r = (sr * am + dr * (255 - am) + 127) / 255;
            int g = (sg * am + dg * (255 - am) + 127) / 255;
            int b = (sb * am + db * (255 - am) + 127) / 255;
            drow[i] = ((fdk_u32)r << 16) | ((fdk_u32)g << 8) | (fdk_u32)b;
        }
    }
}

/* RGBA blit — the color-glyph compositing primitive (C-3). See
 * surface_internal.h's contract comment. Straight source-over with
 * the same integer rounding discipline blend_mask's ARGB branch
 * uses; fully transparent pixels skip the write (the sparse case in
 * emoji bitmaps — the padding around the drawn image). */
void fdk_surface_blend_rgba(fdk_surface *surface, fdk_rect rect,
                            const fdk_u8 *rgba, fdk_i32 rgba_stride) {
    if (surface == NULL || rgba == NULL || rect.width <= 0 ||
        rect.height <= 0 || rgba_stride < rect.width * 4) {
        return;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return;
    }
    if (surface->gpu != NULL) {
        fdk__gpu_blend_rgba(surface, rect, rgba, rgba_stride, 1.0f);
        return;
    }
    if (surface->gpu_failed) {
        return;
    }

    int x0, y0, x1, y1;
    if (!clip_rect(surface, rect, &x0, &y0, &x1, &y1)) {
        return;
    }

    bool argb = surface->format == FDK_SURFACE_FORMAT_ARGB8888;
    for (int y = y0; y < y1; y++) {
        const fdk_u8 *srow =
            rgba + (size_t)(y - rect.y) * (size_t)rgba_stride +
            (size_t)(x0 - rect.x) * 4u;
        fdk_u32 *drow = surface->fb.pixels +
                        (size_t)y * (size_t)surface->fb.stride +
                        (size_t)x0;
        int n = x1 - x0;
        for (int i = 0; i < n; i++) {
            const fdk_u8 *s = srow + (size_t)i * 4u;
            fdk_u32 sa = s[3];
            if (sa == 0) {
                continue;
            }
            if (sa == 255 && argb) {
                drow[i] = ((fdk_u32)sa << 24) | ((fdk_u32)s[0] << 16) |
                          ((fdk_u32)s[1] << 8) | (fdk_u32)s[2];
                continue;
            }
            fdk_u32 d = drow[i];
            if (argb) {
                fdk_u32 da = (d >> 24) & 0xFFu;
                fdk_u32 out_a = sa + da * (255u - sa) / 255u;
                if (out_a == 0) {
                    drow[i] = 0;
                    continue;
                }
                fdk_u32 dr = (d >> 16) & 0xFFu;
                fdk_u32 dg = (d >> 8) & 0xFFu;
                fdk_u32 db = d & 0xFFu;
                fdk_u32 r = ((fdk_u32)s[0] * sa +
                             dr * da * (255u - sa) / 255u) /
                            out_a;
                fdk_u32 g = ((fdk_u32)s[1] * sa +
                             dg * da * (255u - sa) / 255u) /
                            out_a;
                fdk_u32 b = ((fdk_u32)s[2] * sa +
                             db * da * (255u - sa) / 255u) /
                            out_a;
                drow[i] = (out_a << 24) | (r << 16) | (g << 8) | b;
            } else {
                /* XRGB: opaque destination — blend channels, keep
                 * the alpha byte opaque (the blend_mask posture). */
                fdk_u32 dr = (d >> 16) & 0xFFu;
                fdk_u32 dg = (d >> 8) & 0xFFu;
                fdk_u32 db = d & 0xFFu;
                fdk_u32 r = ((fdk_u32)s[0] * sa +
                             dr * (255u - sa) + 127u) / 255u;
                fdk_u32 g = ((fdk_u32)s[1] * sa +
                             dg * (255u - sa) + 127u) / 255u;
                fdk_u32 b = ((fdk_u32)s[2] * sa +
                             db * (255u - sa) + 127u) / 255u;
                drow[i] = ((fdk_u32)r << 16) | ((fdk_u32)g << 8) |
                          (fdk_u32)b;
            }
        }
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
    if (dst->gpu != NULL) {
        fdk_rect dr = { dst_x, dst_y, src_rect.width, src_rect.height };
        fdk__gpu_blit(dst, dr, src, src_rect);
        return FDK_OK;
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

    /* Opaque row copies (documented: blit is an opaque pixel copy).
     * An XRGB source onto an ARGB destination gets alpha forced to
     * opaque — the source's "ignored" top byte must not leak a fake
     * transparency into a compositing surface. All other format
     * combinations copy verbatim (an ARGB source's alpha byte is
     * either meaningful, or ignored by an XRGB destination). */
    size_t copy_len = (size_t)(dx1 - dx0);
    int force_opaque =
        (dst->format == FDK_SURFACE_FORMAT_ARGB8888 &&
         src->format != FDK_SURFACE_FORMAT_ARGB8888);
    for (long long row = 0; row < dy1 - dy0; row++) {
        fdk_u32 *dst_row =
            dst->fb.pixels +
            (size_t)(dy0 + row) * (size_t)dst->fb.stride + (size_t)dx0;
        const fdk_u32 *src_row =
            src->fb.pixels +
            (size_t)(sy0 + row) * (size_t)src->fb.stride + (size_t)sx0;
        memcpy(dst_row, src_row, copy_len * sizeof(fdk_u32));
        if (force_opaque) {
            for (size_t i = 0; i < copy_len; i++) {
                dst_row[i] |= 0xFF000000u;
            }
        }
    }

    damage_add(dst, (fdk_rect){ .x = (fdk_i32)dx0, .y = (fdk_i32)dy0,
                                .width = (fdk_i32)(dx1 - dx0),
                                .height = (fdk_i32)(dy1 - dy0) });
    return FDK_OK;
}

/* Round 14 (the scroll blit): same-surface opaque region copy. This
 * is the preserved-pixel mover behind delta-strip scroll damage — the
 * scrollview shifts its already-painted rows within one framebuffer
 * and only the exposed strip needs repainting. Two contracts differ
 * from fdk_surface_blit:
 *   - source and destination are the SAME surface, so the copy is
 *     done through a single memmove-safe walk: upward shifts walk
 *     top-to-bottom, downward shifts bottom-to-top, so no row is
 *     read after being overwritten (a plain memcpy loop would smear
 *     for overlapping shifts);
 *   - the DESTINATION region IS damage-tracked, and must be: those
 *     pixels are new content AT THOSE COORDINATES on the wire. The
 *     X11 present's slot sync brings the next back slot up to date
 *     by copying exactly the PRESENTED rects — an un-tracked blit
 *     leaves the moved rows off the wire (the server's window keeps
 *     the pre-scroll arrangement outside the strips) and stale in
 *     the next back slot (the following blit reads two-frames-old
 *     rows). Found live by the blit-check's pixel diff (36k
 *     subpixels, list area; the FDK_SCROLL_NOBLIT control passed).
 *     This is SURFACE damage (wire bytes), a different ledger from
 *     the TREE damage the scrollview sets (repaint-walk bounds =
 *     strips only): the paint cost stays strip-limited while the
 *     wire cost honestly includes the moved band. The clip stack is
 *     honored on read and write (the scrollview blits outside its
 *     own paint, so the stack is empty in practice — kept for
 *     safety). */
fdk_result fdk__surface_self_blit(fdk_surface *s, fdk_i32 dst_x,
                                  fdk_i32 dst_y, fdk_i32 src_x,
                                  fdk_i32 src_y, fdk_i32 w, fdk_i32 h) {
    if (s == NULL || w <= 0 || h <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_result r = surface_acquire(s);
    if (!fdk_ok(r)) {
        return r;
    }

    /* Clip the source rect to the framebuffer first. */
    long long sx0 = src_x, sy0 = src_y;
    long long sx1 = (long long)src_x + w;
    long long sy1 = (long long)src_y + h;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > s->fb.width) sx1 = s->fb.width;
    if (sy1 > s->fb.height) sy1 = s->fb.height;
    if (sx0 >= sx1 || sy0 >= sy1) {
        return FDK_OK; /* source entirely outside */
    }

    /* Map onto the destination, clip to the effective clip. The
     * source-clip trims above do NOT trim the destination (the pair
     * keeps its size; a partially outside source just produces a
     * correspondingly smaller copy, same as fdk_surface_blit). */
    long long dx0 = (long long)dst_x + (sx0 - src_x);
    long long dy0 = (long long)dst_y + (sy0 - src_y);
    long long dx1 = dx0 + (sx1 - sx0);
    long long dy1 = dy0 + (sy1 - sy0);
    if (dx1 <= s->clip_x0 || dy1 <= s->clip_y0 ||
        dx0 >= s->clip_x1 || dy0 >= s->clip_y1) {
        return FDK_OK;
    }
    if (dx0 < s->clip_x0) {
        sx0 += s->clip_x0 - dx0;
        dx0 = s->clip_x0;
    }
    if (dy0 < s->clip_y0) {
        sy0 += s->clip_y0 - dy0;
        dy0 = s->clip_y0;
    }
    if (dx1 > s->clip_x1) dx1 = s->clip_x1;
    if (dy1 > s->clip_y1) dy1 = s->clip_y1;
    if (dx0 >= dx1 || dy0 >= dy1) {
        return FDK_OK;
    }

    long long rows = dy1 - dy0;
    size_t copy_len = (size_t)(dx1 - dx0);
    /* Overlap-safe direction: shift content up (dy0 < sy0) must walk
     * from the top; shift down walks from the bottom. Equal rows
     * (dx0 == sx0) can still overlap horizontally — the row-wise
     * walk with memmove covers every case. */
    for (long long i = 0; i < rows; i++) {
        long long row = (dy0 < sy0) ? i : (rows - 1 - i);
        fdk_u32 *dst_row =
            s->fb.pixels + (size_t)(dy0 + row) * (size_t)s->fb.stride +
            (size_t)dx0;
        const fdk_u32 *src_row =
            s->fb.pixels + (size_t)(sy0 + row) * (size_t)s->fb.stride +
            (size_t)sx0;
        memmove(dst_row, src_row, copy_len * sizeof(fdk_u32));
    }
    /* The moved band goes on the wire (see the header comment: the
     * X11 slot sync and the Wayland buffer damage both key off the
     * presented rect list). */
    damage_add(s, (fdk_rect){ .x = (fdk_i32)dx0, .y = (fdk_i32)dy0,
                             .width = (fdk_i32)(dx1 - dx0),
                             .height = (fdk_i32)(dy1 - dy0) });
    return FDK_OK;
}
fdk_result fdk__surface_blit_alpha(fdk_surface *dst, fdk_i32 dst_x,
                                   fdk_i32 dst_y, fdk_surface *src,
                                   fdk_f32 alpha) {
    if (dst == NULL || src == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (src->format != FDK_SURFACE_FORMAT_ARGB8888) {
        /* The group atom composites a rendered group (ARGB offscreen);
         * an XRGB source has no per-pixel alpha to scale. */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!(alpha > 0.0f)) {
        return FDK_OK; /* fully transparent: nothing to write */
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }

    fdk_result r = surface_acquire(dst);
    if (!fdk_ok(r)) {
        return r;
    }
    r = surface_acquire(src);
    if (!fdk_ok(r)) {
        return r;
    }

    /* Two-sided clip (source bounds, destination effective clip) —
     * the same discipline as blit_blend, whole-surface source. */
    long long sx0 = 0;
    long long sy0 = 0;
    long long sx1 = src->fb.width;
    long long sy1 = src->fb.height;
    long long dx0 = dst_x;
    long long dy0 = dst_y;
    long long dx1 = dx0 + sx1;
    long long dy1 = dy0 + sy1;

    if (dx1 <= dst->clip_x0 || dy1 <= dst->clip_y0 ||
        dx0 >= dst->clip_x1 || dy0 >= dst->clip_y1) {
        return FDK_OK;
    }
    if (dx0 < dst->clip_x0) {
        sx0 += dst->clip_x0 - dx0;
        dx0 = dst->clip_x0;
    }
    if (dy0 < dst->clip_y0) {
        sy0 += dst->clip_y0 - dy0;
        dy0 = dst->clip_y0;
    }
    if (dx1 > dst->clip_x1) {
        dx1 = dst->clip_x1;
    }
    if (dy1 > dst->clip_y1) {
        dy1 = dst->clip_y1;
    }
    if (dx0 >= dx1 || dy0 >= dy1) {
        return FDK_OK;
    }

    int dst_argb = (dst->format == FDK_SURFACE_FORMAT_ARGB8888);
    /* alpha scaled to 0..255 with the same rounding every per-pixel
     * blend in this file applies. */
    int ga = (int)(alpha * 255.0f + 0.5f);
    if (ga > 255) {
        ga = 255;
    }

    for (long long row = 0; row < dy1 - dy0; row++) {
        const fdk_u32 *srow =
            src->fb.pixels +
            (size_t)(sy0 + row) * (size_t)src->fb.stride + (size_t)sx0;
        fdk_u32 *drow =
            dst->fb.pixels +
            (size_t)(dy0 + row) * (size_t)dst->fb.stride + (size_t)dx0;
        long long n = dx1 - dx0;
        for (long long i = 0; i < n; i++) {
            fdk_u32 s = srow[i];
            int sa = (int)((s >> 24) & 0xFFu);
            if (sa <= 0) {
                continue;
            }
            sa = (sa * ga + 127) / 255;
            if (sa <= 0) {
                continue;
            }
            fdk_u32 d = drow[i];
            int sr = (int)((s >> 16) & 0xFFu);
            int sg = (int)((s >> 8) & 0xFFu);
            int sb = (int)s & 0xFF;
            int dr = (int)((d >> 16) & 0xFFu);
            int dg = (int)((d >> 8) & 0xFFu);
            int db = (int)d & 0xFF;
            int inv = 255 - sa;
            fdk_u32 out;
            if (dst_argb) {
                int da = (int)((d >> 24) & 0xFFu);
                int oa = sa + (da * inv + 127) / 255;
                if (oa <= 0) {
                    continue;
                }
                int rr = (sr * sa * 255 + dr * da * inv) / (255 * oa);
                int gg = (sg * sa * 255 + dg * da * inv) / (255 * oa);
                int bb = (sb * sa * 255 + db * da * inv) / (255 * oa);
                out = ((fdk_u32)oa << 24) | ((fdk_u32)rr << 16) |
                      ((fdk_u32)gg << 8) | (fdk_u32)bb;
            } else {
                int rr = (sr * sa + dr * inv + 127) / 255;
                int gg = (sg * sa + dg * inv + 127) / 255;
                int bb = (sb * sa + db * inv + 127) / 255;
                out = ((fdk_u32)rr << 16) | ((fdk_u32)gg << 8) |
                      (fdk_u32)bb;
            }
            drow[i] = out;
        }
    }

    fdk__surface_damage_add(dst, (fdk_rect){(fdk_i32)dx0, (fdk_i32)dy0,
                                            (fdk_i32)(dx1 - dx0),
                                            (fdk_i32)(dy1 - dy0)});
    return FDK_OK;
}

fdk_result fdk_surface_blit_blend(fdk_surface *dst, fdk_i32 dst_x,
                                  fdk_i32 dst_y, fdk_surface *src,
                                  fdk_rect src_rect) {
    if (dst == NULL || src == NULL || src_rect.width <= 0 ||
        src_rect.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (src->format != FDK_SURFACE_FORMAT_ARGB8888) {
        /* Per-pixel alpha lives in the source's alpha channel — an
         * XRGB source has none, so the request is refused (apps that
         * want an opaque copy have fdk_surface_blit). */
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_result r = surface_acquire(dst);
    if (!fdk_ok(r)) {
        return r;
    }
    r = surface_acquire(src);
    if (!fdk_ok(r)) {
        return r;
    }
    if (dst->gpu != NULL) {
        fdk_rect dr = { dst_x, dst_y, src_rect.width, src_rect.height };
        fdk__gpu_blit(dst, dr, src, src_rect);
        return FDK_OK;
    }

    /* Same two-sided clipping as fdk_surface_blit (source bounds,
     * then destination effective clip). */
    long long sx0 = src_rect.x;
    long long sy0 = src_rect.y;
    long long sx1 = (long long)src_rect.x + src_rect.width;
    long long sy1 = (long long)src_rect.y + src_rect.height;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > src->fb.width) sx1 = src->fb.width;
    if (sy1 > src->fb.height) sy1 = src->fb.height;
    if (sx0 >= sx1 || sy0 >= sy1) {
        return FDK_OK;
    }

    long long dx0 = (long long)dst_x + (sx0 - src_rect.x);
    long long dy0 = (long long)dst_y + (sy0 - src_rect.y);
    long long dx1 = dx0 + (sx1 - sx0);
    long long dy1 = dy0 + (sy1 - sy0);

    if (dx1 <= dst->clip_x0 || dy1 <= dst->clip_y0 ||
        dx0 >= dst->clip_x1 || dy0 >= dst->clip_y1) {
        return FDK_OK;
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

    int dst_argb = (dst->format == FDK_SURFACE_FORMAT_ARGB8888);

    /* Integer source-over, one rounded blend per channel — the same
     * rounding discipline as fdk_surface_blend_mask. Fully transparent
     * source pixels skip the write (the common case for sprites with
     * padded bounds). */
    for (long long row = 0; row < dy1 - dy0; row++) {
        const fdk_u32 *srow =
            src->fb.pixels +
            (size_t)(sy0 + row) * (size_t)src->fb.stride + (size_t)sx0;
        fdk_u32 *drow =
            dst->fb.pixels +
            (size_t)(dy0 + row) * (size_t)dst->fb.stride + (size_t)dx0;
        long long n = dx1 - dx0;
        for (long long i = 0; i < n; i++) {
            fdk_u32 s = srow[i];
            int sa = (int)((s >> 24) & 0xFFu);
            if (sa <= 0) {
                continue;
            }
            fdk_u32 d = drow[i];
            int sr = (int)((s >> 16) & 0xFFu);
            int sg = (int)((s >> 8) & 0xFFu);
            int sb = (int)s & 0xFF;
            int dr = (int)((d >> 16) & 0xFFu);
            int dg = (int)((d >> 8) & 0xFFu);
            int db = (int)d & 0xFF;
            int inv = 255 - sa;
            fdk_u32 out;
            if (dst_argb) {
                int da = (int)((d >> 24) & 0xFFu);
                int oa = sa + (da * inv + 127) / 255;
                if (oa <= 0) {
                    continue;
                }
                /* out_rgb = (src_rgb*sa + dst_rgb*da*inv/255) / oa,
                 * rearranged into one integer expression. */
                int rr = (sr * sa * 255 + dr * da * inv) / (255 * oa);
                int gg = (sg * sa * 255 + dg * da * inv) / (255 * oa);
                int bb = (sb * sa * 255 + db * da * inv) / (255 * oa);
                out = ((fdk_u32)oa << 24) | ((fdk_u32)rr << 16) |
                      ((fdk_u32)gg << 8) | (fdk_u32)bb;
            } else {
                int rr = (sr * sa + dr * inv + 127) / 255;
                int gg = (sg * sa + dg * inv + 127) / 255;
                int bb = (sb * sa + db * inv + 127) / 255;
                out = ((fdk_u32)rr << 16) | ((fdk_u32)gg << 8) | (fdk_u32)bb;
            }
            drow[i] = out;
        }
    }

    damage_add(dst, (fdk_rect){ .x = (fdk_i32)dx0, .y = (fdk_i32)dy0,
                                .width = (fdk_i32)(dx1 - dx0),
                                .height = (fdk_i32)(dy1 - dy0) });
    return FDK_OK;
}

/* ---- fdk_texture: the explicit software-pixel upload path ----
 *
 * The documented escape hatch for software-pixel widgets in GPU mode:
 * pixels live app-side (RGBA8888), upload happens at draw time in the
 * window's lane context, and the SAME call composites on the CPU path
 * (blend_rgba), so widget paint code stays mode-agnostic. See the
 * declarations in include/fdk/fdk_surface.h. */

/* struct fdk_texture lives in surface_internal.h (the dmabuf
 * module's fallback path touches the payload directly). */

fdk_result fdk_texture_create(fdk_i32 width, fdk_i32 height,
                              fdk_texture **out_texture) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        out_texture == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_texture *t = fdk_alloc(sizeof(*t));
    if (t == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    t->length = (size_t)width * (size_t)height * 4u;
    t->pixels = fdk_alloc(t->length);
    if (t->pixels == NULL) {
        fdk_free(t);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(t->pixels, 0, t->length);
    t->width = width;
    t->height = height;
    *out_texture = t;
    return FDK_OK;
}

fdk_result fdk_texture_update(fdk_texture *texture, fdk_rect rect,
                              const void *pixels, fdk_i32 stride_bytes) {
    if (texture == NULL || pixels == NULL || rect.width <= 0 ||
        rect.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (texture->is_dmabuf) {
        /* the buffer is the dmabuf's, not ours: update would race
         * the zero-copy lane's image and corrupt the fallback */
        return FDK_ERR_INVALID_STATE;
    }
    if (rect.x < 0 || rect.y < 0 ||
        rect.x + rect.width > texture->width ||
        rect.y + rect.height > texture->height ||
        stride_bytes < rect.width * 4) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_u8 *src = pixels;
    for (int row = 0; row < rect.height; row++) {
        memcpy(texture->pixels +
                   ((size_t)(rect.y + row) * (size_t)texture->width +
                    (size_t)rect.x) * 4u,
               src + (size_t)row * (size_t)stride_bytes,
               (size_t)rect.width * 4u);
    }
    return FDK_OK;
}

fdk_result fdk_texture_draw(fdk_surface *surface, fdk_texture *texture,
                            fdk_rect dst) {
    return fdk_texture_draw_ex(surface, texture, dst, FDK_FILTER_NEAREST);
}

/* CPU bilinear sampler: the same 4-tap, edge-clamped, straight-alpha
 * interpolation GL_LINEAR performs (channels interpolated
 * independently — the GPU does the same on straight-alpha texels, so
 * the two samplers agree to rounding). */
static void texture_sample_linear(const fdk_texture *t, float fx,
                                  float fy, fdk_u8 *out) {
    if (fx < 0.0f) {
        fx = 0.0f;
    }
    if (fy < 0.0f) {
        fy = 0.0f;
    }
    if (fx > (float)t->width - 1.0f) {
        fx = (float)t->width - 1.0f;
    }
    if (fy > (float)t->height - 1.0f) {
        fy = (float)t->height - 1.0f;
    }
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < t->width ? x0 + 1 : t->width - 1;
    int y1 = y0 + 1 < t->height ? y0 + 1 : t->height - 1;
    float ax = fx - (float)x0, ay = fy - (float)y0;
    const fdk_u8 *p00 = t->pixels + ((size_t)y0 * t->width + x0) * 4u;
    const fdk_u8 *p10 = t->pixels + ((size_t)y0 * t->width + x1) * 4u;
    const fdk_u8 *p01 = t->pixels + ((size_t)y1 * t->width + x0) * 4u;
    const fdk_u8 *p11 = t->pixels + ((size_t)y1 * t->width + x1) * 4u;
    for (int ch = 0; ch < 4; ch++) {
        float top = (float)p00[ch] * (1.0f - ax) + (float)p10[ch] * ax;
        float bot = (float)p01[ch] * (1.0f - ax) + (float)p11[ch] * ax;
        float v = top * (1.0f - ay) + bot * ay + 0.5f;
        out[ch] = (fdk_u8)(v > 255.0f ? 255.0f : v);
    }
}

fdk_result fdk_texture_draw_ex(fdk_surface *surface, fdk_texture *texture,
                               fdk_rect dst, fdk_texture_filter filter) {
    if (surface == NULL || texture == NULL || dst.width <= 0 ||
        dst.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (filter != FDK_FILTER_NEAREST && filter != FDK_FILTER_LINEAR) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (texture->is_dmabuf) {
        /* the dmabuf import: zero-copy when the lane's EGL bridge
         * can serve it; otherwise the EXPLICIT CPU fallback (linear
         * only — INVALID/tiled answer UNSUPPORTED here, never a
         * linear mmap guess) */
        if (surface->gpu != NULL && !surface->gpu_failed) {
            if (fdk__gpu_dmabuf_name(surface, texture) != 0) {
                fdk__gpu_draw_texture(surface, texture, dst, 0.0f, 0.0f,
                                      1.0f, 1.0f, 1.0f, (int)filter);
                return FDK_OK;
            }
        }
        fdk_result fr = fdk__dmabuf_fallback_realize(texture,
                                                    &texture->dmabuf);
        if (!fdk_ok(fr)) {
            /* the payload flag STAYS: the texture keeps refusing
             * (INVALID/tiled never degrade to empty-pixel draws) */
            return FDK_ERR_UNSUPPORTED;
        }
        texture->is_dmabuf = 0; /* the pixels own it now */
    }
    if (surface->gpu != NULL || surface->gpu_failed) {
        if (surface->gpu_failed) {
            return FDK_ERR_UNSUPPORTED;
        }
        /* GPU path: book the pixels (the flush drain realizes +
         * uploads + draws in the lane context). */
        fdk__gpu_texture_name(surface, texture, texture->width,
                              texture->height, 1, texture->pixels,
                              texture->width * 4, NULL);
        fdk__gpu_draw_texture(surface, texture, dst, 0.0f, 0.0f, 1.0f,
                              1.0f, 1.0f, (int)filter);
        return FDK_OK;
    }
    if (!fdk_ok(surface_acquire(surface))) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* CPU path: nearest-neighbor scale from the RGBA texture with
     * straight-alpha source-over (blend_rgba is 1:1 — its stride
     * sanity check rejects a scaled destination rect). */
    int x0 = surface->clip_x0 > dst.x ? surface->clip_x0 : dst.x;
    int y0 = surface->clip_y0 > dst.y ? surface->clip_y0 : dst.y;
    int x1 = surface->clip_x1 < dst.x + dst.width ? surface->clip_x1
                                                  : dst.x + dst.width;
    int y1 = surface->clip_y1 < dst.y + dst.height ? surface->clip_y1
                                                   : dst.y + dst.height;
    if (x0 >= x1 || y0 >= y1) {
        return FDK_OK;
    }
    bool argb = surface->format == FDK_SURFACE_FORMAT_ARGB8888;
    const int linear = filter == FDK_FILTER_LINEAR;
    for (int y = y0; y < y1; y++) {
        int sy = 0;
        const fdk_u8 *srow = NULL;
        fdk_u8 lin[4];
        if (!linear) {
            sy = (int)((float)(y - dst.y) * (float)texture->height /
                       (float)dst.height);
            if (sy >= texture->height) {
                sy = texture->height - 1;
            }
            srow = texture->pixels +
                   (size_t)sy * (size_t)texture->width * 4u;
        }
        fdk_u32 *drow = surface->fb.pixels +
                        (size_t)y * (size_t)surface->fb.stride +
                        (size_t)x0;
        for (int x = x0; x < x1; x++) {
            const fdk_u8 *t;
            if (linear) {
                texture_sample_linear(
                    texture,
                    ((float)x + 0.5f) * (float)texture->width /
                            (float)dst.width -
                        0.5f,
                    ((float)y + 0.5f) * (float)texture->height /
                            (float)dst.height -
                        0.5f,
                    lin);
                t = lin;
            } else {
                int sx = (int)((float)(x - dst.x) * (float)texture->width /
                               (float)dst.width);
                if (sx >= texture->width) {
                    sx = texture->width - 1;
                }
                t = srow + (size_t)sx * 4u;
            }
            fdk_u32 sa = t[3];
            if (sa == 0) {
                continue;
            }
            if (sa == 255 && argb) {
                drow[x - x0] = ((fdk_u32)sa << 24) |
                               ((fdk_u32)t[0] << 16) |
                               ((fdk_u32)t[1] << 8) | (fdk_u32)t[2];
                continue;
            }
            float a = (float)sa / 255.0f;
            fdk_u32 dp = drow[x - x0];
            float dr = (float)((dp >> 16) & 0xFFu);
            float dg = (float)((dp >> 8) & 0xFFu);
            float db = (float)(dp & 0xFFu);
            float sr = (float)t[0];
            float sg = (float)t[1];
            float sb = (float)t[2];
            float orr = sr * a + dr * (1.0f - a);
            float og = sg * a + dg * (1.0f - a);
            float ob = sb * a + db * (1.0f - a);
            fdk_u32 oa = 0xFF000000u;
            if (argb) {
                float da = (float)((dp >> 24) & 0xFFu);
                oa = (fdk_u32)(unsigned char)(sa + da * (1.0f - a) + 0.5f)
                     << 24;
            }
            drow[x - x0] = oa |
                           ((fdk_u32)((unsigned char)(orr + 0.5f)) << 16) |
                           ((fdk_u32)((unsigned char)(og + 0.5f)) << 8) |
                           (fdk_u32)(unsigned char)(ob + 0.5f);
        }
    }
    fdk__surface_damage_add(surface, (fdk_rect){ .x = x0, .y = y0,
                                                 .width = x1 - x0,
                                                 .height = y1 - y0 });
    return FDK_OK;
}

void fdk_texture_destroy(fdk_texture *texture) {
    if (texture == NULL) {
        return;
    }
    /* GL-side registry entries die with their window engine (bounded
     * table); the pixel memory is app-owned and freed here. A dmabuf
     * texture closes its own dup'd fds. */
    if (texture->is_dmabuf) {
        fdk__dmabuf_texture_close_fds(&texture->dmabuf);
    }
    fdk_free(texture->pixels);
    fdk_free(texture);
}
