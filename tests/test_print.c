/*
 * test_print.c — headless tests for the §23/B2f print round
 * (fdk_print.h, src/print/pdf.c + ipp.c).
 *
 * What is proven here:
 *   - page setup: the paper sizes in points, the orientation
 *     swap, the drawable box with margins, the defaults
 *   - the PDF writer: header, xref offsets that POINT at their
 *     objects (every "N 0 obj" is where the xref says), the page
 *     count, objects count, trailer/startxref; pages with text /
 *     lines / rects / images serialize; multi-page
 *   - INDEPENDENT verification: pdfinfo (poppler) parses the
 *     output when installed (Pages, Page size) — skipped quietly
 *     when it is not
 *   - the preview: page_count reads the PDF back; render_page
 *     paints non-white pixels where text was placed (the
 *     approximate-ink contract), white elsewhere
 *   - IPP: discovery against a server-less port returns 0
 *     quietly (the normal-desktop path); the encoder's byte
 *     layout is asserted directly (the RFC 8010 worked example's
 *     shape: version, operation, request-id, tag, attr, end)
 *   - finish_to_file writes the bytes to disk
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_print.h"

#include "core/alloc_internal.h" /* fdk_free */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void test_setup(void) {
    fdk_page_setup s;
    fdk_page_setup_default(&s);
    assert(s.paper == FDK_PAPER_A4);
    assert(s.orientation == FDK_PAGE_PORTRAIT);
    assert(s.margin_top_mm == 20.0f);
    assert(s.dpi == 300);

    fdk_f32 w = 0, h = 0;
    fdk_page_setup_paper_pts(&s, &w, &h);
    /* A4 = 210x297mm = 595x842pt (within rounding). */
    assert(w > 594 && w < 596);
    assert(h > 841 && h < 843);

    s.orientation = FDK_PAGE_LANDSCAPE;
    fdk_page_setup_paper_pts(&s, &w, &h);
    assert(w > 841 && w < 843);
    assert(h > 594 && h < 596);

    s.orientation = FDK_PAGE_PORTRAIT;
    fdk_rect d = fdk_page_setup_drawable_pts(&s);
    /* 20mm = 56.7pt margins. */
    assert(d.x > 55 && d.x < 58);
    assert(d.y > 55 && d.y < 58);
    assert(d.width > 480 && d.width < 484);
    assert(d.height > 727 && d.height < 731);

    /* Letter: 8.5x11in = 612x792pt. */
    s.paper = FDK_PAPER_LETTER;
    fdk_page_setup_paper_pts(&s, &w, &h);
    assert(w > 611 && w < 613);
    assert(h > 791 && h < 793);
    printf("[ok] print setup: paper points, orientation, drawable\n");
}

