/* search.c — the find-in-scrollback bar: a SearchEntry + next/prev
 * + close, revealed under the notebook. Searches wrap; the match
 * row scrolls into view and paints a highlight band. */
#include "term_app.h"

#include <stdio.h>
#include <string.h>

static int64_t term_search_start_hint(tview *v);

typedef struct {
    twindow *w;
    fdk_widget *bar;
    fdk_widget *entry;
    fdk_widget *label;
    int64_t last_row;
} sbar;

static sbar g_sbar;

static void bar_search_changed(fdk_widget *entry, const char *text,
                               void *user) {
    (void)entry;
    (void)user;
    /* live: start a fresh search from the top of the visible range */
    if (text != NULL && text[0] != '\0') {
        g_sbar.last_row = -1;
        tsearch_next(g_sbar.w);
    }
}

static void bar_activate(fdk_widget *entry, void *user) {
    (void)entry;
    (void)user;
    tsearch_next(g_sbar.w);
}

static void bar_close_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    (void)user;
    tsearch_close(g_sbar.w);
}

void tsearch_toggle(twindow *w) {
    if (g_sbar.bar != NULL && g_sbar.w == w) {
        tsearch_close(w);
        return;
    }
    if (g_sbar.bar != NULL) {
        tsearch_close(g_sbar.w);
    }
    g_sbar.w = w;
    g_sbar.last_row = -1;

    /* the bar: [entry][count label][close] */
    fdk_widget *bar = NULL;
    fdk_box_create(w->notebook /* reparented below */, FDK_HORIZONTAL,
                   &bar);
    /* insert ABOVE the notebook's pages: the notebook adopts pages,
     * so the bar goes into the notebook's PARENT right before it. */
    /* the notebook's parent is the content frame; reparent the bar
     * there (it lays out above the notebook, under the menubar) */
    fdk_widget *frame = w->content != NULL ? w->content : w->root;
    if (frame != NULL) {
        (void)fdk_widget_reparent(bar, frame);
    }
    fdk_box_set_padding(bar, 6);
    fdk_box_set_spacing(bar, 6);

    fdk_search_entry_create(bar, w->app->font_ui, &g_sbar.entry);
    fdk_widget_set_expand(g_sbar.entry, true, false);
    fdk_search_entry_set_on_search(g_sbar.entry, bar_search_changed, w);
    fdk_entry_set_on_activate(g_sbar.entry, bar_activate, w);
    fdk_entry_set_placeholder(g_sbar.entry, "Find in scrollback");

    fdk_label_create(bar, w->app->font_ui, "", &g_sbar.label);
    fdk_widget_set_natural_size(g_sbar.label, 120, 24);

    fdk_widget *btn = NULL;
    fdk_button_create(bar, w->app->font_ui, "Close", &btn);
    fdk_button_set_on_activate(btn, bar_close_clicked, w);

    g_sbar.bar = bar;
    fdk_widget_focus(g_sbar.entry);
}

void tsearch_next(twindow *w) {
    if (g_sbar.bar == NULL || g_sbar.w != w) {
        return;
    }
    tview *v = tapp_focused_view(w);
    const char *text = fdk_entry_get_text(g_sbar.entry);
    if (v == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    int64_t from = g_sbar.last_row + 1;
    if (from >= term_search_start_hint(v)) {
        from = 0;
    }
    int col = 0;
    int64_t row = term_search(&v->t, text, from, &col);
    if (row < 0) {
        fdk_label_set_text(g_sbar.label, "not found");
        return;
    }
    g_sbar.last_row = row;
    char msg[64];
    snprintf(msg, sizeof msg, "row %lld", (long long)row);
    fdk_label_set_text(g_sbar.label, msg);

    /* scroll the match into the middle of the view */
    int64_t abs_top = (int64_t)v->t.back_count;
    int64_t target = row - (v->t.rows / 2);
    if (target < 0) {
        target = 0;
    }
    tview_scroll_to(v, abs_top - target);
    v->search_row = row;
    snprintf(v->search_text, sizeof v->search_text, "%s", text);
    tview_invalidate(v);
}

void tsearch_close(twindow *w) {
    if (g_sbar.bar == NULL) {
        return;
    }
    if (g_sbar.w != NULL) {
        tview *v = tapp_focused_view(g_sbar.w);
        if (v != NULL) {
            v->search_row = -1;
            v->search_text[0] = '\0';
            tview_invalidate(v);
        }
    }
    fdk_widget_destroy(g_sbar.bar);
    memset(&g_sbar, 0, sizeof g_sbar);
    (void)w;
}

/* the wrap point for search: past the last absolute row */
static int64_t term_search_start_hint(tview *v) {
    return (int64_t)v->t.back_count + v->t.rows;
}
