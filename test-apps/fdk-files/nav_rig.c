/* nav_rig.c — round 12: the navigation-storm CPU rig.
 *
 * Boots nothing itself: run_navrig.sh starts fdk-files (DRIG
 * layout) under Xvfb, populates beta-dir with NAVRIG_FILES files
 * (inotify reloads pane0), then THIS rig storms navigation:
 * double-click the beta-dir row (activate -> navigate INTO the big
 * directory), Alt+Left back, repeat NAVRIG_LOOPS times — the
 * "moving thru folders too fast" report, replayed.
 *
 * CPU measurement: /proc/<pid>/stat utime+stime deltas around the
 * storm, sampled by THIS process (the app never instruments
 * itself). % of one core = ticks / (clk_tck * elapsed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
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

/* utime+stime in clock ticks. -1 on failure. */
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
    /* skip past the comm field (parens may contain spaces) */
    char *close_paren = strrchr(buf, ')');
    if (close_paren == NULL) {
        return -1;
    }
    long ut = 0, st = 0;
    /* fields: ) state ppid pgrp session tty tpgid flags minflt
     * cminflt majflt cmajflt utime stime ... — utime=14, stime=15
     * (1-based from the start of the line; after ')' they are 12th
     * and 13th) */
    if (sscanf(close_paren + 1,
               "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "
               "%ld %ld", &ut, &st) != 2) {
        return -1;
    }
    return ut + st;
}

static void click(Display *dpy, int x, int y) {
    XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
    XSync(dpy, False);
    msleep(45);
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
    XSync(dpy, False);
    msleep(45);
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
    XSync(dpy, False);
}

/* A double-click: two clicks ~120ms apart (inside the 400ms
 * double-click window, above the rig-artifact lesson of round 10). */
static void double_click(Display *dpy, int x, int y) {
    click(dpy, x, y);
    msleep(30);
    click(dpy, x, y);
}

/* Alt+Left (Back) through REAL key events. */
static void alt_left(Display *dpy) {
    KeyCode alt = XKeysymToKeycode(dpy, XK_Alt_L);
    KeyCode left = XKeysymToKeycode(dpy, XK_Left);
    XTestFakeKeyEvent(dpy, alt, True, CurrentTime);
    XSync(dpy, False);
    msleep(20);
    XTestFakeKeyEvent(dpy, left, True, CurrentTime);
    XSync(dpy, False);
    msleep(20);
    XTestFakeKeyEvent(dpy, left, False, CurrentTime);
    XSync(dpy, False);
    msleep(20);
    XTestFakeKeyEvent(dpy, alt, False, CurrentTime);
    XSync(dpy, False);
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        fprintf(stderr, "nav_rig: no display\n");
        return 2;
    }
    pid_t app = (env = getenv("NAVRIG_PID")) != NULL
                    ? (pid_t)atoi(env)
                    : (pid_t)0;
    int loops = (env = getenv("NAVRIG_LOOPS")) != NULL ? atoi(env) : 8;
    int lx = (env = getenv("NAVRIG_LIST_X")) != NULL ? atoi(env) : 138;
    int ly = (env = getenv("NAVRIG_LIST_Y")) != NULL ? atoi(env) : 152;
    int files = (env = getenv("NAVRIG_FILES")) != NULL ? atoi(env) : 500;
    if (app <= 0 || loops <= 0) {
        fprintf(stderr, "nav_rig: bad env (pid/loops)\n");
        return 2;
    }
    long clk = sysconf(_SC_CLK_TCK);
    if (clk <= 0) {
        clk = 100;
    }

    /* beta-dir is row 1 (alpha.txt row 0); the double-click point
     * is the row's text band, below the header strip. */
    int px = lx + 60;
    int py = ly + 26 + 30 + 15;

    /* settle: let the inotify reload of the big dir finish */
    msleep(1200);
    long t0 = proc_cpu_ticks(app);
    double w0 = now_s();
    if (t0 < 0) {
        fprintf(stderr, "nav_rig: cannot read /proc/%d/stat\n",
                (int)app);
        return 2;
    }

    for (int i = 0; i < loops; i++) {
        double_click(dpy, px, py); /* INTO the big directory   */
        msleep(250);               /* paint + settle           */
        alt_left(dpy);             /* back to left             */
        msleep(250);
    }

    long t1 = proc_cpu_ticks(app);
    double w1 = now_s();
    if (t1 < 0) {
        fprintf(stderr, "nav_rig: app died mid-storm\n");
        return 2;
    }
    double elapsed = w1 - w0;
    double core_pct =
        ((double)(t1 - t0) / (double)clk) / (elapsed > 0 ? elapsed : 1)
        * 100.0;

    printf("NAV-STORM: %d loops into a %d-file dir in %.1fs "
           "(incl. waits)\n",
           loops, files, elapsed);
    printf("NAV-CPU: app used %.1f CPU-seconds during the storm "
           "=> %.1f%% of one core\n",
           (double)(t1 - t0) / (double)clk, core_pct);
    XCloseDisplay(dpy);
    return 0;
}
