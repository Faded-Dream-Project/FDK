/* test_cff.c — the CFF-outline item: native Type 2 charstring proof
 * tests (1.1.0; the Arch tofu report's closure — 'OTTO' faces were
 * rejected outright before this).
 *
 * Three fonts under test:
 *
 *   - FdkSeac.otf, CRAFTED for this suite (the embedded bytes below):
 *     real fonts on a stock Debian carry no seac, no cyclic subrs, and
 *     precious few flex curves anymore — the crafted font carries
 *     endchar-seac composition (A + grave), local and global subr
 *     calls (the bias rule, stack persistence across calls), the
 *     width argument on first-stack-clearing operators, one
 *     implicitly and one explicitly closed contour, and a CYCLIC
 *     subr (the operator-budget guard). Its plain glyphs must ALSO
 *     shape identically through the vendored stb_truetype's own
 *     Type 2 path — that cross-check validates the font itself.
 *
 *   - URW base35 (fonts-urw-base35): name-keyed CFF from a real
 *     foundry. Required: CI installs it; the suite skips honestly
 *     (with a printed notice) where a distro lacks it.
 *
 *   - Noto Sans CJK (optional): the CID-keyed posture (FDArray/
 *     FDSelect, a shared CharStrings INDEX across collection faces)
 *     — the exact shape of the tofu report. Skips when absent.
 *
 * The crown fence is the VERTEX AGREEMENT SWEEP: for every glyph
 * examined, this module's interpreter and stb's own Type 2 path must
 * emit byte-identical vertex streams — two independent decodings of
 * the same charstrings agreeing is the strongest reference available
 * without shipping a third rasterizer.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_surface.h"

#include "text/text_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- fonts under test ------------------------------------------------ */

static const char *g_urw = NULL;   /* name-keyed CFF (required)   */
static const char *g_noto = NULL;  /* CID-keyed CFF (optional)    */

static const char *urw_candidates[] = {
    /* Debian/Ubuntu (fonts-urw-base35) and Void */
    "/usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf",
    "/usr/share/fonts/opentype/urw-base35/C059-Roman.otf",
    /* Alpine (font-urw-base35 — the package NAME is the directory)
     * and Fedora (urw-base35-fonts) */
    "/usr/share/fonts/urw-base35/NimbusSans-Regular.otf",
    "/usr/share/fonts/urw-base35/C059-Roman.otf",
    "/usr/share/fonts/font-urw-base35/NimbusSans-Regular.otf",
    "/usr/share/fonts/font-urw-base35/C059-Roman.otf",
    /* Arch (gsfonts) */
    "/usr/share/fonts/gsfonts/NimbusSans-Regular.otf",
    NULL,
};
static const char *noto_candidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/home/z/.local/share/fonts/NotoSansCJK-Regular.ttc",
    NULL,
};

static void find_fonts(void) {
    for (int i = 0; urw_candidates[i] != NULL; i++) {
        FILE *f = fopen(urw_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            g_urw = urw_candidates[i];
            break;
        }
    }
    for (int i = 0; noto_candidates[i] != NULL; i++) {
        FILE *f = fopen(noto_candidates[i], "rb");
        if (f != NULL) {
            fclose(f);
            g_noto = noto_candidates[i];
            break;
        }
    }
    if (g_urw == NULL) {
        printf("[skip] no URW base35 CFF font found — the CFF suite "
               "needs one (CI installs fonts-urw-base35)\n");
    }
}

/* ---- the crafted FdkSeac.otf (see scripts/gen_seac_font.py) --------- */

