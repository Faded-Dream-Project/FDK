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
 *
 * 1.1.0-rc2 — THE D-3 CRASH (found on the owner's desktop, Arch/
 * Openbox/NVIDIA): the tablet translation read the cookie's
 * valuator tail through a mirror that was missing XIDeviceEvent's
 * `flags` field (the int between event_y and buttons), so every
 * tail field was read EIGHT BYTES EARLY: "valuators_mask_len" was
 * really the low half of buttons.mask's heap pointer (arbitrary
 * garbage; -708600696 on the crash machine, where the old *4
 * overflowed int and wrapped to a huge positive, defeating the
 * bounds check), and "valuators_mask" was really {valuators.
 * mask_len, uninit pad} reinterpreted as a pointer — 0xbebebebe…,
 * the SEGV address in the report. Two more defects rode along:
 * the event-type constants were off by one (3/4/5 where the
 * protocol numbers KeyRelease/ButtonPress/ButtonRelease — the
 * window was selecting KeyRelease, and ButtonPress arrived where
 * the code expected Motion), and the device scan classified ANY
 * slave pointer with >=3 valuator axes as a "tablet" — mice,
 * XTEST pointers, touchpads — so a desktop with no stylus at all
 * reported "4 tablet device(s) detected" and fed ordinary mouse
 * traffic into the broken reader. The 1.1.0-rc2 posture, layered:
 * correct constants; the mirror verified against XInput2.h at
 * build time wherever the header exists (__has_include static
 * asserts — the check this file always promised and never had);
 * classification that requires STYLUS EVIDENCE (an absolute
 * pressure axis — labeled "Abs Pressure", or the absolute
 * x/y/+3-axis shape of an unlabeled tablet; relative axes alone
 * are never a tablet); and every valuator read bounded by the
 * device's real axis count, the mask's byte length, and the
 * packed values indexing, with all arithmetic in size_t.
 */

#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---- mirror verification (1.1.0-rc2) ----------------------------------------
 *
 * Where the real XInput2.h exists at build time, include it and let
 * the compiler prove the mirrors below against it — layout AND
 * constants. This is the discipline the file header always claimed
 * and never had; the missing `flags` field would have been caught
 * here. XInput2.h needs Xfixes.h (PointerBarrier), so both must be
 * visible. Absent headers: the mirrors stand alone (CI images
 * without libxi-dev build exactly the same, just unverified). */
#if defined(__has_include)
#if __has_include(<X11/extensions/XInput2.h>) && \
    __has_include(<X11/extensions/Xfixes.h>)
