/*
 * gpaint.c — the GPU paint engine: fdk_surface draw commands executed
 * as batched GPU draws instead of CPU rasterization.
 *
 * Design contract (mirrors the lanes' posture):
 *
 *  - ONE shader program; every primitive is an SDF-evaluated quad.
 *    Rects (radius 0..n), circles, strokes, shadows and vertical
 *    gradients are all the same fragment math over different
 *    per-instance parameters; textures and glyph-coverage quads ride
 *    the same vertex stream with a texture flag. Quads are expanded
 *    CPU-side into one interleaved vertex buffer (no instancing —
 *    GLES2 has none) and flushed on state change or buffer fullness.
 *
 *  - ZERO new build dependencies: GL entry points are resolved by
 *    dlopen (libGLESv2.so.2, libGL.so.1 fallback) exactly like the
 *    lanes resolve EGL/GLX. The shader is written in the GLES2/GLSL
 *    100 subset that desktop compatibility-profile GLSL 110 also
 *    compiles (attribute/varying/gl_FragColor), so one source runs on
 *    both flavors; the ES precision statement is prepended only when
 *    the context reports "OpenGL ES".
 *
 *  - COORDINATES: callers speak top-left-origin pixel rects (the
 *    fdk_surface contract). The engine flips Y at vertex emit and in
 *    the scissor; the fragment shader works in rect-local pixels.
 *    Clips: the surface clip stack is mirrored; the scissor is the
 *    intersection of the stack (a rounded clip degrades to its
 *    bounding box — widgets clip with plain rects; recorded honestly).
 *
 *  - GLYPHS: blend_mask coverage blits are the text path. Coverage
 *    bitmaps land in a 1024x1024 R8/LUMINANCE shelf-packed atlas
 *    keyed by a 64-bit FNV-1a of the bitmap bytes (identity-free —
 *    the mask blit does not carry glyph ids). On a full atlas the
 *    pending batch flushes first, then the shelf resets; already-
 *    emitted quads keep sampling the still-intact atlas during that
 *    flush. Hash collisions reuse an entry (2^-64 scale risk, noted,
 *    not hidden).
 *
 *  - TEXTURES: the public fdk_texture API and offscreen-surface blits
 *    share one registry keyed by source pointer, created lazily in
 *    the window's lane context (the shared-context chain makes names
 *    portable across windows — P2).
 *
 *  - FAILURE = DOWNGRADE, never a broken frame: init returns 0 and
 *    the surface stays CPU; a GL error at flush logs once and marks
 *    the state dead (the lane's existing downgrade machinery takes it
 *    from there). No partial-batch guessing, no silent fallback inside
 *    a frame.
 *
 * Not part of the public API — never installed.
 */

#include "gpaint.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* GL entry points (the lane posture: hand-declared, dlopen'd)         */

#define GL_FLOAT 0x1406
#define GL_ARRAY_BUFFER 0x8892
#define GL_STREAM_DRAW 0x88E0
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_LUMINANCE 0x1909
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TRIANGLES 0x0004
#define GL_TEXTURE0 0x84C0
#define GL_NO_ERROR 0

typedef unsigned gl_uint;
typedef int gl_int;
typedef size_t gl_sizeiptr;

struct fdk_gpu_procs {
    void *lib;
    const unsigned char *(*GetString)(unsigned name);
    unsigned (*GetError)(void);
    void (*Viewport)(int, int, int, int);
    void (*Scissor)(int, int, int, int);
    void (*Enable)(unsigned);
    void (*Disable)(unsigned);
    void (*BlendFunc)(unsigned, unsigned);
    void (*PixelStorei)(unsigned, int);
    gl_uint (*CreateShader)(unsigned);
    void (*ShaderSource)(gl_uint, int, const char **, const gl_int *);
    void (*CompileShader)(gl_uint);
    void (*GetShaderiv)(gl_uint, unsigned, gl_int *);
    void (*GetShaderInfoLog)(gl_uint, gl_sizeiptr, gl_sizeiptr *, char *);
    gl_uint (*CreateProgram)(void);
    void (*AttachShader)(gl_uint, gl_uint);
    void (*LinkProgram)(gl_uint);
    void (*GetProgramiv)(gl_uint, unsigned, gl_int *);
    gl_int (*GetUniformLocation)(gl_uint, const char *);
    void (*UseProgram)(gl_uint);
    void (*Uniform2f)(gl_int, float, float);
    void (*GenTextures)(int, gl_uint *);
    void (*BindTexture)(unsigned, gl_uint);
    void (*TexParameteri)(unsigned, unsigned, unsigned);
    void (*TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned,
                       const void *);
    void (*TexSubImage2D)(unsigned, int, int, int, int, int, unsigned,
                          unsigned, const void *);
    void (*DeleteTextures)(int, const gl_uint *);
    void (*GenBuffers)(int, gl_uint *);
    void (*BindBuffer)(unsigned, gl_uint);
    void (*BufferData)(unsigned, gl_sizeiptr, const void *, unsigned);
    void (*VertexAttribPointer)(unsigned, int, unsigned, unsigned char, int,
                                const void *);
    void (*EnableVertexAttribArray)(unsigned);
    void (*DrawArrays)(unsigned, int, int);
    void (*ActiveTexture)(unsigned);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void (*Finish)(void);
};

static struct fdk_gpu_procs g_gp;
static int g_gp_ok;

static void *gp_sym(const char *name) {
    void *p = dlsym(g_gp.lib, name);
    if (p == NULL) {
        g_gp_ok = 0;
    }
    return p;
}

int fdk__gpu_procs_init(void) {
    if (g_gp_ok) {
        return 1;
    }
    /* Presence check only — the RESOLUTION happens in gp_resolve_procs
     * once a context is current, because libGLESv2's and libGL's
     * dispatch tables are keyed to their own context flavors: a desktop
     * GLX context makes libGLESv2's glGetString return NULL (and vice
     * versa). The engine picks the library that ANSWERS for the current
     * context at bring-up time. */
    const char *names[] = { "libGLESv2.so.2", "libGLESv2.so",
                            "libGL.so.1",     "libGL.so" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (lib != NULL) {
            dlclose(lib);
            return 1;
        }
    }
    return 0;
}

/* Resolve the proc table from the library that serves the CURRENT
 * context. 1 = table ready; 0 = no GL library at all (fatal);
 * -1 = libraries exist but none answers for the current context
 * (soft: the caller is running before the lane's first makeCurrent). */
