/* vk_present.h — the Vulkan present lane (the GL lanes' third
 * sibling; 2026-11-11).
 *
 * Same contract as the GLX/EGL present lanes, same words: CPU
 * rasterization unchanged, the lane replaces only present()'s
 * "pixels -> compositor" half. The staging buffer's XRGB8888 bytes
 * ride a host-visible Vulkan buffer into a swapchain image via a
 * queue transfer + blit; FIFO present through the platform's
 * VK_KHR_xlib_surface / VK_KHR_wayland_surface.
 *
 * Not part of the public API — never installed. Internal seam for
 * the X11 and Wayland backends' present dispatch.
 *
 * Zero new build dependencies (the lane posture): the loader is
 * dlopen'd (libvulkan.so.1), every entry point resolved by name,
 * every structure and constant hand-declared from the spec's ABI
 * (verified against Khronos' vulkan_core.h). A stripped box with no
 * Vulkan simply never loads this lane — the GL lanes and the CPU
 * path answer instead.
 */

#ifndef FDK_VK_PRESENT_H
#define FDK_VK_PRESENT_H

#include "fdk/fdk_core.h"
#include "../platform/platform_internal.h"

/* ---- the backend's view ------------------------------------------------
 *
 * The lane object is opaque to both backends; they hold the pointer,
 * feed it staging pixels, and ask it to present. */

struct fdk_vk_lane;

/* One process-wide loader attempt, exactly like the GL lanes'
 * try_load discipline: NULL = no Vulkan this process (missing
 * library, incompatible driver, or the software-ICD policy refusing
 * lavapipe/SwiftShader without FDK_GL_FORCE). `platform` selects the
 * surface extension: "x11" (dpy + xid) or "wayland" (wl_display +
 * wl_surface). The returned lane owns its instance; one lane per
 * connection. */
struct fdk_vk_lane *fdk_vk_lane_try_load(fdk_platform_connection *conn,
                                         const char *platform);

/* The per-window swapchain wrapper (the GL lanes' gl_win/egl_win).
 * `native` points at the platform window: X11 -> (Display*, Window);
 * Wayland -> (struct wl_display*, struct wl_surface*). */
struct fdk_vk_window;

struct fdk_vk_window *fdk_vk_window_try_init(struct fdk_vk_lane *lane,
                                             void *native_a, void *native_b,
                                             fdk_i32 w, fdk_i32 h);

/* The staging buffer the CPU rasterizer paints into — the same
 * contract as fdk_x11_gl_window_staging / fdk_wayland_egl_window_
 * staging: pixels owned by the lane, valid until the next
 * staging() call at a different size. NULL = OOM (the caller's
 * existing fallbacks own that verdict). */
fdk_u32 *fdk_vk_window_staging(struct fdk_vk_window *vwin, fdk_i32 w,
                              fdk_i32 h);

/* Present: upload the damaged rects from the staging buffer into the
 * acquired swapchain image, blit into present layout, queue present.
 * Returns 0 on success, -1 on a lane-verdict failure (the caller's
 * downgrade path owns the salvage — same contract as the GL lanes). */
int fdk_vk_window_present(struct fdk_vk_window *vwin,
                          const fdk_platform_damage *damage, fdk_i32 w,
                          fdk_i32 h);

/* Query: 1 while this window presents through the Vulkan lane (the
 * window_render_path op's Vulkan answer). */
int fdk_vk_window_render_path(const struct fdk_vk_window *vwin);

/* The device string for the backend query (lavapipe, NVIDIA GA10x,
 * ...); "<unknown>" before the first engagement. Never NULL. */
const char *fdk_vk_lane_device(const struct fdk_vk_lane *lane);

/* Teardown. destroy_window first, then destroy (the GL lanes' order). */
void fdk_vk_window_destroy(struct fdk_vk_lane *lane,
                           struct fdk_vk_window *vwin);
void fdk_vk_lane_destroy(struct fdk_vk_lane *lane);

#endif /* FDK_VK_PRESENT_H */
