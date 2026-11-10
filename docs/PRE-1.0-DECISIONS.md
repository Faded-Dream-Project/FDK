# PRE-1.0-DECISIONS.md — the owner's decision list (2026-11-11)

Companion to docs/PRE-1.0-AUDIT.md: for each layout exposure and
frozen semantic — the risk, the cheapest reduction while NO stability
promise exists yet, and the recommendation. None of these are applied;
they are decisions to make BEFORE signing 1.0.

## The layout exposures

### D1. `fdk_widget`'s 1024-byte embed budget
- **Risk**: after 1.0 the ceiling is ABI. Widgets today sit well under
  it, but one widget family (the model-bound tree?) has the least
  headroom; a post-1.0 feature that needs more forces a source break
  for every embedder.
- **Cheapest reduction now**: print `sizeof(fdk_widget)` per widget
  family (a 20-line `make sizes` target), pick the new ceiling from
  the data (2048 if the largest is >512), and pad. Zero code risk
  today; impossible to renegotiate after 1.0.
- **Recommendation**: DO IT before 1.0 — one make target + a padding
  bump. My recommendation: keep 1024 only if the largest family is
  under 640.

### D2. `fdk_event_data`'s tagged union (caller-visible by value)
- **Risk**: a new event payload that grows the union breaks every
  compiled switch-on-events binary silently (stack-slot mismatch).
- **Cheapest reduction now**: add a compile-time assert
  (`sizeof(fdk_event_data) <= 128` or the current max) to abi_check.c
  and pin the number in the changelog. New payloads then must fit or
  ride a pointer — a rule, not a hope.
- **Recommendation**: DO IT — one static assert + one doc line.

### D3. The input-struct contracts (struct_size pattern)
- **Risk**: low — the mechanism is append-only by design; the only
  break is reordering a field.
- **Cheapest reduction now**: a header-lint (the X11 declaration gate
  pattern) asserting the struct_size fields stay FIRST.
- **Recommendation**: fold into the gate (D5) — no separate decision.

### D4. The primitive layouts (fdk_color/rect/size/span)
- **Risk**: none realistically; they mirror the platform types.
- **Recommendation**: NO ACTION; document as frozen.

## The frozen semantics

### D5. The X11 declaration gate (currently a runtime grep)
- **Risk**: the gate is a TEST (fails the suite) — a distro build that
  skips tests ships the violation silently.
- **Cheapest reduction now**: promote it to a BUILD gate: a generated
  translation unit that #errors on a banned token in a public header,
  wired into `make all` (not just `make test`). The proof test feeds
  a synthetic bad header to the same checker and asserts the failure.
- **Recommendation**: DO IT before 1.0 (the owner asked for exactly
  this shape in the session brief; it landed as a test, promote it).

### D6. The export map's removal policy
- **Risk**: `make test-exports` gates ADDITIONS; nothing stops a
  removal once 1.0 promises the symbol.
- **Cheapest reduction now**: the same gate gains a "removed symbol"
  check against the map in git HEAD~ — cheap, and it makes the
  deprecation policy enforceable.
- **Recommendation**: DO IT at 1.0-rc1 (needs a baseline tag first).

### D7. The dmabuf fourcc table's meaning
- **Risk**: removing a format after 1.0 is a silent break for add-ons
  that imported it.
- **Recommendation**: NO ACTION beyond the audit note — additions are
  compatible, removals are the deprecation cycle's business.

### D8. The INVALID-modifier zero-copy semantics (v3, this cycle)
- **Risk**: the semantics are implemented and unit-verified through
  the memfd path, but no DRM-equipped box has soaked the real
  import (the GBM round-trip and the modifier query SKIP).
- **Cheapest reduction now**: the drm-soak CI job exists
  (continue-on-error); one run on a real node closes it.
- **Recommendation**: GATE 1.0 ON IT — one green drm-soak run before
  the release commit, or record the SKIP in the release notes.

### D9. The multi-output lock adoption bug (found this cycle)
- **Risk**: a real corruption (pwindow->surface becomes 0xe5) on the
  second managed lock surface — the adopt path's hand-mirrored field
  inits. The one-screen shape is verified; the two-screen shape dies.
- **Cheapest reduction now**: replace the adopt path's mirrored init
  with a SHARED init helper (window_create calls it too) — the
  divergence can never reopen.
- **Recommendation**: FIX before 1.0 (it is clearly a bug; the shared
  helper is a half-day change that removes a whole bug class).

## The honest rest

- **D10. The virtual keyboard's default keymap** (this cycle): the
  protocol kills a keymap-less VK on its first key; the create now
  installs one. The emission+delivery verification still needs one
  clean rig run. Recommendation: verify before 1.0, else document the
  delivery shape as untested in the release notes.
- **D11. The docs' example set**: the audit's §5 gap list (a
  virtual-keyboard example, the count-only query shape, the X11
  popup path at the ext level). Recommendation: one example per gap
  before 1.0 OR move the gaps into the 1.1 notes explicitly.
