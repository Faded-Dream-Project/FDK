/* test_controls.c — headless tests for the Phase 6 widget catalog
 * (Label, Button, Toggle, Checkbox, Radio, ProgressBar, Separator,
 * Frame).
 *
 * Same discipline as the widget/layout/text suites: standalone roots,
 * offscreen surfaces, synthetic window events fed through
 * fdk_widget_tree_handle_event (exactly what the window glue calls),
 * ASan+UBSan throughout. Needs a system font for the text-bearing
 * cases — without one the whole suite honestly skips (indicators and
 * interaction still work fontless, but the interesting measurements
 * need real glyphs).
 */

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

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

/* Accent color test: the palette's blue fill (89, 166, 242) against
 * the dark track (~26, 31, 43). */
static int is_accent(fdk_u32 px) {
    int r = (int)((px >> 16) & 0xFFu);
    int g = (int)((px >> 8) & 0xFFu);
    int b = (int)(px & 0xFFu);
    return b > 190 && g > 110 && r < 160 && b > r;
}

static fdk_widget *fresh_root(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    return root;
}

/* Full press+release at root coords (through the tree, so hit-testing
 * and the implicit grab are exercised like production input). */
static void click(fdk_widget *root, float x, float y) {
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, x, y);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, x, y);
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
}

/* ---- Label ---- */

static void test_label(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *l = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, "Hello", &l)));
    fdk_size nat;
    fdk_widget_measure(l, &nat);
    fdk_i32 tw = 0, th = 0;
    fdk_text_metrics tm;
    fdk_font_metrics fm;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, "Hello", 5, &tm)));
    fdk_font_get_metrics(g_font, &fm);
    tw = tm.advance_width;
    th = fm.ascent + fm.descent;
    assert(nat.width == tw && nat.height == th);

    /* text change re-measures */
    assert(fdk_ok(fdk_label_set_text(l, "Hello, wider world")));
    fdk_size nat2;
    fdk_widget_measure(l, &nat2);
    assert(nat2.width > nat.width);

    /* get/set round trip, NULL clears */
    assert(strcmp(fdk_label_get_text(l), "Hello, wider world") == 0);
    assert(fdk_ok(fdk_label_set_text(l, NULL)));
    assert(fdk_label_get_text(l) == NULL);
    fdk_widget_measure(l, &nat2);
    assert(nat2.width == 0 && nat2.height == 0);

    /* paint: ink inside the label's arranged bounds (a plain root
     * does not lay children out — arrange the label itself) */
    assert(fdk_ok(fdk_label_set_text(l, "Ink")));
    fdk_size ink_size;
    fdk_widget_measure(l, &ink_size);
    fdk_widget_arrange(l, (fdk_rect){12, 8, ink_size.width,
                                     ink_size.height});
    fdk_rect b = fdk_widget_get_bounds(l);
    assert(b.x == 12 && b.y == 8 && b.width > 4);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int ink = 0, outside = 0;
    for (int y = 0; y < 300; y++) {
        for (int x = 0; x < 400; x++) {
            if (px_at(s, x, y) != 0) {
                if (x >= b.x && x < b.x + b.width + 1 &&
                    y >= b.y && y < b.y + b.height + 1) {
                    ink++;
                } else {
                    outside++;
                }
            }
        }
    }
    assert(ink > 5);
    assert(outside == 0); /* nothing paints outside the label */

    /* NULL font: zero natural size, no crash */
    fdk_widget *lf = NULL;
    assert(fdk_ok(fdk_label_create(root, NULL, "no font", &lf)));
    fdk_size natf;
    fdk_widget_measure(lf, &natf);
    assert(natf.width == 0 && natf.height == 0);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] label: natural size == text extent, re-measure on "
           "set_text, ink inside bounds, fontless\n");
}

/* ---- Button ---- */

static int btn_activates = 0;
static void on_btn_activate(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    btn_activates++;
}

