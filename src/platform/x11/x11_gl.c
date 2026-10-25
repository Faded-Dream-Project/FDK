#define FDK_LOG_TAG "x11"

#include "platform/x11/x11_platform.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <X11/Xutil.h> /* XGetWindowAttributes, XVisualIDFromVisual */
#include <ctype.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* nanosleep — the self-check retry budget */

/* The GPU presentation lane (1.4.18) — implementation.
 *
 * Everything in this file deliberately avoids GL headers: the API
 * subset FDK needs (fixed-function compat profile: one textured
 * quad) has been wire-stable for two decades, and declaring it by
 * hand is what lets the toolkit build anywhere libX11 builds while
 * still reaching libGL when it exists. GLX functions are resolved
 * through glXGetProcAddress (the documented loader entry point);
 * core GL functions through dlsym on the library handle — GLVND
 * exports the core symbols directly and routes them through the
 * current context's dispatch, which keeps them valid across context
 * switches (glXGetProcAddress on core GL can predate the context in
 * GLVND's view; dlsym is the dependable path and costs nothing).
 *
 * The frame loop, once the lane is engaged for a window:
 *
 *      app draws into the staging buffer (fdk_u32, XRGB8888)
 *      present():   MakeCurrent
 *                   glTexSubImage2D of each damaged rect
 *                       (GL_UNPACK_ROW_LENGTH = staging stride)
 *                   one fullscreen quad, NEAREST, texenv REPLACE
 *                   [first COMPLETED frame: server-side pixel self-check]
 *                   glXSwapBuffers
 *
 * The texture is the retained "what the screen shows" copy — the
 * role the front slot plays in the SHM double buffer. Damage rects
 * make the texture differ from the staging buffer exactly where the
 * app drew, the quad repaints everything from the texture, and the
 * swap publishes it. No ShmCompletion routing, no slot-sync memcpy:
 * GL's own back buffer is transient between swaps (the quad redraws
 * all of it every frame), so the asynchronous-read hazard that
 * forces the two-slot dance in x11_surface.c simply does not exist
 * here.
 *
 * The pixel path is exact, not filtered: BGRA/UNSIGNED_BYTE reads
 * the staging buffer's little-endian bytes [B][G][R][X] in that
 * order, NEAREST at 1:1 maps window pixel (x, y) to texel (x, y),
 * and the X byte rides along as a texture alpha the RGB visual
 * discards. The 1.4.17 pixel battery pins the CPU path's bytes; the
 * 1.4.18 tests pin the GL path to those SAME bytes.
 *
 * Library pinning: libGL is dlopen'd once per process and NEVER
 * dlclose'd. Mesa/GLVND register process-global state (dispatch
 * tables, driver config caches, atexit handlers) that does not
 * survive unload cleanly, and FDK applications may create and
 * destroy contexts repeatedly in one process (every test does).
 * A pinned library is reachable memory as far as the sanitizers are
 * concerned — the same discipline fontconfig gets. The handle is
 * per-connection state, but dlopen's refcount makes repeated loads
 * cheap after the first.
 */

/* ---- hand-declared GL/GLX subset (no GL headers in this build) ---- */

typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef struct __GLXContextRec *GLXContext;

typedef void GLvoid;
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;

/* GL constants (values from the GL 1.4 / GLX 1.3 specifications). */
enum {
    GLC_TEXTURE_2D = 0x0DE1,
    GLC_RGB = 0x1907,
    GLC_RGBA = 0x1908,
    GLC_BGRA = 0x80E1,
    GLC_UNSIGNED_BYTE = 0x1401,
    GLC_NEAREST = 0x2600,
    GLC_CLAMP_TO_EDGE = 0x812F,
    GLC_TEXTURE_WRAP_S = 0x2802,
    GLC_TEXTURE_WRAP_T = 0x2803,
    GLC_TEXTURE_MIN_FILTER = 0x2801,
    GLC_TEXTURE_MAG_FILTER = 0x2800,
    GLC_TEXTURE_ENV = 0x2300,
    GLC_TEXTURE_ENV_MODE = 0x2200,
    GLC_REPLACE = 0x1E01,
    GLC_QUADS = 7, /* GL_TRIANGLES is 4 — a quad is 7 (the value, not the
                        * count; the 1.4.18 bring-up tripped over exactly this) */
    GLC_PROJECTION = 0x1701,
    GLC_MODELVIEW = 0x1700,
    GLC_UNPACK_ROW_LENGTH = 0x0CF2,
    GLC_UNPACK_ALIGNMENT = 0x0CF5,
    GLC_VENDOR = 0x1F00,
    GLC_RENDERER = 0x1F01
};

/* GLX constants. (Values verified against the system's
 * /usr/include/GL/glxtokens.h — the header ships with libx11-dev;
 * see the find_fbconfig_for_visual note for the one semantic this
 * build had to adapt to.) */
enum {
    GLXC_DOUBLEBUFFER = 5,
    GLXC_DRAWABLE_TYPE = 0x8010,
    GLXC_WINDOW_BIT = 0x00000001,
    GLXC_RENDER_TYPE = 0x8011,
    GLXC_RGBA_BIT = 0x00000001,
    GLXC_VISUAL_ID = 0x800B,
    GLXC_RGBA_TYPE = 0x8014
};

