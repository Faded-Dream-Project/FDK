/*
 * test_shortcut.c — the 1.3.3 shortcut parser/format/matcher battery
 *
 * Headless and pure: every table entry pins the grammar exactly
 * (modifiers, key names, case/whitespace tolerance, and the loud
 * refusal of every malformed shape). The format() side must be the
 * parser's EXACT inverse — round-trip every valid pair.
 *
 * The window-level dispatch and the menu-bar accelerator scan have
 * their own coverage: logic here, real keys in test_x11_integration
 * (XSendEvent) and the Wayland suite (virtual keyboard).
 */

#include "fdk/fdk.h"

#include "core/shortcut_internal.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- parser: valid forms ---- */

static void test_parse_valid(void) {
    struct {
        const char *spec;
        fdk_u32 mods;
        fdk_scancode key;
    } ok[] = {
        {"Ctrl+S", FDK_MOD_CTRL, 31},
        {"ctrl+s", FDK_MOD_CTRL, 31},
        {"CTRL+S", FDK_MOD_CTRL, 31},
        {"Control+S", FDK_MOD_CTRL, 31},
        {" Ctrl + S ", FDK_MOD_CTRL, 31}, /* whitespace tolerated */
        {"Ctrl\t+S", FDK_MOD_CTRL, 31},   /* tab too — trim's contract */
        {"Shift+Home", FDK_MOD_SHIFT, 102},
        {"Alt+F4", FDK_MOD_ALT, 62},
        {"Super+L", FDK_MOD_SUPER, 38},
        {"Meta+L", FDK_MOD_SUPER, 38},
        {"Ctrl+Shift+Z", FDK_MOD_CTRL | FDK_MOD_SHIFT, 44},
        {"Ctrl+Alt+Delete", FDK_MOD_CTRL | FDK_MOD_ALT, 111},
        {"Ctrl+Shift+Alt+Super+F12", FDK_MOD_CTRL | FDK_MOD_SHIFT |
                                          FDK_MOD_ALT | FDK_MOD_SUPER, 88},
        {"F5", 0, 63},
        {"f5", 0, 63},
        {"F1", 0, 59},
        {"F10", 0, 68},
        {"F11", 0, 87},
        {"F12", 0, 88},
        {"Enter", 0, 28},
        {"Return", 0, 28},
        {"Esc", 0, 1},
        {"Escape", 0, 1},
        {"Space", 0, 57},
        {"Tab", 0, 15},
        {"Backspace", 0, 14},
        {"Delete", 0, 111},
        {"Del", 0, 111},
        {"Insert", 0, 110},
        {"Ins", 0, 110},
        {"Home", 0, 102},
        {"End", 0, 107},
        {"PageUp", 0, 104},
        {"PgUp", 0, 104},
        {"PageDown", 0, 109},
        {"PgDn", 0, 109},
        {"Left", 0, 105},
        {"Right", 0, 106},
        {"Up", 0, 103},
        {"Down", 0, 108},
        {"Plus", 0, 13},
        {"Minus", 0, 12},
        {"Comma", 0, 51},
        {"Period", 0, 52},
        {"Dot", 0, 52},
        {"Slash", 0, 53},
        {"A", 0, 30},
        {"z", 0, 44},
        {"Q", 0, 16},
        {"P", 0, 25},
        {"M", 0, 50},
        {"0", 0, 11},
        {"1", 0, 2},
        {"9", 0, 10},
        /* QWERTY position sanity: the whole first row. */
        {"W", 0, 17}, {"E", 0, 18}, {"R", 0, 19}, {"T", 0, 20},
        {"Y", 0, 21}, {"U", 0, 22}, {"I", 0, 23}, {"O", 0, 24},
        /* Home row. */
        {"S", 0, 31}, {"D", 0, 32}, {"F", 0, 33}, {"G", 0, 34},
        {"H", 0, 35}, {"J", 0, 36}, {"K", 0, 37}, {"L", 0, 38},
        /* Bottom row. */
        {"X", 0, 45}, {"C", 0, 46}, {"V", 0, 47}, {"B", 0, 48},
        {"N", 0, 49},
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        fdk_u32 mods = 0;
        fdk_scancode key = 0;
        fdk_result r = fdk_shortcut_parse(ok[i].spec, &mods, &key);
        if (!fdk_ok(r) || mods != ok[i].mods || key != ok[i].key) {
            fprintf(stderr, "parse failed on \"%s\": r=%d mods=%u "
                    "key=%u (want %u/%u)\n", ok[i].spec, (int)r,
                    mods, (unsigned)key, ok[i].mods,
                    (unsigned)ok[i].key);
            assert(false);
        }
    }
    printf("[ok] shortcut parse: %zu valid specs (mods+keys, case, "
           "whitespace, full name table)\n",
           sizeof(ok) / sizeof(ok[0]));
}

