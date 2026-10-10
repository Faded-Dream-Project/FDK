#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlocale.h>
#include <X11/extensions/shape.h> /* XShape input regions (1.3.2) */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <locale.h>

/* ---- Display-death discipline (1.3.0) ---------------------------------
 *
 * Xlib's default XIO error handler calls exit(1): a lost X server
 * (logout, WM crash, ssh -X disconnect) killed the process with no
 * application say. The core loop already HAS the right contract —
 * a negative dispatch_pending result propagates out of the pump and
 * ends fdk_run cleanly — but Xlib never let us get there.
 *
 * The handler below fixes that in two tiers:
 *
 *      1. While dispatch_pending is draining (fdk__x11_io_armed),
 *      a fatal IO error longjmps back to its frame — the display is
 *      dead, the interrupted Xlib call will never be resumed, and
 *      FDK is single-threaded UI-only (no XInitThreads, so Xlib's
 *      locking macros are no-ops — no lock is left held).
 *      dispatch_pending then reports the negative result the pump
 *      propagates.
 *
 *   2. Outside the guarded region (an X call in application-driven
 *      present()/set_title/... code), the handler flags the
 *      connection and RETURNS — and Xlib's exit(1) after the
 *      handler runs is documented, unavoidable process death: the
 *      flag at least keeps every later teardown path from piling
 *      more X requests onto a dead socket if control somehow
 *      resumes.
 */
static fdk_platform_connection *g_io_conn = NULL;
sigjmp_buf fdk__x11_io_jmp;
volatile int fdk__x11_io_armed = 0;

static int x11_io_error_handler(Display *display) {
    (void)display;
    FDK_ERROR("X server connection lost (fatal IO error)");
    if (g_io_conn != NULL) {
        g_io_conn->display_dead = 1;
    }
    if (fdk__x11_io_armed) {
        fdk__x11_io_armed = 0;
        siglongjmp(fdk__x11_io_jmp, 1);
    }
    return 0; /* unreachable when armed; lets Xlib exit when not */
}


/* HiDPI scale detection (1.4.5).
 *
 * X11 has no scale protocol; two conventions exist, and we read
 * them in priority order, pure Xlib (no Xsettings daemon, no
 * Xrandr dependency):
 *
 *   1. The "Xft.dpi" resource string - what real desktop sessions
 *      set through xrdb (and what XSETTINGS-synced environments
 *      mirror into the RESOURCE_MANAGER property XGetDefault
 *      reads). This is the same source GDK's X11 backend trusts.
 *
 *   2. The screen's own physical metric: pixels per millimeter
 *      from the core protocol's width/widthmm - what a bare
 *      "Xvfb -dpi N" establishes, which is exactly how the test
 *      rig drives a scale-2 environment end to end.
 *
 * scale = round(dpi / 96) clamped to [1, 3]. INTEGER ONLY on
 * purpose: the core's HiDPI compositing path is the
 * nearest-neighbor block blit (fdk_window_paint), and a
 * fractional X11 scale would shimmer on every text edge instead
 * of staying pixel-exact. 96 dpi (the X convention) maps to 1;
 * 144 dpi maps to 2; a 75-dpi projector still maps to 1 (never
 * downscale).
 */
int fdk__x11_detect_scale(Display *display) {
    double dpi = -1.0;

    const char *xft = XGetDefault(display, "Xft", "dpi");
    if (xft != NULL) {
        dpi = atof(xft);
        if (dpi < 0.0 || dpi > 10000.0) {
            dpi = -1.0; /* pathological resource: fall through */
        }
    }
    if (dpi <= 0.0) {
        Screen *scr = ScreenOfDisplay(display, DefaultScreen(display));
        int px = WidthOfScreen(scr);
        int mm = WidthMMOfScreen(scr);
        if (mm > 0 && px > 0) {
            dpi = ((double)px * 25.4) / (double)mm;
        }
    }
    if (dpi <= 0.0) {
        return 1; /* no honest signal: unscaled */
    }
    int scale = (int)(dpi / 96.0 + 0.5);
    if (scale < 1) {
        scale = 1; /* never downscale */
    }
    if (scale > 3) {
        scale = 3; /* 300% is the practical ceiling */
    }
    if (scale > 1) {
        FDK_INFO("HiDPI: scale %d (detected %.1f dpi) - windows are "
                 "physical-sized, the tree composites up",
                 scale, dpi);
    }
    return scale;
}

