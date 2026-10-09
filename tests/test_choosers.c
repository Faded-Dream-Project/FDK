/* test_choosers.c — headless tests for the 1.4.3 batch: the vertical
 * slider, the search debounce, the MenuButton, the paint group (the
 * revealer's crossfade engine), the tree's multi-selection + rubber
 * band, and the font enumeration surface.
 *
 * Same discipline as the catalog suites: standalone roots, offscreen
 * surfaces, synthetic window events through
 * fdk_widget_tree_handle_event, the animation test clock driving the
 * crossfade exactly as the pump would under a window, ASan+UBSan
 * throughout. A system font is needed for the text-bearing geometry;
 * without one those groups honestly skip. The chooser DIALOGS and
 * the popup/auto-scroll behaviors that need a real window live in
 * the X11 integration suite (test_x11_integration.c), not here.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* internal state + helpers   */
#include "widget/widget_internal.h"  /* paint alpha + the clock    */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

/* ---- helpers (event shapes mirror test_controls.c) ---- */

static fdk_event_data ev_button_at(fdk_event_type t, float x, float y,
                                   fdk_u32 mods) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    e.pointer_button.modifiers = mods;
    return e;
}

static fdk_event_data ev_motion_at(float x, float y, fdk_u32 mods) {
    (void)mods; /* motion carries no modifiers (the press's do) */
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

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

static void paint_root(fdk_widget *root, fdk_surface **out_s) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    *out_s = s;
}

/* ===================================================================
 * The vertical slider
 * =================================================================== */

static void test_slider_vertical(void) {
    if (g_font == NULL) {
        printf("  [skip] slider vertical (no system font)\n");
        return;
    }
    fdk_widget *root = fresh_root();

    fdk_widget *sl = NULL;
    assert(fdk_ok(fdk_slider_create(root, 0.0, 100.0, 50.0, &sl)));
    fdk_size nat;
    fdk_widget_measure(sl, &nat);
    assert(fdk_slider_get_orientation(sl) == FDK_SLIDER_HORIZONTAL);
    assert(nat.width == 40 && nat.height == 24); /* the v1 geometry */

    /* Vertical: the cross extents swap. */
    fdk_slider_set_orientation(sl, FDK_SLIDER_VERTICAL);
    assert(fdk_slider_get_orientation(sl) == FDK_SLIDER_VERTICAL);
    fdk_widget_measure(sl, &nat);
    assert(nat.width == 24 && nat.height == 40);

    /* Labeled marks widen the WIDTH (the mirror of the horizontal's
     * height growth). */
    assert(fdk_ok(fdk_slider_add_mark(sl, 0.0, "min")));
    assert(fdk_ok(fdk_slider_add_mark(sl, 100.0, "max")));
    fdk_widget_measure(sl, &nat);
    assert(nat.width > 24);
    assert(nat.height == 40); /* the main axis is untouched */

    /* Value-at rotates: a press near the BOTTOM is the minimum. */
    fdk_widget_arrange(sl, (fdk_rect){10, 10, 24, 200});
    fdk_event_data down =
        ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 12.0f, 200.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    assert(fdk_slider_get_value(sl) < 10.0); /* bottom = min */

    /* A press near the TOP is the maximum. */
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 12.0f, 12.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    assert(fdk_slider_get_value(sl) > 90.0);

    /* Keyboard: Up raises, Down lowers (unchanged semantics). */
    fdk_widget_focus(sl);
    double at_top = fdk_slider_get_value(sl);
    fdk_event_data down_key =
        ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_DOWN);
    (void)fdk_widget_tree_handle_event(root, &down_key);
    double after_down = fdk_slider_get_value(sl);
    assert(after_down < at_top); /* one step below the top press */
    fdk_event_data ku = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_UP);
    (void)fdk_widget_tree_handle_event(root, &ku);
    assert(fdk_slider_get_value(sl) > after_down);

    /* The vertical paint lands: track ink up the middle column. */
    fdk_surface *s = NULL;
    paint_root(root, &s);
    /* The track at the value-axis center: x = 10 + 12 (thumb-less
     * center), somewhere along y. Sample the fill run: at the
     * CURRENT value the fill spans bottom..thumb. */
    bool saw_ink = false;
    for (int y = 20; y < 200; y += 4) {
        if (px_at(s, 22, y) != 0) {
            saw_ink = true;
            break;
        }
    }
    assert(saw_ink);
    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("  [ok] slider vertical: swapped naturals, bottom-min "
           "value-at, rotated marks, paint ink\n");
}

