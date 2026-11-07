/*
 * shortcut.c — keyboard shortcut specification parser (1.3.3)
 *
 * FDK's shortcut layer has two halves:
 *
 *   1. THIS FILE — the pure parser. It turns a human string
 *      ("Ctrl+Shift+S", "F5", "Alt+F4") into the machine pair
 *      (modifier bitmask, scancode) that fdk_key_event carries. It
 *      touches no state, allocates nothing, and is fully unit-tested
 *      headless (tests/test_shortcut.c).
 *
 *   2. The consumers — fdk_window_add_shortcut (the per-window
 *      application table, src/window/window.c) and the menu
 *      accelerators (src/widget/menu.c's live bar scan). They match
 *      KEY_DOWN events against parsed pairs; the matching itself is
 *      the two-field comparison documented on fdk__shortcut_matches.
 *
 * SEMANTICS — the layout question, settled once:
 *
 * A parsed "Ctrl+S" binds the PHYSICAL S key (Linux evdev scancode
 * 31) — the position, not the printed glyph. That is fdk_event.h's
 * documented scancode contract ("bindings you want stable across
 * layouts"), and it is what a shortcut REGISTERED ON A WINDOW means
 * in FDK. Note the deliberate contrast: the Entry widget's built-in
 * editing keys (Ctrl+C/V/X/A) match on `codepoint`, the
 * layout-RESOLVED glyph — the "printed C" rule text editing follows.
 * On a QWERTY layout the two are the same key; on AZERTY/Dvorak they
 * can differ (AZERTY prints 'a' at the physical Q position). Both
 * rules are correct for their layer; both are documented where they
 * live. See docs/platform-input.md's shortcut section.
 *
 * GRAMMAR (case-insensitive, whitespace-tolerant):
 *
 *   spec    := key | modifier { "+" modifier } { "+" key }
 *   modifier:= "ctrl" | "control" | "shift" | "alt" | "super" | "meta"
 *   key     := "a".."z" | "0".."9" | "f1".."f12" | named
 *   named   := tab, enter, return, esc, escape, space, backspace,
 *              delete, del, insert, ins, home, end, pageup, pgup,
 *              pagedown, pgdn, left, right, up, down,
 *              plus, minus, comma, period, slash
 *
 * A bare modifier ("Ctrl"), an empty spec, a dangling separator
 * ("Ctrl+"), a duplicated final key segment, or an unknown name is
 * FDK_ERR_INVALID_ARGUMENT — parse failures are loud, never guesses.
 */

#include "fdk/fdk_event.h"

#include "shortcut_internal.h"

#include <string.h>

/* ---- evdev scancodes the parser may produce (values, not names —
 * the public FDK_KEY_* list stays deliberately minimal; see
 * fdk_event.h) ----------------------------------------------- */

#define EV_A 30
#define EV_B 48
#define EV_C 46
#define EV_D 32
#define EV_E 18
#define EV_F 33
#define EV_G 34
#define EV_H 35
#define EV_I 23
#define EV_J 36
#define EV_K 37
#define EV_L 38
#define EV_M 50
#define EV_N 49
#define EV_O 24
#define EV_P 25
#define EV_Q 16
#define EV_R 19
#define EV_S 31
#define EV_T 20
#define EV_U 22
#define EV_V 47
#define EV_W 17
#define EV_X 45
#define EV_Y 21
#define EV_Z 44

/* Digits: '1'..'9' = 2..10, '0' = 11. */
#define EV_DIGIT0 11
#define EV_DIGIT1 2

/* Punctuation the grammar names. */
#define EV_MINUS 12
#define EV_EQUAL 13
#define EV_COMMA 51
#define EV_DOT 52
#define EV_SLASH 53
/* §31/S6: the xkb keysym spellings of the remaining QWERTY
 * punctuation (evdev standard positions). */
