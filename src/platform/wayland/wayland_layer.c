/*
 * wayland_layer.c — the first-class layer-shell role (1.0.0, WI4)
 *
 * fdk_extend.h's layer-shell section, implemented on the extension-
 * surface machinery: FDK registers ONE internal kind
 * ("fdk-layer-shell-v1") per connection, lazily, on the first
 * fdk_ext_layer_surface_create. The kind's create callback does the
 * protocol work through the same seam an add-on uses (the documented
 * native accessors plus its own role binding), the returned wl_surface
 * is adopted by the standard path, and the role's configure events
 * bridge into the ordinary extension-surface flow through
 * fdk_ext_surface_notify_configure() — the present gate applies
 * verbatim (an eager painter cannot trip the never-configured
 * protocol error; the same enforcement add-on roles get).
 *
 * Per-surface role state rides a small list keyed by the ext surface
 * (the kind callback signature carries only kind-level user data —
 * the same O(n)-is-fine stance as the registries). The role object is
 * destroyed by the kind's destroy callback, which FDK calls BEFORE
 * the native wl_surface dies — the ownership contract of
 * fdk_extend.h's header comment, followed literally.
 *
 * zwlr_layer_shell_v1 is bound eagerly at connect (wayland_connection.c);
 * absent compositors answer FDK_ERR_UNSUPPORTED here and the
 * capability reports false — the honest refusal, never a faked role.
 */

#define FDK_LOG_TAG "wayland-layer"

#include "fdk/fdk_extend.h"
#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "platform/wayland/wayland_platform.h"

#include <string.h>

/* The protocol headers (generated, checked in). */
#include "generated/wlr-layer-shell-unstable-v1-client-protocol.h"

/* ---- per-surface role state -------------------------------------------- */

typedef struct layer_state {
    fdk_ext_surface *surface;      /* lifecycle key  */
    struct wl_surface *wls;        /* setter key (the pwindow's) */
    struct zwlr_layer_surface_v1 *role;
    struct wl_output *output; /* borrowed (an FDK output object) */
    struct layer_state *next;
} layer_state;

static layer_state *layer_states = NULL; /* UI-thread-only, like all of it */

static layer_state *layer_state_find(fdk_ext_surface *surface) {
    for (layer_state *st = layer_states; st != NULL; st = st->next) {
        if (st->surface == surface) {
            return st;
        }
    }
    return NULL;
}

static layer_state *layer_state_find_by_wls(struct wl_surface *wls) {
    for (layer_state *st = layer_states; st != NULL; st = st->next) {
        if (st->wls == wls) {
            return st;
        }
    }
    return NULL;
}

static void layer_state_add(layer_state *st) {
    st->next = layer_states;
    layer_states = st;
}

static void layer_state_remove(layer_state *st) {
    layer_state **p = &layer_states;
    while (*p != NULL && *p != st) {
        p = &(*p)->next;
    }
    if (*p == st) {
        *p = st->next;
    }
}

/* ---- the role's listener ------------------------------------------------- */

static void layer_surface_configure(void *data,
                                    struct zwlr_layer_surface_v1 *role,
                                    uint32_t serial, uint32_t width,
                                    uint32_t height) {
    layer_state *st = data;
    (void)role;
    /* The ext machinery's configure flow: record + emit; the present
     * gate defers commits until this ran (the never-configured
     * protocol error is enforced BY FDK, add-on roles and first-class
     * roles alike). Acking: zwlr's ack_configure — the role's own
     * handshake; FDK acks HERE so the add-on's next present carries a
     * legal state (the same discipline the toplevel path follows). */
    zwlr_layer_surface_v1_ack_configure(role, serial);
    if (width > 0 && height > 0 && width <= 0x7fffffff &&
        height <= 0x7fffffff) {
        fdk_ext_surface_notify_configure(st->surface, (fdk_i32)width,
                                         (fdk_i32)height);
    }
}

