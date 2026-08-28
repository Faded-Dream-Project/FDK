#define FDK_LOG_TAG "i18n"

/*
 * datetime.c — calendar math and date/time formatting
 *
 * The calendar is the proleptic Gregorian, year 1..9999, computed
 * with Howard Hinnant's civil-days algorithms (days_from_civil /
 * civil_from_days) — exact integer math, no time_t, no tm, no
 * leap-year folklore, valid across the whole supported range
 * (including negative day counts for pre-1970 dates).
 *
 * The pattern engine is a CLDR-UDRM-subset: y / yy / yyyy, M / MM /
 * MMM / MMMM, d / dd, E / EEEE, H / HH, h / hh, m / mm, s / ss, a
 * (day period), and single-quoted literals ('de', 'г'.). Unknown
 * pattern characters are a data bug in the rules table, not an input
 * the app controls — they fail loudly with FDK_ERR_INVALID_ARGUMENT
 * rather than printing garbage.
 */

#include "i18n_internal.h"

#include "core/log_internal.h"

#include <string.h>

/* ---- calendar math ---- */

bool fdk_date_is_leap_year(fdk_i32 year) {
    if (year % 4 != 0) {
        return false;
    }
    if (year % 100 != 0) {
        return true;
    }
    return year % 400 == 0;
}

fdk_i32 fdk_date_days_in_month(fdk_i32 year, fdk_i32 month) {
    static const fdk_i32 per_month[12] = {31, 28, 31, 30, 31, 30,
                                          31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && fdk_date_is_leap_year(year)) {
        return 29;
    }
    return per_month[month - 1];
}

static bool date_valid(const fdk_date *d) {
    if (d == NULL || d->year < 1 || d->year > 9999 || d->month < 1 ||
        d->month > 12) {
        return false;
    }
    return d->day >= 1 && d->day <= fdk_date_days_in_month(d->year,
                                                           d->month);
}

