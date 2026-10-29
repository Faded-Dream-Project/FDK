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
| `libXext` | The MIT-SHM extension (`XShmPutImage`, the presentation fast path) | MIT-style | Runtime, build (headers) | No — part of the base X11 distribution wherever libX11 is | Yes in principle (plain PutImage); kept for the fast path |
| `libXrandr` | The multi-monitor output model (M-6, `src/platform/x11/x11_randr.c`): per-CRTC geometry probed at connect, RRNotify-masked, refreshed in the dispatch loop; fdk-shell's own output-event channel rides its own X connection with the same library | MIT-style | Runtime, build (headers) — a DIRECT link dependency (`-lXrandr` on the link line, declared in the packaging metadata of all five families) | Runtime-optional at the SERVER level: no RandR extension = one output (the whole screen), the documented fallback — but the library itself is always linked when the X11 backend is built | Yes in principle (raw RandR wire requests over Xlib); not pursued — XRRGetScreenResourcesCurrent's cached form and the free-list discipline are exactly what a hand-rolled path would reimplement worse |

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
| (none currently) | | | | fontconfig was the long-standing entry here; resolved in 1.0.1 — see the runtime table below |

**Added in 1.0.1 — runtime-only, never linked, never a build
requirement:**

| Dependency | Purpose | License | Build/Runtime | Optional | Replaceable |
|---|---|---|---|---|---|
| fontconfig (system `libfontconfig.so.1`) | System font discovery for `fdk_font_load_system_default()` — the user's own font policy (per-user fonts, distro aliases, `sans-serif` resolution) | MIT-style | RUN TIME ONLY, via `dlopen` in `src/text/fontscan.c` (resolves 11 symbols, treats any miss as "absent"); no pkg-config, no header, no link flag — a build without fontconfig anywhere is unchanged, and a stripped container falls through to the directory scanner | Yes — absent fontconfig, discovery degrades to the known-path list plus a ranked scan of the standard font roots | Yes — the dlopen shim is one file; the resolver stages before/after it are independent |

The earlier "under consideration" note feared a build-time
license/dependency knot; runtime dlopen dissolves it (nothing is
linked, vendored, or distributed). FDK never calls `FcFini` — the
host application may itself be using fontconfig, and tearing down
shared global state from inside a toolkit is exactly the kind of
side effect FDK forbids itself (GTK and QT keep it initialized for
the process lifetime too). stb_truetype stays the rasterizer; the
bundled-fallback-font substitute discussed in that note remains
unnecessary.

**Added in Phase 6 (first slice) — vendored source, not a system
library:**

| Dependency | Purpose | License | Build/Runtime | Optional | Replaceable |
|---|---|---|---|---|---|
| [stb_truetype](https://github.com/nothings/stb) v1.26 (vendored at `third_party/stb/`) | TrueType/OpenType parsing + anti-aliased glyph rasterization for the text layer (`src/text/`) | Dual MIT / public domain — both allow-listed; see `THIRD-PARTY-NOTICES.md` and `third_party/stb/README.md` (provenance + SHA-256 + update procedure) | Compiled in (one TU, `src/text/stb_truetype_impl.c`, with the aggressive warning set scoped off for that file and allocations routed through `fdk_alloc`); no pkg-config, no system library, no ABI surface | No | Yes in principle (harfbuzz+FreeType would be the heavyweight route) — but a from-scratch parser/rasterizer is explicitly out of scope per the original anticipation note, and stb is the established minimal choice (raylib et al.) |
| [nanosvg](https://github.com/memononen/nanosvg) (vendored at `third_party/nanosvg/`) | SVG parsing + anti-aliased rasterization for icon-theme assets in the file dialogs (1.5.2): PNG is preferred when a directory carries both, SVG rasterizes at the requested size | zlib license — allow-listed; see `THIRD-PARTY-NOTICES.md` and `third_party/nanosvg/README.md` (provenance + SHA-256 + update procedure + the honest subset limits) | Compiled in (one TU, `src/theme/nanosvg_impl.c`, same warning-scoping pattern as the stb TUs); no pkg-config, no system library, no ABI surface | No | Yes in principle (librsvg is the heavyweight route — and rejected with Cairo/GLib below, since it would drag that entire stack into every FDK application's link line); the icon-shaped SVG subset is exactly what icon themes ship |

**Explicitly rejected:** Pango, Cairo, GLib, GObject, GIO — large
desktop-stack dependencies FDK's minimalism goal excludes (Cairo/Pango
are typically paired with GLib, which pulls in a much larger
dependency surface than FDK wants). See `docs/licensing-policy.md`.

## The no-libdbus policy (1.1.0, permanent — reconciled 2026-10-30)

**This section's 1.0-era text said FDK "never links, dlopens, spawns,
or requires D-Bus — or any daemon, bus, or activation
infrastructure" — and M-7's native client then changed the second
half of that sentence without this file following (the doc policy
below demands same-change updates; that slipped, and this is the
catch-up). The posture that SURVIVED, and is permanent:**

**FDK never links, dlopens, spawns, or requires libdbus, sd-bus,
GLib/GIO, or any D-Bus CLIENT LIBRARY of any kind.** Where the
desktop's integration surfaces need a bus (the portal FileChooser,
StatusNotifier tray, org.freedesktop.Notifications, AT-SPI2, the
appearance portal, ScreenSaver inhibition), FDK speaks the D-Bus
wire protocol ITSELF — `src/dbus/` is a complete client
implementation written from the public specification in raw C
(docs/dbus.md, DECISIONS D-001). No bus present, no portal, no
screen reader: every one of those surfaces degrades to its
documented in-toolkit fallback (the own file browser, the embedded
narrator, the core cursor font) — the application never fails
because a daemon is absent. The embedded narrator
(`fdk_a11y_set_speaker`) remains the no-daemon a11y answer; the
AT-SPI bridge is the OPT-IN desktop-interop side, not a requirement.

**Added in 1.1.0 — runtime-optional, dlopen'd, never build deps**
(the fontconfig posture, applied to the text/input/cursor lanes;
none of them appears in the link line, and a build without any of
them is unchanged — absent library means the documented fallback,
never an error):

| Library | Purpose | Fallback when absent |
|---|---|---|
| `libharfbuzz.so.0` + `libfribidi.so.1` | Complex-script shaping + bidi (M-2) | The legacy codepoint-at-a-time LTR shaper (FDK_SHAPING=0's own path) |
| `libXi.so.6` | XI2 touch events (M-3) | No touch events; the pointer/keyboard streams unchanged ($FDK_NO_XI2 pins it off) |
| `libXrender.so.1` | The RENDER cursor lane — themed ARGB cursors (S-5) | The core cursor font's nearest-glyph table (every shape keeps an honest twin) |
| `libGL.so.1` | The X11 GPU present lane | The CPU present path (always the design center) |

The audit trail sentence below was also stale: the link line has
carried `-lXrandr` since M-6 (see the table above) and `-lz` since
the PNG encoder — it is `-lX11 -lXext -lXrandr -lwayland-client
-lxkbcommon -lm -ldl -lz` (Wayland half only when the backend is
built). `libatspi`/`libdbus` still appear nowhere in the build;
the wire bytes FDK's own client emits are pinned against captured
libdbus reference output in `make test` (tests/test_dbus.c) — the
cross-implementation proof, not a dependency.

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
