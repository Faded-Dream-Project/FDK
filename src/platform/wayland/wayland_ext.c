/*
 * wayland_ext.c — Wayland extension-surface adoption (1.1.0)
 *
 * The Wayland half of include/fdk/fdk_extend.h's ext_surface_adopt
 * platform op: an add-on created a wl_surface through the wl_display*
 * the extension API handed out and gave it whatever protocol role it
 * speaks (a zwlr_layer_surface_v1, for fdk-shell) — FDK now wraps
 * that surface so it participates in everything a normal FDK window
 * gets: seat event routing (wayland_seat.c resolves wl_surface ->
 * pwindow through the registry this adopt joins), the recycled
 * wl_shm render slots, damage-tracked presents with frame callbacks,
 * and popup parenting (the add-on's role adopts the xdg_popup via
 * the kind's popup_attached hook — wayland_window.c's popup branch).
 *
 * Ownership (fdk_extend.h's lifetime block): from a successful adopt,
 * the wl_surface belongs to FDK — window_destroy wl_surface_destroy's
 * it. The add-on's ROLE objects are torn down by the kind's destroy
 * callback BEFORE that, against a live surface.
 *
 * The xdg-shell machinery is deliberately absent here: xdg_surface,
 * xdg_toplevel, xdg_popup all stay NULL, and the ext marker
 * (wayland_platform.h) bypasses every gate that assumes an xdg
 * handshake — the show()/hide() commit pair, the toplevel resize and
 * size-limit requests. The add-on's own role handshake owns mapping
 * and size: it reports the role's configure through
 * fdk_ext_surface_notify_configure(), which (wayland_ext.c below)
 * flips `configured` and commits any buffer whose present was
 * deferred by the pre-configure gate — the layer-shell rule "wait
 * for the initial configure before committing" is enforced by FDK's
 * present() for EVERY surface it renders, add-on or not.
 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include "generated/viewporter-client-protocol.h"
#include "generated/fractional-scale-v1-client-protocol.h"

#include <string.h>

fdk_result fdk_wayland_ext_surface_adopt(fdk_platform_connection *conn,
                                         const fdk_ext_native_handle *native,
                                         fdk_platform_window **out_pwindow) {
    if (conn == NULL || native == NULL || out_pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (native->backend != FDK_EXT_BACKEND_WAYLAND ||
        native->pointer == NULL) {
        FDK_WARN("ext_surface_adopt: not a Wayland surface handle");
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->display_dead) {
        return FDK_ERR_NO_DISPLAY;
    }

    struct wl_surface *surface = (struct wl_surface *)native->pointer;

    /* Ownership transfers HERE (fdk_extend.h's lifetime block): every
     * failure path below destroys the wl_surface before returning,
     * so the caller can treat any adopt failure as "the native
     * surface is gone" — after it called the kind's destroy callback
     * for the role teardown (the ROLE objects die first, against the
     * still-live surface, exactly like the normal destroy path). */
    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        wl_surface_destroy(surface);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* The same field set window_create initializes (mirrored
     * deliberately rather than shared — the create path's versions
     * carry xdg-specific meaning this wrapper must NOT have). */
    pwindow->conn = conn;
    pwindow->surface = surface;
    pwindow->xdg_surface = NULL; /* no xdg role — the add-on's role  */
    pwindow->xdg_toplevel = NULL;
    pwindow->ext = 1;
    pwindow->popup = 0;
    pwindow->xdg_popup = NULL;
    pwindow->input_transparent = 0;
    pwindow->drop_formats = 0;
    pwindow->last_size.width = 1; /* unknown until the add-on reports
                                   * its role's configure (never 0: the
                                   * buffer math divides/multiplies) */
    pwindow->last_size.height = 1;
    pwindow->pending_size = pwindow->last_size;
    pwindow->configured = 0; /* the pre-configure gate applies to ext
                              * surfaces too — flipped by the resized
                              * op (notify_configure) below */
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->fullscreen = 0;
    pwindow->toplevel_decoration = NULL;
    pwindow->deco_client_side = 0;
    pwindow->buffer = NULL;
    pwindow->buffer_size.width = 0;
    pwindow->buffer_size.height = 0;
    pwindow->buffer_attached = 0;
    pwindow->cursor_shape = 0; /* default arrow until shaped (W2) */
    /* M-4: extension surfaces carry no toplevel (their role is the
     * add-on's) — the icon pair stays NULL forever; set_icon's
     * toplevel gate is the no-op path. */
    pwindow->toplevel_icon = NULL;
    pwindow->toplevel_icon_buffer = NULL;
    pwindow->toplevel_icon_buffer_size.width = 0;
    pwindow->toplevel_icon_buffer_size.height = 0;
    pwindow->icon_absent_logged = 0;
    for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
        pwindow->bg_pending[i] = NULL;
    }
    pwindow->render_pending = NULL;
    pwindow->rendered_ever = 0;
    pwindow->last_refuse_warn_ms = 0;
    pwindow->scale_x120 = 120;
    pwindow->buffer_scale = 1;
    pwindow->scale_applied = 0;
    pwindow->viewport = NULL;
    pwindow->fractional = NULL;
    pwindow->entered_outputs = NULL;
    pwindow->entered_count = 0;
    pwindow->entered_capacity = 0;
    pwindow->frame_ack = 1;
    pwindow->frame_commit_ms = 0;
    pwindow->frame_cb = NULL;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        pwindow->render_slots[i].buffer = NULL;
        pwindow->render_slots[i].pixels = NULL;
        pwindow->render_slots[i].length = 0;
        pwindow->render_slots[i].size.width = 0;
        pwindow->render_slots[i].size.height = 0;
        pwindow->render_slots[i].released = 0;
    }

    /* Output enter/leave tracking (HiDPI): the same listener the
     * create path attaches — the surface's scale follows the outputs
     * it lands on, and the render machinery sizes physical buffers
     * accordingly. Attaching a listener to the add-on's surface proxy
     * is safe: the add-on never attached its own wl_surface listener
     * (its role object — zwlr_layer_surface — is a SEPARATE proxy
     * with its own events). */
    wl_surface_add_listener(pwindow->surface, &g_wl_surface_listener,
                            pwindow);

    /* Fractional-scale preference (HiDPI): bound at create time for
     * normal windows so the compositor's preferred_scale event is
     * never missed — same reasoning for adopted surfaces, and the
     * object is per-surface so the add-on's role does not collide. */
    if (conn->fractional_manager != NULL) {
        pwindow->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            conn->fractional_manager, pwindow->surface);
        if (pwindow->fractional == NULL) {
            FDK_WARN("get_fractional_scale failed; output scale only "
                     "for this extension surface");
        } else {
            wp_fractional_scale_v1_add_listener(
                pwindow->fractional, &g_fractional_scale_listener, pwindow);
        }
    }

    fdk_result r = fdk_wayland_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        if (pwindow->fractional != NULL) {
            wp_fractional_scale_v1_destroy(pwindow->fractional);
        }
        /* Ownership was transferred at entry: the surface dies with
         * this failure (role teardown already ran in the caller). */
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("extension surface adopted (wl_surface %p)", (void *)surface);

    *out_pwindow = pwindow;
    return FDK_OK;
}

