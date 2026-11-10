/* examples/h-status-panel.c — the BLIND example: written ONLY from
 * docs/EXTENDING.md and the public headers (include/fdk/*.h), with
 * src/ unread. A status-bar panel:
 *
 *   - a first-class layer surface (BOTTOM, anchored left-right-top,
 *     exclusive zone = its height),
 *   - an idle-inhibit toggle (the panel holds the screen awake),
 *   - a text-layout label (the P4 shaping/layout API) with a clock.
 *
 * Every point where the docs were wrong, missing, or misleading is
 * recorded in DOC-GAPS.md (the P5 blind-walk report). Written for
 * the sway rig; SKIPs honestly where a cap is missing.
 *
 * Build (the docs' §2 shape):
 *   cc -I$PREFIX/include h-status-panel.c -L$PREFIX/lib -lfdk
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_text.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static fdk_font *g_font = NULL;
static int g_inhibited = 0;
static int g_toggles = 0;
static fdk_ext_surface *g_panel = NULL;

static void on_event(fdk_ext_surface *surface, const fdk_event_data *e,
                     void *user) {
    (void)surface;
    (void)user;
    if (e->type == FDK_EVENT_WINDOW_CONFIGURE) {
        printf("PANEL: configured (%dx%d)\n", e->configure.size.width,
               e->configure.size.height);
    } else if (e->type == FDK_EVENT_KEY_DOWN) {
        /* the toggle: any key press flips the idle inhibitor */
        g_inhibited = !g_inhibited;
        fdk_result r =
            fdk_ext_surface_set_idle_inhibited(surface, g_inhibited);
        g_toggles++;
        printf("PANEL: idle inhibit %s -> %s\n",
               g_inhibited ? "ON" : "OFF",
               fdk_ok(r) ? "ok"
                         : (r == FDK_ERR_UNSUPPORTED ? "UNSUPPORTED"
                                                     : "failed"));
    }
}

static void draw_panel(fdk_surface *s, int w, int h) {
    fdk_surface_fill(s, (fdk_color){ 0.08f, 0.09f, 0.12f, 1.0f });
    fdk_rect accent = { 0, h - 2, w, 2 };
    fdk_surface_fill_rect(s, accent,
                          (fdk_color){ 0.25f, 0.5f, 0.85f, 1.0f });
    /* the label: the P4 layout/shaping API drives the pen */
    char buf[128];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(buf, sizeof buf, "FDK panel  |  %H:%M:%S", &tmv);
    char label[160];
    snprintf(label, sizeof label, "%s  |  inhibit: %s", buf,
             g_inhibited ? "on " : "off");
    if (g_font != NULL) {
        /* the layout introspection: the tier + the advance */
        fdk_text_shaping_tier tier = fdk_text_shaping_tier_get();
        fdk_text_cluster cl[160];
        size_t n = 0;
        fdk_f32 advance = 0.0f;
        if (fdk_ok(fdk_font_layout_utf8(g_font, label, strlen(label),
                                        cl, 160, &n, &advance))) {
            (void)tier;
            fdk_surface_draw_utf8(s, g_font, label, strlen(label), 12,
                                  h - 12,
                                  (fdk_color){ 0.9f, 0.92f, 0.95f, 1.0f });
            (void)advance;
        }
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    /* the docs' §9 capability table */
    if (fdk_ext_active_backend(ctx) != FDK_EXT_BACKEND_WAYLAND ||
        !fdk_ext_has_capability(ctx, FDK_EXT_CAP_LAYER_SHELL)) {
        fprintf(stderr, "SKIP: needs the Wayland layer-shell lane\n");
        fdk_shutdown(ctx);
        return 77;
    }
    g_font = fdk_font_load_system_default(14);
    if (g_font == NULL) {
        printf("PANEL: no system font (the label draws nothing)\n");
    }

    /* §4 + fdk_extend.h: the first-class layer surface */
    fdk_ext_layer_options opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.layer = FDK_LAYER_BOTTOM;
    opts.anchor = FDK_LAYER_ANCHOR_TOP | FDK_LAYER_ANCHOR_LEFT |
                  FDK_LAYER_ANCHOR_RIGHT;
    opts.exclusive_zone = -1; /* the docs: exclusive to the anchored
                               * edge's full size */
    opts.namespace_ = "fdk-status-panel";
    fdk_result r = fdk_ext_layer_surface_create(ctx, &opts, &g_panel);
    if (!fdk_ok(r)) {
        fprintf(stderr, "FAIL: layer surface refused (%d)\n", (int)r);
        fdk_shutdown(ctx);
        return 1;
    }
    (void)fdk_ext_surface_set_event_callback(g_panel, on_event, NULL);
    /* the panel's height: the client's to state (the compositor
     * sizes an anchored edge surface from the committed size —
     * without set_size the bar is 0 px tall and invisible) */
    (void)fdk_ext_layer_surface_set_size(g_panel, 0, 32);
    fdk_ext_surface_show(g_panel); /* the docs: a Wayland no-op; the
                                    * first present maps it */

    /* run: draw on the configure, pump, toggle on a key */
    int configured = 0;
    for (int round = 0; round < 90; round++) {
        fdk_pump_events(ctx, 16);
        fdk_surface *s = NULL;
        if (!fdk_ok(fdk_ext_surface_get_surface(g_panel, &s))) {
            break;
        }
        fdk_surface_info info;
        memset(&info, 0, sizeof info);
        if (fdk_ok(fdk_surface_get_info(s, &info)) &&
            (info.width > 0 || configured)) {
            if (!configured) {
                configured = 1;
                printf("PANEL: first frame %dx%d\n", info.width,
                       info.height);
            }
            draw_panel(s, info.width, info.height);
            (void)fdk_surface_present(s);
        }
    }
    printf("PANEL: %d toggles; clean exit\n", g_toggles);
    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    fdk_ext_surface_destroy(g_panel);
    fdk_shutdown(ctx);
    return 0;
}
