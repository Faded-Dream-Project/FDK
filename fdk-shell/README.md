# fdk-shell — layer-shell surfaces for FDK

fdk-shell is the first **add-on package** built on FDK's extension
API (`fdk/extend.h`, FDK 1.1.0): panel/bar/notification-style
surfaces that live outside the normal window stack — desktop
background layers, docks, top bars, overlays — behind ONE API on
both of FDK's backends:

- **Wayland**: wlr-layer-shell v4 (the protocol sway, labwc, river
  and the wlroots family speak), via wayland-scanner bindings
  vendored in `src/wl/protocol/`.
- **X11 / XLibre**: a `_NET_WM_WINDOW_TYPE_DOCK` window with
  `_NET_WM_STRUT_PARTIAL` reservation hints, sticky across workspaces
  (`_NET_WM_DESKTOP` = 0xFFFFFFFF + `_NET_WM_STATE_STICKY`), and
  per-level stacking state (`_NET_WM_STATE_ABOVE`, `SKIP_TASKBAR`,
  `SKIP_PAGER`) — the EWMH world's equivalent layering semantics,
  honored by OpenBox, fluxbox, IceWM and friends.

Version 0.1.0. Own build, own version, own pkg-config file — a
separate package that happens to live in FDK's tree for now, ready
to move to its own repository unchanged.

## Dependencies

- **FDK 1.1.0 or newer** — the extension API v1
  (`fdk_ext_api_version() >= 1`).
- X11: libX11 (the backend speaks it through the extension API's
  documented `Display*` accessor).
- Wayland: wayland-client + wayland-scanner at build time (the
  generated bindings are committed, so the scanner is optional at
  build; the vendored protocol XMLs are the source).

The X11 and Wayland libraries are **fdk-shell's own dependencies** —
the same license any native client has. What fdk-shell touches of
FDK is exactly the installed public headers and library: the
`tests/isolation.sh` proof installs FDK into a throwaway prefix and
builds this package against that prefix ALONE (no other FDK include
path exists on the compile line — a private reach would be a compile
error), links and runs the demo from the installed stack, and
verifies every imported `fdk_*` symbol resolves from `libfdk.so.1`.

## Building

```sh
make                # static + shared library + demo (debug, ASan)
make release        # optimized
make test           # X11 backend tests (auto-starts a throwaway Xvfb)
make test-wayland   # Wayland tests against $WAYLAND_DISPLAY (self-skips)
make isolation      # the prefix-isolation build proof
make examples       # the demo bar
make install PREFIX=/usr   # headers + libs + pkg-config
```

The default build links the SIBLING FDK checkout (`../build/` +
`../include/`); point `FDK_CFLAGS` / `FDK_LIBS` at an installed FDK
instead — that is what the isolation proof does:

```sh
make FDK_CFLAGS="-I/opt/fdk/include" FDK_LIBS="-L/opt/fdk/lib -lfdk"
```

Backend knobs: `FDKSHELL_DISABLE_WAYLAND=1` (X11 only — for
machines without wayland-client dev files), `FDKSHELL_DISABLE_X11=1`
(Wayland only).

## Workspaces and stacking (the per-backend truth)

**Wayland**: a layer surface is per-OUTPUT, not per-workspace — every
wlroots compositor shows it on all workspaces by construction. There
is nothing to configure and nothing to prove at the API level; the
sway-headless test (`tests/test_wl.c`) switches workspaces through
the compositor's IPC and pins that nothing about the surface changes
(no close, no reconfigure, presenting continues).

**X11**: the same behavior is arranged through EWMH hints, both ways
at once (properties BEFORE mapping — the initial-state contract WMs
honor at map time — and root client messages AFTER mapping, for WMs
that came late or only honor the request shape):

- `_NET_WM_DESKTOP` = `0xFFFFFFFF` ("all desktops") and
  `_NET_WM_STATE_STICKY` — the layer follows the user across
  workspaces and is never unmapped by a desktop switch (which also
  protects its pixels: an unmapped window's contents are undefined
  once remapped — the ghost-bar bug).
- `_NET_WM_STATE_ABOVE` for the TOP and OVERLAY layers;
  OVERLAY additionally carries `_NET_WM_STATE_SKIP_TASKBAR` /
`_NET_WM_STATE_SKIP_PAGER` — the strongest EWMH stacking behavior
  available.

| layer level | Wayland (protocol truth)     | X11 (EWMH mapping)                            |
|-------------|------------------------------|------------------------------------------------|
| background  | below everything             | dock type only (see the honest limits below)  |
| bottom      | below normal windows         | dock type only (same)                          |
| top         | docks and bars               | dock + `_NET_WM_STATE_ABOVE`                   |
| overlay     | above everything, even       | dock + ABOVE + SKIP_TASKBAR + SKIP_PAGER       |
|             | fullscreen                   | (strongest available; see the fullscreen note) |

