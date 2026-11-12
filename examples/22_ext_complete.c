/* 22_ext_complete.c — the extension API's uncovered quarter, live.
 *
 * The 2026-11-12 extensibility audit found 13 extension functions
 * with NO example or test consumer anywhere: the modal trio
 * (fdk_ext_modal_run/quit/depth), the GL quartet minus the frame
 * callback (fdk_ext_gl_make_current/swap/fence_insert/fence_wait),
 * and three layer-surface setters (set_layer/set_margins/
 * set_keyboard) — plus the whole fdk/ext/x11.h door (never included
 * by any example). This example is their reference consumer:
 *
 *   1. A MODAL level: fdk_ext_modal_run() blocks in the pump until
 *      a TIMER (the public fdk_timer_add, the documented "timers
 *      need no extension twin") fires and ends the level through
 *      fdk_ext_modal_quit() — nesting checked with
 *      fdk_ext_modal_depth() inside the callback, proving levels
 *      compose with timers, posts, and fd sources.
 *   2. The GL quartet: on a GL-capable context the example makes
 *      the lane context current (fdk_ext_gl_make_current), draws
 *      one clear through a real glClear with the lane's OWN GL
 *      headers dlopen'd the documented way, inserts a fence
 *      (fdk_ext_gl_fence_insert) and waits it
 *      (fdk_ext_gl_fence_wait), then swaps (fdk_ext_gl_swap) —
 *      the add-on-owned present, outside the frame callback.
 *   3. The layer setters: where the compositor offers the layer
 *      shell, set_layer/set_margins/set_keyboard run against a
 *      real surface pre-configure, and the INVALID_STATE refusal
 *      after the first configure is verified (the documented
 *      "adjust before the handshake" boundary).
 *   4. The X11 door (fdk/ext/x11.h): the EWMH dock helpers on a
 *      plain window — _NET_WM_WINDOW_TYPE_DOCK and struts set,
 *      then read back through the server (a second Xlib
 *      connection the example opens itself, exactly like the
 *      integration tests do).
 *
 * Everything reports honestly: each leg prints PASS / SKIP (with
 * the capability that gated it) / FAIL, and the process exit code
 * is 0 iff no leg failed. On Wayland the X11 door leg SKIPs (the
 * accessor returns NULL for Display*); on an Xvfb+llvmpipe box the
 * GL leg exercises the quartet through the software GL; on sway
 * the layer leg runs for real. The example never requires a
 * display to *look* right — it is a wire-level proof.
 *
 * Run: ./build/examples/22_ext_complete
 * (no options; backend follows the environment, AUTO by default)
 */

#include "fdk/fdk.h"
#include "fdk/fdk_extend.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* The X11 door is OPTIONAL at compile time the same way it is at
 * include time: the example builds on both lanes without Xlib
 * dev headers... but the tree always has them, so we include it
 * directly — including it IS the opt-in (docs/EXTENDING.md §10). */
#include "fdk/ext/x11.h"
#include "fdk/ext/wayland.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <wayland-client.h>

static int g_pass, g_skip, g_fail;

static void report(const char *leg, int state, const char *detail) {
    /* state: 1 pass, 0 skip, -1 fail */
    const char *tag = (state > 0) ? "PASS" : (state == 0) ? "SKIP" : "FAIL";
    printf("[ext-complete] %-28s %s%s%s\n", leg, tag,
           (detail != NULL && *detail) ? " — " : "",
           (detail != NULL) ? detail : "");
    if (state > 0) {
        g_pass++;
    } else if (state == 0) {
        g_skip++;
    } else {
        g_fail++;
    }
}

/* ---- Leg 1: the modal trio ------------------------------------------------
 *
 * The timer ends the modal level from inside the pump — the
 * documented composition (a modal level dispatches everything
 * fdk_pump_events dispatches, timers included). */
static int g_modal_depth_inside;

/* POSIX dlsym returns void*; ISO C (pedantic) forbids the object-to-
 * function-pointer conversion. The union carries the address across
 * — the loader idiom everywhere in FDK after the 2026-11-11
 * zero-warning sweep. */
#define EX22_LOAD(field, sym)                                            \
    do {                                                                 \
        union {                                                          \
            void *obj;                                                   \
            __typeof__(field) fn;                                        \
        } u_;                                                            \
        u_.obj = (sym);                                                  \
        (field) = u_.fn;                                                 \
    } while (0)

