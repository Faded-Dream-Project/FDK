/*
 * 01_hello_world.c — the smallest possible FDK program that opens a
 * real window, now via the shared example helper (see
 * example_window.h): fdk_example_init() connects to a real X11 or
 * Wayland display, fdk_example_open() builds the standard example
 * window (in-window header, content box, status line), and
 * fdk_example_run() pumps until quit.
 *
 * The demo still teaches the event contract: ex->on_event is the
 * helper's observe-only hook — window resizes and key presses are
 * logged to stdout and mirrored into the status line, while the
 * HELPER owns the quit semantics (close request and Escape both end
 * the loop; teardown happens in fdk_example_close() in the documented
 * order: window, then context).
 *
 * The blocking fdk_run() variant (one call, no per-frame control) is
 * documented in fdk_core.h; the examples use the pump loop because
 * they need the per-pass paint slot.
 *
 * Build: make examples
 * Run:   ./build/examples/01_hello_world
 */

#include "example_window.h"

#include <stdio.h>

static void on_event(fdk_window *window, const fdk_event_data *event,
                     void *user) {
    fdk_example *ex = user;
    (void)window;

    switch (event->type) {
        case FDK_EVENT_WINDOW_CLOSE_REQUEST:
            printf("close requested, shutting down\n");
            break;

        case FDK_EVENT_WINDOW_CONFIGURE:
            printf("resized to %dx%d\n", event->configure.size.width,
                   event->configure.size.height);
            fdk_example_set_status(ex, "resized — the toolkit root "
                                       "tracks the new size");
            break;

        case FDK_EVENT_KEY_DOWN:
            if (event->key.codepoint != 0) {
                printf("key pressed: '%c' (scancode %u)\n",
                       (char)event->key.codepoint, event->key.scancode);
                fdk_example_set_status(ex, "key pressed — try Escape");
            } else {
                printf("key pressed: scancode %u (no printable codepoint)\n",
                       event->key.scancode);
                fdk_example_set_status(ex, "key pressed — try Escape");
            }
            break;

        default:
            break; /* other event types not of interest to this example */
    }
}

int main(void) {
    printf("Faded Dream ToolKit %s\n", fdk_get_version_string());

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "01")) {
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "01", "hello world", 640, 480)) {
        fdk_shutdown(ctx);
        return 1;
    }
    ex.on_event = on_event;
    ex.on_event_user = &ex;

    fdk_example_set_status(&ex, "window shown — close it or press Esc");
    printf("window shown — close it or press Esc to exit\n");

    fdk_example_run(&ex);
    fdk_example_close(&ex);
    return 0;
}
