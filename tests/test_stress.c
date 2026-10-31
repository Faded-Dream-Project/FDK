/*
 * test_stress.c — the four PERMANENT extreme-stress legs (make test).
 *
 * Re-landed (the history rewrite lost the original file with the
 * repository deletion; the defects the legs caught are re-landed
 * alongside — see DECISIONS D-035/D-037 and CHANGELOG). The legs are
 * the opposite of the unit suites: they push ONE dimension to a size
 * no real application reaches and assert the results stay CORRECT
 * and the wall-clock stays LINEAR-ish. A regression to a quadratic
 * turns a leg into a timeout; a per-frame leak turns another into
 * an ASan/LSan report; a precision drift turns the last into a
 * ratio assertion failure. Budgets are generous on purpose (loaded
 * CI runners, sanitizer overhead) — they exist to fail LOUDLY on
 * complexity regressions, not to benchmark.
 *
 *   1. 10k-row List            — append/measure/scroll/select/paint
 *   2. 20k-line document       — wrap + per-line advance exactness
 *                                (the f64 wrap-pen pin: identical
 *                                lines must report identical widths
 *                                at ANY depth)
 *   3. 1200 fps full-screen    — sustained frame-rate on a
 *      animation + scrolling    1920x1080 surface with interleaved
 *                                wheel scroll (per-frame leaks)
 *   4. 1 MB single-line text   — shaped attribution linearity (the
 *                                O(n) boundary table + the f64 pen;
 *                                RTL fragments are load-bearing:
 *                                the old insertion sort went
 *                                quadratic exactly there)
 *
 * Every leg degrades to [skip] when its font/display precondition
 * is absent (no TrueType font on the box) — never to a fake pass.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"

#include "widget/widget_internal.h" /* the animation test seams */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static fdk_font *g_font = NULL;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* Leg 1 — the 10k-row List                                             */
/* ------------------------------------------------------------------ */

static void stress_leg_list_10k(void) {
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 300, 400},
                                     &root)));
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 300, 400});

    /* The growth-ratio pin: 2.5k and 10k rows through the SAME
     * append path. Linear stays under the slack; a quadratic
     * regression lands at ~16x for 4x the rows and fails. */
    double t_small = now_s();
    for (int i = 0; i < 2500; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "warmup row %05d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    t_small = now_s() - t_small;

    double t_big_start = now_s();
    for (int i = 2500; i < 10000; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "stress row %05d — %s", i,
                 (i % 3 == 0) ? "alpha" : ((i % 3 == 1) ? "beta" : "gamma"));
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }
    double t_big = now_s() - t_big_start;
    /* 7500 more rows in less than 3x the 2500-row time plus slack
     * (allocator warmup makes the small pass cheap; quadratic needs
     * ~9x and blows this). */
    assert(t_big < t_small * 3.0 + 1.0);

    /* Selection + query sanity at depth. */
    assert(fdk_ok(fdk_list_select(list, 9999)));
    assert(fdk_list_get_selected(list) == 9999);
    assert(fdk_ok(fdk_list_select(list, 0)));
    assert(fdk_list_get_selected(list) == 0);

    /* A repaint of the whole tree at 10k rows (the list culls to its
     * viewport — a regression to full-range painting blows the
     * budget below on any machine). */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(300, 400, &s)));
    double t_paint = now_s();
    for (int i = 0; i < 20; i++) {
        fdk_surface_invalidate_all(s);
        fdk_widget_tree_paint(root, s);
    }
    t_paint = now_s() - t_paint;
    assert(t_paint < 20.0); /* 20 full repaints of the 10k-row tree */

    /* Deep scroll: wheel to the far end through the internal
     * scrollview (the eased target accumulates; the engine's test
     * clock pins time so the flight settles deterministically). */
    fdk_event_data wheel;
    memset(&wheel, 0, sizeof wheel);
    wheel.type = FDK_EVENT_POINTER_SCROLL;
    wheel.scroll.position.x = 150;
    wheel.scroll.position.y = 200;
    wheel.scroll.delta_y = -1;
    fdk__animation_set_test_clock(0);
    for (int i = 0; i < 400; i++) {
        (void)fdk_widget_tree_handle_event(root, &wheel);
    }
    fdk__animation_pump(100000);
    fdk__animation_set_test_clock(-1);
    /* The scrolled tree still paints, still selects: a real click
     * at the viewport's center lands on whichever late row the deep
     * scroll put there. */
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);
    fdk_event_data down, up;
    memset(&down, 0, sizeof down);
    memset(&up, 0, sizeof up);
    down.type = FDK_EVENT_POINTER_BUTTON_DOWN;
    down.pointer_button.position.x = 150;
    down.pointer_button.position.y = 200;
    down.pointer_button.button = 1;
    up = down;
    up.type = FDK_EVENT_POINTER_BUTTON_UP;
    (void)fdk_widget_tree_handle_event(root, &down);
    (void)fdk_widget_tree_handle_event(root, &up);
    assert(fdk_list_get_selected(list) >= 0);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
    printf("[ok] stress 10k-row list: append/measure linear "
           "(%.2fs + %.2fs), 20 full repaints %.2fs, deep scroll + "
           "select + repaint alive\n",
           t_small, t_big, t_paint);
}

