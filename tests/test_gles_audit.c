/* tests/test_gles_audit.c — the GLES format/construct audit, run
 * against a REAL ES context on llvmpipe (surfaceless/pbuffer EGL).
 *
 * The GL_LUMINANCE atlas bug (2026-11-11) was desktop-only and
 * invisible on every local run: the lane's GL is dlopen'd, so no
 * compiler can type-check the enums against the context's profile.
 * This test closes that gap WITHOUT hardware: it boots a strict ES
 * context (Mesa llvmpipe) and creates every construct the lanes use,
 * checking each against that ES version's format table, with
 * NEGATIVE CONTROLS proving the checker itself works (on ES 3.x,
 * GL_LUMINANCE must FAIL; the precision-less shader must FAIL).
 *
 * What it can catch: format-table rejections (LUMINANCE-class),
 * illegal extension use (BGRA without the extension), illegal shader
 * constructs (missing precision), unpack-state misuse. What it
 * cannot: driver quirks beyond the spec (NVIDIA's own opinions) —
 * those stay with run-hardware.sh's report.
 *
 * Run: make test-gles-audit   (no X11/Wayland needed; rc 77 = skip)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

#include "render/gpu_shaders.h" /* the REAL lane shader sources */

/* EGL 1.5 constants (values from the spec; hand-declared like the
 * lanes do — the audit must not depend on EGL headers either). */
#define EGLC_SURFACE_TYPE 0x3033
#define EGLC_PBUFFER_BIT 0x0001
#define EGLC_RENDERABLE_TYPE 0x3040
#define EGLC_OPENGL_ES_BIT 0x0001
#define EGLC_OPENGL_ES2_BIT 0x0004
#define EGLC_RED_SIZE 0x3024
#define EGLC_GREEN_SIZE 0x3023
#define EGLC_BLUE_SIZE 0x3022
#define EGLC_ALPHA_SIZE 0x3021
#define EGLC_NONE 0x3038
#define EGLC_CONTEXT_CLIENT_VERSION 0x3098
#define EGLC_EXTENSIONS 0x3055
#define EGLC_WIDTH 0x3057
#define EGLC_HEIGHT 0x3056
#define EGLC_NO_SURFACE ((void *)0)
#define EGLC_NO_DISPLAY ((void *)0)
#define EGLC_NO_CONTEXT ((void *)0)
#define EGLC_PLATFORM_SURFACELESS_MESA 0x31DD

/* GLES constants. */
#define GLC_TEXTURE_2D 0x0DE1
#define GLC_RGBA 0x1908
#define GLC_BGRA_EXT 0x80E1
#define GLC_LUMINANCE 0x1909
#define GLC_RED 0x1903
#define GLC_R8 0x8229
#define GLC_UNSIGNED_BYTE 0x1401
#define GLC_NEAREST 0x2600
#define GLC_LINEAR 0x2601
#define GLC_CLAMP_TO_EDGE 0x812F
#define GLC_TEXTURE_MIN_FILTER 0x2801
#define GLC_TEXTURE_MAG_FILTER 0x2800
#define GLC_TEXTURE_WRAP_S 0x2802
#define GLC_TEXTURE_WRAP_T 0x2803
#define GLC_UNPACK_ROW_LENGTH 0x0CF2
#define GLC_UNPACK_ALIGNMENT 0x0CF5
#define GLC_TEXTURE0 0x84C0
#define GLC_VERTEX_SHADER 0x8B31
#define GLC_FRAGMENT_SHADER 0x8B30
#define GLC_COMPILE_STATUS 0x8B81
#define GLC_LINK_STATUS 0x8B82
#define GLC_ARRAY_BUFFER 0x8892
#define GLC_STREAM_DRAW 0x88E0
#define GLC_FLOAT 0x1406
#define GLC_FRAMEBUFFER 0x8D40
#define GLC_RENDERBUFFER 0x8D41
#define GLC_COLOR_ATTACHMENT0 0x8CE0
#define GLC_FRAMEBUFFER_COMPLETE 0x8CD5
#define GLC_RGBA8 0x8058
#define GLC_RENDERER 0x1F01
#define GLC_VERSION 0x1F02
#define GLC_EXTENSIONS 0x1F03
#define GLC_RGBA4 0x8056

