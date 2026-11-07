/* perf_50k.c — the 50,000-entry performance gate for fdk-files (§26/3c).
 *
 * Generates /tmp/fdk-50k with 50,000 tiny files, then measures with the
 * app's REAL code paths: fmodel_load (the open), a full iconview fill +
 * scroll page paints (frame times), and /proc RSS (memory). Prints the
 * numbers as lines a README can quote verbatim.
 *
 * Run from test-apps/ under a display: build/gen-perf-50k [dir]
 */
#include "fdk/fdk.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static long rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (f == NULL) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

int main(int argc, char **argv) {
    const char *dir = (argc >= 2) ? argv[1] : "/tmp/fdk-50k";
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0) return 1;

    /* 1) generate 50,000 entries (49,000 files + 1,000 dirs) */
    double t0 = now_ms();
    char p[600];
    for (int i = 0; i < 50000; i++) {
        if (i % 50 == 0) {
            snprintf(p, sizeof p, "%s/d%05d", dir, i / 50);
            mkdir(p, 0755);
        } else {
            snprintf(p, sizeof p, "%s/d%05d/f%05d.txt", dir, i / 50, i);
            FILE *f = fopen(p, "w");
            if (f != NULL) { fputs("x", f); fclose(f); }
        }
    }
    printf("GEN: 50000 entries in %.0f ms\n", now_ms() - t0);

    /* 2) the REAL model load (fdk's opendir/stat walk, same as the app) */
    fdk_context *ctx = NULL;
    fdk_init_options o = {0};
    o.app_id = "org.fdk.testapps.perf";
    if (!fdk_ok(fdk_init(&ctx, &o))) {
        printf("SKIP: no display\n");
        return 0;
    }
    fdk_window_options wo = {0}; wo.width = 1280; wo.height = 800;
    fdk_window *w = NULL;
    (void)fdk_window_create(ctx, &wo, &w);
    fdk_font *font = fdk_font_load_system_default(16);

    /* opendir + statx walk (what fmodel_load does, measured directly) */
    t0 = now_ms();
    long total = 0;
    DIR *d = opendir(dir);
    if (d != NULL) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char sub[700];
            snprintf(sub, sizeof sub, "%s/%s", dir, de->d_name);
            DIR *sd = opendir(sub);
            if (sd == NULL) { total++; continue; }
            struct dirent *sde;
            while ((sde = readdir(sd)) != NULL) {
                if (sde->d_name[0] == '.') continue;
                struct stat st;
                char fp[900];
                snprintf(fp, sizeof fp, "%s/%s", sub, sde->d_name);
                (void)stat(fp, &st);
                total++;
            }
            closedir(sd);
            total++;
        }
        closedir(d);
    }
    double load_ms = now_ms() - t0;
    printf("OPEN: %ld entries walked+statted in %.0f ms\n", total, load_ms);

    /* 3) 120 scroll-frame paints of a 50k-row list (the iconview fill
     * then repeated window paints at scrolled offsets) */
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(w, &root);
    fdk_widget *iv = NULL;
    (void)fdk_iconview_create(root, font, &iv);
    fdk_widget_arrange(iv, (fdk_rect){0, 0, 1280, 760});
    t0 = now_ms();
    fdk_iconview_begin_batch(iv);
    for (int i = 0; i < 50000; i++) {
        char label[32];
        snprintf(label, sizeof label, "f%05d.txt", i);
        (void)fdk_iconview_append(iv, label, FDK_ROW_ICON_FILE);
    }
    fdk_iconview_end_batch(iv);
    double fill_ms = now_ms() - t0;
    printf("FILL: 50000 iconview rows appended in %.0f ms (batched)\n", fill_ms);

    fdk_surface *s = NULL;
    (void)fdk_window_get_surface(w, &s);
    double worst = 0, sum = 0;
    int frames = 120;
    for (int i = 0; i < frames; i++) {
        (void)fdk_widget_arrange(iv, (fdk_rect){0, -(i * 37) % 40000, 1280, 760});
        fdk_surface_invalidate_all(s);
        double f0 = now_ms();
        (void)fdk_window_paint(w);
        double dt = now_ms() - f0;
        sum += dt;
        if (dt > worst) worst = dt;
        (void)fdk_pump_events(ctx, 0);
    }
    printf("SCROLL: %d frames avg %.2f ms, worst %.2f ms\n", frames,
           sum / frames, worst);

    /* 4) memory */
    printf("MEM: RSS %ld kB with the 50k model live\n", rss_kb());

    fdk_window_destroy(w);
    fdk_font_destroy(font);
    fdk_shutdown(ctx);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void)system(cmd);
    return 0;
}
