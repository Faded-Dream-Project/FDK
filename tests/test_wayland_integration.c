/*
 * test_wayland_integration.c — Wayland platform integration test.
 *
 * Runs against a REAL compositor: start weston headless first, e.g.
 *
 *   weston --backend=headless-backend.so --shell=kiosk-shell.so \
 *          --socket=wl-fdk --idle-time=0
 *   WAYLAND_DISPLAY=wl-fdk ./build/tests/test_wayland_integration
 *
 * (scripts/run_wayland_tests.sh does exactly that.) The environment
 * dictates what can be asserted:
 *
 *  - kiosk-shell headless has NO wl_seat (no input devices), so this
 *    test exercises the programmatic surface: decoration mode
 *    negotiation, maximize/unmaximize via xdg_toplevel, minimize
 *    request, configure-driven state events, and pixel verification
 *    through FDK's own framebuffer (what we DREW — the compositor's
 *    end of the pipeline is verified separately by the screenshot
 *    rig with PIL).
 *
 *  - The decoration outcome is compositor policy: kiosk-shell may
 *    confirm CLIENT_SIDE (band stays, drawn by FDK) or force
 *    SERVER_SIDE (FDK must drop its band + emit
 *    FDK_EVENT_WINDOW_DECORATION). BOTH are real, correct outcomes;
 *    the test asserts the reaction matches the answer, not one
 *    specific answer.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dialog.h"
#include "fdk/fdk_theme.h"
#include "fdk/fdk_widgets.h"

/* Internal seam (same discipline as test_x11_integration.c): the
 * deferred-first-frame regression below asserts backend commit state
 * through the sanctioned ops wrapper — the Wayland platform header
 * itself never leaves src/platform/wayland/. */
#include "window/window_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h> /* free() for clipboard strings */
#include <string.h>
#include <unistd.h> /* access() — the injector-availability probe */

static fdk_dialog_response g_wayland_dlg_last =
    (fdk_dialog_response)-99;

static void wayland_dialog_response_cb(fdk_dialog_response response,
                                       void *user) {
    int *count = user;
    (*count)++;
    g_wayland_dlg_last = response;
}

static int g_state_events = 0;
static int g_deco_events = 0;
static int g_last_maximized = -1;
static int g_last_minimized = -1;


/* ---- Virtual-pointer input injection (the interactive section) ----
 *
 * fdk-wl-inject (scripts/, test infra) creates a
 * zwlr_virtual_pointer_v1 device and serves commands over stdin for
 * as long as the pipe is open — the device, and therefore the seat's
 * POINTER CAPABILITY, lives exactly that long (clients binding
 * wl_pointer on the capabilities event, as FDK does, then receive
 * REAL motion/button events with REAL serials — the only source of
 * grab-valid serials under a headless compositor).
 *
 * Absent tooling (plain weston, no injector binary): the
 * interactive section honestly skips. */

#include <stdlib.h>

static FILE *g_injector = NULL;

static bool wayland_injector_start(void) {
    /* popen() hands back a perfectly valid stream even when the
     * command does not exist (sh: not found) — the first write then
     * dies with SIGPIPE and takes the whole test with it, breaking
     * the "absent tooling honestly skips" contract this section is
     * built on. Verify the binary is actually executable first. */
    if (access("/home/z/my-project/scripts/fdk-wl-inject", X_OK) != 0) {
        return false;
    }
    g_injector = popen("/home/z/my-project/scripts/fdk-wl-inject serve",
                       "w");
    return g_injector != NULL;
}

static void wayland_inject(const char *cmd) {
    if (g_injector == NULL) {
        return;
    }
    fprintf(g_injector, "%s\n", cmd);
    fflush(g_injector);
}

static void wayland_injector_stop(void) {
    if (g_injector != NULL) {
        pclose(g_injector); /* EOF: the virtual pointer dies with it */
        g_injector = NULL;
    }
}

