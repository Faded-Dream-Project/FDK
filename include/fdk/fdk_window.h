/*
 * fdk_window.h — Faded Dream ToolKit window API
 *
 * A top-level window backed by whichever platform backend the owning
 * fdk_context selected (X11 or Wayland — see fdk_core.h). No backend
 * type ever appears here; see docs/architecture.md, "no backend
 * leakage".
 *
 * Threading: window functions are UI-thread-only, like the rest of
 * FDK — see docs/threading.md.
 *
 * Phase 2 scope: creation, destruction, show/hide, title, size
 * (including min/max hints), close-request handling, and configure
 * (resize) notification. Phase 8 adds FDK-drawn window decorations
 * (see fdk_window_set_decorated) and window-state management
 * (maximize / minimize / restore / interactive resize, below).
 */

#ifndef FDK_WINDOW_H
#define FDK_WINDOW_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Options for fdk_window_create(). Zero-initialize (or pass NULL) for
 * sane defaults: title "FDK Application", 640x480, no min/max size
 * constraints. This is an "input struct" per docs/abi-policy.md — safe
 * to zero-init, fields only ever appended in the future. */
typedef struct fdk_window_options {
    const char *title;      /* copied internally; NULL = default title */
    fdk_i32 width;           /* <= 0 means "use default" (640) */
    fdk_i32 height;          /* <= 0 means "use default" (480) */
    /* Phase 9 appended fields (zero-init = the top-level defaults):
     * popup windows position at (x, y) relative to their PARENT
     * window's client area, take no WM chrome, grab input until
     * dismissed (click-outside / Escape deliver
     * FDK_EVENT_WINDOW_CLOSE_REQUEST), and are created via
     * fdk_window_create_popup(). */
    int popup;               /* nonzero = popup window             */
    fdk_i32 x;               /* parent-relative popup position     */
    fdk_i32 y;
    /* 1.3.2 appended: INPUT-TRANSPARENT popup (tooltips). Zero-init
     * (and every pre-1.3.2 caller) keeps the interactive-popup
     * contract. Nonzero with popup: the window takes an EMPTY input
     * region — pointer events fall through to whatever is underneath
     * — and never grabs pointer or keyboard: a passive hint window
     * the toolkit shows and dismisses on its own (fdk_widget_
     * set_tooltip). Currently set only through the toolkit's own
     * popup paths; applications building custom passive popups may
     * set it too. */
    int input_transparent;   /* nonzero = click-through popup       */
} fdk_window_options;

/* Creates a top-level window on `ctx`'s platform connection. Writes
 * the new window to *out_window on success.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx or out_window is NULL
 *   FDK_ERR_NOT_INITIALIZED  - ctx has no working platform connection
 *   FDK_ERR_OUT_OF_MEMORY    - allocation failure
 *   FDK_ERR_WINDOW_CREATE    - the platform backend refused/failed
 *
 * The window is NOT shown on the screen until fdk_window_show() is
 * called. On any failure, *out_window is left unchanged. */
fdk_result fdk_window_create(fdk_context *ctx,
                              const fdk_window_options *options,
                              fdk_window **out_window);

/* Phase 9: creates a POPUP window anchored at (x, y) relative to
 * `parent`'s client area — the platform layer for menus, combo
 * dropdowns, and transient panels. Popups take no WM/compositor
 * chrome, grab input while shown (X11: server pointer+keyboard
 * grab; Wayland: xdg_popup.grab), and deliver
 * FDK_EVENT_WINDOW_CLOSE_REQUEST when dismissed by a click outside
 * their bounds or the Escape key. Positioning beyond the screen is
 * the backend's to clamp (Wayland positioner constraints; X11
 * windows clip at the root). Popups must not outlive their parent
 * (destroying the parent force-destroys popups it anchors).
 *
 * Wayland note: xdg_popup.grab must cite an input serial; before
 * any input has arrived (serial 0) compositors may refuse the grab
 * — the popup still shows, it just won't dismiss on outside clicks
 * until then (documented, matching every toolkit's Wayland caveat
 * list). */
fdk_result fdk_window_create_popup(fdk_context *ctx, fdk_window *parent,
                                   fdk_i32 x, fdk_i32 y, fdk_i32 width,
                                   fdk_i32 height,
                                   fdk_window **out_window);

/* Maps the window (makes it visible). No-op if already shown. */
void fdk_window_show(fdk_window *window);

