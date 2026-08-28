#define FDK_LOG_TAG "i18n"

/*
 * plural.c — CLDR plural rules
 *
 * One pure function per rule shape, referenced from the rules table
 * (locale.c). The operand semantics are CLDR's:
 *
 *   n  absolute value of the number
 *   i  integer digits
 *   v  visible fraction digits WITH trailing zeros   ("1.50" -> 2)
 *   w  visible fraction digits WITHOUT trailing zeros ("1.50" -> 1)
 *   f  fraction digits as an integer WITH zeros       ("1.50" -> 50)
 *   t  fraction digits as an integer WITHOUT zeros     ("1.50" -> 5)
 *
 * Operands for doubles are computed from the 9-decimal fixed-point
 * rendering: no language in the covered set asks about fraction
 * digits past 9 (and CLDR samples never do either); the truncation
 * is documented in docs/i18n.md rather than hidden.
 *
 * The covered languages (and their category sets), rules as in CLDR:
 *   one/other          en de nl sv da no nb et fi el es ca it hi
 *   one(0..1)/other    fr, pt (pt-PT narrows one to exactly 1)
 *   one/few/many/other ru uk pl lt        (fractions differ: ru/uk/pl
 *                                          -> other, lt -> many)
 *   one/few/many/other cs sk               (many == fractions)
 *   one/few/other      hr sr bs
 *   zero/one/other     lv                  (fractions -> other)
 *   all six            ar                   (fractions -> other)
 *   other only         root tr id vi th ja ko zh
 * Languages outside the table resolve to the root row = other-only —
 * the same fallback CLDR defines, never a guess.
 */

#include "i18n_internal.h"

#include <stdio.h>
#include <string.h>

/* ---- rule functions (one per CLDR rule shape) ---- */

fdk_plural_category plural_one_i1v0(const fdk_plural_operands *op) {
    /* en & co: one when i = 1 and v = 0 ("1", not "1.0") */
    if (op->i == 1 && op->v == 0) {
        return FDK_PLURAL_ONE;
    }
    return FDK_PLURAL_OTHER;
}

fdk_plural_category plural_one_i01(const fdk_plural_operands *op) {
    /* fr, pt: one for i = 0 or 1 — INCLUDING 0.5 and 1.5 (CLDR's
     * rule keys on the integer part only; French really does treat
     * "1,5 file" as the singular category). */
    if (op->i <= 1) {
        return FDK_PLURAL_ONE;
    }
    return FDK_PLURAL_OTHER;
}

fdk_plural_category plural_ru_uk(const fdk_plural_operands *op) {
    /* Russian / Ukrainian: one/few/many over integers, other for
     * fractions. 21/31/... are ONE here (unlike Polish). */
    if (op->v != 0) {
        return FDK_PLURAL_OTHER;
    }
    fdk_u64 i10 = op->i % 10;
    fdk_u64 i100 = op->i % 100;
    if (i10 == 1 && i100 != 11) {
        return FDK_PLURAL_ONE;
    }
    if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) {
        return FDK_PLURAL_FEW;
    }
    return FDK_PLURAL_MANY; /* 0, 5..20, 25..30, 11..14, 100... */
}

fdk_plural_category plural_pl(const fdk_plural_operands *op) {
    /* Polish: one is EXACTLY 1 (21 is many, unlike Russian);
     * fractions are other. */
    if (op->v != 0) {
        return FDK_PLURAL_OTHER;
    }
    if (op->i == 1) {
        return FDK_PLURAL_ONE;
    }
    fdk_u64 i10 = op->i % 10;
    fdk_u64 i100 = op->i % 100;
    if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) {
        return FDK_PLURAL_FEW;
    }
    return FDK_PLURAL_MANY; /* 0, 5..20, 21, 25..., 11..14, 100... */
}

fdk_plural_category plural_cs_sk(const fdk_plural_operands *op) {
    /* Czech / Slovak: many IS the fraction category; 0 and 5+ are
     * other. */
    if (op->i == 1 && op->v == 0) {
        return FDK_PLURAL_ONE;
    }
    if (op->i >= 2 && op->i <= 4 && op->v == 0) {
        return FDK_PLURAL_FEW;
    }
    if (op->v != 0) {
        return FDK_PLURAL_MANY;
    }
    return FDK_PLURAL_OTHER;
}

fdk_plural_category plural_hr_sr_bs(const fdk_plural_operands *op) {
    /* Croatian / Serbian / Bosnian: one/few over integers, other for
     * everything else (fractions included). */
    if (op->v == 0) {
        fdk_u64 i10 = op->i % 10;
        fdk_u64 i100 = op->i % 100;
        if (i10 == 1 && i100 != 11) {
            return FDK_PLURAL_ONE;
        }
        if (i10 >= 2 && i10 <= 4 && !(i100 >= 12 && i100 <= 14)) {
            return FDK_PLURAL_FEW;
        }
    }
    return FDK_PLURAL_OTHER;
}

