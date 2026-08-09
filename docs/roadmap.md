# FDK Development Roadmap

Work proceeds in phases. Each phase should compile, pass its tests,
and leave the tree in a working state before the next begins — no
phase depends on a later phase's code existing yet, though later
phases' *plans* are sometimes referenced in earlier docs/comments so
the earlier API doesn't need to change shape once the later phase
lands (see e.g. `fdk_init_options` in `fdk_core.h`, already shaped for
the platform-connection error paths Phase 2 will add).

## Phase 0 — Repository Audit ✅ (this milestone)

Repository inspected. Prior state: a `Legacy FDK/` folder and
`.gitignore` at root; language breakdown previously C/CMake/Shell.
Per project decision, the legacy folder's contents were not carried
forward — Phase 1 started from the specification fresh rather than
auditing and salvaging prior code. If the legacy folder is still
present in the repository, it should be reviewed and then removed (or
explicitly archived under a clearly-labeled path) so it doesn't get
mistaken for current source.

## Phase 1 — Foundation ✅ (this milestone)

Implemented and tested:
- Directory structure (`include/fdk/`, `src/<module>/`, `tests/`,
  `examples/`, `docs/`, `themes/`, `tools/`)
- Make-based build system: debug (default, ASan+UBSan) and release
  configs; static (`libfdk.a`) and shared (`libfdk.so`) library
  targets; `make test`, `make examples`, `make install`/`uninstall`
- Core types (`fdk_types.h`): geometry, color, fixed-width ints,
  opaque object handles
- Error handling (`fdk_error.h`): `fdk_result` enum, no exceptions/no
  global errno-style state
- Logging (`fdk_log.h` + internal macros): leveled, pluggable sink
- Internal allocation helpers with OOM handling and overflow-checked
  array allocation
- Context lifecycle (`fdk_core.h`): `fdk_init`/`fdk_run`/`fdk_quit`/
  `fdk_shutdown`
- Versioning (`fdk_version.h`)
- Test suite: 15 tests across lifecycle and allocation, passing clean
  under AddressSanitizer + UndefinedBehaviorSanitizer
- `01_hello_world` example, actually builds and runs
- LICENSE (proprietary draft, flagged for legal review),
  `docs/dependencies.md`, `docs/licensing-policy.md`,
  `docs/abi-policy.md`, `docs/memory.md`, `docs/threading.md`,
  `docs/architecture.md`

Explicitly NOT done in Phase 1 (do not mistake for oversights):
- No platform/window-system connection — `fdk_run()` returns
  immediately with a logged warning rather than pretending to have an
  event loop
- No rendering, no widgets, no theme parser
- No X11 or Wayland code at all yet

## Phase 2 — Platform Layer ✅ (this milestone)

Implemented and tested:
- `src/platform/x11/` backend: connection (`XOpenDisplay`), screen/
  root window, ICCCM `WM_DELETE_WINDOW` registration, EWMH
  `_NET_WM_NAME` + ICCCM `XStoreName` title setting, window create/
  destroy/show/hide/resize/size-limits, event translation (configure,
  focus, keyboard via `XLookupString` for layout-aware codepoints,
  pointer motion/buttons/scroll, enter/leave)
- `src/platform/wayland/` backend: connection + registry global
  discovery (compositor, shm, seat, xdg_wm_base), `xdg-shell` toplevel
  lifecycle (surface → xdg_surface → xdg_toplevel, configure/ack/
  commit handshake, close handling), seat capability binding, keyboard
  via `libxkbcommon` (compositor-supplied keymap compilation, modifier
  state tracking), pointer (motion/enter/leave/button/axis), correct
  external-event-loop integration (`prepare_read`/`poll`/
  `read_events`/`dispatch_pending`, not the simpler self-blocking
  `wl_display_dispatch()`)
- xdg-shell protocol bindings generated via `wayland-scanner` from the
  real upstream `wayland-protocols` XML (vendored with attribution in
  `third_party/wayland-protocols/`) — not hand-rolled protocol
  behavior
- `fdk_platform_ops` internal vtable: the seam that keeps every other
  layer of FDK backend-agnostic (see `docs/architecture.md`)
- Real event loop in `fdk_run()`: `poll()`-based, blocks efficiently
  (no busy-spinning) on the platform connection's fd, exits when
  `fdk_quit()` is called or the last window closes, per its documented
  contract in `fdk_core.h`
- `fdk_init()` now performs real backend auto-detection
  (`FDK_PLATFORM_AUTO`: Wayland if `$WAYLAND_DISPLAY` is set and
  reachable, else X11) and produces real `FDK_ERR_NO_DISPLAY` /
  `FDK_ERR_PLATFORM_INIT` results
- Public `fdk_window.h` and `fdk_event.h` APIs: window lifecycle,
  size/title/limits, and a backend-neutral event model (configure,
  close-request, focus, keyboard, pointer, scroll) — no Xlib or
  wayland-client type anywhere in a public header
