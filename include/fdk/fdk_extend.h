/*
 * fdk_extend.h — Faded Dream ToolKit extension API
 *
 * The surface add-on packages build on (1.0.0). FDK stays small and
 * stays the ROOT: the core and the fdk tools ship together as one
 * package, and everything else — panel layers, notification shells,
 * custom surface roles, input methods — is a SEPARATE package that
 * depends on FDK and uses exactly what this header exposes.
 * examples/19_extension_surfaces.c (the in-tree demo) is the
 * reference consumer of every function here.
 *
 * The model in one paragraph: an add-on REGISTERS a surface KIND
 * (a name plus a small table of callbacks). Creating a surface of
 * that kind calls the add-on's own create callback, which builds the
 * NATIVE surface itself — an X11 window through the Display* this
 * API hands out, or a wl_surface through the wl_display* plus
 * whatever protocol role the add-on speaks — and returns the native
 * handle. FDK then ADOPTS that surface: it joins the event routing
 * (the add-on receives the same backend-neutral fdk_event_data
 * stream applications get), it renders through the ordinary public
 * fdk_surface API, it can parent popups, and the add-on's own
 * file descriptors can join FDK's event loop. FDK never learns what
 * the role IS; the add-on never touches an FDK private struct.
 *
 * Ownership and lifetimes (the whole contract, stated once):
 *   - The ADD-ON owns its ROLE objects (the protocol objects it
 *     created on the native surface: a zwlr_layer_surface_v1, an
 *     X11 property set, anything). kind_ops.destroy() is where it
 *     tears them down; FDK calls it BEFORE destroying the native
 *     surface, so the role teardown runs against a live surface.
 *   - FDK owns the NATIVE SURFACE ITSELF once kind_ops.create()
 *     returns it (the X window is XDestroyWindow'd, the wl_surface
 *     is wl_surface_destroy'd, by FDK — never by the add-on). On
 *     adopt failure FDK still performs this destruction after
 *     calling kind_ops.destroy(), so create-failure cleanup is
 *     symmetric.
 *   - An fdk_ext_surface* handle is valid from a successful
 *     fdk_ext_surface_create()/fdk_ext_popup_create() until
 *     fdk_ext_surface_destroy() — or until the owning fdk_context
 *     is shut down (fdk_shutdown force-destroys survivors).
 *   - Kinds are add-on-owned objects: fdk_ext_kind_destroy() frees
 *     one. A kind with live surfaces refuses
 *     (FDK_ERR_INVALID_STATE) — destroy the surfaces first.
 *
 * Threading: exactly like the rest of FDK — everything here is
 * UI-thread-only (docs/threading.md). In particular, kind callbacks
 * run from inside FDK's dispatch/pump paths.
 *
 * Documentation: docs/EXTENDING.md (every function, with examples).
 */

#ifndef FDK_EXTEND_H
#define FDK_EXTEND_H

#include "fdk_error.h"
#include "fdk_event.h"
#include "fdk_types.h"
#include "fdk_version.h"
#include "fdk_widget.h" /* v3: the widget-hosting root (fdk_widget) */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- API version ---------------------------------------------------------
 *
 * The extension API carries its own version, independent of FDK's
 * package version: additions bump it, and NOTHING is ever removed
 * or renamed (the same append-only discipline as docs/abi-policy.md
 * applies to the core). An add-on pins the version it was written
 * against and refuses to run against an older FDK — the compile-time
 * and run-time pair:
 *
 *     #if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 2
 *     #error "this add-on needs FDK's extension API v2 (FDK 1.0.0)"
 *     #endif
 *     ...
 *     if (fdk_ext_api_version() < 2) { ...refuse to run... }
 *
 * A newer FDK keeps old add-ons working: the runtime number only
 * grows. Version history: v1 (FDK 1.0.0-rc1) — kinds, surfaces,
 * popups, fd sources; v2 (FDK 1.0.0, D-2) — subsurfaces: the
 * FDK-owned child-surface primitive and its position/stacking/mode
 * operations (see the Subsurfaces section below); v3 (FDK 1.0.0) —
 * widget hosting: fdk_ext_surface_host_widgets() puts the ordinary
 * widget catalog on any extension surface (see the Widget hosting
 * section below).
 */
#define FDK_EXT_API_VERSION 3

/* Returns the RUNTIME extension-API version of the loaded FDK
 * library (>= 1). Add-ons call this once at startup and compare
 * against the FDK_EXT_API_VERSION they were compiled with; a
 * library older than the header is a hard error for the add-on. */
int fdk_ext_api_version(void);

/* ---- Backend identity + capabilities ----------------------------------- */

/* Which display-server backend a context resolved to. The numbers
 * are stable ABI (append-only enum). */
typedef enum fdk_ext_backend {
    FDK_EXT_BACKEND_NONE    = 0, /* NULL ctx, or not connected    */
    FDK_EXT_BACKEND_X11     = 1, /* X11 / XLibre (Xlib protocol)  */
    FDK_EXT_BACKEND_WAYLAND = 2, /* Wayland                       */
} fdk_ext_backend;

/* Returns the backend a context ACTUALLY connected to — what
 * FDK_PLATFORM_AUTO resolved to, or the forced backend if the init
 * options named one. FDK_EXT_BACKEND_NONE for a NULL context or a
 * context with no live connection. */
fdk_ext_backend fdk_ext_active_backend(fdk_context *ctx);

/* Runtime capabilities, queried per context (a capability that
 * depends on the connected backend reports itself honestly — an
 * add-on checks instead of assuming, and degrades or refuses as it
 * sees fit). Append-only enum. */
