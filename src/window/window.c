#define FDK_LOG_TAG "window"

#include "fdk/fdk_window.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_widgets.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "render/surface_internal.h"
#include "theme/theme_internal.h"
#include "widget/widget_internal.h"
#include "window/window_internal.h"

#include <string.h>
#include <time.h>

/* ---- Phase 8: FDK-drawn decorations + window management ----
 *
 * The title band is a single widget with a private paint class: a
 * themed fill, the themed bottom rule, the title (a catalog Label
 * child), and three window-management buttons (minimize /
 * maximize-restore / close) drawn as VECTOR GLYPHS right in the band
 * paint — lines and rects, deliberately NOT font glyphs, so a system
 * with no fonts still gets fully functional window buttons (the
 * title text alone degrades). Button hit-testing and interaction
 * (hover/press/activate) live in the band's event callback; the
 * backend's optional move/state ops do the actual window management.
 *
 * The band's height is the theme's title_bar_height metric (a
 * LAYOUT metric: switching themes re-arranges decorated windows via
 * the band's theme hook — see deco_bar_theme_changed).
 */

/* Button box geometry inside the band: 22px wide, 4px inset top and
 * bottom, 6px between buttons. */
#define DECO_BTN_W 22
#define DECO_BTN_GAP 6

/* FDK-drawn resize-edge zone width (px) when fdk_window_set_resizable
 * is on — the CSD stand-in for a WM frame's resize borders. */
#define DECO_RESIZE_BORDER 5

static void window_arrange_deco(fdk_window *window);

/* Internal size floors for the FDK-driven resize drag when the app
 * set no explicit limits via fdk_window_set_size_limits: a decorated
 * window must keep at least band + 8px of content; any window keeps
 * 60x32. */
#define DECO_MIN_W 60
#define DECO_MIN_H 32

/* ---- Pure helpers (headless-tested in tests/test_window_logic.c) ---- */

static fdk_i64 now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (fdk_i64)ts.tv_sec * 1000 + (fdk_i64)ts.tv_nsec / 1000000;
}

bool fdk__window_is_double_click(fdk_i64 now_ms, fdk_i64 last_ms,
                                 fdk_i32 dx, fdk_i32 dy) {
    if (last_ms > now_ms) {
        return false; /* clock went backwards (or never set) */
    }
    if (now_ms - last_ms > FDK_WINDOW_DBLCLICK_MS) {
        return false;
    }
    if (dx < -FDK_WINDOW_DBLCLICK_SLOP || dx > FDK_WINDOW_DBLCLICK_SLOP) {
        return false;
    }
    if (dy < -FDK_WINDOW_DBLCLICK_SLOP || dy > FDK_WINDOW_DBLCLICK_SLOP) {
        return false;
    }
    return true;
}

fdk_window_resize_edge fdk__window_resize_edge_at(fdk_i32 width,
                                                  fdk_i32 height,
                                                  fdk_i32 x, fdk_i32 y,
                                                  fdk_i32 border) {
    if (border <= 0 || width <= 0 || height <= 0) {
        return FDK_WRES_NONE;
    }
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return FDK_WRES_NONE; /* outside the window: not our press */
    }
    int near_l = x < border;
    int near_r = x >= width - border;
    int near_t = y < border;
    int near_b = y >= height - border;
    /* Degenerate case — a window dimension at or below 2*border makes
     * both sides "near"; keep only the nearer one so a corner is never
     * two opposite edges at once. */
    if (near_l && near_r) {
        near_l = (x < width - x);
        near_r = !near_l;
    }
    if (near_t && near_b) {
        near_t = (y < height - y);
        near_b = !near_t;
    }
    if (near_t && near_l) {
        return FDK_WRES_NW;
    }
    if (near_t && near_r) {
        return FDK_WRES_NE;
    }
    if (near_b && near_l) {
        return FDK_WRES_SW;
    }
    if (near_b && near_r) {
        return FDK_WRES_SE;
    }
    if (near_t) {
        return FDK_WRES_N;
    }
    if (near_b) {
        return FDK_WRES_S;
    }
    if (near_l) {
        return FDK_WRES_W;
    }
    if (near_r) {
        return FDK_WRES_E;
    }
    return FDK_WRES_NONE;
}

void fdk__window_resize_apply(fdk_window_resize_edge edge,
                              fdk_i32 dx, fdk_i32 dy,
                              fdk_i32 ox, fdk_i32 oy,
                              fdk_i32 ow, fdk_i32 oh,
                              fdk_i32 min_w, fdk_i32 min_h,
                              fdk_i32 max_w, fdk_i32 max_h,
                              fdk_i32 *out_x, fdk_i32 *out_y,
                              fdk_i32 *out_w, fdk_i32 *out_h) {
    /* Which sides does this edge drag? (Compass values are a bitmask
     * friendly rotation: N=1, NE=2, E=3, ... — deriving sides from the
     * numeric pattern keeps the math table-free.) */
    int n = (edge == FDK_WRES_N || edge == FDK_WRES_NE ||
             edge == FDK_WRES_NW);
    int s = (edge == FDK_WRES_S || edge == FDK_WRES_SE ||
             edge == FDK_WRES_SW);
    int e = (edge == FDK_WRES_E || edge == FDK_WRES_NE ||
             edge == FDK_WRES_SE);
    int w = (edge == FDK_WRES_W || edge == FDK_WRES_NW ||
             edge == FDK_WRES_SW);

    fdk_i32 nw = ow, nh = oh;
    if (w) {
        nw = ow - dx;
    }
    if (e) {
        nw = ow + dx;
    }
    if (n) {
        nh = oh - dy;
    }
    if (s) {
        nh = oh + dy;
    }

    /* Clamp dimensions; then anchor the OPPOSITE edges so the clamped
     * side stays put (a W-edge drag that hit min-width must not also
     * drag the window's right edge across the screen). */
    if (min_w > 0 && nw < min_w) {
        nw = min_w;
    }
    if (min_h > 0 && nh < min_h) {
        nh = min_h;
    }
    if (max_w > 0 && nw > max_w) {
        nw = max_w;
    }
    if (max_h > 0 && nh > max_h) {
        nh = max_h;
    }
    if (nw < 1) {
        nw = 1;
    }
    if (nh < 1) {
        nh = 1;
    }

    *out_w = nw;
    *out_h = nh;
    *out_x = ox;
    *out_y = oy;
    if (w) {
        *out_x = ox + (ow - nw); /* right edge anchored */
    }
    if (n) {
        *out_y = oy + (oh - nh); /* bottom edge anchored */
    }
}