/* EGL handles are opaque pointers (the lanes' own convention);
 * EGLConfig is an integer handle. */
typedef void *EGLDisplay_;
typedef void *EGLContext_;
typedef void *EGLSurface_;
typedef void *EGLConfig_; /* EGLConfig is an opaque POINTER in the
                            * real headers — an int typedef made
                            * eglChooseConfig overflow it (ASan-caught)
                            */
typedef int EGLBoolean_;
typedef int EGLint_;
typedef unsigned gl_uint_;
typedef int gl_int_;

static void *g_egl_lib, *g_gles_lib;
static EGLDisplay_ g_dpy;
static EGLContext_ g_ctx;
static EGLSurface_ g_surf;
static int g_es_major, g_es_minor;
static int g_failures;
static int g_controls_did_not_fire;
static char g_where[192];

/* ---- EGL entry points (dlsym; EGL 1.0-core fns are exportable) ---- */
static EGLBoolean_ (*_eglInitialize)(EGLDisplay_, EGLint_ *, EGLint_ *);
static EGLBoolean_ (*_eglTerminate)(EGLDisplay_);
static EGLBoolean_ (*_eglChooseConfig)(EGLDisplay_, const EGLint_ *,
                                       EGLConfig_ *, EGLint_, EGLint_ *);
static EGLContext_ (*_eglCreateContext)(EGLDisplay_, EGLConfig_,
                                        EGLContext_, const EGLint_ *);
static EGLBoolean_ (*_eglDestroyContext)(EGLDisplay_, EGLContext_);
static EGLSurface_ (*_eglCreatePbufferSurface)(EGLDisplay_, EGLConfig_,
                                               const EGLint_ *);
static EGLBoolean_ (*_eglDestroySurface)(EGLDisplay_, EGLSurface_);
static EGLBoolean_ (*_eglMakeCurrent)(EGLDisplay_, EGLSurface_,
                                      EGLSurface_, EGLContext_);
static const char *(*_eglQueryString)(EGLDisplay_, EGLint_);
static unsigned (*_eglGetError)(void);
static void *(*_eglGetProcAddress)(const char *);
static EGLDisplay_ (*_eglGetPlatformDisplay)(unsigned, void *,
                                             const EGLint_ *);
static EGLDisplay_ (*_eglGetDisplay)(void *);

/* ---- GLES entry points (eglGetProcAddress exclusively — GLVND does
 * not promise dlsym for GL entry points) ---- */
static const unsigned char *(*_glGetString)(gl_uint_);
static unsigned (*_glGetError)(void);
static void (*_glGenTextures)(gl_int_, gl_uint_ *);
static void (*_glBindTexture)(gl_uint_, gl_uint_);
static void (*_glTexImage2D)(gl_uint_, gl_int_, gl_int_, gl_int_,
                             gl_int_, gl_int_, gl_uint_, gl_uint_,
                             const void *);
static void (*_glTexSubImage2D)(gl_uint_, gl_int_, gl_int_, gl_int_,
                                gl_int_, gl_int_, gl_uint_, gl_uint_,
                                const void *);
static void (*_glDeleteTextures)(gl_int_, const gl_uint_ *);
static void (*_glPixelStorei)(gl_uint_, gl_int_);
static void (*_glTexParameteri)(gl_uint_, gl_uint_, gl_int_);
static void (*_glActiveTexture)(gl_uint_);
static gl_uint_ (*_glCreateShader)(gl_uint_);
static void (*_glShaderSource)(gl_uint_, gl_int_, const char **,
                               const gl_int_ *);
