/* 07_advanced.c — the Phase 9 advanced widget set, live.
 *
 * Every Phase 9 control in one window:
 *   - a MenuBar: File (Open / separator / Toolbar check / Quit),
 *     Edit (Copy, Cut, Paste / theme radios / a Recent SUBMENU),
 *     Help (About — a real message dialog)
 *   - a Toolbar of flat action buttons
 *   - a Notebook with pages of controls:
 *       Controls: a ComboBox + an editable ComboBox + Slider +
 *                 SpinButton
 *       Data:     a List and a Tree (on ScrollViews)
 *       Canvas:   a drawing Canvas (sine + grid, damage-driven)
 *   - a status Label narrating everything
 *
 * Everything the menus and combos pop up is TOOLKIT-OWNED: the app
 * loop only pumps events and paints ITS window — the popups
 * auto-paint, grab, and dismiss themselves (try clicking a menu
 * title, then Escape; try the submenu; open the About dialog and
 * watch modality eat your clicks on X11).
 *
 * For the X11 test rig the demo prints:
 *   RIG: file-title <x> <y>       — where to click for the File menu
 *   RIG: edit-title <x> <y>       — for the Edit menu (submenu)
 *   RIG: combo <x> <y> <w> <h>    — the combo field
 *   RIG: dialog-btn <x> <y> <w> <h>
 *   RIG: bar-height <h>           — menu row height (popup geometry)
 *   PHASE: item <name>            — every menu item activation
 *   PHASE: combo <index>          — combo selection changes
 *   PHASE: dialog <response>      — dialog answers
 *   PHASE: quit                   — the Quit item / close request
 *
 * Escape or the close request ends it (the Quit menu item too).
 * Fonts come from fdk_font_load_system_default(). Set
 * FDK_DEMO_FRAMES=N to exit after N pump iterations instead (the
 * automation knob the test rigs use).
 *
 * INIT-tier helper user (see example_window.h): the demo owns its
 * manual-bounds layout (popups, menus, dialogs live in window
 * coordinates). The helper still provides the uniform app_id
 * (org.fdk.example07).
 */

#include "example_window.h"
#include "fdk/fdk_dialog.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    fdk_context *ctx;
    fdk_window *window;
    bool quit;
} app;

static fdk_font *font15 = NULL;
static fdk_widget *status = NULL;
static fdk_widget *combo = NULL;
static fdk_widget *combo_editable = NULL;
static fdk_menu *file_menu = NULL;
static fdk_menu *edit_menu = NULL;
static fdk_menu *help_menu = NULL;
static fdk_menu *recent_menu = NULL;

static void set_status(const char *text) {
    if (status != NULL) {
        (void)fdk_label_set_text(status, text);
    }
}

/* ---- menu item callbacks (each narrates + prints a rig marker) ---- */

static void item_cb(fdk_menu_item *item, void *user) {
    (void)user;
    const char *name = fdk_menu_item_text(item);
    printf("PHASE: item %s\n", name != NULL ? name : "?");
    fflush(stdout);
    char buf[128];
    snprintf(buf, sizeof buf, "Menu: %s",
             name != NULL ? name : "?");
    set_status(buf);
}

static void quit_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (void)user;
    printf("PHASE: quit\n");
    fflush(stdout);
    app.quit = true;
}

static void toolbar_check_cb(fdk_menu_item *item, void *user) {
    (void)user;
    item_cb(item, NULL);
    set_status(fdk_menu_item_is_checked(item)
                   ? "Toolbar check: ON"
                   : "Toolbar check: OFF");
}

static void about_dialog_cb(fdk_menu_item *item, void *user) {
    (void)item;
    (void)user;
    item_cb(item, NULL);
    fdk_dialog_options opts = {
        .title = "About FDK",
        .text = "Faded Dream ToolKit - Phase 9 advanced widgets "
                "demo.\n\nMenus, popups, dialogs: all toolkit-owned, "
                "all auto-painted.",
        .buttons = FDK_DIALOG_BUTTONS_OK,
    };
    (void)fdk_dialog_show_message(app.ctx, &opts, NULL, NULL, NULL);
}

/* ---- dialog responses ---- */

