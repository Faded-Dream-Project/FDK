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

Shaping (M-2 level 1, 1.1.0) is single-line with the full
complex-script pipeline when HarfBuzz and FriBidi are loadable at
run time — the Unicode bidi algorithm (Arabic, Hebrew, Persian, Urdu
render in visual order, not backwards), GSUB/GPOS shaping
(ligatures, contextual forms, mark attachment, real kerning), and a
per-script fontconfig fallback so CJK next to Latin picks the right
font per segment. Without the libraries the classic
codepoint-at-a-time LTR walk runs instead, with metrics identical to
1.4.x. See "Complex-script shaping" below. Codepoints missing from
the font AND every fallback render as the font's `.notdef` glyph,
and invalid UTF-8 decodes to U+FFFD one bad byte at a time, never
reading past `byte_len`.

## System font discovery (1.0.1)

`fdk_font_load_system_default(pixel_size)` picks a UI font without
the application naming a file. FDK bundles no font (licensing
posture), so discovery follows the same chain GTK and QT use on
Linux, minus their hard build-time dependency — fontconfig is
loaded at RUN TIME with `dlopen`, so a build needs nothing new and a
stripped container without it simply falls through:

1. **`$FDK_FONT_FILE`** — explicit user override: one font file
   path. An invalid override logs a warning and falls through
   rather than bricking text.
2. **`$FDK_FONT_DIRS`** — `:`-separated directories scanned first
   (recursive, filename-ranked — see below).
3. **fontconfig** — the generic `sans-serif` family under the
   user's own fontconfig policy (per-user fonts, distro aliases,
   the works). The best-match list is walked past candidates FDK
   cannot load (containers that fail validation — since the CFF
   work, `OTTO` faces are loadable, so Noto Sans CJK and friends
   resolve instead of being walked past) and past files that fail
   the container validation, honoring the collection face index
   (`FC_INDEX`) fontconfig reports.
4. **Known exact paths** — the historical candidate list (DejaVu,
   Liberation, FreeSans, Noto) covering the common
   Debian/Ubuntu/Arch layouts deterministically.
5. **Standard roots scan** — a ranked recursive scan of
   `/usr/share/fonts`, `/usr/local/share/fonts`,
   `$XDG_DATA_HOME/fonts` (default `~/.local/share/fonts`), and
   `~/.fonts`.

The scanner ranks filenames: the known Latin UI families (DejaVu
Sans, Noto Sans, Liberation Sans, FreeSans, Cantarell, Adwaita
Sans, Ubuntu Sans) first, then Noto Sans SC / legacy Ubuntu, CJK
faces last; within a family the regular/default instance wins over
bold/light/condensed variants (FDK synthesizes those styles
itself). Bracket-named variable fonts such as Arch's
`NotoSans[wdth,wght].ttf` count as the regular instance — stb
rasterizes the default master — which is exactly the case the old
hardcoded list missed.

A candidate that passes the tag-level gate but fails the loader's
full container validation (truncated repacks, corrupt files) is
remembered and the next-best candidate takes the slot, so one
broken font on a system cannot break text everywhere.

Which file won is introspectable: `fdk_font_get_file_path(font)`
returns the path any font was loaded from — the answer to "which
face did FDK actually pick", the same question QFontInfo and
pango answer for QT and GTK applications. The resolution (hit or
miss) is cached for the process lifetime, so repeated calls are
cheap after the first.

## Pipeline

```
UTF-8 bytes
   │  strict-ish RFC 3629 decode, U+FFFD for invalid bytes
   ▼
codepoints ──stbtt_FindGlyphIndex──▶ glyph ids (0 = .notdef)
   │
   │  per glyph: cache lookup (rasterize on miss, PER PHASE)
   ▼
float pen advance + pair kerning ──▶ SUBPIXEL glyph placement
   │                                    (left edge at floor(pen),
   │                                     fraction -> 4-phase raster)
   ▼
fdk_surface_blend_mask()  ──▶ XRGB8888 framebuffer, clip-stack honored
   │
   ▼
one damage rect per run (ink union ∩ effective clip)
```

