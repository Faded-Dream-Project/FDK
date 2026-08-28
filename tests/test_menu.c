/* test_menu.c — headless tests for the Phase 9 menu machinery
 * (model, view widget, MenuBar geometry/hit-testing).
 *
 * Same discipline as the other widget suites: standalone roots,
 * offscreen surfaces, synthetic window events fed through
 * fdk_widget_tree_handle_event, ASan+UBSan. Popup-window behavior
 * (real popups, grabs, dismissal) is the X11/Wayland GUI suites'
 * territory; here the MODEL and the VIEW are exercised directly —
 * the view is a plain widget, so headless event injection drives the
 * same handlers the popups use (session == NULL: activation fires
 * without any close logic, exactly what the popup path adds).
 *
 * Needs a system font for measured geometry — honest skip otherwise.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/menu_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

/* ---- helpers (event shapes mirror test_widget.c) ---- */

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

static bool send_button(fdk_widget *root, fdk_event_type t, float x,
                        float y) {
    fdk_event_data e = ev_button(t, x, y);
    return fdk_widget_tree_handle_event(root, &e);
}

static bool send_key(fdk_widget *root, fdk_scancode sc) {
    fdk_event_data e = ev_key(FDK_EVENT_KEY_DOWN, sc);
    return fdk_widget_tree_handle_event(root, &e);
}

static bool send_motion(fdk_widget *root, float x, float y) {
    fdk_event_data e = ev_motion(x, y);
    return fdk_widget_tree_handle_event(root, &e);
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

/* A standalone view of `model` inside `root`, arranged to its
 * measured size — exactly what the session does inside popups. */
static fdk_widget *view_for(fdk_widget *root, fdk_menu *model,
                            fdk_i32 *out_w, fdk_i32 *out_h) {
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(model, 0, &w, &h);
    fdk_widget *v = NULL;
    assert(fdk_ok(fdk_widget_create(root, &fdk_menu_view_class_def,
                                    (fdk_rect){0, 0, w, h}, &v)));
    /* The view's model/session/level fields are zero-initialized by
     * fdk_widget_create; model must be set the way menu.c's session
     * does — via the internal accessor (test-only convenience). */
    assert(fdk_ok(fdk__menu_view_bind(v, model)));
    if (out_w != NULL) {
        *out_w = w;
    }
    if (out_h != NULL) {
        *out_h = h;
    }
    return v;
}

static int activations = 0;
static fdk_menu_item *last_item = NULL;

static void count_activate(fdk_menu_item *item, void *user) {
    (void)user;
    activations++;
    last_item = item;
}

/* ---- the model ---- */

static void test_model_basics(void) {
    fdk_menu *m = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &m)));
    assert(fdk_menu_item_count(m) == 0);

    /* NULL safety. */
    assert(fdk_menu_item_count(NULL) == 0);
    assert(!fdk_ok(fdk_menu_append(NULL, "x", NULL)));
    assert(!fdk_ok(fdk_menu_create(g_font, NULL)));

    fdk_menu_item *a = NULL, *b = NULL, *c = NULL, *r1 = NULL,
                  *r2 = NULL, *chk = NULL;
    assert(fdk_ok(fdk_menu_append(m, "Open", &a)));
    assert(fdk_ok(fdk_menu_append_separator(m)));
    assert(fdk_ok(fdk_menu_append(m, "Save", &b)));
    assert(fdk_ok(fdk_menu_append_check(m, "Toolbar", true, &chk)));
    assert(fdk_ok(fdk_menu_append_radio(m, "Small", true, &r1)));
    assert(fdk_ok(fdk_menu_append_radio(m, "Large", false, &r2)));
    assert(fdk_ok(fdk_menu_append(m, "Quit", &c)));
    assert(fdk_menu_item_count(m) == 7);

    /* Handle stability across growth (the array reallocates; the
     * items themselves never move). */
    for (int i = 0; i < 40; i++) {
        assert(fdk_ok(fdk_menu_append(m, "filler", NULL)));
    }
    assert(strcmp(fdk_menu_item_text(a), "Open") == 0);
    assert(strcmp(fdk_menu_item_text(c), "Quit") == 0);

    /* Types. */
    assert(fdk_menu_item_get_type(a) == FDK_MENU_ITEM_NORMAL);
    assert(fdk_menu_item_get_type(chk) == FDK_MENU_ITEM_CHECK);
    assert(fdk_menu_item_get_type(r1) == FDK_MENU_ITEM_RADIO);
    assert(fdk_menu_item_get_type(NULL) == FDK_MENU_ITEM_NORMAL);

    /* Text mutation. */
    assert(fdk_ok(fdk_menu_item_set_text(b, "Save As")));
    assert(strcmp(fdk_menu_item_text(b), "Save As") == 0);
    assert(fdk_menu_item_text(NULL) == NULL);

    /* Enabled. */
    assert(fdk_menu_item_is_enabled(a));
    fdk_menu_item_set_enabled(a, false);
    assert(!fdk_menu_item_is_enabled(a));
    assert(!fdk_menu_item_is_enabled(NULL));
    fdk_menu_item_set_enabled(a, true);

    /* Checked state. */
    assert(fdk_menu_item_is_checked(chk));
    fdk_menu_item_set_checked(chk, false);
    assert(!fdk_menu_item_is_checked(chk));

    /* Submenu attachment: only NORMAL items take them. */
    fdk_menu *sub = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &sub)));
    assert(fdk_ok(fdk_menu_item_set_submenu(a, sub)));
    assert(!fdk_ok(fdk_menu_item_set_submenu(chk, sub)));
    assert(!fdk_ok(fdk_menu_item_set_submenu(NULL, sub)));
    fdk_menu_destroy(sub);

    /* Shortcuts are display-only metadata — set/clear must not
     * affect anything else. */
    assert(fdk_ok(fdk_menu_item_set_shortcut(c, "Ctrl+Q")));
    assert(fdk_ok(fdk_menu_item_set_shortcut(c, NULL)));
    assert(!fdk_ok(fdk_menu_item_set_shortcut(NULL, "x")));

    fdk_menu_destroy(m);
    fdk_menu_destroy(NULL); /* safe no-op */
    printf("[ok] menu model: CRUD, stable handles, types, submenu "
           "rules, arg safety\n");
}

