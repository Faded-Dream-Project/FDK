#define FDK_LOG_TAG "x11"

/* x11_cursor.c — the named cursor set on X11 (1.1.0-rc2, S-5 / B-12).
 *
 * The public fdk_cursor_shape set beyond the arrow and the hand needs
 * shapes the core cursor font never had ("copy", "zoom-in",
 * "vertical-text", a real open-hand "grab"). Two tiers, per shape,
 * first hit wins and is cached in the connection:
 *
 *   Tier 1 — the XCursor THEME through RENDER. The shared loader
 *   (src/platform/cursor_theme.c — the same no-libxcursor code the
 *   Wayland backend rides) hands out ARGB pixels + hotspot; an ARGB
 *   depth-32 pixmap + XRenderCreateCursor turns them into a server
 *   Cursor. libXrender.so.1 is dlopen'd (the house pattern — no new
 *   build or CI dependency, absent library = tier 2), the RENDER
 *   extension is probed once, and every request that would talk to a
 *   dead display is skipped (the connection's display_dead state is
 *   honored by the callers).
 *
 *   Tier 2 — the nearest core cursor-font glyph. Every X server since
 *   1987; a few shapes are honestly approximated (the table below
 *   names each substitution). Glyph 0 in the table = the shape keeps
 *   the DEFAULT ARROW (there is no honest core twin at all) — that is
 *   the documented no-lying-fallback posture: better the plain arrow
 *   than a shape that lies about what the press will do.
 *
 * The window-level application (XDefineCursor on the window) lives in
 * x11_window.c's fdk_x11_window_set_cursor; this file is the
 * connection-level CURSOR FACTORY. */

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/cursorfont.h>
#include <X11/Xutil.h> /* XCreateImage/XDestroyImage — the include the
                        * removed Xrender.h used to carry transitively */
#include <dlfcn.h>
#include <string.h>

/* POSIX dlsym returns void*; ISO C (pedantic) forbids the direct
 * object-pointer -> function-pointer assignment, so every store
 * goes through the POSIX-blessed *(void**)& idiom (the GL lane's
 * FDK_GL_STORE_FN rule — the zero-warning build stays intact). */
#define FDK_CURSOR_STORE_FN(dst, sym) (*(void **)&(dst) = (sym))

/* ---- The per-shape tier tables ------------------------------------
 *
 * Indexed by (op - FDK_PLATFORM_CURSOR_NAMED_BASE), i.e. public
 * FDK_CURSOR_TEXT (2) first.
 *
 * named_font_glyphs: the core cursor-font glyph for tier 2, -1 = no
 * core twin at all (COPY, ALIAS, ZOOM_IN, ZOOM_OUT, CONTEXT_MENU —
 * the font never had them). NOTE: XC_X_cursor's VALUE is literally
 * 0, so 0 is a REAL glyph here and -1 is the no-twin sentinel. Every
 * substitution is deliberate and documented; the exact-twin rows are
 * marked.
 *
 * named_theme_first: shapes that try the XCursor THEME through
 * RENDER BEFORE the font — either because the font has no twin at
 * all, or because its twin is a poor one (the font's GRABBING is the
 * POINTING hand; its NOT_ALLOWED is a bare X; its NO_DROP is a
 * skull; TEXT's I-beam differs from what the desktop's theme shows).
 * Shapes with exact twins skip the theme (faster, and pixel-identical
 * to what the desktop already shows). */
