/*
 * test_widgets2.c — headless tests for the Phase 9 control family:
 * Slider, SpinButton, Toolbar, Notebook, Canvas.
 *
 * Same discipline as the other headless suites: standalone roots,
 * synthetic events through the tree, offscreen surfaces, ASan+UBSan.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* fdk__text_extent (tab rect math) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

static fdk_event_data ev_button(fdk_event_type t, float x, float y) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.pointer_button.position.x = x;
    e.pointer_button.position.y = y;
    e.pointer_button.button = 1;
    return e;
}

static void click(fdk_widget *root, float x, float y) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
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

/* ---- Slider ---- */

static int g_slider_changes = 0;
static void on_slider(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_slider_changes++;
}

static void test_slider(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sl = NULL;
    assert(fdk_slider_create(root, 0, 100, 50, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_slider_create(root, 100, 0, 50, &sl) ==
           FDK_ERR_INVALID_ARGUMENT); /* max < min */
    assert(fdk_slider_create(root, 0, 100, 50, &sl) == FDK_OK);
    fdk_widget_set_bounds(sl, (fdk_rect){0, 0, 200, 24});
    g_slider_changes = 0;
    fdk_slider_set_on_changed(sl, on_slider, NULL);

    assert(fdk_slider_get_value(sl) == 50.0);
    /* Programmatic set fires. */
    fdk_slider_set_value(sl, 70);
    assert(fdk_slider_get_value(sl) == 70.0);
    assert(g_slider_changes == 1);
    /* Clamping. */
    fdk_slider_set_value(sl, 500);
    assert(fdk_slider_get_value(sl) == 100.0);
    fdk_slider_set_value(sl, -5);
    assert(fdk_slider_get_value(sl) == 0.0);

    /* Range change re-clamps. */
    fdk_slider_set_range(sl, 0, 10);
    assert(fdk_slider_get_value(sl) == 0.0);

    /* Quantization: step 5 snaps mid values. */
    fdk_slider_set_step(sl, 5.0);
    fdk_slider_set_value(sl, 7.0);
    assert(fdk_slider_get_value(sl) == 5.0);

    /* Pointer: press at the far right edge jumps to max. */
    fdk_slider_set_range(sl, 0, 100);
    fdk_slider_set_step(sl, 0.0);
    fdk_slider_set_value(sl, 0);
    fdk_widget_set_can_focus(sl, true);
    assert(fdk_widget_focus(sl));
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN,
                                    193.0f, 12.0f);
    assert(fdk_widget_tree_handle_event(root, &down));
    assert(fdk_slider_get_value(sl) >= 95.0);
    /* Drag back left (implicit grab keeps motion events coming). */
    fdk_event_data motion = {
        .type = FDK_EVENT_POINTER_MOTION,
    };
    motion.pointer.position.x = 7.0f;
    motion.pointer.position.y = 12.0f;
    assert(fdk_widget_tree_handle_event(root, &motion));
    assert(fdk_slider_get_value(sl) <= 5.0);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 7.0f,
                                  12.0f);
    assert(fdk_widget_tree_handle_event(root, &up));

    /* Keyboard: arrows step (range/100), page = 10%, home/end. */
    fdk_event_data k = ev_key(FDK_KEY_RIGHT);
    fdk_slider_set_value(sl, 50);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_slider_get_value(sl) == 51.0);
    fdk_event_data pgup = ev_key(FDK_KEY_PAGE_UP);
    assert(fdk_widget_tree_handle_event(root, &pgup));
    assert(fdk_slider_get_value(sl) == 61.0);
    k = ev_key(FDK_KEY_HOME);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_slider_get_value(sl) == 0.0);
    k = ev_key(FDK_KEY_END);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_slider_get_value(sl) == 100.0);

    /* Type checks. */
    assert(fdk_slider_get_value(root) == 0.0);
    fdk_slider_set_value(root, 5);

    fdk_widget_destroy(root);
    printf("[ok] slider: range/step/clamps, pointer jump+drag, "
           "keyboard stepping, programmatic set\n");
}

