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
#include "widget/widget_internal.h" /* fdk__widget_set_baseline */

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
    (void)b; /* measured through the box, never addressed again */
    (void)c;

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

    /* Bounds are PARENT-RELATIVE to the box (the core contract), so
     * the box sitting at (10,20) does NOT shift its children: content
     * starts at (6,6) IN BOX SPACE; a's slot is 40 tall (30+10
     * margins), its bounds inset by margins: y 11..41. Cross align
     * defaults to FILL: a spans the content width (88) minus its side
     * margins. (The pre-Phase-6 engine baked the box's own position
     * into children — double-offsetting them in absolute space.) */
    assert_bounds(a, 9, 11, 82, 30, "a fills cross, margins inset");
    /* b starts after a's slot + spacing: 6+40+4 = 50 in box space;
     * its cross align is START so it keeps its natural 20-wide size. */
    fdk_widget_set_align(b, FDK_ALIGN_START, FDK_ALIGN_START);
    assert_bounds(b, 6, 50, 20, 50, "b start-aligned cross");

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
    /* left/right are hbox's children: parent-relative to the hbox,
     * which itself sits at (0,30) — the position lives in the hbox's
     * own bounds, not its children's. */
    assert_bounds(left, 0, 0, 200, 150, "left expanded along, fills hbox");
    assert_bounds(right, 200, 0, 100, 150, "right fixed, fills hbox");

    fdk_widget_destroy(root);
    printf("[ok] nested boxes: vertical containing horizontal, "
           "recursive expand\n");
}

static void test_nested_child_change_propagation(void) {
    /* Regression (found live by the 07 text-layout demo): a child
     * change inside a NESTED container must relayout that container
     * AND every ancestor that sized it — frames included (box-ness
     * is hook delegation, not class identity). Before the fix,
     * outer [ frame [ children-added-later ] ] left the frame at
     * its empty natural forever. Fontless frame: no title band, so
     * the numbers are font-independent. */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 300, 200}, &root)));
    fdk_widget *outer = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &outer)));
    fdk_widget_arrange(outer, (fdk_rect){0, 0, 300, 200});

    fdk_widget *frame = NULL;
    assert(fdk_ok(fdk_frame_create(outer, NULL, "F", &frame)));
    fdk_widget *footer = mk_child(outer, 0, 20);

    /* Empty frame natural: padding 10 * 2, no title (no font). */
    assert_bounds(frame, 0, 0, 300, 20, "empty frame");

    /* Children added AFTER the frame was sized: the frame relayouts
     * itself AND outer re-runs (frame natural 20 -> 98, footer
     * moves down). */
    fdk_widget *a = mk_child(frame, 120, 30);
    fdk_widget *b = mk_child(frame, 120, 40);
    assert_bounds(frame, 0, 0, 300, 98, "frame grew with children");
    /* cross-axis default is FILL: children stretch to the frame's
     * inner width (300 - 2*10) at their natural heights */
    assert_bounds(a, 10, 10, 280, 30, "frame child a slotted");
    assert_bounds(b, 10, 48, 280, 40, "frame child b slotted");
    assert_bounds(footer, 0, 98, 300, 20, "footer pushed down");

    /* Box setters now reach frames too (same delegation fix — they
     * used to be silent no-ops on any box subclass). */
    fdk_box_set_spacing(frame, 4);
    assert_bounds(b, 10, 44, 280, 40, "frame spacing setter works");
    fdk_box_set_padding(frame, 6);
    assert_bounds(a, 6, 6, 288, 30, "frame padding setter works");

    /* The same propagation through plain nested boxes: a child added
     * to `inner` (below frame + footer) grows `inner` and re-runs
     * outer's packing to slot it. The padding change above shrank
     * the frame (98 -> 86) and the footer moved up — propagation at
     * work; defaults spacing/padding are 0. */
    fdk_widget *inner = NULL;
    assert(fdk_ok(fdk_box_create(outer, FDK_VERTICAL, &inner)));
    fdk_widget *c = mk_child(inner, 0, 25);
    (void)c;
    assert_bounds(footer, 0, 86, 300, 20, "footer reflowed up");
    assert_bounds(inner, 0, 106, 300, 25, "inner slotted by outer");

    fdk_widget_destroy(root);
    printf("[ok] nested child-change propagation: frames relayout on "
           "their own child changes, ancestors re-run, setters reach "
           "subclasses\n");
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


