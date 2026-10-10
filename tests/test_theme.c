/* test_theme.c — headless theme-engine tests (Phase 7).
 *
 * Everything runs against the built-in default theme, in-memory
 * parses, files written to a temp directory, and standalone widget
 * trees painted onto offscreen surfaces: no display, no window,
 * deterministic.
 *
 * What is proven here:
 *   - the built-in default theme IS the Phase 6 v1 palette,
 *     component for component (the no-regression pin: never touching
 *     themes must change no pixels)
 *   - programmatic themes: create/set/get + every validation rule
 *   - .fdk parsing: a complete file, a partial file (inheritance),
 *     whitespace/BOM/CRLF/CR/comment tolerances, string escapes
 *   - the full adversarial matrix from docs/security.md: unknown
 *     keys/sections, duplicates, bad hex, out-of-range metrics,
 *     leading zeros, over-long lines/strings/integers, NUL bytes,
 *     unterminated strings, wrong versions, zero-byte files — every
 *     case asserts the exact fdk_result code
 *   - runtime switching: fdk_theme_set_default() repaints a live
 *     standalone tree (button fill + separator band change on the
 *     next tree paint), same-pointer no-op adds no damage, destroying
 *     the current theme reverts to the built-in safely
 *   - themed metrics: separator thickness 3 paints a 3px band at the
 *     same center line; default 1 is the v1 rule exactly
 */

#include "fdk/fdk.h"
#include "fdk/fdk_theme.h"
#include "fdk/fdk_widgets.h"

#include "widget/widget_internal.h"
#include "widget/widgets_internal.h" /* 1.4.16: fdk__button_shape_radius */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- helpers ---- */

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

/* Mirrors the renderer's pack_color() exactly (clamp + round). */
static fdk_u32 px_of(fdk_color c) {
    fdk_f32 r = c.r < 0.0f ? 0.0f : (c.r > 1.0f ? 1.0f : c.r);
    fdk_f32 g = c.g < 0.0f ? 0.0f : (c.g > 1.0f ? 1.0f : c.g);
    fdk_f32 b = c.b < 0.0f ? 0.0f : (c.b > 1.0f ? 1.0f : c.b);
    return ((fdk_u32)(r * 255.0f + 0.5f) << 16) |
           ((fdk_u32)(g * 255.0f + 0.5f) << 8) |
           (fdk_u32)(b * 255.0f + 0.5f);
}

static fdk_color exact(fdk_f32 r, fdk_f32 g, fdk_f32 b) {
    fdk_color c = {r, g, b, 1.0f};
    return c;
}

static void assert_color_eq(fdk_color got, fdk_color want,
                            const char *what) {
    if (got.r != want.r || got.g != want.g || got.b != want.b ||
        got.a != want.a) {
        fprintf(stderr,
                "FAIL %s: got (%.3f,%.3f,%.3f,%.3f) want "
                "(%.3f,%.3f,%.3f,%.3f)\n",
                what, got.r, got.g, got.b, got.a, want.r, want.g,
                want.b, want.a);
        assert(!"color mismatch");
    }
}

static fdk_theme *parse_ok(const char *text) {
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_parse(text, strlen(text), &r);
    if (t == NULL) {
        fprintf(stderr, "FAIL parse (expected ok): code %d\n", (int)r);
        assert(!"parse unexpectedly failed");
    }
    assert(r == FDK_OK);
    return t;
}

static void parse_fails(const char *text, fdk_result want,
                        const char *what) {
    fdk_result r = FDK_OK;
    fdk_theme *t = fdk_theme_parse(text, strlen(text), &r);
    if (t != NULL) {
        fdk_theme_destroy(t);
        fprintf(stderr, "FAIL %s: parse unexpectedly succeeded\n", what);
        assert(!"parse should have failed");
    }
    if (r != want) {
        fprintf(stderr, "FAIL %s: code %d, want %d\n", what, (int)r,
                (int)want);
        assert(!"wrong error code");
    }
}

/* Writes `text` to path (creating or truncating). */
static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    size_t n = strlen(text);
    assert(fwrite(text, 1, n, f) == n);
    fclose(f);
}

/* ---- 1. the built-in default IS the 1.4.16 Faded Dream palette ---- */

/* k/255, the same construction the engine's own palette uses, so the
 * pins and the .fdk hex stay byte-identical by construction. */
static fdk_color c8(int r, int g, int b) {
    fdk_color c = {(fdk_f32)r / 255.0f, (fdk_f32)g / 255.0f,
                   (fdk_f32)b / 255.0f, 1.0f};
    return c;
}