typedef enum fdk_ext_capability {
    /* Extension surfaces can render through the public fdk_surface
     * path (framebuffer + damage + present). Both current backends;
     * a future GPU-only backend might not. */
    FDK_EXT_CAP_SHM_RENDER   = 1,
    /* Popups created on extension surfaces take a real input grab
     * with outside-click and Escape dismissal. X11: server-side
     * pointer+keyboard grab. Wayland: xdg_popup.grab (compositors
     * may refuse a grab whose serial predates all input — the
     * popup still shows, it just dismisses on focus loss instead;
     * same caveat every Wayland toolkit documents). */
    FDK_EXT_CAP_POPUP_GRAB   = 2,
    /* EWMH dock semantics are settable by an X11 add-on itself
     * (_NET_WM_WINDOW_TYPE_DOCK + struts): reported for the X11
     * backend. On Wayland, layering is the add-on's own protocol
     * negotiation — FDK cannot vouch for it, so
     * this capability reports false there and the add-on binds the
     * protocol itself. */
    FDK_EXT_CAP_EWMH_DOCK    = 3,
    /* Frame pacing through fdk_surface_frame_ready(): Wayland's
     * compositor frame callbacks; X11's round-10 paint pace (a 17 ms
     * present-interval floor plus SHM-slot backpressure —
     * self-imposed, not compositor feedback, but a real throttle
     * either way). */
    FDK_EXT_CAP_FRAME_PACING = 4,
    /* 1.0.0 render-mode capabilities (fdk_core.h's fdk_render_mode),
     * reporting the CONTEXT's ACTIVE mode honestly:
     *   RENDER_CPU    true unless the context runs GPU-only (the
     *                 software rasterizer exists in every mode; in GPU
     *                 mode its pixels ride the lane's texture upload).
     *   RENDER_HYBRID true only in HYBRID/AUTO contexts (the classic
     *                 framebuffer path with the optional lane).
     *   RENDER_GPU    true only when the lane is REQUIRED (GPU mode
     *                 actually active for this context's windows) —
     *                 never claimed merely because a driver exists. */
    FDK_EXT_CAP_RENDER_CPU    = 5,
    FDK_EXT_CAP_RENDER_HYBRID = 6,
    FDK_EXT_CAP_RENDER_GPU    = 7,
    /* 1.0.0: a FIRST-CLASS layer-shell role (fdk_ext_layer_surface_*,
     * below) — the compositor offers zwlr_layer_shell_v1 (or an
     * equivalent FDK binds) and FDK built the role for the add-on.
     * False on X11 and on Wayland compositors without the protocol
     * (the add-on may still bind its own layer shell through the
     * documented native accessors in fdk/ext/wayland.h). */
    FDK_EXT_CAP_LAYER_SHELL   = 8,
    /* 1.0.0: the GPU lane is loaded for this context's connection
     * (the EGL/GLX presentation lane actually initialized — hardware
     * GL, not a refused software renderer). The GL-programming recipe
     * for add-ons rides the existing native accessors: create your
     * EGLDisplay/GLX context on fdk_ext_get_native_display()'s
     * handle, your EGLSurface/wl_egl_window on
     * fdk_ext_surface_native()'s — the display handles are the SAME
     * connection FDK drives. Present arbitration (an add-on-owned
     * present path per surface) is the next increment; until then
     * FDK's present owns the surface's buffers. */
    FDK_EXT_CAP_GL            = 9,
    /* P3 first-class protocols: the cap says the platform + compositor
     * actually negotiated the protocol (the ops exist and the manager
     * global was bound); every API below answers FDK_ERR_UNSUPPORTED
     * when the matching cap is false. */
    FDK_EXT_CAP_IDLE_INHIBIT  = 10, /* zwp_idle_inhibit_manager_v1    */
    FDK_EXT_CAP_XDG_ACTIVATION = 11, /* xdg_activation_v1             */
    FDK_EXT_CAP_FOREIGN_TOPLEVEL = 12, /* zwlr_foreign_toplevel_v1 (the
                                        * fdk_toplevel.h list)       */
    FDK_EXT_CAP_TEXT_INPUT_V3 = 13, /* zwp_text_input_manager_v3 (the
                                     * IME path)                    */
    FDK_EXT_CAP_SESSION_LOCK = 14,  /* ext_session_lock_manager_v1   */
    FDK_EXT_CAP_INPUT_METHOD_V2 = 15, /* zwp_input_method_manager_v2 */
    FDK_EXT_CAP_VIRTUAL_KEYBOARD = 16, /* zwp_virtual_keyboard_mgr_v1 */
    FDK_EXT_CAP_WL_RAW_EVENTS = 17,  /* the add-on raw-event surface
                                       * (own event queues + raw binds) */
    FDK_EXT_CAP_X11_EVENT_FILTER = 18, /* the raw XEvent pre-dispatch
                                        * hook (core + XI2 events; the
                                        * OR-window path is the popup
                                        * API)                     */
    FDK_EXT_CAP_DMABUF_IMPORT = 19, /* zero-copy dma-buf texture import.
                                     * Wayland: the lane's EGL display
                                     * carries EGL_EXT_image_dma_buf_import
                                     * (+ _modifiers). X11: the mesa
                                     * unified-driver side door — a
                                     * platform-x11 EGL display for THIS
                                     * process's DISPLAY has the import
                                     * extensions AND the GLX context
                                     * binds the image through
                                     * GL_OES_EGL_image (checked at draw;
                                     * its absence degrades honestly per
                                     * draw). FALSE means only the
                                     * explicit CPU-mode linear-mmap
                                     * fallback serves imports — and
                                     * tiled/INVALID modifiers then
                                     * answer UNSUPPORTED at draw. */
} fdk_ext_capability;

/* Returns nonzero when `ctx`'s connected backend has `cap`.
 * False for a NULL context or an unknown capability value. */
int fdk_ext_has_capability(fdk_context *ctx, fdk_ext_capability cap);

/* ---- Documented native access -------------------------------------------
 *
 * The extension API's deliberate bridge to the display system: the
 * native connection handle, and per-surface native handles, handed
 * out as documented void-pointer / XID values with a tag saying
 * which world
 * they belong to. No Xlib or wayland-client TYPE appears in any FDK
 * public header (the "no backend leakage" rule of
 * docs/architecture.md); the add-on includes the native headers it
 * needs and casts to the documented type:
 *
 *   X11:      fdk_ext_native_display.display -> Display*   (Xlib)
 *             fdk_ext_native_handle.xid      -> Window
 *   Wayland:  fdk_ext_native_display.display -> struct wl_display *
 *             fdk_ext_native_handle.pointer  -> struct wl_surface *
 *
 * Lifetime: the native display lives as long as the context
 * (until fdk_shutdown); the native surface handle is valid while its
 * fdk_ext_surface is alive. An add-on speaks its own protocol over
 * these — creating its own wl_registry, setting its own X11
 * properties — and FDK keeps dispatching the connection either way.
 */

typedef struct fdk_ext_native_display {
    fdk_ext_backend backend; /* which member is valid           */
    void *display;           /* Display* / struct wl_display *  */
} fdk_ext_native_display;

typedef struct fdk_ext_native_handle {
    fdk_ext_backend backend; /* which union member is valid     */
    union {
        void *pointer;       /* Wayland: a wl_proxy-derived obj */
        fdk_u64 xid;         /* X11: an X resource ID (Window)  */
    };
} fdk_ext_native_handle;

/* Writes the context's native display connection to *out.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx or out is NULL
 *   FDK_ERR_NOT_INITIALIZED  - ctx has no live platform connection
 */
fdk_result fdk_ext_get_native_display(fdk_context *ctx,
                                      fdk_ext_native_display *out);

/* ---- Surface kinds ------------------------------------------------------
 *
 * A KIND is the add-on's surface class: a name (diagnostics; unique
 * per context), a table of callbacks, and a user pointer FDK hands
 * back with every callback. The callbacks run on FDK's dispatch
 * thread, inside FDK calls — they must not pump events themselves.
 */

typedef struct fdk_ext_surface fdk_ext_surface;
typedef struct fdk_ext_surface_kind fdk_ext_surface_kind;

/* The callback table. `struct_size` MUST be set to
 * sizeof(fdk_ext_surface_ops) by the add-on (zero-initialize the
 * struct first, fill struct_size, then only the members that exist
 * in the FDK version you build against — FDK reads exactly
 * struct_size bytes, so older binaries against newer FDK keep
 * working as fields are APPENDED, never reordered).
 *
 * Which callbacks are required is documented per call site:
 *   - create:     required (fdk_ext_surface_create fails without it)
 *   - destroy:    optional (a role-less surface needs none)
 *   - popup_attached: optional (only Wayland roles that parent
 *                 xdg_popups to themselves — e.g. a layer surface —
 *                 need it; X11 popups parent through the X tree
 *                 automatically)
 */