/* ---- SpinButton ---- */

static int g_spin_changes = 0;
static void on_spin(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    g_spin_changes++;
}

static void test_spin(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *sp = NULL;
    assert(fdk_spin_create(root, g_font, 0, 100, 50, &sp) == FDK_OK);
    fdk_widget_set_bounds(sp, (fdk_rect){0, 0, 120, 28});
    g_spin_changes = 0;
    fdk_spin_set_on_changed(sp, on_spin, NULL);

    assert(fdk_spin_get_value(sp) == 50.0);
    assert(strcmp(fdk_spin_get_text(sp), "50") == 0);

    /* Steppers: click the up button (right column, top half). */
    click(root, 110, 7);
    assert(fdk_spin_get_value(sp) == 51.0);
    assert(g_spin_changes == 1);
    /* Down (bottom half). */
    click(root, 110, 21);
    assert(fdk_spin_get_value(sp) == 50.0);

    /* Keyboard: up/down/page. */
    fdk_event_data k = ev_key(FDK_KEY_UP);
    fdk_widget *entry_inner = fdk_widget_child_at(sp, 0);
    assert(fdk_widget_focus(entry_inner));
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_spin_get_value(sp) == 51.0);
    fdk_event_data pgdn = ev_key(FDK_KEY_PAGE_DOWN);
    assert(fdk_widget_tree_handle_event(root, &pgdn));
    assert(fdk_spin_get_value(sp) == 41.0);

    /* Typing then Enter commits (through the real entry). */
    assert(fdk_ok(fdk_entry_set_text(entry_inner, "77")));
    /* (programmatic set_text doesn't commit — Enter does) */
    assert(fdk_spin_get_value(sp) == 41.0);
    k = ev_key(FDK_KEY_ENTER);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_spin_get_value(sp) == 77.0);

    /* Unparsable text keeps the last value (and rewrites). */
    assert(fdk_ok(fdk_entry_set_text(entry_inner, "nonsense")));
    k = ev_key(FDK_KEY_ENTER);
    assert(fdk_widget_tree_handle_event(root, &k));
    assert(fdk_spin_get_value(sp) == 77.0);
    assert(strcmp(fdk_spin_get_text(sp), "77") == 0);

    /* Range + clamps. */
    fdk_spin_set_range(sp, 0, 10);
    assert(fdk_spin_get_value(sp) == 10.0);
    fdk_spin_set_value(sp, 99);
    assert(fdk_spin_get_value(sp) == 10.0);
    fdk_spin_set_step(sp, 0.5);
    click(root, 110, 7); /* +0.5 clamps to max */
    assert(fdk_spin_get_value(sp) == 10.0);

    /* Fractional formatting round trip. */
    fdk_spin_set_range(sp, 0, 10);
    fdk_spin_set_value(sp, 2.5);
    assert(strcmp(fdk_spin_get_text(sp), "2.5") == 0);

    fdk_widget_destroy(root);
    printf("[ok] spinbutton: steppers, keyboard stepping, Enter "
           "commits typed text, unparsable keeps last value, "
           "clamps, fractional round trip\n");
}

/* ---- Toolbar ---- */

static int g_tb_hits = 0;
static void tb_open(fdk_widget *b, void *user) {
    (void)b;
    g_tb_hits += (int)(size_t)user;
}

