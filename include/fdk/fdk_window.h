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
#include "fdk_core.h" /* fdk_render_mode */

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
    /* 1.0.0 appended: INPUT-TRANSPARENT popup (tooltips). Zero-init
     * (and every pre-1.0.0 caller) keeps the interactive-popup
     * contract. Nonzero with popup: the window takes an EMPTY input
     * region — pointer events fall through to whatever is underneath
     * — and never grabs pointer or keyboard: a passive hint window
     * the toolkit shows and dismisses on its own (fdk_widget_
     * set_tooltip). Currently set only through the toolkit's own
     * popup paths; applications building custom passive popups may
     * set it too. */
    int input_transparent;   /* nonzero = click-through popup       */
    /* 1.0.0 appended: per-window render-mode override (fdk_core.h's
     * fdk_render_mode). 0 = inherit the context's mode (the zero-init
     * default every existing caller keeps). A window may narrow the
     * context's choice (GPU context, CPU window) — the effective mode
     * is resolved at creation and never re-negotiated. */
    int render_mode;
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
/* The context this window lives on (the print dialog and other
 * window-spawned helpers need it; the accessor is the clean seam). */
fdk_context *fdk_window_get_context(fdk_window *window);

/* The window's effective render mode (resolved at creation: the
 * window's own override, else the context's — see fdk_core.h's
 * fdk_render_mode). A per-window diagnostics accessor over the
 * context-level query. */
fdk_render_mode fdk_window_render_mode(const fdk_window *window);

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

/* ---- Window identity (1.0.0, M-4) ----------------------------------------
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

/* ---- Window opacity + audible bell (the terminal round) ----------------
 *
 * Two things every terminal emulator asks of its window that the
 * catalog never grew. Both are documented-honest about the backend
 * split: X11 speaks both natively, Wayland (as of the protocols FDK
 * consumes — xdg-shell has neither) does not, and the calls are
 * clean no-ops returning FDK_ERR_UNSUPPORTED there rather than
 * half-fakes. FDK_ERR_UNSUPPORTED is a REPORT, not a failure to
 * handle: an app that wants a translucent terminal on Wayland today
 * composites its own background (fdk-term's profile opacity does
 * exactly that per-backend). */

/* Sets the whole-window OPACITY (the EWMH _NET_WM_WINDOW_OPACITY
 * convention: 0.0 fully transparent, 1.0 fully opaque, the value
 * clamped into [0,1] before it reaches the platform). This is the
 * COMPOSITOR's per-window alpha — the WM/compositor blends the
 * entire window, contents and decorations alike, over whatever is
 * behind it; it is not a per-pixel alpha channel and no drawing
 * API participates. Reading it back returns the clamped value last
 * set (1.0 when never set).
 *
 * X11: the CARDINAL property is written on the window; a compositing
 * manager honors it (a bare X server without one shows the window
 * fully opaque — the property is then inert, which is the EWMH
 * contract, not an FDK limitation). Wayland: FDK_ERR_UNSUPPORTED.
 *
 * A terminal asks for this with its "background opacity" profile
 * setting; a media player with its mini-player fade; an app that
 * never calls it pays nothing. Safe before or after
 * fdk_window_show(); the property follows the window. */
fdk_result fdk_window_set_opacity(fdk_window *window, double opacity);

/* The clamped opacity last set (1.0 when never set; 1.0 on a NULL
 * window, the defensive convention). */
double fdk_window_get_opacity(const fdk_window *window);

/* Rings the display's AUDIBLE bell on this window's screen — the
 * classic terminal BEL (0x07) answer. X11: XBell on the context's
 * connection at the given percent (0 = the server's default volume;
 * negative inverts the pitch where the server supports it, per
 * XSetBellPercent conventions — passed through clamped to [-100,
 * 100]). Wayland: no bell protocol exists in xdg-shell, so
 * FDK_ERR_UNSUPPORTED — a Wayland terminal answers BEL with its
 * VISUAL bell (flash) and/or a notification, which is what every
 * real Wayland terminal does.
 *
 * BEL is not a stream event: it is the terminal APPLICATION's job
 * to call this when its parser sees 0x07 (FDK never listens to
 * bytes it does not own). Returns FDK_OK on X11 even when the
 * server has the bell disabled (that is the server's state, not a
 * protocol failure). */
fdk_result fdk_window_bell(fdk_window *window, int percent);