typedef struct fdk_ext_surface_ops {
    size_t struct_size;

    /* Builds the native surface AND its role. Inside it the add-on
     * calls fdk_ext_surface_context() to reach the fdk_context,
     * fdk_ext_get_native_display() for the connection, does its own
     * protocol work (XCreateWindow + properties; wl_compositor's
     * create_surface + its role binding), and writes the native
     * handle to *out_native. On failure returns a negative fdk_result
     * and leaves *out_native untouched (nothing was created, or the
     * add-on cleaned up after itself).
     *
     * OWNERSHIP TRANSFER: a returned native surface belongs to FDK
     * from this moment (see the header's lifetime block). On Wayland
     * do NOT commit the surface here — a role's initial configure
     * handshake comes first; commit ordering is the add-on's to
     * obey, and FDK's present path commits only when the add-on
     * renders.
     * On X11 the window should be created UNMAPPED (FDK maps it on
     * fdk_ext_surface_show; the add-on's properties must all be set
     * before that). FDK installs its own event mask on the window
     * at adopt (the standard FDK input set) — the add-on's
     * Expose/StructureNotify/input all flow through the normal
     * fdk_event_data stream afterwards. */
    fdk_result (*create)(fdk_ext_surface *surface,
                         fdk_ext_native_handle *out_native, void *user);

    /* Role teardown, called by fdk_ext_surface_destroy() BEFORE FDK
     * destroys the native surface (the role objects still have a
     * live surface to detach from), and by every other destroy path
     * (parent sweep, fdk_shutdown's leaked-surface sweep). MUST NOT
     * destroy the native surface itself — FDK does that next — and
     * must not destroy OTHER surfaces (their own teardown owns
     * them). May be NULL (nothing to tear down). */
    void (*destroy)(fdk_ext_surface *surface, void *user);

    /* Wayland only: FDK created an xdg_popup parented to this
     * surface's wl_surface and hands the popup proxy over so the
     * role can adopt it (the shell-surface pattern: the role's
     * own get_popup request adopts the xdg_popup). `native_popup`
     * is the struct xdg_popup* — cast it in the add-on, which is
     * the only place wayland-client types may appear. May be NULL
     * (the add-on's role is not a popup parent; Wayland popups on
     * such surfaces then have no role parent and rely on the grab
     * alone). X11 never calls this (X parents popups through the X
     * window tree). */
    void (*popup_attached)(fdk_ext_surface *surface, void *native_popup,
                           void *user);
} fdk_ext_surface_ops;

/* Registers a surface kind on `ctx`. The ops table is COPIED
 * (struct_size bytes); `user` is handed back verbatim to every
 * callback. Kind names are unique per context — a duplicate name
 * fails with FDK_ERR_INVALID_ARGUMENT (a re-registration after a
 * kind was destroyed is legal).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx/name/ops/out_kind NULL, name
 *                              empty, ops->struct_size too small
 *                              or create is NULL, duplicate name
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_ext_register_kind(fdk_context *ctx, const char *name,
                                 const fdk_ext_surface_ops *ops, void *user,
                                 fdk_ext_surface_kind **out_kind);

/* Frees a kind (unregisters it). Refuses while surfaces of the kind
 * are still alive:
 *   FDK_ERR_INVALID_STATE - live surfaces; destroy them first
 * NULL is a safe no-op (FDK_OK). The kind dies with the context
 * anyway (shutdown sweeps its surfaces first, then drops the
 * registry), but explicit destruction is the leak-free pattern. */
fdk_result fdk_ext_kind_destroy(fdk_ext_surface_kind *kind);

/* ---- Extension surfaces -------------------------------------------------- */

/* The extension-surface event callback: the SAME backend-neutral
 * fdk_event_data stream applications receive on windows —
 * configure, expose, focus, pointer, keys, close requests — with
 * the extension surface in place of the window. Configure events
 * deserve a note: on X11 they arrive from the X server
 * (ConfigureNotify); on Wayland FDK emits one when the ADD-ON
 * reports the role's assigned size via
 * fdk_ext_surface_notify_configure(). */
typedef void (*fdk_ext_event_fn)(fdk_ext_surface *surface,
                                 const fdk_event_data *event, void *user);

/* Creates a surface of `kind`. The kind's create callback runs
 * FIRST (it builds the native surface + role); FDK adopts the
 * returned native handle, wraps it, registers it for event routing,
 * and writes the surface to *out_surface. The surface starts
 * hidden: call fdk_ext_surface_show() when the role is ready to
 * appear (X11 maps the window; on Wayland the surface becomes
 * visible when the role's protocol and the add-on's first present
 * say so — show() is a no-op there, documented below).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - kind/out_surface NULL
 *   FDK_ERR_NOT_INITIALIZED  - kind's context has no connection
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_WINDOW_CREATE    - the backend refused the adoption
 *   (plus whatever the kind's create callback returns on its own
 *   failure paths)
 */
fdk_result fdk_ext_surface_create(fdk_ext_surface_kind *kind,
                                  fdk_ext_surface **out_surface);

/* Destroys the surface: the kind's destroy callback runs first
 * (role teardown against the still-live native surface), then FDK
 * destroys the native surface and frees the wrapper. NULL is a
 * safe no-op. Destroying a surface that has live popups sweeps
 * them first (the popup-family contract windows follow). */
void fdk_ext_surface_destroy(fdk_ext_surface *surface);

/* The owning context (never NULL for a live surface). */
fdk_context *fdk_ext_surface_context(fdk_ext_surface *surface);

/* The surface's native handle, by FDK's documented tagging:
 * X11 -> xid (the Window), Wayland -> pointer (the wl_surface*).
 * Valid while the surface lives.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or out is NULL
 */
fdk_result fdk_ext_surface_native(fdk_ext_surface *surface,
                                  fdk_ext_native_handle *out);

/* The surface's RENDER TARGET — the public fdk_surface API
 * (fdk_surface_get_info / present / invalidate / the drawing
 * helpers / frame_ready) is how an add-on draws: same objects,
 * same lifetimes as an application's window surface (the surface
 * is owned by the extension surface; never pass it to
 * fdk_surface_destroy).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or out is NULL
 *   FDK_ERR_NOT_INITIALIZED  - the context's connection is gone
 *   FDK_ERR_OUT_OF_MEMORY    - first-acquisition allocation
 */
fdk_result fdk_ext_surface_get_surface(fdk_ext_surface *surface,
                                       fdk_surface **out);

/* Sets the surface's hover cursor shape (1.0.0-rc1). The shape is
 * re-applied automatically on every pointer entry (Wayland's cursor
 * state does not survive leaving a surface); the fdk_cursor_shape
 * enum lives in fdk_window.h. Add-ons rendering panels typically set
 * FDK_CURSOR_DEFAULT once at creation and FDK_CURSOR_POINTER while
 * the pointer hovers a clickable region. No-op on an unknown shape.
 * Available since extension API v1 (append-only addition). */
void fdk_ext_surface_set_cursor(fdk_ext_surface *surface,
                                int shape);

