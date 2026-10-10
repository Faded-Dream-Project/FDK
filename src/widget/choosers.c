#define FDK_LOG_TAG "widgets"

/*
 * choosers.c — the 1.4.3 dialog trio: About, FontChooser, ColorChooser
 *
 * All three ride the dialog.c lifecycle contract: a toolkit-owned
 * window, auto-painted, exactly one callback, self-destroying after
 * it — built from the stock catalog like every FDK dialog. Each has
 * its own body class whose arrange hook lays the content out (the
 * message dialog's discipline: fixed-size in practice, a resize
 * re-flows).
 *
 * The About dialog is the app's identity card: logo (decoded image,
 * omitted honestly on failure), program name, version, wrap-text
 * comments, copyright, a LINK-styled website row (activating it
 * fires a callback with the URL — FDK never launches anything), and
 * an optionally scrolling license block.
 *
 * The FontChooser exposes fdk_font_enumerate as a picker: a
 * scrolling face list, a size spinner, and a live preview label that
 * reloads the selected face at the selected size on every change.
 *
 * The ColorChooser is the HSV wheel on the canvas: a hue ring with
 * an inscribed saturation/value triangle, rasterized PER-PIXEL in
 * the paint callback (direct framebuffer writes — no primitive
 * approximation), with drag hit-testing on both regions, marker
 * dots, a #rrggbb entry, and current/initial swatches.
 */

#include "widgets_internal.h"
#include "menu_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_dialog.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHOOSE_PAD 20
#define CHOOSE_BTN_GAP 10
#define CHOOSE_BTN_MIN_W 84
#define CHOOSE_FONT_PX 14

/* ===================================================================
 * About dialog
 * =================================================================== */

typedef struct fdk_about {
    fdk_window *window;
    fdk_widget *body;
    fdk_font *font;         /* effective (owned system default) */
    fdk_font *name_font;    /* the larger face for the name; owned;
                               == font when the system could not
                               provide a bigger one               */
    fdk_dialog_response_fn on_response;
    fdk_about_website_fn on_website;
    char *website;          /* owned copy; NULL = no row      */
    void *user_data;
    fdk_dialog_response negative;
    bool modal;
    bool responded;
    bool surfaced;
    /* Content handles (children of the body, in arrange order). */
    fdk_widget *logo;       /* canvas, NULL when none         */
    fdk_surface *logo_img;  /* decoded image, owned           */
    fdk_widget *name_label;
    fdk_widget *version_label;
    fdk_widget *comments;   /* wrap label, NULL when none     */
    fdk_widget *copyright_l;
    fdk_widget *website_btn;
    fdk_widget *license_scroll; /* NULL when none             */
    fdk_widget *license_label;
} fdk_about;

typedef struct fdk_about_body {
    fdk_widget base;
    fdk_about *about;    /* owned */
} fdk_about_body;

static fdk_about *about_of_body(fdk_widget *w) {
    return (w != NULL) ? ((fdk_about_body *)(void *)w)->about : NULL;
}

static void about_body_set(fdk_widget *w, fdk_about *a) {
    ((fdk_about_body *)(void *)w)->about = a;
}


static void about_respond(fdk_about *a, fdk_dialog_response response) {
    if (a->responded) {
        return;
    }
    a->responded = true;
    if (a->on_response != NULL) {
        a->on_response(response, a->user_data);
    }
    if (a->window != NULL) {
        fdk_window *win = a->window;
        a->window = NULL;
        fdk_window_destroy(win);
    }
}

static void about_window_event(fdk_window *window,
                               const fdk_event_data *ev, void *user) {
    (void)window;
    fdk_about *a = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (ev->type == FDK_EVENT_KEY_DOWN &&
         ev->key.scancode == FDK_KEY_ESC)) {
        about_respond(a, a->negative);
    }
}

static void about_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_about *a = user;
    if (a->responded || !a->surfaced) {
        return;
    }
    a->responded = true;
    if (a->on_response != NULL) {
        a->on_response(a->negative, a->user_data);
    }
}

static void about_website_clicked(fdk_widget *button, void *user) {
    (void)button;
    fdk_about *a = user;
    if (a->on_website != NULL && a->website != NULL) {
        a->on_website(a->website, a->user_data);
    }
}

static void about_close_clicked(fdk_widget *button, void *user) {
    (void)button;
    about_respond(user, FDK_DIALOG_CLOSE);
}

/* The logo's paint: the decoded image, top-left in the canvas. */
static void about_logo_paint(fdk_widget *canvas, fdk_surface *surface,
                             fdk_rect bounds, fdk_rect clip,
                             void *user) {
    (void)canvas;
    (void)clip;
    fdk_about *a = user;
    if (a->logo_img == NULL || bounds.width <= 0 ||
        bounds.height <= 0) {
        return;
    }
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(a->logo_img, &info))) {
        return;
    }
    fdk_i32 w = (info.width < bounds.width) ? info.width
                                             : bounds.width;
    fdk_i32 h = (info.height < bounds.height) ? info.height
                                               : bounds.height;
    if (w <= 0 || h <= 0) {
        return;
    }
    (void)fdk_surface_blit(surface, bounds.x, bounds.y, a->logo_img,
                           (fdk_rect){0, 0, w, h});
}

static void about_body_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_about *a = about_of_body(w);
    if (a == NULL) {
        return;
    }
    /* The Close button: bottom-right, the message dialog's rule.
     * Children in the BODY's local coordinates (1.4.16: the 1.4.15
     * band made assigned.y nonzero and the old assigned.y + ... bases
     * double-offset the content below the band). */
    fdk_size bn = {0, 0};
    size_t n = fdk_widget_child_count(w);
    fdk_widget *close_btn =
        (n > 0) ? fdk_widget_child_at(w, n - 1) : NULL;
    if (close_btn != NULL) {
        fdk_widget_measure(close_btn, &bn);
        fdk_i32 bx = assigned.width - CHOOSE_PAD - bn.width;
        if (bx < CHOOSE_PAD) {
            bx = CHOOSE_PAD;
        }
        fdk_i32 by = assigned.height - CHOOSE_PAD - bn.height;
        fdk_widget_set_bounds(close_btn,
                              (fdk_rect){bx, by, bn.width, bn.height});
    }

    /* The content stack, top-down from the padding. */
    fdk_i32 x = CHOOSE_PAD;
    fdk_i32 y = CHOOSE_PAD;
    fdk_i32 max_w = assigned.width - CHOOSE_PAD * 2;
    fdk_i32 bottom = assigned.height - CHOOSE_PAD -
                     (close_btn != NULL ? bn.height + CHOOSE_PAD : 0);

    if (a->logo != NULL) {
        fdk_i32 lw = 64, lh = 64;
        if (a->logo_img != NULL) {
            fdk_surface_info info;
            if (fdk_ok(fdk_surface_get_info(a->logo_img, &info))) {
                lw = info.width;
                lh = info.height;
            }
        }
        fdk_widget_set_bounds(a->logo,
                              (fdk_rect){x, y, lw, lh});
        /* The text column starts right of the logo. */
        y += 0;
    }

    fdk_i32 tx = x + (a->logo != NULL ? 72 : 0);
    fdk_i32 tmax = max_w - (a->logo != NULL ? 72 : 0);
    if (tmax < 1) {
        tmax = 1;
    }

    fdk_size nat = {0, 0};
    if (a->name_label != NULL) {
        fdk_widget_measure(a->name_label, &nat);
        fdk_widget_set_bounds(a->name_label,
                              (fdk_rect){tx, y, tmax, nat.height});
        y += nat.height + 4;
    }
    if (a->version_label != NULL) {
        fdk_widget_measure(a->version_label, &nat);
        fdk_widget_set_bounds(a->version_label,
                              (fdk_rect){tx, y, tmax, nat.height});
        y += nat.height + 8;
    }
    if (a->copyright_l != NULL) {
        fdk_widget_measure(a->copyright_l, &nat);
        fdk_widget_set_bounds(a->copyright_l,
                              (fdk_rect){tx, y, tmax, nat.height});
        y += nat.height + 8;
    }
    if (a->website_btn != NULL) {
        fdk_widget_measure(a->website_btn, &nat);
        fdk_widget_set_bounds(a->website_btn,
                              (fdk_rect){tx, y, nat.width, nat.height});
        y += nat.height + 8;
    }
    /* The comments wrap under the full width (below the logo row). */
    fdk_i32 full_y = (a->logo != NULL)
        ? CHOOSE_PAD + 72
        : y;
    if (full_y < y) {
        full_y = y;
    }
    if (a->comments != NULL) {
        fdk_i32 ch = 0;
        if (a->font != NULL) {
            size_t lines = 0;
            (void)fdk_font_break_lines_utf8(
                a->font, fdk_label_get_text(a->comments),
                strlen(fdk_label_get_text(a->comments)), max_w - 4,
                NULL, 0, &lines, NULL);
            fdk_font_metrics fm;
            fdk_font_get_metrics(a->font, &fm);
            ch = (fdk_i32)(lines * (size_t)(fm.ascent + fm.descent));
        }
        if (ch < 16) {
            ch = 16;
        }
        fdk_widget_set_bounds(a->comments,
                              (fdk_rect){x, full_y, max_w, ch});
        full_y += ch + 8;
    }
    /* The license block fills what remains (scrolling). */
    if (a->license_scroll != NULL) {
        fdk_i32 top = full_y;
        fdk_i32 h = bottom - top;
        if (h < 40) {
            h = 40;
        }
        fdk_widget_set_bounds(a->license_scroll,
                              (fdk_rect){x, top, max_w, h});
    }
}

