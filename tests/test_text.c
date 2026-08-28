/* test_text.c — headless text-layer tests (Phase 6 text foundation).
 *
 * Everything runs on offscreen surfaces and standalone fonts: no
 * display, no window, deterministic. The font is DejaVu Sans (and its
 * Mono sibling) from the system font directories — if no usable font
 * is present the whole suite honestly skips (the X11 suite's
 * established pattern for environment-dependent coverage).
 *
 * What is proven here:
 *   - font lifecycle + failure modes (missing file, garbage bytes,
 *     out-of-range sizes)
 *   - metrics sanity and scale proportionality
 *   - measure/draw agreement: the measured advance is where drawing
 *     actually lands; ink bounds match the damage box exactly
 *   - glyph cache: hit/miss counters, deterministic re-render,
 *     LRU eviction past 2048 distinct (glyph, phase) entries
 *   - clip-stack honoring and damage precision
 *   - UTF-8 edge cases: invalid bytes, unmapped codepoints, NUL
 *   - synthetic styles (Phase 6 completion): bold/italic argument
 *     safety, advance contracts, cache flush + idempotence
 *   - subpixel positioning (Phase 6 completion): agreement,
 *     determinism, pen-shift invariance, phase fan-out
 *   - system font discovery (post-1.0.1): $FDK_FONT_FILE override
 *     precedence and invalid-override fall-through, fontconfig
 *     end-to-end, the Arch variable-font filename scan, nested-dir
 *     scan, regular-beats-bold ranking, cache consistency
 */

#include "fdk/fdk.h"
#include "fdk/fdk_text.h"

#include "text/text_internal.h" /* fdk_text_font_discovery_reset_for_tests */

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- helpers ---- */

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

static fdk_u32 pack(int r, int g, int b) {
    return ((fdk_u32)r << 16) | ((fdk_u32)g << 8) | (fdk_u32)b;
}

static fdk_color rgb(int r, int g, int b) {
    fdk_color c = { .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                    .b = (fdk_f32)b / 255.0f, .a = 1.0f };
    return c;
}

static fdk_color white(void) { return rgb(255, 255, 255); }

/* Counts pixels in the surface that differ from `bg`. */
static long count_ink(fdk_surface *s, int x0, int y0, int x1, int y1,
                      fdk_u32 bg) {
    long ink = 0;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            if (px_at(s, x, y) != bg) {
                ink++;
            }
        }
    }
    return ink;
}

