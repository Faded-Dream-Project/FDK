/* 16_printing.c — the §23/B2f print surface, end to end.
 *
 * Builds a two-page "report" through the print drawing ops (text
 * headings, body lines, an image, rules and a filled chart box),
 * then:
 *   - writes it to /tmp/fdk_print_demo.pdf (open it anywhere)
 *   - runs the PRINT DIALOG (fdk_dialog_print): the live printer
 *     list (IPP), copies / range / duplex when the printer has
 *     it, the page PREVIEW pane, and the File(PDF) pseudo-printer
 *   - the console prints the enumerated printers and the file's
 *     size — the verification trail
 *
 * The preview pane and the written PDF share the recorded ops, so
 * what the dialog previews is what prints.
 */

#include "example_window.h"

#include "fdk/fdk_print.h"
#include "fdk/fdk_text.h"

#include <stdio.h>
#include <string.h>

#include "core/alloc_internal.h" /* fdk_free */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    fdk_context *ctx = NULL;
    if (!fdk_ok(fdk_init(&ctx, NULL)) || ctx == NULL) {
        fprintf(stderr, "fdk_init failed\n");
        return 1;
    }
    fdk_window *window = NULL;
    fdk_window_options wopts = {
        .title = "FDK 16 — printing",
        .width = 520,
        .height = 300,
    };
    if (!fdk_ok(fdk_window_create(ctx, &wopts, &window))) {
        fdk_shutdown(ctx);
        return 1;
    }
    (void)fdk_window_set_decorated(window, true);
    fdk_window_show(window);

    /* ---- build the document -------------------------------------- */
    fdk_page_setup setup;
    fdk_page_setup_default(&setup);
    setup.paper = FDK_PAPER_A4;
    setup.orientation = FDK_PAGE_PORTRAIT;

    fdk_print_job *job = fdk_print_job_create(&setup);
    if (job == NULL) {
        fprintf(stderr, "print job failed\n");
        fdk_shutdown(ctx);
        return 1;
    }

    /* Page 1: the title page. */
    fdk_print_job_draw_text(job, 57, 120, 1, 28,
                            "The FDK Printing Demo");
    fdk_print_job_draw_line(job, 57, 132, 538, 132, 1.2f,
                            (fdk_color){0.2f, 0.2f, 0.4f, 1});
    fdk_print_job_draw_text(job, 57, 170, 0, 12,
                            "This document was produced by FDK's own PDF");
    fdk_print_job_draw_text(job, 57, 186, 0, 12,
                            "writer: base-14 fonts, Flate streams, vector");
    fdk_print_job_draw_text(job, 57, 202, 0, 12,
                            "rules, and an image XObject. No external");
    fdk_print_job_draw_text(job, 57, 218, 0, 12,
                            "library wrote a byte of it.");

    /* a small picture: a gradient panel */
    fdk_surface *img = NULL;
    if (fdk_ok(fdk_surface_create_format(64, 48,
                                         FDK_SURFACE_FORMAT_XRGB8888,
                                         &img))) {
        fdk_surface_info info;
        fdk_surface_get_info(img, &info);
        for (int y = 0; y < 48; y++) {
            for (int x = 0; x < 64; x++) {
                fdk_u32 px =
                    0xFF000000u |
                    ((unsigned)((x * 4) & 0xFF) << 16) |
                    ((unsigned)((y * 5) & 0xFF) << 8) |
                    (unsigned)0x80u;
                info.pixels[(size_t)y * (size_t)info.stride +
                            (size_t)x] = px;
            }
        }
        fdk_print_job_draw_image(job, 57, 240, 192, 144, img);
    }

    /* a chart-ish box column */
    for (int i = 0; i < 6; i++) {
        fdk_f32 h = 20.0f + (fdk_f32)(i * 14);
        fdk_print_job_draw_rect(job, 300, 380 - h, 24, h, true,
                                (fdk_color){0.45f, 0.35f, 0.75f, 1});
    }

    /* Page 2: the data page. */
    fdk_print_job_begin_page(job);
    fdk_print_job_draw_text(job, 57, 80, 2, 16, "Page two: the numbers");
    for (int i = 0; i < 12; i++) {
        char line[96];
        snprintf(line, sizeof line,
                 "row %02d    value %4d    note %c", i, i * 37,
                 'A' + (i % 26));
        fdk_print_job_draw_text(job, 57, (fdk_f32)(110 + i * 18),
                                3, 10, line);
    }
    fdk_print_job_draw_rect(job, 57, 340, 481, 1, true,
                            (fdk_color){0, 0, 0, 1});
    fdk_print_job_draw_text(job, 57, 362, 0, 10,
                            "Courier rows through the print ops.");

    printf("job pages: %zu\n", fdk_print_job_page_count(job));

    /* ---- write the PDF -------------------------------------------- */
    fdk_result r = fdk_print_job_finish_to_file(job,
                                                "/tmp/fdk_print_demo.pdf");
    printf("finish_to_file: %s\n", fdk_ok(r) ? "OK" : "FAILED");
    unsigned char *pdf = NULL;
    size_t len = 0;
    if (fdk_ok(fdk_print_job_finish(job, &pdf, &len))) {
        printf("pdf bytes: %zu\n", len);
    }

    /* ---- the printers out there ------------------------------------ */
    fdk_printer_info printers[8];
    size_t n = fdk_print_enumerate_printers(printers, 8, 800);
    printf("printers found: %zu\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  [%zu] %s (%s) %s\n", i, printers[i].name,
               printers[i].state, printers[i].uri);
    }

    /* ---- the dialog ------------------------------------------------- */
    bool printed = fdk_dialog_print(window, job);
    printf("dialog: %s\n", printed ? "confirmed" : "cancelled");

    fdk_free(pdf);
    fdk_surface_destroy(img);
    fdk_print_job_destroy(job);
    fdk_window_destroy(window);
    fdk_shutdown(ctx);
    return 0;
}
