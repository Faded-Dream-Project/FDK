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
- `src/platform/wayland/` backend (optional at build time — see
  `docs/build.md`'s "Optional Wayland build"): connection + registry
  global discovery (compositor, shm, seat, xdg_wm_base),
  `xdg-shell` toplevel lifecycle (surface → xdg_surface →
  xdg_toplevel, configure/ack/commit handshake, close handling), seat
  capability binding, keyboard via `libxkbcommon` (compositor-supplied
  keymap compilation, modifier state tracking), pointer (motion/
  enter/leave/button/axis), correct external-event-loop integration
  (`prepare_read`/`poll`/`read_events`/`dispatch_pending`, not the
  simpler self-blocking `wl_display_dispatch()`)
- xdg-shell protocol bindings generated via `wayland-scanner` from the
  real upstream `wayland-protocols` XML (vendored with attribution in
  `third_party/wayland-protocols/`) — not hand-rolled protocol
  behavior
- `fdk_platform_ops` internal vtable: the seam that keeps every other
  layer of FDK backend-agnostic (see `docs/architecture.md`)
- Real event loop in `fdk_run()`: `poll()`-based, blocks efficiently
  (no busy-spinning) on the platform connection's fd, exits when
  `fdk_quit()` is called or the last window closes, per its documented
  contract in `fdk_core.h`. The dispatch glue (`context_dispatch_event`
  in `src/core/context.c`) resolves the backend's opaque
  `fdk_platform_window *` back to the owning `fdk_window *` via the
  context's window registry, then calls `fdk_window_dispatch_event()`
  which caches configure sizes and invokes the application's
  registered callback — this is what closes the loop from the backend
  vtable all the way back to the public API.
- `fdk_init()` now performs real backend auto-detection
  (`FDK_PLATFORM_AUTO`: Wayland if `$WAYLAND_DISPLAY` is set and
  reachable, else X11) and produces real `FDK_ERR_NO_DISPLAY` /
  `FDK_ERR_PLATFORM_INIT` results. Each explicit backend
  (`FDK_PLATFORM_X11` / `FDK_PLATFORM_WAYLAND`) is tried without
  silent fallback to the other (per `fdk_core.h`'s documented
  contract and the `test_platform_no_display.c` assertion).
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
- **Wayland windows show a solid background buffer, not rendered
  content.** The Wayland backend commits a white wl_shm buffer when
  the window is shown (and a fresh one on resize) — the functional
  equivalent of X11's background pixel, without which a Wayland
  window would be invisible (compositors map nothing until a buffer
  is committed). `fdk_window_hide()` unmaps via a NULL-buffer commit,
  and `fdk_window_resize()` commits a buffer at the new size — both
  previously documented no-ops, now real. Full rendering remains
  Phase 3.
- **No rendering.** A created window has no drawn content — Phase 3.
- **No timers or idle callbacks**, and no `fdk_invoke_on_ui_thread()`
  (the worker-thread-to-UI-thread scheduling primitive sketched in
  `docs/threading.md`). Deferred; the event loop structure in
  `context.c` has an obvious place to add a timer queue when needed,
  but adding one before anything needs it would be speculative.

## Phase 3 — Rendering (second slice shipped; phase in progress)

Implemented and tested in the FIRST slice (the foundation):
- `fdk_surface` public API (`include/fdk/fdk_surface.h`): a window's
  software drawing target — framebuffer acquisition with live
  resize-following (`fdk_surface_get_info`, XRGB8888 pixels + stride),
  whole-window presentation (`fdk_surface_present`), and blending
  source-over fill primitives (`fill`, `fill_rect`, `draw_rect`,
  `fill_gradient_vertical`) operating on straight-alpha `fdk_color`
- Two new optional `fdk_platform_ops` entries (`window_get_framebuffer`,
  `window_present`) keep the render layer backend-agnostic; a backend
  that can't provide a software framebuffer leaves them NULL and the
  API reports `FDK_ERR_UNSUPPORTED` instead of pretending
- X11 implementation (`src/platform/x11/x11_surface.c`): ZPixmap
  `XImage` at the window's live size, `XPutImage` + flush on present;
  standard 24-bit TrueColor layouts only (checked, `FDK_ERR_UNSUPPORTED`
  otherwise — exotic visuals are refused, not mangled)
- Wayland implementation (`src/platform/wayland/wayland_window.c`):
  one fresh wl_shm XRGB8888 buffer per presented frame, tracked in a
  fixed ring of in-flight slots until `wl_buffer::release`, reusing
  the buffer lifecycle verified against weston in Phase 2; the old
  solid-white background path still covers windows nobody renders
  into, and is bypassed (no white flash) the moment an app presents
- `fdk_pump_events()` (`fdk_core.h`): the application-owned event
  loop primitive rendered apps are built on (fdk_run() blocks between
  input events — nowhere to draw); `fdk_run()` is now a thin wrapper
  over it. Includes the client-side-buffered-events drain fix found
  live during the render demo (Xlib slurps socket data on every flush,
  so events can be in Xlib's queue while poll() sees an empty socket)
- `FDK_EVENT_WINDOW_EXPOSE` (`fdk_event.h`): X11 Expose translation
  so rendered apps repaint covered regions; Wayland never needs it
  (compositors retain the last committed buffer)
- `02_software_render` example: animated gradient + bouncing
  antialiased ball (raw pixel writes) + block-letter logo (fill
  primitives), ~60 fps pump loop, ESC/close exit, verified end-to-end
  on Xvfb (screenshots + H.264 capture + pixel checks) and on weston
  headless (screenshots + pixel checks + WAYLAND_DEBUG protocol
  counts: per-frame pool/buffer/attach/commit/release)
- X11 renderer integration tests: server-side pixel readback via
  XGetImage over a SECOND X connection (fill/rect/border/direct-write
  all verified at exact packed-pixel values), and a resize test that
  confirms the new framebuffer at the new geometry

Explicitly NOT done in the first slice — of that list, the SECOND
slice (this commit series) has now closed the four big ones below;
see `docs/rendering.md` for the full design:

Second slice — damage tracking, clip stack, offscreen surfaces,
full primitive set, and frame pacing (all tested headless + on both
backends):
- **Damage tracking**: every drawing helper records what it touched;
  `present()` sends only the damaged region (X11: per-rect sub-image
  `XPutImage`, with a >=75%-damaged whole-image fallback; Wayland:
  per-rect `wl_surface.damage` hints) and is a TRUE no-op when
  nothing changed — verified end-to-end on X11 by writing a raw
  pixel WITHOUT invalidating and proving the server never receives
  it (see test_x11_integration.c). Raw-pointer writers declare
  damage with `fdk_surface_invalidate()`. Bounded bookkeeping: 64
  rects, overflow degrades to full damage. `02_software_render`
  phase 2 runs at 1-2% of window damage per frame.
- **Wayland buffer recycling + prefetch**: released wl_shm buffers
  stay alive in their slots and are reused; every acquired buffer is
  pre-filled with a copy of the visible frame, which is what makes
  partial damage CORRECT (not just fast) — compositors may ignore
  damage hints and scan out the whole buffer, and every pixel
  outside the damage region genuinely matches the screen. Verified:
  3 shm pools created for 173 commits (was ~1 pool per frame).
- **Frame pacing on Wayland**: every present requests a
  4-wayland-callback; `fdk_surface_frame_ready()` gates the next
  frame on the compositor's acknowledgment (arrives while pumping
  events), with a 250 ms starvation guard because hidden surfaces
  legitimately never receive frame callbacks. X11/offscreen: always
  ready (no core-protocol feedback). Verified: 171 frame requests,
  170 callbacks delivered, commits spaced at the compositor's clock.
- **Clip stack**: `fdk_surface_push_clip`/`pop_clip`/`get_clip` —
  intersecting, LIFO, depth-bounded (32), enforced by every
  primitive and by blit; empty intersections make drawing a safe
  no-op. This is the widget layer's parent-constraint primitive.
- **Offscreen surfaces**: `fdk_surface_create`/`destroy` —
  windowless application-owned drawing targets with deliberately
  padded stride (16-px multiples) so stride-aware paths are always
  exercised; they are also what makes the whole renderer testable
  with no display (tests/test_render.c, 13 cases in `make test`).
- **Primitive set completed (for now)**: `draw_line` (Bresenham),
  `draw_circle`/`fill_circle` (midpoint/scanline),
  `fill_rounded_rect`/`draw_rounded_rect` (radius-clamped, arcs
  plotted once — no double-blended pixels), `blit`
  (surface-to-surface opaque copy, clipped both sides, damages the
  destination). `draw_rect` corners now blend exactly once (was a
  subtle double-blend with translucent colors).

Still NOT done (next structural gaps, in rough order):
- **No MIT-SHM fast path on X11** — XPutImage over a local socket is
  a memcpy per frame (now per-damage-rect); the shared-memory path
  slots invisibly inside `x11_surface.c` when needed (and is
  required anyway for remote X)
- **No double-buffer presentation on X11** — the single XImage is
  both draw target and blit source; correct (XPutImage copies into
  the request stream) but a resize-acquire can race an in-flight
  blit on a real desktop. Frame-callback PACING is now implemented
  on Wayland (see above); X11 has no core-protocol equivalent
- **Primitive set remains 2D-raster basics** — no transforms, image
  decode (PNG etc.), alpha-masked blits, or antialiased
  primitives yet; lines/circles/rounded rects are crisp 1px
  geometry, and the demo ball's antialiasing is application code
  (deliberately — it demonstrates the raw-pixel level)
- **Text integration** (`src/text/`) not started — the demo draws
  block letters as rects precisely because there is no font path yet
- **Single window scale handling** — buffer scale / fractional-scale
  (HiDPI) protocols not wired up; surface dimensions equal window
  dimensions in pixels, which is wrong under any non-1x scale factor
  and is Phase 3's next structural gap to close

## Phase 4 — Widget Foundation ✅ (this milestone)

The retained-mode widget object model everything later builds on.
Implemented and tested:

- `fdk_widget` public API (`include/fdk/fdk_widget.h`): opaque
  widget objects in parent/child trees; standalone roots work with
  no window at all, which is what makes the whole layer provable
  headless (`tests/test_widget.c`, 17 cases in `make test`)
- Hierarchy: create/destroy (recursive), z-order (child order;
  raise/lower), order-preserving reparent with cycle/root refusal,
  child iteration, tree depth capped at 256
- Geometry: parent-relative bounds, absolute (window) bounds
  composed up the chain, `set_bounds` invalidating BOTH the old and
  new regions
- State: per-widget visible/enabled flags with effective (ANDed up
  the chain) queries; hidden/disabled subtrees are input-TRANSPARENT
  (hit-testing passes through to what is underneath); hiding or
  disabling a focused/hovered widget cleanly drops focus/hover with
  the proper OUT/LEAVE events delivered
- Event routing (`fdk_widget_tree_handle_event` — the same entry
  point the window glue uses): topmost-first hit-testing,
  widget-local coordinates, ENTER/LEAVE synthesis from motion,
  implicit pointer grab for press-to-release pairing (release
  delivered even off-widget), bubbling to ancestors until handled,
  scroll and key routing, per-widget event callbacks + a class
  event hook that both always see the event (return value only
  decides handled)
- Focus: one focused widget per tree, eligibility (visible +
  enabled + can-focus), FOCUS_IN/OUT delivery, focus drop on
  hide/disable, window blur/regain mirrored into the focused widget
  without consuming the window event, and built-in Tab / Shift+Tab
  traversal (depth-first order, wrapping, overridable by handling
  the key first, suppressed for modified Tabs)
- Invalidation & painting: tree-global damage bounding box;
  `fdk_widget_tree_paint` repaints in z-order constrained by the
  Phase 3 clip stack (damage ∩ widget ∩ ancestors) — a partial
  repaint is provable: raw marker pixels outside the damage box
  SURVIVE a repaint (asserted headless); empty-damage paint is a
  true no-op
- Window glue (`fdk_window_get_root`, `fdk_window_paint`): lazily
  created root kept in sync with every configure (resize = full
  repaint), EXPOSE invalidates the tree, input events routed into
  the tree BEFORE the application callback (widget-consumed events
  never reach it — documented contract), and a pump loop of
  `fdk_pump_events` + `fdk_window_paint` is a complete widget
  application
- Reentrancy (the part that makes the rest trustworthy): a callback
  may destroy ITSELF, its ancestors, or the whole window/root — the
  core unlinks immediately, defers the frees until the dispatch/
  paint walk unwinds, and the window glue re-verifies registration
  after every tree callback. Exercised headless (ASan-clean) and
  live (the demo's quit button destroys the window from inside its
  own release handler)
- Subclassing vtable (`fdk_widget_class`: event/paint/measure/
  arrange/destroy hooks) — the pattern FDK's own widget families use
  (embed fdk_widget as first member via `src/widget/
  widget_internal.h`); `measure`/`arrange` hook contracts are
  settled now so the Phase 5 layout engine lands without touching
  the object model. APPLICATION-level subclassing is not possible
  yet (the public type is opaque per docs/abi-policy.md — apps use
  callbacks, user data, and the base style setters); revisit at ABI
  freeze
- Base style seed: background fill + corner radius on the base
  widget (the Phase 7 theme system replaces these fields)
- Named scancodes (`FDK_KEY_TAB` etc.) in `fdk_event.h`; X11
  EnterNotify/LeaveNotify now carry positions (was uninitialized)
- Tests: 17 headless cases (`make test`, ASan+UBSan: lifecycle,
  hierarchy, reparent, z-order/clip/hide painting, partial-repaint
  proof, hover/grab/bubbling, focus lifecycle, Tab traversal,
  destroy-during-dispatch, measure/arrange, tree isolation) plus 3
  X11 GUI integration cases (`make test-x11`: tree painting verified
  by server-side pixel readback; REAL input via XSendEvent — hover,
  grab, consume contract, Tab, focus mirror; root-follows-resize
  with the fresh area painted by the tree)
- `examples/03_widgets.c`: interactive panel/button/meter UI — hover
  highlight, pressed state, focus tint, live meter recolor + resize,
  quit-button destroy-from-callback — driven by real injected input
  in the test rig with pixel-verified before/after frames (see
  README "What it looks like")

Explicitly NOT done in Phase 4 (do not mistake for oversights):
- **No layout engine.** Geometry is set by hand; the `measure`/
  `arrange` hooks exist and are tested but nothing drives them yet —
  that IS Phase 5.
- **No widget catalog.** Label/Button/Entry etc. are Phase 6; the
  demo builds "buttons" from base widgets + callbacks precisely to
  prove the foundation needs nothing more.
- **No text rendering** — still Phase 3's remaining gap (src/text/);
  the widget layer is deliberately text-free until then.
- **No Wayland-side widget GUI test this slice** — the Wayland
  toolchain (libwayland-dev) is absent in this environment, so the
  widget layer was verified on X11 + headless only. The layer is
  backend-neutral (it sits on fdk_surface/fdk_event), and the
  Wayland GUI widget tests slot in when the toolchain is next
  available — recorded as a gap, not assumed.
- **Application-level subclassing** blocked on the opaque public
  type (ABI policy) — internal pattern only, documented in
  `fdk_widget.h`.

## Phase 5 — Layout Engine (first slice shipped; phase in progress)

Implemented and tested in the FIRST slice (the box):
- `fdk_layout` public API (`include/fdk/fdk_layout.h`): the BOX
  container (horizontal/vertical) with spacing, padding, and
  homogeneous mode, built strictly on the Phase 4 measure/arrange
  hooks — a container is just a widget subclass whose hooks implement
  a layout policy; nothing else in the object model changed
- Per-child layout hints carried on the child: margins (per side),
  expand (along + cross axis), align (FILL/START/CENTER/END for the
  non-expanded cross axis) — all clamped, all relayouting the parent
  container immediately
- Natural size as a REQUEST: the default measure hook now reports the
  widget's create-time bounds or `fdk_widget_set_natural_size` —
  deliberately independent of the CURRENT bounds, so layout can never
  destroy a child's size request (the classic request/allocate split;
  this also fixed the first real layout-engine bug the tests caught:
  relayout-on-child-add zeroing children before their first measure)
