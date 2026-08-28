/* bench.c — the Phase 11 performance baseline harness.
 *
 * NOT part of `make test` (numbers are machine-dependent); run with
 * `make bench`. Measures the hot paths the roadmap's Phase 11
 * profiling pass cares about, at workloads a real UI produces:
 *
 *   tree      widget create/destroy churn (1000-widget trees)
 *   layout    measure+arrange sweeps over a realistic hierarchy
 *   paint     full repaint vs 1%-damage repaint on an offscreen
 *             surface (the damage-tracker's real job)
 *   text      glyph measurement, paragraph line-breaking
 *   events    synthetic pointer motion through a deep tree
 *   theme     token resolution at paint time + full-tree switch
 *   i18n      number/currency formatting, plural rules, catalog
 *             lookups
 *
 * Output: one line per benchmark — ops/sec and ns/op — plus the
 * machine note. Docs/performance.md records the baseline from the
 * reference machine; the numbers here exist to catch REGRESSIONS
 * (an order-of-magnitude drop), not to chase single-digit percents
 * (the project principle against premature optimization).
 *
 * Uses no display: offscreen surfaces, standalone roots, the C
 * locale-free formatters. Clock is CLOCK_MONOTONIC.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_i18n.h"
#include "fdk/fdk_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static fdk_font *g_font = NULL;

/* ---- a realistic tree: boxes with labels and buttons ---- */

/* Builds a tree the layout engine and painter actually work on:
 * alternating H/V BOXES (measure+arrange cascade through the whole
 * hierarchy) with LABEL leaves (font measurement in the measure
 * pass, text+background ink in the paint pass). This is the shape a
 * real settings-dialog page has, not a pile of inert rectangles. */
static fdk_widget *bench_root(void) {
    fdk_widget *root = NULL;
    (void)fdk_box_create(NULL, FDK_VERTICAL, &root);
    fdk_widget_set_bounds(root, (fdk_rect){0, 0, 800, 600});
    return root;
}

static fdk_widget *build_tree(fdk_widget *parent, int depth,
                              int breadth) {
    if (depth == 0) {
        fdk_widget *label = NULL;
        (void)fdk_label_create(parent, g_font, "benchmark label",
                               &label);
        fdk_widget_set_background(label,
                                  (fdk_color){0.20f, 0.22f, 0.26f,
                                              1.0f});
        return parent;
    }
    for (int i = 0; i < breadth; i++) {
        fdk_widget *box = NULL;
        (void)fdk_box_create(parent,
                             (depth % 2 == 0)
                                 ? FDK_VERTICAL
                                 : FDK_HORIZONTAL,
                             &box);
        fdk_widget_set_expand(box, true, true);
        fdk_widget_set_background(box,
                                  (fdk_color){0.12f, 0.13f, 0.16f,
                                              1.0f});
        build_tree(box, depth - 1, breadth);
    }
    return parent;
}

static size_t count_widgets(fdk_widget *w) {
    size_t n = 1;
    for (size_t i = 0; i < fdk_widget_child_count(w); i++) {
        n += count_widgets(fdk_widget_child_at(w, i));
    }
    return n;
}

static void bench_tree_churn(void) {
    const int rounds = 40;
    double t0 = now_ms();
    size_t total = 0;
    for (int r = 0; r < rounds; r++) {
        fdk_widget *root = bench_root();
        build_tree(root, 3, 6); /* 1 + 6 + 36 + 216 + labels */
        total += count_widgets(root);
        fdk_widget_destroy(root);
    }
    double dt = now_ms() - t0;
    double ops = (double)rounds;
    printf("tree-churn      %8.0f trees/s   %10.0f ns/tree "
           "(%zu widgets each, create+destroy)\n",
           ops / (dt / 1000.0), dt * 1e6 / ops, total / (size_t)rounds);

    /* The same build inside fdk_layout_begin/end_batch: the
     * Phase 11 batching optimization (one relayout per dirty chain
     * instead of one per create). */
    t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        fdk_widget *root = bench_root();
        fdk_layout_begin_batch();
        build_tree(root, 3, 6);
        fdk_layout_end_batch();
        fdk_widget_destroy(root);
    }
    dt = now_ms() - t0;
    printf("tree-churn-batch %7.0f trees/s   %10.0f ns/tree "
           "(same build, layout batched)\n",
           ops / (dt / 1000.0), dt * 1e6 / ops);
}