#define FDK_XI2_HEADER_VERIFY 1
#include <X11/extensions/XInput2.h>
#endif
#endif

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
    /* Tablet traffic (1.1.0, D-3; constants CORRECTED 1.1.0-rc2 — the
     * protocol's real numbering, XI2.h: DeviceChanged=1, KeyPress=2,
     * KeyRelease=3, ButtonPress=4, ButtonRelease=5, Motion=6 — the
     * file's old 3/4/5 selected KeyRelease and mislabeled every
     * button event): motion + buttons from the master pointer (the
     * valuator state of the tablet slave rides them). Core
     * MotionNotify is NOT suppressed by selecting these — the
     * ordinary pointer path keeps flowing; the tablet translation
     * is additive, exactly how a stylus-as-pointer behaves on X11. */
    XI2_BUTTON_PRESS = 4,
    XI2_BUTTON_RELEASE = 5,
    XI2_MOTION = 6,
    /* Byte 0 covers 0..7: Button(4,5) and Motion(6); byte 2 covers
     * 16..23: Touch*(18..20). The mask stays 3 bytes. */
    /* XIDeviceInfo.use values (XInput2.h): */
    XI2_SLAVE_POINTER = 3,
    XI2_SLAVE_KEYBOARD = 4,
    XI2_FLOATING_SLAVE = 5,
    /* XIAnyClassInfo.type values: */
    XI2_VALUATOR_CLASS = 2,
    /* XIValuatorClassInfo.mode values (XInput2.h): */
    XI2_MODE_RELATIVE = 0,
    XI2_MODE_ABSOLUTE = 1,
    /* The valuator-mask sanity bound (1.1.0-rc2): the library-level
     * mask_len is BYTES; anything beyond this is not a mask any
     * real server would marshal (64 axes need 8 bytes; the bound
     * allows 1024 axes' worth, generously). */
    XI2_MASK_BYTES_MAX = 128,
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
 * tail (buttons/valuators/mods/group) is opaque to us. The `flags`
 * field (1.1.0-rc2) is REAL — XIDeviceEvent carries it between
 * event_y and buttons, and omitting it shifted every tail read
 * eight bytes early (the D-3 crash; see the header block). */
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
    int flags;               /* XIDeviceEvent.flags (1.1.0-rc2: was missing
                              * — the D-3 crash's byte shift) */
    /* The tail as Xlib UNMARSHALLED it (pointers point into the
     * cookie blob): XIButtonState {int mask_len; pad; uchar *mask;}
     * then XIValuatorState {int mask_len; pad; uchar *mask;
     * double *valuators;} then mods/group (opaque to us). Spelled
     * structurally (D-3) because the tablet translation reads the
     * valuator state. NOTE (1.1.0-rc2): the library-level mask_len is
     * in BYTES — libXi converts the wire's 4-byte units on the way
     * out (verified empirically against libXi 1.8.2 cookies: a
     * 2-axis motion event carries mask_len == 8). The old *4 here
     * was a 4x over-read even where the offsets were right. */
    struct {
        int buttons_mask_len; /* bytes                            */
        int _pad_buttons;
        const unsigned char *buttons_mask;
        int valuators_mask_len; /* bytes                         */
        int _pad_valuators;
        const unsigned char *valuators_mask; /* set-bit = present   */
        const double *valuator_values; /* packed values for set bits */
        /* mods/group complete the struct so the size assertion
         * covers the whole layout (opaque — never read). */
        int mods_base, mods_latched, mods_locked, mods_effective;
        int group_base, group_latched, group_locked, group_effective;
    } tail;
};
#pragma pack(pop)

/* The verification the header promises: layout + constants against
 * the real XInput2.h, at compile time, wherever it exists. */
#ifdef FDK_XI2_HEADER_VERIFY
#define FDK_XI2_ASSERT(cond) _Static_assert((cond), "XI2 mirror drifted")
FDK_XI2_ASSERT(sizeof(struct fdk_xi2_device_event) == sizeof(XIDeviceEvent));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, evtype) ==
               offsetof(XIDeviceEvent, evtype));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, deviceid) ==
               offsetof(XIDeviceEvent, deviceid));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, sourceid) ==
               offsetof(XIDeviceEvent, sourceid));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, detail) ==
               offsetof(XIDeviceEvent, detail));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, event) ==
               offsetof(XIDeviceEvent, event));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, event_x) ==
               offsetof(XIDeviceEvent, event_x));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, event_y) ==
               offsetof(XIDeviceEvent, event_y));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event, flags) ==
               offsetof(XIDeviceEvent, flags));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.buttons_mask_len) ==
               offsetof(XIDeviceEvent, buttons.mask_len));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.buttons_mask) ==
               offsetof(XIDeviceEvent, buttons.mask));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.valuators_mask_len) ==
               offsetof(XIDeviceEvent, valuators.mask_len));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.valuators_mask) ==
               offsetof(XIDeviceEvent, valuators.mask));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.valuator_values) ==
               offsetof(XIDeviceEvent, valuators.values));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.mods_base) ==
               offsetof(XIDeviceEvent, mods.base));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_event,
                        tail.group_base) ==
               offsetof(XIDeviceEvent, group.base));
