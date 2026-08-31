# FDK Testing

## Two test suites, deliberately separated

**`make test`** — platform-independent. Never requires a display of
any kind (X11 or Wayland). Safe to run in any CI container with zero
setup. Covers: allocation, version/error-string correctness,
`fdk_init()`'s behavior when no display is reachable at all (including
with a display explicitly requested — `FDK_PLATFORM_X11` /
`FDK_PLATFORM_WAYLAND` — to confirm there's no silent fallback), and —
since the Phase 3 second slice — the entire software renderer via
OFFSCREEN surfaces (`tests/test_render.c`): primitive geometry
(fills, gradient interpolation, Bresenham lines, circle chords,
rounded-rect corner cutouts and radius clamping), single-blend
border/arc invariants, the clip stack (nesting, empty intersections,
LIFO unwind, depth bound), damage bookkeeping (bounds, union,
clamping, overflow-to-full, the raw-write declaration contract),
blit semantics (full/partial/clipped/clip-stack/argument checks),
and offscreen lifecycle. Offscreen surfaces are display-independent
by design, which is exactly what makes this coverage headless —
their stride is deliberately padded so the stride-aware paths real
backends hit are always exercised.

**`make test-x11`** — X11 platform integration. Requires a reachable
X11 display. If `$DISPLAY` is already set when you run it, it tests
against that (a real desktop session, or an Xvfb/Xephyr you started
yourself). If not, it starts a throwaway Xvfb automatically, runs
against it, and tears it down afterward — no manual setup needed even
in a bare CI container, as long as `Xvfb` is installed.

There is a `make test-wayland` — the integration binary self-skips
when `$WAYLAND_DISPLAY` is unset, so plain CI never fails on it;
point it at a live compositor (the rig starts sway headless) and it
runs the real suite. See "Wayland test coverage" below.

## Why the split

Per project requirement: ordinary `make test` must not depend on the
developer having a graphical desktop session. `fdk_init()` in Phase 2
genuinely does connect to a real display, unlike Phase 1's stub — so
without this split, plain `make test` would fail in any headless
environment (which is most CI). Splitting the suite is the actual fix,
not faking a platform connection or skipping platform tests silently.

## What `make test` covers beyond the platform tests

