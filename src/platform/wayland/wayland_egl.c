#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "platform/wayland/wayland_egl.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GPU presentation lane on Wayland (1.2.0 / D-1) — implementation.
 *
 * This file mirrors x11_gl.c's structure decision for decision: the
 * same hybrid model (CPU rasterization + GPU composite), the same
 * dlopen posture (zero new build dependencies), the same renderer
 * policy (hardware yes, llvmpipe/softpipe/swrast no, FDK_GL_FORCE
 * overrides, FDK_NO_GL opts out), the same failure contract (any
 * error downgrades the window permanently to wl_shm, never a broken
 * frame), and the same byte-exactness argument (a BGRA/UNSIGNED_BYTE
 * texture NEAREST-sampled 1:1 whose bytes ARE the staging buffer's
 * little-endian XRGB8888 layout — with the X byte EXCLUDED from the
 * contract: the quad shader pins alpha 1.0 and the config prefers an
 * alpha-less swap buffer, so the compositor sees exactly the opaque
 * pixel the wl_shm path's XRGB8888 buffer would show it; see the
 * QUAD_FS note for why passthrough alpha would be a transparent-
 * window bug).
 *
 * The platform differences, called out where they appear:
 *
 *   - EGL, not GLX: eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)
 *     over the connection's own wl_display (needs libEGL.so.1), and
 *     eglCreateWindowSurface over a wl_egl_window (needs
 *     libwayland-egl.so.1) — TWO dlopen'd libraries, both pinned
 *     for the process (the GLX pinning note applies to both: Mesa
 *     registers process-global driver state that does not survive
 *     unload cleanly).
 *
 *   - GLES2, not desktop GL: EGL's Wayland platform serves GLES
 *     contexts (desktop GL on Wayland is EGL_OPENGL_API + usually
 *     unavailable without extra plumbing; every real Wayland GPU
 *     client — GTK4, Qt, Chromium — renders GLES). The fixed-
 *     function quad becomes a small GLES2 shader pair; the API
 *     subset (glCreateShader/glShaderSource/..., glTexImage2D,
 *     glTexSubImage2D, glReadPixels, eglSwapBuffers) has been
 *     wire-stable since GLES2 shipped (2007-ish). Resolved through
 *     eglGetProcAddress exclusively (THE documented loader entry
 *     point for EGL — unlike GLX, core GLES symbols are NOT
 *     guaranteed to be exported from libEGL.so.1 itself under
 *     GLVND).
 *
 *   - No XGetImage self-check: reading back what the COMPOSITOR
 *     latched would need wlr-screencopy or similar — a third
 *     protocol, compositor cooperation, and page-flip timing
 *     races. Instead the first completed present verifies through
 *     glReadPixels (the GL back buffer BEFORE the swap — legal and
 *     synchronous in GLES2 for the default framebuffer). A driver
 *     whose composite is broken would still pass this check, but
 *     the Wayland compositor itself composits the EGL buffer with
 *     its own tested path (the swap hands it a dma-buf), so the
 *     failure mode GLX's server-side check guarded against — a
 *     broken driver-to-Xwindow blit — does not exist here: there
 *     IS no client-visible "window content" to blit; the buffer
 *     goes compositor-side at swap. The check we CAN make (the
 *     texture/quad rendered the right pixels) still catches upload
 *     and projection mistakes, which is what it is for.
 *
 *   - Resizes: wl_egl_window_resize before the texture reallocation
 *     (the EGL surface reallocates its back buffer at the next
 *     swap); the staging buffer reallocates at the new size with
 *     full damage, exactly the GLX lane's model.
 *
 * Library pinning: both libraries are dlopen'd once per process and
 * never dlclose'd — the GLX lane's note, verbatim in spirit.
 */

/* ---- hand-declared EGL/GLES2 subset (no EGL headers in this build) ---- */

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef unsigned int EGLBoolean;
typedef int EGLint;
typedef int32_t EGLint32_t_unused;
#define EGLC_NO_DISPLAY ((EGLDisplay)0)
#define EGLC_NO_CONTEXT ((EGLContext)0)
#define EGLC_NO_SURFACE ((EGLSurface)0)

/* EGL constants (values from the EGL 1.5 specification). */
enum {
    EGLC_SURFACE_TYPE = 0x3033,
    EGLC_WINDOW_BIT = 0x0004,
    EGLC_RENDERABLE_TYPE = 0x3040,
    EGLC_OPENGL_ES2_BIT = 0x0004,
    EGLC_RED_SIZE = 0x3024,
    EGLC_GREEN_SIZE = 0x3023,
    EGLC_BLUE_SIZE = 0x3022,
    EGLC_ALPHA_SIZE = 0x3021,
    EGLC_NONE = 0x3038,
    EGLC_CONTEXT_CLIENT_VERSION = 0x3098,
    EGLC_VENDOR = 0x3053,
    EGLC_RENDERER = 0x3054,
    EGLC_WIDTH = 0x3056,
    EGLC_HEIGHT = 0x3057,
    EGLC_SUCCESS = 0x3000,
    EGLC_PLATFORM_WAYLAND_KHR = 0x31D8
};

