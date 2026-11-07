/*
 * test_selection_pixels.c — the shaped-selection geometry and its
 * PIXEL-LEVEL pins (the Devanagari highlight fix).
 *
 * The bug being pinned (found live on shaped Devanagari): the Entry
 * and TextView selection highlights were drawn from BYTE-PREFIX
 * measurements — "width of text[0..lo)" to "width of text[0..hi)".
 * A prefix that CUTS a grapheme cluster shapes differently than the
 * whole cluster (a half-form consonant's advance is not the
 * cluster's own advance), so the highlight landed mid-glyph, spilled
 * onto neighbours, and grew as the selection moved; on bidi lines a
 * single rectangle could not represent the range at all.
 *
 * The fix's seams, both pinned here:
 *   - fdk_font_selection_spans_utf8: the visual spans of a logical
 *     byte range, snapped to grapheme clusters (API-level pins);
 *   - the Entry/TextView highlight PAINTS those spans (pixel-level
 *     pins: the painted band's extent is scanned off the surface and
 *     compared against the span geometry).
 *
 * Legs skip honestly when their script's font is absent (the §10
 * matrix's discipline): Devanagari needs a Noto-class Indic face,
 * Hebrew/Arabic ride DejaVu's own coverage.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_a11y.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_widgets.h"

#include "text/text_internal.h" /* the font-resolution seam (the
                                  * script matrix's own pair) */
#include "core/alloc_internal.h" /* fdk_free (the resolver's path) */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static fdk_font *g_sans = NULL; /* DejaVu-class Latin face          */
static int g_has_deva = 0;      /* a Devanagari face is loadable    */

static void find_faces(void) {
    static const char *sans_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        NULL,
    };
    for (int i = 0; sans_candidates[i] != NULL; i++) {
        FILE *f = fopen(sans_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            g_sans = fdk_font_load(sans_candidates[i], 16);
            if (g_sans != NULL) {
                break;
            }
        }
    }
    /* Devanagari coverage probe: any loadable face covering U+0938
     * (the script matrix's own resolution walk). */
    const char *skip[4] = {NULL, NULL, NULL, NULL};
    int n_skip = 0;
    for (int attempt = 0; attempt < 4 && !g_has_deva; attempt++) {
        char *path = NULL;
        long face = 0;
        if (!fdk_text_resolve_fallback_font("hi", 0x0938, skip,
                                            n_skip, &path, &face)) {
            break;
        }
        fdk_font *cand = fdk_text_font_load_face(path, face, 16);
        if (cand != NULL && fdk_font_has_glyph(cand, 0x0938)) {
            g_has_deva = 1;
        }
        if (cand != NULL) {
            fdk_font_destroy(cand);
        }
        if (!g_has_deva) {
            if (n_skip < 4) {
                skip[n_skip++] = path;
            }
        }
    }
    for (int i = 0; i < n_skip; i++) {
        fdk_free((void *)(uintptr_t)skip[i]);
    }
}

/* ---- API-level pins --------------------------------------------------- */

/* The whole-text advance (the bidi assertion's upper bound). */
static fdk_i32 m_advance_cap(fdk_font *f, const char *t, size_t len) {
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(f, t, len, &m)));
    return m.advance_width;
}

/* "क स्ते ज": क(3) ' '(1) स(3) ्(3) त(3) े(3) ' '(1) ज(3) = 20 bytes.
 * The स्ते cluster (conjunct + vowel sign) spans bytes [4, 16):
 * byte 7 (between स and ्) is a CODEPOINT boundary INSIDE the
 * grapheme cluster — exactly where a caret can sit and the old
 * prefix-measure painted a mid-cluster edge. */
#define DEVA_TEXT "\xE0\xA4\x95 \xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87 \xE0\xA4\x9C"
#define DEVA_CL0 4u   /* स्ते first byte                       */
#define DEVA_CL1 16u  /* past े — the cluster's end            */
#define DEVA_MID 7u   /* cp boundary INSIDE the cluster        */

