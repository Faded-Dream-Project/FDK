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

#define FDKSHELL_VERSION_STRING "0.5.0"

/* Returns this package's version ("0.5.0"). */
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
 * discovery order (live: hot-plug and mode changes arrive as events);
 * X11 reports the RandR outputs of the screen (0.2.0; the fallback
 * before RandR probing is one output: the whole screen).
 *
 * The contract: enumerate the initial set right after fdk_shell_init
 * (output_count / output_name / output_info), install the output
 * event callback, and every later change — a monitor plugged in or
 * unplugged, a mode or scale change — arrives there. Output indices
 * are STABLE for the life of the output (a removed output's index is
 * not reused while it is still enumerable; gone outputs keep their
 * records until shutdown).
 */

/* Writes the number of selectable outputs to *out_count. */
fdk_result fdk_shell_output_count(fdk_shell *shell, uint32_t *out_count);

/* Copies output `index`'s human-readable name (the Wayland connector
 * name where the compositor provides one, "screen-0" style on X11)
 * into buf. Truncates to buflen-1 + NUL. Returns
 * FDK_ERR_NOT_FOUND for an out-of-range index. */
fdk_result fdk_shell_output_name(fdk_shell *shell, uint32_t index,
                                 char *buf, size_t buflen);

/* Output geometry/metrics (0.2.0). struct_size discipline: zero-init,
 * set struct_size = sizeof(fdk_shell_output_info), read the fields.
 *
 *   x, y:          the output's position in the GLOBAL layout (the
 *                  compositor's coordinate space; 0,0 for the X11
 *                  screen).
 *   width, height: the LOGICAL size (the compositor's mode divided
 *                  by its scale — the size a panel sizes itself
 *                  against).
 *   scale:         the output's scale factor (1 on unscaled outputs;
 *                  fractional-scale compositors round to the
 *                  wl_output integer factor).
 *   physical_width, physical_height: the monitor's physical size in
 *                  millimeters (0 when the compositor does not say).
 */
typedef struct fdk_shell_output_info {
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t scale;
    int32_t physical_width;
    int32_t physical_height;
} fdk_shell_output_info;

/* Writes output `index`'s current metrics to *out (struct_size must
 * be set). FDK_ERR_NOT_FOUND for an out-of-range index. Metrics for
 * an output that has not finished its first description round may be
 * zero — the CHANGED event will arrive when they are. */
fdk_result fdk_shell_output_get_info(fdk_shell *shell, uint32_t index,
                                      fdk_shell_output_info *out);

/* Output lifecycle events (0.2.0). */
typedef enum fdk_shell_output_event_type {
    FDK_SHELL_OUTPUT_ADDED = 1,   /* a new output became enumerable */
    FDK_SHELL_OUTPUT_REMOVED = 2, /* an output went away            */
    FDK_SHELL_OUTPUT_CHANGED = 3, /* metrics changed (mode/scale/
                                     position) — re-read with
                                     fdk_shell_output_info          */
} fdk_shell_output_event_type;

typedef struct fdk_shell_output_event {
    fdk_shell_output_event_type type;
    uint32_t index; /* the stable index the event is about          */
} fdk_shell_output_event;

typedef void (*fdk_shell_output_event_fn)(fdk_shell *shell,
                                          const fdk_shell_output_event *event,
                                          void *user);

/* Installs (or with fn == NULL, removes) the output event receiver.
 * The callback fires from inside FDK's dispatch (the pump), like
 * every fdk-shell callback; it may call any fdk_shell function
 * (creating a layer for a new output is the canonical reaction). */
void fdk_shell_set_output_event_callback(fdk_shell *shell,
                                         fdk_shell_output_event_fn fn,
                                         void *user);

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
    /* 0.2.0 (appended; old callers' smaller struct_size is honored):
     * the surface's NAMESPACE — what compositor RULES match on.
     * Wayland: zwlr_layer_shell's namespace argument (sway's
     * for_window criteria, labwc's window-rules). X11: the window's
     * WM_CLASS res_class (the same rule-matching role). NUL/empty
     * defaults to "fdk-shell". */
    char namespace_[64];
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

/* ---- Cursor shape (0.1.0; the full named set 0.4.0) -------------------
 *
 * The pointer's shape while it hovers the layer or popup. The shape
 * is re-applied automatically on every wl_pointer.enter (Wayland
 * cursor state does not survive leaving a surface); X11 tracks it as
 * window state. Typical panel usage: FDK_SHELL_CURSOR_DEFAULT at
 * creation, FDK_SHELL_CURSOR_POINTER while the pointer hovers a
 * clickable region.
 *
 * The values ARE FDK's fdk_cursor_shape values (the passthrough casts
 * the int), so the full named set (FDK 1.1.0-rc2, S-5) rides through
 * unchanged — a launcher may want TEXT over its search entry, GRAB
 * over drag handles, WAIT while the app list loads. The mirrors below
 * exist so fdk-shell code never needs FDK's header to spell one; see
 * FDK's fdk_window.h (fdk_cursor_shape) for the per-shape semantics. */