FDK_XI2_ASSERT(XI2_BUTTON_PRESS == XI_ButtonPress);
FDK_XI2_ASSERT(XI2_BUTTON_RELEASE == XI_ButtonRelease);
FDK_XI2_ASSERT(XI2_MOTION == XI_Motion);
FDK_XI2_ASSERT(XI2_TOUCH_BEGIN == XI_TouchBegin);
FDK_XI2_ASSERT(XI2_TOUCH_UPDATE == XI_TouchUpdate);
FDK_XI2_ASSERT(XI2_TOUCH_END == XI_TouchEnd);
FDK_XI2_ASSERT(XI2_ALL_MASTER_DEVICES == XIAllMasterDevices);
FDK_XI2_ASSERT(XI2_SLAVE_POINTER == XISlavePointer);
FDK_XI2_ASSERT(XI2_SLAVE_KEYBOARD == XISlaveKeyboard);
FDK_XI2_ASSERT(XI2_FLOATING_SLAVE == XIFloatingSlave);
FDK_XI2_ASSERT(XI2_VALUATOR_CLASS == XIValuatorClass);
FDK_XI2_ASSERT(XI2_MODE_RELATIVE == XIModeRelative);
FDK_XI2_ASSERT(XI2_MODE_ABSOLUTE == XIModeAbsolute);
#undef FDK_XI2_ASSERT
#endif /* FDK_XI2_HEADER_VERIFY */

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
    /* 1.1.0-rc2: no field of this record may ride malloc's fill pattern
     * — zero everything, then give the axis fields their "absent"
     * sentinels (0 would read as "axis 0"). The scan fills the rest. */
    memset(xi2, 0, sizeof(*xi2));
    for (int i = 0; i < FDK_X11_TABLETS; i++) {
        xi2->tablets[i].deviceid = -1;
        xi2->tablets[i].n_axes = 0;
        xi2->tablets[i].pressure_axis = -1;
        xi2->tablets[i].tilt_x_axis = -1;
        xi2->tablets[i].tilt_y_axis = -1;
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
    /* XISetMask, mirrored: byte (type >> 3), bit (type & 7). Bits
     * 4/5/6 are the REAL protocol's ButtonPress/ButtonRelease/
     * Motion (1.1.0-rc2 — the old 3/4/5 selected KeyRelease). */
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
     * devices report evtype 18..20, not 4/5/6). The evtype gate IS
     * the cast guard: only XIDeviceEvent-shaped evtypes (4/5/6) are
     * ever cast; raw/hierarchy/device-changed/property traffic never
     * reaches the cast (the default arm below consumes them). */
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
 *   - DETECTION (1.1.0-rc2 — the stylus-evidence rule): at XI2 load,
 *     XIQueryDevice(XIAllDevices) scans the slave pointers for
 *     STYLUS EVIDENCE — an absolute axis labeled "Abs Pressure"
 *     (labeled path), or the unlabeled-tablet shape (no relative
 *     axes, >=3 absolute, "Abs X"/"Abs Y" present; pressure at
 *     position 2, tilt at 3/4). A device with any RELATIVE axis is
 *     never a tablet: the old >=3-axes-any-mode rule classified
 *     wheel mice, XTEST pointers and touchpads — the owner's
 *     stylus-less desktop reported "4 tablet device(s)" and fed
 *     mouse traffic into the valuator reader (the D-3 crash).
 *     Every verdict logs its evidence at INFO, once.
 *   - SELECTION: windows select XI_Motion + XI_ButtonPress/Release
 *     (the REAL evtypes 6/4/5 — 1.1.0-rc2 corrected the off-by-one that
 *     selected KeyRelease) on the master alongside the touch masks.
 *     Core MotionNotify is NOT suppressed by this (only XI 2.2
 *     touch suppresses its core twin) — the ordinary pointer path
 *     keeps flowing; tablet events are ADDITIVE, which is exactly
 *     how a stylus-as-pointer behaves on X11 (apps pick which
 *     stream to consume).
 *   - TRANSLATION: an event whose sourceid is a tablet becomes a
 *     TABLET event — motion with pressure/tilt from the valuator
 *     state, button 1 (the driver-mapped TIP) as DOWN/UP, buttons
 *     2+ as BUTTON. Every valuator read is bounded by the device's
 *     real axis count, the mask's byte length, and the packed
 *     values indexing (xi2_valuator, 1.1.0-rc2 — the function the
 *     crash died in). Pressure normalization: raw driver units
 *     normalize through the 65535 scale, [0,1] clamps, NaN to 0.
 *     Tilt passes through in degrees (NaN/absurd to 0).
 *   - PROXIMITY: XI2 has NO proximity events for clients (X Input
 *     1.x's ProximityIn/Out never crossed into XI2). FDK
 *     synthesizes PROXIMITY_IN before a tablet's first event of a
 *     session and delivers NO OUT — the tool simply goes silent on
 *     X11, an honest platform gap (D-030) apps see as "the session
 *     ended when the events stopped".
 *
 * The rig cannot mint XI2 tablet valuators (XTEST fakes no axes),
 * so the translation layer is proven three ways (1.1.0-rc2): the test
 * seam (fdk_x11_xi2_test_tablet) for the dispatch shapes; a
 * hostile-cookie rig that drives the REAL cookie handler with
 * synthetic garbage (the crash's regression pin); and REAL XTEST
 * button/motion cookies through the full dispatch path with a
 * tablet record injected for the XTEST pointer. Real-hardware
 * status: implemented — should work (untested on real hardware). */

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
 * the scan reads are spelled — including `mode` (1.1.0-rc2): relative vs
 * absolute is the classification's core stylus evidence). */
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
    int mode;      /* XIModeRelative(0) / XIModeAbsolute(1)         */
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

#ifdef FDK_XI2_HEADER_VERIFY
#define FDK_XI2_ASSERT(cond) _Static_assert((cond), "XI2 mirror drifted")
FDK_XI2_ASSERT(sizeof(struct fdk_xi2_any_class) == sizeof(XIAnyClassInfo));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_any_class, sourceid) ==
               offsetof(XIAnyClassInfo, sourceid));
