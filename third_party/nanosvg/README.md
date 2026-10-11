# Vendored: nanosvg + nanosvgrast

Single-header SVG parser and rasterizer by Mikko Mononen
(memononen/nanosvg on GitHub), zlib license (both alternatives in
THIRD-PARTY-NOTICES.md).

- `nanosvg.h` — parser: SVG subset → flat list of filled paths
  (moveto/lineto/bezier, transforms, groups, linear/radial gradients,
  stroke fills, opacity, inline `style="fill:..."` presentation
  attributes).
- `nanosvgrast.h` — rasterizer: analytic-edge antialiased scanline
  fill of the parsed paths into an RGBA8 buffer at any scale.

## Why this library

FDK's icon-theme engine resolves freedesktop icon themes, and the
themes real desktops ship (Adwaita, Breeze, Yaru, Papirus, ...) keep
most of their sizes as SVG only — a PNG-only engine sees misses where
the desktop shows icons. FDK's dependency posture is no glib, no
cairo, no gdk-pixbuf (the toolkit renders with its own software
rasterizer), so librsvg is out: it would drag that entire stack into
every FDK application's link line. nanosvg is the stb-shaped answer:
two self-contained headers, zero link dependencies, rasterizes
straight into an RGBA buffer — the exact input format
`fdk_surface_create` wants.

## The supported subset (the honest limits)

nanosvg deliberately implements the icon-shaped subset of SVG:
geometry, transforms, gradients, opacity. It does NOT implement
`<text>`, CSS classes/`<style>` blocks (Breeze's ColorScheme-*
classes), filters, masks, or clip-path references. Icon-theme SVGs
are overwhelmingly plain paths with inline fills — Adwaita's entire
scalable tree parses cleanly. When an SVG nonetheless needs an
unsupported feature, the parse yields empty or zero-coverage output;
FDK's policy layer (src/theme/icon_svg.c) detects that and reports
the icon as a MISS so the chain keeps searching (other sizes, other
themes, the procedural glyph floor) instead of drawing a blank.

## Provenance

- Source: https://github.com/memononen/nanosvg (master, 2023-era
  final revision; the repository is in maintenance mode)
- SHA-256:
  - nanosvg.h     e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0
  - nanosvgrast.h 79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8

## Update procedure

1. Download both headers from upstream.
2. Record new SHA-256 sums here and in THIRD-PARTY-NOTICES.md.
3. `make clean && make && make test` — the icon-theme suite pins
   rasterization behavior (color identity, aspect fit, empty-asset
   miss) and will catch regressions.
4. Keep the allocator note in sync: this version exposes no
   allocator macros and uses plain malloc/free internally; every
   allocation it makes is freed by its own nsvgDelete /
   nsvgDeleteRasterizer pair, and the buffers FDK hands it or
   receives from it are allocated and freed on FDK's side
   (src/theme/icon_svg.c). Nothing crosses the boundary unpaired.
