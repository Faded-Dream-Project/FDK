/* _GNU_SOURCE BEFORE every include: pipe2() (same pattern as
 * wayland_clipboard.c). */
#define _GNU_SOURCE

#define FDK_LOG_TAG "wayland"

/*
 * wayland_dnd.c — wl_data_device drag and drop (1.2.0)
 *
 * RECEIVER: a drag arriving at FDK is a sequence on the wl_data_device
 * we already own for the clipboard: ::data_offer announces the drag
 * offer and its MIME types (the pending slot in wayland_clipboard.c
 * accumulates them), then ::enter names the surface + position, then
 * ::motion / ::leave / ::drop. Enter is where acceptance happens: the
 * intersection of the offer's formats and the window's registered mask
 * decides whether FDK answers wl_data_offer.set_actions(copy) — the
 * compositor's cursor feedback and whether ::drop ever arrives follow
 * that answer. On drop FDK receives the preferred MIME over a pipe
 * (the same bounded read the clipboard uses), decodes the uri-list to
 * POSIX paths (the shared codec), delivers FDK_EVENT_DRAG_DROP, then
 * finishes and destroys the offer.
 *
 * SOURCE: fdk_wayland_drag_begin creates a wl_data_source offering
 * text/plain;charset=utf-8 and/or text/uri-list, then calls
 * wl_data_device.start_drag with NO icon surface (legal per protocol;
 * compositors show a default) citing the newest BUTTON serial — the
 * compositor owns the whole drag from there. Our data_source events
 * are the whole story: ::send writes the payload to the target's
 * pipe, ::dnd_drop_performed marks the drop, ::dnd_finished reports
 * SUCCEEDED, ::cancelled reports CANCELLED (both exactly once).
 *
 * Honest limitations (mirrored in fdk_dnd.h): COPY action only; no
 * ask/delete negotiation; no icon surface.
 */

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/mman.h> /* the icon surface's SHM upload (S-9) */
#include <time.h>
#include <unistd.h>

#define FDK_WL_DND_READ_MS 250

static char *wl_strdup(const char *s) {
    size_t len = strlen(s);
    char *out = fdk_alloc(len + 1);
    if (out != NULL) {
        memcpy(out, s, len + 1);
    }
    return out;
}

/* ---- receiver: the data_device drag events (called from the
 * listener stubs in wayland_clipboard.c) ---- */

static void dnd_dispatch(fdk_platform_window *pwindow, fdk_event_type type,
                         double x, double y, int offered, int accepted,
                         const char *text, char **uris, size_t uri_count) {
    fdk_event_data ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.drag.offered_formats = offered;
    ev.drag.accepted_formats = accepted;
    ev.drag.position.x = (fdk_f32)x;
    ev.drag.position.y = (fdk_f32)y;
    ev.drag.text = text;
    ev.drag.uris = uris;
    ev.drag.uri_count = uri_count;
    pwindow->conn->dispatch(pwindow, &ev,
                            pwindow->conn->dispatch_user_data);
}

/* Bounded pipe read of the drop payload (same shape as the clipboard's
 * read_offer_text, parameterized by offer + mime). fdk_alloc'd or
 * NULL. */
