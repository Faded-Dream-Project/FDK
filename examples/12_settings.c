/*
 * 12_settings.c — the app-furniture showcase (1.4.2)
 *
 * A settings dialog in the shape every desktop app ships: a search
 * field, a pill switcher over a named-page Stack, and a context-
 * scoped Statusbar reporting what the app is doing. Every 1.4.2
 * widget earns its keep here —
 *
 *   SearchEntry     the magnifier field (Esc clears, X empties)
 *   StackSwitcher   the pill row (accent on the active page)
 *   Stack           General / Appearance / Storage pages
 *   Statusbar       "hint" (persistent) + "action" (transient)
 *                   contexts, push/pop on every interaction
 *   Slider + marks  the font-size slider with labeled ticks
 *   LevelBar        the sync-level meter (discrete + continuous)
 *   Spinner         the "syncing…" busy arc
 *   Revealer        the Advanced details drawer (SLIDE_UP door)
 *   List + icons    the storage list, multi-select with the
 *                   rubber-band sweep (drag on the empty space)
 *   + the 1.4.1 furniture: frames, checkboxes, radios, link button
 *
 * Interactions to try: type in the search field (the statusbar's
 * "action" context echoes it; Esc clears), flip pages, drag the
 * slider, sweep-select storage rows, toggle "Advanced details".
 */

#include "example_window.h"

typedef struct settings_app {
    fdk_example *ex;
    fdk_widget *statusbar;
    fdk_u32 ctx_hint;
    fdk_u32 ctx_action;
    fdk_widget *font_value;
    fdk_widget *sync_level;
    fdk_widget *sync_level_cont;
    fdk_widget *sync_spinner;
    fdk_widget *revealer;
    fdk_widget *storage_list;
} settings_app;

/* ---- statusbar helpers ---- */

static void push_action(settings_app *app, const char *text) {
    /* The action context replaces its own last message (pop + push
     * keeps the stack depth flat while hint messages survive
     * underneath). */
    fdk_statusbar_pop(app->statusbar, app->ctx_action);
    (void)fdk_statusbar_push(app->statusbar, app->ctx_action, text);
}

static void push_hint(settings_app *app, const char *text) {
    fdk_statusbar_pop(app->statusbar, app->ctx_hint);
    (void)fdk_statusbar_push(app->statusbar, app->ctx_hint, text);
}

/* ---- callbacks ---- */

static void on_search_changed(fdk_widget *entry, void *user) {
    settings_app *app = user;
    const char *text = fdk_entry_get_text(entry);
    if (text[0] == '\0') {
        push_action(app, "Search cleared");
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), "Searching for \"%s\"", text);
        push_action(app, buf);
    }
}

static void on_page_changed(fdk_widget *stack, size_t index,
                            const char *name, void *user) {
    settings_app *app = user;
    char buf[96];
    snprintf(buf, sizeof(buf), "Viewing: %s", name);
    push_hint(app, buf);
    push_action(app, buf);
    (void)stack;
    (void)index;
}

static void on_check_toggled(fdk_widget *check, bool checked,
                             void *user) {
    settings_app *app = user;
    push_action(app, checked ? "Enabled" : "Disabled");
    (void)check;
}

static void on_font_slider(fdk_widget *slider, void *user) {
    settings_app *app = user;
    char buf[64];
    double v = fdk_slider_get_value(slider);
    snprintf(buf, sizeof(buf), "Font size: %g px", v);
    push_action(app, buf);
    if (app->font_value != NULL) {
        char lbl[48];
        snprintf(lbl, sizeof(lbl), "%g px", v);
        (void)fdk_label_set_text(app->font_value, lbl);
    }
}

static void on_theme_radio(fdk_widget *radio, bool checked,
                           void *user) {
    settings_app *app = user;
    if (!checked) {
        return; /* only the newly-lit radio reports */
    }
    push_action(app, "Theme selected");
    (void)radio;
}

static void on_sync_toggled(fdk_widget *toggle, bool checked,
                            void *user) {
    settings_app *app = user;
    if (checked) {
        fdk_spinner_start(app->sync_spinner);
        push_action(app, "Syncing…");
    } else {
        fdk_spinner_stop(app->sync_spinner);
        push_action(app, "Sync paused");
    }
    (void)toggle;
}

static void on_storage_selected(fdk_widget *list, void *user) {
    settings_app *app = user;
    char buf[64];
    size_t n = fdk_list_selected_count(list);
    snprintf(buf, sizeof(buf), "%zu item%s selected", n,
             (n == 1) ? "" : "s");
    push_action(app, buf);
}

static void on_advanced(fdk_widget *button, void *user) {
    settings_app *app = user;
    bool now = !fdk_revealer_get_reveal_child(app->revealer);
    fdk_revealer_set_reveal_child(app->revealer, now);
    push_action(app, now ? "Advanced details shown"
                          : "Advanced details hidden");
    (void)button;
}