/* ===================================================================
 * The search debounce
 * =================================================================== */

typedef struct search_probe {
    int fires;
    char last[64];
} search_probe;

static void probe_search(fdk_widget *entry, const char *text,
                         void *user) {
    (void)entry;
    search_probe *p = user;
    p->fires++;
    snprintf(p->last, sizeof(p->last), "%s", text ? text : "");
}

static void test_search_debounce(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *e = NULL;
    assert(fdk_ok(fdk_search_entry_create(root, g_font, &e)));

    search_probe probe;
    memset(&probe, 0, sizeof(probe));
    fdk_search_entry_set_on_search(e, probe_search, &probe);

    /* Detached tree: no context, no timer — the callback fires
     * IMMEDIATELY (the animation layer's honesty rule). */
    (void)fdk_entry_set_text(e, "abc");
    assert(probe.fires == 1);
    assert(strcmp(probe.last, "abc") == 0);
    assert(!fdk_search_entry_has_pending(e));

    /* Debounce 0 stays immediate. */
    fdk_search_entry_set_debounce(e, 0);
    (void)fdk_entry_set_text(e, "abcd");
    assert(probe.fires == 2);

    /* A non-zero debounce on a detached tree still fires immediately
     * (the snap rule); has_pending stays false — there is no timer. */
    fdk_search_entry_set_debounce(e, 250);
    (void)fdk_entry_set_text(e, "abcde");
    assert(probe.fires == 3);
    assert(strcmp(probe.last, "abcde") == 0);
    assert(!fdk_search_entry_has_pending(e));

    /* Swapping the listener cancels nothing pending (there is
     * nothing TO cancel detached) and the new one fires next. */
    search_probe second;
    memset(&second, 0, sizeof(second));
    fdk_search_entry_set_on_search(e, probe_search, &second);
    (void)fdk_entry_set_text(e, "xyz");
    assert(probe.fires == 3); /* the first heard nothing new */
    assert(second.fires == 1);
    assert(strcmp(second.last, "xyz") == 0);

    /* Plain entries ignore the search API (documented no-ops). */
    fdk_widget *plain = NULL;
    assert(fdk_ok(fdk_entry_create(root, g_font, "", &plain)));
    fdk_search_entry_set_on_search(plain, probe_search, &second);
    (void)fdk_entry_set_text(plain, "nope");
    assert(second.fires == 1); /* unchanged: not a search entry */

    fdk_widget_destroy(root);
    printf("  [ok] search debounce: detached snap-fire, listener "
           "swap, non-search no-op\n");
}

/* ===================================================================
 * The MenuButton
 * =================================================================== */

