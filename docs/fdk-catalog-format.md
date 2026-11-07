# The `.fmo` Message Catalog Format

Specification for FDK's translation catalogs. Version 1.

This document is the normative grammar reference for
`fdk_catalog_parse()` and `fdk_catalog_load()` (Phase 10 —
Internationalization). The parser is deliberately strict, like the
`.fdk` theme parser before it: anything not described here is a parse
error carrying a line number, not a warning. The security posture
(why it rejects first) is documented in `docs/security.md`; the wider
i18n design is `docs/i18n.md`.

## Design goals

- **Human-editable.** A catalog is a small text file a translator
  writes in any editor — the gettext `.po` mental model (which thirty
  years of tooling and translators know), minus everything that made
  `.po` processing ambiguous.
- **Strict.** Unknown keywords, duplicate ids, malformed strings, and
  invalid UTF-8 are hard errors. A misspelled `msgstr` must fail at
  load, not silently fall back to English at runtime.
- **Bounded.** At most 1 MiB of input, lines of at most 1024 bytes,
  strings of at most 1024 bytes, at most 8192 entries. No includes,
  no variables, no expressions — a catalog can never reach outside
  itself.
- **Category-accurate plurals.** Plural forms are indexed by CLDR
  category name (`msgstr[one]`, `msgstr[few]`, ...) rather than
  gettext's positional `[0] [1] [2]`, so a translator writes the
  categories their language actually has and the runtime selects
  them by rule, not by index convention.

## File syntax

A catalog file is a sequence of lines. Each line is exactly one of:

- a **blank line** (zero or more spaces/tabs),
- a **comment**: `#` or `//` followed by anything to end of line,
- a **keyword line** for one of the six keywords below.

Leading whitespace on a line is ignored. Line endings may be LF, CRLF,
or CR. The file must be valid UTF-8 throughout. NUL bytes, raw control
characters, and any byte sequence that is not well-formed UTF-8 are
parse errors.

Comments and blank lines may appear **between** entries but never
**inside** one (an entry is the sequence of its keyword lines) — a
comment in the middle of an entry is an error, because it almost
always means a line was accidentally deleted.

## Entries

```
# singular entry
msgid "key"
msgstr "translation"

# contextual entry (same key, different context)
msgctxt "menu"
msgid "open"
msgstr "Öffnen"

# plural entry
msgid "key"
msgid_plural "%d keys"
msgstr[one] "%d key"
msgstr[other] "%d keys"
```

An entry is:

1. optionally `msgctxt "context"` — the message context (gettext's
   pgettext), distinguishing identical keys used in different UI
   situations;
2. exactly one `msgid "key"` — the source string (English by
   convention). Must be non-empty;
3. for plural entries, exactly one `msgid_plural "source-plural"`;
4. `msgstr "translation"` for singular entries, or one or more
   `msgstr[category] "translation"` lines for plural entries, where
   *category* is one of the six CLDR plural categories: `zero`, `one`,
   `two`, `few`, `many`, `other`.

Entries may be adjacent (no blank line required) — a blank line or
comment is only a visual separator, and a following `msgid` commits
the entry in progress. Each category may appear at most once per
entry. `(msgctxt, msgid)` pairs must be unique across the file.

A plural entry does NOT need `other` (the fallback chain is
category → other → first stored form → the English source), but
omitting it is usually a translation bug and will show up as English
text for the counts you forgot.

## Strings

Strings are double-quoted. Inside a string:

- `\n` is a newline, `\t` a tab, `\"` a quote, `\\` a backslash —
  and nothing else: `\q` is an error, not a literal `q`;
- every other byte must be valid UTF-8;
- strings are single-line (a raw newline ends the line, and the
  unterminated string is an error);
- the empty string is a legal translation; it is not a legal msgid.

## What the parser guarantees

- **No partial results.** A failed parse leaves the caller's output
  pointer untouched; nothing half-parsed is ever handed out.
- **Error discipline.** Every rejection logs `catalog:<line>: <why>`
  and returns `FDK_ERR_CATALOG_PARSE` (or `FDK_ERR_IO` /
  `FDK_ERR_OUT_OF_MEMORY` for the file-loading edge cases; a
  zero-byte file is rejected like an empty theme — it is almost
  certainly a wrong path).
- **Sorted, searchable.** After a successful parse the entries are
  sorted by `(context, msgid)` and looked up by binary search —
  O(log n), no hashing, deterministic memory.
- **Immutable.** A catalog has no mutation API; it is safe for
  concurrent readers the moment it is parsed.

## What is deliberately NOT here

- No `msgid ""` header entry (gettext's metadata convention) —
  metadata belongs to the file's deployment, not its grammar.
- No line-continuation or multi-line strings — one string, one line.
- No placeholder interpretation: `%d` in a translation is literal
  text the APPLICATION formats against (`snprintf` discipline, see
  `fdk_translate_plural` in `fdk_i18n.h`). The toolkit does not
  execute the translator's strings.
- No fuzzy/obsolete markers, no plural-formula headers (the plural
  RULES live in the toolkit's CLDR table, keyed by locale, not in
  every catalog file repeating them).
