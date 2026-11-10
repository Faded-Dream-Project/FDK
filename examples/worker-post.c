/* worker-post.c — P5(e): a background worker thread that reports into
 * the UI loop via fdk_post, public API only. The worker "computes"
 * and posts results; the UI thread paints a bar per received result.
 */
#include <fdk/fdk.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_results = 0;
static fdk_context *g_ctx = NULL;
static fdk_window *g_win = NULL;

static void on_result(void *user) {
    (void)user;
    g_results++;
    if (g_win != NULL) {
        fdk_surface *s = NULL;
        if (fdk_ok(fdk_window_get_surface(g_win, &s))) {
            fdk_surface_fill(s, (fdk_color){ 0.1f, 0.1f, 0.14f, 1.0f });
            for (int i = 0; i < g_results && i < 20; i++) {
                fdk_surface_fill_rect(
                    s, (fdk_rect){ 16 + i * 14, 80, 10, 40 },
                    (fdk_color){ 0.3f, 0.8f, 0.4f, 1.0f });
            }
            fdk_surface_present(s);
        }
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 6; i++) {
        nanosleep(&(struct timespec){ .tv_nsec = 30 * 1000 * 1000 },
                  NULL); /* the "computation" */
        fdk_post(g_ctx, on_result, NULL);
    }
    return NULL;
}

int main(void) {
    if (!fdk_ok(fdk_init(&g_ctx, NULL))) {
        printf("worker-post: no context\n");
        return 77;
    }
    fdk_window_options o;
    memset(&o, 0, sizeof(o));
    o.render_mode = FDK_RENDER_CPU;
    o.title = "fdk-worker-post";
    o.width = 320;
    o.height = 160;
    if (!fdk_ok(fdk_window_create(g_ctx, &o, &g_win))) {
        printf("worker-post: WALL — window create failed\n");
        return 1;
    }
    fdk_window_show(g_win);
    pthread_t th;
    pthread_create(&th, NULL, worker, NULL);
    while (g_results < 6) {
        fdk_pump_events(g_ctx, 16);
    }
    pthread_join(th, NULL);
    printf("worker-post: %d results delivered on the UI thread\n",
           g_results);
    fdk_window_destroy(g_win);
    fdk_shutdown(g_ctx);
    return g_results == 6 ? 0 : 1;
}
