/*
 * extend.c — the extension API's window layer (1.1.0)
 *
 * Implementation of include/fdk/fdk_extend.h's surface half: kind
 * registration, extension-surface creation (the add-on's create
 * callback builds the native surface; the backend's ext_surface_adopt
 * op wraps it), popups parented to extension surfaces, event routing
 * (a trampoline on the internal fdk_window hands every
 * fdk_event_data to the add-on's fdk_ext_event_fn), and the
 * documented accessors (native display/handle, the fdk_surface
 * render target, the backend/capability queries).
 *
 * The structural rule that keeps this safe: an extension surface IS
 * an fdk_window internally — the same creation field-init, the same
 * registry membership, the same dispatch path, the same popup-family
 * sweep — but the fdk_window* is never handed out. Every public
 * function speaks in fdk_ext_surface*, so the window-API operations
 * that would be meaningless or harmful on a role-carrying surface
 * (set_title, maximize, resize...) are simply unreachable, and the
 * operations that ARE meaningful (render, events, popups, show,
 * destroy) are the ones this file forwards.
 */

#define FDK_LOG_TAG "extend"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "window/window_internal.h"

#include <string.h>

/* ---- the objects -------------------------------------------------------- */

/* A registered surface kind (fdk_ext_register_kind). Add-on owned:
 * destroyed explicitly with fdk_ext_kind_destroy (refused while
 * surfaces live) or dropped with the context (after its surfaces
 * were swept). Intrusively linked on the owning context's
 * ext_kinds_head (context_internal.h). */
struct fdk_ext_surface_kind {
    fdk_context *ctx;
    char *name; /* owned copy — diagnostics, uniqueness key      */
    fdk_ext_surface_ops ops; /* the copied callback table         */
    void *user;
    size_t live_surfaces; /* how many of ours still exist        */
    struct fdk_ext_surface_kind *next;
};

/* The extension surface itself. `window` is the internal wrapper
 * built around the ADOPTED platform window; it is never public.
 * `kind` is NULL for FDK-owned extension surfaces (the popups
 * fdk_ext_popup_create makes — no kind callbacks fire for them). */
struct fdk_ext_surface {
    fdk_window *window;
    fdk_ext_surface_kind *kind; /* NULL = FDK-owned (popup)       */
    fdk_ext_event_fn event_cb;
    void *event_cb_user;
};

/* ---- trampoline: events reach the add-on --------------------------------
 *
 * Installed as the internal window's event_callback at creation;
 * fdk_window_dispatch_event invokes it with the fully-translated,
 * backend-neutral event stream, exactly what an application's window
 * callback receives. */
static void ext_event_trampoline(fdk_window *window,
                                 const fdk_event_data *event,
                                 void *user_data) {
    (void)window; /* the public handle is the fdk_ext_surface */
    fdk_ext_surface *ext = user_data;
    if (ext == NULL || ext->event_cb == NULL) {
        return;
    }
    ext->event_cb(ext, event, ext->event_cb_user);
}

/* ---- destroy notification: every destroy path frees the wrapper ---------
 *
 * fdk_window_destroy calls this FIRST, while the window is whole —
 * which is exactly the ordering the extension contract promises: the
 * kind's role teardown runs against the still-live native surface,
 * then FDK destroys that surface (window_destroy's own tail). Every
 * path inherits it: the add-on's fdk_ext_surface_destroy, a parent's
 * popup sweep, fdk_shutdown's leaked-window sweep. */
static void ext_destroy_notify(fdk_window *window, void *user) {
    fdk_ext_surface *ext = user;
    if (ext == NULL) {
        return;
    }
    if (ext->kind != NULL) {
        fdk_ext_surface_kind *kind = ext->kind;
        kind->ops.destroy(ext, kind->user);
        if (kind->live_surfaces > 0) {
            kind->live_surfaces--;
        }
    }
    fdk_free(ext);
    window->ext = NULL;
}

/* ---- API version / backend / capabilities ------------------------------- */

int fdk_ext_api_version(void) {
    return FDK_EXT_API_VERSION;
}

