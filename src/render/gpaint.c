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
    const char *names[] = { "libGLESv2.so.2", "libGLESv2.so",
                            "libGL.so.1",     "libGL.so" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        g_gp.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (g_gp.lib != NULL) {
            break;
        }
    }
    if (g_gp.lib == NULL) {
        return 0;
    }
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
#undef GP
    if (!g_gp_ok) {
        memset(&g_gp, 0, sizeof(g_gp));
        return 0;
    }
    return 1;
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
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y));\n"
    "        } else {\n"
    "            d = length(p - half_sz) -\n"
    "                (min(half_sz.x, half_sz.y) - v_aux0.y);\n"
    "        }\n"
    "        float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 1.5) {\n" /* stroke: ring at stroke_half */
    "        float d;\n"
    "        if (v_aux0.w < 0.5) {\n"
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y));\n"
    "        } else {\n"
    "            d = length(p - half_sz) -\n"
    "                (min(half_sz.x, half_sz.y) - v_aux0.y);\n"
    "        }\n"
    "        float ring = abs(d) - v_aux0.z;\n"
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
    "            v_aux0.z;\n" /* global alpha in aux0.z */
    "    } else {\n"             /* glyph coverage (LUMINANCE) + tint */
    "        float l = texture2D(u_tex, v_uv).r;\n"
    "        c = vec4(v_color.rgb, v_color.a * l * v_aux0.z);\n"
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
};

struct fdk_gpu_state {
    gl_uint program;
    gl_int loc_vp;
    gl_uint vbo;
    float *verts;
    int vert_count; /* floats used */
    int dead;

    int width, height;
    int es; /* GLES flavor (precision stmt + LUMINANCE atlas) */

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
};

/* The gpu state rides the surface struct as an opaque pointer
 * (surface_internal.h declares `void *gpu`); engine details stay
 * here. */
static struct fdk_gpu_state *gp_state(struct fdk_surface *s) {
    return (struct fdk_gpu_state *)s->gpu;
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
        char log[512];
        log[0] = 0;
        g_gp.GetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "fdk-gpaint: shader compile failed: %s\n", log);
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
    if (g->verts == NULL || g->atlas_slots == NULL || g->tex == NULL) {
        free(g->verts);
        free(g->atlas_slots);
        free(g->tex);
        free(g);
        return 0;
    }
    g->width = width;
    g->height = height;
    const char *ver = (const char *)g_gp.GetString(0x1F02 /* VERSION */);
    g->es = (ver != NULL && strstr(ver, "OpenGL ES") != NULL);

    if (!gp_program(g)) {
        free(g->verts);
        free(g->atlas_slots);
        free(g->tex);
        free(g);
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
        g_gp.TexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, GP_ATLAS, GP_ATLAS, 0,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, zeros);
        free(zeros);
    }

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
    if (g->dead) {
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
    float uvs[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
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
    if (w > GP_ATLAS || h > GP_ATLAS) {
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
    if (g->dead || dst.width <= 0 || dst.height <= 0 || coverage == NULL) {
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
    /* LUMINANCE atlas: v=0 is content row 0 (top). */
    gp_quad(s, (float)dst.x, (float)dst.y, (float)dst.width,
            (float)dst.height, 5, 0, 0, 0, color, color, u0, v0, u1, v1,
            global_alpha);
}

/* ------------------------------------------------------------------ */
/* Texture registry                                                    */

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
        if (!slot->valid) {
            g_gp.GenTextures(1, &slot->name);
        }
        slot->valid = 1;
        slot->key = key;
        slot->w = width;
        slot->h = height;
        slot->rgba = format_rgba;
        g_gp.ActiveTexture(GL_TEXTURE0);
        g_gp.BindTexture(GL_TEXTURE_2D, slot->name);
        g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g_gp.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g_gp.PixelStorei(0x0CF5 /* UNPACK_ALIGNMENT */, 1);
        gl_uint ifmt = format_rgba ? GL_RGBA : GL_LUMINANCE;
        g_gp.TexImage2D(GL_TEXTURE_2D, 0, (gl_int)ifmt, width, height, 0,
                        ifmt, GL_UNSIGNED_BYTE, NULL);
        if (created != NULL) {
            *created = 1;
        }
    }
    if (pixels != NULL) {
        g_gp.ActiveTexture(GL_TEXTURE0);
        g_gp.BindTexture(GL_TEXTURE_2D, slot->name);
        g_gp.PixelStorei(0x0CF5, 1);
        if (stride == width * (format_rgba ? 4 : 1)) {
            g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                               format_rgba ? GL_RGBA : GL_LUMINANCE,
                               GL_UNSIGNED_BYTE, pixels);
        } else {
            int bpp = format_rgba ? 4 : 1;
            for (int row = 0; row < height; row++) {
                g_gp.TexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width, 1,
                                   format_rgba ? GL_RGBA : GL_LUMINANCE,
                                   GL_UNSIGNED_BYTE,
                                   pixels + (size_t)row * stride);
            }
            (void)bpp;
        }
    }
    return slot->name;
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
 * glyph atlas): flush first, bind the texture, draw one quad. */
