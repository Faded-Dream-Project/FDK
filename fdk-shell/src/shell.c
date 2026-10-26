/*
 * shell.c — fdk-shell's shared core (0.1.0)
 *
 * The public API's implementation: the per-context handle, layer
 * and popup lifecycle on top of FDK's extension API
 * (fdk_ext_surface_create / fdk_ext_popup_create — every surface
 * here is an FDK extension surface), event forwarding, and the
 * backend dispatch the X11 and Wayland halves plug into.
 *
 * The structural rule (the same one fdk_extend.h teaches): this
 * package never touches an FDK private header or struct. The native
 * protocols are spoken by the backend files through the documented
 * accessors; the shared code only orchestrates.
 */

#include "shell_internal.h"

#include <stdio.h>

/* ---- the pending-layer slot ---------------------------------------------- */

struct fdk_shell_layer *fdkshell__pending_layer = NULL;

const char *fdk_shell_version(void) {
    return FDKSHELL_VERSION_STRING;
}

/* ---- kind ops: routed to the active backend -------------------------------
 *
 * The kind's user data is the fdk_shell handle; the layer being
 * created is staged in fdkshell__pending_layer (single create in
 * flight — the UI-thread contract). */

static fdk_result shell_kind_create(fdk_ext_surface *surface,
                                    fdk_ext_native_handle *out_native,
                                    void *user) {
    fdk_shell *shell = user;
    fdk_shell_layer *layer = fdkshell__pending_layer;
    if (layer == NULL || layer->shell != shell) {
        /* Impossible unless create is invoked outside
         * fdk_shell_layer_create — refuse loudly. */
        FDKS_ERROR("kind create without a staged layer");
        return FDK_ERR_INVALID_STATE;
    }
    fdkshell__pending_layer = NULL;

    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            return fdkshell__x11_layer_create(shell, layer, out_native);
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            return fdkshell__wl_layer_create(shell, layer, out_native);
#endif
        default:
            (void)surface;
            return FDK_ERR_UNSUPPORTED;
    }
}

static void shell_kind_destroy(fdk_ext_surface *surface, void *user) {
    fdk_shell *shell = user;
    (void)surface;
    /* The layer object itself is freed by layer_destroy_notify (the
     * ext window's destroy notification); this callback fires FIRST
     * (the role teardown) while the native surface is still alive. */
    fdk_shell_layer *layer = shell->layers_head;
    while (layer != NULL) {
        if (layer->ext == surface) {
            break;
        }
        layer = layer->next;
    }
    if (layer == NULL) {
        return; /* already unlinked (shutdown sweep shapes) */
    }
    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            fdkshell__x11_layer_destroy(shell, layer);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            fdkshell__wl_layer_destroy(shell, layer);
            break;
#endif
        default:
            break;
    }
}


/* ---- init / shutdown ------------------------------------------------------- */

int fdk_shell_backend_supported(fdk_context *ctx) {
    fdk_ext_backend backend = fdk_ext_active_backend(ctx);
    switch (backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            return fdkshell__x11_supported(ctx);
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            return fdkshell__wl_supported(ctx);
#endif
        default:
            return 0;
    }
}

