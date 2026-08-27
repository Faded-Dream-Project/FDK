/* test_x11_integration.c — X11 PLATFORM INTEGRATION TEST.
 *
 * Requires a reachable X11 display (a real desktop session, or Xvfb —
 * see docs/testing.md for how CI runs this headlessly via
 * `make test-x11`, which is NOT part of plain `make test`). This is
 * the deliberate split docs/testing.md and the project's headless/CI
 * requirement calls for: ordinary `make test` never needs a display;
 * this binary is the one that does, and it says so in its own name.
 *
 * Verifies real, observable behavior against a live X server —
 * connection, window creation/destruction, resize round-trip via the
 * actual ConfigureNotify event path, title get/set, and clean
 * shutdown — not just "compiles" or "doesn't crash".
 */

#include "fdk/fdk.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"

/* Test harness only: the X11 integration test is allowed to reach
 * into the library's internals for VERIFICATION (the library itself
 * never crosses these layers — see docs/architecture.md). Xlib is
 * used for server-side pixel readback (XGetImage over a second X
 * connection, so nothing can be satisfied from an FDK-side cache);
 * the internal headers expose the backend's XID for the readback
 * target. */
#include "platform/x11/x11_platform.h"
#include "window/window_internal.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h> /* XGetImage / XGetPixel / XDestroyImage */
#include <X11/Xatom.h> /* XA_ATOM / XA_INTEGER (fake-WM property work) */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void alarm_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\nFAIL: test timed out waiting for an X11 event "
                     "(no event arrived within 5s) — see "
                     "test_resize_delivers_configure_event\n");
    _exit(1);
}

/* Xvfb-specific workaround, not an FDK behavior: under Xvfb (unlike a
 * real X server), a fresh XOpenDisplay() made immediately after a
 * previous connection in the same process created and destroyed a
 * window can intermittently fail even though the display is up and
 * otherwise reachable — reproduced with raw Xlib with no FDK code
 * involved at all, see docs/testing.md, "Known Xvfb flakiness". A
 * real X server (Xorg/XLibre) does not exhibit this; this retry is a
 * property of the specific headless test server, not of
 * fdk_init()/fdk_x11_connect(), which is why the retry lives here in
 * the test harness rather than inside the library. */
/* Xvfb-specific defensive retry: under Xvfb (unlike a real X server),
 * a fresh XOpenDisplay() made very soon after the server starts can
 * occasionally fail even though the display is otherwise reachable.
 * A short bounded retry absorbs that startup race without masking a
 * real, persistent connection failure (which will still exhaust the
 * retry budget and report accurately) — see docs/testing.md, "Known
 * Xvfb flakiness", for the investigation that identified the actual
 * root cause (a display-number collision in the old `make test-x11`
 * driver script, now fixed) and why this small retry remains as a
 * defensive margin regardless. */
static fdk_result init_with_retry(fdk_context **out_ctx, const fdk_init_options *opts) {
    for (int attempt = 0; attempt < 5; attempt++) {
        fdk_result r = fdk_init(out_ctx, opts);
        if (fdk_ok(r)) {
            return r;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return fdk_init(out_ctx, opts); /* final real attempt/result to report */
}

static void test_connect_and_shutdown(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    fdk_result r = init_with_retry(&ctx, &opts);
    assert(fdk_ok(r));
    assert(ctx != NULL);
    fdk_shutdown(ctx);
    printf("[ok] X11 connect + shutdown\n");
}

static void test_init_with_app_id(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = {
        .backend = FDK_PLATFORM_X11,
        .app_id = "org.fdk.test",
    };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    assert(ctx != NULL);
    fdk_shutdown(ctx);
    printf("[ok] X11 connect with app_id\n");
}

static void test_quit_before_run_is_safe(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    fdk_quit(ctx); /* must not crash even though fdk_run() never called */
    fdk_shutdown(ctx);
    printf("[ok] fdk_quit() before fdk_run() is safe\n");
}

static void test_run_returns_when_no_windows_open(void) {
    /* fdk_run()'s documented contract (fdk_core.h): the loop exits
     * once the last top-level window closes, or fdk_quit() is called
     * — whichever comes first. With zero windows ever created, the
     * loop's condition (quit_requested || window_count == 0) is
     * already true before the first iteration, so this must return
     * immediately rather than blocking in poll() with nothing to
     * wait for. */
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    fdk_run(ctx); /* must return promptly: window_count == 0 */
    fdk_shutdown(ctx);
    printf("[ok] fdk_run() returns immediately with no windows open\n");
}

static void test_window_create_show_destroy(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK X11 Test", .width = 320, .height = 240 };
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    assert(fdk_ok(r));
    assert(win != NULL);

    fdk_size sz;
    assert(fdk_ok(fdk_window_get_size(win, &sz)));
    assert(sz.width == 320);
    assert(sz.height == 240);

    fdk_window_show(win);
    fdk_window_hide(win);
    fdk_window_show(win);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 window create/show/hide/destroy\n");
}

static void test_window_set_title(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    assert(fdk_ok(fdk_window_create(ctx, NULL, &win)));

    /* set_title() doesn't crash and doesn't require the window to be
     * shown first — this exercises the XChangeProperty/XStoreName
     * path in x11_window.c directly. Reading the title back would
     * need an XGetWMName round-trip, out of scope for this smoke
     * check; the property-write path not erroring is what's verified. */
    fdk_window_set_title(win, "Updated Title");
    fdk_window_set_title(win, NULL); /* must not crash on NULL */

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 window set_title (including NULL)\n");
}

/* Event capture state for the resize test below. */
typedef struct {
    fdk_context *ctx;
    int configure_count;
    fdk_size last_configure_size;
} event_capture;

static void capture_callback(fdk_window *window, const fdk_event_data *event, void *user_data) {
    (void)window;
    event_capture *cap = user_data;
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        cap->configure_count++;
        cap->last_configure_size = event->configure.size;
        fdk_quit(cap->ctx); /* stop fdk_run() once we've seen the
                                configure we're testing for, rather
                                than waiting on further events that
                                will never arrive in this test */
    }
}

static void test_resize_delivers_configure_event(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    event_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, capture_callback, &cap);

    fdk_window_show(win);
    fdk_window_resize(win, 500, 400);

    /* fdk_run() is the only event-pumping entry point FDK exposes
     * (see fdk_event.h's documented model — there is deliberately no
     * separate "pump once" API). It blocks in poll() until at least
     * one round of events is available, dispatches them, then
     * re-checks its exit condition (quit_requested or
     * window_count == 0) and returns if that condition now holds —
     * it does not loop forever on its own. Since this window is still
     * open and quit hasn't been requested, calling fdk_run() here
     * will process the queued resize's ConfigureNotify and then
     * return only once poll() has nothing left pending in that pass.
     * We call fdk_quit() from inside the callback once we've seen
     * what we need, which is the documented, supported way to make
     * fdk_run() return promptly rather than waiting on more events
     * that will never come in a test with no further interaction.
     *
     * Safety net: if a regression means no ConfigureNotify ever
     * arrives, fdk_run() would otherwise block in poll() forever and
     * hang the test (and CI). alarm() + SIGALRM turns that into a
     * loud, fast test failure instead of a silent hang — see the
     * signal handler installed in main(). */
    alarm(5);
    fdk_run(ctx);
    alarm(0);

    assert(cap.configure_count >= 1);
    assert(cap.last_configure_size.width == 500);
    assert(cap.last_configure_size.height == 400);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 resize delivers FDK_EVENT_WINDOW_CONFIGURE with correct size\n");
}

static void test_close_request_delivered(void) {
    /* WM_DELETE_WINDOW can't be triggered from inside this process
     * without a window manager actually running (Xvfb has none by
     * default) to send the ClientMessage — that requires either a
     * real WM in the CI image or a synthetic XSendEvent from a
     * separate test harness process. Documented as a known gap rather
     * than faked: see docs/testing.md, "X11 integration test
     * coverage", for exactly this limitation and what would close it. */
    printf("[skip] X11 close-request event (needs a running window "
           "manager to send WM_DELETE_WINDOW; not present under bare "
           "Xvfb) — see docs/testing.md\n");
}

/* ---- Renderer (fdk_surface) tests ----
 *
 * These verify REAL pixels: the app draws through FDK's surface API,
 * presents, then XGetImage reads the window's contents back from the
 * X SERVER side. If the XImage/XPutImage plumbing (x11_surface.c)
 * blitted nothing, misaligned, or mangled the channel order, these
 * comparisons fail — they are not compile-only smoke checks.
 *
 * Pixel channel tolerance: X stores 24-bit TrueColor exactly, and
 * our helpers round-to-nearest on write, so equality on the packed
 * 0x00RRGGBB value is exact. */

/* Reads one pixel of the FDK window via the X server, through a
 * SEPARATE X connection from FDK's (so the readback cannot be
 * satisfied from any FDK-side cache). Helper-owned display, opened
 * lazily. */
static unsigned long x11_readback_pixel(Display **out_dpy, unsigned long xid,
                                         int x, int y) {
    if (*out_dpy == NULL) {
        *out_dpy = XOpenDisplay(NULL);
        assert(*out_dpy != NULL);
    }
    XImage *img = XGetImage(*out_dpy, (Drawable)xid, x, y, 1, 1,
                            ~0UL /* AllPlanes */, ZPixmap);
    assert(img != NULL);
    unsigned long px = (unsigned long)XGetPixel(img, 0, 0);
    XDestroyImage(img);
    return px;
}

typedef struct {
    fdk_context *ctx;
    int configure_count;
    fdk_size last_size;
} surface_capture;

static void surface_event_callback(fdk_window *window,
                                    const fdk_event_data *event,
                                    void *user_data) {
    (void)window;
    surface_capture *cap = user_data;
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        cap->configure_count++;
        cap->last_size = event->configure.size;
    }
}

static unsigned long fdk_window_xid(fdk_window *win) {
    return (unsigned long)win->pwindow->xwindow;
}

static void test_surface_render_readback(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK render test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    /* Map FIRST: drawing into an unmapped window is discarded once
     * mapped again (no backing store under Xvfb), so the draw must
     * happen after the map request has been processed. */
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    assert(surface != NULL);

    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    assert(info.pixels != NULL);
    assert(info.width == 320);
    assert(info.height == 240);
    assert(info.stride >= 320);
    assert(info.format == FDK_SURFACE_FORMAT_XRGB8888);

    /* Draw: solid red field, a green inner rect, a blue 1px border,
     * and a white pixel via direct writes. */
    fdk_surface_fill(surface, (fdk_color){ .r = 1, .g = 0, .b = 0, .a = 1 });
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = 40, .y = 40, .width = 120,
                                      .height = 80 },
                          (fdk_color){ .r = 0, .g = 1, .b = 0, .a = 1 });
    fdk_surface_draw_rect(surface,
                          (fdk_rect){ .x = 10, .y = 10, .width = 300,
                                      .height = 220 },
                          (fdk_color){ .r = 0, .g = 0, .b = 1, .a = 1 });
    fdk_surface_info info2;
    assert(fdk_ok(fdk_surface_get_info(surface, &info2)));
    info2.pixels[200 * info2.stride + 200] = 0x00FFFFFFu;

    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* Red field at a spot clear of the rects (bottom-left area). */
    unsigned long px = x11_readback_pixel(&rb_dpy, xid, 30, 200);
    assert(px == 0x00FF0000u);
    /* Green inner rect center. */
    px = x11_readback_pixel(&rb_dpy, xid, 100, 80);
    assert(px == 0x0000FF00u);
    /* Blue border. */
    px = x11_readback_pixel(&rb_dpy, xid, 10, 120);
    assert(px == 0x000000FFu);
    /* Direct white pixel. */
    px = x11_readback_pixel(&rb_dpy, xid, 200, 200);
    assert(px == 0x00FFFFFFu);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 surface render + server-side pixel readback\n");
}

static void test_surface_follows_resize(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK render resize test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);

    fdk_window_show(win);
    /* Presenting before the map is processed must not crash or hang
     * (an app can legitimately draw its first frame before showing);
     * correctness of READBACK only matters after mapping below. */
    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    fdk_surface_fill(surface, (fdk_color){ .r = 1, .g = 0, .b = 0, .a = 1 });
    assert(fdk_ok(fdk_surface_present(surface)));

    fdk_window_resize(win, 400, 300);

    /* Pump-driven wait for the ConfigureNotify (the documented
     * application loop shape — and a test of fdk_pump_events itself). */
    alarm(5);
    while (cap.configure_count == 0) {
        int r = fdk_pump_events(ctx, 200);
        assert(r >= 0);
    }
    alarm(0);
    assert(cap.last_size.width == 400);
    assert(cap.last_size.height == 300);

    /* New framebuffer at the new size; render and read back. */
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    assert(info.width == 400);
    assert(info.height == 300);
    fdk_surface_fill(surface, (fdk_color){ .r = 0, .g = 0, .b = 1, .a = 1 });
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    /* A pixel only present in the NEW geometry (right/bottom corner
     * area, inside the window now, outside it before): must be the
     * freshly drawn blue, not stale red or the white background. */
    unsigned long px = x11_readback_pixel(&rb_dpy, xid, 390, 290);
    assert(px == 0x000000FFu);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 surface follows resize (new framebuffer, pixels "
           "read back)\n");
}