static char *dnd_read_offer(fdk_platform_connection *conn,
                            struct wl_data_offer *offer, const char *mime) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        return NULL;
    }
    wl_data_offer_receive(offer, mime, fds[1]);
    /* Close OUR write end immediately: the compositor dups its own
     * copy out of the request, so ours serves no purpose past this
     * point — and holding it open means EOF can NEVER arrive on the
     * read end (every drop read used to sit out its full 250 ms
     * budget with the complete payload already buffered, waiting for
     * an EOF our own fd made impossible; the writer's close — the
     * external peer's or our own ::send handler's — is now the only
     * close left, and EOF actually happens). */
    close(fds[1]);

    size_t cap = 256, len = 0;
    char *buf = fdk_alloc(cap);
    if (buf == NULL) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline = (uint64_t)ts.tv_sec * 1000u +
                        (uint64_t)ts.tv_nsec / 1000000u +
                        FDK_WL_DND_READ_MS;
    int done = 0;
    while (!done) {
        /* Serve anything already on the display socket FIRST (the
         * same-connection ::send — see the poll below): the loop's
         * read then finds the payload already in the pipe. */
        if (fdk_wayland_dispatch_pending(conn) < 0) {
            break; /* connection error: keep what we have */
        }
        ssize_t n = read(fds[0], buf + len, cap - len - 1);
        if (n > 0) {
            len += (size_t)n;
            if (len + 1 == cap) {
                if (cap >= 1024u * 1024u) {
                    FDK_WARN("dnd: drop payload over 1 MiB — truncated");
                    break;
                }
                char *grown = fdk_realloc(buf, cap * 2);
                if (grown == NULL) {
                    break;
                }
                buf = grown;
                cap *= 2;
            }
            continue;
        }
        if (n == 0) {
            done = 1;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u +
                       (uint64_t)ts.tv_nsec / 1000000u;
        if (now >= deadline) {
            FDK_WARN("dnd: drop payload read timed out (%d ms), "
                     "%zu bytes kept",
                     FDK_WL_DND_READ_MS, len);
            break;
        }
        /* SAME-CONNECTION drags (1.1.0, S-9): when the dragging
         * client and the dropping client are ONE process — an app
         * dragging between its own windows, exactly what the sway
         * suite's icon leg does — the ::send that fills this pipe is
         * addressed to OUR OWN wl_data_source. It arrives on the
         * DISPLAY socket, so the wait must poll BOTH fds: the pipe
         * (an external writer) and the display (our own ::send,
         * which only fills the pipe once the queue is dispatched —
         * the canonical prepare_read/read/dispatch helper at the top
         * of the next iteration serves it). A pipe-only poll would
         * stare at an empty pipe while the ::send sits in the kernel
         * buffer: the timeout this loop used to hit on a perfectly
         * legal drag.
         *
         * Re-entrancy: the compositor's post-drop ::leave /
         * ::cancelled may ride along; the drag_drop_active fence
         * (set by our caller) keeps their handlers from tearing the
         * drag state down under this read — the fence defers the
         * source-side report to the drop tail. Everything else
         * served here is ordinary frame/configure traffic through
         * the guarded dispatch path the clipboard reads share. */
        struct pollfd pfds[2] = {
            { fds[0], POLLIN, 0 },
            { wl_display_get_fd(conn->display), POLLIN, 0 },
        };
        int r = poll(pfds, 2, (int)(deadline - now));
        if (r < 0 && errno != EINTR) {
            break;
        }
    }
    close(fds[0]);
    /* fds[1] was closed at the receive request (see the comment
     * there) — a second close here could hit a REUSED descriptor
     * number; only the read end remains to close. */
    buf[len] = '\0';
    return buf;
}

void fdk_wayland_dnd_device_enter(fdk_platform_connection *conn,
                                  uint32_t serial, struct wl_surface *surface,
                                  wl_fixed_t x, wl_fixed_t y,
                                  struct wl_data_offer *offer) {
    (void)serial;
    /* The pending offer accumulated its MIME ::offer events before
     * this enter (protocol ordering). Map to the FDK format mask. */
    int offered = 0;
    if (conn->pending_offer == offer) {
        if (conn->pending_offer_has_text) {
            offered |= FDK_DRAG_FORMAT_TEXT;
        }
        if (conn->pending_offer_has_uris) {
            offered |= FDK_DRAG_FORMAT_URI_LIST;
        }
    }
    conn->drag_offer = offer;
    conn->drag_offered = offered;
    conn->drag_hover = NULL;

    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->surface == surface) {
            conn->drag_hover = conn->windows[i];
            break;
        }
    }
    if (conn->drag_hover == NULL) {
        return;
    }
    /* Per-position acceptance (1.1.0, S-9): the widget tree's drop
     * targets own the answer under the pointer (the core layer's
     * hit-test + ancestor walk; the window's mask where no target
     * owns the position). */
    int accepted = fdk__core_drag_accepts_at(
        conn->dispatch_user_data, conn->drag_hover, offered,
        (int)wl_fixed_to_double(x), (int)wl_fixed_to_double(y));
    if (accepted == 0) {
        /* No set_actions answer: the compositor reads no matching
         * action and the drag stays "not here" — the honest reject. */
        return;
    }
    /* Accept with COPY (manager bound at version 3, so set_actions
     * exists). Without this call compositors refuse to deliver drop. */
    wl_data_offer_set_actions(offer,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    /* AND the legacy accept: wlroots only reports the drag as
     * ACCEPTED (drop-able) when the offer received an explicit
     * accept(mime) — set_actions alone negotiates the action but
     * leaves source->accepted unset, and a release over the window
     * then CANCELS the drag instead of dropping (the sway rig
     * lesson: set_actions + action(1) agreed, button release still
     * produced selection(nil) and no ::drop until this was added).
     * accept dropped its timestamp argument in version 3. */
    if (accepted & FDK_DRAG_FORMAT_URI_LIST) {
        wl_data_offer_accept(offer, serial, "text/uri-list");
    } else {
        wl_data_offer_accept(offer, serial, "text/plain;charset=utf-8");
    }
    conn->drag_accepted = accepted;
    conn->drag_x = wl_fixed_to_double(x);
    conn->drag_y = wl_fixed_to_double(y);
    dnd_dispatch(conn->drag_hover, FDK_EVENT_DRAG_ENTER,
                 conn->drag_x, conn->drag_y, offered, accepted, NULL,
                 NULL, 0);
}

