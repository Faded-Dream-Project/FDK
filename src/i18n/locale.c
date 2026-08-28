#define FDK_LOG_TAG "i18n"

/*
 * locale.c — locale parsing and rules resolution
 *
 * The rules table is FDK's curated CLDR subset: separators, grouping
 * shape, percent placement, clock cycle, digit system, date/time
 * patterns, month/weekday names, and the plural rule — one row per
 * (language, territory) override plus one per language plus the root.
 * The names tables live here too (15 languages; the roadmap records
 * the list and the honest fallback for the rest).
 *
 * Parsing is hand-rolled per the module discipline (no sscanf, no
 * strtol): everything is bounded ASCII classification, and a tag that
 * is well-formed but unknown RESOLVES to root rules rather than
 * failing — an unrecognized territory is a data gap, not a syntax
 * error.
 */

#include "i18n_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

/* ---- names tables ----
 *
 * Weekday index 0 = Monday (ISO-8601), everywhere.
 * Abbreviations follow CLDR's abbreviated (not short) variants.
 *
 * MONTH INFLECTION: languages where dates use an inflected month
 * form (Russian, Polish, Ukrainian genitive) store the FORMAT
 * forms CLDR's date patterns select — "25 декабря", not the
 * standalone "декабрь". FDK's engine only formats dates, so the
 * standalone variants have no consumer yet. */

static const fdk_i18n_names fdk__i18n_names_en_static = {
    .months_long = {"January", "February", "March", "April", "May",
                    "June", "July", "August", "September", "October",
                    "November", "December"},
    .months_abbr = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
                    "Aug", "Sept", "Oct", "Nov", "Dec"},
    .days_long = {"Monday", "Tuesday", "Wednesday", "Thursday",
                  "Friday", "Saturday", "Sunday"},
    .days_abbr = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"},
    .am = "AM",
    .pm = "PM",
};
const fdk_i18n_names fdk__i18n_names_en = fdk__i18n_names_en_static;