fdk_result fdk_shell_init(fdk_context *ctx, fdk_shell **out) {
    if (ctx == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!fdk_ext_has_capability(ctx, FDK_EXT_CAP_SHM_RENDER)) {
        return FDK_ERR_NOT_INITIALIZED; /* no live render-capable conn */
    }

    fdk_ext_backend backend = fdk_ext_active_backend(ctx);
    int supported = 0;
    switch (backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            supported = fdkshell__x11_supported(ctx);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            supported = fdkshell__wl_supported(ctx);
            break;
#endif
        default:
            supported = 0;
            break;
    }
    if (!supported) {
        /* Wayland compositors without wlr-layer-shell (GNOME,
         * KDE's kwin do NOT implement it) or a backend compiled out
         * — the honest refusal the README documents. */
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    shell->ctx = ctx;
    shell->backend = backend;

    fdk_result r = FDK_ERR_UNSUPPORTED;
    switch (backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            r = fdkshell__x11_init(shell);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            r = fdkshell__wl_init(shell);
            break;
#endif
        default:
            break;
    }
    if (!fdk_ok(r)) {
        free(shell);
        return r;
    }

    /* The surface kind — THE registration the whole package hangs
     * off (fdk_extend.h's surface kinds). */
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.struct_size = sizeof(ops);
    ops.create = shell_kind_create;
    ops.destroy = shell_kind_destroy;
#if defined(FDKSHELL_BUILD_WAYLAND)
    ops.popup_attached = shell_kind_popup_attached;
#endif
    r = fdk_ext_register_kind(ctx, "fdk-shell.layer", &ops, shell,
                              &shell->kind);
    if (!fdk_ok(r)) {
        switch (backend) {
#if defined(FDKSHELL_BUILD_X11)
            case FDK_EXT_BACKEND_X11:
                fdkshell__x11_shutdown(shell);
                break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
            case FDK_EXT_BACKEND_WAYLAND:
                fdkshell__wl_shutdown(shell);
                break;
#endif
            default:
                break;
        }
        free(shell);
        return r;
    }

    FDKS_INFO("fdk-shell %s initialized (backend %s)",
              FDKSHELL_VERSION_STRING,
              backend == FDK_EXT_BACKEND_X11 ? "x11" : "wayland");
    *out = shell;
    return FDK_OK;
}

static void shell_layer_unlink(fdk_shell *shell, fdk_shell_layer *layer);
static void shell_popup_unlink(fdk_shell *shell, fdk_shell_popup *popup);

void fdk_shell_shutdown(fdk_shell *shell) {
    if (shell == NULL) {
        return;
    }
    /* Teardown, not delivery: destroy every live surface without
     * invoking callbacks (the ext destroy chain runs the role
     * teardown + frees the wrappers; the shell objects unlink in
     * their notify paths). */
    while (shell->popups_head != NULL) {
        fdk_shell_popup_destroy(shell->popups_head);
    }
    while (shell->layers_head != NULL) {
        fdk_shell_layer_destroy(shell->layers_head);
    }
    if (shell->kind != NULL) {
        fdk_ext_kind_destroy(shell->kind);
    }
    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            fdkshell__x11_shutdown(shell);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            fdkshell__wl_shutdown(shell);
            break;
#endif
        default:
            break;
    }
    free(shell);
}

/* ---- outputs --------------------------------------------------------------- */

fdk_result fdk_shell_output_count(fdk_shell *shell, uint32_t *out_count) {
    if (shell == NULL || out_count == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_count = 0;
    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            *out_count = fdkshell__x11_output_count(shell);
            return FDK_OK;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            *out_count = fdkshell__wl_output_count(shell);
            return FDK_OK;
#endif
        default:
            return FDK_ERR_UNSUPPORTED;
    }
}

fdk_result fdk_shell_output_name(fdk_shell *shell, uint32_t index,
                                 char *buf, size_t buflen) {
    if (shell == NULL || buf == NULL || buflen == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            return fdkshell__x11_output_name(shell, index, buf, buflen);
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            return fdkshell__wl_output_name(shell, index, buf, buflen);
#endif
        default:
            return FDK_ERR_UNSUPPORTED;
    }
}

fdk_result fdk_shell_output_get_info(fdk_shell *shell, uint32_t index,
                                      fdk_shell_output_info *out) {
    if (shell == NULL || out == NULL ||
        out->struct_size != sizeof(fdk_shell_output_info)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset((char *)out + sizeof(out->struct_size), 0,
           sizeof(*out) - sizeof(out->struct_size));
    switch (shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            return fdkshell__x11_output_get_info(shell, index, out);
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            return fdkshell__wl_output_get_info(shell, index, out);
#endif
        default:
            return FDK_ERR_UNSUPPORTED;
    }
}

void fdk_shell_set_output_event_callback(fdk_shell *shell,
                                         fdk_shell_output_event_fn fn,
                                         void *user) {
    if (shell == NULL) {
        return;
    }
    shell->output_cb = fn;
    shell->output_cb_user = user;
}

/* The backends' single fire path (shell_internal.h). */
void fdkshell__fire_output_event(fdk_shell *shell,
                                 fdk_shell_output_event_type type,
                                 uint32_t index) {
    if (shell == NULL || shell->output_cb == NULL) {
        return;
    }
    fdk_shell_output_event ev = {.type = type, .index = index};
    shell->output_cb(shell, &ev, shell->output_cb_user);
}

/* ---- layers ---------------------------------------------------------------- */

/* Event forwarding: the ext surface's stream, re-signed with the
 * layer handle. Configure sizes are cached for get_size. */
static void shell_layer_event(fdk_ext_surface *ext,
                              const fdk_event_data *event, void *user) {
    (void)ext;
    fdk_shell_layer *layer = user;
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        layer->size = event->configure.size;
    }
    if (layer->cb != NULL) {
        layer->cb(layer, event, layer->cb_user);
    }
}

