#define FDK_LOG_TAG "widgets"

/*
 * dialog.c — message dialogs (Phase 9 completion)
 *
 * A dialog is a small decorated toplevel built from the stock
 * catalog: a wrapping Label over a row of Buttons. The toolkit owns
 * the whole lifecycle — auto-paint (the window layer repaints it on
 * every event's damage; the app only pumps), one response callback,
 * self-destruction afterwards. Modality on X11 is a pointer+keyboard
 * grab on the dialog (fdk__window_set_modal; presses outside arrive
 * out-of-bounds and hit-test nothing — input waits for the dialog);
 * Wayland has no toplevel-grab protocol, so dialogs there are
 * non-modal, documented rather than faked.
 *
 * Keyboard: the affirmative button takes the initial focus (Enter
 * activates it through the stock Button contract); Escape reaches
 * the window callback (no widget consumes it) and answers the
 * negative response; Tab walks the buttons. The WM's close button
 * answers the negative response too.
 *
 * Reentrancy: the response path (button callback, Escape, close
 * request, early destroy) fires the app's on_response FIRST (the
 * dialog is still whole), then destroys the window — the
 * destroy-notify guards a double response, and the widget/window
 * layers' deferred-destroy machinery makes destroying the dialog
 * from inside its own dispatch safe. The fdk_dialog struct rides on
 * the body widget's subclass allocation: it dies with the window's
 * tree, through any teardown path.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_dialog.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <string.h>

#define DLG_PAD 24        /* outer padding                      */
#define DLG_GAP 18        /* label-to-buttons gap               */
#define DLG_BTN_GAP 10    /* between buttons                    */
#define DLG_WRAP 400      /* label wrap width                   */
#define DLG_FONT_PX 15    /* system-default font size           */
#define DLG_BTN_MIN_W 80  /* minimum button width               */
#define DLG_BTN_PAD 32    /* button text padding (both sides)   */

typedef struct fdk_dialog {
    fdk_window *window;      /* owned lifecycle (self-destroying) */
    fdk_widget *body;        /* the arrange-hooked content widget */
    fdk_widget *label;       /* wrapping Label                    */
    fdk_font *font;          /* effective (borrowed or owned)     */
    fdk_font *font_owned;    /* the system default we loaded      */
    fdk_dialog_response_fn on_response;
    void *on_response_user;
    fdk_dialog_response negative;    /* Escape / WM-close answer  */
    fdk_dialog_response affirmative; /* initial focus / Enter     */
    bool modal;               /* a11y state + X11 grab request      */
    struct dialog_button_ctx {
        struct fdk_dialog *d;
        fdk_dialog_response response;
    } *button_ctxs;          /* referenced by the buttons         */
    size_t button_ctx_count;
    bool responded;
} fdk_dialog;

/* The body widget: a plain background fill whose subclass storage
 * owns the fdk_dialog and whose arrange hook lays out label+buttons
 * whenever the window configures (fixed-size in practice — WMs are
 * asked nothing; a resize just re-flows). */
typedef struct fdk_dialog_body {
    fdk_widget base;
    fdk_dialog *dialog; /* owned */
} fdk_dialog_body;

static fdk_dialog_body *body_of(fdk_widget *w) {
    return (fdk_dialog_body *)(void *)w;
}

static fdk_dialog *dialog_of_body(fdk_widget *w) {
    return (w != NULL) ? body_of(w)->dialog : NULL;
}

static void dialog_body_a11y_describe(const fdk_widget *w,
                                      fdk_a11y_info *out) {
    const fdk_dialog *d =
        (w != NULL) ? ((const fdk_dialog_body *)(const void *)w)->dialog
                    : NULL;
    if (d != NULL && d->modal) {
        out->states |= FDK_A11Y_MODAL;
    }
}

/* ---- response path ---- */

static void dialog_respond(fdk_dialog *d, fdk_dialog_response response) {
    if (d->responded) {
        return;
    }
    d->responded = true;
    /* The callback runs while the dialog is still whole; destroying
     * anything (including the parent window, whose popup/child sweep
     * may take this dialog with it) is legal. */
    if (d->on_response != NULL) {
        d->on_response(response, d->on_response_user);
    }
    if (d->window != NULL) {
        fdk_window *win = d->window;
        d->window = NULL;
        fdk_window_destroy(win); /* releases the modal grab too */
    }
}

