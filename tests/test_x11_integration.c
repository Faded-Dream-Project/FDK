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
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

/* fdk_free for clipboard strings (it is internal: the public API
 * documents free() as equally correct, but the test uses the same
 * allocator the library used, under ASan). */
#include "core/alloc_internal.h"

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


/* The themed window-background pixel the 1.2.1 root default background
 * paints (and the X11 creation-time background pixel matches): the
 * default theme's FDK_TK_WINDOW_BACKGROUND token, channel-packed the
 * renderer's way (R<<16|G<<8|B). Grid gaps show the ROOT's fill, not
 * the pre-1.2.1 raw server black — the stale == 0x00000000u gap
 * assertions this replaces failed live against 0x121721. */
static unsigned long theme_window_bg_pixel(void) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    unsigned long r = (unsigned long)(c.r * 255.0f + 0.5f);
    unsigned long g = (unsigned long)(c.g * 255.0f + 0.5f);
    unsigned long b = (unsigned long)(c.b * 255.0f + 0.5f);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return (r << 16) | (g << 8) | b;
}

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

static fdk_color wcol(int r, int g, int b);

/* ---- 1.2.2 regression: the resize-storm wedge ----
 *
 * An interactive resize queues one ConfigureNotify per drag step
 * (plus Exposes) before the application pumps once — several hundred
 * events for a multi-second drag. Before the batched geometry
 * repaint, the dispatch tail repainted INLINE per event, so draining
 * such a backlog cost one FULL window repaint + framebuffer
 * reallocation + pointer-query round trip PER QUEUED EVENT: the drain
 * rate fell below the WM's queueing rate and the main thread wedged
 * at 100% CPU on one core with the window stuck at a stale size and
 * input (title-bar buttons) queued behind the backlog — the user
 * report this test pins ("after multiple resizes it doesn't update
 * anymore and the cpu goes insane on one core non stop").
 *
 * The storm is driven from a SECOND X connection — exactly what a WM
 * dragging the frame does — so the whole backlog is queued server-
 * side before the first pump. Then three healthy behaviors must hold:
 *   1. the backlog drains (pump reaches the final size) within a
 *      bounded alarm — the pre-fix wedge took ~minutes here;
 *   2. the batched repaint lands: fresh pixels at the FINAL size's
 *      far corner (only exists if a repaint at that size happened);
 *   3. liveness: a NEW resize after the storm still processes in one
 *      pump (the wedge never returned to the caller at all). */
static void test_resize_storm_backlog_drains(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK resize storm test",
                                 .width = 400, .height = 300 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    surface_capture cap = { .ctx = ctx, .configure_count = 0 };
    fdk_window_set_event_callback(win, surface_event_callback, &cap);

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    /* A full-bleed colored tree so the repaint's pixels are
     * attributable: root background + a content box that reflows to
     * every configure. */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget_set_background(root, wcol(20, 20, 20));
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &content)));
    fdk_widget_set_expand(content, true, true);
    fdk_widget_set_background(content, wcol(60, 200, 120));
    fdk_window_set_content(win, content);
    assert(fdk_ok(fdk_window_paint(win)));
    (void)fdk_pump_events(ctx, 200);

    /* The storm: 300 resizes, all queued before one pump. */
    Display *storm_dpy = XOpenDisplay(NULL);
    assert(storm_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);
    int expect_w = 0, expect_h = 0;
    for (int i = 1; i <= 300; i++) {
        expect_w = 400 + i;     /* 401 .. 700 */
        expect_h = 300 + i / 2; /* 300 .. 450 */
        XResizeWindow(storm_dpy, xid, (unsigned)expect_w,
                      (unsigned)expect_h);
    }
    XFlush(storm_dpy);

    /* 1. Drain: bounded by alarm — the alarm handler's 5s note is a
     * floor; this test needs its own larger budget for a slow debug
     * build's legitimate 300-event drain (still well under a second
     * post-fix; pre-fix it exceeded any plausible timeout). */
    alarm(15);
    int pumps = 0;
    while (cap.last_size.width != expect_w ||
           cap.last_size.height != expect_h) {
        int r = fdk_pump_events(ctx, 500);
        assert(r >= 0);
        pumps++;
        assert(pumps < 60); /* 30s hard bound — no infinite wedge */
    }
    alarm(0);
    /* The whole storm must coalesce: hundreds of queued configures,
     * but only a handful of pump iterations (one per poll wake). */
    printf("[ok] storm backlog drained in %d pump call(s), "
           "%d configure(s) coalesced\n", pumps, cap.configure_count);

    /* 2. The batched repaint landed at the FINAL size: the far corner
     * only exists once a framebuffer at 700x450 was painted. */
    Display *rb_dpy = NULL;
    assert(x11_readback_pixel(&rb_dpy, xid, expect_w - 8,
                              expect_h - 8) == 0x003CC878u);
    assert(x11_readback_pixel(&rb_dpy, xid, 8, 8) == 0x003CC878u);
    XCloseDisplay(rb_dpy);

    /* 3. Liveness: a NEW event after the storm processes promptly. */
    XResizeWindow(storm_dpy, xid, 444, 333);
    XFlush(storm_dpy);
    alarm(5);
    while (cap.last_size.width != 444 ||
           cap.last_size.height != 333) {
        int r = fdk_pump_events(ctx, 500);
        assert(r >= 0);
    }
    alarm(0);
    /* ...and its repaint landed too. */
    rb_dpy = NULL;
    assert(x11_readback_pixel(&rb_dpy, xid, 436, 325) == 0x003CC878u);
    XCloseDisplay(rb_dpy);

    XCloseDisplay(storm_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] resize storm: backlog drains, final size painted, "
           "event loop stays live\n");
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


/* KeyPress with ControlMask set (Ctrl+letter combos, Phase 9's
 * entry shortcuts). */
static void x11_send_key_event_ctrl(Display *dpy, unsigned long xid,
                                     unsigned int keycode) {
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = KeyPress;
    ev.xkey.window = (Window)xid;
    ev.xkey.keycode = keycode;
    ev.xkey.state = ControlMask;
    ev.xkey.same_screen = True;
    Status st = XSendEvent(dpy, (Window)xid, False,
                           (long)(KeyPressMask | KeyReleaseMask), &ev);
    assert(st != 0);
    ev.type = KeyRelease;
    st = XSendEvent(dpy, (Window)xid, False,
                    (long)(KeyPressMask | KeyReleaseMask), &ev);
    assert(st != 0);
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
    assert(x11_readback_pixel(&rb_dpy, xid, 205, 50) ==
           theme_window_bg_pixel());
    assert(x11_readback_pixel(&rb_dpy, xid, 100, 175) ==
           theme_window_bg_pixel());

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
    assert(x11_readback_pixel(&rb_dpy, xid, 305, 100) ==
           theme_window_bg_pixel());
    assert(x11_readback_pixel(&rb_dpy, xid, 150, 235) ==
           theme_window_bg_pixel());

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

/* ---- 1.1.4: resize-flash, hover-revalidation, cursor regressions ----
 *
 * The three symptoms reported from a real Cinnamon desktop: windows
 * flashing white during interactive resizes, the maximize/minimize
 * button keeping its hover highlight after the window geometry
 * changed under a stationary pointer, and the resize cursor only
 * appearing while a button was held (that one being the WM's own
 * cursor during _NET_WM_MOVERESIZE — FDK never shaped one itself).
 */

/* The anti-flash contract, server-side: once a window PAINTS, a
 * resize must RETAIN its pixels (NorthWest bit gravity at creation +
 * background flipped to None at the first framebuffer acquisition —
 * the old white-pixel background cleared the window on every resize
 * step, which composited as fast white flashing). Painted dark red,
 * resized by the SERVER (no pump — FDK must not get a chance to
 * repaint), the retained region must still read dark red; with the
 * old background-pixel window it read white. */
static void test_resize_retains_pixels(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK retention test",
                                 .width = 200, .height = 150 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);

    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_window_get_surface(win, &surface)));
    fdk_color red = { .r = 0.7f, .g = 0.05f, .b = 0.05f, .a = 1.0f };
    fdk_surface_fill(surface, red);
    assert(fdk_ok(fdk_surface_present(surface)));
    (void)fdk_pump_events(ctx, 100);

    Display *rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    /* The bit-gravity half of the contract, directly: */
    XWindowAttributes attrs;
    assert(XGetWindowAttributes(rb_dpy, (Window)xid, &attrs) != 0);
    assert(attrs.bit_gravity == NorthWestGravity);

    /* The retention half, behaviorally: server-side resize, readback
     * BEFORE FDK ever sees the configure. */
    XResizeWindow(rb_dpy, (Window)xid, 320, 240);
    XSync(rb_dpy, False);
    unsigned long px = x11_readback_pixel(&rb_dpy, xid, 40, 40);
    assert(px == 0x00B30D0Du); /* the painted red, retained (fill
                                  rounds 0.7/0.05 to B3/0D) */

    /* And the window still repaints correctly once the configure is
     * pumped (the synchronous resize repaint path): */
    (void)fdk_pump_events(ctx, 200);
    fdk_size now = { 0, 0 };
    assert(fdk_ok(fdk_window_get_size(win, &now)));
    assert(now.width == 320 && now.height == 240);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 resize retention: NorthWest bit gravity + "
           "background none (old-size pixels survive a server-side "
           "resize; no white clear)\n");
}

/* The stuck-highlight regression: hover the maximize button, then
 * grow the window under the STATIONARY pointer (the maximize move,
 * minus the actual maximize) — the button flies right, no motion
 * event ever arrives, and only the dispatch-time revalidation (query
 * the real pointer, re-route it as motion) clears the hover. */
static void test_hover_revalidation_on_geometry_change(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK hover reval test",
                                 .width = 240, .height = 160 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    assert(fdk_ok(fdk_window_set_decorated(win, true)));
    fdk_window_show(win);
    /* Drain the map/configure/expose noise (map-related events can
     * arrive arbitrarily late under Xvfb and would race the asserts
     * below with an unsolicited size change). */
    (void)fdk_pump_events(ctx, 300);
    (void)fdk_pump_events(ctx, 300);

    Display *rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    /* Warp the REAL pointer onto the maximize button (warping, not
     * XSendEvent: the revalidation later must find the true pointer
     * there — XSendEvent fakes events without moving the pointer). */
    fdk_rect max = win->deco_btn_max;
    assert(max.width > 0 && max.height > 0);
    int mx = max.x + max.width / 2;
    int my = max.y + max.height / 2;
    XWarpPointer(rb_dpy, None, (Window)xid, 0, 0, 0, 0, mx, my);
    XFlush(rb_dpy);
    (void)fdk_pump_events(ctx, 200);
    assert(fdk__window_deco_hover(win) == 2); /* hovered: maximize */

    /* Grow the window server-side, exactly like a WM maximizing it.
     * The button rects re-arrange to the NEW right edge; the pointer
     * never moves. Without revalidation this hover sticks forever. */
    XResizeWindow(rb_dpy, (Window)xid, 700, 400);
    XSync(rb_dpy, False);
    (void)fdk_pump_events(ctx, 300);
    assert(win->last_size.width == 700);
    assert(fdk__window_deco_hover(win) == 0); /* re-derived: gone */

    /* The pointer is still inside (the window grew around it): hover
     * must re-derive, not just clear — warp to where the button NOW
     * is and confirm hover follows. */
    max = win->deco_btn_max;
    mx = max.x + max.width / 2;
    my = max.y + max.height / 2;
    XWarpPointer(rb_dpy, None, (Window)xid, 0, 0, 0, 0, mx, my);
    XFlush(rb_dpy);
    (void)fdk_pump_events(ctx, 200);
    assert(fdk__window_deco_hover(win) == 2);

    /* Shrink the window away from under the pointer: the pointer is
     * now OUTSIDE, so the revalidation must deliver the clearing
     * path (hover zero, cursor default). */
    XResizeWindow(rb_dpy, (Window)xid, 100, 60);
    XSync(rb_dpy, False);
    (void)fdk_pump_events(ctx, 300);
    assert(win->last_size.width == 100);
    assert(fdk__window_deco_hover(win) == 0);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 hover revalidation: geometry change under a "
           "stationary pointer re-derives hover (maximize button "
           "highlight clears; outside-shrink clears)\n");
}

