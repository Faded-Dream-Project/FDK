# FDK ABI Policy

## Current status: pre-ABI-stable (0.x)

`FDK_ABI_STABLE` (in `include/fdk/fdk_version.h`) is `0`. During the
0.x series, both the API and ABI may break between minor versions
without notice beyond the changelog. This is a deliberate tradeoff:
Phase 1–9 of the roadmap involve real architectural discovery, and
promising ABI stability before the widget/layout/theme systems have
been built against real usage would either be a lie or a straitjacket.

## What "ABI-safe" means for FDK, once we get there

Once `FDK_ABI_STABLE` becomes `1` (targeted at the end of Phase 10 —
Stabilization), the following rules apply and are enforced by review:

1. **All public objects are opaque.** Every type in `fdk_types.h`
   (`fdk_context`, `fdk_window`, `fdk_widget`, etc.) is a forward
   declaration only; applications never see or depend on struct
   layout. This is already true in Phase 1 and is not expected to
   change — it's the mechanism that makes the rest of this policy
   possible.
2. **No struct in a public header may change size or field order**
   once shipped in a stable release, *except* structs explicitly
   documented as "input structs" (like `fdk_init_options`) which:
   - are always passed by pointer, never by value, to API functions
   - are always safe to zero-initialize for defaults
   - may only ever have fields **appended**, never removed or
     reordered, and only with a documented default equivalent to
     zero/NULL
3. **No public function signature changes.** A behavior change that
   needs new parameters gets a new function name (e.g.
   `fdk_window_create_ex`), not a modified existing signature.
4. **Enums may gain new values** (appended, not renumbered) but
   existing values' numeric meaning never changes.
5. **Removing a public symbol is a major-version-bump event**, not a
   minor one.

## Versioning

`fdk_get_version()` / `fdk_get_version_string()` let an application
detect a mismatch between the headers it compiled against and the
`.so` it loaded at runtime — always check these if you're loading FDK
as a plugin/dlopen rather than linking it normally.

## Practical guidance for FDK's own implementers (pre-1.0)

Even though ABI isn't guaranteed yet, treat the public headers as if
it mattered, because:
- it forces the opaque-pointer discipline that makes stability
  *possible* later
- it avoids accumulating a pile of ABI debt as the surface grows

Internal-only structs (anything under `src/`, e.g.
`context_internal.h`) have no such constraint and may change freely —
that's the whole point of keeping them out of `include/`.
