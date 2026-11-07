/*
 * dialog.c — the print dialog (§23/B2f; fdk_print.h).
 *
 * An FDK-owned window on the application's context (the dialog
 * architecture's shape: no nested blocking — the dialog runs on
 * the shared loop; fdk_dialog_print pumps until the user answers,
 * which is exactly what the file dialog's internal browser does
 * for its modal flavor).
 *
 * Layout: the printer combo (File(PDF) + the IPP list), copies,
 * the page preview (fdk_print_render_page on a canvas), page
 * navigation, and Print / Cancel.
 */

#define FDK_LOG_TAG "print"

#include "fdk/fdk_core.h"
#include "fdk/fdk_print.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    fdk_print_job *job;
    fdk_printer_info printers[8];
    size_t n_printers;
    int selected;         /* 0 = File(PDF), 1+ = printers[i-1]    */
    int copies;
    bool done;
    bool confirmed;
    int preview_page;

    fdk_context *ctx;
    fdk_window *win;
    fdk_widget *combo;
    fdk_widget *spin;
    fdk_widget *preview;
    fdk_widget *page_label;
    unsigned char *pdf;
    size_t pdf_len;
    fdk_font *font;
} print_dialog;

static print_dialog g_dlg;

/* ---- widgets' callbacks ---- */

static void preview_paint(fdk_widget *canvas, fdk_surface *s,
                          fdk_rect bounds, fdk_rect clip,
                          void *user) {
    (void)canvas; (void)bounds; (void)clip; (void)user;
    fdk_surface_fill(s, (fdk_color){1, 1, 1, 1});
    if (g_dlg.pdf != NULL && g_dlg.pdf_len > 0) {
        (void)fdk_print_render_page(g_dlg.pdf, g_dlg.pdf_len,
                                    (size_t)g_dlg.preview_page, s);
    }
}

static void refresh_page_label(void) {
    char buf[64];
    size_t pages = g_dlg.pdf != NULL
                       ? fdk_print_pdf_page_count(g_dlg.pdf,
                                                  g_dlg.pdf_len)
                       : 0;
    snprintf(buf, sizeof buf, "page %d / %zu",
             g_dlg.preview_page + 1, pages);
    fdk_label_set_text(g_dlg.page_label, buf);
}

static void refresh_preview(void) {
    fdk_canvas_invalidate(g_dlg.preview);
    refresh_page_label();
}

static void on_printer_changed(fdk_widget *combo, size_t index,
                               void *user) {
    (void)combo; (void)user;
    g_dlg.selected = (int)index;
}

static void on_prev(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    if (g_dlg.preview_page > 0) {
        g_dlg.preview_page--;
        refresh_preview();
    }
}

static void on_next(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    size_t pages = g_dlg.pdf != NULL
                       ? fdk_print_pdf_page_count(g_dlg.pdf,
                                                  g_dlg.pdf_len)
                       : 0;
    if ((size_t)(g_dlg.preview_page + 1) < pages) {
        g_dlg.preview_page++;
        refresh_preview();
    }
}

static void on_print(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    g_dlg.confirmed = true;
    g_dlg.done = true;
}

static void on_cancel(fdk_widget *btn, void *user) {
    (void)btn; (void)user;
    g_dlg.done = true;
}

static void on_window_event(fdk_window *w, const fdk_event_data *ev,
                            void *user) {
    /* WM close = cancel. */
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        g_dlg.done = true;
    }
    (void)w; (void)user;
}

/* ---- the dialog ---- */

