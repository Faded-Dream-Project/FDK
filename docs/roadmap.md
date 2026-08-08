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

## Phase 2 — Platform Layer (next)

- `src/platform/x11/` and `src/platform/wayland/` backends
- Runtime backend selection (`FDK_PLATFORM_AUTO` picks based on
  `$WAYLAND_DISPLAY` reachability, per `fdk_core.h`'s documented
  contract)
- Window abstraction: top-level window creation/show/destroy, resize,
  move, close handling
- Real event loop in `fdk_run()`: poll/dispatch over the platform
  connection's fd, wired to the timer/idle-callback queue
  (`docs/threading.md`'s `fdk_invoke_on_ui_thread` lands here too)
- `fdk_init()` gains real `FDK_ERR_NO_DISPLAY` / `FDK_ERR_PLATFORM_INIT`
  behavior (the error codes already exist in `fdk_error.h`; only the
  code path that can actually produce them is missing)

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
