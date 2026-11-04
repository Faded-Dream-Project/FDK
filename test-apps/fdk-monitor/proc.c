/* proc.c — fdk-monitor's sampler: /proc/stat, /proc/meminfo and
 * /proc/net/dev, read synchronously on the UI thread (the files are
 * tiny kernel reads — the 250 ms cadence is the point, not the
 * latency), pushed into the rolling series the canvas plots. */
#include "monitor.h"

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

void mo_sample_all(void) {
    sample_cpu();
    sample_mem();
    sample_net();
    mapp.samples++;
}
