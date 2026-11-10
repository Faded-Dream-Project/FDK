/*
 * wayland_disabled.c — Wayland backend stub for builds where the
 * Wayland backend is compiled out (FDK_DISABLE_WAYLAND=1, or the
 * default auto-detect path finding no libwayland-dev / libxkbcommon-dev).
 *
 * Provides the two entry points src/core/context.c's
 * select_and_connect() calls (see platform_internal.h):
 *   - fdk_platform_wayland_ops()           — returns NULL ("not built")
 *   - fdk_platform_wayland_display_present() — returns 0 ("no Wayland")
 *
 * That's all the context layer needs to cleanly skip Wayland and fall
 * through to X11 (or fail with FDK_ERR_NO_DISPLAY if X11 also isn't
 * available — see fdk_init() in src/core/context.c). No Wayland
 * headers are #included here, by design.
 *
 * The real Wayland backend (the .c files under src/platform/wayland/)
 * provides working implementations of these two entry points when
 * built with Wayland dev headers available — see that directory's
 * wayland_platform.h and wayland_ops.c. This file is the OTHER side
 * of that conditional build.
 *
 * This file must NOT live under src/platform/wayland/ — the Makefile
 * wildcard (src/platform/wayland/ then a star then .c) would
 * otherwise pull it in even when Wayland is enabled (where the real
 * implementations would already exist, causing duplicate-definition
 * link errors). Keeping it at src/platform/wayland_disabled.c makes
 * the wildcard safe in both configurations.
 */

#include "platform/platform_internal.h"

const fdk_platform_ops *fdk_platform_wayland_ops(void) {
    /* NULL means "this backend was not compiled in" — the explicit
     * contract documented in platform_internal.h. select_and_connect()
     * skips any candidate whose ops is NULL. */
    return NULL;
}

int fdk_platform_wayland_display_present(void) {
    /* Even if $WAYLAND_DISPLAY is set, FDK can't actually speak
     * Wayland in a build that compiled the Wayland backend out. Report
     * "not present" so FDK_PLATFORM_AUTO never even considers trying
     * the (NULL) Wayland ops. FDK_PLATFORM_WAYLAND explicitly requested
     * still fails cleanly with FDK_ERR_NO_DISPLAY via the
     * "candidate_count == 0 -> last_failure == FDK_ERR_NO_DISPLAY"
     * path in select_and_connect(). */
    return 0;
}


/* P3: the add-on raw-event surface — Wayland-only by definition. */
fdk_wl_queue *fdk_wl_queue_create(fdk_context *ctx, const char *name) {
    (void)ctx;
    (void)name;
    return NULL;
}

void fdk_wl_queue_destroy(fdk_wl_queue *queue) {
    (void)queue;
}

void *fdk_wl_queue_registry(fdk_wl_queue *queue) {
    (void)queue;
    return NULL;
}

void *fdk_wl_raw_bind(fdk_wl_queue *queue, fdk_u32 name,
                      const void *wl_interface, fdk_u32 version) {
    (void)queue;
    (void)name;
    (void)wl_interface;
    (void)version;
    return NULL;
}

int fdk_wl_queue_dispatch(fdk_wl_queue *queue) {
    (void)queue;
    return 0;
}

void fdk_wl_queue_attach(fdk_wl_queue *queue, int on_pump) {
    (void)queue;
    (void)on_pump;
}
