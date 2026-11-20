/*
 * fuzz_theme.c — libFuzzer harness for the .fdk theme parser
 * (docs/fuzzing.md).
 *
 * Target: fdk_theme_parse() — the public parse-from-memory entry.
 * The parser's contract (docs/security.md) is that a theme file is
 * data: at worst it fails to load. This harness feeds arbitrary
 * bytes and asserts only that the call returns (crash/hang/leak
 * detection rides on the sanitizer build); a NULL return with any
 * error code is a normal, expected outcome for garbage input.
 *
 * The hermetic pin matters here as much as in the suite: a
 * developer's environment must not change what the parser does
 * with the same bytes.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* The public cap is 1 MiB; larger inputs are refused before
     * parsing, so clamping keeps the corpus interesting rather
     * than spending every run in the early-return path. */
    if (size > 1024 * 1024) {
        size = 1024 * 1024;
    }
    fdk_result err = FDK_OK;
    fdk_theme *t = fdk_theme_parse((const char *)data, size, &err);
    if (t != NULL) {
        fdk_theme_destroy(t);
    }
    return 0;
}