struct fdk_gl_fns {
    /* GLX (resolved via glXGetProcAddress). */
    int (*glXQueryExtension)(Display *, int *, int *);
    GLXFBConfig *(*glXGetFBConfigs)(Display *, int, int *);
    int (*glXGetFBConfigAttrib)(Display *, GLXFBConfig, int, int *);
    GLXContext (*glXCreateNewContext)(Display *, GLXFBConfig, int,
                                      GLXContext, int);
    int (*glXMakeCurrent)(Display *, unsigned long, GLXContext);
    void (*glXSwapBuffers)(Display *, unsigned long);
    void (*glXDestroyContext)(Display *, GLXContext);
    /* Core GL (resolved via dlsym — GLVND exports them). */
    void (*glBegin)(GLenum);
    void (*glEnd)(void);
    void (*glVertex2f)(GLfloat, GLfloat);
    void (*glTexCoord2f)(GLfloat, GLfloat);
    void (*glColor3f)(GLfloat, GLfloat, GLfloat);
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei);
    void (*glMatrixMode)(GLenum);
    void (*glLoadIdentity)(void);
    void (*glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble,
                    GLdouble);
    void (*glEnable)(GLenum);
    void (*glDisable)(GLenum);
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint,
                         GLenum, GLenum, const GLvoid *);
    void (*glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei,
                            GLenum, GLenum, const GLvoid *);
    void (*glTexParameteri)(GLenum, GLenum, GLint);
    void (*glTexEnvi)(GLenum, GLenum, GLint);
    void (*glPixelStorei)(GLenum, GLint);
    const char *(*glGetString)(GLenum);
    GLenum (*glGetError)(void);
    void (*glFinish)(void);
};

struct fdk_x11_gl {
    fdk_platform_connection *conn; /* owner */
    void *lib;                     /* dlopen handle; never dlclose'd */
    struct fdk_gl_fns fn;
    int refused;                   /* policy verdict: stop offering the
                                    * lane to further windows on this
                                    * connection (software renderer, no
                                    * usable context/visual, or a REAL
                                    * self-check failure — see below) */
    int selfcheck;                 /* first-frame verification verdict
                                    * for the WHOLE connection (the
                                    * driver-wide truth), so it is
                                    * decided ONCE and logged ONCE:
                                    *   0 = not yet verified (the next
                                    *       completed, viewable present
                                    *       of any window checks)
                                    *   1 = passed (later windows skip
                                    *       the probe entirely)
                                    *   2 = failed for REAL (persistent
                                    *       mismatch after retries —
                                    *       gl->refused is set too) */
    char renderer[80];             /* first context's GL_RENDERER string */
};

/* selfcheck states (the field above). */
enum {
    GL_SELFCHECK_UNVERIFIED = 0,
    GL_SELFCHECK_PASSED = 1,
    GL_SELFCHECK_FAILED = 2
};

struct fdk_x11_gl_window {
    struct fdk_x11_gl *gl;   /* owning lane state (never NULL)     */
    Window xwindow;          /* the GLX drawable this lane serves  */
    GLXContext ctx;          /* per-window GLX context            */
    GLuint tex;              /* window-sized texture (the retained
                              * "what the screen shows" copy)      */
    int w, h;                /* staging/texture size               */
    fdk_u32 *staging;        /* w*h CPU pixels the app draws into  */
    int tex_ok;              /* texture allocated at (w, h)        */
    int presents;            /* frames through the lane (first one
                              * self-verifies)                    */
    long fail_at;            /* FDK_GL_FAIL_AT test seam: poison
                              * exactly this present index (1-based),
                              * 0 = never                            */
};

/* ---- small utilities ----------------------------------------------------- */

/* Case-insensitive substring search (POSIX strcasestr is GNU-only).
 * Used only for the renderer-string policy, where a miss degrades to
 * "treat as hardware" — a safe direction (the lane then engages and
 * self-verifies). */
