/* 04_widgets.c — the widget catalog, end to end.
 *
 * The entire interface is catalog widgets inside boxes, frames, and
 * (bottom) a GRID — no widget is placed by hand and no pixel is
 * drawn by the app; layout assigns geometry and the catalog paints
 * itself:
 *
 *   Profile frame   — a Toggle and two Checkboxes
 *   Renderer frame  — a radio group (the frame's children)
 *   Session frame   — the 1.4.1 furniture: an Expander whose door
 *                     reveals advanced options, and a Paned whose
 *                     divider DRAGS (and answers the arrows when
 *                     focused — Tab to it)
 *   1.4.5 frame     — the item grid: an IconView of glyph cells
 *   1.4.11 frame    — markup: rich text (bold/italic/color/underline
 *                     /strikethrough) in NOWRAP/WRAP/ELLIPSIZE
 *                     labels, a button, and the tooltips
 *                      (click, ctrl/shift, keyboard grid nav)
 *   1.4.4 frame     — the modern batch: a REORDERABLE notebook, a
 *                      color-well button, an icon-slot entry, an
 *                      overlay-scrollbar list
 *   1.4.3 frame     — the chooser batch's furniture: a MenuButton
 *                     whose attached model pops up (the app keeps
 *                     owning the model), a VERTICAL slider with
 *                     marks, a CROSSFADE revealer (a real per-pixel
 *                     fade — the paint-group engine), and the three
 *                     DIALOG buttons (About / Font / Color), each
 *                     opening the toolkit-owned dialog
 *   Layout frame    — a 3-column x 2-row grid with a two-column
 *                     SPAN cell and an expanding last column:
 *                     resize the window and watch ONLY that column
 *                     absorb the width while the other tracks and
 *                     the gaps stay exactly `spacing` pixels
 *   Buttons         — "Apply" grows the progress bar and rewrites
 *                     the status label; "Reset" clears everything;
 *                     a LINK-styled "Docs" button and a busy
 *                     SPINNER ride the same row (the spinner scans
 *                     for the duration of the startup sweep, then
 *                     parks); every control reports into the
 *                     status line
 *   Tooltips         — rest the pointer on any control for ~half a
 *                     second: a themed, click-through hint pops up
 *                     near it (Apply's is long enough to wrap); any
 *                     press, key, or hover-away dismisses it
 *
 * The progress bar sweeps once on startup (so a screenshot shows it
 * mid-fill) and then holds; Tab moves focus through the controls.
 * Set FDK_DEMO_ANIMATE=1 to keep it sweeping forever, or
 * FDK_DEMO_FRAMES=N to exit after N frames. Close the window or
 * press ESC to exit. Needs a system TrueType font (exits with a
 * notice otherwise).
 */

#include "example_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_argc = 0;
static char **g_argv = NULL;


static fdk_font *font16 = NULL;
static fdk_widget *status = NULL;
static fdk_widget *progress = NULL;
static int apply_count = 0;

static fdk_color col(int r, int g, int b) {
    return (fdk_color){ .r = (fdk_f32)r / 255.0f, .g = (fdk_f32)g / 255.0f,
                        .b = (fdk_f32)b / 255.0f, .a = 1.0f };
}

static void set_status(const char *text) {
    /* Writes the helper's status line (assigned after open). */
    (void)fdk_label_set_text(status, text);
}

static void on_apply(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    apply_count++;
    fdk_progress_set_fraction(
        progress, fdk_progress_get_fraction(progress) + 0.25f);
    char buf[64];
    snprintf(buf, sizeof(buf), "Applied %d time%s.", apply_count,
             apply_count == 1 ? "" : "s");
    set_status(buf);
}

static void on_reset(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    fdk_progress_set_fraction(progress, 0.0f);
    apply_count = 0;
    set_status("Reset. Nothing applied yet.");
}

static void on_any_change(fdk_widget *w, bool checked, void *user) {
    (void)w;
    const char *what = user;
    char buf[96];
    snprintf(buf, sizeof(buf), "%s is now %s.", what,
             checked ? "ON" : "OFF");
    set_status(buf);
}

static void on_docs(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    set_status("The documentation would open in a viewer.");
}

/* ---- 1.4.4: the modern-batch callbacks ---- */

static void on_page_reordered(fdk_widget *notebook, size_t from,
                              size_t to, void *user) {
    (void)notebook;
    (void)user;
    char buf[96];
    snprintf(buf, sizeof(buf),
             "Tab moved from slot %zu to %zu (the page kept showing).",
             from, to);
    set_status(buf);
}

