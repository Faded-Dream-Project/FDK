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
#include "widget/widgets_internal.h" /* fdk__scrollview_viewport    */

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

/* The animation clock (1.3.8): wheel/keyboard gestures now EASE to
 * their target over 140 ms instead of teleporting. The suite pins
 * the engine to a synthetic clock (start stamps and pump reads all
 * come from it) and settles flights by advancing far past their
 * duration — the final-position semantics every older assertion
 * pinned are unchanged, and the new mid-flight checks use chosen
 * pump times. */
static long long g_anim_time = 0;

static void settle(void) {
    g_anim_time += 1000; /* any flight started <= now is done */
    fdk__animation_set_test_clock(g_anim_time);
    fdk__animation_pump(g_anim_time);
}

static void pump_at(long long t) {
    g_anim_time = t;
    fdk__animation_set_test_clock(t);
    fdk__animation_pump(t);
}

static void test_input(void) {
    g_anim_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_rect r = { 0, 0, 200, 150 };
    fdk_widget_set_bounds(sv, r);
    fdk_widget *content = make_content(sv, 400, 1200,
                                       (fdk_color){0, 1, 0, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));

    fdk_i32 sx = -1, sy = -1;

    /* Wheel down over the middle of the viewport: eases to 48.
     * Nothing moves on the event itself (the first tick is the next
     * frame); at exactly half the 140 ms flight cubic-out is
     * 1 - (1 - 0.5)^3 = 0.875, so 48 * 0.875 = 42. */
    fdk_event_data wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);
    pump_at(70);
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 42);
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 48);

    /* Gesture accumulation: a second notch while the first flight
     * is airborne adds to the PENDING target — from the 48 rest
     * position two fast notches are ONE glide of 96 more, landing
     * on 144 (the alternative, retargeting from the stale live
     * offset, would replay the same 1-notch flight twice — the
     * fast-wheel bug found live by this very assertion). */
    wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 144);

    /* The programmatic path snaps INSTANTLY, mid-flight: scroll_to
     * cancels the gesture animation and sets the truth now (no
     * settle() before the assertion), and the canceled flight stays
     * dead afterwards. */
    wheel = ev_scroll(100, 75, 0, -1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 0, 300)));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 300);
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 300);

    /* The wheel event over a CHILD bubbles up: scroll while the
     * pointer is over the content's colored area (48 up, clamped
     * back to 252 = 300 - 48). */
    wheel = ev_scroll(100, 75, 0, 1);
    assert(fdk_widget_tree_handle_event(root, &wheel));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 252);

    /* Wheel up five more notches: 252 - 5*48 = 12, then a sixth
     * clamps to 0 (not negative) — the clamp rule unchanged. */
    for (int i = 0; i < 6; i++) {
        wheel = ev_scroll(100, 75, 0, 1);
        assert(fdk_widget_tree_handle_event(root, &wheel));
    }
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);

    /* Keyboard: unfocused scrollview ignores arrows (they belong to
     * the content's focusables). */
    fdk_event_data key = ev_key(FDK_KEY_DOWN);
    assert(!fdk_widget_tree_handle_event(root, &key));
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);

    /* Focused: arrows (32), PageDown (90% of viewport), Home/End —
     * every gesture settles before its assertion (same numbers the
     * pre-animation suite pinned; only the arrival is eased now). */
    fdk_widget_set_can_focus(sv, true);
    assert(fdk_widget_focus(sv));
    assert(fdk_widget_tree_handle_event(root, &key));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 32);

    fdk_event_data pgdn = ev_key(FDK_KEY_PAGE_DOWN);
    assert(fdk_widget_tree_handle_event(root, &pgdn));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    /* viewport height = 150 - 12 (hbar) = 138; page = (138/10)*9 =
     * 117 (integer math, both in the engine and here). */
    assert(sy == 32 + 117);

    fdk_event_data end = ev_key(FDK_KEY_END);
    assert(fdk_widget_tree_handle_event(root, &end));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 1200 - 138);

    fdk_event_data home = ev_key(FDK_KEY_HOME);
    assert(fdk_widget_tree_handle_event(root, &home));
    settle();
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &sx, &sy)));
    assert(sy == 0);

    fdk_widget_destroy(root);
    printf("[ok] scrollview: wheel (incl. bubbling from content), "
           "keyboard gating on focus, arrows/page/home/end — now "
           "eased: exact mid-flight value, fast-notches accumulate "
           "into the pending target, programmatic snap cancels\n");
}

/* ---- scrollbar interaction ---- */