- Along-axis distribution: naturals packed, expanding children split
  the leftover (integer-safe via accumulated positions); homogeneous
  gives equal shares. Cross axis: expand fills, else natural placed
  per align, clamped to available space. Hidden children take no slot
- Relayout triggers: the arrange hook (bounds assigned), child
  add/remove/reparent, hint and natural-size changes — all through
  one pure-geometry pass (no user code runs during layout)
- Window integration: `fdk_window_set_content` — the window's content
  widget auto-arranges to the root's full bounds on set and on EVERY
  configure (weak reference: destroying the content just deactivates
  the association, validated at each use)
- Tests: 8 headless cases (`tests/test_layout.c` in `make test`:
  measure math, arrangement math, margins-in-slots, homogeneous,
  dynamic add/remove/hide/hint relayout, nested boxes, paint-into-
  slots pixel agreement, argument safety) + the X11 GUI reflow case
  (`make test-x11`: set_content, resize, server-side readback of the
  reflowed boundary at exact pixels, destroyed-content deactivation)
- `examples/04_layout.c`: box-built UI, breathing window + animated
  size request, steady hold phases for the captured proof frames

Still NOT done (next slices of Phase 5):
- **Grid container** — the other classic; the box proves the pattern
- **Min/max size constraints** on widgets (clamps at measure time)
- **Baseline alignment** for the day text exists (Phase 6 dependency)
- **Wayland-side reflow test** — same toolchain gap as Phase 4;
  the engine is backend-neutral and verified headless + on X11

