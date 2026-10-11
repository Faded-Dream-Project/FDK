# FDK Versioning Policy

## The rule

The public version of FDK is **0.0.1** — on purpose. The number is a
project joke: *this absurdly capable toolkit is still called 0.0.1.*
It is not an alpha marker, not a stability claim, and not a maturity
signal, and it does not license any lowering of engineering
standards.

```
Public version:       0.0.1         (the joke, restored)
Engineering standard: extremely high (unchanged by the number)
```

The two axes stay decoupled in both directions: a higher version
number is never permission for lower standards, and engineering
quality is never *why* the version moves — or, as the retraction
below proved, why it should not.

## What 0.0.1 does NOT mean

- It does **not** mean the toolkit is immature: the public API and
  ABI are already governed by `docs/abi-policy.md`
  (`FDK_ABI_STABLE`, append-only, size-pinned, opaque layouts —
  keyed to the Phase 11 stabilization milestone, a *different*
  axis from the public version).
- It does **not** soften any audit: dependency, license, security,
  API, memory, DPI, accessibility, theme, installation,
  external-consumer, performance, X11, Wayland, GUI, visual
  regression, and sanitizer testing all remain fully in force.
- It does **not** license fake completion or marketing language
  beyond what is verified.
- It **does** mean: FDK makes no stable-distribution promises yet.
  The from-source build (`make`, `make install`) is the supported
  consumption path; binary packaging exists as *infrastructure*
  (`packaging/`, `docs/packaging.md`) awaiting the deliberate
  release decision, not as a published artifact.

## Internal milestones are not versions

`docs/roadmap.md` tracks work with Phase numbers and internal
milestone labels (`1.0.0`, `1.0.1`, `1.1.0`, …, `1.4.19`). These
are **engineering milestones** — history markers for when work
landed. **Disambiguation, which matters:** the *roadmap milestone
labelled "1.0.0"* (Phase 11 stabilization, 2024) is a different
thing from any *public version 1.0.0*. Milestone labels appear in
the roadmap, in commit messages, and as parenthetical history tags
in prose, but the version the toolkit reports to the world is
governed by this document alone.

## Where the public version lives

Exactly one source of truth: `include/fdk/fdk_version.h`
(`FDK_VERSION_MAJOR` / `FDK_VERSION_MINOR` / `FDK_VERSION_PATCH` /
`FDK_VERSION_STRING`). Everything else derives from it:

- `fdk_get_version()` / `fdk_get_version_string()` (runtime)
- `fdk.pc` via the Makefile's `sed` extraction (`FDK_PC_VERSION`)
- example/banner output, the benchmark harness header
- release archives and all packaging metadata
  (`docs/packaging.md`) — every recipe substitutes `@VERSION@`
  from the header; nothing hand-edits a version string.

## The SONAME policy (designed, dormant)

The shared library currently builds and installs as a single
unversioned `libfdk.so` with no SONAME — the honest pre-release
posture. The versioned scheme is designed and ready to activate
with the real public 1.0.0 decision: the real file carries the full
version (`libfdk.so.<major.minor.patch>`), the SONAME symlink
(`libfdk.so.<major>`) is what executables record as `NEEDED`, and
the development symlink (`libfdk.so`) is what pkg-config's `-lfdk`
resolves against. Under that policy, ABI-compatible releases
(minor/patch) keep the SONAME and move only the file version; a
future major that breaks the ABI moves the SONAME major in the same
commit. Because nothing depending on an FDK SONAME exists in
public distribution, activating `libfdk.so.1` at the real 1.0.0
breaks no one and invents no history. The Makefile and packaging
pipeline derive both from the header, so the scheme switches on
with the version bump itself.

## Enforcement

`tests/test_core.c::test_version` pins the public version at
`0.0.1`. A change to the version macros without the matching test
change fails the suite. Changing both is a deliberate act that
requires:

1. an explicit project-owner decision (not autonomy, not
   accumulation of features, not semver expectations, not version
   prestige, not a competitor's higher number, not a packaging
   milestone being "done enough"), and
2. updating this document to record the new decision first.

## History

- 2024: the tree briefly reported `1.0.0` … `1.1.1` because
  internal milestone labels leaked into the public version macros.
  That was a bookkeeping mistake, not a promotion decision. The
  public version was reset to `0.0.1` and pinned (roadmap milestone
  1.1.2), and this "0.0.1 on purpose" joke became the project's
  longest-running bit: *this absurdly capable toolkit is still
  called 0.0.1.*
- 2026-10-11 (morning): a distribution pass bumped the public
  version to **1.0.0**, tagged `v1.0.0`, and produced a draft
  GitHub Release with native packages across five distribution
  families.
- 2026-10-11 (same day): **retracted.** Manual testing immediately
  after the pass showed the file picker and folder picker were not
  release-ready (unfinished file-type iconography, no system
  icon-theme integration, picker-quality defects), which is exactly
  the "fake completion" this policy forbids. The `v1.0.0` tag was
  deleted from the remote, the draft release (never published, so
  no consumer ever saw it) was deleted with it, and the public
  version returned to `0.0.1`. Nothing was rewritten: the source
  commits, the finalized license, the packaging recipes, and the
  CI/release workflows from that pass all remain on `main` as
  infrastructure for the eventual, deliberate 1.0.0 — a decision
  that now also carries the lesson that *component quality, not
  pipeline greenness, gates the number.*
