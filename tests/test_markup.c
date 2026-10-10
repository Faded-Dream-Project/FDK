/*
 * test_markup.c — headless tests for the 1.4.11 markup milestone.
 *
 * Everything runs on offscreen surfaces and standalone fonts: no
 * display, no window, deterministic. The font is DejaVu Sans from
 * the system font directories — if no usable font is present the
 * whole suite honestly skips (the established pattern).
 *
 * What is proven here:
 *   - the tag scanner: plain strings round-trip; b/i/u/s spans land
 *     on the right byte ranges; nesting merges into effective runs;
 *     colors parse (#rrggbb, #rrggbbaa, quoted); entities decode;
 *     unknown tags / stray '<' / unknown entities stay LITERAL;
 *     unclosed tags close implicitly; mismatched closes no-op
 *   - the span entry points: zero spans == the classic measure;
 *     bold runs measure BOLD-wide; kerning resets at run boundaries
 *     (a mixed line's advance is the sum of independent runs, within
 *     rounding); draw paints run colors; underline/strikethrough bar
 *     pixels land where the geometry says
 *   - the wrap/ellipsize span twins: a bold word wraps by its BOLD
 *     width; ellipsize picks prefixes by styled widths
 *   - the widget surface: label markup in NOWRAP/WRAP/ELLIPSIZE
 *     (get_text = plain, natural size = styled width, line count,
 *     the ellipsis prefix); button markup (get_text = plain,
 *     measure = styled); tooltip markup (get_tooltip = plain);
 *     set_text clears markup everywhere
 */

#include "fdk/fdk.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_widgets.h"

#include "widget/widgets_internal.h" /* fdk_box_class_def */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *g_font = NULL;

/* ---- helpers ---- */

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static fdk_color rgb(int r, int g, int b) {
    fdk_color c = { .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                    .b = (fdk_f32)b / 255.0f, .a = 1.0f };
    return c;
}

static bool near(fdk_u32 px, fdk_color c, int tol) {
    int pr = (int)((px >> 16) & 0xFFu), pg = (int)((px >> 8) & 0xFFu);
    int pb = (int)(px & 0xFFu);
    int er = pr - (int)(c.r * 255.0f + 0.5f);
    int eg = pg - (int)(c.g * 255.0f + 0.5f);
    int eb = pb - (int)(c.b * 255.0f + 0.5f);
    return abs(er) <= tol && abs(eg) <= tol && abs(eb) <= tol;
}

static fdk_span span_range(size_t s, size_t e, unsigned style) {
    fdk_span sp = {0};
    sp.byte_start = s;
    sp.byte_end = e;
    sp.style = style;
    sp.color_set = false;
    sp.color = (fdk_color){0, 0, 0, 0};
    sp.underline = false;
    sp.strikethrough = false;
    return sp;
}

/* Parses and checks the plain text; returns the spans. */
static fdk_span *parse_check(const char *markup, const char *want_plain,
                             size_t *out_n) {
    char *plain = NULL;
    fdk_span *spans = NULL;
    size_t n = 0;
    assert(fdk_ok(fdk_markup_parse(markup, &plain, &spans, &n)));
    assert(plain != NULL);
    assert(strcmp(plain, want_plain) == 0);
    fdk_markup_free(plain, NULL);
    *out_n = n;
    return spans; /* caller frees */
}

/* ---- 1. the tag scanner ---- */

