/* test_layout.c — headless layout-engine tests (Phase 5).
 *
 * Like the widget suite, everything here runs on standalone roots
 * and offscreen surfaces — layout is pure geometry over the
 * measure/arrange hooks, with zero platform dependence. The window
 * content integration (auto-reflow on configure) is verified by the
 * X11 integration test; these tests prove the engine's math.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_layout.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static fdk_widget *mk_child(fdk_widget *parent, int w, int h) {
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_widget_create(parent, NULL,
                                    (fdk_rect){0, 0, w, h}, &c)));
    return c;
}

static void assert_bounds(fdk_widget *w, int x, int y, int wd, int ht,
                          const char *what) {
    fdk_rect b = fdk_widget_get_bounds(w);
    if (b.x != x || b.y != y || b.width != wd || b.height != ht) {
        fprintf(stderr, "FAIL %s: got (%d,%d,%d,%d) want (%d,%d,%d,%d)\n",
                what, b.x, b.y, b.width, b.height, x, y, wd, ht);
        assert(false);
    }
}

/* ---- measurement (natural size) ---- */

static void test_box_measure(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300}, &root)));
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));

    /* empty box: natural = 0x0 (no padding) */
    fdk_size nat;
    fdk_widget_measure(box, &nat);
    assert(nat.width == 0 && nat.height == 0);

    fdk_widget *a = mk_child(box, 50, 30);
    fdk_widget *b = mk_child(box, 100, 20);
    fdk_widget *c = mk_child(box, 30, 40);

    /* horizontal: width = 50+100+30 + 2*spacing, height = max(30,20,40) */
    fdk_box_set_spacing(box, 10);
    fdk_widget_measure(box, &nat);
    assert(nat.width == 180 + 20);
    assert(nat.height == 40);

    /* padding adds to both axes */
    fdk_box_set_padding(box, 8);
    fdk_widget_measure(box, &nat);
    assert(nat.width == 200 + 16);
    assert(nat.height == 40 + 16);

    /* margins participate: a takes 50+5+5 wide, 30+2+2 tall */
    fdk_widget_set_margin(a, 5, 2, 5, 2);
    fdk_widget_measure(box, &nat);
    assert(nat.width == 200 + 16 + 10);
    assert(nat.height == 40 + 16); /* a: 34 tall < 40 — max wins */

    /* homogeneous: width = 3 * max(60,100,30) + gaps + padding */
    fdk_box_set_homogeneous(box, true);
    fdk_widget_measure(box, &nat);
    assert(nat.width == 300 + 20 + 16);
    assert(nat.height == 40 + 16);

    /* vertical orientation swaps the roles */
    fdk_box_set_orientation(box, FDK_VERTICAL);
    fdk_box_set_homogeneous(box, false);
    fdk_widget_measure(box, &nat);
    assert(nat.width == 100 + 16); /* widest child (b: 100) + padding */
    assert(nat.height == 34 + 20 + 40 + 20 + 16); /* a(34)+b(20)+c(40) */

    fdk_widget_destroy(root);
    printf("[ok] box measure: naturals, spacing, padding, margins, "
           "homogeneous, orientation\n");
}

/* ---- arrangement math ---- */

static void test_box_arrange_horizontal(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300}, &root)));
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));
    fdk_box_set_spacing(box, 10);

    fdk_widget *a = mk_child(box, 50, 30);
    fdk_widget *b = mk_child(box, 100, 20);
    fdk_widget *c = mk_child(box, 30, 40);

    fdk_rect area = {0, 0, 400, 100};
    fdk_widget_arrange(box, area);
    assert_bounds(box, 0, 0, 400, 100, "box itself");

    /* naturals packed at the start: 0..50, 60..160, 170..200; cross
     * default align = FILL? No — default align is (0,0) = FILL. */
    assert_bounds(a, 0, 0, 50, 100, "a fills cross");
    assert_bounds(b, 60, 0, 100, 100, "b fills cross");
    assert_bounds(c, 170, 0, 30, 100, "c fills cross");

    /* one expander absorbs the leftover: 400 - 180(naturals) - 20
     * (gaps) = 200 extra for b alone -> 100+200 = 300 wide */
    fdk_widget_set_expand(b, true, false);
    assert_bounds(a, 0, 0, 50, 100, "a");
    assert_bounds(b, 60, 0, 300, 100, "b expanded");
    assert_bounds(c, 370, 0, 30, 100, "c after expansion");

    /* cross-align START keeps natural height at the top */
    fdk_widget_set_align(a, FDK_ALIGN_FILL, FDK_ALIGN_START);
    assert_bounds(a, 0, 0, 50, 30, "a start-aligned cross");
    fdk_widget_set_align(a, FDK_ALIGN_FILL, FDK_ALIGN_CENTER);
    assert_bounds(a, 0, 35, 50, 30, "a centered cross");
    fdk_widget_set_align(a, FDK_ALIGN_FILL, FDK_ALIGN_END);
    assert_bounds(a, 0, 70, 50, 30, "a end-aligned cross");

    /* cross-expand fills the axis regardless of align */
    fdk_widget_set_expand(a, false, true);
    assert_bounds(a, 0, 0, 50, 100, "a cross-expanded");

    fdk_widget_destroy(root);
    printf("[ok] horizontal arrange: packing, expansion, cross align, "
           "cross expand\n");
}

