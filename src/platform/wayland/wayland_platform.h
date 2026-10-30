/*
 * wayland_platform.h — Wayland backend internals
 *
 * Only included by files inside src/platform/wayland/. No file
 * outside this directory may include wayland-client.h or the
 * generated xdg-shell protocol headers — same "no backend leakage"
 * rule as x11_platform.h, see docs/architecture.md.
 */

#ifndef FDK_WAYLAND_PLATFORM_H
#define FDK_WAYLAND_PLATFORM_H

#include "platform/platform_internal.h"

/* GPU present lane (1.2.0 / D-1) — the EGL counterpart of
 * x11_platform.h's x11_gl.h include. */
#include "platform/wayland/wayland_egl.h"

#include "platform/wayland/generated/xdg-shell-client-protocol.h"
#include "platform/wayland/generated/xdg-decoration-unstable-v1-client-protocol.h"
#include "platform/wayland/generated/primary-selection-unstable-v1-client-protocol.h"

#include <stddef.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

struct fdk_platform_connection {
    /* Application id (fdk_init_options.app_id), duplicated at
     * connect; NULL when unset. Applied to every toplevel via
     * xdg_toplevel.set_app_id — the compositor-side identity window
     * rules (like sway's for_window) and taskbars match. When the
     * application passed none, connect() derives one from
     * /proc/self/cmdline's first token (argv[0] basename) so demos
     * still identify honestly instead of "fdk.app"-alike. */
    char *app_id;
    struct wl_display *display;
    struct wl_registry *registry;

    /* 1.3.2 — dead-connection circuit breaker (the Wayland
     * counterpart of X11's display_dead): set the first time a
     * flush/read/dispatch fails terminally (EPIPE = the compositor
     * is gone). dispatch_pending() then returns FDK_ERR_NO_DISPLAY
     * immediately, ONCE-logged — without this, a dead compositor
     * meant an ERROR line and a failed syscall per pump call, at
     * full loop speed, forever (found live in the rigs: two runaway
     * example apps wrote 4.5 GB and 2.6 GB of duplicate error lines
     * to their console logs). */
    int display_dead;

    /* Dedicated event queue for wl_buffer events (1.1.6). Every
     * wl_buffer created through create_shm_buffer() is moved here
     * via wl_proxy_set_queue, so wl_buffer::release — often the ONLY
     * event the render path needs to make progress — can be
     * dispatched on its own (wl_display_dispatch_queue_pending)
     * without running unrelated listeners re-entrantly. The main
     * loop drains it after every default-queue dispatch
     * (wayland_dispatch.c); the slot-exhaustion wait in
     * wayland_window.c blocks on it with a bounded poll. */
    struct wl_event_queue *release_queue;

    /* cursor-shape-v1 (1.1.0-rc1, the W2 report): the OPTIONAL
     * manager global and the per-pointer device derived from it.
     * When the compositor offers the protocol, cursor shaping goes
     * through set_shape(serial, enum) — no client-side cursor
     * surfaces, no theme-file hunting, and the compositor renders
     * the shape in its own cursor pipeline. Absent (GNOME before
     * 46, older compositors), shaping falls back to the XCursor
     * theme path in wayland_cursor.c. The device is (re)created
     * whenever the seat's pointer object is (seat_capabilities). */
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;

    /* xdg-toplevel-icon-v1 (1.1.0, M-4): the OPTIONAL manager for
     * window icons. Present on KDE Plasma 6.3+ and any compositor
     * that adopted the staging protocol; absent everywhere else at
     * the time of writing (wlroots < 0.19, GNOME) — set_icon then
     * logs one INFO line and no-ops, the documented honest
     * unsupported posture (Wayland offers no icon fallback). The
     * manager's icon_size/done events are advisory (preferred
     * sizes for scalable sources); FDK ships raster pixels and
     * lets the compositor scale, so they are not listened to. */
    struct xdg_toplevel_icon_manager_v1 *toplevel_icon_manager;

    /* idle-inhibit (1.1.0, S-3): the OPTIONAL manager — absent means
     * the compositor has no idle-inhibit protocol and the window op
     * answers FDK_ERR_UNSUPPORTED (the D-Bus fallback engages). */
    struct zwp_idle_inhibit_manager_v1 *idle_inhibit_manager;

    /* xdg-activation (1.1.0, S-6): the OPTIONAL manager. Absent
     * (rare — sway, KDE and GNOME all implement it) means activate
     * and token minting answer FDK_ERR_UNSUPPORTED. The launch_token
     * is the string captured from XDG_ACTIVATION_TOKEN at connect
     * (a launcher vouched for us): it is spent by the FIRST
     * activate() — one launch, one token, the specification's
     * contract. The variable is unset in our own environment right
     * after the read so a spawned grandchild cannot inherit a stale
     * token (the protocol's own hygiene rule). */
    struct xdg_activation_v1 *activation;
    char *launch_token;

    /* text-input-v3 (1.1.0, M-1): the OPTIONAL IME manager + the
     * per-seat text input object (wayland_ime.c). enter/leave are
     * the focus gates; the latched pending strings + the showing
     * flag are the v3 batch state the `done` event applies;
     * ime_enabled tracks the v3 enabled session (its enable event
     * only fires on a false->true commit — the self-heal seam for
     * compositors that drop focus without a leave). */
    struct zwp_text_input_manager_v3 *text_input_manager;
    struct zwp_text_input_v3 *text_input;
    fdk_platform_window *ime_focus;      /* the enter() surface's owner */
    char *ime_pending_preedit;           /* latched, freed per batch   */
    char *ime_pending_commit;            /* latched, freed per batch   */
    int ime_pending_preedit_cursor;
    int ime_preedit_showing;             /* a PREEDIT was dispatched   */
    int ime_enabled;                     /* v3 session open (no leave) */
    char *ime_surrounding;               /* last pushed context (copy) */
    int ime_surrounding_cursor;

    /* --- GPU present lane (1.2.0 / D-1, wayland_egl.c) -------------
     *
     * egl: the per-connection lane state — the dlopen'd libEGL +
     * libwayland-egl handles, the EGLDisplay for this connection's
     * wl_display, the shared quad program, and the renderer-policy
     * verdict. Loaded LAZILY at the first framebuffer acquisition
     * of any window (never at connect: headless apps and no-window
     * apps must not touch EGL at all). NULL = the lane is not
     * available on this connection (no libEGL, no libwayland-egl,
     * EGL init failure, software renderer without FDK_GL_FORCE) or
     * the policy refused it. egl_tried makes the attempt one-shot
     * — NULL is sticky, windows never re-attempt. The exact
     * X11/GLX shape (x11_platform.h's gl/gl_tried), carried over
     * wholesale. */
    struct fdk_wayland_egl *egl;
    int egl_tried;

    /* Globals discovered during the initial registry roundtrip
     * (see wayland_registry.c). All required for Phase 2's
     * "create a window, get input, handle close/resize" scope —
     * none are optional the way e.g. a clipboard-manager global
     * would be. */
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct xdg_wm_base *wm_base;