static int gp_resolve_procs(void) {
    if (g_gp_ok) {
        return 1;
    }
    const char *names[] = { "libGLESv2.so.2", "libGLESv2.so",
                            "libGL.so.1",     "libGL.so" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        void *lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (lib == NULL) {
            continue;
        }
        const unsigned char *(*gs)(unsigned) =
            (const unsigned char *(*)(unsigned))dlsym(lib, "glGetString");
        /* The candidate must answer for the CURRENT context
         * (GL_RENDERER non-NULL = this dispatch owns it). */
        if (gs == NULL || gs(0x1F01) == NULL) {
            continue;
        }
        g_gp.lib = lib;
        g_gp_ok = 1;
#define GP(field, sym) g_gp.field = (void *)gp_sym(sym)
        GP(GetString, "glGetString");
        GP(GetError, "glGetError");
        GP(Viewport, "glViewport");
        GP(Scissor, "glScissor");
        GP(Enable, "glEnable");
        GP(Disable, "glDisable");
        GP(BlendFunc, "glBlendFunc");
        GP(PixelStorei, "glPixelStorei");
        GP(CreateShader, "glCreateShader");
        GP(ShaderSource, "glShaderSource");
        GP(CompileShader, "glCompileShader");
        GP(GetShaderiv, "glGetShaderiv");
        GP(GetShaderInfoLog, "glGetShaderInfoLog");
        GP(CreateProgram, "glCreateProgram");
        GP(AttachShader, "glAttachShader");
        GP(LinkProgram, "glLinkProgram");
        GP(GetProgramiv, "glGetProgramiv");
        GP(GetUniformLocation, "glGetUniformLocation");
        GP(UseProgram, "glUseProgram");
        GP(Uniform2f, "glUniform2f");
        GP(GenTextures, "glGenTextures");
        GP(BindTexture, "glBindTexture");
        GP(TexParameteri, "glTexParameteri");
        GP(TexImage2D, "glTexImage2D");
        GP(TexSubImage2D, "glTexSubImage2D");
        GP(DeleteTextures, "glDeleteTextures");
        GP(GenBuffers, "glGenBuffers");
        GP(BindBuffer, "glBindBuffer");
        GP(BufferData, "glBufferData");
        GP(VertexAttribPointer, "glVertexAttribPointer");
        GP(EnableVertexAttribArray, "glEnableVertexAttribArray");
        GP(DrawArrays, "glDrawArrays");
        GP(ActiveTexture, "glActiveTexture");
        GP(ReadPixels, "glReadPixels");
        GP(Finish, "glFinish");
#undef GP
        if (!g_gp_ok) {
            g_gp.lib = NULL;
            continue;
        }
        return 1;
    }
    return -1; /* libs exist, none current */
}

/* ------------------------------------------------------------------ */
/* Shaders                                                             */

static const char *GP_VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "attribute vec4 a_aux0;\n"
    "attribute vec4 a_aux1;\n"
    "attribute vec4 a_aux2;\n"
    "attribute vec4 a_clip;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_aux0;\n"
    "varying vec4 v_aux1;\n"
    "varying vec4 v_aux2;\n"
    "varying vec4 v_clip;\n"
    "uniform vec2 u_vp;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    v_aux0 = a_aux0;\n"
    "    v_aux1 = a_aux1;\n"
    "    v_aux2 = a_aux2;\n"
    "    v_clip = a_clip;\n"
    "    vec2 c = a_pos / u_vp * 2.0 - 1.0;\n"
    "    gl_Position = vec4(c.x, -c.y, 0.0, 1.0);\n"
    "}\n";

static const char *GP_FS_BODY =
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_aux0;\n"
    "varying vec4 v_aux1;\n"
    "varying vec4 v_aux2;\n"
    "varying vec4 v_clip;\n"
    "uniform sampler2D u_tex;\n"
    "float sd_rect(vec2 p, vec2 b) {\n"
    "    vec2 d = abs(p) - b;\n"
    "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
    "}\n"
    "void main() {\n"
    "    if (gl_FragCoord.x < v_clip.x || gl_FragCoord.x > v_clip.z ||\n"
    "        gl_FragCoord.y < v_clip.y || gl_FragCoord.y > v_clip.w) {\n"
    "        discard;\n"
    "    }\n"
    "    float mode = v_aux0.x;\n"
    "    vec2 half_sz = v_aux2.zw * 0.5;\n"
    "    vec2 p = v_uv;\n" /* rect-local pixels */
    "    vec4 c = vec4(0.0);\n"
    "    if (mode < 0.5) {\n" /* fill: rect (radius) or circle */
    "        float d;\n"
    "        if (v_aux0.w < 0.5) {\n"
    /*  rounded box: sd of the box inset by r, MINUS r (the standard
        rounded-box SDF — without the -r the shape is the inset box) */
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y)) -\n"
    "                v_aux0.y;\n"
    "        } else {\n"
    "            d = length(p - half_sz) - min(half_sz.x, half_sz.y);\n"
    "        }\n"
    "        float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 1.5) {\n" /* stroke: ring at stroke_half */
    "        float d;\n"
    "        if (v_aux0.w < 0.5) {\n"
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y)) -\n"
    "                v_aux0.y;\n"
    "        } else {\n"
    "            d = length(p - half_sz) - min(half_sz.x, half_sz.y);\n"
    "        }\n"
    /*  the CPU's 1px stroke paints the boundary PIXEL ring: its
        center sits half a pixel inside the shape edge. Center the
        GPU ring there too (stroke_half is in pixels). */
    "        float ring = abs(abs(d) - (v_aux0.z - 0.5)) - 0.5;\n"
    "        float a = clamp(0.5 - ring, 0.0, 1.0);\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 2.5) {\n" /* vertical gradient */
    "        float t = clamp(p.y / max(v_aux2.w, 1.0), 0.0, 1.0);\n"
    "        vec3 rgb = mix(v_color.rgb, v_aux1.rgb, t);\n"
    "        float alpha = mix(v_color.a, v_aux1.a, t);\n"
    "        c = vec4(rgb, alpha);\n"
    "    } else if (mode < 3.5) {\n" /* shadow: gaussian-ish falloff */
    "        float d = max(sd_rect(p - half_sz, half_sz), 0.0);\n"
    "        float sigma = max(v_aux0.z, 0.5);\n"
    "        float a = exp(-(d * d) / (2.0 * sigma * sigma));\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 4.5) {\n" /* texture RGBA (uv 0..1) */
    "        vec4 t = texture2D(u_tex, v_uv);\n"
    "        c = vec4(t.rgb * v_color.rgb, t.a * v_color.a) *\n"
    "            v_aux2.x;\n" /* global alpha rides aux2.x */
    "    } else if (mode < 5.5) {\n" /* glyph coverage (LUMINANCE) */
    "        float l = texture2D(u_tex, v_uv).r;\n"
    "        c = vec4(v_color.rgb, v_color.a * l * v_aux2.x);\n"
    "    } else {\n"             /* mode 6: texture, OPAQUE (surface
                                     blits — the XRGB X byte is zero,
                                     alpha must not come from it) */
    "        vec4 t = texture2D(u_tex, v_uv);\n"
    "        c = vec4(t.rgb * v_color.rgb, v_color.a) * v_aux2.x;\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* Per-surface engine state                                            */

#define GP_QUAD_FLOATS 24 /* 6 verts x (pos2 uv2 col4 aux0..2 clip4) */
#define GP_QUAD_VERTS 6
#define GP_MAX_QUADS 4096
#define GP_ATLAS 1024
#define GP_ATLAS_SLOTS 2048
#define GP_TEX_SLOTS 256

struct gp_atlas_entry {
    unsigned long long hash;
    unsigned short x, y, w, h;
};