static void test_menu_button(void) {
    if (g_font == NULL) {
        printf("  [skip] menu button (no system font)\n");
        return;
    }
    fdk_widget *root = fresh_root();
    fdk_widget *mb = NULL;
    assert(fdk_ok(fdk_menu_button_create(root, g_font, "Options",
                                         &mb)));

    /* The arrow zone widens the natural size; NONE gives it back. */
    fdk_size nat;
    fdk_widget_measure(mb, &nat);
    fdk_i32 with_arrow = nat.width;
    fdk_menu_button_set_arrow(mb, FDK_MENU_BUTTON_ARROW_NONE);
    fdk_widget_measure(mb, &nat);
    assert(nat.width + 20 == with_arrow); /* zone (16) + gap (4) */
    fdk_menu_button_set_arrow(mb, FDK_MENU_BUTTON_ARROW_DOWN);

    /* The model is borrowed: set/get round-trips. */
    fdk_menu *model = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &model)));
    fdk_menu_item *it = NULL;
    assert(fdk_ok(fdk_menu_append(model, "Alpha", &it)));
    assert(fdk_ok(fdk_menu_button_set_menu(mb, model)));
    assert(fdk_menu_button_get_menu(mb) == model);
    assert(!fdk_menu_button_is_open(mb)); /* no window: never open */

    /* A11y: the MENU_BUTTON role + HAS_POPUP, EXPANDED while shut. */
    fdk_a11y_info info;
    memset(&info, 0, sizeof(info));
    assert(fdk_ok(fdk_a11y_describe(mb, &info)));
    assert(info.role == FDK_A11Y_ROLE_MENU_BUTTON);
    assert((info.states & FDK_A11Y_HAS_POPUP) != 0);
    assert((info.states & FDK_A11Y_EXPANDED) == 0);
    assert(info.name != NULL && strcmp(info.name, "Options") == 0);
    free(info.name);

    /* The paint lands (button chrome + chevron) — ink somewhere in
     * the arranged bounds. */
    fdk_widget_arrange(mb, (fdk_rect){10, 10, 120, 36});
    fdk_surface *s = NULL;
    paint_root(root, &s);
    bool saw_ink = false;
    for (int y = 14; y < 44; y += 2) {
        for (int x = 14; x < 126; x += 4) {
            if (px_at(s, x, y) != 0) {
                saw_ink = true;
                break;
            }
        }
        if (saw_ink) {
            break;
        }
    }
    assert(saw_ink);
    fdk_surface_destroy(s);

    /* The model stays OURS after the button dies. */
    fdk_widget_destroy(root);
    assert(strcmp(fdk_menu_item_text(it), "Alpha") == 0);
    fdk_menu_destroy(model);
    printf("  [ok] menu button: arrow sizing, borrowed model, a11y "
           "role/states, paint ink\n");
}

/* ===================================================================
 * The paint group (the crossfade engine)
 * =================================================================== */

static void test_paint_group_alpha(void) {
    fdk_widget *root = fresh_root();
    /* A red panel inside the group candidate. */
    fdk_widget *panel = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){20, 20, 100, 80},
                                    &panel)));
    fdk_color red = {1.0f, 0.0f, 0.0f, 1.0f};
    fdk_widget_set_background(panel, red);

    /* Baseline: opaque paint = the exact red. */
    fdk_surface *s = NULL;
    paint_root(root, &s);
    fdk_u32 full = px_at(s, 60, 50);
    assert(full == 0xFF0000u);
    fdk_surface_destroy(s);

    /* Half alpha: the panel composites at 50% over the BLACK root
     * background — pixel value ~0x800000 (blended). */
    fdk__widget_set_paint_alpha(panel, 0.5f);
    paint_root(root, &s);
    fdk_u32 half = px_at(s, 60, 50);
    assert(half > 0x600000 && half < 0xA00000); /* ~50% red */
    fdk_surface_destroy(s);

    /* Quarter alpha for good measure. */
    fdk__widget_set_paint_alpha(panel, 0.25f);
    paint_root(root, &s);
    fdk_u32 quarter = px_at(s, 60, 50);
    assert(quarter < half); /* monotonic: more transparent = dimmer */
    assert(quarter > 0x200000 && quarter < 0x600000);
    fdk_surface_destroy(s);

    /* Outside the group: the background is untouched black. */
    paint_root(root, &s);
    assert(px_at(s, 200, 50) == 0); /* wait — the panel covers only
                                       20..120; 200 is outside */
    fdk_surface_destroy(s);

    /* Back to 1.0: the plain walk, the exact red again. */
    fdk__widget_set_paint_alpha(panel, 1.0f);
    paint_root(root, &s);
    assert(px_at(s, 60, 50) == 0xFF0000u);
    fdk_surface_destroy(s);

    fdk_widget_destroy(root);
    printf("  [ok] paint group: 50%%/25%% blends land between the "
           "endpoints, 1.0 restores the plain walk\n");
}

