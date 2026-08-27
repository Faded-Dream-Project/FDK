/*
 * test_scroll.c — headless tests for the Phase 9 ScrollView
 *
 * Standalone roots + offscreen surfaces + synthetic events (the
 * established discipline). A content of KNOWN geometry (a plain
 * widget with an explicit natural size via set_natural_size) drives
 * everything deterministic:
 *   - content placement at (-x, -y) and viewport clipping (pixels
 *     outside the viewport never ink, even though the content child
 *     extends below/right of the scrollview's bounds)
 *   - scroll_to clamping (negative and past-the-end offsets)
 *   - wheel scrolling (FDK_WIDGET_SCROLL through the tree)
 *   - keyboard scrolling (focused scrollview)
 *   - scrollbar auto-visibility (hidden when content fits)
 *   - thumb drag + trough paging through the tree (implicit grab)
 *   - hit-testing through the scrolled content (clicks map to the
 *     scrolled-in position)
 *   - natural-size measurement follows the content
 *   - argument safety + the no-scrollbar-adoption rule
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widget_internal.h" /* fdk_widget internals: ->parent */

#include <assert.h>
#include <stdio.h>
#include <string.h>

static fdk_event_data ev_button(fdk_event_type t, float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    return e;
}

static fdk_event_data ev_scroll(float x, float y, float dx, float dy) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_SCROLL;
    e.scroll.position.x = x;
    e.scroll.position.y = y;
    e.scroll.delta_x = dx;
    e.scroll.delta_y = dy;
    return e;
}

static fdk_event_data ev_key(fdk_scancode sc) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_KEY_DOWN;
    e.key.scancode = sc;
    return e;
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

/* A content widget of explicit natural size with a solid color. */
static fdk_widget *make_content(fdk_widget *parent, int w, int h,
                                fdk_color c) {
    fdk_widget *c1 = NULL;
    assert(fdk_ok(fdk_widget_create(parent, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &c1)));
    fdk_widget_set_natural_size(c1, w, h);
    fdk_widget_set_background(c1, c);
    return c1;
}

static void click(fdk_widget *root, float x, float y) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

/* ---- basics ---- */

static void test_basics(void) {
    assert(fdk_scrollview_create(NULL, NULL) == FDK_ERR_INVALID_ARGUMENT);

    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));

    /* Natural size follows the content. */
    fdk_widget *content = make_content(sv, 500, 900,
                                       (fdk_color){1, 0, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));
    fdk_size nat = { 0, 0 };
    fdk_widget_measure(sv, &nat);
    assert(nat.width == 500 && nat.height == 900);

    /* Assign a smaller viewport. */
    fdk_rect r = { 0, 0, 200, 150 };
    fdk_widget_set_bounds(sv, r);

    /* Clamping: negative and past-end offsets. */
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, -100, -100)));
    fdk_i32 sx = -1, sy = -1;
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sx == 0 && sy == 0);

    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 10000, 10000)));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    /* max x = 500 - (200 - bar) ... content taller than viewport ->
     * vbar visible; wider too -> hbar visible; classic L-shape math
     * (bar width 12 default). */
    assert(sx == 500 - (200 - 12));
    assert(sy == 900 - (150 - 12));

    /* Content placed at the negative offset. */
    fdk_rect cb = fdk_widget_get_bounds(content);
    assert(cb.x == -sx && cb.y == -sy);
    assert(cb.width == 500 && cb.height == 900);

    /* get on a non-scrollview refused. */
    assert(fdk_scrollview_get_scroll_offset(content, &sx, &sy) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_scrollview_scroll_to(content, 0, 0) ==
           FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    printf("[ok] scrollview: natural size, clamping (L-shape math), "
           "content offset placement, type checks\n");
}

/* ---- clipping + paint ---- */

static void test_paint_clipping(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_rect r = { 10, 10, 100, 80 };
    fdk_widget_set_bounds(sv, r);

    /* Two stacked bands inside the content: red top half, blue
     * bottom half. Content 100x200 (viewport shows 100x68-ish). */
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(sv, NULL,
                                    (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_natural_size(content, 100, 200);
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));
    fdk_widget *top = make_content(content, 100, 100,
                                    (fdk_color){1, 0, 0, 1});
    fdk_widget *bottom = make_content(content, 100, 100,
                                       (fdk_color){0, 0, 1, 1});
    fdk_widget_set_bounds(top, (fdk_rect){0, 0, 100, 100});
    fdk_widget_set_bounds(bottom, (fdk_rect){0, 100, 100, 100});

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(130, 110, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);

    /* At scroll 0: red at the top of the viewport, blue below the
     * fold is CLIPPED (the content extends to y=200 but the
     * viewport's clip must keep it invisible). */
    int red = (px_at(s, 60, 20) >> 16) & 0xFF; /* deep in viewport top */
    assert(red > 200);
    /* Below the scrollview's bottom edge (y >= 90): nothing painted
     * by it at all — the ROOT's background (transparent -> surface
     * clear color 0) shows. */
    assert(px_at(s, 60, 100) == 0x000000);

    /* Scroll down 100: blue now at the viewport top. */
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 0, 100)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int blue = px_at(s, 60, 20) & 0xFF;
    assert(blue > 200);
    /* Above the viewport (y < 10): still nothing. */
    assert(px_at(s, 60, 5) == 0x000000);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] scrollview: viewport clips the content at paint "
           "time; scrolling swaps visible bands\n");
}

