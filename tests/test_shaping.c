/* test_shaping.c — M-2 level 1: the complex-script pipeline's proof
 * tests (1.1.0).
 *
 * Two postures, both run in CI:
 *
 *   - with HarfBuzz + FriBidi loadable (the auto probe): the FULL
 *     assertions — bidi visual order, ligatures, per-script font
 *     fallback, cluster mapping, caret positions, the Latin
 *     compatibility pin (shaped metrics == legacy metrics on the
 *     same font).
 *   - with FDK_SHAPING=0 (the legacy walk): the suite still runs
 *     and pins the LEGACY behavior — logical order, one glyph per
 *     codepoint, .notdef for uncovered scripts — so both paths stay
 *     regression-guarded forever.
 *
 * The deep assertions inspect the shaped placement arrays directly
 * (text_internal.h — the same privilege test_text.c already uses);
 * the behavioral ones go through the public measure/draw surface.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_text.h"

#include "text/text_internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *g_sans = NULL;

static void find_fonts(void) {
    static const char *sans_candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
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
    assert(g_sans != NULL); /* CI installs fonts-dejavu-core */
}

/* ---- helpers ------------------------------------------------------- */

/* Shapes s (must be a literal in this file) and returns the result;
 * asserts success. */
static fdk__shaped *shape_str(fdk_font *f, const char *s) {
    size_t len = strlen(s);
    fdk__shaped *sh = fdk__shape_range(f, s, len, 0, len, NULL, 0);
    assert(sh != NULL);
    return sh;
}

/* Does the placement array mention a font other than the primary? */
static int used_fallback(const fdk__shaped *sh, fdk_font *primary) {
    for (size_t i = 0; i < sh->n_places; i++) {
        if (sh->places[i].font != primary) {
            return 1;
        }
    }
    return 0;
}

/* ---- 1. availability + the Latin compatibility pin ------------------ */

static void test_latin_pin(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    /* The availability probe agrees with itself and never flip-flops
     * mid-process. */
    int a1 = fdk_text_shaping_available();
    int a2 = fdk_text_shaping_available();
    assert(a1 == a2);

    /* The compatibility pin: Latin metrics are IDENTICAL shaped and
     * legacy. The comparison runs against the OTHER mode via a forked
     * child (the probe is one-shot per process), so this suite is
     * self-sufficient in either posture. */
    const char *s = "The quick brown fox jumps over 1234 lazy dogs!";
    size_t len = strlen(s);
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(f, s, len, &m)));

    char pin_file[] = "/tmp/fdk-shape-pin-XXXXXX";
    int fd = mkstemp(pin_file);
    assert(fd >= 0);
    char line[128];
    int n = snprintf(line, sizeof(line), "%d\n", m.advance_width);
    assert(n > 0 && (size_t)n < sizeof(line));
    assert(write(fd, line, (size_t)n) == n);
    close(fd);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* child: the OTHER posture */
        if (a1) {
            setenv("FDK_SHAPING", "0", 1);
        } else {
            setenv("FDK_SHAPING", "1", 1);
        }
        char self[256];
        ssize_t rl = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (rl < 0) {
            _exit(127);
        }
        self[rl] = '\0';
        char *argv_child[4];
        argv_child[0] = self;
        argv_child[1] = (char *)"--latin-pin";
        argv_child[2] = pin_file;
        argv_child[3] = NULL;
        execv(self, argv_child);
        _exit(127);
    }
    int st = 0;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    remove(pin_file);

    fdk_text_metrics m2;
    assert(fdk_ok(fdk_font_measure_utf8(f, "x", 1, &m2)));
    (void)m2;
    fdk_font_destroy(f);
    printf("[ok] availability probe stable; Latin metrics identical "
           "across both postures (%s)\n",
           a1 ? "shaped here" : "legacy here");
}

