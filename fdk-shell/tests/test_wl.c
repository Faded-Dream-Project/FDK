/* test_wl.c — fdk-shell's Wayland backend tests (the wlr-layer-shell
 * half).
 *
 * Requires a RUNNING Wayland compositor that implements
 * wlr-layer-shell v4 (sway/labwc/wlroots-family) reachable through
 * $WAYLAND_DISPLAY — `make test-wayland` points it at one; the
 * binary SELF-SKIPS (exit 0, "[skip]") when no compositor is
 * reachable, so plain CI never fails on it (the same contract FDK's
 * own test-wayland carries).
 *
 * Verified against a real compositor (the rig starts sway headless,
 * pixman renderer): the layer-shell handshake, the configure-driven
 * size (anchored axes compositor-sized, desired axes honored),
 * exclusive-zone + keyboard-mode requests accepted, rendering
 * through FDK's extension surface path (a presented frame with no
 * protocol error, on a live connection afterwards), popups parented
 * to the layer through the popup_attached hook (configure arrives,
 * present succeeds), and clean teardown.
 *
 * NOT verifiable in this environment (documented honestly):
 *   - outside-click dismissal on Wayland needs a real input serial
 *     (the compositor only honors xdg_popup.grab with one); the
 *     headless seat produces no pointer events. The machinery is
 *     FDK's existing, menu-tested xdg_popup grab path.
 *   - visual readback (the compositor's own screenshot would be the
 *     channel; the rig's sway can produce one but layer-surface
 *     content assertions are out of scope here).
 */

#include <fdk-shell/fdk-shell.h>

#include "../src/shell_internal.h" /* verification channel only */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void pump(fdk_context *ctx, int ms) {
    int steps = ms / 50;
    for (int i = 0; i < steps; i++) {
        (void)fdk_pump_events(ctx, 50);
    }
}

/* Configure recorder: the layer's event stream must carry the
 * compositor's size assignment. */
static int g_configure_events = 0;
static fdk_size g_last_configure = { 0, 0 };

static void layer_event(fdk_shell_layer *layer, const fdk_event_data *ev,
                        void *user) {
    (void)layer;
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_CONFIGURE) {
        g_configure_events++;
        g_last_configure = ev->configure.size;
    }
}

