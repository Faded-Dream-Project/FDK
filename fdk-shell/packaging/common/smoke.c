/*
 * smoke.c — fdk-shell binary-package smoke test (public API only).
 *
 * Compiled and run by fdk-shell's packaging install-tests against an
 * INSTALLED fdk-shell + FDK (both found through pkg-config, loaded
 * from the system library paths). It exercises the whole dependency
 * chain this package declares:
 *
 *   - BOTH shared libraries load (libfdk-shell.so.0 + libfdk.so.1)
 *     and their runtime versions agree
 *   - the extension-API version gate (the documented add-on pattern)
 *   - a real display connection through FDK
 *   - the fdk-shell layer handshake: create, show, present a frame
 *   - server-side truth through a second X connection: the layer
 *     window exists, is DOCK-typed, STICKY (_NET_WM_DESKTOP = all
 *     desktops), and carries the strut reservation
 *   - clean teardown in the documented order
 *
 * Usage: fdkshell_smoke [expected-fdk-shell-version]
 * Exit status 0 = pass. Any failure exits nonzero with a message on
 * stderr, naming the stage that failed.
 */

#include <stdio.h>
#include <string.h>

#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"
#include "fdk-shell/fdk-shell.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

int main(int argc, char **argv) {
    const char *expected = argc > 1 ? argv[1] : NULL;

    /* 1. Versions: both libraries must load and agree with the
     * package metadata (catches payload/soname mix-ups). */
    const char *sv = fdk_shell_version();
    printf("SMOKE fdk-shell=%s fdk=%s ext-api=%d\n", sv,
           fdk_get_version_string(), (int)fdk_ext_api_version());
    if (expected && strcmp(sv, expected) != 0) {
        fprintf(stderr, "SMOKE FAIL: fdk-shell version %s != expected %s\n",
                sv, expected);
        return 1;
    }

    /* 2. The extension-API gate (include/fdk-shell/fdk-shell.h's
     * documented add-on pattern, verbatim). */
    if (fdk_get_version() < FDK_VERSION_ENCODE(1, 1, 0) ||
        fdk_ext_api_version() < 1) {
        fprintf(stderr, "SMOKE FAIL: FDK 1.1.0 (extension API v1) required\n");
        return 1;
    }

    /* 3. Real display connection through FDK. */
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        fprintf(stderr, "SMOKE FAIL: fdk_init (no usable display?)\n");
        return 1;
    }

    fdk_shell *shell = NULL;
    fdk_result r = fdk_shell_init(ctx, &shell);
    if (!fdk_ok(r)) {
        fprintf(stderr, "SMOKE FAIL: fdk_shell_init (%d)\n", (int)r);
        fdk_shutdown(ctx);
        return 1;
    }

    /* 4. A top bar layer: create, show, paint, present. */
    fdk_shell_layer_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.layer = FDK_SHELL_LAYER_TOP;
    desc.anchors =
        FDK_SHELL_ANCHOR_TOP | FDK_SHELL_ANCHOR_LEFT | FDK_SHELL_ANCHOR_RIGHT;
    desc.height = 32;
    desc.exclusive_zone = -1;

    fdk_shell_layer *layer = NULL;
    r = fdk_shell_layer_create(shell, &desc, &layer);
    if (!fdk_ok(r)) {
        fprintf(stderr, "SMOKE FAIL: fdk_shell_layer_create (%d)\n", (int)r);
        fdk_shell_shutdown(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_shell_layer_show(layer);

    fdk_surface *surface = NULL;
    int painted = 0;
    for (int i = 0; i < 20 && !painted; i++) {
        (void)fdk_pump_events(ctx, 50);
        if (fdk_ok(fdk_shell_layer_get_surface(layer, &surface))) {
            fdk_surface_info info;
            if (fdk_ok(fdk_surface_get_info(surface, &info)) &&
                info.width > 0) {
                fdk_surface_fill(surface, (fdk_color){ .r = 0.25f,
                                                       .g = 0.55f,
                                                       .b = 0.85f,
                                                       .a = 1.0f });
                if (fdk_ok(fdk_surface_present(surface))) {
                    painted = 1;
                }
            }
        }
    }
    if (!painted) {
        fprintf(stderr, "SMOKE FAIL: the layer never presented a frame\n");
        fdk_shell_layer_destroy(layer);
        fdk_shell_shutdown(shell);
        fdk_shutdown(ctx);
        return 1;
    }
    (void)fdk_pump_events(ctx, 200);

    /* 5. Server-side truth through a second connection — found from
     * the ROOT's window list, using only public API: in this bare Xvfb
     * session our layer is the only DOCK-typed window, and it must
     * carry the sticky desktop hint and the strut reservation. (The
     * full property battery — anchors, margins, ABOVE/SKIP_*, the
     * desktop-switch behavior — lives in the in-tree test suite,
     * which CI runs against the built tree.) */
    {
        Display *dpy = XOpenDisplay(NULL);
        if (dpy == NULL) {
            fprintf(stderr, "SMOKE FAIL: readback connection\n");
            fdk_shell_layer_destroy(layer);
            fdk_shell_shutdown(shell);
            fdk_shutdown(ctx);
            return 1;
        }
        Window root = DefaultRootWindow(dpy);
        Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom dock_atom =
            XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
        Atom desktop_atom = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
        Atom struts_atom = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);

        Window r = None, parent = None;
        Window *kids = NULL;
        unsigned int nk = 0;
        int found_dock = 0, found_sticky = 0, found_strut = 0;
        if (XQueryTree(dpy, root, &r, &parent, &kids, &nk) != 0) {
            for (unsigned int i = 0; i < nk && !found_dock; i++) {
                Atom actual = None;
                int fmt = 0;
                unsigned long n = 0, after = 0;
                unsigned char *data = NULL;
                if (XGetWindowProperty(dpy, kids[i], type_atom, 0, 4, False,
                                       XA_ATOM, &actual, &fmt, &n, &after,
                                       &data) != Success ||
                    data == NULL) {
                    continue;
                }
                int is_dock = (n >= 1 && ((Atom *)data)[0] == dock_atom);
                XFree(data);
                if (!is_dock) {
                    continue;
                }
                found_dock = 1;
                if (XGetWindowProperty(dpy, kids[i], desktop_atom, 0, 1,
                                       False, XA_CARDINAL, &actual, &fmt, &n,
                                       &after, &data) == Success &&
                    data != NULL && n >= 1) {
                    /* Xlib sign-extends 32-bit CARDINALs; the wire value
                     * 0xFFFFFFFF ("all desktops") reads back as -1. */
                    long v = ((long *)data)[0];
                    found_sticky = (v == -1L ||
                                    (unsigned long)v == 0xFFFFFFFFUL);
                    XFree(data);
                }
                if (XGetWindowProperty(dpy, kids[i], struts_atom, 0, 12,
                                       False, XA_CARDINAL, &actual, &fmt, &n,
                                       &after, &data) == Success &&
                    data != NULL && n >= 12) {
                    found_strut = (((long *)data)[2] == 32); /* top strut */
                    XFree(data);
                }
            }
            if (kids != NULL) {
                XFree(kids);
            }
        }
        XCloseDisplay(dpy);
        if (!found_dock || !found_sticky || !found_strut) {
            fprintf(stderr,
                    "SMOKE FAIL: layer window truth (dock=%d sticky=%d "
                    "strut=%d)\n",
                    found_dock, found_sticky, found_strut);
            fdk_shell_layer_destroy(layer);
            fdk_shell_shutdown(shell);
            fdk_shutdown(ctx);
            return 1;
        }
    }

    /* 6. Clean teardown in the documented order. */
    fdk_shell_layer_destroy(layer);
    fdk_shell_shutdown(shell);
    fdk_shutdown(ctx);
    printf("SMOKE OK\n");
    return 0;
}