/* Engages or releases the session's idle inhibitor FOR THIS SURFACE
 * (extension API v3, append-only): the same
 * zwp_idle_inhibit_manager_v1 object the window API drives, on the
 * ext surface's native surface — a panel or lock screen can hold the
 * screen awake exactly like a window. Answers FDK_ERR_UNSUPPORTED
 * where the manager is missing (the D-Bus fallback is the caller's
 * to arrange — the same contract as fdk_window_set_idle_inhibited;
 * the capability to probe is FDK_EXT_CAP_IDLE_INHIBIT). Idempotent
 * both ways. */
fdk_result fdk_ext_surface_set_idle_inhibited(fdk_ext_surface *surface,
                                              int inhibit);

/* Sets the surface's INPUT region (1.0.0-rc2, S-4): `count`
 * window-local logical rectangles inside which the surface receives
 * pointer input; input outside them falls through to whatever is
 * beneath. rects may be NULL only when count is 0, which clears the
 * region (full input). Semantics and limits as FDK's
 * fdk_window_set_input_region (fdk_window.h: FDK_INPUT_REGION_MAX,
 * the error codes, the zero-area = empty region rule). Available
 * since extension API v1 (append-only addition). */
fdk_result fdk_ext_surface_set_input_region(fdk_ext_surface *surface,
                                            const fdk_rect *rects,
                                            fdk_i32 count);

/* Makes the surface visible. X11: maps the window. Wayland: a
 * documented no-op — a role-carrying surface maps when its own
 * protocol handshake and first buffer commit complete, both of
 * which the ADD-ON drives (report the role's configure size with
 * fdk_ext_surface_notify_configure(), then render + present). */
void fdk_ext_surface_show(fdk_ext_surface *surface);

/* Hides the surface (X11: unmaps; Wayland: no-op, symmetrical with
 * show()). The surface and its role stay alive for re-showing. */
void fdk_ext_surface_hide(fdk_ext_surface *surface);

/* The surface's last-known size (from the most recent configure —
 * X11's server events or the add-on's notify_configure reports).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or out_size is NULL
 */
fdk_result fdk_ext_surface_get_size(const fdk_ext_surface *surface,
                                    fdk_size *out_size);

/* Reports a compositor-assigned size for the surface. WAYLAND
 * add-ons call this from their role's configure callback (e.g.
 * zwlr_layer_surface_v1::configure) after acking it: FDK records
 * the size, resizes its render machinery, and delivers
 * FDK_EVENT_WINDOW_CONFIGURE to the event callback — after which
 * the add-on redraws through the fdk_surface path and the next
 * present carries the new size. X11 add-ons never call this (the
 * server's ConfigureNotify arrives on its own).
 *
 * THE PRESENT GATE (1.0.0-rc1): on Wayland, presents before this
 * call are DEFERRED, not committed — no wl_surface.attach reaches
 * the wire until the role's configure has been reported (and, by
 * the documented contract above, acked). The first notified
 * configure commits the deferred first frame post-ack; a configure
 * that changes the size re-drives the paint via EXPOSE instead.
 * The role rule "never commit a buffer before the first
 * configure" is therefore enforced BY FDK: an add-on that paints
 * eagerly (immediately after show(), the way every example does)
 * cannot trip the compositor's protocol error (found live on labwc
 * and sway: "zwlr_layer_surface_v1: error 2: layer_surface has
 * never been configured" took down the whole connection).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface NULL, or w/h <= 0
 */
fdk_result fdk_ext_surface_notify_configure(fdk_ext_surface *surface,
                                            fdk_i32 width, fdk_i32 height);

/* Sets the callback receiving the surface's event stream (replaces
 * any previous one; NULL removes it). `user` rides along. */
void fdk_ext_surface_set_event_callback(fdk_ext_surface *surface,
                                        fdk_ext_event_fn callback,
                                        void *user);

/* ---- Widget hosting (extension API v3, FDK 1.0.0) ------------------------
 *
 * The whole widget catalog — labels, entries, buttons, boxes, the
 * choosers, everything in fdk_widgets.h — on an extension surface.
 * This is the "text on extension surfaces" gap closed: a label is
 * one fdk_label_create away on a shell-surface bar, a notification
 * card, a dock.
 *
 * fdk_ext_surface_host_widgets() creates the surface's WINDOW-OWNED
 * widget root (exactly the root fdk_window_get_root hands out for
 * application windows) and turns on the toolkit-owned repaint: from
 * then on the surface behaves like a toolkit dialog's window —
 *   - configure events resize and re-arrange the tree (X11's real
 *     ConfigureNotify; Wayland's fdk_ext_surface_notify_configure
 *     reports — the root follows both automatically),
 *   - pointer, keyboard, and scroll events dispatch into the tree
 *     (hit-testing, implicit grab, focus — the standard widget
 *     semantics, child-local coordinates),
 *   - damaged regions repaint through the ordinary paint walk and
 *     present at the frame pacing every toolkit window gets
 *     (fdk_surface_frame_ready's gate — hover motion cannot
 *     out-commit the compositor).
 *
 * The contract, stated once:
 *   - The tree OWNS the surface's pixels while hosted: every frame
 *     the root's background fills first (the theme window-background
 *     token by default; fdk_widget_set_background on the root
 *     overrides it), then the widgets draw. Draw custom chrome with
 *     widget primitives (a Canvas, styled widgets), not by writing
 *     the fdk_surface directly — the same posture every auto-paint
 *     window has. Mixing direct present() calls with a hosted tree
 *     is an application race, not a supported mode.
 *   - The add-on's OWN event callback (set_event_callback) keeps
 *     running, under the same rule every application window has
 *     (the documented fdk_window_get_root contract): window-level
 *     events (configure, expose, close) always reach it; INPUT
 *     events a widget of the tree consumed do NOT — a button press
 *     belongs to the button. Events no widget handled still arrive.
 *   - The root is window-owned: destroyed with the surface
 *     (fdk_widget_destroy refuses it, the FDK_WF_WINDOW_ROOT rule);
 *     build the tree into it with the ordinary create calls.
 *   - Keyboard focus follows the surface's own focus exactly like
 *     the raw event stream does — on Wayland a layer surface needs
 *     its keyboard-interactivity set by the add-on's protocol
 *     negotiation before entries and text views can take focus.
 *   - Hosting twice returns the SAME root (idempotent, like
 *     fdk_window_get_root); a NULL surface/out_root fails
 *     FDK_ERR_INVALID_ARGUMENT; OOM propagates.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - surface or out_root is NULL
 *   FDK_ERR_OUT_OF_MEMORY    - the root's first allocation
 */
fdk_result fdk_ext_surface_host_widgets(fdk_ext_surface *surface,
                                        fdk_widget **out_root);

