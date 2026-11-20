# Contributing to FDK

Thank you for your interest in FDK, the Faded Dream ToolKit.

## The honest state of contribution today

FDK is **proprietary software** under the FDK Proprietary License
(`LICENSE`). The project is not an open-contribution project at
this time: outside pull requests are not merged by default, and
contributing code requires an arrangement with the Licensor that
does not exist yet (see the CLA/DCO TODO below — that decision
belongs to the project owner).

What is genuinely welcome today:

- **Bug reports** — via the GitHub issue tracker, using the bug
  report template. The best reports include the FDK version string
  (`fdk_get_version_string()`; note a `-dev` suffix means an
  unreleased development build — see `docs/versioning.md`), the
  backend (X11 or Wayland, and the compositor/WM name), the
  distro, and a minimal reproducer.
- **Security reports** — follow `docs/security.md`'s threat-model
  rules. If you found something that crashes the toolkit, first
  check whether the input violates that document's contract; if
  not, it is a real bug and the bounds are the policy's.
- **Feedback and real-world reports** — what you built with FDK,
  what was missing, what felt wrong. The project's history shows
  downstream integrators' reports (see `docs/FDK-REQUESTS.md`)
  directly shaping the roadmap.
- **Feature requests** — via the issue tracker, using the feature
  request template. Requests that name a concrete user-facing
  outcome ("a date picker widget" rather than "more widgets")
  are far easier to act on.

## What a good bug report contains

1. The exact version string from `fdk_get_version_string()` (or
   the package version).
2. Backend and environment: X11 or Wayland, WM/compositor, distro,
   architecture.
3. A minimal reproducer — a single `.c` file that builds against
   the installed FDK (`gcc repro.c $(pkg-config --cflags --libs
   fdk)`) is ideal.
4. What happened, what you expected, and any log output (FDK logs
   to stderr; `FDK_LOG=debug` raises verbosity).

## Building from source to verify a bug

```
make -j"$(nproc)"        # library + tests
make test                # the full suite (no display needed)
make test-x11            # the X11 integration suite (Xvfb works)
```

See `docs/build.md` for dependencies and `docs/testing.md` for
what the suites cover. If a bug reproduces on your machine but not
in the suite, that gap in the suite is itself worth reporting.

## Code contributions (the current policy)

Until the project owner opens code contribution with the
accompanying legal framework, pull requests from outside the
project will be read but cannot be merged. Small fixes may be
incorporated manually with attribution in the commit message and
`THIRD-PARTY-NOTICES.md` where appropriate — but no contribution
of code can be accepted on the expectation of inclusion.

<!-- TODO(owner): CLA/DCO DECISION — the owner must decide whether
     outside code contribution will ever be open, and if so by
     which instrument: a Contributor License Agreement (assignment
     or license grant) or a DCO sign-off regime, both of which are
     prerequisites for merging outside work into a proprietary
     tree. Until that decision is recorded here, this section's
     "read but cannot merge" posture is the policy. -->

## Conduct

All participation in the project's tracker and communications
follows `CODE_OF_CONDUCT.md`.
