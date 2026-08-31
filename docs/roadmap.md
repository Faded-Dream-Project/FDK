# FDK Development Roadmap

**Versioning convention:** every version-like label in this file
(Phase numbers, and the `1.0.0` / `1.1.x` milestone labels) is an
**internal engineering milestone** — a history marker for when work
landed. None of them is the public version. The public version of
FDK is `0.0.1` by deliberate policy and stays there for the
foreseeable future; see `docs/versioning.md` before touching
`include/fdk/fdk_version.h`.

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

## Phase 3 — Rendering ✅ (completion slice: images, alpha compositing,
## transforms, antialiasing, MIT-SHM + double buffering, HiDPI)

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
- `02_rendering` example (merged rendering demo): animated gradient + bouncing
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
  rects, overflow degrades to full damage. The rendering demo
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

Completion slice (Phase 3 is now CLOSED — every item of the original
"Primitives: rects, rounded rects, lines, borders, fills, gradients,
clipping, transforms, images, alpha compositing, high-DPI scaling +
text" promise is implemented and tested):

- **Image decoding** (`fdk_surface_create_from_image`,
  src/render/surface_image.c): PNG/JPEG/BMP/PSD/TGA/GIF/HDR/PIC via
  the vendored stb_image v2.30 (FDK's second third-party component,
  same dual MIT/public-domain license as stb_truetype — provenance
  and update procedure in third_party/stb/README.md). Security
  discipline per docs/security.md: file stat'd before any decode
  (non-regular / >512 MiB refused), decoded dimensions re-validated
  against the surface bounds, nothing partial ever handed out, zero
  partial results.
- **ARGB8888 surfaces + per-pixel alpha compositing**:
  `fdk_surface_create_format` creates straight-alpha ARGB offscreen
  surfaces (fully transparent initial state); every drawing helper
  composites the alpha channel with the full straight source-over
  formula on ARGB destinations; `fdk_surface_blit_blend` is the
  per-pixel source-over blit (XRGB sources refused); opaque blits
  onto ARGB force alpha 255. Text rendering (blend_mask) composites
  into transparent sprites correctly too.
- **Transforms**: `fdk_matrix` (2x3 affine, translate/scale/rotate
  constructors, mul/invert) + `fdk_surface_blit_transformed` —
  inverse-mapped so translucent sources never double-blend, bilinear
  sampling (edge-clamped), with INTEGER-EXACT fast paths (identity,
  whole-pixel translation, integer scale-up = nearest-neighbor block
  copy — upscaling never blurs); degenerate matrices are documented
  no-ops. Tests caught one real bug here before it shipped
  (fdk_matrix_invert computed the inverse translation from the wrong
  matrix elements).
- **Antialiased primitives**: `draw_line_aa` (integer-endpoint Wu
  with per-column direct evaluation — no incremental drift),
  `draw_circle_aa` / `fill_circle_aa` (exact-distance coverage),
  `fill_rounded_rect_aa` (box-SDF coverage). Axis-aligned AA lines
  are byte-identical to crisp ones; coverage is bounded and monotone
  (all pinned by test).
- **MIT-SHM + double buffering on X11** (x11_surface.c): two pixel
  slots per window, swapped every present; SHM-backed when the
  server supports it (probed once per connection; FDK_NO_MIT_SHM=1
  opts out), with ShmCompletion tracked per segment so the server is
  never racing the app's redraw (the completion-event routing caught
  the classic shmseg-vs-shmid confusion — the fix took the flaky
  "overdue" warnings and an abort with it). Attach-then-IPC_RMID
  segments leak nothing on crash. The X11 integration test observes
  the two SysV segments while rendering and their release on
  destroy, and proves the double-buffer swap behaviorally (un-
  presented drawing never reaches the server).