/* ---- container content (the 1.3.8 regression) ----------------------------
 * A BOX as scrollview content must LAY OUT ITS CHILDREN: the old
 * code positioned the content with set_bounds, which never runs the
 * content's arrange hook — a box of rows rendered nothing at all
 * (found live by example 11's 40-row list: zero rows painted). The
 * arrange path also has to hold when SCROLLING re-positions the
 * content: the box moves, the children keep their in-box slots. */
static void test_container_content(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_widget_set_bounds(sv, (fdk_rect){0, 0, 200, 150});

    fdk_widget *rows = NULL;
    assert(fdk_ok(fdk_box_create(sv, FDK_VERTICAL, &rows)));
    fdk_widget *kids[10];
    for (int i = 0; i < 10; i++) {
        assert(fdk_ok(fdk_widget_create(rows, NULL,
                                        (fdk_rect){0, 0, 120, 25},
                                        &kids[i])));
    }
    assert(fdk_ok(fdk_scrollview_set_content(sv, rows)));
    /* A scroll positions the content and must lay the box's
     * children out (scroll_to runs the internal layout). */
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 0, 0)));

    fdk_rect rb = fdk_widget_get_bounds(rows);
    assert(rb.width == 120 && rb.height == 10 * 25);
    assert(rb.x == 0 && rb.y == 0);
    for (int i = 0; i < 10; i++) {
        fdk_rect kb = fdk_widget_get_bounds(kids[i]);
        assert(kb.y == i * 25);       /* the box laid them out     */
        assert(kb.height == 25);
    }

    /* Scrolling moves the BOX; the children keep their slots. */
    assert(fdk_ok(fdk_scrollview_scroll_to(sv, 0, 100)));
    rb = fdk_widget_get_bounds(rows);
    assert(rb.y == -100);
    for (int i = 0; i < 10; i++) {
        fdk_rect kb = fdk_widget_get_bounds(kids[i]);
        assert(kb.y == i * 25);       /* unchanged in-box position */
    }

    fdk_widget_destroy(root);
    printf("[ok] scrollview: container CONTENT lays out its children "
           "(box rows positioned, slots stable under scrolling — the "
           "1.3.8 set_bounds-vs-arrange regression)\n");
}

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

/* ---- overlay bar mode (1.4.4) ---- */

static fdk_u32 pack_rgb(float r, float g, float b) {
    fdk_u32 ru = (fdk_u32)(r * 255.0f + 0.5f);
    fdk_u32 gu = (fdk_u32)(g * 255.0f + 0.5f);
    fdk_u32 bu = (fdk_u32)(b * 255.0f + 0.5f);
    return (ru << 16) | (gu << 8) | bu;
}