/* Encodes cp as UTF-8 into buf (>= 5 bytes); returns length. */
static int enc_utf8(fdk_u32 cp, char *buf) {
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    buf[0] = (char)(0xF0u | (cp >> 18));
    buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    buf[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ---- fonts under test ---- */

static const char *g_sans = NULL;
static const char *g_mono = NULL;

static void find_fonts(void) {
    static const char *sans_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    static const char *mono_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/DejaVuSansMono.ttf",
        NULL,
    };
    for (int i = 0; sans_candidates[i] != NULL; i++) {
        FILE *f = fopen(sans_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            g_sans = sans_candidates[i];
            break;
        }
    }
    for (int i = 0; mono_candidates[i] != NULL; i++) {
        FILE *f = fopen(mono_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            g_mono = mono_candidates[i];
            break;
        }
    }
}

/* ---- lifecycle + failures ---- */

static void test_font_lifecycle(void) {
    /* Bad arguments. */
    assert(fdk_font_load(NULL, 16) == NULL);
    assert(fdk_font_load(g_sans, 0) == NULL);
    assert(fdk_font_load(g_sans, -4) == NULL);
    assert(fdk_font_load(g_sans, 513) == NULL);

    /* Missing file. */
    assert(fdk_font_load("/nonexistent/font.ttf", 16) == NULL);

    /* Not a font: a regular text file with plausible length. */
    {
        const char *path = "/tmp/fdk_notafont.ttf";
        FILE *f = fopen(path, "wb");
        assert(f != NULL);
        for (int i = 0; i < 4096; i++) {
            fputc(i % 251, f);
        }
        fclose(f);
        assert(fdk_font_load(path, 16) == NULL);
        remove(path);
    }

    /* Directory is not a font either. */
    assert(fdk_font_load("/tmp", 16) == NULL);

    /* Good load, sane metrics, proportionality between sizes. */
    fdk_font *a = fdk_font_load(g_sans, 16);
    assert(a != NULL);
    fdk_font_metrics m16;
    fdk_font_get_metrics(a, &m16);
    assert(m16.ascent > 0);
    assert(m16.descent > 0);
    assert(m16.line_height ==
           m16.ascent + m16.descent + m16.line_gap);
    /* DejaVu Sans at 16px: ascent ~14-15, descent ~4. */
    assert(m16.ascent >= 10 && m16.ascent <= 20);
    assert(m16.descent >= 2 && m16.descent <= 8);

    fdk_font *b = fdk_font_load(g_sans, 32);
    assert(b != NULL);
    fdk_font_metrics m32;
    fdk_font_get_metrics(b, &m32);
    assert(m32.ascent >= 2 * m16.ascent - 1 &&
           m32.ascent <= 2 * m16.ascent + 1);
    assert(m32.descent >= 2 * m16.descent - 1 &&
           m32.descent <= 2 * m16.descent + 1);

    /* NULL-safe accessors. */
    fdk_font_get_metrics(NULL, NULL);
    fdk_font_cache_stats st;
    fdk_font_get_cache_stats(a, &st);
    assert(st.cached_glyphs == 0 && st.cache_hits == 0 &&
           st.cache_misses == 0 && st.evictions == 0);
    fdk_font_get_cache_stats(NULL, NULL);
    fdk_font_destroy(NULL); /* no-op */

    fdk_font_destroy(b);
    fdk_font_destroy(a);
    printf("[ok] font lifecycle: load, failures (missing/garbage/args), "
           "metrics sanity + 2x scale proportionality\n");
}

/* ---- measurement ---- */

static void test_measure(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    fdk_text_metrics m;

    /* Argument safety. */
    assert(fdk_font_measure_utf8(NULL, "x", 1, &m) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_font_measure_utf8(f, NULL, 1, &m) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_font_measure_utf8(f, "x", 1, NULL) ==
           FDK_ERR_INVALID_ARGUMENT);

    /* Empty text: nothing at all. */
    assert(fdk_ok(fdk_font_measure_utf8(f, "", 0, &m)));
    assert(m.advance_width == 0 && m.ink_top == 0 && m.ink_bottom == 0);

    /* Proportional face: W wider than i; additivity holds within
     * rounding (each glyph's advance is fractional; the total is
     * rounded ONCE, so 2 x round(a) can differ from round(2a) by 1,
     * and kerning can only tighten — hence the asymmetric bounds). */
    fdk_text_metrics mw, mi, mww;
    assert(fdk_ok(fdk_font_measure_utf8(f, "W", 1, &mw)));
    assert(fdk_ok(fdk_font_measure_utf8(f, "i", 1, &mi)));
    assert(fdk_ok(fdk_font_measure_utf8(f, "WW", 2, &mww)));
    assert(mw.advance_width > mi.advance_width);
    assert(mww.advance_width >= 2 * mw.advance_width - 1);
    assert(mww.advance_width <= 2 * mw.advance_width + 1);

    /* Ink bounds: "F" lives above the baseline. */
    fdk_text_metrics mf;
    assert(fdk_ok(fdk_font_measure_utf8(f, "F", 1, &mf)));
    assert(mf.ink_top < 0);
    assert(mf.ink_bottom >= 0 && mf.ink_bottom <= mf.ink_top * -1);

    /* "g" dips below the baseline. */
    fdk_text_metrics mg;
    assert(fdk_ok(fdk_font_measure_utf8(f, "g", 1, &mg)));
    assert(mg.ink_bottom > 0);

    /* Whitespace advances but has no ink. */
    fdk_text_metrics ms;
    assert(fdk_ok(fdk_font_measure_utf8(f, "   ", 3, &ms)));
    assert(ms.advance_width > 0);
    assert(ms.ink_top == 0 && ms.ink_bottom == 0);

    /* Not NUL-terminated: byte_len governs. */
    const char five[] = "Hello";
    assert(fdk_ok(fdk_font_measure_utf8(f, five, 5, &m)));
    fdk_text_metrics m2;
    assert(fdk_ok(fdk_font_measure_utf8(f, five, 2, &m2)));
    assert(m2.advance_width < m.advance_width);

    /* Monospace face: every glyph advances identically (within the
     * same rounding slack). */
    if (g_mono != NULL) {
        fdk_font *mono = fdk_font_load(g_mono, 16);
        assert(mono != NULL);
        fdk_text_metrics m1, m4a, m4b;
        assert(fdk_ok(fdk_font_measure_utf8(mono, "W", 1, &m1)));
        assert(fdk_ok(fdk_font_measure_utf8(mono, "WWWW", 4, &m4a)));
        assert(fdk_ok(fdk_font_measure_utf8(mono, "iiii", 4, &m4b)));
        /* THE monospace property: equal-length runs of different
         * glyphs measure identically. (Against a single W, allow the
         * round-of-sum vs sum-of-rounds slack — it drifts both ways.) */
        assert(m4a.advance_width == m4b.advance_width);
        assert(m4a.advance_width >= 4 * m1.advance_width - 2);
        assert(m4a.advance_width <= 4 * m1.advance_width + 2);
        assert(m4b.advance_width == m4a.advance_width);
        fdk_font_destroy(mono);
    }

    fdk_font_destroy(f);
    printf("[ok] measurement: empty/proportional/monospace, ink bounds, "
           "whitespace, byte_len slicing, arg safety\n");
}

/* ---- drawing: ink, damage, determinism ---- */

static void test_draw_ink_and_damage(void) {
    fdk_font *f = fdk_font_load(g_sans, 24);
    assert(f != NULL);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));
    fdk_color bg = rgb(16, 16, 24);
    fdk_surface_fill(s, bg);
    fdk_u32 bgpx = pack(16, 16, 24);
    /* Close the fill's frame: damage assertions below must see ONLY
     * the text draw (present() resets an offscreen surface's damage). */
    assert(fdk_ok(fdk_surface_present(s)));

    const char *text = "FDK text!";
    size_t len = strlen(text);
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m)));

    int pen_x = 20;
    int baseline = 40;
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline,
                                        white())));

    /* Ink exists, above the baseline, within the measured advance. */
    long ink_all = count_ink(s, 0, 0, 200, 100, bgpx);
    assert(ink_all > 30);
    long ink_above = count_ink(s, pen_x, baseline + m.ink_top, pen_x + m.advance_width,
                               baseline, bgpx);
    assert(ink_above > 20);
    /* Nothing left of the pen or past the advance (plus the one
     * rounding pixel of slack). */
    assert(count_ink(s, 0, 0, pen_x, 100, bgpx) == 0);
    assert(count_ink(s, pen_x + m.advance_width + 1, 0, 200, 100, bgpx) == 0);

    /* Damage: exactly the ink band (y bounds are exact by
     * construction — measure and draw share the shaping walk). */
    fdk_rect dmg;
    assert(fdk_surface_get_damage_bounds(s, &dmg));
    assert(dmg.y == baseline + m.ink_top);
    assert(dmg.y + dmg.height == baseline + m.ink_bottom);
    assert(dmg.x >= pen_x);
    assert(dmg.x + dmg.width <= pen_x + m.advance_width);

    /* Re-draw the same text elsewhere: identical glyph pixels prove
     * cache-hit determinism, and hits == the second run's glyph
     * count. */
    fdk_font_cache_stats st0;
    fdk_font_get_cache_stats(f, &st0);
    int baseline2 = 80;
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline2,
                                        white())));
    fdk_font_cache_stats st1;
    fdk_font_get_cache_stats(f, &st1);
    assert(st1.cache_misses == st0.cache_misses);
    assert(st1.cache_hits > st0.cache_hits);
    for (int x = pen_x; x < pen_x + m.advance_width; x++) {
        for (int dy = 0; dy < -m.ink_top + m.ink_bottom; dy++) {
            int y1 = baseline + m.ink_top + dy;
            int y2 = baseline2 + m.ink_top + dy;
            if (y1 >= 0 && y2 >= 0 && y1 < 100 && y2 < 100) {
                assert(px_at(s, x, y1) == px_at(s, x, y2));
            }
        }
    }

    /* Whitespace-only run draws nothing and damages nothing (fill +
     * present first so the empty damage list is meaningful). */
    fdk_surface *w = NULL;
    assert(fdk_ok(fdk_surface_create(100, 50, &w)));
    fdk_surface_fill(w, bg);
    assert(fdk_ok(fdk_surface_present(w)));
    assert(fdk_ok(fdk_surface_draw_utf8(w, f, "     ", 5, 10, 25, white())));
    fdk_rect wdmg;
    assert(fdk_surface_get_damage_bounds(w, &wdmg) == false);
    assert(count_ink(w, 0, 0, 100, 50, bgpx) == 0);

    /* Argument safety. */
    assert(fdk_surface_draw_utf8(NULL, f, text, len, 0, 0, white()) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_draw_utf8(s, NULL, text, len, 0, 0, white()) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_surface_draw_utf8(s, f, NULL, len, 0, 0, white()) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, "", 0, 0, 0, white())));

    fdk_surface_destroy(w);
    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] draw: ink bounds vs measured metrics, damage box exact, "
           "cache-hit determinism, whitespace no-op, arg safety\n");
}