/* ---- Phase 5 completion: grid, min/max constraints, baseline ---- */

static void test_grid_measure_and_arrange(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 2, 2, &grid)));

    /* Four children of differing natural sizes; one spanning two
     * columns in a second grid exercises multi-span distribution. */
    fdk_widget *a = mk_child(grid, 60, 30);   /* (0,0) */
    fdk_widget *b = mk_child(grid, 90, 20);   /* (1,0) */
    fdk_widget *c = mk_child(grid, 40, 50);   /* (0,1) */
    fdk_widget *d = mk_child(grid, 70, 25);   /* (1,1) */
    assert(fdk_ok(fdk_grid_attach(grid, a, 0, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, b, 1, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, c, 0, 1, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, d, 1, 1, 1, 1)));

    fdk_size nat;
    fdk_widget_measure(grid, &nat);
    /* cols: max(60,40)=60, max(90,70)=90; rows: max(30,20)=30,
     * max(50,25)=50. No spacing/padding: 60+90 x 30+50. */
    assert(nat.width == 150 && nat.height == 80);

    /* Arrange at exactly natural, AWAY FROM THE ORIGIN: the classic
     * cell packing, in GRID-RELATIVE coordinates (children at (0,0)
     * inside the grid — the assigned (10,20) is the grid's position
     * relative to ITS parent and must NOT offset the children; the
     * Phase 6 box bug, pinned here so it can't come back). */
    fdk_widget_arrange(grid, (fdk_rect){10, 20, 150, 80});
    assert_bounds(a, 0, 0, 60, 30, "grid a (0,0) grid-relative");
    assert_bounds(b, 60, 0, 90, 30, "grid b (1,0) fills its row height");
    assert_bounds(c, 0, 30, 60, 50, "grid c (0,1)");
    assert_bounds(d, 60, 30, 90, 50, "grid d (1,1)");

    /* Extra space goes nowhere without expand flags: cells keep
     * their sizes, content stays top-left. */
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 200, 100});
    assert_bounds(a, 0, 0, 60, 30, "grid a with extra, no expand");
    assert_bounds(b, 60, 0, 90, 30, "grid b with extra");

    /* Expand column 1 and row 1: they share the +50/+20 extra. */
    fdk_grid_set_column_expand(grid, 1, true);
    fdk_grid_set_row_expand(grid, 1, true);
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 200, 100});
    assert_bounds(a, 0, 0, 60, 30, "grid a: unexpanded tracks fixed");
    assert_bounds(b, 60, 0, 140, 30, "grid b: expanded column grew by 50");
    assert_bounds(c, 0, 30, 60, 70, "grid c: expanded row grew by 20");
    assert_bounds(d, 60, 30, 140, 70, "grid d: both expanded");

    /* Spacing + padding shift every track. */
    fdk_grid_set_column_expand(grid, 1, false);
    fdk_grid_set_row_expand(grid, 1, false);
    fdk_grid_set_spacing(grid, 10);
    fdk_grid_set_padding(grid, 5);
    fdk_size nat2;
    fdk_widget_measure(grid, &nat2);
    /* 5+60+10+90+5 x 5+30+10+50+5 */
    assert(nat2.width == 170 && nat2.height == 100);
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 170, 100});
    assert_bounds(a, 5, 5, 60, 30, "grid a with padding+spacing");
    assert_bounds(b, 75, 5, 90, 30, "grid b with padding+spacing");
    assert_bounds(c, 5, 45, 60, 50, "grid c with padding+spacing");
    assert_bounds(d, 75, 45, 90, 50, "grid d with padding+spacing");

    fdk_widget_destroy(root);

    /* ---- multi-span distribution + growth ---- */
    root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300},
                                    &root)));
    grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 0, 0, &grid))); /* grows on attach */
    fdk_widget *wide = mk_child(grid, 130, 20);
    fdk_widget *l = mk_child(grid, 40, 10);
    fdk_widget *r = mk_child(grid, 60, 10);
    assert(fdk_ok(fdk_grid_attach(grid, wide, 0, 0, 2, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, l, 0, 1, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, r, 1, 1, 1, 1)));

    fdk_size nat3;
    fdk_widget_measure(grid, &nat3);
    /* cols: 40 + 60 = 100 base; the spanning child wants 130 ->
     * deficit 30 -> +15 each: cols 55, 75; rows 20 + 10. */
    assert(nat3.width == 130 && nat3.height == 30);

    fdk_widget_arrange(grid, (fdk_rect){0, 0, 130, 30});
    assert_bounds(wide, 0, 0, 130, 20, "spanning child covers both cols");
    assert_bounds(l, 0, 20, 55, 10, "left cell after deficit distribution");
    assert_bounds(r, 55, 20, 75, 10, "right cell after deficit distribution");

    /* Homogeneous: every column the widest, every row the tallest. */
    fdk_grid_set_homogeneous(grid, true);
    fdk_size nat4;
    fdk_widget_measure(grid, &nat4);
    assert(nat4.width == 150 && nat4.height == 40); /* 2x75 x 2x20 */
    fdk_widget_destroy(root);

    /* ---- hidden children take no track space ---- */
    root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 100, 100},
                                    &root)));
    grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 1, 2, &grid)));
    a = mk_child(grid, 50, 20);
    b = mk_child(grid, 30, 20);
    assert(fdk_ok(fdk_grid_attach(grid, a, 0, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, b, 1, 0, 1, 1)));
    fdk_widget_set_visible(b, false);
    fdk_size nat5;
    fdk_widget_measure(grid, &nat5);
    assert(nat5.width == 50 && nat5.height == 20);
    fdk_widget_destroy(root);

    /* ---- align inside a cell ---- */
    root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 100, 100},
                                    &root)));
    grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 2, 2, &grid)));
    /* a's cell is sized by its neighbors: wide (60) column 0 via the
     * child below it, tall (50) row 0 via the child beside it. */
    a = mk_child(grid, 30, 10);        /* (0,0): the align target */
    fdk_widget *tall = mk_child(grid, 20, 50); /* (1,0): sizes row 0 */
    fdk_widget *widecell = mk_child(grid, 60, 20); /* (0,1): sizes col 0 */
    assert(fdk_ok(fdk_grid_attach(grid, a, 0, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, tall, 1, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, widecell, 0, 1, 1, 1)));
    fdk_widget_set_align(a, FDK_ALIGN_START, FDK_ALIGN_CENTER);
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 80, 70});
    assert_bounds(a, 0, 20, 30, 10, "cell child centered vertically");
    fdk_widget_set_align(a, FDK_ALIGN_END, FDK_ALIGN_END);
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 80, 70});
    assert_bounds(a, 30, 40, 30, 10, "cell child end/end");
    fdk_widget_set_align(a, FDK_ALIGN_FILL, FDK_ALIGN_FILL);
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 80, 70});
    assert_bounds(a, 0, 0, 60, 50, "cell child FILL fills the cell");
    fdk_widget_destroy(root);

    printf("[ok] grid: measure/arrange, spans, growth, expand, spacing, "
           "homogeneous, hidden, cell align\n");
}