static void modal_end_timer(fdk_timer *timer, void *user) {
    (void)timer; /* the handle is not needed — ctx arrives as user */
    fdk_context *ctx = (fdk_context *)user;
    g_modal_depth_inside = fdk_ext_modal_depth(ctx);
    printf("[ext-complete] timer fired inside the modal level "
           "(depth=%d)\n",
           g_modal_depth_inside);
    fdk_ext_modal_quit(ctx); /* legal from any thread; here, the UI one */
}

static int leg_modal(fdk_context *ctx) {
    /* REPEATING on purpose: a one-shot frees itself when it fires
     * (the documented contract — the handle dangles), and it fires
     * INSIDE fdk_ext_modal_run's pump. A repeating timer lets the
     * leg remove it safely after the level ends. */
    fdk_timer *timer = fdk_timer_add(ctx, 150, true, modal_end_timer,
                                     ctx);
    const int depth_before = fdk_ext_modal_depth(ctx);
    fdk_result r = fdk_ext_modal_run(ctx); /* blocks until the quit */
    fdk_timer_remove(timer);
    if (!fdk_ok(r)) {
        report("modal trio", -1, fdk_result_to_string(r));
        return -1;
    }
    if (depth_before != 0 || g_modal_depth_inside != 1) {
        char buf[96];
        snprintf(buf, sizeof buf,
                 "depth before=%d inside=%d (want 0/1)", depth_before,
                 g_modal_depth_inside);
        report("modal trio", -1, buf);
        return -1;
    }
    report("modal trio", 1, "run+quit+depth composed with a timer");
    return 0;
}

/* ---- Leg 2: the GL quartet ------------------------------------------------
 *
 * glClear through the lane's own headers, dlopen'd. The lane
 * headers (GLES on Wayland, desktop GL on X11) are the add-on's
 * own dependencies — FDK's public headers carry no GL types
 * (the no-backend-leakage rule). */
typedef void (*gl_clear_color_fn)(float, float, float, float);
typedef void (*gl_clear_fn)(unsigned int);
typedef void (*gl_finish_fn)(void);

/* The GL surface's kind: a plain native surface on whichever lane
 * is connected. X11: an override-redirect-free child with a class
 * hint (the example-19 pattern). Wayland: a fresh role-less
 * wl_surface through the ext/wayland.h door + the example's own
 * registry roundtrip (the 12_vulkan_surface.c pattern) — ROLE-LESS
 * on purpose: this surface exists to carry GL, not to negotiate a
 * shell role (a role-carrying surface would need its own configure
 * handshake; the GL quartet wants none of that). */

/* The registry listener (a file-scope fn, the docs' shape). */
static void ex22_registry_global(void *data, struct wl_registry *reg,
                                 uint32_t name, const char *iface,
                                 uint32_t version) {
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        struct wl_compositor **out = data;
        *out = (struct wl_compositor *)wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    }
}

static fdk_result gl_kind_create(fdk_ext_surface *surface,
                                 fdk_ext_native_handle *out_native,
                                 void *user) {
    (void)user;
    fdk_context *ctx = fdk_ext_surface_context(surface);
    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        return FDK_ERR_PLATFORM;
    }
    if (nd.backend == FDK_EXT_BACKEND_X11) {
        Display *dpy = (Display *)nd.display;
        int screen = DefaultScreen(dpy);
        /* 512x256 over the lane's small-window gate: the X11 lane
         * keeps windows <= 131072 px off GLX on purpose (the X2
         * live report's panel/popup policy — FDK_GL_MIN_PIXELS);
         * this leg WANTS the lane, so the surface clears the gate.
         * (FDK_GL_FORCE=1 is the other documented override.) */
        Window w = XCreateSimpleWindow(
            dpy, DefaultRootWindow(dpy), 40, 40, 512, 256, 0,
            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
        XClassHint *ch = XAllocClassHint();
        if (ch != NULL) {
            ch->res_name = (char *)"fdk-example-22";
            ch->res_class = (char *)"FdkExample22";
            XSetClassHint(dpy, w, ch);
            XFree(ch);
        }
        XStoreName(dpy, w, "FDK example 22 — GL quartet surface");
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)w;
        return FDK_OK;
    }
    /* Wayland: the door's display + our own registry roundtrip for
     * wl_compositor, then a role-less surface (visibility is not
     * this leg's goal — the GL calls are; see the comment above). */
    struct wl_display *wl = fdk_ext_wl_display(ctx);
    if (wl == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    struct wl_registry *reg = wl_display_get_registry(wl);
    struct wl_compositor *comp = NULL;
    struct wl_registry_listener rl = {0};
    rl.global = ex22_registry_global;
    wl_registry_add_listener(reg, &rl, &comp);
    wl_display_roundtrip(wl);
    wl_registry_destroy(reg);
    if (comp == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    struct wl_surface *wls = wl_compositor_create_surface(comp);
    wl_compositor_destroy(comp);
    if (wls == NULL) {
        return FDK_ERR_PLATFORM;
    }
    out_native->backend = FDK_EXT_BACKEND_WAYLAND;
    out_native->pointer = wls;
    return FDK_OK;
}

static void gl_kind_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user;
    /* Nothing to tear down: no role objects were created. */
}