static void test_scanner(void) {
    /* Plain: byte-identical, zero spans. */
    size_t n = 0;
    fdk_span *sp = parse_check("just plain text", "just plain text", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Single bold span over the whole string. */
    sp = parse_check("<b>bold</b>", "bold", &n);
    assert(n == 1);
    assert(sp[0].byte_start == 0 && sp[0].byte_end == 4);
    assert(sp[0].style == FDK_FONT_STYLE_BOLD);
    assert(!sp[0].color_set && !sp[0].underline && !sp[0].strikethrough);
    fdk_markup_free(NULL, sp);

    /* Bold in the middle: "plain boldword tail" — the span covers
     * exactly the tagged bytes. */
    sp = parse_check("plain <b>boldword</b> tail",
                     "plain boldword tail", &n);
    assert(n == 1);
    assert(sp[0].byte_start == 6 && sp[0].byte_end == 14);
    fdk_markup_free(NULL, sp);

    /* All four single-letter tags. */
    sp = parse_check("<i>it</i><u>ul</u><s>st</s>", "itulst", &n);
    assert(n == 3);
    assert(sp[0].style == FDK_FONT_STYLE_ITALIC);
    assert(sp[1].underline && sp[1].style == 0);
    assert(sp[2].strikethrough && sp[2].style == 0);
    fdk_markup_free(NULL, sp);

    /* Nesting: bold then bold+italic (the scanner merges). */
    sp = parse_check("<b>bo<i>both</i></b>", "boboth", &n);
    assert(n == 2);
    assert(sp[0].byte_start == 0 && sp[0].byte_end == 2);
    assert(sp[0].style == FDK_FONT_STYLE_BOLD);
    assert(sp[1].byte_start == 2 && sp[1].byte_end == 6);
    assert(sp[1].style ==
           (FDK_FONT_STYLE_BOLD | FDK_FONT_STYLE_ITALIC));
    fdk_markup_free(NULL, sp);

    /* Colors: bare, quoted, alpha. */
    sp = parse_check("<color=#ff0000>red</color>", "red", &n);
    assert(n == 1 && sp[0].color_set);
    assert(sp[0].color.r > 0.99f && sp[0].color.g < 0.01f &&
           sp[0].color.b < 0.01f && sp[0].color.a > 0.99f);
    fdk_markup_free(NULL, sp);

    sp = parse_check("<color='#00ff00aa'>q</color>", "q", &n);
    assert(n == 1 && sp[0].color_set);
    assert(sp[0].color.g > 0.99f && sp[0].color.a > 0.65f &&
           sp[0].color.a < 0.67f); /* 0xaa / 255 */
    fdk_markup_free(NULL, sp);

    /* Nested color inside bold: inner color wins, bold carries. */
    sp = parse_check("<b>x<color=#0000ff>y</color></b>", "xy", &n);
    assert(n == 2);
    assert(sp[0].style == FDK_FONT_STYLE_BOLD && !sp[0].color_set);
    assert(sp[1].style == FDK_FONT_STYLE_BOLD && sp[1].color_set);
    assert(sp[1].color.b > 0.99f);
    fdk_markup_free(NULL, sp);

    /* Entities. */
    sp = parse_check("a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos;",
                     "a&b <c> \"d\" 'e'", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* nbsp decodes to the U+00A0 codepoint (2 bytes). */
    sp = parse_check("x&nbsp;y", "x\xC2\xA0y", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Unknown tag: LITERAL (never vanishes). */
    sp = parse_check("<x>hi</x>", "<x>hi</x>", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Stray '<' and unknown entity: literal. */
    sp = parse_check("a < b &nope; c", "a < b &nope; c", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Unclosed tag closes implicitly at end of string. */
    sp = parse_check("<b>never closed", "never closed", &n);
    assert(n == 1);
    assert(sp[0].byte_start == 0 && sp[0].byte_end == 12);
    fdk_markup_free(NULL, sp);

    /* Mismatched close: LITERAL (nothing vanishes silently — the
     * same rule as unknown tags). */
    sp = parse_check("plain </b> text", "plain </b> text", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Empty markup: empty plain, zero spans. */
    sp = parse_check("", "", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Case-insensitive tags, tolerated whitespace. */
    sp = parse_check("<B >x</B>", "x", &n);
    assert(n == 1 && sp[0].style == FDK_FONT_STYLE_BOLD);
    fdk_markup_free(NULL, sp);

    /* Bad color values degrade to literal text. */
    sp = parse_check("<color=red>r</color>", "<color=red>r</color>", &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);
    sp = parse_check("<color=#12345>r</color>", "<color=#12345>r</color>",
                     &n);
    assert(n == 0);
    fdk_markup_free(NULL, sp);

    /* Argument guard. */
    char *plain = NULL;
    fdk_span *spans = NULL;
    size_t cnt = 0;
    assert(fdk_markup_parse(NULL, &plain, &spans, &cnt) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_markup_parse("x", NULL, &spans, &cnt) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_markup_parse("x", &plain, NULL, &cnt) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_markup_parse("x", &plain, &spans, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    printf("[ok] scanner: tags, nesting, colors, entities, lenient "
           "literals, guards\n");
}

/* ---- 2. span measure / draw ---- */

static void test_span_measure(void) {
    const char *text = "boldword rest";
    size_t len = strlen(text);

    fdk_text_metrics plain_m, span_m;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, len, &plain_m)));

    /* Zero spans: exactly the classic measure. */
    assert(fdk_ok(fdk_font_measure_spans_utf8(g_font, text, len, NULL,
                                              0, &span_m)));
    assert(span_m.advance_width == plain_m.advance_width);
    assert(span_m.ink_top == plain_m.ink_top &&
           span_m.ink_bottom == plain_m.ink_bottom);

    /* A bold span over "boldword": the whole line measures wider
     * than plain (stem dilation widens advances). */
    fdk_span sp = span_range(0, 8, FDK_FONT_STYLE_BOLD);
    assert(fdk_ok(fdk_font_measure_spans_utf8(g_font, text, len, &sp, 1,
                                              &span_m)));
    assert(span_m.advance_width > plain_m.advance_width);

    /* The SAME text fully bold via the classic path (font style)
     * must agree with the span path over the whole string — the
     * variant-keyed cache bakes the same synthesis either way. */
    {
        assert(fdk_ok(fdk_font_set_style(g_font,
                                         FDK_FONT_STYLE_BOLD)));
        fdk_text_metrics bold_m;
        assert(fdk_ok(fdk_font_measure_utf8(g_font, text, len,
                                            &bold_m)));
        fdk_span whole = span_range(0, len, FDK_FONT_STYLE_BOLD);
        fdk_text_metrics whole_sp;
        assert(fdk_ok(fdk_font_measure_spans_utf8(g_font, text, len,
                                                  &whole, 1,
                                                  &whole_sp)));
        assert(whole_sp.advance_width == bold_m.advance_width);
        assert(fdk_ok(fdk_font_set_style(g_font, 0)));
    }

    /* Kerning resets at run boundaries: the mixed line's advance
     * equals regular-prefix + bold-run + regular-suffix measures
     * (each an independent walk), within the 1px rounding budget
     * that one-total-vs-three-totals rounding allows. */
    {
        fdk_span mid = span_range(0, 8, FDK_FONT_STYLE_BOLD);
        assert(fdk_ok(fdk_font_measure_spans_utf8(g_font, text, len,
                                                  &mid, 1, &span_m)));
        fdk_text_metrics a, b, c;
        /* The prefix part shapes BOLD (the span's style), the rest
         * regular — three independent walks, kerning reset between
         * them exactly as the span walk resets at the run boundary. */
        assert(fdk_ok(fdk_font_set_style(g_font,
                                         FDK_FONT_STYLE_BOLD)));
        assert(fdk_ok(fdk_font_measure_utf8(g_font, text, 8, &a)));
        assert(fdk_ok(fdk_font_set_style(g_font, 0)));
        assert(fdk_ok(fdk_font_measure_utf8(g_font, text + 8, 1, &b)));
        assert(fdk_ok(fdk_font_measure_utf8(g_font, text + 9,
                                            len - 9, &c)));
        fdk_i32 parts = a.advance_width + b.advance_width +
                        c.advance_width;
        assert(abs(span_m.advance_width - parts) <= 1);
    }

    printf("[ok] span measure: zero-span identity, bold widening, "
           "style-keyed cache agreement, run-boundary kerning\n");
}

static void test_span_draw(void) {
    /* Red span in the middle, white outside: pixel colors prove the
     * per-run color routing. */
    const char *text = "REDredRED";
    size_t len = strlen(text);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 40, &s)));
    fdk_surface_fill_rect(s, (fdk_rect){0, 0, 200, 40}, rgb(0, 0, 0));

    fdk_span sp = span_range(3, 6, 0);
    sp.color_set = true;
    sp.color = rgb(255, 0, 0);
    fdk_font_metrics fm;
    fdk_font_get_metrics(g_font, &fm);
    fdk_i32 baseline = 30;
    assert(fdk_ok(fdk_surface_draw_spans_utf8(
        s, g_font, text, len, &sp, 1, 10, baseline, rgb(255, 255,
                                                        255))));

    /* Scan the glyph band for red pixels (the middle run) and white
     * pixels (the outer runs). */
    bool saw_red = false, saw_white = false;
    for (int y = baseline - fm.ascent; y < baseline + fm.descent; y++) {
        for (int x = 10; x < 100; x++) {
            fdk_u32 px = px_at(s, x, y);
            if (near(px, rgb(255, 0, 0), 40)) {
                saw_red = true;
            }
            if (near(px, rgb(255, 255, 255), 40)) {
                saw_white = true;
            }
        }
    }
    assert(saw_red);
    assert(saw_white);
    fdk_surface_destroy(s);

    /* Underline bar: SPACES carry no glyph ink, so the ONLY pixels
     * a decorated spaces-run can light up are the bar itself — the
     * bar row below the baseline is green, everything above it is
     * untouched background. (Bars take the run color; glyphs and bar
     * share one color by design — CSS semantics.) */
    const char *spaces = "      ";
    size_t sp_len = strlen(spaces);
    s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 40, &s)));
    fdk_surface_fill_rect(s, (fdk_rect){0, 0, 200, 40}, rgb(0, 0, 0));
    sp = span_range(0, sp_len, 0);
    sp.underline = true;
    assert(fdk_ok(fdk_surface_draw_spans_utf8(
        s, g_font, spaces, sp_len, &sp, 1, 10, baseline, rgb(0, 255, 0))));
    /* The bar sits at baseline + descent/2 (span.c's geometry), one
     * pixel thick at 16px (pixel_size/16 = 1). */
    int bar_y = baseline + (fm.descent / 2 > 0 ? fm.descent / 2 : 1);
    bool bar = false;
    for (int x = 10; x < 10 + 50; x++) {
        if (near(px_at(s, x, bar_y), rgb(0, 255, 0), 40)) {
            bar = true;
            break;
        }
    }
    assert(bar);
    /* Nothing green anywhere ABOVE the bar row: spaces carry no ink
     * and the bar is strictly below the baseline. */
    for (int x = 10; x < 70; x++) {
        for (int y = 0; y < bar_y; y++) {
            assert(!near(px_at(s, x, y), rgb(0, 255, 0), 40));
        }
    }
    fdk_surface_destroy(s);

    /* Strikethrough bar: same spaces trick — only the bar can light
     * up; it crosses at ~30% of the ascent. */
    s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 40, &s)));
    fdk_surface_fill_rect(s, (fdk_rect){0, 0, 200, 40}, rgb(0, 0, 0));
    sp = span_range(0, sp_len, 0);
    sp.strikethrough = true;
    assert(fdk_ok(fdk_surface_draw_spans_utf8(
        s, g_font, spaces, sp_len, &sp, 1, 10, baseline, rgb(0, 255, 0))));
    int strike_y = baseline - (fm.ascent * 3) / 10;
    bar = false;
    for (int x = 10; x < 10 + 60; x++) {
        if (near(px_at(s, x, strike_y), rgb(0, 255, 0), 40)) {
            bar = true;
            break;
        }
    }
    assert(bar);
    fdk_surface_destroy(s);

    printf("[ok] span draw: run colors, underline bar geometry, "
           "strikethrough bar geometry\n");
}