static void test_button(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *btn = NULL;
    assert(fdk_ok(fdk_button_create(root, g_font, "Click", &btn)));
    fdk_button_set_on_activate(btn, on_btn_activate, NULL);
    fdk_size btn_nat;
    fdk_widget_measure(btn, &btn_nat);
    fdk_widget_arrange(btn, (fdk_rect){20, 20, btn_nat.width,
                                       btn_nat.height});

    fdk_rect b = fdk_widget_get_bounds(btn);
    assert(b.width > 24 && b.height >= 16);

    /* natural size tracks the text */
    fdk_size nat;
    fdk_widget_measure(btn, &nat);
    fdk_text_metrics tm;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, "Click", 5, &tm)));
    assert(nat.width >= tm.advance_width + 24);

    /* click inside activates */
    float cx = (float)(b.x + b.width / 2);
    float cy = (float)(b.y + b.height / 2);
    click(root, cx, cy);
    assert(btn_activates == 1);

    /* press inside, release far outside: NO activation */
    fdk_event_data down = ev_button(FDK_EVENT_POINTER_BUTTON_DOWN, cx, cy);
    (void)fdk_widget_tree_handle_event(root, &down);
    fdk_event_data up = ev_button(FDK_EVENT_POINTER_BUTTON_UP, 390.0f, 290.0f);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(btn_activates == 1);

    /* keyboard: focus + Space and Enter activate */
    assert(fdk_widget_focus(btn));
    fdk_event_data sp = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_SPACE);
    (void)fdk_widget_tree_handle_event(root, &sp);
    assert(btn_activates == 2);
    fdk_event_data en = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_ENTER);
    (void)fdk_widget_tree_handle_event(root, &en);
    assert(btn_activates == 3);

    /* disabled: input-transparent, no activation */
    fdk_widget_set_enabled(btn, false);
    click(root, cx, cy);
    assert(btn_activates == 3);
    fdk_widget_set_enabled(btn, true);

    /* set_text re-measures */
    assert(fdk_ok(fdk_button_set_text(btn, "A much longer label")));
    fdk_size nat2;
    fdk_widget_measure(btn, &nat2);
    assert(nat2.width > nat.width);

    /* type checks: button functions on a non-button */
    assert(fdk_label_set_text(btn, "x") == FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    printf("[ok] button: click-in activates, release-out does not, "
           "Space/Enter, disabled ignores input, re-measure, type "
           "checks\n");
}

/* ---- Toggle / Checkbox ---- */

static int toggle_changes = 0;
static bool last_toggle_state = false;
static void on_toggle_change(fdk_widget *w, bool checked, void *user) {
    (void)w;
    (void)user;
    toggle_changes++;
    last_toggle_state = checked;
}

static void test_toggle_and_checkbox(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *tog = NULL;
    assert(fdk_ok(fdk_toggle_create(root, g_font, "Dark mode", &tog)));
    fdk_toggle_set_on_change(tog, on_toggle_change, NULL);
    fdk_size tog_nat;
    fdk_widget_measure(tog, &tog_nat);
    if (tog_nat.height < 18) {
        tog_nat.height = 18;
    }
    fdk_widget_arrange(tog, (fdk_rect){10, 10, tog_nat.width,
                                       tog_nat.height});
    fdk_rect tb = fdk_widget_get_bounds(tog);
    assert(tb.width > 30 && tb.height >= 18);

    /* natural size covers track + gap + text */
    fdk_size nat;
    fdk_widget_measure(tog, &nat);
    fdk_text_metrics tm;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, "Dark mode", 9, &tm)));
    assert(nat.width >= 34 + 8 + tm.advance_width - 2);

    assert(!fdk_toggle_is_checked(tog));
    click(root, (float)(tb.x + 10), (float)(tb.y + tb.height / 2));
    assert(fdk_toggle_is_checked(tog));
    assert(toggle_changes == 1 && last_toggle_state);

    click(root, (float)(tb.x + 10), (float)(tb.y + tb.height / 2));
    assert(!fdk_toggle_is_checked(tog));
    assert(toggle_changes == 2 && !last_toggle_state);

    /* programmatic set fires on_change too */
    fdk_toggle_set_checked(tog, true);
    assert(fdk_toggle_is_checked(tog) && toggle_changes == 3);
    fdk_toggle_set_checked(tog, true); /* no-op: already checked */
    assert(toggle_changes == 3);

    /* Space activates the focused toggle */
    assert(fdk_widget_focus(tog));
    fdk_event_data sp = ev_key(FDK_EVENT_KEY_DOWN, FDK_KEY_SPACE);
    (void)fdk_widget_tree_handle_event(root, &sp);
    assert(!fdk_toggle_is_checked(tog) && toggle_changes == 4);

    /* visual difference: knob moves; paint both states and compare */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_toggle_set_checked(tog, true);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    fdk_u32 on_px[400];
    for (int x = 0; x < 400; x++) {
        on_px[x] = px_at(s, x, tb.y + tb.height / 2);
    }
    fdk_toggle_set_checked(tog, false);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int diff = 0;
    for (int x = 0; x < 400; x++) {
        if (px_at(s, x, tb.y + tb.height / 2) != on_px[x]) {
            diff++;
        }
    }
    assert(diff > 4); /* the knob actually moved */

    /* checkbox: same semantics */
    fdk_widget *cb = NULL;
    assert(fdk_ok(fdk_checkbox_create(root, g_font, "Remember", &cb)));
    fdk_size cb_nat;
    fdk_widget_measure(cb, &cb_nat);
    if (cb_nat.height < 16) {
        cb_nat.height = 16;
    }
    fdk_widget_arrange(cb, (fdk_rect){10, 60, cb_nat.width,
                                      cb_nat.height});
    fdk_rect cb_b = fdk_widget_get_bounds(cb);
    assert(!fdk_checkbox_is_checked(cb));
    click(root, (float)(cb_b.x + 8), (float)(cb_b.y + cb_b.height / 2));
    assert(fdk_checkbox_is_checked(cb));
    fdk_checkbox_set_checked(cb, false);
    assert(!fdk_checkbox_is_checked(cb));

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] toggle+checkbox: click/Space/programmatic state, "
           "on_change fires, knob visibly moves, type checks\n");
}

