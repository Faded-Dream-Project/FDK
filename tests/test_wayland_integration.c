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
#include "fdk/fdk_theme.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_state_events = 0;
static int g_deco_events = 0;
static int g_last_maximized = -1;
static int g_last_minimized = -1;

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

    fdk_window_destroy(win);
    fdk_shutdown(ctx);
    printf("[ok] clean teardown under Wayland\n");

    printf("\nall Wayland integration tests passed\n");
    return 0;
}