/* days_from_civil (Hinnant): days since 1970-01-01, exact. */
fdk_i64 fdk_date_to_days(const fdk_date *date) {
    if (!date_valid(date)) {
        return 0;
    }
    fdk_i64 y = date->year;
    fdk_i64 m = date->month;
    fdk_i64 d = date->day;
    y -= m <= 2 ? 1 : 0;
    fdk_i64 era = (y >= 0 ? y : y - 399) / 400;
    fdk_i64 yoe = y - era * 400; /* [0, 399] */
    fdk_i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    fdk_i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* civil_from_days (Hinnant): inverse of the above. */
fdk_result fdk_date_from_days(fdk_i64 days, fdk_date *out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (days < -719162 || days > 2932896) { /* 0001-01-01 .. 9999-12-31 */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_i64 z = days + 719468;
    fdk_i64 era = (z >= 0 ? z : z - 146096) / 146097;
    fdk_i64 doe = z - era * 146097; /* [0, 146096] */
    fdk_i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    fdk_i64 y = yoe + era * 400;
    fdk_i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    fdk_i64 mp = (5 * doy + 2) / 153;
    fdk_i64 d = doy - (153 * mp + 2) / 5 + 1;
    fdk_i64 m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2 ? 1 : 0);
    out->year = (fdk_i32)y;
    out->month = (fdk_i32)m;
    out->day = (fdk_i32)d;
    return FDK_OK;
}

fdk_i32 fdk_date_weekday(const fdk_date *date) {
    if (!date_valid(date)) {
        return -1;
    }
    fdk_i64 days = fdk_date_to_days(date);
    fdk_i64 wd = ((days % 7) + 7 + 3) % 7; /* 1970-01-01 = Thursday */
    return (fdk_i32)wd;                    /* 0 = Monday .. 6 = Sunday */
}

/* ---- pattern engine ---- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflowed;
} outbuf;

static void emit(outbuf *o, const char *text) {
    if (o->overflowed) {
        return;
    }
    if (fdk__fmt_append(o->buf, o->cap, &o->len, text) != 0) {
        o->overflowed = 1;
    }
}

static void emit_num(outbuf *o, fdk_u64 value, int width) {
    if (o->overflowed) {
        return;
    }
    if (fdk__fmt_uint(o->buf, o->cap, &o->len, value, width, NULL) !=
        0) {
        o->overflowed = 1;
    }
}

/* Runs one pattern over (date, time, names). date or time may be
 * NULL: the corresponding tokens are a table bug and fail. */
static fdk_result run_pattern(const char *pattern, const fdk_date *date,
                              const fdk_time *time,
                              const fdk_i18n_names *names, outbuf *o) {
    size_t i = 0;
    size_t n = strlen(pattern);
    while (i < n) {
        char c = pattern[i];
        if (c == '\'') {
            /* Quoted literal: '' is a literal apostrophe. */
            i++;
            if (i < n && pattern[i] == '\'') {
                emit(o, "'");
                i++;
                continue;
            }
            while (i < n && pattern[i] != '\'') {
                char one[2] = {pattern[i], '\0'};
                emit(o, one);
                i++;
            }
            i++; /* closing quote */
            continue;
        }

        /* Measure the run of the same character. */
        size_t run = 1;
        while (i + run < n && pattern[i + run] == c) {
            run++;
        }

        switch (c) {
        case 'y': {
            if (date == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            fdk_u64 y = (fdk_u64)date->year;
            if (run == 2) {
                emit_num(o, y % 100, 2);
            } else if (run == 4) {
                emit_num(o, y, 4);
            } else {
                emit_num(o, y, 0); /* 'y': no padding */
            }
            break;
        }
        case 'M': {
            if (date == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            if (run == 3) {
                emit(o, names->months_abbr[date->month - 1]);
            } else if (run >= 4) {
                emit(o, names->months_long[date->month - 1]);
            } else {
                emit_num(o, (fdk_u64)date->month, run == 2 ? 2 : 1);
            }
            break;
        }
        case 'd': {
            if (date == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            emit_num(o, (fdk_u64)date->day, run >= 2 ? 2 : 1);
            break;
        }
        case 'E': {
            if (date == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            fdk_i32 wd = fdk_date_weekday(date);
            if (wd < 0) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            if (run >= 4) {
                emit(o, names->days_long[wd]);
            } else {
                emit(o, names->days_abbr[wd]);
            }
            break;
        }
        case 'H': {
            if (time == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            emit_num(o, (fdk_u64)time->hour, run >= 2 ? 2 : 1);
            break;
        }
        case 'h': {
            if (time == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            fdk_i32 h = time->hour % 12;
            if (h == 0) {
                h = 12; /* 12 AM / 12 PM clock convention */
            }
            emit_num(o, (fdk_u64)h, run >= 2 ? 2 : 1);
            break;
        }
        case 'm': {
            if (time == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            emit_num(o, (fdk_u64)time->minute, run >= 2 ? 2 : 1);
            break;
        }
        case 's': {
            if (time == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            emit_num(o, (fdk_u64)time->second, run >= 2 ? 2 : 1);
            break;
        }
        case 'a': {
            if (time == NULL) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
            emit(o, time->hour < 12 ? names->am : names->pm);
            break;
        }
        default:
            /* Any other character is a literal (spaces, commas,
             * CJK text like 年/月/日, 'г'. quoted separately). Emit
             * the whole run at once for multi-byte safety. */
            {
                char lit[16];
                if (run >= sizeof(lit)) {
                    return FDK_ERR_INVALID_ARGUMENT;
                }
                for (size_t k = 0; k < run; k++) {
                    lit[k] = pattern[i + k];
                }
                lit[run] = '\0';
                emit(o, lit);
            }
            break;
        }
        i += run;
    }
    return FDK_OK;
}

/* ---- public formatters ---- */

static const fdk_locale *resolve_locale(const fdk_locale *loc) {
    return (loc != NULL) ? loc : fdk_locale_root();
}

fdk_result fdk_format_date(char *buf, size_t cap, const fdk_locale *loc,
                           const fdk_date *date, fdk_date_style style) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    if (!date_valid(date)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    const fdk_i18n_names *names =
        (rules->names != NULL) ? rules->names : &fdk__i18n_names_en;

    const char *pattern;
    switch (style) {
    case FDK_DATE_SHORT:
        pattern = rules->date_short;
        break;
    case FDK_DATE_MEDIUM:
        pattern = rules->date_medium;
        break;
    case FDK_DATE_LONG:
        pattern = rules->date_long;
        break;
    case FDK_DATE_FULL:
        pattern = rules->date_full;
        break;
    case FDK_DATE_ISO:
        pattern = "yyyy-MM-dd";
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }

    outbuf o = {buf, cap, 0, 0};
    fdk_result r = run_pattern(pattern, date, NULL, names, &o);
    if (!fdk_ok(r)) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return r;
    }
    if (o.overflowed) {
        buf[0] = '\0';
        return FDK_ERR_LIMIT;
    }
    buf[o.len] = '\0';
    return FDK_OK;
}

fdk_result fdk_format_time(char *buf, size_t cap, const fdk_locale *loc,
                           const fdk_time *time, fdk_time_style style) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    if (time == NULL || time->hour < 0 || time->hour > 23 ||
        time->minute < 0 || time->minute > 59 || time->second < 0 ||
        time->second > 59) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    const fdk_i18n_names *names =
        (rules->names != NULL) ? rules->names : &fdk__i18n_names_en;

    const char *pattern;
    bool use12;
    switch (style) {
    case FDK_TIME_SHORT:
        pattern = rules->time_short;
        use12 = rules->hour12;
        break;
    case FDK_TIME_MEDIUM:
        pattern = rules->time_medium;
        use12 = rules->hour12;
        break;
    case FDK_TIME_24H:
        pattern = "HH:mm";
        use12 = false;
        break;
    case FDK_TIME_12H:
        /* Locale-aware ordering: languages with localized day
         * periods (ja/zh/ko: 午後/上午/오후) put them BEFORE the
         * time; the rest use the en-shaped "3:30 PM". */
        pattern = (names->am[0] != '\0' &&
                   strcmp(names->am, "AM") != 0)
                      ? "ah:mm"
                      : "h:mm a";
        use12 = true;
        break;
    default:
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Forced 12H on a 24h locale uses this same English AM/PM —
     * the documented fallback (the pattern below is en-shaped). */
    char rewritten[32];
    if (!use12) {
        /* Rewrite the pattern's 12h tokens to 24h equivalents so a
         * forced-24H style works on any locale's pattern. */
        size_t w = 0;
        for (size_t i = 0; pattern[i] != '\0' && w + 2 < sizeof(rewritten);) {
            char pc = pattern[i];
            if (pc == 'h') {
                rewritten[w++] = 'H';
                i++;
            } else if (pc == 'a') {
                i++; /* drop the day-period token */
            } else {
                rewritten[w++] = pc;
                i++;
            }
        }
        /* Also drop a space left before nothing (trailing "15:30 "). */
        while (w > 0 && rewritten[w - 1] == ' ') {
            w--;
        }
        /* Collapse a doubled space left where 'a' was dropped. */
        for (size_t k = 0; k + 1 < w; k++) {
            if (rewritten[k] == ' ' && rewritten[k + 1] == ' ') {
                memmove(&rewritten[k], &rewritten[k + 1], w - k - 1);
                w--;
                k--;
            }
        }
        rewritten[w] = '\0';
        pattern = rewritten;
    }

    outbuf o = {buf, cap, 0, 0};
    fdk_result r = run_pattern(pattern, NULL, time, names, &o);
    if (!fdk_ok(r)) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return r;
    }
    if (o.overflowed) {
        buf[0] = '\0';
        return FDK_ERR_LIMIT;
    }
    buf[o.len] = '\0';
    return FDK_OK;
}