static const char *fdk_gl_strcasestr(const char *hay, const char *needle) {
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

/* Mesa's software rasterizers render correctly through the lane but
 * SLOWER than the plain SHM path (the pixels get software-rasterized
 * either way; GL adds a full-window composite pass on top), so the
 * default policy keeps them on the CPU path. "zink (llvmpipe ..."
 * is caught by the llvmpipe substring; virgl/d3d12 are REAL
 * (virtualized) GPUs and stay eligible. */
static int renderer_is_software(const char *renderer) {
    if (renderer == NULL || renderer[0] == '\0') {
        return 0; /* no string at all: trust the self-check */
    }
    return fdk_gl_strcasestr(renderer, "llvmpipe") != NULL ||
           fdk_gl_strcasestr(renderer, "softpipe") != NULL ||
           fdk_gl_strcasestr(renderer, "swrast") != NULL;
}

/* ---- loader -------------------------------------------------------------- */

/* POSIX dlsym/glXGetProcAddress return void*; ISO C (pedantic)
 * forbids direct object->function pointer conversion. The
 * dereference-through-void** idiom below is the POSIX-blessed way
 * to do this without a warning, and keeps the zero-warning build
 * rule intact. */
#define FDK_GL_STORE_FN(dst, sym) (*(void **)&(dst) = (sym))

/* ---- process-wide policy cache ------------------------------------------
 *
 * The expensive part of the probe is the CONTEXT: creating one and
 * making it current just long enough to read the renderer string.
 * The verdict is driver-wide, so repeating it for every connection
 * (the test suite opens ~a hundred) is pure waste — and under the
 * sanitizers it is slow waste. The cache is keyed on the two env
 * knobs' PRESENCE, so a test (or an app) that flips FDK_GL_FORCE /
 * FDK_NO_GL between connections gets a fresh decision; identical
 * knobs reuse the verdict. A refused verdict makes every later
 * connection's lane check a NULL return before any GL call. */
static int gl_policy_decided = 0;
static int gl_policy_refused = 0;
static int gl_policy_no_gl = 0;
static int gl_policy_force = 0;
static int gl_policy_selfcheck_failed = 0;

static void gl_policy_stamp(int refused, int no_gl, int force) {
    gl_policy_decided = 1;
    gl_policy_refused = refused;
    gl_policy_no_gl = no_gl;
    gl_policy_force = force;
    /* The self-check verdict is NOT stamped here: it is decided later
     * (at the first completed frame) by gl_selfcheck_stamp_failed,
     * under the same env-knob key discipline. */
}

/* A REAL self-check failure is driver-wide: stamp it into the
 * process-wide cache so later connections (same env knobs) refuse
 * the lane before any GL call — one decision, one log line, for the
 * whole process. Same key discipline as gl_policy_cached_refusal:
     * flipping FDK_GL_FORCE / FDK_NO_GL between connections gets a
     * fresh verdict. */
static void gl_selfcheck_stamp_failed(void) {
    gl_policy_selfcheck_failed = 1;
}

static int gl_policy_cached_refusal(void) {
    const int no_gl = getenv("FDK_NO_GL") != NULL;
    const int force = getenv("FDK_GL_FORCE") != NULL;
    if (gl_policy_decided && no_gl == gl_policy_no_gl &&
        force == gl_policy_force && gl_policy_refused) {
        return 1;
    }
    if (gl_policy_selfcheck_failed && !no_gl && !force) {
        return 1; /* a REAL readback failure decided earlier in this
                   * process, same knobs: refuse without re-probing */
    }
    if (gl_policy_selfcheck_failed && force) {
        return 0; /* FDK_GL_FORCE explicitly overrides even a decided
                   * real failure — the test seam's escape hatch */
    }
    return 0;
}

struct fdk_x11_gl *fdk_x11_gl_try_load(fdk_platform_connection *conn) {
    if (conn == NULL) {
        return NULL;
    }

    const int no_gl = getenv("FDK_NO_GL") != NULL;
    const int force = getenv("FDK_GL_FORCE") != NULL;

    if (gl_policy_cached_refusal()) {
        return NULL; /* same knobs, known-refused verdict */
    }

    /* The opt-out mirror of FDK_NO_MIT_SHM: never touch libGL. */
    if (no_gl) {
        FDK_DEBUG("FDK_NO_GL set — GPU present lane disabled");
        gl_policy_stamp(1, no_gl, force);
        return NULL;
    }

    void *lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == NULL) {
        const char *err = dlerror();
        FDK_DEBUG("libGL.so.1 not loadable (%s) — CPU present path only",
                  err != NULL ? err : "unknown error");
        gl_policy_stamp(1, no_gl, force);
        return NULL;
    }

    struct fdk_x11_gl *gl = fdk_alloc(sizeof(struct fdk_x11_gl));
    if (gl == NULL) {
        return NULL; /* the lib stays pinned; no dlclose, see header */
    }
    gl->conn = conn;
    gl->lib = lib;
    gl->refused = 0;
    gl->selfcheck = GL_SELFCHECK_UNVERIFIED;
    gl->renderer[0] = '\0';
    memset(&gl->fn, 0, sizeof(gl->fn));

    /* GLX entry points: the loader's own documented mechanism. */
    void *(*getproc)(const char *);
    FDK_GL_STORE_FN(getproc, dlsym(lib, "glXGetProcAddress"));
    if (getproc == NULL) {
        FDK_GL_STORE_FN(getproc, dlsym(lib, "glXGetProcAddressARB"));
    }
    if (getproc == NULL) {
        FDK_DEBUG("glXGetProcAddress unresolvable — CPU present path only");
        fdk_free(gl);
        return NULL;
    }
#define GLX_RESOLVE(field, name) \
    do { \
        void *sym_ = getproc(name); \
        if (sym_ == NULL) { \
            FDK_DEBUG("%s unresolvable — CPU present path only", name); \
            fdk_free(gl); \
            return NULL; \
        } \
        FDK_GL_STORE_FN(gl->fn.field, sym_); \
    } while (0)
    GLX_RESOLVE(glXQueryExtension, "glXQueryExtension");
    GLX_RESOLVE(glXGetFBConfigs, "glXGetFBConfigs");
    GLX_RESOLVE(glXGetFBConfigAttrib, "glXGetFBConfigAttrib");
    GLX_RESOLVE(glXCreateNewContext, "glXCreateNewContext");
    GLX_RESOLVE(glXMakeCurrent, "glXMakeCurrent");
    GLX_RESOLVE(glXSwapBuffers, "glXSwapBuffers");
    GLX_RESOLVE(glXDestroyContext, "glXDestroyContext");
#undef GLX_RESOLVE

    /* Core GL: GLVND exports the symbols; the dispatch follows the
     * current context at call time. Every one must exist or the lane
     * stays off — a partial function table is a crash, not a
     * degradation. */
#define GL_RESOLVE(field, name) \
    do { \
        void *sym_ = dlsym(lib, name); \
        if (sym_ == NULL) { \
            FDK_DEBUG("%s unresolvable — CPU present path only", name); \
            fdk_free(gl); \
            return NULL; \
        } \
        FDK_GL_STORE_FN(gl->fn.field, sym_); \
    } while (0)
    GL_RESOLVE(glBegin, "glBegin");
    GL_RESOLVE(glEnd, "glEnd");
    GL_RESOLVE(glVertex2f, "glVertex2f");
    GL_RESOLVE(glTexCoord2f, "glTexCoord2f");
    GL_RESOLVE(glColor3f, "glColor3f");
    GL_RESOLVE(glViewport, "glViewport");
    GL_RESOLVE(glMatrixMode, "glMatrixMode");
    GL_RESOLVE(glLoadIdentity, "glLoadIdentity");
    GL_RESOLVE(glOrtho, "glOrtho");
    GL_RESOLVE(glEnable, "glEnable");
    GL_RESOLVE(glDisable, "glDisable");
    GL_RESOLVE(glGenTextures, "glGenTextures");
    GL_RESOLVE(glBindTexture, "glBindTexture");
    GL_RESOLVE(glTexImage2D, "glTexImage2D");
    GL_RESOLVE(glTexSubImage2D, "glTexSubImage2D");
    GL_RESOLVE(glTexParameteri, "glTexParameteri");
    GL_RESOLVE(glTexEnvi, "glTexEnvi");
    GL_RESOLVE(glPixelStorei, "glPixelStorei");
    GL_RESOLVE(glGetString, "glGetString");
    GL_RESOLVE(glGetError, "glGetError");
    GL_RESOLVE(glFinish, "glFinish");
#undef GL_RESOLVE
#undef FDK_GL_STORE_FN

    /* The extension query itself: a server listing GLX among its
     * extensions can still fail the version query — trust the query. */
    int evb = 0, erb = 0;
    if (!gl->fn.glXQueryExtension(conn->display, &evb, &erb)) {
        FDK_DEBUG("GLX extension unavailable — CPU present path only");
        fdk_free(gl);
        return NULL;
    }

    FDK_DEBUG("libGL loaded, GLX present — GPU present lane available");
    gl_policy_stamp(0, no_gl, force);
    return gl;
}