    /* OPTIONAL global (1.1.0, D-2): wl_subcompositor — the factory
     * for wl_subsurface roles (CORE protocol, but a compositor
     * still has to advertise the global). Mandatory in practice
     * (every compositor that offers wl_compositor offers this too);
     * absent means fdk_ext_subsurface_create answers
     * FDK_ERR_UNSUPPORTED — the honest refusal, never a fake. */
    struct wl_subcompositor *subcompositor;

    /* OPTIONAL (1.1.0, D-4): wlr-foreign-toplevel-management — the
     * taskbar substrate (wayland_toplevel.c). The global's NAME is
     * recorded at registry time; the MANAGER is bound lazily at
     * fdk_toplevel_list_open — the protocol sends the initial
     * toplevel burst the moment the manager object exists, and a
     * bind without a listener attached would lose it forever (the
     * sway rig caught this in the first run). Absent (name 0, GNOME,
     * KDE pre-5.x) = open answers NULL, the honest degradation.
     * foreign_toplevels_live enforces the one-list rule (libwayland's
     * one-listener-per-proxy). */
    uint32_t foreign_toplevel_name;
    struct zwlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
    int foreign_toplevels_live;

    /* OPTIONAL global (Phase 8): zxdg_decoration_manager_v1 — absent
     * means the compositor offers no xdg-decoration protocol, and
     * fdk_window_set_decorated() then honestly returns
     * FDK_ERR_UNSUPPORTED rather than stacking FDK's band over the
     * compositor's server-side decorations. */
    struct zxdg_decoration_manager_v1 *decoration_manager;

    /* --- HiDPI (Phase 3 completion) --------------------------------
     *
     * wl_output globals are bound (version >= 2 for the scale event)
     * and tracked so windows can derive their scale from the outputs
     * they are displayed on (xdg_toplevel enter/leave). Both optional
     * globals below extend scale support to FRACTIONAL factors:
     *   - wp_viewporter: maps an exact source rectangle of the buffer
     *     onto the surface (the mechanism fractional scaling needs);
     *   - wp_fractional_scale_manager_v1: the compositor's preferred
     *     scale in 120ths of a unit, per window.
     * When either is missing, FDK uses integer wl_surface buffer
     * scale only — correct on every compositor, fractional factors
     * just round to the nearest supported integer. */
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;

    /* Bound outputs (grown on demand; wl_output v2+ carries the
     * scale event). Kept for the window's lifetime of the
     * connection; outputs that disappear are left in place with
     * scale 0 = "gone" and skipped by the max-scale walk. */
    struct {
        struct wl_output *output;
        int scale; /* wl_output::scale, or 0 once the global is gone */
        uint32_t name; /* registry global name (retirement key) */
    } *outputs;
    size_t output_count;
    size_t output_capacity;

    /* Serial of the most recent pointer-button event (Phase 8):
     * xdg_toplevel.move/resize require the serial of the triggering
     * input event — the compositor validates it against its own
     * event history, so a stale serial makes the request a no-op. */
    uint32_t last_button_serial;

    /* Serial of the most recent input event of ANY kind (button or
     * key — Phase 9): wl_data_device.set_selection must cite one,
     * like move/resize. 0 until real input has arrived; compositors
     * are permitted to ignore set_selection with serial 0 (documented
     * in fdk_clipboard.h). */
    uint32_t last_input_serial;

    /* --- Key repeat (1.3.5, wl_keyboard::repeat_info) -----------
     *
     * Wayland repeat is CLIENT-driven: the protocol hands us the
     * rate/delay and never re-sends the press. One key repeats at a
     * time — the LATEST press re-arms (the GTK rule); its release,
     * keyboard_leave, or a 0 rate disarms. repeat_deadline_ms is
     * monotonic-clock milliseconds; dispatch_pending fires ONE due
     * repeat per call (the pump's next_wakeup_ms cap keeps the loop
     * coming back at period speed, and a slow pump resyncs instead
     * of bursting). rate 0 (the protocol's "repeat disabled") is a
     * supported state, not an error. */
    int repeat_armed;
    uint32_t repeat_key;
    uint32_t repeat_rate_hz;
    int32_t repeat_delay_ms;
    long long repeat_deadline_ms;

    /* --- Clipboard (Phase 9, wl_data_device) --------------------
     *
     * data_device_manager is an OPTIONAL global: without it the
     * clipboard ops report UNSUPPORTED. data_device is created from
     * the manager + seat once the seat's capabilities arrive (it is
     * legal to get_data_device on a seat regardless of caps). */
    struct wl_data_device_manager *data_device_manager;
    struct wl_data_device *data_device;

    /* --- PRIMARY selection (1.3.4, wp_primary_selection_unstable_v1)
     * ---
     *
     * A parallel little protocol to the data-device clipboard above
     * (same shapes, different interfaces): manager global -> device
     * for our seat -> source when WE own it, offer when another
     * client does. The manager is an OPTIONAL global — compositors
     * without the protocol (GNOME's Mutter carried it only late,
     * Weston never) leave these NULL and the primary ops honestly
     * report FDK_ERR_UNSUPPORTED instead of faking a local buffer.
     * The wlroots family (sway, labwc, river, ...) all offer it. */
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct zwp_primary_selection_device_v1 *primary_device;
    struct zwp_primary_selection_source_v1 *primary_source;
    char *primary_owned_text;
    struct zwp_primary_selection_offer_v1 *primary_selection_offer;
    int primary_selection_offer_has_text;
    struct zwp_primary_selection_offer_v1 *primary_pending_offer;
    int primary_pending_offer_has_text;

    /* Our side of ownership: a live data source plus the text it
     * serves (send may be called once per requestor, so the text
     * stays for the source's lifetime). cancelled destroys both. */
    struct wl_data_source *clip_source;
    char *clip_owned_text;
    /* 1.4.9: the image content (encoded PNG). One clipboard content
     * at a time — owning an image clears owned text and vice versa;
     * the source offers image/png only. */
    unsigned char *clip_owned_png;
    size_t clip_owned_png_len;

    /* The compositor's current selection, as seen by us: the latest
     * wl_data_device::data_offer that a ::selection event named.
     * NULL = no selection (or we own it — compositors do not send a
     * client its own selection). has_text_mime is set by the offer's
     * ::offer events (text/plain;charset=utf-8 or bare text/plain).
     *
     * pending_offer is the offer currently streaming its MIME list
     * (announced by ::data_offer, not yet named by ::selection — it
     * might turn out to be a drag-and-drop offer instead, which is
     * exactly why the MIME flags live on the pending slot rather
     * than on the connection: a non-text DnD offer must never leak
     * its flags into the selection slot). */
    struct wl_data_offer *selection_offer;
    int selection_offer_has_text;
    int selection_offer_has_image; /* 1.4.9: image/png in the MIME list */
    struct wl_data_offer *pending_offer;
    int pending_offer_has_text;
    int pending_offer_has_uris; /* 1.2.0: text/uri-list in the pending
                                   offer — drag offers carry it */
    int pending_offer_has_image; /* 1.4.9 */

