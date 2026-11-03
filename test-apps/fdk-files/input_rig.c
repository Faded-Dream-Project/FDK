/* input_rig.c — round 13: the interaction-cost rig (scroll + hover).
 *
 * Boots nothing itself: run_inputrig.sh starts fdk-files (DRIG
 * layout) under Xvfb, navigates INTO the populated beta-dir, then
 * THIS rig storms one of:
 *   mode=scroll — wheel-down bursts over the list (30 rows/frame
 *                 damage: the whole viewport repaints per tick)
 *   mode=hover  — pointer sweeps across rows (hover highlight:
 *                 two rows' damage per crossing)
 * while sampling the app's CPU from /proc (utime+stime deltas,
 * ticks/sec of one core). Prints SCROLL-CPU / HOVER-CPU lines.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

static void msleep(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static long proc_cpu_ticks(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char buf[1024];
    if (fgets(buf, sizeof buf, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);
    char *cp = strrchr(buf, ')');
    if (cp == NULL) {
        return -1;
    }
    long ut = 0, st = 0;
    if (sscanf(cp + 1,
               "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "
               "%ld %ld", &ut, &st) != 2) {
        return -1;
    }
    return ut + st;
}

static void click(Display *dpy, int x, int y) {
    XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
    XSync(dpy, False);
    msleep(40);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    msleep(40);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "input_rig: no display\n");
        return 2;
    }
    pid_t app = (env = getenv("RIG_PID")) != NULL
                    ? (pid_t)atoi(env) : 0;
    int lx = (env = getenv("RIG_LIST_X")) != NULL ? atoi(env) : 138;
    int ly = (env = getenv("RIG_LIST_Y")) != NULL ? atoi(env) : 152;
    const char *mode = (env = getenv("RIG_MODE")) != NULL ? env : "scroll";
    int seconds = (env = getenv("RIG_SECONDS")) != NULL ? atoi(env) : 4;
    if (app <= 0) {
        return 2;
    }
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) {
        clk = 100;
    }

    /* navigate into beta-dir ONCE (double-click row 1) */
    int px = lx + 60;
    int py = ly + 26 + 30 + 15;
    click(dpy, px, py);
    msleep(30);
    click(dpy, px, py);
    msleep(600); /* let the load + first paint settle */

    long t0 = proc_cpu_ticks(app);
    double w0 = now_s();

    if (strcmp(mode, "hover") == 0) {
        /* sweep vertically across rows inside the viewport */
        int top = ly + 70, bot = ly + 400;
        for (double until = now_s() + seconds; now_s() < until;) {
            for (int y = top; y < bot; y += 18) {
                XTestFakeMotionEvent(dpy, 0, px, y, CurrentTime);
                XSync(dpy, False);
                msleep(16);
            }
            for (int y = bot; y > top; y -= 18) {
                XTestFakeMotionEvent(dpy, 0, px, y, CurrentTime);
                XSync(dpy, False);
                msleep(16);
            }
        }
    } else {
        /* wheel bursts: 3 ticks, 30ms apart, then a 60ms rest —
         * the "flick through a long list" rhythm */
        for (double until = now_s() + seconds; now_s() < until;) {
            for (int i = 0; i < 3; i++) {
                XTestFakeButtonEvent(dpy, 5, True, CurrentTime);
                XSync(dpy, False);
                msleep(15);
                XTestFakeButtonEvent(dpy, 5, False, CurrentTime);
                XSync(dpy, False);
                msleep(15);
            }
            msleep(60);
        }
    }

    long t1 = proc_cpu_ticks(app);
    double w1 = now_s();
    msleep(300);
    long t2 = proc_cpu_ticks(app); /* settle: anything still busy? */
    if (t1 < 0 || t2 < 0) {
        fprintf(stderr, "input_rig: app died\n");
        return 2;
    }
    double burst = (double)(t1 - t0) / (double)clk;
    double settle = (double)(t2 - t1) / (double)clk;
    printf("%s-CPU: %.2f CPU-s over %.1fs => %.1f%% of one core "
           "(+%0.2f CPU-s settling)\n",
           strcmp(mode, "hover") == 0 ? "HOVER" : "SCROLL",
           burst, w1 - w0,
           burst / ((w1 - w0) > 0 ? (w1 - w0) : 1) * 100.0, settle);
    XCloseDisplay(dpy);
    return 0;
}