/* Unmaps the window (hides it without destroying it). No-op if
 * already hidden. The window's state (title, size) is preserved and
 * fdk_window_show() will restore visibility. */
void fdk_window_hide(fdk_window *window);

/* Destroys the window and releases its platform resources. `window`
 * must not be used after this call. Passing NULL is a safe no-op.
 * Does NOT call fdk_shutdown() on the owning context. */
void fdk_window_destroy(fdk_window *window);

/* Sets the window title. Safe to call before or after fdk_window_show().
 * `title` is copied internally; the caller retains ownership of the
 * string passed in. NULL is treated as an empty title. */
void fdk_window_set_title(fdk_window *window, const char *title);

/* ---- Window identity (1.1.0, M-4) ----------------------------------------
 *
 * The properties a desktop uses to recognize, group, and represent
 * this window: its ICON (taskbars, docks, window switchers, mission
 * control), its process identity, and its ICCCM window role.
 *
 * fdk_window_set_icon installs `width` x `height` pixels as the
 * window's icon. `pixels` is the toolkit's native ARGB8888 packing
 * (fdk_u32, A<<24 | R<<16 | G<<8 | B — the same layout
 * fdk_surface_create renders and _NET_WM_ICON carries), row-major,
 * no row padding, exactly width*height entries; the data is COPIED
 * before the call returns, so callers may free their buffer
 * immediately. One size is enough: desktops scale it (a 64x64
 * source covers dock to switcher on every mainstream desktop).
 * Repeated calls REPLACE the icon (the last call wins) — the
 * application wanting a multi-size set simply picks its best
 * single source; that is what every mainstream toolkit ships in
 * practice, and what both backends below implement.
 *
 * Per backend:
 *   - X11: the image replaces the _NET_WM_ICON property (EWMH:
 *     the desktop scales what it gets). Passing NULL (or
 *     non-positive dimensions) CLEARS the icon — the property goes
 *     away and the desktop falls back to its generic icon.
 *   - Wayland: xdg-toplevel-icon-v1 where the compositor advertises
 *     it (KDE Plasma 6.3+, others as they adopt the staging
 *     protocol): the LAST set wins, applied at the toplevel's next
 *     wl_surface.commit. Where the compositor does not implement
 *     the protocol the call is a documented no-op (one INFO line at
 *     most — Wayland gives no fallback path for window icons, same
 *     honest posture as every unsupported desktop feature).
 *
 * Window identity beyond the icon is set AUTOMATICALLY at window
 * creation and needs no API:
 *   - X11: _NET_WM_PID (the process id), WM_CLIENT_MACHINE (the
 *     host name), WM_WINDOW_ROLE (fdk_window_set_role below, or
 *     "main" for plain toplevels / "popup" for the popup family),
 *     and WM_CLASS (from fdk_init_options' app_id — since 1.0).
 *     Together these are what EWMH pagers, session managers, and
 *     _NET_WM_PINNING-style desktops read.
 *   - Wayland: the xdg_toplevel's app_id (from fdk_init_options)
 *     is the whole identity the protocol offers; there is no PID
 *     or machine property on Wayland by design.
 *
 * `pixels` NULL or width/height <= 0 clears the icon (both
 * backends). Dimensions above 512 are clamped (an icon is a
 * desktop affordance, not a second framebuffer). */
void fdk_window_set_icon(fdk_window *window, const fdk_u32 *pixels,
                         fdk_i32 width, fdk_i32 height);

/* Sets the ICCCM WM_WINDOW_ROLE for this window (X11; on Wayland
 * there is no role protocol — the call is a documented no-op, no
 * error). `role` is copied; NULL or "" reverts to the default
 * ("main" for toplevels, "popup" for popup windows). The role lets
 * a session manager or WM tell windows of the same class apart
 * ("toolbox" vs "main"); applications that never care simply never
 * call this. Safe before or after fdk_window_show(). */
void fdk_window_set_role(fdk_window *window, const char *role);

