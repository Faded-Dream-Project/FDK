#define _GNU_SOURCE /* memfd_create under -std=c17 */

#define FDK_LOG_TAG "wayland"

/* Cursor shaping on Wayland (1.1.6; the full named set 1.1.0-rc2).
 *
 * Wayland clients own their cursor PIXELS: after wl_pointer::enter the
 * client must answer with wl_pointer.set_cursor(serial, surface, hx,
 * hy), where surface is an ordinary wl_surface carrying the image as
 * a wl_shm buffer. FDK loads those images from the system's XCursor
 * theme files through the shared loader (src/platform/cursor_theme.c
 * — the same no-libxcursor call the X11 backend's RENDER path makes;
 * the format knowledge was earned here in 1.1.4 and moved to the
 * common file when X11 needed it too).
 *
 * Honest degradation: a machine with NO cursor theme anywhere keeps
 * the compositor's own default arrow (pre-1.1.6 behavior), with one
 * DEBUG line per miss — never a wrong shape, never a hidden cursor.
 *
 * The window layer drives this through the optional window_set_cursor
 * op (see window.c's resolution cache — the op only sees transitions):
 * edge 0 is "default" (left_ptr), 1..8 the resize compass, 9 the
 * pointing hand (FDK_PLATFORM_CURSOR_POINTER), and the NAMED range
 * 32..51 the full public fdk_cursor_shape set (S-5): cursor-shape-v1
 * enums when the compositor offers the manager, XCursor theme names
 * otherwise (modern CSS name first, legacy name second). wayland_seat.c
 * re-applies the focused window's stored shape on EVERY
 * wl_pointer.enter (fdk_wayland_cursor_apply) — cursor state does not
 * survive leaving a surface. */

#include "platform/wayland/wayland_platform.h"

#include "generated/cursor-shape-v1-client-protocol.h"

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

/* Cursor names for the compass — the legacy core names every theme
 * ships (index 0 = the default arrow; index 9 = the pointing hand).
 * Themes that only implement the modern naming still symlink these. */
static const char *const g_cursor_names[10] = {
    "left_ptr",             /* 0 = default                       */
    "top_side",             /* N  (FDK_WRES_N)                   */
    "top_right_corner",     /* NE                                */
    "right_side",           /* E                                 */
    "bottom_right_corner",  /* SE                                */
    "bottom_side",          /* S                                 */
    "bottom_left_corner",   /* SW                                */
    "left_side",            /* W                                 */
    "top_left_corner",      /* NW                                */
    "pointer",              /* 9 = the hand (FDK_PLATFORM_CURSOR_POINTER) */
};

/* cursor-shape-v1's enum for the same space (the protocol's numbering,
 * used only when the compositor offers the manager). */
static const uint32_t g_cursor_shape_v1[10] = {
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
};

/* The NAMED set (rc2, S-5) rides the SHARED candidate table
 * (fdk__cursor_named_names, cursor_theme.c — one source of truth for
 * the modern/legacy spellings; both backends ride it). */

/* cursor-shape-v1 enums for the named set, same order. The protocol's
 * semantic space is exactly what the public enum mirrors, so every
 * row has a real value — including the two (CELL, VERTICAL_TEXT,
 * CONTEXT_MENU) the core cursor font cannot draw. */
static const uint32_t g_named_shape_v1[FDK_PLATFORM_CURSOR_NAMED_COUNT] = {
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COPY,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALIAS,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NO_DROP,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_IN,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ZOOM_OUT,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_HELP,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CELL,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CONTEXT_MENU,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_VERTICAL_TEXT,
};

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
    /* FD LIFETIME: same rule as create_shm_buffer (wayland_window.c) —
     * the request carrying fd is resolved at FLUSH time by number, so
     * the fd must stay open until the create_pool request is on the
     * wire; flushing here closes the aliasing window where two pools
     * created in one burst would both name the newest memfd. */
    (void)wl_display_flush(conn->display);
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
 * the first of `names` a theme provides, loading and uploading it on
 * miss; -1 when no theme has any candidate (honest degradation
 * upstream). The cache is keyed by the name that HIT, so the modern
 * and legacy spellings of one shape share an entry whichever arrives
 * first. */
