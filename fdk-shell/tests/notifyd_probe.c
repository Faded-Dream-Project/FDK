/* notifyd_probe.c — the notifyd rig's helper client (1.1.0).
 *
 * Two roles the per-process reference tools (dbus-send, gdbus)
 * cannot play:
 *
 *   flood N   — N Notify calls sent back-to-back from ONE process:
 *               real bus traffic fast enough to cross the daemon's
 *               rate window (a shell loop of dbus-send's cannot —
 *               process startup alone exceeds the window). Every
 *               call's reply is collected; the exit code says
 *               whether all N were answered.
 *
 *   squat [S] — requests org.freedesktop.Notifications with flags 0
 *               (NO allow-replacement, NO replace-existing, and the
 *               queue allowed) and holds it for S seconds (default
 *               20). This is the "stub that refuses replacement"
 *               the etiquette legs need: a --replace against it must
 *               get RequestName -> EXISTS and exit with the one-line
 *               refusal. It also doubles as a second independent
 *               CONSUMER of FDK's public D-Bus client.
 *
 * Built by `make -C fdk-shell test-notifyd` next to the rig; runs
 * under the rig's dbus-run-session (a display is present for the
 * event loop). Exit codes: 0 ok, 1 failure.
 */

#include "fdk/fdk.h"
#include "fdk/fdk_dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N_NAME "org.freedesktop.Notifications"
#define N_PATH "/org/freedesktop/Notifications"
#define N_IFACE "org.freedesktop.Notifications"

typedef struct {
    int want;
    int answered;
    int errored;
} flood_state;

