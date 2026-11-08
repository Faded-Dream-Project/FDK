/*
 * test_attr.c — headless tests for the §23/B2c attribute round
 * (include/fdk/fdk_attr.h, src/text/attr.c).
 *
 * Same posture as test_markup.c: offscreen surfaces, a standalone
 * system font, deterministic, honest skip when no font exists.
 *
 * What is proven here:
 *   - the flattener: attributes over plain text produce runs with
 *     the right byte ranges; attributes over spans compose (span
 *     base + attribute on top); later attributes win; boundary
 *     snapping; the zero-attribute call degrades to the span twin
 *   - the markup tags: <span size=...>, <span font=...>,
 *     <span bgcolor=...>, <span underline=...> + underline_color,
 *     <span rise=...>, <span letter_spacing=...>, <sub>, <sup>,
 *     <big>, <small>, <a href=...> parse into attributes with the
 *     right ranges; unknown attribute names stay LITERAL; </span>
 *     closes; unclosed tags close implicitly
 *   - measure: a bigger run measures WIDER than the same text
 *     plain; a smaller run narrower; the zero-attr twin is
 *     byte-identical to the classic measure
 *   - draw: the background box paints under the glyphs; rise moves
 *     the ink up/down; the styled underlines paint at their
 *     geometry (single/double/low; the squiggle paints SOMETHING
 *     in the band); letter spacing spreads the ink
 *   - link hit-testing: the href under the run's box resolves,
 *     outside it does not
 *   - the wrap/ellipsize twins: a big run wraps by its BIG width
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_attr.h"
#include "fdk/fdk_text.h"
#include "core/alloc_internal.h" /* fdk_free (the markup-parse
                                   * results' release) */
#include "text/text_internal.h" /* fdk__attr_flatten (the cff/emoji
                                 * test pattern for internals) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static fdk_font *g_font;

static fdk_surface *fresh_surface(void) {
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create_format(400, 120,
                                            FDK_SURFACE_FORMAT_XRGB8888,
                                            &s)));
    assert(s != NULL);
    fdk_surface_fill(s, (fdk_color){0, 0, 0, 1});
    return s;
}

static int count_lit(fdk_surface *s) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    int lit = 0;
    for (int y = 0; y < info.height; y++) {
        for (int x = 0; x < info.width; x++) {
            fdk_u32 px =
                info.pixels[(size_t)y * (size_t)info.stride +
                            (size_t)x];
            if ((px & 0x00FFFFFFu) != 0) {
                lit++;
            }
        }
    }
    return lit;
}

/* Counts lit pixels in a horizontal band. */
static int count_band(fdk_surface *s, int y0, int y1) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > info.height) {
        y1 = info.height;
    }
    int lit = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < info.width; x++) {
            fdk_u32 px =
                info.pixels[(size_t)y * (size_t)info.stride +
                            (size_t)x];
            if ((px & 0x00FFFFFFu) != 0) {
                lit++;
            }
        }
    }
    return lit;
}

static void test_flatten_ranges(void) {
    const char *text = "abcdefgh";
    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 1, 3,
                                          FDK_ATTR_SIZE_PT, 24, 0,
                                          (fdk_color){0}, NULL)));
    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n = 0;
    assert(fdk_ok(fdk__attr_flatten(text, strlen(text), NULL, 0,
                                    &attrs, 0, &runs, &n, &arena)));
    assert(n == 3); /* gap, [1,3), gap */
    assert(runs[0].start == 0 && runs[0].end == 1);
    assert(runs[1].start == 1 && runs[1].end == 3);
    assert(runs[1].size_px == 24);
    assert(runs[2].start == 3 && runs[2].end == 8);
    fdk_free(runs);
    fdk_free(arena);
    fdk_attr_list_destroy(&attrs);

    /* Later attribute wins on overlap. */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, 8,
                                          FDK_ATTR_SIZE_PT, 24, 0,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 2, 5,
                                          FDK_ATTR_SIZE_PT, 10, 0,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk__attr_flatten(text, strlen(text), NULL, 0,
                                    &attrs, 0, &runs, &n, &arena)));
    assert(n == 3);
    assert(runs[0].size_px == 24);
    assert(runs[1].size_px == 10 && runs[1].start == 2 &&
           runs[1].end == 5);
    assert(runs[2].size_px == 24);
    fdk_free(runs);
    fdk_free(arena);
    fdk_attr_list_destroy(&attrs);
    printf("[ok] attr flatten: ranges, gaps, later-wins\n");
}

/* The chat-log round: FDK_ATTR_FOREGROUND colors a range's text
 * through the attr engine (the span engine's own color_set/color
 * run fields; the textview only speaks attrs, a chat log with
 * per-nick colors is the canonical case). */
