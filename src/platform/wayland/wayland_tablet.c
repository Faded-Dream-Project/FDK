/*
 * wayland_tablet.c — the zwp_tablet_v2 half of FDK's tablet model
 * (1.1.0, D-3 / B-17)
 *
 * Drawing-tablet input through the tablet protocol family: the
 * manager (bound when the compositor offers it), the per-seat
 * tablet seat, and the per-TOOL event stream. Everything FDK
 * consumes rides the TOOL (proximity, down/up, motion with pressure
 * and tilt, buttons); TABLETS (the physical slabs) and PADS (the
 * button strips/rings) are tracked only far enough to keep the
 * protocol objects alive — no fdk_event carries pad state (the
 * drawing input is the feature; pad programmability is a separate
 * concern with no consumer in this toolkit yet, an honest gap the
 * D-030 decision records rather than a half-API).
 *
 * The translation is 1:1 with the touch model's discipline: plain-
 * argument static bodies (proximity_in_event, motion_event, ...)
 * so the test seam drives the REAL paths — sway headless binds the
 * manager but exposes no virtual-tablet protocol (wlroots has none,
 * the touch situation verbatim), so the listener bodies are the
 * rig's only reachable surface.
 *
 * Ownership: tools/tablets/pads are COMPOSITOR-created objects with
 * client-side records here; "removed" events destroy the proxies
 * (never ours to destroy otherwise); the whole set dies with the
 * connection (the manager's destroy, the tablet seat's destroy).
 */

#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "generated/tablet-unstable-v2-client-protocol.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- dispatch helpers -----------------------------------------------------
 *
 * The tool record (struct fdk_wl_tablet_tool) lives in
 * wayland_platform.h — the connection owns the table. The proxy is
 * compositor-created; the record carries the translation state: the
 * type (from the tool's `type` event), the proximity target (for
 * OUT, which names no surface), the id applications see, and the
 * last position (UP wants the last known coordinates; OUT zeros). */

static void tablet_fill_common(fdk_event_data *event,
                               struct fdk_wl_tablet_tool *tool) {
    event->tablet.tool = tool->id;
    event->tablet.tool_type = tool->type;
    event->tablet.pressure = tool->pressure;
    event->tablet.tilt_x = tool->tilt_x;
    event->tablet.tilt_y = tool->tilt_y;
    event->tablet.position = tool->last;
}

/* PROXIMITY_IN: the tool entered range over this window's surface.
 * Coordinates are unknown until the first motion (the protocol's
 * proximity_in carries none) — zeros, pressure 0. */
static int tablet_proximity_in_event(fdk_platform_connection *conn,
                                     struct fdk_wl_tablet_tool *tool,
                                     struct wl_surface *surface) {
    if (tool == NULL) {
        return 0;
    }
    fdk_platform_window *win = NULL;
    for (size_t i = 0; i < conn->window_count; i++) {
        if (conn->windows[i]->surface == surface) {
            win = conn->windows[i];
            break;
        }
    }
    if (win == NULL) {
        return 0; /* not ours: a subsurface the add-on owns, or a
                   * surface already gone */
    }
    tool->prox = win;
    tool->last.x = 0.0f;
    tool->last.y = 0.0f;
    tool->pressure = 0.0f;
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_TABLET_PROXIMITY_IN;
    tablet_fill_common(&event, tool);
    conn->dispatch(win, &event, conn->dispatch_user_data);
    return 1;
}

/* PROXIMITY_OUT: the protocol names no surface — the record's. */
static int tablet_proximity_out_event(fdk_platform_connection *conn,
                                      struct fdk_wl_tablet_tool *tool) {
    if (tool == NULL || tool->prox == NULL) {
        return 0;
    }
    fdk_platform_window *win = tool->prox;
    tool->prox = NULL;
    tool->last.x = 0.0f;
    tool->last.y = 0.0f;
    tool->pressure = 0.0f;
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_TABLET_PROXIMITY_OUT;
    tablet_fill_common(&event, tool);
    conn->dispatch(win, &event, conn->dispatch_user_data);
    return 1;
}

static int tablet_button_event(fdk_platform_connection *conn,
                               struct fdk_wl_tablet_tool *tool,
                               uint32_t button, uint32_t state) {
    if (tool == NULL || tool->prox == NULL) {
        return 0;
    }
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    event.type = FDK_EVENT_TABLET_BUTTON;
    tablet_fill_common(&event, tool);
    event.tablet.button =
        button | ((state != 0) ? 0u : FDK_TABLET_BUTTON_RELEASE_BIT);
    conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
    return 1;
}

