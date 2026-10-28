#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/log_internal.h"
/* The XSETTINGS notification routes to the theme recheck through
 * the sanctioned platform->window->theme bridge (the same path the
 * clipboard's window glue uses; window_internal.h pulls the ops
 * surface this file already builds on). */
#include "window/window_internal.h"

#include <string.h>

/* ---- the popup-grab watchdog + desktop-switch release (1.1.0-rc1) ---
 *
 * The Openbox report's safety nets: a popup's POINTER grab must never
 * outlive the popup's own relevance.
 *
 *   DESKTOP CHANGE: the root's _NET_CURRENT_DESKTOP PropertyNotify
 *   latches conn->desktop_changed (handled where root events arrive
 *   in dispatch_pending, below). The tick responds by dismissing
 *   every grabbed popup — an open dropdown belongs to the desktop it
 *   opened on, and a grab that survives the switch is exactly the
 *   "workspace switching stopped working" report.
 *
 *   NOT-VIEWABLE WATCHDOG: a popup that unmapped (WM action, our own
 *   hide, an X error) without releasing its grab would hold the
 *   pointer hostage forever. While any window holds a popup grab,
 *   every 500 ms each grabbed window's map_state is checked (one
 *   XGetWindowAttributes round trip per grabbed window per tick —
 *   zero cost when no popup is open); not viewable → release + one
 *   warning naming the duration.
 *
 * The tick runs at the END of dispatch_pending: the pump calls it at
 * least once per loop pass, so 500 ms elapses under any sane poll
 * cadence (and the platform-deadline cap in context.c keeps
 * indefinite fdk_run loops honest). */
#define POPUP_WATCHDOG_INTERVAL_MS 500

void fdk_x11_popup_watchdog_tick(fdk_platform_connection *conn) {
    if (conn == NULL || conn->display_dead ||
        conn->window_count == 0) {
        return;
    }
    uint64_t now = fdk__x11_now_ms();
    int any_grab = 0;
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->grabbed) {
            any_grab = 1;
            break;
        }
    }
    if (!any_grab && !conn->desktop_changed) {
        return; /* the common case: nothing held, nothing owed */
    }
    if (!conn->desktop_changed &&
        now - conn->popup_watchdog_ms < POPUP_WATCHDOG_INTERVAL_MS) {
        return; /* paced */
    }
    conn->popup_watchdog_ms = now;

    /* Iterate a SNAPSHOT index walk: dismissals destroy windows, and
     * destroy mutates conn->windows (the registry unlinks). Walking
     * indices while entries vanish would skip windows — so the
     * desktop-change pass re-scans from the head until no grabbed
     * popup remains (bounded: each iteration removes at least one). */
    if (conn->desktop_changed) {
        conn->desktop_changed = 0;
        int dismissed = 0;
        for (;;) {
            fdk_platform_window *victim = NULL;
            for (size_t i = 0; i < conn->window_count; i++) {
                if (conn->windows[i]->grabbed &&
                    conn->windows[i]->popup) {
                    victim = conn->windows[i];
                    break;
                }
            }
            if (victim == NULL) {
                break;
            }
            fdk_x11_window_popup_set_release_reason(victim,
                                                    "desktop-change");
            FDK_WARN("desktop switch with a popup open — dismissing "
                     "it and releasing its grab");
            fdk_event_data event;
            memset(&event, 0, sizeof(event));
            event.type = FDK_EVENT_WINDOW_CLOSE_REQUEST;
            conn->dispatch(victim, &event,
                           conn->dispatch_user_data);
            dismissed++;
        }
        if (dismissed == 0) {
            /* A grabbed NON-popup (modal dialog) rode across the
             * switch: modality survives desktops by design — nothing
             * to do, but say so under trace. */
            FDK_DEBUG("desktop switch with no popup grabs outstanding");
        }
        return;
    }

    /* The viewability sweep (only when a grab is held). */
    for (size_t i = 0; i < conn->window_count; i++) {
        fdk_platform_window *pwindow = conn->windows[i];
        if (!pwindow->grabbed || !pwindow->popup) {
            continue;
        }
        XWindowAttributes wa;
        if (XGetWindowAttributes(conn->display, pwindow->xwindow,
                                 &wa) == 0) {
            continue; /* window dying: destroy will release */
        }
        if (wa.map_state != IsViewable) {
            uint64_t held =
                pwindow->popup_grab_ms != 0
                    ? (uint64_t)(now - pwindow->popup_grab_ms)
                    : 0;
            FDK_WARN("popup 0x%lx is not viewable but still holds "
                     "its grab (held %llu ms) — the watchdog is "
                     "releasing it",
                     (unsigned long)pwindow->xwindow,
                     (unsigned long long)held);
            fdk_x11_window_popup_set_release_reason(
                pwindow, "watchdog-not-viewable");
            fdk_x11_window_popup_ungrab(pwindow);
        }
    }
}