/* ---- Cursor shape (1.1.0-rc1; the full named set 1.1.0-rc2, S-5) --------
 *
 * The shape the pointer takes while hovering this window. FDK's own
 * resize-edge affordances (directional cursors over a resizable
 * window's borders) run BELOW this setting; an application-set shape
 * applies to the window as a whole and is re-applied automatically
 * every time the pointer enters (Wayland cursor state does not
 * survive leaving a surface; X11 tracks it as window state).
 *
 * The resolution order when several actors want the cursor, most
 * specific first:
 *   1. an application-set shape (this call) — the explicit request
 *      owns the whole window, edges included;
 *   2. a widget hover shape — without an app request, hovering a
 *      text-entry widget (Entry / TextView / Spin, or an editable
 *      Combo's entry) shows the I-beam automatically;
 *   3. FDK's resize-edge compass.
 *
 * The 1.1.0-rc2 set (S-5 / B-12) mirrors the cursor-shape-v1
 * protocol's semantic space — the same names CSS, GTK4 and Qt6 use.
 * Values 0 and 1 predate the set and are unchanged; everything after
 * is appended (ABI note: enum append is the safe-add policy, and no
 * existing value ever moves).
 *
 * The shape is a HINT: a backend or platform without the needed
 * mechanism keeps its default arrow — never a wrong shape. Where a
 * platform lacks an exact twin (the X core cursor font has no
 * "copy", for instance) FDK substitutes the nearest core glyph
 * AFTER first trying the XCursor theme through RENDER, so desktops
 * with a cursor theme get the real shapes; the substitutions are
 * documented in docs/platform-input.md's cursor table. */
typedef enum fdk_cursor_shape {
    FDK_CURSOR_DEFAULT = 0,   /* the platform's standard arrow */
    FDK_CURSOR_POINTER = 1,   /* the pointing hand over clickable things */
    FDK_CURSOR_TEXT = 2,      /* the I-beam over selectable text */
    FDK_CURSOR_CROSSHAIR = 3, /* precise selection (image editors) */
    FDK_CURSOR_WAIT = 4,      /* busy: the app cannot take input */
    FDK_CURSOR_PROGRESS = 5,  /* busy but still interactive */
    FDK_CURSOR_MOVE = 6,      /* an object moves with the pointer */
    FDK_CURSOR_COPY = 7,      /* a drag would copy */
    FDK_CURSOR_ALIAS = 8,     /* a drag would create a shortcut */
    FDK_CURSOR_NO_DROP = 9,   /* a drop is refused here */
    FDK_CURSOR_NOT_ALLOWED = 10, /* the action is not allowed */
    FDK_CURSOR_GRAB = 11,     /* a thing can be grabbed (open hand) */
    FDK_CURSOR_GRABBING = 12, /* a thing is grabbed (closed hand) */
    FDK_CURSOR_ALL_SCROLL = 13,     /* free two-axis panning */
    FDK_CURSOR_COL_RESIZE = 14,     /* a column separator drags */
    FDK_CURSOR_ROW_RESIZE = 15,     /* a row separator drags */
    FDK_CURSOR_ZOOM_IN = 16,  /* clicking zooms in */
    FDK_CURSOR_ZOOM_OUT = 17, /* clicking zooms out */
    FDK_CURSOR_HELP = 18,     /* a question-mark arrow: help here */
    FDK_CURSOR_CELL = 19,     /* spreadsheet cell selection (plus) */
    FDK_CURSOR_CONTEXT_MENU = 20,   /* a context menu is one click away */
    FDK_CURSOR_VERTICAL_TEXT = 21,  /* the I-beam rotated for vertical scripts */

    /* Sentinel: one past the last defined shape (NOT a shape). Lets
     * code iterate/validate the set without hard-coding 22; unknown
     * values in between are invalid, as ever. */
    FDK_CURSOR__COUNT = 22,
} fdk_cursor_shape;

/* Sets the window's hover cursor shape (fdk_cursor_shape above).
 * Applies immediately when the pointer is over the window, and on
 * every subsequent entry. No-op on an unknown shape value. */
void fdk_window_set_cursor(fdk_window *window, fdk_cursor_shape shape);

/* Requests a resize to (width, height). This is a request, not a
 * guarantee — the platform/compositor may clamp, ignore, or adjust it
 * (e.g. Wayland only actually resizes a toplevel in response to a
 * compositor-driven configure; FDK sends the request and the actual
 * resulting size arrives via FDK_EVENT_WINDOW_CONFIGURE). Both width
 * and height must be > 0. */
void fdk_window_resize(fdk_window *window, fdk_i32 width, fdk_i32 height);

/* Returns the window's last-known size (from the most recent configure
 * event, or the creation size if none has arrived yet) via *out_size.
 * Returns FDK_ERR_INVALID_ARGUMENT if window or out_size is NULL. */
