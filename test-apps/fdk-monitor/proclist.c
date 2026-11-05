/* proclist.c — fdk-monitor's process table (round 26): the List
 * with Name / CPU% / RSS / PID columns, rebuilt once a second
 * from mo_sample_procs(), sorted by the header the user clicked
 * (the round-25 header-click signal), with a Kill row that
 * SIGTERMs the selected process after a Yes/No ask.
 *
 * The rebuild is WHOLE-TABLE (clear + append in one batch) — the
 * honest v1: pids churn, and diffing 300 rows by hand buys
 * nothing at 1 Hz. The BATCH keeps it one relayout. Selection is
 * re-anchored by PID after each rebuild (the same process stays
 * selected while it lives) — the thing a real monitor owes. */
#include "monitor.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static fdk_widget *g_list;
static int g_sel_pid = -1; /* re-selection anchor across rebuilds */

/* ---- sorting ------------------------------------------------------- */

static int cmp_cpu(const void *a, const void *b) {
    const mo_proc *x = a, *y = b;
    return (x->cpu_pct < y->cpu_pct) ? -1
           : (x->cpu_pct > y->cpu_pct) ? 1
           : (x->pid < y->pid) ? -1 : (x->pid > y->pid) ? 1 : 0;
}
static int cmp_name(const void *a, const void *b) {
    const mo_proc *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    return (c != 0) ? c : (x->pid < y->pid ? -1 : 1);
}
static int cmp_pid(const void *a, const void *b) {
    const mo_proc *x = a, *y = b;
    return (x->pid < y->pid) ? -1 : (x->pid > y->pid) ? 1 : 0;
}
static int cmp_rss(const void *a, const void *b) {
    const mo_proc *x = a, *y = b;
    return (x->rss_mb < y->rss_mb) ? -1
           : (x->rss_mb > y->rss_mb) ? 1
           : (x->pid < y->pid) ? -1 : (x->pid > y->pid) ? 1 : 0;
}

/* sorts mapp.procs IN PLACE into the current (sort, desc)      */
static void procs_sort(void) {
    int (*fn)(const void *, const void *);
    switch (mapp.proc_sort) {
    case MO_SORT_NAME:
        fn = cmp_name;
        break;
    case MO_SORT_PID:
        fn = cmp_pid;
        break;
    case MO_SORT_RSS:
        fn = cmp_rss;
        break;
    default:
        fn = cmp_cpu;
        break;
    }
    qsort(mapp.procs, mapp.proc_count, sizeof(mo_proc), fn);
    if (mapp.proc_sort_desc) {
        /* reverse in place (keep the tie-stable pid order too) */
        for (size_t i = 0, j = mapp.proc_count; i + 1 < j;) {
            mo_proc t = mapp.procs[i];
            mapp.procs[i++] = mapp.procs[--j];
            mapp.procs[j] = t;
        }
    }
}

/* ---- the row rebuild ------------------------------------------------ */

void mo_proclist_refresh(void) {
    if (g_list == NULL) {
        return;
    }
    mo_sample_procs();
    procs_sort();

    fdk_list_begin_batch(g_list);
    fdk_list_clear(g_list);
    int reselect = -1;
    for (size_t i = 0; i < mapp.proc_count; i++) {
        const mo_proc *p = &mapp.procs[i];
        char cpu[24], rss[24], pid[16];
        snprintf(cpu, sizeof cpu, "%.1f%%", p->cpu_pct);
        if (p->rss_mb >= 1024.0) {
            snprintf(rss, sizeof rss, "%.1f GB", p->rss_mb / 1024.0);
        } else {
            snprintf(rss, sizeof rss, "%.0f MB", p->rss_mb);
        }
        snprintf(pid, sizeof pid, "%d", p->pid);
        size_t row = 0;
        (void)fdk_list_append(g_list, p->name, &row);
        (void)fdk_list_row_set_cell(g_list, row, 1, cpu);
        (void)fdk_list_row_set_cell(g_list, row, 2, rss);
        (void)fdk_list_row_set_cell(g_list, row, 3, pid);
        if (p->pid == g_sel_pid) {
            reselect = (int)i;
        }
    }
    fdk_list_end_batch(g_list);
    /* re-anchor the selection by PID (finds the row, if alive) */
    if (reselect >= 0) {
        (void)fdk_list_select(g_list, (size_t)reselect);
    } else {
        g_sel_pid = -1;
    }
}

/* ---- selection + kill ---------------------------------------------- */

