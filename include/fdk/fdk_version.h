/*
 * fdk_version.h — Faded Dream ToolKit version information
 *
 * Part of the public FDK API. Applications may use these macros to
 * perform compile-time or run-time feature/compatibility checks.
 */

#ifndef FDK_VERSION_H
#define FDK_VERSION_H

/*
 * THE PUBLIC VERSION IS 0.0.1 — ON PURPOSE (docs/versioning.md).
 *
 * The number is a project joke: "this absurdly capable toolkit is
 * still called 0.0.1." It is NOT an alpha marker, NOT a stability
 * claim, and NOT a maturity signal, and it does not license any
 * lowering of engineering standards.
 *
 * History, recorded in full in docs/versioning.md: a 2026-10-11
 * v1.0.0 bump was made as part of a distribution pass and
 * RETRACTED the same day — the premature v1.0.0 tag and its
 * (never-published, draft-only) release were deleted once manual
 * testing showed the file picker and the icon-theme story were
 * not release-ready. The version returned to 0.0.1; the packaging
 * and CI infrastructure from that pass remains for the eventual,
 * deliberate 1.0.0 decision.
 *
 * The bump discipline is unchanged: these macros change only by
 * an explicit project-owner decision, recorded in
 * docs/versioning.md first — not because functionality accumulated,
 * a phase completed, a competitor's version is higher, or semver
 * expectations say so. Internal engineering milestones (the Phase
 * numbers and the 1.x labels used in docs/roadmap.md) are a
 * different axis entirely: they track work, never the public
 * version. The test suite pins this value (tests/test_core.c) so
 * accidental bumps fail the build instead of shipping quietly.
 */
#define FDK_VERSION_MAJOR 0
#define FDK_VERSION_MINOR 0
#define FDK_VERSION_PATCH 1

/* "0.0.1" style string, generated from the numeric components. */
#define FDK_VERSION_STRING "0.0.1"

/* Encode major/minor/patch into a single comparable integer:
 * (major * 1000000) + (minor * 1000) + patch */
#define FDK_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))

#define FDK_VERSION \
    FDK_VERSION_ENCODE(FDK_VERSION_MAJOR, FDK_VERSION_MINOR, FDK_VERSION_PATCH)

/*
 * FDK's ABI is stable as of the Phase 11 stabilization pass (the
 * "1.0.0" of docs/roadmap.md — an internal engineering milestone,
 * NOT the public version, which stays 0.0.1 by policy): public
 * structs are classified and size-pinned (docs/abi-policy.md),
 * enums are append-only, input structs append-only, and the object
 * layouts stay opaque. See docs/abi-policy.md for the full rules and
 * the subclassing decision. The shared library carries no SONAME
 * yet: the versioned-SONAME scheme (libfdk.so.<major>) is designed
 * and documented, and activates with the real public 1.0.0
 * decision (docs/versioning.md records the retracted first
 * attempt and what it taught us).
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
