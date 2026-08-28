/* test_i18n.c — Phase 10 i18n engine tests.
 *
 * Pure headless computation: every formatter, the locale parser's
 * accept/reject matrix, the calendar math (anchored to known dates),
 * the CLDR plural rule table (one pin per language shape), and the
 * message catalog parser's happy path plus the adversarial matrix
 * from docs/fdk-catalog-format.md. Runs under ASan+UBSan with the
 * rest of `make test`.
 *
 * Where a pinned string encodes a policy choice (currency symbol
 * placement, date patterns), the test documents it — the pins define
 * what FDK ships, and regressions are any change to them. */

#include "fdk/fdk_i18n.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_eq(const char *what, const char *got,
                     const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s: got \"%s\", want \"%s\"\n", what,
                got, want);
        assert(false);
    }
}

/* ---- locale parsing ---- */

static void test_locale_parse(void) {
    fdk_locale loc;

    assert(fdk_ok(fdk_locale_parse("en", &loc)));
    check_eq("en language", loc.language, "en");
    assert(loc.script[0] == '\0');
    assert(loc.territory[0] == '\0');

    assert(fdk_ok(fdk_locale_parse("de-CH", &loc)));
    check_eq("de-CH language", loc.language, "de");
    check_eq("de-CH territory", loc.territory, "CH");

    assert(fdk_ok(fdk_locale_parse("zh-Hant-TW", &loc)));
    check_eq("zh-Hant-TW script", loc.script, "Hant");
    check_eq("zh-Hant-TW territory", loc.territory, "TW");

    /* POSIX forms: underscore, charset, modifier. */
    assert(fdk_ok(fdk_locale_parse("de_CH.UTF-8", &loc)));
    check_eq("de_CH territory", loc.territory, "CH");
    assert(fdk_ok(fdk_locale_parse("zh_TW.big5", &loc)));
    check_eq("zh_TW territory", loc.territory, "TW");
    assert(fdk_ok(fdk_locale_parse("en_US@euro", &loc)));
    check_eq("en_US@euro territory", loc.territory, "US");

    /* "C" / "POSIX" = root. */
    assert(fdk_ok(fdk_locale_parse("C", &loc)));
    assert(loc.language[0] == '\0');
    assert(fdk_ok(fdk_locale_parse("POSIX", &loc)));
    assert(loc.language[0] == '\0');

    /* Case normalization. */
    assert(fdk_ok(fdk_locale_parse("EN-us", &loc)));
    check_eq("EN-us language", loc.language, "en");
    check_eq("EN-us territory", loc.territory, "US");

    /* Numeric territory. */
    assert(fdk_ok(fdk_locale_parse("es-419", &loc)));
    check_eq("es-419 territory", loc.territory, "419");

    /* Round trip. */
    fdk_locale zh;
    assert(fdk_ok(fdk_locale_parse("zh-Hant-TW", &zh)));
    char tag[32];
    assert(fdk_locale_to_tag(&zh, tag, sizeof(tag)) == 10);
    check_eq("to_tag round trip", tag, "zh-Hant-TW");

    /* Unknown-but-well-formed resolves to root rules, not error. */
    assert(fdk_ok(fdk_locale_parse("xx-YY", &loc)));
    assert(fdk_locale_parse("xx-YY", &loc) == FDK_OK);

    /* Rejections. */
    assert(fdk_locale_parse("", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("x", &loc) == FDK_ERR_INVALID_ARGUMENT); /* 1 letter */
    assert(fdk_locale_parse("e", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("toolonglanguage", &loc) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en-", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en--US", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en_US-", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("1234", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en!US", &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en-US-!", &loc) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse(NULL, &loc) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_locale_parse("en", NULL) == FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] locale: parse, normalize, round trip, rejections\n");
}

static void test_locale_rules_resolution(void) {
    fdk_locale en, en_in, de, de_ch, pt, pt_pt, xx;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("en-IN", &en_in)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("de-CH", &de_ch)));
    assert(fdk_ok(fdk_locale_parse("pt", &pt)));
    assert(fdk_ok(fdk_locale_parse("pt-PT", &pt_pt)));
    assert(fdk_ok(fdk_locale_parse("qq-ZZ", &xx)));

    /* Root == English shaping for numbers, but other-only plurals. */
    assert(fdk_plural_category_int(fdk_locale_root(), 1) ==
           FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&en, 1) == FDK_PLURAL_ONE);

    /* de-CH swaps separators. */
    char buf[64];
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &de, 1234567,
                                 NULL)));
    check_eq("de grouping", buf, "1.234.567");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &de_ch, 1234567,
                                 NULL)));
    check_eq("de-CH grouping", buf, "1'234'567");

    /* en-IN carries Indian grouping; en does not. */
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 123456789,
                                 NULL)));
    check_eq("en grouping", buf, "123,456,789");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en_in, 123456789,
                                 NULL)));
    check_eq("en-IN Indian grouping", buf, "12,34,56,789");

    /* pt vs pt-PT plural split. */
    assert(fdk_plural_category_int(&pt, 0) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&pt_pt, 0) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&pt_pt, 1) == FDK_PLURAL_ONE);

    /* Unknown tag = root rules (other-only plurals, en-ish shapes).
     */
    assert(fdk_plural_category_int(&xx, 1) == FDK_PLURAL_OTHER);
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &xx, 1234, NULL)));
    check_eq("unknown tag root shaping", buf, "1,234");

    printf("[ok] locale: rules resolution (territory overrides)\n");
}