/* ---- Band geometry ---- */

/* The themed band height, clamped into the metric's own legal range
 * (the parser/setters enforce 12..64; this is belt-and-braces for a
 * programmatically built theme). */
static fdk_i32 deco_band_height(const fdk_window *window) {
    (void)window;
    fdk_i32 h = fdk_theme_get_metric(NULL, FDK_TM_TITLE_BAR_HEIGHT);
    if (h < 12) {
        h = 12;
    }
    if (h > 64) {
        h = 64;
    }
    return h;
}

/* Which window-management buttons does this backend support? */
static bool deco_has_maximize(const fdk_window *window) {
    return window != NULL && window->ops->window_set_maximized != NULL;
}

static bool deco_has_minimize(const fdk_window *window) {
    return window != NULL && window->ops->window_set_minimized != NULL;
}

/* ---- Band painting (fill + rule + vector-glyph buttons) ---- */

/* Button fill by interaction state: the same tokens the catalog
 * Button uses, so the band buttons theme-switch for free. */
static fdk_color deco_button_fill(const fdk_window *window, int which) {
    if (window->deco_pressed == which) {
        return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_PRESSED);
    }
    if (window->deco_hover == which) {
        return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_HOVER);
    }
    return fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
}

/* Draws one window-management glyph centered in (bx, by, bw, bh) in
 * the band's (window-local) coordinate space. `which`: 1 minimize,
 * 2 maximize (restore glyph when the window is maximized), 3 close.
 * All primitives — no font, so the glyphs survive a fontless system
 * and scale with the button, not the font size. */
static void deco_paint_glyph(fdk_window *window, fdk_surface *surface,
                             int which, fdk_i32 bx, fdk_i32 by,
                             fdk_i32 bw, fdk_i32 bh, fdk_color fill) {
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    fdk_i32 cx = bx + bw / 2;
    fdk_i32 cy = by + bh / 2;
    switch (which) {
    case 1: /* minimize: an ink bar near the baseline */
        fdk_surface_fill_rect(surface,
                              (fdk_rect){cx - 5, cy + 3, 10, 2}, ink);
        break;
    case 2:
        if (window->maximized) {
            /* restore: two overlapping window outlines; the front one
             * is filled with the button's own fill so the back one
             * reads as behind it. */
            fdk_surface_draw_rect(surface,
                                  (fdk_rect){cx - 2, cy - 6, 9, 9}, ink);
            fdk_surface_fill_rect(surface,
                                  (fdk_rect){cx - 6, cy - 2, 9, 9}, fill);
            fdk_surface_draw_rect(surface,
                                  (fdk_rect){cx - 6, cy - 2, 9, 9}, ink);
        } else {
            /* maximize: a window outline */
            fdk_surface_draw_rect(surface,
                                  (fdk_rect){cx - 5, cy - 5, 10, 10}, ink);
        }
        break;
    case 3: /* close: a crisp X */
        fdk_surface_draw_line(surface, cx - 4, cy - 4, cx + 4, cy + 4, ink);
        fdk_surface_draw_line(surface, cx - 4, cy + 4, cx + 4, cy - 4, ink);
        break;
    default:
        break;
    }
}

static void deco_bar_paint(fdk_widget *w, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_window *window = fdk_widget_get_user_data(w);
    if (window == NULL || window->deco_bar != w) {
        return;
    }
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_surface_fill_rect(
        surface, bounds,
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND));

    /* Window-management buttons (bounds are root/window-local, which
     * is exactly where the hit rects live). */
    const struct {
        int which;
        const fdk_rect *rect;
        int present;
    } buttons[3] = {
        {1, &window->deco_btn_min, deco_has_minimize(window)},
        {2, &window->deco_btn_max, deco_has_maximize(window)},
        {3, &window->deco_btn_close, 1},
    };
    for (int i = 0; i < 3; i++) {
        if (!buttons[i].present || buttons[i].rect->width <= 0) {
            continue;
        }
        fdk_color fill = deco_button_fill(window, buttons[i].which);
        fdk_surface_fill_rect(surface, *buttons[i].rect, fill);
        deco_paint_glyph(window, surface, buttons[i].which,
                         buttons[i].rect->x, buttons[i].rect->y,
                         buttons[i].rect->width, buttons[i].rect->height,
                         fill);
    }

    fdk_i32 t = fdk_theme_get_metric(NULL, FDK_TM_SEPARATOR_THICKNESS);
    if (t > bounds.height) {
        t = bounds.height;
    }
    fdk_surface_fill_rect(
        surface, (fdk_rect){bounds.x, bounds.y + bounds.height - t,
                            bounds.width, t},
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER));
}

static const fdk_widget_class deco_bar_class = {
    .size = sizeof(fdk_widget),
    .name = "fdk-deco-bar",
    .handle_event = NULL,
    .paint = deco_bar_paint,
    .measure = NULL,
    .arrange = NULL,
    .destroy = NULL,
};