- Two-tier test suite (see `docs/testing.md`): platform-independent
  tests run under plain `make test` (no display needed anywhere, even
  in CI); real X11 integration tests run under `make test-x11` against
  either an existing `$DISPLAY` or an auto-started throwaway Xvfb —
  genuinely verifies connect, window lifecycle, and a full
  resize → `FDK_EVENT_WINDOW_CONFIGURE` round-trip against a live X
  server, not just that the code compiles
- `01_hello_world` example rewritten to actually open a window, handle
  resize/close/keyboard events, and run — verified end-to-end against
  Xvfb

Explicitly NOT done in Phase 2 (do not mistake for oversights — see
`docs/testing.md` and inline doc comments for the specifics):
- **No Wayland integration test.** No suitable headless Wayland
  compositor was available/verified in this development environment
  to test against automatically the way Xvfb allows for X11. The
  Wayland backend compiles cleanly and its window/event logic mirrors
  the X11 backend's structure, but has only been manually
  reasoned-through and code-reviewed, not integration-tested against a
  live compositor. Recorded as a real gap, not silently worked around.
- **No X11 `WM_DELETE_WINDOW` automated test.** The registration and
  `ClientMessage` handling code is real and correct per the ICCCM
  protocol, but exercising it requires an actual window manager
  running to send the message — bare Xvfb has none. `make test-x11`
  says `[skip]` for this rather than silently omitting it.
- **No custom window decorations.** Both backends currently show
  whatever decorations the platform/compositor provides (X11 window
  manager decorations; Wayland server-side decorations where the
  compositor supports `xdg-decoration`, otherwise none) — FDK-drawn
  title bars are Phase 7, deliberately not pulled forward (per the
  directive's explicit instruction not to rush this).
- **`fdk_window_hide()` and `fdk_window_resize()` are no-ops on
  Wayland**, with a logged warning explaining why (both fundamentally
  need the renderer — attaching a buffer — which doesn't exist until
  Phase 3). Documented honestly in `wayland_window.c`'s doc comments
  and `fdk_window.h`, not silently broken.
- **No rendering.** A created window has no drawn content — Phase 3.
- **No timers or idle callbacks**, and no `fdk_invoke_on_ui_thread()`
  (the worker-thread-to-UI-thread scheduling primitive sketched in
  `docs/threading.md`). Deferred; the event loop structure in
  `context.c` has an obvious place to add a timer queue when needed,
  but adding one before anything needs it would be speculative.

## Phase 3 — Rendering

- Renderer abstraction behind `src/render/`, no widget code depends on
  a specific backend renderer
- Primitives: rects, rounded rects, lines, borders, fills, gradients,
  clipping, transforms, images, alpha compositing, high-DPI scaling
- Text integration begins here (`src/text/`) — font loading, glyph
  rasterization; see `docs/dependencies.md` for the anticipated
  minimal-rasterizer dependency

## Phase 4 — Widget Core

- Widget base type, parent/child hierarchy, event dispatch, focus,
  sizing negotiation
- Layout engine (`src/layout/`): horizontal/vertical/grid, min/natural/
  preferred size, margins, padding, alignment, expansion

## Phase 5 — Initial Widgets

- Window, Container, Box, Grid, Label, Button, Entry, Image, and
  enough else to make the "Basic Window" / "Widgets" / "Layout
  demonstration" examples real

## Phase 6 — Theme Engine

- `.fdk` format: grammar spec (`docs/fdk-theme-format.md`, written
  when this phase starts), parser, validator, loader, theme API,
  default theme
- Parser treated as security-sensitive from day one — see
  `docs/security.md`

## Phase 7 — Window Decorations

- FDK-owned title bars, close/maximize/minimize buttons, resize
  handling, decoration theming, correct per-backend protocol usage
  (Wayland xdg-decoration / compositor-specific fallback vs. X11
  atoms/MWM hints — these are NOT identical and Phase 7 documents the
  difference rather than assuming CSD parity)

## Phase 8 — Advanced Widgets

- ScrollView, List, Tree, ComboBox, Menu, Toolbar, ProgressBar,
  Slider, SpinButton, Notebook/TabView, Dialog, Canvas

## Phase 9 — Accessibility / Internationalization

- Accessibility abstraction (roles, names, states, relationships,
  keyboard navigation) — implemented as far as practical without
  requiring a specific platform AT-SPI-equivalent dependency; gaps
  documented rather than faked
- i18n: locale-aware formatting, translation infrastructure,
  pluralization architecture

## Phase 10 — Stabilization

- ABI freeze (`FDK_ABI_STABLE` → 1, see `docs/abi-policy.md`)
- API cleanup pass, performance profiling (not before this — see
  project principle against premature optimization), memory-safety
  audit, full documentation pass, packaging