struct gp_tex_entry {
    void *key;
    unsigned name;
    int w, h, rgba;
    int valid;
    int gl_ok; /* GL texture object created (needs current context) */
    int needs_upload;
    const unsigned char *pend_px;
    fdk_i32 pend_stride;
};

#define GP_MAX_PEND 256
struct gp_pend {
    void *key;
    int opaque; /* surface blit: alpha forced 1 (XRGB X byte is 0) */
    float cx[4], cy[4]; /* destination corners, top-left pixels */
    float cu[4], cv[4]; /* texture coordinates per corner */
    float alpha;
};

struct fdk_gpu_state {
    gl_uint program;
    gl_int loc_vp;
    gl_uint vbo;
    float *verts;
    int vert_count; /* floats used */
    int dead;

    int width, height;
    int es;      /* GLES flavor (precision stmt + LUMINANCE atlas) */
    int gl_ready; /* GL objects built (needs a CURRENT context; built
                   * lazily at the first flush/atlas/texture call) */

    /* Clip stack mirror (surface.c already bounds it; we keep our own
     * intersection in GL orientation). */
    float clip[4]; /* x0, y0, x1, y1 in GL coords (y up) */
    int clip_depth;

    /* Glyph atlas */
    gl_uint atlas_tex;
    int atlas_x, atlas_y, atlas_row_h;
    struct gp_atlas_entry *atlas_slots;
    unsigned atlas_gen;

    /* Texture registry (fdk_texture* / offscreen fdk_surface* keys) */
    struct gp_tex_entry *tex;
    unsigned tex_gen;

    /* Texture draws issued before GL bring-up (no context yet): the
     * registry entry is booked CPU-side and the DRAW is replayed at
     * the next flush, when the lane context is current. */
    struct gp_pend *pend;
    int pend_count;
};

/* The gpu state rides the surface struct as an opaque pointer
 * (surface_internal.h declares `void *gpu`); engine details stay
 * here. */
static struct fdk_gpu_state *gp_state(struct fdk_surface *s) {
    return (struct fdk_gpu_state *)s->gpu;
}

static int gp_program(struct fdk_gpu_state *g);

/* Deferred GL bring-up: builds the program, VBO and glyph atlas the
 * first time it runs with a CURRENT lane context (the first flush,
 * texture or atlas call — never a bare primitive, which runs before
 * any present and therefore without a current context). Returns 1
 * when GL objects exist. Failure marks the engine dead (present then
 * reports UNSUPPORTED — GPU mode never fakes a frame). */
static int gp_ensure_gl(struct fdk_surface *s) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead) {
        return 0;
    }
    if (g->gl_ready) {
        return 1;
    }
    int r = gp_resolve_procs();
    if (r == 0) {
        fprintf(stderr, "fdk-gpaint: no GL library available; engine "
                        "marked dead\n");
        g->dead = 1;
        return 0;
    }
    if (r < 0) {
        return 0; /* soft: primitives no-op until the first present */
    }
    const char *ver = (const char *)g_gp.GetString(0x1F02 /* VERSION */);
    g->es = (ver != NULL && strstr(ver, "OpenGL ES") != NULL);

    if (!gp_program(g)) {
        fprintf(stderr, "fdk-gpaint: GL bring-up failed (shader/program); "
                        "engine marked dead\n");
        g->dead = 1;
        return 0;
    }
    g_gp.GenBuffers(1, &g->vbo);

    /* Atlas texture (LUMINANCE works on both flavors). */
    g_gp.GenTextures(1, &g->atlas_tex);
    g_gp.ActiveTexture(GL_TEXTURE0);
    g_gp.BindTexture(GL_TEXTURE_2D, g->atlas_tex);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    unsigned char *zeros = calloc(GP_ATLAS * GP_ATLAS, 1);
    if (zeros != NULL) {
        g_gp.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, GP_ATLAS, GP_ATLAS,
                        0, GL_LUMINANCE, GL_UNSIGNED_BYTE, zeros);
        free(zeros);
    }
    g->gl_ready = 1;
    {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            const char *r = (const char *)g_gp.GetString(0x1F01);
            const char *v = (const char *)g_gp.GetString(0x1F02);
            fprintf(stderr, "fdk-gpaint: GL up (renderer=%s, version=%s)\n",
                    r ? r : "?", v ? v : "?");
        }
    }
    return 1;
}

static unsigned long long gp_fnv(const unsigned char *data, size_t n,
                                 unsigned long long seed) {
    unsigned long long h = seed;
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static gl_uint gp_compile(struct fdk_gpu_state *g, unsigned type,
                          const char *src) {
    gl_uint sh = g_gp.CreateShader(type);
    const char *parts[2];
    int nparts = 0;
    char es_head[64];
    if (g->es) {
        snprintf(es_head, sizeof(es_head),
                 "#define GLES 1\nprecision mediump float;\n");
        parts[nparts++] = es_head;
    }
    parts[nparts++] = src;
    g_gp.ShaderSource(sh, nparts, parts, NULL);
    g_gp.CompileShader(sh);
    gl_int ok = 0;
    g_gp.GetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        log[0] = 0;
        g_gp.GetShaderInfoLog(sh, (gl_int)sizeof(log), NULL, log);
        const char *glver = (const char *)g_gp.GetString(0x1F02);
        const char *glsl = (const char *)g_gp.GetString(0x8B8C);
        fprintf(stderr,
                "fdk-gpaint: shader compile failed (%s / GLSL %s): %s\n",
                glver ? glver : "?", glsl ? glsl : "?", log);
        return 0;
    }
    return sh;
}

static int gp_program(struct fdk_gpu_state *g) {
    gl_uint vs = gp_compile(g, GL_VERTEX_SHADER, GP_VS);
    gl_uint fs = gp_compile(g, GL_FRAGMENT_SHADER, GP_FS_BODY);
    if (vs == 0 || fs == 0) {
        return 0;
    }
    gl_uint p = g_gp.CreateProgram();
    g_gp.AttachShader(p, vs);
    g_gp.AttachShader(p, fs);
    g_gp.LinkProgram(p);
    gl_int ok = 0;
    g_gp.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "fdk-gpaint: program link failed\n");
        return 0;
    }
    g->program = p;
    g->loc_vp = g_gp.GetUniformLocation(p, "u_vp");
    return 1;
}

/* Attribute slot assignment matches the vertex layout order. */
static const struct {
    const char *name;
    int size;
    int offset; /* floats from vertex start */
} GP_ATTRS[7] = {
    { "a_pos", 2, 0 },  { "a_uv", 2, 2 },   { "a_color", 4, 4 },
    { "a_aux0", 4, 8 }, { "a_aux1", 4, 12 }, { "a_aux2", 4, 16 },
    { "a_clip", 4, 20 },
};