fdk_result fdk_x11_connect(fdk_platform_dispatch_fn dispatch,
                               void *dispatch_user_data, const char *app_id,
                               fdk_platform_connection **out_conn) {
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        FDK_INFO("XOpenDisplay failed (no X11 display reachable)");
        return FDK_ERR_NO_DISPLAY;
    }

    fdk_platform_connection *conn = fdk_alloc(sizeof(fdk_platform_connection));
    if (conn == NULL) {
        XCloseDisplay(display);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    conn->display = display;
    conn->screen = DefaultScreen(display);
    conn->root = RootWindow(display, conn->screen);

    /* HiDPI scale (1.4.5): detected ONCE at connect - see
     * fdk__x11_detect_scale below for the two conventions and the
     * integer-only rule. */
    conn->scale = fdk__x11_detect_scale(display);

    conn->dispatch = dispatch;
    conn->dispatch_user_data = dispatch_user_data;
    conn->windows = NULL;
    conn->window_count = 0;
    conn->window_capacity = 0;
    conn->app_id = NULL;
    conn->display_dead = 0;
    conn->detectable_repeat = 0;
    memset(conn->key_down, 0, sizeof conn->key_down);
    g_io_conn = conn;
    (void)XSetIOErrorHandler(x11_io_error_handler);

    /* Key-repeat discipline (1.3.0): ask the XKB extension for
     * DETECTABLE auto-repeat — real KeyReleases only, repeats as
     * plain KeyPresses — the same request GTK issues at startup.
     * The event translator's key-down bitmap turns those into
     * is_repeat. Without the extension the honest degradation is
     * is_repeat 0 everywhere (X's fake release+press pairs are
     * indistinguishable from fast typing; v1 shipped exactly that). */
    {
        Bool supported = False;
        if (XkbSetDetectableAutoRepeat(display, True, &supported) &&
            supported) {
            conn->detectable_repeat = 1;
            FDK_DEBUG("XKB detectable auto-repeat active — key events "
                      "report is_repeat");
        } else {
            FDK_DEBUG("XKB detectable auto-repeat unavailable — "
                      "is_repeat stays 0 (indistinguishable pairs)");
        }
    }

    /* X Input Method (1.3.1): bring up the connection-wide XIM so
     * per-window XICs can resolve full-Unicode text input. Only the
     * CTYPE locale is touched, and only when it is still the "C"
     * default (an application that set its own locale keeps it);
     * Xutf8LookupString needs a UTF-8 locale to decode non-ASCII. A
     * failed XOpenIM (no IM support on the server) is not an error —
     * the ASCII fallback path keeps running. */
    conn->xim = NULL;
    {
        const char *cur = setlocale(LC_CTYPE, NULL);
        if (cur == NULL || strcmp(cur, "C") == 0) {
            (void)setlocale(LC_CTYPE, "");
        }
        XSetLocaleModifiers("");
        conn->xim = XOpenIM(display, NULL, NULL, NULL);
        if (conn->xim != NULL) {
            FDK_INFO("X input method opened — full-Unicode text entry "
                     "via Xutf8LookupString");
        } else {
            FDK_INFO("X input method unavailable — ASCII text-entry "
                     "fallback (non-ASCII input will not resolve)");
        }
    }

    /* XSHAPE probe (1.3.2): the empty-input-region primitive behind
     * click-through tooltips. */
    {
        int dummy = 0;
        if (XShapeQueryExtension(display, &dummy, &dummy)) {
            conn->shape_ok = 1;
        } else {
            conn->shape_ok = 0;
            FDK_INFO("XSHAPE unavailable — tooltip popups will not be "
                     "click-through");
        }
    }
    /* fdk_alloc does not zero — None is 0 but be explicit anyway:
     * no font cursor exists until window_set_cursor first needs one. */
    for (int i = 0; i < 9; i++) {
        conn->resize_cursors[i] = None;
    }

    /* app_id (when set) rides along on the connection and becomes
     * every window's WM_CLASS — the X11 identity mechanism window
     * managers match for rules and grouping. */
    if (app_id != NULL && app_id[0] != '\0') {
        size_t len = strlen(app_id) + 1;
        conn->app_id = malloc(len); /* Xlib-free storage; freed with
                                     * free() at disconnect */
        if (conn->app_id != NULL) {
            memcpy(conn->app_id, app_id, len);
        }
    }

    /* MIT-SHM probe (Phase 3 completion): use the shared-memory fast
     * path for present() when the server supports the extension and
     * the environment has not opted out (FDK_NO_MIT_SHM=1 — escape
     * hatch for servers that implement it brokenly and for
     * debugging/measuring the copy path). Probing once here keeps
     * per-window acquisition cheap and makes the capability a
     * property of the CONNECTION, which is what the protocol says it
     * is. */
    conn->shm_ok = 0;
    conn->shm_event_base = 0;
    if (getenv("FDK_NO_MIT_SHM") == NULL) {
        int major = 0, minor = 0, pixmaps = 0;
        if (XShmQueryVersion(display, &major, &minor,
                             (Bool *)&pixmaps) == True) {
            conn->shm_ok = 1;
            conn->shm_event_base = XShmGetEventBase(display);
            FDK_DEBUG("MIT-SHM available (v%d.%d) — presentation uses "
                      "the shared-memory path", major, minor);
        }
    } else {
        FDK_DEBUG("FDK_NO_MIT_SHM set — presentation uses the copy path");
    }

    /* WM_DELETE_WINDOW: without registering for this via WM_PROTOCOLS,
     * the window manager would just kill our connection when the user
     * clicks the close button, giving the application no chance to
     * respond (see fdk_event.h, FDK_EVENT_WINDOW_CLOSE_REQUEST). This
     * is the standard ICCCM mechanism, not a desktop-specific hack. */
    conn->wm_protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    conn->wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    conn->net_wm_name = XInternAtom(display, "_NET_WM_NAME", False);
    conn->utf8_string = XInternAtom(display, "UTF8_STRING", False);
    conn->motif_wm_hints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    conn->net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    conn->net_wm_state_maximized_vert =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    /* NOTE the spelling: the EWMH spec atom is ..._HORZ (not
     * ..._HORIZ). A misspelled name here does NOT fail loudly —
     * XInternAtom with only-if-exists=False CREATES a fresh atom
     * nobody else uses, so the _NET_SUPPORTED probe below never
     * matches it, every WM looks "maximize-incapable", and the
     * maximize path silently degrades to the bare-X fallback under
     * real window managers (exactly the 1.1.3 bug: window maximized
     * by the WM, FDK's flag disagreeing). tests/test_x11_integration.c
     * pins the spec spelling against this. */
    conn->net_wm_state_maximized_horiz =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    conn->net_wm_state_fullscreen =
        XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    conn->net_wm_moveresize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    conn->wm_state = XInternAtom(display, "WM_STATE", False);
    conn->wm_change_state = XInternAtom(display, "WM_CHANGE_STATE", False);

    /* Probe the WM's EWMH capabilities ONCE, from the root's
     * _NET_SUPPORTED atom list — the EWMH-sanctioned capability
     * discovery. No list (or an empty one) means no EWMH WM is
     * running (bare X, Xvfb, or a pre-EWMH WM), which is exactly what
     * the bare-X fallback paths in x11_window.c key off. */
    {
        Atom net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
        Atom type = None;
        int format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;
        if (XGetWindowProperty(display, conn->root, net_supported,
                               0, 1024, False, XA_ATOM, &type, &format,
                               &nitems, &bytes_after, &prop) == Success &&
            type == XA_ATOM && format == 32 && nitems > 0) {
            conn->ewmh_wm = 1;
            Atom *atoms = (Atom *)prop;
            int have_vert = 0, have_horiz = 0;
            for (unsigned long i = 0; i < nitems; i++) {
                if (atoms[i] == conn->net_wm_state_maximized_vert) {
                    have_vert = 1;
                }
                if (atoms[i] == conn->net_wm_state_maximized_horiz) {
                    have_horiz = 1;
                }
            }
            conn->ewmh_state_ok = (have_vert && have_horiz) ? 1 : 0;
        }
        if (prop != NULL) {
            XFree(prop);
        }
    }
    FDK_INFO("EWMH window manager %s (maximize support: %s)",
             conn->ewmh_wm ? "detected" : "not detected",
             conn->ewmh_state_ok ? "yes" : "no");

    /* Clipboard helper + atoms (Phase 9). Failure is not fatal — the
     * clipboard ops simply report FDK_ERR_PLATFORM / NULL. */
    conn->clip_helper = None;
    conn->clip_owned_text = NULL;
    if (fdk_ok(fdk_x11_clipboard_init(conn))) {
        FDK_DEBUG("clipboard helper window ready (Phase 9)");
    }

    /* XDND atoms + idle state (1.2.0); cannot fail in practice
     * (interning only), and a failure leaves the DnD ops inert
     * rather than broken — same tolerance as the clipboard. */
    (void)fdk_x11_dnd_init(conn);
    conn->xdnd.source = None;
    conn->xdnd.dest_window = None;
    conn->xdnd.hover = NULL;
    conn->xdnd.offered = 0;

    FDK_INFO("connected (screen %d, %dx%d root)", conn->screen,
             DisplayWidth(display, conn->screen),
             DisplayHeight(display, conn->screen));

    *out_conn = conn;
    return FDK_OK;
}

