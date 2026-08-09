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
void fdk_x11_window_set_size_limits(fdk_platform_window *pwindow,
                                     fdk_size min_size, fdk_size max_size);

#endif /* FDK_X11_PLATFORM_H */
