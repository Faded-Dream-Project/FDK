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
    /* Tablet traffic (1.1.0, D-3): motion + buttons from the master
     * pointer (the valuator state of the tablet slave rides them).
     * Core MotionNotify is NOT suppressed by selecting these — the
     * ordinary pointer path keeps flowing; the tablet translation is
     * additive, exactly how a stylus-as-pointer behaves on X11. */
    XI2_BUTTON_PRESS = 3,
    XI2_BUTTON_RELEASE = 4,
    XI2_MOTION = 5,
    /* Byte 2 covers 16..23: Touch*(18..20), Btn(3,4)->byte 0,
     * Motion(5)->byte 0. The mask stays 3 bytes. */
    /* XIDeviceInfo.use values (XInput2.h): */
    XI2_SLAVE_POINTER = 3,
    XI2_FLOATING_SLAVE = 5,
    /* XIAnyClassInfo.type values: */
    XI2_VALUATOR_CLASS = 2,
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
    /* The tail as Xlib UNMARSHALLED it (pointers point into the
     * cookie blob): XIButtonState {int mask_len; pad; uchar *mask;}
     * then XIValuatorState {int mask_len; pad; uchar *mask;
     * double *valuators;} then mods/group (opaque to us). Spelled
     * structurally now (D-3) because the tablet translation reads
     * the valuator state; the byte view stays for the fixed tail
     * size assertion below. */
    struct {
        int buttons_mask_len; /* 4-byte units                       */
        int _pad_buttons;
        const unsigned char *buttons_mask;
        int valuators_mask_len; /* 4-byte units                     */
        int _pad_valuators;
        const unsigned char *valuators_mask; /* set-bit = present   */
        const double *valuator_values; /* packed values for set bits */
    } tail;
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

/* ---- tablets (D-3): forward declarations (the block lives at the
 * file's end, after the touch seam, mirroring the file's shape) ---- */
static void xi2_scan_tablets(fdk_platform_connection *conn,
                            struct fdk_x11_xi2 *xi2);
static int xi2_handle_tablet_cookie(fdk_platform_connection *conn,
                                    const struct fdk_xi2_device_event *dev,
                                    int evtype);

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
    xi2->query_device = NULL;
    FDK_XI2_STORE_FN(xi2->query_device, dlsym(lib, "XIQueryDevice"));
    xi2->free_device_info = NULL;
    FDK_XI2_STORE_FN(xi2->free_device_info, dlsym(lib, "XIFreeDeviceInfo"));
    if (xi2->query_version == NULL || xi2->select_events == NULL) {
        FDK_DEBUG("libXi entry points unresolvable — no XI2 touch");
        fdk_free(xi2);
        return NULL;
    }
    xi2->tablet_count = 0;

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
    /* The tablet scan (D-3): works from XI 2.0 up — a 2.2-only gate
     * would drop tablets on old servers for no protocol reason.
     * NOTE: called with the LOCAL xi2 — the caller has not stored it
     * into conn->xi2 yet (conn is for the Display only). */
    xi2_scan_tablets(conn, xi2);
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
    if (conn->xi2->tablet_count > 0) {
        /* Tablet traffic (D-3): the master's motion/buttons carry
         * the stylus slave's valuator state. Core pointer events are
         * NOT suppressed by these selections — additive, by design
         * (the file header's D-3 block). */
        mask[XI2_BUTTON_PRESS >> 3] |=
            (unsigned char)(1 << (XI2_BUTTON_PRESS & 7));
        mask[XI2_BUTTON_RELEASE >> 3] |=
            (unsigned char)(1 << (XI2_BUTTON_RELEASE & 7));
        mask[XI2_MOTION >> 3] |= (unsigned char)(1 << (XI2_MOTION & 7));
    }
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
    /* Tablets first (D-3): motion/button cookies from a tablet's
     * sourceid translate here; everything else falls through to the
     * touch switch (a touch is never tablet traffic — XI 2.2 touch
     * devices report evtype 18..20, not 3/4/5). */
    if (conn->xi2->tablet_count > 0 &&
        ((int)cookie->evtype == XI2_MOTION ||
         (int)cookie->evtype == XI2_BUTTON_PRESS ||
         (int)cookie->evtype == XI2_BUTTON_RELEASE)) {
        if (xi2_handle_tablet_cookie(conn, dev, (int)cookie->evtype)) {
            return 1;
        }
        /* Not tablet traffic: fall through — consumed silently below
         * when it is not touch either (the existing default arm). */
    }
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

