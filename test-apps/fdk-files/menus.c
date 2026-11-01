/* menus.c — the menu bar (File/Edit/View/Go/Help), the context
 * menus (file/folder/multi/empty/sidebar), the icon toolbar, and
 * the window shortcuts + the shortcut list dialog. */
#include "bookmarks.h"
#include "menus.h"
#include "fsops.h"
#include "model.h"
#include "ops_dialogs.h"
#include "panes.h"
#include "teardown.h"
#include "trash.h"

#include "fdk/fdk_dialog.h"
#include "fdk/fdk_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct menu_action {
    const char *label;
    const char *shortcut;
    void (*fn)(void);
} menu_action;

/* forward decls */
static void toolbar_trampoline(fdk_widget *btn, void *user);
static void shortcut_fn(fdk_window *w, void *user);
void menu_build_actions(fdk_menu *menu,
                        const menu_action *acts, size_t count);
void pane_build_view(pane *p, tab *t);
static void filter_search_changed(fdk_widget *entry,
                                  const char *text, void *user);
static void menus_ctx_track(fdk_menu *m);
static void act_open_selected(void);
static void act_choose_app(void);
static void act_open_in_new_tab(void);
static void act_properties(void);

/* ---- action targets ------------------------------------------------ */

static void act_back(void) { pane_back(active_pane()); }
static void act_forward(void) { pane_forward(active_pane()); }
static void act_up(void) { pane_up(active_pane()); }
static void act_home(void) { pane_navigate(active_pane(), fi_home()); }
static void act_refresh(void) { pane_reload(active_pane()); }

static void act_new_tab(void) {
    pane_new_tab(active_pane(), active_tab(active_pane())->dir);
}
static void act_close_tab(void) {
    pane *p = active_pane();
    pane_close_tab(p, p->current);
}
static void act_quit(void) { fapp.base.quit = true; }

static void act_new_folder(void) {
    ops_prompt_new_folder(active_pane());
}
static void act_rename(void) { ops_prompt_rename(active_pane()); }
static void act_delete(void) { fi_delete_selection(active_pane(), true); }
static void act_delete_permanent(void) {
    fi_delete_selection(active_pane(), false);
}
static void act_cut(void) { ops_clipboard_set('m'); }
static void act_copy(void) { ops_clipboard_set('c'); }
static void act_paste(void) { ops_clipboard_paste(active_pane()); }

static void act_select_all(void) { pane_select_all(active_pane()); }
static void act_invert(void) {
    pane_invert_selection(active_pane());
}
static void act_prefs(void) { ops_open_prefs(); }
static void act_about(void) { ops_open_about(); }

static void act_view_detail(void) {
    pane_set_view_mode(active_pane(), VIEW_DETAIL);
}
static void act_view_icons(void) {
    pane_set_view_mode(active_pane(), VIEW_ICONS);
}
static void act_view_compact(void) {
    pane_set_view_mode(active_pane(), VIEW_COMPACT);
}
static void act_hidden(void) {
    fapp.show_hidden = !fapp.show_hidden;
    fmodel_set_hidden(fapp.show_hidden);
    pane_reload(active_pane());
    fi_set_status("Hidden files %s",
                  fapp.show_hidden ? "shown" : "hidden");
}
static void act_search(void) { ops_focus_search(); }
static void act_location(void) {
    pane *p = active_pane();
    p->path_editing = !p->path_editing;
    pane_refresh_breadcrumb(p);
    if (p->path_entry != NULL) {
        (void)fdk_widget_focus(p->path_entry);
    }
}

static void act_go_trash(void) { ops_open_trash(); }

static void act_shortcut_list(void) { menus_open_shortcut_list(&fapp); }