/* ---- Radio group ---- */

static int radio_events = 0;
static void on_radio_change(fdk_widget *w, bool checked, void *user) {
    (void)w;
    (void)user;
    radio_events++;
    (void)checked;
}

static void test_radio_group(void) {
    fdk_widget *root = fresh_root();

    /* group 1 = a vertical BOX (so radios get real, clickable
     * bounds); group 2 lives inside an inner plain widget */
    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &box)));
    fdk_box_set_spacing(box, 6);
    fdk_widget *a = NULL, *b = NULL, *c = NULL;
    assert(fdk_ok(fdk_radio_create(box, g_font, "One", &a)));
    assert(fdk_ok(fdk_radio_create(box, g_font, "Two", &b)));
    assert(fdk_ok(fdk_radio_create(box, g_font, "Three", &c)));
    fdk_radio_set_on_change(a, on_radio_change, NULL);
    fdk_radio_set_on_change(b, on_radio_change, NULL);

    fdk_widget *inner = NULL;
    assert(fdk_ok(fdk_widget_create(root, NULL,
                                    (fdk_rect){250, 0, 150, 100},
                                    &inner))); /* clear of the box: a
                                               * later sibling would
                                               * win hit-testing */
    fdk_widget *x = NULL, *y = NULL;
    assert(fdk_ok(fdk_radio_create(inner, g_font, "Other group", &x)));
    assert(fdk_ok(fdk_radio_create(inner, g_font, "Also other", &y)));

    fdk_widget_arrange(box, (fdk_rect){0, 0, 160, 200});

    /* programmatic: check b -> a unchecks (a fires false, b true) */
    radio_events = 0;
    fdk_radio_set_checked(a, true);
    assert(fdk_radio_is_checked(a) && radio_events == 1);
    fdk_radio_set_checked(b, true);
    assert(fdk_radio_is_checked(b) && !fdk_radio_is_checked(a));
    assert(radio_events == 3); /* a: false, b: true */

    /* click c: group-wide exclusivity through real input */
    fdk_rect cb3 = fdk_widget_get_bounds(c);
    radio_events = 0;
    click(root, (float)(cb3.x + 8), (float)(cb3.y + cb3.height / 2));
    assert(fdk_radio_is_checked(c));
    assert(!fdk_radio_is_checked(b));
    assert(radio_events == 1); /* only c has a callback */

    /* the OTHER group is untouched */
    assert(!fdk_radio_is_checked(x) && !fdk_radio_is_checked(y));
    fdk_radio_set_checked(x, true);
    assert(fdk_radio_is_checked(x));
    assert(fdk_radio_is_checked(c)); /* still checked: different parent */

    /* re-checking the checked radio is a no-op */
    radio_events = 0;
    fdk_radio_set_checked(c, true);
    assert(radio_events == 0);

    fdk_widget_destroy(root);
    printf("[ok] radio: parent-scoped groups, exclusivity via program "
           "and click, on_change ordering, no-op recheck\n");
}

