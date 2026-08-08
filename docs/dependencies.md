# FDK Dependency Audit

This document tracks every external dependency FDK has, or is expected
to eventually need. Per project policy (see `docs/licensing-policy.md`),
every dependency must be justified here before it's introduced — "it
makes implementation easier" is not sufficient justification on its
own; the question is always "can this reasonably be implemented
internally in portable C without making FDK unnecessarily enormous or
fragile?"

## Current dependencies (Phase 1 — Foundation)

**None.** The Phase 1 foundation (`src/core/`) depends only on:

| Dependency | Purpose | License | Build/Runtime | Optional | Replaceable |
|---|---|---|---|---|---|
| C standard library (libc) | malloc/free, string ops, stdio, time | N/A (platform) | Runtime | No | No — foundational |
| POSIX (`_POSIX_C_SOURCE=200809L`) | `localtime_r` | N/A (platform) | Runtime | No | Could fall back to `localtime` + mutex if strict non-POSIX portability were ever required; not currently a goal (Artix Linux is POSIX) |

## Anticipated dependencies (future phases)

These are **not yet added**. Listed here so the audit trail exists
before they land, per the project's "document before you add" policy.

| Dependency | Anticipated phase | Purpose | License (to verify at add-time) | Runtime/Build | Optional | Notes |
|---|---|---|---|---|---|---|
| libxcb (or Xlib) | Phase 2 — Platform | X11 protocol I/O | MIT | Runtime | Yes (X11 backend only) | Explicitly permitted platform interface per project requirements |
| libwayland-client | Phase 2 — Platform | Wayland protocol I/O | MIT | Runtime | Yes (Wayland backend only) | Explicitly permitted platform interface |
| xkbcommon | Phase 2 — Platform | Keyboard layout/keymap handling | MIT | Runtime | No (both backends need correct keysym handling) | Small, permissive, avoids reimplementing keymap parsing |
| A minimal font rasterizer (candidate: stb_truetype, single-file, public domain) | Phase 3 — Rendering / text | Glyph rasterization from TrueType/OpenType fonts | Public domain / MIT (verify exact header at add-time) | Runtime | No | Text rendering without this would mean writing a TTF parser + rasterizer from scratch, which is out of scope for FDK's stated goals; single-header permissive libraries are the preferred pattern per project policy |
| fontconfig | Under consideration, Phase 3 | System font discovery | MIT | Runtime | Yes — a bundled fallback font can substitute | fontconfig itself is MIT and not Red-Hat-origin software, despite being commonly packaged alongside GNOME; will be re-evaluated against "small, independent library" preference before committing |

**Explicitly rejected:** Pango, Cairo, GLib, GObject, GIO — see
`docs/licensing-policy.md` §"Red Hat / copyleft audit" for the
reasoning (not strictly all copyleft, but all are large desktop-stack
dependencies FDK's minimalism goal excludes; Cairo and Pango
specifically are also usually paired with GLib, which pulls in a much
larger dependency surface than FDK wants).

## Policy reminders

- Every dependency added must update this table in the same change
  that introduces it.
- Runtime-optional dependencies (X11-only, Wayland-only) must not leak
  into the public API — an application linking FDK should not need to
  know or care which backend is active unless it explicitly asks.
- Before each major release, re-run the Red Hat / copyleft audit
  described in `docs/licensing-policy.md` against both the source tree
  and the actual linked dependency graph (`ldd`/`readelf`), not just a
  text search.