FDK_XI2_ASSERT(sizeof(struct fdk_xi2_valuator_class) ==
               sizeof(XIValuatorClassInfo));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_valuator_class, number) ==
               offsetof(XIValuatorClassInfo, number));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_valuator_class, label) ==
               offsetof(XIValuatorClassInfo, label));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_valuator_class, mode) ==
               offsetof(XIValuatorClassInfo, mode));
FDK_XI2_ASSERT(sizeof(struct fdk_xi2_device_info) == sizeof(XIDeviceInfo));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_info, use) ==
               offsetof(XIDeviceInfo, use));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_info, num_classes) ==
               offsetof(XIDeviceInfo, num_classes));
FDK_XI2_ASSERT(offsetof(struct fdk_xi2_device_info, classes) ==
               offsetof(XIDeviceInfo, classes));
#undef FDK_XI2_ASSERT
#endif /* FDK_XI2_HEADER_VERIFY */

/* ---- classification (1.1.0-rc2) ----------------------------------------------
 *
 * A device is a tablet ONLY on stylus evidence — never on axis count
 * alone (the old rule: any slave pointer with >= 3 axes, which is a
 * wheel mouse, an XTEST pointer, a touchpad's scroll axes; the owner's
 * stylus-less desktop reported "4 tablet device(s) detected" and fed
 * ordinary mouse traffic into the valuator reader). Evidence, in
 * strength order:
 *
 *   1. LABELED: an axis labeled "Abs Pressure" on a device whose axes
 *      are ALL absolute (every modern stack — libinput, wacom, evdev —
 *      labels tablet axes; mice/touchpads label Rel* or nothing).
 *   2. POSITIONAL: no labels on the pressure axis, but the device has
 *      NO relative axes, >= 3 absolute axes, and axes labeled "Abs X"
 *      and "Abs Y" — the universal unlabeled-tablet layout (x, y,
 *      pressure at positions 0/1/2; tilt at 3/4 with five+ absolute
 *      axes). Old Wacom drivers without labels land here.
 *
 * Anything with a relative axis is never a tablet (mice, XTEST
 * pointers, touchpads, KVM virtual pointers, power-button/video-bus
 * pointer attachments — the entire population the old rule swallowed).
 * The shape collection and the verdict are pure functions over
 * (number, label, mode) triples — the classification regression test
 * drives them synthetically, no display required.
 */