static void test_model_measure(void) {
    fdk_menu *m = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &m)));
    fdk_i32 w = 0, h = 0;
    fdk__menu_measure(m, 0, &w, &h);
    fdk_i32 rh = fdk__menu_row_height(m);

    /* Empty menu: minimum size. */
    assert(w >= 40 && h >= 1);

    /* Separators are thinner than rows. */
    assert(fdk_ok(fdk_menu_append_separator(m)));
    fdk__menu_measure(m, 0, &w, &h);
    assert(h >= 1 && h < rh); /* a separator alone is MENU_SEP_H */

    /* Rows stack: n rows + separators. */
    fdk_i32 sep_h = h;
    assert(fdk_ok(fdk_menu_append(m, "Item", NULL)));
    fdk__menu_measure(m, 0, &w, &h);
    assert(h == sep_h + rh);

    /* Longer text widens (needs the font). */
    fdk_i32 w0 = w;
    assert(fdk_ok(fdk_menu_append(m, "A much longer menu entry",
                                  NULL)));
    fdk__menu_measure(m, 0, &w, &h);
    assert(w > w0);
    assert(h == sep_h + rh * 2);

    /* min_width widens but never narrows. */
    fdk_i32 w1 = w;
    fdk__menu_measure(m, w1 + 50, &w, &h);
    assert(w == w1 + 50);
    fdk__menu_measure(m, 10, &w, &h);
    assert(w == w1);

    /* Height clamp: no scrolling in v1 (MENU_MAX_H). */
    fdk_menu *big = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &big)));
    for (int i = 0; i < 100; i++) {
        assert(fdk_ok(fdk_menu_append(big, "row", NULL)));
    }
    fdk__menu_measure(big, 0, &w, &h);
    assert(h <= 512);
    fdk_menu_destroy(big);

    fdk_menu_destroy(m);
    printf("[ok] menu measure: row stacking, separators, width from "
           "text, min_width, height clamp\n");
}

/* ---- the view (headless: session == NULL) ---- */

