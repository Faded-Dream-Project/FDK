/* ui.c — fdk-notes' furniture: the notebook tree, the rich-text
 * editor pane, the focus timer, the reminder lane, the a11y
 * narration sink. */
#include "notes.h"
#include "../common/ta_buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* main.c */
void nt_quit(void);

/* ---- the a11y narration sink ---------------------------------------
 * The README's "a11y narration": the narrator engine speaks focus
 * and state changes through this sink; the app mirrors the last
 * utterance in the status bar (and keeps it for the tests).      */
static void on_speak(const char *utterance, void *user) {
    (void)user;
    if (utterance == NULL || utterance[0] == '\0') {
        return;
    }
    snprintf(napp.last_utterance, sizeof napp.last_utterance, "%s",
             utterance);
    ta_status(&napp.base, "🗣 %s", utterance);
}

void nt_a11y_boot(void) {
    fdk_a11y_set_speaker(on_speak, NULL);
    (void)fdk_a11y_narrator_start();
}

void nt_a11y_shutdown(void) {
    fdk_a11y_set_speaker(NULL, NULL); /* stops the engine too      */
}

/* ---- the tree (notebooks + notes) ---------------------------------- */

static void tree_rebuild(void) {
    fdk_tree_clear(napp.tree);
    for (size_t b = 0; b < napp.book_count; b++) {
        fdk_tree_node book = 0;
        (void)fdk_tree_node_add(napp.tree, FDK_TREE_NODE_NONE,
                                napp.books[b].name, &book);
        for (size_t i = 0; i < napp.books[b].note_count; i++) {
            const nt_note *n =
                &napp.notes[napp.books[b].first_note + i];
            (void)fdk_tree_node_add(napp.tree, book, n->title,
                                    NULL);
        }
    }
}

/* flatten a tree row index -> note index (the tree's row order is
 * the store's order: book, then its notes)                        */
static bool row_to_note(size_t row, size_t *out) {
    size_t seen = 0;
    for (size_t b = 0; b < napp.book_count; b++) {
        size_t book_row = seen++;
        (void)book_row;
        for (size_t i = 0; i < napp.books[b].note_count; i++) {
            if (seen == row) {
                *out = napp.books[b].first_note + i;
                return true;
            }
            seen++;
        }
    }
    return false;
}

/* the tree's row order is the store's order (book, then its
 * notes); a selected LEAF maps back to its note index            */
static bool node_to_note(fdk_tree_node node, size_t *out) {
    if (node == FDK_TREE_NODE_NONE) {
        return false;
    }
    return row_to_note((size_t)node, out);
}

static void on_tree_select(fdk_widget *tree, void *user) {
    (void)user;
    size_t idx = 0;
    if (node_to_note(fdk_tree_get_selected(tree), &idx)) {
        nt_focus_select(idx);
    }
}

/* ---- the editor pane ------------------------------------------------ */

void nt_focus_select(size_t index) {
    if (index >= napp.note_count) {
        return;
    }
    napp.current = index;
    nt_note *n = nt_note_current();
    if (n == NULL) {
        return;
    }
    (void)fdk_entry_set_text(napp.title_entry, n->title);
    (void)fdk_textview_set_text(napp.body_view, n->body);
    nt_apply_attrs();
    char announce[160];
    snprintf(announce, sizeof announce, "%s", n->title);
    fdk_a11y_announce(announce);
    ta_status(&napp.base, "%s", n->title);
}

/* the whole-note rich flags as range attributes over the body */
void nt_apply_attrs(void) {
    nt_note *n = nt_note_current();
    if (n == NULL) {
        (void)fdk_textview_set_attrs(napp.body_view, NULL);
        return;
    }
    fdk_attr_list list;
    if (!fdk_ok(fdk_attr_list_init(&list))) {
        return;
    }
    if (n->bold || n->italic) {
        /* BOLD/ITALIC ride the span kinds: the attr list models
         * size/family/color/underline; weight and slant are span
         * attributes — modeled here as the underline the attr
         * vocabulary offers for italic, and a size delta for bold
         * (the honest v1 mapping; per-range spans are queued)   */
        if (n->italic) {
            fdk_attr a = {0};
            a.byte_start = 0;
            a.byte_end = n->body_len;
            a.kind = FDK_ATTR_UNDERLINE;
            a.v.i = 1;
            (void)fdk_attr_list_add(&list, &a);
        }
        if (n->bold) {
            fdk_attr a = {0};
            a.byte_start = 0;
            a.byte_end = n->body_len;
            a.kind = FDK_ATTR_SIZE_DELTA;
            a.v.f = 1.5f;
            (void)fdk_attr_list_add(&list, &a);
        }
    }
    (void)fdk_textview_set_attrs(napp.body_view, &list);
    fdk_attr_list_destroy(&list);
}