/* Layout: box measure+arrange over the same tree shape. */
static void bench_layout(void) {
    fdk_widget *root = bench_root();
    build_tree(root, 3, 6);
    fdk_widget_arrange(root, (fdk_rect){0, 0, 800, 600});
    const int rounds = 200;
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        fdk_rect full = {0, 0, 800, 600};
        fdk_widget_arrange(root, full);
    }
    double dt = now_ms() - t0;
    printf("layout-sweep    %8.0f sweeps/s  %10.0f ns/sweep "
           "(measure+arrange, %zu widgets)\n",
           rounds / (dt / 1000.0), dt * 1e6 / rounds,
           count_widgets(root));
    fdk_widget_destroy(root);
}

/* Paint: full damage vs a tiny damage box (the 1% case). */
static void bench_paint(void) {
    fdk_widget *root = bench_root();
    build_tree(root, 3, 6);
    fdk_widget_arrange(root, (fdk_rect){0, 0, 800, 600});
    fdk_surface *s = NULL;
    (void)fdk_surface_create(800, 600, &s);

    const int rounds = 100;
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        fdk_widget_invalidate_all(root);
        fdk_widget_tree_paint(root, s);
    }
    double dt = now_ms() - t0;
    printf("paint-full      %8.0f frames/s  %10.0f ns/frame "
           "(full-tree repaint)\n",
           rounds / (dt / 1000.0), dt * 1e6 / rounds);

    /* One label's bounds as the damage source. */
    fdk_widget *leaf = root;
    while (fdk_widget_child_count(leaf) > 0) {
        leaf = fdk_widget_child_at(leaf, 0);
    }
    const int small_rounds = 2000;
    t0 = now_ms();
    for (int r = 0; r < small_rounds; r++) {
        fdk_widget_invalidate(leaf);
        fdk_widget_tree_paint(root, s);
    }
    dt = now_ms() - t0;
    printf("paint-damage    %8.0f frames/s  %10.0f ns/frame "
           "(one-widget damage repaint)\n",
           small_rounds / (dt / 1000.0),
           dt * 1e6 / small_rounds);

    fdk_surface_destroy(s);
    fdk_widget_destroy(root);
}

/* Text: measurement + line breaking over a paragraph. */
static void bench_text(void) {
    if (g_font == NULL) {
        printf("text-*          SKIPPED (no system font)\n");
        return;
    }
    const char *para =
        "The quick brown fox jumps over the lazy dog. "
        "Pack my box with five dozen liquor jugs. "
        "How vexingly quick daft zebras jump! ";
    const int rounds = 2000;
    double t0 = now_ms();
    fdk_text_metrics tm;
    memset(&tm, 0, sizeof(tm));
    for (int r = 0; r < rounds; r++) {
        fdk_font_measure_utf8(g_font, para, strlen(para), &tm);
    }
    double dt = now_ms() - t0;
    printf("text-measure    %8.0f ops/s    %10.0f ns/op (%zu-byte "
           "paragraph)\n",
           rounds / (dt / 1000.0), dt * 1e6 / rounds,
           strlen(para));

    const int br_rounds = 500;
    t0 = now_ms();
    size_t lines = 0;
    for (int r = 0; r < br_rounds; r++) {
        size_t n = 0;
        (void)fdk_font_break_lines_utf8(g_font, para,
                                        strlen(para), 300, NULL, 0,
                                        &n, NULL);
        lines = n;
    }
    dt = now_ms() - t0;
    printf("text-break      %8.0f ops/s    %10.0f ns/op "
           "(count pass, %zu lines @300px)\n",
           br_rounds / (dt / 1000.0), dt * 1e6 / br_rounds, lines);
}

/* Events: pointer motion through the tree (hit-test + hover
 * synthesis + bubbling). */
