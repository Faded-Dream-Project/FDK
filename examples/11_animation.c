/* 11_animation.c — the animation engine, end to end.
 *
 * Two live demos, both driving the 1.3.8 engine through the real
 * ~16 ms ticker (the pump loop the shared example helper runs):
 *
 *   The easing race — seven lanes, one per curve family, each with
 *   a colored block. "Play" (or startup, or every cycle when
 *   FDK_DEMO_ANIMATE=1) starts one 2.2 s flight per lane; every
 *   tick sets the block's x from the EASED progress, so the lanes
 *   visibly disagree: linear marches, quad-in crawls-then-sprints,
 *   cubic-out leaps-then-settles, back_out overshoots past the end
 *   and returns. The whole motion is fdk_widget_animate + set_bounds
 *   + the damage tracker — no custom rendering anywhere.
 *
 *   The smooth scroll — a 40-row list in a ScrollView. "Down 5" and
 *   "Up 5" COMPOSE the animator with programmatic scrolling (the
 *   public scroll_to is the instant truth-setting path by design,
 *   so the button owns the easing): one flight per click, each tick
 *   easing the offset between the from-offset and the target row.
 *   Clicking mid-flight retargets from wherever the list currently
 *   is — the same accumulation rule the wheel gestures get for
 *   free inside the ScrollView. The mouse wheel over the list uses
 *   the ScrollView's built-in flight.
 *
 * Layout: a vertical box stacks the header, the race zone, the
 * buttons, and the list zone. The rows inside the race zone and the
 * ScrollView inside the list zone are placed MANUALLY (labels
 * measure to their text, so aligned lanes want manual geometry; the
 * ScrollView measures to its content, so a fixed-height viewport
 * wants a fixed-natural wrapper) — synced every frame, which is
 * exactly when resizes land.
 *
 * The rig check is deliberately phase-independent: every block is
 * always somewhere inside its lane's travel band, at every point of
 * every cycle — so "colored pixels in the lane band" holds no
 * matter when the capture lands. Set FDK_DEMO_ANIMATE=1 to loop the
 * race forever; FDK_DEMO_FRAMES=N exits after N frames. Close the
 * window or press ESC to exit. Needs a system TrueType font.
 */

#include "example_window.h"
#include "fdk/fdk_animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_font *font15 = NULL;
static fdk_widget *status = NULL;

/* ---- the easing race ----------------------------------------------------- */

typedef struct {
    fdk_ease_kind kind;
    const char *name;
    fdk_color color;    /* the block's fill                       */
    fdk_widget *label;  /* manually placed                        */
    fdk_widget *lane;   /* the travel band                        */
    fdk_widget *block;  /* the moving square, child of the lane   */
    fdk_animation *anim;
    fdk_i32 travel;     /* lane width - block, refreshed on sync  */
} lane_t;