static void about_body_destroy(fdk_widget *w) {
    fdk_about *a = about_of_body(w);
    if (a == NULL) {
        return;
    }
    ((fdk_about_body *)(void *)w)->about = NULL;
    if (a->logo_img != NULL) {
        fdk_surface_destroy(a->logo_img);
    }
    fdk_free(a->website);
    if (a->name_font != NULL && a->name_font != a->font) {
        fdk_font_destroy(a->name_font);
    }
    if (a->font != NULL) {
        fdk_font_destroy(a->font);
    }
    fdk_free(a);
}

static const fdk_a11y_class about_body_a11y = {
    .role = FDK_A11Y_ROLE_DIALOG,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

static const fdk_widget_class fdk_about_body_class = {
    .size = sizeof(fdk_about_body),
    .name = "about-body",
    .handle_event = NULL,
    .paint = NULL, /* base paint: the window_background fill */
    .measure = NULL,
    .arrange = about_body_arrange,
    .destroy = about_body_destroy,
    .a11y = &about_body_a11y,
};

fdk_result fdk_dialog_show_about(fdk_context *ctx,
                                 const fdk_about_dialog_options *options,
                                 fdk_dialog_response_fn on_response,
                                 fdk_about_website_fn on_website,
                                 void *user_data,
                                 fdk_window **out_window) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const char *name = (options != NULL && options->program_name != NULL)
        ? options->program_name
        : "About";
    const char *version =
        (options != NULL) ? options->version : NULL;
    const char *comments =
        (options != NULL) ? options->comments : NULL;
    const char *copyright =
        (options != NULL) ? options->copyright : NULL;
    const char *website =
        (options != NULL) ? options->website : NULL;
    const char *license =
        (options != NULL) ? options->license : NULL;
    const char *logo_path =
        (options != NULL) ? options->logo_path : NULL;
    bool modal = (options != NULL) && options->modal;

    fdk_about *a = fdk_alloc(sizeof(*a));
    if (a == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(a, 0, sizeof(*a));
    a->on_response = on_response;
    a->on_website = on_website;
    a->user_data = user_data;
    a->negative = FDK_DIALOG_CLOSE;
    a->modal = modal;
    a->font = fdk_font_load_system_default(CHOOSE_FONT_PX);
    a->name_font = a->font;
    if (website != NULL) {
        a->website = fdk__strdup(website);
    }

    /* ---- content sizing (before the window: its size follows) ---- */
    fdk_surface *logo_img = NULL;
    if (logo_path != NULL && logo_path[0] != '\0') {
        fdk_result r = fdk_surface_create_from_image(logo_path,
                                                     &logo_img);
        if (!fdk_ok(r)) {
            FDK_WARN("about dialog: logo '%s' failed to decode — "
                     "the dialog continues without it",
                     logo_path);
        }
    }
    a->logo_img = logo_img;

    size_t comment_lines = 0;
    if (comments != NULL && a->font != NULL) {
        (void)fdk_font_break_lines_utf8(a->font, comments,
                                        strlen(comments), 380, NULL,
                                        0, &comment_lines, NULL);
    }
    fdk_i32 line_h = 18;
    if (a->font != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(a->font, &fm);
        line_h = fm.ascent + fm.descent;
        if (line_h < 8) {
            line_h = 8;
        }
    }
    fdk_i32 text_h = line_h;             /* the name line          */
    if (version != NULL) {
        text_h += line_h + 4;
    }
    if (copyright != NULL) {
        text_h += line_h + 8;
    }
    if (website != NULL) {
        text_h += line_h + 8;
    }
    fdk_i32 logo_extent = (logo_img != NULL) ? 72 : 0;
    fdk_i32 head_h = (text_h > logo_extent) ? text_h : logo_extent;
    fdk_i32 comments_h = (fdk_i32)(comment_lines * (size_t)line_h) +
                         ((comment_lines > 0) ? 8 : 0);
    fdk_i32 license_h = (license != NULL) ? 9 * line_h + 8 : 0;
    fdk_i32 btn_h = line_h + 16;

    fdk_i32 width = 380 + CHOOSE_PAD * 2;
    fdk_i32 height = CHOOSE_PAD * 2 + head_h + comments_h +
                     license_h + btn_h + CHOOSE_PAD;
    if (height < 160) {
        height = 160;
    }

    fdk_window_options wopts = {
        .title = name,
        .width = width,
        /* 1.4.15: the FDK title bar (see dialog.c). */
        .height = height + fdk_theme_get_metric(NULL,
                                                FDK_TM_TITLE_BAR_HEIGHT),
    };
    fdk_window *win = NULL;
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        if (a->name_font != NULL && a->name_font != a->font) {
            fdk_font_destroy(a->name_font);
        }
        if (a->font != NULL) {
            fdk_font_destroy(a->font);
        }
        if (logo_img != NULL) {
            fdk_surface_destroy(logo_img);
        }
        fdk_free(a->website);
        fdk_free(a);
        return r;
    }
    a->window = win;
    fdk__window_set_auto_paint(win, true);
    (void)fdk_window_set_decorated(win, true);
    fdk_window_set_resizable(win, false); /* fixed-content dialog */
    fdk__window_set_destroy_notify(win, about_destroyed, a);
    fdk_window_set_event_callback(win, about_window_event, a);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(win, &root);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget *body = NULL;
    r = fdk_widget_create(root, &fdk_about_body_class,
                          (fdk_rect){0, 0, width, height}, &body);
    if (!fdk_ok(r)) {
        goto fail;
    }
    about_body_set(body, a);
    a->body = body;
    fdk_widget_set_accessible_name(body, name);
    fdk_widget_set_background(
        body, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    /* The logo canvas (only when the image decoded). */
    if (logo_img != NULL) {
        fdk_surface_info info;
        fdk_i32 lw = 64, lh = 64;
        if (fdk_ok(fdk_surface_get_info(logo_img, &info))) {
            lw = info.width;
            lh = info.height;
        }
        r = fdk_canvas_create(body, about_logo_paint, a, &a->logo);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_widget_set_natural_size(a->logo, lw, lh);
    }

    /* The name (a slightly larger face when the system font's file
     * can be re-loaded at 18px — the identity card's headline). */
    if (a->font != NULL) {
        const char *path = fdk_font_get_file_path(a->font);
        if (path != NULL) {
            fdk_font *bigger = fdk_font_load(path, 18);
            if (bigger != NULL) {
                a->name_font = bigger;
            }
        }
    }
    r = fdk_label_create(body, a->name_font, name, &a->name_label);
    if (!fdk_ok(r)) {
        goto fail;
    }

    if (version != NULL) {
        r = fdk_label_create(body, a->font, version, &a->version_label);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_label_set_color(a->version_label, fdk__pal_text_disabled());
    }
    if (copyright != NULL) {
        r = fdk_label_create(body, a->font, copyright,
                             &a->copyright_l);
        if (!fdk_ok(r)) {
            goto fail;
        }
    }
    if (website != NULL) {
        r = fdk_button_create(body, a->font, website, &a->website_btn);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_button_set_role(a->website_btn, FDK_BUTTON_ROLE_LINK);
        fdk_button_set_on_activate(a->website_btn, about_website_clicked,
                                   a);
    }
    if (comments != NULL) {
        r = fdk_label_create(body, a->font, comments, &a->comments);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_label_set_mode(a->comments, FDK_LABEL_WRAP);
    }
    if (license != NULL) {
        r = fdk_scrollview_create(body, &a->license_scroll);
        if (!fdk_ok(r)) {
            goto fail;
        }
        r = fdk_label_create(a->license_scroll, a->font, license,
                             &a->license_label);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_label_set_mode(a->license_label, FDK_LABEL_WRAP);
        (void)fdk_scrollview_set_content(a->license_scroll,
                                         a->license_label);
    }

    fdk_widget *close_btn = NULL;
    r = fdk_button_create(body, a->font, "Close", &close_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(close_btn, about_close_clicked, a);

    fdk_window_set_content(win, body);
    a->surfaced = true;
    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }
    if (close_btn != NULL) {
        fdk_widget_focus(close_btn);
    }
    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail:
    if (a->body == NULL) {
        if (a->name_font != NULL && a->name_font != a->font) {
            fdk_font_destroy(a->name_font);
        }
        if (a->font != NULL) {
            fdk_font_destroy(a->font);
        }
        if (logo_img != NULL) {
            fdk_surface_destroy(logo_img);
        }
        fdk_free(a->website);
        fdk_free(a);
    }
    fdk_window_destroy(win);
    return r;
}

/* ===================================================================
 * Font chooser dialog
 * =================================================================== */

#define FC_LIST_W 250
#define FC_PREVIEW_H 84
#define FC_MIN_SIZE 6
#define FC_MAX_SIZE 96

typedef struct fdk_font_chooser {
    fdk_window *window;
    fdk_widget *body;
    fdk_font *font;          /* UI font, owned system default  */
    fdk_font_info *infos;    /* enumerated faces, owned        */
    size_t info_count;
    size_t selected;         /* index into infos; -1 = none    */
    fdk_i32 size;            /* the spinner's value            */
    fdk_font *preview_font;  /* owned loaded face (or NULL)    */
    char *sample;            /* owned preview text             */
    fdk_widget *list;        /* the face list (self-scrolling)   */
    fdk_widget *size_label;  /* "Size:" static text            */
    fdk_widget *spin;        /* the size spinner               */
    fdk_widget *preview;     /* the live sample (a label)      */
    fdk_font_dialog_fn on_done;
    void *user_data;
    bool modal;
    bool responded;
    bool surfaced;
} fdk_font_chooser;

typedef struct fdk_fc_body {
    fdk_widget base;
    fdk_font_chooser *fc;   /* owned */
} fdk_fc_body;

static fdk_font_chooser *fc_of_body(fdk_widget *w) {
    return (w != NULL) ? ((fdk_fc_body *)(void *)w)->fc : NULL;
}

static void fc_respond(fdk_font_chooser *fc,
                       fdk_font_dialog_outcome outcome) {
    if (fc->responded) {
        return;
    }
    fc->responded = true;
    if (fc->on_done != NULL) {
        fdk_font_dialog_result result;
        memset(&result, 0, sizeof(result));
        result.outcome = outcome;
        if (outcome == FDK_FONT_DIALOG_ACCEPTED &&
            fc->selected != (size_t)-1 &&
            fc->selected < fc->info_count) {
            const fdk_font_info *info = &fc->infos[fc->selected];
            result.family = info->family;   /* FDK-owned, valid  */
            result.style = info->style;     /* during the call   */
            result.path = info->path;
            result.face_index = info->face_index;
            result.size = fc->size;
        }
        fc->on_done(&result, fc->user_data);
    }
    if (fc->window != NULL) {
        fdk_window *win = fc->window;
        fc->window = NULL;
        fdk_window_destroy(win);
    }
}

static void fc_window_event(fdk_window *window,
                            const fdk_event_data *ev, void *user) {
    (void)window;
    fdk_font_chooser *fc = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (ev->type == FDK_EVENT_KEY_DOWN &&
         ev->key.scancode == FDK_KEY_ESC)) {
        fc_respond(fc, FDK_FONT_DIALOG_CANCELLED);
    }
}

