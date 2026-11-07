/*
 * fdk_idle.h — idle/screensaver inhibition (1.1.0, S-3 / B-11)
 *
 * "The display must stay awake while I am playing / presenting /
 *  dragging a long operation." One inhibitor handle, whatever the
 * desktop actually speaks:
 *
 *   - WAYLAND: the compositor's own zwp_idle_inhibit_manager_v1 —
 *     the surface-scoped inhibitor (the protocol the compositor
 *     honors directly). Compositors without the manager fall
 *     through to the D-Bus path.
 *   - X11 (and Wayland compositors without the protocol): the
 *     org.freedesktop.ScreenSaver D-Bus service (the KDE/xdg
 *     standard) — Inhibit(app, reason) returns a cookie, UnInhibit
 *     (cookie) releases it.
 *
 * The handle is fire-and-forget from the application's view: the
 * D-Bus half is fully asynchronous (the cookie may still be in
 * flight when the application releases the inhibitor — the release
 * is remembered and the matching UnInhibit goes out the moment the
 * cookie arrives, so an early release never orphans an inhibition
 * on the desktop). fdk_idle_uninhibit(NULL) is a no-op.
 *
 * Scope honesty: org.freedesktop.ScreenSaver is the standard this
 * module speaks; GNOME's org.gnome.ScreenSaver variant (same
 * Inhibit shape) is NOT probed — on GNOME Wayland the compositor
 * protocol path is the one that engages anyway.
 */

#ifndef FDK_IDLE_H
#define FDK_IDLE_H

#include "fdk_types.h"
#include "fdk_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fdk_idle_inhibitor fdk_idle_inhibitor;

/* The inhibitor's lifecycle state. */
typedef enum {
    /* A mechanism is engaged: the Wayland inhibitor object exists,
     * or the ScreenSaver cookie has arrived. */
    FDK_IDLE_INHIBIT_ACTIVE = 0,
    /* The D-Bus Inhibit call is in flight (or the bus connection
     * is still handshaking). */
    FDK_IDLE_INHIBIT_PENDING = 1,
    /* No mechanism exists here: no compositor protocol AND no
     * reachable session bus (or the ScreenSaver service answered
     * with an error). The inhibitor is inert; releasing it is
     * still required (the handle is owned either way). */
    FDK_IDLE_INHIBIT_UNSUPPORTED = 2,
} fdk_idle_inhibit_status;

/* Keeps the display from idling (blanking/locking/screensaving).
 *
 * `window` scopes the compositor-protocol inhibitor to that
 * surface (Wayland); it may be NULL for an application-wide
 * inhibition, which rides the D-Bus path on every backend.
 * `app_name` and `reason` are the human-readable pair the
 * ScreenSaver desktop shows in "what is blocking sleep" UIs;
 * NULL/empty app_name degrades to "FDK application", a NULL
 * reason to "". Both are copied.
 *
 * The handle is written through `out` (NULL out is
 * FDK_ERR_INVALID_ARGUMENT). Releasing: fdk_idle_uninhibit. */
fdk_result fdk_idle_inhibit(fdk_context *ctx, fdk_window *window,
                            const char *app_name, const char *reason,
                            fdk_idle_inhibitor **out);

/* Releases an inhibition: the compositor inhibitor is destroyed,
 * or the ScreenSaver cookie is un-inhibited (now, or when it
 * arrives — see the header's lifecycle note). Frees the handle;
 * NULL is a no-op. */
void fdk_idle_uninhibit(fdk_idle_inhibitor *inhibitor);

/* The inhibitor's current state (the enum above). NULL reads as
 * FDK_IDLE_INHIBIT_UNSUPPORTED. */
fdk_idle_inhibit_status fdk_idle_inhibitor_status(
    const fdk_idle_inhibitor *inhibitor);

#ifdef __cplusplus
}
#endif

#endif /* FDK_IDLE_H */