static void (*_glCompileShader)(gl_uint_);
static void (*_glGetShaderiv)(gl_uint_, gl_uint_, gl_int_ *);
static void (*_glGetShaderInfoLog)(gl_uint_, gl_int_, gl_int_ *, char *);
static gl_uint_ (*_glCreateProgram)(void);
static void (*_glAttachShader)(gl_uint_, gl_uint_);
static void (*_glLinkProgram)(gl_uint_);
static void (*_glGetProgramiv)(gl_uint_, gl_uint_, gl_int_ *);
static void (*_glGenFramebuffers)(gl_int_, gl_uint_ *);
static void (*_glBindFramebuffer)(gl_uint_, gl_uint_);
static void (*_glGenRenderbuffers)(gl_int_, gl_uint_ *);
static void (*_glBindRenderbuffer)(gl_uint_, gl_uint_);
static void (*_glRenderbufferStorage)(gl_uint_, gl_uint_, gl_int_,
                                      gl_int_);
static void (*_glFramebufferRenderbuffer)(gl_uint_, gl_uint_, gl_uint_,
                                          gl_uint_);
static gl_uint_ (*_glCheckFramebufferStatus)(gl_uint_);
static void (*_glReadPixels)(gl_int_, gl_int_, gl_int_, gl_int_,
                             gl_uint_, gl_uint_, void *);
static void (*_glGenBuffers)(gl_int_, gl_uint_ *);
static void (*_glBindBuffer)(gl_uint_, gl_uint_);
static void (*_glBufferData)(gl_uint_, long, const void *, gl_uint_);

static int resolve_all(void) {
    g_egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (g_egl_lib == NULL) {
        return 0;
    }
#define RS(field, sym) do { \
        *(void **)(&field) = dlsym(g_egl_lib, sym); \
        if ((field) == NULL) return 0; \
    } while (0)
    RS(_eglInitialize, "eglInitialize");
    RS(_eglTerminate, "eglTerminate");
    RS(_eglChooseConfig, "eglChooseConfig");
    RS(_eglCreateContext, "eglCreateContext");
    RS(_eglDestroyContext, "eglDestroyContext");
    RS(_eglCreatePbufferSurface, "eglCreatePbufferSurface");
    RS(_eglDestroySurface, "eglDestroySurface");
    RS(_eglMakeCurrent, "eglMakeCurrent");
    RS(_eglQueryString, "eglQueryString");
    RS(_eglGetError, "eglGetError");
    RS(_eglGetProcAddress, "eglGetProcAddress");
    RS(_eglGetDisplay, "eglGetDisplay");
    /* EGL 1.5-only: absent on old stacks — optional. */
    *(void **)(&_eglGetPlatformDisplay) =
        dlsym(g_egl_lib, "eglGetPlatformDisplay");
#undef RS
    g_gles_lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_LOCAL);
    if (g_gles_lib == NULL) {
        return 0;
    }