/* ---- tool lookup --------------------------------------------------------- */

static struct fdk_wl_tablet_tool *tool_for_proxy(
    fdk_platform_connection *conn, struct zwp_tablet_tool_v2 *proxy) {
    for (int i = 0; i < FDK_WL_TABLET_TOOLS; i++) {
        if (conn->tablet_tools[i].proxy == proxy) {
            return &conn->tablet_tools[i];
        }
    }
    return NULL;
}

/* ---- the listeners -------------------------------------------------------- */

/* The tool's identity burst: the seat's tool_added created the
 * proxy (the listener attach happened there); the TYPE arrives as
 * the tool's own first events, before `done`. zwp_tablet_v2's type
 * enum and FDK's are 1:1 by design (both 1..8 in the same order). */
static void tool_type(void *data, struct zwp_tablet_tool_v2 *proxy,
                      uint32_t tool_type) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool != NULL) {
        tool->type = (tool_type >= 1 && tool_type <= 8) ? tool_type : 0;
    }
}

static void tool_hardware_serial(void *data,
                                 struct zwp_tablet_tool_v2 *proxy,
                                 uint32_t hi, uint32_t lo) {
    (void)data; (void)proxy; (void)hi; (void)lo; /* the FDK tool id is
                                                  * the table slot */
}

static void tool_hardware_id_wacom(void *data,
                                   struct zwp_tablet_tool_v2 *proxy,
                                   uint32_t hi, uint32_t lo) {
    (void)data; (void)proxy; (void)hi; (void)lo;
}

static void tool_capability(void *data, struct zwp_tablet_tool_v2 *proxy,
                            uint32_t capability) {
    (void)data; (void)proxy; (void)capability; /* tilt/pressure/
                                                * distance capability
                                                * hints — the events
                                                * speak for themselves */
}

static void tool_done(void *data, struct zwp_tablet_tool_v2 *proxy) {
    (void)data; (void)proxy; /* the identity burst's delimiter */
}

static void tool_removed(void *data, struct zwp_tablet_tool_v2 *proxy) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* A tool removed mid-proximity gets its OUT first (the record's
     * window — the protocol sends no proximity_out for removal). */
    (void)tablet_proximity_out_event(conn, tool);
    tool->proxy = NULL;
    tool->id = 0;
}

static void tool_proximity_in(void *data,
                              struct zwp_tablet_tool_v2 *proxy,
                              uint32_t serial,
                              struct zwp_tablet_v2 *tablet,
                              struct wl_surface *surface) {
    (void)serial;
    (void)tablet;
    fdk_platform_connection *conn = data;
    (void)tablet_proximity_in_event(conn, tool_for_proxy(conn, proxy),
                                    surface);
}

static void tool_proximity_out(void *data,
                               struct zwp_tablet_tool_v2 *proxy) {
    fdk_platform_connection *conn = data;
    (void)tablet_proximity_out_event(conn, tool_for_proxy(conn, proxy));
}

static void tool_down(void *data, struct zwp_tablet_tool_v2 *proxy,
                      uint32_t serial) {
    (void)serial;
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* zwp_tablet_tool_v2.down carries NO coordinates — the tip is at
     * the last motion position, which the record already holds (the
     * protocol guarantees a motion between proximity_in and down on
     * every real tablet). The transition is batched like everything
     * else: the FRAME commits it (a real compositor sends
     * down(+motion) then frame). */
    conn->tablet_pending_down = 1;
}

static void tool_up(void *data, struct zwp_tablet_tool_v2 *proxy) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* Same batched shape as down: no coordinates, frame-committed. */
    conn->tablet_pending_up = 1;
}

static void tool_motion(void *data, struct zwp_tablet_tool_v2 *proxy,
                        wl_fixed_t x, wl_fixed_t y) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* wl_fixed surface-local (already logical once unwrapped — the
     * touch model's note). Batched into the record; the FRAME
     * commits the motion event. Pressure rides its own event. */
    tool->last.x = (fdk_f32)wl_fixed_to_double(x);
    tool->last.y = (fdk_f32)wl_fixed_to_double(y);
    conn->tablet_pending_motion = 1;
}

static void tool_pressure(void *data, struct zwp_tablet_tool_v2 *proxy,
                          uint32_t pressure) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* A pressure-only change is still a pending MOTION (the event
     * family has no separate pressure event; the compositor may
     * send pressure without x/y). */
    tool->pressure = (fdk_f32)(pressure / 65535.0);
    conn->tablet_pending_motion = 1;
}

