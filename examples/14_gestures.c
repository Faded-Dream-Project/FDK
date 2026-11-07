/* 14_gestures.c — the §23/B2b recognizer surface, live.
 *
 * One window split in two: a CANVAS the recognizers act on and a
 * LOG of every fired callback. Attach the same event stream to
 * seven recognizers (the widget's event callback forwards its
 * POINTER_ and TOUCH- events to each through fdk_gesture_feed):
 *
 *   click       single/double/triple — the ball hops, double hops
 *               twice, triple cycles its color
 *   long-press  500 ms — the ball grows; drag before the hold
 *               cancels (logged)
 *   drag        moves the ball (begin/update/end logged with the
 *               offsets)
 *   pan         horizontal-only over the canvas: denied when the
 *               motion is vertical (the latch, logged)
 *   swipe       a fast release flings the ball along the direction
 *   zoom/rotate a two-finger posture scales/spins the ball
 *
 * Everything is OBSERVABLE: every callback appends to the log the
 * window repaints, so the demo doubles as a manual test rig.
 */

#include "example_window.h"

#include "fdk/fdk_gesture.h"
#include "fdk/fdk_text.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static fdk_example app;

#define LOG_LINES 14
#define LOG_TEXT 96

static struct {
    fdk_widget *canvas;   /* the gesture surface                  */
    fdk_font *font;
    fdk_gesture *click, *lp, *drag, *pan, *swipe, *zoom, *rotate;
    /* the ball */
    fdk_f32 ball_x, ball_y, ball_r;
    fdk_color ball_color;
    /* the log */
    char log[LOG_LINES][LOG_TEXT];
    int log_head;
    bool dirty;
} g;

static void log_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.log[g.log_head], LOG_TEXT, fmt, ap);
    va_end(ap);
    g.log_head = (g.log_head + 1) % LOG_LINES;
    g.dirty = true;
}

/* ---- recognizer callbacks ---- */

static void on_click(fdk_gesture *gest, int n_press, fdk_pointf where,
                     void *user) {
    (void)gest; (void)user;
    g.ball_x = where.x;
    g.ball_y = where.y;
    if (n_press == 3) {
        /* triple: cycle the color */
        if (g.ball_color.r > 0.9f && g.ball_color.b < 0.1f) {
            g.ball_color = (fdk_color){0.2f, 0.8f, 0.4f, 1};
        } else if (g.ball_color.g > 0.7f) {
            g.ball_color = (fdk_color){0.9f, 0.5f, 0.9f, 1};
        } else {
            g.ball_color = (fdk_color){0.95f, 0.3f, 0.3f, 1};
        }
    }
    log_line("click x%d at (%.0f, %.0f)", n_press, where.x, where.y);
}

static void on_long_press(fdk_gesture *gest, fdk_pointf where,
                          bool pressed, void *user) {
    (void)gest; (void)user;
    if (pressed) {
        g.ball_r *= 1.3f;
        if (g.ball_r > 60.0f) {
            g.ball_r = 12.0f;
        }
        log_line("long-press at (%.0f, %.0f) -> r=%.0f", where.x,
                 where.y, g.ball_r);
    } else {
        log_line("long-press cancelled (early release)");
    }
}

static void on_drag(fdk_gesture *gest, fdk_pointf offset,
                    fdk_pointf where, void *user) {
    (void)gest; (void)user;
    g.ball_x = where.x;
    g.ball_y = where.y;
    log_line("drag offset (%+.0f, %+.0f)", offset.x, offset.y);
}

static void on_swipe(fdk_gesture *gest, fdk_f32 vx, fdk_f32 vy,
                     fdk_gesture_direction dir, void *user) {
    (void)gest; (void)user;
    const char *name =
        dir == FDK_GESTURE_DIR_LEFT     ? "left"
        : dir == FDK_GESTURE_DIR_RIGHT  ? "right"
        : dir == FDK_GESTURE_DIR_UP     ? "up"
                                        : "down";
    g.ball_x += vx * 0.05f;
    g.ball_y += vy * 0.05f;
    log_line("swipe %s (%.0f px/s)", name,
             sqrtf(vx * vx + vy * vy));
}

static void on_zoom(fdk_gesture *gest, fdk_f32 value, fdk_f32 delta,
                    void *user) {
    (void)gest; (void)user;
    g.ball_r *= (1.0f + delta * 0.5f);
    if (g.ball_r < 4.0f) {
        g.ball_r = 4.0f;
    }
    if (g.ball_r > 80.0f) {
        g.ball_r = 80.0f;
    }
    log_line("zoom %.2fx (delta %+.2f)", value, delta);
}