/* ---- wheel + keyboard ---- */

static void test_input(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_rect r = { 0, 0, 200, 150 };
    fdk_widget_set_bounds(sv, r);
    fdk_widget *content = make_content(sv, 400, 1200,
                                       (fdk_color){0, 1, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));

    fdk_i32 sx = -1, sy = -1;

    /* Wheel down over the middle of the viewport: 48px. */
    fdk_event_data wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 48);

    /* Wheel up twice: back to 0 (clamped, not negative). */
    wheel = ev_scroll(100, 75, 0, 1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);

    /* The wheel event over a CHILD bubbles up: scroll while the
     * pointer is over the content's colored area. */
    wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 48);

    /* Keyboard: unfocused scrollview ignores arrows (they belong to
     * the content's focusables). */
    fdk_event_data key = ev_key(FDK_KEY_DOWN);
    assert(!fdk_widget_tree_handle_event(root, &key));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 48);

    /* Focused: arrows (32), PageDown (90% of viewport), Home/End. */
    fdk_widget_set_can_focus(sv, true);
    assert(fdk_widget_focus(sv));
    assert(fdk_widget_tree_handle_event(root, &key));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 80);

    fdk_event_data pgdn = ev_key(FDK_KEY_PAGE_DOWN);
    assert(fdk_widget_tree_handle_event(root, &pgdn));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    /* viewport height = 150 - 12 (hbar) = 138; page = (138/10)*9 =
     * 117 (integer math, both in the engine and here). */
    assert(sy == 80 + 117);

    fdk_event_data end = ev_key(FDK_KEY_END);
    assert(fdk_widget_tree_handle_event(root, &end));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 1200 - 138);

    fdk_event_data home = ev_key(FDK_KEY_HOME);
    assert(fdk_widget_tree_handle_event(root, &home));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);

    fdk_widget_destroy(root);
    printf("[ok] scrollview: wheel (incl. bubbling from content), "
           "keyboard gating on focus, arrows/page/home/end\n");
}

/* ---- scrollbar interaction ---- */

static void test_scrollbar(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_rect r = { 0, 0, 200, 150 };
    fdk_widget_set_bounds(sv, r);
    fdk_widget *content = make_content(sv, 300, 1000,
                                       (fdk_color){1, 1, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));

    /* Auto-visibility: vbar visible (1000 > viewport), hbar visible
     * (300 > 200). Z-order after the layout's raise()s: content at
     * the bottom, then vbar, then hbar on top — the bars must sit
     * ABOVE the content for their strips to hit-test. */
    assert(fdk_widget_child_count(sv) == 3);
    fdk_widget *vbar = fdk_widget_child_at(sv, 1);
    fdk_widget *hbar = fdk_widget_child_at(sv, 2);
    assert(fdk_widget_child_at(sv, 0) == content);
    assert(fdk_widget_get_visible(vbar));
    assert(fdk_widget_get_visible(hbar));

    /* Bar geometry: vbar at the right edge, width = themed 12. */
    fdk_rect vb = fdk_widget_get_bounds(vbar);
    assert(vb.x == 200 - 12 && vb.width == 12);
    assert(vb.y == 0);

    /* Content that fits: bars hidden, no scroll possible. */
    fdk_widget *sv2 = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv2)));
    fdk_widget_set_bounds(sv2, (fdk_rect){0, 160, 200, 150});
    fdk_widget *c2 = make_content(sv2, 100, 100, (fdk_color){0, 1, 1, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv2, c2)));
    assert(!fdk_widget_get_visible(fdk_widget_child_at(sv2, 1)));
    assert(!fdk_widget_get_visible(fdk_widget_child_at(sv2, 2)));
    assert(fdk_ok(fdk_scrollview_scroll_to(sv2, 10, 10)));
    fdk_i32 sx = -1, sy = -1;
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv2, &sx, &sy)));
    assert(sx == 0 && sy == 0);

    /* Trough click: paging. The vbar of sv: x in [188, 200), thumb
     * at scroll 0 is at pos 0 (top). Click BELOW the thumb (e.g.
     * y=100) -> page down. */
    fdk_i32 viewport_h = 150 - 12;
    fdk_i32 page = (viewport_h / 10) * 9;
    click(root, 194, 100);
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == page);

    /* Thumb drag: press on the thumb, move, release. Mirror the
     * engine's integer math exactly (bar_thumb):
     *   trough = vh = 138; thumb = max(24, 138*138/1000=19) = 24;
     *   range  = 138 - 24 = 114; max_scroll = 1000 - 138 = 862;
     *   pos(before drag, sy=117) = 114*117/862 = 15.
     * Press at y=24 (on the thumb 15..39), drag to y=81:
     *   grab = 24-15 = 9; want = 81-9 = 72; scroll = 72*862/114. */
    {
        int trough = viewport_h;
        int thumb = trough * viewport_h / 1000;
        if (thumb < 24) {
            thumb = 24;
        }
        int range = trough - thumb;
        int max_scroll = 1000 - viewport_h;
        int pos_before = range * 117 / max_scroll;
        int grab = 24 - pos_before;
        int expect = ((81 - grab) * max_scroll) / range;

        fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                        194, 24);
        assert(fdk_widget_tree_handle_event(root, &down));
        fdk_event_data motion = {
            .type = FDK_EVENT_POINTER_MOTION,
        };
        motion.pointer.position.x = 194;
        motion.pointer.position.y = 81;
        assert(fdk_widget_tree_handle_event(root, &motion));
        fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP,
                                      194, 81);
        assert(fdk_widget_tree_handle_event(root, &up));
        assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
        assert(sy == expect);
    }

    fdk_widget_destroy(root);
    printf("[ok] scrollview: bar auto-visibility, edge geometry, "
           "trough paging, thumb drag math\n");
}

