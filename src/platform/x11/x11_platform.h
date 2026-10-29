/*
 * x11_platform.h — X11 backend internals
 *
 * Only included by files inside src/platform/x11/. No file outside
 * this directory may include Xlib headers directly — that's the
 * "no backend leakage" rule from docs/architecture.md, enforced here
 * by keeping the Xlib-typed structs entirely inside this directory.
 */

#ifndef FDK_X11_PLATFORM_H
#define FDK_X11_PLATFORM_H

#include "platform/platform_internal.h"
#include "platform/x11/x11_gl.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h> /* RENDER cursor lane (S-5) — types
                                     * only; the functions are dlopen'd */
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <stddef.h>
#include <stdint.h>

/* 1.4.5: HiDPI detection (x11_connection.c) + the scale query op
 * (x11_window.c, registered in the ops table). */
int fdk__x11_detect_scale(Display *display);

/* 1.1.0 M-6/B-05: the RandR output model (x11_randr.c) — per-CRTC
 * geometry probed at connect, refreshed on RRNotify. Internal only
 * (the Wayland core tracks outputs without an app-facing output
 * API too; fdk-shell drives its OWN X connection for output
 * events). Consumers: window placement (maximize/fullscreen to the
 * containing monitor). */
typedef struct fdk_x11_output {
    int x, y;             /* the CRTC's geometry, root coordinates  */
    int width, height;
    int mm_width, mm_height; /* the output's physical size           */
    char name[32];        /* the output's connector name            */
    int connected;        /* RRIsConnected at probe time            */
} fdk_x11_output;

typedef struct fdk_x11_randr {
    int present;          /* the extension answered the query       */
    int event_base;       /* -1 when absent                         */
    int count;
    fdk_x11_output outputs[16];
} fdk_x11_randr;

void fdk__x11_randr_init(fdk_platform_connection *conn);
void fdk__x11_randr_refresh(fdk_platform_connection *conn);
/* Returns 1 when the event was an RRNotify/RRScreenChange (and was
 * consumed by refreshing the table). */
int fdk__x11_randr_handle(fdk_platform_connection *conn,
                          const XEvent *ev);
int fdk__x11_randr_output_count(const fdk_platform_connection *conn);
const fdk_x11_output *fdk__x11_randr_output_at(
    const fdk_platform_connection *conn, int index);
/* The monitor containing (or nearest to) a point; the whole screen
 * when RandR is absent or reports nothing. */
void fdk__x11_randr_monitor_containing(const fdk_platform_connection *conn,
                                       int x, int y, int *out_x, int *out_y,
                                       int *out_w, int *out_h);
/* XSETTINGS follow (x11_xsettings.c): look up the settings daemon's
 * manager window and select PropertyChangeMask on it (our
 * connection's per-client mask — the owner window is untouched);
 * the throttled re-arm catches daemons appearing/restarting later. */
void fdk_x11_xsettings_arm(fdk_platform_connection *conn);
void fdk_x11_xsettings_rearm_tick(fdk_platform_connection *conn);
fdk_result fdk_x11_window_get_scale(fdk_platform_window *pwindow,
                                    fdk_f32 *out_scale);

/* 1.4.18: render-path introspection op (x11_surface.c) behind
 * fdk_window_get_render_path() — 1 while this window presents
 * through the GPU lane, 0 on the CPU path. */
int fdk_x11_window_render_path(fdk_platform_window *pwindow);

struct fdk_platform_connection {
    /* Application id (fdk_init_options.app_id), duplicated at connect
     * time; NULL when unset. Applied to every window as WM_CLASS (the
     * X11 counterpart of Wayland's xdg_toplevel.set_app_id — window
     * managers match it for rules and taskbar grouping). */
    char *app_id;
    Display *display;
    int screen;
    Window root;

    /* HiDPI scale (1.4.5): X11 has no scale protocol — the window
     * is created at logical x scale physical pixels and the core's
     * paint path composites the logical tree up (the same
     * intermediate + integer-scale blit the Wayland backend rides
     * with viewporter). INTEGER ONLY: the core's blit is the
     * nearest-neighbor block path (no resampling artifacts). One
     * detection at connect — see x11_connection.c for the two
     * conventions (Xft.dpi resource, screen physical metric). */
    int scale;

    /* The RandR output table (M-6/B-05): probed at connect,
     * refreshed on RRNotify (x11_randr.c). Absent extension = count
     * 0 and every placement query falls back to the whole screen —
     * the pre-RandR behavior. */
    fdk_x11_randr randr;

    Atom wm_delete_window;
    Atom wm_protocols;
    Atom net_wm_name;
    Atom utf8_string;
    Atom motif_wm_hints; /* _MOTIF_WM_HINTS, Phase 8 decorations */

    /* Window identity atoms (1.1.0, M-4): _NET_WM_ICON (the EWMH
     * ARGB icon list a taskbar/dock/switcher reads), _NET_WM_PID +
     * WM_CLIENT_MACHINE (the process identity session managers and
     * EWMH pagers correlate — set once per window at create), and
     * WM_WINDOW_ROLE (ICCCM 5.5.1, the per-window role string).
     * Interned unconditionally at connect: a property write to an
     * atom no WM ever reads is one round-trip wasted, not an error
     * — and the bare-X test rigs READ them back. */
    Atom net_wm_icon;
    Atom net_wm_pid;
    Atom wm_client_machine;
    Atom wm_window_role;

    /* Activation + startup notification atoms (1.1.0, S-6):
     * _NET_ACTIVE_WINDOW (the EWMH focus request — data.l[1] is
     * the last USER event timestamp, the number a WM weighs for
     * focus-stealing prevention), _NET_STARTUP_ID (the UTF8
     * property a launched app tags its windows with), and the
     * _NET_STARTUP_INFO_BEGIN/_NET_STARTUP_INFO root-message pair
     * (the chunked startup-notification state protocol — "new:"
     * on mint, "remove:" on the launched app's first show). */
    Atom net_active_window;
    Atom net_startup_id;
    Atom net_startup_info_begin;
    Atom net_startup_info;