/* ---- parser: every malformed shape is a loud error ---- */

static void test_parse_invalid(void) {
    const char *bad[] = {
        "", " ", "+", "Ctrl", "ctrl+", "+S", "S+", "Ctrl++S",
        "Ctrl+Ctrl+S", "Ctrl+Alt+Ctrl+S", "Ctrl+S+S", "S+Ctrl",
        "Ctrl+Hello", "F13", "F0", "ff5", "5F",
        "Ctrl+Shift+", "Shift+Shift+S", "control", "CTRL",
        "Ctrl+backspace2", "Ctrl+ß", "Ctrl+Page", "Ctrl+Updown",
        "Ctrl+ ", "  ",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        fdk_u32 mods = 0;
        fdk_scancode key = 0;
        if (fdk_ok(fdk_shortcut_parse(bad[i], &mods, &key))) {
            fprintf(stderr, "parse ACCEPTED garbage: \"%s\"\n",
                    bad[i]);
            assert(false);
        }
    }
    /* NULL discipline. */
    fdk_u32 mods = 0;
    fdk_scancode key = 0;
    assert(!fdk_ok(fdk_shortcut_parse(NULL, &mods, &key)));
    assert(!fdk_ok(fdk_shortcut_parse("Ctrl+S", NULL, &key)));
    assert(!fdk_ok(fdk_shortcut_parse("Ctrl+S", &mods, NULL)));
    printf("[ok] shortcut parse: %zu malformed specs refused + NULL "
           "discipline\n", sizeof(bad) / sizeof(bad[0]));
}

/* ---- format: the parser's exact inverse ---- */