static void on_selection(fdk_widget *list, void *user) {
    (void)user;
    fdk_i64 row = fdk_list_get_selected(list);
    if (row >= 0 && (size_t)row < mapp.proc_count) {
        g_sel_pid = mapp.procs[(size_t)row].pid;
    } else {
        g_sel_pid = -1;
    }
}

static const mo_proc *selected_proc(void) {
    if (g_sel_pid < 0) {
        return NULL;
    }
    for (size_t i = 0; i < mapp.proc_count; i++) {
        if (mapp.procs[i].pid == g_sel_pid) {
            return &mapp.procs[i];
        }
    }
    return NULL;
}

static void on_kill_answer(fdk_dialog_response response, void *user) {
    int pid = (int)(intptr_t)user;
    if (response != FDK_DIALOG_YES) {
        return;
    }
    if (kill((pid_t)pid, SIGTERM) == 0) {
        ta_status(&mapp.base, "Sent SIGTERM to %d", pid);
    } else {
        ta_status(&mapp.base, "Cannot signal %d (permissions?)", pid);
    }
}

void mo_proclist_kill(void) {
    const mo_proc *p = selected_proc();
    if (p == NULL) {
        ta_status(&mapp.base, "%s", ta_("Select a process first"));
        return;
    }
    char msg[160];
    snprintf(msg, sizeof msg,
             "End process %s (PID %d)?\n"
             "Unsaved work in it will be lost.",
             p->name, p->pid);
    fdk_dialog_options o;
    memset(&o, 0, sizeof o);
    o.title = ta_("End process");
    o.text = msg;
    o.buttons = FDK_DIALOG_BUTTONS_YES_NO;
    o.modal = true;
    o.font = mapp.base.font_ui;
    o.parent = mapp.base.window;
    (void)fdk_dialog_show_message(mapp.base.ctx, &o, on_kill_answer,
                                  (void *)(intptr_t)p->pid, NULL);
}

/* the header click: sort by the column, second click flips */
static void on_header(fdk_widget *list, size_t column, void *user) {
    (void)list;
    (void)user;
    static const mo_proc_sort map[4] = {
        MO_SORT_NAME, MO_SORT_CPU, MO_SORT_RSS, MO_SORT_PID,
    };
    mo_proc_sort want = (column < 4) ? map[column] : MO_SORT_CPU;
    if (mapp.proc_sort == want) {
        mapp.proc_sort_desc = !mapp.proc_sort_desc;
    } else {
        mapp.proc_sort = want;
        /* CPU and RSS read naturally as "biggest first"; the
         * name and pid columns as "ascending"                      */
        mapp.proc_sort_desc = (want == MO_SORT_CPU ||
                               want == MO_SORT_RSS);
    }
    static const char *names[4] = {"name", "CPU", "RSS", "PID"};
    ta_status(&mapp.base, "Processes sorted by %s (%s)",
              names[column],
              mapp.proc_sort_desc ? "descending" : "ascending");
    mo_proclist_refresh();
}

/* ---- build --------------------------------------------------------- */

void mo_proclist_build(void) {
    fdk_font *f = mapp.base.font_ui;
    fdk_widget *list = NULL;
    (void)fdk_list_create(mapp.base.content, f, &list);
    g_list = list;
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_SINGLE);
    fdk_widget_set_expand(list, true, true);
    fdk_widget_set_accessible_name(list, "Processes");
    fdk_list_column cols[4] = {
        {"Process", 190, true},
        {"CPU", 80, false},
        {"Memory", 90, false},
        {"PID", 70, false},
    };
    (void)fdk_list_set_columns(list, cols, 4);
    fdk_list_set_on_header_click(list, on_header, NULL);
    fdk_list_set_on_selection_changed(list, on_selection, NULL);

    /* the Kill row under the list */
    fdk_widget *kill = NULL;
    (void)fdk_button_create(mapp.base.content, f,
                            ta_("End process..."), &kill);
    fdk_button_set_on_activate(kill, (fdk_button_activate_fn)
                                        mo_proclist_kill, NULL);
    fdk_widget_set_accessible_name(kill, "End process");
    fdk_widget_set_tooltip(kill, "SIGTERM the selected process "
                                 "(Ctrl+K)");

    /* the first sample, sorted by CPU (the default)             */
    mapp.proc_sort = MO_SORT_CPU;
    mapp.proc_sort_desc = true;
    mo_proclist_refresh();
}