/* ---- Popups parented to extension surfaces -------------------------------
 *
 * The dropdown/menu primitive for add-on surfaces: parent-relative
 * positioning, no WM/compositor chrome, an input grab while shown,
 * and dismissal by outside click or Escape — delivered as
 * FDK_EVENT_WINDOW_CLOSE_REQUEST on the POPUP's event callback (the
 * same contract fdk_window_create_popup gives applications).
 * Keyboard focus: the grab routes keys to the popup (X11:
 * pointer+keyboard grab; Wayland: xdg_popup.grab).
 *
 * The popup is itself an extension surface (render through
 * fdk_ext_surface_get_surface, receive events through
 * fdk_ext_surface_set_event_callback) — with the one difference
 * that FDK owns its whole lifetime (no kind callbacks fire for it;
 * destroy it with fdk_ext_surface_destroy).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - parent NULL or out_surface NULL
 *   FDK_ERR_NOT_INITIALIZED  - parent's context has no connection
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_WINDOW_CREATE    - the platform refused the popup
 */
fdk_result fdk_ext_popup_create(fdk_ext_surface *parent, fdk_i32 x,
                                fdk_i32 y, fdk_i32 width, fdk_i32 height,
                                fdk_ext_surface **out_surface);

/* ---- Subsurfaces (extension API v2, FDK 1.0.0 D-2) -----------------------
 *
 * An FDK-OWNED CHILD SURFACE of an existing window or extension
 * surface: a rectangular region of the parent that renders, moves,
 * and receives pointer events on its own — the video-in-UI and
 * offscreen-overlay primitive GTK4 calls a subsurface and X11 calls
 * a child window. Unlike a kind surface, no add-on callback builds
 * the native object: FDK creates it (X11: a real child Window of the
 * parent, clipped to it and stacked within it; Wayland: a
 * wl_subsurface role on a fresh wl_surface), and the returned
 * handle behaves like any other extension surface — render through
 * fdk_ext_surface_get_surface, events through
 * fdk_ext_surface_set_event_callback, destroy with
 * fdk_ext_surface_destroy.
 *
 * What lands where, per backend, by construction:
 *   - Rendering: the ordinary fdk_surface path on both (identical
 *     pixels; the region composites OVER the parent's own pixels —
 *     a child can never render UNDER the parent's content on X11,
 *     and the API exposes only that same top-of-parent stacking).
 *   - Pointer events: they route to the child wherever it is the
 *     topmost surface (both backends); coordinates are child-local,
 *     exactly like a window's.
 *   - Keyboard events: they follow the PARENT's focus (the
 *     toplevel on Wayland; the focused top-level on X11) — a child
 *     is an input region, not a focus target. Deliver keys from the
 *     parent's callback.
 *   - Sizing and position: the CLIENT owns both (there is no
 *     configure handshake — nothing assigns a subsurface its size).
 *     Set them at create, then fdk_ext_subsurface_set_position /
 *     fdk_ext_subsurface_resize any time.
 *
 * The commit-mode contract (the one real Wayland/X11 delta): a
 * Wayland subsurface's BUFFER updates either ride the PARENT's next
 * commit (sync mode — the protocol default) or apply on the child's
 * own presents (desync). FDK creates subsurfaces in DESYNC mode —
 * an independently-updating region is the feature's point, and a
 * sync-mode child of a quiet parent would never show a frame.
 * fdk_ext_subsurface_set_desync() switches either way when a parent
 * -locked update discipline is wanted. X11 child windows update on
 * their own always (there is no commit transaction); the call is a
 * harmless no-op there and returns FDK_OK — a mode hint, never an
 * error, the same contract fdk_ext_surface_set_cursor carries.
 *
 * One Wayland protocol truth worth knowing (it bit the first
 * implementation, found live against sway): a subsurface's POSITION,
 * stacking, and very presence in the parent's surface tree are
 * PARENT-managed state — applied only when the parent's state is
 * applied, whatever the child's mode. FDK hides this: every
 * create/set_position/place call also issues an EMPTY commit on the
 * topmost configured ancestor (an attach-less commit — pure state
 * application, invisible to the app's frame pacing), so the move
 * takes effect immediately exactly like X11's. An ancestor still in
 * its role handshake (an unconfigured layer surface) is left alone —
 * its own handshake commit carries the child in.
 *
 * Popups ON a subsurface: X11 parents them naturally (any window
 * can anchor a popup); Wayland REFUSES (FDK_ERR_UNSUPPORTED) — the
 * xdg_popup protocol requires a toplevel/popup/role parent, and a
 * wl_subsurface is none of those. Anchor menus on the parent
 * window or a kind surface instead; the failure is loud and early.
 *
 * Lifetime: a subsurface must not outlive its parent — destroying
 * the parent sweeps its subsurfaces first (the popup contract,
 * verbatim). After that sweep the child handle is dangling; keep
 * the same discipline popups demand (destroy children first when
 * you want their destroy callbacks to run).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - parent/out NULL, or width/height <= 0
 *   FDK_ERR_NOT_INITIALIZED  - parent's context has no connection
 *   FDK_ERR_OUT_OF_MEMORY
 *   FDK_ERR_UNSUPPORTED      - backend lacks the op, or the Wayland
 *                              compositor offers no wl_subcompositor
 *   FDK_ERR_WINDOW_CREATE    - the platform refused the child
 */
fdk_result fdk_ext_subsurface_create(fdk_window *parent, fdk_i32 x,
                                     fdk_i32 y, fdk_i32 width,
                                     fdk_i32 height,
                                     fdk_ext_surface **out_surface);

/* The same primitive parented to an EXTENSION surface (an add-on's
 * role surface — a shell-surface bar carrying an inset video region,
 * an overlay on a notification card). Both kind surfaces and FDK-
 * owned ones (popups, other subsurfaces — nesting is legal on both
 * backends: the protocol nests Wayland subsurfaces freely, and X11
 * child windows compose naturally) may parent; the parent sweep is
 * depth-first through the same child lists. Identical failure codes
 * to fdk_ext_subsurface_create.
 */
fdk_result fdk_ext_subsurface_create_ex(fdk_ext_surface *parent,
                                        fdk_i32 x, fdk_i32 y,
                                        fdk_i32 width, fdk_i32 height,
                                        fdk_ext_surface **out_surface);

/* Moves the child to (x, y) — parent-relative LOGICAL coordinates
 * (the same space the create call used; negative values are legal,
 * the region hangs off the parent's edge). IMMEDIATE on both
 * backends: X11 moves the child window now; Wayland applies the
 * protocol's parent-managed state by issuing the applying commit
 * itself (see the section note above — the position never waits for
 * the parent's next present). FDK_ERR_INVALID_ARGUMENT on a
 * NULL/non-subsurface surface, FDK_ERR_UNSUPPORTED with no backend
 * op. */
fdk_result fdk_ext_subsurface_set_position(fdk_ext_surface *surface,
                                           fdk_i32 x, fdk_i32 y);

/* Resizes the child (positive w/h required). X11 resizes the child
 * window now and the next framebuffer acquisition follows; Wayland
 * records the size and the next present commits a buffer of it. An
 * FDK_EVENT_WINDOW_CONFIGURE is dispatched to the child's callback
 * on both (the repaint obligation the event carries applies).
 * Failure codes as above. */
fdk_result fdk_ext_subsurface_resize(fdk_ext_surface *surface,
                                     fdk_i32 width, fdk_i32 height);

/* Selects the child's commit mode: nonzero = desync (independent
 * updates; the create-time default), 0 = sync (updates apply with
 * the parent's next commit). X11: documented no-op, FDK_OK. */
