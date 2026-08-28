# FDK Architecture

## Layering

FDK is organized in strict layers. Each layer may depend downward on
the layers below it, and never upward:

```
Application code
      |
Widgets  (src/widget/)         — Phase 4 (foundation) landed;
      |                           core widget catalog Phase 6
Layout   (src/layout/)         — Phase 5 complete: box + grid,
      |                           per-child hints, size limits,
      |                           baseline (hooks already in the
      |                           widget API: measure/arrange)
Text     (src/text/)           — Phase 6 complete: fonts, UTF-8
      |                           shaping, glyph cache, subpixel
      |                           positioning, synthetic styles,
      |                           drawing (sits on the render layer)
Rendering abstraction (src/render/) — Phase 3 (software surfaces,
      |                                framebuffer access,
      |                                damage-tracked present, clip
      |                                stack, primitive set,
      |                                alpha-mask glyph blit)
Windowing (src/window/)        — Phase 2 (+ Phase 4 widget glue:
      |                           fdk_window_get_root/paint,
      |                           event routing into the tree)
Platform abstraction (src/platform/) — Phase 2
      |         \
   X11/XLibre   Wayland
```

Alongside this vertical stack, several cross-cutting subsystems are
used by multiple layers rather than sitting in the stack themselves:

- **Core** (`src/core/`) — init/shutdown lifecycle, logging, error
  handling, memory allocation. Everything depends on this; it depends
  on nothing else in FDK. (Phase 1 — this is what currently exists.)
- **Theme** (`src/theme/`) — the theme API and the `.fdk`
  parser/loader (Phase 7). The built-in default theme is the Phase 6
  v1 palette exactly; the widget catalog resolves its 9 color
  accessors and 2 paint metrics against the current default theme at
  paint time. Contains FDK's one deliberately documented internal
  cycle: widget paint hooks call up into the theme module for
  tokens, and `fdk_theme_set_default()` calls back into the widget
  core's root registry to invalidate every live tree (both
  directions are internal .c-level calls; no header gymnastics, and
  the cycle is confined to these two functions).
- **Input** (`src/input/`) — unified keyboard/mouse/touch event
  structures, independent of X11/Wayland specifics. Sits between
  platform and widgets. (Phase 2, extended through later phases)
- **Text** (`src/text/`) — font loading, glyph rendering, text
  measurement/shaping. Consumed by rendering. (Phase 3)

## The rule that matters most: no backend leakage

No X11 type (`Display*`, `Window`, `XEvent`, ...) and no Wayland type
(`wl_display*`, `wl_surface*`, ...) ever appears in a public header
under `include/fdk/`. This is what makes "X11 and Wayland both
supported, application doesn't need to care which" actually true
rather than aspirational. See `src/platform/` (once it exists, Phase
2) for where that boundary is enforced in code — the platform layer's
job is specifically to translate backend-specific reality into the
backend-agnostic types in `fdk_types.h` and the future `fdk_input.h`/
`fdk_window.h`.

## Public vs. internal headers

- `include/fdk/*.h` — the public API. Installed by `make install`.
  Opaque object types only (see `docs/abi-policy.md`). Every function
  here has a doc comment stating its failure modes and, once relevant,
  its threading requirements (`docs/threading.md`).
- `src/**/*_internal.h` — implementation details, never installed,
  never included by anything outside `src/`. This is where the real
  struct layout behind each opaque public type lives (e.g.
  `src/core/context_internal.h` defines `struct fdk_context`).

## Current state (Phase 10 first slice — the accessibility core)