static void test_toolbar(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *tb = NULL;
    assert(fdk_toolbar_create(root, g_font, &tb) == FDK_OK);
    fdk_rect r = { 0, 0, 400, 38 };
    fdk_widget_arrange(tb, r); /* arrange hook: bounds + row layout */

    fdk_widget *b1 = NULL;
    assert(fdk_toolbar_add_button(tb, "Open", tb_open, (void *)1,
                                  &b1) == FDK_OK);
    assert(fdk_toolbar_add_separator(tb) == FDK_OK);
    assert(fdk_toolbar_add_button(tb, "Save", tb_open, (void *)10,
                                  NULL) == FDK_OK);

    /* The buttons live in the toolbar and activate on click. */
    assert(fdk_widget_child_count(tb) == 3);
    g_tb_hits = 0;
    /* Button 1's bounds: x starts at pad 4, natural width of "Open"
     * + button padding 32; click its center. */
    fdk_size nat = { 0, 0 };
    fdk_widget_measure(b1, &nat);
    click(root, 4.0f + (fdk_f32)nat.width / 2.0f, 19.0f);
    assert(g_tb_hits == 1);

    /* Paint the bar: track color + bottom rule visible. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(420, 60, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_u32 rule = px_at(s, 200, 37);
    assert(rule != 0); /* hairline rule inked */

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] toolbar: buttons activate, separator added, bar "
           "chrome paints (rule inked)\n");
}

/* ---- Notebook ---- */

static int g_switches = 0;
static size_t g_last_page = 999;
static void on_switch(fdk_widget *nb, size_t page, void *user) {
    (void)nb;
    (void)user;
    g_switches++;
    g_last_page = page;
}

static void test_notebook(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *nb = NULL;
    assert(fdk_notebook_create(root, g_font, &nb) == FDK_OK);
    fdk_rect r = { 0, 0, 300, 200 };
    fdk_widget_set_bounds(nb, r);

    /* Three pages with colored backgrounds. */
    fdk_widget *p1 = NULL, *p2 = NULL, *p3 = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &p1)));
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &p2)));
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &p3)));
    fdk_widget_set_background(p1, (fdk_color){1, 0, 0, 1});
    fdk_widget_set_background(p2, (fdk_color){0, 1, 0, 1});
    fdk_widget_set_background(p3, (fdk_color){0, 0, 1, 1});

    assert(fdk_notebook_append_page(nb, p1, "Red") == FDK_OK);
    assert(fdk_notebook_append_page(nb, p2, "Green") == FDK_OK);
    assert(fdk_notebook_append_page(nb, p3, "Blue") == FDK_OK);
    assert(fdk_notebook_page_count(nb) == 3);
    assert(fdk_notebook_get_page(nb, 1) == p2);
    assert(fdk_notebook_get_page(nb, 9) == NULL);

    g_switches = 0;
    fdk_notebook_set_on_switch(nb, on_switch, NULL);

    /* Page 0 current; only it is visible. */
    assert(fdk_notebook_get_current_page(nb) == 0);
    assert(fdk_widget_get_visible(p1));
    assert(!fdk_widget_get_visible(p2));
    assert(!fdk_widget_get_visible(p3));
    /* Page 0's bounds are the page area (below the 30px strip). */
    fdk_rect pa = fdk_widget_get_bounds(p1);
    assert(pa.x == 0 && pa.y == 30 && pa.width == 300 &&
           pa.height == 170);

    /* Paint: red page visible under the strip. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(320, 220, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int rr = (int)((px_at(s, 150, 100) >> 16) & 0xFF);
    assert(rr > 200); /* page 0's red */

    /* Click tab 2 (widths: "Red"~3 chars... measure-driven; find
     * via the page-count API: click progressively right until the
     * current page flips — honest tab-rect math: tab width =
     * text + 2*14 pad, gap 2. */
    fdk_i32 tw_red = 0, th = 0;
    fdk__text_extent(g_font, "Red", &tw_red, &th);
    fdk_i32 tw_green = 0;
    fdk__text_extent(g_font, "Green", &tw_green, &th);
    fdk_i32 x_green = 0 + (tw_red + 28) + 2;
    click(root, (float)x_green + (float)tw_green / 2.0f + 14.0f, 15.0f);
    assert(fdk_notebook_get_current_page(nb) == 1);
    assert(g_switches == 1 && g_last_page == 1);
    assert(!fdk_widget_get_visible(p1));
    assert(fdk_widget_get_visible(p2));

    /* The switch paints green. */
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    int gg = (int)((px_at(s, 150, 100) >> 8) & 0xFF);
    assert(gg > 200);

    /* Programmatic switch + callback. */
    assert(fdk_notebook_set_current_page(nb, 2) == FDK_OK);
    assert(g_switches == 2 && g_last_page == 2);
    assert(fdk_notebook_set_current_page(nb, 9) ==
           FDK_ERR_INVALID_ARGUMENT);
    /* Same-page set: no callback. */
    assert(fdk_notebook_set_current_page(nb, 2) == FDK_OK);
    assert(g_switches == 2);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] notebook: pages adopt + visibility switches, tab "
           "clicks (measured rects), programmatic switch, callback "
           "counts, pixel-verified pages\n");
}