static void on_title_changed(fdk_widget *entry, void *user) {
    (void)user;
    nt_note *n = nt_note_current();
    if (n == NULL) {
        return;
    }
    ta_copy(n->title, sizeof n->title,
            fdk_entry_get_text(entry));
    tree_rebuild();
}

static void on_body_changed(fdk_widget *view, void *user) {
    (void)view;
    (void)user;
    nt_note *n = nt_note_current();
    if (n == NULL) {
        return;
    }
    const char *t = fdk_textview_get_text(napp.body_view);
    ta_copy(n->body, sizeof n->body, t != NULL ? t : "");
    n->body_len = strlen(n->body);
}

/* ---- the focus timer (25 minutes) ---------------------------------- */

static void focus_label_update(void) {
    if (napp.focus_running) {
        int m = napp.focus_seconds_left / 60;
        int s = napp.focus_seconds_left % 60;
        char t[32];
        snprintf(t, sizeof t, "%02d:%02d", m, s);
        (void)fdk_label_set_text(napp.focus_label, t);
    } else {
        (void)fdk_label_set_text(napp.focus_label, ta_("Focus"));
    }
}

static void on_focus_tick(fdk_timer *t, void *user) {
    (void)t;
    (void)user;
    if (!napp.focus_running) {
        return;
    }
    napp.focus_seconds_left--;
    if (napp.focus_seconds_left <= 0) {
        napp.focus_running = false;
        fdk_timer_remove(napp.focus_timer);
        napp.focus_timer = NULL;
        (void)fdk_label_set_text(napp.focus_label, ta_("Focus done"));
        fdk_a11y_announce("Focus session complete");
        nt_notify("Focus session complete",
                  "The 25-minute focus run ended.");
    }
    focus_label_update();
}

void nt_focus_toggle(void) {
    if (napp.focus_running) {
        napp.focus_running = false;
        if (napp.focus_timer != NULL) {
            fdk_timer_remove(napp.focus_timer);
            napp.focus_timer = NULL;
        }
    } else {
        napp.focus_running = true;
        napp.focus_seconds_left = 25 * 60;
        napp.focus_timer = fdk_timer_add(napp.base.ctx, 1000, true,
                                         on_focus_tick, NULL);
    }
    focus_label_update();
}

/* ---- reminders + notifications ------------------------------------- */

static fdk_notify *g_notify;

static void on_note_action(fdk_notify *n, uint32_t id, const char *action,
                           void *note_user, void *user) {
    (void)n;
    (void)id;
    (void)user;
    if (action != NULL && strcmp(action, "open") == 0) {
        size_t idx = (size_t)(uintptr_t)note_user;
        nt_focus_select(idx);
    }
}

void nt_notify(const char *summary, const char *body) {
    if (g_notify == NULL) {
        return;
    }
    fdk_notify_request req;
    memset(&req, 0, sizeof req);
    req.app_name = "fdk-notes";
    req.summary = summary;
    req.body = body;
    req.timeout_ms = 5000;
    (void)fdk_notify_send(g_notify, &req, NULL);
}

void nt_notify_boot(void) {
    fdk_notify_callbacks cb;
    memset(&cb, 0, sizeof cb);
    cb.on_action = on_note_action;
    g_notify = fdk_notify_open(napp.base.ctx, &cb);
}

void nt_notify_shutdown(void) {
    if (g_notify != NULL) {
        fdk_notify_close(g_notify);
        g_notify = NULL;
    }
}

/* the reminder sweep (store.c flips the fired flags) */
static void on_reminder_tick(fdk_timer *t, void *user) {
    (void)t;
    (void)user;
    nt_reminder_check();
    for (size_t i = 0; i < napp.note_count; i++) {
        const nt_note *n = &napp.notes[i];
        if (n->reminder_at > 0 && n->reminder_fired &&
            n->reminder_at + 60 > time(NULL)) {
            /* fire once: within a minute of the flip             */
            char body[200];
            snprintf(body, sizeof body, "%s", n->title);
            static const char *actions[] = {"open", "Open note",
                                            NULL};
            fdk_notify_request req;
            memset(&req, 0, sizeof req);
            req.app_name = "fdk-notes";
            req.summary = ta_("Reminder");
            req.body = body;
            req.actions = actions;
            req.urgency = FDK_NOTIFY_URGENCY_NORMAL;
            (void)fdk_notify_send(g_notify, &req,
                                  (void *)(uintptr_t)i);
            fdk_a11y_announce("Reminder");
        }
    }
}