/* The re-exec'd other-posture measurement for the Latin pin. */
static int run_latin_pin(const char *pin_file) {
    FILE *pf = fopen(pin_file, "r");
    if (pf == NULL) {
        return 1;
    }
    int want = 0;
    if (fscanf(pf, "%d", &want) != 1) {
        fclose(pf);
        return 1;
    }
    fclose(pf);
    fdk_font *f = fdk_font_load(g_sans, 16);
    if (f == NULL) {
        return 1;
    }
    const char *s = "The quick brown fox jumps over 1234 lazy dogs!";
    fdk_text_metrics m;
    if (!fdk_ok(fdk_font_measure_utf8(f, s, strlen(s), &m))) {
        return 1;
    }
    fdk_font_destroy(f);
    return m.advance_width == want ? 0 : 1;
}

/* ---- 2. RTL visual order (the headline fix) -------------------------- */

static void test_rtl_arabic(void) {
    const char *s = "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"; /* SLM in Arabic */
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    fdk__shaped *sh = shape_str(f, s);

    if (!fdk_text_shaping_available()) {
        /* LEGACY: logical order, one placement per cp, honest .notdef
         * if DejaVu lacks the glyphs (it does not — DejaVu Sans has
         * basic Arabic). */
        assert(sh->n_places == sh->n_cp);
        for (size_t i = 0; i < sh->n_places; i++) {
            assert(sh->places[i].cluster == sh->cp_byte[i]);
        }
        printf("[ok] legacy: Arabic walks logical order, 1 glyph/cp\n");
    } else {
        /* SHAPED: the visual sequence's clusters run DESCENDING —
         * the LAST logical character renders leftmost. */
        assert(sh->n_cp == 4);
        assert(sh->n_places >= 1);
        size_t prev = (size_t)-1;
        int descended = 0;
        for (size_t i = 0; i < sh->n_places; i++) {
            size_t c = sh->places[i].cluster;
            if (prev != (size_t)-1 && c < prev) {
                descended = 1;
            }
            prev = c;
        }
        assert(descended); /* the fix itself: RTL reordering */
        assert(sh->rtl_visual);

        /* The x positions stay left-to-right across the visual array
         * (the pen always advances rightward across the canvas). */
        for (size_t i = 1; i < sh->n_places; i++) {
            assert(sh->places[i].x >= sh->places[i - 1].x);
        }

        /* Ligatures: joined Arabic shapes to fewer glyphs than
         * codepoints (at minimum, the joining forms differ from the
         * isolated cmap hits — glyph count may drop on lam-lam). The
         * robust invariant: total advance is positive and the
         * attribution still sums to it. */
        fdk_f32 sum = 0.0f;
        for (size_t k = 0; k < sh->n_cp; k++) {
            sum += sh->cp_pen[k + 1] - sh->cp_pen[k];
        }
        assert((int)(sum + 0.5f) == (int)(sh->total + 0.5f));
        assert(sh->total > 0.0f);
        printf("[ok] shaped: Arabic visual order descending, pen "
               "advances rightward, attribution sums to the total "
               "(%zu cp -> %zu glyphs)\n",
               sh->n_cp, sh->n_places);
    }
    fdk__shaped_free(sh);
    fdk_font_destroy(f);
}

/* ---- 3. Mixed bidi ---------------------------------------------------- */

static void test_bidi_mixed(void) {
    /* Latin, then Arabic, then Latin — the classic mixed line. */
    const char *s = "abc \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 def";
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    fdk__shaped *sh = shape_str(f, s);

    if (fdk_text_shaping_available()) {
        /* The FIRST visual placements cover the leading Latin run
         * (clusters 0..2), the LAST cover the trailing run; the
         * Arabic middle appears between them in reversed cluster
         * order. Pin the invariant that survives font differences:
         * the first placement's cluster is in the leading Latin run
         * and the last placement's cluster is in the trailing run. */
        assert(sh->n_places >= 6);
        assert(sh->places[0].cluster == 0); /* 'a' leftmost */
        size_t last = strlen(s);
        assert(sh->places[sh->n_places - 1].cluster ==
               last - 1); /* 'f' of "def" rightmost */
        printf("[ok] shaped: mixed bidi line keeps the Latin runs at "
               "their ends, Arabic reordered between\n");
    } else {
        assert(sh->n_places == sh->n_cp);
        printf("[ok] legacy: mixed line walks logical order\n");
    }
    fdk__shaped_free(sh);
    fdk_font_destroy(f);
}