/* ---- 3. span wrap / ellipsize ---- */

static void test_span_wrap_ellipsis(void) {
    /* A long word whose BOLD width exceeds its regular width by a
     * wide margin (stem dilation scales with glyph count). Somewhere
     * between the two widths lies a budget where the plain text
     * still fits N lines but the bold text needs N+1 — the span-
     * aware breaker must find it (styled widths drive the wrap). */
    const char *text = "tiny wwwwwwwwwwwwwwwwwwwwwwwwwwww tail";
    size_t len = strlen(text);
    const char *word = text + 5;
    size_t word_len = strlen(word) - strlen(" tail");
    fdk_text_metrics reg, bold;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, word, word_len,
                                        &reg)));
    assert(fdk_ok(fdk_font_set_style(g_font, FDK_FONT_STYLE_BOLD)));
    assert(fdk_ok(fdk_font_measure_utf8(g_font, word, word_len,
                                        &bold)));
    assert(fdk_ok(fdk_font_set_style(g_font, 0)));
    assert(bold.advance_width - reg.advance_width >= 8);

    fdk_span whole = span_range(0, len, FDK_FONT_STYLE_BOLD);
    bool discriminated = false;
    size_t plain_n = 0, bold_n = 0;
    fdk_i32 found_w = 0;
    for (fdk_i32 w = reg.advance_width;
         w <= bold.advance_width + 40 && !discriminated; w += 2) {
        size_t pn = 0, bn = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(g_font, text, len, w,
                                                NULL, 0, &pn, NULL)));
        assert(fdk_ok(fdk_font_break_lines_spans_utf8(
            g_font, text, len, &whole, 1, w, NULL, 0, &bn, NULL)));
        if (bn > pn) {
            discriminated = true;
            plain_n = pn;
            bold_n = bn;
            found_w = w;
        }
    }
    assert(discriminated);
    assert(bold_n > plain_n);

    /* Zero spans at the same width: the classic walk, exactly. */
    size_t zero_n = 0;
    assert(fdk_ok(fdk_font_break_lines_spans_utf8(
        g_font, text, len, NULL, 0, found_w, NULL, 0, &zero_n, NULL)));
    assert(zero_n == plain_n);

    /* Ellipsize by styled widths: the whole-string-bold text does
     * not fit where the regular text does — the prefix shrinks
     * under bold metrics. */
    fdk_text_metrics plain_whole;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, len,
                                        &plain_whole)));
    fdk_i32 w = plain_whole.advance_width + 4;
    size_t prefix = 0;
    bool fits = true;
    assert(fdk_ok(fdk_font_ellipsize_utf8(g_font, text, len, w,
                                          &prefix, &fits)));
    assert(fits);
    fits = true;
    assert(fdk_ok(fdk_font_ellipsize_spans_utf8(
        g_font, text, len, &whole, 1, w, &prefix, &fits)));
    assert(!fits);
    assert(prefix < len);

    printf("[ok] span wrap + ellipsize: styled widths drive both "
           "passes (discriminating width %d, %zu vs %zu lines)\n",
           found_w, plain_n, bold_n);
}