void fdk_wayland_dnd_device_motion(fdk_platform_connection *conn,
                                   uint32_t time, wl_fixed_t x,
                                   wl_fixed_t y) {
    (void)time;
    if (conn->drag_hover == NULL || conn->drag_offer == NULL) {
        return;
    }
    conn->drag_x = wl_fixed_to_double(x);
    conn->drag_y = wl_fixed_to_double(y);
    /* S-9: re-evaluate the accepted mask at the new position and
     * refresh the protocol accept — wl_data_offer.accept is DESIGNED
     * for exactly this ("sent by the target during a drag, when the
     * pointer moves"), and wlroots re-reads source->accepted from
     * it. Crossing from a TEXT target into a URI target mid-drag
     * must flip the source's cursor feedback, exactly like X11's
     * per-Position Status replies. */
    int accepted = fdk__core_drag_accepts_at(
        conn->dispatch_user_data, conn->drag_hover, conn->drag_offered,
        (int)conn->drag_x, (int)conn->drag_y);
    if (accepted != conn->drag_accepted) {
        if (accepted == 0) {
            /* Refused at this position: withdraw the accept (a NULL
             * mime clears it) and the set_actions answer. */
            wl_data_offer_accept(conn->drag_offer,
                                 conn->last_input_serial, NULL);
        } else if (accepted & FDK_DRAG_FORMAT_URI_LIST) {
            wl_data_offer_accept(conn->drag_offer,
                                 conn->last_input_serial,
                                 "text/uri-list");
        } else {
            wl_data_offer_accept(conn->drag_offer,
                                 conn->last_input_serial,
                                 "text/plain;charset=utf-8");
        }
        conn->drag_accepted = accepted;
    }
    dnd_dispatch(conn->drag_hover, FDK_EVENT_DRAG_MOTION,
                 conn->drag_x, conn->drag_y, conn->drag_offered,
                 conn->drag_accepted, NULL, NULL, 0);
}

void fdk_wayland_dnd_device_leave(fdk_platform_connection *conn) {
    if (conn->drag_drop_active) {
        /* Post-drop noise served by the payload read's dispatch (the
         * S-9 fence): the drop handler's tail owns the state now —
         * destroying the offer HERE would free the proxy under the
         * read that is still polling its pipe. */
        return;
    }
    if (conn->drag_hover != NULL) {
        dnd_dispatch(conn->drag_hover, FDK_EVENT_DRAG_LEAVE, 0, 0,
                     conn->drag_offered, 0, NULL, NULL, 0);
    }
    conn->drag_hover = NULL;
    conn->drag_offer = NULL;
    conn->drag_accepted = 0;
    /* The drag offer dies with the drag (leave means no drop); the
     * pending slot clears so a stale offer is never reused. */
    if (conn->pending_offer != NULL &&
        conn->pending_offer != conn->selection_offer) {
        wl_data_offer_destroy(conn->pending_offer);
    }
    conn->pending_offer = NULL;
    conn->pending_offer_has_text = 0;
    conn->pending_offer_has_uris = 0;
}

static void drag_source_report(fdk_platform_connection *conn, int status);

