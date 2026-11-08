/* fdk-langs — the language sweep: every writing system the installed
 * fonts cover, rendered through FDK's real widget paths, ink-checked
 * and screenshot-dumped (round 32; the owner's section-4 ask).
 *
 * Per script family (13: Latin, Cyrillic, Greek, Arabic, Hebrew,
 * Devanagari, Thai, Armenian, Georgian, Han, Kana, Hangul,
 * Emoji+ZWJ):
 *   1. a LABEL with the sample line
 *   2. an ENTRY set to the same text, read back byte-identical
 *   3. a TEXTVIEW with a two-line paragraph (the mixed LTR/RTL
 *      bidi pin included)
 *   4. the window TITLE carries the script name
 *   5. paint + count ink pixels — a family whose glyphs fail to
 *      shape or fall back renders blank and FAILS here
 * Then the locale sweep: double/int/currency/date across 24 locales,
 * each result non-empty with the right separator shape.
 *
 * Receipt: one line per family + locale totals.
 * --png=DIR saves one PNG per family. Exit 0 iff no family failed.
 */
#include "../common/app.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_layout.h"
#include "fdk/fdk_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *script;
    const char *sample; /* real words, not filler        */
    const char *mixed;  /* mixed LTR/RTL line (bidi pin) */
    bool rtl;
} family;

static const family FAMILIES[] = {
    { "Latin",      "Hello, FDK — every letter matters.",
                    "fdk 1.1.0 — parity audit", false },
    { "Cyrillic",   "Съешь же ещё этих мягких французских булок.",
                    "версия 1.1.0 — полный аудит", false },
    { "Greek",      "Η γλώσσα μου έδωσε την ελληνική αλφάβητο.",
                    "έκδοση 1.1.0 — πλήρης έλεγχος", false },
    { "Arabic",     "اللغة العربية جميلة وتكتب من اليمين إلى اليسار.",
                    "الإصدار 1.1.0 — النسخة العربية", true },
    { "Hebrew",     "שפה יפה היא עברית, וכתיבתה מימין לשמאל.",
                    "גרסה 1.1.0 — ביקורת מלאה", true },
    { "Devanagari", "मैं गंगा की धारा में स्नान करता हूँ।",
                    "संस्करण 1.1.0 — पूर्ण जाँच", false },
    { "Thai",       "ภาษาไทยงดงามและไม่มีเว้นวรรคระหว่างคำ",
                    "รุ่น 1.1.0 — การตรวจสอบ", false },
    { "Armenian",   "Հայոց լեզուն գեղեցիկ է ու հին։",
                    "տարբերակ 1.1.0 — ամբողջական ստուգում", false },
    { "Georgian",   "ქართული ენა ლამაზი და უძველესია.",
                    "ვერსია 1.1.0 — სრული შემოწმება", false },
    { "Han",        "汉字书写系统源远流长，每一个字都有生命。",
                    "版本 1.1.0 — 完整审查", false },
    { "Kana",       "日本語のかなは美しく整えられています。",
                    "バージョン 1.1.0 — 完全監査", false },
    { "Hangul",     "한글은 세종대왕이 창제한 아름다운 글자입니다.",
                    "버전 1.1.0 — 전체 감사", false },
    { "Emoji",      "ships \xF0\x9F\x9A\x80 done \xF0\x9F\xA7\xA9 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7",
                    "1.1.0 \xE2\x9C\x85 ok \xF0\x9F\x8E\x89", false },
};

#define NFAM ((int)(sizeof FAMILIES / sizeof FAMILIES[0]))

static ta_app TA;
static struct { fdk_widget *label, *entry, *textview; } SW;
static int fam_renders, fam_roundtrip, fam_bidi, fails;
static char png_dir[280];

/* How long each family stays on screen: the sweep used to flash all
 * 13 in a couple of frames and exit — fine for a robot, useless for
 * a human watching it. Default 5 seconds per language; --dwell=<s>
 * overrides (0 = the old flash-through, what the automated runs
 * use). */
