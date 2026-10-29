/*
 * platform_internal.h — internal platform backend interface
 *
 * This is the seam described in docs/architecture.md ("no backend
 * leakage"): everything above this line (src/core, src/window, and
 * eventually src/widget etc.) talks only to `fdk_platform`, never to
 * Xlib/xcb/wayland-client types directly. Everything below this line
 * (src/platform/x11, src/platform/wayland) implements this interface
 * and is the ONLY place backend headers may be #included.
 *
 * Not part of the public API — never installed.
 */

#ifndef FDK_PLATFORM_INTERNAL_H
#define FDK_PLATFORM_INTERNAL_H

#include "fdk/fdk_core.h"
#include "fdk/fdk_dnd.h"
#include "fdk/fdk_error.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_types.h"
#include "fdk/fdk_window.h"

/* Opaque, backend-owned window handle. Cast to whatever the backend
 * actually needs internally (an X11 backend stores its Window ID +
 * Display pointer here; Wayland stores wl_surface / xdg_toplevel
 * pointers, etc.) — see
 * src/platform/x11/x11_platform.c and
 * src/platform/wayland/wayland_platform.c. The public fdk_window
 * (src/window/window_internal.h) holds one of these plus
 * backend-agnostic bookkeeping (event callback, cached size).
 */
typedef struct fdk_platform_window fdk_platform_window;

/* Opaque, backend-owned connection handle — one per fdk_context. */
typedef struct fdk_platform_connection fdk_platform_connection;

/* A platform backend delivers translated events by calling this
 * dispatcher, which src/window/ implements: it looks up the owning
 * fdk_window from the platform_window pointer and invokes that
 * window's registered fdk_event_callback_fn, if any. Backends never
 * call an application's callback directly. */
typedef void (*fdk_platform_dispatch_fn)(fdk_platform_window *pwindow,
                                          const fdk_event_data *event,
                                          void *dispatch_user_data);

/* One implementation of this struct exists per backend
 * (g_fdk_x11_backend, g_fdk_wayland_backend — see each backend's .c
 * file). fdk_init() (src/core/context.c) selects one based on
 * fdk_init_options.backend / FDK_PLATFORM_AUTO detection and stores
 * a pointer to it in the context; every other internal call site goes
 * through ctx->platform_ops rather than caring which backend it is. */
/* A platform framebuffer handed out by a backend's
 * window_get_framebuffer op (the machinery behind fdk_surface —
 * see include/fdk/fdk_surface.h). `stride` is in fdk_u32 PIXEL units
 * (not bytes), >= width; rows start at pixels + (size_t)y * stride.
 * The layout is XRGB8888 (R<<16|G<<8|B, top byte ignored) on both
 * current backends. The pixels pointer's lifetime is governed by the
 * backend: valid until the next get_framebuffer call on that window
 * and — for Wayland, whose buffers are handed to the compositor at
 * present — until window_present, after which a fresh acquisition
 * must be made. */
typedef struct fdk_platform_framebuffer {
    fdk_u32 *pixels;
    fdk_i32 width;
    fdk_i32 height;
    fdk_i32 stride; /* in fdk_u32 units */
} fdk_platform_framebuffer;

/* Damage region handed to window_present (the machinery behind
 * fdk_surface_present's partial-redraw contract — see
 * include/fdk/fdk_surface.h, "Damage tracking"). Rects are in
 * surface-local pixel coordinates, half-open [x, x+w) x [y, y+h),
 * NOT clamped to the framebuffer (backends clamp — they know the
 * real bounds). `full` set means "the whole surface changed" and
 * supersedes the rect list; count == 0 with full == 0 means
 * "nothing changed" (present should no-op). The struct is
 * value-copied / read-only from the backend's perspective. */
#define FDK_PLATFORM_DAMAGE_RECTS 64

/* The input-region op's working-array cap (S-4 / A-06, rc2) — the
 * public twin is FDK_INPUT_REGION_MAX (fdk_window.h); keep the two
 * numerically identical (the window layer validates against the
 * public one before the op ever runs). */
#define FDK_PLATFORM_INPUT_REGION_MAX 64
typedef struct fdk_platform_damage {
    fdk_rect rects[FDK_PLATFORM_DAMAGE_RECTS];
    int count; /* valid rects, <= FDK_PLATFORM_DAMAGE_RECTS */
    int full;  /* 1 = whole surface damaged */
} fdk_platform_damage;

/* The pointing-hand cursor in the window_set_cursor op's shape space
 * (1.1.0-rc1): 0 stays "default arrow", 1..8 are the resize compass
 * (fdk_window_resize_edge), and 9 is the application-requested hand
 * over clickable content. Plain #define, not an enum, so both backends
 * and the core can use it without a shared-header dependency; the
 * PUBLIC face of this is fdk_cursor_shape (fdk_window.h), mapped
 * inside the window layer. */
#define FDK_PLATFORM_CURSOR_POINTER 9

/* The full named set (1.1.0-rc2, S-5 / B-12): public shapes 2..21
 * (FDK_CURSOR_TEXT .. FDK_CURSOR_VERTICAL_TEXT) ride the op space at
 * FDK_PLATFORM_CURSOR_NAMED_BASE + (public - 2), i.e. 32..51 — kept
 * clear of the compass and the hand. The window layer maps; the
 * backends own the per-platform encoding (cursor-shape-v1 enum /
 * XCursor theme name / core cursor-font glyph, per-backend). */
#define FDK_PLATFORM_CURSOR_NAMED_BASE 32
#define FDK_PLATFORM_CURSOR_NAMED_COUNT 20

/* True when `shape` is a valid window_set_cursor op value (the
 * compass, the hand, or the named range). Used by both backends'
 * argument guards so the ranges can never drift apart. */
