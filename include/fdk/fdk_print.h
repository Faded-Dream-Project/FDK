/*
 * fdk_print.h — printing (the §23/B2f round, 2026-11-01): page
 * setup, a self-contained PDF writer, IPP printer discovery, a
 * print dialog, and print preview.
 *
 * FDK's charter says "no CUPS dependency" the same way it says "no
 * D-Bus library": the wire is implemented natively or not at all.
 * The pieces:
 *
 *   page setup   paper size (the ISO/US names + custom mm), DPI
 *                defaults, orientation, margins — the numbers the
 *                writer and the preview both read
 *   PDF writer   a small, correct PDF 1.4 producer (src/print/
 *                pdf.c): catalog/pages/page objects, content
 *                streams (Flate via zlib — an existing
 *                dependency), the base-14 fonts (Helvetica,
 *                Helvetica-Bold, Times, Courier) with WinAnsi
 *                encoding, image XObjects (zlib-compressed RGB,
 *                the surface reader that feeds them), the xref
 *                table + trailer — every byte ours, no library
 *   drawing      the print context IS an fdk_surface (the app
 *                draws with the ordinary primitives; page breaks
 *                are the app's begin_page calls), text draws
 *                through the SAME HarfBuzz pipeline and is
 *                embedded as glyph-positioned text showings
 *   IPP client   printer discovery + submission over raw TCP
 *                (RFC 8010's encoding, RFC 8011's operations):
 *                CUPS-Get-Printers against localhost:631 and
 *                $CUPS_SERVER, Get-Printer-Attributes for the
 *                state, Print-Job with the PDF as
 *                application/pdf — the HTTP/1.1 POST wrapper, no
 *                libcups, no D-Bus
 *   dialog       the print dialog widget (printer list, copies,
 *                page range, duplex when the printer reports it)
 *                and the PREVIEW (the PDF's pages rendered back
 *                through FDK's own decoder — the writer and the
 *                preview share the geometry, so what you see is
 *                what prints)
 *
 * Scope honesty: the writer targets the PDF subset a print path
 * needs (text, images, vector shapes through the content stream);
 * interactive forms, annotations, and encryption are out. Base-14
 * fonts render everywhere; embedding the app's own TTF is the
 * documented next step (the HarfBuzz pipeline already positions
 * by glyph id, which is exactly what a CIDFont subset needs).
 *
 * Sources for the wire formats (fetched this round):
 *   RFC 8010 — IPP/1.1: Encoding and Transport
 *     https://www.rfc-editor.org/rfc/rfc8010.txt
 *   RFC 8011 — IPP/1.1: Model and Semantics
 *     https://www.rfc-editor.org/rfc/rfc8011.txt
 *   PDF 1.7 / ISO 32000-1 structure ( Adobe's published spec)
 *     https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf
 */

#ifndef FDK_PRINT_H
#define FDK_PRINT_H

#include "fdk_error.h"
#include "fdk_surface.h"
#include "fdk_types.h"
#include "fdk_widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Page setup ---------------------------------------------------------- */

typedef enum fdk_paper_size {
    FDK_PAPER_A4 = 1,        /* 210 x 297 mm                     */
    FDK_PAPER_A3 = 2,        /* 297 x 420 mm                     */
    FDK_PAPER_A5 = 3,        /* 148 x 210 mm                     */
    FDK_PAPER_LETTER = 4,    /* 8.5 x 11 in                      */
    FDK_PAPER_LEGAL = 5,     /* 8.5 x 14 in                      */
    FDK_PAPER_TABLOID = 6,   /* 11 x 17 in                       */
    FDK_PAPER_CUSTOM = 7,    /* custom_mm below                  */
} fdk_paper_size;

typedef enum fdk_page_orientation {
    FDK_PAGE_PORTRAIT = 1,
    FDK_PAGE_LANDSCAPE = 2,
} fdk_page_orientation;

/* The page geometry. Zero-init + set_size for a default A4
 * portrait with 20 mm margins (the input-struct discipline:
 * appended fields only, defaults read as zero). */
typedef struct fdk_page_setup {
    fdk_paper_size paper;        /* FDK_PAPER_A4 default           */
    fdk_page_orientation orientation;
    fdk_f32 custom_width_mm;     /* FDK_PAPER_CUSTOM only          */
    fdk_f32 custom_height_mm;
    fdk_f32 margin_top_mm;       /* 20.0 default                   */
    fdk_f32 margin_bottom_mm;
    fdk_f32 margin_left_mm;
    fdk_f32 margin_right_mm;
    fdk_i32 dpi;                 /* 300 default                    */
} fdk_page_setup;

/* Fills `setup' with the defaults (A4 portrait, 20 mm, 300 dpi). */
void fdk_page_setup_default(fdk_page_setup *setup);

/* The page's dimensions in POINTS (1/72", the PDF unit), margins
 * applied: the drawable box the writer and preview share. */
fdk_rect fdk_page_setup_drawable_pts(const fdk_page_setup *setup);
/* The full page's size in points (landscape swaps). */
void fdk_page_setup_paper_pts(const fdk_page_setup *setup,
                              fdk_f32 *out_w, fdk_f32 *out_h);

/* ---- The print context ------------------------------------------------------
 * One print job: create with the setup, draw pages (the context
 * IS an fdk_surface sized to the drawable box — draw with the
 * ordinary primitives), begin_page between pages, finish writes
 * the PDF. */