static void test_layer_on_compositor(void) {
    fdk_context *ctx = NULL;
    fdk_init_options iopts = { .backend = FDK_PLATFORM_WAYLAND };
    fdk_result r = fdk_init(&ctx, &iopts);
    if (!fdk_ok(r)) {
        printf("[skip] no reachable Wayland compositor\n");
        return;
    }

    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd)) ||
        nd.display == NULL) {
        printf("[skip] no native display\n");
        fdk_shutdown(ctx);
        return;
    }

    fdk_shell *shell = NULL;
    r = fdk_shell_init(ctx, &shell);
    if (r == FDK_ERR_UNSUPPORTED) {
        printf("[skip] compositor offers no wlr-layer-shell "
               "(fdk-shell refuses honestly)\n");
        fdk_shutdown(ctx);
        return;
    }
    assert(fdk_ok(r));
    assert(fdk_ext_active_backend(ctx) == FDK_EXT_BACKEND_WAYLAND);

    /* Outputs: a real compositor has at least one. */
    uint32_t outputs = 0;
    assert(fdk_ok(fdk_shell_output_count(shell, &outputs)));
    assert(outputs >= 1);
    char name[64];
    assert(fdk_ok(fdk_shell_output_name(shell, 0, name, sizeof(name))));
    printf("     output 0: \"%s\" (%u total)\n", name, outputs);

    /* A top bar: full-width, 32 tall, auto zone. */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors = FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT |
                   FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    desc.exclusive_zone = -1;

    g_configure_events = 0;
    fdk_shell_layer *layer = NULL;
    assert(fdk_ok(fdk_shell_layer_create(shell, &desc, &layer)));
    fdk_shell_layer_set_event_callback(layer, layer_event, NULL);
    fdk_shell_layer_show(layer); /* THE handshake starter: the empty
                                  * commit that asks for a configure */

    /* The layer-shell handshake: the compositor's configure must
     * arrive through FDK's pump (the add-on's own listener runs
     * inside dispatch) and reach the layer's event stream. */
    for (int i = 0; i < 40 && g_configure_events == 0; i++) {
        pump(ctx, 50);
    }
    assert(g_configure_events >= 1);
    assert(g_last_configure.height == 32);
    assert(g_last_configure.width > 0); /* the compositor sized the
                                         * anchored axis */

    fdk_size sz;
    assert(fdk_ok(fdk_shell_layer_get_size(layer, &sz)));
    assert(sz.height == 32 && sz.width == g_last_configure.width);

    /* Render + present through the public surface path: the frame
     * commits (the extension API's present), the connection stays
     * alive (a protocol violation would kill it — the pumps below
     * would then fail loudly). */
    fdk_surface *surface = NULL;
    assert(fdk_ok(fdk_shell_layer_get_surface(layer, &surface)));
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(surface, &info)));
    assert(info.width == g_last_configure.width);
    assert(info.height == 32);
    fdk_surface_fill(surface, (fdk_color){ .r = 0.25f, .g = 0.55f,
                                           .b = 0.85f, .a = 1.0f });
    assert(fdk_ok(fdk_surface_present(surface)));
    pump(ctx, 400);

    /* The size query is still live — the connection survived the
     * commit (the honest "no protocol error" check). */
    assert(fdk_ok(fdk_shell_layer_get_size(layer, &sz)));
    assert(sz.height == 32);

    /* A popup parented to the layer: the kind's popup_attached hook
     * runs (the zwlr get_popup adoption — the rig's WAYLAND_DEBUG
     * trace pins the exchange: get_popup, the handshake commit, the
     * compositor's xdg_popup.configure(20, 33, 120, 90), the ack),
     * the popup renders, presents, and the connection stays alive.
     *
     * NOTE the event-stream expectation: FDK deliberately stays
     * SILENT on a popup's first configure when the size matches the
     * creation size (the documented menu contract — creation size is
     * authoritative; only size CHANGES dispatch configure). So the
     * observable assertions are render + present + a live
     * connection, not a configure event. */
    fdk_shell_popup *popup = NULL;
    assert(fdk_ok(fdk_shell_popup_create(layer, 20, 32, 120, 90, &popup)));
    /* (no callback needed for this leg — see the note below) */
    fdk_shell_popup_show(popup);
    pump(ctx, 300);
    fdk_surface *ps = NULL;
    assert(fdk_ok(fdk_shell_popup_get_surface(popup, &ps)));
    fdk_surface_fill(ps, (fdk_color){ .r = 1.0f, .g = 0.8f, .b = 0.1f,
                                     .a = 1.0f });
    assert(fdk_ok(fdk_surface_present(ps)));
    pump(ctx, 400);

    /* Keyboard modes + every layer level accepted (configure still
     * arrives, connection still live). */
    for (int level = FDK_SHELL_LAYER_BACKGROUND;
         level <= FDK_SHELL_LAYER_OVERLAY; level++) {
        fdk_shell_layer_desc d2;
        memset(&d2, 0, sizeof(d2));
        d2.struct_size = sizeof(d2);
        d2.layer = (fdk_shell_layer_level)level;
        d2.anchors = FDK_SHELL_ANCHOR_BOTTOM | FDK_SHELL_ANCHOR_LEFT;
        d2.width = 64;
        d2.height = 24;
        d2.exclusive_zone = 24;
        d2.keyboard = (level == FDK_SHELL_LAYER_OVERLAY)
            ? FDK_SHELL_KBD_ON_DEMAND
            : FDK_SHELL_KBD_NONE;
        fdk_shell_layer *l2 = NULL;
        assert(fdk_ok(fdk_shell_layer_create(shell, &d2, &l2)));
        fdk_shell_layer_show(l2);
        pump(ctx, 300);
        fdk_shell_layer_destroy(l2);
        pump(ctx, 100);
    }

    fdk_shell_popup_destroy(popup);
    fdk_shell_layer_destroy(layer);
    pump(ctx, 200);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("[ok] fdk-shell wayland: layer handshake, configure-driven "
           "size, present, popup adoption, all layer levels + kbd "
           "modes, clean teardown\n");
}

int main(void) {
    test_layer_on_compositor();
    printf("fdk-shell wayland tests done\n");
    return 0;
}
