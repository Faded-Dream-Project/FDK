# The `.prefs` Preferences Format

Specification for FDK's application preference files. Version 1.

This document is the normative grammar reference for
`fdk_prefs_open()` / `fdk_prefs_save()` (1.3.7 — application
preferences). It is FDK's third strict line-based dialect, after
the `.fdk` theme format and the `.fmo` message catalog; the family
rules are the same, with ONE deliberate difference in failure
posture (below). The parser's general security posture is
documented in `docs/security.md`.

## Design goals

- **Human-editable.** A preferences file is a small text file a
  user can read and fix in any editor: sections, `key = value`
  lines, comments. No XML, no JSON, no nesting beyond one section
  level (the whole FDK family stops at one).
- **Strict.** Unknown syntax, malformed keys, duplicate keys,
  control characters, and invalid UTF-8 are load errors carrying a
  line number. Typos must not silently produce half-remembered
  settings.
- **Resilient at the API, not at the grammar.** The one family
  difference: a rejected preferences file yields an EMPTY store
  (every getter serves its default) instead of a failed
  `fdk_prefs_open()` — a settings file must never take a working
  application down. The grammar is exactly as strict as the theme
  parser's; what changes is what the caller sees when it fires.
  (A half-themed UI is worse than no theme, so themes fail loud;
  a dead prefs file must not kill the app, so prefs fail soft.
  Both rules are right for their own kind of file.)
- **Bounded.** At most 1 MiB of input, lines of at most 1024
  bytes, values of at most 256 bytes, at most 8192 keys, section
  and key names of at most 64 bytes each. No includes, no imports,
  no variables, no expressions — a prefs file can never reach
  outside itself.

## File syntax

A preferences file is a sequence of lines. Each line is exactly one
of:

- a **blank line** (zero or more spaces/tabs),
- a **comment**: `#` or `//` followed by anything to end of line,
- a **section header**: `[name]` with nothing else on the line,
- a **key line**: `name = value`.

Leading whitespace on a line is ignored. Line endings may be LF or
CRLF (a CR is stripped only immediately before an LF; a CR anywhere
else is a foreign character and is rejected by the value rules).
Whitespace on both sides of the `=` is ignored — a name is
`[A-Za-z0-9_-]`, so a run of spaces or tabs before the `=` can only
be separator (`width = 880`, `width= 880`, and `width  =880` are the
same line; the writer's canonical form is `name = value`). Trailing
whitespace around the value is trimmed. The file SHOULD begin with
the comment header `fdk_prefs_save()` writes (see "Writers" below);
a file without it parses identically.

A `name` (section or key) is 1..64 bytes of `[A-Za-z0-9_-]`. Names
are case-sensitive (`Window.width` and `window.Width` are different
keys — deterministic, and what every sibling format does).

A **value** is any well-formed UTF-8 sequence of 0..256 bytes
containing no control characters (bytes below 0x20). An empty value
is legal: `remembered =` means "the key exists and is empty",
distinct from the key being absent (the string getter returns ""
vs. the caller's default).

Every key line must follow a section header; a key before any
section is a load error. Sections may repeat and interleave (rows
keep the file's own order; `fdk_prefs_save()` reproduces it). A
duplicate `section.name` key anywhere in the file is a load error —
the theme family's hard rule; a hand-edited file with a doubled key
is a file whose state is ambiguous.

## Typed values

The store is strings; typed getters parse:

- **int**: optional `-`/`+`, then decimal digits, the whole value.
  Parsed with `strtol`; a range failure reads as the default.
- **bool**: exactly `true`, `false`, `1`, or `0`.
- **double**: whatever `strtod` accepts, consuming the whole value.

A present-but-unparsable value reads as the DEFAULT (never an
error — a hand-edited `width = narrows` must not crash the app at
startup; it logs one DEBUG line on first read for diagnosis). The
typed setters format exactly as the getters parse — a
set-then-save-then-reopen-then-get round-trips bit-for-bit
(`%.17g` for doubles, `true`/`false` for bools, decimal for ints).

## Path resolution

One file per application, resolved in order:

1. `$FDK_PREFS_FILE` — an absolute-path override (tests, sandboxed
   rigs). A relative value is ignored.
2. `$XDG_CONFIG_HOME/<app_id>.prefs` (only when `XDG_CONFIG_HOME`
   is absolute).
3. `$HOME/.config/<app_id>.prefs` (only when `HOME` is absolute).
4. Otherwise the store is memory-only: reads serve defaults, and
   `fdk_prefs_save()` returns `FDK_ERR_UNSUPPORTED` rather than
   inventing a location.

## Writers

`fdk_prefs_save()` writes the whole file: the generated header
comment (format name and app id), then sections in first-appearance
order with their keys in insertion order. Comments in a loaded file
are NOT preserved across a save (values survive; hand-added
comments do not — documented and deliberate: preserving comments
requires a document editor, not a settings store). The write is
atomic: a temp file in the same directory, `fsync`, then `rename`
over the target. A crash mid-save can leave `<path>.tmpXXXXXX`
residue but never a truncated preferences file.

A save of an UNCHANGED store touches no disk (the store tracks the
exact file state; setting a key back to the file's own value leaves
the store clean) — repeated save-on-timer calls cost nothing.
