/* stress_rig.c — round 14: the REAL-RATE stress rig (Xlib + XTest,
 * no FDK code). Drives the fdk-files DRIG app at raw input rates —
 * the rates the owner's hardware actually produces, which a
 * 60Hz-spaced rig never models (the round-13 lesson).
 *
 * Modes (RIG_MODE):
 *   scroll — wheel events at 4ms gaps, continuous (the freespin /
 *            trackpad rate; a pinned core during scrolling was the
 *            owner's report)
 *   hover  — 1kHz pointer zigzag across rows (the raw pointer rate;
 *            hover highlight trails were the X2-era complaint)
 *   sbdrag — scrollbar thumb drag: 1kHz motion top->bottom and back
 *
 * Environment:
 *   RIG_PID     the app's pid (CPU is sampled from /proc/<pid>)
 *   RIG_LIST_X/Y/W/H  the list's absolute geometry (DRIG-GEO lines)
 *   RIG_SECONDS storm length (default 5)
 *   RIG_RATE_HZ target rate for hover/sbdrag (default 1000)
 *   RIG_NONAV   1 = skip the enter-directory double-click (a second
 *               rig pass over an already-navigated pane; re-clicking
 *               would open a FILE row)
 *   RIG_DIR     up = wheel button 4 instead of 5 (scroll direction,
 *               for visual round-trip checks)
 *
 * Output: STORM-WINDOW <start_ms> <end_ms> markers on stdout (the
 * monotonic clock the app's PRESENT-TRACE lines carry) + the CPU
 * sample of the storm window.
 */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void usleep_us(long us) {
    struct timespec ts = { us / 1000000L, (us % 1000000L) * 1000L };
    nanosleep(&ts, NULL);
}

/* /proc/<pid>/stat utime+stime in ticks -> seconds. */
static double proc_cpu_ticks(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1.0;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* field 14/15 (utime/stime) — after comm, which may hold spaces
     * inside parens; find the LAST ')'. */
    char *rp = strrchr(buf, ')');
    if (rp == NULL) {
        return -1.0;
    }
    long ut = 0, st = 0;
    if (sscanf(rp + 2, "%*s %*s %*s %*s %*s %*s %*s %*s %*s "
                       "%*s %*s %*s %ld %ld", &ut, &st) != 2) {
        return -1.0;
    }
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) {
        clk = 100;
    }
    return (double)(ut + st) / (double)clk;
}

