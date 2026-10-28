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

## [Unreleased-candidate features] — 2026-10-28 (on the rc2 line)

### Added — window identity: icons, PID, host, role (M-4 / B-04)

- **`fdk_window_set_icon()` (public API)**: a window's icon — the
  image taskbars, docks, and window switchers show for it — as
  width x height ARGB8888 pixels (the toolkit's native `fdk_u32`
  packing, identical bytes to `_NET_WM_ICON`). X11 writes the EWMH
  property (byte-exact round-trip pinned by the integration test);
  Wayland speaks `xdg-toplevel-icon-v1` where the compositor offers
  it (KDE Plasma 6.3+; non-square sources are letterboxed onto the
  protocol's required square buffer), and where it does not the call
  is the documented honest no-op — one INFO line, no error, because
  Wayland offers no icon fallback path. `NULL`/non-positive
  dimensions clear the icon on both backends.
- **`fdk_window_set_role()` (public API)**: the ICCCM
  `WM_WINDOW_ROLE` string (X11; no Wayland protocol exists — the
  documented no-op). Windows now carry create-time identity the
  desktop reads: `_NET_WM_PID`, `WM_CLIENT_MACHINE`, and a default
  role (`main`/`popup`); WM_CLASS was already set from the
  app_id. On Wayland the app_id remains the whole identity story
  by protocol design.
- **Tests**: the X11 integration suite reads every identity
  property back from the server through a second raw-Xlib
  connection (pid equality, hostname equality, role default +
  override + revert, byte-exact icon round-trip including replace
  and clear); the Wayland suite proves set + clear keep the
  connection healthy in both compositor worlds (manager bound or
  absent) and the surface path never trips on icon state.

### Added — CFF outlines: OpenType 'OTTO' faces render (the Arch tofu report)

- **The native Type 2 module (core, `src/text/cff.c`)**: CFF-flavored
  OpenType fonts — Noto Sans CJK, URW base35, most .otf files — now
  load and render instead of being rejected as silent-tofu risks.
  Both dialects: `CFF ` including CID-keyed FDArray/FDSelect, and
  `CFF2` at the default instance. Written from the public Adobe
  specifications with real bounds, an operator budget, depth caps
  and stack limits (D-011; provenance in docs/THIRD-PARTY.md).
- **One rasterizer, both flavors**: CFF geometry reaches the SAME
  `stbtt_Rasterize` call the glyf path uses, as `stbtt_vertex`
  arrays — no look or metric drift between flavors. HarfBuzz glyph
  extents route through the native ink box; advances stay with
  hmtx; `maxp numGlyphs` must equal the CharStrings INDEX count or
  the face is rejected.
- **Hardened load order**: the native parser gates 'OTTO' faces
  BEFORE stb's own CFF initialization runs (stb trusts a fabricated
  512 MiB extent and asserts on malformed bytes — the fuzz battery
  found it live). Fallback candidates may now be either sfnt flavor;
  a loaded fallback that does not cover the segment's probe
  codepoint is discarded before caching.
- **tests/test_cff.c**: dual-decoder vertex agreement against stb's
  own Type 2 path over real URW + Noto faces, a crafted seac/subr/
  cyclic-subr font (real foundry fonts no longer carry seac), loader
  rejection (numGlyphs cross-check, hostile extents, truncation),
  and a deterministic mutation fuzz over the CFF table region.


### Added — complex-script text shaping, level 1 (M-2 / B-02)

- **The shaped pipeline (core, `src/text/shaping.c`)**: text now
  renders through the full complex-script stack when HarfBuzz and
  FriBidi are loadable at run time — dlopen'd, never build
  dependencies, exactly the GLX/fontconfig posture. FriBidi resolves
  the Unicode bidi algorithm (Arabic, Hebrew, Persian, Urdu render in
  VISUAL order instead of backwards; brackets mirror); HarfBuzz
  shapes through the font's real GSUB/GPOS tables (joining forms,
  ligatures, mark attachment with x/y offsets, real kerning) fed by
  custom font funcs backed by stb_truetype itself (26.6 pre-scaled,
  so shaped and legacy advances agree by construction); fontconfig
  supplies a per-script fallback font, charset-filtered on a probe
  codepoint, so CJK next to Latin picks the right font per segment.
  `$FDK_SHAPING` pins the posture (0 legacy, 1 require, unset auto);
  without the libraries the 1.4.x codepoint-at-a-time walk runs with
  IDENTICAL metrics — pinned by a test that re-execs itself under
  the other posture.
- **Every text consumer reads shaped placements**: measure, draw,
  span draws with decorations, markup labels, TextView, wrap,
  ellipsize, and the Entry caret + click hit-test (carets and clicks
  can no longer disagree with the ink). One rounding rule survives:
  the pen floors per glyph, rounds once per reported width.
- **Public surface (+2, 797 exports)**:
  `fdk_text_shaping_available()` (the pipeline's verdict — surface
  it or ignore it, the API is identical either way) and
  `fdk_font_has_glyph()` (coverage probe).
