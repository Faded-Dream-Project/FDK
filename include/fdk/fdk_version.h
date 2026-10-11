/*
 * fdk_version.h — Faded Dream ToolKit version information
 *
 * Part of the public FDK API. Applications may use these macros to
 * perform compile-time or run-time feature/compatibility checks.
 */

#ifndef FDK_VERSION_H
#define FDK_VERSION_H

/*
 * THE PUBLIC VERSION IS 1.0.0 — the first stable public release
 * (adopted 2026-10-11 for the v1.0.0 distribution milestone; see
 * docs/versioning.md for the decision record, which also tells the
 * story of the retired "0.0.1 joke" era this replaced).
 *
 * The bump discipline survives the bump: these macros change only
 * by an explicit project-owner decision, recorded in
 * docs/versioning.md first — not because functionality accumulated,
 * a phase completed, a competitor's version is higher, or semver
 * expectations say so. Internal engineering milestones (the Phase
 * numbers and the 1.x labels used in docs/roadmap.md) remain a
 * different axis entirely: they track work, never the public
 * version. Disambiguation: the historical roadmap milestone labelled
 * "1.0.0" (Phase 11 stabilization) is NOT this version; the public
 * v1.0.0 is the 2026 release decision. The test suite pins this
 * value (tests/test_core.c) so accidental bumps fail the build
 * instead of shipping quietly.
 */
#define FDK_VERSION_MAJOR 1
#define FDK_VERSION_MINOR 0
#define FDK_VERSION_PATCH 0

/* "1.0.0" style string, generated from the numeric components. */
#define FDK_VERSION_STRING "1.0.0"

/* Encode major/minor/patch into a single comparable integer:
 * (major * 1000000) + (minor * 1000) + patch */
#define FDK_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))

#define FDK_VERSION \
    FDK_VERSION_ENCODE(FDK_VERSION_MAJOR, FDK_VERSION_MINOR, FDK_VERSION_PATCH)

/*
 * FDK's ABI is stable as of the Phase 11 stabilization pass (an
 * internal engineering milestone — see the disambiguation note
 * above): public
 * structs are classified and size-pinned (docs/abi-policy.md),
 * enums are append-only, input structs append-only, and the object
 * layouts stay opaque. See docs/abi-policy.md for the full rules and
 * the subclassing decision. As of public v1.0.0 the shared library
 * carries a real SONAME (libfdk.so.1, set in the Makefile) whose
 * major tracks ABI-breaking changes per docs/versioning.md's
 * soname policy.
 */
#define FDK_ABI_STABLE 1

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the runtime library version, encoded as FDK_VERSION_ENCODE().
 * Lets an application detect a mismatch between the headers it was built
 * against and the shared library it loaded at run time. */
int fdk_get_version(void);

/* Returns the runtime library version as a static, non-owned string
 * (e.g. "1.0.0"). Never returns NULL. */
const char *fdk_get_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* FDK_VERSION_H */
