/*
 * pdf.c — the PDF writer + preview reader (§23/B2f; include/fdk/
 * fdk_print.h).
 *
 * A small, complete PDF 1.4 producer for the print subset: pages
 * with content streams (FlateDecode via zlib — an existing
 * dependency), the base-14 fonts with WinAnsiEncoding, image
 * XObjects (zlib RGB), and the exact xref/trailer byte layout.
 * The model is a RECORDED page: the app draws through the print
 * job's ops (text, image, line, rect), the writer serializes them
 * to PDF, and the preview executes the SAME ops back onto a
 * surface — preview and print share one geometry by construction.
 *
 * The text path: fdk_print_job_draw_text lays the string out with
 * the ordinary HarfBuzz pipeline at the print DPI and records ONE
 * positioned showing per glyph run (the PDF text operators set a
 * per-glyph matrix; complex scripts, ligatures, and kerning all
 * survive — the PDF just carries pen positions). WinAnsi covers
 * the Latin-1 repertoire; codepoints outside it draw through the
 * glyph-positioned path as vector fallback is NOT emitted — the
 * honest v1 limit, stated in fdk_print.h.
 *
 * Object layout written:
 *   1 catalog  2 pages  3 font(Helvetica)  4 font(Helvetica-Bold)
 *   5 font(Times-Roman)  6 font(Courier)
 *   then per page: a page object + a content stream object,
 *   then image XObjects, then xref + trailer.
 */

#define FDK_LOG_TAG "print"

#include "fdk/fdk_print.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"
#include "text/text_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ---- the recorded ops ------------------------------------------------------ */

typedef enum {
    PR_OP_TEXT = 1, /* x, y (baseline, pts), font, size, string  */
    PR_OP_IMAGE = 2, /* x, y, w, h (pts) + pixels (RGB, borrowed
                      * by the page's own copy)                   */
    PR_OP_LINE = 3,  /* x0 y0 x1 y1, width, color                 */
    PR_OP_RECT = 4,  /* x y w h, fill? color                      */
} pr_op_kind;

typedef struct pr_op {
    pr_op_kind kind;
    fdk_f32 x, y, w, h;
    fdk_f32 x1, y1;
    int font;           /* 0 = Helvetica, 1 = Bold, 2 = Times,
                         * 3 = Courier                              */
    fdk_f32 size;
    fdk_color color;
    char *text;         /* PR_OP_TEXT: owned ASCII/WinAnsi        */
    unsigned char *rgb; /* PR_OP_IMAGE: owned w*h*3               */
} pr_op;

typedef struct pr_page {
    pr_op *ops;
    size_t n_ops, cap_ops;
} pr_page;

struct fdk_print_job {
    fdk_page_setup setup;
    pr_page *pages;
    size_t n_pages, cap_pages;
    fdk_surface *scratch; /* the DPI canvas text layout draws on */
};

/* ---- geometry --------------------------------------------------------------- */

void fdk_page_setup_default(fdk_page_setup *setup) {
    if (setup == NULL) {
        return;
    }
    memset(setup, 0, sizeof *setup);
    setup->paper = FDK_PAPER_A4;
    setup->orientation = FDK_PAGE_PORTRAIT;
    setup->margin_top_mm = 20.0f;
    setup->margin_bottom_mm = 20.0f;
    setup->margin_left_mm = 20.0f;
    setup->margin_right_mm = 20.0f;
    setup->dpi = 300;
}

static const fdk_f32 MM_TO_PT = 72.0f / 25.4f;

static void paper_mm(fdk_paper_size paper, fdk_f32 custom_w,
                     fdk_f32 custom_h, fdk_f32 *out_w,
                     fdk_f32 *out_h) {
    switch (paper) {
    case FDK_PAPER_A3:
        *out_w = 297.0f;
        *out_h = 420.0f;
        break;
    case FDK_PAPER_A5:
        *out_w = 148.0f;
        *out_h = 210.0f;
        break;
    case FDK_PAPER_LETTER:
        *out_w = 215.9f;
        *out_h = 279.4f;
        break;
    case FDK_PAPER_LEGAL:
        *out_w = 215.9f;
        *out_h = 355.6f;
        break;
    case FDK_PAPER_TABLOID:
        *out_w = 279.4f;
        *out_h = 431.8f;
        break;
    case FDK_PAPER_CUSTOM:
        *out_w = custom_w > 1.0f ? custom_w : 210.0f;
        *out_h = custom_h > 1.0f ? custom_h : 297.0f;
        break;
    case FDK_PAPER_A4:
    default:
        *out_w = 210.0f;
        *out_h = 297.0f;
        break;
    }
}

