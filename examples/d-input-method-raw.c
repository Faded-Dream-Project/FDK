/* examples/d-input-method-raw.c — the input-method add-on, the RAW
 * escape hatch (see d-input-method.c for the FDK-native shape: the
 * seat arrives from fdk_ext_wl_seat, no raw queue needed). THIS
 * example shows the full add-on machinery the first-class path
 * hides:
 *   fdk_wl_queue_create      — the add-on's own event queue
 *   fdk_wl_queue_registry    — the private registry on that queue
 *   fdk_wl_raw_bind          — wl_seat bound onto the queue
 *   fdk_input_method_open    — the zwp_input_method_v2 object
 *   fdk_input_method_grab_keyboard, fdk_input_method_commit
 *
 * The add-on vendors its copy of the zwp_input_method_v2 protocol
 * header (examples/protocols/ — standard Wayland client practice for
 * protocols the toolkit does not model) and sets the FULL listener:
 * activate grabs the keyboard, done commits a canned string, just
 * like a real virtual keyboard. Text-input-v3 clients route their
 * traffic to this object while it holds the seat's IM slot.
 *
 * Wall check (P5): every wall this hits is an FDK bug to fix.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "protocols/input-method-unstable-v2-client-protocol.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

static fdk_context *g_ctx = NULL;
static fdk_wl_queue *g_q = NULL;
static struct zwp_input_method_v2 *g_im = NULL;
static void *g_grab = NULL;
static int g_activated = 0;
static int g_commits = 0;
static int g_deactivations = 0;
static uint32_t g_im_serial = 0;

static int g_seat_name = 0;
static int g_have_seat = 0;

static void raw_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)data;
    (void)r;
    (void)version;
    if (strcmp(iface, "wl_seat") == 0) {
        g_seat_name = (int)name;
        g_have_seat = 1;
    }
}

static void raw_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

/* ---- the zwp_input_method_v2 listener (the add-on owns it) ---- */
static void im_activate(void *data,
                        struct zwp_input_method_v2 *im) {
    (void)data;
    (void)im;
    g_activated = 1;
    printf("IM: activated (a text-input client took focus)\n");
}

static void im_deactivate(void *data,
                          struct zwp_input_method_v2 *im) {
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
    g_im_serial++;
    if (g_activated && g_commits < 1) {
        zwp_input_method_v2_commit_string(
            im, "hello from the FDK IM add-on");
        zwp_input_method_v2_commit(im, g_im_serial);
        g_commits++;
        printf("IM: committed a string (serial %u)\n", g_im_serial);
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
    if (!fdk_ok(fdk_init(&g_ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    if (!fdk_ext_has_capability(g_ctx, FDK_EXT_CAP_INPUT_METHOD_V2) ||
        !fdk_ext_has_capability(g_ctx, FDK_EXT_CAP_WL_RAW_EVENTS)) {
        fprintf(stderr, "SKIP: no input-method or raw-event capability\n");
        fdk_shutdown(g_ctx);
        return 77;
    }
    g_q = fdk_wl_queue_create(g_ctx, "fdk-example-im");
    if (g_q == NULL) {
        fprintf(stderr, "FAIL: no raw queue\n");
        fdk_shutdown(g_ctx);
        return 1;
    }
    struct wl_registry_listener rl = { raw_global, raw_remove };
    struct wl_registry *reg = fdk_wl_queue_registry(g_q);
    wl_registry_add_listener(reg, &rl, NULL);
    fdk_wl_queue_attach(g_q, 1); /* FDK's pump dispatches our queue */

    for (int i = 0; i < 20 && !g_have_seat; i++) {
        fdk_pump_events(g_ctx, 16);
    }
    if (!g_have_seat) {
        fprintf(stderr, "SKIP: no wl_seat advertised\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 77;
    }
    printf("IM: wl_seat is name %u (raw, on the add-on queue)\n",
           (unsigned)g_seat_name);

    /* bind the seat ONTO OUR QUEUE and hand the raw proxy to FDK */
    struct wl_seat *seat = fdk_wl_raw_bind(
        g_q, (fdk_u32)g_seat_name, &wl_seat_interface, 1);
    if (seat == NULL) {
        fprintf(stderr, "FAIL: raw seat bind refused\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    g_im = fdk_input_method_open(g_ctx, seat);
    if (g_im == NULL) {
        fprintf(stderr, "FAIL: input method refused\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    zwp_input_method_v2_add_listener(g_im, &g_im_listener, NULL);
    printf("IM: zwp_input_method_v2 minted on the seat\n");

    g_grab = fdk_input_method_grab_keyboard(g_im);
    printf("IM: keyboard grab %s\n",
           g_grab != NULL ? "taken" : "refused");

    for (int round = 0; round < 60; round++) {
        fdk_pump_events(g_ctx, 16);
    }
    printf("IM: %d commits, %d deactivations; tearing down\n",
           g_commits, g_deactivations);

    fdk_input_method_close(g_im);
    fdk_wl_queue_destroy(g_q);
    fdk_shutdown(g_ctx);
    printf("IM example: clean exit\n");
    return 0;
}