static void test_size_limits(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *w = mk_child(root, 50, 20);

    /* Min clamps up. */
    fdk_widget_set_size_limits(w, 80, 0, 0, 0);
    fdk_size m;
    fdk_widget_measure(w, &m);
    assert(m.width == 80 && m.height == 20);

    /* Max clamps down (and survives a later natural change). */
    fdk_widget_set_size_limits(w, 0, 0, 40, 15);
    fdk_widget_measure(w, &m);
    assert(m.width == 40 && m.height == 15);
    fdk_widget_set_natural_size(w, 200, 100);
    fdk_widget_measure(w, &m);
    assert(m.width == 40 && m.height == 15);

    /* Contradictory min > max: min wins, normalized into max. */
    fdk_widget_set_size_limits(w, 90, 30, 50, 0);
    fdk_i32 gmin_w, gmin_h, gmax_w, gmax_h;
    fdk_widget_get_size_limits(w, &gmin_w, &gmin_h, &gmax_w, &gmax_h);
    assert(gmin_w == 90 && gmax_w == 90); /* max raised to min */
    assert(gmin_h == 30 && gmax_h == 0);
    fdk_widget_measure(w, &m);
    assert(m.width == 90 && m.height >= 30);

    /* Constraints flow through CONTAINERS: a box measures its
     * clamped child. */
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &box)));
    fdk_widget *kid = mk_child(box, 50, 20);
    fdk_widget_set_size_limits(kid, 120, 0, 0, 0);
    fdk_size bm;
    fdk_widget_measure(box, &bm);
    assert(bm.width == 120 && bm.height == 20);

    /* NULL args + negatives are safe. */
    fdk_widget_set_size_limits(NULL, 0, 0, 0, 0);
    fdk_widget_set_size_limits(w, -5, -5, -5, -5);
    fdk_widget_get_size_limits(w, &gmin_w, &gmin_h, &gmax_w, &gmax_h);
    assert(gmin_w == 0 && gmax_w == 0);
    fdk_widget_get_size_limits(NULL, NULL, NULL, NULL, NULL);

    fdk_widget_destroy(root);
    printf("[ok] size limits: min/max clamps in every measure, "
           "normalization, container flow, argument safety\n");
}

