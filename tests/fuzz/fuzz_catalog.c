/*
 * fuzz_catalog.c — libFuzzer harness for the .fmo catalog parser
 * (docs/fuzzing.md).
 *
 * Target: fdk_catalog_parse() — the public parse-from-memory entry
 * for the translation-catalog binary format (docs/fdk-catalog-
 * format.md). Catalogs are downloaded data in the real world (an
 * app's translation update), so the same contract as themes holds:
 * at worst it fails to parse.
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"

#include <stdint.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fdk_catalog *cat = NULL;
    fdk_result r = fdk_catalog_parse(data, size, &cat);
    if (fdk_ok(r) && cat != NULL) {
        /* Touch the parsed surface so the harness exercises the
         * accessors too: lookups on a garbage-parsed catalog must
         * stay in bounds by construction. */
        (void)fdk_catalog_get(cat, "x");
        (void)fdk_catalog_has(cat, "x");
        (void)fdk_catalog_get_plural(cat, NULL, "x", 2);
        (void)fdk_catalog_get_in_context(cat, "x", "c");
        (void)fdk_catalog_entry_count(cat);
        fdk_catalog_destroy(cat);
    }
    return 0;
}