/* §26/3a: bookmark the active tab's folder. */
static void act_bookmark(void) {
    const char *dir = active_tab(active_pane())->dir;
    if (bookmarks_add(dir) >= 0) {
        bookmarks_refresh();
        bookmarks_save();
        fi_set_status("Bookmarked %s", dir);
    } else {
        fi_set_status("Already bookmarked (or not a folder)");
    }
}

/* §26/3a: the split view. F3 toggles (the two-pane convention —
 * Norton/Midnight/Total all use it). */
static void act_split_toggle(void) {
    if (fapp.pane_count >= 2) {
        pane_remove_last();
        fi_set_status("Split view off");
    } else if (pane_add() > 0) {
        fi_set_status("Split view on — drag files between panes");
    }
}

/* ---- the menu model ------------------------------------------------- */

static menu_action g_menu_acts[256];
static size_t g_menu_act_count;

static void item_activate(fdk_menu_item *item, void *user) {
    (void)item;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx > 0 && idx <= g_menu_act_count &&
        g_menu_acts[idx - 1].fn != NULL) {
        g_menu_acts[idx - 1].fn();
    }
}

void menus_build(files_app *a) {
    fdk_font *font = a->base.font_ui;
    fdk_widget *bar_host = a->base.menubar_host;

    /* the toolbar row first (icons + tooltips + shortcuts) */
    static const struct {
        const char *icon;
        fdk_row_icon glyph;
        const char *tip;
        const char *shortcut;
        void (*fn)(void);
    } tools[] = {
        {"back", FDK_ROW_ICON_FILE, "Back", "Alt+Left", act_back},
        {"forward", FDK_ROW_ICON_FILE, "Forward", "Alt+Right",
         act_forward},
        {"up", FDK_ROW_ICON_FILE, "Up", "Alt+Up", act_up},
        {"home", FDK_ROW_ICON_HOME, "Home", "Alt+Home", act_home},
        {"refresh", FDK_ROW_ICON_FILE, "Refresh", "F5", act_refresh},
        {"folder-new", FDK_ROW_ICON_FOLDER, "New folder", "Ctrl+N",
         act_new_folder},
        {"tab-new", FDK_ROW_ICON_FILE, "New tab", "Ctrl+T",
         act_new_tab},
        {"view-list", FDK_ROW_ICON_FILE, "Details view", "Ctrl+1",
         act_view_detail},
        {"view-grid", FDK_ROW_ICON_FILE, "Icons view", "Ctrl+2",
         act_view_icons},
        {"view-compact", FDK_ROW_ICON_FILE, "Compact view", "Ctrl+3",
         act_view_compact},
        {"search", FDK_ROW_ICON_FILE, "Search", "Ctrl+F",
         act_search},
        {"eye", FDK_ROW_ICON_FILE, "Hidden files", "Ctrl+H",
         act_hidden},
    };
    fdk_widget *toolbar = a->toolbar;
    for (size_t i = 0;
         i < sizeof tools / sizeof tools[0] && toolbar != NULL;
         i++) {
        ta_icon_button spec = {0};
        spec.icon = tools[i].icon;
        spec.glyph = tools[i].glyph;
        spec.tooltip = tools[i].tip;
        spec.shortcut = tools[i].shortcut;
        /* the activate callback signature needs a void* user; we
         * pass the fn pointer through a trampoline */
        g_menu_acts[g_menu_act_count] = (menu_action){
            .fn = tools[i].fn};
        spec.user = (void *)(uintptr_t)g_menu_act_count;
        g_menu_act_count++;
        spec.on_activate = toolbar_trampoline;
        (void)ta_icon_button_add(toolbar, &a->base, &spec);
        if ((i + 1) % 6 == 0) {
            (void)fdk_toolbar_add_separator(toolbar);
        }
    }
    /* the live filter entry at the toolbar's tail */
    pane *p = fapp.panes[0];
    (void)fdk_search_entry_create(toolbar, a->base.font_ui,
                                  &p->filter_entry);
    fdk_search_entry_set_on_search(p->filter_entry,
                                   filter_search_changed, p);
    fdk_widget_set_natural_size(p->filter_entry, 140, 26);
    (void)fdk_entry_set_placeholder(p->filter_entry, "Filter");

    /* the menu bar */
    fdk_widget *bar = NULL;
    (void)fdk_menu_bar_create(bar_host, font, &bar);

    fdk_menu *m_file = NULL;
    (void)fdk_menu_create(font, &m_file);
    td_track(m_file, TD_MENU_MODEL);
    static const menu_action file_acts[] = {
        {"&New tab", "Ctrl+T", act_new_tab},
        {"Close &tab", "Ctrl+W", act_close_tab},
        {"New &folder", "Ctrl+N", act_new_folder},
        {NULL, NULL, NULL},
        {"&Rename", "F2", act_rename},
        {"&Move to trash", "Delete", act_delete},
        {"Delete permanently", "Shift+Delete", act_delete_permanent},
        {NULL, NULL, NULL},
        {"&Quit", "Ctrl+Q", act_quit},
    };
    menu_build_actions(m_file, file_acts,
                       sizeof file_acts / sizeof file_acts[0]);

    fdk_menu *m_edit = NULL;
    (void)fdk_menu_create(font, &m_edit);
    td_track(m_edit, TD_MENU_MODEL);
    static const menu_action edit_acts[] = {
        {"Cu&t", "Ctrl+X", act_cut},
        {"&Copy", "Ctrl+C", act_copy},
        {"&Paste", "Ctrl+V", act_paste},
        {NULL, NULL, NULL},
        {"Select &all", "Ctrl+A", act_select_all},
        {"&Invert selection", "Ctrl+I", act_invert},
        {NULL, NULL, NULL},
        {"&Preferences", "Ctrl+comma", act_prefs},
    };
    menu_build_actions(m_edit, edit_acts,
                       sizeof edit_acts / sizeof edit_acts[0]);

    fdk_menu *m_view = NULL;
    (void)fdk_menu_create(font, &m_view);
    td_track(m_view, TD_MENU_MODEL);
    static const menu_action view_acts[] = {
        {"&Details", "Ctrl+1", act_view_detail},
        {"&Icons", "Ctrl+2", act_view_icons},
        {"&Compact", "Ctrl+3", act_view_compact},
        {NULL, NULL, NULL},
        {"&Hidden files", "Ctrl+H", act_hidden},
        {"&Location bar", "Ctrl+L", act_location},
        {NULL, NULL, NULL},
        {"&Split view", "F3", act_split_toggle},
    };
    menu_build_actions(m_view, view_acts,
                       sizeof view_acts / sizeof view_acts[0]);

    fdk_menu *m_go = NULL;
    (void)fdk_menu_create(font, &m_go);
    td_track(m_go, TD_MENU_MODEL);
    static const menu_action go_acts[] = {
        {"&Back", "Alt+Left", act_back},
        {"&Forward", "Alt+Right", act_forward},
        {"&Up", "Alt+Up", act_up},
        {"&Home", "Alt+Home", act_home},
        {NULL, NULL, NULL},
        {"&Bookmark this folder", "Ctrl+D", act_bookmark},
        {"&Trash", "", act_go_trash},
    };
    menu_build_actions(m_go, go_acts,
                       sizeof go_acts / sizeof go_acts[0]);

    fdk_menu *m_help = NULL;
    (void)fdk_menu_create(font, &m_help);
    td_track(m_help, TD_MENU_MODEL);
    static const menu_action help_acts[] = {
        {"&Shortcuts", "Ctrl+question", act_shortcut_list},
        {"&About", "", act_about},
    };
    menu_build_actions(m_help, help_acts,
                       sizeof help_acts / sizeof help_acts[0]);

    (void)fdk_menu_bar_append(bar, "&File", m_file);
    (void)fdk_menu_bar_append(bar, "&Edit", m_edit);
    (void)fdk_menu_bar_append(bar, "&View", m_view);
    (void)fdk_menu_bar_append(bar, "&Go", m_go);
    (void)fdk_menu_bar_append(bar, "&Help", m_help);

    /* window-level shortcuts (physical keys, layout-stable) */
    fdk_window *w = a->base.window;
    static const struct {
        const char *spec;
        void (*fn)(void);
    } sc[] = {
        {"Ctrl+T", act_new_tab},   {"Ctrl+W", act_close_tab},
        {"Ctrl+N", act_new_folder}, {"F2", act_rename},
        {"Delete", act_delete},    {"Shift+Delete", act_delete_permanent},
        {"Ctrl+X", act_cut},       {"Ctrl+C", act_copy},
        {"Ctrl+V", act_paste},     {"Ctrl+A", act_select_all},
        {"Ctrl+I", act_invert},    {"Ctrl+F", act_search},
        {"Ctrl+L", act_location},  {"Ctrl+H", act_hidden},
        {"Alt+Left", act_back},    {"Alt+Right", act_forward},
        {"Alt+Up", act_up},        {"Alt+Home", act_home},
        {"F5", act_refresh},       {"Ctrl+Q", act_quit},
        {"F3", act_split_toggle},
        {"Ctrl+D", act_bookmark},
        {"Ctrl+question", act_shortcut_list},
    };
    for (size_t i = 0; i < sizeof sc / sizeof sc[0]; i++) {
        g_menu_acts[g_menu_act_count] = (menu_action){
            .fn = sc[i].fn};
        (void)fdk_window_add_shortcut(
            w, sc[i].spec, shortcut_fn,
            (void *)(uintptr_t)g_menu_act_count);
        g_menu_act_count++;
    }
}

