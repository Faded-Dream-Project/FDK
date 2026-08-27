# Text Rendering

FDK's text layer (`include/fdk/fdk_text.h`, `src/text/`) renders
UTF-8 text from TrueType fonts into any `fdk_surface` — window or
offscreen — through the same clip stack, damage tracking, and
present path as every other primitive. It is fully software and
backend-neutral: glyphs are rasterized once, cached, and alpha-blended
into the framebuffer, so text costs the same on X11 and Wayland and
needs no platform font services.

## The shape of the API

- **`fdk_font_load(path, pixel_size)`** — one font object per
  (file, size). The whole file is read into memory; the sfnt
  container is validated before parsing (see "Untrusted fonts"
  below). The first face of a `.ttc` collection is used.
- **`fdk_font_get_metrics()`** — ascent / descent / line gap /
  line_height, rounded to whole pixels from the font's hhea table.
- **`fdk_font_measure_utf8()`** — advance width plus ink bounds
  (offsets from the baseline) for a single line. Measurement and
  drawing share one shaping walk, so the numbers measure() reports
  are exactly where draw() paints.
- **`fdk_surface_draw_utf8()`** — draws a run at `(pen_x,
  baseline_y)`. Per-glyph alpha bitmaps are blended source-over,
  honoring the clip stack; the run's ink is merged into a single
  damage rectangle.
- **`fdk_font_get_cache_stats()`** — cached/hit/miss/eviction
  counters for the glyph cache (useful for demos and profiling).

Shaping v1 is single-line, left-to-right, codepoint-at-a-time with
pair kerning (`kern` table, basic GPOS pair support via stb).
Ligatures, bidi, and complex scripts are roadmap items; codepoints
missing from the font render as the font's `.notdef` glyph, and
invalid UTF-8 decodes to U+FFFD one bad byte at a time, never reading
past `byte_len`.

## Pipeline

```
UTF-8 bytes
   │  strict-ish RFC 3629 decode, U+FFFD for invalid bytes
   ▼
codepoints ──stbtt_FindGlyphIndex──▶ glyph ids (0 = .notdef)
   │
   │  per glyph: cache lookup (rasterize on miss)
   ▼
float pen advance + pair kerning ──▶ integer glyph placement
   │                                    (each glyph at round(pen))
   ▼
fdk_surface_blend_mask()  ──▶ XRGB8888 framebuffer, clip-stack honored
   │
   ▼
one damage rect per run (ink union ∩ effective clip)
```

- **Rounding**: each glyph is placed at `round(pen)` and the total
  advance is rounded once at the end — so `2 × measure("W")` can
  differ from `measure("WW")` by a pixel (round-of-sum vs
  sum-of-rounds; it drifts both ways). The tests pin this behavior.
- **Damage**: glyphs' ink boxes are unioned in 64-bit space and
  intersected with the renderer's live effective clip, so a
  100-glyph line costs one damage rect and a clipped run damages
  only the visible span. Runs with no ink (pure whitespace) add no
  damage.

## Glyph cache