/* ---- number formatting ---- */

static void test_format_int(void) {
    char buf[64];
    fdk_locale en, de, fr, hi, ar;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("fr", &fr)));
    assert(fdk_ok(fdk_locale_parse("hi", &hi)));
    assert(fdk_ok(fdk_locale_parse("ar", &ar)));

    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 0, NULL)));
    check_eq("en 0", buf, "0");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 1234567,
                                 NULL)));
    check_eq("en 1234567", buf, "1,234,567");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, -9876, NULL)));
    check_eq("en -9876", buf, "-9,876");

    /* INT64_MIN exactly. */
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, INT64_MIN,
                                 NULL)));
    check_eq("en INT64_MIN", buf,
             "-9,223,372,036,854,775,808");

    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &de, 1234567,
                                 NULL)));
    check_eq("de 1234567", buf, "1.234.567");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &fr, 1234567,
                                 NULL)));
    check_eq("fr 1234567", buf, "1 234 567");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &hi, 12345678,
                                 NULL)));
    check_eq("hi 12345678 (Indian)", buf, "1,23,45,678");
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &ar, 1234567,
                                 NULL)));
    check_eq("ar 1234567 (Arabic digits, U+066C groups)", buf,
             "١٬٢٣٤٬٥٦٧");

    /* Options: no grouping, forced sign, forced minimum fraction. */
    fdk_number_options opt = {0};
    opt.use_grouping = -1;
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 1234567,
                                 &opt)));
    check_eq("en no grouping", buf, "1234567");
    fdk_number_options sign_opt = {0};
    sign_opt.sign_always = 1;
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 42,
                                 &sign_opt)));
    check_eq("en forced sign", buf, "+42");
    fdk_number_options frac_opt = {0};
    frac_opt.min_fraction_digits = 2;
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), &en, 5, &frac_opt)));
    check_eq("en forced fraction", buf, "5.00");

    /* Buffer discipline. */
    assert(fdk_format_int(buf, 4, &en, 1234567, NULL) ==
           FDK_ERR_LIMIT);
    assert(fdk_format_int(NULL, 64, &en, 1, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_int(buf, 0, &en, 1, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    /* NULL locale = root (en-like). */
    assert(fdk_ok(fdk_format_int(buf, sizeof(buf), NULL, 1234, NULL)));
    check_eq("NULL locale = root", buf, "1,234");

    printf("[ok] format_int: separators, grouping, digits, options\n");
}

static void test_format_double(void) {
    char buf[64];
    fdk_locale en, de, ar;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("ar", &ar)));

    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &en, 1234.5, 2,
                                    NULL)));
    check_eq("en 1234.5", buf, "1,234.50");
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &de, 1234.5, 2,
                                    NULL)));
    check_eq("de 1234.5", buf, "1.234,50");
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &de, -0.125, 2,
                                    NULL)));
    check_eq("de -0.125 ties-to-even", buf, "-0,12");

    /* Rounding half-even on exact binary ties. */
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &en, 0.125, 2,
                                    NULL)));
    check_eq("en 0.125 ties-to-even", buf, "0.12");
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &en, 0.375, 2,
                                    NULL)));
    check_eq("en 0.375 ties-to-even", buf, "0.38");

    /* Arabic digits, U+066C group, U+066B decimal. */
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &ar, 1234.5, 2,
                                    NULL)));
    check_eq("ar 1234.5", buf, "١٬٢٣٤٫٥٠");

    /* Bounds discipline. */
    assert(fdk_format_double(buf, sizeof(buf), &en, 1e15, 2, NULL) ==
           FDK_ERR_UNSUPPORTED);
    assert(fdk_format_double(buf, sizeof(buf), &en, 1e-10, 2, NULL) ==
           FDK_ERR_UNSUPPORTED);
    assert(fdk_format_double(buf, sizeof(buf), &en, 0.0 / 0.0, 2,
                             NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_double(buf, sizeof(buf), &en, 1.0 / 0.0, 2,
                             NULL) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_double(buf, sizeof(buf), &en, 1.0, -1, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_double(buf, sizeof(buf), &en, 1.0, 10, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Options: max clamps the requested fraction count (0 = unset
     * per the input-struct convention). */
    fdk_number_options opt = {0};
    opt.max_fraction_digits = 1;
    assert(fdk_ok(fdk_format_double(buf, sizeof(buf), &en, 1234.567,
                                    2, &opt)));
    check_eq("en max_fraction clamps", buf, "1,234.6");

    printf("[ok] format_double: rounding, separators, limits\n");
}

static void test_format_currency(void) {
    char buf[96];
    fdk_locale en, de, fr, ja, hi;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("fr", &fr)));
    assert(fdk_ok(fdk_locale_parse("ja", &ja)));
    assert(fdk_ok(fdk_locale_parse("hi", &hi)));

    /* Symbol position policy: the currency's home convention. */
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &en, 1234.5,
                                      "USD")));
    check_eq("en USD", buf, "$1,234.50");
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &de, 1234.5,
                                      "EUR")));
    check_eq("de EUR", buf, "1.234,50 €");
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &fr, 1234.5,
                                      "EUR")));
    check_eq("fr EUR", buf, "1 234,50 €");
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &ja, 12345,
                                      "JPY")));
    check_eq("ja JPY (0 decimals)", buf, "¥12,345");
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &hi, 123456.5,
                                      "INR")));
    check_eq("hi INR (Indian grouping)", buf, "₹1,23,456.50");

    /* 3-decimal currency; KWD's home convention puts the symbol
     * after with a space. */
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &en, 1234.5678,
                                      "KWD")));
    check_eq("en KWD (3 decimals)", buf, "1,234.568 د.ك");

    /* Negative: sign before the symbol. */
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &en, -42.25,
                                      "USD")));
    check_eq("en negative USD", buf, "-$42.25");

    /* Case-insensitive code; unknown-but-valid code falls back. */
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &en, 5.0,
                                      "usd")));
    check_eq("en usd (case)", buf, "$5.00");
    assert(fdk_ok(fdk_format_currency(buf, sizeof(buf), &en, 1234.5,
                                      "XYZ")));
    check_eq("en unknown code", buf, "1,234.50 XYZ");

    /* Garbage codes are argument errors. */
    assert(fdk_format_currency(buf, sizeof(buf), &en, 1.0, "US") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_currency(buf, sizeof(buf), &en, 1.0, "USDD") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_currency(buf, sizeof(buf), &en, 1.0, "U!") ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_format_currency(buf, sizeof(buf), &en, 1.0, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] format_currency: table, positions, fallbacks\n");
}

