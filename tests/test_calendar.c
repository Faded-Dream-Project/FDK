/* test_calendar.c — headless tests for the 1.1.0 calendar family
 * (S-10): the month grid, the date picker field, and the time
 * picker stepper.
 *
 * The discipline of test_combo.c: standalone roots, offscreen
 * surfaces, synthetic events, ASan+UBSan. The date picker's POPUP
 * is GUI-suite territory (a real window); here the field, the
 * format plumbing, and the calendar's own semantics are driven
 * directly. The grid math is pinned through the a11y virtual
 * children (the 42 day cells describe as ISO dates) and through
 * real click events. Needs a system font — honest skip otherwise.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

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
    e.pointer_button.modifiers = 0;
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

static void press(fdk_widget *root, fdk_scancode sc) {
    fdk_event_data e = ev_key(sc);
    (void)fdk_widget_tree_handle_event(root, &e);
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
                                    (fdk_rect){0, 0, 400, 320},
                                    &root)));
    return root;
}

static bool date_eq(const fdk_date *a, const fdk_date *b) {
    return a->year == b->year && a->month == b->month && a->day == b->day;
}

static fdk_widget *fresh_calendar(fdk_widget *root) {
    fdk_widget *cal = NULL;
    assert(fdk_ok(fdk_calendar_create(root, g_font, &cal)));
    fdk_size nat = {0, 0};
    fdk_widget_measure(cal, &nat);
    assert(nat.width > 100 && nat.height > 100);
    fdk_widget_arrange(cal, (fdk_rect){0, 0, nat.width, nat.height});
    return cal;
}

/* The ISO date name of a virtual day cell (the grid-math pin). */
static void cell_name(fdk_widget *cal, size_t index, char *buf,
                      size_t cap) {
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, index, &info)));
    assert(info.name != NULL);
    snprintf(buf, cap, "%s", info.name);
    fdk_a11y_info_free(&info);
}

/* ---- the calendar model ---- */

static int sel_count = 0;
static fdk_date sel_got;

static void count_selected(fdk_widget *cal, const fdk_date *d,
                           void *user) {
    (void)cal;
    (void)user;
    sel_count++;
    sel_got = *d;
}

static void test_calendar_model(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cal = fresh_calendar(root);

    /* Defaults: 1970-01 browsed, no selection, no today. */
    fdk_i32 y = 0, m = 0;
    fdk_calendar_get_month(cal, &y, &m);
    assert(y == 1970 && m == 1);
    fdk_date d;
    assert(!fdk_calendar_get_date(cal, &d));

    /* Validation: impossible dates rejected. */
    fdk_date bad = {2025, 2, 30};
    assert(!fdk_ok(fdk_calendar_select(cal, &bad)));
    bad = (fdk_date){2025, 13, 1};
    assert(!fdk_ok(fdk_calendar_select(cal, &bad)));
    bad = (fdk_date){0, 1, 1};
    assert(!fdk_ok(fdk_calendar_select(cal, &bad)));
    bad = (fdk_date){2025, 0, 1};
    assert(!fdk_ok(fdk_calendar_select(cal, &bad)));
    bad = (fdk_date){2024, 2, 29}; /* 2024 IS leap: valid */
    assert(fdk_ok(fdk_calendar_select(cal, &bad)));
    bad = (fdk_date){2025, 2, 29}; /* 2025 is not */
    assert(!fdk_ok(fdk_calendar_select(cal, &bad)));

    /* Select follows the month; same-date re-select is silent. */
    sel_count = 0;
    fdk_calendar_set_on_selected(cal, count_selected, NULL);
    fdk_date pick = {2025, 12, 25};
    assert(fdk_ok(fdk_calendar_select(cal, &pick)));
    assert(sel_count == 1);
    assert(date_eq(&sel_got, &pick));
    fdk_calendar_get_month(cal, &y, &m);
    assert(y == 2025 && m == 12);
    assert(fdk_ok(fdk_calendar_select(cal, &pick)));
    assert(sel_count == 1); /* no re-fire */

    /* get_date round trip. */
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 2025 && d.month == 12 && d.day == 25);

    /* set_month browses without touching the selection. */
    assert(fdk_ok(fdk_calendar_set_month(cal, 2024, 2)));
    fdk_calendar_get_month(cal, &y, &m);
    assert(y == 2024 && m == 2);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 2025 && d.month == 12 && d.day == 25);
    assert(!fdk_ok(fdk_calendar_set_month(cal, 2024, 0)));
    assert(!fdk_ok(fdk_calendar_set_month(cal, 0, 1)));

    /* Today marker: set/clear, invalid ignored. */
    fdk_date today = {2024, 2, 29};
    fdk_calendar_set_today(cal, &today);
    fdk_calendar_set_today(cal, NULL);
    bad = (fdk_date){2024, 2, 30};
    fdk_calendar_set_today(cal, &bad); /* ignored: no crash */

    fdk_widget_destroy(root);
    printf("[ok] calendar model: validation, follow-month selection, "
           "silent re-select, browse-without-select, today marker\n");
}

