#define FDK_LOG_TAG "wayland"

#include "platform/wayland/wayland_platform.h"

#include "platform/wayland/wayland_egl.h"
#include "render/gpaint.h"
#include "render/dmabuf_egl_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The GPU presentation lane on Wayland (1.0.0 / D-1) — implementation.
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
    EGLC_VERSION = 0x3054,
    EGLC_EXTENSIONS = 0x3055,
    EGLC_CLIENT_APIS = 0x308D,
    EGLC_CONFIG_ID = 0x3028,
    EGLC_CONTEXT_CLIENT_TYPE = 0x3097,
    EGLC_WIDTH = 0x3056,
    EGLC_HEIGHT = 0x3057,
    EGLC_SUCCESS = 0x3000,
    EGLC_PLATFORM_WAYLAND_KHR = 0x31D8,
    EGLC_BUFFER_SIZE = 0x3020,
    EGLC_DEPTH_SIZE = 0x3025,
    EGLC_STENCIL_SIZE = 0x3026,
    EGLC_GL_COLORSPACE = 0x309D,
    EGLC_GL_COLORSPACE_LINEAR = 0x3089,
    EGLC_GL_COLORSPACE_SRGB = 0x308B
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
    GLC_EXTENSIONS = 0x1F03,
    GLC_TEXTURE0 = 0x84C0,
    GLC_COMPILE_STATUS = 0x8B81,
    GLC_LINK_STATUS = 0x8B82,
    GLC_VERTEX_SHADER = 0x8B31,
    GLC_FRAGMENT_SHADER = 0x8B30,
    GLC_FLOAT = 0x1406,
    GLC_ARRAY_BUFFER = 0x8892,
    GLC_STATIC_DRAW = 0x88E4,
    GLC_TRIANGLES = 0x0004,
    GLC_COLOR_BUFFER_BIT = 0x4000,
    GLC_VERSION = 0x1F02,
    GLC_FRAMEBUFFER = 0x8D40,
    GLC_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GLC_FRAMEBUFFER_UNSUPPORTED = 0x8CDD
};

/* The quad program: a_pos carries NORMALIZED [0,1]² coordinates (the
 * VBO is size-independent; w/h ride the u_size uniform for the
 * draw-time viewport reference), Y flipped — the staging buffer's
 * row 0 is the TOP row — and texcoords 1:1. The ndc math is the
 * standard fullscreen-quad form: a_pos * 2 - 1 spans -1..+1 across
 * the WHOLE viewport; dividing pixel coordinates by u_size here
 * would shrink the quad to a single texel. */
#include "render/gpu_shaders.h"


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
    EGLBoolean (*eglGetConfigAttrib)(EGLDisplay, EGLConfig,
                                     unsigned int, EGLint *);
    EGLBoolean (*eglQueryContext)(EGLDisplay, EGLContext, unsigned int,
                                  EGLint *);
    EGLBoolean (*eglSwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*eglSwapInterval)(EGLDisplay, EGLint);
    EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint,
                                  EGLint *);
    const char *(*eglQueryString)(EGLDisplay, unsigned int);
    EGLint (*eglGetError)(void);
    void *(*eglGetProcAddress)(const char *);
    /* eglReleaseThread (EGL 1.2+): the per-thread state cleanup the
     * canonical teardown sequence ends with — NVIDIA keeps a small
     * malloc'd thread-state record alive per thread that touched the
     * display until this call runs (see fdk_wayland_egl_shutdown). */
    EGLBoolean (*eglReleaseThread)(void);
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
    void (*glDisableVertexAttribArray)(unsigned int);
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
    void (*glDeleteTextures)(int, const unsigned int *);
    unsigned int (*glCheckFramebufferStatus)(unsigned int);
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
/* Optional (libwayland-egl >= 1.15): the attached-size query the
 * first-present trace uses to name a size mismatch. NULL-tolerant. */
int (*wl_egl_window_get_attached_size)(struct wl_egl_window *, int *,
                                        int *);

/* Extension tokens (substring match against the ES2 extension
 * string — ES2's glGetString(GL_EXTENSIONS) form; the token spellings
 * are unique, none is a prefix of another). */
#define WLGL_EXT_BGRA "GL_EXT_texture_format_BGRA8888"
#define WLGL_EXT_ROWLEN "GL_EXT_unpack_subimage"

/* The UPLOAD CONTRACT's four shapes (fastest first — the probe's
 * preference order and the present ladder's rotation order):
 *
 *   0 BGRA + ROW_LENGTH   zero CPU cost per upload (the Mesa shape)
 *   1 BGRA + packed rows  sub-rects memcpy'd tightly per damaged rect
 *   2 RGBA twin + ROWLEN  a CPU R<->B swizzle of the whole staging
 *                         buffer, then strided sub-rects
 *   3 RGBA twin + packed  pure core GLES2 — no extension assumed
 *
 * The shape is CONNECTION state: the probe decides it once off what
 * the driver actually accepts (a real 4x4 upload per shape, not the
 * extension string), and the present ladder may rotate it downward
 * at runtime if a present fails with the chosen shape. */
enum {
    WLGL_SHAPE_BGRA_ROWLEN = 0,
    WLGL_SHAPE_BGRA_PACKED = 1,
    WLGL_SHAPE_TWIN_ROWLEN = 2,
    WLGL_SHAPE_TWIN_PACKED = 3,
    WLGL_SHAPE_COUNT = 4
};

static int lane_shape_uses_bgra(int shape) {
    return shape == WLGL_SHAPE_BGRA_ROWLEN ||
           shape == WLGL_SHAPE_BGRA_PACKED;
}

static int lane_shape_uses_rowlen(int shape) {
    return shape == WLGL_SHAPE_BGRA_ROWLEN ||
           shape == WLGL_SHAPE_TWIN_ROWLEN;
}

static const char *lane_shape_name(int shape) {
    switch (shape) {
    case WLGL_SHAPE_BGRA_ROWLEN: return "BGRA + UNPACK_ROW_LENGTH";
    case WLGL_SHAPE_BGRA_PACKED: return "BGRA + packed sub-rects";
    case WLGL_SHAPE_TWIN_ROWLEN: return "RGBA twin + UNPACK_ROW_LENGTH";
    case WLGL_SHAPE_TWIN_PACKED: return "RGBA twin + packed sub-rects";
    default: return "(invalid shape)";
    }
}

