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

/* ---- 1. the built-in default IS the 1.4.0 Modern palette ---- */

static void test_builtin_pin(void) {
    /* The core colors, component for component. If this ever fails,
    * the built-in "Modern" palette (the 1.4.0 retune of v1) changed
    * without its test — the pin IS the palette's regression net.
    * The v1 values it replaced are preserved as the documented
    * .fdk recipe in docs/fdk-theme-format.md. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TEXT),
                    exact(0.93f, 0.94f, 0.97f), "text");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED),
                    exact(0.44f, 0.46f, 0.52f), "text_disabled");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND),
                    exact(0.13f, 0.15f, 0.20f), "control bg");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_HOVER),
        exact(0.17f, 0.20f, 0.27f), "control bg hover");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_PRESSED),
        exact(0.21f, 0.25f, 0.33f), "control bg pressed");
    assert_color_eq(
        fdk_theme_get_color(NULL, FDK_TK_CONTROL_BACKGROUND_DISABLED),
        exact(0.11f, 0.12f, 0.16f), "control bg disabled");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_CONTROL_BORDER),
                    exact(0.20f, 0.23f, 0.30f), "control border");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT),
                    exact(0.42f, 0.62f, 1.00f), "accent");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_TRACK),
                    exact(0.11f, 0.12f, 0.16f), "track");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND),
                    exact(0.09f, 0.10f, 0.14f), "window background");
    /* The 1.4.0 modern-face families, pinned with the rest. */
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_SIDEBAR_BACKGROUND),
                    exact(0.10f, 0.12f, 0.16f), "sidebar bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_MENU_BACKGROUND),
                    exact(0.15f, 0.17f, 0.23f), "menu bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_HOVER),
                    exact(0.50f, 0.69f, 1.00f), "accent hover");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_PRESSED),
                    exact(0.34f, 0.52f, 0.88f), "accent pressed");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ACCENT_TEXT),
                    exact(0.97f, 0.98f, 1.00f), "accent text");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_LINK),
                    exact(0.55f, 0.73f, 1.00f), "link");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ENTRY_BACKGROUND),
                    exact(0.07f, 0.08f, 0.11f), "entry bg");
    assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ENTRY_BORDER),
                    exact(0.17f, 0.19f, 0.25f), "entry border");
    /* row_hover ships translucent (the soft dense-list hover). */
    {
        fdk_color rh_want = {0.14f, 0.16f, 0.22f, 0.60f};
        assert_color_eq(fdk_theme_get_color(NULL, FDK_TK_ROW_HOVER),
                        rh_want, "row hover");
    }

    assert(fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(NULL, FDK_TM_SEPARATOR_THICKNESS) == 1);
    /* The 1.4.0 metrics. */
    assert(fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS) == 6);
    assert(fdk_theme_get_metric(NULL, FDK_TM_MENU_CORNER_RADIUS) == 8);
    assert(fdk_theme_get_metric(NULL, FDK_TM_LIST_ROW_HEIGHT) == 30);
    assert(fdk_theme_get_metric(NULL, FDK_TM_FOCUS_RING_WIDTH) == 2);

    assert(strcmp(fdk_theme_name(NULL), "FDK Modern") == 0);
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
    assert(strcmp(fdk_theme_name(t), "FDK Modern") == 0);
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
                    exact(0.42f, 0.62f, 1.00f), "current untouched");

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
    assert(strcmp(fdk_theme_name(t), "FDK Modern") == 0);
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
                    exact(0.13f, 0.15f, 0.20f), "partial inherits bg");
    assert(fdk_theme_get_metric(t, FDK_TM_BUTTON_CORNER_RADIUS) == 8);
    assert(strcmp(fdk_theme_name(t), "FDK Modern") == 0);
    fdk_theme_destroy(t);

    /* Comments-only file = the defaults, but named. */
    fdk_theme *t2 = parse_ok("# nothing but comments\n\n# really\n");
    assert_color_eq(fdk_theme_get_color(t2, FDK_TK_TEXT),
                    exact(0.93f, 0.94f, 0.97f), "comments-only text");
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
                        exact(0.93f, 0.94f, 0.97f),
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

    /* Baseline paint: Modern colors. */
    fdk_widget_tree_paint(root, s);
    fdk_u32 btn_px = px_at(s, 80, 35); /* button center, clear of the
                                        * radius-8 corners */
    assert(btn_px == px_of(exact(0.13f, 0.15f, 0.20f)));
    fdk_u32 sep_px = px_at(s, 80, 75); /* 70 + 10/2 = the 1px line */
    assert(sep_px == px_of(exact(0.20f, 0.23f, 0.30f)));
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
    assert(px_at(s, 80, 35) == px_of(exact(0.13f, 0.15f, 0.20f)));

    /* NULL switch = the built-in, explicitly. */
    fdk_theme_set_default(NULL);
    fdk_widget_tree_paint(root, s);
    assert(px_at(s, 80, 35) == px_of(exact(0.13f, 0.15f, 0.20f)));

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
                    exact(0.93f, 0.94f, 0.97f), "post-churn default");
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

int main(void) {
    test_builtin_pin();
    test_programmatic();
    test_parse_full();
    test_parse_partial_and_tolerances();
    test_parse_errors();
    test_load_files();
    test_switch_repaints();
    test_root_registry();
    test_v1_recipe();
    printf("all headless theme tests passed\n");
    return 0;
}