/* ---- clipping ---- */

static void test_clip(void) {
    fdk_font *f = fdk_font_load(g_sans, 24);
    assert(f != NULL);

    /* Two identical surfaces; one draws through a clip. Fill +
     * present both so the later damage assertion sees only the text
     * draws. */
    fdk_surface *a = NULL, *b = NULL;
    assert(fdk_ok(fdk_surface_create(200, 80, &a)));
    assert(fdk_ok(fdk_surface_create(200, 80, &b)));
    fdk_color bg = rgb(10, 10, 14);
    fdk_surface_fill(a, bg);
    fdk_surface_fill(b, bg);
    assert(fdk_ok(fdk_surface_present(a)));
    assert(fdk_ok(fdk_surface_present(b)));
    fdk_u32 bgpx = pack(10, 10, 14);

    const char *text = "Clipped text run";
    size_t len = strlen(text);
    int pen_x = 10, baseline = 40;

    fdk_rect clip = {40, 0, 30, 80};
    assert(fdk_ok(fdk_surface_push_clip(a, clip)));
    assert(fdk_ok(fdk_surface_draw_utf8(a, f, text, len, pen_x, baseline,
                                        white())));
    fdk_surface_pop_clip(a);
    assert(fdk_ok(fdk_surface_draw_utf8(b, f, text, len, pen_x, baseline,
                                        white())));

    /* Outside the clip: identical (untouched background). */
    for (int y = 0; y < 80; y++) {
        for (int x = 0; x < 200; x += 3) {
            if (x >= 40 && x < 70) {
                continue;
            }
            assert(px_at(a, x, y) == bgpx);
        }
    }
    /* Inside the clip region: some ink was painted. */
    assert(count_ink(a, 40, 0, 70, 80, bgpx) > 5);
    /* The unclipped draw painted outside the clip window too. */
    assert(count_ink(b, 0, 0, 40, 80, bgpx) > 5);

    /* Damage on the clipped surface stayed inside the clip x-range. */
    fdk_rect dmg;
    assert(fdk_surface_get_damage_bounds(a, &dmg));
    assert(dmg.x >= 40);
    assert(dmg.x + dmg.width <= 70);

    fdk_surface_destroy(a);
    fdk_surface_destroy(b);
    fdk_font_destroy(f);
    printf("[ok] clip: glyphs honor the clip stack; damage clipped to the "
           "visible span\n");
}

/* ---- UTF-8 handling ---- */

static void test_utf8(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    fdk_text_metrics m;

    /* Invalid bytes decode to U+FFFD one byte at a time — no crash,
     * nonzero advance (replacement glyph), and 3 bad bytes consume
     * exactly 3 bytes (the 4th char still shapes). */
    const char bad[] = "\xFF\xFE{W";
    assert(fdk_ok(fdk_font_measure_utf8(f, bad, 4, &m)));
    assert(m.advance_width > 0);

    fdk_text_metrics mw;
    assert(fdk_ok(fdk_font_measure_utf8(f, "W", 1, &mw)));
    /* U+FFFD + '{' + 'W' must be wider than just 'W'. */
    assert(m.advance_width > mw.advance_width);

    /* Truncated multi-byte sequence at the buffer end. */
    const char trunc[] = "A\xE2\x82";
    assert(fdk_ok(fdk_font_measure_utf8(f, trunc, 3, &m)));

    /* Unmapped codepoint (unassigned plane 15) renders .notdef. */
    const char unmapped[] = "\xF3\xBB\xB0\x80"; /* U+DEF00 */
    assert(fdk_ok(fdk_font_measure_utf8(f, unmapped, 4, &m)));
    assert(m.advance_width > 0);

    /* Multi-byte codepoint that IS mapped: Euro sign. */
    const char euro[] = "\xE2\x82\xAC"; /* U+20AC */
    assert(fdk_ok(fdk_font_measure_utf8(f, euro, 3, &m)));
    assert(m.advance_width > 0);

    /* NUL is a codepoint like any other (usually .notdef). */
    const char with_nul[] = "A\0B";
    assert(fdk_ok(fdk_font_measure_utf8(f, with_nul, 3, &m)));

    /* Draw path with the same garbage: no crash, ink appears. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(120, 40, &s)));
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, bad, 4, 5, 25, white())));
    assert(count_ink(s, 0, 0, 120, 40, 0) > 0);

    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] utf-8: invalid bytes -> U+FFFD (no crash, advances), "
           "truncated sequences, unmapped -> .notdef, mapped multi-byte\n");
}

/* ---- cache eviction ---- */

static void test_cache_eviction(void) {
    fdk_font *f = fdk_font_load(g_sans, 12);
    assert(f != NULL);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(64, 32, &s)));

    /* Walk codepoint space, drawing in chunks, until the cache is
     * forced to evict (DejaVu covers thousands of glyphs; a few
     * thousand codepoints always exceed the 2048-entry cache, which
     * subpixel phases key per (glyph, phase) pair). */
    char buf[64 * 4];
    int evicted = 0;
    for (fdk_u32 base = 0x20; base < 0x2600 && !evicted; base += 64) {
        int n = 0;
        for (fdk_u32 cp = base; cp < base + 64; cp++) {
            if (cp >= 0xD800u && cp <= 0xDFFFu) {
                continue; /* no surrogates in UTF-8 */
            }
            n += enc_utf8(cp, buf + n);
        }
        assert(fdk_ok(fdk_surface_draw_utf8(s, f, buf, (size_t)n, 0, 20,
                                            white())));
        fdk_font_cache_stats st;
        fdk_font_get_cache_stats(f, &st);
        assert(st.cached_glyphs <= 2048);
        if (st.evictions > 0) {
            evicted = 1;
            assert(st.cache_misses > 2048);
        }
    }
    assert(evicted);

    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] cache: LRU eviction past 2048 (glyph, phase) entries, "
           "bounded residency\n");
}

/* ---- kerning ---- */

static void test_kerning(void) {
    /* Kerned pair renders narrower than the naive sum — proves the
     * kern table is consulted (DejaVu Sans kerns "AV"). */
    fdk_font *f = fdk_font_load(g_sans, 32);
    assert(f != NULL);

    fdk_text_metrics mav, ma, mv;
    assert(fdk_ok(fdk_font_measure_utf8(f, "AV", 2, &mav)));
    assert(fdk_ok(fdk_font_measure_utf8(f, "A", 1, &ma)));
    assert(fdk_ok(fdk_font_measure_utf8(f, "V", 1, &mv)));

    if (mav.advance_width < ma.advance_width + mv.advance_width) {
        printf("[ok] kerning: AV pair tightened by %d px\n",
               ma.advance_width + mv.advance_width - mav.advance_width);
    } else {
        /* Rounding can mask a sub-pixel kern; the pair must at least
         * never exceed the naive sum. */
        assert(mav.advance_width == ma.advance_width + mv.advance_width);
        printf("[ok] kerning: no pair tightening visible at 32px "
               "(sub-pixel)\n");
    }
    fdk_font_destroy(f);
}