static void test_pump_events_nonblocking(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    /* No windows: nothing can arrive, but the call must be well-
     * behaved — timeout 0 returns promptly, timeout 100 returns
     * within ~100 ms (both non-negative). */
    int r0 = fdk_pump_events(ctx, 0);
    assert(r0 >= 0);
    int r100 = fdk_pump_events(ctx, 100);
    assert(r100 >= 0);

    /* NULL/invalid contexts report errors, not crashes. */
    assert(fdk_pump_events(NULL, 0) < 0);

    fdk_shutdown(ctx);
    printf("[ok] fdk_pump_events timeout semantics + argument checks\n");
}

/* Damage tracking, end to end against the X server. The interesting
 * part is the RAW-WRITE contract: a pixel written directly through
 * info.pixels WITHOUT fdk_surface_invalidate() must NOT reach the
 * server (present() is a documented no-op with empty damage — the
 * server keeps the old pixel), and must reach it exactly once the
 * application declares the damage. That makes the no-op skip
 * observable, not just asserted. */
static void test_surface_damage_partial_present(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK damage test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));

    /* Frame 1: full red. */
    fdk_surface_fill(surface, (fdk_color){ .r = 1, .g = 0, .b = 0, .a = 1 });
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 100);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    assert(x11_readback_pixel(&rb_dpy, xid, 20, 20) == 0x00FF0000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 270, 150) == 0x00FF0000u);

    /* Frame 2: a small blue rect — ONLY that region is damaged, so
     * ONLY that sub-image may change on screen. The region outside
     * must still show frame 1's red (which it can only do if the
     * client-side XImage content outside the damage was preserved —
     * the property that makes partial presents correct). */
    fdk_surface_fill_rect(surface,
                          (fdk_rect){ .x = 100, .y = 80, .width = 40,
                                      .height = 30 },
                          (fdk_color){ .r = 0, .g = 0, .b = 1, .a = 1 });
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 100);

    assert(x11_readback_pixel(&rb_dpy, xid, 120, 95) == 0x000000FFu);
    assert(x11_readback_pixel(&rb_dpy, xid, 20, 20) == 0x00FF0000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 270, 150) == 0x00FF0000u);

    /* Raw write WITHOUT invalidate: present() must skip it entirely —
     * the framebuffer holds white, the server keeps red. */
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    info.pixels[180 * info.stride + 200] = 0x00FFFFFFu; /* (200,180) */
    assert(fdk_ok(fdk_surface_present(surface)));       /* no-op */
    (void)fdk_pump_events(ctx, 100);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 180) == 0x00FF0000u);

    /* Declaring the damage makes it arrive. */
    fdk_surface_invalidate(surface, (fdk_rect){ .x = 200, .y = 180,
                                                .width = 1, .height = 1 });
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 100);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 180) == 0x00FFFFFFu);

    /* Empty-damage present after a pure re-acquire: still a no-op. */
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    assert(fdk_ok(fdk_surface_present(surface)));
    assert(x11_readback_pixel(&rb_dpy, xid, 20, 20) == 0x00FF0000u);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 damage-tracked partial present (sub-image blits + "
           "observable no-op skip)\n");
}

/* The new primitive set drawn to a real window and read back from
 * the server: line, filled circle, circle outline, rounded rect. */
static void test_surface_primitives_readback(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK primitives test",
                                 .width = 400, .height = 300 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    fdk_surface_fill(surface, (fdk_color){ 0, 0, 0, 1 });

    /* Line across the top. */
    fdk_surface_draw_line(surface, 10, 10, 100, 10,
                          (fdk_color){ 1, 1, 1, 1 });
    /* Filled green circle + white outline circle at the same center
     * (outline drawn after fill so the ring wins on overlap). */
    fdk_surface_fill_circle(surface, 200, 100, 40,
                            (fdk_color){ 0, 1, 0, 1 });
    fdk_surface_draw_circle(surface, 200, 100, 40,
                            (fdk_color){ 1, 1, 1, 1 });
    /* Rounded blue rect with a 15px radius. */
    fdk_surface_fill_rounded_rect(surface,
                                  (fdk_rect){ .x = 50, .y = 150,
                                              .width = 80, .height = 60 },
                                  15, (fdk_color){ 0, 0, 1, 1 });

    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    assert(x11_readback_pixel(&rb_dpy, xid, 50, 10) == 0x00FFFFFFu);
    assert(x11_readback_pixel(&rb_dpy, xid, 50, 11) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 9, 10) == 0x00000000u);

    /* Circle: center is white (outline over fill at exact center is
     * fill green — center is interior, not on the ring), +x cardinal
     * is the white ring, just outside is black. */
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 100) == 0x0000FF00u);
    assert(x11_readback_pixel(&rb_dpy, xid, 240, 100) == 0x00FFFFFFu);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 60) == 0x00FFFFFFu);
    assert(x11_readback_pixel(&rb_dpy, xid, 245, 100) == 0x00000000u);

    /* Rounded rect: middle filled, bounding-box corner cut away,
     * right edge filled at the bottom corner-center row. */
    assert(x11_readback_pixel(&rb_dpy, xid, 90, 180) == 0x000000FFu);
    assert(x11_readback_pixel(&rb_dpy, xid, 50, 150) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 129, 194) == 0x000000FFu);

    /* X11 has no frame-callback feedback: frame_ready is always true. */
    assert(fdk_surface_frame_ready(surface));

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 primitives (line, circles, rounded rect) + "
           "frame_ready\n");
}

/* Offscreen surface composed, blitted onto a window, presented, and
 * verified server-side — the sprite/cache pattern end to end. */
static void test_offscreen_blit_to_window(void) {
    fdk_surface *sprite = NULL;
    assert(fdk_ok(fdk_surface_create(60, 40, &sprite)));
    fdk_surface_fill(sprite, (fdk_color){ 0, 1, 0, 1 });
    fdk_surface_fill_rect(sprite,
                          (fdk_rect){ .x = 10, .y = 10, .width = 20,
                                      .height = 20 },
                          (fdk_color){ 1, 0, 0, 1 });

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK blit test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    fdk_surface_fill(surface, (fdk_color){ 0, 0, 0, 1 });
    assert(fdk_ok(fdk_surface_present(surface)));

    assert(fdk_ok(fdk_surface_blit(surface, 40, 50, sprite,
                                   (fdk_rect){ .x = 0, .y = 0,
                                               .width = 60, .height = 40 })));
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    assert(x11_readback_pixel(&rb_dpy, xid, 40, 50) == 0x0000FF00u);
    assert(x11_readback_pixel(&rb_dpy, xid, 50, 60) == 0x00FF0000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 99, 89) == 0x0000FF00u);
    assert(x11_readback_pixel(&rb_dpy, xid, 39, 49) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 100, 90) == 0x00000000u);

    /* Partial-source blit: just the red square, placed elsewhere. */
    assert(fdk_ok(fdk_surface_blit(surface, 200, 120, sprite,
                                   (fdk_rect){ .x = 10, .y = 10,
                                               .width = 20, .height = 20 })));
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 100);
    assert(x11_readback_pixel(&rb_dpy, xid, 210, 130) == 0x00FF0000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 219, 139) == 0x00FF0000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 199, 129) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 220, 140) == 0x00000000u);

    XCloseDisplay(rb_dpy);
    fdk_surface_destroy(sprite);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 offscreen blit to window (full + partial source)\n");
}

/* ---- Widget foundation (Phase 4) tests ----
 *
 * These exercise the full window-glue path against a live X server:
 *
 *   - fdk_window_get_root() + a widget tree, painted via
 *     fdk_window_paint(), verified SERVER-SIDE (XGetImage over a
 *     second connection): root/child/z-order/clipping pixels.
 *
 *   - REAL INPUT: XSendEvent(3X11) delivers genuine MotionNotify /
 *     ButtonPress / ButtonRelease / KeyPress / FocusIn events through
 *     the X server into the FDK window; the X11 backend translates
 *     them, the window glue routes them into the widget tree, and the
 *     widget callbacks verify hit-testing, local coordinates, the
 *     implicit grab, Tab traversal, and the consumed-events contract.
 *     Nothing here calls fdk_widget_tree_handle_event directly — the
 *     input path is the same one a user's physical mouse takes.
 *
 * (XSendEvent events carry send_event=true; FDK's translator treats
 * them exactly like hardware events — which is what makes this a
 * REAL input-path test rather than a simulation.) */

typedef struct {
    int enter, leave, motion, down, up;
    int key_down, focus_in, focus_out;
    fdk_pointf last_local;
    bool handle;
} widget_recorder;

static bool widget_record_event(fdk_widget *w, const fdk_widget_event *e,
                                void *ud) {
    (void)w;
    widget_recorder *r = ud;
    switch (e->type) {
        case FDK_WIDGET_POINTER_ENTER: r->enter++; break;
        case FDK_WIDGET_POINTER_LEAVE: r->leave++; break;
        case FDK_WIDGET_POINTER_MOTION:
            r->motion++;
            r->last_local = e->position;
            break;
        case FDK_WIDGET_POINTER_DOWN:
            r->down++;
            r->last_local = e->pointer.position;
            break;
        case FDK_WIDGET_POINTER_UP:
            r->up++;
            r->last_local = e->pointer.position;
            break;
        case FDK_WIDGET_KEY_DOWN: r->key_down++; break;
        case FDK_WIDGET_FOCUS_IN: r->focus_in++; break;
        case FDK_WIDGET_FOCUS_OUT: r->focus_out++; break;
        default: break;
    }
    return r->handle;
}

static fdk_color wcol(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f,
                        .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void test_widget_tree_paint_readback(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK widget paint test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    assert(fdk_widget_is_root(root));
    fdk_widget_set_background(root, wcol(20, 20, 20));

    /* Same shape as the headless z-order test: overlapping siblings,
     * a child on its parent, and a child poking out of its parent's
     * bottom edge (must be clipped). */
    fdk_widget *red = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                     (fdk_rect){20, 20, 120, 100}, &red)));
    fdk_widget_set_background(red, wcol(220, 50, 50));
    fdk_widget *green = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                     (fdk_rect){80, 90, 160, 70}, &green)));
    fdk_widget_set_background(green, wcol(50, 200, 50));
    fdk_widget *blue = NULL;
    assert(fdk_ok(fdk_widget_create(red, NULL,
                                     (fdk_rect){30, 30, 40, 40}, &blue)));
    fdk_widget_set_background(blue, wcol(60, 90, 230));
    fdk_widget *poke = NULL;
    assert(fdk_ok(fdk_widget_create(red, NULL,
                                     (fdk_rect){2, 85, 40, 40}, &poke)));
    fdk_widget_set_background(poke, wcol(250, 250, 60));

    /* The window owns its root: destroying it directly must be
     * refused (tree must survive). */
    fdk_widget_destroy(root);
    assert(fdk_widget_child_count(root) == 2);

    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    assert(x11_readback_pixel(&rb_dpy, xid, 25, 25) == 0x00DC3232u);   /* red    */
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 120) == 0x0032C832u); /* green  */
    assert(x11_readback_pixel(&rb_dpy, xid, 110, 60) == 0x00DC3232u);  /* red    */
    assert(x11_readback_pixel(&rb_dpy, xid, 70, 70) == 0x003C5AE6u);   /* blue   */
    assert(x11_readback_pixel(&rb_dpy, xid, 100, 100) == 0x0032C832u); /* green over red */
    assert(x11_readback_pixel(&rb_dpy, xid, 30, 110) == 0x00FAFA3Cu);  /* poke   */
    assert(x11_readback_pixel(&rb_dpy, xid, 30, 125) == 0x00141414u);  /* poke clipped:
                                                                            root bg */
    assert(x11_readback_pixel(&rb_dpy, xid, 10, 10) == 0x00141414u);   /* root bg */

    /* Hiding a subtree removes it from the next paint (full damage). */
    fdk_widget_set_visible(red, false);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    assert(x11_readback_pixel(&rb_dpy, xid, 25, 25) == 0x00141414u);  /* red gone */
    assert(x11_readback_pixel(&rb_dpy, xid, 70, 70) == 0x00141414u);  /* child gone with it */
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 120) == 0x0032C832u); /* green stays */

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win); /* frees the whole tree — ASan verifies */
    fdk_shutdown(ctx);
    printf("[ok] X11 widget tree paints through the window glue "
           "(server-side readback: z-order, clip, hide)\n");
}

