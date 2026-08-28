/*
 * i18n_internal.h — shared internals of the i18n module
 *
 * Not part of the public API — never installed. The public contract
 * lives in include/fdk/fdk_i18n.h.
 *
 * The module is table-driven: fdk_locale_parse resolves a tag against
 * the rules table (longest match: language+territory, then language,
 * then the root row) and stores the row INDEX in fdk_locale.rules.
 * The formatters read the row; nothing about a locale is ever
 * recomputed per call.
 */

#ifndef FDK_I18N_INTERNAL_H
#define FDK_I18N_INTERNAL_H

#include "fdk/fdk_i18n.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- localized names (one table per shipped language) ---- */

typedef struct fdk_i18n_names {
    const char *months_long[12];  /* "January" ...          */
    const char *months_abbr[12];  /* "Jan", "Sept." ...     */
    const char *days_long[7];     /* index 0 = Monday       */
    const char *days_abbr[7];     /* "Mon" ...              */
    const char *am;               /* "AM", "午前" ...        */
    const char *pm;
} fdk_i18n_names;

/* ---- plural rule function ----
 *
 * Receives the CLDR operands; returns the category. Implementations
 * are pure functions over the operands (see plural.c for the table
 * and the language list). */
typedef fdk_plural_category (*fdk_plural_rule_fn)(
    const fdk_plural_operands *op);

/* ---- plural rule functions (defined in plural.c, referenced by
 * the rules table in locale.c) ----
 *
 * Named by the rule shape they implement, not by language (many
 * languages share a shape). */
fdk_plural_category plural_one_i1v0(const fdk_plural_operands *op);
fdk_plural_category plural_one_i01(const fdk_plural_operands *op);
fdk_plural_category plural_ru_uk(const fdk_plural_operands *op);
fdk_plural_category plural_pl(const fdk_plural_operands *op);
fdk_plural_category plural_cs_sk(const fdk_plural_operands *op);
fdk_plural_category plural_hr_sr_bs(const fdk_plural_operands *op);
fdk_plural_category plural_lt(const fdk_plural_operands *op);
fdk_plural_category plural_lv(const fdk_plural_operands *op);
fdk_plural_category plural_ar(const fdk_plural_operands *op);

/* ---- one locale-rules row ---- */

typedef struct fdk_i18n_rules {
    const char *language;   /* "" = the root row                    */
    const char *territory;  /* "" = the language's default row      */

    const char *decimal;    /* "." / "," / "٫"                      */
    const char *group;      /* "," / "." / " " / "'" / "٬"          */
    fdk_u8 group_primary;   /* digits in the least-significant group */
    fdk_u8 group_secondary; /* digits in every more-significant group
                             * (== primary except Indian 3 / 2)      */

    const char *percent;    /* "%" / "٪"                             */
    bool percent_space;     /* "50 %" (fr/de/ru) vs "50%" (en)       */

    bool hour12;            /* default clock cycle                   */

    fdk_u8 digits;          /* 0 = latn "0"-"9", 1 = arab "٠"-"٩"    */

    const char *date_short;   /* CLDR-subset patterns; see the     */
    const char *date_medium;  /* pattern engine in datetime.c      */
    const char *date_long;
    const char *date_full;
    const char *time_short;
    const char *time_medium;

    const fdk_i18n_names *names; /* NULL = English names           */

    fdk_plural_rule_fn plural;   /* NULL = other-only (root)       */
} fdk_i18n_rules;

/* The rules table, terminated by a row with language == NULL. Rows
 * are ordered so (language, territory) specifics precede their
 * (language, "") default, and the root row ("","") is first —
 * resolution scans and keeps the best (most specific) hit. */
extern const fdk_i18n_rules fdk__i18n_rules_table[];

/* The English names table (the fallback for locales without names). */
extern const fdk_i18n_names fdk__i18n_names_en;

/* Resolves a (language, territory) pair to a rules row. Never NULL —
 * unknown pairs return the root row. */
const fdk_i18n_rules *fdk__i18n_rules_for(const char *language,
                                          const char *territory);

/* The row a locale's rules index refers to (bounds-checked to the
 * root row — a stale index can't crash a formatter). */
const fdk_i18n_rules *fdk__i18n_rules_row(fdk_u32 index);

/* ---- shared low-level formatting helpers (format.c) ---- */

/* Appends `text` to buf/cap with bounds checking. Returns 0 on
 * success, 1 when the buffer filled (the caller converts that into
 * FDK_ERR_LIMIT). Writes a NUL only on success; the caller
 * terminates on the final success path. */
int fdk__fmt_append(char *buf, size_t cap, size_t *len,
                    const char *text);

/* Appends a non-negative integer with digit substitution and
 * zero-padding to `pad` (0 = no padding), WITHOUT grouping. */
int fdk__fmt_uint(char *buf, size_t cap, size_t *len, fdk_u64 value,
                  int pad, const char *digits);

/* Formats the digit string [0,n_digits) with grouping per the rules
 * row into buf/cap. Digits are already substituted. */
int fdk__fmt_group(char *buf, size_t cap, size_t *len,
                   const char *digits, size_t n_digits,
                   const fdk_i18n_rules *rules);

/* Rounds |value| to `fraction_digits` decimals and splits it into a
 * sign flag, digit buffer (integer+fraction concatenated), and the
 * integer-digit count. Uses the C-locale snprintf("%.*f") of the
 * exact binary value (ties-to-even), then re-parses — see
 * docs/i18n.md. Returns an fdk_result. */
fdk_result fdk__fmt_split_double(fdk_f64 value, int fraction_digits,
                                 bool *negative, char *digits,
                                 size_t digits_cap, size_t *int_digits);

#endif /* FDK_I18N_INTERNAL_H */