static const fdk_i18n_names names_de = {
    .months_long = {"Januar", "Februar", "März", "April", "Mai", "Juni",
                    "Juli", "August", "September", "Oktober", "November",
                    "Dezember"},
    .months_abbr = {"Jan.", "Feb.", "März", "Apr.", "Mai", "Juni",
                    "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez."},
    .days_long = {"Montag", "Dienstag", "Mittwoch", "Donnerstag",
                  "Freitag", "Samstag", "Sonntag"},
    .days_abbr = {"Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa.", "So."},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_fr = {
    .months_long = {"janvier", "février", "mars", "avril", "mai",
                    "juin", "juillet", "août", "septembre", "octobre",
                    "novembre", "décembre"},
    .months_abbr = {"janv.", "févr.", "mars", "avr.", "mai", "juin",
                    "juil.", "août", "sept.", "oct.", "nov.", "déc."},
    .days_long = {"lundi", "mardi", "mercredi", "jeudi", "vendredi",
                  "samedi", "dimanche"},
    .days_abbr = {"lun.", "mar.", "mer.", "jeu.", "ven.", "sam.",
                  "dim."},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_es = {
    .months_long = {"enero", "febrero", "marzo", "abril", "mayo",
                    "junio", "julio", "agosto", "septiembre",
                    "octubre", "noviembre", "diciembre"},
    .months_abbr = {"ene.", "feb.", "mar.", "abr.", "may.", "jun.",
                    "jul.", "ago.", "sept.", "oct.", "nov.", "dic."},
    .days_long = {"lunes", "martes", "miércoles", "jueves", "viernes",
                  "sábado", "domingo"},
    .days_abbr = {"lun.", "mar.", "mié.", "jue.", "vie.", "sáb.",
                  "dom."},
    .am = "a.m.", .pm = "p.m.",
};

static const fdk_i18n_names names_it = {
    .months_long = {"gennaio", "febbraio", "marzo", "aprile",
                    "maggio", "giugno", "luglio", "agosto",
                    "settembre", "ottobre", "novembre", "dicembre"},
    .months_abbr = {"gen", "feb", "mar", "apr", "mag", "giu", "lug",
                    "ago", "set", "ott", "nov", "dic"},
    .days_long = {"lunedì", "martedì", "mercoledì", "giovedì",
                  "venerdì", "sabato", "domenica"},
    .days_abbr = {"lun", "mar", "mer", "gio", "ven", "sab", "dom"},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_pt = {
    .months_long = {"janeiro", "fevereiro", "março", "abril", "maio",
                    "junho", "julho", "agosto", "setembro", "outubro",
                    "novembro", "dezembro"},
    .months_abbr = {"jan.", "fev.", "mar.", "abr.", "mai.", "jun.",
                    "jul.", "ago.", "set.", "out.", "nov.", "dez."},
    .days_long = {"segunda-feira", "terça-feira", "quarta-feira",
                  "quinta-feira", "sexta-feira", "sábado", "domingo"},
    .days_abbr = {"seg.", "ter.", "qua.", "qui.", "sex.", "sáb.",
                  "dom."},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_nl = {
    .months_long = {"januari", "februari", "maart", "april", "mei",
                    "juni", "juli", "augustus", "september",
                    "oktober", "november", "december"},
    .months_abbr = {"jan.", "feb.", "mrt.", "apr.", "mei", "jun.",
                    "jul.", "aug.", "sep.", "okt.", "nov.", "dec."},
    .days_long = {"maandag", "dinsdag", "woensdag", "donderdag",
                  "vrijdag", "zaterdag", "zondag"},
    .days_abbr = {"ma", "di", "wo", "do", "vr", "za", "zo"},
    .am = "a.m.", .pm = "p.m.",
};

static const fdk_i18n_names names_sv = {
    .months_long = {"januari", "februari", "mars", "april", "maj",
                    "juni", "juli", "augusti", "september", "oktober",
                    "november", "december"},
    .months_abbr = {"jan.", "feb.", "mars", "apr.", "maj", "juni",
                    "juli", "aug.", "sep.", "okt.", "nov.", "dec."},
    .days_long = {"måndag", "tisdag", "onsdag", "torsdag", "fredag",
                  "lördag", "söndag"},
    .days_abbr = {"mån.", "tis.", "ons.", "tors.", "fre.", "lör.",
                  "sön."},
    .am = "FM", .pm = "EM",
};

static const fdk_i18n_names names_ru = {
    /* Format (genitive) months: "25 декабря 2025 г." */
    .months_long = {"января", "февраля", "марта", "апреля", "мая",
                    "июня", "июля", "августа", "сентября",
                    "октября", "ноября", "декабря"},
    .months_abbr = {"янв.", "февр.", "мар.", "апр.", "мая", "июн.",
                    "июл.", "авг.", "сент.", "окт.", "нояб.", "дек."},
    .days_long = {"понедельник", "вторник", "среда", "четверг",
                  "пятница", "суббота", "воскресенье"},
    .days_abbr = {"пн", "вт", "ср", "чт", "пт", "сб", "вс"},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_pl = {
    /* Format (genitive) months: "25 grudnia 2025" */
    .months_long = {"stycznia", "lutego", "marca", "kwietnia",
                    "maja", "czerwca", "lipca", "sierpnia",
                    "września", "października", "listopada",
                    "grudnia"},
    .months_abbr = {"sty.", "lut.", "mar.", "kwi.", "maj", "cze.",
                    "lip.", "sie.", "wrz.", "paź.", "lis.", "gru."},
    .days_long = {"poniedziałek", "wtorek", "środa", "czwartek",
                  "piątek", "sobota", "niedziela"},
    .days_abbr = {"pon.", "wt.", "śr.", "czw.", "pt.", "sob.",
                  "niedz."},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_uk = {
    /* Format (genitive) months */
    .months_long = {"січня", "лютого", "березня", "квітня",
                    "травня", "червня", "липня", "серпня",
                    "вересня", "жовтня", "листопада", "грудня"},
    .months_abbr = {"січ.", "лют.", "бер.", "квіт.", "трав.",
                    "черв.", "лип.", "серп.", "вер.", "жовт.",
                    "лист.", "груд."},
    .days_long = {"понеділок", "вівторок", "середа", "четвер",
                  "п'ятниця", "субота", "неділя"},
    .days_abbr = {"пн", "вт", "ср", "чт", "пт", "сб", "нд"},
    .am = "AM", .pm = "PM",
};

static const fdk_i18n_names names_ja = {
    .months_long = {"1月", "2月", "3月", "4月", "5月", "6月", "7月",
                    "8月", "9月", "10月", "11月", "12月"},
    .months_abbr = {"1月", "2月", "3月", "4月", "5月", "6月", "7月",
                    "8月", "9月", "10月", "11月", "12月"},
    .days_long = {"月曜日", "火曜日", "水曜日", "木曜日", "金曜日",
                  "土曜日", "日曜日"},
    .days_abbr = {"月", "火", "水", "木", "金", "土", "日"},
    .am = "午前", .pm = "午後",
};

static const fdk_i18n_names names_zh = {
    .months_long = {"一月", "二月", "三月", "四月", "五月", "六月",
                    "七月", "八月", "九月", "十月", "十一月", "十二月"},
    .months_abbr = {"1月", "2月", "3月", "4月", "5月", "6月", "7月",
                    "8月", "9月", "10月", "11月", "12月"},
    .days_long = {"星期一", "星期二", "星期三", "星期四", "星期五",
                  "星期六", "星期日"},
    .days_abbr = {"周一", "周二", "周三", "周四", "周五", "周六",
                  "周日"},
    .am = "上午", .pm = "下午",
};

static const fdk_i18n_names names_ko = {
    .months_long = {"1월", "2월", "3월", "4월", "5월", "6월", "7월",
                    "8월", "9월", "10월", "11월", "12월"},
    .months_abbr = {"1월", "2월", "3월", "4월", "5월", "6월", "7월",
                    "8월", "9월", "10월", "11월", "12월"},
    .days_long = {"월요일", "화요일", "수요일", "목요일", "금요일",
                  "토요일", "일요일"},
    .days_abbr = {"월", "화", "수", "목", "금", "토", "일"},
    .am = "오전", .pm = "오후",
};

static const fdk_i18n_names names_ar = {
    .months_long = {"يناير", "فبراير", "مارس", "أبريل", "مايو",
                    "يونيو", "يوليو", "أغسطس", "سبتمبر", "أكتوبر",
                    "نوفمبر", "ديسمبر"},
    .months_abbr = {"يناير", "فبراير", "مارس", "أبريل", "مايو",
                    "يونيو", "يوليو", "أغسطس", "سبتمبر", "أكتوبر",
                    "نوفمبر", "ديسمبر"},
    .days_long = {"الاثنين", "الثلاثاء", "الأربعاء", "الخميس",
                  "الجمعة", "السبت", "الأحد"},
    .days_abbr = {"الاثنين", "الثلاثاء", "الأربعاء", "الخميس",
                  "الجمعة", "السبت", "الأحد"},
    .am = "ص", .pm = "م",
};

static const fdk_i18n_names names_hi = {
    .months_long = {"जनवरी", "फ़रवरी", "मार्च", "अप्रैल", "मई",
                    "जून", "जुलाई", "अगस्त", "सितंबर", "अक्टूबर",
                    "नवंबर", "दिसंबर"},
    .months_abbr = {"जन.", "फ़र.", "मार्च", "अप्रैल", "मई", "जून",
                    "जुल.", "अग.", "सित.", "अक्टू.", "नव.", "दिस."},
    .days_long = {"सोमवार", "मंगलवार", "बुधवार", "गुरुवार",
                  "शुक्रवार", "शनिवार", "रविवार"},
    .days_abbr = {"सोम", "मंगल", "बुध", "गुरु", "शुक्र", "शनि",
                  "रवि"},
    .am = "AM", .pm = "PM",
};

/* ---- the rules table ----
 *
 * Order: root first, then per-language rows with territory-specific
 * overrides BEFORE their language-default row. Resolution keeps the
 * most specific hit. Patterns use the mini-language implemented in
 * datetime.c (y/yy/yyyy, M/MM/MMM/MMMM, d/dd, E/EEEE, h/hh, H/HH,
 * m/mm, s/ss, a, quoted literals).
 *
 * Plural rule functions are defined in plural.c; NULL = other-only.
 * Names NULL = English fallback. */

#define R_ROOT_DECIMAL "."
#define R_ROOT_GROUP ","

const fdk_i18n_rules fdk__i18n_rules_table[] = {
    /* ---- root (CLDR root: en-like shaping, other-only plurals) */
    {"", "", ".", ",", 3, 3, "%", false, false, 0,
     "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y",
     "h:mm a", "h:mm:ss a",
     &fdk__i18n_names_en, NULL},

    /* ---- English: en-IN carries Indian grouping */
    {"en", "IN", ".", ",", 3, 2, "%", false, true, 0,
     "d/M/yy", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "h:mm a", "h:mm:ss a",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"en", "", ".", ",", 3, 3, "%", false, true, 0,
     "M/d/yy", "MMM d, y", "MMMM d, y", "EEEE, MMMM d, y",
     "h:mm a", "h:mm:ss a",
     &fdk__i18n_names_en, plural_one_i1v0},

    /* ---- German: de-CH swaps separators to 1'234.56 */
    {"de", "CH", ".", "'", 3, 3, "%", true, false, 0,
     "dd.MM.yyyy", "dd.MM.y", "d. MMMM y", "EEEE, d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_de, plural_one_i1v0},
    {"de", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd.MM.yyyy", "dd.MM.y", "d. MMMM y", "EEEE, d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_de, plural_one_i1v0},

    /* ---- French: NBSP-style grouping (regular space, U+0020),
     * one covers 0 and 1 */
    {"fr", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd/MM/y", "d MMM y", "d MMMM y", "EEEE d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_fr, plural_one_i01},

    /* ---- Spanish */
    {"es", "", ",", ".", 3, 3, "%", true, false, 0,
     "d/M/yy", "d MMM y", "d 'de' MMMM 'de' y",
     "EEEE, d 'de' MMMM 'de' y",
     "HH:mm", "HH:mm:ss",
     &names_es, plural_one_i1v0},

    /* ---- Italian */
    {"it", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd/MM/yy", "d MMM y", "d MMMM y", "EEEE d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_it, plural_one_i1v0},

    /* ---- Portuguese: pt-PT keeps one for exactly 1 (the pt-BR /
     * pt-root rule widens one to 0 and 1, like French) */
    {"pt", "PT", ",", ".", 3, 3, "%", true, false, 0,
     "dd/MM/yy", "dd/MM/y", "d 'de' MMMM 'de' y",
     "EEEE, d 'de' MMMM 'de' y",
     "HH:mm", "HH:mm:ss",
     &names_pt, plural_one_i1v0},
    {"pt", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd/MM/yy", "dd/MM/y", "d 'de' MMMM 'de' y",
     "EEEE, d 'de' MMMM 'de' y",
     "HH:mm", "HH:mm:ss",
     &names_pt, plural_one_i01},

    /* ---- Dutch */
    {"nl", "", ",", ".", 3, 3, "%", true, false, 0,
     "d-M-y", "d MMM y", "d MMMM y", "EEEE d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_nl, plural_one_i1v0},

    /* ---- Swedish */
    {"sv", "", ",", " ", 3, 3, "%", true, false, 0,
     "y-MM-dd", "d MMM y", "'den' d MMMM y", "EEEE 'den' d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_sv, plural_one_i1v0},

    /* ---- Russian: one/few/many/other */
    {"ru", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "dd MMM y", "d MMMM y 'г'.", "EEEE, d MMMM y 'г'.",
     "HH:mm", "HH:mm:ss",
     &names_ru, plural_ru_uk},

    /* ---- Ukrainian: same plural shape as Russian */
    {"uk", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "dd MMM y", "d MMMM y", "EEEE, d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_uk, plural_ru_uk},

    /* ---- Polish: one/few/many/other */
    {"pl", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_pl, plural_pl},

    /* ---- Czech & Slovak: one/few/many(=v!=0)/other */
    {"cs", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.yyyy", "d. M. y", "d. MMMM y", "EEEE d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_cs_sk},
    {"sk", "", ",", " ", 3, 3, "%", true, false, 0,
     "d. M. y", "d. M. y", "d. MMMM y", "EEEE, d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_cs_sk},

    /* ---- Croatian / Serbian / Bosnian: one/few/other */
    {"hr", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd.MM.y.", "d. MMM y.", "d. MMMM y.", "EEEE, d. MMMM y.",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_hr_sr_bs},
    {"sr", "", ",", ".", 3, 3, "%", true, false, 0,
     "d.M.y.", "d. MMM y.", "d. MMMM y.", "EEEE, d. MMMM y.",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_hr_sr_bs},
    {"bs", "", ",", ".", 3, 3, "%", true, false, 0,
     "d.M.y.", "d. MMM y.", "d. MMMM y.", "EEEE, d. MMMM y.",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_hr_sr_bs},

    /* ---- Lithuanian & Latvian */
    {"lt", "", ",", " ", 3, 3, "%", true, false, 0,
     "y-MM-dd", "y MMM d", "y MMMM d", "y MMMM d, EEEE",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_lt},
    {"lv", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "y. 'gada' d. MMM", "y. 'gada' d. MMMM",
     "EEEE, y. 'gada' d. MMMM",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_lv},

    /* ---- Greek, Turkish: plain one/other and other-only */
    {"el", "", ",", ".", 3, 3, "%", true, false, 0,
     "d/M/y", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"tr", "", ",", ".", 3, 3, "%", true, false, 0,
     "d.MM.y", "d MMM y", "d MMMM y", "d MMMM y EEEE",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, NULL},

    /* ---- Japanese / Chinese / Korean: other-only, CJK dates */
    {"ja", "", ".", ",", 3, 3, "%", false, false, 0,
     "y/MM/dd", "y/MM/dd", "y年M月d日", "y年M月d日EEEE",
     "H:mm", "H:mm:ss",
     &names_ja, NULL},
    {"zh", "", ".", ",", 3, 3, "%", false, false, 0,
     "y/M/d", "y年M月d日", "y年M月d日", "y年M月d日EEEE",
     "H:mm", "H:mm:ss",
     &names_zh, NULL},
    {"ko", "", ".", ",", 3, 3, "%", false, true, 0,
     "yy. M. d.", "y. M. d.", "y년 M월 d일", "y년 M월 d일 EEEE",
     "a h:mm", "a h:mm:ss",
     &names_ko, NULL},

    /* ---- Arabic: Arabic-Indic digits + Arabic separators + the
     * six-category rule */
    {"ar", "", "٫", "٬", 3, 3, "٪", false, false, 1,
     "d/M/y", "d MMM y", "d MMMM y", "EEEE، d MMMM y",
     "HH:mm", "HH:mm:ss",
     &names_ar, plural_ar},

    /* ---- Hindi: Indian grouping, one/other */
    {"hi", "", ".", ",", 3, 2, "%", true, false, 0,
     "d/M/yy", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "h:mm a", "h:mm:ss a",
     &names_hi, plural_one_i1v0},

    /* ---- Other-only languages FDK ships (no names rows) */
    {"id", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd/MM/y", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "HH.mm", "HH.mm.ss",
     &fdk__i18n_names_en, NULL},
    {"vi", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd/MM/y", "d MMM y", "d MMMM y", "EEEE, d MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, NULL},
    {"th", "", ".", ",", 3, 3, "%", false, false, 0,
     "d/M/y", "d MMM y", "d MMMM y", "EEEEที่ d MMMM y",
     "H:mm", "H:mm:ss",
     &fdk__i18n_names_en, NULL},
    {"fi", "", ",", " ", 3, 3, "%", true, false, 0,
     "d.M.y", "d MMM y", "d MMMM y", "EEEE d. MMMM y",
     "H.mm", "H.mm.ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"da", "", ",", ".", 3, 3, "%", true, false, 0,
     "dd.MM.y", "dd. MMM y", "d. MMMM y", "EEEE 'den' d. MMMM y",
     "HH.mm", "HH.mm.ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"no", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "d. MMM y", "d. MMMM y", "EEEE d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"nb", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "d. MMM y", "d. MMMM y", "EEEE d. MMMM y",
     "HH:mm", "HH:mm:ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"et", "", ",", " ", 3, 3, "%", true, false, 0,
     "dd.MM.y", "d. MMM y", "d. MMMM y", "EEEE, d. MMMM y",
     "H:mm", "H:mm:ss",
     &fdk__i18n_names_en, plural_one_i1v0},
    {"ca", "", ",", ".", 3, 3, "%", true, false, 0,
     "d/M/yy", "d MMM y", "d 'de' MMMM 'de' y",
     "EEEE, d 'de' MMMM 'de' y",
     "H:mm", "H:mm:ss",
     &fdk__i18n_names_en, plural_one_i1v0},

    {NULL, NULL, NULL, NULL, 0, 0, NULL, false, false, 0,
     NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

/* ---- resolution ---- */

const fdk_i18n_rules *fdk__i18n_rules_for(const char *language,
                                          const char *territory) {
    const fdk_i18n_rules *best = &fdk__i18n_rules_table[0]; /* root */
    for (size_t i = 0; fdk__i18n_rules_table[i].language != NULL; i++) {
        const fdk_i18n_rules *r = &fdk__i18n_rules_table[i];
        if (strcmp(r->language, language) != 0) {
            continue;
        }
        if (r->territory[0] == '\0') {
            /* language default: better than root, weaker than a
             * territory hit — keep scanning for a territory row. */
            best = r;
            continue;
        }
        if (strcmp(r->territory, territory) == 0) {
            return r; /* most specific possible */
        }
    }
    return best;
}

const fdk_i18n_rules *fdk__i18n_rules_row(fdk_u32 index) {
    size_t n = 0;
    while (fdk__i18n_rules_table[n].language != NULL) {
        n++;
    }
    if (index >= n) {
        return &fdk__i18n_rules_table[0];
    }
    return &fdk__i18n_rules_table[index];
}

/* ---- tag parsing ---- */

static bool ascii_is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool ascii_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* A POSIX charset/modifier segment: letters, digits, '-', '.', '_',
 * 1..16 chars. */
static bool is_charset_segment(const char *s, size_t n) {
    if (n == 0 || n > 16) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!ascii_is_letter(c) && !ascii_is_digit(c) && c != '-' &&
            c != '.' && c != '_') {
            return false;
        }
    }
    return true;
}

fdk_result fdk_locale_parse(const char *tag, fdk_locale *out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->rules = 0; /* root until proven otherwise */

    if (tag == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* The tag is measured with strlen: an embedded NUL simply ends
     * it (the C string IS the tag; bytes past it are not ours). */
    size_t len = strlen(tag);
    if (len == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* "C" and "POSIX" are the root locale. */
    if (strcmp(tag, "C") == 0 || strcmp(tag, "POSIX") == 0) {
        return FDK_OK; /* language "" == root */
    }

    /* Split on '-', '_', '.', and '@' — BCP-47 subtag separators
     * plus the POSIX '.' (charset) and '@' (modifier) marks, so
     * "de_CH.UTF-8" splits into de / CH / UTF-8 rather than gluing
     * "CH.UTF" into one unrecognizable segment. Segments after the
     * first three that are charset/modifier-shaped are accepted and
     * discarded; anything else is a syntax error. */
    char segs[8][24];
    size_t seg_len[8];
    size_t seg_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = (i < len) ? tag[i] : '-';
        if (c == '-' || c == '_' || c == '.' || c == '@') {
            size_t n = i - start;
            if (n == 0) {
                return FDK_ERR_INVALID_ARGUMENT; /* empty segment */
            }
            if (seg_count == 8) {
                return FDK_ERR_INVALID_ARGUMENT; /* too many parts */
            }
            if (n >= sizeof(segs[0])) {
                return FDK_ERR_INVALID_ARGUMENT; /* oversized subtag */
            }
            memcpy(segs[seg_count], tag + start, n);
            segs[seg_count][n] = '\0';
            seg_len[seg_count] = n;
            seg_count++;
            start = i + 1;
        }
    }

    /* "C"/"POSIX" with trailing junk ("C.UTF-8", the container
     * default) is still the root locale. */
    if ((seg_len[0] == 1 && segs[0][0] == 'C') ||
        (seg_len[0] == 5 && strcmp(segs[0], "POSIX") == 0)) {
        return FDK_OK;
    }

    /* Language: 2..8 letters. */
    size_t lang_len = seg_len[0];
    if (lang_len < 2 || lang_len > 8) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < lang_len; i++) {
        if (!ascii_is_letter(segs[0][i])) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
    }
    for (size_t i = 0; i < lang_len; i++) {
        out->language[i] = lower(segs[0][i]);
    }
    out->language[lang_len] = '\0';

    size_t next = 1;
    /* Optional script: exactly 4 letters, title case after
     * normalization ("Hant", "LATN" accepted). Only in position 1 —
     * a 4-letter segment later is a charset-looking thing we
     * discard, not a script. */
    if (next < seg_count && seg_len[next] == 4) {
        bool all_letters = true;
        for (size_t i = 0; i < 4; i++) {
            if (!ascii_is_letter(segs[next][i])) {
                all_letters = false;
                break;
            }
        }
        if (all_letters) {
            out->script[0] = upper(segs[next][0]);
            for (size_t i = 1; i < 4; i++) {
                out->script[i] = lower(segs[next][i]);
            }
            out->script[4] = '\0';
            next++;
        }
    }

    /* Optional territory: 2 letters or 3 digits. */
    if (next < seg_count &&
        (seg_len[next] == 2 || seg_len[next] == 3)) {
        size_t n = seg_len[next];
        bool ok;
        if (n == 2) {
            ok = ascii_is_letter(segs[next][0]) &&
                 ascii_is_letter(segs[next][1]);
            if (ok) {
                out->territory[0] = upper(segs[next][0]);
                out->territory[1] = upper(segs[next][1]);
                out->territory[2] = '\0';
            }
        } else {
            ok = ascii_is_digit(segs[next][0]) &&
                 ascii_is_digit(segs[next][1]) &&
                 ascii_is_digit(segs[next][2]);
            if (ok) {
                memcpy(out->territory, segs[next], 3);
                out->territory[3] = '\0';
            }
        }
        if (ok) {
            next++;
        }
        /* A 2-3 char segment that is neither a territory nor a
         * charset shape (e.g. "q!") falls through to the charset
         * check below and fails there — no silent drops. */
    }

    /* Everything remaining: charset ("UTF-8"), modifier ("euro"),
     * extension-ish junk the real world puts in LANG. Accepted and
     * discarded when charset-shaped. */
    while (next < seg_count) {
        if (!is_charset_segment(segs[next], seg_len[next])) {
            return FDK_ERR_INVALID_ARGUMENT;
        }
        next++;
    }

    /* Resolve the rules row now — formatters never rescan. */
    const fdk_i18n_rules *row =
        fdk__i18n_rules_for(out->language, out->territory);
    size_t idx = (size_t)(row - fdk__i18n_rules_table);
    out->rules = (fdk_u32)idx;
    return FDK_OK;
}

fdk_i32 fdk_locale_to_tag(const fdk_locale *loc, char *buf, size_t cap) {
    if (buf == NULL || cap == 0 || loc == NULL) {
        if (buf != NULL && cap > 0) {
            buf[0] = '\0';
        }
        return (fdk_i32)FDK_ERR_INVALID_ARGUMENT;
    }
    buf[0] = '\0';
    size_t len = 0;

    if (loc->language[0] != '\0') {
        if (fdk__fmt_append(buf, cap, &len, loc->language) != 0) {
            return (fdk_i32)FDK_ERR_LIMIT;
        }
    } else {
        return 0; /* root: empty tag */
    }
    if (loc->script[0] != '\0') {
        if (fdk__fmt_append(buf, cap, &len, "-") != 0 ||
            fdk__fmt_append(buf, cap, &len, loc->script) != 0) {
            return (fdk_i32)FDK_ERR_LIMIT;
        }
    }
    if (loc->territory[0] != '\0') {
        if (fdk__fmt_append(buf, cap, &len, "-") != 0 ||
            fdk__fmt_append(buf, cap, &len, loc->territory) != 0) {
            return (fdk_i32)FDK_ERR_LIMIT;
        }
    }
    buf[len] = '\0';
    return (fdk_i32)len;
}

const fdk_locale *fdk_locale_root(void) {
    static fdk_locale root = {"", "", "", 0};
    return &root;
}

const fdk_locale *fdk_locale_english(void) {
    /* Resolved once: the "en" language-default row's index. */
    static fdk_locale en = {"en", "", "", 0};
    if (en.rules == 0) {
        const fdk_i18n_rules *row = fdk__i18n_rules_for("en", "");
        en.rules = (fdk_u32)(row - fdk__i18n_rules_table);
    }
    return &en;
}