/* The documented native-display accessor behind
 * fdk_ext_get_native_display() (1.1.0): the connection's
 * struct wl_display*, borrowed for the connection's lifetime. */
void *fdk_wayland_native_display(fdk_platform_connection *conn) {
    return (conn != NULL) ? (void *)conn->display : NULL;
}

/* fdk_ext_surface_notify_configure()'s op: the role assigned this
 * size (the add-on already acked its protocol configure); the render
 * slots size themselves from it at the next acquire/present — the
 * same fields xdg_surface_configure updates for normal windows.
 *
 * 1.1.0-rc1 (found live on labwc and sway, 2026-10-26): this op ALSO
 * flips `configured` — the flag present()'s first-configure gate
 * reads — and commits the deferred first frame. The add-on contract
 * (fdk_extend.h, and fdk-shell's layer_configure): notify_configure
 * runs AFTER the role's configure was acked, so a commit happening
 * here is post-ack on the wire. The bug it closes: the demo bar
 * painted immediately after show() (before the role's configure
 * could possibly arrive — one full round trip away), present()
 * attached+committed the buffer, and the compositor killed the
 * connection: "zwlr_layer_surface_v1: error 2: layer_surface has
 * never been configured". The add-on can no longer get this wrong:
 * FDK holds the buffer until this op runs. */