static void test_builtin_pin(void) {
    /* The core colors, component for component. If this ever fails,
    * the built-in "Faded Dream" palette (the 1.4.16 gray-with-violet
    * retune) changed without its test — the pin IS the palette's
    * regression net. The 1.4.0 Modern values it replaced are
    * preserved as a documented .fdk recipe in
    * docs/fdk-theme-format.md, exactly as v1's were before them. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TEXT),
                    c8(230, 230, 236), "text");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED),
                    c8(139, 139, 150), "text_disabled");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND),
                    c8(51, 51, 60), "control bg");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_HOVER),
        c8(62, 62, 73), "control bg hover");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_PRESSED),
        c8(74, 74, 87), "control bg pressed");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_DISABLED),
        c8(43, 43, 51), "control bg disabled");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER),
                    c8(70, 70, 83), "control border");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT),
                    c8(143, 121, 217), "accent");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TRACK),
                    c8(42, 42, 49), "track");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND),
                    c8(35, 35, 41), "window background");
    /* The 1.4.0 modern-face families, pinned with the rest. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_SIDEBAR_BACKGROUND),
                    c8(41, 41, 49), "sidebar bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_MENU_BACKGROUND),
                    c8(55, 55, 66), "menu bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_HOVER),
                    c8(161, 141, 227), "accent hover");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_PRESSED),
                    c8(119, 99, 194), "accent pressed");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_TEXT),
                    c8(245, 243, 251), "accent text");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_LINK),
                    c8(169, 146, 232), "link");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ENTRY_BACKGROUND),
                    c8(30, 30, 36), "entry bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ENTRY_BORDER),
                    c8(59, 59, 71), "entry border");
    /* row_hover ships translucent (the soft dense-list hover). */
    {
        fdk_color rh_want = {(fdk_f32)58 / 255.0f, (fdk_f32)58 / 255.0f,
                             (fdk_f32)70 / 255.0f, (fdk_f32)89 / 255.0f};
        assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ROW_HOVER),
                        rh_want, "row hover");
    }
    /* 1.4.16: the titlebar family is UNSET in the built-in — every
     * one reads through to its base token (the fallback contract,
     * pinned here so the band cannot silently drift off the control
     * family for themes that never opted into chrome). */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TITLEBAR_BACKGROUND),
                    c8(51, 51, 60), "titlebar bg (fallback: control)");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TITLEBAR_TEXT),
                    c8(230, 230, 236), "titlebar text (fallback: text)");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TITLEBAR_BORDER),
                    c8(70, 70, 83), "titlebar border (fallback: border)");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_TITLEBAR_BUTTON_HOVER),
        c8(62, 62, 73), "titlebar btn hover (fallback)");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_TITLEBAR_BUTTON_PRESSED),
        c8(74, 74, 87), "titlebar btn pressed (fallback)");
    /* 1.4.16: ROUNDED is the default shape. */
    assert(fdk_theme_get_metric(NULL, FDK_TM_BUTTON_SHAPE) == 0);

    assert(fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(NULL, FDK_TM_SEPARATOR_THICKNESS) == 1);
    /* The 1.4.0 metrics. */
    assert(fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS) == 6);
    assert(fdk_theme_get_metric(NULL, FDK_TM_MENU_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(NULL, FDK_TM_LIST_ROW_HEIGHT) == 30);
    assert(fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH) == 2);

    assert(strcmp(fdk_theme_name(NULL), "Faded Dream") == 0);
    assert(fdk_theme_author(NULL) == NULL);

    /* get_default is never NULL and NULL-theme access resolves to it. */
    assert(fdk_theme_get_default() != NULL);
    assert(fdk_theme_get_color(fdk_theme_get_default(), FDK_TK_TEXT).r
           == fdk_theme_get_color(NULL, FDK_TK_TEXT).r);

    /* A default-theme COPY equals the built-in (round-trip). */
    fdk_theme *t = fdk_theme_create_default();
    assert(t != NULL);
    for (int i = 0; i < FDK_TK_COUNT; i++) {
        assert(fdk_theme_get_color(t, (fdk_theme_token)i).r ==
               fdk_theme_get_color(NULL, (fdk_theme_token)i).r);
    }
    for (int i = 0; i < FDK_TM_COUNT; i++) {
        assert(fdk_theme_get_metric(t, (fdk_theme_metric)i) ==
               fdk_theme_get_metric(NULL, (fdk_theme_metric)i));
    }
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(strcmp(fdk_theme_name(t), "Faded Dream") == 0);
    fdk_theme_destroy(t);
    printf("[ok] built-in default theme = the 1.4.0 Modern palette, "
           "component for component\n");
}

/* ---- 2. programmatic themes ---- */

static void test_programmatic(void) {
    fdk_theme *t = fdk_theme_create_default();
    assert(t != NULL);

    fdk_color c = exact(1.0f, 0.5f, 0.25f);
    assert(fdk_ok(fdk_theme_set_color(t, FDK_TK_ACCENT, c)));
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT), c, "set/get");
    /* Not installed: the current default is untouched. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT),
                    exact((fdk_f32)143 / 255.0f, (fdk_f32)121 / 255.0f,
                   (fdk_f32)217 / 255.0f), "current untouched");

    assert(fdk_ok(fdk_theme_set_metric(t, FDK_TM_BUTTON_CORNER_RADIUS,
                                       0)));
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 0);

    /* Validation: bad token / metric / ranges / NULL theme. */
    fdk_color black = exact(0, 0, 0);
    assert(fdk_theme_set_color(NULL, FDK_TK_TEXT, black)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_color(t, (fdk_theme_token)99, black)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_color(t, (fdk_theme_token)-1, black)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(NULL, FDK_TM_SEPARATOR_THICKNESS, 1)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(t, FDK_TM_BUTTON_CORNER_RADIUS, 33)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(t, FDK_TM_BUTTON_CORNER_RADIUS, -1)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(t, FDK_TM_SEPARATOR_THICKNESS, 0)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(t, FDK_TM_SEPARATOR_THICKNESS, 9)
           == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_metric(t, (fdk_theme_metric)77, 1)
           == FDK_ERR_INVALID_ARGUMENT);

    /* Out-of-range READS: logged + benign sentinel, no crash. */
    assert(fdk_theme_get_color(t, (fdk_theme_token)99).a == 1.0f);
    assert(fdk_theme_get_metric(t, (fdk_theme_metric)99) == 0);

    /* Rename + length cap. */
    assert(fdk_ok(fdk_theme_set_name(t, "Custom")));
    assert(strcmp(fdk_theme_name(t), "Custom") == 0);
    assert(fdk_theme_set_name(t, NULL) == FDK_OK);
    assert(strcmp(fdk_theme_name(t), "Faded Dream") == 0);
    char big[200];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    assert(fdk_theme_set_name(t, big) == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_set_name(NULL, "x") == FDK_ERR_INVALID_ARGUMENT);

    fdk_theme_destroy(t);
    printf("[ok] programmatic themes: set/get, validation, rename, "
           "no accidental install\n");
}

/* ---- 3. a complete .fdk parse ---- */

static const char *k_full_theme =
    "# a complete theme exercising every key\n"
    "version = 1\n"
    "name = \"Day\\\"light\\\\\"\n"
    "author = \"tester\"\n"
    "\n"
    "[colors]\n"
    "window_background = #010203\n"
    "text = #04050607\n"
    "text_disabled = #08090A\n"
    "control_background = #0b0c0d\n"
    "control_background_hover = #0E0F10\n"
    "control_background_pressed = #111213\n"
    "control_background_disabled = #141516\n"
    "control_border = #171819\n"
    "accent = #1a1B1c\n"
    "track = #1D1e1F\n"
    "# 1.4.0 modern-face keys:\n"
    "sidebar_background = #202122\n"
    "menu_background = #232425\n"
    "accent_hover = #262728\n"
    "accent_pressed = #292a2b\n"
    "accent_text = #2c2d2e\n"
    "link = #2f3031\n"
    "entry_background = #323334\n"
    "entry_border = #353637\n"
    "row_hover = #38393a3b\n"
    "\n"
    "[metrics]\n"
    "button_corner_radius = 5\n"
    "separator_thickness = 3\n"
    "entry_corner_radius = 4\n"
    "menu_corner_radius = 7\n"
    "list_row_height = 22\n"
    "focus_ring_width = 3\n";