    /* --- Drag and drop (1.2.0, wayland_dnd.c) ---
     *
     * Receiver: the drag the data_device currently tracks (enter/
     * motion/leave/drop in the device listener; wayland_clipboard.c
     * routes them here). drag_hover NULL when the drag is not over
     * an FDK window or was not accepted (set_actions answered only
     * on a format intersection). Source: OUR outgoing drag (the
     * wl_data_source handed to start_drag; its listener reports
     * send / finished / cancelled). */
    struct wl_data_offer *drag_offer;
    fdk_platform_window *drag_hover;
    int drag_offered;    /* FDK_DRAG_FORMAT mask the offer carries */
    int drag_accepted;   /* the intersection we answered with      */
    double drag_x, drag_y;
    struct wl_data_source *drag_source;
    char *drag_text;           /* owned; TEXT payload for ::send     */
    char *drag_uri_payload;    /* owned; uri-list payload for ::send */
    int drag_drop_performed;
    /* Same-connection drags (1.1.0, S-9): the payload read inside
     * fdk_wayland_dnd_device_drop DISPATCHES the queue to serve our
     * own data_source's ::send — which can also deliver the
     * compositor's post-drop ::leave / ::cancelled. Those handlers
     * must not tear the drag state down MID-READ (the offer proxy
     * would die under the read): drag_drop_active fences them, and
     * the source-side report they would have fired is DEFERRED (with
     * its status) to the drop handler's tail, which owns the state
     * once the read is done. */
    int drag_drop_active;
    int drag_deferred_report;  /* a source-side end event arrived    */
    int drag_deferred_status;  /* ... with this fdk_drag_status      */
    void (*drag_on_done)(int status, void *user);
    void *drag_on_done_user;
    /* Drag icon (1.1.0, S-9): the wl_surface handed to start_drag as
     * the icon — compositor-owned position/blend for the drag's
     * life. Destroyed at drag end (the wl_data_source destroyed /
     * cancelled / finished path): surface first, one roundtrip (the
     * compositor releases its reference), then the buffer. The SHM
     * mapping is already gone at creation (the cursor-upload rule:
     * the server keeps its own mapping of the pool). NULL when the
     * drag carries no icon. */
    struct wl_surface *drag_icon;
    struct wl_buffer *drag_icon_buffer;

    /* Bound from the seat's capabilities (wayland_seat.c). May be
     * NULL if the seat genuinely has no keyboard/pointer/touch —
     * checked before use, never assumed present. */
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wl_touch *touch; /* M-3: wl_touch, direct contact slots */

    /* --- Tablets / stylus (1.1.0, D-3, wayland_tablet.c) -----------
     *
     * The zwp_tablet_v2 family: the manager (optional global), the
     * per-seat tablet seat, and the tool records. Tools are tracked
     * (not tablets/pads — no fdk_event needs the slabs, and pad
     * programmability is the D-030 honest gap). The pending_* flags
     * are the protocol's frame batching: down/up/motion/pressure/
     * tilt arrive as individual events, the FRAME commits them. */
    struct zwp_tablet_manager_v2 *tablet_manager;
    struct zwp_tablet_seat_v2 *tablet_seat;

    /* wl_touch slot routing (M-3): DOWN carries the surface; UP and
     * MOTION carry only the id — this table maps id → the window
     * the contact went down on, so the whole contact's events land
     * on the right dispatch target. Bounded at FDK_WL_TOUCH_IDS;
     * overflow contacts are dropped with one INFO line (the core's
     * tap/scroll policy needs exactly <= 2 sane slots anyway — a
     * 30-finger palm grab is not precision input). */
#define FDK_WL_TOUCH_IDS 32
    struct {
        int in_use;
        uint32_t id;
        fdk_platform_window *win;
    } touch_ids[FDK_WL_TOUCH_IDS];

    /* --- Tablet tool table (1.1.0, D-3) -----------------------------
     *
     * Bounded like the touch id table; the FDK `tool` id handed to
     * applications is the slot index + 1. proxy == NULL && id != 0
     * marks the TEST SEAM's record (no compositor proxy behind it). */
#define FDK_WL_TABLET_TOOLS 8
    struct fdk_wl_tablet_tool {
        struct zwp_tablet_tool_v2 *proxy;
        fdk_u32 id;
        fdk_u32 type; /* fdk_tablet_tool_type (the `type` event) */
        fdk_platform_window *prox;
        fdk_pointf last;
        float pressure;
        float tilt_x;
        float tilt_y;
    } tablet_tools[FDK_WL_TABLET_TOOLS];
    int tablet_pending_down;   /* frame-batched tip transitions     */
    int tablet_pending_up;
    int tablet_pending_motion;
    int tablet_seam_current;   /* the TEST SEAM's active tool slot
                                 * (-1 = none): the seam mints records
                                 * without proxies, and this index
                                 * says which one the event phases
                                 * target (tests only, but it rides
                                 * the connection so multiple
                                 * contexts stay independent). */

    /* xkbcommon keymap state, populated from the compositor-supplied
     * keymap (wl_keyboard::keymap event) rather than FDK guessing at
     * a layout — this is the standard Wayland approach, in contrast
     * to X11 where XLookupString queries the X server's active
     * layout directly (see x11_events.c). */
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* Pointer focus tracking: Wayland delivers pointer button/motion
     * events against whichever surface last got a wl_pointer::enter,
     * not tagged per-event the way X11's XEvent carries .window. */
    fdk_platform_window *pointer_focus;
    fdk_platform_window *keyboard_focus;
    double pointer_x, pointer_y; /* last known position, surface-local */

    /* --- Cursor shaping (1.1.6, wayland_cursor.c) -----------------
     *
     * Wayland clients own their cursor pixels: wl_pointer::enter must
     * be answered with wl_pointer.set_cursor(serial, surface, hx, hy)
     * where surface carries the cursor image as an ordinary wl_shm
     * buffer. FDK loads those images from the system's XCursor theme
     * files by hand (libxcursor is NOT a dependency — same
     * distro-agnostic choice as the X11 backend's cursor-font
     * glyphs). One shared cursor surface per connection; a small
     * cache maps each loaded shape (cursor NAME + nominal size) to
     * its uploaded wl_buffer + hotspot. theme_state: 0 = untouched,
     * 1 = a theme file was found, -1 = themes were searched and none
     * was usable (every later shape request then degrades honestly
     * with a single DEBUG line — the compositor's default arrow
     * stays, which is exactly the pre-1.1.6 behavior). */
    struct wl_surface *cursor_surface;
    struct {
        char *name;              /* cursor name, e.g. "left_ptr"    */
        int size;                /* nominal size the image was picked at */
        struct wl_buffer *buffer;
        int hotspot_x, hotspot_y;
    } *cursor_cache;
    size_t cursor_cache_count;
    size_t cursor_cache_capacity;
    int cursor_theme_state;