void fdk_wayland_dnd_device_drop(fdk_platform_connection *conn) {
    if (conn->drag_hover == NULL || conn->drag_offer == NULL) {
        return;
    }
    fdk_platform_window *pwindow = conn->drag_hover;
    struct wl_data_offer *offer = conn->drag_offer;
    /* S-9: the fence — the payload read dispatches the queue (the
     * same-connection case), and the post-drop ::leave / ::cancelled
     * it may serve must not tear this very drag down mid-read. */
    conn->drag_drop_active = 1;
    conn->drag_deferred_report = 0;
    /* S-9: the mask at the RELEASE position — the data actually read
     * (and the event's accepted_formats) must match what the target
     * under the pointer accepts, not what some earlier enter did. */
    int accepted = fdk__core_drag_accepts_at(
        conn->dispatch_user_data, pwindow, conn->drag_offered,
        (int)conn->drag_x, (int)conn->drag_y);
    fdk_drag_event payload = {
        .offered_formats = conn->drag_offered,
        .accepted_formats = accepted,
        .position = { (fdk_f32)conn->drag_x, (fdk_f32)conn->drag_y },
    };

    char *raw = NULL;
    if (accepted & FDK_DRAG_FORMAT_URI_LIST) {
        raw = dnd_read_offer(conn, offer, "text/uri-list");
    } else if (accepted & FDK_DRAG_FORMAT_TEXT) {
        raw = dnd_read_offer(conn, offer, "text/plain;charset=utf-8");
    }

    char **uris = NULL;
    size_t uri_count = 0;
    char *text = NULL;
    if (raw != NULL) {
        if (accepted & FDK_DRAG_FORMAT_URI_LIST) {
            (void)fdk__dnd_parse_uri_list(raw, strlen(raw), &uris,
                                          &uri_count);
            if (uris == NULL) {
                text = wl_strdup("");
            }
        } else {
            text = raw;
            raw = NULL;
        }
    }

    fdk_event_data ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = FDK_EVENT_DRAG_DROP;
    ev.drag = payload;
    ev.drag.text = text;
    ev.drag.uris = uris;
    ev.drag.uri_count = uri_count;
    conn->dispatch(pwindow, &ev, conn->dispatch_user_data);

    fdk__dnd_free_uri_list(uris, uri_count);
    fdk_free(text);
    fdk_free(raw);

    wl_data_offer_finish(offer); /* v3: the transfer is complete */
    wl_data_offer_destroy(offer);
    if (conn->pending_offer == offer) {
        conn->pending_offer = NULL;
        conn->pending_offer_has_text = 0;
        conn->pending_offer_has_uris = 0;
    }
    conn->drag_offer = NULL;
    conn->drag_hover = NULL;
    conn->drag_accepted = 0;
    conn->drag_drop_active = 0;
    if (conn->drag_deferred_report) {
        /* The source-side end event served mid-read: run the report
         * NOW, with the state consistent (the plain dispatch path's
         * behavior, one pump later). */
        int status = conn->drag_deferred_status;
        conn->drag_deferred_report = 0;
        drag_source_report(conn, status);
    }
}

/* ---- source ---- */

static void drag_source_target(void *data, struct wl_data_source *source,
                               const char *mime_type) {
    (void)data; (void)source; (void)mime_type;
}

static void drag_source_send(void *data, struct wl_data_source *source,
                             const char *mime_type, int32_t fd) {
    (void)source;
    fdk_platform_connection *conn = data;
    const char *payload = NULL;
    if (strcmp(mime_type, "text/uri-list") == 0) {
        payload = conn->drag_uri_payload;
    } else if (strcmp(mime_type, "text/plain;charset=utf-8") == 0 ||
               strcmp(mime_type, "text/plain") == 0) {
        payload = conn->drag_text;
    }
    if (payload == NULL) {
        close(fd);
        return;
    }
    size_t len = strlen(payload);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, payload + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        off += (size_t)n;
    }
    close(fd);
}

/* Test seam (the fdk__x11_gl_test_reset precedent — internal symbol,
 * declared extern by the integration test): the number of drag-icon
 * surfaces currently live. The sway suite asserts 0 -> 1 (drag
 * begin) -> 0 (drag end) through it. */
int fdk__wl_drag_icon_live = 0;

/* The drag icon's teardown (S-9): surface first, one roundtrip so
 * the compositor releases its buffer reference, THEN the buffer
 * object — destroying the buffer before the compositor is done with
 * it is the use-after-free the roundtrip closes. The client-side SHM
 * mapping is already gone (the cursor-upload rule). */