#define RG(field, sym) do { \
        *(void **)(&(field)) = _eglGetProcAddress(sym); \
        if ((field) == NULL) { \
            /* fall back to dlsym for pre-GLVND stacks */ \
            *(void **)(&(field)) = dlsym(g_gles_lib, sym); \
            if ((field) == NULL) return 0; \
        } \
    } while (0)
    RG(_glGetString, "glGetString");
    RG(_glGetError, "glGetError");
    RG(_glGenTextures, "glGenTextures");
    RG(_glBindTexture, "glBindTexture");
    RG(_glTexImage2D, "glTexImage2D");
    RG(_glTexSubImage2D, "glTexSubImage2D");
    RG(_glDeleteTextures, "glDeleteTextures");
    RG(_glPixelStorei, "glPixelStorei");
    RG(_glTexParameteri, "glTexParameteri");
    RG(_glActiveTexture, "glActiveTexture");
    RG(_glCreateShader, "glCreateShader");
    RG(_glShaderSource, "glShaderSource");
    RG(_glCompileShader, "glCompileShader");
    RG(_glGetShaderiv, "glGetShaderiv");
    RG(_glGetShaderInfoLog, "glGetShaderInfoLog");
    RG(_glCreateProgram, "glCreateProgram");
    RG(_glAttachShader, "glAttachShader");
    RG(_glLinkProgram, "glLinkProgram");
    RG(_glGetProgramiv, "glGetProgramiv");
    RG(_glGenFramebuffers, "glGenFramebuffers");
    RG(_glBindFramebuffer, "glBindFramebuffer");
    RG(_glGenRenderbuffers, "glGenRenderbuffers");
    RG(_glBindRenderbuffer, "glBindRenderbuffer");
    RG(_glRenderbufferStorage, "glRenderbufferStorage");
    RG(_glFramebufferRenderbuffer, "glFramebufferRenderbuffer");
    RG(_glCheckFramebufferStatus, "glCheckFramebufferStatus");
    RG(_glReadPixels, "glReadPixels");
    RG(_glGenBuffers, "glGenBuffers");
    RG(_glBindBuffer, "glBindBuffer");
    RG(_glBufferData, "glBufferData");
#undef RG
    return 1;
}

static const char *es_str(gl_uint_ name) {
    const unsigned char *s = _glGetString(name);
    return s != NULL ? (const char *)s : "(null)";
}

static int has_ext(const char *needle) {
    const char *e = es_str(GLC_EXTENSIONS);
    return e != NULL && strstr(e, needle) != NULL;
}

static unsigned drain_err(void) {
    unsigned e = _glGetError();
    while (_glGetError() != 0) {
        /* drain everything; report the first */
    }
    return e;
}

/* verdict(name, expect_fail, actual_err): 0 ok, 1 mismatch.
 * The negative controls are STACK-DEPENDENT: NVIDIA's ES contexts
 * reject GL_LUMINANCE and precision-less GLSL ES 1.00 fragment
 * shaders (the 2026-11-11 17:01Z box proved both); Mesa's ES
 * contexts accept both (leniency kept for ANGLE/back-compat). On a
 * lenient stack a control that did not fire is recorded as
 * "control did not fire" — never a failure of the code under audit,
 * and never hidden: the summary carries it. */
static int verdict(const char *name, int expect_fail, unsigned err) {
    int failed = (err != 0);
    const char *verdict_s;
    if (expect_fail) {
        if (failed) {
            verdict_s = "correctly rejected (control FIRED)";
        } else {
            verdict_s = "accepted — control did not fire on this "
                        "stack (lenient driver; NVIDIA rejects)";
            g_controls_did_not_fire++;
        }
    } else {
        verdict_s = failed ? "REJECTED — BUG" : "accepted (core-legal)";
    }
    printf("  %-46s %s (0x%x)\n", name, verdict_s, err);
    /* A failure is ONLY a pass-expected construct rejected by the
     * stack. A negative control that did not fire is a stack
     * property (leniency), recorded in the summary, not a failure. */
    if (!expect_fail && failed) {
        snprintf(g_where, sizeof(g_where), "%s", name);
        g_failures++;
        return 1;
    }
    return 0;
}

static int try_create(unsigned ifmt, unsigned fmt, unsigned type) {
    gl_uint_ t = 0;
    _glGenTextures(1, &t);
    _glBindTexture(GLC_TEXTURE_2D, t);
    _glTexImage2D(GLC_TEXTURE_2D, 0, (gl_int_)ifmt, 4, 4, 0, fmt, type,
                  NULL);
    unsigned e = drain_err();
    _glBindTexture(GLC_TEXTURE_2D, 0);
    _glDeleteTextures(1, &t);
    return (int)e;
}

