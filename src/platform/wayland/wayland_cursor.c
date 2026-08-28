#define _GNU_SOURCE /* memfd_create under -std=c17 */

#define FDK_LOG_TAG "wayland"

/* Cursor shaping on Wayland (1.1.6).
 *
 * Wayland clients own their cursor PIXELS: after wl_pointer::enter the
 * client must answer with wl_pointer.set_cursor(serial, surface, hx,
 * hy), where surface is an ordinary wl_surface carrying the image as
 * a wl_shm buffer. FDK loads those images from the system's XCursor
 * theme files by hand:
 *
 *   - No libxcursor dependency — the same distro-agnostic call the
 *     X11 backend made with cursor-FONT glyphs instead of themes.
 *     The XCursor container format is small, stable and documented
 *     (libxcursor's xcursor.spec): see xcursor_parse() below.
 *   - Search order mirrors libxcursor: $XCURSOR_PATH (colon list) if
 *     set, else ~/.icons, /usr/share/icons, /usr/share/pixmaps; each
 *     searched for <theme>/cursors/<name>, walking the theme's
 *     index.theme Inherit chain, finally the built-in "default"
 *     theme. fopen follows the symlinks themes use heavily
 *     ("default -> left_ptr"), so no readlink pass is needed.
 *   - Honest degradation: a machine with NO cursor theme anywhere
 *     keeps the compositor's own default arrow (pre-1.1.6 behavior),
 *     with one DEBUG line per miss — never a wrong shape, never a
 *     hidden cursor.
 *
 * The window layer drives this through the optional window_set_cursor
 * op (see window.c's compass cache — the op only sees transitions):
 * edge 0 is "default" (left_ptr), 1..8 the resize compass. */

#include "platform/wayland/wayland_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* ---- XCursor container format ------------------------------------
 *
 * Every value is a LITTLE-ENDIAN 32-bit CARD32 (verified byte-by-byte
 * against Debian's Adwaita theme files; the first four bytes are
 * literally ASCII "Xcur" — read LE that is 0x72756358).
 *
 *   file header (16 bytes): magic "Xcur", header=16, version, ntoc
 *     (version is NOT stable across generators — Adwaita files carry
 *     65536 where xcursorgen writes 1 — so it is not validated)
 *   TOC entry (12 bytes) x ntoc: type, subtype, position
 *
 * The image chunk (type 0xfffd0002, subtype = nominal size) begins
 * at its TOC position:
 *
 *   header, type, subtype, version, width, height, xhot, yhot, delay
 *   [any further fields generators may add — skipped via header]
 *   width*height CARD32 ARGB pixels (a<<24 | r<<16 | g<<8 | b)
 *
 * `header` (36 in every file in the wild = 9 CARD32s) counts the
 * whole chunk header, and pixels follow at exactly chunk+header —
 * proven contiguous by Adwaita's own TOC (88 + 36 + 24*24*4 == the
 * next chunk's recorded position). */
#define XCURSOR_IMAGE_TYPE  0xfffd0002u
#define XCURSOR_MAX_FILE    (4u * 1024u * 1024u)  /* sane bound      */
#define XCURSOR_MAX_DIM     512u                 /* real: <= 128    */

/* Cursor names for the compass — the legacy core names every theme
 * ships (index 0 = the default arrow). Themes that only implement
 * the modern naming still symlink these. */
static const char *const g_cursor_names[9] = {
    "left_ptr",             /* 0 = default                       */
    "top_side",             /* N  (FDK_WRES_N)                   */
    "top_right_corner",     /* NE                                */
    "right_side",           /* E                                 */
    "bottom_right_corner",  /* SE                                */
    "bottom_side",          /* S                                 */
    "bottom_left_corner",   /* SW                                */
    "left_side",            /* W                                 */
    "top_left_corner",      /* NW                                */
};

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Parses an in-memory XCursor file, returning the ARGB image whose
 * nominal size is closest to `nominal` (preferring >=). The pixels
 * array is fdk_alloc'd; the caller owns it. */