static void drag_icon_destroy(fdk_platform_connection *conn) {
    if (conn->drag_icon != NULL) {
        fdk__wl_drag_icon_live--;
    }
    if (conn->drag_icon == NULL) {
        return;
    }
    if (!conn->display_dead) {
        wl_surface_destroy(conn->drag_icon);
        (void)wl_display_roundtrip(conn->display);
        if (conn->drag_icon_buffer != NULL) {
            wl_buffer_destroy(conn->drag_icon_buffer);
        }
    }
    conn->drag_icon = NULL;
    conn->drag_icon_buffer = NULL;
}

static void drag_source_report(fdk_platform_connection *conn, int status) {
    void (*on_done)(int, void *) = conn->drag_on_done;
    void *user = conn->drag_on_done_user;
    conn->drag_on_done = NULL;
    conn->drag_on_done_user = NULL;
    if (conn->drag_source != NULL) {
        wl_data_source_destroy(conn->drag_source);
        conn->drag_source = NULL;
    }
    drag_icon_destroy(conn);
    if (on_done != NULL) {
        on_done(status, user);
    }
}

static void drag_source_cancelled(void *data,
                                  struct wl_data_source *source) {
    (void)source;
    fdk_platform_connection *conn = data;
    if (conn->drag_drop_active) {
        /* Served mid-drop-read (the S-9 fence): defer — the teardown
         * (source + icon destroy, on_done) runs from the drop tail,
         * where the state is consistent. */
        conn->drag_deferred_report = 1;
        conn->drag_deferred_status = FDK_DRAG_CANCELLED;
        return;
    }
    drag_source_report(conn, FDK_DRAG_CANCELLED);
}

static void drag_source_dnd_drop_performed(void *data,
                                           struct wl_data_source *source) {
    (void)source;
    fdk_platform_connection *conn = data;
    conn->drag_drop_performed = 1;
}

static void drag_source_dnd_finished(void *data,
                                     struct wl_data_source *source) {
    (void)source;
    fdk_platform_connection *conn = data;
    if (conn->drag_drop_active) {
        conn->drag_deferred_report = 1;
        conn->drag_deferred_status = FDK_DRAG_SUCCEEDED;
        return;
    }
    drag_source_report(conn, FDK_DRAG_SUCCEEDED);
}

static void drag_source_action(void *data, struct wl_data_source *source,
                               uint32_t dnd_action) {
    (void)data; (void)source; (void)dnd_action;
}

static const struct wl_data_source_listener g_drag_source_listener = {
    .target = drag_source_target,
    .send = drag_source_send,
    .cancelled = drag_source_cancelled,
    .dnd_drop_performed = drag_source_dnd_drop_performed,
    .dnd_finished = drag_source_dnd_finished,
    .action = drag_source_action,
};

void fdk_wayland_dnd_teardown(fdk_platform_connection *conn) {
    if (conn->drag_source != NULL) {
        drag_source_report(conn, FDK_DRAG_CANCELLED);
    }
    drag_icon_destroy(conn);
    fdk_free(conn->drag_text);
    conn->drag_text = NULL;
    fdk_free(conn->drag_uri_payload);
    conn->drag_uri_payload = NULL;
    conn->drag_offer = NULL;
    conn->drag_hover = NULL;
}

/* ---- the two ops ---- */

void fdk_wayland_window_set_drop_formats(fdk_platform_window *pwindow,
                                         int formats) {
    pwindow->drop_formats = formats;
    /* Wayland acceptance is answered at the drag's ENTER via
     * set_actions; nothing to send now. */
}

/* The drag icon surface (S-9): ARGB8888 into a dedicated SHM pool —
 * the cursor-upload shape exactly (flush-while-fd-open, munmap after
 * pool creation, the server keeps its own mapping). The surface is
 * committed BEFORE start_drag (the compositor wants a content-carrying
 * icon) and rides the DEFAULT queue: icon buffers carry no listener,
 * so any release event the compositor sends is dispatched-and-dropped
 * by the main loop. Returns the surfaced icon or NULL (the drag then
 * proceeds WITHOUT one — feedback, not function). */
