/*
 * fdk_i18n.h — Faded Dream ToolKit: internationalization
 *
 * Phase 10's second half: locale-aware formatting, translation
 * catalogs, and pluralization. Three pieces, one design rule:
 *
 *   NO PROCESS-GLOBAL STATE. FDK never calls setlocale() and never
 *   consults the environment. Every function takes the locale it
 *   formats for as an explicit value, so one process can format for
 *   a different locale per window, per widget, per call — and the
 *   same code is deterministic under test (the gettext model of one
 *   global locale, and the "works differently when LANG changes"
 *   class of bug, are both impossible here).
 *
 * The pieces:
 *
 *   fdk_locale     a parsed BCP-47 / POSIX language tag. A fixed-size
 *                  VALUE type — no allocation, no lifetime, copyable.
 *                  Parsing resolves the tag against FDK's locale
 *                  rules table (separators, grouping, date patterns,
 *                  month/weekday names, plural rules) by longest
 *                  match: language+territory, then language, then
 *                  the root defaults (CLDR root ≈ English shaping).
 *                  Unknown tags parse successfully and format with
 *                  root rules — an unrecognized territory is not an
 *                  error (see fdk_locale_parse's contract).
 *
 *   formatters     numbers (integers exact to 64 bits, doubles via
 *                  fixed-point), currency (ISO-4217 table + locale
 *                  separators), percent, dates (proleptic Gregorian,
 *                  1..9999, exact civil-day math — no tm/no time_t/
 *                  no leap-year folklore), and times (12/24-hour per
 *                  locale, forced overrides available).
 *
 *   catalogs       translation message catalogs loaded from a strict,
 *                  bounded, line-numbered text format (the same
 *                  security discipline as the .fdk theme parser —
 *                  see docs/fdk-catalog-format.md) with binary-search
 *                  lookup, message contexts, and plural-form
 *                  selection driven by CLDR plural rules.
 *
 *   pluralization  the six CLDR plural categories (zero/one/two/few/
 *                  many/other) computed from the standard operand set
 *                  (n, i, v, w, f, t) by a rules table covering the
 *                  languages with non-trivial rules (plus every
 *                  one/other and other-only language FDK ships names
 *                  for). Languages not in the table fall back to
 *                  other-only, which is also CLDR's root behavior;
 *                  the covered list is documented in the roadmap, and
 *                  unknown does NOT mean "guessed" — it means
 *                  "formatted like root".
 *
 * Positioning: FDK itself has no user-visible strings to translate
 * (window titles, labels — all application text). i18n is application
 * infrastructure: the app formats and translates, then hands the
 * result to widgets. The toolkit does not inject translations into
 * widgets, and does not interpret "%n"-style placeholders inside
 * catalog strings (the app composes with its own snprintf discipline;
 * the header documents the pattern).
 *
 * Threading: all functions are pure computation over immutable
 * inputs — they read no global state and write only the caller's
 * buffer. Unlike the object-bearing parts of FDK (widgets, windows,
 * themes), the formatters and plural rules are safe to call from any
 * thread simultaneously. Catalogs are read-only after a successful
 * parse/load and likewise safe for concurrent reads.
 */

#ifndef FDK_I18N_H
#define FDK_I18N_H

#include "fdk_types.h"
#include "fdk_error.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Locale ---- */

/* A parsed language tag: a VALUE, not an object. Copy it, embed it,
 * keep it on the stack. The subtags are NUL-terminated, lower-cased
 * language / title-cased script / upper-cased territory, each at most
 * 7 characters plus the terminator (longer subtags are rejected at
 * parse — nothing silently truncates). `rules` is the resolved index
 * into FDK's internal rules table; treat it as opaque. */
typedef struct fdk_locale {
    char language[8];  /* ISO 639: "en", "pt", "zh"      */
    char script[8];    /* ISO 15924: "Latn", "Hant"; ""  */
    char territory[8]; /* ISO 3166: "US", "PT"; ""       */
    fdk_u32 rules;     /* resolved rules index (opaque)  */
} fdk_locale;