    /* S-6 state: the captured DESKTOP_STARTUP_ID (a launcher's
     * voucher for us — see x11_connection.c for the read-once,
     * unset-immediately discipline), the per-process sequence for
     * OUR minted ids, and the last user event timestamp (updated
     * by the dispatch pump from key/button/motion events — 0 =
     * CurrentTime, which WMs treat as "unknown, decide yourself"). */
    char *startup_id;
    unsigned long sn_seq;
    unsigned long last_event_time;

    /* Phase 8 window management: EWMH atoms + what the running WM
     * actually supports, probed once at connect from the root's
     * _NET_SUPPORTED list (see x11_connection.c). ewmh_wm != 0 means
     * an EWMH-capable WM is running (the list exists and is
     * non-empty); the bare-X fallback paths (no WM: FDK
     * moves/resizes/unmaps directly) key off ewmh_wm.
     * ewmh_state_ok additionally records that the WM advertises both
     * _NET_WM_STATE_MAXIMIZED_{VERT,HORZ} — INFORMATIONAL (the probe
     * log line): maximize requests go through the _NET_WM_STATE
     * client message whenever ewmh_wm is set regardless, because a
     * WM without the atoms simply ignores the message, while the
     * direct-geometry fallback would lie about the outcome (see
     * fdk_x11_window_set_maximized). */
    Atom net_wm_state;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_maximized_horiz;
    Atom net_wm_state_fullscreen; /* 1.4.10 */
    Atom net_wm_moveresize;
    Atom wm_state;         /* ICCCM WM_STATE (iconic/normal tracking) */
    Atom wm_change_state;  /* ICCCM iconify request message type      */
    Atom net_current_desktop; /* root PropertyNotify watched by the
                               * popup watchdog (1.1.0-rc1) — desktop
                               * switches dismiss open popups + their
                               * grabs                              */
    /* XSETTINGS live follow (1.1.0): the settings daemon's manager
     * window (owner of _XSETTINGS_S<screen>) and the property atom.
     * PropertyNotify on owner/xsettings_settings routes to the theme
     * recheck (color-scheme + icon-theme liveness — the GTK
     * behavior). The owner is re-armed on a 2 s throttle (daemon
     * restarts), and a daemon absent at connect leaves None until
     * the throttle finds one. */
    Window xsettings_owner;
    Atom xsettings_settings;
    uint64_t xsettings_rearm_ms;
    int ewmh_wm;           /* nonzero: an EWMH WM is running          */
    int ewmh_state_ok;     /* nonzero: it advertises _NET_WM_STATE    */
    /* Popup-grab watchdog pacing (1.1.0-rc1): the last watchdog
     * tick (monotonic ms) — the check runs at most every 500 ms
     * while any popup holds a grab, and only then (the common case,
     * no popup open, pays nothing). Set at connect; read/written by
     * x11_dispatch.c's tail. */
    uint64_t popup_watchdog_ms;
    /* Desktop-switch latch (1.1.0-rc1): set by the root
     * PropertyNotify handler in dispatch_pending when
     * _NET_CURRENT_DESKTOP changes; consumed by the watchdog tick,
     * which dismisses every grabbed popup (they belong to the world
     * they opened in). */
    int desktop_changed;
    /* Display death (1.3.0): set by the XIO error handler the moment
     * the connection to the X server is lost. Every teardown path
     * checks it and skips server requests (they cannot succeed and —
     * worse — each one risks re-entering Xlib's fatal IO path, whose
     * default behavior is exit(1), killing the process mid-teardown
     * with no application say). dispatch_pending reports it as a
     * negative result so fdk_run()/pump loops exit cleanly. */
    int display_dead;

    /* Key-repeat discipline (1.3.0): XkbSetDetectableAutoRepeat(True)
     * is requested at connect — when the server accepts, KeyRelease
     * events are REAL releases only (repeats arrive as additional
     * KeyPresses). key_down[] is the 256-bit keycode bitmap the
     * KeyPress translator consults: a press of an already-down key
     * IS the repeat. Servers without the extension keep X's legacy
     * fake release+press pairs, which are INDISTINGUISHABLE from
     * real typing — every event then reports is_repeat 0, the honest
     * answer (and the same answer v1 always gave). */
    int detectable_repeat;
    unsigned char key_down[32];

    /* X Input Method (1.3.1): the connection-wide XIM created at
     * connect (XOpenIM) when the locale supports it; every window
     * gets an XIC (input context) so Xutf8LookupString can resolve
     * full-Unicode text from the user's ACTUAL keyboard layout —
     * v1's XLookupString path produced Latin-1-at-best and ASCII
     * in practice (non-ASCII codepoints came through as 0: é/ä/中
     * were untypable on X11). NULL XIM = the ASCII fallback keeps
     * running (the v1 behavior, for servers/locales without IM
     * support). */
    XIM xim;
    int xim_tried; /* lazy bring-up guard (1.1.0-rc1, X1): one attempt */

    /* XSHAPE (1.3.2): probed at connect; the empty-input-region
     * primitive tooltips need (click-through popups). 0 = extension
     * unavailable (tooltip popups then swallow clicks that land on
     * them — the honest degradation, logged when it happens). */
    int shape_ok;

    /* --- MIT-SHM (Phase 3 completion) ---
     *
     * Probed once at connect. shm_ok: the server supports the shared
     * memory extension AND runtime policy allows using it (see
     * x11_connection.c for the env-var opt-out). shm_event_base: the
     * extension's first event code, needed to recognize
     * ShmCompletion events in the dispatch loop (they arrive like any
     * other X event; their drawable is the window we put to). */
    int shm_ok;
    int shm_event_base;

    /* --- GPU present lane (1.4.18, x11_gl.c) ---
     *
     * gl: the per-connection lane state — libGL handle, resolved
     * entry points, renderer-policy verdict. Loaded LAZILY at the
     * first framebuffer acquisition of any window (never at
     * connect: headless apps and no-window apps must not touch
     * libGL at all). NULL = the lane is not available on this
     * connection (no libGL, no GLX, unresolved symbols, FDK_NO_GL)
     * OR the policy refused it (software renderer without
     * FDK_GL_FORCE). gl_tried makes the attempt one-shot — NULL is
     * sticky, windows never re-attempt. */
    struct fdk_x11_gl *gl;
    int gl_tried;