enum {
    XI2_ABS_TRACK = 8, /* absolute axis numbers tracked for the fallback */
};
struct xi2_label_atoms {
    unsigned long pressure, tilt_x, tilt_y, abs_x, abs_y;
};
struct xi2_axis_shape {
    int n_axes;  /* valuator classes counted                            */
    int n_abs;   /* axes with mode == absolute                          */
    int n_rel;   /* axes with mode == relative                          */
    int pressure;     /* axis number labeled Abs Pressure, -1          */
    int tilt_x, tilt_y; /* Abs Tilt X / Abs Tilt Y, -1                 */
    int abs_x, abs_y;  /* Abs X / Abs Y, -1                            */
    int abs_order[XI2_ABS_TRACK]; /* absolute axis numbers, ascending */
};
enum xi2_tablet_verdict {
    XI2_TABLET_NO = 0,
    XI2_TABLET_LABELED,   /* Abs Pressure label, absolute-only device  */
    XI2_TABLET_POSITIONAL /* unlabeled pressure, absolute x/y/+3 shape  */
};

/* Collects one valuator class into the shape (pure — the regression
 * test drives this directly with synthetic atoms). Sane axis numbers
 * only (0..511): a broken class list can not smuggle an index. */
static void xi2_axis_shape_add(struct xi2_axis_shape *s,
                               const struct xi2_label_atoms *atoms,
                               int number, unsigned long label, int mode) {
    if (number < 0 || number > 511) {
        return; /* not an axis index any real device carries */
    }
    s->n_axes++;
    if (mode == XI2_MODE_ABSOLUTE) {
        s->n_abs++;
        if (s->n_abs <= XI2_ABS_TRACK) {
            s->abs_order[s->n_abs - 1] = number;
            /* keep ascending: driver order is normally already
             * ascending, insertion handles the odd one out */
            for (int i = s->n_abs - 1; i > 0 &&
                 s->abs_order[i - 1] > s->abs_order[i]; i--) {
                int t = s->abs_order[i - 1];
                s->abs_order[i - 1] = s->abs_order[i];
                s->abs_order[i] = t;
            }
        }
    } else {
        s->n_rel++;
    }
    if (label != 0 /* None */ && mode == XI2_MODE_ABSOLUTE) {
        if (label == atoms->pressure) {
            s->pressure = number;
        } else if (label == atoms->tilt_x) {
            s->tilt_x = number;
        } else if (label == atoms->tilt_y) {
            s->tilt_y = number;
        } else if (label == atoms->abs_x) {
            s->abs_x = number;
        } else if (label == atoms->abs_y) {
            s->abs_y = number;
        }
    }
}

/* The verdict (pure). Returns XI2_TABLET_* and, when not NO, fills
 * out's axis fields; the caller fills deviceid/n_axes/tool_type. */
static enum xi2_tablet_verdict xi2_classify_tablet(
    const struct xi2_axis_shape *s, struct fdk_x11_tablet *out) {
    if (s->n_rel == 0 && s->pressure >= 0) {
        out->pressure_axis = s->pressure;
        out->tilt_x_axis = s->tilt_x;
        out->tilt_y_axis = s->tilt_y;
        return XI2_TABLET_LABELED;
    }
    if (s->n_rel == 0 && s->n_abs >= 3 && s->abs_x >= 0 && s->abs_y >= 0) {
        out->pressure_axis = s->abs_order[2];
        out->tilt_x_axis = (s->n_abs >= 5) ? s->abs_order[3] : -1;
        out->tilt_y_axis = (s->n_abs >= 5) ? s->abs_order[4] : -1;
        return XI2_TABLET_POSITIONAL;
    }
    return XI2_TABLET_NO;
}