    fdk_platform_dispatch_fn dispatch;
    void *dispatch_user_data;

    /* Same registry pattern as the X11 backend and fdk_context — see
     * x11_platform.h's struct fdk_platform_connection comment and
     * context_internal.h for the shared rationale. */
    fdk_platform_window **windows;
    size_t window_count;
    size_t window_capacity;

    int last_dispatch_errno; /* set by wl_display_dispatch() failure */
};

#define FDK_WL_RENDER_SLOTS 4

/* The GPU lane's small-surface gate (1.2.0 / D-1) — the same value
 * and rationale as x11_surface.c's FDK_GL_MIN_PIXELS: a window at or
 * below this many pixels stays on wl_shm (the whole panel/popup
 * class; their presents are a few hundred KB of memcpy while the
 * lane's per-window fixed costs — EGL context + surface + program
 * setup — cost milliseconds). FDK_GL_FORCE overrides, exactly as on
 * X11. */
#define FDK_WL_GL_MIN_PIXELS 131072

/* Bounded release wait (1.1.6; reworked 1.1.0-rc1): when every render
 * slot is in flight and the application still wants a framebuffer,
 * FDK waits this long — READING the display socket (queue-scoped
 * prepare_read, see get_framebuffer) — for the compositor to release
 * a buffer before considering reaping or refusing. The 1.1.6 budget
 * was 100ms of what turned out to be pure usleep under a busy default
 * queue (the W1 freeze: the dispatch loop collapsed to ~10 events/s
 * and wlroots killed clients whose 4096-byte server buffer backed
 * up). With the wait actually reading events, a release that is on
 * the wire lands within one poll slice (~1ms locally) and one that
 * the compositor will send at its next frame lands within ~16ms at
 * 60Hz; 25ms covers a slow compositor's release latency while never
 * blocking the event loop long enough to matter. Slow compositors
 * (Muffin's experimental Wayland session, the 1.1.6 report) see
 * delayed frames instead of dropped ones; the pacing gate in
 * fdk_window_paint's auto-paint tail keeps repaint attempts off the
 * refusal path in the first place. */
#define FDK_WL_RELEASE_WAIT_MS 25

/* Interactive-resize churn bound (1.1.7): when every busy slot holds
 * a buffer at the WRONG size (the interactive-resize state — every
 * configure step changes the target), waiting for a release at the
 * CURRENT size is provably futile: no release can ever match, so the
 * 1.1.6 full wait burned FDK_WL_RELEASE_WAIT_MS of idle polling per
 * frame before the wrong-size reaper fired anyway (~10fps resizing
 * with an idle CPU — the 1.1.7 live report: "still laggy when
 * resize, the CPU is not being fully used"). The churn budget is one
 * bounded poll slice: fast compositors still get the chance to
 * release (recycle or reap-after-release, no in-flight destroy at
 * all); slow ones get the reaper almost immediately. */
#define FDK_WL_CHURN_WAIT_MS 10

/* How long a refusal may re-warn at full volume (the log-spam guard:
 * one WARN per stall episode, DEBUG for the rest). */
#define FDK_WL_REFUSE_WARN_INTERVAL_MS 2000

/* Frame-pacing guard: if the compositor stays silent this long after
 * a commit (ms), fdk_wayland_window_frame_ready() reports ready
 * anyway. Hidden surfaces legitimately never receive frame
 * callbacks; FDK's contract is to pace, never to starve. */
#define FDK_WL_FRAME_GUARD_MS 250

struct fdk_platform_window {
    fdk_platform_connection *conn;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    /* Toplevel icon (1.1.0, M-4): the xdg-toplevel-icon-v1 pair a
     * set_icon call leaves on this toplevel. The PROTOCOL lets the
     * icon object die right after manager.set_icon ("can be
     * destroyed without the toplevel losing its icon") — but the
     * wl_buffer must outlive the ICON, so the honest lifetime here
     * is: both stay on the window, both are destroyed together (a
     * replacement set_icon, window destroy). Buffer contents are
     * frozen the moment add_buffer is sent (the spec forbids
     * touching the pool region afterwards), which is exactly why
     * they are never recycled. NULL when no icon is set or the
     * compositor never offered the manager (the set_icon no-op). */
    struct xdg_toplevel_icon_v1 *toplevel_icon;
    struct wl_buffer *toplevel_icon_buffer;
    fdk_size toplevel_icon_buffer_size; /* the SQUARE buffer dims */
    /* Warn-once stamp for the manager-less INFO line (M-4): the
     * no-op itself is permanent per compositor, the LOG is once
     * per window so a per-frame set_icon caller cannot spam. */
    int icon_absent_logged;
    /* Idle inhibitor (1.1.0, S-3): the per-surface
     * zwp_idle_inhibitor_v1 while engaged (NULL otherwise); dies
     * with the window (the destroy path) or at the idle module's
     * release — whichever comes first. */
    struct zwp_idle_inhibitor_v1 *idle_inhibitor;
    /* Extension surface (1.1.0, fdk_extend.h): nonzero when this
     * wrapper ADOPTED an add-on's wl_surface (wayland_ext.c) — the
     * surface's ROLE belongs to the add-on (a layer surface, an
     * add-on protocol FDK does not know), so xdg_surface and
     * xdg_toplevel stay NULL and every xdg-shell gate below
     * (first-configure commit deferral, the show()/hide() commit
     * handshake, toplevel resize/limits) must be bypassed: the
     * add-on's own role handshake drives mapping and size, reported
     * back through fdk_ext_surface_notify_configure(). */
    int ext;
    int popup;                    /* Phase 9: xdg_popup window      */
    struct xdg_popup *xdg_popup;  /* set when popup                */
    /* Subsurface role (1.1.0, D-2 / B-16): set when this wrapper
     * carries an FDK-CREATED child surface (wayland_ext.c's
     * subsurface_create) — the wl_subsurface proxy that owns the
     * parent/child relationship. The role must die BEFORE the
     * wl_surface (destroy order in window_destroy); position/stack
     * /mode requests ride it. `is_subsurface` (the flag) gates the
     * ext-surface ops that assume a ROLE-FREE surface and the
     * popup-parent refusal (xdg_popup cannot anchor to a
     * wl_subsurface). `sub_parent` links the child to its parent
     * wrapper — the ancestor walk that applies tree state. */
    struct wl_subsurface *subsurface;
    struct fdk_platform_window *sub_parent;
    int is_subsurface;            /* 1.1.0 D-2: FDK-owned child    */
    /* Input-transparent popup (1.3.2, tooltips): set at CREATE from
     * fdk_window_options.input_transparent — empty input region on
     * the surface before the first commit, and NO xdg_popup.grab:
     * the hint is click-through and never takes keyboard ownership
     * from the owner (the create-time contract, see
     * platform_internal.h). */
    int input_transparent;        /* 1.3.2: passive (tooltip) popup */
    int drop_formats;             /* 1.2.0 DnD: accepted drop formats */