    /* XI2 touch (1.1.0, M-3): dlopen'd libXi + the X Input
     * extension's opcode and 2.2-touch verdict. Loaded EAGERLY at
     * connect (unlike the lazy GL lane: selecting XI_TouchBegin on
     * windows must happen at CREATE time, so the capability must be
     * known before the first window exists). NULL = no libXi / no
     * extension / XI < 2.2 — windows then simply carry no touch
     * selection, and touch events never arrive (the honest
     * degradation; core input is unaffected). */
    struct fdk_x11_xi2 *xi2;

    /* --- Resize cursors (1.1.4) ---
     *
     * Directional resize cursors, lazily created from the server's
     * built-in cursor font (XCreateFontCursor — core protocol, no
     * libXcursor dependency) and indexed by the
     * fdk_window_resize_edge compass value 1..8. Entry None = not
     * created yet (None is 0, and fdk_alloc does not zero — the
     * connect path initializes the array explicitly). Freed at
     * disconnect (fdk_x11_cursor_shutdown). The window-level shape
     * state (which cursor is currently defined on which window)
     * lives in the X server per window and dies with the window. */
    Cursor resize_cursors[10];   /* 0..8 compass + 9 hand (W2) */

    /* --- The named cursor set (1.1.0-rc2, S-5 / B-12) ---
     *
     * The public fdk_cursor_shape set beyond the hand: TEXT ..
     * VERTICAL_TEXT, indexed by (op - FDK_PLATFORM_CURSOR_NAMED_BASE).
     * Two tiers, per shape, first hit wins and is cached here:
     *   1. an XCursor-theme image through RENDER (dlopen'd
     *      libXrender.so.1 — XRenderCreateCursor from an ARGB
     *      depth-32 pixmap; the SAME shared theme loader the Wayland
     *      backend uses, cursor_theme.c) — real shapes wherever a
     *      cursor theme exists;
     *   2. the nearest core cursor-font glyph (x11_cursor.c's table)
     *      — every X server since 1987, a few shapes honestly
     *      approximated (the font has no "copy", no zoom, no
     *      vertical-text).
     * Entry None = not created yet (or the shape fell back to the
     * default arrow deliberately: None defined on the window IS the
     * default arrow). Freed by fdk__x11_named_cursors_shutdown. */
    Cursor named_cursors[FDK_PLATFORM_CURSOR_NAMED_COUNT];

    /* The dlopen'd RENDER lane (NULL until first named cursor
     * request; stays NULL when libXrender.so.1 or the RENDER
     * extension is absent — tier 2 then carries everything). */
    struct fdk_x11_cursor_render *cursor_render;

    fdk_platform_dispatch_fn dispatch;
    void *dispatch_user_data;

    /* --- Clipboard (Phase 9, ICCCM CLIPBOARD selection; 1.3.4 adds
     * PRIMARY) ---
     *
     * clip_helper is a never-mapped InputOnly window that acts as
     * FDK's selection owner/requestor for BOTH selections. Selection
     * traffic is not tied to any visible window (a context may own
     * the clipboard with zero windows open), so the connection owns
     * a private helper — the same design every ICCCM-faithful
     * toolkit uses. The atoms below are interned once at connect
     * (XA_PRIMARY is a protocol-constant atom — never interned).
     * clip_owned_text / primary_owned_text are the copies FDK serves
     * while it owns each selection; SelectionClear (another client
     * took over) frees the matching one. */
    Window clip_helper;
    Atom atom_clipboard;      /* CLIPBOARD                            */
    Atom atom_targets;        /* TARGETS                              */
    Atom atom_incr;           /* INCR (recognized only — refused)     */
    Atom atom_text;           /* TEXT (legacy compound text alias)    */
    Atom atom_text_plain;     /* text/plain;charset=utf-8             */
    Atom atom_fdk_selection;  /* private property for convert replies */
    char *clip_owned_text;    /* fdk_alloc'd, NULL when not owner     */
    char *primary_owned_text; /* fdk_alloc'd, NULL when not PRIMARY's
                               * owner (1.3.4; XA_PRIMARY)             */
    /* 1.4.9: the clipboard's owned image (encoded PNG). One content
     * at a time: owning an image clears owned text and vice versa.
     * atom_image_png is the target we serve/read. */
    unsigned char *clip_owned_png; /* fdk_alloc'd, NULL when no image  */
    size_t clip_owned_png_len;
    Atom atom_image_png;      /* image/png                            */

    /* 1.4.12: the INCR (incremental) transfer machinery — both
     * directions ride the helper window. TX is an in-flight SERVE of
     * a payload beyond the atomic cap: chunks stream to the
     * REQUESTOR's window+property, each triggered by its delete of
     * the previous chunk (PropertyNotify), ending with a zero-length
     * chunk. The bytes point INTO the owned content (never a second
     * copy); replacing/freeing the content aborts the flight with
     * the graceful zero chunk. One flight at a time — a new request
     * supersedes the old one (documented simplification). The read
     * side is stack-local to the reading call. */
    struct {
        Window requestor;       /* foreign window we write chunks to  */
        Atom property;          /* its property                       */
        Atom type;              /* chunk property type (the target)   */
        const unsigned char *bytes; /* the copy being streamed: the
                                     * owned content (borrowed), or a
                                     * session-owned re-encode (the
                                     * Latin-1 legacy path)            */
        size_t len;
        size_t pos;
        bool owns_bytes;        /* free `bytes` at flight end         */
        bool active;
    } clip_incr_tx;

    /* --- Drag and drop (1.2.0, XDND) — x11_dnd.c ---
     *
     * Receiver state: the XDND handshake runs on client messages
     * to our windows (XdndEnter/Position/Leave/Drop), routed from
     * x11_dispatch.c BEFORE translation. `xdnd` is the incoming
     * drag's context; `xdnd_source` is OUR outgoing drag (pointer
     * grab with owner_events, target tracking by tree walk). The
     * clip_helper doubles as the XDND source identity (selection
     * owner + the window Status/Finished replies arrive on). */
    Atom atom_xdnd_aware;
    Atom atom_xdnd_enter;
    Atom atom_xdnd_position;
    Atom atom_xdnd_status;
    Atom atom_xdnd_leave;
    Atom atom_xdnd_drop;
    Atom atom_xdnd_finished;
    Atom atom_xdnd_action_copy;
    Atom atom_xdnd_type_list;
    Atom atom_xdnd_selection;
    Atom atom_text_uri_list;
    struct {
        Window source;        /* XDndEnter's source window, None idle  */
        Window dest_window;   /* window the last Status named         */
        fdk_platform_window *hover; /* window entered (LEAVE book-keeping) */
        int version;          /* negotiated min(peer, 5)              */
        int offered;          /* FDK_DRAG_FORMAT mask of the type list */
    } xdnd;
    struct {
        int active;
        int version;
        int formats;
        char *text;           /* owned; TEXT payload, NULL if absent   */
        char *uri_payload;    /* owned; text/uri-list wire payload     */
        Window target;        /* current candidate, None when over none */
        int target_accepts;   /* last XDndStatus accept bit           */
        int drop_pending;     /* Drop sent; awaiting XDndFinished      */
        uint64_t drop_sent_ms;
        void (*on_done)(int status, void *user);
        void *on_done_user;
    } xdnd_source;

