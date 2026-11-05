/* view.h — the terminal view: a focused Canvas widget painting the
 * term grid, plus the input plumbing (keys, IME, mouse reporting,
 * selection) and the PTY fd watch.
 *
 * One view per terminal (a tab or a split pane owns one). The view
 * is display-only state: cell metrics from the profile font, the
 * blink clock, the last pointer cell. All grid truth stays in term,
 * all sequence truth in vt. */
#ifndef FDKTERM_VIEW_H
#define FDKTERM_VIEW_H

#include "term.h"
#include "term_pty.h"
#include "vt.h"

#include "fdk/fdk.h"

typedef struct tprofile tprofile; /* profile.h (app layer) */

typedef struct tview {
    /* model */
    term t;
    vt *parser;
    pty pt;

    /* identity */
    struct tapp *app;        /* back-pointer (window, prefs)        */
    struct twindow *owner;   /* the window/tab that hosts this view */
    int view_id;             /* for logs                            */

    /* the widget */
    fdk_widget *canvas;

    /* metrics */
    fdk_font *font;      /* borrowed from the app's profile fonts  */
    int cell_w, cell_h;  /* advance + line height                  */
    int ascent;

    /* viewport into scrollback (0 = live bottom) */
    int64_t scroll_off;

    /* cursor blink */
    int64_t blink_ms;
    bool blink_on;
    fdk_timer *blink_timer;

    /* visual bell flash */
    int64_t flash_until_ms;

    /* the last pointer cell (motion-report dedup) */
    int last_mx, last_my;

    /* selection drag state */
    bool selecting;
    int sel_anchor_x;
    int64_t sel_anchor_y; /* absolute rows */

    /* search overlay */
    bool search_open;
    char search_text[128];
    int64_t search_row; /* the current match's absolute row */

    /* the PTY read watch */
    fdk_ext_fd_source *watch;

    /* profile snapshot the view renders with */
    int font_px;
    double opacity;      /* the profile's background opacity       */
    tscheme scheme;

    /* unread/bell flags for the tab chrome */
    bool has_unread;
    bool activity_since_focus;
} tview;

/* Creates the view inside `parent` (fills it), spawns the PTY per
 * the profile, and arms the fd watch. The font comes from the app's
 * profile (reloaded on profile changes by the app). */
bool tview_create(tview *v, struct tapp *app, struct twindow *owner,
                  fdk_widget *parent, const char *cwd);

/* Tears down: watch, PTY, parser, grid. */
void tview_destroy(tview *v);

/* Font/metric reload (tprofile change, theme font size change). */
void tview_refont(tview *v, fdk_font *font);

/* Full repaint. */
void tview_invalidate(tview *v);

/* Scroll control (clamped; any nonzero offset pauses the live
 * follow, returning to 0 resumes it). */
void tview_scroll_by(tview *v, int64_t lines);
void tview_scroll_to(tview *v, int64_t off);
bool tview_at_live(tview *v);

/* Focus: routes the canvas focus. */
void tview_focus(tview *v);

/* The current cell size for window-grid math. */
void tview_metrics(const tview *v, int *w, int *h);

/* Grid -> absolute row mapping. */
int64_t tview_abs_y(const tview *v, int grid_y);

#endif /* FDKTERM_VIEW_H */