/* ---- the grid math (the a11y virtual children) ---- */

static void test_calendar_grid(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cal = fresh_calendar(root);

    /* 42 virtual day cells, always. */
    assert(fdk_a11y_virtual_count(cal) == 42);

    /* January 1970: the 1st was a Thursday. Monday-lead grid:
     * cells 0..2 = Dec 29..31 1969, cell 3 = Jan 1, cell 33 = Jan
     * 31, cells 34..41 = Feb 1..8. */
    char name[32];
    cell_name(cal, 0, name, sizeof(name));
    assert(strcmp(name, "1969-12-29") == 0);
    cell_name(cal, 3, name, sizeof(name));
    assert(strcmp(name, "1970-01-01") == 0);
    cell_name(cal, 33, name, sizeof(name));
    assert(strcmp(name, "1970-01-31") == 0);
    cell_name(cal, 41, name, sizeof(name));
    assert(strcmp(name, "1970-02-08") == 0);

    /* February 1970 (28 days, Sunday lead = 6): cells 0..5 =
     * Jan 26..31, cell 6 = Feb 1. */
    assert(fdk_ok(fdk_calendar_set_month(cal, 1970, 2)));
    cell_name(cal, 0, name, sizeof(name));
    assert(strcmp(name, "1970-01-26") == 0);
    cell_name(cal, 5, name, sizeof(name));
    assert(strcmp(name, "1970-01-31") == 0);
    cell_name(cal, 6, name, sizeof(name));
    assert(strcmp(name, "1970-02-01") == 0);
    cell_name(cal, 33, name, sizeof(name));
    assert(strcmp(name, "1970-02-28") == 0);
    cell_name(cal, 34, name, sizeof(name));
    assert(strcmp(name, "1970-03-01") == 0);

    /* Leap February 2024 (29 days, Thursday lead = 3): cell 3 =
     * Feb 1, cell 31 = Feb 29. */
    assert(fdk_ok(fdk_calendar_set_month(cal, 2024, 2)));
    cell_name(cal, 3, name, sizeof(name));
    assert(strcmp(name, "2024-02-01") == 0);
    cell_name(cal, 31, name, sizeof(name));
    assert(strcmp(name, "2024-02-29") == 0);
    cell_name(cal, 32, name, sizeof(name));
    assert(strcmp(name, "2024-03-01") == 0);

    /* A December straddling the year edge: cell 0 of 2025-12-01
     * (Monday!) is Dec 1 itself. */
    assert(fdk_ok(fdk_calendar_set_month(cal, 2025, 12)));
    cell_name(cal, 0, name, sizeof(name));
    assert(strcmp(name, "2025-12-01") == 0);
    cell_name(cal, 30, name, sizeof(name));
    assert(strcmp(name, "2025-12-31") == 0);
    cell_name(cal, 31, name, sizeof(name));
    assert(strcmp(name, "2026-01-01") == 0);

    /* Virtual ACTIVATE selects (the automation seam). */
    assert(fdk_ok(fdk_a11y_virtual_perform(cal, 31,
                                           FDK_A11Y_ACTION_ACTIVATE,
                                           0.0)));
    fdk_date d;
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 2026 && d.month == 1 && d.day == 1);
    fdk_i32 by = 0, bm = 0;
    fdk_calendar_get_month(cal, &by, &bm);
    assert(by == 2026 && bm == 1);

    fdk_widget_destroy(root);
    printf("[ok] calendar grid: 42 cells, Monday lead, month edges, "
           "leap February, year rollover, virtual activate\n");
}