/* ---- Canvas ---- */

static int g_canvas_paint_calls = 0;
static fdk_rect g_last_clip = { -1, -1, -1, -1 };

static void on_canvas_paint(fdk_widget *canvas, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip,
                            void *user) {
    (void)canvas;
    (void)user;
    g_canvas_paint_calls++;
    g_last_clip = clip;
    /* Draw a marker rect the test can find. */
    fdk_surface_fill_rect(surface, bounds, (fdk_color){1, 1, 1, 1});
    fdk_surface_fill_rect(surface,
                          (fdk_rect){bounds.x + 2, bounds.y + 2,
                                     bounds.width - 4,
                                     bounds.height - 4},
                          (fdk_color){0, 0, 0, 1});
}

static void test_canvas(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cv = NULL;
    assert(fdk_canvas_create(root, on_canvas_paint, NULL, &cv) ==
           FDK_OK);
    fdk_rect r = { 10, 10, 100, 80 };
    fdk_widget_set_bounds(cv, r);

    g_canvas_paint_calls = 0;
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(140, 120, &s)));
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);

    /* The callback ran, with the widget's absolute bounds and the
     * effective clip inside them. */
    assert(g_canvas_paint_calls == 1);
    assert(g_last_clip.x >= 10 && g_last_clip.y >= 10);
    assert(g_last_clip.x + g_last_clip.width <= 110);
    assert(g_last_clip.y + g_last_clip.height <= 90);

    /* The drawing actually landed: white frame at the canvas edge,
     * black interior. */
    assert(px_at(s, 10, 50) == 0xFFFFFF);
    assert(px_at(s, 60, 50) == 0x000000);

    /* Idempotent repaint discipline: paint again (damage cleared)
     * -> same result, and the callback only runs when damaged. */
    g_canvas_paint_calls = 0;
    fdk_widget_tree_paint(root, s); /* no new damage: no call */
    assert(g_canvas_paint_calls == 0);
    fdk_canvas_invalidate(cv);
    fdk_widget_tree_paint(root, s);
    assert(g_canvas_paint_calls == 1);

    /* Callback replace + arg safety. */
    fdk_canvas_set_paint_callback(cv, NULL, NULL);
    fdk_canvas_invalidate(cv);
    fdk_widget_tree_paint(root, s);
    assert(g_canvas_paint_calls == 1); /* NULL callback: no call */
    fdk_canvas_invalidate(root);
    fdk_canvas_set_paint_callback(root, on_canvas_paint, NULL);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] canvas: paint callback with bounds+clip, drawing "
           "lands pixel-exact, damage-driven call counts, NULL "
           "callback safe\n");
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
        printf("[skip] no system TrueType font found — the Phase 9 "
               "controls need real glyph metrics; see "
               "docs/testing.md\n");
        return 0;
    }

    test_slider();
    test_spin();
    test_toolbar();
    test_notebook();
    test_canvas();

    fdk_font_destroy(g_font);
    printf("all phase-9 control tests passed\n");
    return 0;
}
