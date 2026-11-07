#define FDK_LOG_TAG "widgets"

/*
 * calendar.c — the calendar grid + the date/time pickers (1.1.0 / S-10)
 *
 * Three widgets, one posture: NO clock reads, NO environment
 * consultation (the i18n module's rule — "FDK formats the calendar
 * numbers it is given"). "Today" is injected, the pickers start
 * dateless / at 00:00:00, and every date goes through the i18n
 * module's validated proleptic-Gregorian math (fdk_date_to_days /
 * from_days), so a leap February or a year rollover can never
 * produce an impossible date.
 *
 * fdk_calendar — the month grid (the GtkCalendar counterpart):
 * a header (chevron nav + "Month Year" from the locale names
 * table), a weekday row (Monday-lead, ISO 8601 — the rules table
 * carries no CLDR first-day field, so the ISO default is the
 * documented fence), and SIX week rows of day cells. Leading and
 * trailing cells carry the adjacent months' days in the disabled
 * color; selecting one (click or keyboard walk) both selects the
 * date and moves the browse month — the grid "scrolls". The
 * selected day paints the selection colors; the injected "today"
 * paints an accent underline; keyboard citizens get the whole-
 * widget focus ring (the button's).
 *
 * fdk_date_picker — a formatted field (fdk_format_date, the locale
 * + style in the widget) plus a chevron zone, opening a POPUP
 * WINDOW (the menu machinery's shape, minus the menu: one popup
 * with one calendar child) at the field's bottom-left. Picking a
 * day applies, formats, fires, closes; CLOSE_REQUEST (the grab's
 * outside-click translation) and Esc dismiss unchanged. The
 * combo_guard discipline covers a picker destroyed while open:
 * the guard's picker pointer NULLs at destroy, the popup's
 * callbacks see NULL and merely unwind, and the destroy-notify /
 * closed paths free the shared context exactly once.
 *
 * fdk_time_picker — the segmented stepper: [HH] : [MM] [: SS]
 * [AM/PM] painted from the locale's digit table, one ACTIVE
 * segment (press to activate; the active segment paints the
 * selection colors), chevron zones at the right edge stepping
 * the active segment with wraparound. Keyboard: Left/Right move
 * the segment, Up/Down step, PageUp/PageDown step minutes and
 * seconds by ten / hours by six. No text mode — the level-1
 * fence named in the header.
 *
 * All three report the a11y surface their content warrants: the
 * calendar enumerates its 42 day cells as virtual children (the
 * combo's painted-row pattern) with ACTIVATE performing the
 * selection; the pickers report the value interface (current/
 * min/max in the i18n day-number space for dates, seconds-of-day
 * for times) with formatted value_text.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"
#include "../i18n/i18n_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- shared geometry knobs (font-derived; integer math only) ---- */

#define CAL_PAD 6        /* outer inset of the grid box          */
#define CAL_CELL_PAD_X 5 /* day-number inset inside a cell       */
#define CAL_CELL_PAD_Y 4
#define CAL_HEADER_GAP 4 /* header row -> weekday row            */
#define CAL_NAV_W 24     /* prev/next chevron hit square         */
#define CAL_WEEKS 6      /* rows laid out, always (stable size)  */

#define PICK_FIELD_PAD_X 8   /* picker field text inset           */
#define PICK_ZONE_W 24       /* chevron / stepper zone width      */
#define PICK_ZONE_SPLIT 12   /* time stepper: upper/lower half    */

/* =====================================================================
 * Shared helpers
 * ===================================================================== */

static const fdk_i18n_names *cal_names(const fdk_locale *loc) {
    const fdk_locale *l = (loc != NULL) ? loc : fdk_locale_root();
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    return (rules->names != NULL) ? rules->names : &fdk__i18n_names_en;
}

/* The locale the widget holds (root when never set). */

/* Cell metrics from the font, cached per widget (fonts are
 * immutable loads; the cache never goes stale). */
typedef struct cal_metrics {
    fdk_i32 cell_w;    /* a day cell                            */
    fdk_i32 cell_h;
    fdk_i32 text_h;    /* one line extent                       */
    fdk_i32 header_h;  /* month/year row                        */
    fdk_i32 weekday_h; /* weekday abbr row                      */
    fdk_i32 total_w;
    fdk_i32 total_h;
} cal_metrics;

static void cal_measure_metrics(fdk_font *font, cal_metrics *m) {
    memset(m, 0, sizeof(*m));
    if (font == NULL) {
        return;
    }
    fdk_i32 dw = 0, dh = 0;
    fdk__text_extent(font, "00", &dw, &dh);
    m->text_h = dh;
    m->cell_w = dw + CAL_CELL_PAD_X * 2;
    m->cell_h = dh + CAL_CELL_PAD_Y * 2;
    m->header_h = dh + 10;
    m->weekday_h = dh + 6;
    m->total_w = CAL_PAD * 2 + m->cell_w * 7;
    m->total_h = CAL_PAD * 2 + m->header_h + CAL_HEADER_GAP +
                 m->weekday_h + m->cell_h * CAL_WEEKS;
}

/* Day-number math over the browsed month: cell i in [0, 42) is the
 * date days(first-of-month) + (i - lead), lead = ISO weekday of the
 * first (Monday = 0 — the grid leads Monday). Returns false when
 * the day-number falls outside the supported calendar range (the
 * year-1 / year-9999 edges; those cells paint nothing). */
static bool cal_cell_date(fdk_i32 year, fdk_i32 month, fdk_i32 cell,
                          fdk_date *out) {
    fdk_date first = {year, month, 1};
    fdk_i64 days = fdk_date_to_days(&first);
    fdk_i32 lead = fdk_date_weekday(&first); /* 0 = Monday */
    return fdk_ok(fdk_date_from_days(days + (cell - lead), out));
}

/* Unambiguous validity check (to_days returns 0 both for the epoch
 * and for invalid input — this side-steps the collision). */
static bool cal_date_ok(const fdk_date *d) {
    if (d == NULL || d->year < 1 || d->year > 9999 || d->month < 1 ||
        d->month > 12) {
        return false;
    }
    return d->day >= 1 && d->day <= fdk_date_days_in_month(d->year,
                                                           d->month);
}

static bool cal_cell_in_month(fdk_i32 year, fdk_i32 month, fdk_i32 cell) {
    fdk_date first = {year, month, 1};
    fdk_i32 lead = fdk_date_weekday(&first);
    fdk_i32 dim = fdk_date_days_in_month(year, month);
    return cell >= lead && cell < lead + dim;
}

/* Month arithmetic with day clamping (PageUp/PageDown keep the
 * day-of-month where the target month allows). */
static bool cal_shift_month(fdk_i32 year, fdk_i32 month, fdk_i32 day,
                            fdk_i32 delta, fdk_date *out) {
    fdk_i64 total = (fdk_i64)year * 12 + (month - 1) + delta;
    fdk_i32 y = (fdk_i32)(total / 12);
    fdk_i32 m = (fdk_i32)(total % 12) + 1;
    if (total < 0) { /* floor division fix for the era edge */
        y = (fdk_i32)((total - 11) / 12);
        m = (fdk_i32)(total - (fdk_i64)y * 12) + 1;
    }
    if (y < 1 || y > 9999) {
        return false;
    }
    fdk_i32 dim = fdk_date_days_in_month(y, m);
    fdk_date d = {y, m, (day > dim) ? dim : day};
    *out = d;
    return true;
}

/* =====================================================================
 * fdk_calendar — the month grid
 * ===================================================================== */

typedef struct fdk_calendar_impl {
    fdk_widget base;
    fdk_font *font;         /* borrowed */
    fdk_locale *locale;     /* owned copy; NULL = root       */
    fdk_i32 year;           /* browsed month                 */
    fdk_i32 month;          /* 1..12                         */
    bool has_selection;
    fdk_date selected;
    bool has_today;
    fdk_date today;
    fdk_calendar_selected_fn on_selected;
    void *on_selected_user;
    fdk_i32 hover_cell;     /* -1 = none                     */
    bool metrics_done;
    cal_metrics m;
} fdk_calendar_impl;

static fdk_calendar_impl *cal_of(fdk_widget *w) {
    return (fdk_calendar_impl *)(void *)w;
}

