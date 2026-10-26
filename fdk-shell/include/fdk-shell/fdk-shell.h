/*
 * fdk-shell.h — layer-shell surfaces for FDK (fdk-shell 0.1.0)
 *
 * fdk-shell is the first ADD-ON PACKAGE built on FDK's extension API
 * (fdk/extend.h, FDK 1.1.0): panel/bar/notification-style surfaces
 * that live OUTSIDE the normal window stack — desktop background
 * layers, docks, top bars, overlays — behind ONE API on both of
 * FDK's backends:
 *
 *   Wayland:  wlr-layer-shell v4 (the protocol sway, labwc, river,
 *             wlroots compositors speak), via wayland-scanner
 *             generated bindings vendored in this package.
 *   X11/XLibre: a _NET_WM_WINDOW_TYPE_DOCK window with
 *             _NET_WM_STRUT_PARTIAL reservation hints — the EWMH
 *             world's equivalent layering semantics, honored by
 *             OpenBox, fluxbox, IceWM and friends.
 *
 * fdk-shell links FDK's INSTALLED PUBLIC headers and library only
 * (proved by tests/isolation.sh, which installs FDK into a throwaway
 * prefix and builds this package against nothing else). The native
 * protocols are spoken through the extension API's documented
 * accessors: Xlib and wayland-client are fdk-shell's own
 * dependencies, exactly as they would be for any native client.
 *
 * DECLARING THE FDK VERSION YOU NEED (the add-on pattern,
 * docs/EXTENDING.md - Versioning):
 *
 *     #include <fdk-shell/fdk-shell.h>
 *     // compile-time: the extension API this package requires
 *     #if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 1
 *     #error "fdk-shell needs FDK 1.1.0 (extension API v1)"
 *     #endif
 *     // run-time: the loaded library matches
 *     if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0)) fail();
 *     if (fdk_ext_api_version() < 1) fail();
 *
 * Threading: everything here is UI-thread-only, like FDK itself.
 */

#ifndef FDK_SHELL_H
#define FDK_SHELL_H

#include <fdk/fdk.h>
#include <fdk/fdk_extend.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FDKSHELL_VERSION_STRING "0.1.0"

/* Returns this package's version ("0.1.0"). */
const char *fdk_shell_version(void);

/* ---- the per-context handle ---------------------------------------------- */

typedef struct fdk_shell fdk_shell;

/* Initializes fdk-shell on `ctx` (one handle per context; a second
 * init on the same context fails with FDK_ERR_ALREADY_INITIALIZED).
 *
 * What init does per backend:
 *   Wayland: binds fdk-shell's own wl_registry on FDK's wl_display,
 *   discovers wlr-layer-shell (v4) and the outputs, and registers
 *   the surface kind. Fails with FDK_ERR_UNSUPPORTED when the
 *   compositor does not offer zwlr_layer_shell_v1 — check
 *   fdk_shell_backend_supported() first when a fallback matters.
 *   X11: registers the surface kind (the dock/strut semantics need
 *   no protocol setup).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx or out is NULL
 *   FDK_ERR_NOT_INITIALIZED  - ctx has no live connection
 *   FDK_ERR_UNSUPPORTED      - Wayland without wlr-layer-shell
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_shell_init(fdk_context *ctx, fdk_shell **out);

/* Tears the handle down: destroys any layers still alive (their
 * event callbacks see nothing — teardown, not delivery), releases
 * the kind and the protocol objects. The fdk_context itself is
 * untouched. NULL is a safe no-op. */
void fdk_shell_shutdown(fdk_shell *shell);

/* Nonzero when this context's backend can host layer surfaces
 * (Wayland with wlr-layer-shell, or X11). Same probe init uses. */
int fdk_shell_backend_supported(fdk_context *ctx);

/* ---- outputs ---------------------------------------------------------------
 *
 * Output selection is part of a layer's description (which screen a
 * panel docks to). Wayland enumerates the compositor's outputs in
 * discovery order; X11 reports exactly ONE output (the screen —
 * multi-monitor docks are a RandR feature outside this package's X11
 * scope, documented honestly in the README).
 */

/* Writes the number of selectable outputs to *out_count. */
fdk_result fdk_shell_output_count(fdk_shell *shell, uint32_t *out_count);

/* Copies output `index`'s human-readable name (the Wayland connector
 * name where the compositor provides one, "screen-0" style on X11)
 * into buf. Truncates to buflen-1 + NUL. Returns
 * FDK_ERR_NOT_FOUND for an out-of-range index. */
fdk_result fdk_shell_output_name(fdk_shell *shell, uint32_t index,
                                 char *buf, size_t buflen);

/* ---- layers ---------------------------------------------------------------- */

typedef struct fdk_shell_layer fdk_shell_layer;

/* The stacking layer a surface occupies — zwlr_layer_shell_v1's
 * names, and the EWMH ordering fdk-shell's X11 backend arranges. */