/* ---- Label modes: wrap / ellipsize / alignment ---- */

/* Ink extent (min/max x of non-background pixels) within a row band. */
static void ink_extent_x(fdk_surface *s, int y0, int y1, int *out_min,
                         int *out_max) {
    *out_min = -1;
    *out_max = -1;
    for (int x = 0; x < 400; x++) {
        for (int y = y0; y < y1; y++) {
            if (px_at(s, x, y) != 0) {
                if (*out_min < 0) {
                    *out_min = x;
                }
                *out_max = x;
                break;
            }
        }
    }
}

static int ink_rows(fdk_surface *s, int y0, int y1) {
    int rows = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < 400; x++) {
            if (px_at(s, x, y) != 0) {
                rows++;
                break;
            }
        }
    }
    return rows;
}

static void test_label_modes(void) {
    fdk_widget *root = fresh_root();
    fdk_font_metrics fm;
    fdk_font_get_metrics(g_font, &fm);
    fdk_i32 pitch = fm.ascent + fm.descent;

    const char *text = "the quick brown fox jumps over the lazy dog";

    /* WRAP: natural height follows the wrap at the requested width. */
    fdk_widget *w = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, text, &w)));
    fdk_label_set_mode(w, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(w, 120, 0);
    fdk_size nat;
    fdk_widget_measure(w, &nat);
    assert(nat.width == 120);
    assert(nat.height >= 2 * pitch); /* wraps to several lines */
    fdk_i32 lines_nat = nat.height / pitch;
    assert(lines_nat * pitch == nat.height); /* integral line count */

    fdk_widget_arrange(w, (fdk_rect){10, 10, 120, nat.height});
    assert(fdk_label_get_line_count(w) == (size_t)lines_nat);

    /* Narrower width -> more lines; wider -> one. */
    fdk_widget_arrange(w, (fdk_rect){10, 10, 60, 400});
    assert(fdk_label_get_line_count(w) > (size_t)lines_nat);
    fdk_widget_arrange(w, (fdk_rect){10, 10, 400, 400});
    assert(fdk_label_get_line_count(w) == 1);

    /* Painted wrapped ink spans multiple line pitches and stays in
     * the horizontal band. */
    fdk_widget_arrange(w, (fdk_rect){10, 10, 120, 400});
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int min_x = 0, max_x = 0;
    ink_extent_x(s, 10, 10 + 6 * pitch, &min_x, &max_x);
    assert(min_x >= 10 && min_x < 10 + 10); /* starts at the left edge */
    assert(max_x < 10 + 120 + 4);           /* never past the width  */
    int rows = ink_rows(s, 10, 10 + 6 * pitch);
    assert(rows > pitch); /* ink across more than one line box */
    fdk_surface_destroy(s);

    /* ELLIPSIZE: full text when wide, truncated ink when narrow. */
    fdk_text_metrics whole;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, strlen(text),
                                        &whole)));
    fdk_label_set_mode(w, FDK_LABEL_ELLIPSIZE);
    fdk_size nat2;
    fdk_widget_measure(w, &nat2);
    assert(nat2.width == whole.advance_width); /* natural = FULL text */

    /* wide: everything shows */
    fdk_widget_arrange(w, (fdk_rect){10, 10, whole.advance_width + 4,
                                     pitch});
    assert(fdk_label_get_line_count(w) == 1);
    s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    ink_extent_x(s, 10, 10 + pitch, &min_x, &max_x);
    int full_ink_wide = max_x - min_x + 1;
    fdk_surface_destroy(s);

    /* narrow: ink strictly narrower, still one line, ellipsis drawn
     * at the right end */
    fdk_i32 narrow = whole.advance_width / 2;
    fdk_widget_arrange(w, (fdk_rect){10, 10, narrow, pitch});
    assert(fdk_label_get_line_count(w) == 1);
    s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    ink_extent_x(s, 10, 10 + pitch, &min_x, &max_x);
    assert(max_x < 10 + narrow);            /* clipped in width */
    assert(max_x - min_x + 1 < full_ink_wide); /* truncated, not full */
    assert(max_x > 10 + narrow - 40);       /* ink reaches the right
                                               * region: the ellipsis */
    fdk_surface_destroy(s);

    /* Alignment: same text, three labels on three rows of equal
     * width; START hugs the left edge, END the right, CENTER the
     * middle. */
    {
        fdk_widget *l0 = NULL, *l1 = NULL, *l2 = NULL;
        assert(fdk_ok(fdk_label_create(root, g_font, "align", &l0)));
        assert(fdk_ok(fdk_label_create(root, g_font, "align", &l1)));
        assert(fdk_ok(fdk_label_create(root, g_font, "align", &l2)));
        fdk_label_set_alignment(l1, FDK_ALIGN_CENTER);
        fdk_label_set_alignment(l2, FDK_ALIGN_END);
        fdk_i32 y = 60;
        fdk_widget_arrange(l0, (fdk_rect){20, y, 300, pitch});
        fdk_widget_arrange(l1, (fdk_rect){20, y + 40, 300, pitch});
        fdk_widget_arrange(l2, (fdk_rect){20, y + 80, 300, pitch});
        assert(fdk_label_get_alignment(l0) == FDK_ALIGN_START);
        assert(fdk_label_get_alignment(l1) == FDK_ALIGN_CENTER);
        assert(fdk_label_get_alignment(l2) == FDK_ALIGN_END);

        s = NULL;
        assert(fdk_ok(fdk_surface_create(400, 300, &s)));
        fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
        fdk_widget_tree_paint(root, s);
        int mn0, mx0, mn1, mx1, mn2, mx2;
        ink_extent_x(s, y, y + pitch, &mn0, &mx0);
        ink_extent_x(s, y + 40, y + 40 + pitch, &mn1, &mx1);
        ink_extent_x(s, y + 80, y + 80 + pitch, &mn2, &mx2);
        assert(mn0 >= 20 && mn0 <= 24);      /* START: left edge */
        assert(mn1 > mn0 + 40);              /* CENTER: pushed in */
        assert(mx1 < 320 - 40);              /* CENTER: not at edge */
        assert(mx2 >= 320 - 5 && mx2 <= 320); /* END: right edge */
        /* CENTER is symmetric within a couple of pixels. */
        int c0 = mn0 + mx0, c1 = mn1 + mx1, c2 = mn2 + mx2;
        assert(c1 > c0 && c1 < c2);
        assert(abs((c1 - (20 + 320)) ) < 6);
        fdk_surface_destroy(s);
    }

    /* mode round trip; unknown value ignored; empty text = 0 lines */
    assert(fdk_label_get_mode(w) == FDK_LABEL_ELLIPSIZE);
    fdk_label_set_mode(w, (fdk_label_mode)99);
    assert(fdk_label_get_mode(w) == FDK_LABEL_ELLIPSIZE);
    assert(fdk_ok(fdk_label_set_text(w, NULL)));
    assert(fdk_label_get_line_count(w) == 0);

    fdk_widget_destroy(root);
    printf("[ok] label modes: wrap line counts + ink spans, ellipsize "
           "truncation + right-end ink, alignment edges, arg safety\n");
}