/* ---- clicks + keyboard walking ---- */

static void test_calendar_input(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cal = fresh_calendar(root);
    fdk_size nat = {0, 0};
    fdk_widget_measure(cal, &nat);

    /* Derive the grid origin from the a11y cell bounds (public
     * geometry — the same rect the paint uses). */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, 3, &info)));
    fdk_rect jan1 = info.bounds; /* 1970-01-01 (the Thursday) */
    fdk_a11y_info_free(&info);
    fdk_i32 cw = jan1.width;
    fdk_i32 chh = jan1.height;

    /* Click Jan 1's center: selects. */
    sel_count = 0;
    fdk_calendar_set_on_selected(cal, count_selected, NULL);
    click(root, (float)(jan1.x + cw / 2), (float)(jan1.y + chh / 2));
    fdk_date d;
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 1970 && d.month == 1 && d.day == 1);
    assert(sel_count == 1);

    /* Click the trailing next-month cell (Feb 8): the grid scrolls. */
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, 41, &info)));
    fdk_rect feb8 = info.bounds;
    fdk_a11y_info_free(&info);
    click(root, (float)(feb8.x + cw / 2), (float)(feb8.y + chh / 2));
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 1970 && d.month == 2 && d.day == 8);
    fdk_i32 y, m;
    fdk_calendar_get_month(cal, &y, &m);
    assert(y == 1970 && m == 2);

    /* Keyboard: Down +7, Left -1, PageUp a month. */
    assert(fdk_widget_focus(cal));
    press(root, FDK_KEY_DOWN);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.day == 15);
    press(root, FDK_KEY_LEFT);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.day == 14);
    press(root, FDK_KEY_PAGE_UP);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 1970 && d.month == 1 && d.day == 14);
    press(root, FDK_KEY_PAGE_DOWN);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 1970 && d.month == 2 && d.day == 14);

    /* Arrows across the month edge scroll (Jan 1 Left = Dec 31). */
    fdk_date pick = {1970, 1, 1};
    assert(fdk_ok(fdk_calendar_select(cal, &pick)));
    press(root, FDK_KEY_LEFT);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 1969 && d.month == 12 && d.day == 31);

    /* Home with today set jumps there. */
    fdk_date today = {2025, 6, 15};
    fdk_calendar_set_today(cal, &today);
    press(root, FDK_KEY_HOME);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 2025 && d.month == 6 && d.day == 15);
    /* Home without today = first of month. */
    fdk_calendar_set_today(cal, NULL);
    press(root, FDK_KEY_HOME);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.year == 2025 && d.month == 6 && d.day == 1);
    /* End = last of month. */
    press(root, FDK_KEY_END);
    assert(fdk_calendar_get_date(cal, &d));
    assert(d.day == 30); /* June has 30 */

    fdk_widget_destroy(root);
    printf("[ok] calendar input: click select, trailing-cell scroll, "
           "keyboard walk + month edges, Home/End + today\n");
}

/* ---- the calendar paint (pixel pins) ---- */