static void test_format_percent(void) {
    char buf[64];
    fdk_locale en, fr, ar;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("fr", &fr)));
    assert(fdk_ok(fdk_locale_parse("ar", &ar)));

    assert(fdk_ok(fdk_format_percent(buf, sizeof(buf), &en, 0.5)));
    check_eq("en 50%", buf, "50%");
    assert(fdk_ok(fdk_format_percent(buf, sizeof(buf), &fr, 0.5)));
    check_eq("fr 50 %", buf, "50 %");
    assert(fdk_ok(fdk_format_percent(buf, sizeof(buf), &en, -0.25)));
    check_eq("en negative", buf, "-25%");
    assert(fdk_ok(fdk_format_percent(buf, sizeof(buf), &ar, 0.5)));
    check_eq("ar ٥٠٪", buf, "٥٠٪");
    /* Round to integer percent. */
    assert(fdk_ok(fdk_format_percent(buf, sizeof(buf), &en, 0.12345)));
    check_eq("en rounds", buf, "12%");

    printf("[ok] format_percent: placement and rounding\n");
}

/* ---- calendar ---- */

static void test_calendar(void) {
    /* Anchored to known weekdays. */
    fdk_date d = {1970, 1, 1};
    assert(fdk_date_to_days(&d) == 0);
    assert(fdk_date_weekday(&d) == 3); /* Thursday, Mon = 0 */

    d = (fdk_date){1969, 12, 31};
    assert(fdk_date_to_days(&d) == -1);
    assert(fdk_date_weekday(&d) == 2); /* Wednesday */

    d = (fdk_date){2000, 1, 1};
    assert(fdk_date_weekday(&d) == 5); /* Saturday */

    d = (fdk_date){2024, 1, 1};
    assert(fdk_date_weekday(&d) == 0); /* Monday */

    d = (fdk_date){2025, 12, 25};
    assert(fdk_date_weekday(&d) == 3); /* Thursday */

    /* Round trips across the range. */
    for (fdk_i64 days = -719162; days <= 2932896;
         days += 971) { /* whole range, ~3033 samples */
        fdk_date out;
        assert(fdk_ok(fdk_date_from_days(days, &out)));
        assert(fdk_date_to_days(&out) == days);
    }
    fdk_date edge;
    assert(fdk_ok(fdk_date_from_days(-719162, &edge)));
    assert(edge.year == 1 && edge.month == 1 && edge.day == 1);
    assert(fdk_ok(fdk_date_from_days(2932896, &edge)));
    assert(edge.year == 9999 && edge.month == 12 && edge.day == 31);
    assert(fdk_date_from_days(2932897, &edge) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_date_from_days(-719163, &edge) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Leap years. */
    assert(fdk_date_is_leap_year(2000));
    assert(!fdk_date_is_leap_year(1900));
    assert(fdk_date_is_leap_year(2024));
    assert(!fdk_date_is_leap_year(2023));
    assert(fdk_date_days_in_month(2024, 2) == 29);
    assert(fdk_date_days_in_month(2023, 2) == 28);
    assert(fdk_date_days_in_month(2023, 1) == 31);
    assert(fdk_date_days_in_month(2023, 4) == 30);

    /* Validation. */
    fdk_date bad = {2023, 2, 29};
    assert(fdk_date_to_days(&bad) == 0); /* rejected -> 0 sentinel */
    assert(fdk_date_weekday(&bad) == -1);
    bad = (fdk_date){2023, 13, 1};
    assert(fdk_date_weekday(&bad) == -1);
    bad = (fdk_date){2023, 0, 1};
    assert(fdk_date_weekday(&bad) == -1);
    bad = (fdk_date){2023, 1, 0};
    assert(fdk_date_weekday(&bad) == -1);
    bad = (fdk_date){0, 1, 1};
    assert(fdk_date_weekday(&bad) == -1);
    bad = (fdk_date){10000, 1, 1};
    assert(fdk_date_weekday(&bad) == -1);

    printf("[ok] calendar: epochs, weekdays, leap, round trips\n");
}

static void test_format_date(void) {
    char buf[96];
    fdk_locale en, de, fr, ja, ru, hi;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("fr", &fr)));
    assert(fdk_ok(fdk_locale_parse("ja", &ja)));
    assert(fdk_ok(fdk_locale_parse("ru", &ru)));
    assert(fdk_ok(fdk_locale_parse("hi", &hi)));

    fdk_date d = {2025, 12, 25};

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &en, &d,
                                  FDK_DATE_SHORT)));
    check_eq("en short", buf, "12/25/25");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &en, &d,
                                  FDK_DATE_MEDIUM)));
    check_eq("en medium", buf, "Dec 25, 2025");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &en, &d,
                                  FDK_DATE_LONG)));
    check_eq("en long", buf, "December 25, 2025");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &en, &d,
                                  FDK_DATE_FULL)));
    check_eq("en full", buf, "Thursday, December 25, 2025");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &de, &d,
                                  FDK_DATE_ISO)));
    check_eq("ISO", buf, "2025-12-25");

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &de, &d,
                                  FDK_DATE_SHORT)));
    check_eq("de short", buf, "25.12.2025");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &de, &d,
                                  FDK_DATE_LONG)));
    check_eq("de long", buf, "25. Dezember 2025");

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &fr, &d,
                                  FDK_DATE_MEDIUM)));
    check_eq("fr medium", buf, "25 déc. 2025");

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &ja, &d,
                                  FDK_DATE_LONG)));
    check_eq("ja long", buf, "2025年12月25日");

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &ru, &d,
                                  FDK_DATE_MEDIUM)));
    check_eq("ru medium", buf, "25 дек. 2025");
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &ru, &d,
                                  FDK_DATE_LONG)));
    check_eq("ru long (г.)", buf, "25 декабря 2025 г.");

    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &hi, &d,
                                  FDK_DATE_FULL)));
    check_eq("hi full", buf, "गुरुवार, 25 दिसंबर 2025");

    /* Two-digit years and single-digit months/days zero-pad per
     * pattern width. */
    fdk_date small = {2005, 3, 7};
    assert(fdk_ok(fdk_format_date(buf, sizeof(buf), &en, &small,
                                  FDK_DATE_SHORT)));
    check_eq("en short 2005-03-07", buf, "3/7/05");

    /* Invalid date rejected. */
    fdk_date bad = {2023, 2, 30};
    assert(fdk_format_date(buf, sizeof(buf), &en, &bad,
                           FDK_DATE_SHORT) == FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] format_date: styles, localized names, validation\n");
}

