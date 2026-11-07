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

/* round 25: does this note match the search needle (title or
 * body, case-insensitive ASCII like fdk-files' filter)? An empty
 * needle matches everything. */
static bool note_matches(const nt_note *n, const char *needle) {
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    const char *hay[2] = {n->title, n->body};
    size_t nl = strlen(needle);
    for (int h = 0; h < 2; h++) {
        const char *s = hay[h];
        size_t sl = strlen(s);
        for (size_t i = 0; i + nl <= sl; i++) {
            size_t j = 0;
            while (j < nl) {
                char a = s[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') {
                    a = (char)(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z') {
                    b = (char)(b - 'A' + 'a');
                }
                if (a != b) {
                    break;
                }
                j++;
            }
            if (j == nl) {
                return true;
            }
        }
    }
    return false;
}

static void tree_rebuild(void) {
    fdk_tree_clear(napp.tree);
    const char *needle = napp.search;
    size_t shown = 0;
    for (size_t b = 0; b < napp.book_count; b++) {
        fdk_tree_node book = 0;
        for (size_t i = 0; i < napp.books[b].note_count; i++) {
            const nt_note *n =
                &napp.notes[napp.books[b].first_note + i];
            if (!note_matches(n, needle)) {
                continue;
            }
            if (book == 0) {
                (void)fdk_tree_node_add(napp.tree,
                                        FDK_TREE_NODE_NONE,
                                        napp.books[b].name, &book);
            }
            (void)fdk_tree_node_add(napp.tree, book, n->title,
                                    NULL);
            shown++;
        }
    }
    if (needle != NULL && needle[0] != '\0') {
        ta_status(&napp.base, ta_("%zu notes match \"%s\""),
                  shown, needle);
    }
}

/* flatten a tree row index -> note index (the tree's row order is
 * the store's order: book, then its notes)                        */
static bool row_to_note(size_t row, size_t *out) {
    /* round 25: the walk mirrors tree_rebuild exactly — books
     * only occupy a row when at least one of their notes matches
     * the needle, and non-matching notes never occupy one. */
    size_t seen = 0;
    for (size_t b = 0; b < napp.book_count; b++) {
        size_t book_row = SIZE_MAX;
        for (size_t i = 0; i < napp.books[b].note_count; i++) {
            const nt_note *n =
                &napp.notes[napp.books[b].first_note + i];
            if (!note_matches(n, napp.search)) {
                continue;
            }
            if (book_row == SIZE_MAX) {
                book_row = seen++;
            }
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

/* Round 22: the per-range rich text. The note's spans become the
 * textview's attribute list — italic maps to the underline the
 * attr vocabulary offers, bold to a size delta (the honest v1
 * mapping until the attr kinds grow weight/slant entries).     */
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
    for (size_t i = 0; i < n->span_count; i++) {
        const nt_span *sp = &n->spans[i];
        if (sp->end <= sp->start) {
            continue; /* collapsed by an edit: dead span          */
        }
        if (sp->italic) {
            fdk_attr a = {0};
            a.byte_start = sp->start;
            a.byte_end = sp->end;
            a.kind = FDK_ATTR_UNDERLINE;
            a.v.i = 1;
            (void)fdk_attr_list_add(&list, &a);
        }
        if (sp->bold) {
            fdk_attr a = {0};
            a.byte_start = sp->start;
            a.byte_end = sp->end;
            a.kind = FDK_ATTR_SIZE_DELTA;
            a.v.f = 1.5f;
            (void)fdk_attr_list_add(&list, &a);
        }
    }
    (void)fdk_textview_set_attrs(napp.body_view, &list);
    fdk_attr_list_destroy(&list);
}

/* Toggles `which` over the CURRENT SELECTION. A collapsed
 * selection formats nothing (the caller tells the user).        */
bool nt_format_selection(bool bold, bool italic) {
    nt_note *n = nt_note_current();
    if (n == NULL || napp.body_view == NULL) {
        return false;
    }
    size_t anchor = 0, caret = 0;
    if (!fdk_textview_get_selection(napp.body_view, &anchor,
                                    &caret)) {
        return false;
    }
    size_t lo = (anchor < caret) ? anchor : caret;
    size_t hi = (anchor < caret) ? caret : anchor;
    if (lo >= hi || hi > n->body_len) {
        return false;
    }
    for (size_t i = 0; i < n->span_count; i++) {
        nt_span *sp = &n->spans[i];
        if (sp->start <= lo && lo < sp->end) {
            if (bold) {
                sp->bold = !sp->bold;
            }
            if (italic) {
                sp->italic = !sp->italic;
            }
            if (!sp->bold && !sp->italic) {
                memmove(&n->spans[i], &n->spans[i + 1],
                        (n->span_count - i - 1) * sizeof(nt_span));
                n->span_count--;
            }
            nt_apply_attrs();
            return true;
        }
    }
    if (n->span_count >= NT_MAX_SPANS) {
        return false;
    }
    nt_span *sp = &n->spans[n->span_count++];
    sp->start = lo;
    sp->end = hi;
    sp->bold = bold;
    sp->italic = italic;
    nt_apply_attrs();
    return true;
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
    /* spans past the new end are dead; clamp then drop empties
     * (the honest v1: ranges are byte-anchored, edits do not
     * reflow them — interior edits keep the surrounding text)  */
    size_t w = 0;
    for (size_t i = 0; i < n->span_count; i++) {
        if (n->spans[i].end > n->body_len) {
            n->spans[i].end = n->body_len;
        }
        if (n->spans[i].start > n->spans[i].end) {
            n->spans[i].start = n->spans[i].end;
        }
        if (n->spans[i].end > n->spans[i].start) {
            n->spans[w++] = n->spans[i];
        }
    }
    n->span_count = w;
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

/* the search box's live filter: copy the needle, rebuild the
 * tree to the matches. The selection is dropped by the rebuild —
 * pick the FIRST match so the pane shows something real. */
static void on_search_changed(fdk_widget *entry, void *user) {
    (void)user;
    ta_copy(napp.search, sizeof napp.search,
            fdk_entry_get_text(entry));
    tree_rebuild();
    if (napp.search[0] != '\0' && napp.note_count > 0) {
        for (size_t i = 0; i < napp.note_count; i++) {
            if (note_matches(&napp.notes[i], napp.search)) {
                nt_focus_select(i);
                break;
            }
        }
    }
}

void nt_ui_build(void) {
    ta_app *a = &napp.base;

    /* Round 23 (the fdk-edit lesson, applied here): the content
     * box is VERTICAL — two plain children stacked top-to-bottom
     * pushed the editor pane below the window. A HORIZONTAL box
     * lays [tree | pane] side by side and expands to fill.     */
    fdk_widget *row = NULL;
    (void)fdk_box_create(a->content, FDK_HORIZONTAL, &row);
    fdk_widget_set_expand(row, true, true);

    /* the left: the search box (round 25) + the notebook tree */
    fdk_widget *tree_host = NULL;
    (void)fdk_box_create(row, FDK_VERTICAL, &tree_host);
    fdk_widget_set_expand(tree_host, false, true);
    fdk_widget *se = NULL;
    (void)fdk_entry_create(tree_host, a->font_ui, "", &se);
    napp.search_entry = se;
    fdk_widget_set_expand(se, true, false);
    fdk_entry_set_placeholder(se, ta_("Search all notes"));
    fdk_entry_set_on_changed(se, on_search_changed, NULL);
    fdk_widget_set_accessible_name(se, ta_("Search all notes"));
    (void)fdk_tree_create(tree_host, a->font_ui, &napp.tree);
    fdk_widget_set_expand(napp.tree, false, true);
    fdk_widget_set_accessible_name(napp.tree,
                                   ta_("Notebooks and notes"));
    fdk_tree_set_on_selection_changed(napp.tree, on_tree_select,
                                      NULL);

    /* the right: title entry + body textview + focus label */
    fdk_widget *pane = NULL;
    (void)fdk_box_create(row, FDK_VERTICAL, &pane);
    fdk_widget_set_expand(pane, true, true);
    fdk_widget *te = NULL;
    (void)fdk_entry_create(pane, a->font_ui, "", &te);
    napp.title_entry = te;
    fdk_widget_set_expand(te, true, false);
    fdk_entry_set_placeholder(te, ta_("Note title"));
    fdk_entry_set_on_changed(te, on_title_changed, NULL);
    fdk_widget *fl = NULL;
    (void)fdk_label_create(pane, a->font_ui, ta_("Focus"), &fl);
    napp.focus_label = fl;
    fdk_widget *bv = NULL;
    (void)fdk_textview_create(pane, a->font_ui, &bv);
    napp.body_view = bv;
    fdk_widget_set_expand(bv, true, true);
    fdk_textview_set_on_changed(bv, on_body_changed, NULL);
    fdk_widget_set_accessible_name(bv, ta_("Note body"));

    /* the reminder sweep, once a minute */
    napp.reminder_timer = fdk_timer_add(a->ctx, 60000, true,
                                        on_reminder_tick, NULL);
}

void nt_tree_refresh(void) {
    tree_rebuild();
}