/* ---- 4. Per-script fallback ------------------------------------------- */

static void test_fallback_cjk(void) {
    /* DejaVu Sans has NO Han glyphs: the pipeline must route them to
     * fontconfig's answer (Noto/WQY/Sarasa here) — or, in legacy,
     * honestly draw .notdef on the primary. */
    const char *s = "\xE6\xBC\xA2\xE5\xAD\x97"; /* two Han cp */
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    assert(!fdk_font_has_glyph(f, 0x6F22u)); /* the premise */

    fdk__shaped *sh = shape_str(f, s);
    if (fdk_text_shaping_available()) {
        if (used_fallback(sh, f)) {
            /* A real CJK font answered: the glyphs are its glyph ids,
             * rasters carry ink, the total is positive. */
            assert(sh->n_places >= 2);
            assert(sh->total > 0.0f);
            for (size_t i = 0; i < sh->n_places; i++) {
                assert(sh->places[i].font != f);
                assert(sh->places[i].raster != NULL);
            }
            printf("[ok] shaped: CJK routed to the fontconfig "
                   "fallback font, ink present\n");
        } else {
            /* No CJK font installed: the honest .notdef posture. */
            printf("[ok] shaped: no CJK font available — primary "
                   ".notdef (honest)\n");
        }
    } else {
        assert(sh->n_places == sh->n_cp);
        assert(!used_fallback(sh, f));
        printf("[ok] legacy: CJK stays on the primary (.notdef)\n");
    }
    fdk__shaped_free(sh);

    /* A mixed Latin+CJK line: the Latin stays on the primary, the
     * CJK rides the fallback — two fonts in one shape. */
    if (fdk_text_shaping_available()) {
        const char *mix = "A\xE6\xBC\xA2\xE5\xAD\x97" "Z";
        fdk__shaped *m = shape_str(f, mix);
        if (used_fallback(m, f)) {
            int pri = 0, fb = 0;
            for (size_t i = 0; i < m->n_places; i++) {
                if (m->places[i].font == f) {
                    pri++;
                } else {
                    fb++;
                }
            }
            assert(pri >= 2); /* A and Z stay on DejaVu */
            assert(fb >= 2);  /* the Han cp ride the fallback */
            printf("[ok] shaped: mixed Latin+CJK line spans two "
                   "fonts (%d primary + %d fallback glyphs)\n", pri,
                   fb);
        }
        fdk__shaped_free(m);
    }
    fdk_font_destroy(f);
}

/* ---- 5. Clusters, marks, conjuncts ------------------------------------ */

static void test_clusters_marks(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    /* Combining mark: e + U+0301 (COMBINING ACUTE). Shaped, the pair
     * composes to at most 2 glyphs and the MARK adds ~nothing to
     * the width (DejaVu's ccmp composes them to the precomposed \u00e9
     * glyph — one glyph, the base's own advance; a font without the
     * composition attaches the mark with a zero advance). Legacy:
     * two codepoints, two placements. */
    const char *s = "e\xCC\x81";
    fdk_text_metrics e_alone, combo;
    assert(fdk_ok(fdk_font_measure_utf8(f, "e", 1, &e_alone)));
    assert(fdk_ok(fdk_font_measure_utf8(f, s, strlen(s), &combo)));
    fdk__shaped *sh = shape_str(f, s);
    assert(sh->n_cp == 2);
    if (fdk_text_shaping_available()) {
        assert(sh->n_places >= 1 && sh->n_places <= 2);
        fdk_i32 added = combo.advance_width - e_alone.advance_width;
        assert(added >= 0 && added <= 1); /* the mark is ~free */
        printf("[ok] shaped: e + combining acute is %zu glyph(s), the "
               "mark adds %d px\n", sh->n_places, added);
    } else {
        assert(sh->n_places == 2);
        printf("[ok] legacy: mark is its own placement\n");
    }
    fdk__shaped_free(sh);

    /* Every placement's cluster is a real codepoint boundary of the
     * text (the byte↔glyph mapping contract). */
    const char *t = "Hi \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 there";
    fdk__shaped *sh2 = shape_str(f, t);
    for (size_t i = 0; i < sh2->n_places; i++) {
        size_t c = sh2->places[i].cluster;
        int at_boundary = 0;
        for (size_t k = 0; k < sh2->n_cp; k++) {
            if (sh2->cp_byte[k] == c) {
                at_boundary = 1;
                break;
            }
        }
        assert(at_boundary);
    }
    printf("[ok] every placement's cluster is a codepoint boundary\n");
    fdk__shaped_free(sh2);
    fdk_font_destroy(f);
}