int fdk__gpu_surface_init(struct fdk_surface *s, int width, int height) {
    if (s->gpu != NULL) {
        return 1;
    }
    if (!fdk__gpu_procs_init()) {
        return 0;
    }
    struct fdk_gpu_state *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return 0;
    }
    g->verts = malloc(sizeof(float) * GP_QUAD_FLOATS * GP_MAX_QUADS);
    g->atlas_slots = calloc(GP_ATLAS_SLOTS, sizeof(*g->atlas_slots));
    g->tex = calloc(GP_TEX_SLOTS, sizeof(*g->tex));
    g->pend = calloc(GP_MAX_PEND, sizeof(*g->pend));
    if (g->verts == NULL || g->atlas_slots == NULL || g->tex == NULL ||
        g->pend == NULL) {
        free(g->verts);
        free(g->atlas_slots);
        free(g->tex);
        free(g->pend);
        free(g);
        return 0;
    }
    g->width = width;
    g->height = height;
    g->clip[0] = 0.0f;
    g->clip[1] = 0.0f;
    g->clip[2] = (float)width;
    g->clip[3] = (float)height;
    s->gpu = g;
    return 1;
}

void fdk__gpu_surface_destroy(struct fdk_surface *s) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL) {
        return;
    }
    /* GL objects die with their context; leak-free at process level is
     * the lane's contract (contexts are destroyed with the window). */
    free(g->verts);
    free(g->atlas_slots);
    free(g->tex);
    free(g->pend);
    free(g);
    s->gpu = NULL;
}

void fdk__gpu_surface_resize(struct fdk_surface *s, int width, int height) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || (width == g->width && height == g->height)) {
        return;
    }
    fdk__gpu_flush(s); /* draw what's pending at the old size */
    g->width = width;
    g->height = height;
    if (g->clip_depth == 0) {
        g->clip[2] = (float)width;
        g->clip[3] = (float)height;
    }
}

/* ------------------------------------------------------------------ */
/* pwindow registry                                                    */

#define GP_MAX_WINDOWS 64
static struct { void *pwindow; struct fdk_surface *s; } g_gp_windows[GP_MAX_WINDOWS];

void fdk__gpu_register_window(void *pwindow, struct fdk_surface *s) {
    if (pwindow == NULL) {
        return;
    }
    for (int i = 0; i < GP_MAX_WINDOWS; i++) {
        if (g_gp_windows[i].pwindow == pwindow ||
            g_gp_windows[i].pwindow == NULL) {
            g_gp_windows[i].pwindow = pwindow;
            g_gp_windows[i].s = s;
            return;
        }
    }
}

void fdk__gpu_unregister_window(void *pwindow) {
    if (pwindow == NULL) {
        return;
    }
    for (int i = 0; i < GP_MAX_WINDOWS; i++) {
        if (g_gp_windows[i].pwindow == pwindow) {
            g_gp_windows[i].pwindow = NULL;
            g_gp_windows[i].s = NULL;
        }
    }
}