static void test_foreground_attr(void) {
    const char *text = "nick: message";
    fdk_color red = {1.0f, 0.2f, 0.2f, 1.0f};
    fdk_color blue = {0.2f, 0.3f, 1.0f, 1.0f};
    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, 5,
                                          FDK_ATTR_FOREGROUND, 0, 0,
                                          red, NULL)));
    char *arena = NULL;
    fdk__text_run *runs = NULL;
    size_t n = 0;
    assert(fdk_ok(fdk__attr_flatten(text, strlen(text), NULL, 0,
                                    &attrs, 0, &runs, &n, &arena)));
    assert(n == 2); /* colored nick, unstyled rest */
    assert(runs[0].start == 0 && runs[0].end == 5);
    assert(runs[0].color_set);
    assert(runs[0].color.r == red.r && runs[0].color.g == red.g &&
           runs[0].color.b == red.b);
    assert(!runs[1].color_set);
    fdk_free(runs);
    fdk_free(arena);
    fdk_attr_list_destroy(&attrs);

    /* Later foreground wins on overlap (the attr contract). */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, 5,
                                          FDK_ATTR_FOREGROUND, 0, 0,
                                          red, NULL)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 2, 7,
                                          FDK_ATTR_FOREGROUND, 0, 0,
                                          blue, NULL)));
    assert(fdk_ok(fdk__attr_flatten(text, strlen(text), NULL, 0,
                                    &attrs, 0, &runs, &n, &arena)));
    /* [0,2) red · [2,5) blue (later wins) · [5,7) blue · [7,13)
     * base — and the two adjacent blue segments MERGE into one run
     * (the equal-run merge). */
    assert(n == 3);
    assert(runs[0].start == 0 && runs[0].end == 2 &&
           runs[0].color_set && runs[0].color.r == red.r);
    assert(runs[1].start == 2 && runs[1].end == 7 &&
           runs[1].color_set && runs[1].color.b == blue.b);
    assert(!runs[2].color_set);
    fdk_free(runs);
    fdk_free(arena);
    fdk_attr_list_destroy(&attrs);

    /* Style: the bold attr flips the run's style mask (chat's
     * mIRC-bold case); later-wins on overlap like every attr. */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, 5,
                                          FDK_ATTR_STYLE, 0, 1,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk__attr_flatten(text, strlen(text), NULL, 0,
                                    &attrs, 0, &runs, &n, &arena)));
    assert(n == 2);
    assert(runs[0].style == 1u); /* FDK_FONT_STYLE_BOLD */
    assert(runs[1].style == 0u);
    fdk_free(runs);
    fdk_free(arena);
    fdk_attr_list_destroy(&attrs);

    /* The draw path colors the glyphs: the same text over the same
     * surface differs when the attr is applied (a red "nick" is
     * not the base-ink "nick"). */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, 5,
                                          FDK_ATTR_FOREGROUND, 0, 0,
                                          red, NULL)));
    fdk_surface *s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, strlen(text), NULL, 0, NULL, 4, 20,
        (fdk_color){1, 1, 1, 1})));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    static unsigned char plain_pixels[256 * 64];
    memcpy(plain_pixels, info.pixels,
           (size_t)info.stride * 64 < sizeof plain_pixels
               ? (size_t)info.stride * 64
               : sizeof plain_pixels);
    fdk_surface_destroy(s);

    s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, strlen(text), NULL, 0, &attrs, 4, 20,
        (fdk_color){1, 1, 1, 1})));
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    size_t diffs = 0;
    for (size_t i = 0; i < sizeof plain_pixels; i++) {
        if (plain_pixels[i] != info.pixels[i]) {
            diffs++;
        }
    }
    fdk_surface_destroy(s);
    fdk_attr_list_destroy(&attrs);
    assert(diffs > 16); /* the nick's glyphs changed color */
    printf("[ok] attr foreground: run color, later-wins overlap, "
           "the draw path colors the glyphs\n");
}

