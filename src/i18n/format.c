#define FDK_LOG_TAG "i18n"

/*
 * format.c — number, currency, and percent formatting
 *
 * The engine is deliberately two-stage: the C-locale fixed-point
 * conversion (snprintf "%.*f" — deterministic BECAUSE FDK never
 * calls setlocale, so the decimal point is '.' and there is never
 * grouping) produces the digits, and this module rewrites separators,
 * grouping, digits, and symbols per the locale's rules row. No
 * printf-locale surprises, no libc number-format state.
 *
 * Rounding note: "%.*f" rounds the exact binary value to nearest,
 * ties-to-even — the same policy CLDR/ICU default to for currency.
 * fdk_format_int needs no conversion at all (exact decimal digits
 * from the 64-bit integer), and INT64_MIN is handled through the
 * unsigned magnitude so nothing overflows.
 */

#include "i18n_internal.h"

#include "core/log_internal.h"

#include <stdio.h>
#include <string.h>

/* Arabic-Indic digit glyphs (U+0660..U+0669), one string each — the
 * latn path is plain '0'+'d' arithmetic with no table. */
static const char *const digits_arab[10] = {
    "٠", "١", "٢", "٣", "٤", "٥", "٦", "٧", "٨", "٩"};

/* ---- shared helpers (declared in i18n_internal.h) ---- */

int fdk__fmt_append(char *buf, size_t cap, size_t *len,
                    const char *text) {
    if (text == NULL) {
        return 0;
    }
    while (*text != '\0') {
        if (*len + 1 >= cap) {
            return 1;
        }
        buf[(*len)++] = *text++;
    }
    return 0;
}

int fdk__fmt_uint(char *buf, size_t cap, size_t *len, fdk_u64 value,
                  int pad, const char *digits) {
    /* digits == NULL means latn: single-byte fast path. The arab
     * table yields multi-byte glyphs, so the generic path builds a
     * reversed byte buffer first. */
    if (digits == NULL) {
        char tmp[24];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + (int)(value % 10));
            value /= 10;
        } while (value != 0);
        while (n < pad) {
            tmp[n++] = '0';
        }
        while (n > 0) {
            if (*len + 1 >= cap) {
                return 1;
            }
            buf[(*len)++] = tmp[--n];
        }
        return 0;
    }
    /* Multi-byte digits: emit most-significant first via recursion
     * into the decimal decomposition. */
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value != 0);
    while (n < pad) {
        tmp[n++] = '0';
    }
    while (n > 0) {
        if (fdk__fmt_append(buf, cap, len, digits_arab[tmp[--n] - '0']) !=
            0) {
            return 1;
        }
    }
    return 0;
}

int fdk__fmt_group(char *buf, size_t cap, size_t *len,
                   const char *digits, size_t n_digits,
                   const fdk_i18n_rules *rules) {
    /* digits[] here is ASCII '0'..'9' (the already-rounded decimal
     * string from the C-locale conversion, or the int formatter's
     * output). Groups are measured from the least-significant end
     * (the last group is primary-sized, every group left of it is
     * secondary-sized: Western 3/3 gives 1,234,567; Indian 3/2 gives
     * 1,23,456), but EMITTED most-significant-first. */
    if (n_digits == 0) {
        /* A zero magnitude still prints one digit ("0"). */
        if (rules->digits == 1) {
            return fdk__fmt_append(buf, cap, len, digits_arab[0]);
        }
        if (*len + 1 >= cap) {
            return 1;
        }
        buf[(*len)++] = '0';
        return 0;
    }

    /* Measure the groups right-to-left. */
    size_t gsizes[40]; /* 16 int digits + slack; bounded inputs */
    size_t ngroups = 0;
    size_t remaining = n_digits;
    while (remaining > 0) {
        size_t g = (ngroups == 0) ? rules->group_primary
                                  : rules->group_secondary;
        if (g == 0) {
            g = 3; /* defensive: a zero group size would loop */
        }
        if (remaining <= g) {
            gsizes[ngroups++] = remaining;
            remaining = 0;
        } else {
            gsizes[ngroups++] = g;
            remaining -= g;
        }
    }

    /* Emit left-to-right: the FIRST group in output order is the
     * LAST measured. */
    size_t start = 0; /* start of the next group to emit */
    for (size_t gi = ngroups; gi > 0; gi--) {
        size_t gsize = gsizes[gi - 1];
        for (size_t k = 0; k < gsize; k++) {
            char c = digits[start + k];
            if (rules->digits == 1) {
                if (fdk__fmt_append(buf, cap, len,
                                    digits_arab[c - '0']) != 0) {
                    return 1;
                }
            } else {
                if (*len + 1 >= cap) {
                    return 1;
                }
                buf[(*len)++] = c;
            }
        }
        start += gsize;
        if (gi > 1 && fdk__fmt_append(buf, cap, len, rules->group) != 0) {
            return 1;
        }
    }
    return 0;
}

