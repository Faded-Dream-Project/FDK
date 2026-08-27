/* 03_widgets.c — the Phase 4 widget foundation, live.
 *
 * Everything on screen here is a widget: the window's root widget
 * (fdk_window_get_root) carries a small tree of panels, buttons, and
 * a meter, and FDK does the rest — hit-testing, hover, the implicit
 * pointer grab, focus, Tab traversal, invalidation, damage-driven
 * partial repaints, and presentation — driven by a pump loop of
 * exactly two calls.
 *
 * Interactions to try:
 *   - move the mouse: buttons highlight on hover
 *   - click "hue": the button cycles color AND the meter's fill
 *     recolors (two partial repaints, one frame)
 *   - click "grow"/"shrink": the meter's fill resizes live
 *   - Tab / Shift+Tab: focus ring moves between the buttons
 *   - click "quit" (or press Escape / close the window): a widget
 *     callback destroys the window from inside an event dispatch —
 *     the reentrancy path the widget core is built around
 *
 * This is still the BASE widget class: real Button/Label widgets with
 * text arrive with the core-widget phase (see docs/roadmap.md); the
 * point of this example is that the foundation beneath them — tree,
 * events, focus, damage, painting — is real and complete.
 */

#include "fdk/fdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- demo state ---- */

typedef struct {
    fdk_color normal, hover, pressed, focused;
    bool is_down;
    bool armed_quit;
} button_style;

typedef struct {
    fdk_widget *widget;
    button_style style;
} demo_button;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

/* The app's "quit" flag + window, so a widget callback can end the
 * program by destroying the window mid-dispatch. */
static struct {
    fdk_window *window;
    bool quit;
} app;

/* The meter: a panel whose FILL child's width we resize. */
static struct {
    fdk_widget *fill;
    fdk_color color;
    int level; /* 0..100 */
} meter;

static void meter_set_level(int level) {
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }
    meter.level = level;
    fdk_rect fill_bounds = fdk_widget_get_bounds(meter.fill);
    fill_bounds.width = (level * fill_bounds.width) / 100; /* keep x/y/h */
    /* Re-derive from the panel's width so the math is stable: */
    fdk_rect panel = fdk_widget_get_bounds(
        fdk_widget_parent(meter.fill));
    fill_bounds.x = 6;
    fill_bounds.y = 6;
    fill_bounds.height = panel.height - 12;
    fill_bounds.width = ((panel.width - 12) * level) / 100;
    fdk_widget_set_bounds(meter.fill, fill_bounds);
    fdk_widget_set_background(meter.fill, meter.color);
}

/* Applies the right background for the button's current state. */
static void button_refresh(demo_button *btn) {
    fdk_widget *w = btn->widget;
    fdk_color c = btn->style.normal;
    if (fdk_widget_has_focus(w)) {
        c = btn->style.focused;
    }
    if (fdk_widget_is_hovered(w)) {
        c = btn->style.hover;
    }
    if (btn->style.is_down) {
        c = btn->style.pressed;
    }
    fdk_widget_set_background(w, c);
}

/* ---- per-button behavior ---- */

static void on_hue_click(void) {
    /* Cycle the meter color through a small palette. */
    static const fdk_color palette[] = {
        { .r = 0.15f, .g = 0.83f, .b = 0.49f, .a = 1.0f },
        { .r = 0.86f, .g = 0.30f, .b = 0.46f, .a = 1.0f },
        { .r = 0.32f, .g = 0.60f, .b = 0.94f, .a = 1.0f },
        { .r = 0.95f, .g = 0.77f, .b = 0.26f, .a = 1.0f },
    };
    static size_t next = 1;
    meter.color = palette[next];
    next = (next + 1) % (sizeof(palette) / sizeof(palette[0]));
    meter_set_level(meter.level); /* recolor via set_background */
}

static void on_grow_click(int delta) {
    meter_set_level(meter.level + delta);
}

/* Generic button event handler: hover/press visuals + the click
 * action on press-release-inside. */
static bool button_event(fdk_widget *w, const fdk_widget_event *e,
                         void *user_data) {
    demo_button *btn = user_data;
    if (btn->widget != w) {
        return false; /* shouldn't happen; defensive */
    }
    switch (e->type) {
        case FDK_WIDGET_POINTER_ENTER:
        case FDK_WIDGET_POINTER_LEAVE:
        case FDK_WIDGET_FOCUS_IN:
        case FDK_WIDGET_FOCUS_OUT:
            button_refresh(btn);
            return false; /* purely visual — let it bubble */
        case FDK_WIDGET_POINTER_DOWN:
            btn->style.is_down = true;
            button_refresh(btn);
            return true;
        case FDK_WIDGET_POINTER_UP:
            if (btn->style.is_down) {
                btn->style.is_down = false;
                button_refresh(btn);
                /* Press-then-release-inside == a click. */
                if (btn->style.armed_quit) {
                    /* THE quit-button path: destroy the window from
                     * INSIDE a widget event dispatch. The widget core
                     * defers the tree teardown and the window glue
                     * re-verifies registration — this is exactly the
                     * reentrancy scenario the Phase 4 machinery
                     * exists to make safe. */
                    app.quit = true;
                    fdk_window_destroy(app.window);
                    app.window = NULL;
                } else {
                    int tag = (int)(intptr_t)fdk_widget_get_user_data(w);
                    if (tag == 0) {
                        on_hue_click();
                    } else if (tag == 1) {
                        on_grow_click(+12);
                    } else if (tag == 2) {
                        on_grow_click(-12);
                    }
                }
            }
            return true;
        default:
            break;
    }
    return false;
}