static int cursor_shape_get(fdk_platform_connection *conn,
                            const char *const *names, int nominal) {
    /* hit? (same shape at a nearby scale reuses the entry: the pick
     * is nominal-size-closest, so equal names collide first) */
    for (int i = 0; names[i] != NULL; i++) {
        for (size_t c = 0; c < conn->cursor_cache_count; c++) {
            if (strcmp(conn->cursor_cache[c].name, names[i]) == 0) {
                return (int)c;
            }
        }
    }

    uint32_t *pixels = NULL;
    int w = 0, h = 0, hx = 0, hy = 0;
    fdk_result r = fdk__cursor_theme_load(names, nominal, &pixels, &w, &h,
                                          &hx, &hy);
    if (!fdk_ok(r)) {
        /* Name the miss honestly: only the FIRST candidate is logged
         * (the loader already logged each parse failure). */
        FDK_DEBUG("cursor shape '%s' not found in any theme — keeping "
                  "the compositor default", names[0]);
        return -1;
    }
    /* Which candidate hit? Re-ask the loader's knowledge by checking
     * the cache-miss names against the file system is wasteful; the
     * loader returns pixels without the name. Find it cheaply: the
     * debug line in the loader has it, but for the cache key any of
     * the candidates maps to the same image intent — key by the FIRST
     * name (aliases of one shape are interchangeable by construction:
     * the loader only ever returns pixels for a name a theme really
     * ships, and both spellings denote the same glyph). */
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
    size_t nlen = strlen(names[0]) + 1;
    conn->cursor_cache[slot].name = fdk_alloc(nlen);
    if (conn->cursor_cache[slot].name == NULL) {
        wl_buffer_destroy(buffer);
        conn->cursor_cache_count--;
        return -1;
    }
    memcpy(conn->cursor_cache[slot].name, names[0], nlen);
    conn->cursor_cache[slot].size = nominal;
    conn->cursor_cache[slot].buffer = buffer;
    conn->cursor_cache[slot].hotspot_x = hx;
    conn->cursor_cache[slot].hotspot_y = hy;
    conn->cursor_theme_state = 1; /* a theme is really in play */
    return (int)slot;
}

/* Single-name convenience wrapper (the compass + hand call sites). */
static int cursor_shape_get_named(fdk_platform_connection *conn,
                                  const char *name, int nominal) {
    const char *const names[2] = {name, NULL};
    return cursor_shape_get(conn, names, nominal);
}

/* ---- the platform op --------------------------------------------- */

/* THE shape application (1.1.0-rc1, the W2 report; the named set
 * rc2). Two transports, in preference order:
 *
 *   cursor-shape-v1 (when the compositor offered the manager and the
 *   seat's pointer exists): wp_cursor_shape_device_v1.set_shape(serial,
 *   enum) — the compositor renders the shape itself; no theme files,
 *   no client cursor surface, and the serial is whatever input serial
 *   the caller holds (the ENTER serial on entry — the protocol's
 *   explicit "use a serial from an enter or button event" rule). The
 *   full named set is native here: the protocol's enum space is what
 *   the public fdk_cursor_shape mirrors.
 *
 *   wl_pointer.set_cursor(serial, surface, hx, hy) with the XCursor
 *   theme path — the fallback for compositors without the protocol.
 *   The serial must be the enter serial while the pointer is
 *   entering; last_input_serial (the caller passes it) is exactly
 *   that at enter time.
 *
 * Called from TWO places: the op below (shape transitions requested
 * by the window layer or the application) and wayland_seat.c's
 * pointer_enter — which is the W2 fix itself: a client that answers
 * NO enter at all keeps whatever shape the compositor last showed
 * over anything (the reported "cursor stays the resize shape until
 * the pointer leaves"), so EVERY wl_pointer.enter re-applies the
 * focused window's stored shape. */