static void click(Display *dpy, int x, int y) {
    XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
    XSync(dpy, False);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "stress_rig: no display\n");
        return 2;
    }
    pid_t app = (env = getenv("RIG_PID")) != NULL
                    ? (pid_t)atoi(env) : 0;
    int lx = (env = getenv("RIG_LIST_X")) != NULL ? atoi(env) : 138;
    int ly = (env = getenv("RIG_LIST_Y")) != NULL ? atoi(env) : 152;
    int lw = (env = getenv("RIG_LIST_W")) != NULL ? atoi(env) : 900;
    int lh = (env = getenv("RIG_LIST_H")) != NULL ? atoi(env) : 500;
    const char *mode = (env = getenv("RIG_MODE")) != NULL ? env : "scroll";
    int seconds = (env = getenv("RIG_SECONDS")) != NULL ? atoi(env) : 5;
    int rate_hz = (env = getenv("RIG_RATE_HZ")) != NULL ? atoi(env) : 1000;
    /* RIG_NONAV=1: skip the enter-directory double-click (a second
     * rig invocation over an already-navigated pane would double-
     * click a FILE row and open it). RIG_DIR=up sends button 4
     * instead of 5 — the round-trip direction for visual checks. */
    int nonav = (env = getenv("RIG_NONAV")) != NULL ? atoi(env) : 0;
    int wheel_down = ((env = getenv("RIG_DIR")) == NULL ||
                      strcmp(env, "up") != 0);
    if (app <= 0) {
        return 2;
    }

    /* navigate into beta-dir ONCE (double-click row 1) */
    int px = lx + 60;
    int py = ly + 26 + 30 + 15;
    if (!nonav) {
        click(dpy, px, py);
        msleep(30);
        click(dpy, px, py);
        msleep(800); /* let the load + first paint settle */
    }

    /* Storm markers on the RIG's stdout (NOT the app's log — the
     * app's redirect is non-append, and any marker appended to that
     * file gets overwritten by the app's continuing writes). The
     * driver cuts the PRESENT-TRACE timeline by these monotonic
     * timestamps — the same clock the trace lines carry. */
    struct timespec mk;
    clock_gettime(CLOCK_MONOTONIC, &mk);
    long long storm_start_ms = (long long)mk.tv_sec * 1000LL +
                               mk.tv_nsec / 1000000LL;

    long t0 = 0;
    double w0 = 0;
    long ticks0 = 0;
    /* /proc stat sampling uses proc_cpu_ticks; keep raw ticks too. */
    char ppath[64];
    snprintf(ppath, sizeof ppath, "/proc/%ld/stat", (long)app);

    if (strcmp(mode, "scroll") == 0) {
        /* Park the pointer over the list FIRST: wheels go to
         * whatever is under the pointer, and a cold Xvfb pointer
         * sits at (0,0) — window-managerDecoration territory, not
         * the list. The hit-test then routes every wheel to the
         * scrollview under it. */
        XTestFakeMotionEvent(dpy, 0, lx + lw / 2, ly + lh / 2,
                             CurrentTime);
        XSync(dpy, False);
        /* Continuous wheel at 4ms gaps — the freespin/trackpad rate.
         * No bursts-and-rests: the owner's report is a pinned core
         * DURING continuous scrolling. */
        long c0 = (long)(proc_cpu_ticks(app) * 100.0); /* ticks*100/clk adj below */
        (void)c0;
        double cpu0 = proc_cpu_ticks(app);
        w0 = now_s();
        for (double until = now_s() + seconds; now_s() < until;) {
            for (int i = 0; i < 16 && now_s() < until; i++) {
                const unsigned int wheel_btn = wheel_down ? 5u : 4u;
                XTestFakeButtonEvent(dpy, wheel_btn, True, CurrentTime);
                XTestFakeButtonEvent(dpy, wheel_btn, False, CurrentTime);
                usleep_us(4000);
            }
            XSync(dpy, False);
        }
        double cpu1 = proc_cpu_ticks(app);
        double w1 = now_s();
        printf("SCROLL-CPU: %.2f CPU-s over %.1fs => %.1f%% of one core",
               cpu1 - cpu0, w1 - w0, (cpu1 - cpu0) / (w1 - w0) * 100.0);
    } else if (strcmp(mode, "hover") == 0) {
        /* 1kHz zigzag across the rows — the raw pointer rate. The
         * step is ~2 rows per event (a real fast sweep covers rows
         * at exactly this scale). */
        double cpu0 = proc_cpu_ticks(app);
        w0 = now_s();
        int step = 30;
        double gap = 1.0 / (double)(rate_hz > 0 ? rate_hz : 1000);
        for (double until = now_s() + seconds; now_s() < until;) {
            for (int i = 0; i < 16 && now_s() < until; i++) {
                int y = ly + 40 + ((i * step) % (lh > 80 ? lh - 80 : 1));
                XTestFakeMotionEvent(dpy, 0, lx + lw / 2, y,
                                     CurrentTime);
                usleep_us((long)(gap * 1e6));
            }
            XSync(dpy, False);
        }
        double cpu1 = proc_cpu_ticks(app);
        double w1 = now_s();
        printf("HOVER-CPU: %.2f CPU-s over %.1fs => %.1f%% of one core",
               cpu1 - cpu0, w1 - w0, (cpu1 - cpu0) / (w1 - w0) * 100.0);
    } else if (strcmp(mode, "sbdrag") == 0) {
        /* Scrollbar thumb drag: press on the thumb, 1kHz motion top
         * to bottom and back, release. */
        int sb_x = lx + lw - 8; /* the overlay strip hugs the right */
        double cpu0 = proc_cpu_ticks(app);
        w0 = now_s();
        XTestFakeMotionEvent(dpy, 0, sb_x, ly + lh / 4, CurrentTime);
        XSync(dpy, False);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        double gap = 1.0 / (double)(rate_hz > 0 ? rate_hz : 1000);
        int y = ly + lh / 4;
        int dir = 1;
        for (double until = now_s() + seconds; now_s() < until;) {
            for (int i = 0; i < 16 && now_s() < until; i++) {
                y += dir * 4;
                if (y > ly + lh - 8) {
                    y = ly + lh - 8;
                    dir = -1;
                } else if (y < ly + 8) {
                    y = ly + 8;
                    dir = 1;
                }
                XTestFakeMotionEvent(dpy, 0, sb_x, y, CurrentTime);
                usleep_us((long)(gap * 1e6));
            }
            XSync(dpy, False);
        }
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        double cpu1 = proc_cpu_ticks(app);
        double w1 = now_s();
        printf("SBDRAG-CPU: %.2f CPU-s over %.1fs => %.1f%% of one core",
               cpu1 - cpu0, w1 - w0, (cpu1 - cpu0) / (w1 - w0) * 100.0);
    } else {
        fprintf(stderr, "stress_rig: unknown mode '%s'\n", mode);
        return 2;
    }
    (void)ticks0;
    (void)t0;
    (void)ppath;

    /* settle sampling: some handlers run after the last event */
    double cpu_pre_settle = proc_cpu_ticks(app);
    msleep(500);
    double cpu_post = proc_cpu_ticks(app);
    printf(" (+%.2f CPU-s settling)\n", cpu_post - cpu_pre_settle);

    clock_gettime(CLOCK_MONOTONIC, &mk);
    long long storm_end_ms = (long long)mk.tv_sec * 1000LL +
                             mk.tv_nsec / 1000000LL;
    printf("STORM-WINDOW %lld %lld\n", storm_start_ms, storm_end_ms);
    XCloseDisplay(dpy);
    return 0;
}