struct fdk_surface *fdk__gpu_surface_for_window(void *pwindow) {
    if (pwindow == NULL) {
        return NULL;
    }
    for (int i = 0; i < GP_MAX_WINDOWS; i++) {
        if (g_gp_windows[i].pwindow == pwindow) {
            return g_gp_windows[i].s;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Batch emit                                                          */

/* mode: 0 fill, 1 stroke, 2 gradient, 3 shadow, 4 tex-rgba, 5 glyph.
 * shape: 0 rect, 1 circle (aux0.w). uv semantics: rect-local pixels
 * for modes 0-3, 0..1 texture coords for 4-5. */
static void gp_quad(struct fdk_surface *s, float x, float y, float w,
                    float h, int mode, int shape, float radius,
                    float stroke_half, fdk_color c, fdk_color c1,
                    float u0, float v0, float u1, float v1,
                    float global_alpha) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g->dead || w <= 0 || h <= 0) {
        return;
    }
    if (g->vert_count + GP_QUAD_FLOATS * GP_QUAD_VERTS >
        GP_QUAD_FLOATS * GP_MAX_QUADS) {
        fdk__gpu_flush(s);
    }
    if (g->dead || g->vert_count + GP_QUAD_FLOATS * GP_QUAD_VERTS >
                       GP_QUAD_FLOATS * GP_MAX_QUADS) {
        /* Still no room after the flush attempt (GL not brought up
         * yet — the flush retained its verts): drop this quad rather
         * than run past the buffer. */
        return;
    }
    float *v = g->verts + g->vert_count;
    /* Corner positions stay in TOP-LEFT pixel space; the vertex
     * shader does the single Y flip into clip space (a flip at emit
     * AND in the shader mirrors the scene — caught by the smoke's
     * CPU/GPU pixel comparison). */
    (void)g;
    /* uv values: for pixel-space modes uv = rect-local pixel coords of
     * each corner; for texture modes pass 0..1 directly. */
    /* Pixel-space modes (0-3) evaluate the SDF in RECT-LOCAL pixels:
     * their uv corners are always (0,0)-(w,h) regardless of the
     * caller's u0..v1 (those are texture coords, modes 4-5 only).
     * Getting this wrong pins every fragment to the rect edge —
     * alpha 0.5 everywhere (the bug the smoke's half-blend caught). */
    float uu0, vv0, uu1, vv1;
    if (mode < 4) {
        uu0 = 0.0f;
        vv0 = 0.0f;
        uu1 = w;
        vv1 = h;
    } else {
        uu0 = u0;
        vv0 = v0;
        uu1 = u1;
        vv1 = v1;
    }
    float uvs[4][2] = { { uu0, vv0 }, { uu1, vv0 }, { uu1, vv1 },
                        { uu0, vv1 } };
    float glpos[4][2] = { { x, y }, { x + w, y }, { x + w, y + h },
                          { x, y + h } };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    float a0[4] = { (float)mode, radius, stroke_half, (float)shape };
    float a1[4] = { c1.r, c1.g, c1.b, c1.a };
    float a2[4] = { global_alpha, 0.0f, w, h };
    for (int i = 0; i < 6; i++) {
        int k = idx[i];
        *v++ = glpos[k][0];
        *v++ = glpos[k][1];
        *v++ = uvs[k][0];
        *v++ = uvs[k][1];
        *v++ = c.r;
        *v++ = c.g;
        *v++ = c.b;
        *v++ = c.a;
        for (int j = 0; j < 4; j++) *v++ = a0[j];
        for (int j = 0; j < 4; j++) *v++ = a1[j];
        for (int j = 0; j < 4; j++) *v++ = a2[j];
        for (int j = 0; j < 4; j++) *v++ = g->clip[j];
    }
    g->vert_count += GP_QUAD_FLOATS * GP_QUAD_VERTS;
}

void fdk__gpu_fill(struct fdk_surface *s, fdk_color c) {
    fdk_rect all = { 0, 0, s->fb.width, s->fb.height };
    fdk__gpu_fill_rect(s, all, c);
}

void fdk__gpu_fill_rect(struct fdk_surface *s, fdk_rect r, fdk_color c) {
    gp_quad(s, (float)r.x, (float)r.y, (float)r.width, (float)r.height, 0, 0,
            0, 0, c, c, 0, 0, 0, 0, 1.0f);
}

void fdk__gpu_draw_rect(struct fdk_surface *s, fdk_rect r, fdk_color c,
                        fdk_i32 thickness) {
    /* The public draw_rect is a 1px outline (no width parameter). */
    gp_quad(s, (float)r.x, (float)r.y, (float)r.width, (float)r.height, 1, 0,
            0, (float)thickness * 0.5f, c, c, 0, 0, 0, 0, 1.0f);
}

void fdk__gpu_fill_rounded_rect(struct fdk_surface *s, fdk_rect r,
                                fdk_i32 radius, fdk_color c, int aa) {
    (void)aa; /* SDF AA always; parity with the -_aa variant is exact */
    gp_quad(s, (float)r.x, (float)r.y, (float)r.width, (float)r.height, 0, 0,
            (float)radius, 0, c, c, 0, 0, 0, 0, 1.0f);
}

void fdk__gpu_draw_rounded_rect(struct fdk_surface *s, fdk_rect r,
                                fdk_i32 radius, fdk_color c,
                                fdk_i32 thickness, int aa) {
    (void)aa;
    gp_quad(s, (float)r.x, (float)r.y, (float)r.width, (float)r.height, 1, 0,
            (float)radius, (float)thickness * 0.5f, c, c, 0, 0, 0, 0,
            1.0f);
}

void fdk__gpu_fill_circle(struct fdk_surface *s, fdk_f32 cx, fdk_f32 cy,
                          fdk_f32 radius, fdk_color c, int aa) {
    (void)aa;
    gp_quad(s, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 0, 1,
            0, 0, c, c, 0, 0, 0, 0, 1.0f);
}

void fdk__gpu_draw_circle(struct fdk_surface *s, fdk_f32 cx, fdk_f32 cy,
                          fdk_f32 radius, fdk_color c, fdk_f32 thickness,
                          int aa) {
    (void)aa;
    gp_quad(s, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 1, 1,
            0, thickness * 0.5f, c, c, 0, 0, 0, 0, 1.0f);
}

void fdk__gpu_draw_line(struct fdk_surface *s, fdk_f32 x0, fdk_f32 y0,
                        fdk_f32 x1, fdk_f32 y1, fdk_color c, fdk_f32 width,
                        int aa) {
    /* The public draw_line/draw_line_aa are 1px lines; axis-aligned
     * ones are exact thin rects, diagonals degrade to their bounding
     * box on the GPU (the CPU AA rasterizer stays the pixel-exact
     * reference — recorded honestly in the header comment). */
    (void)aa;
    fdk_f32 w = (width > 0.5f) ? width : 1.0f;
    if (y0 == y1) {
        fdk_rect r = { (fdk_i32)x0, (fdk_i32)(y0 - w * 0.5f),
                       (fdk_i32)(x1 - x0), (fdk_i32)(w + 0.5f) };
        fdk__gpu_fill_rect(s, r, c);
    } else if (x0 == x1) {
        fdk_rect r = { (fdk_i32)(x0 - w * 0.5f), (fdk_i32)y0,
                       (fdk_i32)(w + 0.5f), (fdk_i32)(y1 - y0) };
        fdk__gpu_fill_rect(s, r, c);
    } else {
        fdk_rect r = { (fdk_i32)(x0 < x1 ? x0 : x1),
                       (fdk_i32)(y0 < y1 ? y0 : y1),
                       (fdk_i32)(x1 > x0 ? x1 - x0 : x0 - x1),
                       (fdk_i32)(y1 > y0 ? y1 - y0 : y0 - y1) };
        fdk__gpu_fill_rect(s, r, c);
    }
}

void fdk__gpu_fill_gradient_vertical(struct fdk_surface *s, fdk_rect r,
                                     fdk_color c0, fdk_color c1) {
    gp_quad(s, (float)r.x, (float)r.y, (float)r.width, (float)r.height, 2, 0,
            0, 0, c0, c1, 0, 0, 0, 0, 1.0f);
}

/* ------------------------------------------------------------------ */
/* Atlas (glyph coverage)                                              */

static int gp_atlas_put(struct fdk_surface *s, int w, int h,
                        const unsigned char *cov, fdk_i32 stride,
                        float *u0, float *v0, float *u1, float *v1) {
    struct fdk_gpu_state *g = gp_state(s);
    if (w > GP_ATLAS || h > GP_ATLAS || !gp_ensure_gl(s)) {
        return 0;
    }
    if (g->atlas_x + w > GP_ATLAS) {
        g->atlas_x = 0;
        g->atlas_y += g->atlas_row_h;
        g->atlas_row_h = 0;
    }
    if (g->atlas_y + h > GP_ATLAS) {
        fdk__gpu_flush(s); /* in-flight quads sample the old shelf */
        if (g->dead) {
            return 0;
        }
        g->atlas_x = 0;
        g->atlas_y = 0;
        g->atlas_row_h = 0;
        g->atlas_gen++;
        memset(g->atlas_slots, 0,
               sizeof(*g->atlas_slots) * GP_ATLAS_SLOTS);
    }
    int ax = g->atlas_x, ay = g->atlas_y;
    g->atlas_x += w;
    if (h > g->atlas_row_h) {
        g->atlas_row_h = h;
    }
    g_gp.ActiveTexture(GL_TEXTURE0);
    g_gp.BindTexture(GL_TEXTURE_2D, g->atlas_tex);
    for (int row = 0; row < h; row++) {
        g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, ax, ay + row, w, 1, GL_LUMINANCE,
                           GL_UNSIGNED_BYTE, cov + (size_t)row * stride);
    }
    *u0 = (float)ax;
    *v0 = (float)ay;
    *u1 = (float)(ax + w);
    *v1 = (float)(ay + h);
    return 1;
}

static struct gp_atlas_entry *gp_atlas_slot(struct fdk_gpu_state *g,
                                            unsigned long long hash) {
    unsigned idx = (unsigned)(hash % GP_ATLAS_SLOTS);
    for (int probe = 0; probe < 64; probe++) {
        struct gp_atlas_entry *e = &g->atlas_slots[idx];
        if (e->hash == hash || e->hash == 0) {
            return e;
        }
        idx = (idx + 1) % GP_ATLAS_SLOTS;
    }
    return NULL; /* table pressure: treat as miss+full */
}

void fdk__gpu_blend_mask(struct fdk_surface *s, fdk_rect dst,
                         const unsigned char *coverage, fdk_i32 stride,
                         fdk_color color, fdk_f32 global_alpha) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g->dead || dst.width <= 0 || dst.height <= 0 || coverage == NULL ||
        !gp_ensure_gl(s)) {
        return;
    }
    unsigned long long hash = gp_fnv(coverage,
                                     (size_t)stride * dst.height, 0);
    hash = gp_fnv((const unsigned char *)&dst.width, sizeof(dst.width),
                  hash);
    hash = gp_fnv((const unsigned char *)&dst.height, sizeof(dst.height),
                  hash);
    struct gp_atlas_entry *e = gp_atlas_slot(g, hash);
    float u0, v0, u1, v1;
    if (e != NULL && e->hash == hash) {
        u0 = (float)e->x;
        v0 = (float)e->y;
        u1 = (float)(e->x + e->w);
        v1 = (float)(e->y + e->h);
    } else {
        if (!gp_atlas_put(s, dst.width, dst.height, coverage, stride, &u0,
                          &v0, &u1, &v1)) {
            return;
        }
        if (e != NULL) {
            e->hash = hash;
            e->x = (unsigned short)u0;
            e->y = (unsigned short)v0;
            e->w = (unsigned short)dst.width;
            e->h = (unsigned short)dst.height;
        }
    }
    /* LUMINANCE atlas: v=0 is content row 0 (top). The sampler takes
     * NORMALIZED coordinates — the atlas rect is in texels. */
    float an = (float)GP_ATLAS;
    gp_quad(s, (float)dst.x, (float)dst.y, (float)dst.width,
            (float)dst.height, 5, 0, 0, 0, color, color, u0 / an, v0 / an,
            u1 / an, v1 / an, global_alpha);
}