static void test_box_arrange_vertical_margins(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 200, 400}, &root)));
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &box)));
    fdk_box_set_padding(box, 6);
    fdk_box_set_spacing(box, 4);

    fdk_widget *a = mk_child(box, 40, 30);
    fdk_widget_set_margin(a, 3, 5, 3, 5); /* slot: 46 wide, 40 tall   */
    fdk_widget *b = mk_child(box, 20, 50);

    fdk_rect area = {10, 20, 100, 200};
    fdk_widget_arrange(box, area);

    /* content starts at (16,26); a's slot is 40 tall (30+10 margins),
     * its bounds inset by margins: y 31..61. Cross align defaults to
     * FILL: a spans the content width (88) minus its side margins. */
    assert_bounds(a, 19, 31, 82, 30, "a fills cross, margins inset");
    /* b starts after a's slot + spacing: 26+40+4 = 70; its cross
     * align is START so it keeps its natural 20-wide size. */
    fdk_widget_set_align(b, FDK_ALIGN_START, FDK_ALIGN_START);
    assert_bounds(b, 16, 70, 20, 50, "b start-aligned cross");

    /* along-axis expand with margins: b grows, its bounds grow by the
     * same amount (margins fixed) */
    fdk_widget_set_expand(b, false, true);
    fdk_rect bb = fdk_widget_get_bounds(b);
    assert(bb.height > 50);
    assert(fdk_widget_get_bounds(a).height == 30); /* a untouched */

    fdk_widget_destroy(root);
    printf("[ok] vertical arrange: padding, margins inside slots, "
           "along-axis expansion\n");
}

static void test_box_homogeneous_distribution(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 100}, &root)));
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));

    fdk_widget *a = mk_child(box, 50, 30);
    fdk_widget *b = mk_child(box, 100, 30);
    fdk_widget *c = mk_child(box, 30, 30);

    fdk_box_set_homogeneous(box, true);
    fdk_rect area = {0, 0, 300, 60};
    fdk_widget_arrange(box, area);

    /* 3 equal slots of 100 (no spacing) */
    assert_bounds(a, 0, 0, 100, 60, "a equal share");
    assert_bounds(b, 100, 0, 100, 60, "b equal share");
    assert_bounds(c, 200, 0, 100, 60, "c equal share");

    fdk_widget_destroy(root);
    printf("[ok] homogeneous equal distribution\n");
}

static void test_dynamic_children_relayout(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 100}, &root)));
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));

    fdk_widget *a = mk_child(box, 100, 30);
    fdk_widget *b = mk_child(box, 100, 30);
    fdk_rect area = {0, 0, 200, 50};
    fdk_widget_arrange(box, area);
    assert_bounds(a, 0, 0, 100, 50, "a");
    assert_bounds(b, 100, 0, 100, 50, "b");

    /* adding a child relayouts immediately: three-way split */
    fdk_widget *c = mk_child(box, 100, 30);
    (void)c;
    assert_bounds(a, 0, 0, 100, 50, "a after add");
    assert_bounds(b, 100, 0, 100, 50, "b after add");
    fdk_rect cb = fdk_widget_get_bounds(fdk_widget_child_at(box, 2));
    assert(cb.x == 200 && cb.width == 100);

    /* destroying the middle child closes the gap */
    fdk_widget_destroy(b);
    assert_bounds(a, 0, 0, 100, 50, "a after remove");
    fdk_rect nb = fdk_widget_get_bounds(fdk_widget_child_at(box, 1));
    assert(nb.x == 100 && nb.width == 100);

    /* hiding takes the child out of the layout; showing restores it */
    fdk_widget_set_visible(a, false);
    nb = fdk_widget_get_bounds(fdk_widget_child_at(box, 0));
    assert(nb.x == 0); /* survivor moved to the start */
    fdk_widget_set_visible(a, true);
    assert_bounds(a, 0, 0, 100, 50, "a re-shown");

    /* hint changes relayout: shrink a's REQUEST (the new Phase 5 API)
     * and expand the survivor — it absorbs the freed space. */
    fdk_widget *survivor = fdk_widget_child_at(box, 1);
    fdk_widget_set_natural_size(a, 40, 30);
    fdk_widget_set_expand(survivor, true, false);
    fdk_rect sb = fdk_widget_get_bounds(survivor);
    assert(sb.width == 160); /* 100 natural + (200 - 40 - 100) leftover */

    fdk_widget_destroy(root);
    printf("[ok] dynamic relayout: add, remove, hide/show, hint change\n");
}