fdk_result fdk_window_get_size(const fdk_window *window, fdk_size *out_size);

/* ---- HiDPI (Phase 3 completion; X11 since 1.4.5) ----
 *
 * Reports the window's current scale factor: the number of BUFFER
 * (physical) pixels per logical unit. 1.0 = the window's pixels are
 * its logical units.
 *
 * Per backend:
 *   - Wayland: the live wl_surface buffer scale / fractional-scale
 *     preference, updated by the compositor as the window moves
 *     between outputs.
 *   - X11 (1.4.5): detected ONCE at connect from the desktop's two
 *     conventions — the Xft.dpi resource string (xrdb /
 *     XSETTINGS-synced sessions; what GDK's X11 backend trusts) or,
 *     failing that, the screen's own pixels-per-millimeter metric
 *     (what a bare "Xvfb -dpi N" establishes). scale = round(dpi/96)
 *     clamped to [1, 3], INTEGER ONLY (the compositing path is the
 *     pixel-exact nearest-neighbor block blit — fractional scales
 *     would shimmer). X11 has no dynamic scale protocol: a dpi
 *     change needs a restart, same as every X toolkit.
 *
 * What scales and what doesn't:
 *   - fdk_surface_get_info() reports PHYSICAL dimensions (logical
 *     size x scale); raw-pixel drawing is physical by definition.
 *   - The WIDGET layer (fdk_window_set_content, decorations) stays
 *     LOGICAL: FDK composites the widget tree onto the physical
 *     buffer at the current scale automatically (fdk_window_paint).
 *   - Pointer/scroll/drag events arrive in LOGICAL coordinates on
 *     both backends (the X11 backend divides physical input by the
 *     scale); hit-testing and widget math never see the factor.
 *   - Apps that want crisp text/images at scale > 1 load fonts and
 *     images at size x scale themselves; fdk_window_get_scale tells
 *     them the factor.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - window or out_scale is NULL
 */
fdk_result fdk_window_get_scale(const fdk_window *window,
                                fdk_f32 *out_scale);

/* ---- Render-path introspection (1.4.18) ---------------------------------
 *
 * FDK is a HYBRID renderer: every pixel is rasterized on the CPU
 * (the surface layer's exact, antialiased software rasterizer —
 * identical output on every machine), while the PRESENTATION of a
 * finished frame takes one of two paths:
 *
 *   FDK_RENDER_PATH_CPU — the software path. The damaged rects
 *       travel to the server through shared memory (MIT-SHM/XPutImage
 *       on X11, wl_shm on Wayland).
 *
 *   FDK_RENDER_PATH_GL — the GPU present lane (X11 today). The
 *       damaged rects upload into a GL texture and one fullscreen
 *       composite carries the frame to the screen through GLX —
 *       byte-identical pixels, hardware-accelerated blit.
 *
 * Which path a window uses is decided by the backend at the first
 * framebuffer acquisition, from real capabilities (libGL loadable,
 * GLX present, a matching FBConfig, a hardware renderer — Mesa's
 * software rasterizers stay on the CPU path because it is faster
 * there). It is not a request: applications cannot force a path,
 * they can only observe it (and rely on the guaranteed fallback:
 * any lane failure permanently and seamlessly reverts that window
 * to FDK_RENDER_PATH_CPU, mid-frame, without losing the frame).
 *
 * Environment knobs (mirroring FDK_NO_MIT_SHM's shape):
 *   FDK_NO_GL=1    never load the GPU lane (opt out entirely)
 *   FDK_GL_FORCE=1 engage it even on software GL renderers
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - window or out_path is NULL
 *   FDK_ERR_NOT_INITIALIZED  - window has no live backend connection
 */
typedef enum fdk_render_path {
    /* Software present: CPU rasterization + SHM/copy transport. */
    FDK_RENDER_PATH_CPU = 0,
    /* Hybrid present: CPU rasterization + GPU composite (GLX). */
    FDK_RENDER_PATH_GL = 1
} fdk_render_path;

fdk_result fdk_window_get_render_path(const fdk_window *window,
                                      fdk_render_path *out_path);

/* Sets minimum/maximum size hints. Pass 0 for either dimension of
 * either struct to mean "no constraint" in that dimension. These are
 * hints handed to the platform (WM_NORMAL_HINTS on X11, xdg_toplevel
 * min/max on Wayland) — enforcement is the platform's responsibility,
 * not FDK's — with one exception: FDK's own resize-edge drags (see
 * fdk_window_set_resizable) also clamp to these limits, because on a
 * bare X server there is no window manager to do it. */
