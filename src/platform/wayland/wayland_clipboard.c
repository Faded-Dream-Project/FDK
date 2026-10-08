/* _GNU_SOURCE (defined BEFORE any header): pipe2(). The build's
 * _POSIX_C_SOURCE covers clock_gettime; Xlib's headers incidentally
 * pull both in for the x11 backend, but this file includes none. */
#define _GNU_SOURCE

#define FDK_LOG_TAG "wayland"

/*
 * wayland_clipboard.c — wl_data_device clipboard (Phase 9); the
 * wp_primary_selection_unstable_v1 PRIMARY selection joined it in
 * 1.3.4 (the half Wayland clients traditionally lost — the classic
 * Unix "whatever is selected" buffer that middle-click pastes)
 *
 * Set: wl_data_source offered as "text/plain;charset=utf-8" (the
 * canonical Wayland text clipboard MIME) and published with
 * wl_data_device.set_selection citing the newest input serial. The
 * compositor later calls send(fd) once per pasting client; we write
 * the bytes and close. cancelled means the compositor replaced our
 * selection — destroy source + text.
 *
 * Get: compositors push the current selection to every client as
 * wl_data_device::data_offer + ::selection events (including right
 * after the data device is created), so by the time an application
 * calls get_text, conn->selection_offer is normally already current.
 * When it is not (the selection changed since our last dispatch),
 * ONE wl_display_roundtrip catches up — bounded, and the events it
 * delivers go through the ordinary dispatch path exactly as if
 * pumped by fdk_run (the widget layer's dispatch guard is
 * depth-counted; see widget.c). The actual transfer then rides a
 * pipe: wl_data_offer.receive asks the owner to write into our write
 * end, and a bounded poll() read collects the bytes without further
 * protocol traffic.
 *
 * PRIMARY (1.3.4) mirrors every shape above one protocol over — the
 * zwp_* twins of device/source/offer with the same MIME discipline,
 * the same pipe transfer, the same own-source fast path — with two
 * differences worth naming: the manager global is OPTIONAL (absent
 * compositors get honest FDK_ERR_UNSUPPORTED, not a fake buffer),
 * and the ::selection event's own fine print ("the data_offer is
 * valid until ... the client loses keyboard focus") means a NULL
 * offer on focus loss is ROUTINE here, not an emptied selection.
 *
 * Honest limitations (mirrored in fdk_clipboard.h): no source
 * actions, text only, and set_selection before any input event
 * carries serial 0 which compositors may ignore. Drag-and-drop
 * lives in wayland_dnd.c; the device listener below routes the
 * drag events there.
 */

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FDK_WL_CLIP_READ_MS 250

/* ---- data offer (the compositor's current selection, from FDK's
 * perspective a read-only handle) ---- */

static void offer_offer(void *data, struct wl_data_offer *offer,
                        const char *mime_type) {
    fdk_platform_connection *conn = data;
    (void)offer;
    if (strcmp(mime_type, "text/plain;charset=utf-8") == 0 ||
        strcmp(mime_type, "text/plain") == 0) {
        conn->pending_offer_has_text = 1;
    }
    if (strcmp(mime_type, "text/uri-list") == 0) {
        /* 1.2.0: drag offers (and clipboard managers) may carry
         * files; the DnD enter path reads this flag. */
        conn->pending_offer_has_uris = 1;
    }
}

/* The DnD half of the offer protocol — FDK does no drag-and-drop, so
 * these are required no-ops like the pointer frame events in
 * wayland_seat.c. */
static void offer_source_actions(void *data, struct wl_data_offer *offer,
                                 uint32_t source_actions) {
    (void)data; (void)offer; (void)source_actions;
}
static void offer_action(void *data, struct wl_data_offer *offer,
                         uint32_t dnd_action) {
    (void)data; (void)offer; (void)dnd_action;
}

static const struct wl_data_offer_listener g_offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

static void device_data_offer(void *data, struct wl_data_device *device,
                              struct wl_data_offer *offer) {
    (void)device;
    fdk_platform_connection *conn = data;
    /* A new offer is being announced (the ::selection event that
     * names it follows — or doesn't, for drag-and-drop). The MIME
     * ::offer events accumulate on the PENDING slot; ::selection
     * later promotes it with its flags intact. A previous unconsumed
     * pending offer (a DnD offer that never became a selection) is
     * released here. */
    if (conn->pending_offer != NULL &&
        conn->pending_offer != conn->selection_offer) {
        wl_data_offer_destroy(conn->pending_offer);
    }
    conn->pending_offer = offer;
    conn->pending_offer_has_text = 0;
    conn->pending_offer_has_uris = 0;
    wl_data_offer_add_listener(offer, &g_offer_listener, conn);
}

