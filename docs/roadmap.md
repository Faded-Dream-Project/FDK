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