void fdk_window_set_size_limits(fdk_window *window, fdk_size min_size, fdk_size max_size);

/* ---- Window state (Phase 8) ----
 *
 * All of these are REQUESTS — the platform may clamp, ignore, or
 * adjust them, exactly like fdk_window_resize. FDK reports what
 * actually happened through FDK_EVENT_WINDOW_STATE (see fdk_event.h);
 * fdk_window_is_maximized()/is_minimized() read FDK's last-known
 * state, not the request. */

/* Asks for the window to fill the screen (EWMH _NET_WM_STATE
 * maximized under a conforming X11 WM; xdg_toplevel.set_maximized on
 * Wayland; on a bare X server FDK itself moves+resizes the window to
 * the full screen, remembering the prior geometry for unmaximize).
 * Returns FDK_ERR_UNSUPPORTED when the backend cannot maximize at
 * all; FDK_OK means the request was sent/applied — watch
 * FDK_EVENT_WINDOW_STATE for the confirmed state. */
/* Asks for the window to go FULLSCREEN — the whole screen, every
 * edge, no WM furniture (the presentation/video/kiosk state).
 * On X11 this is the EWMH _NET_WM_STATE_FULLSCREEN client message
 * under a conforming WM (with the bare-X geometry fallback when
 * nobody is listening — the same world-split maximize uses); on
 * Wayland it is xdg_toplevel.set_fullscreen. State confirmation
 * arrives as FDK_EVENT_WINDOW_STATE; fdk_window_is_fullscreen()
 * reads FDK's last-known truth. Returns FDK_ERR_UNSUPPORTED when
 * the backend cannot (e.g. a Wayland popup with no toplevel).
 * (1.4.10) */
fdk_result fdk_window_fullscreen(fdk_window *window);

/* Returns a fullscreen window to its former state. The compositor
 * restores the remembered geometry (the pre-fullscreen size is the
 * WM's business, not FDK's guess). */
fdk_result fdk_window_unfullscreen(fdk_window *window);

/* FDK's last-known fullscreen state (updated by the platform's
 * state events, not by request optimism). */
bool fdk_window_is_fullscreen(const fdk_window *window);

fdk_result fdk_window_maximize(fdk_window *window);

/* Returns a maximized window to its remembered (or compositor-
 * chosen) geometry. Same request/confirmation contract as
 * fdk_window_maximize(). */
fdk_result fdk_window_unmaximize(fdk_window *window);

/* Asks for the window to be minimized/iconified. On X11 this is the
 * ICCCM iconify request (any window manager honors it; without one,
 * FDK unmaps the window itself). On Wayland this is
 * xdg_toplevel.set_minimized — a fire-and-forget request: the
 * protocol has no un-minimize request and no acknowledgement, so
 * FDK marks the state optimistically and clears it when the
 * compositor next reports the window activated. */
fdk_result fdk_window_minimize(fdk_window *window);

/* Restores a minimized window (un-iconifies it). Unsupported on
 * Wayland (no protocol request exists — compositors unminimize via
 * activation, e.g. a taskbar click); returns FDK_ERR_UNSUPPORTED
 * there without changing anything. */
fdk_result fdk_window_restore(fdk_window *window);

/* FDK's last-known maximized/minimized state (from
 * FDK_EVENT_WINDOW_STATE). Note this is state-as-reported, not
 * request-as-sent — see the request/confirmation contract above. */
bool fdk_window_is_maximized(const fdk_window *window);
/* FDK's last-known minimized state. */
bool fdk_window_is_minimized(const fdk_window *window);

/* ---- Interactive resize (Phase 8) ----
 *
 * When enabled, a border-wide zone around the window's edges and
 * corners captures pointer drags and resizes the window — the CSD
 * (client-side decorations) answer to "the WM frame is gone, so who
 * draws the resize handles?". Drags are handed to the WM/compositor
 * where the platform supports it (EWMH _NET_WM_MOVERESIZE on X11,
 * xdg_toplevel.resize on Wayland); otherwise FDK drives the resize
 * itself, clamping to fdk_window_set_size_limits.
 *
 * The zone overlays the outermost pixels of the window content —
 * widgets living under it don't see presses there (the same
 * trade-off every CSD toolkit makes). fdk_window_set_decorated(true)
 * enables it automatically: a window with FDK's own title bar has no
 * WM frame left to resize it. Call set_resizable(false) AFTER
 * decorating for a fixed-size decorated window. */