static void device_selection(void *data, struct wl_data_device *device,
                             struct wl_data_offer *offer) {
    (void)device;
    fdk_platform_connection *conn = data;
    if (conn->selection_offer != NULL) {
        wl_data_offer_destroy(conn->selection_offer);
    }
    if (offer == NULL) {
        /* Clipboard emptied. */
        conn->selection_offer = NULL;
        conn->selection_offer_has_text = 0;
        return;
    }
    /* Promote the pending offer (per protocol ordering the ::offer
     * MIME events for it have already arrived). A selection naming
     * an offer we never saw ::data_offer for cannot happen on a
     * well-behaved compositor; if it somehow does, treat it as
     * text-less rather than dereferencing untracked state. */
    int has_text = 0;
    if (conn->pending_offer == offer) {
        has_text = conn->pending_offer_has_text;
        conn->pending_offer = NULL;
        conn->pending_offer_has_text = 0;
    }
    conn->selection_offer = offer;
    conn->selection_offer_has_text = has_text;

    /* 1.2.4: this event does NOT mean our own source was replaced.
     * wlroots 0.18 echoes the freshly-set selection back to the
     * keyboard-focused client — the setter itself — so destroying
     * our source here self-destructed EVERY set under sway/wlroots
     * (the cross-process interop rig caught it: readers always saw
     * selection(nil) because the source died the instant it was
     * registered; sway's debug log showed no rejection at all — the
     * request was honored and then un-done client-side). Replacement
     * has its own protocol signal, wl_data_source::cancelled, which
     * source_cancelled below already handles (and now also drops the
     * stale offer). Ownership remains clip_source != NULL, and
     * get_text's own-selection fast path serves locally without ever
     * touching the echoed offer. */
}

/* The DnD half of the device protocol — routed to wayland_dnd.c
 * (1.2.0), which owns acceptance (set_actions), the drag events,
 * and the drop transfer. These thin forwarders exist so the device
 * listener stays assembled in this file next to the selection
 * handling it shares state with. */
static void device_enter(void *data, struct wl_data_device *device,
                         uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *offer) {
    (void)device;
    fdk_wayland_dnd_device_enter(data, serial, surface, x, y, offer);
}
static void device_leave(void *data, struct wl_data_device *device) {
    (void)device;
    fdk_wayland_dnd_device_leave(data);
}
static void device_motion(void *data, struct wl_data_device *device,
                          uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)device;
    fdk_wayland_dnd_device_motion(data, time, x, y);
}
static void device_drop(void *data, struct wl_data_device *device) {
    (void)device;
    fdk_wayland_dnd_device_drop(data);
}

static const struct wl_data_device_listener g_device_listener = {
    .data_offer = device_data_offer,
    .selection = device_selection,
    .enter = device_enter,
    .leave = device_leave,
    .motion = device_motion,
    .drop = device_drop,
};

/* ---- data source (our side of ownership) ---- */

static void source_target(void *data, struct wl_data_source *source,
                          const char *mime_type) {
    (void)data; (void)source; (void)mime_type;
}

static void source_send(void *data, struct wl_data_source *source,
                        const char *mime_type, int32_t fd) {
    (void)source;
    fdk_platform_connection *conn = data;
    if (strcmp(mime_type, "text/plain;charset=utf-8") != 0 &&
        strcmp(mime_type, "text/plain") != 0) {
        /* We only ever offer text MIMes, but be strict anyway. */
        close(fd);
        return;
    }
    const char *text = (conn->clip_owned_text != NULL)
        ? conn->clip_owned_text
        : "";
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* requestor vanished mid-transfer: not our error */
        }
        off += (size_t)n;
    }
    close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
    fdk_platform_connection *conn = data;
    /* The compositor replaced our selection (or shut down). The
     * source is inert from here — destroy it and drop the text.
     * 1.2.4: also drop the selection offer — if it wrapped OUR source
     * (wlroots echoes the freshly-set selection back to the focused
     * setter), it just died with the source; a later get_text must
     * not touch it. The next selection event stores the new owner's
     * offer. */
    wl_data_source_destroy(source);
    if (conn->clip_source == source) {
        conn->clip_source = NULL;
    }
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = NULL;
    if (conn->selection_offer != NULL) {
        wl_data_offer_destroy(conn->selection_offer);
        conn->selection_offer = NULL;
        conn->selection_offer_has_text = 0;
    }
}