- **tests/test_shaping.c**: RTL visual order, mixed bidi, fallback
  selection, cluster/byte mapping, caret monotonicity, Arabic wrap +
  ellipsize agreement, draw/measure agreement on a trilingual
  string, the fallback lifecycle (dies with its primary, ASan
  clean), and the cross-posture Latin pin. CI runs both postures
  (the ubuntu job's auto probe + an explicit `FDK_SHAPING=0` leg;
  every distro container gains harfbuzz + fribidi + a CJK font).
- **Packaging**: the optional runtimes are declared the `libgl1`
  way — deb `Recommends: libharfbuzz0b, libfribidi0`, rpm
  `Recommends:`, arch `optdepends` — never hard dependencies.

### Fixed

- **The live desktop-bus posture of the TASK B live tests**: both
  `test_notify_client` and `test_tray_item` now follow
  `test_dbus_live.c`'s skip discipline — no display backend, a stale
  bus address, or a REAL notification daemon / StatusNotifierWatcher
  already owning the bus name each skips loudly (exit 0, guidance
  included) instead of aborting on an assertion, and the mock never
  queues behind (DO_NOT_QUEUE) a real desktop's watcher — running
  the binaries by hand on a live desktop can no longer crash the
  suite OR vandalize the user's tray by stealing the watcher name.


### Added — the app-facing notification + tray API (TASK B side 1)

- **`fdk_notify.h` (core)**: the app side of
  org.freedesktop.Notifications on the native D-Bus client —
  send (summary/body markup/actions/replaces/timeout/icon as
  name, path, or raw image bytes), withdraw, and
  NotificationClosed + ActionInvoked events carrying the
  note_user the send took. The daemon adoption flow (name watch,
  capabilities + identity probe), the NO_BUS/NO_DAEMON states
  reported through the API, re-adoption on daemon restart, and
  teardown-inside-its-own-event are proven by
  tests/test_notify_client.c against an in-test mock daemon on a
  REAL bus (every marshaled field validated at the mock, the
  image-data bytes read back flat).
- **`fdk_tray.h` (core)**: the app side of the tray — a
  StatusNotifierItem export (both interface spellings, the New* +
  PropertiesChanged dual emission) with a complete DBusMenu
  server (labels, separators, check/radio with sibling clearing,
  any-depth submenus, enabled/disabled, live LayoutUpdated, the
  AboutToShow dirty handshake, Event→click callbacks). KDE-first
  / ayatana-fallback watcher registration with re-registration
  through watcher restarts, proven by tests/test_tray_item.c
  against an in-test mock watcher/host.
- **The byte-array bulk form** in the D-Bus value model:
  `fdk_dbus_value_new_byte_array` / `_array_flat_bytes` — one
  flat allocation instead of n byte nodes, marshaled as one
  block, produced by the UNMARSHALER too (every 'ay' on the wire
  is flat). A 256x256 icon stopped being 256K heap nodes on the
  UI thread; notifyd's and trayhost's byte walks take the flat
  path first (both rigs re-run ALL PASS). Exports 758 → 795.

### Added — the UI font size (TASK C)

- **`fdk_ui_font_size` / `fdk_ui_scale` / `fdk_ui_font_px`**
  (core, fdk_theme.h; src/theme/ui_metrics.c): one process-wide
  answer to "what size is the UI text?", resolved like the color
  scheme — `$FDK_UI_FONT_SIZE`/`$FDK_UI_SCALE` overrides, then
  FDK's settings store (`ui.font_size`/`ui.scale`), then the
  desktop's statement (XSETTINGS `Gtk/FontName` on X11; the
  portal's `font-name` and `text-scaling-factor` — followed LIVE
  unless overridden), then the 16 px / 1.0 defaults. The shells
  scale every dimension off it (card, pads, icon box, buttons,
  bar height, cell, menu rows) with `--font-size` pinning; core
  chrome re-derives its historical sizes at the 14 px baseline
  (ratios preserved; `FDK_UI_FONT_SIZE=14` = the old pixels).
  tests/test_ui_metrics.c plus the rigs' XS sweeps (12/16/24/32
  through the ENV channel: glyph-free side pads, on-screen
  menus, trace math) all green.

### Added — the StatusNotifier tray host (S-1 / A-11)

- **`trayhost` (fdk-shell example, S-1 / A-11)**: a complete
  org.kde.StatusNotifierWatcher host on FDK's native D-Bus client,
  rendering the registered StatusNotifierItems as a floating
  top-right TOP-layer strip on both backends. The full protocol
  surface: both registration dialects (the KDE bus-name form and
  the Ayatana object-path form), the four watcher properties
  through org.freedesktop.DBus.Properties (Get/GetAll/
  PropertiesChanged emission), the host self-registration over
  the real bus path, item loading with the
  org.kde→org.ayatana→org.freedesktop interface fallback chain,
  the a(iiay) ARGB32 network-byte-order pixmap decode with
  best-size selection, the (sa(iiay)(ss)ss) ToolTip parse, the
  New* signal family (scoped to the item's unique bus name — the
  documented well-known-sender match-rule trap), and vanished-item
  drops via NameOwnerChanged (hostile clients never unregister
  politely). The full com.canonical.dbusmenu client: GetLayout's
  two-out-argument reply parse (u revision + the bare root
  struct), AboutToShow handshakes, Event(clicked/opened/closed)
  with NO_REPLY_EXPECTED, mnemonic-underscore stripping,
  separators/checkmarks/radios/disabled rows, cascading submenus
  that flip at the screen edge, and mid-open refreshes that are
  discarded stale-safe (the open rows point into the old tree).
  Rendering honesty: Passive items fade to alpha 0.45 (never
  hidden), NeedsAttention swaps to the attention pixmap plus an
  accent marker strip, left/middle/right clicks drive
  Activate/SecondaryActivate/the menu. Hostile-input posture and
  the polite bus-name etiquette match notifyd's. Env trace:
  FDK_TRAYHOST_TRACE=1. Rig: `make -C fdk-shell test-tray` (62
  checks; D-007 records the shape decisions).