static void on_rotate(fdk_gesture *gest, fdk_f32 value, fdk_f32 delta,
                      void *user) {
    (void)gest; (void)user;
    log_line("rotate %+.1f deg (delta %+.1f)", value * 57.2958f,
             delta * 57.2958f);
}

/* ---- the canvas ---- */

static void canvas_paint(fdk_widget *w, fdk_surface *s,
                         fdk_rect bounds, fdk_rect clip, void *user) {
    (void)w; (void)bounds; (void)clip; (void)user;
    fdk_surface_info info;
    fdk_surface_get_info(s, &info);
    fdk_surface_fill(s, (fdk_color){0.09f, 0.09f, 0.12f, 1});

    /* the ball */
    fdk_surface_fill_circle_aa(
        s, (fdk_i32)g.ball_x, (fdk_i32)g.ball_y, (fdk_i32)g.ball_r,
        g.ball_color);

    /* the log */
    fdk_i32 y = info.height - LOG_LINES * 16 - 8;
    for (int i = 0; i < LOG_LINES; i++) {
        const char *line =
            g.log[(g.log_head + i) % LOG_LINES];
        if (line[0] != '\0') {
            fdk_surface_draw_utf8(s, g.font, line, strlen(line), 8, y,
                                  (fdk_color){0.7f, 0.75f, 0.8f, 1});
        }
        y += 16;
    }
}

/* The window-level stream: the FULL event set (pointer AND the
 * touch slots the widget-local model does not carry). */
static void window_event(fdk_window *w, const fdk_event_data *ev,
                         void *user) {
    fdk_gesture_feed(g.click, ev);
    fdk_gesture_feed(g.lp, ev);
    fdk_gesture_feed(g.drag, ev);
    fdk_gesture_feed(g.pan, ev);
    fdk_gesture_feed(g.swipe, ev);
    fdk_gesture_feed(g.zoom, ev);
    fdk_gesture_feed(g.rotate, ev);
    if (g.dirty) {
        fdk_canvas_invalidate(g.canvas);
        g.dirty = false;
    }
    (void)w; (void)user;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!fdk_example_init(&app.ctx, "14")) {
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK 14 — gesture recognizers",
        .width = 640,
        .height = 480,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fdk_shutdown(app.ctx);
        return 1;
    }
    (void)fdk_window_set_decorated(app.window, true);
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(app.window, &root)));

    g.font = fdk_font_load_system_default(13);
    g.ball_x = 320;
    g.ball_y = 180;
    g.ball_r = 16.0f;
    g.ball_color = (fdk_color){0.95f, 0.3f, 0.3f, 1};

    g.click = fdk_gesture_create(FDK_GESTURE_CLICK);
    g.lp = fdk_gesture_create(FDK_GESTURE_LONG_PRESS);
    g.drag = fdk_gesture_create(FDK_GESTURE_DRAG);
    g.pan = fdk_gesture_create(FDK_GESTURE_PAN);
    g.swipe = fdk_gesture_create(FDK_GESTURE_SWIPE);
    g.zoom = fdk_gesture_create(FDK_GESTURE_ZOOM);
    g.rotate = fdk_gesture_create(FDK_GESTURE_ROTATE);
    fdk_gesture_on_click(g.click, on_click, NULL);
    fdk_gesture_on_long_press(g.lp, on_long_press, NULL);
    fdk_gesture_on_drag(g.drag, on_drag, NULL);
    fdk_gesture_on_drag(g.pan, on_drag, NULL);
    fdk_gesture_set_orientation(g.pan, FDK_GESTURE_DIR_LEFT);
    fdk_gesture_on_swipe(g.swipe, on_swipe, NULL);
    fdk_gesture_on_scale(g.zoom, on_zoom, NULL);
    fdk_gesture_on_scale(g.rotate, on_rotate, NULL);

    fdk_window_set_event_callback(app.window, window_event, NULL);

    /* The canvas: the paint-callback widget, full-window. */
    assert(fdk_ok(fdk_canvas_create(root, canvas_paint, NULL,
                                    &g.canvas)));
    fdk_widget_arrange(g.canvas, (fdk_rect){0, 0, 640, 480});

    log_line("drag the ball; double-click hops; hold 500ms grows;");
    log_line("fling swipes; two fingers zoom/rotate (touchscreen)");

    fdk_window_show(app.window);
    fdk_example_run(&app);

    fdk_gesture_destroy(g.click);
    fdk_gesture_destroy(g.lp);
    fdk_gesture_destroy(g.drag);
    fdk_gesture_destroy(g.pan);
    fdk_gesture_destroy(g.swipe);
    fdk_gesture_destroy(g.zoom);
    fdk_gesture_destroy(g.rotate);
    fdk_font_destroy(g.font);
    fdk_example_close(&app);
    return 0;
}