static const int named_font_glyphs[FDK_PLATFORM_CURSOR_NAMED_COUNT] = {
    XC_xterm,               /* TEXT — exact                          */
    XC_crosshair,           /* CROSSHAIR — exact                     */
    XC_watch,               /* WAIT — exact                          */
    XC_exchange,            /* PROGRESS — GDK's own core twin        */
    XC_fleur,               /* MOVE — exact (the four-way arrow)     */
    -1,                     /* COPY — no core twin (tier 1 or arrow) */
    -1,                     /* ALIAS — no core twin (tier 1 or arrow)*/
    XC_pirate,              /* NO_DROP — the skull, GDK's core twin  */
    XC_X_cursor,            /* NOT_ALLOWED — the X of refusal (the
                             * glyph VALUE 0 — a real glyph, see the
                             * table comment)                       */
    XC_hand1,               /* GRAB — the open hand                  */
    XC_hand2,               /* GRABBING — closed-hand; the font has
                             * no fist, the pointing hand is the
                             * closest grabbing affordance           */
    XC_fleur,               /* ALL_SCROLL — the pan twin             */
    XC_sb_h_double_arrow,   /* COL_RESIZE — exact                    */
    XC_sb_v_double_arrow,   /* ROW_RESIZE — exact                    */
    -1,                     /* ZOOM_IN — no core twin (tier 1/arrow) */
    -1,                     /* ZOOM_OUT — no core twin (tier 1/arrow)*/
    XC_question_arrow,      /* HELP — exact                          */
    XC_plus,                /* CELL — the spreadsheet plus           */
    -1,                     /* CONTEXT_MENU — no core twin (the font
                             * has no menu glyph; tier 1 or arrow)   */
    XC_xterm,               /* VERTICAL_TEXT — rotated I-beam; the
                             * font has no vertical glyph, the plain
                             * I-beam is the closest text affordance */
};

static const unsigned char named_theme_first[FDK_PLATFORM_CURSOR_NAMED_COUNT] = {
    1, /* TEXT — the theme I-beam matches the desktop           */
    0, /* CROSSHAIR — exact twin                               */
    0, /* WAIT — exact twin                                    */
    0, /* PROGRESS — the exchange twin is the convention        */
    0, /* MOVE — exact twin                                    */
    1, /* COPY — no core twin                                  */
    1, /* ALIAS — no core twin                                 */
    1, /* NO_DROP — the skull is harsh; themes draw the circle  */
    1, /* NOT_ALLOWED — the bare X is harsh                     */
    0, /* GRAB — hand1 is a good twin                          */
    1, /* GRABBING — hand2 is the POINTING hand, a poor twin    */
    0, /* ALL_SCROLL — the pan twin                             */
    0, /* COL_RESIZE — exact twin                               */
    0, /* ROW_RESIZE — exact twin                               */
    1, /* ZOOM_IN — no core twin                                */
    1, /* ZOOM_OUT — no core twin                               */
    0, /* HELP — exact twin                                     */
    0, /* CELL — the plus is the convention                     */
    1, /* CONTEXT_MENU — no core twin                           */
    1, /* VERTICAL_TEXT — no vertical glyph                     */
};

/* ---- Tier 1: the RENDER lane ------------------------------------- */

/* Pins libXrender.so.1 + the extension probe. Sticky: one attempt
 * per connection, `ok` decides every later call. NULL-safe by
 * contract (conn->cursor_render == NULL until first use). */