    /* Solid background buffer — the Wayland equivalent of X11's
     * background pixel. Wayland compositors map nothing until a
     * client commits an actual buffer, so without this a Phase 2
     * window would be invisible (see attach_background_buffer() in
     * wayland_window.c). wl_shm_pool objects are destroyed at buffer
     * creation time — legal per the wl_shm spec, which keeps the
     * server-side mapping alive until the buffer itself is destroyed
     * — so only the buffer needs tracking here. The buffer is
     * destroyed from its wl_buffer::release listener, never reused:
     * content never changes, a resize simply makes a new one.
     *
     * Once an application renders via fdk_surface, this background
     * path is bypassed (rendered_ever / render_pending below) and the
     * committed render buffers take over visibility duty. */
    struct wl_buffer *buffer;   /* current background buffer, NULL when none */
    fdk_size buffer_size;       /* dimensions of `buffer` */
    int buffer_attached;        /* nonzero while `buffer` is committed to the surface */
    /* 1.3.2 — every background buffer the COMPOSITOR may still hold:
     * the current one (== `buffer` above, pre-render) plus any
     * REPLACED ones whose wl_buffer::release has not arrived yet.
     * The release listener removes its own entry (and destroys the
     * proxy); window destroy reaps whatever remains. Without this
     * set, a replaced-but-unreleased background buffer was orphaned
     * at window destroy with its listener still holding this window
     * pointer — the queued release then dispatched after the free
     * was a use-after-free (found live by the clipboard interop rig
     * once the first-configure EXPOSE began presenting popup content
     * over the background fill). The cap bounds a pathological
     * configure storm; overflow destroys the oldest entry
     * (libwayland drops that object's late events — zombie proxies). */
#define FDK_WL_BG_SLOTS 8
    struct wl_buffer *bg_pending[FDK_WL_BG_SLOTS];

    /* --- Software rendering (fdk_surface machinery) ---
     *
     * A small fixed pool of wl_shm buffers that are RECYCLED, not
     * destroyed: once the compositor releases a buffer
     * (wl_buffer::release), the buffer and its client-side mapping
     * stay alive in its slot, marked `released`, ready for the next
     * acquisition. This is what makes damage-tracked partial redraw
     * CORRECT and not merely fast: a recycled (or freshly created)
     * buffer is pre-filled with a copy of the currently visible
     * frame before being handed to the application, so every pixel
     * outside the newly drawn damage region matches the screen —
     * required, because compositors are allowed to ignore damage
     * hints and scan out the whole buffer. A buffer is only truly
     * destroyed on window destruction or a size change.
     *
     * Slot states: buffer == NULL -> free. buffer != NULL &&
     * buffer == render_pending -> acquired by the app, being drawn.
     * buffer != NULL && buffer == the surface's live `buffer` ->
     * currently committed/visible. Otherwise in flight (or released
     * and awaiting reuse). */
    struct {
        struct wl_buffer *buffer; /* NULL = slot free */
        uint32_t *pixels;         /* mmap mapping backing `buffer` */
        size_t length;            /* mapping size in bytes */
        fdk_size size;            /* buffer dimensions */
        int released;             /* compositor done -> reusable */
    } render_slots[FDK_WL_RENDER_SLOTS];
    struct wl_buffer *render_pending; /* acquired but not yet presented */
    int rendered_ever;                /* first render present() completed */

    /* --- GPU present lane (1.2.0 / D-1, wayland_egl.c) --------------
     *
     * egl_win: the per-window lane state (EGL surface + context +
     * wl_egl_window + texture + staging buffer) while this window
     * presents through the GPU lane; NULL on the CPU/wl_shm path.
     * egl_downgraded: sticky per-window refusal — a window that
     * downgraded (lane failure, or the small-surface policy) never
     * re-engages; the connection-wide policy refusal (software
     * renderer without FDK_GL_FORCE, no usable config) is sticky on
     * the lane state itself. The exact X11 shape (gl_win /
     * gl_downgraded). */
    struct fdk_wayland_egl_window *egl_win;
    int egl_downgraded;
    fdk_size render_size;  /* the lane's current staging size (the SHM
                            * path derives sizes from the slots)     */
    int lane_frame_deferred; /* a lane present returned before the
                              * first configure; the configure handler
                              * flushes it (see
                              * fdk__wayland_commit_deferred_first_frame) */

    /* --- HiDPI (Phase 3 completion) --------------------------------
     *
     * The window's scale in 120ths of a logical unit (240 = 2x,
     * 300 = 2.5x), exactly the fractional-scale-v1 unit — integer
     * factors are multiples of 120. Sources, in precedence order:
     * the compositor's wp_fractional_scale_v1::preferred_scale (when
     * the protocol pair is available), else the maximum wl_output
     * scale among the outputs the toplevel is currently shown on
     * (xdg_toplevel enter/leave), else 120 = 1x. buffer_scale is the
     * INTEGER wl_surface scale actually applied (exact for integer
     * factors; the viewport carries the fractional remainder — see
     * apply_window_scale()). Render buffers are PHYSICAL pixels:
     * logical size x buffer_scale (rounded up under a viewport).
     * scale_applied tracks that the protocol objects match the
     * current value (re-applied on change before the next commit). */
    int scale_x120;      /* preferred scale, 120ths (>= 120)          */
    int buffer_scale;    /* integer wl_surface buffer scale (>= 1)    */
    int scale_applied;   /* scale_x120 pushed to the protocol yet?    */
    struct wp_viewport *viewport;          /* NULL without viewporter */
    struct wp_fractional_scale_v1 *fractional; /* NULL without mgr    */

    /* Outputs this toplevel currently occupies (xdg_toplevel
     * enter/leave), driving the output-derived scale when the
     * fractional-scale protocol is unavailable. Small dynamic array
     * of borrowed wl_output pointers; capacity grows on demand. */
    struct wl_output **entered_outputs;
    size_t entered_count;
    size_t entered_capacity;

    /* --- Frame pacing (wl_surface.frame) ---
     *
     * Every render present requests a frame callback after its
     * commit; the callback (frame_callback_done below) sets
     * frame_ack, which fdk_wayland_window_frame_ready() reports —
     * together with the FDK_WL_FRAME_GUARD_MS starvation guard and
     * the "never presented yet" case. Callbacks can only arrive
     * while the application pumps events; that contract is
     * documented in fdk_surface.h's frame-pacing section. */
    int frame_ack;        /* compositor acknowledged the last frame */
    fdk_i64 frame_commit_ms; /* monotonic ms of the last render commit */

    /* Last time (monotonic ms) the slot-exhaustion path refused an
     * acquisition at WARN volume — see FDK_WL_REFUSE_WARN_INTERVAL_MS
     * (the 1.1.6 log-spam guard; 0 = never). */
    fdk_i64 last_refuse_warn_ms;