static lane_t lanes[] = {
    { FDK_EASE_LINEAR, "Linear", {0.55f, 0.72f, 0.95f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_SMOOTHSTEP, "Smoothstep", {0.46f, 0.87f, 0.62f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_QUAD_IN, "Quad in", {0.92f, 0.66f, 0.35f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_CUBIC_OUT, "Cubic out", {0.87f, 0.49f, 0.62f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_SINE_IN_OUT, "Sine", {0.61f, 0.52f, 0.89f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_EXPO_OUT, "Expo out", {0.95f, 0.83f, 0.40f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
    { FDK_EASE_BACK_OUT, "Back out", {0.95f, 0.55f, 0.49f, 1.0f},
      NULL, NULL, NULL, NULL, 0 },
};
#define LANE_COUNT ((int)(sizeof lanes / sizeof lanes[0]))
#define LANE_H 36
#define LANE_GAP 6
#define BLOCK 22
#define LABEL_W 104
#define RACE_MS 2200

static void set_status(const char *text) {
    (void)fdk_label_set_text(status, text);
}

static fdk_widget *race_zone = NULL;

/* Manual row geometry: label column + travel lane per row, inside
 * the zone's current bounds. The blocks are children of their lanes
 * and keep their own x (the animation owns it; the sync never
 * touches block x). */
static void race_sync_layout(void) {
    fdk_rect z = fdk_widget_get_bounds(race_zone);
    if (z.width < LABEL_W + BLOCK + 8) {
        return;
    }
    fdk_i32 lane_w = z.width - LABEL_W - 8;
    for (int i = 0; i < LANE_COUNT; i++) {
        fdk_i32 y = i * (LANE_H + LANE_GAP);
        fdk_rect lb = { 0, y + (LANE_H - 15) / 2, LABEL_W, LANE_H };
        fdk_widget_set_bounds(lanes[i].label, lb);
        fdk_rect ln = { LABEL_W + 4, y, lane_w, LANE_H };
        fdk_widget_set_bounds(lanes[i].lane, ln);
        lanes[i].travel = lane_w > BLOCK ? lane_w - BLOCK : 0;
    }
}

/* One block's per-frame step: eased progress -> x inside the lane.
 * e may exceed 1 (back_out's overshoot) — the cap lets the square
 * peek half a block past the end without escaping the band. */
static void race_tick(fdk_animation *anim, double e, void *user) {
    (void)anim;
    lane_t *ln = user;
    double x = (double)ln->travel * e;
    if (x < 0.0) {
        x = 0.0;
    }
    if (x > (double)ln->travel + BLOCK * 0.5) {
        x = (double)ln->travel + BLOCK * 0.5;
    }
    fdk_rect b = { (fdk_i32)x, (LANE_H - BLOCK) / 2, BLOCK, BLOCK };
    fdk_widget_set_bounds(ln->block, b);
}

static void race_done(fdk_animation *anim, bool finished, void *user) {
    lane_t *ln = user;
    if (ln->anim == anim) {
        ln->anim = NULL;
    }
    if (finished && ln == &lanes[LANE_COUNT - 1]) {
        set_status("Race finished — Play runs it again.");
    }
}

static void race_play(void) {
    for (int i = 0; i < LANE_COUNT; i++) {
        if (lanes[i].anim != NULL) {
            fdk_animation_cancel(lanes[i].anim);
            lanes[i].anim = NULL;
        }
        fdk_rect start = { 0, (LANE_H - BLOCK) / 2, BLOCK, BLOCK };
        fdk_widget_set_bounds(lanes[i].block, start);
        lanes[i].anim = fdk_widget_animate(
            lanes[i].block, RACE_MS, lanes[i].kind, race_tick,
            race_done, &lanes[i]);
    }
    set_status("Seven easings racing over the same 2.2 s.");
}

static void on_play(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    race_play();
}

/* ---- the smooth scroll ---------------------------------------------------- */

static fdk_widget *list_zone = NULL; /* fixed-natural wrapper      */
static fdk_widget *scroller = NULL;
static fdk_animation *scroll_anim = NULL;
static fdk_i32 scroll_from = 0;
static fdk_i32 scroll_target = 0;
#define ROW_H 26
#define SCROLL_MS 300

/* Manual sync: the scrollview fills its wrapper (which carries the
 * natural height the box respects — a scrollview's own natural is
 * its content's, and 40 rows would ask for all of it). Direct
 * set_bounds does NOT run the scrollview's arrange hook (only a
 * container's layout does), so the internal placement is driven
 * through the public path — scroll_by(0, 0) re-runs the layout
 * while KEEPING the offset (a real resize keeps the reader's
 * place). Change-guarded: the sync runs on resize, not per frame,
 * so it never fights an in-flight scroll. */
static void scroll_sync_layout(void) {
    fdk_rect z = fdk_widget_get_bounds(list_zone);
    fdk_rect s = fdk_widget_get_bounds(scroller);
    if (s.x == 0 && s.y == 0 && s.width == z.width &&
        s.height == z.height) {
        return;
    }
    /* ARRANGE runs the scrollview's hook (bounds + internal
     * placement of the content) — set_bounds alone would skip it. */
    fdk_widget_arrange(scroller, (fdk_rect){0, 0, z.width, z.height});
}

static void scroll_tick(fdk_animation *anim, double e, void *user) {
    (void)anim;
    (void)user;
    fdk_i32 y = scroll_from + (fdk_i32)((double)(scroll_target -
                                                 scroll_from) * e);
    (void)fdk_scrollview_scroll_to(scroller, 0, y);
}

static void scroll_done(fdk_animation *anim, bool finished, void *user) {
    (void)user;
    (void)finished;
    if (scroll_anim == anim) {
        scroll_anim = NULL;
    }
}

/* Animated scroll composed by the app (the programmatic scroll_to
 * is instant BY DESIGN — the wheel path eases inside the
 * scrollview; a button that wants the same feel drives it exactly
 * like the wheel path would). */
static void scroll_animated(fdk_i32 rows) {
    if (scroll_anim != NULL) {
        /* Retarget from the LIVE offset (the accumulation rule). */
        fdk_animation_cancel(scroll_anim);
        scroll_anim = NULL;
    }
    fdk_i32 x = 0, y = 0;
    (void)fdk_scrollview_get_scroll_offset(scroller, &x, &y);
    scroll_from = y;
    scroll_target = y + rows * ROW_H;
    /* Clamp: the content is 40 x 26 = 1040 tall; the viewport is
     * the zone height minus the scrollbar strip. */
    fdk_rect z = fdk_widget_get_bounds(list_zone);
    fdk_i32 viewport_h = z.height - 12; /* horizontal bar hidden:
                                         * the rows fit the width */
    fdk_i32 max_h = 40 * ROW_H - viewport_h;
    if (max_h < 0) {
        max_h = 0;
    }
    if (scroll_target < 0) {
        scroll_target = 0;
    }
    if (scroll_target > max_h) {
        scroll_target = max_h;
    }
    if (scroll_target == scroll_from) {
        set_status(rows > 0 ? "Already at the bottom." :
                              "Already at the top.");
        return;
    }
    scroll_anim = fdk_widget_animate(scroller, SCROLL_MS,
                                     FDK_EASE_CUBIC_OUT, scroll_tick,
                                     scroll_done, NULL);
    set_status(rows > 0 ? "Easing down five rows (the wheel works too)."
                        : "Easing up five rows (the wheel works too).");
}

static void on_down(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    scroll_animated(5);
}

static void on_up(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    scroll_animated(-5);
}

/* ---- build + run ------------------------------------------------------------ */

int main(void) {
    font15 = fdk_font_load_system_default(15);
    if (font15 == NULL) {
        fprintf(stderr, "11_animation: no system TrueType font found "
                        "— this demo needs one. Install a face like "
                        "DejaVu Sans or Noto Sans, or point "
                        "FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("11_animation: using font %s\n",
           fdk_font_get_file_path(font15));

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "11")) {
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "11", "animation", 640, 560)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget *content = ex.content;
    fdk_box_set_spacing(content, 10);

    /* Header row: Play + the status line. */
    fdk_widget *header = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &header);
    fdk_box_set_spacing(header, 10);
    fdk_widget_set_natural_size(header, 320, 34);
    fdk_widget *play = NULL;
    (void)fdk_button_create(header, font15, "Play", &play);
    fdk_button_set_on_activate(play, on_play, NULL);
    (void)fdk_widget_set_tooltip(play,
                                 "Restart the seven-lane easing race");
    status = NULL;
    (void)fdk_label_create(header, font15,
                           "Seven easings, one 2.2 s flight each.",
                           &status);
    fdk_label_set_mode(status, FDK_LABEL_ELLIPSIZE);
    fdk_widget_set_expand(status, true, false);

    /* The race zone: one manual-geometry widget holding the lanes.
     * Fixed natural height (boxes give it the leftover window
     * height via vertical expand). */
    race_zone = NULL;
    (void)fdk_widget_create(content, NULL,
                            (fdk_rect){0, 0, 560,
                                       LANE_COUNT * (LANE_H + LANE_GAP)},
                            &race_zone);
    fdk_widget_set_expand(race_zone, true, true);
    fdk_widget_set_natural_size(race_zone, 560,
                                LANE_COUNT * (LANE_H + LANE_GAP));
    for (int i = 0; i < LANE_COUNT; i++) {
        fdk_widget *lab = NULL;
        (void)fdk_label_create(race_zone, font15, lanes[i].name, &lab);
        lanes[i].label = lab;
        fdk_widget *lane = NULL;
        (void)fdk_widget_create(race_zone, NULL,
                                (fdk_rect){0, 0, 100, LANE_H}, &lane);
        fdk_widget_set_background_token(lane, FDK_TK_CONTROL_BACKGROUND);
        (void)fdk_widget_set_tooltip(lane, lanes[i].name);
        fdk_widget *block = NULL;
        (void)fdk_widget_create(lane, NULL,
                                (fdk_rect){0, (LANE_H - BLOCK) / 2,
                                           BLOCK, BLOCK},
                                &block);
        fdk_widget_set_background(block, lanes[i].color);
        lanes[i].lane = lane;
        lanes[i].block = block;
    }

    /* The smooth-scroll controls + list zone. */
    fdk_widget *nav = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &nav);
    fdk_box_set_spacing(nav, 8);
    fdk_widget_set_natural_size(nav, 240, 34);
    fdk_widget *down = NULL;
    (void)fdk_button_create(nav, font15, "Down 5", &down);
    fdk_button_set_on_activate(down, on_down, NULL);
    fdk_widget *up = NULL;
    (void)fdk_button_create(nav, font15, "Up 5", &up);
    fdk_button_set_on_activate(up, on_up, NULL);
    (void)fdk_widget_set_tooltip(
        down, "Animated scrolling composed with fdk_widget_animate; "
              "the mouse wheel eases inside the ScrollView itself");

    list_zone = NULL;
    (void)fdk_widget_create(content, NULL, (fdk_rect){0, 0, 560, 170},
                            &list_zone);
    fdk_widget_set_natural_size(list_zone, 560, 170);
    fdk_widget_set_expand(list_zone, true, false);
    scroller = NULL;
    (void)fdk_scrollview_create(list_zone, &scroller);
    fdk_widget *rows = NULL;
    (void)fdk_box_create(scroller, FDK_VERTICAL, &rows);
    for (int i = 0; i < 40; i++) {
        fdk_widget *rl = NULL;
        char buf[40];
        snprintf(buf, sizeof buf, "Row %02d — scroll me", i);
        (void)fdk_label_create(rows, font15, buf, &rl);
        fdk_widget_set_natural_size(rl, 200, ROW_H);
    }
    (void)fdk_scrollview_set_content(scroller, rows);

    /* First layout pass (manual zones need real bounds), then the
     * first race — a capture taken mid-flight shows the curves
     * disagreeing. */
    (void)fdk_window_paint(ex.window);
    race_sync_layout();
    scroll_sync_layout();
    race_play();

    int loop = getenv("FDK_DEMO_ANIMATE") != NULL;
    unsigned long races = 1;
    while (fdk_example_pump(&ex)) {
        /* Manual-geometry sync: resizes land as configure events,
         * and the pump is the single place every frame passes. */
        race_sync_layout();
        scroll_sync_layout();
        if (loop) {
            /* Re-race every ~3.4 s: the flight is 2.2 s, so the
             * blocks spend most of the time moving. */
            if (ex.frames % 220 == 219) {
                race_play();
                races++;
            }
        }
    }

    fdk_font_destroy(font15);
    fdk_example_close(&ex);
    printf("11_animation: exited cleanly after %ld frames, "
           "%lu races\n",
           ex.frames, races);
    return 0;
}