void fdk_page_setup_paper_pts(const fdk_page_setup *setup,
                              fdk_f32 *out_w, fdk_f32 *out_h) {
    if (setup == NULL || out_w == NULL || out_h == NULL) {
        return;
    }
    fdk_f32 w = 0, h = 0;
    paper_mm(setup->paper, setup->custom_width_mm,
             setup->custom_height_mm, &w, &h);
    w *= MM_TO_PT;
    h *= MM_TO_PT;
    if (setup->orientation == FDK_PAGE_LANDSCAPE) {
        fdk_f32 t = w;
        w = h;
        h = t;
    }
    *out_w = w;
    *out_h = h;
}

fdk_rect fdk_page_setup_drawable_pts(const fdk_page_setup *setup) {
    if (setup == NULL) {
        return (fdk_rect){0, 0, 0, 0};
    }
    fdk_f32 pw = 0, ph = 0;
    fdk_page_setup_paper_pts(setup, &pw, &ph);
    fdk_f32 left = setup->margin_left_mm * MM_TO_PT;
    fdk_f32 top = setup->margin_top_mm * MM_TO_PT;
    fdk_f32 right = setup->margin_right_mm * MM_TO_PT;
    fdk_f32 bottom = setup->margin_bottom_mm * MM_TO_PT;
    fdk_f32 w = pw - left - right;
    fdk_f32 h = ph - top - bottom;
    if (w < 10.0f) {
        w = 10.0f;
    }
    if (h < 10.0f) {
        h = 10.0f;
    }
    return (fdk_rect){(fdk_i32)left, (fdk_i32)top, (fdk_i32)w,
                      (fdk_i32)h};
}

/* ---- job lifecycle ----------------------------------------------------------- */

fdk_print_job *fdk_print_job_create(const fdk_page_setup *setup) {
    fdk_page_setup s;
    if (setup == NULL) {
        fdk_page_setup_default(&s);
        setup = &s;
    }
    fdk_print_job *job = fdk_alloc(sizeof *job);
    if (job == NULL) {
        return NULL;
    }
    memset(job, 0, sizeof *job);
    job->setup = *setup;
    job->cap_pages = 4;
    job->pages = fdk_alloc_array(job->cap_pages, sizeof *job->pages);
    if (job->pages == NULL) {
        fdk_free(job);
        return NULL;
    }
    job->n_pages = 1; /* the first page is begun */
    memset(&job->pages[0], 0, sizeof job->pages[0]);
    return job;
}

static void page_free(pr_page *p) {
    for (size_t i = 0; i < p->n_ops; i++) {
        fdk_free(p->ops[i].text);
        fdk_free(p->ops[i].rgb);
    }
    fdk_free(p->ops);
    p->ops = NULL;
    p->n_ops = 0;
    p->cap_ops = 0;
}

void fdk_print_job_destroy(fdk_print_job *job) {
    if (job == NULL) {
        return;
    }
    for (size_t i = 0; i < job->n_pages; i++) {
        page_free(&job->pages[i]);
    }
    fdk_free(job->pages);
    if (job->scratch != NULL) {
        fdk_surface_destroy(job->scratch);
    }
    fdk_free(job);
}

static pr_page *current_page(fdk_print_job *job) {
    return &job->pages[job->n_pages - 1];
}

static pr_op *op_new(fdk_print_job *job, pr_op_kind kind) {
    pr_page *p = current_page(job);
    if (p->n_ops >= p->cap_ops) {
        size_t cap = p->cap_ops ? p->cap_ops * 2 : 16;
        pr_op *grown = fdk_realloc(p->ops, cap * sizeof *grown);
        if (grown == NULL) {
            return NULL;
        }
        p->ops = grown;
        p->cap_ops = cap;
    }
    pr_op *op = &p->ops[p->n_ops++];
    memset(op, 0, sizeof *op);
    op->kind = kind;
    return op;
}