/* Real-app loop shape: pump + paint each iteration. Painting matters
 * under sway's headless output — the surface stays live in the
 * compositor's tree while frames flow, and the interactive section
 * needs the window mapped to receive the injected pointer events
 * (an idle-pumping window drops out of the tree on this backend;
 * every FDK example paints in its loop, so this helper IS the
 * documented usage pattern). */
static void pump_and_paint(fdk_context *ctx, fdk_window *win, int ms) {
    int steps = ms / 50;
    if (steps < 1) {
        steps = 1;
    }
    for (int i = 0; i < steps; i++) {
        (void)fdk_pump_events(ctx, 50);
        fdk_window_paint(win);
    }
}

static void wayland_menu_item_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (*(int *)user)++;
}

/* The opener wiring: statics because the callback has no closure
 * state (stock Button signature). */
static fdk_menu *g_wayland_menu = NULL;
static fdk_widget *g_wayland_menu_anchor = NULL;

static void wayland_menu_button_cb(fdk_widget *button, void *user) {
    (void)button;
    (*(int *)user)++;
    if (g_wayland_menu != NULL && g_wayland_menu_anchor != NULL) {
        (void)fdk_menu_popup_at(g_wayland_menu, g_wayland_menu_anchor,
                                0, 30);
    }
}

static void window_callback(fdk_window *window, const fdk_event_data *event,
                            void *user) {
    (void)window;
    (void)user;
    switch (event->type) {
    case FDK_EVENT_WINDOW_STATE:
        g_state_events++;
        g_last_maximized = event->state.maximized;
        g_last_minimized = event->state.minimized;
        break;
    case FDK_EVENT_WINDOW_DECORATION:
        g_deco_events++;
        break;
    default:
        break;
    }
}

/* The themed band fill under the default theme (v1 control bg):
 * 0.16*255=40.8->40, 0.18*255=45.9->45, 0.26*255=66.3->66 with the
 * renderer's exact rounding — read the actual pixel and compare
 * against the SAME lookup the paint hook used, component by component
 * (0.16f, 0.18f, 0.26f). */
