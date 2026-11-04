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

typedef struct mo_app {
    ta_app base;
    mo_cpu cpu;
    mo_mem mem;
    mo_net net;
    bool running;           /* the sampler timer's life            */
    unsigned interval_ms;   /* 250 default, prefs-persisted        */
    unsigned samples;
    /* the UI (graph.c/main.c own the widgets) */
    fdk_widget *cpu_graph;
    fdk_widget *net_graph;
    fdk_widget *cpu_level;
    fdk_widget *mem_level;
    fdk_widget *swap_level;
    char cpu_label[64], mem_label[96], net_label[96];
} mo_app;

extern mo_app mapp;

/* proc.c */
void mo_sample_all(void);       /* one tick: all three sources       */
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

#endif /* FDK_MONITOR_APP_H */