#define EV_SEMICOLON 39
#define EV_APOSTROPHE 40
#define EV_GRAVE 41
#define EV_BACKSLASH 43
#define EV_BRACKETLEFT 26
#define EV_BRACKETRIGHT 27
#define EV_RO 4 /* obsolete; not in the grammar — reserved        */
#define EV_KPADD 78
#define EV_KPSUBTRACT 83
#define EV_KPMULTIPLY 55
#define EV_KPDIVIDE 98
#define EV_KPENTER 96
#define EV_KP0 82
#define EV_KP1 79
#define EV_KP2 80
#define EV_KP3 81
#define EV_KP4 75
#define EV_KP5 76
#define EV_KP6 77
#define EV_KP7 71
#define EV_KP8 72
#define EV_KP9 73

/* Named non-printing keys (same values as the FDK_KEY_* defines). */
#define EV_TAB 15
#define EV_ENTER 28
#define EV_ESC 1
#define EV_SPACE 57
#define EV_BACKSPACE 14
#define EV_HOME 102
#define EV_END 107
#define EV_PAGEUP 104
#define EV_PAGEDOWN 109
#define EV_DELETE 111
#define EV_INSERT 110
#define EV_LEFT 105
#define EV_RIGHT 106
#define EV_UP 103
#define EV_DOWN 108

/* F1..F10 = 59..68; F11 = 87; F12 = 88. */
#define EV_F1 59
#define EV_F11 87
#define EV_F12 88
/* §31/S6: F13..F24 (evdev 183..194, contiguous). */
#define EV_F13 183
#define EV_F24 194

/* ---- tokenizing ------------------------------------------------------- */

typedef struct {
    const char *p;   /* cursor into the spec */
    bool dangled;    /* the LAST segment consumed ended with a '+'
                      * (a separator with nothing after it — the
                      * parse must fail at the end of input) */
} scan_t;

/* Copies the next '+'-delimited segment into seg (NUL-terminated,
 * lowercased). Returns false at end of string. Segments longer than
 * the buffer are returned truncated (and then simply fail the name
 * lookup — the honest error, no overflow). */
static bool next_segment(scan_t *s, char *seg, size_t cap) {
    size_t n = 0;
    const char *p = s->p;
    if (p == NULL || *p == '\0') {
        return false;
    }
    while (*p != '\0' && *p != '+') {
        char c = *p++;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (n + 1 < cap) {
            seg[n++] = c;
        }
    }
    seg[n] = '\0';
    s->dangled = false;
    if (*p == '+') {
        p++;
        s->dangled = true; /* unless a real segment follows */
    }
    s->p = p;
    return true;
}

/* Leading/trailing whitespace inside a segment is trimmed. */
static void trim(char *seg) {
    size_t len = strlen(seg);
    size_t start = 0;
    while (start < len && (seg[start] == ' ' || seg[start] == '\t')) {
        start++;
    }
    size_t end = len;
    while (end > start && (seg[end - 1] == ' ' || seg[end - 1] == '\t')) {
        end--;
    }
    memmove(seg, seg + start, end - start);
    seg[end - start] = '\0';
}

/* Single-character key names. */
/* Returns: 0 = not a single-char key, 1 = parsed, 2 = parsed AND
 * the symbol is the shifted US glyph (the caller ORs in Shift). */
