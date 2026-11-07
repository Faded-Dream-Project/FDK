/*
 * test_idle.c — the idle-inhibition module's headless battery (1.1.0, S-3)
 *
 * What plain `make test` can pin without a display or a bus: the
 * argument-safety contracts and the NULL-handle reads. The real
 * coverage lives in the rigs:
 *   - tests/test_dbus_live.c (make test-dbus): a mock
 *     org.freedesktop.ScreenSaver — the cookie round trip, the
 *     DEFERRED release (uninhibit before the cookie lands still
 *     sends the matching UnInhibit — never an orphaned inhibition),
 *     and the error path.
 *   - tests/test_wayland_integration.c (the sway rig): the
 *     compositor path against sway's real zwp_idle_inhibit_manager.
 *   - tests/test_x11_integration.c: the X11 world (no platform op)
 *     engaging the D-Bus fallback shape and staying healthy.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_idle.h"

#include <assert.h>
#include <stdio.h>

static void test_argument_safety(void) {
    /* NULL context / NULL out: refused, nothing written. */
    fdk_idle_inhibitor *h = (fdk_idle_inhibitor *)0x1;
    assert(fdk_idle_inhibit(NULL, NULL, "a", "r", &h) ==
           FDK_ERR_INVALID_ARGUMENT);
    assert(h == (fdk_idle_inhibitor *)0x1);
}

static void test_null_handle_reads(void) {
    /* The NULL-handle contract: status reads UNSUPPORTED, release
     * is a no-op. */
    assert(fdk_idle_inhibitor_status(NULL) ==
           FDK_IDLE_INHIBIT_UNSUPPORTED);
    fdk_idle_uninhibit(NULL); /* must not crash */
    printf("[ok] idle: NULL-handle reads + no-op release\n");
}

int main(void) {
    test_argument_safety();
    printf("[ok] idle: argument safety (NULL ctx/out refused)\n");
    test_null_handle_reads();
    printf("all idle tests passed\n");
    return 0;
}