static void test_nested_boxes(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 200}, &root)));
    fdk_widget *vbox = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &vbox)));

    fdk_widget *header = mk_child(vbox, 0, 30);
    fdk_widget_set_expand(header, false, false);
    fdk_widget *hbox = NULL;
    assert(fdk_ok(fdk_box_create(vbox, FDK_HORIZONTAL, &hbox)));
    fdk_widget_set_expand(hbox, false, true);
    fdk_widget *footer = mk_child(vbox, 0, 20);

    fdk_widget *left = mk_child(hbox, 0, 0);
    fdk_widget_set_expand(left, true, false);
    fdk_widget *right = mk_child(hbox, 100, 0);
    fdk_widget_set_align(right, FDK_ALIGN_FILL, FDK_ALIGN_FILL);

    fdk_rect area = {0, 0, 300, 200};
    fdk_widget_arrange(vbox, area);

    /* header 30, footer 20, hbox expands to 150; left expands to 200,
     * right fixed at 100 */
    assert_bounds(header, 0, 0, 300, 30, "header");
    assert_bounds(hbox, 0, 30, 300, 150, "hbox cross-expanded");
    assert_bounds(footer, 0, 180, 300, 20, "footer");
    assert_bounds(left, 0, 30, 200, 150, "left expanded along, fills hbox");
    assert_bounds(right, 200, 30, 100, 150, "right fixed, fills hbox");

    fdk_widget_destroy(root);
    printf("[ok] nested boxes: vertical containing horizontal, "
           "recursive expand\n");
}

static void test_layout_paints(void) {
    /* Layout + painting together: a laid-out tree must paint exactly
     * into the computed slots (geometry and pixels agree). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 100, &s)));

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 100}, &root)));
    fdk_widget_set_background(root, (fdk_color){0, 0, 0, 1});
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));

    fdk_widget *a = mk_child(box, 100, 100);
    fdk_widget_set_background(a, (fdk_color){1, 0, 0, 1});
    fdk_widget *b = mk_child(box, 100, 100);
    fdk_widget_set_background(b, (fdk_color){0, 1, 0, 1});
    fdk_widget *c = mk_child(box, 100, 100);
    fdk_widget_set_background(c, (fdk_color){0, 0, 1, 1});

    fdk_rect area = {0, 0, 300, 100};
    fdk_widget_arrange(box, area); /* also the box's slot in the root? */
    /* the box is the root's child but nothing arranged the ROOT's
     * children — arrange the box directly gave it (0,0,300,100) ✓ */

    fdk_widget_tree_paint(root, s);

    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    fdk_u32 px = info.pixels[50 * info.stride + 50] & 0xFFFFFFu;
    assert(px == 0xFF0000u);
    px = info.pixels[50 * info.stride + 150] & 0xFFFFFFu;
    assert(px == 0x00FF00u);
    px = info.pixels[50 * info.stride + 250] & 0xFFFFFFu;
    assert(px == 0x0000FFu);
    /* boundary pixels: at x=100 the green starts (a is [0,100)) */
    px = info.pixels[50 * info.stride + 99] & 0xFFFFFFu;
    assert(px == 0xFF0000u);
    px = info.pixels[50 * info.stride + 100] & 0xFFFFFFu;
    assert(px == 0x00FF00u);

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] laid-out tree paints exactly into its slots\n");
}

static void test_argument_safety(void) {
    /* non-box widgets reject box setters (no crash, no effect) */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &root)));
    fdk_box_set_spacing(root, 5);   /* not a box: ignored */
    fdk_box_set_padding(root, 5);
    fdk_box_set_homogeneous(root, true);
    fdk_box_set_orientation(root, FDK_VERTICAL);
    assert(fdk_box_get_orientation(root) == (fdk_orientation)0);
    assert_bounds(root, 0, 0, 10, 10, "root untouched");

    /* bad arguments */
    fdk_widget *b = NULL;
    assert(fdk_box_create(NULL, (fdk_orientation)99, &b) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_box_create(NULL, FDK_HORIZONTAL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* clamps: negative spacing/padding/margins clamp to 0 */
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));
    fdk_box_set_spacing(box, -5);
    fdk_box_set_padding(box, -5);
    fdk_widget *c = mk_child(box, 10, 10);
    fdk_widget_set_margin(c, -1, -1, -1, -1);
    fdk_rect area = {0, 0, 50, 20};
    fdk_widget_arrange(box, area);
    assert_bounds(c, 0, 0, 10, 20, "clamped hints layout cleanly");
    /* (cross defaults to FILL: c spans the box height) */

    fdk_widget_destroy(root);
    printf("[ok] argument safety: non-box targets, clamps, invalid args\n");
}

int main(void) {
    test_box_measure();
    test_box_arrange_horizontal();
    test_box_arrange_vertical_margins();
    test_box_homogeneous_distribution();
    test_dynamic_children_relayout();
    test_nested_boxes();
    test_layout_paints();
    test_argument_safety();
    printf("all headless layout tests passed\n");
    return 0;
}