The Phase 4 widget foundation is fully headless-testable by design
(standalone roots + offscreen surfaces), so `tests/test_widget.c`
runs in plain `make test` alongside the renderer suite: hierarchy
and destroy cascades (subclass destroy hooks counted), reparent and
z-order, effective visibility/enabled chains with input
pass-through, painting (z-order, parent clipping, hidden subtrees),
the partial-repaint PROOF (raw marker pixels outside the damage box
survive a repaint; the surface's recorded damage bounds equal the
invalidated widget's bounds), hover synthesis with local
coordinates, the implicit grab, bubbling with per-level coordinate
translation, focus lifecycle (drop on hide/disable, window
blur/regain), Tab traversal (wrap, override, modifier guard),
destroy-during-dispatch (self, ancestor, whole root — the deferred
free exercised under ASan), measure/arrange hooks, and inter-tree
isolation. 17 cases, all under ASan+UBSan.

## Text tests (Phase 6)

`tests/test_text.c` runs headless in plain `make test` (10 cases,
ASan+UBSan) and needs no display — only a system TrueType font
(DejaVu Sans / Noto Sans candidates; the whole suite honestly skips,
[X11-suite style](#known-xvfb-flakiness-investigated-and-fixed), when
the environment has none). Covered: font lifecycle and every failure
mode (missing file, garbage bytes, directories, out-of-range sizes —
including the sfnt container gate that keeps malformed fonts from
becoming out-of-bounds reads inside stb_truetype), metrics sanity and
2x scale proportionality, the system font discovery chain
(`fdk_font_load_system_default`'s env overrides, fontconfig
discovery driven end-to-end through a private `fonts.conf`, the
Arch variable-font filename scan, corrupt-candidate rejection, and
cache consistency), measurement (proportional vs monospace,
ink bounds, whitespace, byte_len slicing, and the pinned
round-of-sum vs sum-of-rounds behavior), draw/damage agreement (the
damage box is exactly the measured ink band), cache-hit determinism
(redraw is pixel-identical) and LRU eviction past 512 glyphs, clip
stack honoring (ink outside the clip untouched, damage clipped to
the visible span), and UTF-8 edge cases (invalid bytes to U+FFFD,
truncated sequences, unmapped codepoints, embedded NUL), greedy line breaking (every
line fits its width AND agrees exactly with measuring its bytes —
the agreement-by-construction contract; hard \n/\r\n breaks with
empty lines preserved; mid-word breaks for over-long words with
every glyph accounted for; trailing-space trimming; count-then-fill
two-pass calls; max_lines truncation flagging; argument safety), and
ellipsis (fits whole, no-fit prefix maximal — the next codepoint
provably overflows — codepoint-boundary and no-trailing-space
guarantees, degenerate widths below the ellipsis itself, argument
safety).

The X11 suite adds `test_text_render_readback`: 48px "FDK" drawn
into a mapped window and verified through the X server's own pixels
— ink on a mid-height scan is boxed by the measured metrics, only
ever adds light over the background (AA edges included), and nothing
bleeds left of the pen or below the ink band. Skips honestly when no
font exists.

## Widget catalog tests (Phase 6)

`tests/test_controls.c` (10 headless cases, ASan+UBSan, font-gated
honest skip): label geometry (natural size == measured text,
re-measure on set_text, ink confined to bounds), label modes (WRAP
natural height = line count at the requested width, narrower arrange
grows the count and wider collapses to one, wrapped ink spans
multiple line pitches inside the band; ELLIPSIZE natural = the FULL
text, narrow arrange truncates with right-end ink and clips at the
edge; START/CENTER/END alignment verified by ink extents at the
left edge, symmetric middle, and right edge), radio arrow-key
traversal (Down/Right select + focus the next member, Up/Left the
previous, wrap-around both ends, hidden and disabled members
skipped, a lone radio lets the arrows bubble to ancestors), button
interaction
(click-in activates, release-out does NOT, Space/Enter, disabled
ignores input, cross-type setters rejected), toggle + checkbox
(click/Space/programmatic, on_change firing, knob visibly moving
between states), radio groups (parent-scoped exclusivity through
both programmatic and click paths, on_change ordering, no-op
recheck), progress (clamps, exact accent fill widths at 0/50/100%),
separator (1px rule exactly on its line), frame (title band reserves
layout space, children stack below it, title ink paints, fontless
frame = plain box), and argument safety (NULL args, cross-type
confusion, no-op setters).

The X11 suite adds `test_widget_catalog_gui`: a Button + Toggle +
ProgressBar interface as the window's CONTENT, driven by REAL
injected clicks (XSendEvent through the server) — the button's
activation grows the progress bar with server-verified pixels at
each step, and the toggle flips state.

A second GUI case, `test_label_radio_arrow_gui`, drives the text
layout: a WRAP label's first two line bands verified through the
server's pixels (single-XGetImage region captures), nothing inked
past the label's right edge, an ELLIPSIZE label's ink clipped
exactly at its boundary, REAL arrow keypresses (keycode 116/111 =
scancode 108/103) moving radio selection AND focus with the accent
dot appearing server-side on the newly selected row, and a window
resize whose re-wrapped paragraph puts ink into the band that was
empty before — all through the same pump/paint loop production apps
run.

## Layout tests (Phase 5)

The layout engine is pure geometry over the widget hooks, so
`tests/test_layout.c` runs headless in plain `make test` (13 cases,
ASan+UBSan): box measure math (naturals + spacing + padding +
margins, homogeneous, orientation), arrangement math (packing,
expansion absorbing the leftover, cross-axis align/expand), margins
inside slots, dynamic relayout on child add/remove/hide/hint change,
nested child-change PROPAGATION (the regression case for the two
engine bugs the 07 demo found: a Frame relayouts when ITS children
change — box-ness by hook delegation — and ancestors re-run when a
nested container's natural changes, with the box setters reaching
subclasses), nested boxes, a pixel-agreement case (a laid-out tree paints exactly
into its computed slots), and argument safety — plus the completion
slice's four: GRID measure/arrange (track naturals, spans
distributing their deficit, growth on attach, expand tracks,
homogeneous, hidden children, cell align), the grid NOTIFIER
regression (attaching or changing a child re-packs without an
explicit arrange — the delegation rule again), SIZE LIMITS (min/max
clamped into every measure, max<min normalization, limits honored
through a real container's negotiation, argument safety), and
BASELINE (box cross-axis alignment against the group max, the
bottom-edge fallback for widgets without one, and a Label's reported
ascent). The window integration
(auto-reflow of the content widget on every configure) is the X11
suite's job: `test_widget_layout_reflow_on_resize` resizes a live
window and verifies SERVER-SIDE that the box reflowed — the fixed
header stays 40px, the expanding panel owns the new space, and the
boundary sits at the exact pixel layout computed. Destroying the
content widget and calling fdk_window_layout() must deactivate the
association cleanly rather than arranging a freed widget. The
completion slice adds the X11 GRID GUI case (tracks, a min-width
limit through a real container, expand column/row, a spanning cell,
gaps, and reflow on live resize — all server-readback) and the X11
BASELINE case (16px and 32px labels sharing one baseline row, their
ink tops/bottoms verified at exact pixels).

The Wayland side of the reflow contract runs in the sway rig (see
the Wayland section below): a client-side resize through a FLOATING
toplevel must re-arrange the tree and reach the screen — the case
that found the two resize-path engine bugs recorded in the roadmap's
Phase 5 entry (the synthesized-configure fix and the stale cached
framebuffer drop).

## Theme tests (Phase 7)

`tests/test_theme.c` runs headless under ASan+UBSan and pins:

- **The no-regression pin**: the built-in default theme equals the
  Phase 6 v1 palette component for component (all 9 consumed colors
  plus the 2 metrics that replaced `BTN_RADIUS` and the 1px rule).
  If this ever fails, "never touching themes" changed pixels.
- **Programmatic themes**: create/set/get, every validation rule
  (bad tokens/metrics/ranges/NULL, rename caps), and that editing a
  theme never installs it.
- **Parsing**: a complete file (all keys, string escapes, both hex
  forms), partial files (inheritance), and the whitespace
  tolerances (comments, blank lines, CRLF, lone CR, UTF-8 BOM,
  tabs, no trailing newline, spaced brackets).
- **The adversarial matrix from `docs/security.md`**: 40+ malformed
  inputs — unknown keys/sections, duplicates, bad hex, wrong value
  types, out-of-range metrics, leading zeros, over-long integers/
  strings/lines, control chars, unterminated strings, bad escapes,
  embedded NULs (with explicit lengths, so NUL can't truncate),
  wrong versions, oversized inputs — each asserting the exact
  `fdk_result` code. This matrix caught two real parser bugs before
  they shipped.
- **File loading**: valid/missing/zero-byte/bad-version files, NULL
  paths (a zero-byte file is rejected, not silently defaulted).
- **Live switching**: a standalone tree with a button and separator
  painted to an offscreen surface before/after
  `fdk_theme_set_default()` — fill and band pixels change to the new
  theme, the separator thickness metric paints a 3px band on the
  same center line, same-pointer switches add no damage, and
  destroying the current theme reverts to the built-in safely.
- **Registry hygiene**: 8 roots created and destroyed in scrambled
  order with switches before/during/after.

The X11 suite adds `test_theme_switch_gui`: a real window whose
button fill, corner radius (rounded vs square corners, verified at
the corner pixel), and separator band (1px vs 3px) are read back
server-side across two switches, with the round trip back to the
built-in theme pixel-exact.

The demo rig `scripts/run_theme_demo_x11.sh` drives `05_theme` with
real clicks through three themes and PIL-verifies 13 properties
including the pixel-exact round trip.

## Decoration & window-management tests (Phase 8)

Three layers, deliberately:

**Pure math, headless** (`tests/test_window_logic.c`, runs in plain
`make test`): the resize-edge zone classifier (all eight zones,
corner precedence, degenerate narrower-than-2*border windows,
out-of-bounds points), the edge-drag geometry solver (every edge and
corner, min/max clamping with opposite-edge anchoring — a clamped
W-drag must not drag the right edge along), and the double-click
window/slop boundaries. These are pure functions in window.c
precisely so they can be proven with no display at all.

**X11 GUI** (in `make test-x11`): `test_decorations_gui` covers the
band server-side — themed fill and rule, content below, MWM hints
on/off, a REAL click on the close button delivering a genuine
close-request, band drag to the exact position, the on/off round
trip — and now also the maximize and minimize band buttons through
real synthetic input (server-verified geometry: the screen fill and
the remembered restore origin; the unmap/map transitions), band
double-click maximize/restore, and the themed `title_bar_height`
metric re-flowing the window live with pixel proof.
`test_window_state_gui` proves the bare-X fallback world: FDK as its
own WM (maximize saves geometry and fills the screen; unmaximize
restores exactly; minimize unmaps; restore remaps; every flip
delivers exactly one FDK_EVENT_WINDOW_STATE). `test_resize_edges_gui`
proves the FDK-driven resize drags: zones off by default (content
widgets receive corner presses), SE/E/N drags resizing exactly, and
the app's size limits clamping the drag.

**The fake window manager** (`test_ewmh_fake_wm`): under Xvfb there
is no WM, which is exactly right for the fallback tests — but the
EWMH paths a real desktop exercises would go untested. So the test
BECOMES a window manager on a second X connection: it advertises
_NET_SUPPORTED (so FDK's connect-time probe enables the message
paths), takes SubstructureRedirectMask on the root, and answers
_NET_WM_STATE client messages by rewriting the window's
_NET_WM_STATE property — what real WMs do; the PropertyNotify is how
FDK learns state. It also maximizes/restores the window like a WM
would (so FDK's configure-driven reflow is exercised), simulates a
_NET_WM_MOVERESIZE move, and maintains WM_STATE for iconification.
The test asserts the messages field-by-field: add/remove actions,
both maximized atoms, the source indication, the MOVE direction at
the exact translated root coordinates, SIZE_BOTTOMRIGHT from the SE
corner press, and IconicState from the iconify request. It installs
after the bare-X tests and uninstalls before the suite ends so
nothing downstream sees a phantom WM.

**The self-confirming-test lesson (1.1.3)**: the fake WM above once
interned its "maximized" atoms with the same misspelling the
library had (`_NET_WM_STATE_MAXIMIZED_HORIZ` — the EWMH spec atom
is `..._HORZ`), so the test verified FDK against a copy of FDK's
own bug. The typo shipped: under real WMs the capability probe
never matched, maximize silently degraded to the bare-X fallback,
and the maximized state desynced from the WM (a user report from
Cinnamon, reproduced and fixed in milestone 1.1.3). Two guards now
exist. `test_ewmh_atom_spelling` checks the library's interned
atoms against the SPEC strings through a second X connection
(XInternAtom with only-if-exists is the oracle: it returns None for
any name nobody interned, so a misspelling cannot hide); and the
fake WM's atoms are spelled per the spec independently, so any
drift in the library fails the field-by-field message assertions
instead of echoing them.

**The real-WM rig (openbox, maintainer staging scripts)**: the
fake WM verifies the PROTOCOL EXCHANGE, but two bugs of a different
kind only manifest with a WM that actually takes over the
interaction — the fake WM never grabs the pointer, so it cannot see
them. The rig runs Xvfb + openbox 3.6.1 (user-space prefix, no dbus)
and drives the decorations example with REAL input through the
XTEST extension (direct XTestFakeMotionEvent/ButtonEvent calls —
NOT xdotool, whose motion is silently ignored by this environment's
Xvfb while its queries work, which cost an hour of phantom
"broken input" debugging): it verifies the EWMH probe detects the
WM, a band drag MOVES the window (openbox clamps to the workspace,
so the rig drags away from screen edges first), the maximize
button fills the screen with both _NET_WM_STATE atoms set and the
state event delivered, unmaximize restores the geometry, an edge
resize grows the window, and — the regression that motivated the
rig — the first CONTENT click after a WM-driven drag still
activates the widget it hit-tests (the stale-grab check). This rig
found and verified the fixes for all three 1.1.3 bugs.

**The resize-flash probe (1.1.4)**: the second Cinnamon report —
"the window flashes white while resizing" — is a TIMING bug: the
flash exists only for the frames between the WM's resize and the
client's repaint, so it cannot be asserted by the request/response
tests above. `scripts/resize_flash_probe.c` samples the window's
pixels mid-storm instead: it drives a 90-step XTEST resize drag on
the decorations example under openbox and grabs the window's
top-left 200x100 region right after EVERY motion step (the region
stays inside the window for the whole SE-corner drag — race-free
grabs), counting near-white pixels per sample. The pass bar is a
worst per-sample white ratio under 0.5 (dark-themed content is a
few percent text ink; the bug's background clear was ~1.0). The
rig (`scripts/verify_resize_flash.sh`) runs the probe twice: once
against the current build (must read clean) and once against a
git-worktree build of the PRE-FIX commit (must reproduce the
flash) — a control run that proves the probe can actually see the
bug it exists to guard. Fixed build: worst ratio 0.000 across
1.82M sampled pixels; pre-fix control: 1.000.

Three new in-suite regressions accompany it (in
`test_x11_integration.c`): `test_resize_retains_pixels` pins the
anti-flicker contract server-side — the window's bit gravity is
NorthWest, and a server-side resize leaves the painted region's
pixels intact when read back BEFORE FDK processes the configure
(no pump in between: the old background-pixel window read white
here); `test_hover_revalidation_on_geometry_change` reproduces the
stuck maximize-button highlight with the REAL pointer (XWarpPointer
onto the button — warping, not XSendEvent, because the
revalidation must find the true pointer position — then a
server-side window GROWTH under the stationary pointer; hover must
re-derive to nothing, follow the button to its new position, and
clear entirely when a shrink moves the window out from under the
pointer); and `test_resize_cursor_affordance` asserts the cursor
compass state through the `fdk__window_cursor_edge` seam (the X
cursor itself is not queryable without XFixes, which this
environment lacks — the seam asserts the shape logic: E and SE
edge-zone hovers, interior/leave/edges-off all restoring the
default). One XTEST-adjacent lesson: `XWarpPointer` is the way to
place the REAL pointer for query-pointer-dependent tests;
XSendEvent synthesizes delivery without moving anything, and an
XEvent compound literal must set `.type` INSIDE the member
initializer (`.type = X, .xconfigure = {...}` zeroes the type
field — the xconfigure initializer runs last and its own unlisted
`type` member overwrites the outer one).

**Wayland** (`make test-wayland` + `tests/test_wayland_integration.c`):
runs against a REAL compositor reachable via $WAYLAND_DISPLAY (the
binary self-skips without one, so plain CI never fails on it). The
verification compositor is sway 1.10 headless (wlroots pixman
renderer): Debian's weston 14 ships NO xdg-decoration implementation
in any shell, and its desktop-shell additionally can't run from a
non-root prefix. sway advertises the protocol, honors client-side
mode requests, and reports tiled windows as maximized at map time —
the test asserts the correct REACTION to whichever world the
compositor picks (client-side confirmed: the band is pixel-verified
in-frame; server-side forced: FDK drops its band and delivers
FDK_EVENT_WINDOW_DECORATION) rather than one specific answer. Also
verified: configure-states[]-driven FDK_EVENT_WINDOW_STATE, the
minimize request's optimistic flag with restore honestly
FDK_ERR_UNSUPPORTED (xdg-shell has no unminimize request), and — via
the maintainer rig's WAYLAND_DEBUG counting — the exact protocol
requests (set_mode x3 across an on/off/on cycle, decoration
configure, set_maximized/unset_maximized/set_minimized). Two real
backend bugs were caught on its first runs and fixed: the
xdg-decoration object must be created before the surface's first
buffer (sway enforces it), and a wl_surface.frame callback whose
window dies before `done` leaked its proxy (LeakSanitizer).

**Demo rig**: `scripts/run_decorations_demo_x11.sh` drives
`06_decorations` end-to-end with real input — band drag, decoration
toggle off/on, the band's MAXIMIZE button, a band double-click
restore, an SE resize-corner drag (460x300 -> 500x330), and close
via the band button — 16 PIL checks including the band returning at
the post-drag position and pixel-identical on/off/on.

A build-system bug the first Phase 8 slice exposed and fixed: the
Makefile had no header dependency tracking, so internal-header edits
left stale objects (ASan caught the struct-offset corruption on
first window creation). `-MMD -MP` dependency files are now
generated and included; the suite is safe against incremental-header
edits.

## What `make test-x11` actually verifies

Real, observable behavior against a live X server (see
`tests/test_x11_integration.c`):

- Connect and clean shutdown
- Connect with a custom `app_id`
- `fdk_quit()` called before `fdk_run()` doesn't crash
- `fdk_run()` returns immediately when no windows are open (per its
  documented exit condition in `fdk_core.h`)
- Window create → show → hide → show → destroy
- Window size is correctly reported after creation
- `fdk_window_set_title()`, including with `NULL`
- **Resize round-trip**: calling `fdk_window_resize()` and actually
  receiving a real `FDK_EVENT_WINDOW_CONFIGURE` event back through
  `fdk_run()`'s dispatch path, with the correct new size — this is the
  test that proves the event loop, X11 event translation, and
  callback dispatch all work together, not just that individual
  pieces compile.
- **Resize-storm backlog drain** (1.2.2): 300 `XResizeWindow` calls
  from a SECOND X connection — exactly what a WM produces during an
  interactive drag, all queued server-side before one pump — must
  drain to the final size within a bounded alarm, the batched
  geometry repaint must land (fresh pixels read back through a
  separate connection at the final size's far corner, a region that
  only exists once a framebuffer at that size was painted), and a
  NEW resize after the storm must still process in one pump. The
  last check is liveness — the pre-1.2.2 wedge never returned to the
  caller at all (one core pegged forever inside a single
  `dispatch_pending`, input queued behind the backlog: "it doesn't
  update anymore and the title bar buttons stop working"). Post-fix
  the whole 300-configure storm coalesces into ONE pump call.
- **`fdk_pump_events()` timeout semantics** (0 = non-blocking,
  positive = bounded wait) and its argument-error contract
- **Renderer pixel readback**: draw through `fdk_surface` (solid
  fill, filled rect, 1px border, direct `info.pixels` write),
  present, then read the window's contents back from the X SERVER
  via `XGetImage` over a second, independent X connection — and
  compare against the exact packed 0x00RRGGBB values. A blit that
  drew nothing, misaligned, or swapped channels fails this.
- **Renderer resize-follow**: resize mid-session, pump until the
  `FDK_EVENT_WINDOW_CONFIGURE` arrives (exercising the documented
  application loop shape), re-acquire the framebuffer — which must
  now be the new geometry — render, and verify a pixel that only
  exists in the NEW geometry reads back as the freshly drawn color.
- **Damage-tracked partial present** (second slice): present frame
  1, then damage only a small rect — the server must show the new
  rect AND keep frame 1's pixels everywhere else (proving the
  client-side content outside the damage survives); then write a
  pixel RAW without `fdk_surface_invalidate()` and prove the server
  never receives it (present is a true no-op on empty damage); then
  declare the damage and watch exactly that pixel arrive. The no-op
  skip is OBSERVABLE here, not just asserted.
- **New primitives readback**: line, filled circle, circle outline,
  and rounded rect (middle filled, corners cut, radius clamped)
  verified server-side at exact pixel values; `frame_ready()` is
  asserted always-true on X11.
- **Offscreen blit to window**: a sprite composed on an offscreen
  surface, blitted (full and partial source rects) onto a window
  surface, presented, and verified server-side — the cache/sprite
  pattern end to end.
- **Widget tree painting** (Phase 4): a window root widget carrying
  overlapping panels, a child on its parent, and a child poking out
  of its parent's bounds, painted via `fdk_window_paint` and
  verified server-side at exact pixel values (z-order, parent
  clipping, hidden-subtree removal, and the refusal to destroy a
  window-owned root).
- **REAL widget input via XSendEvent** (Phase 4): genuine
  MotionNotify / ButtonPress / ButtonRelease / KeyPress / FocusIn
  events sent through the X server into the FDK window — the same
  path physical input takes — verifying hover ENTER/LEAVE synthesis
  with widget-local coordinates, the implicit pointer grab
  (press→move→release all delivered to the press target, nothing to
  the widget underneath), the consumed-events contract (the
  application's window callback stops seeing events a widget
  handles), built-in Tab traversal driven by a real keypress, and
  window focus events mirrored into the focused widget.
- **Widget root follows resize**: after a live resize the root
  widget's bounds track the new client size and the fresh geometry
  is painted by the TREE (not the platform background pixel),
  verified server-side at a coordinate that only exists in the new
  geometry.

**Known, honest gap:** `WM_DELETE_WINDOW` (the close-request path,
`FDK_EVENT_WINDOW_CLOSE_REQUEST`) is not exercised by `make test-x11`.
Triggering it requires a window manager actually running to send the
`ClientMessage` — bare Xvfb has no window manager by default. The test
says so explicitly (`[skip]` line) rather than silently omitting
coverage. Closing this gap would mean either running a minimal WM
(e.g. a scripted `xdotool`/synthetic `XSendEvent` from a second
process) inside `make test-x11`, or a separate `make test-x11-wm`
target — deferred rather than half-implemented; the underlying
`WM_DELETE_WINDOW` registration and `ClientMessage` handling in
`src/platform/x11/x11_connection.c` and `x11_dispatch.c` is real code,
just not exercised by an automated test yet.

(This is exactly the technique the Phase 4 widget input tests DO use
for pointer/key/focus events — `XSendEvent` from a second connection —
so the remaining close-request gap is purely about the ClientMessage
shape a window manager produces, which is equally synthesizable when
someone picks this up.)

## Advanced widget tests (Phase 9)

**Headless** (plain `make test`, ASan+UBSan): `test_clipboard.c`
exercises the core dispatch and the mock-backend contract (set/get
round trip, replace semantics, NULL on empty); `test_entry.c` the
whole editing model as pure logic (cluster-safe cursor clamping,
selection algebra, cut/copy/paste against the mock, password/
max-length/read-only, preedit); `test_scroll.c` the scrollview math
(offsets, clamping, auto-hide thresholds); `test_list.c` and
`test_tree.c` the selection semantics as table-driven cases
(the full modifier grammar, moving key cursors, stable handles,
expander behavior); `test_menu.c` the model CRUD + measure math +
view activation state machine (click/keyboard/check/radio/disable,
bar hit-testing); `test_combo.c` the model + editable custom-text
state machine; `test_controls.c` the slider/spin/toolbar/notebook/
canvas logic.

**X11 GUI** (in `make test-x11`): the clipboard suite makes the
test itself a SECOND X client and performs the real selection
handshake through the server — both directions, TARGETS
negotiation, refusal when not owner, loss-of-ownership release.
`test_entry_gui` drives real key events into a focused Entry
(typing, Shift-selection, Ctrl+V from the real clipboard,
Backspace). `test_popup_window` pixel-verifies a popup window's
fill plus both dismissal paths (outside-click press, Escape).
`test_menu_gui` is the full popup lifecycle through real input:
bar click maps the popup, the popup AUTO-PAINTS (server-side
pixel proof), keyboard Down+Enter activates + closes, check items
flip state + fire callbacks + close, hover opens the submenu
chain (nested popups), Escape peels one level per press, and the
chain closes cleanly. `test_combo_gui` runs the dropdown
lifecycle (click opens the auto-painted list, picking a row sets
active + fires on_changed + closes, Escape dismisses without
changing the selection). `test_dialog_gui` proves modality is
REAL: the dialog's pointer+keyboard grab is held server-side
(the test's own foreign grab is REFUSED while the dialog lives
and succeeds after), Enter answers OK, Escape answers Cancel,
buttons answer through real clicks, and early destroy answers
Cancel via the destroy-notify path.

**Wayland** (`make test-wayland` under the sway rig): the menu
popup test is the deepest integration in the suite — the rig
drives sway's seat cursor to absolute coordinates so a REAL
button event (and its valid input serial) reaches the client
through the compositor; the menu button click, the popup maps,
an item is clicked FOR REAL, the chain closes, and outside-click
dismissal works. The dialog test verifies mapping + auto-paint +
early-destroy-answers-Cancel + clean ASan teardown. (Modality is
deliberately not asserted on Wayland — no toplevel-grab protocol
exists; dialogs there are non-modal, documented.)

**Demo rig**: `scripts/run_advanced_demo_x11.sh` drives
`examples/11_advanced` with real clicks (the same XSendEvent
discipline), PIL-verifying five frames — File menu popup, combo
dropdown, modal dialog, nested submenu, chain dismissed (the
last one structurally: the driver's XQueryTree assertion that
the root's child count returned to baseline proves every popup
window left the server) — plus all the demo's phase markers.

## Accessibility tests (Phase 10, first slice)

**Headless** (`tests/test_a11y.c`, 97 checks in plain `make test`,
ASan+UBSan): the whole layer is display-independent because it IS
the widget layer. Covered: the describe matrix over the full
catalog (roles, computed names, core + semantic states, value
interfaces with their rendered texts), accessible-name/description
overrides and their precedence over computed names,
SHOWING-vs-VISIBLE with hidden ancestors, the notification
matrix (children-changed on create/destroy, bounds, visible,
enabled, focus moves notifying BOTH ends, name changes from label
text edits, value changes, radio-group sibling unchecks), the
subscriber discipline (subtree scope filtering, duplicate
subscribes as no-ops, the 16-slot limit, unsubscribe +
NOT_FOUND), and the ACTION drivers verified against real widget
state — button ACTIVATE fires the callback, checkbox ACTIVATE
toggles, slider SET_VALUE quantizes and fires on_changed, tree
EXPAND/COLLAPSE/ACTIVATE flip the model, list-row ACTIVATE
selects, notebook/combo/scrollview SET_VALUE switch/scroll. The
Entry's new modes are exercised here too (password, read-only
with the reader contract, max-length refusal semantics, and the
VALUE_CHANGED notifications).

**X11 GUI** (in `make test-x11`): `test_a11y_gui` runs the layer
against a LIVE window — the window root announces the WINDOW role
with the window's title as its accessible name and bounds
matching the window size, `fdk_window_set_title` propagates to
the name, and REAL key events typed through the X server arrive
in the Entry's value interface — the exact snapshot a bridge
would poll.

## Narrator tests (1.1.0)

**Headless** (`tests/test_narrator.c`, 46 checks in plain
`make test`, ASan+UBSan): the embedded screen reader core is
display-independent for the same reason the a11y layer is — it is
"just" a subscriber. Covered: the composer matrix (named/unnamed
widgets, accessible-name overrides, every spoken state word —
checked/pressed/selected are covered via checked; disabled,
read-only — value renderings for slider/progress/entry, the
snprintf truncation + size-and-retry semantics, and the invalid
argument guards), the forced-announce path (NULL/empty no-ops,
sinkless no-crash), the engine e2e (start-requires-sink,
focus-move narration with focus-out silence, refocus no-op,
toggles narrated at the new value, unfocused background churn
silent, the stop/start/park lifecycle with announce surviving
stop, typing NOT narrated, focused slider/spin value narration),
localization through a real parsed catalog (role + state words
translated, untranslated msgids passing through, NULL =
English), and the FDK_A11Y_MAX_SUBSCRIBERS slot exhaustion →
FDK_ERR_LIMIT with recovery. The suite also guards the
documentation contract that a fully destroyed pointer is UB (the
dying-widget refusal protects mid-teardown callbacks only) —
noted in-source rather than tested-with-UB.

**X11 GUI**: `examples/12_narrator.c` doubles as the live proof —
run under Xvfb with `FDK_DEMO_FRAMES`, its stdout must contain
the full narration sequence (forced announcement, four focus
utterances, the toggle and value utterances in order) and it must
exit cleanly; the screenshot battery captures the subtitle bar
mid-tour.

## Wayland test coverage

`make test-wayland` builds and runs
`tests/test_wayland_integration.c` against whatever compositor
`$WAYLAND_DISPLAY` reaches (the binary self-skips without one, so
plain `make test`-style CI never fails on it). The verification
compositor for the maintained rig (`scripts/run_wayland_suite.sh`)
is sway 1.10 headless — wlroots pixman renderer, `WLR_BACKENDS=
headless`, a config that floats FDK's windows at a known position
so client-side resizes take effect. Debian's weston 14 ships no
xdg-decoration implementation in any shell and its desktop-shell
cannot run from a non-root prefix, which is why sway won (the
Phase 3-era runs used weston's kiosk shell; the compositor was
switched when Phase 8 needed xdg-decoration).

Since 1.1.5 that weston "limitation" is a second, deliberate
verification world: `scripts/run_wayland_deco_weston.sh` runs the
SAME suite plus the decorations demo against weston kiosk-shell
headless — a real compositor that advertises no
zxdg_decoration_manager_v1, i.e. the exact Muffin/Cinnamon-Wayland
condition of the third user report. The suite's decorations
section hard-asserts set_decorated(true) succeeds there (CSD is
the xdg-shell default; the old code returned UNSUPPORTED and the
demo exited before mapping a window), the demo must run its full
auto cycle with ZERO xdg-decoration traffic in the WAYLAND_DEBUG
trace, and a control build of the pre-fix commit under the same
weston must reproduce the user's exact failure line. Seat-less
kiosk-shell also exercises the honest-skip paths: the clipboard
section (a data_device_manager global exists but no wl_seat, so no
data device can) and the interactive menu section.

The suite currently verifies, all against the live compositor and
all under ASan+UBSan with leak checking:

- Connection + registry + seat binding; the xdg-shell handshake
  with the empty first commit, configure, ack, buffer attach,
  damage, and commit in the protocol-correct order
  (WAYLAND_DEBUG traces were used to develop it; the rig still
  counts requests where order matters).
- Client-side decorations end to end (pixel-verified themed band)
  on BOTH worlds — sway (xdg-decoration negotiated: set_mode
  CLIENT_SIDE, and the forced-SERVER teardown contract if the
  compositor ever insists) and weston kiosk-shell (no protocol at
  all: the band is the only chrome that can exist),
  maximize/unmaximize state events from configure's states[],
  minimize request + optimistic flag, honest FDK_ERR_UNSUPPORTED
  restore (xdg-shell has no unminimize request).
- Layout reflow through a compositor-driven resize; HiDPI scale
  handling (logical vs physical geometry).
- Clipboard set/get round trip on FDK's own selection.
- Menu popups THROUGH the compositor: the rig drives sway's seat
  cursor to absolute coordinates so the click is a real button
  event with a valid input serial — the popup maps under
  xdg_popup.grab, an item is activated for real, the chain
  closes, and outside-click dismissal works.
- Dialogs: mapped, auto-painted, early destroy answers Cancel,
  clean teardown.
- Clean shutdown (window destroy, buffer release, disconnect)
  with zero leaks — the leak-free teardown is itself a tested
  assertion, and it caught two real backend bugs on its first
  runs (the xdg-decoration object ordering, and a leaked
  wl_surface.frame callback proxy when a window died before
  `done` arrived).

Three real bugs were found and fixed by Wayland-side testing
during Phase 3, all of which would have reproduced on any
compositor — kept here because they shaped the discipline the
current code follows:

1. **Missing `wl_surface.damage`** — compositors schedule
   repaints from the damage region; a committed-but-undamaged
   surface is never drawn (the buffer latches, the window stays
   invisible).
2. **Missing output flush after listener callbacks** — requests
   queued by event handlers sat in libwayland's output buffer
   while the poll() loop blocked, deadlocking the handshake until
   an unrelated event happened to wake it.
3. **Xlib-side sibling (found by the X11 render demo)** — events
   buffered in Xlib's internal queue are invisible to poll();
   `fdk_pump_events()` now drains client-side-buffered events
   before waiting on the fd.

The remaining honest gap: the maintained rig starts the compositor
itself but lives OUTSIDE the Makefile (`scripts/run_wayland_
suite.sh`), because it depends on a user-space sway/wlroots
toolchain extracted without root access — reproducible, but too
environment-specific to hard-code. `make test-wayland` against a
system compositor is the automation seam; the rig is the
verification.

## Known Xvfb flakiness (investigated and fixed)

During development, `make test-x11` was intermittently flaky —
roughly 30-50% failure rate on repeated runs, with `fdk_init()`
failing to connect on the second or third connection attempt within
the same test binary. Root-caused via a systematic isolation process
(see the actual investigation transcript in project history if
available) to **two separate real issues**, both now fixed:

1. `fdk_x11_window_destroy()` called `XDestroyWindow()` followed only
   by an implicit flush on the next request, not an explicit
   `XSync()`. A caller that destroys a window and immediately calls
   `fdk_shutdown()` (which calls `XCloseDisplay()`) could race: the
   destroy request might still be sitting in Xlib's client-side write
   buffer when the socket closes. Fixed by adding `XSync()` after
   `XDestroyWindow()` in `x11_window.c`.
2. **The actual primary cause**: the original `make test-x11` Makefile
   recipe used `$RANDOM` to pick a throwaway display number, but Make
   recipes run under `/bin/sh`, which on this project's development
   environment is `dash` — and `dash` does not implement `$RANDOM`
   (it silently expands to empty string). Every invocation therefore
   used the exact same fixed display number, causing a stale-socket
   race between successive test runs. Fixed by deriving the display
   number from the shell's own PID (`$$$$` in the Makefile, which
   Make expands to `$$` for the shell, which the shell expands to its
   PID) instead of `$RANDOM`.

Both fixes are in place; `make test-x11` was stress-tested for
stability (multiple consecutive clean runs) after each fix to confirm
before moving on, per the project's "verify, don't assume" standard.
This section stays in the docs as a record of what was found and why,
not just what the fix was — useful if similar flakiness ever
resurfaces in a different environment.

## The fontconfig 320 B exit "leak" (RESOLVED in 1.1.5 — was an FDK LSan-scoping bug)

History: both suites intermittently failed ASan at exit with 320 B
leaked entirely inside libfontconfig.so. The Task-22 investigation
attributed it to cache temperature (failed-then-passed on identical
trees; the cold/warm pattern was real) and waved it off as
third-party noise. The 1.1.5 sway-rig work made it persistent and
therefore investigable, and a minimal probe (three
`fdk_font_load_system_default` calls, no display, no compositor)
reproduced it deterministically — no compositor involved at all.

Root cause: fontscan.c's `__lsan_disable`/`__lsan_enable` bracket
covered only `FcInit()`, while the design comment (and the actual
allocations) cover the FIRST FcConfigSubstitute /
FcDefaultSubstitute / FcFontSort walk too: that chain builds
fontconfig's process-lifetime pools in malloc'd memory — but ONLY
when the on-disk fontconfig cache is cold; a warm cache serves the
structures mmapped and allocates nothing. That is the entire
cold/warm nondeterminism, finally explained. FDK must not FcFini
those pools away (host apps may share fontconfig — GTK/Qt keep it
mapped too), so the bracket now covers the whole discovery call,
matching the comment's stated intent.

Standing rule unchanged: a fontconfig-framed leak report in an
FDK-free stack (no FDK frames anywhere) remains noise; but an
FDK-path leak that persists across warm runs is REAL until proven
otherwise — this one was.

Original Task-22 observation (kept for the record): `make test-x11`
failed at exit with `AddressSanitizer: 320 byte(s) leaked in 3
allocation(s)` — every stack frame inside `libfontconfig.so.1`. All
test cases had already passed; the report came from ASan's exit-time
leak scan. The failing run and a passing run of the IDENTICAL tree
differed only in cache temperature, which is why it was filed as
environment noise at the time — the 1.1.5 investigation (above)
found the deterministic mechanism underneath the pattern and the
FDK-side fix. The "re-run once, then investigate" protocol stands,
with the amendment that PERSISTENCE (not stack framing alone)
decides what is real.

## The weston kiosk-shell proxy "leak" (RESOLVED in 1.1.5 — was collateral of a protocol-error kill)

Running the Wayland suite against weston kiosk-shell tripped
LeakSanitizer on one 96-byte wl_proxy inside libwayland-client —
zero FDK frames, absent under sway. Root cause found by tracing the
suite's full protocol traffic: kiosk-shell configures every window
FULLSCREEN, and the suite's client-driven resize committed a buffer
larger than the configured state — a protocol error weston enforces
by killing the connection. Everything after the kill "passed"
vacuously on the dead connection, and the never-delivered teardown
left the proxy behind at exit. The resize gate (roadmap 1.1.5 item
4) keeps the connection alive; the suite now runs leak-clean under
weston with LeakSanitizer fully enabled — no ASAN_OPTIONS
carve-outs anywhere in the rigs.

## Sanitizers

Every test binary (both suites) is built with AddressSanitizer +
UndefinedBehaviorSanitizer by default (see `docs/build.md` — this is
the standard debug-build configuration, not a special test-only flag).
A test that "passes" but leaks memory or trips UB is a failure, not a
pass, per `docs/memory.md`.

## Performance baseline (not a test)

`make bench` builds and runs `tests/bench.c` against release objects
(no sanitizers — they would distort timings by multiples) and prints
one ops/s + ns/op line per benchmark: tree construction (eager and
layout-batched), layout sweeps, full vs damage-tracked repaints, text
measurement and line-breaking, event dispatch, theme switching, and
the i18n formatters. It is deliberately NOT part of `make test`:
performance numbers are machine-dependent. The reference baseline and
the findings it produced (including the 515x layout-batching win)
live in `docs/performance.md`.

## The Wayland rigs' 1.1.6 hardening (buffer pacing + cursors)

The second Cinnamon-Wayland report (see roadmap 1.1.6) forced three
test-infrastructure rebuilds and found two latent bugs — recorded
here because every piece of them will bite again otherwise.

**The injector is back, with a handshake.** `fdk-wl-inject` (the
zwlr_virtual_pointer_v1 client that mints REAL input serials under a
headless compositor) was lost to a session reset and recreated from
the suite's documented command protocol (`move x y` / `down b` /
`up b` / `tap b`). Two design changes over the original: (1) the
output extent is DISCOVERED from the first wl_output's mode event
instead of assumed — the headless output is 1280x720, not 800, and
the wrong constant scaled every injected Y by 0.9 (a pointer sent to
(165,115) arrived at (165,103.5); the trace's enter coordinate
(65,43.496) against the window's known geometry pinned it in one
look); (2) a READY-FILE handshake — the injector writes the path in
$FDK_WL_INJECT_READY once the virtual pointer exists, and the suite
polls that file before trusting the pipe. Without the handshake,
running the suite under a compositor that lacks the wlr protocol
(kiosk-shell weston) died on SIGPIPE at the first command write —
an "absent tooling honestly skips" contract that was only ever
checked per-machine, not per-compositor.

**The sway rig's floating rule is load-bearing.** The suite's
interactive coordinates all assume the window sits at (100,60) —
which requires `for_window [app_id="org.fdk.test"] floating enable,
move position 100 px 60 px` in the rig's sway config. That line was
lost in the same reset as the injector; since the injector was also
gone, every interactive section silently [skip]ped and nothing
noticed. Two lessons: the [skip] greps must be READ (a rig that
passes on all-skips proves nothing), and `position 100 60` is not a
sway command — `move position 100 px 60 px` is; the invalid form
parses without error and places the window at sway's centered
default. Placement is verifiable directly: run any client with the
rule's app_id and ask `swaymsg -t get_tree` where it actually is
(scripts/probe_placement.sh does this; note a window WITHOUT CSD
gets sway's titlebar and sits 25px lower than placed — probe with a
decorated client).

**A latent suite UAF the new coverage detonated.** The menu section
destroyed its font while the opener button borrowing that font still
lived in the main window's tree; no test repainted that tree
afterwards, so it survived every battery — until the new pacing
section created a second window, sway reconfigured the first, and
the dispatch tail's synchronous repaint painted a Button through a
freed font under ASan. Borrowed resources must outlive their
borrowers; the section now destroys the button before the font.

**What the pacing regressions can and cannot pin.** The suite can
deterministically build a full pool (back-to-back damaged presents
with zero pumping — the commits sit unflushed in libwayland's output
buffer, so the compositor cannot have released anything) and assert
frame_ready stays honest, and that an acquisition against that pool
waits and lands. What it CANNOT do against a healthy compositor is
reproduce the user's guard-time WARN storm: sway releases within a
frame, so the 100ms wait always recovers — the storm needs a
compositor slower than the wait window (Muffin's experimental
session under load). The rig therefore asserts ZERO "buffers in
flight" WARN lines across suite and demo (the regression tripwire),
and the live re-test on the reported machine is the real control.

**Cursor evidence is protocol-level.** The window-layer seam
(fdk__window_cursor_edge) proves the compass armed; the actual
wire proof is the suite's WAYLAND_DEBUG stderr (captured separately
from stdout since 1.1.6) grepped for wl_pointer.set_cursor —
requests that can only exist if the theme loader found a real
cursor file, parsed it, and uploaded it. Debian's Adwaita and
"default" (Inherits=Adwaita) themes are present on the test image;
the pre-fix control build fails exactly this check (and the
set_app_id one) — the probes see the features they guard.

**The recurring object-collision trap.** Debug and release share
build/obj; running `make release` and then a rig (which builds
debug) mixes flags-silently and fails at link or run with no
obvious cause ("FAIL: build", suite exit 2). `make clean` between
battery phases is mandatory, not cosmetic.

**1.1.7 — the churn budget is provably right but not sway-visible.**
The resize-churn regression (exhaust the pool at size A, resize to
size B inside a measured call) pins that the churned acquisition
completes under a ceiling and the window settles — but the
100ms-vs-10ms difference between the 1.1.6 and 1.1.7 budgets
cannot be discriminated on sway: a probe trace of the PRE-FIX
build shows sway releasing superseded wrong-size buffers within
the resize call itself, crediting the pool before the acquisition
wait starts (the wrong-size reaper then takes an empty slot with
no wait at all — both budgets converge to single-digit ms). The
defect only bites on compositors that hold buffers past the
release window (Muffin under load — the live report). The suite's
wall-clock assert is therefore a ceiling tripwire against a
catastrophic reintroduction, NOT the discriminator; the live
re-test on the reported machine is the control, exactly like the
1.1.6 WARN-storm limitation above. Under kiosk-shell weston the
same block takes the honest compositor-owned skip (fullscreen
refuses the client resize; the churn then rides the kiosk
configure's own synchronous repaint).

**1.1.7 — the top-edge press is protocol-asserted, both ways.**
The titlebar-edge regression injects a REAL tap at the N edge
over the deco band and the rig asserts >=1 xdg_toplevel.resize and
ZERO xdg_toplevel.move in the suite's WAYLAND_DEBUG trace. The
zero-move check needs a manual grep (the rig helpers assert
minimums only) and depends on the tap being the suite's ONLY
interactive handover — any new suite section that legitimately
drags the band must move its assertions to a scoped trace. The
pre-fix control (eacc8c9 worktree + the new probes) demonstrates
the bug exactly: move=1, resize=0.

**The .pc-clobber trap generalizes beyond re-extraction.** Any
script that extracts debs into the prefix (the wayland toolchain
rebuild AND install_openbox_rig.sh alike) overwrites the pkg-config
prefix rewrites; a rig that builds after an openbox install fails
with "wayland-client.h: No such file or directory" or "Package
wayland-client was not found" while manual builds worked minutes
earlier. Re-run the sed prefix fix after EVERY deb-extracting
script, then verify with `pkg-config --cflags wayland-client`
pointing at the prefix before blaming the rig.

## 1.2.0 — capability validation rigs

The headless suite grew two files: test_dnd_logic.c (the uri codec
both backends share — CR/LF tolerance, comments, percent-decoding,
malformed-escape containment, non-file schemes verbatim, empty and
hostile payloads, round trips, and the public API's argument
safety) and test_file_dialog_logic.c (the scan seam — hidden and
dirs-only filtering, dirs-first ordering, no ./.. rows, unreadable
dirs fail closed, entries ownership, result_free tolerance).

The GUI/interop layer is where 1.2.0's real coverage lives, all
against REAL external applications (no FDK-to-FDK):

  - scripts/xdnd_source.c — raw-Xlib XDND SOURCE (an external app
    dragging INTO FDK). Full handshake; exit 0 only on Finished
    success=1. The X11 suite spawns it against a registered window
    and asserts the decoded drops (files -> POSIX paths, text).
  - scripts/xdnd_sink.c — raw-Xlib XDND TARGET (an external window
    FDK drags INTO). The suite drives a REAL pointer via XTEST
    (scripts/xtest_driver.c) with HUMAN-PACED steps and pumps
    between (one driver call per step), asserts FDK's drag
    reported SUCCEEDED, and that the sink decoded BOTH payloads.
  - scripts/wl_dnd_source.c — raw-libwayland drag source for the
    sway rig (generated xdg-shell protocol object, zero FDK
    linkage); started on a real injected press+motion, dragged
    onto the FDK window, release -> drop.

Lessons (each cost an hour; all now encoded in the rigs):

  1. NON-BLOCKING CHILD DRAINS ONLY. A blocking fgets on a child
     that goes silent mid-handshake (xdnd_source waiting for the
     Status FDK hasn't sent because nothing pumps) starves the
     pump and deadlocks the protocol. Every drain in the suites
     is fcntl-O_NONBLOCK + poll-pump loops now.
  2. The XTEST driver's startup self-check MOVED THE POINTER
     (11,22), thrashing drag targets between invocations; it is
     opt-in via FDK_XTEST_SELFCHECK=1.
  3. sway grants the seat's POINTER capability only when the
     virtual pointer produces its FIRST event — after injector
     start, inject a priming motion before waiting on anything
     pointer-dependent.
  4. A fresh client connecting AFTER the virtual pointer exists
     still sees capabilities(0), and wl_seat.get_pointer before
     any capability ever existed is a protocol error — spawn
     order in the rig: client first (it dispatches, waiting),
     injector second (its device creation is the caps update).
  5. wlroots delivers ::drop only when the drag offer received
     the legacy accept(mime) as well as set_actions — set_actions
     alone leaves source->accepted unset and a release CANCELS.
  6. XDND's Finished reply must set the success bit (protocol
     v2+); without it the dragging client correctly reads failure.
  7. The sway rig sleeps 1.5s after the socket appears: sway's
     seat/output initialize after the socket file, and the first
     injected click races the first window's placement otherwise
     (the flake that masqueraded as a config regression).
  8. stdout is lost on abort (buffers unflushed) — interop
     assertions read from ACCUMULATED child output, never from
     print-through drains.

Honest gaps, stated: no external-client rig for FDK->external
drags on Wayland this milestone (the shared payload codec is
pinned by both headless tests and the X11 interop; the Wayland
source op is protocol-exercised via wl_dnd_source's peer role);
wlroots 0.18 ends the drag source with ::cancelled after a clean
receive->finish->destroy tail (payload unaffected; suite asserts
either end signal, rig verifies the tail in WAYLAND_DEBUG).

## 1.2.5 — the query-pointer contract test, and the rigs that were caught lying

The Wayland suite gained the load-bearing regression for the
unified cursor hit-test (see roadmap 1.2.5): after REAL injected
input hovers the maximize button, a client-side shrink under the
stationary pointer, and the window_query_pointer op is interrogated
BEFORE any pump — the client resize path updates last_size
synchronously while the seat cache still holds the pre-resize
position, so the stale state is deterministic without any
compositor round-trip. The seam assert fails on the pre-fix body
(verified by running the suite against the reverted code), and the
observable half — hover and cursor cleared by the leave synthesis
after the flush — follows. This mirrors the X11 suite's
hover-revalidation test, which drives the same scenario with
XWarpPointer + server-side resizes.

Two rig-integrity lessons from this milestone, both recorded
because they produced GREEN OUTPUT FROM BROKEN RUNS:

1. A rig that does not BUILD its subject can verify stale
   artifacts. The sway example rig assumed build/examples existed;
   the clipboard-interop rig make-cleans between runs; the next
   sway rig run then "passed" 8/8 against the previous session's
   PNGs. The rig now builds the examples first, and the captures
   are deleted before each run.
2. A compositor-config rule other rigs depend on must live IN the
   rig that needs it. The Wayland test rig's sway config had lost
   the floating pin (org.fdk.test windows at (100,60)) that every
   injected coordinate is computed from — under tiling the clicks
   missed and the suite failed at the first injected button press.
   The rule is restored with a comment stating it is load-bearing.

The sway restore path for the recurring sandbox wipe is now a
durable script (scripts/rebuild_sway_rig.sh): apt-cache recursive
closure resolution, skipping packages the system already provides
(extracting libc6 into the prefix would break the binaries), sway
+ libwlroots + grim (grim is NOT in sway's dependency closure — a
rig without it happily verifies stale captures) into
/home/z/apt/prefix, .pc prefix fixes, and a headless startup
verification.