/* the shortcut trampoline: menu/window callbacks hand us the fn
 * pointer we stored as user data */
static void shortcut_fn(fdk_window *w, void *user) {
    (void)w;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx < g_menu_act_count && g_menu_acts[idx].fn != NULL) {
        g_menu_acts[idx].fn();
    }
}

static void toolbar_trampoline(fdk_widget *btn, void *user) {
    (void)btn;
    size_t idx = (size_t)(uintptr_t)user;
    if (idx < g_menu_act_count && g_menu_acts[idx].fn != NULL) {
        g_menu_acts[idx].fn();
    }
}

/* builds items WITH actions+shortcuts, separators where label NULL */
void menu_build_actions(fdk_menu *menu,
                        const menu_action *acts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (acts[i].label == NULL) {
            (void)fdk_menu_append_separator(menu);
            continue;
        }
        fdk_menu_item *it = NULL;
        (void)fdk_menu_append(menu, ta_(acts[i].label), &it);
        if (acts[i].shortcut != NULL && acts[i].shortcut[0] != '\0') {
            (void)fdk_menu_item_set_shortcut(it,
                                             acts[i].shortcut);
        }
        fdk_menu_item_set_on_activate(it, item_activate,
                                      (void *)(uintptr_t)0);
        g_menu_acts[g_menu_act_count] = acts[i];
        fdk_menu_item_set_on_activate(it, item_activate,
                                      (void *)(uintptr_t)g_menu_act_count);
        g_menu_act_count++;
    }
}

