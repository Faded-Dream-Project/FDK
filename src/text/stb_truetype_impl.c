/*
 * stb_truetype_impl.c — the single translation unit that compiles the
 * vendored stb_truetype implementation (third_party/stb/stb_truetype.h,
 * v1.26; provenance and update procedure in third_party/stb/README.md).
 *
 * Two deliberate deviations from a stock `#define IMPLEMENTATION`
 * drop-in:
 *
 *  1. stb's allocator macros are routed to FDK's fdk_alloc/fdk_free so
 *     glyph bitmaps live in the same allocator (and therefore the same
 *     sanitizer/leak accounting) as every other FDK allocation.
 *
 *  2. stb_truetype is not warning-clean under FDK's full aggressive
 *     warning set (-Wconversion, -Wsign-conversion, ...). Rather than
 *     weakening the project-wide set, diagnostics are pushed off for
 *     the vendored include in THIS translation unit only and restored
 *     right after. Every FDK-authored file keeps the full set.
 *
 * Not part of the public API — never installed.
 */

#include "core/alloc_internal.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(x, u) ((void)(u), fdk_alloc(x))
#define STBTT_free(x, u)   ((void)(u), fdk_free(x))

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#endif

#include "stb_truetype.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