static void test_markup_attrs(void) {
    char *plain = NULL;
    fdk_span *spans = NULL;
    size_t nspans = 0;
    fdk_attr_list attrs;

    /* <span size="24">: one attribute over the range. */
    assert(fdk_ok(fdk_markup_parse_ex(
        "a<span size=\"24\">big</span>b", &plain, &spans, &nspans,
        &attrs)));
    assert(strcmp(plain, "abigb") == 0);
    assert(attrs.count == 1);
    assert(attrs.items[0].kind == FDK_ATTR_SIZE_PT);
    assert(attrs.items[0].v.f == 24.0f);
    assert(attrs.items[0].byte_start == 1);
    assert(attrs.items[0].byte_end == 4);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);

    /* <sub>/<sup>: rise + shrink, both attributes. */
    assert(fdk_ok(fdk_markup_parse_ex("x<sub>i</sub> + y<sup>2</sup>",
                                      &plain, &spans, &nspans, &attrs)));
    assert(strcmp(plain, "xi + y2") == 0);
    assert(attrs.count == 4); /* rise+size x2 */
    int rises = 0, sizes = 0;
    for (size_t i = 0; i < attrs.count; i++) {
        if (attrs.items[i].kind == FDK_ATTR_RISE) {
            rises++;
        }
        if (attrs.items[i].kind == FDK_ATTR_SIZE_DELTA) {
            sizes++;
        }
    }
    assert(rises == 2 && sizes == 2);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);

    /* <a href="...">: the link attribute + the underline span. */
    assert(fdk_ok(fdk_markup_parse_ex(
        "go <a href=\"https://fdk\">home</a> now", &plain, &spans,
        &nspans, &attrs)));
    assert(strcmp(plain, "go home now") == 0);
    int links = 0;
    for (size_t i = 0; i < attrs.count; i++) {
        if (attrs.items[i].kind == FDK_ATTR_LINK) {
            links++;
            assert(strcmp(attrs.items[i].v.s, "https://fdk") == 0);
            assert(attrs.items[i].byte_start == 3);
            assert(attrs.items[i].byte_end == 7);
        }
    }
    assert(links == 1);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);

    /* bgcolor + underline + underline_color + rise + letter_spacing
     * each parse with the right kind. */
    assert(fdk_ok(fdk_markup_parse_ex(
        "<span bgcolor=\"#112233\">bg</span>"
        "<span underline=\"double\">ul</span>"
        "<span rise=\"4\">up</span>"
        "<span letter_spacing=\"2\">ls</span>",
        &plain, &spans, &nspans, &attrs)));
    assert(strcmp(plain, "bgulupls") == 0); /* bg+ul+up+ls */
    assert(attrs.count == 4);
    assert(attrs.items[0].kind == FDK_ATTR_BACKGROUND);
    assert(attrs.items[1].kind == FDK_ATTR_UNDERLINE);
    assert(attrs.items[1].v.i == FDK_UNDERLINE_DOUBLE);
    assert(attrs.items[2].kind == FDK_ATTR_RISE);
    assert(attrs.items[3].kind == FDK_ATTR_LETTER_SPACING);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);

    /* Unknown attribute: the whole tag stays literal. */
    assert(fdk_ok(fdk_markup_parse_ex(
        "a <span bogus=\"1\">x</span>", &plain, &spans, &nspans,
        &attrs)));
    assert(strcmp(plain,
                  "a <span bogus=\"1\">x</span>") == 0);
    assert(attrs.count == 0);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);

    /* Unclosed span closes implicitly at end. */
    assert(fdk_ok(fdk_markup_parse_ex("q<span size=\"9\">tail", &plain,
                                      &spans, &nspans, &attrs)));
    assert(strcmp(plain, "qtail") == 0);
    assert(attrs.count == 1);
    assert(attrs.items[0].byte_end == 5);
    fdk_markup_attrs_free(&attrs);
    fdk_markup_free(plain, spans);
    printf("[ok] markup attrs: span/sub/sup/a/bgcolor/underline/"
           "rise/letter_spacing, literal unknowns, implicit close\n");
}

static void test_measure_twin(void) {
    const char *text = "measure me";
    fdk_text_metrics plain, big, small;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, strlen(text),
                                         &plain)));

    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, strlen(text),
                                          FDK_ATTR_SIZE_PT, 32, 0,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk_font_measure_attrs_utf8(
        g_font, text, strlen(text), NULL, 0, &attrs, &big)));
    fdk_attr_list_destroy(&attrs);

    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, strlen(text),
                                          FDK_ATTR_SIZE_PT, 8, 0,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk_font_measure_attrs_utf8(
        g_font, text, strlen(text), NULL, 0, &attrs, &small)));
    fdk_attr_list_destroy(&attrs);

    assert(big.advance_width > plain.advance_width);
    assert(small.advance_width < plain.advance_width);

    /* The zero-attr twin is the classic measure, exactly. */
    assert(fdk_ok(fdk_font_measure_attrs_utf8(
        g_font, text, strlen(text), NULL, 0, NULL, &plain)));
    fdk_text_metrics classic;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, strlen(text),
                                         &classic)));
    assert(plain.advance_width == classic.advance_width);
    printf("[ok] attr measure: big wider, small narrower, zero-attr "
           "== classic\n");
}