static void flood_reply(fdk_dbus_connection *bus, fdk_dbus_message *reply,
                        void *user) {
    (void)bus;
    flood_state *st = user;
    if (reply == NULL || fdk_dbus_message_is_error(reply)) {
        st->errored++;
        return;
    }
    fdk_dbus_value **args = NULL;
    size_t n = 0;
    if (fdk_dbus_message_read_args(reply, &args, &n) == FDK_OK &&
        n == 1 && args != NULL &&
        fdk_dbus_value_type(args[0]) == FDK_DBUS_UINT32 &&
        fdk_dbus_value_uint32(args[0]) != 0) {
        st->answered++;
    } else {
        st->errored++;
    }
    if (args != NULL) {
        fdk_dbus_values_free_array(args, n);
    }
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int run_flood(fdk_context *ctx, int count) {
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (bus == NULL) {
        fprintf(stderr, "probe: no session bus\n");
        return 1;
    }
    /* Pump until ready (the handshake rides the loop). */
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(bus)) {
        fprintf(stderr, "probe: bus never became ready\n");
        fdk_dbus_connection_close(bus);
        return 1;
    }

    flood_state st = { .want = count, .answered = 0, .errored = 0 };
    double t0 = now_s();
    for (int i = 0; i < count; i++) {
        /* The empty actions array and hints dict as REAL array values
         * (the wire signature must be susssasa{sv}i — strings would
         * lie about the shape). */
        fdk_dbus_message *call = fdk_dbus_message_new_method_call(
            N_NAME, N_PATH, N_IFACE, "Notify");
        fdk_dbus_value *empty_actions = fdk_dbus_value_new_array("s", NULL, 0);
        fdk_dbus_value *empty_hints = fdk_dbus_value_new_array("{sv}", NULL, 0);
        if (call == NULL || empty_actions == NULL || empty_hints == NULL ||
            !fdk_ok(fdk_dbus_message_append_string(call, "probe-flood")) ||
            !fdk_ok(fdk_dbus_message_append_uint32(call, 0)) ||
            !fdk_ok(fdk_dbus_message_append_string(call, "")) ||
            !fdk_ok(fdk_dbus_message_append_string(call, "burst")) ||
            !fdk_ok(fdk_dbus_message_append_string(call, "")) ||
            !fdk_ok(fdk_dbus_message_append(call, empty_actions)) ||
            !fdk_ok(fdk_dbus_message_append(call, empty_hints)) ||
            !fdk_ok(fdk_dbus_message_append_int32(call, 1200))) {
            fprintf(stderr, "probe: build failed at %d\n", i);
            fdk_dbus_message_free(call);
            fdk_dbus_value_free(empty_actions);
            fdk_dbus_value_free(empty_hints);
            fdk_dbus_connection_close(bus);
            return 1;
        }
        fdk_dbus_value_free(empty_actions);
        fdk_dbus_value_free(empty_hints);
        if (fdk_dbus_connection_call(bus, call, 3000, flood_reply, &st) ==
            NULL) {
            fprintf(stderr, "probe: send failed at %d\n", i);
        }
        fdk_pump_events(ctx, 0); /* flush without waiting */
    }
    double burst_s = now_s() - t0;

    /* Collect: pump until all N replies or 5 s. */
    deadline = now_s() + 5.0;
    while (st.answered + st.errored < st.want && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    printf("probe: %d/%d answered (%d errors) in %.3f s burst\n",
           st.answered, st.want, st.errored, burst_s);
    fdk_dbus_connection_close(bus);
    return (st.answered == st.want && st.errored == 0) ? 0 : 1;
}


/* icon R G B [timeout] -- a Notify whose image-data hint carries a
 * TRUE byte array (the spec's ay): a 16x16 solid RGBA icon. The
 * cross-implementation text clients (gdbus) cannot express ay --
 * their bare number lists infer as int32 arrays, which the daemon
 * must refuse -- so this mode is how the rig exercises the real
 * raw-icon path end to end. Prints the returned id. */
static int run_icon(fdk_context *ctx, int rr, int gg, int bb,
                    int timeout) {
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (bus == NULL) {
        fprintf(stderr, "probe: no session bus\n");
        return 1;
    }
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(bus)) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    /* (iiibiiay): 16x16, rowstride 64, alpha, 8 bps, 4 channels.
     * Ownership (the value API's rule, applied once, correctly):
     * new_array/new_struct/new_dict_entry TAKE their children;
     * new_variant CLONES its child (the original stays ours). */
    enum { W = 16, H = 16, RS = 64, CH = 4 };
    fdk_dbus_value *bytes[W * H * CH];
    size_t n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            bytes[n++] = fdk_dbus_value_new_byte((uint8_t)rr);
            bytes[n++] = fdk_dbus_value_new_byte((uint8_t)gg);
            bytes[n++] = fdk_dbus_value_new_byte((uint8_t)bb);
            bytes[n++] = fdk_dbus_value_new_byte(0xFF);
        }
    }
    fdk_dbus_value *data = fdk_dbus_value_new_array("y", bytes, n);
    /* bytes[] now owned by the array -- not freed individually. */

    fdk_dbus_value *fields[7];
    fields[0] = fdk_dbus_value_new_int32(W);
    fields[1] = fdk_dbus_value_new_int32(H);
    fields[2] = fdk_dbus_value_new_int32(RS);
    fields[3] = fdk_dbus_value_new_boolean(1);
    fields[4] = fdk_dbus_value_new_int32(8);
    fields[5] = fdk_dbus_value_new_int32(CH);
    fields[6] = data;
    fdk_dbus_value *icon_struct = fdk_dbus_value_new_struct(fields, 7);
    /* fields[] (incl. data) now owned by the struct. */

    fdk_dbus_value *actions = fdk_dbus_value_new_array("s", NULL, 0);
    fdk_dbus_value *urg = fdk_dbus_value_new_byte(1);
    fdk_dbus_value *urg_v = fdk_dbus_value_new_variant(urg);
    fdk_dbus_value_free(urg); /* the variant keeps its own clone */
    fdk_dbus_value *key = fdk_dbus_value_new_string("urgency");
    fdk_dbus_value *key2 = fdk_dbus_value_new_string("image-data");
    fdk_dbus_value *icon_v = fdk_dbus_value_new_variant(icon_struct);
    fdk_dbus_value_free(icon_struct); /* ditto */
    fdk_dbus_value *e1 = fdk_dbus_value_new_dict_entry(key, urg_v);
    fdk_dbus_value *e2 = fdk_dbus_value_new_dict_entry(key2, icon_v);
    fdk_dbus_value *hint_items[2] = { e1, e2 };
    fdk_dbus_value *hints = fdk_dbus_value_new_array("{sv}", hint_items, 2);
    /* e1/e2 (and everything they own) now belong to hints. */

    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        N_NAME, N_PATH, N_IFACE, "Notify");
    int ok = call != NULL && actions != NULL && hints != NULL &&
             fdk_ok(fdk_dbus_message_append_string(call, "probe-icon")) &&
             fdk_ok(fdk_dbus_message_append_uint32(call, 0)) &&
             fdk_ok(fdk_dbus_message_append_string(call, "")) &&
             fdk_ok(fdk_dbus_message_append_string(call, "Icon card")) &&
             fdk_ok(fdk_dbus_message_append_string(call, "raw ay icon")) &&
             fdk_ok(fdk_dbus_message_append(call, actions)) &&
             fdk_ok(fdk_dbus_message_append(call, hints)) &&
             fdk_ok(fdk_dbus_message_append_int32(call, timeout));
    /* new_array OWNS e1/e2 (and transitively key/key2/urg_v/icon_v);
     * freeing them here would be the double-free the rig's ASan run
     * caught. Only the two top-level containers go. */
    fdk_dbus_value_free(actions);
    fdk_dbus_value_free(hints);
    if (!ok) {
        fdk_dbus_message_free(call);
        fdk_dbus_connection_close(bus);
        return 1;
    }
    /* Collect the id via the flood reply counter (one-shot). */
    flood_state st = { .want = 1, .answered = 0, .errored = 0 };
    if (fdk_dbus_connection_call(bus, call, 3000, flood_reply, &st) ==
        NULL) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    deadline = now_s() + 3.0;
    while (st.answered + st.errored < 1 && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    fdk_dbus_connection_close(bus);
    return (st.answered == 1) ? 0 : 1;
}

