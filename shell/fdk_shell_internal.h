/*
 * fdk_shell_internal.h — shared state of the fdk-shell add-on.
 *
 * One overlay = one wl_overlay (Wayland) or one x11_overlay
 * (X11), allocated by fdk_shell_overlay_create and freed by the
 * KIND's destroy callback... with one wrinkle the extension
 * contract forces: the kind's destroy callback receives only the
 * surface and the user pointer, so the overlay state IS the user
 * pointer, and fdk_ext_surface_destroy is where the free happens
 * (the destroy callback NULLs the pointers it owns).
 */

#ifndef FDK_SHELL_INTERNAL_H
#define FDK_SHELL_INTERNAL_H

#include "fdk_shell_build.h" /* FDK_SHELL_WAYLAND (Makefile -D)  */

#include "fdk_shell.h"

#include <fdk/fdk_error.h>
#include <fdk/fdk_extend.h>

/* The per-overlay state (backend-neutral head; each backend adds
 * its own members through embedding). */
typedef struct shell_overlay {
    fdk_ext_surface *surface;   /* FDK's wrapper (borrowed)       */
    fdk_ext_surface_kind *kind; /* the registered kind (owned)    */
    fdk_shell_overlay_cfg cfg;  /* last-applied configuration     */
    fdk_ext_backend backend;
} shell_overlay;

/* Per-backend kind ops (fdk_extend.h contract). */
fdk_result fdk_shell_wl_create(fdk_ext_surface *surface,
                               fdk_ext_native_handle *out_native,
                               void *user);
void fdk_shell_wl_destroy(fdk_ext_surface *surface, void *user);
void fdk_shell_wl_popup_attached(fdk_ext_surface *surface,
                                 void *native_popup, void *user);

fdk_result fdk_shell_x11_create(fdk_ext_surface *surface,
                                fdk_ext_native_handle *out_native,
                                void *user);
void fdk_shell_x11_destroy(fdk_ext_surface *surface, void *user);
void fdk_shell_x11_popup_attached(fdk_ext_surface *surface,
                                  void *native_popup, void *user);

/* The backend states (defined in their .c files; the dispatcher
 * allocates one slot per backend and hands it to the kind ops as
 * the user pointer — the states embed the shell_overlay head). */
struct wl_state {
    shell_overlay head;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_output *output;
    struct zwlr_layer_shell_v1 *shell;
    struct zwlr_layer_surface_v1 *layer;
};
struct x11_state {
    shell_overlay head;
    unsigned long window; /* Window (XID); X11 headers stay out
                           * of this shared header           */
};

#endif /* FDK_SHELL_INTERNAL_H */