/* ---- Band interaction ---- */

/* Which band button contains the window-local point? 0 = none. */
static int deco_button_at(const fdk_window *window, fdk_i32 x, fdk_i32 y) {
    const struct {
        int which;
        const fdk_rect *rect;
        int present;
    } buttons[3] = {
        {1, &window->deco_btn_min, deco_has_minimize(window)},
        {2, &window->deco_btn_max, deco_has_maximize(window)},
        {3, &window->deco_btn_close, 1},
    };
    for (int i = 0; i < 3; i++) {
        if (!buttons[i].present) {
            continue;
        }
        const fdk_rect *r = buttons[i].rect;
        if (x >= r->x && x < r->x + r->width &&
            y >= r->y && y < r->y + r->height) {
            return buttons[i].which;
        }
    }
    return 0;
}

/* The close button delivers the SAME event the WM's delete would, so
 * application close semantics are identical either way. Nested into
 * fdk_window_dispatch_event — the same reentrancy protections as the
 * real path apply (a callback may destroy the window; callers must
 * not touch `window` afterwards). */
static void deco_close_activate(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    fdk_event_data ev;
    memset(&ev, 0, sizeof ev);
    ev.type = FDK_EVENT_WINDOW_CLOSE_REQUEST;
    fdk_window_dispatch_event(window, &ev);
}

static void deco_button_activate(fdk_window *window, int which) {
    switch (which) {
    case 1:
        (void)fdk_window_minimize(window);
        break;
    case 2:
        if (window->maximized) {
            (void)fdk_window_unmaximize(window);
        } else {
            (void)fdk_window_maximize(window);
        }
        break;
    case 3:
        deco_close_activate(window);
        return; /* the app callback may have destroyed the window */
    default:
        break;
    }
}

/* Bar-local drag. Snap formulation: on each motion the window is
 * moved so the press anchor sits under the pointer again (origin +=
 * local_now - anchor). Because the move is flushed before the next
 * motion event is generated, each event's bar-local coordinates are
 * relative to the frame the previous move produced — the drag
 * converges instead of drifting.
 *
 * Preferred path is the backend's interactive move (EWMH
 * _NET_WM_MOVERESIZE / xdg_toplevel.move): one call, the WM drives
 * everything, and reparenting WMs move the FRAME not just the client
 * (fixing the documented v1 caveat). The snap drag is the fallback
 * for bare X. */
static bool deco_bar_event(fdk_widget *w, const fdk_widget_event *ev,
                           void *user) {
    fdk_window *window = user;
    if (window == NULL || w != window->deco_bar) {
        return false;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if (ev->pointer.button != 1) {
            return false; /* non-primary button */
        }
        {
            fdk_i32 x = (fdk_i32)ev->pointer.position.x;
            fdk_i32 y = (fdk_i32)ev->pointer.position.y;

            /* Button press: track press + hover, activate on release
             * inside the same button. */
            int hit = deco_button_at(window, x, y);
            if (hit != 0) {
                window->deco_pressed = hit;
                window->deco_hover = hit;
                fdk_widget_invalidate(w);
                return true;
            }

            /* Band background: double-click toggles maximize (the
             * classic title-bar gesture). */
            fdk_i64 now = now_ms();
            if (fdk__window_is_double_click(
                    now, window->last_band_click_ms,
                    x - window->last_band_click_x,
                    y - window->last_band_click_y)) {
                window->last_band_click_ms = -(FDK_WINDOW_DBLCLICK_MS + 1);
                window->dragging = false;
                if (window->maximized) {
                    (void)fdk_window_unmaximize(window);
                } else {
                    (void)fdk_window_maximize(window);
                }
                return true;
            }
            window->last_band_click_ms = now;
            window->last_band_click_x = x;
            window->last_band_click_y = y;

            /* Single press on the band starts a move — unless the
             * window is maximized (dragging a maximized window is a
             * restore-on-drag gesture FDK v1 does not implement). */
            if (window->maximized) {
                return true;
            }
            if (window->ops->window_begin_move != NULL &&
                fdk_ok(window->ops->window_begin_move(
                    window->pwindow, x, y))) {
                return true; /* WM/compositor drives from here */
            }
            if (window->ops->window_get_position == NULL ||
                window->ops->window_move_to == NULL) {
                return false; /* backend can't move at all */
            }
            if (!fdk_ok(window->ops->window_get_position(
                    window->pwindow, &window->drag_origin_x,
                    &window->drag_origin_y))) {
                return false;
            }
            window->drag_anchor = ev->pointer.position;
            window->dragging = true;
            return true;
        }

    case FDK_WIDGET_POINTER_MOTION:
        if (window->deco_pressed != 0) {
            /* While a button is held: keep it highlighted; activation
             * is decided at release. */
            return true;
        }
        if (!window->dragging) {
            /* Hover highlight follows the pointer across buttons. */
            int hover = deco_button_at(
                window, (fdk_i32)ev->position.x, (fdk_i32)ev->position.y);
            if (hover != window->deco_hover) {
                window->deco_hover = hover;
                fdk_widget_invalidate(w);
            }
            return false;
        }
        window->ops->window_move_to(
            window->pwindow,
            window->drag_origin_x +
                (fdk_i32)(ev->position.x - window->drag_anchor.x),
            window->drag_origin_y +
                (fdk_i32)(ev->position.y - window->drag_anchor.y));
        /* The move above becomes the frame the NEXT motion event is
         * measured against; keep the origin current, anchor stays. */
        window->drag_origin_x += (fdk_i32)(ev->position.x -
                                           window->drag_anchor.x);
        window->drag_origin_y += (fdk_i32)(ev->position.y -
                                           window->drag_anchor.y);
        return true;

    case FDK_WIDGET_POINTER_UP:
        if (window->deco_pressed != 0) {
            int pressed = window->deco_pressed;
            window->deco_pressed = 0;
            window->deco_hover = deco_button_at(
                window, (fdk_i32)ev->pointer.position.x,
                (fdk_i32)ev->pointer.position.y);
            fdk_widget_invalidate(w);
            if (window->deco_hover == pressed) {
                deco_button_activate(window, pressed);
                /* close may have destroyed the window — return NOW,
                 * touching nothing else (the bar itself is protected
                 * by the tree's deferred destroy). */
            }
            return true;
        }
        window->dragging = false;
        return true;

    default:
        return false;
    }
}

