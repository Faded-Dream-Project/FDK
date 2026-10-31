<div align="center">

<img src="examples/data/fdk_logo.png" width="96" alt="FDK logo — a crescent moon over a dreamy night sky fading from soft purple into deep dark blue"/>

# Faded Dream ToolKit (FDK)

**Part of the Faded Dream Project**

FDK is a general-purpose native GUI toolkit for modern Linux desktops,
developed as part of the
[Faded Dream Project](https://github.com/Faded-Dream-Project/Faded-Dream-dotfiles).

[![License: FDK Proprietary v1.0](https://img.shields.io/badge/license-FDK%20Proprietary%20v1.0-blueviolet?style=flat-square)](LICENSE)
[![Built with C](https://img.shields.io/badge/built%20with-C-orange?style=flat-square&logo=c)](https://www.open-std.org/jtc1/sc22/wg14/)
[![Standard: C17](https://img.shields.io/badge/standard-C17-orange?style=flat-square&logo=c)](https://www.open-std.org/jtc1/sc22/wg14/)
[![Backends: X11 and Wayland](https://img.shields.io/badge/backends-X11%20%7C%20Wayland-blueviolet?style=flat-square)](docs/architecture.md)
[![Status: in development](https://img.shields.io/badge/status-in%20development-orange?style=flat-square)](docs/versioning.md)
[![Part of Faded Dream](https://img.shields.io/badge/part%20of-Faded%20Dream-purple?style=flat-square)](https://github.com/Faded-Dream-Project/Faded-Dream-dotfiles)

</div>

---

FDK targets the practical role GTK and Qt currently serve for
applications that choose to target it: real X11 and Wayland backends
that talk directly to Xlib and libwayland-client, its own
damage-tracked renderer with a GPU present lane, its own widget,
layout, text, and theme engines, and FDK-drawn window decorations —
with no GTK/Qt dependency and a minimal, always-justified dependency
set (`docs/dependencies.md`).

> **Building FDK:** `main` is the development branch — the only
> branch, and the one to build from. The project is **in development,
> no stable release yet**: every push to `main` is CI-gated, but
> "green on main" means "not broken", not "released".

**Status:** **in development — no stable release yet.** The public
**extension API** (`fdk/extend.h`, `docs/EXTENDING.md`) is the
landmark of the current tree: a small, documented, stable, versioned
surface that add-on packages build on so FDK stays simple and becomes
the root other libraries grow from. The first add-on, **fdk-shell**
(layer-shell surfaces — panels, docks, overlays — on wlr-layer-shell
and EWMH dock hints, behind one API), lives in `fdk-shell/` as a
separate package that compiles against FDK's installed public headers
and library only (the `fdk-shell/tests/isolation.sh` proof). The
versioning policy and the SONAME scheme (`libfdk.so.1`, ABI
append-only) are development-time policy — they govern how the tree
evolves as it heads toward a first stable release, which the owner
will cut when it is genuinely ready. A cross-distribution packaging
+ CI pipeline is built and kept green on every push as a validation
check (`docs/packaging.md`); nothing is published anywhere. The
phase-by-phase account of what is and isn't covered lives in
`docs/roadmap.md` and `PROGRESS.md`.

FDK is **distro-agnostic**: it builds and runs on any modern Linux
distribution where X11, Wayland, and POSIX are available.

**Fonts:** FDK bundles no font (licensing posture). Applications
discover a system UI font at run time through fontconfig, the standard
font directories, or `$FDK_FONT_FILE` / `$FDK_FONT_DIRS` overrides —
see `docs/text.md`.

## Features

Everything below is implemented and covered by the two-tier test
suite (`docs/testing.md`).

- **Native X11 and Wayland backends** — real Xlib and
  libwayland-client integration, popup surfaces, HiDPI, and a
  poll()-based event loop you can either own or hand to `fdk_run()`.
- **Rendering** — a damage-tracked software rasterizer (gradients,
  antialiased shapes, alpha compositing, transforms, image decode and
  PNG encode), plus a hybrid GPU present lane (X11/GLX since 1.4.18,
  Wayland/EGL since 1.2.0): where a hardware GL renderer exists,
  frames reach the screen through GLX or EGL — byte-identical to the
  CPU path, with a guaranteed mid-frame fallback (`FDK_NO_GL` /
  `FDK_GL_FORCE` knobs, `docs/rendering.md`).
- **Widgets** — a full catalog: buttons (with role vocabulary),
  entries (password / read-only / max-length), TextView editor, List,
  Tree, IconView, Notebook with drag-reorderable tabs, menus and
  submenus, toolbar, Canvas, dialogs (including a real file dialog
  with list/icon browsing), Font and Color choosers, Picture, sliders,
  spinners, level bars, revealers, and more.
- **Desktop icon themes in the file dialogs** — the pickers resolve
  their icons through an internal freedesktop icon-theme engine: the
  user's selected theme (GTK/KDE settings or the system default
  channel, no bus) with full inheritance and fallback,
  per-file-type icons (folders, images, audio, video, archives,
  source, executables), symbolic icons tinted with the active
  theme's ink — PNG through the vendored decoder and SVG rasterized
  at the requested size through the vendored nanosvg, with broken
  assets refused so they never shadow the chain behind them, and
  FDK's procedural glyphs as the honest floor
  (`include/fdk/fdk_dialog.h`'s icon-themes section states the
  exact limits).
- **Layout** — box (horizontal/vertical) and grid containers with
  margins, expansion, baseline and cross-axis alignment, spans, and
  per-widget min/max limits; automatic reflow on resize.
- **Text** — TrueType shaping with kerning and subpixel positioning,
  synthetic bold/italic, an LRU glyph cache, labels that wrap,
  ellipsize, and align — and full-Unicode XIM input on X11.
- **Theming** — runtime-switchable `.fdk` theme files with a strict,
  fail-closed grammar; one shipped face (the default); more faces
  are user-installed `.FDKThemes` drop-ins; live re-theming of running
  applications; `fdk-theme` / `fdk-set` command-line tools.
- **FDK-drawn window decorations** — a themed title band with full
  window management (drag, maximize, fullscreen, resize), handing off
  to the WM/compositor where protocols exist.
- **Platform services** — clipboard (X11 CLIPBOARD and PRIMARY,
  Wayland data-device and primary-selection, including images), drag
  and drop (text, files, and folders — with drag icons and
  widget-level drop targets), keyboard shortcuts, menu accelerators
  and mnemonics, tooltips, undo/redo, and an atomically saved
  application preferences store.
- **Desktop integration (1.1.0)** — the services a desktop
  application actually needs, all on FDK's own zero-dependency
  implementations: a native D-Bus client (raw C from the wire
  spec, no libdbus) riding a StatusNotifier **tray host** and a
  full **notification server** (both as fdk-shell examples with
  their own rigs); **dark mode** following the desktop's setting
  on every channel that exists (color-scheme portal, XSETTINGS,
  the settings daemon, a fallback file); **portal dialogs** — the
  xdg-desktop-portal FileChooser where the portal exists, the
  built-in file dialog where it doesn't; **idle inhibition** (the
  Wayland protocol or org.freedesktop.ScreenSaver, whichever the
  desktop speaks); **window activation + startup notification**
  (xdg-activation-v1, the X11 EWMH lane, launch vouchers); the
  **foreign toplevel list** (the taskbar substrate —
  wlr-foreign-toplevel-management and EWMH _NET_CLIENT_LIST);
  **session save/restore** (X11's ICCCM WM_SAVE_YOURSELF /
  WM_COMMAND lane, and Wayland's xdg-session-management-v1 where
  a compositor offers it); multi-monitor placement (X11 RandR,
  Wayland output events) with HiDPI on both; the full **cursor
  shape set** themed from the desktop's own cursor theme; input
  regions for click-through overlays; **IME** (text-input-v3 and
  XIM preedit — CJK input works), **touch** with tap/scroll and
  pinch/swipe gestures, and **drawing-tablet** input; AT-SPI2
  accessibility (level 1 + the Text interface) for real screen
  readers; color **emoji** (COLRv0/v1, CBDT, sbix); and animated
  GIF. Where a desktop lacks a mechanism FDK degrades honestly
  and says so — never an error loop (`docs/TESTING-ON-DESKTOP.md`
  is the per-feature sign-off checklist).
- **Internationalization** — explicit-locale formatting (number
  groupings including Indian and Arabic-Indic, currency, dates),
  CLDR plural rules for 33 languages, and strict `.fmo` translation
  catalogs (`docs/i18n.md`).
- **Accessibility** — the accessibility tree *is* the widget tree:
  describe / observe / drive any widget headlessly, plus the embedded
  narrator — an in-process screen reader core with no D-Bus and no
  bridge process.
- **Animation** — an easing-curve library and a one-ticker animator
  with exact handle-lifetime guarantees; smooth wheel/keyboard
  scrolling in every ScrollView.

## Requirements

- GCC with C17 support (developed against GCC 13+; see `docs/build.md`)
- X11 development headers (always required — X11 is the baseline
  backend)
- Optional: Wayland development headers (`libwayland-dev`,
  `wayland-protocols`, `libxkbcommon-dev`) — auto-detected at build
  time; if absent, FDK builds as X11-only
- `Xvfb`, optionally, to run `make test-x11` without an existing
  desktop session

## Installation

There are no published packages — the project is in development.
Build from source (below); the packaging recipes in `docs/packaging.md`
exist and are exercised by CI on every push, as validation checks.

## Building

```sh
make            # debug build (ASan+UBSan on by default): libs + tools
make test       # platform-independent test suite (no display needed)
make test-x11   # X11 integration suite (auto-starts a throwaway Xvfb)
make examples   # build the example programs
```

Optional build knobs:

```sh
make FDK_ENABLE_WAYLAND=1    # require the Wayland backend (error if
                             # wayland-dev / xkbcommon-dev are absent)
make FDK_DISABLE_WAYLAND=1   # force an X11-only build
```

Release builds, `make install`, and the full command reference are in
`docs/build.md`.

## Installing from source and linking

```sh
make install                      # PREFIX defaults to /usr/local
cc myapp.c $(pkg-config --cflags --libs fdk)
```

The same install puts the `fdk-theme`, `fdk-set`, and `fdk-prefs`
command-line tools into `$(PREFIX)/bin` and the one shipped theme —
`faded-dream`, the packaged copy of the built-in default — into
`$(PREFIX)/share/fdk/.FDKThemes`. Additional faces are not package
content: drop any `.fdk` file into `~/.FDKThemes` (or
`~/.local/share/fdk/.FDKThemes`) and `fdk-theme set <name>` picks
it up immediately.

## A first window

```c
#include "fdk/fdk.h"
#include "fdk/fdk_event.h"
#include "fdk/fdk_window.h"

static void on_event(fdk_window *window, const fdk_event_data *event, void *user_data) {
    fdk_context *ctx = user_data;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(window);
        fdk_quit(ctx);
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    fdk_init(&ctx, NULL); /* connects to a real X11 or Wayland display */

    fdk_window *window = NULL;
    fdk_window_options opts = { .title = "Hello", .width = 640, .height = 480 };
    fdk_window_create(ctx, &opts, &window);
    fdk_window_set_event_callback(window, on_event, ctx);
    fdk_window_show(window);

    fdk_run(ctx); /* real poll()-based event loop */
    fdk_shutdown(ctx);
    return 0;
}
```

`examples/01_hello_world.c` (via `make examples`) is a fuller version
of this — it needs a reachable X11 or Wayland display to run.

## Examples

Twelve example programs build with `make examples` (run from the
repository root):

| Example | Shows |
|---|---|
| `01_hello_world` | a real window, event logging, clean shutdown |
| `02_rendering` | the software renderer end to end — primitives, image compositing, transforms, AA |
| `03_text` | glyph rendering gallery and the multi-line TextView editor |
| `04_widgets` | the widget catalog with live interactions |
| `05_theme` | live theme cycling, including external `.fdk` files |
| `06_decorations` | FDK-drawn decorations and window management |
| `07_advanced` | menus, popups, modal dialogs, notebook, canvas |
| `08_narrator` | the embedded narrator speaking focus and toggles |
| `09_capabilities` | platform/input capabilities, render-path report, drag-and-drop receiver |
| `10_file_manager` | FDK Files — a real small file manager |
| `11_animation` | the animation engine: an easing-curve race |
| `12_settings` | a complete settings application from the catalog |

## Themes

FDK restyles at runtime from data: every live window repaints from one
call, decoration included.

```c
fdk_theme *t = fdk_theme_load("my-theme.fdk", NULL); /* strict .fdk */
fdk_theme_set_default(t);
```

- The `.fdk` grammar is strict and fail-closed — unknown keys,
  duplicates, and malformed values are line-numbered errors, never a
  half-themed UI; missing tokens inherit the built-in defaults
  (`docs/fdk-theme-format.md`).
- One face ships with the toolkit: `faded-dream` — the packaged
  copy of the calm gray-with-violet built-in default (the fallback
  theme, byte-equal by contract). The next faces are `.FDKThemes`
  drop-ins the user installs, not package content.
- `fdk-theme set <name>` changes the global theme — applied live by
  every running FDK application that didn't install its own;
  `fdk-set` does the same per application. Custom themes load from
  `$FDK_THEME_DIR`, `~/.FDKThemes`, and the XDG data hierarchy
  (`docs/cli.md`).

## What it looks like

Real captured frames from the test rigs — not mockups
(`docs/screenshots.md` has the full gallery):

![04_widgets on Wayland](docs/screenshots/suite_wayland_04_widgets.png)

![02_rendering on Wayland](docs/screenshots/suite_wayland_02_rendering.png)

![Three themes, same code](docs/screenshots/theme_three_themes_1380x330.png)

## Documentation

| Doc | Covers |
|---|---|
| `docs/architecture.md` | Layering, module boundaries, public/internal header split |
| `docs/roadmap.md` | Phase-by-phase plan and current status |
| `PROGRESS.md` | The fast, authoritative implemented-features ledger |
| `docs/build.md` | Build system reference, release builds, install |
| `docs/testing.md` | The two-tier test suite and environment quirks |
| `docs/rendering.md` | The renderer, the GPU present lane, and its env knobs |
| `docs/text.md` | The text stack and font discovery |
| `docs/i18n.md` | The internationalization engine |
| `docs/platform-input.md` | Keyboard/IME/XIM input across backends |
| `docs/memory.md` | Ownership model, allocation policy |
| `docs/threading.md` | UI-thread affinity, worker-thread rules |
| `docs/performance.md` | The performance baseline harness |
| `docs/abi-policy.md` | The ABI freeze, struct classification, subclassing decision |
| `docs/versioning.md` | The versioning policy, the version-history record, and the active SONAME scheme |
| `docs/packaging.md` | The per-distribution packaging recipes, and how CI validates them |
| `docs/dependencies.md` | Every current and anticipated dependency, justified |
| `docs/licensing-policy.md` | What licenses are/aren't allowed in, and the audit procedure |
| `docs/fdk-theme-format.md` | The `.fdk` theme file grammar |
| `docs/fdk-catalog-format.md` | The `.fmo` translation-catalog grammar |
| `docs/fdk-prefs-format.md` | The `.prefs` application-settings grammar |
| `docs/cli.md` | The `fdk-theme` / `fdk-set` / `fdk-prefs` tools |
| `docs/security.md` | The strict-parser security rules |
| `docs/screenshots.md` | The captured proof gallery |

## Project principles

- **No GTK, no Qt, no wrapping either.** FDK implements its own
  widget system, rendering, layout, event handling, and decorations;
  the X11 and Wayland backends talk directly to Xlib and
  libwayland-client.
- **Minimal dependencies, always justified** (`docs/dependencies.md`).
- **No copyleft, anywhere in the dependency graph**
  (`docs/licensing-policy.md`).
- **Correct over quick; architecture over feature-count**
  (`docs/roadmap.md`).
- **No fake completion** — the roadmap lists what is *not* covered as
  carefully as what is.

## License

FDK is **proprietary software**. The toolkit — its source code,
framework implementation, and shipped assets — remains the exclusive
property of its rights holder and may not be copied, modified,
redistributed, sublicensed, published, or commercially exploited
without express written authorization. See [`LICENSE`](LICENSE)
(the FDK Proprietary License, version 1.0; governed by the laws of
Romania, with exclusive venue in Bucharest).

**Applications built with FDK are governed separately and remain
yours.** The license governs FDK itself, not the software you build
with it: your application's source code, assets, branding, and
licensing stay your independent property, and you may distribute and
sell your applications under terms of your choosing. FDK's
restrictions never attach to independent application code.

**The FDK runtime is installed separately.** Applications must not
bundle, embed, or redistribute the FDK runtime: end users obtain and
install it themselves — for example, `make install` from this
repository, or another channel the Licensor authorizes — to run FDK
applications. Requiring the runtime does not diminish your
application's independence or its distributability.

Third-party components bundled with FDK remain under their own
permissive licenses — see `THIRD-PARTY-NOTICES.md`.