static const unsigned char k_fdkseac[436] = {
    0x4F, 0x54, 0x54, 0x4F, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00, 0x50,
    0x43, 0x46, 0x46, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6C,
    0x00, 0x00, 0x00, 0x8A, 0x63, 0x6D, 0x61, 0x70, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x44, 0x68, 0x65, 0x61, 0x64,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3C, 0x00, 0x00, 0x00, 0x36,
    0x68, 0x68, 0x65, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x74,
    0x00, 0x00, 0x00, 0x24, 0x68, 0x6D, 0x74, 0x78, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x98, 0x00, 0x00, 0x00, 0x14, 0x6D, 0x61, 0x78, 0x70,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xAC, 0x00, 0x00, 0x00, 0x06,
    0x01, 0x00, 0x04, 0x02, 0x00, 0x01, 0x01, 0x01, 0x08, 0x46, 0x64, 0x6B,
    0x53, 0x65, 0x61, 0x63, 0x00, 0x01, 0x01, 0x01, 0x08, 0xCF, 0x11, 0xB2,
    0x0F, 0x92, 0xBB, 0x12, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x05, 0x8B,
    0xDB, 0x05, 0x0B, 0x00, 0x00, 0x22, 0x00, 0x7C, 0x00, 0xAF, 0x00, 0x01,
    0xF8, 0x88, 0x15, 0x8B, 0x14, 0x92, 0x13, 0x00, 0x02, 0x01, 0x01, 0x06,
    0x08, 0xF7, 0xC0, 0x8B, 0x05, 0x0B, 0x21, 0x0A, 0x00, 0x05, 0x01, 0x01,
    0x17, 0x26, 0x35, 0x3C, 0x3E, 0xF8, 0x88, 0xEF, 0xEF, 0x15, 0xF8, 0x24,
    0x8B, 0x05, 0x8B, 0xF8, 0x24, 0x05, 0xFC, 0x24, 0x8B, 0x05, 0x8B, 0xFC,
    0x24, 0x05, 0x0E, 0x8B, 0xBD, 0xBD, 0x15, 0x72, 0xF7, 0x5C, 0x05, 0x20,
    0x0A, 0x72, 0xFB, 0x5C, 0x05, 0x0E, 0xF9, 0x50, 0xF8, 0xEC, 0x15, 0x8B,
    0xDB, 0x05, 0xC7, 0x3B, 0x05, 0x4F, 0x3B, 0x05, 0x0E, 0x8B, 0xF8, 0xEC,
    0xCC, 0xF7, 0x55, 0x0E, 0x21, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x04, 0x00, 0x38,
    0x00, 0x00, 0x00, 0x0A, 0x00, 0x08, 0x00, 0x02, 0x00, 0x02, 0x00, 0x41,
    0x00, 0x60, 0x00, 0xC0, 0xE0, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x41,
    0x00, 0x60, 0x00, 0xC0, 0xE0, 0x00, 0xFF, 0xFF, 0xFF, 0xC0, 0xFF, 0xA2,
    0xFF, 0x43, 0x20, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x5F, 0x0F, 0x3C, 0xF5, 0x00, 0x00, 0x03, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8, 0x03, 0xE8,
    0x00, 0x00, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x03, 0x20, 0xFF, 0x38, 0x00, 0x00, 0x03, 0xE8,
    0x00, 0x00, 0x00, 0x00, 0x03, 0xE8, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05,
    0x01, 0xF4, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00,
    0x01, 0xF4, 0x00, 0x00, 0x01, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00,
    0x00, 0x05, 0x00, 0x00,
};

/* Written once to /tmp and loaded through the public file loader —
 * the loader is the surface under test, not fdk__cff_parse alone. */
static const char *k_seac_path = "/tmp/fdk_cff_seac.otf";

static void write_seac_font(void) {
    FILE *f = fopen(k_seac_path, "wb");
    assert(f != NULL);
    assert(fwrite(k_fdkseac, 1, sizeof k_fdkseac, f) == sizeof k_fdkseac);
    fclose(f);
}

/* ---- helpers --------------------------------------------------------- */

static int count_ink(fdk_surface *s, int *interior) {
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return -1;
    }
    const fdk_u32 *px = (const fdk_u32 *)info.pixels;
    int ink = 0, inner = 0;
    for (int y = 0; y < info.height; y++) {
        for (int x = 0; x < info.width; x++) {
            fdk_u32 c = px[y * info.stride + x];
            if (((c >> 16) & 0xFF) < 100 && ((c >> 8) & 0xFF) < 100 &&
                (c & 0xFF) < 100) {
                ink++;
                if (x >= info.width / 4 && x < info.width - info.width / 4 &&
                    y >= info.height / 4 && y < info.height - info.height / 4) {
                    inner++;
                }
            }
        }
    }
    if (interior != NULL) {
        *interior = inner;
    }
    return ink;
}