fdk_result fdk_ext_subsurface_set_desync(fdk_ext_surface *surface,
                                         int desync);

/* Restacks the child immediately ABOVE the sibling — both must be
 * subsurfaces of the SAME parent (mixed parents fail
 * FDK_ERR_INVALID_ARGUMENT; a non-subsurface argument fails
 * FDK_ERR_INVALID_STATE). IMMEDIATE on both backends (Wayland: the
 * applying ancestor commit, as set_position). Relative-to-PARENT
 * stacking is deliberately not exposed: an X11 child always renders
 * over the parent's own pixels, and the API keeps one cross-backend
 * truth instead of a Wayland-only under-parent mode.
 * FDK_ERR_UNSUPPORTED with no backend op. */
fdk_result fdk_ext_subsurface_place_above(fdk_ext_surface *surface,
                                          fdk_ext_surface *sibling);

/* The mirror: BELOW the sibling. Same validation, same failures. */
fdk_result fdk_ext_subsurface_place_below(fdk_ext_surface *surface,
                                          fdk_ext_surface *sibling);

/* ---- Layer-shell surfaces (extension API v3, 1.0.0) -----------------------
 *
 * The panel/dock/notification-layer role as a FIRST-CLASS FDK
 * surface: FDK binds zwlr_layer_shell_v1 when the compositor offers
 * it, builds the zwlr_layer_surface_v1 role for the add-on, bridges
 * the role's configure handshake into the standard extension-surface
 * flow (the present gate applies — notify_configure drives it), and
 * the add-on renders/events exactly like any other extension surface
 * (get_surface + set_event_callback + popups + widget hosting all
 * work). The add-on NEVER touches the protocol objects; FDK owns the
 * role's lifetime (destroyed with the surface, before the wl_surface).
 *
 * Compositors without the protocol: the capability reports false and
 * creation answers FDK_ERR_UNSUPPORTED — the honest refusal, never a
 * faked xdg_toplevel with dock hints. An add-on that wants to speak
 * the protocol ITSELF (its own bind, its own listeners) uses the
 * native accessors in fdk/ext/wayland.h instead.
 */

/* zwlr_layer_shell_v1's layer values (numeric identity matches the
 * protocol; append-only). */
enum {
    FDK_LAYER_BACKGROUND = 0,
    FDK_LAYER_BOTTOM     = 1,
    FDK_LAYER_TOP        = 2,
    FDK_LAYER_OVERLAY    = 3,
};

/* Anchor bits (zwlr_layer_surface_v1.set_anchor's enum). */
enum {
    FDK_LAYER_ANCHOR_TOP    = 1,
    FDK_LAYER_ANCHOR_BOTTOM = 2,
    FDK_LAYER_ANCHOR_LEFT   = 4,
    FDK_LAYER_ANCHOR_RIGHT  = 8,
};

/* Keyboard interactivity (zwlr_layer_surface_v1.set_keyboard_
 * interactivity's enum). */
enum {
    FDK_LAYER_KB_NONE       = 0,
    FDK_LAYER_KB_EXCLUSIVE  = 1,
    FDK_LAYER_KB_ON_DEMAND  = 2,
};

typedef struct fdk_ext_layer_options {
    size_t struct_size;      /* sizeof(fdk_ext_layer_options); FDK reads
                              * exactly this many bytes (append-only) */

    int layer;               /* FDK_LAYER_*; default TOP            */
    fdk_u32 anchor;          /* FDK_LAYER_ANCHOR_* bits; default 0
                              * (compositor centers)               */
    fdk_i32 exclusive_zone;  /* -1 = exclusive to the anchored edge's
                              * full size; >=0 the reserve; default 0 */
    fdk_i32 margin_top, margin_bottom, margin_left, margin_right;
    int keyboard;            /* FDK_LAYER_KB_*; default NONE        */
    const char *namespace_;  /* role namespace (compositor logs);
                              * NULL = "fdk"                        */
} fdk_ext_layer_options;

/* Creates a layer surface. The role's configure events arrive as
 * FDK_EVENT_WINDOW_CONFIGURE on the surface's event callback (the
 * add-on acks implicitly by rendering — the present gate owns the
 * protocol discipline, exactly like add-on-driven roles).
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx/out NULL, or options->struct_size
 *                              smaller than this header's struct
 *   FDK_ERR_NOT_INITIALIZED  - no live connection
 *   FDK_ERR_UNSUPPORTED      - the compositor offers no layer shell
 *                              (check FDK_EXT_CAP_LAYER_SHELL first)
 *   FDK_ERR_OUT_OF_MEMORY
 */
fdk_result fdk_ext_layer_surface_create(fdk_context *ctx,
                                        const fdk_ext_layer_options *options,
                                        fdk_ext_surface **out_surface);

/* Post-create adjustments — legal any time before the FIRST configure
 * is reported (the role state applies with the initial handshake);
 * after that they answer FDK_ERR_INVALID_STATE (a re-map of layer
 * state is the compositor's move, not the client's — destroy and
 * recreate). All take the surface; wrong-kind surfaces answer
 * FDK_ERR_INVALID_ARGUMENT. */
fdk_result fdk_ext_layer_surface_set_layer(fdk_ext_surface *surface,
                                           int layer);
fdk_result fdk_ext_layer_surface_set_anchor(fdk_ext_surface *surface,
                                            fdk_u32 anchor);
fdk_result fdk_ext_layer_surface_set_exclusive_zone(fdk_ext_surface *surface,
                                                    fdk_i32 zone);
/* The client-chosen dimensions (0 = the anchored dimension is the
 * compositor's). Applies with the role state; call before the first
 * configure like the other adjustments. */
fdk_result fdk_ext_layer_surface_set_size(fdk_ext_surface *surface,
                                          fdk_i32 width, fdk_i32 height);
fdk_result fdk_ext_layer_surface_set_margins(fdk_ext_surface *surface,
                                             fdk_i32 top, fdk_i32 bottom,
                                             fdk_i32 left, fdk_i32 right);
fdk_result fdk_ext_layer_surface_set_keyboard(fdk_ext_surface *surface,
                                              int mode);

/* ---- Nested / modal loops (extension API v3, 1.0.0) -----------------------
 *
 * A supported re-entrant run level: fdk_ext_modal_run() pumps events
 * (fdk_pump_events(-1), the same primitive fdk_run wraps) until THIS
 * level is ended with fdk_ext_modal_quit() — or fdk_quit() ends the
 * whole application, or the connection dies. This is how a menu, a
 * combo dropdown, a drag, or a custom dialog runs "until done"
 * without hand-rolling a pump loop, and how an add-on hosts a popup
 * that must keep dispatching while the caller's stack is still
 * unfolded.
 *
 * The contract:
 *   - Levels NEST: each fdk_ext_modal_run() is its own level;
 *     fdk_ext_modal_quit() ends the INNERMOST running level and it
 *     alone (the flag is consumed by the loop that reads it).
 *   - UNLIKE fdk_run(), a modal level does NOT exit when the last
 *     top-level window closes — a menu host keeps dispatching until
 *     explicitly quit (fdk_run's no-windows rule is the application
 *     loop's, not a modal level's).
 *   - Legal from inside callbacks (a widget handler may run a modal
 *     level), from fdk_post callbacks, and from fd-source callbacks —
 *     anything already on the UI thread. NEVER from a worker thread:
 *     only the UI thread may pump (docs/threading.md). A worker that
 *     needs the loop unblocked uses fdk_ext_post to arrange it.
 *   - fdk_ext_modal_quit() is legal from ANY thread: it sets the
     * innermost level's quit flag and nudges the pump's wake fd, so
     * a worker can end a UI-thread modal loop promptly.
 *   - Timers, posts, fd sources, and modal levels compose: a modal
 *     level dispatches everything fdk_pump_events dispatches.
 *
 * Can fail with:
 *   FDK_ERR_INVALID_ARGUMENT - ctx is NULL
 *   FDK_ERR_NOT_INITIALIZED  - no live platform connection (or
 *                              fdk_shutdown already ran)
 *   (a pump-level fatal propagates as its negative result)
 */