int fdk_x11_dispatch_pending(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (conn->display_dead) {
        /* Dead connection: report the fatal negative result the
         * pump/fdk_run contract consumes (fdk_run exits its loop,
         * the application tears down normally — the 1.3.0 fix for
         * "X server death used to exit(1) the process via Xlib's
         * default IO handler"). */
        return FDK_ERR_NO_DISPLAY;
    }
    /* Arm the IO-error longjmp (see x11_connection.c): any Xlib
     * call below hitting a lost connection unwinds HERE instead of
     * exiting the process. (1.4.14: the second, duplicated copy of
     * both blocks below — a merge artifact — removed; one arm, one
     * disarm, in one place.) */
    if (sigsetjmp(fdk__x11_io_jmp, 1) != 0) {
        fdk__x11_io_armed = 0;
        return FDK_ERR_NO_DISPLAY;
    }
    fdk__x11_io_armed = 1;

    int processed = 0;

    while (XPending(conn->display) > 0) {
        XEvent xevent;
        XNextEvent(conn->display, &xevent);

        /* Motion coalescing (1.1.0-rc1, X2 — the lagging dropdown
         * highlight): X11 queues EVERY MotionNotify the pointer
         * hardware produced (hundreds per second in motion), and the
         * old loop translated and dispatched them one at a time — a
         * listener repaint per event meant the present rate, not the
         * frame rate, paced the backlog, and the hover feedback fell
         * further behind with every event (the live report: "the
         * dropdown highlight lags behind the pointer; on Wayland
         * it's instant"). The X11-correct drain: when this event is
         * a motion and MORE motions for the same window are already
         * queued, skip ahead to the LATEST one — the intermediate
         * positions are dead state nobody can render between frames
         * anyway (Xt/GTK's classic XPeekIfEvent pattern). Buttons,
         * keys, crossings and everything else keep exact ordering;
         * only the redundant motion spam collapses. */
        if (xevent.type == MotionNotify) {
            XEvent newer;
            while (XPending(conn->display) > 0 &&
                   XPeekEvent(conn->display, &newer) &&
                   newer.type == MotionNotify &&
                   newer.xmotion.window == xevent.xmotion.window) {
                /* Consume the peeked motion; the loop re-peeks until
                 * the next queued event is not a same-window motion.
                 * Only POSITION is lost (dead state between frames);
                 * buttons, keys, and crossings keep exact order, and
                 * a different window's motions are never swallowed
                 * (its own coalescing pass handles them). */
                XNextEvent(conn->display, &xevent);
            }
        }

        /* IM filtering (1.3.1): Xlib requires EVERY event to pass
         * through XFilterEvent before normal processing when an input
         * method is active — the IM may consume composition traffic
         * (and dead-key sequences) entirely. Filtered events produce
         * no FDK event. */
        if (conn->xim != NULL && XFilterEvent(&xevent, None)) {
            continue;
        }

        /* XI2 touch (1.1.0, M-3): GenericEvents (type 35) carry the
         * X Input protocol; the cookie payload needs XGetEventData
         * (libX11 itself). Matched by the extension opcode and fully
         * handled by the dlopen'd-libXi module (x11_xi2.c), which
         * resolves its own target window from the payload. The
         * cookie data is freed HERE — the module never owns it. */
        if (xevent.type == 35 /* GenericEvent */) {
            if (conn->xi2 != NULL &&
                xevent.xcookie.extension == conn->xi2->opcode) {
                if (XGetEventData(conn->display, &xevent.xcookie)) {
                    (void)fdk_x11_xi2_handle_cookie(conn, &xevent.xcookie);
                    XFreeEventData(conn->display, &xevent.xcookie);
                    processed++;
                }
                continue;
            }
            /* Someone else's GenericEvent (another extension's):
             * nothing to translate, nothing to free (we never fetched
             * the cookie). */
            continue;
        }

        /* Root PropertyNotify (1.1.0-rc1): the popup watchdog's
         * desktop-change input. _NET_CURRENT_DESKTOP changes arrive
         * on the ROOT window (PropertyChangeMask selected at
         * connect); handled BEFORE the window-table lookup — the
         * root is deliberately not a registered window. Other root
         * property churn (the WM's own bookkeeping) is ignored. */
        if (xevent.type == PropertyNotify &&
            xevent.xproperty.window == conn->root &&
            xevent.xproperty.atom == conn->net_current_desktop) {
            conn->desktop_changed = 1;
            continue;
        }

        /* XSETTINGS live follow (1.1.0): a settings-daemon property
         * rewrite — color-scheme, icon theme, whatever the daemon
         * publishes. Routed BEFORE the window lookup (the manager
         * window is not an FDK window); the theme recheck re-reads
         * the whole XSETTINGS state it tracks, so one notification
         * serves every consumer. */
        if (xevent.type == PropertyNotify &&
            conn->xsettings_owner != None &&
            xevent.xproperty.window == conn->xsettings_owner &&
            xevent.xproperty.atom == conn->xsettings_settings) {
            fdk__window_theme_settings_notify();
            continue;
        }

        /* A drag WE started owns pointer events while active (the
         * XDND source state machine — target tracking, Enter/Leave/
         * Position emission, the drop handshake). Routed before
         * everything else: the grab directs the events at us, not
         * at whatever window is visually under the pointer. */
        if (fdk_x11_dnd_source_handle_event(conn, &xevent)) {
            processed++;
            continue;
        }

        /* ShmCompletion (MIT-SHM): the server finished reading a
         * shared pixel segment — clear that slot's in-flight flag so
         * the next acquisition can hand it out again. Routed BEFORE
         * the translate path: it is a backend-internal event the
         * application never sees. */
        if (conn->shm_ok &&
            xevent.type == conn->shm_event_base + ShmCompletion) {
            fdk_platform_window *shm_window =
                fdk_x11_find_window(conn, xevent.xany.window);
            if (shm_window != NULL) {
                fdk_x11_surface_shm_completion(
                    shm_window,
                    (unsigned long)((const XShmCompletionEvent *)&xevent)
                        ->shmseg);
            }
            continue;
        }

        /* Clipboard helper traffic (Phase 9): selection requests we
         * must serve, ownership losses, stray notifies. Routed BEFORE
         * the window-table lookup — the helper is intentionally not
         * in that table (it is not an application window).
         *
         * 1.2.0: XDndStatus / XDndFinished replies to a drag WE
         * started arrive on the same helper (it is our XDND source
         * identity), and SelectionRequests naming the XDND selection
         * must reach the DnD serving path BEFORE the clipboard's
         * (which only stocks the CLIPBOARD selection's payload). */
        if (fdk_x11_dnd_source_handle_helper_message(conn, &xevent)) {
            processed++;
            continue;
        }
        if (xevent.type == SelectionRequest &&
            fdk_x11_dnd_serve_selection(
                conn, &xevent.xselectionrequest)) {
            processed++;
            continue;
        }
        if (fdk_x11_clipboard_handle_event(conn, &xevent)) {
            continue;
        }

        Window xwindow = xevent.xany.window;
        fdk_platform_window *pwindow = fdk_x11_find_window(conn, xwindow);
        if (pwindow == NULL) {
            /* Event for a window FDK doesn't own/track (root window
             * notifications some WMs send us, or a window we already
             * destroyed but X still had queued events for). Not an
             * error — just nothing to dispatch. */
            continue;
        }

        /* WM_DELETE_WINDOW arrives as a ClientMessage, not a distinct
         * X event type — handled here rather than in
         * fdk_x11_translate_event() since it needs conn->wm_protocols/
         * wm_delete_window to identify, which that function doesn't
         * have access to (by design — it only sees the XEvent). */
        if (xevent.type == ClientMessage &&
            xevent.xclient.message_type == conn->wm_protocols &&
            (Atom)xevent.xclient.data.l[0] == conn->wm_delete_window) {
            fdk_event_data event = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
            conn->dispatch(pwindow, &event, conn->dispatch_user_data);
            processed++;
            continue;
        }

        /* XDND receiver traffic (1.2.0): the Enter/Position/Leave/
         * Drop handshake for drags arriving at THIS window, from any
         * external client (or another FDK process). */
        if (fdk_x11_dnd_handle_client_message(conn, &xevent)) {
            processed++;
            continue;
        }

        fdk_event_data event;
        if (fdk_x11_translate_event(pwindow, &xevent, &event)) {
            conn->dispatch(pwindow, &event, conn->dispatch_user_data);
            processed++;
        }
    }

    /* The XDND source's Finished watchdog (retires a target that
     * died mid-handshake after the drop was sent). */
    fdk_x11_dnd_source_tick(conn);

    /* The XSETTINGS owner re-arm (1.1.0): throttled to one cheap
     * selection-owner round trip per 2 s — catches settings daemons
     * that appear or restart after connect. */
    fdk_x11_xsettings_rearm_tick(conn);

    /* The popup-grab watchdog + desktop-switch release (1.1.0-rc1):
     * the last thing this dispatch does — everything above may have
     * opened or closed popups, and the tick's job is exactly to
     * observe the state the batch left behind. */
    fdk_x11_popup_watchdog_tick(conn);

    /* The GL lane's timer-based self-check retry (1.1.0-rc1, B.2):
     * fires the scheduled probe attempt here instead of the OLD
     * inline XSync+nanosleep budget, so a present never blocks the
     * loop (a popup may be holding its grab). The pump wakes for the
     * deadline through fdk_x11_next_wakeup_ms. */
    fdk_x11_gl_selfcheck_tick(conn->gl);

    fdk__x11_io_armed = 0;
    return processed;
}
