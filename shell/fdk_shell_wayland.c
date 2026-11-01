/*
 * fdk_shell_wayland.c — the Wayland side of fdk-shell: a
 * zwlr_layer_surface_v1 client built on FDK's extension API.
 *
 * The add-on owns exactly what docs/EXTENDING.md's contract says
 * it owns: its own registry (a second registry on the shared
 * display is ordinary Wayland), the layer-shell manager, the
 * layer surface (the ROLE), and the configure acks. FDK owns the
 * wl_surface's lifetime, the buffer presents (gated until the
 * first configure is reported — the labwc/sway error-2 rule,
 * enforced by fdk_extend.h's present gate), and the xdg_popup
 * creation whose proxy popup_attached adopts here via
 * get_popup.
 */

#include "fdk_shell_internal.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdlib.h>
#include <string.h>

#include "wayland-client.h"

typedef struct wl_state wl_overlay;

/* ---- the registry pass: shell + compositor + (named) output -------------- */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *iface,
                            uint32_t version)
{
    wl_overlay *ov = data;
    if (strcmp(iface, "zwlr_layer_shell_v1") == 0) {
        ov->shell = wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface,
            version < 4 ? version : 4);
    } else if (strcmp(iface, "wl_compositor") == 0) {
        ov->compositor = wl_registry_bind(registry, name,
                                          &wl_compositor_interface,
                                          version < 4 ? version : 4);
    } else if (strcmp(iface, "wl_output") == 0) {
        /* First output wins (v1; a named-output selector needs a
         * second registry pass reading the output's own name
         * event — a v2 addition). */
        if (!ov->output)
            ov->output = wl_registry_bind(registry, name,
                                          &wl_output_interface, 1);
    }
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name)
{
    (void)data; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

/* ---- the layer surface's events ------------------------------------------ */

static void layer_configure(void *data,
                            struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t width,
                            uint32_t height)
{
    wl_overlay *ov = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    /* Report AFTER the ack (the extension contract's order); FDK
     * commits the deferred first frame post-ack and resizes its
     * render machinery, then the event callback redraws. */
    fdk_ext_surface_notify_configure(ov->head.surface, (fdk_i32)width,
                                     (fdk_i32)height);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer)
{
    (void)data; (void)layer;
    /* The compositor withdrew the layer surface; the app observes
     * the surface's silence and destroys it at its leisure. */
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    layer_configure,
    layer_closed,
};

/* ---- the kind ops --------------------------------------------------------- */

fdk_result fdk_shell_wl_create(fdk_ext_surface *surface,
                               fdk_ext_native_handle *out_native,
                               void *user)
{
    wl_overlay *ov = user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    fdk_shell_overlay_cfg *cfg = &ov->head.cfg;
    uint32_t anchor = 0;

    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)) ||
        nd.backend != FDK_EXT_BACKEND_WAYLAND)
        return FDK_ERR_NOT_INITIALIZED;

    ov->head.surface = surface;
    ov->display = (struct wl_display *)nd.display;

    ov->registry = wl_display_get_registry(ov->display);
    if (!ov->registry)
        return FDK_ERR_OUT_OF_MEMORY;
    wl_registry_add_listener(ov->registry, &registry_listener, ov);
    wl_display_roundtrip(ov->display);
    if (!ov->shell || !ov->compositor) {
        fdk_shell_wl_destroy(surface, user);
        return FDK_ERR_UNSUPPORTED; /* no layer-shell compositor */
    }

    struct wl_surface *wl_surf = wl_compositor_create_surface(
        ov->compositor);
    if (!wl_surf) {
        fdk_shell_wl_destroy(surface, user);
        return FDK_ERR_WINDOW_CREATE;
    }

    /* The role. Namespace: the app identity, "fdk-shell" is the
     * documented default. */
    ov->layer = zwlr_layer_shell_v1_get_layer_surface(
        ov->shell, wl_surf, ov->output, (uint32_t)cfg->layer,
        "fdk-shell");
    if (!ov->layer) {
        wl_surface_destroy(wl_surf);
        fdk_shell_wl_destroy(surface, user);
        return FDK_ERR_WINDOW_CREATE;
    }
    zwlr_layer_surface_v1_add_listener(ov->layer, &layer_listener, ov);

    /* Set every role parameter BEFORE the first commit. */
    int w = 0, h = 0;
    int horiz = (cfg->anchor & FDK_SHELL_ANCHOR_LEFT) &&
                (cfg->anchor & FDK_SHELL_ANCHOR_RIGHT);
    int vert = (cfg->anchor & FDK_SHELL_ANCHOR_TOP) &&
               (cfg->anchor & FDK_SHELL_ANCHOR_BOTTOM);
    int none = !(cfg->anchor & (FDK_SHELL_ANCHOR_TOP |
                                FDK_SHELL_ANCHOR_BOTTOM |
                                FDK_SHELL_ANCHOR_LEFT |
                                FDK_SHELL_ANCHOR_RIGHT));
    int size = cfg->size > 0 ? cfg->size : 32;
    if (horiz && !vert) {
        h = size; /* width comes from the anchored edges        */
    } else if (vert && !horiz) {
        w = size;
    } else if (none) {
        w = size;
        h = size; /* floating overlay                            */
    } else {
        w = size; /* corner/edge anchor: both axes from cfg     */
        h = size;
    }
    zwlr_layer_surface_v1_set_size(ov->layer, w, h);

    if (cfg->anchor & FDK_SHELL_ANCHOR_TOP)
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    if (cfg->anchor & FDK_SHELL_ANCHOR_BOTTOM)
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    if (cfg->anchor & FDK_SHELL_ANCHOR_LEFT)
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    if (cfg->anchor & FDK_SHELL_ANCHOR_RIGHT)
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    zwlr_layer_surface_v1_set_anchor(ov->layer, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(ov->layer, cfg->exclusive);
    zwlr_layer_surface_v1_set_margin(ov->layer, cfg->margin_top,
                                     cfg->margin_right,
                                     cfg->margin_bottom,
                                     cfg->margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        ov->layer, (uint32_t)cfg->keyboard);

    /* The initial EMPTY commit starts the configure handshake.
     * No buffer may ride it (the protocol's never-configured
     * error); FDK's present gate makes the eager first render
     * after show() safe by deferring its commit until the first
     * notify_configure above. */
    wl_surface_commit(wl_surf);

    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = wl_surf;
    return FDK_OK;
}