static fdk_widget *g_well_label = NULL;
static void on_well_color(fdk_widget *button, fdk_color color,
                          void *user) {
    (void)button;
    (void)user;
    if (g_well_label != NULL) {
        fdk_widget_set_background(g_well_label, color);
    }
    set_status("Color well: the chooser's pick, live.");
}

static void on_path_icon(fdk_widget *entry, fdk_entry_slot slot,
                         void *user) {
    (void)entry;
    (void)user;
    (void)slot;
    set_status("The folder glyph is decorative — presses fall "
               "through to the caret.");
}

/* ---- 1.4.5: the grid callbacks ---- */

static void on_iv_selection(fdk_widget *view, void *user) {
    (void)view;
    (void)user;
    set_status("IconView: selection changed (try arrows — Down "
               "steps a whole row).");
}

static void on_iv_activate(fdk_widget *view, size_t index, void *user) {
    (void)view;
    (void)user;
    char buf[96];
    snprintf(buf, sizeof(buf), "IconView: opened item %zu.", index);
    set_status(buf);
}

/* ---- 1.4.3: the chooser-batch callbacks ---- */

static fdk_context *g_ctx = NULL; /* the dialogs' context */
static fdk_widget *g_fade_revealer = NULL; /* the crossfade door  */
static fdk_menu *g_menu = NULL;           /* the menu button's model */

static void on_menu_item(fdk_menu_item *item, void *user) {
    (void)user;
    char buf[96];
    snprintf(buf, sizeof(buf), "Menu: %s.",
             fdk_menu_item_text(item));
    set_status(buf);
}

static void on_fade_toggle(fdk_widget *w, bool checked, void *user) {
    (void)w;
    (void)user;
    /* The crossfade's flight: a REAL per-pixel fade through the
     * paint-group engine (not a slide — the door keeps its size). */
    if (g_fade_revealer != NULL) {
        fdk_revealer_set_reveal_child(g_fade_revealer, checked);
    }
    set_status(checked ? "Crossfaded in." : "Crossfaded out.");
}

static void on_about(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    fdk_about_dialog_options opts = {
        .program_name = "FDK Widget Catalog",
        .version = "Milestone 1.4.3",
        .comments = "The About dialog: an identity card built from the "
                    "stock catalog — logo, name, version, comments, "
                    "copyright, a website row, and a scrolling license.",
        .copyright = "(c) the FDK authors",
        .website = "https://fdk.example",
        .license = "Permission is hereby granted, free of charge, to "
                   "any person obtaining a copy of this software...",
    };
    (void)fdk_dialog_show_about(g_ctx, &opts, NULL, NULL, NULL, NULL);
    set_status("The About dialog opened.");
}

static void on_font_chooser(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    (void)fdk_dialog_choose_font(g_ctx, NULL, NULL, NULL, NULL);
    set_status("The font chooser opened.");
}

static void on_color_chooser(fdk_widget *w, void *user) {
    (void)w;
    (void)user;
    fdk_color_dialog_options opts = {
        .initial = {0.25f, 0.5f, 0.75f, 1.0f},
    };
    (void)fdk_dialog_choose_color(g_ctx, &opts, NULL, NULL, NULL);
    set_status("The color chooser opened.");
}