static void bench_events(void) {
    fdk_widget *root = bench_root();
    build_tree(root, 3, 6);
    fdk_widget_arrange(root, (fdk_rect){0, 0, 800, 600});
    const int rounds = 20000;
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        fdk_event_data ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = FDK_EVENT_POINTER_MOTION;
        ev.pointer.position.x = (fdk_f32)(r % 800);
        ev.pointer.position.y = (fdk_f32)((r * 7) % 600);
        (void)fdk_widget_tree_handle_event(root, &ev);
    }
    double dt = now_ms() - t0;
    printf("event-motion    %8.0f events/s  %10.0f ns/event "
           "(hit-test + hover + bubble)\n",
           rounds / (dt / 1000.0), dt * 1e6 / rounds);
    fdk_widget_destroy(root);
}

/* Theme: token resolution (paint-time lookups) + switch cost. */
static void bench_theme(void) {
    fdk_widget *root = bench_root();
    build_tree(root, 3, 6);
    fdk_widget_arrange(root, (fdk_rect){0, 0, 800, 600});
    fdk_theme *t = fdk_theme_create_default();
    const int rounds = 500;
    double t0 = now_ms();
    for (int r = 0; r < rounds; r++) {
        /* Switch back and forth: each switch invalidates every live
         * tree (the documented cost). */
        fdk_theme_set_default((r % 2 == 0) ? t : NULL);
    }
    double dt = now_ms() - t0;
    printf("theme-switch    %8.0f swaps/s   %10.0f ns/swap "
           "(invalidate ALL live trees)\n",
           rounds / (dt / 1000.0), dt * 1e6 / rounds);
    fdk_theme_set_default(NULL);
    fdk_theme_destroy(t);
    fdk_widget_destroy(root);
}

/* i18n: formatters + plural + catalog lookup. */
static void bench_i18n(void) {
    fdk_locale de;
    if (!fdk_ok(fdk_locale_parse("de", &de))) {
        printf("i18n-*          SKIPPED (locale parse failed)\n");
        return;
    }
    char buf[96];
    const int n_rounds = 200000;
    double t0 = now_ms();
    for (int r = 0; r < n_rounds; r++) {
        (void)fdk_format_int(buf, sizeof(buf), &de, (fdk_i64)r,
                             NULL);
    }
    double dt = now_ms() - t0;
    printf("i18n-fmt-int    %8.0f ops/s    %10.0f ns/op\n",
           n_rounds / (dt / 1000.0), dt * 1e6 / n_rounds);

    const int d_rounds = 100000;
    t0 = now_ms();
    for (int r = 0; r < d_rounds; r++) {
        (void)fdk_format_double(buf, sizeof(buf), &de,
                                (fdk_f64)r * 1.5, 2, NULL);
    }
    dt = now_ms() - t0;
    printf("i18n-fmt-double %8.0f ops/s    %10.0f ns/op\n",
           d_rounds / (dt / 1000.0), dt * 1e6 / d_rounds);

    const int p_rounds = 500000;
    t0 = now_ms();
    volatile fdk_plural_category cat = FDK_PLURAL_OTHER;
    for (int r = 0; r < p_rounds; r++) {
        cat = fdk_plural_category_int(&de, (fdk_i64)r);
    }
    (void)cat;
    dt = now_ms() - t0;
    printf("i18n-plural     %8.0f ops/s    %10.0f ns/op "
           "(de one/other)\n",
           p_rounds / (dt / 1000.0), dt * 1e6 / p_rounds);

    const int c_rounds = 500000;
    t0 = now_ms();
    const char *got = NULL;
    for (int r = 0; r < c_rounds; r++) {
        got = fdk_catalog_get(NULL, "key");
    }
    (void)got;
    dt = now_ms() - t0;
    printf("i18n-catalog    %8.0f ops/s    %10.0f ns/op "
           "(NULL-cat miss path)\n",
           c_rounds / (dt / 1000.0), dt * 1e6 / c_rounds);
}

int main(void) {
    g_font = fdk_font_load_system_default(16);
    printf("== FDK bench (FDK %s, %s) ==\n",
           fdk_get_version_string(),
           g_font != NULL ? "system font" : "no font");
    bench_tree_churn();
    bench_layout();
    bench_paint();
    bench_text();
    bench_events();
    bench_theme();
    bench_i18n();
    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    printf("== baseline: docs/performance.md ==\n");
    return 0;
}