/* GL/GLES constants (values from the GLES2 specification). */
enum {
    GLC_TEXTURE_2D = 0x0DE1,
    GLC_RGBA = 0x1908,
    GLC_BGRA = 0x80E1,
    GLC_UNSIGNED_BYTE = 0x1401,
    GLC_NEAREST = 0x2600,
    GLC_CLAMP_TO_EDGE = 0x812F,
    GLC_TEXTURE_WRAP_S = 0x2802,
    GLC_TEXTURE_WRAP_T = 0x2803,
    GLC_TEXTURE_MIN_FILTER = 0x2801,
    GLC_TEXTURE_MAG_FILTER = 0x2800,
    GLC_UNPACK_ROW_LENGTH = 0x0CF2,
    GLC_UNPACK_ALIGNMENT = 0x0CF5,
    GLC_VENDOR = 0x1F00,
    GLC_RENDERER = 0x1F01,
    GLC_TEXTURE0 = 0x84C0,
    GLC_COMPILE_STATUS = 0x8B81,
    GLC_LINK_STATUS = 0x8B82,
    GLC_VERTEX_SHADER = 0x8B31,
    GLC_FRAGMENT_SHADER = 0x8B30,
    GLC_FLOAT = 0x1406,
    GLC_ARRAY_BUFFER = 0x8892,
    GLC_STATIC_DRAW = 0x88E4,
    GLC_TRIANGLES = 0x0004,
    GLC_COLOR_BUFFER_BIT = 0x4000
};

/* The quad program: a_pos carries NORMALIZED [0,1]² coordinates (the
 * VBO is size-independent; w/h ride the u_size uniform for the
 * draw-time viewport reference), Y flipped — the staging buffer's
 * row 0 is the TOP row — and texcoords 1:1. The ndc math is the
 * standard fullscreen-quad form: a_pos * 2 - 1 spans -1..+1 across
 * the WHOLE viewport; dividing pixel coordinates by u_size here
 * would shrink the quad to a single texel. */
static const char *QUAD_VS =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "uniform vec2 u_size;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x * 2.0 - 1.0,\n"
    "                  1.0 - a_pos.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_pos;\n"
    "}\n";
static const char *QUAD_FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0);\n"
    "}\n";

/* The alpha decision above is CORRECTNESS, not style: the staging
 * buffer is XRGB8888 (the wl_shm path's own format), so its top byte
 * — the byte a BGRA texture upload reads as ALPHA — is ZERO. A
 * passthrough shader would hand the compositor an ARGB8888 swap
 * buffer with alpha 0: a fully TRANSPARENT window (premultiplied
 * blending leaves the destination untouched), while the CPU path's
 * XRGB8888 buffers are opaque BY FORMAT. Forcing alpha 1.0 in the
 * shader restores the CPU path's opacity semantics on any swap
 * format; preferring an alpha-less EGLConfig (below) makes the swap
 * buffer itself XRGB8888 — opaque by protocol, no blending at all —
 * so the lane is byte-identical to wl_shm on BOTH levels. The
 * self-check below masks the alpha byte off its comparisons for the
 * same reason (the X byte is nobody's pixel contract). */

struct fdk_wl_gl_fns {
    /* EGL core (libEGL.so.1). */
    EGLDisplay (*eglGetPlatformDisplay)(unsigned int, void *, const EGLint *);
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*eglTerminate)(EGLDisplay);
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
                                  EGLint, EGLint *);
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
                                   const EGLint *);
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay, EGLConfig,
                                         void *, const EGLint *);
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
                                 EGLContext);
    EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
    const char *(*eglQueryString)(EGLDisplay, unsigned int);
    EGLint (*eglGetError)(void);
    void *(*eglGetProcAddress)(const char *);
    /* GLES2 (through eglGetProcAddress — GLVND does not promise the
     * core symbols exist as dlsym-able libEGL exports). */
    unsigned int (*glCreateShader)(unsigned int);
    void (*glShaderSource)(unsigned int, int, const char *const *,
                           const int *);
    void (*glCompileShader)(unsigned int);
    void (*glGetShaderiv)(unsigned int, unsigned int, int *);
    void (*glGetShaderInfoLog)(unsigned int, int, int *, char *);
    unsigned int (*glCreateProgram)(void);
    void (*glAttachShader)(unsigned int, unsigned int);
    void (*glLinkProgram)(unsigned int);
    void (*glGetProgramiv)(unsigned int, unsigned int, int *);
    void (*glGetProgramInfoLog)(unsigned int, int, int *, char *);
    void (*glUseProgram)(unsigned int);
    int (*glGetUniformLocation)(unsigned int, const char *);
    void (*glUniform2f)(int, float, float);
    void (*glUniform1i)(int, int);
    int (*glGetAttribLocation)(unsigned int, const char *);
    void (*glEnableVertexAttribArray)(unsigned int);
    void (*glVertexAttribPointer)(unsigned int, int, unsigned int,
                                  unsigned char, int, const void *);
    unsigned int (*glGenBuffers)(int, unsigned int *);
    void (*glBindBuffer)(unsigned int, unsigned int);
    void (*glBufferData)(unsigned int, long, const void *, unsigned int);
    void (*glGenTextures)(int, unsigned int *);
    void (*glBindTexture)(unsigned int, unsigned int);
    void (*glTexImage2D)(unsigned int, int, int, int, int, int,
                         unsigned int, unsigned int, const void *);
    void (*glTexSubImage2D)(unsigned int, int, int, int, int, int,
                            unsigned int, unsigned int, const void *);
    void (*glTexParameteri)(unsigned int, unsigned int, int);
    void (*glPixelStorei)(unsigned int, int);
    const unsigned char *(*glGetString)(unsigned int);
    unsigned int (*glGetError)(void);
    void (*glReadPixels)(int, int, int, int, unsigned int, unsigned int,
                         void *);
    void (*glViewport)(int, int, int, int);
    void (*glFinish)(void);
    void (*glActiveTexture)(unsigned int);
    void (*glDrawArrays)(unsigned int, int, int);
};

/* libwayland-egl's whole exported surface (stable since 2012):
 * wl_egl_window_create/resize/destroy + the window struct type. */
struct wl_egl_window;
struct wl_egl_window *(*wl_egl_window_create)(struct wl_surface *, int,
                                              int);