#define FDK_PLATFORM_CURSOR_OP_VALID(shape)                          \
    (((shape) >= 0 && (shape) <= FDK_PLATFORM_CURSOR_POINTER) ||     \
     ((shape) >= FDK_PLATFORM_CURSOR_NAMED_BASE &&                   \
      (shape) < FDK_PLATFORM_CURSOR_NAMED_BASE +                     \
                    FDK_PLATFORM_CURSOR_NAMED_COUNT))

/* ---- Shared XCursor theme loader (src/platform/cursor_theme.c) ----
 *
 * Both backends need the same thing the 1.1.4 Wayland backend built:
 * cursor images as ARGB pixels from the system's XCursor theme files,
 * with no libxcursor dependency (the container format is small and
 * documented — see cursor_theme.c's format block). Wayland uploads
 * them to a wl_shm buffer for wl_pointer.set_cursor; X11 (rc2, S-5)
 * wraps them into RENDER cursors for the shapes the core cursor font
 * lacks. `names` is a NULL-terminated candidate list tried in order
 * (modern CSS name first, legacy name second); the pixels are
 * fdk_alloc'd and owned by the caller. FDK_ERR_UNSUPPORTED = no
 * candidate found in any theme (the honest-degradation signal). */
fdk_result fdk__cursor_theme_load(const char *const *names, int nominal,
                                  uint32_t **out_pixels, int *out_w,
                                  int *out_h, int *out_hx, int *out_hy);

/* The candidate NAME list per NAMED shape (cursor_theme.c owns the
 * table; both backends ride it — one source of truth for the
 * modern/legacy spellings). Row i is public shape (i + 2), i.e. op
 * FDK_PLATFORM_CURSOR_NAMED_BASE + i; NULL-terminated rows. */
extern const char *const
    fdk__cursor_named_names[FDK_PLATFORM_CURSOR_NAMED_COUNT][4];