Each font owns a cache of up to **512 rasterized glyphs**
(fdk_alloc'd, ASan-tracked via the stb allocator hooks). Lookup is a
linear scan of live entries — fine at 512 entries because
rasterization and blending dominate; promotion to a hash table is a
recorded optimization for when profiling justifies it. When the
cache is full, the least-recently-used glyph is evicted. 512 keeps
entire alphabets plus punctuation resident; eviction only matters
for CJK-heavy runs (and then it is correct, just slower).

## stb_truetype (vendored)

Parsing and rasterization are [stb_truetype
v1.26](https://github.com/nothings/stb), vendored at
`third_party/stb/` — FDK's first third-party component. License
(dual MIT / public domain), provenance, SHA-256, and the update
procedure live in `third_party/stb/README.md` and
`THIRD-PARTY-NOTICES.md`. It compiles in exactly one translation unit
(`src/text/stb_truetype_impl.c`) with the aggressive warning set
disabled for that file only, and its allocations routed through
`fdk_alloc`/`fdk_free`. See `docs/licensing-policy.md` for why this
dependency is allowed (and why GPL/LGPL never will be).

## Untrusted fonts

stb_truetype does no range checking of font-file offsets — it assumes
a trusted font (its own header says so). FDK therefore validates the
sfnt container itself before parsing: magic (TrueType flavor only —
CFF/`OTTO` is rejected explicitly rather than rendering tofu), the
table directory inside the file, and every table's
`[offset, offset+length)` extent inside the file. This defeats
garbage files and truncated fonts (the test suite feeds both). It is
not a full audit of table contents: load fonts from sources you
trust.

## Line layout: wrapping and ellipsis

`fdk_font_break_lines_utf8` (src/text/layout.c) is the greedy
word-wrapper. It rides the SAME per-glyph walk as measure and draw
(decode -> kern -> advance), so a returned line's
`advance_width` is by construction the width painting those bytes
produces — there is no second rounding rule in the stack. Rules:
breaks land after space runs (the space stays out of the next line),
a word longer than the line breaks at glyph boundaries rather than
overflowing, `'\n'` is a hard break (`"\r\n"` counts once, empty
lines survive), trailing spaces are trimmed, and kerning never
crosses a line boundary (every line is an independent run — exactly
what `fdk_surface_draw_utf8` does with those bytes). Callers can
count lines first (`max_lines = 0`) and then fill a right-sized
array; a too-small array reports `out_truncated`.

`fdk_font_ellipsize_utf8` finds the longest codepoint-boundary
prefix whose advance leaves room for a U+2026 ellipsis, trailing
spaces trimmed, with explicit degenerate behavior below the
ellipsis's own width (prefix 0 — draw it and let the clip stack
hide the overflow). The ellipsis run is defined once
(`FDK_TEXT_ELLIPSIS_UTF8` in the text internals) so the pass that
measures it and the Label paint hook that draws it cannot drift
apart.

Both passes warm the glyph cache like measurement does (documented
const-laundering through the layer's single `fdk_text_font_mutable`
point).

The Label consumes all of it: `fdk_label_set_mode`
(`NOWRAP`/`WRAP`/`ELLIPSIZE`), `fdk_label_set_alignment`
(START/CENTER/END per line), and `fdk_label_get_line_count`, with
the display cache rebuilt on arrange and lazily at paint — see
`fdk_widgets.h` for the wrap label's documented v1 contract
(natural height is measured at the natural width; there is no
width-for-height layout yet).

## What's deliberately NOT here yet

Recorded on the roadmap, in rough dependency order:

- Subpixel glyph positioning (cache keys on the fractional offset).
- Bold/italic selection beyond loading the specific face file.
- Width-for-height label layout (wrap labels re-flow at the
  ALLOCATED width; their natural height still answers the natural
  width — documented in fdk_widgets.h).
- Bidi and complex scripts; fallback font chains.
- Colored glyphs (emoji), SDF rendering, hinting.

## Testing

`tests/test_text.c` (9 cases, ASan+UBSan): lifecycle and failure
modes, metrics sanity and 2× scale proportionality, measurement
(including the rounding behavior above), draw/damage/ink-bounds
agreement, cache-hit determinism and eviction, clip-stack honoring,
UTF-8 edge cases, greedy wrapping (fit/agreement/hard breaks/
mid-word/truncation), and ellipsis (fits/no-fit/maximal/boundary/
degenerate). `tests/test_x11_integration.c` adds a
server-side readback case that verifies real glyph ink reached the X
server's pixels inside the measured metrics box and nowhere else.
Both suites honestly skip when the environment has no system font.
`examples/05_text.c` exercises all of it live (wordmark, size
ladder, measured-run chaining, animated per-glyph wave, live cache
stats).
