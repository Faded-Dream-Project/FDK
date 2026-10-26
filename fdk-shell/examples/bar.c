/* bar.c — the fdk-shell demo (0.1.0).
 *
 * A plain colored bar docked to the top of the screen with one
 * working dropdown — the smallest honest demonstration of everything
 * fdk-shell gives a panel: the layer handshake (anchored, reserving
 * its own space), rendering through FDK's ordinary public surface
 * API, real pointer events on a layer surface, and a popup
 * (dropdown) parented to it with the full grab/dismissal contract.
 *
 * Build:  make examples            (links fdk + fdk-shell)
 * Run:    ./build/examples/bar    (any desktop; Wayland compositor
 *                                  with wlr-layer-shell or any X11)
 *         ./build/examples/bar 5   (auto-quit after 5 seconds — the
 *                                  rigs' headless shape)
 *
 * Every pixel is plain fill math — no fonts, no theme, nothing but
 * the API under test: what you see is exactly what the extension
 * surface machinery can do.
 */

#include <fdk-shell/fdk-shell.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- palette ------------------------------------------------------------ */

static const fdk_color BAR_BG = { .r = 0.13f, .g = 0.15f, .b = 0.19f,
                                  .a = 1.0f };
static const fdk_color BAR_BG_HOVER = { .r = 0.22f, .g = 0.25f, .b = 0.32f,
                                        .a = 1.0f };
static const fdk_color ACCENT = { .r = 0.30f, .g = 0.62f, .b = 0.95f,
                                  .a = 1.0f };
static const fdk_color ITEM_A = { .r = 0.16f, .g = 0.35f, .b = 0.24f,
                                  .a = 1.0f };
static const fdk_color ITEM_B = { .r = 0.35f, .g = 0.16f, .b = 0.24f,
                                  .a = 1.0f };
static const fdk_color ITEM_C = { .r = 0.24f, .g = 0.16f, .b = 0.35f,
                                  .a = 1.0f };
static const fdk_color POPUP_BG = { .r = 0.10f, .g = 0.11f, .b = 0.14f,
                                    .a = 1.0f };

/* ---- state ---------------------------------------------------------------- */

typedef struct {
    fdk_context *ctx;
    fdk_shell *shell;
    fdk_shell_layer *bar;

    /* The bar's hover state (the button region). */
    int hover_button; /* pointer over the "Menu" region       */
    int pressed_button;

    /* The dropdown. */
    fdk_shell_popup *popup;
    int popup_item_hover; /* -1 none, 0..2 over an item        */
    int selected;         /* the last chosen item (rendered as
                           * an accent swatch in the bar)      */

    int running;
    fdk_size bar_size;
} bar_app;

static const int MENU_X = 8;       /* the button's bar-local origin */
static const int MENU_W = 96;
static const int MENU_H = 24;      /* inside a 32px bar             */

static const int POPUP_ITEM_H = 28;
static const int POPUP_W = 180;
static const int POPUP_H = 3 * POPUP_ITEM_H + 8;

/* ---- rendering --------------------------------------------------------------- */