typedef struct fdk_platform_ops {
    /* Human-readable backend name for logging ("x11", "wayland"). */
    const char *name;

    /* Attempts to connect to the platform (X11 display / Wayland
     * compositor). On success, writes a new connection handle to
     * *out_conn and returns FDK_OK. On failure (no display reachable,
     * protocol setup failure, required global missing) returns
     * FDK_ERR_NO_DISPLAY or FDK_ERR_PLATFORM_INIT and leaves
     * *out_conn unchanged — this backend is then considered
     * unavailable and (for FDK_PLATFORM_AUTO) the other backend is
     * tried. `dispatch` and `dispatch_user_data` are stored by the
     * backend and used for all future event delivery on this
     * connection. */
    fdk_result (*connect)(fdk_platform_dispatch_fn dispatch,
                           void *dispatch_user_data, const char *app_id,
                           fdk_platform_connection **out_conn);

    /* Releases the connection and everything still open on it (any
     * windows the caller failed to destroy first are force-destroyed,
     * with a warning logged — callers should not rely on this, see
     * fdk_shutdown()'s documented behavior in fdk_core.h). */
    void (*disconnect)(fdk_platform_connection *conn);

    /* Returns a pollable file descriptor for this connection's event
     * source (the X11 connection fd, or wl_display_get_fd()). Used by
     * the event loop (src/core/context.c) to block in poll()/epoll()
     * without busy-spinning. Returns -1 if the backend has no single
     * fd to offer (not expected for either current backend, but the
     * interface allows for it). */
    int (*get_event_fd)(fdk_platform_connection *conn);

    /* Processes whatever events are currently pending on the
     * connection (reads from the fd, translates, calls `dispatch` for
     * each). Does not block — call after poll()/epoll() indicates the
     * fd is readable, or opportunistically to drain a burst. Returns
     * the number of events processed (>= 0), or a negative fdk_result
     * value cast to int on unrecoverable connection failure (the
     * caller should treat the connection as dead and stop the loop —
     * see fdk_run()'s handling in src/core/context.c). */
    int (*dispatch_pending)(fdk_platform_connection *conn);

    /* OPTIONAL (1.3.5): the backend's own next timer deadline, in
     * monotonic milliseconds from NOW — the one piece of timing the
     * core timer queue cannot know about (Wayland's client-driven
     * key repeat). Returns 1 with *out_ms_until set when a deadline
     * exists (values <= 0 mean "due now"), 0 when the backend holds
     * no pending timing of its own. The pump caps its poll() wait
     * with this exactly like timers_next_deadline(), so an
     * indefinite fdk_run() still wakes to fire platform deadlines.
     * Backends without self-timed state (X11: repeat is
     * server-side) leave it NULL. */
    int (*next_wakeup_ms)(fdk_platform_connection *conn,
                          long long *out_ms_until);

    /* ---- OPTIONAL test seams (1.3.5) ----
     *
     * Production code NEVER calls these. sway headless cannot
     * deliver wl_keyboard.key to clients at all (the seat never
     * adopts a virtual keyboard's keymap — protocol-traced in
     * 1.3.3), which leaves the Wayland keyboard listener bodies
     * unreachable from the integration rig. These two hooks run
     * those REAL listener bodies (key press/release with the full
     * key-repeat arm/disarm discipline, and repeat_info rate/delay
     * updates) against the live connection, so the suite can drive
     * the state machine end-to-end without a hardware keyboard.
     * test_key returns 1 when the event was deliverable (keyboard
     * focus + keymap present), 0 when the seat-less/compositor
     * configuration makes it a no-op — the suite's skip signal.
     * Only the Wayland backend implements them; X11 covers key
     * delivery with XSendEvent against a real display. */
    int (*test_key)(fdk_platform_connection *conn, uint32_t key,
                    int pressed);
    void (*test_repeat_info)(fdk_platform_connection *conn,
                             int32_t rate, int32_t delay);
    /* Installs the standard default keymap through the same apply
     * path the wl_keyboard::keymap event takes — the empty keymap
     * sway headless's virtual keyboard serves leaves xkb_state NULL
     * and the listener bodies unreachable. Returns 1 when a live
     * xkb_state exists afterwards (the suite's go signal). */
    int (*test_default_keymap)(fdk_platform_connection *conn);

    /* OPTIONAL test seam (1.1.0, M-3): the REAL wl_touch listener
     * bodies, driven directly — sway headless's seat carries no
     * touch capability (wlroots has no virtual-touch protocol), so
     * the rig cannot reach them through the wire. phase:
     * 0=down 1=motion 2=up 3=cancel; (x,y) logical surface-local
     * (ignored for up/cancel); `pwin` names the down target's
     * surface. Returns 1 when the down was deliverable. Only the
     * Wayland backend implements it. */
    int (*test_touch)(fdk_platform_connection *conn,
                      fdk_platform_window *pwin, int phase,
                      uint32_t id, double x, double y);

    /* OPTIONAL test seam (1.1.0-rc2, S-5): drives the cursor APPLY
     * path for any op shape against the live connection — the
     * production window_set_cursor op needs a real input serial
     * (a pointer enter), which the headless compositor only mints
     * through the injector rig. Returns the cursor-shape-v1 ENUM
     * actually cited in the wp_cursor_shape_device_v1.set_shape
     * request (the suite's per-shape mapping pin), or -1 when this
     * compositor/connection offers no cursor-shape manager or no
     * seat pointer (the honest-skip signal). Only the Wayland
     * backend implements it. */
    int (*test_cursor_apply)(fdk_platform_connection *conn,
                             fdk_platform_window *pwin, int op_shape);

    /* OPTIONAL (1.1.0, M-1 IME): push the focused text widget's
     * context (UTF-8 text + caret byte offset; NULL text = the focus
     * moved to a non-text widget) to the platform's input-method
     * machinery. Wayland: set_surrounding_text + commit on the
     * enabled text-input-v3. X11 leaves it NULL (XIM tracks its own
     * state through the IC). Called by the window layer's
     * post-routing hook — focus gains and IME event routing. */
    void (*ime_update_surrounding)(fdk_platform_window *pwindow,
                                   const char *text, int cursor);

    /* OPTIONAL (1.1.0, S-3 idle inhibition): engage/release the
     * compositor's native idle inhibition for this window's
     * surface (Wayland: zwp_idle_inhibit_manager_v1). Returns
     * FDK_ERR_UNSUPPORTED when the backend/compositor has no
     * native mechanism — the idle module then rides the
     * org.freedesktop.ScreenSaver D-Bus path instead. Idempotent. */
    fdk_result (*window_set_idle_inhibit)(fdk_platform_window *pwindow,
                                          int inhibit);

    /* OPTIONAL (1.1.0, S-6 activation): bring this window to the
     * user (Wayland: xdg-activation-v1 — a compositor-minted token,
     * or the one captured from XDG_ACTIVATION_TOKEN at connect
     * while unconsumed; X11: the _NET_ACTIVE_WINDOW root message
     * with the last user event timestamp, XRaiseWindow +
     * XSetInputFocus under bare X). FDK_ERR_UNSUPPORTED when the
     * backend has no mechanism at all. */
    fdk_result (*window_activate)(fdk_platform_window *pwindow);

    /* OPTIONAL (1.1.0, S-6): mint an activation token tied to this
     * window for a LAUNCHER to pass to a child process (Wayland:
     * xdg_activation_token_v1 done string, fetched through a PRIVATE
     * event queue + one bounded roundtrip — reentrancy-safe from
     * inside an event handler, the clipboard precedent; X11: a
     * startup-notification-format id + the "new:" root message).
     * Writes cap-1 bytes + NUL. The token is spent by the child. */
    fdk_result (*window_activation_token)(fdk_platform_window *pwindow,
                                          char *out, size_t cap);

    /* --- Window operations --- */

    /* Creates a window. `parent` is NULL for top-levels; for popup
     * windows (options->popup, Phase 9) it is the parent platform
     * window (X11: position reference for the override-redirect
     * child; Wayland: the xdg_surface the xdg_popup attaches to). */
    fdk_result (*window_create)(fdk_platform_connection *conn,
                                 const fdk_window_options *options,
                                 fdk_platform_window *parent,
                                 fdk_platform_window **out_pwindow);
    void (*window_destroy)(fdk_platform_window *pwindow);
    void (*window_show)(fdk_platform_window *pwindow);
    void (*window_hide)(fdk_platform_window *pwindow);
    void (*window_set_title)(fdk_platform_window *pwindow, const char *title);

    /* OPTIONAL (1.1.0, M-4 window identity): the window's icon as
     * width x height ARGB8888 pixels (fdk_u32 packing, A<<24|R<<16|
     * G<<8|B — identical bytes to _NET_WM_ICON and the
     * xdg-toplevel-icon ARGB8888 buffer). pixels == NULL or
     * non-positive dims CLEAR the icon; repeated calls replace
     * (last wins). The pixels need only live for the call — every
     * backend copies into its own representation (the X11 property
     * buffer / the Wayland SHM pool) before returning. */
    void (*window_set_icon)(fdk_platform_window *pwindow,
                            const fdk_u32 *pixels,
                            int width, int height);
    /* OPTIONAL (M-4): the ICCCM WM_WINDOW_ROLE string (X11; Wayland
     * leaves it NULL — no protocol exists, the public API documents
     * the no-op). NULL/empty reverts the role to the backend's
     * create-time default. */
    void (*window_set_role)(fdk_platform_window *pwindow,
                            const char *role);
    void (*window_resize)(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height);
    void (*window_set_size_limits)(fdk_platform_window *pwindow,
                                    fdk_size min_size, fdk_size max_size);

    /* OPTIONAL (Phase 8 decorations): turn the window manager's /
     * compositor's own decorations for this window on or off. X11
     * implements this via _MOTIF_WM_HINTS (respected by all major
     * WMs; harmless where ignored). Wayland leaves it NULL until
     * xdg-decoration lands — callers treat NULL/UNAVAILABLE as "this
     * backend cannot drop its chrome" and must NOT draw their own
     * title bar over the compositor's. */
    fdk_result (*window_set_wm_decorations)(fdk_platform_window *pwindow,
                                            bool on);

    /* OPTIONAL: the window's top-left position in root (screen)
     * coordinates. NULL = the backend cannot know it (Wayland:
     * clients are not told their position). */
    fdk_result (*window_get_position)(fdk_platform_window *pwindow,
                                      fdk_i32 *out_x, fdk_i32 *out_y);

    /* OPTIONAL: move the window to root coordinates. NULL = the
     * backend cannot (Wayland: only the compositor moves windows).
     * Under a reparenting WM this moves the client window; WMs that
     * reparent may or may not carry the frame along — v1 documents
     * this as exact under bare/non-reparenting X servers. */
    void (*window_move_to)(fdk_platform_window *pwindow,
                           fdk_i32 x, fdk_i32 y);

    /* OPTIONAL: atomically move AND resize (the FDK-driven resize-edge
     * path; a separate move+resize pair would tear on the wire under a
     * real WM). NULL = the caller falls back to window_move_to +
     * window_resize. */
    void (*window_move_resize_to)(fdk_platform_window *pwindow,
                                  fdk_i32 x, fdk_i32 y,
                                  fdk_i32 width, fdk_i32 height);

    /* ---- OPTIONAL window-state operations (Phase 8) ----
     *
     * All of these are REQUESTS; the platform may ignore them (see
     * each op). State changes the platform acknowledges are reported
     * back as FDK_EVENT_WINDOW_STATE through the normal dispatch path,
     * never assumed from the request alone — except where a backend
     * performs the action itself (the bare-X fallbacks below), where
     * it dispatches the event directly. */

    /* Request maximized/unmaximized. X11: _NET_WM_STATE message under
     * an EWMH WM (state arrives via PropertyNotify); without a WM the
     * backend maximizes itself (fullscreen move+resize, geometry
     * saved for restore) and dispatches the state event. Wayland:
     * xdg_toplevel.set_maximized/unset; state arrives via configure.
     * NULL = backend cannot maximize at all. */
    /* 1.4.10: the fullscreen request pair (one op, bool — the
     * maximize pattern). State truth flows back through
     * FDK_EVENT_WINDOW_STATE exactly like maximized. */
    fdk_result (*window_set_fullscreen)(fdk_platform_window *pwindow,
                                        bool fullscreen);
    fdk_result (*window_set_maximized)(fdk_platform_window *pwindow,
                                       bool maximized);

    /* Request minimized (iconic)/restored. X11: XIconifyWindow (any
     * WM) / XUnmapWindow + flag (bare X); restore is XMapWindow.
     * State tracked via the WM_STATE property where a WM maintains
     * it. Wayland: xdg_toplevel.set_minimized — the protocol gives NO
     * acknowledgement (no unminimize request either; compositors
     * unminimize via activation), so the backend reports the REQUEST
     * optimistically and clears it on the next activated configure;
     * window_set_minimized(false) returns FDK_ERR_UNSUPPORTED there.
     * NULL = backend cannot minimize. */
    fdk_result (*window_set_minimized)(fdk_platform_window *pwindow,
                                       bool minimized);

    /* OPTIONAL interactive-move/resize starters: hand the drag to the
     * WM/compositor (EWMH _NET_WM_MOVERESIZE on X11, xdg_toplevel
     * .move/.resize on Wayland), which then drives it with its own
     * pointer grab — FDK sees only the resulting configures. The
     * press point is window-local; backends translate as their
     * protocol needs. `edge` is an fdk_window_resize_edge compass
     * value (see window_internal.h; plain int here so this header
     * needn't depend on it); only begin_resize uses it. Return
     * FDK_OK when the platform took the drag, FDK_ERR_UNSUPPORTED
     * (or NULL op) when the caller should run its own fallback drag
     * via window_move_to/move_resize_to. */
    fdk_result (*window_begin_move)(fdk_platform_window *pwindow,
                                    fdk_i32 local_x, fdk_i32 local_y);
    fdk_result (*window_begin_resize)(fdk_platform_window *pwindow,
                                      int edge, fdk_i32 local_x,
                                      fdk_i32 local_y);

    /* --- OPTIONAL pointer introspection + cursor shaping (1.1.4) ---
     *
     * Both exist for the same class of bug: FDK's chrome has to
     * REACT to pointer position at moments the platform generates no
     * event for, and to ADVERTISE affordances a WM frame's borders
     * give for free.
     *
     * window_query_pointer writes the pointer's current position in
     * WINDOW-LOCAL coordinates to out_x/out_y and returns nonzero
     * when the pointer is inside this window's bounds right now. The
     * window layer calls it after geometry changes (configure,
     * window-state flips): when a window moves/resizes under a
     * STATIONARY pointer the platform delivers no motion event, so
     * hover state computed against the OLD geometry sticks (the
     * classic highlight-that-never-clears on a maximize button).
     * NULL = unsupported; revalidation is skipped, nothing else
     * changes.
     *
     * window_set_cursor selects the cursor shape shown over this
     * window. `edge` is 0 (default arrow), an
     * fdk_window_resize_edge compass value (plain int here so this
     * header needn't depend on window_internal.h): FDK's resize-edge
     * zones use it to show the directional resize cursor BEFORE any
     * button is held — the affordance users expect from a resizable
     * window — or FDK_PLATFORM_CURSOR_POINTER (9): the pointing hand
     * an application requests over clickable regions (1.1.0-rc1,
     * fdk_window_set_cursor / fdk_ext_surface_set_cursor — the
     * public fdk_cursor_shape enum maps onto this space; the backend
     * owns the encoding), or a NAMED shape (1.1.0-rc2, S-5):
     * FDK_PLATFORM_CURSOR_NAMED_BASE + (public - 2) for the full
     * fdk_cursor_shape set (TEXT .. VERTICAL_TEXT). Called only on
     * shape transitions (the
     * window layer caches the applied shape). NULL = the backend
     * cannot shape cursors; the platform default applies (documented
     * per-backend — honest unsupported, no lying fallback). */
    int (*window_query_pointer)(fdk_platform_window *pwindow,
                                fdk_i32 *out_x, fdk_i32 *out_y);
    void (*window_set_cursor)(fdk_platform_window *pwindow, int edge);

    /* OPTIONAL (1.1.0-rc2, S-4 / A-06): sets the window's INPUT
     * region — count window-LOCAL LOGICAL rectangles (0 <= count
     * <= FDK_PLATFORM_INPUT_REGION_MAX; NULL rects with count 0 =
     * clear, full input). Input on the window outside the rects
     * falls through to whatever is beneath. X11: XShape ShapeInput,
     * logical rects scaled to physical by the connection's scale.
     * Wayland: wl_surface.set_input_region (surface-local == the
     * logical space, so the rects pass through unscaled), committed
     * when the surface is already configured (never before — the
     * unconfigured-commit protocol error). NULL = the backend lacks
     * the mechanism (the window stays fully interactive; the window
     * layer reports FDK_ERR_UNSUPPORTED). */
    void (*window_set_input_region)(fdk_platform_window *pwindow,
                                    const fdk_rect *rects, int count);

    /* --- Software rendering ---
     *
     * Both render ops are OPTIONAL: a backend that cannot provide a
     * software framebuffer (e.g. a future GPU-only backend) leaves
     * them NULL and the surface layer (src/render/surface.c) reports
     * FDK_ERR_UNSUPPORTED to the application.
     *
     * window_get_framebuffer returns the window's current CPU-drawing
     * buffer in *out_fb, (re)creating it if the window was resized
     * since the last call. A present-less no-draw call is legal.
     *
     * window_present makes the damaged parts of the current buffer
     * visible on screen. `damage` is never NULL and follows the
     * fdk_platform_damage contract above: full=1 -> whole surface;
     * count==0 -> nothing changed (backends treat it as a successful
     * no-op); otherwise exactly the listed rects changed (backends
     * may coarsen granularity but must not present garbage outside
     * the damage as new content — a fresh buffer must therefore
     * carry valid content outside damage, e.g. by copying the
     * previous frame into it, because compositors are allowed to
     * ignore damage hints entirely). With no buffer ever acquired it
     * is a documented no-op returning FDK_OK. See x11_surface.c and
     * wayland_window.c for per-backend notes. */
    fdk_result (*window_get_framebuffer)(fdk_platform_window *pwindow,
                                          fdk_platform_framebuffer *out_fb);
    fdk_result (*window_present)(fdk_platform_window *pwindow,
                                 const fdk_platform_damage *damage);

    /* OPTIONAL frame-pacing query behind
     * fdk_surface_frame_ready(): returns nonzero when the next frame
     * may be drawn/presented now (compositor acknowledged the last
     * frame, or no pacing applies). NULL means "always ready" (X11,
     * and any backend without display-driven frame feedback). Must
     * never block and never busy-wait — it is a state query. */
    int (*window_frame_ready)(fdk_platform_window *pwindow);

    /* OPTIONAL first-commit query, test/diagnostic seam behind the
     * internal fdk__window_ever_presented(): returns nonzero once a
     * frame has actually reached the screen (a real commit on
     * Wayland, an XPutImage on X11), 0 before that. NULL means
     * "unknown" (-1 to the caller). Exists because Wayland's
     * configure handshake can leave an app's first present DEFERRED
     * (see the deferred-first-frame handling in wayland_window.c) —
     * the regression test for that path needs an honest, observable
     * answer to "is this window actually on screen yet". */
    int (*window_ever_presented)(fdk_platform_window *pwindow);

    /* OPTIONAL render-path introspection (1.4.18) behind
     * fdk_window_get_render_path(): returns the presentation path
     * this window actually uses RIGHT NOW — 0 = the CPU/software
     * path (MIT-SHM or XPutImage copy, or the Wayland wl_shm commit),
     * 1 = the GPU present lane (X11/GLX: damaged rects uploaded to a
     * GL texture, one fullscreen composite, glXSwapBuffers). NULL =
     * the backend has no GPU lane at all (Wayland today) and the
     * frontend answers 0/CPU. The value is read LIVE off the
     * backend's own state, so it reflects mid-session downgrades.
     * FDK's rasterization is CPU-side on BOTH paths — see
     * docs/rendering.md's GPU-present-lane section for the hybrid
     * model and the policy/env knobs (FDK_NO_GL, FDK_GL_FORCE). */
    int (*window_render_path)(fdk_platform_window *pwindow);

    /* OPTIONAL HiDPI scale query behind fdk_window_get_scale()
     * (Phase 3 completion): writes the window's current scale factor
     * (1.0 = one buffer pixel per logical unit) to *out_scale.
     * NULL means "always 1.0" — the honest answer for X11, whose
     * core protocol has no scale concept (X11 windows are their
     * pixels; HiDPI there is a font-size/DPI convention outside
     * FDK's scope, see docs/rendering.md). The Wayland backend
     * reports its live wl_surface buffer scale / fractional
     * preferred scale. */
    fdk_result (*window_get_scale)(fdk_platform_window *pwindow,
                                   fdk_f32 *out_scale);

    /* ---- OPTIONAL clipboard operations (Phase 9) ----
     *
     * Context-wide (not per-window) text clipboard behind
     * include/fdk/fdk_clipboard.h. NULL = the backend has no
     * clipboard support; the frontend reports FDK_ERR_UNSUPPORTED /
     * NULL with a warning rather than failing silently.
     *
     * clipboard_set_text makes the backend the clipboard owner and
     * keeps a copy to serve later requests (X11 SelectionRequest /
     * Wayland wl_data_source.send). `text` is NUL-terminated and
     * never NULL; the backend copies it. Replaces any previous
     * content.
     *
     * clipboard_get_text returns a freshly allocated NUL-terminated
     * UTF-8 string owned by the caller (fdk_free), or NULL when the
     * clipboard is empty/unreadable. Implementations must be
     * synchronous from the caller's perspective and internally
     * bounded (X11's convert-and-wait may not hang on a stuck
     * owner) and must not re-enter dispatch re-entrantly. */
    fdk_result (*clipboard_set_text)(fdk_platform_connection *conn,
                                     const char *text);
    char *(*clipboard_get_text)(fdk_platform_connection *conn);

    /* ---- OPTIONAL PRIMARY selection operations (1.3.4) ----
     *
     * The classic Unix "current selection" buffer distinct from the
     * copy/paste CLIPBOARD: text becomes PRIMARY by being SELECTED
     * (the toolkit owns it automatically when an Entry selection
     * changes), and middle-click pastes it. Semantics per backend:
     *
     *   X11: the ICCCM XA_PRIMARY selection — real ownership,
     *   serving, and reading through the same machinery as
     *   CLIPBOARD. Sets are BEST-EFFORT (no verification
     *   round-trip: the classic model re-owns on every selection
     *   change).
     *
     *   Wayland: the wp_primary_selection_unstable_v1 protocol
     *   where the compositor offers it (sway and wlroots compositors
     *   do); compositors without it leave the op NULL ->
     *   FDK_ERR_UNSUPPORTED / NULL.
     *
     * clipboard_set_primary_text follows clipboard_set_text's copy
     * and replacement semantics. clipboard_get_primary_text follows
     * clipboard_get_text's ownership and NULL-when-empty semantics. */
    fdk_result (*clipboard_set_primary_text)(
        fdk_platform_connection *conn, const char *text);
    char *(*clipboard_get_primary_text)(fdk_platform_connection *conn);

    /* ---- OPTIONAL clipboard image operations (1.4.9) ----
     *
     * The clipboard's image/png surface behind the core's
     * encode/decode (fdk_clipboard_set_image / get_image). The
     * seam carries ENCODED PNG bytes — backends move bytes for the
     * image/png target (X11) or mime type (Wayland), the core owns
     * the format. One clipboard content at a time: setting an
     * image replaces any text the backend serves and vice versa.
     *
     * clipboard_set_image copies `png` (never NULL, len > 0) and
     * makes the backend the owner serving image/png. Backends may
     * refuse payloads beyond their transport's comfortable atomic
     * size (X11: 4 MiB — INCR stays a deliberate LATER) with
     * FDK_ERR_UNSUPPORTED.
     *
     * clipboard_get_image returns freshly allocated PNG bytes
     * (fdk_free) with *out_len set, or NULL when the clipboard has
     * no image (or it is unreadable/oversized). */
    fdk_result (*clipboard_set_image)(
        fdk_platform_connection *conn, const unsigned char *png,
        size_t len);
    unsigned char *(*clipboard_get_image)(
        fdk_platform_connection *conn, size_t *out_len);

    /* ---- OPTIONAL popup/dialog grab operations (Phase 9
     * completion — the menu machinery) ----
     *
     * window_popup_regrab re-asserts an existing popup's input grab
     * after a popup stacked ABOVE it closed: server/compositor grabs
     * do not stack (a nested popup's grab replaces its parent's, and
     * closing the child does not restore the parent's). Called only
     * on popup windows that grabbed at show. NULL = the backend
     * cannot re-grab (keyboard nav after submenu dismissal then
     * degrades to the pre-grab focus, documented).
     *
     * window_set_modal takes (modal=true) or releases (false) a
     * POINTER+KEYBOARD grab on a TOPLEVEL window for modal dialogs:
     * while grabbed, no other window on the connection receives
     * input. Unlike popup grabs, presses outside the window are NOT
     * dismissals — they arrive as ordinary out-of-bounds events and
     * are ignored (the modal contract is "input waits for the
     * dialog", not "click away closes it"). NULL or
     * FDK_ERR_UNSUPPORTED = the backend cannot do modal grabs
     * (Wayland: no toplevel-grab protocol exists — dialogs there
     * are non-modal, documented honestly). */
    void (*window_popup_regrab)(fdk_platform_window *pwindow);
    fdk_result (*window_set_modal)(fdk_platform_window *pwindow,
                                   bool modal);

    /* Input-transparent popups (1.3.2, tooltips) are a CREATE-TIME
     * contract carried by fdk_window_options.input_transparent, not
     * a post-create op: Wayland's xdg_popup.grab must be decided
     * when the role object is built, and X11's grab lives in show —
     * one flag answered by both. The platform layer must then:
     *   (a) apply an EMPTY input region (X11: XShape input region,
     *       if probed; Wayland: wl_region before the first commit) so
     *       pointer events fall through to whatever is underneath;
     *   (b) NEVER take the popup grab — grabbing would contradict
     *       click-through and (X11) steal the app's keyboard while
     *       the hint is up.
     * Backends without the input-region primitive degrade honestly:
     * the popup swallows clicks that land on it (logged). */

    /* ---- OPTIONAL drag-and-drop operations (1.2.0) ----
     *
     * Behind include/fdk/fdk_dnd.h. Formats are the public
     * fdk_drag_format OR-mask (plain int here so this header needn't
     * depend on fdk_dnd.h).
     *
     * window_set_drop_formats registers what this window accepts.
     * The backend stores the mask on its platform window and uses it
     * during protocol negotiation: X11 replies accept/reject in its
     * XDndStatus (the dragging client's cursor feedback follows it);
     * Wayland drives wl_data_offer set_actions with it. Drag events
     * are dispatched through the normal dispatch fn (the window
     * layer's routing delivers them to the app callback; the widget
     * tree ignores them) — FDK_EVENT_DRAG_ENTER/MOTION/LEAVE with
     * position + offered/accepted masks, and FDK_EVENT_DRAG_DROP with
     * the decoded payload (POSIX paths for file URIs, UTF-8 text),
     * which the backend allocates and frees after the dispatch
     * returns. NULL = backend cannot receive drops (headless).
     *
     * drag_begin starts a source drag from this window offering
     * `formats` with `text` / the `uri_count` paths in `uris` (all
     * copied by the backend before returning). `on_done` fires
     * exactly once, from inside a later dispatch, with an
     * fdk_drag_status value as int. The drag runs INSIDE the normal
     * event dispatch — no nested loop, no blocking; pointer events
     * while the drag is active belong to it. NULL = backend cannot
     * start drags (the frontend reports FDK_ERR_UNSUPPORTED).
     * 1.1.0, S-9: the icon triple (icon/icon_w/icon_h — ARGB8888,
     * fdk_u32 packing, already clamped to 256 by the frontend) rides
     * the drag; NULL icon = none. The buffer is TEMPORARY — the
     * backend must finish its own copy (XImage / SHM upload) before
     * returning; the frontend frees it at return. */
    void (*window_set_drop_formats)(fdk_platform_window *pwindow,
                                    int formats);
    fdk_result (*drag_begin)(fdk_platform_window *origin, int formats,
                             const char *text,
                             const char *const *uris, size_t uri_count,
                             const fdk_u32 *icon, int icon_w, int icon_h,
                             void (*on_done)(int status, void *user),
                             void *user);

    /* ---- OPTIONAL extension-surface adoption (1.1.0) ----
     *
     * The machinery behind include/fdk/fdk_extend.h (see that
     * header for the ownership model; this is the backend half).
     *
     * ext_surface_adopt wraps a NATIVE surface an add-on created
     * through the documented native-display accessor (an X11
     * Window the add-on built unmapped with its own properties, or
     * a wl_surface the add-on gave a protocol role FDK does not
     * know) in a new fdk_platform_window and registers it for event
     * routing — after which the platform treats it like any other
     * window: dispatch translates its events, the render ops give
     * it a framebuffer and a present path, popups can parent to it.
     *
     * Per backend:
     *   X11: allocates the wrapper, installs FDK's standard event
     *   mask (XSelectInput — the add-on selected nothing; FDK owns
     *   event selection for windows it routes), creates the XIC
     *   when the connection has an XIM, registers the XID.
     *
     *   Wayland: allocates the wrapper around the wl_surface (the
     *   surface listener attaches — output enter/leave keeps the
     *   HiDPI scale machinery live; the fractional-scale object is
     *   bound when the manager exists), registers it in the
     *   surface->pwindow table the seat uses for pointer/keyboard
     *   focus routing. The surface must NOT have been committed yet
     *   (the add-on's role handshake owns the first commit).
     *
     * The native handle arrives in the public tagged struct
     * (fdk_ext_native_handle from fdk_extend.h) so this header stays
     * free of backend types: xid is the Window, pointer is the
     * wl_surface. Backends cast to their own types here — the only
     * sanctioned crossing point.
     *
     * Returns FDK_ERR_INVALID_ARGUMENT for a NULL/out-of-backend
     * native handle, FDK_ERR_OUT_OF_MEMORY, or
     * FDK_ERR_WINDOW_CREATE on registration failure. The backend
     * does NOT take ownership on failure. */
    fdk_result (*ext_surface_adopt)(fdk_platform_connection *conn,
                                    const fdk_ext_native_handle *native,
                                    fdk_platform_window **out_pwindow);

    /* OPTIONAL (1.1.0): writes a window's native surface handle in
     * the same tagged shape ext_surface_adopt consumed (X11: the
     * Window XID; Wayland: the wl_surface*) — the accessor behind
     * fdk_ext_surface_native(). NULL op = no native handle. */
    fdk_result (*ext_surface_native)(fdk_platform_window *pwindow,
                                     fdk_ext_native_handle *out);

    /* OPTIONAL (1.1.0): an extension surface's ROLE assigned it a
     * new size (fdk_ext_surface_notify_configure — the Wayland
     * shape; X11's server-driven ConfigureNotify never goes through
     * it). The backend updates its buffer-side size bookkeeping
     * (X11: last_size, which re-sizes the framebuffer pair at the
     * next acquire; Wayland: last_size + pending_size, which sizes
     * the render slots the next present commits). NULL op = the
     * backend has no buffer bookkeeping to update. */
    void (*ext_surface_resized)(fdk_platform_window *pwindow,
                                fdk_i32 width, fdk_i32 height);

    /* OPTIONAL (1.1.0): the backend's native display connection
     * handle, borrowed for the connection's lifetime — the
     * documented accessor behind fdk_ext_get_native_display()
     * (include/fdk/fdk_extend.h). X11 returns the Display*; Wayland
     * the struct wl_display*; the tag comes from
     * fdk_ext_active_backend(). NULL op or NULL result = no native
     * handle to hand out (none of the current backends). */
    void *(*native_display)(fdk_platform_connection *conn);
} fdk_platform_ops;