static void tool_distance(void *data, struct zwp_tablet_tool_v2 *proxy,
                          uint32_t distance) {
    (void)data;
    (void)proxy;
    (void)distance; /* hover height: no fdk_event carries it yet */
}

static void tool_tilt(void *data, struct zwp_tablet_tool_v2 *proxy,
                      wl_fixed_t tilt_x, wl_fixed_t tilt_y) {
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* Degrees from vertical, wl_fixed on the wire. */
    tool->tilt_x = (fdk_f32)wl_fixed_to_double(tilt_x);
    tool->tilt_y = (fdk_f32)wl_fixed_to_double(tilt_y);
    conn->tablet_pending_motion = 1;
}

static void tool_rotation(void *data, struct zwp_tablet_tool_v2 *proxy,
                          wl_fixed_t degrees) {
    (void)data;
    (void)proxy;
    (void)degrees; /* z-rotation of airbrush/mouse tools: not carried */
}

static void tool_slider(void *data, struct zwp_tablet_tool_v2 *proxy,
                        int32_t position) {
    (void)data;
    (void)proxy;
    (void)position; /* the airbrush's finger wheel: not carried */
}

static void tool_wheel(void *data, struct zwp_tablet_tool_v2 *proxy,
                       wl_fixed_t degrees, int32_t clicks) {
    (void)data;
    (void)proxy;
    (void)degrees;
    (void)clicks; /* the mouse tool's wheel: POINTER_SCROLL territory */
}

static void tool_button(void *data, struct zwp_tablet_tool_v2 *proxy,
                        uint32_t serial, uint32_t button,
                        uint32_t state) {
    (void)serial;
    fdk_platform_connection *conn = data;
    (void)tablet_button_event(conn, tool_for_proxy(conn, proxy), button,
                              state);
}

static void tool_frame(void *data, struct zwp_tablet_tool_v2 *proxy,
                       uint32_t time) {
    /* zwp_tablet_v2's batching: motion/pressure/tilt/down/up arrive
     * as individual events but the FRAME is the commit delimiter —
     * a real compositor sends (motion, pressure, tilt...) then
     * frame, and a press is (down, frame). FDK dispatches each
     * contact event as it arrives for TOUCH (order-driven core
     * engine); tablets have NO core engine between the listener and
     * the app — so the batched shape is translated HERE: the frame
     * listener is where a DOWN/UP (coordinate-less, above) actually
     * fires, at the position the same frame's motion events already
     * recorded.
     *
     * The accumulated-batch fields (motion's x/y/pressure, the tilt
     * events) ride the connection's pending record — the simplest
     * correct shape, since the protocol may interleave them freely
     * within one frame. */
    (void)time;
    fdk_platform_connection *conn = data;
    struct fdk_wl_tablet_tool *tool = tool_for_proxy(conn, proxy);
    if (tool == NULL) {
        return;
    }
    /* A pending tip transition fires at the recorded position. */
    if (conn->tablet_pending_down && tool->prox != NULL) {
        fdk_event_data event;
        memset(&event, 0, sizeof(event));
        event.type = FDK_EVENT_TABLET_DOWN;
        tablet_fill_common(&event, tool);
        event.tablet.pressure = tool->pressure;
        conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
    } else if (conn->tablet_pending_up && tool->prox != NULL) {
        fdk_event_data event;
        memset(&event, 0, sizeof(event));
        event.type = FDK_EVENT_TABLET_UP;
        tablet_fill_common(&event, tool);
        conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
    }
    /* The pending MOTION (position/pressure/tilt) commits as one
     * event — at frame time, exactly like a real compositor batches
     * a position + pressure change into one motion. */
    if (conn->tablet_pending_motion && tool->prox != NULL) {
        fdk_event_data event;
        memset(&event, 0, sizeof(event));
        event.type = FDK_EVENT_TABLET_MOTION;
        tablet_fill_common(&event, tool);
        conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
    }
    conn->tablet_pending_down = 0;
    conn->tablet_pending_up = 0;
    conn->tablet_pending_motion = 0;
}

static const struct zwp_tablet_tool_v2_listener g_tool_listener = {
    .type = tool_type,
    .hardware_serial = tool_hardware_serial,
    .hardware_id_wacom = tool_hardware_id_wacom,
    .capability = tool_capability,
    .done = tool_done,
    .removed = tool_removed,
    .proximity_in = tool_proximity_in,
    .proximity_out = tool_proximity_out,
    .down = tool_down,
    .up = tool_up,
    .motion = tool_motion,
    .pressure = tool_pressure,
    .distance = tool_distance,
    .tilt = tool_tilt,
    .rotation = tool_rotation,
    .slider = tool_slider,
    .wheel = tool_wheel,
    .button = tool_button,
    .frame = tool_frame,
};