void fdk_window_set_resizable(fdk_window *window, bool resizable);
/* Whether FDK resize edges are enabled. */
bool fdk_window_get_resizable(const fdk_window *window);

/* ---- FDK-drawn decorations (Phase 8) ----
 *
 * decorated == true: FDK draws its own title bar INSIDE the client
 * area — a themed band with the window title on the left and a close
 * button on the right — and asks the backend to drop the window
 * manager's own chrome (X11: _MOTIF_WM_HINTS). Dragging the band
 * moves the window where the backend allows it (X11). The close
 * button delivers a normal FDK_EVENT_WINDOW_CLOSE_REQUEST, so the
 * application's close semantics are identical to the WM's.
 *
 * The title bar is a widget subtree under the window's root: it is
 * themed like every catalog widget (fdk_theme_set_default repaints
 * it), fdk_window_set_title keeps its text in sync, and the content
 * widget (fdk_window_set_content) is laid out BELOW the band.
 *
 * Returns FDK_ERR_UNSUPPORTED when the backend cannot drop its own
 * decorations (Wayland until xdg-decoration lands) — in that case
 * nothing changes: drawing FDK's bar over the compositor's would
 * give every window two title bars.
 */
fdk_result fdk_window_set_decorated(fdk_window *window, bool decorated);

/* Whether FDK decorations are currently on. */
bool fdk_window_get_decorated(const fdk_window *window);

/* The title bar's font. NULL (the default) uses
 * fdk_font_load_system_default() — FDK bundles no font, so a system
 * with none renders a bar without title text (the close button and
 * drag still work; a warning is logged once). The font is BORROWED:
 * keep it alive until the window is destroyed or another font is
 * set. */
fdk_result fdk_window_set_decoration_font(fdk_window *window,
                                          fdk_font *font);

/* ---- Application shortcuts (1.3.3) -----------------------------------
 *
 * A per-window table of key bindings the application registers and
 * the toolkit dispatches. On every KEY_DOWN (including auto-repeat
 * presses) the window tests its table BEFORE the widget tree sees
 * the key — accelerator semantics: Ctrl+S fires Save whether focus
 * sits in an Entry, on a button, or nowhere. A match consumes the
 * event: the tree and the application's window callback never see
 * it. That precedence is also the documented footgun — do not
 * register editing keys the focused widget should own (Ctrl+C/V/X/
 * Z/A): the table would shadow the Entry's built-ins. Menu bar
 * accelerators (fdk_menu_item_set_shortcut) run AFTER this table.
 *
 * Specs are parsed by fdk_shortcut_parse (see fdk_event.h for the
 * grammar); "Ctrl+S" binds the physical S position, layout-stable.
 * Two registrations may use the same spec — the FIRST registered
 * fires (registration order wins, like every FDK dispatch table).
 *
 * The callback runs in the window's dispatch context: it may destroy
 * the window (the classic Ctrl+Q quit), open dialogs, mutate the
 * tree — the same contract any event handler gets.
 *
 * Shortcut ids: fdk_window_add_shortcut returns a small integer >= 1
 * (0 = registration failed; the spec did not parse). Remove with
 * fdk_window_remove_shortcut. The table dies with the window.
 */
typedef void (*fdk_shortcut_fn)(fdk_window *window, void *user_data);

/* Registers `spec` ("Ctrl+S", "F5", ...). Returns the shortcut id
 * (>= 1), or 0 when the window is NULL or the spec does not parse
 * (FDK_ERR_INVALID_ARGUMENT is also returned through the public
 * error API for diagnostics). */
int fdk_window_add_shortcut(fdk_window *window, const char *spec,
                            fdk_shortcut_fn on_press, void *user_data);
/* Removes a registration (id from add_shortcut). Unknown ids are a
 * quiet no-op — removal code runs in arbitrary teardown orders. */
void fdk_window_remove_shortcut(fdk_window *window, int id);
/* The number of live registrations (diagnostics / conflict UIs). */
size_t fdk_window_shortcut_count(const fdk_window *window);

#ifdef __cplusplus
}
#endif

#endif /* FDK_WINDOW_H */
