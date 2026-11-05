/* proc.c — fdk-monitor's sampler: /proc/stat, /proc/meminfo and
 * /proc/net/dev, read synchronously on the UI thread (the files are
 * tiny kernel reads — the 250 ms cadence is the point, not the
 * latency), pushed into the rolling series the canvas plots. */
#include "monitor.h"

#include <dirent.h>
#include <sys/statvfs.h>
#include <time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mo_app mapp;

double mo_series_push(mo_series *s, double v) {
    if (s->count < MO_HISTORY) {
        s->hist[s->count++] = v;
    } else {
        memmove(s->hist, s->hist + 1,
                (MO_HISTORY - 1) * sizeof *s->hist);
        s->hist[MO_HISTORY - 1] = v;
    }
    s->latest = v;
    return v;
}

double mo_series_max(const mo_series *s) {
    double m = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->hist[i] > m) {
            m = s->hist[i];
        }
    }
    return m;
}

/* ---- /proc/stat: aggregate CPU ------------------------------------- */

static void sample_cpu(void) {
    FILE *f = fopen("/proc/stat", "r");
    if (f == NULL) {
        return;
    }
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0,
                       irq = 0, softirq = 0, steal = 0;
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
                   &steal);
    fclose(f);
    if (n < 5) {
        return; /* a truncated first line: keep the last sample    */
    }
    unsigned long long idle_all = idle + iowait;
    unsigned long long total = user + nice + sys + idle_all + irq +
                               softirq + steal;
    unsigned long long dtotal = total - mapp.cpu.total_prev;
    unsigned long long didle = idle_all - mapp.cpu.idle_prev;
    mapp.cpu.total_prev = total;
    mapp.cpu.idle_prev = idle_all;
    if (dtotal == 0) {
        return; /* sample raced a tick: unchanged                  */
    }
    double load = 100.0 * (double)(dtotal - didle) / (double)dtotal;
    if (load < 0) {
        load = 0;
    }
    if (load > 100) {
        load = 100;
    }
    (void)mo_series_push(&mapp.cpu.load, load);
}

/* ---- /proc/meminfo --------------------------------------------------- */

static void sample_mem(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (f == NULL) {
        return;
    }
    char key[48];
    unsigned long long kb = 0;
    unsigned long long total = 0, avail = 0, swap_total = 0,
                       swap_free = 0;
    while (fscanf(f, "%47s %llu", key, &kb) >= 2) {
        if (strcmp(key, "MemTotal:") == 0) {
            total = kb;
        } else if (strcmp(key, "MemAvailable:") == 0) {
            avail = kb;
        } else if (strcmp(key, "SwapTotal:") == 0) {
            swap_total = kb;
        } else if (strcmp(key, "SwapFree:") == 0) {
            swap_free = kb;
        }
    }
    fclose(f);
    if (total == 0) {
        return;
    }
    mapp.mem.total_mb = (double)total / 1024.0;
    mapp.mem.used_mb = (double)(total - avail) / 1024.0;
    mapp.mem.swap_total_mb = (double)swap_total / 1024.0;
    mapp.mem.swap_used_mb = (double)(swap_total - swap_free) / 1024.0;
    (void)mo_series_push(&mapp.mem.used_frac,
                         (double)(total - avail) / (double)total);
}

/* ---- /proc/net/dev: the default route's interface wins, else the
 * first non-loopback interface with traffic --------------------------- */