/* Every destroy path (app destroy, shutdown sweep) funnels through
 * fdk_ext_surface_destroy, whose notify chain runs the kind's role
 * teardown first; this is where the shell object itself is freed. */
fdk_result fdk_shell_layer_create(fdk_shell *shell,
                                  const fdk_shell_layer_desc *desc,
                                  fdk_shell_layer **out) {
    if (shell == NULL || desc == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc->struct_size < offsetof(fdk_shell_layer_desc, layer) +
                                sizeof(fdk_shell_layer_level)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc->anchors == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc->anchors & ~(uint32_t)(FDK_SHELL_ANCHOR_TOP |
                                    FDK_SHELL_ANCHOR_BOTTOM |
                                    FDK_SHELL_ANCHOR_LEFT |
                                    FDK_SHELL_ANCHOR_RIGHT)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc->layer < FDK_SHELL_LAYER_BACKGROUND ||
        desc->layer > FDK_SHELL_LAYER_OVERLAY) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (desc->keyboard < FDK_SHELL_KBD_NONE ||
        desc->keyboard > FDK_SHELL_KBD_ON_DEMAND) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_shell_layer *layer = calloc(1, sizeof(*layer));
    if (layer == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    layer->shell = shell;
    layer->cb = NULL;
    layer->cb_user = NULL;
    layer->size.width = 0;
    layer->size.height = 0;
    /* The desc copy respects struct_size: fields beyond what the
     * caller knows about stay zero (the append-only contract, both
     * directions). */
    memset(&layer->desc, 0, sizeof(layer->desc));
    memcpy(&layer->desc, desc,
           (desc->struct_size < sizeof(layer->desc)) ? desc->struct_size
                                                    : sizeof(layer->desc));

    /* Stage + adopt: the kind's create callback (shell_kind_create)
     * dispatches to the backend, which builds the native surface +
     * role and returns the tagged handle. */
    fdkshell__pending_layer = layer;
    fdk_result r = fdk_ext_surface_create(shell->kind, &layer->ext);
    if (!fdk_ok(r)) {
        fdkshell__pending_layer = NULL;
        free(layer);
        return r;
    }

    /* Events route through the shell's forwarder. */
    fdk_ext_surface_set_event_callback(layer->ext, shell_layer_event,
                                       layer);

    /* Link into the shell's list. */
    layer->prev = shell->layers_tail;
    layer->next = NULL;
    if (shell->layers_tail != NULL) {
        shell->layers_tail->next = layer;
    } else {
        shell->layers_head = layer;
    }
    shell->layers_tail = layer;

    *out = layer;
    return FDK_OK;
}

static void shell_layer_unlink(fdk_shell *shell, fdk_shell_layer *layer) {
    if (layer->prev != NULL) {
        layer->prev->next = layer->next;
    } else if (shell->layers_head == layer) {
        shell->layers_head = layer->next;
    }
    if (layer->next != NULL) {
        layer->next->prev = layer->prev;
    } else if (shell->layers_tail == layer) {
        shell->layers_tail = layer->prev;
    }
    layer->prev = layer->next = NULL;
}

void fdk_shell_layer_destroy(fdk_shell_layer *layer) {
    if (layer == NULL) {
        return;
    }
    fdk_shell *shell = layer->shell;

    /* Child popups die WITH their layer: FDK's popup-family sweep
     * destroys the ext popups when the parent surface goes — destroy
     * the shell wrappers FIRST (found by the parent back-pointer) so
     * nothing dangles. This is also the documented application
     * contract: popups before their layer (README), enforced here
     * either way. */
    fdk_shell_popup *p = shell->popups_head;
    while (p != NULL) {
        fdk_shell_popup *next = p->next;
        if (p->parent == layer) {
            fdk_shell_popup_destroy(p);
        }
        p = next;
    }

    /* The ext destroy chain runs the kind's role teardown FIRST —
     * shell_kind_destroy finds this layer through the shell's list,
     * so unlink only AFTER the destroy (the layer is the role's
     * context). Then the native surface dies, then our wrapper. */
    fdk_ext_surface_destroy(layer->ext);
    shell_layer_unlink(shell, layer);
    free(layer);
}

fdk_result fdk_shell_layer_get_surface(fdk_shell_layer *layer,
                                       fdk_surface **out) {
    if (layer == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return fdk_ext_surface_get_surface(layer->ext, out);
}

void fdk_shell_layer_set_event_callback(fdk_shell_layer *layer,
                                        fdk_shell_event_fn callback,
                                        void *user) {
    if (layer == NULL) {
        return;
    }
    layer->cb = callback;
    layer->cb_user = user;
}

void fdk_shell_layer_set_cursor(fdk_shell_layer *layer,
                                fdk_shell_cursor cursor) {
    if (layer == NULL) {
        return;
    }
    /* The extension surface carries it; the shell's enum maps 1:1
     * onto fdk_cursor_shape's values (both start DEFAULT=0,
     * POINTER=1 — the extension API takes the plain int). */
    fdk_ext_surface_set_cursor(layer->ext, (int)cursor);
}

fdk_result fdk_shell_layer_get_size(const fdk_shell_layer *layer,
                                    fdk_size *out_size) {
    if (layer == NULL || out_size == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_size = layer->size;
    return FDK_OK;
}

void fdk_shell_layer_show(fdk_shell_layer *layer) {
    if (layer == NULL) {
        return;
    }
    fdk_ext_surface_show(layer->ext);
    switch (layer->shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            fdkshell__x11_layer_show(layer);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            fdkshell__wl_layer_show(layer);
            break;
#endif
        default:
            break;
    }
}

void fdk_shell_layer_hide(fdk_shell_layer *layer) {
    if (layer == NULL) {
        return;
    }
    fdk_ext_surface_hide(layer->ext);
    switch (layer->shell->backend) {
#if defined(FDKSHELL_BUILD_X11)
        case FDK_EXT_BACKEND_X11:
            fdkshell__x11_layer_hide(layer);
            break;
#endif
#if defined(FDKSHELL_BUILD_WAYLAND)
        case FDK_EXT_BACKEND_WAYLAND:
            fdkshell__wl_layer_hide(layer);
            break;
#endif
        default:
            break;
    }
}

/* ---- popups ------------------------------------------------------------------ */

static void shell_popup_event(fdk_ext_surface *ext,
                              const fdk_event_data *event, void *user) {
    (void)ext;
    fdk_shell_popup *popup = user;
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        popup->size = event->configure.size;
    }
    if (popup->cb != NULL) {
        popup->cb(popup, event, popup->cb_user);
    }
}

/* The shared popup body: an extension popup parented to `parent_ext`
 * (a layer OR another popup), wrapped and linked. */
/* The shared popup body: an extension popup parented to `parent_ext`
 * (a LAYER or another POPUP — the cascade API), wrapped and linked
 * into the shell's popup list. */
static fdk_result shell_popup_create_common(fdk_shell *shell,
                                            fdk_shell_layer *layer_parent,
                                            fdk_shell_popup *popup_parent,
                                            fdk_ext_surface *parent_ext,
                                            int32_t x, int32_t y,
                                            int32_t width, int32_t height,
                                            fdk_shell_popup **out) {
    if (shell == NULL || parent_ext == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_shell_popup *popup = calloc(1, sizeof(*popup));
    if (popup == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    popup->shell = shell;
    popup->parent = layer_parent; /* NULL for cascade children    */
    popup->parent_popup = popup_parent;
    popup->size.width = width > 0 ? width : 0;
    popup->size.height = height > 0 ? height : 0;

    /* The extension API's popup contract — the entire grab/
     * dismissal/positioning machinery, on both backends (Wayland's
     * kind popup_attached hook parents the xdg_popup to the layer
     * surface; a popup parent uses xdg_popup-on-xdg_popup). */
    fdk_result r = fdk_ext_popup_create(parent_ext, x, y, width, height,
                                        &popup->ext);
    if (!fdk_ok(r)) {
        free(popup);
        return r;
    }
    fdk_ext_surface_set_event_callback(popup->ext, shell_popup_event,
                                       popup);

    popup->prev = shell->popups_tail;
    popup->next = NULL;
    if (shell->popups_tail != NULL) {
        shell->popups_tail->next = popup;
    } else {
        shell->popups_head = popup;
    }
    shell->popups_tail = popup;

    *out = popup;
    return FDK_OK;
}

fdk_result fdk_shell_popup_create(fdk_shell_layer *parent, int32_t x,
                                  int32_t y, int32_t width, int32_t height,
                                  fdk_shell_popup **out) {
    if (parent == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return shell_popup_create_common(parent->shell, parent, NULL,
                                     parent->ext, x, y, width, height,
                                     out);
}

fdk_result fdk_shell_popup_create_child(fdk_shell_popup *parent,
                                        int32_t x, int32_t y,
                                        int32_t width, int32_t height,
                                        fdk_shell_popup **out) {
    if (parent == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* The cascade chain resolves to its layer ancestor for the
     * family teardown semantics (a layer dying sweeps every popup,
     * root or cascade). */
    fdk_shell_layer *root_layer = parent->parent;
    for (fdk_shell_popup *p = parent; p != NULL; p = p->parent_popup) {
        if (p->parent != NULL) {
            root_layer = p->parent;
            break;
        }
    }
    return shell_popup_create_common(parent->shell, root_layer, parent,
                                     parent->ext, x, y, width, height,
                                     out);
}

static void shell_popup_unlink(fdk_shell *shell, fdk_shell_popup *popup) {
    if (popup->prev != NULL) {
        popup->prev->next = popup->next;
    } else if (shell->popups_head == popup) {
        shell->popups_head = popup->next;
    }
    if (popup->next != NULL) {
        popup->next->prev = popup->prev;
    } else if (shell->popups_tail == popup) {
        shell->popups_tail = popup->prev;
    }
    popup->prev = popup->next = NULL;
}

void fdk_shell_popup_destroy(fdk_shell_popup *popup) {
    if (popup == NULL) {
        return;
    }
    fdk_shell *shell = popup->shell;
    /* Cascade children die with their parent (the family contract):
     * destroy their shell wrappers FIRST — the ext destroy below
     * sweeps their ext surfaces, which would otherwise leave the
     * wrappers' pointers dangling. Re-scan per round (the popup
     * destroy's own recursion frees entries anywhere in the list —
     * a saved `next` chain would dangle, the layer sweep's shape). */
    for (;;) {
        fdk_shell_popup *victim = NULL;
        for (fdk_shell_popup *p = shell->popups_head; p != NULL;
             p = p->next) {
            if (p->parent_popup == popup) {
                victim = p;
                break;
            }
        }
        if (victim == NULL) {
            break;
        }
        fdk_shell_popup_destroy(victim);
    }
    shell_popup_unlink(shell, popup);
    fdk_ext_surface_destroy(popup->ext);
    free(popup);
}

fdk_result fdk_shell_popup_get_surface(fdk_shell_popup *popup,
                                       fdk_surface **out) {
    if (popup == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return fdk_ext_surface_get_surface(popup->ext, out);
}

void fdk_shell_popup_set_event_callback(fdk_shell_popup *popup,
                                        fdk_shell_popup_event_fn callback,
                                        void *user) {
    if (popup == NULL) {
        return;
    }
    popup->cb = callback;
    popup->cb_user = user;
}

void fdk_shell_popup_set_cursor(fdk_shell_popup *popup,
                                fdk_shell_cursor cursor) {
    if (popup == NULL) {
        return;
    }
    fdk_ext_surface_set_cursor(popup->ext, (int)cursor);
}

void fdk_shell_popup_show(fdk_shell_popup *popup) {
    if (popup == NULL) {
        return;
    }
    fdk_ext_surface_show(popup->ext);
}

void fdk_shell_popup_hide(fdk_shell_popup *popup) {
    if (popup == NULL) {
        return;
    }
    fdk_ext_surface_hide(popup->ext);
}

/* ---- layer teardown ordering (why destroy is shaped as it is) --------------
 *
 * fdk_shell_layer_destroy: (1) popups first — FDK's popup-family
 * sweep would destroy their ext surfaces anyway, so the shell
 * wrappers must go before their ext handles dangle; (2) the ext
 * destroy (kind role teardown finds the layer through the shell's
 * list, so the list unlink has to come after it); (3) unlink + free.
 * fdk_shell_shutdown follows the same shape globally: popups, then
 * layers, then the kind, then the backend state. */
