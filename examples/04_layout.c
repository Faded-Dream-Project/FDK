/* 04_layout.c — the Phase 5 layout engine, live.
 *
 * The whole interface is built from boxes: the window's CONTENT is a
 * vertical box (header row, button row, an expanding main panel,
 * footer), and every child position on screen is assigned by layout —
 * fdk_widget_create bounds are only size REQUESTS here, nothing is
 * placed by hand.
 *
 * Phase 5 completion adds the GRID: inside the main panel, below the
 * breathing meter, a 3x2 grid with a two-column SPAN cell and an
 * expanding last column — resize the window and watch ONLY that
 * column absorb the width while the others keep their tracks (and
 * the gaps stay exactly `spacing` pixels).
 *
 * The demo also animates two things layout responds to:
 *   - the meter's size REQUEST (fdk_widget_set_natural_size) breathes
 *     inside the expanding panel — the box re-arranges it live
 *   - the WINDOW itself oscillates in size — every configure
 *     re-arranges the content box, and every child reflows
 *
 * Try resizing the window yourself: everything follows. Tab moves
 * focus between the buttons; Escape or the close request ends it.
 */

#include "fdk/fdk.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static struct {
    fdk_window *window;
    bool quit;
} app;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (event->type == FDK_EVENT_KEY_DOWN &&
         event->key.scancode == FDK_KEY_ESC)) {
        app.quit = true;
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "fdk_init failed (no display?)\n");
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 04 — layout",
        .width = 560,
        .height = 400,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &app.window))) {
        fprintf(stderr, "fdk_window_create failed\n");
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);
    fdk_widget_set_background(root, col(22, 24, 32));

    /* content: vertical box — the only thing placed "by hand" is the
     * top container (set_content does even that). */
    fdk_widget *content = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &content);
    fdk_box_set_padding(content, 12);
    fdk_box_set_spacing(content, 10);
    fdk_window_set_content(app.window, content);

    /* header row: dots + expanding spacer, fixed height */
    fdk_widget *header = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &header);
    fdk_widget_set_natural_size(header, 0, 28);
    const int dot_rgb[3][3] = {{235, 90, 90}, {235, 190, 90}, {100, 210, 130}};
    for (int i = 0; i < 3; i++) {
        fdk_widget *dot = NULL;
        (void)fdk_widget_create(header, NULL,
                                (fdk_rect){0, 0, 20, 20}, &dot);
        fdk_widget_set_margin(dot, 4, 4, i < 2 ? 8 : 4, 4);
        fdk_widget_set_background(
            dot, col(dot_rgb[i][0], dot_rgb[i][1], dot_rgb[i][2]));
        fdk_widget_set_corner_radius(dot, 10);
    }
    fdk_widget *title_bar = NULL;
    (void)fdk_widget_create(header, NULL, (fdk_rect){0, 0, 0, 20}, &title_bar);
    fdk_widget_set_expand(title_bar, true, false);
    fdk_widget_set_margin(title_bar, 12, 4, 4, 4);
    fdk_widget_set_background(title_bar, col(38, 42, 58));
    fdk_widget_set_corner_radius(title_bar, 6);

    /* button row: three equal thirds (all expand) */
    fdk_widget *row = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row);
    fdk_widget_set_natural_size(row, 0, 48);
    const int btn_rgb[3][2] = {{86, 96, }, {96, 86, }, {86, 96, }};
    for (int i = 0; i < 3; i++) {
        fdk_widget *btn = NULL;
        (void)fdk_widget_create(row, NULL, (fdk_rect){0, 0, 60, 44}, &btn);
        fdk_widget_set_expand(btn, true, true);
        fdk_widget_set_margin(btn, i > 0 ? 10 : 0, 0, 0, 0);
        fdk_widget_set_can_focus(btn, true);
        fdk_widget_set_corner_radius(btn, 8);
        fdk_widget_set_background(btn, col(btn_rgb[i][0], btn_rgb[i][1], 120));
    }

    /* main area: expanding vertical box holding the breathing meter */
    fdk_widget *main_box = NULL;
    (void)fdk_box_create(content, FDK_VERTICAL, &main_box);
    fdk_widget_set_expand(main_box, false, true);
    fdk_box_set_padding(main_box, 10);
    fdk_widget_set_background(main_box, col(30, 34, 48));
    fdk_widget_set_corner_radius(main_box, 10);

    fdk_widget *meter = NULL;
    (void)fdk_widget_create(main_box, NULL, (fdk_rect){0, 0, 0, 100}, &meter);
    fdk_widget_set_align(meter, FDK_ALIGN_FILL, FDK_ALIGN_START);
    fdk_widget_set_margin(meter, 0, 0, 0, 8);
    fdk_widget_set_background(meter, col(80, 210, 130));
    fdk_widget_set_corner_radius(meter, 8);

    /* the GRID (Phase 5 completion): 3 columns x 2 rows, spacing 8.
     * Track naturals come from the cells' create bounds: col widths
     * 90 / 120 / 70, row heights 40. The bottom-left cell SPANS two
     * columns; the LAST column is expand-marked, so it — and only it
     * — absorbs the window's width changes (watch it breathe with
     * the window while the other tracks and the gaps stay put). */
    fdk_widget *cells = NULL;
    (void)fdk_grid_create(main_box, 2, 3, &cells);
    fdk_grid_set_spacing(cells, 8);
    fdk_grid_set_column_expand(cells, 2, true);
    fdk_widget *gc[5];
    const int gc_rgb[5][3] = {
        {70, 130, 230},  /* (0,0) blue — track 0 */
        {235, 170, 70}, /* (1,0) amber — track 1 */
        {160, 100, 230},/* (2,0) violet — the EXPANDING track */
        {70, 190, 200}, /* (0,1)+(1,1) teal, SPANNING tracks 0+1 */
        {230, 110, 170},/* (2,1) pink — expanding track, row 1 */
    };
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 90, 40}, &gc[0]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 120, 40}, &gc[1]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 70, 40}, &gc[2]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 90, 40}, &gc[3]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 70, 40}, &gc[4]);
    for (int i = 0; i < 5; i++) {
        fdk_widget_set_background(gc[i], col(gc_rgb[i][0], gc_rgb[i][1],
                                             gc_rgb[i][2]));
    }
    (void)fdk_grid_attach(cells, gc[0], 0, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[1], 1, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[2], 2, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[3], 0, 1, 2, 1); /* colspan 2 */
    (void)fdk_grid_attach(cells, gc[4], 2, 1, 1, 1);

    /* footer: fixed strip */
    fdk_widget *footer = NULL;
    (void)fdk_widget_create(content, NULL, (fdk_rect){0, 0, 0, 18}, &footer);
    fdk_widget_set_background(footer, col(38, 42, 58));
    fdk_widget_set_corner_radius(footer, 6);

    fdk_window_show(app.window);
    (void)fdk_window_paint(app.window);

    /* Drive: the window breathes (layout reflows on every configure)
     * and the meter's REQUEST breathes (layout reflows on the hint).
     * After the animated phase, two STEADY holds at fixed sizes —
     * deterministic states for the test rig's screenshots (capturing
     * mid-oscillation races the resize/paint cycle). */
    int frames = 0;
    int phase = 0; /* 0 storm, 1 hold A, 2 hold B */
    int hold_start = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break;
        }

        int w, h;
        int meter_h;
        if (phase == 0) {
            double t = (double)frames / 24.0;
            w = 560 + (int)(120.0 * sin(t));
            h = 400 + (int)(70.0 * sin(t * 0.63));
            meter_h = 60 + (int)(90.0 * (0.5 + 0.5 * sin(t * 1.7)));
        } else {
            /* steady: fixed size, gentle meter breathing only (the
             * range is tight so the grid below always fits in the
             * smaller hold too) */
            w = (phase == 1) ? 660 : 500;
            h = (phase == 1) ? 480 : 380;
            meter_h = 90 + (int)(20.0 * sin((double)frames / 20.0));
        }
        fdk_window_resize(app.window, w, h);
        fdk_widget_set_natural_size(meter, 0, meter_h);

        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        (void)fdk_window_paint(app.window);
        frames++;

        if (phase == 0 && frames >= 24 * 8) {
            phase = 1;
            hold_start = frames;
            printf("HOLD_A\n");
            fflush(stdout);
        } else if (phase == 1 && frames - hold_start >= 24 * 3) {
            phase = 2;
            hold_start = frames;
            printf("HOLD_B\n");
            fflush(stdout);
        } else if (phase == 2 && frames - hold_start >= 24 * 3) {
            app.quit = true;
        }
    }

    printf("04_layout: exited cleanly after %d frames\n", frames);
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_shutdown(ctx);
    return 0;
}