/* ------------------------------------------------------------------ */
/* Texture registry                                                    */

/* Create the GL object for a booked texture and upload pending
 * pixels. Requires a CURRENT context. */
static unsigned gp_tex_realize(struct fdk_gpu_state *g,
                               struct gp_tex_entry *slot) {
    if (slot->gl_ok) {
        return slot->name;
    }
    g_gp.GenTextures(1, &slot->name);
    slot->gl_ok = 1;
    g_gp.ActiveTexture(GL_TEXTURE0);
    g_gp.BindTexture(GL_TEXTURE_2D, slot->name);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g_gp.PixelStorei(0x0CF5 /* UNPACK_ALIGNMENT */, 1);
    /* format: 0=luminance, 1=rgba (fdk_texture), 2=bgra (offscreen
     * surface pixels are BGRX in memory with X unused — uploading them
     * as RGBA made every texel alpha 0, i.e. invisible). */
    gl_uint ifmt = slot->rgba == 0 ? GL_LUMINANCE
                                   : GL_RGBA; /* placeholder, replaced below */
    (void)ifmt;
    g_gp.TexImage2D(GL_TEXTURE_2D, 0,
                    (gl_int)(slot->rgba == 0 ? GL_LUMINANCE
                             : slot->rgba == 2 ? 0x80E1 /* GL_BGRA */
                                               : GL_RGBA),
                    slot->w, slot->h, 0,
                    slot->rgba == 0 ? GL_LUMINANCE
                    : slot->rgba == 2 ? 0x80E1
                                      : GL_RGBA,
                    GL_UNSIGNED_BYTE, NULL);
    slot->needs_upload = 1;
    return slot->name;
}

unsigned fdk__gpu_texture_name(struct fdk_surface *s, void *key, int width,
                               int height, int format_rgba,
                               const unsigned char *pixels, fdk_i32 stride,
                               int *created) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || key == NULL || width <= 0 || height <= 0) {
        return 0;
    }
    struct gp_tex_entry *slot = NULL;
    for (int i = 0; i < GP_TEX_SLOTS; i++) {
        struct gp_tex_entry *t = &g->tex[i];
        if (t->valid && t->key == key) {
            slot = t;
            break;
        }
        if (slot == NULL && !t->valid) {
            slot = t;
        }
    }
    if (slot == NULL) {
        return 0; /* registry full: bounded, honest */
    }
    if (!slot->valid || slot->w != width || slot->h != height ||
        slot->rgba != format_rgba) {
        slot->valid = 1;
        slot->key = key;
        slot->w = width;
        slot->h = height;
        slot->rgba = format_rgba;
        slot->gl_ok = 0; /* re-realize at the next flush/draw */
        if (created != NULL) {
            *created = 1;
        }
    }
    if (getenv("FDK_GP_TRACE") != NULL) {
        fprintf(stderr,
                "fdk-gpaint: texture_name key=%p %dx%d px=%d glready=%d "
                "valid=%d\n",
                key, width, height, pixels != NULL, g->gl_ready,
                slot->valid);
    }
    if (pixels != NULL) {
        if (g->gl_ready) {
            gp_tex_realize(g, slot);
            g_gp.PixelStorei(0x0CF5, 1);
            gl_uint fmt = format_rgba == 0 ? GL_LUMINANCE
                          : format_rgba == 2 ? 0x80E1 /* GL_BGRA */
                                             : GL_RGBA;
            if (stride == width * (format_rgba ? 4 : 1)) {
                g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                                   fmt, GL_UNSIGNED_BYTE, pixels);
            } else {
                for (int row = 0; row < height; row++) {
                    g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1,
                                       fmt, GL_UNSIGNED_BYTE,
                                       pixels + (size_t)row * stride);
                }
            }
            slot->needs_upload = 0;
        } else {
            /* No current context: book the pixels; the flush's drain
             * realizes + uploads before replaying the draw. */
            slot->pend_px = pixels;
            slot->pend_stride = stride;
            slot->needs_upload = 1;
        }
    }
    return 1; /* booked (the draw path replays through the queue) */
}

void fdk__gpu_texture_drop(struct fdk_surface *s, void *key) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL) {
        return;
    }
    for (int i = 0; i < GP_TEX_SLOTS; i++) {
        if (g->tex[i].valid && g->tex[i].key == key) {
            g_gp.DeleteTextures(1, &g->tex[i].name);
            memset(&g->tex[i], 0, sizeof(g->tex[i]));
        }
    }
}

/* Texture quads cannot ride the batch (the batch flush binds the
 * glyph atlas): each draw is booked in the pending queue and replayed
 * at the flush's drain — with the texture realized + uploaded first.
 * This works BOTH before GL bring-up (no context yet) and after. */
static void gp_tex_book_opaque(struct fdk_surface *s, void *key,
                               float x, float y, float w, float h,
                               float u0, float v0, float u1, float v1,
                               float global_alpha, int opaque);

static void gp_tex_book(struct fdk_surface *s, void *key, float x,
                        float y, float w, float h, float u0, float v0,
                        float u1, float v1, float global_alpha) {
    gp_tex_book_opaque(s, key, x, y, w, h, u0, v0, u1, v1, global_alpha,
                       0);
}

static void gp_tex_book_opaque(struct fdk_surface *s, void *key, float x,
                               float y, float w, float h, float u0,
                               float v0, float u1, float v1,
                               float global_alpha, int opaque) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || key == NULL) {
        return;
    }
    if (g->pend_count >= GP_MAX_PEND) {
        fdk__gpu_flush(s); /* drain makes room; drop on true overflow */
    }
    if (g->pend_count >= GP_MAX_PEND) {
        return;
    }
    /* Booking must NOT touch the registry: the caller already
     * registered the texture with its SOURCE dimensions (re-registering
     * with the destination size would invalidate the booked pixels). */
    int found = 0;
    for (int j = 0; j < GP_TEX_SLOTS; j++) {
        if (g->tex[j].valid && g->tex[j].key == key) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return;
    }
    (void)w;
    (void)h;
    struct gp_pend *p = &g->pend[g->pend_count++];
    p->key = key;
    p->opaque = opaque;
    p->cx[0] = x;        p->cy[0] = y;
    p->cx[1] = x + w;    p->cy[1] = y;
    p->cx[2] = x + w;    p->cy[2] = y + h;
    p->cx[3] = x;        p->cy[3] = y + h;
    p->cu[0] = u0;       p->cv[0] = v0;
    p->cu[1] = u1;       p->cv[1] = v0;
    p->cu[2] = u1;       p->cv[2] = v1;
    p->cu[3] = u0;       p->cv[3] = v1;
    p->alpha = global_alpha;
}