static void test_format_time(void) {
    char buf[64];
    fdk_locale en, de, ja, ko;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));
    assert(fdk_ok(fdk_locale_parse("ja", &ja)));
    assert(fdk_ok(fdk_locale_parse("ko", &ko)));

    fdk_time t = {15, 30, 45};

    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &en, &t,
                                  FDK_TIME_SHORT)));
    check_eq("en short", buf, "3:30 PM");
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &en, &t,
                                  FDK_TIME_MEDIUM)));
    check_eq("en medium", buf, "3:30:45 PM");
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &de, &t,
                                  FDK_TIME_SHORT)));
    check_eq("de short", buf, "15:30");

    /* Forced cycles. */
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &en, &t,
                                  FDK_TIME_24H)));
    check_eq("en forced 24h", buf, "15:30");
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &de, &t,
                                  FDK_TIME_12H)));
    check_eq("de forced 12h (AM/PM fallback)", buf, "3:30 PM");
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &ja, &t,
                                  FDK_TIME_12H)));
    check_eq("ja forced 12h", buf, "午後3:30");

    /* Midnight / noon. */
    fdk_time midnight = {0, 0, 0};
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &en, &midnight,
                                  FDK_TIME_SHORT)));
    check_eq("en midnight", buf, "12:00 AM");
    fdk_time noon = {12, 0, 0};
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &en, &noon,
                                  FDK_TIME_SHORT)));
    check_eq("en noon", buf, "12:00 PM");

    /* ko's native cycle is 12h (오전/오후). */
    assert(fdk_ok(fdk_format_time(buf, sizeof(buf), &ko, &t,
                                  FDK_TIME_SHORT)));
    check_eq("ko short", buf, "오후 3:30");

    /* Validation. */
    fdk_time bad = {24, 0, 0};
    assert(fdk_format_time(buf, sizeof(buf), &en, &bad,
                           FDK_TIME_SHORT) == FDK_ERR_INVALID_ARGUMENT);
    bad = (fdk_time){12, 60, 0};
    assert(fdk_format_time(buf, sizeof(buf), &en, &bad,
                           FDK_TIME_SHORT) == FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] format_time: cycles, periods, validation\n");
}