    /* Simple open-addressed lookup from X11 Window ID -> our
     * fdk_platform_window*, so dispatch_pending() can find the right
     * window for an incoming XEvent (which only carries the raw X ID
     * in xany.window). A hash table would be overkill for the number
     * of top-level windows a typical app has open at once; linear
     * scan of a small dynamic array is simpler and fast enough — see
     * docs/platform-input.md if this ever needs revisiting under
     * profiling (per project principle: don't optimize before
     * measuring). */
    fdk_platform_window **windows;
    size_t window_count;
    size_t window_capacity;
};

struct fdk_platform_window {
    fdk_platform_connection *conn;
    Window xwindow;
    fdk_size last_size; /* most recent ConfigureNotify size */
    /* Extension surface (1.1.0, fdk_extend.h): nonzero when this
     * wrapper ADOPTED an add-on-created window (x11_ext.c) instead
     * of creating it. Everything downstream treats it identically —
     * the marker exists for the few create-time contracts that
     * differ: FDK installed the event mask at adopt (the add-on
     * selected none), and WM-chrome conveniences FDK adds at create
     * (WM_DELETE_WINDOW, XDndAware, WM_CLASS, titles) were left to
     * the add-on's own X calls. */
    int ext;
    /* Per-window X Input Context (1.3.1): created at window create
     * when the connection has an XIM; Xutf8LookupString resolves
     * full-Unicode text through it. NULL = ASCII fallback. Destroyed
     * (and focus-unset) with the window. */
    XIC xic;

    /* XIM IME composition state (1.1.0, M-1; x11_ime.c): the per-IC
     * preedit buffer (UTF-8, lazily allocated at the first draw
     * callback), its capacity, the showing flag, and the per-window
     * XIMCallback array (client_data = this pwindow — XIM callbacks
     * carry their own context). NULL/0/0 when the window never
     * composes. */
    char *ime_preedit;
    size_t ime_preedit_cap;
    int ime_preedit_showing;
    void *ime_cbs; /* fdk_x11_preedit_cbs — opaque here */

    /* Phase 9 popup state: override-redirect windows that grab the
     * pointer while shown; a click outside their bounds dismisses
     * them (translated to a close request in x11_events.c, never
     * delivered as a button event). */
    int popup;
    /* Input-transparent popup (1.3.2, tooltips): set at CREATE from
     * fdk_window_options.input_transparent — empty XShape input
     * region applied at create, popup grab never taken (see
     * fdk_x11_window_show), so the hint is click-through and never
     * steals keyboard focus from the app that owns it. */
    int input_transparent;

    /* Startup-notification state (1.1.0, S-6): a copy of the
     * connection's captured DESKTOP_STARTUP_ID for windows created
     * while the voucher is held — the window's _NET_STARTUP_ID
     * property value AND the id whose "remove:" state ends the
     * launch feedback at this window's FIRST show (the
     * startup-notification protocol's completion step, emitted
     * once). NULL for windows created without a voucher. Freed at
     * destroy. */
    char *startup_id;
    int startup_remove_sent;

    /* Popup trace stamp (1.1.0-rc1, X3): the last IN-BOUNDS button
     * press time on this popup — the destroy-site fallback uses it to
     * tell "an item was selected" (app destroy right after a click)
     * from a bare programmatic destroy. Milliseconds, monotonic. */
    long long popup_last_button_ms;

    /* Last cursor shape applied via XDefineCursor (1.1.0-rc1, W2):
     * mirrors the Wayland pwindow's field so fdk_window_set_cursor
     * behaves identically across backends — X11 re-applies a window
     * cursor automatically on entry (the cursor is window state, not
     * per-enter protocol), so this exists for get/set symmetry and
     * the hand shape's glyph caching. */
    int cursor_shape;
    int grabbed;

    /* --- Popup grab accounting (1.1.0-rc1, the Openbox report) ---
     *
     * The grab is POINTER-ONLY now (an active XGrabKeyboard
     * deactivates the WM's passive grabs — Super+number workspace
     * switching died while a dropdown was open); keys route through
     * XSetInputFocus instead, and these fields carry what the
     * release path owes back:
     *   popup_saved_focus/_revert — who held focus before the popup
     *     took it (restored best-effort on ungrab; the holder may
     *     have died — errors are swallowed there);
     *   popup_grab_ms — when the grab was taken (the trace line and
     *     the watchdog log the HELD duration);
     *   popup_release_reason — set by every dismissal path before
     *     the release ("outside-click", "escape", "destroy",
     *     "unmap", "desktop-change", "watchdog-not-viewable") so
     *     FDK_POPUP_TRACE answers WHY a popup went away. */
    Window popup_saved_focus;
    int popup_saved_revert;
    uint64_t popup_grab_ms;
    char popup_release_reason[32];

    /* 1.2.0 DnD: the formats this window accepts as a drop target
     * (mirror of the fdk_window's public mask, kept live by the
     * window_set_drop_formats op — the XDND accept/reject reply in
     * every Status message reads THIS copy). */
    int drop_formats;

    /* Phase 9 completion: modal-dialog grab state (a TOPLEVEL
     * holding pointer+keyboard grabs so no other window receives
     * input until it closes; outside presses are ignored, not
     * dismissals — see window_set_modal in x11_window.c). */
    int modal_grab;