static void dialog_response(fdk_dialog_response response, void *user) {
    (void)user;
    printf("PHASE: dialog %d\n", (int)response);
    fflush(stdout);
    set_status(response == FDK_DIALOG_OK
                   ? "Dialog: OK (and the modal grab is gone)"
                   : "Dialog: cancelled");
}

static void show_dialog_clicked(fdk_widget *button, void *user) {
    (void)button;
    (void)user;
    fdk_dialog_options opts = {
        .title = "A modal question",
        .text = "Modality: while this is up on X11, nothing else in "
                "the process takes input. Press Enter for OK, "
                "Escape to cancel, or click a button.",
        .buttons = FDK_DIALOG_BUTTONS_OK_CANCEL,
        .modal = true,
    };
    (void)fdk_dialog_show_message(app.ctx, &opts, dialog_response,
                                  NULL, NULL);
}

/* ---- combo changes ---- */

static void combo_changed(fdk_widget *w, size_t index, void *user) {
    (void)user;
    printf("PHASE: combo %zd\n", (ssize_t)index);
    fflush(stdout);
    char buf[128];
    if (index == FDK_COMBO_NONE) {
        snprintf(buf, sizeof buf, "Combo: custom text '%s'",
                 fdk_combo_active_text(w));
    } else {
        snprintf(buf, sizeof buf, "Combo: '%s'",
                 fdk_combo_active_text(w));
    }
    set_status(buf);
}

/* ---- the canvas page ---- */

static void canvas_paint(fdk_widget *canvas, fdk_surface *surface,
                         fdk_rect bounds, fdk_rect clip, void *user) {
    (void)canvas;
    (void)clip;
    (void)user;
    /* A sine over the bounds + a hairline grid, in theme colors. */
    fdk_color grid = fdk_theme_get_color(NULL, FDK_TK_TRACK);
    for (int x = bounds.x; x < bounds.x + bounds.width; x += 24) {
        fdk_rect col = {x, bounds.y, 1, bounds.height};
        fdk_surface_fill_rect(surface, col, grid);
    }
    for (int y = bounds.y; y < bounds.y + bounds.height; y += 24) {
        fdk_rect row = {bounds.x, y, bounds.width, 1};
        fdk_surface_fill_rect(surface, row, grid);
    }
    fdk_color accent = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
    fdk_i32 mid = bounds.y + bounds.height / 2;
    fdk_i32 half = bounds.height / 2 - 8;
    for (int x = bounds.x + 1; x < bounds.x + bounds.width; x++) {
        double t = (double)(x - bounds.x) / (double)bounds.width;
        fdk_i32 y = mid -
            (fdk_i32)(sin(t * 6.28318 * 2.0) * (double)half);
        fdk_rect dot = {x, y, 1, 1};
        fdk_surface_fill_rect(surface, dot, accent);
    }
}

/* ---- window events ---- */

static void toolbar_action(fdk_widget *button, void *user) {
    (void)button;
    const char *name = user;
    printf("PHASE: item %s\n", name != NULL ? name : "?");
    fflush(stdout);
    char buf[128];
    snprintf(buf, sizeof buf, "Toolbar: %s", name != NULL ? name : "?");
    set_status(buf);
}

static fdk_widget *g_bar = NULL;
static fdk_widget *g_toolbar = NULL;
static fdk_widget *g_notebook = NULL;

static void layout_all(fdk_i32 w, fdk_i32 h) {
    if (g_bar == NULL || g_toolbar == NULL || g_notebook == NULL ||
        status == NULL) {
        return;
    }
    fdk_i32 bar_h = 26;
    fdk_i32 tool_h = 38;
    fdk_i32 status_h = 26;
    fdk_widget_arrange(g_bar, (fdk_rect){0, 0, w, bar_h});
    fdk_widget_arrange(g_toolbar, (fdk_rect){0, bar_h, w, tool_h});
    fdk_i32 nb_h = h - bar_h - tool_h - status_h;
    if (nb_h < 40) {
        nb_h = 40;
    }
    fdk_widget_arrange(g_notebook,
                       (fdk_rect){0, bar_h + tool_h, w, nb_h});
    fdk_widget_arrange(status, (fdk_rect){8, h - status_h + 2,
                                          w - 16, status_h - 4});
}