## Phase 6 — Core Widgets + Text Foundation (first slice shipped; phase in progress)

**Shipped — text foundation** (`include/fdk/fdk_text.h`, `src/text/`,
docs in `docs/text.md`):
- Font object per (file, pixel size): load with sfnt container
  validation (magic, directory, table extents — stb assumes a trusted
  font, so FDK gates it), TTC first-face support, hhea metrics
- UTF-8 single-line shaping with pair kerning, strict-ish decode
  (U+FFFD per bad byte), .notdef fallback, measure/draw agreement by
  construction (one shared shaping walk)
- Glyph cache: 512 entries, LRU eviction, hit/miss/eviction stats;
  rasterization + parsing by vendored stb_truetype v1.26
  (`third_party/stb/`, FDK's first third-party component — dual
  MIT/public-domain, see `THIRD-PARTY-NOTICES.md`)
- Rendering through `fdk_surface_blend_mask()` (the new internal
  alpha-mask primitive): clip-stack honored, one damage rect per run
  (ink union ∩ effective clip), integer source-over blending
- Tests: 7 headless cases + X11 server-readback glyph verification;
  `examples/05_text.c` (wordmark, size ladder, measured-run chaining,
  per-glyph wave, live cache stats) with a captured proof frame

**Still to build (rest of Phase 6):**
- Label, Button, Toggle, Checkbox, Radio, ProgressBar, Separator,
  Frame/Panel — as internal subclasses of fdk_widget consuming
  `fdk_text.h`, with focus visuals, keyboard activation
  (Space/Enter), and disabled states