static void test_calendar_paint(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *cal = fresh_calendar(root);
    fdk_date pick = {1970, 1, 1};
    assert(fdk_ok(fdk_calendar_select(cal, &pick)));

    fdk_size nat = {0, 0};
    fdk_widget_measure(cal, &nat);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 320, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);

    /* The selected cell paints the SELECTION fill. The token is
     * TRANSLUCENT (alpha 0x61) over the opaque control fill, so the
     * expected pixel is the source-over blend; the sample sits in
     * the cell's top band, clear of the centered digit glyph. */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, 3, &info)));
    fdk_rect cell = info.bounds;
    fdk_a11y_info_free(&info);
    fdk_u32 got = px_at(s, cell.x + cell.width / 2, cell.y + 2);
    fdk_color sel = fdk_theme_get_color(NULL, FDK_TK_SELECTION_BACKGROUND);
    fdk_color ctl0 = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    int gr = (int)((got >> 16) & 0xFFu), gg = (int)((got >> 8) & 0xFFu),
        gb = (int)(got & 0xFFu);
    float a = sel.a;
    int er = (int)((sel.r * a + ctl0.r * (1.0f - a)) * 255.0f + 0.5f);
    int eg = (int)((sel.g * a + ctl0.g * (1.0f - a)) * 255.0f + 0.5f);
    int eb = (int)((sel.b * a + ctl0.b * (1.0f - a)) * 255.0f + 0.5f);
    assert(gr == er && gg == eg && gb == eb);

    /* An unselected in-month cell paints the control background (top
     * band, clear of the glyph). */
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, 10, &info)));
    cell = info.bounds;
    fdk_a11y_info_free(&info);
    got = px_at(s, cell.x + cell.width / 2, cell.y + 2);
    fdk_color ctl = fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND);
    gr = (int)((got >> 16) & 0xFFu);
    gg = (int)((got >> 8) & 0xFFu);
    gb = (int)(got & 0xFFu);
    assert(gr == (int)(ctl.r * 255.0f + 0.5f) &&
           gg == (int)(ctl.g * 255.0f + 0.5f) &&
           gb == (int)(ctl.b * 255.0f + 0.5f));

    /* The today marker's accent underline. */
    fdk_date today = {1970, 1, 8};
    fdk_calendar_set_today(cal, &today);
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    assert(fdk_ok(fdk_a11y_virtual_describe(cal, 10, &info)));
    cell = info.bounds;
    fdk_a11y_info_free(&info);
    /* Scan the cell's lower band for the accent color. */
    fdk_color acc = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    bool found = false;
    for (int dy = cell.height - 6; dy < cell.height && !found; dy++) {
        for (int dx = 4; dx < cell.width - 4 && !found; dx++) {
            fdk_u32 p = px_at(s, cell.x + dx, cell.y + dy);
            if ((int)((p >> 16) & 0xFFu) ==
                    (int)(acc.r * 255.0f + 0.5f) &&
                (int)((p >> 8) & 0xFFu) ==
                    (int)(acc.g * 255.0f + 0.5f) &&
                (int)(p & 0xFFu) == (int)(acc.b * 255.0f + 0.5f)) {
                found = true;
            }
        }
    }
    assert(found);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] calendar paint: selection fill, plain control fill, "
           "today accent underline\n");
}

/* ---- the date picker field ---- */

static int dp_count = 0;
static fdk_date dp_got;

static void count_dp(fdk_widget *picker, const fdk_date *d, void *user) {
    (void)picker;
    (void)user;
    dp_count++;
    dp_got = *d;
}