static void test_spans_latin(void) {
    const char *t = "hello world";
    size_t len = strlen(t);
    fdk_text_span sp[8];
    size_t n = 0;

    /* Whole line: one span covering the advance. */
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(g_sans, t, len, &m)));
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 0,
                                                len, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 == 0);
    assert(sp[0].x1 >= m.advance_width - 1 &&
           sp[0].x1 <= m.advance_width + 1);

    /* [0,5) "hello": its right edge is where the space's span
     * begins ([5,11) selects the space too), and "world" starts one
     * space-width later — no overlap, exactly one space of gap. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 0, 5,
                                                sp, 8, &n)));
    assert(n == 1);
    fdk_i32 hello_x1 = sp[0].x1;
    assert(hello_x1 > 0);
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 5,
                                                len, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 == hello_x1); /* the space rides the right span */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 6,
                                                len, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 > hello_x1); /* ...and "world" leaves the gap */
    fdk_text_span sp_all[8];
    size_t n_all = 0;
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 0,
                                                len, sp_all, 8,
                                                &n_all)));
    assert(sp[0].x1 == sp_all[0].x1);

    /* Empty selection: zero spans, still FDK_OK. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 3, 3,
                                                sp, 8, &n)));
    assert(n == 0);
    printf("[ok] sel-spans api: latin adjacency, whole-line, empty\n");
}

static void test_spans_devanagari_snap(void) {
    if (!g_has_deva) {
        printf("[skip] sel-spans devanagari (no Indic face — the "
               "cluster-snap legs need real conjunct shaping)\n");
        return;
    }
    const char *t = DEVA_TEXT;
    size_t len = strlen(t);
    fdk_text_span sp[8];
    size_t n = 0;

    /* The WHOLE स्ते cluster: one span (the line is single-direction). */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len,
                                                DEVA_CL0, DEVA_CL1,
                                                sp, 8, &n)));
    assert(n == 1);
    fdk_i32 cl_x0 = sp[0].x0, cl_x1 = sp[0].x1;
    assert(cl_x1 > cl_x0); /* real width: the conjunct's advance */

    /* MID-CLUSTER START (byte 7 — the cp boundary inside स्ते): the
     * span must EQUAL the whole-cluster span — the snap-outward
     * rule. THE REGRESSION PIN: the old prefix-measure produced a
     * different (spilling) rectangle here. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len,
                                                DEVA_MID, DEVA_CL1,
                                                sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 == cl_x0);
    assert(sp[0].x1 == cl_x1);

    /* MID-CLUSTER END (byte 13 — between त and े): same equality. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len,
                                                DEVA_CL0, 13,
                                                sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 == cl_x0);
    assert(sp[0].x1 == cl_x1);

    /* ZERO-WIDTH MARKS never widen: the cluster contains ् and े
     * (zero/near-zero advance marks); its span is bounded by the
     * NEIGHBOURS' spans on both sides, and the neighbours do not
     * leak into it either. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len, 0,
                                                DEVA_CL0, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x1 <= cl_x0); /* "क " ends at or before स्ते starts */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, t, len,
                                                DEVA_CL1, len, sp, 8,
                                                &n)));
    assert(n == 1);
    assert(sp[0].x0 >= cl_x1); /* " ज" starts at or after स्ते ends */

    printf("[ok] sel-spans api: devanagari cluster snap (mid-cluster "
           "selection == whole-cluster box), marks never widen, "
           "neighbours never leak\n");
}

static void test_spans_rtl_and_bidi_split(void) {
    /* Hebrew, isolated run: "אבגד" — the LOGICAL halves map to the
     * VISUAL halves, swapped: "אב" (logical first half) is the
     * visual RIGHT half, flush against the line's right edge, and
     * the two halves' spans meet EXACTLY at the advance midpoint
     * (the mapping is a clean reversal, no gaps, no overlap). */
    const char *heb = "\xD7\x90\xD7\x91\xD7\x92\xD7\x93"; /* אבגד */
    size_t hlen = strlen(heb);
    fdk_text_span sp[8];
    size_t n = 0;
    fdk_text_metrics hm;
    assert(fdk_ok(fdk_font_measure_utf8(g_sans, heb, hlen, &hm)));
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, heb, hlen, 0,
                                                hlen, sp, 8, &n)));
    assert(n == 1);
    fdk_i32 total = sp[0].x1;
    assert(total >= hm.advance_width - 1);
    /* Logical "אב" (first half): flush right. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, heb, hlen, 0,
                                                4, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x1 == total); /* flush right */
    fdk_i32 first_half_x0 = sp[0].x0;
    /* Logical "גד" (second half): flush left, and the halves meet. */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, heb, hlen, 4,
                                                hlen, sp, 8, &n)));
    assert(n == 1);
    assert(sp[0].x0 == 0); /* flush left */
    assert(sp[0].x1 == first_half_x0); /* exact midpoint meeting */

    /* THE BIDI SPLIT: "aaa אבגד bbb", logical [2, 8) = "a" + "אב".
     * Visual order: "aaa " + "דגבא" + " bbb" — the selected pieces
     * are the third 'a' + space (left) and the Hebrew block's
     * VISUAL right half (= logical "אב"); the gap between them is
     * the Hebrew block's visual LEFT half (logical "גד",
     * unselected). The selection must come out as TWO spans. */
    const char *mix = "aaa \xD7\x90\xD7\x91\xD7\x92\xD7\x93 bbb";
    size_t mlen = strlen(mix); /* 4 + 8 + 4 = 16 */
    assert(mlen == 16);
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, mix, mlen, 2,
                                                8, sp, 8, &n)));
    assert(n == 2); /* THE multi-rect pin */
    assert(sp[0].x1 < sp[1].x0 - 1); /* a real, wide gap */
    assert(sp[0].x0 > 0);            /* the "a " piece             */
    assert(sp[1].x1 < m_advance_cap(g_sans, mix, mlen));

    /* Selecting the whole line is still ONE span (contiguous logical
     * range covering whole runs stays visually contiguous). */
    assert(fdk_ok(fdk_font_selection_spans_utf8(g_sans, mix, mlen, 0,
                                                mlen, sp, 8, &n)));
    assert(n == 1);

    printf("[ok] sel-spans api: hebrew logical->visual mapping, bidi "
           "mid-run selection splits into two rectangles with a gap\n");
}

