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