static void test_baseline_alignment(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300},
                                    &root)));

    /* Plain widgets carry no baseline: get_baseline is false. */
    fdk_widget *plain = mk_child(root, 30, 10);
    fdk_i32 b = 123;
    assert(!fdk_widget_get_baseline(plain, &b));
    assert(b == 0);

    /* Synthesize text baselines via the internal setter (the Label
     * sets the same field from its font ascent in production). */
    fdk_widget *row = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &row)));
    fdk_widget *t1 = mk_child(row, 40, 30);
    fdk_widget *t2 = mk_child(row, 50, 16);
    fdk_widget *t3 = mk_child(row, 30, 22); /* no baseline: bottom */
    fdk__widget_set_baseline(t1, 24);
    fdk__widget_set_baseline(t2, 12);
    fdk__widget_set_baseline(t3, -1);
    fdk_i32 got = 0;
    assert(fdk_widget_get_baseline(t1, &got) && got == 24);

    fdk_widget_set_align(t1, FDK_ALIGN_FILL, FDK_ALIGN_BASELINE);
    fdk_widget_set_align(t2, FDK_ALIGN_FILL, FDK_ALIGN_BASELINE);
    fdk_widget_set_align(t3, FDK_ALIGN_FILL, FDK_ALIGN_BASELINE);

    /* Measure: baseline group extent. Deepest baseline = 24 (t1);
     * t3 (no baseline) uses its bottom = 22. Group height =
     * max(24-24+30, 24-12+16, 24-22+22) = max(30, 28, 24) = 30. */
    fdk_size m;
    fdk_widget_measure(row, &m);
    assert(m.height == 30);

    /* Arrange: every child's baseline (or bottom) lands on y=24. */
    fdk_widget_arrange(row, (fdk_rect){0, 0, 200, 30});
    assert_bounds(t1, 0, 0, 40, 30, "baseline t1 at top");
    assert_bounds(t2, 40, 12, 50, 16, "baseline t2 hung from the row");
    assert_bounds(t3, 90, 2, 30, 22, "baseline-less t3 bottom on the row");

    /* Labels report their font's ascent as the baseline (a real
     * font from the system, skipped honestly when none exists). */
    fdk_font *font = fdk_font_load_system_default(16);
    if (font != NULL) {
        fdk_widget *lab = NULL;
        assert(fdk_ok(fdk_label_create(root, font, "Ag", &lab)));
        fdk_font_metrics fm;
        fdk_font_get_metrics(font, &fm);
        fdk_size lm;
        fdk_widget_measure(lab, &lm); /* populates the baseline */
        assert(lm.height == fm.ascent + fm.descent);
        fdk_i32 lb = 0;
        assert(fdk_widget_get_baseline(lab, &lb));
        assert(lb == fm.ascent);
        fdk_widget_destroy(lab);
        fdk_font_destroy(font);
    }

    fdk_widget_destroy(root);
    printf("[ok] baseline: box cross-axis alignment, group extent, "
           "bottom-edge fallback, label ascent\n");
}