/* Backend entry points. Each returns NULL if that backend was not
 * compiled in (Wayland in a build with FDK_DISABLE_WAYLAND=1 — see
 * the Makefile's optional-Wayland logic and docs/build.md). */
const fdk_platform_ops *fdk_platform_x11_ops(void);
const fdk_platform_ops *fdk_platform_wayland_ops(void);

/* Auto-detection per fdk_core.h's documented FDK_PLATFORM_AUTO
 * contract: Wayland if $WAYLAND_DISPLAY is set (existence check only
 * here; the actual connection attempt in wayland_ops->connect() is
 * what proves reachability), otherwise X11. */
int fdk_platform_wayland_display_present(void);

/* ---- text/uri-list codec (shared by both backends + tests) ----
 *
 * The XDND and Wayland worlds exchange files as RFC-2483
 * text/uri-list: one URI per line, CRLF-tolerant, comment lines
 * ('#') skipped. FDK's public contract hands applications POSIX
 * paths, so the decode side converts file:// URIs (percent-decoded)
 * to paths and leaves other URIs verbatim; the encode side does the
 * reverse (escaping, absolute-path normalization). The
 * implementation lives in src/window/dnd_uri.c (backend-agnostic
 * pure C, unit-tested in tests/test_dnd_logic.c) but is declared
 * HERE at the seam so both backends link one implementation and
 * cannot drift. */

