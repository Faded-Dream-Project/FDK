# Decision: FriBidi (LGPL) vs the "never LGPL" rule

**Status: OPTIONS AND RECOMMENDATION ONLY — no policy change is
made by this document. The decision is the owner's.**

*Written 2026-11-20, after the licensing-conflict review.*

## The conflict, stated precisely

`docs/licensing-policy.md` says LGPL is **never allowed, under any
circumstances** — "including dynamic-linking-only LGPL code, since
FDK's own license doesn't attempt to satisfy LGPL's relinking
requirements" — and that the rule covers libraries FDK links
against "statically or dynamically."

`src/text/shaping.c` dlopens **FriBidi** at runtime (with
HarfBuzz) when `$FDK_SHAPING` is enabled. FriBidi's license was
verified from upstream on 2026-11-20:

- Repository: <https://github.com/fribidi/fribidi>
- `COPYING` (fetched from the repo, first lines verbatim):
  *"GNU LESSER GENERAL PUBLIC LICENSE — Version 2.1, February
  1999"*
- Every source file header (e.g. `lib/fribidi.c`) grants:
  *"either version 2.1 of the License, or (at your option) any
  later version"* — i.e. **LGPL-2.1-or-later**.
- The project also offers a commercial-licensing contact
  (`fribidi.license@gmail.com`), per its README.

So the project's stated rule and its actual runtime behavior
disagree. Two aggravating details, both found during this review:

1. **The docs have the wrong SONAME.** `docs/dependencies.md`
   lists `libfribidi.so.1`; the code dlopens `libfribidi.so.0`
   (the SONAME FriBidi has carried across 1.0.x — the .so.1
   spelling would never load). If the owner keeps FriBidi in any
   form, that row needs fixing.
