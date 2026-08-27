/* _GNU_SOURCE (defined BEFORE any header): pipe2(). The build's
 * _POSIX_C_SOURCE covers clock_gettime; Xlib's headers incidentally
 * pull both in for the x11 backend, but this file includes none. */
#define _GNU_SOURCE

#define FDK_LOG_TAG "wayland"

/*
 * wayland_clipboard.c — wl_data_device clipboard (Phase 9)
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
 * Honest limitations (mirrored in fdk_clipboard.h): no DnD, no
 * source actions, text only, and set_selection before any input
 * event carries serial 0 which compositors may ignore.
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

    /* An incoming foreign selection implies ours (if any) was
     * replaced — mirror the X11 SelectionClear cleanup. */
    if (conn->clip_source != NULL) {
        wl_data_source_destroy(conn->clip_source);
        conn->clip_source = NULL;
        fdk_free(conn->clip_owned_text);
        conn->clip_owned_text = NULL;
    }
}

/* The rest of wl_data_device is drag-and-drop; required no-ops. */
static void device_enter(void *data, struct wl_data_device *device,
                         uint32_t serial, struct wl_surface *surface,
                         wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *offer) {
    (void)data; (void)device; (void)serial; (void)surface;
    (void)x; (void)y; (void)offer;
}
static void device_leave(void *data, struct wl_data_device *device) {
    (void)data; (void)device;
}
static void device_motion(void *data, struct wl_data_device *device,
                          uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data; (void)device; (void)time; (void)x; (void)y;
}
static void device_drop(void *data, struct wl_data_device *device) {
    (void)data; (void)device;
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
     * source is inert from here — destroy it and drop the text. */
    wl_data_source_destroy(source);
    if (conn->clip_source == source) {
        conn->clip_source = NULL;
    }
    fdk_free(conn->clip_owned_text);
    conn->clip_owned_text = NULL;
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

/* ---- lifecycle wiring ---- */

void fdk_wayland_clipboard_device_ready(fdk_platform_connection *conn) {
    if (conn->data_device != NULL ||
        conn->data_device_manager == NULL || conn->seat == NULL) {
        return; /* already up, or still missing a prerequisite */
    }
    conn->data_device =
        wl_data_device_manager_get_data_device(conn->data_device_manager,
                                                conn->seat);
    if (conn->data_device == NULL) {
        FDK_WARN("clipboard: get_data_device failed");
        return;
    }
    wl_data_device_add_listener(conn->data_device, &g_device_listener, conn);
    /* The compositor sends the CURRENT selection to a fresh data
     * device; the roundtrip in the caller's initial sync (or the next
     * dispatch) delivers it into conn->selection_offer. */
}

void fdk_wayland_clipboard_teardown(fdk_platform_connection *conn) {
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

/* Bounded read of the offer's text: receive into a pipe, then poll +
 * read until EOF or deadline. Returns an fdk_alloc'd string or NULL. */
static char *read_offer_text(fdk_platform_connection *conn) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        FDK_WARN("clipboard: pipe2 failed (%s)", strerror(errno));
        return NULL;
    }
    wl_data_offer_receive(conn->selection_offer,
                          "text/plain;charset=utf-8", fds[1]);
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
    char *text = read_offer_text(conn);

    /* The offer is single-use per receive in spirit (the spec allows
     * one transfer per offer); drop it so a later get_text forces a
     * fresh look at the compositor's state. */
    wl_data_offer_destroy(conn->selection_offer);
    conn->selection_offer = NULL;
    conn->selection_offer_has_text = 0;
    return text;
}
