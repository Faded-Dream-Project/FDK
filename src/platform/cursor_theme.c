#define _GNU_SOURCE /* nothing yet, kept for symmetry with the users */

#define FDK_LOG_TAG "cursor"

/* cursor_theme.c — the shared XCursor theme loader (1.1.4 Wayland,
 * shared + candidate lists 1.1.0-rc2 S-5).
 *
 * Both backends need cursor images as raw ARGB pixels from the
 * system's XCursor theme files, with no libxcursor dependency: the
 * container format is small, stable and documented (libxcursor's
 * xcursor.spec) — see the format block below. The Wayland backend
 * uploads them to a wl_shm buffer for wl_pointer.set_cursor; the X11
 * backend (rc2) wraps them into RENDER cursors for the shapes the
 * core cursor font lacks. This file knows neither backend — it hands
 * out pixels.
 *
 * The public seam is fdk__cursor_theme_load() (platform_internal.h):
 * a NULL-terminated candidate NAME list tried in order (modern CSS
 * name first, legacy name second — themes ship either), returning
 * FDK_ERR_UNSUPPORTED when no candidate exists anywhere (the
 * honest-degradation signal both backends turn into "keep the
 * platform's own default").
 *
 * Search order mirrors libxcursor: $XCURSOR_PATH (colon list) if set,
 * else ~/.icons, /usr/share/icons, /usr/share/pixmaps; each searched
 * for <theme>/cursors/<name>, walking the theme's index.theme Inherit
 * chain, finally the built-in "default" theme. fopen follows the
 * symlinks themes use heavily ("default -> left_ptr"), so no readlink
 * pass is needed. The theme itself: $XCURSOR_THEME, else "default".
 *
 * This file originated as the static parser inside
 * src/platform/wayland/wayland_cursor.c (1.1.4); the bytes of the
 * format knowledge were earned there against Debian's Adwaita theme
 * files and moved here verbatim when X11 needed them too. */

#include "platform/platform_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
     * straight copy renders identically to every other Wayland client
     * — and the X11 RENDER path (x11_cursor.c) hands the same native
     * CARD32s to a depth-32 ZPixmap XImage. */
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
        if (strncmp(line, "Inherits=", 9) != 0 &&
            strncmp(line, "Inherit=", 8) != 0) {
            continue;
        }
        char *p = line + (strncmp(line, "Inherits=", 9) == 0 ? 9 : 8);
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

/* ---- the named-set candidate table (shared by both backends) -----
 *
 * Row i = public shape (i + 2) = op FDK_PLATFORM_CURSOR_NAMED_BASE + i.
 * Modern CSS-style spelling first, the legacy libxcursor-era name
 * second; themes ship either spelling. Rows are NULL-terminated. */
const char *const
    fdk__cursor_named_names[FDK_PLATFORM_CURSOR_NAMED_COUNT][4] = {
    {"text", "xterm", NULL},                    /* TEXT            */
    {"crosshair", NULL},                        /* CROSSHAIR       */
    {"wait", "watch", NULL},                    /* WAIT            */
    {"progress", "left_ptr_watch", NULL},       /* PROGRESS        */
    {"move", "fleur", NULL},                    /* MOVE            */
    {"copy", NULL},                             /* COPY            */
    {"alias", NULL},                            /* ALIAS           */
    {"no_drop", "no-drop", "crossed_circle"},   /* NO_DROP         */
    {"not_allowed", "not-allowed", "crossed_circle"}, /* NOT_ALLOWED */
    {"grab", "hand1", "openhand"},              /* GRAB            */
    {"grabbing", "hand2", "closedhand"},        /* GRABBING        */
    {"all_scroll", "all-scroll", "fleur"},      /* ALL_SCROLL      */
    {"col_resize", "col-resize", "sb_h_double_arrow"}, /* COL_RESIZE */
    {"row_resize", "row-resize", "sb_v_double_arrow"}, /* ROW_RESIZE */
    {"zoom_in", "zoom-in", NULL},               /* ZOOM_IN         */
    {"zoom_out", "zoom-out", NULL},             /* ZOOM_OUT        */
    {"help", "question_arrow", "left_ptr_help"},/* HELP            */
    {"cell", NULL},                             /* CELL            */
    {"context_menu", "context-menu", "menu"},   /* CONTEXT_MENU    */
    {"vertical_text", "vertical-text", NULL},   /* VERTICAL_TEXT   */
};

/* ---- the seam ----------------------------------------------------- */

fdk_result fdk__cursor_theme_load(const char *const *names, int nominal,
                                  uint32_t **out_pixels, int *out_w,
                                  int *out_h, int *out_hx, int *out_hy) {
    if (names == NULL || names[0] == NULL || out_pixels == NULL ||
        out_w == NULL || out_h == NULL || out_hx == NULL ||
        out_hy == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (nominal < 8) {
        nominal = 8; /* sanity floor — real themes live at 24-48 */
    }
    const char *theme = getenv("XCURSOR_THEME");
    if (theme == NULL || theme[0] == '\0') {
        theme = "default";
    }

    for (int i = 0; names[i] != NULL; i++) {
        size_t len = 0;
        unsigned char *file = find_xcursor_file(theme, names[i], &len);
        if (file == NULL) {
            continue; /* try the next candidate name */
        }
        uint32_t *pixels = NULL;
        int w = 0, h = 0, hx = 0, hy = 0;
        fdk_result r = xcursor_parse(file, len, nominal, &pixels, &w, &h,
                                     &hx, &hy);
        fdk_free(file);
        if (fdk_ok(r)) {
            FDK_DEBUG("cursor theme '%s': '%s' -> %dx%d (hotspot %d,%d)",
                      theme, names[i], w, h, hx, hy);
            *out_pixels = pixels;
            *out_w = w;
            *out_h = h;
            *out_hx = hx;
            *out_hy = hy;
            return FDK_OK;
        }
        FDK_DEBUG("cursor file for '%s' failed to parse (%s) — trying "
                  "the next candidate", names[i],
                  fdk_result_to_string(r));
    }
    return FDK_ERR_UNSUPPORTED;
}
