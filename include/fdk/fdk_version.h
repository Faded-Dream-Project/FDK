/*
 * fdk_version.h — Faded Dream ToolKit version information
 *
 * Part of the public FDK API. Applications may use these macros to
 * perform compile-time or run-time feature/compatibility checks.
 */

#ifndef FDK_VERSION_H
#define FDK_VERSION_H

#define FDK_VERSION_MAJOR 0
#define FDK_VERSION_MINOR 1
#define FDK_VERSION_PATCH 0

/* "0.1.0" style string, generated from the numeric components. */
#define FDK_VERSION_STRING "0.1.0"

/* Encode major/minor/patch into a single comparable integer:
 * (major * 1000000) + (minor * 1000) + patch */
#define FDK_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))

#define FDK_VERSION \
    FDK_VERSION_ENCODE(FDK_VERSION_MAJOR, FDK_VERSION_MINOR, FDK_VERSION_PATCH)

/*
 * FDK is pre-ABI-stable during the 0.x series. ABI/API may break between
 * minor versions until FDK_VERSION_MAJOR reaches 1. See docs/abi-policy.md.
 */
#define FDK_ABI_STABLE 0

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the runtime library version, encoded as FDK_VERSION_ENCODE().
 * Lets an application detect a mismatch between the headers it was built
 * against and the shared library it loaded at run time. */
int fdk_get_version(void);

/* Returns the runtime library version as a static, non-owned string
 * (e.g. "0.1.0"). Never returns NULL. */
const char *fdk_get_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* FDK_VERSION_H */