fdk_ext_backend fdk_ext_active_backend(fdk_context *ctx) {
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_EXT_BACKEND_NONE;
    }
    /* Identity against the backend factories — the same comparison
     * select_and_connect performed when it stored ctx->ops. */
    if (ctx->ops == fdk_platform_x11_ops()) {
        return FDK_EXT_BACKEND_X11;
    }
    if (ctx->ops == fdk_platform_wayland_ops()) {
        return FDK_EXT_BACKEND_WAYLAND;
    }
    return FDK_EXT_BACKEND_NONE;
}

int fdk_ext_has_capability(fdk_context *ctx, fdk_ext_capability cap) {
    if (ctx == NULL || ctx->ops == NULL) {
        return 0;
    }
    const fdk_platform_ops *ops = ctx->ops;
    switch (cap) {
        case FDK_EXT_CAP_SHM_RENDER:
            /* The software-render path exists for extension surfaces
             * iff it exists at all on this backend (both current
             * backends: the adopt op wraps the same render ops). */
            return ops->ext_surface_adopt != NULL &&
                   ops->window_get_framebuffer != NULL;
        case FDK_EXT_CAP_POPUP_GRAB:
            /* X11: the server-side pointer+keyboard grab is always
             * available. Wayland: xdg_popup.grab (serial-0 caveat
             * documented in fdk_extend.h). */
            return ops->ext_surface_adopt != NULL;
        case FDK_EXT_CAP_EWMH_DOCK:
            /* Dock hints are X11-only truth: on Wayland, layering is
             * the add-on's own protocol negotiation, which FDK cannot
             * vouch for. */
            return fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_X11;
        case FDK_EXT_CAP_FRAME_PACING:
            /* Compositor-driven pacing = the frame_ready op exists
             * (Wayland); X11's always-ready semantics report false —
             * there is nothing to pace against. */
            return ops->window_frame_ready != NULL;
        default:
            return 0; /* unknown capability: honestly absent */
    }
}