static void bar_paint(bar_app *app) {
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_layer_get_surface(app->bar, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    app->bar_size.width = info.width;
    app->bar_size.height = info.height;

    /* The bar field. The background NEVER changes with hover (the
     * 1.1.0-rc1 W3 report: the demo used to repaint the whole bar in
     * its hover tint, so hovering the button lit the ENTIRE bar —
     * correct hit-testing made it look like a pointer-path bug). Only
     * the button chip carries hover state, the way every real panel
     * does. */
    fdk_surface_fill(s, BAR_BG);

    /* The "Menu" button: a rounded chip with an accent underline
     * while hovered/pressed. */
    fdk_color chip = app->pressed_button ? ACCENT
                     : app->hover_button  ? BAR_BG_HOVER
                                          : BAR_BG;
    fdk_surface_fill_rounded_rect(
        s, (fdk_rect){ .x = MENU_X, .y = 4, .width = MENU_W,
                       .height = MENU_H },
        6, chip);
    fdk_surface_fill_rect(
        s, (fdk_rect){ .x = MENU_X + 8, .y = 4 + MENU_H / 2 - 3,
                       .width = 26, .height = 6 },
        ACCENT);
    fdk_surface_fill_rect(
        s, (fdk_rect){ .x = MENU_X + 40, .y = 4 + MENU_H / 2 - 3,
                       .width = 16, .height = 6 },
        ACCENT);
    fdk_surface_fill_rect(
        s, (fdk_rect){ .x = MENU_X + 62, .y = 4 + MENU_H / 2 - 3,
                       .width = 8, .height = 6 },
        ACCENT);

    /* The selected item's swatch (proof the dropdown works). */
    if (app->selected >= 0) {
        static const fdk_color items[3] = { 0 };
        fdk_color cols[3] = { ITEM_A, ITEM_B, ITEM_C };
        (void)items;
        fdk_surface_fill_rounded_rect(
            s, (fdk_rect){ .x = MENU_X + MENU_W + 16, .y = 8,
                           .width = 16, .height = 16 },
            4, cols[app->selected]);
    }

    /* A right-edge clock strip — a plain accent block (the bar is
     * "colored", per the demo's contract; no font dependency). */
    fdk_surface_fill_rect(
        s, (fdk_rect){ .x = info.width - 96, .y = 8, .width = 80,
                       .height = 16 },
        ACCENT);

    (void)fdk_surface_present(s);
}

static void popup_paint(bar_app *app) {
    if (app->popup == NULL) {
        return;
    }
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_shell_popup_get_surface(app->popup, &s))) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(s, &info))) {
        return;
    }
    fdk_surface_fill(s, POPUP_BG);

    static const fdk_color items[3] = { { .r = 0.16f, .g = 0.35f,
                                          .b = 0.24f, .a = 1.0f },
                                        { .r = 0.35f, .g = 0.16f,
                                          .b = 0.24f, .a = 1.0f },
                                        { .r = 0.24f, .g = 0.16f,
                                          .b = 0.35f, .a = 1.0f } };
    for (int i = 0; i < 3; i++) {
        fdk_color c = items[i];
        if (app->popup_item_hover == i) {
            c.r *= 1.6f;
            c.g *= 1.6f;
            c.b *= 1.6f;
            if (c.r > 1.0f) c.r = 1.0f;
            if (c.g > 1.0f) c.g = 1.0f;
            if (c.b > 1.0f) c.b = 1.0f;
        }
        fdk_surface_fill_rounded_rect(
            s, (fdk_rect){ .x = 8, .y = 4 + i * POPUP_ITEM_H,
                           .width = info.width - 16,
                           .height = POPUP_ITEM_H - 6 },
            5, c);
    }
    (void)fdk_surface_present(s);
}

/* ---- events -------------------------------------------------------------------- */

static void bar_open_popup(bar_app *app) {
    if (app->popup != NULL) {
        return; /* already open */
    }
    /* Anchored right under the Menu button. */
    fdk_result r = fdk_shell_popup_create(app->bar, MENU_X,
                                          app->bar_size.height > 0
                                              ? app->bar_size.height
                                              : 32,
                                          POPUP_W, POPUP_H, &app->popup);
    if (!fdk_ok(r)) {
        fprintf(stderr, "bar: popup create failed (%d)\n", r);
        app->popup = NULL;
        return;
    }
    app->popup_item_hover = -1;
    fdk_shell_popup_show(app->popup);
    popup_paint(app);
}

static void bar_close_popup(bar_app *app) {
    if (app->popup == NULL) {
        return;
    }
    fdk_shell_popup_destroy(app->popup);
    app->popup = NULL;
    bar_paint(app); /* repaint the (un-pressed) button state */
}

