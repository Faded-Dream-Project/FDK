#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "core/log_internal.h"

#include <errno.h>
#include <poll.h>

/* Unlike X11 (x11_dispatch.c), Wayland's client library already does
 * event translation FOR us at the wl_*_listener callback level — each
 * listener function above (wayland_seat.c, wayland_window.c) calls
 * conn->dispatch() directly as events arrive, rather than this
 * function pulling raw events off a queue and translating them one by
 * one. dispatch_pending() here is purely "ask libwayland-client to
 * read and process whatever's queued", which ends up invoking those
 * listeners synchronously.
 *
 * This follows the standard libwayland idiom for an external event
 * loop (context.c's poll() loop is exactly that): prepare_read() /
 * poll on the fd / read_events() / dispatch_pending(), rather than
 * the simpler wl_display_dispatch() (which does its own internal
 * blocking read and would be wrong to call after our own poll() —
 * that risks a second, redundant blocking read). See
 * https://wayland.freedesktop.org/docs/html/apb.html for the pattern
 * this mirrors. */
int fdk_wayland_dispatch_pending(fdk_platform_connection *conn) {
    /* wl_display_prepare_read() fails (nonzero) if events are already
     * queued from a previous read — in that case just dispatch those
     * first, matching x11_dispatch_pending()'s "drain what's already
     * buffered" behavior; the protocol requires this (you must not
     * call prepare_read() again until the queue is drained). */
    if (wl_display_prepare_read(conn->display) != 0) {
        return wl_display_dispatch_pending(conn->display);
    }

    if (wl_display_flush(conn->display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(conn->display);
        FDK_ERROR("wl_display_flush failed (errno=%d)", errno);
        return (int)FDK_ERR_PLATFORM_INIT;
    }

    /* Non-blocking check that the fd actually has data before
     * committing to read_events(), which otherwise blocks until data
     * arrives. context.c's event loop always poll()s before calling
     * us from within the loop body, so this check is redundant (and
     * near-instant) there — but context.c also calls us once before
     * the first poll(), to drain anything already buffered client-
     * side from the initial roundtrips, and at that point there is no
     * prior poll() guaranteeing readability. This keeps
     * dispatch_pending() safe to call in both situations without
     * forcing callers to know which one they're in. */
    struct pollfd pfd = { .fd = wl_display_get_fd(conn->display), .events = POLLIN };
    int poll_result = poll(&pfd, 1, 0); /* timeout 0: check, don't wait */
    if (poll_result <= 0) {
        wl_display_cancel_read(conn->display);
        return 0;
    }

    if (wl_display_read_events(conn->display) < 0) {
        FDK_ERROR("wl_display_read_events failed (errno=%d)", errno);
        return (int)FDK_ERR_PLATFORM_INIT;
    }

    int dispatched = wl_display_dispatch_pending(conn->display);
    if (dispatched < 0) {
        FDK_ERROR("wl_display_dispatch_pending failed (errno=%d)", errno);
        return (int)FDK_ERR_PLATFORM_INIT;
    }
    return dispatched;
}