    /* The pending wl_surface.frame callback, destroyed when `done`
     * arrives (frame_callback_done) OR at window destruction — a
     * callback whose window dies first would otherwise leak its
     * proxy (a real leak the sway-headless test caught: present ->
     * destroy before the compositor answers). */
    struct wl_callback *frame_cb;

    fdk_size last_size;
    fdk_size pending_size;   /* accumulated from configure until ack+commit */
    int configured;          /* nonzero once the first xdg_surface.configure
                                 has been acked — required before the first
                                 wl_surface.commit per xdg-shell protocol */

    /* --- Phase 8: window-state + decoration bookkeeping ---
     *
     * maximized is derived from xdg_toplevel::configure's states
     * array (the compositor's word, not our request); minimized is
     * request-optimistic — the protocol has no minimized state in
     * configure and no acknowledgement of set_minimized, so it is
     * cleared on the next activated configure (compositors send
     * activated when a window is un-minimized into focus). Flips
     * dispatch FDK_EVENT_WINDOW_STATE.
     *
     * fullscreen (1.1.5) is likewise derived from the states array,
     * but is NOT public API — FDK exposes no fullscreen request yet —
     * it exists purely as the resize gate: while the toplevel is
     * maximized or fullscreen the compositor owns the geometry, and
     * xdg-shell requires the client to commit the configured size.
     * Committing anything else is a protocol error on strict
     * compositors (weston kills the connection: "xdg_surface
     * geometry is larger than the configured fullscreen state"),
     * so fdk_wayland_window_resize refuses while either is set.
     *
     * toplevel_decoration is the per-window xdg-decoration object,
     * created on the first set_wm_decorations() call and alive until
     * window destruction; deco_client_side caches the compositor's
     * last confirmed mode (its configure event) — when it answers
     * SERVER_SIDE against our CLIENT_SIDE request, FDK emits
     * FDK_EVENT_WINDOW_DECORATION so the window layer drops its band
     * instead of double-decorating. */
    int maximized;
    int minimized;
    int fullscreen;

    /* Last cursor shape APPLIED through window_set_cursor (1.1.0-rc1,
     * W2): the seat's pointer_enter re-applies exactly this shape on
     * every entry (with the enter serial) — Wayland cursor state does
     * not survive leaving a surface, and a client that answers no
     * wl_pointer.enter at all keeps whatever shape it had before
     * (the resize cursor "sticking" until the pointer left the bar).
     * 0 = default arrow; see FDK_PLATFORM_CURSOR_POINTER. */
    int cursor_shape;
    struct zxdg_toplevel_decoration_v1 *toplevel_decoration;
    int deco_client_side;
};

/* Registry helpers, implemented in wayland_registry.c. */
/* HiDPI output tracking (wayland_registry.c). track_output binds
 * listener state for a freshly bound wl_output (name = its registry
 * global, the retirement key); forget_output retires a record whose
 * global went away (scale 0 = "gone", proxy released);
 * destroy_outputs releases everything at disconnect. */
void fdk_wayland_track_output(fdk_platform_connection *conn,
                              struct wl_output *output, uint32_t name);
void fdk_wayland_forget_output(fdk_platform_connection *conn,
                               uint32_t name);
void fdk_wayland_destroy_outputs(fdk_platform_connection *conn);

fdk_result fdk_wayland_register_window(fdk_platform_connection *conn,
                                        fdk_platform_window *pwindow);
void fdk_wayland_unregister_window(fdk_platform_connection *conn,
                                    fdk_platform_window *pwindow);

/* Seat/input listener setup, implemented in wayland_seat.c, called
 * once compositor/shm/seat/wm_base are all bound (wayland_registry.c). */
void fdk_wayland_bind_seat_listeners(fdk_platform_connection *conn);
void fdk_wayland_teardown_seat(fdk_platform_connection *conn);

/* Key repeat (1.3.5, wayland_seat.c): fires one due repeat (returns
 * the count), called at every dispatch_pending exit; and the ops:
 * the pump's wakeup cap + the test seams (see platform_internal.h). */
int fdk_wayland_key_repeat_fire(fdk_platform_connection *conn);
int fdk_wayland_next_wakeup_ms(fdk_platform_connection *conn,
                               long long *out_ms_until);
int fdk_wayland_test_key(fdk_platform_connection *conn, uint32_t key,
                         int pressed);
void fdk_wayland_test_repeat_info(fdk_platform_connection *conn,
                                  int32_t rate, int32_t delay);
int fdk_wayland_test_default_keymap(fdk_platform_connection *conn);

/* Clipboard (wayland_clipboard.c). device_ready is idempotent: it
 * creates the wl_data_device once BOTH the manager global and the
 * seat exist (whichever binds second triggers it). */
void fdk_wayland_clipboard_device_ready(fdk_platform_connection *conn);
void fdk_wayland_clipboard_teardown(fdk_platform_connection *conn);

/* Drag and drop (wayland_dnd.c): the data_device drag events the
 * device listener in wayland_clipboard.c routes here, the teardown
 * (cancels an in-flight drag of ours), and the two ops entries. */
void fdk_wayland_dnd_device_enter(fdk_platform_connection *conn,
                                  uint32_t serial,
                                  struct wl_surface *surface,
                                  wl_fixed_t x, wl_fixed_t y,
                                  struct wl_data_offer *offer);
void fdk_wayland_dnd_device_motion(fdk_platform_connection *conn,
                                   uint32_t time, wl_fixed_t x,
                                   wl_fixed_t y);
void fdk_wayland_dnd_device_leave(fdk_platform_connection *conn);
void fdk_wayland_dnd_device_drop(fdk_platform_connection *conn);
void fdk_wayland_dnd_teardown(fdk_platform_connection *conn);
void fdk_wayland_window_set_drop_formats(fdk_platform_window *pwindow,
                                         int formats);
fdk_result fdk_wayland_drag_begin(fdk_platform_window *origin, int formats,
                                  const char *text,
                                  const char *const *uris, size_t uri_count,
                                  const fdk_u32 *icon, int icon_w,
                                  int icon_h,
                                  void (*on_done)(int, void *), void *user);
fdk_result fdk_wayland_clipboard_set_text(fdk_platform_connection *conn,
                                           const char *text);
char *fdk_wayland_clipboard_get_text(fdk_platform_connection *conn);

/* PRIMARY selection (1.3.4): the wp_primary_selection_unstable_v1
 * twins of the two clipboard ops. set is BEST-EFFORT by the public
 * contract (mirror of the X11 side: no verification, the classic
 * model re-owns per selection change). */
fdk_result fdk_wayland_clipboard_set_primary_text(
    fdk_platform_connection *conn, const char *text);
char *fdk_wayland_clipboard_get_primary_text(
    fdk_platform_connection *conn);
/* 1.4.9: the clipboard's image/png surface (data-device pipe
 * transfers — no size cap beyond the 32 MiB read bound). */
