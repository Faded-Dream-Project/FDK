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
    fdk_init_options opts = { .backend = FDK_PLATFORM_WAYLAND };
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
            fdk_u32 below = info.pixels[(size_t)info.stride * (size_t)below_row +
                                        info.width / 2];
            assert(pixel_is_band_fill(mid));
            assert(!pixel_is_band_fill(below) ||
                   info.height < (fdk_i32)(48.0f * p_scale));
            printf("[ok] client-side decorations confirmed: themed band "
                   "rendered in-frame (pixel-verified), content below\n");
        }
    }

    /* ---- Maximize: request + configure-driven state event ---- */
    /* Sway's default layout tiles windows, which reports MAXIMIZED in
     * the toplevel configure AT MAP TIME — before any request of
     * ours. (kiosk-shell weston maximizes everything too.) The test
     * therefore handles both worlds: a compositor that maximizes on
     * request, and one that already maximized at map. */
    bool pre_max = fdk_window_is_maximized(win);
    int before = g_state_events;
    assert(fdk_ok(fdk_window_maximize(win)));
    for (int i = 0; i < 40; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (fdk_window_is_maximized(win)) {
            break;
        }
    }
    if (!pre_max) {
        /* Fresh transition: the configure must have carried
         * MAXIMIZED, and FDK must have delivered the event. */
        assert(fdk_window_is_maximized(win));
        assert(g_state_events > before);
        assert(g_last_maximized == 1);
        printf("[ok] maximize: xdg configure reported MAXIMIZED state, "
               "FDK_EVENT_WINDOW_STATE delivered\n");
    } else {
        /* Already maximized at map: the state event fired THEN (the
         * map-time configure proved the whole chain — states parsing,
         * change detection, event dispatch); the later request is a
         * no-op that correctly dispatches nothing new. The REQUEST
         * itself is protocol-verified by the rig's WAYLAND_DEBUG
         * counts. */
        assert(g_state_events >= 1);
        assert(fdk_window_is_maximized(win));
        printf("[ok] maximize request sent; compositor tiled the window "
               "(maximized at map, state event then delivered)\n");
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