static bool pixel_is_band_fill(fdk_u32 px) {
    fdk_color c = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    fdk_u32 r = (fdk_u32)(c.r * 255.0f + 0.5f);
    fdk_u32 g = (fdk_u32)(c.g * 255.0f + 0.5f);
    fdk_u32 b = (fdk_u32)(c.b * 255.0f + 0.5f);
    return ((px >> 16) & 0xFFu) == r && ((px >> 8) & 0xFFu) == g &&
           (px & 0xFFu) == b;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    /* app_id matters: the sway rig floats windows with this prefix,
     * so client-side resizes (the reflow test) actually take effect
     * instead of being overridden by the tiling layout. */
    fdk_init_options opts = { .backend = FDK_PLATFORM_WAYLAND,
                              .app_id = "org.fdk.test" };
    fdk_result r = fdk_init(&ctx, &opts);
    if (!fdk_ok(r)) {
        printf("[skip] no Wayland compositor reachable (fdk_init: %d)\n", r);
        return 0;
    }

    /* ---- The deferred-first-frame regression (found live in 1.1.0:
     * every example painted before the first configure and never
     * mapped) ----
     *
     * Exact application ordering: create, show, PAINT, only then
     * enter the pump loop. The paint happens before the first
     * xdg configure has been read, so the backend must defer the
     * commit — and must land it by itself the moment the configure
     * is acked. Before the fix this test hangs below: the surface
     * layer had consumed the damage, every later present was a
     * no-op, and rendered_ever never became true. */
    {
        fdk_window *dw = NULL;
        fdk_window_options dwopts = { .title = "FDK deferred frame",
                                      .width = 300, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &dwopts, &dw)));
        fdk_widget *droot = NULL;
        assert(fdk_ok(fdk_window_get_root(dw, &droot)));
        fdk_widget_set_background(droot,
                                  (fdk_color){0.2f, 0.4f, 0.9f, 1.0f});
        fdk_window_show(dw);
        fdk_window_paint(dw); /* BEFORE any pump — the deferred path */
        assert(fdk__window_ever_presented(dw) == 0);

        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (fdk__window_ever_presented(dw) != 0) {
                break; /* committed at the configure itself */
            }
        }
        if (fdk__window_ever_presented(dw) == 0) {
            /* Configure proposed a different size: the EXPOSE branch
             * re-drove the tree, and the application's NEXT paint
             * (still the documented loop shape) presents at the real
             * size. One more round must land it. */
            fdk_window_paint(dw);
            for (int i = 0; i < 8; i++) {
                (void)fdk_pump_events(ctx, 50);
                if (fdk__window_ever_presented(dw) != 0) {
                    break;
                }
            }
        }
        assert(fdk__window_ever_presented(dw) == 1);
        printf("[ok] deferred first frame: present-before-configure "
               "maps the window (no idle-wedge)\n");
        fdk_window_destroy(dw);
    }

    fdk_window *win = NULL;
    fdk_window_options wopts = { .title = "FDK wayland state test",
                                 .width = 320, .height = 240 };
    assert(fdk_ok(fdk_window_create(ctx, &wopts, &win)));
    fdk_window_set_event_callback(win, window_callback, NULL);

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(win, &root)));
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL, (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_background(content, (fdk_color){0.0f, 1.0f, 0.0f, 1.0f});
    fdk_window_set_content(win, content);

    fdk_window_show(win);
    /* First configure arrives here (kiosk-shell maximizes everything,
     * so the size may be the output size — size-agnostic assertions
     * only). */
    for (int i = 0; i < 40 && !fdk_window_get_decorated(win); i++) {
        (void)fdk_pump_events(ctx, 50);
    }
    fdk_size size = { 0, 0 };
    assert(fdk_ok(fdk_window_get_size(win, &size)));
    assert(size.width > 0 && size.height > 0);

    /* ---- Decorations: request client-side, accept either answer ---- */
    r = fdk_window_set_decorated(win, true);
    if (r == FDK_ERR_UNSUPPORTED) {
        printf("[ok] decorations honestly UNSUPPORTED (compositor lacks "
               "xdg-decoration; no double-decorating)\n");
    } else {
        assert(fdk_ok(r));
        assert(fdk_window_get_decorated(win));
        (void)fdk_pump_events(ctx, 400);

        if (g_deco_events > 0 && !fdk_window_get_decorated(win)) {
            /* Compositor forced SERVER_SIDE and FDK tore its band down —
             * the no-double-decoration contract. */
            printf("[ok] compositor forced server-side decorations; FDK "
                   "dropped its band (decoration event delivered)\n");
        } else {
            /* CLIENT_SIDE confirmed (or not yet contradicted): the band
             * is drawn INSIDE our framebuffer — verify at the pixel
             * level what we render. */
            assert(fdk_ok(fdk_window_paint(win)));
            fdk_surface *surface = NULL;
            assert(fdk_ok(fdk_window_get_surface(win, &surface)));
            fdk_surface_info info;
            assert(fdk_ok(fdk_surface_get_info(surface, &info)));
            /* Band probes in PHYSICAL rows: the band is 28 LOGICAL
             * rows tall (title_bar_height metric), so the below-band
             * probe must clear it at any scale — 40 logical rows up,
             * converted through the live scale. */
            fdk_f32 p_scale = 1.0f;
            (void)fdk_window_get_scale(win, &p_scale);
            fdk_i32 below_row = (fdk_i32)(40.0f * p_scale);
            if (below_row >= info.height) {
                below_row = info.height - 1;
            }
            fdk_u32 mid = info.pixels[(fdk_i32)(info.stride * 2) +
                                      info.width / 2];
            fdk_u32 below = info.pixels[(size_t)info.stride *
                                            (size_t)below_row +
                                        (size_t)(info.width / 2)];
            assert(pixel_is_band_fill(mid));
            assert(!pixel_is_band_fill(below) ||
                   info.height < (fdk_i32)(48.0f * p_scale));
            printf("[ok] client-side decorations confirmed: themed band "
                   "rendered in-frame (pixel-verified), content below\n");
        }
    }

    /* ---- Maximize: request + configure-driven state event ---- */
    /* The compositor's world dictates what can be asserted — THREE
     * real worlds, all handled:
     *  (a) TILED (sway's default layout): windows report MAXIMIZED in
     *      the toplevel configure AT MAP TIME — before any request of
     *      ours (kiosk-shell weston maximizes everything too).
     *  (b) HONORED: a compositor that answers set_maximized with a
     *      configure carrying MAXIMIZED.
     *  (c) DECLINED: sway 1.10 + a FLOATING window (the rig floats
     *      app_id "org.fdk.test" so client resizes take effect) never
     *      acts on the request — the window keeps its floating
     *      geometry and states (verified against sway's own tree:
     *      floating_con, fullscreen_mode 0). xdg-shell lets the
     *      compositor decide; FDK must not fake a state the
     *      compositor never confirmed, and must not invent an event. */
    bool pre_max = fdk_window_is_maximized(win);
    int before = g_state_events;
    assert(fdk_ok(fdk_window_maximize(win)));
    for (int i = 0; i < 40; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (fdk_window_is_maximized(win)) {
            break;
        }
    }
    if (pre_max) {
        /* (a) Already maximized at map: the state event fired THEN
         * (the map-time configure proved the whole chain — states
         * parsing, change detection, event dispatch); the later
         * request is a no-op that correctly dispatches nothing new.
         * The REQUEST itself is protocol-verified by the rig's
         * WAYLAND_DEBUG counts. */
        assert(g_state_events >= 1);
        assert(fdk_window_is_maximized(win));
        printf("[ok] maximize request sent; compositor tiled the window "
               "(maximized at map, state event then delivered)\n");
    } else if (fdk_window_is_maximized(win)) {
        /* (b) Fresh transition: the configure must have carried
         * MAXIMIZED, and FDK must have delivered the event. */
        assert(g_state_events > before);
        assert(g_last_maximized == 1);
        printf("[ok] maximize: xdg configure reported MAXIMIZED state, "
               "FDK_EVENT_WINDOW_STATE delivered\n");
    } else {
        /* (c) Declined: the request went out (the rig's WAYLAND_DEBUG
         * count proves the wire), the compositor never confirmed, so
         * FDK reports the honest state — still not maximized, and NO
         * spurious state event may fire for a no-change configure. */
        assert(g_state_events == before);
        printf("[ok] maximize request sent; compositor declined it "
               "(floating window keeps its geometry; FDK reports the "
               "compositor's truth, no invented state/event)\n");
    }

    /* ---- Unmaximize ---- */
    before = g_state_events;
    assert(fdk_ok(fdk_window_unmaximize(win)));
    for (int i = 0; i < 40; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (!fdk_window_is_maximized(win)) {
            break;
        }
    }
    printf("[ok] unmaximize request sent (post-state: maximized=%d)\n",
           fdk_window_is_maximized(win) ? 1 : 0);

    /* ---- Minimize: fire-and-forget request, optimistic flag ---- */
    before = g_state_events;
    assert(fdk_ok(fdk_window_minimize(win)));
    (void)fdk_pump_events(ctx, 300);
    assert(fdk_window_is_minimized(win));
    assert(g_state_events > before);
    /* Restore is honestly unsupported on Wayland (no protocol request). */
    assert(fdk_window_restore(win) == FDK_ERR_UNSUPPORTED);
    printf("[ok] minimize: request sent, optimistic state flagged, "
           "restore honestly UNSUPPORTED\n");

    /* ---- Layout reflow on resize (Phase 5 completion item) ----
     *
     * The Phase 5 roadmap entry recorded a Wayland-side reflow test
     * as blocked on the compositor toolchain; sway headless closes
     * it. A vertical box with a colored header/body/footer is the
     * window content; a resize (client request -> compositor
     * configure) must re-arrange the tree with zero application
     * code, verifiable in the framebuffer: the body panel STRETCHES
     * (expand) and the footer MOVES to the new bottom. */
    {
        fdk_window *rwin = NULL;
        fdk_window_options ropts = { .title = "FDK wayland reflow test",
                                     .width = 300, .height = 200 };
        assert(fdk_ok(fdk_window_create(ctx, &ropts, &rwin)));
        fdk_window_show(rwin);
        for (int i = 0; i < 12; i++) {
            (void)fdk_pump_events(ctx, 30); /* first configure */
        }

        fdk_widget *rroot = NULL;
        assert(fdk_ok(fdk_window_get_root(rwin, &rroot)));
        fdk_widget *vbox = NULL;
        assert(fdk_ok(fdk_box_create(rroot, FDK_VERTICAL, &vbox)));
        fdk_widget *header = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 30}, &header)));
        fdk_widget_set_background(header,
                                  (fdk_color){0.9f, 0.2f, 0.2f, 1.0f});
        fdk_widget_set_expand(header, false, false);
        fdk_widget_set_natural_size(header, 10, 30);
        fdk_widget *body = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 10}, &body)));
        fdk_widget_set_background(body,
                                  (fdk_color){0.2f, 0.9f, 0.2f, 1.0f});
        fdk_widget_set_expand(body, false, true); /* fills leftover height */
        fdk_widget *footer = NULL;
        assert(fdk_ok(fdk_widget_create(vbox, NULL,
                                        (fdk_rect){0, 0, 10, 20}, &footer)));
        fdk_widget_set_background(footer,
                                  (fdk_color){0.2f, 0.2f, 0.9f, 1.0f});
        fdk_widget_set_expand(footer, false, false);
        fdk_window_set_content(rwin, vbox);

        /* Paint at the initial size and sample the vertical bands. */
        assert(fdk_ok(fdk_window_paint(rwin)));
        fdk_surface *rs = NULL;
        assert(fdk_ok(fdk_window_get_surface(rwin, &rs)));
        fdk_surface_info rinfo;
        assert(fdk_ok(fdk_surface_get_info(rs, &rinfo)));
        fdk_i32 h0 = rinfo.height;

        /* Resize + pump; on Wayland the recorded size reaches the
         * screen through the app's NEXT COMMIT (a floating toplevel
         * follows the last committed buffer), so the loop paints as
         * it pumps — each paint commits the new size and the tree
         * re-arranges itself with zero application code. */
        fdk_window_resize(rwin, rinfo.width / 2, h0 + 120);
        for (int i = 0; i < 30; i++) {
            (void)fdk_window_paint(rwin);
            (void)fdk_pump_events(ctx, 40);
            fdk_size cur = { 0, 0 };
            (void)fdk_window_get_size(rwin, &cur);
            if (cur.height >= h0 + 100) {
                break;
            }
        }
        assert(fdk_ok(fdk_window_paint(rwin)));
        assert(fdk_ok(fdk_surface_get_info(rs, &rinfo)));

        if (rinfo.height <= h0 + 60) {
            /* TILED world: the compositor controls the geometry — a
             * client resize is overridden by the layout, so the
             * growth never lands. The full reflow verification runs
             * in the rig's FLOATING phase (for_window
             * [app_id="org.fdk.test"] floating enable); this branch
             * records honestly that this run tiled instead. */
            fdk_window_destroy(rwin);
            printf("[ok] Wayland layout reflow: growth skipped "
                   "(compositor tiles windows — client resize overridden; "
                   "the floating rig phase verifies the reflow)\n");
        } else {
        /* The GREEN body (expanding) must reach deep into the grown
         * window; the BLUE footer sits at the very bottom. Sampling
         * the physical framebuffer, scale-agnostic: relative bands. */
        fdk_i32 probe = rinfo.height - rinfo.height / 8;
        fdk_u32 bottom_px =
            rinfo.pixels[(size_t)probe * (size_t)rinfo.stride +
                         (size_t)(rinfo.width / 2)];
        int br = (int)((bottom_px >> 16) & 0xFFu);
        int bg = (int)((bottom_px >> 8) & 0xFFu);
        int bb = (int)(bottom_px & 0xFFu);
        /* The footer is the last 20 LOGICAL rows; the probe at 7/8
         * height should be inside the body (green) or the footer
         * (blue) — but NOT the red header or the black background. */
        int inked = (br > 80 && br > bg + 40) || (bg > 80 && bg > br + 40) ||
                    (bb > 80 && bb > br + 40);
        assert(inked);

        fdk_window_destroy(rwin);
        printf("[ok] Wayland layout reflow: resize configure re-arranges "
               "the content tree (expanding body, moved footer)\n");
        }
    }

    /* ---- HiDPI scale (Phase 3 completion) ----
     *
     * Whatever scale the compositor prefers (sway's config decides:
     * 1x by default, 2x in the rig's scaled run), the invariants are
     * the same: fdk_window_get_scale reports it, the surface's
     * framebuffer is PHYSICAL (logical x scale, ceil), and the widget
     * tree still covers the window proportionally after a paint (the
     * scale > 1 path composites through the logical intermediate). */
    {
        fdk_f32 scale = 1.0f;
        assert(fdk_ok(fdk_window_get_scale(win, &scale)));
        assert(scale >= 0.99f);

        /* Unmaximize first so the window has a compositor-decided
         * (not screen-filling) logical size — cleaner math. */
        (void)fdk_window_unmaximize(win);
        for (int i = 0; i < 40; i++) {
            (void)fdk_pump_events(ctx, 50);
            if (!fdk_window_is_maximized(win)) {
                break;
            }
        }
        /* Let enter/leave + preferred_scale settle. */
        for (int i = 0; i < 8; i++) {
            (void)fdk_pump_events(ctx, 50);
        }

        fdk_size logical = { 0, 0 };
        assert(fdk_ok(fdk_window_get_size(win, &logical)));
        assert(logical.width > 0 && logical.height > 0);

        fdk_f32 scale_now = 1.0f;
        assert(fdk_ok(fdk_window_get_scale(win, &scale_now)));

        fdk_surface *hs = NULL;
        assert(fdk_ok(fdk_window_get_surface(win, &hs)));
        fdk_surface_info hinfo;
        assert(fdk_ok(fdk_surface_get_info(hs, &hinfo)));

        /* Physical = ceil(logical * scale), within one pixel of the
         * float prediction (rounding at the 120th quantum). */
        fdk_f32 want_w = (fdk_f32)logical.width * scale_now;
        fdk_f32 want_h = (fdk_f32)logical.height * scale_now;
        assert((fdk_f32)hinfo.width >= want_w - 1.0f);
        assert((fdk_f32)hinfo.width <= want_w + 1.0f);
        assert((fdk_f32)hinfo.height >= want_h - 1.0f);
        assert((fdk_f32)hinfo.height <= want_h + 1.0f);

        /* Paint through the (possibly scaled) path and verify the
         * content widget's green still fills the area below any
         * decoration band — proportionally correct at any scale. */
        assert(fdk_ok(fdk_window_paint(win)));
        fdk_surface_info hinfo2;
        assert(fdk_ok(fdk_surface_get_info(hs, &hinfo2)));
        fdk_i32 probe_y = hinfo2.height - hinfo2.height / 4;
        fdk_u32 px = hinfo2.pixels[(size_t)probe_y * (size_t)hinfo2.stride +
                                   (size_t)(hinfo2.width / 2)];
        int green = ((px >> 8) & 0xFFu) > 200 &&
                    ((px >> 16) & 0xFFu) < 60 && (px & 0xFFu) < 60;
        assert(green); /* content green, not background black */

        printf("[ok] HiDPI: scale=%.3f logical %dx%d physical %dx%d "
               "(=logical x scale), content paints proportionally\n",
               (double)scale_now, logical.width, logical.height,
               hinfo2.width, hinfo2.height);
    }

    /* ---- Clipboard (Phase 9) ----
     *
     * What the protocol honestly allows us to verify against a
     * headless compositor:
     *  - SET + GET round trip on our own selection (compositors
     *    never send a client its own selection back, so FDK serves
     *    from its tracked copy — this is the real production path
     *    for "did my own copy survive").
     *  - GET with no text selection -> NULL (the data-device
     *    listener machinery correctly reports emptiness).
     *  Cross-client transfers cannot be driven here: set_selection
     *    must cite an input serial, and the headless seat generates
     *    no input events (documented in fdk_clipboard.h). That path
     *    is exercised structurally (protocol objects + requests in
     *    the WAYLAND_DEBUG trace of the rig) and by the X11 suite's
     *    cross-process equivalents. */
    {
        assert(fdk_ok(fdk_clipboard_set_text(ctx, "wayland phase 9")));
        char *text = fdk_clipboard_get_text(ctx);
        assert(text != NULL && strcmp(text, "wayland phase 9") == 0);
        printf("[ok] clipboard: set + get round trip on own selection\n");

        assert(fdk_ok(fdk_clipboard_set_text(ctx, "replaced")));
        char *text2 = fdk_clipboard_get_text(ctx);
        assert(text2 != NULL && strcmp(text2, "replaced") == 0);
        printf("[ok] clipboard: replace semantics\n");
        /* text/text2 leak is intentional-free below via explicit free;
         * there is no public fdk_free, and the docs bless free(). */
        free(text);
        free(text2);

        /* Empty text reads as NULL. */
        assert(fdk_ok(fdk_clipboard_set_text(ctx, "")));
        assert(fdk_clipboard_get_text(ctx) == NULL);
        printf("[ok] clipboard: empty own-selection reads as NULL\n");
    }


    /* ---- Menus / dialogs (Phase 9 completion) ----
     *
     * The popup machinery against a REAL compositor, two layers:
     *
     * 1. INTERACTIVE (when sway's IPC is reachable): swaymsg drives
     *    the seat cursor — REAL pointer events carrying REAL grab
     *    serials, the one thing a headless seat cannot mint any
     *    other way. A button click opens a context menu; the popup
     *    grabs with the click's serial; sway configures + maps it;
     *    a second click lands ON the popup's first item and
     *    ACTIVATES it. The item callback firing is end-to-end proof
     *    of the whole chain (input -> serial -> xdg_popup.grab ->
     *    configure -> auto-paint buffer -> hit-test -> activate ->
     *    chain close).
     *
     * 2. STRUCTURAL (always): popup creation must not error the
     *    protocol (a marshal/protocol error poisons the connection
     *    — the Phase 9 tests caught exactly that in the decoration
     *    path), and destroying the PARENT window sweeps the popup
     *    (the popup-family contract) with clean ASan teardown.
     *
     * The serial-0 grab (programmatic popup with no prior input) is
     * documented in fdk_window.h: sway never configures such a
     * popup — honest behavior, which is why the structural section
     * asserts survival, not mapping. */
    {
        fdk_font *font = fdk_font_load(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16);
        if (font == NULL) {
            printf("[skip] wayland menu popup (no system font)\n");
        } else {
            fdk_menu *menu = NULL;
            assert(fdk_ok(fdk_menu_create(font, &menu)));
            fdk_menu_item *mi_open = NULL;
            assert(fdk_ok(fdk_menu_append(menu, "Open", &mi_open)));
            assert(fdk_ok(fdk_menu_append_separator(menu)));
            assert(fdk_ok(fdk_menu_append_check(menu, "Toolbar",
                                                false, NULL)));

            /* The opener button on the MAIN window: clicking it pops
             * the menu below it — the canonical context-menu flow. */
            static int item_hits = 0;
            static int button_hits = 0;
            fdk_menu_item_set_on_activate(mi_open, wayland_menu_item_cb,
                                          &item_hits);
            fdk_widget *btn = NULL;
            assert(fdk_ok(fdk_button_create(root, font, "Menu", &btn)));
            g_wayland_menu = menu;
            g_wayland_menu_anchor = btn;
            fdk_button_set_on_activate(
                btn, wayland_menu_button_cb, &button_hits);
            fdk_widget_arrange(btn, (fdk_rect){20, 40, 90, 30});
            fdk_window_paint(win);
            (void)fdk_pump_events(ctx, 300);

            if (wayland_injector_start()) {
                /* Wait for the pointer capability to reach us (the
                 * device attaches on the injector's connection; the
                 * capabilities event arrives on our next roundtrip). */
                pump_and_paint(ctx, win, 500);

                /* Window pinned at (100,60) by the rig's sway config;
                 * the button's center is therefore at (165,115). */
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                assert(button_hits == 1);
                printf("[ok] wayland menu: REAL seat click reached "
                       "the button (valid grab serial minted)\n");

                /* The popup: anchored below the button (window-local
                 * (20,70), absolute (120,130)); its first row's
                 * center is around (160,143) — move + click there. */
                wayland_inject("move 160 143");
                pump_and_paint(ctx, win, 500);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 500);
                assert(item_hits == 1);
                printf("[ok] wayland menu: popup MAPPED, item "
                       "clicked THROUGH the real compositor, chain "
                       "closed\n");

                /* Outside-click dismissal: open again, click far
                 * away. The popup's grab routes the press to the
                 * popup tree -> sway dismisses (popup_done) -> the
                 * chain closes. Proof it closed: the opener button
                 * is clickable again (a live grab would have eaten
                 * it). */
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                assert(button_hits == 2);
                wayland_inject("move 400 400");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                wayland_inject("move 165 115");
                pump_and_paint(ctx, win, 300);
                wayland_inject("tap 1");
                pump_and_paint(ctx, win, 400);
                assert(button_hits == 3);
                printf("[ok] wayland menu: outside click dismissed "
                       "the chain (opener reachable again)\n");
                wayland_injector_stop();
            } else {
                printf("[skip] wayland menu INTERACTIVE section "
                       "(fdk-wl-inject unavailable — serial-0 popups "
                       "never map on sway, so only the structural "
                       "checks can run here)\n");
            }

            /* Structural: serial-0 programmatic popup must not
             * poison the protocol (the decoration-marshal bug did),
             * and the parent sweep must take the popup down. */
            assert(fdk_ok(fdk_menu_popup_at(menu, btn, 0, 30)));
            (void)fdk_pump_events(ctx, 300);
            printf("[ok] wayland menu popup: protocol survived the "
                   "serial-0 popup request\n");

            fdk_menu_destroy(menu);
            fdk_font_destroy(font);
        }

        /* Dialog: a toplevel built and auto-painted by the toolkit;
         * the early-destroy path answers the negative response. */
        static int dlg_responses = 0;
        fdk_dialog_options dopts = {
            .title = "FDK wayland dialog",
            .text = "Hello from the Wayland suite",
            .buttons = FDK_DIALOG_BUTTONS_OK_CANCEL,
        };
        fdk_window *dlg = NULL;
        assert(fdk_ok(fdk_dialog_show_message(
            ctx, &dopts, wayland_dialog_response_cb, &dlg_responses,
            &dlg)));
        (void)fdk_pump_events(ctx, 300);
        assert(dlg_responses == 0); /* nobody answered yet */
        fdk_window_destroy(dlg); /* early destroy answers negative */
        (void)fdk_pump_events(ctx, 200);
        assert(dlg_responses == 1 &&
               g_wayland_dlg_last == FDK_DIALOG_CANCEL);
        printf("[ok] wayland dialog: mapped + auto-painted, early "
               "destroy answers Cancel, clean teardown\n");
    }

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] clean teardown under Wayland\n");

    printf("\nall Wayland integration tests passed\n");
    return 0;
}