- **Subpixel positioning**: each glyph's left edge is placed at
  `floor(pen)` and the fractional remainder (kerning and fractional
  advances make it non-integral) is quantized into one of 4 phases
  (0, 1/4, 1/2, 3/4) baked into that phase's rasterization — every
  glyph paints within 1/8 px of where the float pen actually is.
  The total advance stays `round(final pen)`, exactly the v1
  behavior, and y stays integer (lines sit on the baseline). A pen
  shift of a whole pixel reuses the same phase keys — translated
  ink, zero re-rasterization.
- **Damage**: glyphs' ink boxes are unioned in 64-bit space and
  intersected with the renderer's live effective clip, so a
  100-glyph line costs one damage rect and a clipped run damages
  only the visible span. Runs with no ink (pure whitespace) add no
  damage.

## Synthetic styles

`fdk_font_set_style(font, FDK_FONT_STYLE_BOLD | FDK_FONT_STYLE_ITALIC)`
synthesizes the two classic styles on whatever face the font object
loaded — SYNTHESIS, not face selection. When a real bold or italic
face FILE exists, `fdk_font_load()` of that file is the better
choice; a designed face beats a synthesized one.

- **BOLD** widens strokes by `pixel_size/24` px (min 1) via
  horizontal dilation (max-window), and the advance grows by the
  same stem — so a bold run measures exactly as wide as it paints.
  The ink's height is untouched.
- **ITALIC** is an oblique shear (~12 degrees) anchored at the
  baseline: ascenders lean right, descenders left. The advance is
  deliberately unchanged (CSS `font-synthesis` semantics) — the
  slanted ink may overhang the next glyph's slot, which the damage
  model already covers.

Both passes run at raster time, so a style change flushes the glyph
cache (every cached bitmap is now the wrong shape); the next
measure/draw re-rasterizes on demand. Measurement always reflects
the current style. Re-setting the same style is a no-op.

## Glyph cache