/* A one-shot "sync progresses" tick: the battery-style level bar
 * climbs while sync is on (the toggle's callback drives the
 * spinner; a simple frame counter here walks the level). */
static void on_frame(settings_app *app) {
    static int tick = 0;
    if (!fdk_spinner_is_spinning(app->sync_spinner)) {
        return;
    }
    tick++;
    if (tick % 8 != 0) {
        return;
    }
    double v = fdk_levelbar_get_value(app->sync_level);
    v += 1.0;
    if (v > 4.0) {
        v = 0.0; /* the demo loops */
    }
    fdk_levelbar_set_value(app->sync_level, v);
    fdk_levelbar_set_value(app->sync_level_cont, v / 4.0);
}

/* ---- page builders ---- */

static fdk_widget *build_general_page(settings_app *app,
                                       fdk_widget *stack) {
    fdk_widget *page = NULL;
    (void)fdk_box_create(stack, FDK_VERTICAL, &page);
    fdk_box_set_padding(page, 4);
    fdk_box_set_spacing(page, 12);

    fdk_font *font = app->ex->ui;

    fdk_widget *f1 = NULL;
    (void)fdk_frame_create(page, font, "Startup", &f1);
    fdk_widget *auto_start = NULL;
    (void)fdk_checkbox_create(f1, font, "Launch at login", &auto_start);
    fdk_checkbox_set_checked(auto_start, true);
    fdk_checkbox_set_on_changed(auto_start, on_check_toggled, app);
    fdk_widget *notifs = NULL;
    (void)fdk_checkbox_create(f1, font, "Show notifications", &notifs);
    fdk_checkbox_set_on_changed(notifs, on_check_toggled, app);

    fdk_widget *f2 = NULL;
    (void)fdk_frame_create(page, font, "Text", &f2);
    fdk_widget *slider = NULL;
    (void)fdk_slider_create(f2, 12.0, 24.0, 16.0, &slider);
    (void)fdk_slider_add_mark(slider, 12.0, "12");
    (void)fdk_slider_add_mark(slider, 16.0, "16");
    (void)fdk_slider_add_mark(slider, 24.0, "24");
    fdk_slider_set_step(slider, 1.0);
    fdk_slider_set_on_changed(slider, on_font_slider, app);
    fdk_widget *val = NULL;
    (void)fdk_label_create(f2, font, "16 px", &val);
    app->font_value = val;
    return page;
}

static fdk_widget *build_appearance_page(settings_app *app,
                                         fdk_widget *stack) {
    fdk_widget *page = NULL;
    (void)fdk_box_create(stack, FDK_VERTICAL, &page);
    fdk_box_set_padding(page, 4);
    fdk_box_set_spacing(page, 12);

    fdk_font *font = app->ex->ui;

    fdk_widget *f1 = NULL;
    (void)fdk_frame_create(page, font, "Theme", &f1);
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    (void)fdk_radio_create(f1, font, "Modern (default)", &r1);
    (void)fdk_radio_create(f1, font, "Classic", &r2);
    (void)fdk_radio_create(f1, font, "High contrast", &r3);
    fdk_radio_set_checked(r1, true);
    fdk_radio_set_on_changed(r1, on_theme_radio, app);
    fdk_radio_set_on_changed(r2, on_theme_radio, app);
    fdk_radio_set_on_changed(r3, on_theme_radio, app);

    fdk_widget *f2 = NULL;
    (void)fdk_frame_create(page, font, "Sync", &f2);
    fdk_widget *row = NULL;
    (void)fdk_box_create(f2, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 10);
    fdk_widget *sync = NULL;
    (void)fdk_toggle_create(row, font, "Sync", &sync);
    fdk_toggle_set_on_changed(sync, on_sync_toggled, app);
    fdk_widget *spin = NULL;
    (void)fdk_spinner_create(row, &spin);
    app->sync_spinner = spin;
    fdk_widget *lbl = NULL;
    (void)fdk_label_create(f2, font, "Sync level:", &lbl);
    fdk_widget *lb = NULL;
    (void)fdk_levelbar_create(f2, 0.0, 4.0, &lb);
    fdk_levelbar_set_segments(lb, 4);
    fdk_levelbar_set_value(lb, 2.0);
    app->sync_level = lb;
    fdk_widget *lbc = NULL;
    (void)fdk_levelbar_create(f2, 0.0, 1.0, &lbc);
    fdk_levelbar_set_mode(lbc, FDK_LEVELBAR_CONTINUOUS);
    fdk_levelbar_set_value(lbc, 0.5);
    app->sync_level_cont = lbc;
    return page;
}