const char *fdk_x11_gl_renderer(const struct fdk_x11_gl *gl) {
    return (gl != NULL && gl->renderer[0] != '\0') ? gl->renderer : NULL;
}

void fdk_x11_gl_shutdown(struct fdk_x11_gl *gl) {
    /* Every window must already be destroyed (x11_surface_cleanup runs
     * per window before the connection dies) — contexts die with their
     * windows. What remains is the state itself. The library handle is
     * deliberately NOT dlclose'd: the pinning note in the file header. */
    if (gl == NULL) {
        return;
    }
    fdk_free(gl);
}

/* TEST SEAM — see x11_gl.h. Not exported (the version script keeps
 * the fdk_x11_* internals out of the public surface). */
void fdk__x11_gl_test_reset(void) {
    gl_policy_selfcheck_failed = 0;
}

/* ---- per-window lane ------------------------------------------------------ */

/* Finds a doublebuffered RGBA FBConfig whose visual is exactly the
 * window's visual (GLX renders through the window's own visual; a
 * mismatched context cannot make the window current). Returns NULL
 * when none exists — the honest answer on servers whose GLX does not
 * cover the default visual, and the lane's cue to stand down.
 *
 * NOTE on glXGetFBConfigAttrib's return value: the spec says it
 * returns True/False for attribute validity, but the GLVND dispatch
 * observed in the wild returns 0 while STILL writing the correct
 * value (verified empirically against Mesa's swrast GLX — 240/240
 * queries "failed" with correct values). The lane therefore IGNORES
 * the return value and trusts the written attribute, with sentinels
 * pre-set so a genuinely aborted call cannot accidentally match. */
static GLXFBConfig find_fbconfig_for_visual(struct fdk_x11_gl *gl,
                                            VisualID want) {
    Display *dpy = gl->conn->display;
    int n = 0;
    GLXFBConfig *cfgs = gl->fn.glXGetFBConfigs(dpy, gl->conn->screen, &n);
    if (cfgs == NULL || n <= 0) {
        if (cfgs != NULL) {
            XFree(cfgs);
        }
        return NULL;
    }
    GLXFBConfig chosen = NULL;
    for (int i = 0; i < n && chosen == NULL; i++) {
        int vid = -1, dbl = -1, rtype = -1, dtype = -1;
        (void)gl->fn.glXGetFBConfigAttrib(dpy, cfgs[i], GLXC_VISUAL_ID,
                                          &vid);
        if (vid <= 0 || (VisualID)(unsigned)vid != want) {
            continue;
        }
        (void)gl->fn.glXGetFBConfigAttrib(dpy, cfgs[i], GLXC_DOUBLEBUFFER,
                                          &dbl);
        if (dbl != 1) {
            continue;
        }
        (void)gl->fn.glXGetFBConfigAttrib(dpy, cfgs[i], GLXC_RENDER_TYPE,
                                          &rtype);
        if ((rtype & GLXC_RGBA_BIT) == 0) {
            continue;
        }
        (void)gl->fn.glXGetFBConfigAttrib(dpy, cfgs[i], GLXC_DRAWABLE_TYPE,
                                          &dtype);
        if ((dtype & GLXC_WINDOW_BIT) == 0) {
            continue;
        }
        chosen = cfgs[i];
    }
    XFree(cfgs);
    return chosen;
}

