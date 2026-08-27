/*
 * test_window_logic.c — headless unit tests for the Phase 8 window
 * management math.
 *
 * The resize-edge classification, the edge-drag geometry solver, and
 * the double-click detector are PURE functions (window_internal.h)
 * precisely so they can be proven with no display at all — they run
 * in plain `make test` alongside the other headless suites.
 *
 * The X11/Wayland behavioral layers on top (EWMH messages, xdg
 * requests, band interactions) are covered by test_x11_integration.c
 * (incl. its fake window manager) and test_wayland_integration.c.
 */

#include "window/window_internal.h"

#include <assert.h>
#include <stdio.h>

static int checks = 0;
#define CHECK(cond)                                            \
    do {                                                       \
        assert(cond);                                          \
        checks++;                                              \
    } while (0)

/* ---- Edge classification ---- */

static void test_edge_classification(void) {
    /* Interior points are never resize zones. */
    CHECK(fdk__window_resize_edge_at(200, 150, 100, 75, 5) ==
          FDK_WRES_NONE);
    CHECK(fdk__window_resize_edge_at(200, 150, 5, 75, 5) ==
          FDK_WRES_NONE); /* exactly past the left zone */
    CHECK(fdk__window_resize_edge_at(200, 150, 194, 75, 5) ==
          FDK_WRES_NONE); /* 194 = first column right of the right zone */

    /* All eight zones, away from the corners. */
    CHECK(fdk__window_resize_edge_at(200, 150, 100, 2, 5) == FDK_WRES_N);
    CHECK(fdk__window_resize_edge_at(200, 150, 100, 148, 5) == FDK_WRES_S);
    CHECK(fdk__window_resize_edge_at(200, 150, 2, 75, 5) == FDK_WRES_W);
    CHECK(fdk__window_resize_edge_at(200, 150, 198, 75, 5) == FDK_WRES_E);

    /* Corners win over edges. */
    CHECK(fdk__window_resize_edge_at(200, 150, 2, 2, 5) == FDK_WRES_NW);
    CHECK(fdk__window_resize_edge_at(200, 150, 198, 2, 5) == FDK_WRES_NE);
    CHECK(fdk__window_resize_edge_at(200, 150, 2, 148, 5) == FDK_WRES_SW);
    CHECK(fdk__window_resize_edge_at(200, 150, 198, 148, 5) ==
          FDK_WRES_SE);

    /* Degenerate window: narrower than twice the border — the point
     * is "near" both edges; the nearer one must win and a corner must
     * never combine opposite sides. */
    CHECK(fdk__window_resize_edge_at(8, 150, 1, 75, 5) == FDK_WRES_W);
    CHECK(fdk__window_resize_edge_at(8, 150, 6, 75, 5) == FDK_WRES_E);
    /* (1,1) is nearest the top-left -> NW is fine, but a 1px-wide
     * window can only degrade, never produce nonsense. */
    fdk_window_resize_edge e =
        fdk__window_resize_edge_at(1, 1, 0, 0, 5);
    CHECK(e == FDK_WRES_NW || e == FDK_WRES_N || e == FDK_WRES_W);

    /* Out-of-bounds points are not drags (the window never sees them
     * in practice; the function is defensive anyway). */
    CHECK(fdk__window_resize_edge_at(200, 150, -1, 75, 5) ==
          FDK_WRES_NONE);
    CHECK(fdk__window_resize_edge_at(200, 150, 100, 200, 5) ==
          FDK_WRES_NONE);

    /* Zero/negative border or window disables the zones entirely. */
    CHECK(fdk__window_resize_edge_at(200, 150, 0, 0, 0) ==
          FDK_WRES_NONE);
    CHECK(fdk__window_resize_edge_at(0, 150, 0, 0, 5) == FDK_WRES_NONE);
    printf("[ok] resize-edge classification (interior, 8 zones, "
           "corners, degenerate widths, out-of-bounds, disabled)\n");
}

/* ---- Edge-drag geometry ---- */

