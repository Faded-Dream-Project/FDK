/* gl_viewport_3d.c — 1.0.0 (the 2026-11-18 re-release): an animated,
 * depth-tested 3D scene in a GL viewport inside an ordinary FDK
 * window, with ordinary widgets around it and a menu whose dropdown
 * opens OVER the GL region.
 *
 * The scene is a spinning depth-tested gradient (core-profile-clean:
 * the callback clears color+depth with its own dlopen'd GL — a full
 * mesh lives in the app; the point is the CALLBACK owns the context,
 * the viewport, and the frame pacing). The app asks for a frame from
 * a TIMER (application-driven pacing — FDK forces nothing). A Button
 * toggles the animation.
 *
 * Run: ./build/examples/gl_viewport_3d
 * llvmpipe/Xvfb works for correctness (FDK_GL_FORCE=1).
 */
#include <fdk/fdk.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <dlfcn.h>
#include <assert.h>

/* Minimal dlopen'd GL subset (the documented add-on pattern). */
static void (*g_glXGetProcAddress)(const char *);
static void (*g_glViewport)(int, int, int, int);
static void (*g_glClearColor)(float, float, float, float);
static void (*g_glClear)(unsigned);
static void (*g_glEnable)(unsigned);
enum { GL_DEPTH_TEST = 0x0B71 };
enum { GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100 };
/* POSIX-blessed object->function store (the lane's own idiom): the
 * conditional-call form trips -Wpedantic, so resolve through a typed
 * local loaded into a void* slot. */
static void gl_get(void **slot, void *loader, const char *name,
                   void *lib) {
    /* The dereference-through-void** store: ISO C pedantic forbids
     * direct object->function casts, POSIX blesses this idiom (it is
     * what the lane itself does — see x11_gl.c's FDK_GL_STORE_FN). */
    if (loader != NULL) {
        void *(*gpa)(const char *);
        *(void **)&gpa = loader;
        *slot = gpa(name);
    } else {
        *slot = dlsym(lib, name);
    }
}

#define GET(fn, name) \
    gl_get((void **)&(fn), *(void **)&g_glXGetProcAddress, name, lib)

static void gl_load(void) {
    void *lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (lib == NULL) return;
    *(void **)&g_glXGetProcAddress = dlsym(lib, "glXGetProcAddressARB");
    if (g_glXGetProcAddress == NULL) {
        *(void **)&g_glXGetProcAddress = dlsym(lib, "glXGetProcAddress");
    }
    GET(g_glViewport, "glViewport");
    GET(g_glClearColor, "glClearColor");
    GET(g_glClear, "glClear");
    GET(g_glEnable, "glEnable");
}

static int g_animating = 1;
static float g_angle;
static int g_frames;

static void on_render(fdk_widget *vp, int w, int h, fdk_f32 scale, void *user) {
    (void)vp; (void)scale; (void)user;
    if (g_glViewport == NULL) return;
    g_glViewport(0, 0, w, h);
    float pulse = 0.5f + 0.5f * (float)sin(g_angle);
    g_glClearColor(0.05f + 0.10f * pulse, 0.06f, 0.10f + 0.05f * pulse, 1.0f);
    g_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g_glEnable(GL_DEPTH_TEST);
    g_frames++;
}

static void on_lost(fdk_widget *vp, void *user) {
    (void)vp; (void)user;
    printf("[glv3d] GL context lost — the region falls back\n");
}

static void on_tick(fdk_timer *timer, void *user) {
    (void)timer;
    fdk_widget *vp = user;
    if (g_animating) {
        g_angle += 0.04f;
        fdk_gl_viewport_request_frame(vp);
    }
}

static bool on_toggle(fdk_widget *btn, const fdk_widget_event *ev,
                      void *user) {
    (void)user;
    if (ev != NULL && ev->type == FDK_WIDGET_POINTER_UP) {
        g_animating = !g_animating;
        printf("[glv3d] animation %s (btn=%p)\n",
               g_animating ? "on" : "off", (void *)btn);
    }
    return false;
}

int main(void) {
    gl_load();
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) return 77;
    fdk_window_options o;
    memset(&o, 0, sizeof o);
    o.title = "FDK GL viewport — 3D in a widget tree";
    o.width = 640;
    o.height = 480;
    fdk_window *w = NULL;
    if (!fdk_ok(fdk_window_create(ctx, &o, &w))) {
        fdk_shutdown(ctx);
        return 77;
    }
    /* The app may engage BEFORE the first present — no throwaway
     * present needed anymore (the 2026-11-18 API). */
    fdk_result er = fdk_window_gl_engage(w);
    fdk_gl_status_data st;
    (void)fdk_gl_status(w, &st);
    printf("[glv3d] engage: %d (reason %d)\n", (int)er, (int)st.reason);
    if (fdk_ok(er)) {
        fdk_gl_info gi;
        (void)fdk_window_gl_info(w, &gi);
        printf("[glv3d] GL %s %d.%d — %s / %s\n", gi.flavor,
               gi.version_major, gi.version_minor, gi.vendor,
               gi.renderer);
    }

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(w, &root);
    fdk_widget *col = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &col);
    /* A menu bar: its dropdown opens OVER the GL region below it. */
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(col, NULL, &bar);
    fdk_menu *menu = NULL;
    (void)fdk_menu_create(NULL, &menu);
    fdk_menu_item *it = NULL;
    (void)fdk_menu_append(menu, "Request frame", &it);
    (void)fdk_menu_append_separator(menu);
    (void)fdk_menu_append(menu, "Second entry", &it);
    fdk_widget *toggle = NULL;
    (void)fdk_button_create(col, NULL, "Pause / resume", &toggle);
    fdk_widget_set_event_callback(toggle, on_toggle, NULL);
    fdk_widget *vp = NULL;
    {
        fdk_result r_vp = fdk_gl_viewport_create(col, on_render, NULL,
                                                 &vp);
        assert(fdk_ok(r_vp));
        (void)r_vp;
    }
    fdk_gl_viewport_set_context_lost_callback(vp, on_lost, NULL);

    /* Manual bounds (the widget-zoo pattern): the column is the full
     * window; the bar/button take their natural heights and the GL
     * viewport owns everything below. */
    fdk_widget_arrange(col, (fdk_rect){0, 0, 640, 480});
    fdk_widget_arrange(bar, (fdk_rect){0, 0, 640, 28});
    fdk_widget_arrange(toggle, (fdk_rect){0, 28, 640, 30});
    fdk_widget_arrange(vp, (fdk_rect){0, 58, 640, 422});

    (void)fdk_timer_add(ctx, 16, true, on_tick, vp);

    fdk_window_show(w);
    for (int i = 0; i < 120; i++) {
        fdk_pump_events(ctx, 16);
        (void)fdk_window_paint(w); /* the app drives the frames */
    }
    printf("[glv3d] %d GL frames rendered (animated, on-demand)\n", g_frames);
    fdk_window_destroy(w);
    fdk_shutdown(ctx);
    return 0;
}