void fdk_wayland_ext_surface_resized(fdk_platform_window *pwindow,
                                     fdk_i32 width, fdk_i32 height) {
    if (pwindow == NULL || width < 1 || height < 1) {
        return;
    }
    int size_changed =
        pwindow->last_size.width != width ||
        pwindow->last_size.height != height;
    if (size_changed) {
        /* A real resize: a fractional viewport's source rectangle is
         * size-derived, so it must re-apply before the next commit
         * (the same reset xdg_surface_configure performs). */
        pwindow->scale_applied = 0;
    }
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->pending_size = pwindow->last_size;
    pwindow->configured = 1; /* the gate: the role is configured    */

    /* The same deferred-first-frame commit xdg_surface_configure
     * performs after ITS ack — the pending buffer an eager add-on
     * painted (pre-configure) goes out now, post-ack, or an EXPOSE
     * re-drives the paint when the configure changed the size. */
    (void)fdk__wayland_commit_deferred_first_frame(pwindow);
}

/* fdk_ext_surface_native()'s op: the wrapped wl_surface. */
fdk_result fdk_wayland_ext_surface_native(fdk_platform_window *pwindow,
                                          fdk_ext_native_handle *out) {
    if (pwindow == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->backend = FDK_EXT_BACKEND_WAYLAND;
    out->pointer = pwindow->surface;
    return FDK_OK;
}

/* ---- Subsurfaces (1.1.0, D-2 / B-16) -------------------------------------
 *
 * The Wayland half of fdk_ext_subsurface_create: a wl_subsurface
 * role on a fresh wl_surface, parented to the parent's surface.
 * The core-protocol shapes that matter here:
 *
 *   - Mapping: a subsurface becomes visible when it has a committed
 *     buffer AND the parent is mapped — no role handshake, no
 *     configure. `configured` therefore starts 1 (the present gate's
 *     wait-for-configure deferral would otherwise wedge the child's
 *     first frame forever: nothing will ever configure it).
 *   - Mode: sync (protocol default — child state applies with the
 *     parent's commits) vs desync (applies on the child's own). FDK
 *     creates DESYNC: an independently-updating region is the
 *     feature's entire point, and a sync child of a quiet parent
 *     would never show a frame. set_desync flips either way.
 *   - Position: wl_subsurface_set_position is double-buffered —
 *     it takes effect at the child's next commit (desync) or the
 *     parent's (sync); the API documents this while X11 moves
 *     immediately.
 *   - Stacking: place_above/place_below take a sibling surface and
 *     are applied like position (cached until the next commit).
 *   - Input: the seat routes pointer events to the child's
 *     wl_surface wherever it is topmost — the conn->windows scan
 *     the seat already does finds the registered child naturally.
 *     Keyboard focus stays on the toplevel (compositor truth).
 *   - Teardown: the wl_subsurface object dies BEFORE the
 *     wl_surface (window_destroy's tail), and the window layer's
 *     parent sweep destroys children before the parent's own
 *     surface — the only orders the protocol blesses.
 *
 * Everything else (render slots, damage, frame callbacks, HiDPI
 * scale tracking through the shared surface listener, the fractional
 * -scale preference) is the same machinery an adopted ext surface
 * gets — this create copies that field set deliberately. */
/* THE PROTOCOL'S TREE-STATE RULE (found live against sway 1.10, the
 * D-2 rig's first lesson): a subsurface's POSITION, stacking, and
 * very PRESENCE in the parent's surface tree are PARENT-MANAGED
 * state — "applied whenever the state of the parent surface is
 * applied" (wayland.xml, verbatim) — regardless of the child's
 * sync/desync mode, which governs only the child's OWN buffer state.
 * A desync child that committed its buffer into a tree whose parent
 * never committed again is INVISIBLE and unhittable: sway never
 * renders it (its frame callback never fires — the empirical tell).
 *
 * FDK applies the tree state deterministically: an EMPTY commit on
 * the TOPMOST ancestor (empty = no buffer attach, purely a state
 * application; legal on any surface, and invisible to the app's own
 * frame pacing). Gated on the ancestor being CONFIGURED — an
 * unconfigured role surface (layer-shell pre-configure, an unshown
 * window) must not receive a stray commit; its own handshake commit
 * will carry the child in (correct by construction). */
static void subsurface_apply_tree_state(fdk_platform_window *child) {
    if (child == NULL || child->conn == NULL) {
        return;
    }
    fdk_platform_window *top = child->sub_parent;
    if (top == NULL) {
        return; /* not a subsurface (or corrupted) — nothing to apply */
    }
    while (top->sub_parent != NULL) {
        top = top->sub_parent; /* nesting: the ROOT's application
                                * cascades down the whole tree */
    }
    if (top->configured) {
        wl_surface_commit(top->surface);
    }
}

fdk_result fdk_wayland_subsurface_create(
    fdk_platform_connection *conn, fdk_platform_window *parent,
    fdk_i32 x, fdk_i32 y, fdk_i32 width, fdk_i32 height,
    fdk_platform_window **out_pwindow) {
    if (conn == NULL || parent == NULL || out_pwindow == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->display_dead) {
        return FDK_ERR_NO_DISPLAY;
    }
    if (conn->subcompositor == NULL || conn->compositor == NULL) {
        /* Honest refusal — never a fake. (subcompositor is CORE
         * protocol; a compositor without it also has no business
         * serving wl_compositor — but the check is cheap.) */
        return FDK_ERR_UNSUPPORTED;
    }

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* The ext-adopt field set (mirrored deliberately): no xdg
     * objects, the render-slot machinery, output-enter scale
     * tracking — with the TWO subsurface deltas: last_size is the
     * CLIENT's chosen logical size (known now — nothing assigns it
     * later), and `configured` starts 1 (no handshake exists). */
    pwindow->conn = conn;
    pwindow->surface = NULL; /* created below, torn down on failure */
    pwindow->xdg_surface = NULL;
    pwindow->xdg_toplevel = NULL;
    pwindow->ext = 1;
    pwindow->popup = 0;
    pwindow->xdg_popup = NULL;
    pwindow->subsurface = NULL;
    pwindow->sub_parent = NULL; /* set at the role creation below */
    pwindow->is_subsurface = 1;
    pwindow->input_transparent = 0;
    pwindow->drop_formats = 0;
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->pending_size = pwindow->last_size;
    pwindow->configured = 1; /* no configure will ever arrive */
    pwindow->maximized = 0;
    pwindow->minimized = 0;
    pwindow->fullscreen = 0;
    pwindow->toplevel_decoration = NULL;
    pwindow->deco_client_side = 0;
    pwindow->buffer = NULL;
    pwindow->buffer_size.width = 0;
    pwindow->buffer_size.height = 0;
    pwindow->buffer_attached = 0;
    pwindow->cursor_shape = 0;
    pwindow->toplevel_icon = NULL;
    pwindow->toplevel_icon_buffer = NULL;
    pwindow->toplevel_icon_buffer_size.width = 0;
    pwindow->toplevel_icon_buffer_size.height = 0;
    pwindow->icon_absent_logged = 0;
    for (int i = 0; i < FDK_WL_BG_SLOTS; i++) {
        pwindow->bg_pending[i] = NULL;
    }
    pwindow->render_pending = NULL;
    pwindow->rendered_ever = 0;
    pwindow->last_refuse_warn_ms = 0;
    pwindow->scale_x120 = 120;
    pwindow->buffer_scale = 1;
    pwindow->scale_applied = 0;
    pwindow->viewport = NULL;
    pwindow->fractional = NULL;
    pwindow->entered_outputs = NULL;
    pwindow->entered_count = 0;
    pwindow->entered_capacity = 0;
    pwindow->frame_ack = 1;
    pwindow->frame_commit_ms = 0;
    pwindow->frame_cb = NULL;
    pwindow->idle_inhibitor = NULL;
    for (int i = 0; i < FDK_WL_RENDER_SLOTS; i++) {
        pwindow->render_slots[i].buffer = NULL;
        pwindow->render_slots[i].pixels = NULL;
        pwindow->render_slots[i].length = 0;
        pwindow->render_slots[i].size.width = 0;
        pwindow->render_slots[i].size.height = 0;
        pwindow->render_slots[i].released = 0;
    }
    /* GPU lane (D-1): engaged lazily at the first framebuffer
     * acquisition — a video-sized child is exactly the lane's
     * audience, and the lane works on subsurfaces like on any
     * wl_surface (wl_egl_window wraps it fine). */
    pwindow->egl_win = NULL;
    pwindow->egl_downgraded = 0;
    pwindow->render_size.width = 0;
    pwindow->render_size.height = 0;
    pwindow->lane_frame_deferred = 0;

    pwindow->surface = wl_compositor_create_surface(conn->compositor);
    if (pwindow->surface == NULL) {
        FDK_ERROR("subsurface wl_compositor_create_surface failed");
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }

    /* The role FIRST — a wl_surface with no role is a protocol error
     * the moment it is committed; get_subsurface(parent, child) both
     * assigns it and records the parent/child link. */
    pwindow->subsurface = wl_subcompositor_get_subsurface(
        conn->subcompositor, pwindow->surface, parent->surface);
    if (pwindow->subsurface == NULL) {
        FDK_ERROR("wl_subcompositor_get_subsurface failed");
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return FDK_ERR_WINDOW_CREATE;
    }

    /* Output enter/leave tracking (HiDPI) + fractional-scale
     * preference: the same listeners an adopted surface gets — the
     * child's scale follows the outputs the PARENT lands on (the
     * compositor sends enter/leave per surface, children included). */
    wl_surface_add_listener(pwindow->surface, &g_wl_surface_listener,
                            pwindow);
    if (conn->fractional_manager != NULL) {
        pwindow->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
            conn->fractional_manager, pwindow->surface);
        if (pwindow->fractional == NULL) {
            FDK_WARN("get_fractional_scale failed; output scale only "
                     "for this subsurface");
        } else {
            wp_fractional_scale_v1_add_listener(
                pwindow->fractional, &g_fractional_scale_listener, pwindow);
        }
    }

    /* DESYNC from birth (the feature's point: independent frame
     * updates — the child's OWN buffer state applies at its own
     * commits), and the initial position. set_position's coords are
     * PARENT SURFACE-LOCAL — the same logical space the API speaks.
     * The position itself is parent-managed tree state: the empty
     * ancestor commit below pulls the child into the tree AT that
     * position (without it, a quiet parent leaves the child
     * invisible forever — the sway lesson). */
    wl_subsurface_set_desync(pwindow->subsurface);
    wl_subsurface_set_position(pwindow->subsurface, x, y);
    pwindow->sub_parent = parent;
    subsurface_apply_tree_state(pwindow);

    fdk_result r = fdk_wayland_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        if (pwindow->fractional != NULL) {
            wp_fractional_scale_v1_destroy(pwindow->fractional);
        }
        wl_subsurface_destroy(pwindow->subsurface);
        wl_surface_destroy(pwindow->surface);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("subsurface %p created under %p (%dx%d at %d,%d, desync)",
              (void *)pwindow->surface, (void *)parent->surface,
              width, height, x, y);

    *out_pwindow = pwindow;
    return FDK_OK;
}