/* DnD-only source events: no-ops for the same reason as above. */
static void source_dnd_drop_performed(void *data, struct wl_data_source *source) {
    (void)data; (void)source;
}
static void source_dnd_finished(void *data, struct wl_data_source *source) {
    (void)data; (void)source;
}
static void source_action(void *data, struct wl_data_source *source,
                          uint32_t dnd_action) {
    (void)data; (void)source; (void)dnd_action;
}

static const struct wl_data_source_listener g_source_listener = {
    .target = source_target,
    .send = source_send,
    .cancelled = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished = source_dnd_finished,
    .action = source_action,
};

/* ---- PRIMARY selection (1.3.4, wp_primary_selection_unstable_v1) ----
 *
 * The zwp_* twins of the three listeners above, with the shapes
 * learned in 1.2.4 already baked in: ::selection NEVER destroys our
 * source (wlroots echoes the freshly-set selection back to the
 * keyboard-focused setter — the exact trap that self-destructed
 * every clipboard set under sway until ::cancelled was recognized
 * as the only real replacement signal); cancelled is checked against
 * the epoch slot before freeing anything. */

static void primary_offer_offer(void *data,
                                struct zwp_primary_selection_offer_v1 *offer,
                                const char *mime_type) {
    fdk_platform_connection *conn = data;
    (void)offer;
    if (strcmp(mime_type, "text/plain;charset=utf-8") == 0 ||
        strcmp(mime_type, "text/plain") == 0) {
        conn->primary_pending_offer_has_text = 1;
    }
}

static const struct zwp_primary_selection_offer_v1_listener
    g_primary_offer_listener = {
        .offer = primary_offer_offer,
};

static void primary_device_data_offer(
    void *data, struct zwp_primary_selection_device_v1 *device,
    struct zwp_primary_selection_offer_v1 *offer) {
    (void)device;
    fdk_platform_connection *conn = data;
    if (conn->primary_pending_offer != NULL &&
        conn->primary_pending_offer != conn->primary_selection_offer) {
        zwp_primary_selection_offer_v1_destroy(
            conn->primary_pending_offer);
    }
    conn->primary_pending_offer = offer;
    conn->primary_pending_offer_has_text = 0;
    zwp_primary_selection_offer_v1_add_listener(
        offer, &g_primary_offer_listener, conn);
}

static void primary_device_selection(
    void *data, struct zwp_primary_selection_device_v1 *device,
    struct zwp_primary_selection_offer_v1 *id) {
    (void)device;
    fdk_platform_connection *conn = data;
    if (conn->primary_selection_offer != NULL) {
        zwp_primary_selection_offer_v1_destroy(
            conn->primary_selection_offer);
    }
    if (id == NULL) {
        /* Emptied — or the routine NULL the protocol's fine print
         * promises on keyboard-focus loss. Either way: no primary
         * text to read. Our own source (if any) is NOT touched:
         * ownership lives until ::cancelled says otherwise. */
        conn->primary_selection_offer = NULL;
        conn->primary_selection_offer_has_text = 0;
        return;
    }
    int has_text = 0;
    if (conn->primary_pending_offer == id) {
        has_text = conn->primary_pending_offer_has_text;
        conn->primary_pending_offer = NULL;
        conn->primary_pending_offer_has_text = 0;
    }
    conn->primary_selection_offer = id;
    conn->primary_selection_offer_has_text = has_text;
}

static const struct zwp_primary_selection_device_v1_listener
    g_primary_device_listener = {
        .data_offer = primary_device_data_offer,
        .selection = primary_device_selection,
};

static void primary_source_send(
    void *data, struct zwp_primary_selection_source_v1 *source,
    const char *mime_type, int32_t fd) {
    (void)source;
    fdk_platform_connection *conn = data;
    if (strcmp(mime_type, "text/plain;charset=utf-8") != 0 &&
        strcmp(mime_type, "text/plain") != 0) {
        close(fd);
        return;
    }
    const char *text = (conn->primary_owned_text != NULL)
        ? conn->primary_owned_text
        : "";
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* requestor vanished mid-transfer: not our error */
        }
        off += (size_t)n;
    }
    close(fd);
}

