# FDK Versioning Policy

## The rule

The public version of FDK is **1.0.0** — the first stable public
release, adopted 2026-10-11 by an explicit project-owner decision
for the v1.0.0 distribution milestone (source archives + native
binary packages + the release pipeline; see `docs/packaging.md`).

```
Public version:      1.0.0         (semver, from here on)
Engineering standard: extremely high  (unchanged by the number)
```

The two axes stay decoupled in both directions: a higher version
number is never permission for lower standards, and engineering
quality was never *why* the version moved. The bump happened because
the owner decided FDK is ready to be *distributed and depended on*
as a stable 1.x — which brings compatibility obligations the number
now has to honor (below).

## What 1.0.0 does NOT mean

- It does **not** mean every imaginable feature is implemented — the
  roadmap (`docs/roadmap.md`) still tracks what is deliberately not
  covered, and the OUT-for-now list remains honest.
- It does **not** soften any audit: dependency, license, security,
  API, memory, DPI, accessibility, theme, installation,
  external-consumer, performance, X11, Wayland, GUI, visual
  regression, and sanitizer testing all remain fully in force.
- It does **not** license fake completion or marketing language
  beyond what is verified.
- It **does** mean: within the 1.x series, the public API and ABI
  are stable per `docs/abi-policy.md` (append-only, size-pinned,
  opaque layouts), and releases follow semantic versioning:
  - **patch** (1.0.x): fixes only, no new API;
  - **minor** (1.x.0): additive API, ABI-compatible, SONAME
    unchanged;
  - **major** (x.0.0): the compatibility epoch breaks; the SONAME
    major moves with it.

## Internal milestones are not versions

`docs/roadmap.md` tracks work with Phase numbers and internal
milestone labels (`1.0.0`, `1.0.1`, `1.1.0`, …, `1.4.19`). These
are **engineering milestones** — history markers for when work
landed. **Disambiguation, which now matters:** the *roadmap
milestone labelled "1.0.0"* (Phase 11 stabilization, 2024) is a
different thing from the *public version 1.0.0* adopted 2026-10-11
by this policy. Milestone labels appear in the roadmap, in commit
messages, and as parenthetical history tags in prose, but the
version the toolkit reports to the world is governed by this
document alone.

## Where the public version lives

Exactly one source of truth: `include/fdk/fdk_version.h`
(`FDK_VERSION_MAJOR` / `FDK_VERSION_MINOR` / `FDK_VERSION_PATCH` /
`FDK_VERSION_STRING`). Everything else derives from it:

- `fdk_get_version()` / `fdk_get_version_string()` (runtime)
- `fdk.pc` via the Makefile's `sed` extraction (`FDK_PC_VERSION`)
- example/banner output, the benchmark harness header
- release archives and all packaging metadata
  (`docs/packaging.md`)

## The SONAME policy (adopted with 1.0.0)

The shared library carries a real SONAME since 1.0.0:
**`libfdk.so.<FDK_VERSION_MAJOR>`** — currently `libfdk.so.1`. The
installed layout is the standard one: the versioned real file
(`libfdk.so.1.0.0`), the SONAME symlink executables record as
`NEEDED` (`libfdk.so.1`), and the development symlink
pkg-config's `-lfdk` resolves against (`libfdk.so`). Before 1.0.0
the library shipped as a single unversioned `libfdk.so` with no
SONAME — acceptable for a from-source toolkit, wrong for binary
packaging.

The ABI itself was frozen long before the number caught up
(`FDK_ABI_STABLE`, `docs/abi-policy.md`, keyed to the Phase 11
stabilization). Because nothing depending on an FDK SONAME existed
in public distribution before 1.0.0, introducing
`libfdk.so.1` now breaks no one and invents no history: 1.x is
simply the first compatibility epoch. Minor and patch releases keep
the SONAME and move only the file version; a future 2.0.0 that
breaks the ABI moves the SONAME major in the same commit, and the
test suite plus the packaging pipeline carry both automatically
(they derive the SONAME from the header).

## Enforcement

`tests/test_core.c::test_version` pins the public version at
`1.0.0`. A change to the version macros without the matching test
change fails the suite. Changing both is a deliberate act that
requires:

1. an explicit project-owner decision (not autonomy, not
   accumulation of features, not semver expectations, not version
   prestige, not a competitor's higher number), and
2. updating this document to record the new decision first.

## History

- 2024: the tree briefly reported `1.0.0` … `1.1.1` because
  internal milestone labels leaked into the public version macros.
  That was a bookkeeping mistake, not a promotion decision. The
  public version was reset to `0.0.1` and pinned (roadmap milestone
  1.1.2), and this "0.0.1 on purpose" joke became the project's
  longest-running bit: *this absurdly capable toolkit is still
  called 0.0.1.*
- 2026-10-11: the owner retired the joke by adopting **1.0.0** for
  the first public distribution milestone — release pipeline, native
  packages across five distribution families, finalized license
  (`LICENSE`), and this policy. The joke's real legacy survives in
  the discipline above: the number was never allowed to mean
  anything about quality in either direction.
