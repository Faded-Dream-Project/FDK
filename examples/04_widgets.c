/* 04_widgets.c — the widget catalog, end to end.
 *
 * The entire interface is catalog widgets inside boxes, frames, and
 * (bottom) a GRID — no widget is placed by hand and no pixel is
 * drawn by the app; layout assigns geometry and the catalog paints
 * itself:
 *
 *   Profile frame   — a Toggle and two Checkboxes
 *   Renderer frame  — a radio group (the frame's children)
 *   Layout frame    — a 3-column x 2-row grid with a two-column
 *                     SPAN cell and an expanding last column:
 *                     resize the window and watch ONLY that column
 *                     absorb the width while the other tracks and
 *                     the gaps stay exactly `spacing` pixels
 *   Buttons         — "Apply" grows the progress bar and rewrites
 *                     the status label; "Reset" clears everything;
 *                     every control reports into the status line
 *
 * The progress bar sweeps once on startup (so a screenshot shows it
 * mid-fill) and then holds; Tab moves focus through the controls.
 * Set FDK_DEMO_ANIMATE=1 to keep it sweeping forever, or
 * FDK_DEMO_FRAMES=N to exit after N frames. Close the window or
 * press ESC to exit. Needs a system TrueType font (exits with a
 * notice otherwise).
 */

#include "example_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *font16 = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static int apply_count = 0;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void set_status(const char *text) {
    /* Writes the helper's status line (assigned after open). */
    (void)fdk_label_set_text(status, text);
}

static void on_apply(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    apply_count++;
    fdk_progress_set_fraction(
        progress, fdk_progress_get_fraction(progress) + 0.25f);
    char buf[64];
    snprintf(buf, sizeof(buf), "Applied %d time%s.", apply_count,
             apply_count == 1 ? "" : "s");
    set_status(buf);
}

static void on_reset(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    fdk_progress_set_fraction(progress, 0.0f);
    apply_count = 0;
    set_status("Reset. Nothing applied yet.");
}

static void on_any_change(fdk_widget *w, bool checked, void *user) {
    (void)w;
    const char *what = user;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s is now %s.", what,
             checked ? "ON" : "OFF");
    set_status(buf);
}

int main(void) {
    font16 = fdk_font_load_system_default(16);
    if (font16 == NULL) {
        fprintf(stderr, "04_widgets: no system TrueType font found "
                        "— this demo needs one. Install a face like "
                        "DejaVu Sans or Noto Sans, or point "
                        "FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("04_widgets: using font %s\n",
           fdk_font_get_file_path(font16));

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "04")) {
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "04", "widgets", 560, 685)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget *content = ex.content;
    fdk_box_set_spacing(content, 12);
    fdk_widget_set_background(ex.root, col(18, 20, 28));
    (void)fdk_window_paint(ex.window); /* repaint under the new bg */

    /* --- frame: profile options --- */
    fdk_widget *profile = NULL;
    (void)fdk_frame_create(content, font16, "Profile", &profile);
    fdk_widget_set_background(profile, col(26, 29, 40));
    fdk_widget *pub = NULL;
    (void)fdk_toggle_create(profile, font16, "Public profile", &pub);
    fdk_toggle_set_on_change(pub, on_any_change, (void *)"Public profile");
    fdk_widget *mail = NULL;
    (void)fdk_checkbox_create(profile, font16, "Show email address",
                              &mail);
    fdk_checkbox_set_on_change(mail, on_any_change,
                               (void *)"Show email");
    fdk_widget *news = NULL;
    (void)fdk_checkbox_create(profile, font16, "Newsletter", &news);
    fdk_checkbox_set_on_change(news, on_any_change,
                               (void *)"Newsletter");

    /* --- frame: rendering mode (radio group = frame's children) --- */
    fdk_widget *render = NULL;
    (void)fdk_frame_create(content, font16, "Renderer", &render);
    fdk_widget_set_background(render, col(26, 29, 40));
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    (void)fdk_radio_create(render, font16, "Software (X11)", &r1);
    (void)fdk_radio_create(render, font16, "Software (Wayland)", &r2);
    (void)fdk_radio_create(render, font16, "Software (auto)", &r3);
    fdk_radio_set_checked(r3, true);
    fdk_radio_set_on_change(r1, on_any_change, (void *)"Renderer: X11");
    fdk_radio_set_on_change(r2, on_any_change,
                            (void *)"Renderer: Wayland");
    fdk_radio_set_on_change(r3, on_any_change, (void *)"Renderer: auto");

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: the GRID (the layout engine's third container) ---
     * 3 columns x 2 rows, spacing 8. Track naturals come from the
     * cells' create bounds: col widths 90 / 120 / 70, row heights 40.
     * The bottom-left cell SPANS two columns; the LAST column is
     * expand-marked, so it — and only it — absorbs the window's
     * width changes (resize the window and watch it grow while the
     * other tracks and the gaps stay put). */
    fdk_widget *grid_frame = NULL;
    (void)fdk_frame_create(content, font16, "Layout — grid", &grid_frame);
    fdk_widget_set_background(grid_frame, col(26, 29, 40));
    fdk_widget *cells = NULL;
    (void)fdk_grid_create(grid_frame, 2, 3, &cells);
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
        fdk_widget_set_corner_radius(gc[i], 6);
    }
    (void)fdk_grid_attach(cells, gc[0], 0, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[1], 1, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[2], 2, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[3], 0, 1, 2, 1); /* colspan 2 */
    (void)fdk_grid_attach(cells, gc[4], 2, 1, 1, 1);

    /* --- button row --- */
    fdk_widget *row = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 10);
    fdk_widget *apply = NULL;
    (void)fdk_button_create(row, font16, "Apply", &apply);
    fdk_button_set_on_activate(apply, on_apply, NULL);
    fdk_widget *reset = NULL;
    (void)fdk_button_create(row, font16, "Reset", &reset);
    fdk_button_set_on_activate(reset, on_reset, NULL);
    fdk_widget *filler = NULL;
    (void)fdk_widget_create(row, NULL, (fdk_rect){0, 0, 0, 1}, &filler);
    fdk_widget_set_expand(filler, true, false);

    /* --- progress + status --- */
    (void)fdk_progress_create(content, &progress);
    fdk_widget_set_natural_size(progress, 0, 14);
    fdk_widget_set_expand(progress, true, false);

    /* The demo's status line IS the helper's (bottom of the frame). */
    status = ex.status;

    const char *anim = getenv("FDK_DEMO_ANIMATE");
    const bool animate =
        anim != NULL && anim[0] != '\0' && strcmp(anim, "0") != 0;

    while (fdk_example_pump(&ex)) {
        /* One startup sweep so a screenshot shows the bar mid-fill;
         * then it holds (an idle app presents nothing). */
        if (ex.frames < 120) {
            fdk_progress_set_fraction(
                progress, (fdk_f32)ex.frames / 120.0f);
            if (ex.frames == 0) {
                fdk_example_set_status(&ex, "Sweeping...");
            }
        } else if (ex.frames == 120) {
            if (!animate) {
                apply_count = 4;
                fdk_example_set_status(&ex, "Applied 4 times.");
            }
        } else if (animate) {
            fdk_progress_set_fraction(
                progress, (fdk_f32)(ex.frames % 200) / 199.0f);
        }
    }

    fdk_font_destroy(font16);
    fdk_example_close(&ex);
    return 0;
}
