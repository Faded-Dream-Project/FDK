/*
 * 01_hello_world.c — the smallest possible FDK program that opens a
 * real window.
 *
 * Requires a reachable X11 or Wayland display to run (fdk_init()
 * genuinely connects to one as of Phase 2 — see docs/roadmap.md).
 * There is no renderer yet (Phase 3), so the window will appear with
 * whatever blank/undefined content the platform gives an unbuffered
 * surface — on X11 that's typically the border/background color set
 * at creation; on Wayland, nothing is shown until Phase 3 attaches a
 * buffer (see fdk_wayland_window_show()'s doc comment in
 * src/platform/wayland/wayland_window.c for why). The window still
 * genuinely exists, receives real events, and responds to being
 * closed or resized — that's what this example demonstrates.
 *
 * Close the window (title bar close button, or Alt+F4/equivalent) to
 * exit; the example demonstrates the documented pattern of NOT
 * auto-destroying on FDK_EVENT_WINDOW_CLOSE_REQUEST (see fdk_event.h)
 * — it logs the request and then destroys the window itself.
 *
 * Build: make examples
 * Run:   ./build/examples/01_hello_world
 */

#include "fdk/fdk.h"

#include <stdio.h>

static void on_event(fdk_window *window, const fdk_event_data *event, void *user_data) {
    fdk_context *ctx = user_data;

    switch (event->type) {
        case FDK_EVENT_WINDOW_CLOSE_REQUEST:
            printf("close requested, shutting down\n");
            fdk_window_destroy(window);
            fdk_quit(ctx);
            break;

        case FDK_EVENT_WINDOW_CONFIGURE:
            printf("resized to %dx%d\n", event->configure.size.width,
                   event->configure.size.height);
            break;

        case FDK_EVENT_KEY_DOWN:
            if (event->key.codepoint != 0) {
                printf("key pressed: '%c' (scancode %u)\n",
                       (char)event->key.codepoint, event->key.scancode);
            } else {
                printf("key pressed: scancode %u (no printable codepoint)\n",
                       event->key.scancode);
            }
            break;

        default:
            break; /* other event types not of interest to this example */
    }
}

int main(void) {
    printf("Faded Dream ToolKit %s\n", fdk_get_version_string());

    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, NULL);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_init failed: %s\n", fdk_result_to_string(r));
        fprintf(stderr, "(this example needs a real X11 or Wayland display to run)\n");
        return 1;
    }

    fdk_window *window = NULL;
    fdk_window_options opts = {
        .title = "FDK Hello World",
        .width = 640,
        .height = 480,
    };
    r = fdk_window_create(ctx, &opts, &window);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_create failed: %s\n", fdk_result_to_string(r));
        fdk_shutdown(ctx);
        return 1;
    }

    fdk_window_set_event_callback(window, on_event, ctx);
    fdk_window_show(window);

    printf("window shown — close it to exit\n");
    fdk_run(ctx);

    fdk_shutdown(ctx);
    return 0;
}