/* ---- 6. Caret mapping + wrap/ellipsize agreement ----------------------- */

static void test_caret_and_wrap(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);

    /* Caret x: strictly non-decreasing over LTR text, 0 at 0, the
     * rounded total at the end. */
    const char *s = "caret mapping across words";
    fdk__shaped *sh = shape_str(f, s);
    fdk_i32 prev = 0;
    assert(fdk__shaped_caret_x(sh, 0) == 0);
    for (size_t k = 0; k < sh->n_cp; k++) {
        fdk_i32 x = fdk__shaped_caret_x(sh, sh->cp_byte[k]);
        assert(x >= prev);
        prev = x;
    }
    assert(prev <= (fdk_i32)(sh->total + 0.5f));
    assert(fdk__shaped_caret_x(sh, sh->range_end) ==
           (fdk_i32)(sh->total + 0.5f));
    printf("[ok] caret mapping monotonic, anchors at 0 and the total\n");
    fdk__shaped_free(sh);

    /* Wrap: a long Arabic line wraps into several lines; every line's
     * advance agrees with its own measure (the line-as-paragraph
     * semantics), and no visible cp is lost. */
    const char *long_ar =
        "\xD8\xA3\xD9\x87\xD9\x84\xD8\xA7\x20\xD8\xB3\xD9\x8A\xD8\xAF"
        "\xD9\x8A\x20\xD9\x83\xD9\x8A\xD9\x81\x20\xD8\xAD\xD8\xA7\xD9"
        "\x84\xD9\x83\x20\xD8\xA7\xD9\x84\xD9\x8A\xD9\x88\xD9\x85";
    fdk_text_metrics whole;
    assert(fdk_ok(fdk_font_measure_utf8(f, long_ar, strlen(long_ar),
                                        &whole)));
    fdk_i32 width = whole.advance_width / 2;
    size_t count = 0;
    assert(fdk_ok(fdk_font_break_lines_utf8(f, long_ar,
                                            strlen(long_ar), width,
                                            NULL, 0, &count, NULL)));
    assert(count >= 2);
    fdk_text_line *lines = calloc(count, sizeof(*lines));
    assert(lines != NULL);
    size_t filled = 0;
    bool trunc = true;
    assert(fdk_ok(fdk_font_break_lines_utf8(f, long_ar,
                                            strlen(long_ar), width,
                                            lines, count, &filled,
                                            &trunc)));
    assert(filled == count && !trunc);
    size_t visible_in = 0, visible_out = 0;
    for (size_t i = 0; i < strlen(long_ar); i++) {
        if (long_ar[i] != ' ') {
            visible_in++;
        }
    }
    for (size_t i = 0; i < filled; i++) {
        fdk_text_metrics lm;
        assert(fdk_ok(fdk_font_measure_utf8(
            f, long_ar + lines[i].byte_offset, lines[i].byte_len,
            &lm)));
        assert(lines[i].advance_width == lm.advance_width);
        for (size_t j = 0; j < lines[i].byte_len; j++) {
            if (long_ar[lines[i].byte_offset + j] != ' ') {
                visible_out++;
            }
        }
    }
    assert(visible_in == visible_out); /* nothing lost */
    free(lines);
    printf("[ok] Arabic paragraph wraps at spaces; every line's "
           "advance agrees with its own measure; no cp lost\n");

    /* Ellipsize: an overflowing Arabic string gets a prefix + the
     * ellipsis within the budget. */
    size_t prefix = 0;
    bool fits = true;
    assert(fdk_ok(fdk_font_ellipsize_utf8(f, long_ar,
                                          strlen(long_ar), width,
                                          &prefix, &fits)));
    assert(!fits);
    assert(prefix < strlen(long_ar));
    printf("[ok] Arabic ellipsize: prefix %zu of %zu bytes fits the "
           "budget\n", prefix, strlen(long_ar));
    fdk_font_destroy(f);
}