/* ---- the tablet seat listener --------------------------------------------- */

static void seat_tablet_added(void *data, struct zwp_tablet_seat_v2 *ts,
                              struct zwp_tablet_v2 *tablet) {
    /* Physical slabs: no fdk_event needs them individually (the tool
     * stream is the input). Destroying the proxy immediately is
     * legal per the protocol ("can be destroyed by the client as
     * soon as it is no longer needed") and keeps the table lean. */
    (void)data;
    (void)ts;
    zwp_tablet_v2_destroy(tablet);
}

static void seat_tool_added(void *data, struct zwp_tablet_seat_v2 *ts,
                            struct zwp_tablet_tool_v2 *tool) {
    (void)ts;
    fdk_platform_connection *conn = data;
    /* The seat mints the tool proxy; the record's slot is taken
     * here (the identity burst — type et al — follows on the tool's
     * own events before `done`). Overflow: the proxy still gets its
     * listener (protocol correctness) but no record, so every event
     * scan misses it — the tool is inert, one INFO line. */
    zwp_tablet_tool_v2_add_listener(tool, &g_tool_listener, conn);
    for (int i = 0; i < FDK_WL_TABLET_TOOLS; i++) {
        if (conn->tablet_tools[i].id == 0) {
            conn->tablet_tools[i].proxy = tool;
            conn->tablet_tools[i].id = (fdk_u32)(i + 1);
            conn->tablet_tools[i].type = 0; /* the `type` event fills */
            conn->tablet_tools[i].prox = NULL;
            conn->tablet_tools[i].last.x = 0.0f;
            conn->tablet_tools[i].last.y = 0.0f;
            conn->tablet_tools[i].pressure = 0.0f;
            conn->tablet_tools[i].tilt_x = 0.0f;
            conn->tablet_tools[i].tilt_y = 0.0f;
            return;
        }
    }
    FDK_INFO("tablet: over the %d-tool table — new tool inert",
             FDK_WL_TABLET_TOOLS);
}

static void seat_pad_added(void *data, struct zwp_tablet_seat_v2 *ts,
                           struct zwp_tablet_pad_v2 *pad) {
    /* Pads (button strips/rings): tracked-but-inert — the D-030
     * honest gap. Destroying the proxy now also unsubscribes its
     * group/button/ring events, which we would otherwise have to
     * implement as no-op listeners. */
    (void)data;
    (void)ts;
    zwp_tablet_pad_v2_destroy(pad);
}

static const struct zwp_tablet_seat_v2_listener g_tablet_seat_listener = {
    .tablet_added = seat_tablet_added,
    .tool_added = seat_tool_added,
    .pad_added = seat_pad_added,
};

/* ---- lifecycle: manager bind + seat attach -------------------------------- */

void fdk_wayland_tablet_seat_ready(fdk_platform_connection *conn) {
    if (conn == NULL || conn->seat == NULL ||
        conn->tablet_manager == NULL || conn->tablet_seat != NULL) {
        return; /* no manager, no seat yet, or already attached */
    }
    conn->tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(
        conn->tablet_manager, conn->seat);
    if (conn->tablet_seat == NULL) {
        FDK_WARN("get_tablet_seat failed — no tablet events");
        return;
    }
    zwp_tablet_seat_v2_add_listener(conn->tablet_seat,
                                    &g_tablet_seat_listener, conn);
}

void fdk_wayland_tablet_shutdown(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    /* The tool proxies die with the tablet seat/compositor side
     * objects; only OUR records need clearing (the connection is
     * going away). The tablet seat and manager proxies: */
    if (conn->tablet_seat != NULL) {
        zwp_tablet_seat_v2_destroy(conn->tablet_seat);
        conn->tablet_seat = NULL;
    }
    memset(conn->tablet_tools, 0, sizeof(conn->tablet_tools));
}