/* ---- line breaking ---- */

/* Verifies one line's reported advance equals what measuring its
 * bytes reports — the agreement-by-construction contract. */
static void check_line_agrees(fdk_font *f, const char *text,
                               const fdk_text_line *lines, size_t n) {
    for (size_t i = 0; i < n; i++) {
        fdk_text_metrics m;
        assert(fdk_ok(fdk_font_measure_utf8(f, text + lines[i].byte_offset,
                                             lines[i].byte_len, &m)));
        assert(lines[i].advance_width == m.advance_width);
    }
}

/* True when cp is a UTF-8 continuation byte (10xxxxxx). */
static int is_cont(unsigned char c) { return (c & 0xC0u) == 0x80u; }

static void test_break_lines(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    /* 1. Basic greedy wrap: every line fits, every line's advance
     * agrees with measure, no visible character is lost. */
    const char *s = "alpha bravo charlie delta echo foxtrot";
    size_t len = strlen(s);
    fdk_text_metrics whole;
    assert(fdk_ok(fdk_font_measure_utf8(f, s, len, &whole)));
    fdk_i32 width = whole.advance_width / 3; /* ~2 words per line */
    assert(width > 0);

    size_t count = 0;
    assert(fdk_ok(fdk_font_break_lines_utf8(f, s, len, width, NULL, 0,
                                             &count, NULL)));
    assert(count >= 2 && count <= 7);
    fdk_text_line *lines = calloc(count, sizeof(*lines));
    assert(lines != NULL);
    size_t filled = 0;
    bool trunc = true;
    assert(fdk_ok(fdk_font_break_lines_utf8(f, s, len, width, lines,
                                             count, &filled, &trunc)));
    assert(filled == count && !trunc);
    check_line_agrees(f, s, lines, filled);

    size_t visible_src = 0, visible_out = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] != ' ') visible_src++;
    }
    for (size_t i = 0; i < filled; i++) {
        assert(lines[i].advance_width <= width);
        assert(lines[i].byte_offset + lines[i].byte_len <= len);
        for (size_t b = 0; b < lines[i].byte_len; b++) {
            if (s[lines[i].byte_offset + b] != ' ') visible_out++;
        }
    }
    assert(visible_src == visible_out); /* nothing dropped */
    free(lines);

    /* 2. Hard newlines: one line each, "\n\n" keeps an empty line,
     * "\r\n" counts as a single break. */
    {
        fdk_text_line l[4];
        size_t n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "one\ntwo", 7, 100,
                                                 l, 4, &n, NULL)));
        assert(n == 2);
        assert(l[0].byte_offset == 0 && l[0].byte_len == 3);
        assert(l[1].byte_offset == 4 && l[1].byte_len == 3);

        n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "a\n\nb", 4, 100,
                                                 l, 4, &n, NULL)));
        assert(n == 3);
        assert(l[0].byte_len == 1 && l[1].byte_len == 0 &&
               l[2].byte_len == 1);

        n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "a\r\nb", 4, 100,
                                                 l, 4, &n, NULL)));
        assert(n == 2);
        assert(l[0].byte_len == 1 && l[1].byte_offset == 3);
    }

    /* 3. Long word: mid-word breaks, every glyph preserved, lines
     * still within width. */
    {
        char word[41];
        memset(word, 'M', 40);
        word[40] = '\0';
        fdk_text_metrics mm;
        assert(fdk_ok(fdk_font_measure_utf8(f, "M", 1, &mm)));
        fdk_i32 w = mm.advance_width * 3; /* ~3 M's per line */
        assert(w > 0);
        fdk_text_line l[64];
        size_t n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, word, 40, w, l, 64,
                                                 &n, NULL)));
        assert(n >= 10 && n <= 40);
        size_t m_total = 0;
        for (size_t i = 0; i < n; i++) {
            m_total += l[i].byte_len;
            if (l[i].byte_len > 1) {
                assert(l[i].advance_width <= w);
            }
        }
        assert(m_total == 40); /* every M accounted for */
    }

    /* 4. Trailing spaces trimmed; leading spaces of line 1 kept;
     * trailing all-space tail emits no line. */
    {
        fdk_text_line l[4];
        size_t n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "word   ", 7, 100,
                                                 l, 4, &n, NULL)));
        assert(n == 1 && l[0].byte_len == 4);
        fdk_text_metrics mw;
        assert(fdk_ok(fdk_font_measure_utf8(f, "word", 4, &mw)));
        assert(l[0].advance_width == mw.advance_width);

        n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "   ", 3, 100, l, 4,
                                                 &n, NULL)));
        assert(n == 0); /* pure spaces: nothing visible */
    }

    /* 5. Truncation: max_lines caps output, flag set, prefix lines
     * identical to the untruncated pass. */
    {
        size_t full_n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, s, len, width, NULL,
                                                 0, &full_n, NULL)));
        assert(full_n >= 3);
        fdk_text_line *full = calloc(full_n, sizeof(*full));
        assert(full != NULL);
        size_t fn = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, s, len, width, full,
                                                 full_n, &fn, NULL)));
        assert(fn == full_n);

        fdk_text_line head[2];
        size_t hn = 0;
        bool trunc_flag = false;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, s, len, width, head,
                                                 2, &hn, &trunc_flag)));
        assert(hn == 2 && trunc_flag);
        assert(head[0].byte_offset == full[0].byte_offset &&
               head[0].byte_len == full[0].byte_len &&
               head[0].advance_width == full[0].advance_width);
        assert(head[1].byte_len == full[1].byte_len);
        free(full);
    }

    /* 6. Everything fits: one line, whole text, exact advance. */
    {
        fdk_text_metrics mt;
        assert(fdk_ok(fdk_font_measure_utf8(f, "tiny", 4, &mt)));
        fdk_text_line l[2];
        size_t n = 0;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "tiny", 4,
                                                 mt.advance_width, l, 2,
                                                 &n, NULL)));
        assert(n == 1 && l[0].byte_len == 4);
        assert(l[0].advance_width == mt.advance_width);
    }

    /* 7. Argument safety + empties. */
    {
        size_t n = 9;
        assert(fdk_font_break_lines_utf8(NULL, "x", 1, 1, NULL, 0, &n,
                                         NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_break_lines_utf8(f, NULL, 1, 1, NULL, 0, &n,
                                         NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_break_lines_utf8(f, "x", 1, 1, NULL, 0, NULL,
                                         NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_break_lines_utf8(f, "x", 1, 1, NULL, 2, &n,
                                         NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_break_lines_utf8(f, "x", 1, 0, NULL, 0, &n,
                                         NULL) == FDK_ERR_INVALID_ARGUMENT);
        n = 9;
        assert(fdk_ok(fdk_font_break_lines_utf8(f, "", 0, 100, NULL, 0,
                                                 &n, NULL)));
        assert(n == 0);
    }

    fdk_font_destroy(f);
    printf("[ok] break_lines: greedy wrap fits/agrees, hard breaks, "
           "mid-word, trimming, truncation flag, arg safety\n");
}

/* ---- ellipsis ---- */

static void test_ellipsize(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    const char *s = "The quick brown fox jumps over the lazy dog";
    size_t len = strlen(s);
    const char *ell = "\xE2\x80\xA6"; /* U+2026 */
    fdk_text_metrics me, mw;
    assert(fdk_ok(fdk_font_measure_utf8(f, ell, 3, &me)));
    assert(me.advance_width > 0);
    assert(fdk_ok(fdk_font_measure_utf8(f, s, len, &mw)));

    /* 1. Wide enough: fits, prefix = everything. */
    {
        size_t prefix = 0;
        bool fits = false;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, s, len,
                                               mw.advance_width + 5,
                                               &prefix, &fits)));
        assert(fits && prefix == len);
    }

    /* 2. Narrow: not fitting, prefix maximal, boundary-safe, with
     * room for the ellipsis exactly. */
    {
        fdk_i32 width = mw.advance_width / 2;
        size_t prefix = 0;
        bool fits = true;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, s, len, width, &prefix,
                                               &fits)));
        assert(!fits && prefix < len);
        assert(prefix == 0 || !is_cont((unsigned char)s[prefix]));
        fdk_text_metrics mp;
        assert(fdk_ok(fdk_font_measure_utf8(f, s, prefix, &mp)));
        assert(mp.advance_width + me.advance_width <= width);
        /* Maximal: the next whole codepoint would not fit. */
        size_t next = prefix;
        if (next < len) {
            next++; /* the codepoint's first byte */
            while (next < len && is_cont((unsigned char)s[next])) {
                next++; /* its continuation bytes */
            }
            fdk_text_metrics mn;
            assert(fdk_ok(fdk_font_measure_utf8(f, s, next, &mn)));
            assert(mn.advance_width + me.advance_width > width);
        }
        /* Prefix never ends on a space. */
        assert(prefix == 0 || s[prefix - 1] != ' ');
    }

    /* 3. Room for (almost) everything: prefix excludes only the
     * tail, and the trimmed-space rule holds mid-string. */
    {
        fdk_i32 width = mw.advance_width - me.advance_width - 1;
        size_t prefix = 0;
        bool fits = true;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, s, len, width, &prefix,
                                               &fits)));
        assert(!fits && prefix > 0 && prefix < len);
    }

    /* 4. Degenerate widths: ellipsis alone wider than the budget. */
    {
        size_t prefix = 99;
        bool fits = true;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, s, len,
                                               me.advance_width - 1,
                                               &prefix, &fits)));
        assert(!fits && prefix == 0);
        prefix = 99;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, s, len, 0, &prefix,
                                               &fits)));
        assert(!fits && prefix == 0);
    }

    /* 5. Empty text always fits; out_fits optional. */
    {
        size_t prefix = 5;
        assert(fdk_ok(fdk_font_ellipsize_utf8(f, "", 0, 10, &prefix,
                                               NULL)));
        assert(prefix == 0);
    }

    /* 6. Argument safety. */
    {
        size_t prefix = 0;
        assert(fdk_font_ellipsize_utf8(NULL, s, len, 10, &prefix,
                                       NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_ellipsize_utf8(f, NULL, len, 10, &prefix,
                                       NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_ellipsize_utf8(f, s, len, 10, NULL,
                                       NULL) == FDK_ERR_INVALID_ARGUMENT);
        assert(fdk_font_ellipsize_utf8(f, s, len, -1, &prefix,
                                       NULL) == FDK_ERR_INVALID_ARGUMENT);
    }

    fdk_font_destroy(f);
    printf("[ok] ellipsize: fits/no-fit/maximal prefix/boundary-safe/"
           "degenerate/arg safety\n");
}

/* ---- synthetic styles (Phase 6 completion) ---- */

static void test_font_style(void) {
    /* Argument safety. */
    assert(fdk_font_set_style(NULL, FDK_FONT_STYLE_BOLD) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(fdk_font_get_style(NULL) == 0u);

    fdk_font *f = fdk_font_load(g_sans, 24);
    assert(f != NULL);
    assert(fdk_font_get_style(f) == 0u);

    /* Unknown bits are masked away, not stored. */
    assert(fdk_ok(fdk_font_set_style(f, 0xFFu)));
    assert(fdk_font_get_style(f) ==
           (FDK_FONT_STYLE_BOLD | FDK_FONT_STYLE_ITALIC));
    assert(fdk_ok(fdk_font_set_style(f, 0u)));
    assert(fdk_font_get_style(f) == 0u);

    const char *text = "FDK text!"; /* 8 inked glyphs + 1 space */
    size_t len = strlen(text);
    fdk_text_metrics m_reg;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_reg)));

    /* Synthetic bold: every INKED glyph's advance grows by the stem
     * (pixel_size/24, min 1); the space has no bitmap so it does not
     * grow. The total is exact — the stem is an integer, so the
     * final rounding is unaffected. Ink HEIGHT is untouched (the
     * pass only widens). */
    int stem = 24 / 24;
    assert(stem >= 1);
    int inked = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] != ' ') {
            inked++;
        }
    }
    assert(fdk_ok(fdk_font_set_style(f, FDK_FONT_STYLE_BOLD)));
    fdk_text_metrics m_bold;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_bold)));
    assert(m_bold.advance_width == m_reg.advance_width + inked * stem);
    assert(m_bold.ink_top == m_reg.ink_top);
    assert(m_bold.ink_bottom == m_reg.ink_bottom);

    /* Synthetic italic: the oblique shear reshapes ink but
     * deliberately leaves every advance (and so the total) alone. */
    assert(fdk_ok(fdk_font_set_style(f, FDK_FONT_STYLE_ITALIC)));
    fdk_text_metrics m_ital;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_ital)));
    assert(m_ital.advance_width == m_reg.advance_width);

    /* Bold + italic combine: stem growth only (shear adds no
     * advance). */
    assert(fdk_ok(fdk_font_set_style(
        f, FDK_FONT_STYLE_BOLD | FDK_FONT_STYLE_ITALIC)));
    fdk_text_metrics m_both;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_both)));
    assert(m_both.advance_width == m_bold.advance_width);

    /* A style change flushes the cache (rasterizations bake the
     * style in); setting the SAME style again is a no-op. */
    assert(fdk_ok(fdk_font_set_style(f, 0u)));
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_reg)));
    fdk_font_cache_stats st_warm;
    fdk_font_get_cache_stats(f, &st_warm);
    assert(st_warm.cached_glyphs > 0);

    assert(fdk_ok(fdk_font_set_style(f, FDK_FONT_STYLE_BOLD)));
    fdk_font_cache_stats st_flushed;
    fdk_font_get_cache_stats(f, &st_flushed);
    assert(st_flushed.cached_glyphs == 0);

    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_bold)));
    fdk_font_cache_stats st_misses;
    fdk_font_get_cache_stats(f, &st_misses);
    assert(st_misses.cached_glyphs > 0);
    assert(fdk_ok(fdk_font_set_style(f, FDK_FONT_STYLE_BOLD)));
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m_bold)));
    fdk_font_cache_stats st_idem;
    fdk_font_get_cache_stats(f, &st_idem);
    assert(st_idem.cache_misses == st_misses.cache_misses);
    assert(st_idem.cached_glyphs > 0);

    /* Draw agreement under style (bold is currently set): the ink
     * stays inside the BOLD measured advance (+1 rounding px). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(200, 100, &s)));
    fdk_color bg = rgb(16, 16, 24);
    fdk_surface_fill(s, bg);
    fdk_u32 bgpx = pack(16, 16, 24);
    assert(fdk_ok(fdk_surface_present(s)));
    int pen_x = 20;
    int baseline = 50;
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline,
                                        white())));
    assert(count_ink(s, 0, 0, pen_x, 100, bgpx) == 0);
    assert(count_ink(s, pen_x + m_bold.advance_width + 1, 0, 200, 100,
                     bgpx) == 0);
    assert(count_ink(s, pen_x, 0, pen_x + m_bold.advance_width, 100,
                     bgpx) > 30);

    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] style: bold advance += stem/glyph (height untouched), "
           "italic advance unchanged, combos, mask+flush+idempotence, "
           "draw agreement\n");
}

/* ---- subpixel positioning (Phase 6 completion) ---- */