/* Parses a text/uri-list payload into a freshly allocated argv-style
 * array (entries fdk_alloc'd, the array too). Returns FDK_OK; on
 * empty/garbage input *out_list is NULL and *out_count 0. Always
 * sets both outs. */
fdk_result fdk__dnd_parse_uri_list(const char *payload, size_t len,
                                   char ***out_list, size_t *out_count);

/* Frees a parse result (the array and its entries). */
void fdk__dnd_free_uri_list(char **list, size_t count);

/* Builds a text/uri-list payload from POSIX paths / URIs: entries
 * with a scheme pass through, anything else is treated as a path
 * (realpath-normalized when resolvable) and file://-encoded with
 * escaping. Returns an fdk_alloc'd NUL-terminated CRLF-joined
 * payload, or NULL. */
char *fdk__dnd_build_uri_list(const char *const *uris, size_t count);

/* ---- Extension surfaces (1.1.0) -----------------------------------------
 *
 * The window-layer half of include/fdk/fdk_extend.h (implemented in
 * src/window/extend.c). The WAYLAND backend calls this from its
 * popup-creation path when the popup's parent is an extension
 * surface: an xdg_popup was created with NO xdg parent (the parent
 * has no xdg_surface — its role belongs to the add-on), and the
 * add-on's kind must adopt it (e.g.
 * zwlr_layer_surface_v1.get_popup).
 *
 * `ctx` is the owning context — the SAME value the backend stored as
 * dispatch_user_data at connect() time (context.c passes the context
 * itself there; it is the one established bridge from a platform
 * connection back to the core). The hook resolves the parent window
 * through the registry and routes to the kind's popup_attached
 * callback. Silent no-op when the parent is not an extension surface
 * or its kind has no callback (the popup then stands on its grab
 * alone). */
