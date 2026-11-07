/* layer_repro.c — the minimal layer-shell client that replays the
 * exact request sequence the hud add-on traced (round 29) against a
 * real wlroots compositor, with a ROUNDTRIP after every step so any
 * wl_display.error is read and NAMED the moment it arrives — the
 * diagnostic the original flow never got (it died mid-flush, outside
 * any dispatch).
 *
 * variants (argv[1], default "a"):
 *   a  plain: configure → ack → attach+damage+frame+commit
 *   b  like a, plus wl_surface.set_buffer_scale(1) before attach
 *      (what FDK's present path sent)
 *   c  like a but ARGB8888 buffer instead of XRGB8888
 *   d  like a but TWO commits back-to-back without dispatching
 *      between (what FDK's hud did on frames 1 and 2)
 *
 * Prints STEP lines; on any error, prints the protocol error's
 * interface/object/code and exits 42. Survives 10 frame cycles then
 * prints ALIVE and exits 0.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "layer-shell.h"

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct zwlr_layer_shell_v1 *lshell;
static struct wl_shm *shm;
static uint32_t comp_id, comp_ver;
static struct wl_surface *surf;
static struct zwlr_layer_surface_v1 *lsurf;
static int configured;
static int died;

static void _REGISTRY(void *d, struct wl_registry *r, uint32_t name,
                     const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        comp_id = name; comp_ver = ver;
        comp = wl_registry_bind(r, name, &wl_compositor_interface,
                                ver < 5 ? ver : 5);
    }
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        lshell = wl_registry_bind(r, name,
                                  &zwlr_layer_shell_v1_interface, 1);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
}
static void _REGISTRY_DONE(void *d, struct wl_registry *r) { (void)d; (void)r; }
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

static const struct wl_registry_listener reg_l = {
    .global = _REGISTRY, .global_remove = _REGISTRY_DONE };

static void LCONF(void *d, struct zwlr_layer_surface_v1 *s,
                  uint32_t serial, uint32_t w, uint32_t h) {
    (void)d; (void)w; (void)h;
    zwlr_layer_surface_v1_ack_configure(s, serial);
    configured = 1;
}
static void LCLOSED(void *d, struct zwlr_layer_surface_v1 *s) {
    (void)d; (void)s; died = 1;
}
static const struct zwlr_layer_surface_v1_listener l_l = {
    .configure = LCONF, .closed = LCLOSED };

static int step(const char *what) {
    if (wl_display_roundtrip(dpy) < 0 || died) {
        uint32_t code = 0;
        const struct wl_interface *iface = NULL;
        uint32_t id = 0;
        code = wl_display_get_protocol_error(dpy, &iface, &id);
        printf("DIED at %s: errno=%s", what, strerror(errno));
        if (iface)
            printf(" — protocol error %u on %s#%u", code,
                   iface->name, id);
        else
            printf(" — no protocol error readable");
        printf("\n");
        return -1;
    }
    printf("STEP ok: %s\n", what);
    return 0;
}

static struct wl_buffer *make_buffer(int w, int h, int argb) {
    int stride = w * 4;
    int size = stride * h;
    char name[] = "/fdk-layer-repro";
    int fd = memfd_create(name, 0);
    ftruncate(fd, size);
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      fd, 0);
    memset(data, 0x80, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(
        pool, 0, w, h, stride,
        argb ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    munmap(data, size);
    return buf;
}

int main(int argc, char **argv) {
    const char *v = argc > 1 ? argv[1] : "a";
    int use_scale = v[0] == 'b';
    int use_argb = v[0] == 'c';
    int double_commit = v[0] == 'd';
    int W = 240, H = 200;

    dpy = wl_display_connect(NULL);
    if (!dpy) { perror("connect"); return 1; }
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_l, NULL);
    if (step("registry roundtrip")) return 42;
    if (!comp || !lshell || !shm) {
        printf("missing globals: comp=%p lshell=%p shm=%p\n",
               (void *)comp, (void *)lshell, (void *)shm);
        return 3;
    }
    struct wl_compositor *comp2 = comp;
    if (v[0] == 'g') {
        /* the fdk-hud bind: wl_compositor pinned at version 1 */
        struct wl_registry *r4 = wl_display_get_registry(dpy);
        comp2 = NULL;
        comp2 = wl_registry_bind(r4, comp_id, &wl_compositor_interface, 1);
    }
    if (v[0] == 'f') {
        struct wl_registry *reg2 = wl_display_get_registry(dpy);
        wl_registry_add_listener(reg2, &reg_l, NULL);
        wl_display_roundtrip(dpy);
        /* _REGISTRY bound comp/lshell/shm through reg2's globals too;
         * bind a SECOND compositor via version 5 from the same registry */
        comp2 = NULL;
        struct wl_registry *r3 = wl_display_get_registry(dpy);
        /* reuse the main listener: it rebinds comp — save/restore */
        struct wl_compositor *keep = comp;
        wl_registry_add_listener(r3, &reg_l, NULL);
        wl_display_roundtrip(dpy);
        comp2 = comp;
        comp = keep;
    }
    surf = wl_compositor_create_surface(comp2);
    lsurf = zwlr_layer_shell_v1_get_layer_surface(
        lshell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "fdk-repro");
    zwlr_layer_surface_v1_set_anchor(
        lsurf, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
               ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_margin(lsurf, 90, 700, 0, 0);
    zwlr_layer_surface_v1_set_size(lsurf, W, H);
    zwlr_layer_surface_v1_set_exclusive_zone(lsurf, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        lsurf, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(lsurf, &l_l, NULL);
    wl_surface_commit(surf);
    if (step("initial commit → configure")) return 42;
    if (!configured) { printf("no configure!\n"); return 5; }

    struct wl_buffer *buf = make_buffer(W, H, use_argb);
    for (int frame = 0; frame < 10; frame++) {
        if (use_scale) wl_surface_set_buffer_scale(surf, 1);
        wl_surface_attach(surf, buf, 0, 0);
        if (frame == 0 && v[0] >= 'e')
            wl_surface_damage(surf, 0, 0, 2147483647, 2147483647);
        else
            wl_surface_damage(surf, 0, 0, W, H);
        wl_surface_frame(surf);
        wl_surface_commit(surf);
        char what[64];
        snprintf(what, sizeof what, "frame %d commit", frame);
        if (step(what)) return 42;
        if (double_commit) {
            wl_surface_attach(surf, buf, 0, 0);
            wl_surface_damage(surf, 0, 0, W, H);
            wl_surface_frame(surf);
            wl_surface_commit(surf);
        }
        wl_buffer_destroy(buf);
        buf = make_buffer(W, H, use_argb);
    }
    printf("ALIVE after 10 frames (variant %s)\n", v);
    return 0;
}