void (*wl_egl_window_resize)(struct wl_egl_window *, int, int, int, int);
void (*wl_egl_window_destroy)(struct wl_egl_window *);

struct fdk_wayland_egl {
    fdk_platform_connection *conn; /* owner */
    void *lib_egl;                 /* libEGL.so.1 handle; never dlclose'd */
    void *lib_wl_egl;              /* libwayland-egl.so.1 handle; pinned  */
    struct fdk_wl_gl_fns fn;
    EGLDisplay egl_display;
    EGLConfig config;
    unsigned int program;          /* the quad program (shared by windows) */
    int program_u_size;
    int program_u_tex;
    int program_a_pos;
    unsigned int quad_vbo;         /* the fullscreen-quad vertex buffer    */
    int refused;                   /* policy verdict: stop offering       */
    int selfcheck;                 /* 0 unverified / 1 passed             */
    char renderer[80];
};

enum {
    WL_GL_SELFCHECK_UNVERIFIED = 0,
    WL_GL_SELFCHECK_PASSED = 1
};

struct fdk_wayland_egl_window {
    struct fdk_wayland_egl *egl; /* owning lane state (never NULL) */
    struct wl_egl_window *wl_win;
    EGLSurface surface;
    EGLContext ctx;
    unsigned int tex;
    int w, h;
    fdk_u32 *staging;
    int tex_ok;
    int presents;
    long fail_at;                 /* FDK_GL_FAIL_AT test seam            */
};

/* ---- small utilities (shared policy with x11_gl.c) --------------------- */

static const char *fdk_wlgl_strcasestr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) {
        return hay;
    }
    for (const char *p = hay; *p != '\0'; p++) {
        size_t i = 0;
        while (i < nlen && p[i] != '\0' &&
               tolower((unsigned char)p[i]) ==
                   tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return p;
        }
    }
    return NULL;
}

static int renderer_is_software(const char *renderer) {
    if (renderer == NULL || renderer[0] == '\0') {
        return 0;
    }
    return fdk_wlgl_strcasestr(renderer, "llvmpipe") != NULL ||
           fdk_wlgl_strcasestr(renderer, "softpipe") != NULL ||
           fdk_wlgl_strcasestr(renderer, "swrast") != NULL;
}

/* POSIX dlsym/eglGetProcAddress return void*; ISO C (pedantic)
 * forbids direct object->function conversion — the dereference
 * idiom, as in x11_gl.c. */
#define FDK_WLGL_STORE_FN(dst, sym) (*(void **)&(dst) = (sym))

/* ---- process-wide policy cache (the x11_gl.c discipline) ---------------
 *
 * Keyed on the env knobs' presence, exactly as the GLX lane's cache:
 * flipping FDK_GL_FORCE / FDK_NO_GL between connections gets a fresh
 * decision; identical knobs reuse the verdict. A REAL self-check
 * failure carries its own stamp with the GLX lane's exact key
 * discipline: it refuses later default-knob connections for the
 * whole process (one decision, one log line) while FDK_GL_FORCE
 * explicitly overrides it — the test seam's escape hatch. */
static int wlgl_policy_decided = 0;
static int wlgl_policy_refused = 0;
static int wlgl_policy_no_gl = 0;
static int wlgl_policy_force = 0;
static int wlgl_policy_selfcheck_failed = 0;

void fdk__wayland_egl_test_reset(void) {
    /* TEST SEAM (mirrors fdk__x11_gl_test_reset): clear the
     * process-wide verdicts so a later test group — or a real run
     * after a poisoned leg — starts from an honest UNVERIFIED state.
     * Never compiled out, never called by the library itself. */
    wlgl_policy_decided = 0;
    wlgl_policy_refused = 0;
    wlgl_policy_no_gl = 0;
    wlgl_policy_force = 0;
    wlgl_policy_selfcheck_failed = 0;
}

static void wlgl_policy_stamp(int refused, int no_gl, int force) {
    wlgl_policy_decided = 1;
    wlgl_policy_refused = refused;
    wlgl_policy_no_gl = no_gl;
    wlgl_policy_force = force;
    /* The self-check verdict is NOT stamped here: it is decided at
     * the first completed frame, under the same env-knob key
     * discipline (see the self-check block in present()). */
}

/* A REAL self-check failure is driver-wide: stamped into the
 * process-wide cache so later connections (same env knobs) refuse
 * the lane before any EGL call — one decision, one log line, for
 * the whole process. */
static void wlgl_selfcheck_stamp_failed(void) {
    wlgl_policy_selfcheck_failed = 1;
}

static int wlgl_policy_cached_refusal(void) {
    const int no_gl = getenv("FDK_NO_GL") != NULL;
    const int force = getenv("FDK_GL_FORCE") != NULL;
    if (wlgl_policy_decided && no_gl == wlgl_policy_no_gl &&
        force == wlgl_policy_force && wlgl_policy_refused) {
        return 1;
    }
    if (wlgl_policy_selfcheck_failed && !no_gl && !force) {
        return 1; /* a REAL readback failure decided earlier in this
                   * process, same knobs: refuse without re-probing */
    }
    if (wlgl_policy_selfcheck_failed && force) {
        return 0; /* FDK_GL_FORCE explicitly overrides even a decided
                   * real failure — the test seam's escape hatch */
    }
    return 0;
}

/* ---- loader -------------------------------------------------------------- */