/* Parses `tag` into `out`.
 *
 * Accepted syntax, both spellings the real world uses:
 *   BCP-47:   "de", "de-CH", "zh-Hant-TW", "es-419"
 *   POSIX:    "de_CH.UTF-8", "zh_TW.big5", "C", "C.UTF-8", "POSIX"
 * (POSIX '_' is treated as the subtag separator; a following charset
 * segment and any "@modifier" are parsed and discarded; "C" and
 * "POSIX" produce the root locale, language "".)
 *
 * Rules: language is 2..8 ASCII letters; script is exactly 4 letters
 * (only accepted in the middle position); territory is 2 letters or
 * 3 digits; charset/modifier segments must be 1..16 chars of letters,
 * digits, '-', '.', or '_'. Anything else — embedded NULs (the tag is
 * measured with strlen, so none possible), empty subtags ("de--CH"),
 * trailing separators, unknown punctuation — fails with
 * FDK_ERR_INVALID_ARGUMENT. An unknown-but-well-formed language or
 * territory is NOT an error: it resolves to root rules.
 *
 * Case is normalized (EN → en, ch → CH). Writing back through
 * fdk_locale_to_tag reproduces the canonical BCP-47 form.
 */
fdk_result fdk_locale_parse(const char *tag, fdk_locale *out);

/* The canonical BCP-47 tag for a locale: "zh-Hant-TW" form, script
 * only when present, no charset (it was discarded at parse). "" for
 * the root locale. Returns the number of bytes written EXCLUDING the
 * terminator, or a negative fdk_result code on a bad argument or a
 * too-small buffer (the buffer is never overrun; on failure *buf is
 * set to "" when cap > 0). */
fdk_i32 fdk_locale_to_tag(const fdk_locale *loc, char *buf, size_t cap);

/* The root locale (language "", root rules) — the fallback every
 * NULL-locale argument below means. A pointer to static storage. */
const fdk_locale *fdk_locale_root(void);

/* A convenient English locale (language "en", no territory — en
 * root rules: "1,234.56", M/d/y). A pointer to static storage. */
const fdk_locale *fdk_locale_english(void);

/* ---- Number formatting ---- */

/* Options for the number formatters. This is an "input struct" per
 * docs/abi-policy.md: zero-initialize it for the defaults, fields are
 * only ever appended. Defaults: locale grouping, no forced sign,
 * locale-and-kind fraction digits.
 *
 * min/max_fraction_digits (0..9) override the kind's default when
 * nonzero; max < min fails with FDK_ERR_INVALID_ARGUMENT.
 * use_grouping: 0 = the locale default (on), nonzero toggles it to
 * that value (1 = group even where the locale wouldn't — none today
 * — or -1/any odd value = off; plain `= 1` and `= 0` cover the real
 * uses: default, force-off).
 * sign_always: nonzero prepends '+' to non-negative output.
 */
typedef struct fdk_number_options {
    int min_fraction_digits;
    int max_fraction_digits;
    int use_grouping;
    int sign_always;
} fdk_number_options;

/* Formats an exact signed 64-bit integer with the locale's grouping
 * and digits: 1234567 → "1,234,567" (en), "1.234.567" (de), "12,34,567"
 * (hi Indian grouping), "١٢٣٤٥٦٧" (ar). INT64_MIN formats exactly.
 * Returns FDK_OK, or FDK_ERR_INVALID_ARGUMENT (bad args), or
 * FDK_ERR_LIMIT (buffer too small; 32 bytes always suffice).
 * On success buf is NUL-terminated. */
fdk_result fdk_format_int(char *buf, size_t cap, const fdk_locale *loc,
                          fdk_i64 value, const fdk_number_options *opt);

/* Formats a double in fixed-point notation with the locale's
 * separators and digits. `fraction_digits` (0..9) is the number of
 * digits after the decimal separator; rounding is to nearest,
 * ties-to-even, performed by the C-locale fixed-point conversion of
 * the exact binary value (FDK never calls setlocale, so the
 * conversion's '.' and no-grouping output is an invariant this
 * function then rewrites — see the implementation notes in
 * docs/i18n.md).
 *
 * Doubles whose magnitude is >= 1e15 or < 1e-9 (and nonzero) are
 * refused with FDK_ERR_UNSUPPORTED rather than silently formatting
 * through exponent notation v1 does not implement — honest limits,
 * not garbage. NaN and infinities are refused with
 * FDK_ERR_INVALID_ARGUMENT. 64 bytes always suffice. */