static void fc_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_font_chooser *fc = user;
    if (fc->responded || !fc->surfaced) {
        return;
    }
    fc->responded = true;
    if (fc->on_done != NULL) {
        fdk_font_dialog_result result;
        memset(&result, 0, sizeof(result));
        result.outcome = FDK_FONT_DIALOG_CANCELLED;
        fc->on_done(&result, fc->user_data);
    }
}

/* Rebuilds the preview label at the CURRENT face + size. The label
 * is destroyed and recreated because labels borrow their font at
 * creation — the only honest way to change it. */
static void fc_reload_preview(fdk_font_chooser *fc) {
    if (fc->preview != NULL) {
        fdk_widget_destroy(fc->preview);
        fc->preview = NULL;
    }
    if (fc->preview_font != NULL) {
        fdk_font_destroy(fc->preview_font);
        fc->preview_font = NULL;
    }
    fdk_font *pf = NULL;
    if (fc->selected != (size_t)-1 && fc->selected < fc->info_count) {
        const fdk_font_info *info = &fc->infos[fc->selected];
        pf = fdk_font_load_face(info->path, info->face_index, fc->size);
    }
    fc->preview_font = pf;
    fdk_widget *parent = fc->body;
    if (pf != NULL) {
        (void)fdk_label_create(parent, pf, fc->sample, &fc->preview);
    } else {
        /* The face failed to load at this size (or nothing is
         * selected): the sample renders in the UI font, dimmed —
         * the honest degrade, never a blank. */
        (void)fdk_label_create(parent, fc->font, fc->sample,
                               &fc->preview);
        fdk_label_set_color(fc->preview, fdk__pal_text_disabled());
    }
    if (fc->preview != NULL) {
        fdk_label_set_mode(fc->preview, FDK_LABEL_WRAP);
    }
    /* The body re-arranges (the preview slot follows). */
    if (fc->body != NULL) {
        fdk_widget_invalidate(fc->body);
        fdk_widget_child_layout_changed(fc->body->parent);
    }
}

static void fc_list_changed(fdk_widget *list, void *user) {
    (void)list;
    fdk_font_chooser *fc = user;
    fdk_i64 active = fdk_list_get_selected(list);
    if (active < 0 || (size_t)active >= fc->info_count) {
        return;
    }
    fc->selected = (size_t)active;
    fc_reload_preview(fc);
}

static void fc_spin_changed(fdk_widget *spin, void *user) {
    fdk_font_chooser *fc = user;
    double v = fdk_spin_get_value(spin);
    fdk_i32 want = (fdk_i32)(v + 0.5);
    if (want < FC_MIN_SIZE) {
        want = FC_MIN_SIZE;
    }
    if (want > FC_MAX_SIZE) {
        want = FC_MAX_SIZE;
    }
    if (want == fc->size) {
        return;
    }
    fc->size = want;
    fc_reload_preview(fc);
}

static void fc_ok_clicked(fdk_widget *button, void *user) {
    (void)button;
    fc_respond(user, FDK_FONT_DIALOG_ACCEPTED);
}

static void fc_cancel_clicked(fdk_widget *button, void *user) {
    (void)button;
    fc_respond(user, FDK_FONT_DIALOG_CANCELLED);
}

static void fc_body_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_font_chooser *fc = fc_of_body(w);
    if (fc == NULL) {
        return;
    }
    /* Buttons at the bottom-right (the house rule). */
    fdk_size ok_n = {0, 0}, cancel_n = {0, 0};
    size_t n = fdk_widget_child_count(w);
    fdk_widget *ok_btn = (n >= 2) ? fdk_widget_child_at(w, n - 2)
                                  : NULL;
    fdk_widget *cancel_btn = (n >= 1) ? fdk_widget_child_at(w, n - 1)
                                      : NULL;
    fdk_i32 bh = 0;
    if (ok_btn != NULL) {
        fdk_widget_measure(ok_btn, &ok_n);
        if (ok_n.height > bh) {
            bh = ok_n.height;
        }
    }
    if (cancel_btn != NULL) {
        fdk_widget_measure(cancel_btn, &cancel_n);
        if (cancel_n.height > bh) {
            bh = cancel_n.height;
        }
    }
    /* Children in the BODY's local coordinates (the 1.4.16
     * double-offset fix — see about_body_arrange). */
    fdk_i32 by = assigned.height - CHOOSE_PAD - bh;
    fdk_i32 x = assigned.width - CHOOSE_PAD;
    if (cancel_btn != NULL) {
        x -= cancel_n.width;
        fdk_widget_set_bounds(cancel_btn,
                              (fdk_rect){x, by, cancel_n.width, bh});
        x -= CHOOSE_BTN_GAP;
    }
    if (ok_btn != NULL) {
        x -= ok_n.width;
        fdk_widget_set_bounds(ok_btn,
                              (fdk_rect){x, by, ok_n.width, bh});
    }

    /* The list fills the left column, below the padding, above the
     * buttons (self-scrolling — the List's own scrollview rides
     * along). */
    fdk_i32 top = CHOOSE_PAD;
    fdk_i32 bottom = by - CHOOSE_PAD;
    if (bottom < top + 40) {
        bottom = top + 40;
    }
    if (fc->list != NULL) {
        fdk_widget_set_bounds(fc->list,
                              (fdk_rect){CHOOSE_PAD,
                                         top, FC_LIST_W,
                                         bottom - top});
    }
    /* The right column: size row at the top, preview below. */
    fdk_i32 rx = CHOOSE_PAD + FC_LIST_W + CHOOSE_PAD;
    fdk_i32 rw = assigned.width - CHOOSE_PAD - rx;
    if (rw < 60) {
        rw = 60;
    }
    fdk_size lab_n = {0, 0};
    fdk_i32 sy = top;
    if (fc->size_label != NULL) {
        fdk_widget_measure(fc->size_label, &lab_n);
        fdk_i32 spin_w = 84;
        fdk_widget_set_bounds(fc->size_label,
                              (fdk_rect){rx, sy + 4, lab_n.width,
                                         lab_n.height});
        if (fc->spin != NULL) {
            fdk_widget_set_bounds(
                fc->spin,
                (fdk_rect){rx + lab_n.width + 10, sy, spin_w,
                           lab_n.height + 4});
        }
        sy += lab_n.height + 4 + CHOOSE_PAD;
    }
    if (fc->preview != NULL) {
        fdk_i32 ph = bottom - sy;
        if (ph < FC_PREVIEW_H) {
            ph = FC_PREVIEW_H;
        }
        fdk_widget_set_bounds(fc->preview,
                              (fdk_rect){rx, sy, rw, ph});
    }
}