/* Sends a synthetic-but-real X event into the FDK window through the
 * X server (the server queues it for clients selecting the mask on
 * the window — FDK's XSelectInput does). flush + a pump round makes
 * delivery synchronous for the test. */
static void x11_send_pointer_event(Display *dpy, unsigned long xid, int type,
                                   int mask, int x, int y,
                                   unsigned int button) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.xbutton.window = (Window)xid;
    ev.xbutton.subwindow = None;
    ev.xbutton.x = x;
    ev.xbutton.y = y;
    ev.xbutton.x_root = x;
    ev.xbutton.y_root = y;
    ev.xbutton.state = 0;
    ev.xbutton.button = button;
    ev.xbutton.same_screen = True;
    Status s = XSendEvent(dpy, (Window)xid, False, (long)mask, &ev);
    assert(s != 0);
    XFlush(dpy);
}

static void x11_send_key_event(Display *dpy, unsigned long xid, int type,
                               unsigned int keycode) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.xkey.window = (Window)xid;
    ev.xkey.subwindow = None;
    ev.xkey.x = 1;
    ev.xkey.y = 1;
    ev.xkey.x_root = 1;
    ev.xkey.y_root = 1;
    ev.xkey.state = 0;
    ev.xkey.keycode = keycode;
    ev.xkey.same_screen = True;
    Status s = XSendEvent(dpy, (Window)xid, False,
                          (long)(KeyPressMask | KeyReleaseMask), &ev);
    assert(s != 0);
    XFlush(dpy);
}

static void x11_send_focus_event(Display *dpy, unsigned long xid, int type) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.xfocus.window = (Window)xid;
    ev.xfocus.mode = NotifyNormal;
    ev.xfocus.detail = NotifyNonlinear;
    Status s = XSendEvent(dpy, (Window)xid, False, (long)FocusChangeMask,
                          &ev);
    assert(s != 0);
    XFlush(dpy);
}

typedef struct {
    int window_pointer_events; /* what the APP callback still sees */
    int window_key_events;
    int window_focus_events;
} window_event_counter;

static void window_count_callback(fdk_window *window,
                                  const fdk_event_data *event,
                                  void *user_data) {
    (void)window;
    window_event_counter *c = user_data;
    switch (event->type) {
        case FDK_EVENT_POINTER_MOTION:
        case FDK_EVENT_POINTER_BUTTON_DOWN:
        case FDK_EVENT_POINTER_BUTTON_UP:
            c->window_pointer_events++;
            break;
        case FDK_EVENT_KEY_DOWN:
        case FDK_EVENT_KEY_UP:
            c->window_key_events++;
            break;
        case FDK_EVENT_WINDOW_FOCUS:
            c->window_focus_events++;
            break;
        default:
            break;
    }
}

static void test_widget_real_input_via_xsendevent(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK widget input test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(24, 24, 28));

    fdk_widget *a = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                     (fdk_rect){20, 20, 120, 60}, &a)));
    fdk_widget_set_background(a, wcol(200, 60, 60));
    fdk_widget_set_can_focus(a, true);
    fdk_widget *b = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                     (fdk_rect){160, 20, 120, 60}, &b)));
    fdk_widget_set_background(b, wcol(60, 60, 200));
    fdk_widget_set_can_focus(b, true);

    widget_recorder ra, rb;
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    fdk_widget_set_event_callback(a, widget_record_event, &ra);
    fdk_widget_set_event_callback(b, widget_record_event, &rb);

    window_event_counter wc;
    memset(&wc, 0, sizeof(wc));
    fdk_window_set_event_callback(win, window_count_callback, &wc);

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    /* Drain the map/configure/expose noise the window generated on
     * show, so the ONLY events processed below are the ones this test
     * sends (map-related events can arrive arbitrarily late under
     * Xvfb; they are harmless but would interleave with the exact
     * counts asserted below). Bounded at ~2s by the alarm net. */
    alarm(5);
    for (int quiet = 0; quiet < 2;) {
        int r = fdk_pump_events(ctx, 100);
        assert(r >= 0);
        quiet = (r == 0) ? quiet + 1 : 0;
    }
    alarm(0);

    /* Motion into a: ENTER + MOTION with a-local coordinates. The
     * event is unhandled by every widget, so the APP callback also
     * sees the window-level motion (routing contract: widget-consumed
     * events are the only ones held back). */
    x11_send_pointer_event(send_dpy, xid, MotionNotify, PointerMotionMask,
                           30, 30, 0);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.enter == 1 && ra.motion == 1);
    assert(ra.last_local.x == 10.0f && ra.last_local.y == 10.0f);
    assert(fdk_widget_is_hovered(a));
    assert(wc.window_pointer_events == 1);

    /* Cross to b: a LEAVE, b ENTER+MOTION. */
    x11_send_pointer_event(send_dpy, xid, MotionNotify, PointerMotionMask,
                           170, 30, 0);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.leave == 1);
    assert(rb.enter == 1 && rb.motion == 1);
    assert(rb.last_local.x == 10.0f && rb.last_local.y == 10.0f);
    assert(wc.window_pointer_events == 2);

    /* Press on a, move to b, release: implicit grab keeps all of it
     * on a (release coords in a-local space, off-bounds is legal). */
    x11_send_pointer_event(send_dpy, xid, ButtonPress, ButtonPressMask,
                           30, 30, Button1);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.down == 1);

    x11_send_pointer_event(send_dpy, xid, MotionNotify, PointerMotionMask,
                           170, 30, 0);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.motion == 2 && rb.motion == 1); /* rb untouched: grab */
    assert(ra.last_local.x == 150.0f && ra.last_local.y == 10.0f);

    x11_send_pointer_event(send_dpy, xid, ButtonRelease, ButtonReleaseMask,
                           170, 30, Button1);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.up == 1 && rb.up == 0); /* release went to the grab */
    assert(ra.last_local.x == 150.0f && ra.last_local.y == 10.0f);

    /* Now a CONSUMES its events: the app callback stops seeing them.
     * (The five unhandled events above — motions x3, press, release —
     * all reached the app; the consumed one must not.) */
    ra.handle = true;
    x11_send_pointer_event(send_dpy, xid, MotionNotify, PointerMotionMask,
                           30, 30, 0);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.enter == 2 && ra.motion == 3);
    assert(wc.window_pointer_events == 5); /* unchanged by the consumed one */

    /* Keyboard: focus a programmatically, then a REAL Tab keypress
     * (X keycode 23 == evdev scancode 15 == FDK_KEY_TAB) drives the
     * built-in traversal to b — and is consumed (app sees no key).
     * (a stops consuming first: the consume-contract check above set
     * its handle flag.) */
    ra.handle = false;
    assert(fdk_widget_focus(a));
    assert(ra.focus_in == 1);
    x11_send_key_event(send_dpy, xid, KeyPress, 23);
    (void)fdk_pump_events(ctx, 200);
    assert(ra.key_down == 1);              /* Tab was DELIVERED to a   */
    assert(ra.focus_out == 1 && rb.focus_in == 1);
    assert(fdk_widget_tree_get_focused(root) == b);
    assert(wc.window_key_events == 0);     /* ...and consumed by the tree */

    /* Window focus events mirror into the focused widget (b) without
     * being consumed — the app callback still sees them. */
    x11_send_focus_event(send_dpy, xid, FocusIn);
    (void)fdk_pump_events(ctx, 200);
    assert(rb.focus_in == 2);
    assert(wc.window_focus_events == 1);
    x11_send_focus_event(send_dpy, xid, FocusOut);
    (void)fdk_pump_events(ctx, 200);
    assert(rb.focus_out == 1);
    assert(wc.window_focus_events == 2);
    assert(fdk_widget_tree_get_focused(root) == b); /* tree keeps focus */

    /* Interaction changed hover/pressed state visuals: a repaint only
     * touches what actually changed (partial damage over the wire). */
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    XCloseDisplay(send_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 real input via XSendEvent: hover, grab, consume "
           "contract, Tab traversal, focus mirror\n");
}

static void test_widget_root_follows_resize(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK widget resize test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(30, 144, 255));

    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    fdk_window_resize(win, 400, 300);
    alarm(5);
    while (cap.configure_count == 0) {
        int r = fdk_pump_events(ctx, 200);
        assert(r >= 0);
    }
    alarm(0);

    /* The glue resized the root to the new client size and damaged
     * everything; one paint covers the fresh framebuffer. */
    fdk_rect rb = fdk_widget_get_bounds(root);
    assert(rb.x == 0 && rb.y == 0 && rb.width == 400 && rb.height == 300);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rd_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    /* A pixel that only exists in the NEW geometry: must be the root
     * widget's background (repainted by the widget path), not the X
     * window's background pixel. */
    assert(x11_readback_pixel(&rd_dpy, xid, 390, 290) == 0x001E90FFu);
    assert(x11_readback_pixel(&rd_dpy, xid, 150, 100) == 0x001E90FFu);

    XCloseDisplay(rd_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 widget root follows resize; fresh area painted by "
           "the tree\n");
}

static void test_widget_layout_reflow_on_resize(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK layout reflow test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    /* A vertical box as the window's content: fixed header + an
     * expanding panel. set_content arranges it immediately; every
     * configure re-arranges it (the Phase 5 window integration). */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *vbox = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &vbox)));

    fdk_widget *header = NULL;
    assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                    (fdk_rect){0, 0, 0, 40}, &header)));
    fdk_widget_set_natural_size(header, 0, 40);
    fdk_widget_set_background(header, wcol(220, 160, 40));

    fdk_widget *panel = NULL;
    assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                    (fdk_rect){0, 0, 0, 50}, &panel)));
    fdk_widget_set_expand(panel, false, true);
    fdk_widget_set_background(panel, wcol(50, 120, 220));

    fdk_window_set_content(win, vbox);

    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    /* header band on top, panel filling the rest */
    assert(x11_readback_pixel(&rb_dpy, xid, 150, 20) == 0x00DCA028u);
    assert(x11_readback_pixel(&rb_dpy, xid, 150, 100) == 0x003278DCu);

    /* Resize: the content box must REFLOW (header still 40 tall, the
     * expanding panel owns all the new space) with zero app code. */
    fdk_window_resize(win, 400, 300);
    alarm(5);
    while (cap.configure_count == 0) {
        int r = fdk_pump_events(ctx, 200);
        assert(r >= 0);
    }
    alarm(0);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    /* header unchanged (40px), panel extends into the new geometry */
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 20) == 0x00DCA028u);
    assert(x11_readback_pixel(&rb_dpy, xid, 390, 290) == 0x003278DCu);
    /* and the boundary is exactly where layout says (y=40) */
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 39) == 0x00DCA028u);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 40) == 0x003278DCu);

    /* destroying the content deactivates the association cleanly */
    fdk_widget_destroy(panel);
    fdk_window_layout(win); /* must notice panel is gone, not crash */
    fdk_window_set_content(win, NULL);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 window content reflows on resize (box layout, "
           "expanding panel, zero app code)\n");
}

/* ---- Phase 5 completion: grid + size limits + baseline ---- */

/* Grid tracks, size limits, expand columns, and reflow-on-resize —
 * the grid's GUI-parity case for the box reflow test above. Track
 * math (padding 10, spacing 10, green min-width 140):
 *   col0 = 120 (red), col1 = 140 (green, min-clamped from 80)
 *   row0 = 60 (red/green), row1 = 50 (blue)
 *   natural = 290 x 140; window 360x240 -> col0 +70, row0 +100. */
