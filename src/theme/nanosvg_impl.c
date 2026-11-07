/*
 * nanosvg_impl.c — the single translation unit that compiles the
 * vendored nanosvg parser + rasterizer (third_party/nanosvg/, provenance
 * and update procedure in third_party/nanosvg/README.md).
 *
 * One deliberate deviation from a stock drop-in, same policy as
 * stb_image_impl.c: diagnostics are pushed off for the vendored
 * includes in THIS translation unit only and restored right after.
 * Every FDK-authored file keeps the full aggressive set
 * (-Wconversion, -Wsign-conversion, ...).
 *
 * Allocator note (why no macro routing, unlike stb): this nanosvg
 * revision exposes no NSVG_MALLOC/NSVG_FREE seam — it calls plain
 * malloc/realloc/free internally. That is safe here because nothing
 * nanosvg allocates ever crosses the FDK boundary: the parser's
 * allocations are all released by nsvgDelete(image), the
 * rasterizer's by nsvgDeleteRasterizer(rast), and both are called on
 * every path (including failure) by the policy layer in
 * src/theme/icon_svg.c. The buffers that DO cross — the SVG text we
 * hand to nsvgParse and the RGBA raster we get back — are allocated
 * and freed on FDK's side. Under the sanitizers, plain malloc is
 * interposed globally anyway, so leak accounting is unchanged.
 *
 * Not part of the public API — never installed.
 */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