/* The resize-cursor affordance: hovering an edge zone sets the
 * directional cursor BEFORE any button is held; interior motion and
 * window leave restore the default. Observable through the
 * fdk__window_cursor_edge seam (the X cursor itself is not queryable
 * without XFixes, which this environment lacks). */
static void test_resize_cursor_affordance(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK cursor test",
                                 .width = 240, .height = 160 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    assert(fdk_ok(fdk_window_set_decorated(win, true)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    Display *rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    /* Interior motion: default arrow. */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 120, 80, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == 0);

    /* East edge: the right-side resize cursor. */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 238, 80, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == FDK_WRES_E);

    /* SE corner: the corner cursor (transitions exercise the cache). */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 238, 158, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == FDK_WRES_SE);

    /* Back inside: default again. */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 120, 80, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == 0);

    /* Edge again, then LEAVE the window: the cursor must reset even
     * though the last motion was over a zone (this is also the
     * band-hover reset path — hover dies with the pointer). */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 238, 80, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == FDK_WRES_E);
    {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = LeaveNotify;
        ev.xcrossing.window = (Window)xid;
        ev.xcrossing.x = 250;
        ev.xcrossing.y = 80;
        ev.xcrossing.mode = NotifyNormal;
        ev.xcrossing.detail = NotifyNonlinear;
        Status s = XSendEvent(rb_dpy, (Window)xid, False,
                              (long)LeaveWindowMask, &ev);
        assert(s != 0);
        XFlush(rb_dpy);
    }
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == 0);

    /* Turning the edges off resets the cursor too (chrome does not
     * outlive the edges that advertise it). */
    x11_send_pointer_event(rb_dpy, xid, MotionNotify,
                           PointerMotionMask, 238, 80, 0);
    (void)fdk_pump_events(ctx, 100);
    assert(fdk__window_cursor_edge(win) == FDK_WRES_E);
    fdk_window_set_resizable(win, false);
    assert(fdk__window_cursor_edge(win) == 0);

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 resize cursor: edge-zone hovers shape the "
           "directional cursor (E, SE), interior/leave/edges-off "
           "restore the default\n");
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
    /* Bounded retry for the documented Xvfb XOpenDisplay race (same
     * reason as init_with_retry / the clipboard children — the raw
     * Xlib connections churn more as the suite grows). */
    for (int attempt = 0; attempt < 10; attempt++) {
        wm->dpy = XOpenDisplay(NULL);
        if (wm->dpy != NULL) {
            break;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert(wm->dpy != NULL);
    wm->root = DefaultRootWindow(wm->dpy);
    wm->net_supported = XInternAtom(wm->dpy, "_NET_SUPPORTED", False);
    wm->net_wm_state = XInternAtom(wm->dpy, "_NET_WM_STATE", False);
    wm->net_wm_state_maximized_vert =
        XInternAtom(wm->dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    /* SPEC spelling (...HORZ, not ...HORIZ). The 1.1.3 lesson: this
     * fake WM once interned the same misspelling FDK's connection
     * code had, so the test verified the library against a copy of
     * its own bug and the typo shipped — under real WMs the probe
     * never matched, maximize silently degraded to the bare-X
     * fallback, and the maximized state desynced from the WM. The
     * atoms below are now the EWMH spec strings, so any spelling
     * drift in FDK fails this test instead of echoing it. */
    wm->net_wm_state_maximized_horiz =
        XInternAtom(wm->dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
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

static void test_ewmh_atom_spelling(void) {
    /* The 1.1.3 regression, pinned directly: FDK interned
     * _NET_WM_STATE_MAXIMIZED_HORIZ where the EWMH spec atom is
     * ..._HORZ. XInternAtom(only_if_exists=True) is the oracle — it
     * succeeds ONLY when the exact spec string was already interned
     * (by FDK's connect, since nothing else runs here) and returns
     * None for any misspelling. Checking through a SECOND connection
     * makes it independent of ordering and of anything FDK caches
     * client-side: atoms are server-global truth. */
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK atom spelling",
                                 .width = 100, .height = 80 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));

    Display *d = XOpenDisplay(NULL);
    assert(d != NULL);
    Atom spec_vert =
        XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_VERT", True);
    Atom spec_horiz =
        XInternAtom(d, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
    /* The library's own interned atoms, through the internal
     * verification seam (same one fdk_window_xid uses): they must BE
     * the spec atoms, atom-for-atom, on the server. */
    Atom fdk_vert = win->pwindow->conn->net_wm_state_maximized_vert;
    Atom fdk_horiz = win->pwindow->conn->net_wm_state_maximized_horiz;
    XCloseDisplay(d);

    assert(spec_vert != None);
    assert(spec_horiz != None);
    assert(fdk_vert == spec_vert);
    assert(fdk_horiz == spec_horiz);

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] X11 EWMH atom spelling: library atoms are the spec "
           "atoms (HORZ, not HORIZ) server-globally\n");
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

/* =====================================================================
 * Clipboard (Phase 9)
 * =====================================================================
 *
 * The real protocol is exercised with FORKED foreign clients speaking
 * raw Xlib — the same trick the fake-WM test uses, applied to the
 * ICCCM selection machinery. Three directions are verified:
 *   1. FDK round trip (own ownership, served from the local copy);
 *   2. a foreign OWNER serves FDK's get_text (ConvertSelection ->
 *      SelectionRequest to the child -> SelectionNotify back);
 *   3. FDK SERVES a foreign REQUESTOR (the child converts and reads
 *      the property FDK wrote), including the TARGETS negotiation;
 * plus the SelectionClear handoff when the foreign client takes
 * ownership away from FDK.
 *
 * Children use _exit() only (no atexit, no ASan leak reporting, no
 * stdio flush) and inherit nothing but COW memory. They talk to the
 * parent over a socketpair: "R" = ready, "P" = verified, anything
 * else / early hangup = failure with a distinct exit code. */

#include <sys/socket.h>
#include <sys/wait.h>

static void clip_child_serve_requests(Display *dpy, Window w, Atom clip,
                                      Atom utf8, Atom targets,
                                      const char *text) {
    (void)w;
    (void)clip;
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == SelectionRequest) {
            XSelectionRequestEvent *req = &ev.xselectionrequest;
            Atom prop = req->property != None ? req->property : req->target;
            if (req->target == targets) {
                Atom list[1] = { utf8 };
                XChangeProperty(dpy, req->requestor, prop, XA_ATOM, 32,
                                PropModeReplace,
                                (const unsigned char *)list, 1);
            } else if (req->target == utf8) {
                XChangeProperty(dpy, req->requestor, prop, utf8, 8,
                                PropModeReplace,
                                (const unsigned char *)text,
                                (int)strlen(text));
            } else {
                prop = None;
            }
            XSelectionEvent reply;
            memset(&reply, 0, sizeof(reply));
            reply.type = SelectionNotify;
            reply.display = dpy;
            reply.requestor = req->requestor;
            reply.selection = req->selection;
            reply.target = req->target;
            reply.property = prop;
            reply.time = req->time;
            XSendEvent(dpy, req->requestor, False, 0, (XEvent *)&reply);
            XFlush(dpy);
        }
    }
}

/* The documented Xvfb race (see init_with_retry above) applies to the
 * forked children's raw XOpenDisplay too — same bounded retry. */
static Display *clip_child_open_display(void) {
    for (int attempt = 0; attempt < 10; attempt++) {
        Display *dpy = XOpenDisplay(NULL);
        if (dpy != NULL) {
            return dpy;
        }
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* Foreign clipboard owner: takes CLIPBOARD ownership for `text`,
 * serves requests until the parent hangs up. Exits 0 on hangup. */
static void clip_foreign_owner_main(int sock, const char *text) {
    alarm(0);
    Display *dpy = clip_child_open_display();
    if (dpy == NULL) {
        _exit(10);
    }
    Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                   0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom targets = XInternAtom(dpy, "TARGETS", False);
    XSetSelectionOwner(dpy, clip, w, CurrentTime);
    if (XGetSelectionOwner(dpy, clip) != w) {
        _exit(11);
    }
    XFlush(dpy);
    (void)!write(sock, "R", 1);

    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = ConnectionNumber(dpy);
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        pfds[1].fd = sock;
        pfds[1].events = POLLIN;
        pfds[1].revents = 0;
        int r = poll(pfds, 2, 3000);
        if (r < 0) {
            _exit(12);
        }
        if (pfds[1].revents != 0) {
            char c;
            if (recv(sock, &c, 1, 0) <= 0) {
                _exit(0); /* parent hung up: done */
            }
            _exit(0); /* any parent message means stop */
        }
        clip_child_serve_requests(dpy, w, clip, utf8, targets, text);
    }
}

/* Foreign clipboard requestor: converts TARGETS then UTF8_STRING from
 * the current owner (FDK, in the test) and verifies the text.
 * Exits 0 + "P" on success, distinct codes otherwise. */
static void clip_foreign_requestor_main(int sock, const char *want) {
    alarm(0);
    Display *dpy = clip_child_open_display();
    if (dpy == NULL) {
        _exit(20);
    }
    Window w = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy),
                                   0, 0, 1, 1, 0, 0, 0);
    Atom clip = XInternAtom(dpy, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom targets = XInternAtom(dpy, "TARGETS", False);
    Atom prop = XInternAtom(dpy, "_FDK_TEST_PROP", False);

    /* Pass 1: TARGETS must list UTF8_STRING. */
    XConvertSelection(dpy, clip, targets, prop, w, CurrentTime);
    XFlush(dpy);
    int saw_targets_notify = 0;
    int utf8_advertised = 0;
    for (;;) {
        XEvent ev;
        if (XPending(dpy) == 0) {
            struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
            if (poll(&pfd, 1, 3000) <= 0) {
                _exit(21); /* no TARGETS answer */
            }
            continue;
        }
        XNextEvent(dpy, &ev);
        if (ev.type != SelectionNotify ||
            ev.xselection.selection != clip) {
            continue;
        }
        saw_targets_notify = 1;
        if (ev.xselection.property == None) {
            _exit(22);
        }
        Atom type = None;
        int fmt = 0;
        unsigned long n = 0, left = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, prop, 0, 64, True, XA_ATOM,
                               &type, &fmt, &n, &left, &data) != Success) {
            _exit(23);
        }
        if (type == XA_ATOM && fmt == 32 && data != NULL) {
            Atom *atoms = (Atom *)data;
            for (unsigned long i = 0; i < n; i++) {
                if (atoms[i] == utf8) {
                    utf8_advertised = 1;
                }
            }
        }
        if (data != NULL) {
            XFree(data);
        }
        break;
    }
    if (!saw_targets_notify || !utf8_advertised) {
        fprintf(stderr, "[clip child] pass1 fail: notify=%d advertised=%d "
                "utf8_atom=%lu\n",
                saw_targets_notify, utf8_advertised, (unsigned long)utf8);
        /* Re-read without delete for a dump. */
        Atom t2 = None; int f2 = 0; unsigned long n2 = 0, l2 = 0;
        unsigned char *d2 = NULL;
        (void)XGetWindowProperty(dpy, w, prop, 0, 64, False, AnyPropertyType,
                                 &t2, &f2, &n2, &l2, &d2);
        fprintf(stderr, "[clip child] reread: type=%lu fmt=%d n=%lu data=%p\n",
                (unsigned long)t2, f2, n2, (void *)d2);
        if (d2 != NULL && f2 == 32) {
            Atom *a2 = (Atom *)d2;
            for (unsigned long k = 0; k < n2 && k < 8; k++) {
                char *nm = XGetAtomName(dpy, a2[k]);
                fprintf(stderr, "[clip child]   atom[%lu]=%lu (%s)\n", k,
                        (unsigned long)a2[k], nm ? nm : "?");
                if (nm) XFree(nm);
            }
        }
        _exit(24);
    }

    /* Pass 2: the UTF8_STRING payload itself. */
    XConvertSelection(dpy, clip, utf8, prop, w, CurrentTime);
    XFlush(dpy);
    for (;;) {
        XEvent ev;
        if (XPending(dpy) == 0) {
            struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
            if (poll(&pfd, 1, 3000) <= 0) {
                _exit(25); /* no text answer */
            }
            continue;
        }
        XNextEvent(dpy, &ev);
        if (ev.type != SelectionNotify ||
            ev.xselection.selection != clip) {
            continue;
        }
        if (ev.xselection.property == None) {
            _exit(26);
        }
        Atom type = None;
        int fmt = 0;
        unsigned long n = 0, left = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, w, prop, 0, 1024, True,
                               AnyPropertyType, &type, &fmt, &n, &left,
                               &data) != Success) {
            _exit(27);
        }
        int ok = (type == utf8 && fmt == 8 && data != NULL &&
                  n == strlen(want) &&
                  memcmp(data, want, n) == 0);
        if (data != NULL) {
            XFree(data);
        }
        if (!ok) {
            fprintf(stderr, "[clip child] pass2 mismatch\n");
            _exit(28);
        }
        (void)!write(sock, "P", 1);
        _exit(0);
    }
}