/* Root-absolute cell rect for paint; bounds is the widget's. */
static fdk_rect cal_cell_rect(const fdk_calendar_impl *c, fdk_rect bounds,
                              fdk_i32 cell) {
    fdk_i32 col = cell % 7;
    fdk_i32 row = cell / 7;
    fdk_i32 grid_y = bounds.y + CAL_PAD + c->m.header_h + CAL_HEADER_GAP +
                     c->m.weekday_h;
    return (fdk_rect){
        bounds.x + CAL_PAD + col * c->m.cell_w,
        grid_y + row * c->m.cell_h,
        c->m.cell_w,
        c->m.cell_h,
    };
}

static fdk_i32 cal_hit_cell(const fdk_calendar_impl *c, fdk_i32 x,
                            fdk_i32 y) {
    if (c->m.cell_w <= 0 || c->m.cell_h <= 0) {
        return -1;
    }
    fdk_i32 grid_y = CAL_PAD + c->m.header_h + CAL_HEADER_GAP +
                     c->m.weekday_h;
    if (y < grid_y) {
        return -1;
    }
    fdk_i32 col = (x - CAL_PAD) / c->m.cell_w;
    fdk_i32 row = (y - grid_y) / c->m.cell_h;
    if (col < 0 || col > 6 || row < 0 || row >= CAL_WEEKS) {
        return -1;
    }
    fdk_i32 cell = row * 7 + col;
    fdk_date d;
    if (!cal_cell_date(c->year, c->month, cell, &d)) {
        return -1; /* outside the supported calendar */
    }
    return cell;
}

static void cal_fire_selected(fdk_calendar_impl *c) {
    if (c->on_selected != NULL) {
        c->on_selected(&c->base, &c->selected, c->on_selected_user);
    }
}

/* The one selection path: validates, follows the month, notifies. */
static void cal_apply_selection(fdk_widget *w, fdk_date d) {
    fdk_calendar_impl *c = cal_of(w);
    if (c->has_selection && c->selected.year == d.year &&
        c->selected.month == d.month && c->selected.day == d.day) {
        return; /* no-op: no callback */
    }
    c->has_selection = true;
    c->selected = d;
    c->year = d.year; /* the browse month follows */
    c->month = d.month;
    fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
    fdk__a11y_notify(w, FDK_A11Y_CHILDREN_CHANGED, 0);
    fdk_widget_invalidate(w);
    cal_fire_selected(c);
}

static void cal_browse(fdk_widget *w, fdk_i32 year, fdk_i32 month) {
    fdk_calendar_impl *c = cal_of(w);
    if (year < 1 || year > 9999 || month < 1 || month > 12) {
        return;
    }
    if (c->year == year && c->month == month) {
        return;
    }
    c->year = year;
    c->month = month;
    fdk__a11y_notify(w, FDK_A11Y_CHILDREN_CHANGED, 0);
    fdk_widget_invalidate(w);
}

/* Keyboard day walk with month scrolling: day delta in days. */
static void cal_step_selection(fdk_widget *w, fdk_i32 delta_days) {
    fdk_calendar_impl *c = cal_of(w);
    if (!c->has_selection) {
        /* Nothing selected: arrows land on the first of the browsed
         * month (a deterministic anchor, documented in the tests). */
        fdk_date d = {c->year, c->month, 1};
        cal_apply_selection(w, d);
        return;
    }
    fdk_i64 days = fdk_date_to_days(&c->selected);
    fdk_date d;
    if (!fdk_ok(fdk_date_from_days(days + delta_days, &d))) {
        return; /* the calendar-range edge */
    }
    cal_apply_selection(w, d);
}

static void cal_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                      fdk_rect clip) {
    (void)clip;
    fdk_calendar_impl *c = cal_of(w);
    if (bounds.width <= 0 || bounds.height <= 0 || c->font == NULL) {
        return;
    }
    if (!c->metrics_done) {
        cal_measure_metrics(c->font, &c->m);
        c->metrics_done = true;
    }
    const bool enabled = fdk_widget_is_effectively_enabled(w);

    /* The box: a field-shaped control surface. */
    fdk_i32 cr = fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect_aa(surface, bounds, cr,
                                     fdk__pal_control());
    fdk_surface_draw_rounded_rect_aa(surface, bounds, cr,
                                     fdk__pal_border());

    const fdk_i18n_names *names = cal_names(c->locale);

    /* Header: ‹ Month Year ›. */
    fdk_i32 head_y = bounds.y + CAL_PAD;
    fdk_i32 head_cy = head_y + c->m.header_h / 2;
    fdk_color nav_col =
        enabled ? fdk__pal_text() : fdk__pal_text_disabled();
    /* Left chevron (‹). */
    fdk_i32 lx = bounds.x + CAL_PAD + CAL_NAV_W / 2;
    fdk_surface_draw_line(surface, lx + 3, head_cy - 4, lx - 3, head_cy,
                          nav_col);
    fdk_surface_draw_line(surface, lx - 3, head_cy, lx + 3, head_cy + 4,
                          nav_col);
    /* Right chevron (›). */
    fdk_i32 rx = bounds.x + bounds.width - CAL_PAD - CAL_NAV_W / 2;
    fdk_surface_draw_line(surface, rx - 3, head_cy - 4, rx, head_cy,
                          nav_col);
    fdk_surface_draw_line(surface, rx, head_cy, rx - 3, head_cy + 4,
                          nav_col);
    /* Month + year, centered between the chevrons. */
    char head[64];
    int hn = snprintf(head, sizeof(head), "%s %d",
                      names->months_long[(c->month > 0 && c->month <= 12)
                                             ? c->month - 1
                                             : 0],
                      (int)c->year);
    if (hn > 0) {
        fdk_i32 hw = 0, hh = 0;
        fdk__text_extent(c->font, head, &hw, &hh);
        fdk_i32 tx = bounds.x + (bounds.width - hw) / 2;
        fdk__draw_text(surface, c->font, head, nav_col, tx,
                       fdk__center_baseline(c->font, head_y,
                                            c->m.header_h));
    }

    /* Weekday row: Monday-lead abbreviations, centered per column. */
    fdk_i32 wd_y = bounds.y + CAL_PAD + c->m.header_h + CAL_HEADER_GAP;
    for (int col = 0; col < 7; col++) {
        const char *wd = names->days_abbr[col]; /* index 0 = Monday */
        fdk_i32 ww = 0, wh = 0;
        fdk__text_extent(c->font, wd, &ww, &wh);
        fdk_i32 cell_x = bounds.x + CAL_PAD + col * c->m.cell_w;
        fdk__draw_text(surface, c->font, wd, fdk__pal_text_disabled(),
                       cell_x + (c->m.cell_w - ww) / 2,
                       fdk__center_baseline(c->font, wd_y, c->m.weekday_h));
    }

    /* The six week rows. */
    for (int cell = 0; cell < CAL_WEEKS * 7; cell++) {
        fdk_date d;
        if (!cal_cell_date(c->year, c->month, cell, &d)) {
            continue; /* beyond the supported calendar */
        }
        fdk_rect cr4 = cal_cell_rect(c, bounds, cell);
        bool in_month = cal_cell_in_month(c->year, c->month, cell);
        bool is_sel = c->has_selection &&
                      c->selected.year == d.year &&
                      c->selected.month == d.month &&
                      c->selected.day == d.day;
        bool is_today = c->has_today && c->today.year == d.year &&
                        c->today.month == d.month && c->today.day == d.day;

        /* Cell fills: selection > hover > plain. */
        if (is_sel && enabled) {
            fdk_surface_fill_rounded_rect_aa(surface, cr4, 4,
                                             fdk__pal_selection());
        } else if (cell == c->hover_cell && in_month && enabled) {
            fdk_surface_fill_rounded_rect_aa(surface, cr4, 4,
                                             fdk__pal_row_hover());
        }

        char num[8];
        int nn = snprintf(num, sizeof(num), "%d", (int)d.day);
        if (nn <= 0) {
            continue;
        }
        fdk_i32 nw = 0, nh = 0;
        fdk__text_extent(c->font, num, &nw, &nh);
        fdk_color tcol;
        if (is_sel && enabled) {
            tcol = fdk__pal_selection_text();
        } else if (!enabled) {
            tcol = fdk__pal_text_disabled();
        } else if (in_month) {
            tcol = fdk__pal_text();
        } else {
            tcol = fdk__pal_text_disabled();
        }
        fdk__draw_text(surface, c->font, num, tcol,
                       cr4.x + (cr4.width - nw) / 2,
                       fdk__center_baseline(c->font, cr4.y, cr4.height));

        /* Today marker: an accent underline inside the cell. */
        if (is_today && !is_sel) {
            fdk_rect bar = {cr4.x + cr4.width / 2 - 5,
                            cr4.y + cr4.height - 4, 10, 2};
            fdk_surface_fill_rect(surface, bar, fdk__pal_accent());
        }
    }

    /* Whole-widget focus ring (the button's discipline). */
    if ((w->flags & FDK_WF_FOCUSED) != 0 && enabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2, bounds.height - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 rr = cr > fw ? cr - fw : 0;
            fdk_surface_draw_rounded_rect_aa(surface, ring, rr,
                                             fdk__pal_focus_ring());
        }
    }
}