fdk_result fdk__fmt_split_double(fdk_f64 value, int fraction_digits,
                                 bool *negative, char *digits,
                                 size_t digits_cap, size_t *int_digits) {
    if (negative == NULL || digits == NULL || int_digits == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (fraction_digits < 0 || fraction_digits > 9) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* Reject NaN / infinity (x*0 != 0 is true for exactly those) and
     * out-of-range magnitudes (see header). */
    if (value * 0.0 != 0.0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (value > 1e15 - 1 || value < -(1e15 - 1)) {
        return FDK_ERR_UNSUPPORTED;
    }
    if (value < 1e-9 && value > -1e-9 && value != 0.0) {
        return FDK_ERR_UNSUPPORTED;
    }

    *negative = value < 0.0;
    fdk_f64 mag = *negative ? -value : value;

    char tmp[64];
    int written = snprintf(tmp, sizeof(tmp), "%.*f", fraction_digits,
                           mag);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        return FDK_ERR_UNSUPPORTED;
    }

    /* Re-parse the fixed-point string: [int-digits][.][frac-digits].
     * The C locale guarantees '.' and no grouping (documented
     * invariant; FDK never calls setlocale). */
    size_t n = (size_t)written;
    size_t id = 0;
    while (id < n && tmp[id] != '.') {
        id++;
    }
    size_t fd = (id < n) ? n - id - 1 : 0;

    if (id + fd >= digits_cap) {
        return FDK_ERR_UNSUPPORTED;
    }
    memcpy(digits, tmp, id);
    if (fd > 0) {
        memcpy(digits + id, tmp + id + 1, fd);
    }
    digits[id + fd] = '\0';
    *int_digits = id;
    return FDK_OK;
}

/* ---- argument plumbing ---- */

static const fdk_locale *resolve_locale(const fdk_locale *loc) {
    return (loc != NULL) ? loc : fdk_locale_root();
}

static const fdk_number_options *resolve_options(
    const fdk_number_options *opt) {
    static const fdk_number_options zero;
    return (opt != NULL) ? opt : &zero;
}

/* Final NUL + FDK_ERR_LIMIT discipline shared by every formatter:
 * on success the buffer is terminated; on overflow the buffer is
 * set to "" (when it has room) so a caller can still print it. */
static fdk_result finish(char *buf, size_t cap, size_t len, int overflowed) {
    if (overflowed) {
        if (cap > 0) {
            buf[0] = '\0';
        }
        return FDK_ERR_LIMIT;
    }
    buf[len] = '\0';
    return FDK_OK;
}

/* ---- integer formatting ---- */

fdk_result fdk_format_int(char *buf, size_t cap, const fdk_locale *loc,
                          fdk_i64 value, const fdk_number_options *opt) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    const fdk_number_options *o = resolve_options(opt);
    if (o->min_fraction_digits != 0 || o->max_fraction_digits != 0) {
        if (o->max_fraction_digits != 0 &&
            o->min_fraction_digits > o->max_fraction_digits) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
    }

    bool negative = value < 0;
    fdk_u64 mag = negative ? (fdk_u64)(-(value + 1)) + 1
                           : (fdk_u64)value;

    /* Exact decimal digits (no printf): reversed then handed to the
     * grouping writer. */
    char rev[24];
    int n = 0;
    do {
        rev[n++] = (char)('0' + (int)(mag % 10));
        mag /= 10;
    } while (mag != 0);
    /* digits in most-significant-first order for fdk__fmt_group: */
    char digits[24];
    for (int i = 0; i < n; i++) {
        digits[i] = rev[n - 1 - i];
    }

    size_t len = 0;
    int ov = 0;
    if (negative) {
        ov |= fdk__fmt_append(buf, cap, &len, "-");
    } else if (o->sign_always) {
        ov |= fdk__fmt_append(buf, cap, &len, "+");
    }

    if (o->use_grouping == 0 || o->use_grouping == 1) {
        /* default (0): the locale's grouping — always on today */
        ov |= fdk__fmt_group(buf, cap, &len, digits, (size_t)n, rules);
    } else {
        for (int i = 0; i < n && ov == 0; i++) {
            if (rules->digits == 1) {
                ov |= fdk__fmt_append(buf, cap, &len,
                                      digits_arab[digits[i] - '0']);
            } else {
                if (len + 1 >= cap) {
                    ov = 1;
                } else {
                    buf[len++] = digits[i];
                }
            }
        }
    }

    /* min/max fraction digits (an integer with fraction defaults of
     * zero still honors an explicit min, e.g. "1.00"): */
    int frac = o->min_fraction_digits;
    if (o->max_fraction_digits > 0 && frac > o->max_fraction_digits) {
        frac = o->max_fraction_digits;
    }
    if (frac > 0) {
        ov |= fdk__fmt_append(buf, cap, &len, rules->decimal);
        for (int i = 0; i < frac; i++) {
            if (rules->digits == 1) {
                ov |= fdk__fmt_append(buf, cap, &len,
                                      digits_arab[0]);
            } else {
                if (len + 1 >= cap) {
                    ov = 1;
                } else {
                    buf[len++] = '0';
                }
            }
        }
    }
    return finish(buf, cap, len, ov);
}