static fdk_result xcursor_parse(const unsigned char *data, size_t len,
                                int nominal,
                                uint32_t **out_pixels, int *out_w, int *out_h,
                                int *out_hx, int *out_hy) {
    /* Magic: the four bytes ASCII "Xcur" (byte-order independent
     * check — see the format block above for why the constant is
     * read LE in practice). */
    if (len < 16 || data[0] != 0x58 || data[1] != 0x63 ||
        data[2] != 0x75 || data[3] != 0x72 || rd_u32(data + 4) != 16) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    uint32_t ntoc = rd_u32(data + 12);
    if (ntoc == 0 || ntoc > 4096 || 16u + ntoc * 12u > len) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Best image chunk: minimal |subtype - nominal|, ties prefer the
     * LARGER subtype (crisper on HiDPI than the smaller neighbor). */
    uint32_t best_pos = 0;
    int best_delta = 1 << 30;
    int found = 0;
    for (uint32_t t = 0; t < ntoc; t++) {
        const unsigned char *toc = data + 16 + (size_t)t * 12u;
        uint32_t type = rd_u32(toc);
        uint32_t subtype = rd_u32(toc + 4);
        uint32_t pos = rd_u32(toc + 8);
        if (type != XCURSOR_IMAGE_TYPE || pos >= len || pos < 16) {
            continue;
        }
        int delta = (int)subtype > nominal ? (int)subtype - nominal
                                           : nominal - (int)subtype;
        if (!found || delta < best_delta) {
            found = 1;
            best_delta = delta;
            best_pos = pos;
        }
    }
    if (!found) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    const unsigned char *chunk = data + best_pos;
    if ((size_t)(len - best_pos) < 36u) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    uint32_t header = rd_u32(chunk);
    uint32_t type = rd_u32(chunk + 4);
    uint32_t width = rd_u32(chunk + 16);
    uint32_t height = rd_u32(chunk + 20);
    uint32_t xhot = rd_u32(chunk + 24);
    uint32_t yhot = rd_u32(chunk + 28);
    if (type != XCURSOR_IMAGE_TYPE || header < 36 ||
        width == 0 || height == 0 ||
        width > XCURSOR_MAX_DIM || height > XCURSOR_MAX_DIM ||
        xhot >= width || yhot >= height ||
        (uint64_t)header > (uint64_t)(len - best_pos)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const unsigned char *px = chunk + header;
    uint64_t count = (uint64_t)width * (uint64_t)height;
    if ((uint64_t)(len - best_pos - header) < count * 4u) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    uint32_t *pixels = fdk_alloc((size_t)count * 4u);
    if (pixels == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    /* XCursor stores ARGB CARD32s; wl_shm ARGB8888 is what libxcursor's
     * own Wayland backend uploads these bytes into verbatim, so a
     * straight copy renders identically to every other Wayland client. */
    for (uint64_t i = 0; i < count; i++) {
        pixels[i] = rd_u32(px + (size_t)i * 4u);
    }

    *out_pixels = pixels;
    *out_w = (int)width;
    *out_h = (int)height;
    *out_hx = (int)xhot;
    *out_hy = (int)yhot;
    return FDK_OK;
}

/* ---- theme resolution -------------------------------------------- */

/* Max hops through Inherit chains — real chains are 1-2 deep; 8 is
 * generous while still bounding loops. */
#define THEME_MAX_HOPS 8

/* Reads <dir>/<theme>/index.theme and appends the FIRST Inherit=
 * list's entries to `names` (up to the cap). index.theme is INI-ish;
 * Inherit lines look like "Inherit=Foo,Bar". Best-effort: missing
 * file or no such line leaves the list untouched. */
static void theme_inherits(const char *dir, const char *theme,
                           char names[][128], int *count, int cap) {
    char path[1024];
    if ((size_t)snprintf(path, sizeof path, "%s/%s/index.theme",
                         dir, theme) >= sizeof path) {
        return;
    }
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "Inherits=", 9) != 0) {
            continue;
        }
        char *p = line + 9;
        while (*p != '\0' && *count < cap) {
            char *comma = strchr(p, ',');
            size_t n = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
            /* trim trailing whitespace/newline */
            while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' ||
                             p[n - 1] == ' ' || p[n - 1] == '\t')) {
                n--;
            }
            if (n > 0 && n < 128) {
                memcpy(names[*count], p, n);
                names[*count][n] = '\0';
                (*count)++;
            }
            if (comma == NULL) {
                break;
            }
            p = comma + 1;
        }
        break; /* first Inherit= only, like libxcursor */
    }
    fclose(f);
}

/* Finds and reads the cursor file for `name` from `theme_start`,
 * walking the Inherit chain and finally the built-in "default"
 * theme. Returns a malloc'd buffer + length, or NULL. */