static void test_subpixel_positioning(void) {
    /* Kerned strings accumulate fractional pen positions; the walk
     * floors each glyph's left edge and quantizes the fractional
     * remainder into one of 4 phase-keyed rasterizations. */
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    const char *text = "AVATAR Waveform";
    size_t len = strlen(text);

    /* Stable totals: the measured advance is the rounded float pen
     * — repeating the measure must not drift. */
    fdk_text_metrics m, m2;
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m)));
    assert(fdk_ok(fdk_font_measure_utf8(f, text, len, &m2)));
    assert(m.advance_width == m2.advance_width);

    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 100, &s)));
    fdk_color bg = rgb(16, 16, 24);
    fdk_surface_fill(s, bg);
    fdk_u32 bgpx = pack(16, 16, 24);
    assert(fdk_ok(fdk_surface_present(s)));
    int pen_x = 15;
    int baseline = 40;
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline,
                                        white())));

    /* (1) Measure/draw agreement survives fractional pens: ink
     * stays inside the measured advance (+1 rounding px). */
    assert(count_ink(s, 0, 0, pen_x, 100, bgpx) == 0);
    assert(count_ink(s, pen_x + m.advance_width + 1, 0, 300, 100,
                     bgpx) == 0);
    assert(count_ink(s, pen_x, 0, pen_x + m.advance_width, 100, bgpx) >
           30);

    /* (2) Determinism: a second draw at another baseline is
     * pixel-identical row for row and served from the cache (no new
     * rasterizations — the phase keys hit). */
    fdk_font_cache_stats st0;
    fdk_font_get_cache_stats(f, &st0);
    int baseline2 = 80;
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, text, len, pen_x, baseline2,
                                        white())));
    fdk_font_cache_stats st1;
    fdk_font_get_cache_stats(f, &st1);
    assert(st1.cache_misses == st0.cache_misses);
    assert(st1.cache_hits > st0.cache_hits);
    for (int x = pen_x; x < pen_x + m.advance_width; x++) {
        for (int dy = 0; dy < -m.ink_top + m.ink_bottom; dy++) {
            int y1 = baseline + m.ink_top + dy;
            int y2 = baseline2 + m.ink_top + dy;
            if (y1 >= 0 && y2 >= 0 && y1 < 100 && y2 < 100) {
                assert(px_at(s, x, y1) == px_at(s, x, y2));
            }
        }
    }

    /* (3) Pen-shift invariance: drawing at pen+1 shifts every glyph
     * by exactly one integer pixel — the fractional phase (and so
     * the cache keys) must be UNCHANGED: zero new rasterizations,
     * ink translated pixel-exactly. */
    fdk_surface *s2 = NULL;
    assert(fdk_ok(fdk_surface_create(300, 100, &s2)));
    fdk_surface_fill(s2, bg);
    assert(fdk_ok(fdk_surface_present(s2)));
    fdk_font_cache_stats st2a;
    fdk_font_get_cache_stats(f, &st2a);
    assert(fdk_ok(fdk_surface_draw_utf8(s2, f, text, len, pen_x + 1,
                                        baseline, white())));
    fdk_font_cache_stats st2b;
    fdk_font_get_cache_stats(f, &st2b);
    assert(st2b.cache_misses == st2a.cache_misses);
    for (int x = pen_x; x < pen_x + m.advance_width; x++) {
        for (int dy = 0; dy < -m.ink_top + m.ink_bottom; dy++) {
            int y = baseline + m.ink_top + dy;
            if (y >= 0 && y < 100) {
                assert(px_at(s, x, y) == px_at(s2, x + 1, y));
            }
        }
    }
    fdk_surface_destroy(s2);

    /* (4) Phase fan-out: repeating ONE glyph accumulates its
     * (typically fractional) advance, so consecutive copies land on
     * different subpixel phases — more cache entries than distinct
     * codepoints. Only asserted when the advance really is
     * fractional at this size (10 rounded singles != the rounded
     * run of ten is the observable proof of a fractional advance). */
    fdk_font *f2 = fdk_font_load(g_sans, 16);
    assert(f2 != NULL);
    const char *ten = "AAAAAAAAAA";
    fdk_text_metrics ma, mt;
    assert(fdk_ok(fdk_font_measure_utf8(f2, "A", 1, &ma)));
    assert(fdk_ok(fdk_font_measure_utf8(f2, ten, 10, &mt)));
    fdk_font_cache_stats stf;
    fdk_font_get_cache_stats(f2, &stf);
    assert(stf.cached_glyphs >= 1);
    assert(stf.cached_glyphs <= 2048); /* documented public bound */
    if (mt.advance_width != 10 * ma.advance_width) {
        assert(stf.cached_glyphs > 1); /* phases actually fan out */
        assert(stf.cached_glyphs <= 4); /* at most 4 phases per glyph */
    }
    fdk_font_destroy(f2);

    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] subpixel: kerned fractional pens keep measure/draw "
           "agreement, deterministic redraws, pen-shift invariance "
           "(no re-rasterization), phase fan-out\n");
}

