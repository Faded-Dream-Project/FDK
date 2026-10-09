/* test_containers.c — headless tests for the 1.4.1 container pair
 * (Paned, the two-pane splitter; Expander, the disclosure section).
 *
 * Same discipline as the catalog suites: standalone roots, offscreen
 * surfaces, synthetic window events through
 * fdk_widget_tree_handle_event, the animation test clock driving the
 * Expander's reveal exactly as the pump would under a window, and
 * ASan+UBSan throughout. A system font is needed for the text-bearing
 * geometry (the header band, the label shift); without one the suite
 * honestly skips.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* paned_of / expander_of state  */
#include "widget/widget_internal.h"  /* the animation test clock      */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

/* ---- helpers (event shapes mirror test_controls.c) ---- */

static fdk_event_data ev_button(fdk_event_type t, float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    return e;
}

static fdk_event_data ev_motion(float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = FDK_EVENT_POINTER_MOTION;
    e.pointer.position.x = x;
    e.pointer.position.y = y;
    return e;
}

static fdk_event_data ev_key(fdk_event_type t, fdk_scancode sc) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.key.scancode = sc;
    return e;
}

static void anim_at(long long t) {
    fdk__animation_set_test_clock(t);
    fdk__animation_pump(t);
}

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static fdk_u32 pack_color(fdk_color c) {
    fdk_f32 r = c.r < 0.0f ? 0.0f : (c.r > 1.0f ? 1.0f : c.r);
    fdk_f32 g = c.g < 0.0f ? 0.0f : (c.g > 1.0f ? 1.0f : c.g);
    fdk_f32 b = c.b < 0.0f ? 0.0f : (c.b > 1.0f ? 1.0f : c.b);
    return ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
           ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
           (fdk_u32)(b * 255.0f + 0.5f);
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static void click(fdk_widget *root, float x, float y) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

static void press_move_release(fdk_widget *root, float x0, float y0,
                               float x1, float y1) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x0, y0);
    fdk_event_data move = ev_motion(x1, y1);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x1, y1);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &move);
    (void)fdk_widget_tree_handle_event(root, &up);
}

/* A plain child with a controllable natural size (the measure
 * fallback: create-time bounds are the size request). */
static fdk_widget *plain_child(fdk_widget *parent, fdk_i32 w, fdk_i32 h) {
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_widget_create(parent, NULL,
                                    (fdk_rect){0, 0, w, h}, &c)));
    return c;
}

/* ---- Paned ---- */

