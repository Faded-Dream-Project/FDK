# Pull request

FDK is a proprietary project (see `LICENSE` and
`CONTRIBUTING.md`). Outside pull requests are read but cannot be
merged until the project owner opens code contribution with its
legal framework — the CLA/DCO decision is the owner's and is
tracked as a TODO in `CONTRIBUTING.md`. If you are submitting
anyway (small fix, proof of concept for an issue), this template
is what reviewers will check against.

## What and why

<!-- One or two sentences: what this changes, and the problem or
     issue it addresses. Link the issue if one exists. -->

## Checklist

- [ ] The change is described accurately, with no claims beyond
      what is implemented.
- [ ] `make test` passes.
- [ ] `make test-x11` passes (for anything touching windows,
      input, or rendering).
- [ ] New behavior is pinned by a test — the suite is regression
      armor; a change without a pin will silently rot.
- [ ] Public API changes are append-only per `docs/abi-policy.md`
      (new symbols, opaque types, no layout changes) and the
      SONAME stays `libfdk.so.1`.
- [ ] No new dependency of any kind without reading
      `docs/licensing-policy.md` first (license allow-list; the
      runtime-optional dlopen posture if applicable).
- [ ] No version macro changes (`include/fdk/fdk_version.h`) —
      version moves are the owner's decision per
      `docs/versioning.md`.
- [ ] No tag, release, or publish steps are included.

## Screenshots / evidence

<!-- For visual or behavioral changes: before/after screenshots,
     test output excerpts, or the reproducer from the issue now
     passing. -->
