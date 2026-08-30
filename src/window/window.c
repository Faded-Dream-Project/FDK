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
#include "widget/widgets_internal.h" /* window-root class, a11y name */
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

/* 1.2.1: re-reads the root's DEFAULT window background on a theme
 * switch (defined beside fdk_window_get_root, its only setter). */
static void window_root_theme_changed(fdk_widget *root);

/* Cursor shaping + hover revalidation (1.1.4) — defined above the
 * interactive-resize section, forward-declared here because
 * fdk_window_set_resizable and fdk_window_set_decorated (both earlier
 * in the file) also reset the cursor when the edge zones change. */
static void window_update_cursor(fdk_window *window, fdk_i32 x,
                                 fdk_i32 y);
static void window_reset_cursor(fdk_window *window);
static void window_revalidate_pointer(fdk_window *window);

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

/* Edges whose FDK-driven drag moves the window ORIGIN (every edge
 * touching the top or left side). The compositor-driven path
 * (begin_resize) never needs the origin — the WM owns the geometry —
 * so this only gates the FDK fallback and the cursor affordance.
 * Shared by both so the cursor can never promise what a press
 * cannot deliver (the 1.1.7 top-edge report: cursor said resize,
 * press delivered a move). */
static bool window_edge_needs_origin(fdk_window_resize_edge edge) {
    return edge == FDK_WRES_W || edge == FDK_WRES_NW ||
           edge == FDK_WRES_SW || edge == FDK_WRES_N ||
           edge == FDK_WRES_NE;
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
                /* The WM/compositor now owns the drag AND the pointer
                 * grab — the button release that would normally end
                 * the tree's implicit grab goes to the WM's grab and
                 * never arrives here. Cancel the tree's grab now or
                 * it stays stale forever (press-to-release pairing
                 * broken, hover frozen, and the NEXT press misrouted
                 * to this band instead of its real hit target — a
                 * content click would start a spurious window move). */
                fdk__widget_tree_cancel_grab(w);
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

static fdk_result window_create_full(fdk_context *ctx,
                                     const fdk_window_options *options,
                                     fdk_window *parent,
                                     fdk_window **out_window);

fdk_result fdk_window_create(fdk_context *ctx,
                              const fdk_window_options *options,
                              fdk_window **out_window) {
    return window_create_full(ctx, options, NULL, out_window);
}

fdk_result fdk_window_create_popup(fdk_context *ctx, fdk_window *parent,
                                   fdk_i32 x, fdk_i32 y, fdk_i32 width,
                                   fdk_i32 height,
                                   fdk_window **out_window) {
    if (out_window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (parent == NULL) {
        return FDK_ERR_INVALID_ARGUMENT; /* popups need a parent */
    }
    fdk_window_options options;
    memset(&options, 0, sizeof(options));
    options.popup = 1;
    options.x = x;
    options.y = y;
    options.width = width;
    options.height = height;
    return window_create_full(ctx, &options, parent, out_window);
}

static fdk_result window_create_full(fdk_context *ctx,
                                     const fdk_window_options *options,
                                     fdk_window *parent,
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
    window->auto_paint = false;
    window->destroy_notify = NULL;
    window->destroy_notify_user = NULL;
    window->popup_parent = NULL;
    window->popup_first = NULL;
    window->popup_prev = NULL;
    window->popup_next = NULL;
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
    window->cursor_edge = FDK_WRES_NONE;
    window->is_popup = (options != NULL && options->popup != 0);

    if (options != NULL && options->title != NULL) {
        size_t n = strlen(options->title) + 1;
        window->title = fdk_alloc(n);
        if (window->title != NULL) {
            memcpy(window->title, options->title, n);
        }
    }

    fdk_result r = ctx->ops->window_create(
        ctx->conn, options,
        (parent != NULL) ? parent->pwindow : NULL, &window->pwindow);
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

    /* Popup family bookkeeping: a popup links itself onto its anchor
     * parent's child list, so the parent's destroy sweeps it (the
     * documented "popups must not outlive their parent" contract). */
    if (window->is_popup && parent != NULL) {
        window->popup_parent = parent;
        window->popup_prev = NULL;
        window->popup_next = parent->popup_first;
        if (parent->popup_first != NULL) {
            parent->popup_first->popup_prev = window;
        }
        parent->popup_first = window;
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
    /* Toolkit destroy-notify FIRST, while the window is still whole:
     * holders of borrowed references (the menu session's popups)
     * must drop them before any teardown happens. The notify may
     * NOT destroy this window again (it is already dying). */
    if (window->destroy_notify != NULL) {
        window->destroy_notify(window, window->destroy_notify_user);
    }
    /* Popups must not outlive their parent (fdk_window.h): sweep
     * them first, deepest-first via recursion (each popup's own
     * sweep runs inside its destroy). */
    while (window->popup_first != NULL) {
        fdk_window_destroy(window->popup_first);
    }
    /* Unlink from OUR popup parent (if we are somebody's popup). */
    if (window->popup_parent != NULL) {
        if (window->popup_prev != NULL) {
            window->popup_prev->popup_next = window->popup_next;
        } else {
            window->popup_parent->popup_first = window->popup_next;
        }
        if (window->popup_next != NULL) {
            window->popup_next->popup_prev = window->popup_prev;
        }
    }
    if (window->root != NULL) {
        /* The window owns its root; drop the ownership marker so the
         * widget layer lets us destroy it, then tear the tree down
         * (subclass destroy hooks run, deferred destroys settle).
         * The decoration band is a subtree of the root and dies with
         * it. */
        window->root->window_owner = NULL;
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
    /* A11y: the root's accessible name follows the title (the root
     * may not exist yet — set_title runs before get_root sometimes). */
    if (window->root != NULL) {
        fdk_widget_set_accessible_name(window->root, window->title);
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
        /* And the edge cursor must not outlive the edges it
         * advertises. */
        window_reset_cursor(window);
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

/* ---- Cursor shaping + hover revalidation (1.1.4) ----
 *
 * Two affordances a WM frame gives for free that FDK's chrome must
 * provide itself when it owns the window: the cursor that says
 * "this edge drags" BEFORE any button is held, and hover state that
 * survives the window's own geometry changes. */

/* Applies the cursor shape for a window-local position: over an edge
 * zone (same hit-test the press path uses) the directional resize
 * cursor; anywhere else the default arrow. No-op when the backend
 * cannot shape cursors or the shape has not changed (the platform op
 * only ever sees transitions). */
static void window_update_cursor(fdk_window *window, fdk_i32 x,
                                 fdk_i32 y) {
    if (window->ops->window_set_cursor == NULL) {
        return;
    }
    fdk_window_resize_edge edge = FDK_WRES_NONE;
    if (window->resizable) {
        edge = fdk__window_resize_edge_at(
            window->last_size.width, window->last_size.height, x, y,
            DECO_RESIZE_BORDER);
        /* The affordance must match the press filter exactly: never
         * show a resize cursor for an edge this backend cannot drag.
         * With a compositor-driven begin_resize every edge works; a
         * backend with neither begin_resize nor window_get_position
         * must not advertise its origin-moving edges (the 1.1.7 live
         * report: Wayland showed top-edge resize cursors while the
         * press fell through to the band and MOVED the window). */
        if (edge != FDK_WRES_NONE &&
            window->ops->window_begin_resize == NULL &&
            window_edge_needs_origin(edge) &&
            window->ops->window_get_position == NULL) {
            edge = FDK_WRES_NONE;
        }
    }
    if ((int)edge == window->cursor_edge) {
        return;
    }
    window->cursor_edge = (int)edge;
    window->ops->window_set_cursor(window->pwindow, (int)edge);
}

/* Restores the default arrow unconditionally (pointer left the
 * window, or the edge zones disappeared). */
static void window_reset_cursor(fdk_window *window) {
    if (window->ops->window_set_cursor == NULL ||
        window->cursor_edge == FDK_WRES_NONE) {
        return;
    }
    window->cursor_edge = FDK_WRES_NONE;
    window->ops->window_set_cursor(window->pwindow, (int)FDK_WRES_NONE);
}

/* Hover/cursor revalidation after a geometry change: when the window
 * moves/resizes under a STATIONARY pointer (maximize is the classic
 * case — the maximize button flies right as the window grows), the
 * platform generates no motion event, so every hover state computed
 * against the OLD geometry sticks (a highlight that never clears).
 * Query the REAL pointer position and route it exactly as if a
 * motion/leave had arrived: the tree re-hits, the band re-evaluates
 * its buttons, the cursor re-finds its edge.
 *
 * Routed through the internal seams only — the application's event
 * callback never sees these synthesized positions (it has nothing to
 * learn from a motion it did not cause). */
static void window_revalidate_pointer(fdk_window *window) {
    if (window->ops->window_query_pointer == NULL) {
        return; /* backend cannot answer; hover waits for the next
                   real motion (the pre-1.1.4 behavior) */
    }
    fdk_i32 x = 0;
    fdk_i32 y = 0;
    if (window->ops->window_query_pointer(window->pwindow, &x, &y)) {
        window_update_cursor(window, x, y);
        if (window->root != NULL) {
            fdk_event_data motion;
            memset(&motion, 0, sizeof motion);
            motion.type = FDK_EVENT_POINTER_MOTION;
            motion.pointer.position.x = (fdk_f32)x;
            motion.pointer.position.y = (fdk_f32)y;
            (void)fdk_widget_tree_handle_event(window->root, &motion);
        }
        return;
    }
    /* Pointer is not over the window anymore (the classic unmaximize:
     * the window shrank away from under it): everything hover-shaped
     * goes neutral — the band buttons, the tree's hovered widget, the
     * cursor. The tree's leave routing delivers the LEAVE events its
     * widgets expect; the position fields are meaningless on a
     * synthetic leave and documented as such. */
    if (window->deco_hover != 0) {
        window->deco_hover = 0;
        if (window->deco_bar != NULL) {
            fdk_widget_invalidate(window->deco_bar);
        }
    }
    window_reset_cursor(window);
    if (window->root != NULL) {
        fdk_event_data leave;
        memset(&leave, 0, sizeof leave);
        leave.type = FDK_EVENT_POINTER_LEAVE;
        leave.pointer.position.x = -1.0f;
        leave.pointer.position.y = -1.0f;
        (void)fdk_widget_tree_handle_event(window->root, &leave);
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

    /* Cursor affordance: hovering an edge zone (or entering the
     * window through one) shows the directional resize cursor BEFORE
     * any button is held — the same affordance a WM frame's borders
     * give for free. Skipped mid-drag: the shape the hover set is
     * still correct while the drag runs, and a WM-driven drag has the
     * pointer grabbed anyway. */
    if (window->resize_edge == FDK_WRES_NONE &&
        (event->type == FDK_EVENT_POINTER_MOTION ||
         event->type == FDK_EVENT_POINTER_ENTER)) {
        window_update_cursor(window, (fdk_i32)event->pointer.position.x,
                             (fdk_i32)event->pointer.position.y);
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
            /* Re-anchor the cursor to where the drag ENDED: the edge
             * zones moved with every drag step, so the pre-press
             * shape may no longer match what is under the pointer. */
            window_update_cursor(
                window, (fdk_i32)event->pointer_button.position.x,
                (fdk_i32)event->pointer_button.position.y);
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
    /* Preferred: hand the drag to the WM/compositor — FIRST, because
     * the compositor-driven path needs NO origin knowledge at all:
     * xdg_toplevel.resize / _NET_WM_MOVERESIZE carry an edge and a
     * serial, and the WM owns every pixel of the geometry from
     * there. 1.1.7 live report: this used to run AFTER the origin
     * gate below, so on Wayland — whose ops table has no
     * window_get_position at all — every origin-moving edge (N, NE,
     * NW, W, SW) bailed out of the resize filter and the press fell
     * through to the deco band, which MOVED the window while the
     * cursor had just promised a resize ("it grabs it even tho the
     * cursor to resize shows") — the top edge over the titlebar was
     * the reported case; the left edge was equally dead. */
    if (window->ops->window_begin_resize != NULL &&
        fdk_ok(window->ops->window_begin_resize(window->pwindow,
                                                (int)edge, x, y))) {
        return true; /* WM drives; we see only configures */
    }
    /* FDK-driven fallback (bare X11 without a WM). Edges that move
     * the window origin need to KNOW the origin — on a backend
     * without window_get_position only the bottom/right edges can
     * resize. The origin is fetched even for edges that never MOVE
     * the window: the solver outputs the unchanged origin back, and
     * move_resize_to must not "move" the window to a fabricated
     * (0,0). */
    bool needs_origin = window_edge_needs_origin(edge);
    if (needs_origin && window->ops->window_get_position == NULL) {
        return false;
    }
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
     * keep FDK's own bookkeeping in sync.
     *
     * The 1.1.4 fallout flags (geo_changed / state_flipped /
     * first_expose) mark the events that change what SHOULD be under
     * the pointer or on the screen without generating any further
     * input events of their own; the dispatch tail below uses them
     * for hover revalidation and the synchronous resize repaint. */
    bool geo_changed = false;
    bool state_flipped = false;
    bool first_expose = false;

    if (event->type == FDK_EVENT_WINDOW_CONFIGURE) {
        /* A cached framebuffer acquired at the OLD size is stale now;
         * drop it so the next acquire re-fetches at the new size
         * (without this, a get_info between presents pins an
         * old-size buffer and the window presents the old size
         * forever — see fdk__surface_drop_framebuffer). */
        if (window->surface != NULL) {
            fdk__surface_drop_framebuffer(window->surface);
        }
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
        geo_changed = true;
    } else if (event->type == FDK_EVENT_KEY_DOWN &&
               window->is_popup &&
               event->key.scancode == FDK_KEY_ESC) {
        /* Escape dismisses a popup — the universal dismissal key. It
         * is consumed here so widgets never see it.
         *
         * The recursive dispatch runs the popup's close handlers,
         * and a menu session CLOSES ITS CHAIN on that close request
         * — destroying THIS window before the recursion returns. The
         * first real destroyer found a use-after-free here (the
         * outer frame kept reading the freed window); cache the
         * identity and bail out if we died. */
        fdk_context *esc_ctx = window->ctx;
        fdk_platform_window *esc_pwindow = window->pwindow;
        fdk_event_data close = { .type = FDK_EVENT_WINDOW_CLOSE_REQUEST };
        fdk_window_dispatch_event(window, &close);
        if (fdk_context_find_window_by_pwindow(esc_ctx, esc_pwindow) !=
            window) {
            return; /* destroyed by the close handling */
        }
    } else if (event->type == FDK_EVENT_WINDOW_EXPOSE) {
        if (window->root != NULL) {
            fdk_widget_invalidate_all(window->root);
            /* First map, before anything was ever presented: the
             * window's pixels are still the creation-time background
             * (white — or undefined memory once the app has acquired
             * a framebuffer, whose acquisition flips the background
             * to None; see x11_surface.c). Paint NOW so the first
             * frame the compositor/WM shows carries content instead.
             * Later exposes (unocclusion and friends) keep the app's
             * pacing: they arrive in bursts whose coalescing belongs
             * to the paint loop, and the window already has a frame
             * on screen. */
            first_expose = (fdk__window_ever_presented(window) == 0);
        }
    } else if (event->type == FDK_EVENT_POINTER_LEAVE) {
        /* The pointer left the window: the tree's routing below
         * clears ITS hovered widget, but the band-button hover and
         * the resize-edge cursor are window-layer state that nothing
         * else clears — without this they stuck until the next
         * enter (a highlighted button that "forgot" it was left). */
        if (window->deco_hover != 0) {
            window->deco_hover = 0;
            if (window->deco_bar != NULL) {
                fdk_widget_invalidate(window->deco_bar);
            }
        }
        window_reset_cursor(window);
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
            state_flipped = (was_max != window->maximized);
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

    /* The application's (or a toolkit session's) window callback is
     * allowed to destroy the window — the same contract the widget
     * handlers get (the menu popup callback closes its chain here).
     * Re-verify registration before the auto-paint tail touches it. */
    if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
        return; /* destroyed by the event callback */
    }

    /* ---- 1.1.4→1.2.2: geometry-change fallout ----
     *
     * Events that change what SHOULD be under the pointer or on the
     * screen (configure / state flip / first expose) mark the window
     * for a BATCHED revalidation + repaint instead of performing it
     * inline here. The flush (fdk__window_flush_geo_repaints) runs
     * from the pump the moment this event batch is fully drained.
     *
     * Same-batch rationale (found live, 1.2.2): an interactive resize
     * queues one ConfigureNotify per drag step, each carrying an
     * Expose; repainting inline meant a FULL repaint + framebuffer
     * reallocation + pointer-query round trip PER QUEUED EVENT. The
     * drain rate (~15 events/s under the load) fell below the WM's
     * queueing rate, so the main thread spun at 100% on one core
     * walking a growing backlog of stale sizes — the window stopped
     * updating, title-bar clicks sat behind the backlog, and the CPU
     * never idled ("doesn't update anymore, one core pegged non
     * stop"). Deferring to the batch end paints the FINAL size once.
     *
     * The geo bookkeeping itself (stale framebuffer drop, root
     * resize, band arrange, content reflow) still runs inline — it is
     * cheap, idempotent per event, and keeps fdk_window_get_size()
     * authoritative mid-batch. Only the expensive tail defers, and it
     * defers by microseconds: the pump flushes before it returns to
     * the application, so a lone configure still repaints within its
     * own pump call — the 1.1.4 synchronous-repaint contract (close
     * the resize-to-paint gap to sub-frame) is preserved, just once
     * per batch instead of once per queued event. */
    if (geo_changed || state_flipped || first_expose) {
        window->geo_repaint_pending = true;
    }

    /* Toolkit-owned windows (menu popups, dialogs — see
     * fdk__window_set_auto_paint) keep themselves on screen without
     * the application's help: after an event is fully routed, any
     * pending damage is repainted and presented here. The initial
     * paint rides the EXPOSE every backend dispatches on first map;
     * hover/selection changes ride the events that caused them. The
     * application's own windows never take this path — their loop,
     * their pacing. */
    if (window->auto_paint && window->root != NULL &&
        fdk_widget_tree_has_damage(window->root)) {
        (void)fdk_window_paint(window);
    }
}

fdk_result fdk_window_get_root(fdk_window *window, fdk_widget **out_root) {
    if (window == NULL || out_root == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->root == NULL) {
        fdk_rect bounds = {0, 0, window->last_size.width,
                           window->last_size.height};
        fdk_result r = fdk_widget_create(NULL,
                                         fdk__widget_window_root_class(),
                                         bounds, &window->root);
        if (!fdk_ok(r)) {
            return r;
        }
        window->root->flags |= FDK_WF_WINDOW_ROOT;
        /* 1.2.1 — the window-background DEFAULT.
         *
         * Before this, a window root painted NOTHING unless the
         * application set a background itself, and every stock text
         * surface (Labels, List rows) paints TRANSPARENTLY over
         * whatever the framebuffer already held. Under the retained-
         * buffer damage model both backends use (X11's synced back
         * slot, Wayland's prefetch-visible-frame slots), "whatever the
         * buffer held" is the PREVIOUS FRAME — so changing a label's
         * text, re-listing a directory, or moving a list selection
         * drew the new glyphs straight over the old ones (the live
         * 1.2.0 report: "the old text doesn't get removed", file
         * names stacking on file names, selection bands on selection
         * bands). Examples 03/04/08 had dodged this by setting their
         * own root background; 09/10 exposed the trap.
         *
         * The fix is the default every real toolkit ships: the root
         * fills with the theme's window-background token, so any
         * damaged region is freshly cleared before its widgets draw.
         * An application's explicit fdk_widget_set_background() on the
         * root still wins and survives theme switches (the default
         * flag is cleared there); otherwise the hook below re-reads
         * the token on every default-theme switch, same as the
         * palette the widgets resolve at paint time. */
        window->root->background =
            fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
        window->root->flags |= FDK_WF_ROOT_BG_DEFAULT;
        fdk__widget_set_theme_hook(window->root,
                                   window_root_theme_changed);
        /* Opaque back-edge for Phase 9: widgets (e.g. Entry's clipboard
         * integration) resolve their owning window's context via
         * fdk__widget_window_owner() + fdk__window_context(). The
         * widget layer never dereferences this. */
        window->root->window_owner = window;
        /* A11y: the window's title is the root's accessible name. */
        fdk_widget_set_accessible_name(window->root, window->title);
        FDK_DEBUG("window root widget created (%dx%d)", bounds.width,
                  bounds.height);
    }
    *out_root = window->root;
    return FDK_OK;
}

/* Theme hook for the root default background (1.2.1): re-reads the
 * window-background token on a default-theme switch unless the
 * application overrode it (FDK_WF_ROOT_BG_DEFAULT cleared by
 * fdk_widget_set_background). The invalidating walk that CALLS this
 * hook damages the whole root afterwards, so the new color reaches
 * the screen on the app's next paint — the same contract the palette
 * consumers already ride. */
static void window_root_theme_changed(fdk_widget *root) {
    if ((root->flags & FDK_WF_ROOT_BG_DEFAULT) != 0) {
        root->background = fdk_theme_get_color(NULL,
                                               FDK_TK_WINDOW_BACKGROUND);
    }
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
                        * with it, nothing left to present */
    }
    return fdk_surface_present(surface);
}

void fdk__window_flush_geo_repaints(fdk_context *ctx) {
    if (ctx == NULL) {
        return;
    }

    /* One pass over the window list, one repaint per flagged window.
     * The 1.1.4 fallout tail (hover revalidation + the synchronous
     * geometry repaint), lifted out of per-event dispatch and run at
     * batch end — see window_internal.h's flag comment for the resize
     * backlog this closes.
     *
     * Destroy-safety mirrors the dispatch tail: revalidation routes
     * synthesized pointer events through the tree, and a paint hook
     * may run application code (destroy the window, destroy ANOTHER
     * window, create popups). Every step re-verifies registration by
     * identity, and the index only advances when the slot still
     * holds the window it held on entry — a removal reshuffles the
     * tail into the current slot, and that window must still get its
     * own turn. A window created mid-flush appends past the count
     * observed on entry; the loop condition re-reads window_count,
     * so new tail windows are visited too (their flag is clear —
     * nothing dispatches events inside a flush). */
    size_t i = 0;
    while (i < ctx->window_count) {
        fdk_window *window = ctx->windows[i];
        if (window == NULL || !window->geo_repaint_pending) {
            i++;
            continue;
        }

        /* Clear BEFORE the work: anything the revalidation routing
         * or a paint hook re-flags lands in the NEXT flush (the
         * pump's next turn), never a nested repaint loop. */
        window->geo_repaint_pending = false;
        fdk_platform_window *pwindow = window->pwindow;

        window_revalidate_pointer(window);
        if (fdk_context_find_window_by_pwindow(ctx, pwindow) != window) {
            continue; /* destroyed by revalidation routing; the slot
                         now holds the next window — visit it */
        }

        if (window->root != NULL &&
            fdk_widget_tree_has_damage(window->root)) {
            (void)fdk_window_paint(window);
            if (fdk_context_find_window_by_pwindow(ctx, pwindow) !=
                window) {
                continue; /* destroyed mid-paint (paint hook) */
            }
        }
        i++;
    }
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
        /* The band just appeared (possibly under the pointer, if this
         * ran at runtime): re-derive hover + cursor from where the
         * pointer actually is. */
        window_reset_cursor(window);
        window_revalidate_pointer(window);
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
        /* The edge cursor is chrome: gone with the edges (unless the
         * app kept them — revalidation re-derives it either way). */
        window_reset_cursor(window);
        window_revalidate_pointer(window);
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

fdk_context *fdk__window_context(void *window_owner) {
    return (window_owner != NULL) ? ((fdk_window *)window_owner)->ctx
                                  : NULL;
}

fdk_window *fdk__window_of_owner(void *window_owner) {
    return (fdk_window *)window_owner;
}

void fdk__window_set_auto_paint(fdk_window *window, bool auto_paint) {
    if (window != NULL) {
        window->auto_paint = auto_paint;
    }
}

void fdk__window_set_destroy_notify(fdk_window *window,
                                    void (*notify)(fdk_window *, void *),
                                    void *user) {
    if (window != NULL) {
        window->destroy_notify = notify;
        window->destroy_notify_user = user;
    }
}

void fdk__window_regrab(fdk_window *window) {
    /* Re-asserts a popup's input grab after a popup ABOVE it in the
     * chain was dismissed: X server/compositor grabs do not stack, so
     * the parent's grab did not come back on its own (the menu
     * session calls this when a submenu closes but the parent menu
     * stays open). No-op when the backend has no such op (non-popup
     * windows, Wayland popups without a usable serial). */
    if (window == NULL || !window->is_popup) {
        return;
    }
    if (window->ops->window_popup_regrab != NULL) {
        window->ops->window_popup_regrab(window->pwindow);
    }
}

fdk_result fdk__window_set_modal(fdk_window *window, bool modal) {
    if (window == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (window->ops->window_set_modal == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    return window->ops->window_set_modal(window->pwindow, modal);
}

int fdk__window_ever_presented(const fdk_window *window) {
    if (window == NULL || window->ops == NULL ||
        window->ops->window_ever_presented == NULL) {
        return -1;
    }
    return window->ops->window_ever_presented(window->pwindow);
}

int fdk__window_deco_hover(const fdk_window *window) {
    return (window != NULL) ? window->deco_hover : 0;
}

int fdk__window_cursor_edge(const fdk_window *window) {
    return (window != NULL) ? window->cursor_edge : FDK_WRES_NONE;
}