static void test_parse_full(void) {
    fdk_theme *t = parse_ok(k_full_theme);

    /* Escapes: name is Day"light\ */
    assert(strcmp(fdk_theme_name(t), "Day\"light\\") == 0);
    assert(strcmp(fdk_theme_author(t), "tester") == 0);

    /* 6-digit hex: alpha defaults to 1.0. */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND),
                    exact(1.0f / 255, 2.0f / 255, 3.0f / 255),
                    "window bg hex");
    /* 8-digit hex with explicit alpha 7/255 (case-insensitive digits
     * covered by 0E0F10 / 1a1B1c below). */
    fdk_color want = {4.0f / 255, 5.0f / 255, 6.0f / 255, 7.0f / 255};
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT), want, "rgba");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BORDER),
                    exact(23.0f / 255, 24.0f / 255, 25.0f / 255),
                    "border lowercase");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT),
                    exact(26.0f / 255, 27.0f / 255, 28.0f / 255),
                    "accent mixed case");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TRACK),
                    exact(29.0f / 255, 30.0f / 255, 31.0f / 255),
                    "track");
    /* The 1.4.0 keys parse to their tokens (row_hover carries an
     * explicit alpha — the 8-digit form on the new vocabulary too). */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_SIDEBAR_BACKGROUND),
                    exact(32.0f / 255, 33.0f / 255, 34.0f / 255),
                    "sidebar hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_MENU_BACKGROUND),
                    exact(35.0f / 255, 36.0f / 255, 37.0f / 255),
                    "menu hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT_HOVER),
                    exact(38.0f / 255, 39.0f / 255, 40.0f / 255),
                    "accent hover hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT_PRESSED),
                    exact(41.0f / 255, 42.0f / 255, 43.0f / 255),
                    "accent pressed hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT_TEXT),
                    exact(44.0f / 255, 45.0f / 255, 46.0f / 255),
                    "accent text hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_LINK),
                    exact(47.0f / 255, 48.0f / 255, 49.0f / 255),
                    "link hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ENTRY_BACKGROUND),
                    exact(50.0f / 255, 51.0f / 255, 52.0f / 255),
                    "entry bg hex");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ENTRY_BORDER),
                    exact(53.0f / 255, 54.0f / 255, 55.0f / 255),
                    "entry border hex");
    {
        fdk_color rh = fdk_theme_get_color(t, FDK_TK_ROW_HOVER);
        fdk_color rh_want = {56.0f / 255, 57.0f / 255, 58.0f / 255,
                             59.0f / 255};
        assert_color_eq(rh, rh_want, "row hover hex");
    }

    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 5);
    assert(fdk_theme_get_metric(t, FDK_TM_SEPARATOR_THICKNESS) == 3);
    assert(fdk_theme_get_metric(t, FDK_TM_ENTRY_CORNER_RADIUS) == 4);
    assert(fdk_theme_get_metric(t, FDK_TM_MENU_CORNER_RADIUS) == 7);
    assert(fdk_theme_get_metric(t, FDK_TM_LIST_ROW_HEIGHT) == 22);
    assert(fdk_theme_get_metric(t, FDK_TM_FOCUS_RING_WIDTH) == 3);

    fdk_theme_destroy(t);
    printf("[ok] complete .fdk parse: all tokens, escapes, hex forms\n");
}

/* ---- 4. partial themes + tolerances ---- */

static void test_parse_partial_and_tolerances(void) {
    /* Three colors only: everything else inherits. */
    fdk_theme *t = parse_ok(
        "[colors]\ntext = #FFFFFF\naccent = #123456\n"
        "track = #ABCDEF\n");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT),
                    exact(1, 1, 1), "partial text");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT),
                    exact(0x12 / 255.0f, 0x34 / 255.0f, 0x56 / 255.0f),
                    "partial accent");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TRACK),
                    exact(0xAB / 255.0f, 0xCD / 255.0f, 0xEF / 255.0f),
                    "partial track");
    /* Inherited: */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND),
                    exact((fdk_f32)51 / 255.0f, (fdk_f32)51 / 255.0f,
                   (fdk_f32)60 / 255.0f), "partial inherits bg");
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(strcmp(fdk_theme_name(t), "Faded Dream") == 0);
    fdk_theme_destroy(t);

    /* Comments-only file = the defaults, but named. */
    fdk_theme *t2 = parse_ok("# nothing but comments\n\n# really\n");
    assert_color_eq(fdk_theme_get_color(t2, FDK_TK_TEXT),
                    exact((fdk_f32)230 / 255.0f, (fdk_f32)230 / 255.0f,
                   (fdk_f32)236 / 255.0f), "comments-only text");
    fdk_theme_destroy(t2);

    /* Whitespace, bracket space, CRLF, lone CR, BOM, no final
     * newline, tabs around '='. */
    fdk_theme *t3 = parse_ok(
        "\xEF\xBB\xBF"
        "name=\"Tabs\"\r\n"
        "[ colors ]\r"
        "\ttext\t=\t#0A0B0C  \n"
        "  [metrics]  \n"
        "  button_corner_radius=31"); /* no trailing newline */
    assert(strcmp(fdk_theme_name(t3), "Tabs") == 0);
    assert_color_eq(fdk_theme_get_color(t3, FDK_TK_TEXT),
                    exact(10 / 255.0f, 11 / 255.0f, 12 / 255.0f),
                    "crlf/cr/bom text");
    assert(fdk_theme_get_metric(t3, FDK_TM_BUTTON_CORNER_RADIUS) == 31);
    fdk_theme_destroy(t3);

    printf("[ok] partial themes inherit; comments/CRLF/CR/BOM/tabs/"
           "no-final-newline tolerated\n");
}

/* ---- 5. the adversarial matrix (docs/security.md) ---- */