/* The device scan at XI2 load: fills conn->xi2->tablets. Logs every
 * accepted device at INFO with its evidence, once (per connection —
 * the scan runs once); rejected axis-bearing pointer slaves get one
 * DEBUG line each so a desktop diagnosis can say exactly why the
 * mouse is not a tablet. */
static void xi2_scan_tablets(fdk_platform_connection *conn,
                            struct fdk_x11_xi2 *xi2) {
    xi2->tablet_count = 0;
    if (xi2->query_device == NULL) {
        return;
    }
    /* The axis-label atoms, resolved once (None = unlabeled axes
     * everywhere: the positional fallback carries the scan). */
    xi2->atom_pressure =
        (unsigned long)XInternAtom(conn->display, "Abs Pressure", True);
    xi2->atom_tilt_x =
        (unsigned long)XInternAtom(conn->display, "Abs Tilt X", True);
    xi2->atom_tilt_y =
        (unsigned long)XInternAtom(conn->display, "Abs Tilt Y", True);
    xi2->atom_abs_x =
        (unsigned long)XInternAtom(conn->display, "Abs X", True);
    xi2->atom_abs_y =
        (unsigned long)XInternAtom(conn->display, "Abs Y", True);
    struct xi2_label_atoms atoms = {
        xi2->atom_pressure, xi2->atom_tilt_x, xi2->atom_tilt_y,
        xi2->atom_abs_x, xi2->atom_abs_y,
    };

    int count = 0;
    struct fdk_xi2_device_info *infos =
        xi2->query_device(conn->display, /* XIAllDevices = */ 0, &count);
    if (infos == NULL) {
        return;
    }
    int pointer_slaves = 0;
    for (int i = 0; i < count && xi2->tablet_count < FDK_X11_TABLETS;
         i++) {
        struct fdk_xi2_device_info *info = &infos[i];
        if (info->use != XI2_SLAVE_POINTER &&
            info->use != XI2_FLOATING_SLAVE) {
            continue; /* masters aggregate; keyboards are irrelevant */
        }
        pointer_slaves++;
        struct xi2_axis_shape shape;
        memset(&shape, 0, sizeof(shape));
        shape.pressure = -1;
        shape.tilt_x = -1;
        shape.tilt_y = -1;
        shape.abs_x = -1;
        shape.abs_y = -1;
        for (int c = 0; c < info->num_classes; c++) {
            struct fdk_xi2_any_class *any = info->classes != NULL
                                                ? info->classes[c]
                                                : NULL;
            if (any == NULL || any->type != XI2_VALUATOR_CLASS) {
                continue;
            }
            struct fdk_xi2_valuator_class *vc =
                (struct fdk_xi2_valuator_class *)any;
            xi2_axis_shape_add(&shape, &atoms, vc->number, vc->label,
                               vc->mode);
        }
        struct fdk_x11_tablet t;
        memset(&t, 0, sizeof(t));
        t.deviceid = info->deviceid;
        t.n_axes = shape.n_axes;
        t.pressure_axis = -1;
        t.tilt_x_axis = -1;
        t.tilt_y_axis = -1;
        t.tool_type = FDK_TABLET_TOOL_PEN; /* the axis shape's
                                             * default; names are
                                             * driver-spelled noise */
        t.in_range = 0;
        enum xi2_tablet_verdict v = xi2_classify_tablet(&shape, &t);
        const char *name = info->name != NULL ? info->name : "?";
        if (v == XI2_TABLET_LABELED) {
            FDK_INFO("X Input: tablet '%s' (id %d): Abs Pressure label "
                     "— pressure axis %d, tilt %d/%d, %d axes",
                     name, info->deviceid, t.pressure_axis, t.tilt_x_axis,
                     t.tilt_y_axis, t.n_axes);
        } else if (v == XI2_TABLET_POSITIONAL) {
            FDK_INFO("X Input: tablet '%s' (id %d): unlabeled absolute "
                     "x/y/+%d-axis shape — pressure axis %d, %d axes",
                     name, info->deviceid, t.n_axes, t.pressure_axis,
                     t.n_axes);
        } else {
            if (shape.n_axes > 0) {
                FDK_DEBUG("X Input: '%s' (id %d): not a tablet — %d "
                          "axis(es), %d absolute, no stylus evidence",
                          name, info->deviceid, shape.n_axes, shape.n_abs);
            }
            continue;
        }
        xi2->tablets[xi2->tablet_count++] = t;
    }
    xi2->free_device_info(infos);
    if (xi2->tablet_count > 0) {
        FDK_INFO("X Input: %d tablet device(s) — stylus events "
                 "available",
                 xi2->tablet_count);
    } else {
        FDK_INFO("X Input: no tablet device (no stylus evidence among "
                 "%d pointer device(s)) — tablet events disabled",
                 pointer_slaves);
    }
}

