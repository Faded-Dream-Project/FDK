/*
 * fdk_shell.h — fdk-shell: the shell-surface add-on for FDK
 * (fdk-shell-overlay, the first surface add-on package).
 *
 * Panels, docks, notification shells, on-screen displays: surfaces
 * that live OUTSIDE the normal window stack — compositor-layer
 * surfaces on Wayland (zwlr_layer_shell_v1) and dock windows on
 * X11 (_NET_WM_WINDOW_TYPE_DOCK + struts). This package builds
 * them on FDK's extension API (fdk_extend.h) and nothing else:
 * the returned handle IS an fdk_ext_surface*, so rendering
 * (fdk_ext_surface_get_surface), events
 * (fdk_ext_surface_set_event_callback), input regions, popups and
 * subsurfaces all ride the extension API unchanged.
 *
 * What the add-on does per backend:
 *   Wayland: binds zwlr_layer_shell_v1 through its OWN registry on
 *   the shared wl_display, creates the layer surface with
 *   layer/anchor/margins/exclusive-zone/keyboard mode, acks each
 *   configure and reports it (fdk_ext_surface_notify_configure —
 *   FDK's present gate then handles "never commit a buffer before
 *   the first configure"), and adopts xdg_popups via
 *   zwlr_layer_surface_v1.get_popup.
 *   X11: creates an override-redirect window with
 *   _NET_WM_WINDOW_TYPE_DOCK and _NET_WM_STRUT_PARTIAL, sized
 *   against the monitor geometry; ConfigureNotify flows through
 *   the normal event stream (no notify_configure needed).
 *
 * Version: shell API v1, against FDK extension API v2.
 *
 * Usage sketch:
 *
 *     fdk_shell_overlay_cfg cfg = {
 *         .layer     = FDK_SHELL_LAYER_TOP,
 *         .anchor    = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT
 *                    | FDK_SHELL_ANCHOR_RIGHT,
 *         .exclusive = 32,          reserve the bar's strip
 *     };
 *     fdk_ext_surface *bar;
 *     if (!fdk_ok(fdk_shell_overlay_create(ctx, &cfg, &bar)))
 *         return 1;
 *     fdk_ext_surface_set_event_callback(bar, on_event, NULL);
 *     fdk_ext_surface_show(bar);    X11 maps; Wayland commits
 *                                      the empty first frame
 *     ... draw on fdk_ext_surface_get_surface(bar) ...
 *
 * Docs: docs/EXTENDING.md (the extension contracts this builds
 * on), docs/PARITY.md (why this package exists).
 */

#ifndef FDK_SHELL_H
#define FDK_SHELL_H

#include <fdk/fdk_error.h>
#include <fdk/fdk_extend.h>
#include <fdk/fdk_types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Version ------------------------------------------------------------ */

#define FDK_SHELL_API_VERSION 1

/* Runtime version of the loaded shell add-on (compare against
 * FDK_SHELL_API_VERSION the same way add-ons check
 * fdk_ext_api_version()). */
int fdk_shell_api_version(void);

/* ---- Configuration ------------------------------------------------------ */

/* The compositor layer (Wayland layer-shell layers; on X11 the
 * stacking class is approximated by the dock's override-redirect
 * always-on-top nature — the enum still guides stacking intent
 * where a WM honors it). */
typedef enum fdk_shell_layer {
    FDK_SHELL_LAYER_BACKGROUND = 0, /* the desktop background     */
    FDK_SHELL_LAYER_BOTTOM     = 1, /* below windows              */
    FDK_SHELL_LAYER_TOP        = 2, /* above windows (panels)     */
    FDK_SHELL_LAYER_OVERLAY    = 3, /* above everything (OSDs)    */
} fdk_shell_layer;

/* Which output edges the surface is anchored to (bitmask; the
 * surface stretches between anchored edges). */
#define FDK_SHELL_ANCHOR_TOP    0x01u
#define FDK_SHELL_ANCHOR_BOTTOM 0x02u
#define FDK_SHELL_ANCHOR_LEFT   0x04u
#define FDK_SHELL_ANCHOR_RIGHT  0x08u

/* Keyboard focus desire (layer-shell's keyboard_interactivity;
 * X11 approximates NONE and EXCLUSIVE via focus policy). */
typedef enum fdk_shell_keyboard {
    FDK_SHELL_KBD_NONE      = 0, /* no keyboard focus            */
    FDK_SHELL_KBD_EXCLUSIVE = 1, /* take exclusive keyboard      */
    FDK_SHELL_KBD_ON_DEMAND = 2, /* focus when clicked (v4)      */
} fdk_shell_keyboard;

typedef struct fdk_shell_overlay_cfg {
    size_t struct_size;   /* set to sizeof(this struct)          */

    fdk_shell_layer layer;       /* default TOP                   */
    unsigned anchor;             /* default TOP|LEFT|RIGHT        */
    fdk_shell_keyboard keyboard; /* default NONE                  */

    /* Offsets from each anchored edge (logical px; 0 = flush). */
    int margin_top, margin_bottom, margin_left, margin_right;

    /* Exclusive zone: the strip this surface reserves for itself
     * so normal windows never overlap it (Wayland:
     * exclusive_zone; X11: strut hints). -1 means "pass through
     * the zone of the surface below me" (layer-shell rule); 0
     * reserves nothing. */
    int exclusive;

    /* Initial size. For a surface anchored to both ends of an
     * axis, that axis's size comes from the compositor and the
     * value here is ignored; the other axis uses size (a bar
     * anchored TOP|LEFT|RIGHT: width assigned, height = size). */
    int size;

    /* Desired output name (Wayland: wl_output name; NULL = the
     * compositor's choice / X11: primary). */
    const char *output_name;
} fdk_shell_overlay_cfg;

/* ---- Overlay lifecycle --------------------------------------------------- */

/* Creates a shell overlay surface of the given configuration.
 * The returned handle is an extension surface: render through
 * fdk_ext_surface_get_surface(), receive events through
 * fdk_ext_surface_set_event_callback() (FDK_EVENT_WINDOW_CONFIGURE
 * carries each compositor-assigned size), destroy with
 * fdk_ext_surface_destroy(). show() maps (X11) or starts the
 * configure handshake (Wayland) — FDK's present gate makes eager
 * first draws safe on both.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx/cfg/out NULL, struct_size too
 *                              small, or a bad anchor/layer
 *   FDK_ERR_NOT_INITIALIZED  - ctx has no live platform connection
 *   FDK_ERR_UNSUPPORTED      - Wayland compositor offers no
 *                              zwlr_layer_shell_v1
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_WINDOW_CREATE    - the backend refused the surface
 */
fdk_result fdk_shell_overlay_create(fdk_context *ctx,
                                    const fdk_shell_overlay_cfg *cfg,
                                    fdk_ext_surface **out);

/* Adjusts the live overlay's anchors/margins/exclusive zone (the
 * layer-shell set_layer / set_anchor / set_margin / set_exclusive_
 * zone calls on Wayland — the new configure arrives through the
 * event callback; X11: moves/resize + strut update). Same failure
 * codes as create. */
fdk_result fdk_shell_overlay_reconfigure(fdk_ext_surface *overlay,
                                         const fdk_shell_overlay_cfg *cfg);

/* Returns the backend the overlay actually used (diagnostics for
 * apps that care; FDK_EXT_BACKEND_NONE on a NULL overlay). */
fdk_ext_backend fdk_shell_overlay_backend(const fdk_ext_surface *overlay);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SHELL_H */