static int letter_scan(const char *seg, fdk_scancode *out) {
    if (seg[0] == '\0' || seg[1] != '\0') {
        return 0; /* not a single character */
    }
    char c = seg[0];
    /* §31/S6: literal punctuation — "Ctrl+?" means Ctrl+Shift+slash
     * on a US layout. The mapping is US QWERTY's shifted glyphs; on
     * other layouts the physical base key keeps working (X11/Wayland
     * report scancodes by position, so "?" still lands on the slash
     * key's code; a layout where ? is unshifted simply needs no
     * Shift for the same binding — the MENU LABEL shows the spec
     * verbatim either way). */
    if (c == '?' || c == '/' || c == ';' || c == ':' || c == '\'' ||
        c == '"' || c == ',' || c == '<' || c == '.' || c == '>' ||
        c == '[' || c == '{' || c == ']' || c == '}' || c == '\\' ||
        c == '|' || c == '`' || c == '~' || c == '-' || c == '_' ||
        c == '=' || c == '+' || c == '!' || c == '@' || c == '#' ||
        c == '$' || c == '%' || c == '^' || c == '&' || c == '*' ||
        c == '(' || c == ')') {
        static const struct {
            char sym;
            fdk_scancode base;
            bool shifted;
        } punct[] = {
            {'?', EV_SLASH, true},     {'/', EV_SLASH, false},
            {';', EV_SEMICOLON, false}, {':', EV_SEMICOLON, true},
            {'\'', EV_APOSTROPHE, false},
            {'"', EV_APOSTROPHE, true},
            {',', EV_COMMA, false},    {'<', EV_COMMA, true},
            {'.', EV_DOT, false},      {'>', EV_DOT, true},
            {'[', EV_BRACKETLEFT, false},
            {'{', EV_BRACKETLEFT, true},
            {']', EV_BRACKETRIGHT, false},
            {'}', EV_BRACKETRIGHT, true},
            {'\\', EV_BACKSLASH, false},
            {'|', EV_BACKSLASH, true},
            {'`', EV_GRAVE, false},    {'~', EV_GRAVE, true},
            {'-', EV_MINUS, false},    {'_', EV_MINUS, true},
            {'=', EV_EQUAL, false},    {'+', EV_EQUAL, false},
            {'!', EV_DIGIT1, true},
            {'@', (fdk_scancode)3, true},   /* shift+2 */
            {'#', (fdk_scancode)4, true},   /* shift+3 */
            {'$', (fdk_scancode)5, true},   /* shift+4 */
            {'%', (fdk_scancode)6, true},   /* shift+5 */
            {'^', (fdk_scancode)7, true},   /* shift+6 */
            {'&', (fdk_scancode)8, true},   /* shift+7 */
            {'*', (fdk_scancode)9, true},   /* shift+8 */
            {'(', (fdk_scancode)10, true},  /* shift+9 */
            {')', EV_DIGIT0, true},         /* shift+0 */
        };
        for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]);
             i++) {
            if (punct[i].sym == c) {
                *out = punct[i].base;
                return punct[i].shifted ? 2 : 1;
            }
        }
    }
    if (c >= 'a' && c <= 'z') {
        /* ALPHABETICAL index -> that letter's evdev code (KEY_A=30,
         * KEY_B=48, ... — evdev letter codes are NOT alphabetical;
         * the table is). */
        static const fdk_scancode letters[26] = {
            EV_A, EV_B, EV_C, EV_D, EV_E, EV_F, EV_G, EV_H, EV_I,
            EV_J, EV_K, EV_L, EV_M, EV_N, EV_O, EV_P, EV_Q, EV_R,
            EV_S, EV_T, EV_U, EV_V, EV_W, EV_X, EV_Y, EV_Z,
        };
        *out = letters[c - 'a'];
        return 1;
    }
    if (c >= '0' && c <= '9') {
        *out = (fdk_scancode)(c == '0' ? EV_DIGIT0 : (unsigned)(c - '1') + 2u);
        return 1;
    }
    return 0;
}