struct fdk_wayland_egl *fdk_wayland_egl_try_load(
    fdk_platform_connection *conn) {
    if (conn == NULL || conn->display == NULL) {
        return NULL;
    }

    const int no_gl = getenv("FDK_NO_GL") != NULL;
    const int force = getenv("FDK_GL_FORCE") != NULL;

    if (wlgl_policy_cached_refusal()) {
        return NULL;
    }
    if (no_gl) {
        FDK_DEBUG("FDK_NO_GL set — GPU present lane disabled");
        wlgl_policy_stamp(1, no_gl, force);
        return NULL;
    }

    void *lib_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib_egl == NULL) {
        const char *err = dlerror();
        FDK_DEBUG("libEGL.so.1 not loadable (%s) — CPU present path only",
                  err != NULL ? err : "unknown error");
        wlgl_policy_stamp(1, no_gl, force);
        return NULL;
    }
    void *lib_wl_egl = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib_wl_egl == NULL) {
        const char *err = dlerror();
        FDK_DEBUG("libwayland-egl.so.1 not loadable (%s) — CPU present "
                  "path only",
                  err != NULL ? err : "unknown error");
        /* libEGL stays pinned (the GLX lane's note). */
        wlgl_policy_stamp(1, no_gl, force);
        return NULL;
    }

    struct fdk_wayland_egl *egl = fdk_alloc(sizeof(struct fdk_wayland_egl));
    if (egl == NULL) {
        return NULL; /* libs stay pinned */
    }
    egl->conn = conn;
    egl->lib_egl = lib_egl;
    egl->lib_wl_egl = lib_wl_egl;
    egl->egl_display = EGLC_NO_DISPLAY;
    egl->config = NULL;
    egl->program = 0;
    egl->program_u_size = -1;
    egl->program_u_tex = -1;
    egl->program_a_pos = -1;
    egl->quad_vbo = 0;
    egl->refused = 0;
    egl->selfcheck = WL_GL_SELFCHECK_UNVERIFIED;
    egl->renderer[0] = '\0';
    memset(&egl->fn, 0, sizeof(egl->fn));

    /* wl_egl_window trio — dlsym on libwayland-egl (its only symbols). */
#define WL_RESOLVE(field, name) \
    do { \
        void *sym_ = dlsym(lib_wl_egl, name); \
        if (sym_ == NULL) { \
            FDK_DEBUG("%s unresolvable — CPU present path only", name); \
            fdk_free(egl); \
            return NULL; \
        } \
        FDK_WLGL_STORE_FN(field, sym_); \
    } while (0)
    WL_RESOLVE(wl_egl_window_create, "wl_egl_window_create");
    WL_RESOLVE(wl_egl_window_resize, "wl_egl_window_resize");
    WL_RESOLVE(wl_egl_window_destroy, "wl_egl_window_destroy");
#undef WL_RESOLVE

    /* EGL core — dlsym on libEGL (these ARE guaranteed exports of the
     * loader library itself). */
#define EGL_RESOLVE(field, name) \
    do { \
        void *sym_ = dlsym(lib_egl, name); \
        if (sym_ == NULL) { \
            FDK_DEBUG("%s unresolvable — CPU present path only", name); \
            fdk_free(egl); \
            return NULL; \
        } \
        FDK_WLGL_STORE_FN(egl->fn.field, sym_); \
    } while (0)
    EGL_RESOLVE(eglGetPlatformDisplay, "eglGetPlatformDisplay");
    EGL_RESOLVE(eglInitialize, "eglInitialize");
    EGL_RESOLVE(eglTerminate, "eglTerminate");
    EGL_RESOLVE(eglChooseConfig, "eglChooseConfig");
    EGL_RESOLVE(eglCreateContext, "eglCreateContext");
    EGL_RESOLVE(eglDestroyContext, "eglDestroyContext");
    EGL_RESOLVE(eglCreateWindowSurface, "eglCreateWindowSurface");
    EGL_RESOLVE(eglDestroySurface, "eglDestroySurface");
    EGL_RESOLVE(eglMakeCurrent, "eglMakeCurrent");
    EGL_RESOLVE(eglSwapBuffers, "eglSwapBuffers");
    EGL_RESOLVE(eglSwapInterval, "eglSwapInterval");
    EGL_RESOLVE(eglQueryString, "eglQueryString");
    EGL_RESOLVE(eglGetError, "eglGetError");
    EGL_RESOLVE(eglGetProcAddress, "eglGetProcAddress");
#undef EGL_RESOLVE

    /* The Wayland EGLDisplay over OUR wl_display. */
    EGLDisplay ed = egl->fn.eglGetPlatformDisplay(
        (unsigned int)EGLC_PLATFORM_WAYLAND_KHR, conn->display, NULL);
    if (ed == EGLC_NO_DISPLAY) {
        FDK_DEBUG("eglGetPlatformDisplay(WAYLAND) failed (0x%x) — CPU "
                  "present path only",
                  (unsigned)egl->fn.eglGetError());
        fdk_free(egl);
        return NULL;
    }
    EGLint maj = 0, min = 0;
    if (!egl->fn.eglInitialize(ed, &maj, &min)) {
        FDK_DEBUG("eglInitialize failed (0x%x) — CPU present path only",
                  (unsigned)egl->fn.eglGetError());
        fdk_free(egl);
        return NULL;
    }
    egl->egl_display = ed;

    /* One XRGB GLES2 window config, shared by every window —
     * ALPHA_SIZE 0 FIRST (an XRGB8888 swap buffer: opaque by
     * protocol, the wl_shm path's own format, no compositor
     * blending at all), with the ARGB8 form as the fallback: the
     * quad shader forces alpha 1.0 either way (see QUAD_FS's note),
     * so both configs present identical opaque pixels. */
    const EGLint cfg_xrgb[] = {
        EGLC_SURFACE_TYPE, EGLC_WINDOW_BIT,
        EGLC_RENDERABLE_TYPE, EGLC_OPENGL_ES2_BIT,
        EGLC_RED_SIZE, 8, EGLC_GREEN_SIZE, 8, EGLC_BLUE_SIZE, 8,
        EGLC_ALPHA_SIZE, 0,
        EGLC_NONE
    };
    const EGLint cfg_argb[] = {
        EGLC_SURFACE_TYPE, EGLC_WINDOW_BIT,
        EGLC_RENDERABLE_TYPE, EGLC_OPENGL_ES2_BIT,
        EGLC_RED_SIZE, 8, EGLC_GREEN_SIZE, 8, EGLC_BLUE_SIZE, 8,
        EGLC_ALPHA_SIZE, 8,
        EGLC_NONE
    };
    EGLint nc = 0;
    if (!egl->fn.eglChooseConfig(ed, cfg_xrgb, &egl->config, 1, &nc) ||
        nc < 1) {
        FDK_DEBUG("no alpha-less GLES2 window EGLConfig — trying the "
                  "ARGB8 form (the shader pins opacity)");
        if (!egl->fn.eglChooseConfig(ed, cfg_argb, &egl->config, 1, &nc) ||
            nc < 1) {
            FDK_DEBUG("no GLES2 window EGLConfig — CPU present path only");
            (void)egl->fn.eglTerminate(ed);
            egl->egl_display = EGLC_NO_DISPLAY;
            fdk_free(egl);
            return NULL;
        }
    }

    /* GLES2 entry points — eglGetProcAddress exclusively (the
     * documented EGL loader path; GLVND does not promise dlsym). */
