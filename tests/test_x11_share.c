/* test_x11_share.c — P2 close-out: the X11 lane's share-root and a
 * texture exchange between two of FDK's window contexts (the
 * documented fdk_ext_gl contract: "texture objects cross between
 * windows"). Two GPU windows; the add-on pattern:
 *   make window A's lane context current, upload a named texture;
 *   make window B's lane context current, bind that name, read back.
 * With the share-root the content must round-trip; without it the
 * name is meaningless in B's context (the pre-parity behavior).
 */
#include "fdk/fdk.h"
#include "platform/x11/x11_platform.h"
#include "window/window_internal.h" /* w->pwindow for the lane seam */
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

static int g_fail = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        printf("[%s] %s\n", (cond) ? "ok" : "FAIL", name);                   \
        if (!(cond)) {                                                       \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* raw GL, resolved from the lane's own pinned libGL via its dlopen
 * handle (the test includes internal headers by precedent) */
typedef unsigned int gl_uint;
static struct {
    void (*GenTextures)(int, gl_uint *);
    void (*BindTexture)(int, gl_uint);
    void (*TexImage2D)(int, int, int, int, int, int, int, int,
                       const void *);
    void (*GetTexImage)(int, int, int, int, void *);
    void (*DeleteTextures)(int, const gl_uint *);
    void (*Finish)(void);
} g_gl;

static int resolve_gl(fdk_window *wa) {
    /* the lane pins its own libGL; resolve through that handle */
    extern void *fdk_x11_gl_lib_for_test(fdk_platform_window *pw);
    void *h = fdk_x11_gl_lib_for_test(wa->pwindow);
    if (h == NULL) {
        return 0;
    }
    struct { void **pfn; const char *name; } tab[] = {
        { (void **)&g_gl.GenTextures, "glGenTextures" },
        { (void **)&g_gl.BindTexture, "glBindTexture" },
        { (void **)&g_gl.TexImage2D, "glTexImage2D" },
        { (void **)&g_gl.GetTexImage, "glGetTexImage" },
        { (void **)&g_gl.DeleteTextures, "glDeleteTextures" },
        { (void **)&g_gl.Finish, "glFinish" },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        *tab[i].pfn = dlsym(h, tab[i].name);
        if (*tab[i].pfn == NULL) {
            return 0;
        }
    }
    return 1;
}

static fdk_u8 g_pat[4 * 4 * 4];
static void make_pattern(void) {
    for (int i = 0; i < 16; i++) {
        g_pat[i * 4 + 0] = (fdk_u8)(30 + i * 13);
        g_pat[i * 4 + 1] = (fdk_u8)(200 - i * 11);
        g_pat[i * 4 + 2] = (fdk_u8)(77 + i * 5);
        g_pat[i * 4 + 3] = 255;
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL))) {
        printf("SKIP: no FDK context\n");
        return 77;
    }
    fdk_window *wa = NULL, *wb = NULL;
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.render_mode = FDK_RENDER_GPU;
    o.title = "fdk-share-a";
    o.width = 64;
    o.height = 64;
    fdk_result r = fdk_window_create(ctx, &o, &wa);
    if (!fdk_ok(r)) {
        printf("SKIP: no GPU lane (%d)\n", (int)r);
        return 77;
    }
    if (!resolve_gl(wa)) {
        printf("SKIP: the lane's GL symbols are unavailable\n");
        return 77;
    }
    o.title = "fdk-share-b";
    if (!fdk_ok(fdk_window_create(ctx, &o, &wb))) {
        printf("SKIP: second GPU window refused\n");
        return 77;
    }
    fdk_window_show(wa);
    fdk_window_show(wb);
    for (int i = 0; i < 3; i++) {
        fdk_pump_events(ctx, 16);
    }
    make_pattern();

    /* the lane context attaches at the first present: prime both */
    for (int i = 0; i < 2; i++) {
        fdk_surface *s0 = NULL;
        fdk_window_get_surface(wa, &s0);
        fdk_surface_fill(s0, (fdk_color){ 0.1f, 0.1f, 0.1f, 1.0f });
        (void)fdk_surface_present(s0);
        fdk_window_get_surface(wb, &s0);
        fdk_surface_fill(s0, (fdk_color){ 0.1f, 0.1f, 0.1f, 1.0f });
        (void)fdk_surface_present(s0);
        fdk_pump_events(ctx, 16);
    }

    /* A: upload a named texture in window A's lane context */
    gl_uint t = 0;
    CHECK(fdk_ok(fdk_x11_window_gl_make_current(wa->pwindow)),
          "make A current");
    g_gl.GenTextures(1, &t);
    g_gl.BindTexture(0x0DE1 /* GL_TEXTURE_2D */, t);
    g_gl.TexImage2D(0x0DE1, 0, 0x1908 /* GL_RGBA */, 4, 4, 0, 0x1908,
                    0x1401 /* GL_UNSIGNED_BYTE */, g_pat);
    g_gl.Finish();

    /* B: the same name must resolve to the same object with the same
     * content in window B's lane context (the share-root) */
    fdk_u8 got[4 * 4 * 4];
    memset(got, 0xAA, sizeof(got));
    CHECK(fdk_ok(fdk_x11_window_gl_make_current(wb->pwindow)),
          "make B current");
    g_gl.BindTexture(0x0DE1, t);
    g_gl.GetTexImage(0x0DE1, 0, 0x1908, 0x1401, got);
    int same = memcmp(got, g_pat, sizeof(g_pat)) == 0;
    if (!same) {
        printf("[info] first 4 bytes got=(%u,%u,%u,%u) want=(%u,%u,%u,%u)\n",
               got[0], got[1], got[2], got[3], g_pat[0], g_pat[1], g_pat[2],
               g_pat[3]);
    }
    CHECK(same, "texture uploaded in A reads back in B (share-root)");

    /* both windows still present fine after the cross-context play */
    fdk_surface *sa = NULL;
    fdk_window_get_surface(wa, &sa);
    fdk_surface_fill(sa, (fdk_color){ 0.3f, 0.7f, 0.4f, 1.0f });
    CHECK(fdk_ok(fdk_surface_present(sa)), "A presents");
    fdk_surface *sb = NULL;
    fdk_window_get_surface(wb, &sb);
    fdk_surface_fill(sb, (fdk_color){ 0.7f, 0.3f, 0.4f, 1.0f });
    CHECK(fdk_ok(fdk_surface_present(sb)), "B presents");
    fdk_pump_events(ctx, 16);

    g_gl.DeleteTextures(1, &t);
    fdk_window_destroy(wa);
    fdk_window_destroy(wb);
    fdk_shutdown(ctx);
    printf(g_fail ? "X11 SHARE: %d FAILURES\n" : "X11 SHARE: all green\n",
           g_fail);
    return g_fail ? 1 : 0;
}