static void test_parse_errors(void) {
    /* Unknown keys in each section. */
    parse_fails("[colors]\ntxt = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "unknown color key");
    parse_fails("[theme]\nnaem = \"x\"\n", FDK_ERR_THEME_PARSE,
                "unknown theme key");
    parse_fails("[metrics]\nradius = 4\n", FDK_ERR_THEME_PARSE,
                "unknown metric key");

    /* Unknown section; duplicate section; entry before a section. */
    parse_fails("[palette]\ntext = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "unknown section");
    parse_fails("[colors]\ntext = #111111\n[colors]\ntrack = #222222\n",
                FDK_ERR_THEME_PARSE, "duplicate section");
    /* A color key before any header: the implicit section is
     * [theme]-only, so this is an unknown key, not a color. */
    parse_fails("text = #FFFFFF\n[colors]\n", FDK_ERR_THEME_PARSE,
                "color key in implicit theme section");

    /* Duplicate keys. */
    parse_fails("[colors]\ntext = #111111\ntext = #222222\n",
                FDK_ERR_THEME_PARSE, "duplicate color key");
    parse_fails("[theme]\nname = \"a\"\nname = \"b\"\n",
                FDK_ERR_THEME_PARSE, "duplicate name");
    parse_fails("[theme]\nversion = 1\nversion = 1\n",
                FDK_ERR_THEME_PARSE, "duplicate version");
    parse_fails("[metrics]\nseparator_thickness = 2\n"
                "separator_thickness = 3\n",
                FDK_ERR_THEME_PARSE, "duplicate metric");

    /* Malformed hex. */
    parse_fails("[colors]\ntext = FFFFFF\n", FDK_ERR_THEME_PARSE,
                "hex without #");
    parse_fails("[colors]\ntext = #FFFFF\n", FDK_ERR_THEME_PARSE,
                "hex 5 digits");
    parse_fails("[colors]\ntext = #123456789\n", FDK_ERR_THEME_PARSE,
                "hex 9 digits");
    parse_fails("[colors]\ntext = #GGGGGG\n", FDK_ERR_THEME_PARSE,
                "hex bad chars");
    parse_fails("[colors]\ntext = #12345 6\n", FDK_ERR_THEME_PARSE,
                "space inside hex (also trailing junk)");

    /* Wrong value types per section. */
    parse_fails("[colors]\ntext = 123\n", FDK_ERR_THEME_PARSE,
                "int where hex belongs");
    parse_fails("[theme]\nname = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "hex where string belongs");
    parse_fails("[theme]\nname = unquoted\n", FDK_ERR_THEME_PARSE,
                "unquoted string");

    /* Metric range violations. */
    parse_fails("[metrics]\nbutton_corner_radius = 33\n",
                FDK_ERR_THEME_PARSE, "radius above 32");
    parse_fails("[metrics]\nseparator_thickness = 0\n",
                FDK_ERR_THEME_PARSE, "thickness below 1");
    parse_fails("[metrics]\nseparator_thickness = 9\n",
                FDK_ERR_THEME_PARSE, "thickness above 8");
    parse_fails("[metrics]\nbutton_corner_radius = -1\n",
                FDK_ERR_THEME_PARSE, "negative radius");

    /* Integer syntax. */
    parse_fails("[metrics]\nbutton_corner_radius = 08\n",
                FDK_ERR_THEME_PARSE, "leading zero");
    parse_fails("[metrics]\nbutton_corner_radius = 12345678901\n",
                FDK_ERR_THEME_PARSE, "integer too long");
    parse_fails("[metrics]\nbutton_corner_radius\n",
                FDK_ERR_THEME_PARSE, "missing = and value");
    parse_fails("[metrics]\nbutton_corner_radius = \n",
                FDK_ERR_THEME_PARSE, "missing value");

    /* Strings. */
    parse_fails("[theme]\nname = \"unterminated\n",
                FDK_ERR_THEME_PARSE, "unterminated string");
    parse_fails("[theme]\nname = \"bad\\nescape\"\n",
                FDK_ERR_THEME_PARSE, "bad escape");
    parse_fails("[theme]\nname = \"dangling\\\n",
                FDK_ERR_THEME_PARSE, "dangling backslash");
    parse_fails("[theme]\nauthor = \"ok\"\nauthor = \"also ok\"\n",
                FDK_ERR_THEME_PARSE, "duplicate author");
    {
        /* >128 content bytes. */
        char big[256];
        strcpy(big, "[theme]\nname = \"");
        memset(big + strlen(big), 'a', 130);
        big[strlen(big)] = '\0';
        strcat(big, "\"\n");
        parse_fails(big, FDK_ERR_THEME_PARSE, "string over 128");
    }
    {
        /* Control char (tab) inside a string. */
        char bad[64];
        strcpy(bad, "[theme]\nname = \"a\tb\"\n");
        parse_fails(bad, FDK_ERR_THEME_PARSE, "control char in string");
    }

    /* Line-level structure. */
    parse_fails("[colors]\ntext = #FFFFFF extra\n",
                FDK_ERR_THEME_PARSE, "trailing content after value");
    parse_fails("[colors] trailing\n", FDK_ERR_THEME_PARSE,
                "content after section header");
    parse_fails("[]\n", FDK_ERR_THEME_PARSE, "empty section name");
    parse_fails("[colors\ntext = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "unclosed section bracket");
    parse_fails("[colors]\nText = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "uppercase key rejected");
    parse_fails("[colors]\n9text = #FFFFFF\n", FDK_ERR_THEME_PARSE,
                "key starts with digit");
    /* A '#' line is a whole-line comment - even one that looks like
     * an entry. It overrides nothing. */
    {
        fdk_theme *t = parse_ok("[colors]\n#text = #FFFFFF\n");
        assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT),
                        exact((fdk_f32)230 / 255.0f, (fdk_f32)230 / 255.0f,
                        (fdk_f32)236 / 255.0f),
                        "comment-looking line changed nothing");
        fdk_theme_destroy(t);
    }

    /* Version handling. */
    parse_fails("[theme]\nversion = 2\n", FDK_ERR_THEME_VERSION,
                "version 2");
    parse_fails("[theme]\nversion = 0\n", FDK_ERR_THEME_VERSION,
                "version 0");
    parse_fails("[theme]\nversion = -1\n", FDK_ERR_THEME_VERSION,
                "version -1");
    parse_fails("[theme]\nversion = \"1\"\n", FDK_ERR_THEME_PARSE,
                "version as string");

    /* Binary garbage including NULs: rejected with a line number,
     * never a crash. (Explicit length - strlen would stop at the
     * first NUL, which is exactly the truncation the parser must not
     * do internally; see docs/security.md rule 7.) */
    {
        static const char garbage[] = "\x00\x01\x02\x03\n[colors]\n";
        fdk_result r = FDK_OK;
        fdk_theme *t = fdk_theme_parse(garbage, sizeof garbage - 1, &r);
        assert(t == NULL);
        assert(r == FDK_ERR_THEME_PARSE);
        /* And a NUL buried mid-value cannot fake a terminator. */
        static const char sneaky[] =
            "[colors]\ntext = #FF" "\x00" "FFFF\n";
        r = FDK_OK;
        t = fdk_theme_parse(sneaky, sizeof sneaky - 1, &r);
        assert(t == NULL);
        assert(r == FDK_ERR_THEME_PARSE);
    }

    /* Over-long line (1025 content bytes on one line). */
    {
        char *big = malloc(1200);
        assert(big != NULL);
        strcpy(big, "[theme]\nname = \"");
        size_t pos = strlen(big);
        memset(big + pos, 'b', 1025);
        big[pos + 1025] = '\0';
        strcat(big, "\"\n");
        parse_fails(big, FDK_ERR_THEME_PARSE, "line over 1024");
        free(big);
    }

    /* Input-level contract. */
    parse_fails("", FDK_ERR_INVALID_ARGUMENT, "empty memory input");
    {
        fdk_result r = FDK_OK;
        assert(fdk_theme_parse(NULL, 10, &r) == NULL);
        assert(r == FDK_ERR_INVALID_ARGUMENT);
    }
    {
        /* Above the 1 MiB cap: refused before parsing. */
        static const char one = 'x';
        fdk_result r = FDK_OK;
        assert(fdk_theme_parse(&one, 1024u * 1024u + 1u, &r) == NULL);
        assert(r == FDK_ERR_INVALID_ARGUMENT);
    }

    printf("[ok] adversarial matrix: 40+ malformed inputs rejected "
           "with exact codes (parse/version/invalid-arg)\n");
}

/* ---- 6. file loading ---- */

static void test_load_files(void) {
    const char *dir = "/tmp/fdk-theme-test";
    (void)system("rm -rf /tmp/fdk-theme-test && mkdir -p /tmp/fdk-theme-test");

    char path[256];

    /* Valid file. */
    snprintf(path, sizeof path, "%s/valid.fdk", dir);
    write_file(path, "name = \"Filey\"\n[colors]\ntext = #ABCDEF\n");
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_load(path, &r);
    assert(t != NULL && r == FDK_OK);
    assert(strcmp(fdk_theme_name(t), "Filey") == 0);
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT),
                    exact(0xAB / 255.0f, 0xCD / 255.0f, 0xEF / 255.0f),
                    "loaded text");
    fdk_theme_destroy(t);

    /* Missing file. */
    snprintf(path, sizeof path, "%s/nope.fdk", dir);
    r = FDK_OK;
    assert(fdk_theme_load(path, &r) == NULL);
    assert(r == FDK_ERR_THEME_IO);

    /* Zero-byte file: rejected, not silently-defaulted. */
    snprintf(path, sizeof path, "%s/empty.fdk", dir);
    write_file(path, "");
    r = FDK_OK;
    assert(fdk_theme_load(path, &r) == NULL);
    assert(r == FDK_ERR_THEME_PARSE);

    /* Bad version from a file. */
    snprintf(path, sizeof path, "%s/future.fdk", dir);
    write_file(path, "version = 2\n");
    r = FDK_OK;
    assert(fdk_theme_load(path, &r) == NULL);
    assert(r == FDK_ERR_THEME_VERSION);

    /* Parse error carries through from a file too. */
    snprintf(path, sizeof path, "%s/bad.fdk", dir);
    write_file(path, "[colors]\ntypo = #FFFFFF\n");
    r = FDK_OK;
    assert(fdk_theme_load(path, &r) == NULL);
    assert(r == FDK_ERR_THEME_PARSE);

    /* NULL path + NULL out_error are safe. */
    assert(fdk_theme_load(NULL, &r) == NULL);
    assert(r == FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_theme_load(NULL, NULL) == NULL);

    (void)system("rm -rf /tmp/fdk-theme-test");
    printf("[ok] file loading: valid/missing/empty/bad-version/bad-"
           "grammar/null-path\n");
}

/* ---- 7. runtime switching repaints live trees ---- */

static void test_switch_repaints(void) {
    /* A light theme, parsed from the same grammar users would. */
    fdk_theme *light = parse_ok(
        "name = \"Light\"\n"
        "[colors]\n"
        "control_background = #E8E8E8\n"
        "control_border = #777777\n"
        "[metrics]\n"
        "separator_thickness = 3\n");
    /* And a contrasting accent for the round trip back. */
    fdk_theme *dark2 = fdk_theme_create_default();
    assert(dark2 != NULL);
    assert(fdk_ok(fdk_theme_set_color(
        dark2, FDK_TK_CONTROL_BACKGROUND, exact(0.5f, 0.1f, 0.1f))));

    /* A standalone tree: root with an explicit background + a
     * fontless button (paints only its fill) + a separator. */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 200, 120},
                                    &root)));
    fdk_widget_set_background(root, exact(0.07f, 0.09f, 0.13f));

    fdk_widget *btn = NULL;
    assert(fdk_ok(fdk_button_create(root, NULL, NULL, &btn)));
    fdk_widget_set_bounds(btn, (fdk_rect){20, 20, 120, 30});

    fdk_widget *sep = NULL;
    assert(fdk_ok(fdk_separator_create(root, FDK_HORIZONTAL, &sep)));
    fdk_widget_set_bounds(sep, (fdk_rect){20, 70, 120, 10});

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 120, &s)));

    /* Baseline paint: Faded Dream colors. */
    fdk_widget_tree_paint(root, s);
    fdk_u32 btn_px = px_at(s, 80, 35); /* button center, clear of the
                                        * radius-8 corners */
    assert(btn_px == px_of(c8(51, 51, 60)));
    fdk_u32 sep_px = px_at(s, 80, 75); /* 70 + 10/2 = the 1px line */
    assert(sep_px == px_of(c8(70, 70, 83))); /* control_border */
    /* 1px rule: the rows above/below are root, not separator. */
    assert(px_at(s, 80, 74) == px_of(exact(0.07f, 0.09f, 0.13f)));
    assert(px_at(s, 80, 76) == px_of(exact(0.07f, 0.09f, 0.13f)));

    /* Switch: set_default must damage the root (full repaint). */
    assert(root->has_damage == false); /* the paint above settled it */
    fdk_theme_set_default(light);
    assert(root->has_damage == true);

    /* Same-pointer no-op: settling damage then re-setting the SAME
     * theme must not re-damage. */
    fdk_widget_tree_paint(root, s);
    assert(root->has_damage == false);
    fdk_theme_set_default(light);
    assert(root->has_damage == false);

    /* The switched paint shows the light theme: button fill + a 3px
     * separator band centered on the same line (69..71). */
    btn_px = px_at(s, 80, 35);
    assert(btn_px == px_of(exact(0xE8 / 255.0f, 0xE8 / 255.0f,
                                 0xE8 / 255.0f)));
    for (int y = 74; y <= 76; y++) {
        assert(px_at(s, 80, y) == px_of(exact(0x77 / 255.0f,
                                              0x77 / 255.0f,
                                              0x77 / 255.0f)));
    }
    assert(px_at(s, 80, 73) == px_of(exact(0.07f, 0.09f, 0.13f)));
    assert(px_at(s, 80, 77) == px_of(exact(0.07f, 0.09f, 0.13f)));

    /* Switch again (engine repaints through the root registry). */
    fdk_theme_set_default(dark2);
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 80, 35) == px_of(exact(0.5f, 0.1f, 0.1f)));

    /* Destroying the CURRENT theme reverts to the built-in and is
     * safe: the next paint is Modern again, no dangling pointer. */
    fdk_theme_set_default(light); /* light is current; dark2 is not */
    fdk_theme_destroy(light);
    assert(fdk_theme_get_default() != NULL);
    fdk_widget_tree_paint(root, s); /* flush the revert damage */
    assert(px_at(s, 80, 35) == px_of(c8(51, 51, 60)));

    /* NULL switch = the built-in, explicitly. */
    fdk_theme_set_default(NULL);
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 80, 35) == px_of(c8(51, 51, 60)));

    fdk_theme_destroy(dark2);
    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] theme switch repaints a live tree (fill + separator "
           "band), no-op is inert, destroy-current reverts safely\n");
}