void fdk__ext_popup_attached(fdk_context *ctx,
                             fdk_platform_window *parent_pwindow,
                             void *native_popup);

/* Popup close-request REASON channel (1.1.0-rc1, X3): a backend that
 * is about to dispatch FDK_EVENT_WINDOW_CLOSE_REQUEST for a popup
 * calls this with WHY (window_internal.h's FDK_POPUP_CLOSE_* values)
 * immediately before the dispatch; the window layer's dispatcher
 * consumes it when the event lands on a popup and FDK_POPUP_TRACE
 * names the reason in its close-request line. Single-slot, set-then-
 * consume; a reason nobody consumes is drained harmlessly by the next
 * non-popup close-request. Defined in window.c. */
void fdk__window_note_popup_close(int reason);

/* The reason values (1.1.0-rc1, X3). Here rather than
 * window_internal.h so the BACKEND generators (x11_events.c's outside
 * click, wayland_window.c's xdg_popup_done) can cite them — they
 * include this header through their platform headers; window.c gets
 * them transitively through window_internal.h. */
enum {
    FDK_POPUP_CLOSE_NONE = 0,
    FDK_POPUP_CLOSE_OUTSIDE_CLICK = 1, /* X11: press outside the grab */
    FDK_POPUP_CLOSE_ESCAPE = 2,        /* window.c: the universal key */
    FDK_POPUP_CLOSE_COMPOSITOR = 3,    /* Wayland: xdg_popup_done */
    FDK_POPUP_CLOSE_PARENT = 4,        /* the parent's destroy sweep */
};

/* ---- Per-position drag acceptance (1.1.0, S-9 / B-13) ---------------------
 *
 * The backends' acceptance query for a hovering drag, implemented in
 * the CORE window layer (window.c): hit-test the widget tree at the
 * window-local position, walk UP from the deepest hit widget to the
 * first registered drop target (fdk_widget_set_drop_target), and
 * intersect `offered` with THAT widget's mask — or with the WINDOW's
 * own mask when no target owns the position (the pre-S-9 rule, so
 * windows without widget targets behave byte-identically).
 *
 * `dispatch_user` is the fdk_context* the backend holds as
 * conn->dispatch_user_data. Returns the accepted mask (0 = refuse);
 * a missing window/context answers the pwindow's own mask (the
 * backend's copy, kept in sync by window_set_drop_formats) — never a
 * protocol stall. Call this at every acceptance decision site: XDND
 * Status replies, wl_data_offer.accept, and the drop-time data
 * reads. */
int fdk__core_drag_accepts_at(void *dispatch_user,
                              fdk_platform_window *pwindow,
                              int offered, int x, int y);

#endif /* FDK_PLATFORM_INTERNAL_H */