/* ---- pluralization ---- */

static void test_plural_operands(void) {
    fdk_plural_operands op;

    fdk_plural_operands_from_int(1, &op);
    assert(op.n == 1.0 && op.i == 1 && op.v == 0 && op.w == 0 &&
           op.f == 0 && op.t == 0);

    fdk_plural_operands_from_int(-42, &op);
    assert(op.n == 42.0 && op.i == 42);

    fdk_plural_operands_from_int(INT64_MIN, &op);
    assert(op.i == 9223372036854775808ull);

    assert(fdk_ok(fdk_plural_operands_from_double(1.5, &op)));
    assert(op.i == 1 && op.f == 5 && op.v == 1 && op.w == 1 &&
           op.t == 5);

    /* A double cannot carry written trailing zeros: 1.50 IS 1.5. */
    assert(fdk_ok(fdk_plural_operands_from_double(1.50, &op)));
    assert(op.f == 5 && op.v == 1 && op.w == 1 && op.t == 5);

    assert(fdk_ok(fdk_plural_operands_from_double(0.05, &op)));
    assert(op.i == 0 && op.f == 5 && op.v == 2 && op.w == 2);

    assert(fdk_ok(fdk_plural_operands_from_double(1.0, &op)));
    assert(op.v == 0 && op.f == 0);

    assert(fdk_plural_operands_from_double(0.0 / 0.0, &op) ==
           FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] plural operands: n/i/v/w/f/t\n");
}