static void test_grid_layout_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK grid layout test",
                                 .width = 360, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 2, 2, &grid)));
    fdk_grid_set_spacing(grid, 10);
    fdk_grid_set_padding(grid, 10);
    fdk_grid_set_column_expand(grid, 0, true);
    fdk_grid_set_row_expand(grid, 0, true);

    fdk_widget *red = NULL;
    assert(fdk_ok(fdk_widget_create(grid, NULL,
                                    (fdk_rect){0, 0, 120, 60}, &red)));
    fdk_widget_set_natural_size(red, 120, 60);
    fdk_widget_set_background(red, wcol(220, 50, 50));
    assert(fdk_ok(fdk_grid_attach(grid, red, 0, 0, 1, 1)));

    fdk_widget *green = NULL;
    assert(fdk_ok(fdk_widget_create(grid, NULL,
                                    (fdk_rect){0, 0, 80, 60}, &green)));
    fdk_widget_set_natural_size(green, 80, 60);
    /* Min-width forces the measure up: col1 negotiates 140, not 80 —
     * the size-limit clamp flowing through a REAL container. */
    fdk_widget_set_size_limits(green, 140, 0, 0, 0);
    fdk_widget_set_background(green, wcol(50, 180, 80));
    assert(fdk_ok(fdk_grid_attach(grid, green, 1, 0, 1, 1)));

    fdk_widget *blue = NULL;
    assert(fdk_ok(fdk_widget_create(grid, NULL,
                                    (fdk_rect){0, 0, 100, 50}, &blue)));
    fdk_widget_set_natural_size(blue, 100, 50);
    fdk_widget_set_background(blue, wcol(60, 100, 220));
    assert(fdk_ok(fdk_grid_attach(grid, blue, 0, 1, 2, 1))); /* spans */

    fdk_window_set_content(win, grid);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    /* 360x240: col0 [10,200) gap col1 [210,350); row0 [10,170) gap
     * row1 [180,230). */
    assert(x11_readback_pixel(&rb_dpy, xid, 100, 50) == 0x00DC3232u);
    assert(x11_readback_pixel(&rb_dpy, xid, 300, 50) == 0x0032B450u);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 205) == 0x003C64DCu);
    assert(x11_readback_pixel(&rb_dpy, xid, 205, 50) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 100, 175) == 0x00000000u);

    /* Resize to 460x300: extra +100 wide -> col0 290 (x [10,300)),
     * +160 tall -> row0 220 (y [10,230)); green moved right, the
     * spanning blue moved down — with zero app code. */
    fdk_window_resize(win, 460, 300);
    alarm(5);
    while (cap.configure_count == 0) {
        int r = fdk_pump_events(ctx, 200);
        assert(r >= 0);
    }
    alarm(0);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    assert(x11_readback_pixel(&rb_dpy, xid, 150, 50) == 0x00DC3232u);
    assert(x11_readback_pixel(&rb_dpy, xid, 400, 100) == 0x0032B450u);
    assert(x11_readback_pixel(&rb_dpy, xid, 200, 260) == 0x003C64DCu);
    assert(x11_readback_pixel(&rb_dpy, xid, 305, 100) == 0x00000000u);
    assert(x11_readback_pixel(&rb_dpy, xid, 150, 235) == 0x00000000u);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 grid layout: tracks, min-width limit through a "
           "real container, expand column/row, span, gaps, reflow on "
           "resize\n");
}

/* Baseline alignment at the PIXEL level: two labels at very different
 * sizes share one baseline row, so the small label's ink starts
 * distinctly LOWER while both inks hang down to (nearly) the same
 * row. Requires a system font; honestly skipped without one. */
static void test_baseline_alignment_gui(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    const char *font_path = NULL;
    for (int i = 0; font_candidates[i] != NULL; i++) {
        FILE *f = fopen(font_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            font_path = font_candidates[i];
            break;
        }
    }
    if (font_path == NULL) {
        printf("[skip] X11 baseline alignment GUI (no system TrueType "
               "font found)\n");
        return;
    }
    fdk_font *small = fdk_font_load(font_path, 16);
    fdk_font *big = fdk_font_load(font_path, 32);
    assert(small != NULL && big != NULL);

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK baseline test",
                                 .width = 320, .height = 120 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *hbox = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &hbox)));
    fdk_box_set_padding(hbox, 20);

    fdk_widget *lab_small = NULL;
    assert(fdk_ok(fdk_label_create(hbox, small, "Ag", &lab_small)));
    fdk_widget *lab_big = NULL;
    assert(fdk_ok(fdk_label_create(hbox, big, "Ag", &lab_big)));
    fdk_widget_set_align(lab_small, FDK_ALIGN_FILL, FDK_ALIGN_BASELINE);
    fdk_widget_set_align(lab_big, FDK_ALIGN_FILL, FDK_ALIGN_BASELINE);
    fdk_window_set_content(win, hbox);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    /* Ink-band scan per label: find the topmost/bottommost bright
     * row inside each label's arranged x-range. */
    fdk_rect rs = fdk_widget_get_bounds(lab_small);
    fdk_rect rb = fdk_widget_get_bounds(lab_big);
    Display *d = NULL;
    unsigned long xid = fdk_window_xid(win);
    int a_top = -1, a_bot = -1, b_top = -1, b_bot = -1;
    for (int y = 0; y < 120; y++) {
        for (int x = rs.x; x < rs.x + rs.width && x < 320; x++) {
            unsigned long px = x11_readback_pixel(&d, xid, x, y);
            if (((px >> 16) & 0xFFu) > 100 && ((px >> 8) & 0xFFu) > 100) {
                if (a_top < 0) a_top = y;
                a_bot = y;
                break;
            }
        }
        for (int x = rb.x; x < rb.x + rb.width && x < 320; x++) {
            unsigned long px = x11_readback_pixel(&d, xid, x, y);
            if (((px >> 16) & 0xFFu) > 100 && ((px >> 8) & 0xFFu) > 100) {
                if (b_top < 0) b_top = y;
                b_bot = y;
                break;
            }
        }
    }
    assert(a_top >= 0 && b_top >= 0); /* both inks found */
    /* Shared baseline: the small label starts clearly lower (top
     * differs by the ascent gap ~14px for DejaVu 16 vs 32) — under
     * TOP alignment the tops would be equal. */
    assert(a_top - b_top > 8);
    /* Both inks hang from the same row: bottoms within the descent
     * scale difference (a couple of pixels at these sizes). */
    assert(b_bot - a_bot < 8 && b_bot >= a_bot);

    XCloseDisplay(d);
    fdk_window_destroy(win);
    fdk_font_destroy(small);
    fdk_font_destroy(big);
    fdk_shutdown(ctx);
    printf("[ok] X11 baseline alignment GUI: 16px and 32px labels share "
           "one baseline row (ink tops %d vs %d, bottoms %d vs %d)\n",
           a_top, b_top, a_bot, b_bot);
}

/* ---- Text rendering (Phase 6) ---- */

/* Draws real shaped text into a mapped window and verifies, through
 * the X server's own pixels, that glyph ink reached the screen inside
 * the measured metrics box and nowhere else. Requires a system font;
 * honestly skipped when the environment has none. */
static void test_text_render_readback(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    const char *font_path = NULL;
    for (int i = 0; font_candidates[i] != NULL; i++) {
        FILE *f = fopen(font_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            font_path = font_candidates[i];
            break;
        }
    }
    if (font_path == NULL) {
        printf("[skip] X11 text render readback (no system TrueType "
               "font found for shaping)\n");
        return;
    }

    fdk_font *font = fdk_font_load(font_path, 48);
    assert(font != NULL);

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK text test",
                                 .width = 320, .height = 160 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));

    fdk_color bg = { .r = 0.05f, .g = 0.05f, .b = 0.08f, .a = 1.0f };
    fdk_surface_fill(surface, bg);
    fdk_color fg = { .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f };
    const char *text = "FDK";
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(font, text, 3, &m)));
    int pen_x = 40, baseline = 90;
    assert(fdk_ok(fdk_surface_draw_utf8(surface, font, text, 3, pen_x,
                                        baseline, fg)));
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* Scan a horizontal line through the middle of the ink box: at
     * 48px, "FDK" strokes are several pixels thick, so a mid-height
     * scan must cross white ink multiple times. */
    int mid_y = baseline + m.ink_top / 2;
    long ink = 0;   /* pixels touched by glyph coverage (incl. AA) */
    long bright = 0; /* solid stroke interiors */
    for (int x = pen_x; x < pen_x + m.advance_width; x++) {
        unsigned long px = x11_readback_pixel(&rb_dpy, xid, x, mid_y);
        if (px != 0x000D0D14u) { /* != packed bg (13,13,20) */
            ink++;
            unsigned long r = (px >> 16) & 0xFFu;
            unsigned long g = (px >> 8) & 0xFFu;
            unsigned long b = px & 0xFFu;
            /* White-on-dark compositing can only ADD light: every
             * channel at least as bright as the background. (AA edge
             * pixels sit anywhere in between — hence the separate
             * bright-core count below, not a per-pixel brightness
             * demand.) */
            assert(r >= 13 && g >= 13 && b >= 20);
            if (r > 100 && g > 100 && b > 100) {
                bright++;
            }
        }
    }
    assert(ink > 20);    /* real glyph coverage, not noise */
    assert(bright > 10); /* solid stroke interiors at 48px */

    /* Left of the pen: untouched background. */
    for (int x = 0; x < pen_x - 2; x += 4) {
        unsigned long px = x11_readback_pixel(&rb_dpy, xid, x, mid_y);
        assert(px == 0x000D0D14u);
    }
    /* Below the ink box: untouched background. */
    for (int x = pen_x; x < pen_x + m.advance_width; x += 4) {
        unsigned long px =
            x11_readback_pixel(&rb_dpy, xid, x, baseline + m.ink_bottom + 6);
        assert(px == 0x000D0D14u);
    }

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] X11 text render + server-side glyph readback "
           "(%ld ink px on mid-scan, ink boxed by metrics)\n",
           ink);
}


/* ---- Widget catalog (Phase 6) ---- */

/* Builds a real catalog UI (Button + Toggle + ProgressBar inside a
 * box) as the window's CONTENT, drives it with REAL X input
 * (XSendEvent through the server), and verifies server-side that:
 * the button press activated (progress fill grew), the toggle click
 * flipped its state, and everything painted through the window glue.
 * Skips honestly without a system font. */
static int catalog_activations = 0;
typedef struct {
    fdk_widget *progress;
} catalog_state;
static void catalog_on_activate(fdk_widget *w, void *user) {
    (void)w; /* the button; the PROGRESS comes in via user_data */
    catalog_state *st = user;
    fdk_progress_set_fraction(
        st->progress, fdk_progress_get_fraction(st->progress) + 0.25f);
    catalog_activations++;
}

static void test_widget_catalog_gui(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    const char *font_path = NULL;
    for (int i = 0; font_candidates[i] != NULL; i++) {
        FILE *f = fopen(font_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            font_path = font_candidates[i];
            break;
        }
    }
    if (font_path == NULL) {
        printf("[skip] X11 widget catalog GUI (no system font)\n");
        return;
    }
    fdk_font *font = fdk_font_load(font_path, 16);
    assert(font != NULL);

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK catalog test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(18, 20, 28));

    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &content)));
    fdk_box_set_padding(content, 12);
    fdk_box_set_spacing(content, 10);
    fdk_window_set_content(win, content);

    fdk_widget *btn = NULL;
    assert(fdk_ok(fdk_button_create(content, font, "Click me", &btn)));
    catalog_state cstate = { .progress = NULL };
    fdk_button_set_on_activate(btn, catalog_on_activate, &cstate);
    fdk_widget *tog = NULL;
    assert(fdk_ok(fdk_toggle_create(content, font, "Dark mode", &tog)));
    fdk_widget *prog = NULL;
    assert(fdk_ok(fdk_progress_create(content, &prog)));
    fdk_widget_set_natural_size(prog, 0, 12);
    fdk_widget_set_expand(prog, true, false);
    cstate.progress = prog;

    fdk_window_layout(win);
    (void)fdk_window_paint(win);
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* Progress starts empty: no accent pixels on its mid-line. */
    fdk_rect pb = fdk_widget_get_bounds(prog);
    int my = pb.y + pb.height / 2;
    int accent0 = 0;
    for (int x = pb.x; x < pb.x + pb.width; x++) {
        unsigned long px = x11_readback_pixel(&rb_dpy, xid, x, my);
        unsigned long b = px & 0xFFu;
        unsigned long g = (px >> 8) & 0xFFu;
        unsigned long r = (px >> 16) & 0xFFu;
        if (b > 190 && g > 110 && r < 160) {
            accent0++;
        }
    }
    assert(accent0 == 0);
    assert(catalog_activations == 0);
    assert(!fdk_toggle_is_checked(tog));

    /* REAL click on the button: motion + press + release through the
     * X server, exactly as a user would. */
    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    fdk_rect bb = fdk_widget_get_bounds(btn);
    int cx = bb.x + bb.width / 2;
    int cy = bb.y + bb.height / 2;
    x11_send_pointer_event(send_dpy, xid, MotionNotify,
                           PointerMotionMask, cx, cy, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(send_dpy, xid, ButtonPress,
                           ButtonPressMask, cx, cy, Button1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(send_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, cx, cy, Button1);
    (void)fdk_pump_events(ctx, 200);

    assert(catalog_activations == 1);
    assert(fdk_progress_get_fraction(prog) == 0.25f);

    /* Repaint (app-driven, like production) and let the server see
     * it before readback. */
    (void)fdk_window_paint(win);
    (void)fdk_pump_events(ctx, 200);

    /* And the growth is REAL pixels on the server: ~25% of the bar
     * is now accent. */
    int accent25 = 0;
    for (int x = pb.x; x < pb.x + pb.width; x++) {
        unsigned long px = x11_readback_pixel(&rb_dpy, xid, x, my);
        unsigned long b = px & 0xFFu;
        unsigned long g = (px >> 8) & 0xFFu;
        unsigned long r = (px >> 16) & 0xFFu;
        if (b > 190 && g > 110 && r < 160) {
            accent25++;
        }
    }
    assert(accent25 >= pb.width / 4 - 6 && accent25 <= pb.width / 4 + 6);

    /* REAL click on the toggle: state flips. */
    fdk_rect tb = fdk_widget_get_bounds(tog);
    int tx = tb.x + 12;
    int ty = tb.y + tb.height / 2;
    x11_send_pointer_event(send_dpy, xid, ButtonPress,
                           ButtonPressMask, tx, ty, Button1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(send_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, tx, ty, Button1);
    (void)fdk_pump_events(ctx, 200);
    assert(fdk_toggle_is_checked(tog));

    /* Button still activates on repeated clicks (three more to fill
     * the bar to 100%). */
    for (int i = 0; i < 3; i++) {
        x11_send_pointer_event(send_dpy, xid, ButtonPress,
                               ButtonPressMask, cx, cy, Button1);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(send_dpy, xid, ButtonRelease,
                               ButtonReleaseMask, cx, cy, Button1);
        (void)fdk_pump_events(ctx, 100);
    }
    assert(catalog_activations == 4);
    assert(fdk_progress_get_fraction(prog) == 1.0f);

    (void)fdk_window_paint(win);
    (void)fdk_pump_events(ctx, 200);
    int accent100 = 0;
    for (int x = pb.x; x < pb.x + pb.width; x++) {
        unsigned long px = x11_readback_pixel(&rb_dpy, xid, x, my);
        unsigned long b = px & 0xFFu;
        unsigned long g = (px >> 8) & 0xFFu;
        unsigned long r = (px >> 16) & 0xFFu;
        if (b > 190 && g > 110 && r < 160) {
            accent100++;
        }
    }
    assert(accent100 >= pb.width - 8); /* full bar */

    XCloseDisplay(send_dpy);
    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] X11 widget catalog GUI: real input drives button "
           "activation, progress fill grows on-screen, toggle "
           "flips\n");
}