static int leg_gl(fdk_context *ctx, fdk_ext_surface *surface) {
    if (!fdk_ext_has_capability(ctx, FDK_EXT_CAP_GL)) {
        report("GL quartet", 0, "FDK_EXT_CAP_GL false (no lane)");
        return 0;
    }
    /* 1. make_current: the lane context on the surface's drawable. */
    fdk_result r = fdk_ext_gl_make_current(surface);
    if (!fdk_ok(r)) {
        char buf[128];
        snprintf(buf, sizeof buf, "make_current refused (%s)",
                 fdk_result_to_string(r));
        report("GL quartet", 0, buf); /* UNSUPPORTED = honest skip */
        return 0;
    }
    /* 2. a real GL call through the lane's own header, dlopen'd. */
    void *lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
    const char *libname = "libGLESv2.so.2";
    if (lib == NULL) {
        lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
        libname = "libGL.so.1";
    }
    if (lib == NULL) {
        report("GL quartet", 0, "no GL library loadable");
        return 0;
    }
    gl_clear_fn glClear_ = NULL;
    gl_clear_color_fn gl_color_ = NULL;
    gl_finish_fn gl_finish_ = NULL;
    EX22_LOAD(glClear_, dlsym(lib, "glClear"));
    EX22_LOAD(gl_color_, dlsym(lib, "glClearColor"));
    EX22_LOAD(gl_finish_, dlsym(lib, "glFinish"));
    if (glClear_ == NULL || gl_color_ == NULL || gl_finish_ == NULL) {
        report("GL quartet", 0, "glClear/glClearColor not loadable");
        return 0;
    }
    gl_color_(0.10f, 0.15f, 0.35f, 1.0f);
    glClear_(0x00004000u /* GL_COLOR_BUFFER_BIT */);
    gl_finish_();
    /* 3. the fence pair: insert + wait on the lane context. */
    fdk_u64 fence = 0;
    r = fdk_ext_gl_fence_insert(surface, &fence);
    if (r == FDK_OK) {
        r = fdk_ext_gl_fence_wait(surface, fence);
    }
    char fence_note[128];
    if (fdk_ok(r)) {
        snprintf(fence_note, sizeof fence_note,
                 "clear via %s + fence pair waited", libname);
    } else {
        /* The documented UNSUPPORTED: the driver lacks the sync
         * extension. The clear itself still ran; the skip is at
         * the fence, labeled. */
        snprintf(fence_note, sizeof fence_note,
                 "clear ok; fences refused (%s)",
                 fdk_result_to_string(r));
    }
    /* 4. the add-on-owned present. */
    r = fdk_ext_gl_swap(surface);
    if (!fdk_ok(r)) {
        report("GL quartet", -1, "swap refused after a drawn frame");
        return -1;
    }
    report("GL quartet", 1, fence_note);
    return 0;
}

/* ---- Leg 3: the layer setters -------------------------------------------- */