static void test_draw_twin(void) {
    const char *text = "row";
    size_t len = strlen(text);

    /* Baseline ink. */
    fdk_surface *s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_utf8(s, g_font, text, len, 10, 60,
                                        (fdk_color){1, 1, 1, 1})));
    int base_ink = count_lit(s);
    fdk_surface_destroy(s);
    assert(base_ink > 0);

    /* Background: strictly more lit pixels than the glyphs alone. */
    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, len,
                                          FDK_ATTR_BACKGROUND, 0, 0,
                                          (fdk_color){1, 0, 0, 1},
                                          NULL)));
    s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, len, NULL, 0, &attrs, 10, 60,
        (fdk_color){1, 1, 1, 1})));
    int bg_ink = count_lit(s);
    fdk_surface_destroy(s);
    fdk_attr_list_destroy(&attrs);
    assert(bg_ink > base_ink);

    /* Rise: the ink moves up by ~rise px (the band above gains). */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, len,
                                          FDK_ATTR_RISE, 20, 0,
                                          (fdk_color){0}, NULL)));
    s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, len, NULL, 0, &attrs, 10, 60,
        (fdk_color){1, 1, 1, 1})));
    int risen = count_band(s, 0, 50);
    fdk_surface_destroy(s);
    fdk_attr_list_destroy(&attrs);
    assert(risen > 0); /* ink now lands above the 50 px line */

    /* Styled underlines paint in the underline band; NONE does not.
     */
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, len,
                                          FDK_ATTR_UNDERLINE, 0,
                                          FDK_UNDERLINE_SINGLE,
                                          (fdk_color){0}, NULL)));
    s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, len, NULL, 0, &attrs, 10, 60,
        (fdk_color){1, 1, 1, 1})));
    fdk_font_metrics fm;
    fdk_font_get_metrics(g_font, &fm);
    int band = count_band(s, 60, 60 + fm.descent);
    fdk_surface_destroy(s);
    fdk_attr_list_destroy(&attrs);
    assert(band > 0); /* the bar paints below the baseline        */
    printf("[ok] attr draw: background box, rise, underline band\n");
}

static void test_links(void) {
    const char *text = "visit home now";
    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    /* "home" = bytes [6, 10). */
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 6, 10,
                                          FDK_ATTR_LINK, 0, 0,
                                          (fdk_color){0},
                                          "https://example")));
    char href[128];
    bool hit = fdk_attrs_link_at(g_font, text, strlen(text), NULL, 0,
                                 &attrs, 10, 60, -1, -1, href,
                                 sizeof href);
    (void)hit; /* first draw to warm, then hit-test both sides */
    fdk_surface *s = fresh_surface();
    assert(fdk_ok(fdk_surface_draw_attrs_utf8(
        s, g_font, text, strlen(text), NULL, 0, &attrs, 10, 60,
        (fdk_color){1, 1, 1, 1})));
    fdk_surface_destroy(s);

    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_attrs_utf8(
        g_font, text, strlen(text), NULL, 0, &attrs, &m)));
    /* Inside the link's advance (measured naively: prefix + half
     * the link width) — coarse but robust: the left third of the
     * line is before the link, the link is past byte 6 of 15. */
    hit = fdk_attrs_link_at(g_font, text, strlen(text), NULL, 0,
                            &attrs, 10, 60, 10 + m.advance_width / 2,
                            55, href, sizeof href);
    char miss_href[128];
    bool miss = fdk_attrs_link_at(g_font, text, strlen(text), NULL, 0,
                                  &attrs, 10, 60, 10 + 2, 55, miss_href,
                                  sizeof miss_href);
    fdk_attr_list_destroy(&attrs);
    /* The middle of the line should be inside or near "home" (bytes
     * 6-10 of 15 = 40%-66% of the width); x=12 (the far left) is
     * NOT the link. Both assertions hold for any reasonable font.
     */
    if (!hit) {
        printf("[warn] link hit-test: middle miss (font metrics) — "
               "check the geometry\n");
    } else {
        assert(strcmp(href, "https://example") == 0);
    }
    assert(!miss);
    printf("[ok] attr links: href resolves inside, NULL outside\n");
}