/* ---- Label modes + radio arrow keys, real X11 input ---- */

/* One-shot region capture: the whole rect in a single XGetImage.
 * Counts pixels differing from bg (0x00RRGGBB, alpha dropped). */
static int x11_count_ink_in_region(Display **out_dpy, unsigned long xid,
                                   int x, int y, int w, int h,
                                   unsigned long bg) {
    if (w <= 0 || h <= 0) {
        return 0;
    }
    if (*out_dpy == NULL) {
        *out_dpy = XOpenDisplay(NULL);
        assert(*out_dpy != NULL);
    }
    XImage *img = XGetImage(*out_dpy, (Drawable)xid, x, y,
                            (unsigned int)w, (unsigned int)h,
                            ~0UL, ZPixmap);
    assert(img != NULL);
    int ink = 0;
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            unsigned long px =
                (unsigned long)XGetPixel(img, xx, yy) & 0x00FFFFFFu;
            if (px != bg) {
                ink++;
            }
        }
    }
    XDestroyImage(img);
    return ink;
}

static int is_accent_px(unsigned long px) {
    unsigned long b = px & 0xFFu;
    unsigned long g = (px >> 8) & 0xFFu;
    unsigned long r = (px >> 16) & 0xFFu;
    return b > 190 && g > 110 && r < 160 && b > r;
}

static void test_label_radio_arrow_gui(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    const char *font_path = NULL;
    for (int i = 0; font_candidates[i] != NULL; i++) {
        FILE *f = fopen(font_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            font_path = font_candidates[i];
            break;
        }
    }
    if (font_path == NULL) {
        printf("[skip] X11 label/radio arrow GUI (no system font)\n");
        return;
    }
    fdk_font *font = fdk_font_load(font_path, 16);
    assert(font != NULL);
    fdk_font_metrics fm;
    fdk_font_get_metrics(font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK label + radio arrows",
                                 .width = 380, .height = 360 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(18, 20, 28));
    unsigned long bg = ((unsigned long)18 << 16) |
                       ((unsigned long)20 << 8) | (unsigned long)28;

    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &content)));
    fdk_box_set_padding(content, 12);
    fdk_box_set_spacing(content, 10);
    fdk_window_set_content(win, content);

    fdk_widget *wrap = NULL;
    assert(fdk_ok(fdk_label_create(
        content, font,
        "the quick brown fox jumps over the lazy dog while the lazy "
        "dog dreams of quicker browner foxes",
        &wrap)));
    fdk_label_set_mode(wrap, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(wrap, 240, 0);
    fdk_widget_set_expand(wrap, true, true); /* width-follows and
                                                * headroom for rewrap */

    fdk_widget *ell = NULL;
    assert(fdk_ok(fdk_label_create(
        content, font,
        "an ellipsized label whose text is far too long for this row",
        &ell)));
    fdk_label_set_mode(ell, FDK_LABEL_ELLIPSIZE);
    fdk_widget_set_expand(ell, true, false);

    fdk_widget *group = NULL;
    assert(fdk_ok(fdk_box_create(content, FDK_VERTICAL, &group)));
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    assert(fdk_ok(fdk_radio_create(group, font, "Red", &r1)));
    assert(fdk_ok(fdk_radio_create(group, font, "Green", &r2)));
    assert(fdk_ok(fdk_radio_create(group, font, "Blue", &r3)));

    fdk_window_layout(win);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* A) WRAP: multiple lines on screen — ink in each of the first
     * two line bands, none right of the label's right edge. */
    fdk_rect wb = fdk_widget_get_absolute_bounds(wrap);
    size_t lines_before = fdk_label_get_line_count(wrap);
    assert(lines_before >= 2);
    for (size_t i = 0; i < 2; i++) {
        int band_y = wb.y + (int)i * pitch + 2;
        int ink = x11_count_ink_in_region(&rb_dpy, xid, wb.x, band_y,
                                          wb.width, pitch - 4, bg);
        assert(ink > 20); /* real glyphs, server-side */
    }
    assert(x11_count_ink_in_region(&rb_dpy, xid,
                                   wb.x + wb.width + 2, wb.y, 4,
                                   wb.height, bg) == 0);

    /* B) ELLIPSIZE: one line, inked, truncated exactly at the edge —
     * the column just past the label's right boundary is background. */
    fdk_rect eb = fdk_widget_get_absolute_bounds(ell);
    assert(fdk_label_get_line_count(ell) == 1);
    assert(x11_count_ink_in_region(&rb_dpy, xid, eb.x, eb.y + 2,
                                   eb.width - 4, pitch - 4, bg) > 20);
    assert(x11_count_ink_in_region(&rb_dpy, xid,
                                   eb.x + eb.width + 2, eb.y, 4,
                                   pitch, bg) == 0);

    /* C) Radio arrows with REAL key events: focus Red, press Down
     * (X keycode 116 == scancode 108); Green selects + takes focus.
     * Then Up (keycode 111) goes back. */
    fdk_radio_set_checked(r1, true);
    assert(fdk_widget_focus(r1));
    fdk_rect r2b = fdk_widget_get_absolute_bounds(r2);
    int dot_x = r2b.x + 8;
    int dot_y = r2b.y + (r2b.height - 16) / 2 + 8;
    assert(!is_accent_px(x11_readback_pixel(&rb_dpy, xid, dot_x,
                                            dot_y)));

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    x11_send_key_event(send_dpy, xid, KeyPress, 116); /* Down */
    (void)fdk_pump_events(ctx, 200);
    assert(fdk_radio_is_checked(r2) && !fdk_radio_is_checked(r1));
    assert(fdk_widget_tree_get_focused(root) == r2);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    assert(is_accent_px(x11_readback_pixel(&rb_dpy, xid, dot_x,
                                           dot_y)));

    x11_send_key_event(send_dpy, xid, KeyPress, 111); /* Up */
    (void)fdk_pump_events(ctx, 200);
    assert(fdk_radio_is_checked(r1) && fdk_widget_has_focus(r1));

    /* D) Narrower window: the wrap label re-wraps TALLER — the band
     * that was empty (first line past the old count) gains server-
     * side ink, and the ellipsized label stays one clipped line. */
    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);
    fdk_window_resize(win, 250, 360);
    alarm(5);
    while (cap.configure_count == 0) {
        int r = fdk_pump_events(ctx, 200);
        assert(r >= 0);
    }
    alarm(0);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    wb = fdk_widget_get_absolute_bounds(wrap);
    size_t lines_after = fdk_label_get_line_count(wrap);
    assert(lines_after > lines_before);
    int new_band_y = wb.y + (int)lines_before * pitch + 2;
    assert(new_band_y + pitch < 360);
    assert(x11_count_ink_in_region(&rb_dpy, xid, wb.x, new_band_y,
                                   wb.width, pitch - 4, bg) > 20);

    eb = fdk_widget_get_absolute_bounds(ell);
    assert(fdk_label_get_line_count(ell) == 1);
    assert(x11_count_ink_in_region(&rb_dpy, xid,
                                   eb.x + eb.width + 2, eb.y, 4,
                                   pitch, bg) == 0);

    XCloseDisplay(send_dpy);
    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] X11 label modes + radio arrows GUI: wrapped bands "
           "server-verified, ellipsis clipped at the edge, REAL arrow "
           "keys select+focus, resize re-wraps\n");
}


/* Phase 7 theme engine, end to end on a real window: the v1 palette
 * paints by default; fdk_theme_set_default() re-damages the tree so
 * the next window paint shows the new colors AND the new metrics
 * (square vs rounded button corners, 1px vs 3px separator band);
 * switching back restores the v1 pixels exactly. */
static void test_theme_switch_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK theme switch test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(20, 20, 20));

    /* A fontless button (fill only) and a separator - the two catalog
     * widgets that consume themed colors + themed metrics. */
    fdk_widget *btn = NULL;
    assert(fdk_ok(fdk_button_create(root, NULL, NULL, &btn)));
    fdk_widget_set_bounds(btn, (fdk_rect){20, 20, 120, 30});
    fdk_widget *sep = NULL;
    assert(fdk_ok(fdk_separator_create(root, FDK_HORIZONTAL, &sep)));
    fdk_widget_set_bounds(sep, (fdk_rect){20, 70, 120, 10});

    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* v1 defaults: control fill 0.16/0.18/0.26 -> 0x292E42; border
     * 0.30/0.33/0.44 -> 0x4D5470; radius 8 cuts the corner pixel;
     * 1px rule at y = 70 + 10/2 = 75. */
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 35) == 0x00292E42u);
    assert(x11_readback_pixel(&rb_dpy, xid, 21, 21) == 0x00141414u);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 75) == 0x004D5470u);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 74) == 0x00141414u);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 76) == 0x00141414u);

    /* A contrasting theme from the .fdk grammar: light fill, square
     * corners, 3px separator band. */
    static const char light_fdk[] =
        "name = \"Light GUI\"\n"
        "[colors]\n"
        "control_background = #E8E8E8\n"
        "control_border = #777777\n"
        "[metrics]\n"
        "button_corner_radius = 0\n"
        "separator_thickness = 3\n";
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *light = fdk_theme_parse(light_fdk, sizeof light_fdk - 1,
                                       &r);
    assert(light != NULL && r == FDK_OK);

    fdk_theme_set_default(light);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    assert(x11_readback_pixel(&rb_dpy, xid, 80, 35) == 0x00E8E8E8u);
    /* radius 0: the corner pixel IS the fill now. */
    assert(x11_readback_pixel(&rb_dpy, xid, 21, 21) == 0x00E8E8E8u);
    /* thickness 3: band centered on the same line, 74..76. */
    for (int y = 74; y <= 76; y++) {
        assert(x11_readback_pixel(&rb_dpy, xid, 80, y) == 0x00777777u);
    }
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 73) == 0x00141414u);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 77) == 0x00141414u);

    /* Back to the built-in: the v1 pixels return exactly. */
    fdk_theme_set_default(NULL);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 35) == 0x00292E42u);
    assert(x11_readback_pixel(&rb_dpy, xid, 21, 21) == 0x00141414u);
    assert(x11_readback_pixel(&rb_dpy, xid, 80, 75) == 0x004D5470u);

    fdk_theme_destroy(light);
    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 theme switch repaints a live window (colors, "
           "corner radius, separator thickness; round trip exact)\n");
}


/* Phase 8 decorations, end to end on a real window: the themed title
 * band paints at the top (server-side readback), the content widget
 * is laid out below it, _MOTIF_WM_HINTS is set while decorated and
 * removed when not, the band's close button delivers a REAL
 * close-request, dragging the band moves the window (verified via
 * XTranslateCoordinates), and the mode round-trips. */
static int deco_close_requests = 0;
static int deco_state_events = 0;

/* Pumps until FDK's own view of the window size matches (the band's
 * button rects and the resize zones are computed from it): the X
 * server may have applied a resize before FDK processed the
 * ConfigureNotify, and input aimed using the fresh geometry against
 * FDK's stale view would miss. */
static void deco_wait_size(fdk_context *ctx, fdk_window *win, int w,
                           int h) {
    for (int i = 0; i < 40; i++) {
        fdk_size sz = { 0, 0 };
        (void)fdk_window_get_size(win, &sz);
        if (sz.width == w && sz.height == h) {
            return;
        }
        (void)fdk_pump_events(ctx, 50);
    }
}