static fdk_color ink_color(void) {
    fdk_color c = {0.06f, 0.06f, 0.06f, 1.0f};
    return c;
}

static fdk_color paper_color(void) {
    fdk_color c = {1.0f, 1.0f, 1.0f, 1.0f};
    return c;
}

/* Compares this module's shape against stb's own Type 2 decode of
 * the same glyph, vertex for vertex. Returns the number of vertices
 * (both agreed), or -1 on any divergence (already printed). */
static int agree_stb(fdk_font *f, int gid) {
    stbtt_vertex *mv = NULL;
    int mnv = fdk__cff_glyph_shape(f->cff, gid, &mv);
    stbtt_vertex *sv = NULL;
    int snv = stbtt_GetGlyphShape(&f->info, gid, &sv);
    int ok = 1;
    if (mnv != snv) {
        printf("  gid %d: vertex count mine=%d stb=%d\n", gid, mnv, snv);
        ok = 0;
    }
    for (int i = 0; ok && i < mnv; i++) {
        if (mv[i].type != sv[i].type || mv[i].x != sv[i].x ||
            mv[i].y != sv[i].y || mv[i].cx != sv[i].cx ||
            mv[i].cy != sv[i].cy || mv[i].cx1 != sv[i].cx1 ||
            mv[i].cy1 != sv[i].cy1) {
            printf("  gid %d v%d: mine t=%d(%d,%d c %d,%d %d,%d) "
                   "stb t=%d(%d,%d c %d,%d %d,%d)\n",
                   gid, i, mv[i].type, mv[i].x, mv[i].y, mv[i].cx,
                   mv[i].cy, mv[i].cx1, mv[i].cy1, sv[i].type, sv[i].x,
                   sv[i].y, sv[i].cx, sv[i].cy, sv[i].cx1, sv[i].cy1);
            ok = 0;
        }
    }
    free(mv);
    free(sv); /* stb routes its malloc through STBTT_malloc (libc) */
    return ok ? mnv : -1;
}

/* ---- 1. the crafted font: load, cmap, metrics, draw ------------------ */

static void test_crafted_load(void) {
    fdk_font *f = fdk_font_load_face(k_seac_path, 0, 32);
    assert(f != NULL);
    assert(f->cff != NULL);
    assert(fdk__cff_glyph_count(f->cff) == 5);

    /* cmap round-trip: A/grave/Agrave/E000 (the loader takes cmap
     * through stb's table work, format-agnostic for 'OTTO'). */
    assert(stbtt_FindGlyphIndex(&f->info, 0x41) == 1);
    assert(stbtt_FindGlyphIndex(&f->info, 0x60) == 2);
    assert(stbtt_FindGlyphIndex(&f->info, 0xC0) == 3);
    assert(stbtt_FindGlyphIndex(&f->info, 0xE000) == 4);
    assert(fdk_font_has_glyph(f, 0x41));
    assert(!fdk_font_has_glyph(f, 0x42)); /* B is not in this font */

    /* advance: 500 units at the FDK pixel convention (hhea
     * ascent-descent = 1000): scale = 32/1000. */
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(f, "A", 1, &m)));
    assert(m.advance_width == 16); /* 500 * 32/1000 */
    /* ink: 'A' has no descender — all ink above the baseline, so
     * ink_top < ink_bottom <= 0 in surface coordinates. */
    assert(m.ink_top < 0);
    assert(m.ink_bottom <= 0);
    assert(m.ink_bottom > m.ink_top);

    /* public draw: ink lands where the glyph is. The crafted 'A' is
     * a small triangle (~(1.6..10.4) x (0.8..8)px above the baseline
     * at pen 4): count ink in exactly that window. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(120, 64, &s)));
    fdk_surface_fill(s, paper_color());
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, "A", 1, 4, 40, ink_color())));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(s, &info)));
    const fdk_u32 *px = (const fdk_u32 *)info.pixels;
    int ink = count_ink(s, NULL);
    int body = 0; /* pixels in the A's own window (x 4..16, y 32..40) */
    for (int y = 32; y < 40; y++) {
        for (int x = 4; x < 16; x++) {
            fdk_u32 c = px[y * info.stride + x];
            if (((c >> 16) & 0xFF) < 100) {
                body++;
            }
        }
    }
    assert(ink > 25);
    assert(body > 3);
    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] crafted font: loads as CFF, cmap round-trip, "
           "advance 500u, ink renders in the glyph window\n");
}