static demo_button make_button(fdk_widget *parent, fdk_rect bounds,
                               fdk_color normal, fdk_color hover,
                               fdk_color pressed, fdk_color focused,
                               int tag, bool quit) {
    demo_button btn;
    memset(&btn, 0, sizeof(btn));
    btn.style.normal = normal;
    btn.style.hover = hover;
    btn.style.pressed = pressed;
    btn.style.focused = focused;
    btn.style.armed_quit = quit;
    fdk_widget *w = NULL;
    if (!fdk_ok(fdk_widget_create(parent, NULL, bounds, &w))) {
        fprintf(stderr, "fdk_widget_create failed\n");
        exit(1);
    }
    /* Stash the action tag on the widget; the button struct rides in
     * the callback's user_data (parented to a static array below). */
    fdk_widget_set_user_data(w, (void *)(intptr_t)tag);
    btn.widget = w;
    fdk_widget_set_corner_radius(w, 8);
    fdk_widget_set_can_focus(w, true);
    fdk_widget_set_background(w, normal);
    return btn;
}

/* Window-level events: only the ones widgets DON'T consume reach this
 * callback — Escape (no widget handled it) and the close request. */
static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user_data) {
    (void)window;
    (void)user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        app.quit = true;
        return;
    }
    if (event->type == FDK_EVENT_KEY_DOWN &&
        event->key.scancode == FDK_KEY_ESC) {
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
        .title = "FDK 03 — widget foundation",
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
    fdk_widget_set_background(root, col(24, 26, 34));

    /* Header strip with three "traffic light" dots (pure decoration,
     * but real widgets: rounded, clipped, painted by the tree walk). */
    fdk_widget *header = NULL;
    (void)fdk_widget_create(root, NULL,
                            (fdk_rect){12, 12, 536, 56}, &header);
    fdk_widget_set_background(header, col(34, 38, 52));
    fdk_widget_set_corner_radius(header, 10);
    const int dot_colors[3][3] = {
        {235, 90, 90}, {235, 190, 90}, {100, 210, 130},
    };
    for (int i = 0; i < 3; i++) {
        fdk_widget *dot = NULL;
        (void)fdk_widget_create(header, NULL,
                                (fdk_rect){12 + i * 24, 16, 24, 24}, &dot);
        fdk_widget_set_background(
            dot, col(dot_colors[i][0], dot_colors[i][1], dot_colors[i][2]));
        fdk_widget_set_corner_radius(dot, 12);
    }

    /* Three action buttons. The demo_button structs must outlive the
     * callbacks, so they live in static storage. */
    static demo_button buttons[4];
    buttons[0] = make_button(root, (fdk_rect){12, 84, 160, 48},
                             col(70, 76, 100), col(96, 104, 134),
                             col(48, 52, 70), col(88, 96, 160), 0, false);
    buttons[1] = make_button(root, (fdk_rect){184, 84, 160, 48},
                             col(70, 76, 100), col(96, 104, 134),
                             col(48, 52, 70), col(88, 96, 160), 1, false);
    buttons[2] = make_button(root, (fdk_rect){356, 84, 160, 48},
                             col(70, 76, 100), col(96, 104, 134),
                             col(48, 52, 70), col(88, 96, 160), 2, false);
    /* Quit button, visually distinct, bottom-right. */
    buttons[3] = make_button(root, (fdk_rect){388, 340, 160, 48},
                             col(160, 62, 74), col(196, 78, 92),
                             col(116, 42, 52), col(220, 96, 110), 3, true);
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        fdk_widget_set_event_callback(buttons[i].widget, button_event,
                                       &buttons[i]);
    }

    /* The meter: panel + fill child whose bounds the buttons change. */
    fdk_widget *meter_panel = NULL;
    (void)fdk_widget_create(root, NULL,
                            (fdk_rect){12, 148, 504, 176}, &meter_panel);
    fdk_widget_set_background(meter_panel, col(34, 38, 52));
    fdk_widget_set_corner_radius(meter_panel, 12);

    fdk_widget *meter_fill = NULL;
    (void)fdk_widget_create(meter_panel, NULL,
                            (fdk_rect){6, 6, 100, 164}, &meter_fill);
    fdk_widget_set_corner_radius(meter_fill, 8);
    meter.fill = meter_fill;
    meter.color = (fdk_color){ .r = 0.15f, .g = 0.83f, .b = 0.49f,
                               .a = 1.0f };
    meter.level = 20;
    meter_set_level(20);

    /* An information strip at the bottom (the "focus" indicator is
     * baked into button colors; this strip is just panel decoration
     * proving deep trees paint correctly). */
    fdk_widget *strip = NULL;
    (void)fdk_widget_create(root, NULL,
                            (fdk_rect){12, 340, 360, 48}, &strip);
    fdk_widget_set_background(strip, col(34, 38, 52));
    fdk_widget_set_corner_radius(strip, 10);

    /* The whole application loop: pump, then paint-if-damaged. */
    fdk_window_show(app.window);
    (void)fdk_window_paint(app.window);

    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(ctx, 15);
        if (app.quit) {
            break; /* the quit button already destroyed the window */
        }
        if (app.window == NULL) {
            break;
        }
        /* X11: always ready; Wayland: paced by compositor callbacks. */
        fdk_surface *surface = NULL;
        if (fdk_ok(fdk_window_get_surface(app.window, &surface)) &&
            !fdk_surface_frame_ready(surface)) {
            continue;
        }
        (void)fdk_window_paint(app.window);
        frames++;
    }

    printf("03_widgets: exited cleanly after %d frames\n", frames);

    /* ESC / close-request paths leave the window for us; the quit
     * button path already destroyed it (and NULLed the handle). */
    if (app.window != NULL) {
        fdk_window_destroy(app.window);
        app.window = NULL;
    }
    fdk_shutdown(ctx);
    return 0;
}
