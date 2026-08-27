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

There is currently no `make test-wayland` — see "Wayland test
coverage" below for why, honestly.

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

`tests/test_text.c` runs headless in plain `make test` (9 cases,
ASan+UBSan) and needs no display — only a system TrueType font
(DejaVu Sans / Noto Sans candidates; the whole suite honestly skips,
[X11-suite style](#known-xvfb-flakiness-investigated-and-fixed), when
the environment has none). Covered: font lifecycle and every failure
mode (missing file, garbage bytes, directories, out-of-range sizes —
including the sfnt container gate that keeps malformed fonts from
becoming out-of-bounds reads inside stb_truetype), metrics sanity and
2x scale proportionality, measurement (proportional vs monospace,
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
`tests/test_layout.c` runs headless in plain `make test` (9 cases,
ASan+UBSan): box measure math (naturals + spacing + padding +
margins, homogeneous, orientation), arrangement math (packing,
expansion absorbing the leftover, cross-axis align/expand), margins
inside slots, dynamic relayout on child add/remove/hide/hint change,
nested child-change PROPAGATION (the regression case for the two
engine bugs the 07 demo found: a Frame relayouts when ITS children
change — box-ness by hook delegation — and ancestors re-run when a
nested container's natural changes, with the box setters reaching
subclasses), nested boxes, a pixel-agreement case (a laid-out tree paints exactly
into its computed slots), and argument safety. The window integration
(auto-reflow of the content widget on every configure) is the X11
suite's job: `test_widget_layout_reflow_on_resize` resizes a live
window and verifies SERVER-SIDE that the box reflowed — the fixed
header stays 40px, the expanding panel owns the new space, and the
boundary sits at the exact pixel layout computed. Destroying the
content widget and calling fdk_window_layout() must deactivate the
association cleanly rather than arranging a freed widget.

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

The demo rig `scripts/run_theme_demo_x11.sh` drives `08_theme` with
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
`09_decorations` end-to-end with real input — band drag, decoration
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

## Wayland test coverage

There is no automated `make test-wayland` yet (see the gap note
below), but the Wayland backend **has been integration-tested against
a real compositor**: Weston 14 running with its headless backend and
pixman renderer, driven end-to-end with the hello-world example as a
client. Verified in that setup:

- `fdk_init()` connects, binds wl_compositor/wl_shm/xdg_wm_base, and
  reports the backend honestly (including the no-seat warning).
- The xdg-shell handshake completes: initial empty commit, configure
  received and acked, then the solid background buffer is created,
  attached (with full-surface damage), and committed.
- `WAYLAND_DEBUG=1` protocol traces confirm the exact request order
  above, and a compositor screenshot pixel-verifies the window is
  actually mapped and visible (solid white covering the output).
- Clean shutdown path (window destroy, buffer destroy, disconnect)
  exercised when the compositor connection closes.

The Phase 3 render path was verified in the same rig with the
`02_software_render` example:

- Per-frame protocol counts from `WAYLAND_DEBUG=1` over ~155 frames:
  155 wl_shm pool creations, 155 buffer creations, 154 attaches,
  155 commits, 152 releases — exactly the one-fresh-buffer-per-present
  lifecycle `wayland_window.c` implements, with the compositor
  releasing superseded buffers (no leaks, no slot exhaustion).
- Two compositor screenshots ~3.5 s apart pixel-verify the ANIMATED
  content is live: different gradient colors in each capture (the
  palette cycled), 276+ distinct colors, and the white ball/logo
  pixels present.
- kiosk-shell's fullscreen `xdg_toplevel.configure(1024, 640)` was
  received and the renderer followed it — the framebuffer resized and
  the screenshots show the gradient covering the full output, not the
  original 640x480 window.

And a third real bug was found by exactly this testing, this time on
the X11 side during the render demo:

3. **Events buffered in Xlib's queue were invisible to poll()** —
   Xlib reads socket data into its internal queue during ANY I/O,
   including the `XFlush` at the end of every `fdk_surface_present()`.
   An event (the demo's synthetic `WM_DELETE_WINDOW`, arriving while
   the app was mid-render) could therefore already have left the
   socket — and poll() on the connection fd never fires for it again.
   Events that happened to land during the poll() wait worked; events
   landing during rendering were swallowed forever. Fixed by
   `fdk_pump_events()` draining client-side-buffered events before
   waiting on the fd (both backends' dispatch_pending are safe to
   call in that position by design).

Two real bugs were found and fixed by exactly this testing, both of
which would have reproduced on any compositor:

1. **Missing `wl_surface.damage`** — compositors schedule repaints
   from the damage region; a committed-but-undamaged surface is never
   drawn (the buffer latches, the window stays invisible).
2. **Missing output flush after listener callbacks** — requests
   queued by event handlers sat in libwayland's output buffer while
   the poll() loop blocked, deadlocking the handshake until an
   unrelated event (e.g. a compositor ping) happened to wake it.

The remaining gap: this is not yet wired into `make test-wayland`
because the test environment assembled a fully user-space Weston
(extracted distribution packages with no root access, path relocation
via `WESTON_MODULE_MAP`, `--shell=kiosk-shell` to avoid
desktop-shell's helper clients, `--use-pixman` for real software
rendering) — reproducible, but too environment-specific to hard-code
into the Makefile honestly. The X11 backend remains the coverage bar;
Wayland is verified to a manual-but-real integration level, one step
short of automated CI.

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

## Sanitizers

Every test binary (both suites) is built with AddressSanitizer +
UndefinedBehaviorSanitizer by default (see `docs/build.md` — this is
the standard debug-build configuration, not a special test-only flag).
A test that "passes" but leaks memory or trips UB is a failure, not a
pass, per `docs/memory.md`.