    /* --- Phase 8 window-state bookkeeping ---
     *
     * maximized/minimized are the backend's view of the truth,
     * updated by our own fallback actions, by PropertyNotify on
     * _NET_WM_STATE / WM_STATE (a real WM talking back), or by the
     * frontend's requests. Every actual FLIP dispatches
     * FDK_EVENT_WINDOW_STATE through conn->dispatch — the frontend
     * caches from that, never from the request. saved_* hold the
     * pre-maximize geometry for the bare-X fallback's restore. */
    int maximized;
    int minimized;
    int fullscreen; /* 1.4.10: tracked via _NET_WM_STATE property */
    int has_saved;
    fdk_i32 saved_x, saved_y, saved_w, saved_h;

    /* Software-rendering state, owned by x11_surface.c: a
     * DOUBLE-BUFFERED pair of pixel buffers plus the GC used to blit
     * them. Slots are created lazily on first acquisition and
     * recreated on resize. `back` indexes the slot the application is
     * currently drawing into; a present swaps the indices, so a
     * blit already handed to the server (which for MIT-SHM reads the
     * segment asynchronously) never aliases the buffer being drawn —
     * the Phase 3 completion fix for the documented resize/blit
     * race. NULL images until the first fdk_surface acquisition. */
    struct {
        XImage *image;      /* NULL = slot not created               */
        char *malloc_data;  /* non-SHM path: Xlib-freed pixel buffer */
        int shm_attached;   /* SHM path active for this slot          */
        XShmSegmentInfo shm;
        int in_flight;      /* SHM put awaiting ShmCompletion        */
    } render_slots[2];
    int render_back;        /* slot index the app draws into          */
    GC render_gc;
    fdk_size render_size;   /* dimensions both slots were created at  */
    int presented_ever;     /* a present reached the server (the
                               window_ever_presented op — diagnostic/
                               regression seam, see platform_internal.h) */
    int background_dropped; /* 1 once the app first acquired the
                               framebuffer: the creation-time white
                               background pixel was replaced by
                               background None (the server never
                               clears again — the resize-flash fix;
                               see x11_window.c) */

    /* GPU present lane state (1.4.18, x11_gl.c): non-NULL while this
     * window presents through GLX (staging buffer + context +
     * texture). NULL = the classic SHM/copy path — including after a
     * mid-session downgrade, which is permanent per window.
     * gl_downgraded is that permanence: set when a lane present fails
     * (or the staging buffer cannot be allocated), never cleared —
     * the re-engagement attempt in get_framebuffer checks it. */
    struct fdk_x11_gl_window *gl_win;
    int gl_downgraded;
};

/* Implemented in x11_events.c, used by x11_dispatch.c. Not part of
 * the fdk_platform_ops interface — internal to this backend only. */
int fdk_x11_translate_event(fdk_platform_window *pwindow, XEvent *xevent,
                             fdk_event_data *out);

/* X modifier-mask (event state / XQueryPointer state) to
 * fdk_key_modifier bits. x11_events.c; the DnD layer uses it for
 * drag.modifiers. */
fdk_u32 fdk__x11_translate_modifiers(unsigned int x_state);

/* Display-death guard (x11_connection.c owns the storage; see the
 * discipline comment there). dispatch_pending arms the jump so a
 * fatal IO error longjmps back to its frame instead of Xlib's
 * exit(1). */
#include <setjmp.h>
extern sigjmp_buf fdk__x11_io_jmp;
extern volatile int fdk__x11_io_armed;

/* Window registry, implemented in x11_window.c, used by
 * x11_connection.c (to clean up on disconnect) and x11_dispatch.c (to
 * resolve an incoming XEvent's window ID back to our wrapper struct).
 * Returns NULL from the lookup if the ID isn't a window FDK created
 * (X delivers events for windows we don't own in some cases, e.g.
 * root window property notifications some WMs send). */
fdk_result fdk_x11_register_window(fdk_platform_connection *conn,
                                    fdk_platform_window *pwindow);
void fdk_x11_unregister_window(fdk_platform_connection *conn,
                                fdk_platform_window *pwindow);
fdk_platform_window *fdk_x11_find_window(fdk_platform_connection *conn,
                                          Window xwindow);

/* Declared here, defined one-per-file across x11_connection.c,
 * x11_window.c, x11_dispatch.c; assembled into the fdk_platform_ops
 * vtable in x11_ops.c. Exposed at file scope (not static) purely for
 * that cross-file wiring — none of these are called from outside
 * src/platform/x11/. */
/* Lazy input-method bring-up (1.1.0-rc1, X1): opens the XIM exactly
 * once, when a window first sees FocusIn/KeyPress — pointer-only apps
 * never pay for it. */
void fdk__x11_ensure_xim(fdk_platform_connection *conn);

/* Lazy per-window XIC (same report): the first FocusIn/KeyPress on a
 * window creates its input context. */
void fdk__x11_window_ensure_xic(fdk_platform_window *pwindow);

fdk_result fdk_x11_connect(fdk_platform_dispatch_fn dispatch,
                            void *dispatch_user_data, const char *app_id,
                            fdk_platform_connection **out_conn);
void fdk_x11_disconnect(fdk_platform_connection *conn);
int fdk_x11_get_event_fd(fdk_platform_connection *conn);
int fdk_x11_dispatch_pending(fdk_platform_connection *conn);

fdk_result fdk_x11_window_create(fdk_platform_connection *conn,
                                  const fdk_window_options *options,
                                  fdk_platform_window *parent,
                                  fdk_platform_window **out_pwindow);
/* Extension-surface adoption (1.1.0) — x11_ext.c. Wraps an add-on's
 * unmapped X window (built against the Display* fdk_extend.h hands
 * out) in a registered pwindow: FDK's event mask, XIC when an XIM
 * exists, real geometry via XGetGeometry. See platform_internal.h's
 * ext_surface_adopt contract. */
fdk_result fdk_x11_ext_surface_adopt(fdk_platform_connection *conn,
                                     const fdk_ext_native_handle *native,
                                     fdk_platform_window **out_pwindow);
/* fdk_ext_get_native_display()'s op: the Display*, borrowed. */
void *fdk_x11_native_display(fdk_platform_connection *conn);
/* fdk_ext_surface_native()'s op: the wrapped window's XID. */
fdk_result fdk_x11_ext_surface_native(fdk_platform_window *pwindow,
                                      fdk_ext_native_handle *out);