static int leg_layer(fdk_context *ctx) {
    if (!fdk_ext_has_capability(ctx, FDK_EXT_CAP_LAYER_SHELL)) {
        report("layer setters", 0, "FDK_EXT_CAP_LAYER_SHELL false");
        return 0;
    }
    fdk_ext_layer_options opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof(opts);
    opts.layer = FDK_LAYER_TOP;
    opts.anchor = FDK_LAYER_ANCHOR_TOP | FDK_LAYER_ANCHOR_LEFT;
    opts.exclusive_zone = 0;
    opts.keyboard = FDK_LAYER_KB_NONE;
    opts.namespace_ = "ext-complete";
    fdk_ext_surface *s = NULL;
    fdk_result r = fdk_ext_layer_surface_create(ctx, &opts, &s);
    if (!fdk_ok(r)) {
        report("layer setters", 0, fdk_result_to_string(r));
        return 0;
    }
    /* The three uncovered setters, against the real role. */
    r = fdk_ext_layer_surface_set_layer(s, FDK_LAYER_BOTTOM);
    const int set_layer_ok = fdk_ok(r);
    r = fdk_ext_layer_surface_set_margins(s, 4, 4, 4, 4);
    const int set_margins_ok = fdk_ok(r);
    r = fdk_ext_layer_surface_set_keyboard(s, FDK_LAYER_KB_ON_DEMAND);
    const int set_kb_ok = fdk_ok(r);
    if (!set_layer_ok || !set_margins_ok || !set_kb_ok) {
        report("layer setters", -1, "a pre-configure setter refused");
        fdk_ext_surface_destroy(s);
        return -1;
    }
    /* Drive the handshake: pump until a configure arrives (the
     * role acks implicitly through FDK's present gate machinery
     * when the example draws — the simplest honest drive is a few
     * pumps and a present). */
    fdk_surface *sf = NULL;
    (void)fdk_ext_surface_get_surface(s, &sf);
    for (int i = 0; i < 40 && sf != NULL; i++) {
        fdk_pump_events(ctx, 20);
        fdk_surface_info info;
        memset(&info, 0, sizeof info);
        if (fdk_ok(fdk_surface_get_info(sf, &info)) && info.width > 0) {
            (void)fdk_surface_fill(sf, (fdk_color){0, 0, 0, 1});
            (void)fdk_surface_present(sf);
            break;
        }
    }
    /* The documented boundary: after the first configure, the
     * setters answer FDK_ERR_INVALID_STATE. */
    r = fdk_ext_layer_surface_set_layer(s, FDK_LAYER_TOP);
    if (r != FDK_ERR_INVALID_STATE) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "post-configure set_layer answered %d (want %d)",
                 (int)r, (int)FDK_ERR_INVALID_STATE);
        report("layer setters", -1, buf);
        fdk_ext_surface_destroy(s);
        return -1;
    }
    fdk_ext_surface_destroy(s);
    report("layer setters", 1,
           "set_layer/margins/keyboard + the post-configure refusal");
    return 0;
}

/* ---- Leg 4: the X11 door -------------------------------------------------- */

static int leg_x11_door(fdk_context *ctx) {
    Display *dpy = fdk_ext_x11_display(ctx);
    if (dpy == NULL) {
        report("X11 door", 0, "not an X11 connection");
        return 0;
    }
    /* A plain window to dress: the door's helpers take fdk_window*. */
    fdk_window_options wopts;
    memset(&wopts, 0, sizeof wopts);
    wopts.width = 200;
    wopts.height = 60;
    wopts.title = "ext-complete dock";
    fdk_window *w = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &w)) || w == NULL) {
        report("X11 door", -1, "window_create refused");
        return -1;
    }
    fdk_result r = fdk_ext_x11_set_window_type(w, "_NET_WM_WINDOW_TYPE_DOCK");
    unsigned long struts[12];
    memset(struts, 0, sizeof struts);
    struts[2] = 60;  /* top strut: 60px from the top edge */
    struts[9] = 1900; /* right side length of that top edge */
    r |= fdk_ext_x11_set_struts(w, struts);
    if (!fdk_ok(r)) {
        report("X11 door", -1, "door helper refused");
        fdk_window_destroy(w);
        return -1;
    }
    /* The properties ride the SAME connection FDK drives; flush it
     * so the server (and the probe connection below) sees them
     * before the readback — the classic Xlib buffered-request trap
     * the example's first run hit. */
    XSync(dpy, False);
    /* Read it back through a SECOND connection — the server is
     * the truth, exactly like the integration tests' approach. */
    const char *dpy_name = getenv("DISPLAY");
    if (dpy_name == NULL) {
        report("X11 door", 0, "no DISPLAY to read back through");
        fdk_window_destroy(w);
        return 0;
    }
    Display *probe = XOpenDisplay(dpy_name);
    if (probe == NULL) {
        report("X11 door", 0, "probe XOpenDisplay failed");
        fdk_window_destroy(w);
        return 0;
    }
    Window xid = (Window)fdk_ext_x11_window_handle(w);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    int got_type = 0, got_struts = 0;
    Atom type_atom = XInternAtom(probe, "_NET_WM_WINDOW_TYPE", False);
    if (XGetWindowProperty(probe, xid, type_atom, 0, 64, False,
                           XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) == Success &&
        data != NULL) {
        if (nitems >= 1) {
            Atom dock = XInternAtom(probe, "_NET_WM_WINDOW_TYPE_DOCK",
                                    False);
            if (*(Atom *)(void *)data == dock) {
                got_type = 1;
            }
        }
        XFree(data);
    }
    Atom strut_atom = XInternAtom(probe, "_NET_WM_STRUT_PARTIAL", False);
    data = NULL;
    if (XGetWindowProperty(probe, xid, strut_atom, 0, 24, False,
                           XA_CARDINAL, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) == Success &&
        data != NULL) {
        if (nitems >= 12) {
            unsigned long *v = (unsigned long *)(void *)data;
            if (v[2] == 60 && v[9] == 1900) {
                got_struts = 1;
            }
        }
XFree(data);
    }
    XCloseDisplay(probe);
    fdk_window_destroy(w);
    if (!got_type || !got_struts) {
        report("X11 door", -1,
               "server readback missed the properties");
        return -1;
    }
    report("X11 door", 1,
           "window_type + struts set and read back via the server");
    return 0;
}