/* ---- double formatting ---- */

fdk_result fdk_format_double(char *buf, size_t cap, const fdk_locale *loc,
                             fdk_f64 value, int fraction_digits,
                             const fdk_number_options *opt) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);
    const fdk_number_options *o = resolve_options(opt);
    if (fraction_digits < 0 || fraction_digits > 9) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (o->min_fraction_digits > 0 &&
        o->max_fraction_digits > 0 &&
        o->min_fraction_digits > o->max_fraction_digits) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* min_fraction overrides upward; max clamps the requested count
     * (both only when the caller set them). */
    int frac = fraction_digits;
    if (o->max_fraction_digits > 0 && frac > o->max_fraction_digits) {
        frac = o->max_fraction_digits;
    }
    if (o->min_fraction_digits > frac) {
        frac = o->min_fraction_digits;
    }

    bool negative = false;
    char digits[40];
    size_t int_digits = 0;
    fdk_result r = fdk__fmt_split_double(value, frac, &negative, digits,
                                         sizeof(digits), &int_digits);
    if (!fdk_ok(r)) {
        return r;
    }

    size_t len = 0;
    int ov = 0;
    if (negative) {
        ov |= fdk__fmt_append(buf, cap, &len, "-");
    } else if (o->sign_always) {
        ov |= fdk__fmt_append(buf, cap, &len, "+");
    }

    if (o->use_grouping == 0 || o->use_grouping == 1) {
        ov |= fdk__fmt_group(buf, cap, &len, digits, int_digits, rules);
    } else {
        for (size_t i = 0; i < int_digits && ov == 0; i++) {
            if (rules->digits == 1) {
                ov |= fdk__fmt_append(buf, cap, &len,
                                      digits_arab[digits[i] - '0']);
            } else {
                if (len + 1 >= cap) {
                    ov = 1;
                } else {
                    buf[len++] = digits[i];
                }
            }
        }
    }

    if (frac > 0) {
        ov |= fdk__fmt_append(buf, cap, &len, rules->decimal);
        for (size_t i = int_digits; i < int_digits + (size_t)frac;
             i++) {
            if (rules->digits == 1) {
                ov |= fdk__fmt_append(buf, cap, &len,
                                      digits_arab[digits[i] - '0']);
            } else {
                if (len + 1 >= cap) {
                    ov = 1;
                } else {
                    buf[len++] = digits[i];
                }
            }
        }
    }
    return finish(buf, cap, len, ov);
}

/* ---- currency ---- */

typedef struct fdk_currency_info {
    char code[4];
    const char *symbol;
    fdk_u8 decimals;
    bool symbol_after; /* false = before the amount            */
    bool symbol_space; /* a space between symbol and amount    */
} fdk_currency_info;

static const fdk_currency_info currency_table[] = {
    {"USD", "$", 2, false, false},
    {"EUR", "€", 2, true, true},
    {"GBP", "£", 2, false, false},
    {"JPY", "¥", 0, false, false},
    {"CNY", "¥", 2, false, false},
    {"INR", "₹", 2, false, false},
    {"KRW", "₩", 0, false, false},
    {"RUB", "₽", 2, true, true},
    {"BRL", "R$", 2, false, true},
    {"TRY", "₺", 2, true, true},
    {"CHF", "CHF", 2, true, true},
    {"SEK", "kr", 2, true, true},
    {"NOK", "kr", 2, true, true},
    {"DKK", "kr", 2, true, true},
    {"PLN", "zł", 2, true, true},
    {"UAH", "₴", 2, true, true},
    {"CZK", "Kč", 2, true, true},
    {"HUF", "Ft", 2, true, true},
    {"AUD", "A$", 2, false, false},
    {"CAD", "C$", 2, false, false},
    {"NZD", "NZ$", 2, false, false},
    {"SGD", "S$", 2, false, false},
    {"HKD", "HK$", 2, false, false},
    {"MXN", "MX$", 2, false, false},
    {"ILS", "₪", 2, false, false},
    {"THB", "฿", 2, false, false},
    {"VND", "₫", 0, true, true},
    {"IDR", "Rp", 2, false, false},
    {"ZAR", "R", 2, false, false},
    {"SAR", "SAR", 2, true, true},
    {"AED", "AED", 2, true, true},
    {"KWD", "د.ك", 3, true, true},
    {"BHD", "BD", 3, true, true},
};