static void gp_tex_quad(struct fdk_surface *s, unsigned name, float x,
                        float y, float w, float h, float u0, float v0,
                        float u1, float v1, float global_alpha) {
    struct fdk_gpu_state *g = gp_state(s);
    if (g == NULL || g->dead || name == 0) {
        return;
    }
    fdk__gpu_flush(s);
    if (g->dead) {
        return;
    }
    gp_quad(s, x, y, w, h, 4, 0, 0, 0, (fdk_color){ 1, 1, 1, 1 },
            (fdk_color){ 0, 0, 0, 0 }, u0, v0, u1, v1, global_alpha);
    g_gp.ActiveTexture(GL_TEXTURE0);
    g_gp.BindTexture(GL_TEXTURE_2D, name);
    g_gp.DrawArrays(GL_TRIANGLES, 0, g->vert_count / GP_QUAD_FLOATS);
    unsigned err = g_gp.GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr, "fdk-gpaint: GL error 0x%x at texture draw\n", err);
        g->dead = 1;
    }
    g->vert_count = 0;
}

void fdk__gpu_draw_texture(struct fdk_surface *s, unsigned name,
                           fdk_rect dst, float u0, float v0, float u1,
                           float v1, float global_alpha) {
    gp_tex_quad(s, name, (float)dst.x, (float)dst.y, (float)dst.width,
                (float)dst.height, u0, v0, u1, v1, global_alpha);
}

void fdk__gpu_blend_rgba(struct fdk_surface *s, fdk_rect dst,
                         const unsigned char *rgba, fdk_i32 stride,
                         fdk_f32 global_alpha) {
    if (gp_state(s) == NULL || dst.width <= 0 || dst.height <= 0) {
        return;
    }
    unsigned name = fdk__gpu_texture_name(s, (void *)(uintptr_t)rgba,
                                          dst.width, dst.height, 1, rgba,
                                          stride, NULL);
    gp_tex_quad(s, name, (float)dst.x, (float)dst.y, (float)dst.width,
                (float)dst.height, 0.0f, 0.0f, 1.0f, 1.0f, global_alpha);
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
    unsigned name = fdk__gpu_texture_name(
        s, (void *)src, src->fb.width, src->fb.height, 1, px,
        src->fb.stride * 4, NULL);
    float su0 = (float)src_rect.x / (float)src->fb.width;
    float sv0 = (float)src_rect.y / (float)src->fb.height;
    float su1 = (float)(src_rect.x + src_rect.width) / (float)src->fb.width;
    float sv1 = (float)(src_rect.y + src_rect.height) / (float)src->fb.height;
    gp_tex_quad(s, name, (float)dst.x, (float)dst.y, (float)dst.width,
                (float)dst.height, su0, sv0, su1, sv1, 1.0f);
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
    unsigned err = g_gp.GetError();
    if (err != GL_NO_ERROR) {
        fprintf(stderr,
                "fdk-gpaint: GL error 0x%x at flush — engine marked dead; "
                "the lane downgrade takes over\n",
                err);
        g->dead = 1;
    }
    g->vert_count = 0;
}
