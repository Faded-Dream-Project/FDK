/* test_extfd_race.c — G-7: an extfd source added between the pump's
 * poll() and its dispatch must not fire with another fd's revents.
 *
 * Found by audit while chasing fdk-files' notification failure
 * (§27/2a): fdk__extfd_dispatch pairs ctx->extfds[i] with the pollfd
 * at index k, but iterates the LIVE source count — a source ADDED
 * after fdk__extfd_fill_poll wrote the pollfd array (reachable
 * publicly: a widget event handler that calls fdk_ext_loop_add_fd
 * runs in the backend-dispatch phase, which sits between poll() and
 * the extfd dispatch in the same pump) is paired with a pollfd
 * slot beyond the filled region: revents the poll never reported
 * for it. The new source's callback then fires with fabricated
 * events (a read on an unauthed socket in the app's case).
 *
 * The rig: a window whose event callback adds a pipe source on the
 * FIRST event the window receives (the map/expose traffic every
 * freshly shown window gets — no injector needed). The pipe has a
 * live writer and no data, so its only honest event set is empty;
 * any same-pump firing with nonzero revents is the bug.
 */
#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int g_pipe_rd = -1;
static int g_pipe_wr = -1;
static fdk_ext_fd_source *g_src;
static volatile int g_added;
static int g_spurious;
static int g_honest;

static void pipe_cb(fdk_ext_fd_source *src, uint32_t revents,
                    void *user) {
    (void)src;
    (void)user;
    if (g_added == 1) {
        g_spurious++;
        printf("BUG: fired in the add pump with revents=0x%x\n",
               revents);
    } else {
        g_honest++;
    }
}

static void window_cb(fdk_window *w, const fdk_event_data *ev,
                      void *user) {
    (void)w;
    (void)ev;
    /* the FIRST event (map/expose traffic at show time): add the
     * source from the backend-dispatch phase — between the pump's
     * poll() and its extfd dispatch */
    if (g_src == NULL && g_pipe_rd >= 0) {
        g_src = fdk_ext_loop_add_fd((fdk_context *)user, g_pipe_rd,
                                    FDK_EXT_FD_IN, pipe_cb, NULL);
        g_added = 1;
    }
}

int main(void) {
    fdk_context *ctx = NULL;
    fdk_init_options o = {0};
    o.app_id = "org.fdk.test.extfdrace";
    if (!fdk_ok(fdk_init(&ctx, &o))) {
        printf("[skip] no display\n");
        return 0;
    }
    int pfd[2];
    if (pipe(pfd) != 0) {
        printf("[skip] no pipe\n");
        return 0;
    }
    g_pipe_rd = pfd[0];
    g_pipe_wr = pfd[1];

    fdk_window *w = NULL;
    fdk_window_options wo = {0};
    wo.width = 200;
    wo.height = 100;
    wo.title = "extfd race";
    assert(fdk_ok(fdk_window_create(ctx, &wo, &w)));
    fdk_window_set_event_callback(w, window_cb, ctx);
    fdk_window_show(w);
    (void)fdk_window_paint(w);

    for (int i = 0; i < 80; i++) {
        (void)fdk_pump_events(ctx, 20);
    }
    if (g_src != NULL) {
        fdk_ext_loop_remove_fd(g_src);
    }
    fdk_window_destroy(w);
    close(pfd[0]);
    close(pfd[1]);

    if (g_spurious > 0) {
        printf("FAIL: %d spurious firings\n", g_spurious);
        fdk_shutdown(ctx);
        return 1;
    }
    printf("[ok] extfd: a mid-pump-added source does not fire with "
           "fabricated revents (added=%d honest=%d)\n",
           g_added, g_honest);
    fdk_shutdown(ctx);
    return 0;
}