static void test_paned_layout(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *p = NULL;
    assert(fdk_ok(fdk_paned_create(root, FDK_HORIZONTAL, &p)));
    fdk_widget_arrange(p, (fdk_rect){0, 0, 300, 200});

    /* Empty paned: measures to just the divider; nothing to slot. */
    fdk_size nat;
    fdk_widget_measure(p, &nat);
    assert(nat.width == FDK_PANED_DIVIDER && nat.height == 0);

    /* Auto split: child1 gets its natural (60), child2 the rest. */
    fdk_widget *c1 = plain_child(p, 60, 40);
    fdk_widget *c2 = plain_child(p, 80, 40);
    fdk_widget_measure(p, &nat);
    assert(nat.width == 60 + FDK_PANED_DIVIDER + 80); /* shrink-wrap */
    assert(nat.height == 40);
    assert(!fdk_paned_position_is_set(p));
    assert(fdk_paned_get_position(p) == 60);
    assert(c1->bounds.width == 60); /* auto split applied          */
    assert(c2->bounds.x == 60 + FDK_PANED_DIVIDER);
    assert(c2->bounds.width == 300 - 60 - FDK_PANED_DIVIDER);
    assert(c1->bounds.height == 200 && c2->bounds.height == 200);

    /* Pin: the divider offset is clamped so it stays visible. */
    assert(fdk_paned_set_position(p, 100) == FDK_OK);
    assert(fdk_paned_position_is_set(p));
    assert(fdk_paned_get_position(p) == 100);
    assert(c1->bounds.width == 100);
    assert(c2->bounds.x == 100 + FDK_PANED_DIVIDER);
    assert(fdk_paned_set_position(p, -5) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_paned_set_position(p, 5000) == FDK_OK); /* clamped */
    assert(fdk_paned_get_position(p) == 300 - FDK_PANED_DIVIDER);
    assert(c1->bounds.width == 300 - FDK_PANED_DIVIDER);
    assert(c2->bounds.width == 0); /* squeezed to zero, GTK-style  */

    /* Unset: back to the auto split. */
    fdk_paned_unset_position(p);
    assert(!fdk_paned_position_is_set(p));
    assert(fdk_paned_get_position(p) == 60);
    assert(c1->bounds.width == 60);

    /* The third child is refused loudly (the class's fixed slots). */
    fdk_widget *c3 = NULL;
    assert(fdk_widget_create(p, NULL, (fdk_rect){0, 0, 10, 10}, &c3) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* A wider child re-splits the auto position through the layout
     * notifier (child re-measure -> paned relayout). */
    fdk_widget_set_natural_size(c1, 120, 40);
    fdk_widget_child_layout_changed(p);
    assert(fdk_paned_get_position(p) == 120);
    assert(c1->bounds.width == 120);

    /* Single child: it IS the paned (no divider reserved). */
    fdk_widget_destroy(c2);
    assert(p->child_count == 1);
    assert(c1->bounds.width == 300); /* full extent, no divider   */

    /* Vertical orientation: the split runs along Y. */
    fdk_widget *pv = NULL;
    assert(fdk_ok(fdk_paned_create(root, FDK_VERTICAL, &pv)));
    fdk_widget_arrange(pv, (fdk_rect){0, 220, 100, 300});
    fdk_widget *t1 = plain_child(pv, 40, 50);
    fdk_widget *t2 = plain_child(pv, 40, 70);
    assert(fdk_paned_get_position(pv) == 50);
    assert(t1->bounds.height == 50);
    assert(t2->bounds.y == 50 + FDK_PANED_DIVIDER);
    assert(t2->bounds.height == 300 - 50 - FDK_PANED_DIVIDER);
    assert(t1->bounds.width == 100);

    /* Measure: vertical shrink-wrap swaps the axes. */
    fdk_widget_measure(pv, &nat);
    assert(nat.height == 50 + FDK_PANED_DIVIDER + 70);
    assert(nat.width == 40);

    fdk_widget_destroy(root);
    printf("[ok] paned: auto split honors naturals, pin clamps to the "
           "visible range, squeeze-to-zero, unset returns, third child "
           "refused, single child fills, vertical axes\n");
}

static void test_paned_drag(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *p = NULL;
    assert(fdk_ok(fdk_paned_create(root, FDK_HORIZONTAL, &p)));
    fdk_widget_arrange(p, (fdk_rect){0, 0, 300, 200});
    (void)plain_child(p, 60, 40);
    (void)plain_child(p, 80, 40);

    /* Press in the divider band (auto position 60, band [60,66)),
     * drag to x=150, release: the divider lands at 150 - grab
     * offset. The press lands at x=63 -> grab offset 3. */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 63.0f,
                                    100.0f);
    assert(fdk_widget_tree_handle_event(root, &down)); /* consumed  */
    fdk_event_data move = ev_motion(150.0f, 100.0f);
    assert(fdk_widget_tree_handle_event(root, &move));
    assert(fdk_paned_position_is_set(p));
    assert(fdk_paned_get_position(p) == 150 - 3);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 150.0f,
                                  100.0f);
    assert(fdk_widget_tree_handle_event(root, &up));
    assert(fdk_paned_get_position(p) == 150 - 3); /* parked */

    /* Drag clamps at both walls (presses start INSIDE the current
     * band — the divider owns only its own 6 px). */
    press_move_release(root, 150.0f, 100.0f, -50.0f, 100.0f);
    assert(fdk_paned_get_position(p) == 0);
    press_move_release(root, 3.0f, 100.0f, 900.0f, 100.0f);
    assert(fdk_paned_get_position(p) == 300 - FDK_PANED_DIVIDER);

    /* A press OUTSIDE the band is not ours (pane content owns it). */
    down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, 10.0f, 100.0f);
    /* Pane 1 is a plain widget: the press is unhandled (bubbles to
     * the root, which does not consume plain presses either). */
    assert(!fdk_widget_tree_handle_event(root, &down));
    fdk_event_data up2 = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 10.0f,
                                   100.0f);
    (void)fdk_widget_tree_handle_event(root, &up2);

    /* Divider hover: the pill + accent line paint while the pointer
     * is inside the band. */
    fdk_paned_unset_position(p);
    fdk_event_data enter = ev_motion(63.0f, 100.0f);
    (void)fdk_widget_tree_handle_event(root, &enter);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(320, 220, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    fdk_color acc = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    fdk_color bord = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER);
    /* The 1-px divider line, centered in the band: x = 63. Sampled
     * at y=60, clear of the 3-dot grip at the vertical center. */
    assert(px_at(s, 63, 60) == pack_color(acc)); /* hot: accent  */
    /* Leaving: the line reverts to the border color. */
    fdk_event_data leave = ev_motion(200.0f, 100.0f);
    (void)fdk_widget_tree_handle_event(root, &leave);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 63, 60) == pack_color(bord));

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] paned drag: press-moves-release retargets with grab "
           "offset, wall clamps, out-of-band press bubbles, hot/resting "
           "divider ink\n");
}