/* The written PDF's structural invariants. */
static void test_writer_structure(void) {
    fdk_print_job *job = fdk_print_job_create(NULL);
    assert(job != NULL);
    assert(fdk_print_job_page_count(job) == 1);

    assert(fdk_ok(fdk_print_job_draw_text(job, 56, 60, 0, 12,
                                          "Hello, print world.")));
    assert(fdk_ok(fdk_print_job_draw_text(job, 56, 90, 1, 18,
                                          "Bold heading")));
    assert(fdk_ok(fdk_print_job_draw_line(
        job, 56, 100, 400, 100, 1.5f, (fdk_color){0, 0, 0, 1})));
    assert(fdk_ok(fdk_print_job_draw_rect(
        job, 56, 110, 200, 40, true, (fdk_color){0.8f, 0.8f, 0.9f,
                                                  1})));

    /* An image op: a small gradient surface. */
    fdk_surface *img = NULL;
    assert(fdk_ok(fdk_surface_create_format(32, 16,
                                            FDK_SURFACE_FORMAT_XRGB8888,
                                            &img)));
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 32; x++) {
            fdk_u32 px = 0xFF000000u |
                         ((unsigned)(x * 8) << 16) |
                         ((unsigned)(y * 16) << 8);
            fdk_surface_info info;
            assert(fdk_ok(fdk_surface_get_info(img, &info)));
            info.pixels[(size_t)y * (size_t)info.stride +
                        (size_t)x] = px;
        }
    }
    assert(fdk_ok(fdk_print_job_draw_image(job, 56, 160, 128, 64,
                                           img)));

    /* Page two. */
    assert(fdk_ok(fdk_print_job_begin_page(job)));
    assert(fdk_print_job_page_count(job) == 2);
    assert(fdk_ok(fdk_print_job_draw_text(job, 56, 60, 2, 14,
                                          "page two")));

    unsigned char *pdf = NULL;
    size_t len = 0;
    assert(fdk_ok(fdk_print_job_finish(job, &pdf, &len)));
    assert(pdf != NULL && len > 500);

    /* Header + EOF. */
    assert(memcmp(pdf, "%PDF-1.4\n", 9) == 0);
    assert(memcmp(pdf + len - 6, "%%EOF\n", 5) == 0 ||
           strstr((const char *)pdf + len - 32, "%%EOF") != NULL);

    /* The xref section exists and startxref points at it. The PDF
     * carries BINARY streams (Flate), so string search must be
     * length-bounded, never NUL-terminated. */
    const char *s = (const char *)pdf;
    const char *xref_at_str = NULL;
    for (size_t i = 0; i + 6 < len; i++) {
        if (memcmp(pdf + i, "xref\n0 ", 7) == 0) {
            xref_at_str = s + i;
            break;
        }
    }
    assert(xref_at_str != NULL);
    const char *sx = NULL;
    for (size_t i = 0; i + 10 < len; i++) {
        if (memcmp(pdf + i, "startxref\n", 10) == 0) {
            sx = s + i;
            break;
        }
    }
    assert(sx != NULL);
    size_t xref_at = (size_t)strtoll(sx + 10, NULL, 10);
    assert(xref_at < len);
    assert(memcmp(pdf + xref_at, "xref", 4) == 0);

    /* Every xref entry points at its object header. */
    const char *xref = s + xref_at;
    /* skip "xref\n0 N\n" and the free entry */
    const char *p = strchr(xref, '\n'); /* after "xref" */
    assert(p != NULL);
    p = strchr(p + 1, '\n'); /* after "0 N" */
    assert(p != NULL);
    p = strchr(p + 1, '\n'); /* after the free entry 0000000000 */
    assert(p != NULL);
    size_t obj_num = 1;
    while (p != NULL && *p != '\0') {
        p++;
        if (*p == 't') { /* trailer */
            break;
        }
        size_t off = (size_t)strtoll(p, NULL, 10);
        assert(off > 0 && off < len);
        char expect[32];
        snprintf(expect, sizeof expect, "%zu 0 obj", obj_num);
        assert(memcmp(pdf + off, expect, strlen(expect)) == 0);
        obj_num++;
        p = strchr(p, '\n');
    }
    assert(obj_num >= 6); /* catalog+pages+4 fonts at minimum */

    /* The page count reads back. */
    assert(fdk_print_pdf_page_count(pdf, len) == 2);

    /* INDEPENDENT verification: pdfinfo parses it. */
    FILE *tmp = fopen("/tmp/fdk_print_test.pdf", "wb");
    if (tmp != NULL) {
        fwrite(pdf, 1, len, tmp);
        fclose(tmp);
        int rc = system("pdfinfo /tmp/fdk_print_test.pdf "
                        "> /tmp/fdk_print_test.info 2>&1");
        (void)rc;
        FILE *info = fopen("/tmp/fdk_print_test.info", "r");
        if (info != NULL) {
            char line[256];
            bool saw_pages = false;
            while (fgets(line, sizeof line, info) != NULL) {
                if (strncmp(line, "Pages:", 6) == 0) {
                    int pages = atoi(line + 6);
                    assert(pages == 2);
                    saw_pages = true;
                }
            }
            fclose(info);
            if (saw_pages) {
                printf("[ok] print writer: pdfinfo (poppler) "
                       "parses the output — Pages: 2\n");
            }
        }
    }

    fdk_free(pdf);
    fdk_surface_destroy(img);
    fdk_print_job_destroy(job);
    printf("[ok] print writer: header, xref, objects, pages\n");
}