static bool name_scan(const char *seg, fdk_scancode *out) {
    static const struct {
        const char *name;
        fdk_scancode code;
    } table[] = {
        {"tab", EV_TAB},       {"enter", EV_ENTER},
        {"return", EV_ENTER},  {"esc", EV_ESC},
        {"escape", EV_ESC},    {"space", EV_SPACE},
        {"backspace", EV_BACKSPACE}, {"delete", EV_DELETE},
        {"del", EV_DELETE},    {"insert", EV_INSERT},
        {"ins", EV_INSERT},    {"home", EV_HOME},
        {"end", EV_END},       {"pageup", EV_PAGEUP},
        {"pgup", EV_PAGEUP},   {"pagedown", EV_PAGEDOWN},
        {"pgdn", EV_PAGEDOWN}, {"left", EV_LEFT},
        {"right", EV_RIGHT},   {"up", EV_UP},
        {"down", EV_DOWN},     {"plus", EV_EQUAL},
        {"minus", EV_MINUS},   {"comma", EV_COMMA},
        {"period", EV_DOT},    {"dot", EV_DOT},
        {"slash", EV_SLASH},
        /* §31/S6: the xkb keysym names — an app that copies the
         * keysym spelling from xkbcommon's header (the question of
         * "Ctrl+question") must not get a parse warning. */
        {"question", EV_SLASH},        /* Shift+slash on US       */
        {"semicolon", EV_SEMICOLON},
        {"apostrophe", EV_APOSTROPHE},
        {"grave", EV_GRAVE},
        {"backslash", EV_BACKSLASH},
        {"bracketleft", EV_BRACKETLEFT},
        {"bracketright", EV_BRACKETRIGHT},
        {"equal", EV_EQUAL},
        {"equals", EV_EQUAL},
        {"prior", EV_PAGEUP},          /* the xkb PageUp spelling */
        {"next", EV_PAGEDOWN},
        /* KP_ keysyms */
        {"kp_add", EV_KPADD},
        {"kp_subtract", EV_KPSUBTRACT},
        {"kp_multiply", EV_KPMULTIPLY},
        {"kp_divide", EV_KPDIVIDE},
        {"kp_enter", EV_KPENTER},
        {"kp_0", EV_KP0}, {"kp_1", EV_KP1}, {"kp_2", EV_KP2},
        {"kp_3", EV_KP3}, {"kp_4", EV_KP4}, {"kp_5", EV_KP5},
        {"kp_6", EV_KP6}, {"kp_7", EV_KP7}, {"kp_8", EV_KP8},
        {"kp_9", EV_KP9},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(seg, table[i].name) == 0) {
            *out = table[i].code;
            return true;
        }
    }
    /* F1..F12. */
    if (seg[0] == 'f' && seg[1] >= '1' && seg[1] <= '9' && seg[2] == '\0') {
        int n = seg[1] - '0';
        *out = (fdk_scancode)(EV_F1 + n - 1);
        return true;
    }
    if (strcmp(seg, "f10") == 0) {
        *out = (fdk_scancode)(EV_F1 + 9);
        return true;
    }
    if (strcmp(seg, "f11") == 0) {
        *out = (fdk_scancode)EV_F11;
        return true;
    }
    if (strcmp(seg, "f12") == 0) {
        *out = (fdk_scancode)EV_F12;
        return true;
    }
    /* F13..F24 (§31/S6): two-digit, contiguous evdev codes. */
    if (seg[0] == 'f' && seg[1] == '1' && seg[2] >= '3' &&
        seg[2] <= '9' && seg[3] == '\0') {
        *out = (fdk_scancode)(EV_F13 + (seg[2] - '3'));
        return true;
    }
    if (strcmp(seg, "f20") == 0) {
        *out = (fdk_scancode)(EV_F13 + 7);
        return true;
    }
    if (seg[0] == 'f' && seg[1] == '2' && seg[2] >= '1' &&
        seg[2] <= '4' && seg[3] == '\0') {
        *out = (fdk_scancode)(EV_F13 + 8 + (seg[2] - '1'));
        return true;
    }
    return false;
}

static bool modifier_scan(const char *seg, fdk_u32 *out_bit) {
    if (strcmp(seg, "ctrl") == 0 || strcmp(seg, "control") == 0) {
        *out_bit = FDK_MOD_CTRL;
        return true;
    }
    if (strcmp(seg, "shift") == 0) {
        *out_bit = FDK_MOD_SHIFT;
        return true;
    }
    if (strcmp(seg, "alt") == 0) {
        *out_bit = FDK_MOD_ALT;
        return true;
    }
    if (strcmp(seg, "super") == 0 || strcmp(seg, "meta") == 0) {
        *out_bit = FDK_MOD_SUPER;
        return true;
    }
    return false;
}

