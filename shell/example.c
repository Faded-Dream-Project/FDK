/*
 * shell/example.c — the fdk-shell demonstration: a top bar that
 * reserves its exclusive zone, drawn panels on it, and a popup
 * menu anchored to it (the popup exercises the layer-shell
 * get_popup adoption on Wayland and plain X parenting on X11).
 *
 * Note on text: extension surfaces render through the raw
 * fdk_surface API, whose public surface has no text primitives
 * (text lives in FDK's widget layer; docs/PARITY.md tracks
 * "widget hosting on extension surfaces" as the gap that would
 * close this). The example draws geometry — a bar, a launcher
 * block, a clock block — which is exactly what a shell's chrome
 * is underneath.
 *
 * Build: make shell-example   (links FDK + the shell add-on)
 * Run:   ./build/shell/example
 */

#include "fdk/fdk_core.h"
#include "fdk/fdk_error.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_types.h"

#include "fdk_shell.h"

#include <stdio.h>

static fdk_ext_surface *g_bar;
static fdk_ext_surface *g_popup;

static void draw_bar(void)
{
    fdk_surface *s;
    fdk_surface_info info;
    if (!fdk_ok(fdk_ext_surface_get_surface(g_bar, &s)) ||
        !fdk_ok(fdk_surface_get_info(s, &info)))
        return;

    fdk_surface_fill(s, (fdk_color){0x14, 0x14, 0x1c, 0xff});
    /* The launcher block (left). */
    fdk_surface_fill_rect(s, (fdk_rect){8, 8, 16, 16},
                          (fdk_color){0x35, 0x84, 0xe4, 0xff});
    /* The clock block (right). */
    fdk_surface_fill_rect(s, (fdk_rect){info.width - 56, 8, 48, 16},
                          (fdk_color){0xd8, 0xd8, 0xe8, 0xff});
    /* The task area separators (middle). */
    for (int i = 0; i < 4; i++)
        fdk_surface_fill_rect(s, (fdk_rect){64 + i * 40, 10, 28, 12},
                              (fdk_color){0x2a, 0x2a, 0x38, 0xff});
    fdk_surface_present(s);
}

static void on_popup_event(fdk_ext_surface *popup,
                           const fdk_event_data *event, void *user)
{
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST)
        fdk_ext_surface_destroy(popup);
    (void)user;
}

static void on_bar_event(fdk_ext_surface *bar,
                         const fdk_event_data *event, void *user)
{
    switch (event->type) {
    case FDK_EVENT_WINDOW_CONFIGURE:
    case FDK_EVENT_WINDOW_EXPOSE:
        draw_bar();
        break;
    case FDK_EVENT_POINTER_BUTTON_DOWN:
        /* A menu anchored on the bar (the popup-adoption path). */
        if (!g_popup) {
            if (fdk_ok(fdk_ext_popup_create(bar, 8, 32, 160, 120,
                                            &g_popup)) &&
                g_popup) {
                fdk_ext_surface_set_event_callback(g_popup,
                                                    on_popup_event,
                                                    NULL);
                fdk_surface *s;
                if (fdk_ok(fdk_ext_surface_get_surface(g_popup,
                                                       &s))) {
                    fdk_surface_fill(
                        s, (fdk_color){0x22, 0x22, 0x30, 0xff});
                    fdk_surface_present(s);
                }
            }
        } else {
            fdk_ext_surface_destroy(g_popup);
            g_popup = NULL;
        }
        break;
    default:
        break;
    }
    (void)user;
}

int main(void)
{
    fdk_context *ctx = NULL;
    fdk_result rc = fdk_init(&ctx, NULL);
    if (!fdk_ok(rc) || !ctx) {
        fprintf(stderr, "fdk_init failed: %s\n",
                fdk_result_to_string(rc));
        return 1;
    }

    fdk_shell_overlay_cfg cfg = {0};
    cfg.struct_size = sizeof cfg;
    cfg.layer = FDK_SHELL_LAYER_TOP;
    cfg.anchor = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                 FDK_SHELL_ANCHOR_RIGHT;
    cfg.exclusive = 32; /* the bar's strip: normal windows dodge  */
    cfg.size = 32;

    rc = fdk_shell_overlay_create(ctx, &cfg, &g_bar);
    if (!fdk_ok(rc)) {
        fprintf(stderr, "fdk_shell_overlay_create: %s\n",
                fdk_result_to_string(rc));
        fdk_shutdown(ctx);
        return 1;
    }
    printf("fdk-shell: bar created on the %s backend\n",
           fdk_shell_overlay_backend(g_bar) == FDK_EXT_BACKEND_WAYLAND
               ? "wayland"
               : "x11");

    fdk_ext_surface_set_event_callback(g_bar, on_bar_event, NULL);
    fdk_ext_surface_set_cursor(g_bar, FDK_CURSOR_DEFAULT);
    fdk_ext_surface_show(g_bar);

    fdk_run(ctx); /* the event loop; fdk_quit() exits it        */

    if (g_popup)
        fdk_ext_surface_destroy(g_popup);
    fdk_ext_surface_destroy(g_bar);
    fdk_shutdown(ctx);
    return 0;
}