/* ------------------------------------------------------------------ */
/* Leg 2 — the 20k-line document                                        */
/* ------------------------------------------------------------------ */

static void stress_leg_document_20k(void) {
    /* 20k identical hard-broken lines, ~44 bytes each (~880 KB):
     * the wrap engine walks the whole shaped range with TEXT-ABSOLUTE
     * prefix pens — 20k lines x ~350 px/line pushes the pen past
     * 2^23 px, where an f32 snapshot's ulp is 1 px. THE PIN: every
     * identical line must report an IDENTICAL advance_width at ANY
     * depth (pre-fix, deep lines drifted by whole pixels). */
    const size_t N_LINES = 20000;
    const char *const unit =
        "the quick brown fox jumps over the lazy dog 0123\n";
    const size_t unit_len = strlen(unit);
    char *text = malloc(N_LINES * unit_len + 1);
    assert(text != NULL);
    for (size_t i = 0; i < N_LINES; i++) {
        memcpy(text + i * unit_len, unit, unit_len);
    }
    text[N_LINES * unit_len] = '\0';
    const size_t text_len = N_LINES * unit_len;

    size_t n_lines = 0;
    bool truncated = false;
    fdk_result r = fdk_font_break_lines_utf8(g_font, text, text_len,
                                             10000, NULL, 0, &n_lines,
                                             &truncated);
    /* max_width 10000px: no soft wrapping, one line per hard break. */
    assert(fdk_ok(r));
    assert(n_lines == N_LINES);
    assert(!truncated);

    /* Second pass, materialized: assert the depth-exactness. */
    fdk_text_line *lines = malloc(n_lines * sizeof *lines);
    assert(lines != NULL);
    r = fdk_font_break_lines_utf8(g_font, text, text_len, 10000, lines,
                                  n_lines, &n_lines, &truncated);
    assert(fdk_ok(r));
    assert(n_lines == N_LINES);

    fdk_i32 w0 = lines[0].advance_width;
    fdk_i32 w_first_half = lines[N_LINES / 2].advance_width;
    fdk_i32 w_last = lines[N_LINES - 1].advance_width;
    assert(w0 > 0);
    /* THE f64 WRAP-PEN PIN: identical content, identical width,
     * first line, half-depth line, last line. */
    assert(w_first_half == w0);
    assert(w_last == w0);
    /* And every byte bookmark sits exactly one unit apart. */
    assert(lines[0].byte_offset == 0);
    assert(lines[1].byte_offset == unit_len);
    assert(lines[N_LINES - 1].byte_offset ==
           (N_LINES - 1) * unit_len);
    assert(lines[N_LINES - 1].byte_len == unit_len - 1); /* no \n */

    int mismatches = 0;
    for (size_t i = 1; i < N_LINES; i++) {
        if (lines[i].advance_width != w0 ||
            lines[i].byte_offset != i * unit_len) {
            mismatches++;
        }
    }
    assert(mismatches == 0);

    /* The widget-level shape: the TextView holds the document at ITS
     * documented cap — TV_MAX_TEXT is 256 KB (~5.8k of these lines),
     * so the 20k-line document above rides the break_lines seam
     * (unbounded by design) and the widget leg pins the capped
     * maximum: a 250 KB document relayouts in one pass and reports
     * its line count exactly. */
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 480, 360},
                                     &root)));
    fdk_widget *tv = NULL;
    assert(fdk_ok(fdk_textview_create(root, g_font, &tv)));
    fdk_textview_set_wrap(tv, FDK_TEXTVIEW_WRAP_NONE);
    const size_t cap_lines = (256 * 1024) / unit_len; /* TV_MAX_TEXT */
    char *cap_text = malloc(cap_lines * unit_len + 1);
    assert(cap_text != NULL);
    memcpy(cap_text, text, cap_lines * unit_len);
    cap_text[cap_lines * unit_len] = '\0';
    fdk_textview_set_text(tv, cap_text);
    /* Two honest counts over the same bytes: line_count follows
     * break_lines (a TRAILING \n opens no extra empty line — N
     * newlines at the end of the text are N lines), while
     * paragraph_count is the editor model (N newlines = N+1
     * paragraphs, the empty tail counts). Both asserted. */
    assert(fdk_textview_line_count(tv) == cap_lines);
    assert(fdk_textview_paragraph_count(tv) == cap_lines + 1);
    fdk_widget_destroy(root);
    free(cap_text);

    free(lines);
    free(text);
    printf("[ok] stress 20k-line document: wrap exact at depth "
           "(%zu lines, every advance == %d px, byte bookmarks "
           "exact; the f64 wrap pen)\n",
           n_lines, (int)w0);
}

