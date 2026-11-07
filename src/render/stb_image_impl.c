/*
 * stb_image_impl.c — the single translation unit that compiles the
 * vendored stb_image implementation (third_party/stb/stb_image.h,
 * v2.30; provenance and update procedure in third_party/stb/README.md).
 *
 * Same two deliberate deviations from a stock `#define
 * STB_IMAGE_IMPLEMENTATION` drop-in as stb_truetype_impl.c:
 *
 *  1. stb's allocator macros are routed to FDK's fdk_alloc/fdk_free so
 *     decoded image buffers live in the same allocator (and therefore
 *     the same sanitizer/leak accounting) as every other FDK
 *     allocation. FDK never frees what it did not allocate — the
 *     decoded RGBA buffer is copied into an offscreen surface and
 *     released here, in this TU, through the same macro pair.
 *
 *  2. stb_image is not warning-clean under FDK's full aggressive
 *     warning set (-Wconversion, -Wsign-conversion, ...). Rather than
 *     weakening the project-wide set, diagnostics are pushed off for
 *     the vendored include in THIS translation unit only and restored
 *     right after. Every FDK-authored file keeps the full set.
 *
 * Not part of the public API — never installed.
 */

#include "core/alloc_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_MALLOC(sz) fdk_alloc(sz)
#define STBI_FREE(p)    fdk_free(p)
#define STBI_REALLOC(p, sz) fdk_realloc(p, sz)
#define STBI_FAILURE_USERMSG  1 /* stbi_failure_reason() strings mention
                                   * the actual problem; FDK logs them at
                                   * WARN and never hands them to the app
                                   * (see surface_image.c) */

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
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif

#include "stb_image.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