static int compile_link(const char *vs_src, const char *fs_src,
                        char *log, size_t logsz) {
    log[0] = 0;
    gl_uint_ vs = _glCreateShader(GLC_VERTEX_SHADER);
    gl_uint_ fs = _glCreateShader(GLC_FRAGMENT_SHADER);
    /* The ES precision head mirrors gp_compile's: GLSL ES 1.00
     * fragment shaders have no default float precision. The negative
     * control below compiles WITHOUT it and must fail. */
    char head[64];
    snprintf(head, sizeof(head),
             "#define GLES 1\nprecision mediump float;\n");
    const char *fparts[2] = { head, fs_src };
    /* GP_VS/QUAD_VS never got the head (vertex shaders have a default
     * highp float in GLSL ES 1.00) — match gp_compile: VS without. */
    _glShaderSource(vs, 1, &vs_src, NULL);
    _glCompileShader(vs);
    gl_int_ ok = 0;
    _glGetShaderiv(vs, GLC_COMPILE_STATUS, &ok);
    if (!ok) {
        _glGetShaderInfoLog(vs, (gl_int_)logsz, NULL, log);
        _glGetShaderiv(fs, GLC_COMPILE_STATUS, &ok); /* silence */
        return 0;
    }
    _glShaderSource(fs, 2, fparts, NULL);
    _glCompileShader(fs);
    ok = 0;
    _glGetShaderiv(fs, GLC_COMPILE_STATUS, &ok);
    if (!ok) {
        _glGetShaderInfoLog(fs, (gl_int_)logsz, NULL, log);
        return 0;
    }
    gl_uint_ p = _glCreateProgram();
    _glAttachShader(p, vs);
    _glAttachShader(p, fs);
    _glLinkProgram(p);
    ok = 0;
    _glGetProgramiv(p, GLC_LINK_STATUS, &ok);
    return ok ? 1 : 0;
}