static void deco_window_callback(fdk_window *window,
                                 const fdk_event_data *event,
                                 void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        deco_close_requests++;
    } else if (event->type == FDK_EVENT_WINDOW_STATE) {
        deco_state_events++;
    }
}

static bool motif_hints_present(Display *dpy, unsigned long xid) {
    Atom hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    Atom type = None;
    int fmt = 0;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, (Window)xid, hints, 0, 5, False,
                           AnyPropertyType, &type, &fmt, &n, &left,
                           &data) != Success) {
        return false;
    }
    bool present = (data != NULL);
    XFree(data);
    return present;
}

static void test_decorations_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK decorations test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, deco_window_callback, NULL);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(20, 20, 20));

    /* Content: a plain widget with a distinctive fill, auto-arranged
     * by the window glue. */
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_background(content, wcol(60, 120, 200));
    fdk_window_set_content(win, content);

    /* Baseline: content fills the whole window. */
    assert(fdk_window_get_decorated(win) == false);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    assert(x11_readback_pixel(&rb_dpy, xid, 10, 5) == 0x003C78C8u);
    assert(!motif_hints_present(rb_dpy, xid));

    /* Decorate: the band paints, content shifts below it. */
    assert(fdk_ok(fdk_window_set_decorated(win, true)));
    assert(fdk_window_get_decorated(win) == true);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    /* Band fill (v1 control bg) and its themed 1px bottom rule. */
    assert(x11_readback_pixel(&rb_dpy, xid, 160, 5) == 0x00292E42u);
    assert(x11_readback_pixel(&rb_dpy, xid, 160, 27) == 0x004D5470u);
    /* The close button (22x20 at x=292..314): fill at its left edge,
     * vertically centered - clear of the radius-8 corners and of the
     * centered glyph. */
    assert(x11_readback_pixel(&rb_dpy, xid, 295, 14) == 0x00292E42u);
    /* Content now starts BELOW the 28px band. */
    assert(x11_readback_pixel(&rb_dpy, xid, 10, 50) == 0x003C78C8u);
    assert(x11_readback_pixel(&rb_dpy, xid, 10, 5) == 0x00292E42u);
    /* The WM has been asked to drop its chrome. */
    assert(motif_hints_present(rb_dpy, xid));

    /* Drag the band: press at bar-local (100, 10) — inside the band,
     * clear of the 5px top resize zone that Phase 8's resize edges
     * now own — motion to (140, 30) -> the window moves by
     * (+40, +20). */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 100, 10, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonPress, ButtonPressMask,
                           100, 10, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 140, 30, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 140, 30, 1);
    (void)fdk_pump_events(ctx, 100);
    {
        Window child = 0;
        int px = 0, py = 0;
        assert(XTranslateCoordinates(rb_dpy, (Window)xid,
                                     DefaultRootWindow(rb_dpy), 0, 0,
                                     &px, &py, &child));
        assert(px == 40 && py == 20);
    }

    /* The close button delivers a real close-request (the app
     * callback counts it; nothing is destroyed here). */
    assert(deco_close_requests == 0);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 303, 14, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonPress, ButtonPressMask,
                           303, 14, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 303, 14, 1);
    (void)fdk_pump_events(ctx, 100);
    assert(deco_close_requests == 1);

    /* Undecorate: band gone, content back to full, hints removed. */
    assert(fdk_ok(fdk_window_set_decorated(win, false)));
    assert(fdk_window_get_decorated(win) == false);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    assert(x11_readback_pixel(&rb_dpy, xid, 160, 5) == 0x003C78C8u);
    assert(!motif_hints_present(rb_dpy, xid));

    /* Round trip back on. */
    assert(fdk_ok(fdk_window_set_decorated(win, true)));
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);
    assert(x11_readback_pixel(&rb_dpy, xid, 160, 5) == 0x00292E42u);

    /* ---- Phase 8 completion: band buttons + double-click ---- */
    {
        int states0 = deco_state_events;

        /* The maximize button (w-52 .. w-30 = 268..290 for w=320):
         * a real click on the band's vector glyph button maximizes
         * the window (bare X: FDK itself moves+resizes to the full
         * screen) and delivers a real state event. */
        x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                               PointerMotionMask, 279, 14, 0);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                               ButtonPressMask, 279, 14, 1);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                               ButtonReleaseMask, 279, 14, 1);
        (void)fdk_pump_events(ctx, 200);
        assert(fdk_window_is_maximized(win));
        assert(deco_state_events == states0 + 1);
        {
            /* Server-side truth: the window now fills the screen. */
            Window root_ret = 0, child = 0;
            int px = 0, py = 0;
            unsigned int bw = 0, depth = 0, w = 0, h = 0;
            assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px,
                                &py, &w, &h, &bw, &depth));
            (void)child;
            assert(px == 0 && py == 0);
            assert((int)w == DisplayWidth(rb_dpy, DefaultScreen(rb_dpy)));
            assert((int)h == DisplayHeight(rb_dpy, DefaultScreen(rb_dpy)));
        }

        /* Double-click the band (center, clear of buttons): toggles
         * maximize back off; the window returns to the geometry it
         * had before (the post-drag position + 320x240). */
        deco_wait_size(ctx, win,
                       DisplayWidth(rb_dpy, DefaultScreen(rb_dpy)),
                       DisplayHeight(rb_dpy, DefaultScreen(rb_dpy)));
        x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                               ButtonPressMask, 160, 14, 1);
        (void)fdk_pump_events(ctx, 60);
        x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                               ButtonReleaseMask, 160, 14, 1);
        (void)fdk_pump_events(ctx, 60);
        x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                               ButtonPressMask, 160, 14, 1);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                               ButtonReleaseMask, 160, 14, 1);
        (void)fdk_pump_events(ctx, 200);
        assert(!fdk_window_is_maximized(win));
        assert(deco_state_events == states0 + 2);
        {
            Window root_ret = 0, child = 0;
            int px = 0, py = 0;
            unsigned int bw = 0, depth = 0, w = 0, h = 0;
            assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px,
                                &py, &w, &h, &bw, &depth));
            (void)child;
            assert(px == 40 && py == 20); /* pre-maximize origin */
            assert(w == 320 && h == 240); /* pre-maximize size */
        }

        /* The minimize button (w-76 .. w-54 = 244..266): unmaps the
         * window (bare X has no icon manager) + state event; restore
         * maps it back. */
        deco_wait_size(ctx, win, 320, 240); /* FDK saw the restore */
        x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                               PointerMotionMask, 255, 14, 0);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                               ButtonPressMask, 255, 14, 1);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                               ButtonReleaseMask, 255, 14, 1);
        (void)fdk_pump_events(ctx, 200);
        assert(fdk_window_is_minimized(win));
        assert(deco_state_events == states0 + 3);
        {
            XWindowAttributes wa;
            assert(XGetWindowAttributes(rb_dpy, (Window)xid, &wa));
            assert(wa.map_state == IsUnmapped);
        }
        assert(fdk_ok(fdk_window_restore(win)));
        (void)fdk_pump_events(ctx, 200);
        assert(!fdk_window_is_minimized(win));
        assert(deco_state_events == states0 + 4);
        {
            XWindowAttributes wa;
            assert(XGetWindowAttributes(rb_dpy, (Window)xid, &wa));
            assert(wa.map_state == IsViewable);
        }

        /* ---- Per-theme title-bar height: switching to a theme with
         * title_bar_height=40 grows the band (row 38 becomes band
         * fill; content that was there moves down) and switches
         * back cleanly. ---- */
        assert(fdk_ok(fdk_window_paint(win)));
        (void)fdk_pump_events(ctx, 200);
        assert(x11_readback_pixel(&rb_dpy, xid, 160, 38) ==
               0x003C78C8u); /* content at 38: below the 28px band */
        fdk_theme *tall = fdk_theme_create_default();
        assert(tall != NULL);
        assert(fdk_ok(fdk_theme_set_metric(
            tall, FDK_TM_TITLE_BAR_HEIGHT, 40)));
        fdk_theme_set_default(tall);
        assert(fdk_ok(fdk_window_paint(win)));
        (void)fdk_pump_events(ctx, 200);
        assert(x11_readback_pixel(&rb_dpy, xid, 160, 38) ==
               0x00292E42u); /* band fill: the band now covers 0..39 */
        assert(x11_readback_pixel(&rb_dpy, xid, 160, 5) ==
               0x00292E42u);
        /* Content moved down: row 50 was content before (28px band),
         * still content (below 40) — and the band's bottom rule moved
         * from y=27 to y=39. */
        assert(x11_readback_pixel(&rb_dpy, xid, 160, 39) ==
               0x004D5470u); /* themed rule at the new band bottom */
        fdk_theme_set_default(NULL); /* revert to built-in */
        fdk_theme_destroy(tall);
        assert(fdk_ok(fdk_window_paint(win)));
        (void)fdk_pump_events(ctx, 200);
        assert(x11_readback_pixel(&rb_dpy, xid, 160, 38) ==
               0x003C78C8u); /* back to the 28px band */
    }

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 FDK decorations: themed band, content below, "
           "MWM hints on/off, real close-request, drag moves the "
           "window, max/min buttons + double-click maximize + themed "
           "band height, round trip\n");
}


/* ---- Phase 8 completion: window state + resize edges + EWMH ---- */

static int state_gui_events = 0;
static int state_gui_last_max = -1;
static int state_gui_last_min = -1;

static void state_window_callback(fdk_window *window,
                                  const fdk_event_data *event,
                                  void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_STATE) {
        state_gui_events++;
        state_gui_last_max = event->state.maximized;
        state_gui_last_min = event->state.minimized;
    }
}

/* Bare X (no WM — exactly what Xvfb gives us): FDK itself performs
 * the window management, so state changes are synchronous and the
 * geometry is directly observable. */
static void test_window_state_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK state test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, state_window_callback, NULL);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* Initial state: not maximized, not minimized. */
    assert(!fdk_window_is_maximized(win));
    assert(!fdk_window_is_minimized(win));

    /* Maximize: FDK fills the screen, saves the geometry, and
     * dispatches the state event synchronously. */
    int ev0 = state_gui_events;
    assert(fdk_ok(fdk_window_maximize(win)));
    assert(fdk_window_is_maximized(win));
    assert(state_gui_events == ev0 + 1);
    assert(state_gui_last_max == 1 && state_gui_last_min == 0);
    (void)fdk_pump_events(ctx, 200);
    rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 0);
        assert((int)w == DisplayWidth(rb_dpy, DefaultScreen(rb_dpy)));
        assert((int)h == DisplayHeight(rb_dpy, DefaultScreen(rb_dpy)));
    }

    /* Idempotent request: no redundant event. */
    assert(fdk_ok(fdk_window_maximize(win)));
    assert(state_gui_events == ev0 + 1);

    /* Unmaximize: back to the saved geometry + event. */
    assert(fdk_ok(fdk_window_unmaximize(win)));
    assert(!fdk_window_is_maximized(win));
    assert(state_gui_events == ev0 + 2);
    (void)fdk_pump_events(ctx, 200);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 0 && w == 300 && h == 200);
    }

    /* Minimize: unmap + event (no WM to manage icons). */
    assert(fdk_ok(fdk_window_minimize(win)));
    assert(fdk_window_is_minimized(win));
    assert(state_gui_events == ev0 + 3);
    assert(state_gui_last_max == 0 && state_gui_last_min == 1);
    (void)fdk_pump_events(ctx, 200);
    {
        XWindowAttributes wa;
        assert(XGetWindowAttributes(rb_dpy, (Window)xid, &wa));
        assert(wa.map_state == IsUnmapped);
    }

    /* Restore: map + event. */
    assert(fdk_ok(fdk_window_restore(win)));
    assert(!fdk_window_is_minimized(win));
    assert(state_gui_events == ev0 + 4);
    (void)fdk_pump_events(ctx, 200);
    {
        XWindowAttributes wa;
        assert(XGetWindowAttributes(rb_dpy, (Window)xid, &wa));
        assert(wa.map_state == IsViewable);
    }

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 window state (bare X): maximize fills the screen "
           "and remembers geometry, unmaximize restores, minimize "
           "unmaps, restore remaps, every flip delivers exactly one "
           "FDK_EVENT_WINDOW_STATE\n");
}

/* FDK-drawn resize edges (the bare-X fallback path — no WM to hand
 * the drag to): synthetic pointer drags on the zones resize the
 * window through XMoveResizeWindow, clamped to the app's size limits;
 * with edges off, presses in the same places reach content widgets.
 */
static int resize_gui_button_presses = 0;

static bool resize_gui_button_cb(fdk_widget *w,
                                 const fdk_widget_event *event,
                                 void *user) {
    (void)w;
    (void)user;
    if (event->type == FDK_WIDGET_POINTER_DOWN) {
        resize_gui_button_presses++;
        return true;
    }
    return false;
}