fdk_result fdk_shortcut_parse(const char *spec, fdk_u32 *out_modifiers,
                              fdk_scancode *out_key) {
    if (spec == NULL || out_modifiers == NULL || out_key == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    /* "Ctrl++" — a LITERAL plus as the key. The splitter eats '+'
     * as a separator, so rewrite the trailing "++" to the plus
     * name once, up front (bounded spec, stack copy). Any other
     * doubled plus stays an error ("Ctrl++S" is still garbage). */
    char fixed[96];
    const char *use = spec;
    size_t len = strlen(spec);
    if (len >= 2 && len + 4 < sizeof fixed &&
        spec[len - 1] == '+' && spec[len - 2] == '+') {
        /* "Ctrl++" -> "Ctrl+plus": drop ONE trailing '+', append
         * the plus NAME. "Ctrl++S" keeps its inner "++" (still an
         * error); only the TRAILING double-plus is the literal. */
        memcpy(fixed, spec, len - 1);
        memcpy(fixed + len - 1, "plus", 5);
        use = fixed;
    }
    scan_t s = {use, false};
    char seg[24];
    fdk_u32 mods = 0;
    fdk_scancode key = 0;
    bool have_key = false;

    while (next_segment(&s, seg, sizeof(seg))) {
        trim(seg);
        if (seg[0] == '\0') {
            return FDK_ERR_INVALID_ARGUMENT; /* "Ctrl++S" / "+S" / "S+" */
        }
        fdk_u32 bit = 0;
        if (!have_key && modifier_scan(seg, &bit)) {
            if ((mods & bit) != 0) {
                return FDK_ERR_INVALID_ARGUMENT; /* "Ctrl+Ctrl+S" */
            }
            mods |= bit;
            continue;
        }
        /* Not a modifier (or a modifier name AFTER the key — "S+Ctrl"
         * is garbage, not a second key): it must be the key. */
        if (have_key) {
            return FDK_ERR_INVALID_ARGUMENT; /* two key segments */
        }
        int letter = letter_scan(seg, &key);
        if (letter == 0 && !name_scan(seg, &key)) {
            return FDK_ERR_INVALID_ARGUMENT; /* unknown name */
        }
        if (letter == 2) {
            mods |= FDK_MOD_SHIFT; /* the shifted US glyph ("Ctrl+?") */
        }
        have_key = true;
    }
    if (!have_key) {
        return FDK_ERR_INVALID_ARGUMENT; /* bare modifier / empty spec */
    }
    /* A separator with nothing after it ("S+", "Ctrl+"): the last
     * segment's dangled flag survives to here because no further
     * segment was read to clear it. */
    if (s.dangled) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_modifiers = mods;
    *out_key = key;
    return FDK_OK;
}

/* ---- matching --------------------------------------------------------- */

bool fdk__shortcut_matches(fdk_u32 spec_modifiers, fdk_scancode spec_key,
                           const fdk_key_event *event) {
    if (event == NULL) {
        return false;
    }
    /* EXACT modifier equality: "Ctrl+S" does not fire while Shift is
     * also held (that is "Ctrl+Shift+S", a different binding), and
     * "Ctrl+Shift+S" does not fire on bare Ctrl+S. The four FDK_MOD
     * bits are the whole comparison — lock states (Caps/Num) never
     * appear in fdk_key_event.modifiers on either backend. */
    return event->modifiers == spec_modifiers && event->scancode == spec_key;
}

/* ---- formatting (the parser's exact inverse) ------------------------- */

/* Appends src (len n) into out[cap] at *used; returns false when it
 * would not fit. Bounds-safe strcpy, no snprintf anywhere (the
 * release build's warning set stays empty). */
static bool append_str(char *out, size_t cap, size_t *used,
                       const char *src) {
    size_t n = strlen(src);
    if (*used + n + 1 > cap) {
        return false;
    }
    memcpy(out + *used, src, n);
    *used += n;
    out[*used] = '\0';
    return true;
}

static bool key_name(fdk_scancode code, char *out, size_t cap) {
    static const struct {
        fdk_scancode code;
        const char *name;
    } table[] = {
        {EV_TAB, "tab"},       {EV_ENTER, "enter"},
        {EV_ESC, "esc"},       {EV_SPACE, "space"},
        {EV_BACKSPACE, "backspace"}, {EV_DELETE, "delete"},
        {EV_INSERT, "insert"}, {EV_HOME, "home"},
        {EV_END, "end"},       {EV_PAGEUP, "pageup"},
        {EV_PAGEDOWN, "pagedown"}, {EV_LEFT, "left"},
        {EV_RIGHT, "right"},   {EV_UP, "up"},
        {EV_DOWN, "down"},     {EV_EQUAL, "plus"},
        {EV_MINUS, "minus"},   {EV_COMMA, "comma"},
        {EV_DOT, "period"},    {EV_SLASH, "slash"},
        {EV_SEMICOLON, "semicolon"},
        {EV_APOSTROPHE, "apostrophe"},
        {EV_GRAVE, "grave"},   {EV_BACKSLASH, "backslash"},
        {EV_BRACKETLEFT, "bracketleft"},
        {EV_BRACKETRIGHT, "bracketright"},
        {EV_F11, "f11"},       {EV_F12, "f12"},
        {(fdk_scancode)(EV_F1 + 9), "f10"},
    };
    size_t used = 0;
    out[0] = '\0';
    /* F1..F9 compute (single digit); f10 lives in the table above —
     * '1' + 9 would be ':', not "10". */
    if (code >= EV_F1 && code < (fdk_scancode)(EV_F1 + 9)) {
        char f[3] = {'f', (char)('1' + (code - EV_F1)), '\0'};
        return append_str(out, cap, &used, f);
    }
    if (code >= EV_F13 && code <= EV_F24) {
        char f[4] = {'f', '1' + (char)(code - EV_F13) / 10,
                     '3' + (char)(code - EV_F13) % 10, '\0'};
        if (code == EV_F13 + 7) { /* 20: the only one ending in 0 */
            f[1] = '2';
            f[2] = '0';
        } else if (code >= EV_F13 + 8) { /* 21..24 */
            f[1] = '2';
            f[2] = (char)('1' + (code - (EV_F13 + 8)));
        }
        return append_str(out, cap, &used, f);
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].code == code) {
            return append_str(out, cap, &used, table[i].name);
        }
    }
    /* Letters and digits: reverse of the QWERTY rows. */
    static const char *rows[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const fdk_scancode row0 = EV_Q, row1 = EV_A, row2 = EV_Z;
    if (code >= row0 && code <= (fdk_scancode)(row0 + 9)) {
        char c[2] = {rows[0][code - row0], '\0'};
        return append_str(out, cap, &used, c);
    }
    if (code >= row1 && code <= (fdk_scancode)(row1 + 8)) {
        char c[2] = {rows[1][code - row1], '\0'};
        return append_str(out, cap, &used, c);
    }
    if (code >= row2 && code <= (fdk_scancode)(row2 + 6)) {
        char c[2] = {rows[2][code - row2], '\0'};
        return append_str(out, cap, &used, c);
    }
    if (code >= 2 && code <= 10) {
        char c[2] = {(char)('1' + (code - 2)), '\0'};
        return append_str(out, cap, &used, c);
    }
    if (code == EV_DIGIT0) {
        return append_str(out, cap, &used, "0");
    }
    return false;
}