static void cal_measure(fdk_widget *w, fdk_size *out) {
    fdk_calendar_impl *c = cal_of(w);
    if (!c->metrics_done) {
        cal_measure_metrics(c->font, &c->m);
        c->metrics_done = true;
    }
    out->width = (c->m.total_w > 0) ? c->m.total_w : 0;
    out->height = (c->m.total_h > 0) ? c->m.total_h : 0;
}

static bool cal_event(fdk_widget *w, const fdk_widget_event *ev,
                      void *user) {
    (void)user;
    fdk_calendar_impl *c = cal_of(w);
    if (!c->metrics_done) {
        cal_measure_metrics(c->font, &c->m);
        c->metrics_done = true;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION: {
        fdk_i32 cell = cal_hit_cell(c, (fdk_i32)ev->position.x,
                                    (fdk_i32)ev->position.y);
        if (cell != c->hover_cell) {
            c->hover_cell = cell;
            fdk_widget_invalidate(w);
        }
        return false;
    }
    case FDK_WIDGET_POINTER_LEAVE:
        if (c->hover_cell != -1) {
            c->hover_cell = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        return true; /* the press arms the release (the grab pair) */
    case FDK_WIDGET_POINTER_UP: {
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        fdk_i32 x = (fdk_i32)ev->pointer.position.x;
        fdk_i32 y = (fdk_i32)ev->pointer.position.y;
        /* Header nav zones. */
        if (y >= CAL_PAD && y < CAL_PAD + c->m.header_h) {
            if (x >= CAL_PAD && x < CAL_PAD + CAL_NAV_W) {
                fdk_date d;
                if (cal_shift_month(c->year, c->month, 1, -1, &d)) {
                    cal_browse(w, d.year, d.month);
                }
                return true;
            }
            if (x >= (fdk_i32)w->bounds.width - CAL_PAD - CAL_NAV_W &&
                x < (fdk_i32)w->bounds.width - CAL_PAD) {
                fdk_date d;
                if (cal_shift_month(c->year, c->month, 1, 1, &d)) {
                    cal_browse(w, d.year, d.month);
                }
                return true;
            }
            return true;
        }
        fdk_i32 cell = cal_hit_cell(c, x, y);
        if (cell >= 0) {
            fdk_date d;
            if (cal_cell_date(c->year, c->month, cell, &d)) {
                cal_apply_selection(w, d);
            }
        }
        return true;
    }
    case FDK_WIDGET_KEY_DOWN: {
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        fdk_scancode k = ev->key.scancode;
        if (k == FDK_KEY_LEFT) {
            cal_step_selection(w, -1);
            return true;
        }
        if (k == FDK_KEY_RIGHT) {
            cal_step_selection(w, 1);
            return true;
        }
        if (k == FDK_KEY_UP) {
            cal_step_selection(w, -7);
            return true;
        }
        if (k == FDK_KEY_DOWN) {
            cal_step_selection(w, 7);
            return true;
        }
        if (k == FDK_KEY_PAGE_UP || k == FDK_KEY_PAGE_DOWN) {
            fdk_i32 delta = (k == FDK_KEY_PAGE_UP) ? -1 : 1;
            fdk_i32 day = c->has_selection ? c->selected.day : 1;
            fdk_i32 ysrc = c->has_selection ? c->selected.year : c->year;
            fdk_i32 msrc = c->has_selection ? c->selected.month : c->month;
            fdk_date d;
            if (cal_shift_month(ysrc, msrc, day, delta, &d)) {
                cal_apply_selection(w, d);
            }
            return true;
        }
        if (k == FDK_KEY_HOME) {
            if (c->has_today) {
                cal_apply_selection(w, c->today);
            } else {
                fdk_date d = {c->year, c->month, 1};
                cal_apply_selection(w, d);
            }
            return true;
        }
        if (k == FDK_KEY_END) {
            fdk_date d = {c->year, c->month,
                          fdk_date_days_in_month(c->year, c->month)};
            cal_apply_selection(w, d);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static void cal_destroy(fdk_widget *w) {
    fdk_calendar_impl *c = cal_of(w);
    fdk_free(c->locale);
}

/* ---- a11y: the calendar + its 42 painted day cells ---- */

static void cal_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_calendar_impl *c = (const fdk_calendar_impl *)(const void *)w;
    char buf[64];
    if (c->has_selection &&
        fdk_ok(fdk_format_date(buf, sizeof(buf), NULL, &c->selected,
                               FDK_DATE_ISO))) {
        out->value_text = fdk__strdup(buf);
        out->has_value = true;
        out->value_current =
            (double)fdk_date_to_days(&c->selected);
        out->value_min = (double)fdk_date_to_days(
        &(fdk_date){1, 1, 1});
        out->value_max = (double)fdk_date_to_days(
        &(fdk_date){9999, 12, 31});
    }
}

static size_t cal_a11y_virtual_count(const fdk_widget *w) {
    (void)w;
    return (size_t)(CAL_WEEKS * 7);
}

static void cal_a11y_virtual_describe(const fdk_widget *w, size_t index,
                                      fdk_a11y_info *out) {
    const fdk_calendar_impl *c =
        (const fdk_calendar_impl *)(const void *)w;
    if (index >= (size_t)(CAL_WEEKS * 7)) {
        return;
    }
    fdk_date d;
    if (!cal_cell_date(c->year, c->month, (fdk_i32)index, &d)) {
        return;
    }
    out->role = FDK_A11Y_ROLE_OPTION;
    char buf[64];
    if (fdk_ok(fdk_format_date(buf, sizeof(buf), NULL, &d,
                               FDK_DATE_ISO))) {
        out->name = fdk__strdup(buf);
    }
    if (c->has_selection && c->selected.year == d.year &&
        c->selected.month == d.month && c->selected.day == d.day) {
        out->states |= FDK_A11Y_SELECTED;
    }
    out->bounds = cal_cell_rect(c, fdk_widget_get_absolute_bounds(w),
        (fdk_i32)index);
}

static fdk_a11y_action_set cal_a11y_virtual_actions(
    const fdk_widget *w, size_t index) {
    (void)w;
    (void)index;
    return FDK_A11Y_ACTION_ACTIVATE;
}

static bool cal_a11y_virtual_perform(fdk_widget *w, size_t index,
                                     fdk_a11y_action action, double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_calendar_impl *c = cal_of(w);
    if (index >= (size_t)(CAL_WEEKS * 7)) {
        return false;
    }
    fdk_date d;
    if (!cal_cell_date(c->year, c->month, (fdk_i32)index, &d)) {
        return false;
    }
    cal_apply_selection(w, d);
    return true;
}

static const fdk_a11y_class cal_a11y = {
    .role = FDK_A11Y_ROLE_CALENDAR,
    .describe = cal_a11y_describe,
    .virtual_count = cal_a11y_virtual_count,
    .virtual_describe = cal_a11y_virtual_describe,
    .virtual_actions = cal_a11y_virtual_actions,
    .virtual_perform = cal_a11y_virtual_perform,
};

const fdk_widget_class fdk_calendar_class_def = {
    .size = sizeof(fdk_calendar_impl),
    .name = "calendar",
    .handle_event = NULL, /* the per-instance callback does it */
    .paint = cal_paint,
    .measure = cal_measure,
    .arrange = NULL,
    .destroy = cal_destroy,
    .a11y = &cal_a11y,
};

/* ---- fdk_calendar public API ---- */

fdk_result fdk_calendar_create(fdk_widget *parent, fdk_font *font,
                               fdk_widget **out_calendar) {
    if (out_calendar == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_calendar_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_calendar_impl *c = cal_of(w);
    c->font = font;
    c->locale = NULL;
    c->year = 1970;
    c->month = 1;
    c->has_selection = false;
    c->has_today = false;
    c->on_selected = NULL;
    c->on_selected_user = NULL;
    c->hover_cell = -1;
    c->metrics_done = false;
    memset(&c->m, 0, sizeof(c->m));
    fdk_widget_set_can_focus(w, true);
    fdk_widget_set_event_callback(w, cal_event, NULL);
    fdk_widget_child_layout_changed(w->parent);
    *out_calendar = w;
    return FDK_OK;
}

void fdk_calendar_set_locale(fdk_widget *calendar, const fdk_locale *locale) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def) {
        return;
    }
    fdk_calendar_impl *c = cal_of(calendar);
    if (locale == NULL) {
        if (c->locale == NULL) {
            return;
        }
        fdk_free(c->locale);
        c->locale = NULL;
    } else {
        if (c->locale == NULL) {
            c->locale = fdk_alloc(sizeof(*c->locale));
            if (c->locale == NULL) {
                return;
            }
        }
        *c->locale = *locale;
    }
    fdk_widget_invalidate(calendar);
}

fdk_result fdk_calendar_select(fdk_widget *calendar, const fdk_date *date) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def ||
        !cal_date_ok(date)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    cal_apply_selection(calendar, *date);
    return FDK_OK;
}

bool fdk_calendar_get_date(fdk_widget *calendar, fdk_date *out_date) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def ||
        out_date == NULL) {
        return false;
    }
    fdk_calendar_impl *c = cal_of(calendar);
    if (!c->has_selection) {
        return false;
    }
    *out_date = c->selected;
    return true;
}