/* ---- the test seam (the touch discipline: drive the REAL bodies) --------
 *
 * sway headless binds the manager but no virtual-tablet protocol
 * exists in wlroots — the listener bodies are unreachable from any
 * rig. The seam drives them directly:
 *
 *   phase 0 = tool_added (tool_type; mints the tool record)
 *   phase 1 = proximity_in (targets `win`'s surface)
 *   phase 2 = motion+pressure/tilt into the pending record
 *   phase 3 = frame (commits pending motion / down / up)
 *   phase 4 = proximity_out
 *   phase 5 = button (button index | 0x80000000 = release)
 *   phase 6 = down (pending; fires at frame)
 *   phase 7 = up (pending; fires at frame)
 *
 * Returns 1 when the event was deliverable, 0 when dropped (the
 * caller treats 0 as the skip signal, the touch seam's contract). */
int fdk_wayland_test_tablet(fdk_platform_connection *conn,
                            fdk_platform_window *win, int phase,
                            uint32_t tool_type, double x, double y,
                            double pressure, double tilt_x,
                            double tilt_y, uint32_t button) {
    if (conn == NULL) {
        return 0;
    }
    /* The seam's ACTIVE tool: the record the last phase-0 minted
     * (conn->tablet_seam_current; -1 before the first mint). Real
     * rigs have compositor-minted proxies — the seam's records carry
     * proxy == NULL, which is what keeps them distinct from live
     * ones in every scan. */
    struct fdk_wl_tablet_tool *tool = NULL;
    if (conn->tablet_seam_current >= 0 &&
        conn->tablet_seam_current < FDK_WL_TABLET_TOOLS) {
        struct fdk_wl_tablet_tool *cand =
            &conn->tablet_tools[conn->tablet_seam_current];
        if (cand->id != 0 && cand->proxy == NULL) {
            tool = cand;
        }
    }
    switch (phase) {
    case 0: { /* tool_added: mint + make current */
        for (int i = 0; i < FDK_WL_TABLET_TOOLS; i++) {
            if (conn->tablet_tools[i].id == 0) {
                conn->tablet_tools[i].proxy = NULL;
                conn->tablet_tools[i].id = (fdk_u32)(i + 1);
                conn->tablet_tools[i].type =
                    (tool_type >= 1 && tool_type <= 8) ? tool_type : 0;
                conn->tablet_tools[i].prox = NULL;
                conn->tablet_seam_current = i;
                return 1;
            }
        }
        return 0;
    }
    case 1: /* proximity_in */
        return tablet_proximity_in_event(
            conn, tool, win != NULL ? win->surface : NULL);
    case 2: { /* motion -> pending */
        if (tool == NULL) {
            return 0;
        }
        tool->last.x = (fdk_f32)x;
        tool->last.y = (fdk_f32)y;
        tool->pressure = (fdk_f32)pressure;
        tool->tilt_x = (fdk_f32)tilt_x;
        tool->tilt_y = (fdk_f32)tilt_y;
        conn->tablet_pending_motion = 1;
        return 1;
    }
    case 3: { /* frame: the real listener's commit */
        if (tool == NULL) {
            return 0;
        }
        int delivered = 0;
        if (conn->tablet_pending_down && tool->prox != NULL) {
            fdk_event_data event;
            memset(&event, 0, sizeof(event));
            event.type = FDK_EVENT_TABLET_DOWN;
            tablet_fill_common(&event, tool);
            conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
            delivered = 1;
        } else if (conn->tablet_pending_up && tool->prox != NULL) {
            fdk_event_data event;
            memset(&event, 0, sizeof(event));
            event.type = FDK_EVENT_TABLET_UP;
            tablet_fill_common(&event, tool);
            conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
            delivered = 1;
        }
        if (conn->tablet_pending_motion && tool->prox != NULL) {
            fdk_event_data event;
            memset(&event, 0, sizeof(event));
            event.type = FDK_EVENT_TABLET_MOTION;
            tablet_fill_common(&event, tool);
            conn->dispatch(tool->prox, &event, conn->dispatch_user_data);
            delivered = 1;
        }
        conn->tablet_pending_down = 0;
        conn->tablet_pending_up = 0;
        conn->tablet_pending_motion = 0;
        return delivered;
    }
    case 4: /* proximity_out */
        return tablet_proximity_out_event(conn, tool);
    case 5: /* button (release = high bit) */
        return tablet_button_event(conn, tool,
                                   button & ~FDK_TABLET_BUTTON_RELEASE_BIT,
                                   (button & FDK_TABLET_BUTTON_RELEASE_BIT)
                                       ? 0
                                       : 1);
    case 6: /* down (pending until frame) */
        if (tool == NULL) {
            return 0;
        }
        conn->tablet_pending_down = 1;
        return 1;
    default: /* up (pending until frame) */
        if (tool == NULL) {
            return 0;
        }
        conn->tablet_pending_up = 1;
        return 1;
    }
}