static long dwell_ms = 5000;
static fdk_font *sys_font; /* the honest coverage gate */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int ink_permille(void) {
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_window_get_surface(TA.window, &s))) {
        return -1;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return -1;
    }
    /* theme-agnostic: the modal luminance IS the background; ink is
     * everything that differs from it clearly (works for dark and
     * light themes alike) */
    long hist[256] = {0};
    long total = 0;
    for (int y = 0; y < (int)info.height; y += 3) {
        const fdk_u32 *row = info.pixels + (size_t)y * (size_t)info.stride;
        for (int x = 0; x < (int)info.width; x += 3) {
            fdk_u32 px = row[x];
            int r = (int)((px >> 16) & 0xFF);
            int g = (int)((px >> 8) & 0xFF);
            int b = (int)(px & 0xFF);
            hist[(r * 3 + g * 5 + b * 2) / 10]++;
            total++;
        }
    }
    if (!total) {
        return -1;
    }
    int bg = 0;
    for (int i = 1; i < 256; i++) {
        if (hist[i] > hist[bg]) {
            bg = i;
        }
    }
    long ink = 0;
    for (int i = 0; i < 256; i++) {
        if (i < bg - 40 || i > bg + 40) {
            ink += hist[i];
        }
    }
    return (int)((ink * 1000) / total);
}

static void run_family(int i) {
    int unc = 0;
    const family *f = &FAMILIES[i];
    char title[128];
    snprintf(title, sizeof title, "lang: %s (%d/%d)", f->script, i + 1, NFAM);
    fdk_window_set_title(TA.window, title);

    fdk_widget *lab = SW.label, *ent = SW.entry, *tv = SW.textview;
    fdk_label_set_text(lab, f->sample);
    fdk_entry_set_text(ent, f->sample);
    const char *back = fdk_entry_get_text(ent);
    bool rt = back && strcmp(back, f->sample) == 0;
    char para[512];
    snprintf(para, sizeof para, "%s\n%s", f->sample, f->mixed);
    fdk_textview_set_text(tv, para);

    /* layout runs in the pump's tick: set the texts, pump so the
     * relayout + paint actually happen (a bare tree-paint would
     * rasterize the widgets' STALE bounds — every family sampled
     * zero ink until this was understood, round 32) */
    fdk_widget_invalidate_all(TA.content);
    ta_pump(&TA);
    ta_pump(&TA);
    int r = ink_permille();
    bool ok = r > 1; /* thin-stroked scripts reach ~2-10 permille */
    if (!rt) {
        printf("LANG FAIL %s: entry round-trip mismatch\n", f->script);
        fails++;
    }
    if (!ok) {
        printf("LANG FAIL %s: no ink rendered (ratio %d permille)\n",
               f->script, r);
        fails++;
    } else {
        fam_renders++;
        fam_roundtrip += rt;
    }
    /* the bidi pin: the mixed line round-trips the entry too (the
     * caret math is byte-offset based; the visual reordering is the
     * shaper's job and is exercised by the ink check above — the
     * mixed line is IN the textview paragraph) */
    fdk_entry_set_text(ent, f->mixed);
    back = fdk_entry_get_text(ent);
    bool b = back && strcmp(back, f->mixed) == 0;
    fam_bidi += b;
    if (!b) {
        printf("LANG FAIL %s: bidi line round-trip\n", f->script);
        fails++;
    }
    unc = fdk_text_count_uncovered(sys_font, f->sample, strlen(f->sample)) +
        fdk_text_count_uncovered(sys_font, f->mixed, strlen(f->mixed));
    if (unc > 0) {
        printf("LANG FAIL %s: %d codepoint(s) would draw .notdef (tofu) on this machine\n", f->script, unc);
        fails++;
    }
    printf("LANG %-10s ink=%4d permille renders=%s entry=%s bidi=%s uncovered=%d\n",
           f->script, r, ok ? "ok" : "NO", rt ? "ok" : "NO",
           b ? "ok" : "NO", unc);
    if (png_dir[0] && ok) {
        fdk_surface *ps = NULL;
        if (fdk_ok(fdk_window_get_surface(TA.window, &ps))) {
            char path[340];
            snprintf(path, sizeof path, "%s/%02d-%s.png", png_dir, i,
                     f->script);
            fdk_surface_save_png(ps, path);
        }
    }

    /* the watchable dwell: keep pumping so the window stays live
     * (paints, input, resize) while the family stays on screen */
    if (dwell_ms > 0) {
        long deadline = now_ms() + dwell_ms;
        while (ta_pump(&TA)) {
            if (now_ms() >= deadline) break;
        }
    }}