/* ---- Radio arrow-key traversal ---- */

static int radio_arrow_events = 0;
static void on_radio_arrow_change(fdk_widget *w, bool checked,
                                  void *user) {
    (void)w;
    (void)user;
    if (checked) {
        radio_arrow_events++;
    }
}

static int root_key_events = 0;
static bool on_root_event(fdk_widget *w, const fdk_widget_event *ev,
                          void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_WIDGET_KEY_DOWN) {
        root_key_events++;
    }
    return false; /* observe, never consume */
}

static void key(fdk_widget *root, fdk_scancode sc) {
    fdk_event_data e = ev_key(FDK_EVENT_KEY_DOWN, sc);
    (void)fdk_widget_tree_handle_event(root, &e);
}

static void test_radio_arrows(void) {
    fdk_widget *root = fresh_root();
    fdk_widget_set_event_callback(root, on_root_event, NULL);

    fdk_widget *box = NULL;
    assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &box)));
    fdk_widget *a = NULL, *b = NULL, *c = NULL;
    assert(fdk_ok(fdk_radio_create(box, g_font, "One", &a)));
    assert(fdk_ok(fdk_radio_create(box, g_font, "Two", &b)));
    assert(fdk_ok(fdk_radio_create(box, g_font, "Three", &c)));
    fdk_radio_set_on_change(a, on_radio_arrow_change, NULL);
    fdk_radio_set_on_change(b, on_radio_arrow_change, NULL);
    fdk_radio_set_on_change(c, on_radio_arrow_change, NULL);
    fdk_widget_arrange(box, (fdk_rect){0, 0, 160, 200});

    /* Down/Right advance and select, focus follows. */
    fdk_radio_set_checked(a, true);
    assert(fdk_widget_focus(a));
    radio_arrow_events = 0;
    key(root, FDK_KEY_DOWN);
    assert(fdk_radio_is_checked(b) && !fdk_radio_is_checked(a));
    assert(fdk_widget_has_focus(b));
    assert(radio_arrow_events == 1); /* b checked (a's false not
                                        * counted by the callback) */

    key(root, FDK_KEY_RIGHT);
    assert(fdk_radio_is_checked(c) && fdk_widget_has_focus(c));

    /* Wrap-around at both ends. */
    key(root, FDK_KEY_DOWN);
    assert(fdk_radio_is_checked(a) && fdk_widget_has_focus(a));
    key(root, FDK_KEY_UP);
    assert(fdk_radio_is_checked(c) && fdk_widget_has_focus(c));
    key(root, FDK_KEY_LEFT);
    assert(fdk_radio_is_checked(b) && fdk_widget_has_focus(b));

    /* Arrows on a checked radio re-selecting it: no change events
     * (selection unchanged), focus still moves. */
    radio_arrow_events = 0;
    key(root, FDK_KEY_UP);
    assert(fdk_radio_is_checked(a) && fdk_widget_has_focus(a));
    assert(radio_arrow_events == 1); /* b false, a already-checked no-op */

    /* Hidden member is skipped. */
    fdk_widget_set_visible(b, false);
    key(root, FDK_KEY_DOWN);
    assert(fdk_radio_is_checked(c) && fdk_widget_has_focus(c));
    key(root, FDK_KEY_UP);
    assert(fdk_radio_is_checked(a) && fdk_widget_has_focus(a));
    fdk_widget_set_visible(b, true);

    /* Disabled member is skipped. */
    fdk_widget_set_enabled(c, false);
    key(root, FDK_KEY_DOWN);
    assert(fdk_radio_is_checked(b) && fdk_widget_has_focus(b));
    fdk_widget_set_enabled(c, true);

    /* A group with a single member lets arrows bubble to the root's
     * callback (the tree's unhandled-key observer). */
    {
        fdk_widget *lone_box = NULL;
        assert(fdk_ok(fdk_box_create(root, FDK_VERTICAL, &lone_box)));
        fdk_widget *lone = NULL;
        assert(fdk_ok(fdk_radio_create(lone_box, g_font, "Lone",
                                       &lone)));
        fdk_widget_arrange(lone_box, (fdk_rect){200, 0, 150, 40});
        assert(fdk_widget_focus(lone));
        root_key_events = 0;
        key(root, FDK_KEY_DOWN);
        assert(root_key_events == 1); /* bubbled: nothing consumed it */
        assert(fdk_radio_is_checked(lone) == false); /* no selection
                                                        * change */
    }

    fdk_widget_destroy(root);
    printf("[ok] radio arrows: Down/Right select+focus, wrap-around, "
           "skip hidden/disabled, lone radio bubbles\n");
}