fdk_result fdk_calendar_set_month(fdk_widget *calendar, fdk_i32 year,
                                  fdk_i32 month) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (year < 1 || year > 9999 || month < 1 || month > 12) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    cal_browse(calendar, year, month);
    return FDK_OK;
}

void fdk_calendar_get_month(fdk_widget *calendar, fdk_i32 *out_year,
                            fdk_i32 *out_month) {
    if (out_year != NULL) {
        *out_year = 0;
    }
    if (out_month != NULL) {
        *out_month = 0;
    }
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def) {
        return;
    }
    fdk_calendar_impl *c = cal_of(calendar);
    if (out_year != NULL) {
        *out_year = c->year;
    }
    if (out_month != NULL) {
        *out_month = c->month;
    }
}

void fdk_calendar_set_today(fdk_widget *calendar, const fdk_date *today) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def) {
        return;
    }
    fdk_calendar_impl *c = cal_of(calendar);
    if (today == NULL) {
        if (c->has_today) {
            c->has_today = false;
            fdk_widget_invalidate(calendar);
        }
        return;
    }
    if (!cal_date_ok(today)) {
        return; /* invalid: ignored, like theme setters */
    }
    if (c->has_today && c->today.year == today->year &&
        c->today.month == today->month && c->today.day == today->day) {
        return;
    }
    c->has_today = true;
    c->today = *today;
    fdk_widget_invalidate(calendar);
}

void fdk_calendar_set_on_selected(fdk_widget *calendar,
                                  fdk_calendar_selected_fn on_selected,
                                  void *user) {
    if (calendar == NULL || calendar->klass != &fdk_calendar_class_def) {
        return;
    }
    fdk_calendar_impl *c = cal_of(calendar);
    c->on_selected = on_selected;
    c->on_selected_user = user;
}

/* =====================================================================
 * fdk_date_picker — the formatted field + the popup calendar
 * ===================================================================== */

typedef struct fdk_date_picker_impl {
    fdk_widget base;
    fdk_font *font;       /* borrowed */
    fdk_locale *locale;   /* owned copy; NULL = root       */
    fdk_date_style style; /* field rendering               */
    bool has_date;
    fdk_date date;
    fdk_date_picker_changed_fn on_changed;
    void *on_changed_user;
    bool popup_open;
    struct date_guard *guard; /* shared with the popup ctx   */
    bool hovering;
    bool pressed;
} fdk_date_picker_impl;

static fdk_date_picker_impl *dp_of(fdk_widget *w) {
    return (fdk_date_picker_impl *)(void *)w;
}

extern const fdk_widget_class fdk_date_picker_class_def;

/* The guard: NULLs its picker when the picker is destroyed while the
 * popup is open (the combo_guard discipline — the popup outlives the
 * field until dismissal, and every callback re-checks). */
typedef struct date_guard {
    fdk_widget *picker; /* NULL once the picker is destroyed */
} date_guard;

typedef struct date_popup_ctx {
    date_guard *guard;  /* freed here on every end path     */
    fdk_window *popup;  /* NULL once dismissed/destroyed    */
    fdk_widget *calendar;
} date_popup_ctx;

static void dp_fire_changed(fdk_date_picker_impl *p) {
    if (p->on_changed != NULL) {
        p->on_changed(&p->base, &p->date, p->on_changed_user);
    }
}

static void dp_popup_destroyed(fdk_window *window, void *user);
static void dp_popup_event(fdk_window *window, const fdk_event_data *ev,
                           void *user);

/* The one close path: called for selection-apply, CLOSE_REQUEST, and
 * Esc. Destroys the popup window (the destroy-notify then runs the
 * shared unwind below) — idempotent through ctx->popup NULLing. */
static void dp_popup_close(date_popup_ctx *ctx) {
    if (ctx->popup != NULL) {
        fdk_window *pop = ctx->popup;
        ctx->popup = NULL;
        fdk_window_destroy(pop); /* fires dp_popup_destroyed */
    }
}

static void dp_popup_unwind(date_popup_ctx *ctx) {
    fdk_widget *w = ctx->guard->picker;
    if (w != NULL && w->klass == &fdk_date_picker_class_def) {
        fdk_date_picker_impl *p = dp_of(w);
        p->popup_open = false;
        p->guard = NULL;
        fdk_widget_invalidate(w);
    }
    fdk_free(ctx->guard);
    fdk_free(ctx);
}

static void dp_popup_destroyed(fdk_window *window, void *user) {
    (void)window;
    date_popup_ctx *ctx = user;
    if (ctx->popup == window) {
        ctx->popup = NULL;
    }
    dp_popup_unwind(ctx);
}

static void dp_popup_event(fdk_window *window, const fdk_event_data *ev,
                           void *user) {
    (void)window;
    date_popup_ctx *ctx = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        dp_popup_close(ctx);
        return;
    }
    /* Esc dismisses unchanged (the dialog's window-level rule: the
     * calendar's own keys are consumed before this runs). */
    if (ev->type == FDK_EVENT_KEY_DOWN &&
        ev->key.scancode == FDK_KEY_ESC) {
        dp_popup_close(ctx);
    }
}

/* The calendar's on_selected: applies to the field (if it still
 * lives) and closes. */
static void dp_calendar_selected(fdk_widget *calendar, const fdk_date *date,
                                 void *user) {
    (void)calendar;
    date_popup_ctx *ctx = user;
    fdk_widget *w = ctx->guard->picker;
    if (w == NULL || w->klass != &fdk_date_picker_class_def) {
        return; /* the picker died mid-flight: unwind only */
    }
    fdk_date_picker_impl *p = dp_of(w);
    if (p->guard != ctx->guard) {
        return; /* a stale popup from a previous open */
    }
    if (!p->has_date || p->date.year != date->year ||
        p->date.month != date->month || p->date.day != date->day) {
        p->has_date = true;
        p->date = *date;
        fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
        fdk_widget_invalidate(w);
        dp_fire_changed(p);
    }
    dp_popup_close(ctx);
}