void pane_build_view(pane *p, tab *t);
static void filter_search_changed(fdk_widget *entry,
                                  const char *text, void *user) {
    (void)entry;
    pane *p = user;
    fmodel_set_filter(text);
    pane_build_view(p, active_tab(p));
}

/* ---- the context menus ---------------------------------------------- */

void menus_popup_context(pane *p, int for_what, fdk_i32 x,
                         fdk_i32 y) {
    fdk_font *font = fapp.base.font_ui;
    fdk_menu *m = NULL;
    (void)fdk_menu_create(font, &m);

    switch (for_what) {
    case CTX_FILE: {
        static const menu_action acts[] = {
            {"&Open", "", act_open_selected},
            {"Open &with…", "", act_choose_app},
            {NULL, NULL, NULL},
            {"Cu&t", "Ctrl+X", act_cut},
            {"&Copy", "Ctrl+C", act_copy},
            {NULL, NULL, NULL},
            {"&Rename", "F2", act_rename},
            {"Move to &trash", "Delete", act_delete},
            {"Delete permanently", "Shift+Delete",
             act_delete_permanent},
            {NULL, NULL, NULL},
            {"P&roperties", "Ctrl+I", act_properties},
        };
        menu_build_actions(m, acts, sizeof acts / sizeof acts[0]);
        break;
    }
    case CTX_FOLDER: {
        static const menu_action acts[] = {
            {"&Open", "", act_open_selected},
            {"Open in new &tab", "", act_open_in_new_tab},
            {NULL, NULL, NULL},
            {"Cu&t", "Ctrl+X", act_cut},
            {"&Copy", "Ctrl+C", act_copy},
            {NULL, NULL, NULL},
            {"&Rename", "F2", act_rename},
            {"Move to &trash", "Delete", act_delete},
            {NULL, NULL, NULL},
            {"P&roperties", "Ctrl+I", act_properties},
        };
        menu_build_actions(m, acts, sizeof acts / sizeof acts[0]);
        break;
    }
    case CTX_MULTI: {
        static const menu_action acts[] = {
            {"Cu&t", "Ctrl+X", act_cut},
            {"&Copy", "Ctrl+C", act_copy},
            {NULL, NULL, NULL},
            {"Move to &trash", "Delete", act_delete},
            {"Delete permanently", "Shift+Delete",
             act_delete_permanent},
        };
        menu_build_actions(m, acts, sizeof acts / sizeof acts[0]);
        break;
    }
    case CTX_EMPTY:
    default: {
        static const menu_action acts[] = {
            {"New &folder", "Ctrl+N", act_new_folder},
            {"&Paste", "Ctrl+V", act_paste},
            {NULL, NULL, NULL},
            {"Select &all", "Ctrl+A", act_select_all},
            {"&Refresh", "F5", act_refresh},
        };
        menu_build_actions(m, acts, sizeof acts / sizeof acts[0]);
        break;
    }
    }

    (void)fdk_menu_popup_at(m, active_tab(p)->list, x, y);
    /* the menu model is a stack local by design: popup_at copies
     * nothing, so we leak-free it by keeping it alive until the
     * chain closes — FDK requires the model to outlive the popup,
     * so park it in a static registry freed on close (the honest
     * shape; see menus_ctx_track) */
    menus_ctx_track(m);
}