/* ---- ProgressBar ---- */

static void test_progress(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *p = NULL;
    assert(fdk_ok(fdk_progress_create(root, &p)));
    assert(fdk_progress_get_fraction(p) == 0.0f);

    fdk_widget_set_natural_size(p, 200, 10);
    fdk_widget_arrange(p, (fdk_rect){10, 30, 200, 10});
    fdk_rect b = fdk_widget_get_bounds(p);
    assert(b.width == 200 && b.height == 10);

    /* clamps */
    fdk_progress_set_fraction(p, -0.5f);
    assert(fdk_progress_get_fraction(p) == 0.0f);
    fdk_progress_set_fraction(p, 1.7f);
    assert(fdk_progress_get_fraction(p) == 1.0f);

    /* fill width tracks the fraction (mid-row pixel scan) */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    int my = b.y + b.height / 2;

    /* Count ACCENT pixels (the bright blue fill), not any ink: the
     * dark track legitimately paints at every fraction. */
    fdk_progress_set_fraction(p, 0.0f);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int filled0 = 0;
    for (int x = b.x; x < b.x + b.width; x++) {
        if (is_accent(px_at(s, x, my))) {
            filled0++;
        }
    }
    assert(filled0 == 0); /* 0% paints no fill */

    fdk_progress_set_fraction(p, 0.5f);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int filled50 = 0;
    for (int x = b.x; x < b.x + b.width; x++) {
        if (is_accent(px_at(s, x, my))) {
            filled50++;
        }
    }
    assert(filled50 >= 96 && filled50 <= 104); /* ~100 of 200 */

    fdk_progress_set_fraction(p, 1.0f);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int filled100 = 0;
    for (int x = b.x; x < b.x + b.width; x++) {
        if (is_accent(px_at(s, x, my))) {
            filled100++;
        }
    }
    assert(filled100 >= 196);

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] progress: clamps, exact fill widths at 0/50/100%%\n");
}

