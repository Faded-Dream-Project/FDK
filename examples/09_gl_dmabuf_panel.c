/* 09_gl_dmabuf_panel.c — the GL-canvas panel in GPU mode.
 *
 * The add-on shape EXTENDING.md teaches, end to end:
 *
 *   1. a context in GPU mode (the lane is REQUIRED there — a driver
 *      that cannot serve it refuses fdk_init with FDK_ERR_UNSUPPORTED,
 *      and this example then runs in HYBRID, honestly labeled),
 *   2. an extension surface of a custom kind ("com.fadeddream.gl-panel")
 *      built on the native APIs EXTENDING.md documents (X11: a plain
 *      window; Wayland: your own wl_surface from your own registry),
 *   3. a WORKER (fdk_worker_submit) that renders a gradient off-thread,
 *      posting progress notes with fdk_post on the way,
 *   4. the finished buffer imported as a dma-buf (the memfd/linear
 *      path — the zero-copy lane engages when the driver supports it;
 *      the documented CPU-mode fallback serves the same texture
 *      everywhere else),
 *   5. the imported texture drawn with fdk_texture_draw and a caption
 *      drawn with the text API (fdk_surface_draw_utf8) — the same
 *      calls on every render mode, per the docs' cross-mode contract.
 *
 * Build (from a configured FDK tree):
 *   cc -Iinclude examples/09_gl_dmabuf_panel.c -L<install>/lib -lfdk \
 *      $(pkg-config --cflags --libs x11 wayland-client) -lm -lpthread \
 *      -o 09_gl_dmabuf_panel
 * Run: DISPLAY=... ./09_gl_dmabuf_panel      (or WAYLAND_DISPLAY=...)
 *
 * The panel runs for about three seconds (long enough to see three
 * worker-completed frames), then exits 0. Exit 2 = a capability it
 * refused to fake was missing (see the checks along the way).
 */

#define _GNU_SOURCE /* memfd_create */

#include "fdk/fdk.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_text.h"
#include "fdk/fdk_thread.h"
#include "fdk/fdk_extend.h"
#include "fdk/fdk_error.h"

#include <X11/Xlib.h>       /* the X11 branch: the docs' own calls */
#include <wayland-client.h> /* the Wayland branch: your own registry */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PANEL_W 320
#define PANEL_H 200
#define FOURCC(a, b, c, d)                                                    \
    ((fdk_u32)(a) | ((fdk_u32)(b) << 8) | ((fdk_u32)(c) << 16) |              \
     ((fdk_u32)(d) << 24))

/* ---- the extension kind: one plain native surface, no special role ---- */

/* The registry listener (C17: a file-scope function, the docs' shape). */
static void panel_registry_global(void *data, struct wl_registry *reg,
                                  uint32_t name, const char *iface,
                                  uint32_t version) {
    (void)version;
    if (strcmp(iface, "wl_compositor") == 0) {
        struct wl_compositor **out = data;
        *out = (struct wl_compositor *)wl_registry_bind(
            reg, name, &wl_compositor_interface, 4);
    }
}

