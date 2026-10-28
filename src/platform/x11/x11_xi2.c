/*
 * x11_xi2.c — XI2 touch through dlopen'd libXi (1.1.0, M-3)
 *
 * The X11 side of the touch model: XI_TouchBegin / XI_TouchUpdate /
 * XI_TouchEnd (X Input Extension >= 2.2) translated into FDK's
 * FDK_EVENT_TOUCH_* events. The tap / two-finger-scroll policy lives
 * in the core (src/window/touch.c) — identical to the Wayland side.
 *
 * LOAD DISCIPLINE (the GPU lane's posture, docs/dependencies.md):
 * libXi is dlopen'd at RUN time, never a build or link dependency.
 * A machine without libXi.so.6, or a server without XI >= 2.2,
 * simply gets no touch events — window creation, the core protocol,
 * everything else is unaffected. The library handle is pinned for
 * the process lifetime (dlopen's refcount; no dlclose — the same
 * rule as libGL: Xlib caches extension state against the handle and
 * a teardown dlclose is a use-after-free waiting to happen).
 *
 * HEADERS: this file deliberately does NOT require XInput2.h. The
 * struct layout and constants below mirror XInput2.h's wire format
 * exactly (the ABI has been stable since XI 2.0); where the header
 * IS present at build time (__has_include), we include it and let
 * the compiler verify the mirror against the real definitions —
 * belt and braces, zero new build requirements either way.
 *
 * EVENT DELIVERY: XI2 events are GenericEvents (type 35) whose
 * payload arrives through Xlib's cookie mechanism — XGetEventData/
 * XFreeEventData (libX11 itself, already linked). x11_dispatch.c's
 * loop hands every GenericEvent whose extension opcode matches ours
 * to fdk_x11_xi2_handle_cookie() here.
 *
 * EMULATION: the XI 2.2 protocol suppresses emulated core button
 * events for touches on windows where the client selected XI2 touch
 * events — so the core engine's synthesized tap clicks are the ONLY
 * click a touch produces for FDK. No double-click pathology.
 *
 * COORDINATES: XIDeviceEvent.event_x/event_y are window-local
 * PHYSICAL pixels — divided by the connection's integer scale, the
 * same conversion every core pointer event gets (x11_events.c).
 */

#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

/* ---- XI2 constants (XInput2.h values) --------------------------------- */

enum {
    XI2_GENERIC_EVENT = 35, /* Xlib's GenericEvent type */
    XI2_TOUCH_BEGIN = 18,
    XI2_TOUCH_UPDATE = 19,
    XI2_TOUCH_END = 20,
    /* deviceid for XISelectEvents: all master devices (the pointer
     * master carries direct-touch streams; slaves would give RAW
     * duplicates of everything). */
    XI2_ALL_MASTER_DEVICES = 1,
    /* mask bytes: XISetMask uses (type >> 3) byte, (type & 7) bit —
     * TouchEnd (20) needs byte 2, so three bytes cover 18..20. */
    XI2_TOUCH_MASK_LEN = 3,
};

/* POSIX dlsym returns void*; ISO C (pedantic) forbids the direct
 * object-pointer -> function-pointer assignment, so every store
 * goes through the POSIX-blessed *(void**)& idiom (the GL lane's
 * FDK_GL_STORE_FN rule — the zero-warning build stays intact). */
#define FDK_XI2_STORE_FN(dst, sym) (*(void **)&(dst) = (sym))

/* The libXi function table — the definition lives in
 * x11_platform.h (the dispatch loop matches cookies by `opcode`). */

/* XIEventMask (wire layout, mirrored): */
struct fdk_xi2_event_mask {
    int deviceid;
    int mask_len;
    unsigned char *mask;
};

/* XIDeviceEvent (wire layout, mirrored — see the file header for
 * the stability argument). Only the fields the touch translation
 * reads are spelled out positionally up to the ones we need; the
 * tail (buttons/valuators/mods/group) is opaque to us. */
#pragma pack(push, 8) /* match the natural x86_64/ABI alignment XInput2.h
                       * was compiled with; every field below is at its
                       * natural alignment anyway (the pack is a
                       * belt-and-braces no-op that documents intent) */