typedef enum fdk_shell_cursor {
    FDK_SHELL_CURSOR_DEFAULT = 0, /* the standard arrow */
    FDK_SHELL_CURSOR_POINTER = 1, /* the pointing hand */
    FDK_SHELL_CURSOR_TEXT = 2,    /* the I-beam */
    FDK_SHELL_CURSOR_CROSSHAIR = 3,
    FDK_SHELL_CURSOR_WAIT = 4,
    FDK_SHELL_CURSOR_PROGRESS = 5,
    FDK_SHELL_CURSOR_MOVE = 6,
    FDK_SHELL_CURSOR_COPY = 7,
    FDK_SHELL_CURSOR_ALIAS = 8,
    FDK_SHELL_CURSOR_NO_DROP = 9,
    FDK_SHELL_CURSOR_NOT_ALLOWED = 10,
    FDK_SHELL_CURSOR_GRAB = 11,
    FDK_SHELL_CURSOR_GRABBING = 12,
    FDK_SHELL_CURSOR_ALL_SCROLL = 13,
    FDK_SHELL_CURSOR_COL_RESIZE = 14,
    FDK_SHELL_CURSOR_ROW_RESIZE = 15,
    FDK_SHELL_CURSOR_ZOOM_IN = 16,
    FDK_SHELL_CURSOR_ZOOM_OUT = 17,
    FDK_SHELL_CURSOR_HELP = 18,
    FDK_SHELL_CURSOR_CELL = 19,
    FDK_SHELL_CURSOR_CONTEXT_MENU = 20,
    FDK_SHELL_CURSOR_VERTICAL_TEXT = 21,
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

/* ---- Runtime re-anchoring + auto-hide (0.5.0, C-5 / A-05 / A-08) -----------
 *
 * The layer's placement parameters are LIVE: anchors, margins, the
 * exclusive zone, the desired size, and the keyboard mode can change
 * on a shown surface without destroying it.
 *
 *   Wayland: the zwlr requests (set_anchor / set_margin /
 *     set_exclusive_zone / set_size / set_keyboard_interactivity)
 *     are double-buffered state — one commit applies them, the next
 *     configure confirms. The surface is NEVER destroyed.
 *   X11: the anchoring math is recomputed and the window is moved /
 *     resized in place; the struts are rewritten (a zone change
 *     re-reserves immediately); USPosition keeps the WM from
 *     "helping".
 *
 * set_level (the stacking tier) has no live protocol form on
 * Wayland (zwlr has no set_layer request): the layer surface's ROLE
 * is swapped — destroyed and re-created on the target shell layer
 * over the same wl_surface (the same surface identity, one
 * unconfigure/configure cycle). On X11 it is one _NET_WM_STATE
 * exchange (REMOVE the old tier's atoms, ADD the new) plus the
 * _FDK_WM_LAYER property rewrite.
 *
 * AUTO-HIDE (A-08): fdk_shell_layer_set_auto_hide arms the
 * reveal-strip model — the classic hidden panel:
 *
 *   - HIDDEN: the surface slides off its PRIMARY anchored edge
 *     (the edge whose axis it reserves — a top bar slides up),
 *     leaving `reveal_px` pixels on screen as the reveal strip.
 *     The exclusive zone is WITHDRAWN while hidden (other windows
 *     take the space — that is the point of hiding).
 *   - the pointer entering the strip (or the layer) REVEALS: the
 *     surface slides back, the zone returns.
 *   - the pointer LEAVING the revealed layer starts a `dwell_ms`
 *     timer; its expiry hides again. reveal(0)/dwell(0) hide/reveal
 *     instantly.
 *   - fdk_shell_layer_reveal / _hide_now drive the states from
 *     application code (menus that must not hide while open, for
 *     instance — call reveal() on popup-open, hide_now() never: the
 *     dwell restarts on the next leave).
 *
 * The slide itself is the platform's geometry change (instant on
 * X11, compositor-paced on Wayland); an animated glide is future
 * polish — the positions, the input strip, and the zone semantics
 * are the contract. Layers with no single-anchor axis (a centered
 * floating layer) have no slide direction: set_auto_hide answers
 * FDK_ERR_INVALID_ARGUMENT for them. */

fdk_result fdk_shell_layer_set_anchor(fdk_shell_layer *layer,
                                      uint32_t anchors);
fdk_result fdk_shell_layer_set_margin(fdk_shell_layer *layer,
                                      int32_t top, int32_t right,
                                      int32_t bottom, int32_t left);
fdk_result fdk_shell_layer_set_exclusive_zone(fdk_shell_layer *layer,
                                              int32_t zone);
fdk_result fdk_shell_layer_set_desired_size(fdk_shell_layer *layer,
                                            int32_t width, int32_t height);
fdk_result fdk_shell_layer_set_keyboard_mode(fdk_shell_layer *layer,
                                             fdk_shell_keyboard_mode mode);
fdk_result fdk_shell_layer_set_level(fdk_shell_layer *layer,
                                     fdk_shell_layer_level level);

/* Arms auto-hide: reveal_px > 0 enables (the on-screen strip while
 * hidden); 0 disables and reveals if hidden. dwell_ms is the
 * leave-to-hide delay (0 = instant). FDK_ERR_INVALID_ARGUMENT on a
 * layer with no slide direction or a negative reveal. */
fdk_result fdk_shell_layer_set_auto_hide(fdk_shell_layer *layer,
                                         int32_t reveal_px,
                                         uint32_t dwell_ms);
/* Forces the shown state (cancels a pending hide). */
void fdk_shell_layer_reveal(fdk_shell_layer *layer);
/* Forces the hidden state immediately (the reveal strip stays). */
void fdk_shell_layer_hide_now(fdk_shell_layer *layer);

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

/* 0.2.0 — CASCADES: a popup parented to another popup (launcher
 * menus with submenus). The child positions itself parent-relative
 * (x, y in the PARENT POPUP's local coordinates), stacks above it,
 * and dismisses together with it (destroying a parent sweeps its
 * children — the same family contract layers follow). Any depth the
 * backend allows: X11 through the window tree, Wayland through
 * xdg_popup-on-xdg_popup (the compositor clamps to the screen). */
fdk_result fdk_shell_popup_create_child(fdk_shell_popup *parent,
                                        int32_t x, int32_t y,
                                        int32_t width, int32_t height,
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