Each font owns a cache of up to **2048 rasterized glyph
entries** (fdk_alloc'd, ASan-tracked via the stb allocator hooks).
Subpixel positioning keys each entry by *(glyph, subpixel phase)* —
four phases per distinct glyph — so 2048 entries preserve the
real-glyph coverage the original 512 gave at integer positioning.
Lookup is a linear scan of live entries — fine at 2048 entries
because rasterization and blending dominate; promotion to a hash
table is a recorded optimization for when profiling justifies it.
When the cache is full, the least-recently-used entry is evicted.
That keeps entire alphabets plus punctuation resident across all
phases; eviction only matters for CJK-heavy runs (and then it is
correct, just slower).

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
sfnt container itself before parsing: magic (TrueType flavor or
CFF-flavored OpenType `OTTO` — both load; see the CFF section), the
table directory inside the file, and every table's
`[offset, offset+length)` extent inside the file. This defeats
garbage files and truncated fonts (the test suite feeds both, plus a
deterministic mutation fuzz over the CFF table region). It is
not a full audit of table contents: load fonts from sources you
trust. For `OTTO` faces the CFF table itself is additionally parsed
by FDK's own bounds-checked Type 2 module BEFORE stb ever touches
the face — see below for why that order is load-bearing.

## Complex-script shaping (M-2 level 1, 1.1.0)

The full pipeline — `src/text/shaping.c` — is the GLX/fontconfig
posture applied to text: **HarfBuzz and FriBidi are dlopen'd at run
time, never build dependencies, never dlclose'd**. A machine without
them gets the 1.4.x codepoint-at-a-time walk, bit-identical metrics;
a machine with them gets real complex-script rendering. `$FDK_SHAPING`
pins it: `0` forces the legacy walk, `1` requires the libraries (a
loud error if missing), unset auto-probes. `fdk_text_shaping_available()`
reports the verdict; the soname probe covers Debian's
`libharfbuzz.so.0b` spelling and everyone else's `libharfbuzz.so.0`.

The pipeline, one shaped range at a time:

```
UTF-8 range ─▶ decode ─▶ FriBidi (paragraph direction, embedding
                            levels, visual->logical map; embedding
                            controls like LRE/RLE are REMOVED)
                │
                ├─ segment by (direction parity, script, style, font)
                │    └─ font per segment: the primary if it covers
                │       the script, else fontconfig's answer for the
                │       script's language (charset-filtered on a probe
                │       codepoint — FC_LANG alone lets weak matches
                │       through), cached on the primary font
                │
                └─ per segment: mirror brackets (RTL) ─▶ HarfBuzz
                     shape (GSUB/GPOS: ligatures, joining forms,
                     mark attachment, kerning) ─▶ placements
```

**Placements are the one seam**: every consumer — measure, draw,
wrap, ellipsize, the Entry caret and hit-test — reads the same array
of `(glyph raster, left-edge x, cumulative pen, cluster byte)`
entries in visual order. The legacy walk produces the same structure
(one placement per codepoint), so there is still exactly one
rounding rule in the stack (the pen floors per glyph, rounds once
per reported width), and Latin metrics are IDENTICAL across both
postures — `tests/test_shaping.c` pins that by re-exec'ing itself
under the other posture and comparing.

The glyph metrics HarfBuzz shapes against come from stb_truetype
itself (custom `hb_font_funcs`: nominal glyph, advances, extents),
pre-scaled to 26.6 pixels against FDK's pixel-height box — so a
shaped advance and a legacy advance of the same glyph agree by
construction. The synthetic-bold stem is added to shaped advances
exactly as the rasterizer bakes it into cached advances (one shared
`fdk__text_bold_stem()`).

Level-1 scope, stated honestly:

- each measured/drawn range is its own bidi paragraph (the standard
  level-1 toolkit semantics; wrapped lines re-order per line);
- line breaking stays LOGICAL (spaces, hard breaks); ligature
  clusters never split (the per-cp attribution lands a merged
  glyph's advance on its cluster's last codepoint);
- the Entry caret maps through the visual placement boundary —
  exact for single-direction text (including CJK), segment
  granularity for mixed-direction runs;
- embedding controls (LRE/RLE/PDF/LRO/RLO and friends) that FriBidi
  removes contribute no glyphs and no advance — a run made only of
  them is invisible;
- color emoji stays out of scope (ROADMAP C-3, after HarfBuzz).

The fallback chain: one font per script per primary font, loaded at
the same pixel size, `is_fallback`-flagged (never gets fallbacks of
its own — two levels, no cycles), destroyed with its primary.
`fdk_font_has_glyph()` answers coverage questions directly. Fallback
candidates may be either sfnt flavor (the loader takes CFF outlines
since the native Type 2 module landed — before it, a CJK font set
made only of CFF faces, as on Arch, left every fallback candidate
unloadable and CJK text rendered as `.notdef` boxes — the tofu
report that closed with this item); a fallback that loads but does
not cover the segment's probe codepoint is DISCARDED AND THE WALK
RETRIES the next fontconfig candidate (up to four — a bad ranking
is a next-candidate event, not a dead script). On a machine where
no candidate answers, the primary's `.notdef` renders, honestly AND
LOUDLY: a WARN names the script, the probe codepoint, and the reason
(no fontconfig answer / load failure / no coverage) once per font+
script, and the chosen face is INFO-logged per script. Script
classification itself (the codepoint→script table the segmenter
binary-searches) is order-sensitive by construction: the table is
sorted by `lo`, a probe-time self-check ERRORs if a future edit
breaks that, and the six-script matrix pins it from the request
side.

## CFF outlines — OpenType 'OTTO' faces (1.1.0)

CFF-flavored OpenType (`OTTO` sfnt carrying a `CFF `/`CFF2` table —
Noto Sans CJK, URW base35, most .otf fonts) renders through FDK's
own Type 2 charstring module, `src/text/cff.c` — an original
implementation written from the public Adobe specifications (TN
5176, TN 5177, the OpenType CFF2 chapter; provenance in
`docs/THIRD-PARTY.md`). The vendored stb_truetype carries a Type 2
interpreter of its own, but it initializes its CFF view with a
FABRICATED 512 MiB table extent (its own `@TODO` admits it), has no
CFF2 support, and asserts on malformed INDEX/DICT bytes — a single
mutated byte in a Private DICT takes `stbtt_InitFont` down through
`stbtt__cff_int`'s assert. FDK's module owns the whole CFF path
with real bounds, budgets and depth caps instead.

**The load order is load-bearing**: for an `OTTO` face, the native
parser runs FIRST and the face is rejected outright when the CFF
table fails to parse — only then does `stbtt_InitFont` run (for the
cmap/hmtx/hhea side, which is format-agnostic). stb never sees a
CFF table the hardened parser rejected; the fuzz battery found the
assert hazard live before the reorder closed it.

**The seam**: glyph geometry comes back as `stbtt_vertex` arrays in
font units (through the FontMatrix, int16-clamped), rasterized by
the SAME public `stbtt_Rasterize` call the glyf path uses — one
rasterizer, one look, no drift between flavors. Advances and the
vertical metrics still come from hmtx/hhea through stb; HarfBuzz's
glyph-extents callback routes through the native ink box (GPOS mark
attachment rides it). maxp `numGlyphs` must equal the CharStrings
INDEX count (the OpenType CFF chapter's rule) — a mismatch rejects
the face.

**What the interpreter covers**: both dialects (`CFF ` incl.
CID-keyed FDArray/FDSelect — the Noto posture — and `CFF2` at the
default instance, blend deltas dropped), local and global subrs
with the spec's bias rule, the full curve operator set including
the four flex forms, endchar-seac composition (StandardEncoding
codes resolved through the charset), hint masks sized from the
declared stems, and the IMPLICIT contour close (Type 2 charstrings
need not line back to their moveto — the consumer closes them, and
the closing vertex is synthesized exactly where stb's own decode
puts it).

**The fence**: for every glyph examined, this module and stb's own
Type 2 path must emit byte-identical vertex streams — two
independent decodings of the same charstrings agreeing is the
strongest reference available without shipping a third
rasterizer. `tests/test_cff.c` sweeps real URW and Noto faces
against it and pins a crafted seac/subr/cyclic-subr font (real
foundry fonts no longer carry seac), plus loader rejection and a
deterministic mutation fuzz over the CFF table region.

## Line layout: wrapping and ellipsis

`fdk_font_break_lines_utf8` (src/text/layout.c) is the greedy
word-wrapper. It rides the shaped result's per-codepoint LOGICAL pen
attribution (the same placements measure and draw consume), so a
returned line's `advance_width` is by construction the width shaping
those bytes produces — there is no second rounding rule in the
stack. Rules:
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

- ~~Subpixel glyph positioning~~ — SHIPPED (4-phase subpixel
  rasterization above).
- ~~Bold/italic selection beyond loading the specific face file~~ —
  SHIPPED (synthesis above; designed-face files remain better).
- Width-for-height label layout (wrap labels re-flow at the
  ALLOCATED width; their natural height still answers the natural
  width — documented in fdk_widgets.h).
- ~~Bidi and complex scripts; fallback font chains~~ — SHIPPED at
  level 1 (M-2: the dlopen'd HarfBuzz + FriBidi + fontconfig
  fallback pipeline above). Level 2 (full paragraph bidi with
  per-line direction carrying, justification, true RTL caret
  granularity) stays on the roadmap.
- ~~CFF-flavored OpenType (`OTTO`)~~ — SHIPPED (the native Type 2
  module above; CFF2 renders at the default instance — variation
  instances are roadmap material with the rest of variable-font
  support).
- ~~Colored glyphs (emoji)~~ — SHIPPED (the color-glyph module
  below; SDF rendering, hinting, and variable-font instances beyond
  the default stay roadmap material).

## Color glyphs — emoji (C-3, 1.1.0)

`src/text/emoji.c` parses the three color-glyph families the Linux
desktop actually meets — all natively from the public OpenType
specifications, every field layout pinned against fontTools (the
reference implementation; it generated the test fixtures):

- **COLRv0 + CPAL** — the layer-list format. A base glyph maps to
  layer records (outline glyph + palette index); the layers
  composite source-over bottom-to-top through the SAME stb
  rasterizer mono text uses (color and mono coverage are
  pixel-identical by construction).
- **COLRv1 + CPAL** — the paint-graph format. The engine evaluates
  the graph per pixel into depth-indexed scratch buffers: glyph
  clips, solid/linear/radial/sweep fills (evaluated in the paint's
  own coordinate space through the inverse accumulated transform),
  every transform flavor (rotate/skew route through a supersampled
  nonzero-winding rasterizer; axis-aligned transforms ride stb's own
  rasterizer), PaintColrGlyph reuse, clip boxes, and the 28 W3C
  blend modes. Variable (PaintVar*) paints render at the DEFAULT
  instance — the deltas are skipped, matching the toolkit's
  no-variable-fonts posture. Palette index 0xFFFF ("the foreground
  color") renders opaque black: the glyph cache has no per-draw
  color context to thread a foreground through (FreeType's own
  default), and only palette 0 is read.
- **CBDT/CBLC and sbix** — bitmap strikes (PNG payloads decoded
  through the same stb_image path the clipboard rides). Strike
  selection by log-ratio against the requested pixels-per-em;
  downscaling is a box filter with exact fractional coverage, upscaling
  bilinear.

A color glyph's cache entry carries straight-RGBA8 pixels and
composites through `fdk_surface_blend_rgba` (internal) — the run
color tints nothing. The public API is unchanged. Synthetic
bold/italic do not apply (they are outline concepts; an emoji is an
image). `$FDK_NO_COLOR_GLYPHS=1` disables the color path for OUTLINE
faces (the reference glyph draws mono, tinted); bitmap-only faces
ignore it.

**The bitmap-only loader gate**: a face with no glyf and no CFF but
a real CBDT/CBLC pair — the fonts-noto-color-emoji posture, 10.5 MB
of PNG strikes and zero outlines — loads. stbtt_InitFont's outline
requirement is bypassed and the stbtt_fontinfo hand-filled (cmap
subtable selection walks stb's own preference order), so glyph ids,
advances, and metrics answer exactly as for an outline face; the
outline rasterizer is fenced off (`font->bitmap_only`).

The shaping pipeline routes emoji to an emoji font through the script
table's Zsye bucket (Extended_Pictographic ranges; fontconfig
`und-zsye` ranking with a charset probe doing the decisive filter) —
a text-presentation symbol the primary font covers still resolves to
the primary. `tests/test_emoji.c` is the rig.

## Testing

`tests/test_cff.c` (the CFF-outline item): the crafted seac/subr/
cyclic-subr font (geometry pinned vertex-exact, cross-checked
against stb's own decode), the vertex-agreement sweep over real URW
name-keyed and Noto CID-keyed faces, loader rejection (numGlyphs
cross-check, hostile table extents, truncation, CFF2-tag-on-CFF1),
and a deterministic mutation fuzz over the CFF table region.

`tests/test_shaping.c` (M-2): the complex-script suite — RTL visual
order, mixed bidi, per-script fallback selection (the charset-
filtered fontconfig query), the six-script fallback matrix
(Arabic, Devanagari, Hebrew, Thai, Bengali, Han: fallback resolved,
no glyph 0, Arabic contextual joining, Devanagari conjunct merging —
a fontconfig answer without a shaped fallback is the unsorted-table
failure signature, a FAILURE not a skip), cluster/byte mapping, caret monotonicity,
Arabic wrap and ellipsize agreement, draw/measure agreement on a
trilingual string, and the Latin compatibility pin (the suite
re-executes itself under the other posture and compares metrics). Both
postures run in CI (the ubuntu job runs the auto probe and an
explicit `FDK_SHAPING=0` leg). `make test-textdemo`
(tests/textdemo_rig.sh) adds the §10 pixel rig: text_demo
screenshots compared shaped-vs-legacy with join anchors and a
negative control.

`tests/test_text.c` (11 cases, ASan+UBSan): lifecycle and failure
modes, metrics sanity and 2× scale proportionality, measurement
(including the rounding behavior above), draw/damage/ink-bounds
agreement, cache-hit determinism and eviction, clip-stack honoring,
UTF-8 edge cases, greedy wrapping (fit/agreement/hard breaks/
mid-word/truncation), and ellipsis (fits/no-fit/maximal/boundary/
degenerate) — plus the completion slice's two: synthetic styles
(argument safety, masking, bold/italic advance contracts, cache
flush + idempotence, draw agreement) and subpixel positioning
(fractional-pen agreement, determinism, pen-shift invariance, phase
fan-out). `tests/test_x11_integration.c` adds a
server-side readback case that verifies real glyph ink reached the X
server's pixels inside the measured metrics box and nowhere else.
Both suites honestly skip when the environment has no system font.
`examples/05_text.c` exercises all of it live (wordmark, size
ladder, measured-run chaining, animated per-glyph wave, live cache
stats).
