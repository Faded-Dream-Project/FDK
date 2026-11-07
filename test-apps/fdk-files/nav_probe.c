/* nav_probe.c — round 12: single-navigation profiler.
 *
 * Boots nothing: run_navprobe.sh boots the DRIG app + a populated
 * beta-dir, then THIS probe double-clicks into beta-dir ONCE and
 * samples, at 20ms resolution: per-THREAD utime+stime of the app
 * and the app's R/S state — identifying WHICH thread burns the
 * ~0.37 CPU-seconds a navigation costs (main vs worker pool), and
 * whether the burn is one contiguous block or a spin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
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

static long thread_ticks(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    char buf[512];
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
    if (sscanf(cp + 1, "%*s %*s %*s %*s %*s %*s %*s %*s %*s %*s "
                        "%*s %ld %ld", &ut, &st) != 2) {
        return -1;
    }
    return ut + st;
}

int main(void) {
    const char *env;
    Display *dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        return 2;
    }
    pid_t app = (env = getenv("NAVRIG_PID")) != NULL
                    ? (pid_t)atoi(env) : 0;
    int lx = (env = getenv("NAVRIG_LIST_X")) != NULL ? atoi(env) : 138;
    int ly = (env = getenv("NAVRIG_LIST_Y")) != NULL ? atoi(env) : 152;
    if (app <= 0) {
        return 2;
    }

    /* snapshot per-thread ticks, then double-click beta-dir, then
     * sample every 20ms for 1.5s printing the MAIN thread's delta
     * and total-delta each tick. */
    char tpath[128];
    snprintf(tpath, sizeof tpath, "/proc/%d/task/%d/stat",
             (int)app, (int)app); /* main thread == pid */

    long main0 = thread_ticks(tpath);
    long tot0 = 0;
    {
        char dpath[128];
        snprintf(dpath, sizeof dpath, "/proc/%d/task", (int)app);
        DIR *d = opendir(dpath);
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] < '0' || de->d_name[0] > '9') {
                    continue;
                }
                char p[512];
                snprintf(p, sizeof p, "%s/%s/stat", dpath,
                         de->d_name);
                long t = thread_ticks(p);
                if (t > 0) {
                    tot0 += t;
                }
            }
            closedir(d);
        }
    }

    int px = lx + 60;
    int py = ly + 26 + 30 + 15;
    /* double-click */
    for (int c = 0; c < 2; c++) {
        XTestFakeMotionEvent(dpy, 0, px, py, CurrentTime);
        XSync(dpy, False);
        msleep(45);
        XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
        XSync(dpy, False);
        msleep(45);
        XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
        XSync(dpy, False);
        msleep(30);
    }

    /* settle 1s, then measure IO/syscalls over 3s windows: a
     * forever-painting app shows growing write_bytes + syscw. */
    msleep(1000);
    {
        char iopath[96];
        snprintf(iopath, sizeof iopath, "/proc/%d/io", (int)app);
        unsigned long long w0 = 0, cw0 = 0, w1 = 0, cw1 = 0;
        FILE *f = fopen(iopath, "r");
        if (f != NULL) {
            char lb[256];
            while (fgets(lb, sizeof lb, f) != NULL) {
                if (strncmp(lb, "write_bytes:", 12) == 0) {
                    sscanf(lb + 12, "%llu", &w0);
                } else if (strncmp(lb, "syscw:", 6) == 0) {
                    sscanf(lb + 6, "%llu", &cw0);
                }
            }
            fclose(f);
        }
        msleep(3000);
        f = fopen(iopath, "r");
        if (f != NULL) {
            char lb[256];
            while (fgets(lb, sizeof lb, f) != NULL) {
                if (strncmp(lb, "write_bytes:", 12) == 0) {
                    sscanf(lb + 12, "%llu", &w1);
                } else if (strncmp(lb, "syscw:", 6) == 0) {
                    sscanf(lb + 6, "%llu", &cw1);
                }
            }
            fclose(f);
        }
        printf("SPIN-3s: wrote %llu KB in %llu syscalls "
               "(presenting?%s)\n",
               (w1 - w0) / 1024, cw1 - cw0,
               (cw1 - cw0) > 500 ? " YES — frames going out" : " no");
        fflush(stdout);
    }

    double t0 = now_s();
    for (int i = 0; i < 75; i++) { /* 1.5s @ 20ms */
        msleep(20);
        long m = thread_ticks(tpath);
        long tot = 0;
        char dpath[128];
        snprintf(dpath, sizeof dpath, "/proc/%d/task", (int)app);
        DIR *d = opendir(dpath);
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] < '0' || de->d_name[0] > '9') {
                    continue;
                }
                char p[512];
                snprintf(p, sizeof p, "%s/%s/stat", dpath,
                         de->d_name);
                long t = thread_ticks(p);
                if (t > 0) {
                    tot += t;
                }
            }
            closedir(d);
        }
        printf("%5.0fms main+%4ld all+%4ld\n",
               (now_s() - t0) * 1000.0, m - main0, tot - tot0);
        fflush(stdout);
        if (m < 0) {
            break; /* app died */
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