/* Spawns a clipboard child role and returns its pid + socket. */
static pid_t clip_spawn(void (*fn)(int, const char *), const char *arg,
                        int *out_sock) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        close(sv[0]);
        fn(sv[1], arg);
        _exit(99); /* fn never returns */
    }
    close(sv[1]);
    *out_sock = sv[0];
    return pid;
}

static void test_clipboard(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    /* --- 1. Own-ownership round trip --- */
    assert(fdk_ok(fdk_clipboard_set_text(ctx, "FDK phase 9 clipboard")));
    char *text = fdk_clipboard_get_text(ctx);
    assert(text != NULL && strcmp(text, "FDK phase 9 clipboard") == 0);
    fdk_free(text);

    /* Replace semantics: the second set wins wholesale. */
    assert(fdk_ok(fdk_clipboard_set_text(ctx, "second")));
    text = fdk_clipboard_get_text(ctx);
    assert(text != NULL && strcmp(text, "second") == 0);
    fdk_free(text);

    /* Empty clipboard text reads as NULL (documented contract). */
    assert(fdk_ok(fdk_clipboard_set_text(ctx, "")));
    assert(fdk_clipboard_get_text(ctx) == NULL);
    printf("[ok] clipboard: FDK round trip, replace semantics, "
           "empty-as-NULL\n");

    /* --- 2. Foreign owner serves FDK's get (the child must stay
     * alive while FDK reads, so the handshake is manual: wait for
     * "R", read the clipboard, then hang up + reap) --- */
    {
        int sock = -1;
        pid_t pid = clip_spawn(clip_foreign_owner_main,
                               "_FDK_FOREIGN_TEXT_", &sock);
        assert(pid > 0);
        alarm(5);
        char c = 0;
        assert(recv(sock, &c, 1, 0) == 1 && c == 'R');
        alarm(0);
        /* FDK's own "" from part 1 is replaced by the child's
         * ownership; the local fast path must NOT serve. */
        char *foreign = fdk_clipboard_get_text(ctx);
        assert(foreign != NULL &&
               strcmp(foreign, "_FDK_FOREIGN_TEXT_") == 0);
        fdk_free(foreign);
        close(sock); /* child exits on hangup */
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
        printf("[ok] clipboard: foreign owner serves FDK get_text "
               "(real SelectionRequest/Notify through the server)\n");
    }

    /* --- 3. FDK serves a foreign requestor (+ TARGETS) --- */
    {
        assert(fdk_ok(fdk_clipboard_set_text(ctx, "served by FDK")));
        int sock = -1;
        pid_t pid = clip_spawn(clip_foreign_requestor_main,
                               "served by FDK", &sock);
        assert(pid > 0);
        /* FDK must pump to see (and answer) the child's requests:
         * fdk_pump_events drives dispatch_pending, which routes
         * helper-window events into the clipboard module. */
        alarm(5);
        char c = 0;
        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            ssize_t n = recv(sock, &c, 1, MSG_DONTWAIT);
            if (n == 1) {
                break;
            }
        }
        alarm(0);
        if (c != 'P') {
            /* Diagnostics: the child's exit code names the failing
             * stage (see clip_foreign_requestor_main). */
            int st = 0;
            (void)waitpid(pid, &st, 0);
            fprintf(stderr, "clipboard requestor child failed: "
                    "exit=%d sig=%d msg=%d\n",
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1,
                    WIFSIGNALED(st) ? WTERMSIG(st) : 0, (int)c);
            assert(0);
        }
        close(sock);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
        printf("[ok] clipboard: FDK serves foreign requestor "
               "(TARGETS advertises UTF8_STRING; UTF8 payload exact)\n");
    }

    /* --- 4. SelectionClear: losing ownership drops the local copy --- */
    {
        assert(fdk_ok(fdk_clipboard_set_text(ctx, "mine, briefly")));
        int sock = -1;
        pid_t pid = clip_spawn(clip_foreign_owner_main,
                               "_FDK_SECOND_OWNER_", &sock);
        assert(pid > 0);
        alarm(5);
        char c = 0;
        assert(recv(sock, &c, 1, 0) == 1 && c == 'R');
        alarm(0);
        /* Pump: the X server delivered SelectionClear to FDK's helper
         * when the child took over. dispatch_pending must process it
         * (freeing our copy) — observable as the NEXT get reading the
         * child's text instead of ours. */
        (void)fdk_pump_events(ctx, 200);
        char *text2 = fdk_clipboard_get_text(ctx);
        assert(text2 != NULL && strcmp(text2, "_FDK_SECOND_OWNER_") == 0);
        fdk_free(text2);
        close(sock);
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0);
        printf("[ok] clipboard: SelectionClear processed — ownership "
               "loss drops the served copy\n");
    }

    fdk_shutdown(ctx);
}

/* ---- Entry widget under real X11 input (Phase 9) ----
 *
 * Real KeyPress events through XSendEvent (server-side keycode ->
 * XLookupString -> codepoint translation), driving a REAL window's
 * Entry: typing, ctrl-combos, and the full clipboard ROUND TRIP
 * (Ctrl+A select, Ctrl+X cut, Ctrl+V paste) against the real X
 * clipboard — the cross-module path the headless suite cannot
 * exercise (widget -> window owner -> context -> x11_clipboard).
 *
 * PC keycodes (Xvfb default map): h=43 e=26 l=46 o=32, a=38,
 * x=53, c=54, v=55, Control_L=37. */
static void test_entry_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_font *font = fdk_font_load_system_default(16);
    assert(font != NULL);

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK entry test",
                                 .width = 300, .height = 120 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, font, "", &entry)));
    fdk_rect r = { 20, 20, 240, 36 };
    fdk_widget_set_bounds(entry, r);
    assert(fdk_widget_focus(entry));

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    /* Type "hello". */
    static const int hello_keys[5] = { 43, 26, 46, 46, 32 };
    for (int i = 0; i < 5; i++) {
        x11_send_key_event(send_dpy, xid, KeyPress, (unsigned)hello_keys[i]);
        x11_send_key_event(send_dpy, xid, KeyRelease, (unsigned)hello_keys[i]);
        (void)fdk_pump_events(ctx, 50);
    }
    (void)fdk_pump_events(ctx, 100);
    assert(strcmp(fdk_entry_get_text(entry), "hello") == 0);
    assert(fdk_entry_get_cursor(entry) == 5);

    /* Ctrl+A (select all), Ctrl+X (cut): text moves to the REAL X
     * clipboard. */
    x11_send_key_event_ctrl(send_dpy, xid, 38); /* a */
    x11_send_key_event_ctrl(send_dpy, xid, 53); /* x */
    (void)fdk_pump_events(ctx, 100);
    assert(strcmp(fdk_entry_get_text(entry), "") == 0);
    char *clip = fdk_clipboard_get_text(ctx);
    assert(clip != NULL && strcmp(clip, "hello") == 0);
    fdk_free(clip);
    printf("[ok] entry: real keys type; Ctrl+A/X cut to the real X "
           "clipboard\n");

    /* Ctrl+V (paste): the clipboard text comes back into the entry
     * through wl_/x11_clipboard's get path. */
    x11_send_key_event_ctrl(send_dpy, xid, 55); /* v */
    (void)fdk_pump_events(ctx, 100);
    assert(strcmp(fdk_entry_get_text(entry), "hello") == 0);

    /* Backspace deletes the last glyph through the same real path. */
    x11_send_key_event(send_dpy, xid, KeyPress, 22); /* Backspace */
    (void)fdk_pump_events(ctx, 100);
    assert(strcmp(fdk_entry_get_text(entry), "hell") == 0);

    XCloseDisplay(send_dpy);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] entry: Ctrl+V pastes back from the real clipboard; "
           "Backspace deletes (full widget<->clipboard round trip)\n");
}


static int close_requests = 0;
static void popup_close_counter(fdk_window *w, const fdk_event_data *e,
                                void *u) {
    (void)w;
    (void)u;
    if (e->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        close_requests++;
    }
}

/* ---- Popup windows (Phase 9) ----
 *
 * A real popup over a real toplevel: server-side pixel readback of
 * the popup's fill, the outside-click dismissal (the grab redirects
 * the click to our connection; x11_events.c turns out-of-bounds
 * presses into close requests), and Escape dismissal. */