static void test_plural_categories(void) {
    fdk_locale en, fr, ru, pl, cs, ar, lv, lt, hr, ja, de;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("fr", &fr)));
    assert(fdk_ok(fdk_locale_parse("ru", &ru)));
    assert(fdk_ok(fdk_locale_parse("pl", &pl)));
    assert(fdk_ok(fdk_locale_parse("cs", &cs)));
    assert(fdk_ok(fdk_locale_parse("ar", &ar)));
    assert(fdk_ok(fdk_locale_parse("lv", &lv)));
    assert(fdk_ok(fdk_locale_parse("lt", &lt)));
    assert(fdk_ok(fdk_locale_parse("hr", &hr)));
    assert(fdk_ok(fdk_locale_parse("ja", &ja)));
    assert(fdk_ok(fdk_locale_parse("de", &de)));

    /* en / de: one only for exact 1. */
    assert(fdk_plural_category_int(&en, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&en, 0) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&en, 2) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&en, 21) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&de, 1) == FDK_PLURAL_ONE);
    fdk_plural_operands op;
    assert(fdk_ok(fdk_plural_operands_from_double(1.5, &op)));
    assert(fdk_plural_category_for(&en, &op) == FDK_PLURAL_OTHER);

    /* fr: one for 0 and 1 (and 1.5 via i). */
    assert(fdk_plural_category_int(&fr, 0) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&fr, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&fr, 2) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_for(&fr, &op) == FDK_PLURAL_ONE);

    /* ru: 1/21 one; 2-4/22-24 few; 0,5-20,11-14,25.. many; frac other. */
    assert(fdk_plural_category_int(&ru, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&ru, 21) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&ru, 31) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&ru, 2) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&ru, 22) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&ru, 4) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&ru, 0) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ru, 5) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ru, 11) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ru, 12) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ru, 25) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ru, 100) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_for(&ru, &op) == FDK_PLURAL_OTHER);

    /* pl: one is EXACTLY 1; 21 is many; fractions other. */
    assert(fdk_plural_category_int(&pl, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&pl, 21) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&pl, 2) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&pl, 22) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&pl, 5) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&pl, 12) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&pl, 0) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_for(&pl, &op) == FDK_PLURAL_OTHER);

    /* cs: many = fractions; 0 and 5+ other. */
    assert(fdk_plural_category_int(&cs, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&cs, 2) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&cs, 4) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&cs, 5) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&cs, 0) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_for(&cs, &op) == FDK_PLURAL_MANY);

    /* ar: the full six. */
    assert(fdk_plural_category_int(&ar, 0) == FDK_PLURAL_ZERO);
    assert(fdk_plural_category_int(&ar, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&ar, 2) == FDK_PLURAL_TWO);
    assert(fdk_plural_category_int(&ar, 3) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&ar, 10) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&ar, 11) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ar, 99) == FDK_PLURAL_MANY);
    assert(fdk_plural_category_int(&ar, 100) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&ar, 101) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&ar, 103) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_for(&ar, &op) == FDK_PLURAL_OTHER);

    /* lv: zero/one/other. */
    assert(fdk_plural_category_int(&lv, 0) == FDK_PLURAL_ZERO);
    assert(fdk_plural_category_int(&lv, 10) == FDK_PLURAL_ZERO);
    assert(fdk_plural_category_int(&lv, 11) == FDK_PLURAL_ZERO);
    assert(fdk_plural_category_int(&lv, 15) == FDK_PLURAL_ZERO);
    assert(fdk_plural_category_int(&lv, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&lv, 21) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&lv, 2) == FDK_PLURAL_OTHER);

    /* lt: many = fractions; one/few per last digits. */
    assert(fdk_plural_category_int(&lt, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&lt, 21) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&lt, 2) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&lt, 9) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&lt, 10) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&lt, 11) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&lt, 20) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_for(&lt, &op) == FDK_PLURAL_MANY);

    /* hr: one/few/other. */
    assert(fdk_plural_category_int(&hr, 1) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&hr, 21) == FDK_PLURAL_ONE);
    assert(fdk_plural_category_int(&hr, 2) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&hr, 22) == FDK_PLURAL_FEW);
    assert(fdk_plural_category_int(&hr, 5) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&hr, 11) == FDK_PLURAL_OTHER);

    /* ja: other only. */
    assert(fdk_plural_category_int(&ja, 1) == FDK_PLURAL_OTHER);
    assert(fdk_plural_category_int(&ja, 0) == FDK_PLURAL_OTHER);

    printf("[ok] plural: CLDR categories per language shape\n");
}

/* ---- catalogs ---- */

static const char *const GOOD_CATALOG =
    "# FDK message catalog\n"
    "\n"
    "msgid \"hello\"\n"
    "msgstr \"Hallo\"\n"
    "\n"
    "msgctxt \"menu\"\n"
    "msgid \"open\"\n"
    "msgstr \"Öffnen\"\n"
    "\n"
    "msgctxt \"door\"\n"
    "msgid \"open\"\n"
    "msgstr \"offen\"\n"
    "\n"
    "msgid \"files\"\n"
    "msgid_plural \"%d files\"\n"
    "msgstr[one] \"%d Datei\"\n"
    "msgstr[other] \"%d Dateien\"\n"
    "\n"
    "// line comment\n"
    "msgid \"escaped\"\n"
    "msgstr \"line\\nbreak \\\"quoted\\\" tab\\there\"\n"
    "msgid \"unicode\"\n"
    "msgstr \"Русский текст — 中文 — العربية\"\n";

