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
#include <stddef.h>

struct fdk_platform_connection {
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

    fdk_platform_dispatch_fn dispatch;
    void *dispatch_user_data;

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

    /* Software-rendering state, owned by x11_surface.c: an XImage
     * holding the application's pixels plus the GC used to blit it.
     * NULL until the first fdk_surface acquisition. */
    XImage *render_image;
    GC render_gc;
    fdk_size render_size; /* dimensions render_image was created at */
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
                            void *dispatch_user_data,
                            fdk_platform_connection **out_conn);
void fdk_x11_disconnect(fdk_platform_connection *conn);
int fdk_x11_get_event_fd(fdk_platform_connection *conn);
int fdk_x11_dispatch_pending(fdk_platform_connection *conn);

fdk_result fdk_x11_window_create(fdk_platform_connection *conn,
                                  const fdk_window_options *options,
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

#endif /* FDK_X11_PLATFORM_H */