struct fdk_xi2_device_event {
    int type;                /* GenericEvent (35) */
    unsigned long serial;
    int send_event;
    Display *display;
    int extension;           /* our opcode */
    int evtype;              /* XI_TouchBegin/Update/End */
    unsigned long time;      /* Time */
    int deviceid;
    int sourceid;
    int detail;              /* the touch id (the slot) */
    int _pad0;               /* align root to 8 */
    Window root;
    Window event;            /* our window */
    Window child;
    double root_x;
    double root_y;
    double event_x;          /* window-local, PHYSICAL px */
    double event_y;
    /* opaque tail: XIButtonState buttons; XIValuatorState
     * valuators; XIModifierState mods; XIGroupState group. */
    unsigned char tail[72];
};
#pragma pack(pop)

/* The single touch-translation entry point (also the test seam's
 * body): builds the FDK event and dispatches it. event_x/event_y
 * are PHYSICAL window-local pixels; phase 0=down 1=update 2=end
 * 3=cancel(wipe). Returns 1 when a TOUCH event was dispatched. */
static int xi2_dispatch_touch(fdk_platform_connection *conn,
                              fdk_platform_window *pwindow, int phase,
                              uint32_t id, double event_x, double event_y) {
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    switch (phase) {
    case 0:
        event.type = FDK_EVENT_TOUCH_DOWN;
        break;
    case 1:
        event.type = FDK_EVENT_TOUCH_MOTION;
        break;
    case 2:
        event.type = FDK_EVENT_TOUCH_UP;
        break;
    default:
        event.type = FDK_EVENT_TOUCH_CANCEL;
        break;
    }
    event.touch.id = id;
    /* Physical -> logical, the same integer-scale division every
     * core pointer event takes (x11_events.c's x11_to_logical_*). */
    int s = conn->scale;
    event.touch.position.x =
        (s > 1) ? (fdk_f32)(event_x / s) : (fdk_f32)event_x;
    event.touch.position.y =
        (s > 1) ? (fdk_f32)(event_y / s) : (fdk_f32)event_y;
    conn->dispatch(pwindow, &event, conn->dispatch_user_data);
    return 1;
}

/* ---- lifecycle --------------------------------------------------------- */

struct fdk_x11_xi2 *fdk_x11_xi2_try_load(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return NULL;
    }

    void *lib = dlopen("libXi.so.6", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        const char *err = dlerror();
        FDK_DEBUG("libXi.so.6 not loadable (%s) — no XI2 touch",
                  err != NULL ? err : "unknown error");
        return NULL;
    }

    struct fdk_x11_xi2 *xi2 = fdk_alloc(sizeof(struct fdk_x11_xi2));
    if (xi2 == NULL) {
        return NULL; /* the lib stays pinned; no dlclose */
    }
    xi2->lib = lib;
    xi2->touch_ok = 0;
    xi2->warned_once = 0;
    xi2->query_version = NULL;
    FDK_XI2_STORE_FN(xi2->query_version, dlsym(lib, "XIQueryVersion"));
    xi2->select_events = NULL;
    FDK_XI2_STORE_FN(xi2->select_events, dlsym(lib, "XISelectEvents"));
    if (xi2->query_version == NULL || xi2->select_events == NULL) {
        FDK_DEBUG("libXi entry points unresolvable — no XI2 touch");
        fdk_free(xi2);
        return NULL;
    }

    /* The extension opcode comes from libX11 itself (already
     * linked): GenericEvent matching keys on it. */
    int first_event = 0, first_error = 0, opcode = 0;
    if (!XQueryExtension(conn->display, "XInputExtension", &opcode,
                         &first_event, &first_error)) {
        FDK_DEBUG("server has no X Input extension — no XI2 touch");
        fdk_free(xi2);
        return NULL;
    }
    xi2->opcode = opcode;

    /* Touch needs XI 2.2 (XIQueryVersion clamps UP to what the
     * server speaks; a return of Success with major/minor >= 2.2 is
     * our gate). */
    int major = 2, minor = 2;
    if (xi2->query_version(conn->display, &major, &minor) != Success ||
        major < 2 || (major == 2 && minor < 2)) {
        FDK_DEBUG("server XI %d.%d < 2.2 — no touch events",
                  major, minor);
        fdk_free(xi2);
        return NULL;
    }
    xi2->touch_ok = 1;
    FDK_INFO("X Input %d.%d: touch events available", major, minor);
    return xi2;
}