### Added — the notification daemon (S-2 / A-12) and its substrate

- **`fdk_icon_theme_surface(name, size)` (FDK core, public API —
  the 758th export)**: the icon-theme engine the file dialogs use,
  exposed as an owned-clone lookup for applications that draw their
  own chrome. Resolves one icon name through the full live-theme
  policy (XSETTINGS, the GTK/KDE/XFCE channels, Inherits, hicolor,
  pixmaps), rasterizes themed SVG at the requested size, tints
  `-symbolic` names, and hands back a fresh ARGB8888 surface; NULL
  on a miss is the caller's cue to draw its own glyph.
- **`notifyd` (fdk-shell example, S-2 / A-12)**: a complete
  org.freedesktop.Notifications server on FDK's native D-Bus client,
  displaying cards as overlay layers on both backends. The full
  specification surface (Notify with replaces_id/actions/hints,
  expiry semantics including critical-never-expires, CloseNotification
  with correct reasons, ActionInvoked, honest GetCapabilities,
  GetServerInformation, Introspect), a render-only markup sanitizer
  (links drawn, never followed; `<img>` stripped, src never opened),
  size/rate caps with every call still answered, the polite
  bus-name etiquette (one clear line naming an incumbent through its
  own GetServerInformation; `--replace` / `--wait` for the takeover
  dance), per-card layers sized exactly to the card (the honest
  click-through shape until input regions land), stacking that
  self-heals across output changes, and an env-gated trace
  (FDK_NOTIFYD_TRACE=1). The rig
  (`make -C fdk-shell test-notifyd`) proves it against a real
  dbus-daemon with libdbus and GLib as cross-implementation callers,
  plus pixel, XTEST-click, EWMH, and layer-shell legs.
- **`docs/TESTING-ON-DESKTOP.md`**: the numbered owner-verification
  checklist for everything only a real desktop can prove.

### Fixed

- **LSan vs fontconfig's cold cache**: a fresh CI runner's first
  fontconfig query writes its on-disk cache through the FcAtomic
  machinery, whose small bookkeeping pools (320 B) sit outside
  fontscan.c's `__lsan_disable` bracket and are held for the process
  lifetime by design — nondeterministic by cache temperature. Now
  suppressed by module in tests/data/lsan-gl.supp (the GL-lane
  pattern), so sanitized runs stay strict for everything FDK owns.

## [Unreleased-candidate fixes] — 2026-10-27 (on the rc2 line)

### Fixed — CI and packaging truth

- **The five-family packaging red at 0.2.0**: fdk-shell's Makefile
  carried its own hand-maintained version (0.1.0) next to the
  header's single source of truth (0.2.0) — the staged library was
  `libfdk-shell.so.0.1.0` and the generated `.pc` said `Version:
  0.1.0`, and every distro install-test caught the mismatch. The
  Makefile now derives the version from the header (FDK's own
  established pattern).
- **The alpine CI flake**: readbacks ride a second X connection and
  could beat the just-flushed present to the server, reading the
  PRE-paint frame (the empty search entry — pure fill — exactly the
  asserted-against pixel). `x11_frame_settle()` now guards all 18
  paint-then-readback sites in the X11 suite.
- **The void mirror outage**: `repo-default.voidlinux.org`'s
  round-robin was unreachable for hours; the single same-host retry
  could not recover. Real mirror rotation now runs before every
  remote install (build + both install-test containers).

### Fixed — D-Bus accessors (found by the alt-client test port)

- `fdk_dbus_message_signature()` reported the frozen received-message
  field for locally BUILT messages — an app that appended a body read
  back `""`. It now reports the builder's live signature (what
  serialize() puts on the wire).
- `fdk_dbus_message_read_args()` consulted only the received-message
  body store — reading back a locally built message silently returned
  "no arguments". It now reads the builder's buffers.
- `dbus_internal.h` unconditionally redefined `FDK_LOG_TAG`,
  re-attributing a caller's own log tag (the test binary's
  `dbus-test` became `dbus` on every line). The including
  translation unit owns its tag now.

### Added — the cross-implementation proof

`make test-dbus` gains the libdbus leg: dbus-send calls the service
FDK serves and libdbus's own parser must accept FDK's reply bytes
and print them (ported from the retired alt client line; see
DECISIONS D-004). Plus the append all-or-nothing, reply/error
routing, and error-name discipline sections in the headless battery.