typedef enum fdk_shell_layer_level {
    FDK_SHELL_LAYER_BACKGROUND = 0, /* the desktop wallpaper level   */
    FDK_SHELL_LAYER_BOTTOM     = 1, /* below normal windows          */
    FDK_SHELL_LAYER_TOP        = 2, /* docks and bars                */
    FDK_SHELL_LAYER_OVERLAY    = 3, /* above everything, notifications */
} fdk_shell_layer_level;

/* Screen edges a layer anchors to — OR-combine. An anchored edge
 * glues the surface to that edge (the compositor sizes the anchored
 * axes; `width`/`height` in the description size the free ones).
 * Zero anchors is invalid. */
#define FDK_SHELL_ANCHOR_TOP    0x01u
#define FDK_SHELL_ANCHOR_BOTTOM 0x02u
#define FDK_SHELL_ANCHOR_LEFT   0x04u
#define FDK_SHELL_ANCHOR_RIGHT  0x08u

/* Keyboard interactivity — zwlr_layer_surface_v1's modes. */
typedef enum fdk_shell_keyboard_mode {
    FDK_SHELL_KBD_NONE      = 0, /* no keyboard focus at all        */
    FDK_SHELL_KBD_EXCLUSIVE = 1, /* takes ALL keyboard input        */
    FDK_SHELL_KBD_ON_DEMAND = 2, /* focus on click (layer-shell v4) */
} fdk_shell_keyboard_mode;

/* Layer creation description. Zero-init, set struct_size =
 * sizeof(fdk_shell_layer_desc), fill what applies (the
 * append-only-input-struct discipline FDK's ABI policy uses).
 *
 *   anchors:       which edges the surface is glued to (required).
 *   margin_*:      gap between the surface and its anchored edges
 *                  (logical pixels; all default 0).
 *   exclusive_zone: space the surface asks OTHER surfaces to keep
 *                  clear of: -1 = auto (the surface's own extent
 *                  along its anchored edge), 0 = reserve nothing,
 *                  >0 = reserve exactly that many logical pixels.
 *   keyboard:      FDK_SHELL_KBD_NONE by default.
 *   width/height:  the DESIRED size along un-anchored axes (<=0 =
 *                  0). Anchored axes are compositor-sized (Wayland)
 *                  or screen-derived (X11).
 *   output_index:  which output to place on: -1 = the default
 *                  output (Wayland: all outputs get their own layer
 *                  surface — the shell-panel shape — when unset;
 *                  X11: the screen). >= 0: that output only.
 *
 * Backend notes (the SAME desc drives both):
 *   Wayland: every field maps 1:1 onto zwlr_layer_surface_v1
 *   requests. output_index -1 passes a NULL wl_output (the "every
 *   output" protocol shape).
 *   X11: geometry is computed from the screen + anchors + margins +
 *   desired size; exclusive_zone becomes _NET_WM_STRUT_PARTIAL
 *   (auto = the surface's own extent); every layer is STICKY across
 *   workspaces (_NET_WM_DESKTOP = 0xFFFFFFFF + _NET_WM_STATE_STICKY,
 *   pre-map properties + post-map client messages — the Wayland
 *   behavior arranged through EWMH); TOP/OVERLAY carry
 *   _NET_WM_STATE_ABOVE (OVERLAY adds SKIP_TASKBAR/SKIP_PAGER — the
 *   strongest EWMH stacking state; fullscreen-over-docks is a
 *   documented protocol ceiling, see the README); keyboard EXCLUSIVE
 *   maps to an XGrabKeyboard while shown, ON_DEMAND to best-effort
 *   focus. BACKGROUND/BOTTOM have no below-normal-windows EWMH
 *   mapping (background/overlay deltas are Wayland-only truth — X11
 *   documents this honestly).
 */
typedef struct fdk_shell_layer_desc {
    uint32_t struct_size;
    fdk_shell_layer_level layer;
    uint32_t anchors;
    int32_t margin_top;
    int32_t margin_right;
    int32_t margin_bottom;
    int32_t margin_left;
    int32_t exclusive_zone; /* -1 auto, 0 none, >0 explicit */
    fdk_shell_keyboard_mode keyboard;
    int32_t width;  /* desired un-anchored axis size  */
    int32_t height; /* desired un-anchored axis size  */
    int32_t output_index; /* -1 default, >=0 specific  */
} fdk_shell_layer_desc;

/* The layer event callback: FDK's backend-neutral event stream (see
 * fdk_event.h) with the layer in place of a window — configure (the
 * compositor assigned a size: redraw now), expose, pointer, keys,
 * and FDK_EVENT_WINDOW_CLOSE_REQUEST when the surface is being
 * closed by the environment (a Wayland output going away delivers
 * "closed"; the application responds by destroying the layer). */
typedef void (*fdk_shell_event_fn)(fdk_shell_layer *layer,
                                   const fdk_event_data *event,
                                   void *user);