/* fdk_ext_surface_notify_configure()'s op: buffer-side resize. */
void fdk_x11_ext_surface_resized(fdk_platform_window *pwindow,
                                 fdk_i32 width, fdk_i32 height);
void fdk_x11_window_destroy(fdk_platform_window *pwindow);
void fdk_x11_window_show(fdk_platform_window *pwindow);
void fdk_x11_window_hide(fdk_platform_window *pwindow);
void fdk_x11_window_set_title(fdk_platform_window *pwindow, const char *title);

/* Window identity (1.1.0, M-4): the icon + ICCCM role ops behind the
 * same-named platform_internal.h entries. set_icon builds/replaces
 * _NET_WM_ICON (multi-image: repeated calls before the flush append
 * images); pixels==NULL/non-positive dims delete the property.
 * set_role rewrites WM_WINDOW_ROLE (NULL/empty = the "main"/
 * "popup" default). PID + WM_CLIENT_MACHINE are set at CREATE in
 * x11_window.c (no public op — they never change). */
void fdk_x11_window_set_icon(fdk_platform_window *pwindow,
                             const fdk_u32 *pixels,
                             int width, int height);
void fdk_x11_window_set_role(fdk_platform_window *pwindow,
                             const char *role);

void fdk_x11_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height);
fdk_result fdk_x11_window_set_wm_decorations(fdk_platform_window *pwindow,
                                             bool on);
fdk_result fdk_x11_window_get_position(fdk_platform_window *pwindow,
                                       fdk_i32 *out_x, fdk_i32 *out_y);
void fdk_x11_window_move_to(fdk_platform_window *pwindow, fdk_i32 x,
                            fdk_i32 y);
void fdk_x11_window_move_resize_to(fdk_platform_window *pwindow,
                                   fdk_i32 x, fdk_i32 y,
                                   fdk_i32 width, fdk_i32 height);
fdk_result fdk_x11_window_set_maximized(fdk_platform_window *pwindow,
                                        bool maximized);
fdk_result fdk_x11_window_set_minimized(fdk_platform_window *pwindow,
                                        bool minimized);
fdk_result fdk_x11_window_begin_move(fdk_platform_window *pwindow,
                                     fdk_i32 local_x, fdk_i32 local_y);
fdk_result fdk_x11_window_begin_resize(fdk_platform_window *pwindow,
                                       int edge, fdk_i32 local_x,
                                       fdk_i32 local_y);

/* Pointer introspection + cursor shaping (1.1.4; the optional ops
 * behind the same-named platform_internal.h entries).
 * fdk_x11_window_query_pointer returns nonzero only when the pointer
 * is inside the window's current bounds. fdk_x11_window_set_cursor
 * takes an fdk_window_resize_edge compass value (0 = default arrow),
 * FDK_PLATFORM_CURSOR_POINTER, or a NAMED op value (rc2, S-5);
 * out-of-range is ignored. fdk_x11_cursor_shutdown frees the
 * connection's cached font cursors — called from disconnect. */
int fdk_x11_window_query_pointer(fdk_platform_window *pwindow,
                                 fdk_i32 *out_x, fdk_i32 *out_y);
void fdk_x11_window_set_cursor(fdk_platform_window *pwindow, int edge);
void fdk_x11_cursor_shutdown(fdk_platform_connection *conn);

/* The named cursor set (1.1.0-rc2, S-5; src/platform/x11/x11_cursor.c):
 * fdk__x11_named_cursor returns the (cached) Cursor for a NAMED op
 * value — tier 1 the XCursor theme through the dlopen'd RENDER lane,
 * tier 2 the nearest core cursor-font glyph, None only when both
 * tiers honestly failed (the default arrow). fdk__x11_named_cursors_
 * shutdown frees the cache + dlclose's the RENDER lane (called from
 * fdk_x11_cursor_shutdown; display-dead aware like it). */
Cursor fdk__x11_named_cursor(fdk_platform_connection *conn, int op_shape);
void fdk__x11_named_cursors_shutdown(fdk_platform_connection *conn,
                                     int display_dead);

/* Input region (1.1.0-rc2, S-4 / A-06; x11_window.c): the XShape
 * translation of the window_set_input_region op. */
void fdk_x11_window_set_input_region(fdk_platform_window *pwindow,
                                     const fdk_rect *rects, int count);

/* Activation + startup notification (1.1.0, S-6; x11_window.c):
 * _NET_ACTIVE_WINDOW (EWMH) / raise+focus (bare X), the SN-token
 * mint with its "new:" root message, and the chunked
 * startup-notification state emitter ("new:" from the mint,
 * "remove:" from the first show of a window carrying an id). */
fdk_result fdk_x11_window_activate(fdk_platform_window *pwindow);
fdk_result fdk_x11_window_activation_token(fdk_platform_window *pwindow,
                                            char *out, size_t cap);
void fdk_x11_send_startup_info(fdk_platform_connection *conn,
                                const char *message);

/* ---- The dlopen'd RENDER cursor lane (x11_cursor.c) ----
 *
 * libXrender.so.1 pinned lazily on the first named-cursor request:
 * XRenderCreateCursor turns an ARGB depth-32 pixmap (built from the
 * shared XCursor theme loader's pixels) into a server Cursor — the
 * same thing libXcursor itself does, without the dependency. Absent
 * library or extension = NULL/sticky-off; the core cursor font
 * carries every shape through its nearest-glyph table instead. */
struct fdk_x11_cursor_render {
    void *lib;                        /* dlopen("libXrender.so.1")  */
    int ok;                           /* extension + symbols won    */
    int tried;                        /* sticky after one attempt   */
    int (*query_ext)(Display *, int *, int *);
    XRenderPictFormat *(*find_std)(Display *, int);
    Picture (*create_pic)(Display *, Drawable, XRenderPictFormat *,
                          unsigned long, const XRenderPictureAttributes *);
    void (*free_pic)(Display *, Picture);
    Cursor (*create_cursor)(Display *, Picture, unsigned int,
                            unsigned int);
};

/* PictStandardARGB32 (Xrender.h's enum value, spelled here because
 * the header's name is only visible where it is included). */
#define FDK_X11_PICT_STANDARD_ARGB32 4

