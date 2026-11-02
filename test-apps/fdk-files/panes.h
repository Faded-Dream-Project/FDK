/* panes.h — the navigation surfaces of fdk-files. */
#ifndef FILES_PANES_H
#define FILES_PANES_H

#include "files.h"
#include "model.h"

#include "fdk/fdk_widgets.h"

/* One tab: one location + its view. A pane hosts one or more TABS;
 * the task's "two panes" split is two panes in a Paned. */
typedef struct tab {
    char dir[FI_PATH_MAX];
    char title[64];        /* the tab label (the dir's basename)   */
    fdk_widget *page;      /* the tab's page box (owns the view)   */
    fdk_widget *view_host; /* where the current view mode lives    */
    fdk_widget *breadcrumb_host;
    int view_mode;         /* VIEW_DETAIL / VIEW_ICONS / COMPACT   */
    fdk_widget *list;      /* the model-bound list (detail/compact)*/
    /* §29/G-10: the drag-source arm state is gone — FDK's
     * widget-level drag source owns the gesture now. */
} tab;

enum { VIEW_DETAIL = 0, VIEW_ICONS = 1, VIEW_COMPACT = 2 };

typedef struct pane {
    tab *tabs[16];
    size_t tab_count, current;
    fdk_widget *notebook;
    fdk_widget *notebook_host; /* the box the notebook lives in   */

    /* history per pane */
    char back[64][FI_PATH_MAX];
    size_t back_count;
    char fwd[64][FI_PATH_MAX];
    size_t fwd_count;

    /* the breadcrumb's editable path entry (Ctrl+L swaps in) */
    fdk_widget *path_entry;
    bool path_editing;

    /* the live filter box (toolbar tail) */
    fdk_widget *filter_entry;

    /* the recursive search state */
    bool searching;
    char search_root[FI_PATH_MAX];

    /* the inotify watch on the current tab's dir (-1 = none) */
    int watch_fd;

    /* the breadcrumb payloads of the CURRENT bar (rebuilt with it;
     * freed by pane_crumbs_free at rebuild + teardown) */
    struct crumb **crumbs;
    size_t crumb_count, crumb_cap;
} pane;

/* The active pane/tab helpers (panes.c). */
struct pane *active_pane(void);
tab *active_tab(struct pane *p);

/* Builds the whole UI: toolbar, sidebar, panes, statusbar wiring.
 * Returns the pane count (1 or 2). */
int panes_build(files_app *a);

/* Frees a pane's non-widget state (widgets die with the tree). */
void panes_shutdown(void);

/* Navigation with history semantics. */
void pane_back(pane *p);
void pane_forward(pane *p);
void pane_up(pane *p);

/* Tab management. */
void pane_new_tab(pane *p, const char *start_dir);
/* §26/3a: the split view. pane_add() appends the second pane (opens
 * at the active pane's location, becomes active); returns the new
 * pane count or -1 at the two-pane cap. pane_remove_last() closes
 * it (and is a no-op with one pane). */
int pane_add(void);
void pane_remove_last(void);
void pane_close_tab(pane *p, size_t index);

/* The view modes (each rebuilds the current tab's view host). */
void pane_set_view_mode(pane *p, int mode);

/* The breadcrumb bar for the current tab. */
void pane_refresh_breadcrumb(pane *p);

/* Selection helpers. */
void pane_select_all(pane *p);
void pane_invert_selection(pane *p);

/* Drag source: begin a URI drag of the current selection. */
void pane_begin_drag(pane *p);

/* The status line for the current state. */
void pane_report_status(pane *p);

/* type-ahead find state (shared, keyed by pane). */
void pane_typeahead(pane *p, const char *ch);

/* The recursive search (async-shaped via a timer). */
void pane_start_search(pane *p, const char *root, const char *needle);
void pane_stop_search(pane *p);

#endif /* FILES_PANES_H */