/* Theme switches may change title_bar_height (a LAYOUT metric): re-
 * run the band arrangement + content layout. Called from the widget
 * root registry's theme-notify walk; the same walk damages the root
 * afterwards, so no explicit invalidate is needed. */
static void deco_bar_theme_changed(fdk_widget *w) {
    fdk_window *window = fdk_widget_get_user_data(w);
    if (window == NULL || !window->decorated ||
        window->deco_bar != w) {
        return;
    }
    window_arrange_deco(window);
    fdk_window_layout(window);
}

/* Sizes the band and its children for the current window width; the
 * content widget is re-laid-out below the band by the caller. */
static void window_arrange_deco(fdk_window *window) {
    if (!window->decorated || window->root == NULL ||
        window->deco_bar == NULL) {
        return;
    }
    fdk_i32 h = deco_band_height(window);
    fdk_i32 w = window->last_size.width;
    fdk_widget_set_bounds(window->deco_bar, (fdk_rect){0, 0, w, h});

    /* Buttons right-aligned: close always; maximize/minimize when the
     * backend supports the state ops. */
    fdk_i32 bh = h - 8;
    if (bh < 4) {
        bh = 4;
    }
    fdk_i32 x = w - (DECO_BTN_W + 6);
    if (x < 0) {
        x = 0;
    }
    window->deco_btn_close =
        (fdk_rect){x, 4, DECO_BTN_W, bh};
    if (deco_has_maximize(window)) {
        x -= DECO_BTN_W + DECO_BTN_GAP;
        if (x < 0) {
            x = 0;
        }
        window->deco_btn_max = (fdk_rect){x, 4, DECO_BTN_W, bh};
    } else {
        window->deco_btn_max = (fdk_rect){0, 0, 0, 0};
    }
    if (deco_has_minimize(window)) {
        x -= DECO_BTN_W + DECO_BTN_GAP;
        if (x < 0) {
            x = 0;
        }
        window->deco_btn_min = (fdk_rect){x, 4, DECO_BTN_W, bh};
    } else {
        window->deco_btn_min = (fdk_rect){0, 0, 0, 0};
    }

    /* Title: left-padded, clear of the leftmost button. */
    fdk_i32 title_right = x - 4;
    fdk_i32 th = 0;
    if (window->deco_font != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(window->deco_font, &fm);
        th = fm.ascent + fm.descent;
    }
    fdk_i32 ly = (h - th) / 2;
    if (ly < 0) {
        ly = 0;
    }
    if (window->deco_title != NULL) {
        fdk_i32 tw = title_right - 10;
        if (tw < 0) {
            tw = 0;
        }
        fdk_widget_set_bounds(
            window->deco_title,
            (fdk_rect){10, ly, tw, th > 0 ? th : 1});
    }
}