/* XI2 touch (1.1.0, M-3; src/platform/x11/x11_xi2.c): dlopen'd
 * libXi behind the touch event model. try_load pins the library
 * and probes XI >= 2.2 (NULL = unavailable, sticky); select_window
 * (from window create) selects XI_TouchBegin/Update/End;
 * handle_cookie (from the dispatch loop's GenericEvent branch)
 * translates one delivered cookie; test_touch drives the SAME
 * translation+dispatch path with synthesized geometry (the
 * Wayland test_touch twin). */
struct fdk_x11_xi2 *fdk_x11_xi2_try_load(fdk_platform_connection *conn);
void fdk_x11_xi2_shutdown(struct fdk_x11_xi2 *xi2);
void fdk_x11_xi2_select_window(fdk_platform_connection *conn,
                               unsigned long xwindow);
int fdk_x11_xi2_handle_cookie(fdk_platform_connection *conn,
                              XGenericEventCookie *cookie);
int fdk_x11_xi2_test_touch(fdk_platform_connection *conn,
                          fdk_platform_window *pwindow, int phase,
                          uint32_t id, double x, double y);

/* XIM IME (1.1.0, M-1; src/platform/x11/x11_ime.c): the preedit
 * callback machinery behind FDK_EVENT_IME_PREEDIT. attach_cbs builds
 * the per-window XIMCallback array + returns the XNPreeditAttributes
 * nested list for XCreateIC (caller XFree's the LIST — the callback
 * array stays on the pwindow); dispatch is the event entry point
 * (also the test seam's body); test_apply drives the buffer math
 * headless; window_cleanup is the destroy path. fdk__x11_preedit_
 * apply is the pure UTF-8 char-index splice every draw callback
 * runs (declared here so tests/test_ime.c can pin it directly). */
void *fdk_x11_ime_attach_cbs(fdk_platform_window *pwindow);
void fdk_x11_ime_dispatch(fdk_platform_window *pwindow,
                          fdk_event_type type, const char *text,
                          int cursor);
size_t fdk_x11_ime_test_apply(char *buf, size_t cap, long chg_first,
                              long chg_length, const char *insert);
void fdk_x11_ime_window_cleanup(fdk_platform_window *pwindow);
size_t fdk__x11_preedit_apply(char *buf, size_t cap, long chg_first,
                              long chg_length, const char *insert);

/* The module's record (x11_xi2.c owns the definition's twin — the
 * dlopen handle, the extension opcode for GenericEvent matching,
 * the 2.2-touch verdict, and the libXi function table). Only the
 * opcode is read outside the module (the dispatch loop's cookie
 * match). */
struct fdk_x11_xi2 {
    void *lib; /* dlopen handle; pinned for the process lifetime */
    int opcode; /* the X Input extension's major opcode on this server */
    int touch_ok; /* server speaks XI >= 2.2 (touch events exist) */
    int warned_once; /* one INFO line per connection, not per window */
    Status (*query_version)(Display *, int *, int *);
    Status (*select_events)(Display *, Window, void *, int);
};

/* Phase 8 state helpers (x11_window.c), shared with x11_events.c's
 * PropertyNotify translation: */
/* Compare-and-flip + FDK_EVENT_WINDOW_STATE dispatch (no-op when the
 * state didn't change). */
void fdk_x11_window_update_state(fdk_platform_window *pwindow,
                                 int maximized, int minimized,
                                 int fullscreen);
/* Window's _NET_WM_STATE property -> maximized (both axes), 0 when
 * absent/unreadable. */
int fdk_x11_window_net_state_maximized(fdk_platform_window *pwindow);
int fdk__x11_net_state_fullscreen(fdk_platform_window *pwindow);
/* Shared reader: what=0 both-axes maximized, what=1 fullscreen. */
int fdk__x11_net_state(fdk_platform_window *pwindow, int what);
fdk_result fdk_x11_window_set_fullscreen(fdk_platform_window *pwindow,
                                         bool fullscreen);
/* Window's WM_STATE property -> iconic flag, -1 when unreadable. */
int fdk_x11_window_wm_state_iconic(fdk_platform_window *pwindow);
void fdk_x11_window_set_size_limits(fdk_platform_window *pwindow,
                                     fdk_size min_size, fdk_size max_size);

/* Software rendering (see x11_surface.c for the design notes). */
fdk_result fdk_x11_window_get_framebuffer(fdk_platform_window *pwindow,
                                           fdk_platform_framebuffer *out_fb);
fdk_result fdk_x11_window_present(fdk_platform_window *pwindow,
                                  const fdk_platform_damage *damage);

/* The window_ever_presented ops entry (diagnostic seam — see
 * platform_internal.h). */
int fdk_x11_window_ever_presented(fdk_platform_window *pwindow);
void fdk_x11_surface_cleanup(fdk_platform_window *pwindow);

/* Clears the in-flight flag of the slot owning `shmseg` on this
 * window (ShmCompletion routing; see x11_surface.c). Called from
 * x11_dispatch.c and from the acquire-side sync wait. */
void fdk_x11_surface_shm_completion(fdk_platform_window *pwindow,
                                    unsigned long shmseg);

/* THE lane downgrade (x11_surface.c, shared 1.1.0-rc1): salvages the
 * current staging frame into fresh classic slots and re-presents it
 * full-damage through the SHM/copy path, permanently marking the
 * window CPU. Used by the present path's own -1 verdict AND by the
 * GL self-check tick's REAL-failure decision (which lands between
 * presents). No-op when the window is not on the lane. */
fdk_result fdk__x11_surface_downgrade_from_lane(
    fdk_platform_window *pwindow);

/* Popup grabs (x11_window.c): called from window_show/hide when the
 * window is a popup. Idempotent; failures log and continue (the
 * popup still works, it just doesn't grab). POINTER-ONLY since
 * 1.1.0-rc1 (the Openbox passive-grab finding) — keys route through
 * focus, saved + restored here. */
void fdk_x11_window_popup_grab(fdk_platform_window *pwindow);
void fdk_x11_window_popup_ungrab(fdk_platform_window *pwindow);

/* Re-asserts a popup's grab after a nested popup's grab replaced it
 * (menu submenu dismissal — grabs do not stack). */
void fdk_x11_window_popup_regrab(fdk_platform_window *pwindow);