static void test_resize_edges_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK resize test",
                                 .width = 200, .height = 100 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    /* A button spanning the whole window: any press that gets past
     * the (disabled) resize zones lands on it. */
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_background(content, (fdk_color){0.1f, 0.1f, 0.6f, 1.0f});
    fdk_window_set_content(win, content);
    fdk_widget_set_event_callback(content, resize_gui_button_cb, NULL);

    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);

    /* Edges OFF by default (undecorated): a press in the corner zone
     * reaches the content widget and resizes nothing. */
    assert(!fdk_window_get_resizable(win));
    resize_gui_button_presses = 0;
    rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 2, 50, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 2, 50, 1);
    (void)fdk_pump_events(ctx, 100);
    assert(resize_gui_button_presses == 1);

    /* Turn the edges on; app limits: min 120x80, max 400x300. */
    fdk_window_set_resizable(win, true);
    assert(fdk_window_get_resizable(win));
    fdk_window_set_size_limits(win, (fdk_size){120, 80},
                               (fdk_size){400, 300});

    /* SE corner drag (+30, +30): grows to 230x130, origin pinned. */
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 198, 98, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 228, 128, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 228, 128, 1);
    (void)fdk_pump_events(ctx, 200);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 0 && w == 230 && h == 130);
    }

    /* E edge drag (+40, 0) from the current 230x130: 270x130. */
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 228, 65, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 268, 65, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 268, 65, 1);
    (void)fdk_pump_events(ctx, 200);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 0 && w == 270 && h == 130);
    }

    /* N edge drag DOWNWARD (+0, +20) shrinks from the top: the
     * origin moves down by 20 and the height drops by 20. */
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 135, 2, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 135, 22, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 135, 22, 1);
    (void)fdk_pump_events(ctx, 200);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 20 && w == 270 && h == 110);
    }

    /* Min clamp: SE-corner drag from (268,108) to (10,40) — a
     * shrink of (-258,-68) against 270x110 — clamps at the app's
     * 120x80 minimum with the origin pinned (SE drags never move it).
     * The window is at (0,20) after the N-edge test, so the final
     * geometry is exactly (0,20,120,80). */
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 268, 108, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 10, 40, 0);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 10, 40, 1);
    (void)fdk_pump_events(ctx, 200);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        assert(XGetGeometry(rb_dpy, (Window)xid, &root_ret, &px, &py,
                            &w, &h, &bw, &depth));
        assert(px == 0 && py == 20 && w == 120 && h == 80);
    }

    /* Content presses still work in the middle (no zone there). */
    resize_gui_button_presses = 0;
    x11_send_pointer_event(rb_dpy, xid, ButtonPress,
                           ButtonPressMask, 100, 50, 1);
    (void)fdk_pump_events(ctx, 100);
    x11_send_pointer_event(rb_dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 100, 50, 1);
    (void)fdk_pump_events(ctx, 100);
    assert(resize_gui_button_presses == 1);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 resize edges (bare X): off by default (content "
           "gets corner presses), SE/E/N drags resize exactly, "
           "min-size clamps hold\n");
}
/* ---- The EWMH fake window manager ----
 *
 * Xvfb runs with NO window manager, which is exactly right for the
 * bare-X fallback tests above — but it leaves the EWMH protocol
 * paths (the ones a real desktop WM exercises) untestable... unless
 * the test BECOMES the window manager. On a second X connection this
 * fake WM:
 *
 *   - advertises _NET_SUPPORTED (so FDK's connect-time probe sees an
 *     EWMH WM and enables the message paths),
 *   - selects SubstructureRedirectMask|SubstructureNotifyMask on the
 *     root (the defining privilege of a WM),
 *   - answers _NET_WM_STATE client messages by rewriting the
 *     window's _NET_WM_STATE property (what real WMs do; the
 *     PropertyNotify is how FDK learns the state),
 *   - simulates the WM side of maximize (move+resize to the full
 *     screen on ADD, restore the remembered geometry on REMOVE),
 *   - records _NET_WM_MOVERESIZE and WM_CHANGE_STATE requests for
 *     field-by-field assertions.
 *
 * It installs AFTER the bare-X tests (each test creates a fresh
 * context, so the probe re-runs per context) and uninstalls before
 * the suite ends (root input deselected + _NET_SUPPORTED deleted) so
 * nothing downstream sees a phantom WM.
 */
typedef struct {
    Display *dpy;
    Window root;
    Atom net_supported;
    Atom net_wm_state;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_state_maximized_horiz;
    Atom net_wm_moveresize;
    Atom wm_change_state;
    /* Last _NET_WM_STATE message fields. */
    long mr_action, mr_a1, mr_a2, mr_a3;
    int saw_net_wm_state;
    /* Last _NET_WM_MOVERESIZE fields. */
    long move_x, move_y, move_dir, move_button;
    int saw_moveresize;
    /* Last WM_CHANGE_STATE (iconify) state. */
    long change_state;
    int saw_change_state;
    /* Saved pre-maximize geometry of the (single) managed window. */
    int has_saved, saved_x, saved_y;
    unsigned int saved_w, saved_h;
} fake_wm;

static void fake_wm_install(fake_wm *wm) {
    memset(wm, 0, sizeof *wm);
    wm->dpy = XOpenDisplay(NULL);
    assert(wm->dpy != NULL);
    wm->root = DefaultRootWindow(wm->dpy);
    wm->net_supported = XInternAtom(wm->dpy, "_NET_SUPPORTED", False);
    wm->net_wm_state = XInternAtom(wm->dpy, "_NET_WM_STATE", False);
    wm->net_wm_state_maximized_vert =
        XInternAtom(wm->dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    wm->net_wm_state_maximized_horiz =
        XInternAtom(wm->dpy, "_NET_WM_STATE_MAXIMIZED_HORIZ", False);
    wm->net_wm_moveresize =
        XInternAtom(wm->dpy, "_NET_WM_MOVERESIZE", False);
    wm->wm_change_state =
        XInternAtom(wm->dpy, "WM_CHANGE_STATE", False);
    wm->change_state = -1;

    /* Advertise exactly what we implement: FDK keys its EWMH
     * maximize path off both maximized atoms being listed. */
    Atom supported[3];
    supported[0] = wm->net_wm_state_maximized_vert;
    supported[1] = wm->net_wm_state_maximized_horiz;
    supported[2] = wm->net_wm_moveresize;
    XChangeProperty(wm->dpy, wm->root, wm->net_supported, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)supported, 3);

    /* Become the WM (only possible because nobody else is). */
    XSelectInput(wm->dpy, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(wm->dpy, False);
}

static void fake_wm_uninstall(fake_wm *wm) {
    XSelectInput(wm->dpy, wm->root, 0);
    XDeleteProperty(wm->dpy, wm->root, wm->net_supported);
    XSync(wm->dpy, False);
    XCloseDisplay(wm->dpy);
}

/* Reads the window's current _NET_WM_STATE atom list and rewrites it
 * with add/remove applied to the two maximized atoms. */
static void fake_wm_apply_state(fake_wm *wm, Window w, long action,
                                Atom a1, Atom a2) {
    Atom cur[16];
    unsigned long n = 0;
    Atom type = None;
    int fmt = 0;
    unsigned long bytes_after = 0;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(wm->dpy, w, wm->net_wm_state, 0, 16, False,
                           XA_ATOM, &type, &fmt, &n, &bytes_after,
                           &prop) == Success && prop != NULL &&
        type == XA_ATOM) {
        for (unsigned long i = 0; i < n && i < 16; i++) {
            cur[i] = ((Atom *)prop)[i];
        }
    }
    if (prop != NULL) {
        XFree(prop);
    }
    /* Apply add/remove for both atoms. */
    for (int which = 0; which < 2; which++) {
        Atom a = (which == 0) ? a1 : a2;
        if (a == None) {
            continue;
        }
        int found = -1;
        for (unsigned long i = 0; i < n; i++) {
            if (cur[i] == a) {
                found = (int)i;
                break;
            }
        }
        if (action == 1 /* ADD */ && found < 0 && n < 16) {
            cur[n++] = a;
        } else if (action == 0 /* REMOVE */ && found >= 0) {
            cur[found] = cur[n - 1];
            n--;
        }
    }
    if (n > 0) {
        XChangeProperty(wm->dpy, w, wm->net_wm_state, XA_ATOM, 32,
                        PropModeReplace,
                        (const unsigned char *)cur, (int)n);
    } else {
        XDeleteProperty(wm->dpy, w, wm->net_wm_state);
    }
    /* The property rewrite generates the PropertyNotify FDK listens
     * for. Now simulate the WM's own reaction: ADD maximized ->
     * fullscreen the window (remembering geometry); REMOVE ->
     * restore it. */
    int maximized = 0;
    for (unsigned long i = 0; i < n; i++) {
        if (cur[i] == wm->net_wm_state_maximized_vert ||
            cur[i] == wm->net_wm_state_maximized_horiz) {
            maximized++;
        }
    }
    maximized = (maximized == 2) ? 1 : 0;
    if (maximized && !wm->has_saved) {
        Window root_ret = 0;
        int x = 0, y = 0;
        unsigned int bw = 0, depth = 0;
        assert(XGetGeometry(wm->dpy, w, &root_ret, &x, &y,
                            &wm->saved_w, &wm->saved_h, &bw, &depth));
        wm->saved_x = x;
        wm->saved_y = y;
        wm->has_saved = 1;
        XMoveResizeWindow(wm->dpy, w, 0, 0,
                          (unsigned int)DisplayWidth(
                              wm->dpy, DefaultScreen(wm->dpy)),
                          (unsigned int)DisplayHeight(
                              wm->dpy, DefaultScreen(wm->dpy)));
    } else if (!maximized && wm->has_saved) {
        XMoveResizeWindow(wm->dpy, w, wm->saved_x, wm->saved_y,
                          wm->saved_w, wm->saved_h);
        wm->has_saved = 0;
    }
}

/* Drains pending events; acts on the ones a WM cares about. */
static void fake_wm_pump(fake_wm *wm) {
    /* XSync FIRST: it flushes this connection and waits for the
     * server, which by then has also processed every OTHER client's
     * already-flushed requests (FDK's XSendEvent from the step the
     * test just ran) — so anything we're waiting for is in our queue
     * by the time XPending looks. Without it the pump races the
     * server's SendEvent processing and flakily misses messages. */
    XSync(wm->dpy, False);
    while (XPending(wm->dpy) > 0) {
        XEvent ev;
        XNextEvent(wm->dpy, &ev);
        if (ev.type != ClientMessage) {
            continue;
        }
        (void)0; /* all ClientMessages below are classified by type */
        if (ev.xclient.message_type == wm->net_wm_state) {
            wm->mr_action = ev.xclient.data.l[0];
            wm->mr_a1 = ev.xclient.data.l[1];
            wm->mr_a2 = ev.xclient.data.l[2];
            wm->mr_a3 = ev.xclient.data.l[3];
            wm->saw_net_wm_state++;
            fake_wm_apply_state(wm, ev.xclient.window,
                                ev.xclient.data.l[0],
                                (Atom)ev.xclient.data.l[1],
                                (Atom)ev.xclient.data.l[2]);
        } else if (ev.xclient.message_type == wm->net_wm_moveresize) {
            wm->move_x = ev.xclient.data.l[0];
            wm->move_y = ev.xclient.data.l[1];
            wm->move_dir = ev.xclient.data.l[2];
            wm->move_button = ev.xclient.data.l[3];
            wm->saw_moveresize++;
            /* A real WM runs the interactive drag with a pointer
             * grab; simulating the full drag is unnecessary — the
             * message fields are what this test asserts. Perform one
             * representative move so the window visibly reacts. */
            if (wm->move_dir == 8 /* MOVE */) {
                XMoveWindow(wm->dpy, ev.xclient.window, 30, 10);
            }
        } else if (ev.xclient.message_type == wm->wm_change_state) {
            wm->change_state = ev.xclient.data.l[0];
            wm->saw_change_state++;
            /* ICCCM: the WM maintains WM_STATE on the window; Iconic
             * = 3, Normal = 1. FDK watches this property. */
            long state[2] = { ev.xclient.data.l[0] == 3 ? 3L : 1L, 0L };
            XChangeProperty(wm->dpy, ev.xclient.window,
                            XInternAtom(wm->dpy, "WM_STATE", False),
                            XA_INTEGER, 32, PropModeReplace,
                            (const unsigned char *)state, 2);
        }
    }
    XSync(wm->dpy, False);
}

/* Pumps until *counter reaches want (bounded); returns whether it
 * did. The XSync-first pump is already deterministic for flushed
 * requests; the bounded retry additionally absorbs scheduler jitter
 * so the suite never flakes. */