static void fc_body_destroy(fdk_widget *w) {
    fdk_font_chooser *fc = fc_of_body(w);
    if (fc == NULL) {
        return;
    }
    ((fdk_fc_body *)(void *)w)->fc = NULL;
    if (fc->preview_font != NULL) {
        fdk_font_destroy(fc->preview_font);
    }
    fdk_font_info_list_free(fc->infos, fc->info_count);
    fdk_free(fc->sample);
    if (fc->font != NULL) {
        fdk_font_destroy(fc->font);
    }
    fdk_free(fc);
}

static const fdk_a11y_class fc_body_a11y = {
    .role = FDK_A11Y_ROLE_FONT_CHOOSER,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

static const fdk_widget_class fdk_fc_body_class = {
    .size = sizeof(fdk_fc_body),
    .name = "font-chooser-body",
    .handle_event = NULL,
    .paint = NULL,
    .arrange = fc_body_arrange,
    .destroy = fc_body_destroy,
    .a11y = &fc_body_a11y,
};

fdk_result fdk_dialog_choose_font(fdk_context *ctx,
                                  const fdk_font_dialog_options *options,
                                  fdk_font_dialog_fn on_done,
                                  void *user_data,
                                  fdk_window **out_window) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const char *title = (options != NULL && options->title != NULL)
        ? options->title
        : "Select Font";
    const char *sample = (options != NULL && options->sample != NULL)
        ? options->sample
        : "The quick brown fox jumps over the lazy dog "
          "— 0123456789";
    const char *init_family =
        (options != NULL) ? options->initial_family : NULL;
    const char *init_style =
        (options != NULL) ? options->initial_style : NULL;
    fdk_i32 init_size = (options != NULL) ? options->initial_size : 0;
    bool modal = (options != NULL) && options->modal;

    fdk_font_chooser *fc = fdk_alloc(sizeof(*fc));
    if (fc == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(fc, 0, sizeof(*fc));
    fc->on_done = on_done;
    fc->user_data = user_data;
    fc->modal = modal;
    fc->selected = (size_t)-1;
    fc->size = (init_size >= FC_MIN_SIZE && init_size <= FC_MAX_SIZE)
        ? init_size
        : 12;
    fc->font = fdk_font_load_system_default(CHOOSE_FONT_PX);
    fc->sample = fdk__strdup(sample);
    if (fc->sample == NULL) {
        if (fc->font != NULL) {
            fdk_font_destroy(fc->font);
        }
        fdk_free(fc);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* The enumeration drives everything; an empty list still shows
     * the dialog (preview dimmed, OK refused) — an honest empty
     * beats an error box. */
    fdk_result r = fdk_font_enumerate(&fc->infos, &fc->info_count);
    if (!fdk_ok(r)) {
        fdk_free(fc->sample);
        if (fc->font != NULL) {
            fdk_font_destroy(fc->font);
        }
        fdk_free(fc);
        return r;
    }

    fdk_i32 width = 600;
    fdk_i32 height = 400;
    fdk_window_options wopts = {
        .title = title,
        .width = width,
        /* 1.4.15: the FDK title bar (see dialog.c). */
        .height = height + fdk_theme_get_metric(NULL,
                                                FDK_TM_TITLE_BAR_HEIGHT),
    };
    fdk_window *win = NULL;
    r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fc->window = win;
    fdk__window_set_auto_paint(win, true);
    (void)fdk_window_set_decorated(win, true);
    fdk_window_set_resizable(win, false); /* fixed-content dialog */
    fdk__window_set_destroy_notify(win, fc_destroyed, fc);
    fdk_window_set_event_callback(win, fc_window_event, fc);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(win, &root);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget *body = NULL;
    r = fdk_widget_create(root, &fdk_fc_body_class,
                          (fdk_rect){0, 0, width, height}, &body);
    if (!fdk_ok(r)) {
        goto fail;
    }
    ((fdk_fc_body *)(void *)body)->fc = fc;
    fc->body = body;
    fdk_widget_set_accessible_name(body, title);
    fdk_widget_set_background(
        body, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    /* The face list: single selection, one row per face. The List
     * carries its OWN scrollview (the self-contained rule) — placing
     * it directly in the body. */
    r = fdk_list_create(body, fc->font, &fc->list);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_list_set_selection_mode(fc->list, FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_selection_changed(fc->list, fc_list_changed, fc);
    size_t initial = (size_t)-1;
    for (size_t i = 0; i < fc->info_count; i++) {
        char row[256];
        snprintf(row, sizeof(row), "%s %s", fc->infos[i].family,
                 fc->infos[i].style);
        if (initial == (size_t)-1 && init_family != NULL &&
            strcmp(fc->infos[i].family, init_family) == 0 &&
            (init_style == NULL ||
             strcmp(fc->infos[i].style, init_style) == 0)) {
            initial = i;
        }
        (void)fdk_list_append(fc->list, row, NULL);
    }
    if (initial == (size_t)-1) {
        initial = 0; /* the first face, when nothing matches */
    }
    if (fc->info_count > 0) {
        (void)fdk_list_select(fc->list, initial);
        fc->selected = initial;
    }

    /* The size row. */
    r = fdk_label_create(body, fc->font, "Size:", &fc->size_label);
    if (!fdk_ok(r)) {
        goto fail;
    }
    r = fdk_spin_create(body, fc->font, FC_MIN_SIZE, FC_MAX_SIZE,
                        (double)fc->size, &fc->spin);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_spin_set_step(fc->spin, 1.0);
    fdk_spin_set_on_changed(fc->spin, fc_spin_changed, fc);

    /* The preview (created via the reload path so the initial face
     * loads through the same code every change takes). */
    fc_reload_preview(fc);

    fdk_widget *ok_btn = NULL;
    fdk_widget *cancel_btn = NULL;
    r = fdk_button_create(body, fc->font, "OK", &ok_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_role(ok_btn, FDK_BUTTON_ROLE_SUGGESTED);
    fdk_button_set_on_activate(ok_btn, fc_ok_clicked, fc);
    r = fdk_button_create(body, fc->font, "Cancel", &cancel_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(cancel_btn, fc_cancel_clicked, fc);

    fdk_window_set_content(win, body);
    fc->surfaced = true;
    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }
    if (fc->list != NULL) {
        fdk_widget_focus(fc->list);
    }
    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail:
    if (fc->body == NULL) {
        fdk_font_info_list_free(fc->infos, fc->info_count);
        fdk_free(fc->sample);
        if (fc->font != NULL) {
            fdk_font_destroy(fc->font);
        }
        fdk_free(fc);
    }
    fdk_window_destroy(win);
    return r;
}

/* ===================================================================
 * Color chooser dialog
 * =================================================================== */

#define CC_WHEEL 216          /* the canvas's square extent        */
#define CC_RING_OUT 104       /* the hue ring's outer radius      */
#define CC_RING_IN 82         /* the hue ring's inner radius      */
#define CC_TRI_R 74           /* the SV triangle's circumradius   */

typedef struct fdk_color_chooser {
    fdk_window *window;
    fdk_widget *body;
    fdk_font *font;          /* owned system default              */
    double h;                /* hue, degrees [0,360)              */
    double s;                /* saturation [0,1]                  */
    double v;                /* value [0,1]                       */
    fdk_color initial;       /* the options' starting color       */
    fdk_widget *wheel;       /* the canvas                        */
    fdk_widget *swatch_cur;  /* background widget                 */
    fdk_widget *swatch_init; /* background widget                 */
    fdk_widget *hex_entry;   /* NULL when !show_hex               */
    bool dragging;           /* a wheel drag holds the grab       */
    bool dragging_ring;      /* ring (vs triangle) region         */
    fdk_color_dialog_fn on_done;
    void *user_data;
    bool modal;
    bool responded;
    bool surfaced;
} fdk_color_chooser;

typedef struct fdk_cc_body {
    fdk_widget base;
    fdk_color_chooser *cc;  /* owned */
} fdk_cc_body;

static fdk_color_chooser *cc_of_body(fdk_widget *w) {
    return (w != NULL) ? ((fdk_cc_body *)(void *)w)->cc : NULL;
}

/* ---- HSV <-> RGB (the classic sector arithmetic) ---- */

static fdk_color cc_hsv_to_rgb(double h, double s, double v) {
    h = fmod(h, 360.0);
    if (h < 0.0) {
        h += 360.0;
    }
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    double c = v * s;
    double hp = h / 60.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    if (hp < 1.0)      { r = c; g = x; }
    else if (hp < 2.0) { r = x; g = c; }
    else if (hp < 3.0) { g = c; b = x; }
    else if (hp < 4.0) { g = x; b = c; }
    else if (hp < 5.0) { r = x; b = c; }
    else               { r = c; b = x; }
    double m = v - c;
    fdk_color out = {
        (fdk_f32)(r + m), (fdk_f32)(g + m), (fdk_f32)(b + m), 1.0f};
    return out;
}

static void cc_rgb_to_hsv(fdk_color c, double *out_h, double *out_s,
                          double *out_v) {
    double r = (c.r < 0) ? 0 : ((c.r > 1) ? 1 : c.r);
    double g = (c.g < 0) ? 0 : ((c.g > 1) ? 1 : c.g);
    double b = (c.b < 0) ? 0 : ((c.b > 1) ? 1 : c.b);
    double max = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
    double min = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
    double d = max - min;
    double h = 0.0;
    if (d > 1e-9) {
        if (max == r) {
            h = 60.0 * fmod(((g - b) / d), 6.0);
        } else if (max == g) {
            h = 60.0 * (((b - r) / d) + 2.0);
        } else {
            h = 60.0 * (((r - g) / d) + 4.0);
        }
    }
    if (h < 0.0) {
        h += 360.0;
    }
    *out_h = h;
    *out_s = (max > 1e-9) ? d / max : 0.0;
    *out_v = max;
}

static fdk_color cc_current_color(const fdk_color_chooser *cc) {
    return cc_hsv_to_rgb(cc->h, cc->s, cc->v);
}

/* The SV triangle's vertices (local canvas coords). */
static void cc_tri_vertices(double *ax, double *ay, double *bx,
                            double *by, double *cx, double *cy) {
    double c = CC_WHEEL / 2.0;
    *ax = c;
    *ay = c - CC_TRI_R;
    *bx = c - CC_TRI_R * 0.866025403784439;
    *by = c + CC_TRI_R * 0.5;
    *cx = c + CC_TRI_R * 0.866025403784439;
    *cy = c + CC_TRI_R * 0.5;
}

/* Pixel (local) -> triangle weights (a: hue vertex, b: black, c:
 * white). Returns false outside the triangle. */
static bool cc_tri_weights(double px, double py, double *wa,
                           double *wb, double *wc) {
    double ax, ay, bx, by, cx, cy;
    cc_tri_vertices(&ax, &ay, &bx, &by, &cx, &cy);
    double v0x = bx - ax, v0y = by - ay;
    double v1x = cx - ax, v1y = cy - ay;
    double v2x = px - ax, v2y = py - ay;
    double d00 = v0x * v0x + v0y * v0y;
    double d01 = v0x * v1x + v0y * v1y;
    double d11 = v1x * v1x + v1y * v1y;
    double d20 = v2x * v0x + v2y * v0y;
    double d21 = v2x * v1x + v2y * v1y;
    double denom = d00 * d11 - d01 * d01;
    if (fabs(denom) < 1e-12) {
        return false;
    }
    double wb_l = (d11 * d20 - d01 * d21) / denom;
    double wc_l = (d00 * d21 - d01 * d20) / denom;
    double wa_l = 1.0 - wb_l - wc_l;
    if (wa_l < -1e-9 || wb_l < -1e-9 || wc_l < -1e-9) {
        return false;
    }
    *wa = wa_l;
    *wb = wb_l;
    *wc = wc_l;
    return true;
}

/* State -> the SV marker's local position. */
static void cc_sv_pos(const fdk_color_chooser *cc, double *out_x,
                      double *out_y) {
    double ax, ay, bx, by, cx, cy;
    cc_tri_vertices(&ax, &ay, &bx, &by, &cx, &cy);
    double wa = cc->s * cc->v;
    double wb = 1.0 - cc->v;
    double wc = (1.0 - cc->s) * cc->v;
    *out_x = wa * ax + wb * bx + wc * cx;
    *out_y = wa * ay + wb * by + wc * cy;
}

/* The hue marker's local position (on the ring's mid-radius). */
static void cc_hue_pos(const fdk_color_chooser *cc, double *out_x,
                       double *out_y) {
    double c = CC_WHEEL / 2.0;
    double r = (CC_RING_IN + CC_RING_OUT) / 2.0;
    double rad = cc->h * 3.14159265358979323846 / 180.0;
    *out_x = c + r * cos(rad);
    *out_y = c - r * sin(rad);
}

/* ---- the wheel's paint: direct per-pixel rasterization ---------- */

static void cc_wheel_paint(fdk_widget *canvas, fdk_surface *surface,
                           fdk_rect bounds, fdk_rect clip,
                           void *user) {
    (void)canvas;
    fdk_color_chooser *cc = user;
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(surface, &info)) ||
        info.pixels == NULL) {
        return;
    }
    /* The rasterization loop runs over clip ∩ bounds (the raw
     * writes bypass the clip stack, so it is honored by hand). */
    fdk_i32 x0 = (clip.x > bounds.x) ? clip.x : bounds.x;
    fdk_i32 y0 = (clip.y > bounds.y) ? clip.y : bounds.y;
    fdk_i32 x1 = clip.x + clip.width;
    fdk_i32 y1 = clip.y + clip.height;
    if (x1 > bounds.x + bounds.width) {
        x1 = bounds.x + bounds.width;
    }
    if (y1 > bounds.y + bounds.height) {
        y1 = bounds.y + bounds.height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    double ctr = CC_WHEEL / 2.0;
    /* The pure hue color at the CURRENT hue (the triangle's tint). */
    fdk_color hue_c = cc_hsv_to_rgb(cc->h, 1.0, 1.0);
    fdk_u32 hue_px = 0xFF000000u |
                     ((fdk_u32)(hue_c.r * 255.0 + 0.5) << 16) |
                     ((fdk_u32)(hue_c.g * 255.0 + 0.5) << 8) |
                     (fdk_u32)(hue_c.b * 255.0 + 0.5);

    for (fdk_i32 y = y0; y < y1; y++) {
        fdk_u32 *row = info.pixels +
                       (size_t)y * (size_t)info.stride;
        for (fdk_i32 x = x0; x < x1; x++) {
            double lx = (double)x - (double)bounds.x;
            double ly = (double)y - (double)bounds.y;
            double dx = lx - ctr;
            double dy = ly - ctr;
            double r = sqrt(dx * dx + dy * dy);
            fdk_u32 px = 0;
            if (r >= CC_RING_IN && r <= CC_RING_OUT) {
                /* The hue ring: angle -> hue. */
                double ang =
                    atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
                if (ang < 0.0) {
                    ang += 360.0;
                }
                fdk_color c = cc_hsv_to_rgb(ang, 1.0, 1.0);
                px = 0xFF000000u |
                     ((fdk_u32)(c.r * 255.0 + 0.5) << 16) |
                     ((fdk_u32)(c.g * 255.0 + 0.5) << 8) |
                     (fdk_u32)(c.b * 255.0 + 0.5);
            } else if (r < CC_RING_IN + 2.0) {
                /* The SV triangle (weights -> s,v -> color). */
                double wa, wb, wc;
                if (cc_tri_weights(lx, ly, &wa, &wb, &wc)) {
                    double vv = wa + wc;
                    double ss = (vv > 1e-9) ? wa / vv : 0.0;
                    /* Anti-alias the triangle's edge: fade the last
                     * 1.5px of the weights' negative margin. */
                    fdk_color c = cc_hsv_to_rgb(cc->h, ss, vv);
                    px = 0xFF000000u |
                         ((fdk_u32)(c.r * 255.0 + 0.5) << 16) |
                         ((fdk_u32)(c.g * 255.0 + 0.5) << 8) |
                         (fdk_u32)(c.b * 255.0 + 0.5);
                } else {
                    /* Just outside: the hue-tinted rim so the
                     * triangle reads as cut from the ring's disc. */
                    px = hue_px;
                }
            }
            if (px != 0) {
                row[(size_t)x] = px;
            }
        }
    }

    /* The markers: a white dot with a dark ring reads on any color.
     * (draw after the rasterization; the primitives honor clips.) */
    fdk_i32 ox = bounds.x;
    fdk_i32 oy = bounds.y;
    {
        double mx, my;
        cc_hue_pos(cc, &mx, &my);
        fdk_i32 hx = ox + (fdk_i32)(mx + 0.5);
        fdk_i32 hy = oy + (fdk_i32)(my + 0.5);
        fdk_surface_fill_circle_aa(surface, hx, hy, 6,
                                   (fdk_color){1, 1, 1, 1});
        fdk_surface_draw_circle_aa(surface, hx, hy, 6,
                                   (fdk_color){0, 0, 0, 0.8f});
    }
    {
        double mx, my;
        cc_sv_pos(cc, &mx, &my);
        fdk_i32 sx = ox + (fdk_i32)(mx + 0.5);
        fdk_i32 sy = oy + (fdk_i32)(my + 0.5);
        fdk_surface_fill_circle_aa(surface, sx, sy, 5,
                                   (fdk_color){1, 1, 1, 1});
        fdk_surface_draw_circle_aa(surface, sx, sy, 5,
                                   (fdk_color){0, 0, 0, 0.8f});
    }
}

/* ---- the wheel's input ---- */

static void cc_apply_wheel_point(fdk_color_chooser *cc, fdk_f32 lx,
                                 fdk_f32 ly) {
    double ctr = CC_WHEEL / 2.0;
    double dx = (double)lx - ctr;
    double dy = (double)ly - ctr;
    double r = sqrt(dx * dx + dy * dy);
    if (cc->dragging_ring || r >= CC_RING_IN) {
        double ang =
            atan2(-dy, dx) * 180.0 / 3.14159265358979323846;
        if (ang < 0.0) {
            ang += 360.0;
        }
        cc->h = ang;
    } else {
        double wa, wb, wc;
        if (cc_tri_weights((double)lx, (double)ly, &wa, &wb, &wc)) {
            double vv = wa + wc;
            double ss = (vv > 1e-9) ? wa / vv : 0.0;
            cc->s = ss;
            cc->v = vv;
        }
    }
    /* The swatch + hex row follow the state. */
    if (cc->swatch_cur != NULL) {
        fdk_widget_set_background(cc->swatch_cur, cc_current_color(cc));
    }
    if (cc->hex_entry != NULL) {
        fdk_color c = cc_current_color(cc);
        char hex[10];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                 (unsigned)(c.r * 255.0f + 0.5f) & 0xFFu,
                 (unsigned)(c.g * 255.0f + 0.5f) & 0xFFu,
                 (unsigned)(c.b * 255.0f + 0.5f) & 0xFFu);
        (void)fdk_entry_set_text(cc->hex_entry, hex);
    }
    if (cc->wheel != NULL) {
        fdk_canvas_invalidate(cc->wheel);
    }
}

static bool cc_wheel_event(fdk_widget *w, const fdk_widget_event *ev,
                           void *user) {
    fdk_color_chooser *cc = user;
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN: {
        if (ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        double ctr = CC_WHEEL / 2.0;
        double dx = (double)ev->pointer.position.x - ctr;
        double dy = (double)ev->pointer.position.y - ctr;
        double r = sqrt(dx * dx + dy * dy);
        if (r > CC_RING_OUT + 8.0) {
            return false; /* outside the wheel entirely */
        }
        cc->dragging = true;
        cc->dragging_ring = (r >= CC_RING_IN);
        cc_apply_wheel_point(cc, ev->pointer.position.x,
                             ev->pointer.position.y);
        return true; /* the implicit grab takes it from here */
    }
    case FDK_WIDGET_POINTER_MOTION:
        if (!cc->dragging) {
            return false;
        }
        cc_apply_wheel_point(cc, ev->position.x, ev->position.y);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (!cc->dragging) {
            return false;
        }
        cc->dragging = false;
        return true;
    default:
        return false;
    }
    (void)w;
}

/* ---- the hex entry ---- */

static void cc_hex_activated(fdk_widget *entry, void *user) {
    (void)entry;
    fdk_color_chooser *cc = user;
    if (cc->hex_entry == NULL) {
        return;
    }
    const char *t = fdk_entry_get_text(cc->hex_entry);
    if (t == NULL) {
        return;
    }
    while (*t == ' ') {
        t++;
    }
    if (*t == '#') {
        t++;
    }
    size_t n = strlen(t);
    if (n < 6) {
        return; /* not a color: keep the current state */
    }
    long rv = strtol(t, NULL, 16);
    if (rv < 0) {
        return;
    }
    fdk_color c = {
        (fdk_f32)((double)((rv >> 16) & 0xFF) / 255.0),
        (fdk_f32)((double)((rv >> 8) & 0xFF) / 255.0),
        (fdk_f32)((double)(rv & 0xFF) / 255.0), 1.0f};
    cc_rgb_to_hsv(c, &cc->h, &cc->s, &cc->v);
    if (cc->swatch_cur != NULL) {
        fdk_widget_set_background(cc->swatch_cur, c);
    }
    if (cc->wheel != NULL) {
        fdk_canvas_invalidate(cc->wheel);
    }
}

/* ---- the dialog frame ---- */

static void cc_respond(fdk_color_chooser *cc,
                       fdk_color_dialog_outcome outcome) {
    if (cc->responded) {
        return;
    }
    cc->responded = true;
    if (cc->on_done != NULL) {
        fdk_color_dialog_result result;
        result.outcome = outcome;
        result.color = (outcome == FDK_COLOR_DIALOG_ACCEPTED)
            ? cc_current_color(cc)
            : cc->initial;
        cc->on_done(&result, cc->user_data);
    }
    if (cc->window != NULL) {
        fdk_window *win = cc->window;
        cc->window = NULL;
        fdk_window_destroy(win);
    }
}

static void cc_window_event(fdk_window *window,
                            const fdk_event_data *ev, void *user) {
    (void)window;
    fdk_color_chooser *cc = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (ev->type == FDK_EVENT_KEY_DOWN &&
         ev->key.scancode == FDK_KEY_ESC)) {
        cc_respond(cc, FDK_COLOR_DIALOG_CANCELLED);
    }
}

static void cc_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_color_chooser *cc = user;
    if (cc->responded || !cc->surfaced) {
        return;
    }
    cc->responded = true;
    if (cc->on_done != NULL) {
        fdk_color_dialog_result result;
        result.outcome = FDK_COLOR_DIALOG_CANCELLED;
        result.color = cc->initial;
        cc->on_done(&result, cc->user_data);
    }
}

static void cc_ok_clicked(fdk_widget *button, void *user) {
    (void)button;
    cc_respond(user, FDK_COLOR_DIALOG_ACCEPTED);
}

static void cc_cancel_clicked(fdk_widget *button, void *user) {
    (void)button;
    cc_respond(user, FDK_COLOR_DIALOG_CANCELLED);
}

static void cc_body_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_color_chooser *cc = cc_of_body(w);
    if (cc == NULL) {
        return;
    }
    /* Buttons at the bottom-right (the house rule). */
    fdk_size ok_n = {0, 0}, cancel_n = {0, 0};
    size_t n = fdk_widget_child_count(w);
    fdk_widget *ok_btn = (n >= 2) ? fdk_widget_child_at(w, n - 2)
                                  : NULL;
    fdk_widget *cancel_btn = (n >= 1) ? fdk_widget_child_at(w, n - 1)
                                      : NULL;
    fdk_i32 bh = 0;
    if (ok_btn != NULL) {
        fdk_widget_measure(ok_btn, &ok_n);
        if (ok_n.height > bh) {
            bh = ok_n.height;
        }
    }
    if (cancel_btn != NULL) {
        fdk_widget_measure(cancel_btn, &cancel_n);
        if (cancel_n.height > bh) {
            bh = cancel_n.height;
        }
    }
    /* Children in the BODY's local coordinates (the 1.4.16
     * double-offset fix — the wheel rode 28px low under the 1.4.15
     * band; caught by the chooser's own pixel test). */
    fdk_i32 by = assigned.height - CHOOSE_PAD - bh;
    fdk_i32 x = assigned.width - CHOOSE_PAD;
    if (cancel_btn != NULL) {
        x -= cancel_n.width;
        fdk_widget_set_bounds(cancel_btn,
                              (fdk_rect){x, by, cancel_n.width, bh});
        x -= CHOOSE_BTN_GAP;
    }
    if (ok_btn != NULL) {
        x -= ok_n.width;
        fdk_widget_set_bounds(ok_btn,
                              (fdk_rect){x, by, ok_n.width, bh});
    }

    /* The wheel on the left; the side column on the right. */
    fdk_i32 top = CHOOSE_PAD;
    if (cc->wheel != NULL) {
        fdk_widget_set_bounds(
            cc->wheel,
            (fdk_rect){CHOOSE_PAD, top, CC_WHEEL,
                       CC_WHEEL});
    }
    fdk_i32 rx = CHOOSE_PAD + CC_WHEEL + CHOOSE_PAD;
    fdk_i32 rw = assigned.width - CHOOSE_PAD - rx;
    if (rw < 90) {
        rw = 90;
    }
    fdk_i32 sy = top;
    fdk_i32 sw_h = 44;
    if (cc->swatch_cur != NULL) {
        fdk_widget_set_bounds(cc->swatch_cur,
                              (fdk_rect){rx, sy, rw, sw_h});
        sy += sw_h + 6;
    }
    if (cc->swatch_init != NULL) {
        fdk_widget_set_bounds(cc->swatch_init,
                              (fdk_rect){rx, sy, rw, sw_h / 2});
        sy += sw_h / 2 + 10;
    }
    if (cc->hex_entry != NULL) {
        fdk_size en = {0, 0};
        fdk_widget_measure(cc->hex_entry, &en);
        fdk_widget_set_bounds(cc->hex_entry,
                              (fdk_rect){rx, sy, rw, en.height});
        sy += en.height + 8;
    }
}