static void popup_event(fdk_shell_popup *popup, const fdk_event_data *ev,
                        void *user) {
    bar_app *app = user;
    switch (ev->type) {
        case FDK_EVENT_WINDOW_EXPOSE:
            popup_paint(app); /* same contract as the bar's expose */
            break;
        case FDK_EVENT_POINTER_MOTION:
        case FDK_EVENT_POINTER_ENTER: {
            /* Repaint ONLY when the hovered item changes (W1 (c)): the
             * old code repainted on EVERY motion event — an unbounded
             * commit rate the compositor never asked for, and the fuel
             * of the buffer-in-flight spiral. Enter seeds hover the
             * same way (position included). */
            int hover =
                (ev->pointer.position.y >= 4.0f &&
                 ev->pointer.position.y <
                     4.0f + 3.0f * (fdk_f32)POPUP_ITEM_H)
                    ? (int)((ev->pointer.position.y - 4.0f) /
                            (fdk_f32)POPUP_ITEM_H)
                    : -1;
            if (hover != app->popup_item_hover) {
                app->popup_item_hover = hover;
                /* The hand over a clickable item, the arrow over the
                 * popup's padding. */
                fdk_shell_popup_set_cursor(
                    app->popup, hover >= 0 ? FDK_SHELL_CURSOR_POINTER
                                           : FDK_SHELL_CURSOR_DEFAULT);
                popup_paint(app);
            }
            break;
        }
        case FDK_EVENT_POINTER_BUTTON_DOWN:
            if (app->popup_item_hover >= 0) {
                app->selected = app->popup_item_hover;
                bar_close_popup(app); /* a choice was made */
            }
            break;
        case FDK_EVENT_WINDOW_CLOSE_REQUEST:
            /* Outside click or Escape — the dismissal contract. */
            (void)popup;
            bar_close_popup(app);
            break;
        default:
            break;
    }
}

static void bar_event(fdk_shell_layer *layer, const fdk_event_data *ev,
                      void *user) {
    bar_app *app = user;
    (void)layer;
    switch (ev->type) {
        case FDK_EVENT_WINDOW_CONFIGURE:
            bar_paint(app); /* the compositor sized us: repaint */
            break;
        case FDK_EVENT_WINDOW_EXPOSE:
            /* The ghost-bar fix (found live, 2026-10-25): an EXPOSE
             * means the server no longer retains some region of the
             * window — a desktop switch on a WM without sticky
             * support, a fullscreen window that covered us closing,
             * an unocclusion. fdk_event.h's contract for rendered
             * surfaces is "re-present"; the retained-damage model
             * would otherwise conclude nothing changed and paint
             * NOTHING, leaving whatever was underneath showing in
             * the bar's region. Re-paint on every expose (X sends
             * them in bursts; painting the whole bar is the cheap,
             * correct answer — same shape FDK's own examples use). */
            bar_paint(app);
            if (app->popup != NULL) {
                popup_paint(app); /* it can be exposed too (a window
                                   * raised over it, then lowered) */
            }
            break;
        case FDK_EVENT_POINTER_MOTION:
        case FDK_EVENT_POINTER_ENTER: {
            /* ENTER carries its surface-local position (fdk_event.h);
             * entering directly onto the button must highlight it
             * WITHOUT waiting for a first motion (the W3 report's
             * "hover state set on enter before any motion" — the
             * demo's old enter-blindness made re-entry look dead). */
            fdk_f32 x = ev->pointer.position.x;
            fdk_f32 y = ev->pointer.position.y;
            int over = (x >= MENU_X && x < MENU_X + MENU_W && y >= 4 &&
                        y < 4 + MENU_H);
            if (over != app->hover_button) {
                app->hover_button = over;
                /* W2: the hand over the clickable button, the arrow
                 * everywhere else — the cursor tracks hover. */
                fdk_shell_layer_set_cursor(
                    app->bar, over ? FDK_SHELL_CURSOR_POINTER
                                   : FDK_SHELL_CURSOR_DEFAULT);
                bar_paint(app);
            }
            break;
        }
        case FDK_EVENT_POINTER_LEAVE:
            if (app->hover_button) {
                app->hover_button = 0;
                app->pressed_button = 0;
                fdk_shell_layer_set_cursor(app->bar,
                                           FDK_SHELL_CURSOR_DEFAULT);
                bar_paint(app);
            }
            break;
        case FDK_EVENT_POINTER_BUTTON_DOWN:
            if (app->hover_button) {
                app->pressed_button = 1;
                bar_paint(app);
            }
            break;
        case FDK_EVENT_POINTER_BUTTON_UP:
            if (app->pressed_button && app->hover_button) {
                app->pressed_button = 0;
                if (app->popup == NULL) {
                    bar_open_popup(app);
                    if (app->popup != NULL) {
                        fdk_shell_popup_set_event_callback(app->popup,
                                                           popup_event,
                                                           app);
                    }
                } else {
                    bar_close_popup(app); /* toggle */
                }
            } else {
                app->pressed_button = 0;
                bar_paint(app);
            }
            break;
        default:
            break;
    }
}

