# RELEASING — how the `release` branch moves

FDK ships from two places, and they have different jobs:

| ref | what it is | who moves it |
|-----|------------|--------------|
| `main` | **UNSTABLE by design** — all development lands here, one logical commit at a time, CI-gated per push | anyone with push rights, continuously |
| `release` | the branch consumers should build from or track — always exactly a commit that was a **tagged, CI-green 1.x.0 release on `main`** | the project owner, by explicit approval, **fast-forward only** |
| `vX.Y.Z` tags + GitHub Releases | the immutable, citable releases (source + the five package families) | the owner, at release time |

## The rule

`release` moves **only** by fast-forward from a tag:

1. A version is cut on `main` per `docs/versioning.md` (the recorded
   owner decision), the full CI matrix is green **at the job level**
   on the exact commit, and the owner signs off.
2. The owner tags the commit (`vX.Y.Z`) and publishes the GitHub
   Release (the `release` workflow builds and attaches the packages
   for the five families + source).
3. The owner fast-forwards `release` to that commit:

   ```sh
   git fetch origin
   git push origin vX.Y.Z^{commit}:refs/heads/release   # fast-forward only
   ```

   A push that is not a fast-forward of the current `release` tip is
   a mistake: **never force-push `release`, never rebase it, never
   rewrite its history.** If a release turns out to be bad, the fix
   is a NEW release (a new tag + a new fast-forward), not a rewrite —
   the same discipline that governs the 2026-10-11 retraction of the
   premature 1.0.0 (`docs/versioning.md`).

4. `release` is protected: no force-push, no deletion, and the CI
   workflow runs on it too, so a consumer who pins the branch always
   sees green on exactly what they built.

## For consumers

- **Building FDK yourself:** track `release` (or a tag).
- **Binary packages:** the GitHub Releases page carries the `.deb`,
  `.rpm`, `.xbps`, `.apk`, Arch packages and the source archive for
  each release — see `docs/packaging.md`.
- **Packaging FDK for a distribution:** package from a tag, never
  from `main` (`docs/packaging.md`'s pinning note).

`main` is where the next version is being built. Its HEAD can carry
release candidates (an `-rcN` suffix — again `docs/versioning.md`),
half-landed subsystems between commits, and docs that describe the
destination rather than the moment. It is green on every push — that
is the contract CI enforces — but "green on main" means "not broken",
not "released".