static void test_paned_keyboard(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *p = NULL;
    assert(fdk_ok(fdk_paned_create(root, FDK_HORIZONTAL, &p)));
    fdk_widget_arrange(p, (fdk_rect){0, 0, 300, 200});
    (void)plain_child(p, 60, 40);
    (void)plain_child(p, 80, 40);
    fdk_paned_set_position(p, 100);

    /* Focus + arrows: 8-px steps each way; wrong-axis arrows are
     * not consumed (they bubble to whoever wants them). */
    fdk_widget_focus(p);
    fdk_event_data right = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_RIGHT);
    assert(fdk_widget_tree_handle_event(root, &right));
    assert(fdk_paned_get_position(p) == 108);
    fdk_event_data left = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_LEFT);
    assert(fdk_widget_tree_handle_event(root, &left));
    assert(fdk_paned_get_position(p) == 100);

    /* Vertical paned answers Up/Down; Left/Right bubble. */
    fdk_widget *pv = NULL;
    assert(fdk_ok(fdk_paned_create(root, FDK_VERTICAL, &pv)));
    fdk_widget_arrange(pv, (fdk_rect){310, 0, 90, 240});
    (void)plain_child(pv, 40, 50);
    (void)plain_child(pv, 40, 70);
    fdk_paned_set_position(pv, 100);
    fdk_widget_focus(pv);
    fdk_event_data down = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_DOWN);
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_paned_get_position(pv) == 108);
    fdk_event_data up = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_UP);
    assert(fdk_widget_tree_handle_event(root, &up));
    assert(fdk_paned_get_position(pv) == 100);
    fdk_event_data miss = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_RIGHT);
    assert(!fdk_widget_tree_handle_event(root, &miss));

    /* Home/End jump to the extremes (clamped). */
    fdk_event_data end = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_END);
    assert(fdk_widget_tree_handle_event(root, &end));
    assert(fdk_paned_get_position(pv) == 240 - FDK_PANED_DIVIDER);
    fdk_event_data home = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_HOME);
    assert(fdk_widget_tree_handle_event(root, &home));
    assert(fdk_paned_get_position(pv) == 0);

    /* a11y: SPLIT_PANE role, the divider offset is the value. */
    fdk_paned_set_position(pv, 42);
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(pv, &info)));
    assert(info.role == FDK_A11Y_ROLE_SPLIT_PANE);
    assert(info.has_value);
    assert(info.value_current == 42.0);
    assert(info.value_min == 0.0);
    assert(info.value_max == (double)(240 - FDK_PANED_DIVIDER));
    fdk_a11y_info_free(&info);

    fdk_widget_destroy(root);
    printf("[ok] paned keyboard: axis-matched arrows step 8 px, "
           "wrong-axis arrows bubble, Home/End extremes, a11y value "
           "interface\n");
}

/* ---- Expander ---- */

static int g_expand_changes = 0;
static bool g_last_expanded = false;
static void on_expand_change(fdk_widget *w, bool expanded, void *user) {
    (void)w;
    (void)user;
    g_expand_changes++;
    g_last_expanded = expanded;
}

static void test_expander_layout(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *e = NULL;
    assert(fdk_ok(fdk_expander_create(root, g_font, "Details", &e)));
    fdk_expander_set_on_changed(e, on_expand_change, NULL);
    g_expand_changes = 0;

    /* Collapsed: header only; the child is invisible (input-
     * transparent — a press in the content zone falls through). */
    fdk_widget *content = plain_child(e, 200, 120);
    fdk_size nat;
    fdk_widget_measure(e, &nat);
    fdk_i32 header_h = nat.height;
    assert(header_h >= 24); /* the fontless floor, or the band */
    assert(nat.width > 0);  /* chevron + gap + "Details"      */
    assert((content->flags & FDK_WF_VISIBLE) == 0);

    /* The label API. */
    assert(strcmp(fdk_expander_get_label(e), "Details") == 0);
    assert(fdk_expander_set_label(e, "More") == FDK_OK);
    assert(strcmp(fdk_expander_get_label(e), "More") == 0);
    assert(fdk_expander_set_label(NULL, "x") ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Second child refused. */
    fdk_widget *extra = NULL;
    assert(fdk_widget_create(e, NULL, (fdk_rect){0, 0, 10, 10}, &extra) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Expand: the child becomes visible, the natural grows by the
     * content height, the reveal flight runs on the test clock. */
    anim_at(0);
    assert(!fdk_expander_is_expanded(e));
    fdk_expander_set_expanded(e, true);
    assert(g_expand_changes == 1 && g_last_expanded);
    assert((content->flags & FDK_WF_VISIBLE) != 0);
    anim_at(80); /* mid-flight */
    fdk_widget_measure(e, &nat);
    assert(nat.height > header_h && nat.height < header_h + 120);
    anim_at(200); /* done (160 ms) */
    fdk_widget_measure(e, &nat);
    assert(nat.height == header_h + 120);

    /* a11y: EXPANDER role, EXPANDED state, name = label. */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(e, &info)));
    assert(info.role == FDK_A11Y_ROLE_EXPANDER);
    assert((info.states & FDK_A11Y_EXPANDED) != 0);
    assert(info.name != NULL && strcmp(info.name, "More") == 0);
    fdk_a11y_info_free(&info);

    /* Collapse: the flight returns; at rest the child hides again. */
    fdk_expander_set_expanded(e, false);
    assert(g_expand_changes == 2 && !g_last_expanded);
    anim_at(280); /* mid-return */
    fdk_widget_measure(e, &nat);
    assert(nat.height > header_h);
    anim_at(400); /* landed */
    fdk_widget_measure(e, &nat);
    assert(nat.height == header_h);
    assert((content->flags & FDK_WF_VISIBLE) == 0);

    /* Retarget-from-live: expand, fly half-way, collapse — the
     * return departs from the CURRENT blend, not from 1. (The
     * clock keeps moving FORWARD: the last pump was at 400, so the
     * flights below run on [400, 560] — going backwards would clamp
     * elapsed to 0 and freeze the door.) */
    fdk_expander_set_expanded(e, true);
    anim_at(480);
    fdk_widget_measure(e, &nat);
    fdk_i32 half_h = nat.height;
    assert(half_h > header_h && half_h < header_h + 120);
    fdk_expander_set_expanded(e, false);
    anim_at(490); /* 10 ms into the return */
    fdk_widget_measure(e, &nat);
    assert(nat.height < half_h); /* already below the departure */

    anim_at(0);
    fdk_widget_destroy(root);
    printf("[ok] expander: collapsed hides child + measures to the "
           "header, expand/collapse fly 160 ms on the test clock, "
           "retargets from live, a11y EXPANDED + name, second child "
           "refused\n");
}