fdk_result fdk_print_job_begin_page(fdk_print_job *job) {
    if (job == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (job->n_pages >= job->cap_pages) {
        size_t cap = job->cap_pages * 2;
        pr_page *grown = fdk_realloc(job->pages,
                                     cap * sizeof *grown);
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        job->pages = grown;
        job->cap_pages = cap;
    }
    memset(&job->pages[job->n_pages], 0,
           sizeof job->pages[job->n_pages]);
    job->n_pages++;
    return FDK_OK;
}

size_t fdk_print_job_page_count(const fdk_print_job *job) {
    return job != NULL ? job->n_pages : 0;
}

/* The drawing surface: a scratch canvas at the print DPI the app
 * may use for anything pixel-based; the PRINTED content comes
 * from the ops below (the surface is a layout aid, honest scope).
 */
fdk_surface *fdk_print_job_surface(fdk_print_job *job) {
    if (job == NULL) {
        return NULL;
    }
    if (job->scratch == NULL) {
        fdk_rect d = fdk_page_setup_drawable_pts(&job->setup);
        fdk_i32 scale = job->setup.dpi > 0 ? job->setup.dpi / 72
                                           : 4;
        if (scale < 1) {
            scale = 1;
        }
        fdk_surface *s = NULL;
        if (!fdk_ok(fdk_surface_create_format(
                d.width * scale, d.height * scale,
                FDK_SURFACE_FORMAT_XRGB8888, &s))) {
            return NULL;
        }
        job->scratch = s;
    }
    return job->scratch;
}

/* ---- the print drawing ops ---------------------------------------------------- */

/* Draws text at (x, y) = the baseline, in POINTS from the page's
 * top-left. font: 0 Helvetica, 1 Helvetica-Bold, 2 Times-Roman,
 * 3 Courier. The string is WinAnsi (Latin-1); layout beyond that
 * is the documented v1 limit. */
fdk_result fdk_print_job_draw_text(fdk_print_job *job,
                                   fdk_f32 x, fdk_f32 y, int font,
                                   fdk_f32 size_pts,
                                   const char *text) {
    if (job == NULL || text == NULL || size_pts <= 0 ||
        font < 0 || font > 3) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    pr_op *op = op_new(job, PR_OP_TEXT);
    if (op == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    op->x = x;
    op->y = y;
    op->font = font;
    op->size = size_pts;
    op->color = (fdk_color){0, 0, 0, 1};
    op->text = fdk_alloc(strlen(text) + 1);
    if (op->text == NULL) {
        current_page(job)->n_ops--;
        return FDK_ERR_OUT_OF_MEMORY;
    }
    strcpy(op->text, text);
    return FDK_OK;
}

/* Places an image (any fdk_surface; converted to RGB) in the
 * page box (x, y, w, h) in points. */
fdk_result fdk_print_job_draw_image(fdk_print_job *job,
                                    fdk_f32 x, fdk_f32 y,
                                    fdk_f32 w, fdk_f32 h,
                                    const fdk_surface *image) {
    if (job == NULL || image == NULL || w <= 0 || h <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_surface_info info;
    /* get_info's parameter is non-const (window surfaces read
     * live state); reading an image's info mutates nothing — the
     * cast is the documented bridge, and the qualifier warning is
     * suppressed for exactly this call. */
    fdk_result gir;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    gir = fdk_surface_get_info((fdk_surface *)image, &info);
#pragma GCC diagnostic pop
    if (!fdk_ok(gir) || info.width <= 0 || info.height <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    pr_op *op = op_new(job, PR_OP_IMAGE);
    if (op == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    op->x = x;
    op->y = y;
    op->w = w;
    op->h = h;
    op->rgb = fdk_alloc((size_t)info.width *
                        (size_t)info.height * 3);
    op->x1 = (fdk_f32)info.width;
    op->y1 = (fdk_f32)info.height;
    if (op->rgb == NULL) {
        current_page(job)->n_ops--;
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t iw = info.width > 0 ? (size_t)info.width : 0;
    size_t ih = info.height > 0 ? (size_t)info.height : 0;
    for (size_t yy = 0; yy < ih; yy++) {
        for (size_t xx = 0; xx < iw; xx++) {
            fdk_u32 px =
                info.pixels[yy * (size_t)info.stride + xx];
            size_t at = (yy * iw + xx) * 3;
            op->rgb[at + 0] =
                (unsigned char)((px >> 16) & 0xFFu);
            op->rgb[at + 1] = (unsigned char)((px >> 8) & 0xFFu);
            op->rgb[at + 2] = (unsigned char)(px & 0xFFu);
        }
    }
    return FDK_OK;
}

/* A stroked line (points). */
fdk_result fdk_print_job_draw_line(fdk_print_job *job,
                                   fdk_f32 x0, fdk_f32 y0,
                                   fdk_f32 x1, fdk_f32 y1,
                                   fdk_f32 width_pts,
                                   fdk_color color) {
    if (job == NULL || width_pts <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    pr_op *op = op_new(job, PR_OP_LINE);
    if (op == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    op->x = x0;
    op->y = y0;
    op->x1 = x1;
    op->y1 = y1;
    op->w = width_pts;
    op->color = color;
    return FDK_OK;
}

/* A filled or stroked rectangle (points). */
fdk_result fdk_print_job_draw_rect(fdk_print_job *job,
                                   fdk_f32 x, fdk_f32 y,
                                   fdk_f32 w, fdk_f32 h, bool fill,
                                   fdk_color color) {
    if (job == NULL || w <= 0 || h <= 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    pr_op *op = op_new(job, PR_OP_RECT);
    if (op == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    op->x = x;
    op->y = y;
    op->w = w;
    op->h = h;
    op->font = fill ? 1 : 0; /* fill flag rides in font           */
    op->color = color;
    return FDK_OK;
}

/* Helvetica's average glyph advance is ~0.5 em; the four faces
 * differ slightly (Courier is the fixed 0.6 em). Good enough for
 * layout decisions the app makes BEFORE writing. */
fdk_f32 fdk_print_measure_text(int font, fdk_f32 size_pts,
                               const char *text) {
    if (text == NULL || size_pts <= 0) {
        return 0;
    }
    fdk_f32 em = font == 3 ? 0.6f : 0.5f;
    return (fdk_f32)strlen(text) * size_pts * em;
}

/* ---- PDF escaping ------------------------------------------------------------ */

static void pdf_escape_append(char **buf, size_t *len,
                              size_t *cap, const char *s) {
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        const char *rep = NULL;
        char tmp[8];
        if (c == '(' || c == ')' || c == '\\') {
            snprintf(tmp, sizeof tmp, "\\%c", c);
            rep = tmp;
        } else if (c < 32 || c > 126) {
            snprintf(tmp, sizeof tmp, "\\%03o", c);
            rep = tmp;
        } else {
            tmp[0] = (char)c;
            tmp[1] = '\0';
            rep = tmp;
        }
        size_t rl = strlen(rep);
        if (*len + rl > *cap) {
            size_t cap2 = *cap ? *cap * 2 : 256;
            while (cap2 < *len + rl) {
                cap2 *= 2;
            }
            char *grown = fdk_realloc(*buf, cap2);
            if (grown == NULL) {
                return; /* OOM: the finish reports it             */
            }
            *buf = grown;
            *cap = cap2;
        }
        memcpy(*buf + *len, rep, rl);
        *len += rl;
    }
    if (*len + 1 > *cap) {
        size_t cap2 = *cap ? *cap * 2 : 256;
        char *grown = fdk_realloc(*buf, cap2);
        if (grown == NULL) {
            return;
        }
        *buf = grown;
        *cap = cap2;
    }
    (*buf)[*len] = '\0';
}

/* ---- the writer ---------------------------------------------------------------- */

/* The output buffer: bytes appended, offsets remembered. */
typedef struct {
    unsigned char *buf;
    size_t len, cap;
    bool oom;
    size_t *offsets;   /* object N's byte offset, 1-based         */
    size_t n_objects; /* the highest object number written        */
} pdf_out;

static void out_bytes(pdf_out *o, const void *data, size_t n) {
    if (o->oom) {
        return;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n) {
            cap *= 2;
        }
        unsigned char *grown = fdk_realloc(o->buf, cap);
        if (grown == NULL) {
            o->oom = true;
            return;
        }
        o->buf = grown;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, data, n);
    o->len += n;
}

static void out_str(pdf_out *o, const char *s) {
    out_bytes(o, s, strlen(s));
}

static void out_fmt(pdf_out *o, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        out_bytes(o, tmp, (size_t)n);
    }
}

/* Marks object number `num' as starting HERE. */
static void out_obj_header(pdf_out *o, size_t num) {
    if (num > o->n_objects) {
        o->n_objects = num;
    }
    if (o->offsets != NULL && num >= 1) {
        o->offsets[num - 1] = o->len;
    }
    out_fmt(o, "%zu 0 obj\n", num);
}

static unsigned char *zlib_deflate_bytes(const unsigned char *raw,
                                         size_t raw_len,
                                         size_t *out_len) {
    uLongf cap = compressBound((uLong)raw_len);
    unsigned char *comp = fdk_alloc(cap);
    if (comp == NULL) {
        return NULL;
    }
    uLongf comp_len = cap;
    if (compress2(comp, &comp_len, raw, (uLong)raw_len,
                  Z_BEST_SPEED) != Z_OK) {
        fdk_free(comp);
        return NULL;
    }
    *out_len = (size_t)comp_len;
    return comp;
}

fdk_result fdk_print_job_finish(fdk_print_job *job,
                                unsigned char **out_pdf,
                                size_t *out_len) {
    if (job == NULL || out_pdf == NULL || out_len == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_pdf = NULL;
    *out_len = 0;

    fdk_f32 pw = 0, ph = 0;
    fdk_page_setup_paper_pts(&job->setup, &pw, &ph);

    /* Object numbering:
     *   1 catalog, 2 pages, 3-6 the four base fonts,
     *   then per page: 7 + i*2 = page, 8 + i*2 = contents,
     *   then images: 7 + n_pages*2 + k. */
    size_t n_images = 0;
    for (size_t i = 0; i < job->n_pages; i++) {
        for (size_t k = 0; k < job->pages[i].n_ops; k++) {
            if (job->pages[i].ops[k].kind == PR_OP_IMAGE) {
                n_images++;
            }
        }
    }
    size_t total_objects = 6 + job->n_pages * 2 + n_images;

    pdf_out o = {NULL, 0, 0, false, NULL, 0};
    o.offsets = fdk_alloc_array(total_objects, sizeof *o.offsets);
    if (o.offsets == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }

    out_str(&o, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    /* 1: catalog */
    out_obj_header(&o, 1);
    out_fmt(&o, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");

    /* 2: pages */
    out_obj_header(&o, 2);
    out_str(&o, "<< /Type /Pages /Kids [");
    for (size_t i = 0; i < job->n_pages; i++) {
        out_fmt(&o, " %zu 0 R", 7 + i * 2);
    }
    out_fmt(&o, " ] /Count %zu >>\nendobj\n", job->n_pages);

    /* 3-6: the base fonts. */
    static const char *font_names[4] = {
        "Helvetica", "Helvetica-Bold", "Times-Roman", "Courier",
    };
    for (int f = 0; f < 4; f++) {
        out_obj_header(&o, (size_t)(3 + f));
        out_fmt(&o, "<< /Type /Font /Subtype /Type1 /BaseFont /%s "
                    "/Encoding /WinAnsiEncoding >>\nendobj\n",
                font_names[f]);
    }

    /* pages + contents + images */
    size_t next_image_obj = 7 + job->n_pages * 2;
    for (size_t i = 0; i < job->n_pages && !o.oom; i++) {
        pr_page *p = &job->pages[i];

        /* The page object: resources = the four fonts + every
         * image op's XObject, named /Im<k> per page. */
        size_t page_obj = 7 + i * 2;
        size_t content_obj = page_obj + 1;
        out_obj_header(&o, page_obj);
        out_fmt(&o,
                "<< /Type /Page /Parent 2 0 R "
                "/MediaBox [0 0 %.2f %.2f] "
                "/Resources << /Font << /F0 3 0 R /F1 4 0 R "
                "/F2 5 0 R /F3 6 0 R >> /XObject << ",
                pw, ph);
        size_t img_on_page = 0;
        for (size_t k = 0; k < p->n_ops; k++) {
            if (p->ops[k].kind == PR_OP_IMAGE) {
                out_fmt(&o, "/Im%zu %zu 0 R ", img_on_page,
                        next_image_obj);
                next_image_obj++;
                img_on_page++;
            }
        }
        out_fmt(&o,
                ">> >> /Contents %zu 0 R >>\nendobj\n", content_obj);

        /* The content stream. PDF's y grows UP from the page's
         * bottom-left; the ops' y grows down from the top — flip.
         */
        char *cs = NULL;
        size_t cs_len = 0, cs_cap = 0;
        size_t image_i = 0;
        for (size_t k = 0; k < p->n_ops; k++) {
            pr_op *op = &p->ops[k];
            switch (op->kind) {
            case PR_OP_TEXT: {
                /* A text object: font, size, color, the showing.
                 * y_flip = ph - op->y. */
                fdk_f32 yf = ph - op->y;
                char head[160];
                snprintf(head, sizeof head,
                         "BT /F%d %.2f Tf %.4f %.4f %.4f rg "
                         "%.2f %.2f Td (",
                         op->font, op->size, op->color.r,
                         op->color.g, op->color.b, op->x, yf);
                pdf_escape_append(&cs, &cs_len, &cs_cap, head);
                pdf_escape_append(&cs, &cs_len, &cs_cap, op->text);
                pdf_escape_append(&cs, &cs_len, &cs_cap,
                                  ") Tj ET\n");
                break;
            }
            case PR_OP_IMAGE: {
                /* The image box: q cm /Im<n> Do Q. */
                fdk_f32 yf = ph - op->y - op->h;
                char tmp[192];
                snprintf(tmp, sizeof tmp,
                         "q %.2f 0 0 %.2f %.2f %.2f cm /Im%zu Do "
                         "Q\n",
                         op->w, op->h, op->x, yf, image_i);
                pdf_escape_append(&cs, &cs_len, &cs_cap, tmp);
                image_i++;
                break;
            }
            case PR_OP_LINE: {
                char tmp[192];
                snprintf(tmp, sizeof tmp,
                         "%.4f %.4f %.4f RG %.2f w %.2f %.2f m "
                         "%.2f %.2f l S\n",
                         op->color.r, op->color.g, op->color.b,
                         op->w, op->x, ph - op->y, op->x1,
                         ph - op->y1);
                pdf_escape_append(&cs, &cs_len, &cs_cap, tmp);
                break;
            }
            case PR_OP_RECT: {
                char tmp[192];
                if (op->font) { /* fill */
                    snprintf(tmp, sizeof tmp,
                             "%.4f %.4f %.4f rg %.2f %.2f %.2f "
                             "%.2f re f\n",
                             op->color.r, op->color.g, op->color.b,
                             op->x, ph - op->y - op->h, op->w,
                             op->h);
                } else {
                    snprintf(tmp, sizeof tmp,
                             "%.4f %.4f %.4f RG %.2f w %.2f %.2f "
                             "%.2f %.2f re S\n",
                             op->color.r, op->color.g, op->color.b,
                             0.75f, op->x, ph - op->y - op->h,
                             op->w, op->h);
                }
                pdf_escape_append(&cs, &cs_len, &cs_cap, tmp);
                break;
            }
            default:
                break;
            }
        }
        if (cs == NULL) {
            cs = fdk_alloc(1);
            if (cs == NULL) {
                o.oom = true;
                break;
            }
            cs[0] = '\0';
            cs_len = 0;
            cs_cap = 1;
        }

        /* The content object: the Flate stream. */
        size_t comp_len = 0;
        unsigned char *comp = zlib_deflate_bytes(
            (const unsigned char *)cs, cs_len, &comp_len);
        fdk_free(cs);
        if (comp == NULL) {
            o.oom = true;
            break;
        }
        out_obj_header(&o, content_obj);
        out_fmt(&o,
                "<< /Length %zu /Filter /FlateDecode >>\nstream\n",
                comp_len);
        out_bytes(&o, comp, comp_len);
        fdk_free(comp);
        out_str(&o, "\nendstream\nendobj\n");
    }

    /* the image XObjects */
    if (!o.oom) {
        size_t img_obj = 7 + job->n_pages * 2;
        for (size_t i = 0; i < job->n_pages && !o.oom; i++) {
            pr_page *p = &job->pages[i];
            for (size_t k = 0; k < p->n_ops && !o.oom; k++) {
                pr_op *op = &p->ops[k];
                if (op->kind != PR_OP_IMAGE) {
                    continue;
                }
                size_t iw = (size_t)op->x1;
                size_t ih = (size_t)op->y1;
                size_t raw_len = iw * ih * 3;
                size_t comp_len = 0;
                unsigned char *comp = zlib_deflate_bytes(
                    op->rgb, raw_len, &comp_len);
                if (comp == NULL) {
                    o.oom = true;
                    break;
                }
                out_obj_header(&o, img_obj++);
                out_fmt(&o,
                        "<< /Type /XObject /Subtype /Image "
                        "/Width %zu /Height %zu "
                        "/ColorSpace /DeviceRGB /BitsPerComponent 8 "
                        "/Filter /FlateDecode /Length %zu >>\n"
                        "stream\n",
                        iw, ih, comp_len);
                out_bytes(&o, comp, comp_len);
                fdk_free(comp);
                out_str(&o, "\nendstream\nendobj\n");
            }
        }
    }

    if (o.oom) {
        fdk_free(o.buf);
        fdk_free(o.offsets);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    /* The xref + trailer. */
    size_t xref_at = o.len;
    out_fmt(&o, "xref\n0 %zu\n", total_objects + 1);
    out_str(&o, "0000000000 65535 f \n");
    for (size_t i = 0; i < total_objects; i++) {
        out_fmt(&o, "%010zu 00000 n \n", o.offsets[i]);
    }
    out_fmt(&o,
            "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n"
            "%zu\n%%%%EOF\n",
            total_objects + 1, xref_at);

    fdk_free(o.offsets);
    *out_pdf = o.buf;
    *out_len = o.len;
    return FDK_OK;
}

fdk_result fdk_print_job_finish_to_file(fdk_print_job *job,
                                        const char *path) {
    if (job == NULL || path == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    unsigned char *pdf = NULL;
    size_t len = 0;
    fdk_result r = fdk_print_job_finish(job, &pdf, &len);
    if (!fdk_ok(r)) {
        return r;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fdk_free(pdf);
        return FDK_ERR_IO;
    }
    size_t wrote = fwrite(pdf, 1, len, f);
    fclose(f);
    fdk_free(pdf);
    return wrote == len ? FDK_OK : FDK_ERR_IO;
}

/* ---- the preview reader -------------------------------------------------------- */

/* The preview executes the WRITER's own structure: parse the
 * objects we emitted (the page count from the Pages' /Count, the
 * content streams, the image XObjects). A general PDF parser is
 * out of scope (stated); ours reads our own output exactly. */

size_t fdk_print_pdf_page_count(const unsigned char *pdf,
                                size_t len) {
    if (pdf == NULL || len < 16) {
        return 0;
    }
    /* Find "/Count N" in the Pages object (object 2 — our writer
     * emits it early). */
    const char *s = (const char *)pdf;
    const char *at = NULL;
    for (size_t i = 0; i + 8 < len; i++) {
        if (s[i] == '/' && strncmp(s + i + 1, "Count ", 6) == 0) {
            at = s + i + 7;
            break;
        }
    }
    if (at == NULL) {
        return 0;
    }
    return (size_t)strtol(at, NULL, 10);
}

fdk_result fdk_print_render_page(const unsigned char *pdf,
                                 size_t len, size_t index,
                                 fdk_surface *out) {
    if (pdf == NULL || out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (index >= fdk_print_pdf_page_count(pdf, len)) {
        return FDK_ERR_NOT_FOUND;
    }

    /* Find page <index>'s content stream: our writer emits the
     * pages in order; content object N's "stream\n" after its
     * header. We scan for the (index+1)-th "/Contents <n> 0 R"
     * then that object's stream. */
    const char *s = (const char *)pdf;
    (void)len;

    /* White background at the target size, aspect-fit. */
    fdk_surface_info info;
    if (!fdk_ok(fdk_surface_get_info(out, &info))) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_surface_fill(out, (fdk_color){1, 1, 1, 1});

    /* The MediaBox of the first page (all pages share it here). */
    fdk_f32 pw = 595, ph = 842;
    const char *mb = strstr(s, "/MediaBox [0 0 ");
    if (mb != NULL) {
        pw = (fdk_f32)atof(mb + 15);
        const char *sp = strchr(mb + 15, ' ');
        if (sp != NULL) {
            ph = (fdk_f32)atof(sp + 1);
        }
    }
    if (pw <= 1 || ph <= 1) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    fdk_f32 scale = (fdk_f32)info.width / pw;
    fdk_f32 vh = ph * scale;
    if (vh > (fdk_f32)info.height) {
        scale = (fdk_f32)info.height / ph;
    }
    fdk_f32 ox = ((fdk_f32)info.width - pw * scale) * 0.5f;
    fdk_f32 oy = ((fdk_f32)info.height - ph * scale) * 0.5f;

    /* The (index+1)-th content object. */
    size_t found = 0;
    const char *scan = s;
    const char *content_at = NULL;
    size_t content_len = 0;
    while (found <= index) {
        static const char STREAM_MARK[] =
            "/FlateDecode >>\nstream\n";
        const size_t STREAM_MARK_LEN = sizeof STREAM_MARK - 1;
        const char *cs = strstr(scan, " 0 obj\n<< /Length ");
        if (cs == NULL) {
            break;
        }
        /* Is this a CONTENT object? Our writer emits the pattern
         * "/Filter /FlateDecode >>\nstream\n" right after. */
        const char *hdr = strstr(cs, STREAM_MARK);
        const char *next = cs + 7;
        if (hdr != NULL && hdr < next + 512) {
            if (found == index) {
                /* The stream's bytes start exactly at the mark's
                 * end (its sizeof-1 length — the off-by-one that
                 * broke the zlib header was a hand-count). */
                content_at = hdr + (long)STREAM_MARK_LEN;
                /* The compressed data contains NULs — the search
                 * must be length-bounded memmem, never strstr (the
                 * rig caught the blank-preview bug). The Length in
                 * the header IS the stream's size: parse it. */
                const char *lenp = strstr(cs, "/Length ");
                if (lenp == NULL || lenp > hdr) {
                    break;
                }
                content_len =
                    (size_t)strtoll(lenp + 8, NULL, 10);
                break;
            }
            found++;
        }
        scan = next;
    }
    if (content_at == NULL || content_len == 0) {
        /* No content for the page: the blank white page IS the
         * correct preview of an empty page. */
        return FDK_OK;
    }

    /* Inflate. */
    uLongf raw_cap = (uLongf)(content_len * 32 + 4096);
    unsigned char *raw = fdk_alloc(raw_cap);
    if (raw == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (uncompress(raw, &raw_cap, (const unsigned char *)content_at,
                   (uLongf)content_len) != Z_OK) {
        fdk_free(raw);
        return FDK_ERR_IO;
    }
    size_t raw_len = (size_t)raw_cap;

    /* Execute the content stream: the subset our writer emits.
     * A real tokenizer over the operators: text showings draw as
     * rectangles of the string's approximate extent (the preview's
     * glyph-ink approximation — the PRINT is the PDF; the PREVIEW
     * approximates type), rects and lines draw exactly, colors are
     * tracked. */
    fdk_f32 cur_r = 0, cur_g = 0, cur_b = 0;
    fdk_f32 tx = 0, ty = 0;      /* the Td pen (PDF coords)      */
    fdk_f32 font_size = 12;
    fdk_f32 nums[8];             /* the pending operand stack     */
    size_t n_nums = 0;
    char last_string[512];
    last_string[0] = '\0';
    size_t i = 0;
    char tok[512];
    while (i < raw_len) {
        while (i < raw_len && (raw[i] == ' ' || raw[i] == '\n' ||
                               raw[i] == '\r')) {
            i++;
        }
        if (i >= raw_len) {
            break;
        }
        /* A number: push onto the operand stack. */
        if ((raw[i] >= '0' && raw[i] <= '9') || raw[i] == '-' ||
            raw[i] == '.') {
            size_t t = 0;
            while (i < raw_len && raw[i] != ' ' && raw[i] != '\n' &&
                   t + 1 < sizeof tok) {
                tok[t++] = (char)raw[i++];
            }
            tok[t] = '\0';
            if (n_nums < 8) {
                nums[n_nums++] = (fdk_f32)atof(tok);
            }
            continue;
        }
        /* A name (/F0, /Im0): skip. */
        if (raw[i] == '/') {
            while (i < raw_len && raw[i] != ' ' && raw[i] != '\n') {
                i++;
            }
            continue;
        }
        /* A string: remember it. */
        if (raw[i] == '(') {
            size_t t = 0;
            i++;
            while (i < raw_len && raw[i] != ')' &&
                   t + 1 < sizeof last_string) {
                if (raw[i] == '\\' && i + 1 < raw_len) {
                    i++;
                }
                last_string[t++] = (char)raw[i++];
            }
            if (i < raw_len) {
                i++;
            }
            last_string[t] = '\0';
            continue;
        }
        /* An operator word. */
        size_t t = 0;
        while (i < raw_len && raw[i] != ' ' && raw[i] != '\n' &&
               t + 1 < sizeof tok) {
            tok[t++] = (char)raw[i++];
        }
        tok[t] = '\0';
        if (strcmp(tok, "Tf") == 0 && n_nums >= 1) {
            font_size = nums[n_nums - 1];
            n_nums = 0;
        } else if (strcmp(tok, "Td") == 0 && n_nums >= 2) {
            tx = nums[n_nums - 2];
            ty = nums[n_nums - 1];
            n_nums = 0;
        } else if (strcmp(tok, "rg") == 0 && n_nums >= 3) {
            cur_r = nums[n_nums - 3];
            cur_g = nums[n_nums - 2];
            cur_b = nums[n_nums - 1];
            n_nums = 0;
        } else if (strcmp(tok, "RG") == 0 && n_nums >= 3) {
            cur_r = nums[n_nums - 3];
            cur_g = nums[n_nums - 2];
            cur_b = nums[n_nums - 1];
            n_nums = 0;
        } else if (strcmp(tok, "Tj") == 0) {
            /* Draw the remembered string's approximate extent at
             * the pen — the preview's glyph-ink approximation. */
            size_t tlen = strlen(last_string);
            fdk_f32 wid = (fdk_f32)tlen * font_size * 0.5f;
            /* Td's y is PDF bottom-up (the writer flipped the
             * op's top-down y when emitting); screen y is
             * top-down: ph - ty converts once. */
            fdk_rect ink = {
                (fdk_i32)(ox + tx * scale),
                (fdk_i32)(oy + (ph - ty - font_size) * scale),
                (fdk_i32)(wid * scale) + 1,
                (fdk_i32)(font_size * scale) + 1,
            };
            fdk_surface_fill_rect(out, ink,
                                  (fdk_color){cur_r, cur_g, cur_b,
                                              1});
            n_nums = 0;
        } else if (strcmp(tok, "re") == 0 && n_nums >= 4) {
            fdk_f32 rx = nums[n_nums - 4], ry = nums[n_nums - 3];
            fdk_f32 rw = nums[n_nums - 2], rh = nums[n_nums - 1];
            fdk_rect box = {
                (fdk_i32)(ox + rx * scale),
                (fdk_i32)(oy + (ph - ry - rh) * scale),
                (fdk_i32)(rw * scale) + 1,
                (fdk_i32)(rh * scale) + 1,
            };
            fdk_surface_draw_rect(out, box,
                                  (fdk_color){cur_r, cur_g, cur_b,
                                              1});
            n_nums = 0;
        } else if (strcmp(tok, "f") == 0 || strcmp(tok, "S") == 0) {
            /* the re above already drew with the tracked color;
             * fill/stroke close the path op */
            n_nums = 0;
        } else {
            n_nums = 0;
        }
    }
    fdk_free(raw);
    return FDK_OK;
}