fdk_result fdk_ext_modal_run(fdk_context *ctx);

/* Ends the innermost running modal level. Safe from any thread; a
 * no-op when no level is running. */
void fdk_ext_modal_quit(fdk_context *ctx);

/* The number of currently running modal levels (0 = none; a
 * diagnostics helper — NULL ctx is 0). */
int fdk_ext_modal_depth(const fdk_context *ctx);

/* ---- Event-loop file-descriptor sources ----------------------------------
 *
 * The pump's poll set, opened to add-ons: a watched fd wakes
 * fdk_pump_events()/fdk_run() exactly like display input, and the
 * callback fires from inside the pump (between event batches, the
 * same slot timers fire from — the reentrancy rules below match
 * fdk_timer_add's). This is how an add-on's sockets and pipes
 * integrate with FDK's single event loop instead of a private
 * thread. TIMERS need no extension twin: the public
 * fdk_timer_add/fdk_timer_remove/fdk_timer_reset API IS the loop's
 * timer surface, callable by any library on top of FDK.
 *
 * The flags and the callback's revents use FDK's own bit values —
 * numerically identical to poll(2)'s POLLIN/POLLOUT/POLLERR/
 * POLLHUP, so a callback can pass them straight through where a
 * poll-based loop expects them.
 */
#define FDK_EXT_FD_IN    0x001u /* readable                            */
#define FDK_EXT_FD_OUT   0x004u /* writable                            */
#define FDK_EXT_FD_ERR   0x008u /* error condition (reported, watched) */
#define FDK_EXT_FD_HUP   0x010u /* hangup (reported, never watched)    */

typedef struct fdk_ext_fd_source fdk_ext_fd_source;

/* Called when the watched fd reports revents (a nonzero subset of
 * FDK_EXT_FD_* — ERR/HUP are always reported even if unwatched,
 * matching poll(2)). Runs inside fdk_pump_events()/fdk_run(). */
typedef void (*fdk_ext_fd_fn)(fdk_ext_fd_source *source, uint32_t revents,
                              void *user);

/* Watches `fd` for the FDK_EXT_FD_IN / FDK_EXT_FD_OUT conditions on
 * `ctx`'s loop. fd < 0 or fn NULL fails (NULL return, no state
 * touched). Reentrancy, matching the timer contract: the callback
 * may remove its own or other sources, add sources (first fired on
 * a LATER pump call), destroy windows and surfaces, call
 * fdk_quit(). Removal is synchronous: a removed source never fires
 * again. Returns NULL on invalid arguments or allocation failure. */
fdk_ext_fd_source *fdk_ext_loop_add_fd(fdk_context *ctx, int fd,
                                       uint32_t flags, fdk_ext_fd_fn fn,
                                       void *user);

/* Re-targets a live source's watch mask (FDK_EXT_FD_IN / OUT).
 * This is how a protocol with an output buffer avoids the busy-wake
 * trap: watch IN alone while the buffer is empty, add OUT only
 * while bytes are pending — a socket whose buffer has space is
 * always writable, so a permanently-watched POLLOUT would spin the
 * loop. Invalid flags / NULL source are refused (FDK_ERR_INVALID_
 * ARGUMENT); a deactivated (mid-removal) source is a no-op. Safe
 * inside the source's own callback. Available since extension API
 * v1 (the native D-Bus client is its first consumer). */
fdk_result fdk_ext_loop_update_fd(fdk_ext_fd_source *source,
                                  uint32_t flags);

/* Removes and frees the source (the fd is NOT closed — it was
 * never FDK's). Safe inside the source's own callback (the free is
 * deferred until the callback returns, exactly like
 * fdk_timer_remove inside a timer). NULL is a no-op. */
void fdk_ext_loop_remove_fd(fdk_ext_fd_source *source);

/* The watched fd (diagnostics; -1 on a NULL source). */
int fdk_ext_fd_source_fd(const fdk_ext_fd_source *source);



/* ---- P3: the Wayland add-on raw-event surface ---------------------------
 *
 * An add-on that wants a protocol FDK does not model creates its OWN
 * event queue (fdk_wl_queue_create), reads the advertised globals
 * through the private registry on that queue (fdk_wl_queue_registry —
 * set your own wl_registry listener; it fires on YOUR queue), binds
 * them by NAME with fdk_wl_raw_bind, sets listeners on the returned
 * raw proxies, and either dispatches the queue itself
 * (fdk_wl_queue_dispatch) or attaches it to FDK's pump
 * (fdk_wl_queue_attach) — the pump then gives the queue one
 * non-blocking dispatch pass after the main round each frame.
 * Dispatch goes only to the add-on's listeners; FDK's handlers are
 * never involved. Wayland-only: create answers NULL (and the rest
 * are no-ops) wherever FDK_EXT_CAP_WL_RAW_EVENTS is false. */
typedef struct fdk_wl_queue fdk_wl_queue;

fdk_wl_queue *fdk_wl_queue_create(fdk_context *ctx, const char *name);
void fdk_wl_queue_destroy(fdk_wl_queue *queue);
/* The add-on's private wl_registry proxy (lives on the queue). */
void *fdk_wl_queue_registry(fdk_wl_queue *queue);
/* Binds the advertised global `name` (from the add-on's own listener)
 * onto the queue; returns the raw wl_proxy* the add-on owns. */
void *fdk_wl_raw_bind(fdk_wl_queue *queue, fdk_u32 name,
                      const void *wl_interface, fdk_u32 version);
int fdk_wl_queue_dispatch(fdk_wl_queue *queue);
void fdk_wl_queue_attach(fdk_wl_queue *queue, int on_pump);

/* ---- P3: the X11 add-on event filter ----------------------------------
 *
 * A pre-dispatch hook: the filter sees every RAW XEvent (core or
 * XInput2) before FDK translates it, and may consume it (nonzero
 * return = FDK skips the event). Keycodes arrive untranslated, so an
 * add-on wanting XKB-level detail runs xkbcommon itself on the raw
 * event. One filter per context; setting a new one replaces the old
 * (NULL removes). Override-redirect windows are the X11 popup path
 * (fdk_ext_popup_create) and their events flow through the same
 * filter. X11-only: FDK_ERR_UNSUPPORTED elsewhere. */
typedef int (*fdk_x11_event_filter_fn)(void *user, void *xevent);