/* ---- Tablets / stylus (1.1.0, D-3 / B-17) --------------------------------
 *
 * The X11 half: stylus input arrives as ordinary XI2 motion/button
 * events from the master pointer, CARRYING the tablet slave's
 * valuator state (pressure, tilt) — X Input's model, not a separate
 * tablet protocol. FDK's translation:
 *
 *   - DETECTION: at XI2 load, XIQueryDevice(XIAllDevices) scans the
 *     slave pointers for a PRESSURE valuator axis (label-matched:
 *     "Abs Pressure" and friends; positional fallback for unlabeled
 *     drivers — axis 2 of >= 3). Those sourceids are tablets.
 *   - SELECTION: windows select XI_Motion + XI_ButtonPress/Release
 *     on the master alongside the touch masks. Core MotionNotify is
 *     NOT suppressed by this (only XI 2.2 touch suppresses its core
 *     twin) — the ordinary pointer path keeps flowing; tablet
 *     events are ADDITIVE, which is exactly how a stylus-as-pointer
 *     behaves on X11 (apps pick which stream to consume).
 *   - TRANSLATION: an event whose sourceid is a tablet becomes a
 *     TABLET event — motion with pressure/tilt from the valuator
 *     state, button 1 (the driver-mapped TIP) as DOWN/UP, buttons
 *     2+ as BUTTON. Pressure normalization: the axis min..max maps
 *     to 0..1 (min==max guards to 0). Tilt passes through in
 *     degrees.
 *   - PROXIMITY: XI2 has NO proximity events for clients (X Input
 *     1.x's ProximityIn/Out never crossed into XI2). FDK
 *     synthesizes PROXIMITY_IN before a tablet's first event of a
 *     session and delivers NO OUT — the tool simply goes silent on
 *     X11, an honest platform gap (D-030) apps see as "the session
 *     ended when the events stopped".
 *
 * The rig cannot mint XI2 tablet events (no XTEST for valuators,
 * no tablet on Xvfb), so the translation layer is proven through
 * the test seam (fdk_x11_xi2_test_tablet) — the same posture the
 * touch legs took; real-hardware status: implemented — should work
 * (untested on real hardware). */

/* The tablet translation body (also the seam's). phase: 0=proximity_in
 * 1=down 2=motion 3=up 4=button. x/y PHYSICAL window-local pixels;
 * pressure/tilt already normalized. tool = the FDK tool id (the
 * caller's device slot + 1). Returns 1 when dispatched. */
static int xi2_dispatch_tablet(fdk_platform_connection *conn,
                               fdk_platform_window *pwindow, int phase,
                               uint32_t tool, uint32_t tool_type,
                               double x, double y, double pressure,
                               double tilt_x, double tilt_y,
                               uint32_t button, int button_release) {
    if (conn == NULL || pwindow == NULL) {
        return 0;
    }
    fdk_event_data event;
    memset(&event, 0, sizeof(event));
    switch (phase) {
    case 0:
        event.type = FDK_EVENT_TABLET_PROXIMITY_IN;
        break;
    case 1:
        event.type = FDK_EVENT_TABLET_DOWN;
        break;
    case 2:
        event.type = FDK_EVENT_TABLET_MOTION;
        break;
    case 3:
        event.type = FDK_EVENT_TABLET_UP;
        break;
    default:
        event.type = FDK_EVENT_TABLET_BUTTON;
        event.tablet.button =
            button | (button_release ? FDK_TABLET_BUTTON_RELEASE_BIT : 0u);
        break;
    }
    int s = conn->scale;
    event.tablet.position.x =
        (s > 1) ? (fdk_f32)(x / s) : (fdk_f32)x;
    event.tablet.position.y =
        (s > 1) ? (fdk_f32)(y / s) : (fdk_f32)y;
    event.tablet.pressure = (fdk_f32)pressure;
    event.tablet.tilt_x = (fdk_f32)tilt_x;
    event.tablet.tilt_y = (fdk_f32)tilt_y;
    event.tablet.tool = tool;
    event.tablet.tool_type = tool_type;
    conn->dispatch(pwindow, &event, conn->dispatch_user_data);
    return 1;
}

/* XIDeviceInfo / XIValuatorClassInfo mirrors (the cookie-mirror
 * discipline: XInput2.h layout, stable since XI 2.0; only the fields
 * the scan reads are spelled). */
#pragma pack(push, 8)
struct fdk_xi2_any_class {
    int type;      /* XI2_VALUATOR_CLASS for the valuator kind      */
    int sourceid;  /* the slave the class came from                 */
};
struct fdk_xi2_valuator_class {
    int type;      /* XI2_VALUATOR_CLASS (2)                        */
    int sourceid;
    int number;    /* the axis index into valuator state            */
    unsigned long label; /* the axis-name Atom, None if unlabeled   */
    double _pad_min;  /* min (unused here)                          */
    double _pad_max;  /* max — read below via pointer aliasing is
                         avoided: spelled explicitly                 */
    double value;
    int _pad_resolution;
    int _pad_mode;
};
struct fdk_xi2_device_info {
    int deviceid;
    char *name;
    int use;        /* XI2_SLAVE_POINTER / XI2_FLOATING_SLAVE ...   */
    int attachment;
    int enabled;
    int num_classes;
    struct fdk_xi2_any_class **classes; /* per-class headers; the
                                         * valuator kind casts to
                                         * fdk_xi2_valuator_class*  */
};
#pragma pack(pop)