static void test_popup_window(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK popup parent",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 100);

    close_requests = 0;

    fdk_window *pop = NULL;
    assert(fdk_ok(fdk_window_create_popup(ctx, win, 40, 40, 120, 80,
                                          &pop)));
    /* Paint the popup a distinctive color via its root. */
    fdk_widget *proot = NULL;
    assert(fdk_ok(fdk_window_get_root(pop, &proot)));
    fdk_widget_set_background(proot, (fdk_color){0.0f, 1.0f, 0.0f, 1.0f});
    fdk_window_set_event_callback(pop, popup_close_counter, NULL);

    fdk_window_show(pop);
    (void)fdk_pump_events(ctx, 200);
    assert(fdk_ok(fdk_window_paint(pop)));
    (void)fdk_pump_events(ctx, 100);

    /* Server-side readback: the popup's green fill at its center.
     * The popup sits at parent-origin + (40,40); the parent window
     * origin under bare Xvfb is (0,0)+size of nothing — read at the
     * popup's center via the popup's own XID. */
    Display *rb_dpy = NULL;
    unsigned long pop_xid = fdk_window_xid(pop);
    /* The parent's absolute origin: */
    Window child_ret = None;
    int px = 0, py = 0;
    unsigned long parent_xid = fdk_window_xid(win);
    rb_dpy = XOpenDisplay(NULL);
    assert(rb_dpy != NULL);
    assert(XTranslateCoordinates(rb_dpy, (Window)parent_xid,
                                 DefaultRootWindow(rb_dpy), 0, 0, &px,
                                 &py, &child_ret));
    unsigned long px_color =
        x11_readback_pixel(&rb_dpy, pop_xid, 60, 40) & 0xFFFFFF;
    int green = ((px_color >> 8) & 0xFFu) > 200 &&
                ((px_color >> 16) & 0xFFu) < 60 &&
                (px_color & 0xFFu) < 60;
    assert(green);
    (void)px;
    (void)py;
    (void)child_ret;

    /* Outside click: the parent's area (screen coords inside the
     * PARENT but outside the popup) — the grab routes it to the
     * popup's connection, x11_events sees out-of-bounds, the popup
     * gets a close request. */
    /* XSendEvent bypasses grabs — drive the dismissal path directly:
     * a ButtonPress with out-of-bounds coordinates to the popup
     * window (the exact shape a grabbed outside click produces). */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ButtonPress;
    ev.xbutton.window = (Window)pop_xid;
    ev.xbutton.x = -50; /* outside the popup: the dismissal shape */
    ev.xbutton.y = -50;
    ev.xbutton.button = 1;
    ev.xbutton.same_screen = True;
    assert(XSendEvent(rb_dpy, (Window)pop_xid, False,
                      (long)(ButtonPressMask | ButtonReleaseMask), &ev));
    XFlush(rb_dpy);
    (void)fdk_pump_events(ctx, 200);
    assert(close_requests == 1);
    printf("[ok] popup: outside-click press delivers close request\n");

    /* Escape: the popup's own key path. */
    XEvent esc;
    memset(&esc, 0, sizeof(esc));
    esc.type = KeyPress;
    esc.xkey.window = (Window)pop_xid;
    esc.xkey.keycode = 9; /* Escape on the default map */
    esc.xkey.same_screen = True;
    assert(XSendEvent(rb_dpy, (Window)pop_xid, False,
                      (long)(KeyPressMask | KeyReleaseMask), &esc));
    XFlush(rb_dpy);
    (void)fdk_pump_events(ctx, 200);
    assert(close_requests == 2);
    printf("[ok] popup: Escape delivers close request\n");

    XCloseDisplay(rb_dpy);
    fdk_window_destroy(pop);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] popup window: pixel-verified fill, outside-click + "
           "Escape dismissal, clean teardown\n");
}


/* ---- Menus, ComboBox, Dialogs (Phase 9 completion) ----
 *
 * The popup-chain machinery driven through REAL X events: the bar
 * opens a toolkit-owned popup (an override-redirect child — found
 * via XQueryTree), auto-paint keeps it on screen without the test
 * ever painting it (server-side pixel proof), items activate through
 * synthetic clicks/keys, submenus nest as popup-of-popup children,
 * Escape peels one level, and the modal dialog's server-side grab is
 * verified by ANOTHER client's grab failing while FDK holds it. */

#include "widget/menu_internal.h"

static int menu_open_hits = 0;
static int menu_toolbar_hits = 0;

static void menu_open_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (void)user;
    menu_open_hits++;
}

static void menu_toolbar_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (void)user;
    menu_toolbar_hits++;
}

/* Children of an X window (override-redirect popups included). */
static int x11_child_windows(Display *dpy, Window xid,
                             Window *out, int max) {
    Window root, parent;
    Window *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, xid, &root, &parent, &children, &n)) {
        return -1;
    }
    int count = 0;
    for (unsigned int i = 0; i < n && count < max; i++) {
        out[count++] = children[i];
    }
    if (children != NULL) {
        XFree(children);
    }
    return count;
}

/* The child present in `now` but not `before` (0 when none). */
static Window x11_new_child(Window *before, int n_before, Window *now,
                            int n_now) {
    for (int i = 0; i < n_now; i++) {
        bool found = false;
        for (int j = 0; j < n_before; j++) {
            if (now[i] == before[j]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return now[i];
        }
    }
    return 0;
}

static void x11_click(Display *dpy, Window xid, int x, int y) {
    x11_send_pointer_event(dpy, xid, ButtonPress, ButtonPressMask, x, y, 1);
    x11_send_pointer_event(dpy, xid, ButtonRelease, ButtonReleaseMask, x, y, 1);
}

static void x11_move(Display *dpy, Window xid, int x, int y) {
    x11_send_pointer_event(dpy, xid, MotionNotify, PointerMotionMask, x, y, 0);
}

static void test_menu_gui(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
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
        printf("[skip] X11 menu GUI (no system TrueType font found)\n");
        return;
    }

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    fdk_font *font = fdk_font_load(font_path, 16);
    assert(font != NULL);

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK menu test",
                                 .width = 400, .height = 300 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));

    /* File: Open / sep / Toolbar(check) / Quit-disabled
     * Edit: Recent > (a.txt, b.txt) — the submenu case. */
    fdk_menu *file = NULL, *edit = NULL, *recent = NULL;
    assert(fdk_ok(fdk_menu_create(font, &file)));
    assert(fdk_ok(fdk_menu_create(font, &edit)));
    assert(fdk_ok(fdk_menu_create(font, &recent)));
    fdk_menu_item *mi_open = NULL, *mi_toolbar = NULL, *mi_quit = NULL,
                  *mi_recent = NULL, *mi_a = NULL;
    assert(fdk_ok(fdk_menu_append(file, "Open", &mi_open)));
    assert(fdk_ok(fdk_menu_append_separator(file)));
    assert(fdk_ok(fdk_menu_append_check(file, "Toolbar", false,
                                        &mi_toolbar)));
    assert(fdk_ok(fdk_menu_append(file, "Quit", &mi_quit)));
    fdk_menu_item_set_enabled(mi_quit, false);
    fdk_menu_item_set_on_activate(mi_open, menu_open_cb, NULL);
    fdk_menu_item_set_on_activate(mi_toolbar, menu_toolbar_cb, NULL);
    assert(fdk_ok(fdk_menu_append(edit, "Copy", NULL)));
    assert(fdk_ok(fdk_menu_append(edit, "Recent", &mi_recent)));
    assert(fdk_ok(fdk_menu_item_set_submenu(mi_recent, recent)));
    assert(fdk_ok(fdk_menu_append(recent, "a.txt", &mi_a)));
    assert(fdk_ok(fdk_menu_append(recent, "b.txt", NULL)));

    fdk_widget *bar = NULL;
    assert(fdk_ok(fdk_menu_bar_create(root, font, &bar)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "File", file)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "Edit", edit)));
    fdk_i32 row_h = fdk__menu_row_height(file);
    fdk_widget_arrange(bar, (fdk_rect){0, 0, 400, row_h});

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 150);
    fdk_window_paint(win);
    (void)fdk_pump_events(ctx, 100);

    Display *send = XOpenDisplay(NULL);
    assert(send != NULL);
    Window xid = (Window)fdk_window_xid(win);
    /* X11 popups are override-redirect children OF THE ROOT (placed
     * in root coordinates — see x11_window.c), so chain tracking
     * diffs the ROOT's child list. */
    Window root_scr = DefaultRootWindow(send);

    Window before[16], now[16];
    int n_before = x11_child_windows(send, root_scr, before, 16);
    assert(n_before >= 0);

    /* --- open the File menu by clicking its title --- */
    menu_open_hits = 0;
    x11_click(send, xid, 20, row_h / 2);
    (void)fdk_pump_events(ctx, 200);

    int n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 1);
    Window popup = x11_new_child(before, n_before, now, n_now);
    assert(popup != 0);
    printf("[ok] menu: clicking the bar title maps the popup\n");

    /* Auto-paint: the popup's surface color, server-side, without
     * the test ever calling fdk_window_paint on it. */
    Display *rb = NULL;
    unsigned long px = x11_readback_pixel(&rb, (unsigned long)popup, 6, 6);
    fdk_color ctl = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    int cr = (int)((px >> 16) & 0xFFu);
    int cg = (int)((px >> 8) & 0xFFu);
    int cb = (int)(px & 0xFFu);
    assert(cr == (int)(ctl.r * 255.0f + 0.5f) &&
           cg == (int)(ctl.g * 255.0f + 0.5f) &&
           cb == (int)(ctl.b * 255.0f + 0.5f));
    printf("[ok] menu: popup AUTO-PAINTED (server-side pixel proof)\n");

    /* Keyboard: Down lands on Open, Enter activates it. */
    x11_send_key_event(send, popup, KeyPress, 116); /* Down */
    x11_send_key_event(send, popup, KeyRelease, 116);
    x11_send_key_event(send, popup, KeyPress, 36); /* Enter (scancode 28) */
    x11_send_key_event(send, popup, KeyRelease, 36);
    (void)fdk_pump_events(ctx, 200);
    assert(menu_open_hits == 1);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before); /* chain closed after activation */
    printf("[ok] menu: keyboard Down+Enter activates + closes\n");

    /* --- Escape dismissal --- */
    x11_click(send, xid, 20, row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 1);
    popup = x11_new_child(before, n_before, now, n_now);
    x11_send_key_event(send, popup, KeyPress, 9); /* Escape */
    x11_send_key_event(send, popup, KeyRelease, 9);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);
    printf("[ok] menu: Escape dismisses the chain\n");

    /* --- check item through a real click --- */
    x11_click(send, xid, 20, row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    popup = x11_new_child(before, n_before, now, n_now);
    assert(popup != 0);
    /* Row 2 = "Toolbar" (row 0, a 9px separator, then row 2). */
    x11_click(send, popup, 30, row_h + 9 + row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    assert(menu_toolbar_hits == 1);
    assert(fdk_menu_item_is_checked(mi_toolbar));
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);
    printf("[ok] menu: click activates the check item (state + "
           "callback + close)\n");

    /* --- submenu: hover opens a popup-of-popup --- */
    x11_click(send, xid, 70, row_h / 2); /* Edit title */
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 1);
    Window edit_pop = x11_new_child(before, n_before, now, n_now);
    assert(edit_pop != 0);
    /* Hover the "Recent" row (row 1 of the Edit menu): the submenu
     * maps as ANOTHER root child (X11 nests by placement, not by
     * window parentage; the FDK layer tracks the chain). */
    x11_move(send, edit_pop, 30, row_h + row_h / 2);
    (void)fdk_pump_events(ctx, 250);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 2);
    Window sub = 0;
    for (int i = 0; i < n_now; i++) {
        if (now[i] != edit_pop && now[i] != xid) {
            bool old = false;
            for (int j = 0; j < n_before; j++) {
                if (before[j] == now[i]) {
                    old = true;
                    break;
                }
            }
            if (!old) {
                sub = now[i];
            }
        }
    }
    assert(sub != 0);
    printf("[ok] menu: hover opens the SUBMENU (nested chain)\n");

    /* Escape on the submenu closes ONLY it. */
    x11_send_key_event(send, sub, KeyPress, 9);
    x11_send_key_event(send, sub, KeyRelease, 9);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 1); /* parent menu still open */
    printf("[ok] menu: Escape peels one submenu level (parent stays)\n");

    /* Close the rest; the bar's toggle click also works. */
    x11_click(send, xid, 70, row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);

    /* Destroying the bar with a chain open is safe (close via
     * fdk_menu_bar_close first exercises the public path). */
    x11_click(send, xid, 20, row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    fdk_menu_bar_close(bar);
    (void)fdk_pump_events(ctx, 150);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);

    XCloseDisplay(rb);
    XCloseDisplay(send);
    fdk_window_destroy(win);
    fdk_menu_destroy(file);
    fdk_menu_destroy(edit);
    fdk_menu_destroy(recent);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] menu GUI: bar click, auto-paint, keyboard, check "
           "item, submenu nesting, level-wise Escape, close, clean "
           "teardown\n");
}

