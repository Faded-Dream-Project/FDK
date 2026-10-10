# The `.fdk` Theme Format

Specification for FDK's theme files. Version 1.

This document is the normative grammar reference for `fdk_theme_load()`
and `fdk_theme_parse()` (Phase 7 — Theme Engine; the 1.4.16
additions — the titlebar family, `button_shape`, the `.FDKThemes`
folders — are marked inline). The parser is
deliberately strict: anything not described here is a parse error, not a
warning. The parser's security posture (why it rejects first and asks
questions never) is documented in `docs/security.md`.

## Design goals

- **Human-editable.** A theme is a small text file a user can write in
  any editor. No XML, no JSON, no nesting deeper than one section level.
- **Strict.** Unknown sections, unknown keys, duplicate keys, and
  malformed values are hard errors. Typos must not silently produce a
  half-themed UI.
- **Partial by design.** Every color and metric is optional; a theme
  that specifies three colors and nothing else inherits the built-in
  defaults for the rest. Overriding is opt-in per token.
- **Bounded.** The parser accepts at most 1 MiB of input, lines of at
  most 1024 bytes, and strings of at most 128 bytes. There are no
  includes, imports, variables, or expressions — a theme file can never
  reach outside itself.

## File syntax

A theme file is a sequence of lines. Each line is exactly one of:

- a **blank line** (zero or more spaces/tabs),
- a **comment**: `#` followed by anything to end of line,
- a **section header**: `[name]` (whitespace inside the brackets is
  ignored, e.g. `[ colors ]`),
- an **entry**: `key = value`.

Leading and trailing whitespace on a line is ignored. Whitespace around
`key`, `=`, and `value` is ignored. Line endings may be LF, CRLF, or CR
(a bare CR is treated as a line terminator). A UTF-8 byte-order mark at
the very start of the file is skipped. Anything else — including NUL
bytes and non-ASCII bytes outside a quoted string — is a parse error at
that line.

There is no line-continuation syntax and no quoting except for string
values. A `#` inside a quoted string does not start a comment. A
**zero-byte file is rejected** — it is almost certainly a wrong path
or a failed download, not a theme. A file containing only comments is
valid and equals the built-in defaults (the grammar ran; it overrode
nothing).

### Sections

| Section      | Contents                                    |
|--------------|---------------------------------------------|
| `[theme]`    | File metadata: `version`, `name`, `author`  |
| `[colors]`   | Color tokens (see table below)              |
| `[metrics]`  | Integer metrics (see table below)           |

Rules:

- Entries before any section header belong to an **implicit
  `[theme]` section** — the example below starts with a bare
  `version = 1` line and needs no header. Only `[theme]` keys
  (`version`, `name`, `author`) may appear there; anything else
  before a header is a parse error.
- Each section may appear at most once. A second `[colors]` block is a
  parse error ("duplicate section").
- Sections may appear in any order, but `[theme]` is conventionally
  first.
- `version` may only appear in `[theme]`. It must be the integer `1`.
  Any other value is rejected with `FDK_ERR_THEME_VERSION` (a theme
  written for a later format version than this build understands).
  Omitting `version` means version 1.

### Values

**Colors** are `#RRGGBB` or `#RRGGBBAA` — exactly 6 or 8 hexadecimal
digits after the `#`, case-insensitive. No CSS color names, no `rgb()`
notation, no 3-digit shorthand. The alpha channel defaults to `FF`
(opaque) in the 6-digit form. Components are converted to FDK's
`fdk_color` floats by division by 255.0 — the format stores 8-bit
channel values, and the parser performs no gamma correction, blending,
or rounding beyond that division.

**Metrics** are decimal integers with an optional leading `-` (all
current metrics have minimum values that make negatives invalid, so a
leading `-` always fails range validation). Leading zeros are rejected
(`08` is an error, not `8` — strictness over convenience). Each metric
has its own valid range (table below); out-of-range values are parse
errors, not clamping.

**Strings** (`name`, `author`) are double-quoted. Only two escapes
exist: `\"` (literal quote) and `\\` (literal backslash). A backslash
before any other character is a parse error. Strings hold at most 128
bytes of content, must be closed on the same line, and may not contain
control characters (bytes 0x00–0x1F). Non-ASCII UTF-8 bytes are
permitted and stored verbatim.

## Color tokens

