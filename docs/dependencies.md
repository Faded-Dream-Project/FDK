# FDK Dependency Audit

This document tracks every external dependency FDK actually has, plus
ones anticipated for future phases. Per project policy (see
`docs/licensing-policy.md`), every dependency must be justified here
before it's introduced — "it makes implementation easier" is not
sufficient justification on its own; the question is always "can this
reasonably be implemented internally in portable C without making FDK
unnecessarily enormous or fragile?"

## Current dependencies (Phase 1 + Phase 2)

| Dependency | Purpose | License | Build/Runtime | Optional | Replaceable |
|---|---|---|---|---|---|
| C standard library (libc) | malloc/free, string ops, stdio, time | N/A (platform) | Runtime | No | No — foundational |
| POSIX (`_POSIX_C_SOURCE=200809L`) | `localtime_r`, `poll()`, `nanosleep()` | N/A (platform) | Runtime | No | FDK targets modern Linux where POSIX is universally available; avoiding POSIX would mean reimplementing `localtime_r`/`poll()` for no real portability gain |
| Xlib (`libX11`) | X11 protocol client library — the X11 backend (`src/platform/x11/`) | MIT-style ("X11 License") | Runtime, build (headers) | Yes — only linked/used when the X11 backend is active at runtime; the library is always compiled in (see docs/architecture.md) but an application only pays the connection cost if `fdk_init()` actually selects X11 | Could theoretically be replaced with raw `libxcb` for a smaller footprint; not pursued for Phase 2 since Xlib's keysym/XKB conveniences (`XLookupString`, `XkbSetDetectableAutoRepeat`) meaningfully simplified correct keyboard handling — revisit only if profiling or dependency-size pressure justifies it |
| `libwayland-client` | Wayland protocol client library — the Wayland backend (`src/platform/wayland/`) | MIT | Runtime, build (headers) — **only when the Wayland backend is actually built**, see below | Yes, at both build time and runtime. Build-optional: if `pkg-config --exists wayland-client xkbcommon` is false, FDK's Makefile silently skips the Wayland backend entirely and compiles `src/platform/wayland_disabled.c` in its place, which returns `NULL` from `fdk_platform_wayland_ops()` so the runtime auto-detection in `src/core/context.c` cleanly falls through to X11 (or fails with `FDK_ERR_NO_DISPLAY` if `FDK_PLATFORM_WAYLAND` was explicitly requested). Use `make FDK_DISABLE_WAYLAND=1` to force this even when dev headers ARE present, or `make FDK_ENABLE_WAYLAND=1` to turn a missing-headers situation into a hard build error rather than a silent X11-only fallback. See `docs/build.md` "Optional Wayland build". | No — this is the canonical, only reasonable way to speak the Wayland protocol from C |
| `wayland-protocols` (specifically `stable/xdg-shell/xdg-shell.xml`) | Protocol description used to generate `src/platform/wayland/generated/xdg-shell-client-protocol.h` / `xdg-shell-protocol.c` via `wayland-scanner` at development time; the XML itself is vendored into `third_party/wayland-protocols/` and the *generated* C files are committed/built as part of FDK, not fetched at build time | MIT-style — see the `<copyright>` block in the XML itself, preserved in `third_party/wayland-protocols/xdg-shell.xml` | Build-time only (code generation); no runtime dependency on `wayland-protocols` itself, only on the generated C output and `libwayland-client` | No — xdg-shell is the standard, required protocol for any Wayland toplevel window | N/A — protocol description, not a library |
| `wayland-scanner` (part of `libwayland-dev`'s tooling) | Generates the xdg-shell C bindings from the XML above | MIT | Build-time only | N/A | N/A |
| `libxkbcommon` | XKB keymap compilation and keysym resolution for the Wayland backend's keyboard handling (`src/platform/wayland/wayland_seat.c`) — Wayland delivers a raw keymap over a fd and expects the client to compile it itself, unlike X11 where the server does layout resolution | MIT | Runtime, build (headers) | Yes, only used when Wayland backend + keyboard capability are both active | No — the standard, effectively only correct way to do XKB keymap handling on Linux; reimplementing an XKB compiler internally would be a large, security-sensitive undertaking directly contrary to the project's minimalism goal |

**Note on X11 provenance:** Xlib's copyright headers include a Red
Hat, Inc. copyright line among several others (X Consortium, MIT,
etc.) — per the project's licensing policy ("don't blindly classify a
library as 'Red Hat software' merely because it's packaged by a
distribution or happens to be used by Red Hat" / "investigate the
actual project/origin/license situation"), this was checked directly:
the license is the permissive MIT-style "X11 License", the code
predates and is independent of GTK/GNOME/the modern Red Hat desktop
stack, and it is one of the two explicitly project-permitted platform
interfaces. Not excluded.

## Anticipated dependencies (future phases, not yet added)

| Dependency | Anticipated phase | Purpose | License (to verify at add-time) | Notes |
|---|---|---|---|---|
| fontconfig | Under consideration (with the Phase 6 widget catalog) | System font discovery | MIT | Not Red-Hat-origin software despite common GNOME association — same investigate-before-excluding principle as Xlib above; a bundled fallback font could substitute if this is skipped |

**Added in Phase 6 (first slice) — vendored source, not a system
library:**

| Dependency | Purpose | License | Build/Runtime | Optional | Replaceable |
|---|---|---|---|---|---|
| [stb_truetype](https://github.com/nothings/stb) v1.26 (vendored at `third_party/stb/`) | TrueType/OpenType parsing + anti-aliased glyph rasterization for the text layer (`src/text/`) | Dual MIT / public domain — both allow-listed; see `THIRD-PARTY-NOTICES.md` and `third_party/stb/README.md` (provenance + SHA-256 + update procedure) | Compiled in (one TU, `src/text/stb_truetype_impl.c`, with the aggressive warning set scoped off for that file and allocations routed through `fdk_alloc`); no pkg-config, no system library, no ABI surface | No | Yes in principle (harfbuzz+FreeType would be the heavyweight route) — but a from-scratch parser/rasterizer is explicitly out of scope per the original anticipation note, and stb is the established minimal choice (raylib et al.) |

**Explicitly rejected:** Pango, Cairo, GLib, GObject, GIO — large
desktop-stack dependencies FDK's minimalism goal excludes (Cairo/Pango
are typically paired with GLib, which pulls in a much larger
dependency surface than FDK wants). See `docs/licensing-policy.md`.

## Policy reminders

- Every dependency added must update this table in the same change
  that introduces it. This update was made alongside the Phase 2
  platform layer landing, not after the fact.
- Runtime-optional dependencies (X11-only, Wayland-only) must not leak
  into the public API — confirmed: no Xlib or wayland-client type
  appears in any `include/fdk/*.h` header, only in
  `src/platform/{x11,wayland}/*.h` (internal-only, see
  `docs/architecture.md`'s "no backend leakage" rule).
- Before each major release, re-run the Red Hat / copyleft audit
  described in `docs/licensing-policy.md` against both the source tree
  and the actual linked dependency graph (`ldd`/`readelf`), not just a
  text search.
