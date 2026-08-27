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

#include <X11/Xlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <stddef.h>

struct fdk_platform_connection {
    /* Application id (fdk_init_options.app_id), duplicated at connect
     * time; NULL when unset. Applied to every window as WM_CLASS (the
     * X11 counterpart of Wayland's xdg_toplevel.set_app_id — window
     * managers match it for rules and taskbar grouping). */
    char *app_id;
    Display *display;
    int screen;
    Window root;
    Atom wm_delete_window;
    Atom wm_protocols;
    Atom net_wm_name;
    Atom utf8_string;
    Atom motif_wm_hints; /* _MOTIF_WM_HINTS, Phase 8 decorations */

    /* Phase 8 window management: EWMH atoms + what the running WM
     * actually supports, probed once at connect from the root's
     * _NET_SUPPORTED list (see x11_connection.c). ewmh_wm != 0 means
     * an EWMH-capable WM is running (the list exists and is
     * non-empty); ewmh_state_ok additionally requires both
     * _NET_WM_STATE_MAXIMIZED_{VERT,HORIZ} in it. The bare-X fallback
     * paths (no WM: FDK moves/resizes/unmaps directly) key off
     * ewmh_wm. */
    Atom net_wm_state;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_maximized_horiz;
    Atom net_wm_moveresize;
    Atom wm_state;         /* ICCCM WM_STATE (iconic/normal tracking) */
    Atom wm_change_state;  /* ICCCM iconify request message type      */
    int ewmh_wm;           /* nonzero: an EWMH WM is running          */
    int ewmh_state_ok;     /* nonzero: it advertises _NET_WM_STATE    */

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

    fdk_platform_dispatch_fn dispatch;
    void *dispatch_user_data;

    /* --- Clipboard (Phase 9, ICCCM CLIPBOARD selection) ---
     *
     * clip_helper is a never-mapped InputOnly window that acts as
     * FDK's selection owner/requestor. Selection traffic is not tied
     * to any visible window (a context may own the clipboard with
     * zero windows open), so the connection owns a private helper —
     * the same design every ICCCM-faithful toolkit uses. The atoms
     * below are interned once at connect. clip_owned_text is the
     * copy FDK serves while it owns CLIPBOARD; SelectionClear
     * (another client took over) frees it. */
    Window clip_helper;
    Atom atom_clipboard;      /* CLIPBOARD                            */
    Atom atom_targets;        /* TARGETS                              */
    Atom atom_incr;           /* INCR (recognized only — refused)     */
    Atom atom_text;           /* TEXT (legacy compound text alias)    */
    Atom atom_text_plain;     /* text/plain;charset=utf-8             */
    Atom atom_fdk_selection;  /* private property for convert replies */
    char *clip_owned_text;    /* fdk_alloc'd, NULL when not owner     */

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

    /* Phase 9 popup state: override-redirect windows that grab the
     * pointer (and keyboard) while shown; a click outside their
     * bounds dismisses them (translated to a close request in
     * x11_events.c, never delivered as a button event). */
    int popup;
    int grabbed;

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
};

/* Implemented in x11_events.c, used by x11_dispatch.c. Not part of
 * the fdk_platform_ops interface — internal to this backend only. */
int fdk_x11_translate_event(fdk_platform_window *pwindow, XEvent *xevent,
                             fdk_event_data *out);

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
void fdk_x11_window_destroy(fdk_platform_window *pwindow);
void fdk_x11_window_show(fdk_platform_window *pwindow);
void fdk_x11_window_hide(fdk_platform_window *pwindow);
void fdk_x11_window_set_title(fdk_platform_window *pwindow, const char *title);
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

/* Phase 8 state helpers (x11_window.c), shared with x11_events.c's
 * PropertyNotify translation: */
/* Compare-and-flip + FDK_EVENT_WINDOW_STATE dispatch (no-op when the
 * state didn't change). */
void fdk_x11_window_update_state(fdk_platform_window *pwindow,
                                 int maximized, int minimized);
/* Window's _NET_WM_STATE property -> maximized (both axes), 0 when
 * absent/unreadable. */
int fdk_x11_window_net_state_maximized(fdk_platform_window *pwindow);
/* Window's WM_STATE property -> iconic flag, -1 when unreadable. */
int fdk_x11_window_wm_state_iconic(fdk_platform_window *pwindow);
void fdk_x11_window_set_size_limits(fdk_platform_window *pwindow,
                                     fdk_size min_size, fdk_size max_size);

/* Software rendering (see x11_surface.c for the design notes). */
fdk_result fdk_x11_window_get_framebuffer(fdk_platform_window *pwindow,
                                           fdk_platform_framebuffer *out_fb);
fdk_result fdk_x11_window_present(fdk_platform_window *pwindow,
                                  const fdk_platform_damage *damage);
void fdk_x11_surface_cleanup(fdk_platform_window *pwindow);

/* Clears the in-flight flag of the slot owning `shmseg` on this
 * window (ShmCompletion routing; see x11_surface.c). Called from
 * x11_dispatch.c and from the acquire-side sync wait. */
void fdk_x11_surface_shm_completion(fdk_platform_window *pwindow,
                                    unsigned long shmseg);

/* Popup grabs (x11_window.c): called from window_show/hide when the
 * window is a popup. Idempotent; failures log and continue (the
 * popup still works, it just doesn't grab). */
void fdk_x11_window_popup_grab(fdk_platform_window *pwindow);
void fdk_x11_window_popup_ungrab(fdk_platform_window *pwindow);

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

#endif /* FDK_X11_PLATFORM_H */