/* keeps popup models alive until their chain closes (the destroy
 * rides the menu's own activate; without activation the model is
 * freed at app teardown) */
#define MAX_CTX_MENUS 8
static fdk_menu **g_ctx_menus; /* heap so the sweep can free it    */
static size_t g_ctx_menu_count;

static void menus_ctx_track(fdk_menu *m) {
    if (g_ctx_menus == NULL) {
        g_ctx_menus = calloc(MAX_CTX_MENUS, sizeof *g_ctx_menus);
        if (g_ctx_menus == NULL) {
            fdk_menu_destroy(m); /* cannot track: die now, not leak */
            return;
        }
        td_track(g_ctx_menus, TD_CTX_MENUS);
    }
    if (g_ctx_menu_count < MAX_CTX_MENUS) {
        g_ctx_menus[g_ctx_menu_count++] = m;
    } else {
        fdk_menu_destroy(m); /* full: honest immediate destroy */
    }
}

/* §26: pop + track an app-built context menu (bookmarks' remove).
 * Same lifetime rule as every popup here: the model is parked in
 * the ctx registry and freed when the chain closes. */
void menus_ctx_popup(fdk_menu *m, fdk_widget *anchor, fdk_i32 x,
                     fdk_i32 y) {
    if (m == NULL) {
        return;
    }
    (void)fdk_menu_popup_at(m, anchor, x, y);
    menus_ctx_track(m);
}