static void dp_open_popup(fdk_widget *w) {
    fdk_date_picker_impl *p = dp_of(w);
    if (p->popup_open || p->font == NULL) {
        return;
    }
    fdk_window *win = fdk__window_of_owner(fdk__widget_window_owner(w));
    if (win == NULL) {
        return; /* standalone tree: no window to anchor to */
    }
    /* The popup's size: the calendar's natural size. */
    fdk_widget *probe = NULL;
    if (!fdk_ok(fdk_calendar_create(NULL, p->font, &probe)) ||
        probe == NULL) {
        return;
    }
    fdk_size natural = {0, 0};
    fdk_widget_measure(probe, &natural);
    fdk_i32 w_px = natural.width;
    fdk_i32 h_px = natural.height;
    fdk_widget_destroy(probe);
    if (w_px <= 0 || h_px <= 0) {
        return;
    }

    date_popup_ctx *ctx = fdk_alloc(sizeof(*ctx));
    date_guard *guard = fdk_alloc(sizeof(*guard));
    if (ctx == NULL || guard == NULL) {
        fdk_free(ctx);
        fdk_free(guard);
        return;
    }
    guard->picker = w;
    ctx->guard = guard;
    ctx->popup = NULL;
    ctx->calendar = NULL;

    /* Anchor: the field's bottom-left, clamped inside the owner. */
    fdk_rect abs = fdk_widget_get_absolute_bounds(w);
    fdk_size owner_size = {0, 0};
    (void)fdk_window_get_size(win, &owner_size);
    fdk_i32 x = abs.x;
    fdk_i32 y = abs.y + abs.height;
    if (x + w_px > (fdk_i32)owner_size.width) {
        x = (fdk_i32)owner_size.width - w_px;
    }
    if (x < 0) {
        x = 0;
    }
    if (y + h_px > (fdk_i32)owner_size.height) {
        y = abs.y - h_px; /* flip above the field */
        if (y < 0) {
            y = 0;
        }
    }

    fdk_window *pop = NULL;
    fdk_result r = fdk_window_create_popup(win->ctx, win, x, y, w_px, h_px,
                                           &pop);
    if (!fdk_ok(r)) {
        fdk_free(guard);
        fdk_free(ctx);
        return;
    }
    ctx->popup = pop;
    fdk__window_set_auto_paint(pop, true);
    fdk__window_set_destroy_notify(pop, dp_popup_destroyed, ctx);
    fdk_window_set_event_callback(pop, dp_popup_event, ctx);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(pop, &root);
    fdk_widget *cal = NULL;
    if (fdk_ok(r)) {
        r = fdk_calendar_create(root, p->font, &cal);
    }
    if (!fdk_ok(r) || cal == NULL) {
        dp_popup_close(ctx);
        return;
    }
    ctx->calendar = cal;
    fdk_widget_set_bounds(cal, (fdk_rect){0, 0, w_px, h_px});
    fdk_calendar_set_locale(cal, p->locale);
    fdk_calendar_set_on_selected(cal, dp_calendar_selected, ctx);
    if (p->has_date) {
        (void)fdk_calendar_select(cal, &p->date);
    } else {
        (void)fdk_calendar_set_month(cal, 1970, 1);
    }

    p->popup_open = true;
    p->guard = guard;
    fdk__a11y_notify(w, FDK_A11Y_STATE_CHANGED, FDK_A11Y_EXPANDED);
    fdk_window_show(pop);
    fdk_widget_focus(cal);
}

static void dp_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                     fdk_rect clip) {
    (void)clip;
    fdk_date_picker_impl *p = dp_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    const bool enabled = fdk_widget_is_effectively_enabled(w);
    fdk_color fill = fdk__pal_entry();
    if (p->hovering && enabled) {
        fill = fdk__pal_control_hover();
    }
    if (p->pressed && enabled) {
        fill = fdk__pal_control_pressed();
    }
    if (!enabled) {
        fill = fdk__pal_control_disabled();
    }
    fdk_i32 cr = fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect_aa(surface, bounds, cr, fill);
    fdk_surface_draw_rounded_rect_aa(surface, bounds, cr,
                                     fdk__pal_entry_border());

    /* The chevron zone. */
    fdk_color col = enabled ? fdk__pal_text() : fdk__pal_text_disabled();
    fdk_i32 cx = bounds.x + bounds.width - PICK_ZONE_W / 2;
    fdk_i32 cy = bounds.y + bounds.height / 2;
    fdk_surface_draw_line(surface, cx - 4, cy - 2, cx, cy + 2, col);
    fdk_surface_draw_line(surface, cx, cy + 2, cx + 4, cy - 2, col);

    /* The formatted date (an em dash while dateless). */
    const char *text = "—";
    char buf[64];
    if (p->has_date &&
        fdk_ok(fdk_format_date(buf, sizeof(buf),
                               p->locale, &p->date, p->style))) {
        text = buf;
    }
    if (p->font != NULL && text[0] != '\0') {
        fdk_i32 baseline = fdk__center_baseline(p->font, bounds.y,
                                                 bounds.height);
        fdk__draw_text(surface, p->font, text, col,
                       bounds.x + PICK_FIELD_PAD_X, baseline);
    }

    if ((w->flags & FDK_WF_FOCUSED) != 0 && enabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2, bounds.height - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 rr = cr > fw ? cr - fw : 0;
            fdk_surface_draw_rounded_rect_aa(surface, ring, rr,
                                             fdk__pal_focus_ring());
        }
    }
}

static void dp_measure(fdk_widget *w, fdk_size *out) {
    fdk_date_picker_impl *p = dp_of(w);
    out->width = 0;
    out->height = 0;
    if (p->font == NULL) {
        return;
    }
    /* Widest realistic rendering: the LONG style of a date near the
     * range's wide end (a stable natural width that never jitters
     * as the date changes; the field is usually stretch-laid-out). */
    char buf[64];
    fdk_date probe = {9999, 12, 31};
    fdk_i32 tw = 0, th = 0;
    if (fdk_ok(fdk_format_date(buf, sizeof(buf), p->locale, &probe,
                               FDK_DATE_FULL))) {
        fdk__text_extent(p->font, buf, &tw, &th);
    } else {
        fdk__text_extent(p->font, "—", &tw, &th);
    }
    out->width = tw + PICK_FIELD_PAD_X * 2 + PICK_ZONE_W;
    out->height = th + 12;
    (void)w;
}

static bool dp_event(fdk_widget *w, const fdk_widget_event *ev,
                     void *user) {
    (void)user;
    fdk_date_picker_impl *p = dp_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION:
        if (!p->hovering) {
            p->hovering = true;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        if (p->hovering) {
            p->hovering = false;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        p->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (p->pressed && !p->popup_open) {
            dp_open_popup(w);
        }
        p->pressed = false;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_KEY_DOWN:
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        if (ev->key.scancode == FDK_KEY_ENTER ||
            ev->key.scancode == FDK_KEY_SPACE ||
            ev->key.scancode == FDK_KEY_DOWN ||
            ev->key.scancode == FDK_KEY_UP) {
            if (!p->popup_open) {
                dp_open_popup(w);
            }
            return true;
        }
        return false;
    default:
        return false;
    }
}

static void dp_destroy(fdk_widget *w) {
    fdk_date_picker_impl *p = dp_of(w);
    if (p->guard != NULL) {
        p->guard->picker = NULL; /* the popup unwinds alone */
        p->guard = NULL;
    }
    fdk_free(p->locale);
}

/* ---- a11y ---- */

static void dp_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_date_picker_impl *p =
        (const fdk_date_picker_impl *)(const void *)w;
    if (!p->has_date) {
        return;
    }
    char buf[64];
    if (fdk_ok(fdk_format_date(buf, sizeof(buf), p->locale, &p->date,
                               FDK_DATE_ISO))) {
        out->value_text = fdk__strdup(buf);
        out->has_value = true;
        out->value_current = (double)fdk_date_to_days(&p->date);
        out->value_min = (double)fdk_date_to_days(&(fdk_date){1, 1, 1});
        out->value_max = (double)fdk_date_to_days(
        &(fdk_date){9999, 12, 31});
    }
}

static fdk_a11y_action_set dp_a11y_actions(const fdk_widget *w) {
    (void)w;
    return FDK_A11Y_ACTION_ACTIVATE;
}