static void cc_body_destroy(fdk_widget *w) {
    fdk_color_chooser *cc = cc_of_body(w);
    if (cc == NULL) {
        return;
    }
    ((fdk_cc_body *)(void *)w)->cc = NULL;
    if (cc->font != NULL) {
        fdk_font_destroy(cc->font);
    }
    fdk_free(cc);
}

static const fdk_a11y_class cc_body_a11y = {
    .role = FDK_A11Y_ROLE_COLOR_CHOOSER,
    .describe = NULL,
    .actions = NULL,
    .perform = NULL,
};

static const fdk_widget_class fdk_cc_body_class = {
    .size = sizeof(fdk_cc_body),
    .name = "color-chooser-body",
    .handle_event = NULL,
    .paint = NULL,
    .arrange = cc_body_arrange,
    .destroy = cc_body_destroy,
    .a11y = &cc_body_a11y,
};

fdk_result fdk_dialog_choose_color(fdk_context *ctx,
                                   const fdk_color_dialog_options *options,
                                   fdk_color_dialog_fn on_done,
                                   void *user_data,
                                   fdk_window **out_window) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const char *title = (options != NULL && options->title != NULL)
        ? options->title
        : "Select Color";
    fdk_color initial = (options != NULL) ? options->initial
                                          : (fdk_color){0, 0, 0, 1};
    if (initial.a <= 0.0f) {
        initial.a = 1.0f; /* a color dialog's colors are opaque */
    }
    bool show_hex = (options == NULL) || options->show_hex;
    bool modal = (options != NULL) && options->modal;

    fdk_color_chooser *cc = fdk_alloc(sizeof(*cc));
    if (cc == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(cc, 0, sizeof(*cc));
    cc->on_done = on_done;
    cc->user_data = user_data;
    cc->modal = modal;
    cc->initial = initial;
    cc->font = fdk_font_load_system_default(CHOOSE_FONT_PX);
    cc_rgb_to_hsv(initial, &cc->h, &cc->s, &cc->v);

    fdk_i32 width = CHOOSE_PAD * 2 + CC_WHEEL + 140;
    fdk_i32 height = CHOOSE_PAD * 2 + CC_WHEEL + 52;
    fdk_window_options wopts = {
        .title = title,
        .width = width,
        /* 1.4.15: the FDK title bar (see dialog.c). */
        .height = height + fdk_theme_get_metric(NULL,
                                                FDK_TM_TITLE_BAR_HEIGHT),
    };
    fdk_window *win = NULL;
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        if (cc->font != NULL) {
            fdk_font_destroy(cc->font);
        }
        fdk_free(cc);
        return r;
    }
    cc->window = win;
    fdk__window_set_auto_paint(win, true);
    (void)fdk_window_set_decorated(win, true);
    fdk_window_set_resizable(win, false); /* fixed-content dialog */
    fdk__window_set_destroy_notify(win, cc_destroyed, cc);
    fdk_window_set_event_callback(win, cc_window_event, cc);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(win, &root);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget *body = NULL;
    r = fdk_widget_create(root, &fdk_cc_body_class,
                          (fdk_rect){0, 0, width, height}, &body);
    if (!fdk_ok(r)) {
        goto fail;
    }
    ((fdk_cc_body *)(void *)body)->cc = cc;
    cc->body = body;
    fdk_widget_set_accessible_name(body, title);
    fdk_widget_set_background(
        body, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    /* The wheel canvas with its raster paint + drag input. */
    r = fdk_canvas_create(body, cc_wheel_paint, cc, &cc->wheel);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget_set_natural_size(cc->wheel, CC_WHEEL, CC_WHEEL);
    fdk_widget_set_event_callback(cc->wheel, cc_wheel_event, cc);

    /* The swatches: plain background widgets (the current state and
     * the initial color side by side for comparison). */
    r = fdk_widget_create(body, NULL, (fdk_rect){0, 0, 90, 44},
                          &cc->swatch_cur);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget_set_background(cc->swatch_cur, cc_current_color(cc));
    fdk_widget_set_corner_radius(cc->swatch_cur, 6);
    fdk_widget_set_accessible_name(cc->swatch_cur, "Current color");
    r = fdk_widget_create(body, NULL, (fdk_rect){0, 0, 90, 22},
                          &cc->swatch_init);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget_set_background(cc->swatch_init, initial);
    fdk_widget_set_corner_radius(cc->swatch_init, 6);
    fdk_widget_set_accessible_name(cc->swatch_init, "Initial color");

    if (show_hex) {
        r = fdk_entry_create(body, cc->font, "", &cc->hex_entry);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_entry_set_on_activate(cc->hex_entry, cc_hex_activated, cc);
        fdk_color c = cc_current_color(cc);
        char hex[10];
        snprintf(hex, sizeof(hex), "#%02x%02x%02x",
                 (unsigned)(c.r * 255.0f + 0.5f) & 0xFFu,
                 (unsigned)(c.g * 255.0f + 0.5f) & 0xFFu,
                 (unsigned)(c.b * 255.0f + 0.5f) & 0xFFu);
        (void)fdk_entry_set_text(cc->hex_entry, hex);
    }

    fdk_widget *ok_btn = NULL;
    fdk_widget *cancel_btn = NULL;
    r = fdk_button_create(body, cc->font, "OK", &ok_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_role(ok_btn, FDK_BUTTON_ROLE_SUGGESTED);
    fdk_button_set_on_activate(ok_btn, cc_ok_clicked, cc);
    r = fdk_button_create(body, cc->font, "Cancel", &cancel_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(cancel_btn, cc_cancel_clicked, cc);

    fdk_window_set_content(win, body);
    cc->surfaced = true;
    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }
    if (ok_btn != NULL) {
        fdk_widget_focus(ok_btn);
    }
    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail:
    if (cc->body == NULL) {
        if (cc->font != NULL) {
            fdk_font_destroy(cc->font);
        }
        fdk_free(cc);
    }
    fdk_window_destroy(win);
    return r;
}

/* ===================================================================
 * Color button (1.4.4) — the color-well swatch
 * =================================================================== */

#define CB_W 44
#define CB_H 30
#define CB_INSET 5      /* the well's frame inside the button face  */
#define CB_CHECKER 6    /* the alpha checkerboard's cell size       */

typedef struct fdk_colorbutton {
    fdk_widget base;
    fdk_color color;         /* the well's current color             */
    char *title;             /* owned; the chooser's title (or NULL) */
    bool pressed;            /* pointer down inside                  */
    bool hovering;           /* semantic hover flag                  */
    fdk_hover_fade fade;     /* the 1.4.1 hover paint blend          */
    fdk_color_button_fn on_set;
    void *on_set_data;
    /* The live chooser's token (NULL when none is up): the dialog's
     * done callback and the button's destroy hook share it, so a
     * button destroyed while its chooser is open can never dangle
     * into the callback. */
    struct fdk_cb_pending *pending;
} fdk_colorbutton;

/* The heap token bridging the chooser's done callback and a button
 * that may die before the chooser answers. Freed exactly once, by
 * whichever of the two fires last. */
typedef struct fdk_cb_pending {
    fdk_widget *button;  /* NULLed by the button's destroy hook */
} fdk_cb_pending;

static fdk_colorbutton *cbtn_of(fdk_widget *w) {
    return (fdk_colorbutton *)(void *)w;
}

extern const fdk_widget_class fdk_colorbutton_class_def;

/* The core acceptance path (the dialog's done callback after the
 * token check, and the headless test seam). */
static void cbtn_apply_result(fdk_widget *w,
                              const fdk_color_dialog_result *res) {
    fdk_colorbutton *cb = cbtn_of(w);
    if (res == NULL || res->outcome != FDK_COLOR_DIALOG_ACCEPTED) {
        return; /* CANCELLED: the well keeps its color */
    }
    cb->color = res->color;
    fdk_widget_invalidate(w);
    fdk__a11y_notify(w, FDK_A11Y_VALUE_CHANGED, 0);
    if (cb->on_set != NULL) {
        /* The app callback may destroy the button (or anything
         * else) — nothing is touched after it returns. */
        cb->on_set(w, res->color, cb->on_set_data);
    }
}

static void cbtn_dialog_done(const fdk_color_dialog_result *res,
                             void *user) {
    fdk_cb_pending *tok = user;
    fdk_widget *w = tok->button;
    if (w != NULL && (w->flags & FDK_WF_DESTROYING) == 0) {
        cbtn_of(w)->pending = NULL; /* before the app callback */
        cbtn_apply_result(w, res);
    }
    fdk_free(tok);
}

static void cbtn_open_chooser(fdk_widget *w) {
    fdk_colorbutton *cb = cbtn_of(w);
    if (cb->pending != NULL) {
        return; /* one chooser at a time */
    }
    fdk_context *ctx =
        fdk__window_context(fdk__widget_window_owner(w));
    if (ctx == NULL) {
        return; /* detached tree: documented no-op */
    }
    fdk_cb_pending *tok = fdk_alloc(sizeof(*tok));
    if (tok == NULL) {
        return; /* OOM: no chooser this press (honest) */
    }
    tok->button = w;
    fdk_color_dialog_options opts;
    memset(&opts, 0, sizeof(opts));
    opts.title = cb->title;
    opts.initial = cb->color;
    opts.show_hex = true;
    opts.modal = true;
    opts.parent = fdk__window_of_owner(fdk__widget_window_owner(w));
    fdk_result r = fdk_dialog_choose_color(ctx, &opts, cbtn_dialog_done,
                                           tok, NULL);
    if (!fdk_ok(r)) {
        fdk_free(tok);
        return;
    }
    cb->pending = tok;
}

static bool cbtn_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_colorbutton *cb = cbtn_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_DOWN:
        if ((w->flags & FDK_WF_ENABLED) == 0 ||
            ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return false;
        }
        if (!fdk_widget_has_focus(w)) {
            (void)fdk_widget_focus(w);
        }
        cb->pressed = true;
        fdk_widget_invalidate(w);
        return true;
    case FDK_WIDGET_POINTER_UP:
        if (cb->pressed) {
            cb->pressed = false;
            fdk_widget_invalidate(w);
            /* The classic press-inside-release-inside rule. */
            cbtn_open_chooser(w);
        }
        return true;
    case FDK_WIDGET_KEY_DOWN:
        if ((w->flags & FDK_WF_ENABLED) != 0 &&
            (ev->key.scancode == FDK_KEY_SPACE ||
             ev->key.scancode == FDK_KEY_ENTER)) {
            cbtn_open_chooser(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_ENTER:
        cb->hovering = true;
        fdk__hover_fade_arm(w, &cb->fade, true);
        fdk_widget_invalidate(w);
        return false;
    case FDK_WIDGET_POINTER_LEAVE:
        cb->hovering = false;
        fdk__hover_fade_arm(w, &cb->fade, false);
        fdk_widget_invalidate(w);
        return false;
    default:
        return false;
    }
}

/* The checkerboard under translucent colors: the classic two-tone
 * proof-of-alpha board, in fixed neutrals (it represents the ALPHA
 * channel, not a theme surface). */
static void cbtn_paint_checker(fdk_surface *surface, fdk_rect well) {
    fdk_color a = {0.92f, 0.92f, 0.92f, 1.0f};
    fdk_color b = {0.78f, 0.78f, 0.78f, 1.0f};
    fdk_surface_fill_rect(surface, well, a);
    for (fdk_i32 row = 0; row * CB_CHECKER < well.height; row++) {
        fdk_i32 y = row * CB_CHECKER;
        fdk_i32 h = CB_CHECKER;
        if (y + h > well.height) {
            h = well.height - y;
        }
        for (fdk_i32 col = (row % 2); col * CB_CHECKER < well.width;
             col += 2) {
            fdk_i32 x = col * CB_CHECKER;
            fdk_i32 cw = CB_CHECKER;
            if (x + cw > well.width) {
                cw = well.width - x;
            }
            fdk_rect cell = {well.x + x, well.y + y, cw, h};
            if (cell.width > 0 && cell.height > 0) {
                fdk_surface_fill_rect(surface, cell, b);
            }
        }
    }
}

static void cbtn_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_colorbutton *cb = cbtn_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    /* The button face: control family, the hover blend riding the
     * 1.4.1 machinery, pressed snapping (presses never fade). */
    fdk_color face = fdk__pal_control();
    if (cb->fade.t > 0.0f) {
        fdk_color hover = fdk__pal_control_hover();
        face.r = face.r + (hover.r - face.r) * cb->fade.t;
        face.g = face.g + (hover.g - face.g) * cb->fade.t;
        face.b = face.b + (hover.b - face.b) * cb->fade.t;
        face.a = face.a + (hover.a - face.a) * cb->fade.t;
    }
    if (cb->pressed) {
        face = fdk__pal_control_pressed();
    }
    if ((w->flags & FDK_WF_ENABLED) == 0) {
        face = fdk__pal_control_disabled();
    }
    fdk_i32 radius =
        fdk_theme_get_metric(NULL, FDK_TM_BUTTON_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius, face);
    if ((w->flags & FDK_WF_ENABLED) != 0) {
        fdk_surface_draw_rounded_rect(surface, bounds, radius,
                                      fdk__pal_border());
    }

    /* The well: the checkerboard (the alpha proof) under the color.
     * A fully transparent color shows the board alone. */
    fdk_rect well = {bounds.x + CB_INSET, bounds.y + CB_INSET,
                     bounds.width - CB_INSET * 2,
                     bounds.height - CB_INSET * 2};
    if (well.width <= 0 || well.height <= 0) {
        return;
    }
    cbtn_paint_checker(surface, well);
    if (cb->color.a > 0.0f) {
        fdk_color c = cb->color;
        if ((w->flags & FDK_WF_ENABLED) == 0) {
            /* Dimmed well: the color desaturates toward the face. */
            c.r = c.r * 0.5f + face.r * 0.5f;
            c.g = c.g * 0.5f + face.g * 0.5f;
            c.b = c.b * 0.5f + face.b * 0.5f;
        }
        fdk_surface_fill_rounded_rect(surface, well, 3, c);
    }
    fdk_surface_draw_rounded_rect(surface, well, 3, fdk__pal_border());
}

