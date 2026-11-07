# Security Policy

FDK's stance on untrusted input. This document is written as the Phase 7
theme engine lands — the `.fdk` parser is the toolkit's first public API
that consumes a structured file format from potentially untrusted
sources (a user dropping a downloaded theme into their app), so the
rules are set down before the parser, not after the first bug.

The policy applies toolkit-wide: every future parser (image decode,
later theme versions, whatever comes) follows the same rules.

## Threat model

- A theme file is **data**, not code. It may come from the internet,
  a chat attachment, or a hostile party. Nothing in a theme file is
  ever executed, dereferenced as a pointer, used as a length for
  anything but its own bounded scan, or interpreted as a format string.
- The attacker controls every byte of the file: size, line lengths,
  byte values (including NULs and invalid UTF-8), key names, numbers of
  any length, and the total count of entries.
- The user running the application is not the attacker; the goal is
  that a malicious theme can at worst *fail to load* — never crash the
  app, hang it, leak memory proportional to attacker effort, or read
  anything outside the file.

## The rules

1. **Reject first.** The grammar is strict; every deviation is an
   error with a line number. There is no recovery mode, no
   best-effort parse, no warning-and-continue. A malformed theme
   produces `NULL` and a diagnostic — never a half-themed UI and never
   a crash. Unknown keys are errors, not forward-compatibility
   homework.

   The Phase 10 message-catalog parser (`fdk_catalog_parse` /
   `fdk_catalog_load`, grammar in `docs/fdk-catalog-format.md`)
   follows the same rules with its own bounds: 1 MiB input, 1024-byte
   lines, 1024-byte strings, 8192 entries, UTF-8 validation
   (overlongs, surrogates, and raw control bytes rejected), and the
   four-escape-only string sublanguage (`\"` `\\` `\n` `\t` —
   no second language to interpret). The i18n formatters themselves
   add no parsing surface at all: they are pure computation over the
   caller's buffer with hard magnitude limits (see `docs/i18n.md`).

2. **Bounded input.** `fdk_theme_parse()` refuses inputs over 1 MiB
   and `fdk_theme_load()` refuses files over 1 MiB before reading them
   (`fseek`/`ftell` first, allocate second — never `fread` into an
   unbounded buffer). Lines are capped at 1024 bytes; strings at 128
   bytes. Every loop in the parser has a bound derived from these
   caps, never from attacker-supplied counts.

3. **No second language.** No variables, no expressions, no includes,
   no imports, no escape beyond `\"` and `\\`, no float parsing (the
   two numeric forms are hex colors and integers — parsed with
   hand-rolled digit loops, not `strtol`/`sscanf`/`strtod` and their
   locale and overflow surprises). The parser is a finite state
   machine over bytes, not an evaluator of anything.

4. **Allocation discipline.** All allocations go through `fdk_alloc`
   (the toolkit's checked allocator), are bounded by the input caps,
   and are freed on every error path. Partial themes are never
   returned, so partial allocations never escape: on any error the
   whole working object is destroyed. The test suite runs every parser
   case under ASan+UBSan.

5. **Integer care.** All sizes and indices use `size_t`/`fdk_i32` with
   explicit width checks at every narrowing. Hex digit accumulation
   uses `unsigned` and rejects runs longer than 8 digits before any
   shift can overflow. Line counting is `int` with a hard cap. There
   is no pointer arithmetic in the parser beyond `buf + pos` where
   `pos < len` is a loop invariant.

6. **No global state.** `fdk_theme_parse()` is a pure function of its
   arguments. It reads no globals, writes no globals, and touches no
   filesystem. `fdk_theme_load()` does exactly one `fopen` of the
   caller's path — no search paths, no environment variables, no
   `$HOME` probing, no directory enumeration.

7. **NUL bytes are bytes.** The parser works on `(pointer, length)`,
   never on NUL-terminated strings, so an embedded NUL cannot truncate
   a value mid-parse. A NUL byte is simply an invalid character in
   every context (error, with its line number) — it cannot split a
   key, fake a terminator, or alias two lines.

8. **Fail closed on memory pressure.** If any allocation fails, the
   parse fails. No theme is ever constructed from uninitialized or
   partially initialized memory.

## Verification

- `tests/test_theme.c` includes an adversarial corpus: empty input,
   binary garbage, embedded NULs, over-long lines, unterminated
   quotes, wrong value types, out-of-range metrics, duplicate and
   unknown keys, non-`1` versions, CRLF/CR/LF mixes, a UTF-8 BOM, and
   a file that is exactly at each size cap. Every case asserts both
   the NULL return and the exact `fdk_result` code, under
   ASan+UBSan.
- The parser's object code is reviewed against the rule that its only
   loops are: bounded line scan, bounded token scan, bounded digit
   accumulation, and the fixed token/metric tables.

## Non-goals (honestly)

- FDK does not currently cryptographic-verify theme provenance; a
  signature scheme would be pointless this early and wrong until
  there is an ecosystem to sign. Load themes from sources you trust,
  same as fonts.
- FDK does not sandbox file *reads* — `fdk_theme_load(path)` reads the
  path the application gives it, with the application's privileges.
  The application decides what paths are acceptable.