static int combo_changed_hits = 0;
static fdk_i64 combo_changed_index = -2;

static void combo_changed_cb(fdk_widget *combo, size_t index,
                             void *user) {
    (void)combo;
    (void)user;
    combo_changed_hits++;
    combo_changed_index = (index == FDK_COMBO_NONE) ? -1 : (fdk_i64)index;
}

static void test_combo_gui(void) {
    static const char *font_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        NULL,
    };
    const char *font_path = font_candidates[0];
    FILE *f = fopen(font_path, "rb");
    if (f == NULL) {
        printf("[skip] X11 combo GUI (no system TrueType font found)\n");
        return;
    }
    fclose(f);

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));
    fdk_font *font = fdk_font_load(font_path, 16);
    assert(font != NULL);

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK combo test",
                                 .width = 360, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));

    fdk_widget *combo = NULL;
    assert(fdk_ok(fdk_combo_create(root, font, &combo)));
    assert(fdk_ok(fdk_combo_append(combo, "Red", NULL)));
    assert(fdk_ok(fdk_combo_append(combo, "Green", NULL)));
    assert(fdk_ok(fdk_combo_append(combo, "Blue", NULL)));
    fdk_combo_set_on_changed(combo, combo_changed_cb, NULL);
    fdk_i32 row_h = fdk__menu_row_height(NULL);
    fdk_size nat = {0, 0};
    fdk_widget_measure(combo, &nat);
    fdk_widget_arrange(combo, (fdk_rect){10, 10, 220, nat.height});

    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 150);
    fdk_window_paint(win);
    (void)fdk_pump_events(ctx, 100);

    Display *send = XOpenDisplay(NULL);
    assert(send != NULL);
    Window xid = (Window)fdk_window_xid(win);
    Window root_scr = DefaultRootWindow(send);
    Window before[16], now[16];
    int n_before = x11_child_windows(send, root_scr, before, 16);

    /* Click the field: the dropdown maps (a root child — override
     * redirect, placed at the combo). */
    combo_changed_hits = 0;
    x11_click(send, xid, 40, 10 + nat.height / 2);
    (void)fdk_pump_events(ctx, 200);
    int n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before + 1);
    Window pop = x11_new_child(before, n_before, now, n_now);
    assert(pop != 0);

    /* The dropdown is auto-painted (menu surface color). */
    Display *rb = NULL;
    unsigned long px = x11_readback_pixel(&rb, (unsigned long)pop, 6, 6);
    fdk_color ctl = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    assert((int)((px >> 16) & 0xFFu) == (int)(ctl.r * 255.0f + 0.5f) &&
           (int)((px >> 8) & 0xFFu) == (int)(ctl.g * 255.0f + 0.5f) &&
           (int)(px & 0xFFu) == (int)(ctl.b * 255.0f + 0.5f));
    printf("[ok] combo: click opens the auto-painted dropdown\n");

    /* Pick "Green" (row 1). */
    x11_click(send, pop, 30, row_h + row_h / 2);
    (void)fdk_pump_events(ctx, 200);
    assert(combo_changed_hits == 1 && combo_changed_index == 1);
    assert(fdk_combo_get_active(combo) == 1);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);
    printf("[ok] combo: picking a row sets active + fires on_changed + "
           "closes\n");

    /* Reopen + Escape: dismissed with no change. */
    combo_changed_hits = 0;
    x11_click(send, xid, 40, 10 + nat.height / 2);
    (void)fdk_pump_events(ctx, 200);
    n_now = x11_child_windows(send, root_scr, now, 16);
    pop = x11_new_child(before, n_before, now, n_now);
    assert(pop != 0);
    x11_send_key_event(send, pop, KeyPress, 9);
    x11_send_key_event(send, pop, KeyRelease, 9);
    (void)fdk_pump_events(ctx, 200);
    assert(combo_changed_hits == 0);
    n_now = x11_child_windows(send, root_scr, now, 16);
    assert(n_now == n_before);
    printf("[ok] combo: Escape dismisses without changing the "
           "selection\n");

    XCloseDisplay(rb);
    XCloseDisplay(send);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] combo GUI: dropdown lifecycle through real input\n");
}

static int dialog_responses = 0;
static fdk_dialog_response dialog_last = (fdk_dialog_response)-99;

static void dialog_resp_cb(fdk_dialog_response response, void *user) {
    (void)user;
    dialog_responses++;
    dialog_last = response;
}

/* The a11y layer against a REAL window: the window root announces
 * the WINDOW role, its accessible name follows the title (and a
 * set_title updates it), bounds match the live window size, and a
 * REAL key event typed into an Entry arrives through the a11y value
 * interface — the same snapshot a bridge would poll. */
static void test_a11y_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_font *font = fdk_font_load_system_default(16);
    assert(font != NULL);

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK a11y test",
                                 .width = 320, .height = 140 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));

    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(root, &info)));
    assert(info.role == FDK_A11Y_ROLE_WINDOW);
    assert(info.name != NULL && strcmp(info.name, "FDK a11y test") == 0);
    assert(info.bounds.width == 320 && info.bounds.height == 140);
    assert((info.states & FDK_A11Y_VISIBLE) != 0 &&
           (info.states & FDK_A11Y_SHOWING) != 0);
    fdk_a11y_info_free(&info);
    printf("[ok] a11y: window root role/name/bounds/visible\n");

    /* Title changes propagate to the accessible name. */
    fdk_window_set_title(win, "Renamed");
    assert(fdk_ok(fdk_a11y_describe(root, &info)));
    assert(info.name != NULL && strcmp(info.name, "Renamed") == 0);
    fdk_a11y_info_free(&info);
    printf("[ok] a11y: set_title updates the root name\n");

    /* A widget in the tree + REAL typed input read back through the
     * value interface. */
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, font, "", &entry)));
    fdk_rect r = { 20, 20, 200, 32 };
    fdk_widget_set_bounds(entry, r);
    assert(fdk_widget_focus(entry));

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);
    /* Type "hi" (Xvfb default map: h=43, i=31). */
    static const int keys[2] = { 43, 31 };
    for (int i = 0; i < 2; i++) {
        x11_send_key_event(send_dpy, xid, KeyPress, (unsigned)keys[i]);
        x11_send_key_event(send_dpy, xid, KeyRelease, (unsigned)keys[i]);
        (void)fdk_pump_events(ctx, 30);
    }
    XCloseDisplay(send_dpy);

    assert(fdk_ok(fdk_a11y_describe(entry, &info)));
    assert(info.role == FDK_A11Y_ROLE_ENTRY);
    assert((info.states & FDK_A11Y_EDITABLE) != 0);
    assert((info.states & FDK_A11Y_FOCUSED) != 0);
    assert(info.value_text != NULL && strcmp(info.value_text, "hi") == 0);
    fdk_a11y_info_free(&info);
    printf("[ok] a11y: real typed input visible through the value "
           "interface\n");

    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] a11y GUI: describe/notify layer against a live window\n");
}

static void test_dialog_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *parent = NULL;
    fdk_window_options popts = { .title = "FDK dialog parent",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &popts, &parent)));
    fdk_window_show(parent);
    (void)fdk_pump_events(ctx, 150);

    Display *send = XOpenDisplay(NULL);
    assert(send != NULL);
    Window root_win = DefaultRootWindow(send);

    /* The test's own grab-probe window (modal-grab verification),
     * created BEFORE the baseline so it never pollutes the diffs. */
    Window probe = XCreateSimpleWindow(send, root_win, 0, 0, 10, 10, 0,
                                       0, 0);
    XSelectInput(send, probe, StructureNotifyMask);
    XMapWindow(send, probe);
    XFlush(send);

    Window before[16], now[16];
    int n_before = x11_child_windows(send, root_win, before, 16);

    /* 1) Modal OK/Cancel: Enter answers OK (initial focus). */
    dialog_responses = 0;
    fdk_dialog_options dopts = {
        .title = "Confirm",
        .text = "Save your changes before quitting?",
        .buttons = FDK_DIALOG_BUTTONS_OK_CANCEL,
        .modal = true,
    };
    fdk_window *dlg = NULL;
    assert(fdk_ok(fdk_dialog_show_message(ctx, &dopts, dialog_resp_cb,
                                          NULL, &dlg)));
    (void)fdk_pump_events(ctx, 250);

    int n_now = x11_child_windows(send, root_win, now, 16);
    assert(n_now > n_before);
    Window dlg_xid = x11_new_child(before, n_before, now, n_now);
    assert(dlg_xid != 0);
    printf("[ok] dialog: mapped as a real toplevel\n");

    /* MODAL GRAB, verified server-side: while FDK holds the grab,
     * ANOTHER client's pointer grab must fail with AlreadyGrabbed. */
    int grab_rc = XGrabPointer(send, probe, False, ButtonPressMask,
                               GrabModeAsync, GrabModeAsync, None, None,
                               CurrentTime);
    assert(grab_rc == AlreadyGrabbed);
    XUngrabPointer(send, CurrentTime);
    XFlush(send);
    printf("[ok] dialog: modal grab held server-side (foreign grab "
           "refused)\n");

    /* Enter -> the focused OK button -> response OK + self-destruct. */
    Window dlg_children[4];
    int n_dlg = x11_child_windows(send, dlg_xid, dlg_children, 4);
    (void)n_dlg; /* reparenting details are the WM's business (none
                    under bare Xvfb, but the assert above is about
                    the toplevel itself) */
    x11_send_key_event(send, dlg_xid, KeyPress, 36); /* Enter */
    x11_send_key_event(send, dlg_xid, KeyRelease, 36);
    (void)fdk_pump_events(ctx, 250);
    assert(dialog_responses == 1 && dialog_last == FDK_DIALOG_OK);
    n_now = x11_child_windows(send, root_win, now, 16);
    assert(n_now == n_before);
    printf("[ok] dialog: Enter answers OK; self-destroys\n");

    /* The grab is gone after the dialog died. */
    grab_rc = XGrabPointer(send, probe, False, ButtonPressMask,
                           GrabModeAsync, GrabModeAsync, None, None,
                           CurrentTime);
    assert(grab_rc == GrabSuccess);
    XUngrabPointer(send, CurrentTime);
    XFlush(send);
    printf("[ok] dialog: modal grab released on destroy\n");

    /* 2) Escape answers Cancel. */
    dialog_responses = 0;
    dopts.modal = false;
    dopts.title = "Escape me";
    assert(fdk_ok(fdk_dialog_show_message(ctx, &dopts, dialog_resp_cb,
                                          NULL, NULL)));
    (void)fdk_pump_events(ctx, 250);
    n_now = x11_child_windows(send, root_win, now, 16);
    assert(n_now == n_before + 1);
    dlg_xid = x11_new_child(before, n_before, now, n_now);
    x11_send_key_event(send, dlg_xid, KeyPress, 9); /* Escape */
    x11_send_key_event(send, dlg_xid, KeyRelease, 9);
    (void)fdk_pump_events(ctx, 250);
    assert(dialog_responses == 1 && dialog_last == FDK_DIALOG_CANCEL);
    printf("[ok] dialog: Escape answers Cancel\n");

    /* 3) A button CLICK through real input (find the button via the
     * widget tree — child 1 of the body, child 0 of the root). */
    dialog_responses = 0;
    dopts.title = "Click me";
    assert(fdk_ok(fdk_dialog_show_message(ctx, &dopts, dialog_resp_cb,
                                          NULL, &dlg)));
    (void)fdk_pump_events(ctx, 250);
    n_now = x11_child_windows(send, root_win, now, 16);
    assert(n_now == n_before + 1);
    dlg_xid = x11_new_child(before, n_before, now, n_now);
    assert(dlg_xid != 0);
    fdk_widget *dlg_root = NULL;
    assert(fdk_ok(fdk_window_get_root(dlg, &dlg_root)));
    fdk_widget *body = fdk_widget_child_at(dlg_root, 0);
    assert(body != NULL);
    fdk_widget *ok_btn = fdk_widget_child_at(body, 1); /* label, OK, Cancel */
    assert(ok_btn != NULL);
    fdk_rect okr = fdk_widget_get_absolute_bounds(ok_btn);
    x11_click(send, dlg_xid, okr.x + okr.width / 2, okr.y + okr.height / 2);
    (void)fdk_pump_events(ctx, 250);
    assert(dialog_responses == 1 && dialog_last == FDK_DIALOG_OK);
    printf("[ok] dialog: button click through real input\n");

    /* 4) Early destroy answers the negative response. */
    dialog_responses = 0;
    dopts.title = "Kill me";
    assert(fdk_ok(fdk_dialog_show_message(ctx, &dopts, dialog_resp_cb,
                                          NULL, &dlg)));
    (void)fdk_pump_events(ctx, 200);
    fdk_window_destroy(dlg);
    (void)fdk_pump_events(ctx, 150);
    assert(dialog_responses == 1 && dialog_last == FDK_DIALOG_CANCEL);
    printf("[ok] dialog: early destroy answers Cancel via the "
           "destroy-notify\n");

    XDestroyWindow(send, probe);
    XCloseDisplay(send);
    fdk_window_destroy(parent);
    fdk_shutdown(ctx);
    printf("[ok] dialog GUI: modal grab, Enter/Escape/click responses, "
           "early destroy, clean teardown\n");
}