static struct fdk_x11_cursor_render *render_lane(fdk_platform_connection *conn) {
    if (conn->cursor_render != NULL) {
        return conn->cursor_render;
    }
    struct fdk_x11_cursor_render *lane = fdk_alloc(sizeof *lane);
    if (lane == NULL) {
        return NULL;
    }
    memset(lane, 0, sizeof *lane);
    lane->lib = dlopen("libXrender.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (lane->lib == NULL) {
        FDK_DEBUG("libXrender.so.1 absent (%s) — named cursors fall "
                  "back to the core cursor font", dlerror());
        lane->tried = 1;
        conn->cursor_render = lane;
        return lane; /* sticky off */
    }
    FDK_CURSOR_STORE_FN(lane->query_ext, dlsym(lane->lib,
                                                  "XRenderQueryExtension"));
    FDK_CURSOR_STORE_FN(lane->find_std, dlsym(lane->lib,
                                                "XRenderFindStandardFormat"));
    FDK_CURSOR_STORE_FN(lane->create_pic,
                        dlsym(lane->lib, "XRenderCreatePicture"));
    FDK_CURSOR_STORE_FN(lane->free_pic, dlsym(lane->lib,
                                                "XRenderFreePicture"));
    FDK_CURSOR_STORE_FN(lane->create_cursor,
                        dlsym(lane->lib, "XRenderCreateCursor"));
    lane->tried = 1;
    if (lane->query_ext == NULL || lane->find_std == NULL ||
        lane->create_pic == NULL || lane->free_pic == NULL ||
        lane->create_cursor == NULL) {
        FDK_DEBUG("libXrender.so.1 lacks the cursor entry points — "
                  "named cursors fall back to the core cursor font");
        conn->cursor_render = lane; /* sticky off (ok stays 0) */
        return lane;
    }
    int ev = 0, err = 0;
    if (!lane->query_ext(conn->display, &ev, &err)) {
        FDK_DEBUG("server offers no RENDER extension — named cursors "
                  "fall back to the core cursor font");
        conn->cursor_render = lane; /* sticky off */
        return lane;
    }
    lane->ok = 1;
    conn->cursor_render = lane;
    FDK_DEBUG("RENDER cursor lane armed (libXrender dlopen'd)");
    return lane;
}

/* True when the host's byte order matches the value the pixels are
 * stored in (native u32 ARGB). On the mismatch (a big-endian host
 * against an LSB server or vice versa) the swap below fixes the
 * bytes; kept as data, not #ifdefs, so both paths stay compiled. */
static int host_is_lsb(void) {
    const uint16_t probe = 1;
    unsigned char b0 = 0;
    memcpy(&b0, &probe, 1);
    return b0 != 0;
}

/* Builds one server Cursor from theme pixels through RENDER.
 * None when the lane is off, the theme has no image, or any step
 * fails (tier 2 then takes over — a failed tier 1 is never cached as
 * a failure, it is simply retried never: tier 2's result is cached). */
static Cursor render_theme_cursor(fdk_platform_connection *conn,
                                  const char *const *names) {
    struct fdk_x11_cursor_render *lane = render_lane(conn);
    if (lane == NULL || !lane->ok || conn->display_dead) {
        return None;
    }
    uint32_t *pixels = NULL;
    int w = 0, h = 0, hx = 0, hy = 0;
    if (!fdk_ok(fdk__cursor_theme_load(names, 24, &pixels, &w, &h,
                                       &hx, &hy))) {
        return None;
    }

    /* Native-endian ARGB CARD32s -> a depth-32 ZPixmap XImage ->
     * a pixmap -> a standard-format ARGB32 Picture -> the Cursor.
     * XPutImage converts from the image's byte_order to the server's
     * wire order, so the data only needs to be honest native u32s
     * (byte-swapped once here when host and image order disagree). */
    int lsb_host = host_is_lsb();
    if ((lsb_host && ImageByteOrder(conn->display) != LSBFirst) ||
        (!lsb_host && ImageByteOrder(conn->display) != MSBFirst)) {
        for (int i = 0; i < w * h; i++) {
            uint32_t v = pixels[i];
            pixels[i] = (v << 24) | ((v & 0xff00) << 8) |
                        ((v >> 8) & 0xff00) | (v >> 24);
        }
    }
    XImage *image = XCreateImage(conn->display, NULL, 32, ZPixmap, 0,
                                 (char *)pixels, (unsigned)w, (unsigned)h,
                                 32, w * 4);
    if (image == NULL) {
        fdk_free(pixels);
        return None;
    }
    image->byte_order = ImageByteOrder(conn->display);
    Pixmap pixmap = XCreatePixmap(conn->display, conn->root,
                                  (unsigned)w, (unsigned)h, 32);
    GC gc = XCreateGC(conn->display, pixmap, 0, NULL);
    XPutImage(conn->display, pixmap, gc, image, 0, 0, 0, 0,
              (unsigned)w, (unsigned)h);
    XFreeGC(conn->display, gc);
    /* The XImage owns the pixels buffer through its data pointer
     * when created with XCreateImage — destroy frees it; our own
     * fdk_free would double-free. */
    image->data = NULL;
    XDestroyImage(image);
    fdk_free(pixels);

    void *fmt = lane->find_std(conn->display, FDK_X11_PICT_STANDARD_ARGB32);
    Cursor cursor = None;
    if (fmt != NULL) {
        Picture pic = lane->create_pic(conn->display, pixmap, fmt, 0, NULL);
        if (pic != None) {
            cursor = lane->create_cursor(conn->display, pic,
                                         (unsigned)hx, (unsigned)hy);
            lane->free_pic(conn->display, pic);
        }
    }
    XFreePixmap(conn->display, pixmap);
    if (cursor != None) {
        FDK_DEBUG("named cursor built through RENDER from the XCursor "
                  "theme ('%s')", names[0]);
    }
    return cursor;
}