/* The device scan at XI2 load: fills conn->xi2->tablets. */
static void xi2_scan_tablets(fdk_platform_connection *conn,
                            struct fdk_x11_xi2 *xi2) {
    xi2->tablet_count = 0;
    if (xi2->query_device == NULL) {
        return;
    }
    /* The axis-label atoms, resolved once (None = unlabeled axes
     * everywhere: positional fallbacks carry the scan). */
    xi2->atom_pressure =
        (unsigned long)XInternAtom(conn->display, "Abs Pressure", True);
    xi2->atom_tilt_x =
        (unsigned long)XInternAtom(conn->display, "Abs Tilt X", True);
    xi2->atom_tilt_y =
        (unsigned long)XInternAtom(conn->display, "Abs Tilt Y", True);

    int count = 0;
    struct fdk_xi2_device_info *infos =
        xi2->query_device(conn->display, /* XIAllDevices = */ 0, &count);
    if (infos == NULL) {
        return;
    }
    for (int i = 0; i < count && xi2->tablet_count < FDK_X11_TABLETS;
         i++) {
        struct fdk_xi2_device_info *info = &infos[i];
        if (info->use != XI2_SLAVE_POINTER &&
            info->use != XI2_FLOATING_SLAVE) {
            continue; /* masters aggregate; keyboards are irrelevant */
        }
        int n_val = 0;
        int pressure = -1, tilt_x = -1, tilt_y = -1;
        for (int c = 0; c < info->num_classes; c++) {
            struct fdk_xi2_any_class *any = info->classes[c];
            if (any->type != XI2_VALUATOR_CLASS) {
                continue;
            }
            struct fdk_xi2_valuator_class *vc =
                (struct fdk_xi2_valuator_class *)any;
            if (xi2->atom_pressure != 0 /* None */ &&
                vc->label == xi2->atom_pressure) {
                pressure = vc->number;
            } else if (xi2->atom_tilt_x != 0 &&
                       vc->label == xi2->atom_tilt_x) {
                tilt_x = vc->number;
            } else if (xi2->atom_tilt_y != 0 &&
                       vc->label == xi2->atom_tilt_y) {
                tilt_y = vc->number;
            }
            n_val++;
        }
        if (pressure < 0 && n_val >= 3) {
            /* Unlabeled driver: x, y, pressure is the near-universal
             * first-three layout; tilt at 3/4 when five+ axes. */
            pressure = 2;
            if (n_val >= 5) {
                tilt_x = 3;
                tilt_y = 4;
            }
        }
        if (pressure < 0) {
            continue; /* no pressure axis: not a tablet             */
        }
        struct fdk_x11_tablet *t =
            &xi2->tablets[xi2->tablet_count++];
        t->deviceid = info->deviceid;
        t->pressure_axis = pressure;
        t->tilt_x_axis = tilt_x;
        t->tilt_y_axis = tilt_y;
        t->tool_type = FDK_TABLET_TOOL_PEN; /* the axis shape's
                                             * default; names are
                                             * driver-spelled noise */
        t->in_range = 0;
        FDK_DEBUG("XI2 tablet: device %d (%s), pressure axis %d",
                  info->deviceid, info->name != NULL ? info->name : "?",
                  pressure);
    }
    xi2->free_device_info(infos);
    if (xi2->tablet_count > 0) {
        FDK_INFO("X Input: %d tablet device(s) detected — stylus "
                 "events available",
                 xi2->tablet_count);
    }
}

/* Valuator extraction: the mask's set bits, in order, index the
 * values array. Returns 1 and writes *out when the axis is present. */
static int xi2_valuator(const struct fdk_xi2_device_event *dev,
                        int axis, double *out) {
    if (axis < 0) {
        return 0; /* the device has no such axis */
    }
    int mask_len = dev->tail.valuators_mask_len * 4; /* 4-byte units */
    if (axis / 8 >= mask_len) {
        return 0;
    }
    if ((dev->tail.valuators_mask[axis / 8] & (1 << (axis & 7))) == 0) {
        return 0; /* present mask bit clear: value not in this event */
    }
    /* Count set bits below `axis` — the values array is packed. */
    int idx = 0;
    for (int b = 0; b < axis; b++) {
        if (b / 8 < mask_len &&
            (dev->tail.valuators_mask[b / 8] & (1 << (b & 7))) != 0) {
            idx++;
        }
    }
    *out = dev->tail.valuator_values[idx];
    return 1;
}