/* ---- 2. the crafted font: geometry, seac, the budget guard ---------- */

/* Vertex expectations, derived by hand from the charstrings:
 * gid 1 'A': (50,50) -> (25,250) -> (325,250) -> (300,50) with the
 *   IMPLICIT close back to (50,50) (the charstring ends open).
 * gid 3 'Agrave': seac — A's five vertices, then grave translated
 *   by (0,600): (700,1200) -> (700,1280) -> (760,1200) -> (700,1120)
 *   -> implicit close (700,1200). */
static void expect_verts(const stbtt_vertex *v, int nv,
                         const int (*want)[3], int n_want, int gid) {
    assert(nv == n_want);
    for (int i = 0; i < n_want; i++) {
        if (v[i].type != want[i][0] || v[i].x != want[i][1] ||
            v[i].y != want[i][2]) {
            printf("  gid %d v%d: got t=%d(%d,%d) want t=%d(%d,%d)\n",
                   gid, i, v[i].type, v[i].x, v[i].y, want[i][0],
                   want[i][1], want[i][2]);
            assert(0);
        }
    }
}

static void test_crafted_geometry(void) {
    fdk_font *f = fdk_font_load_face(k_seac_path, 0, 32);
    assert(f != NULL);

    /* gid 0 (.notdef box) and gid 1 ('A'): stb agreement FIRST — it
     * validates the crafted font against the vendored reference. */
    assert(agree_stb(f, 0) == 5);
    assert(agree_stb(f, 1) == 5);
    assert(agree_stb(f, 2) == 5);

    /* gid 1 'A' exact geometry (local subr edge, implicit close). */
    stbtt_vertex *v = NULL;
    int nv = fdk__cff_glyph_shape(f->cff, 1, &v);
    static const int want_a[5][3] = {
        {STBTT_vmove, 50, 50},   {STBTT_vline, 25, 250},
        {STBTT_vline, 325, 250}, {STBTT_vline, 300, 50},
        {STBTT_vline, 50, 50}, /* implicit close */
    };
    expect_verts(v, nv, want_a, 5, 1);
    free(v);

    /* gid 3 'Agrave': the seac composition. */
    nv = fdk__cff_glyph_shape(f->cff, 3, &v);
    static const int want_agrave[10][3] = {
        {STBTT_vmove, 50, 50},    {STBTT_vline, 25, 250},
        {STBTT_vline, 325, 250},  {STBTT_vline, 300, 50},
        {STBTT_vline, 50, 50},    /* A, implicitly closed   */
        {STBTT_vmove, 700, 1200}, {STBTT_vline, 700, 1280},
        {STBTT_vline, 760, 1200}, {STBTT_vline, 700, 1120},
        {STBTT_vline, 700, 1200}, /* grave at (0,600), closed */
    };
    expect_verts(v, nv, want_agrave, 10, 3);
    free(v);

    /* The seac box must also cover both components. */
    int x0, y0, x1, y1;
    assert(fdk__cff_glyph_box(f->cff, 3, &x0, &y0, &x1, &y1));
    assert(x0 == 25 && x1 == 760);
    assert(y0 == 50 && y1 == 1280);

    /* gid 4: the cyclic subr — the operator budget must turn the
     * self-call into a malformed-glyph report, never a hang. */
    nv = fdk__cff_glyph_shape(f->cff, 4, &v);
    assert(nv == -1 && v == NULL);

    fdk_font_destroy(f);
    printf("[ok] crafted geometry: subr edge, implicit close, seac "
           "A+grave@(0,600), cyclic subr budgeted\n");
}