fdk_result fdk_x11_set_event_filter(fdk_context *ctx,
                                    fdk_x11_event_filter_fn filter,
                                    void *user);

#ifdef __cplusplus
}
#endif


/* ---- GL for add-ons (P2: a real API, not a recipe) --------------------
 *
 * FDK_EXT_CAP_GL gates every call. The context is the OWNING window's
 * lane context (shared with every other window's context on the
 * connection — texture objects cross between them), current on the
 * surface's native drawable. The frame callback runs from FDK's frame
 * scheduling (right before present) with the context ALREADY current:
 * draw, return, FDK swaps — the add-on never touches swap itself
 * unless it drives the surface with fdk_ext_gl_swap().
 *
 * Present arbitration: a surface with a frame callback presents its GL
 * frame while the rest of the window (and other windows) keep their
 * own render modes. The callback replaces the paint engine for that
 * window's buffer — software content rides fdk_texture if mixed. */
fdk_result fdk_ext_gl_make_current(fdk_ext_surface *surface);

typedef void (*fdk_ext_gl_frame_fn)(fdk_ext_surface *surface, void *user);

/* Registers the per-frame render callback. NULL removes it. The
 * callback runs on the UI thread with the lane context current. */
fdk_result fdk_ext_gl_set_frame_callback(fdk_ext_surface *surface,
                                         fdk_ext_gl_frame_fn fn,
                                         void *user);

/* Presents the surface's GL frame now (for callbacks that finished
 * drawing outside the frame callback; the frame callback's frame is
 * swapped automatically). */
fdk_result fdk_ext_gl_swap(fdk_ext_surface *surface);

/* Explicit sync where the driver supports it (EGLSync / GL fence).
 * FDK_ERR_UNSUPPORTED when the driver lacks the extension at runtime
 * — capability-honest, never faked. */
fdk_result fdk_ext_gl_fence_insert(fdk_ext_surface *surface,
                                   fdk_u64 *out_fence);
fdk_result fdk_ext_gl_fence_wait(fdk_ext_surface *surface, fdk_u64 fence);



/* ---- P3: the session-lock surface (ext-session-lock-v1) ------------------
 *
 * The lock-screen protocol, first-class: mint the lock object
 * (fdk_session_lock_open — the compositor answers through the event
 * callback: 1 = locked, 0 = finished, i.e. the lock was refused or
 * taken; on finished the add-on must destroy and give up), map one
 * lock surface per output (fdk_session_lock_surface takes the RAW
 * wl_surface/wl_output the add-on owns — a lock surface is raw
 * protocol, not an FDK window), and unlock
 * (fdk_session_lock_unlock — destroys the lock object). The raw
 * handles come from the add-on raw-event surface
 * (FDK_EXT_CAP_WL_RAW_EVENTS); the wl_surface MUST BE ROLE-LESS (a
 * surface that already carries a role — a layer surface's native
 * handle, an xdg window's — cannot take the lock role: the
 * compositor answers protocol error 2 and kills the connection).
 * The FIRST-CLASS path (fdk_session_lock_screen_create, v3) mints
 * FDK's own role-less surface, listens for the configure, and
 * renders — no raw handles at all. Wayland-only: NULL/UNSUPPORTED
 * where FDK_EXT_CAP_SESSION_LOCK is false. */
typedef void (*fdk_session_lock_event_fn)(void *user, int locked);

void *fdk_session_lock_open(fdk_context *ctx,
                            fdk_session_lock_event_fn on_event, void *user);
void *fdk_session_lock_surface(fdk_context *ctx, void *lock,
                               void *wl_surface, void *wl_output);
void fdk_session_lock_unlock(void *lock);

/* The FDK-MANAGED lock screen (extension API v3): a FIRST-CLASS lock
 * surface — FDK creates a role-less wl_surface (a surface that
 * already carries a role — e.g. an ext surface's native accessor —
 * CANNOT take the lock role: the compositor answers protocol error
 * 2 and kills the connection), attaches the role on the
 * connection's first bound output, installs the configure listener
 * (the compositor sizes the surface to the output; the configure
 * flows through the ext-surface event stream), and adopts the
 * surface: draw through fdk_ext_surface_get_surface + present, the
 * same objects and lifetimes as every ext surface. The handle stays
 * valid until fdk_ext_surface_destroy (destroy BEFORE
 * fdk_session_lock_unlock — the role teardown wants its surface).
 * Wayland-only: FDK_ERR_UNSUPPORTED where FDK_EXT_CAP_SESSION_LOCK
 * is false. Available since extension API v3. */
fdk_result fdk_session_lock_screen_create(fdk_context *ctx, void *lock,
                                          fdk_ext_surface **out_surface);

/* The same, on the connection's `output_index`-th bound output — the
 * multi-output lock screen: a lock covers the session only when EVERY
 * output carries a lock surface, so one screen per output is the
 * real shape (the outputs accessor fdk_ext_wl_output_count says how
 * many). FDK_ERR_INVALID_ARGUMENT for a bad index; the same
 * UNSUPPORTED contract otherwise. Available since extension API v3
 * (append-only). */
fdk_result fdk_session_lock_screen_create_on_output(
    fdk_context *ctx, void *lock, fdk_i32 output_index,
    fdk_ext_surface **out_surface);



/* ---- P3: input-method-v2 + virtual-keyboard (the IM add-on pair) --------
 *
 * The on-screen-keyboard / IM add-on protocols, first-class:
 * fdk_input_method_open mints the input method bound to a seat (raw
 * wl_seat from the add-on raw-event surface or FDK's seat accessor) —
 * the compositor routes text-input-v3 traffic to it; the add-on sets
 * ITS OWN listeners (activate/deactivate/done) on the returned raw
 * proxy, commits text with fdk_input_method_commit (commit_string +
 * the protocol's done), and may take the keyboard grab
 * (fdk_input_method_grab). fdk_virtual_keyboard_create mints the
 * companion virtual keyboard (optional keymap; XKB_LAYOUT format);
 * fdk_virtual_keyboard_key emits key/press-release events.
 * Wayland-only: NULL/UNSUPPORTED where FDK_EXT_CAP_INPUT_METHOD_V2 /
 * FDK_EXT_CAP_VIRTUAL_KEYBOARD are false. */
void *fdk_input_method_open(fdk_context *ctx, void *seat_raw);
void fdk_input_method_close(void *im);
void *fdk_input_method_grab_keyboard(void *im);
void fdk_input_method_commit(void *im, const char *text);
void *fdk_virtual_keyboard_create(fdk_context *ctx, void *seat_raw,
                                  const char *keymap,
                                  fdk_u32 keymap_size);
void fdk_virtual_keyboard_key(void *kb, fdk_u32 time_ms, fdk_u32 key,
                              fdk_u32 state);

/* Destroys the keyboard (zwp_virtual_keyboard_v1). The CI leak
 * catch, 2026-11-11: without it the proxy survives the connection
 * teardown and LeakSanitizer names it on every Wayland run. NULL is
 * a no-op. Available since extension API v3 (append-only). */
void fdk_virtual_keyboard_destroy(void *kb);

#endif /* FDK_EXTEND_H */