fdk_result fdk_window_create(fdk_context *ctx,
                              const fdk_window_options *options,
                              fdk_window **out_window) {
    if (ctx == NULL || out_window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ctx->ops == NULL || ctx->conn == NULL) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    fdk_window *window = fdk_alloc(sizeof(fdk_window));
    if (window == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    window->ctx = ctx;
    window->ops = ctx->ops;
    window->event_callback = NULL;
    window->event_callback_user_data = NULL;
    window->surface = NULL;
    window->paint_intermediate = NULL;
    window->root = NULL;
    window->content = NULL;
    window->decorated = false;
    window->deco_bar = NULL;
    window->deco_title = NULL;
    window->deco_close = NULL;
    window->deco_font = NULL;
    window->deco_font_owned = NULL;
    window->title = NULL;
    window->dragging = false;
    window->drag_anchor = (fdk_pointf){0.0f, 0.0f};
    window->drag_origin_x = 0;
    window->drag_origin_y = 0;
    window->maximized = false;
    window->minimized = false;
    window->min_size = (fdk_size){0, 0};
    window->max_size = (fdk_size){0, 0};
    window->resizable = false;
    window->resizable_explicit = false;
    window->resize_edge = FDK_WRES_NONE;
    window->resize_press_x = 0;
    window->resize_press_y = 0;
    window->resize_orig_x = 0;
    window->resize_orig_y = 0;
    window->resize_orig_w = 0;
    window->resize_orig_h = 0;
    window->last_band_click_ms = -(FDK_WINDOW_DBLCLICK_MS + 1);
    window->last_band_click_x = 0;
    window->last_band_click_y = 0;
    window->deco_btn_min = (fdk_rect){0, 0, 0, 0};
    window->deco_btn_max = (fdk_rect){0, 0, 0, 0};
    window->deco_btn_close = (fdk_rect){0, 0, 0, 0};
    window->deco_hover = 0;
    window->deco_pressed = 0;

    if (options != NULL && options->title != NULL) {
        size_t n = strlen(options->title) + 1;
        window->title = fdk_alloc(n);
        if (window->title != NULL) {
            memcpy(window->title, options->title, n);
        }
    }

    fdk_result r = ctx->ops->window_create(ctx->conn, options, &window->pwindow);
    if (!fdk_ok(r)) {
        fdk_free(window);
        return r;
    }

    window->last_size.width = (options != NULL && options->width > 0) ? options->width : 640;
    window->last_size.height = (options != NULL && options->height > 0) ? options->height : 480;

    r = fdk_context_register_window(ctx, window);
    if (!fdk_ok(r)) {
        ctx->ops->window_destroy(window->pwindow);
        fdk_free(window);
        return r;
    }

    FDK_DEBUG("window created");

    *out_window = window;
    return FDK_OK;
}

void fdk_window_show(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    window->ops->window_show(window->pwindow);
}

void fdk_window_hide(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    window->ops->window_hide(window->pwindow);
}

void fdk_window_destroy(fdk_window *window) {
    if (window == NULL) {
        return;
    }
    if (window->root != NULL) {
        /* The window owns its root; drop the ownership marker so the
         * widget layer lets us destroy it, then tear the tree down
         * (subclass destroy hooks run, deferred destroys settle).
         * The decoration band is a subtree of the root and dies with
         * it. */
        window->root->flags &= ~FDK_WF_WINDOW_ROOT;
        fdk_widget_destroy(window->root);
        window->root = NULL;
    }
    window->deco_bar = NULL;
    window->deco_title = NULL;
    window->deco_close = NULL;
    if (window->deco_font_owned != NULL) {
        fdk_font_destroy(window->deco_font_owned);
        window->deco_font_owned = NULL;
    }
    window->deco_font = NULL;
    fdk_free(window->title);
    window->title = NULL;
    fdk_surface_detach_from_window(window);
    fdk_surface_destroy(window->paint_intermediate);
    window->paint_intermediate = NULL;
    fdk_context_unregister_window(window->ctx, window);
    window->ops->window_destroy(window->pwindow);
    fdk_free(window);
}

void fdk_window_set_title(fdk_window *window, const char *title) {
    if (window == NULL) {
        return;
    }
    window->ops->window_set_title(window->pwindow, title);

    /* Keep our own copy so a later set_decorated(true) can label the
     * band without asking the backend to read it back. */
    fdk_free(window->title);
    window->title = NULL;
    if (title != NULL) {
        size_t n = strlen(title) + 1;
        window->title = fdk_alloc(n);
        if (window->title != NULL) {
            memcpy(window->title, title, n);
        }
    }
    if (window->decorated && window->deco_title != NULL) {
        (void)fdk_label_set_text(window->deco_title, window->title);
    }
}

void fdk_window_resize(fdk_window *window, fdk_i32 width, fdk_i32 height) {
    if (window == NULL || width <= 0 || height <= 0) {
        return;
    }
    window->ops->window_resize(window->pwindow, width, height);
}

fdk_result fdk_window_get_size(const fdk_window *window, fdk_size *out_size) {
    if (window == NULL || out_size == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_size = window->last_size;
    return FDK_OK;
}

fdk_result fdk_window_get_scale(const fdk_window *window,
                                fdk_f32 *out_scale) {
    if (window == NULL || out_scale == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* NULL op = backend has no scale concept (X11): 1.0 is the
     * honest answer, not a fallback masquerading as information. */
    if (window->ops->window_get_scale == NULL) {
        *out_scale = 1.0f;
        return FDK_OK;
    }
    return window->ops->window_get_scale(window->pwindow, out_scale);
}

void fdk_window_set_size_limits(fdk_window *window, fdk_size min_size, fdk_size max_size) {
    if (window == NULL) {
        return;
    }
    /* Remember them: FDK's own resize-edge drag clamps to these too
     * (on a bare X server there is no WM to enforce hints). */
    window->min_size = min_size;
    window->max_size = max_size;
    window->ops->window_set_size_limits(window->pwindow, min_size, max_size);
}

/* ---- Window state (Phase 8) ---- */

fdk_result fdk_window_maximize(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->window_set_maximized == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->window_set_maximized(window->pwindow, true);
}

fdk_result fdk_window_unmaximize(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->window_set_maximized == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->window_set_maximized(window->pwindow, false);
}

fdk_result fdk_window_minimize(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->window_set_minimized == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->window_set_minimized(window->pwindow, true);
}

fdk_result fdk_window_restore(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->window_set_minimized == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->window_set_minimized(window->pwindow, false);
}

bool fdk_window_is_maximized(const fdk_window *window) {
    return window != NULL && window->maximized;
}

bool fdk_window_is_minimized(const fdk_window *window) {
    return window != NULL && window->minimized;
}

/* ---- Interactive resize (Phase 8) ---- */

void fdk_window_set_resizable(fdk_window *window, bool resizable) {
    if (window == NULL) {
        return;
    }
    window->resizable = resizable;
    window->resizable_explicit = true;
    if (!resizable) {
        /* A drag in flight when edges turn off must stop cleanly. */
        window->resize_edge = FDK_WRES_NONE;
    }
}

bool fdk_window_get_resizable(const fdk_window *window) {
    return window != NULL && window->resizable;
}

/* Applies one step of the FDK-driven resize drag (the bare-X
 * fallback; the EWMH/Wayland path never reaches here — the backend
 * took the drag). */
static void window_apply_resize(fdk_window *window, fdk_i32 x, fdk_i32 y) {
    fdk_i32 min_w = window->min_size.width > 0 ? window->min_size.width
                                               : DECO_MIN_W;
    fdk_i32 min_h = window->min_size.height;
    if (min_h <= 0) {
        min_h = window->decorated ? deco_band_height(window) + 8
                                  : DECO_MIN_H;
    }
    fdk_i32 max_w = window->max_size.width;
    fdk_i32 max_h = window->max_size.height;

    fdk_i32 nx = 0, ny = 0, nw = 0, nh = 0;
    fdk__window_resize_apply(window->resize_edge,
                             x - window->resize_press_x,
                             y - window->resize_press_y,
                             window->resize_orig_x, window->resize_orig_y,
                             window->resize_orig_w, window->resize_orig_h,
                             min_w, min_h, max_w, max_h,
                             &nx, &ny, &nw, &nh);
    if (nw == window->resize_orig_w && nh == window->resize_orig_h &&
        nx == window->resize_orig_x && ny == window->resize_orig_y) {
        return; /* clamped to no-op: don't send anything */
    }
    if (window->ops->window_move_resize_to != NULL) {
        window->ops->window_move_resize_to(window->pwindow, nx, ny, nw, nh);
    } else if (window->ops->window_move_to != NULL) {
        window->ops->window_move_to(window->pwindow, nx, ny);
        window->ops->window_resize(window->pwindow, nw, nh);
    }
}

/* Returns true when the window-level layer consumed the pointer
 * event (an edge-zone press or an in-flight resize drag): the widget
 * tree must not see it. */
static bool window_handle_resize_event(fdk_window *window,
                                       const fdk_event_data *event) {
    if (!window->resizable) {
        return false;
    }

    if (window->resize_edge != FDK_WRES_NONE) {
        /* Drag in flight (FDK-driven only; a backend-driven drag
         * leaves resize_edge NONE and events flow normally). */
        switch (event->type) {
        case FDK_EVENT_POINTER_MOTION:
            window_apply_resize(window,
                                (fdk_i32)event->pointer.position.x,
                                (fdk_i32)event->pointer.position.y);
            return true;
        case FDK_EVENT_POINTER_BUTTON_UP:
            window->resize_edge = FDK_WRES_NONE;
            return true;
        default:
            return true; /* swallow everything else mid-drag */
        }
    }

    if (event->type != FDK_EVENT_POINTER_BUTTON_DOWN ||
        event->pointer_button.button != 1) {
        return false;
    }
    fdk_i32 x = (fdk_i32)event->pointer_button.position.x;
    fdk_i32 y = (fdk_i32)event->pointer_button.position.y;
    fdk_window_resize_edge edge = fdk__window_resize_edge_at(
        window->last_size.width, window->last_size.height, x, y,
        DECO_RESIZE_BORDER);
    if (edge == FDK_WRES_NONE) {
        return false;
    }
    /* Edges that move the window origin need to KNOW the origin; on a
     * backend without window_get_position only the bottom/right edges
     * can resize. */
    bool needs_origin = (edge == FDK_WRES_W || edge == FDK_WRES_NW ||
                         edge == FDK_WRES_SW || edge == FDK_WRES_N ||
                         edge == FDK_WRES_NE);
    if (needs_origin && window->ops->window_get_position == NULL) {
        return false;
    }
    /* Preferred: hand the drag to the WM/compositor. */
    if (window->ops->window_begin_resize != NULL &&
        fdk_ok(window->ops->window_begin_resize(window->pwindow,
                                                (int)edge, x, y))) {
        return true; /* WM drives; we see only configures */
    }
    /* FDK-driven fallback (bare X). The window's real origin is
     * needed even for edges that never MOVE it: the solver outputs
     * the unchanged origin back, and move_resize_to must not "move"
     * the window to a fabricated (0,0). */
    if (window->ops->window_get_position != NULL &&
        !fdk_ok(window->ops->window_get_position(
            window->pwindow, &window->resize_orig_x,
            &window->resize_orig_y))) {
        return false;
    }
    if (window->ops->window_get_position == NULL) {
        if (needs_origin) {
            return false; /* can't drag origin-moving edges blind */
        }
        window->resize_orig_x = 0;
        window->resize_orig_y = 0;
    }
    window->resize_orig_w = window->last_size.width;
    window->resize_orig_h = window->last_size.height;
    window->resize_press_x = x;
    window->resize_press_y = y;
    window->resize_edge = edge;
    return true;
}

void fdk_window_set_event_callback(fdk_window *window,
                                    fdk_event_callback_fn callback,
                                    void *user_data) {
    if (window == NULL) {
        return;
    }
    window->event_callback = callback;
    window->event_callback_user_data = user_data;
}

void fdk_window_dispatch_event(fdk_window *window, const fdk_event_data *event) {
    /* Keep fdk_window_get_size() authoritative without requiring the
     * application to handle FDK_EVENT_WINDOW_CONFIGURE itself just to
     * keep FDK's own bookkeeping in sync. */
    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        window->last_size = event->configure.size;
        if (window->root != NULL) {
            /* The root tracks the window's client size; a resize is a
             * full repaint on both backends (fresh framebuffer). */
            fdk_widget_root_resized(window->root, event->configure.size);
        }
        if (window->decorated) {
            /* The decoration band spans the new width. */
            window_arrange_deco(window);
        }
        if (window->content != NULL) {
            /* Phase 5: the content widget reflows with the window. */
            fdk_window_layout(window);
        }
    } else if (event->type == FDK_EVENT_WINDOW_EXPOSE) {
        if (window->root != NULL) {
            fdk_widget_invalidate_all(window->root);
        }
    } else if (event->type == FDK_EVENT_WINDOW_STATE) {
        /* Cache the reported state (the flags are FDK's truth, not
         * the request) and repaint — the maximize button's glyph
         * swaps between maximize and restore. */
        bool was_max = window->maximized;
        bool was_min = window->minimized;
        window->maximized = event->state.maximized != 0;
        window->minimized = event->state.minimized != 0;
        if ((was_max != window->maximized ||
             was_min != window->minimized) &&
            window->root != NULL) {
            fdk_widget_invalidate_all(window->root);
        }
    } else if (event->type == FDK_EVENT_WINDOW_DECORATION) {
        /* Wayland xdg-decoration override: the compositor insisted on
         * server-side decorations. Drawing FDK's band on top would
         * double-decorate, so tear ours down; the application still
         * receives this event (it may have its own chrome to hide). */
        if (event->decoration.client_side == 0 && window->decorated) {
            (void)fdk_window_set_decorated(window, false);
        }
    }

    /* FDK-drawn resize edges get first claim on pointer events —
     * chrome before content, exactly like a WM frame would. */
    if (window_handle_resize_event(window, event)) {
        return;
    }

    /* Widget trees get first claim on input events (pointer, keys,
     * window focus). Events a widget handles are consumed here and
     * never reach the application's window callback — that is the
     * documented contract of fdk_window_get_root() (see
     * include/fdk/fdk_widget.h). Window-level events (configure,
     * expose, close-request) are never consumed by widgets.
     *
     * A widget handler is allowed to destroy the window (the classic
     * quit button) — which frees this very fdk_window. Cache what the
     * post-routing code needs and re-verify registration before
     * touching `window` again. */
    fdk_context *ctx = window->ctx;
    fdk_platform_window *pwindow = window->pwindow;
    bool handled_by_tree = false;
    if (window->root != NULL) {
        handled_by_tree = fdk_widget_tree_handle_event(window->root, event);
    }

    if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
        return; /* destroyed by a widget handler: nothing left to do */
    }

    if (!handled_by_tree && window->event_callback != NULL) {
        window->event_callback(window, event, window->event_callback_user_data);
    }
}

fdk_result fdk_window_get_root(fdk_window *window, fdk_widget **out_root) {
    if (window == NULL || out_root == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->root == NULL) {
        fdk_rect bounds = {0, 0, window->last_size.width,
                           window->last_size.height};
        fdk_result r = fdk_widget_create(NULL, NULL, bounds, &window->root);
        if (!fdk_ok(r)) {
            return r;
        }
        window->root->flags |= FDK_WF_WINDOW_ROOT;
        FDK_DEBUG("window root widget created (%dx%d)", bounds.width,
                  bounds.height);
    }
    *out_root = window->root;
    return FDK_OK;
}

fdk_result fdk_window_paint(fdk_window *window) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->root == NULL) {
        return FDK_OK; /* no tree: the app drives the surface itself */
    }
    fdk_surface *surface = NULL;
    fdk_result r = fdk_window_get_surface(window, &surface);
    if (!fdk_ok(r)) {
        return r;
    }
    /* A paint hook may destroy the window (freeing the surface with
     * it); cache the context/pwindow and re-verify before presenting. */
    fdk_context *ctx = window->ctx;
    fdk_platform_window *pwindow = window->pwindow;

    /* HiDPI compositing (Phase 3 completion): the widget tree is
     * LOGICAL; the window surface's framebuffer is PHYSICAL (logical
     * x scale — see fdk_window_get_scale). At scale 1 the tree paints
     * straight into the window surface exactly as it always has (the
     * pixel-identical path every existing test pins). At scale > 1
     * the tree paints into a logical-sized ARGB intermediate and is
     * composited on with blit_transformed's exact integer-scale path
     * (nearest-neighbor block scaling — no resampling artifacts);
     * the intermediate's transparent regions let the application's
     * own physical-resolution background show through, preserving
     * the scale-1 layering semantics widget-over-app-background. */
    fdk_f32 scale = 1.0f;
    (void)fdk_window_get_scale(window, &scale);

    if (scale > 1.01f) {
        fdk_i32 lw = window->last_size.width;
        fdk_i32 lh = window->last_size.height;
        if (lw > 0 && lh > 0) {
            if (window->paint_intermediate == NULL ||
                window->paint_intermediate->fb.width != lw ||
                window->paint_intermediate->fb.height != lh) {
                fdk_surface_destroy(window->paint_intermediate);
                window->paint_intermediate = NULL;
                fdk_surface *inter = NULL;
                if (fdk_ok(fdk_surface_create_format(
                        lw, lh, FDK_SURFACE_FORMAT_ARGB8888, &inter))) {
                    window->paint_intermediate = inter;
                }
            }
            if (window->paint_intermediate != NULL) {
                fdk_surface *inter = window->paint_intermediate;
                /* Clear to transparent: this frame's widget coverage
                 * only. (The widgets blend over whatever the app put
                 * in the window surface — same layering as scale 1.) */
                fdk_color transparent = { .r = 0.0f, .g = 0.0f, .b = 0.0f,
                                          .a = 0.0f };
                fdk_surface_fill(inter, transparent);
                fdk_widget_tree_paint(window->root, inter);
                if (fdk_context_find_window_by_pwindow(ctx, pwindow) !=
                    window) {
                    return FDK_OK; /* destroyed mid-paint */
                }
                fdk_surface_blit_transformed(surface,
                                             fdk_matrix_scale(scale), inter);
                return fdk_surface_present(surface);
            }
            /* Intermediate allocation failed: fall through to the
             * direct path (tree squeezed into the top-left of the
             * physical buffer) rather than not painting at all —
             * degraded, logged once by the allocator, honest. */
            FDK_WARN("HiDPI intermediate surface unavailable; painting "
                     "unscaled");
        }
    }

    fdk_widget_tree_paint(window->root, surface);
    if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
        return FDK_OK; /* window destroyed mid-paint; the tree went
                        * with it, nothing to present */
    }
    return fdk_surface_present(surface);
}