- Widget-level text layout helpers: line breaking, ellipsize,
  alignment — on top of the run-level API that just landed
- Subpixel glyph positioning; bold/italic faces beyond file choice

Phase 3's original "src/text/ gap" is closed; remaining Phase 3
rendering gaps (MIT-SHM, X11 double buffering, transforms, image
decode, alpha blits, AA primitives, HiDPI) stay parked as recorded —
none block the widget catalog.

## Phase 7 — Theme Engine

- `.fdk` format: grammar spec (`docs/fdk-theme-format.md`, written
  when this phase starts), parser, validator, loader, theme API,
  default theme replacing the Phase 4 base-style fields
- Parser treated as security-sensitive from day one — see
  `docs/security.md`

## Phase 8 — Window Decorations

- FDK-owned title bars, close/maximize/minimize buttons, resize
  handling, decoration theming, correct per-backend protocol usage
  (Wayland xdg-decoration / compositor-specific fallback vs. X11
  atoms/MWM hints — these are NOT identical and Phase 8 documents
  the difference rather than assuming CSD parity)

## Phase 9 — Advanced Widgets

- ScrollView, List, Tree, ComboBox, Menu, Toolbar, Slider,
  SpinButton, Notebook/TabView, Dialog, Canvas — plus text Entry
  input (cursor, selection, clipboard architecture, IME groundwork)

## Phase 10 — Accessibility / Internationalization

- Accessibility abstraction (roles, names, states, relationships,
  keyboard navigation) — implemented as far as practical without
  requiring a specific platform AT-SPI-equivalent dependency; gaps
  documented rather than faked
- i18n: locale-aware formatting, translation infrastructure,
  pluralization architecture

## Phase 11 — Stabilization

- ABI freeze (`FDK_ABI_STABLE` → 1, see `docs/abi-policy.md`; this
  is also where application-embeddable widget subclassing is
  revisited)
- API cleanup pass, performance profiling (not before this — see
  project principle against premature optimization), memory-safety
  audit, full documentation pass, packaging
