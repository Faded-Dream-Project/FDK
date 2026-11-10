/* examples/f-session-lock.c — P5 (f), the FDK-native session-lock
 * screen: ONE public call for the whole role. NO raw-event surface,
 * NO vendored protocol headers, NO hand-rolled handshake:
 *
 *   fdk_session_lock_open         — the lock object (the verdict
 *     callback: locked / finished),
 *   fdk_session_lock_screen_create — FDK mints its own role-less
 *     wl_surface, attaches the lock role on the first bound output,
 *     listens for the compositor's configure, and adopts the surface:
 *     draw through the ordinary fdk_surface API,
 *   fdk_session_lock_unlock       — the release.
 *
 * (The raw escape hatch — fdk_session_lock_surface with add-on-owned
 * handles — lives in f-session-lock-raw.c. A surface that already
 * carries a role cannot take the lock role: protocol error 2, the
 * connection dies. That wall shaped this API.)
 *
 * Wall check (P5): every wall this hits is an FDK bug to fix.
 */
#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include <stdio.h>
#include <string.h>

static int g_locked = 0;
static int g_finished = 0;

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

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "no FDK context\n");
        return 1;
    }
    if (!fdk_ext_has_capability(ctx, FDK_EXT_CAP_SESSION_LOCK)) {
        fprintf(stderr, "SKIP: no session-lock capability\n");
        fdk_shutdown(ctx);
        return 77;
    }

    /* 1. the lock object */
    void *lock = fdk_session_lock_open(ctx, on_lock_verdict, NULL);
    if (lock == NULL) {
        fprintf(stderr, "FAIL: lock refused\n");
        fdk_shutdown(ctx);
        return 1;
    }
    printf("LOCK: minted\n");

    /* 2. the FDK-managed lock screen */
    fdk_ext_surface *es = NULL;
    fdk_result r = fdk_session_lock_screen_create(ctx, lock, &es);
    if (!fdk_ok(r)) {
        fprintf(stderr, "FAIL: lock screen refused (%d)\n", (int)r);
        fdk_session_lock_unlock(lock);
        fdk_shutdown(ctx);
        return 1;
    }
    printf("LOCK: the managed screen adopted\n");

    /* 3. draw: the locked screen (FDK renders; the present commits
     * the buffer that maps the lock) */
    fdk_surface *s = NULL;
    if (!fdk_ok(fdk_ext_surface_get_surface(es, &s))) {
        fprintf(stderr, "FAIL: no render target\n");
        fdk_ext_surface_destroy(es);
        fdk_session_lock_unlock(lock);
        fdk_shutdown(ctx);
        return 1;
    }
    for (int round = 0; round < 40; round++) {
        fdk_pump_events(ctx, 16);
        if (round == 2) {
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

    /* 4. the release (the role teardown wants its surface alive) */
    fdk_ext_surface_destroy(es);
    fdk_session_lock_unlock(lock);
    fdk_shutdown(ctx);
    printf("LOCK example: clean exit\n");
    return 0;
}
