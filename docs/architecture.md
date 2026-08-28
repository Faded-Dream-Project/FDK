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
`window_present`, and the `window_frame_ready` pacing query); see
`docs/rendering.md` for the full design), `src/layout/` (Phase 5: the
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
Wayland xdg-decoration set_mode; `window_set_maximized` /
`window_set_minimized` — EWMH _NET_WM_STATE messages + PropertyNotify
tracking on X11 (with honest bare-X fallbacks where FDK is its own
WM), xdg_toplevel requests on Wayland; `window_begin_move` /
`window_begin_resize` — _NET_WM_MOVERESIZE / xdg_toplevel.move/
resize, handing interactive drags to the WM/compositor where the
platform supports it; `window_move_resize_to` for FDK-driven atomic
resize drags. FDK_EVENT_WINDOW_STATE / FDK_EVENT_WINDOW_DECORATION
report platform truth, never request optimism — see
platform_internal.h, which documents the EWMH vs xdg-decoration vs
bare-X differences instead of assuming them away), `src/core/
clipboard.c` + the backends' clipboard implementations (Phase 9:
`fdk_clipboard_set_text`/`fdk_clipboard_get_text` over one
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
public semantics — the seam a future AT-SPI2 bridge or a test driver
sits on; class-level descriptors ride a new `.a11y` field on the
widget class vtable, and every catalog widget plus window roots
describe themselves).
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
(menu items, notebook tabs), the AT-SPI2 bridge itself, and IME
protocols are recorded as not-yet (each with its reason), while the
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