2. **HarfBuzz is NOT in conflict.** Its COPYING is the so-called
   "Old MIT" license (verified from
   <https://github.com/harfbuzz/harfbuzz>'s COPYING) —
   permissive, allow-listed. Only the FriBidi half of the shaping
   pair touches LGPL.

## What LGPL 2.1 actually demands (for weighing the options)

LGPL 2.1 §6 obligations attach to a "work that uses the Library"
**that is distributed combined with it** — and the escape hatch
most relevant here is §6(b): a "suitable shared-library mechanism"
that uses *at run time a copy of the library already present on
the user's computer system* and works with a modified,
interface-compatible version. FDK's model — dlopen a system
FriBidi, never bundle it, fall back when absent — is close to
exactly what §6(b) describes. The dominant conservative reading
among proprietary vendors is that dlopen-only, never-bundled use
does not trigger §6 at all because the combined work is never
distributed; FSF's own FAQ historically treats dlopen as "probably
still combined" in borderline cases. **This document does not
resolve that argument — it is exactly the ambiguity the policy's
"never LGPL, including dynamic-linking-only" rule was written to
avoid.**

## The options

### (a) Amend the policy: a narrowly worded runtime-optional carve-out

Rewrite the "never allowed" clause to add one exception:

> LGPL-2.1-or-later libraries are permitted **only** as
> runtime-optional, `dlopen`-only, never-bundled system
> dependencies — never in the link line, never vendored, never in
> a shipped package's dependencies — and only when (1) the
> library's absence is a fully functional documented fallback,
> (2) `THIRD-PARTY-NOTICES.md` discloses the dependency and its
> license, and (3) the user can replace the library with a
> modified interface-compatible build (the LGPL §6(b) posture).

- **What it costs:** a policy paragraph; a notice entry; fixing
  the `libfribidi.so.0` row; keeping the fallback path honest
  (it already is — `FDK_SHAPING=0` and the legacy walk exist).
  Maybe 1–2 hours of work, plus whatever review the owner wants.
- **What it risks:** the "never LGPL" rule becomes "never LGPL,
  except…" — and exceptions grow. The licensing-policy already
  says the license "was finalized without review by legal counsel
  and is not legal advice"; a carve-out leans harder on that
  caveat. If FDK ever wants its own runtime distribution channel
  (Flatpak, AppImage), the carve-out must be re-examined: those
  bundle their runtime, and bundling an LGPL lib into FDK's
  distribution artifact is squarely outside the carve-out.
- **What it buys:** keeps full complex-script shaping + bidi with
  zero in-tree code, keeps the "works everywhere, better where the
  libs exist" behavior, and matches what the code already does.

### (b) Implement UAX #9 (bidi) in-tree under a permissive license, drop FriBidi

FDK would need: the bidi character-class tables (derived from
Unicode data), the bidirectional algorithm's explicit-level,
implicit-resolution, and reordering passes, and bracket-pair
mirroring. HarfBuzz (MIT) would stay for shaping.

- **What it costs:** UAX #9 is the single hardest "small" Unicode
  algorithm — the reference implementation is ~4k lines; a careful
  from-scratch C implementation with tables is realistically
  **2,500–4,000 lines and several weeks of work** to reach the
  conformance level FriBidi has (its BidiTest.txt suite is
  ~90k cases; FDK would want to run it too, which is itself a
  harness). The tables are Unicode-data-derived (permissive,
  license-checked — same class as the UAX #14 tables this
  project already vendored for line breaking), but the
  **algorithm implementation must be written from the spec, not
  from FriBidi's source** — a translation would be a derivative
  work of LGPL code and would defeat the point.
- **What it risks:** bidi bugs are silent and user-visible
  (mis-ordered Arabic, wrong caret motion in Hebrew). FriBidi is
  25 years old and battle-tested; a fresh implementation reaches
  parity slowly. Until then, mixed-direction text regresses for
  every user who has the system library.
- **What it buys:** the "never LGPL" rule stays absolute; the
  runtime-optional mechanism (already built for HarfBuzz) gains a
  permissive in-tree twin; no external legal ambiguity at all.

### (c) Ask FriBidi upstream for a commercial/proprietary license

The README offers a contact for it.

- **What it costs:** negotiation, unknown fee, unknown timeline.
- **What it risks:** silence, or terms that don't fit a
  hobbyist-origin project's budget.
- **What it buys:** the cleanest legal position if (a)'s
  ambiguity is unacceptable and (b)'s cost is too high. Worth one
  email if the owner leans this way.

### (d) Status quo, but document the contradiction

Fix the SONAME typo, add the LGPL disclosure to
THIRD-PARTY-NOTICES.md, and leave the policy text and the code as
they are.

- **What it costs:** 30 minutes.
- **What it risks:** the most dangerous option long-term — an
  audit-grade reader (a distro, a future contributor, an
  acquirer) finds the policy saying "never" while the code says
  "sometimes," and trust in every other policy claim drops with
  it. Listed for completeness; **not recommended.**

## Recommendation

**(a) — the narrow carve-out — with (c) as a background probe.**

Reasoning: the code already implements the exact posture the
carve-out would license (dlopen-only, optional, documented
fallback, never bundled), the shaping quality gap it avoids is the
largest user-facing regression in the whole decision space, and
(b) is weeks of high-risk work better spent after a release is
actually standing. The carve-out should be written so that **the
first bundled distribution artifact invalidates it** — that keeps
the exception from growing silently. Send the (c) email in
parallel; it costs nothing and its answer would obsolete the
ambiguity.

**The owner decides. Nothing in this document changes
`docs/licensing-policy.md`, `docs/dependencies.md`, the code, or
the notice files.**

## Verification trail (for the owner's own audit)

- FriBidi COPYING fetched from the GitHub API 2026-11-20:
  LGPL-2.1 full text; source headers grant "2.1 … or (at your
  option) any later version."
- HarfBuzz COPYING fetched the same way: "Old MIT".
- LGPL 2.1 §6 options (a)–(e) and §4 reviewed from the published
  license text; the dlopen-only reading summarized above is a
  reading of the license language, not legal advice.