static void test_date_picker(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *p = NULL;
    assert(fdk_ok(fdk_date_picker_create(root, g_font, NULL, &p)));
    fdk_size nat = {0, 0};
    fdk_widget_measure(p, &nat);
    assert(nat.width > 60 && nat.height >= 16);
    fdk_widget_arrange(p, (fdk_rect){0, 0, nat.width, nat.height});

    /* Dateless by default. */
    fdk_date d;
    assert(!fdk_date_picker_get_date(p, &d));

    /* set/get round trip + callback. */
    dp_count = 0;
    fdk_date_picker_set_on_changed(p, count_dp, NULL);
    fdk_date pick = {2025, 12, 25};
    assert(fdk_ok(fdk_date_picker_set_date(p, &pick)));
    assert(dp_count == 1);
    assert(dp_got.year == 2025 && dp_got.month == 12 && dp_got.day == 25);
    assert(fdk_date_picker_get_date(p, &d));
    assert(d.year == 2025 && d.month == 12 && d.day == 25);
    /* Same date: silent. */
    assert(fdk_ok(fdk_date_picker_set_date(p, &pick)));
    assert(dp_count == 1);
    /* Invalid: rejected. */
    fdk_date bad = {2025, 2, 30};
    assert(!fdk_ok(fdk_date_picker_set_date(p, &bad)));

    /* The field paints the formatted date: ISO style reads
     * "2025-12-25". The paint pin: scan the field's text band for
     * ink. */
    fdk_date_picker_set_style(p, FDK_DATE_ISO);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 320, &s)));
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);
    fdk_rect b = fdk_widget_get_bounds(p);
    bool ink = false;
    fdk_color txt = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    for (int yy = b.y + 4; yy < b.y + b.height - 4 && !ink; yy++) {
        for (int xx = b.x + 4; xx < b.x + b.width / 2 && !ink; xx++) {
            fdk_u32 px = px_at(s, xx, yy);
            if ((int)((px >> 16) & 0xFFu) ==
                    (int)(txt.r * 255.0f + 0.5f) &&
                (int)((px >> 8) & 0xFFu) ==
                    (int)(txt.g * 255.0f + 0.5f) &&
                (int)(px & 0xFFu) == (int)(txt.b * 255.0f + 0.5f)) {
                ink = true;
            }
        }
    }
    assert(ink);

    /* The German locale changes the SHORT rendering (a paint-level
     * smoke: no assert on the string itself — the formatters have
     * their own suite — only that locale plumbing doesn't break the
     * field). */
    fdk_locale de;
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    fdk_date_picker_set_locale(p, &de);
    fdk_date_picker_set_style(p, FDK_DATE_SHORT);
    fdk_widget_invalidate_all(root);
    fdk_widget_tree_paint(root, s);

    /* Keyboard opener consumed (the popup needs a window — GUI
     * suites). */
    assert(fdk_widget_focus(p));
    fdk_event_data e = ev_key(FDK_KEY_ENTER);
    /* Standalone tree: no window to anchor a popup; the opener must
     * not crash and the picker must not report an open popup. */
    (void)fdk_widget_tree_handle_event(root, &e);
    assert(!fdk_date_picker_popup_open(p));

    /* a11y: the value interface (day-number space). */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(p, &info)));
    assert(info.role == FDK_A11Y_ROLE_DATE_EDITOR);
    assert(info.has_value);
    assert(info.value_text != NULL);
    assert(strcmp(info.value_text, "2025-12-25") == 0);
    fdk_a11y_info_free(&info);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] date picker: dateless start, set/get + silent "
           "re-set, validation, field paint, locale plumbing, "
           "headless opener safety, a11y value\n");
}

/* ---- the time picker ---- */

static int tp_count = 0;
static fdk_time tp_got;

static void count_tp(fdk_widget *picker, const fdk_time *t, void *user) {
    (void)picker;
    (void)user;
    tp_count++;
    tp_got = *t;
}