static void primary_source_cancelled(
    void *data, struct zwp_primary_selection_source_v1 *source) {
    fdk_platform_connection *conn = data;
    /* Another client took the primary (or the compositor is done
     * with it). Epoch check before freeing: a replaced source's
     * cancelled must not drop a NEWER set's text. */
    zwp_primary_selection_source_v1_destroy(source);
    if (conn->primary_source == source) {
        conn->primary_source = NULL;
        fdk_free(conn->primary_owned_text);
        conn->primary_owned_text = NULL;
    }
}

static const struct zwp_primary_selection_source_v1_listener
    g_primary_source_listener = {
        .send = primary_source_send,
        .cancelled = primary_source_cancelled,
};

/* ---- lifecycle wiring ---- */

void fdk_wayland_clipboard_device_ready(fdk_platform_connection *conn) {
    if (conn->data_device != NULL ||
        conn->data_device_manager == NULL || conn->seat == NULL) {
        /* fall through to the primary half below — the two devices
         * converge independently (a compositor may offer PRIMARY
         * without wl_data_device_manager or vice versa) */
    } else {
        conn->data_device =
            wl_data_device_manager_get_data_device(conn->data_device_manager,
                                                    conn->seat);
        if (conn->data_device == NULL) {
            FDK_WARN("clipboard: get_data_device failed");
        } else {
            wl_data_device_add_listener(conn->data_device,
                                        &g_device_listener, conn);
            /* The compositor sends the CURRENT selection to a fresh
             * data device; the roundtrip in the caller's initial sync
             * (or the next dispatch) delivers it into
             * conn->selection_offer. */
        }
    }
    /* PRIMARY device (1.3.4): same convergence rule — manager
     * global + seat. Absent manager (compositor without the
     * protocol) leaves primary_device NULL and the primary ops
     * report UNSUPPORTED; that is a supported configuration, not
     * an error worth a log line. */
    if (conn->primary_device == NULL &&
        conn->primary_manager != NULL && conn->seat != NULL) {
        conn->primary_device =
            zwp_primary_selection_device_manager_v1_get_device(
                conn->primary_manager, conn->seat);
        if (conn->primary_device == NULL) {
            FDK_WARN("clipboard: primary get_device failed");
        } else {
            zwp_primary_selection_device_v1_add_listener(
                conn->primary_device, &g_primary_device_listener, conn);
        }
    }
}

void fdk_wayland_clipboard_teardown(fdk_platform_connection *conn) {
    /* An in-flight drag of ours is cancelled first (it reports its
     * on_done, matching every other teardown path). */
    fdk_wayland_dnd_teardown(conn);
    if (conn->clip_source != NULL) {
        wl_data_source_destroy(conn->clip_source);
        conn->clip_source = NULL;
    }
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = NULL;
    if (conn->pending_offer != NULL &&
        conn->pending_offer != conn->selection_offer) {
        wl_data_offer_destroy(conn->pending_offer);
    }
    conn->pending_offer = NULL;
    conn->pending_offer_has_text = 0;
    conn->pending_offer_has_uris = 0;
    if (conn->selection_offer != NULL) {
        wl_data_offer_destroy(conn->selection_offer);
        conn->selection_offer = NULL;
    }
    conn->selection_offer_has_text = 0;
    if (conn->data_device != NULL) {
        wl_data_device_release(conn->data_device);
        conn->data_device = NULL;
    }
    if (conn->data_device_manager != NULL) {
        wl_data_device_manager_destroy(conn->data_device_manager);
        conn->data_device_manager = NULL;
    }
    /* PRIMARY half (1.3.4): same teardown discipline — sources and
     * text first, then offers, then the device, then the manager. */
    if (conn->primary_source != NULL) {
        zwp_primary_selection_source_v1_destroy(conn->primary_source);
        conn->primary_source = NULL;
    }
    fdk_free(conn->primary_owned_text);
    conn->primary_owned_text = NULL;
    if (conn->primary_pending_offer != NULL &&
        conn->primary_pending_offer != conn->primary_selection_offer) {
        zwp_primary_selection_offer_v1_destroy(
            conn->primary_pending_offer);
    }
    conn->primary_pending_offer = NULL;
    conn->primary_pending_offer_has_text = 0;
    if (conn->primary_selection_offer != NULL) {
        zwp_primary_selection_offer_v1_destroy(
            conn->primary_selection_offer);
        conn->primary_selection_offer = NULL;
    }
    conn->primary_selection_offer_has_text = 0;
    if (conn->primary_device != NULL) {
        zwp_primary_selection_device_v1_destroy(conn->primary_device);
        conn->primary_device = NULL;
    }
    if (conn->primary_manager != NULL) {
        zwp_primary_selection_device_manager_v1_destroy(
            conn->primary_manager);
        conn->primary_manager = NULL;
    }
}

