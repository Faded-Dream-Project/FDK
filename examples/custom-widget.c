/* custom-widget.c — P5(c): a custom widget class with per-instance
 * fields, public API only. The subclass embeds fdk_widget first; the
 * class vtable carries struct_size + instance_size so fdk_widget_create
 * makes ONE allocation covering both; fdk_widget_instance() reaches
 * the fields. The paint hook draws the per-instance counter.
 */
#include <fdk/fdk.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    fdk_widget_embed widget; /* MUST be first (see fdk_widget.h) */
    int clicks;        /* per-instance field */
    float hue;         /* per-instance field */
} counter_widget;

static const fdk_widget_class g_counter_class;

static void counter_init(fdk_widget *w) {
    counter_widget *cw = FDK_WIDGET_INSTANCE(w, counter_widget);
    cw->clicks = 0;
    cw->hue = 0.55f;
}

static void counter_paint(fdk_widget *w, fdk_surface *s, fdk_rect cell,
                          fdk_rect clip) {
    (void)clip;
    counter_widget *cw = FDK_WIDGET_INSTANCE(w, counter_widget);
    fdk_color bg = { cw->hue, 0.4f, 0.6f, 1.0f };
    fdk_surface_fill_rect(s, cell, bg);
    /* the instance field drives the visual: one column per click */
    for (int i = 0; i < cw->clicks && i < 16; i++) {
        fdk_surface_fill_rect(
            s, (fdk_rect){ cell.x + 4 + i * 8, cell.y + cell.height - 12,
                           6, 8 },
            (fdk_color){ 1.0f, 1.0f, 1.0f, 1.0f });
    }
}

static const fdk_widget_class g_counter_class = {
    .struct_size = sizeof(fdk_widget_class),
    .size = sizeof(counter_widget),
    .instance_init = counter_init,
    .name = "counter",
    .paint = counter_paint,
};

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("custom-widget: no context\n");
        return 77;
    }
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.render_mode = FDK_RENDER_CPU;
    o.title = "fdk-custom-widget";
    o.width = 320;
    o.height = 200;
    fdk_window *w = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &o, &w))) {
        printf("custom-widget: WALL — window create failed\n");
        return 77;
    }
    fdk_widget *root = NULL;
    if (!fdk_ok(fdk_window_get_root(w, &root))) {
        printf("custom-widget: WALL — no root widget\n");
        return 1;
    }
    fdk_widget *counter = NULL;
    if (!fdk_ok(fdk_widget_create(root, &g_counter_class,
                                  (fdk_rect){ 20, 20, 160, 80 },
                                  &counter))) {
        printf("custom-widget: WALL — widget create failed\n");
        return 1;
    }
    /* drive the per-instance field, then paint through the window */
    counter_widget *cw = FDK_WIDGET_INSTANCE(counter, counter_widget);
    cw->clicks = 5;
    fdk_window_show(w);
    for (int i = 0; i < 5; i++) {
        fdk_pump_events(ctx, 16);
    }
    fdk_window_paint(w);
    printf("custom-widget: instance clicks=%d hue=%.2f painted\n",
           cw->clicks, (double)cw->hue);
    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    return 0;
}
