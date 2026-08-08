# FDK Architecture

## Layering

FDK is organized in strict layers. Each layer may depend downward on
the layers below it, and never upward:

```
Application code
      |
Widgets  (src/widget/)         — Phase 4-5, 8
      |
Layout   (src/layout/)         — Phase 4
      |
Rendering abstraction (src/render/) — Phase 3
      |
Windowing (src/window/)        — Phase 2, decorations in Phase 7
      |
Platform abstraction (src/platform/) — Phase 2
      |         \
   X11/XLibre   Wayland
```

Alongside this vertical stack, several cross-cutting subsystems are
used by multiple layers rather than sitting in the stack themselves:

- **Core** (`src/core/`) — init/shutdown lifecycle, logging, error
  handling, memory allocation. Everything depends on this; it depends
  on nothing else in FDK. (Phase 1 — this is what currently exists.)
- **Theme** (`src/theme/`) — the `.fdk` format parser/loader and theme
  API. Consumed by rendering and widgets, doesn't depend on either.
  (Phase 6)
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

## Current state (Phase 1 — Foundation)

Implemented: `src/core/` — context lifecycle (`fdk_init`/`fdk_run`/
`fdk_quit`/`fdk_shutdown`), logging, error result codes, allocation
helpers, versioning. No platform connection is attempted yet;
`fdk_run()` currently returns immediately and logs a warning saying
so, honestly, rather than pretending to block on a window system that
isn't wired up. See `docs/roadmap.md` for what's next.