struct fdk_wayland_egl {
    fdk_platform_connection *conn; /* owner */
    void *lib_egl;                 /* libEGL.so.1 handle; pinned for the
                                    * connection, dlclosed at its teardown
                                    * (the round-7 NVIDIA exit-leak) */
    void *lib_wl_egl;              /* libwayland-egl.so.1 handle; pinned  */
    struct fdk_wl_gl_fns fn;
    EGLDisplay egl_display;
    EGLConfig config;
    EGLContext root_ctx;           /* the share-root (never current)      */
    unsigned int program;          /* the quad program (shared by windows) */
    int program_u_size;
    int program_u_tex;
    int program_a_pos;
    unsigned int quad_vbo;         /* the fullscreen-quad vertex buffer    */
    int refused;                   /* policy verdict: stop offering       */
    int selfcheck;                 /* 0 unverified / 1 passed             */
    /* Upload-path capabilities (the 1.0.0 NVIDIA fix — see the
     * UPLOAD CONTRACT block in present()): decided once per
     * connection off the FIRST context's extension string. */
    int has_bgra_ext;              /* GL_EXT_texture_format_BGRA8888     */
    int has_rowlen_ext;            /* GL_EXT_unpack_subimage             */
    /* The UPLOAD CONTRACT's operative state (the probe + ladder,
     * 1.0.0): `shape` is the connection's current upload shape —
     * the probe picks the first shape the DRIVER accepts (a real
     * 4x4 upload, not the extension string), and the present
     * ladder can rotate it at runtime when a present fails.
     * `shape_rejected` bit i marks shape i unavailable (extension
     * illegal, probe-rejected, or FDK_GL_PROBE_SKIP-forced).
     * `bgra_internal_rgba` records which internalformat spelling
     * the probe validated for the BGRA shapes: 0 = GL_BGRA_EXT as
     * internalformat (the EXT_texture_format_BGRA8888 table's own
     * row — the NVIDIA-legal spelling), 1 = GL_RGBA internal with
     * GL_BGRA_EXT format (the ES2-lenient spelling some stacks
     * accept). */
    int shape;                     /* WLGL_SHAPE_* currently in use       */
    unsigned int shape_rejected;   /* bit per shape                        */
    int bgra_internal_rgba;        /* probe-validated BGRA triple          */
    int probe_done;                /* the probe ran for this connection    */
    /* The texture graveyard (the share-group consequence): a dead
     * window's texture object outlives its context (share-group
     * members die with the GROUP, not the context), so destroys
     * queue the id here and the NEXT present on any window of the
     * connection — a context is current right then — deletes it.
     * Bounded ring; eglTerminate sweeps whatever remains. */
    unsigned int graveyard[16];
    int graveyard_count;
    int syncobj_note_logged;       /* §20: the registry-relevance note
                                    * has fired once for this process    */
    int engaged_dumped;            /* FDK_GL_TRACE: the engagement dump
                                    * fired once per connection          */
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
    /* The RGBA twin (the no-BGRA upload path's swizzled copy — the
     * UPLOAD CONTRACT block in present()): same w/h as staging,
     * reallocated with it, byte-swapped R<->B per pixel. NULL while
     * the BGRA extension serves the upload. */
    fdk_u32 *rgba_twin;
    /* Tightly-packed row scratch for sub-rect uploads when
     * UNPACK_ROW_LENGTH is unavailable: capacity in fdk_u32 units,
     * grown on demand to the largest single damaged rect. */
    fdk_u32 *pack_scratch;
    size_t pack_capacity;
    int tex_ok;
    int presents;
    long fail_at;                 /* FDK_GL_FAIL_AT test seam            */
    int poison_first;             /* FDK_GL_POISON_FIRST test seam       */
    /* ---- TASK-1 visibility (the GTX 1050 Ti report: the WARN named
     * neither the call nor the error code) --------------------
     *
     * lane_op names the lane's LAST GL call (with its shape-defining
     * arguments) so a present failure is attributed to the call that
     * really raised it; lane_checked is true while every call is
     * error-drained individually (the first present at a size — first
     * frame AND every resize — or the whole early window under
     * FDK_GL_TRACE), false once the lane is trusted (one drain per
     * present then, attributed to lane_op). */
    char lane_op[160];            /* "glTexImage2D(512x384, fmt=BGRA)"    */
    int lane_checked;             /* per-call drains active this present  */
    int lane_tracing;             /* FDK_GL_TRACE covers this present     */
    const char *lane_stage;       /* stage name for this present's report */
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

/* ---- TASK-1 visibility: error names, the trace, the drains ----------
 *
 * The GTX 1050 Ti report (2026-10-31): the lane failed its FIRST
 * present and the WARN named neither the failing call nor the error
 * code — on a driver the sandbox cannot reproduce, an unnamed
 * failure is undebuggable. Every error the lane raises now arrives
 * as a NAME (GL_INVALID_ENUM, EGL_BAD_ALLOC, ...), attributed to the
 * call that raised it, with the upload shape, the texture format,
 * the rect and the stage in the same line. */

static const char *lane_gl_err_name(unsigned int err) {
    switch (err) {
    case 0x0500: return "GL_INVALID_ENUM";
    case 0x0501: return "GL_INVALID_VALUE";
    case 0x0502: return "GL_INVALID_OPERATION";
    case 0x0503: return "GL_STACK_OVERFLOW";
    case 0x0504: return "GL_STACK_UNDERFLOW";
    case 0x0505: return "GL_OUT_OF_MEMORY";
    case 0x0506: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    default: return "unknown GL error";
    }
}

static const char *lane_egl_err_name(unsigned int err) {
    switch (err) {
    case 0x3000: return "EGL_SUCCESS";
    case 0x3001: return "EGL_NOT_INITIALIZED";
    case 0x3002: return "EGL_BAD_ACCESS";
    case 0x3003: return "EGL_BAD_ALLOC";
    case 0x3004: return "EGL_BAD_ATTRIBUTE";
    case 0x3005: return "EGL_BAD_CONFIG";
    case 0x3006: return "EGL_BAD_CONTEXT";
    case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
    case 0x3008: return "EGL_BAD_DISPLAY";
    case 0x3009: return "EGL_BAD_MATCH";
    case 0x300A: return "EGL_BAD_NATIVE_PIXMAP";
    case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
    case 0x300C: return "EGL_BAD_PARAMETER";
    case 0x300D: return "EGL_BAD_SURFACE";
    case 0x300E: return "EGL_CONTEXT_LOST";
    default: return "unknown EGL error";
    }
}

static const char *lane_fmt_name(unsigned int fmt) {
    return fmt == GLC_BGRA ? "GL_BGRA_EXT (0x80e1)"
                           : "GL_RGBA (0x1908)";
}

/* FDK_GL_TRACE=1: the lane's calls log at INFO for the first
 * presents — every GL/EGL call with its result, plus the one-time
 * engagement dump (EGL vendor/version/client APIs, the chosen
 * config's attributes, the context's API+version, the GL strings,
 * the extension flags, the wl_egl_window's attached size). INFO
 * (not DEBUG) on purpose: the default level shows it, so the recipe
 * for a desktop report is one env var, no log-level dance.
 *
 * Deliberately UNCACHED (a getenv per present): the §21 suite's
 * trace leg toggles the knob mid-process, and one lookup against
 * the environment block is nothing next to the upload that follows
 * it. */
static int wlgl_trace_enabled(void) {
    return getenv("FDK_GL_TRACE") != NULL;
}

/* How many presents per window the trace covers: the first four (a
 * first present, a partial-damage follow-up, room for a ladder
 * retry) and EVERY present that reallocates the texture (resizes —
 * the other risky first). */
static int lane_trace_this_present(const struct fdk_wayland_egl_window *ewin) {
    return wlgl_trace_enabled() &&
           (ewin->presents < 4 || !ewin->tex_ok);
}

/* Drains the GL error flag and returns the FIRST error raised since
 * the last drain (0 = clean). Bounded: a driver stuck raising errors
 * cannot spin this loop. */
static unsigned int lane_first_gl_error(
    const struct fdk_wayland_egl *egl) {
    unsigned int first = 0;
    for (int i = 0; i < 4; i++) {
        const unsigned int err = egl->fn.glGetError();
        if (err == 0) {
            break;
        }
        if (first == 0) {
            first = err;
        }
    }
    return first;
}

/* The loud per-call verdict: names the error, the call, the shape,
 * the format, the rect and the stage — ONE line a desktop report
 * can be built from. */
static void lane_report_call_error(
    const struct fdk_wayland_egl_window *ewin, unsigned int err,
    const char *stage) {
    const struct fdk_wayland_egl *egl = ewin->egl;
    FDK_WARN("GL error 0x%x (%s) at %s during %s — upload shape "
             "%s, texture %dx%d, presents so far %d — the ladder "
             "decides whether this downgrades",
             err, lane_gl_err_name(err), ewin->lane_op, stage,
             lane_shape_name(egl->shape),
             ewin->w, ewin->h, ewin->presents);
}

/* A setup-stage drain (TASK 1c): an error left over from SETUP must
 * be named and consumed BEFORE the first present, so the present's
 * verdict can only blame calls the present itself made. A stale
 * error here is WARNed, not swallowed silently. */
static void lane_stale_drain(struct fdk_wayland_egl *egl,
                             const char *stage) {
    const unsigned int err = lane_first_gl_error(egl);
    if (err != 0) {
        FDK_WARN("stale GL error 0x%x (%s) drained at %s — raised by "
                 "earlier setup, not blamed on the present that "
                 "follows",
                 err, lane_gl_err_name(err), stage);
    }
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
/* The pinned libEGL handle (round-7): process-static so the release
 * can run after wl_display_disconnect (the fdk_wayland_egl state is
 * freed before that point). */
static void *g_wlgl_library = NULL;
static int wlgl_policy_decided = 0;
static int wlgl_policy_refused = 0;
static int wlgl_policy_no_gl = 0;
static int wlgl_policy_force = 0;
static int wlgl_policy_selfcheck_failed = 0;
/* §22's teardown counters (the test seam below): connections shut
 * down, and how many of them reached the eglReleaseThread tail. */
static int wlgl_teardown_count = 0;
static int wlgl_release_thread_count = 0;

/* Extension-API v3, cap 19: the connection's zero-copy import truth.
 * Triggers the lane's lazy EGL load (the one-shot bring-up) and asks
 * the DISPLAY for the import extensions. 0 when no lane, no display,
 * or the extension is missing — exactly when the CPU-mode linear
 * fallback is the only import path. */
int fdk_wayland_egl_dmabuf_available(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return 0;
    }
    struct fdk_wayland_egl *e = conn->egl;
    if (e == NULL) {
        if (conn->egl_tried) {
            return 0;
        }
        conn->egl = fdk_wayland_egl_try_load(conn);
        conn->egl_tried = 1;
        e = conn->egl;
        if (e == NULL) {
            return 0;
        }
    }
    if (e->egl_display == NULL || e->lib_egl == NULL) {
        return 0;
    }
    struct fdk__dmabuf_egl_api *api = NULL;
    if (!fdk__dmabuf_egl_load(&api)) {
        return 0;
    }
    return fdk__dmabuf_egl_dpy_supports(api, e->egl_display, NULL);
}

void *fdk_wayland_egl_lane_display(fdk_platform_connection *conn) {
    if (conn == NULL || conn->egl == NULL) {
        return NULL;
    }
    return conn->egl->egl_display;
}

const char *fdk_wayland_egl_renderer(struct fdk_wayland_egl *egl) {
    return (egl != NULL && egl->renderer[0] != '\0') ? egl->renderer
                                                     : NULL;
}

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
    wlgl_teardown_count = 0;
    wlgl_release_thread_count = 0;
}

/* TEST SEAM (§22, the exit-leak round): the rig asserts the
 * canonical teardown tail call fired once per connection —
 * eglReleaseThread is the one step whose absence nothing else in
 * the log would name (the windows' destroys are already traced).
 * Counters are process-wide and never reset by the library. */