fdk_result fdk_format_double(char *buf, size_t cap, const fdk_locale *loc,
                             fdk_f64 value, int fraction_digits,
                             const fdk_number_options *opt);

/* ---- Currency ---- */

/* Formats `amount` in the currency named by the 3-letter ISO-4217
 * `code`, using the LOCALE's separators/grouping/digits and the
 * CURRENCY's symbol, decimal digits, and symbol position (from
 * FDK's currency table; unknown codes format as the code itself,
 * 2 decimals, symbol after the amount with a space — the fallback is
 * documented, not silent).
 *
 * Examples: ("en", USD, 1234.5) → "$1,234.50"; ("de", EUR, 1234.5)
 * → "1.234,50 €"; ("fr", EUR, 1234.5) → "1 234,50 €";
 * ("ja", JPY, 12345) → "¥12,345" (JPY rounds to 0 decimals);
 * ("hi", INR, 123456.5) → "₹1,23,456.50" (Indian grouping).
 *
 * Rounding is to nearest, ties-to-even, at the currency's digit
 * count. The same magnitude limits as fdk_format_double apply.
 * 96 bytes always suffice. */
fdk_result fdk_format_currency(char *buf, size_t cap, const fdk_locale *loc,
                               fdk_f64 amount, const char *currency_code);

/* ---- Percent ---- */

/* Formats `fraction` (0.5 = 50%) with the locale's number rules and
 * percent symbol placement: "50%" (en), "50 %" (fr, de, ru),
 * "٥٠٪" (ar). Same double limits as fdk_format_double. */
fdk_result fdk_format_percent(char *buf, size_t cap, const fdk_locale *loc,
                              fdk_f64 fraction);

/* ---- Calendar ---- */

/* Proleptic-Gregorian calendar values, validated. No time zones, no
 * timestamps, no locale-era reckoning: FDK formats the calendar
 * numbers it is given. fdk_date_to_days/from_days use the exact
 * civil-days algorithm (Howard Hinnant's), correct for every date in
 * the supported range without a time_t anywhere. */

typedef struct fdk_date {
    fdk_i32 year;  /* 1..9999  */
    fdk_i32 month; /* 1..12    */
    fdk_i32 day;   /* 1..31, validated per month */
} fdk_date;

typedef struct fdk_time {
    fdk_i32 hour;   /* 0..23 */
    fdk_i32 minute; /* 0..59 */
    fdk_i32 second; /* 0..59 (leap seconds don't exist here — the
                       civil day is exactly 86400 s) */
} fdk_time;

/* Returns the number of days between 1970-01-01 and `date`
 * (negative before). Validated; FDK_ERR_INVALID_ARGUMENT on a bad
 * date (wrong month, wrong day-for-month, out of range) writes
 * nothing. Leap years are the proleptic Gregorian rule: divisible by
 * 4, except centuries not divisible by 400. */
fdk_i64 fdk_date_to_days(const fdk_date *date);

/* Inverse of fdk_date_to_days. Valid for any `days` whose date lands
 * inside 1..9999 (days -719162 = 0001-01-01 .. 2932896 = 9999-12-31);
 * outside that window fails with FDK_ERR_INVALID_ARGUMENT. */
fdk_result fdk_date_from_days(fdk_i64 days, fdk_date *out);

/* Weekday of a validated date: 0 = Monday .. 6 = Sunday (ISO-8601
 * numbering). A bad date returns -1. */
fdk_i32 fdk_date_weekday(const fdk_date *date);

bool fdk_date_is_leap_year(fdk_i32 year);
fdk_i32 fdk_date_days_in_month(fdk_i32 year, fdk_i32 month);

/* ---- Date / time formatting ---- */