static void test_catalog_parse_and_lookup(void) {
    fdk_catalog *cat = NULL;
    fdk_result r = fdk_catalog_parse(GOOD_CATALOG,
                                     strlen(GOOD_CATALOG), &cat);
    assert(fdk_ok(r));
    assert(cat != NULL);
    assert(fdk_catalog_entry_count(cat) == 6);

    check_eq("plain get", fdk_catalog_get(cat, "hello"), "Hallo");
    check_eq("ctx menu", fdk_catalog_get_in_context(cat, "menu",
                                                    "open"),
             "Öffnen");
    check_eq("ctx door", fdk_catalog_get_in_context(cat, "door",
                                                    "open"),
             "offen");
    assert(fdk_catalog_get(cat, "missing") == NULL);
    assert(fdk_catalog_get(cat, "open") == NULL); /* ctx-only entry */
    assert(fdk_catalog_has(cat, "hello"));
    assert(!fdk_catalog_has(cat, "nope"));
    check_eq("escapes", fdk_catalog_get(cat, "escaped"),
             "line\nbreak \"quoted\" tab\there");
    check_eq("utf-8 stored", fdk_catalog_get(cat, "unicode"),
             "Русский текст — 中文 — العربية");

    /* Plural selection per locale. */
    fdk_locale en, ru;
    assert(fdk_ok(fdk_locale_parse("en", &en)));
    assert(fdk_ok(fdk_locale_parse("ru", &ru)));
    check_eq("plural n=1 (en one)", fdk_catalog_get_plural(cat, &en,
                                                           "files", 1),
             "%d Datei");
    check_eq("plural n=2 (en other)",
             fdk_catalog_get_plural(cat, &en, "files", 2),
             "%d Dateien");
    /* ru has one/few/many/other; the catalog stores one/other, so
     * non-one falls to other. */
    check_eq("plural n=5 (ru falls to other)",
             fdk_catalog_get_plural(cat, &ru, "files", 5),
             "%d Dateien");
    /* ru n=1 hits [one]. */
    check_eq("plural n=1 (ru one)", fdk_catalog_get_plural(cat, &ru,
                                                           "files", 1),
             "%d Datei");

    /* Translate conveniences: NULL catalog = source fallback. */
    check_eq("translate NULL cat", fdk_translate(NULL, "hello"),
             "hello");
    check_eq("translate hit", fdk_translate(cat, "hello"), "Hallo");
    check_eq("translate miss", fdk_translate(cat, "nope"), "nope");
    check_eq("translate_plural NULL cat n=1",
             fdk_translate_plural(NULL, &en, "%d file", "%d files", 1),
             "%d file");
    check_eq("translate_plural NULL cat n=2",
             fdk_translate_plural(NULL, &en, "%d file", "%d files", 2),
             "%d files");
    check_eq("translate_plural miss n=0 (en other)",
             fdk_translate_plural(cat, &en, "?", "??", 0),
             "??");

    fdk_catalog_destroy(cat);

    /* Comment-only input is a valid empty catalog. */
    static const char *comments_only = "# nothing here\n\n";
    r = fdk_catalog_parse(comments_only, strlen(comments_only), &cat);
    assert(fdk_ok(r));
    assert(fdk_catalog_entry_count(cat) == 0);
    fdk_catalog_destroy(cat);

    /* Adjacent entries with no separator line commit correctly. */
    static const char *adjacent =
        "msgid \"a\"\nmsgstr \"A\"\nmsgid \"b\"\nmsgstr \"B\"\n";
    r = fdk_catalog_parse(adjacent, strlen(adjacent), &cat);
    assert(fdk_ok(r));
    assert(fdk_catalog_entry_count(cat) == 2);
    check_eq("adjacent a", fdk_catalog_get(cat, "a"), "A");
    check_eq("adjacent b", fdk_catalog_get(cat, "b"), "B");
    fdk_catalog_destroy(cat);

    printf("[ok] catalog: parse, contexts, plurals, fallbacks\n");
}