/* ---- the factory -------------------------------------------------- */

Cursor fdk__x11_named_cursor(fdk_platform_connection *conn, int op_shape) {
    if (conn == NULL || op_shape < FDK_PLATFORM_CURSOR_NAMED_BASE ||
        op_shape >= FDK_PLATFORM_CURSOR_NAMED_BASE +
                        FDK_PLATFORM_CURSOR_NAMED_COUNT) {
        return None;
    }
    int idx = op_shape - FDK_PLATFORM_CURSOR_NAMED_BASE;
    if (conn->named_cursors[idx] != None) {
        return conn->named_cursors[idx]; /* the cache */
    }
    if (conn->display_dead) {
        return None; /* cursors died with the connection */
    }

    /* Tier 1: the theme through RENDER, for the shapes a theme
     * could improve on (named_theme_first — the rest skip straight
     * to tier 2: faster, and pixel-identical to what the desktop
     * already shows). The candidate names are the SHARED table
     * (fdk__cursor_named_names, cursor_theme.c — the same one the
     * Wayland backend rides). */
    Cursor cursor = None;
    if (named_theme_first[idx]) {
        cursor = render_theme_cursor(conn, fdk__cursor_named_names[idx]);
    }

    /* Tier 2: the nearest core cursor-font glyph (-1 = none). */
    if (cursor == None && named_font_glyphs[idx] >= 0) {
        cursor = XCreateFontCursor(conn->display,
                                   (unsigned)named_font_glyphs[idx]);
    }

    if (cursor == None) {
        /* Both tiers honestly failed: cache nothing, keep the default
         * arrow (XDefineCursor(None) IS the parent's cursor). The
         * next request retries tier 1 — cheap, once per miss. */
        FDK_DEBUG("named cursor op=%d has no theme image and no core "
                  "glyph — the default arrow stays", op_shape);
        return None;
    }
    conn->named_cursors[idx] = cursor;
    return cursor;
}

void fdk__x11_named_cursors_shutdown(fdk_platform_connection *conn,
                                     int display_dead) {
    if (conn == NULL) {
        return;
    }
    if (!display_dead) {
        for (int i = 0; i < FDK_PLATFORM_CURSOR_NAMED_COUNT; i++) {
            if (conn->named_cursors[i] != None) {
                XFreeCursor(conn->display, conn->named_cursors[i]);
                conn->named_cursors[i] = None;
            }
        }
    } else {
        /* Server gone: the cursors died with the connection. */
        for (int i = 0; i < FDK_PLATFORM_CURSOR_NAMED_COUNT; i++) {
            conn->named_cursors[i] = None;
        }
    }
    if (conn->cursor_render != NULL) {
        if (conn->cursor_render->lib != NULL) {
            dlclose(conn->cursor_render->lib);
        }
        fdk_free(conn->cursor_render);
        conn->cursor_render = NULL;
    }
}
