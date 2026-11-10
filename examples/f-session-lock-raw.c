/* examples/f-session-lock-raw.c — the session-lock screen, the RAW
 * escape hatch (see f-session-lock.c for the FDK-native shape: the
 * layer kind + the native accessor + fdk_ext_wl_output — no raw
 * queue, no vendored headers, no hand-rolled kind). THIS example
 * shows the full add-on machinery the first-class path hides:
 *   fdk_wl_queue_create / fdk_wl_queue_registry / fdk_wl_raw_bind
 *     — the raw-event surface (wl_compositor + wl_output on the
 *       add-on's own queue),
 *   fdk_session_lock_open      — the ext_session_lock_v1 object,
 *   fdk_session_lock_surface   — the lock surface per output,
 *   a registered fdk_ext_surface_kind whose create callback performs
 *     the role handshake and hands FDK the raw wl_surface — FDK then
 *     owns rendering/present on it (the ext-surface ownership
 *     transfer), so the lock screen is an ordinary FDK surface to
 *     draw on,
 *   fdk_session_lock_unlock    — the release.
 *
 * The compositor's verdicts arrive through the session-lock event
 * callback (1 = locked, 0 = finished — refused or taken). Sway maps
 * the lock once the surface commits a buffer for every output.
 *
 * Wall check (P5): every wall this hits is an FDK bug to fix.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "protocols/ext-session-lock-v1-client-protocol.h"
#include <stdio.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

static fdk_context *g_ctx = NULL;
static fdk_wl_queue *g_q = NULL;
static void *g_lock = NULL;
static int g_locked = 0;
static int g_finished = 0;
static int g_have_compositor = 0;
static int g_outputs = 0;

static uint32_t g_compositor_name = 0;
static uint32_t g_output_name = 0;

static void raw_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)data;
    (void)r;
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        g_compositor_name = name;
        g_have_compositor = 1;
    } else if (strcmp(iface, "wl_output") == 0 && g_output_name == 0) {
        g_output_name = name;
        g_outputs++;
    }
}

static void raw_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data;
    (void)r;
    (void)name;
}

static void on_lock_verdict(void *user, int locked) {
    (void)user;
    if (locked) {
        g_locked = 1;
        printf("LOCK: locked (exclusive access granted)\n");
    } else {
        g_finished = 1;
        printf("LOCK: finished (refused or taken)\n");
    }
}

/* ---- the session-lock surface kind -------------------------------------- */
struct lock_kind_state {
    struct wl_surface *surface;   /* ours until FDK adopts it */
    struct wl_compositor *compositor;
    struct ext_session_lock_surface_v1 *lock_surface;
};

static fdk_result lock_create(fdk_ext_surface *surface,
                              fdk_ext_native_handle *out_native,
                              void *user) {
    struct lock_kind_state *st = user;
    (void)surface;
    if (st->compositor == NULL || g_output_name == 0) {
        return (fdk_result)-1; /* no raw globals: refuse */
    }
    st->surface = wl_compositor_create_surface(st->compositor);
    if (st->surface == NULL) {
        return (fdk_result)-1;
    }
    /* the role: the lock surface on the output (raw protocol) */
    struct wl_output *out = fdk_wl_raw_bind(
        g_q, g_output_name, &wl_output_interface, 2);
    if (out == NULL) {
        return (fdk_result)-1;
    }
    st->lock_surface = fdk_session_lock_surface(g_ctx, g_lock,
                                                st->surface, out);
    if (st->lock_surface == NULL) {
        return (fdk_result)-1;
    }
    /* the native handle: FDK adopts the wl_surface (ownership
     * transfer — FDK now renders/commits on it; we never touch it
     * again except through fdk_ext_surface_notify_configure) */
    out_native->pointer = (void *)st->surface;
    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    return FDK_OK;
}

static void lock_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    struct lock_kind_state *st = user;
    if (st->lock_surface != NULL) {
        ext_session_lock_surface_v1_destroy(st->lock_surface);
        st->lock_surface = NULL;
    }
}