/* Drain the pending texture draws (flush calls this with GL up). */
static void gp_pend_drain(struct fdk_gpu_state *g) {
    if (g->pend_count == 0) {
        return;
    }
    for (int i = 0; i < g->pend_count; i++) {
        struct gp_pend *p = &g->pend[i];
        struct gp_tex_entry *slot = NULL;
        for (int j = 0; j < GP_TEX_SLOTS; j++) {
            if (g->tex[j].valid && g->tex[j].key == p->key) {
                slot = &g->tex[j];
                break;
            }
        }
        if (slot == NULL) {
            continue;
        }
        unsigned name = gp_tex_realize(g, slot);
        if (slot->needs_upload && slot->pend_px != NULL) {
            g_gp.PixelStorei(0x0CF5, 1);
            gl_uint dfmt = slot->rgba == 0 ? GL_LUMINANCE
                           : slot->rgba == 2 ? 0x80E1 /* GL_BGRA */
                                             : GL_RGBA;
            for (int row = 0; row < slot->h; row++) {
                g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, 0, row, slot->w, 1,
                                   dfmt, GL_UNSIGNED_BYTE,
                                   slot->pend_px +
                                       (size_t)row * slot->pend_stride);
            }
            slot->needs_upload = 0;
            slot->pend_px = NULL;
        }
        /* emit the quad directly and draw with THIS texture bound */
        int idx[6] = { 0, 1, 2, 0, 2, 3 };
        float a0[4] = { p->opaque ? 6.0f : 4.0f, 0.0f, 0.0f, 0.0f };
        float a1[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float a2[4] = { p->alpha, 0.0f,
                        p->cx[1] - p->cx[0], p->cy[2] - p->cy[1] };
        float *v = g->verts;
        for (int k = 0; k < 6; k++) {
            int c = idx[k];
            *v++ = p->cx[c];
            *v++ = p->cy[c];
            *v++ = p->cu[c];
            *v++ = p->cv[c];
            *v++ = 1.0f;
            *v++ = 1.0f;
            *v++ = 1.0f;
            *v++ = 1.0f;
            for (int j = 0; j < 4; j++) *v++ = a0[j];
            for (int j = 0; j < 4; j++) *v++ = a1[j];
            for (int j = 0; j < 4; j++) *v++ = a2[j];
            for (int j = 0; j < 4; j++) *v++ = g->clip[j];
        }
        g_gp.BindBuffer(GL_ARRAY_BUFFER, g->vbo);
        g_gp.BufferData(GL_ARRAY_BUFFER,
                        (gl_sizeiptr)GP_QUAD_FLOATS * GP_QUAD_VERTS *
                            (gl_sizeiptr)sizeof(float),
                        g->verts, GL_STREAM_DRAW);
        for (int a = 0; a < 7; a++) {
            g_gp.VertexAttribPointer(
                (gl_uint)a, GP_ATTRS[a].size, GL_FLOAT, 0,
                GP_QUAD_FLOATS * (int)sizeof(float),
                (const void *)(intptr_t)(GP_ATTRS[a].offset *
                                         (int)sizeof(float)));
            g_gp.EnableVertexAttribArray((gl_uint)a);
        }
        g_gp.ActiveTexture(GL_TEXTURE0);
        g_gp.BindTexture(GL_TEXTURE_2D, name);
        g_gp.DrawArrays(GL_TRIANGLES, 0, GP_QUAD_VERTS);
        if (getenv("FDK_GP_TRACE") != NULL) {
            unsigned char probe[4];
            g_gp.Finish();
            g_gp.ReadPixels((int)(p->cx[0] + p->cx[1]) / 2,
                            (int)p->cy[0], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                            probe);
            fprintf(stderr,
                    "fdk-gpaint: drain draw %d op=%d (%.0f,%.0f)"
                    "(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) slot %dx%d\n",
                    i, p->opaque, p->cx[0], p->cy[0], p->cx[1], p->cy[1],
                    p->cx[2], p->cy[2], p->cx[3], p->cy[3], slot->w,
                    slot->h);
        }
    }
    g->pend_count = 0;
    if (getenv("FDK_GP_TRACE") != NULL) {
        fprintf(stderr, "fdk-gpaint: drained texture draws\n");
    }
    unsigned err = g_gp.GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "fdk-gpaint: GL error 0x%x at texture drain\n",
                err);
        g->dead = 1;
    }
}

void fdk__gpu_draw_texture(struct fdk_surface *s, void *key,
                           fdk_rect dst, float u0, float v0, float u1,
                           float v1, float global_alpha) {
    gp_tex_book(s, key, (float)dst.x, (float)dst.y, (float)dst.width,
                (float)dst.height, u0, v0, u1, v1, global_alpha);
}

void fdk__gpu_blend_rgba(struct fdk_surface *s, fdk_rect dst,
                         const unsigned char *rgba, fdk_i32 stride,
                         fdk_f32 global_alpha) {
    if (gp_state(s) == NULL || dst.width <= 0 || dst.height <= 0) {
        return;
    }
    /* book the pixels too: pre-GL draws upload at drain time */
    (void)fdk__gpu_texture_name(s, (void *)(uintptr_t)rgba, dst.width,
                                dst.height, 1, rgba, stride, NULL);
    gp_tex_book(s, (void *)(uintptr_t)rgba, (float)dst.x, (float)dst.y,
                (float)dst.width, (float)dst.height, 0.0f, 0.0f, 1.0f,
                1.0f, global_alpha);
}

void fdk__gpu_blit(struct fdk_surface *s, fdk_rect dst,
                   const struct fdk_surface *src, fdk_rect src_rect) {
    if (gp_state(s) == NULL || src == NULL || dst.width <= 0 ||
        dst.height <= 0 || src_rect.width <= 0 || src_rect.height <= 0) {
        return;
    }
    const unsigned char *px = (const unsigned char *)src->own_pixels;
    if (px == NULL) {
        return; /* window-source blit in GPU mode: not supported yet */
    }
    /* own_pixels stride is in fdk_u32 units; the engine wants bytes. */
    (void)fdk__gpu_texture_name(s, (void *)src, src->fb.width,
                                src->fb.height, 2, px, src->fb.stride * 4,
                                NULL);
    float su0 = (float)src_rect.x / (float)src->fb.width;
    float sv0 = (float)src_rect.y / (float)src->fb.height;
    float su1 = (float)(src_rect.x + src_rect.width) / (float)src->fb.width;
    float sv1 = (float)(src_rect.y + src_rect.height) / (float)src->fb.height;
    gp_tex_book_opaque(s, (void *)src, (float)dst.x, (float)dst.y,
                       (float)dst.width, (float)dst.height, su0, sv0,
                       su1, sv1, 1.0f, 1);
}