**The fullscreen-over-docks limit (X11)**: EWMH window managers draw
fullscreen windows over docks BY DESIGN — the spec's fullscreen
stacking rule. A dock carrying `_NET_WM_STATE_ABOVE` is kept above
normal windows (and, on many WMs, above plain fullscreen windows),
but EWMH has NO state that guarantees "above fullscreen windows" the
way layer-shell's overlay layer does. This is a protocol ceiling,
not an fdk-shell defect; on Wayland the overlay level really does
sit above fullscreen surfaces.

**What a custom WM would need to honor a true overlay layer**: EWMH
alone cannot express it, so a WM that wants to must agree on a
private contract. The minimal sufficient shape (any of these three,
in increasing strength):

1. **A private stacking state atom** — e.g. honor
   `_NET_WM_STATE_ABOVE` on `_NET_WM_WINDOW_TYPE_DOCK` windows as
   "above fullscreen, below nothing" (today's common WM behavior
   puts fullscreen above even ABOVE docks). One stacking rule, no
   new windows-must-understand protocol.
2. **A private layer property** — the WM reads a CARDINAL such as
   `_FDK_WM_LAYER` (0..3, background/bottom/top/overlay) off dock
   windows and inserts them into its stack at that layer, with
   overlay above fullscreen and system-modal windows. fdk-shell
   would set it alongside the EWMH hints the day a WM adopts it.
3. **The full layer-shell stacking model** — per-output layer
   domains with the six-level order (background, bottom, normal,
   top, fullscreen, overlay), exclusive-zone-aware reservation, and
   keyboard-mode semantics — i.e. implement what wlroots does. The
   `fdk_shell_layer_desc` fields map onto it 1:1 (they were designed
   from zwlr_layer_shell_v1).

The same WM should also honor the existing pieces this package
already speaks: `_NET_WM_STRUT_PARTIAL` reservations, sticky
desktops, and — for pixel correctness on unocclusion — sending
`Expose` to dock windows when their content is lost (the section
below).

## Content loss and the EXPOSE contract

A layer's pixels can be lost underneath the application on any
backend: a desktop switch on a WM without sticky support, a
fullscreen window that covered the bar closing, an unocclusion. FDK
delivers `FDK_EVENT_WINDOW_EXPOSE` through the layer's event
callback, and fdk_event.h's contract for rendered surfaces is
**re-present** — the damage-tracking model otherwise concludes
nothing changed and paints nothing. The demo bar's handler is the
canonical shape:

```c
case FDK_EVENT_WINDOW_EXPOSE:
    bar_paint(app);          /* re-fill + fdk_surface_present */
    break;
```

On Wayland the compositor retains buffer content, so exposes are
rare (the buffer is re-attached); on X11 they are the difference
between a bar and a ghost (see `tests/test_x11.c`'s desktop-switch
rig: a full EWMH window manager runs inside the test, switches
`_NET_CURRENT_DESKTOP` through the pager-shaped client message, and
checks the bar's pixels on the round trip).

## The popup contract on real desktops (1.1.0-rc1 lessons)

Two behaviors were found live (Openbox on X11) and are now the
package's contract, each pinned by tests above:

- **The popup's grab is POINTER-ONLY.** An active XGrabKeyboard
  deactivates the window manager's passive grabs — Super+number
  workspace switching stopped working while a dropdown was open.
  Popups still receive keys (arrows, Escape) through focus:
  FDK takes XSetInputFocus on the popup while it is open and
  restores the previous focus on release. Outside-click dismissal
  works exactly as before through the pointer grab.
- **A desktop switch dismisses open popups.** The popup belongs to
  the desktop it opened on; `_NET_CURRENT_DESKTOP` changes release
  the grab and deliver the popup's close request (the sticky LAYER
  rides through untouched — that contract is unchanged).
- **The layer positions itself.** The window carries
  USPosition/USSize WM_NORMAL_HINTS with the anchoring math; a
  dock-typed window without position hints is "unplaced", and WMs
  run their placement policy (Openbox centers it mid-screen).