static void test_time_picker(void) {
    fdk_widget *root = fresh_root();
    fdk_widget *t = NULL;
    assert(fdk_ok(fdk_time_picker_create(root, g_font, NULL, &t)));
    fdk_size nat = {0, 0};
    fdk_widget_measure(t, &nat);
    assert(nat.width > 60 && nat.height >= 16);
    fdk_widget_arrange(t, (fdk_rect){0, 0, nat.width, nat.height});

    /* 00:00:00 default (the root locale is 24h). */
    fdk_time tm;
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 0 && tm.minute == 0 && tm.second == 0);

    /* set/get + validation + callback. */
    tp_count = 0;
    fdk_time_picker_set_on_changed(t, count_tp, NULL);
    fdk_time pick = {13, 45, 30};
    assert(fdk_ok(fdk_time_picker_set_time(t, &pick)));
    assert(tp_count == 1);
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 13 && tm.minute == 45 && tm.second == 30);
    assert(fdk_ok(fdk_time_picker_set_time(t, &pick)));
    assert(tp_count == 1); /* silent re-set */
    fdk_time bad = {24, 0, 0};
    assert(!fdk_ok(fdk_time_picker_set_time(t, &bad)));
    bad = (fdk_time){0, 60, 0};
    assert(!fdk_ok(fdk_time_picker_set_time(t, &bad)));
    bad = (fdk_time){0, 0, -1};
    assert(!fdk_ok(fdk_time_picker_set_time(t, &bad)));

    /* Keyboard stepping (24h, SHORT): hour wraps 23 -> 0. */
    fdk_time zero = {23, 59, 0};
    assert(fdk_ok(fdk_time_picker_set_time(t, &zero)));
    assert(fdk_widget_focus(t));
    press(root, FDK_KEY_UP); /* hour 23 -> 0 (wrap) */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 0 && tm.minute == 59);

    /* RIGHT moves to the minute segment; Up wraps 59 -> 0. */
    press(root, FDK_KEY_RIGHT);
    press(root, FDK_KEY_UP);
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 0 && tm.minute == 0);

    /* Down steps back: minute 0 -> 59. */
    press(root, FDK_KEY_DOWN);
    fdk_time_picker_get_time(t, &tm);
    assert(tm.minute == 59);

    /* MEDIUM adds the second segment; PageUp steps minutes by 10. */
    fdk_time_picker_set_style(t, FDK_TIME_MEDIUM);
    fdk_time base = {10, 30, 15};
    assert(fdk_ok(fdk_time_picker_set_time(t, &base)));
    press(root, FDK_KEY_LEFT); /* minute -> hour  */
    press(root, FDK_KEY_RIGHT); /* hour -> minute */
    press(root, FDK_KEY_PAGE_UP); /* minute +10 */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.minute == 40 && tm.second == 15);
    press(root, FDK_KEY_PAGE_DOWN); /* minute -10 */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.minute == 30);

    /* Forced 12H: the AM/PM segment exists; stepping the HOUR walks
     * 1..12 keeping the daypart; the DAYPART toggle flips 12h.
     * (Active segment entering this block: minute — walk left to
     * the hour first.) */
    fdk_time_picker_set_style(t, FDK_TIME_12H);
    fdk_time noon = {12, 0, 0};
    assert(fdk_ok(fdk_time_picker_set_time(t, &noon)));
    press(root, FDK_KEY_LEFT); /* minute -> hour */
    press(root, FDK_KEY_UP); /* 12 -> 1 PM */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 13);
    press(root, FDK_KEY_DOWN); /* 1 PM -> 12 PM */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 12);
    /* Walk to the daypart segment and toggle. */
    press(root, FDK_KEY_RIGHT); /* minute */
    press(root, FDK_KEY_RIGHT); /* minute -> daypart (12H has no seconds) */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 12);
    press(root, FDK_KEY_UP); /* toggle PM -> AM */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 0);
    press(root, FDK_KEY_DOWN); /* toggle back */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 12);

    /* The en locale defaults 12h (SHORT follows the locale). */
    fdk_locale en;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    fdk_time_picker_set_locale(t, &en);
    fdk_time_picker_set_style(t, FDK_TIME_SHORT);
    fdk_time eleven = {23, 5, 0};
    assert(fdk_ok(fdk_time_picker_set_time(t, &eleven)));
    /* Up on the hour (the active segment is daypart now — set it
     * back to hour by walking left). */
    press(root, FDK_KEY_LEFT); /* daypart -> minute */
    press(root, FDK_KEY_LEFT); /* minute -> hour */
    press(root, FDK_KEY_UP); /* 11 PM -> 12 PM (hour 12) */
    fdk_time_picker_get_time(t, &tm);
    assert(tm.hour == 12); /* 12 PM */

    /* Click a segment to activate it: the geometry comes from the
     * paint layout; the minute segment sits after "HH : ". */
    fdk_size nat2 = {0, 0};
    fdk_widget_measure(t, &nat2);
    /* The minute zone: between a third and two thirds of the field,
     * left of the stepper zone. */
    click(root, (float)((float)nat2.width * 0.45f),
          (float)(nat2.height / 2));
    press(root, FDK_KEY_UP);
    fdk_time_picker_get_time(t, &tm);
    assert(tm.minute == 6); /* 5 -> 6 */

    /* a11y value: seconds-of-day. */
    fdk_a11y_info info;
    assert(fdk_ok(fdk_a11y_describe(t, &info)));
    assert(info.role == FDK_A11Y_ROLE_DATE_EDITOR);
    assert(info.has_value);
    assert(info.value_min == 0.0 && info.value_max == 86399.0);
    double want = tm.hour * 3600 + tm.minute * 60 + tm.second;
    assert(info.value_current == want);
    fdk_a11y_info_free(&info);

    fdk_widget_destroy(root);
    printf("[ok] time picker: defaults, validation, silent re-set, "
           "segment walking, wraparound, 12h daypart math, locale "
           "cycle, click activation, a11y value\n");
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
        printf("[skip] no system TrueType font found — calendar "
               "geometry needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_calendar_model();
    test_calendar_grid();
    test_calendar_input();
    test_calendar_paint();
    test_date_picker();
    test_time_picker();

    fdk_font_destroy(g_font);
    printf("all calendar tests passed\n");
    return 0;
}
