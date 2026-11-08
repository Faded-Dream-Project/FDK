/* 21_scale_button.c — fdk_scale_button: the slider-in-a-popup control
 * (the GtkScaleButton/GtkVolumeButton row, FDK's own machinery).
 *
 * A window with a scale button over 0..100 (step 5) — the volume
 * posture: the button's face is a value meter, a press pops a small
 * window below it (above when the room runs out) carrying a vertical
 * slider and a live value label; every drag updates THIS window's
 * readout live, and Escape / a second press / clicking away all
 * close the popup. The honest outcomes print on the status line as
 * the value moves. */
#include "example_window.h"

static fdk_example ex;
static fdk_widget *g_readout;
static fdk_widget *g_value_label;

static void scale_changed(fdk_widget *button, double value, void *user) {
    (void)button;
    (void)user;
    char buf[64];
    (void)snprintf(buf, sizeof(buf), "volume: %.0f%%", value);
    fdk_label_set_text(g_value_label, buf);
    (void)snprintf(buf, sizeof(buf),
                   "scale button: the popup slider drives this "
                   "readout live (%.0f)", value);
    fdk_example_set_status(&ex, buf);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "21")) {
        return 1;
    }
    if (!fdk_example_open(&ex, ctx, "21",
                          "scale button (the slider-in-a-popup control)",
                          380, 190)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_font *font = ex.ui;

    fdk_widget *row = NULL;
    fdk_box_create(ex.content, FDK_HORIZONTAL, &row);
    fdk_box_set_padding(row, 12);
    fdk_box_set_spacing(row, 12);

    fdk_widget *sb = NULL;
    fdk_scale_button_create(row, font, 0.0, 100.0, 35.0, &sb);
    fdk_scale_button_set_step(sb, 5.0);
    fdk_scale_button_set_on_value_changed(sb, scale_changed, NULL);

    char buf[64];
    (void)snprintf(buf, sizeof(buf), "volume: %.0f%%", 35.0);
    fdk_label_create(row, font, buf, &g_value_label);
    fdk_widget_set_expand(g_value_label, true, false);

    fdk_label_create(ex.content, font,
                     "Press the meter button: drag the popup's "
                     "vertical slider (live), Escape or a second "
                     "press closes it.",
                     &g_readout);
    fdk_widget_set_natural_size(g_readout, 350, 40);

    fdk_example_set_status(&ex,
                           "the button face is the value meter; the "
                           "popup owns the slider");
    fdk_example_run(&ex);
    fdk_shutdown(ctx);
    return 0;
}