static const struct { const char *tag; const char *cur; } LOCALES[] = {
    { "en-US", "USD" }, { "en-GB", "GBP" }, { "de-DE", "EUR" },
    { "fr-FR", "EUR" }, { "es-ES", "EUR" }, { "it-IT", "EUR" },
    { "pt-BR", "BRL" }, { "nl-NL", "EUR" }, { "sv-SE", "SEK" },
    { "pl-PL", "PLN" }, { "tr-TR", "TRY" }, { "ru-RU", "RUB" },
    { "el-GR", "EUR" }, { "he-IL", "ILS" }, { "ar-EG", "EGP" },
    { "fa-IR", "IRR" }, { "hi-IN", "INR" }, { "th-TH", "THB" },
    { "vi-VN", "VND" }, { "id-ID", "IDR" }, { "ja-JP", "JPY" },
    { "ko-KR", "KRW" }, { "zh-CN", "CNY" }, { "zh-TW", "TWD" },
};

static void locale_sweep(void) {
    int ok = 0;
    for (size_t i = 0; i < sizeof LOCALES / sizeof LOCALES[0]; i++) {
        fdk_locale loc;
        if (!fdk_ok(fdk_locale_parse(LOCALES[i].tag, &loc))) {
            printf("LOCALE FAIL %s: parse\n", LOCALES[i].tag);
            fails++;
            continue;
        }
        char num[64], cur[64], date[64];
        fdk_number_options nopt = {2, 2, 1, 0};
        fdk_date d = {2026, 11, 7};
        bool good =
            fdk_ok(fdk_format_double(num, sizeof num, &loc, 1234567.89,
                                     2, &nopt)) &&
            fdk_ok(fdk_format_currency(cur, sizeof cur, &loc, 1234.56,
                                       LOCALES[i].cur)) &&
            fdk_ok(fdk_format_date(date, sizeof date, &loc, &d,
                                   FDK_DATE_SHORT));
        bool has_sep = strchr(num, ',') || strchr(num, '.') ||
                       strchr(num, ' ') || strstr(num, "\xE2\x80\x89") ||
                       strstr(num, "\xC2\xA0") ||
                       /* Arabic-Indic: U+066C thousands, U+066B decimal */
                       strstr(num, "\xD9\xAC") ||
                       strstr(num, "\xD9\xAB");
        if (good && strlen(cur) > 3 && has_sep && strlen(date) >= 6) {
            ok++;
        } else {
            printf("LOCALE FAIL %s: num='%s' cur='%s' date='%s'\n",
                   LOCALES[i].tag, num, cur, date);
            fails++;
        }
    }
    printf("LOCALE swept %zu: %d formatted cleanly\n",
           sizeof LOCALES / sizeof LOCALES[0], ok);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--png=", 6) == 0) {
            snprintf(png_dir, sizeof png_dir, "%s", argv[i] + 6);
        }
        if (strncmp(argv[i], "--dwell=", 8) == 0) {
            dwell_ms = strtol(argv[i] + 8, NULL, 10) * 1000L;
            if (dwell_ms < 0) dwell_ms = 0;
        }
    }
    if (!ta_init(&TA, "fdk-langs") ||
        !ta_open(&TA, "fdk-langs — the language sweep", 1000, 720)) {
        return 1;
    }
    sys_font = fdk_font_load_system_default(14);
    fdk_widget *col = NULL;
    fdk_box_create(TA.content, FDK_VERTICAL, &col);
    TA.content = col;
    fdk_label_create(TA.content, TA.font_ui, "", &SW.label);
    fdk_entry_create(TA.content, TA.font_ui, "", &SW.entry);
    fdk_textview_create(TA.content, TA.font_ui, &SW.textview);

    /* settle: the first pumps deliver the window's configure and run
     * the layout pass — without them the framebuffer is still 1x1 and
     * every family would sample zero ink (found live, round 32) */
    for (int i = 0; i < 6 && ta_pump(&TA); i++) {
    }

    for (int i = 0; i < NFAM; i++) {
        run_family(i);
    }
    locale_sweep();
    printf("LANGS receipt: families=%d renders=%d entry=%d bidi=%d "
           "locales=24 fails=%d\n",
           NFAM, fam_renders, fam_roundtrip, fam_bidi, fails);
    if (sys_font != NULL) {
        fdk_font_destroy(sys_font);
    }
    ta_close(&TA);
    return fails == 0 ? 0 : 2;
}