/* Is `widget` still a live descendant of `root`? (The content
 * pointer is weak: a destroyed content must silently deactivate.) */
static bool widget_in_tree(fdk_widget *root, fdk_widget *widget) {
    for (fdk_widget *cur = widget; cur != NULL; cur = cur->parent) {
        if (cur == root) {
            return true;
        }
    }
    return false;
}

void fdk_window_set_content(fdk_window *window, fdk_widget *content) {
    if (window == NULL) {
        return;
    }
    if (content != NULL) {
        fdk_widget *root = NULL;
        if (!fdk_ok(fdk_window_get_root(window, &root)) ||
            !widget_in_tree(root, content)) {
            FDK_WARN("set_content: widget is not in the window's tree");
            return;
        }
    }
    window->content = content;
    if (content != NULL) {
        fdk_window_layout(window);
    }
}

/* ---- FDK-drawn decorations (public API) ---- */

static void deco_load_font(fdk_window *window) {
    if (window->deco_font != NULL) {
        return; /* app font borrowed, or already loaded */
    }
    window->deco_font_owned = fdk_font_load_system_default(14);
    window->deco_font = window->deco_font_owned;
    /* NULL (no system font) is legal: the band renders without title
     * text; the loader already warned once. The window buttons are
     * vector glyphs and keep working either way. */
}