bool fdk_shortcut_format(fdk_u32 modifiers, fdk_scancode key,
                         char *out_spec, size_t spec_max) {
    if (out_spec == NULL || spec_max == 0) {
        return false;
    }
    char keybuf[16];
    if (!key_name(key, keybuf, sizeof(keybuf))) {
        return false; /* scancode outside the grammar: no spec exists */
    }
    /* Unknown modifier bits set: the pair is not expressible. */
    fdk_u32 known = FDK_MOD_CTRL | FDK_MOD_SHIFT | FDK_MOD_ALT |
                    FDK_MOD_SUPER;
    if ((modifiers & ~known) != 0) {
        return false;
    }
    static const struct {
        fdk_u32 bit;
        const char *name;
    } mods[] = {
        {FDK_MOD_CTRL, "ctrl+"}, {FDK_MOD_SHIFT, "shift+"},
        {FDK_MOD_ALT, "alt+"},   {FDK_MOD_SUPER, "super+"},
    };
    char buf[FDK_SHORTCUT_SPEC_MAX];
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        if ((modifiers & mods[i].bit) != 0) {
            if (!append_str(buf, sizeof(buf), &used, mods[i].name)) {
                return false; /* cannot happen: 4 mods + key < 32 bytes */
            }
        }
    }
    if (!append_str(buf, sizeof(buf), &used, keybuf)) {
        return false;
    }
    if (used + 1 > spec_max) {
        return false; /* caller's buffer too small */
    }
    memcpy(out_spec, buf, used + 1);
    return true;
}