/* ---- the tray -------------------------------------------------------- */

static fdk_tray_item *g_tray;
static fdk_tray_menu *g_tmenu;

static void on_tray_menu_click(fdk_tray_menu *menu, uint32_t id,
                               void *user) {
    (void)menu;
    (void)user;
    if (id == 1) {
        nt_focus_toggle();
    } else if (id == 2) {
        nt_quit();
    }
}

void nt_tray_boot(void) {
    fdk_tray_item_options opts;
    memset(&opts, 0, sizeof opts);
    opts.id = "fdk-notes";
    opts.title = "fdk-notes";
    opts.category = "Utility";
    g_tray = fdk_tray_item_open(napp.base.ctx, &opts, NULL, NULL);
    if (g_tray == NULL) {
        return;
    }
    g_tmenu = fdk_tray_menu_create();
    (void)fdk_tray_menu_add(g_tmenu, 0, "_Focus timer");
    (void)fdk_tray_menu_add_separator(g_tmenu, 0);
    (void)fdk_tray_menu_add(g_tmenu, 0, "_Quit");
    fdk_tray_menu_on_click(g_tmenu, on_tray_menu_click, NULL);
    fdk_tray_item_set_menu(g_tray, g_tmenu);
    fdk_tray_item_set_tooltip(g_tray, NULL, "fdk-notes",
                              "The notes app");
    fdk_tray_item_set_status(g_tray, FDK_TRAY_STATUS_ACTIVE);
}

void nt_tray_shutdown(void) {
    if (g_tray != NULL) {
        fdk_tray_item_close(g_tray);
        g_tray = NULL;
    }
    if (g_tmenu != NULL) {
        fdk_tray_menu_close(g_tmenu);
        g_tmenu = NULL;
    }
}

/* ---- the build ------------------------------------------------------- */

void nt_ui_build(void) {
    ta_app *a = &napp.base;

    /* the left: the notebook tree */
    fdk_widget *tree_host = NULL;
    (void)fdk_widget_create(a->content, NULL,
                            (fdk_rect){0, 0, 260, 400}, &tree_host);
    (void)fdk_tree_create(tree_host, a->font_ui, &napp.tree);
    fdk_widget_set_expand(napp.tree, false, true);
    fdk_widget_set_accessible_name(napp.tree,
                                   ta_("Notebooks and notes"));
    fdk_tree_set_on_selection_changed(napp.tree, on_tree_select,
                                      NULL);

    /* the right: title entry + body textview + focus label */
    fdk_widget *pane = NULL;
    (void)fdk_widget_create(a->content, NULL,
                            (fdk_rect){0, 0, 640, 400}, &pane);
    fdk_widget *te = NULL;
    (void)fdk_entry_create(pane, a->font_ui, "", &te);
    napp.title_entry = te;
    fdk_widget_set_bounds(te, (fdk_rect){8, 8, 500, 30});
    fdk_entry_set_placeholder(te, ta_("Note title"));
    fdk_entry_set_on_changed(te, on_title_changed, NULL);
    fdk_widget *fl = NULL;
    (void)fdk_label_create(pane, a->font_ui, ta_("Focus"), &fl);
    napp.focus_label = fl;
    fdk_widget_set_bounds(fl, (fdk_rect){520, 12, 110, 24});
    fdk_widget *bv = NULL;
    (void)fdk_textview_create(pane, a->font_ui, &bv);
    napp.body_view = bv;
    fdk_widget_set_bounds(bv, (fdk_rect){8, 46, 620, 330});
    fdk_textview_set_on_changed(bv, on_body_changed, NULL);
    fdk_widget_set_accessible_name(bv, ta_("Note body"));

    /* the reminder sweep, once a minute */
    napp.reminder_timer = fdk_timer_add(a->ctx, 60000, true,
                                        on_reminder_tick, NULL);
}

void nt_tree_refresh(void) {
    tree_rebuild();
}