static void layer_surface_closed(void *data,
                                 struct zwlr_layer_surface_v1 *role) {
    layer_state *st = data;
    (void)role;
    /* The compositor retracted the layer: the surface is gone from
     * the desktop's point of view. Destroy it — the add-on's event
     * stream ends (fdk_ext_surface_destroy is the honest signal; the
     * role teardown runs against the live surface per the contract). */
    fdk_ext_surface_destroy(st->surface);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

/* ---- the internal kind --------------------------------------------------- */

static void layer_kind_create_apply(const fdk_ext_layer_options *o,
                                    struct zwlr_layer_surface_v1 *role) {
    zwlr_layer_surface_v1_set_layer(
        role, (o->layer >= FDK_LAYER_BACKGROUND &&
               o->layer <= FDK_LAYER_OVERLAY)
                  ? (uint32_t)o->layer
                  : (uint32_t)FDK_LAYER_TOP);
    zwlr_layer_surface_v1_set_anchor(role, o->anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(role, o->exclusive_zone);
    zwlr_layer_surface_v1_set_margin(role, o->margin_top, o->margin_bottom,
                                     o->margin_left, o->margin_right);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        role, (o->keyboard >= FDK_LAYER_KB_NONE &&
               o->keyboard <= FDK_LAYER_KB_ON_DEMAND)
                  ? (uint32_t)o->keyboard
                  : (uint32_t)FDK_LAYER_KB_NONE);
}

static fdk_result layer_kind_create(fdk_ext_surface *surface,
                                    fdk_ext_native_handle *out_native,
                                    void *user) {
    fdk_platform_connection *conn = user;
    const fdk_ext_layer_options *o = conn->pending_layer_opts;
    conn->pending_layer_opts = NULL;
    if (o == NULL || conn->compositor == NULL || conn->layer_shell == NULL) {
        return FDK_ERR_INVALID_STATE;
    }

    struct wl_surface *wls = wl_compositor_create_surface(conn->compositor);
    if (wls == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* An FDK output, if the caller pinned one through the margin of
     * the API (v3 keeps the compositor's pick: NULL output). */
    struct wl_output *output = NULL;

    struct zwlr_layer_surface_v1 *role = zwlr_layer_shell_v1_get_layer_surface(
        conn->layer_shell, wls, output,
        (o->layer >= FDK_LAYER_BACKGROUND && o->layer <= FDK_LAYER_OVERLAY)
            ? (uint32_t)o->layer
            : (uint32_t)FDK_LAYER_TOP,
        (o->namespace_ != NULL) ? o->namespace_ : "fdk");
    if (role == NULL) {
        wl_surface_destroy(wls);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    layer_kind_create_apply(o, role);

    layer_state *st = fdk_alloc(sizeof *st);
    if (st == NULL) {
        zwlr_layer_surface_v1_destroy(role);
        wl_surface_destroy(wls);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    st->surface = surface;
    st->wls = wls;
    st->role = role;
    st->output = output;
    zwlr_layer_surface_v1_add_listener(role, &layer_surface_listener, st);
    layer_state_add(st);

    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = wls;
    return FDK_OK;
}

static void layer_kind_destroy(fdk_ext_surface *surface, void *user) {
    (void)user;
    layer_state *st = layer_state_find(surface);
    if (st == NULL) {
        return;
    }
    layer_state_remove(st);
    if (st->role != NULL) {
        zwlr_layer_surface_v1_destroy(st->role);
    }
    fdk_free(st);
}

static const fdk_ext_surface_ops layer_kind_ops = {
    .struct_size = sizeof(fdk_ext_surface_ops),
    .create = layer_kind_create,
    .destroy = layer_kind_destroy,
    .popup_attached = NULL,
};

/* ---- the public entry points ---------------------------------------------
 *
 * fdk_ext_layer_surface_create() and the setters dispatch through the
 * platform ops table (src/window/extend.c) — the Wayland module is
 * optional at build time, so the core never calls it directly. These
 * are the op implementations.
 */

int fdk_wayland_layer_shell_available(fdk_platform_connection *conn) {
    return conn != NULL && conn->layer_shell != NULL;
}

fdk_result fdk_wayland_layer_surface_create(
    fdk_context *ctx, fdk_platform_connection *conn,
    const fdk_ext_layer_options *options, fdk_ext_surface **out_surface) {
    if (ctx == NULL || conn == NULL || options == NULL ||
        out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->layer_shell == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (conn->layer_kind == NULL) {
        fdk_result r = fdk_ext_register_kind(ctx, "fdk-layer-shell-v1",
                                             &layer_kind_ops, conn,
                                             &conn->layer_kind);
        if (!fdk_ok(r)) {
            return r;
        }
    }
    conn->pending_layer_opts = options;
    fdk_result r = fdk_ext_surface_create(conn->layer_kind, out_surface);
    conn->pending_layer_opts = NULL; /* cleared even on failure */
    return r;
}

/* The setters route through ONE op with a selector. */
enum {
    FDK_WL_LAYER_SET_LAYER = 1,
    FDK_WL_LAYER_SET_ANCHOR = 2,
    FDK_WL_LAYER_SET_EXCLUSIVE = 3,
    FDK_WL_LAYER_SET_MARGINS = 4,
    FDK_WL_LAYER_SET_KEYBOARD = 5,
};

fdk_result fdk_wayland_layer_surface_set(fdk_platform_window *pwindow,
                                         int what, fdk_i32 a, fdk_i32 b,
                                         fdk_i32 c, fdk_i32 d) {
    if (pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    layer_state *st =
        layer_state_find_by_wls(pwindow->surface);
    if (st == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* not a layer surface */
    }
    switch (what) {
        case FDK_WL_LAYER_SET_LAYER:
            zwlr_layer_surface_v1_set_layer(st->role, (uint32_t)a);
            return FDK_OK;
        case FDK_WL_LAYER_SET_ANCHOR:
            zwlr_layer_surface_v1_set_anchor(st->role, (uint32_t)a);
            return FDK_OK;
        case FDK_WL_LAYER_SET_EXCLUSIVE:
            zwlr_layer_surface_v1_set_exclusive_zone(st->role, a);
            return FDK_OK;
        case FDK_WL_LAYER_SET_MARGINS:
            zwlr_layer_surface_v1_set_margin(st->role, a, b, c, d);
            return FDK_OK;
        case FDK_WL_LAYER_SET_KEYBOARD:
            zwlr_layer_surface_v1_set_keyboard_interactivity(st->role,
                                                             (uint32_t)a);
            return FDK_OK;
        default:
            return FDK_ERR_INVALID_ARGUMENT;
    }
}