static void test_grid_notification_relayout(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL, (fdk_rect){0, 0, 400, 300},
                                    &root)));

    /* The notification path, not the explicit arrange: a grid with
     * established bounds whose children arrive (attach) and CHANGE
     * (natural size) afterwards must re-pack itself with NO explicit
     * fdk_widget_arrange call — the exact bug the grid shipped with
     * (the notifier knew boxes only; grid children stayed wherever
     * the last unrelated arrange left them). */
    fdk_widget *grid = NULL;
    assert(fdk_ok(fdk_grid_create(root, 2, 2, &grid)));
    fdk_widget_arrange(grid, (fdk_rect){0, 0, 200, 100}); /* once */

    fdk_widget *a = mk_child(grid, 60, 20);
    fdk_widget *b = mk_child(grid, 40, 30);
    /* Attach — nothing else: the notification must place them. */
    assert(fdk_ok(fdk_grid_attach(grid, a, 0, 0, 1, 1)));
    assert(fdk_ok(fdk_grid_attach(grid, b, 1, 0, 1, 1)));
    assert_bounds(a, 0, 0, 60, 30, "attach placed a, cell-height FILL");
    assert_bounds(b, 60, 0, 40, 30, "attach placed b (notification)");

    /* A child's natural-size change must re-pack the grid AND
     * propagate to the ancestor box (its natural grew). */
    fdk_widget *outer = NULL;
    assert(fdk_ok(fdk_box_create(NULL, FDK_VERTICAL, &outer)));
    fdk_widget *grid2 = NULL;
    assert(fdk_ok(fdk_grid_create(outer, 1, 1, &grid2)));
    fdk_widget_arrange(outer, (fdk_rect){0, 0, 300, 200});
    fdk_widget *c = mk_child(grid2, 50, 50);
    assert(fdk_ok(fdk_grid_attach(grid2, c, 0, 0, 1, 1)));
    fdk_size before;
    fdk_widget_measure(outer, &before);
    assert(before.height == 50);
    fdk_widget_set_natural_size(c, 50, 90);
    fdk_size after;
    fdk_widget_measure(outer, &after);
    assert(after.height == 90); /* grid re-measured, box saw the growth */
    assert_bounds(c, 0, 0, 50, 90, "grid child re-arranged at its new size");

    fdk_widget_destroy(outer);
    fdk_widget_destroy(root);
    printf("[ok] grid notification: attach + child-change re-pack without "
           "explicit arrange (notifier grid-ness regression)\n");
}


/* ---- layout batching (Phase 11) ----
 *
 * The batch contract: identical FINAL geometry to the eager path,
 * one flush per dirty chain, and safety across destroys. */

/* Collects the tree's geometry into a flat digest: bounds in
 * depth-first order. Two trees built identically must produce the
 * same digest whether layout ran eagerly or batched. */
static void geometry_digest(fdk_widget *w, unsigned long *acc) {
    fdk_rect b = fdk_widget_get_bounds(w);
    *acc = *acc * 1315423911ul + (unsigned long)b.x;
    *acc = *acc * 31ul + (unsigned long)b.y;
    *acc = *acc * 31ul + (unsigned long)b.width;
    *acc = *acc * 31ul + (unsigned long)b.height;
    for (size_t i = 0; i < fdk_widget_child_count(w); i++) {
        geometry_digest(fdk_widget_child_at(w, i), acc);
    }
}

/* Builds the standard batching-fixture tree on `root`: nested
 * boxes with fixed-size children (deterministic geometry). */
static void build_fixture(fdk_widget *root) {
    for (int i = 0; i < 4; i++) {
        fdk_widget *row = NULL;
        assert(fdk_ok(fdk_box_create(root, FDK_HORIZONTAL, &row)));
        fdk_widget_set_expand(row, true, false);
        for (int k = 0; k < 3; k++) {
            (void)mk_child(row, 40 + k * 10, 20 + k);
        }
        fdk_widget *col = NULL;
        assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &col)));
        (void)mk_child(col, 25, 15);
        (void)mk_child(col, 30, 10);
    }
}

