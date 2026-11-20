/*
 * fdk_version.h — Faded Dream ToolKit version information
 *
 * Part of the public FDK API. Applications may use these macros to
 * perform compile-time or run-time feature/compatibility checks.
 */

#ifndef FDK_VERSION_H
#define FDK_VERSION_H

/*
 * THE PUBLIC VERSION IS 1.0.0-dev — DEVELOPMENT POSTURE AGAIN
 * (docs/versioning.md). The 1.0.0 release was cut THREE times and
 * withdrawn three times (2026-11-09, 2026-11-12, 2026-11-19); the
 * last withdrawal (commit 8ecd83f, recorded in CHANGELOG.md) came
 * after the 2026-11-18 re-release shipped six GL defects found
 * downstream. NO TAG EXISTS and no release is published — the
 * project is in development until the owner says otherwise. The
 * 2026-11-19 suffix decision records why "-dev" and not another
 * "-rcN": an rc implies a candidate standing for release review;
 * development on main is not that. The suffix makes every build
 * from main self-identify as unreleased ("1.0.0-dev" in the
 * version string, pkg-config, and package metadata) so a bug
 * report against a withdrawn number can never be mistaken for a
 * bug in a shipped release.
 *
 * History, recorded in full in docs/versioning.md: the number was
 * a deliberate joke (0.0.1, "this absurdly capable toolkit is still
 * called 0.0.1") until the 2026-10-12 decision — after a retracted
 * premature 1.0.0 pass whose named blockers were closed by the
 * picker overhaul, the owner validated the result on real hardware
 * (native Wayland under labwc included) and demanded the release,
 * then set the one-face posture that re-cut its assets.
 *
 * The bump discipline is unchanged: these macros change only by
 * an explicit project-owner decision, recorded in
 * docs/versioning.md first — not because functionality accumulated,
 * a phase completed, a competitor's version is higher, or semver
 * expectations say so. Internal engineering milestones (the Phase
 * numbers and the 1.x labels used in ROADMAP.md) are a
 * different axis entirely: they track work, never the public
 * version. The test suite pins this value (tests/test_core.c) so
 * accidental bumps fail the build instead of shipping quietly.
 */
#define FDK_VERSION_MAJOR 1
#define FDK_VERSION_MINOR 0
#define FDK_VERSION_PATCH 0

/* "1.0.0-dev" style string, generated from the numeric components
 * plus the pre-release suffix. The suffix is "-dev" while main is
 * in development (no tag, no release); it returns to "" only by
 * the owner's release decision, recorded in docs/versioning.md
 * first. The suffix lives here, next to the numeric triple it
 * decorates. */
#define FDK_VERSION_SUFFIX "-dev"
#define FDK_VERSION_STRING "1.0.0-dev"

/* Encode major/minor/patch into a single comparable integer:
 * (major * 1000000) + (minor * 1000) + patch */
#define FDK_VERSION_ENCODE(major, minor, patch) \
    (((major) * 1000000) + ((minor) * 1000) + (patch))

#define FDK_VERSION \
    FDK_VERSION_ENCODE(FDK_VERSION_MAJOR, FDK_VERSION_MINOR, FDK_VERSION_PATCH)

/*
 * FDK's ABI is stable as of the Phase 11 stabilization pass (the
 * "1.0.0" of ROADMAP.md — an internal engineering milestone,
 * distinct from but now aligned with the public version): public
 * structs are classified and size-pinned (docs/abi-policy.md),
 * enums are append-only, input structs append-only, and the object
 * layouts stay opaque. See docs/abi-policy.md for the full rules and
 * the subclassing decision. Since the public 1.0.0 decision of
 * 2026-10-12 the shared library carries the versioned SONAME
 * scheme: the real file is libfdk.so.<major.minor.patch>, the
 * SONAME symlink libfdk.so.<major> is what executables record as
 * NEEDED, and libfdk.so is the development symlink (docs/versioning.md
 * records the retracted first attempt and what it taught us).
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
