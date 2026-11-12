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

/* Prototypes for the no-op wayland-layer twins defined below (the
 * real ones live in src/platform/wayland/wayland_im_vk.c and are
 * declared in its wayland_platform.h — which needs the scanner
 * headers and is not includable here). */
void fdk_wayland_input_method_destroy(void *im);
void *fdk_wayland_input_method_grab(void *im);
void fdk_wayland_input_method_commit(void *im, const char *text);
void fdk_wayland_virtual_keyboard_key(void *kb, fdk_u32 time_ms,
                                      fdk_u32 key, fdk_u32 state);
void fdk_wayland_virtual_keyboard_destroy(void *kb);
void fdk_session_lock_unlock_impl(void *lock);

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


/* P3: the add-on raw-event surface — Wayland-only by definition.
 * fdk_wl_queue_create is NOT stubbed here: the ops-routing
 * definition in src/window/extend.c covers both builds (in a
 * Wayland-disabled build ctx->ops->wl_queue_create is NULL and it
 * returns NULL on its own); a second definition here collided at
 * link time in the X11-only configuration. */
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

/* The handle-taking input-method / virtual-keyboard entries in
 * src/window/extend.c call the wayland layer directly (no ctx to
 * route through — the handle IS the connection's object), so the
 * disabled build needs the no-op twins here. Found live: the
 * X11-only configuration failed to link fdk_input_method_close /
 * fdk_virtual_keyboard_key (CI always builds with Wayland, which is
 * why this never fired there). */
void fdk_wayland_input_method_destroy(void *im) {
    (void)im;
}

void *fdk_wayland_input_method_grab(void *im) {
    (void)im;
    return NULL;
}

void fdk_wayland_input_method_commit(void *im, const char *text) {
    (void)im;
    (void)text;
}

void fdk_wayland_virtual_keyboard_key(void *kb, fdk_u32 time_ms,
                                      fdk_u32 key, fdk_u32 state) {
    (void)kb;
    (void)time_ms;
    (void)key;
    (void)state;
}

void fdk_wayland_virtual_keyboard_destroy(void *kb) {
    (void)kb;
}

/* Same class: fdk_session_lock_unlock() in extend.c routes through
 * this impl symbol (the lock handle carries no conn back-reference),
 * so the disabled build needs the no-op twin. */
void fdk_session_lock_unlock_impl(void *lock) {
    (void)lock;
}