static void sample_net(void) {
    FILE *f = fopen("/proc/net/dev", "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    if (fgets(line, sizeof line, f) == NULL ||
        fgets(line, sizeof line, f) == NULL) { /* the two headers   */
        fclose(f);
        return;
    }
    unsigned long long best_rx = 0, best_tx = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = ' ';
        char ifname[32];
        unsigned long long rx = 0, tx = 0;
        /* name rx_bytes rx_packets ... then tx_bytes at column 10 */
        unsigned long long vals[16];
        int nf = sscanf(line, "%31s", ifname);
        if (nf != 1) {
            continue;
        }
        char *p = colon;
        size_t vi = 0;
        while (vi < 16) {
            char *end = NULL;
            unsigned long long v = strtoull(p, &end, 10);
            if (end == p) {
                break;
            }
            vals[vi++] = v;
            p = end;
        }
        if (vi < 1) {
            continue;
        }
        rx = vals[0];
        tx = (vi > 8) ? vals[8] : 0;
        if (strncmp(ifname, "lo", 3) == 0) {
            continue; /* loopback: never the metered interface      */
        }
        if (rx + tx >= best_rx + best_tx) {
            best_rx = rx;
            best_tx = tx;
        }
    }
    fclose(f);
    unsigned long long drx = best_rx - mapp.net.rx_prev;
    unsigned long long dtx = best_tx - mapp.net.tx_prev;
    if (mapp.net.rx_prev == 0 && mapp.net.tx_prev == 0) {
        drx = dtx = 0; /* first sample: rates need a delta          */
    }
    mapp.net.rx_prev = best_rx;
    mapp.net.tx_prev = best_tx;
    double secs = (double)mapp.interval_ms / 1000.0;
    mapp.net.rx_rate = (double)drx / secs;
    mapp.net.tx_rate = (double)dtx / secs;
    (void)mo_series_push(&mapp.net.rx_kib, mapp.net.rx_rate / 1024.0);
    (void)mo_series_push(&mapp.net.tx_kib, mapp.net.tx_rate / 1024.0);
}

/* round 25: /proc/uptime + /proc/loadavg (one line each, fixed
 * format by the kernel). Uptime is a double; loads are three
 * decimals. Failing reads leave the previous values alone. */