/* Stamps WHY the next release happens (outside-click / escape /
 * destroy / unmap / desktop-change / watchdog-not-viewable) — the
 * FDK_POPUP_TRACE answer to "why did my popup go away". Call BEFORE
 * the ungrab/destroy so the trace line carries it. */
void fdk_x11_window_popup_set_release_reason(
    fdk_platform_window *pwindow, const char *reason);

/* The popup lifecycle trace line (FDK_POPUP_TRACE=1; x11_window.c):
 * one INFO line tagged "popup". Safe to call from any X11 file —
 * the env check is cached; with tracing off it is a flag test. */
void fdk__x11_popup_trace(const char *fmt, ...);

/* Monotonic milliseconds (CLOCK_MONOTONIC) — the shared X11-side
 * clock for grab-held accounting, the popup watchdog, and the GL
 * self-check retry pacing. */
uint64_t fdk__x11_now_ms(void);

/* Modal-dialog grab on a toplevel (Phase 9 completion): while set,
 * no other window on the connection receives input; outside
 * presses are ignored, not dismissals. */
fdk_result fdk_x11_window_set_modal(fdk_platform_window *pwindow,
                                    bool modal);

/* Clipboard (x11_clipboard.c). Called from x11_connection.c
 * (setup/teardown) and x11_dispatch.c (helper-window events, which
 * must be routed there BEFORE the window-table lookup — the helper is
 * deliberately not in the table). */
fdk_result fdk_x11_clipboard_init(fdk_platform_connection *conn);
void fdk_x11_clipboard_shutdown(fdk_platform_connection *conn);
/* Returns 1 when the raw XEvent belonged to the clipboard helper and
 * was consumed (SelectionRequest / SelectionClear / stray
 * SelectionNotify), 0 when it is someone else's event. */
int fdk_x11_clipboard_handle_event(fdk_platform_connection *conn,
                                   const XEvent *xevent);
fdk_result fdk_x11_clipboard_set_text(fdk_platform_connection *conn,
                                      const char *text);
char *fdk_x11_clipboard_get_text(fdk_platform_connection *conn);
/* 1.3.4: the PRIMARY selection (XA_PRIMARY) over the same helper
 * window and the same ICCCM machinery. set is BEST-EFFORT (no
 * ownership-verification round-trip — the classic model re-owns per
 * selection change). */
fdk_result fdk_x11_clipboard_set_primary_text(
    fdk_platform_connection *conn, const char *text);
char *fdk_x11_clipboard_get_primary_text(fdk_platform_connection *conn);
/* 1.4.9: the clipboard's image/png surface (atomic property
 * transfers; the 4 MiB documented cap). */
fdk_result fdk_x11_clipboard_set_image(fdk_platform_connection *conn,
                                       const unsigned char *png,
                                       size_t len);
unsigned char *fdk_x11_clipboard_get_image(
    fdk_platform_connection *conn, size_t *out_len);

/* THE selection-read engine (1.4.14, x11_clipboard.c): convert
 * `selection`/`target` onto `requestor`'s atom_fdk_selection
 * property, wait the bounded notify, read atomic/multi-part/INCR
 * (foreign requestors get PropertyChangeMask for the flight, their
 * own mask restored after). The clipboard's gets pass the helper
 * window; the XDND drop fetch (x11_dnd.c) passes the drop-target
 * window — external file managers serve text/uri-list via INCR, the
 * drop that used to vanish. Returns RAW bytes (fdk_alloc'd,
 * *out_len and *out_type set) or NULL (refusal/timeout/malformation). */
unsigned char *fdk__x11_read_selection(fdk_platform_connection *conn,
                                       Window requestor, Atom selection,
                                       Atom target, unsigned long timestamp,
                                       int notify_wait_ms, Atom *out_type,
                                       size_t *out_len);

/* Drag and drop (x11_dnd.c). init/shutdown: connection lifetime
 * (called from x11_connection.c next to the clipboard's).
 * window_init: stamps XdndAware on a new window (x11_window.c calls
 * it from window_create). handle_client_message: the RECEIVER's
 * XDndEnter/Position/Leave/Drop (x11_dispatch.c routes client
 * messages here after the WM_PROTOCOLS check). source_handle_event:
 * the SOURCE's pointer/keyboard interception while a drag we began
 * is active (dispatch_pending calls it FIRST). source_handle_
 * helper_message: XDndStatus/XDndFinished arriving on the helper.
 * source_tick: the Finished watchdog (dispatch loop tail).
 * serve_selection: SelectionRequests naming the XdndSelection (the
 * dispatch routes them to the DnD serving before the clipboard's
 * generic path, which would otherwise refuse them). The two ops
 * entries behind platform_internal.h close the set. */
fdk_result fdk_x11_dnd_init(fdk_platform_connection *conn);
void fdk_x11_dnd_shutdown(fdk_platform_connection *conn);
void fdk_x11_dnd_window_init(fdk_platform_window *pwindow);
int fdk_x11_dnd_handle_client_message(fdk_platform_connection *conn,
                                      const XEvent *xevent);
int fdk_x11_dnd_source_handle_event(fdk_platform_connection *conn,
                                     XEvent *xevent);
int fdk_x11_dnd_source_handle_helper_message(
    fdk_platform_connection *conn, const XEvent *xevent);
void fdk_x11_dnd_source_tick(fdk_platform_connection *conn);

/* The popup-grab watchdog + desktop-switch release (1.1.0-rc1,
 * x11_dispatch.c): called at the end of every dispatch_pending —
 * releases any popup grab whose window is no longer viewable, and
 * dismisses grabbed popups latched by a _NET_CURRENT_DESKTOP change.
 * Zero cost when no popup holds a grab and no switch happened. */
void fdk_x11_popup_watchdog_tick(fdk_platform_connection *conn);
int fdk_x11_dnd_serve_selection(fdk_platform_connection *conn,
                                const XSelectionRequestEvent *req);
void fdk_x11_dnd_source_finish(fdk_platform_connection *conn, int status);
void fdk_x11_window_set_drop_formats(fdk_platform_window *pwindow,
                                     int formats);
fdk_result fdk_x11_drag_begin(fdk_platform_window *origin, int formats,
                              const char *text,
                              const char *const *uris, size_t uri_count,
                              void (*on_done)(int, void *), void *user);

#endif /* FDK_X11_PLATFORM_H */