static void cbtn_measure(fdk_widget *w, fdk_size *out) {
    (void)w;
    out->width = CB_W;
    out->height = CB_H;
}

static void cbtn_destroy(fdk_widget *w) {
    fdk_colorbutton *cb = cbtn_of(w);
    fdk_free(cb->title);
    cb->title = NULL;
    if (cb->pending != NULL) {
        /* The chooser is still up: the token survives until its done
         * callback frees it, but it must stop pointing here. */
        cb->pending->button = NULL;
        cb->pending = NULL;
    }
}

/* ---- a11y: a BUTTON whose value is the color ---- */

static void cbtn_a11y_describe(const fdk_widget *w, fdk_a11y_info *out) {
    const fdk_colorbutton *cb = (const fdk_colorbutton *)(const void *)w;
    fdk_color c = cb->color;
    fdk_u32 r = (fdk_u32)(c.r * 255.0f + 0.5f);
    fdk_u32 g = (fdk_u32)(c.g * 255.0f + 0.5f);
    fdk_u32 b = (fdk_u32)(c.b * 255.0f + 0.5f);
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    char buf[16];
    (void)snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                   (unsigned)r, (unsigned)g, (unsigned)b);
    out->value_text = fdk__strdup(buf);
    out->has_value = true;
    out->value_min = 0.0;
    out->value_max = 0.0;
    out->value_current = 0.0;
}