#define GLES_RESOLVE(field, name) \
    do { \
        void *sym_ = egl->fn.eglGetProcAddress(name); \
        if (sym_ == NULL) { \
            FDK_DEBUG("%s unresolvable — CPU present path only", name); \
            (void)egl->fn.eglTerminate(ed); \
            egl->egl_display = EGLC_NO_DISPLAY; \
            fdk_free(egl); \
            return NULL; \
        } \
        FDK_WLGL_STORE_FN(egl->fn.field, sym_); \
    } while (0)
    GLES_RESOLVE(glCreateShader, "glCreateShader");
    GLES_RESOLVE(glShaderSource, "glShaderSource");
    GLES_RESOLVE(glCompileShader, "glCompileShader");
    GLES_RESOLVE(glGetShaderiv, "glGetShaderiv");
    GLES_RESOLVE(glGetShaderInfoLog, "glGetShaderInfoLog");
    GLES_RESOLVE(glCreateProgram, "glCreateProgram");
    GLES_RESOLVE(glAttachShader, "glAttachShader");
    GLES_RESOLVE(glLinkProgram, "glLinkProgram");
    GLES_RESOLVE(glGetProgramiv, "glGetProgramiv");
    GLES_RESOLVE(glGetProgramInfoLog, "glGetProgramInfoLog");
    GLES_RESOLVE(glUseProgram, "glUseProgram");
    GLES_RESOLVE(glGetUniformLocation, "glGetUniformLocation");
    GLES_RESOLVE(glUniform2f, "glUniform2f");
    GLES_RESOLVE(glUniform1i, "glUniform1i");
    GLES_RESOLVE(glGetAttribLocation, "glGetAttribLocation");
    GLES_RESOLVE(glEnableVertexAttribArray, "glEnableVertexAttribArray");
    GLES_RESOLVE(glVertexAttribPointer, "glVertexAttribPointer");
    GLES_RESOLVE(glGenBuffers, "glGenBuffers");
    GLES_RESOLVE(glBindBuffer, "glBindBuffer");
    GLES_RESOLVE(glBufferData, "glBufferData");
    GLES_RESOLVE(glGenTextures, "glGenTextures");
    GLES_RESOLVE(glBindTexture, "glBindTexture");
    GLES_RESOLVE(glTexImage2D, "glTexImage2D");
    GLES_RESOLVE(glTexSubImage2D, "glTexSubImage2D");
    GLES_RESOLVE(glTexParameteri, "glTexParameteri");
    GLES_RESOLVE(glPixelStorei, "glPixelStorei");
    GLES_RESOLVE(glGetString, "glGetString");
    GLES_RESOLVE(glGetError, "glGetError");
    GLES_RESOLVE(glReadPixels, "glReadPixels");
    GLES_RESOLVE(glViewport, "glViewport");
    GLES_RESOLVE(glFinish, "glFinish");
    GLES_RESOLVE(glActiveTexture, "glActiveTexture");
    GLES_RESOLVE(glDrawArrays, "glDrawArrays");
#undef GLES_RESOLVE
#undef FDK_WLGL_STORE_FN

    FDK_DEBUG("libEGL + libwayland-egl loaded, EGL display initialized — "
              "GPU present lane available");
    wlgl_policy_stamp(0, no_gl, force);
    return egl;
}

void fdk_wayland_egl_shutdown(struct fdk_wayland_egl *egl) {
    /* Windows die before the connection (their surfaces/contexts go
     * with them). What remains is the shared program/VBO and the
     * state itself; the libraries stay pinned — the header's note.
     * The quad's VBO is NOT orphaned through a live GL call here:
     * teardown has no current context (every window's context died
     * with its window), and calling GLES entry points with none is
     * undefined — eglTerminate marks the display's resources for
     * deletion, which is the honest process-teardown answer. */
    if (egl == NULL) {
        return;
    }
    if (egl->egl_display != EGLC_NO_DISPLAY) {
        (void)egl->fn.eglTerminate(egl->egl_display);
    }
    fdk_free(egl);
}

/* ---- shader helpers ------------------------------------------------------ */