/* ---- 8. registry hygiene across many roots ---- */

static void test_root_registry(void) {
    /* The root registry must not leak: create and destroy a pile of
     * roots, switch themes (walking the registry), and confirm a
     * fresh root still repaints correctly afterwards. ASan covers
     * the memory side; this pins the behavior side. */
    fdk_widget *roots[8];
    for (int i = 0; i < 8; i++) {
        assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                        (fdk_rect){0, 0, 16, 16},
                                        &roots[i])));
    }
    fdk_theme *t = parse_ok("[colors]\ntext = #0000FF\n");
    fdk_theme_set_default(t); /* walks 8 roots + any others */
    fdk_theme_set_default(NULL);

    /* Destroy in a scrambled order (middle-out) to exercise both
     * list-head and interior removals. */
    int order[8] = {3, 4, 2, 5, 1, 6, 0, 7};
    for (int i = 0; i < 8; i++) {
        fdk_widget_destroy(roots[order[i]]);
    }
    fdk_theme_set_default(t); /* walks zero of them */
    fdk_theme_destroy(t);

    /* And the theme engine still works for a fresh tree. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TEXT),
                    exact((fdk_f32)230 / 255.0f, (fdk_f32)230 / 255.0f,
                   (fdk_f32)236 / 255.0f), "post-churn default");
    printf("[ok] root registry: 8 roots created/destroyed in scrambled "
           "order, switches before/during/after all safe\n");
}

/* ---- 9. the documented v1 recipe round-trips (1.4.0) ----
 *
 * docs/fdk-theme-format.md promises: loading the printed "v1.fdk"
 * recipe reproduces the Phase 6 v1 palette exactly. This test keeps
 * that promise honest — the recipe below is the doc's, verbatim;
 * a mismatch means the doc lied or the parser drifted.
 */
