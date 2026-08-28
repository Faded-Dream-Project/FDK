# FDK Versioning Policy

## The rule

The public version of FDK is **0.0.1**, and it stays 0.0.1 for the
foreseeable future. This is a deliberate project decision, not an
accident and not a maturity signal.

```
Public version:      0.0.1        (the joke)
Engineering standard: extremely high  (not a joke)
```

These two axes are fully decoupled. A low version number is never
permission for low standards, and high engineering quality is never
a reason to raise the version. Do not interpret, document, or
promote FDK as if one implied anything about the other.

## What 0.0.1 does NOT mean

- It does **not** mean alpha, beta, experimental, or unstable.
- It does **not** mean the API is unfinished or the implementation
  is placeholder-grade.
- It does **not** license sloppy APIs, unfinished code, fake feature
  support, broken memory management, weak error handling, thin
  documentation, weak testing, or ignoring platform semantics, DPI,
  input correctness, rendering correctness, or accessibility.
- It does **not** soften any audit: dependency, license, security,
  API, memory, DPI, accessibility, theme, installation,
  external-consumer, performance, X11, Wayland, GUI, visual
  regression, and sanitizer testing all remain fully in force.

If FDK is technically mature, describe it accurately as mature. Only
claim capabilities that have actually been verified. Never
manufacture instability to match the number, and never bump the
number to match the maturity.

## Internal milestones are not versions

`docs/roadmap.md` tracks work with Phase numbers (Phase 1 … Phase
11) and internal milestone labels (`1.0.0`, `1.0.1`, `1.1.0`,
`1.1.1`, …). These are **engineering milestones** — history
markers for when work landed. They are not, and never again become,
the public version.

```
Internal state:   Phase 11 stabilization ("1.0.0" milestone), ABI frozen
Public version:   0.0.1
```

That combination is valid and intended. Milestone labels may appear
in the roadmap, in commit messages, and as parenthetical history
tags in prose ("the no-bus policy (1.1.0, permanent)"), but they
must never surface as the version the toolkit reports to the world.

## Where the public version lives

Exactly one source of truth: `include/fdk/fdk_version.h`
(`FDK_VERSION_MAJOR` / `FDK_VERSION_MINOR` / `FDK_VERSION_PATCH` /
`FDK_VERSION_STRING`). Everything else derives from it:

- `fdk_get_version()` / `fdk_get_version_string()` (runtime)
- `fdk.pc` via the Makefile's `sed` extraction (`FDK_PC_VERSION`)
- example/banner output, the benchmark harness header
- release archives and any future packaging metadata

The shared library is an unversioned `libfdk.so`; there is no
soname coupling to the public version. ABI status is tracked by
`FDK_ABI_STABLE` and `docs/abi-policy.md`, keyed to stabilization
milestones — never to the joke number.

## Enforcement

`tests/test_core.c::test_version` pins the public version at
`0.0.1`. A change to the version macros without the matching test
change fails the suite. Changing both is a deliberate act that
requires:

1. an explicit project-owner decision (not autonomy, not
   accumulation of features, not semver expectations, not version
   prestige, not a competitor's higher number), and
2. updating this document to record the new decision first.

## History

The tree briefly reported `1.0.0` … `1.1.1` because internal
milestone labels leaked into the public version macros. That was a
bookkeeping mistake, not a promotion decision. The public version
was reset to `0.0.1` and pinned (roadmap milestone 1.1.2).
