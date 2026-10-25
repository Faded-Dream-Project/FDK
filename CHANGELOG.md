# Changelog

FDK's public version moves only by a recorded project-owner decision
(docs/versioning.md — the rule, the enforcement, the full history
including the 2026-10-11 retraction of a premature 1.0.0). This file
is the consumer-facing summary of what each version changed; the
engineering narrative lives in docs/roadmap.md and PROGRESS.md.

The format follows Keep-a-Changelog conventions adapted to this
project: releases are chronological, each entry carries the public
API surface delta explicitly (FDK's ABI promise — docs/abi-policy.md —
makes that the contract that matters), and in-development release
candidates are marked until the owner promotes them.

## [1.1.0-rc1] — 2026-10-25 (release candidate; no tag — the owner
                 decides the final 1.1.0)

### Added — the extension API (`include/fdk/fdk_extend.h`)

The point of this release: FDK stays simple and becomes the root
other libraries and packages build on. Add-on packages can now be
written against a small, documented, stable, versioned public
surface instead of FDK's internals. Full documentation:
docs/EXTENDING.md.

- **Extension API versioning**: `FDK_EXT_API_VERSION` (compile-time)
  and `fdk_ext_api_version()` (runtime) — add-ons declare what they
  need and check what they got; additions bump the number,
  removals/renames are forbidden (same append-only discipline as the
  core ABI policy).
- **Surface kinds**: `fdk_ext_register_kind()` +
  `fdk_ext_surface_ops` (create / destroy / popup_attached) — an
  add-on registers a named kind of surface FDK knows nothing about,
  creates the native surface itself (X window, or wl_surface plus
  whatever protocol role it speaks), and hands it to FDK, which
  adopts it: event routing, the full `fdk_surface` render path
  (framebuffer, damage tracking, present, frame pacing), and popup
  parenting all work on extension surfaces.
- **Event loop integration**: `fdk_ext_loop_add_fd()` /
  `fdk_ext_loop_remove_fd()` — poll fd sources integrated into
  `fdk_pump_events()` with the timer discipline (reentrancy-safe,
  snapshot + deferred free). Timers ride the existing public
  `fdk_timer_add/remove/reset` API — one primitive, not two.
- **Documented native accessors**: `fdk_ext_get_native_display()`
  (the `Display*` / `struct wl_display *` the context is using),
  `fdk_ext_surface_native()` (the surface's `Window` XID or
  `wl_surface *`), `fdk_ext_surface_get_surface()` (the public
  `fdk_surface *` render target). No private struct is ever
  reachable.
- **Popups parented to extension surfaces**:
  `fdk_ext_popup_create()` — parent-relative positioning, input
  grab, outside-click and Escape dismissal (delivered as
  `FDK_EVENT_WINDOW_CLOSE_REQUEST`), on both backends; on Wayland the
  kind's `popup_attached` hook lets the add-on parent the xdg_popup
  to its own role object (the layer-shell `get_popup` pattern).
- **Backend + capability queries**: `fdk_ext_active_backend()`
  (Wayland or X11/XLibre) and `fdk_ext_has_capability()` (SHM
  rendering, popup grabs, EWMH dock hints, compositor frame pacing).

### Added — fdk-shell, the first add-on package (separate directory,
             own build, own version — ready to move to its own repo)

- Layer-shell-style surfaces behind one API: layers (background /
  bottom / top / overlay), edge anchors, margins, exclusive zone,
  keyboard interactivity modes, per-output selection (Wayland).
- Wayland backend: wlr-layer-shell v4 via wayland-scanner generated
  bindings (vendored protocol XML).
- X11/XLibre backend: `_NET_WM_WINDOW_TYPE_DOCK` +
  `_NET_WM_STRUT_PARTIAL` struts — the same layering semantics the
  EWMH world offers, behind the same API.
- Popups built on the Part-1 popup API; a demo bar (top-docked,
  one working dropdown); Xvfb strut/dock verification test and a
  prefix-isolation build test proving fdk-shell compiles against
  FDK's installed public headers and library ONLY.

### Packaging / build

- The shared library file becomes `libfdk.so.1.1.0-rc1`; the SONAME
  symlink stays `libfdk.so.1` (the additions are ABI-append-only —
  no existing public symbol, type, or struct layout changed).
- New public header installed: `include/fdk/fdk_extend.h`; the
  umbrella `fdk.h` includes it.

### Fixed — the rc version vs. the five package grammars

Found live by CI on the first push of the candidate: the raw
`1.1.0-rc1` string is illegal or wrong in four of the five package
version grammars (rpm rejects the hyphen outright; apk reserves it
for pkgrel; arch's `pkgver` forbids it; xbps forbids it AND the
ambiguity broke repository lookups; deb accepted it but ordered the
rc ABOVE the future final). The pipeline now translates the suffix
once, at the metadata layer (`fdk_packver` in `packaging/ci/common.sh`):
`1.1.0~rc1` for deb/rpm, `1.1.0_rc1` for apk, `1.1.0rc1` for
arch/xbps — while the `.so` filename, pkg-config version, and smoke
test keep the raw string the library itself reports. Plain releases
pass through unchanged. Documented in `docs/packaging.md`. Also
corrected three stale "ten shipped theme faces" package descriptions
the one-face alignment had missed (deb control, rpm spec, xbps
descriptions now say the default theme, faded-dream).

### Unreleased-candidate notes

- No git tag, no GitHub release for rc1 (owner's instruction). The
  v1.0.0 release and its tag are untouched. CI still builds and
  install-tests all five package families on every push to main —
  that is pipeline validation, not a release, and it is exactly how
  the rc-version defect above was caught.

## [1.0.0] — 2026-10-12 (re-cut same day as the one-face alignment)

The first public release. The versioned-SONAME scheme activated with
it (`libfdk.so.1`). Highlights of the surface as shipped:

- Dual-backend core (Wayland via xdg-shell; X11/XLibre), software
  renderer (exact + antialiased rasterizer, damage-tracked
  presentation, MIT-SHM / wl_shm transport, optional GLX present
  lane), retained widget tree + box layout + text (stb_truetype) +
  theming (the built-in default plus `.fdk` face files), clipboard
  (text + PNG images, PRIMARY selection), drag and drop, dialogs,
  i18n, a11y narration, undo, prefs, animation, the fdk-theme /
  fdk-prefs / fdk-set CLI tools.
- Ships exactly ONE theme — `faded-dream`, the packaged copy of the
  built-in default/fallback. The next faces are `.FDKThemes`
  drop-ins users install (`~/.FDKThemes` / system data dirs), never
  package content; the Makefile's `SHIPPED_THEMES` is pinned to the
  single file so a stray face cannot ride into a release.
- Five package families built and install-tested in CI (deb, rpm,
  pacman, apk, xbps), source archive + SHA256SUMS, all published on
  the main repository's v1.0.0 release.