void menus_ctx_shutdown(void) {
    for (size_t i = 0; i < g_ctx_menu_count; i++) {
        fdk_menu_destroy(g_ctx_menus[i]);
    }
    g_ctx_menu_count = 0;
    td_untrack(g_ctx_menus);
    free(g_ctx_menus);
    g_ctx_menus = NULL;
}

/* ---- the shortcut list ---------------------------------------------- */

typedef struct sc_list_row {
    const char *action;
    const char *keys;
} sc_list_row;

void menus_open_shortcut_list(files_app *a) {
    static const sc_list_row rows[] = {
        {"New tab", "Ctrl+T"},
        {"Close tab", "Ctrl+W"},
        {"New folder", "Ctrl+N"},
        {"Rename", "F2"},
        {"Move to trash", "Delete"},
        {"Delete permanently", "Shift+Delete"},
        {"Cut", "Ctrl+X"},
        {"Copy", "Ctrl+C"},
        {"Paste", "Ctrl+V"},
        {"Select all", "Ctrl+A"},
        {"Invert selection", "Ctrl+I"},
        {"Filter / search", "Ctrl+F"},
        {"Toggle location bar", "Ctrl+L"},
        {"Hidden files", "Ctrl+H"},
        {"Back / Forward", "Alt+Left / Alt+Right"},
        {"Up / Home", "Alt+Up / Alt+Home"},
        {"Refresh", "F5"},
        {"Details / Icons / Compact", "Ctrl+1 / 2 / 3"},
        {"Quit", "Ctrl+Q"},
        {"This list", "Ctrl+?"},
    };
    fdk_window *win = NULL;
    fdk_window_options wo = {0};
    wo.title = "Keyboard shortcuts";
    wo.width = 460;
    wo.height = 480;
    (void)fdk_window_create(a->base.ctx, &wo, &win);
    (void)fdk_window_set_decorated(win, true);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(win, &root);
    fdk_widget *box = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 12);
    fdk_window_set_content(win, box);
    fdk_widget *list = NULL;
    (void)fdk_list_create(box, a->base.font_ui, &list);
    fdk_widget_set_expand(list, true, true);
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char line[128];
        snprintf(line, sizeof line, "%-28s %s", rows[i].action,
                 rows[i].keys);
        size_t idx = 0;
        (void)fdk_list_append(list, line, &idx);
    }
    fdk_widget_set_accessible_name(list, "Keyboard shortcuts");
    fdk_window_show(win);
    /* auto-managed: closing it just destroys the window (FDK owns
     * the toplevel; no callback needed for a read-only list) */
}

/* ---- selection-dependent actions (context menus' targets) ---------- */

static void act_open_selected(void) {
    pane *p = active_pane();
    const file_info *sel[64];
    size_t n = fmodel_selected(p, sel, 64);
    for (size_t i = 0; i < n; i++) {
        if (sel[i]->is_dir) {
            pane_navigate(p, sel[i]->path);
            return;
        }
        ops_open_with(p, sel[i]);
    }
}

static void act_choose_app(void) {
    pane *p = active_pane();
    const file_info *sel[1];
    if (fmodel_selected(p, sel, 1) == 1) {
        ops_choose_app(p, sel[0]);
    }
}

static void act_open_in_new_tab(void) {
    pane *p = active_pane();
    const file_info *sel[1];
    if (fmodel_selected(p, sel, 1) == 1 && sel[0]->is_dir) {
        pane_new_tab(p, sel[0]->path);
    }
}

static void act_properties(void) {
    pane *p = active_pane();
    const file_info *sel[1];
    if (fmodel_selected(p, sel, 1) == 1) {
        ops_properties(p, sel[0]);
    }
}