/* Sets the ICCCM WM_COMMAND — the command a session manager
 * re-executes to restart this application in its current state
 * (1.0.0, D-5; the X11 session lane, see fdk_session.h). `argv` is
 * an argv-style array of NUL-terminated strings (argv[0] the
 * program, then its arguments); all strings are copied, the array
 * need only live for the call. X11 writes the WM_COMMAND property
 * (type STRING, NUL-separated argv); on Wayland there is no
 * equivalent — the call is a documented no-op, no error.
 *
 * ICCCM's multi-window rule is the application's to keep: exactly
 * ONE top-level window of the client should carry a nonzero
 * WM_COMMAND (the "main" window); FDK writes the property on
 * whichever window it is called for and never second-guesses.
 * NULL `argv` or argc <= 0 CLEARS the command.
 *
 * The property is ALSO the WM_SAVE_YOURSELF ack: when the session
 * manager asks the client to save (FDK_EVENT_SESSION_SAVE), FDK
 * rewrites this window's WM_COMMAND after the event callback
 * returns — with the CURRENT command (zero-length when none was
 * set) — because the PropertyNotify that rewrite generates is the
 * acknowledgment the session manager waits for (ICCCM appendix C).
 * Calling this INSIDE the FDK_EVENT_SESSION_SAVE handler with the
 * updated restart arguments is the intended pattern. */
void fdk_window_set_command(fdk_window *window,
                            const char *const *argv, int argc);

/* ---- Cursor shape (1.0.0-rc1; the full named set 1.0.0-rc2, S-5) --------
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
 * The 1.0.0-rc2 set (S-5 / B-12) mirrors the cursor-shape-v1
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

/* ---- Input region (1.0.0-rc2, S-4 / A-06) -----------------------------
 *
 * The window's INPUT region: the set of rectangles (window-LOCAL,
 * LOGICAL coordinates — the same space pointer events deliver)
 * inside which the window receives pointer and touch input. Clicks
 * on the window but OUTSIDE the region fall through to whatever is
 * beneath it — the mechanism notification overlays, auto-hide
 * reveal strips and shaped panels are built on (the 1.0.0
 * input_transparent tooltip was the degenerate whole-window case).
 *
 * The region is a HINT shaped by the platform's native mechanism
 * (XShape on X11, wl_surface.set_input_region on Wayland); a
 * backend without the mechanism keeps the window fully interactive
 * and the call returns FDK_ERR_UNSUPPORTED. The region persists
 * across resizes (window-relative); clearing it (NULL / 0 rects)
 * restores full input. A zero-area rect set (e.g. one {0,0,0,0}
 * rect) yields the EMPTY region — fully click-through, the
 * input_transparent behavior on demand.
 *
 * On extension surfaces use fdk_ext_surface_set_input_region()
 * (fdk_extend.h) — the same semantics through the add-on API. */

/* The maximum number of rectangles one input region may carry (the
 * platform op's fixed working array; aligned with the damage cap). */
#define FDK_INPUT_REGION_MAX 64

/* Sets the window's input region from `count` window-local logical
 * rectangles (inclusive of their edges). rects may be NULL only
 * when count is 0, which CLEARS the region (full input again).
 * FDK_ERR_INVALID_ARGUMENT on NULL window, negative count, count
 * above FDK_INPUT_REGION_MAX, or NULL rects with count > 0.
 * FDK_ERR_UNSUPPORTED when the backend lacks the mechanism. */
fdk_result fdk_window_set_input_region(fdk_window *window,
                                       const fdk_rect *rects,
                                       fdk_i32 count);

/* ---- Activation + startup notification (1.0.0, S-6 / B-04) -------------
 *
 * "Activation" is the desktop's focus-stealing-prevention machinery:
 * the difference between a window DEMANDING attention and a window
 * the user just asked for. The two halves:
 *
 *   1. fdk_window_activate() — bring THIS window to the user (focus,
 *      raise, unminimize as the platform allows): the "click this
 *      notification to jump to the conversation" case. Both backends
 *      ride their native protocol — Wayland xdg-activation-v1 (a
 *      compositor-minted token vouches for the request; a tokenless
 *      beg is exactly what the protocol exists to refuse), X11 the
 *      EWMH _NET_ACTIVE_WINDOW message carrying the last USER event
 *      timestamp (the number a WM weighs against its own focus
 *      history — GTK's and Qt's exact posture).
 *
 *   2. fdk_window_activation_token() — the LAUNCHER half. An FDK
 *      application spawning another program hands the child a token
 *      through its environment so the child's first window arrives
 *      pre-vouched: set XDG_ACTIVATION_TOKEN=<token> (Wayland) or
 *      DESKTOP_STARTUP_ID=<token> (X11) in the child's environ.
 *      The launched side needs no code: FDK captures both variables
 *      at fdk_init (and unsets them in its own environment, per both
 *      specifications, so grandchildren do not inherit a stale
 *      token), X11 sets _NET_STARTUP_ID on every window created
 *      while the id is held, Wayland spends it on the first
 *      activate() — the one-launch-one-token contract.
 *
 * The X11 token carries the full startup-notification feedback loop
 * client-side (no D-Bus, no dependencies): minting emits the
 * "new:" state to the root window through _NET_STARTUP_INFO_BEGIN
 * chunks (the message openbox/metacity-family WMs listen for), and
 * the first SHOW of a window created under a captured id emits the
 * matching "remove:" — the launch feedback ends when the app is up,
 * the honest end of the protocol's story. Wayland tokens are
 * compositor-minted (xdg_activation_token_v1.done) — sway, KDE and
 * GNOME all implement the protocol; where a compositor does not,
 * minting returns FDK_ERR_UNSUPPORTED and the launcher simply
 * spawns without a token (the child's activate() then mints its
 * own fresh token — the degraded-but-legal path).
 *
 * Tokens are one-shot by specification: the launcher mints, the
 * child spends. fdk_window_activate() on the child side always
 * works standalone — a self-minted token vouches for itself on
 * compositors that honor recent-input serials (the set_serial the
 * mint attaches), which is the best any client can do without a
 * vouching parent. */