void fdk_wayland_subsurface_set_position(fdk_platform_window *pwindow,
                                         fdk_i32 x, fdk_i32 y) {
    if (pwindow == NULL || pwindow->subsurface == NULL) {
        return;
    }
    wl_subsurface_set_position(pwindow->subsurface, x, y);
    /* Position is parent-managed state: apply it NOW (the empty
     * ancestor commit) — X11's immediacy, protocol-legally. */
    subsurface_apply_tree_state(pwindow);
}

void fdk_wayland_subsurface_resize(fdk_platform_window *pwindow,
                                   fdk_i32 width, fdk_i32 height) {
    if (pwindow == NULL || pwindow->subsurface == NULL || width < 1 ||
        height < 1) {
        return;
    }
    /* No protocol resize exists for a subsurface — the SIZE is the
     * next committed buffer's size. Record it (the ext_surface_
     * resized field discipline: last_size + pending_size drive the
     * render slots, scale_applied resets so a fractional viewport
     * re-derives its source rect); the next present commits a buffer
     * of this size and the compositor follows. The window layer
     * dispatches the CONFIGURE event to the child's callback. */
    int size_changed = pwindow->last_size.width != width ||
                       pwindow->last_size.height != height;
    if (size_changed) {
        pwindow->scale_applied = 0;
    }
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;
    pwindow->pending_size = pwindow->last_size;
}

void fdk_wayland_subsurface_set_desync(fdk_platform_window *pwindow,
                                       int desync) {
    if (pwindow == NULL || pwindow->subsurface == NULL) {
        return;
    }
    if (desync) {
        wl_subsurface_set_desync(pwindow->subsurface);
    } else {
        wl_subsurface_set_sync(pwindow->subsurface);
    }
}

fdk_result fdk_wayland_subsurface_place(fdk_platform_window *pwindow,
                                        fdk_platform_window *sibling,
                                        int above) {
    if (pwindow == NULL || sibling == NULL || pwindow->subsurface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (sibling->surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Cached like position: applied at the next commit. The window
     * layer already validated both are children of the SAME parent —
     * the protocol's own rule (siblings must share a parent). */
    if (above) {
        wl_subsurface_place_above(pwindow->subsurface, sibling->surface);
    } else {
        wl_subsurface_place_below(pwindow->subsurface, sibling->surface);
    }
    /* Stacking is parent-managed state too: apply it NOW. */
    subsurface_apply_tree_state(pwindow);
    return FDK_OK;
}