/* ---- 4. the widget surface ---- */

static void test_label_markup(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, &fdk_box_class_def,
                                    (fdk_rect){0, 0, 400, 300},
                                    &root)));
    fdk_widget *lab = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, NULL, &lab)));

    /* NOWRAP: the natural size uses the styled width. */
    assert(fdk_ok(fdk_label_set_markup(lab, "plain <b>bold</b>")));
    assert(strcmp(fdk_label_get_text(lab), "plain bold") == 0);
    fdk_size styled = {0, 0};
    fdk_widget_measure(lab, &styled);

    fdk_widget *plain_lab = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, "plain bold",
                                   &plain_lab)));
    fdk_size plain = {0, 0};
    fdk_widget_measure(plain_lab, &plain);
    assert(styled.width > plain.width);

    /* Paint smoke: all three modes paint without a crash on an
     * offscreen surface (ASan exercises the run cache). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 300, &s)));
    fdk_widget_tree_paint(root, s);

    /* WRAP: a bold-heavy text wraps into MORE lines than its plain
     * twin at the same width — find a discriminating width with the
     * break engine first (font-metric independent), then let both
     * labels wrap at it. */
    const char *wrap_text = "one two three four five six seven eight";
    size_t wrap_len = strlen(wrap_text);
    fdk_span wrap_all = span_range(0, wrap_len, FDK_FONT_STYLE_BOLD);
    fdk_i32 wrap_w = 0;
    bool found = false;
    for (fdk_i32 w = 90; w <= 200 && !found; w += 5) {
        size_t pn = 0, bn = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(
            g_font, wrap_text, wrap_len, w, NULL, 0, &pn, NULL)));
        assert(fdk_ok(fdk_font_break_lines_spans_utf8(
            g_font, wrap_text, wrap_len, &wrap_all, 1, w, NULL, 0,
            &bn, NULL)));
        if (bn > pn) {
            wrap_w = w;
            found = true;
        }
    }
    assert(found);
    fdk_label_set_mode(lab, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(lab, wrap_w, 0);
    assert(fdk_ok(fdk_label_set_markup(
        lab, "one <b>two three four five six seven eight</b>")));
    fdk_widget_measure(lab, &styled);
    fdk_widget *plain2 = NULL;
    assert(fdk_ok(fdk_label_create(root, g_font, NULL, &plain2)));
    fdk_label_set_mode(plain2, FDK_LABEL_WRAP);
    fdk_widget_set_natural_size(plain2, wrap_w, 0);
    assert(fdk_ok(fdk_label_set_text(
        plain2, "one two three four five six seven eight")));
    fdk_widget_measure(plain2, &plain);
    assert(styled.height > plain.height);
    fdk_widget_tree_paint(root, s);

    /* ELLIPSIZE: get_line_count stays 1; the plain text is the full
     * truth; paint is a smoke pass. */
    fdk_label_set_mode(lab, FDK_LABEL_ELLIPSIZE);
    fdk_widget_set_natural_size(lab, 60, 0);
    assert(fdk_ok(fdk_label_set_markup(
        lab, "<b>AAAAAAAAAAAAAAAAAAAA</b> bbb")));
    fdk_widget_tree_paint(root, s);
    assert(fdk_label_get_line_count(lab) == 1);
    assert(strcmp(fdk_label_get_text(lab),
                  "AAAAAAAAAAAAAAAAAAAA bbb") == 0);

    /* set_text clears the markup: the size returns to the plain
     * twin's (plain_lab stayed NOWRAP all along). */
    assert(fdk_ok(fdk_label_set_text(lab, "plain bold")));
    fdk_widget_set_natural_size(lab, 0, 0);
    fdk_label_set_mode(lab, FDK_LABEL_NOWRAP);
    fdk_widget_measure(lab, &styled);
    fdk_size nowrap_plain = {0, 0};
    fdk_widget_measure(plain_lab, &nowrap_plain);
    assert(styled.width == nowrap_plain.width);

    /* Markup with colors + underline paints (decorations exercise
     * the bar path inside the widget paint walk). */
    assert(fdk_ok(fdk_label_set_markup(
        lab, "<color=#ff0000>red</color> <u>under</u> <s>gone</s>")));
    fdk_widget_tree_paint(root, s);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);

    printf("[ok] label markup: NOWRAP/WRAP/ELLIPSIZE sizes, plain "
           "get_text, set_text clears, paint smoke\n");
}