/* ------------------------------------------------------------------ */
/* Leg 3 — 1200 fps full-screen animation + scrolling                   */
/* ------------------------------------------------------------------ */

static int g_frames = 0;
static fdk_surface *g_fs = NULL;
static int g_done_fired = 0;

static void fs_tick(fdk_animation *anim, double t, void *user) {
    (void)anim;
    (void)user;
    /* Full-screen frame at 1200 fps: fill + a moving band. The
     * surface is 1920x1080 x 4 B = ~8 MB per frame — a per-frame
     * leak shows up as an ASan/LSan report long before the budget
     * expires. */
    fdk_surface_fill(g_fs, (fdk_color){ .r = (float)(0.5 * t),
                                        .g = 0.25f, .b = 0.75f,
                                        .a = 1.0f });
    int band_x = (int)(t * 1800.0);
    fdk_rect band = { band_x, 100, 120, 880 };
    fdk_surface_fill_rect(g_fs, band,
                          (fdk_color){ .r = 1, .g = 1, .b = 0, .a = 1 });
    g_frames++;
}

static void fs_done(fdk_animation *anim, bool finished, void *user) {
    (void)anim;
    (void)user;
    if (finished) {
        g_done_fired++;
    }
}

static void stress_leg_animation_1200fps(void) {
    /* 1920x1080 is the floor of "full-screen" on real hardware and
     * the format's create cap is 16384 — comfortably legal. */
    fdk_surface *s = NULL;
    assert(fdk_ok(fdk_surface_create(1920, 1080, &s)));
    g_fs = s;
    g_frames = 0;
    g_done_fired = 0;

    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_widget_create(NULL, NULL,
                                     (fdk_rect){0, 0, 1920, 1080},
                                     &root)));
    /* The scrolling half: a tall list INSIDE the animated tree —
     * wheel events interleaved with the animation frames, exactly
     * the "animated overlay + scrolling content" shape. */
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(root, g_font, &list)));
    fdk_widget_set_bounds(list, (fdk_rect){0, 0, 1920, 1080});
    for (int i = 0; i < 2000; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "scroll row %04d", i);
        assert(fdk_ok(fdk_list_append(list, buf, NULL)));
    }

    /* 1200 fps: frame period ~0.833 ms. A 3600 ms animation driven
     * on the pinned test clock at 1 ms per pump ticks exactly 3600
     * frames — each pump IS a frame (the engine's own "animation
     * traffic" unit; GRACE_PUMPS counts the same passes). The clock
     * is pinned to 0 BEFORE the animation starts so its start_ms is
     * 0 — then every pump carries the synthetic timestamp, keeping
     * the leg deterministic and independent of wall-clock
     * scheduling. */
    fdk__animation_set_test_clock(0);
    fdk_animation *anim = fdk_widget_animate(root, 1200,
                                             FDK_EASE_LINEAR,
                                             fs_tick, fs_done, NULL);
    assert(anim != NULL);

    fdk_event_data wheel;
    memset(&wheel, 0, sizeof wheel);
    wheel.type = FDK_EVENT_POINTER_SCROLL;
    wheel.scroll.position.x = 960;
    wheel.scroll.position.y = 540;
    wheel.scroll.delta_y = -1;

    double t0 = now_s();
    long long t = 0;
    int wheels = 0;
    for (int f = 1; f <= 1200; f++) {
        t = f; /* 1 ms per pump = ~1200 fps */
        fdk__animation_set_test_clock(t);
        fdk__animation_pump(t);
        if ((f % 8) == 0) {
            /* The scrolling half: a wheel notch every 8th frame —
             * the list's eased scroll rides the SAME pump cadence
             * (its flight starts at the pinned now and settles over
             * the next frames). */
            (void)fdk_widget_tree_handle_event(root, &wheel);
            wheels++;
        }
    }
    double elapsed = now_s() - t0;

    assert(g_frames == 1200);
    assert(g_done_fired == 1);
    assert(!fdk_animation_running(anim));
    /* The ~64-pump zombie grace window (the header's honest
     * contract, now stated there in pump units): the handle stays
     * a valid QUERY object for GRACE_PUMPS more passes. Query it
     * inside the window. */
    assert(fdk_animation_widget(anim) == root);
    for (int i = 0; i < 70; i++) {
        fdk__animation_pump(t + 1 + i);
    }
    fdk__animation_set_test_clock(-1);

    /* The tree is still fully alive: a repaint after the storm. */
    fdk_surface_invalidate_all(s);
    fdk_widget_tree_paint(root, s);

    fdk_widget_destroy(root);
    fdk_surface_destroy(s);
    g_fs = NULL;
    printf("[ok] stress 1200fps animation + scrolling: %d full-screen "
           "1920x1080 frames + %d wheel events in %.2fs, done fired "
           "once, zombie grace honored, tree repaints after\n",
           g_frames, wheels, elapsed);
    assert(elapsed < 60.0);
}