### The repository

- A `release` branch now exists (fast-forward-only pointer at
  approved, tagged, CI-green releases — docs/RELEASING.md; created
  at the v1.0.0 tag commit). CI runs on both `main` and `release`;
  the README and packaging docs point consumers there.

## [1.1.0-rc2] — 2026-10-26 (release candidate; no tag — the owner
                 decides the final 1.1.0)

### Added — the native D-Bus client (`include/fdk/fdk_dbus.h`)

The architectural unlock of the rest of the roadmap: a complete
D-Bus client implemented FROM THE PUBLIC SPECIFICATION in raw C
(src/dbus/), taking **no D-Bus library dependency of any kind** —
no libdbus, no sd-bus, no GLib — and no new link-time or run-time
dependency at all. Everything rides FDK's own event loop; every
byte from the socket is treated as untrusted input.

- **Wire format**: full marshaling and unmarshaling of every basic
  and container type (alignment, both endiannesses, nesting depth
  ≤ 64, 8 MiB message cap), with the byte layout PINNED against
  captured reference-implementation output in the tests (the
  169-byte GetNameOwner frame, variant bodies, int64 arrays,
  empty-array padding).
- **Transports + auth**: session/system bus resolution
  (DBUS_*_BUS_ADDRESS, XDG_RUNTIME_DIR, ';' lists),
  unix/path, unix/abstract, unix:dir, tcp:, nonce-tcp (percent
  decoding); SASL EXTERNAL with the ANONYMOUS fallback; non-blocking,
  CLOEXEC sockets; a no-bus environment fails fast with ONE log
  line (the degradation contract every feature above this layer
  keys off).
- **Calls, signals, names**: async method calls with timeouts (at
  most once; late replies to canceled calls dropped), match rules
  (AddMatch refcounted, client-side fail-open evaluation incl.
  argN), name ownership (RequestName + unicast NameAcquired/
  NameLost), name watching with transition reconciliation
  (GetNameOwner + NameOwnerChanged), properties (Get/Set + typed
  PropertiesChanged subscription), introspection (a lenient XML
  parser serving a full node model), and serving incoming method
  calls (with the UnknownMethod auto-reply posture).
- **The reentrancy discipline**: callbacks may call any fdk_dbus
  function — including closing the connection whose callback is
  running; a DEAD connection stays a valid handle until the app
  closes it (every call answers a clean error, never a dangling
  dereference); closes from inside a callback free at the
  outermost wrapper.
- **Hardening**: fuzz tests in plain `make test` (all truncation
  prefixes + 20k seeded mutations under ASan/UBSan), grammar
  validators (names/paths/strict UTF-8), uninvited SCM_RIGHTS
  descriptors closed on arrival.
- **Test surface**: tests/test_dbus.c (headless wire suite) and
  tests/test_dbus_live.c (`make test-dbus` — a REAL dbus-daemon via
  dbus-run-session; the ubuntu CI job makes skips fatal). 115 new
  exported symbols (756 total), append-only; SONAME stays
  libfdk.so.1.

### Added — extension API