int fdk__wayland_egl_test_teardown_calls(int *release_thread) {
    if (release_thread != NULL) {
        *release_thread = wlgl_release_thread_count;
    }
    return wlgl_teardown_count;
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
    g_wlgl_library = lib_egl; /* the release hook's copy */
    egl->lib_wl_egl = lib_wl_egl;
    egl->egl_display = EGLC_NO_DISPLAY;
    egl->config = NULL;
    egl->root_ctx = EGLC_NO_CONTEXT;
    egl->program = 0;
    egl->program_u_size = -1;
    egl->program_u_tex = -1;
    egl->program_a_pos = -1;
    egl->quad_vbo = 0;
    egl->refused = 0;
    egl->selfcheck = WL_GL_SELFCHECK_UNVERIFIED;
    egl->has_bgra_ext = 0;
    egl->has_rowlen_ext = 0;
    egl->shape = WLGL_SHAPE_BGRA_ROWLEN;
    egl->shape_rejected = 0;
    egl->bgra_internal_rgba = 0;
    egl->probe_done = 0;
    egl->graveyard_count = 0;
    egl->syncobj_note_logged = 0;
    egl->engaged_dumped = 0;
    egl->renderer[0] = '\0';
    memset(&egl->fn, 0, sizeof(egl->fn));
    wl_egl_window_get_attached_size = NULL;

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
    /* Optional (libwayland-egl 1.15+): the attached-size query. A
     * missing symbol only costs the trace that one line. */
    {
        void *sym_ = dlsym(lib_wl_egl, "wl_egl_window_get_attached_size");
        if (sym_ != NULL) {
            FDK_WLGL_STORE_FN(wl_egl_window_get_attached_size, sym_);
        }
    }
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
    EGL_RESOLVE(eglGetConfigAttrib, "eglGetConfigAttrib");
    EGL_RESOLVE(eglQueryContext, "eglQueryContext");
    EGL_RESOLVE(eglQuerySurface, "eglQuerySurface");
    EGL_RESOLVE(eglSwapBuffers, "eglSwapBuffers");
    EGL_RESOLVE(eglSwapInterval, "eglSwapInterval");
    EGL_RESOLVE(eglQueryString, "eglQueryString");
    EGL_RESOLVE(eglGetError, "eglGetError");
    EGL_RESOLVE(eglGetProcAddress, "eglGetProcAddress");
    /* Optional (EGL 1.2+, present everywhere real; a stub without it
     * only costs the shutdown tail call — see shutdown's note). */
    {
        void *sym_ = dlsym(lib_egl, "eglReleaseThread");
        if (sym_ != NULL) {
            FDK_WLGL_STORE_FN(egl->fn.eglReleaseThread, sym_);
        }
    }
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
    /* EGL's sort order prefers LARGER total color bits, so an 8/8/8
     * request happily returns a 10-bit-per-channel config (llvmpipe's
     * RGB10: BUFFER_SIZE 30). That breaks the lane's byte-identity
     * contract by construction — 8-bit values written through a
     * 10-bit unorm surface double-round, and Mesa's ReadPixels
     * conversion truncates where the CPU packer rounds (the
     * 2026-11-11 wayland-lane audit: ref (26,31,41) read back as
     * (25,30,41), a 1-LSB shift on every other value). Re-enumerate
     * and prefer the config whose channels are EXACTLY 8 bits; keep
     * the driver's answer only when no 8-bit form exists (honest,
     * logged — the pixel gates would then legitimately carry a
     * 1-LSB allowance). */
    {
        EGLConfig cands[64];
        EGLint got = 0;
        const EGLint *req = cfg_xrgb;
        if (!egl->fn.eglChooseConfig(ed, req, cands, 64, &got) || got < 1) {
            req = cfg_argb;
            if (!egl->fn.eglChooseConfig(ed, req, cands, 64, &got) ||
                got < 1) {
                got = 0; /* keep the single-config answer above */
            }
        }
        for (int i = 0; i < got; i++) {
            EGLint r = 0, g = 0, b = 0, a = 0;
            (void)egl->fn.eglGetConfigAttrib(ed, cands[i], EGLC_RED_SIZE,
                                             &r);
            (void)egl->fn.eglGetConfigAttrib(ed, cands[i], EGLC_GREEN_SIZE,
                                             &g);
            (void)egl->fn.eglGetConfigAttrib(ed, cands[i], EGLC_BLUE_SIZE,
                                             &b);
            (void)egl->fn.eglGetConfigAttrib(ed, cands[i], EGLC_ALPHA_SIZE,
                                             &a);
            if (r == 8 && g == 8 && b == 8 && a == 0) {
                egl->config = cands[i];
                break;
            }
        }
    }

    /* The SHARE-ROOT context (the 1.0.0 fix, live-caught by the §19
     * trace rig): one context that is never made current — it exists
     * only to anchor the share GROUP every per-window context joins
     * at creation (eglCreateContext's share parameter). The quad
     * program and VBO are minted ONCE under the first window's
     * context and must be valid objects in EVERY window's context:
     * without a shared group the names live in the first context's
     * private namespace and every LATER window's glUseProgram fails
     * with GL_INVALID_VALUE — exactly what the trace caught on the
     * rig (every second window silently downgrading since the lane
     * landed). The root is "surfaceless" by construction: it never
     * needs a surface because it is never current, so no
     * EGL_KHR_surfaceless_context is required. It dies LAST, in
     * fdk_wayland_egl_shutdown, after every window context is gone —
     * the group's objects (the program, the VBO, any graveyard
     * leftovers) are swept with it or by eglTerminate. */
    const EGLint root_attr[] = { EGLC_CONTEXT_CLIENT_VERSION, 2,
                                 EGLC_NONE };
    egl->root_ctx = egl->fn.eglCreateContext(ed, egl->config,
                                             EGLC_NO_CONTEXT, root_attr);
    if (egl->root_ctx == EGLC_NO_CONTEXT) {
        const unsigned int ce = (unsigned)egl->fn.eglGetError();
        FDK_DEBUG("share-root eglCreateContext failed (0x%x, %s) — CPU "
                  "present path only",
                  ce, lane_egl_err_name(ce));
        (void)egl->fn.eglTerminate(ed);
        egl->egl_display = EGLC_NO_DISPLAY;
        fdk_free(egl);
        return NULL;
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
    GLES_RESOLVE(glDisableVertexAttribArray, "glDisableVertexAttribArray");
    GLES_RESOLVE(glVertexAttribPointer, "glVertexAttribPointer");
    GLES_RESOLVE(glGenBuffers, "glGenBuffers");
    GLES_RESOLVE(glBindBuffer, "glBindBuffer");
    GLES_RESOLVE(glBufferData, "glBufferData");
    GLES_RESOLVE(glGenTextures, "glGenTextures");
    GLES_RESOLVE(glBindTexture, "glBindTexture");
    GLES_RESOLVE(glTexImage2D, "glTexImage2D");
    GLES_RESOLVE(glTexSubImage2D, "glTexSubImage2D");
    GLES_RESOLVE(glDeleteTextures, "glDeleteTextures");
    GLES_RESOLVE(glCheckFramebufferStatus, "glCheckFramebufferStatus");
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
     * deletion, which is the honest process-teardown answer. The
     * share-root context dies HERE, after the windows: destroying it
     * earlier would still be legal (the group outlives any one
     * member) but this order keeps the invariant obvious.
     *
     * THE TAIL CALL (the 2026-11-01 exit-leak report): eglTerminate
     * alone is NOT the end of the canonical sequence — EGL keeps
     * PER-THREAD state (the current binding, a driver thread record)
     * alive after it, and libEGL_nvidia allocates that thread record
     * (256 B in 4 allocations on the GTX 1050 Ti, frames entirely
     * inside libEGL_nvidia.so.0+0xc94e1) and frees it only when
     * eglReleaseThread runs. FDK called EGL from exactly one thread
     * (the main thread — the platform's own threading contract), so
     * ONE release here retires that state before the leak checker
     * runs; the call is also the textbook tail of the teardown
     * sequence (EGL 1.5 §3.2: after eglTerminate, a thread that used
     * the display should call eglReleaseThread). */
    if (egl == NULL) {
        return;
    }
    wlgl_teardown_count++;
    const int trace = wlgl_trace_enabled();
    if (egl->egl_display != EGLC_NO_DISPLAY) {
        if (egl->root_ctx != EGLC_NO_CONTEXT) {
            if (trace) {
                FDK_INFO("GL trace: teardown — eglDestroyContext(share "
                         "root)");
            }
            (void)egl->fn.eglDestroyContext(egl->egl_display,
                                            egl->root_ctx);
            egl->root_ctx = EGLC_NO_CONTEXT;
        }
        if (trace) {
            FDK_INFO("GL trace: teardown — eglTerminate");
        }
        (void)egl->fn.eglTerminate(egl->egl_display);
    }
    if (egl->fn.eglReleaseThread != NULL) {
        wlgl_release_thread_count++;
        if (trace) {
            FDK_INFO("GL trace: teardown — eglReleaseThread");
        }
        (void)egl->fn.eglReleaseThread();
    } else if (trace) {
        FDK_INFO("GL trace: teardown — eglReleaseThread unavailable "
                 "(pre-EGL-1.2 loader) — skipped");
    }
    /* NOTE: the LIBRARY release is NOT here — it runs from the
     * connection's post-wl_display_disconnect tail
     * (fdk_wayland_egl_libraries_release): the EGL dlclose is only
     * safe once the wl_display is dead, mirroring the X11 lane's
     * XCloseDisplay rule (see wayland_connection.c). */
    fdk_free(egl);
}

void fdk_wayland_egl_libraries_release(void) {
    /* Round-7 exit-leak companion: NVIDIA's per-display driver
     * objects (the 4x256 B at +0xc94e1) survive eglTerminate +
     * eglReleaseThread; they free at the library's dlclose
     * destructors. Called by the connection AFTER
     * wl_display_disconnect — the EGL Wayland-platform caches are
     * keyed to the (now dead) display, so unwalking them is
     * impossible. dlopen refcounting makes a later re-load correct
     * (a re-created context re-resolves the whole table). GLVND
     * note: libEGL.so.1 is the vendor dispatcher; its refs to
     * libEGL_nvidia drop with it — which is what retires the
     * libdbus-1 allocations NVIDIA pulls in transitively too. */
    if (g_wlgl_library != NULL) {
        (void)dlclose(g_wlgl_library);
        g_wlgl_library = NULL;
    }
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

/* ---- THE PROBE (the 1.0.0 fix): ask the driver, not the string ----
 *
 * The GTX 1050 Ti report proved extension strings are not truth: both
 * extensions advertised, the fast path chosen, the first present
 * still failed with an unnamed GL error. The ES2 spec itself explains
 * how that happens — GL_EXT_texture_format_BGRA8888's table adds the
 * (internalformat GL_BGRA_EXT, format GL_BGRA_EXT, UNSIGNED_BYTE)
 * row, while internalformat GL_RGBA paired with format GL_BGRA_EXT is
 * in NO table (ES2's unsized-format table pairs GL_RGBA with GL_RGBA
 * only); Mesa accepts the mixed pair, NVIDIA's strict validator
 * rejects it. The same class of trap hides in GL_UNPACK_ROW_LENGTH:
 * setting it AT ALL (even to 0) is GL_INVALID_ENUM on a context
 * without GL_EXT_unpack_subimage.
 *
 * So the lane now PROBES: with the first context current, each shape
 * gets a real 4x4 upload (both internalformat spellings tried for the
 * BGRA shapes — spec-legal first), a 2x2 sub-rect update, and the
 * ROW_LENGTH pixelstore where the shape needs it; every call drained.
 * The first shape whose every call drains clean becomes the
 * connection's shape, and the validated internalformat spelling is
 * the one present() uses. A readback is NOT possible on the probe
 * (core GLES2 has no glGetTexImage; an FBO round-trip would assume
 * ES3/EXT_framebuffer_object) — the first-frame self-check already
 * proves the rendered pixels end-to-end, so the probe's job is only
 * the driver's format/enum ACCEPTANCE, which is exactly what raises
 * the unnamed error.
 *
 * FDK_GL_PROBE_SKIP=<n> (test seam) rejects the first n shapes as if
 * the driver refused them; FDK_GL_CORE_ONLY pins the pure-core shape
 * as before. If every shape is rejected the lane refuses for the
 * connection — after logging each probe's verdict. */
static int lane_probe_shape(struct fdk_wayland_egl *egl, int shape,
                            int *internal_rgba_out) {
    if (lane_shape_uses_bgra(shape) && !egl->has_bgra_ext) {
        return 0; /* extension not advertised: not even tryable */
    }
    if (lane_shape_uses_rowlen(shape) && !egl->has_rowlen_ext) {
        return 0;
    }
    const int core_only = getenv("FDK_GL_CORE_ONLY") != NULL;
    if (core_only && shape != WLGL_SHAPE_TWIN_PACKED) {
        return 0;
    }

    /* Known patch bytes: BGRA uploads read them as [B,G,R,X]; the
     * twin path swizzles first — the probe only cares that the CALL
     * is accepted, and these bytes are what a follow-up readback (the
     * first-frame self-check) can attribute. */
    static const fdk_u32 patch[16] = {
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFFFFu,
        0x0000FF00u, 0x00FF0000u, 0x00FFFFFFu, 0x000000FFu,
        0x000000FFu, 0x00FFFFFFu, 0x00FF0000u, 0x0000FF00u,
        0x00FFFFFFu, 0x000000FFu, 0x0000FF00u, 0x00FF0000u
    };
    /* The twin form of the patch (R<->B swizzled) for the RGBA
     * shapes — lane_swizzle_rgba's byte math, pinned driverless in
     * test_gl_lane_fallback.c. */
    fdk_u32 twin[16];
    for (int i = 0; i < 16; i++) {
        const fdk_u32 p = patch[i];
        twin[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0x000000FFu) |
                  ((p & 0x000000FFu) << 16);
    }

    unsigned int tex = 0;
    egl->fn.glGenTextures(1, &tex);
    lane_setup_texture(egl, tex);
    egl->fn.glPixelStorei(GLC_UNPACK_ALIGNMENT, 1);
    (void)lane_first_gl_error(egl); /* the probe starts from a clean
                                     * error flag */

    char op[96];
    int ok = 1;
    unsigned int err = 0;

    /* The ROW_LENGTH pixelstore: only touched when the extension
     * exists (the ES2 spec makes the pname itself illegal without
     * GL_EXT_unpack_subimage — the pre-probe code set it to 0
     * unconditionally, raising GL_INVALID_ENUM on strict drivers
     * even on the fallback shapes). */
    if (egl->has_rowlen_ext) {
        (void)snprintf(op, sizeof(op), "glPixelStorei(UNPACK_ROW_LENGTH, %d)",
                       lane_shape_uses_rowlen(shape) ? 4 : 0);
        egl->fn.glPixelStorei(GLC_UNPACK_ROW_LENGTH,
                              lane_shape_uses_rowlen(shape) ? 4 : 0);
        err = lane_first_gl_error(egl);
        if (err != 0) {
            FDK_INFO("GPU lane upload probe: %s rejected by the driver — "
                     "0x%x (%s) at %s",
                     lane_shape_name(shape), err, lane_gl_err_name(err), op);
            ok = 0;
        }
    }

    if (ok) {
        const fdk_u32 *data = lane_shape_uses_bgra(shape) ? patch : twin;
        const unsigned int fmt =
            lane_shape_uses_bgra(shape) ? GLC_BGRA : GLC_RGBA;
        /* The BGRA shapes try both internalformat spellings — the
         * spec row first (GL_BGRA_EXT/GL_BGRA_EXT), the ES2-lenient
         * mixed pair second (GL_RGBA internal + GL_BGRA_EXT format):
         * which one the driver accepts is THE NVIDIA question, and
         * only the driver can answer it. */
        const unsigned int internals[2] = { GLC_BGRA, GLC_RGBA };
        const int n_internals = lane_shape_uses_bgra(shape) ? 2 : 1;
        int accepted = -1;
        for (int i = 0; i < n_internals && accepted < 0; i++) {
            (void)snprintf(op, sizeof(op),
                           "glTexImage2D(4x4, internal=%s, fmt=%s)",
                           internals[i] == GLC_BGRA ? "GL_BGRA_EXT"
                                                    : "GL_RGBA",
                           lane_fmt_name(fmt));
            egl->fn.glTexImage2D(GLC_TEXTURE_2D, 0,
                                 (int)internals[i], 4, 4, 0, fmt,
                                 GLC_UNSIGNED_BYTE, data);
            err = lane_first_gl_error(egl);
            if (err == 0) {
                accepted = i;
            } else {
                FDK_INFO("GPU lane upload probe: %s rejected the triple "
                         "internal=%s + fmt=%s — 0x%x (%s) at %s",
                         lane_shape_name(shape),
                         internals[i] == GLC_BGRA ? "GL_BGRA_EXT"
                                                  : "GL_RGBA",
                         lane_fmt_name(fmt), err, lane_gl_err_name(err), op);
            }
        }
        if (accepted < 0) {
            ok = 0;
        } else {
            *internal_rgba_out = (accepted == 1);
            /* The sub-rect update (the partial-damage path's call):
             * same format through glTexSubImage2D. */
            (void)snprintf(op, sizeof(op), "glTexSubImage2D(2x2 at 1,1)");
            egl->fn.glTexSubImage2D(GLC_TEXTURE_2D, 0, 1, 1, 2, 2, fmt,
                                    GLC_UNSIGNED_BYTE, data + 5);
            err = lane_first_gl_error(egl);
            if (err != 0) {
                FDK_INFO("GPU lane upload probe: %s rejected the sub-rect "
                         "update — 0x%x (%s) at %s",
                         lane_shape_name(shape), err,
                         lane_gl_err_name(err), op);
                ok = 0;
            }
        }
    }

    /* The probe's texture dies now (the context is current; a clean
     * glDeleteTextures under the very state we just validated). */
    egl->fn.glDeleteTextures(1, &tex);
    (void)lane_first_gl_error(egl);
    return ok;
}

/* Runs the probe ladder once per connection (first window's
 * engagement): fills egl->shape / shape_rejected / bgra_internal_rgba.
 * Returns 0 when some shape is available, -1 when every shape was
 * rejected (the caller refuses the lane — the honest answer when a
 * driver accepts NO core-legal upload at all). */
static int lane_probe_upload_shapes(struct fdk_wayland_egl *egl) {
    long skip = 0;
    const char *sk = getenv("FDK_GL_PROBE_SKIP");
    if (sk != NULL && sk[0] != '\0') {
        skip = strtol(sk, NULL, 10);
        if (skip < 0) {
            skip = 0;
        }
    }
    for (int s = 0; s < WLGL_SHAPE_COUNT; s++) {
        if (s < skip) {
            egl->shape_rejected |= (unsigned int)1u << s;
            FDK_INFO("GPU lane upload probe: %s skipped "
                     "(FDK_GL_PROBE_SKIP)",
                     lane_shape_name(s));
            continue;
        }
        int internal_rgba = 0;
        if (lane_probe_shape(egl, s, &internal_rgba)) {
            egl->shape = s;
            egl->bgra_internal_rgba = internal_rgba;
            return 0;
        }
        egl->shape_rejected |= (unsigned int)1u << s;
    }
    egl->shape = -1;
    return -1;
}

/* The next un-rejected shape above `from` (the present ladder's
 * rotation), or -1 when the ladder is exhausted. */
static int lane_next_shape(const struct fdk_wayland_egl *egl, int from) {
    for (int s = from + 1; s < WLGL_SHAPE_COUNT; s++) {
        if ((egl->shape_rejected & ((unsigned int)1u << s)) == 0) {
            return s;
        }
    }
    return -1;
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
    /* Share the ROOT: this window's context joins the connection's
     * share group, so the quad program/VBO (minted once, under the
     * first window's context) are valid objects here — the 1.0.0
     * fix for the every-second-window glUseProgram failure the trace
     * rig caught (see the try_load root comment). */
    EGLContext ctx = egl->fn.eglCreateContext(egl->egl_display, egl->config,
                                              egl->root_ctx, ctx_attr);
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
        const unsigned int me = (unsigned)egl->fn.eglGetError();
        FDK_WARN("eglMakeCurrent failed (0x%x, %s) — CPU present path for "
                  "this connection",
                 me, lane_egl_err_name(me));
        (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
        (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
        wl_egl_window_destroy(wl_win);
        egl->refused = 1;
        return NULL;
    }
    /* Setup-stage drain #1: the context's error flag at birth plus
     * anything the renderer/extension reads raised — consumed BEFORE
     * the program build so later stages only see their own errors. */
    lane_stale_drain(egl, "lane engagement (first makeCurrent)");

    /* Renderer policy: the FIRST context decides for the connection
     * (driver-wide string). Software rasterizers stay on wl_shm. */
    char gl_vendor[96] = { 0 };
    char gl_version[96] = { 0 };
    if (egl->renderer[0] == '\0') {
        const unsigned char *r = egl->fn.glGetString(GLC_RENDERER);
        if (r != NULL) {
            (void)snprintf(egl->renderer, sizeof(egl->renderer), "%s", r);
        }
    }
    if (wlgl_trace_enabled()) {
        /* The engagement dump's GL strings — read NOW (the context
         * unbinds before the dump is emitted below). */
        const unsigned char *v = egl->fn.glGetString(GLC_VENDOR);
        const unsigned char *ver = egl->fn.glGetString(GLC_VERSION);
        if (v != NULL) {
            (void)snprintf(gl_vendor, sizeof(gl_vendor), "%s", v);
        }
        if (ver != NULL) {
            (void)snprintf(gl_version, sizeof(gl_version), "%s", ver);
        }
    }

    /* UPLOAD PATH capabilities (the 1.0.0 NVIDIA fix): decided once
     * per connection, off this first context's ES2 extension string.
     *
     * The GTX 1050 Ti report (2026-10-31, labwc + the proprietary
     * driver): the lane engaged (renderer string read fine, program
     * linked) and the FIRST present failed with a GL error — the
     * upload used GL_BGRA + GL_UNPACK_ROW_LENGTH, which are NOT
     * core GLES2: BGRA needs GL_EXT_texture_format_BGRA8888 and
     * ROW_LENGTH needs GL_EXT_unpack_subimage (or ES3). Mesa's
     * swrast exposes both, which is exactly why every rig passed;
     * the NVIDIA ES2 context rejects them (GL_INVALID_ENUM et al).
     * The fix is the honest core-legal form: detect the extensions,
     * fall back to an RGBA twin (CPU R<->B swizzle) and per-rect row
     * packing when they are absent — present() implements all four
     * combinations. FDK_GL_CORE_ONLY=1 pins the fallback shapes on
     * any driver (the rig's deterministic leg). */
    if (!egl->has_bgra_ext && !egl->has_rowlen_ext &&
        egl->renderer[0] != '\0') {
        const unsigned char *ext = egl->fn.glGetString(GLC_EXTENSIONS);
        if (ext != NULL) {
            if (fdk_wlgl_strcasestr((const char *)ext, WLGL_EXT_BGRA) !=
                NULL) {
                egl->has_bgra_ext = 1;
            }
            if (fdk_wlgl_strcasestr((const char *)ext, WLGL_EXT_ROWLEN) !=
                NULL) {
                egl->has_rowlen_ext = 1;
            }
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

    /* Setup-stage drain #2: the extension/string reads are done —
     * whatever they raised stays here. */
    lane_stale_drain(egl, "lane engagement (extension detection)");

    /* THE PROBE (the 1.0.0 fix): once per connection, with this
     * first context current — every shape's upload calls tried for
     * real, and the first the DRIVER accepts becomes the
     * connection's shape. Extension strings are input, not verdict
     * (the GTX 1050 Ti report: both extensions advertised, the fast
     * path chosen, the first present still failed). All shapes
     * rejected = the lane refuses for this connection — honestly,
     * with every probe's verdict already logged above. */
    if (!egl->probe_done) {
        egl->probe_done = 1;
        if (lane_probe_upload_shapes(egl) != 0) {
            FDK_WARN("GPU lane upload probe: every upload shape was "
                     "rejected by the driver (see each probe line) — "
                     "refusing the GPU present lane for this "
                     "connection");
            (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                         EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
            (void)egl->fn.eglDestroyContext(egl->egl_display, ctx);
            (void)egl->fn.eglDestroySurface(egl->egl_display, surface);
            wl_egl_window_destroy(wl_win);
            egl->refused = 1;
            wlgl_policy_stamp(1, getenv("FDK_NO_GL") != NULL, forced);
            return NULL;
        }
    }
    /* Setup-stage drain #2b: the probe drained per call, but the
     * round trip is a stage of its own. */
    lane_stale_drain(egl, "lane engagement (upload probe)");

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
    /* Setup-stage drain #3: the program build's errors (shader
     * compile failures log their own INFOLOG lines; any stray flag
     * is named here, not blamed on the first present). */
    lane_stale_drain(egl, "lane engagement (quad program build)");

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
    ewin->rgba_twin = NULL;
    ewin->pack_scratch = NULL;
    ewin->pack_capacity = 0;
    ewin->tex_ok = 0;
    ewin->presents = 0;
    ewin->fail_at = 0;
    ewin->poison_first = getenv("FDK_GL_POISON_FIRST") != NULL;
    ewin->lane_op[0] = '\0';
    ewin->lane_checked = 0;
    ewin->lane_tracing = 0;
    ewin->lane_stage = "present";

    const char *fail_at = getenv("FDK_GL_FAIL_AT");
    if (fail_at != NULL && fail_at[0] != '\0') {
        long v = strtol(fail_at, NULL, 10);
        ewin->fail_at = v > 0 ? v : 0;
    }

    egl->fn.glGenTextures(1, &ewin->tex);
    lane_setup_texture(egl, ewin->tex);
    egl->fn.glPixelStorei(GLC_UNPACK_ALIGNMENT, 1);
    /* Setup-stage drain #4: texture mint + sampler state + the
     * alignment pixelstore — the last setup calls before unbind. */
    lane_stale_drain(egl, "lane engagement (texture mint)");

    /* Unbind: another window's init/present rebinds as needed. */
    (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                 EGLC_NO_SURFACE, EGLC_NO_CONTEXT);

    FDK_INFO("GPU present lane active on Wayland (renderer \"%s\")",
             egl->renderer[0] != '\0' ? egl->renderer : "unknown");
    FDK_INFO("GPU lane upload path: %s — driver-probed (a real 4x4 "
             "upload picked this shape; extensions BGRA8888=%d "
             "unpack_subimage=%d, BGRA internalformat=%s)",
             lane_shape_name(egl->shape), egl->has_bgra_ext,
             egl->has_rowlen_ext,
             egl->bgra_internal_rgba ? "GL_RGBA" : "GL_BGRA_EXT");
    /* The surface-format facts, the EGL twin of the X11 lane's
     * "fdk-gl: fbconfig srgb=... doublebuf=..." line (the 2026-11-11
     * audit: a Wayland session must answer the same sRGB question).
     * EGL_GL_COLORSPACE needs EGL 1.5; an older stack honestly
     * reports "query-failed" rather than guessing. */
    {
        EGLint bufsz = 0, alphasz = 0, depthsz = 0, stensz = 0;
        EGLint cs = 0;
        const char *cs_name = "query-failed";
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_BUFFER_SIZE, &bufsz);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_ALPHA_SIZE, &alphasz);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_DEPTH_SIZE, &depthsz);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_STENCIL_SIZE, &stensz);
        if (egl->fn.eglQuerySurface(egl->egl_display, ewin->surface,
                                    EGLC_GL_COLORSPACE, &cs)) {
            cs_name = cs == EGLC_GL_COLORSPACE_SRGB ? "sRGB"
                    : cs == EGLC_GL_COLORSPACE_LINEAR ? "linear"
                    : "other";
        }
        FDK_INFO("fdk-egl: config buffersize=%d alpha=%d depth=%d "
                 "stencil=%d colorspace=%s (EGL_GL_COLORSPACE; "
                 "colorspace conversion is NOT requested — pixels go "
                 "to the compositor as painted)",
                 bufsz, alphasz, depthsz, stensz, cs_name);
    }
    /* FDK_GL_TRACE's one-per-connection engagement dump (TASK 1b):
     * everything a desktop report needs to interpret a lane failure
     * — the EGL stack's identity, the config the lane rides, the
     * context's API+version, the GL strings, the extension flags,
     * and the wl_egl_window's attached size at engagement (the
     * first-present size mismatch is a classic NVIDIA-era failure
     * shape; the attached size vs the present size is named again
     * per traced present). */
    if (wlgl_trace_enabled() && !egl->engaged_dumped) {
        egl->engaged_dumped = 1;
        const char *ev = egl->fn.eglQueryString(egl->egl_display,
                                                 EGLC_VENDOR);
        const char *ever = egl->fn.eglQueryString(egl->egl_display,
                                                  EGLC_VERSION);
        const char *ecap = egl->fn.eglQueryString(egl->egl_display,
                                                  EGLC_CLIENT_APIS);
        FDK_INFO("GL trace: engagement — EGL vendor \"%s\", version "
                 "\"%s\", client APIs \"%s\"",
                 ev != NULL ? ev : "(null)",
                 ever != NULL ? ever : "(null)",
                 ecap != NULL ? ecap : "(null)");
        const char *eext = egl->fn.eglQueryString(egl->egl_display,
                                                  EGLC_EXTENSIONS);
        FDK_INFO("GL trace: engagement — EGL display extensions: "
                 "surfaceless_context=%d, bind_wayland_display=%d",
                 eext != NULL &&
                         fdk_wlgl_strcasestr(eext,
                                             "EGL_KHR_surfaceless_context")
                     ? 1
                     : 0,
                 eext != NULL &&
                         fdk_wlgl_strcasestr(eext,
                                             "EGL_WL_bind_wayland_display")
                     ? 1
                     : 0);
        EGLint cid = 0, rs = 0, gs = 0, bs = 0, as = 0, st = 0, rt = 0;
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_CONFIG_ID, &cid);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_RED_SIZE, &rs);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_GREEN_SIZE, &gs);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_BLUE_SIZE, &bs);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_ALPHA_SIZE, &as);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_SURFACE_TYPE, &st);
        (void)egl->fn.eglGetConfigAttrib(egl->egl_display, egl->config,
                                         EGLC_RENDERABLE_TYPE, &rt);
        FDK_INFO("GL trace: engagement — EGLConfig id=%d "
                 "R%d G%d B%d A%d surface_type=0x%x renderable=0x%x",
                 cid, rs, gs, bs, as, (unsigned)st, (unsigned)rt);
        EGLint ctype = 0, cver = 0;
        (void)egl->fn.eglQueryContext(egl->egl_display, ctx,
                                      EGLC_CONTEXT_CLIENT_TYPE, &ctype);
        (void)egl->fn.eglQueryContext(egl->egl_display, ctx,
                                      EGLC_CONTEXT_CLIENT_VERSION, &cver);
        FDK_INFO("GL trace: engagement — context client type 0x%x "
                 "(0x30A0 = EGL_OPENGL_ES_API), client version %d",
                 (unsigned)ctype, cver);
        FDK_INFO("GL trace: engagement — GL vendor \"%s\", renderer "
                 "\"%s\", version \"%s\"",
                 gl_vendor[0] != '\0' ? gl_vendor : "(null)",
                 egl->renderer[0] != '\0' ? egl->renderer : "(null)",
                 gl_version[0] != '\0' ? gl_version : "(null)");
        FDK_INFO("GL trace: engagement — upload shape %d (%s) "
                 "probe-validated; extensions: BGRA8888=%d, "
                 "unpack_subimage=%d (FDK_GL_CORE_ONLY=%d), "
                 "rejected-mask 0x%x",
                 egl->shape, lane_shape_name(egl->shape),
                 egl->has_bgra_ext, egl->has_rowlen_ext,
                 getenv("FDK_GL_CORE_ONLY") != NULL,
                 egl->shape_rejected);
        if (wl_egl_window_get_attached_size != NULL) {
            int aw = 0, ah = 0;
            if (wl_egl_window_get_attached_size(wl_win, &aw, &ah) != 0) {
                aw = -1;
                ah = -1;
            }
            FDK_INFO("GL trace: engagement — wl_egl_window created "
                     "%dx%d, attached size now %dx%d",
                     (int)w, (int)h, aw, ah);
        }
    }
    /* §20 relevance note (once per process): whether the compositor
     * advertises wp_linux_drm_syncobj_manager_v1 decides how much the
     * single-owner rule is load-bearing HERE. Advertised + a driver
     * that wraps (NVIDIA): every attaching commit on this surface
     * needs an acquire point — EGL's commits carry them, FDK's own
     * wl_shm commits are refused (wayland_window.c). Not advertised
     * (this sandbox's sway/pixman, most VM rigs): the rule still
     * holds — one presenter per surface is correct independent of
     * the sync protocol — but nothing would kill the connection over
     * it. pwindow carries the connection for the flag. */
    if (!egl->syncobj_note_logged) {
        egl->syncobj_note_logged = 1;
        FDK_INFO("GL lane: compositor %s wp_linux_drm_syncobj_manager_v1 "
                 "— %s",
                 pwindow->conn->syncobj_manager_advertised
                     ? "advertises"
                     : "does not advertise",
                 pwindow->conn->syncobj_manager_advertised
                     ? "a driver wrapping this surface in the "
                       "explicit-sync role makes FDK's wl_shm-commit "
                       "refusal load-bearing (the single-owner rule)"
                     : "the single-owner rule still holds (one "
                       "presenter per surface), but no explicit-sync "
                       "role can enforce it");
    }
    return ewin;
}