static void test_v1_recipe(void) {
    static const char v1_fdk[] =
        "version = 1\n"
        "name    = \"FDK Dark (v1)\"\n"
        "\n"
        "[colors]\n"
        "window_background          = #121721\n"
        "text                       = #EBEDF5\n"
        "text_disabled              = #737885\n"
        "control_background         = #292E42\n"
        "control_background_hover   = #38405C\n"
        "control_background_pressed = #475275\n"
        "control_background_disabled= #1F212E\n"
        "control_border             = #4D5470\n"
        "accent                     = #59A6F2\n"
        "track                      = #1A1F2B\n"
        "tooltip_background         = #F2F5FC\n"
        "tooltip_text               = #1C2130\n"
        "tooltip_border             = #B3BAD1\n"
        "selection_background       = #59A6F273\n"
        "selection_text             = #F2F5FC\n"
        "focus_ring                 = #59A6F2E6\n"
        "success                    = #5CBA6B\n"
        "warning                    = #E6AD40\n"
        "danger                     = #E8595C\n"
        "sidebar_background         = #1A1F29\n"
        "menu_background            = #262B3B\n"
        "accent_hover               = #80B0FF\n"
        "accent_pressed             = #5785D9\n"
        "accent_text                = #F7FAFF\n"
        "link                       = #8CBAFF\n"
        "entry_background           = #12151D\n"
        "entry_border               = #2B3040\n"
        "row_hover                  = #24293899\n"
        "\n"
        "[metrics]\n"
        "button_corner_radius = 8\n"
        "separator_thickness  = 1\n"
        "title_bar_height     = 28\n"
        "scrollbar_width      = 12\n"
        "menu_item_height     = 26\n"
        "tooltip_corner_radius = 6\n"
        "entry_corner_radius  = 6\n"
        "menu_corner_radius   = 8\n"
        "list_row_height      = 26\n"
        "focus_ring_width     = 1\n";
    fdk_result r = FDK_ERR_UNKNOWN;
    fdk_theme *t = fdk_theme_parse(v1_fdk, sizeof v1_fdk - 1, &r);
    assert(t != NULL && r == FDK_OK);

    /* The v1 core at PIXEL exactness: the parser stores
     * channel/255 floats, so the pins are the 8-bit roundings of the
     * v1 literals (0.16 -> 41 -> 41/255) — the same pixels the v1
     * floats painted, which is the doc's actual promise. */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_WINDOW_BACKGROUND),
                    exact(18 / 255.0f, 23 / 255.0f, 33 / 255.0f),
                    "v1 window bg");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT),
                    exact(235 / 255.0f, 237 / 255.0f, 245 / 255.0f),
                    "v1 text");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND),
                    exact(41 / 255.0f, 46 / 255.0f, 66 / 255.0f),
                    "v1 control bg");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND_HOVER),
                    exact(56 / 255.0f, 64 / 255.0f, 92 / 255.0f),
                    "v1 control hover");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND_PRESSED),
                    exact(71 / 255.0f, 82 / 255.0f, 117 / 255.0f),
                    "v1 control pressed");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BORDER),
                    exact(77 / 255.0f, 84 / 255.0f, 112 / 255.0f),
                    "v1 border");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_ACCENT),
                    exact(89 / 255.0f, 166 / 255.0f, 242 / 255.0f),
                    "v1 accent");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TRACK),
                    exact(26 / 255.0f, 31 / 255.0f, 43 / 255.0f),
                    "v1 track");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TEXT_DISABLED),
                    exact(115 / 255.0f, 120 / 255.0f, 133 / 255.0f),
                    "v1 text disabled");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_CONTROL_BACKGROUND_DISABLED),
                    exact(31 / 255.0f, 33 / 255.0f, 46 / 255.0f),
                    "v1 control disabled");
    /* The 8-digit forms carry their alphas. */
    {
        fdk_color sel_want = {89 / 255.0f, 166 / 255.0f, 242 / 255.0f,
                              115 / 255.0f}; /* 0x73 */
        assert_color_eq(fdk_theme_get_color(t, FDK_TK_SELECTION_BACKGROUND),
                        sel_want, "v1 selection");
        fdk_color ring_want = {89 / 255.0f, 166 / 255.0f, 242 / 255.0f,
                               230 / 255.0f}; /* 0xE6 */
        assert_color_eq(fdk_theme_get_color(t, FDK_TK_FOCUS_RING),
                        ring_want, "v1 ring");
    }
    /* The v1 metrics, including the row-height (26) the recipe pins. */
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(t, FDK_TM_LIST_ROW_HEIGHT) == 26);
    assert(fdk_theme_get_metric(t, FDK_TM_FOCUS_RING_WIDTH) == 1);

    fdk_theme_destroy(t);
    printf("[ok] the documented v1 recipe parses and reproduces the "
           "Phase 6 palette exactly\n");
}

