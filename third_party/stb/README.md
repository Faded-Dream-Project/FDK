# Vendored: stb_truetype.h, stb_image.h

FDK vendors **stb_truetype v1.26** as its TrueType/OpenType parsing and
glyph-rasterization engine, and **stb_image v2.30** as its image decoder
(PNG/JPEG/BMP/... into ARGB8888 surfaces, Phase 3 completion). Both are
single-file public-domain/MIT libraries from the same upstream.

- **Upstream:** https://github.com/nothings/stb (`stb_truetype.h`,
  `stb_image.h`, master at time of vendoring)
- **Version:** v1.26 (`// stb_truetype.h - v1.26 - public domain`)
- **SHA-256:** `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab`
- **License:** dual MIT / public domain (see the license block at the
  end of the file, reproduced in `/THIRD-PARTY-NOTICES.md`). Both
  alternatives are on FDK's allow-list (`docs/licensing-policy.md`);
  neither imposes copyleft or attribution-beyond-notice obligations.

## Why vendored (not a system dependency)

- Single header, no build-system dependency, no pkg-config, no ABI
  surface — matches FDK's zero-external-dependency build.
- It is the industry-standard choice for dependency-free C text
  (used by raylib and many others); writing a from-scratch font
  parser + rasterizer would add thousands of lines of high-risk code
  for no architectural gain at this stage.
- It keeps FDK's rendering fully software and backend-neutral, which
  is the project's direction.

## How it is compiled

`src/text/stb_truetype_impl.c` is the single translation unit that
defines `STB_TRUETYPE_IMPLEMENTATION`. stb_truetype is not
warning-clean under FDK's full aggressive warning set
(`-Wconversion`, `-Wsign-conversion`, …), so that one TU disables
those diagnostics with pragma push/pop **for the vendored code only**;
every FDK-authored file still compiles under the full set. The stb
allocator macros are routed to FDK's `fdk_alloc`/`fdk_free` so the
glyph bitmaps are tracked by the same allocator (and the same
sanitizers) as everything else.

## Updating

Replace the header, update the version + SHA-256 above, re-run the
full test suite (`make test`, `make test-x11`), and check the diff of
the public stb API surface used by `src/text/text.c`
(`stbtt_InitFont`, `stbtt_ScaleForPixelHeight`, `stbtt_FindGlyphIndex`,
`stbtt_GetGlyphBitmap`, `stbtt_GetGlyphBitmapBox`, `stbtt_GetGlyphAdvance`,
`stbtt_GetGlyphKernAdvance`, `stbtt_GetGlyphBox`, `stbtt_GetFontVMetrics`).

## Security note

stb_truetype does not range-check offsets in font files (see its own
header warning). FDK only feeds it fonts the application explicitly
loads via `fdk_font_load()`; FDK never loads fonts from untrusted
sources on its own.

## stb_image.h v2.30

- **Version:** v2.30 (`// stb_image.h - v2.30 - public domain image loader`)
- **SHA-256:** `594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3`
- **License:** dual MIT / public domain — identical block as stb_truetype
  (same author, same file footer).
- **Compiled in:** `src/render/stb_image_impl.c` (single TU, allocator
  routed to fdk_alloc/fdk_free, aggressive warnings suppressed for the
  vendored code only — same discipline as stb_truetype_impl.c).
- **Used by:** `src/render/surface_image.c` — `fdk_surface_create_from_image`.
- **Security note:** decoded images are attacker-controlled data exactly
  like downloaded themes (docs/security.md): FDK bounds the input file
  size before decode (512 MiB cap), re-validates the decoded dimensions
  against fdk_surface_create's 1..16384 limits, and never hands out
  partial results.

### Updating

Replace the header, update the version + SHA-256 above, re-run the full
test suite (`make test`, `make test-x11`), and check the diff of the
public stb API surface used by `src/render/surface_image.c`
(`stbi_load`, `stbi_failure_reason`, `stbi_image_free`).