static void test_expander_interaction(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *e = NULL;
    assert(fdk_ok(fdk_expander_create(root, g_font, "Section", &e)));
    (void)plain_child(e, 200, 120); /* the content slot           */
    /* The header band = the COLLAPSED natural height (header only),
     * measured BEFORE the first expansion — the arranged bounds
     * below are the test's own choice and include content slack. */
    fdk_size nat;
    fdk_widget_measure(e, &nat);
    fdk_i32 band = nat.height;
    assert(band >= 24);
    fdk_widget_arrange(e, (fdk_rect){10, 10, 260, band + 120});

    anim_at(0);
    fdk_expander_set_expanded(e, true);
    anim_at(200);

    /* Click on the HEADER toggles (the click helper: press +
     * release inside). */
    g_expand_changes = 0;
    fdk_expander_set_on_changed(e, on_expand_change, NULL);
    click(root, 60.0f, 20.0f); /* inside the header band           */
    assert(g_expand_changes == 1 && !g_last_expanded);

    /* A click on the CONTENT area does not toggle (the child owns
     * it; the press reaches the child, not the header branch). */
    click(root, 60.0f, (float)(10 + band + 20)); /* content zone */
    assert(g_expand_changes == 1); /* unchanged                    */

    /* Space toggles while focused (keyboard parity with buttons). */
    fdk_widget_focus(e);
    fdk_event_data sp = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_SPACE);
    assert(fdk_widget_tree_handle_event(root, &sp));
    assert(g_expand_changes == 2 && g_last_expanded);

    /* A11y ACTIVATE performs the same toggle. */
    assert(fdk_ok(fdk_a11y_perform(e, FDK_A11Y_ACTION_ACTIVATE, 0.0)));
    assert(g_expand_changes == 3 && !g_last_expanded);

    /* Paint proof: the chevron rotates with the reveal — collapsed
     * ink sits right of center, expanded ink sits below it. Sample
     * a small window around the glyph's rotated positions. */
    anim_at(500);
    fdk_expander_set_expanded(e, true);
    anim_at(700);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 200, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    /* Glyph box: x = 10 + 6 = 16, centered on the header band. */
    fdk_i32 gx = 16 + 5; /* the glyph center, x                   */
    int ink_below = 0, ink_right = 0;
    for (int dy = 1; dy <= 6; dy++) {
        if (px_at(s, gx + dy, 10 + band / 2) != 0x000000u) {
            ink_below++; /* the rotated "v" arm sweeps below     */
        }
        if (px_at(s, gx + 3 + dy, 10 + band / 2 - dy) != 0x000000u) {
            ink_right++;
        }
    }
    assert(ink_below > 0); /* expanded: pointing DOWN             */

    /* Destroy mid-flight is ASan-clean (the animator drops the
     * flight without firing done into a dying widget). */
    fdk_expander_set_expanded(e, false);
    anim_at(750); /* 50 ms into the return flight */
    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] expander interaction: header click / content fall-"
           "through / Space / a11y ACTIVATE all toggle; chevron "
           "rotates down when expanded; mid-flight destroy clean\n");
}

/* ---- row icons (the sidebar glyph seam) ---- */