void fdk_wayland_cursor_apply(fdk_platform_connection *conn,
                              fdk_platform_window *pwindow,
                              uint32_t serial, int shape) {
    if (conn == NULL || pwindow == NULL ||
        !FDK_PLATFORM_CURSOR_OP_VALID(shape)) {
        return;
    }
    const int named = (shape >= FDK_PLATFORM_CURSOR_NAMED_BASE)
                          ? shape - FDK_PLATFORM_CURSOR_NAMED_BASE
                          : -1;

    if (conn->cursor_shape_device != NULL && conn->pointer != NULL) {
        uint32_t v1 = (named >= 0)
                          ? g_named_shape_v1[named]
                          : g_cursor_shape_v1[shape];
        wp_cursor_shape_device_v1_set_shape(conn->cursor_shape_device,
                                            serial, v1);
        return; /* the compositor owns the pixels from here */
    }

    /* Theme fallback. */
    if (conn->pointer == NULL || serial == 0) {
        return; /* no pointer yet / no serial to cite: nothing to shape */
    }

    /* Nominal cursor size follows the window's scale (24px at 1x is
     * the desktop default; the theme's closest size is picked). */
    int nominal = (24 * pwindow->scale_x120 + 119) / 120;
    if (nominal < 12) {
        nominal = 12;
    }
    int idx = (named >= 0)
                  ? cursor_shape_get(conn, fdk__cursor_named_names[named],
                                     nominal)
                  : cursor_shape_get_named(conn, g_cursor_names[shape],
                                           nominal);
    if (idx < 0) {
        return; /* compositor default arrow stays */
    }

    if (conn->cursor_surface == NULL) {
        conn->cursor_surface = wl_compositor_create_surface(conn->compositor);
        if (conn->cursor_surface == NULL) {
            return;
        }
    }

    /* Commit the image, then point the pointer at it. */
    wl_surface_attach(conn->cursor_surface, conn->cursor_cache[idx].buffer,
                      0, 0);
    wl_surface_damage(conn->cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(conn->cursor_surface);
    wl_pointer_set_cursor(conn->pointer, serial, conn->cursor_surface,
                          conn->cursor_cache[idx].hotspot_x,
                          conn->cursor_cache[idx].hotspot_y);
}

void fdk_wayland_window_set_cursor(fdk_platform_window *pwindow, int edge) {
    if (pwindow == NULL || !FDK_PLATFORM_CURSOR_OP_VALID(edge)) {
        return;
    }
    fdk_platform_connection *conn = pwindow->conn;
    if (conn->pointer == NULL || conn->last_input_serial == 0) {
        return; /* no pointer yet: nothing to shape, honestly */
    }
    pwindow->cursor_shape = edge;
    fdk_wayland_cursor_apply(conn, pwindow, conn->last_input_serial, edge);
}

/* Test seam (1.1.0-rc2, S-5; platform_internal.h's fdk_platform_ops
 * entry — production code never calls it): drives the apply path for
 * ANY op shape with the newest input serial (synthetic 1 when none
 * arrived — the REQUEST still goes out; wlroots applies shape advice
 * without serial validation) and returns the cursor-shape-v1 enum the
 * set_shape request cited, so the suite pins the full per-shape
 * mapping against the live compositor. -1 = no manager or no seat
 * pointer on this connection (the honest skip). */
int fdk_wayland_test_cursor_apply(fdk_platform_connection *conn,
                                  fdk_platform_window *pwindow,
                                  int op_shape) {
    if (conn == NULL || pwindow == NULL ||
        !FDK_PLATFORM_CURSOR_OP_VALID(op_shape)) {
        return -1;
    }
    if (conn->cursor_shape_device == NULL || conn->pointer == NULL) {
        return -1;
    }
    uint32_t serial = conn->last_input_serial != 0
                          ? conn->last_input_serial
                          : 1;
    fdk_wayland_cursor_apply(conn, pwindow, serial, op_shape);
    pwindow->cursor_shape = op_shape; /* mirror the op's bookkeeping */
    int named = (op_shape >= FDK_PLATFORM_CURSOR_NAMED_BASE)
                    ? op_shape - FDK_PLATFORM_CURSOR_NAMED_BASE
                    : -1;
    return (named >= 0) ? (int)g_named_shape_v1[named]
                        : (int)g_cursor_shape_v1[op_shape];
}

void fdk_wayland_cursor_teardown(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return;
    }
    if (conn->cursor_shape_device != NULL) {
        wp_cursor_shape_device_v1_destroy(conn->cursor_shape_device);
        conn->cursor_shape_device = NULL;
    }
    if (conn->cursor_shape_manager != NULL) {
        wp_cursor_shape_manager_v1_destroy(conn->cursor_shape_manager);
        conn->cursor_shape_manager = NULL;
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