static const fdk_currency_info *find_currency(const char *code) {
    if (code == NULL) {
        return NULL;
    }
    for (size_t i = 0;
         i < sizeof(currency_table) / sizeof(currency_table[0]); i++) {
        bool match = true;
        for (int k = 0; k < 3; k++) {
            char a = code[k];
            if (a >= 'a' && a <= 'z') {
                a = (char)(a - 'a' + 'A');
            }
            if (a != currency_table[i].code[k]) {
                match = false;
                break;
            }
        }
        if (match && (code[3] == '\0')) {
            return &currency_table[i];
        }
    }
    return NULL;
}

fdk_result fdk_format_currency(char *buf, size_t cap,
                               const fdk_locale *loc, fdk_f64 amount,
                               const char *currency_code) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);

    const fdk_currency_info *cur = find_currency(currency_code);
    bool fallback = (cur == NULL);
    fdk_currency_info unknown;
    if (fallback) {
        /* Unknown code: validate it is at least 3 ASCII letters (a
         * garbage code is an argument error; an unlisted-but-real
         * code formats as itself). */
        if (currency_code == NULL) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        for (int k = 0; k < 3; k++) {
            char c = currency_code[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) {
                return FDK_ERR_INVALID_ARGUMENT;
            }
        }
        if (currency_code[3] != '\0') {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        char upper[4];
        for (int k = 0; k < 3; k++) {
            char c = currency_code[k];
            upper[k] = (c >= 'a' && c <= 'z')
                          ? (char)(c - 'a' + 'A')
                          : c;
        }
        upper[3] = '\0';
        memcpy(unknown.code, upper, 4);
        unknown.symbol = NULL; /* resolved from unknown.code below */
        unknown.decimals = 2;
        unknown.symbol_after = true;
        unknown.symbol_space = true;
        cur = &unknown;
    }

    bool negative = false;
    char digits[40];
    size_t int_digits = 0;
    fdk_result r = fdk__fmt_split_double(
        amount, cur->decimals, &negative, digits, sizeof(digits),
        &int_digits);
    if (!fdk_ok(r)) {
        return r;
    }

    const char *symbol =
        fallback ? (const char *)unknown.code : cur->symbol;

    size_t len = 0;
    int ov = 0;
    if (negative) {
        ov |= fdk__fmt_append(buf, cap, &len, "-");
    }
    if (!cur->symbol_after) {
        ov |= fdk__fmt_append(buf, cap, &len, symbol);
        if (cur->symbol_space) {
            ov |= fdk__fmt_append(buf, cap, &len, " ");
        }
    }
    ov |= fdk__fmt_group(buf, cap, &len, digits, int_digits, rules);
    if (cur->decimals > 0) {
        ov |= fdk__fmt_append(buf, cap, &len, rules->decimal);
        for (size_t i = int_digits;
             i < int_digits + (size_t)cur->decimals; i++) {
            if (rules->digits == 1) {
                ov |= fdk__fmt_append(buf, cap, &len,
                                      digits_arab[digits[i] - '0']);
            } else {
                if (len + 1 >= cap) {
                    ov = 1;
                } else {
                    buf[len++] = digits[i];
                }
            }
        }
    }
    if (cur->symbol_after) {
        if (cur->symbol_space) {
            ov |= fdk__fmt_append(buf, cap, &len, " ");
        }
        ov |= fdk__fmt_append(buf, cap, &len, symbol);
    }
    return finish(buf, cap, len, ov);
}

/* ---- percent ---- */

fdk_result fdk_format_percent(char *buf, size_t cap,
                              const fdk_locale *loc, fdk_f64 fraction) {
    if (buf == NULL || cap == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    const fdk_locale *l = resolve_locale(loc);
    const fdk_i18n_rules *rules = fdk__i18n_rules_row(l->rules);

    /* Scale first; the same magnitude limits as the double
     * formatter then apply to the scaled value. */
    fdk_f64 scaled = fraction * 100.0;

    bool negative = false;
    char digits[40];
    size_t int_digits = 0;
    fdk_result r = fdk__fmt_split_double(scaled, 0, &negative, digits,
                                         sizeof(digits), &int_digits);
    if (!fdk_ok(r)) {
        return r;
    }

    size_t len = 0;
    int ov = 0;
    if (negative) {
        ov |= fdk__fmt_append(buf, cap, &len, "-");
    }
    ov |= fdk__fmt_group(buf, cap, &len, digits, int_digits, rules);
    if (rules->percent_space) {
        ov |= fdk__fmt_append(buf, cap, &len, " ");
    }
    ov |= fdk__fmt_append(buf, cap, &len, rules->percent);
    return finish(buf, cap, len, ov);
}