/* Asks the compositor/WM to bring this window to the user — focus,
 * raise, unminimize (the "present" verb in GTK/Qt terms). Safe any
 * time after create; a no-op on a hidden window (it activates when
 * shown). Returns FDK_ERR_INVALID_ARGUMENT on NULL window,
 * FDK_ERR_UNSUPPORTED when the backend has no activation mechanism
 * (a connection whose compositor offers neither xdg-activation nor
 * a WM — rare; the honest answer rather than a silent no-op). */
fdk_result fdk_window_activate(fdk_window *window);

/* Mints an activation token tied to this window for a LAUNCHER: the
 * string a parent process passes to the child it spawns (see the
 * section above). Writes at most cap-1 bytes plus a NUL to out and
 * returns FDK_OK; FDK_ERR_INVALID_ARGUMENT on bad arguments or a
 * buffer too small (a 512-byte buffer always suffices — the X11 id
 * is short, the Wayland token bounded well below), FDK_ERR_UNSUPPORTED
 * when the platform cannot mint (no xdg-activation on Wayland).
 * The token is spent by the CHILD (never by the minter); minting
 * does not activate anything by itself. */
fdk_result fdk_window_activation_token(fdk_window *window,
                                       char *out, size_t cap);

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

/* ---- HiDPI (Phase 3 completion; X11 since 1.0.0) ----
 *
 * Reports the window's current scale factor: the number of BUFFER
 * (physical) pixels per logical unit. 1.0 = the window's pixels are
 * its logical units.
 *
 * Per backend:
 *   - Wayland: the live wl_surface buffer scale / fractional-scale
 *     preference, updated by the compositor as the window moves
 *     between outputs.
 *   - X11 (1.0.0): detected ONCE at connect from the desktop's two
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

/* ---- Render-path introspection (1.0.0) ---------------------------------
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
 *   FDK_RENDER_PATH_GL — the GPU present lane (both backends:
 *       X11/GLX since 1.0.0, Wayland/EGL since 1.0.0). The
 *       damaged rects upload into a GL texture and one fullscreen
 *       composite carries the frame to the screen through GLX or
 *       EGL — byte-identical pixels, hardware-accelerated blit.
 *
 * Which path a window uses is decided by the backend at the first
 * framebuffer acquisition, from real capabilities (the GL libraries
 * loadable, a matching visual/config, a hardware renderer — Mesa's
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
    /* Hybrid present: CPU rasterization + GPU composite
     * (GLX on X11, EGL on Wayland). */
    FDK_RENDER_PATH_GL = 1
} fdk_render_path;

fdk_result fdk_window_get_render_path(const fdk_window *window,
                                      fdk_render_path *out_path);

/* fdk_window_get_render_backend() (2026-11-11): WHICH GPU present
 * lane carries this window's frames — the finer-grained companion
 * of fdk_window_get_render_path(). The lane order under AUTO is
 * fixed by policy: Vulkan first, then OpenGL, then the CPU path.
 *
 * The answer is read live off the backend's own state (a mid-session
 * downgrade reports CPU honestly, the same contract render_path
 * lives under). Applications that REQUIRE a hardware backend
 * (the gpu_strict shape) check this after showing a window and
 * refuse to run on CPU.
 *
 * Forcing (environment):
 *   FDK_GPU_BACKEND=vulkan  only the Vulkan lane may present
 *   FDK_GPU_BACKEND=opengl  only the GL lanes (GLX/EGL) may present
 *   FDK_GPU_BACKEND=auto    the priority order (the default)
 *   FDK_GL_VULKAN=1         alias of =vulkan
 *   FDK_GL_OPENGL=1         alias of =opengl
 * A FORCED backend is a requirement, not a preference: when it
 * cannot engage, the window presents on the CPU and this query
 * reports FDK_RENDER_BACKEND_CPU — a strict application sees the
 * honest answer and exits; FDK never silently substitutes the
 * other GPU backend.
 *
 * FDK_NO_GL=1 keeps its master opt-out meaning (nothing GPU loads).
 * The renderer-policy gates still apply per lane: software
 * rasterizers (llvmpipe, lavapipe, SwiftShader) stay refused
 * without FDK_GL_FORCE=1.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - window or out_backend is NULL
 *   FDK_ERR_NOT_INITIALIZED  - window has no live backend connection
 */
