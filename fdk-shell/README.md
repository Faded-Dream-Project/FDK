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
  `_NET_WM_STRUT_PARTIAL` reservation hints — the EWMH world's
  equivalent layering semantics, honored by OpenBox, fluxbox, IceWM
  and friends.

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
  `set_event_callback` / `get_size` / `show` / `hide`
  (desc: layer, anchors, margins, exclusive zone, keyboard mode,
  desired size, output index)
- Popups: `fdk_shell_popup_create` / `destroy` / `get_surface` /
  `set_event_callback` / `show` / `hide`

`include/fdk-shell/fdk-shell.h` documents every function and the
desc struct field-by-field, including the per-backend notes.

## Honest limits (0.1.0)

- **Layer levels**: background / bottom / top / overlay are
  protocol truth on Wayland; on X11 they ride the stacking the WM
  gives dock-typed windows (the deltas are Wayland-only — no fake
  fidelity).
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
  parent-sweep.
- `tests/test_wl.c` — against a real layer-shell compositor (the
  maintainer rigs run sway headless): layer handshake,
  configure-driven size, present, popup adoption, every layer level
  + keyboard mode, clean teardown. Self-skips without
  `$WAYLAND_DISPLAY`.
- `tests/isolation.sh` — the prefix-isolation build proof (see
  Dependencies above).

Not covered in this environment (documented for the release notes):
outside-click dismissal ON WAYLAND needs a real input serial (the
headless seat produces no pointer events; the machinery is FDK's
existing, menu-tested xdg_popup grab path), and visual readback on
Wayland would need the compositor's screenshot channel.

## License

Same as FDK (see FDK's LICENSE) — this package is part of the same
project, distributed separately.