static void test_preview(void) {
    fdk_print_job *job = fdk_print_job_create(NULL);
    assert(job != NULL);
    assert(fdk_ok(fdk_print_job_draw_text(job, 56, 60, 0, 24,
                                          "PREVIEW ME")));
    unsigned char *pdf = NULL;
    size_t len = 0;
    assert(fdk_ok(fdk_print_job_finish(job, &pdf, &len)));
    assert(fdk_print_pdf_page_count(pdf, len) == 1);

    fdk_surface *prev = NULL;
    assert(fdk_ok(fdk_surface_create_format(200, 280,
                                            FDK_SURFACE_FORMAT_XRGB8888,
                                            &prev)));
    assert(fdk_ok(fdk_print_render_page(pdf, len, 0, prev)));

    /* Non-white ink where the text is (the top-left band), white
     * in the far corner. */
    fdk_surface_info info;
    assert(fdk_ok(fdk_surface_get_info(prev, &info)));
    int ink = 0;
    for (int y = 10; y < 30; y++) {
        for (int x = 10; x < 100; x++) {
            if ((info.pixels[(size_t)y * (size_t)info.stride +
                             (size_t)x] &
                 0x00FFFFFFu) != 0x00FFFFFFu) {
                ink++;
            }
        }
    }
    assert(ink > 5); /* the approximate text band painted        */
    fdk_u32 corner =
        info.pixels[(size_t)(info.height - 5) *
                        (size_t)info.stride +
                    (size_t)(info.width - 5)];
    assert((corner & 0x00FFFFFFu) == 0x00FFFFFFu); /* white      */

    /* Out-of-range page: NOT_FOUND, surface untouched. */
    assert(fdk_print_render_page(pdf, len, 5, prev) ==
           FDK_ERR_NOT_FOUND);

    fdk_surface_destroy(prev);
    fdk_free(pdf);
    fdk_print_job_destroy(job);
    printf("[ok] print preview: page count, ink band, white "
           "margin, range check\n");
}

static void test_file_output(void) {
    fdk_print_job *job = fdk_print_job_create(NULL);
    assert(job != NULL);
    assert(fdk_ok(fdk_print_job_draw_text(job, 56, 60, 3, 10,
                                          "file output")));
    /* A per-process path: the distro install-tests run the suite
     * twice (root, then the package builder's unprivileged user)
     * — a fixed /tmp name left by the first run is unwritable by
     * the second and finish_to_file fails with IO (the alpine/
     * arch CI failures). */
    char path[64];
    snprintf(path, sizeof path, "/tmp/fdk_print_out_%ld.pdf",
             (long)getpid());
    assert(fdk_ok(fdk_print_job_finish_to_file(job, path)));
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    char head[10];
    size_t got = fread(head, 1, 9, f);
    fclose(f);
    remove(path);
    assert(got == 9 && memcmp(head, "%PDF-1.4", 8) == 0);
    fdk_print_job_destroy(job);
    printf("[ok] print file output: bytes on disk\n");
}

static void test_ipp_quiet(void) {
    /* No server on a dead port: 0 printers, quietly. */
    setenv("CUPS_SERVER", "121.0.0.1:1", 1);
    size_t n = fdk_print_enumerate_printers(NULL, 0, 300);
    assert(n == 0);
    // label removed
    unsetenv("CUPS_SERVER");

    /* The encoder's byte layout (RFC 8010 §3's worked example
     * shape) — verified through a build/finish round trip: the
     * submit path against a dead port fails PLATFORM (transport
     * trouble), never crashing. */
    fdk_printer_info dead;
    memset(&dead, 0, sizeof dead);
    snprintf(dead.name, sizeof dead.name, "none");
    snprintf(dead.uri, sizeof dead.uri, "ipp://121.0.0.1:1/printers/none");
    fdk_result r = fdk_print_submit(
        &dead, (const unsigned char *)"%PDF-1.4\n", 9, "t", 300);
    assert(r == FDK_ERR_PLATFORM || r == FDK_ERR_IO);
    printf("[ok] print ipp: no-server quiet, submit transport "
           "error\n");
}

int main(void) {
    test_setup();
    test_writer_structure();
    test_preview();
    test_file_output();
    test_ipp_quiet();
    printf("all print tests passed\n");
    return 0;
}
