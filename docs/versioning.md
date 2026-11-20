# FDK Versioning Policy

## The rule

FDK is **in development — no stable release exists.** The public
version is whatever `include/fdk/fdk_version.h` says — currently
`1.0.0-dev`: the numeric triple `1.0.0` plus the `-dev` suffix
(the 2026-11-19 suffix decision, below). **No git tag exists and no
release is published.** Every version move is made exactly the way
this document demands: by the project owner, explicitly — not by
autonomy, feature accumulation, or semver pressure.

```
Public version:       1.0.0-dev    (development; no tag, no release)
Engineering standard: extremely high (unchanged by the number)
```

The two axes stay decoupled in both directions: a higher version
number is never permission for lower standards, and engineering
quality is never *why* the version moves.

## The 2026-11-19 suffix decision (why "-dev")

The 1.0.0 release has now been cut and withdrawn three times:

| Date | What happened |
| --- | --- |
| 2026-11-09 | First 1.0.0 tag + release; withdrawn by the owner the same day, back to development |
| 2026-11-12 | Re-cut and published as final 1.0.0; that release belonged to the project's former repository and did not survive the 2026-10-31 return to development posture (all release material went with it) |
| 2026-11-18 | Re-release; **withdrawn 2026-11-19** (commit `8ecd83f`, recorded in CHANGELOG.md) after Halcyon — the first downstream integrator — found six GL defects in it. All six are fixed on main; the release cut with them stays withdrawn. The tag and release are deleted. |

A tree whose version string reads exactly `1.0.0` while no release
exists is a hazard: any bug report, distro package, or downstream
build from main would be indistinguishable from a bug in a real
1.0.0. The suffix fixes that: builds from main self-identify as
`1.0.0-dev` in `fdk_get_version_string()`, pkg-config `Version:`,
and every package family's metadata. `-dev` was chosen over another
`-rcN` deliberately: an rc is a *candidate standing for release
review*, and development on main is not that.

The suffix changes only the version string, pkg-config, and package
**metadata**. The shared-library file name stays numeric
MAJOR.MINOR.PATCH (see the SONAME policy below), so `-dev` never
appears in a `.so` filename.

## What a version number does NOT mean

- It does **not** soften any audit: dependency, license, security,
  API, memory, DPI, accessibility, theme, installation,
  external-consumer, performance, X11, Wayland, GUI, visual
  regression, and sanitizer testing all remain fully in force.
- It does **not** license fake completion or marketing language
  beyond what is verified.
- It does **not** imply any stable-distribution promise: there is
  no published release to promise with. The ABI contract
  (`docs/abi-policy.md`, `FDK_ABI_STABLE`, the SONAME, append-only
  rules) is maintained as development-time discipline, and the
  from-source build (`make`, `make install`) is the supported
  consumption path; the packaging recipes (`packaging/`,
  `docs/packaging.md`) are kept buildable by CI, nothing more.

## Internal milestones are not versions

`ROADMAP.md` tracks work with Phase numbers and internal milestone
labels. These are **engineering milestones** — history markers for
when work landed, distinct from any public version. Milestone
labels appear in the roadmap, in commit messages, and as
parenthetical history tags in prose, but the version the toolkit
reports to the world is governed by this document alone.

## Where the public version lives

Exactly one source of truth: `include/fdk/fdk_version.h`
(`FDK_VERSION_MAJOR` / `FDK_VERSION_MINOR` / `FDK_VERSION_PATCH` /
`FDK_VERSION_SUFFIX` / `FDK_VERSION_STRING`). Everything else
derives from it:

- `fdk_get_version()` / `fdk_get_version_string()` (runtime)
- `fdk.pc` via the Makefile's `sed` extraction (`FDK_PC_VERSION`)
- example/banner output, the benchmark harness header
- all packaging metadata built by the CI packaging checks
  (`docs/packaging.md`) — every recipe substitutes `@VERSION@`
  from the header; nothing hand-edits a version string

The five package families map a hyphenated raw version through
`packaging/ci/common.sh::fdk_packver` into their native pre-release
grammars (deb/rpm `1.0.0~dev`, apk `1.0.0_dev`, arch/xbps
`1.0.0dev`); a plain numeric version passes through every grammar
unchanged, so at a final release all five spell the same thing.

## The SONAME policy (the active development-time scheme)

The shared library builds and installs under the versioned scheme:
the real file carries the full numeric version
(`libfdk.so.<major.minor.patch>` — never a pre-release suffix; the
GNU convention, and Alpine's abuild derives the apk `so:provides`
version from the filename tail, where a suffixed tail was proven
live on CI, 2026-10-25, to make apk index reject the package), the
SONAME symlink (`libfdk.so.<major>`) is what executables record as
`NEEDED`, and the development symlink (`libfdk.so`) is what
pkg-config's `-lfdk` resolves against. Under that policy,
ABI-compatible releases (minor/patch) keep the SONAME and move only
the file version; a future major that breaks the ABI moves the
SONAME major in the same commit. The Makefile and packaging
pipeline derive all of these from the header.

## Enforcement

`tests/test_core.c::test_version` pins the public version (it
asserts against the `fdk_version.h` macros themselves). A change to
the version macros without the matching test change fails the
suite. Changing both is a deliberate act that requires:

1. an explicit project-owner decision (not autonomy, not
   accumulation of features, not semver expectations, not version
   prestige, not a competitor's higher number, not a packaging
   milestone being "done enough"), and
2. updating this document to record the new decision first.