fdk_result fdk_window_set_decorated(fdk_window *window, bool decorated) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (decorated == window->decorated) {
        return FDK_OK; /* idempotent */
    }

    if (decorated) {
        if (window->ops->window_set_wm_decorations == NULL) {
            FDK_WARN("set_decorated: this backend cannot drop its own "
                     "decorations; refusing to stack FDK's on top");
            return FDK_ERR_UNSUPPORTED;
        }
        fdk_result r = window->ops->window_set_wm_decorations(
            window->pwindow, false);
        if (!fdk_ok(r)) {
            return r;
        }

        fdk_widget *root = NULL;
        r = fdk_window_get_root(window, &root);
        if (!fdk_ok(r)) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }

        deco_load_font(window);

        r = fdk_widget_create(root, &deco_bar_class,
                              (fdk_rect){0, 0, window->last_size.width,
                                         deco_band_height(window)},
                              &window->deco_bar);
        if (!fdk_ok(r)) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }
        r = fdk_label_create(window->deco_bar, window->deco_font,
                             window->title, &window->deco_title);
        if (!fdk_ok(r)) {
            /* Partial band is worse than none: roll back cleanly. */
            fdk_widget_destroy(window->deco_bar);
            window->deco_bar = NULL;
            window->deco_title = NULL;
            window->ops->window_set_wm_decorations(window->pwindow, true);
            return r;
        }
        /* The band carries its owning window (interaction + the theme
         * hook), and gets the theme-notify hook so a
         * title_bar_height metric change re-arranges it. */
        fdk_widget_set_user_data(window->deco_bar, window);
        fdk__widget_set_theme_hook(window->deco_bar, deco_bar_theme_changed);
        fdk_widget_set_event_callback(window->deco_bar, deco_bar_event,
                                      window);
        window->decorated = true;
        window_arrange_deco(window);
        /* Owning the chrome means owning resize: the WM frame (and
         * its handles) is gone. An app that wants a fixed-size
         * decorated window opts back out explicitly. */
        if (!window->resizable_explicit) {
            window->resizable = true;
        }
        fdk_window_layout(window);
        fdk_widget_invalidate_all(window->root);
        FDK_DEBUG("FDK decorations enabled (WM chrome dropped)");
    } else {
        window->decorated = false;
        window->dragging = false;
        window->deco_pressed = 0;
        window->deco_hover = 0;
        if (window->deco_bar != NULL) {
            fdk__widget_set_theme_hook(window->deco_bar, NULL);
            fdk_widget_destroy(window->deco_bar);
            window->deco_bar = NULL;
            window->deco_title = NULL;
            window->deco_close = NULL;
        }
        if (window->ops->window_set_wm_decorations != NULL) {
            window->ops->window_set_wm_decorations(window->pwindow, true);
        }
        /* Without FDK chrome there is nothing for the edge zones to
         * stand in for — unless the app explicitly asked for them. */
        if (!window->resizable_explicit) {
            window->resizable = false;
        }
        if (window->root != NULL) {
            fdk_window_layout(window);
            fdk_widget_invalidate_all(window->root);
        }
        FDK_DEBUG("FDK decorations disabled (WM chrome restored)");
    }
    return FDK_OK;
}