static void test_layout_batch_equivalence(void) {
    /* Eager reference. */
    fdk_widget *eager = NULL;
    assert(fdk_ok(fdk_box_create(NULL, FDK_VERTICAL, &eager)));
    build_fixture(eager);
    fdk_widget_arrange(eager, (fdk_rect){0, 0, 400, 300});
    unsigned long digest_eager = 7;
    geometry_digest(eager, &digest_eager);

    /* Batched build: creates mark instead of relayouting. */
    fdk_widget *batched = NULL;
    fdk_layout_begin_batch();
    assert(fdk_ok(fdk_box_create(NULL, FDK_VERTICAL, &batched)));
    build_fixture(batched);
    /* Geometry is intentionally NOT yet final inside the batch. */
    fdk_layout_end_batch();
    fdk_widget_arrange(batched, (fdk_rect){0, 0, 400, 300});
    unsigned long digest_batched = 7;
    geometry_digest(batched, &digest_batched);
    assert(digest_eager == digest_batched);

    /* A SECOND mutation round inside a batch after a flush: marks
     * accumulate again and the next flush catches them. */
    fdk_layout_begin_batch();
    fdk_widget *extra = mk_child(batched, 50, 50);
    fdk_widget_set_natural_size(extra, 60, 60);
    fdk_layout_end_batch();
    fdk_widget_arrange(batched, (fdk_rect){0, 0, 400, 300});
    /* The eager twin with the same mutation: */
    fdk_widget *extra2 = mk_child(eager, 50, 50);
    fdk_widget_set_natural_size(extra2, 60, 60);
    fdk_widget_arrange(eager, (fdk_rect){0, 0, 400, 300});
    digest_eager = 7;
    digest_batched = 7;
    geometry_digest(eager, &digest_eager);
    geometry_digest(batched, &digest_batched);
    assert(digest_eager == digest_batched);

    /* Nested batches: only the outermost flushes. */
    fdk_layout_begin_batch();
    fdk_layout_begin_batch();
    fdk_widget *n1 = mk_child(batched, 10, 10);
    (void)n1;
    fdk_layout_end_batch(); /* inner: no flush yet */
    fdk_layout_end_batch(); /* outer: flush */
    fdk_layout_end_batch(); /* unbalanced: harmless no-op */
    fdk_widget_arrange(batched, (fdk_rect){0, 0, 400, 300});

    fdk_widget_destroy(eager);
    fdk_widget_destroy(batched);
    printf("[ok] layout batch: batched == eager geometry; nesting; "
           "unbalanced end\n");
}

static void test_layout_batch_destroy_safety(void) {
    /* Destroying widgets mid-batch must never leave the pending set
     * with a dangling entry (the flush would relayout freed
     * memory). */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_box_create(NULL, FDK_VERTICAL, &root)));
    fdk_layout_begin_batch();
    build_fixture(root);
    /* Destroy a whole nested subtree that carries pending marks. */
    fdk_widget *victim = fdk_widget_child_at(root, 2);
    fdk_widget_destroy(victim);
    /* More mutations after the destroy. */
    (void)mk_child(root, 35, 25);
    fdk_layout_end_batch();
    fdk_widget_arrange(root, (fdk_rect){0, 0, 400, 300});

    /* The tree is consistent: every remaining child of root still
     * has sane (non-degenerate) bounds after the flush. */
    for (size_t i = 0; i < fdk_widget_child_count(root); i++) {
        fdk_widget *c = fdk_widget_child_at(root, i);
        fdk_rect b = fdk_widget_get_bounds(c);
        assert(b.width > 0 || b.height > 0);
    }
    fdk_widget_destroy(root);
    printf("[ok] layout batch: mid-batch destroy is flush-safe\n");
}

int main(void) {
    test_layout_batch_equivalence();
    test_layout_batch_destroy_safety();
    test_box_measure();
    test_box_arrange_horizontal();
    test_box_arrange_vertical_margins();
    test_box_homogeneous_distribution();
    test_dynamic_children_relayout();
    test_nested_boxes();
    test_nested_child_change_propagation();
    test_layout_paints();
    test_argument_safety();
    test_grid_measure_and_arrange();
    test_grid_notification_relayout();
    test_size_limits();
    test_baseline_alignment();
    printf("all headless layout tests passed\n");
    return 0;
}