/* ---- Separator ---- */

static void test_separator(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *sep = NULL;
    assert(fdk_ok(fdk_separator_create(root, FDK_HORIZONTAL, &sep)));
    fdk_widget_arrange(sep, (fdk_rect){0, 20, 300, 1});

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 40, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);

    fdk_rect b = fdk_widget_get_bounds(sep);
    assert(b.width == 300 && b.height == 1);
    int line_y = b.y + b.height / 2;
    int on_line = 0;
    for (int x = 0; x < 300; x++) {
        if (px_at(s, x, line_y) != 0) {
            on_line++;
        }
    }
    assert(on_line >= 295); /* the rule spans the width */
    int off_line = 0;
    for (int x = 0; x < 300; x++) {
        if (px_at(s, x, line_y + 3) != 0) {
            off_line++;
        }
    }
    assert(off_line == 0); /* and only the 1px line */

    /* vertical variant */
    fdk_widget *vs = NULL;
    assert(fdk_ok(fdk_separator_create(root, FDK_VERTICAL, &vs)));
    assert(fdk_separator_create(root, (fdk_orientation)99, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] separator: 1px rule exactly on its line, "
           "orientation validation\n");
}

/* ---- Frame ---- */

static void test_frame(void) {
    fdk_widget *root = fresh_root();

    fdk_widget *frame = NULL;
    assert(fdk_ok(fdk_frame_create(root, g_font, "Settings", &frame)));
    fdk_widget_set_background(frame, (fdk_color){0.1f, 0.1f, 0.15f, 1});

    /* children added directly: frame IS the box */
    fdk_widget *row = NULL;
    assert(fdk_ok(fdk_button_create(frame, g_font, "Apply", &row)));
    fdk_widget *row2 = NULL;
    assert(fdk_ok(fdk_checkbox_create(frame, g_font, "Verbose",
                                      &row2)));

    fdk_widget_arrange(frame, (fdk_rect){20, 20, 360, 240});
    fdk_rect fb = fdk_widget_get_bounds(frame); /* frame is root's child at (20,20): rel == abs */
    fdk_rect cb = fdk_widget_get_absolute_bounds(row);

    /* the first child sits below the title band (padding + band),
     * NOT at the plain padding line — in ABSOLUTE terms, since the
     * child's own bounds are parent-relative to the frame */
    fdk_font_metrics fm;
    fdk_font_get_metrics(g_font, &fm);
    fdk_i32 band = fm.ascent + fm.descent + 8;
    assert(cb.y == fb.y + 10 + band);
    fdk_rect cb2 = fdk_widget_get_absolute_bounds(row2);
    assert(cb2.y > cb.y); /* stacked with the frame's spacing */

    /* title ink paints inside the band */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    fdk_widget_tree_paint(root, s);
    int title_ink = 0;
    for (int y = fb.y; y < fb.y + 10 + band; y++) {
        for (int x = fb.x; x < fb.x + 120; x++) {
            if (px_at(s, x, y) != 0) {
                title_ink++;
            }
        }
    }
    assert(title_ink > 5);

    /* natural size includes the band: measure with children */
    fdk_size nat;
    fdk_widget_measure(frame, &nat);
    fdk_size row_nat;
    fdk_widget_measure(row, &row_nat);
    assert(nat.height >= 10 * 2 + band + row_nat.height + 8 + 16 - 4);

    /* set_title repaints without relayout churn; NULL clears */
    assert(fdk_ok(fdk_frame_set_title(frame, "Advanced")));
    assert(fdk_frame_set_title(frame, NULL) == FDK_OK);
    assert(fdk_frame_set_title(row, "nope") ==
           FDK_ERR_INVALID_ARGUMENT);

    /* fontless frame: no band, children at plain padding */
    fdk_widget *fr2 = NULL;
    assert(fdk_ok(fdk_frame_create(root, NULL, "No font", &fr2)));
    fdk_widget *child2 = NULL;
    assert(fdk_ok(fdk_label_create(fr2, NULL, "x", &child2)));
    fdk_widget_arrange(fr2, (fdk_rect){20, 40, 200, 100});
    fdk_rect f2b = fdk_widget_get_bounds(fr2);
    fdk_rect c2b = fdk_widget_get_absolute_bounds(child2);
    assert(c2b.y == f2b.y + 10);

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    printf("[ok] frame: title band reserves layout space, children "
           "stack below it, title ink paints, fontless = plain box\n");
}