static void test_view_interaction(void) {
    fdk_menu *m = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &m)));
    fdk_menu_item *open = NULL, *sep_after = NULL, *chk = NULL,
                  *r1 = NULL, *r2 = NULL, *disabled = NULL;
    assert(fdk_ok(fdk_menu_append(m, "Open", &open)));
    assert(fdk_ok(fdk_menu_append_separator(m)));
    (void)sep_after;
    assert(fdk_ok(fdk_menu_append_check(m, "Toolbar", false, &chk)));
    assert(fdk_ok(fdk_menu_append_radio(m, "Small", true, &r1)));
    assert(fdk_ok(fdk_menu_append_radio(m, "Large", false, &r2)));
    assert(fdk_ok(fdk_menu_append(m, "Disabled", &disabled)));
    fdk_menu_item_set_enabled(disabled, false);
    fdk_menu_set_on_activate(m, count_activate, NULL);

    fdk_widget *root = fresh_root();
    fdk_i32 w = 0, h = 0;
    fdk_widget *v = view_for(root, m, &w, &h);
    fdk_i32 rh = fdk__menu_row_height(m);
    fdk_i32 sep_h = 9;

    /* Row geometry: fdk__menu_row_at. */
    assert(fdk__menu_row_at(v, 0.0f) == 0);          /* "Open"     */
    assert(fdk__menu_row_at(v, (float)(rh - 1)) == 0);
    assert(fdk__menu_row_at(v, (float)rh) == 1);      /* separator  */
    assert(fdk__menu_row_at(v, (float)(rh + sep_h)) == 2); /* Toolbar */
    assert(fdk__menu_row_at(v, (float)(rh * 5)) == 5); /* Disabled  */
    assert(fdk__menu_row_at(v, (float)(rh * 10)) == -1); /* below    */
    assert(fdk__menu_row_at(v, -3.0f) == -1);

    /* Click "Open": the fallback on_activate fires. */
    activations = 0;
    last_item = NULL;
    assert(send_button(root, FDK_EVENT_POINTER_BUTTON_DOWN, 5, (float)(rh / 2)));
    assert(send_button(root, FDK_EVENT_POINTER_BUTTON_UP, 5, (float)(rh / 2)));
    assert(activations == 1 && last_item == open);

    /* Click the separator: nothing fires. */
    activations = 0;
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_DOWN, 5, (float)(rh + 2));
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_UP, 5, (float)(rh + 2));
    assert(activations == 0);

    /* Click the check item: state flips THEN the callback runs. */
    activations = 0;
    last_item = NULL;
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_DOWN, 5, (float)(rh + sep_h + rh / 2));
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_UP, 5, (float)(rh + sep_h + rh / 2));
    assert(activations == 1 && last_item == chk);
    assert(fdk_menu_item_is_checked(chk)); /* false -> true */

    /* Radio group: activating Large unchecks Small. */
    activations = 0;
    last_item = NULL;
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_DOWN, 5, (float)(rh + sep_h + rh * 2 + rh / 2));
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_UP, 5, (float)(rh + sep_h + rh * 2 + rh / 2));
    assert(activations == 1 && last_item == r2);
    assert(fdk_menu_item_is_checked(r2));
    assert(!fdk_menu_item_is_checked(r1));

    /* Disabled item: click swallowed, nothing fires. */
    activations = 0;
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_DOWN, 5, (float)(rh * 4 + sep_h + rh / 2));
    (void)send_button(root, FDK_EVENT_POINTER_BUTTON_UP, 5, (float)(rh * 4 + sep_h + rh / 2));
    assert(activations == 0);

    /* Keyboard: Down moves the cursor (separators and disabled rows
     * skipped), Enter activates. Keys route to the focused widget —
     * the session focuses the view at popup open; the test does the
     * same. */
    fdk_widget_set_can_focus(v, true);
    assert(fdk_widget_focus(v));
    activations = 0;
    last_item = NULL;
    assert(send_key(root, FDK_KEY_DOWN));
    assert(send_key(root, FDK_KEY_DOWN));
    assert(send_key(root, FDK_KEY_ENTER));
    /* Down x2 from cold start lands on "Toolbar" (the disabled row is
     * last, arrows stop where they should). */
    assert(activations == 1 && last_item == chk);

    /* Home + Enter activates the first row. */
    activations = 0;
    last_item = NULL;
    (void)send_key(root, FDK_KEY_HOME);
    (void)send_key(root, FDK_KEY_ENTER);
    assert(activations == 1 && last_item == open);

    /* End + Enter: the last ENABLED row is the radio "Large". */
    activations = 0;
    last_item = NULL;
    (void)send_key(root, FDK_KEY_END);
    (void)send_key(root, FDK_KEY_ENTER);
    assert(activations == 1 && last_item == r2);

    fdk_widget_destroy(root);
    fdk_menu_destroy(m);
    printf("[ok] menu view: click/keyboard activation, check flip, "
           "radio group, separators, disabled rows\n");
}