fdk_result fdk_wayland_clipboard_set_image(
    fdk_platform_connection *conn, const unsigned char *png,
    size_t len);
unsigned char *fdk_wayland_clipboard_get_image(
    fdk_platform_connection *conn, size_t *out_len);

/* Declared here, defined across wayland_connection.c, wayland_window.c
 * dispatch is wl_display_dispatch() itself (no separate translate step
 * the way X11 has — see wayland_dispatch.c's doc comment for why).
 * Assembled into the fdk_platform_ops vtable in wayland_ops.c. */
fdk_result fdk_wayland_connect(fdk_platform_dispatch_fn dispatch,
                                void *dispatch_user_data, const char *app_id,
                                fdk_platform_connection **out_conn);
void fdk_wayland_disconnect(fdk_platform_connection *conn);
int fdk_wayland_get_event_fd(fdk_platform_connection *conn);
int fdk_wayland_dispatch_pending(fdk_platform_connection *conn);

fdk_result fdk_wayland_window_create(fdk_platform_connection *conn,
                                      const fdk_window_options *options,
                                      fdk_platform_window *parent,
                                      fdk_platform_window **out_pwindow);
/* Extension-surface adoption (1.1.0) — wayland_ext.c. Wraps an
 * add-on's role-carrying wl_surface (committed NEVER yet — the
 * add-on's role handshake owns the first commit) in a registered
 * pwindow with the render-slot machinery. See
 * platform_internal.h's ext_surface_adopt contract. */
fdk_result fdk_wayland_ext_surface_adopt(fdk_platform_connection *conn,
                                         const fdk_ext_native_handle *native,
                                         fdk_platform_window **out_pwindow);
/* fdk_ext_get_native_display()'s op: the wl_display*, borrowed. */
void *fdk_wayland_native_display(fdk_platform_connection *conn);
/* fdk_ext_surface_native()'s op: the wrapped wl_surface. */
fdk_result fdk_wayland_ext_surface_native(fdk_platform_window *pwindow,
                                          fdk_ext_native_handle *out);
/* fdk_ext_surface_notify_configure()'s op: buffer-side resize +
 * configured-gate flip + deferred-first-frame commit (wayland_ext.c).
 * Runs AFTER the add-on acked its role's configure. */
void fdk_wayland_ext_surface_resized(fdk_platform_window *pwindow,
                                     fdk_i32 width, fdk_i32 height);
/* The deferred-first-frame commit shared by xdg_surface_configure()
 * and fdk_wayland_ext_surface_resized(): commits (post-ack) a buffer
 * whose present was deferred by the pre-configure gate, or dispatches
 * EXPOSE to re-drive a paint the configure's size invalidated.
 * Returns 1 when a frame was committed. Defined in wayland_window.c. */
int fdk__wayland_commit_deferred_first_frame(
    fdk_platform_window *pwindow);

/* Subsurfaces (1.1.0, D-2 / B-16) — wayland_ext.c. The FDK-owned
 * wl_subsurface child behind fdk_ext_subsurface_create: a fresh
 * wl_surface given the subsurface role against the parent's surface,
 * DESYNC by default (independent commits — the feature's point),
 * positioned in parent SURFACE-LOCAL (= FDK logical) coordinates,
 * `configured` from the start (no handshake exists to wait for).
 * See platform_internal.h's subsurface op block. */
fdk_result fdk_wayland_subsurface_create(
    fdk_platform_connection *conn, fdk_platform_window *parent,
    fdk_i32 x, fdk_i32 y, fdk_i32 width, fdk_i32 height,
    fdk_platform_window **out_pwindow);
void fdk_wayland_subsurface_set_position(fdk_platform_window *pwindow,
                                         fdk_i32 x, fdk_i32 y);
void fdk_wayland_subsurface_resize(fdk_platform_window *pwindow,
                                   fdk_i32 width, fdk_i32 height);
void fdk_wayland_subsurface_set_desync(fdk_platform_window *pwindow,
                                       int desync);
fdk_result fdk_wayland_subsurface_place(fdk_platform_window *pwindow,
                                        fdk_platform_window *sibling,
                                        int above);

/* Shared wl_surface/fractional-scale listeners (defined in
 * wayland_window.c, non-static since 1.1.0): the adoption path
 * attaches the same output-enter/leave and preferred-scale tracking
 * to extension surfaces that created windows get. */
extern const struct wl_surface_listener g_wl_surface_listener;
extern const struct wp_fractional_scale_v1_listener
    g_fractional_scale_listener;
void fdk_wayland_window_destroy(fdk_platform_window *pwindow);
void fdk_wayland_window_show(fdk_platform_window *pwindow);
void fdk_wayland_window_hide(fdk_platform_window *pwindow);
void fdk_wayland_window_set_title(fdk_platform_window *pwindow, const char *title);

/* Toplevel icon (1.1.0, M-4): xdg-toplevel-icon-v1 where bound, the
 * documented honest no-op where the compositor offers nothing. */
void fdk_wayland_window_set_icon(fdk_platform_window *pwindow,
                                 const fdk_u32 *pixels,
                                 int width, int height);

/* Touch test seam (1.1.0, M-3), the test_key discipline: drives the
 * REAL wl_touch listener bodies (sway headless exposes no touch
 * capability, so the rig cannot reach them through the protocol).
 * phase 0=down 1=motion 2=up 3=cancel; (x,y) logical surface-local,
 * ignored for up/cancel; `win` = the target surface's platform
 * window for the down phase. Returns 1 when a down was deliverable
 * (0 = dropped: unknown surface). */
/* Tablets / stylus (1.1.0, D-3) — wayland_tablet.c. Manager bind +
 * the seat-ready tablet seat attach + the shutdown sweep + the test
 * seam (the touch discipline: the listener bodies driven directly —
 * wlroots has no virtual-tablet protocol, so the rig cannot mint a
 * real tool). */
void fdk_wayland_tablet_seat_ready(fdk_platform_connection *conn);
void fdk_wayland_tablet_shutdown(fdk_platform_connection *conn);
/* Foreign toplevels (1.1.0, D-4) — wayland_toplevel.c. */
fdk_result fdk_wayland_toplevels_open(
    fdk_platform_connection *conn,
    fdk_platform_toplevels_changed_fn on_change, void *user,
    struct fdk_platform_toplevels **out);
void fdk_wayland_toplevels_close(struct fdk_platform_toplevels *t);
fdk_u32 fdk_wayland_toplevels_count(struct fdk_platform_toplevels *t);
const struct fdk_platform_toplevel *fdk_wayland_toplevels_at(
    struct fdk_platform_toplevels *t, fdk_u32 index);
fdk_result fdk_wayland_toplevels_activate(
    struct fdk_platform_toplevels *t, fdk_u32 index);
fdk_result fdk_wayland_toplevels_close_window(
    struct fdk_platform_toplevels *t, fdk_u32 index);
