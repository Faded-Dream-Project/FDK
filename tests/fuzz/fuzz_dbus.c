/*
 * fuzz_dbus.c — libFuzzer harness for the D-Bus wire parser
 * (docs/fuzzing.md).
 *
 * Target: fdk__dbus_message_parse() — the internal unmarshal entry
 * (src/dbus/dbus_message.c) that turns socket bytes into an
 * fdk_dbus_message. This is the most exposed parser in the
 * toolkit: the other end of a D-Bus connection is the session bus
 * and any peer on it, and the wire reference (the freedesktop
 * specification) is exactly what a hostile peer can violate.
 * The harness includes src/ internal headers the way the suite's
 * own tests do (precedent: window_internal.h in test files).
 *
 * The harness also round-trips every successfully-parsed message
 * through read-back of a few fields, so a message that parses but
 * carries inconsistent lengths is walked, not just allocated.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
/* dbus_internal.h needs FDK_LOG_TAG defined first (log_internal.h),
 * exactly like tests/test_dbus.c. */
#define FDK_LOG_TAG "dbus-fuzz"
#include "dbus/dbus_internal.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16 || size > 1024 * 1024) {
        return 0; /* below the header floor / far above any frame */
    }
    size_t consumed = 0;
    int oom = 0;
    fdk_dbus_message *m = fdk__dbus_message_parse(data, size,
                                                   &consumed, &oom);
    if (m != NULL) {
        (void)fdk_dbus_message_kind(m);
        (void)fdk_dbus_message_serial(m);
        fdk_dbus_message_free(m);
    }
    return 0;
}