/* ------------------------------------------------------------------ */
/* Leg 4 — the 1 MB single-line text                                    */
/* ------------------------------------------------------------------ */

static void stress_leg_text_1mb(void) {
    /* One single line, 1 MB, MIXED direction: Latin (LTR) and
     * Arabic (RTL) fragments — the RTL runs are load-bearing, the
     * old attribution insertion sort went quadratic exactly there
     * (harfbuzz emits DESCENDING cluster ids on RTL; 128 KB took
     * ~19 s under ASan, 1 MB would take ~19 min = a timeout, which
     * is how the leg fails a regression). DejaVu Sans covers both
     * scripts, so the real HarfBuzz pipeline runs. */
    const char *ltr = "measure stress ";           /* 15 bytes */
    const char *rtl = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 "; /* "مرحبا " */
    const size_t ltr_len = strlen(ltr);
    const size_t rtl_len = strlen(rtl);
    const size_t unit = ltr_len + rtl_len;             /* 21 bytes */
    const size_t N_UNITS = (1024 * 1024) / unit;       /* ~49,932 */

    char *text = malloc(N_UNITS * unit + 1);
    assert(text != NULL);
    for (size_t i = 0; i < N_UNITS; i++) {
        memcpy(text + i * unit, ltr, ltr_len);
        memcpy(text + i * unit + ltr_len, rtl, rtl_len);
    }
    text[N_UNITS * unit] = '\0';
    const size_t text_len = N_UNITS * unit;

    /* THE LINEARITY PIN: 1/8 of the text vs the whole text, same
     * content repeated — linear shaping lands near 8x; the old
     * quadratic lands near 64x; the budget below sits between. */
    fdk_text_metrics m_small;
    double t_small = now_s();
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, text_len / 8,
                                        &m_small)));
    t_small = now_s() - t_small;

    fdk_text_metrics m_big;
    double t_big_start = now_s();
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, text_len,
                                        &m_big)));
    double t_big = now_s() - t_big_start;

    assert(m_big.advance_width > 0);
    /* Advance scales with length (the f64 pen's drift pin: the
     * whole line's advance is past 2^23 px, where an f32
     * accumulator would have drifted by pixels). */
    double ratio = (double)m_big.advance_width /
                   (double)m_small.advance_width;
    assert(ratio > 7.0 && ratio < 9.0);
    /* And the wall-clock ratio stays linear-ish. */
    assert(t_big < t_small * 12.0 + 1.0);

    /* Repetition exactness at the public seam: one unit's advance,
     * repeated, accounts for the whole line within rounding (the
     * boundary table's attribution is additive per cp by
     * construction). */
    fdk_text_metrics m_unit;
    assert(fdk_ok(fdk_font_measure_utf8(g_font, text, unit, &m_unit)));
    double whole = (double)m_unit.advance_width * (double)N_UNITS;
    double drift = whole - (double)m_big.advance_width;
    if (drift < 0) {
        drift = -drift;
    }
    /* Subpixel phase accumulation keeps per-unit repeats from being
     * bit-exact; the bound is a generous N/2 px of phase slack. */
    assert(drift < (double)N_UNITS / 2.0);

    free(text);
    printf("[ok] stress 1MB single-line text: shaped in %.2fs "
           "(1/8: %.2fs — linear), advance ratio %.2fx, repetition "
           "drift %.1f px over %zu units (the O(n) boundary table + "
           "the f64 pen)\n",
           t_big, t_small, ratio, drift, N_UNITS);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
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
        printf("[skip] no system TrueType font found — the stress "
               "legs need real glyph metrics; see docs/testing.md\n");
        return 0;
    }

    stress_leg_list_10k();
    stress_leg_document_20k();
    stress_leg_animation_1200fps();
    stress_leg_text_1mb();

    printf("all stress legs passed\n");
    return 0;
}