static void sample_sys(void) {
    FILE *f = fopen("/proc/uptime", "r");
    if (f != NULL) {
        double up = 0.0;
        if (fscanf(f, "%lf", &up) == 1) {
            mapp.sys.uptime_s = up;
        }
        fclose(f);
    }
    f = fopen("/proc/loadavg", "r");
    if (f != NULL) {
        double l1 = 0.0, l5 = 0.0, l15 = 0.0;
        if (fscanf(f, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
            mapp.sys.load1 = l1;
            mapp.sys.load5 = l5;
            mapp.sys.load15 = l15;
        }
        fclose(f);
    }
}

void mo_sample_all(void) {
    sample_cpu();
    sample_mem();
    sample_net();
    sample_sys();
    mapp.samples++;
}

/* ---- round 26: the root filesystem + the process table ---------- */

void mo_sample_disk(void) {
    struct statvfs st;
    if (statvfs("/", &st) != 0 || st.f_blocks == 0) {
        return; /* keep the previous sample on failure            */
    }
    double block = (double)st.f_frsize;
    double total = (double)st.f_blocks * block;
    /* free-to-everyone; the root-reserved blocks count as used
     * (df's rule — a full-ish root disk should read high)      */
    double free_b = (double)st.f_bavail * block;
    if (total <= 0.0) {
        return;
    }
    mapp.disk.total_gb = total / 1073741824.0;
    mapp.disk.used_gb = (total - free_b) / 1073741824.0;
    mapp.disk.used_frac = (total - free_b) / total;
}

/* the per-pid baseline (jiffies at the LAST proc sample) — the
 * delta quotient is the CPU share. A plain array keyed by slot;
 * procs churn, so each sample rebuilds it.                       */
typedef struct proc_prev {
    int pid;
    unsigned long long jiffies;
} proc_prev;

static proc_prev g_prev[MO_PROC_MAX];
static size_t g_prev_count;
static double g_prev_mono;

static unsigned long long prev_jiffies(int pid,
                                       unsigned long long *seed) {
    for (size_t i = 0; i < g_prev_count; i++) {
        if (g_prev[i].pid == pid) {
            return g_prev[i].jiffies;
        }
    }
    return *seed; /* first sight: treat as "no delta yet"       */
}

void mo_sample_procs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double mono = (double)now.tv_sec + (double)now.tv_nsec / 1e9;

    DIR *d = opendir("/proc");
    if (d == NULL) {
        return;
    }
    /* the /proc/stat totals for the same window (the CPU share's
     * denominator). mapp.cpu holds the LAST tick's counters —
     * good enough at 1 Hz granularity.                          */
    unsigned long long total_jiffies = 0;
    {
        FILE *f = fopen("/proc/stat", "r");
        if (f != NULL) {
            char line[256];
            if (fgets(line, sizeof line, f) != NULL &&
                line[0] == 'c' && line[1] == 'p' && line[2] == 'u') {
                unsigned long long v = 0;
                const char *p = line + 3;
                while (*p != '\0' && *p != '\n') {
                    while (*p == ' ') {
                        p++;
                    }
                    if (*p < '0' || *p > '9') {
                        break;
                    }
                    v = 0;
                    while (*p >= '0' && *p <= '9') {
                        v = v * 10u + (unsigned long long)(*p - '0');
                        p++;
                    }
                    total_jiffies += v;
                }
            }
            fclose(f);
        }
    }
    /* the total-jiffies DELTA needs its own baseline            */
    static unsigned long long g_total_prev;
    unsigned long long total_delta = 0;
    if (g_total_prev != 0 && total_jiffies > g_total_prev) {
        total_delta = total_jiffies - g_total_prev;
    }
    g_total_prev = total_jiffies;

    mo_proc out[MO_PROC_MAX];
    proc_prev next[MO_PROC_MAX];
    size_t n = 0, k = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        /* numeric names only (= pids)                            */
        if (e->d_name[0] < '0' || e->d_name[0] > '9') {
            continue;
        }
        /* 256 (NAME_MAX) + the /proc/ prefixes, truncation-proof */
        char path[300];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (f == NULL) {
            continue; /* raced with exit                          */
        }
        char buf[512];
        if (fgets(buf, sizeof buf, f) == NULL) {
            fclose(f);
            continue;
        }
        fclose(f);
        /* "pid (comm) state ..." — comm may hold spaces and
         * parens; everything after the LAST ')' is fixed-field */
        char *rp = strrchr(buf, ')');
        if (rp == NULL) {
            continue;
        }
        *rp = '\0';
        int pid = atoi(buf);
        const char *name_start = strchr(buf, '(');
        if (name_start == NULL) {
            continue;
        }
        name_start++;
        if (n >= MO_PROC_MAX) {
            break;
        }
        mo_proc *p = &out[n++];
        p->pid = pid;
        snprintf(p->name, MO_PROC_NAME, "%s", name_start);
        /* fields after ')': state(3) ... utime(14) stime(15);
         * rp+2 skips " S " — count whitespace-separated fields  */
        unsigned long long utime = 0, stime = 0;
        int field = 3; /* state is field 3                        */
        const char *q = rp + 2;
        while (*q != '\0' && field <= 15) {
            while (*q == ' ') {
                q++;
            }
            if (*q == '\0') {
                break;
            }
            unsigned long long v = 0;
            bool num = false;
            while (*q >= '0' && *q <= '9') {
                v = v * 10u + (unsigned long long)(*q - '0');
                q++;
                num = true;
            }
            if (num) {
                if (field == 14) {
                    utime = v;
                } else if (field == 15) {
                    stime = v;
                }
                field++;
            } else {
                /* a non-numeric token (the state letter, or a
                 * comm remnant) still consumes its slot       */
                while (*q != ' ' && *q != '\0') {
                    q++;
                }
                field++;
            }
        }
        unsigned long long jiffies = utime + stime;
        /* the CPU share: this pid's jiffy growth over the
         * window, as a percent of ALL cores' jiffy growth     */
        unsigned long long zero = 0;
        unsigned long long base = prev_jiffies(pid, &zero);
        p->cpu_pct = 0.0;
        if (total_delta > 0 && base != 0 && jiffies > base) {
            p->cpu_pct = 100.0 * (double)(jiffies - base) /
                         (double)total_delta;
        }
        /* seed the next baseline                                 */
        if (k < MO_PROC_MAX) {
            next[k].pid = pid;
            next[k].jiffies = jiffies;
            k++;
        }
        /* RSS from /proc/<pid>/status (VmRSS, kB)                */
        p->rss_mb = 0.0;
        snprintf(path, sizeof path, "/proc/%d/status", pid);
        f = fopen(path, "r");
        if (f != NULL) {
            while (fgets(buf, sizeof buf, f) != NULL) {
                if (buf[0] == 'V' && buf[1] == 'm' &&
                    buf[2] == 'R' && buf[3] == 'S' && buf[4] == 'S') {
                    p->rss_mb = (double)strtoll(buf + 5, NULL, 10) /
                                1024.0;
                    break;
                }
            }
            fclose(f);
        }
    }
    closedir(d);

    memcpy(g_prev, next, k * sizeof(proc_prev));
    g_prev_count = k;
    g_prev_mono = mono;

    (void)g_prev_mono;
    memcpy(mapp.procs, out, n * sizeof(mo_proc));
    mapp.proc_count = n;
}
