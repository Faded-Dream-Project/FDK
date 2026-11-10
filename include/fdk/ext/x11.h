/*
 * fdk/ext/x11.h — the OPT-IN X11 escape hatch (1.0.0).
 *
 * The Wayland twin's X11 side: including it IS the opt-in (it needs
 * Xlib's dev headers). EWMH/ICCCM work becomes ordinary Xlib code;
 * the two helpers below cover the dock case the extension API's
 * capability describes.
 */
#ifndef FDK_EXT_X11_H
#define FDK_EXT_X11_H

#include <X11/Xlib.h>

#include "../fdk_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The connection FDK drives. Valid until fdk_shutdown; NULL when the
 * context runs on Wayland or has no live connection. */
Display *fdk_ext_x11_display(fdk_context *ctx);

/* A window's X resource ID (0 when not an X11 window). */
unsigned long fdk_ext_x11_window_handle(fdk_window *window);

/* EWMH dock semantics (the FDK_EXT_CAP_EWMH_DOCK work): set the
 * window's _NET_WM_WINDOW_TYPE (a single atom name) and struts
 * (_NET_WM_STRUT_PARTIAL, 12 longs: left/right/top/bottom each
 * {from, length}; a zero length means no strut on that edge — pass
 * all-zero struts to clear). Screen-global strut semantics are the
 * honest X11 limit (docs/EXTENDING.md §12): a secondary-monitor
 * panel reserves what the running WM interprets. */
fdk_result fdk_ext_x11_set_window_type(fdk_window *window,
                                       const char *type_atom);
fdk_result fdk_ext_x11_set_struts(fdk_window *window,
                                  unsigned long struts[12]);

#ifdef __cplusplus
}
#endif
#endif /* FDK_EXT_X11_H */