typedef enum fdk_date_style {
    FDK_DATE_SHORT  = 0, /* numeric:       12/25/25, 25.12.2025    */
    FDK_DATE_MEDIUM = 1, /* abbreviated:  Dec 25, 2025, 25. Dez. 2025 */
    FDK_DATE_LONG   = 2, /* full month:   December 25, 2025        */
    FDK_DATE_FULL   = 3, /* with weekday: Thursday, December 25, 2025 */
    FDK_DATE_ISO    = 4, /* always 2025-12-25 (any locale)         */
} fdk_date_style;

typedef enum fdk_time_style {
    FDK_TIME_SHORT  = 0, /* locale default cycle: 3:30 PM / 15:30  */
    FDK_TIME_MEDIUM = 1, /* + seconds: 3:30:45 PM / 15:30:45      */
    FDK_TIME_24H    = 2, /* forced 24-hour: 15:30                  */
    FDK_TIME_12H    = 3, /* forced 12-hour: 3:30 PM                */
} fdk_time_style;

/* Formats a date in the locale's pattern for `style`. Month and
 * weekday names come from FDK's locale names table (15 languages
 * shipped; locales without names fall back to English names —
 * documented, so a Norwegian app gets "des." not garbage). 64 bytes
 * always suffice. FDK_ERR_INVALID_ARGUMENT on an invalid date or bad
 * args. */
fdk_result fdk_format_date(char *buf, size_t cap, const fdk_locale *loc,
                           const fdk_date *date, fdk_date_style style);

/* Formats a time. The 12/24-hour choice comes from the locale
 * (en 12-hour with AM/PM; de/fr/es/ru/ja/... 24-hour) unless the
 * style forces one. AM/PM strings are localized for the 12-hour
 * languages FDK ships (en, zh, ja, ko, ar, hi); forcing 12H on a
 * 24-hour locale without localized periods falls back to "AM"/"PM".
 * 48 bytes always suffice. */
fdk_result fdk_format_time(char *buf, size_t cap, const fdk_locale *loc,
                           const fdk_time *time, fdk_time_style style);

/* ---- Pluralization (CLDR plural rules) ---- */

/* The six CLDR plural categories, in precedence order for
 * documentation purposes (rule tables are mutually exclusive; the
 * order here only matters for readable printing). */
typedef enum fdk_plural_category {
    FDK_PLURAL_ZERO = 0,
    FDK_PLURAL_ONE  = 1,
    FDK_PLURAL_TWO  = 2,
    FDK_PLURAL_FEW  = 3,
    FDK_PLURAL_MANY = 4,
    FDK_PLURAL_OTHER = 5,
} fdk_plural_category;

/* The CLDR operand set, computed exactly for integers and computed
 * from the 9-decimal fixed-point rendering for doubles (a double's
 * fraction beyond 9 digits never reaches a real plural rule in any
 * language FDK covers; documented in docs/i18n.md).
 *
 *   n  absolute value of the number
 *   i  integer digits
 *   v  fraction digits WITH trailing zeros ("1.50" -> 2)
 *   w  fraction digits WITHOUT trailing zeros ("1.50" -> 1)
 *   f  fraction digits as an integer WITH trailing zeros ("1.50" -> 50)
 *   t  fraction digits as an integer WITHOUT trailing zeros ("1.50" -> 5)
 */
typedef struct fdk_plural_operands {
    fdk_f64 n;
    fdk_u64 i;
    fdk_u32 v;
    fdk_u32 w;
    fdk_u64 f;
    fdk_u64 t;
} fdk_plural_operands;

void fdk_plural_operands_from_int(fdk_i64 value,
                                  fdk_plural_operands *out);
fdk_result fdk_plural_operands_from_double(fdk_f64 value,
                                           fdk_plural_operands *out);

/* The plural category for `operands` under `loc`'s language. NULL loc
 * = root (other-only). Languages without a rule entry in FDK's table
 * behave as root: FDK_PLURAL_OTHER for everything — the same
 * fallback CLDR defines, never a guess. */
fdk_plural_category fdk_plural_category_for(
    const fdk_locale *loc, const fdk_plural_operands *operands);

/* "fdk_plural_category(loc, 3)" convenience for integer counts —
 * the overwhelmingly common call. */