/* One-time GL state for a freshly created context: the quad program
 * (fixed function: texture REPLACE, no lighting), NEAREST 1:1
 * sampling, clamped wrapping (the right/bottom edge must never wrap
 * around to texel column 0 — REPEAT would, at u exactly 1.0), and
 * byte-aligned unpacking. */
static void lane_setup_gl_state(struct fdk_x11_gl *gl) {
    gl->fn.glEnable(GLC_TEXTURE_2D);
    gl->fn.glTexEnvi(GLC_TEXTURE_ENV, GLC_TEXTURE_ENV_MODE, GLC_REPLACE);
    gl->fn.glPixelStorei(GLC_UNPACK_ALIGNMENT, 1);
}

struct fdk_x11_gl_window *fdk_x11_gl_window_try_init(
    struct fdk_x11_gl *gl, fdk_platform_window *pwindow) {
    if (gl == NULL || pwindow == NULL || gl->refused) {
        return NULL;
    }

    XWindowAttributes wa;
    if (XGetWindowAttributes(gl->conn->display, pwindow->xwindow, &wa) == 0) {
        return NULL; /* window is gone; the caller falls back */
    }
    GLXFBConfig cfg =
        find_fbconfig_for_visual(gl, XVisualIDFromVisual(wa.visual));
    if (cfg == NULL) {
        FDK_DEBUG("no GLX FBConfig for the window's visual — CPU present "
                  "path for this connection");
        gl->refused = 1; /* same visual for all FDK windows: stop trying */
        return NULL;
    }

    /* Direct first (local: DRI or client-side swrast); indirect second
     * (remote X, the only option there). Both failing = no context. */
    GLXContext ctx = gl->fn.glXCreateNewContext(gl->conn->display, cfg,
                                                GLXC_RGBA_TYPE, NULL, True);
    if (ctx == NULL) {
        ctx = gl->fn.glXCreateNewContext(gl->conn->display, cfg,
                                         GLXC_RGBA_TYPE, NULL, False);
    }
    if (ctx == NULL) {
        FDK_DEBUG("glXCreateNewContext failed — CPU present path for this "
                  "connection");
        gl->refused = 1;
        return NULL;
    }

    if (!gl->fn.glXMakeCurrent(gl->conn->display, pwindow->xwindow, ctx)) {
        FDK_WARN("glXMakeCurrent failed — CPU present path for this "
                 "connection");
        gl->fn.glXDestroyContext(gl->conn->display, ctx);
        gl->refused = 1;
        return NULL;
    }

    /* Renderer policy: the FIRST context on the connection decides for
     * all of them (the string is driver-wide, and it needs a current
     * context to exist at all — no scratch window, the first real one
     * pays the probe). Software rasterizers stay on the SHM path —
     * faster there, and fewer moving parts — unless FDK_GL_FORCE=1
     * explicitly asks to exercise the lane anyway. */
    const char *renderer = gl->fn.glGetString(GLC_RENDERER);
    if (renderer != NULL && gl->renderer[0] == '\0') {
        (void)snprintf(gl->renderer, sizeof(gl->renderer), "%s", renderer);
    }
    const int forced = getenv("FDK_GL_FORCE") != NULL;
    if (renderer_is_software(gl->renderer) && !forced) {
        FDK_INFO("GL renderer is software (\"%s\") — staying on the CPU "
                 "present path (FDK_GL_FORCE=1 overrides)",
                 gl->renderer);
        gl->fn.glXMakeCurrent(gl->conn->display, None, NULL);
        gl->fn.glXDestroyContext(gl->conn->display, ctx);
        gl->refused = 1;
        /* Driver-wide verdict: later connections with the same knobs
         * skip the whole context dance. */
        gl_policy_stamp(1, getenv("FDK_NO_GL") != NULL, forced);
        return NULL;
    }

    struct fdk_x11_gl_window *gwin =
        fdk_alloc(sizeof(struct fdk_x11_gl_window));
    if (gwin == NULL) {
        gl->fn.glXMakeCurrent(gl->conn->display, None, NULL);
        gl->fn.glXDestroyContext(gl->conn->display, ctx);
        return NULL;
    }
    gwin->gl = gl;
    gwin->xwindow = pwindow->xwindow;
    gwin->ctx = ctx;
    gwin->tex = 0;
    gwin->w = 0;
    gwin->h = 0;
    gwin->staging = NULL;
    gwin->tex_ok = 0;
    gwin->presents = 0;
    gwin->fail_at = 0;

    /* The test seam (documented like FDK_NO_MIT_SHM): poison exactly
     * the Nth present so the permanent-downgrade path can be exercised
     * end to end against a real server. */
    const char *fail_at = getenv("FDK_GL_FAIL_AT");
    if (fail_at != NULL && fail_at[0] != '\0') {
        long v = strtol(fail_at, NULL, 10);
        gwin->fail_at = v > 0 ? v : 0;
    }

    gl->fn.glGenTextures(1, &gwin->tex);
    gl->fn.glBindTexture(GLC_TEXTURE_2D, gwin->tex);
    gl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MIN_FILTER,
                           GLC_NEAREST);
    gl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_MAG_FILTER,
                           GLC_NEAREST);
    gl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_S,
                           GLC_CLAMP_TO_EDGE);
    gl->fn.glTexParameteri(GLC_TEXTURE_2D, GLC_TEXTURE_WRAP_T,
                           GLC_CLAMP_TO_EDGE);
    lane_setup_gl_state(gl);
    (void)gl->fn.glGetError(); /* setup-time errors surface at present */

    /* Leave the context unbound: another window's init (or present)
     * rebinds as needed. Context/GL-object state survives unbinding. */
    gl->fn.glXMakeCurrent(gl->conn->display, None, NULL);

    FDK_INFO("GPU present lane active (renderer \"%s\")",
             gl->renderer[0] != '\0' ? gl->renderer : "unknown");
    return gwin;
}

