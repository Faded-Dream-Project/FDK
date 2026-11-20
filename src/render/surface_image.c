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
#include "core/context_internal.h" /* fdk__context_active: budget override */

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

/* Pixel budget (fuzz-lane finding, 2026-11-20; per-format 1.0.0-dev):
 * stb's GIF decoder allocates up to THREE full-frame buffers (out +
 * background + history), while PNG/JPEG decode into ONE — so a
 * single global limit either strangles real photos or leaves the
 * GIF worst case at hundreds of MiB. A 27-byte GIF claiming
 * 24836x14080 OOMed the process inside the decoder before any
 * post-decode check could run, and a "legal" 16132x16191 frame
 * (both sides under 16384) still summed to ~2.2 GB.
 * stbi_info_from_memory/stbi_info parse ONLY the header (no frame
 * allocation), so this pre-flight rejects by total pixels before
 * any buffer exists — per FORMAT:
 *
 *   PNG / JPEG / others: 96 Mi pixels — a 24 MP photo, an 8K
 *     wallpaper (33.2 Mi), full-frame 12 MP + margin. Single
 *     buffer: 384 MiB worst case at the cap.
 *   GIF: 16 Mi pixels — the triple buffer bounds at 192 MiB.
 *     Real GIFs are banners and short clips; 16 Mi is generous
 *     and the hostile-input headroom stays tight.
 *
 * Named, not magic. The app override (fdk_init_options.
 * image_max_pixels) sets the PNG/JPEG class cap directly and the
 * GIF class to one sixth of it, keeping the 6:1 worst-case ratio;
 * 0 (zero-init) keeps these defaults. */
#define FDK_IMAGE_MAX_PIXELS_PNG_JPEG \
    (96u * 1024u * 1024u)
#define FDK_IMAGE_MAX_PIXELS_GIF (16u * 1024u * 1024u)
/* The override's floor and the GIF:PNG ratio it preserves. */
#define FDK_IMAGE_BUDGET_RATIO_GIF 6u

/* The process-wide effective caps, resolved once at first use
 * (fdk_init sets the context field before any decode can run;
 * single-context processes are every FDK app today — see the
 * context_internal.h note). 0,0 = not yet resolved. */
static unsigned int g_budget_png_jpeg = 0;
static unsigned int g_budget_gif = 0;

static void budget_resolve(void) {
    if (g_budget_png_jpeg != 0) {
        return;
    }
    unsigned int cap = FDK_IMAGE_MAX_PIXELS_PNG_JPEG;
    unsigned int gif = FDK_IMAGE_MAX_PIXELS_GIF;
    const fdk_context *ctx = fdk__context_active();
    if (ctx != NULL && ctx->image_max_pixels != 0) {
        cap = ctx->image_max_pixels;
        gif = cap / FDK_IMAGE_BUDGET_RATIO_GIF;
        if (gif < (1u << 20)) {
            gif = 1u << 20; /* never below the init floor */
        }
    }
    g_budget_png_jpeg = cap;
    g_budget_gif = gif;
}

/* Which budget a stream falls under. stbi_info does not report the
 * format; a cheap magic sniff does (GIF87a/GIF89a are the only
 * multi-buffer codecs on the stb path). */
static bool is_gif_header(const void *buf, int len) {
    const unsigned char *b = buf;
    return len >= 6 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' &&
           b[3] == '8' && (b[4] == '7' || b[4] == '9') && b[5] == 'a';
}

static unsigned int budget_for(const void *buf, int len) {
    budget_resolve();
    return is_gif_header(buf, len) ? g_budget_gif : g_budget_png_jpeg;
}

static unsigned int budget_for_file(FILE *f) {
    unsigned char head[6];
    size_t got = fread(head, 1, sizeof head, f);
    rewind(f);
    budget_resolve();
    if (got >= 6 && head[0] == 'G' && head[1] == 'I' &&
        head[2] == 'F' && head[3] == '8' &&
        (head[4] == '7' || head[4] == '9') && head[5] == 'a') {
        return g_budget_gif;
    }
    return g_budget_png_jpeg;
}

static fdk_result fdk__image_header_budget_ok(const void *buf,
                                              int len) {
    int w = 0, h = 0;
    if (stbi_info_from_memory((const stbi_uc *)buf, len, &w, &h,
                              NULL) == 0) {
        return FDK_OK; /* not a parseable header: let the decoder
                        * produce its honest refusal below */
    }
    unsigned int budget = budget_for(buf, len);
    if (w > 0 && h > 0 &&
        (unsigned long long)w * (unsigned long long)h >
            (unsigned long long)budget) {
        FDK_WARN("image dimensions exceed pixel budget (%dx%d, max "
                 "%u pixels)", w, h, budget);
        return FDK_ERR_UNSUPPORTED;
    }
    return FDK_OK;
}