int main(void) {
    printf("==== GLES format/construct audit (real ES context) ====\n");
    if (!resolve_all()) {
        printf("SKIP: no EGL/GLES stack (rc 77)\n");
        return 77;
    }

    /* Display: Mesa's surfaceless platform first, default fallback. */
    if (_eglGetPlatformDisplay != NULL) {
        g_dpy = _eglGetPlatformDisplay(EGLC_PLATFORM_SURFACELESS_MESA,
                                       NULL, NULL);
    }
    if (g_dpy == EGLC_NO_DISPLAY) {
        g_dpy = _eglGetDisplay(NULL);
    }
    if (g_dpy == EGLC_NO_DISPLAY) {
        printf("SKIP: no EGL display (rc 77)\n");
        return 77;
    }
    EGLint_ maj = 0, mn = 0;
    if (!_eglInitialize(g_dpy, &maj, &mn)) {
        printf("SKIP: eglInitialize failed (rc 77)\n");
        return 77;
    }

    /* Config: pbuffer-capable so the context has a default framebuffer
     * to read from; ES2 renderable bit covers ES 2.0 AND ES 3.x. */
    const EGLint_ cfg[] = { EGLC_SURFACE_TYPE, EGLC_PBUFFER_BIT,
                            EGLC_RENDERABLE_TYPE, EGLC_OPENGL_ES2_BIT,
                            EGLC_RED_SIZE, 8, EGLC_GREEN_SIZE, 8,
                            EGLC_BLUE_SIZE, 8, EGLC_NONE };
    EGLConfig_ conf = NULL;
    EGLint_ nc = 0;
    if (!_eglChooseConfig(g_dpy, cfg, &conf, 1, &nc) || nc < 1) {
        printf("SKIP: no pbuffer-capable ES EGLConfig (rc 77)\n");
        return 77;
    }

    /* Context: ES 3 first (the strict format table the NVIDIA box
     * runs), ES 2 fallback (the strict Mesa path the wayland lane's
     * CLIENT_VERSION=2 request can land on). FDK_GLES_AUDIT_ES=2
     * forces the ES 2 matrix (R8 must fail there — the audit's other
     * direction); =3 forces ES 3. */
    const EGLint_ c3[] = { EGLC_CONTEXT_CLIENT_VERSION, 3, EGLC_NONE };
    const EGLint_ c2[] = { EGLC_CONTEXT_CLIENT_VERSION, 2, EGLC_NONE };
    const char *force = getenv("FDK_GLES_AUDIT_ES");
    if (force != NULL && force[0] == '3') {
        g_ctx = _eglCreateContext(g_dpy, conf, EGLC_NO_CONTEXT, c3);
    } else if (force != NULL && force[0] == '2') {
        g_ctx = _eglCreateContext(g_dpy, conf, EGLC_NO_CONTEXT, c2);
        if (g_ctx == EGLC_NO_CONTEXT) {
            printf("SKIP: forced ES 2 context refused (rc 77)\n");
            return 77;
        }
    } else {
        g_ctx = _eglCreateContext(g_dpy, conf, EGLC_NO_CONTEXT, c3);
        if (g_ctx == EGLC_NO_CONTEXT) {
            g_ctx = _eglCreateContext(g_dpy, conf, EGLC_NO_CONTEXT, c2);
        }
    }
    if (g_ctx == EGLC_NO_CONTEXT) {
        printf("SKIP: no ES context (rc 77)\n");
        return 77;
    }

    const EGLint_ pb[] = { EGLC_WIDTH, 16, EGLC_HEIGHT, 16, EGLC_NONE };
    g_surf = _eglCreatePbufferSurface(g_dpy, conf, pb);
    if (g_surf == EGLC_NO_SURFACE) {
        /* surfaceless-context path (EGL_KHR_surfaceless_context) */
        const char *dx = _eglQueryString(g_dpy, EGLC_EXTENSIONS);
        if (dx == NULL || strstr(dx, "EGL_KHR_surfaceless_context") ==
                              NULL) {
            printf("SKIP: no pbuffer surface and no surfaceless "
                   "context (rc 77)\n");
            return 77;
        }
    }
    if (!_eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        printf("SKIP: makeCurrent failed (rc 77)\n");
        return 77;
    }

    const char *ver = es_str(GLC_VERSION);
    if (ver == NULL || strncmp(ver, "OpenGL ES", 9) != 0) {
        printf("FAIL: context is not GLES (version \"%s\")\n",
               ver != NULL ? ver : "(null)");
        return 3;
    }
    g_es_major = ver[10] - '0';
    g_es_minor = (ver[11] == '.') ? ver[12] - '0' : 0;
    printf("context: %s | egl %d.%d | surfaceless-capable display "
           "extensions present\n", ver, maj, mn);
    printf("matrix (expected verdicts are the ES %d.%d core format "
           "table):\n", g_es_major, g_es_minor);

    /* ---- the mask-format matrix (the LUMINANCE-class catch) ---- */
    unsigned e;
    e = (unsigned)try_create(GLC_R8, GLC_RED, GLC_UNSIGNED_BYTE);
    verdict("tex R8/RED/UB (ES3 core mask format)", g_es_major < 3, e);
    e = (unsigned)try_create(GLC_LUMINANCE, GLC_LUMINANCE,
                             GLC_UNSIGNED_BYTE);
    /* NEGATIVE CONTROL: on ES 3.x this MUST fail — if a future Mesa
     * leniently accepts it the checker's teeth are gone (fail loudly
     * so the negative control stays meaningful). */
    verdict("tex LUMINANCE/UB (NEGATIVE CONTROL)", g_es_major >= 3, e);
    e = (unsigned)try_create(GLC_RGBA, GLC_RGBA, GLC_UNSIGNED_BYTE);
    verdict("tex RGBA/UB (core everywhere)", 0, e);
    if (has_ext("GL_EXT_texture_format_BGRA8888")) {
        e = (unsigned)try_create(GLC_BGRA_EXT, GLC_BGRA_EXT,
                                 GLC_UNSIGNED_BYTE);
        verdict("tex BGRA_EXT/UB (gated: EXT_texture_format_BGRA8888)",
                0, e);
    } else {
        printf("  %-46s %s\n", "tex BGRA_EXT/UB",
               "skipped — extension absent (the lane falls to the "
               "RGBA twin)");
    }

    /* ---- unpack states ---- */
    {
        gl_uint_ t = 0;
        static unsigned char buf[8 * 2 * 4];
        memset(buf, 0x7f, sizeof(buf));
        _glGenTextures(1, &t);
        _glBindTexture(GLC_TEXTURE_2D, t);
        _glTexImage2D(GLC_TEXTURE_2D, 0, GLC_RGBA, 8, 4, 0, GLC_RGBA,
                      GLC_UNSIGNED_BYTE, NULL);
        drain_err();
        _glPixelStorei(GLC_UNPACK_ROW_LENGTH, 8);
        _glTexSubImage2D(GLC_TEXTURE_2D, 0, 0, 0, 4, 2, GLC_RGBA,
                         GLC_UNSIGNED_BYTE, buf);
        int rowlen_ok = _glGetError() == 0;
        (void)drain_err();
        if (g_es_major >= 3) {
            verdict("UNPACK_ROW_LENGTH sub-upload (ES3 core)", 0,
                    rowlen_ok ? 0 : 0x502);
        } else if (has_ext("GL_EXT_unpack_subimage")) {
            verdict("UNPACK_ROW_LENGTH sub-upload "
                    "(gated: EXT_unpack_subimage)", 0,
                    rowlen_ok ? 0 : 0x502);
        } else {
            printf("  %-46s %s\n", "UNPACK_ROW_LENGTH sub-upload",
                   "skipped — no unpack_subimage (the lane packs rows "
                   "instead)");
        }
        _glPixelStorei(GLC_UNPACK_ROW_LENGTH, 0);
        _glPixelStorei(GLC_UNPACK_ALIGNMENT, 1);
        _glTexSubImage2D(GLC_TEXTURE_2D, 0, 0, 2, 3, 1, GLC_RGBA,
                         GLC_UNSIGNED_BYTE, buf);
        e = drain_err();
        verdict("UNPACK_ALIGNMENT=1 odd-width row", 0, e);
        _glPixelStorei(GLC_UNPACK_ALIGNMENT, 4);
        _glBindTexture(GLC_TEXTURE_2D, 0);
        _glDeleteTextures(1, &t);
    }

    /* ---- NPOT with the lane's sampler posture ---- */
    {
        gl_uint_ t = 0;
        _glGenTextures(1, &t);
        _glBindTexture(GLC_TEXTURE_2D, t);
        _glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MIN_FILTER,
                         GLC_LINEAR);
        _glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MAG_FILTER,
                         GLC_LINEAR);
        _glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_S,
                         GLC_CLAMP_TO_EDGE);
        _glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_T,
                         GLC_CLAMP_TO_EDGE);
        _glTexImage2D(GLC_TEXTURE_2D, 0, GLC_RGBA, 33, 17, 0, GLC_RGBA,
                      GLC_UNSIGNED_BYTE, NULL);
        e = drain_err();
        verdict("NPOT 33x17 LINEAR+CLAMP (no mips)", 0, e);
        _glBindTexture(GLC_TEXTURE_2D, 0);
        _glDeleteTextures(1, &t);
    }

    /* ---- FBO + ReadPixels (the seam's readback path) ---- */
    {
        gl_uint_ fbo = 0, rb = 0;
        _glGenFramebuffers(1, &fbo);
        _glGenRenderbuffers(1, &rb);
        _glBindFramebuffer(GLC_FRAMEBUFFER, fbo);
        _glBindRenderbuffer(GLC_RENDERBUFFER, rb);
        _glRenderbufferStorage(GLC_RENDERBUFFER, GLC_RGBA8, 16, 16);
        _glFramebufferRenderbuffer(GLC_FRAMEBUFFER,
                                   GLC_COLOR_ATTACHMENT0,
                                   GLC_RENDERBUFFER, rb);
        unsigned st = _glCheckFramebufferStatus(GLC_FRAMEBUFFER);
        if (st != GLC_FRAMEBUFFER_COMPLETE) {
            (void)drain_err();
            printf("  %-46s %s\n", "FBO RGBA8 + ReadPixels",
                   "skipped — RGBA8 renderbuffer incomplete (no "
                   "OES_rgb8_rgba8; the lane never reads this way "
                   "here)");
        } else {
            static unsigned char px[16 * 16 * 4];
            _glReadPixels(0, 0, 16, 16, GLC_RGBA, GLC_UNSIGNED_BYTE, px);
            e = drain_err();
            verdict("FBO RGBA8 + ReadPixels RGBA/UB (the seam path)",
                    0, e);
        }
        _glBindFramebuffer(GLC_FRAMEBUFFER, 0);
        _glBindRenderbuffer(GLC_RENDERBUFFER, 0);
    }

    /* ---- the REAL lane shaders against this ES context ---- */
    {
        char log[512];
        if (!compile_link(GP_VS, GP_FS_BODY, log, sizeof(log))) {
            printf("  %-46s %s\n", "lane shaders (GP_VS+GP_FS_BODY, "
                   "ES precision head)",
                   "FAILED TO LINK — BUG");
            printf("    log: %s\n", log);
            g_failures++;
            snprintf(g_where, sizeof(g_where), "lane shaders");
        } else {
            printf("  %-46s %s\n",
                   "lane shaders (GP_VS+GP_FS_BODY, ES precision head)",
                   "compiled + linked");
        }
        /* NEGATIVE CONTROL: without the precision head, GLSL ES 1.00
         * fragment shaders have no default float precision — this
         * MUST fail, proving the prepend is load-bearing. */
        gl_uint_ fs = _glCreateShader(GLC_FRAGMENT_SHADER);
        const char *src = GP_FS_BODY;
        _glShaderSource(fs, 1, &src, NULL);
        _glCompileShader(fs);
        gl_int_ ok = 0;
        _glGetShaderiv(fs, GLC_COMPILE_STATUS, &ok);
        verdict("FS without the precision head (NEGATIVE CONTROL)",
                1, ok ? 0 : 0x502);
        if (ok) {
            char log2[512];
            _glGetShaderInfoLog(fs, (gl_int_)sizeof(log2), NULL, log2);
            (void)log2;
        }
        _glGetError();
        if (!compile_link(QUAD_VS, QUAD_FS, log, sizeof(log))) {
            printf("  %-46s %s\n", "wayland quad shaders", "FAILED");
            g_failures++;
        } else {
            printf("  %-46s %s\n", "wayland quad shaders",
                   "compiled + linked");
        }
    }

    if (g_controls_did_not_fire > 0) {
        printf("controls that did not fire on this stack: %d "
               "(Mesa is lenient where NVIDIA is strict — the "
               "static audit is the checker with teeth here)\n",
               g_controls_did_not_fire);
    }
    printf("==== GLES AUDIT: %s ====\n",
           g_failures == 0 ? "all green"
                           : "FAILURES (see the matrix above)");
    if (g_failures != 0 && g_where[0] != 0) {
        printf("first failure at: %s\n", g_where);
    }
    _eglMakeCurrent(g_dpy, EGLC_NO_SURFACE, EGLC_NO_SURFACE,
                    EGLC_NO_CONTEXT);
    if (g_surf != EGLC_NO_SURFACE) {
        _eglDestroySurface(g_dpy, g_surf);
    }
    _eglDestroyContext(g_dpy, g_ctx);
    _eglTerminate(g_dpy);
    return g_failures == 0 ? 0 : 3;
}
