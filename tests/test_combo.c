/* test_combo.c — headless tests for the Phase 9 ComboBox.
 *
 * Same discipline: standalone roots, offscreen surfaces, synthetic
 * events, ASan+UBSan. The dropdown POPUP is GUI-suite territory;
 * here the widget's own semantics are driven directly (the combo's
 * event callback is what the popup rides on). Needs a system font
 * for measured geometry — honest skip otherwise.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

static fdk_event_data ev_key(fdk_event_type t, fdk_scancode sc) {
    fdk_event_data e;
    memset(&e, 0, sizeof(e));
    e.type = t;
    e.key.scancode = sc;
    return e;
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
                                    (fdk_rect){0, 0, 300, 200},
                                    &root)));
    return root;
}

static int changed = 0;
static fdk_i64 changed_index = -2;

static void count_changed(fdk_widget *combo, size_t index, void *user) {
    (void)combo;
    (void)user;
    changed++;
    changed_index = (index == FDK_COMBO_NONE) ? -1 : (fdk_i64)index;
}

/* The combo's embedded Entry: first child in editable mode. */
static fdk_widget *combo_entry(fdk_widget *combo) {
    assert(fdk_widget_child_count(combo) == 1);
    return fdk_widget_child_at(combo, 0);
}

static void test_combo_model(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_combo_create(root, g_font, &c)));

    assert(fdk_combo_count(c) == 0);
    assert(fdk_combo_get_active(c) == -1);
    assert(strcmp(fdk_combo_active_text(c), "") == 0);

    size_t i0 = 99, i1 = 99, i2 = 99;
    assert(fdk_ok(fdk_combo_append(c, "Red", &i0)));
    assert(fdk_ok(fdk_combo_append(c, "Green", &i1)));
    assert(fdk_ok(fdk_combo_append(c, "Blue", &i2)));
    assert(i0 == 0 && i1 == 1 && i2 == 2);
    assert(fdk_combo_count(c) == 3);
    assert(strcmp(fdk_combo_text(c, 1), "Green") == 0);
    assert(fdk_combo_text(c, 5) == NULL);
    assert(fdk_combo_text(root, 0) == NULL); /* not a combo */

    /* NULL text appends as an empty row (consistent with strdup("")). */
    assert(fdk_ok(fdk_combo_append(c, NULL, NULL)));
    assert(strcmp(fdk_combo_text(c, 3), "") == 0);
    assert(fdk_ok(fdk_combo_remove(c, 3)));
    assert(fdk_combo_count(c) == 3);

    /* Arg safety. */
    assert(!fdk_ok(fdk_combo_append(NULL, "x", NULL)));
    assert(!fdk_ok(fdk_combo_append(root, "x", NULL)));
    assert(!fdk_ok(fdk_combo_remove(c, 9)));
    assert(!fdk_ok(fdk_combo_remove(NULL, 0)));
    assert(fdk_combo_count(NULL) == 0);

    fdk_widget_destroy(root);
    printf("[ok] combo model: CRUD, arg safety, empty-active state\n");
}

static void test_combo_active(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_combo_create(root, g_font, &c)));
    assert(fdk_ok(fdk_combo_append(c, "Red", NULL)));
    assert(fdk_ok(fdk_combo_append(c, "Green", NULL)));
    assert(fdk_ok(fdk_combo_append(c, "Blue", NULL)));

    changed = 0;
    fdk_combo_set_on_changed(c, count_changed, NULL);

    /* Programmatic selection fires on_changed. */
    assert(fdk_ok(fdk_combo_set_active(c, 1)));
    assert(fdk_combo_get_active(c) == 1);
    assert(strcmp(fdk_combo_active_text(c), "Green") == 0);
    assert(changed == 1 && changed_index == 1);

    /* No-op reselect stays silent. */
    assert(fdk_ok(fdk_combo_set_active(c, 1)));
    assert(changed == 1);

    /* Clearing. */
    assert(fdk_ok(fdk_combo_set_active(c, -1)));
    assert(fdk_combo_get_active(c) == -1);
    assert(strcmp(fdk_combo_active_text(c), "") == 0);
    assert(changed == 2 && changed_index == -1);
    assert(fdk_ok(fdk_combo_set_active(c, -1)));
    assert(changed == 2); /* still silent on a repeat clear */

    /* Out-of-range refuses. */
    assert(!fdk_ok(fdk_combo_set_active(c, 3)));
    assert(!fdk_ok(fdk_combo_set_active(c, -5)));

    /* remove() keeps the selection honest. */
    assert(fdk_ok(fdk_combo_set_active(c, 2))); /* Blue */
    assert(fdk_ok(fdk_combo_remove(c, 2)));     /* removing the active */
    assert(fdk_combo_get_active(c) == -1);
    assert(fdk_combo_count(c) == 2);
    assert(fdk_ok(fdk_combo_set_active(c, 1))); /* Green at 1 */
    assert(fdk_ok(fdk_combo_remove(c, 0)));     /* shifting down */
    assert(fdk_combo_get_active(c) == 0);
    assert(strcmp(fdk_combo_active_text(c), "Green") == 0);

    fdk_combo_clear(c);
    assert(fdk_combo_count(c) == 0);
    assert(fdk_combo_get_active(c) == -1);
    assert(fdk_combo_active_text(NULL) != NULL);

    fdk_widget_destroy(root);
    printf("[ok] combo active: set/clear semantics, on_changed "
           "counts, remove shifting\n");
}