/* Creates a layer surface from `desc` (struct_size must be set).
 * The surface appears when its protocol handshake and first present
 * complete (Wayland) or at fdk_shell_layer_show (X11 maps it; call
 * show on both — it is the honest cross-backend idiom).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - shell/desc/out NULL, struct_size too
 *                              small, no anchors, bad enum values
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_WINDOW_CREATE    - the backend refused the surface
 */
fdk_result fdk_shell_layer_create(fdk_shell *shell,
                                  const fdk_shell_layer_desc *desc,
                                  fdk_shell_layer **out);

/* Destroys the layer (role teardown, then the native surface — the
 * extension API's documented ordering). Popups parented to it are
 * swept first. NULL is a safe no-op. */
void fdk_shell_layer_destroy(fdk_shell_layer *layer);

/* The layer's render target — the ordinary public fdk_surface API
 * (get_info, draw, present). Same ownership rules as an
 * application's window surface. */
fdk_result fdk_shell_layer_get_surface(fdk_shell_layer *layer,
                                       fdk_surface **out);

/* Sets the layer's event callback (replaces any previous one). */
void fdk_shell_layer_set_event_callback(fdk_shell_layer *layer,
                                        fdk_shell_event_fn callback,
                                        void *user);

/* ---- Cursor shape (0.1.0) ---------------------------------------------
 *
 * The pointer's shape while it hovers the layer or popup. The shape
 * is re-applied automatically on every wl_pointer.enter (Wayland
 * cursor state does not survive leaving a surface); X11 tracks it as
 * window state. Typical panel usage: FDK_SHELL_CURSOR_DEFAULT at
 * creation, FDK_SHELL_CURSOR_POINTER while the pointer hovers a
 * clickable region. */
typedef enum fdk_shell_cursor {
    FDK_SHELL_CURSOR_DEFAULT = 0, /* the standard arrow */
    FDK_SHELL_CURSOR_POINTER = 1, /* the pointing hand */
} fdk_shell_cursor;

/* Sets the layer's hover cursor shape. No-op on an unknown value. */
void fdk_shell_layer_set_cursor(fdk_shell_layer *layer,
                                fdk_shell_cursor cursor);

/* The layer's current size (the most recent configure). */
fdk_result fdk_shell_layer_get_size(const fdk_shell_layer *layer,
                                    fdk_size *out_size);

/* Shows / hides the layer. Wayland: show() is when the layer's
 * protocol mapping begins in earnest (the extension API's
 * documented no-op — mapping completes at the role's configure +
 * first present); hide() is likewise inert (a layer surface unmaps
 * by destroying and re-creating its role — destroy/create is the
 * documented idiom). X11: maps / unmaps the dock window. */
void fdk_shell_layer_show(fdk_shell_layer *layer);
void fdk_shell_layer_hide(fdk_shell_layer *layer);

/* ---- popups (dropdowns) ---------------------------------------------------- */

typedef struct fdk_shell_popup fdk_shell_popup;

/* Creates a dropdown-style popup parented to `layer`, positioned at
 * (x, y) in LAYER-LOCAL logical coordinates, sized width x height.
 * The popup takes the input grab while shown (X11 server grab /
 * xdg_popup.grab on Wayland — the layer-shell get_popup pattern),
 * and a click outside its bounds or Escape delivers
 * FDK_EVENT_WINDOW_CLOSE_REQUEST to its callback: the application
 * responds by destroying it.
 *
 * Render and receive events exactly like a layer
 * (fdk_shell_popup_get_surface / set_event_callback / show /
 * destroy).
 *
 * Can fail with the fdk_ext_popup_create error set (the extension
 * API's popup contract, verbatim). */
fdk_result fdk_shell_popup_create(fdk_shell_layer *parent, int32_t x,
                                  int32_t y, int32_t width, int32_t height,
                                  fdk_shell_popup **out);

void fdk_shell_popup_destroy(fdk_shell_popup *popup);
fdk_result fdk_shell_popup_get_surface(fdk_shell_popup *popup,
                                       fdk_surface **out);

typedef void (*fdk_shell_popup_event_fn)(fdk_shell_popup *popup,
                                         const fdk_event_data *event,
                                         void *user);

void fdk_shell_popup_set_event_callback(fdk_shell_popup *popup,
                                        fdk_shell_popup_event_fn callback,
                                        void *user);

/* Sets the popup's hover cursor shape (same semantics as the layer's;
 * typical usage is the hand over the clickable items). */
void fdk_shell_popup_set_cursor(fdk_shell_popup *popup,
                                fdk_shell_cursor cursor);
void fdk_shell_popup_show(fdk_shell_popup *popup);
void fdk_shell_popup_hide(fdk_shell_popup *popup);

#ifdef __cplusplus
}
#endif

#endif /* FDK_SHELL_H */
