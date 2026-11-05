/* monitor.h — fdk-monitor: the app state + the sampler API. */
#ifndef FDK_MONITOR_APP_H
#define FDK_MONITOR_APP_H

#include "../common/app.h"
#include <stdbool.h>
#include <stddef.h>

/* the rolling-history depth (samples at 250 ms = 4 minutes) */
#define MO_HISTORY 960

typedef struct mo_series {
    double hist[MO_HISTORY]; /* ring: [0] = oldest                */
    size_t count;            /* samples taken (capped at depth)   */
    double latest;           /* the last sampled value             */
} mo_series;

typedef struct mo_cpu {
    unsigned long long idle_prev, total_prev; /* /proc/stat deltas */
    mo_series load;                           /* 0..100 percent    */
} mo_cpu;

typedef struct mo_mem {
    double used_mb, total_mb, swap_used_mb, swap_total_mb;
    mo_series used_frac; /* 0..1 of RAM */
} mo_mem;

typedef struct mo_net {
    unsigned long long rx_prev, tx_prev; /* /proc/net/dev bytes    */
    double rx_rate, tx_rate;             /* bytes/s                */
    mo_series rx_kib, tx_kib;            /* KiB/s history          */
} mo_net;

/* round 25: the system context lines — /proc/uptime and
 * /proc/loadavg, sampled once a tick and shown in the status
 * bar. load1/5/15 are the kernel's own averages. */
typedef struct mo_sys {
    double uptime_s;        /* seconds since boot                    */
    double load1, load5, load15;
} mo_sys;

/* round 26: the root filesystem's usage (statvfs), the 4th
 * levelbar. used_frac is 0..1 of the TOTAL capacity (the
 * reserved-for-root blocks count as used — df's "use%"). */
typedef struct mo_disk {
    double used_gb, total_gb;
    double used_frac;
} mo_disk;

/* round 26: the PROCESS TABLE. One row per live /proc/<pid>:
 * the name, the pid, the CPU share (percent of ALL cores'
 * capacity, GNOME-monitor style), and resident memory. cpu is
 * a DELTA quotient between proc samples — the first sample
 * after boot shows 0 (no baseline yet), like every monitor. */
#define MO_PROC_MAX 1024
#define MO_PROC_NAME 32

typedef struct mo_proc {
    int pid;
    char name[MO_PROC_NAME];
    double cpu_pct;   /* 0..100 * ncores                          */
    double rss_mb;    /* resident set, from VmRSS                 */
} mo_proc;

/* the process-table sort keys (header clicks) */
typedef enum mo_proc_sort {
    MO_SORT_CPU = 0,   /* the default: busiest first              */
    MO_SORT_NAME,
    MO_SORT_PID,
    MO_SORT_RSS,
} mo_proc_sort;

typedef struct mo_app {
    ta_app base;
    mo_cpu cpu;
    mo_mem mem;
    mo_net net;
    mo_sys sys;
    mo_disk disk;
    /* the process table (round 26): procs is the SORTED display
     * array; proclist.c rebuilds the List rows from it ~1 Hz. */
    mo_proc procs[MO_PROC_MAX];
    size_t proc_count;
    mo_proc_sort proc_sort;
    bool proc_sort_desc;   /* every key sorts desc by default    */
    unsigned proc_tick;    /* samples since the last proc sample */
    bool running;           /* the sampler timer's life            */
    unsigned interval_ms;   /* 250 default, prefs-persisted        */
    unsigned samples;
    /* the UI (graph.c/main.c own the widgets) */
    fdk_widget *cpu_graph;
    fdk_widget *net_graph;
    fdk_widget *cpu_level;
    fdk_widget *mem_level;
    fdk_widget *swap_level;
    fdk_widget *disk_level;    /* round 26: / usage            */
    char cpu_label[64], mem_label[96], net_label[96];
} mo_app;

extern mo_app mapp;

/* proc.c */
void mo_sample_all(void);       /* one tick: all three sources       */
void mo_sample_disk(void);      /* statvfs("/") into mapp.disk       */
void mo_sample_procs(void);     /* /proc/<pid> snapshot + CPU deltas */
double mo_series_push(mo_series *s, double v); /* returns v          */
double mo_series_max(const mo_series *s);      /* plot scale helper  */

/* graph.c */
void mo_graphs_build(void);
void mo_graphs_tick(void);

/* tray_notify.c */
void mo_tray_notify_boot(void);
void mo_tray_notify_shutdown(void);
void mo_tray_tick(void);
void mo_notify_boot(void);
void mo_notify_test(void);
void mo_notify_check(void);

/* main.c */
void mo_set_running(bool run);
void mo_tray_on_quit(void);

/* proclist.c (round 26) */
void mo_proclist_build(void);   /* the List + the kill row          */
void mo_proclist_refresh(void); /* resample + rebuild rows          */
void mo_proclist_kill(void);    /* SIGTERM the selected process     */

#endif /* FDK_MONITOR_APP_H */