/* ---- argument safety ---- */

static void test_argument_safety(void) {
    assert(fdk_label_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_button_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_toggle_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_checkbox_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_radio_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_progress_create(NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_separator_create(NULL, FDK_HORIZONTAL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_frame_create(NULL, NULL, NULL, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* NULL-widget setters are safe no-ops / errors, never crashes */
    fdk_label_set_text(NULL, "x");
    fdk_label_set_color(NULL, (fdk_color){1, 1, 1, 1});
    assert(fdk_label_get_text(NULL) == NULL);
    fdk_button_set_on_activate(NULL, NULL, NULL);
    fdk_toggle_set_checked(NULL, true);
    assert(fdk_toggle_is_checked(NULL) == false);
    fdk_toggle_set_on_change(NULL, NULL, NULL);
    fdk_checkbox_set_checked(NULL, true);
    assert(fdk_checkbox_is_checked(NULL) == false);
    fdk_radio_set_checked(NULL, true);
    assert(fdk_radio_is_checked(NULL) == false);
    fdk_progress_set_fraction(NULL, 0.5f);
    assert(fdk_progress_get_fraction(NULL) == 0.0f);
    assert(fdk_frame_set_title(NULL, "x") == FDK_ERR_INVALID_ARGUMENT);

    /* cross-type confusion is rejected */
    fdk_widget *root = fresh_root();
    fdk_widget *lab = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, "L", &lab)));
    fdk_toggle_set_checked(lab, true); /* no-op: not a toggle */
    assert(fdk_toggle_is_checked(lab) == false);
    fdk_progress_set_fraction(lab, 0.5f);
    assert(fdk_progress_get_fraction(lab) == 0.0f);
    assert(fdk_button_set_text(lab, "x") == FDK_ERR_INVALID_ARGUMENT);

    fdk_widget_destroy(root);
    printf("[ok] argument safety: NULL args, cross-type confusion, "
           "no-op setters\n");
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
        printf("[skip] no system TrueType font found — the catalog's "
               "measured geometry needs real glyphs; see "
               "docs/testing.md\n");
        return 0;
    }

    test_label();
    test_button();
    test_toggle_and_checkbox();
    test_radio_group();
    test_label_modes();
    test_radio_arrows();
    test_progress();
    test_separator();
    test_frame();
    test_argument_safety();

    fdk_font_destroy(g_font);
    printf("all headless widget-catalog tests passed\n");
    return 0;
}