static void test_view_paint(void) {
    fdk_menu *m = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &m)));
    assert(fdk_ok(fdk_menu_append_check(m, "Toolbar", true, NULL)));

    fdk_widget *root = fresh_root();
    fdk_i32 w = 0, h = 0;
    fdk_widget *v = view_for(root, m, &w, &h);
    fdk_i32 rh = fdk__menu_row_height(m);
    (void)v;

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(w, h, &s)));

    /* Paint: menu surface = control color. */
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    fdk_u32 mid = px_at(s, w - 5, 2);
    int mr = (int)((mid >> 16) & 0xFFu);
    int mg = (int)((mid >> 8) & 0xFFu);
    int mb = (int)(mid & 0xFFu);
    fdk_color ctl = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    assert(mr == (int)(ctl.r * 255.0f + 0.5f) &&
           mg == (int)(ctl.g * 255.0f + 0.5f) &&
           mb == (int)(ctl.b * 255.0f + 0.5f));

    /* Hover highlight: motion over the row changes its pixels. */
    fdk_u32 before = px_at(s, w / 2, rh / 2);
    (void)send_motion(root, (float)(w / 2), (float)(rh / 2));
    assert(fdk_widget_tree_has_damage(root));
    fdk_widget_tree_paint(root, s);
    fdk_u32 after = px_at(s, w / 2, rh / 2);
    assert(before != after); /* hover fill differs from plain */

    /* Idempotency: repainting an undamaged tree changes nothing. */
    fdk_u32 again = px_at(s, w / 2, rh / 2);
    fdk_widget_tree_paint(root, s);
    assert(again == px_at(s, w / 2, rh / 2));

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    fdk_menu_destroy(m);
    printf("[ok] menu view paint: themed surface, hover highlight, "
           "idempotent repaint\n");
}

/* ---- the MenuBar (headless: geometry + hit-testing; the popup
 * chain itself is GUI-suite territory) ---- */

static void test_menu_bar_headless(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *bar = NULL;
    assert(fdk_ok(fdk_menu_bar_create(root, g_font, &bar)));
    assert(fdk_menu_bar_count(bar) == 0);
    assert(fdk_menu_bar_count(root) == 0); /* not a bar */

    fdk_menu *fm = NULL, *em = NULL;
    assert(fdk_ok(fdk_menu_create(g_font, &fm)));
    assert(fdk_ok(fdk_menu_create(g_font, &em)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "File", fm)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "Edit", em)));
    assert(fdk_menu_bar_count(bar) == 2);

    /* Arrange (the layout hook packs titles). */
    fdk_size nat = {0, 0};
    fdk_widget_measure(bar, &nat);
    assert(nat.height >= 16); /* the themed menu row height */
    fdk_rect bar_rect = {0, 0, 300, nat.height};
    fdk_widget_arrange(bar, bar_rect);

    /* Hit-testing maps window coordinates to titles. */
    int t0 = fdk__menu_bar_hit(bar, 10, 5);
    int t1 = fdk__menu_bar_hit(bar, 90, 5);
    assert(t0 == 0 && t1 == 1);
    assert(fdk__menu_bar_hit(bar, 10, 200) == -1); /* below the bar */
    assert(fdk__menu_bar_hit(bar, 10, -5) == -1);  /* above */

    /* Remove: the open-title guard path is GUI territory; here the
     * count and hit mapping just shrink. */
    assert(fdk_ok(fdk_menu_bar_remove(bar, 0)));
    assert(fdk_menu_bar_count(bar) == 1);
    assert(fdk__menu_bar_hit(bar, 30, 5) == 0); /* Edit reflows to x=6 */
    assert(!fdk_ok(fdk_menu_bar_remove(bar, 5)));
    assert(!fdk_ok(fdk_menu_bar_remove(root, 0)));

    /* Close with no chain: a safe no-op. */
    fdk_menu_bar_close(bar);
    fdk_menu_bar_close(root); /* wrong class: no-op */

    /* Paint: the bar's track fill + a bottom rule. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, nat.height, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    fdk_u32 rule = px_at(s, 150, nat.height - 1);
    fdk_u32 body = px_at(s, 150, 2);
    assert(rule != body); /* the hairline differs from the fill    */

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    fdk_menu_destroy(fm);
    fdk_menu_destroy(em);
    printf("[ok] menu bar: layout, window-coord hit tests, remove, "
           "close no-op, painted chrome\n");
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
        printf("[skip] no system TrueType font found — menu geometry "
               "needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_model_basics();
    test_model_measure();
    test_view_interaction();
    test_view_paint();
    test_menu_bar_headless();

    fdk_font_destroy(g_font);
    printf("all menu tests passed\n");
    return 0;
}