bool fdk_dialog_print(fdk_window *parent, fdk_print_job *job) {
    if (job == NULL || parent == NULL) {
        return false;
    }
    memset(&g_dlg, 0, sizeof g_dlg);
    g_dlg.job = job;
    g_dlg.copies = 1;
    g_dlg.preview_page = 0;

    /* The finished PDF once: preview and submission share it. */
    if (!fdk_ok(fdk_print_job_finish(job, &g_dlg.pdf,
                                     &g_dlg.pdf_len))) {
        return false;
    }

    g_dlg.font = fdk_font_load_system_default(14);
    if (g_dlg.font == NULL) {
        g_dlg.font = fdk_font_load(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 14);
    }
    if (g_dlg.font == NULL) {
        fdk_free(g_dlg.pdf);
        g_dlg.pdf = NULL;
        return false;
    }

    g_dlg.n_printers =
        fdk_print_enumerate_printers(g_dlg.printers, 8, 800);

    fdk_window_options wopts = {
        .title = "Print",
        .width = 560,
        .height = 560,
    };
    if (!fdk_ok(fdk_window_create(fdk_window_get_context(parent),
                                  &wopts, &g_dlg.win))) {
        fdk_font_destroy(g_dlg.font);
        fdk_free(g_dlg.pdf);
        g_dlg.pdf = NULL;
        return false;
    }
    (void)fdk_window_set_decorated(g_dlg.win, true);
    fdk_window_set_event_callback(g_dlg.win, on_window_event, NULL);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(g_dlg.win, &root);

    /* Row 1: the printer combo. */
    fdk_widget *l1 = NULL;
    (void)fdk_label_create(root, g_dlg.font, "Printer:", &l1);
    fdk_widget_arrange(l1, (fdk_rect){16, 14, 70, 24});
    (void)fdk_combo_create(root, g_dlg.font, &g_dlg.combo);
    fdk_combo_append(g_dlg.combo, "File (PDF)", NULL);
    for (size_t i = 0; i < g_dlg.n_printers; i++) {
        char label[160];
        snprintf(label, sizeof label, "%.100s (%.20s)",
                 g_dlg.printers[i].name, g_dlg.printers[i].state);
        fdk_combo_append(g_dlg.combo, label, NULL);
    }
    fdk_combo_set_active(g_dlg.combo, 0);
    fdk_combo_set_on_changed(g_dlg.combo, on_printer_changed, NULL);
    fdk_widget_arrange(g_dlg.combo, (fdk_rect){90, 12, 300, 28});

    /* Row 2: copies. */
    fdk_widget *l2 = NULL;
    (void)fdk_label_create(root, g_dlg.font, "Copies:", &l2);
    fdk_widget_arrange(l2, (fdk_rect){16, 52, 70, 24});
    (void)fdk_spin_create(root, g_dlg.font, 1, 99, 1,
                          &g_dlg.spin);
    fdk_spin_set_value(g_dlg.spin, 1);
    fdk_widget_arrange(g_dlg.spin, (fdk_rect){90, 50, 90, 28});

    /* The preview pane. */
    (void)fdk_canvas_create(root, preview_paint, NULL,
                            &g_dlg.preview);
    fdk_widget_arrange(g_dlg.preview,
                       (fdk_rect){16, 90, 380, 400});
    fdk_widget *prev = NULL;
    (void)fdk_button_create(root, g_dlg.font, "<", &prev);
    fdk_button_set_on_activate(prev, on_prev, NULL);
    fdk_widget_arrange(prev, (fdk_rect){420, 90, 32, 28});
    fdk_widget *next = NULL;
    (void)fdk_button_create(root, g_dlg.font, ">", &next);
    fdk_button_set_on_activate(next, on_next, NULL);
    fdk_widget_arrange(next, (fdk_rect){456, 90, 32, 28});
    (void)fdk_label_create(root, g_dlg.font, "", &g_dlg.page_label);
    fdk_widget_arrange(g_dlg.page_label, (fdk_rect){420, 124, 90, 22});

    /* Print / Cancel. */
    fdk_widget *do_print = NULL;
    (void)fdk_button_create(root, g_dlg.font, "Print", &do_print);
    fdk_button_set_on_activate(do_print, on_print, NULL);
    fdk_widget_arrange(do_print, (fdk_rect){340, 500, 100, 30});
    fdk_widget *cancel = NULL;
    (void)fdk_button_create(root, g_dlg.font, "Cancel", &cancel);
    fdk_button_set_on_activate(cancel, on_cancel, NULL);
    fdk_widget_arrange(cancel, (fdk_rect){450, 500, 100, 30});

    refresh_preview();
    fdk_window_show(g_dlg.win);

    /* The modal pump: FDK dialogs run on the caller's loop; this
     * one owns its pump until answered (the file dialog's modal
     * flavor uses the same shape internally). */
    while (!g_dlg.done) {
        (void)fdk_pump_events(fdk_window_get_context(parent), 15);
    }

    bool result = false;
    if (g_dlg.confirmed) {
        g_dlg.copies = (int)fdk_spin_get_value(g_dlg.spin);
        if (g_dlg.selected <= 0) {
            /* The File(PDF) pseudo-printer. A real app would ask
             * for the path (fdk_dialog_save_file); the demo path
             * keeps the example self-contained. */
            result = fdk_ok(fdk_print_job_finish_to_file(
                job, "/tmp/fdk_print_dialog.pdf"));
        } else if ((size_t)(g_dlg.selected - 1) <
                   g_dlg.n_printers) {
            fdk_result r = FDK_ERR_PLATFORM;
            for (int c = 0; c < g_dlg.copies; c++) {
                r = fdk_print_submit(
                    &g_dlg.printers[g_dlg.selected - 1],
                    g_dlg.pdf, g_dlg.pdf_len, "FDK document",
                    5000);
                if (!fdk_ok(r)) {
                    break;
                }
            }
            result = fdk_ok(r);
        }
    }

    fdk_window_destroy(g_dlg.win);
    fdk_font_destroy(g_dlg.font);
    fdk_free(g_dlg.pdf);
    g_dlg.pdf = NULL;
    return result;
}