static fdk_a11y_action_set cbtn_a11y_actions(const fdk_widget *w) {
    (void)w;
    return (fdk_a11y_action_set)FDK_A11Y_ACTION_ACTIVATE;
}

static bool cbtn_a11y_perform(fdk_widget *w, fdk_a11y_action action,
                              double value) {
    (void)value;
    if (action != FDK_A11Y_ACTION_ACTIVATE) {
        return false;
    }
    cbtn_open_chooser(w);
    return true;
}

static const fdk_a11y_class cbtn_a11y = {
    .role = FDK_A11Y_ROLE_BUTTON,
    .describe = cbtn_a11y_describe,
    .actions = cbtn_a11y_actions,
    .perform = cbtn_a11y_perform,
};

const fdk_widget_class fdk_colorbutton_class_def = {
    .size = sizeof(fdk_colorbutton),
    .name = "colorbutton",
    .handle_event = cbtn_handle_event,
    .paint = cbtn_paint,
    .measure = cbtn_measure,
    .arrange = NULL,
    .destroy = cbtn_destroy,
    .a11y = &cbtn_a11y,
};

/* ---- public API ---- */

fdk_result fdk_color_button_create(fdk_widget *parent, fdk_color color,
                                  fdk_widget **out_button) {
    if (out_button == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_widget *w = NULL;
    fdk_result r = fdk_widget_create(parent, &fdk_colorbutton_class_def,
                                     (fdk_rect){0, 0, CB_W, CB_H}, &w);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_colorbutton *cb = cbtn_of(w);
    if (color.a < 0.0f) {
        color.a = 0.0f;
    }
    cb->color = color;
    fdk_widget_set_can_focus(w, true);
    fdk_widget_child_layout_changed(w->parent);
    *out_button = w;
    return FDK_OK;
}

fdk_color fdk_color_button_get_color(fdk_widget *button) {
    if (button == NULL || button->klass != &fdk_colorbutton_class_def) {
        return (fdk_color){0, 0, 0, 0};
    }
    return cbtn_of(button)->color;
}

fdk_result fdk_color_button_set_color(fdk_widget *button, fdk_color color) {
    if (button == NULL || button->klass != &fdk_colorbutton_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_colorbutton *cb = cbtn_of(button);
    if (color.a < 0.0f) {
        color.a = 0.0f;
    }
    if (cb->color.r == color.r && cb->color.g == color.g &&
        cb->color.b == color.b && cb->color.a == color.a) {
        return FDK_OK; /* idempotent */
    }
    cb->color = color;
    fdk_widget_invalidate(button);
    fdk__a11y_notify(button, FDK_A11Y_VALUE_CHANGED, 0);
    return FDK_OK;
}

fdk_result fdk_color_button_set_title(fdk_widget *button, const char *title) {
    if (button == NULL || button->klass != &fdk_colorbutton_class_def) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_colorbutton *cb = cbtn_of(button);
    char *copy = (title != NULL) ? fdk__strdup(title) : NULL;
    if (title != NULL && copy == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    fdk_free(cb->title);
    cb->title = copy;
    return FDK_OK;
}

void fdk_color_button_set_on_color_set(fdk_widget *button,
                                       fdk_color_button_fn fn,
                                       void *user_data) {
    if (button == NULL || button->klass != &fdk_colorbutton_class_def) {
        return;
    }
    fdk_colorbutton *cb = cbtn_of(button);
    cb->on_set = fn;
    cb->on_set_data = user_data;
}

/* Internal test seam (widgets_internal.h): drives the acceptance
 * path exactly as the dialog's done callback would (the token hop
 * is the only difference, and the X11 GUI group covers that half). */
void fdk__colorbutton_apply_result(fdk_widget *button,
                                   const fdk_color_dialog_result *res) {
    if (button == NULL || button->klass != &fdk_colorbutton_class_def) {
        return;
    }
    cbtn_apply_result(button, res);
}