- `fdk_ext_loop_update_fd()` — retarget a live fd source's watch
  mask (the POLLOUT busy-wait trap's answer; the D-Bus client is
  the first consumer). FDK_EXT_API_VERSION stays 1: a purely
  additive function, no ops-table growth, old add-on binaries keep
  working.

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
- Desktop integration on X11 (found live on a real NVIDIA desktop):
  layers are STICKY across workspaces (`_NET_WM_DESKTOP` =
  0xFFFFFFFF + `_NET_WM_STATE_STICKY`, set as pre-map properties
  and re-asserted as root client messages after mapping — the
  Wayland behavior, arranged through EWMH); TOP/OVERLAY layers
  carry `_NET_WM_STATE_ABOVE`, OVERLAY adds SKIP_TASKBAR/SKIP_PAGER
  (the strongest EWMH stacking behavior; the fullscreen-over-docks
  protocol ceiling and what a custom WM would need to honor a true
  overlay layer are documented in fdk-shell's README); and the demo
  bar repaints on `FDK_EVENT_WINDOW_EXPOSE` — the content-loss
  contract that fixes the ghost-bar bug (a desktop round-trip left
  the bar showing whatever was underneath).
- fdk-shell's shared library links FDK's shared library (a real
  `NEEDED: libfdk.so.1` entry — the whole chain resolves through
  the loader and the distro dependency tooling), and its release
  config drops the sanitizer runtimes like FDK's does.
- Popups built on the Part-1 popup API; a demo bar (top-docked,
  one working dropdown); Xvfb strut/dock verification test and a
  prefix-isolation build test proving fdk-shell compiles against
  FDK's installed public headers and library ONLY.
- fdk-shell packages in all five distribution families
  (deb/rpm/arch/apk/xbps), each declaring a real, versioned
  dependency on the FDK runtime pinned at the extension-API floor;
  every family's install-test proves the dependency is enforced by
  the native package manager (installing fdk-shell alone is
  REFUSED), and the build itself compiles against the INSTALLED FDK
  packages — the isolation proof re-run inside every packaging
  container (fdk-shell/packaging/).

### Packaging / build

- The shared library file becomes the numeric `libfdk.so.1.1.0`
  (pre-release suffixes never appear in the .so file name — the rc
  lives in the version string, pkg-config, and package metadata);
  the SONAME symlink stays `libfdk.so.1` (the additions are
  ABI-append-only — no existing public symbol, type, or struct
  layout changed).
- New public header installed: `include/fdk/fdk_extend.h`; the
  umbrella `fdk.h` includes it.

### Fixed — the rc version vs. the five package grammars

Two distinct rc-version defects, both found live by CI on the
candidate's first pushes:

1. **Package metadata**: the raw `1.1.0-rc1` string is illegal or
   wrong in four of the five package version grammars (rpm rejects
   the hyphen outright; apk reserves it for pkgrel; arch's `pkgver`
   forbids it; xbps forbids it AND the ambiguity broke repository
   lookups; deb accepted it but ordered the rc ABOVE the future
   final). The pipeline now translates the suffix once, at the
   metadata layer (`fdk_packver` in `packaging/ci/common.sh`):
   `1.1.0~rc1` for deb/rpm, `1.1.0_rc1` for apk, `1.1.0rc1` for
   arch/xbps. Plain releases pass through unchanged.

2. **The .so file name**: the real file was installed as
   `libfdk.so.1.1.0-rc1`, and Alpine's abuild derives the apk
   `so:`provides version from the .so FILENAME tail
   (`sover=${file##*.so.}`) — producing
   `provides = so:libfdk.so.1=1.1.0-rc1`, which `apk index`
   rejects with "package file format error" (reproduced locally
   with apk-tools 2.14.12; even a hyphenless `1.1.0rc1` tail is
   rejected — only numeric tails are valid). The Makefile now names
   the real file with the numeric `FDK_SO_VERSION`
   (`libfdk.so.1.1.0`) — the GNU convention; pre-release suffixes
   never appear in the .so file name.

The dividing line in both fixes: pkg-config `Version` and the smoke
test keep the raw string the library itself reports, while the .so
file name is numeric — the file name and the version string are
different things, and each stays what its grammar requires.
Also fixed a third, independent CI flake the second push exposed:
the HiDPI GUI test's private 192-dpi Xvfb used a hard-coded `:98`
inside the `make test-x11` harness's own display range
(90 + pid%400), and on collision the test unlinked the SUITE
server's socket, its probe then reached the 96-dpi suite server
over the TCP fallback, and the scale assertion aborted the debian
job (1-in-400 per run, PID-gated). The private display now lives
below every harness range, is checked free before spawning, and is
verified OURS by its 800x600 geometry — a foreign server is never
touched (docs/testing.md's 1.1.0-rc1 lesson).
Documented in `docs/packaging.md`. Also corrected three stale "ten
shipped theme faces" package descriptions the one-face alignment
had missed (deb control, rpm spec, xbps descriptions now say the
default theme, faded-dream).

### Fixed — the GPU present lane's self-check verdicts (found live
                on an NVIDIA GTX 1050 Ti desktop)

The lane's first-frame self-verification had three defects, all
visible as repeated warnings on a real desktop (every dropdown
click re-logged the whole failure sequence, once per new popup
window):

1. **Not-viewable reads were treated as GPU failures.** A popup
   whose first present raced its own map got `XGetImage` →
   BadMatch — a property of the WINDOW STATE, not the driver. The
   check now gates on viewability first (`map_state ==
   IsViewable`, the programmatic MapNotify/Expose equivalent) and
   clamps its probe region into the server-reported window bounds;
   a not-viewable or unreadable window is INCONCLUSIVE and retried
   at the next completed present, never a downgrade.
2. **Present-latency timing was treated as a driver limitation.**
   The first read after `glXSwapBuffers` can still show the
   window's pre-frame background (DRI3/Present schedules the flip
   at vblank; compositing WMs re-composite on their own clock) —
   the live report's "server (000000) vs CPU (color)" mismatch was
   exactly the bar window's BlackPixel background. A bounded retry
   budget (~3 vblanks) now separates the timing case (a retry
   matches: PASS, logged once at INFO) from the real limitation
   (every retry mismatches: FAIL).
3. **The verdict was per-window.** Each new popup re-engaged the
   lane, re-probed, re-failed, and re-logged — the repeated
   warnings, and the XID churn (window + GLX context + GC + two
   SHM segments per click). The verdict is now decided once per
   connection (the driver-wide truth): PASS and later windows skip
   the probe entirely; a REAL failure refuses the lane for the
   whole process (the same env-knob-keyed policy cache the
   renderer verdict uses) and logs exactly one warning.
   `FDK_GL_SELFCHECK_POISON=1` is the new test seam (the
   FDK_GL_FAIL_AT shape) and `fdk__x11_gl_test_reset()` clears the
   stamp between test groups.

### Fixed — the second live-desktop round (2026-10-26; labwc/Wayland + Openbox/NVIDIA X11)

Six findings from the owner's machines, each reproduced headlessly
before the fix and measured before/after where a number exists:

