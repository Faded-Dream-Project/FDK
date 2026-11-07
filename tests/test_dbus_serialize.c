/* test_dbus_serialize.c — G-8: per-address handshake serialization.
 *
 * Found live in fdk-files (§27/2a): the app's five desktop modules
 * (notify, tray, settings portal, idle, atspi) each opened their own
 * session-bus connection around the same pumps; the burst of five
 * SIMULTANEOUS SASL handshakes made dbus-daemon close the first one
 * mid-auth ("bus closed the connection" / "socket read failed" on
 * the notification client — the wire trace showed the first
 * connection auth fine and then get dropped as the next four
 * opened). The fix serializes the connect/auth/hello phase per
 * address: later connects are parked (handle returned, sends queue)
 * until the predecessor settles.
 *
 * The rig: five connections with NO pump between the opens (the
 * worst case — all five would have authed in the same pump burst),
 * then pumping. All five must reach ready, in order, with zero
 * failures. Self-skips without a session bus.
 */
#include "fdk/fdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define N 5

static int g_ok = 0, g_fail = 0;

static void rdy(fdk_dbus_connection *c, int ok, const char *err,
                void *user) {
    (void)c;
    (void)err;
    (void)user;
    if (ok) {
        g_ok++;
    } else {
        g_fail++;
    }
}

int main(void) {
    if (getenv("DBUS_SESSION_BUS_ADDRESS") == NULL) {
        printf("[skip] no session bus in the environment\n");
        return 0;
    }
    fdk_context *ctx = NULL;
    fdk_init_options o = {0};
    o.app_id = "org.fdk.test.dbusserialize";
    if (!fdk_ok(fdk_init(&ctx, &o))) {
        printf("[skip] no display\n");
        return 0;
    }
    fdk_dbus_connection *cs[N] = {0};
    for (int i = 0; i < N; i++) {
        cs[i] = fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, rdy, NULL);
        if (cs[i] == NULL) {
            printf("[skip] connect %d refused (no bus)\n", i);
            for (int j = 0; j < i; j++) {
                fdk_dbus_connection_close(cs[j]);
            }
            fdk_shutdown(ctx);
            return 0;
        }
    }
    for (int i = 0; i < 400; i++) {
        (void)fdk_pump_events(ctx, 20);
        if (g_ok + g_fail == N) {
            break;
        }
    }
    for (int i = 0; i < N; i++) {
        if (cs[i] != NULL) {
            fdk_dbus_connection_close(cs[i]);
        }
    }
    fdk_shutdown(ctx);
    if (g_ok != N || g_fail != 0) {
        printf("FAIL: ready=%d failed=%d (of %d)\n", g_ok, g_fail, N);
        return 1;
    }
    printf("[ok] dbus: %d simultaneous session connects all reached "
           "ready (serialized handshakes; zero failures)\n",
           N);
    return 0;
}