/* Valuator extraction (1.1.0-rc2 hardening — this is the function the
 * D-3 crash died in). The mask's set bits, in order, index the values
 * array. EVERY input is untrusted: the axis is bounded by the
 * device's real axis count (from the scan) before any arithmetic;
 * the mask length is sanity-bounded (bytes, not the wire's 4-byte
 * units — libXi already converted, and the old *4 both overflowed
 * signed int on garbage and over-read the allocation 4x where the
 * offsets were right); the pointers are NULL-checked; all indexing
 * arithmetic is size_t. Returns 1 and writes *out when the axis is
 * present. */
static int xi2_valuator(const struct fdk_xi2_device_event *dev,
                        const struct fdk_x11_tablet *tablet, int axis,
                        double *out) {
    if (dev == NULL || tablet == NULL || out == NULL) {
        return 0;
    }
    if (axis < 0) {
        return 0; /* the device has no such axis */
    }
    if (axis >= tablet->n_axes) {
        return 0; /* beyond the device's REAL valuator count — never
                   * index a mask bit for an axis the device lacks */
    }
    int mask_len = dev->tail.valuators_mask_len; /* BYTES (libXi) */
    if (mask_len <= 0 || mask_len > XI2_MASK_BYTES_MAX) {
        return 0; /* empty, or not a length any real server marshals */
    }
    if (dev->tail.valuators_mask == NULL ||
        dev->tail.valuator_values == NULL) {
        return 0;
    }
    size_t byte = (size_t)axis >> 3;
    if (byte >= (size_t)mask_len) {
        return 0;
    }
    if ((dev->tail.valuators_mask[byte] & (1 << (axis & 7))) == 0) {
        return 0; /* present mask bit clear: value not in this event */
    }
    /* Count set bits below `axis` — the values array is packed. The
     * loop is bounded by both the axis and the mask's own bytes. */
    size_t idx = 0;
    for (int b = 0; b < axis; b++) {
        size_t bb = (size_t)b >> 3;
        if (bb < (size_t)mask_len &&
            (dev->tail.valuators_mask[bb] & (1 << (b & 7))) != 0) {
            idx++;
        }
    }
    /* idx < popcount(mask) by construction (axis's own bit is set),
     * and the values array is exactly popcount doubles — libXi's
     * cookie contract. The old code indexed it off a misaligned
     * mask pointer instead; see the header block. */
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
        return 0; /* a device we never classified: ordinary pointer
                   * traffic — ignored, never indexed (1.1.0-rc2) */
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
    (void)xi2_valuator(dev, tablet, tablet->pressure_axis, &pressure);
    (void)xi2_valuator(dev, tablet, tablet->tilt_x_axis, &tilt_x);
    (void)xi2_valuator(dev, tablet, tablet->tilt_y_axis, &tilt_y);
    /* Pressure normalization: X valuator axes carry raw driver units
     * (the common Wacom scale is 0..65535); the class min/max is not
     * in the EVENT, so the practical normalize is the 65535 clamp —
     * drivers that report 0..1 land below the guard and pass through
     * (values outside [0,1] clamp; the seam pins both shapes). NaN
     * guards to 0 (a hostile value can not poison the ink). */
    if (!(pressure >= 0.0)) { /* NaN or negative */
        pressure = 0.0;
    }
    if (pressure > 1.0) {
        pressure = pressure / 65535.0;
    }
    if (pressure > 1.0) {
        pressure = 1.0;
    }
    if (!(tilt_x >= -360.0 && tilt_x <= 360.0)) {
        tilt_x = 0.0; /* NaN/absurd tilt is absent, not ink */
    }
    if (!(tilt_y >= -360.0 && tilt_y <= 360.0)) {
        tilt_y = 0.0;
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
        if (dev->detail <= 0 || dev->detail > 255) {
            return 1; /* not a button number any driver maps —
                       * consumed untranslated (1.1.0-rc2) */
        }
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

/* ---- 1.1.0-rc2 test seams: tablet injection + classification driver ------- */

void fdk_x11_xi2_test_inject_tablet(fdk_platform_connection *conn,
                                    int deviceid, int n_axes,
                                    int pressure_axis, int tilt_x_axis,
                                    int tilt_y_axis) {
    if (conn == NULL || conn->xi2 == NULL) {
        return;
    }
    struct fdk_x11_tablet *t = &conn->xi2->tablets[0];
    memset(t, 0, sizeof(*t));
    t->deviceid = deviceid;
    t->n_axes = n_axes;
    t->pressure_axis = pressure_axis;
    t->tilt_x_axis = tilt_x_axis;
    t->tilt_y_axis = tilt_y_axis;
    t->tool_type = FDK_TABLET_TOOL_PEN;
    t->in_range = 0;
    conn->xi2->tablet_count = 1;
}

void fdk_x11_xi2_test_clear_tablets(fdk_platform_connection *conn) {
    if (conn == NULL || conn->xi2 == NULL) {
        return;
    }
    conn->xi2->tablet_count = 0;
    for (int i = 0; i < FDK_X11_TABLETS; i++) {
        conn->xi2->tablets[i].deviceid = -1;
        conn->xi2->tablets[i].n_axes = 0;
        conn->xi2->tablets[i].pressure_axis = -1;
        conn->xi2->tablets[i].tilt_x_axis = -1;
        conn->xi2->tablets[i].tilt_y_axis = -1;
    }
}

int fdk_x11_xi2_test_classify(const int *numbers,
                              const unsigned long *labels,
                              const int *modes, int n,
                              struct fdk_x11_tablet *out) {
    struct xi2_label_atoms atoms = {
        FDK_XI2_TEST_ATOM_PRESSURE, FDK_XI2_TEST_ATOM_TILT_X,
        FDK_XI2_TEST_ATOM_TILT_Y, FDK_XI2_TEST_ATOM_ABS_X,
        FDK_XI2_TEST_ATOM_ABS_Y,
    };
    struct xi2_axis_shape shape;
    memset(&shape, 0, sizeof(shape));
    shape.pressure = -1;
    shape.tilt_x = -1;
    shape.tilt_y = -1;
    shape.abs_x = -1;
    shape.abs_y = -1;
    for (int i = 0; i < n; i++) {
        xi2_axis_shape_add(&shape, &atoms, numbers[i], labels[i],
                           modes[i]);
    }
    memset(out, 0, sizeof(*out));
    out->deviceid = -1;
    out->pressure_axis = -1;
    out->tilt_x_axis = -1;
    out->tilt_y_axis = -1;
    out->n_axes = shape.n_axes;
    switch (xi2_classify_tablet(&shape, out)) {
    case XI2_TABLET_LABELED:
        return 1;
    case XI2_TABLET_POSITIONAL:
        return 2;
    default:
        return 0;
    }
}