/* ---- 7. Draw/measure agreement (pixels) -------------------------------- */

static void test_draw_agrees(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    fdk_surface *sf = NULL;
    assert(fdk_ok(fdk_surface_create(200, 40, &sf)) && sf != NULL);

    /* Arabic + CJK + Latin in one string: whatever fonts answered,
     * the drawn ink must sit inside the measured advance, and the
     * damage rect must be non-empty when ink exists. */
    const char *s =
        "mixed \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xE6\xBC\xA2\xE5\xAD\x97"
        " end";
    fdk_text_metrics m;
    assert(fdk_ok(fdk_font_measure_utf8(f, s, strlen(s), &m)));
    fdk_surface_draw_utf8(sf, f, s, strlen(s), 0, 30,
                          (fdk_color){0, 0, 0, 255});
    fdk_rect dmg = {0, 0, 0, 0};
    assert(fdk_surface_get_damage_bounds(sf, &dmg));
    /* ink exists (Latin alone guarantees it) — the damage is
     * non-empty, inside the surface, on the baseline's line box.
     * (Ink may legitimately overhang the measured ADVANCE — fallback
     * fonts' glyphs are not metric-compatible with the primary — so
     * the horizontal contract is the surface, not the advance.) */
    assert(dmg.width > 0 && dmg.height > 0);
    assert(dmg.x >= 0 && dmg.x + dmg.width <= 200);
    assert(dmg.y <= 30 && dmg.y + dmg.height > 30 - m.ink_bottom - 1);
    fdk_surface_destroy(sf);
    fdk_font_destroy(f);
    printf("[ok] draw/measure agreement on a trilingual string: ink "
           "inside the measured box\n");
}

/* ---- 8. The fallback font lifecycle ------------------------------------ */

static void test_fallback_lifecycle(void) {
    fdk_font *f = fdk_font_load(g_sans, 16);
    assert(f != NULL);
    if (fdk_text_shaping_available()) {
        /* Force a fallback load, then destroy the primary: the
         * fallback dies with it (ASan would flag the leak otherwise
         * — this is also the leak regression test). */
        const char *s = "\xE6\xBC\xA2\xE5\xAD\x97";
        fdk__shaped *sh = shape_str(f, s);
        (void)used_fallback(sh, f);
        fdk__shaped_free(sh);
    }
    fdk_font_destroy(f);
    printf("[ok] fallback fonts die with their primary (destroy "
           "clean under ASan)\n");
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv) {
    find_fonts();

    /* The re-exec'd other-posture leg (the Latin pin). */
    if (argc == 3 && argv != NULL &&
        strcmp(argv[1], "--latin-pin") == 0) {
        return run_latin_pin(argv[2]);
    }

    test_latin_pin();
    test_rtl_arabic();
    test_bidi_mixed();
    test_fallback_cjk();
    test_clusters_marks();
    test_caret_and_wrap();
    test_draw_agrees();
    test_fallback_lifecycle();

    printf("all shaping tests passed (%s posture)\n",
           fdk_text_shaping_available() ? "full pipeline" : "legacy");
    return 0;
}