static fdk_result fdk__image_header_budget_ok_file(const char *path) {
    int w = 0, h = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return FDK_OK; /* the caller's stat/decode reports the real
                        * error; budget-check nothing here */
    }
    unsigned int budget = budget_for_file(f);
    fclose(f);
    if (stbi_info(path, &w, &h, NULL) == 0) {
        return FDK_OK;
    }
    if (w > 0 && h > 0 &&
        (unsigned long long)w * (unsigned long long)h >
            (unsigned long long)budget) {
        FDK_WARN("image dimensions exceed pixel budget (%dx%d, max "
                 "%u pixels): %s", w, h, budget, path);
        return FDK_ERR_UNSUPPORTED;
    }
    return FDK_OK;
}

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
    {
        fdk_result budget = fdk__image_header_budget_ok_file(path);
        if (!fdk_ok(budget)) {
            return budget;
        }
    }

    {
        fdk_result budget = fdk__image_header_budget_ok_file(path);
        if (!fdk_ok(budget)) {
            return budget;
        }
    }

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

/* The memory twin (1.0.0): decodes an in-memory image stream
 * (PNG/JPEG/BMP/... — the clipboard's image/png reads ride this).
 * Same validation ladder as the file variant minus the stat checks;
 * a documented hard cap on input length keeps hostile "images" from
 * claiming unbounded allocations. */
/* §25/G-4 — the any-thread pixel decoder. Same reject-first,
 * size-bounded, 4-channel stb path as fdk_surface_create_from_image,
 * minus the (UI-thread-only) surface object: the result is a plain
 * malloc'd RGBA buffer a worker thread can produce and the UI thread
 * can wrap. Thread-safety by construction: no FDK state is touched
 * (stb_image is thread-safe; the log macro below is the one shared
 * facility and fdk_log is documented any-thread-safe). */
fdk_result fdk_image_decode_pixels(const char *path,
                                   fdk_image_pixels *out_pixels) {
    if (path == NULL || out_pixels == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    out_pixels->pixels = NULL;
    out_pixels->width = 0;
    out_pixels->height = 0;
    out_pixels->stride = 0;

    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return FDK_ERR_NOT_FOUND;
        }
        return FDK_ERR_NOT_A_FILE;
    }
    if (!S_ISREG(st.st_mode)) {
        return FDK_ERR_NOT_A_FILE;
    }
    if (st.st_size <= 0) {
        return FDK_ERR_UNSUPPORTED;
    }
    if ((unsigned long long)st.st_size >
        (unsigned long long)FDK_IMAGE_MAX_FILE_BYTES) {
        FDK_WARN("image file too large to decode (%lld bytes, max %u): %s",
                 (long long)st.st_size, FDK_IMAGE_MAX_FILE_BYTES, path);
        return FDK_ERR_UNSUPPORTED;
    }

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (data == NULL) {
        FDK_WARN("image decode failed: %s: %s", path,
                 stbi_failure_reason() != NULL ? stbi_failure_reason()
                                               : "unknown reason");
        return FDK_ERR_UNSUPPORTED;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        FDK_WARN("image dimensions out of range (%dx%d): %s", w, h, path);
        stbi_image_free(data);
        return FDK_ERR_UNSUPPORTED;
    }

    out_pixels->pixels = data; /* stb's buffer IS the malloc'd RGBA  */
    out_pixels->width = w;
    out_pixels->height = h;
    out_pixels->stride = (size_t)w * 4;
    return FDK_OK;
}

void fdk_image_pixels_free(fdk_image_pixels *pixels) {
    if (pixels == NULL) {
        return;
    }
    /* stbi_image_free is the allocator's pair (free() on the usual
     * platforms, but the pair is the contract) */
    if (pixels->pixels != NULL) {
        stbi_image_free(pixels->pixels);
    }
    pixels->pixels = NULL;
    pixels->width = 0;
    pixels->height = 0;
    pixels->stride = 0;
}

fdk_result fdk_surface_create_from_image_bytes(const void *data,
                                               size_t len,
                                               fdk_surface **out_surface) {
    if (data == NULL || out_surface == NULL || len == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (len > FDK_IMAGE_MAX_FILE_BYTES) {
        FDK_WARN("image buffer too large to decode (%zu bytes, max %u)",
                 len, FDK_IMAGE_MAX_FILE_BYTES);
        return FDK_ERR_UNSUPPORTED;
    }
    {
        fdk_result budget = fdk__image_header_budget_ok(data, (int)len);
        if (!fdk_ok(budget)) {
            return budget;
        }
    }
    int w = 0, h = 0, channels = 0;
    unsigned char *px = stbi_load_from_memory(
        (const stbi_uc *)data, (int)len, &w, &h, &channels, 4);
    if (px == NULL) {
        FDK_WARN("image decode failed: %s",
                 stbi_failure_reason() != NULL ? stbi_failure_reason()
                                               : "unknown reason");
        return FDK_ERR_UNSUPPORTED;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        FDK_WARN("image dimensions out of range (%dx%d)", w, h);
        stbi_image_free(px);
        return FDK_ERR_UNSUPPORTED;
    }
    fdk_surface *surface = NULL;
    fdk_result r = fdk_surface_create_format(w, h,
                                             FDK_SURFACE_FORMAT_ARGB8888,
                                             &surface);
    if (!fdk_ok(r)) {
        stbi_image_free(px);
        return r;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = px + (size_t)y * (size_t)w * 4u;
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
    stbi_image_free(px);
    FDK_DEBUG("image decoded from %zu bytes (%dx%d, %d channels)", len,
              w, h, channels);
    *out_surface = surface;
    return FDK_OK;
}
