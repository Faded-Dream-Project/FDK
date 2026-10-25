/*
 * fdk_extend.h — Faded Dream ToolKit extension API
 *
 * The surface add-on packages build on (1.1.0). FDK stays small and
 * stays the ROOT: the core and the fdk tools ship together as one
 * package, and everything else — panel layers, notification shells,
 * custom surface roles, input methods — is a SEPARATE package that
 * depends on FDK and uses exactly what this header exposes. The
 * first such package is fdk-shell (wlr-layer-shell surfaces); its
 * source is the reference consumer of every function here.
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
 *     #if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 1
 *     #error "this add-on needs FDK's extension API v1 (FDK 1.1.0)"
 *     #endif
 *     ...
 *     if (fdk_ext_api_version() < 1) { ...refuse to run... }
 *
 * A newer FDK keeps old add-ons working: the runtime number only
 * grows.
 */
#define FDK_EXT_API_VERSION 1

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
     * negotiation (wlr-layer-shell) — FDK cannot vouch for it, so
     * this capability reports false there and the add-on binds the
     * protocol itself. */
    FDK_EXT_CAP_EWMH_DOCK    = 3,
    /* The compositor drives frame pacing (frame-ready feedback):
     * Wayland's frame callbacks. X11 has none in the core protocol
     * (fdk_surface_frame_ready is always-true there), so it reports
     * false — the add-on paces itself. */
    FDK_EXT_CAP_FRAME_PACING = 4,
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
     * role can adopt it (the wlr-layer-shell pattern:
     * zwlr_layer_surface_v1.get_popup(xdg_popup)). `native_popup`
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

/* Removes and frees the source (the fd is NOT closed — it was
 * never FDK's). Safe inside the source's own callback (the free is
 * deferred until the callback returns, exactly like
 * fdk_timer_remove inside a timer). NULL is a no-op. */
void fdk_ext_loop_remove_fd(fdk_ext_fd_source *source);

/* The watched fd (diagnostics; -1 on a NULL source). */
int fdk_ext_fd_source_fd(const fdk_ext_fd_source *source);

#ifdef __cplusplus
}
#endif

#endif /* FDK_EXTEND_H */