/* ---- main ---------------------------------------------------------------------- */

int main(int argc, char **argv) {
    long auto_quit_s = -1;
    if (argc > 1) {
        auto_quit_s = strtol(argv[1], NULL, 10);
    }

    /* The add-on's version gate (the documented pattern). */
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0) ||
        fdk_ext_api_version() < 1) {
        fprintf(stderr, "bar: FDK 1.1.0 (extension API v1) required\n");
        return 1;
    }

    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "bar: no display\n");
        return 1;
    }

    bar_app app;
    memset(&app, 0, sizeof(app));
    app.ctx = ctx;
    app.selected = -1;
    app.running = 1;

    fdk_result r = fdk_shell_init(ctx, &app.shell);
    if (r == FDK_ERR_UNSUPPORTED) {
        fprintf(stderr,
                "bar: this backend offers no layer surfaces "
                "(Wayland compositor without wlr-layer-shell?)\n");
        fdk_shutdown(ctx);
        return 1;
    }
    if (!fdk_ok(r)) {
        fprintf(stderr, "bar: fdk_shell_init failed (%d)\n", r);
        fdk_shutdown(ctx);
        return 1;
    }

    /* The bar: top edge, full width, 32 logical pixels tall, auto
     * exclusive zone (maximized windows stop underneath it). */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors =
        FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT | FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    desc.exclusive_zone = -1;

    r = fdk_shell_layer_create(app.shell, &desc, &app.bar);
    if (!fdk_ok(r)) {
        fprintf(stderr, "bar: layer create failed (%d)\n", r);
        fdk_shell_shutdown(app.shell);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell_layer_set_event_callback(app.bar, bar_event, &app);
    fdk_shell_layer_show(app.bar);
    bar_paint(&app);

    /* The loop: pump; paint on demand (every event paints itself —
     * the demo has no animation). A FATAL pump result (the display
     * connection died — FDK_ERR_NO_DISPLAY and friends) breaks the
     * loop for cleanup and a NON-ZERO exit: the documented contract
     * for application-owned loops (a negative pump result is fatal;
     * the core logs it once and paces repeats, this side must exit).
     * Found live on labwc 2026-10-26: the protocol-killed connection
     * used to spin here forever at 100% CPU. */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int fatal = 0;
    while (app.running) {
        fdk_result pr = fdk_pump_events(ctx, 100);
        if (pr < 0) {
            /* Negative = fatal (the display connection died —
             * FDK_ERR_NO_DISPLAY and friends; a positive count is
             * dispatched events, zero is a quiet timeout). */
            fprintf(stderr,
                    "bar: the display connection died (%d) — exiting\n",
                    (int)pr);
            fatal = 1;
            app.running = 0;
            break;
        }

        if (auto_quit_s > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed = now.tv_sec - start.tv_sec;
            if (elapsed >= auto_quit_s) {
                app.running = 0;
            }
        }
    }

    bar_close_popup(&app);
    fdk_shell_layer_destroy(app.bar);
    fdk_shell_shutdown(app.shell);
    fdk_shutdown(ctx);
    if (fatal) {
        return 1;
    }
    printf("bar: done (selected item: %d)\n", app.selected);
    return 0;
}