void fdk_x11_gl_window_destroy(struct fdk_x11_gl *gl,
                               struct fdk_x11_gl_window *gwin) {
    if (gwin == NULL) {
        return;
    }
    if (gl != NULL && !gl->conn->display_dead) {
        /* A context must not be current on another drawable when it is
         * destroyed — unbind first (single UI thread; nothing else can
         * hold it). */
        gl->fn.glXMakeCurrent(gl->conn->display, None, NULL);
        gl->fn.glXDestroyContext(gl->conn->display, gwin->ctx);
    }
    if (gwin->staging != NULL) {
        fdk_free(gwin->staging);
    }
    fdk_free(gwin);
}

fdk_u32 *fdk_x11_gl_window_staging(struct fdk_x11_gl_window *gwin, int w,
                                   int h) {
    if (gwin == NULL || w <= 0 || h <= 0) {
        return NULL;
    }
    if (gwin->staging != NULL && gwin->w == w && gwin->h == h) {
        return gwin->staging;
    }
    fdk_u32 *fresh = fdk_alloc(sizeof(fdk_u32) * (size_t)w * (size_t)h);
    if (fresh == NULL) {
        return NULL; /* OOM: the caller reports it like slot_create's */
    }
    if (gwin->staging != NULL) {
        fdk_free(gwin->staging);
    }
    gwin->staging = fresh;
    gwin->w = w;
    gwin->h = h;
    gwin->tex_ok = 0; /* the texture re-allocates at the new size on the
                       * next present, which is a full-damage frame by
                       * the surface contract (new size = new content) */
    return fresh;
}

/* Draws the fullscreen quad mapping the whole texture 1:1 onto the
 * whole window. Orientation: the staging buffer's row 0 is the TOP
 * row (FDK/window coordinates); glOrtho(0, w, h, 0) flips Y so that
 * vertex (x, y) lands on window pixel (x, y) directly, and the
 * matching texcoord (x/w, y/h) samples texel (x, y). self_check()
 * verifies this with a bottom-left-origin readback flipped back. */
static void draw_fullscreen_quad(struct fdk_x11_gl *gl, int w, int h) {
    gl->fn.glViewport(0, 0, (GLsizei)w, (GLsizei)h);
    gl->fn.glMatrixMode(GLC_PROJECTION);
    gl->fn.glLoadIdentity();
    gl->fn.glOrtho(0.0, (GLdouble)w, (GLdouble)h, 0.0, -1.0, 1.0);
    gl->fn.glMatrixMode(GLC_MODELVIEW);
    gl->fn.glLoadIdentity();
    gl->fn.glBegin(GLC_QUADS);
    gl->fn.glTexCoord2f(0.0f, 0.0f);
    gl->fn.glVertex2f(0.0f, 0.0f);
    gl->fn.glTexCoord2f(1.0f, 0.0f);
    gl->fn.glVertex2f((GLfloat)w, 0.0f);
    gl->fn.glTexCoord2f(1.0f, 1.0f);
    gl->fn.glVertex2f((GLfloat)w, (GLfloat)h);
    gl->fn.glTexCoord2f(0.0f, 1.0f);
    gl->fn.glVertex2f(0.0f, (GLfloat)h);
    gl->fn.glEnd();
}