/* ---- adoption rules ---- */

static void test_adoption(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_widget_set_bounds(sv, (fdk_rect){0, 0, 100, 100});

    fdk_widget *content = make_content(sv, 400, 400,
                                       (fdk_color){1, 0, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));

    /* Adopting a scrollbar is refused. */
    fdk_widget *bar = fdk_widget_child_at(sv, 1);
    assert(fdk_scrollview_set_content(sv, bar) ==
           FDK_ERR_INVALID_ARGUMENT);
    /* (unchanged: content is still the content) */
    assert(fdk_widget_child_at(sv, 0) == content);

    /* set_content replaces (destroys) the old content. */
    fdk_widget *c2 = make_content(root, 50, 50, (fdk_color){0, 1, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, c2)));
    /* c2 reparented into sv; old content destroyed. Child count:
     * 2 bars + c2. */
    assert(fdk_widget_child_count(sv) == 3);
    assert(c2->parent == sv);
    /* No overflow: bars hidden again. */
    assert(!fdk_widget_get_visible(fdk_widget_child_at(sv, 1)));

    /* NULL clears (destroys) the content. */
    assert(fdk_ok(fdk_scrollview_set_content(sv, NULL)));
    assert(fdk_widget_child_count(sv) == 2);

    fdk_widget_destroy(root);
    printf("[ok] scrollview: adoption rules (no scrollbars, replace "
           "destroys, NULL clears)\n");
}

/* ---- hit-testing through the scroll ---- */

static void test_hit_testing(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_widget_set_bounds(sv, (fdk_rect){0, 0, 200, 150});

    /* Content with two buttons at known positions. */
    fdk_widget *content = NULL;
    assert(fdk_ok(fdk_widget_create(sv, NULL,
                                    (fdk_rect){0, 0, 10, 10},
                                    &content)));
    fdk_widget_set_natural_size(content, 200, 600);
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));

    fdk_font *font = fdk_font_load_system_default(14);
    fdk_widget *b1 = NULL;
    fdk_widget *b2 = NULL;
    assert(fdk_ok(fdk_button_create(content, font, "one", &b1)));
    assert(fdk_ok(fdk_button_create(content, font, "two", &b2)));
    fdk_widget_set_bounds(b1, (fdk_rect){10, 10, 60, 30});
    fdk_widget_set_bounds(b2, (fdk_rect){10, 400, 60, 30});

    static int hits = 0;
    fdk_button_set_on_activate(b2, NULL, NULL);
    /* Simpler observation: clicking where b2 IS after scrolling hits
     * b2 (checked via focus: the click focuses the button). */

    /* Scroll so b2's row (y=400..430) lands at viewport y=20:
     * scroll_y = 380. Verify via HOVER (the tree's hit-test
     * observable): a motion at viewport (30,35) maps to content
     * (30,415) — inside b2. */
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 0, 380)));
    fdk_event_data motion = {
        .type = FDK_EVENT_POINTER_MOTION,
    };
    motion.pointer.position.x = 30;
    motion.pointer.position.y = 35;
    (void)fdk_widget_tree_handle_event(root, &motion);
    assert(fdk_widget_is_hovered(b2));
    assert(!fdk_widget_is_hovered(b1));

    /* b1 scrolled out of the viewport: a motion over its WOULD-BE
     * position (viewport y=15 -> content y=395, which is b2's gap
     * area, not b1) must not hover b1. */
    motion.pointer.position.x = 30;
    motion.pointer.position.y = 15;
    (void)fdk_widget_tree_handle_event(root, &motion);
    assert(!fdk_widget_is_hovered(b1));
    assert(fdk_widget_is_hovered(content));

    fdk_font_destroy(font);
    fdk_widget_destroy(root);
    (void)hits;
    printf("[ok] scrollview: hit-testing maps through the scroll "
           "offset (scrolled-in button receives the click)\n");
}

int main(void) {
    test_basics();
    test_paint_clipping();
    test_input();
    test_scrollbar();
    test_adoption();
    test_hit_testing();
    printf("all scrollview tests passed\n");
    return 0;
}