1. **Wayland: the buffer-in-flight freeze (W1)** — the release wait
   in `get_framebuffer` used the DEFAULT-queue
   `wl_display_prepare_read()`, which refuses while ANY default-queue
   event is pending — under pointer motion that is always true, so
   the wait degenerated into a pure 100ms sleep: releases on the
   socket were never read, every acquire refused, the dispatch rate
   collapsed to ~10 events/s against ~1000/s arriving, and wlroots
   killed the connection when the server-side buffer overflowed. The
   wait is now queue-scoped (`prepare_read_queue` on the release
   queue): a release on the wire lands within one poll slice, and a
   pending release event is dispatched immediately instead of slept
   on. Complements: `wl_display_flush` after every commit (a present
   between pumps no longer waits a full pump cadence to reach the
   compositor), the frame request marshalled BEFORE the commit it
   paces, an fd-lifetime flush in `create_shm_buffer`/cursor upload
   (libwayland resolves fd arguments at FLUSH time by number — pools
   created in one un-flushed burst aliased the newest memfd
   server-side, so whole paint cycles silently never appeared), a
   frame-callback pacing gate in the dispatch tail (hover repaints
   can no longer commit faster than the compositor consumes), and the
   release budget tightened 100ms -> 25ms. Verified by flood rigs on
   headless sway AND labwc (~1000 injected events/s for 45s, popup
   held open, max-rate variant): zero "in flight" warnings, zero
   connection deaths, buffers created == destroyed (no leak), the
   bar alive and selecting items afterwards.
2. **Wayland: the cursor never answered enter (W2)** — a client
   that answers no `wl_pointer.enter` keeps whatever shape the
   compositor last showed anywhere (the report: the resize cursor
   "sticking" over the bar until the pointer left). Every enter now
   re-applies the window's stored shape citing the ENTER serial:
   cursor-shape-v1's `set_shape` when the compositor offers it
   (sway 1.10 does), else `wl_pointer.set_cursor` with the
   XCursor-theme image. New public surface at every layer —
   `fdk_window_set_cursor()`/`fdk_cursor_shape` (core),
   `fdk_ext_surface_set_cursor()` (extension), and
   `fdk_shell_{layer,popup}_set_cursor()` (fdk-shell) — two shapes:
   the arrow and the pointing hand (the demo bar shows the hand over
   its button). Rig-verified at the protocol level (every enter
   serial cited by a `set_shape`) and the pixel level (hand over the
   button, arrow elsewhere, hand again on motionless re-entry).
3. **The hover that lit the whole bar (W3)** — the DEMO's bug, not
   the pointer path's: the bar repainted its entire background in a
   hover tint when any widget was hovered (the bar's own
   `hover_button ? BAR_BG_HOVER : BAR_BG`). Only the button chip
   carries hover now; ENTER seeds hover with its carried position
   (re-entry onto the button highlights without any motion) and
   LEAVE clears it. Pixel-verified in all four states.
4. **X11: every app paid the input-method startup (X1)** —
   `XOpenIM` ran at connect for every application; with a slow or
   dead IM daemon configured that is SECONDS of startup for
   pointer-only apps. The IM and per-window XICs are now lazy
   (first FocusIn/KeyPress), `FDK_STARTUP_TRACE=1` phases the whole
   startup (connect legs, EWMH, theme, first fontconfig query, GL
   self-check, first present) with per-phase and total milliseconds,
   and `FDK_LOG_LEVEL` (read at library load) answers field reports
   without recompiling. Measured with a 700ms XOpenIM stall injected
   via LD_PRELOAD: eager init 714ms / total 769ms -> lazy init 3ms /
   total 54ms (the stall now costs a pointer-only app nothing); the
   healthy-IM case improved 16ms -> 4ms of init. The typing tests
   (real key entry through the lazy XIC) all still pass. One
   regression FOUND AND FIXED by this change: window-create's XID
   was previously flushed to the server only by the XIC round-trip —
   a second connection could observe BadDrawable racing an un-flushed
   create (reproduced minimal; an explicit `XSync` barrier now owns
   the contract).
5. **X11: the dropdown highlight trailing the pointer (X2)** — two
   confirmed contributors. (a) The dispatch loop translated every
   queued MotionNotify one at a time with a repaint each — now
   redundant motions for the same window collapse to the latest
   (buttons/keys/crossings keep exact order). (b) Every popup built
   its OWN GLXContext on the GPU present lane — the panel/popup
   class (<= 131072 px) now stays on the CPU path by policy (no
   context, no texture upload, no vsync-scheduled swap; a
   small-windows-only process never even dlopens libGL;
   `FDK_GL_FORCE=1` overrides, and the byte-identical equivalence
   proof still runs through it). New measurement:
   `FDK_POPUP_TRACE=1` reports `[popup] motion->present N ms` at
   `fdk_surface_present` — the convergence of every present path, so
   extension-API popups (which paint directly) are measured exactly
   like widget-tree windows. Under Xvfb/llvmpipe flood (max wire
   rate, hover forced to flip every event): median 0-1ms, p90 1ms,
   max 32-38ms in both lanes. On the reporter's NVIDIA hardware the
   expected field signature: no more "GPU present lane active" per
   click, and the same sub-frame latencies.
