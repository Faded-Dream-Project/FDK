/* bench.h — fdk-workbench: the release kitchen-sink.
 *
 * One application, the library ALONE, every desktop subsystem the
 * six focused apps use only one or two of at a time — combined in
 * ONE process, cross-wired: undo drives the doc AND the animated
 * banner; the file dialog feeds the worker thread whose stats feed
 * the level bar; the print path writes a real PDF to disk; i18n
 * re-formats the inspector live; prefs persist the doc; DnD moves
 * rows sidebar → pinned; clipboard round-trips through the system.
 */
#ifndef BENCH_H
#define BENCH_H

#include "app.h"
#include "fdk/fdk_undo.h"

#include <pthread.h>

#define BENCH_MAX_ROWS 512
#define BENCH_TEXT_CAP 128

typedef struct bench_row {
    char text[BENCH_TEXT_CAP];
} bench_row;

typedef struct bench {
    ta_app ta;

    /* the document */
    bench_row rows[BENCH_MAX_ROWS];
    int row_count;
    int selected;
    bool dirty;
    char path[512]; /* empty until opened/saved */

    /* undo */
    fdk_undo_stack *undo;

    /* the widgets the phases talk to */
    fdk_widget *doc_canvas;   /* the table view (paint callback)  */
    fdk_widget *banner;       /* the animated accent sweep        */
    fdk_widget *entry;        /* the quick-add row                */
    fdk_widget *side_list;    /* the sidebar (drag source)        */
    fdk_widget *pinned_list;  /* the inspector (drop target)      */
    fdk_widget *stat_rows, *stat_words, *stat_bytes;
    fdk_widget *level;        /* the doc-size level bar           */
    fdk_widget *fmt_label;    /* the i18n live-format label       */
    double banner_x;          /* the sweep position [0..1]        */
    long banner_paints;       /* diagnostic: paint-walk receipts  */

    /* the worker thread's delivery */
    pthread_t worker;
    bool worker_live;
    long w_lines, w_words, w_bytes;

    /* session (Wayland concept; X11 degrades to NULL — printed) */
    fdk_session *session;

    int undos_applied, redos_applied, drops, prints, posts;
} bench;

/* doc.c — the document + its undo ops + view refresh */
void bench_doc_init(bench *b);
void bench_doc_add(bench *b, const char *text, bool record);
void bench_doc_delete(bench *b, int index, bool record);
void bench_doc_replace(bench *b, int index, const char *text,
                       bool record);
void bench_doc_refresh_views(bench *b);
void bench_doc_save_to(bench *b, const char *path);
void bench_doc_load_from(bench *b, const char *path);
long bench_doc_count_words(const bench *b);

/* the undo op closures (doc.c) */
void bench_undo_apply(bench *b);   /* stack undo + banner */
void bench_undo_redo(bench *b);

/* the banner animation (main.c) */
void bench_banner_sweep(bench *b);

#endif /* BENCH_H */