static fdk_widget *build_storage_page(settings_app *app,
                                       fdk_widget *stack) {
    fdk_widget *page = NULL;
    (void)fdk_box_create(stack, FDK_VERTICAL, &page);
    fdk_box_set_padding(page, 4);
    fdk_box_set_spacing(page, 12);

    fdk_font *font = app->ex->ui;

    /* The multi-select list with the 1.4.1 symbolic row icons —
     * sweep-select by dragging on the EMPTY space below the rows
     * (the rubber band), ctrl-sweep to union. */
    fdk_widget *list = NULL;
    (void)fdk_list_create(page, font, &list);
    fdk_list_set_selection_mode(list, FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_selection_changed(list, on_storage_selected, app);
    static const struct {
        const char *text;
        fdk_row_icon icon;
    } rows[] = {
        {"Documents", FDK_ROW_ICON_FOLDER},
        {"Downloads", FDK_ROW_ICON_FOLDER},
        {"Pictures", FDK_ROW_ICON_FOLDER},
        {"notes.txt", FDK_ROW_ICON_FILE},
        {"report.pdf", FDK_ROW_ICON_FILE},
        {"archive.tar", FDK_ROW_ICON_FILE},
        {"System", FDK_ROW_ICON_DRIVE},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        fdk_result r = fdk_list_append(list, rows[i].text, NULL);
        if (fdk_ok(r)) {
            (void)fdk_list_row_set_icon(list, i, rows[i].icon);
        }
    }
    app->storage_list = list;

    /* The Advanced drawer: a link button flips a SLIDE_UP revealer
     * (the door's bottom edge stays put — it rises into the room). */
    fdk_widget *adv = NULL;
    (void)fdk_button_create(page, font, "Advanced details…", &adv);
    fdk_button_set_role(adv, FDK_BUTTON_ROLE_LINK);
    fdk_button_set_on_activate(adv, on_advanced, app);

    fdk_widget *rv = NULL;
    (void)fdk_revealer_create(page, &rv);
    fdk_revealer_set_transition(rv, FDK_REVEAL_SLIDE_UP);
    fdk_widget *drawer = NULL;
    (void)fdk_box_create(rv, FDK_VERTICAL, &drawer);
    fdk_box_set_spacing(drawer, 8);
    fdk_widget *d1 = NULL;
    (void)fdk_label_create(
        drawer, font, "Free space: 128 GB of 512 GB", &d1);
    fdk_widget *d2 = NULL;
    (void)fdk_label_create(
        drawer, font, "Cache: 1.2 GB (auto-managed)", &d2);
    fdk_widget *d3 = NULL;
    (void)fdk_levelbar_create(drawer, 0.0, 512.0, &d3);
    fdk_levelbar_set_value(d3, 384.0);
    app->revealer = rv;
    return page;
}

int main(void) {
    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "12")) {
        return 1;
    }

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "12", "settings app", 460, 560)) {
        fdk_shutdown(ctx);
        return 1;
    }

    settings_app app;
    memset(&app, 0, sizeof(app));
    app.ex = &ex;

    fdk_font *font = ex.ui;

    /* The search field. */
    fdk_widget *search = NULL;
    (void)fdk_search_entry_create(ex.content, font, &search);
    (void)fdk_entry_set_placeholder(search, "Search settings…");
    fdk_entry_set_on_changed(search, on_search_changed, &app);

    /* The stack + its pill switcher. */
    fdk_widget *stack = NULL;
    (void)fdk_stack_create(ex.content, &stack);
    fdk_widget_set_expand(stack, false, true);
    fdk_stack_set_on_changed(stack, on_page_changed, &app);

    fdk_widget *general = build_general_page(&app, stack);
    fdk_widget *appearance = build_appearance_page(&app, stack);
    fdk_widget *storage = build_storage_page(&app, stack);
    (void)fdk_stack_add(stack, general, "general", "General");
    (void)fdk_stack_add(stack, appearance, "appearance",
                        "Appearance");
    (void)fdk_stack_add(stack, storage, "storage", "Storage");

    fdk_widget *switcher = NULL;
    (void)fdk_stackswitcher_create(ex.content, font, &switcher);
    fdk_stackswitcher_set_stack(switcher, stack);

    /* The statusbar: hint context persistent underneath, action
     * context transient on top. */
    fdk_widget *sb = NULL;
    (void)fdk_statusbar_create(ex.content, font, &sb);
    app.statusbar = sb;
    app.ctx_hint = fdk_statusbar_get_context_id(sb, "hint");
    app.ctx_action = fdk_statusbar_get_context_id(sb, "action");
    (void)fdk_statusbar_push(sb, app.ctx_hint,
                             "Viewing: general — Esc quits");
    (void)fdk_statusbar_push(sb, app.ctx_action,
                             "3 pages, 7 storage items");

    (void)appearance;
    fdk_window_paint(ex.window);

    while (fdk_example_pump(&ex)) {
        on_frame(&app);
    }

    fdk_example_close(&ex);
    return 0;
}