int main(void) {
    if (!fdk_ok(fdk_init(&g_ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    if (!fdk_ext_has_capability(g_ctx, FDK_EXT_CAP_SESSION_LOCK) ||
        !fdk_ext_has_capability(g_ctx, FDK_EXT_CAP_WL_RAW_EVENTS)) {
        fprintf(stderr, "SKIP: no session-lock or raw-event capability\n");
        fdk_shutdown(g_ctx);
        return 77;
    }
    g_q = fdk_wl_queue_create(g_ctx, "fdk-example-lock");
    if (g_q == NULL) {
        fprintf(stderr, "FAIL: no raw queue\n");
        fdk_shutdown(g_ctx);
        return 1;
    }
    struct wl_registry_listener rl = { raw_global, raw_remove };
    struct wl_registry *reg = fdk_wl_queue_registry(g_q);
    wl_registry_add_listener(reg, &rl, NULL);
    fdk_wl_queue_attach(g_q, 1);
    for (int i = 0; i < 20 && (!g_have_compositor || g_output_name == 0);
         i++) {
        fdk_pump_events(g_ctx, 16);
    }
    if (!g_have_compositor || g_output_name == 0) {
        fprintf(stderr, "SKIP: no wl_compositor/wl_output on the queue\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 77;
    }
    struct wl_compositor *comp = fdk_wl_raw_bind(
        g_q, g_compositor_name, &wl_compositor_interface, 4);
    if (comp == NULL) {
        fprintf(stderr, "FAIL: compositor bind refused\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }

    /* the lock object */
    g_lock = fdk_session_lock_open(g_ctx, on_lock_verdict, NULL);
    if (g_lock == NULL) {
        fprintf(stderr, "FAIL: lock refused\n");
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    printf("LOCK: minted\n");

    /* the surface kind: FDK adopts the role-carrying wl_surface */
    static struct lock_kind_state st;
    memset(&st, 0, sizeof st);
    st.compositor = comp;
    fdk_ext_surface_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.struct_size = sizeof ops;
    ops.create = lock_create;
    ops.destroy = lock_destroy;
    fdk_ext_surface_kind *kind = NULL;
    if (!fdk_ok(fdk_ext_register_kind(g_ctx, "session-lock-screen",
                                      &ops, &st, &kind))) {
        fprintf(stderr, "FAIL: kind registration\n");
        fdk_session_lock_unlock(g_lock);
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    fdk_ext_surface *es = NULL;
    fdk_result r = fdk_ext_surface_create(kind, &es);
    if (!fdk_ok(r)) {
        fprintf(stderr, "FAIL: lock surface creation (%d)\n", (int)r);
        fdk_session_lock_unlock(g_lock);
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    printf("LOCK: surface adopted by FDK\n");

    /* the configure handshake: the compositor sizes the lock surface
     * to the output; a real screen reads the configure event — the
     * demo reports the sway headless 1280x720 and paints. */
    fdk_ext_surface_notify_configure(es, 1280, 720);
    fdk_ext_surface_show(es);
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(es, &s))) {
        fprintf(stderr, "FAIL: no FDK surface on the lock\n");
        fdk_ext_surface_destroy(es);
        fdk_session_lock_unlock(g_lock);
        fdk_wl_queue_destroy(g_q);
        fdk_shutdown(g_ctx);
        return 1;
    }
    for (int round = 0; round < 40; round++) {
        fdk_pump_events(g_ctx, 16);
        if (round == 2) {
            /* the locked screen: FDK renders, the present commits */
            fdk_surface_fill(s, (fdk_color){ 0.02f, 0.02f, 0.05f, 1.0f });
            fdk_rect panel = { 400, 300, 480, 120 };
            fdk_surface_fill_rect(s, panel,
                                  (fdk_color){ 0.15f, 0.3f, 0.55f, 1.0f });
            (void)fdk_surface_present(s);
            printf("LOCK: presented the lock screen\n");
        }
    }
    printf("LOCK: verdicts locked=%d finished=%d; unlocking\n", g_locked,
           g_finished);
    fdk_ext_surface_destroy(es);
    fdk_session_lock_unlock(g_lock);
    fdk_ext_kind_destroy(kind);
    fdk_wl_queue_destroy(g_q);
    fdk_shutdown(g_ctx);
    printf("LOCK example: clean exit\n");
    return 0;
}