/* The dialog window's event callback: window-level semantics only —
 * everything the widgets handle (button clicks, Enter on the focused
 * button, Tab walking) is consumed before this runs. */
static void dialog_window_event(fdk_window *window,
                                const fdk_event_data *ev, void *user) {
    (void)window;
    fdk_dialog *d = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        dialog_respond(d, d->negative);
        return; /* the window is gone; dispatch's checks handle it */
    }
    if (ev->type == FDK_EVENT_KEY_DOWN &&
        ev->key.scancode == FDK_KEY_ESC) {
        dialog_respond(d, d->negative);
    }
}

/* Destroy-notify: the dialog died without answering (early app
 * destroy, parent window teardown, shutdown). The documented
 * "destroying it early answers the negative response" contract. The
 * callback must NOT destroy the window (already dying). */
static void dialog_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_dialog *d = user;
    if (d->responded) {
        return;
    }
    d->responded = true;
    if (d->on_response != NULL) {
        d->on_response(d->negative, d->on_response_user);
    }
}

/* ---- body hooks ---- */

static void dialog_body_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    fdk_dialog *d = dialog_of_body(w);
    if (d == NULL) {
        return;
    }
    /* Buttons at the bottom-right, label filling the rest. Child 0
     * is the label; children 1.. are the buttons. */
    fdk_i32 bw = 0, bh = 0;
    size_t n = fdk_widget_child_count(w);
    for (size_t i = 1; i < n; i++) {
        fdk_widget *c = fdk_widget_child_at(w, i);
        fdk_size nat = {0, 0};
        fdk_widget_measure(c, &nat);
        bw += nat.width + DLG_BTN_GAP;
        if (nat.height > bh) {
            bh = nat.height;
        }
    }
    if (bw > 0) {
        bw -= DLG_BTN_GAP;
    }
    fdk_i32 x = assigned.x + assigned.width - DLG_PAD - bw;
    if (x < assigned.x + DLG_PAD) {
        x = assigned.x + DLG_PAD;
    }
    fdk_i32 y = assigned.y + assigned.height - DLG_PAD - bh;
    for (size_t i = 1; i < n; i++) {
        fdk_widget *c = fdk_widget_child_at(w, i);
        fdk_size nat = {0, 0};
        fdk_widget_measure(c, &nat);
        fdk_widget_set_bounds(c, (fdk_rect){x, y, nat.width, bh});
        x += nat.width + DLG_BTN_GAP;
    }
    if (d->label != NULL) {
        fdk_i32 lw = assigned.width - DLG_PAD * 2;
        fdk_i32 lh = assigned.height - DLG_PAD * 2 - bh - DLG_GAP;
        if (lh < 1) {
            lh = 1;
        }
        fdk_widget_set_bounds(
            d->label,
            (fdk_rect){assigned.x + DLG_PAD, assigned.y + DLG_PAD, lw,
                       lh});
    }
}

static void dialog_body_destroy(fdk_widget *w) {
    fdk_dialog *d = dialog_of_body(w);
    if (d == NULL) {
        return;
    }
    body_of(w)->dialog = NULL;
    /* The buttons are torn down after this hook (teardown_free runs
     * the hook first, children after); their destroy paths never
     * touch the ctxs, so freeing them here is safe. */
    fdk_free(d->button_ctxs);
    d->button_ctxs = NULL;
    if (d->font_owned != NULL) {
        fdk_font_destroy(d->font_owned);
        d->font_owned = NULL;
    }
    fdk_free(d);
}

static const fdk_a11y_class dialog_body_a11y = {
    .role = FDK_A11Y_ROLE_DIALOG,
    /* name: the dialog title (set as the accessible-name override
     * at creation, so it is overridable by the app); MODAL state
     * computed from the dialog's options. */
    .describe = dialog_body_a11y_describe,
    .actions = NULL,
    .perform = NULL,
};

static const fdk_widget_class fdk_dialog_body_class = {
    .size = sizeof(fdk_dialog_body),
    .name = "dialog-body",
    .handle_event = NULL,
    .paint = NULL, /* base paint: the window_background fill set below */
    .measure = NULL,
    .arrange = dialog_body_arrange,
    .destroy = dialog_body_destroy,
    .a11y = &dialog_body_a11y,
};