void fdk_x11_disconnect(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }

    if (conn->window_count > 0) {
        FDK_WARN("disconnecting with %zu window(s) still open — "
                 "force-destroying (caller should have destroyed them "
                 "explicitly; see fdk_shutdown() in fdk_core.h)",
                 conn->window_count);
        /* fdk_x11_window_destroy() mutates conn->windows via
         * fdk_x11_unregister_window(), so iterate defensively from
         * the end rather than assuming indices stay stable. */
        while (conn->window_count > 0) {
            fdk_x11_window_destroy(conn->windows[conn->window_count - 1]);
        }
    }
    fdk_free(conn->windows);

    fdk_x11_clipboard_shutdown(conn);

    fdk_x11_dnd_shutdown(conn);

    fdk_x11_cursor_shutdown(conn);

    free(conn->app_id);
    conn->app_id = NULL;

    if (conn->display_dead) {
        /* The server is gone: XCloseDisplay would walk straight back
         * into the fatal IO path. The socket dies with the process
         * or the close-on-exit — leak the Display handle honestly. */
        FDK_INFO("disconnecting (display already dead)");
        g_io_conn = NULL;
        fdk_free(conn);
        return;
    }
    if (conn->xim != NULL) {
        XCloseIM(conn->xim);
        conn->xim = NULL;
    }
    FDK_INFO("disconnecting");
    XCloseDisplay(conn->display);
    g_io_conn = NULL;
    fdk_free(conn);
}

int fdk_x11_get_event_fd(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return -1;
    }
    return ConnectionNumber(conn->display);
}