/* First-COMPLETED-frame self-verification, SERVER-side: after the
 * swap, read four spread pixels of the actual window through FDK's
 * own display connection (XGetImage — synchronous, exact, and the
 * same channel the integration tests trust). A mismatch means the
 * driver's composite is wrong — the lane downgrades and the classic
 * path immediately re-presents the correct frame, so the worst case
 * on a broken driver is one transient frame, never a persistent
 * wrong image. Reading the window (not the GL back buffer) also
 * verifies the actual contract: what the screen shows. (A GL-side
 * glReadPixels pre-swap check was tried first and abandoned: this
 * stack's read-back-buffer behavior is quirky — glReadBuffer raises
 * INVALID_ENUM for either candidate constant, and the
 * glXGetProcAddress-resolved readback disagreed with the visible
 * framebuffer.)
 *
 * THE 1.1.0 REWORK — three verdicts, not one (found live on an
 * NVIDIA GTX 1050 Ti desktop, 2026-10-25):
 *
 *   INCONCLUSIVE (retry later, never a failure):
 *     - the window is not viewable yet (map_state != IsViewable). A
 *       popup whose first present races its own map, a window the
 *       WM has not mapped: XGetImage would raise BadMatch (request
 *       73) — which proves NOTHING about the driver.
 *     - XGetImage itself fails (NULL) for any reason. BadMatch is
 *       the documented answer for not-viewable/out-of-bounds; it is
 *       a property of the WINDOW STATE, not of the GPU lane.
 *     Inconclusive means: this present still counts as successful,
 *     and the NEXT completed present of ANY window on the connection
 *     re-attempts the check. A window that never becomes viewable
 *     simply never verifies — verification is opportunistic.
 *
 *   PASS: the probes match. The verdict is stored on the CONNECTION
 *     (gl->selfcheck): later windows never probe again — no per-popup
 *     XGetImage, no per-popup GLX-context churn, no repeated warnings.
 *
 *   FAIL (a REAL driver problem): the probes mismatch PERSISTENTLY —
 *     after bounded retries that ride out present-latency timing.
 *     The swap being issued does not mean its pixels are in the
 *     window's visible content yet: DRI3/Present schedules the actual
 *     flip at vblank, and a compositing WM re-composites on its own
 *     clock. The first XGetImage can therefore read the window's
 *     PRE-FRAME content — typically its creation background pixel
 *     (the live report's "server (000000) vs CPU (color)" was exactly
 *     the fdk-shell bar's BlackPixel background). One retry budget
 *     (~3 vblanks) separates the timing case (a retry matches: PASS)
 *     from the real limitation (every retry mismatches: FAIL). Only
 *     the REAL case refuses the lane for the whole PROCESS (the
 *     verdict is driver-wide — see gl_selfcheck_stamp_failed) and
 *     logs once.
 *
 * FDK_GL_SELFCHECK_POISON=1 (test seam, the FDK_GL_FAIL_AT shape):
 * force every probe comparison to mismatch — exercising the retry
 * budget, the REAL-failure verdict, and the process-wide refusal
 * end to end. FDK_GL_FORCE=1 overrides a decided failure verdict so
 * later test groups can still engage the lane.
 */
#define GL_SELFCHECK_ATTEMPTS 3
#define GL_SELFCHECK_RETRY_MS 9

enum {
    GL_CHK_PASS = 0,
    GL_CHK_FAIL = 1,         /* REAL: persistent mismatch        */
    GL_CHK_INCONCLUSIVE = -1 /* not viewable / read failed: retry  */
};

/* One probe pass: reads the four quarter-grid pixels and compares
 * them to the staging buffer. Returns 0 = all match, 1 = at least
 * one mismatch, -1 = a read failed (inconclusive). Probes are
 * clamped into the SERVER-reported window bounds — the region must
 * be inside the window or XGetImage raises BadMatch by contract. */
static int self_check_probes(struct fdk_x11_gl *gl,
                              struct fdk_x11_gl_window *gwin, int win_w,
                              int win_h) {
    Display *dpy = gl->conn->display;
    int w = gwin->w;
    int h = gwin->h;
    const int poisoned = getenv("FDK_GL_SELFCHECK_POISON") != NULL;
    for (int i = 0; i < 4; i++) {
        /* quarter-grid probes: w/8, 3w/8, 5w/8, 7w/8 (and same in y) */
        int sx = (w * (i * 2 + 1)) / 8;
        int sy = (h * (i * 2 + 1)) / 8;
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
        if (sx >= win_w) sx = win_w - 1; /* staging and server sizes
                                          * can differ mid-resize */
        if (sy >= win_h) sy = win_h - 1;
        if (sx < 0 || sy < 0) {
            continue; /* degenerate 1px window: nothing to probe */
        }
        XImage *im = XGetImage(dpy, gwin->xwindow, sx, sy, 1, 1, ~0UL,
                               ZPixmap);
        if (im == NULL) {
            /* BadMatch (not viewable / out of bounds) or any other
             * read error: a property of the window state, not of the
             * driver — inconclusive, retried at the next present. */
            return -1;
        }
        unsigned long got = (unsigned long)XGetPixel(im, 0, 0) & 0xFFFFFFUL;
        XDestroyImage(im);
        fdk_u32 want = gwin->staging[(size_t)sy * (size_t)w + (size_t)sx];
        if (poisoned || got != (unsigned long)(want & 0xFFFFFFUL)) {
            return 1;
        }
    }
    return 0;
}

static int self_check(struct fdk_x11_gl *gl, struct fdk_x11_gl_window *gwin) {
    Display *dpy = gl->conn->display;

    /* The viewability gate: XGetWindowAttributes is a round trip —
     * by the time it answers, the server has processed every request
     * before it (the map, the swap). map_state IsViewable means the
     * window is mapped AND every ancestor is: the honest programmatic
     * equivalent of "wait for MapNotify/Expose". Anything else is
     * INCONCLUSIVE — a not-yet-viewable window proves nothing. */
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, gwin->xwindow, &wa) == 0) {
        return GL_CHK_INCONCLUSIVE; /* window gone: not our verdict */
    }
    if (wa.map_state != IsViewable || wa.width < 1 || wa.height < 1) {
        return GL_CHK_INCONCLUSIVE;
    }

    for (int attempt = 0; attempt < GL_SELFCHECK_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            /* Ride out present latency: the swap was issued, but
             * vblank-scheduled presents (DRI3/Present, compositing
             * WMs) land the pixels up to a couple of vblanks later.
             * XSync first (drain the wire), then give the compositor
             * a slice of a vblank before re-reading. */
            XSync(dpy, False);
            struct timespec ts = { 0, GL_SELFCHECK_RETRY_MS * 1000000L };
            nanosleep(&ts, NULL);
        }
        int rc = self_check_probes(gl, gwin, wa.width, wa.height);
        if (rc == 0) {
            if (attempt > 0) {
                FDK_INFO("GPU lane self-check passed on retry %d — the "
                         "first read raced the present landing "
                         "(vblank/compositor latency)", attempt);
            }
            return GL_CHK_PASS;
        }
        if (rc < 0) {
            return GL_CHK_INCONCLUSIVE; /* read failed: not a verdict */
        }
        /* mismatch — maybe timing; the loop decides after the budget */
    }
    return GL_CHK_FAIL;
}

