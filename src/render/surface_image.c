/*
 * surface_image.c — image decoding into offscreen surfaces
 * (fdk_surface_create_from_image; see include/fdk/fdk_surface.h).
 *
 * The decoder is the vendored stb_image v2.30 (third_party/stb/,
 * compiled in stb_image_impl.c). FDK's job here is the policy layer
 * around it, following docs/security.md's rules for
 * attacker-controlled input — a downloaded image is attacker data
 * exactly like a downloaded theme:
 *
 *   - reject-first: stat the path BEFORE any decode; refuse
 *     non-regular files and anything over 512 MiB outright;
 *   - bounded: decode results are re-validated against the same
 *     1..16384 bounds as fdk_surface_create before a single pixel
 *     is trusted;
 *   - fdk_alloc discipline: stb's allocator is routed to
 *     fdk_alloc/fdk_free (stb_image_impl.c), the decoded buffer is
 *     copied into the surface and released on every path — including
 *     failure — and nothing partial is ever handed to the app
 *     (out_surface stays untouched unless FDK_OK is returned);
 *   - no global state: everything is per-call.
 *
 * Not part of the public API surface beyond the one function — never
 * installed.
 */

#define FDK_LOG_TAG "render"

#include "surface_internal.h"

#include "core/log_internal.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* stb_image.h's declarations (implementation lives in
 * stb_image_impl.c; only the prototype surface is included here). */
#include "stb_image.h"

/* Hard ceiling on input file size. Real images are megabytes; this
 * bound exists so a corrupt/hostile "image" cannot make FDK allocate
 * by file size at all (stb allocates by header dimensions, this caps
 * the read side). */
#define FDK_IMAGE_MAX_FILE_BYTES (512u * 1024u * 1024u)

fdk_result fdk_surface_create_from_image(const char *path,
                                         fdk_surface **out_surface) {
    if (path == NULL || out_surface == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Reject-first: the file must exist, be a regular file, and be
     * size-bounded — before any decoding happens. */
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return FDK_ERR_NOT_FOUND;
        }
        /* Directories, permission failures, ... — EISDIR and EACCES
         * are the common shapes; report the honest generic I/O class
         * for all of them. */
        return FDK_ERR_NOT_A_FILE;
    }
    if (!S_ISREG(st.st_mode)) {
        return FDK_ERR_NOT_A_FILE;
    }
    if (st.st_size <= 0) {
        return FDK_ERR_UNSUPPORTED; /* empty file is not an image */
    }
    if ((unsigned long long)st.st_size > (unsigned long long)FDK_IMAGE_MAX_FILE_BYTES) {
        FDK_WARN("image file too large to decode (%lld bytes, max %u): %s",
                 (long long)st.st_size, FDK_IMAGE_MAX_FILE_BYTES, path);
        return FDK_ERR_UNSUPPORTED;
    }

    /* Decode. stbi_load validates the container before allocating the
     * pixel buffer; 4 channels requested so every format (including
     * paletted PNG and grayscale JPEG) arrives as RGBA8 with STRAIGHT
     * (non-premultiplied) alpha — exactly FDK's ARGB8888 layout. */
    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (data == NULL) {
        FDK_WARN("image decode failed: %s: %s", path,
                 stbi_failure_reason() != NULL ? stbi_failure_reason()
                                               : "unknown reason");
        return FDK_ERR_UNSUPPORTED;
    }

    /* Re-validate the decoded dimensions against the surface bounds
     * (a header may legally declare dimensions fdk_surface_create
     * would refuse — 0 or > 16384). */
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        FDK_WARN("image dimensions out of range (%dx%d): %s", w, h, path);
        stbi_image_free(data);
        return FDK_ERR_UNSUPPORTED;
    }

    /* Create the destination surface FIRST, then copy — so a failure
     * to allocate leaves nothing partial behind. */
    fdk_surface *surface = NULL;
    fdk_result r = fdk_surface_create_format(w, h,
                                             FDK_SURFACE_FORMAT_ARGB8888,
                                             &surface);
    if (!fdk_ok(r)) {
        stbi_image_free(data);
        return r;
    }

    /* Row-by-row copy: stb's buffer is tightly packed, the surface's
     * stride is padded (by design, see surface_internal.h). */
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = data + (size_t)y * (size_t)w * 4u;
        fdk_u32 *drow = surface->fb.pixels +
                        (size_t)y * (size_t)surface->fb.stride;
        for (int x = 0; x < w; x++) {
            unsigned char r8 = srow[(size_t)x * 4u + 0u];
            unsigned char g8 = srow[(size_t)x * 4u + 1u];
            unsigned char b8 = srow[(size_t)x * 4u + 2u];
            unsigned char a8 = srow[(size_t)x * 4u + 3u];
            drow[x] = ((fdk_u32)a8 << 24) | ((fdk_u32)r8 << 16) |
                      ((fdk_u32)g8 << 8) | (fdk_u32)b8;
        }
    }

    stbi_image_free(data);

    FDK_DEBUG("image decoded (%dx%d, %d source channels): %s", w, h,
              channels, path);

    *out_surface = surface;
    return FDK_OK;
}