static void test_revealer_crossfade(void) {
    if (g_font == NULL) {
        printf("  [skip] revealer crossfade (no system font)\n");
        return;
    }
    fdk_widget *root = fresh_root();
    fdk_widget *rv = NULL;
    assert(fdk_ok(fdk_revealer_create(root, &rv)));
    fdk_revealer_set_transition(rv, FDK_REVEAL_CROSSFADE);
    assert(fdk_revealer_get_transition(rv) == FDK_REVEAL_CROSSFADE);

    fdk_widget *child = NULL;
    assert(fdk_ok(fdk_widget_create(rv, NULL,
                                    (fdk_rect){0, 0, 100, 80},
                                    &child)));
    fdk_color green = {0.0f, 1.0f, 0.0f, 1.0f};
    fdk_widget_set_background(child, green);

    /* Hidden: nothing paints. */
    fdk_surface *s = NULL;
    paint_root(root, &s);
    assert(px_at(s, 40, 30) == 0);
    fdk_surface_destroy(s);

    /* Opening: the flight starts; at the halfway tick the blend is
     * between black and green (the door keeps FULL geometry — a fade
     * is opacity, not size). The clock is primed to 0 first (the
     * flight stamps its start from the pinned clock). */
    anim_at(0);
    fdk_revealer_set_reveal_child(rv, true);
    anim_at(80); /* halfway through the default 160ms cubic-out */
    fdk_widget_arrange(rv, (fdk_rect){20, 20, 100, 80});
    paint_root(root, &s);
    fdk_u32 mid = px_at(s, 60, 50);
    assert(mid > 0 && mid < 0x00FF00); /* some green, not full */
    /* The door is FULL-SIZE at every blend (the crossfade rule):
     * the pixel at the child's far corner is also tinted. */
    fdk_u32 corner = px_at(s, 115, 95);
    assert(corner > 0 && corner < 0x00FF00);
    fdk_surface_destroy(s);

    /* Landed: full green, the plain walk again (alpha 1.0). */
    anim_at(400);
    paint_root(root, &s);
    assert(px_at(s, 60, 50) == 0x00FF00u);
    fdk_surface_destroy(s);

    /* Closing: the fade runs back down to nothing. */
    anim_at(400);
    fdk_revealer_set_reveal_child(rv, false);
    anim_at(480); /* mid-return: still visible, partially faded */
    paint_root(root, &s);
    fdk_u32 closing = px_at(s, 60, 50);
    assert(closing > 0 && closing < 0x00FF00); /* fading, not gone */
    fdk_surface_destroy(s);
    anim_at(800);
    paint_root(root, &s);
    assert(px_at(s, 60, 50) == 0); /* fully hidden, nothing paints */
    fdk_surface_destroy(s);

    fdk_widget_destroy(root);
    printf("  [ok] revealer crossfade: full-size door, mid-flight "
           "blend, landed green, hidden black\n");
}

/* ===================================================================
 * The tree's multi-selection + rubber band
 * =================================================================== */