- **FDK_POPUP_TRACE=1** (FDK's env var) narrates the popup
  lifecycle — create, map, grab verdicts, focus takeover, first
  present, and the REASON each release happened (outside-click /
  escape / unmap / destroy / desktop-change / watchdog) with the
  grab-held duration. The first tool for any "the dropdown
  misbehaves" report.

## A bar in thirty lines (of the parts that matter)

```c
#include <fdk-shell/fdk-shell.h>

/* The add-on's version gate — the documented pattern. */
#if !defined(FDK_EXT_API_VERSION) || FDK_EXT_API_VERSION < 1
#error "fdk-shell needs FDK 1.1.0 (extension API v1)"
#endif

fdk_context *ctx = NULL;
fdk_init(&ctx, NULL);                 /* auto backend */

fdk_shell *shell = NULL;
fdk_shell_init(ctx, &shell);          /* binds layer-shell / docks */

fdk_shell_layer_desc desc = {0};
desc.struct_size     = sizeof(desc);
desc.layer           = FDK_SHELL_LAYER_TOP;
desc.anchors         = FDK_SHELL_ANCHOR_TOP
                     | FDK_SHELL_ANCHOR_LEFT
                     | FDK_SHELL_ANCHOR_RIGHT;
desc.height          = 32;            /* logical pixels   */
desc.exclusive_zone  = -1;            /* auto: reserve our extent */

fdk_shell_layer *bar = NULL;
fdk_shell_layer_create(shell, &desc, &bar);
fdk_shell_layer_set_event_callback(bar, on_event, &app);
fdk_shell_layer_show(bar);

for (;;) {                            /* the ordinary FDK loop */
    fdk_pump_events(ctx, 100);
    /* render via fdk_shell_layer_get_surface() + the fdk_surface
     * API; present; handle events in on_event */
}

/* A dropdown (see examples/bar.c for the full working demo):
 *
 *   fdk_shell_popup_create(bar, x, y, w, h, &pop);
 *   fdk_shell_popup_set_event_callback(pop, on_pop_event, &app);
 *   fdk_shell_popup_show(pop);
 *   ... FDK_EVENT_WINDOW_CLOSE_REQUEST on outside click / Escape ...
 *   fdk_shell_popup_destroy(pop);
 */
```

The full working demo — hover, press, toggle, item pick, outside-
click and Escape dismissal — is `examples/bar.c`
(`./build/examples/bar`, or `./build/examples/bar 5` to auto-quit
after 5 seconds).

## API surface (0.1.0)

- `fdk_shell_init` / `fdk_shell_shutdown` / `fdk_shell_version`
  / `fdk_shell_backend_supported`
- Outputs: `fdk_shell_output_count`, `fdk_shell_output_name`
- Layers: `fdk_shell_layer_create` / `destroy` / `get_surface` /
  `set_event_callback` / `get_size` / `show` / `hide` /
  `set_cursor` (desc: layer, anchors, margins, exclusive zone,
  keyboard mode, desired size, output index)
- Popups: `fdk_shell_popup_create` / `destroy` / `get_surface` /
  `set_event_callback` / `show` / `hide` / `set_cursor`

`include/fdk-shell/fdk-shell.h` documents every function and the
desc struct field-by-field, including the per-backend notes.

**The hover cursor** (`fdk_shell_layer_set_cursor` /
`fdk_shell_popup_set_cursor`, 1.1.0-rc1): the arrow or the pointing
hand while the pointer hovers the surface — re-applied automatically
on every entry (Wayland cursor state does not survive leaving a
surface; the W2 report was a bar that kept the resize cursor until
the pointer left). The demo bar shows the worked example: the hand
over its menu button, the arrow everywhere else.

## Honest limits (0.1.0)

- **Layer levels**: background / bottom / top / overlay are
  protocol truth on Wayland; on X11 they ride the stacking state of
  the table above (ABOVE/SKIP_* per level). BACKGROUND/BOTTOM have
  no below-normal-windows EWMH mapping — docks stack above normal
  windows on every EWMH WM — and OVERLAY cannot be guaranteed above
  fullscreen (the documented protocol ceiling). No fake fidelity.
- **Per-output selection**: full on Wayland (the compositor's
  outputs, enumerated in discovery order). X11 reports exactly ONE
  output (the screen) — multi-monitor docks are a RandR feature
  outside this package's X11 scope.
- **Keyboard ON_DEMAND**: layer-shell v4 on Wayland; on X11 it
  degrades to "focusable, best-effort" (there is no per-surface
  on-demand focus protocol). EXCLUSIVE is a real XGrabKeyboard.
- **Wayland compositors without wlr-layer-shell** (GNOME, KDE's
  kwin): `fdk_shell_init` fails with `FDK_ERR_UNSUPPORTED` — check
  `fdk_shell_backend_supported()` first when a fallback matters.
- **Live re-anchoring**: parameters are create-time (layer-shell's
  double-buffered initial state; X11 geometry); destroy + re-create
  is the documented idiom for a moved panel.
- **Unmapping a layer on Wayland**: destroy + re-create the layer
  (the role owns mapping; `hide` is an X11 concept there and a
  documented no-op).

## Testing

- `tests/test_x11.c` — Xvfb: dock type, strut math (auto, margins,
  explicit zone), anchor geometry (stretch / corner / centered),
  server-side pixel readback, the EXCLUSIVE grab verified by a
  second client's grab FAILING, popup position/render/dismissal/
  parent-sweep; the sticky/stacking state as pre-map properties
  (per level); the desktop-switch rig — a minimal EWMH window
  manager running INSIDE the test (root substructure redirect,
  `_NET_SUPPORTED`, per-window `_NET_WM_DESKTOP`, pager-shaped
  `_NET_CURRENT_DESKTOP` client-message switching) proving the
  layer never unmaps across switches, a control window does, the
  post-map state messages restore what the "late WM" deleted, and
  the EXPOSE repaint returns the bar's pixels after the round trip;
  and the 1.1.0-rc1 live-desktop regressions: the layer's
  USPosition/USSize hints (the self-positioned dock contract — an
  unpositioned dock is "unplaced" and real WMs move it), the
  popup's pixels ON SCREEN (a ROOT-region readback proves stacking,
  not just the window's own buffer) STAYING until a real dismissal,
  the WM's PASSIVE grabs firing while a popup is open and after its
  dismissal (the in-test WM registers a Super+number XGrabKey; a
  XTest-synthesized Super+1 must REACH the WM through the open
  dropdown — the pointer-only grab fix), and a desktop switch
  dismissing an open popup with the sticky layer riding through.
- `tests/test_wl.c` — against a real layer-shell compositor (the
  maintainer rigs run sway headless): layer handshake,
  configure-driven size, present, popup adoption, every layer level
  + keyboard mode, clean teardown, the workspace-switch
  confirmation (no close/reconfigure across switches, presenting
  continues; swaymsg is the control channel), and the eager-paint
  startup regression (painting IMMEDIATELY after show() — before
  the role's configure can arrive — must survive: FDK's present
  gate holds the buffer until the post-ack report). Self-skips
  without `$WAYLAND_DISPLAY`.
- `tests/bar_rig.sh` (`make test-bar-rig`) — the REAL-COMPOSITOR
  bar rig: the actual demo bar against headless sway AND labwc
  (each self-skips when not runnable; labwc needs an Xwayland on
  PATH) for several seconds — alive, pixels on the output (grim,
  when available), the WAYLAND_DEBUG trace clean (the configure
  arrived, no protocol error, and no `wl_surface.attach` before the
  first `ack_configure`) — plus the compositor-kill leg: kill the
  compositor mid-run and the bar must exit by itself, non-zero,
  with exactly one fatal-dispatch line in a bounded log.
- `tests/isolation.sh` — the prefix-isolation build proof (see
  Dependencies above), including the ELF boundary: the shared
  library carries a real `NEEDED: libfdk.so.1` entry, every
  imported `fdk_*` symbol resolves from it, and the exports are
  `fdk_shell_*` only.

Not covered in this environment (documented for the release notes):
outside-click dismissal ON WAYLAND needs a real input serial (the
headless seat produces no pointer events; the machinery is FDK's
existing, menu-tested xdg_popup grab path), and visual readback on
Wayland would need the compositor's screenshot channel.

## Packaging (the FDK dependency, declared everywhere)

fdk-shell ships as its own package in every distribution family FDK
does, each declaring a REAL, VERSIONED dependency on the FDK
runtime — pinned at the FDK version that introduced the extension
API this package is built against (extension API v1 first shipped
in FDK 1.1.0-rc1; the API is append-only within the SONAME, so the
floor never moves backwards):

| family | runtime package | dependency declaration                    |
|--------|-----------------|--------------------------------------------|
| deb    | `libfdk-shell0` + `fdk-shell-dev` | `Depends: libfdk1 (>= 1.1.0~rc1)` (dev: `fdk-dev (>= …)`) |
| rpm    | `libfdk-shell0` + `fdk-shell-devel` | `Requires: libfdk1 >= 1.1.0~rc1` |
| arch   | `fdk-shell` (single package) | `depends=(fdk>=1.1.0rc1 …)` |
| apk    | `fdk-shell` + `fdk-shell-dev` | `depends="fdk>=1.1.0_rc1"` (+ so: auto-deps) |
| xbps   | `fdk-shell` + `fdk-shell-devel` | `depends="fdk-libs>=1.1.0rc1"` |

(The pre-release spellings differ per grammar — the same
`fdk_packver` mapping FDK's own packaging uses; plain releases are
byte-identical across families.)

Every family's install-test proves the dependency is REAL, in the
native package manager: installing fdk-shell's runtime ALONE is
refused (unmet `libfdk1`/`fdk`/`fdk-libs`), the full set installs
together with resolution, pkg-config discovers the
`fdk-shell` -> `fdk` chain, and the graphical smoke (a layer
created, shown, presented, verified server-side for dock type +
sticky + strut) runs from the installed stack. The build itself
compiles against the INSTALLED FDK packages — the isolation proof
re-run inside every packaging container.

## License

Same as FDK (see FDK's LICENSE) — this package is part of the same
project, distributed separately.
