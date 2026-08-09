#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h>
#include <string.h>

#define X11_DEFAULT_WIDTH  640
#define X11_DEFAULT_HEIGHT 480
#define X11_DEFAULT_TITLE  "FDK Application"

fdk_result fdk_x11_window_create(fdk_platform_connection *conn,
                                     const fdk_window_options *options,
                                     fdk_platform_window **out_pwindow) {
    fdk_i32 width = X11_DEFAULT_WIDTH;
    fdk_i32 height = X11_DEFAULT_HEIGHT;
    const char *title = X11_DEFAULT_TITLE;

    if (options != NULL) {
        if (options->width > 0)  width = options->width;
        if (options->height > 0) height = options->height;
        if (options->title != NULL) title = options->title;
    }

    unsigned long black = BlackPixel(conn->display, conn->screen);
    unsigned long white = WhitePixel(conn->display, conn->screen);

    Window xwindow = XCreateSimpleWindow(
        conn->display, conn->root,
        0, 0, (unsigned int)width, (unsigned int)height,
        0 /* border width */, black, white);

    if (xwindow == 0) {
        FDK_ERROR("XCreateSimpleWindow failed");
        return FDK_ERR_WINDOW_CREATE;
    }

    /* Select the input events FDK's event model in fdk_event.h can
     * translate. StructureNotifyMask gets us ConfigureNotify (resize).
     * Not selecting VisibilityChangeMask/etc — those don't map to any
     * event fdk_event.h defines yet. */
    XSelectInput(conn->display, xwindow,
                 StructureNotifyMask | FocusChangeMask | KeyPressMask |
                 KeyReleaseMask | PointerMotionMask | ButtonPressMask |
                 ButtonReleaseMask | EnterWindowMask | LeaveWindowMask);

    /* Register for WM_DELETE_WINDOW so close requests arrive as an
     * event we can translate rather than killing the connection. */
    XSetWMProtocols(conn->display, xwindow, &conn->wm_delete_window, 1);

    XStoreName(conn->display, xwindow, title);
    /* Also set _NET_WM_NAME/UTF8_STRING for correct non-ASCII title
     * rendering under EWMH-compliant window managers; XStoreName above
     * is the ICCCM fallback for WMs that don't read _NET_WM_NAME. */
    XChangeProperty(conn->display, xwindow, conn->net_wm_name, conn->utf8_string,
                     8, PropModeReplace,
                     (const unsigned char *)title, (int)strlen(title));

    fdk_platform_window *pwindow = fdk_alloc(sizeof(fdk_platform_window));
    if (pwindow == NULL) {
        XDestroyWindow(conn->display, xwindow);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    pwindow->conn = conn;
    pwindow->xwindow = xwindow;
    pwindow->last_size.width = width;
    pwindow->last_size.height = height;

    fdk_result r = fdk_x11_register_window(conn, pwindow);
    if (!fdk_ok(r)) {
        XDestroyWindow(conn->display, xwindow);
        fdk_free(pwindow);
        return r;
    }

    FDK_DEBUG("window created (0x%lx, %dx%d, \"%s\")", xwindow, width, height, title);

    *out_pwindow = pwindow;
    return FDK_OK;
}

void fdk_x11_window_destroy(fdk_platform_window *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    fdk_x11_unregister_window(pwindow->conn, pwindow);
    XDestroyWindow(pwindow->conn->display, pwindow->xwindow);
    /* XSync (not just XFlush) ensures the destroy request has been
     * sent AND acknowledged by the server before this function
     * returns, rather than leaving it sitting in Xlib's client-side
     * write buffer. Without this, a caller that destroys a window and
     * then immediately calls fdk_shutdown() (which calls
     * fdk_x11_disconnect() -> XCloseDisplay()) can race: XCloseDisplay
     * closing the socket before the destroy request was actually
     * flushed corrupts the teardown from the server's point of view.
     * Reproduced directly against Xvfb (which is more sensitive to
     * this than a full Xorg server tends to be in practice) — see
     * docs/testing.md, "Known Xvfb flakiness", for the investigation
     * that found this. */
    XSync(pwindow->conn->display, False);
    fdk_free(pwindow);
}

void fdk_x11_window_show(fdk_platform_window *pwindow) {
    XMapWindow(pwindow->conn->display, pwindow->xwindow);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_hide(fdk_platform_window *pwindow) {
    XUnmapWindow(pwindow->conn->display, pwindow->xwindow);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_set_title(fdk_platform_window *pwindow, const char *title) {
    if (title == NULL) {
        title = "";
    }
    XStoreName(pwindow->conn->display, pwindow->xwindow, title);
    XChangeProperty(pwindow->conn->display, pwindow->xwindow,
                     pwindow->conn->net_wm_name, pwindow->conn->utf8_string,
                     8, PropModeReplace,
                     (const unsigned char *)title, (int)strlen(title));
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_resize(fdk_platform_window *pwindow, fdk_i32 width, fdk_i32 height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    XResizeWindow(pwindow->conn->display, pwindow->xwindow,
                  (unsigned int)width, (unsigned int)height);
    XFlush(pwindow->conn->display);
}

void fdk_x11_window_set_size_limits(fdk_platform_window *pwindow,
                                        fdk_size min_size, fdk_size max_size) {
    XSizeHints *hints = XAllocSizeHints();
    if (hints == NULL) {
        FDK_ERROR("XAllocSizeHints failed");
        return;
    }

    hints->flags = 0;
    if (min_size.width > 0 || min_size.height > 0) {
        hints->flags |= PMinSize;
        hints->min_width = min_size.width > 0 ? min_size.width : 1;
        hints->min_height = min_size.height > 0 ? min_size.height : 1;
    }
    if (max_size.width > 0 || max_size.height > 0) {
        hints->flags |= PMaxSize;
        /* 0 in only one dimension of max_size means "no max in this
         * dimension"; approximate "unbounded" with a very large value
         * since XSizeHints has no explicit "no limit" sentinel once
         * PMaxSize is set at all. */
        hints->max_width = max_size.width > 0 ? max_size.width : 100000;
        hints->max_height = max_size.height > 0 ? max_size.height : 100000;
    }

    XSetWMNormalHints(pwindow->conn->display, pwindow->xwindow, hints);
    XFree(hints);
    XFlush(pwindow->conn->display);
}