/* ---- 3. the vertex agreement sweep (the crown fence) ----------------- */

static void sweep_agreement(fdk_font *f, int limit, const char *label) {
    int n = fdk__cff_glyph_count(f->cff);
    if (n > limit) {
        n = limit;
    }
    int checked = 0, both_empty = 0;
    for (int g = 0; g < n; g++) {
        int r = agree_stb(f, g);
        if (r < 0) {
            assert(0); /* divergence already printed */
        }
        if (r == 0) {
            both_empty++; /* empty glyph: both decoders agree on nada */
        }
        checked++;
    }
    printf("[ok] %s: %d glyphs, vertex streams identical to stb "
           "(%d empty)\n",
           label, checked, both_empty);
}

static void test_vertex_agreement(void) {
    if (g_urw == NULL) {
        return;
    }
    fdk_font *f = fdk_font_load(g_urw, 32);
    assert(f != NULL);
    sweep_agreement(f, 400, "URW name-keyed CFF");
    fdk_font_destroy(f);

    if (g_noto != NULL) {
        fdk_font *f0 = fdk_font_load_face(g_noto, 0, 32);
        assert(f0 != NULL);
        assert(fdk__cff_glyph_count(f0->cff) == 65535);
        /* the gids the probes proved interesting: .notdef, a Latin
         * face glyph, the JP/SC/TC cjk codepoints' gids. */
        sweep_agreement(f0, 120, "Noto CID face 0");
        int probe_gids[] = {24227, 58935, 15365, 23790, 37746, 65534};
        for (unsigned i = 0; i < sizeof probe_gids / sizeof probe_gids[0];
             i++) {
            /* >= 0: agreement is the fence; gid 65534 legitimately
             * decodes as empty on BOTH sides. */
            assert(agree_stb(f0, probe_gids[i]) >= 0);
        }
        fdk_font_destroy(f0);

        fdk_font *f2 = fdk_font_load_face(g_noto, 2, 32);
        assert(f2 != NULL); /* the collection's SC face */
        sweep_agreement(f2, 120, "Noto CID face 2");
        fdk_font_destroy(f2);
    }
}

/* ---- 4. real fonts render (the tofu report's closure) ---------------- */

static void test_real_render(void) {
    if (g_urw == NULL) {
        return;
    }
    fdk_font *f = fdk_font_load(g_urw, 40);
    assert(f != NULL);
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 90, &s)));
    fdk_surface_fill(s, paper_color());
    const char *latin = "Aa Gg Qg Rr";
    assert(fdk_ok(fdk_surface_draw_utf8(s, f, latin, strlen(latin), 4, 50,
                                        ink_color())));
    int inner = 0;
    int ink = count_ink(s, &inner);
    assert(ink > 600);
    assert(inner > 150); /* dense glyph bodies, not hollow boxes */
    fdk_surface_destroy(s);
    fdk_font_destroy(f);
    printf("[ok] URW CFF: Latin renders with dense ink\n");

    if (g_noto != NULL) {
        fdk_font *c = fdk_font_load_face(g_noto, 2, 40);
        assert(c != NULL);
        fdk_surface *cs = NULL;
        assert(fdk_ok(fdk_surface_create(220, 100, &cs)));
        fdk_surface_fill(cs, paper_color());
        const char *cjk = "\xE6\xBC\xA2\xE5\xAD\x97\xE6\xB8\xAC\xE8\xA9\xA6";
        assert(fdk_ok(fdk_surface_draw_utf8(cs, c, cjk, strlen(cjk), 4, 60,
                                            ink_color())));
        int cinner = 0;
        int cink = count_ink(cs, &cinner);
        /* four full-width glyphs at 40px measure ~800 strictly-dark
         * pixels with ~440 in the interior band (calibrated live); a
         * .notdef box is a hollow rectangle — ~360 border pixels
         * with an EMPTY interior. The interior floor is what closes
         * the tofu signature. */
        assert(cink > 600);
        assert(cinner > 200);
        fdk_surface_destroy(cs);
        fdk_font_destroy(c);
        printf("[ok] Noto CID CFF: CJK renders with dense ink "
               "(tofu closed)\n");
    }
}