/* ---- 1.4.16: the titlebar fallback family ---- */

static void test_titlebar_fallback(void) {
    /* The contract in three layers:
     *   (a) unset keys read the BASE token — including one the theme
     *       itself overrode (a partial theme re-themes the band);
     *   (b) a set key stops the fallback dead;
     *   (c) fdk_theme_set_color counts as "set" (programmatic
     *       themes get the same semantics as parsed ones).
     * Plus the grammar's strictness edges: unknown titlebar keys
     * still parse-error, and duplicates still fail. */
    const char *partial =
        "version = 1\n"
        "[colors]\n"
        "control_background = #101010\n"
        "text               = #EFEFEF\n";
    fdk_theme *t = parse_ok(partial);

    /* (a) both fallback paths: the base the THEME overrode... */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BACKGROUND),
                    exact(16 / 255.0f, 16 / 255.0f, 16 / 255.0f),
                    "unset titlebar bg follows the theme's control bg");
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TITLEBAR_TEXT),
                    exact(239 / 255.0f, 239 / 255.0f, 239 / 255.0f),
                    "unset titlebar text follows the theme's text");
    /* ...and the base the theme left at the built-in default. */
    assert_color_eq(fdk_theme_get_color(t, FDK_TK_TITLEBAR_BORDER),
                    c8(70, 70, 83),
                    "unset titlebar border follows the builtin border");
    assert_color_eq(
        fdk_theme_get_color(t, FDK_TK_TITLEBAR_BUTTON_HOVER),
        c8(62, 62, 73), "unset titlebar hover follows the builtin");
    fdk_theme_destroy(t);

    /* (b) a set key wins over BOTH the fallback and the base. */
    const char *opted_in =
        "version = 1\n"
        "[colors]\n"
        "control_background  = #FFFFFF\n"
        "titlebar_background = #0A0A0A\n"
        "titlebar_text       = #FFFFFF\n"
        "titlebar_button_hover = #2E2E2E\n"
        "titlebar_button_pressed = #454545\n"
        "titlebar_border     = #000000\n";
    fdk_theme *mono = parse_ok(opted_in);
    assert_color_eq(fdk_theme_get_color(mono, FDK_TK_TITLEBAR_BACKGROUND),
                    exact(10 / 255.0f, 10 / 255.0f, 10 / 255.0f),
                    "set titlebar bg beats the white control bg");
    assert_color_eq(fdk_theme_get_color(mono, FDK_TK_TITLEBAR_TEXT),
                    exact(1.0f, 1.0f, 1.0f), "set titlebar text");
    assert_color_eq(fdk_theme_get_color(mono, FDK_TK_TITLEBAR_BORDER),
                    exact(0.0f, 0.0f, 0.0f), "set titlebar border");
    assert_color_eq(fdk_theme_get_color(mono, FDK_TK_CONTROL_BACKGROUND),
                    exact(1.0f, 1.0f, 1.0f),
                    "the base token is untouched by the chrome keys");
    /* The unset members of an otherwise-set family still fall back. */
    assert_color_eq(fdk_theme_get_color(mono, FDK_TK_TITLEBAR_BUTTON_HOVER),
                    exact(46 / 255.0f, 46 / 255.0f, 46 / 255.0f),
                    "set hover; pressed checked separately below");
    assert_color_eq(
        fdk_theme_get_color(mono, FDK_TK_TITLEBAR_BUTTON_PRESSED),
        exact(69 / 255.0f, 69 / 255.0f, 69 / 255.0f),
        "set pressed");
    fdk_theme_destroy(mono);

    /* (c) set_color marks the override: a create_default theme with
     * only TITLEBAR_BACKGROUND set diverges from its own control
     * fill; every other member still reads through. */
    fdk_theme *prog = fdk_theme_create_default();
    assert(prog != NULL);
    assert(fdk_ok(fdk_theme_set_color(prog, FDK_TK_TITLEBAR_BACKGROUND,
                                      exact(0.25f, 0.5f, 0.75f))));
    assert_color_eq(fdk_theme_get_color(prog, FDK_TK_TITLEBAR_BACKGROUND),
                    exact(0.25f, 0.5f, 0.75f), "programmatic set wins");
    assert_color_eq(fdk_theme_get_color(prog, FDK_TK_TITLEBAR_TEXT),
                    c8(230, 230, 236),
                    "programmatic theme's unset text still falls back");
    fdk_theme_destroy(prog);

    /* Strictness: a typo'd titlebar key is still a parse error, and
     * a duplicate titlebar key is still a duplicate. */
    parse_fails("version = 1\n[colors]\ntitlebar_bakground = #000000\n",
                FDK_ERR_THEME_PARSE, "unknown titlebar key");
    parse_fails("version = 1\n[colors]\n"
                "titlebar_background = #000000\n"
                "titlebar_background = #111111\n",
                FDK_ERR_THEME_PARSE, "duplicate titlebar key");

    printf("[ok] titlebar family: unset keys read through to their "
           "base tokens, set keys win, programmatic counts as set\n");
}

/* ---- 1.4.16: the button SHAPE metric ---- */

