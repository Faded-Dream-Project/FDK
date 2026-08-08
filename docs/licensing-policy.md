# FDK Licensing Policy

## FDK's own license

FDK itself is proprietary software — see `/LICENSE`. That license is a
draft and explicitly flagged for legal review before any public or
commercial release; do not treat it as final.

## What FDK may depend on

Third-party code (source incorporated directly, or libraries linked
at build/runtime) must be under a license compatible with a
proprietary product:

**Allowed:** MIT, BSD-2-Clause, BSD-3-Clause, ISC, public-domain-like
permissive licenses (Unlicense, CC0), or similarly permissive terms
that do not impose copyleft, source-disclosure, or "same license"
obligations on FDK or on applications built with it.

**Never allowed, under any circumstances:**
- GPL (any version)
- LGPL (any version) — including dynamic-linking-only LGPL code,
  since FDK's own license doesn't attempt to satisfy LGPL's relinking
  requirements
- AGPL
- MPL, EPL, CDDL, or any other copyleft or weak-copyleft license
- Code with no clear license at all ("license unknown" is treated as
  "not allowed" until resolved, never assumed permissive)

This applies to:
- Code copied or adapted into FDK's own source tree
- Libraries FDK links against, statically or dynamically
- Code an implementer might be tempted to "translate" from a
  copyleft project into C — a derivative work is still a derivative
  work regardless of language translation

## Red Hat / copyleft audit procedure

Before each major release (and any time a new dependency is proposed),
audit for:

1. **Text search** of the source tree and build files for: `GTK`,
   `GLib`, `GObject`, `GIO`, `Pango`, `Cairo`, `Qt`, `GPL`, `LGPL`,
   `AGPL`, and known copyleft license header boilerplate.
2. **Actual dependency graph inspection** — text search alone is
   insufficient, since a dependency's own dependencies might pull in
   something disallowed transitively. Use `ldd` on built binaries and
   inspect each linked library's license, and check build-system
   dependency declarations (e.g. `pkg-config --list-all` output
   referenced by the build).
3. **Do not assume by association.** A library isn't disqualified
   merely because a Red Hat-affiliated project happens to package or
   use it, or because it's commonly found alongside GNOME. Check its
   actual origin, license, and dependency chain before excluding or
   including it. (Example: fontconfig is MIT-licensed and not
   Red-Hat-authored software, even though it's ubiquitous in
   GNOME/freedesktop environments — see `docs/dependencies.md`.)

## Attribution

When a permissively-licensed third-party component is incorporated,
its license text and copyright notice are preserved in
`THIRD-PARTY-NOTICES.md` at the repository root (created when the
first such dependency is actually added — none exist as of Phase 1).

## When uncertain

If a dependency's license, provenance, or transitive dependency
licensing can't be confirmed, it is not incorporated until it can be.
This is a hard rule, not a judgment call to be made under deadline
pressure.