- **HiDPI**: `fdk_window_get_scale()`; Wayland wires wl_output
  tracking + wl_surface enter/leave (output-derived integer max),
  wp_fractional_scale_manager_v1 + wp_viewporter for fractional
  factors (viewport source = logical x exact scale in 120ths), and
  `wl_surface.preferred_buffer_scale`/fractional preferred_scale
  events; buffers are PHYSICAL (logical x scale); damage converts to
  surface-local logical coords at present; the WIDGET layer stays
  logical and composites through an ARGB intermediate at scale > 1
  (scale 1 path byte-identical, pinned by the existing suites).
  X11 honestly reports 1.0 (no core-protocol scale). Verified
  end-to-end against sway at output scale 2: physical exactly 2x
  logical, content proportional, decoration band correct at any
  scale. The scaled test run also caught a REAL pre-existing leak
  (a pending frame callback's proxy overwritten by the next one) —
  fixed.
- **Demo + rig**: examples/10_images.c (four panels: decode+blend,
  2x/rotated/1.7x transforms, crisp-vs-AA, runtime ARGB sprite
  accumulation) + scripts/run_images_demo_x11.sh — 13 PIL checks,
  ALL PASS.
- Tests added: 7 headless render groups (ARGB blending + accumulation
  math, blit_blend incl. clip conformance, image decode failure codes
  + known pixels + compositing, matrix algebra, transformed blit
  exactness/bilinear/degenerate/rotation, transformed alpha, AA
  primitives incl. axis-exactness) + X11 MIT-SHM/double-buffer case +
  Wayland HiDPI case (scale 1 and 2 runs) — headless suite 91 checks,
  X11 26, all demo rigs PASS.

Remaining (parked, honest — none are Phase 3's promised scope):
premultiplied-alpha fast paths and subpixel-RGB text rasterization
(Phase 11 performance territory); a compositor-config with a true
fractional output scale for the rig (the fractional code paths are
built and bound; sway's headless output here runs integer 2x).

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

## Phase 5 — Layout Engine — COMPLETE

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

Completion slice (grid, constraints, baseline, Wayland reflow):
- **Grid container** (`src/layout/grid.c`, the Phase 5 API in
  `fdk_layout.h`): children attach at (column, row) with colspan/
  rowspan; the grid grows to contain any attachment. Track sizes are
  the maximum natural of their children (a multi-span child
  distributes any deficit it introduces equally over its span);
  `fdk_grid_set_column_expand` / `_row_expand` mark tracks that share
  the container's EXTRA space; homogeneous mode equalizes tracks per
  axis; spacing/padding match the box's semantics. Same per-child
  hints as the box (margins, align, expand), and the same
  pure-geometry relayout triggers — attach, child-change
  propagation, hint changes. Re-attaching moves the child.
- **Min/max size constraints**: `fdk_widget_set_size_limits` clamps
  into EVERY measure result (0 = unconstrained; max < min is
  normalized, min wins), so any container — box, grid, or a future
  one — negotiates within the limits without knowing about them.
  Changing limits relayouts the parent immediately, like the other
  hint setters.
- **Baseline alignment**: `FDK_ALIGN_BASELINE` on the box's cross
  axis (and grid cells' vertical align). Text-bearing widgets report
  their baseline (`fdk_widget_get_baseline`; `fdk__widget_set_
  baseline` is the internal setter controls use — the Label reports
  its font's ascent); widgets without one use their BOTTOM edge, so
  a mixed row still lines up. The container's baseline is the MAX of
  its children's, the group offset per child from that max.
- **Wayland-side reflow test**: the toolchain gap closed with the
  Phase 8 sway rig — `test_wayland_integration.c` now drives a real
  client-side resize through the compositor and verifies the
  re-arranged tree in the framebuffer (expanding body stretches,
  footer moves to the new bottom).
- Tests: +4 headless layout cases (grid measure/arrange with spans,
  growth, expand, homogeneous, hidden, cell align; grid notifier
  regression; size limits through a real container; baseline with
  bottom-edge fallback and label ascent) and +2 X11 GUI cases (grid
  through window resize with server-side pixel readback of tracks/
  gaps/limits; baseline alignment of 16px + 32px labels sharing one
  baseline row) — and the Wayland reflow case above.
- `examples/04_layout.c`: the demo's main panel now carries a 3x2
  GRID with a two-column spanning cell and an expanding last column
  (LAYOUT DEMO rig extended: fixed track widths, exact 8px gaps,
  the span, and the expand column absorbing the window's growth are
  all PIL-verified across the two hold phases).

Two REAL engine bugs the completion slice found and fixed (both
caught by tests/demos that finally exercised the paths):
1. **Wayland client-side resize deadlocked.** `fdk_window_resize()`
   on a surface-rendered window recorded the new size platform-side
   but never told the frontend: the widget tree's own damage
   tracking said "clean", no compositor configure ever arrived
   (the compositor only reacts to a commit), so nothing repainted,
   nothing committed, and the window stayed at its old size forever.
   The Wayland resize now synthesizes the FDK_EVENT_WINDOW_CONFIGURE
   the frontend would have gotten from a compositor (optimistic in
   the same sense the request itself is — a disagreeing compositor
   corrects it with a real configure).
2. **A framebuffer acquired between presents pinned the OLD size.**
   The surface layer caches its acquired framebuffer until present;
   a `get_info` between presents re-acquired at the pre-resize size,
   and the post-resize paint then drew into that stale buffer
   (clipped) and committed the OLD size — on BOTH backends; X11's
   tests just never hit the ordering. CONFIGURE handling now drops
   the cached framebuffer (`fdk__surface_drop_framebuffer`) so the
   next acquire re-fetches at the current size.

Also fixed en route (found by the 04/05 demo rigs while validating
this slice): X11 double-buffer SLOT SYNCHRONIZATION — frame N+1's
partial damage shipped rects from a slot whose contents were two
presents old (the breathing meter flickered whole storm-era frames
between correct ones); the presented region is now copied front ->
back after every present. And `fdk_pump_events`' timeout contract
is now enforced against a monotonic deadline — X11's per-present
ShmCompletion events were waking the poll early, pacing a draw-
per-frame loop at 252fps instead of the requested 15ms.

## Phase 6 — Core Widgets + Text Foundation — COMPLETE

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

**Shipped — core widget catalog** (`include/fdk/fdk_widgets.h`,
`src/widget/controls.c` + `statics.c`): Label, Button, Toggle,
Checkbox, RadioButton (group = parent widget), ProgressBar,
Separator, and Frame (a titled vertical box — children arrange below
the title band via a new `title_inset` in the box packing). All are
widget subclasses on the Phase 4 vtable with measure hooks sizing
them from measured text; interaction is press-then-release-inside
(the Phase 4 implicit grab) plus Space/Enter on focus; disabled
widgets are input-transparent and dimmed. Verified by 8 headless
catalog cases, an X11 GUI case driving REAL clicks through the
server, and `examples/06_widgets.c` (CATALOG DEMO PASS, 12 PIL
checks, captured proof frame).

**Fixed en route (real engine bug, found by the catalog's frames):**
box packing used the box's own parent-relative position as its
children's origin, double-offsetting every child of any box not at
(0, 0) — invisible under the Phase 5 tests (all their boxes sat at
the origin). Children are now packed in box space per the core's
parent-relative contract, and every subclass constructor re-notifies
layout after initializing its fields (the create-time notification
measures a still-zeroed subclass). test_layout's margins/nested
expectations recomputed to the contract.

**Shipped (completion slice — text layout + keyboard polish):**
- `fdk_font_break_lines_utf8` / `fdk_text_line` (fdk_text.h): greedy
  word-wrap over the SAME shaping walk as measure/draw — a line's
  reported advance is by construction what its bytes paint at.
  Hard `\n`/`\r\n` breaks, trailing-space trimming, mid-word breaks
  for over-long words, count-only calls, max_lines truncation flag
- `fdk_font_ellipsize_utf8`: maximal codepoint-boundary prefix that
  fits beside a U+2026 ellipsis; the shared ellipsis constant lives
  in one place so the measured and drawn glyph cannot drift
- Label modes (fdk_widgets.h): `FDK_LABEL_NOWRAP / _WRAP /
  _ELLIPSIZE` via `fdk_label_set_mode`, per-line alignment via
  `fdk_label_set_alignment` (START/CENTER/END, FILL = START), and
  `fdk_label_get_line_count`; the display cache rebuilds on arrange
  (resize) and lazily at paint
- Radio arrow-key traversal: Up/Left previous, Down/Right next,
  wrapping, hidden/disabled members skipped, focus follows
  selection, lone radios let the arrows bubble
- Tests: +2 headless text cases (wrap/ellipsis contracts incl.
  agreement-by-construction and maximality), +2 headless widget
  cases (label modes with pixel verification, radio arrows), +1 X11
  GUI case (server-readback of wrapped bands, edge-clipped
  ellipsis, REAL XSendEvent arrow keys, resize re-wrap)
  — 73 headless, 20 X11 + 1 honest skip
- `examples/07_text_layout.c` + rig (TEXT LAYOUT DEMO PASS, 16 PIL
  checks: wrap bands, edge clipping, three alignments, selection
  dot moving between holds, live re-wrap after window narrowing)

**Fixed en route (two more REAL Phase 5 engine bugs, found by the
07 demo):**
1. `box_class_of` used exact class identity, so the catalog's Frame
   (a box subclass delegating both packing hooks) was NOT treated
   as a box: its own children never triggered its relayout, and the
   box setters (spacing/padding/homogeneous/orientation getters)
   were silent no-ops on it. Box-ness is now HOOK DELEGATION — any
   class running the box packing hooks relayouts like a box.
2. A container's natural-size change never propagated to ANCESTOR
   containers: `content [ frame [ radios ] ]` sized the frame
   before the radios existed and nothing ever re-ran content's
   layout — the radios stayed at 0x0 (demo 06 survived only because
   its last content child happened to be created after every
   frame's children). `fdk_widget_child_layout_changed` now
   propagates up the parent chain (pure geometry, terminates at the
   first non-container), and the box setters propagate too.
   Regression test: test_layout's nested child-change propagation
   case (fontless, exact numbers).

**Completion slice (subpixel + synthetic styles):**
- **Subpixel glyph positioning**: the shaping walk floors each
  glyph's left edge and quantizes the fractional remainder (kerning
  and fractional advances make it non-integral) into one of 4
  phase-keyed rasterizations (`stbtt_GetGlyphBitmapSubpixel`), so
  every glyph paints within 1/8 px of where the float pen actually
  is; the measured total stays `round(final pen)` — v1 behavior
  unchanged. The glyph cache keys (glyph, phase) pairs (capacity
  2048 = the old 512 glyphs' worth of coverage); y stays integer
  (lines sit on the baseline). Contracts pinned by tests:
  measure/draw agreement survives fractional pens, redraws are
  deterministic, a pen shift of exactly 1px translates the ink
  pixel-exactly with ZERO new rasterizations (phase keys ignore the
  integer part), and a repeated glyph fans out across phases (more
  cache entries than codepoints).
- **Synthetic bold/italic** (`fdk_font_set_style` / `get_style`):
  SYNTHESIS, not face selection — BOLD dilates strokes by
  pixel_size/24 px (min 1) and grows the advance by the same stem
  (a bold run measures exactly as wide as it paints); ITALIC is an
  oblique shear anchored at the baseline with the advance
  deliberately unchanged (CSS font-synthesis semantics; the
  overhang rides the damage model). A designed face file loaded
  with `fdk_font_load` remains the better choice — the header docs
  say so. Style changes flush the glyph cache (rasterizations bake
  the style in); re-setting the same style is a documented no-op.
  Contracts pinned by tests: argument safety, unknown-bit masking,
  bold advance = regular + stem x inked glyphs (height untouched),
  italic advance unchanged, combo = stem only, flush + idempotence,
  draw agreement under style.
- En route, the eviction test's cache bound was recomputed for the
  new capacity (2048 (glyph, phase) entries) and the two stale
  capacity mentions in `fdk_text.h` / `docs/text.md` updated.

**Still to build (rest of Phase 6):**
- Subpixel glyph positioning; bold/italic faces beyond file choice
  — BOTH SHIPPED ABOVE; nothing remains. (Width-for-height layout
  for wrap labels stays recorded as a v1 limitation below — it
  wants a later layout pass, not more Phase 6 scope.)

Phase 3's original "src/text/ gap" is closed (and Phase 3 itself is
now COMPLETE — its completion slice landed after Phase 8, closing
MIT-SHM, X11 double buffering, transforms, image decode, alpha
blits, AA primitives, and HiDPI; see the Phase 3 entry). The wrap label's documented v1
limitation (natural height measured at the natural width; no
width-for-height layout) is recorded above and in fdk_widgets.h —
it wants the Phase 5 grid/constraint work or a later layout pass.

## Phase 7 — Theme Engine — COMPLETE (first slice)

- `.fdk` format: grammar spec written first (`docs/fdk-theme-format.md`),
  then the parser (`src/theme/parse.c`) — strict (unknown anything is an
  error with a line number), bounded (1 MiB input, 1024-byte lines,
  128-byte strings), partial-by-design (missing tokens inherit the
  built-in defaults; unknown keys are errors so typos fail loudly),
  zero partial results. The security rules it is written to, and why,
  are `docs/security.md` (new).
- Theme API (`include/fdk/fdk_theme.h`): `fdk_theme` objects with 10
  color tokens + 2 paint metrics, built-in default theme = the Phase 6
  v1 palette byte-for-byte (the no-regression pin: never touching
  themes changes no pixels), programmatic themes, `fdk_theme_parse`
  (memory) / `fdk_theme_load` (file) with exact `fdk_result` codes
  (the error enum's reserved -300/-301/-302 theme codes finally used).
- Runtime switching: `fdk_theme_set_default()` swaps the current theme
  and invalidates every live widget tree — window-owned AND standalone
  — via a new root registry in the widget core (roots can't be
  reparented, so membership changes only at create/destroy). Paint
  hooks resolve tokens at paint time, so there is no cached color to
  flush; same-pointer switch is a no-op; destroying the current theme
  reverts to the built-in first.
- Catalog integration: the 9 `fdk__pal_*` accessors became theme
  lookups (the seam stayed), the Button's corner radius and the
  Separator's band thickness became themed metrics (`BTN_RADIUS` died).
  Deliberately NOT themed in v1: toggle/checkbox/radio shapes (they
  derive from height/geometry, not a corner radius) and the window
  background pixel (backends keep their Phase 2 behavior; the
  `window_background` token is opt-in for apps — demo 08 shows the
  pattern).
- Tests: `tests/test_theme.c` — 8 headless groups under ASan+UBSan
  (v1 pin, programmatic rules, full/partial parse, the full
  adversarial matrix from docs/security.md: 40+ malformed inputs each
  asserting its exact code, file loading incl. zero-byte rejection,
  live-tree switch repaint with pixel proof, root-registry churn) +
  1 X11 GUI case (real window, server-side readback across a switch:
  colors, corner radius, separator thickness, exact round trip).
  The matrix caught two real parser bugs before they shipped
  (section-header dedup colliding with the first key's bit; and the
  spec's own headerless example was illegal under the first grammar —
  fixed by the implicit leading [theme] section).
- `examples/08_theme.c` + `examples/data/{daylight,matrix}.fdk`: live
  theme cycling via real clicks (rig
  `scripts/run_theme_demo_x11.sh`, 13 PIL checks incl. pixel-exact
  round trip). Proof: `docs/screenshots/theme_three_themes_1380x330.png`.
- Remaining (parked, recorded honestly): per-widget-class token
  sections in the format, spacing/padding scale, more metrics,
  auto window-background application, theme file search paths,
  Wayland-side GUI verification (toolchain).

## Phase 8 — Window Decorations & Window Management — COMPLETE

Everything this phase's roadmap line promised, on BOTH backends,
honest about the places the platforms genuinely differ (the EWMH vs
xdg-decoration vs bare-X triangle is documented in
platform_internal.h, not assumed away).

### FDK-drawn decorations (first slice, carried forward and extended)

- `fdk_window_set_decorated(true/false)`: FDK draws its own title
  band INSIDE the client area (a themed band: the window title as a
  catalog Label — default color = themed text — plus three
  window-management buttons) and asks the backend to drop the
  platform's chrome (`fdk_window_set_decoration_font` picks the face;
  default is `fdk_font_load_system_default()`, FDK still bundles no
  font). The content widget (Phase 5) is laid out below the band on
  every configure; `fdk_window_set_title` keeps the band label in
  sync (the backend title is still set for taskbars).
- The band's three buttons — minimize, maximize/restore, close — are
  drawn as VECTOR GLYPHS (lines/rects in the band's paint hook), NOT
  font text: a fontless system loses only the title text, never the
  window buttons, and the glyphs scale with the button, not the font.
  Hit-testing + hover/press highlight (the same themed control
  tokens the catalog Button uses) live in the band's event callback.
- The band's height is the THEME metric `title_bar_height` (12..64,
  default 28): a layout metric, not just paint — switching themes
  re-arranges decorated windows through the widget core's new
  theme-notify walk (an internal per-widget hook; the public
  fdk_widget_class is untouched).
- The close button synthesizes a REAL FDK_EVENT_WINDOW_CLOSE_REQUEST
  through the normal dispatch path — application close semantics are
  identical whether the WM or FDK's button asked.
- Build-system hardening found by the first slice: the Makefile had
  NO header dependency tracking — fixed with `-MMD -MP` + `-include`
  (placed AFTER all targets; an earlier include made the first .d's
  object the default goal — make pitfall, documented in the
  Makefile).

### Window state (maximize / minimize / restore)

- Public API: `fdk_window_maximize` / `fdk_window_unmaximize` /
  `fdk_window_minimize` / `fdk_window_restore` /
  `fdk_window_is_maximized` / `fdk_window_is_minimized` — all
  requests, with the truth reported back through the new
  FDK_EVENT_WINDOW_STATE (caches mirroring last_size's contract).
- Double-clicking the band toggles maximize (400ms / 5px slop — the
  math is a pure function, unit-tested headless).
- X11, three worlds keyed off a connect-time _NET_SUPPORTED probe:
  an EWMH WM gets _NET_WM_STATE add/remove client messages (source
  indication: application) and reports back by rewriting the
  window's _NET_WM_STATE property, which FDK watches
  (PropertyChangeMask) — a WM that ignores a request produces no
  event and is_maximized keeps telling the truth; minimize is the
  ICCCM XIconifyWindow request tracked via the WM_STATE property.
  Under BARE X (no WM — Xvfb, kiosks) FDK performs the actions
  itself: move+resize to the full screen with the geometry saved for
  restore, unmap/map for minimize/restore, state events dispatched
  directly.
- Wayland: xdg_toplevel.set_maximized/unset with the state derived
  from configure's states[] array; set_minimized is fire-and-forget
  (the protocol has no acknowledgement and no unminimize request —
  FDK marks optimistically, clears on the next activated configure,
  and fdk_window_restore honestly returns FDK_ERR_UNSUPPORTED).

### Interactive move & resize

- Band drag: preferred path hands the drag to the WM/compositor —
  _NET_WM_MOVERESIZE(MOVE) on X11-with-EWMH (fixes the first slice's
  documented "reparenting WMs move only the client" caveat), xdg_
  toplevel.move on Wayland (the backend tracks the last button
  serial for the request) — falling back to the first slice's
  snap-formulated move under bare X.
- Resize edges: `fdk_window_set_resizable` (auto-ON while
  decorated — owning the chrome means owning resize): a 5px
  border/corner zone (pure-function classification, headless-tested)
  captures drags — handed to the WM/compositor where available
  (_NET_WM_MOVERESIZE direction codes / xdg_toplevel.resize edges),
  or driven by FDK itself under bare X through a new atomic
  `window_move_resize_to` op, clamped to the app's
  fdk_window_set_size_limits (on a bare X server there is no WM to
  enforce hints) with sane internal floors.

### Wayland xdg-decoration

- The protocol is generated from wayland-protocols (checked into
  src/platform/wayland/generated/ like xdg-shell) and bound as an
  OPTIONAL global: no zxdg_decoration_manager_v1 -> set_decorated
  honestly fails FDK_ERR_UNSUPPORTED rather than double-decorating.
- The per-window decoration object is created at WINDOW-CREATE time,
  before the first buffer — a protocol requirement the first
  implementation got wrong and sway caught live ("xdg_toplevel_
  decoration must not have a buffer at creation"). set_mode
  (CLIENT/SERVER) then rides the normal set_decorated flow, and a
  compositor that forces SERVER_SIDE against our request arrives as
  the new FDK_EVENT_WINDOW_DECORATION — FDK tears its own band down
  before the app sees the event, so a window can never end up with
  two title bars.

### Tests (every layer, every backend)

- Headless `tests/test_window_logic.c` (36 checks): edge
  classification (all 8 zones, corners, degenerate windows,
  out-of-bounds), the edge-drag solver (all edges/corners, min/max
  clamps with opposite-edge anchoring), double-click boundaries.
- X11 GUI: `test_window_state_gui` (bare-X maximize/unmaximize/
  minimize/restore with server-side geometry + exactly-one-event
  semantics), `test_resize_edges_gui` (edges off by default —
  content gets corner presses; SE/E/N drags resize exactly; min
  clamps hold), the extended `test_decorations_gui` (maximize +
  minimize band buttons through real synthetic input, double-click
  maximize/restore, themed band-height switch re-flowing content
  with pixel proof), and `test_ewmh_fake_wm` — the test BECOMES an
  EWMH window manager on a second X connection (advertises
  _NET_SUPPORTED, takes SubstructureRedirect on root, answers
  _NET_WM_STATE by rewriting the property like a real WM) and
  verifies the message paths field-by-field: add/remove actions,
  both maximized atoms, source indication, MOVE direction at the
  exact root coordinates, SIZE_BOTTOMRIGHT from the SE corner, and
  WM_CHANGE_STATE iconify — plus the PropertyNotify-driven state
  events on FDK's side.
- Wayland `tests/test_wayland_integration.c` + `make test-wayland`:
  runs against a REAL compositor. Debian's weston 14 ships no
  xdg-decoration implementation in ANY shell, so the verification
  compositor is sway 1.10 headless (wlroots 0.18, pixman) — which
  advertises zxdg_decoration_manager_v1, honors client-side mode
  requests, and reports tiled windows as maximized at map time (the
  test asserts the reaction matches whichever world the compositor
  picks, honestly handling both). Verified: client-side decoration
  confirmed + band pixel-verified in-frame, maximize state events
  from configure states[], minimize request + optimistic flag,
  restore honestly unsupported, clean teardown under ASan — the
  leak-free teardown caught two real bugs on its first runs (the
  decoration-object protocol-order error above, and a leaked
  wl_surface.frame callback proxy when a window is destroyed before
  the compositor answers — both fixed in the backend).
- Demo rigs: `scripts/run_decorations_demo_x11.sh` now drives the
  full interactive surface with REAL input — band drag, decoration
  toggle on/off/on, maximize button, band double-click restore, SE
  resize-corner drag (460x300 -> 500x330), close via the band — 16
  PIL checks; `scripts/run_wayland_tests.sh` (staging env) runs the
  sway integration test + a self-driving demo cycle with WAYLAND_
  DEBUG protocol counting (set_mode x3, decoration configure, set_
  maximized/unset/set_minimized) — WAYLAND RIG PASS.
- Full-suite verification: 0 warnings debug AND release (Wayland
  enabled); headless 84/84; X11 25/25 (was 22); all 8 demo rigs
  PASS (03-09 + the Wayland rig).

Remaining (parked, honest — none of these are Phase 8 scope): the
weston 14 in Debian has no xdg-decoration (sway is the verification
compositor; a weston-side run needs a weston build with it); resize
cursor shapes (needs a cursor API — Phase 10 territory); per-app
max-size enforcement under EWMH WMs is the WM's job (FDK clamps its
own drags); drag-a-maximized-window-to-restore (the Windows-style
gesture) is not implemented — dragging a maximized band is a no-op,
documented.

## Phase 9 — Advanced Widgets — COMPLETE

Every item on the original list is implemented, integrated, tested
headless + on X11 with real input + on Wayland against a real
compositor, demoed with pixel-verified rigs, and documented:
ScrollView, List, Tree, ComboBox (non-editable + editable), Menu
(bar + dropdowns + submenus + context menus), Toolbar, Slider,
SpinButton, Notebook, Dialog (message dialog, X11 modal grabs),
Canvas, and text Entry (cursor, selection, clipboard, IME
groundwork). The enabling layers underneath them — the pointer
modifier bitmasks, the popup window platform layer, and the
widget→window back-edge that lets a widget open its own toplevel —
are part of this phase and landed with it.

### Clipboard architecture (the first slice)

- Public API: `fdk_clipboard_set_text` / `fdk_clipboard_get_text`
  (UTF-8; set copies, get returns a caller-freed string or NULL).
  One OPTIONAL platform-op pair (`clipboard_set_text` /
  `clipboard_get_text`) — the whole mechanism is backend-side, no
  widget code touches wire formats.
- X11: the ICCCM `CLIPBOARD` ownership model —
  `XSetSelectionOwner` on a dedicated helper window with
  timestamp-verified acquisition (a losing race answers NULL,
  never stale bytes), `TARGETS` + `UTF8_STRING`/`STRING`/`TEXT`/
  `text/plain;charset=utf-8` request serving with a synthetic
  SelectionNotify reply (grant AND refusal — the requesting
  client never hangs), `SelectionClear` releasing the buffer,
  and a bounded (250 ms) non-re-entrant wait for the
  SelectionNotify on paste. Deliberately NOT supported (see
  `fdk_clipboard.h`): PRIMARY (FDK is a CLIPBOARD-only client),
  INCR incremental transfers (refused with a warning — recorded
  below), and COMPOUND_TEXT (the four targets above cover every
  modern client). The test SUITE becomes a second client and
  performs the real selection handshake through the X server
  both directions.
- Wayland: `wl_data_device` + `wl_data_source` (offer with the
  MIME set above, `send` writing to the read fd, cancellation)
  and `wl_data_offer` receive for pasting. The `wl_seat` gained a
  `wl_data_device_manager` binding; the sway rig verifies the
  set/get round trip on FDK's own selection (a headless compositor
  mints no other clients, so cross-client transfer is the X11
  suite's job — recorded, not faked).

### Text Entry

- Public API (`include/fdk/fdk_widgets.h`): text get/set,
  byte-offset cursor get/set, selection anchor+extent
  get/select-range/select-all, changed + activate (Enter)
  callbacks, password mode, max length, read-only mode, and
  `fdk_entry_set_preedit` — IME GROUNDWORK: a composition string
  renders underlined at the cursor without entering the text
  buffer, exactly the seam a real input method needs (the platform
  key events route through it; wiring an actual IME protocol is
  Phase 10+ scope, recorded below).
- Editing model: byte-offset cursor that CLAMPS to cluster-safe
  boundaries (never lands mid-UTF-8-sequence), selection with
  keyboard (Shift+arrows/Home/End) and pointer (press-drag with
  the implicit grab, double/triple click word/line select using
  the shared `fdk_text` cluster walker), clipboard cut/copy/paste
  through the real backend clipboard above, Backspace/Delete with
  selection-erase priority, and Home/End. All of it is pure logic
  over the Phase 6 text measuring walk — the same code that
  measures paints the cursor and selection.
- Rendering: a 1px caret bar rendered while enabled + focused
  (steady — blinking is deliberately absent in v1 because FDK
  has no animation/timer clock yet; a blink needs a repaint
  source, recorded below), selection renders as a themed
  highlight band, and horizontal scrolling follows the caret
  (the entry is a single-line viewport over its text). Password
  mode renders bullets without changing the buffer.

### Containers

- **ScrollView** (`fdk_scrollview_*`): content at (-x,-y) clipped
  by the paint walk (the Phase 4 clip stack does the actual
  clipping — a scroll is one offset change + one invalidate, no
  per-child geometry churn), internal overlay Scrollbars that
  auto-hide when the content fits, thumb drag with implicit grab +
  trough paging, wheel events bubble up from the content and
  scroll the view, `scroll_to`/`scroll_by`/`get_scroll_offset`
  programmatic control. Themed `scrollbar_width` metric (6..24,
  default 12).
- **List** (`fdk_list_*`): row model + view in one widget;
  SINGLE/MULTIPLE/NONE selection modes with the full modifier
  grammar (plain collapse, Ctrl toggle, Shift range, Ctrl+Shift
  additive — riding the Phase 9 pointer-modifier bitmask), a
  MOVING key cursor shared by arrow navigation and Shift
  extension, row CRUD that self-syncs the view (create/destroy
  surplus row widgets, remap in child order).
- **Tree** (`fdk_tree_*`): flat node store with STABLE handles
  (indices survive reorders), the visible sequence derived
  pre-order from expansion state, row widgets synced like the
  List's, stroked-triangle expanders that toggle without selecting
  (expander-zone hit testing), keyboard model derived FROM the
  selection (Left collapses-or-jumps-parent, Right
  expands-or-enters-child, arrows move with Shift extending).

### Control family

- **Slider**: pointer jump-to + drag with quantized steps
  (round-half-to-even, unit-tested), keyboard stepping
  (arrows/Page/Page-ends/Home/End), min/max clamps, value
  fraction, themed track + filled span + handle.
- **SpinButton**: an embedded Entry (real text editing, locale-
  free strict parsing — `strtod` semantics, no locale commas) with
  chevron steppers; commits on Enter, on stepper press, and on
  focus-leave (the FOCUS_OUT that never bubbles — the spin
  watches the entry's events); unparsable input keeps the last
  value.
- **Toolbar**: stock Buttons in a row + bar chrome (themed rule),
  `fdk_toolbar_append`/`insert`/`remove`/`separator`, wired to
  the layout-notifier so add/remove reflows the bar.
- **Notebook**: adopted pages (the notebook reparents the page
  widget, exactly-one-visible semantics enforced), measured tab
  rects with per-tab hover (tracked from MOTION because invisible
  pages never hover), tab clicks switch + notify, programmatic
  `fdk_notebook_set_current_page`, page add/remove reflows.
- **Canvas**: a paint callback with (surface, bounds, clip) and
  DAMAGE-DRIVEN call counts — the canvas repaints only what its
  region intersected, the app draws with the full Phase 3
  primitive set, and the rig asserts the call counts match the
  damage actually sent.

### Popup window platform layer (the enabling primitive)

- Public API: `fdk_window_create_popup(ctx, parent, x, y, w, h,
  &win)` — popup windows position parent-relative, stack in a
  family (the parent tracks its popup chain; destroying the
  parent force-destroys the chain deepest-first), take a
  POINTER+KEYBOARD grab at show, and deliver FDK_EVENT_WINDOW_
  CLOSE_REQUEST on outside press, Escape, or platform dismissal.
- X11: override-redirect windows (no WM reparenting, no focus
  stealing) mapped THEN grabbed (XGrabPointer/XGrabKeyboard on the
  popup, owner-events=false) — out-of-bounds presses under the
  grab become close requests. `window_popup_regrab` re-asserts a
  popup's grab after a popup stacked ABOVE it closes (server
  grabs don't stack; closing a submenu would otherwise leave the
  parent menu ungrabbed).
- Wayland: `xdg_positioner` (anchor + gravity + constraint
  adjustment, slide-x/y so menus flip at screen edges) +
  `xdg_popup` + `xdg_popup.grab` citing the last input serial;
  `popup_done` becomes the close request. The serial-0 caveat is
  documented in the header: before any real input arrives, a
  grab request is protocol-illegal, so the first popup of a
  session shows but doesn't dismiss on outside clicks (the
  backends track every button/key/pointer-enter serial to make
  this window vanish in practice).
- Popup windows are TOOLKIT-OWNED plumbing: the widget layer
  (menus, combos) creates them, auto-paints them, and dismisses
  them; the application loop pumps events and paints ITS windows
  only.

### Menu

- Three-layer design: the **model** (`fdk_menu`, `fdk_menu_item`)
  is pure data — items with type (normal/separator/check/radio),
  text, shortcut hint, enabled + checked state, a BORROWED
  submenu pointer (any item can carry one — `fdk_menu_item_set_
  submenu`; the submenu is a separate app-owned model), and an
  on_activate callback that runs AFTER the chain closes and any
  check/radio state flips. The **view** is a widget that renders
  a model's visible page. The **bar** is a horizontal widget of
  titles that opens views as popups. All three live in
  `src/widget/menu.c` (~1500 lines).
- Activation paths: click (press selects + highlights, release
  inside activates), keyboard (Down/Up move the highlight, Enter
  activates, Left/Right traverse the bar, Escape closes one
  level, accelerator letters not implemented — recorded below),
  and hover-opens (once a chain is open, moving across bar titles
  swaps menus without another click).
- Submenus open NESTED popups (a chain: each view is a popup
  anchored to its parent item) — Escape peels one level, outside
  click closes the chain, activating a normal item closes the
  whole chain and fires the callback. The chain re-grabs on each
  level's dismissal (the `window_popup_regrab` op above).
- Check items flip + notify; radio items enforce one-checked-per-
  group; separators render as themed rules and never highlight.
- Context menus: `fdk_menu_popup_at(menu, anchor_widget, x, y)`
  opens any model as a context menu at a widget-relative point.
- Layout: rows stack vertically at the `menu_item_height` theme
  metric (16..48, default 26) with text-measured widths
  (shortcut column reserved when any item has one), width
  clamped to the popup surface, height clamped at 512px (no
  internal scrolling in v1 — recorded below).

### ComboBox

- Two modes from one widget: non-editable (a button + chevron
  that opens a popup list) and editable (`fdk_combo_set_editable`
  swaps in an embedded Entry — real text editing in the field;
  typing goes CUSTOM, a first-class state distinct from every
  model row, not an error; picking a row from the dropdown
  replaces the custom text). The dropdown does not filter on
  typing yet — recorded below.
- Model API: append/remove/clear/count/text/active
  get/set/active_text + `on_changed`. Picking a row sets active,
  fires on_changed, closes the popup; Escape dismisses without
  changing the selection; the opener keys (Down/Up/Enter on a
  focused non-editable combo) open the dropdown and hand focus
  to it.
- The dropdown is the menu machinery's popup view (shared
  auto-paint, grab, dismissal) — one popup implementation serves
  menus, submenus, and combos.

### Dialog

- Public API (`include/fdk/fdk_dialog.h`): `fdk_dialog_show_
  message(ctx, &options, on_response, user)` — buttons presets
  (OK / OK-Cancel / Yes-No-Cancel), a response enum delivered to
  ONE callback, non-blocking by design (nothing in FDK blocks the
  event loop; the dialog is a real toolkit-owned window).
- Modality: on X11 the new OPTIONAL `window_set_modal` platform
  op takes a pointer+keyboard server grab on the dialog's
  toplevel — presses outside arrive as ordinary out-of-bounds
  events and are IGNORED (the modal contract is "input waits for
  the dialog", not "click-away closes it"), and the grab is
  released on destroy. The test suite verifies the grab is held
  SERVER-SIDE by refusing a foreign grab while the dialog lives,
  and released after.
- Wayland honestly: NO toplevel-grab protocol exists — dialogs
  are NON-MODAL there (documented in the header, tested as
  such: the dialog maps, answers, and tears down cleanly, but
  nothing blocks other windows). Faking it client-side (eating
  events aimed at other windows) is not possible on Wayland and
  is not attempted.
- Responses: Enter answers the default (OK/Yes), Escape answers
  Cancel/No, button clicks answer their button, and EARLY
  DESTROY (the app kills the dialog) answers Cancel through the
  destroy-notify path so no callback is silently dropped.

### Pointer modifiers + the widget→window back-edge

- `fdk_pointer_button_event` and the widget pointer event carry a
  modifiers bitmask (`FDK_MOD_SHIFT/CTRL/ALT/LOGO`) — translated
  from `xbutton.state` on X11, from `xkb_state` on Wayland. The
  List/Tree/Entry modifier grammar rides it. (A pointer-only
  Wayland seat has no xkb_state — found live by the virtual-
  pointer rig; reading modifiers there returns 0, not a crash.)
- Widgets can open windows: the menu/combo/dialog machinery
  creates popups and dialogs from INSIDE widget event callbacks.
  The back-edge (widget → `fdk_window_create_popup` → backend
  window → auto-paint) runs through the window layer's
  toolkit-owned-window path, which paints popups itself instead
  of invoking the application's window callback.

### Tests (every layer, every backend)

- Headless (`make test`, 140 checks across 18 binaries, ASan+
  UBSan): entry editing model (cursor clamping, selection
  algebra, clipboard cut/copy/paste against a MOCK backend,
  password/max-length/read-only), scrollview math (offsets,
  clamping, auto-hide thresholds), list + tree selection
  semantics (the full modifier grammar as table-driven cases),
  menu model CRUD + measure math (row stacking, separator/
  shortcut columns, width clamping), menu view activation
  (click/keyboard/check/radio/disable), combo model + editable
  state machine, slider/spin/notebook/toolbar/canvas logic, and
  the window-logic pure functions.
- X11 GUI (`make test-x11`, 62 checks + 1 honest skip): the
  clipboard suite performs REAL selection handshakes through the
  X server (both directions, MULTIPLE, INCR refusal, loss of
  ownership); Entry driven by real XSendEvent keys (typing,
  Shift-selection, Ctrl+V from the real clipboard); popup
  windows pixel-verified + both dismissal paths; menu GUI (bar
  click maps the popup, AUTO-PAINTED server-side, keyboard Down+
  Enter activates, check items flip, submenus nest, Escape peels
  levels, hover opens the chain); combo GUI (dropdown lifecycle
  through real input); dialog GUI (mapped as a real toplevel,
  modal grab held server-side — a foreign grab is REFUSED while
  the dialog lives — Enter/Escape/click responses, early-destroy
  answers Cancel, grab released on destroy).
- Wayland (`make test-wayland` against sway headless + the rig):
  decorations + state + reflow + HiDPI + clipboard round trip as
  before, plus the NEW menu popup test — the rig drives sway's
  seat cursor to absolute coordinates so a REAL button event
  (and a valid grab serial) reaches the client, the menu popup
  maps THROUGH the compositor, an item is clicked for real, the
  chain closes, and outside-click dismissal works; and the
  dialog test (mapped + auto-painted, early destroy answers
  Cancel, clean teardown under ASan).
- Demo rig: `examples/11_advanced.c` + `scripts/run_advanced_
  demo_x11.sh` — every Phase 9 control in one window, driven
  with real input; PIL verifies 5 frames (File menu popup themed
  + mapped, combo dropdown, modal dialog, nested submenu right
  of its parent, chain dismissed) plus every phase marker. (The
  Wayland rig exercises the same machinery through the sway
  integration suite's menu/dialog cases, not this demo binary.)

### Remaining (parked, recorded honestly — none of it Phase 9 scope)

- IME protocols (text-input-v2/v3) are groundwork-only: the
  preedit seam exists and is tested; wiring a real input method
  is Phase 10+ (i18n) scope. X11 INCR clipboard transfers are
  refused (oversized clips need the incremental protocol — the
  refusal is logged, never half-received).
- Menu accelerator underlined letters (Alt+letter mnemonics) and
  application-global accelerators are not implemented — shortcut
  HINTS render, but nothing parses a modifier grammar at
  dispatch time yet.
- Editable-combo typing does not filter the dropdown (the full
  model always shows; ghost-completion and prefix filtering are
  future polish); neither menus nor combo dropdowns scroll in v1
  — the shared popup measure clamps height at 512px (combo
  dropdowns ARE the menu view machinery, so they inherit the
  clamp). The Entry caret does not blink (steady bar — blinking
  needs a repaint clock FDK does not have yet; a Phase 10/11
  timer primitive is the prerequisite).
- Wayland modality is impossible without a compositor protocol
  (dialog-grab); the xdg-desktop protocol conversation is
  tracked upstream — FDK reports non-modal there, documented.
- A `make test-wayland` target that starts its own compositor is
  still a rig script (`scripts/run_wayland_suite.sh`), not a
  Makefile target — the toolchain lives in a user prefix.

## Phase 10 — Accessibility / Internationalization — COMPLETE

The roadmap line promised four things — an accessibility abstraction
(roles, names, states, relationships, keyboard navigation)
implemented as far as practical WITHOUT requiring a platform
AT-SPI-equivalent dependency, gaps documented rather than faked; and
the three i18n pillars (locale-aware formatting, translation
infrastructure, pluralization architecture). All four landed, tested
headless + X11, with exactly one deliberate gap: the AT-SPI2 BRIDGE
process itself, which the phase's own wording scopes out ("without
requiring a specific platform AT-SPI-equivalent dependency") — the
seam it would consume (query + notification + action + text +
relations) is the API below, and the bridge is a consumer of it the
same way the X11/Wayland backends sit under the widget layer.

### What landed (the a11y core, `include/fdk/fdk_a11y.h`)

- **The design**: the accessibility tree IS the widget tree. There
  is no parallel object model and no daemon — a bridge (a future
  AT-SPI2 bridge process, an embedded screen reader, a test
  driver) enumerates the SAME tree apps build, via the public
  walker (`fdk_widget_parent`/`child_at`) + `fdk_a11y_describe`,
  observes it through subscriptions, and drives it through
  actions. This is deliberately the GTK/Qt architectural
  difference: their a11y trees are bridge-side projections with
  cache-coherency bugs; FDK's is the tree itself, so it can never
  disagree with what is on screen. And because the widget layer is
  backend-neutral, the whole a11y layer is verifiable headless.
- **Describe**: `fdk_a11y_describe(widget, &info)` snapshots a
  widget — role (34 WAI-ARIA/ATK-named roles), accessible name
  (per-widget override > class-computed: a Label's text, a
  Button's label, a Frame's title, a window's title), description,
  states (17-bit set; the core computes ENABLED/VISIBLE/SHOWING/
  FOCUSABLE/FOCUSED — SHOWING walks ancestors so a hidden
  container hides its subtree — the class contributes CHECKED/
  SELECTED/EXPANDED/EDITABLE/READ_ONLY/MULTI_SELECTABLE/
  HAS_POPUP/MODAL/...), root-absolute bounds, and a VALUE
  interface (current/min/max + rendered text: sliders, progress,
  spins, scrolls, entries, notebooks, combos).
- **Notifications**: `fdk_a11y_subscribe(scope, fn, user)` —
  subtree-scoped or global (NULL = everything, the bridge
  pattern); bounded at 16 subscriptions (FDK_ERR_LIMIT beyond);
  events for CHILDREN/STATE/NAME/DESCRIPTION/BOUNDS/VALUE
  changes, fired by the widget core at every mutation point
  (create/destroy/reparent, set_bounds, show/hide, enable/
  disable, focus moves — both ends — label/title/text/value
  setters) AFTER the mutation, with the snapshot-walk discipline
  that makes callback-time tree mutation safe. Detached-widget
  events (destroy) reach global subscribers only — subtree scopes
  cannot contain a detached widget; documented in the header.
- **Actions**: `fdk_a11y_perform(widget, action, value)` —
  programmatic driving through each widget's OWN public semantics
  (not input synthesis): ACTIVATE (buttons fire their callbacks,
  checkboxes toggle, list rows select, tree nodes select,
  combos open), FOCUS (universal over focusable widgets),
  INCREMENT/DECREMENT/SET_VALUE (sliders, spins), EXPAND/
  COLLAPSE (tree items). `fdk_a11y_actions_of` queries what a
  widget supports RIGHT NOW (a button without a callback
  advertises nothing; a collapsed tree item cannot COLLAPSE).
  This API is also the UI-automation seam: the test suite drives
  widgets with it.
- **Class descriptors**: `fdk_a11y_class` (role + describe/
  actions/perform hooks) attached to the widget class vtable's
  new `.a11y` field (safe append per the pre-1.0 ABI policy —
  every class def uses designated initializers). App-defined
  subclasses can point it at their own static descriptor.
- **Catalog wiring — every widget describes itself**: Label,
  Button, Toggle, Checkbox, Radio, ProgressBar (value 0..1 +
  "75%" text), Separator, Frame (GROUP + title), Entry (EDITABLE/
  READ_ONLY states, text as value), ScrollView (scroll position
  as value + SET_VALUE), List (MULTI_SELECTABLE; rows are real
  widgets: LIST_ITEM role + name + SELECTED + ACTIVATE), Tree
  (rows: TREE_ITEM + EXPANDED + ACTIVATE/EXPAND/COLLAPSE), Slider
  (value + SET_VALUE/INCREMENT/DECREMENT), SpinButton (same),
  Toolbar, Notebook (TAB_LIST + current page as value +
  SET_VALUE), Canvas, ComboBox (HAS_POPUP/EXPANDED + active index
  as value + SET_VALUE/ACTIVATE), MenuBar/MenuView (container
  roles), Dialog (DIALOG + MODAL state, title as name), and
  window roots (WINDOW role, the window's title as the accessible
  name — window.c creates roots with the new window-root class;
  `fdk_window_set_title` updates the name).
- **Bonus, forced by honesty**: the Phase 9 roadmap claimed Entry
  password mode, read-only mode, and max length — code review
  during this slice found they were never implemented. They now
  are: `fdk_entry_set_password` (one bullet per CLUSTER — buffer,
  caret, selection, hit-testing all keep working in byte/cluster
  space; only rendering changes), `fdk_entry_set_read_only`
  (selection + copy keep working — the reader contract; typing,
  cut, paste, Backspace/Delete consumed and ignored), and
  `fdk_entry_set_max_length` (cap in bytes, applies to growth
  only; typing, paste, and set_text all refuse to exceed it).
- **Error code**: FDK_ERR_LIMIT (-8, "resource limit reached") —
  generic, for bounded-resource refusals (first user: the a11y
  subscriber cap).
- **Tests**: `tests/test_a11y.c` — 97 headless checks under ASan+
  UBSan: the full catalog describe matrix (roles, names, states,
  values), override precedence, SHOWING-vs-VISIBLE with hidden
  ancestors, the notification matrix (children/bounds/state/
  name/value, focus both ends, radio sibling unchecks), the
  subscriber discipline (scope filtering, duplicates, limit,
  unsubscribe/NOT_FOUND), and the ACTION drivers verified against
  real widget state (button callbacks fire, slider SET_VALUE
  quantizes, tree EXPAND/COLLAPSE flip model state, notebook/
  combo/scrollview SET_VALUE). Plus one X11 GUI case: a live
  window's root (WINDOW role, title as name, bounds == window
  size, set_title propagates) and REAL typed input read back
  through the Entry's value interface.

### What landed second (the completion, two commits)

- **Virtual children — painted rows are addressable** (the ATK/IA2
  pattern for rendered content): containers that DRAW their rows
  enumerate them through the a11y class's new virtual_* hooks with
  the same describe/actions/perform shape real widgets get.
  `fdk_a11y_virtual_count/describe/actions/perform` are the public
  queries; the container fires CHILDREN_CHANGED when its virtual
  set changes (combo row mutations, notebook page appends, menubar
  title add/remove). Wired: MenuView rows (MENU_ITEM /
  CHECK_MENU_ITEM / RADIO_MENU_ITEM / SEPARATOR, item text as the
  name, CHECKED, per-item ENABLED, ACTIVATE driving the exact
  pointer-release path; bounds computed from the row layout),
  MenuBar titles (MENU role, HAS_POPUP + EXPANDED-while-open,
  ACTIVATE replaying the full click semantics: toggle the open
  menu, switch while a chain is open, or open fresh — honestly
  failing headless where there is no window to anchor a popup),
  ComboBox options (OPTION role, SELECTED on the active row,
  ACTIVATE = fdk_combo_set_active's path), Notebook tabs (TAB
  role, SELECTED on the current page, ACTIVATE = switch, strip
  geometry bounds). Menu MODEL item mutations while a popup is
  open do not fire per-view notifications (the model has no view
  list; views are transient popup content a bridge enumerates
  fresh) — the one documented notification gap.
- **Text interfaces** (the ATK Text shape): char/word/line
  granularity over the standard operand of BYTE offsets, with
  caret and selection. Entry implements it fully — reads AND
  mutators (set caret / set selection through the same public
  semantics the keyboard uses, off-boundary offsets refused);
  word runs use the exact double-click word_range; char runs are
  UTF-8 cluster-aware. Label implements it read-only (a Label
  wraps at PAINT time, so v1 reports the whole text as one line —
  visual line runs need the display cache, parked below).
  SpinButton delegates to its embedded Entry. `fdk_a11y_text_
  length/caret/selection/at_offset/set_caret/set_selection` +
  `fdk_a11y_has_text_interface` are the public API.
- **Relationships**: labelled-by / label-for, described-by /
  description-for, controlled-by / controller-for — stored as
  symmetric directed edges (adding one direction inserts the
  inverse; removing either removes both), bounded at 16 edges per
  widget (FDK_ERR_LIMIT beyond), self-relations refused,
  duplicates no-ops, RELATIONS_CHANGED notifications on both ends,
  and teardown removes every edge that touched a destroyed widget
  so dangling relation targets cannot exist. The classic use — a
  Label naming an Entry — is one call: fdk_a11y_add_relation(
  label, FDK_A11Y_RELATION_LABEL_FOR, entry).
- **The entire i18n engine** (commit "i18n: Phase 10 second
  half"): explicit-locale formatting — FDK never calls setlocale
  and never reads the environment, so one process can format per
  window/widget/call and every result is deterministic under test.
  fdk_locale is a fixed-size VALUE parsed from BCP-47 AND POSIX
  tags (de-CH, de_CH.UTF-8, C.UTF-8, en_US@euro) resolving by
  longest match against a curated CLDR rules table (en-IN Indian
  grouping, de-CH apostrophes, pt-PT vs pt plural split; unknown
  tags format like root, never a guess). Numbers: exact int64
  (INT64_MIN included), doubles via two-stage C-locale
  conversion + separator/grouping/digit rewrite (Western, Indian
  1,23,45,678, Swiss 1'234'567, Arabic-Indic ١٬٢٣٤٬٥٦٧ with
  U+066B/6C), half-even rounding, honest magnitude limits.
  Currency: ISO-4217 table (0/2/3 decimals, position per the
  currency's home convention — a documented simplification vs
  CLDR's per-pair data). Percent with per-locale placement. The
  calendar is the proleptic Gregorian 1..9999 on exact civil-day
  integer math (no time_t, no tm), with pattern-driven date/time
  formatting (CLDR-subset mini-language, quoted literals, 12/24h
  with forced overrides, CJK period-first ordering: 오후 3:30) and
  month/weekday names for 15 languages — FORMAT (inflected) forms
  where the language requires them ("25 декабря", not "декабрь";
  "25 grudnia", not "grudzień"). Pluralization: the six CLDR
  categories on the standard operand set (n/i/v/w/f/t), 33
  languages covering every rule shape, fraction-category
  differences honored (ru/pl fractions → other, cs/sk/lt → many,
  fr 1.5 → one, ar 3.5 → other; Polish "one" is exactly 1 while
  Russian covers 21 — the classic confusion pair, both correct).
  Translation catalogs: the strict, bounded .fmo format
  (docs/fdk-catalog-format.md — the theme parser's discipline:
  line-numbered errors, no partial results, UTF-8 validation) with
  binary-search lookup, message contexts, category-named plural
  forms, and translate/translate_plural falling back to the
  English source rule. New FDK_ERR_CATALOG_PARSE (-303).

### Tests (the completion adds to the first slice's 97 checks)

- tests/test_a11y.c: virtual children for menu views (roles,
  names, CHECKED, separator discipline, ACTIVATE firing the real
  activation + check toggle, out-of-range/wrong-action
  rejections), menubar titles (HAS_POPUP, honest headless ACTIVATE
  failure), combo options (SELECTED tracking through ACTIVATE),
  notebook tabs (SELECTED, strip bounds, current-page ACTIVATE);
  the text interface (char/word/line runs with exact ranges,
  truncation-with-full-range, caret/selection round trips through
  the public entry API, read-only Label, delegated Spin, no-text
  Button, UTF-8 cluster runs, mid-sequence caret refusal);
  relationships (symmetric add/remove, inverse queries,
  duplicates, multi-edge enumeration, self-relation refusal,
  notifications on both ends, destroy cleanup, the 16-edge
  FDK_ERR_LIMIT cap).
- tests/test_i18n.c (new, 14 groups): locale parse/normalize/
  round-trip/reject matrix, territory override resolution,
  int/double/currency/percent pins across en/de/fr/hi/ar/de-CH/
  en-IN/ja (grouping, digits, separators, half-even ties, INT64_
  MIN, limits, option plumbing), calendar anchors (epoch, known
  weekdays, leap years, full-range round trips at 3033 samples,
  validation), date/time styles per language (ISO, localized
  names, г. suffix, genitive months, 12/24h forcing, CJK periods,
  midnight/noon), plural operands (v/w/f/t exactness incl. the
  double trailing-zero rule) and the per-language category matrix
  (every shape: en/fr/ru/pl/cs/ar/lv/lt/hr/ja), catalog parse +
  contexts + plural selection + fallbacks + 21 adversarial
  rejections with no-partial-result checks + file IO paths.
- Full battery: headless 413 checks (was 237 at the phase's
  start); X11 integration suite PASS (incl. the live-window a11y
  cases); 0 warnings debug AND release both configs.

### Remaining (parked, recorded honestly)

- **The AT-SPI2 bridge process itself**: the seam exists (query +
  notification + action + text + relations); the bridge (D-Bus
  peer, object-path tree, cache protocol) stays OUT of the
  toolkit — SUPERSEDED at 1.1.0 by the no-bus policy
  (`docs/dependencies.md`): the in-process embedded narrator is
  FDK's screen reader answer, and external bridging is an
  application-side consumer of this API, never toolkit work.
  Recorded so nobody hunts for it in the source tree.
- **Label visual-line runs**: a WRAP label reports its whole text
  as one LINE run; per-visual-line runs need the paint-time
  display cache exposed to the a11y layer. (Entry is single-line,
  so its LINE run is exact.)
- **Menu model mutations while a popup is open** fire no
  per-view CHILDREN_CHANGED (models have no view list; views are
  transient popup content). Enumerating a view's virtual children
  is always fresh, so a bridge re-query never lies — only the
  push notification is missing.
- **i18n data breadth, beyond the engine**: names for 15
  languages (not 200+), CLDR's per-(locale,currency) symbol
  positions collapsed to per-currency, ar digit-system
  territories not differentiated (ar-MA-style Latin-digit
  exceptions), per-territory date-pattern variants (en-GB would
  still show M/d/y). The ENGINE takes all of these as data
  additions; nothing structural is missing.

## Phase 11 — Stabilization — COMPLETE

The roadmap line promised five things: the ABI freeze
(`FDK_ABI_STABLE` → 1, with the application-embeddable-subclassing
question revisited), an API cleanup pass, performance profiling
(deliberately last — the project principle against premature
optimization), a memory-safety audit, a full documentation pass, and
packaging. All five landed; FDK is 1.0.0.

### The ABI freeze

- **The audit** classified every struct in every public header
  (docs/abi-policy.md's table): opaque objects (layouts never
  exposed), frozen VALUE types (geometry, color, events, metrics,
  dates, locales — FDK's GdkRectangle/QRect tier), append-only
  INPUT structs (init/window/dialog/number options), the one
  caller-allocated RESULT struct (fdk_a11y_info — future additions
  get accessors, never fields), and the append-only VTABLES
  (widget class, a11y class).
- **Compile-time enforcement**: `src/core/abi_check.c` pins every
  frozen struct's size with `_Static_assert` — an accidental field
  edit now fails FDK's own build instead of detonating in someone's
  application. (The assertions earned their keep immediately: the
  first draft's guessed sizes failed the build, and the file was
  corrected against the REAL measured values.)
- **`FDK_ABI_STABLE` is 1; the version is 1.0.0.**
- **The subclassing decision** (recorded in full in abi-policy.md):
  the widget-class VTABLE stays public (it documents the hook
  contracts), the `struct fdk_widget` LAYOUT stays internal —
  exposing it would freeze the object model forever (the GTK
  lesson). The 1.0 extension surface is class hooks + user_data,
  event callbacks, Canvas, and composition; if real applications
  hit the wall, the 2.0 answer is an allocation-callback pattern,
  not layout exposure.

### Performance profiling (the findings)

`tests/bench.c` + `make bench` (release objects — the sanitizer
build distorts numbers by multiples) measure the hot paths at
realistic workloads; `docs/performance.md` is the recorded baseline.
Two real findings, one fixed:

- **Quadratic tree construction — FIXED**: every child mutation
  relayouted the parent AND propagated to every ancestor, so
  building a nested UI widget-by-widget cost O(n²)-ish — 558 ms for
  a 475-widget tree. The fix is `fdk_layout_begin_batch()` /
  `fdk_layout_end_batch()` (public, fdk_layout.h): inside a batch,
  invalidations mark containers dirty (bit sets) instead of
  relayouting; the flush relayouts each dirty chain's topmost
  container once, whose arrange cascade refreshes every descendant.
  Outside a batch the behavior is byte-identical to the eager path —
  the ENTIRE existing test suite pins that, plus new tests proving
  batched == eager geometry, nested batches, unbalanced-end
  tolerance, and mid-batch destroy safety (destroyed widgets are
  forgotten from the pending set). **515x** on the fixture (558 ms →
  1.1 ms per tree).
- **paint-full is memory-bandwidth, not bookkeeping**: 12 ms/frame
  for a full 475-widget repaint is dominated by overdraw (~38 MB of
  pixel traffic at 800x600), which is what the fill primitives
  cost, not an algorithm bug. Steady-state damage repaints are 46 µs
  (260x cheaper). An occlusion cull is recorded in
  docs/performance.md as the honest frontier — to be built IF a
  profile of a real app ever shows full repaints dominating.
- Everything else measured healthy: 77 ns event dispatch, 23 ns
  plural rules, 1.2 µs whole-tree theme invalidation, 10 µs
  paragraph measurement.

### API cleanup pass

- 298 public functions scanned: every one now carries a doc comment
  (the trivial accessors gained one-liners naming their contracts;
  the non-trivial ones were already documented).
- Zero TODO/FIXME/XXX markers across src/ and include/.
- Naming is uniformly `fdk_<noun>_<verb>`; every public header is in
  the fdk.h umbrella; no backend type leaks into any public header
  (the Phase 2 invariant, re-verified).

### Memory-safety audit

Documented in docs/memory.md as six reproducible criteria: raw
allocators confined to src/core/alloc.c; every allocation path has
its free on EVERY exit (parsers destroy working objects on error;
widget teardown is a complete ordered walk); no unchecked
multiplication before allocation (fdk_alloc_array guards centrally;
the hand-rolled grows were audited for the same shape);
dangling-pointer classes are structurally prevented (unlink-before-
callbacks, two-sided relation removal, layout-batch forgetting,
window-root teardown ordering); every test binary runs ASan+UBSan
with leak detection; every parser-facing buffer is bounded. The
audit's known sharp edges (single-threaded object model; a destroyed
pointer is ordinary C UB) are documented, not hidden.

### Documentation pass + packaging

- docs/: abi-policy (classification + decision), performance
  (baseline + findings), memory (audit), build (bench + pkg-config
  linking), testing (bench), i18n + catalog format (Phase 10) —
  alongside the existing architecture/rendering/text/threading/
  security/testing/build/dependencies/licensing set.
- Packaging: `fdk.pc.in` → `build/fdk.pc` generated at install time
  from `fdk_version.h` (the version can never drift from the
  headers) with the platform deps the build itself resolved as
  Requires.private; `make install` places headers, both libraries,
  and the .pc; `make bench` documented; `make uninstall` removes
  all three.
- **Full battery at freeze: headless 415+ checks, X11 integration
  suite PASS, release build 0 warnings, bench baseline recorded.**

### Remaining (parked, recorded honestly)

- The occlusion cull (see above) — data-driven future work, not
  owed by this phase.
- The AT-SPI2 bridge — REJECTED at 1.1.0 by the no-bus policy
  (`docs/dependencies.md`): the embedded narrator is the
  in-process answer; external bridging is application-side work
  on the public seam.
- Distribution packaging (deb/rpm/APK recipes) — the Makefile
  install + pkg-config are the toolkit's packaging surface;
  distro-specific recipes are downstream work, not library work.

## Post-1.0 maintenance

### 1.0.1 — system font discovery rework (the Arch bug) — COMPLETE

User report: the demos requiring DejaVu or Noto fonts claimed no
font was installed on Arch Linux with `noto-fonts` installed. Root
cause: the system-default probe — and every demo's private copy of
the same list — hardcoded exact filenames like
`NotoSans-Regular.ttf`, while Arch's noto-fonts ships variable
fonts named `NotoSans[wdth,wght].ttf`; fontconfig, `~/.fonts`, and
the XDG font directories were never consulted at all.

The fix (`src/text/fontscan.c`) is the discovery chain GTK and QT
use on Linux, minus their build-time dependency — fontconfig is
dlopen'd at RUN TIME (resolving the long-standing "anticipated
dependencies" entry: nothing linked, vendored, or distributed):

1. `$FDK_FONT_FILE` — explicit user override
2. `$FDK_FONT_DIRS` — user-prioritized directories, scanned first
3. fontconfig — the user's own font policy (`sans-serif`), walked
   past CFF/corrupt candidates, honoring `FC_INDEX`
4. the known exact-path list (Debian/Ubuntu/Arch layouts)
5. a ranked recursive scan of the standard font roots, including
   per-user ones

Variable fonts count as their default (regular) instance — stb
rasterizes the default master, verified end-to-end. A candidate
that passes the tag-level gate but fails the loader's full
container validation (truncated repacks — this staging container
ships exactly such a "variable" Noto whose directory claims 17 MB
inside a 5 MB file) is remembered and the next-best candidate
takes the slot, so one broken font cannot break text everywhere.

- New public API: `fdk_font_get_file_path()` (append-only, per the
  ABI freeze policy; the opaque `fdk_font` grew an internal field).
- New env knobs: `FDK_FONT_FILE`, `FDK_FONT_DIRS` (documented in
  `fdk_text.h`, `docs/text.md`, README).
- Demos 05/06/07/08 dropped their private broken probe lists and
  use `fdk_font_load_system_default()`; the failure notice now
  names the escape hatches instead of suggesting only two font
  packages.
- Tests: a 10-scenario discovery group in `tests/test_text.c`
  (override precedence, invalid-override fall-through, fontconfig
  end-to-end via a private `fonts.conf`, the Arch bracket-name
  scan, nested-dir scan, regular-beats-bold ranking,
  corrupt-candidate rejection, cache consistency, accessor edges,
  argument guard) — plus two Xvfb demo simulations (a valid
  variable font in Arch naming; a corrupt font with fall-through
  to the next stage).
- LeakSanitizer is scoped off fontconfig's process-lifetime init
  allocations inside `fontscan.c` — FDK must not `FcFini` state a
  host application may share; the rationale is documented at the
  code.
- Battery: full headless suite, X11 integration suite, release
  build 0 warnings, bench baseline — all green.

### 1.1.0 — the no-bus policy + the embedded narrator — COMPLETE

Directive: FDK must never rely on D-Bus or any Red Hat/Freedesktop
daemon infrastructure; wherever the desktop bus ecosystem would do
a job for other toolkits, FDK does the job itself, in-process.

**The policy** (`docs/dependencies.md`, "The no-bus policy"):
FDK never links, dlopens, spawns, or requires D-Bus — or any
daemon/bus/activation service — for any toolkit functionality. The
audit: zero D-Bus references in `src/` + `include/`; link line is
`-lX11 -lXext -lwayland-client -lxkbcommon -lm -ldl`; libatspi and
libdbus appear nowhere. The parked "future AT-SPI2 bridge" notes in
this file were rewritten — the bridge is not future work, it is
rejected work, superseded by the in-process answer below.

**The implementation — `src/widget/a11y_narrator.c`, the embedded
screen reader core** (the consumer Phase 10's design always named,
now shipped):

- `fdk_a11y_set_speaker(fn, user)` — the utterance sink: whatever
  the application wires (TTS, braille, a subtitle bar, a log); FDK
  itself links, loads, and requires none of it. Detaching (NULL)
  parks the engine — a narrator with nowhere to speak holds no
  subscriber slot.
- `fdk_a11y_narrator_start/stop/active` — the engine: an ordinary
  global-scope subscriber of the public a11y notifications (one of
  the 16 slots; FDK_ERR_LIMIT surfaces honestly when they are
  full). While started it narrates focus moves (the gaining side
  only — "Save, button"), CHECKED/PRESSED/SELECTED/EXPANDED
  toggles on the focused widget at the new value, and VALUE
  changes of focused non-editable controls compactly
  ("Volume, 64"). The deliberate silences: focus-out, unfocused
  background churn, and per-keystroke typing (editable text value
  floods would drown everything else — the same trade-off real
  screen readers make with echo modes).
- `fdk_a11y_announce(text)` — forced status utterances
  ("File saved") through the sink regardless of engine state.
- `fdk_a11y_compose_announcement(widget, buf, cap)` — the engine's
  own composition path, published: name, role, spoken states
  (checked/pressed/selected/expanded/read-only/required/invalid/
  modal/busy/disabled), and rendered value, snprintf semantics.
- `fdk_a11y_narrator_set_catalog(cat)` — the glue words localize
  through FDK's own i18n engine (gettext-convention msgids: the
  role names plus the state words — the exact set is in the
  header); English when no catalog is wired.

Reentrancy: utterances are heap-composed BEFORE the sink runs, so
the sink never sees borrowed widget state and follows the standard
FDK callback contract (may query, must not destroy).

- **Widget bug found by the narrator tests and fixed**: the
  SpinButton never set FDK_WF_CAN_FOCUS — every other control of
  its family (slider, entry, buttons, toggles) does — so focus()
  on a spin was a silent no-op and the widget was unreachable by
  Tab. Fixed in `fdk_spin_create`.
- Demo: `examples/12_narrator.c` — the no-bus screen reader live:
  a scripted tour narrates focus, a toggle, a value change, and a
  forced announcement to a subtitle label + stdout, then the
  engine keeps narrating interactively (Xvfb-verified; screenshot
  in `docs/screenshots/narrator_demo_460x400.png`).
- Tests: `tests/test_narrator.c` — 46 checks: the composer (names,
  overrides, every spoken state, value renderings, truncation +
  snprintf retry semantics, invalid args), the announce path, the
  engine e2e (focus narration both ends, refocus no-op, toggle at
  new value, unfocused-churn silence, stop/start/park lifecycle,
  typing silence, spin/slider value narration), localization
  through a parsed catalog, and the FDK_ERR_LIMIT slot-exhaustion
  with recovery.
- Battery: clean rebuild 0 warnings, full headless suite, X11
  integration suite, release 0 warnings, bench baseline, DESTDIR
  install (pc reports 1.1.0).

### 1.1.1 — the deferred first frame + the example-suite consolidation — COMPLETE

User report (tested on a real Wayland desktop): "03_widgets is
broken, 11_advanced doesn't open on Wayland, some other ones are
laggy, and there are too many demos — combine and remove the
useless ones."

**The deferred-first-frame bug (every widget example was affected,
not just the two named).** Root cause chain: an FDK application's
documented loop shape is create -> show -> paint -> pump. The paint
before the first pump runs before the first xdg configure has been
read, so the Wayland backend correctly DEFERS the commit
(xdg-shell forbids committing content before ack_configure) — but
the surface layer had already consumed the frame's damage, and the
widget tree had cleared its damage flag. Every later present was a
true no-op: the pending buffer was never committed, the window
never mapped, and the application sat invisible forever. The X11
backend never showed this (no configure handshake; the first
present works immediately), and the Wayland test suite pumped
before its first paint, so the deferral path was never exercised
by a test — the exact ordering every example ships with.

The fix (`wayland_window.c`): the commit the deferred present was
waiting for now happens inside `xdg_surface_configure`, the moment
the configure is acked — the present's commit tail was refactored
into `commit_render_pending()` and is called there when the pending
buffer still matches the configured size; when the configure
proposes a different size, the backend dispatches
`FDK_EVENT_WINDOW_EXPOSE` instead (the same re-drive the X11
backend's ExposureMask provides on first map) and the application's
next paint presents at the real size.

- Regression test: the Wayland integration suite now opens a window
  in the exact application order (show, paint BEFORE any pump) and
  asserts the commit landed — through a new optional platform op,
  `window_ever_presented` (exposed internally as
  `fdk__window_ever_presented()`; NULL = unknown). The op is the
  sanctioned seam: `wayland_platform.h` never leaves
  `src/platform/wayland/`, so the test asserts through the ops
  vtable instead of the backend struct. Implemented on both
  backends (X11: a presented_ever flag set at the first put).
- Also fixed in the same suite: `wayland_injector_start()` used
  `popen()`, which returns a valid stream even when the injector
  binary does not exist — the first write then died with SIGPIPE
  and took the whole test with it, breaking the "absent tooling
  honestly skips" contract. Now an `access(X_OK)` probe gates it.

**The lag.** The demos that DID map on Wayland were the animated
ones, and three of them presented a full frame per compositor
callback forever: the images demo redrew every panel at 60 fps with
a rotating blit; the theme and decorations demos swept their
progress meters forever; the layout demo oscillated the WINDOW SIZE
every frame (a resize storm — new buffers, full damage, compositor
relayout per frame). The consolidated demos below are
damage-gated: animations run for a short intro and then freeze, and
an idle FDK app presents NOTHING (the damage-tracking no-op path,
finally exercised by the examples themselves). `FDK_DEMO_ANIMATE=1`
keeps any animation running for rigs; `FDK_DEMO_FRAMES=N` bounds
the loop for automation.

**The consolidation: 12 example programs -> 8**, merges instead of
deletions:

- `01_hello_world` — unchanged.
- `02_rendering` — was `02_software_render` + `10_images`: the
  damage-demo ball, gradient, and logo panel beside the image,
  transform, and AA panels.
- `03_text` — was `05_text` + `07_text_layout`: the raw-rendering
  gallery as a Canvas widget above the label-mode frames (one
  window, both layers of the text stack).
- `04_widgets` — was `06_widgets` + `04_layout`'s grid (a new
  "Layout — grid" frame with the spanning cell and expanding
  column; the oscillation is gone — resizing the window by hand is
  the demo now). `03_widgets` (the Phase 4 base-class demo) is
  folded in spirit: everything it showed is exercised by the
  catalog.
- `05_theme` — was `08_theme`.
- `06_decorations` — was `09_decorations`.
- `07_advanced` — was `11_advanced` (now maps on Wayland — the fix
  above).
- `08_narrator` — was `12_narrator`; also fixed a real bug found by
  the rig: the demo registered NO window event callback and wired
  no Quit handler, so the window-close button and ESC did nothing
  and the app ran forever.

**Verification** (both new rigs live in the maintainer's staging
scripts, not the repo): every example launched on sway 1.10 headless
(wlroots pixman, floating windows, wlr-screencopy captures via
grim) and on Xvfb (ffmpeg x11grab, closed through the real
WM_DELETE_WINDOW helper) — screenshots pixel-verified per example
(window geometry by background-color bbox, gradient variety, logo,
text glyphs, grid cells, themed content), every app exiting cleanly
through the real close path. Full battery: clean rebuild 0 warnings
(debug, Wayland enabled), headless suite, X11 integration suite,
Wayland integration suite on sway (including the new regression),
both example rigs PASS.

### 1.1.2 — the public version reset to 0.0.1 (the versioning policy) — COMPLETE

**Project decision (owner override):** FDK's public version is
`0.0.1`, deliberately, for the foreseeable future. The number is a
joke ("this absurdly capable toolkit is still called 0.0.1"); the
engineering standard it reports is not. Internal milestones — the
Phase numbers and the `1.0.0` … `1.1.1` labels in this file — are
engineering history, never the public version. Nothing about the
number licenses lower standards, and nothing about the engineering
raises the number. Full policy: `docs/versioning.md` (new).

What leaked and was reset: the tree had been reporting the internal
milestone labels as the public version (`fdk_version.h` said 1.1.1,
`fdk.pc` derived 1.1.1, the README status line said 1.1.1) — a
bookkeeping mistake, not a promotion decision. Changes:

- `include/fdk/fdk_version.h`: `FDK_VERSION_*` = 0.0.1, with the
  policy comment inline (the single source of truth — the Makefile's
  `FDK_PC_VERSION` sed and every banner derive from it; no other
  copy exists to drift).
- `tests/test_core.c::test_version`: the public version is now
  PINNED — asserts `0.0.1` / `FDK_VERSION_ENCODE(0,0,1)` directly,
  so an accidental bump fails the suite instead of shipping.
- `docs/versioning.md` (new): the policy — decoupling rules, what
  0.0.1 does NOT mean, milestone-vs-version mapping, single source
  of truth, bump procedure (owner decision recorded in the doc
  first, then the pin).
- `docs/abi-policy.md`: ABI status re-keyed from semver to
  milestones ("the 0.x series is over" / "major-version-bump event"
  / "pre-1.0" language removed — under the policy the public version
  *looks* like 0.x forever, so ABI status can never be read off the
  number; `FDK_ABI_STABLE` + the Phase 11 freeze are the contract).
- `docs/performance.md`: benchmark provenance line re-labeled
  (measured at the Phase 11 stabilization state; public version
  0.0.1).
- README: status line rewritten for 0.0.1 with an accurate maturity
  description + pointer to the policy; milestone references in prose
  re-worded so they cannot read as public-version claims; this
  roadmap got the versioning convention note at the top.
- Standing rule recorded for future work (also applies beyond
  versioning): do NOT chase GTK/Qt feature parity blindly — use the
  survey to find problems a serious toolkit must solve, prioritize
  by technical value to FDK, never to fill a comparison table; and
  do NOT optimize for adoption/ecosystem size over technical
  quality.

Battery: clean rebuild (debug + release, Wayland on — 0 warnings in
both), full headless suite (version pin passes), X11 integration
suite, fdk.pc reports 0.0.1. One environment quirk chased and
documented during the battery: a one-time fontconfig cold-cache
ASan leak report on the first `make test-x11` in a fresh container
(leak entirely inside `libfontconfig.so`, cache-temperature
nondeterminism, not an FDK bug) — recorded in
`docs/testing.md`'s environment-quirks section with the
verification method.

### 1.1.3 — the real-window-manager fixes (the Cinnamon report) — COMPLETE

User report, tested on a real Cinnamon X11 desktop: "can't drag the
window from the [FDK] bar [on the decorations example], can't resize
it, and the [maximize] button only resets to unmaximized while the
window stays maximized — but the rest works."

All three symptoms traced to the EWMH paths never having run under a
REAL window manager before: the test rigs use bare Xvfb (no WM) and
a fake WM that answers messages but never grabs the pointer, and the
Wayland rig uses sway. Three bugs, each reproduced empirically under
Xvfb + openbox 3.6.1 with real XTEST-driven input before fixing:

1. **The atom typo (maximize/unmaximize desync).** FDK interned
   `_NET_WM_STATE_MAXIMIZED_HORIZ`; the EWMH spec atom is
   `_NET_WM_STATE_MAXIMIZED_HORZ`. XInternAtom with
   only-if-exists=False silently CREATES the misspelled atom, so the
   connect-time _NET_SUPPORTED probe never matched it — every real
   WM looked "maximize-incapable" (`ewmh_state_ok` = 0) and
   `set_maximized` fell through to the bare-X fallback. That
   fallback `XMoveResizeWindow`s to the full screen under a WM that
   owns the window, then optimistically dispatches the state flip —
   while Metacity-family WMs clamp restore requests for windows they
   consider maximized: the window stays maximized while FDK's flag
   (and the title-bar button glyph) report unmaximized. The exact
   user symptom. Fixes: the spelling (x11_connection.c), plus
   `set_maximized` now takes the _NET_WM_STATE client-message
   request whenever an EWMH WM is detected (a WM without the atoms
   honestly ignores it — no lying fallback under a WM; the bare-X
   path only runs when there is truly no WM).

2. **The implicit pointer grab blocked the WM's drag (drag/resize
   dead).** The button press that starts a band drag leaves the X
   server's implicit pointer grab held by the window; a WM that
   drives interactive moves with its own XGrabPointer (openbox,
   Metacity-family, most others) gets AlreadyGrabbed and its move
   op sees no events — the message is sent, accepted, and nothing
   moves. `begin_move`/`begin_resize` now XUngrabPointer before
   sending _NET_WM_MOVERESIZE. Verified under openbox: without the
   ungrab the window never follows the drag; with it the window
   tracks the pointer exactly.

3. **The stale widget-tree grab after a WM-driven drag.** The WM's
   grab consumes the button release that would end the widget
   tree's implicit grab, so `root->grab` stayed set to the
   decoration band forever: press-to-release pairing broken, hover
   frozen, and every later press misrouted to the band — a content
   click would start a spurious window move instead of activating
   the widget. New internal seam `fdk__widget_tree_cancel_grab()`
   (widget.c), called by the window layer at the WM handover (both
   backends — Wayland compositors eat the release the same way).

Why the tests missed all three: the fake-WM test interned the same
misspelled atom as the library (a self-confirming test — it verified
FDK against a copy of FDK's own bug), and a fake WM that never grabs
the pointer cannot exhibit bugs 2 and 3 by construction. New guards:
`test_ewmh_atom_spelling` (XInternAtom only-if-exists through a
second connection is the oracle — a misspelled name returns None),
the fake WM's atoms spelled per the spec independently, and a
real-WM rig (openbox + Xvfb + direct XTEST input — xdotool's motion
is silently ignored by this environment's Xvfb; its queries work,
which made debugging input delivery its own adventure) verifying
probe/drag/maximize/unmaximize/edge-resize/post-drag-content-click
end to end. Rig: scripts/verify_wm_deco.sh in the staging area.

Battery: clean rebuild debug + release (Wayland on, 0 warnings),
headless suite, X11 integration suite (incl. both new guards), the
Wayland suite + decorations-demo rig on sway, and the openbox
real-WM rig — ALL PASS, twice consecutively.

### 1.1.4 — the resize flash, the stuck highlight, the missing cursor (the second Cinnamon report) — COMPLETE

User report, same real Cinnamon X11 desktop: "the window flashes
like it becomes white then shows it again changing fast while
resizing [...] one visual bug is that the highlight stays on the
maximize minimize button [...] and the mouse doesn't change icon
to show that the window can be resized — it only changes when I
hold click on the side of the window."

Three symptoms, one per layer, all reproduced empirically before
fixing:

1. **The white flash during interactive resize.** The X11
   top-level was created with a white background PIXEL and the
   default ForgetGravity: every resize step made the server CLEAR
   the whole window to white, and the client only repaints after
   the configure event travels back through its event loop — so
   each step of a drag showed a full frame of background. Fix in
   two halves: (a) NorthWest bit gravity at creation, with the
   background flipping from the white pixel to None at the FIRST
   framebuffer acquisition (a never-rendered window keeps its
   documented white — the 01_hello_world contract, matching
   Wayland's committed solid-color buffer — while a rendered
   window is never server-cleared again and every resize retains
   its old pixels anchored top-left); and (b) a SYNCHRONOUS
   damage-gated repaint in fdk_window_dispatch_event's tail for
   configure/state-flip/first-expose events, closing the gap
   between the WM's resize and the application's next loop pass —
   the exact frame where the flash lived. Application paint
   pacing is untouched: only damage FDK's own geometry acceptance
   caused goes out synchronously.

2. **The stuck maximize/minimize highlight.** Maximizing grows
   the window under a STATIONARY pointer: the button flies right,
   no motion event ever arrives (the pointer did not move), and
   the hover highlight computed against the old geometry sticks
   forever. New optional platform op `window_query_pointer`
   (XQueryPointer window-local + bounds check on X11; the seat's
   pointer-focus cache on Wayland) + a dispatch-tail
   revalidation: query the real pointer, route it through the
   tree and the cursor logic exactly as if a motion/leave had
   arrived (the application's event callback never sees these
   synthesized positions). Pointer-leave now also clears the
   band-button hover and the resize cursor — window-layer state
   nothing else was clearing.

3. **The missing resize cursor.** FDK never shaped a cursor at
   all — the cursor the user saw while holding a button was the
   WM's own during _NET_WM_MOVERESIZE. New optional platform op
   `window_set_cursor`, reusing the resize-edge compass: hovering
   an edge zone (same hit-test the press path uses) shows the
   directional resize cursor BEFORE any button is held, restoring
   the default on interior/leave/edges-off. X11 implements it
   with lazily-created XCreateFontCursor glyphs from the server's
   built-in cursor font (core protocol, no libXcursor), cached
   per connection and freed at disconnect; Wayland honestly has
   no implementation yet (proper shaping needs a cursor theme
   over wl_shm + per-enter set_cursor — tracked as future work),
   so the compositor default applies there.

Verified with a new class of test: a mid-storm pixel probe
(scripts/resize_flash_probe.c) that samples the window during a
90-step XTEST resize drag under openbox — the fixed build reads a
worst per-sample white ratio of 0.000 across 1.82M pixels, while a
git-worktree build of the PRE-FIX commit reproduces the user's
flash at ratio 1.000 (the control run proving the probe works).
Three in-suite regressions: server-side resize retention
(bit-gravity + pixel readback before the configure is pumped),
hover revalidation under a stationary pointer with the REAL
pointer warped onto the moving button, and the cursor-affordance
compass through a new internal seam. Wayland gets the same
dispatch-tail behaviors for free where its protocols allow (the
configure-time synchronous repaint is exactly the sanctioned
ack-then-commit flow; hover revalidation reads the seat cache).

Battery: clean rebuild debug + release (Wayland on, 0 warnings),
headless suite, X11 integration suite (incl. the three new
regressions), Wayland integration suite + decorations-demo rig on
sway, X11 + Wayland example rigs, the openbox real-WM rig (all six
1.1.3 checks still green), and the resize-flash probe with its
pre-fix control — ALL PASS.

### 1.1.5 — decorations on compositors without xdg-decoration (the Cinnamon Wayland report) — COMPLETE

User report, third from the same real desktop — this time Cinnamon's
experimental Wayland session: the FDK backend initializes cleanly
(connected, compositor/shm/xdg_wm_base bound), then the demo prints
`06_decorations: set_decorated failed (unsupported)` and the window
never appears — "it instantly crashes", the user suspected the
experimental session itself. The compositor was blameless: Muffin's
Wayland session does not advertise zxdg_decoration_manager_v1, and
FDK's Wayland `window_set_wm_decorations` treated a missing
decoration object as blanket FDK_ERR_UNSUPPORTED — in BOTH
directions. The reasoning inverted the protocol: a compositor that
does not advertise xdg-decoration never draws chrome itself
(client-side is the xdg-shell default), so asking to draw FDK's own
band cannot stack two title bars — it is the ONLY chrome that can
exist. The failure then hit the demo's fatal path (exit before the
window ever maps), which reads as an instant crash.

1. **Manager-less client-side decorations.** set_wm_decorations
   now splits by direction: on=false (FDK draws its band) succeeds
   without a protocol request when the manager is absent — weston
   kiosk-shell, Muffin's Wayland session, most tiling WMs are
   exactly this; on=true (compositor chrome wanted) still honestly
   reports FDK_ERR_UNSUPPORTED, because no protocol way to ask
   exists. The window layer's teardown path already ignored that
   direction's result, so the demo's full toggle cycle now runs on
   such compositors: undecorate leaves a plain chromeless window,
   redecorate brings the band back.

2. **First-configure size proposals were silently dropped** — found
   by the new rig, not by the user: the backend suppressed
   FDK_EVENT_WINDOW_CONFIGURE for the very first configure ("the
   app already knows its creation size"), but resize-at-map
   compositors (kiosk-shell fullscreen, tiling WMs) propose their
   size exactly there. The platform layer's size updated while the
   window layer kept laying out at the creation size — 320x240 of
   UI islanded in the top-left of a 1024x640 framebuffer, the rest
   zero-black. The first configure now emits the event whenever it
   actually changed the size; a first configure that kept the size
   stays silent as before.

3. **Suite hardening exposed by running under a second compositor.**
   The Wayland integration suite's decorations section replaced its
   honest-UNSUPPORTED skip with a hard assertion (set_decorated
   must succeed on EVERY compositor) plus pixel verification of the
   band; the clipboard section gained a seat-less honest skip
   (kiosk-shell weston advertises wl_data_device_manager but has no
   wl_seat, so no data device can exist — sway's seat still runs
   the full assertions).

4. **Client resizes under compositor-owned geometry were a
   protocol error.** The weston rig's suite run died mid-way with
   `xdg_wm_base error: xdg_surface geometry (512 x 760) is larger
   than the configured fullscreen state (1024 x 640)` — and every
   check after the kill "passed" vacuously on the dead connection
   (plus a phantom proxy leak at exit). Kiosk-shell weston
   configures EVERY window fullscreen; FDK only tracked MAXIMIZED
   and ACTIVATED from the states array, so a client-driven
   fdk_window_resize committed a non-configured buffer — legal for
   a floating or tiled surface, fatal under fullscreen/maximized
   where xdg-shell requires the configured size. FDK now tracks
   FULLSCREEN from configure states and refuses the resize with a
   warning while either state is set (public API gains no
   fullscreen concept yet — the flag exists purely as the gate).
   Parked refinement: binding xdg_wm_base >= 4 and parsing the
   TILED_* states would let client resizes through under tiling
   WMs, which the spec explicitly permits for tiled surfaces. The
   seat-less xdg_popup.grab marshal error (NULL seat marshalled a
   NULL object — libwayland logs and drops the request) is now
   guarded: no seat, no grab — a popup without a grab is valid
   protocol.

5. **The fontconfig "cache temperature" leak was FDK's own LSan
   bracket, not the weather.** Both suites intermittently failed
   ASan at exit with 320 B leaked entirely inside libfontconfig —
   previously waved off as cold-cache noise (Task 22) because it
   failed-then-passed on identical trees. A minimal probe (three
   font loads, no display, no compositor) reproduced it
   deterministically: fontscan's __lsan_disable/__lsan_enable
   bracket covered only FcInit, while the design comment — and the
   actual allocations — cover the first
   FcConfigSubstitute/FcDefaultSubstitute/FcFontSort walk, which
   builds fontconfig's process-lifetime pools in malloc'd memory
   exactly when the on-disk cache is cold (a warm cache serves the
   structures mmapped, allocating nothing — the nondeterminism
   explained). The bracket now covers the whole discovery call,
   matching the documented intent; the fontconfig quirk note in
   testing.md is rewritten to the resolved truth.

Verified with a new rig class: weston 14 kiosk-shell headless — a
REAL compositor that ships no xdg-decoration (the exact Muffin
condition) — runs the integration suite, the decorations demo's
full auto cycle under WAYLAND_DEBUG (zero zxdg_decoration traffic,
zero set_decorated failures, configures + commits counted), and a
screenshot of the final decorated state (fullscreen band,
pixel-checked). The control run (a git-worktree build of pre-fix
9eed9a2 under the same weston) reproduces the user's exact line and
instant exit — the probe sees the bug it guards. Battery: clean
rebuild debug + release (Wayland on, 0 warnings), headless suite,
X11 integration suite (leak-clean, LSan fully on), Wayland
integration suite + full rig on sway (manager-present, protocol
negotiation unchanged: 3 set_mode requests in the auto cycle), the
weston manager-less rig + pre-fix control — now with LeakSanitizer
fully enabled on BOTH compositors — and the openbox real-WM rig
(all six checks green) — ALL PASS.

### 1.1.6 — Wayland buffer starvation + the missing resize cursor (the second Cinnamon Wayland report) — COMPLETE

The follow-up report from the same Muffin (Cinnamon experimental
Wayland) session as 1.1.5, with a full log: the demo now survives
and works, but (1) `[WARN] all 4 render buffers in flight
(compositor not releasing?) — refusing new acquisition` fired ~80
times per session in bursts, (2) the UI "feels slower than X11" on
the same old laptop, and (3) the edge-hover resize cursor that
1.1.4 gave X11 was simply absent on Wayland.

Root cause (1)+(2): a protocol-legal slow compositor. Muffin is
allowed to take >250ms per present under load; when it did, the
frame-pacing starvation guard (FDK_WL_FRAME_GUARD_MS) declared the
window "ready", the app painted, acquisition found all four render
slots unreleased and REFUSED — dropping the frame and WARNing,
once per loop pass (~15ms), for the whole burst. Maximize toggling
amplified it: every configure step changes the buffer size, and
the pool's wrong-size slots could not be reaped while unreleased,
so resizes exhausted the pool even faster. Slow frames + dropped
frames = "slower than X11".

The fix, in four parts (wayland_window.c / wayland_dispatch.c):
  - A DEDICATED wl_event_queue for every wl_buffer's events
    (wl_proxy_set_queue at creation). wl_buffer::release is often
    the only event the render path needs; on its own queue it can
    be dispatched without running unrelated listeners — which is
    what makes the next two parts safe from inside a listener
    (the dispatch tail's synchronous resize repaint).
  - WAIT, DON'T REFUSE: a full pool now blocks up to
    FDK_WL_RELEASE_WAIT_MS (100ms) in 10ms steps — flush, dispatch
    buffered releases, prepare_read/poll/read_events, repeat —
    before even considering a refusal. The pre-fix behavior on the
    user's machine (drop + WARN per pass) becomes "delay one frame
    and land it".
  - WRONG-SIZE REAP AS LAST RESORT: after the wait, an unreleased
    slot at a different size is destroyed and its slot reused
    (each buffer owns its memfd pool; destroying a committed wl_shm
    buffer is legal — the compositor keeps its mapping). Interactive
    resize churn can no longer wedge the pool.
  - frame_ready()'s starvation guard now also requires POOL
    CAPACITY: "ready" while every slot is in flight was the pacing
    lie that walked the app into the refusals. The WARN itself is
    rate-limited to once per 2s episode.

Fix (3): the resize cursor is now real on Wayland —
src/platform/wayland/wayland_cursor.c implements window_set_cursor
with a hand-rolled XCursor theme loader (no libxcursor, mirroring
the X11 backend's cursor-font choice): $XCURSOR_PATH or the
libxcursor search roots, Inherit chains via index.theme
("Inherits=" — the key has an s), the "default" theme fallback,
closest-size image pick, ARGB upload over wl_shm, one cached
cursor surface per connection, wl_pointer.set_cursor citing the
live input serial. The container format was verified byte-by-byte
against Debian's Adwaita files: all fields LITTLE-endian (magic is
the literal bytes "Xcur"), version is NOT validated (Adwaita
writes 65536, xcursorgen writes 1), and the image chunk header
counts 9 CARD32s (header=36; pixels begin at chunk+header — proven
contiguous by Adwaita's own TOC arithmetic). A machine without any
cursor theme degrades honestly to the compositor's default arrow
(one DEBUG line per miss).

Two more fixes the new cursor test forced out:
  - pointer_enter (wayland_seat.c) dispatched FDK_EVENT_POINTER_
    ENTER with its position fields unset (0,0) — the window layer
    hit-tests ENTER through the resize compass, so every Wayland
    entry armed the NW cursor and seeded hover at the top-left
    corner until the first motion. The X11 backend had always set
    these; Wayland now does too.
  - fdk_init_options.app_id unset left every Wayland toplevel at
    the generic "fdk.app" (the user's log showed app_id=(none) in
    core's line and the placeholder on the wire). connect() now
    derives the id from /proc/self/cmdline's first token (argv[0]
    basename): demos identify as "06_decorations" in taskbars and
    compositor window rules.

Verified: the sway rig grew three pacing assertions (full pool +
unacknowledged frame -> frame_ready says wait; acquisition against
a full pool waits and lands; pumping recovers) and a cursor section
driven by the REBUILT virtual-pointer injector (the original was
lost to a session reset; the recreation adds a ready-file
handshake so compositors without zwlr_virtual_pointer — kiosk-
shell weston — honestly skip instead of SIGPIPEing the suite, and
discovers the output extent from wl_output instead of assuming
1280x800 — the headless output is 1280x720, and the 0.9 Y-scaling
of that wrong constant was traced through an 11.5px popup miss).
Protocol evidence is asserted in the suite's WAYLAND_DEBUG trace
(>=2 wl_pointer.set_cursor with real serials) and the demo's
(xdg_toplevel.set_app_id "06_decorations"; zero "buffers in
flight" WARN lines anywhere). The pre-fix control (worktree at
e0e2630) fails exactly the app_id and set_cursor checks — the
probes see the features. The rig also restored its lost sway
for_window floating rule (interactive coordinates assumed it since
the injector died) and fixed a latent suite UAF (the menu section
destroyed its font while the opener button borrowing it still
lived — the pacing section's configure-repaint detonated it under
ASan). Battery: clean rebuilds debug + release (Wayland on,
0 warnings), headless suite, X11 integration suite (green; no X11
code touched this milestone), Wayland integration suite + sway
rig PASS (5 new [ok]s), weston manager-less rig PASS, sway
examples rig PASS (8/8 pixel-verified via grim, restored to the
prefix). The openbox real-WM rigs were not rerun: openbox was
lost to the environment reset and no X11-side line changed — the
in-suite X11 regressions (which ran green) pin that surface.

### 1.1.7 — the resize-churn stall + the titlebar edge that moved instead of resizing (the third Cinnamon Wayland report) — COMPLETE

The 1.1.6 re-test from the same Muffin (Cinnamon experimental
Wayland) session: the WARN storm and the missing cursor are gone,
but (1) interactive resizing is "still laggy", (2) "the CPU is
not being fully used when resizing" — the loop idles while the
window stutters — and (3) a NEW affordance lie: pressing the top
edge over the titlebar, where the 1.1.6 cursor correctly shows
the N-resize shape, GRABS the window and starts a MOVE ("it grabs
it even tho the cursor to resize shows").

Root cause (1)+(2): the 1.1.6 release wait exits ONLY on a
release at the CURRENT size — during an interactive resize every
busy slot holds a buffer at a WRONG size (each configure step
changes the target), so no release can ever match the exit
condition: the wait burns its full 100ms budget per frame in
10ms poll slices and THEN the wrong-size reaper fires anyway.
~10fps during resize with the CPU asleep in poll — both live
symptoms, one cause. Fix (wayland_window.c): the budget is now
CLASSIFIED before waiting. Pure hoarding (every busy slot at the
current size — a release there is recyclable and worth the full
FDK_WL_RELEASE_WAIT_MS) keeps the 1.1.6 contract; churn (any busy
slot at a wrong size) gets ONE bounded poll slice
(FDK_WL_CHURN_WAIT_MS = 10ms) before the reaper, because every
exit from a churn wait ends in a reaper or a fresh allocation
regardless. Inside the wait, ANY release is now progress: a
right-size release recycles, a wrong-size release is reaped on
the spot and its slot reused (previously ignored until the
deadline — the wait outlived its own usefulness).

Root cause (3): an ordering bug in the shared press filter
(window.c, present since the 1.1.3 interactive work but only
REACHABLE on Wayland since 1.1.6 gave the backend a resize
cursor to lie with). The filter ran the FDK-fallback's origin
gate ("origin-moving edges need window_get_position") BEFORE
trying the compositor-driven begin_resize — and the Wayland ops
table has no window_get_position at all, so every origin-moving
edge (N, NE, NW, W, SW — the top edge over the band and the
whole left edge) dead-ended out of the resize filter. The press
fell through to the deco band, whose own handler starts a MOVE.
X11 was never bitten because both ops exist there. Fix:
begin_resize is tried FIRST (it needs no origin — xdg_toplevel
.resize / _NET_WM_MOVERESIZE carry an edge and a serial, and the
compositor owns the geometry from there); the origin gate now
guards only the FDK-driven fallback it was designed for. The
cursor affordance got the matching predicate
(window_edge_needs_origin, shared by press and cursor paths): a
backend that can neither hand off nor compute origins no longer
ADVERTISES its origin-moving edges — the cursor can never again
promise what a press cannot deliver.

Verified: the sway rig's suite grew a top-edge press regression
(inject a real tap at the N edge over the band; the rig's
WAYLAND_DEBUG assertions require >=1 xdg_toplevel.resize and
ZERO xdg_toplevel.move in the suite trace) and a resize-churn
block (exhaust the pool at size A, resize to size B inside the
measured call: the acquisition must complete under a generous
ceiling and the window must settle — or honestly skip under
compositor-owned geometry, kiosk-shell's fullscreen). The pre-fix
control (worktree at eacc8c9 + the new probes) demonstrates the
bug exactly: move=1, resize=0 on the injected top-edge press.
Honest limitation, documented in testing.md: sway releases
buffers within a frame, so the pool is credited BEFORE the
acquisition wait starts — pre-fix and post-fix converge to a few
ms on that compositor and the churn wall-clock assert is a
ceiling tripwire, not the discriminator; the 100ms-vs-10ms
difference only bites on compositors slower than the release
window (Muffin under load; the user's re-test is that control).
Battery on the final tree: clean rebuilds debug + release
(Wayland on, 0 warnings both), headless suite, X11 integration
suite (the press reorder is shared code — green), sway rig PASS
(set_cursor=3 now: edge + default + top edge; resize=1, move=0,
refusals=0), weston manager-less rig PASS, sway examples rig
PASS 8/8, openbox real-WM rig ALL 6 PASS (reinstalled after the
reset — "edge resize" and "drag" pin the X11 side of the
reorder), resize-flash rig PASS (pre-fix control still
reproduces), X11 examples rig PASS.

### 1.2.0 — real-world capability validation: clipboard showcase, drag and drop, file dialogs, the file manager

The directive from the fifth user report was a pivot, not a fix:
stop polishing the (working) resize/drag subsystems and prove the
toolkit can do REAL desktop work — clipboard, drag and drop, file
and folder selection — through real GUI applications, tested
against real external applications, on both backends. Do not
invent success: every capability ships with what was actually
verified and how.

What already existed (Phase 9): the text clipboard on both
backends (ICCCM CLIPBOARD / wl_data_device set_selection), the
message dialog, and the full widget catalog including Entry with
selection + Ctrl+X/C/V. What 1.2.0 added:

  - DRAG AND DROP, both directions, both backends
    (fdk_dnd.h + src/window/dnd.c + src/window/dnd_uri.c +
    x11_dnd.c + wayland_dnd.c). Receiving: a window registers
    FDK_DRAG_FORMAT_TEXT / URI_LIST; FDK negotiates (XDND v5
    Enter/Position/Status/Leave/Drop/Finished on X11;
    wl_data_device offers + set_actions + accept + finish on
    Wayland), decodes file:// URI lists to POSIX paths through
    ONE shared codec (dnd_uri.c — parse, percent-decode, build,
    escape; headless-pinned in test_dnd_logic.c), and delivers
    DRAG_ENTER/MOTION/LEAVE/DROP to the window callback with the
    payload valid for the callback's duration. Sending:
    fdk_drag_begin starts a drag from inside a press handler
    (X11: pointer grab + tree-walk target tracking inside the
    ordinary dispatch — no nested loop; Wayland: start_drag with
    the press serial, NULL icon) and reports SUCCEEDED/CANCELLED
    exactly once. Two real bugs ASan/tests caught on the way: the
    *len++ pointer-increment in the uri builder, and the FOLDER
    dialog kinds descending on Open instead of accepting.

  - FILE / FOLDER DIALOGS (fdk_dialog.h + file_dialog.c). FDK
    has no portal to defer to, so the dialog is a real FDK window
    — Up / hidden-toggle / path bar / scrolling list / Open +
    Cancel / status — browsing with real opendir/readdir scans
    (dirs first, alphabetical, hidden behind the toggle; the scan
    is the headless seam in widgets_internal.h). The result
    model is explicit: ACCEPTED with paths[] (count preserved —
    multi-selection never silently discarded), CANCELLED, ERROR;
    folder kinds stat()-verify every path IS a directory at
    accept time (a listing is a snapshot; the filesystem is not).
    File kinds descend into a lone selected directory (the
    standard affordance); folder kinds treat Open as SELECT.

  - LIST ROW ACTIVATION (list.c): double-click and Enter fire
    on_row_activate — the "open this" gesture both new apps are
    built on; the dblclick uses the shared window predicate
    (400ms/slop policy), and a slow re-click asserts it does NOT
    re-fire.

  - THE TWO REAL APPLICATIONS:
    examples/09_capabilities.c — one app answering "what can FDK
    do with the desktop?": a clipboard section (entry + copy/
    cut/paste/clear + narrated last operation), a drop target
    panel that highlights during drags and reports the drop
    (text or files-with-paths, folders classified by stat), four
    file-dialog buttons with visible accepted/cancelled/error
    outcomes, and a drag-source panel that drags text + two real
    files into other applications. RIG:/PHASE: lines throughout.
    examples/10_file_manager.c — a small but real file manager:
    places list, Up/Refresh/Hidden toolbar, ellipsized path bar,
    multiple-selection file list (click, ctrl+click, shift+
    click, shift+arrows), double-click/Enter navigation, and a
    status bar that always reports the selection (one item with
    its full path, "Selected N items", or the folder's path).
    Deliberately NOT a file manager suite: no previews, search,
    or file operations — the "can FDK build a credible browsing
    app" proof the directive asked for.

  - EXAMPLES CONSOLIDATION AUDIT (the directive's demand, and
    its honest outcome): all eight prior examples each teach a
    distinct layer (first-contact, renderer, text stack, layout
    + basic catalog, theming, decorations + CSD, advanced
    popup-owning widgets, accessibility) and every one is
    pixel-verified by the examples rigs — none demonstrates
    "essentially the same thing" as another, none is a toy
    animation pile (the renderer/text demos freeze when idle and
    exist to teach damage tracking), so nothing was merged or
    removed. The two NEW examples are the consolidated
    capability apps the directive asked for (no clipboard_test/
    clipboard_test2/file_test sprawl — one capabilities app, one
    file manager), and the count went 8 -> 10 with breadth, not
    redundancy.

INTEROP, how it was actually verified (the directive: real
external applications, not FDK-to-FDK):

  - X11, external -> FDK: scripts/xdnd_source.c — a RAW-Xlib
    client (no FDK) performing a complete XDND drag into the FDK
    window; the suite asserts the FDK side decoded 2 files to
    POSIX paths and the source saw Status accept=1 + Finished
    success=1.
  - X11, FDK -> external: scripts/xdnd_sink.c — a plain mapped
    Xlib window speaking XDND; the suite drives a REAL pointer
    drag (XTEST, human-paced steps) from an FDK press through
    fdk_drag_begin into the sink; the sink prints BOTH decoded
    payloads (uri-list + text) and FDK reports SUCCEEDED.
  - Wayland, external -> FDK: scripts/wl_dnd_source.c — a raw
    libwayland client (generated xdg-shell protocol object, zero
    FDK linkage) that starts a wl_data_device drag on a real
    injected press; the sway rig drags from its surface onto the
    FDK window and asserts ENTER + DROP + 2 decoded paths + the
    source's ::send + receive->finish->destroy in FDK's trace.
  - Wayland, FDK -> external: NOT covered by an external-client
    rig this milestone (the source op is exercised end-to-end by
    the codec/suite and the X11 interop pins the shared payload
    shape); documented as the honest gap it is.

Honest capability matrix (what ships, no more):

  clipboard        text only, both backends (Phase 9 + entry
                   integration); no PRIMARY, no INCR, no images
  dnd receive      text + uri-list -> POSIX paths, both backends,
                   window-level targets (widget-level = future)
  dnd send         text + uri-list, COPY action only, both
                   backends; X11 verified against a real external
                   client, Wayland against the shared codec + a
                   wlroots quirk below
  dnd wayland      wlroots 0.18 ends the SOURCE side with
                   ::cancelled after a clean post-drop
                   receive->finish->destroy (drop + payload are
                   correct; the result signal is the quirk) —
                   suite-asserted both-ways-end, documented
  file dialogs     open file / files / folder / folders; the
                   four kinds share one honest result model;
                   no SAVE dialog (parked — nothing fake)
  file manager     browse/select/multi/navigate/keyboard/scroll;
                   no previews/search/operations (deliberate)

Rig lessons that cost an hour each, recorded: a blocking fgets
on a child that goes silent mid-handshake starves the pump (all
child drains are non-blocking now); the XTEST driver's startup
self-check MOVED THE POINTER, thrashing drag targets (self-check
is opt-in via FDK_XTEST_SELFCHECK=1); sway grants the seat's
pointer capability only when the virtual pointer produces its
FIRST event (a priming motion after injector start is part of
the DnD section now); a fresh client connecting after the
virtual pointer exists still sees caps=0 (spawn order: client
first, injector second — get_pointer before any capability is a
protocol error); wlroots delivers ::drop only when the offer
also got the legacy accept(mime) — set_actions alone negotiates
the action but leaves the source unaccepted and a release then
cancels; XDND's Finished must set the success bit (v2+) or the
dragging app reads the drop as failed.

Battery on the final tree: clean rebuilds debug + release
(Wayland on, 0 warnings), headless suite (two new test files:
dnd logic — codec round trips, hostile input containment,
argument safety; file-dialog logic — scan filtering/ordering,
unreadable dirs, ownership), X11 integration suite (four new
GUI sections: list activation, file dialog accept/cancel/
folder-button, dnd receiver external files+text, dnd source
real-pointer external sink), sway rig PASS (incl. the Wayland
dnd interop section), weston manager-less rig PASS, openbox
real-WM rig ALL PASS, resize-flash rig PASS (pre-fix control
still reproduces), X11 examples rig 10/10, sway examples rig
10/10 (both new apps pixel-captured).

### 1.2.1 — the stale-paint class fix, the prompt dialog, and the file manager that earns the name

The sixth user report was two findings in one breath. First, a
rendering class: on Wayland "the old text doesn't get removed" —
file names stamped over old file names after navigation,
selection bands stacked on selection bands, status lines
overprinting their previous sentence. Second, a product judgment:
the 1.2.0 file manager was "too simple to be the default", and
the capabilities app lacked the one thing a typing-capable
toolkit owes a demo — a box you type into.

ROOT CAUSE (the rendering class, and why it looked Wayland-only):
every window root painted NOTHING unless the app set its own
background, and the stock text surfaces (Labels, List rows) are
deliberately transparent — they paint glyphs over whatever the
retained framebuffer already holds. Both backends are retained-
buffer damage models (X11: synced front/back XImage slots;
Wayland: prefetch-visible-frame buffer recycling), so "whatever
the buffer held" is the previous frame: a damaged region redraws
new text over old pixels. Examples 03/04/08 had dodged the trap
by setting their own root background; 09/10 exposed it. It was
never Wayland-specific — it was reachable on both backends
everywhere a root went unfilled (the report landed on Wayland
because that is where the user lives).

The fix, in layers:

  - ROOT DEFAULT BACKGROUND: fdk_window_get_root fills the root
    with the theme's window-background token; any damaged region
    is freshly cleared before its widgets draw. An explicit
    fdk_widget_set_background on the root is an override that
    survives theme switches (FDK_WF_ROOT_BG_DEFAULT cleared
    there); otherwise the root's theme hook re-reads the token on
    every default-theme switch, same contract as the palette.
  - PRE-FIRST-FRAME COLOR: both backends' creation-time fill (X11
    background pixel, Wayland's committed background buffer) is
    the themed window background instead of white — the frame
    before the first paint and the first painted frame now agree
    (no white→dark flash at map).
  - THE EXAMPLES' MISSING PAINT: 09/10 never called
    fdk_window_paint in their loops — after the initial frame
    they only updated on resizes (and the resize-time repaint is
    where the accumulated damage overprinted). Both now paint
    when the tree has damage, the documented app pattern
    (04_widgets.c).

The new core API — fdk_dialog_show_prompt (fdk_dialog.h): the
message dialog's text-input twin. One question, one Entry
(initially focused, prefilled value starts SELECTED so typing
replaces it), OK/Cancel, Enter-in-the-entry answers OK,
Escape answers CANCEL (an Entry with an active selection
collapses it first, then a second Escape bubbles — the stock
Entry no longer eats a no-op Escape). The answer contract is
explicit like the file dialog's: OK carries the text (valid only
during the callback), everything else carries NULL.

The file manager, rewritten as FDK Files (10_file_manager.c):

  - toolbar: Back / Forward (real history stacks, capped) / Up /
    Refresh / New Folder / Rename / Delete / Hidden
  - location bar: an EDITABLE Entry — type any path, Enter goes
    (~ expands, stat-verified, honest error otherwise)
  - filter box: re-filters the listing as you type
  - sorting: Name / Size / Modified combo + Ascending/Descending
    toggle; directories always group first
  - listing columns: Name + Size + Modified (ellipsized names,
    human sizes, local dates); multiple selection with the full
    click/ctrl/shift/keyboard set; double-click or Enter enters
    a folder, opens a file through xdg-open when present
    (honest status when not)
  - file ops: New Folder and Rename through the PROMPT dialog;
    Delete confirms with YES/NO then unlinks files and removes
    EMPTY directories — recursive delete is deliberately absent
  - status bar: items (+hidden count), selection count and total
    size, filesystem free space (statvfs), sort state
  - keyboard: Backspace=Up, F5=Refresh, Ctrl+H=Hidden,
    Alt+Left/Right=history, Escape=quit

Two app bugs the rigs caught before they could ship: fdk_list_
clear fires the selection callback per removed row MID-RELOAD
while the app's parallel arrays are half-swapped (a loading guard
now brackets every reload), and the store was qsort-indexed for
display while the lookups used display rows against readdir
order — the store is now PERMUTED into sorted order so display
row == store index everywhere.

The capabilities app (09_capabilities.c) gained the TEXT INPUT
section — a big box you simply type into, with a live line
(bytes / caret / selection from the public Entry APIs), a
committed line on Enter, and Password / Read-only mode toggles —
plus a fuller clipboard section (Read clipboard pulls foreign
content into a preview line; Set greeting plants a timestamped
string for other apps) and an Ask Yes/No message-dialog demo.
The window reflows on resize now.

Verification (rigs under scripts/, captures under download/):

  - sway headless + wlr-virtual-pointer (verify_fm_wayland.sh):
    A) navigate into a smaller directory — 0 stale ink pixels
    below the new listing; B1) the selection band renders (~7k
    band pixels); B2) moving the selection leaves 133 text-AA
    pixels where ~6.6k band pixels were; C) a status label's
    long text fully clears before the short one (0 stale px
    right of the new text, 1.5k while the long text is up).
    ALL PASS.
  - Xvfb + openbox + xdotool, keyboard included
    (verify_fm_x11.sh): navigation, selection, New Folder typed
    through the prompt, Rename through the prompt, Delete via
    Enter-on-Yes, filter-as-you-type, location-bar navigation,
    F5/Alt+Left shortcuts — every step console-asserted AND
    verified on disk (folder created, renamed, gone). The typing
    playground's live line and pixels verified. ALL PASS.
  - headless suite + both integration suites: all green on the
    dual-backend build (0 warnings).

Honest gaps: Wayland keyboard injection in the rigs (no virtual
keyboard protocol in the sway rig — typing verified on X11
only); xdg-open coverage depends on the host; the listing is
single-column text (no icon view, no renaming in place); delete
is non-recursive on purpose.

### 1.2.2 — the resize backlog wedge (the first X11 report: "after multiple resizes it doesn't update anymore and the cpu goes insane on one core non stop")

The seventh user report, X11 this time ("no idea if its the same
thing wayland"): expanding the window sometimes "takes a long
time to update and the cpu goes insane, only one core gets fully
utilized", and after several resizes "it doesn't update the
window anymore and the cpu goes insane on one core non stop and
the title bar buttons stop working".

ROOT CAUSE (found live with a signal-sample profiler on an
instrumented build under Xvfb + a 480-resize storm driver): an
interactive resize queues one ConfigureNotify per drag step
(plus an Expose each) server-side before the application pumps
once — several hundred events for a multi-second drag. The 1.1.4
synchronous resize repaint ran INLINE in the dispatch tail of
EVERY one of them: a full-window widget repaint + a framebuffer
pair reallocation (shmget/XShmAttach per step) + an XSync round
trip for the in-flight slot + an XQueryPointer round trip for
hover revalidation — per queued event, at stale sizes. The drain
rate (~15 events/s under that load) fell below the WM's queueing
rate; the `while (XPending)` loop walked a backlog that never
shortened, at 100% of one core. Every symptom maps directly: the
window "doesn't update" (it paints stale sizes), the CPU "goes
insane non stop" (the backlog walk), the "title bar buttons stop
working" (their clicks are queued BEHIND the backlog). The
profiler stacks pinned it: blend_pixel <- fill_rect <- base_paint
<- tree paint <- fdk_window_paint <- dispatch tail, 30/30
samples, dispatch_pending itself entered ~0 times/s — the loop
was inside ONE dispatch call the whole time.

FIX (the shared window layer, both backends): the geometry
fallout tail (hover revalidation + synchronous repaint) is now
BATCHED. Configure/state-flip/first-expose events set a window
flag (`geo_repaint_pending`); the geo bookkeeping itself (stale
framebuffer drop, root resize, band arrange, content reflow)
still runs inline — cheap, idempotent, keeps get_size()
authoritative mid-batch. `fdk__window_flush_geo_repaints()` runs
from the pump the moment the backend's dispatch_pending drains
the queue (both drain sites + fdk_run's initial drain): one
revalidate + one repaint per flagged window per BATCH, at the
batch's FINAL size. A lone configure still repaints within its
own pump call — the 1.1.4 sub-frame gap contract is preserved,
just once per batch instead of once per queued event. Wayland's
protocol flow is untouched (ack_configure stays inline in the
backend; the commit moves by microseconds, still the same
event-loop turn).

VERIFIED (all on the final tree):
- The 480-resize storm (the repro driver: XResizeWindow from a
  second connection at 2ms/step): pre-fix 88-100% of one core
  burning forever after the storm with the loop wedged inside
  one dispatch; post-fix 0.00s CPU one second after storm end,
  clean exit on WM_DELETE_WINDOW. The 1800-resize variant: 26%
  of one core DURING the storm (the legitimate ~60fps live
  repaint), 0.00s after.
- Visual: after a 1200-resize storm the window is pixel-identical
  to the pre-storm reference frame (repainted correctly at the
  final size); the file manager example storms to 1200 resizes
  with 0.06s CPU over the next 3s, no wedge.
- NEW IN-SUITE REGRESSION (test_x11_integration.c,
  test_resize_storm_backlog_drains): 300 configures queued from a
  second X connection must drain with the final size reached,
  fresh pixels at the final size's far corner (readback through a
  separate connection), and a NEW resize after the storm still
  processed in one pump — liveness, the wedge's exact symptom.
  Post-fix it drains in ONE pump call (300 configures
  coalesced); pre-fix the same loop could not finish in any
  plausible budget.
- test_grid_layout_gui's four gap-pixel assertions were stale
  against 1.2.1's root default background (expected raw server
  black; the gap has shown the themed window-background
  0x121721 since 1.2.1) — they now read the token through
  fdk_theme_get_color. Found because this session actually ran
  the suite; a pre-existing failure on 1.2.1, not a regression.
- Headless suite (debug, ASan/LSan, zero leaks) and the full X11
  integration suite (77 [ok]s) green; the Xvfb examples rig
  passes 8/8 pixel-verified (its 01 check now expects the 1.2.1
  themed background pixel instead of pre-1.2.1 white).

Honest gaps: the Wayland side could not be live-tested this
session (the sandbox lost its libwayland toolchain and
compositors to an environment reset, and the apt network is
unavailable to rebuild them) — the change lives in the shared
window layer + context pump, compiles in the X11-only build, and
the Wayland configure path was code-reviewed against the
ack-then-commit contract (the 1.1.5/1.1.6/1.1.7 fixes). The
Wayland suites + rigs must run once the toolchain is back.

### 1.2.3 — the file dialog earns "release quality" (SAVE, places, filters, path bar — and the List that never painted)

The standing release-quality audit for the file manager family.
The 1.2.0 dialog could open files and folders; a desktop toolkit's
dialog does more, and every piece below is real, tested logic:

- SAVE_FILE (fdk_dialog_save_file, or kind=SAVE): a Name row
  seeded from start_name (selected, rename-convention), filled by
  single-clicking a listed file, Enter/Save to accept. Validation
  is honest and total: non-empty, no '/', not "."/"..", <= 255
  bytes, whitespace-only refused, the parent directory still
  existing — every failure is a status-line message, never a
  silent wrong answer. An existing REGULAR target gets a nested
  Yes/No overwrite ask (a toolkit-owned message dialog on the
  same context; declining returns to the dialog — it does not
  cancel); a directory target is refused; so are fifos/sockets.
  The accepted path canonicalizes the parent (realpath) and keeps
  the leaf exactly as typed — no extension guessing. On X11 the
  ask takes the modal grab and the file dialog RE-TAKES it when
  declined (the grab is re-established, not assumed).
- Filesystem discovery, the Places sidebar: pure POSIX (getenv +
  stat + /proc/self/mounts + one level of /media and /mnt) — no
  udev, no D-Bus, per the no-bus policy. $HOME, conventional XDG
  dirs when they exist, /, real-filesystem mounts (whitelisted
  types; pseudo filesystems are noise), media/mnt entries;
  stat()-verified at discovery, deduplicated by canonical path,
  capped. fdk__fs_discover_places is the headless seam.
- Name filters (options.filters, ";"-separated globs): one combo
  row per pattern plus "All files", first pattern active; '*'
  and '?' with case-insensitive matching (the GTK convention);
  directories are never filtered (navigation must always work).
  fdk__file_dialog_glob_match is the seam.
- The path bar became an Entry: type a location, Enter browses —
  absolute, "~"-expanded, or relative to the current folder;
  failed browses never abandon the working directory (the target
  is probed first; the reason lands in the status line).
- Boundary hardening: every path is built with fdk__path_join
  (allocated, untruncated) instead of fixed 4096 buffers; browse
  normalizes trailing slashes; Up works on a copy; the accept
  path re-stats everything (listings are snapshots).

THE BIG ONE — found by the 1.2.3 dialog rig, fixed in the
toolkit: the List widget never painted when it was sized AFTER
its last append (or appended-to while still 0x0).
fdk_widget_set_bounds() is pure geometry — it does not run
arrange hooks — and the List only synced its internal scrollview
inside the append path. A hand-positioned list kept a 0x0/stale
scrollview; the paint walk skips empty children, so every row
stayed invisible forever. FDK's own 1.2.0 dialog worked only by
accident of ordering (its reload appended rows after set_content
sized the list). Fix: a list_paint hook lazily syncs the
scrollview before the subtree is walked (one compare when in
sync; the heal damages the region, which schedules one more
identical frame and converges).

Verification: headless logic suite (glob matching incl.
case-insensitivity and '?' semantics, filter parsing incl.
whitespace trimming and empty fragments, filtered scans incl.
"dirs never filtered", discovery invariants incl. stat-existence
+ dedup + HOME-first + cap, path helpers incl. ~ expansion and
root-join, save-name validation incl. the 255-byte boundary) —
all green, leak-clean. X11 integration: 8 dialog GUI tests
(keyboard OPEN accept, Escape cancel, OPEN_FOLDER button accept
stat-verified, SAVE fresh-name Enter-accept with the not-created
contract, overwrite ask DECLINED leaves the dialog up then
cancels, overwrite ask CONFIRMED accepts the existing path,
empty-name Enter never answers) — 81 [ok] total, plus the
pre-existing storm/grid checks unchanged. A pixel-scan rig
(row-band ink counts through a second X connection) verifies the
new layout paints: toolbar, path bar, places rows, filtered file
rows, name row, buttons — the filtered listing shows exactly
subdir/ + main.c under "*.c;*.h;Makefile", readme.txt correctly
hidden. The Xvfb examples rig passes 8/8 (the list_paint change
regresses nothing). Wayland build compiles with the restored
apt-prefix toolchain; live Wayland verification runs in this
milestone's battery alongside the clipboard interop work.

### 1.2.4 — the clipboard actually interops (two Wayland bugs, the X11 Latin-1 legs, and the injector reborn)

The standing release-quality clipboard audit: real cross-process
interop on BOTH backends, driven exactly like the desktop does it.

THE WAYLAND FINDINGS (both real bugs, both found by building the
interop rig rather than reading the code again):

1. The clipboard was dead until the first pump. Connect did ONE
   roundtrip (globals + binds) and returned with the seat's
   name/capabilities events still queued — so the wl_data_device
   (created on seat arrival) did not exist yet, and a set/get
   immediately after fdk_init answered FDK_ERR_UNSUPPORTED. The
   set-selection-before-any-input limitation was documented; THIS
   was a different, undocumented one: even WITH input, the first
   clipboard call before a pump lost. Fix: the standard
   two-roundtrip startup (roundtrip 1 collects globals, roundtrip 2
   collects the binds' replies — capabilities, and with them the
   data device, keyboard/pointer proxies).

2. Every clipboard set under wlroots self-destructed instantly.
   wlroots 0.18 echoes the freshly-set selection back to the
   KEYBOARD-FOCUSED client — the setter itself — and FDK's
   device_selection treated ANY incoming selection as "ours was
   replaced", destroying the source it had just registered. The
   compositor then saw the source die and cleared the global
   selection: readers always got selection(nil). sway's debug log
   showed no rejection — the request was honored and then un-done
   client-side. Fix: replacement has its own protocol signal
   (wl_data_source::cancelled); device_selection no longer touches
   our source, and source_cancelled now also drops the (possibly
   echo-wrapped) selection offer.

The rig that caught it: two independent FDK processes through sway
1.10 headless, with the REAL input prerequisites — a rebuilt
fdk-wl-inject (the zwlr_virtual_pointer_v1 + zwp_virtual_keyboard_v1
client, lost to session resets and recreated from the suite's
documented contract; the keyboard half matters because sway CLEARS
keyboard focus when the seat has no keyboard, and wlroots pushes
selections only to the keyboard-focused client — a windowless
reader never sees one). The setter sets from a real button event
(the Ctrl+C shape: set_selection with the press's serial); the
reader maps a window and is tapped into focus (the wl-paste trick).
Three directions verified: set/read, replacement set/read, and
multi-byte UTF-8 ("h\xc3\xa9llo w\xc3\xb6rld" crosses the pipe
exactly).

THE X11 ADDITIONS: the suite's foreign-owner/foreign-requestor child
processes grew the Latin-1 legs the ICCCM paths existed for — an
xterm-class owner that serves XA_STRING ONLY (FDK's fallback convert
widens 0xE9 to UTF-8), and a Latin-1 requestor reading FDK's
XA_STRING rendition (\xC3\xA9 -> 0xE9, and the em-dash honestly
'?'), plus a multi-byte UTF-8 round trip through the whole interop
path. 84 [ok] total.

BONUS (the injector's return): the Wayland suite's interactive
sections — menu clicks through the real compositor, the cursor
edge tests, the resize handover — had been silently [skip]ping for
want of the injector binary. All run and pass again (27 [ok] under
the interop rig's compositor; the DnD section still needs its own
drag-source helper, an honest skip).

Honest note: FDK's set_text still carries serial 0 before any input
event (compositors may ignore it — documented in fdk_clipboard.h);
the rig mints real serials exactly as a real desktop would. The
one-clipboard-per-context, text-only, no-PRIMARY scope notes all
stand.

### 1.2.5 — the shared example-window helper, the unified cursor hit-test, and the ten-example rigs

Two standing release-quality items landed here: the example-suite
helper (the "every demo hand-rolls its own window" debt) and the
query_pointer hit-test unification (the last X11/Wayland contract
divergence in the cursor/hover revalidation path). Plus the rigs
grew teeth: both example suites now run all TEN demos.

THE EXAMPLE HELPER (examples/example_window.h, header-only):

Every demo used to hand-roll the same boilerplate — fdk_init with
or without an app_id, a window titled however it felt that day, its
own close/Escape handling, its own pump loop. Ten programs, ten
spellings. The helper makes the suite uniform: fdk_example_init
(app_id "org.fdk.exampleNN" — taskbars and rig rules can address
each demo), fdk_example_open (the standard window: "FDK NN — name"
title, an in-window HEADER — name left, "Esc — quit" hint right,
hairline below, so the label survives in every screenshot even when
the WM titlebar is cropped — plus a content box and a status line),
fdk_example_pump (ONE loop pass: events, then a damage-gated,
frame-paced paint), fdk_example_run, fdk_example_close (window,
then the helper's font, then the context — and the "exited cleanly"
line the rigs grep). Quit semantics live in ONE place (WM close +
Escape flip ex->quit); demos needing window-level events chain
through the observe-only ex->on_event hook.

Two integration tiers, both real: FULL (open + content in
ex->content) for the box-layout demos — 01, 03, 04, 05, 08, the
narrator rebuilt as a box form whose narration sink IS the helper's
status line; INIT (init only, the demo owns its window) for the
demos whose subject IS the chrome or the raw framebuffer — 02 (a
widget frame would fight the direct surface writes), 06 (the FDK
band is the titlebar), 07/09/10 (manual-bounds playgrounds). The
09/10 app_ids normalized to the org.fdk.exampleNN scheme.

THE UNIFIED HIT-TEST (the last cursor-contract divergence):

window_revalidate_pointer (the 1.1.4 hover/cursor revalidation
after a geometry change) asks each backend one question: "is the
pointer inside the window's CURRENT geometry?" The X11 op always
answered with the bounds check (XQueryPointer's window-local
coordinates vs the last ConfigureNotify size). The Wayland op
answered from the seat's pointer_focus ALONE — so a window that
SHRANK under a stationary pointer (the unmaximize case the
revalidation exists for) kept answering "inside" with cached
coordinates outside the new size, and the revalidation routed a
synthetic out-of-bounds MOTION where the X11 backend delivers the
honest LEAVE. Fix: the Wayland op runs the identical check against
its last acked size — same coordinate space wl_pointer delivers,
both integer- and fractional-scale projection paths. Both comments
now state the contract in the same words ("nonzero only when the
pointer is within the window's current geometry").

The regression is load-bearing (proven against the pre-fix body):
the client resize path updates last_size synchronously while the
seat cache still holds the pre-resize position, so the suite
interrogates the op BEFORE any compositor round-trip — the stale
state is deterministic, the seam assert fails on the old code, and
the observable half (hover + cursor cleared by the leave
synthesis) follows after the flush.

THE RIGS GREW TEETH (and were caught lying twice):

- Both example rigs now run all TEN demos (09/10 were never in
  either; the X11 rig's dark-content check covers them, and the
  sway rig pixel-verifies their mapped windows).
- The sway examples rig BUILDS the examples first: it once
  verified a whole suite against stale PNGs after the clip-interop
  rig make-cleaned the tree in between (the Task-24 lesson,
  repeated).
- The Wayland test rig's sway config lost the floating pin
  ("org.fdk.test windows at (100,60)") the injector-driven
  coordinates are computed from — under tiling every click missed
  and the suite failed at the first injected button press.
  Restored, with a comment stating why it is load-bearing.
- grim (the capture half of every sway rig) is NOT part of sway's
  dependency closure: it silently vanished with the prefix, and
  the rig "passed" against the previous session's captures. The
  sway-restore script (rebuild_sway_rig.sh — the durable recovery
  path for the recurring /home/z/apt wipe) now fetches it too, and
  verifies sway actually starts headless.

Verification battery on the final tree: headless suite green;
X11 integration suite green; Xvfb example rig 10/10 pixel-verified
+ clean exits; Wayland integration suite 27 [ok] under sway
(including the new query-contract test, both halves); clipboard
interop rig three-directions green; sway example rig 10/10
pixel-verified on FRESH captures; release config zero warnings.