static void test_tree_multi_select(void) {
    if (g_font == NULL) {
        printf("  [skip] tree multi-select (no system font)\n");
        return;
    }
    fdk_widget *root = fresh_root();
    fdk_widget *tree = NULL;
    assert(fdk_ok(fdk_tree_create(root, g_font, &tree)));
    assert(fdk_tree_get_selection_mode(tree) ==
           FDK_TREE_SELECTION_SINGLE);

    fdk_tree_node a, b, c, d, e;
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "A", &a)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "B", &b)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "C", &c)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "D", &d)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "E", &e)));
    fdk_widget_arrange(tree, (fdk_rect){0, 0, 300, 200});

    /* Row geometry: find the row widget for a node (the visible
     * sequence is creation order here). */
    assert(fdk_tree_visible_count(tree) == 5);

    fdk_tree_set_selection_mode(tree, FDK_TREE_SELECTION_MULTIPLE);
    assert(fdk_tree_get_selection_mode(tree) ==
           FDK_TREE_SELECTION_MULTIPLE);

    /* Every click is a full press+release pair: the implicit grab
     * would otherwise route the NEXT press to the last row hit
     * (press-to-press without a release is not a click). */
    fdk_i32 rh = 24; /* the system font's row height (measured) */
    fdk_size nat;
    fdk_widget_measure(tree, &nat);
    if (nat.height >= 5) {
        rh = nat.height / 5;
    }

    /* Plain click on row 1 (B): selects only B. */
    fdk_event_data down =
        ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 40.0f,
                     (float)(rh + rh / 2), 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    fdk_event_data up =
        ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 40.0f,
                     (float)(rh + rh / 2), 0);
    (void)fdk_widget_tree_handle_event(root, &up);
    fdk_tree_node sel[8];
    size_t n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 1 && sel[0] == b);

    /* Ctrl+click on row 3 (D): union — B and D. */
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 40.0f,
                        (float)(3 * rh + rh / 2), FDK_MOD_CTRL);
    (void)fdk_widget_tree_handle_event(root, &down);
    up = ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 40.0f,
                      (float)(3 * rh + rh / 2), FDK_MOD_CTRL);
    (void)fdk_widget_tree_handle_event(root, &up);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 2 && sel[0] == b && sel[1] == d);

    /* Shift+click on row 0 (A): the range from the anchor (D, slot
     * 3) to slot 0 — A..D replace everything. */
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 40.0f,
                        (float)(rh / 2), FDK_MOD_SHIFT);
    (void)fdk_widget_tree_handle_event(root, &down);
    up = ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 40.0f,
                      (float)(rh / 2), FDK_MOD_SHIFT);
    (void)fdk_widget_tree_handle_event(root, &up);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 4);
    assert(sel[0] == a && sel[1] == b && sel[2] == c && sel[3] == d);

    /* The rubber band: a press on EMPTY tree space (below the last
     * row) starts a sweep; dragging UP over the rows selects them. */
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 150.0f, 180.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    fdk_event_data motion =
        ev_motion_at(160.0f, (float)(rh / 2), 0);
    (void)fdk_widget_tree_handle_event(root, &motion);
    /* The band spans y in [rh/2, 180] — every row intersects it. */
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 5); /* A..E: the sweep replaced the old selection */

    /* Release ends the gesture; the band disappears. */
    up = ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 160.0f, 20.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &up);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 5); /* the selection stays after the release */

    /* Ctrl-sweep unions over the pre-band state. */
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 150.0f, 180.0f,
                        FDK_MOD_CTRL);
    (void)fdk_widget_tree_handle_event(root, &down);
    /* A zero-extent ctrl band keeps everything (the union rule). */
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 5);
    up = ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 150.0f, 180.0f, 0);
    (void)fdk_widget_tree_handle_event(root, &up);

    /* Mode switch resets to the classic single read: the FIRST
     * selected node, or none. */
    fdk_tree_set_selection_mode(tree, FDK_TREE_SELECTION_SINGLE);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 1); /* the collapsed-to-first rule */
    assert(fdk_tree_get_selected(tree) == sel[0]);

    /* NONE ignores selection input entirely. */
    fdk_tree_set_selection_mode(tree, FDK_TREE_SELECTION_NONE);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 0);
    down = ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 40.0f,
                        (float)(rh + rh / 2), 0);
    (void)fdk_widget_tree_handle_event(root, &down);
    n = fdk_tree_get_selected_nodes(tree, sel, 8);
    assert(n == 0);

    fdk_widget_destroy(root);
    printf("  [ok] tree multi-select: plain/ctrl/shift clicks, the "
           "sweep, ctrl-union, mode resets\n");
}

/* ===================================================================
 * The font enumeration surface
 * =================================================================== */