static void test_catalog_adversarial(void) {
    fdk_catalog *cat = NULL;

    struct {
        const char *name;
        const char *text;
    } bad[] = {
        {"unknown keyword", "msgtxt \"a\"\nmsgstr \"b\"\n"},
        {"missing msgstr", "msgid \"a\"\n"},
        {"unterminated string", "msgid \"a\nmsgstr \"b\"\n"},
        {"bad escape", "msgid \"a\"\nmsgstr \"b\\q\"\n"},
        {"empty msgid", "msgid \"\"\nmsgstr \"b\"\n"},
        {"duplicate msgid", "msgid \"a\"\nmsgstr \"b\"\n"
                            "msgid \"a\"\nmsgstr \"c\"\n"},
        {"duplicate ctx msgid", "msgctxt \"m\"\nmsgid \"a\"\n"
                                "msgstr \"b\"\nmsgctxt \"m\"\n"
                                "msgid \"a\"\nmsgstr \"c\"\n"},
        {"plural without forms", "msgid \"a\"\nmsgid_plural \"as\"\n"},
        {"msgstr[cat] without plural", "msgid \"a\"\n"
                                       "msgstr[one] \"b\"\n"},
        {"duplicate category", "msgid \"a\"\nmsgid_plural \"as\"\n"
                                "msgstr[one] \"b\"\n"
                                "msgstr[one] \"c\"\n"},
        {"trailing garbage", "msgid \"a\" extra\nmsgstr \"b\"\n"},
        {"garbage line", "hello there\n"},
        {"msgctxt mid-entry", "msgid \"a\"\nmsgctxt \"m\"\n"
                              "msgstr \"b\"\n"},
        {"bad category name", "msgid \"a\"\nmsgid_plural \"as\"\n"
                              "msgstr[some] \"b\"\n"},
        {"empty string as msgid", "msgid \"\""},
        {"comment inside entry", "msgid \"a\"\n# oops\n"
                                 "msgstr \"b\"\n"},
        {"msgid then msgid", "msgid \"a\"\nmsgid \"b\"\n"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cat = NULL;
        fdk_result r = fdk_catalog_parse(bad[i].text,
                                         strlen(bad[i].text), &cat);
        if (fdk_ok(r)) {
            fprintf(stderr, "FAIL: %s should have been rejected\n",
                    bad[i].name);
            assert(false);
        }
        assert(r == FDK_ERR_CATALOG_PARSE || r == FDK_ERR_INVALID_ARGUMENT);
        assert(cat == NULL); /* no partial results */
    }

    /* Oversized line (1025 bytes). */
    char big[1100];
    memset(big, 'x', sizeof(big));
    memcpy(big, "msgid \"", 7);
    big[1024] = '\n';
    big[1025] = '\0';
    /* one line of >1024 chars */
    assert(fdk_catalog_parse(big, strlen(big), &cat) ==
           FDK_ERR_CATALOG_PARSE);

    /* Invalid UTF-8 (0xFF byte). */
    static const char raw[8] = {'m', 's', 'g', 'i', 'd', ' ',
                                (char)0xFF, '\0'};
    static const char rest[] = "\"x\"\nmsgstr \"y\"\n";
    char joined[32];
    memcpy(joined, raw, 7);
    joined[7] = '"';
    memcpy(joined + 8, "x\"\nmsgstr \"y\"\n", 14);
    (void)rest;
    assert(fdk_catalog_parse(joined, 8 + 14, &cat) ==
           FDK_ERR_CATALOG_PARSE);

    /* NULL/zero handling. */
    assert(fdk_catalog_parse("x", 1, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_catalog_parse(NULL, 5, &cat) ==
           FDK_ERR_INVALID_ARGUMENT);
    cat = NULL;
    assert(fdk_ok(fdk_catalog_parse(NULL, 0, &cat)));
    assert(fdk_catalog_entry_count(cat) == 0);
    fdk_catalog_destroy(cat);
    fdk_catalog_destroy(NULL); /* safe no-op */

    printf("[ok] catalog: %zu adversarial rejections, no partial "
           "results\n",
           sizeof(bad) / sizeof(bad[0]) + 4);
}

static void test_catalog_load(void) {
    /* Write a real file, load it, verify, then exercise the IO
     * failure paths. */
    const char *path = "/tmp/fdk-test-catalog.fmo";
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fputs("msgid \"from-file\"\nmsgstr \"Aus Datei\"\n", f);
    fclose(f);

    fdk_catalog *cat = NULL;
    assert(fdk_ok(fdk_catalog_load(path, &cat)));
    check_eq("file load", fdk_catalog_get(cat, "from-file"),
             "Aus Datei");
    fdk_catalog_destroy(cat);

    /* Nonexistent path. */
    assert(fdk_catalog_load("/tmp/fdk-does-not-exist.fmo", &cat) ==
           FDK_ERR_IO);

    /* Empty file. */
    f = fopen(path, "wb");
    assert(f != NULL);
    fclose(f);
    assert(fdk_catalog_load(path, &cat) == FDK_ERR_CATALOG_PARSE);

    /* NULL path. */
    assert(fdk_catalog_load(NULL, &cat) == FDK_ERR_INVALID_ARGUMENT);

    remove(path);
    printf("[ok] catalog: file load + IO failure paths\n");
}

int main(void) {
    test_locale_parse();
    test_locale_rules_resolution();
    test_format_int();
    test_format_double();
    test_format_currency();
    test_format_percent();
    test_calendar();
    test_format_date();
    test_format_time();
    test_plural_operands();
    test_plural_categories();
    test_catalog_parse_and_lookup();
    test_catalog_adversarial();
    test_catalog_load();
    printf("all i18n tests passed\n");
    return 0;
}