typedef enum fdk_render_backend {
    /* The classic CPU present path (SHM on Wayland, XImage/MIT-SHM
     * on X11) — including every downgraded window. */
    FDK_RENDER_BACKEND_CPU = 0,
    /* The Vulkan present lane: the staging frame rides a
     * host-visible buffer into the swapchain image (queue transfer
     * + present through VK_KHR_xlib_surface /
     * VK_KHR_wayland_surface). */
    FDK_RENDER_BACKEND_VULKAN = 1,
    /* The OpenGL present lanes: GLX on X11, EGL on Wayland. */
    FDK_RENDER_BACKEND_OPENGL = 2
} fdk_render_backend;

fdk_result fdk_window_get_render_backend(const fdk_window *window,
                                         fdk_render_backend *out_backend);

/* The device string behind the active GPU lane (e.g. "llvmpipe
 * (LLVM 19.1.7, 256 bits)", "NVIDIA GA10x", "AMD RADV NAVI10") —
 * "<none>" on the CPU path, "<unknown>" when the lane engaged but
 * never read its device string. A diagnostic label for logs and
 * strict-app error messages, never an identity. */
const char *fdk_window_render_device(const fdk_window *window);

/* fdk_window_gpu_paint_stats() (2026-11-12): what the GPU paint
 * engine ACTUALLY did — the "is the GPU really rendering, or did it
 * fall back to CPU?" question answered with counters, not vibes.
 *
 * Meaningful in FDK_RENDER_GPU mode (fdk_window_render_mode()):
 * there the whole draw stream executes as SDF-evaluated GPU quads
 * (one shader, one vertex stream) — quads/batches are the GPU's
 * actual draw work. In HYBRID/CPU modes every field reads zero:
 * pixels are CPU-rasterized there and only the PRESENT rides the
 * GPU lane (see fdk_render_path / fdk_window_get_render_backend
 * for that side).
 *
 * Frame-scoped numbers (frame_quads, frame_batches) cover the frame
 * in flight — they reset at every present; lifetime totals keep
 * running. total_frames counts completed presents (the frame
 * boundary the lanes call at swap time), so a live window's
 * total_frames > 0 while frame_quads shows THIS frame's load.
 *
 * The read is a snapshot without locks: read it from the same
 * thread that paints. Can fail with FDK_ERR_INVALID_ARGUMENT
 * (window or out_stats NULL). */
typedef struct fdk_gpu_paint_stats {
    unsigned active;            /* engine attached to the window surface */
    unsigned dead;              /* engine marked dead (lane downgrade)   */
    unsigned frame_quads;       /* quads emitted since the last present  */
    unsigned frame_batches;     /* GPU batches since the last present    */
    unsigned long long total_quads;      /* lifetime quads               */
    unsigned long long total_batches;    /* lifetime GPU batches         */
    unsigned long long total_frames;     /* completed presents           */
    unsigned long long atlas_uploads;    /* glyph-mask shelf uploads     */
    unsigned long long texture_uploads;  /* texture realizes/uploads     */
} fdk_gpu_paint_stats;

fdk_result fdk_window_gpu_paint_stats(const fdk_window *window,
                                      fdk_gpu_paint_stats *out_stats);

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
 * (1.0.0) */
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

/* ---- Application shortcuts (1.0.0) -----------------------------------
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


/* ---- P3: idle-inhibit (zwp_idle_inhibit_manager_v1) -----------------------
 *
 * Engages (or releases) a Wayland idle inhibitor on this window's
 * surface: the compositor will not idle-suspend/lock while the window
 * is visible and the inhibitor lives. Idempotent; answers
 * FDK_ERR_UNSUPPORTED when the compositor lacks the protocol (check
 * FDK_EXT_CAP_IDLE_INHIBIT) — on X11 there is no such protocol and
 * the screensaver conversation belongs to the app (the
 * org.freedesktop.ScreenSaver D-Bus interface), same as every X11
 * toolkit. */
fdk_result fdk_window_set_idle_inhibited(fdk_window *window,
                                         int inhibited);