void fdk_x11_xi2_shutdown(struct fdk_x11_xi2 *xi2) {
    if (xi2 == NULL) {
        return;
    }
    /* No dlclose — see the file header; the handle is process-
     * lifetime state. Only the record dies. */
    fdk_free(xi2);
}

/* Selects XI_TouchBegin/Update/End on the window (called from
 * window create; harmless where the extension is absent). */
void fdk_x11_xi2_select_window(fdk_platform_connection *conn,
                               unsigned long xwindow) {
    if (conn == NULL || conn->xi2 == NULL || !conn->xi2->touch_ok ||
        xwindow == 0) {
        return;
    }
    unsigned char mask[XI2_TOUCH_MASK_LEN];
    memset(mask, 0, sizeof(mask));
    /* XISetMask, mirrored: byte (type >> 3), bit (type & 7). */
    mask[XI2_TOUCH_BEGIN >> 3] |= (unsigned char)(1 << (XI2_TOUCH_BEGIN & 7));
    mask[XI2_TOUCH_UPDATE >> 3] |= (unsigned char)(1 << (XI2_TOUCH_UPDATE & 7));
    mask[XI2_TOUCH_END >> 3] |= (unsigned char)(1 << (XI2_TOUCH_END & 7));
    struct fdk_xi2_event_mask em = {
        .deviceid = XI2_ALL_MASTER_DEVICES,
        .mask_len = XI2_TOUCH_MASK_LEN,
        .mask = mask,
    };
    if (conn->xi2->select_events(conn->display, (Window)xwindow, &em, 1) !=
        Success) {
        if (!conn->xi2->warned_once) {
            FDK_INFO("XISelectEvents(touch) refused — no touch on this "
                     "window (per-connection notice)");
            conn->xi2->warned_once = 1;
        }
    }
}

/* The GenericEvent branch of the dispatch loop: resolves the target
 * window from the cookie payload (the XIDeviceEvent's `event` field
 * — GenericEvents report their receipt window in the payload, the
 * outer XEvent's xany.window is unreliable for them), translates,
 * dispatches. Returns nonzero when the cookie was OURS (the cookie
 * DATA itself is the caller's to free — this function never owns
 * it). `cookie->data` must already be valid (XGetEventData done). */
int fdk_x11_xi2_handle_cookie(fdk_platform_connection *conn,
                              XGenericEventCookie *cookie) {
    if (conn == NULL || conn->xi2 == NULL || cookie == NULL) {
        return 0;
    }
    if (cookie->extension != conn->xi2->opcode) {
        return 0; /* someone else's GenericEvent */
    }
    if (cookie->data == NULL) {
        return 1; /* ours, but empty (XGetEventData failed upstream) */
    }
    const struct fdk_xi2_device_event *dev = cookie->data;
    int phase = -1;
    switch ((int)cookie->evtype) {
    case XI2_TOUCH_BEGIN:
        phase = 0;
        break;
    case XI2_TOUCH_UPDATE:
        phase = 1;
        break;
    case XI2_TOUCH_END:
        phase = 2;
        break;
    default:
        return 1; /* OUR extension, but XI key/button/hierarchy/
                   * property traffic FDK models through the core
                   * protocol instead — consumed, not translated */
    }
    fdk_platform_window *pwindow =
        fdk_x11_find_window(conn, dev->event);
    if (pwindow == NULL) {
        return 1; /* a window we already destroyed but the server
                   * still had queued events for — the documented
                   * no-op of the core loop */
    }
    return xi2_dispatch_touch(conn, pwindow, phase,
                              (uint32_t)dev->detail,
                              dev->event_x, dev->event_y);
}

/* Test seam (the Wayland test_touch twin): drive the REAL
 * translation+dispatch path with synthesized contact geometry.
 * phase 0=down 1=motion 2=up 3=cancel; (x,y) PHYSICAL window-local
 * pixels (the raw XIDeviceEvent space — the caller divides nothing;
 * the scale conversion inside is part of what is under test). */
int fdk_x11_xi2_test_touch(fdk_platform_connection *conn,
                           fdk_platform_window *pwindow, int phase,
                           uint32_t id, double x, double y) {
    if (conn == NULL || pwindow == NULL) {
        return 0;
    }
    return xi2_dispatch_touch(conn, pwindow, phase, id, x, y);
}