static void test_row_icons(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    size_t idx0 = 0, idx1 = 0;
    assert(fdk_ok(fdk_list_append(list, "Home", &idx0)));
    assert(fdk_ok(fdk_list_append(list, "Documents", &idx1)));

    /* Default NONE; the setter/getter round-trip; unknown values
     * read as NONE; out-of-range rows are refused. */
    assert(fdk_list_row_get_icon(list, 0) == FDK_ROW_ICON_NONE);
    assert(fdk_list_row_set_icon(list, 0, FDK_ROW_ICON_HOME) == FDK_OK);
    assert(fdk_list_row_set_icon(list, 1, FDK_ROW_ICON_FOLDER) == FDK_OK);
    assert(fdk_list_row_get_icon(list, 0) == FDK_ROW_ICON_HOME);
    assert(fdk_list_row_get_icon(list, 1) == FDK_ROW_ICON_FOLDER);
    assert(fdk_list_row_set_icon(list, 0, (fdk_row_icon)77) == FDK_OK);
    assert(fdk_list_row_get_icon(list, 0) == FDK_ROW_ICON_NONE);
    assert(fdk_list_row_set_icon(list, 9, FDK_ROW_ICON_FILE) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_list_row_get_icon(NULL, 0) == FDK_ROW_ICON_NONE);
    fdk_list_row_set_icon(NULL, 0, FDK_ROW_ICON_FILE);

    /* Iconed rows are 22 px wider than the text alone (measured
     * from a clean NONE baseline — the round-trip block above left
     * row 1 with an icon). */
    fdk_size nat_plain, nat_icon;
    assert(fdk_list_row_set_icon(list, 0, FDK_ROW_ICON_NONE) == FDK_OK);
    assert(fdk_list_row_set_icon(list, 1, FDK_ROW_ICON_NONE) == FDK_OK);
    fdk_widget_measure(list, &nat_plain);
    assert(fdk_list_row_set_icon(list, 1, FDK_ROW_ICON_FOLDER) == FDK_OK);
    fdk_widget_measure(list, &nat_icon);
    assert(nat_icon.width == nat_plain.width + 16 + 6);

    /* The glyph paints ink inside its box (row 0, first column of
     * the icon zone; text starts 22 px further right). */
    fdk_widget_arrange(list, (fdk_rect){0, 0, 220, 120});
    fdk_list_row_set_icon(list, 0, FDK_ROW_ICON_HOME);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(240, 140, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int ink = 0;
    fdk_i32 row_h = fdk_list_get_row_height(list);
    for (int y = 0; y < (int)row_h; y++) {
        for (int x = 10; x < 26; x++) { /* LIST_ROW_PAD_X..+16      */
            if (px_at(s, x, y) != 0x000000u) {
                ink++;
            }
        }
    }
    assert(ink > 6); /* the house glyph, not a stray pixel        */

    /* Batch discipline: icons set inside a batch settle once. */
    fdk_list_begin_batch(list);
    assert(fdk_list_row_set_icon(list, 0, FDK_ROW_ICON_DRIVE) == FDK_OK);
    assert(fdk_list_row_set_icon(list, 1, FDK_ROW_ICON_FILE) == FDK_OK);
    fdk_list_end_batch(list);
    assert(fdk_list_row_get_icon(list, 0) == FDK_ROW_ICON_DRIVE);
    assert(fdk_list_row_get_icon(list, 1) == FDK_ROW_ICON_FILE);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] row icons: NONE default, round-trip + unknown-as-"
           "NONE, 22-px width accounting, glyph ink, batch settle\n");
}

/* ---- Stack (1.4.2) ---- */

static int g_stack_changes;
static size_t g_stack_last_index;
static const char *g_stack_last_name;

static void on_stack_change(fdk_widget *stack, size_t index,
                            const char *name, void *user) {
    (void)stack;
    (void)user;
    g_stack_changes++;
    g_stack_last_index = index;
    g_stack_last_name = name;
}