bool fdk_window_get_decorated(const fdk_window *window) {
    return window != NULL && window->decorated;
}

fdk_result fdk_window_set_decoration_font(fdk_window *window,
                                          fdk_font *font) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Swap the effective font; the owned system default (if any) is
     * kept for a later NULL revert — it is one small face. */
    window->deco_font = font;
    if (window->decorated && window->deco_title != NULL) {
        /* Re-create the label's text layout under the new face by
         * re-setting the text (a no-op change still re-measures) and
         * re-arranging the band geometry. */
        (void)fdk_label_set_text(window->deco_title, window->title);
        window_arrange_deco(window);
    }
    return FDK_OK;
}

void fdk_window_layout(fdk_window *window) {
    if (window == NULL || window->content == NULL ||
        window->root == NULL) {
        return;
    }
    if (!widget_in_tree(window->root, window->content)) {
        /* The content widget was destroyed or reparented away —
         * deactivate the association rather than arrange a stray. */
        window->content = NULL;
        return;
    }
    fdk_rect full = fdk_widget_get_bounds(window->root);
    if (window->decorated && window->deco_bar != NULL) {
        fdk_i32 h = deco_band_height(window);
        full.y = h;
        full.height -= h;
        if (full.height < 0) {
            full.height = 0;
        }
    }
    fdk_widget_arrange(window->content, full);
}