static void test_font_enumerate(void) {
    fdk_font_info *infos = NULL;
    size_t count = 0;
    fdk_result r = fdk_font_enumerate(&infos, &count);
    assert(fdk_ok(r));
    if (count == 0) {
        printf("  [skip] font enumerate (no system fonts)\n");
        return;
    }
    printf("  [..] enumerated %zu faces\n", count);

    /* Every entry is complete and sorted. */
    for (size_t i = 0; i < count; i++) {
        assert(infos[i].family != NULL && infos[i].family[0] != '\0');
        assert(infos[i].style != NULL);
        assert(infos[i].path != NULL && infos[i].path[0] == '/');
        assert(infos[i].face_index >= 0);
    }
    for (size_t i = 1; i < count; i++) {
        int c = strcmp(infos[i - 1].family, infos[i].family);
        assert(c <= 0);
        if (c == 0) {
            assert(strcmp(infos[i - 1].style, infos[i].style) <= 0);
        }
    }

    /* The first entry loads through the face-index API. */
    fdk_font *f = fdk_font_load_face(infos[0].path,
                                     infos[0].face_index, 16);
    assert(f != NULL);
    fdk_font_destroy(f);

    /* fdk_font_load is face 0 (the documented equivalence). */
    f = fdk_font_load(infos[0].path, 16);
    assert(f != NULL);
    fdk_font_destroy(f);

    /* Negative face indices are refused. */
    assert(fdk_font_load_face(infos[0].path, -1, 16) == NULL);

    fdk_font_info_list_free(infos, count);
    /* A double free would be caught by ASan — the point of running
     * this line at all. */
    printf("  [ok] font enumerate: %zu sorted faces, load_face "
           "round-trip, list free\n", count);
}

/* ===================================================================
 * main
 * =================================================================== */

/* ---- Color button (1.4.4) ---- */

static void test_color_button(void); /* defined below (this file's
                                       * post-main definition style) */

static int g_color_sets = 0;
static fdk_color g_last_color = {0, 0, 0, 0};
static void on_color_set(fdk_widget *button, fdk_color color,
                         void *user) {
    (void)button;
    (void)user;
    g_color_sets++;
    g_last_color = color;
}