/* FDK_GL_TRACE: the teardown ladder of one window's lane state —
 * the same observation contract the present trace holds (the §22
 * exit-leak report: a teardown question must be answerable from the
 * log alone, without a debugger on the owner's machine). */
static void lane_trace_teardown(const char *what) {
    if (wlgl_trace_enabled()) {
        FDK_INFO("GL trace: window teardown — %s", what);
    }
}

void fdk_wayland_egl_window_destroy(struct fdk_wayland_egl *egl,
                                    struct fdk_wayland_egl_window *ewin) {
    if (ewin == NULL) {
        return;
    }
    /* The CANONICAL teardown order (the 1.0.0 NVIDIA fix — the old
     * code bound the doomed surface's context with a makeCurrent
     * pair purely as a ritual and THEN destroyed things; binding a
     * context whose driver state is mid-failure is exactly the kind
     of driver edge an EGL teardown must never poke):
     *
     *   1. ONE eglMakeCurrent(NO_SURFACE, NO_CONTEXT) — releases the
     *      binding present() may have left (a current context/surface
     *      pair must be unbound before its objects are destroyed —
     *      and destroying objects that are still bound is undefined
     *      territory on some drivers).
     *   2. eglDestroySurface — BEFORE wl_egl_window_destroy: the
     *      EGLSurface references the wl_egl_window (the platform's
     *      documented lifetime rule).
     *   3. eglDestroyContext — the context shares nothing FDK owns
     *      (program/VBO are deleted by eglTerminate/shutdown).
     *   4. wl_egl_window_destroy — frees the client-side wrapper;
     *      no protocol request is involved at any step, so the
     *      wl_surface's committed/pending state stays exactly what
     *      the wl_shm downgrade path expects to inherit.
     *
     * No GLES entry point runs here: the texture is deleted with the
     * context, and calling GL without a current context is undefined.
     * All shutdown paths (normal, downgrade, dead connection) go
     * through this one order.
     *
     * THE SHARE-GROUP CORRECTION (1.0.0): with every context sharing
     * the connection's root, the texture does NOT die with this
     * context (share-group members die with the GROUP) — it is queued
     * in the connection's graveyard and deleted by the next present
     * on any window, when a context is legitimately current. A full
     * ring simply drops to eglTerminate's sweep (bounded staleness,
     * never unbounded growth); 16 slots cover any realistic window
     * churn between presents. */
    if (egl != NULL && ewin->tex != 0 &&
        egl->graveyard_count < (int)(sizeof(egl->graveyard) /
                                     sizeof(egl->graveyard[0]))) {
        egl->graveyard[egl->graveyard_count++] = ewin->tex;
    }
    if (egl != NULL && egl->egl_display != EGLC_NO_DISPLAY) {
        lane_trace_teardown("eglMakeCurrent(NO_SURFACE, NO_CONTEXT)");
        (void)egl->fn.eglMakeCurrent(egl->egl_display, EGLC_NO_SURFACE,
                                     EGLC_NO_SURFACE, EGLC_NO_CONTEXT);
        lane_trace_teardown("eglDestroySurface(window)");
        (void)egl->fn.eglDestroySurface(egl->egl_display, ewin->surface);
        lane_trace_teardown("eglDestroyContext(window)");
        (void)egl->fn.eglDestroyContext(egl->egl_display, ewin->ctx);
    }
    lane_trace_teardown("wl_egl_window_destroy");
    wl_egl_window_destroy(ewin->wl_win);
    if (ewin->rgba_twin != NULL) {
        fdk_free(ewin->rgba_twin);
    }
    if (ewin->pack_scratch != NULL) {
        fdk_free(ewin->pack_scratch);
    }
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
    /* The RGBA twin rides the same size lifecycle (the no-BGRA
     * upload path's swizzle target — see present()). */
    if (ewin->rgba_twin != NULL) {
        fdk_free(ewin->rgba_twin);
        ewin->rgba_twin = NULL;
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

/* The present attempt's verdict classes (the ladder's input): a GL
 * error at a named call and a swap failure are LADDER-able (rotate
 * the upload shape, recreate the texture, retry once per shape);
 * everything else (the self-check's persistent mismatch — a process
 * verdict; a staging/twin allocation failure — OOM; an unsupported
 * default framebuffer) is fatal: no shape can fix it. */
enum {
    LANE_PRESENT_OK = 0,
    LANE_PRESENT_GL = 1,
    LANE_PRESENT_SWAP = 2,
    LANE_PRESENT_FATAL = 3
};

/* LANE_GL_OP (the TASK-1 attribution macro): runs one lane GL call
 * with its description recorded in lane_op (the "at <call>" of the
 * failure report), the FDK_GL_TRACE line when this present is
 * traced, and — while this present is checked call-by-call (the
 * first present at a size, or any traced present) — an immediate
 * error drain so a failure is attributed to the exact call that
 * raised it, never to the present as a whole. Requires `ewin` in
 * scope; returns -1 from the enclosing function on the first error
 * after the loud report. */
#define LANE_GL_OP(desc, expr)                                           \
    do {                                                                 \
        (void)snprintf(ewin->lane_op, sizeof(ewin->lane_op), "%s",       \
                       desc);                                            \
        (expr);                                                          \
        if (ewin->lane_checked) {                                        \
            const unsigned int lane_err_ =                               \
                lane_first_gl_error(ewin->egl);                          \
            if (ewin->lane_tracing) {                                    \
                FDK_INFO("GL trace: present#%d %s -> %s (0x%x)",         \
                         ewin->presents + 1, desc,                       \
                         lane_err_ == 0                                  \
                             ? "clean"                                    \
                             : lane_gl_err_name(lane_err_),              \
                         lane_err_);                                     \
            }                                                            \
            if (lane_err_ != 0) {                                        \
                lane_report_call_error(ewin, lane_err_,                  \
                                       ewin->lane_stage);                \
                return -1;                                              \
            }                                                            \
        }                                                                \
    } while (0)

/* The draw: program + quad + 1:1 texture. The quad's texture must
 * be bound on unit 0 at DRAW time (not just at upload time) — the
 * program's sampler is unit 0, and glBindTexture at upload leaves
 * the right texture bound there; this re-asserts it defensively.
 *
 * glViewport at EVERY draw (the 1.0.0 fix): GLES2's initial viewport
 * comes from the surface size at the context's first makeCurrent —
 * it does NOT follow wl_egl_window resizes, and relying on the
 * initial value also leaves the draw at the mercy of a driver's
 * first-binding bookkeeping. One explicit call pins the viewport to
 * this frame's size; it is free and it is what every real engine
 * does.
 *
 * Every call rides LANE_GL_OP (the TASK-1 attribution) so a draw-time
 * failure names its own call — the GTX 1050 Ti report's unnamed
 * "GL error during GPU present" is this function's calls as often
 * as the upload's. */
static int draw_fullscreen_quad(struct fdk_wayland_egl_window *ewin,
                                 int w, int h) {
    struct fdk_wayland_egl *egl = ewin->egl;
    char desc[128];
    (void)snprintf(desc, sizeof(desc), "glViewport(0, 0, %d, %d)", w, h);
    LANE_GL_OP(desc, egl->fn.glViewport(0, 0, w, h));
    LANE_GL_OP("glUseProgram(quad program)",
               egl->fn.glUseProgram(egl->program));
    (void)snprintf(desc, sizeof(desc), "glUniform2f(u_size, %d, %d)", w, h);
    LANE_GL_OP(desc,
               egl->fn.glUniform2f(egl->program_u_size, (float)w,
                                   (float)h));
    LANE_GL_OP("glUniform1i(u_tex, 0)",
               egl->fn.glUniform1i(egl->program_u_tex, 0));
    LANE_GL_OP("glActiveTexture(TEXTURE0)",
               egl->fn.glActiveTexture(GLC_TEXTURE0));
    LANE_GL_OP("glBindBuffer(quad VBO)",
               egl->fn.glBindBuffer(GLC_ARRAY_BUFFER, egl->quad_vbo));
    LANE_GL_OP("glEnableVertexAttribArray(a_pos)",
               egl->fn.glEnableVertexAttribArray(
                   (unsigned int)egl->program_a_pos));
    LANE_GL_OP("glVertexAttribPointer(a_pos, 2, FLOAT)",
               egl->fn.glVertexAttribPointer(
                   (unsigned int)egl->program_a_pos, 2, GLC_FLOAT, 0, 0,
                   NULL));
    /* Disable the attribute arrays this quad does not use. The paint
     * engine's lane leaves arrays 1..6 enabled (7 arrays, stride 96)
     * pointing at ITS VBO — after a 1-quad flush that buffer holds 24
     * bytes, and THIS 6-vertex draw would read up to 576 bytes through
     * the stale arrays. Desktop-GL drivers don't enforce the read
     * range (the X11 lane stayed green); strict ES 3.x raises
     * GL_INVALID_OPERATION at the draw — the 2026-11-11 wayland-lane
     * audit caught exactly that (every present after a small flush
     * queued 0x502 into the next flush's error check). Disabling is
     * also correct on desktop (a disabled unused array is a no-op). */
    for (unsigned int ai = 0; ai < 16; ai++) {
        if (ai == (unsigned int)egl->program_a_pos) {
            continue;
        }
        egl->fn.glDisableVertexAttribArray(ai);
    }
    LANE_GL_OP("glDrawArrays(TRIANGLES, 0, 6)",
               egl->fn.glDrawArrays(GLC_TRIANGLES, 0, 6));
    return LANE_PRESENT_OK;
}

/* The RGBA twin's swizzle pass (the no-BGRA upload path): pure —
 * exposed through fdk__wayland_egl_test_swizzle for the fallback
 * unit test (no EGL driver needed to verify the byte math). */
static void lane_swizzle_rgba(fdk_u32 *dst, const fdk_u32 *src,
                              size_t n) {
    for (size_t i = 0; i < n; i++) {
        const fdk_u32 p = src[i];
        dst[i] = (p & 0xFF00FF00u) | ((p >> 16) & 0x000000FFu) |
                 ((p & 0x000000FFu) << 16);
    }
}

/* The tightly-packed row copy for one damaged rect (the no-ROW_LENGTH
 * sub-rect path): pure — exposed through
 * fdk__wayland_egl_test_pack_rows. */
static void lane_pack_rows(fdk_u32 *dst, const fdk_u32 *src, int stride,
                           int x, int y, int rw, int rh) {
    for (int j = 0; j < rh; j++) {
        memcpy(dst + (size_t)j * (size_t)rw,
               src + (size_t)(y + j) * (size_t)stride + (size_t)x,
               sizeof(fdk_u32) * (size_t)rw);
    }
}

/* The RGBA twin (the no-BGRA upload path): swizzles the whole
 * staging buffer R<->B into the twin — a memcpy-class pass the CPU
 * rasterizer that just produced the frame dwarfs. Returns 0 on
 * success, -1 on allocation failure (the caller downgrades; the
 * staging buffer itself is untouched, so the salvage path still
 * holds the frame). */
static int lane_ensure_rgba_twin(struct fdk_wayland_egl_window *ewin) {
    if (ewin->rgba_twin == NULL) {
        ewin->rgba_twin =
            fdk_alloc(sizeof(fdk_u32) * (size_t)ewin->w * (size_t)ewin->h);
        if (ewin->rgba_twin == NULL) {
            return -1;
        }
    }
    lane_swizzle_rgba(ewin->rgba_twin, ewin->staging,
                      (size_t)ewin->w * (size_t)ewin->h);
    return 0;
}

/* Tightly packs one damaged rect's rows into the scratch buffer
 * (the no-ROW_LENGTH sub-rect path): GL receives a buffer whose rows
 * are contiguous — the exact layout glTexSubImage2D requires when
 * UNPACK_ROW_LENGTH cannot express the window stride. Returns NULL
 * on allocation failure. */
static const fdk_u32 *lane_pack_rect(struct fdk_wayland_egl_window *ewin,
                                     const fdk_u32 *src, int w, int x, int y,
                                     int rw, int rh) {
    const size_t need = (size_t)rw * (size_t)rh;
    if (ewin->pack_scratch == NULL || ewin->pack_capacity < need) {
        fdk_u32 *fresh = fdk_alloc(sizeof(fdk_u32) * need);
        if (fresh == NULL) {
            return NULL;
        }
        if (ewin->pack_scratch != NULL) {
            fdk_free(ewin->pack_scratch);
        }
        ewin->pack_scratch = fresh;
        ewin->pack_capacity = need;
    }
    lane_pack_rows(ewin->pack_scratch, src, w, x, y, rw, rh);
    return ewin->pack_scratch;
}

static int lane_present_attempt(struct fdk_wayland_egl_window *ewin,
                                const fdk_platform_damage *damage, int w,
                                int h);

/* GPU paint-mode flush (the GPU render mode's present): make the
 * window's lane context current, issue the surface's pending GPU
 * batch, swap. No staging upload, no texture compositing — the batch
 * IS the frame. Resizes ride wl_egl_window_resize like the upload
 * path's staging realloc. Failure = FDK_ERR_UNSUPPORTED: GPU mode has
 * no CPU fallback by contract, and present reports it rather than
 * faking a frame. */
fdk_result fdk_wayland_egl_window_paint_flush(
    struct fdk_wayland_egl_window *ewin, void *surface_ptr) {
    if (ewin == NULL || surface_ptr == NULL || ewin->surface == 0 ||
        ewin->wl_win == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    fdk_surface *s = (fdk_surface *)surface_ptr;
    if (s->gpu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (s->fb.width > 0 && (s->fb.width != ewin->w ||
                            s->fb.height != ewin->h)) {
        wl_egl_window_resize(ewin->wl_win, s->fb.width,
                             s->fb.height, 0, 0);
        ewin->w = s->fb.width;
        ewin->h = s->fb.height;
        ewin->tex_ok = 0;
    }
    if (!egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                ewin->surface, ewin->ctx)) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* A pre-swap drain, the CPU lane's own discipline (see
     * present()'s "the pre-present stale drain"): an error left
     * over from the previous frame's swap or this window's init
     * must be consumed BEFORE the engine's flush, because the
     * flush's tail check marks the engine DEAD on any queued error
     * — a lane-owned error must never kill the engine (the X11 lane
     * never noticed: llvmpipe's desktop-GLX swap raises nothing). */
    {
    /* Loop, not one read: the spec queues errors INDEFINITELY —
     * one swap can leave more than one, and any survivor leaks
     * into the engine's flush (the observed rate was exactly one
     * per present, one single read matched by luck). */
        for (;;) {
            unsigned pre = egl->fn.glGetError();
            if (pre == 0) {
                break;
            }
            FDK_WARN("GL lane (paint flush): stale GL error 0x%x drained "
                     "before the engine's flush — raised by the previous "
                     "frame's swap or setup, not the flush that follows",
                     pre);
        }
    }
    fdk__gpu_flush(s);
    if (s->gpu == NULL) { /* flush marked the engine dead */
        return FDK_ERR_UNSUPPORTED;
    }
    EGLBoolean ok = egl->fn.eglSwapBuffers(egl->egl_display, ewin->surface);
    (void)wl_display_flush(egl->conn->display);
    {
        /* The post-swap drain (the CPU lane's se2 discipline), looped
         * for the same reason as the pre-flush one: every queued
         * error, not just the first. */
        for (;;) {
            unsigned post = egl->fn.glGetError();
            if (post == 0) {
                break;
            }
            FDK_WARN("GL lane (paint flush): swap queued 0x%x — consumed "
                     "here so it cannot kill the engine's next flush",
                     post);
        }
    }
    return ok ? FDK_OK : FDK_ERR_UNSUPPORTED;
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

    /* The stage name every failure report of this present carries
     * (TASK 1a): the first present, the first present at a NEW size
     * (every resize — tex_ok resets with the staging realloc), or a
     * steady-state present. */
    ewin->lane_stage = ewin->presents == 0
                           ? "the first present"
                           : (!ewin->tex_ok
                                  ? "the first present at the new size "
                                    "(resize)"
                                  : "a steady-state present");
    const int trace = lane_trace_this_present(ewin);
    /* Per-call error attribution (TASK 1a/1c): while this present is
     * a "first" (first frame, first frame at a new size) or traced,
     * every GL call is drained individually — the failure names the
     * exact call. Once the lane is trusted, one drain at the present
     * tail (attributed to the last lane call) keeps the steady-state
     * cost at a single glGetError. */
    ewin->lane_checked = ewin->presents == 0 || !ewin->tex_ok || trace;
    ewin->lane_tracing = trace;

    if (!egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                ewin->surface, ewin->ctx)) {
        const unsigned int me = (unsigned)egl->fn.eglGetError();
        FDK_WARN("eglMakeCurrent failed (0x%x, %s) at %s — context "
                 "lost, permanent CPU-path downgrade for this window",
                 me, lane_egl_err_name(me), ewin->lane_stage);
        return -1;
    }
    /* TASK 1c — the pre-present stale drain: an error left over from
     * setup (or the previous present's tail) is named and consumed
     * BEFORE the first present, so this present's verdict can only
     * blame calls this present made. The risky firsts drain LOUDLY
     * (a stale error is a bug worth a WARN); steady state drains
     * silently, exactly the old single glGetError. */
    if (ewin->presents == 0 || !ewin->tex_ok) {
        lane_stale_drain(egl, ewin->lane_stage);
    } else {
        (void)lane_first_gl_error(egl);
    }

    if (trace) {
        FDK_INFO("GL trace: present#%d begins — %dx%d, damage %s, "
                 "texture %s, stage %s",
                 ewin->presents + 1, w, h,
                 damage->full ? "full"
                              : "partial (see glTexSubImage2D lines)",
                 ewin->tex_ok ? "established" : "fresh (glTexImage2D)",
                 ewin->lane_stage);
        if (wl_egl_window_get_attached_size != NULL && !ewin->tex_ok) {
            int aw = 0, ah = 0;
            if (wl_egl_window_get_attached_size(ewin->wl_win, &aw, &ah) ==
                0) {
                FDK_INFO("GL trace: present#%d wl_egl_window attached "
                         "%dx%d, presenting %dx%d%s",
                         ewin->presents + 1, aw, ah, w, h,
                         (aw == w && ah == h) ? "" : " — MISMATCH");
            }
        }
    }

    /* The graveyard drain (the share-group consequence): delete the
     * queued textures of destroyed windows — a context is current
     * right now, which is the whole point of the queue. */
    if (egl->graveyard_count > 0) {
        egl->fn.glDeleteTextures(egl->graveyard_count, egl->graveyard);
        (void)lane_first_gl_error(egl);
        egl->graveyard_count = 0;
    }

    /* === THE RETRY LADDER (the 1.0.0 fix) ==============================
     *
     * A failed present is not a verdict until every shape has had its
     * chance: the probe validated the shape against 4x4 uploads, but
     * the first REAL present can still fail (a size- or state-dependent
     * rejection the probe could not see — exactly the GTX 1050 Ti
     * report's shape). On a GL error or a swap failure the ladder
     * rotates to the next un-rejected shape, recreates the texture
     * (fresh GL object + sampler state + a full texImage2D), and
     * retries ONCE per shape. Only when the ladder is exhausted does
     * the window take the permanent downgrade — with the full trace
     * already in the log.
     *
     * §20 legality (the single-owner rule): the ladder never touches
     * wl_surface state — no attach, no commit, no wl_shm buffer while
     * the EGL surface owns this wl_surface. The only protocol traffic
     * is eglSwapBuffers' own driver-issued attach+commit (on success);
     * a FAILED swap leaves nothing to clean up, and the texture
     * recreation is GL-object state, invisible to the compositor. The
     * downgrade path (ladder exhausted) is unchanged: slot create ->
     * EGL destroy -> wl_shm attach, the load-bearing order. */
    for (int attempt = 0;; attempt++) {
        if (attempt > 0) {
            ewin->lane_stage = "a ladder retry (rotated upload shape)";
            ewin->lane_checked = ewin->presents == 0 || !ewin->tex_ok ||
                                 ewin->lane_tracing;
        }
        const int rc = lane_present_attempt(ewin, damage, w, h);
        if (rc == LANE_PRESENT_OK) {
            ewin->presents++;
            /* The wl_surface commit that publishes the EGL buffer
             * happens inside eglSwapBuffers (libwayland-egl attaches
             * the new buffer and commits) — the flush discipline the
             * SHM path applies after commit_render_pending applies
             * here too, so an app presenting outside the pump still
             * reaches the compositor promptly. */
            (void)wl_display_flush(egl->conn->display);
            return 0;
        }
        if (rc == LANE_PRESENT_FATAL) {
            return -1; /* already reported; no retry can fix it */
        }
        const int next = lane_next_shape(egl, egl->shape);
        if (next < 0) {
            FDK_WARN("GPU present ladder exhausted — every upload shape "
                     "failed (last: %s, %d presents on this window) — "
                     "permanent CPU-path downgrade for this window",
                     lane_shape_name(egl->shape), ewin->presents);
            return -1;
        }
        FDK_WARN("GPU present ladder: shape %d (%s) failed — rotating "
                 "the connection to shape %d (%s) with a fresh texture, "
                 "one retry",
                 egl->shape, lane_shape_name(egl->shape), next,
                 lane_shape_name(next));
        egl->shape = next;
        /* The fresh texture: a driver that choked on the old object's
         * state gets a clean one (mint + sampler params), and tex_ok
         * resets so the retry re-establishes the full image. */
        egl->fn.glDeleteTextures(1, &ewin->tex);
        (void)lane_first_gl_error(egl);
        egl->fn.glGenTextures(1, &ewin->tex);
        lane_setup_texture(egl, ewin->tex);
        ewin->tex_ok = 0;
    }
}

/* One present attempt: the completeness check, the shape-driven
 * upload, the draw, the tail drain, the self-check, the swap. All
 * failure WARNs are emitted here (with the full TASK-1 attribution),
 * so a ladder retry's log reads as one coherent story. Requires the
 * context CURRENT (present() made it so) and ewin->lane_* prepared. */
static int lane_present_attempt(struct fdk_wayland_egl_window *ewin,
                                const fdk_platform_damage *damage, int w,
                                int h) {
    struct fdk_wayland_egl *egl = ewin->egl;
    const int trace = ewin->lane_tracing;

    /* The default framebuffer's completeness, checked once per
     * texture lifecycle (the ES2-spec statuses for the default
     * framebuffer): a diagnostic query whose own error is traced but
     * not fatal (drivers disagree on querying the default fb), while
     * an explicit GL_FRAMEBUFFER_UNSUPPORTED is a real present
     * failure — there is nothing to draw into. */
    if (!ewin->tex_ok) {
        (void)snprintf(ewin->lane_op, sizeof(ewin->lane_op),
                       "glCheckFramebufferStatus(default)");
        const unsigned int fbstat =
            egl->fn.glCheckFramebufferStatus(GLC_FRAMEBUFFER);
        const unsigned int qerr = lane_first_gl_error(egl);
        if (trace) {
            FDK_INFO("GL trace: present#%d default framebuffer status "
                     "0x%x (query error 0x%x, %s)",
                     ewin->presents + 1, fbstat, qerr,
                     qerr == 0 ? "clean"
                               : lane_gl_err_name(qerr));
        }
        if (fbstat == GLC_FRAMEBUFFER_UNSUPPORTED) {
            FDK_WARN("default framebuffer reports "
                     "GL_FRAMEBUFFER_UNSUPPORTED at %s — GPU present "
                     "lane downgrading this window to the CPU path",
                     ewin->lane_stage);
            return LANE_PRESENT_FATAL;
        }
    }

    LANE_GL_OP("glBindTexture(TEXTURE_2D, window texture)",
               egl->fn.glBindTexture(GLC_TEXTURE_2D, ewin->tex));

    /* === the UPLOAD CONTRACT (§19 + the 1.0.0 probe/ladder) ==========
     *
     * The four shapes (fastest first): BGRA + ROW_LENGTH, BGRA with
     * tightly packed sub-rects, a CPU-swizzled RGBA twin + ROW_LENGTH,
     * and the twin with packed sub-rects — pure core GLES2. The
     * OPERATIVE shape is egl->shape: the PROBE picked it off real 4x4
     * uploads the driver accepted (not the extension string — the GTX
     * 1050 Ti report: both extensions advertised, the fast path chosen,
     * the first present still failed, because internalformat GL_RGBA
     * paired with format GL_BGRA_EXT is in no ES2 table while Mesa
     * tolerates it and NVIDIA does not), and the ladder can rotate it
     * DOWN at runtime when a present fails.
     *
     * The twin is byte-identical INK (swizzling [B,G,R,X] to
     * [R,G,B,X] makes the RGBA upload produce the same texel the
     * BGRA upload of the original would), so the self-check, the
     * equivalence rigs, and the readback all see one unchanged
     * pipeline — whichever shape serves the upload. */
    const int use_bgra = lane_shape_uses_bgra(egl->shape);
    const int use_rowlen = lane_shape_uses_rowlen(egl->shape);
    const fdk_u32 *src_buf = ewin->staging;
    if (!use_bgra) {
        if (lane_ensure_rgba_twin(ewin) != 0) {
            return LANE_PRESENT_FATAL; /* allocation failure: OOM —
                                        * the frame is intact in
                                        * staging for the CPU salvage */
        }
        src_buf = ewin->rgba_twin;
    }
    const unsigned int src_fmt = use_bgra ? GLC_BGRA : GLC_RGBA;
    /* The probe-validated internalformat: the BGRA shapes ride the
     * spelling the driver accepted (GL_BGRA_EXT's own table row by
     * default; the ES2-lenient GL_RGBA internal + BGRA format when
     * that was what the driver took). */
    const unsigned int internal =
        (use_bgra && !egl->bgra_internal_rgba) ? GLC_BGRA : GLC_RGBA;
    char desc[128];
    /* ROW_LENGTH is touched ONLY when the extension exists — on a
     * context without GL_EXT_unpack_subimage the pname itself is
     * GL_INVALID_ENUM (value 0 included; the pre-1.0.0 code set it
     * unconditionally, poisoning strict drivers on the fallback
     * shapes too). With the extension, it carries the window stride
     * for rowlen shapes and resets to 0 (tight) for packed ones. */
    if (egl->has_rowlen_ext) {
        (void)snprintf(desc, sizeof(desc),
                       "glPixelStorei(UNPACK_ROW_LENGTH, %d)",
                       use_rowlen ? w : 0);
        LANE_GL_OP(desc,
                   egl->fn.glPixelStorei(GLC_UNPACK_ROW_LENGTH,
                                         use_rowlen ? w : 0));
    }

    /* TEST SEAM (FDK_GL_POISON_FIRST): the first present's first
     * attempt performs one guaranteed-invalid upload call (a format
     * enum no conformant driver accepts) BEFORE the real one, so the
     * real upload's drain reports a driver-style failure — the rig's
     * deterministic proof that the ladder recovers a poisoned first
     * present. Error-state-only: rejected calls change nothing. */
    if (ewin->presents == 0 && ewin->poison_first) {
        ewin->poison_first = 0;
        egl->fn.glTexImage2D(GLC_TEXTURE_2D, 0, GLC_RGBA, 4, 4, 0,
                             0x9999u, GLC_UNSIGNED_BYTE, NULL);
    }

    if (!ewin->tex_ok) {
        (void)snprintf(desc, sizeof(desc),
                       "glTexImage2D(%dx%d, internal=%s, fmt=%s)",
                       w, h,
                       internal == GLC_BGRA ? "GL_BGRA_EXT" : "GL_RGBA",
                       lane_fmt_name(src_fmt));
        LANE_GL_OP(desc,
                   egl->fn.glTexImage2D(GLC_TEXTURE_2D, 0, (int)internal,
                                        w, h, 0, src_fmt,
                                        GLC_UNSIGNED_BYTE, src_buf));
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
            /* Full image: width == stride, so the upload is correct
             * with or without UNPACK_ROW_LENGTH support. */
            (void)snprintf(desc, sizeof(desc),
                           "glTexSubImage2D(full %dx%d, fmt=%s)", w, h,
                           lane_fmt_name(src_fmt));
            LANE_GL_OP(desc,
                       egl->fn.glTexSubImage2D(GLC_TEXTURE_2D, 0, 0, 0, w,
                                               h, src_fmt, GLC_UNSIGNED_BYTE,
                                               src_buf));
        } else {
            for (int i = 0; i < live; i++) {
                int rw = (int)(rx1[i] - rx0[i]);
                int rh = (int)(ry1[i] - ry0[i]);
                const fdk_u32 *src = src_buf;
                if (use_rowlen) {
                    /* Stride straight out of the full buffer —
                     * ROW_LENGTH carries the window stride. */
                    src = src_buf + (size_t)ry0[i] * (size_t)w +
                          (size_t)rx0[i];
                } else {
                    /* No ROW_LENGTH: rows must arrive tightly packed
                     * — one memcpy pass per damaged rect. */
                    src = lane_pack_rect(ewin, src_buf, w, (int)rx0[i],
                                         (int)ry0[i], rw, rh);
                    if (src == NULL) {
                        return LANE_PRESENT_FATAL; /* scratch allocation
                                                    * failure: OOM,
                                                    * frame intact */
                    }
                }
                (void)snprintf(desc, sizeof(desc),
                               "glTexSubImage2D(rect %dx%d at %lld,%lld, "
                               "fmt=%s)",
                               rw, rh, rx0[i], ry0[i],
                               lane_fmt_name(src_fmt));
                LANE_GL_OP(desc,
                           egl->fn.glTexSubImage2D(
                               GLC_TEXTURE_2D, 0, (int)rx0[i], (int)ry0[i],
                               rw, rh, src_fmt, GLC_UNSIGNED_BYTE, src));
            }
        }
    }

    {
        const int drawrc = draw_fullscreen_quad(ewin, w, h);
        if (drawrc != 0) {
            return drawrc; /* already reported with attribution */
        }
    }

    /* The steady-state tail drain (the risky firsts drained per call
     * above): one glGetError per present, attributed to the last
     * lane call — an error raised anywhere since the last drain lands
     * here, named, with the shape/format/stage context attached. */
    {
        const unsigned int tail_err = lane_first_gl_error(egl);
        if (tail_err != 0) {
            lane_report_call_error(ewin, tail_err,
                                   ewin->presents > 0 && ewin->tex_ok
                                       ? "a steady-state present "
                                         "(end-of-present drain — "
                                         "attributed to the last lane "
                                         "call)"
                                       : ewin->lane_stage);
            return LANE_PRESENT_GL;
        }
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
            (void)snprintf(desc, sizeof(desc),
                           "glReadPixels(self-check probe %d,%d)", sx, sy);
            LANE_GL_OP(desc,
                       egl->fn.glReadPixels(sx, h - 1 - sy, 1, 1, GLC_RGBA,
                                            GLC_UNSIGNED_BYTE, rgba));
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
            return LANE_PRESENT_FATAL; /* this window downgrades,
                                       * later ones refuse */
        }
        egl->selfcheck = WL_GL_SELFCHECK_PASSED;
    }

    /* Unconditional (not trace-gated — the 2026-11-11 wayland-lane
     * audit: the failing presents were exactly the untraced ones): a
     * pre-swap error check + a POST-swap drain. The swap's internal
     * flush is the last GL work of the present; anything it queues
     * must not leak into the next engine flush's error check (the
     * engine marks itself dead on a queued error — a lane-owned
     * error must never kill the engine). */
    unsigned pe = egl->fn.glGetError();
    if (pe != 0) {
        FDK_WARN("GL lane: present#%d queued 0x%x before the swap "
                 "(after the tail drain — the upload, the quad draw or "
                 "the self-check raised it)",
                 ewin->presents + 1, pe);
    }
    if (!egl->fn.eglSwapBuffers(egl->egl_display, ewin->surface)) {
        const unsigned int se = (unsigned)egl->fn.eglGetError();
        FDK_WARN("eglSwapBuffers failed (0x%x, %s) at %s after a clean "
                 "draw — the ladder gives the next upload shape one "
                 "chance before any downgrade",
                 se, lane_egl_err_name(se), ewin->lane_stage);
        return LANE_PRESENT_SWAP;
    }
    unsigned se2 = egl->fn.glGetError();
    if (se2 != 0) {
        FDK_WARN("GL lane: present#%d queued 0x%x AT THE SWAP (the "
                 "swap's internal flush raised it) — consumed here so "
                 "it cannot kill the engine\n", ewin->presents + 1, se2);
    }
    return LANE_PRESENT_OK;
}

/* ---- §19 test seams (1.0.0-rc2): the driverless fallback-shape pins ---- */

void fdk__wayland_egl_test_swizzle(fdk_u32 *dst, const fdk_u32 *src,
                                   int n) {
    lane_swizzle_rgba(dst, src, (size_t)n);
}

void fdk__wayland_egl_test_pack_rows(fdk_u32 *dst, const fdk_u32 *src,
                                     int stride, int x, int y, int rw,
                                     int rh) {
    lane_pack_rows(dst, src, stride, x, y, rw, rh);
}


/* ---- FDK_EXT_CAP_GL (1.0.0): the loaded lane's EGLDisplay -----------------
 * (wayland_egl.h's "no backend leakage" note keeps EGL types inside
 * this file — the accessor hands the DISPLAY out as void*, the
 * documented cast target of fdk_extend.h's GL recipe.) */

void *fdk_wayland_gl_display(fdk_platform_connection *conn) {
    return (conn != NULL && conn->egl != NULL) ? (void *)conn->egl->egl_display
                                               : NULL;
}

/* Test seam: readback of the GPU frame. makeCurrent, draw whatever
 * is pending straight into the back buffer, then read BEFORE any
 * swap — the X11 seam's contract, stated in its own words: "this
 * isolates rasterization from present semantics (post-swap
 * back-buffer content is driver-defined)". The EGL lane needs it
 * MORE than the GLX one: eglSwapBuffers gives the back buffer
 * EGL_BUFFER_DESTROYED semantics on this stack (llvmpipe zeroes it),
 * and the wayland-lane comparison (2026-11-11) read back only the
 * scene background because the freshly painted batch was never
 * drawn — the seam skipped the flush, so glReadPixels sampled the
 * pre-paint buffer. The engine's flush is idempotent on an empty
 * batch, so this only ever draws what the caller booked. */
fdk_result fdk_wayland_egl_window_readback(
    struct fdk_wayland_egl_window *ewin, void *surface_ptr, fdk_u32 *out,
    int width, int height) {
    if (ewin == NULL || surface_ptr == NULL || ewin->surface == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    fdk_surface *s = (fdk_surface *)surface_ptr;
    if (s->gpu == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (!egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                ewin->surface, ewin->ctx)) {
        return FDK_ERR_UNSUPPORTED;
    }
    /* The pre-flush stale drain (the paint flush's discipline): an
     * error the previous swap queued must be consumed BEFORE the
     * engine's flush — the flush's tail check marks the engine dead
     * on any queued error, and the readback would then fail with a
     * "precheck" that names a dead engine the caller never killed. */
    (void)egl->fn.glGetError();
    fdk__gpu_flush(s);
    return fdk__gpu_engine_readback(s, out, width, height) == 0
               ? FDK_OK
               : FDK_ERR_UNSUPPORTED;
}

/* P2: makeCurrent / swap without the engine (add-on GL). */
fdk_result fdk_wayland_egl_window_make_current(
    struct fdk_wayland_egl_window *ewin) {
    if (ewin == NULL || ewin->surface == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    if (!egl->fn.eglMakeCurrent(egl->egl_display, ewin->surface,
                                ewin->surface, ewin->ctx)) {
        return FDK_ERR_UNSUPPORTED;
    }
    return FDK_OK;
}

fdk_result fdk_wayland_egl_window_swap(struct fdk_wayland_egl_window *ewin) {
    if (ewin == NULL || ewin->surface == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    EGLBoolean ok = egl->fn.eglSwapBuffers(egl->egl_display, ewin->surface);
    (void)wl_display_flush(egl->conn->display);
    return ok ? FDK_OK : FDK_ERR_UNSUPPORTED;
}

/* The add-on frame callback's present: context current, callback, swap
 * (the engine flush is skipped — present arbitration). */
fdk_result fdk_wayland_egl_window_cb_flush(struct fdk_wayland_egl_window *ewin,
                                           void (*fn)(void *, void *),
                                           void *user, void *surface) {
    fdk_result r = fdk_wayland_egl_window_make_current(ewin);
    if (!fdk_ok(r)) {
        return r;
    }
    fn(user, surface);
    return fdk_wayland_egl_window_swap(ewin);
}

/* ---- P2: real fences (EGL_KHR_fence_sync; EGL_ANDROID_native_fence_
 * sync gives a native FD where the driver has it) ---------------------- */
struct fdk_wl_sync_fns {
    void *create_sync;   /* eglCreateSyncKHR */
    void *destroy_sync;  /* eglDestroySyncKHR */
    void *client_wait;   /* eglClientWaitSyncKHR */
    void *dup_fd;        /* eglDupNativeFenceFDANDROID */
};

static struct fdk_wl_sync_fns g_wl_sync;

typedef void *(*egl_gpa_fn)(void *, const char *);

static int wl_sync_resolve(void *lib_egl, egl_gpa_fn gpa) {
    if (g_wl_sync.create_sync != NULL) {
        return 1;
    }
    g_wl_sync.create_sync = gpa(lib_egl, "eglCreateSyncKHR");
    g_wl_sync.destroy_sync = gpa(lib_egl, "eglDestroySyncKHR");
    g_wl_sync.client_wait = gpa(lib_egl, "eglClientWaitSyncKHR");
    g_wl_sync.dup_fd = gpa(lib_egl, "eglDupNativeFenceFDANDROID");
    return g_wl_sync.create_sync != NULL && g_wl_sync.client_wait != NULL &&
           g_wl_sync.destroy_sync != NULL;
}

fdk_result fdk_wayland_egl_window_fence_insert(
    struct fdk_wayland_egl_window *ewin, fdk_u64 *out_fence) {
    if (ewin == NULL || ewin->surface == 0 || out_fence == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    if (!wl_sync_resolve(egl->lib_egl,
                             (void *)egl->fn.eglGetProcAddress)) {
        return FDK_ERR_UNSUPPORTED; /* driver lacks EGL_KHR_fence_sync */
    }
    void *(*create)(void *, unsigned, const void *) =
        (void *(*)(void *, unsigned, const void *))g_wl_sync.create_sync;
    void *sync = create(egl->egl_display, 0x30F0 /* EGL_SYNC_FENCE_KHR */,
                        NULL);
    if (sync == NULL) {
        return FDK_ERR_UNSUPPORTED;
    }
    *out_fence = (fdk_u64)(uintptr_t)sync;
    return FDK_OK;
}

fdk_result fdk_wayland_egl_window_fence_wait(
    struct fdk_wayland_egl_window *ewin, fdk_u64 fence) {
    if (ewin == NULL || fence == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_wayland_egl *egl = ewin->egl;
    if (!wl_sync_resolve(egl->lib_egl,
                             (void *)egl->fn.eglGetProcAddress)) {
        return FDK_ERR_UNSUPPORTED;
    }
    unsigned (*cwait)(void *, void *, int) =
        (unsigned (*)(void *, void *, int))g_wl_sync.client_wait;
    /* EGL_SYNC_FLUSH_COMMANDS_BIT | forever: the waiter decides. */
    unsigned r = cwait(egl->egl_display, (void *)(uintptr_t)fence,
                       0x0001 | 0xFFFFFFFFu);
    void (*destroy)(void *, void *) =
        (void (*)(void *, void *))g_wl_sync.destroy_sync;
    destroy(egl->egl_display, (void *)(uintptr_t)fence);
    /* 0x3051 = EGL_CONDITION_SATISFIED_KHR */
    return r == 0x3051 ? FDK_OK : FDK_ERR_UNSUPPORTED;
}