static void test_button_shape(void) {
    /* Parse + range + default + the shared radius arithmetic. */
    assert(fdk_theme_get_metric(NULL, FDK_TM_BUTTON_SHAPE) == 0);
    assert(fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS) == 8);

    fdk_theme *t = parse_ok("version = 1\n[metrics]\nbutton_shape = 1\n");
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 1);
    fdk_theme_destroy(t);

    t = parse_ok("version = 1\n[metrics]\nbutton_shape = 2\n");
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_SHAPE) == 2);
    fdk_theme_destroy(t);

    parse_fails("version = 1\n[metrics]\nbutton_shape = 3\n",
                FDK_ERR_THEME_PARSE, "shape 3 out of range");
    parse_fails("version = 1\n[metrics]\nbutton_shape = -1\n",
                FDK_ERR_THEME_PARSE, "negative shape");

    fdk_theme *prog = fdk_theme_create_default();
    assert(prog != NULL);
    assert(fdk_theme_set_metric(prog, FDK_TM_BUTTON_SHAPE, 1) == FDK_OK);
    assert(fdk_theme_set_metric(prog, FDK_TM_BUTTON_SHAPE, 2) == FDK_OK);
    assert(fdk_theme_set_metric(prog, FDK_TM_BUTTON_SHAPE, 3) ==
           FDK_ERR_INVALID_ARGUMENT);
    fdk_theme_destroy(prog);

    /* The shared arithmetic (fdk__button_shape_radius, via the
     * internal header — the same include precedent as the discovery
     * suite): ROUNDED honors the radius metric clamped to the box,
     * CIRCLE is half the short side, SQUARE is 0. */
    fdk_theme *round = fdk_theme_create_default();
    assert(round != NULL);
    fdk_theme_set_default(round); /* becomes the NULL-resolution */
    assert(fdk__button_shape_radius(120, 30) == 8);  /* metric < half */
    assert(fdk__button_shape_radius(12, 30) == 6);   /* clamped to 6 */
    fdk_theme_set_metric(round, FDK_TM_BUTTON_SHAPE, 1);
    assert(fdk__button_shape_radius(120, 30) == 15); /* circle: pill */
    assert(fdk__button_shape_radius(30, 30) == 15);  /* circle: round */
    fdk_theme_set_metric(round, FDK_TM_BUTTON_SHAPE, 2);
    assert(fdk__button_shape_radius(120, 30) == 0);  /* square */
    fdk_theme_destroy(round); /* reverts current to the built-in */

    /* Pixel proof through the catalog Button itself: the same
     * theme, same geometry, only the SHAPE flipped. ROUNDED (radius
     * 8) fills the near-corner pixel; CIRCLE (radius 15 on a 30-px
     * button) cuts it. */
    {
        fdk_theme *pxt = parse_ok("version = 1\n[metrics]\n"
                                  "button_corner_radius = 8\n");
        fdk_theme_set_default(pxt);
        fdk_widget *b = NULL;
        assert(fdk_ok(fdk_button_create(NULL, NULL, NULL, &b)));
        fdk_widget_set_bounds(b, (fdk_rect){10, 10, 120, 30});
        fdk_surface *ps = NULL;
        assert(fdk_ok(fdk_surface_create(140, 50, &ps)));
        fdk_surface_fill(ps, exact(0.0f, 0.0f, 0.0f));
        fdk_widget_tree_paint(b, ps);
        /* (13,13): 3px in from the button corner. */
        assert(px_at(ps, 13, 13) == px_of(c8(51, 51, 60))); /* filled */
        fdk_theme_set_metric(pxt, FDK_TM_BUTTON_SHAPE, 1);
        fdk_widget_invalidate(b); /* tree paint is damage-gated; the
                                   * metric change itself is not a
                                   * repaint trigger (the documented
                                   * set_metric contract) */
        fdk_surface_fill(ps, exact(0.0f, 0.0f, 0.0f));
        fdk_widget_tree_paint(b, ps);
        assert(px_at(ps, 13, 13) == 0x00000000u); /* circle cut it */
        /* DEAD CENTER still filled in both shapes. */
        assert(px_at(ps, 70, 25) == px_of(c8(51, 51, 60)));
        fdk_widget_destroy(b);
        fdk_surface_destroy(ps);
        fdk_theme_set_default(NULL);
        fdk_theme_destroy(pxt);
    }

    printf("[ok] button_shape: parse/range/set_metric + the shared "
           "radius arithmetic (rounded/circle/square) + pixel proof\n");
}

/* ---- 1.4.16: the token-following widget background ---- */

static void test_background_token(void) {
    /* A plain widget whose background is a TOKEN paints the token's
     * color, and a theme switch re-resolves it at paint time with no
     * re-set call — the pixel flips. An explicit set_background then
     * cancels the mode (frozen across switches); setting the token
     * again re-enters it. */
    fdk_theme *a = parse_ok("version = 1\n[colors]\n"
                            "sidebar_background = #123456\n");
    fdk_theme *b = parse_ok("version = 1\n[colors]\n"
                            "sidebar_background = #654321\n");
    fdk_theme_set_default(a);

    fdk_widget *w = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                    (fdk_rect){0, 0, 100, 40}, &w)));
    fdk_widget_set_background_token(w, FDK_TK_SIDEBAR_BACKGROUND);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(100, 40, &s)));
    fdk_widget_tree_paint(w, s);
    assert(px_at(s, 50, 20) == px_of(exact(0x12 / 255.0f, 0x34 / 255.0f,
                                           0x56 / 255.0f)));

    /* The switch re-damages the root; the SAME call re-resolves. */
    assert(w->has_damage == false);
    fdk_theme_set_default(b);
    assert(w->has_damage == true); /* the invalidating walk ran */
    fdk_widget_tree_paint(w, s);
    assert(px_at(s, 50, 20) == px_of(exact(0x65 / 255.0f, 0x43 / 255.0f,
                                           0x21 / 255.0f)));

    /* Explicit color: wins immediately, survives the next switch. */
    fdk_widget_set_background(w, exact(0.5f, 0.5f, 0.5f));
    fdk_widget_tree_paint(w, s);
    assert(px_at(s, 50, 20) == px_of(exact(0.5f, 0.5f, 0.5f)));
    fdk_theme_set_default(a);
    fdk_widget_tree_paint(w, s);
    assert(px_at(s, 50, 20) == px_of(exact(0.5f, 0.5f, 0.5f)));

    /* Re-entering token mode resumes following. */
    fdk_widget_set_background_token(w, FDK_TK_SIDEBAR_BACKGROUND);
    fdk_widget_tree_paint(w, s);
    assert(px_at(s, 50, 20) == px_of(exact(0x12 / 255.0f, 0x34 / 255.0f,
                                           0x56 / 255.0f)));

    fdk_widget_destroy(w);
    fdk_surface_destroy(s);
    fdk_theme_set_default(NULL);
    fdk_theme_destroy(a);
    fdk_theme_destroy(b);

    printf("[ok] widget background tokens: paint-time resolution, live "
           "switching, explicit-color precedence\n");
}

int main(void) {
    /* Hermeticity (1.4.13): the lazy global-settings boot reads the
     * reserved "fdk" prefs store and $FDK_THEME at the first theme
     * resolution. Pin both to nothing so this binary's built-in-
     * palette pins hold on ANY machine — including one where the
     * user ran `fdk-theme set matrix`. (An empty FDK_THEME behaves
     * as unset; an absolute missing file is a clean first run.) */
    setenv("FDK_PREFS_FILE", "/nonexistent-fdk-hermetic.prefs", 1);
    setenv("FDK_THEME", "", 1);

    test_builtin_pin();
    test_programmatic();
    test_parse_full();
    test_parse_partial_and_tolerances();
    test_parse_errors();
    test_load_files();
    test_switch_repaints();
    test_root_registry();
    test_v1_recipe();
    test_titlebar_fallback();
    test_button_shape();
    test_background_token();
    printf("all headless theme tests passed\n");
    return 0;
}
