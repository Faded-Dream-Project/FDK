# FDK Versioning Policy

## The rule

FDK is **in development — no stable release yet.** The public version
is whatever `include/fdk/fdk_version.h` says — currently a `1.1.0`
pre-release iteration (`rcN`) carrying the owner's 2026-10-25
directive that FDK grow a public **extension API** (add-on packages
building on `fdk/extend.h`); the owner explicitly chose `1.1.0-rc1`
over `1.0.1` because new public API is a minor bump ("not 1.0.1,
since this adds public API"). The `-rcN` suffix is the owner's
brake: **no git tag exists and nothing is published** — the project
has no releases page, no release branch, and no tags; the owner
decides if and when a first stable release is ever cut, and how it
is numbered. Every version move is made exactly the way this
document demands: by the project owner, explicitly — not by
autonomy, feature accumulation, or semver pressure. (For the entire
life before the 1.0.0 era the number was a project joke (`0.0.1` —
*this absurdly capable toolkit is still called 0.0.1*.)

```
Public version:       1.1.0-rcN     (development iteration; no tag, no release)
Engineering standard: extremely high (unchanged by the number)
```

The two axes stay decoupled in both directions: a higher version
number is never permission for lower standards, and engineering
quality is never *why* the version moves — or, as the retraction
below proved, why it should not.

The extension-API additions are ABI-append-only (opaque types, new
functions, `struct_size`-forwarded input structs), so the SONAME
stays `libfdk.so.1`; the file name moves to the numeric
`libfdk.so.1.1.0` with the SONAME symlink unchanged (the .so file
name is always numeric MAJOR.MINOR.PATCH — pre-release suffixes
never appear in it; they live in the version string, pkg-config,
and package metadata).

## What the 1.0.0 decision rested on (historical)

The 2026-10-11 retraction named its blockers precisely: the file
and folder pickers were not release-ready (unfinished file-type
iconography, no system icon-theme integration, picker-quality
defects). The milestones that closed them — picker stabilization
(1.5.1), the audit pass (the default detection channel, SVG
rasterization, the refusal set — 1.5.2), the polish pass (per-pixel
alpha compositing, the live XSETTINGS desktop — 1.5.3), and the
crash fix (CSS-class SVGs — 1.5.4) — are recorded in
`docs/roadmap.md`, each pixel-verified and regression-pinned. The
project owner then tested the result on real hardware (native
Wayland under labwc included) and demanded the release. Component
closed the gate, not pipeline greenness — the lesson the retraction
taught, applied. That decision, and the 1.0.0 releases that followed
it, belonged to the project's former repository; when the project
returned to in-development status (2026-10-31, below), all of that
release material went with it. What remains true is the lesson:
component quality, not pipeline greenness, gates any future number.

With the 1.0.0-era decision the **SONAME scheme activated**
(`libfdk.so.1`) and the packaging pipeline left its disclosed
`PACKAGING-SKIP` posture; both survive as development-time policy —
the recipes are exercised by CI on every push as validation checks,
with nothing published anywhere.

## What a version number does NOT mean

- It does **not** soften any audit: dependency, license, security,
  API, memory, DPI, accessibility, theme, installation,
  external-consumer, performance, X11, Wayland, GUI, visual
  regression, and sanitizer testing all remain fully in force.
- It does **not** license fake completion or marketing language
  beyond what is verified.
- In the current, in-development posture it does **not** imply any
  stable-distribution promise: there is no published release to
  promise with. The ABI contract (`docs/abi-policy.md`,
  `FDK_ABI_STABLE`, the SONAME, append-only rules) is maintained
  as development-time discipline, and the from-source build
  (`make`, `make install`) is the supported consumption path; the
  packaging recipes (`packaging/`, `docs/packaging.md`) are kept
  buildable by CI, nothing more.

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
- all packaging metadata built by the CI packaging checks
  (`docs/packaging.md`) — every recipe substitutes `@VERSION@`
  from the header; nothing hand-edits a version string.

## The SONAME policy (the active development-time scheme)

The shared library builds and installs under the versioned scheme:
the real file carries the full version (`libfdk.so.<major.minor.patch>`),
the SONAME symlink (`libfdk.so.<major>`) is what executables record
as `NEEDED`, and the development symlink (`libfdk.so`) is what
pkg-config's `-lfdk` resolves against. Under that policy,
ABI-compatible releases (minor/patch) keep the SONAME and move only
the file version; a future major that breaks the ABI moves the
SONAME major in the same commit. Because nothing depending on an
FDK SONAME existed in public distribution before 1.0.0, activating
`libfdk.so.1` broke no one and invented no history. The Makefile
and packaging pipeline derive both from the header.

## Enforcement

`tests/test_core.c::test_version` pins the public version (it
asserts against the `fdk_version.h` macros themselves). A change to
the version macros without the matching
test change fails the suite. Changing both is a deliberate act that
requires:

1. an explicit project-owner decision (not autonomy, not
   accumulation of features, not semver expectations, not version
   prestige, not a competitor's higher number, not a packaging
   milestone being "done enough"), and
2. updating this document to record the new decision first.

The 1.0.0 bump of 2026-10-12 satisfied both: the owner demanded the
release after verifying the picker work on real hardware, and this
document was updated in the same commit that moved the macros.

The 1.1.0-rc1 bump of 2026-10-25 satisfies both again: the owner set
the release target and the number in the same breath as the feature
directive ("Bump FDK to 1.1.0-rc1 (not 1.0.1, since this adds public
API)... Do NOT create or move any git tag and do NOT touch the 1.0.0
release. I'll decide on the final tag.") — that directive's tag
embargo posture is the one the project still keeps today: no tags,
no releases, until the owner says otherwise.

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
- 2026-10-12: **the real 1.0.0.** The retraction's named blockers
  were closed by the picker overhaul (milestones 1.5.1 through
  1.5.4: file-type iconography + system icon-theme integration,
  SVG rasterization with the refusal set, per-pixel alpha
  compositing + the live XSETTINGS desktop, and the CSS-class SVG
  crash fix — each pixel-verified and regression-pinned), the
  project owner validated the result on real hardware (native
  Wayland under labwc included) and explicitly demanded the
  release, and this document recorded the decision in the same
  commit that moved the macros. The versioned-SONAME scheme
  (`libfdk.so.1`) activated with it; the `v1.0.0` tag was re-created
  on the decision commit and pushed to the main repository.
- 2026-10-12 (same day, hours later): **the 1.0.0 alignment — one
  shipped face.** The owner inspected the live release and set the
  product posture: FDK ships exactly ONE theme — `faded-dream`, the
  packaged copy of the built-in default — and the next faces are
  `.FDKThemes` drop-ins the user installs, never package content
  ("it needs to ship only one the next ones must be in (.FDKthemes)
  btw the default one being Faded-dream"). The version number did
  NOT move — a scope decision is not a version decision; the
  release was re-cut instead: `SHIPPED_THEMES` in the Makefile
  pinned to the single file (a stray .fdk can no longer ride a
  wildcard into a release), the source tree's `.FDKThemes/`
  reduced to `faded-dream.fdk`, the nine other faces preserved as
  engine fixtures under `tests/data/faces/` (never installed, their
  pins carried over to the fixture corpus in test_shipped_themes.c),
  and the `v1.0.0` tag re-created on the alignment commit with the
  release assets replaced on the main repository.
- 2026-10-25: **1.1.0-rc1 — the extension API candidate.** The owner
  directed the next version to add a public extension API
  (`include/fdk/fdk_extend.h`) so add-on packages can be built on top
  of FDK — proved by fdk-shell, a separate layer-shell package that
  compiles against FDK's installed public headers and library only —
  and set the number explicitly: 1.1.0-rc1, a MINOR bump because the
  change ADDS public API (the owner's own words ruled out 1.0.1).
  The additions are ABI-append-only (new
  functions, opaque types, no layout changes to existing public
  structs), so the SONAME stays `libfdk.so.1`.
- 2026-10-26: **1.1.0-rc1 → 1.1.0-rc2 (rcN bump, same candidate).**
  The owner's Phase 2 directive authorized rcN bumps freely ("Bump
  FDK to 1.1.0-rc1 ... bumping rcN is fine"). rc2 marks the native
  D-Bus client (the M-7 foundation): 115 new append-only public
  symbols in include/fdk/fdk_dbus.h, plus the additive
  fdk_ext_loop_update_fd() (FDK_EXT_API_VERSION stays 1 — no
  ops-table growth). SONAME stays libfdk.so.1.
- 2026-10-31: **back to in development — the clean re-publish.** The
  project owner returned FDK to in-development status: the former
  repository (with its v1.0.0 tag, its GitHub releases, and its
  `release` branch) was retired, and the history was re-published
  fresh — one branch, `main`, zero tags, no releases, no published
  artifacts of any kind. The version macros are untouched (still the
  1.1.0 rcN iteration); they describe the development tree, not a
  release. This document, the README, and the CI workflows were
  reworded to match: nothing anywhere claims a published release.