static void test_button_and_tooltip_markup(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, &fdk_box_class_def,
                                    (fdk_rect){0, 0, 400, 200},
                                    &root)));

    /* Button: plain label via markup, styled measure. */
    fdk_widget *btn = NULL;
    assert(fdk_ok(fdk_button_create(root, g_font, NULL, &btn)));
    assert(fdk_ok(fdk_button_set_markup(btn, "Run <b>now</b>")));
    assert(strcmp(fdk_button_get_text(btn), "Run now") == 0);
    fdk_size styled = {0, 0};
    fdk_widget_measure(btn, &styled);

    fdk_widget *btn2 = NULL;
    assert(fdk_ok(fdk_button_create(root, g_font, "Run now", &btn2)));
    fdk_size plain = {0, 0};
    fdk_widget_measure(btn2, &plain);
    assert(styled.width > plain.width);

    /* Paint smoke (roles + markup together). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(400, 200, &s)));
    fdk_widget_tree_paint(root, s);

    /* set_text clears markup: size returns to plain. */
    assert(fdk_ok(fdk_button_set_text(btn, "Run now")));
    fdk_widget_measure(btn, &styled);
    assert(styled.width == plain.width);
    fdk_widget_tree_paint(root, s);

    /* Tooltip markup: get_tooltip reports the PLAIN text. */
    assert(fdk_ok(fdk_widget_set_tooltip_markup(
        btn, "<b>Save</b> the <color=#ff0000>file</color>")));
    assert(strcmp(fdk_widget_get_tooltip(btn), "Save the file") == 0);

    /* set_tooltip clears the markup (no span leak into a plain tip:
     * the hover machinery never sees spans after this). */
    assert(fdk_ok(fdk_widget_set_tooltip(btn, "plain tip")));
    assert(strcmp(fdk_widget_get_tooltip(btn), "plain tip") == 0);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);

    printf("[ok] button + tooltip markup: styled sizes, plain "
           "getters, set_text/set_tooltip clearing\n");
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
        g_font = fdk_font_load(candidates[i], 16);
        if (g_font != NULL) {
            break;
        }
    }
    if (g_font == NULL) {
        printf("[skip] no system TrueType font found — the markup "
               "suite needs real glyph metrics; see "
               "docs/testing.md\n");
        return 0;
    }

    test_scanner();
    test_span_measure();
    test_span_draw();
    test_span_wrap_ellipsis();
    test_label_markup();
    test_button_and_tooltip_markup();

    fdk_font_destroy(g_font);
    printf("all markup tests passed\n");
    return 0;
}