static struct wl_surface *drag_icon_create(fdk_platform_connection *conn,
                                           const fdk_u32 *icon, int w,
                                           int h) {
    size_t stride = (size_t)w * 4u;
    size_t length = stride * (size_t)h;
    int fd = memfd_create("fdk-drag-icon", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)length) < 0) {
        close(fd);
        return NULL;
    }
    void *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memcpy(map, icon, length);
    struct wl_shm_pool *pool =
        wl_shm_create_pool(conn->shm, fd, (int32_t)length);
    /* FD LIFETIME: the cursor-upload rule — the create_pool request
     * resolves fd at flush time by number, so flush while it is open. */
    (void)wl_display_flush(conn->display);
    close(fd);
    if (pool == NULL) {
        munmap(map, length);
        return NULL;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, length); /* the server keeps its own mapping */
    if (buffer == NULL) {
        return NULL;
    }

    struct wl_surface *surf =
        wl_compositor_create_surface(conn->compositor);
    if (surf == NULL) {
        wl_buffer_destroy(buffer);
        return NULL;
    }
    /* No input region: the icon never takes pointer input (a
     * compositor would not route to it anyway, but the surface
     * input region is client-side truth — set it empty). */
    struct wl_region *empty = wl_compositor_create_region(conn->compositor);
    wl_surface_set_input_region(surf, empty);
    wl_region_destroy(empty);

    wl_surface_attach(surf, buffer, 0, 0);
    wl_surface_damage(surf, 0, 0, (int32_t)w, (int32_t)h);
    wl_surface_commit(surf);
    conn->drag_icon = surf;
    conn->drag_icon_buffer = buffer;
    fdk__wl_drag_icon_live++;
    return surf;
}

fdk_result fdk_wayland_drag_begin(fdk_platform_window *origin, int formats,
                                  const char *text,
                                  const char *const *uris, size_t uri_count,
                                  const fdk_u32 *icon, int icon_w,
                                  int icon_h,
                                  void (*on_done)(int, void *), void *user) {
    fdk_platform_connection *conn = origin->conn;
    if (conn->data_device == NULL || conn->data_device_manager == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (conn->drag_source != NULL) {
        return FDK_ERR_PLATFORM; /* one drag at a time */
    }
    if (conn->last_button_serial == 0) {
        /* start_drag must cite the serial of a press; zero means we
         * have seen no input — the compositor would refuse (the
         * documented stale-serial limitation, refused up front). */
        FDK_WARN("dnd: no button serial yet — the compositor would "
                 "reject start_drag; refusing honestly");
        return FDK_ERR_PLATFORM;
    }

    char *text_copy = NULL;
    char *uri_payload = NULL;
    if (formats & FDK_DRAG_FORMAT_TEXT) {
        text_copy = wl_strdup(text != NULL ? text : "");
        if (text_copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    if (formats & FDK_DRAG_FORMAT_URI_LIST) {
        uri_payload = fdk__dnd_build_uri_list(uris, uri_count);
        if (uri_payload == NULL) {
            fdk_free(text_copy);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }

    struct wl_data_source *source =
        wl_data_device_manager_create_data_source(conn->data_device_manager);
    if (source == NULL) {
        fdk_free(text_copy);
        fdk_free(uri_payload);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    wl_data_source_add_listener(source, &g_drag_source_listener, conn);
    if (formats & FDK_DRAG_FORMAT_URI_LIST) {
        wl_data_source_offer(source, "text/uri-list");
    }
    if (formats & FDK_DRAG_FORMAT_TEXT) {
        wl_data_source_offer(source, "text/plain;charset=utf-8");
        wl_data_source_offer(source, "text/plain");
    }

    conn->drag_source = source;
    conn->drag_text = text_copy;
    conn->drag_uri_payload = uri_payload;
    conn->drag_drop_performed = 0;
    conn->drag_on_done = on_done;
    conn->drag_on_done_user = user;

    /* The icon surface (S-9): committed BEFORE start_drag so the
     * compositor receives a content-carrying icon. Best-effort — a
     * failed upload logs nothing and the drag runs icon-less. */
    struct wl_surface *icon_surf = NULL;
    if (icon != NULL && icon_w > 0 && icon_h > 0 && conn->shm != NULL) {
        icon_surf = drag_icon_create(conn, icon, icon_w, icon_h);
    }

    /* The button serial (not just any input serial): start_drag is
     * required to cite the press that began the implicit grab. NULL
     * icon surface is protocol-legal (compositors draw a default). */
    wl_data_device_start_drag(conn->data_device, source,
                              origin->surface, icon_surf,
                              conn->last_button_serial);
    wl_display_flush(conn->display);
    return FDK_OK;
}