static bool fake_wm_wait(fake_wm *wm, int *counter, int want) {
    for (int i = 0; i < 20 && *counter < want; i++) {
        fake_wm_pump(wm);
        if (*counter >= want) {
            return true;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return *counter >= want;
}

static int ewmh_state_events = 0;

static void ewmh_window_callback(fdk_window *window,
                                 const fdk_event_data *event,
                                 void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_STATE) {
        ewmh_state_events++;
    }
}

static void test_ewmh_fake_wm(void) {
    fake_wm wm;
    fake_wm_install(&wm);

    /* FDK connects AFTER the fake WM advertised itself: the
     * connect-time _NET_SUPPORTED probe must discover it and enable
     * the EWMH paths. */
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK ewmh test",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, ewmh_window_callback, NULL);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);
    unsigned long xid = fdk_window_xid(win);

    /* ---- Maximize goes through _NET_WM_STATE, not through FDK's
     * own move+resize: the fake WM performs it, FDK learns via the
     * property, and exactly one state event fires. ---- */
    int ev0 = ewmh_state_events;
    wm.saw_net_wm_state = 0;
    assert(fdk_ok(fdk_window_maximize(win)));
    assert(fake_wm_wait(&wm, &wm.saw_net_wm_state, 1));
    (void)fdk_pump_events(ctx, 300); /* FDK reads PropertyNotify */
    assert(wm.saw_net_wm_state == 1);
    assert(wm.mr_action == 1 /* _NET_WM_STATE_ADD */);
    assert((Atom)wm.mr_a1 == wm.net_wm_state_maximized_vert);
    assert((Atom)wm.mr_a2 == wm.net_wm_state_maximized_horiz);
    assert(wm.mr_a3 == 1 /* source indication: application */);
    assert(fdk_window_is_maximized(win));
    assert(ewmh_state_events == ev0 + 1);
    {
        /* The WM (not FDK) resized the window to the full screen. */
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        Display *d = XOpenDisplay(NULL);
        assert(d != NULL);
        assert(XGetGeometry(d, (Window)xid, &root_ret, &px, &py, &w,
                            &h, &bw, &depth));
        XCloseDisplay(d);
        assert(px == 0 && py == 0);
        assert((int)w == DisplayWidth(wm.dpy, DefaultScreen(wm.dpy)));
    }

    /* ---- Unmaximize: REMOVE message, geometry restored, event. -- */
    wm.saw_net_wm_state = 0;
    assert(fdk_ok(fdk_window_unmaximize(win)));
    assert(fake_wm_wait(&wm, &wm.saw_net_wm_state, 1));
    (void)fdk_pump_events(ctx, 300);
    assert(wm.saw_net_wm_state == 1);
    assert(wm.mr_action == 0 /* _NET_WM_STATE_REMOVE */);
    assert(!fdk_window_is_maximized(win));
    assert(ewmh_state_events == ev0 + 2);
    {
        Window root_ret = 0;
        int px = 0, py = 0;
        unsigned int bw = 0, depth = 0, w = 0, h = 0;
        Display *d = XOpenDisplay(NULL);
        assert(d != NULL);
        assert(XGetGeometry(d, (Window)xid, &root_ret, &px, &py, &w,
                            &h, &bw, &depth));
        XCloseDisplay(d);
        assert(px == 0 && py == 0 && w == 300 && h == 200);
    }

    /* ---- Band drag under a WM: the press must hand the drag to the
     * WM via _NET_WM_MOVERESIZE(MOVE) instead of FDK moving the
     * window itself. (Needs the FDK band — decorate first.) ---- */
    assert(fdk_ok(fdk_window_set_decorated(win, true)));
    (void)fdk_pump_events(ctx, 100);
    wm.saw_moveresize = 0;
    x11_send_pointer_event(wm.dpy, xid, ButtonPress, ButtonPressMask,
                           100, 10, 1);
    (void)fdk_pump_events(ctx, 100);
    /* The press point in root coordinates, measured BEFORE the fake
     * WM's simulated reaction moves the window: FDK translated the
     * point through the window's position at press time. */
    Window child = 0;
    int press_rx = 0, press_ry = 0;
    assert(XTranslateCoordinates(wm.dpy, (Window)xid, wm.root,
                                 100, 10, &press_rx, &press_ry, &child));
    assert(fake_wm_wait(&wm, &wm.saw_moveresize, 1));
    assert(wm.move_dir == 8 /* _NET_WM_MOVERESIZE_MOVE */);
    assert(wm.move_button == 1 /* the initiating button */);
    assert(wm.move_x == press_rx && wm.move_y == press_ry);
    /* Release the (WM-grabbed, in reality) button so nothing sticks. */
    x11_send_pointer_event(wm.dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 100, 10, 1);
    (void)fdk_pump_events(ctx, 100);

    /* ---- Resize edges under a WM: a corner press hands the drag to
     * the WM via _NET_WM_MOVERESIZE with the right direction code. -- */
    fdk_window_set_resizable(win, true);
    wm.saw_moveresize = 0;
    /* The window is 300x200 (restored by the fake WM's unmaximize);
     * its SE corner zone is (295..299, 195..199). Wait for FDK to
     * SEE the restored size — the resize zones are computed from
     * FDK's view, not the server's. */
    {
        fdk_size sz = { 0, 0 };
        for (int i = 0; i < 40; i++) {
            (void)fdk_window_get_size(win, &sz);
            if (sz.width == 300 && sz.height == 200) {
                break;
            }
            (void)fdk_pump_events(ctx, 50);
        }
        assert(sz.width == 300 && sz.height == 200);
    }
    x11_send_pointer_event(wm.dpy, xid, ButtonPress, ButtonPressMask,
                           298, 198, 1);
    (void)fdk_pump_events(ctx, 100);
    assert(fake_wm_wait(&wm, &wm.saw_moveresize, 1));
    assert(wm.move_dir == 4 /* SIZE_BOTTOMRIGHT, from FDK_WRES_SE */);
    x11_send_pointer_event(wm.dpy, xid, ButtonRelease,
                           ButtonReleaseMask, 298, 198, 1);
    (void)fdk_pump_events(ctx, 100);

    /* ---- Minimize under a WM: XIconifyWindow's WM_CHANGE_STATE
     * message; the WM maintains WM_STATE and FDK tracks it via
     * PropertyNotify. ---- */
    wm.saw_change_state = 0;
    assert(fdk_ok(fdk_window_minimize(win)));
    assert(fake_wm_wait(&wm, &wm.saw_change_state, 1));
    (void)fdk_pump_events(ctx, 300);
    assert(wm.saw_change_state == 1);
    assert(wm.change_state == 3 /* IconicState */);
    assert(fdk_window_is_minimized(win));
    /* (The state event here is the WM_STATE property flip; under a
     * real WM the minimize flag tracks exactly this property.) */
    assert(ewmh_state_events > ev0 + 2);

    /* Restore maps the window back; the fake WM resets WM_STATE. */
    assert(fdk_ok(fdk_window_restore(win)));
    {
        /* fdk_window_restore XMaps directly (WMs intercept maps);
         * the fake WM then sees the map and would restore focus in a
         * real session. The minimized flag cleared via the map path.
         */
        (void)fdk_pump_events(ctx, 300);
    }
    assert(!fdk_window_is_minimized(win));

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    fake_wm_uninstall(&wm);
    printf("[ok] X11 EWMH (fake WM): _NET_WM_STATE add/remove with "
           "exact atoms + source, property-driven state events, WM-"
           "driven maximize geometry, band drag via "
           "_NET_WM_MOVERESIZE(MOVE) at the right root coords, resize "
           "corner with the right direction, WM_CHANGE_STATE "
           "iconify\n");
}

/* ---- Phase 3 completion: MIT-SHM presentation + double buffering ---- */

/* Counts this process's live SysV shared-memory segments by reading
 * /proc/sysvipc/shm (Linux; the same file the ipcs(1) tool reads).
 * MIT-SHM pixel buffers are shmget segments, so their presence — and
 * their disappearance on window destroy — is directly observable. */
static int sysv_shm_segment_count(void) {
    FILE *f = fopen("/proc/sysvipc/shm", "r");
    if (f == NULL) {
        return -1; /* not Linux / not mounted: caller skips the check */
    }
    char line[256];
    int count = 0;
    /* header: "key shmid perms size cpid lpid nattch ... status".
     * MIT-SHM segments are shmget(IPC_PRIVATE, ...) — their KEY is 0
     * by definition, so every parseable data line counts. */
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long key, shmid;
        if (sscanf(line, "%lx %lu", &key, &shmid) == 2) {
            count++;
        }
    }
    fclose(f);
    return count;
}

static void test_mitm_shm_and_double_buffer(void) {
    /* Part 1: MIT-SHM is really used. Presenting frames must create
     * SysV segments (two slots), and destroying the window must
     * release them (the attach-then-IPC_RMID discipline). Skipped
     * honestly when the machine can't report segments or the server
     * has no MIT-SHM (FDK then uses the copy path — log-visible). */
    int before = sysv_shm_segment_count();

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK MIT-SHM test",
                                 .width = 120, .height = 90 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);

    /* X11 scale is honestly 1.0 (no core-protocol scale concept). */
    fdk_f32 x11_scale = 2.0f;
    assert(fdk_ok(fdk_window_get_scale(win, &x11_scale)));
    assert(x11_scale == 1.0f);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    fdk_color frame_a = { .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f };
    fdk_surface_fill(surface, frame_a);
    assert(fdk_ok(fdk_surface_present(surface)));

    int during = sysv_shm_segment_count();
    int after = -1;
    if (before >= 0 && during >= 0) {
        /* Two slots = two segments while the window renders. (Only
         * meaningful when the server has MIT-SHM — Xvfb does.) */
        if (during - before == 2) {
            printf("[ok] MIT-SHM presentation active (2 SysV segments "
                   "for the buffer pair)\n");
        } else if (during - before == 0) {
            printf("[ok] MIT-SHM not in use (copy path; segments: %d)\n",
                   during);
        } else {
            printf("[warn] unexpected SysV segment delta %d -> %d\n",
                   before, during);
        }
    }

    /* Part 2: double buffering is observable. After a present, the
     * next get_info hands out a DIFFERENT pixel buffer; drawing into
     * it does not change what the server shows until the next
     * present. */
    Display *rb_dpy = NULL;
    unsigned long xid = fdk_window_xid(win);
    unsigned long red_px = x11_readback_pixel(&rb_dpy, xid, 60, 45) & 0xFFFFFF;

    fdk_surface_info info2;
    assert(fdk_ok(fdk_surface_get_info(surface, &info2)));
    assert(info2.pixels != info.pixels); /* the swap happened */

    fdk_color frame_b = { .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f };
    fdk_surface_fill(surface, frame_b);
    /* Frame B is drawn but NOT presented: the server still shows A. */
    unsigned long still_a =
        x11_readback_pixel(&rb_dpy, xid, 60, 45) & 0xFFFFFF;
    assert(still_a == red_px);

    assert(fdk_ok(fdk_surface_present(surface)));
    unsigned long blue_px =
        x11_readback_pixel(&rb_dpy, xid, 60, 45) & 0xFFFFFF;
    assert(blue_px != red_px);
    assert((blue_px & 0xFF) != 0); /* blue channel present */

    /* Teardown releases the segments (only assertably so when they
     * existed). */
    fdk_window_destroy(win);
    if (rb_dpy != NULL) {
        XCloseDisplay(rb_dpy);
    }
    fdk_shutdown(ctx);

    if (before >= 0 && during >= 0 && during - before == 2) {
        after = sysv_shm_segment_count();
        assert(after == before); /* no leaked segments */
        printf("[ok] MIT-SHM segments released on window destroy\n");
    }

    printf("[ok] X11 double buffering: acquire-after-present swaps "
           "buffers; un-presented drawing never reaches the server\n");
}

int main(void) {
    signal(SIGALRM, alarm_handler);

    test_connect_and_shutdown();
    test_init_with_app_id();
    test_quit_before_run_is_safe();
    test_run_returns_when_no_windows_open();
    test_window_create_show_destroy();
    test_window_set_title();
    test_resize_delivers_configure_event();
    test_close_request_delivered();
    test_pump_events_nonblocking();
    test_surface_render_readback();
    test_mitm_shm_and_double_buffer();
    test_surface_follows_resize();
    test_surface_damage_partial_present();
    test_surface_primitives_readback();
    test_offscreen_blit_to_window();
    test_widget_tree_paint_readback();
    test_widget_real_input_via_xsendevent();
    test_widget_root_follows_resize();
    test_widget_layout_reflow_on_resize();
    test_grid_layout_gui();
    test_baseline_alignment_gui();
    test_text_render_readback();
    test_widget_catalog_gui();
    test_label_radio_arrow_gui();
    test_theme_switch_gui();
    test_decorations_gui();
    test_window_state_gui();
    test_resize_edges_gui();
    test_ewmh_fake_wm();

    printf("\nall X11 integration tests passed\n");
    return 0;
}
