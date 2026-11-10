#include "fdk/fdk_version.h"
#include "fdk/unstable.h" /* the unstable tier residents */

int fdk_get_version(void) {
    return FDK_VERSION;
}

const char *fdk_get_version_string(void) {
    return FDK_VERSION_STRING;
}


/* ---- the explicitly unstable tier (include/fdk/unstable.h) ----------------
 *
 * The residents live in the FDK_UNSTABLE version node (the build's
 * second version script); the contract is "may break any release" —
 * the functions exist so the tier has honest, testable members on
 * day one, not as a placeholder. */

int fdk_unstable_api_level(void) {
    return FDK_UNSTABLE_API_LEVEL;
}

const char *fdk_unstable_tier_name(void) {
    return "FDK_UNSTABLE";
}