static void test_stack(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *st = NULL;
    assert(fdk_ok(fdk_stack_create(root, &st)));
    fdk_stack_set_on_changed(st, on_stack_change, NULL);
    g_stack_changes = 0;

    /* Empty stack: the 40x40 floor. */
    fdk_size nat;
    fdk_widget_measure(st, &nat);
    assert(nat.width == 40 && nat.height == 40);

    /* Pages: named keys, titles, adoption. */
    fdk_widget *pg1 = plain_child(st, 120, 80);
    fdk_widget *pg2 = plain_child(st, 200, 60);
    fdk_widget *pg3 = plain_child(st, 100, 150);
    assert(fdk_stack_add(st, pg1, "general", "General") == FDK_OK);
    assert(fdk_stack_add(st, pg2, "appearance", NULL) == FDK_OK);
    assert(fdk_stack_add(st, pg3, "about", "About") == FDK_OK);
    assert(fdk_stack_page_count(st) == 3);
    assert(fdk_stack_get_child_by_name(st, "appearance") == pg2);
    assert(fdk_stack_get_child_by_name(st, "nope") == NULL);
    assert(strcmp(fdk_stack_page_name(st, 1), "appearance") == 0);
    /* NULL title falls back to the name. */
    assert(strcmp(fdk_stack_page_title(st, 1), "appearance") == 0);
    assert(strcmp(fdk_stack_page_title(st, 0), "General") == 0);

    /* Duplicate and NULL names refused. */
    assert(fdk_stack_add(st, plain_child(st, 5, 5), "general", NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_stack_add(st, plain_child(st, 5, 5), NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* First page visible; the others invisible; current by name. */
    assert((pg1->flags & FDK_WF_VISIBLE) != 0);
    assert((pg2->flags & FDK_WF_VISIBLE) == 0);
    assert(strcmp(fdk_stack_get_visible_name(st), "general") == 0);
    assert(fdk_stack_get_visible_child(st) == pg1);

    /* The natural size is the MAX over pages. */
    fdk_widget_measure(st, &nat);
    assert(nat.width == 200 && nat.height == 150);

    /* Arrange: the current page owns the full bounds (LOCAL slot
     * coordinates — parent-relative, like every arrange). */
    fdk_widget_arrange(st, (fdk_rect){10, 10, 300, 200});
    assert(pg1->bounds.x == 0 && pg1->bounds.y == 0);
    assert(pg1->bounds.width == 300 && pg1->bounds.height == 200);

    /* Switch by name: visibility flips, callback fires once. */
    assert(fdk_stack_set_visible_name(st, "about") == FDK_OK);
    assert(g_stack_changes == 1 && g_stack_last_index == 2 &&
           strcmp(g_stack_last_name, "about") == 0);
    assert((pg1->flags & FDK_WF_VISIBLE) == 0);
    assert((pg3->flags & FDK_WF_VISIBLE) != 0);
    assert(pg3->bounds.width == 300 && pg3->bounds.height == 200);
    /* Switching to the current page: no callback. */
    assert(fdk_stack_set_visible_name(st, "about") == FDK_OK);
    assert(g_stack_changes == 1);
    assert(fdk_stack_set_visible_name(st, "nope") == FDK_ERR_NOT_FOUND);
    /* Switch by widget. */
    assert(fdk_stack_set_visible_child(st, pg2) == FDK_OK);
    assert(g_stack_changes == 2 && g_stack_last_index == 1);
    assert(fdk_stack_set_visible_child(st, root) == FDK_ERR_NOT_FOUND);

    /* Titles are mutable; NULL restores the fallback. */
    assert(fdk_stack_set_page_title(st, 1, "Look & Feel") == FDK_OK);
    assert(strcmp(fdk_stack_page_title(st, 1), "Look & Feel") == 0);
    assert(fdk_stack_set_page_title(st, 1, NULL) == FDK_OK);
    assert(strcmp(fdk_stack_page_title(st, 1), "appearance") == 0);
    assert(fdk_stack_set_page_title(st, 9, "x") == FDK_ERR_INVALID_ARGUMENT);

    /* a11y: PANEL role, name = the current page's title. */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(st, &info)));
    assert(info.role == FDK_A11Y_ROLE_PANEL);
    assert(info.name != NULL && strcmp(info.name, "appearance") == 0);
    fdk_a11y_info_free(&info);

    /* The survivor rule (the notebook's): removing the current page
     * shows the page that shifted into its slot; removing an earlier
     * page keeps showing the same page. */
    g_stack_changes = 0;
    assert(fdk_stack_remove_page(st, 1) == FDK_OK); /* current, of 3 */
    assert(fdk_stack_page_count(st) == 2);
    assert(fdk_stack_get_visible_child(st) == pg3); /* shifted in    */
    assert(g_stack_changes == 1 && g_stack_last_index == 1);
    assert(fdk_stack_remove_page(st, 0) == FDK_OK); /* earlier page  */
    assert(fdk_stack_get_visible_child(st) == pg3); /* same page     */
    assert(fdk_stack_remove_page(st, 5) == FDK_ERR_INVALID_ARGUMENT);

    /* BOX pages — the regression example 12 found live: a page that
     * is itself a container must not send the notifier climbing
     * back into the stack (the sync uses the page's ARRANGE hook
     * now, plain set_bounds semantics, no cycle). A plain-page test
     * can never catch this: plain widgets never dispatch. */
    {
        fdk_widget *st2 = NULL;
        assert(fdk_ok(fdk_stack_create(root, &st2)));
        fdk_widget *bp = NULL;
        assert(fdk_ok(fdk_box_create(st2, FDK_VERTICAL, &bp)));
        fdk_widget *inner = NULL;
        assert(fdk_ok(fdk_label_create(bp, g_font, "nested", &inner)));
        assert(fdk_ok(fdk_stack_add(st2, bp, "box", "Box")));
        fdk_widget_arrange(st2, (fdk_rect){0, 0, 200, 100});
        assert(inner->bounds.width == 200); /* the box page packed   */
        /* Switching away and back re-arranges without recursion. */
        fdk_widget *bp2 = NULL;
        assert(fdk_ok(fdk_box_create(st2, FDK_VERTICAL, &bp2)));
        assert(fdk_ok(fdk_stack_add(st2, bp2, "box2", "Box2")));
        assert(fdk_ok(fdk_stack_set_visible_name(st2, "box2")));
        assert(fdk_ok(fdk_stack_set_visible_name(st2, "box")));
        assert((bp->flags & FDK_WF_VISIBLE) != 0);
    }

    fdk_widget_destroy(root);
    printf("[ok] stack: named pages, unique keys, title fallback + "
           "mutation, visibility switching with one callback per "
           "change, max-natural measure, the survivor rule, BOX "
           "pages without notifier recursion\n");
}

/* ---- Revealer (1.4.2) ---- */

static int g_reveal_events;
static bool g_reveal_last;

static void on_reveal(fdk_widget *rv, bool revealed, void *user) {
    (void)rv;
    (void)user;
    g_reveal_events++;
    g_reveal_last = revealed;
}

static void test_revealer(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *rv = NULL;
    assert(fdk_ok(fdk_revealer_create(root, &rv)));
    fdk_revealer_set_on_revealed(rv, on_reveal, NULL);
    g_reveal_events = 0;

    /* Hidden: 0x0 natural, the child invisible; second child refused. */
    fdk_widget *content = plain_child(rv, 200, 120);
    fdk_size nat;
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 0 && nat.height == 0);
    assert((content->flags & FDK_WF_VISIBLE) == 0);
    fdk_widget *extra = NULL;
    assert(fdk_widget_create(rv, NULL, (fdk_rect){0, 0, 10, 10}, &extra) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Defaults + the API's getters. */
    assert(fdk_revealer_get_transition(rv) == FDK_REVEAL_SLIDE_DOWN);
    assert(fdk_revealer_get_transition_duration(rv) == 160);
    assert(!fdk_revealer_get_reveal_child(rv));
    assert(!fdk_revealer_get_child_revealed(rv));

    /* The default flight (SLIDE_DOWN, 160 ms cubic-out) on the test
     * clock — the expander's discipline verbatim. */
    fdk_widget_arrange(rv, (fdk_rect){10, 10, 260, 120});
    anim_at(0);
    fdk_revealer_set_reveal_child(rv, true);
    assert((content->flags & FDK_WF_VISIBLE) != 0);
    anim_at(80); /* mid-flight */
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 200);
    assert(nat.height > 0 && nat.height < 120);
    assert(!fdk_revealer_get_child_revealed(rv));
    anim_at(200); /* landed */
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 200 && nat.height == 120);
    assert(fdk_revealer_get_child_revealed(rv));
    assert(g_reveal_events == 1 && g_reveal_last);

    /* The child is arranged at its FULL natural (the reveal is a
     * clip, never a squeeze) — LOCAL slot coordinates. */
    assert(content->bounds.x == 0 && content->bounds.y == 0);
    assert(content->bounds.width == 260 && content->bounds.height == 120);

    /* Collapse: the return flight; hidden at rest. */
    fdk_revealer_set_reveal_child(rv, false);
    anim_at(400);
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 0 && nat.height == 0);
    assert((content->flags & FDK_WF_VISIBLE) == 0);
    assert(g_reveal_events == 2 && !g_reveal_last);

    /* Retarget-from-live: depart from the CURRENT blend. */
    fdk_revealer_set_reveal_child(rv, true);
    anim_at(480);
    fdk_widget_measure(rv, &nat);
    fdk_i32 half_h = nat.height;
    assert(half_h > 0 && half_h < 120);
    fdk_revealer_set_reveal_child(rv, false);
    anim_at(490);
    fdk_widget_measure(rv, &nat);
    assert(nat.height < half_h);
    anim_at(700); /* land the return flight before what follows */

    /* SLIDE_UP anchoring on a plain parent: the door's BOTTOM edge
     * stays put while it opens (the origin rises). Hand-place the
     * full-open rect — bottom edge at y = 220. */
    anim_at(700);
    fdk_revealer_set_transition(rv, FDK_REVEAL_SLIDE_UP);
    fdk_widget_arrange(rv, (fdk_rect){10, 100, 200, 120});
    fdk_revealer_set_reveal_child(rv, true);
    anim_at(710); /* a crack open */
    assert(rv->bounds.height > 0 && rv->bounds.height < 120);
    assert(rv->bounds.y > 100 && rv->bounds.y < 220);
    assert(rv->bounds.y + rv->bounds.height == 220); /* bottom fixed */
    anim_at(900); /* landed */
    assert(rv->bounds.y == 100 && rv->bounds.height == 120);
    assert(fdk_revealer_get_child_revealed(rv));

    /* NONE: instant landing, no flight (the snap fires the callback
     * synchronously — the duration-0 contract). */
    anim_at(1000);
    fdk_revealer_set_transition(rv, FDK_REVEAL_NONE);
    assert(fdk_revealer_get_transition(rv) == FDK_REVEAL_NONE);
    g_reveal_events = 0;
    fdk_revealer_set_reveal_child(rv, false);
    assert(g_reveal_events == 1 && !g_reveal_last);
    assert(!fdk_revealer_get_child_revealed(rv));
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 0 && nat.height == 0);
    fdk_revealer_set_reveal_child(rv, true);
    assert(g_reveal_events == 2 && g_reveal_last);
    assert(fdk_revealer_get_child_revealed(rv));
    fdk_widget_measure(rv, &nat);
    assert(nat.width == 200 && nat.height == 120);

    /* The duration-0 snap behaves the same on a sliding door. */
    fdk_revealer_set_transition_duration(rv, 0);
    assert(fdk_revealer_get_transition_duration(rv) == 0);
    fdk_revealer_set_reveal_child(rv, false);
    assert(!fdk_revealer_get_child_revealed(rv));

    /* Duration bounds: > 10000 refused. */
    fdk_revealer_set_transition_duration(rv, 99999u);
    assert(fdk_revealer_get_transition_duration(rv) == 0);

    anim_at(0);
    fdk_widget_destroy(root);
    printf("[ok] revealer: 0x0 hidden natural, the 160-ms door on the "
           "test clock, full-natural child slots, SLIDE_UP bottom "
           "anchoring, NONE + duration-0 snaps, retargets from live\n");
}

