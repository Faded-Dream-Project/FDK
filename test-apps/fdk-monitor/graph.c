/* graph.c — fdk-monitor's canvas graphs: a filled area series over
 * a graticule, painted through FDK's surface primitives inside the
 * canvas paint callback (the paint machinery owns clip + damage;
 * the callback only draws). */
#include "monitor.h"

#include <stdio.h>
#include <string.h>

/* the theme tokens the graphs read (fall back to fixed tints when
 * the theme refuses — a monitor must stay readable) */
static fdk_color tok_or(fdk_theme_token t, unsigned char r,
                        unsigned char g, unsigned char b) {
    fdk_color c = fdk_theme_get_color(NULL, t);
    if (c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0) {
        c.r = r;
        c.g = g;
        c.b = b;
        c.a = 255;
    }
    return c;
}

typedef struct graph_style {
    fdk_color line, fill, grid, text;
} graph_style;

static void graph_style_init(graph_style *gs) {
    gs->line = tok_or(FDK_TK_ACCENT, 0x35, 0x84, 0xe4);
    gs->fill = gs->line;
    gs->fill.a = 64; /* translucent area under the line            */
    gs->grid = tok_or(FDK_TK_CONTROL_BORDER, 0x3a, 0x3a, 0x40);
    gs->grid.a = 96;
    gs->text = tok_or(FDK_TK_TEXT_DISABLED, 0x9a, 0x9a, 0xa0);
}

/* one series' paint: grid + filled area + the 1px line, right-to-
 * left time (the newest sample at the right edge). `max` is the y
 * scale (0 maps to the bottom, max to the top). */
static void series_paint(fdk_surface *s, fdk_rect b,
                         const mo_series *series, double max,
                         const graph_style *gs) {
    /* the graticule: 3 interior rules                          */
    for (int i = 1; i <= 3; i++) {
        fdk_i32 y = b.y + (fdk_i32)((double)b.height * i / 4.0);
        (void)fdk_surface_fill_rect(
            s, (fdk_rect){b.x, y, b.width, 1}, gs->grid);
    }
    if (series->count < 2) {
        return; /* one point has no slope                          */
    }
    double scale = (max > 0.000001) ? max : 1.0;
    fdk_i32 n = (fdk_i32)series->count;
    fdk_i32 x1 = b.x + (fdk_i32)b.width - 1;
    fdk_i32 step_x = (fdk_i32)b.width / (MO_HISTORY - 1);
    if (step_x < 1) {
        step_x = 1;
    }
    /* the area fill: vertical 1px slices (trapezoid-free, cheap,
     * and the damage-merger coalesces them)                      */
    for (fdk_i32 i = 1; i < n; i++) {
        double v0 = series->hist[i - 1];
        double v1 = series->hist[i];
        if (v0 > scale) v0 = scale;
        if (v1 > scale) v1 = scale;
        fdk_i32 h_in = (fdk_i32)b.height - 2;
        if (h_in < 1) {
            h_in = 1; /* a degenerate band: flat-line it           */
        }
        fdk_i32 y0 = b.y + (fdk_i32)b.height - 1 -
                     (fdk_i32)((v0 / scale) * (double)h_in);
        fdk_i32 y1 = b.y + (fdk_i32)b.height - 1 -
                     (fdk_i32)((v1 / scale) * (double)h_in);
        fdk_i32 top = (y0 < y1) ? y0 : y1;
        fdk_i32 bot = (y0 > y1) ? y0 : y1;
        fdk_i32 x = x1 - (n - 1 - i) * step_x;
        fdk_i32 w_slice = step_x + 1; /* step_x >= 1                */
        fdk_i32 h_slice = (bot >= top) ? (bot - top + 1) : 1;
        if (h_slice < 1) {
            h_slice = 1;
        }
        (void)fdk_surface_fill_rect(
            s, (fdk_rect){x, top, w_slice, h_slice}, gs->fill);
        (void)fdk_surface_fill_rect(
            s, (fdk_rect){x, y1, w_slice, 1}, gs->line);
    }
}

static void graph_frame(fdk_surface *s, fdk_rect bounds) {
    fdk_color bg = tok_or(FDK_TK_ENTRY_BACKGROUND, 0x23, 0x23, 0x29);
    (void)fdk_surface_fill_rect(s, bounds, bg);
    fdk_color border = tok_or(FDK_TK_ENTRY_BORDER, 0x44, 0x44, 0x4a);
    (void)fdk_surface_draw_rect(s, bounds, border);
}

static fdk_rect graph_plot(fdk_rect bounds) {
    fdk_rect plot = bounds;
    plot.x += 2;
    plot.y += 2;
    plot.width = (bounds.width > 4) ? bounds.width - 4 : 1;
    plot.height = (bounds.height > 4) ? bounds.height - 4 : 1;
    return plot;
}

static void cpu_graph_paint(fdk_widget *w, fdk_surface *s,
                            fdk_rect bounds, fdk_rect clip,
                            void *user) {
    (void)w;
    (void)clip;
    (void)user;
    graph_style gs;
    graph_style_init(&gs);
    graph_frame(s, bounds);
    series_paint(s, graph_plot(bounds), &mapp.cpu.load, 100.0, &gs);
}

static void net_graph_paint(fdk_widget *w, fdk_surface *s,
                            fdk_rect bounds, fdk_rect clip,
                            void *user) {
    (void)w;
    (void)clip;
    (void)user;
    graph_style gs;
    graph_style_init(&gs);
    graph_frame(s, bounds);
    /* the y scale: the busier direction, floored at 16 KiB/s so an
     * idle link does not amplify noise to full height            */
    double max = mo_series_max(&mapp.net.rx_kib);
    double tmax = mo_series_max(&mapp.net.tx_kib);
    if (tmax > max) {
        max = tmax;
    }
    if (max < 16.0) {
        max = 16.0;
    }
    series_paint(s, graph_plot(bounds), &mapp.net.rx_kib, max, &gs);
    /* TX rides the same scale in the accent-warm tint            */
    gs.line = tok_or(FDK_TK_WARNING, 0xe0, 0x7a, 0x3c);
    gs.fill = gs.line;
    gs.fill.a = 48;
    series_paint(s, graph_plot(bounds), &mapp.net.tx_kib, max, &gs);
}

void mo_graphs_build(void) {
    (void)fdk_canvas_create(mapp.base.content, cpu_graph_paint, NULL,
                            &mapp.cpu_graph);
    fdk_widget_set_expand(mapp.cpu_graph, true, true);
    (void)fdk_canvas_create(mapp.base.content, net_graph_paint, NULL,
                            &mapp.net_graph);
    fdk_widget_set_expand(mapp.net_graph, true, true);
}

void mo_graphs_tick(void) {
    if (mapp.cpu_graph != NULL) {
        fdk_canvas_invalidate(mapp.cpu_graph);
    }
    if (mapp.net_graph != NULL) {
        fdk_canvas_invalidate(mapp.net_graph);
    }
}