typedef struct fdk_print_job fdk_print_job;

fdk_print_job *fdk_print_job_create(const fdk_page_setup *setup);
void fdk_print_job_destroy(fdk_print_job *job);

/* The current page's drawing surface (the drawable box at the
 * setup's DPI; 1 pt = dpi/72 px). Same pointer until the next
 * begin_page. */
fdk_surface *fdk_print_job_surface(fdk_print_job *job);

/* Starts a new page. The first page is ALREADY begun at create. */
fdk_result fdk_print_job_begin_page(fdk_print_job *job);

/* Finishes and writes the PDF. Returns the byte count; the buffer
 * is fdk_alloc'd and caller-owned (fdk_free). */
fdk_result fdk_print_job_finish(fdk_print_job *job,
                                unsigned char **out_pdf,
                                size_t *out_len);

/* Writes the finished PDF to a path (the convenience form). */
fdk_result fdk_print_job_finish_to_file(fdk_print_job *job,
                                        const char *path);

/* The number of pages begun so far. */
size_t fdk_print_job_page_count(const fdk_print_job *job);

/* ---- The print drawing ops ------------------------------------------------
 * The recorded primitives the writer serializes and the preview
 * executes: one geometry, two consumers. Coordinates are POINTS
 * from the page's top-left; y grows DOWN (the writer flips to
 * PDF's bottom-up). */

/* Text at (x, y) = the BASELINE. font: 0 Helvetica, 1
 * Helvetica-Bold, 2 Times-Roman, 3 Courier. The string is
 * WinAnsi (Latin-1) — the documented v1 repertoire. */
fdk_result fdk_print_job_draw_text(fdk_print_job *job,
                                   fdk_f32 x, fdk_f32 y, int font,
                                   fdk_f32 size_pts,
                                   const char *text);
/* An image placed in the box (converted to RGB at native
 * resolution). */
fdk_result fdk_print_job_draw_image(fdk_print_job *job,
                                    fdk_f32 x, fdk_f32 y,
                                    fdk_f32 w, fdk_f32 h,
                                    const fdk_surface *image);
/* A stroked line. */
fdk_result fdk_print_job_draw_line(fdk_print_job *job,
                                   fdk_f32 x0, fdk_f32 y0,
                                   fdk_f32 x1, fdk_f32 y1,
                                   fdk_f32 width_pts,
                                   fdk_color color);
/* A filled or stroked rectangle. */
fdk_result fdk_print_job_draw_rect(fdk_print_job *job,
                                   fdk_f32 x, fdk_f32 y,
                                   fdk_f32 w, fdk_f32 h, bool fill,
                                   fdk_color color);

/* Measures text at the print scale (Helvetica metrics
 * approximation): the advance in points. */
fdk_f32 fdk_print_measure_text(int font, fdk_f32 size_pts,
                               const char *text);


/* ---- IPP: printer discovery + submission --------------------------------- */

typedef struct fdk_printer_info {
    char name[128];        /* the printer's human name             */
    char uri[256];         /* ipp://host:port/printers/NAME        */
    char state[32];        /* idle / processing / stopped          */
    bool accepts_pdf;      /* document-format-supported            */
    bool duplex;           /* sides-supported                      */
} fdk_printer_info;

/* Enumerates printers: CUPS-Get-Printers against the local IPP
 * server (localhost:631, or $CUPS_SERVER). Returns the count
 * found (0 with the server absent — not an error); out may be
 * NULL to just count. The raw-socket HTTP/1.1 + application/ipp
 * exchange is RFC 8010's, implemented natively (no libcups). */
size_t fdk_print_enumerate_printers(fdk_printer_info *out,
                                    size_t cap, int timeout_ms);

/* Submits a finished PDF to a printer (Print-Job). Returns
 * FDK_OK on successful-ok, an FDK_ERR_* on transport/status
 * trouble. `job_name' names the job in the queue. */
fdk_result fdk_print_submit(const fdk_printer_info *printer,
                            const unsigned char *pdf, size_t len,
                            const char *job_name, int timeout_ms);

/* ---- The print dialog ------------------------------------------------------ */

/* Runs the toolkit's modal print dialog: printer list (live
 * enumeration, a File(PDF) pseudo-printer writing to a chosen
 * path), copies (1-99), page range (all / from-to), duplex
 * checkbox when the selected printer reports it, and a PREVIEW
 * pane rendering the job's pages through the preview below.
 * Returns true when the user confirmed (the job was submitted or
 * written); false on cancel. */
bool fdk_dialog_print(fdk_window *parent, fdk_print_job *job);

/* ---- Print preview -----------------------------------------------------------
 * Renders a finished PDF's pages onto a surface. The decoder is
 * FDK's own (the writer's inverse for the subset the writer
 * emits: FlateDecode streams, base-14 text, RGB images) — it
 * reads the SAME primitives the writer recorded, so the preview
 * is geometrically exact. */

/* The number of pages in a written PDF. */
size_t fdk_print_pdf_page_count(const unsigned char *pdf,
                                size_t len);

/* Renders page `index' (0-based) into `out' (any size — the page
 * is scaled to fit, aspect preserved, centered on white). */
fdk_result fdk_print_render_page(const unsigned char *pdf,
                                 size_t len, size_t index,
                                 fdk_surface *out);

#ifdef __cplusplus
}
#endif

#endif /* FDK_PRINT_H */