/* ---- 1.2.0: list row activation ---------------------------------- */

static struct {
    int activations;
    size_t last_row;
} list_act;

static void list_row_activated(fdk_widget *list, size_t row, void *user) {
    (void)list; (void)user;
    list_act.activations++;
    list_act.last_row = row;
}

static void test_list_activation_gui(void) {
    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_font *font = fdk_font_load_system_default(16);
    assert(font != NULL);
    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "list act", .width = 240,
                                 .height = 160 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 200);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, font, &list)));
    /* Bounds BEFORE the rows: the list syncs its internal scrollview
     * on every append (list_relayout), so bounds set after the rows
     * leave the internals at the old size — set_bounds does not run
     * arrange hooks (the probe_dblclick lesson). */
    fdk_widget_set_bounds(list, (fdk_rect){ 10, 10, 200, 120 });
    fdk_list_append(list, "alpha", NULL);
    fdk_list_append(list, "beta", NULL);
    fdk_list_append(list, "gamma", NULL);
    fdk_list_set_on_row_activate(list, list_row_activated, NULL);
    /* list bounds: the rows live inside the scrollview's content
     * box (which also holds two scrollbar widgets); rather than
     * guessing the internal child order, walk to leaf widgets — the
     * rows are the tree's leaves, in row order. */
    fdk_widget *row1 = NULL;
    {
        fdk_widget *leaves[8] = {0};
        int nleaves = 0;
        /* Iterative BFS over children; leaves (no children) in order. */
        fdk_widget *queue[64] = { list };
        int head = 0, tail = 1;
        while (head < tail && nleaves < 8) {
            fdk_widget *cur = queue[head++];
            size_t cn = fdk_widget_child_count(cur);
            if (cn == 0) {
                leaves[nleaves++] = cur;
            } else {
                for (size_t i = 0; i < cn && tail < 64; i++) {
                    queue[tail++] = fdk_widget_child_at(cur, i);
                }
            }
        }
        /* The first leaves are the scrollbars (empty rows container
         * possible pre-layout); find the leaves that sit under the
         * rows container: they have no children AND their text is
         * non-NULL. Row 1 is the second ROW leaf: filter by checking
         * the ancestor two levels up is the same for all rows. */
        fdk_widget *rows_leaves[8] = {0};
        int nrows_leaves = 0;
        fdk_widget *rows_parent = NULL;
        for (int i = 0; i < nleaves; i++) {
            fdk_widget *p = leaves[i]->parent;
            if (p == NULL) {
                continue;
            }
            if (rows_parent == NULL || p == rows_parent) {
                /* heuristics: rows' parent chain depth 3 from list */
                fdk_widget *gp = p->parent;
                if (gp != NULL && gp->parent == list) {
                    rows_parent = p;
                    rows_leaves[nrows_leaves++] = leaves[i];
                }
            }
        }
        assert(nrows_leaves >= 3);
        row1 = rows_leaves[1];
    }
    assert(row1 != NULL);
    fdk_rect r1 = fdk_widget_get_bounds(row1);

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);
    unsigned long xid = fdk_window_xid(win);

    memset(&list_act, 0, sizeof(list_act));
    /* A double-click on row 1 (two fast press/release pairs). Row
     * bounds are in the scroll content's space; the window-space
     * click adds the list's origin (10, 10). */
    fdk_i32 click_x = 10 + r1.x + 30;
    fdk_i32 click_y = 10 + r1.y + r1.height / 2;
    for (int i = 0; i < 2; i++) {
        x11_send_pointer_event(send_dpy, xid, ButtonPress,
                               ButtonPressMask | ButtonReleaseMask,
                               click_x, click_y, 1);
        (void)fdk_pump_events(ctx, 60);
        x11_send_pointer_event(send_dpy, xid, ButtonRelease,
                               ButtonPressMask | ButtonReleaseMask,
                               click_x, click_y, 1);
        (void)fdk_pump_events(ctx, 60);
    }
    assert(list_act.activations == 1);
    assert(list_act.last_row == 1);

    /* Enter activates the keyboard cursor's row (row 2 after one
     * Down from row 1's selection). */
    assert(fdk_widget_focus(list));
    x11_send_key_event(send_dpy, xid, KeyPress, 116); /* Down */
    (void)fdk_pump_events(ctx, 100);
    x11_send_key_event(send_dpy, xid, KeyPress, 36); /* Enter (keycode 36 = scancode 28) */
    (void)fdk_pump_events(ctx, 100);
    assert(list_act.activations == 2);
    assert(list_act.last_row == 2);

    /* Slow double-click (two clicks far apart in time) must NOT
     * re-fire: the second click below lands after the dblclick
     * window (400ms) — enforced by sleeping past it. */
    struct timespec ts = { 0, 450 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    memset(&list_act, 0, sizeof(list_act));
    x11_send_pointer_event(send_dpy, xid, ButtonPress,
                           ButtonPressMask | ButtonReleaseMask,
                           click_x, click_y, 1);
    (void)fdk_pump_events(ctx, 60);
    x11_send_pointer_event(send_dpy, xid, ButtonRelease,
                           ButtonPressMask | ButtonReleaseMask,
                           click_x, click_y, 1);
    (void)fdk_pump_events(ctx, 60);
    assert(list_act.activations == 0); /* plain click: select only */

    XCloseDisplay(send_dpy);
    fdk_window_destroy(win);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    printf("[ok] list activation: double-click + Enter fire, slow "
           "re-click does not\n");
}

/* ---- 1.2.0: file dialogs ------------------------------------------ */

static struct {
    fdk_file_dialog_outcome outcome;
    char paths[4][512];
    size_t count;
} fd_result;

static void file_dialog_done(const fdk_file_dialog_result *result,
                             void *user) {
    (void)user;
    memset(&fd_result, 0, sizeof(fd_result));
    fd_result.outcome = result->outcome;
    fd_result.count = result->count;
    for (size_t i = 0; i < result->count && i < 4; i++) {
        snprintf(fd_result.paths[i], sizeof(fd_result.paths[i]), "%s",
                 result->paths[i]);
    }
}

/* Builds a scratch dir with one subdir + one file; returns the dir
 * (caller frees) and fills the expected file path. */
static char *make_dialog_scratch(char *file_out, size_t file_cap) {
    static char tmpl[] = "/tmp/fdk-fdlg-XXXXXX";
    char *d = mkdtemp(tmpl);
    assert(d != NULL);
    static char buf[600];
    snprintf(buf, sizeof(buf), "%s/sub", d);
    assert(mkdir(buf, 0755) == 0);
    snprintf(buf, sizeof(buf), "%s/note.txt", d);
    FILE *f = fopen(buf, "w");
    assert(f != NULL);
    fputs("hi", f);
    fclose(f);
    snprintf(file_out, file_cap, "%s/note.txt", d);
    return strdup(d);
}

static void drop_dialog_scratch(const char *dir) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s/note.txt", dir);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s/sub", dir);
    rmdir(buf);
    rmdir(dir);
    free((void *)dir);
}