/* ---- StackSwitcher (1.4.2) ---- */

static void test_stackswitcher(void) {
    fdk_widget *root = fresh_root();
    /* A standalone root paints nothing by default; give it the
     * theme's window surface so the INACTIVE pill's transparency
     * reads as the window background (what an app window shows). */
    fdk_widget_set_background(
        root, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    fdk_widget *st = NULL;
    assert(fdk_ok(fdk_stack_create(root, &st)));
    fdk_widget *pg1 = plain_child(st, 100, 80);
    fdk_widget *pg2 = plain_child(st, 100, 80);
    assert(fdk_stack_add(st, pg1, "one", "First") == FDK_OK);
    assert(fdk_stack_add(st, pg2, "two", "Second") == FDK_OK);

    fdk_widget *sw = NULL;
    assert(fdk_ok(fdk_stackswitcher_create(root, g_font, &sw)));
    assert(fdk_stackswitcher_get_stack(sw) == NULL); /* unbound     */
    /* A non-stack is refused; NULL stays unbound. */
    fdk_stackswitcher_set_stack(sw, root);
    assert(fdk_stackswitcher_get_stack(sw) == NULL);

    /* Binding: the pill cache builds (visible through measure). */
    fdk_stackswitcher_set_stack(sw, st);
    assert(fdk_stackswitcher_get_stack(sw) == st);
    fdk_size nat;
    fdk_widget_measure(sw, &nat);
    assert(nat.width > 40); /* two titled pills + gap              */
    assert(nat.height == 30);
    fdk_widget_arrange(sw, (fdk_rect){0, 0, nat.width, 30});
    fdk_widget_arrange(st, (fdk_rect){0, 40, 200, 80});

    /* Paint: the ACTIVE pill carries the accent fill (page 0). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 140, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    fdk_color accent = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    /* Pill 0 spans [0, w0); sample at its center. */
    fdk_i32 tw = 0, th = 0;
    fdk__text_extent(g_font, "First", &tw, &th);
    fdk_i32 w0 = tw + 28;
    fdk_u32 p0 = px_at(s, w0 / 2, 3); /* y=3: clear of the text */
    assert(p0 == pack_color(accent)); /* the active pill            */
    /* Pill 1 (inactive) is flat: the window background. */
    fdk__text_extent(g_font, "Second", &tw, &th);
    fdk_i32 w1 = tw + 28;
    fdk_u32 p1 = px_at(s, w0 + 4 + w1 / 2, 3);
    fdk_color winbg =
        fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND);
    assert(p1 == pack_color(winbg));

    /* a11y: TAB_LIST with one virtual TAB per pill; ACTIVATE
     * switches; SELECTED rides the active page. */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(sw, &info)));
    assert(info.role == FDK_A11Y_ROLE_TAB_LIST);
    fdk_a11y_info_free(&info);
    assert(fdk_ok(fdk_a11y_virtual_describe(sw, 1, &info)));
    assert(info.role == FDK_A11Y_ROLE_TAB);
    assert(info.name != NULL && strcmp(info.name, "Second") == 0);
    assert((info.states & FDK_A11Y_SELECTED) == 0);
    fdk_a11y_info_free(&info);
    assert(fdk_ok(fdk_a11y_virtual_perform(sw, 1,
                                           FDK_A11Y_ACTION_ACTIVATE,
                                           0.0)));
    assert(fdk_stack_get_visible_child(st) == pg2);
    assert(fdk_ok(fdk_a11y_virtual_describe(sw, 1, &info)));
    assert((info.states & FDK_A11Y_SELECTED) != 0);
    fdk_a11y_info_free(&info);

    /* Clicking a pill switches (the cached-name path). */
    click(root, (float)(w0 + 4 + w1 / 2), 15.0f);
    assert(fdk_stack_get_visible_child(st) == pg2);
    /* Stack death by another hand: the switcher unbinds cleanly. */
    fdk_widget_destroy(st);
    assert(fdk_stackswitcher_get_stack(sw) == NULL);
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s); /* paints the empty row, no UAF */
    fdk_size nat2;
    fdk_widget_measure(sw, &nat2);
    assert(nat2.width == 40); /* SWITCHER_MIN_W, no pills           */

    /* Rebind after a mutation: the cache rebuilds at paint/measure. */
    fdk_widget *st2 = NULL;
    assert(fdk_ok(fdk_stack_create(root, &st2)));
    assert(fdk_stack_add(st2, plain_child(st2, 10, 10), "x",
                         "About FDK") == FDK_OK);
    fdk_stackswitcher_set_stack(sw, st2);
    fdk_widget_measure(sw, &nat2);
    assert(nat2.width > 40); /* one real pill rebuilt                */

    /* NULL unbinds. */
    fdk_stackswitcher_set_stack(sw, NULL);
    assert(fdk_stackswitcher_get_stack(sw) == NULL);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] stackswitcher: binding + pill cache rebuild, accent "
           "active pill + flat inactive (pixels), TAB virtuals with "
           "ACTIVATE/SELECTED, click switching, stack-death unbind\n");
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        g_font = fdk_font_load(candidates[i], 16);
        if (g_font != NULL) {
            break;
        }
    }
    if (g_font == NULL) {
        printf("[skip] no system TrueType font found — the containers' "
               "measured geometry needs real glyphs; see "
               "docs/testing.md\n");
        return 0;
    }

    test_paned_layout();
    test_paned_drag();
    test_paned_keyboard();
    test_expander_layout();
    test_expander_interaction();
    test_row_icons();
    test_stack();
    test_revealer();
    test_stackswitcher();

    fdk_font_destroy(g_font);
    printf("all container tests passed\n");
    return 0;
}
