/* examples/d-input-method.c — P5 (d), the FDK-native input-method
 * add-on: the on-screen-keyboard half of the text-input conversation.
 *
 * NO raw-event surface, NO hand-rolled seat binding. The public path:
 *
 *   fdk_ext_wl_seat              — the connection's seat (the
 *     documented accessor; the opt-in fdk/ext/wayland.h),
 *   fdk_input_method_open        — the zwp_input_method_v2 object,
 *   fdk_input_method_grab_keyboard, fdk_input_method_commit,
 *   fdk_input_method_close.
 *
 * The protocol's events (activate/deactivate/done) are the ADD-ON's
 * to listen for — the returned proxy is raw by documented design
 * ("the add-on sets ITS OWN listeners on the returned raw proxy"),
 * so this example carries its own copy of the zwp_input_method_v2
 * listener header (examples/protocols/ — the standard Wayland
 * client practice for protocols the toolkit does not model). The
 * activate path fires when a text-input-v3 client focuses; the
 * done event applies the latched state and gates the commit.
 *
 * Wall check (P5): every wall this hits is an FDK bug to fix.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "fdk/ext/wayland.h" /* the opt-in escape hatch: the seat
                              * accessor (this example speaks the IM
                              * protocol's own listener contract) */
#include "protocols/input-method-unstable-v2-client-protocol.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client-core.h>

static int g_activated = 0;
static int g_commits = 0;
static int g_deactivations = 0;
static uint32_t g_done_serial = 0;

/* ---- the zwp_input_method_v2 listener (the add-on owns it) ---- */
static void im_activate(void *data, struct zwp_input_method_v2 *im) {
    (void)data;
    (void)im;
    g_activated = 1;
    printf("IM: activated (a text-input client took focus)\n");
}

static void im_deactivate(void *data, struct zwp_input_method_v2 *im) {
    (void)data;
    (void)im;
    g_activated = 0;
    g_deactivations++;
    printf("IM: deactivated\n");
}

static void im_surrounding(void *data, struct zwp_input_method_v2 *im,
                           const char *text, uint32_t cursor,
                           uint32_t anchor) {
    (void)data;
    (void)im;
    (void)text;
    (void)cursor;
    (void)anchor;
}

static void im_content_type(void *data, struct zwp_input_method_v2 *im,
                            uint32_t hint, uint32_t purpose) {
    (void)data;
    (void)im;
    (void)hint;
    (void)purpose;
}

static void im_unavailable(void *data, struct zwp_input_method_v2 *im) {
    (void)data;
    (void)im;
    printf("IM: unavailable (another IM holds the seat)\n");
}

static void im_done(void *data, struct zwp_input_method_v2 *im) {
    (void)data;
    /* the latched state applied: v2's done carries no serial — the
     * commit request's serial is the count of done events so far */
    g_done_serial++;
    if (g_activated && g_commits < 1) {
        zwp_input_method_v2_commit_string(im,
                                          "hello from the FDK IM add-on");
        zwp_input_method_v2_commit(im, g_done_serial);
        g_commits++;
        printf("IM: committed a string (serial %u)\n", g_done_serial);
    }
}

static const struct zwp_input_method_v2_listener g_im_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
    .surrounding_text = im_surrounding,
    .text_change_cause = NULL,
    .content_type = im_content_type,
    .done = im_done,
    .unavailable = im_unavailable,
};

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    if (!fdk_ext_has_capability(ctx, FDK_EXT_CAP_INPUT_METHOD_V2)) {
        fprintf(stderr, "SKIP: no input-method capability\n");
        fdk_shutdown(ctx);
        return 77;
    }
    struct wl_seat *seat = fdk_ext_wl_seat(ctx, 0);
    if (seat == NULL) {
        fprintf(stderr, "SKIP: no connection seat\n");
        fdk_shutdown(ctx);
        return 77;
    }

    /* the input-method object on FDK's own seat */
    struct zwp_input_method_v2 *im =
        fdk_input_method_open(ctx, seat);
    if (im == NULL) {
        fprintf(stderr, "FAIL: input method refused\n");
        fdk_shutdown(ctx);
        return 1;
    }
    zwp_input_method_v2_add_listener(im, &g_im_listener, NULL);
    printf("IM: zwp_input_method_v2 minted on FDK's seat\n");

    /* the grab: while we hold it, key events route to the grab
     * object; a real keyboard forwards them to its UI. */
    void *grab = fdk_input_method_grab_keyboard(im);
    printf("IM: keyboard grab %s\n", grab != NULL ? "taken" : "refused");

    for (int round = 0; round < 60; round++) {
        fdk_pump_events(ctx, 16);
    }
    printf("IM: %d commits, %d deactivations; tearing down\n", g_commits,
           g_deactivations);

    fdk_input_method_close(im);
    fdk_shutdown(ctx);
    printf("IM example: clean exit\n");
    return 0;
}