fdk_plural_category fdk_plural_category_int(const fdk_locale *loc,
                                            fdk_i64 n);

/* ---- Translation catalogs ---- */

/* An immutable message catalog parsed from the .fmo format
 * (docs/fdk-catalog-format.md): msgid/msgstr pairs with optional
 * message contexts and CLDR-category plural forms. Created by parse
 * or load, destroyed by fdk_catalog_destroy. Read-only after a
 * successful parse: safe for concurrent readers. */
typedef struct fdk_catalog fdk_catalog;

/* Parses a catalog from memory. The format is strict (unknown
 * syntax, duplicate ids, oversized anything, invalid UTF-8 —
 * FDK_ERR_CATALOG_PARSE with the line number logged; the bounds are
 * 1 MiB input, 1024-byte lines, 1024-byte strings, 8192 entries).
 * No partial results: on failure *out is untouched and nothing is
 * allocated. An input with only comments/blank lines is a VALID
 * empty catalog (unlike theme files, which reject zero-byte inputs:
 * a catalog's job is lookups, and "no translations" is a legitimate
 * state — the English fallbacks handle it). */
fdk_result fdk_catalog_parse(const void *data, size_t size,
                             fdk_catalog **out);

/* Loads and parses a catalog file (same open/size-cap/read
 * discipline as fdk_theme_load; FDK_ERR_CATALOG_PARSE on a zero-byte
 * file, FDK_ERR_IO on open/seek/read failures). */
fdk_result fdk_catalog_load(const char *path, fdk_catalog **out);

void fdk_catalog_destroy(fdk_catalog *catalog);

/* Lookup. Returns the stored translation for (msgctxt, ) msgid, or
 * NULL when the catalog has no such entry (contexts via the _in_
 * _context forms; NULL msgctxt means "no context"). The returned
 * pointer is owned by the catalog and valid until destroy. */
const char *fdk_catalog_get(const fdk_catalog *catalog, const char *msgid);
const char *fdk_catalog_get_in_context(const fdk_catalog *catalog,
                                       const char *msgctxt,
                                       const char *msgid);
bool fdk_catalog_has(const fdk_catalog *catalog, const char *msgid);
size_t fdk_catalog_entry_count(const fdk_catalog *catalog);

/* Plural lookup: computes `loc`'s plural category for the count and
 * returns that category's stored form for the entry; falls back to
 * the entry's [other] form, then NULL (the entry is absent or has
 * neither form). Integer count (exact operands). */
const char *fdk_catalog_get_plural(const fdk_catalog *catalog,
                                   const fdk_locale *loc,
                                   const char *msgid, fdk_i64 n);
const char *fdk_catalog_get_plural_in_context(const fdk_catalog *catalog,
                                              const fdk_locale *loc,
                                              const char *msgctxt,
                                              const char *msgid,
                                              fdk_i64 n);

/* ---- The translate conveniences (the app-facing pattern) ----
 *
 * The classic pair. NULL/missing catalog falls back to the SOURCE
 * language (English by convention, like gettext): fdk_translate
 * returns msgid, fdk_translate_plural returns msgid for n == 1 and
 * msgid_plural otherwise — the English one/other rule applied to the
 * SOURCE, never to the target locale (a French catalog absent means
 * the app shows English, not French plurals).
 *
 * The documented composition pattern for interpolated messages:
 *
 *     const char *tmpl = fdk_translate_plural(cat, loc,
 *                                             "%d file",
 *                                             "%d files", n);
 *     snprintf(buf, sizeof buf, tmpl, n);
 *
 * FDK does not interpret placeholders inside catalog strings: the
 * catalog stores literal text, the app does its own printf against
 * it (the translator's format string IS the format string).
 */
const char *fdk_translate(const fdk_catalog *catalog, const char *msgid);
const char *fdk_translate_plural(const fdk_catalog *catalog,
                                 const fdk_locale *loc,
                                 const char *msgid,
                                 const char *msgid_plural, fdk_i64 n);

#ifdef __cplusplus
}
#endif

#endif /* FDK_I18N_H */