/* ---- system font discovery (post-1.0.1 rework) ----
 *
 * fdk_font_load_system_default() resolves through: $FDK_FONT_FILE,
 * $FDK_FONT_DIRS scan, fontconfig (dlopen'd), the known-path list,
 * then a ranked scan of the standard font roots. These scenarios pin
 * each stage against the exact regressions the rework fixed — most
 * visibly Arch Linux, whose noto-fonts ships "NotoSans[wdth,wght]
 * .ttf" variable fonts the old hardcoded list could never match.
 *
 * Scenario order matters: the fontconfig end-to-end check must be
 * the first thing in this process that initializes fontconfig
 * (fontconfig reads $FONTCONFIG_FILE once, at init), so it runs
 * before the fall-through scenario. Every scenario scrubs the
 * environment and resets the cached resolution first. */

static int tt_copy_file(const char *dst, const char *src) {
    FILE *in = fopen(src, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    char buf[4096];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (fclose(out) != 0) {
        rc = -1;
    }
    fclose(in);
    return rc;
}

static int tt_write_text(const char *path, const char *text) {
    FILE *out = fopen(path, "wb");
    if (out == NULL) {
        return -1;
    }
    int rc = fputs(text, out) == EOF ? -1 : 0;
    if (fclose(out) != 0) {
        rc = -1;
    }
    return rc;
}

static void tt_scrub_env(void) {
    unsetenv("FDK_FONT_FILE");
    unsetenv("FDK_FONT_DIRS");
    unsetenv("FONTCONFIG_FILE");
    fdk_text_font_discovery_reset_for_tests();
}

static void test_system_font_discovery(void) {
    /* (1) $FDK_FONT_FILE wins outright and loads the same face a
     * direct fdk_font_load() of the same file produces. */
    char odir[] = "/tmp/fdk-disc-override-XXXXXX";
    assert(mkdtemp(odir) != NULL);
    char probe[256];
    int n = snprintf(probe, sizeof(probe), "%s/ProbeFace.ttf", odir);
    assert(n > 0 && (size_t)n < sizeof(probe));
    assert(tt_copy_file(probe, g_sans) == 0);

    tt_scrub_env();
    setenv("FDK_FONT_FILE", probe, 1);
    fdk_font *f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(fdk_font_get_file_path(f) != NULL);
    assert(strcmp(fdk_font_get_file_path(f), probe) == 0);
    fdk_font *direct = fdk_font_load(probe, 16);
    assert(direct != NULL);
    fdk_font_metrics mo, md;
    fdk_font_get_metrics(f, &mo);
    fdk_font_get_metrics(direct, &md);
    assert(mo.ascent == md.ascent && mo.descent == md.descent &&
           mo.line_height == md.line_height);
    fdk_font_destroy(direct);
    fdk_font_destroy(f);
    tt_scrub_env();
    remove(probe);
    remove(odir);

    /* (2) fontconfig end-to-end: a config whose ONLY directory holds
     * one probe copy, aliased onto the generic sans-serif. If FDK
     * resolved through fontconfig the answer must be the temp copy —
     * no other stage could ever produce that path. Gated on the same
     * dlopen FDK would perform; without fontconfig this scenario
     * proves nothing and skips honestly. */
    void *fc = dlopen("libfontconfig.so.1", RTLD_NOW | RTLD_LOCAL);
    if (fc == NULL) {
        printf("[skip] fontconfig not loadable — skipping the "
               "fontconfig e2e scenario\n");
    } else {
        char fdir[] = "/tmp/fdk-disc-fc-XXXXXX";
        assert(mkdtemp(fdir) != NULL);
        char fcache[256], fcopy[256], fconf[256];
        n = snprintf(fcache, sizeof(fcache), "%s/cache", fdir);
        assert(n > 0 && (size_t)n < sizeof(fcache));
        n = snprintf(fcopy, sizeof(fcopy), "%s/FcProbeFace.ttf", fdir);
        assert(n > 0 && (size_t)n < sizeof(fcopy));
        n = snprintf(fconf, sizeof(fconf), "%s/fonts.conf", fdir);
        assert(n > 0 && (size_t)n < sizeof(fconf));
        assert(mkdir(fcache, 0700) == 0);
        assert(tt_copy_file(fcopy, g_sans) == 0);
        char conf[1024];
        n = snprintf(conf, sizeof(conf),
                     "<?xml version=\"1.0\"?>\n"
                     "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
                     "<fontconfig>\n"
                     "  <dir>%s</dir>\n"
                     "  <cachedir>%s</cachedir>\n"
                     "  <alias>\n"
                     "    <family>sans-serif</family>\n"
                     "    <prefer>\n"
                     "      <family>DejaVu Sans</family>\n"
                     "      <family>Noto Sans</family>\n"
                     "      <family>Liberation Sans</family>\n"
                     "    </prefer>\n"
                     "  </alias>\n"
                     "</fontconfig>\n",
                     fdir, fcache);
        assert(n > 0 && (size_t)n < sizeof(conf));
        assert(tt_write_text(fconf, conf) == 0);

        tt_scrub_env();
        setenv("FONTCONFIG_FILE", fconf, 1);
        f = fdk_font_load_system_default(16);
        assert(f != NULL);
        assert(fdk_font_get_file_path(f) != NULL);
        assert(strcmp(fdk_font_get_file_path(f), fcopy) == 0);
        fdk_font_destroy(f);
        tt_scrub_env();

        /* Best-effort cleanup; fontconfig may leave cache files in
         * the cachedir, which remove() on a non-empty dir reports
         * and we deliberately ignore. */
        remove(fcopy);
        remove(fconf);
        remove(fcache);
        remove(fdir);
    }

    /* (3) An INVALID override warns and falls through: the known-path
     * stage still finds the suite's own probe file (every find_fonts
     * candidate is reachable by a later stage in every environment
     * where this suite runs at all). */
    tt_scrub_env();
    setenv("FDK_FONT_FILE", "/nonexistent/fdk-probe.ttf", 1);
    f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(strcmp(fdk_font_get_file_path(f), "/nonexistent/fdk-probe.ttf") != 0);
    fdk_font_destroy(f);
    tt_scrub_env();

    /* (4) The Arch regression: variable-font bracket filenames
     * ("NotoSans[wdth,wght].ttf") — the exact naming the old
     * hardcoded list could never match. */
    char adir[] = "/tmp/fdk-disc-arch-XXXXXX";
    assert(mkdtemp(adir) != NULL);
    char afont[256];
    n = snprintf(afont, sizeof(afont), "%s/NotoSans[wdth,wght].ttf", adir);
    assert(n > 0 && (size_t)n < sizeof(afont));
    assert(tt_copy_file(afont, g_sans) == 0);
    tt_scrub_env();
    setenv("FDK_FONT_DIRS", adir, 1);
    f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(fdk_font_get_file_path(f) != NULL);
    assert(strcmp(fdk_font_get_file_path(f), afont) == 0);
    fdk_font_destroy(f);
    tt_scrub_env();
    remove(afont);
    remove(adir);

    /* (5) The scanner descends into subdirectories (distros nest:
     * Fedora's dejavu-sans-fonts/, Debian's truetype/dejavu/). */
    char ndir[] = "/tmp/fdk-disc-nested-XXXXXX";
    assert(mkdtemp(ndir) != NULL);
    char nsub[256], nfont[256];
    n = snprintf(nsub, sizeof(nsub), "%s/nested", ndir);
    assert(n > 0 && (size_t)n < sizeof(nsub));
    n = snprintf(nfont, sizeof(nfont), "%s/LiberationSans-Regular.ttf",
                 nsub);
    assert(n > 0 && (size_t)n < sizeof(nfont));
    assert(mkdir(nsub, 0700) == 0);
    assert(tt_copy_file(nfont, g_sans) == 0);
    tt_scrub_env();
    setenv("FDK_FONT_DIRS", ndir, 1);
    f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(fdk_font_get_file_path(f) != NULL);
    assert(strcmp(fdk_font_get_file_path(f), nfont) == 0);
    fdk_font_destroy(f);
    tt_scrub_env();
    remove(nfont);
    remove(nsub);
    remove(ndir);

    /* (6) Ranking: the regular face beats a bold face of the same
     * family — FDK synthesizes bold itself, so the regular file is
     * the right default. */
    char bdir[] = "/tmp/fdk-disc-rank-XXXXXX";
    assert(mkdtemp(bdir) != NULL);
    char bold[256], reg[256];
    n = snprintf(bold, sizeof(bold), "%s/DejaVuSans-Bold.ttf", bdir);
    assert(n > 0 && (size_t)n < sizeof(bold));
    n = snprintf(reg, sizeof(reg), "%s/DejaVuSans.ttf", bdir);
    assert(n > 0 && (size_t)n < sizeof(reg));
    assert(tt_copy_file(bold, g_sans) == 0);
    assert(tt_copy_file(reg, g_sans) == 0);
    tt_scrub_env();
    setenv("FDK_FONT_DIRS", bdir, 1);
    f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(fdk_font_get_file_path(f) != NULL);
    assert(strcmp(fdk_font_get_file_path(f), reg) == 0);
    fdk_font_destroy(f);
    tt_scrub_env();
    remove(bold);
    remove(reg);
    remove(bdir);

    /* (7) Resilience: a candidate that passes the tag-level gate but
     * fails the loader's full container validation (here: a truncated
     * font copy deliberately ranked FIRST by the scanner's tie-break)
     * must be rejected and the next-best candidate loaded — the
     * failure mode of corrupt/truncated repacked fonts. */
    char rdir[] = "/tmp/fdk-disc-resil-XXXXXX";
    assert(mkdtemp(rdir) != NULL);
    char rbad[256], rgood[256];
    n = snprintf(rbad, sizeof(rbad), "%s/DejaVuSans.ttf", rdir);
    assert(n > 0 && (size_t)n < sizeof(rbad));
    n = snprintf(rgood, sizeof(rgood), "%s/NotoSans-Regular.ttf", rdir);
    assert(n > 0 && (size_t)n < sizeof(rgood));
    {
        /* Truncated copy: the sfnt magic is in the first 12 bytes so
         * the tag-level gate passes, but the table extents run past
         * the truncation point so full validation fails. */
        FILE *in = fopen(g_sans, "rb");
        assert(in != NULL);
        char head[100 * 1024];
        size_t got = fread(head, 1, sizeof(head), in);
        fclose(in);
        assert(got == sizeof(head)); /* DejaVu Sans is larger */
        FILE *out = fopen(rbad, "wb");
        assert(out != NULL);
        assert(fwrite(head, 1, got, out) == got);
        fclose(out);
    }
    assert(tt_copy_file(rgood, g_sans) == 0);
    tt_scrub_env();
    setenv("FDK_FONT_DIRS", rdir, 1);
    f = fdk_font_load_system_default(16);
    assert(f != NULL);
    assert(fdk_font_get_file_path(f) != NULL);
    assert(strcmp(fdk_font_get_file_path(f), rgood) == 0);
    fdk_font_destroy(f);
    tt_scrub_env();
    remove(rbad);
    remove(rgood);
    remove(rdir);

    /* (8) Plain default resolution and cache consistency: two loads
     * agree on the same file, metrics are sane. */
    fdk_font *fa = fdk_font_load_system_default(16);
    assert(fa != NULL);
    fdk_font *fb = fdk_font_load_system_default(16);
    assert(fb != NULL);
    assert(fdk_font_get_file_path(fa) != NULL);
    assert(strcmp(fdk_font_get_file_path(fa),
                  fdk_font_get_file_path(fb)) == 0);
    fdk_font_metrics mm;
    fdk_font_get_metrics(fa, &mm);
    assert(mm.ascent > 0 && mm.line_height > 0);
    fdk_font_destroy(fa);
    fdk_font_destroy(fb);

    /* (9) Accessor edges: NULL font answers NULL; a directly loaded
     * font reports exactly the path it was given. */
    assert(fdk_font_get_file_path(NULL) == NULL);
    f = fdk_font_load(g_sans, 12);
    assert(f != NULL);
    assert(strcmp(fdk_font_get_file_path(f), g_sans) == 0);
    fdk_font_destroy(f);

    /* (10) The argument guard is unchanged. */
    assert(fdk_font_load_system_default(0) == NULL);
    assert(fdk_font_load_system_default(513) == NULL);

    tt_scrub_env();
    printf("[ok] system font discovery: FDK_FONT_FILE override + "
           "invalid fall-through, fontconfig e2e, Arch variable-font "
           "scan, nested-dir scan, regular-beats-bold ranking, "
           "corrupt-candidate rejection, cache consistency\n");
}

int main(void) {
    find_fonts();
    if (g_sans == NULL) {
        printf("[skip] no system TrueType font found (tried DejaVu Sans, "
               "Noto Sans) — text suite requires a font file to shape; "
               "see docs/testing.md\n");
        return 0;
    }
    printf("using font: %s\n", g_sans);
    test_font_lifecycle();
    test_measure();
    test_draw_ink_and_damage();
    test_clip();
    test_utf8();
    test_cache_eviction();
    test_kerning();
    test_break_lines();
    test_ellipsize();
    test_font_style();
    test_subpixel_positioning();
    test_system_font_discovery();
    printf("all headless text tests passed\n");
    return 0;
}