/* ---- button activation ---- */

static void dialog_button_clicked(fdk_widget *button, void *user) {
    (void)button;
    const struct dialog_button_ctx *ctx = user;
    dialog_respond(ctx->d, ctx->response);
}

/* ---- public API ---- */

fdk_result fdk_dialog_show_message(fdk_context *ctx,
                                   const fdk_dialog_options *options,
                                   fdk_dialog_response_fn on_response,
                                   void *user_data,
                                   fdk_window **out_window) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    const char *title = (options != NULL && options->title != NULL)
                            ? options->title
                            : "Message";
    const char *text = (options != NULL && options->text != NULL)
                           ? options->text
                           : "";
    fdk_dialog_buttons buttons = (options != NULL)
                                     ? options->buttons
                                     : FDK_DIALOG_BUTTONS_OK;
    bool modal = (options != NULL) && options->modal;

    /* The button plan: labels, responses, the negative answer. */
    struct {
        const char *label;
        fdk_dialog_response response;
    } plan[3];
    size_t plan_n = 0;
    fdk_dialog_response affirmative = FDK_DIALOG_OK;
    fdk_dialog_response negative = FDK_DIALOG_CANCEL;
    switch (buttons) {
    case FDK_DIALOG_BUTTONS_OK_CANCEL:
        plan[plan_n].label = "OK";
        plan[plan_n].response = FDK_DIALOG_OK;
        plan_n++;
        plan[plan_n].label = "Cancel";
        plan[plan_n].response = FDK_DIALOG_CANCEL;
        plan_n++;
        affirmative = FDK_DIALOG_OK;
        negative = FDK_DIALOG_CANCEL;
        break;
    case FDK_DIALOG_BUTTONS_YES_NO:
        plan[plan_n].label = "Yes";
        plan[plan_n].response = FDK_DIALOG_YES;
        plan_n++;
        plan[plan_n].label = "No";
        plan[plan_n].response = FDK_DIALOG_NO;
        plan_n++;
        affirmative = FDK_DIALOG_YES;
        negative = FDK_DIALOG_NO;
        break;
    case FDK_DIALOG_BUTTONS_YES_NO_CANCEL:
        plan[plan_n].label = "Yes";
        plan[plan_n].response = FDK_DIALOG_YES;
        plan_n++;
        plan[plan_n].label = "No";
        plan[plan_n].response = FDK_DIALOG_NO;
        plan_n++;
        plan[plan_n].label = "Cancel";
        plan[plan_n].response = FDK_DIALOG_CANCEL;
        plan_n++;
        affirmative = FDK_DIALOG_YES;
        negative = FDK_DIALOG_CANCEL;
        break;
    case FDK_DIALOG_BUTTONS_CLOSE:
        plan[plan_n].label = "Close";
        plan[plan_n].response = FDK_DIALOG_CLOSE;
        plan_n++;
        affirmative = FDK_DIALOG_CLOSE;
        negative = FDK_DIALOG_CLOSE;
        break;
    case FDK_DIALOG_BUTTONS_OK:
    default:
        plan[plan_n].label = "OK";
        plan[plan_n].response = FDK_DIALOG_OK;
        plan_n++;
        affirmative = FDK_DIALOG_OK;
        negative = FDK_DIALOG_CANCEL;
        break;
    }

    fdk_dialog *d = fdk_alloc(sizeof(fdk_dialog));
    if (d == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    d->window = NULL;
    d->body = NULL;
    d->label = NULL;
    d->font = (options != NULL) ? options->font : NULL;
    d->font_owned = NULL;
    d->on_response = on_response;
    d->on_response_user = user_data;
    d->negative = negative;
    d->affirmative = affirmative;
    d->button_ctxs = NULL;
    d->button_ctx_count = 0;
    d->responded = false;

    /* Font: borrowed, or the system default we load and own. */
    if (d->font == NULL) {
        d->font_owned = fdk_font_load_system_default(DLG_FONT_PX);
        d->font = d->font_owned;
    }

    /* Size the dialog from its content: the label wraps at DLG_WRAP
     * (its line count at that width sizes the height — counted with
     * the breaker's count-only mode, no array needed); the button
     * row sizes from its buttons. */
    size_t line_count = 0;
    if (d->font != NULL && text[0] != '\0') {
        (void)fdk_font_break_lines_utf8(d->font, text, strlen(text),
                                        DLG_WRAP, NULL, 0, &line_count,
                                        NULL);
    }
    if (line_count < 1) {
        line_count = 1;
    }
    fdk_i32 line_h = 24;
    if (d->font != NULL) {
        fdk_font_metrics fm;
        fdk_font_get_metrics(d->font, &fm);
        line_h = fm.ascent + fm.descent;
        if (line_h < 8) {
            line_h = 8;
        }
    }
    fdk_i32 text_h = (fdk_i32)line_count * line_h;

    fdk_i32 btn_w = 0, btn_h = 0;
    for (size_t i = 0; i < plan_n; i++) {
        fdk_i32 bw = 0, bh = 0;
        fdk__text_extent(d->font, plan[i].label, &bw, &bh);
        bw += DLG_BTN_PAD;
        if (bw < DLG_BTN_MIN_W) {
            bw = DLG_BTN_MIN_W;
        }
        if (bh + 16 > btn_h) {
            btn_h = bh + 16;
        }
        btn_w += bw + DLG_BTN_GAP;
    }
    if (btn_w > 0) {
        btn_w -= DLG_BTN_GAP;
    }

    fdk_i32 width = DLG_WRAP + DLG_PAD * 2;
    fdk_i32 min_row = btn_w + DLG_PAD * 2;
    if (min_row > width) {
        width = min_row;
    }
    fdk_i32 height = DLG_PAD * 2 + text_h + DLG_GAP + btn_h;
    if (height < 120) {
        height = 120;
    }

    fdk_window_options wopts = {
        .title = title,
        .width = width,
        .height = height,
    };
    fdk_window *win = NULL;
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        if (d->font_owned != NULL) {
            fdk_font_destroy(d->font_owned);
        }
        fdk_free(d);
        return r;
    }
    d->window = win;
    d->modal = modal;
    fdk__window_set_auto_paint(win, true);
    fdk__window_set_destroy_notify(win, dialog_destroyed, d);
    fdk_window_set_event_callback(win, dialog_window_event, d);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(win, &root);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget *body = NULL;
    r = fdk_widget_create(root, &fdk_dialog_body_class,
                          (fdk_rect){0, 0, width, height}, &body);
    if (!fdk_ok(r)) {
        goto fail;
    }
    body_of(body)->dialog = d;
    d->body = body;
    /* A11y: the dialog's title is its accessible name. */
    fdk_widget_set_accessible_name(body, title);
    fdk_widget_set_background(
        body, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

    r = fdk_label_create(body, d->font, text, &d->label);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_label_set_mode(d->label, FDK_LABEL_WRAP);

    d->button_ctxs =
        fdk_alloc_array(plan_n, sizeof(*d->button_ctxs));
    if (d->button_ctxs == NULL && plan_n > 0) {
        r = FDK_ERR_OUT_OF_MEMORY;
        goto fail;
    }
    d->button_ctx_count = plan_n;
    for (size_t i = 0; i < plan_n; i++) {
        fdk_widget *btn = NULL;
        r = fdk_button_create(body, d->font, plan[i].label, &btn);
        if (!fdk_ok(r)) {
            goto fail;
        }
        d->button_ctxs[i].d = d;
        d->button_ctxs[i].response = plan[i].response;
        fdk_button_set_on_activate(btn, dialog_button_clicked,
                                   &d->button_ctxs[i]);
    }

    /* Content arrangement: window->content drives configure re-flow
     * through the body's arrange hook. */
    fdk_window_set_content(win, body);

    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }

    /* The affirmative button takes the initial focus: Enter works
     * through the stock Button contract. */
    if (plan_n > 0) {
        fdk_widget *btn = fdk_widget_child_at(body, 1);
        if (btn != NULL) {
            fdk_widget_focus(btn);
        }
    }

    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail:
    /* fdk_window_destroy tears the tree down; the body's destroy
     * hook (or the plain frees for pre-body failures) releases d. */
    if (d->body == NULL) {
        if (d->font_owned != NULL) {
            fdk_font_destroy(d->font_owned);
        }
        fdk_free(d->button_ctxs);
        fdk_free(d);
    }
    fdk_window_destroy(win);
    return r;
}