static bool dp_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                            double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    fdk_date_picker_impl *p = dp_of(w);
    if (!p->popup_open) {
        dp_open_popup(w);
    }
    return true;
}

static const fdk_a11y_class dp_a11y = {
    .role = FDK_A11Y_ROLE_DATE_EDITOR,
    .describe = dp_a11y_describe,
    .actions = dp_a11y_actions,
    .perform = dp_a11y_perform,
};

const fdk_widget_class fdk_date_picker_class_def = {
    .size = sizeof(fdk_date_picker_impl),
    .name = "date_picker",
    .handle_event = NULL,
    .paint = dp_paint,
    .measure = dp_measure,
    .arrange = NULL,
    .destroy = dp_destroy,
    .a11y = &dp_a11y,
};

/* ---- fdk_date_picker public API ---- */

fdk_result fdk_date_picker_create(fdk_widget *parent, fdk_font *font,
                                  const fdk_locale *locale,
                                  fdk_widget **out_picker) {
    if (out_picker == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_date_picker_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_date_picker_impl *p = dp_of(w);
    p->font = font;
    p->locale = NULL;
    p->style = FDK_DATE_MEDIUM;
    p->has_date = false;
    p->on_changed = NULL;
    p->on_changed_user = NULL;
    p->popup_open = false;
    p->guard = NULL;
    p->hovering = false;
    p->pressed = false;
    if (locale != NULL) {
        p->locale = fdk_alloc(sizeof(*p->locale));
        if (p->locale != NULL) {
            *p->locale = *locale;
        }
    }
    fdk_widget_set_can_focus(w, true);
    fdk_widget_set_event_callback(w, dp_event, NULL);
    fdk_widget_child_layout_changed(w->parent);
    *out_picker = w;
    return FDK_OK;
}

void fdk_date_picker_set_locale(fdk_widget *picker,
                                const fdk_locale *locale) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def) {
        return;
    }
    fdk_date_picker_impl *p = dp_of(picker);
    if (locale == NULL) {
        if (p->locale == NULL) {
            return;
        }
        fdk_free(p->locale);
        p->locale = NULL;
    } else {
        if (p->locale == NULL) {
            p->locale = fdk_alloc(sizeof(*p->locale));
            if (p->locale == NULL) {
                return;
            }
        }
        *p->locale = *locale;
    }
    fdk_widget_invalidate(picker);
}

fdk_result fdk_date_picker_set_date(fdk_widget *picker,
                                    const fdk_date *date) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def ||
        !cal_date_ok(date)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_date_picker_impl *p = dp_of(picker);
    if (!p->has_date || p->date.year != date->year ||
        p->date.month != date->month || p->date.day != date->day) {
        p->has_date = true;
        p->date = *date;
        fdk__a11y_notify(picker, FDK_A11Y_VALUE_CHANGED, 0);
        fdk_widget_invalidate(picker);
        dp_fire_changed(p);
    }
    return FDK_OK;
}

bool fdk_date_picker_get_date(fdk_widget *picker, fdk_date *out_date) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def ||
        out_date == NULL) {
        return false;
    }
    fdk_date_picker_impl *p = dp_of(picker);
    if (!p->has_date) {
        return false;
    }
    *out_date = p->date;
    return true;
}

void fdk_date_picker_set_style(fdk_widget *picker, fdk_date_style style) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def ||
        style < FDK_DATE_SHORT || style > FDK_DATE_ISO) {
        return;
    }
    fdk_date_picker_impl *p = dp_of(picker);
    if (p->style != style) {
        p->style = style;
        fdk_widget_invalidate(picker);
    }
}

void fdk_date_picker_set_on_changed(fdk_widget *picker,
                                    fdk_date_picker_changed_fn on_changed,
                                    void *user) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def) {
        return;
    }
    fdk_date_picker_impl *p = dp_of(picker);
    p->on_changed = on_changed;
    p->on_changed_user = user;
}

bool fdk_date_picker_popup_open(fdk_widget *picker) {
    if (picker == NULL || picker->klass != &fdk_date_picker_class_def) {
        return false;
    }
    return dp_of(picker)->popup_open;
}

/* =====================================================================
 * fdk_time_picker — the segmented stepper
 * ===================================================================== */

typedef enum {
    TSEG_HOUR = 0,
    TSEG_MINUTE = 1,
    TSEG_SECOND = 2,
    TSEG_DAYPART = 3, /* AM/PM (12h only) */
    TSEG_COUNT = 4,
} tseg;

typedef struct fdk_time_picker_impl {
    fdk_widget base;
    fdk_font *font;       /* borrowed */
    fdk_locale *locale;   /* owned copy; NULL = root       */
    fdk_time_style style; /* SHORT/MEDIUM follow hour12    */
    fdk_time time;
    tseg active;          /* the segment the stepper drives */
    fdk_time_picker_changed_fn on_changed;
    void *on_changed_user;
    bool hovering;
    bool pressed; /* inside a stepper zone             */
    bool metrics_done;
    /* Cached segment geometry (widget-local). */
    fdk_i32 seg_x[TSEG_COUNT];
    fdk_i32 seg_w[TSEG_COUNT];
    fdk_i32 nsegs;        /* how many segments exist now   */
    bool has_daypart;
    bool hour12;          /* the resolved cycle            */
    fdk_i32 field_w;
    fdk_i32 field_h;
} fdk_time_picker_impl;

static fdk_time_picker_impl *tp_of(fdk_widget *w) {
    return (fdk_time_picker_impl *)(void *)w;
}

extern const fdk_widget_class fdk_time_picker_class_def;

static bool tp_use12(const fdk_time_picker_impl *t) {
    switch (t->style) {
    case FDK_TIME_24H:
        return false;
    case FDK_TIME_12H:
        return true;
    default:
        break;
    }
    const fdk_locale *l =
        (t->locale != NULL) ? t->locale : fdk_locale_root();
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    return rules->hour12;
}

static bool tp_has_seconds(const fdk_time_picker_impl *t) {
    return t->style == FDK_TIME_MEDIUM;
}

/* (Re)builds the cached segment geometry from the font + the
 * current segment set. Called lazily by paint/measure/hit. */
static void tp_layout(fdk_time_picker_impl *t) {
    memset(t->seg_x, 0, sizeof(t->seg_x));
    memset(t->seg_w, 0, sizeof(t->seg_w));
    t->hour12 = tp_use12(t);
    t->has_daypart = t->hour12;
    bool secs = tp_has_seconds(t);
    t->nsegs = secs ? 3 : 2;
    if (t->has_daypart) {
        t->nsegs += 1;
    }
    if (t->font == NULL) {
        t->field_w = 0;
        t->field_h = 0;
        return;
    }
    fdk_i32 dw = 0, dh = 0, cw = 0, pw = 0, sw = 0;
    fdk__text_extent(t->font, "00", &dw, &dh);
    fdk__text_extent(t->font, ":", &cw, &sw);
    fdk__text_extent(t->font, "PM", &pw, &sw);
    (void)sw;
    fdk_i32 pad = 6;
    fdk_i32 x = PICK_FIELD_PAD_X;
    for (int i = 0; i < TSEG_COUNT; i++) {
        tseg seg = (tseg)i;
        bool exists = (seg == TSEG_HOUR) || (seg == TSEG_MINUTE) ||
                      (seg == TSEG_SECOND && secs) ||
                      (seg == TSEG_DAYPART && t->has_daypart);
        if (!exists) {
            continue;
        }
        fdk_i32 w = (seg == TSEG_DAYPART) ? pw + pad * 2 : dw + pad * 2;
        t->seg_x[seg] = x;
        t->seg_w[seg] = w;
        x += w;
        if (seg != TSEG_DAYPART) {
            x += cw + 4; /* the separator after numeric segments */
        } else {
            x += 6;
        }
    }
    /* The trailing separator after the last numeric segment is
     * replaced by the daypart's leading gap (or dropped when no
     * daypart follows). */
    if (!t->has_daypart) {
        x -= cw + 4; /* no separator after the last segment */
    } else {
        x += 2; /* a small gap before AM/PM */
    }
    t->field_w = x + PICK_FIELD_PAD_X + PICK_ZONE_W;
    t->field_h = dh + 12;
}