int fdk_x11_gl_window_present(struct fdk_x11_gl_window *gwin,
                              const fdk_platform_damage *damage, int w,
                              int h) {
    if (gwin == NULL || damage == NULL || w <= 0 || h <= 0 ||
        gwin->staging == NULL || gwin->w != w || gwin->h != h) {
        return -1;
    }
    struct fdk_x11_gl *gl = gwin->gl;
    Display *dpy = gl->conn->display;

    /* The test seam: poison exactly the Nth present (1-based) without
     * touching GL — the caller's downgrade path must recover. */
    if (gwin->fail_at > 0 && gwin->presents + 1 == gwin->fail_at) {
        gwin->presents++;
        return -1;
    }

    if (!gl->fn.glXMakeCurrent(dpy, (unsigned long)gwin->xwindow,
                               gwin->ctx)) {
        return -1; /* context lost: permanent downgrade */
    }
    (void)gl->fn.glGetError(); /* drain pre-existing errors — they are
                                * not this frame's verdict */

    gl->fn.glBindTexture(GLC_TEXTURE_2D, gwin->tex);
    gl->fn.glPixelStorei(GLC_UNPACK_ROW_LENGTH, w);

    /* Texture (re)allocation at a new size — a full-damage frame by
     * the surface contract, so no content needs preserving. */
    if (!gwin->tex_ok) {
        gl->fn.glTexImage2D(GLC_TEXTURE_2D, 0, (GLint)GLC_RGBA, (GLsizei)w,
                            (GLsizei)h, 0, GLC_BGRA, GLC_UNSIGNED_BYTE,
                            gwin->staging);
        gwin->tex_ok = 1;
    } else {
        /* Same damage coarsening as the SHM path: clamp the rects,
         * count live area, and switch to one whole-image upload at
         * >= 75% damaged (one big transfer beats a burst of small
         * ones through the same driver paths). */
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
            gl->fn.glTexSubImage2D(GLC_TEXTURE_2D, 0, 0, 0, (GLsizei)w,
                                   (GLsizei)h, GLC_BGRA, GLC_UNSIGNED_BYTE,
                                   gwin->staging);
        } else {
            for (int i = 0; i < live; i++) {
                const fdk_u32 *src = gwin->staging +
                                     (size_t)ry0[i] * (size_t)w +
                                     (size_t)rx0[i];
                gl->fn.glTexSubImage2D(
                    GLC_TEXTURE_2D, 0, (GLint)rx0[i], (GLint)ry0[i],
                    (GLsizei)(rx1[i] - rx0[i]), (GLsizei)(ry1[i] - ry0[i]),
                    GLC_BGRA, GLC_UNSIGNED_BYTE, src);
            }
        }
    }

    draw_fullscreen_quad(gl, w, h);

    /* Any GL error from this frame's upload/draw is a driver problem
     * the lane refuses to paper over: downgrade, let the SHM path
     * carry the frame. */
    if (gl->fn.glGetError() != 0) {
        FDK_WARN("GL error during GPU present — downgrading this window "
                 "to the CPU path");
        return -1;
    }

    gl->fn.glXSwapBuffers(dpy, (unsigned long)gwin->xwindow);
    XFlush(dpy);
    gwin->presents++;

    /* The self-check verdict belongs to the CONNECTION (the driver),
     * not the window: while unverified, the first window whose frame
     * completes on a VIEWABLE window decides for every later one.
     * INCONCLUSIVE (not viewable / transient read failure) defers to
     * the next completed present — the present itself SUCCEEDED, so
     * zero is returned and the window stays on the lane. A REAL
     * failure (persistent mismatch after the retry budget) refuses
     * the lane for this connection AND stamps the process-wide
     * policy (same env-knob key as the renderer verdict), so every
     * later window — popups included — engages nothing, probes
     * nothing, and logs nothing further. Exactly one warning per
     * process either way. */
    if (gl->selfcheck == GL_SELFCHECK_UNVERIFIED) {
        int verdict = self_check(gl, gwin);
        if (verdict == GL_CHK_PASS) {
            gl->selfcheck = GL_SELFCHECK_PASSED;
        } else if (verdict == GL_CHK_FAIL) {
            FDK_WARN("GPU lane self-check: persistent mismatch after "
                     "%d reads — a real driver readback limitation "
                     "(renderer \"%s\"), refusing the GPU present "
                     "lane for this process",
                     GL_SELFCHECK_ATTEMPTS,
                     gl->renderer[0] != '\0' ? gl->renderer : "unknown");
            gl->selfcheck = GL_SELFCHECK_FAILED;
            gl->refused = 1;
            gl_selfcheck_stamp_failed();
            return -1; /* this window downgrades; later ones never
                        * engage — the caller salvages the frame */
        }
        /* GL_CHK_INCONCLUSIVE: fall through — the frame is fine, the
         * verdict stays pending for the next completed present. */
    }
    return 0;
}
