/*
 * fuzz_prefs.c — libFuzzer harness for the .prefs parser
 * (docs/fuzzing.md).
 *
 * Target: the prefs file dialect parsed by prefs_parse() inside
 * src/core/prefs.c. There is no public parse-from-memory entry —
 * the parser is only reachable through fdk_prefs_open(), which
 * reads $FDK_PREFS_FILE when it is an absolute path (the same
 * override the hermetic pin uses). The harness writes the fuzz
 * case to a scratch file, points the env var at it, opens the
 * store, and touches a few getters so a successfully-parsed
 * garbage file is actually walked.
 *
 * The contract (docs/security.md's prefs half, and fdk_prefs.h's
 * "defaults rather than half-parsed values") is: at worst a
 * rejected file becomes an empty store with one log line.
 *
 * The file round-trip makes this the slowest harness per exec;
 * the CI job gives it the same short budget as the others, which
 * still buys thousands of cases.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SCRATCH "/tmp/fdk-fuzz-prefs.prefs"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The reader bounds the file itself; skip inputs so large the
     * write dominates the budget without exercising the parser. */
    if (size > 64 * 1024) {
        return 0;
    }
    FILE *f = fopen(SCRATCH, "wb");
    if (f == NULL) {
        return 0;
    }
    fwrite(data, 1, size, f);
    fclose(f);

    if (setenv("FDK_PREFS_FILE", SCRATCH, 1) != 0) {
        return 0;
    }
    fdk_prefs *p = NULL;
    fdk_result r = fdk_prefs_open("fuzz-app", &p);
    unsetenv("FDK_PREFS_FILE");
    if (fdk_ok(r) && p != NULL) {
        (void)fdk_prefs_get(p, "k", NULL);
        (void)fdk_prefs_get_int(p, "k", 0);
        (void)fdk_prefs_get_bool(p, "k", false);
        (void)fdk_prefs_get_double(p, "k", 0.0);
        (void)fdk_prefs_count(p);
        fdk_prefs_destroy(p);
    }
    return 0;
}