void fdk_shell_wl_destroy(fdk_ext_surface *surface, void *user)
{
    wl_overlay *ov = user;
    if (ov->layer) {
        zwlr_layer_surface_v1_destroy(ov->layer);
        ov->layer = NULL;
    }
    if (ov->shell) {
        wl_proxy_destroy((struct wl_proxy *)ov->shell);
        ov->shell = NULL;
    }
    if (ov->output) {
        wl_proxy_destroy((struct wl_proxy *)ov->output);
        ov->output = NULL;
    }
    if (ov->compositor) {
        wl_proxy_destroy((struct wl_proxy *)ov->compositor);
        ov->compositor = NULL;
    }
    if (ov->registry) {
        wl_proxy_destroy((struct wl_proxy *)ov->registry);
        ov->registry = NULL;
    }
    (void)surface;
}

void fdk_shell_wl_popup_attached(fdk_ext_surface *surface,
                                 void *native_popup, void *user)
{
    wl_overlay *ov = user;
    /* Adopt FDK's xdg_popup so it stacks with the layer surface
     * (the wlr-layer-shell pattern; X11 never calls this). */
    if (ov->layer)
        zwlr_layer_surface_v1_get_popup(
            ov->layer, (struct xdg_popup *)native_popup);
    (void)surface;
}