fdk_result fdk_ext_get_native_display(fdk_context *ctx,
                                      fdk_ext_native_display *out) {
    if (ctx == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->ops == NULL || ctx->conn == NULL ||
        ctx->ops->native_display == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    void *display = ctx->ops->native_display(ctx->conn);
    if (display == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    out->backend = fdk_ext_active_backend(ctx);
    out->display = display;
    return FDK_OK;
}

/* ---- kind registration --------------------------------------------------- */

fdk_result fdk_ext_register_kind(fdk_context *ctx, const char *name,
                                 const fdk_ext_surface_ops *ops, void *user,
                                 fdk_ext_surface_kind **out_kind) {
    if (ctx == NULL || name == NULL || ops == NULL || out_kind == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (name[0] == '\0') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ops->struct_size < offsetof(fdk_ext_surface_ops, create) +
                                sizeof(fdk_result (*)(void))) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ops->create == NULL) {
        /* create is the one required callback: without it there is
         * no way to build the native surface. destroy and
         * popup_attached may legitimately be NULL. */
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Duplicate-name check: kinds are diagnosable by name, and a
     * silent second registration of "fdk-shell.layer" would hide a
     * library double-init. Re-registration AFTER a destroy is legal
     * (the old kind is gone from the registry). */
    fdk_ext_surface_kind *scan = ctx->ext_kinds_head;
    while (scan != NULL) {
        if (strcmp(scan->name, name) == 0) {
            FDK_WARN("extension kind '%s' already registered on this "
                     "context",
                     name);
            return FDK_ERR_INVALID_ARGUMENT;
        }
        scan = scan->next;
    }

    fdk_ext_surface_kind *kind = fdk_alloc(sizeof(*kind));
    if (kind == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t name_len = strlen(name) + 1;
    kind->name = fdk_alloc(name_len);
    if (kind->name == NULL) {
        fdk_free(kind);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(kind->name, name, name_len);

    /* Copy EXACTLY struct_size bytes (zeroed first): an add-on
     * compiled against an older FDK leaves the newer tail fields
     * zero/NULL, and this file reads them only through the fields it
     * knows — the append-only contract both sides can rely on. */
    memset(&kind->ops, 0, sizeof(kind->ops));
    memcpy(&kind->ops, ops,
           (ops->struct_size < sizeof(kind->ops)) ? ops->struct_size
                                                  : sizeof(kind->ops));
    kind->ops.struct_size = ops->struct_size;
    kind->user = user;
    kind->live_surfaces = 0;
    kind->ctx = ctx;
    kind->next = ctx->ext_kinds_head;
    ctx->ext_kinds_head = kind;

    FDK_DEBUG("extension kind '%s' registered", name);
    *out_kind = kind;
    return FDK_OK;
}

fdk_result fdk_ext_kind_destroy(fdk_ext_surface_kind *kind) {
    if (kind == NULL) {
        return FDK_OK;
    }
    if (kind->live_surfaces > 0) {
        FDK_WARN("kind '%s' still has %zu live surface(s) — destroy "
                 "them first",
                 kind->name, kind->live_surfaces);
        return FDK_ERR_INVALID_STATE;
    }
    /* Unlink from the context's registry (head or middle). */
    fdk_context *ctx = kind->ctx;
    if (ctx != NULL) {
        if (ctx->ext_kinds_head == kind) {
            ctx->ext_kinds_head = kind->next;
        } else {
            fdk_ext_surface_kind *scan = ctx->ext_kinds_head;
            while (scan != NULL && scan->next != kind) {
                scan = scan->next;
            }
            if (scan != NULL) {
                scan->next = kind->next;
            }
        }
    }
    fdk_free(kind->name);
    fdk_free(kind);
    return FDK_OK;
}

/* ---- surface creation ---------------------------------------------------- */

fdk_result fdk_ext_surface_create(fdk_ext_surface_kind *kind,
                                  fdk_ext_surface **out_surface) {
    if (kind == NULL || out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_context *ctx = kind->ctx;
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops->ext_surface_adopt == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_ext_surface *ext = fdk_alloc(sizeof(*ext));
    if (ext == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    ext->window = NULL;
    ext->kind = kind;
    ext->event_cb = NULL;
    ext->event_cb_user = NULL;

    /* The add-on builds the native surface + its role. Inside it,
     * fdk_ext_surface_context() must resolve — so ext->window is set
     * AFTER the callback returns; the accessor special-cases the
     * creation window via the kind's context (see its body). */
    fdk_ext_native_handle native;
    memset(&native, 0, sizeof(native));
    fdk_result r = kind->ops.create(ext, &native, kind->user);
    if (!fdk_ok(r)) {
        /* The add-on cleaned up after itself (its contract). The
         * wrapper dies; nothing was adopted. */
        fdk_free(ext);
        return r;
    }

    /* Adopt the native surface (ownership per fdk_extend.h: on every
     * adopt failure from here the NATIVE SURFACE IS GONE — the
     * backend destroyed it; the kind's role teardown must still run,
     * which is why destroy is called before re-checking). */
    fdk_platform_window *pwindow = NULL;
    r = ctx->ops->ext_surface_adopt(ctx->conn, &native, &pwindow);
    if (!fdk_ok(r)) {
        kind->ops.destroy(ext, kind->user);
        fdk_free(ext);
        return r;
    }

    /* Build the internal window wrapper — the same fields
     * window_create_full initializes (fdk__window_init_fields),
     * around the adopted platform window. */
    fdk_window *window = fdk_alloc(sizeof(*window));
    if (window == NULL) {
        /* The native was adopted — ownership says it dies here: the
         * backend's window_destroy is the sanctioned destroyer, and
         * the kind's role teardown runs FIRST against the still-live
         * surface (the same ordering every other destroy path
         * takes). */
        kind->ops.destroy(ext, kind->user);
        ctx->ops->window_destroy(pwindow);
        fdk_free(ext);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk__window_init_fields(window, ctx, NULL);
    window->pwindow = pwindow;
    window->ext = ext;
    ext->window = window;

    /* Route events + teardown through the trampolines. */
    window->event_callback = ext_event_trampoline;
    window->event_callback_user_data = ext;
    fdk__window_set_destroy_notify(window, ext_destroy_notify, ext);

    r = fdk_context_register_window(ctx, window);
    if (!fdk_ok(r)) {
        /* ext_destroy_notify runs inside (kind teardown + wrapper
         * free), window_destroy releases the platform window. */
        fdk_window_destroy(window);
        return r;
    }

    kind->live_surfaces++;
    FDK_DEBUG("extension surface created (kind '%s')", kind->name);
    *out_surface = ext;
    return FDK_OK;
}

void fdk_ext_surface_destroy(fdk_ext_surface *surface) {
    if (surface == NULL) {
        return;
    }
    /* ext_destroy_notify (installed on the window) performs the kind
     * teardown and frees the wrapper; fdk_window_destroy then
     * releases the native surface through the backend op. Every
     * other destroy path (popup sweep, shutdown sweep) already runs
     * through this same machinery. */
    fdk_window_destroy(surface->window);
}

fdk_context *fdk_ext_surface_context(fdk_ext_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    if (surface->window != NULL) {
        return surface->window->ctx;
    }
    /* Creation window: the kind's create callback is running RIGHT
     * NOW (the window does not exist yet) — the kind knows the
     * context. */
    return (surface->kind != NULL) ? surface->kind->ctx : NULL;
}

fdk_result fdk_ext_surface_native(fdk_ext_surface *surface,
                                  fdk_ext_native_handle *out) {
    if (surface == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->window == NULL) {
        return FDK_ERR_INVALID_STATE; /* inside kind->create */
    }
    const fdk_platform_ops *ops = surface->window->ops;
    if (ops == NULL || ops->ext_surface_native == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* The backend writes the tagged handle (X11: the Window XID;
     * Wayland: the wl_surface*) — the window layer never touches a
     * pwindow itself (the one-way-opaque discipline). */
    return ops->ext_surface_native(surface->window->pwindow, out);
}

fdk_result fdk_ext_surface_get_surface(fdk_ext_surface *surface,
                                       fdk_surface **out) {
    if (surface == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    /* The PUBLIC render path — the same fdk_surface an application's
     * window owns (lazily created, destroyed with the window, never
     * fdk_surface_destroy'd by the add-on). */
    return fdk_window_get_surface(surface->window, out);
}

void fdk_ext_surface_set_cursor(fdk_ext_surface *surface, int shape) {
    if (surface == NULL) {
        return;
    }
    /* The internal window wrapper carries the shape exactly like an
     * application window's would; the plain int IS fdk_cursor_shape's
     * values (the header keeps the dependency-free signature). */
    fdk_window_set_cursor(surface->window, (fdk_cursor_shape)shape);
}

fdk_result fdk_ext_surface_set_input_region(fdk_ext_surface *surface,
                                            const fdk_rect *rects,
                                            fdk_i32 count) {
    if (surface == NULL || surface->window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* The window layer owns validation + the op dispatch; the
     * extension surface's internal window wrapper carries the region
     * exactly like an application window's would. */
    return fdk_window_set_input_region(surface->window, rects, count);
}

void fdk_ext_surface_show(fdk_ext_surface *surface) {
    if (surface == NULL || surface->window == NULL) {
        return;
    }
    surface->window->ops->window_show(surface->window->pwindow);
}

void fdk_ext_surface_hide(fdk_ext_surface *surface) {
    if (surface == NULL || surface->window == NULL) {
        return;
    }
    surface->window->ops->window_hide(surface->window->pwindow);
}

fdk_result fdk_ext_surface_get_size(const fdk_ext_surface *surface,
                                    fdk_size *out_size) {
    if (surface == NULL || out_size == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    return fdk_window_get_size(surface->window, out_size);
}

fdk_result fdk_ext_surface_notify_configure(fdk_ext_surface *surface,
                                            fdk_i32 width, fdk_i32 height) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    /* The backend's buffer-side bookkeeping learns the size FIRST
     * (the same field a backend's own configure path updates), so
     * the framebuffer re-acquisition the dispatched event triggers
     * sizes at the reported geometry. */
    if (surface->window->ops->ext_surface_resized != NULL) {
        surface->window->ops->ext_surface_resized(
            surface->window->pwindow, width, height);
    }
    /* The same dispatch a backend's configure event takes — through
     * fdk_window_dispatch_event, so the size cache, the framebuffer
     * drop, and the add-on's event callback all see one consistent
     * configure. */
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_WINDOW_CONFIGURE;
    event.configure.size.width = width;
    event.configure.size.height = height;
    fdk_window_dispatch_event(surface->window, &event);
    return FDK_OK;
}

void fdk_ext_surface_set_event_callback(fdk_ext_surface *surface,
                                        fdk_ext_event_fn callback,
                                        void *user) {
    if (surface == NULL) {
        return;
    }
    surface->event_cb = callback;
    surface->event_cb_user = user;
}

/* ---- popups --------------------------------------------------------------- */

fdk_result fdk_ext_popup_create(fdk_ext_surface *parent, fdk_i32 x,
                                fdk_i32 y, fdk_i32 width, fdk_i32 height,
                                fdk_ext_surface **out_surface) {
    if (parent == NULL || out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (parent->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    fdk_context *ctx = parent->window->ctx;
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    /* The popup is created through the SAME platform path
     * fdk_window_create_popup takes — parent-relative positioning,
     * the input grab, outside-click/Esc dismissal — with the
     * extension surface's platform window as the parent. On Wayland
     * the backend's popup branch notices the ext parent and routes
     * the fresh xdg_popup through the kind's popup_attached hook. */
    fdk_window *popup = NULL;
    fdk_result r = fdk__window_create_popup_ex(
        ctx, parent->window, x, y, width, height, false, &popup);
    if (!fdk_ok(r)) {
        return r;
    }

    /* Wrap it as an FDK-OWNED extension surface (kind == NULL: no
     * kind callbacks fire; the standard popup teardown is all there
     * is) so the add-on renders and receives events uniformly. */
    fdk_ext_surface *ext = fdk_alloc(sizeof(*ext));
    if (ext == NULL) {
        fdk_window_destroy(popup);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    ext->window = popup;
    ext->kind = NULL;
    ext->event_cb = NULL;
    ext->event_cb_user = NULL;
    popup->ext = ext;

    /* The popup keeps window_create's own event callback slot —
     * route it through the same trampoline. (Its destroy_notify was
     * NOT set by create: install ours so the wrapper frees on every
     * destroy path, exactly like kind surfaces minus the kind.) */
    popup->event_callback = ext_event_trampoline;
    popup->event_callback_user_data = ext;
    fdk__window_set_destroy_notify(popup, ext_destroy_notify, ext);

    *out_surface = ext;
    return FDK_OK;
}

/* ---- subsurfaces (extension API v2, 1.1.0 D-2) ---------------------------
 *
 * The FDK-owned child-surface primitive (fdk_extend.h's Subsurfaces
 * section). The create flow mirrors fdk_ext_popup_create's wrapper
 * pattern exactly — the backend's subsurface_create op builds the
 * native child (X11 child window / wl_subsurface), the window layer
 * wraps it in an fdk_window with the event trampoline and destroy
 * notify, and links it onto the parent's child list so the parent's
 * destroy sweeps it (the popup contract, the sub_* fields). */

static fdk_result subsurface_create_full(fdk_context *ctx,
                                         fdk_window *parent, fdk_i32 x,
                                         fdk_i32 y, fdk_i32 width,
                                         fdk_i32 height,
                                         fdk_ext_surface **out_surface) {
    if (out_surface == NULL || parent == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx == NULL || ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ctx->ops->subsurface_create == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_ext_surface *ext = fdk_alloc(sizeof(*ext));
    if (ext == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    ext->window = NULL;
    ext->kind = NULL; /* FDK-owned: no kind callbacks ever fire   */
    ext->event_cb = NULL;
    ext->event_cb_user = NULL;

    /* The backend builds the native child parented to the PARENT's
     * platform window (the tagged-native indirection the kind path
     * needs does not apply — FDK creates this surface itself). */
    fdk_platform_window *pwindow = NULL;
    fdk_result r = ctx->ops->subsurface_create(ctx->conn, parent->pwindow,
                                               x, y, width, height,
                                               &pwindow);
    if (!fdk_ok(r)) {
        fdk_free(ext);
        return r;
    }

    fdk_window *window = fdk_alloc(sizeof(*window));
    if (window == NULL) {
        /* The native child was created — ownership says it dies here
         * (the backend's window_destroy is the sanctioned destroyer,
         * the same ordering every other create-failure path takes). */
        ctx->ops->window_destroy(pwindow);
        fdk_free(ext);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk__window_init_fields(window, ctx, NULL);
    window->pwindow = pwindow;
    window->ext = ext;
    window->is_subsurface = true;
    /* The client-chosen size (window_create_full's options->width
     * equivalent — the ext wrapper has no options, and get_size
     * answers the window layer's last_size). */
    window->last_size.width = width;
    window->last_size.height = height;
    ext->window = window;

    window->event_callback = ext_event_trampoline;
    window->event_callback_user_data = ext;
    fdk__window_set_destroy_notify(window, ext_destroy_notify, ext);

    r = fdk_context_register_window(ctx, window);
    if (!fdk_ok(r)) {
        /* ext_destroy_notify runs inside (wrapper free — no kind),
         * window_destroy releases the platform child. */
        fdk_window_destroy(window);
        return r;
    }

    /* The child list: the parent's destroy sweeps children first
     * (window.c), and place_above/below validates same-parent
     * siblings through these links. */
    window->sub_parent = parent;
    window->sub_prev = NULL;
    window->sub_next = parent->sub_first;
    if (parent->sub_first != NULL) {
        parent->sub_first->sub_prev = window;
    }
    parent->sub_first = window;

    FDK_DEBUG("subsurface extension surface created");
    *out_surface = ext;
    return FDK_OK;
}

fdk_result fdk_ext_subsurface_create(fdk_window *parent, fdk_i32 x,
                                     fdk_i32 y, fdk_i32 width,
                                     fdk_i32 height,
                                     fdk_ext_surface **out_surface) {
    if (parent == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    return subsurface_create_full(parent->ctx, parent, x, y, width, height,
                                  out_surface);
}

fdk_result fdk_ext_subsurface_create_ex(fdk_ext_surface *parent,
                                        fdk_i32 x, fdk_i32 y,
                                        fdk_i32 width, fdk_i32 height,
                                        fdk_ext_surface **out_surface) {
    if (parent == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (parent->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    return subsurface_create_full(parent->window->ctx, parent->window, x, y,
                                  width, height, out_surface);
}

/* The subsurface op dispatch shape shared by every mutator below:
 * validate the handle, refuse anything that is not a subsurface
 * (INVALID_STATE — the handle is real but the operation belongs to
 * another kind of surface), then let a backend without the op
 * answer UNSUPPORTED. */
static fdk_result subsurface_dispatch(fdk_ext_surface *surface,
                                      fdk_window **out_window) {
    if (surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (surface->window == NULL) {
        return FDK_ERR_INVALID_STATE;
    }
    if (!surface->window->is_subsurface) {
        return FDK_ERR_INVALID_STATE;
    }
    *out_window = surface->window;
    return FDK_OK;
}

fdk_result fdk_ext_subsurface_set_position(fdk_ext_surface *surface,
                                           fdk_i32 x, fdk_i32 y) {
    fdk_window *window = NULL;
    fdk_result r = subsurface_dispatch(surface, &window);
    if (!fdk_ok(r)) {
        return r;
    }
    if (window->ops->subsurface_set_position == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    window->ops->subsurface_set_position(window->pwindow, x, y);
    return FDK_OK;
}

fdk_result fdk_ext_subsurface_resize(fdk_ext_surface *surface,
                                     fdk_i32 width, fdk_i32 height) {
    fdk_window *window = NULL;
    fdk_result r = subsurface_dispatch(surface, &window);
    if (!fdk_ok(r)) {
        return r;
    }
    if (width <= 0 || height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->subsurface_resize == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    window->ops->subsurface_resize(window->pwindow, width, height);
    /* The resize is a client-owned fact on both backends — no server
     * configure will arrive for the Wayland child (and X11's own
     * ConfigureNotify races this dispatch), so the surface layer
     * dispatches the CONFIGURE itself: the documented event the
     * child's callback repaints on. */
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_WINDOW_CONFIGURE;
    event.configure.size.width = width;
    event.configure.size.height = height;
    fdk_window_dispatch_event(window, &event);
    return FDK_OK;
}

fdk_result fdk_ext_subsurface_set_desync(fdk_ext_surface *surface,
                                         int desync) {
    fdk_window *window = NULL;
    fdk_result r = subsurface_dispatch(surface, &window);
    if (!fdk_ok(r)) {
        return r;
    }
    if (window->ops->subsurface_set_desync == NULL) {
        /* No commit transaction exists — the documented hint-answer
         * (X11 declares the op, so this arm is for future backends). */
        return FDK_OK;
    }
    window->ops->subsurface_set_desync(window->pwindow, desync);
    return FDK_OK;
}

static fdk_result subsurface_place(fdk_ext_surface *surface,
                                   fdk_ext_surface *sibling, int above) {
    fdk_window *window = NULL;
    fdk_result r = subsurface_dispatch(surface, &window);
    if (!fdk_ok(r)) {
        return r;
    }
    if (sibling == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (sibling->window == NULL || !sibling->window->is_subsurface) {
        return FDK_ERR_INVALID_STATE;
    }
    if (sibling->window == window) {
        return FDK_ERR_INVALID_ARGUMENT; /* self-relative is meaningless */
    }
    if (sibling->window->sub_parent != window->sub_parent) {
        /* The protocol's own rule (both backends): stack siblings
         * must share a parent. */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->subsurface_place == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->subsurface_place(window->pwindow,
                                         sibling->window->pwindow, above);
}

fdk_result fdk_ext_subsurface_place_above(fdk_ext_surface *surface,
                                          fdk_ext_surface *sibling) {
    return subsurface_place(surface, sibling, 1);
}

fdk_result fdk_ext_subsurface_place_below(fdk_ext_surface *surface,
                                          fdk_ext_surface *sibling) {
    return subsurface_place(surface, sibling, 0);
}

/* ---- the backend hook (platform_internal.h) ------------------------------ */

void fdk__ext_popup_attached(fdk_context *ctx,
                             fdk_platform_window *parent_pwindow,
                             void *native_popup) {
    if (ctx == NULL || parent_pwindow == NULL || native_popup == NULL) {
        return;
    }
    /* Resolve the parent through the shared window registry: the
     * backend passed its connect()-time dispatch_user_data (the
     * context) because the seam only flows one way — the registry
     * lookup is the sanctioned crossing back. */
    fdk_window *parent =
        fdk_context_find_window_by_pwindow(ctx, parent_pwindow);
    if (parent == NULL || parent->ext == NULL) {
        return; /* not an extension surface: nothing to adopt */
    }
    fdk_ext_surface *ext = parent->ext;
    if (ext->kind == NULL || ext->kind->ops.popup_attached == NULL) {
        /* FDK-owned (popup) or a kind that is not a popup parent:
         * the popup stands on its grab alone — documented. */
        return;
    }
    ext->kind->ops.popup_attached(ext, native_popup, ext->kind->user);
}

/* ---- shutdown detach (window_internal.h) ---------------------------------
 *
 * Kinds are add-on owned; the context only links them. At shutdown
 * every extension surface has already been destroyed (the leaked-
 * window sweep ran first), so live_surfaces is 0 everywhere — but
 * the kind objects themselves stay alive until the add-on destroys
 * them. Detaching (ctx := NULL) keeps a late fdk_ext_kind_destroy
 * from touching freed context memory. */
void fdk__ext_kinds_detach(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }
    fdk_ext_surface_kind *kind = ctx->ext_kinds_head;
    while (kind != NULL) {
        fdk_ext_surface_kind *next = kind->next;
        kind->ctx = NULL;
        kind->next = NULL;
        if (kind->live_surfaces != 0) {
            /* Impossible (the sweep destroyed them first) — but this
             * is exactly where a corrupted registry would surface,
             * so say it instead of silently continuing. */
            FDK_WARN("kind '%s' kept %zu surface(s) through shutdown",
                     kind->name, kind->live_surfaces);
        }
        kind = next;
    }
    ctx->ext_kinds_head = NULL;
}