/* ---- 5. loader rejection battery ------------------------------------- */

/* Loads a mutated copy of the crafted font; asserts rejection. */
static void reject_copy(const char *label, unsigned char *bytes,
                        size_t len, long face) {
    char path[] = "/tmp/fdk_cff_bad_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, bytes, len) == (ssize_t)len);
    close(fd);
    fdk_font *f = fdk_font_load_face(path, (fdk_i32)face, 16);
    if (f != NULL) {
        printf("  !! %s: EXPECTED rejection, got a font\n", label);
    }
    assert(f == NULL);
    unlink(path);
    printf("  [ok] %s rejected\n", label);
}

static unsigned char *seac_copy(void) {
    unsigned char *b = malloc(sizeof k_fdkseac);
    assert(b != NULL);
    memcpy(b, k_fdkseac, sizeof k_fdkseac);
    return b;
}

/* Finds a table record in the crafted sfnt; returns its index. */
static int find_table_rec(unsigned char *b, const char *tag) {
    unsigned int n = (unsigned int)b[4] << 8 | b[5];
    for (unsigned int i = 0; i < n; i++) {
        if (memcmp(b + 12 + 16 * i, tag, 4) == 0) {
            return (int)(12 + 16 * i);
        }
    }
    return -1;
}

static unsigned int be32at(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static void put32at(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void test_loader_battery(void) {
    /* numGlyphs (maxp+4) vs CharStrings count: mismatch rejected. */
    unsigned char *b = seac_copy();
    int rec = find_table_rec(b, "maxp");
    assert(rec > 0);
    unsigned int maxp_off = be32at(b + rec + 8);
    b[maxp_off + 4] = 0; /* numGlyphs 5 -> 0... use 6 via 16-bit write */
    b[maxp_off + 4] = 0;
    b[maxp_off + 5] = 6;
    reject_copy("numGlyphs != CharStrings count", b, sizeof k_fdkseac, 0);
    free(b);

    /* CFF table extent beyond the file. */
    b = seac_copy();
    rec = find_table_rec(b, "CFF ");
    assert(rec > 0);
    put32at(b + rec + 12, 100000); /* length past EOF */
    reject_copy("CFF length past EOF", b, sizeof k_fdkseac, 0);
    free(b);

    /* 'OTTO' claiming a CFF2 table that carries CFF1 data. */
    b = seac_copy();
    rec = find_table_rec(b, "CFF ");
    assert(rec > 0);
    memcpy(b + rec, "CFF2", 4);
    reject_copy("CFF2 tag on CFF1 data", b, sizeof k_fdkseac, 0);
    free(b);

    /* Truncation inside the CFF table: the extent check fires. */
    {
        char path[] = "/tmp/fdk_cff_trunc_XXXXXX";
        int fd = mkstemp(path);
        assert(fd >= 0);
        rec = find_table_rec((unsigned char *)k_fdkseac, "CFF ");
        unsigned int cff_off = be32at(k_fdkseac + rec + 8);
        unsigned int cff_len = be32at(k_fdkseac + rec + 12);
        assert(cff_len > 40);
        assert(write(fd, k_fdkseac, cff_off + cff_len / 2) ==
               (ssize_t)(cff_off + cff_len / 2));
        close(fd);
        fdk_font *f = fdk_font_load_face(path, 0, 16);
        assert(f == NULL);
        unlink(path);
        printf("  [ok] truncated CFF table rejected\n");
    }

    /* A real font with its CFF header corrupted (major version). */
    if (g_urw != NULL) {
        FILE *src = fopen(g_urw, "rb");
        assert(src != NULL);
        unsigned char *big = malloc(200000);
        assert(big != NULL);
        size_t n = fread(big, 1, 200000, src);
        fclose(src);
        assert(n > 1000);
        /* find the CFF table record of THIS font */
        unsigned int nt = ((unsigned)big[4] << 8) | big[5];
        unsigned int cff_rec = 0;
        for (unsigned int i = 0; i < nt; i++) {
            if (memcmp(big + 12 + 16 * i, "CFF ", 4) == 0) {
                cff_rec = 12 + 16 * i;
                break;
            }
        }
        assert(cff_rec != 0);
        unsigned int off = be32at(big + cff_rec + 8);
        big[off] = 9; /* bogus major version */
        reject_copy("URW with CFF major=9", big, n, 0);
        free(big);
    }
    printf("[ok] loader battery: malformed CFF faces rejected, no "
           "crashes (ASan guards)\n");
}

/* ---- 6. deterministic mutation fuzz ---------------------------------- */

/* Mutates bytes inside the CFF table region of copies of the crafted
 * font (and of the real URW font when present) with a fixed LCG:
 * every mutant must either load and shape all glyphs without a
 * crash, or be rejected. Sanitizers make every out-of-bounds read a
 * hard failure — this is the module's parse-surface fence. */
static unsigned long fuzz_lcg(unsigned long *s) {
    *s = *s * 6364136223846793005UL + 1442695040888963407UL;
    return (*s >> 33);
}

static void fuzz_one(const unsigned char *base, size_t len, int cff_rec,
                     unsigned long *state, const char *label) {
    unsigned char *b = malloc(len);
    assert(b != NULL);
    memcpy(b, base, len);
    unsigned int off = be32at(b + cff_rec + 8);
    unsigned int tlen = be32at(b + cff_rec + 12);
    for (int k = 0; k < 6; k++) {
        size_t at = off + fuzz_lcg(state) % tlen;
        b[at] = (unsigned char)(fuzz_lcg(state) & 0xFF);
    }
    char path[] = "/tmp/fdk_cff_fuzz_XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, b, len) == (ssize_t)len);
    close(fd);
    fdk_font *f = fdk_font_load_face(path, 0, 24);
    if (f != NULL) {
        if (f->cff != NULL) {
            int n = fdk__cff_glyph_count(f->cff);
            for (int g = 0; g < n && g < 64; g++) {
                stbtt_vertex *v = NULL;
                (void)fdk__cff_glyph_shape(f->cff, g, &v);
                free(v);
                int x0, y0, x1, y1;
                (void)fdk__cff_glyph_box(f->cff, g, &x0, &y0, &x1, &y1);
            }
        }
        fdk_font_destroy(f);
    }
    unlink(path);
    free(b);
    (void)label;
}

static void test_fuzz(void) {
    unsigned long state = 0xCFF00FFEEDUL;
    for (int i = 0; i < 250; i++) {
        int cff_rec = find_table_rec((unsigned char *)k_fdkseac, "CFF ");
        fuzz_one(k_fdkseac, sizeof k_fdkseac, cff_rec, &state, "crafted");
    }
    printf("[ok] fuzz: 250 crafted-font mutants parsed without a "
           "crash\n");

    if (g_urw != NULL) {
        FILE *src = fopen(g_urw, "rb");
        assert(src != NULL);
        unsigned char *big = malloc(200000);
        assert(big != NULL);
        size_t n = fread(big, 1, 200000, src);
        fclose(src);
        assert(n > 1000);
        int cff_rec = find_table_rec(big, "CFF ");
        assert(cff_rec > 0);
        for (int i = 0; i < 120; i++) {
            fuzz_one(big, n, cff_rec, &state, "urw");
        }
        free(big);
        printf("[ok] fuzz: 120 URW-font mutants parsed without a "
               "crash\n");
    }
}

/* ---- main ------------------------------------------------------------- */

int main(void) {
    find_fonts();

    /* The crafted font is self-contained: its legs ALWAYS run. The
     * real-font legs (agreement sweep, rendering, fuzz) each skip
     * honestly on a machine with no CFF font — find_fonts already
     * printed the notice. */
    write_seac_font();
    test_crafted_load();
    test_crafted_geometry();
    test_vertex_agreement();
    test_real_render();
    test_loader_battery();
    test_fuzz();
    unlink(k_seac_path);

    printf("all CFF tests passed\n");
    return 0;
}