int fdk_wayland_test_tablet(fdk_platform_connection *conn,
                            fdk_platform_window *win, int phase,
                            uint32_t tool_type, double x, double y,
                            double pressure, double tilt_x,
                            double tilt_y, uint32_t button);

int fdk_wayland_test_touch(fdk_platform_connection *conn,
                           fdk_platform_window *win, int phase,
                           uint32_t id, double x, double y);

/* Cursor apply test seam (1.1.0-rc2, S-5; wayland_cursor.c): drives
 * fdk_wayland_cursor_apply for any op shape with the newest input
 * serial (1 when none arrived) and returns the cursor-shape-v1 enum
 * the set_shape request cited — the suite's per-shape mapping pin.
 * -1 = no cursor-shape manager / no seat pointer (honest skip). */
int fdk_wayland_test_cursor_apply(fdk_platform_connection *conn,
                                  fdk_platform_window *pwindow,
                                  int op_shape);

/* Input region (1.1.0-rc2, S-4 / A-06; wayland_window.c): the
 * wl_surface.set_input_region translation of the op. */
void fdk_wayland_window_set_input_region(fdk_platform_window *pwindow,
                                         const fdk_rect *rects,
                                         int count);

/* text-input-v3 (1.1.0, M-1; wayland_ime.c): called when BOTH the
 * manager and the seat exist (either binding order), and from the
 * surrounding push. teardown is part of disconnect;
 * window_destroyed is the destroy-path guard (a dying window's
 * latched batch must never apply — see wayland_ime.c). */
void fdk_wayland_ime_seat_ready(fdk_platform_connection *conn);
void fdk_wayland_ime_teardown(fdk_platform_connection *conn);
void fdk_wayland_ime_window_destroyed(fdk_platform_connection *conn,
                                      fdk_platform_window *pwindow);
void fdk_wayland_ime_update_surrounding(fdk_platform_window *pwindow,
                                        const char *text, int cursor);

/* Idle inhibition (1.1.0, S-3; wayland_window.c): the platform op —
 * create/destroy the per-surface zwp_idle_inhibitor_v1. */
fdk_result fdk_wayland_window_set_idle_inhibit(
    fdk_platform_window *pwindow, int inhibit);

/* xdg-activation (1.1.0, S-6; wayland_window.c): activate this
 * window through a compositor token, and mint a token for a
 * launcher to hand a child process. */
fdk_result fdk_wayland_window_activate(fdk_platform_window *pwindow);
fdk_result fdk_wayland_window_activation_token(
    fdk_platform_window *pwindow, char *out, size_t cap);
void fdk_wayland_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height);
void fdk_wayland_window_set_size_limits(fdk_platform_window *pwindow,
                                         fdk_size min_size, fdk_size max_size);

/* Phase 8 window management (see platform_internal.h for the vtable
 * contract of each): */
fdk_result fdk_wayland_window_set_wm_decorations(fdk_platform_window *pwindow,
                                                 bool on);
fdk_result fdk_wayland_window_set_fullscreen(
    fdk_platform_window *pwindow, bool fullscreen);
fdk_result fdk_wayland_window_set_maximized(fdk_platform_window *pwindow,
                                            bool maximized);
fdk_result fdk_wayland_window_set_minimized(fdk_platform_window *pwindow,
                                            bool minimized);
fdk_result fdk_wayland_window_begin_move(fdk_platform_window *pwindow,
                                         fdk_i32 local_x, fdk_i32 local_y);
fdk_result fdk_wayland_window_begin_resize(fdk_platform_window *pwindow,
                                           int edge, fdk_i32 local_x,
                                           fdk_i32 local_y);
/* The optional pointer-introspection op (1.1.4): the seat's cached
 * pointer_focus + surface-local position, valid whenever this window
 * holds pointer focus. Cursor SHAPING (window_set_cursor, 1.1.6) is
 * implemented in wayland_cursor.c — XCursor theme images uploaded
 * over wl_shm, attached to a shared cursor surface. */
int fdk_wayland_window_query_pointer(fdk_platform_window *pwindow,
                                     fdk_i32 *out_x, fdk_i32 *out_y);
void fdk_wayland_window_set_cursor(fdk_platform_window *pwindow, int edge);

/* Cursor machinery teardown (wayland_cursor.c): destroys the cursor
 * surface, every cached shape buffer, and the cache itself. Called
 * from fdk_wayland_disconnect(). */
/* Applies `shape` (the window_set_cursor op's space) citing `serial`
 * — cursor-shape-v1's set_shape when the manager/device exist, else
 * the XCursor-theme set_cursor path. wayland_seat.c calls this on
 * every wl_pointer.enter (the W2 fix); the op below calls it on
 * shape transitions. */
void fdk_wayland_cursor_apply(fdk_platform_connection *conn,
                              fdk_platform_window *pwindow,
                              uint32_t serial, int shape);

void fdk_wayland_cursor_teardown(fdk_platform_connection *conn);

/* Drains the dedicated wl_buffer release queue (wayland_dispatch.c
 * helper, also safe to call from inside a listener — it only ever
 * runs wl_buffer listeners). Returns the number of events dispatched,
 * or -1 on a connection error. */
int fdk_wayland_release_queue_dispatch(fdk_platform_connection *conn);

void fdk_wayland_window_update_state(fdk_platform_window *pwindow,
                                     int maximized, int minimized,
                                     int fullscreen);

/* Software rendering (fdk_surface machinery) — see the render_slots
 * comment in struct fdk_platform_window above for the recycling
 * lifecycle. */
/* GPU lane (1.2.0 / D-1) — see wayland_egl.h; the render_path op
 * and the shared downgrade helper live in wayland_window.c. */
int fdk_wayland_window_render_path(fdk_platform_window *pwindow);
fdk_result fdk__wayland_surface_downgrade_from_lane(
    fdk_platform_window *pwindow);

fdk_result fdk_wayland_window_get_framebuffer(fdk_platform_window *pwindow,
                                               fdk_platform_framebuffer *out_fb);

/* HiDPI (Phase 3 completion): recompute the window's scale from the
 * enter/leave output set (wayland_window.c), and the
 * fdk_window_get_scale() query op. */
fdk_result fdk_wayland_window_recompute_scale(
    fdk_platform_window *pwindow, struct wl_output *output, int entered);
fdk_result fdk_wayland_window_get_scale(fdk_platform_window *pwindow,
                                        fdk_f32 *out_scale);
fdk_result fdk_wayland_window_present(fdk_platform_window *pwindow,
                                      const fdk_platform_damage *damage);
int fdk_wayland_window_frame_ready(fdk_platform_window *pwindow);

/* The window_ever_presented ops entry: nonzero once a render buffer
 * has actually been COMMITTED to the compositor (the deferred-first-
 * frame regression observable — see wayland_window.c). */
int fdk_wayland_window_ever_presented(fdk_platform_window *pwindow);

#endif /* FDK_WAYLAND_PLATFORM_H */