fdk_plural_category plural_lt(const fdk_plural_operands *op) {
    /* Lithuanian: many IS the fraction category (f != 0), checked
     * FIRST because the integer rules also match i=1 of "1.5". */
    if (op->f != 0) {
        return FDK_PLURAL_MANY;
    }
    fdk_u64 n10 = op->i % 10;
    fdk_u64 n100 = op->i % 100;
    if (n10 == 1 && !(n100 >= 11 && n100 <= 19)) {
        return FDK_PLURAL_ONE;
    }
    if (n10 >= 2 && n10 <= 9 && !(n100 >= 11 && n100 <= 19)) {
        return FDK_PLURAL_FEW;
    }
    return FDK_PLURAL_OTHER; /* 0, 10..19, 20, 30, ... */
}

fdk_plural_category plural_lv(const fdk_plural_operands *op) {
    /* Latvian: zero/one over integers; fractions are other (0.5 is
     * NOT zero even though its integer part is 0). */
    if (op->f != 0) {
        return FDK_PLURAL_OTHER;
    }
    fdk_u64 n10 = op->i % 10;
    fdk_u64 n100 = op->i % 100;
    if (n10 == 0 || (n100 >= 11 && n100 <= 19)) {
        return FDK_PLURAL_ZERO;
    }
    if (n10 == 1 && n100 != 11) {
        return FDK_PLURAL_ONE;
    }
    return FDK_PLURAL_OTHER;
}

fdk_plural_category plural_ar(const fdk_plural_operands *op) {
    /* Arabic: the full six. Zero/one/two are exact n matches
     * (0.0 counts as zero even written "0.0"); any other fraction
     * is other; the 3..10 / 11..99 rules read i's last two digits. */
    if (op->n == 0.0) {
        return FDK_PLURAL_ZERO;
    }
    if (op->n == 1.0) {
        return FDK_PLURAL_ONE;
    }
    if (op->n == 2.0) {
        return FDK_PLURAL_TWO;
    }
    if (op->f != 0 || op->i == 0) {
        return FDK_PLURAL_OTHER; /* 0.5, 100.5, ... */
    }
    fdk_u64 i100 = op->i % 100;
    if (i100 >= 3 && i100 <= 10) {
        return FDK_PLURAL_FEW;
    }
    if (i100 >= 11 && i100 <= 99) {
        return FDK_PLURAL_MANY;
    }
    return FDK_PLURAL_OTHER; /* i100 in 0..2 outside the exact n
                              * matches above (100, 101, 102, 202...) */
}

/* ---- public API ---- */

void fdk_plural_operands_from_int(fdk_i64 value,
                                  fdk_plural_operands *out) {
    if (out == NULL) {
        return;
    }
    fdk_u64 mag = (value < 0)
                      ? (fdk_u64)(-(value + 1)) + 1
                      : (fdk_u64)value;
    out->n = (fdk_f64)mag;
    out->i = mag;
    out->v = 0;
    out->w = 0;
    out->f = 0;
    out->t = 0;
}

fdk_result fdk_plural_operands_from_double(fdk_f64 value,
                                           fdk_plural_operands *out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (value != value) { /* NaN */
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_f64 mag = (value < 0.0) ? -value : value;
    if (mag >= 1e15) {
        return FDK_ERR_UNSUPPORTED;
    }

    out->n = mag;
    out->i = (fdk_u64)mag;
    out->v = 0;
    out->w = 0;
    out->f = 0;
    out->t = 0;

    /* Fraction operands from the 9-decimal fixed-point rendering
     * (trailing-zero trimming for w/t). The subtraction
     * (mag - (double)out->i) keeps the fraction's leading zeros
     * ("0.05" -> 05) because %.9f renders it without an exponent. */
    char tmp[32];
    int written = snprintf(tmp, sizeof(tmp), "%.9f",
                           mag - (fdk_f64)out->i);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
        return FDK_ERR_UNSUPPORTED;
    }
    const char *frac = tmp;
    while (*frac != '\0' && *frac != '.') {
        frac++;
    }
    if (*frac == '.') {
        frac++;
        for (const char *p = frac; *p != '\0'; p++) {
            out->f = out->f * 10 + (fdk_u64)(*p - '0');
            out->v++;
        }
        /* A double carries no WRITTEN trailing zeros (1.50 == 1.5),
         * so strip the ones the %.9f rendering added: v/f become the
         * minimal fraction (== w/t). A string source would keep
         * them; the double API cannot. */
        while (out->v > 0 && out->f % 10 == 0) {
            out->f /= 10;
            out->v--;
        }
        out->t = out->f;
        out->w = out->v;
    }
    return FDK_OK;
}

fdk_plural_category fdk_plural_category_for(
    const fdk_locale *loc, const fdk_plural_operands *operands) {
    if (operands == NULL) {
        return FDK_PLURAL_OTHER;
    }
    const fdk_i18n_rules *rules =
        fdk__i18n_rules_row(loc != NULL ? loc->rules : 0);
    if (rules->plural == NULL) {
        return FDK_PLURAL_OTHER;
    }
    return rules->plural(operands);
}

fdk_plural_category fdk_plural_category_int(const fdk_locale *loc,
                                            fdk_i64 n) {
    fdk_plural_operands op;
    fdk_plural_operands_from_int(n, &op);
    return fdk_plural_category_for(loc, &op);
}