int main(int argc, char **argv) {
    g_argc = argc;
    g_argv = argv;
    font16 = fdk_font_load_system_default(16);
    if (font16 == NULL) {
        fprintf(stderr, "04_widgets: no system TrueType font found "
                        "— this demo needs one. Install a face like "
                        "DejaVu Sans or Noto Sans, or point "
                        "FDK_FONT_FILE at a .ttf/.ttc\n");
        return 1;
    }
    printf("04_widgets: using font %s\n",
           fdk_font_get_file_path(font16));

    fdk_context *ctx = NULL;
    if (!fdk_example_init(&ctx, "04")) {
        return 1;
    }
    g_ctx = ctx;

    fdk_example ex;
    if (!fdk_example_open(&ex, ctx, "04", "widgets", 560, 2260)) {
        fdk_shutdown(ctx);
        return 1;
    }
    fdk_widget *content = ex.content;
    fdk_box_set_spacing(content, 12);

    /* --- frame: profile options --- */
    fdk_widget *profile = NULL;
    (void)fdk_frame_create(content, font16, "Profile", &profile);
    fdk_widget_set_background_token(profile, FDK_TK_CONTROL_BACKGROUND);
    /* 1.4.0: an entry with PLACEHOLDER text — the hint paints in the
     * disabled-text color only while the field is empty. */
    fdk_widget *search = NULL;
    (void)fdk_entry_create(profile, font16, "", &search);
    fdk_entry_set_placeholder(search, "Search the settings…");
    fdk_widget *pub = NULL;
    (void)fdk_toggle_create(profile, font16, "Public profile", &pub);
    fdk_toggle_set_on_changed(pub, on_any_change, (void *)"Public profile");
    (void)fdk_widget_set_tooltip(pub, "Anyone can view your profile page");
    fdk_widget *mail = NULL;
    (void)fdk_checkbox_create(profile, font16, "Show email address",
                              &mail);
    fdk_checkbox_set_on_changed(mail, on_any_change,
                               (void *)"Show email");
    (void)fdk_widget_set_tooltip(
        mail, "Your email appears on your public profile");
    fdk_widget *news = NULL;
    (void)fdk_checkbox_create(profile, font16, "Newsletter", &news);
    fdk_checkbox_set_on_changed(news, on_any_change,
                               (void *)"Newsletter");
    (void)fdk_widget_set_tooltip(
        news, "One digest email per week — no marketing");

    /* --- frame: rendering mode (radio group = frame's children) --- */
    fdk_widget *render = NULL;
    (void)fdk_frame_create(content, font16, "Renderer", &render);
    fdk_widget_set_background_token(render, FDK_TK_CONTROL_BACKGROUND);
    fdk_widget *r1 = NULL, *r2 = NULL, *r3 = NULL;
    (void)fdk_radio_create(render, font16, "Software (X11)", &r1);
    (void)fdk_radio_create(render, font16, "Software (Wayland)", &r2);
    (void)fdk_radio_create(render, font16, "Software (auto)", &r3);
    fdk_radio_set_checked(r3, true);
    fdk_radio_set_on_changed(r1, on_any_change, (void *)"Renderer: X11");
    fdk_radio_set_on_changed(r2, on_any_change,
                            (void *)"Renderer: Wayland");
    fdk_radio_set_on_changed(r3, on_any_change, (void *)"Renderer: auto");
    (void)fdk_widget_set_tooltip(r1, "Draw with plain X11 put-image");
    (void)fdk_widget_set_tooltip(r2, "Draw through the Wayland stack");
    (void)fdk_widget_set_tooltip(
        r3, "Pick the backend from the session environment");

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: session (the 1.4.1 interactive furniture) ---
     *
     * The Expander's door reveals the "advanced" options (click the
     * header, or Tab to it and press Space; the chevron rotates as
     * the door flies). The Paned below it splits two work areas:
     * drag the divider, or focus the paned (Tab) and step it with
     * the arrows — Home/End jump to the walls. */
    fdk_widget *session = NULL;
    (void)fdk_frame_create(content, font16, "Session", &session);
    fdk_widget_set_background_token(session, FDK_TK_CONTROL_BACKGROUND);
    fdk_widget *adv = NULL;
    (void)fdk_expander_create(session, font16, "Advanced settings", &adv);
    (void)fdk_widget_set_tooltip(
        adv, "The expander reveals its content with an animated door — "
             "click the header or press Space while focused");
    fdk_widget *preload = NULL;
    (void)fdk_checkbox_create(adv, font16, "Preload file metadata",
                              &preload);
    fdk_checkbox_set_on_changed(preload, on_any_change,
                               (void *)"Preload metadata");
    fdk_widget *cache = NULL;
    (void)fdk_entry_create(adv, font16, "", &cache);
    fdk_entry_set_placeholder(cache, "Custom cache path…");

    fdk_widget *split = NULL;
    (void)fdk_paned_create(session, FDK_HORIZONTAL, &split);
    fdk_widget_set_natural_size(split, 0, 62);
    fdk_widget_set_expand(split, true, false);
    fdk_widget *pane_a = NULL;
    (void)fdk_widget_create(split, NULL, (fdk_rect){0, 0, 210, 62},
                            &pane_a);
    fdk_widget_set_background_token(pane_a, FDK_TK_CONTROL_BACKGROUND);
    fdk_widget_set_corner_radius(pane_a, 6);
    fdk_widget *pane_b = NULL;
    (void)fdk_widget_create(split, NULL, (fdk_rect){0, 0, 210, 62},
                            &pane_b);
    fdk_widget_set_background_token(pane_b, FDK_TK_SIDEBAR_BACKGROUND);
    fdk_widget_set_corner_radius(pane_b, 6);
    (void)fdk_widget_set_tooltip(
        split, "Drag the divider between the panes — or focus the "
               "splitter and use the arrow keys (Home/End for the "
               "walls)");

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: the 1.4.3 furniture (the chooser batch) ---
     *
     * A MenuButton with an ATTACHED model (the model stays the
     * demo's — reusable, app-owned); a VERTICAL slider with marks
     * (min at the BOTTOM, ticks to the left); and a CROSSFADE
     * revealer whose door fades per-pixel through the paint-group
     * engine. The three dialog buttons open the toolkit-owned
     * choosers. */
    {
        fdk_widget *f143 = NULL;
        (void)fdk_frame_create(content, font16, "1.4.3 — choosers",
                               &f143);
        fdk_widget_set_background_token(f143,
                                FDK_TK_CONTROL_BACKGROUND);

        fdk_widget *frow = NULL;
        (void)fdk_box_create(f143, FDK_HORIZONTAL, &frow);
        fdk_box_set_spacing(frow, 14);

        /* The menu button + its model. */
        fdk_menu *menu = NULL;
        (void)fdk_menu_create(font16, &menu);
        fdk_menu_item *mi = NULL;
        (void)fdk_menu_append(menu, "Refresh", &mi);
        fdk_menu_item_set_on_activate(mi, on_menu_item, NULL);
        (void)fdk_menu_append(menu, "Revert changes", &mi);
        fdk_menu_item_set_on_activate(mi, on_menu_item, NULL);
        (void)fdk_menu_append_separator(menu);
        (void)fdk_menu_append(menu, "Preferences…", &mi);
        fdk_menu_item_set_on_activate(mi, on_menu_item, NULL);
        g_menu = menu;
        fdk_widget *mbtn = NULL;
        (void)fdk_menu_button_create(frow, font16, "Actions", &mbtn);
        (void)fdk_menu_button_set_menu(mbtn, menu);
        (void)fdk_widget_set_tooltip(
            mbtn, "A menu button: click to pop the attached model up "
                  "(the app keeps owning it — the arrow tracks the "
                  "chain)");

        /* The vertical slider with marks. */
        fdk_widget *vsl = NULL;
        (void)fdk_slider_create(frow, 0.0, 100.0, 65.0, &vsl);
        fdk_slider_set_orientation(vsl, FDK_SLIDER_VERTICAL);
        fdk_widget_set_natural_size(vsl, 0, 96);
        (void)fdk_slider_add_mark(vsl, 0.0, "0");
        (void)fdk_slider_add_mark(vsl, 100.0, "100");
        (void)fdk_widget_set_tooltip(
            vsl, "The vertical slider: min at the bottom, ticks and "
                 "labels to the left of the trough");

        /* The crossfade revealer + its toggle. */
        fdk_widget *cfcol = NULL;
        (void)fdk_box_create(frow, FDK_VERTICAL, &cfcol);
        fdk_box_set_spacing(cfcol, 8);
        fdk_widget *fade_t = NULL;
        (void)fdk_toggle_create(cfcol, font16, "Crossfade", &fade_t);
        fdk_toggle_set_on_changed(fade_t, on_fade_toggle, NULL);
        fdk_widget *fade_rv = NULL;
        (void)fdk_revealer_create(cfcol, &fade_rv);
        fdk_revealer_set_transition(fade_rv, FDK_REVEAL_CROSSFADE);
        fdk_widget *fade_panel = NULL;
        (void)fdk_widget_create(fade_rv, NULL, (fdk_rect){0, 0, 170, 46},
                                &fade_panel);
        fdk_widget_set_background(fade_panel, col(70, 130, 230));
        fdk_widget_set_corner_radius(fade_panel, 6);
        (void)fdk_widget_set_tooltip(
            fade_rv, "The crossfade revealer: a real per-pixel fade "
                     "(the paint-group engine) — the door keeps its "
                     "size while the content blends in");
        /* Wire the toggle to the revealer's target. */
        g_fade_revealer = fade_rv;
        fdk_toggle_set_on_changed(fade_t, on_fade_toggle, NULL);

        /* The three chooser dialogs. */
        fdk_widget *about_b = NULL;
        (void)fdk_button_create(frow, font16, "About…", &about_b);
        fdk_button_set_on_activate(about_b, on_about, NULL);
        fdk_widget *font_b = NULL;
        (void)fdk_button_create(frow, font16, "Font…", &font_b);
        fdk_button_set_on_activate(font_b, on_font_chooser, NULL);
        fdk_widget *color_b = NULL;
        (void)fdk_button_create(frow, font16, "Color…", &color_b);
        fdk_button_set_on_activate(color_b, on_color_chooser, NULL);
    }

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: the 1.4.4 furniture (the modern batch) ---
     *
     * A REORDERABLE notebook (drag any tab — the page keeps
     * showing while its tab moves); a COLOR-WELL button whose
     * chooser pick repaints a live swatch label; an entry with the
     * FOLDER leading glyph and the CLEAR trailing button; and an
     * overlay-scrollbar list (thin transient thumbs that fade when
     * idle — wheel it and watch the bar breathe). */
    {
        fdk_widget *f144 = NULL;
        (void)fdk_frame_create(content, font16, "1.4.4 — modern batch",
                               &f144);
        fdk_widget_set_background_token(f144,
                                FDK_TK_CONTROL_BACKGROUND);

        fdk_widget *mrow = NULL;
        (void)fdk_box_create(f144, FDK_HORIZONTAL, &mrow);
        fdk_box_set_spacing(mrow, 16);

        /* The reorderable notebook: three colored pages. */
        fdk_widget *nb = NULL;
        (void)fdk_notebook_create(mrow, font16, &nb);
        fdk_widget_set_natural_size(nb, 300, 150);
        fdk_notebook_set_on_page_reordered(nb, on_page_reordered, NULL);
        static const char *nb_labels[3] = {"Alpha", "Beta", "Gamma"};
        static const fdk_color nb_cols[3] = {
            {0.30f, 0.45f, 0.75f, 1.0f},
            {0.35f, 0.65f, 0.45f, 1.0f},
            {0.70f, 0.50f, 0.75f, 1.0f},
        };
        for (int i = 0; i < 3; i++) {
            fdk_widget *page = NULL;
            (void)fdk_widget_create(nb, NULL, (fdk_rect){0, 0, 10, 10},
                                    &page);
            fdk_widget_set_background(page, nb_cols[i]);
            (void)fdk_notebook_append_page(nb, page, nb_labels[i]);
        }
        (void)fdk_widget_set_tooltip(
            nb, "Drag a tab to reorder it — the strip swaps live "
                "and the shown page never changes");

        /* The color well + its live swatch. */
        fdk_widget *wcol = NULL;
        (void)fdk_box_create(mrow, FDK_VERTICAL, &wcol);
        fdk_box_set_spacing(wcol, 10);
        fdk_widget *well = NULL;
        (void)fdk_color_button_create(
            wcol, (fdk_color){0.95f, 0.62f, 0.25f, 1.0f}, &well);
        fdk_color_button_set_title(well, "Pick the accent");
        fdk_color_button_set_on_color_set(well, on_well_color, NULL);
        (void)fdk_widget_set_tooltip(
            well, "The color well: opens the HSV chooser; the pick "
                  "repaints the swatch below (alpha shows its "
                  "checkerboard)");
        fdk_widget *well_label = NULL;
        (void)fdk_widget_create(wcol, NULL, (fdk_rect){0, 0, 90, 26},
                                &well_label);
        fdk_widget_set_background(well_label,
                                  (fdk_color){0.95f, 0.62f, 0.25f, 1.0f});
        fdk_widget_set_corner_radius(well_label, 6);
        g_well_label = well_label;

        /* The icon-slot entry: a path field. */
        fdk_widget *pentry = NULL;
        (void)fdk_entry_create(wcol, font16, "/home/dev/notes.txt",
                               &pentry);
        (void)fdk_entry_set_icon(pentry, FDK_ENTRY_SLOT_LEADING,
                                 FDK_ENTRY_ICON_FOLDER);
        (void)fdk_entry_set_icon(pentry, FDK_ENTRY_SLOT_TRAILING,
                                 FDK_ENTRY_ICON_CLEAR);
        (void)fdk_entry_set_on_icon_press(pentry, FDK_ENTRY_SLOT_LEADING,
                                          on_path_icon, NULL);
        fdk_widget_set_natural_size(pentry, 220, 0);
        (void)fdk_widget_set_tooltip(
            pentry, "Icon slots: the folder glyph leads, the X "
                    "clears (undoably) — geometry, hover pill and "
                    "presses included");

        /* The overlay-scrollbar view: a plain ScrollView over a
         * tall label column (the public path — no internals). */
        fdk_widget *osv = NULL;
        (void)fdk_scrollview_create(mrow, &osv);
        fdk_widget_set_natural_size(osv, 190, 150);
        fdk_widget *ocol = NULL;
        (void)fdk_box_create(osv, FDK_VERTICAL, &ocol);
        fdk_box_set_spacing(ocol, 4);
        for (int i = 1; i <= 24; i++) {
            char text[32];
            snprintf(text, sizeof(text), "Overlay row %d", i);
            fdk_widget *lab = NULL;
            (void)fdk_label_create(ocol, font16, text, &lab);
        }
        (void)fdk_scrollview_set_content(osv, ocol);
        (void)fdk_scrollview_set_bar_mode(osv,
                                          FDK_SCROLL_BARS_OVERLAY);
        (void)fdk_widget_set_tooltip(
            osv, "Overlay scrollbars: thin thumbs that fade when "
                 "idle and return on scroll — wheel me");
    }

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: the 1.4.5 furniture (the item grid) ---
     *
     * An IconView: fixed-size cells (glyph over label) flowing in
     * as many columns as the width fits; the whole selection model
     * (click / ctrl / shift in reading order) and grid keyboard
     * navigation (Down steps a ROW). */
    {
        fdk_widget *f145 = NULL;
        (void)fdk_frame_create(content, font16, "1.4.5 — item grid",
                               &f145);
        fdk_widget_set_background_token(f145,
                                FDK_TK_CONTROL_BACKGROUND);
        fdk_widget *iv = NULL;
        (void)fdk_iconview_create(f145, font16, &iv);
        static const char *iv_labels[6] = {
            "Folder", "Report", "Home", "Drive", "Notes", "Recent",
        };
        static const fdk_row_icon iv_icons[6] = {
            FDK_ROW_ICON_FOLDER, FDK_ROW_ICON_FILE, FDK_ROW_ICON_HOME,
            FDK_ROW_ICON_DRIVE, FDK_ROW_ICON_FILE, FDK_ROW_ICON_RECENT,
        };
        fdk_iconview_begin_batch(iv);
        for (int i = 0; i < 6; i++) {
            (void)fdk_iconview_append(iv, iv_labels[i], iv_icons[i]);
        }
        fdk_iconview_end_batch(iv);
        fdk_widget_set_natural_size(iv, 420, 200);
        fdk_iconview_set_on_selection_changed(iv, on_iv_selection,
                                              NULL);
        fdk_iconview_set_on_item_activate(iv, on_iv_activate, NULL);
        (void)fdk_widget_set_tooltip(
            iv, "The item grid: cells flow into columns; click, "
                "ctrl/shift-select, or drive it with the arrows "
                "(Down steps a whole row; Enter opens)");
    }

    /* --- frame: the 1.4.9 picture (the image display) ---
     *
     * The logo under all four fit policies is a four-way viewer:
     * NONE (1:1 clipped), CONTAIN (letterbox), COVER (crop), FILL
     * (stretch). The loaded file path can be overridden as argv[1].
     */
    {
        fdk_widget *f149 = NULL;
        (void)fdk_frame_create(content, font16,
                               "1.4.9 — picture (four fits)",
                               &f149);
        fdk_widget_set_background_token(f149,
                                FDK_TK_CONTROL_BACKGROUND);
        const char *pic_path = "examples/data/fdk_logo.png";
        if (g_argc > 1 && g_argv[1] != NULL) {
            pic_path = g_argv[1];
        }
        static const fdk_picture_fit fits[4] = {
            FDK_PICTURE_FIT_NONE, FDK_PICTURE_FIT_CONTAIN,
            FDK_PICTURE_FIT_COVER, FDK_PICTURE_FIT_FILL,
        };
        static const char *fit_names[4] = {
            "NONE", "CONTAIN", "COVER", "FILL",
        };
        for (int i = 0; i < 4; i++) {
            fdk_widget *row = NULL;
            (void)fdk_box_create(f149, FDK_HORIZONTAL, &row);
            fdk_widget_set_expand(row, true, false);
            fdk_widget *lab = NULL;
            (void)fdk_label_create(row, font16, fit_names[i], &lab);
            fdk_widget_set_natural_size(lab, 64, 0);
            fdk_label_set_color(lab, col(150, 158, 178));
            fdk_widget *pic = NULL;
            (void)fdk_picture_create(row, &pic);
            if (fdk_picture_set_from_file(pic, pic_path) != FDK_OK) {
                /* No logo in this checkout: the empty picture keeps
                 * the frame honest (zero-size, no crash). */
            }
            fdk_picture_set_fit(pic, fits[i]);
            fdk_widget_set_natural_size(pic, 0, 72);
            fdk_widget_set_expand(pic, true, true);
            (void)fdk_widget_set_tooltip(
                pic, "The picture widget: an owned surface under a "
                     "content-fit policy");
        }
    }

    /* --- frame: the 1.4.11 markup (rich text everywhere) ---
     *
     * One tiny tag vocabulary (<b> <i> <u> <s> <color=#rrggbb> +
     * entities) driving every text surface: a NOWRAP label with
     * mixed styles and colors, a WRAP label whose bold words wrap
     * by their BOLD widths, an ELLIPSIZE label cut by styled
     * metrics, and a button whose label is markup. Hover the rows:
     * the TOOLTIPS are markup too. */
    {
        fdk_widget *f1411 = NULL;
        (void)fdk_frame_create(content, font16,
                               "1.4.11 — markup (rich text)", &f1411);
        fdk_widget_set_background_token(f1411,
                                FDK_TK_CONTROL_BACKGROUND);

        fdk_widget *mrow1 = NULL;
        (void)fdk_box_create(f1411, FDK_HORIZONTAL, &mrow1);
        fdk_box_set_spacing(mrow1, 16);
        fdk_widget *ml1 = NULL;
        (void)fdk_label_create(
            mrow1, font16,
            "<b>Bold</b> <i>italic</i> <u>under</u> "
            "<s>gone</s> <color=#f0a35e>amber</color> "
            "<b><i><color=#7ec8e3>everything</color></i></b> "
            "&amp; entities &lt;too&gt;",
            &ml1);
        (void)fdk_widget_set_tooltip_markup(
            ml1, "<b>NOWRAP</b> markup: mixed styles, run colors, "
                 "decoration bars — <u>one</u> label");

        fdk_widget *mrow2 = NULL;
        (void)fdk_box_create(f1411, FDK_HORIZONTAL, &mrow2);
        fdk_box_set_spacing(mrow2, 16);
        fdk_widget *ml2 = NULL;
        (void)fdk_label_create(
            mrow2, font16,
            "<b>Wrapped markup:</b> the <color=#9fd98a>green</color> "
            "words wrap by their <b>BOLD</b> widths — a styled word "
            "moves whole, exactly like a plain one",
            &ml2);
        fdk_label_set_mode(ml2, FDK_LABEL_WRAP);
        fdk_widget_set_natural_size(ml2, 400, 0);
        fdk_widget_set_expand(ml2, true, false);
        (void)fdk_widget_set_tooltip_markup(
            ml2, "<b>WRAP</b> markup: the break engine measures "
                 "each glyph at <i>its own</i> style");

        fdk_widget *mrow3 = NULL;
        (void)fdk_box_create(f1411, FDK_HORIZONTAL, &mrow3);
        fdk_box_set_spacing(mrow3, 16);
        fdk_widget *ml3 = NULL;
        (void)fdk_label_create(
            mrow3, font16,
            "<b>A very long bold headline that will not fit and "
            "gets cut by styled metrics</b>",
            &ml3);
        fdk_label_set_mode(ml3, FDK_LABEL_ELLIPSIZE);
        fdk_widget_set_natural_size(ml3, 300, 0);
        fdk_widget_set_expand(ml3, true, false);
        (void)fdk_widget_set_tooltip_markup(
            ml3, "<b>ELLIPSIZE</b> markup: the prefix is chosen by "
                 "<color=#f0a35e>styled</color> widths");

        fdk_widget *mbtn = NULL;
        (void)fdk_button_create(mrow3, font16, NULL, &mbtn);
        (void)fdk_button_set_markup(
            mbtn, "Run <b>now</b>, <color=#9fd98a>go</color>");
        (void)fdk_widget_set_tooltip_markup(
            mbtn, "A markup <b>button</b>: the label is rich text "
                 "too");
    }

    (void)fdk_separator_create(content, FDK_HORIZONTAL, NULL);

    /* --- frame: the GRID (the layout engine's third container) ---
     * 3 columns x 2 rows, spacing 8. Track naturals come from the
     * cells' create bounds: col widths 90 / 120 / 70, row heights 40.
     * The bottom-left cell SPANS two columns; the LAST column is
     * expand-marked, so it — and only it — absorbs the window's
     * width changes (resize the window and watch it grow while the
     * other tracks and the gaps stay put). */
    fdk_widget *grid_frame = NULL;
    (void)fdk_frame_create(content, font16, "Layout — grid", &grid_frame);
    fdk_widget_set_background_token(grid_frame,
                                FDK_TK_CONTROL_BACKGROUND);
    fdk_widget *cells = NULL;
    (void)fdk_grid_create(grid_frame, 2, 3, &cells);
    fdk_grid_set_spacing(cells, 8);
    fdk_grid_set_column_expand(cells, 2, true);
    fdk_widget *gc[5];
    const int gc_rgb[5][3] = {
        {70, 130, 230},  /* (0,0) blue — track 0 */
        {235, 170, 70}, /* (1,0) amber — track 1 */
        {160, 100, 230},/* (2,0) violet — the EXPANDING track */
        {70, 190, 200}, /* (0,1)+(1,1) teal, SPANNING tracks 0+1 */
        {230, 110, 170},/* (2,1) pink — expanding track, row 1 */
    };
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 90, 40}, &gc[0]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 120, 40}, &gc[1]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 70, 40}, &gc[2]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 90, 40}, &gc[3]);
    (void)fdk_widget_create(cells, NULL, (fdk_rect){0, 0, 70, 40}, &gc[4]);
    for (int i = 0; i < 5; i++) {
        fdk_widget_set_background(gc[i], col(gc_rgb[i][0], gc_rgb[i][1],
                                             gc_rgb[i][2]));
        fdk_widget_set_corner_radius(gc[i], 6);
    }
    (void)fdk_grid_attach(cells, gc[0], 0, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[1], 1, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[2], 2, 0, 1, 1);
    (void)fdk_grid_attach(cells, gc[3], 0, 1, 2, 1); /* colspan 2 */
    (void)fdk_grid_attach(cells, gc[4], 2, 1, 1, 1);

    /* --- button row (1.4.0 roles) --- */
    fdk_widget *row = NULL;
    (void)fdk_box_create(content, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 10);
    fdk_widget *apply = NULL;
    (void)fdk_button_create(row, font16, "Apply", &apply);
    fdk_button_set_role(apply, FDK_BUTTON_ROLE_SUGGESTED);
    fdk_button_set_on_activate(apply, on_apply, NULL);
    (void)fdk_widget_set_tooltip(
        apply,
        "Apply the current settings and advance the progress bar by "
        "one quarter — a long hint like this one wraps at a readable "
        "width instead of stretching across the window");
    fdk_widget *reset = NULL;
    (void)fdk_button_create(row, font16, "Reset", &reset);
    fdk_button_set_on_activate(reset, on_reset, NULL);
    (void)fdk_widget_set_tooltip(
        reset, "Clear the progress bar and the apply counter");
    /* A toggle-BUTTON (stays pressed in) and a destructive one —
     * the 1.4.0 role/state vocabulary. */
    fdk_widget *pin = NULL;
    (void)fdk_button_create(row, font16, "Pin", &pin);
    fdk_button_set_checked(pin, true);
    (void)fdk_widget_set_tooltip(
        pin, "A checked button stays pressed in (the toggle-button "
             "state) — app state drives it");
    fdk_widget *danger = NULL;
    (void)fdk_button_create(row, font16, "Delete profile…", &danger);
    fdk_button_set_role(danger, FDK_BUTTON_ROLE_DESTRUCTIVE);
    fdk_button_set_on_activate(danger, on_reset, NULL);
    (void)fdk_widget_set_tooltip(
        danger, "The destructive role paints danger-filled — reserve "
                "it for the irreversible action");
    fdk_widget *filler = NULL;
    (void)fdk_widget_create(row, NULL, (fdk_rect){0, 0, 0, 1}, &filler);
    fdk_widget_set_expand(filler, true, false);
    /* 1.4.1: the LINK-styled button (flat, link-colored, the
     * underline fades in under the pointer) and the busy SPINNER
     * (scanning for the duration of the startup sweep — then it
     * parks, which is what a screenshot shows). */
    fdk_widget *docs = NULL;
    (void)fdk_button_create(row, font16, "Docs", &docs);
    fdk_button_set_role(docs, FDK_BUTTON_ROLE_LINK);
    fdk_button_set_on_activate(docs, on_docs, NULL);
    (void)fdk_widget_set_tooltip(
        docs, "A link-styled button — flat, link-colored, the "
              "underline fades in under the pointer");
    fdk_widget *spinner = NULL;
    (void)fdk_spinner_create(row, &spinner);
    fdk_spinner_start(spinner);
    (void)fdk_widget_set_tooltip(
        spinner, "A busy indicator — the arc rotates while work is "
                 "happening with no known fraction");

    /* --- progress + status --- */
    (void)fdk_progress_create(content, &progress);
    fdk_widget_set_natural_size(progress, 0, 14);
    fdk_widget_set_expand(progress, true, false);
    (void)fdk_widget_set_tooltip(
        progress, "How much of the startup sweep has completed");

    /* The demo's status line IS the helper's (bottom of the frame). */
    status = ex.status;

    const char *anim = getenv("FDK_DEMO_ANIMATE");
    const bool animate =
        anim != NULL && anim[0] != '\0' && strcmp(anim, "0") != 0;

    while (fdk_example_pump(&ex)) {
        /* One startup sweep so a screenshot shows the bar mid-fill;
         * then it holds (an idle app presents nothing). The spinner
         * scans for the same duration, then parks (the "door" on
         * the 1.4.1 furniture stays as the user left it). */
        if (ex.frames < 120) {
            fdk_progress_set_fraction(
                progress, (fdk_f32)ex.frames / 120.0f);
            if (ex.frames == 0) {
                fdk_example_set_status(&ex, "Sweeping...");
            }
        } else if (ex.frames == 120) {
            fdk_spinner_stop(spinner);
            if (!animate) {
                apply_count = 4;
                fdk_example_set_status(&ex, "Applied 4 times.");
            }
        } else if (animate) {
            fdk_progress_set_fraction(
                progress, (fdk_f32)(ex.frames % 200) / 199.0f);
        }
    }

    fdk_font_destroy(font16);
    fdk_menu_destroy(g_menu);
    fdk_example_close(&ex);
    return 0;
}