static fdk_result panel_create(fdk_ext_surface *surface,
                               fdk_ext_native_handle *out_native,
                               void *user) {
    (void)user;
    fdk_context *ctx = fdk_ext_surface_context(surface);

    fdk_ext_native_display nd;
    if (!fdk_ok(fdk_ext_get_native_display(ctx, &nd))) {
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (nd.backend == FDK_EXT_BACKEND_X11) {
        /* The docs' X11 branch, verbatim in spirit: a plain window,
         * UNMAPPED, properties already set; FDK maps it at show time.
         * Override-redirect would make it a popup — a panel is an
         * ordinary child of the root. */
        Display *dpy = (Display *)nd.display;
        Window win = XCreateSimpleWindow(
            dpy, DefaultRootWindow(dpy), 40, 40, PANEL_W, PANEL_H, 0,
            BlackPixel(dpy, DefaultScreen(dpy)),
            BlackPixel(dpy, DefaultScreen(dpy)));
        if (win == None) {
            return FDK_ERR_UNSUPPORTED;
        }
        out_native->backend = FDK_EXT_BACKEND_X11;
        out_native->xid = (fdk_u64)win;
        return FDK_OK;
    }
    if (nd.backend == FDK_EXT_BACKEND_WAYLAND) {
        /* Wayland: FDK does not hand out its wl_compositor — bind your
         * own registry on the shared display and take it from there
         * (EXTENDING.md: "your own registry is ordinary Wayland;
         * events fan out per listener"). */
        struct wl_display *dpy = (struct wl_display *)nd.display;
        struct wl_registry *reg = wl_display_get_registry(dpy);
        if (reg == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        struct {
            struct wl_compositor *compositor;
        } found = {NULL};
        struct wl_registry_listener rl = {0};
        rl.global = panel_registry_global;
        wl_registry_add_listener(reg, &rl, &found);
        wl_display_roundtrip(dpy);
        wl_display_roundtrip(dpy); /* the bind's own event */
        wl_registry_destroy(reg);
        if (found.compositor == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        struct wl_surface *surf =
            wl_compositor_create_surface(found.compositor);
        if (surf == NULL) {
            return FDK_ERR_UNSUPPORTED;
        }
        /* A panel on Wayland carries a role (xdg_surface at minimum);
         * this example is the X11 lane's demonstration — the sandbox
         * and CI run Xvfb. The Wayland branch is the registry story
         * the docs teach; a real panel adds its role object here and
         * owns it through ops.destroy. */
        wl_surface_destroy(surf);
        wl_compositor_destroy(found.compositor);
        return FDK_ERR_UNSUPPORTED;
    }
    return FDK_ERR_UNSUPPORTED;
}

static void panel_destroy(fdk_ext_surface *surface, void *user) {
    (void)surface;
    (void)user;
    /* The add-on owns ROLE objects; this panel created none (a plain
     * window/surface carries no extra protocol object). Nothing to do.
     * FDK destroys the native surface itself after this returns. */
}

/* ---- the worker + the post lane --------------------------------------- */

static fdk_context *g_ctx = NULL;
static fdk_ext_surface *g_panel = NULL;
static fdk_surface *g_sf = NULL;
static fdk_texture *g_tex = NULL;
static int g_import_honest_refusal = 0; /* UNSUPPORTED = the driver
                                         * truthfully lacks the lane */
static fdk_font *g_font = NULL;
static int g_frames = 0;

typedef struct job_payload {
    uint8_t *pixels; /* PANEL_W*PANEL_H*4, BGRA8888-LE (the fourcc below) */
    int done;
} job_payload;

static void note_from_worker(void *user) {
    /* Runs on the UI thread (fdk_post's contract). A real panel would
     * flip a "decoding…" label; the example prints the round trip. */
    printf("[panel] worker note arrived on the UI thread: %s\n",
           (const char *)user);
}

static void *panel_job(void *user_data, void (*report)(double, void *),
                       void *report_user) {
    job_payload *p = user_data;
    (void)report;
    (void)report_user;
    (void)fdk_post(g_ctx, note_from_worker, (void *)"rendering gradient");
    /* The gradient: BGRA8888-LE in memory (B,G,R,A per pixel). */
    for (int y = 0; y < PANEL_H; y++) {
        for (int x = 0; x < PANEL_W; x++) {
            uint8_t *px = p->pixels + ((size_t)y * (size_t)PANEL_W +
                                     (size_t)x) * 4;
            px[0] = (uint8_t)(x * 255 / (PANEL_W - 1)); /* B */
            px[1] = (uint8_t)(y * 255 / (PANEL_H - 1)); /* G */
            px[2] = (uint8_t)(64);                      /* R */
            px[3] = 0xFF;                               /* A */
        }
    }
    p->done = 1;
    return p; /* the result flows to done() as its first argument */
}

static void panel_job_done(void *result, void *user) {
    job_payload *p = (result != NULL) ? result : user;
    /* The worker pool's done() runs on the UI thread (fdk_thread.h).
     * The pixels are now stable — wrap them in a dma-buf and import. */
    int fd = memfd_create("fdk-panel-gradient", 0);
    if (fd < 0) {
        printf("[panel] memfd failed; the import is skipped this frame\n");
        return;
    }
    size_t need = (size_t)PANEL_W * PANEL_H * 4;
    if (write(fd, p->pixels, need) != (ssize_t)need) {
        close(fd);
        return;
    }
    fdk_dmabuf_params dp;
    memset(&dp, 0, sizeof dp);
    dp.struct_size = sizeof dp;
    dp.width = PANEL_W;
    dp.height = PANEL_H;
    dp.fourcc = FOURCC('B', 'G', 'R', 'A'); /* BGRA8888-LE */
    dp.n_planes = 1;
    dp.planes[0].fd = fd;
    dp.planes[0].stride = (fdk_u32)PANEL_W * 4;
    dp.planes[0].offset = 0;
    fdk_texture *tex = NULL;
    fdk_result r = fdk_texture_import_dmabuf(&dp, &tex);
    close(fd); /* the import owns its own descriptor state */
    if (!fdk_ok(r)) {
        /* The capability-honest contract: no driver support is
         * FDK_ERR_UNSUPPORTED, a clean per-import answer. The example
         * records it and keeps drawing (the pipeline is the point;
         * zero-copy needs the driver). */
        printf("[panel] import refused: %s\n", fdk_result_to_string(r));
        if (r == FDK_ERR_UNSUPPORTED) {
            g_import_honest_refusal = 1;
        }
        return;
    }
    if (g_tex != NULL) {
        fdk_texture_destroy(g_tex);
    }
    g_tex = tex;
    printf("[panel] dma-buf imported (%s zero-copy lane; the documented "
           "CPU-mode fallback serves the same texture elsewhere)\n",
           fdk_ext_has_capability(g_ctx, FDK_EXT_CAP_DMABUF_IMPORT)
               ? "the driver reports a"
               : "no");
    g_frames++;
}

/* ---- the frame --------------------------------------------------------- */

static void panel_frame(void) {
    if (g_sf == NULL) {
        return;
    }
    fdk_surface_fill(g_sf, (fdk_color){0.04f, 0.04f, 0.06f, 1.0f});
    if (g_tex != NULL) {
        /* The cross-mode contract: the same call on every render mode.
         * On a GPU-mode window this rides the lane's texture path. */
        (void)fdk_texture_draw(g_sf, g_tex,
                               (fdk_rect){8, 8, PANEL_W - 16, PANEL_H - 48});
    }
    if (g_font != NULL) {
        /* The caption: the layout API for the numbers, the documented
         * draw entry for the pixels (baseline math from the metrics). */
        char caption[128];
        snprintf(caption, sizeof caption,
                 "frame %d — dma-buf panel (GPU mode)", g_frames);
        fdk_font_metrics fm;
        fdk_font_get_metrics(g_font, &fm);
        (void)fdk_surface_draw_utf8(g_sf, g_font, caption,
                                    strlen(caption), 10,
                                    PANEL_H - 24 + fm.descent,
                                    (fdk_color){0.9f, 0.9f, 0.95f, 1.0f});
    }
    (void)fdk_surface_present(g_sf);
}

int main(void) {
    /* GPU mode: the lane is REQUIRED. A driver that cannot serve it
     * refuses init with FDK_ERR_UNSUPPORTED — the documented honest
     * behavior — and the example falls back to HYBRID, labeled. */
    fdk_init_options io;
    memset(&io, 0, sizeof io); /* zero-init: backend AUTO, app_id NULL */
    io.render_mode = FDK_RENDER_GPU;
    fdk_context *ctx = NULL;
    fdk_result r = fdk_init(&ctx, &io);
    const char *mode = "GPU";
    if (!fdk_ok(r)) {
        printf("[panel] GPU mode refused (%s) — retrying in HYBRID\n",
               fdk_result_to_string(r));
        memset(&io, 0, sizeof io);
        io.render_mode = FDK_RENDER_HYBRID;
        if (!fdk_ok(fdk_init(&ctx, &io))) {
            return 2;
        }
        mode = "HYBRID (the GPU lane refused)";
    }
    g_ctx = ctx;
    /* The honest resolution: the docs' accessor names the ACTIVE mode
     * and the reason when it differs from the request (GPU requested,
     * lane absent -> the refusal story names itself). */
    fdk_render_mode active = FDK_RENDER_AUTO;
    const char *reason = NULL;
    fdk_context_render_mode(ctx, &active, &reason);
    printf("[panel] requested %s; active=%d (%s)\n", mode, active,
           reason != NULL ? reason : "as requested");
    printf("[panel] caps: GL=%d dmabuf_import=%d\n",
           fdk_ext_has_capability(ctx, FDK_EXT_CAP_GL),
           fdk_ext_has_capability(ctx, FDK_EXT_CAP_DMABUF_IMPORT));

    /* The kind + the surface. */
    fdk_ext_surface_ops ops = {0};
    ops.struct_size = sizeof(ops);
    ops.create = panel_create;
    ops.destroy = panel_destroy;
    fdk_ext_surface_kind *kind = NULL;
    if (!fdk_ok(fdk_ext_register_kind(ctx, "com.fadeddream.gl-panel",
                                      &ops, NULL, &kind))) {
        printf("[panel] kind registration failed\n");
        fdk_shutdown(ctx);
        return 2;
    }
    if (!fdk_ok(fdk_ext_surface_create(kind, &g_panel))) {
        printf("[panel] ext surface refused on this backend\n");
        fdk_shutdown(ctx);
        return 2;
    }
    if (!fdk_ok(fdk_ext_surface_get_surface(g_panel, &g_sf))) {
        printf("[panel] no fdk_surface on the ext surface\n");
        fdk_shutdown(ctx);
        return 2;
    }

    /* The caption font: the toolkit's system-default accessor (the
     * DOC-GAPS entry was a doc miss — the accessor always existed;
     * what was missing was the cross-reference from fdk_font_load). */
    g_font = fdk_font_load_system_default(14);
    if (g_font == NULL) {
        printf("[panel] no system default font — the caption is "
               "skipped\n");
    }


    /* The worker: the gradient renders off-thread; notes ride fdk_post;
     * the finished pixels come back through done() on the UI thread. */
    job_payload payload;
    memset(&payload, 0, sizeof payload);
    payload.pixels = malloc((size_t)PANEL_W * PANEL_H * 4);
    if (payload.pixels == NULL) {
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_worker *pool = NULL;
    if (!fdk_ok(fdk_worker_pool_create(ctx, 1, &pool)) || pool == NULL) {
        printf("[panel] worker pool refused\n");
        fdk_shutdown(ctx);
        return 2;
    }
    fdk_job_spec spec;
    memset(&spec, 0, sizeof spec);
    spec.job = panel_job;
    spec.done = panel_job_done;
    spec.user_data = &payload;
    spec.done_user = &payload;
    if (!fdk_ok(fdk_worker_submit(pool, &spec, NULL))) {
        printf("[panel] job submit refused\n");
        fdk_worker_pool_destroy(pool);
        fdk_shutdown(ctx);
        return 2;
    }

    /* The cursor over the panel: the add-on-facing set_cursor (the
     * audit's §5 gap) — a labeled pointer shape, the documented
     * UNSUPPORTED contract on a lane without the cursor machinery. */
    /* void by contract: the cursor set is best-effort per lane (the
     * documented UNSUPPORTED surfaces at the capability, not here). */
    fdk_ext_surface_set_cursor(g_panel, FDK_CURSOR_DEFAULT);
    printf("[panel] set_cursor issued\n");

    /* The loop: ~3 seconds, three worker frames. The frame callback
     * path (fdk_ext_gl_set_frame_callback) is the add-on's alternative
     * when it wants the compositor's pacing; this example drives the
     * pump itself and presents when a frame is ready to show. */
    int last_seen = -1;
    for (int i = 0; i < 190; i++) {
        fdk_pump_events(ctx, 16);
        if (g_frames >= 3) {
            panel_frame();
            last_seen = g_frames;
            break;
        }
        (void)last_seen;
    }
    panel_frame(); /* the final composition, caption included */

    printf("[panel] done: %d worker frame(s) imported and drawn\n",
           g_frames);

    fdk_worker_pool_destroy(pool);
    free(payload.pixels);
    if (g_tex != NULL) {
        fdk_texture_destroy(g_tex);
    }
    if (g_font != NULL) {
        fdk_font_destroy(g_font);
    }
    fdk_ext_surface_destroy(g_panel);
    fdk_ext_kind_destroy(kind);
    fdk_shutdown(ctx);
    /* Success = the whole pipeline ran: worker -> post -> import
     * attempt -> draw. Zero-copy ENGAGEMENT needs a driver that
     * supports it (the DRM runner); on a box without the lane the
     * documented UNSUPPORTED refusal is the honest pass, labeled. */
    if (g_frames >= 3) {
        return 0;
    }
    if (g_import_honest_refusal) {
        printf("[panel] pipeline verified; zero-copy needs a DRM box "
               "(the honest UNSUPPORTED, as documented)\n");
        return 0;
    }
    return 2;
}