The 2026-11-19 `-dev` suffix decision satisfied both: the owner
ordered the version hygiene pass after the third withdrawal, and
this document records the decision in the same commit that moved
the macros.

## PROPOSED (not adopted): pre-release policy for the next release attempt

*This section is a proposal the owner has not adopted. Nothing in
it is policy until the owner says so. It is recorded here so the
decision has a concrete starting point.*

The proposal: the next release attempt goes through tagged release
candidates published as GitHub **pre-releases**, with the tag format
`v1.0.0-rc.N` and the in-tree version string `1.0.0-rc.N`.

- **Candidate cadence.** An rc is cut when the owner decides the
  tree is a candidate, not on a calendar. Each rc is tagged, built
  by `release.yml` as a pre-release, and left up for the exit
  window.
- **Exit criterion (the gate from rc to final).** An rc graduates
  to `v1.0.0` only when it has been out for **N days with no P0
  defect found** (crash, data loss, security, or any defect that
  breaks a documented core path) **and K external consumers have
  built against it** — where "external" means a person or project
  outside the FDK project's own development loop, with a filed
  report (issue, email, or the FDK-REQUESTS channel) attesting the
  build worked. Both conditions, not either. Suggested starting
  values: N = 14, K = 2 — the three withdrawals all shared one
  root cause (defects a downstream integrator found within days of
  the release), so a window shorter than the time it took
  downstream to find them proves nothing, and fewer than two
  consumers means the candidate was never really exercised outside
  the project. The owner should tune both numbers to the actual
  rate of downstream engagement.
- **What an rc is not.** An rc does not reset the withdrawal
  discipline: if a P0 lands on a published rc, the rc is marked
  broken in its notes and the next rc supersedes it; only the
  final tag gets the "must be withdrawn if defective" treatment
  the three 1.0.0 cuts received.

## History

- 2024: the tree briefly reported internal milestone labels in the
  public version macros. That was a bookkeeping mistake, not a
  promotion decision. The public version was reset to `0.0.1` and
  pinned, and this "0.0.1 on purpose" joke became the project's
  longest-running bit: *this absurdly capable toolkit is still
  called 0.0.1.*
- 2026-10-11 (morning): a distribution pass bumped the public
  version to **1.0.0**, tagged `v1.0.0`, and produced a draft
  GitHub Release with native packages across five distribution
  families.
- 2026-10-11 (same day): **retracted.** Manual testing immediately
  after the pass showed the file and folder pickers were not
  release-ready. The `v1.0.0` tag and the (never-published) draft
  release were deleted, and the public version returned to
  `0.0.1`. Nothing was rewritten — the infrastructure remained on
  `main` for the eventual, deliberate 1.0.0, a decision that
  carries the lesson that *component quality, not pipeline
  greenness, gates the number.*
- 2026-10-12: **the first real 1.0.0.** The retraction's named
  blockers were closed by the picker overhaul, the project owner
  validated the result on real hardware (native Wayland under
  labwc included) and explicitly demanded the release, and this
  document recorded the decision in the same commit that moved the
  macros. The versioned-SONAME scheme (`libfdk.so.1`) activated
  with it.
- 2026-10-12 (hours later): **the one-face alignment.** The owner
  set the product posture: FDK ships exactly ONE theme —
  `faded-dream` — and the next faces are `.FDKThemes` drop-ins the
  user installs, never package content. A scope decision is not a
  version decision; the release was re-cut, the number untouched.
- 2026-10-25: **1.0.0-rc1 — the extension API candidate.** The
  owner directed the extension API (`include/fdk/fdk_extend.h`)
  and set the number explicitly: 1.0.0-rc1, a MINOR bump because
  the change ADDS public API. ABI-append-only; SONAME stays
  `libfdk.so.1`.
- 2026-10-26: **rc1 → rc2.** rcN bumps authorized by the owner;
  rc2 marks the native D-Bus client. SONAME stays `libfdk.so.1`.
- 2026-10-31: **back to in development — the clean re-publish.**
  The owner returned FDK to in-development status: the former
  repository (with its tag, releases, and `release` branch) was
  retired, and the history was re-published fresh — one branch,
  `main`, zero tags, no releases, no published artifacts of any
  kind. The version macros describe the development tree, not a
  release.
- 2026-11-08: **1.0.0 — the first-release decision.** The owner
  set the first release to 1.0.0: the rcN numbering retired, the
  suffix dropped, MINOR 1 → 0. The SONAME stays `libfdk.so.1`.
- 2026-11-09: **first cut and first withdrawal.** Tagged,
  published, withdrawn by the owner the same day — back to
  development. The walkthrough evidence remains real
  (docs/REAL-DESKTOP-SIGNOFF.md).
- 2026-11-12: **re-cut as final.** Tag `v1.0.0`, the GitHub
  release published, the version string final `1.0.0`. (That
  release belonged to the former repository's lineage; see
  2026-10-31 and the 2026-11-18 re-release below for the posture
  history.)
- 2026-11-18: **the re-release.** The owner ordered the release
  cut again; `release.yml` built the bundle plus the five distro
  families' packages on the tag.
- 2026-11-19: **withdrawn again (commit `8ecd83f`).** The release
  shipped six GL defects found by Halcyon, the first downstream
  integrator (docs/FDK-REQUESTS.md sessions 7-8). All six are
  fixed on main, but a release cut with the defects stays
  withdrawn: the tag and release are deleted, the CHANGELOG gains
  the withdrawal record, and the project is a development version
  again until the owner says otherwise. In the same hygiene pass
  the version string gains the `-dev` suffix — the 2026-11-19
  suffix decision this document records.