static unsigned int lane_compile(struct fdk_wayland_egl *egl,
                                 unsigned int kind, const char *src) {
    unsigned int sh = egl->fn.glCreateShader(kind);
    if (sh == 0) {
        return 0;
    }
    const char *srcs[1] = { src };
    egl->fn.glShaderSource(sh, 1, srcs, NULL);
    egl->fn.glCompileShader(sh);
    int ok = 0;
    egl->fn.glGetShaderiv(sh, GLC_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        int n = 0;
        egl->fn.glGetShaderInfoLog(sh, (int)sizeof(log) - 1, &n, log);
        log[n > 0 && n < (int)sizeof(log) ? n : 0] = '\0';
        FDK_WARN("quad shader compile failed: %s", log);
        return 0;
    }
    return sh;
}

/* Builds the shared quad program + VBO. Requires a CURRENT context
 * (called from window_try_init with the first context bound).
 * Returns 0 on failure. */
static int lane_build_program(struct fdk_wayland_egl *egl) {
    if (egl->program != 0) {
        return 1; /* already built (later windows share it) */
    }
    unsigned int vs = lane_compile(egl, GLC_VERTEX_SHADER, QUAD_VS);
    unsigned int fs = lane_compile(egl, GLC_FRAGMENT_SHADER, QUAD_FS);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    unsigned int prog = egl->fn.glCreateProgram();
    if (prog == 0) {
        return 0;
    }
    egl->fn.glAttachShader(prog, vs);
    egl->fn.glAttachShader(prog, fs);
    egl->fn.glLinkProgram(prog);
    int ok = 0;
    egl->fn.glGetProgramiv(prog, GLC_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        int n = 0;
        egl->fn.glGetProgramInfoLog(prog, (int)sizeof(log) - 1, &n, log);
        log[n > 0 && n < (int)sizeof(log) ? n : 0] = '\0';
        FDK_WARN("quad program link failed: %s", log);
        return 0;
    }
    egl->program = prog;
    egl->program_u_size = egl->fn.glGetUniformLocation(prog, "u_size");
    egl->program_u_tex = egl->fn.glGetUniformLocation(prog, "u_tex");
    egl->program_a_pos = egl->fn.glGetAttribLocation(prog, "a_pos");

    /* The quad: two triangles covering normalized (0,0)-(1,1)² (the
     * shader maps that to the whole viewport; see QUAD_VS's note);
     * the u_size uniform still carries w/h per present for the
     * draw's reference, so the buffer is size-independent. */
    static const float quad[] = {
        0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f
    };
    egl->fn.glGenBuffers(1, &egl->quad_vbo);
    egl->fn.glBindBuffer(GLC_ARRAY_BUFFER, egl->quad_vbo);
    egl->fn.glBufferData(GLC_ARRAY_BUFFER, (long)sizeof(quad), quad,
                         GLC_STATIC_DRAW);
    return 1;
}

/* One-time texture state (per texture — GL objects carry their own
 * sampler params). */
static void lane_setup_texture(struct fdk_wayland_egl *egl,
                               unsigned int tex) {
    egl->fn.glBindTexture(GLC_TEXTURE_2D, tex);
    egl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MIN_FILTER,
                            GLC_NEAREST);
    egl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MAG_FILTER,
                            GLC_NEAREST);
    egl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_S,
                            GLC_CLAMP_TO_EDGE);
    egl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_T,
                            GLC_CLAMP_TO_EDGE);
}

/* ---- per-window lane ------------------------------------------------------ */