/* ---- the two public ops ---- */

fdk_result fdk_wayland_clipboard_set_text(fdk_platform_connection *conn,
                                          const char *text) {
    if (conn->data_device == NULL) {
        return FDK_ERR_UNSUPPORTED; /* no manager global or no seat */
    }
    if (text == NULL) {
        text = "";
    }
    size_t len = strlen(text);
    char *copy = fdk_alloc(len + 1);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, len + 1);

    struct wl_data_source *source =
        wl_data_device_manager_create_data_source(conn->data_device_manager);
    if (source == NULL) {
        fdk_free(copy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    wl_data_source_add_listener(source, &g_source_listener, conn);
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "text/plain");

    /* Replace any source of ours still outstanding. */
    if (conn->clip_source != NULL) {
        wl_data_source_destroy(conn->clip_source);
    }
    conn->clip_source = source;
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = copy;

    /* Serial: the newest input event's. Before any input (serial 0)
     * compositors may ignore the request — documented, not faked. */
    wl_data_device_set_selection(conn->data_device, source,
                                 conn->last_input_serial);
    wl_display_flush(conn->display);
    return FDK_OK;
}

/* Bounded read of an offer's text: receive into a pipe, then poll +
 * read until EOF or deadline. Returns an fdk_alloc'd string or NULL.
 * `primary` selects which protocol's receive request to send (the
 * transfer mechanics are otherwise identical). */
static char *read_offer_text(fdk_platform_connection *conn, bool primary) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        FDK_WARN("clipboard: pipe2 failed (%s)", strerror(errno));
        return NULL;
    }
    if (primary) {
        zwp_primary_selection_offer_v1_receive(
            conn->primary_selection_offer,
            "text/plain;charset=utf-8", fds[1]);
    } else {
        wl_data_offer_receive(conn->selection_offer,
                              "text/plain;charset=utf-8", fds[1]);
    }
    /* Give the request a chance to reach the compositor and the
     * owner's write to start before we poll. */
    wl_display_flush(conn->display);

    /* Collect into a growing buffer. The 1 MiB cap matches the
     * document-level "no INCR-style giant transfers" policy. */
    size_t cap = 256, len = 0;
    char *buf = fdk_alloc(cap);
    if (buf == NULL) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    struct timespec deadline_ts;
    clock_gettime(CLOCK_MONOTONIC, &deadline_ts);
    uint64_t deadline = (uint64_t)deadline_ts.tv_sec * 1000u +
                        (uint64_t)deadline_ts.tv_nsec / 1000000u +
                        FDK_WL_CLIP_READ_MS;

    int done = 0;
    while (!done) {
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n > 0) {
            len += (size_t)n;
            if (len + 1 == cap) {
                if (cap >= 1024u * 1024u) {
                    FDK_WARN("clipboard: offer larger than 1 MiB — "
                             "truncating read");
                    break;
                }
                char *grown = fdk_realloc(buf, cap * 2);
                if (grown == NULL) {
                    break; /* keep what we have */
                }
                buf = grown;
                cap *= 2;
            }
            continue;
        }
        if (n == 0) {
            done = 1; /* EOF: the owner closed its end */
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break; /* hard error: keep what we have */
        }
        /* Nothing to read right now: bounded wait. */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        uint64_t now = (uint64_t)now_ts.tv_sec * 1000u +
                       (uint64_t)now_ts.tv_nsec / 1000000u;
        if (now >= deadline) {
            FDK_WARN("clipboard: offer read timed out after %d ms",
                     FDK_WL_CLIP_READ_MS);
            break;
        }
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        int r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno != EINTR) {
            break;
        }
        if (r == 0) {
            continue; /* re-check deadline at loop top */
        }
    }
    close(fds[0]);
    close(fds[1]);
    buf[len] = '\0';
    return buf;
}