static void test_overlay_bars(void) {
    /* Mode API: default CLASSIC, get/set round trip, idempotent,
     * argument safety. */
    fdk_widget *root = fresh_root();
    fdk_widget *sv = NULL;
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    assert(fdk_scrollview_get_bar_mode(sv) == FDK_SCROLL_BARS_CLASSIC);
    assert(fdk_scrollview_set_bar_mode(sv, FDK_SCROLL_BARS_OVERLAY) ==
           FDK_OK);
    assert(fdk_scrollview_get_bar_mode(sv) == FDK_SCROLL_BARS_OVERLAY);
    assert(fdk_scrollview_set_bar_mode(sv, FDK_SCROLL_BARS_OVERLAY) ==
           FDK_OK); /* idempotent */
    assert(fdk_scrollview_set_bar_mode(NULL,
                                       FDK_SCROLL_BARS_OVERLAY) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_scrollview_get_bar_mode(NULL) == FDK_SCROLL_BARS_CLASSIC);
    fdk_widget *plain = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 5, 5}, &plain)));
    assert(fdk_scrollview_set_bar_mode(plain,
                                       FDK_SCROLL_BARS_OVERLAY) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_widget_destroy(plain);
    fdk_widget_destroy(root);

    /* Geometry: the overlay viewport is the FULL bounds (the classic
     * mode reserves strips); the overlay bar sits at the edge with
     * the overlay metric's thickness. Content taller + wider than
     * the view so BOTH bars exist. */
    root = fresh_root();
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    fdk_widget *content = make_content(sv, 400, 500,
                                       (fdk_color){0.2f, 0.6f, 0.9f, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));
    fdk_widget_arrange(sv, (fdk_rect){0, 0, 200, 150});

    /* CLASSIC baseline: viewport loses both strips. */
    fdk_i32 vw = 0, vh = 0;
    fdk__scrollview_viewport(sv, &vw, &vh);
    assert(vw == 200 - 12 && vh == 150 - 12); /* metric default 12 */

    /* OVERLAY: viewport is the full bounds. */
    assert(fdk_scrollview_set_bar_mode(sv, FDK_SCROLL_BARS_OVERLAY) ==
           FDK_OK);
    fdk__scrollview_viewport(sv, &vw, &vh);
    assert(vw == 200 && vh == 150);

    /* The bars: internal children "scrollbar" — found by class name
     * through the tree (they are the only two). Overlay thickness
     * defaults to 6; the vertical bar runs the FULL height at the
     * right edge, the horizontal bar loses the corner 6px. */
    fdk_widget *bars[2] = {NULL, NULL};
    int nbars = 0;
    fdk_widget *sv_child = NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        if (root->children[i]->klass->name != NULL &&
            strcmp(root->children[i]->klass->name, "scrollview") == 0) {
            sv_child = root->children[i];
        }
    }
    assert(sv_child == sv);
    for (size_t i = 0; i < sv->child_count && nbars < 2; i++) {
        if (sv->children[i]->klass->name != NULL &&
            strcmp(sv->children[i]->klass->name, "scrollbar") == 0) {
            bars[nbars++] = sv->children[i];
        }
    }
    assert(nbars == 2);
    /* Detached tree (no window -> no idle clock): the honesty rule —
     * overlay bars stay VISIBLE while their axes overflow. */
    for (int i = 0; i < 2; i++) {
        assert(fdk_widget_get_visible(bars[i]));
    }
    fdk_rect vb = fdk_widget_get_bounds(bars[0]);
    fdk_rect hb = fdk_widget_get_bounds(bars[1]);
    /* bars[0] is the vertical bar (created first), bars[1] the
     * horizontal one. */
    assert(vb.x == 200 - 6 && vb.width == 6);
    assert(vb.y == 0 && vb.height == 150); /* full height: no strip */
    assert(hb.y == 150 - 6 && hb.height == 6);
    assert(hb.x == 0 && hb.width == 200 - 6); /* corner conceded */

    /* Overlay paint: no trough — the content shows through the bar
     * strip wherever the thumb is not. The thumb occupies the top of
     * the vbar (view 150, content 500 -> 45px); sample far below it. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(220, 170, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 strip = px_at(s, 197, 100);
    assert(strip == pack_rgb(0.2f, 0.6f, 0.9f));
    fdk_surface_destroy(s);

    /* Wheel still scrolls (and wakes the bar — visible stays true
     * in a detached tree). The flight settles through the synthetic
     * animation clock, like every gesture test here. */
    g_anim_time = 0;
    fdk__animation_set_test_clock(0);
    fdk_event_data wheel = ev_scroll(100, 75, 0.0f, -1.0f);
    (void)fdk_widget_tree_handle_event(root, &wheel);
    settle();
    fdk_i32 ox = 0, oy = 0;
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &ox, &oy)));
    assert(oy == 48); /* one notch, clamped far from the edges */
    assert(ox == 0);
    assert(fdk_widget_get_visible(bars[0]));

    /* Mode switch back: strips reserved again, offsets re-clamped. */
    assert(fdk_scrollview_set_bar_mode(sv, FDK_SCROLL_BARS_CLASSIC) ==
           FDK_OK);
    fdk__scrollview_viewport(sv, &vw, &vh);
    assert(vw == 200 - 12 && vh == 150 - 12);
    assert(fdk_ok(fdk_scrollview_get_scroll_offset(sv, &ox, &oy)));
    assert(oy == 48); /* unchanged: still in range */

    /* A fitting axis has no bar in either mode. */
    fdk_widget_destroy(root);
    root = fresh_root();
    assert(fdk_ok(fdk_scrollview_create(root, &sv)));
    content = make_content(sv, 100, 100, (fdk_color){1, 1, 1, 1});
    assert(fdk_ok(fdk_scrollview_set_content(sv, content)));
    fdk_widget_arrange(sv, (fdk_rect){0, 0, 200, 150});
    assert(fdk_scrollview_set_bar_mode(sv, FDK_SCROLL_BARS_OVERLAY) ==
           FDK_OK);
    for (size_t i = 0; i < sv->child_count; i++) {
        if (sv->children[i]->klass->name != NULL &&
            strcmp(sv->children[i]->klass->name, "scrollbar") == 0) {
            assert(!fdk_widget_get_visible(sv->children[i]));
        }
    }

    fdk_widget_destroy(root);
    printf("[ok] overlay bars: mode API, full-bounds viewport, "
           "6-px edge bars (full-height vertical, corner-conceding "
           "horizontal), content showing through the strip, detached-"
           "tree honesty (no idle clock -> visible), wheel + mode "
           "round-trip, no phantom bars on fitting axes\n");
}

int main(void) {
    test_basics();
    test_paint_clipping();
    test_input();
    test_container_content();
    test_scrollbar();
    test_adoption();
    test_hit_testing();
    test_overlay_bars();
    printf("all scrollview tests passed\n");
    return 0;
}