static void window_event(fdk_window *window, const fdk_event_data *ev,
                         void *user) {
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        if (window == app.window) {
            app.quit = true;
        }
    } else if (ev->type == FDK_EVENT_WINDOW_CONFIGURE &&
               window == app.window) {
        layout_all(ev->configure.size.width,
                   ev->configure.size.height);
    }
}

/* ---- menus ---- */

static void build_menus(fdk_widget *parent) {
    assert(fdk_ok(fdk_menu_create(font15, &file_menu)));
    assert(fdk_ok(fdk_menu_create(font15, &edit_menu)));
    assert(fdk_ok(fdk_menu_create(font15, &help_menu)));
    assert(fdk_ok(fdk_menu_create(font15, &recent_menu)));

    fdk_menu_item *it = NULL;
    assert(fdk_ok(fdk_menu_append(file_menu, "Open", &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    fdk_menu_item_set_shortcut(it, "Ctrl+O");
    assert(fdk_ok(fdk_menu_append_separator(file_menu)));
    assert(fdk_ok(fdk_menu_append_check(file_menu, "Toolbar", true,
                                        &it)));
    fdk_menu_item_set_on_activate(it, toolbar_check_cb, NULL);
    assert(fdk_ok(fdk_menu_append_separator(file_menu)));
    assert(fdk_ok(fdk_menu_append(file_menu, "Quit", &it)));
    fdk_menu_item_set_on_activate(it, quit_cb, NULL);
    fdk_menu_item_set_shortcut(it, "Ctrl+Q");

    assert(fdk_ok(fdk_menu_append(edit_menu, "Copy", &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    fdk_menu_item_set_shortcut(it, "Ctrl+C");
    assert(fdk_ok(fdk_menu_append(edit_menu, "Cut", &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    assert(fdk_ok(fdk_menu_append(edit_menu, "Paste", &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    fdk_menu_item_set_shortcut(it, "Ctrl+V");
    assert(fdk_ok(fdk_menu_append_separator(edit_menu)));
    assert(fdk_ok(fdk_menu_append_radio(edit_menu, "Light theme",
                                        false, &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    assert(fdk_ok(fdk_menu_append_radio(edit_menu, "Dark theme",
                                        true, &it)));
    fdk_menu_item_set_on_activate(it, item_cb, NULL);
    assert(fdk_ok(fdk_menu_append_separator(edit_menu)));
    assert(fdk_ok(fdk_menu_append(edit_menu, "Recent", &it)));
    assert(fdk_ok(fdk_menu_item_set_submenu(it, recent_menu)));
    fdk_menu_item *rec = NULL;
    assert(fdk_ok(fdk_menu_append(recent_menu, "phase9.txt", &rec)));
    fdk_menu_item_set_on_activate(rec, item_cb, NULL);
    assert(fdk_ok(fdk_menu_append(recent_menu, "roadmap.md", &rec)));
    fdk_menu_item_set_on_activate(rec, item_cb, NULL);

    assert(fdk_ok(fdk_menu_append(help_menu, "About", &it)));
    fdk_menu_item_set_on_activate(it, about_dialog_cb, NULL);

    fdk_widget *bar = NULL;
    assert(fdk_ok(fdk_menu_bar_create(parent, font15, &bar)));
    g_bar = bar;
    assert(fdk_ok(fdk_menu_bar_append(bar, "File", file_menu)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "Edit", edit_menu)));
    assert(fdk_ok(fdk_menu_bar_append(bar, "Help", help_menu)));

    /* Title centers for the rig (the bar lays titles from x=6, each
     * padded 10px). */
    printf("RIG: file-title %d %d\n", 6 + (10 + 12), 13);
    printf("RIG: edit-title %d %d\n", 6 + 48 + (10 + 12), 13);
    printf("RIG: bar-height %d\n", 26);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!fdk_example_init(&app.ctx, "07")) {
        return 1;
    }

    font15 = fdk_font_load_system_default(15);
    /* Fontless systems still work: indicators, checks, radios,
     * arrows, and every vector glyph render; text is absent. */

    fdk_window_options wopts = {
        .title = "FDK advanced widgets",
        .width = 720,
        .height = 520,
    };
    fdk_result r = fdk_window_create(app.ctx, &wopts, &app.window);
    if (!fdk_ok(r)) {
        fprintf(stderr, "fdk_window_create failed: %d\n", r);
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);
    fdk_widget *root = NULL;
    assert(fdk_ok(fdk_window_get_root(app.window, &root)));

    /* The vertical layout: menubar (fixed height), toolbar (fixed),
     * notebook (expands), status (fixed). Simple manual bounds —
     * the point is the widget zoo, not layout. */
    build_menus(root);

    fdk_widget *toolbar = NULL;
    assert(fdk_ok(fdk_toolbar_create(root, font15, &toolbar)));
    g_toolbar = toolbar;
    fdk_widget *new_btn = NULL;
    assert(fdk_ok(fdk_toolbar_add_button(toolbar, "New", toolbar_action,
                                         (void *)"New", &new_btn)));
    (void)new_btn;
    assert(fdk_ok(fdk_toolbar_add_button(toolbar, "Save", toolbar_action,
                                         (void *)"Save", NULL)));
    assert(fdk_ok(fdk_toolbar_add_separator(toolbar)));
    assert(fdk_ok(fdk_toolbar_add_button(toolbar, "Print", toolbar_action,
                                         (void *)"Print", NULL)));

    fdk_widget *notebook = NULL;
    assert(fdk_ok(fdk_notebook_create(root, font15, &notebook)));
    g_notebook = notebook;

    /* Page 1: controls (a plain container with manual layout). */
    fdk_widget *page1 = NULL;
    assert(fdk_ok(fdk_widget_create(notebook, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &page1)));
    (void)fdk_notebook_append_page(notebook, page1, "Controls");
    fdk_widget *lbl = NULL;
    assert(fdk_ok(fdk_label_create(page1, font15,
                                   "Combo (pick one):", &lbl)));
    fdk_widget_arrange(lbl, (fdk_rect){24, 18, 140, 24});
    combo = NULL;
    assert(fdk_ok(fdk_combo_create(page1, font15, &combo)));
    assert(fdk_ok(fdk_combo_append(combo, "Red", NULL)));
    assert(fdk_ok(fdk_combo_append(combo, "Green", NULL)));
    assert(fdk_ok(fdk_combo_append(combo, "Blue", NULL)));
    assert(fdk_ok(fdk_combo_append(combo, "Black", NULL)));
    assert(fdk_ok(fdk_combo_set_active(combo, 1)));
    fdk_combo_set_on_changed(combo, combo_changed, NULL);
    fdk_widget_arrange(combo, (fdk_rect){170, 14, 180, 30});

    lbl = NULL;
    assert(fdk_ok(fdk_label_create(page1, font15,
                                   "Combo (editable):", &lbl)));
    fdk_widget_arrange(lbl, (fdk_rect){24, 58, 140, 24});
    combo_editable = NULL;
    assert(fdk_ok(fdk_combo_create(page1, font15, &combo_editable)));
    assert(fdk_ok(fdk_combo_append(combo_editable, "small", NULL)));
    assert(fdk_ok(fdk_combo_append(combo_editable, "medium", NULL)));
    assert(fdk_ok(fdk_combo_append(combo_editable, "large", NULL)));
    assert(fdk_ok(fdk_combo_set_active(combo_editable, 0)));
    fdk_combo_set_on_changed(combo_editable, combo_changed, NULL);
    fdk_combo_set_editable(combo_editable, true);
    fdk_widget_arrange(combo_editable, (fdk_rect){170, 54, 180, 30});

    lbl = NULL;
    assert(fdk_ok(fdk_label_create(page1, font15, "Slider:", &lbl)));
    fdk_widget_arrange(lbl, (fdk_rect){24, 98, 140, 24});
    fdk_widget *slider = NULL;
    assert(fdk_ok(fdk_slider_create(page1, 0.0, 100.0, 40.0, &slider)));
    fdk_widget_arrange(slider, (fdk_rect){170, 96, 180, 28});

    lbl = NULL;
    assert(fdk_ok(fdk_label_create(page1, font15, "Spin:", &lbl)));
    fdk_widget_arrange(lbl, (fdk_rect){24, 138, 140, 24});
    fdk_widget *spin = NULL;
    assert(fdk_ok(fdk_spin_create(page1, font15, 0.0, 100.0, 50.0,
                                  &spin)));
    fdk_widget_arrange(spin, (fdk_rect){170, 134, 180, 30});

    fdk_widget *dlg_btn = NULL;
    assert(fdk_ok(fdk_button_create(page1, font15,
                                     "Show modal dialog", &dlg_btn)));
    fdk_button_set_on_activate(dlg_btn, show_dialog_clicked, NULL);
    fdk_widget_arrange(dlg_btn, (fdk_rect){24, 190, 180, 34});

    /* Page 2: list + tree. */
    fdk_widget *page2 = NULL;
    assert(fdk_ok(fdk_widget_create(notebook, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &page2)));
    (void)fdk_notebook_append_page(notebook, page2, "Data");
    fdk_widget *list = NULL;
    assert(fdk_ok(fdk_list_create(page2, font15, &list)));
    for (int i = 0; i < 40; i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "Row %d", i);
        (void)fdk_list_append(list, buf, NULL);
    }
    fdk_widget_arrange(list, (fdk_rect){16, 12, 300, 300});

    fdk_widget *tree = NULL;
    assert(fdk_ok(fdk_tree_create(page2, font15, &tree)));
    fdk_tree_node src = FDK_TREE_NODE_NONE;
    fdk_tree_node include = FDK_TREE_NODE_NONE;
    assert(fdk_ok(fdk_tree_node_add(tree, src, "src", &src)));
    assert(fdk_ok(fdk_tree_node_add(tree, src, "widget.c", NULL)));
    assert(fdk_ok(fdk_tree_node_add(tree, src, "window.c", NULL)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "include", &include)));
    assert(fdk_ok(fdk_tree_node_add(tree, include, "fdk.h", NULL)));
    assert(fdk_ok(fdk_tree_node_add(tree, FDK_TREE_NODE_NONE,
                                    "tests", NULL)));
    fdk_widget_arrange(tree, (fdk_rect){330, 12, 300, 300});

    /* Page 3: canvas. */
    fdk_widget *page3 = NULL;
    assert(fdk_ok(fdk_widget_create(notebook, NULL,
                                    (fdk_rect){0, 0, 10, 10}, &page3)));
    (void)fdk_notebook_append_page(notebook, page3, "Canvas");
    fdk_widget *canvas = NULL;
    assert(fdk_ok(fdk_canvas_create(page3, canvas_paint, NULL,
                                    &canvas)));
    fdk_widget_arrange(canvas, (fdk_rect){16, 12, 620, 300});

    /* The status line. */
    status = NULL;
    assert(fdk_ok(fdk_label_create(root, font15, "Ready.", &status)));

    /* The top-level bands (re-run on every configure above). */
    layout_all(720, 520);

    /* Rig markers for the clickable chrome — WINDOW-ABSOLUTE (the
     * pages live inside the notebook's page area, so parent-relative
     * bounds would be useless to a driver). */
    fdk_rect cb = fdk_widget_get_absolute_bounds(combo);
    printf("RIG: combo %d %d %d %d\n", cb.x, cb.y, cb.width,
           cb.height);
    fdk_rect db = fdk_widget_get_absolute_bounds(dlg_btn);
    printf("RIG: dialog-btn %d %d %d %d\n", db.x, db.y, db.width,
           db.height);
    fflush(stdout);

    fdk_window_show(app.window);
    fdk_window_paint(app.window);

    /* The loop: pump + (damage-gated) paint THIS window; the
     * menu/combo popups and dialogs paint themselves. An idle app
     * presents nothing at all. */
    const char *limit_s = getenv("FDK_DEMO_FRAMES");
    const int frame_limit = (limit_s != NULL) ? atoi(limit_s) : 0;
    int frames = 0;
    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
        if (fdk_widget_tree_has_damage(root)) {
            fdk_window_paint(app.window);
        }
        frames++;
        if (frame_limit > 0 && frames >= frame_limit) {
            break;
        }
    }

    printf("PHASE: exit\n");
    fflush(stdout);
    fdk_menu_destroy(file_menu);
    fdk_menu_destroy(edit_menu);
    fdk_menu_destroy(help_menu);
    fdk_menu_destroy(recent_menu);
    fdk_window_destroy(app.window);
    if (font15 != NULL) {
        fdk_font_destroy(font15);
    }
    fdk_shutdown(app.ctx);
    return 0;
}