6. **The popup trace's "(destroy)" (X3)** — close-requests now carry
   their reason (outside click / Escape / compositor dismissal —
   the generators stamp a single-slot channel; was "(unknown)" for
   the X11 outside-click and the Wayland dismissal), and destroys
   report the best-known reason, including "in-bounds button N ms
   ago — likely item selected". All six paths verified live on both
   backends.

`scripts/w1_stress.sh`, `w2_verify.sh`, `w3_verify.sh`,
`x2_measure.sh` (with the XTest driver) are the reusable rigs.

The round's own push then exposed its build defect, closed before
the next: the cross-package prerequisites that keep sibling-tree
builds honest (an FDK rebuild must invalidate fdk-shell's binaries —
the rigs' three-patch-old bar) had landed as UNCONDITIONAL make
prerequisites on `../build/libfdk.a`/`../build/libfdk.so`, and the
pristine fdk-shell-only tarball families (rpm/alpine/arch) died at
`No rule to make target '../build/libfdk.so'` — their link inputs
point at the installed FDK and their trees carry no sibling checkout
at all (debian/void survived only because their build contexts still
contain one). The prerequisites are now gated on the EXPANDED link
inputs (deferred so they judge the caller's final values), and the
isolation proof grew the pristine-tree leg — the package copied
ALONE into a scratch directory, the containers' exact condition —
that would have caught it before CI did.

KNOWN FLAKE (once, not reproduced in 5 runs including under load):
fdk-shell's passive-grab test asserted `presses >= 1` zero-times
during a rig-teardown-heavy window — watched for in CI, no code
path connects it to this round.

### Fixed — the live-desktop bar findings (2026-10-26; sway/labwc + Openbox)

Four root causes found by running the fdk-shell demo bar on real
desktops, each reproduced in this environment first (headless sway
AND labwc 0.8.3 for the Wayland legs; a real Openbox 3.6.1 under
Xvfb for the X11 legs) and each pinned by a permanent test:

1. **Wayland: the layer surface's startup sequence** — the bar
   painted immediately after `show()`, so FDK's present attached and
   committed a buffer BEFORE the layer surface's first configure,
   and wlroots killed the whole connection
   (`zwlr_layer_surface_v1: error 2: layer_surface has never been
   configured`). Extension surfaces now ride the same
   first-configure deferral xdg windows always had: presents before
   the role's configure are deferred, and
   `fdk_ext_surface_notify_configure()` (the add-on's post-ack
   report) commits the deferred first frame — the protocol rule is
   enforced by FDK, not left to each add-on. Pinned by the
   eager-paint test, the bar rig's WAYLAND_DEBUG trace assertions,
   and alive/pixels legs against both compositors.

2. **The fatal-dispatch busy-loop** — after that connection died,
   the app-owned pump loop spun at 100% CPU logging the same
   "backend dispatch_pending failed" line ~2.2 million times in 3
   seconds (the backend's circuit breaker logged once; the core's
   own error report did not). The core now reports a fatal dispatch
   ONCE per code, and identical repeats sleep out the caller's
   remaining timeout before returning the error — an ignoring loop
   paces itself instead of spinning. The demo bar now exits
   non-zero on a fatal pump. Pinned by the bar rig's
   compositor-kill leg (self-exit within 5s, non-zero, one line,
   bounded log). The one-time Wayland death line now carries the
   protocol error's interface, object id and code from libwayland.

3. **X11: popup keyboard grabs vs the WM's passive grabs** — an
   active XGrabKeyboard deactivates passive grabs, so Openbox's
   Super+number workspace switching silently stopped working while a
   dropdown was open (recovered when it closed). Popups now take a
   POINTER-ONLY grab and route keys through focus (XSetInputFocus,
   saved and restored, error-swallowed); grab result codes are
   handled explicitly (GrabFrozen/AlreadyGrabbed retried once,
   NotViewable reported); every dismissal path stamps its reason; a
   dispatch-tail watchdog releases any grab whose popup is no longer
   viewable; and a _NET_CURRENT_DESKTOP switch dismisses open
   popups. The GPU self-check's retry budget moved from inline
   XSync+sleep rounds (~20 ms of blocked loop while a grab was
   held) to timer-based attempts fired from the dispatch tail (the
   pump wakes for them through the backend's new
   `next_wakeup_ms`). Pinned by the in-test EWMH WM's passive-grab
   rig (XTest-synthesized Super+1 received by the WM with the popup
   open and after dismissal), the popup pixel-stays test
   (root-region readback), and the desktop-change dismissal test.

4. **X11: the layer's self-positioning** — a dock-typed window with
   no position hints is "unplaced", and Openbox ran its placement
   policy: the panel centered mid-screen (confirmed in its debug
   log). fdk-shell now sets USPosition/USSize WM_NORMAL_HINTS with
   the anchoring math; popups raise on map (an override-redirect
   window created under a raised window stays under it otherwise).
   Pinned by reading the hints back server-side.

`FDK_POPUP_TRACE=1` documents the popup lifecycle for real-desktop
diagnosis: create/map/raise, grab verdicts + focus takeover, first
present (with the lane), unmap/destroy, and the REASON each release
happened with the grab-held duration.

### Fixed — the fdk-shell packaging matrix's first live CI pass

Six defect classes, all packaging/build-environment script defects
(none in library code), all invisible to the sandbox battery — three
of the first four because the sandbox cannot run the native packagers
(no rpmbuild/abuild/xbps-create/pacman, uid 1001, no docker), the
fourth because the local deb verification extracts packages instead
of installing them; the fifth and sixth fell the same way (the .pc
defect needed a clean-container consumer, the XTEST header a builder
without it — this sandbox has carried the Xtst dev package all
along):

1. **The deb/rpm install-test smoke needed X11 dev headers as TEST
   INFRASTRUCTURE** (it reads server-side truth through a second
   Xlib connection; the .pc chain deliberately carries no `x11`
   Requires). The clean install containers now pre-install
   `libx11-dev` / `libX11-devel` — the same class as their
   gcc/pkg-config/xvfb, and what the alpine/void/arch families
   already declared. CI's own logs proved the fix sufficient: the
   identical smoke passed inside the deb build container, where the
   headers existed.
2. **FDK's own rpm install-test selected `dist/*.rpm`**, sweeping
   the add-on's packages into a test whose removal step then got
   (correctly) refused — a working dependency blocking removal of
   its base. It now selects FDK's own rpms by name; arch's
   equivalent `head -1` sort-order landmine is now a deterministic
   `fdk-[0-9]*` glob.
3. **The fdk-shell apk build hand-fed a bare sha512 into the
   APKBUILD** — abuild's checksum array must carry the FILENAME
   (`abuild checksum` writes the correct form; FDK's own apk
   pipeline already used exactly that sequence). The rewrite also
   fixed the artifact collection (abuild writes to REPODEST, not
   the build dir) and dropped the `abuild -r -F` cargo cult.
4. **The fdk-shell xbps build checked `dist/` while xbps-create
   wrote into the current directory** — both packages had built
   successfully; the collection line FDK's own void build carries
   (`mv ./*.xbps dist/`) was missing.

5. **The generated fdk-shell.pc carried the library's platform link
   deps in Libs** (`pkg-config --libs x11 wayland-client` appended —
   every consumer forced to install the X11 AND Wayland dev
   packages; the clean-container smokes died at
   `ld: cannot find -lwayland-client`; build-machine -L paths baked
   into the artifact) **and never defined its own variables**
   (`${libdir}`/`${includedir}` undefined — empty expansion that
   only worked because /usr/include and the default linker path
   need no flags). The .pc now matches fdk.pc's contract: variables
   defined, `Libs: -lfdk-shell` + `Requires: fdk`, platform flags
   in `Libs.private`. The smoke's own second Xlib connection now
   carries `-lX11` on its link line (modern ld refuses to resolve a
   binary's own symbols through a shared library's transitive
   NEEDED — "DSO missing from command line"), and pkg-config
   discovery failures print the real diagnostic instead of
   swallowing it.

The same run closed void: the corrected .pc made the build-phase
smoke discover and link the installed fdk-shell (the old
"cannot find" never recurred to fire its new diagnostic), and the
remaining void failure was the install-test's missing runtime
shlibs — xbps resolves a package's declared shlib-requires only
against INSTALLED providers, so the combined-set transaction
aborted on fdk-devel's libXext.so.6/libxkbcommon.so.0; the
install-test now pre-installs the same runtime shlibs FDK's own
void install-test carries.

Also: the fdk-shell distro build scripts restored the explaining
comment for FDK's `LSAN_OPTIONS=exitcode=0` distro-container leak
policy (the copy had dropped it), the inert copies of that flag
where the sanitizer toolchain is off (alpine/void, musl/minimal)
were removed, and the el10-leg Xvfb gating in the rpm/alpine/void
install-tests now follows the helper's disclosed-skip contract
(`if fdk_start_xvfb; then … else disclose`) instead of hard-failing
on images with no Xorg server.

A fourth push (the live-desktop bar fixes, 2026-10-26) added the
sixth class: **the XTEST dev header is test infrastructure too — for
the BUILDERS this time.** fdk-shell's new passive-grab passthrough
test synthesizes Super+number keys through the XTEST extension, and
five of six CI jobs died at the same line — `tests/test_x11.c:
fatal error: X11/extensions/XTest.h: No such file or directory` —
because no builder pre-installed the Xtst dev package (`libxtst-dev`
deb/apk, `libXtst-devel` rpm/void, `libxtst` arch). The rpm job
"passed" only through its disclosed el10 Xvfb skip: the suite never
compiled where the header was missing. All six install lists (CI's
ubuntu apt + each family's build container) now declare it. The
dependency is honest, not incidental: the core suite dodged XTEST
for years with XWarpPointer (pointer placement) and XSendEvent
(delivery), but a passive grab activates only on synthesized input
— a KEY event — which XSendEvent's `send_event` delivery can never
trigger; the test that proves the WM's Super bindings survive an
open popup has no cheaper input channel. The same run's void job
died one step earlier — a reposync connection timeout on the
round-robin mirror (a mirror outage, not a code defect); the
critical xbps line now retries once, the same tolerance the later
lines always carried.

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