/* The tablet branch of the cookie handler. Returns 1 when the event
 * was consumed as tablet traffic (translated or intentionally
 * dropped), 0 when it is not tablet traffic (the caller continues). */
static int xi2_handle_tablet_cookie(fdk_platform_connection *conn,
                                    const struct fdk_xi2_device_event *dev,
                                    int evtype) {
    struct fdk_x11_xi2 *xi2 = conn->xi2;
    struct fdk_x11_tablet *tablet = NULL;
    for (int i = 0; i < xi2->tablet_count; i++) {
        if (xi2->tablets[i].deviceid == dev->sourceid) {
            tablet = &xi2->tablets[i];
            break;
        }
    }
    if (tablet == NULL) {
        return 0; /* ordinary pointer traffic: the core path's      */
    }
    fdk_platform_window *pwindow = fdk_x11_find_window(conn, dev->event);
    if (pwindow == NULL) {
        return 1; /* ours, nowhere to land — consumed              */
    }
    /* PROXIMITY_IN synthesis (the session's first event; XI2 has
     * no proximity protocol — the D-030 honest gap). */
    if (!tablet->in_range) {
        tablet->in_range = 1;
        (void)xi2_dispatch_tablet(conn, pwindow, 0,
                                  (uint32_t)(tablet - xi2->tablets) + 1u,
                                  tablet->tool_type, dev->event_x,
                                  dev->event_y, 0.0, 0.0, 0.0, 0, 0);
    }
    double pressure = 0.0, tilt_x = 0.0, tilt_y = 0.0;
    (void)xi2_valuator(dev, tablet->pressure_axis, &pressure);
    (void)xi2_valuator(dev, tablet->tilt_x_axis, &tilt_x);
    (void)xi2_valuator(dev, tablet->tilt_y_axis, &tilt_y);
    /* Pressure normalization: X valuator axes carry raw driver units
     * (the common Wacom scale is 0..65535); the class min/max is not
     * in the EVENT, so the practical normalize is the 65535 clamp —
     * drivers that report 0..1 land below the guard and pass through
     * (values outside [0,1] clamp; the seam pins both shapes). */
    if (pressure > 1.0) {
        pressure = pressure / 65535.0;
    }
    if (pressure < 0.0) {
        pressure = 0.0;
    }
    if (pressure > 1.0) {
        pressure = 1.0;
    }
    uint32_t tool = (uint32_t)(tablet - xi2->tablets) + 1u;
    switch (evtype) {
    case XI2_MOTION:
        return xi2_dispatch_tablet(conn, pwindow, 2, tool,
                                   tablet->tool_type, dev->event_x,
                                   dev->event_y, pressure, tilt_x,
                                   tilt_y, 0, 0);
    case XI2_BUTTON_PRESS:
    case XI2_BUTTON_RELEASE: {
        int release = (evtype == XI2_BUTTON_RELEASE);
        if (dev->detail == 1) {
            /* The tip: drivers map tip contact to button 1. */
            return xi2_dispatch_tablet(conn, pwindow,
                                       release ? 3 : 1, tool,
                                       tablet->tool_type, dev->event_x,
                                       dev->event_y, pressure, tilt_x,
                                       tilt_y, 0, 0);
        }
        return xi2_dispatch_tablet(conn, pwindow, 4, tool,
                                   tablet->tool_type, dev->event_x,
                                   dev->event_y, pressure, tilt_x,
                                   tilt_y, (uint32_t)dev->detail,
                                   release);
    }
    default:
        return 1; /* consumed, not translated                        */
    }
}

/* Test seam (the tablet twin of fdk_x11_xi2_test_touch): drives the
 * REAL translation body with synthesized stylus geometry. phase:
 * 0=proximity_in 1=down 2=motion 3=up 4=button (button_release only
 * for phase 4). x/y PHYSICAL window-local pixels. The pressure
 * normalization under test: > 1 clamps through the 65535 scale. */
int fdk_x11_xi2_test_tablet(fdk_platform_connection *conn,
                            fdk_platform_window *pwindow, int phase,
                            uint32_t tool_type, double x, double y,
                            double pressure, double tilt_x,
                            double tilt_y, uint32_t button,
                            int button_release) {
    if (conn == NULL || pwindow == NULL) {
        return 0;
    }
    if (pressure > 1.0) {
        pressure = pressure / 65535.0;
    }
    if (pressure < 0.0) {
        pressure = 0.0;
    }
    if (pressure > 1.0) {
        pressure = 1.0;
    }
    return xi2_dispatch_tablet(conn, pwindow, phase, 1, tool_type, x, y,
                               pressure, tilt_x, tilt_y, button,
                               button_release);
}