/* The affine twin of fdk_surface_blit_transformed: the matrix maps
 * SOURCE pixels to DESTINATION pixels (x' = m00*x + m01*y + tx, same
 * row-major layout as the public fdk_matrix). The four corners bake
 * through it, the UV corners follow the source rect, and the vertex
 * shader's single Y flip keeps the convention documented in gpaint.h.
 * Works for ANY invertible affine — rotation/shear included — with no
 * bounding-box degradation. */
void fdk__gpu_blit_transformed(struct fdk_surface *s, fdk_matrix m,
                               const struct fdk_surface *src) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || src == NULL) {
        return;
    }
    const unsigned char *px = (const unsigned char *)src->own_pixels;
    if (px == NULL) {
        return; /* window-source blit in GPU mode: not supported yet */
    }
    (void)fdk__gpu_texture_name(s, (void *)src, src->fb.width,
                                src->fb.height, 2, px, src->fb.stride * 4,
                                NULL);
    float w = (float)src->fb.width, h = (float)src->fb.height;
    float sx[4] = { 0, w, w, 0 };
    float sy[4] = { 0, 0, h, h };
    float dx[4], dy[4];
    for (int i = 0; i < 4; i++) {
        dx[i] = m.m00 * sx[i] + m.m01 * sy[i] + m.tx;
        dy[i] = m.m10 * sx[i] + m.m11 * sy[i] + m.ty;
    }
    /* One quad per corner pair: the batch's gp_quad emits an
     * axis-aligned rect, so transformed blits book their baked
     * corners directly (drawn at the flush's drain). */
    if (g->pend_count >= GP_MAX_PEND) {
        fdk__gpu_flush(s);
    }
    if (g->dead || g->pend_count >= GP_MAX_PEND) {
        return;
    }
    struct gp_pend *p = &g->pend[g->pend_count++];
    p->key = (void *)src;
    p->opaque = 1;
    float su = w > 0 ? 1.0f / w : 0.0f;
    float sv = h > 0 ? 1.0f / h : 0.0f;
    for (int i = 0; i < 4; i++) {
        p->cx[i] = dx[i];
        p->cy[i] = dy[i];
        p->cu[i] = sx[i] * su;
        p->cv[i] = sy[i] * sv;
    }
    p->alpha = 1.0f;
}

/* Test seam: read the CURRENT framebuffer (the caller made the
 * window's lane context current and drew the frame). Bottom-up GL
 * rows come back as top-down RGBA8888. Returns 0 on success. */
int fdk__gpu_engine_readback(struct fdk_surface *s, fdk_u32 *out,
                             int width, int height) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || out == NULL || width <= 0 ||
        height <= 0 || !gp_ensure_gl(s) || g_gp.ReadPixels == NULL ||
        g_gp.Finish == NULL) {
        fprintf(stderr,
                "fdk-gpaint: readback precheck (g=%p dead=%d glready=%d "
                "rp=%p fin=%p lib=%s)\n",
                (void *)g, g ? g->dead : -1,
                g ? g->gl_ready : -1, (void *)g_gp.ReadPixels,
                (void *)g_gp.Finish,
                g_gp.lib != NULL ? "resolved" : "none");
        return -1;
    }
    g_gp.Finish();
    unsigned char *rowbuf = malloc((size_t)width * 4u);
    if (rowbuf == NULL) {
        return -1;
    }
    for (int y = 0; y < height; y++) {
        g_gp.ReadPixels(0, height - 1 - y, width, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, rowbuf);
        fdk_u32 *dst = out + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            dst[x] = (fdk_u32)rowbuf[x * 4] |
                     ((fdk_u32)rowbuf[x * 4 + 1] << 8) |
                     ((fdk_u32)rowbuf[x * 4 + 2] << 16) |
                     ((fdk_u32)rowbuf[x * 4 + 3] << 24);
        }
    }
    free(rowbuf);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Clips                                                               */

/* The surface's clip_x0..y1 fields are the single source of truth
 * (surface.c recomputes the intersection on every push/pop); the
 * engine mirrors them into GL orientation after each change. */
void fdk__gpu_sync_clip(struct fdk_surface *s) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead) {
        return;
    }
    fdk__gpu_flush(s); /* clip changes are per-quad; keep ordering */
    float gy0 = (float)g->height - (float)s->clip_y1;
    float gy1 = (float)g->height - (float)s->clip_y0;
    g->clip[0] = (float)s->clip_x0;
    g->clip[1] = gy0;
    g->clip[2] = (float)s->clip_x1;
    g->clip[3] = gy1;
    g->clip_depth = s->clip_depth;
}

/* ------------------------------------------------------------------ */
/* Flush                                                               */

void fdk__gpu_flush(struct fdk_surface *s) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || g->vert_count == 0) {
        return;
    }
    if (!gp_ensure_gl(s)) {
        /* Soft failure (no current context yet — this flush ran from
         * a clip sync or a texture path before the first present):
         * KEEP the batch; the next flush with a current context draws
         * it. A DEAD engine drops everything. */
        if (g->dead) {
            g->vert_count = 0;
        }
        return;
    }
    g_gp.Viewport(0, 0, g->width, g->height);
    g_gp.Disable(GL_SCISSOR_TEST); /* clips ride the per-quad data */
    g_gp.Enable(GL_BLEND);
    g_gp.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    g_gp.UseProgram(g->program);
    g_gp.Uniform2f(g->loc_vp, (float)g->width, (float)g->height);
    g_gp.ActiveTexture(GL_TEXTURE0);
    g_gp.BindTexture(GL_TEXTURE_2D, g->atlas_tex);
    g_gp.BindBuffer(GL_ARRAY_BUFFER, g->vbo);
    gl_sizeiptr bytes =
        (gl_sizeiptr)g->vert_count * (gl_sizeiptr)sizeof(float);
    g_gp.BufferData(GL_ARRAY_BUFFER, bytes, g->verts, GL_STREAM_DRAW);
    for (int i = 0; i < 7; i++) {
        g_gp.VertexAttribPointer((gl_uint)i, GP_ATTRS[i].size, GL_FLOAT, 0,
                                 GP_QUAD_FLOATS * (int)sizeof(float),
                                 (const void *)(intptr_t)(
                                     GP_ATTRS[i].offset *
                                     (int)sizeof(float)));
        g_gp.EnableVertexAttribArray((gl_uint)i);
    }
    g_gp.DrawArrays(GL_TRIANGLES, 0, g->vert_count / GP_QUAD_FLOATS);
    if (getenv("FDK_GP_TRACE") != NULL) {
        fprintf(stderr, "fdk-gpaint: flush %d quads (%d floats)\n",
                g->vert_count / GP_QUAD_FLOATS, g->vert_count);
    }
    g->vert_count = 0;
    gp_pend_drain(g); /* texture draws booked before GL was up */
    unsigned err = g_gp.GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr,
                "fdk-gpaint: GL error 0x%x at flush — engine marked dead; "
                "the lane downgrade takes over\n",
                err);
        g->dead = 1;
    }
}