static void test_resize_apply(void) {
    fdk_i32 x = 0, y = 0, w = 0, h = 0;

    /* East: only width changes, origin pinned. */
    fdk__window_resize_apply(FDK_WRES_E, 30, 0, 10, 20, 100, 50, 0, 0,
                             0, 0, &x, &y, &w, &h);
    CHECK(x == 10 && y == 20 && w == 130 && h == 50);

    /* West: width grows leftwards AND the origin tracks so the RIGHT
     * edge stays anchored. */
    fdk__window_resize_apply(FDK_WRES_W, -30, 0, 100, 20, 100, 50, 0, 0,
                             0, 0, &x, &y, &w, &h);
    CHECK(x == 70 && y == 20 && w == 130 && h == 50);

    /* South: only height changes. */
    fdk__window_resize_apply(FDK_WRES_S, 0, 40, 10, 20, 100, 50, 0, 0,
                             0, 0, &x, &y, &w, &h);
    CHECK(x == 10 && y == 20 && w == 100 && h == 90);

    /* North: origin tracks so the BOTTOM stays anchored. */
    fdk__window_resize_apply(FDK_WRES_N, 0, -40, 10, 100, 100, 50, 0, 0,
                             0, 0, &x, &y, &w, &h);
    CHECK(x == 10 && y == 60 && w == 100 && h == 90);

    /* SE corner: both grow, origin pinned. */
    fdk__window_resize_apply(FDK_WRES_SE, 25, 35, 10, 20, 100, 50, 0, 0,
                             0, 0, &x, &y, &w, &h);
    CHECK(x == 10 && y == 20 && w == 125 && h == 85);

    /* NW corner with clamping: dragging the corner far INTO the
     * window (right+down) shrinks it; min clamps then anchor the
     * opposite edges, never collapse the window through them. */
    fdk__window_resize_apply(FDK_WRES_NW, 500, 500, 100, 100, 300,
                             200, 60, 32, 0, 0, &x, &y, &w, &h);
    CHECK(w == 60 && h == 32);
    CHECK(x == 100 + 300 - 60); /* right edge anchored at 400 */
    CHECK(y == 100 + 200 - 32); /* bottom edge anchored at 300 */

    /* Max clamps too. */
    fdk__window_resize_apply(FDK_WRES_E, 500, 0, 0, 0, 100, 50, 0, 0,
                             200, 0, &x, &y, &w, &h);
    CHECK(x == 0 && w == 200);

    /* Degenerate drag to negative sizes without limits: clamped to
     * 1x1, origin keeps the far edges anchored. */
    fdk__window_resize_apply(FDK_WRES_SE, -500, -500, 0, 0, 100, 50, 0,
                             0, 0, 0, &x, &y, &w, &h);
    CHECK(w == 1 && h == 1 && x == 0 && y == 0);

    /* NONE copies through untouched. */
    fdk__window_resize_apply(FDK_WRES_NONE, 100, 100, 11, 22, 33, 44, 0,
                             0, 0, 0, &x, &y, &w, &h);
    CHECK(x == 11 && y == 22 && w == 33 && h == 44);
    printf("[ok] edge-drag geometry (4 edges, corners, min/max "
           "clamps with opposite-edge anchoring, degenerates)\n");
}

/* ---- Double-click detection ---- */

static void test_double_click(void) {
    /* In time + in slop -> double click. */
    CHECK(fdk__window_is_double_click(10000, 9800, 2, 1));
    /* Out of time -> not. */
    CHECK(!fdk__window_is_double_click(10000, 9500, 0, 0));
    /* In time, out of slop -> not. */
    CHECK(!fdk__window_is_double_click(10000, 9800, 6, 0));
    CHECK(!fdk__window_is_double_click(10000, 9800, 0, -6));
    /* Exactly on both limits counts (<=). */
    CHECK(fdk__window_is_double_click(10000, 9600, 5, -5));
    /* Clock going backwards (or a never-set sentinel) -> not. */
    CHECK(!fdk__window_is_double_click(100, 200, 0, 0));
    CHECK(!fdk__window_is_double_click(
        5000, 5000 - (FDK_WINDOW_DBLCLICK_MS + 1), 0, 0));
    printf("[ok] double-click window/slop (boundaries both sides)\n");
}

int main(void) {
    test_edge_classification();
    test_resize_apply();
    test_double_click();
    printf("all window-logic tests passed (%d checks)\n", checks);
    return 0;
}