static unsigned char *find_xcursor_file(const char *theme_start,
                                        const char *name, size_t *out_len) {
    /* Search roots: $XCURSOR_PATH (colon-separated) else the
     * libxcursor defaults. */
    char roots[6][1024];
    int root_count = 0;
    const char *xcp = getenv("XCURSOR_PATH");
    if (xcp != NULL && xcp[0] != '\0') {
        const char *p = xcp;
        while (*p != '\0' && root_count < 4) {
            const char *colon = strchr(p, ':');
            size_t n = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
            if (n > 0 && n < 1024) {
                memcpy(roots[root_count], p, n);
                roots[root_count][n] = '\0';
                root_count++;
            }
            if (colon == NULL) {
                break;
            }
            p = colon + 1;
        }
    } else {
        const char *home = getenv("HOME");
        if (home != NULL) {
            (void)snprintf(roots[root_count], 1024, "%s/.icons", home);
            root_count++;
        }
        (void)snprintf(roots[root_count++], 1024, "/usr/share/icons");
        (void)snprintf(roots[root_count++], 1024, "/usr/share/pixmaps");
    }

    /* The walk list: the start theme, its inherits (BFS, deduped,
     * capped), then "default" as the universal fallback (pushed
     * once — it is often ALSO the start theme when XCURSOR_THEME is
     * unset). */
    char queue[THEME_MAX_HOPS + 2][128];
    int qcount = 0;
    if (theme_start != NULL && theme_start[0] != '\0' &&
        strcmp(theme_start, "default") != 0 &&
        strlen(theme_start) < 128) {
        (void)snprintf(queue[qcount++], 128, "%s", theme_start);
    }
    (void)snprintf(queue[qcount++], 128, "default");

    for (int qi = 0; qi < qcount && qi < THEME_MAX_HOPS; qi++) {
        const char *theme = queue[qi];
        for (int r = 0; r < root_count; r++) {
            char path[1024];
            if ((size_t)snprintf(path, sizeof path, "%s/%s/cursors/%s",
                                 roots[r], theme, name) >= sizeof path) {
                continue;
            }
            FILE *f = fopen(path, "rb");
            if (f == NULL) {
                continue;
            }
            unsigned char *buf = fdk_alloc(XCURSOR_MAX_FILE);
            size_t got = 0;
            if (buf != NULL) {
                got = fread(buf, 1, XCURSOR_MAX_FILE, f);
            }
            fclose(f);
            if (buf != NULL && got > 0 && got < XCURSOR_MAX_FILE) {
                *out_len = got;
                return buf;
            }
            fdk_free(buf);
            /* A theme with this name but not this cursor: keep
             * searching other roots + inherited themes. */
        }
        /* Queue the inherits of EVERY root that has this theme
         * (dedupe by name — chains loop in the wild). */
        char inh[THEME_MAX_HOPS][128];
        int inh_count = 0;
        for (int r = 0; r < root_count; r++) {
            theme_inherits(roots[r], theme, inh, &inh_count,
                           THEME_MAX_HOPS);
        }
        for (int i = 0; i < inh_count && qcount < THEME_MAX_HOPS + 2;
             i++) {
            int dup = 0;
            for (int j = 0; j < qcount; j++) {
                if (strcmp(queue[j], inh[i]) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (!dup && inh[i][0] != '\0' && strlen(inh[i]) < 128) {
                (void)snprintf(queue[qcount++], 128, "%s", inh[i]);
            }
        }
    }
    return NULL;
}

/* ---- upload + cache ---------------------------------------------- */

/* Uploads ARGB pixels to a wl_shm ARGB8888 buffer (the render path's
 * XRGB twin — see create_shm_buffer in wayland_window.c). Left on the
 * DEFAULT event queue: cursor buffers carry no listener, so release
 * events (compositors may send them) are dispatched and dropped by
 * the main loop instead of piling up on the release queue. */
static struct wl_buffer *cursor_upload(fdk_platform_connection *conn,
                                       const uint32_t *pixels, int w, int h) {
    size_t stride = (size_t)w * 4u;
    size_t length = stride * (size_t)h;
    int fd = memfd_create("fdk-cursor", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, (off_t)length) < 0) {
        close(fd);
        return NULL;
    }
    void *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    memcpy(map, pixels, length);

    struct wl_shm_pool *pool =
        wl_shm_create_pool(conn->shm, fd, (int32_t)length);
    close(fd);
    if (pool == NULL) {
        munmap(map, length);
        return NULL;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    munmap(map, length); /* the server keeps its own mapping */
    return buffer;
}

/* Cache lookup + fill: returns the INDEX into conn->cursor_cache for
 * `name`, loading and uploading it on miss; -1 when no theme
 * provides the shape (honest degradation upstream). */
static int cursor_shape_get(fdk_platform_connection *conn, const char *name,
                            int nominal) {
    /* hit? (same shape at a nearby scale reuses the entry: the pick
     * is nominal-size-closest, so equal names collide first) */
    for (size_t i = 0; i < conn->cursor_cache_count; i++) {
        if (strcmp(conn->cursor_cache[i].name, name) == 0) {
            return (int)i;
        }
    }

    const char *theme = getenv("XCURSOR_THEME");
    if (theme == NULL || theme[0] == '\0') {
        theme = "default";
    }
    size_t len = 0;
    unsigned char *file = find_xcursor_file(theme, name, &len);
    if (file == NULL) {
        FDK_DEBUG("cursor shape '%s' not found in theme '%s' — keeping "
                  "the compositor default", name, theme);
        return -1;
    }
    uint32_t *pixels = NULL;
    int w = 0, h = 0, hx = 0, hy = 0;
    fdk_result r = xcursor_parse(file, len, nominal, &pixels, &w, &h,
                                 &hx, &hy);
    fdk_free(file);
    if (!fdk_ok(r)) {
        FDK_DEBUG("cursor file for '%s' failed to parse (%s)", name,
                  fdk_result_to_string(r));
        return -1;
    }
    struct wl_buffer *buffer = cursor_upload(conn, pixels, w, h);
    fdk_free(pixels);
    if (buffer == NULL) {
        return -1;
    }

    /* grow the cache */
    if (conn->cursor_cache_count == conn->cursor_cache_capacity) {
        size_t cap = conn->cursor_cache_capacity == 0
                         ? 8
                         : conn->cursor_cache_capacity * 2;
        void *grown = fdk_realloc(conn->cursor_cache,
                                  cap * sizeof *conn->cursor_cache);
        if (grown == NULL) {
            wl_buffer_destroy(buffer);
            return -1;
        }
        conn->cursor_cache = grown;
        conn->cursor_cache_capacity = cap;
    }
    size_t slot = conn->cursor_cache_count++;
    conn->cursor_cache[slot].name = NULL;
    size_t nlen = strlen(name) + 1;
    conn->cursor_cache[slot].name = fdk_alloc(nlen);
    if (conn->cursor_cache[slot].name == NULL) {
        wl_buffer_destroy(buffer);
        conn->cursor_cache_count--;
        return -1;
    }
    memcpy(conn->cursor_cache[slot].name, name, nlen);
    conn->cursor_cache[slot].size = nominal;
    conn->cursor_cache[slot].buffer = buffer;
    conn->cursor_cache[slot].hotspot_x = hx;
    conn->cursor_cache[slot].hotspot_y = hy;
    conn->cursor_theme_state = 1; /* a theme is really in play */
    FDK_DEBUG("cursor shape '%s' loaded from theme '%s' (%dx%d, "
              "hotspot %d,%d)", name, theme, w, h, hx, hy);
    return (int)slot;
}

/* ---- the platform op --------------------------------------------- */

void fdk_wayland_window_set_cursor(fdk_platform_window *pwindow, int edge) {
    if (pwindow == NULL || edge < 0 || edge > 8) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->pointer == NULL || conn->last_input_serial == 0) {
        return; /* no pointer yet: nothing to shape, honestly */
    }

    /* Nominal cursor size follows the window's scale (24px at 1x is
     * the desktop default; the theme's closest size is picked). */
    int nominal = (24 * pwindow->scale_x120 + 119) / 120;
    if (nominal < 12) {
        nominal = 12;
    }
    int shape = cursor_shape_get(conn, g_cursor_names[edge], nominal);
    if (shape < 0) {
        return; /* compositor default arrow stays */
    }

    if (conn->cursor_surface == NULL) {
        conn->cursor_surface = wl_compositor_create_surface(conn->compositor);
        if (conn->cursor_surface == NULL) {
            return;
        }
    }

    /* Commit the image, then point the pointer at it. The serial is
     * the latest input serial (the enter serial while hovering) —
     * exactly what the protocol wants set_cursor to cite. */
    wl_surface_attach(conn->cursor_surface, conn->cursor_cache[shape].buffer,
                      0, 0);
    wl_surface_damage(conn->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(conn->cursor_surface);
    wl_pointer_set_cursor(conn->pointer, conn->last_input_serial,
                          conn->cursor_surface,
                          conn->cursor_cache[shape].hotspot_x,
                          conn->cursor_cache[shape].hotspot_y);
    (void)wl_display_flush(conn->display);
}

void fdk_wayland_cursor_teardown(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    for (size_t i = 0; i < conn->cursor_cache_count; i++) {
        if (conn->cursor_cache[i].buffer != NULL) {
            wl_buffer_destroy(conn->cursor_cache[i].buffer);
        }
        fdk_free(conn->cursor_cache[i].name);
    }
    fdk_free(conn->cursor_cache);
    conn->cursor_cache = NULL;
    conn->cursor_cache_count = 0;
    conn->cursor_cache_capacity = 0;
    if (conn->cursor_surface != NULL) {
        wl_surface_destroy(conn->cursor_surface);
        conn->cursor_surface = NULL;
    }
}
