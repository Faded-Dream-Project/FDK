/*
 * example_window.h — the shared example-window helper (1.2.5)
 *
 * Every example used to hand-roll the same boilerplate: init (with
 * or without an app_id), a window titled however it felt that day,
 * its own close/Escape handling, its own pump loop. Ten programs,
 * ten spellings of "FDK NN — name". The helper makes the suite
 * uniform:
 *
 *   - fdk_example_init()   — fdk_init with the per-example app_id
 *                            ("org.fdk.exampleNN" — taskbars and rig
 *                            rules can address each demo)
 *   - fdk_example_open()   — the standard window: "FDK NN — name"
 *                            title, an in-window HEADER (the example
 *                            titlebar: name on the left, "Esc — quit"
 *                            hint on the right, a hairline below — the
 *                            label survives in every screenshot/rig
 *                            capture even when the WM titlebar is
 *                            cropped), a content box the example
 *                            fills, and a status line at the bottom
 *                            (fdk_example_set_status)
 *   - fdk_example_pump()   — ONE loop pass: pump events, then a
 *                            damage-gated, frame-paced paint
 *   - fdk_example_run()    — pump until quit
 *   - fdk_example_close()  — teardown in the right order
 *
 * Quit semantics live in ONE place: WM close request (the destroy
 * helper, the rig's close path) and Escape both flip ex->quit; an
 * example needing window-level events chains through ex->on_event
 * (called BEFORE the quit handling — observe-only; there is no
 * honest consume API, so handlers must not assume events stop).
 *
 * Two integration tiers, both real:
 *   FULL  — fdk_example_open + content in ex->content: the box-layout
 *           demos (01, 03, 04, 05, 08).
 *   INIT  — fdk_example_init only, the example owns its window: the
 *           demos whose subject IS the window chrome or framebuffer —
 *           02 (raw-surface renderer: the widget frame would fight
 *           the direct surface writes), 06 (the FDK decoration band
 *           is the titlebar — a helper header would duplicate it),
 *           and the manual-bounds playgrounds (07, 09, 10) whose
 *           absolute set_bounds layouts predate the box contract.
 *
 * Fontless systems: the header/status labels accept a NULL font (the
 * 07 contract — text is absent, everything else renders), so the
 * helper never fails just because no TrueType face is installed.
 *
 * Header-only on purpose: the Makefile builds every C file under
 * examples/ as its own program; static functions keep the helper out
 * of the library and out of every example's link namespace. Unused
 * statics in any one example are fine (marked FDK_EXAMPLE_FN).
 */
#ifndef FDK_EXAMPLE_WINDOW_H
#define FDK_EXAMPLE_WINDOW_H

/* Header-only helpers are included whole: any given example uses
 * only part of the API, so unused statics must not warn. */
#if defined(__GNUC__) || defined(__clang__)
#define FDK_EXAMPLE_FN __attribute__((unused))
#else
#define FDK_EXAMPLE_FN
#endif

#include "fdk/fdk.h"
#include "fdk/fdk_widgets.h"
#include "fdk/fdk_window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fdk_example {
    fdk_context *ctx;
    fdk_window *window;
    fdk_widget *root;     /* the window's root widget            */
    fdk_widget *content;  /* the vertical box examples fill      */
    fdk_widget *status;   /* helper-owned bottom label           */
    fdk_widget *header_label; /* "FDK NN — name" (restylable)    */
    fdk_widget *header_hint;  /* "Esc — quit" (restylable)       */
    fdk_font *ui;         /* 14px UI font (header + status)      */
    char label[96];       /* "FDK NN — name", for exit lines     */
    long frames;          /* painted frames (the pump counts)    */
    long frame_limit;     /* FDK_DEMO_FRAMES, 0 = unlimited      */
    bool quit;
    /* Observe-only window-event hook (runs before the helper's
     * quit semantics; leave NULL when the example has none). */
    void (*on_event)(fdk_window *w, const fdk_event_data *ev,
                     void *user);
    void *on_event_user;
} fdk_example;

FDK_EXAMPLE_FN static void fdk_example__window_event(fdk_window *w,
                                                       const fdk_event_data *ev,
                                                       void *user) {
    fdk_example *ex = user;
    if (ex->on_event != NULL) {
        ex->on_event(w, ev, ex->on_event_user);
    }
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        ex->quit = true;
        return;
    }
    if (ev->type == FDK_EVENT_KEY_DOWN &&
        ev->key.scancode == FDK_KEY_ESC) {
        ex->quit = true;
    }
}

/* fdk_init with the uniform app_id. Writes NULL + a message on
 * failure (no display). */
FDK_EXAMPLE_FN static bool fdk_example_init(fdk_context **ctx,
                                             const char *number) {
    char app_id[64];
    snprintf(app_id, sizeof app_id, "org.fdk.example%s", number);
    fdk_init_options opts = {0};
    opts.app_id = app_id;
    if (!fdk_ok(fdk_init(ctx, &opts))) {
        fprintf(stderr, "example %s: fdk_init failed (no display?)\n",
                number);
        return false;
    }
    return true;
}

/* Opens the standard window. The example builds its widgets inside
 * ex->content (a padded vertical box). */
