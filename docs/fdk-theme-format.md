# The `.fdk` Theme Format

Specification for FDK's theme files. Version 1.

This document is the normative grammar reference for `fdk_theme_load()`
and `fdk_theme_parse()` (Phase 7 — Theme Engine). The parser is
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

All optional; each inherits from the built-in default theme when absent.

## Metrics

| Key                    | Range  | Default | Paints / lays out               |
|------------------------|--------|---------|---------------------------------|
| `button_corner_radius` | 0–32   | 8       | Button fill + focus-ring corner radius |
| `separator_thickness`  | 1–8    | 1       | Separator band thickness        |
| `title_bar_height`     | 12–64  | 28      | FDK-drawn title band height     |

All optional; each inherits from the built-in default theme when absent.
Most metrics are paint-time values only — they do not change any
widget's natural size (a separator's size request remains the
application's). `title_bar_height` is the exception, deliberately:
it is a LAYOUT metric — switching the default theme re-arranges
every decorated window (the band grows/shrinks and the content
widget reflows below it), then repaints. It only affects windows
using FDK's own decorations (`fdk_window_set_decorated`).

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