struct fdk_wayland_egl_window *fdk_wayland_egl_window_try_init(
    struct fdk_wayland_egl *egl, fdk_platform_window *pwindow) {
    if (egl == NULL || pwindow == NULL || egl->refused ||
        pwindow->surface == NULL) {
        return NULL;
    }

    /* Renderer policy needs a current context; the context needs a
     * surface; the surface needs the wl_egl_window — created at the
     * CURRENT size (the caller passes it through render_size, set
     * before acquisition). Zero-size (pre-configure) falls back —
     * the caller retries at the next acquisition with a size. */
    fdk_i32 w = pwindow->render_size.width;
    fdk_i32 h = pwindow->render_size.height;
    if (w < 1 || h < 1) {
        w = 1;
        h = 1;
    }

    struct wl_egl_window *wl_win = wl_egl_window_create(pwindow->surface,
                                                        (int)w, (int)h);
    if (wl_win == NULL) {
        FDK_DEBUG("wl_egl_window_create failed — CPU present path for "
                  "this connection");
        egl->refused = 1;
        return NULL;
    }
    EGLSurface surface = egl->fn.eglCreateWindowSurface(
        egl->egl_display, egl->config, wl_win, NULL);
    if (surface == EGLC_NO_SURFACE) {
        FDK_DEBUG("eglCreateWindowSurface failed (0x%x) — CPU present "
                  "path for this connection",
                  (unsigned)egl->fn.eglGetError());
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        return NULL;
    }
    const EGLint ctx_attr[] = { EGLC_CONTEXT_CLIENT_VERSION, 2,
                                EGLC_NONE };
    EGLContext ctx = egl->fn.eglCreateContext(egl->egl_display, egl->config,
                                              EGLC_NO_CONTEXT, ctx_attr);
    if (ctx == EGLC_NO_CONTEXT) {
        FDK_DEBUG("eglCreateContext failed (0x%x) — CPU present path for "
                  "this connection",
                  (unsigned)egl->fn.eglGetError());
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        return NULL;
    }
    if (!egl->fn.eglMakeCurrent(egl->egl_display, surface, surface, ctx)) {
        FDK_WARN("eglMakeCurrent failed (0x%x) — CPU present path for "
                  "this connection",
                 (unsigned)egl->fn.eglGetError());
        (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        return NULL;
    }

    /* Renderer policy: the FIRST context decides for the connection
     * (driver-wide string). Software rasterizers stay on wl_shm. */
    if (egl->renderer[0] == '\0') {
        const unsigned char *r = egl->fn.glGetString(GLC_RENDERER);
        if (r != NULL) {
            (void)snprintf(egl->renderer, sizeof(egl->renderer), "%s", r);
        }
    }
    const int forced = getenv("FDK_GL_FORCE") != NULL;
    if (renderer_is_software(egl->renderer) && !forced) {
        FDK_INFO("GL renderer is software (\"%s\") — staying on the CPU "
                 "present path (FDK_GL_FORCE=1 overrides)",
                 egl->renderer);
        (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                     EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
        (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        wlgl_policy_stamp(1, getenv("FDK_NO_GL") != NULL, forced);
        return NULL;
    }

    /* The shared program rides the first context. */
    if (!lane_build_program(egl)) {
        (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                     EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
        (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        return NULL;
    }

    struct fdk_wayland_egl_window *ewin =
        fdk_alloc(sizeof(struct fdk_wayland_egl_window));
    if (ewin == NULL) {
        (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                     EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
        (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        return NULL;
    }
    ewin->egl = egl;
    ewin->wl_win = wl_win;
    ewin->surface = surface;
    ewin->ctx = ctx;
    ewin->tex = 0;
    ewin->w = 0;
    ewin->h = 0;
    ewin->staging = NULL;
    ewin->tex_ok = 0;
    ewin->presents = 0;
    ewin->fail_at = 0;

    const char *fail_at = getenv("FDK_GL_FAIL_AT");
    if (fail_at != NULL && fail_at[0] != '\0') {
        long v = strtol(fail_at, NULL, 10);
        ewin->fail_at = v > 0 ? v : 0;
    }

    egl->fn.glGenTextures(1, &ewin->tex);
    lane_setup_texture(egl, ewin->tex);
    egl->fn.glPixelStorei(GLC_UNPACK_ALIGNMENT, 1);
    (void)egl->fn.glGetError();

    /* Unbind: another window's init/present rebinds as needed. */
    (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                 EGLC_NO_SURFACE, EGLC_NO_CONTEXT);

    FDK_INFO("GPU present lane active on Wayland (renderer \"%s\")",
             egl->renderer[0] != '\0' ? egl->renderer : "unknown");
    return ewin;
}

void fdk_wayland_egl_window_destroy(struct fdk_wayland_egl *egl,
                                    struct fdk_wayland_egl_window *ewin) {
    if (ewin == NULL) {
        return;
    }
    if (egl != NULL && egl->egl_display != EGLC_NO_DISPLAY) {
        (void)egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                     ewin->surface, ewin->ctx);
        (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                     EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
        (void)egl->fn.eglDestroyContext(egl->egl_display, ewin->ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, ewin->surface);
    }
    wl_egl_window_destroy(ewin->wl_win);
    if (ewin->staging != NULL) {
        fdk_free(ewin->staging);
    }
    fdk_free(ewin);
}

fdk_u32 *fdk_wayland_egl_window_staging(struct fdk_wayland_egl_window *ewin,
                                        int w, int h) {
    if (ewin == NULL || w <= 0 || h <= 0) {
        return NULL;
    }
    if (ewin->staging != NULL && ewin->w == w && ewin->h == h) {
        return ewin->staging;
    }
    fdk_u32 *fresh = fdk_alloc(sizeof(fdk_u32) * (size_t)w * (size_t)h);
    if (fresh == NULL) {
        return NULL;
    }
    if (ewin->staging != NULL) {
        fdk_free(ewin->staging);
    }
    ewin->staging = fresh;
    ewin->w = w;
    ewin->h = h;
    ewin->tex_ok = 0;
    /* Resize the EGL-side wrapper to the new size too — the EGL
     * surface reallocates its back buffer at the next swap, exactly
     * the GLX lane's resize model. */
    wl_egl_window_resize(ewin->wl_win, w, h, 0, 0);
    return fresh;
}

/* The draw: program + quad + 1:1 texture. The quad's texture must
 * be bound on unit 0 at DRAW time (not just at upload time) — the
 * program's sampler is unit 0, and glBindTexture at upload leaves
 * the right texture bound there; this re-asserts it defensively. */
static void draw_fullscreen_quad(struct fdk_wayland_egl *egl, int w, int h) {
    egl->fn.glUseProgram(egl->program);
    egl->fn.glUniform2f(egl->program_u_size, (float)w, (float)h);
    egl->fn.glUniform1i(egl->program_u_tex, 0);
    egl->fn.glActiveTexture(GLC_TEXTURE0);
    egl->fn.glBindBuffer(GLC_ARRAY_BUFFER, egl->quad_vbo);
    egl->fn.glEnableVertexAttribArray((unsigned int)egl->program_a_pos);
    egl->fn.glVertexAttribPointer((unsigned int)egl->program_a_pos, 2,
                                  GLC_FLOAT, 0, 0, NULL);
    egl->fn.glDrawArrays(GLC_TRIANGLES, 0, 6);
}

int fdk_wayland_egl_window_present(struct fdk_wayland_egl_window *ewin,
                                   const fdk_platform_damage *damage, int w,
                                   int h) {
    if (ewin == NULL || damage == NULL || w <= 0 || h <= 0 ||
        ewin->staging == NULL || ewin->w != w || ewin->h != h) {
        return -1;
    }
    struct fdk_wayland_egl *egl = ewin->egl;

    if (ewin->fail_at > 0 && ewin->presents + 1 == ewin->fail_at) {
        ewin->presents++;
        return -1;
    }

    if (!egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                ewin->surface, ewin->ctx)) {
        return -1; /* context lost: permanent downgrade */
    }
    (void)egl->fn.glGetError();

    egl->fn.glBindTexture(GLC_TEXTURE_2D, ewin->tex);
    egl->fn.glPixelStorei(GLC_UNPACK_ROW_LENGTH, w);

    if (!ewin->tex_ok) {
        egl->fn.glTexImage2D(GLC_TEXTURE_2D, 0, GLC_RGBA, w, h, 0, GLC_BGRA,
                             GLC_UNSIGNED_BYTE, ewin->staging);
        ewin->tex_ok = 1;
    } else {
        long long area = 0;
        int live = 0;
        long long rx0[FDK_PLATFORM_DAMAGE_RECTS];
        long long ry0[FDK_PLATFORM_DAMAGE_RECTS];
        long long rx1[FDK_PLATFORM_DAMAGE_RECTS];
        long long ry1[FDK_PLATFORM_DAMAGE_RECTS];
        if (!damage->full) {
            for (int i = 0; i < damage->count; i++) {
                const fdk_rect *rc = &damage->rects[i];
                long long x0 = rc->x, y0 = rc->y;
                long long x1 = (long long)rc->x + rc->width;
                long long y1 = (long long)rc->y + rc->height;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 > w) x1 = w;
                if (y1 > h) y1 = h;
                if (x1 > x0 && y1 > y0) {
                    rx0[live] = x0;
                    ry0[live] = y0;
                    rx1[live] = x1;
                    ry1[live] = y1;
                    live++;
                    area += (x1 - x0) * (y1 - y0);
                }
            }
        }
        if (damage->full || area * 4 >= (long long)w * h * 3) {
            egl->fn.glTexSubImage2D(GLC_TEXTURE_2D, 0, 0, 0, w, h, GLC_BGRA,
                                    GLC_UNSIGNED_BYTE, ewin->staging);
        } else {
            for (int i = 0; i < live; i++) {
                const fdk_u32 *src = ewin->staging +
                                     (size_t)ry0[i] * (size_t)w +
                                     (size_t)rx0[i];
                egl->fn.glTexSubImage2D(
                    GLC_TEXTURE_2D, 0, (int)rx0[i], (int)ry0[i],
                    (int)(rx1[i] - rx0[i]), (int)(ry1[i] - ry0[i]),
                    GLC_BGRA, GLC_UNSIGNED_BYTE, src);
            }
        }
    }

    draw_fullscreen_quad(egl, w, h);

    if (egl->fn.glGetError() != 0) {
        FDK_WARN("GL error during GPU present — downgrading this window "
                 "to the CPU path");
        return -1;
    }

    /* First-completed-frame self-verification (the Wayland form —
     * see the file header): glReadPixels of the BACK BUFFER pre-swap,
     * four quarter-grid probes against the staging buffer. GLES2's
     * readback is bottom-left-origin, so probe (sx, sy) reads row
     * (h - 1 - sy). A mismatch is REAL (no vblank race — the pixels
     * are in the buffer we just drew) and refuses the lane for the
     * connection + process policy, mirroring the GLX lane's honest
     * verdicts without the server-side read. */
    if (egl->selfcheck == WL_GL_SELFCHECK_UNVERIFIED) {
        const int poisoned = getenv("FDK_GL_SELFCHECK_POISON") != NULL;
        int ok = 1;
        for (int i = 0; i < 4 && ok; i++) {
            int sx = (w * (i * 2 + 1)) / 8;
            int sy = (h * (i * 2 + 1)) / 8;
            if (sx >= w) sx = w - 1;
            if (sy >= h) sy = h - 1;
            if (sx < 0 || sy < 0) {
                continue;
            }
            fdk_u32 got = 0;
            /* RGBA/UNSIGNED_BYTE — the ONLY format GLES2 guarantees
             * for default-framebuffer readback (BGRA pairs ride the
             * implementation-color-read query; the GLX lane's BGRA
             * readback is a DESKTOP GL guarantee that does not cross
             * over). Bytes land [R,G,B,A]; the staging u32 is
             * [B,G,R,X] little-endian — compare channel-wise. */
            unsigned char rgba[4] = { 0, 0, 0, 0 };
            egl->fn.glReadPixels(sx, h - 1 - sy, 1, 1, GLC_RGBA,
                                 GLC_UNSIGNED_BYTE, rgba);
            got = (fdk_u32)rgba[0] << 16 | (fdk_u32)rgba[1] << 8 |
                  (fdk_u32)rgba[2];
            fdk_u32 want = ewin->staging[(size_t)sy * (size_t)w +
                                         (size_t)sx] & 0x00FFFFFFu;
            /* Alpha masked off BOTH sides: the staging buffer's X
             * byte is zero by format, while the shader pins 1.0 (the
             * QUAD_FS note) — the X byte is nobody's contract. */
            if (poisoned || got != want) {
                ok = 0;
            }
        }
        if (!ok) {
            FDK_WARN("GPU lane self-check: persistent mismatch — "
                     "refusing the GPU present lane for this process "
                     "(renderer \"%s\")",
                     egl->renderer[0] != '\0' ? egl->renderer : "unknown");
            egl->selfcheck = WL_GL_SELFCHECK_UNVERIFIED; /* stays; the
                                                          * verdict is
                                                          * process-wide
                                                          * via the stamp
                                                          * below */
            egl->refused = 1;
            wlgl_selfcheck_stamp_failed();
            return -1; /* this window downgrades, later ones refuse */
        }
        egl->selfcheck = WL_GL_SELFCHECK_PASSED;
    }

    if (!egl->fn.eglSwapBuffers(egl->egl_display, ewin->surface)) {
        return -1;
    }
    ewin->presents++;

    /* The wl_surface commit that publishes the EGL buffer happens
     * inside eglSwapBuffers (libwayland-egl attaches the new buffer
     * and commits) — the flush discipline the SHM path applies after
     * commit_render_pending applies here too, so an app presenting
     * outside the pump still reaches the compositor promptly. */
    (void)wl_display_flush(egl->conn->display);
    return 0;
}