char *fdk_wayland_clipboard_get_text(fdk_platform_connection *conn) {
    if (conn->data_device == NULL) {
        FDK_WARN("clipboard: no wl_data_device on this compositor");
        return NULL;
    }
    /* We own it: compositors never send a client its own selection,
     * so serve from the local copy. */
    if (conn->clip_source != NULL) {
        if (conn->clip_owned_text == NULL ||
            conn->clip_owned_text[0] == '\0') {
            return NULL;
        }
        size_t len = strlen(conn->clip_owned_text);
        char *copy = fdk_alloc(len + 1);
        if (copy != NULL) {
            memcpy(copy, conn->clip_owned_text, len + 1);
        }
        return copy;
    }
    if (conn->selection_offer == NULL || !conn->selection_offer_has_text) {
        /* Catch up on selection events we may not have dispatched
         * yet — ONE roundtrip, bounded by the connection's own
         * health. Events it delivers go through the normal dispatch
         * path (same as a fdk_run pump; the widget dispatch guard is
         * depth-counted). */
        if (wl_display_roundtrip(conn->display) < 0) {
            return NULL;
        }
        if (conn->selection_offer == NULL || !conn->selection_offer_has_text) {
            return NULL; /* genuinely no text selection */
        }
    }
    char *text = read_offer_text(conn, false);

    /* The offer is single-use per receive in spirit (the spec allows
     * one transfer per offer); drop it so a later get_text forces a
     * fresh look at the compositor's state. */
    wl_data_offer_destroy(conn->selection_offer);
    conn->selection_offer = NULL;
    conn->selection_offer_has_text = 0;
    return text;
}

/* ---- the two PRIMARY ops (1.3.4) ---- */

fdk_result fdk_wayland_clipboard_set_primary_text(
    fdk_platform_connection *conn, const char *text) {
    if (conn->primary_device == NULL) {
        /* No manager global (compositor without the protocol) or no
         * seat yet — either way the honest answer, same as the
         * clipboard's. */
        return FDK_ERR_UNSUPPORTED;
    }
    if (text == NULL) {
        text = "";
    }
    size_t len = strlen(text);
    char *copy = fdk_alloc(len + 1);
    if (copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, len + 1);

    struct zwp_primary_selection_source_v1 *source =
        zwp_primary_selection_device_manager_v1_create_source(
            conn->primary_manager);
    if (source == NULL) {
        fdk_free(copy);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    zwp_primary_selection_source_v1_add_listener(
        source, &g_primary_source_listener, conn);
    zwp_primary_selection_source_v1_offer(
        source, "text/plain;charset=utf-8");
    zwp_primary_selection_source_v1_offer(source, "text/plain");

    /* Replace any source of ours still outstanding (destroying the
     * proxy retires it without a cancelled — same discipline as the
     * clipboard set). */
    if (conn->primary_source != NULL) {
        zwp_primary_selection_source_v1_destroy(conn->primary_source);
    }
    conn->primary_source = source;
    fdk_free(conn->primary_owned_text);
    conn->primary_owned_text = copy;

    /* Same serial contract as the clipboard set: the newest input
     * event's; serial 0 before any input may be ignored. */
    zwp_primary_selection_device_v1_set_selection(
        conn->primary_device, source, conn->last_input_serial);
    wl_display_flush(conn->display);
    return FDK_OK;
}

char *fdk_wayland_clipboard_get_primary_text(
    fdk_platform_connection *conn) {
    if (conn->primary_device == NULL) {
        FDK_WARN("clipboard: no primary-selection device on this "
                 "compositor");
        return NULL;
    }
    /* We own it: serve from the local copy (compositors do not send
     * a client its own selection — and wlroots ECHOES the fresh one
     * back, so the offer slot is not authoritative while we own). */
    if (conn->primary_source != NULL) {
        if (conn->primary_owned_text == NULL ||
            conn->primary_owned_text[0] == '\0') {
            return NULL;
        }
        size_t len = strlen(conn->primary_owned_text);
        char *copy = fdk_alloc(len + 1);
        if (copy != NULL) {
            memcpy(copy, conn->primary_owned_text, len + 1);
        }
        return copy;
    }
    if (conn->primary_selection_offer == NULL ||
        !conn->primary_selection_offer_has_text) {
        /* Catch up on primary events we may not have dispatched yet
         * — ONE roundtrip, same eventual-consistency contract as the
         * clipboard read. */
        if (wl_display_roundtrip(conn->display) < 0) {
            return NULL;
        }
        if (conn->primary_selection_offer == NULL ||
            !conn->primary_selection_offer_has_text) {
            return NULL; /* genuinely no text primary selection */
        }
    }
    char *text = read_offer_text(conn, true);

    /* Single-use per receive in spirit — same as the clipboard
     * offer. A later get forces a fresh look. */
    zwp_primary_selection_offer_v1_destroy(conn->primary_selection_offer);
    conn->primary_selection_offer = NULL;
    conn->primary_selection_offer_has_text = 0;
    return text;
}