Implemented: `src/core/` (context lifecycle, logging, error codes,
allocation, versioning — Phase 1, plus the Phase 2 additions to
`context.c` and `context_internal.h` that perform the real backend
selection and the real poll()-based event loop, and the Phase 3
addition of `fdk_pump_events()` — the application-driven loop
primitive rendered apps are built on) plus `src/platform/x11/`,
`src/platform/wayland/` (optional — see `docs/build.md`'s "Optional
Wayland build"), `src/platform/wayland_disabled.c` (the build-time
stub used when Wayland dev headers aren't available), `src/window/`
(Phase 2, plus the Phase 4 widget glue: the window's lazily created
root widget, configure/expose handling, and input routing through
the tree before the application callback), `src/render/` (Phase 3:
the `fdk_surface` software renderer — pixel access, damage-tracked
presentation, a clip stack, offscreen surfaces, and the blending
primitive set — implemented on both backends via optional
`fdk_platform_ops` entries (`window_get_framebuffer`, damage-taking
`window_present`, the `window_frame_ready` pacing query, and the
`window_ever_presented` diagnostic seam); see
`docs/rendering.md` for the full design; on Wayland a present that
runs before the first xdg configure is DEFERRED and committed by
the backend at configure time — the deferred-first-frame contract
that keeps the application's show -> paint -> pump order mapping
windows on every compositor. Wayland buffer LIFECYCLE (1.1.6):
every wl_buffer rides a dedicated wl_event_queue, so
wl_buffer::release can be dispatched alone (never re-entrantly
running input/configure listeners); when every render slot is in
flight the acquisition path WAITS for a release with a CLASSIFIED
budget (1.1.7): pure hoarding (every busy slot at the current
size — a release there is recyclable) waits up to
FDK_WL_RELEASE_WAIT_MS, while CHURN (any busy slot at a wrong
size — the interactive-resize state) waits only one poll slice
(FDK_WL_CHURN_WAIT_MS) before reaping, because the 1.1.6 wait
exited only on a CURRENT-size release that a wrong-size pool can
never produce and burned its full budget per resize frame
(~10fps resizing with an idle CPU — the third Cinnamon Wayland
report). Inside the wait ANY release is progress: right-size
recycles, wrong-size is reaped on the spot and the slot reused;
after the budget an unreleased WRONG-SIZE slot is reaped as the
resize-churn escape (each buffer owns its memfd pool —
destroying a committed wl_shm buffer is legal), and only then
does the path refuse, rate-limited to one WARN per 2s episode.
`window_frame_ready`'s starvation guard additionally requires POOL
CAPACITY: a hidden surface still paces at the guard floor, but
"ready" with zero available buffers was the pacing lie that slow
compositors (Muffin's experimental Wayland session) turned into
frame-dropping WARN storms. The first configure EMITS
FDK_EVENT_WINDOW_CONFIGURE whenever it proposes a size different
from the creation size, because resize-at-map compositors
(kiosk-shell fullscreen, tiling WMs) state their size exactly there
and a suppressed event left the window layer laying out at the
creation size inside a full-compositor buffer (1.1.5, found live
under weston kiosk-shell), regression-tested in
`tests/test_wayland_integration.c`); on X11 the anti-flicker
background LIFECYCLE (1.1.4, found live on a compositing desktop):
top-levels are created with a white background pixel + NorthWest
bit gravity, and the FIRST framebuffer acquisition flips the
background to None — a window the app never renders into shows its
background (the documented 01_hello_world contract, matching
Wayland's committed solid-color buffer), while a rendered window is
never server-cleared again, and every resize step retains its old
pixels anchored top-left (the combination that removed the
white-flash-during-resize; a background clear is a full frame of
background between the WM's resize and the client's repaint).
The window layer closes the remaining gap itself:
`fdk_window_dispatch_event`'s tail repaints+ presents SYNCHRONOUSLY
when a configure/state-flip/first-expose damaged the tree — the
frame the compositor would otherwise composite between the resize
and the application's next loop pass is exactly where the flash
lived — and the same tail revalidates hover/cursor from the real
pointer position (see the pointer-affordance pair below). This
does not take over application paint pacing: only damage FDK's own
geometry acceptance caused goes out synchronously; everything the
application changes still waits for its own loop, exactly as
before), `src/layout/` (Phase 5: the
box layout engine — containers as widget subclasses over the
measure/arrange hooks — plus the window content glue), `src/widget/` (Phase
4: the retained-mode widget foundation — hierarchy, state, focus,
event routing with hover/grab/bubbling, invalidation, damage-driven
z-order painting on the clip stack, the subclass vtable, and the
reentrancy machinery that makes destroy-from-callback safe; see
`docs/roadmap.md`'s Phase 4 entry; the Phase 6 catalog — Label,
Button, Toggle, Checkbox, Radio, ProgressBar, Separator, Frame —
and the Phase 6 text foundation that backs their labels live here
and in `src/text/`), `src/theme/` (Phase 7: the theme API and
the strict `.fdk` parser — the built-in default is the v1 palette
exactly, and switching the default theme invalidates every live
tree through the widget core's root registry), and the Phase 8
decoration + window-management layer in `src/window/window.c` (an
FDK-drawn themed title band under the window's root with
vector-glyph minimize/maximize-restore/close buttons, double-click
maximize, and FDK-drawn resize edges, over a set of new OPTIONAL
platform ops: `window_set_wm_decorations` — X11 _MOTIF_WM_HINTS,
Wayland xdg-decoration set_mode, or (1.1.5) plain success without
the protocol for the client-side direction: a compositor that
doesn't advertise zxdg_decoration_manager_v1 never draws chrome
itself — client-side is the xdg-shell default — so FDK's band is
the only chrome that can exist and nothing can stack; only the
platform-chrome direction honestly reports FDK_ERR_UNSUPPORTED
then; `window_set_maximized` /
`window_set_minimized` — EWMH _NET_WM_STATE messages + PropertyNotify
tracking on X11 (with honest bare-X fallbacks where FDK is its own
WM; the fallback never runs under a detected WM, because it
optimistically dispatches the state flip as if FDK's own action were
the outcome — a lie under a WM that clamps or reinterprets the
geometry request, and the source of the 1.1.3 maximized-state
desync), xdg_toplevel requests on Wayland (1.1.5 gates client-driven
`fdk_window_resize` while the configure states say MAXIMIZED or
FULLSCREEN — compositor-owned geometry must be committed exactly as
configured or strict compositors kill the connection; the FULLSCREEN
flag is internal-only, purely the resize gate, until a public
fullscreen API exists); `window_begin_move` /
`window_begin_resize` — _NET_WM_MOVERESIZE / xdg_toplevel.move/
resize, handing interactive drags to the WM/compositor where the
platform supports it (the X11 implementation releases the press's
implicit pointer grab BEFORE sending _NET_WM_MOVERESIZE — the WM's
own XGrabPointer gets AlreadyGrabbed otherwise and the drag never
starts; and the window layer cancels the widget tree's implicit
grab at the same handover, because the WM consumes the button
release and the tree's press-to-release pairing would stay broken
— see fdk__widget_tree_cancel_grab). The press filter's ORDER
matters as much as the ops (1.1.7, the third Cinnamon Wayland
report): the compositor-driven begin_resize is tried FIRST — it
needs no origin knowledge, the WM owns the geometry from the
serial onward — while the origin gate (origin-moving edges need
window_get_position) guards ONLY the FDK-driven fallback it was
built for. 1.1.6 ran the gate first, and since the Wayland ops
table has no window_get_position at all, every origin-moving
edge (N/NE/NW/W/SW) fell through the filter into the deco
band's move path: a titlebar-edge press MOVED the window while
the cursor promised a resize; `window_move_resize_to` for
FDK-driven atomic resize drags. The 1.1.4 pointer-affordance pair
completes the set: `window_query_pointer` (X11 XQueryPointer
window-local + bounds check; Wayland's seat cache — pointer_focus +
the last surface-local position) lets the window layer RE-DERIVE
hover after geometry changes, because a window that moves/resizes
under a STATIONARY pointer generates no motion event — the maximize
button flies right, the pointer stays put, and a highlight computed
against the old geometry sticks forever without the revalidation
(query the real pointer, route it as if a motion/leave had arrived;
the application's event callback never sees these synthesized
positions). `window_set_cursor` (X11: lazily created XCreateFontCursor
glyphs from the server's built-in cursor font — core protocol, no
libXcursor — cached per connection and freed at disconnect; Wayland
since 1.1.6: a hand-rolled XCursor THEME loader in
src/platform/wayland/wayland_cursor.c — $XCURSOR_PATH or the
libxcursor search roots walked through index.theme `Inherits=`
chains to the "default" theme, closest-size image, ARGB upload
over wl_shm, one cached cursor surface per connection,
wl_pointer.set_cursor citing the live input serial; no libxcursor
dependency, and a machine with no cursor theme anywhere keeps the
compositor's own arrow — honest degradation, never a hidden
cursor) maps the same compass vocabulary as begin_resize to
directional cursors, so FDK's edge zones advertise "this edge
drags" BEFORE any button is held — the affordance a WM frame's
borders give for free. The cursor path applies the SAME
capability predicate as the press filter
(window_edge_needs_origin, shared by both): a backend that can
neither hand the drag off (no begin_resize) nor compute its own
origin (no window_get_position) must not advertise its
origin-moving edges — the cursor may never promise what a press
cannot deliver. The ENTER event carries its surface-local
position on both backends (the Wayland seat handler forgot it
until 1.1.6 — every entry armed the NW cursor and seeded hover at
the top-left corner until the first motion). FDK_EVENT_WINDOW_STATE /
FDK_EVENT_WINDOW_DECORATION report platform truth, never request
optimism — see platform_internal.h, which documents the EWMH vs
xdg-decoration vs bare-X differences instead of assuming them away),
`src/core/clipboard.c` + the backends' clipboard implementations
(Phase 9: `fdk_clipboard_set_text`/`fdk_clipboard_get_text` over one
OPTIONAL platform-op pair — ICCCM CLIPBOARD ownership with a
helper window and a bounded non-re-entrant SelectionNotify wait on
X11, wl_data_device/wl_data_source on Wayland), and the Phase 9
popup + advanced-widget layers: `fdk_window_create_popup` in
`src/window/window.c` (parent-relative popup windows stacked in a
family chain — destroying the parent force-destroys the chain
deepest-first — over two more OPTIONAL platform ops:
`window_popup_regrab`, re-asserting a popup's grab after a popup
stacked above it closes because server/compositor grabs do not
stack, and `window_set_modal`, taking a pointer+keyboard grab on a
TOPLEVEL for modal dialogs on X11 — Wayland has no toplevel-grab
protocol, so dialogs there are honestly non-modal; popups grab at
show: override-redirect + XGrabPointer/XGrabKeyboard on X11,
xdg_positioner + xdg_popup + grab citing the last input serial on
Wayland, with outside-press and Escape delivered as
FDK_EVENT_WINDOW_CLOSE_REQUEST), and the advanced widget catalog
in `src/widget/` (Entry — cluster-safe cursor, selection algebra,
real clipboard integration, IME preedit groundwork; ScrollView +
overlay Scrollbars; List and Tree with the full modifier grammar
over the pointer-event modifiers bitmask; Slider, SpinButton,
Toolbar, Notebook, Canvas; the three-layer Menu system — model /
view / bar — with nested submenu chains riding the popup layer;
ComboBox in non-editable and editable modes, its dropdown being
the menu view machinery; and the modal message dialog over
`fdk_dialog_show_message`, non-blocking by design). Popups and
dialogs are TOOLKIT-OWNED windows: the window layer's
owned-window path paints them itself instead of invoking the
application's window callback — the widget→window back-edge that
lets a widget open its own toplevel from inside an event handler.
See `docs/roadmap.md`'s Phase 9 entry for the full inventory.
`src/widget/a11y.c` + `include/fdk/fdk_a11y.h` (Phase 10 first
slice: the accessibility core — the a11y tree IS the widget tree;
`fdk_a11y_describe` snapshots role/name/description/states/bounds/
value, `fdk_a11y_subscribe` delivers children/state/name/bounds/
value change notifications with the widget core's mutation points
firing them, and `fdk_a11y_perform` drives widgets through their own
public semantics — the seam the embedded narrator (1.1.0) and test
drivers sit on; class-level descriptors ride a new `.a11y` field on
the widget class vtable, and every catalog widget plus window roots
describe themselves). `src/widget/a11y_narrator.c` (1.1.0: the
embedded screen reader core — the no-bus policy made real; an
ordinary subscriber that composes utterances from live describe()
snapshots and speaks through an application-wired sink, so screen
reader access needs no registry, no bus, and no bridge process).
`fdk_init()` performs a real
platform connection with backend auto-detection; `fdk_run()` is a
real `poll()`-based event loop that exits on `fdk_quit()` or when
the last top-level window closes; windows can be created, shown,
resized, and receive real translated input/configure/close events
on both backends; applications can draw real pixels into a window's
surface and present them — sending only what changed — on either
backend (`examples/02_software_render.c`); and applications can now
build widget trees that FDK itself hit-tests, focuses, repaints,
and presents (`examples/03_widgets.c`).
See `docs/roadmap.md`'s entries for the precise, honest lists of
what is and isn't covered — in particular i18n (the whole second
half of Phase 10), per-item a11y nodes for painted-row containers
(menu items, notebook tabs), and IME
protocols are recorded as not-yet (each with its reason; external
assistive-technology bridging is an application-side consumer of
the public seam per the no-bus policy, not toolkit work), while the
renderer (damage tracking, clip stack, offscreen surfaces, images,
transforms, AA, MIT-SHM, HiDPI), the text stack (subpixel
positioning), the widget foundation + layout engine + catalog +
theme engine, decorations + window management, the Phase 9 advanced
widgets, and the a11y core are in and tested; the widget layer —
backend-neutral by construction — is verified headless + on X11,
and the Wayland backend has real integration tests against sway
headless (xdg-decoration negotiation, menu popups through the
compositor's real seat, dialogs included — see `docs/testing.md`;
the weston 14 in Debian ships no xdg-decoration, which is why the
verification compositor is sway).

### Widget layer specifics (Phase 4)

The widget layer (`src/widget/`) sits strictly above windowing and
rendering: it consumes `fdk_event_data` (translated input) and draws
through `fdk_surface` primitives plus the clip stack, so it contains
no backend code at all — which is why `tests/test_widget.c` can
prove the entire layer with offscreen surfaces and no display. The
window glue lives in `src/window/window.c` (not in `src/widget/`):
the window owns its root widget, resizes it on configure, invalidates
it on expose, routes input events through
`fdk_widget_tree_handle_event` before the application's window
callback (widget-consumed events don't re-deliver — documented in
`fdk_widget.h`), and `fdk_window_paint` wraps tree-paint + present.

One deliberate boundary: the public `fdk_widget` type is opaque, so
the embed-fdk_widget-as-first-member subclassing pattern is internal
to `src/` (via `widget_internal.h`); applications extend widgets via
callbacks, user data, and the base style setters until the ABI
freeze (see `docs/abi-policy.md`).