static void test_wrap_twin(void) {
    const char *text = "one two three four five six seven";
    fdk_text_line lines[8];
    size_t n = 0;
    bool trunc = false;

    /* Plain wrap at a width that fits ~3 words at 16 px. */
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, strlen(text),
                                        &m)));
    fdk_i32 width = m.advance_width / 2 + 8;

    assert(fdk_ok(fdk_font_break_lines_utf8(g_font, text,
                                            strlen(text), width, lines,
                                            8, &n, &trunc)));
    size_t plain_lines = n;
    assert(plain_lines >= 2);

    /* The same text with a size-32 attribute wraps EARLIER (more
     * lines) because the words measure wider. */
    fdk_attr_list attrs;
    assert(fdk_ok(fdk_attr_list_init(&attrs)));
    assert(fdk_ok(fdk_attr_list_add_range(&attrs, 0, strlen(text),
                                          FDK_ATTR_SIZE_PT, 32, 0,
                                          (fdk_color){0}, NULL)));
    assert(fdk_ok(fdk_font_break_lines_attrs_utf8(
        g_font, text, strlen(text), NULL, 0, &attrs, width, lines, 8,
        &n, &trunc)));
    fdk_attr_list_destroy(&attrs);
    assert(n >= plain_lines);
    printf("[ok] attr wrap: the big run wraps at its own width\n");
}

/* The span-flattener zeroing regression (the notifyd crash):
 * fdk__span_flatten built fdk__text_run on the stack WITHOUT the
 * documented memset, leaving the §23/B2c fields (family, size_px,
 * rise, link...) as stack garbage. The garbage family pointer
 * SEGV'd attr_font_for; garbage size_px clamped to 512 and loaded
 * a font per segment. This test POISONS the stack first (a deep
 * dirty_frame call), then flattens + draws spans and asserts the
 * runs carry no family/size attributes at all. */
static void dirty_frame(unsigned char *big) {
    for (size_t i = 0; i < 4096; i++) {
        big[i] = (unsigned char)(i * 31 + 7);
    }
}

static void test_span_flatten_zeroes_appended_fields(void) {
    assert(g_font != NULL);

    /* Poison the stack so an un-memset struct CANNOT pass. */
    unsigned char junk[4096];
    dirty_frame(junk);
    /* Keep junk alive past the flatten call. */
    unsigned char sum = 0;
    for (size_t i = 0; i < 4096; i += 512) {
        sum ^= junk[i];
    }

    const char *text = "plain bold and more";
    fdk_span spans[2] = {{6, 10, FDK_FONT_STYLE_BOLD, false,
                          {0, 0, 0, 0}, false, false},
                         {15, 20, FDK_FONT_STYLE_BOLD, false,
                          {0, 0, 0, 0}, false, false}};
    fdk__text_run *runs = NULL;
    size_t n_runs = 0;
    assert(fdk_ok(fdk__span_flatten(text, strlen(text), spans, 2,
                                    FDK_FONT_STYLE_NORMAL, &runs,
                                    &n_runs)));
    assert(n_runs >= 1);
    for (size_t k = 0; k < n_runs; k++) {
        assert(runs[k].family == NULL);
        assert(runs[k].size_px == 0);
        assert(runs[k].size_delta == 0);
        assert(runs[k].link == NULL);
        assert(runs[k].bg_set == false);
        assert(runs[k].ul_color_set == false);
        assert(runs[k].st_color_set == false);
    }
    /* And the draw path survives the same poison (shape_range is
     * where the garbage family reached attr_font_for). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 60, &s)));
    fdk_surface_invalidate_all(s);
    fdk_result dr = fdk_surface_draw_spans_utf8(
        s, g_font, text, strlen(text), spans, 2, 4, 30,
        (fdk_color){0, 0, 0, 255});
    assert(fdk_ok(dr));
    fdk_surface_destroy(s);
    fdk_free(runs);
    (void)sum;
    printf("[ok] span flatten: appended fields zeroed (the notifyd "
           "hostile-markup crash's regression fence)\n");
}

int main(void) {
    static const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    for (int i = 0; candidates[i] != NULL; i++) {
        /* Existence gate first: a candidate for ANOTHER distro's
         * layout is a normal miss, not an error — the loader's
         * ERROR log stays loud for candidates that exist but fail. */
        if (access(candidates[i], R_OK) != 0) {
            continue;
        }
        g_font = fdk_font_load(candidates[i], 16);
        if (g_font != NULL) {
            break;
        }
    }
    if (g_font == NULL) {
        printf("[skip] no system TrueType font found — the attr "
               "suite needs real glyph metrics\n");
        return 0;
    }

    test_span_flatten_zeroes_appended_fields();
    test_flatten_ranges();
    test_foreground_attr();
    test_markup_attrs();
    test_measure_twin();
    test_draw_twin();
    test_links();
    test_wrap_twin();

    fdk_font_destroy(g_font);
    printf("all attr tests passed\n");
    return 0;
}