static void test_color_button(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cb = NULL;
    fdk_color red = {0.85f, 0.15f, 0.15f, 1.0f};
    assert(fdk_color_button_create(root, red, &cb) == FDK_OK);
    assert(fdk_color_button_create(NULL, red, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* get/set + idempotence; set does NOT fire the gesture
     * callback (the checked-setter discipline). */
    fdk_color got = fdk_color_button_get_color(cb);
    assert(got.r == red.r && got.g == red.g && got.b == red.b &&
           got.a == red.a);
    assert(fdk_color_button_get_color(NULL).a == 0.0f);
    fdk_color blue = {0.15f, 0.35f, 0.90f, 1.0f};
    g_color_sets = 0;
    fdk_color_button_set_on_color_set(cb, on_color_set, NULL);
    assert(fdk_color_button_set_color(cb, blue) == FDK_OK);
    got = fdk_color_button_get_color(cb);
    assert(got.b > 0.8f);
    assert(g_color_sets == 0);
    assert(fdk_color_button_set_color(cb, blue) == FDK_OK); /* idem */
    assert(fdk_color_button_set_color(NULL, blue) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_color_button_set_on_color_set(NULL, on_color_set, NULL);

    /* Title (the chooser's window title). */
    assert(fdk_color_button_set_title(cb, "Pick a tint") == FDK_OK);
    assert(fdk_color_button_set_title(cb, NULL) == FDK_OK);

    /* Natural size + focusability. */
    fdk_size nat = {0, 0};
    fdk_widget_measure(cb, &nat);
    assert(nat.width == 44 && nat.height == 30);
    assert(fdk_widget_get_can_focus(cb));

    /* Paint: the well center is the color (over the checkerboard);
     * an alpha=0 color shows the checkerboard alone (two-tone
     * pixels within a 6-px cell); the opaque blue center is solid. */
    fdk_widget_arrange(cb, (fdk_rect){10, 10, 44, 30});
    fdk_surface *s = NULL;
    paint_root(root, &s);
    fdk_u32 mid = px_at(s, 32, 25); /* the well's center            */
    fdk_u32 expect_blue =
        ((fdk_u32)(0.15f * 255.0f + 0.5f) << 16) |
        ((fdk_u32)(0.35f * 255.0f + 0.5f) << 8) |
        (fdk_u32)(0.90f * 255.0f + 0.5f);
    assert(mid == expect_blue);

    /* Alpha honesty: a translucent color shows the checkerboard
     * THROUGH it (the well's two-tone pattern under the tint). */
    assert(fdk_color_button_set_color(
               cb, (fdk_color){1.0f, 1.0f, 1.0f, 0.5f}) == FDK_OK);
    fdk_widget_invalidate(cb);
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    /* The well spans (15..49, 15..39): sample a 6-px checkerboard
     * cell pair — one light, one dark, both tinted toward white. */
    fdk_u32 c1 = px_at(s, 20, 20);
    fdk_u32 c2 = px_at(s, 27, 21);
    assert(c1 != c2); /* the two board tones differ                */

    /* The acceptance seam: an ACCEPTED chooser result writes the
     * color and fires the callback; CANCELLED changes nothing. */
    g_color_sets = 0;
    fdk_color_dialog_result acc = {
        .outcome = FDK_COLOR_DIALOG_ACCEPTED,
        .color = {0.2f, 0.7f, 0.3f, 1.0f},
    };
    fdk__colorbutton_apply_result(cb, &acc);
    got = fdk_color_button_get_color(cb);
    assert(got.g > 0.6f && got.r < 0.3f);
    assert(g_color_sets == 1 && g_last_color.g > 0.6f);
    fdk_color_dialog_result can = {
        .outcome = FDK_COLOR_DIALOG_CANCELLED,
        .color = {0, 0, 0, 1},
    };
    fdk__colorbutton_apply_result(cb, &can);
    got = fdk_color_button_get_color(cb);
    assert(got.g > 0.6f); /* untouched */
    assert(g_color_sets == 1);
    fdk__colorbutton_apply_result(NULL, &acc); /* argument safety   */

    /* The a11y contract: a BUTTON whose value text is the hex. */
    fdk_a11y_info info;
    memset(&info, 0, sizeof(info));
    assert(fdk_a11y_describe(cb, &info) == FDK_OK);
    assert(info.value_text != NULL);
    assert(info.value_text[0] == '#');
    assert(strlen(info.value_text) == 7);
    fdk_a11y_info_free(&info);
    assert(fdk_a11y_actions_of(cb) & FDK_A11Y_ACTION_ACTIVATE);

    /* Presses in a DETACHED tree are a documented no-op (no
     * context to show a window on) — the button survives, state
     * intact, and the press is still consumed. */
    g_color_sets = 0;
    fdk_event_data down =
        ev_button_at(FDK_EVENT_POINTER_BUTTON_DOWN, 30.0f, 25.0f, 0);
    fdk_event_data up =
        ev_button_at(FDK_EVENT_POINTER_BUTTON_UP, 30.0f, 25.0f, 0);
    assert(fdk_widget_tree_handle_event(root, &down) == true);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(g_color_sets == 0);
    /* Keyboard: Space opens (also a no-op detached) + consumes. */
    fdk_event_data sp = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_SPACE);
    assert(fdk_widget_tree_handle_event(root, &sp) == true);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] color button: create/get/set (silent setter), "
           "title, 44x30 natural, opaque + checkerboard-under-alpha "
           "pixel proofs, ACCEPTED/CANCELLED seam, hex value text + "
           "ACTIVATE action, detached no-op presses\n");
}

int main(void) {
    g_font = fdk_font_load_system_default(14);
    if (g_font == NULL) {
        printf("test_choosers: no system font — geometry groups "
               "skip, logic groups still run\n");
    }

    test_slider_vertical();
    test_search_debounce();
    test_menu_button();
    test_paint_group_alpha();
    test_revealer_crossfade();
    test_tree_multi_select();
    test_font_enumerate();
    test_color_button();

    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    fdk__animation_drain_all();
    printf("all 1.4.3 chooser-suite tests passed\n");
    return 0;
}
