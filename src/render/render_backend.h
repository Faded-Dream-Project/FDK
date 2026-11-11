/* render_backend.h — the GPU backend priority + forcing policy
 * (2026-11-11), shared by the X11 and Wayland lane-acquisition
 * paths.
 *
 * The owner's contract, stated once:
 *   priority  Vulkan first, OpenGL second, CPU last;
 *   forcing   FDK_GPU_BACKEND=vulkan|opengl|auto (canonical) and
 *             FDK_GL_VULKAN=1 / FDK_GL_OPENGL=1 (aliases);
 *   AUTO      the priority order above (Vulkan -> OpenGL -> CPU).
 *
 * Forcing semantics (a FORCED backend is a REQUIREMENT, the same
 * contract FDK_RENDER_GPU already lives under):
 *   FDK_GPU_BACKEND=vulkan (or FDK_GL_VULKAN=1)
 *     only the Vulkan lane may present. If it cannot engage, the
 *     window takes the CPU path — and fdk_window_render_backend()
 *     reports CPU honestly, so a strict app (gpu_strict) can refuse
 *     to run. NO silent substitution of OpenGL.
 *   FDK_GPU_BACKEND=opengl (or FDK_GL_OPENGL=1)
 *     only the GL lanes (GLX on X11, EGL on Wayland). Vulkan never
 *     loads.
 *   unset / auto
 *     Vulkan first, then OpenGL, then CPU.
 *
 * FDK_NO_GL stays the master opt-out (nothing GPU loads at all).
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_RENDER_BACKEND_H
#define FDK_RENDER_BACKEND_H

#include <stdlib.h>
#include <string.h>

enum {
    FDK__RB_AUTO = 0,
    FDK__RB_VULKAN,
    FDK__RB_OPENGL
};

/* The forced-or-auto choice, read once per lane acquisition (the
 * env does not change mid-process by contract; every FDK knob in
 * the tree already assumes that). */
static inline int fdk__rb_choice(void) {
    const char *b = getenv("FDK_GPU_BACKEND");
    if (b != NULL) {
        if (strcmp(b, "vulkan") == 0 || strcmp(b, "vk") == 0) {
            return FDK__RB_VULKAN;
        }
        if (strcmp(b, "opengl") == 0 || strcmp(b, "gl") == 0) {
            return FDK__RB_OPENGL;
        }
        /* anything else (including "auto") falls through to AUTO */
    }
    if (getenv("FDK_GL_VULKAN") != NULL) {
        return FDK__RB_VULKAN;
    }
    if (getenv("FDK_GL_OPENGL") != NULL) {
        return FDK__RB_OPENGL;
    }
    return FDK__RB_AUTO;
}

/* May the Vulkan lane be ATTEMPTED under the current forcing? */
static inline int fdk__rb_vulkan_allowed(void) {
    if (getenv("FDK_NO_GL") != NULL) {
        return 0; /* the master opt-out owns every GPU lane */
    }
    return fdk__rb_choice() != FDK__RB_OPENGL;
}

/* May the GL lanes (GLX/EGL) be ATTEMPTED under the current
 * forcing? (Their own renderer/size policies still apply.) */
static inline int fdk__rb_opengl_allowed(void) {
    if (getenv("FDK_NO_GL") != NULL) {
        return 0;
    }
    return fdk__rb_choice() != FDK__RB_VULKAN;
}

#endif /* FDK_RENDER_BACKEND_H */