| Key                        | Paints                                              |
|----------------------------|-----------------------------------------------------|
| `window_background`        | Recommended window/root background (apps opt in; FDK does not force it onto existing windows) |
| `text`                     | Primary text (labels, button captions)              |
| `text_disabled`            | Text on disabled widgets                            |
| `control_background`       | Button/checkbox/toggle resting fill                 |
| `control_background_hover` | Control fill under the pointer                      |
| `control_background_pressed` | Control fill while pressed                        |
| `control_background_disabled` | Control fill when disabled                       |
| `control_border`           | Separators, frame rules, outlines                   |
| `accent`                   | Checked state, progress fill, focus ring            |
| `track`                    | Progress bar / toggle track                         |
| `tooltip_background`       | Tooltip popup fill (1.3.2)                          |
| `tooltip_text`             | Tooltip label                                       |
| `tooltip_border`           | Tooltip popup edge                                  |
| `selection_background`     | Selected text/rows/entries (1.3.2; default is a translucent accent — light themes can set it opaque) |
| `selection_text`           | Text ON a selection                                 |
| `focus_ring`               | Keyboard-focus indicator                            |
| `success`                  | Positive/validated semantic state                   |
| `warning`                  | Caution semantic state                              |
| `danger`                   | Destructive/error semantic state                    |
| `sidebar_background`       | Places/panel sidebar surfaces (1.4.0; the file dialog's Places column) |
| `menu_background`          | Menu/combo POPUP surface (1.4.0)                    |
| `accent_hover`             | Filled-button accent under the pointer (1.4.0)      |
| `accent_pressed`           | Filled-button accent while pressed (1.4.0)          |
| `accent_text`              | Text ON an accent fill (1.4.0: suggested buttons)   |
| `link`                     | Hyperlink text (1.4.0; reserved for link-styled controls) |
| `entry_background`         | Text-field fill (1.4.0; the flat modern field)      |
| `entry_border`             | Text-field edge (1.4.0)                             |
| `row_hover`                | Dense-row hover fill (1.4.0; lists, menus, breadcrumbs) |
| `titlebar_background`      | FDK-drawn decoration band fill (1.4.16; FALLBACK: `control_background`) |
| `titlebar_text`            | Band title text + window-button glyphs (1.4.16; FALLBACK: `text`) |
| `titlebar_border`          | Band bottom rule (1.4.16; FALLBACK: `control_border`) |
| `titlebar_button_hover`    | Band button under the pointer (1.4.16; FALLBACK: `control_background_hover`) |
| `titlebar_button_pressed`  | Band button while pressed (1.4.16; FALLBACK: `control_background_pressed`) |

All optional; each inherits from the built-in default theme when
absent. The five `titlebar_*` keys are the FALLBACK family: a theme
that omits one reads the base token named after it — including a
base the theme itself overrode — so every pre-1.4.16 theme paints
exactly the band it always painted, and a theme that wants a
distinct chrome (Mono Chromatic's black band over white content,
Pink Rave's hot-pink one) says so in five keys. A theme may set
some of the family and not the rest; each key falls back
independently.

## Metrics

| Key                    | Range  | Default | Paints / lays out               |
|------------------------|--------|---------|---------------------------------|
| `button_corner_radius` | 0–32   | 8       | Button fill + focus-ring corner radius |
| `separator_thickness`  | 1–8    | 1       | Separator band thickness        |
| `tooltip_corner_radius` | 0–16  | 6       | Tooltip box corner radius (1.3.2) |
| `title_bar_height`     | 12–64  | 28      | FDK-drawn title band height     |
| `menu_item_height`     | 16–48  | 26      | Menu bar/row minimum height     |
| `scrollbar_width`      | 6–24   | 12      | ScrollView bar thickness        |
| `entry_corner_radius`  | 0–16   | 6       | Entry-field corners (1.4.0; also the combo field and the breadcrumb bar) |
| `menu_corner_radius`   | 0–16   | 8       | Menu/combo popup corners (1.4.0) |
| `list_row_height`      | 16–48  | 30      | List row height FLOOR (1.4.0)   |
| `focus_ring_width`     | 1–4    | 2       | Focus ring stroke (1.4.0; buttons and entries) |
| `scrollbar_overlay_width` | 4–12 | 6       | Overlay bar thickness (1.4.4; the transient thumb) |
| `iconview_cell_width`   | 64–192 | 96     | IconView cell width (1.4.6; LAYOUT metric) |
| `iconview_cell_height`  | 64–224 | 84     | IconView cell height (1.4.6; LAYOUT metric) |
| `textview_pad`          | 0–32   | 8      | TextView inner inset, both axes (1.4.8; LAYOUT metric) |
| `button_shape`          | 0–2    | 0      | 0 rounded, 1 CIRCLE, 2 square (1.4.16) |

All optional; each inherits from the built-in default theme when absent.
`button_shape` (1.4.16) is the SHAPE story: `0` defers to
`button_corner_radius` (the historical look), `1` rounds to half the
button's short side — a true circle on a square button, a pill on a
wide one; Mono Chromatic ships this — and `2` forces square corners
regardless of the radius. It applies wherever buttons are painted:
the catalog Button, the MenuButton, and the decoration band's window
buttons, so one key shapes the whole chrome. Paint-time only — no
widget's natural size changes.
Most metrics are paint-time values only — they do not change any
widget's natural size (a separator's size request remains the
application's). The exceptions are LAYOUT metrics, deliberately:
`title_bar_height` re-arranges every decorated window (the band
grows/shrinks and the content widget reflows below it), and
`list_row_height` re-places every List row — switching the default
theme re-arranges both, then repaints. `title_bar_height` only
affects windows using FDK's own decorations
(`fdk_window_set_decorated`).

## Error semantics

Blank lines and comments are invisible to the grammar, and
**trailing comments are not supported** - a comment occupies a whole
line. Every diagnostic carries the 1-based line number. On any error the
parse produces no theme object at all — there is no "best effort"
partial result, and no partial state escapes the parser. The caller
receives `NULL` and (if requested via the `out_error` parameter) one of:

| Result                    | Meaning                                        |
|---------------------------|------------------------------------------------|
| `FDK_ERR_THEME_PARSE`     | Grammar, unknown key/section, duplicate, range, empty (zero-byte) file |
| `FDK_ERR_THEME_VERSION`   | `version` present but not `1`                  |
| `FDK_ERR_THEME_IO`        | `fdk_theme_load`: open/read/size failure       |
| `FDK_ERR_OUT_OF_MEMORY`   | Allocation failure                             |
| `FDK_ERR_INVALID_ARGUMENT`| `NULL` input, absurd length                    |

## Complete example

```
# daylight.fdk — a light theme example (see examples/data/)
version = 1
name    = "Daylight"
author  = "FDK examples"

[colors]
window_background        = #F4F5F7
text                     = #1B1F27
text_disabled            = #9AA0AB
control_background       = #FFFFFF
control_background_hover = #E8EBF0
control_background_pressed = #D6DAE2
control_background_disabled = #ECECEC
control_border           = #C4C9D2
accent                   = #2563EB
track                    = #E1E4EA

[metrics]
button_corner_radius = 6
separator_thickness  = 1
```

## The v1 look as a theme file (the legacy recipe)

The built-in default is the 1.4.16 "Faded Dream" palette — the calm
gray-with-violet retune (window #232329, controls #33333C, one violet
accent family #8F79D9 kept to the small parts); before it, the 1.4.0
"Modern" palette carried a blue undertone in every gray, and before
that the Phase 6 "v1" look. Every pixel any of them painted is
reproducible exactly by loading a theme
(`fdk_theme_load` + `fdk_theme_set_default`) — v1 first:

```
# v1.fdk — the Phase 6 v1 palette, exact (the pre-1.4.0 built-in)
version = 1
name    = "FDK Dark (v1)"

[colors]
window_background          = #121721
text                       = #EBEDF5
text_disabled              = #737885
control_background         = #292E42
control_background_hover   = #38405C
control_background_pressed = #475275
control_background_disabled= #1F212E
control_border             = #4D5470
accent                     = #59A6F2
track                      = #1A1F2B
tooltip_background         = #F2F5FC
tooltip_text               = #1C2130
tooltip_border             = #B3BAD1
selection_background       = #59A6F273
selection_text             = #F2F5FC
focus_ring                 = #59A6F2E6
success                    = #5CBA6B
warning                    = #E6AD40
danger                     = #E8595C
sidebar_background         = #1A1F29
menu_background            = #262B3B
accent_hover               = #80B0FF
accent_pressed             = #5785D9
accent_text                = #F7FAFF
link                       = #8CBAFF
entry_background           = #12151D
entry_border               = #2B3040
row_hover                  = #24293899

[metrics]
button_corner_radius = 8
separator_thickness  = 1
title_bar_height     = 28
scrollbar_width      = 12
scrollbar_overlay_width = 6
iconview_cell_width   = 96
iconview_cell_height  = 84
textview_pad          = 8
menu_item_height     = 26
tooltip_corner_radius = 6
entry_corner_radius  = 6
menu_corner_radius   = 8
list_row_height      = 26
focus_ring_width     = 1
```

(Values are the 8-bit roundings of the v1 floats; the 1.4.0-only keys
carry v1-calibrated analogues so the recipe stays complete — a v1
strict reconstruction of the pre-1.4.0 pixels ignores them, since no
pre-1.4.0 widget consumed them.)

## The 1.4.0 Modern look as a theme file (the 1.4.16 migration recipe)

Faded Dream replaced Modern's blue-grained grays with a true gray
ramp and its sky-blue accent with violet. The Modern look survives
the same way v1's does — as a recipe:

```
# modern.fdk — the 1.4.0 "Modern" palette, exact (the pre-1.4.16
# built-in; every value is the renderer's own rounding of the
# Modern floats, extracted from the 1.4.15 tree byte-for-byte)
version = 1
name    = "FDK Modern"

[colors]
window_background          = #171A24
text                       = #EDF0F7
text_disabled              = #707585
control_background         = #212633
control_background_hover   = #2B3345
control_background_pressed = #364054
control_background_disabled= #1C1F29
control_border             = #333B4D
accent                     = #6B9EFF
track                      = #1C1F29
tooltip_background         = #F2F5FC
tooltip_text               = #1C2130
tooltip_border             = #B3BAD1
selection_background       = #6B9EFF61
selection_text             = #F2F5FC
focus_ring                 = #6B9EFFE6
success                    = #61BD73
warning                    = #E8B045
danger                     = #E85C61
sidebar_background         = #1A1F29
menu_background            = #262B3B
accent_hover               = #80B0FF
accent_pressed             = #5785E0
accent_text                = #F7FAFF
link                       = #8CBAFF
entry_background           = #12141C
entry_border               = #2B3040
row_hover                  = #24293899

[metrics]
button_corner_radius = 8
title_bar_height     = 28
list_row_height      = 30
focus_ring_width     = 2
```

## What is deliberately NOT in version 1

Fonts, spacing scale, padding, per-widget-class sections, state
transitions/animations, images/icons, DPI variants, dark/light
auto-switching, and includes/imports. Every one of these has a sane
future home in this format (more sections, more keys), and none of them
belong in the first cut: the Phase 7 engine exists to replace the
hardcoded v1 palette, not to anticipate every theming feature ever
shipped by a major toolkit. Adding keys later is backward-compatible
(unknown keys stay errors for old parsers by design — a theme that
needs a key this FDK does not know should fail loudly, not render
wrong); adding *sections* later bumps `version`.

## Discovery and installation (1.4.13; folders grew in 1.4.16)

A theme file does not need to be loaded by path. The discovery layer
(`fdk_theme_find`, the engine behind the `fdk-theme` CLI) resolves a
NAME against the search path and the naming rules below; the
normative reference for that half is `docs/cli.md` and
`include/fdk/fdk_theme.h`.

- A theme is installed by placing `<stem>.fdk` in a search directory
  (first match wins):
  `$FDK_THEME_DIR`, `$HOME/.FDKThemes` (1.4.16: the custom per-user
  folder — drop a file here and it beats every system location),
  `$XDG_DATA_HOME/fdk/.FDKThemes` (default
  `~/.local/share/fdk/.FDKThemes` — the canonical folder `make
  install` fills), `$XDG_DATA_HOME/fdk/themes` (the 1.4.13
  location, still scanned), then each `$XDG_DATA_DIRS` entry's
  `fdk/.FDKThemes` followed by its `fdk/themes` (default
  `/usr/local/share:/usr/share`). FDK's own five faces ship in the
  source tree's `.FDKThemes/` and install into
  `<prefix>/share/fdk/.FDKThemes/`.
- The stem is the canonical handle: `matrix` resolves to
  `matrix.fdk`, exact and case-sensitive, first directory on the
  path wins. Stems should be 1..64 characters of `[A-Za-z0-9_-]`
  (the same character class as a prefs key half).
- A theme whose file name differs from its internal `name` is also
  reachable BY that internal name — the discovery pass compares the
  quoted `name` exactly.
- The stem grammar is a security boundary as well as a convention:
  a lookup name is interpolated into a path only when it matches the
  grammar, so `../`-shaped or `/`-containing input can never walk
  outside the search directories.

Nothing in this section changes the file format itself — it is pure
placement policy over version 1.