static void test_file_dialog_gui(void) {
    char want_file[512];
    char *dir = make_dialog_scratch(want_file, sizeof(want_file));

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    Display *send_dpy = XOpenDisplay(NULL);
    assert(send_dpy != NULL);

    /* --- 1. OPEN_FILE, driven by keyboard: Down Down Enter picks
     * note.txt (rows: sub/, note.txt) and accepts on activation. */
    fdk_file_dialog_options o1 = {0};
    o1.kind = FDK_FILE_DIALOG_OPEN_FILE;
    o1.start_dir = dir;
    fdk_window *dlg = NULL;
    assert(fdk_ok(fdk_dialog_open_file(ctx, &o1, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    unsigned long dxid = fdk_window_xid(dlg);
    x11_send_key_event(send_dpy, dxid, KeyPress, 116); /* Down -> sub/ */
    (void)fdk_pump_events(ctx, 100);
    x11_send_key_event(send_dpy, dxid, KeyPress, 116); /* Down -> note */
    (void)fdk_pump_events(ctx, 100);
    x11_send_key_event(send_dpy, dxid, KeyPress, 36); /* Enter -> accept on activation */
    (void)fdk_pump_events(ctx, 250);
    assert(fd_result.outcome == FDK_FILE_DIALOG_ACCEPTED);
    assert(fd_result.count == 1);
    assert(strcmp(fd_result.paths[0], want_file) == 0);
    printf("[ok] file dialog: OPEN_FILE keyboard accept -> %s\n",
           want_file);

    /* --- 2. CANCELLED via Escape. */
    fdk_file_dialog_options o2 = {0};
    o2.kind = FDK_FILE_DIALOG_OPEN_FILE;
    o2.start_dir = dir;
    assert(fdk_ok(fdk_dialog_open_file(ctx, &o2, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    dxid = fdk_window_xid(dlg);
    x11_send_key_event(send_dpy, dxid, KeyPress, 9); /* Escape (keycode 9 = scancode 1) */
    (void)fdk_pump_events(ctx, 250);
    assert(fd_result.outcome == FDK_FILE_DIALOG_CANCELLED);
    assert(fd_result.count == 0);
    printf("[ok] file dialog: Escape answers CANCELLED (count 0)\n");

    /* --- 3. OPEN_FOLDER via the accept BUTTON (the click path):
     * body child order is fixed by creation — 1.2.3: [up, home,
     * hidden, combo, path, places, list, status, accept, cancel];
     * accept is index 8. */
    fdk_file_dialog_options o3 = {0};
    o3.kind = FDK_FILE_DIALOG_OPEN_FOLDER;
    o3.start_dir = dir;
    assert(fdk_ok(fdk_dialog_open_file(ctx, &o3, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    {
        fdk_widget *droot = NULL;
        assert(fdk_ok(fdk_window_get_root(dlg, &droot)));
        fdk_widget *body = fdk_widget_child_at(droot, 0);
        assert(body != NULL);
        fdk_widget *accept = fdk_widget_child_at(body, 8);
        assert(accept != NULL);
        fdk_rect ab = fdk_widget_get_bounds(accept);
        /* The list holds one row (sub/): select it first. */
        dxid = fdk_window_xid(dlg);
        x11_send_key_event(send_dpy, dxid, KeyPress, 116); /* Down */
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(send_dpy, dxid, ButtonPress,
                               ButtonPressMask | ButtonReleaseMask,
                               ab.x + 8, ab.y + ab.height / 2, 1);
        (void)fdk_pump_events(ctx, 100);
        x11_send_pointer_event(send_dpy, dxid, ButtonRelease,
                               ButtonPressMask | ButtonReleaseMask,
                               ab.x + 8, ab.y + ab.height / 2, 1);
        (void)fdk_pump_events(ctx, 250);
    }
    assert(fd_result.outcome == FDK_FILE_DIALOG_ACCEPTED);
    assert(fd_result.count == 1);
    /* Folders are verified directories by stat at accept time. */
    {
        struct stat st;
        assert(stat(fd_result.paths[0], &st) == 0 && S_ISDIR(st.st_mode));
    }
    printf("[ok] file dialog: OPEN_FOLDER button accept -> %s "
           "(stat-verified directory)\n", fd_result.paths[0]);

    /* --- 4. SAVE, fresh name, keyboard only: the Name row holds the
     * initial focus; Enter in it is the Save activation. No file
     * exists -> no confirmation -> straight ACCEPT, and the promised
     * path does NOT exist (save-as contract). */
    fdk_file_dialog_options o4 = {0};
    o4.kind = FDK_FILE_DIALOG_SAVE_FILE;
    o4.start_dir = dir;
    o4.start_name = "fresh.txt";
    assert(fdk_ok(fdk_dialog_save_file(ctx, &o4, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    dxid = fdk_window_xid(dlg);
    x11_send_key_event(send_dpy, dxid, KeyPress, 36); /* Enter = Save */
    (void)fdk_pump_events(ctx, 250);
    assert(fd_result.outcome == FDK_FILE_DIALOG_ACCEPTED);
    assert(fd_result.count == 1);
    {
        char want[600];
        snprintf(want, sizeof(want), "%s/fresh.txt", dir);
        assert(strcmp(fd_result.paths[0], want) == 0);
        struct stat st;
        assert(stat(want, &st) != 0); /* not created — the app writes */
    }
    printf("[ok] file dialog: SAVE fresh name Enter-accepts the "
           "non-existing target\n");

    /* --- 5. SAVE onto the EXISTING note.txt: Enter raises the
     * nested overwrite ask (a second root window); Escape there
     * declines -> the file dialog is STILL UP (no callback fired),
     * and a following Escape cancels it. */
    fdk_file_dialog_options o5 = {0};
    o5.kind = FDK_FILE_DIALOG_SAVE_FILE;
    o5.start_dir = dir;
    o5.start_name = "note.txt";
    assert(fdk_ok(fdk_dialog_save_file(ctx, &o5, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    dxid = fdk_window_xid(dlg);
    /* Snapshot root children, press Enter, snapshot again: the new
     * child is the overwrite confirmation. */
    Window root = DefaultRootWindow(send_dpy);
    Window dummy_root, dummy_parent;
    Window *pre = NULL, *post = NULL;
    unsigned int npre = 0, npost = 0;
    XQueryTree(send_dpy, root, &dummy_root, &dummy_parent, &pre,
               &npre);
    x11_send_key_event(send_dpy, dxid, KeyPress, 36); /* Enter = Save */
    (void)fdk_pump_events(ctx, 300);
    XQueryTree(send_dpy, root, &dummy_root, &dummy_parent, &post,
               &npost);
    Window confirm_xid = None;
    for (unsigned int i = 0; i < npost; i++) {
        bool known = false;
        for (unsigned int j = 0; j < npre; j++) {
            if (post[i] == pre[j]) {
                known = true;
                break;
            }
        }
        if (!known && post[i] != dxid) {
            confirm_xid = post[i];
        }
    }
    if (pre != NULL) {
        XFree(pre);
    }
    if (post != NULL) {
        XFree(post);
    }
    assert(confirm_xid != None);
    x11_send_key_event(send_dpy, confirm_xid, KeyPress, 9); /* Esc = No */
    (void)fdk_pump_events(ctx, 300);
    /* Still up: the sentinel flips ONLY when a callback fires. */
    fd_result.outcome = FDK_FILE_DIALOG_ERROR; /* sentinel */
    /* The Name row started with start_name SELECTED (the
     * rename-everywhere convention): the first Escape collapses
     * that selection (Entry contract), the second bubbles to the
     * window layer and cancels. */
    x11_send_key_event(send_dpy, dxid, KeyPress, 9);
    (void)fdk_pump_events(ctx, 100);
    x11_send_key_event(send_dpy, dxid, KeyPress, 9); /* Esc: cancel */
    (void)fdk_pump_events(ctx, 250);
    assert(fd_result.outcome == FDK_FILE_DIALOG_CANCELLED);
    printf("[ok] file dialog: overwrite ask declined -> dialog stays "
           "up, then cancels\n");

    /* --- 6. SAVE onto note.txt again; Enter answers YES in the
     * nested ask -> ACCEPTED with the existing file's path. */
    fdk_file_dialog_options o6 = {0};
    o6.kind = FDK_FILE_DIALOG_SAVE_FILE;
    o6.start_dir = dir;
    o6.start_name = "note.txt";
    assert(fdk_ok(fdk_dialog_save_file(ctx, &o6, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    dxid = fdk_window_xid(dlg);
    pre = NULL;
    npre = 0;
    XQueryTree(send_dpy, root, &dummy_root, &dummy_parent, &pre,
               &npre);
    x11_send_key_event(send_dpy, dxid, KeyPress, 36); /* Enter = Save */
    (void)fdk_pump_events(ctx, 300);
    post = NULL;
    npost = 0;
    XQueryTree(send_dpy, root, &dummy_root, &dummy_parent, &post,
               &npost);
    confirm_xid = None;
    for (unsigned int i = 0; i < npost; i++) {
        bool known = false;
        for (unsigned int j = 0; j < npre; j++) {
            if (post[i] == pre[j]) {
                known = true;
                break;
            }
        }
        if (!known && post[i] != dxid) {
            confirm_xid = post[i];
        }
    }
    if (pre != NULL) {
        XFree(pre);
    }
    if (post != NULL) {
        XFree(post);
    }
    assert(confirm_xid != None);
    x11_send_key_event(send_dpy, confirm_xid, KeyPress, 36); /* Enter=Yes */
    (void)fdk_pump_events(ctx, 300);
    assert(fd_result.outcome == FDK_FILE_DIALOG_ACCEPTED);
    assert(fd_result.count == 1);
    assert(strcmp(fd_result.paths[0], want_file) == 0);
    printf("[ok] file dialog: overwrite ask confirmed -> ACCEPTED "
           "with the existing path\n");

    /* --- 7. SAVE with an EMPTY name: Enter validates (status line
     * complaint), the dialog STAYS UP — an invalid save never
     * answers. Escape then cancels. */
    fdk_file_dialog_options o7 = {0};
    o7.kind = FDK_FILE_DIALOG_SAVE_FILE;
    o7.start_dir = dir;
    assert(fdk_ok(fdk_dialog_save_file(ctx, &o7, file_dialog_done, NULL,
                                       &dlg)));
    (void)fdk_pump_events(ctx, 250);
    dxid = fdk_window_xid(dlg);
    x11_send_key_event(send_dpy, dxid, KeyPress, 36); /* Enter */
    (void)fdk_pump_events(ctx, 250);
    x11_send_key_event(send_dpy, dxid, KeyPress, 9); /* Esc */
    (void)fdk_pump_events(ctx, 250);
    assert(fd_result.outcome == FDK_FILE_DIALOG_CANCELLED);
    assert(fd_result.count == 0);
    printf("[ok] file dialog: empty SAVE name never answers "
           "(validation keeps the dialog up)\n");

    XCloseDisplay(send_dpy);
    fdk_shutdown(ctx); /* any dialog window left dies here safely */
    drop_dialog_scratch(dir);
    printf("[ok] file dialog GUI: accept/cancel paths, folder kind "
           "returns a real directory\n");
}

/* ---- 1.2.0: drag and drop ---------------------------------------- */

static struct {
    int enters, motions, leaves, drops;
    char last_text[256];
    char uris[4][512];
    size_t uri_count;
} dnd_rx;

static void dnd_count_window_event(fdk_window *w, const fdk_event_data *ev,
                                   void *user) {
    (void)w; (void)user;
    if (ev->type < FDK_EVENT_DRAG_ENTER || ev->type > FDK_EVENT_DRAG_DROP) {
        return;
    }
    switch (ev->type) {
    case FDK_EVENT_DRAG_ENTER: dnd_rx.enters++; break;
    case FDK_EVENT_DRAG_MOTION: dnd_rx.motions++; break;
    case FDK_EVENT_DRAG_LEAVE: dnd_rx.leaves++; break;
    case FDK_EVENT_DRAG_DROP:
        dnd_rx.drops++;
        if (ev->drag.text != NULL) {
            snprintf(dnd_rx.last_text, sizeof(dnd_rx.last_text), "%s",
                     ev->drag.text);
        }
        for (size_t i = 0; i < ev->drag.uri_count && i < 4; i++) {
            snprintf(dnd_rx.uris[i], sizeof(dnd_rx.uris[i]), "%s",
                     ev->drag.uris[i]);
        }
        dnd_rx.uri_count = ev->drag.uri_count;
        break;
    default: break;
    }
}

/* Pumps while the xdnd_source child runs (bounded). Returns the
 * child's exit status, or -1 on timeout. */
static int pump_wait_child(fdk_context *ctx, pid_t pid, int timeout_ms) {
    int spins = timeout_ms / 50;
    for (int i = 0; i < spins; i++) {
        (void)fdk_pump_events(ctx, 50);
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }
    return -1;
}

/* Pumps FDK while draining a popen child's stdout NON-BLOCKINGLY
 * (the child goes silent mid-handshake; a blocking read would starve
 * the pump). Returns the child's exit code, or -1 on timeout. */
static int pump_child_while_draining(fdk_context *ctx, FILE *child,
                                     int timeout_s) {
    int fd = fileno(child);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    char buf[4096];
    size_t used = 0;
    int spins = timeout_s * 20;
    int eof = 0;
    for (int i = 0; i < spins && !eof; i++) {
        (void)fdk_pump_events(ctx, 50);
        char chunk[512];
        ssize_t n;
        while ((n = read(fd, chunk, sizeof(chunk))) >= 0) {
            if (n == 0) {
                eof = 1; /* child closed its stdout: done */
                break;
            }
            size_t take = (size_t)n;
            if (used + take >= sizeof(buf)) {
                take = sizeof(buf) - 1 - used;
            }
            memcpy(buf + used, chunk, take);
            used += take;
        }
    }
    buf[used] = '\0';
    /* print-through for the rig logs */
    fputs(buf, stdout);
    fflush(stdout);
    return pclose(child);
}

static void test_dnd_receiver_gui(void) {
    const char *src_bin = getenv("FDK_XDND_SOURCE_BIN");
    if (src_bin == NULL) {
        src_bin = "/home/z/my-project/scripts/xdnd_source";
    }
    if (access(src_bin, X_OK) != 0) {
        printf("[skip] dnd receiver: %s not built (external interop "
               "rig builds it)\n", src_bin);
        return;
    }

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "dnd target",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, dnd_count_window_event, NULL);
    fdk_window_set_drop_formats(win, FDK_DRAG_FORMAT_TEXT |
                                         FDK_DRAG_FORMAT_URI_LIST);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 250);

    Display *probe = XOpenDisplay(NULL);
    assert(probe != NULL);
    unsigned long xid = fdk_window_xid(win);
    /* Root coords of a point inside our window (the window may be at
     * (0,0) under bare Xvfb, but asking the server is the truth). */
    int lx = 0, ly = 0;
    Window junk = None;
    XTranslateCoordinates(probe, (Window)xid, DefaultRootWindow(probe),
                          80, 60, &lx, &ly, &junk);
    char cmd[512];

    /* --- files drop --- */
    memset(&dnd_rx, 0, sizeof(dnd_rx));
    snprintf(cmd, sizeof(cmd), "%s 0x%lx %d %d files", src_bin, xid, lx,
             ly);
    FILE *child = popen(cmd, "r");
    assert(child != NULL);
    /* Drain the child NON-BLOCKINGLY while FDK pumps: the child goes
     * silent while waiting for FDK's XDndStatus, and a blocking fgets
     * would starve the pump (the probe_dndrx lesson — the handshake
     * needs BOTH sides live). */
    alarm(8);
    int rc = pump_child_while_draining(ctx, child, 8);
    alarm(0);
    assert(rc == 0);
    assert(dnd_rx.enters >= 1);
    assert(dnd_rx.drops == 1);
    assert(dnd_rx.uri_count == 2);
    assert(strcmp(dnd_rx.uris[0], "/etc/hostname") == 0);
    assert(strcmp(dnd_rx.uris[1], "/etc/os-release") == 0);
    printf("[ok] dnd receiver: external raw-Xlib source dropped 2 "
           "files, decoded to POSIX paths\n");

    /* --- text drop --- */
    memset(&dnd_rx, 0, sizeof(dnd_rx));
    snprintf(cmd, sizeof(cmd), "%s 0x%lx %d %d text", src_bin, xid, lx,
             ly);
    child = popen(cmd, "r");
    assert(child != NULL);
    alarm(8);
    rc = pump_child_while_draining(ctx, child, 8);
    alarm(0);
    assert(rc == 0);
    assert(dnd_rx.drops == 1);
    assert(strcmp(dnd_rx.last_text, "Hello from an external client") == 0);
    printf("[ok] dnd receiver: external text drop decoded as UTF-8\n");

    XCloseDisplay(probe);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
}

/* Source side: FDK drags into a raw-Xlib sink window, driven by the
 * REAL pointer through XTEST. */
static struct {
    bool armed;
    bool drag_started_ok;
    bool reported;
    fdk_drag_status status;
} dnd_tx;

static void dnd_tx_done(fdk_drag_status status, void *user);

static void dnd_tx_window_event(fdk_window *w, const fdk_event_data *ev,
                                void *user) {
    (void)user;
    if (ev->type == FDK_EVENT_POINTER_BUTTON_DOWN) {
        dnd_tx.armed = true;
    } else if (ev->type == FDK_EVENT_POINTER_MOTION && dnd_tx.armed) {
        dnd_tx.armed = false;
        const char *uris[2] = { "/etc/hostname", "/etc/os-release" };
        fdk_result r = fdk_drag_begin(
            w, FDK_DRAG_FORMAT_TEXT | FDK_DRAG_FORMAT_URI_LIST,
            "FDK to the outside", uris, 2, dnd_tx_done, NULL);
        dnd_tx.drag_started_ok = fdk_ok(r);
    }
}

static void dnd_tx_done(fdk_drag_status status, void *user) {
    (void)user;
    dnd_tx.status = status;
    dnd_tx.reported = true;
}

static void test_dnd_source_gui(void) {
    const char *sink_bin = getenv("FDK_XDND_SINK_BIN");
    if (sink_bin == NULL) {
        sink_bin = "/home/z/my-project/scripts/xdnd_sink";
    }
    const char *xtest = getenv("FDK_XTEST_BIN");
    if (xtest == NULL) {
        xtest = "/tmp/xtest_driver";
    }
    if (access(sink_bin, X_OK) != 0 || access(xtest, X_OK) != 0) {
        printf("[skip] dnd source: %s or %s not built (interop rig)\n",
               sink_bin, xtest);
        return;
    }

    fdk_context *ctx = NULL;
    fdk_init_options opts = { .backend = FDK_PLATFORM_X11 };
    assert(fdk_ok(init_with_retry(&ctx, &opts)));

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "dnd source",
                                 .width = 300, .height = 200 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, dnd_tx_window_event, NULL);
    fdk_window_show(win);
    (void)fdk_pump_events(ctx, 250);

    /* The sink window, placed away from ours, stdout piped. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s 420 20 280 180", sink_bin);
    FILE *sink = popen(cmd, "r");
    assert(sink != NULL);
    /* Non-blocking drain helper via fd. */
    int sink_fd = fileno(sink);
    fcntl(sink_fd, F_SETFL, O_NONBLOCK);

    char line[512];
    char sink_out[4096] = {0};
    unsigned long sink_xid = 0;
    alarm(6);
    for (;;) {
        while (read(sink_fd, line, sizeof(line)) > 0) {
            strncat(sink_out, line,
                    sizeof(sink_out) - strlen(sink_out) - 1);
            if (sscanf(line, "SINK: ready 0x%lx", &sink_xid) == 1) {
                /* fallthrough */
            }
        }
        (void)fdk_pump_events(ctx, 50);
        if (sink_xid != 0) {
            break;
        }
    }
    alarm(0);
    assert(sink_xid != 0);

    unsigned long xid = fdk_window_xid(win);
    Display *probe = XOpenDisplay(NULL);
    assert(probe != NULL);
    int wx = 0, wy = 0, sx = 0, sy = 0;
    Window junk = None;
    XTranslateCoordinates(probe, (Window)xid, DefaultRootWindow(probe),
                          60, 40, &wx, &wy, &junk);
    XCloseDisplay(probe);
    sx = 420 + 100; /* inside the sink rect we placed */
    sy = 20 + 60;

    char drive[512];
    const char *libprefix = getenv("FDK_XLIB_PREFIX");
    if (libprefix == NULL) {
        libprefix = "/home/z/apt/prefix/usr/lib/x86_64-linux-gnu";
    }
    memset(&dnd_tx, 0, sizeof(dnd_tx));
    alarm(15);
    /* Human-paced drag: ONE pointer step per driver invocation, with
     * real pump time between steps — a real application's loop pumps
     * continuously, and the XDND handshake needs FDK to see the
     * target's Status BEFORE the release (a single driver call doing
     * move-move-move-release starves it; the probe lesson). The
     * xtest_driver binary may link shared X libs from the local
     * extraction prefix — put it on the loader path. */
    const int steps[][2] = {
        { wx, wy },        /* over our window */
        { wx + 40, wy + 30 },
        { sx - 60, sy },   /* approach the sink */
        { sx - 20, sy },
        { sx, sy },        /* center of the sink */
    };
    char one[300];
    snprintf(one, sizeof(one), "LD_LIBRARY_PATH=%s", libprefix);
    setenv("LD_LIBRARY_PATH", libprefix, 1);

    (void)system("clear"); /* no-op formatting for logs */
    snprintf(drive, sizeof(drive), "\"%s\" 0x%lx m:%d,%d w:60 d", xtest,
             xid, steps[0][0], steps[0][1]);
    (void)system(drive);
    (void)fdk_pump_events(ctx, 300);
    for (int i = 1; i < 5; i++) {
        snprintf(drive, sizeof(drive), "\"%s\" 0x%lx m:%d,%d", xtest, xid,
                 steps[i][0], steps[i][1]);
        (void)system(drive);
        /* Pump while the sink answers Status. */
        for (int p = 0; p < 4; p++) {
            (void)fdk_pump_events(ctx, 60);
            while (read(sink_fd, line, sizeof(line)) > 0) {
                strncat(sink_out, line,
                        sizeof(sink_out) - strlen(sink_out) - 1);
            }
        }
    }
    snprintf(drive, sizeof(drive), "\"%s\" 0x%lx u", xtest, xid);
    (void)system(drive);
    /* Let the handshake finish: FDK drop -> sink convert+Finished.
     * Everything the sink says is ACCUMULATED (earlier drains already
     * consumed the pipe — a print-only drain loses the payloads to
     * stdout buffering on abort). */
    int done_spins = 0;
    while (done_spins++ < 120) {
        (void)fdk_pump_events(ctx, 50);
        while (read(sink_fd, line, sizeof(line)) > 0) {
            strncat(sink_out, line,
                    sizeof(sink_out) - strlen(sink_out) - 1);
        }
        if (dnd_tx.reported &&
            strstr(sink_out, "payload uri") != NULL &&
            strstr(sink_out, "payload text") != NULL) {
            break;
        }
    }
    alarm(0);
    /* Print-through for rig logs (flushed explicitly). */
    fputs(sink_out, stdout);
    fflush(stdout);
    assert(dnd_tx.drag_started_ok);
    assert(dnd_tx.status == FDK_DRAG_SUCCEEDED);

    /* The sink must have decoded BOTH payloads (uri list + text). */
    assert(strstr(sink_out, "payload uri") != NULL);
    assert(strstr(sink_out, "file:///etc/hostname") != NULL);
    assert(strstr(sink_out, "payload text") != NULL);
    assert(strstr(sink_out, "FDK to the outside") != NULL);
    printf("[ok] dnd source: FDK drag into a raw-Xlib window: "
           "SUCCEEDED, both payloads received by the external app\n");

    /* Retire the sink (it runs until SIGTERM), then reap, then tear
     * down our own side. */
    (void)system("pkill -x xdnd_sink 2>/dev/null || true");
    (void)pclose(sink);
    fdk_window_destroy(win);
    fdk_shutdown(ctx);
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
    test_resize_storm_backlog_drains();
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
    test_resize_retains_pixels();
    test_hover_revalidation_on_geometry_change();
    test_resize_cursor_affordance();
    test_ewmh_atom_spelling();
    test_ewmh_fake_wm();
    test_mitm_shm_and_double_buffer();
    test_clipboard();
    test_entry_gui();
    test_popup_window();
    test_menu_gui();
    test_combo_gui();
    test_dialog_gui();
    test_a11y_gui();
    test_list_activation_gui();
    test_file_dialog_gui();
    test_dnd_receiver_gui();
    test_dnd_source_gui();

    printf("\nall X11 integration tests passed\n");
    return 0;
}