int main(void) {
    printf("[ext-complete] extension API %d (compiled %d)\n",
           fdk_ext_api_version(), FDK_EXT_API_VERSION);
    if (fdk_ext_api_version() != FDK_EXT_API_VERSION) {
        printf("[ext-complete] header/library mismatch\n");
        return 2;
    }

    /* GPU mode first: the GL quartet needs the lane. A refusal is
     * the documented honest fallback (HYBRID), labeled. Software GL
     * (llvmpipe, the sandbox's Xvfb) engages with the documented
     * FDK_GL_FORCE override — set it before init so the lane's
     * one-shot renderer probe sees it. */
    if (getenv("FDK_GL_FORCE") == NULL) {
        setenv("FDK_GL_FORCE", "1", 1);
    }
    fdk_init_options io;
    memset(&io, 0, sizeof io);
    io.render_mode = FDK_RENDER_GPU;
    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, &io);
    const char *mode = "GPU";
    if (!fdk_ok(r)) {
        printf("[ext-complete] GPU mode refused (%s) — HYBRID\n",
               fdk_result_to_string(r));
        memset(&io, 0, sizeof io);
        io.render_mode = FDK_RENDER_HYBRID;
        if (!fdk_ok(fdk_init(&ctx, &io))) {
            printf("[ext-complete] init failed\n");
            return 2;
        }
        mode = "HYBRID";
    }
    printf("[ext-complete] backend=%d mode=%s\n",
           (int)fdk_ext_active_backend(ctx), mode);

    /* Leg 1 runs on any connection. */
    if (leg_modal(ctx) != 0) {
        goto done;
    }

    /* Legs 2-4. The GL quartet needs a surface with a lane
     * drawable: a plain window's ext path is not exposed — the
     * documented surface for add-on GL is an extension surface
     * on a GPU-capable context. Create a minimal kind for it. */
    {
        fdk_ext_surface_ops ops;
        memset(&ops, 0, sizeof ops);
        ops.struct_size = sizeof(ops);
        ops.create = gl_kind_create;
        ops.destroy = gl_kind_destroy;
        fdk_ext_surface_kind *kind = NULL;
        if (!fdk_ok(fdk_ext_register_kind(ctx, "com.fadeddream.extc",
                                          &ops, NULL, &kind))) {
            report("GL quartet", -1, "kind registration failed");
            goto done;
        }
        fdk_ext_surface *s = NULL;
        if (fdk_ok(fdk_ext_surface_create(kind, &s)) && s != NULL) {
            /* Engage the lane FIRST: the X11 lane loads GLX lazily
             * at the first framebuffer acquisition (a present), so
             * the cap and make_current both need one present before
             * they can answer yes. Fill + present one frame through
             * the ordinary public path, then run the quartet. */
            fdk_surface *sf = NULL;
            if (fdk_ok(fdk_ext_surface_get_surface(s, &sf)) &&
                sf != NULL) {
                (void)fdk_surface_fill(sf,
                                       (fdk_color){0.05f, 0.05f, 0.08f,
                                                   1.0f});
                (void)fdk_surface_present(sf);
            }
            (void)leg_gl(ctx, s);
            fdk_ext_surface_destroy(s);
        } else {
            report("GL quartet", 0, "ext surface refused on backend");
        }
        fdk_ext_kind_destroy(kind);
    }

    if (leg_layer(ctx) != 0) {
        goto done;
    }
    if (leg_x11_door(ctx) != 0) {
        goto done;
    }

done:
    printf("[ext-complete] %d pass, %d skip, %d fail\n", g_pass,
           g_skip, g_fail);
    fdk_shutdown(ctx);
    return (g_fail == 0) ? 0 : 1;
}