/* ---- pixel-level pins -------------------------------------------------- */

static fdk_u32 px_at(fdk_surface *s, int x, int y) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    return info.pixels[(size_t)y * (size_t)info.stride + (size_t)x] &
           0x00FFFFFFu;
}

/* Scans the horizontal band [y0, y1) of two same-size surfaces for
 * differing pixels; reports the min/max x of the differing set and
 * the number of contiguous differing RUNS (runs of identical pixels
 * separate them — the bidi gap detector). */
static int scan_diff(fdk_surface *a, fdk_surface *b, int y0, int y1,
                     int x0, int x1, int *out_min_x, int *out_max_x) {
    int min_x = -1, max_x = -1;
    int runs = 0;
    int in_run = 0;
    for (int x = x0; x < x1; x++) {
        int diff = 0;
        for (int y = y0; y < y1 && !diff; y++) {
            if (px_at(a, x, y) != px_at(b, x, y)) {
                diff = 1;
            }
        }
        if (diff) {
            if (min_x < 0) {
                min_x = x;
            }
            max_x = x;
            if (!in_run) {
                runs++;
                in_run = 1;
            }
        } else {
            in_run = 0;
        }
    }
    *out_min_x = min_x;
    *out_max_x = max_x;
    return runs;
}

/* Paints the tree into a FRESH surface the same size as `s` (the
 * snapshot to diff against another snapshot). */
static void snapshot_paint(fdk_widget *root, fdk_surface *s,
                           fdk_surface **out_snapshot) {
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    assert(fdk_ok(fdk_surface_create(info.width, info.height,
                                     out_snapshot)));
    fdk_surface_invalidate_all(*out_snapshot);
    fdk_widget_tree_paint(root, *out_snapshot);
}

/* ENTRY pixel pin: the painted selection band equals the span
 * geometry — mid-cluster and whole-cluster selections paint the
 * SAME band, and the band never leaks into the neighbours. */