FDK_EXAMPLE_FN static bool fdk_example_open(fdk_example *ex,
                                            fdk_context *ctx,
                                            const char *number,
                                            const char *name,
                                            fdk_i32 width,
                                            fdk_i32 height) {
    memset(ex, 0, sizeof *ex);
    ex->ctx = ctx;
    snprintf(ex->label, sizeof ex->label, "FDK %s — %s", number, name);

    const char *limit_s = getenv("FDK_DEMO_FRAMES");
    ex->frame_limit = (limit_s != NULL) ? atol(limit_s) : 0;

    fdk_window_options wopts = {0};
    wopts.title = ex->label;
    wopts.width = width;
    wopts.height = height;
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &ex->window))) {
        fprintf(stderr, "example %s: fdk_window_create failed\n",
                number);
        return false;
    }
    fdk_window_set_event_callback(ex->window, fdk_example__window_event,
                                  ex);
    if (!fdk_ok(fdk_window_get_root(ex->window, &ex->root))) {
        fdk_window_destroy(ex->window);
        ex->window = NULL;
        return false;
    }
    /* NULL on fontless systems is fine — labels accept it (the 07
     * contract): the header text is absent, nothing else changes. */
    ex->ui = fdk_font_load_system_default(14);

    /* root: vertical box — [header][hairline][content...][status] */
    fdk_widget *frame = NULL;
    if (!fdk_ok(fdk_box_create(ex->root, FDK_VERTICAL, &frame))) {
        goto fail;
    }
    fdk_box_set_padding(frame, 0);
    fdk_box_set_spacing(frame, 0);
    fdk_window_set_content(ex->window, frame);

    /* The example titlebar: name left, "Esc — quit" right. */
    fdk_widget *header = NULL;
    if (!fdk_ok(fdk_box_create(frame, FDK_HORIZONTAL, &header))) {
        goto fail;
    }
    fdk_box_set_padding(header, 10);
    fdk_box_set_spacing(header, 8);
    fdk_widget *title_lbl = NULL;
    (void)fdk_label_create(header, ex->ui, ex->label, &title_lbl);
    ex->header_label = title_lbl;
    fdk_widget *spacer = NULL;
    if (fdk_ok(fdk_box_create(header, FDK_HORIZONTAL, &spacer))) {
        fdk_widget_set_expand(spacer, true, false); /* push hint right */
    }
    fdk_widget *hint_lbl = NULL;
    (void)fdk_label_create(header, ex->ui, "Esc — quit", &hint_lbl);
    ex->header_hint = hint_lbl;
    (void)fdk_label_set_color(
        hint_lbl, fdk_theme_get_color(NULL, FDK_TK_TEXT_DISABLED));

    fdk_widget *hair = NULL;
    (void)fdk_separator_create(frame, FDK_HORIZONTAL, &hair);

    /* The example's content box (padded, spaced). */
    if (!fdk_ok(fdk_box_create(frame, FDK_VERTICAL, &ex->content))) {
        goto fail;
    }
    fdk_box_set_padding(ex->content, 14);
    fdk_box_set_spacing(ex->content, 10);
    fdk_widget_set_expand(ex->content, false, true);

    /* Status line at the bottom. */
    (void)fdk_label_create(frame, ex->ui, "", &ex->status);
    (void)fdk_label_set_mode(ex->status, FDK_LABEL_ELLIPSIZE);

    fdk_window_show(ex->window);
    (void)fdk_window_paint(ex->window); /* the first frame */
    return true;

fail:
    if (ex->ui != NULL) {
        fdk_font_destroy(ex->ui);
        ex->ui = NULL;
    }
    fdk_window_destroy(ex->window);
    ex->window = NULL;
    return false;
}

FDK_EXAMPLE_FN static void fdk_example_set_status(fdk_example *ex,
                                                   const char *text) {
    if (ex->status != NULL) {
        (void)fdk_label_set_text(ex->status, text);
    }
}

/* ONE loop pass. Returns false when the example should exit:
 * quit (WM close / Escape), or the FDK_DEMO_FRAMES budget. The paint
 * is damage-gated and frame-paced — the same contract the
 * hand-rolled loops used: an idle app presents nothing, and Wayland's
 * compositor feedback throttles the frame rate. */
FDK_EXAMPLE_FN static bool fdk_example_pump(fdk_example *ex) {
    if (ex->quit) {
        return false;
    }
    (void)fdk_pump_events(ex->ctx, 15);
    if (ex->quit) {
        return false;
    }
    fdk_surface *surface = NULL;
    if (fdk_ok(fdk_window_get_surface(ex->window, &surface)) &&
        !fdk_surface_frame_ready(surface)) {
        return true; /* frame pending — nothing else to do this pass */
    }
    if (fdk_widget_tree_has_damage(ex->root)) {
        (void)fdk_window_paint(ex->window);
    }
    ex->frames++;
    if (ex->frame_limit > 0 && ex->frames >= ex->frame_limit) {
        return false;
    }
    return true;
}

/* Pumps until quit (WM close / Escape / frame budget). */
FDK_EXAMPLE_FN static void fdk_example_run(fdk_example *ex) {
    while (fdk_example_pump(ex)) {
        /* nothing else to do */
    }
}

/* Teardown: window first (it borrows nothing we free after), then
 * the helper's font, then the context. Prints the rig-facing exit
 * line (the example suites grep "exited cleanly"). */
FDK_EXAMPLE_FN static void fdk_example_close(fdk_example *ex) {
    printf("%s: exited cleanly after %ld frames\n", ex->label,
           ex->frames);
    if (ex->window != NULL) {
        fdk_window_destroy(ex->window);
        ex->window = NULL;
    }
    if (ex->ui != NULL) {
        fdk_font_destroy(ex->ui);
        ex->ui = NULL;
    }
    if (ex->ctx != NULL) {
        fdk_shutdown(ex->ctx);
        ex->ctx = NULL;
    }
}

#endif /* FDK_EXAMPLE_WINDOW_H */