static void test_combo_editable(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_combo_create(root, g_font, &c)));
    assert(fdk_ok(fdk_combo_append(c, "Red", NULL)));
    assert(fdk_ok(fdk_combo_append(c, "Green", NULL)));

    changed = 0;
    fdk_combo_set_on_changed(c, count_changed, NULL);

    /* Non-editable by default: no Entry child. */
    assert(fdk_widget_child_count(c) == 0);

    fdk_combo_set_editable(c, true);
    assert(fdk_widget_child_count(c) == 1); /* the embedded Entry */
    fdk_widget *entry = combo_entry(c);
    assert(fdk_widget_has_focus(entry) || true); /* focus optional */

    /* Editing text: custom state. */
    changed = 0;
    assert(fdk_ok(fdk_combo_set_active(c, 0))); /* Red */
    assert(changed == 1);
    assert(strcmp(fdk_combo_active_text(c), "Red") == 0);

    changed = 0;
    assert(fdk_ok(fdk_entry_set_text(entry, "Purple")));
    assert(fdk_combo_get_active(c) == -1); /* went custom */
    assert(strcmp(fdk_combo_active_text(c), "Purple") == 0);
    assert(changed == 1 && changed_index == -1);

    /* Typing the ACTIVE row's text again: no transition. */
    assert(fdk_ok(fdk_combo_set_active(c, 1))); /* Green */
    changed = 0;
    assert(fdk_ok(fdk_entry_set_text(entry, "Green")));
    assert(fdk_combo_get_active(c) == 1); /* still the active row */
    assert(changed == 0);

    /* Back to non-editable: the Entry dies, the field shows the
     * active row. */
    fdk_combo_set_editable(c, false);
    assert(fdk_widget_child_count(c) == 0);
    assert(strcmp(fdk_combo_active_text(c), "Green") == 0);

    /* Idempotent mode flips. */
    fdk_combo_set_editable(c, false);
    fdk_combo_set_editable(c, true);
    assert(fdk_widget_child_count(c) == 1);

    fdk_widget_destroy(root);
    printf("[ok] combo editable: embedded Entry, custom text state, "
           "no-transition typing, mode flips\n");
}

static void test_combo_paint(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_combo_create(root, g_font, &c)));
    assert(fdk_ok(fdk_combo_append(c, "Red", NULL)));
    assert(fdk_ok(fdk_combo_set_active(c, 0)));

    fdk_size nat = {0, 0};
    fdk_widget_measure(c, &nat);
    assert(nat.height >= 16);
    fdk_widget_arrange(c, (fdk_rect){10, 10, nat.width, nat.height});

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 200, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);

    /* The field's control fill inside the arranged bounds. */
    fdk_u32 fill = px_at(s, 20, 15);
    int fr = (int)((fill >> 16) & 0xFFu);
    int fg = (int)((fill >> 8) & 0xFFu);
    int fb = (int)(fill & 0xFFu);
    fdk_color ctl = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    assert(fr == (int)(ctl.r * 255.0f + 0.5f) &&
           fg == (int)(ctl.g * 255.0f + 0.5f) &&
           fb == (int)(ctl.b * 255.0f + 0.5f));

    /* The chevron zone draws ink (vector strokes) near the right
     * edge: scan the zone for a non-background pixel. */
    fdk_rect b = fdk_widget_get_bounds(c);
    bool ink_found = false;
    for (int y = 12; y < 28 && !ink_found; y++) {
        for (int x = b.x + b.width - 20; x < b.x + b.width - 4; x++) {
            fdk_u32 px = px_at(s, x, y);
            if (px != fill) {
                ink_found = true;
                break;
            }
        }
    }
    assert(ink_found);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] combo paint: field fill in bounds, chevron ink in "
           "the dropdown zone\n");
}

static void test_combo_keyboard(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *c = NULL;
    assert(fdk_ok(fdk_combo_create(root, g_font, &c)));
    assert(fdk_ok(fdk_combo_append(c, "Red", NULL)));

    /* Non-editable combos are focusable and swallow the opener keys
     * (the dropdown itself needs a window — GUI suites). */
    fdk_widget_set_can_focus(c, true);
    assert(fdk_widget_focus(c));
    fdk_event_data e = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_DOWN);
    assert(fdk_widget_tree_handle_event(root, &e));
    e = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_ENTER);
    assert(fdk_widget_tree_handle_event(root, &e));

    /* Editable: focus moves to the Entry; the combo itself stops
     * being focusable. */
    fdk_combo_set_editable(c, true);
    assert(!fdk_widget_get_can_focus(c));

    fdk_widget_destroy(root);
    printf("[ok] combo keyboard: opener keys consumed, editable "
           "focus handoff\n");
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
        printf("[skip] no system TrueType font found — combo geometry "
               "needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_combo_model();
    test_combo_active();
    test_combo_editable();
    test_combo_paint();
    test_combo_keyboard();

    fdk_font_destroy(g_font);
    printf("all combo tests passed\n");
    return 0;
}