static void test_entry_pixels_devanagari(void) {
    if (!g_has_deva) {
        printf("[skip] sel-pixels entry devanagari (no Indic face)\n");
        return;
    }
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 260, 60},
                                     &root)));
    fdk_widget *entry = NULL;
    assert(fdk_ok(fdk_entry_create(root, g_sans, DEVA_TEXT, &entry)));
    fdk_rect r = {10, 10, 240, 36};
    fdk_widget_set_bounds(entry, r);

    fdk_surface *sel = NULL;
    assert(fdk_ok(fdk_surface_create(260, 60, &sel)));

    /* The span geometry (the ground truth the band must match). */
    fdk_text_span sp[8];
    size_t n = 0;
    assert(fdk_ok(fdk_font_selection_spans_utf8(
        g_sans, DEVA_TEXT, strlen(DEVA_TEXT), DEVA_CL0, DEVA_CL1, sp,
        8, &n)));
    assert(n == 1);
    fdk_i32 want_x0 = 10 + 8 + sp[0].x0; /* bounds + entry inset */
    fdk_i32 want_x1 = 10 + 8 + sp[0].x1;

    /* Paint 1: WHOLE-cluster selection. */
    assert(fdk_ok(fdk_entry_select_range(entry, DEVA_CL0, DEVA_CL1)));
    fdk_surface *snapA = NULL;
    snapshot_paint(root, sel, &snapA);
    /* Paint 2: MID-cluster selection (anchor inside स्ते). */
    assert(fdk_ok(fdk_entry_select_range(entry, DEVA_MID, DEVA_CL1)));
    fdk_surface *snapB = NULL;
    snapshot_paint(root, sel, &snapB);
    /* Paint 3: NO selection (the reference). */
    assert(fdk_ok(fdk_entry_select_range(entry, DEVA_CL1, DEVA_CL1)));
    fdk_surface *snapC = NULL;
    snapshot_paint(root, sel, &snapC);

    int a0 = -1, a1 = -1, b0 = -1, b1 = -1;
    int runsA = scan_diff(snapA, snapC, 12, 44, 0, 260, &a0, &a1);
    int runsB = scan_diff(snapB, snapC, 12, 44, 0, 260, &b0, &b1);
    (void)runsA;
    (void)runsB;

    /* THE SNAP PIN, pixel-level: the mid-cluster band IS the
     * whole-cluster band (same extent, ±1px of antialiasing at the
     * edges). Pre-fix these differed — the mid-cluster band spilled
     * past the cluster's box. */
    assert(a0 >= 0 && b0 >= 0);
    assert(b0 >= a0 - 1 && b0 <= a0 + 1);
    assert(b1 >= a1 - 1 && b1 <= a1 + 1);

    /* THE BOX PIN: the band equals the span geometry (±1px) — the
     * rectangle is the cluster's advance box, the neighbours stay
     * unpainted, and the zero-width marks inside the cluster did
     * not widen it. */
    assert(a0 >= want_x0 - 1 && a0 <= want_x0 + 1);
    assert(a1 >= want_x1 - 1 && a1 <= want_x1 + 1);

    fdk_surface_destroy(snapA);
    fdk_surface_destroy(snapB);
    fdk_surface_destroy(snapC);
    fdk_surface_destroy(sel);
    fdk_widget_destroy(root);
    printf("[ok] sel-pixels entry: devanagari cluster band — "
           "mid-cluster == whole-cluster, extent == the span box "
           "(no spill, no widening)\n");
}

/* TEXTVIEW pixel pin: the bidi split selection paints TWO separated
 * bands on one line. */
static void test_textview_pixels_bidi(void) {
    const char *mix = "aaa \xD7\x90\xD7\x91\xD7\x92\xD7\x93 bbb";
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 260, 80},
                                     &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, g_sans, &tv)));
    fdk_rect r = {10, 10, 240, 60};
    fdk_widget_set_bounds(tv, r);
    fdk_textview_set_wrap(tv, FDK_TEXTVIEW_WRAP_NONE);
    fdk_textview_set_text(tv, mix);

    fdk_surface *sel = NULL;
    assert(fdk_ok(fdk_surface_create(260, 80, &sel)));

    /* Selection [2, 8): "a" + "אב" — the two-rectangle case. */
    assert(fdk_ok(fdk_a11y_text_set_selection(tv, 2, 8)));
    fdk_surface *snapA = NULL;
    snapshot_paint(root, sel, &snapA);
    /* Collapse: the reference. */
    assert(fdk_ok(fdk_a11y_text_set_selection(tv, 2, 2)));
    fdk_surface *snapC = NULL;
    snapshot_paint(root, sel, &snapC);

    int x0 = -1, x1 = -1;
    int runs = scan_diff(snapA, snapC, 12, 68, 0, 260, &x0, &x1);
    /* THE MULTI-RECT PIN, pixel-level: TWO separated bands on the
     * single line (the gap is the unselected visual half of the
     * Hebrew block — several pixels wide). */
    assert(runs == 2);
    assert(x0 >= 0 && x1 > x0);

    fdk_surface_destroy(snapA);
    fdk_surface_destroy(snapC);
    fdk_surface_destroy(sel);
    fdk_widget_destroy(root);
    printf("[ok] sel-pixels textview: bidi mid-run selection paints "
           "two separated highlight bands on one line\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    find_faces();
    if (g_sans == NULL) {
        printf("[skip] no system TrueType font found — selection "
               "geometry needs real glyphs; see docs/testing.md\n");
        return 0;
    }

    test_spans_latin();
    test_spans_devanagari_snap();
    test_spans_rtl_and_bidi_split();
    test_entry_pixels_devanagari();
    test_textview_pixels_bidi();

    printf("all selection-geometry tests passed\n");
    return 0;
}