static int g_squat_stop;

static void squat_name_event(fdk_dbus_connection *bus, const char *name,
                             int owned, void *user) {
    (void)user;
    if (owned) {
        printf("probe: squatting %s as %s\n", name,
               fdk_dbus_connection_unique_name(bus));
        fflush(stdout);
    } else {
        printf("probe: lost %s — exiting\n", name);
        fflush(stdout);
        g_squat_stop = 1;
    }
}

static int run_squat(fdk_context *ctx, int seconds) {
    fdk_dbus_connection *bus =
        fdk_dbus_connect(ctx, FDK_DBUS_SESSION_BUS, NULL, NULL);
    if (bus == NULL) {
        fprintf(stderr, "probe: no session bus\n");
        return 1;
    }
    double deadline = now_s() + 5.0;
    while (!fdk_dbus_connection_ready(bus) && now_s() < deadline) {
        fdk_pump_events(ctx, 20);
    }
    if (!fdk_dbus_connection_ready(bus)) {
        fprintf(stderr, "probe: bus never became ready\n");
        fdk_dbus_connection_close(bus);
        return 1;
    }
    fdk_dbus_connection_set_name_events(bus, squat_name_event, NULL);

    /* flags 0: no ALLOW_REPLACEMENT — the stub that refuses. */
    fdk_dbus_message *call = fdk_dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "RequestName");
    if (call == NULL ||
        !fdk_ok(fdk_dbus_message_append_string(call, N_NAME)) ||
        !fdk_ok(fdk_dbus_message_append_uint32(call, 0))) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    fdk_dbus_pending *p =
        fdk_dbus_connection_call(bus, call, 3000, NULL, NULL);
    if (p == NULL) {
        fdk_dbus_connection_close(bus);
        return 1;
    }
    double hold_until = now_s() + seconds;
    while (!g_squat_stop && now_s() < hold_until) {
        fdk_pump_events(ctx, 100);
    }
    fdk_dbus_connection_close(bus);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: notifyd_probe flood N | squat [sec] | "
                        "icon R G B [timeout-ms]\n");
        return 2;
    }
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "probe: no display\n");
        return 1;
    }
    int rc;
    if (strcmp(argv[1], "flood") == 0) {
        rc = run_flood(ctx, argc > 2 ? atoi(argv[2]) : 40);
    } else if (strcmp(argv[1], "squat") == 0) {
        rc = run_squat(ctx, argc > 2 ? atoi(argv[2]) : 20);
    } else if (strcmp(argv[1], "icon") == 0 && argc >= 5) {
        rc = run_icon(ctx, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                      argc > 5 ? atoi(argv[5]) : 6000);
    } else {
        fprintf(stderr, "probe: unknown mode '%s'\n", argv[1]);
        rc = 2;
    }
    fdk_shutdown(ctx);
    return rc;
}
