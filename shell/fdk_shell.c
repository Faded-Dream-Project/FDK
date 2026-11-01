/*
 * fdk_shell.c — the fdk-shell add-on's front end: configuration
 * validation, per-backend dispatch, kind registration.
 *
 * One kind per overlay (the kind's user pointer is the overlay
 * state; kinds are add-on-owned per fdk_extend.h, so creating N
 * overlays registers N uniquely-named kinds and each destroy path
 * frees its own). The backend is whatever the context connected
 * to — the same binary is a layer-shell client on Wayland and an
 * EWMH dock client on X11.
 */

#include "fdk_shell_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const fdk_ext_surface_ops wl_ops = {
    sizeof(wl_ops),
    fdk_shell_wl_create,
    fdk_shell_wl_destroy,
    fdk_shell_wl_popup_attached,
};

static const fdk_ext_surface_ops x11_ops = {
    sizeof(x11_ops),
    fdk_shell_x11_create,
    fdk_shell_x11_destroy,
    fdk_shell_x11_popup_attached,
};

/* Wayland-disabled builds link only the X11 leg; the wl_ops row
 * still exists (the dispatcher selects per backend), so its three
 * ops resolve to stubs that fail loudly if the selection logic is
 * ever wrong. */
#if !FDK_SHELL_WAYLAND
fdk_result fdk_shell_wl_create(fdk_ext_surface *s,
                               fdk_ext_native_handle *n, void *u)
{
    (void)s; (void)n; (void)u;
    return FDK_ERR_UNSUPPORTED;
}
void fdk_shell_wl_destroy(fdk_ext_surface *s, void *u)
{
    (void)s; (void)u;
}
void fdk_shell_wl_popup_attached(fdk_ext_surface *s, void *p, void *u)
{
    (void)s; (void)p; (void)u;
}
#endif

int fdk_shell_api_version(void)
{
    return FDK_SHELL_API_VERSION;
}

static int cfg_valid(const fdk_shell_overlay_cfg *cfg)
{
    if (!cfg || cfg->struct_size < sizeof(*cfg))
        return 0;
    if ((unsigned)cfg->layer > FDK_SHELL_LAYER_OVERLAY)
        return 0;
    if (cfg->anchor & ~(FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_BOTTOM |
                        FDK_SHELL_ANCHOR_LEFT | FDK_SHELL_ANCHOR_RIGHT))
        return 0;
    if ((unsigned)cfg->keyboard > FDK_SHELL_KBD_ON_DEMAND)
        return 0;
    return 1;
}

fdk_result fdk_shell_overlay_create(fdk_context *ctx,
                                    const fdk_shell_overlay_cfg *cfg_in,
                                    fdk_ext_surface **out)
{
    fdk_ext_native_display nd;
    const fdk_ext_surface_ops *ops;
    fdk_ext_backend backend;
    shell_overlay *ov;
    fdk_shell_overlay_cfg cfg;
    char kind_name[32];

    if (!out)
        return FDK_ERR_INVALID_ARGUMENT;
    *out = NULL;
    if (!ctx || !cfg_valid(cfg_in))
        return FDK_ERR_INVALID_ARGUMENT;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)))
        return FDK_ERR_NOT_INITIALIZED;
    backend = nd.backend;

    cfg = *cfg_in;
    if (cfg.anchor == 0)
        cfg.anchor = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                     FDK_SHELL_ANCHOR_RIGHT;

    if (backend == FDK_EXT_BACKEND_WAYLAND) {
        /* The Wayland overlay state embeds the head; the registry
         * pass and the role live in fdk_shell_wayland.c. */
        ov = calloc(1, sizeof(struct wl_state));
        if (!ov)
            return FDK_ERR_OUT_OF_MEMORY;
        ops = &wl_ops;
    } else if (backend == FDK_EXT_BACKEND_X11) {
        ov = calloc(1, sizeof(struct x11_state));
        if (!ov)
            return FDK_ERR_OUT_OF_MEMORY;
        ops = &x11_ops;
    } else {
        return FDK_ERR_NOT_INITIALIZED;
    }
    ov->cfg = cfg;
    ov->backend = backend;

    /* Unique kind name per overlay (kind names are per-context
     * unique; a counter suffices — the context dies with all its
     * kinds anyway). */
    static unsigned seq;
    snprintf(kind_name, sizeof kind_name, "fdk-shell-%u", ++seq);

    fdk_result rc = fdk_ext_register_kind(ctx, kind_name, ops, ov,
                                          &ov->kind);
    if (!fdk_ok(rc)) {
        free(ov);
        return rc;
    }

    rc = fdk_ext_surface_create(ov->kind, out);
    if (!fdk_ok(rc)) {
        fdk_ext_kind_destroy(ov->kind);
        free(ov);
        return rc;
    }
    return FDK_OK;
}

fdk_result fdk_shell_overlay_reconfigure(fdk_ext_surface *overlay,
                                         const fdk_shell_overlay_cfg *cfg)
{
    (void)overlay; (void)cfg;
    /* v2: live reconfigure (Wayland set_* + commit; X11 move/
     * resize + strut update). The v1 contract is create-time
     * configuration; a reconfigure that changes the shape is a
     * destroy + create away for callers that need it now. */
    return FDK_ERR_UNSUPPORTED;
}

fdk_ext_backend fdk_shell_overlay_backend(const fdk_ext_surface *overlay)
{
    if (!overlay)
        return FDK_EXT_BACKEND_NONE;
    /* The extension API deliberately does not expose another
     * object's internals; the backend of an overlay is the
     * context's, which the caller can query directly. */
    return fdk_ext_active_backend(fdk_ext_surface_context(
        (fdk_ext_surface *)overlay));
}