static void test_format_roundtrip(void) {
    fdk_u32 all_mods[] = {
        0, FDK_MOD_CTRL, FDK_MOD_SHIFT, FDK_MOD_ALT, FDK_MOD_SUPER,
        FDK_MOD_CTRL | FDK_MOD_SHIFT, FDK_MOD_CTRL | FDK_MOD_ALT,
        FDK_MOD_CTRL | FDK_MOD_SHIFT | FDK_MOD_ALT | FDK_MOD_SUPER,
    };
    fdk_scancode keys[] = {
        16, 17, 30, 31, 44, 45, 46, 47, 2, 11,          /* letters/digits */
        59, 63, 68, 87, 88,                              /* F-keys */
        1, 15, 28, 57, 102, 107, 104, 109, 111, 110,     /* named */
        105, 106, 103, 108, 13, 12, 51, 52, 53,
    };
    size_t n = 0;
    for (size_t m = 0; m < sizeof(all_mods) / sizeof(all_mods[0]); m++) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            fdk_u32 mods = all_mods[m];
            fdk_scancode key = keys[k];
            char spec[FDK_SHORTCUT_SPEC_MAX];
            if (!fdk_shortcut_format(mods, key, spec, sizeof(spec))) {
                fprintf(stderr, "format refused mods=%u key=%u\n",
                        mods, (unsigned)key);
                assert(false);
            }
            fdk_u32 back_mods = 0;
            fdk_scancode back_key = 0;
            if (!fdk_ok(fdk_shortcut_parse(spec, &back_mods,
                                           &back_key)) ||
                back_mods != mods || back_key != key) {
                fprintf(stderr, "round-trip broke: mods=%u key=%u -> "
                        "\"%s\" -> %u/%u\n", mods, (unsigned)key, spec,
                        back_mods, (unsigned)back_key);
                assert(false);
            }
            n++;
        }
    }
    /* Canonical form: fixed order, lowercase. */
    char spec[FDK_SHORTCUT_SPEC_MAX];
    assert(fdk_shortcut_format(FDK_MOD_CTRL | FDK_MOD_SHIFT, 31, spec,
                               sizeof(spec)));
    assert(strcmp(spec, "ctrl+shift+s") == 0);
    assert(fdk_shortcut_format(0, 63, spec, sizeof(spec)));
    assert(strcmp(spec, "f5") == 0);
    /* Unmappable inputs are refused, not guessed. */
    assert(!fdk_shortcut_format(0, 200, spec, sizeof(spec)));
    assert(!fdk_shortcut_format(0x80000000u, 31, spec, sizeof(spec)));
    assert(!fdk_shortcut_format(0, 31, spec, 1)); /* "s"+NUL needs 2 */
    assert(!fdk_shortcut_format(FDK_MOD_CTRL | FDK_MOD_SHIFT, 31, spec,
                                8)); /* "ctrl+shift+s" needs 12 */
    assert(!fdk_shortcut_format(0, 31, NULL, 8));
    assert(!fdk_shortcut_format(0, 31, spec, 0));
    printf("[ok] shortcut format: %zu round-trips + canonical order + "
           "refusals\n", n);
}

/* ---- matcher: exact-modifier equality semantics ---- */

static void test_matcher(void) {
    fdk_u32 CTRL = FDK_MOD_CTRL;
    fdk_key_event ev;
    memset(&ev, 0, sizeof(ev));

    ev.scancode = 31; /* S */
    ev.modifiers = FDK_MOD_CTRL;
    assert(fdk__shortcut_matches(CTRL, 31, &ev));
    ev.modifiers = FDK_MOD_CTRL | FDK_MOD_SHIFT;
    assert(!fdk__shortcut_matches(CTRL, 31, &ev)); /* extra Shift blocks */
    ev.modifiers = 0;
    assert(!fdk__shortcut_matches(CTRL, 31, &ev)); /* missing Ctrl blocks */
    ev.modifiers = FDK_MOD_ALT;
    assert(!fdk__shortcut_matches(CTRL, 31, &ev)); /* wrong modifier */
    ev.scancode = 30; /* A, not S */
    ev.modifiers = FDK_MOD_CTRL;
    assert(!fdk__shortcut_matches(CTRL, 31, &ev));

    /* Bare-key specs match with zero modifiers held. */
    ev.scancode = 63; /* F5 */
    ev.modifiers = 0;
    assert(fdk__shortcut_matches(0, 63, &ev));
    ev.modifiers = FDK_MOD_SHIFT;
    assert(!fdk__shortcut_matches(0, 63, &ev));

    /* is_repeat does not participate: repeats fire like presses. */
    ev.scancode = 31;
    ev.modifiers = FDK_MOD_CTRL;
    ev.is_repeat = 1;
    assert(fdk__shortcut_matches(CTRL, 31, &ev));
    ev.is_repeat = 0;
    assert(fdk__shortcut_matches(CTRL, 31, &ev));

    assert(!fdk__shortcut_matches(CTRL, 31, NULL)); /* NULL event */
    printf("[ok] shortcut matcher: exact modifiers, wrong keys, "
           "repeat-independence, NULL discipline\n");
}

int main(void) {
    test_parse_valid();
    test_parse_invalid();
    test_format_roundtrip();
    test_matcher();
    printf("all shortcut tests passed\n");
    return 0;
}