static void tp_fire_changed(fdk_time_picker_impl *t) {
    if (t->on_changed != NULL) {
        t->on_changed(&t->base, &t->time, t->on_changed_user);
    }
}

/* Renders one numeric segment (latn digits — the formatters'
 * current date/time output; see the module note). */
static void tp_num(char *buf, size_t cap, int value) {
    snprintf(buf, cap, "%02d", value);
}

/* Steps the active segment. delta may be any magnitude (PageUp/Down
 * pass 10/6); wraparound is per-segment. */
static void tp_step(fdk_widget *w, fdk_i32 delta) {
    fdk_time_picker_impl *t = tp_of(w);
    fdk_time old = t->time;
    fdk_time nt = t->time;
    tseg seg = t->active;
    if (seg == TSEG_HOUR) {
        if (t->hour12) {
            /* 12h stepping keeps the daypart and walks 1..12: the
             * absolute hour is daypart + (h12 % 12). */
            int abs = t->time.hour;
            int daypart = (abs >= 12) ? 12 : 0;
            int h12 = (abs % 12 == 0) ? 12 : abs % 12;
            int nh12 = ((h12 - 1 + delta) % 12 + 12) % 12 + 1;
            nt.hour = daypart + (nh12 % 12);
        } else {
            nt.hour = (int)(((fdk_i64)t->time.hour + delta) % 24 + 24) % 24;
        }
    } else if (seg == TSEG_MINUTE || seg == TSEG_SECOND) {
        int v = (seg == TSEG_MINUTE) ? t->time.minute : t->time.second;
        int nv = (int)(((fdk_i64)v + delta) % 60 + 60) % 60;
        if (seg == TSEG_MINUTE) {
            nt.minute = nv;
        } else {
            nt.second = nv;
        }
    } else { /* TSEG_DAYPART: any nonzero delta toggles */
        nt.hour = (t->time.hour + 12) % 24;
    }
    if (nt.hour != old.hour || nt.minute != old.minute ||
        nt.second != old.second) {
        t->time = nt;
        fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
        fdk_widget_invalidate(w);
        tp_fire_changed(t);
    }
}

/* Segment order for Left/Right walking: hour, minute, [second],
 * [daypart]. */
static tseg tp_next_seg(const fdk_time_picker_impl *t, tseg cur,
                        int dir) {
    tseg order[TSEG_COUNT];
    int n = 0;
    order[n++] = TSEG_HOUR;
    order[n++] = TSEG_MINUTE;
    if (t->style == FDK_TIME_MEDIUM) {
        order[n++] = TSEG_SECOND;
    }
    if (t->has_daypart) {
        order[n++] = TSEG_DAYPART;
    }
    int at = -1;
    for (int i = 0; i < n; i++) {
        if (order[i] == cur) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        at = 0;
    }
    int next = (at + dir + n) % n;
    return order[next];
}

static void tp_paint(fdk_widget *w, fdk_surface *surface, fdk_rect bounds,
                     fdk_rect clip) {
    (void)clip;
    fdk_time_picker_impl *t = tp_of(w);
    if (bounds.width <= 0 || bounds.height <= 0 || t->font == NULL) {
        return;
    }
    if (!t->metrics_done) {
        tp_layout(t);
        t->metrics_done = true;
    }
    const bool enabled = fdk_widget_is_effectively_enabled(w);
    fdk_color fill = fdk__pal_entry();
    if (t->hovering && enabled) {
        fill = fdk__pal_control_hover();
    }
    if (t->pressed && enabled) {
        fill = fdk__pal_control_pressed();
    }
    if (!enabled) {
        fill = fdk__pal_control_disabled();
    }
    fdk_i32 cr = fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect_aa(surface, bounds, cr, fill);
    fdk_surface_draw_rounded_rect_aa(surface, bounds, cr,
                                     fdk__pal_entry_border());

    fdk_color col = enabled ? fdk__pal_text() : fdk__pal_text_disabled();
    const fdk_i18n_names *names = cal_names(t->locale);
    fdk_i32 baseline = fdk__center_baseline(t->font, bounds.y,
                                             bounds.height);

    /* The segments. */
    for (int i = 0; i < TSEG_COUNT; i++) {
        tseg seg = (tseg)i;
        if (t->seg_w[seg] <= 0) {
            continue;
        }
        fdk_i32 x = bounds.x + t->seg_x[seg];
        fdk_i32 wd = t->seg_w[seg];
        char buf[8];
        const char *text = buf;
        if (seg == TSEG_DAYPART) {
            text = (t->time.hour < 12) ? names->am : names->pm;
        } else if (seg == TSEG_HOUR) {
            int h = t->time.hour;
            if (t->hour12) {
                h = (h % 12 == 0) ? 12 : h % 12;
            }
            tp_num(buf, sizeof(buf), h);
        } else if (seg == TSEG_MINUTE) {
            tp_num(buf, sizeof(buf), t->time.minute);
        } else {
            tp_num(buf, sizeof(buf), t->time.second);
        }
        /* Active segment: the selection colors. */
        if (seg == t->active && enabled) {
            fdk_rect sel = {x, bounds.y + 2, wd, bounds.height - 4};
            fdk_surface_fill_rounded_rect_aa(surface, sel, 4,
                                             fdk__pal_selection());
        }
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(t->font, text, &tw, &th);
        fdk_color tc = col;
        if (seg == t->active && enabled) {
            tc = fdk__pal_selection_text();
        }
        fdk__draw_text(surface, t->font, text, tc,
                       x + (wd - tw) / 2, baseline);
        /* Separator after this numeric segment when another numeric
         * segment follows (hour:minute, minute:second); the daypart
         * gets a plain gap, the last segment nothing. */
        bool numeric = (seg != TSEG_DAYPART);
        bool numeric_follows =
            (seg == TSEG_HOUR) ||
            (seg == TSEG_MINUTE && t->style == FDK_TIME_MEDIUM);
        if (numeric && numeric_follows) {
            fdk_i32 cw = 0, chh = 0;
            fdk__text_extent(t->font, ":", &cw, &chh);
            (void)chh;
            fdk__draw_text(surface, t->font, ":", col, x + wd + 2,
                           baseline);
        }
    }

    /* The stepper zone: ▲ over ▼, split at the vertical midpoint. */
    fdk_i32 zx = bounds.x + bounds.width - PICK_ZONE_W;
    fdk_color zcol = enabled ? fdk__pal_text() : fdk__pal_text_disabled();
    fdk_i32 zcx = zx + PICK_ZONE_W / 2;
    fdk_i32 mid = bounds.y + bounds.height / 2;
    /* ▲ */
    fdk_surface_draw_line(surface, zcx - 4, mid - 8, zcx, mid - 12, zcol);
    fdk_surface_draw_line(surface, zcx, mid - 12, zcx + 4, mid - 8, zcol);
    /* ▼ */
    fdk_surface_draw_line(surface, zcx - 4, mid + 8, zcx, mid + 12, zcol);
    fdk_surface_draw_line(surface, zcx, mid + 12, zcx + 4, mid + 8, zcol);

    if ((w->flags & FDK_WF_FOCUSED) != 0 && enabled) {
        fdk_i32 fw = fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH);
        fdk_rect ring = {bounds.x + fw, bounds.y + fw,
                         bounds.width - fw * 2, bounds.height - fw * 2};
        if (ring.width > 0 && ring.height > 0) {
            fdk_i32 rr = cr > fw ? cr - fw : 0;
            fdk_surface_draw_rounded_rect_aa(surface, ring, rr,
                                             fdk__pal_focus_ring());
        }
    }
}

static void tp_measure(fdk_widget *w, fdk_size *out) {
    fdk_time_picker_impl *t = tp_of(w);
    if (!t->metrics_done) {
        tp_layout(t);
        t->metrics_done = true;
    }
    out->width = t->field_w;
    out->height = t->field_h;
    (void)w;
}

/* Which segment/stepper a widget-local point is over. */
static tseg tp_hit_seg(fdk_time_picker_impl *t, fdk_i32 x) {
    for (int i = 0; i < TSEG_COUNT; i++) {
        tseg seg = (tseg)i;
        if (t->seg_w[seg] > 0 && x >= t->seg_x[seg] &&
            x < t->seg_x[seg] + t->seg_w[seg]) {
            return seg;
        }
    }
    return TSEG_HOUR;
}

static bool tp_event(fdk_widget *w, const fdk_widget_event *ev,
                     void *user) {
    (void)user;
    fdk_time_picker_impl *t = tp_of(w);
    if (!t->metrics_done) {
        tp_layout(t);
        t->metrics_done = true;
    }
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION:
        if (!t->hovering) {
            t->hovering = true;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        if (t->hovering) {
            t->hovering = false;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_DOWN: {
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        fdk_i32 x = (fdk_i32)ev->pointer.position.x;
        fdk_i32 zx = (fdk_i32)w->bounds.width - PICK_ZONE_W;
        if (x >= zx) {
            /* Stepper: upper +1, lower -1; the press arms it. */
            t->pressed = true;
            fdk_widget_invalidate(w);
            return true;
        }
        /* Segment select. */
        tseg seg = tp_hit_seg(t, x);
        if (seg != t->active) {
            t->active = seg;
            fdk_widget_invalidate(w);
        }
        return true;
    }
    case FDK_WIDGET_POINTER_UP: {
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        fdk_i32 x = (fdk_i32)ev->pointer.position.x;
        fdk_i32 y = (fdk_i32)ev->pointer.position.y;
        fdk_i32 zx = (fdk_i32)w->bounds.width - PICK_ZONE_W;
        if (t->pressed && x >= zx) {
            fdk_i32 mid = (fdk_i32)w->bounds.height / 2;
            tp_step(w, (y < mid) ? 1 : -1);
        }
        t->pressed = false;
        fdk_widget_invalidate(w);
        return true;
    }
    case FDK_WIDGET_KEY_DOWN: {
        if (!fdk_widget_is_effectively_enabled(w)) {
            return true;
        }
        fdk_scancode k = ev->key.scancode;
        if (k == FDK_KEY_LEFT || k == FDK_KEY_RIGHT) {
            tseg next = tp_next_seg(t, t->active,
                                    (k == FDK_KEY_RIGHT) ? 1 : -1);
            if (next != t->active) {
                t->active = next;
                fdk_widget_invalidate(w);
            }
            return true;
        }
        if (k == FDK_KEY_UP) {
            tp_step(w, 1);
            return true;
        }
        if (k == FDK_KEY_DOWN) {
            tp_step(w, -1);
            return true;
        }
        if (k == FDK_KEY_PAGE_UP || k == FDK_KEY_PAGE_DOWN) {
            fdk_i32 mag = (t->active == TSEG_MINUTE ||
                           t->active == TSEG_SECOND)
                              ? 10
                              : (t->active == TSEG_HOUR ? 6 : 1);
            tp_step(w, (k == FDK_KEY_PAGE_UP) ? mag : -mag);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

static void tp_destroy(fdk_widget *w) {
    fdk_time_picker_impl *t = tp_of(w);
    fdk_free(t->locale);
}

/* ---- a11y ---- */

static void tp_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_time_picker_impl *t =
        (const fdk_time_picker_impl *)(const void *)w;
    char buf[64];
    if (fdk_ok(fdk_format_time(buf, sizeof(buf), t->locale, &t->time,
                               FDK_TIME_24H))) {
        out->value_text = fdk__strdup(buf);
        out->has_value = true;
        out->value_current = (double)(t->time.hour * 3600 +
                                      t->time.minute * 60 +
                                      t->time.second);
        out->value_min = 0.0;
        out->value_max = 86399.0;
    }
}

static const fdk_a11y_class tp_a11y = {
    .role = FDK_A11Y_ROLE_DATE_EDITOR,
    .describe = tp_a11y_describe,
};

const fdk_widget_class fdk_time_picker_class_def = {
    .size = sizeof(fdk_time_picker_impl),
    .name = "time_picker",
    .handle_event = NULL,
    .paint = tp_paint,
    .measure = tp_measure,
    .arrange = NULL,
    .destroy = tp_destroy,
    .a11y = &tp_a11y,
};

/* ---- fdk_time_picker public API ---- */

fdk_result fdk_time_picker_create(fdk_widget *parent, fdk_font *font,
                                  const fdk_locale *locale,
                                  fdk_widget **out_picker) {
    if (out_picker == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_time_picker_class_def,
                                     (fdk_rect){0, 0, 0, 0}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_time_picker_impl *t = tp_of(w);
    t->font = font;
    t->locale = NULL;
    t->style = FDK_TIME_SHORT;
    t->time = (fdk_time){0, 0, 0};
    t->active = TSEG_HOUR;
    t->on_changed = NULL;
    t->on_changed_user = NULL;
    t->hovering = false;
    t->pressed = false;
    t->metrics_done = false;
    t->nsegs = 2;
    t->has_daypart = false;
    t->hour12 = false;
    t->field_w = 0;
    t->field_h = 0;
    if (locale != NULL) {
        t->locale = fdk_alloc(sizeof(*t->locale));
        if (t->locale != NULL) {
            *t->locale = *locale;
        }
    }
    fdk_widget_set_can_focus(w, true);
    fdk_widget_set_event_callback(w, tp_event, NULL);
    fdk_widget_child_layout_changed(w->parent);
    *out_picker = w;
    return FDK_OK;
}

void fdk_time_picker_set_locale(fdk_widget *picker,
                                const fdk_locale *locale) {
    if (picker == NULL || picker->klass != &fdk_time_picker_class_def) {
        return;
    }
    fdk_time_picker_impl *t = tp_of(picker);
    if (locale == NULL) {
        if (t->locale == NULL) {
            return;
        }
        fdk_free(t->locale);
        t->locale = NULL;
    } else {
        if (t->locale == NULL) {
            t->locale = fdk_alloc(sizeof(*t->locale));
            if (t->locale == NULL) {
                return;
            }
        }
        *t->locale = *locale;
    }
    t->metrics_done = false; /* the AM/PM width may differ */
    fdk_widget_invalidate(picker);
}

fdk_result fdk_time_picker_set_time(fdk_widget *picker,
                                    const fdk_time *time) {
    if (picker == NULL || picker->klass != &fdk_time_picker_class_def ||
        time == NULL || time->hour < 0 || time->hour > 23 ||
        time->minute < 0 || time->minute > 59 || time->second < 0 ||
        time->second > 59) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_time_picker_impl *t = tp_of(picker);
    if (t->time.hour != time->hour || t->time.minute != time->minute ||
        t->time.second != time->second) {
        t->time = *time;
        fdk__a11y_notify(picker, FDK_A11Y_VALUE_CHANGED, 0);
        fdk_widget_invalidate(picker);
        tp_fire_changed(t);
    }
    return FDK_OK;
}

void fdk_time_picker_get_time(fdk_widget *picker, fdk_time *out_time) {
    if (out_time == NULL) {
        return;
    }
    if (picker == NULL || picker->klass != &fdk_time_picker_class_def) {
        memset(out_time, 0, sizeof(*out_time));
        return;
    }
    *out_time = tp_of(picker)->time;
}

void fdk_time_picker_set_style(fdk_widget *picker, fdk_time_style style) {
    if (picker == NULL || picker->klass != &fdk_time_picker_class_def ||
        style < FDK_TIME_SHORT || style > FDK_TIME_12H) {
        return;
    }
    fdk_time_picker_impl *t = tp_of(picker);
    if (t->style != style) {
        t->style = style;
        t->metrics_done = false; /* the segment set changed */
        if (t->active == TSEG_SECOND && style != FDK_TIME_MEDIUM) {
            t->active = TSEG_MINUTE;
        }
        if (t->active == TSEG_DAYPART && !tp_use12(t)) {
            t->active = TSEG_MINUTE;
        }
        fdk_widget_invalidate(picker);
        fdk_widget_child_layout_changed(picker);
    }
}

void fdk_time_picker_set_on_changed(fdk_widget *picker,
                                    fdk_time_picker_changed_fn on_changed,
                                    void *user) {
    if (picker == NULL || picker->klass != &fdk_time_picker_class_def) {
        return;
    }
    fdk_time_picker_impl *t = tp_of(picker);
    t->on_changed = on_changed;
    t->on_changed_user = user;
}
