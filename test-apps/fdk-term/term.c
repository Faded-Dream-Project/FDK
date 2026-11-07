/* term.c — the grid model (see term.h). The scrollback ring grows
 * in pages; rows are column-arrays the resize path reallocates. */
#include "term.h"
#include "wide.h"

#include <stdlib.h>
#include <string.h>

/* ---- row management ------------------------------------------------- */

static trow *row_alloc(int cols) {
    trow *r = calloc(1, sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    r->cols = cols;
    r->cells = calloc((size_t)cols, sizeof(tcell));
    if (r->cells == NULL) {
        free(r);
        return NULL;
    }
    for (int i = 0; i < cols; i++) {
        r->cells[i].fg = TCOLOR_UNSET;
        r->cells[i].bg = TCOLOR_UNSET;
    }
    return r;
}

static void row_free(trow *r) {
    if (r != NULL) {
        free(r->cells);
        free(r);
    }
}

static void row_resize(trow *r, int cols) {
    if (r->cols == cols) {
        return;
    }
    tcell *nc = calloc((size_t)cols, sizeof(tcell));
    if (nc == NULL) {
        return;
    }
    int keep = r->cols < cols ? r->cols : cols;
    memcpy(nc, r->cells, (size_t)keep * sizeof(tcell));
    for (int i = r->cols; i < cols; i++) {
        nc[i].fg = TCOLOR_UNSET;
        nc[i].bg = TCOLOR_UNSET;
    }
    free(r->cells);
    r->cells = nc;
    r->cols = cols;
}

/* ---- scrollback ring ------------------------------------------------- */

static trow *back_at(const term *t, size_t i);

/* The ring push (shared with vt.c's scroll path through the
 * non-static twin below: the "row leaves the grid" semantics and the
 * capacity/overwrite rules live in exactly one place). */
static void back_push(term *t, trow *r) {
    if (t->back_cap == 0) {
        row_free(r); /* scrollback disabled */
        return;
    }
    if (t->back_count < t->back_cap) {
        /* grow in the ring: allocate a fresh slot */
        t->back[t->back_count] = r;
        t->back_count++;
        return;
    }
    /* ring full: overwrite the oldest (head), advance. */
    row_free(t->back[t->back_head]);
    t->back[t->back_head] = r;
    t->back_head = (t->back_head + 1) % t->back_cap;
}

void term__back_push(term *t, trow *r) {
    back_push(t, r);
}

const trow *term_back_row(const term *t, size_t abs_index) {
    if (t->on_alt || abs_index >= t->back_count) {
        return NULL;
    }
    return back_at(t, abs_index);
}

static trow *back_at(const term *t, size_t i) {
    return t->back[(t->back_head + i) % t->back_cap];
}

/* ---- lifecycle -------------------------------------------------------- */

void term_init(term *t, int cols, int rows, size_t scrollback_lines) {
    memset(t, 0, sizeof *t);
    t->cols = cols;
    t->rows = rows;
    t->scroll_top = 0;
    t->scroll_bot = rows;
    t->autowrap = true;
    t->cursor_visible = true;
    t->pen.fg = TCOLOR_UNSET;
    t->pen.bg = TCOLOR_UNSET;

    t->grid = calloc((size_t)rows, sizeof(trow));
    for (int y = 0; y < rows; y++) {
        trow *r = row_alloc(cols);
        if (r != NULL) {
            t->grid[y] = *r;
            free(r); /* the struct contents moved into the array */
        }
    }
    t->ngrid = rows;

    t->alt = calloc((size_t)rows, sizeof(trow));
    for (int y = 0; y < rows; y++) {
        trow *r = row_alloc(cols);
        if (r != NULL) {
            t->alt[y] = *r;
            free(r);
        }
    }
    t->nalt = rows;

    if (scrollback_lines > 0) {
        t->back_cap = scrollback_lines;
        t->back = calloc(scrollback_lines, sizeof(trow *));
    }
}

void term_free(term *t) {
    for (int y = 0; y < t->ngrid; y++) {
        free(t->grid[y].cells);
    }
    free(t->grid);
    for (int y = 0; y < t->nalt; y++) {
        free(t->alt[y].cells);
    }
    free(t->alt);
    for (size_t i = 0; i < t->back_count && t->back != NULL; i++) {
        trow *r = back_at(t, i);
        free(r->cells);
        free(r);
    }
    free(t->back);
    free(t->reply);
    memset(t, 0, sizeof *t);
}

trow *term_row(term *t, int grid_y) {
    if (grid_y < 0 || grid_y >= t->rows) {
        return NULL;
    }
    return t->on_alt ? &t->alt[grid_y] : &t->grid[grid_y];
}

int64_t term_abs_rows(term *t) {
    return (int64_t)(t->on_alt ? 0 : t->back_count) + t->rows;
}

tcell *term_cell(term *t, int y, int x) {
    trow *r = term_row(t, y);
    if (r == NULL || x < 0 || x >= r->cols) {
        return NULL;
    }
    return &r->cells[x];
}

/* ---- resize ------------------------------------------------------------ */

void term_resize(term *t, int cols, int rows) {
    if (cols < 1 || rows < 1) {
        return;
    }
    if (t->on_alt) {
        /* the alternate grid resizes in place (no scrollback flow) */
        for (int y = 0; y < t->rows; y++) {
            row_resize(&t->alt[y], cols);
        }
        if (rows > t->rows) {
            t->alt = realloc(t->alt, (size_t)rows * sizeof(trow));
            for (int y = t->rows; y < rows; y++) {
                trow *r = row_alloc(cols);
                if (r != NULL) {
                    t->alt[y] = *r;
                    free(r);
                }
            }
        } else if (rows < t->rows) {
            for (int y = rows; y < t->rows; y++) {
                free(t->alt[y].cells);
            }
            t->alt = realloc(t->alt, (size_t)rows * sizeof(trow));
        }
        t->nalt = rows;
        t->cols = cols;
        t->rows = rows;
        t->scroll_top = 0;
        t->scroll_bot = rows;
        if (t->cx >= cols) {
            t->cx = cols - 1;
        }
        if (t->cy >= rows) {
            t->cy = rows - 1;
        }
        return;
    }

    /* Primary: rebuild as a flat list = scrollback + grid, resize,
     * redistribute. */
    size_t total = t->back_count + (size_t)t->rows;
    trow *all = calloc(total ? total : 1, sizeof(trow));
    size_t n = 0;
    for (size_t i = 0; i < t->back_count; i++) {
        trow *r = back_at(t, i);
        all[n++] = *r;
        free(r);
    }
    for (int y = 0; y < t->rows; y++) {
        all[n++] = t->grid[y];
    }
    t->back_count = 0;

    /* re-column every row */
    for (size_t i = 0; i < n; i++) {
        row_resize(&all[i], cols);
    }

    /* row-count change: shrink drops from the TOP (into scrollback
     * semantics is already flat here: just cap), grow adds empties
     * at the bottom — but not beyond what exists. */
    int keep_rows = (int)n < rows ? (int)n : rows;
    size_t head_drop = n > (size_t)keep_rows ? n - (size_t)keep_rows : 0;

    free(t->grid);
    t->grid = calloc((size_t)rows, sizeof(trow));
    for (int y = 0; y < keep_rows; y++) {
        t->grid[y] = all[head_drop + (size_t)y];
    }
    for (int y = keep_rows; y < rows; y++) {
        trow *r = row_alloc(cols);
        if (r != NULL) {
            t->grid[y] = *r;
            free(r);
        }
    }
    /* the dropped head goes back into scrollback (oldest first) */
    for (size_t i = 0; i < head_drop; i++) {
        trow *r = calloc(1, sizeof(trow));
        if (r != NULL) {
            *r = all[i];
            back_push(t, r);
        }
    }
    free(all);

    /* the alt screen tracks the primary's geometry (the alt branch
     * does this for alt-screen resizes; the primary path used to
     * leave alt at its ORIGINAL row count — an OOB the moment the
     * app entered the alt screen, and the close-time overflow in
     * term_free, which walks alt by t->rows) */
    for (int y = 0; y < t->nalt && y < t->rows; y++) {
        row_resize(&t->alt[y], cols);
    }
    if (rows > t->nalt) {
        t->alt = realloc(t->alt, (size_t)rows * sizeof(trow));
        for (int y = t->nalt; y < rows; y++) {
            trow *r = row_alloc(cols);
            if (r != NULL) {
                t->alt[y] = *r;
                free(r);
            }
        }
    } else if (rows < t->nalt) {
        for (int y = rows; y < t->nalt; y++) {
            free(t->alt[y].cells);
        }
        t->alt = realloc(t->alt, (size_t)rows * sizeof(trow));
    }
    t->nalt = rows;

    t->ngrid = rows;
    t->cols = cols;
    t->rows = rows;
    t->scroll_top = 0;
    t->scroll_bot = rows;
    if (t->cx >= cols) {
        t->cx = cols - 1;
    }
    if (t->cy >= rows) {
        t->cy = rows - 1;
    }
}

/* ---- selection --------------------------------------------------------- */

static void norm_sel(const term *t, int *x0, int64_t *y0, int *x1,
                     int64_t *y1) {
    if (*y0 > *y1 || (*y0 == *y1 && *x0 > *x1)) {
        int tx = *x0;
        int64_t ty = *y0;
        *x0 = *x1;
        *y0 = *y1;
        *x1 = tx;
        *y1 = ty;
    }
    (void)t;
}

char *term_selection_text(const term *t) {
    if (!t->sel_active) {
        return NULL;
    }
    int x0 = t->sel_x0, x1 = t->sel_x1;
    int64_t y0 = t->sel_y0, y1 = t->sel_y1;
    if (t->sel_rect) {
        if (x0 > x1) {
            int tx = x0;
            x0 = x1;
            x1 = tx;
        }
        if (y0 > y1) {
            int64_t ty = y0;
            y0 = y1;
            y1 = ty;
        }
    } else {
        norm_sel(t, &x0, &y0, &x1, &y1);
        /* selection end is exclusive in x when on the same-ordered
         * row; simplify: include the start cell of the end corner */
    }

    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }

    int64_t ay0 = y0, ay1 = y1; /* already absolute */

    for (int64_t ay = ay0; ay <= ay1; ay++) {
        const trow *r = NULL;
        if (t->on_alt) {
            if (ay >= 0 && ay < t->rows) {
                r = &t->alt[ay];
            }
        } else {
            if (ay < (int64_t)t->back_count) {
                r = back_at(t, (size_t)ay);
            } else if (ay < (int64_t)t->back_count + t->rows) {
                r = &t->grid[ay - (int64_t)t->back_count];
            }
        }
        if (r == NULL) {
            continue;
        }
        int lx0 = 0, lx1 = r->cols;
        if (t->sel_rect) {
            lx0 = x0;
            lx1 = x1 + 1;
        } else if (ay == ay0) {
            lx0 = x0;
        }
        if (ay == ay1) {
            lx1 = x1 + 1;
        }
        if (lx1 > r->cols) {
            lx1 = r->cols;
        }
        for (int x = lx0; x < lx1; x++) {
            const tcell *c = &r->cells[x];
            if (c->attr & TATTR_WIDE_CONT) {
                continue;
            }
            if (c->cp == 0) {
                continue;
            }
            /* encode the cluster */
            uint32_t cps[3] = {c->cp, c->ncluster > 0 ? c->cluster[0] : 0,
                               c->ncluster > 1 ? c->cluster[1] : 0};
            for (int k = 0; k < 3; k++) {
                uint32_t cp = cps[k];
                if (k > 0 && cp == 0) {
                    break;
                }
                char buf[4];
                int nlen = 0;
                if (cp < 0x80) {
                    buf[nlen++] = (char)cp;
                } else if (cp < 0x800) {
                    buf[nlen++] = (char)(0xC0 | (cp >> 6));
                    buf[nlen++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    buf[nlen++] = (char)(0xE0 | (cp >> 12));
                    buf[nlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[nlen++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[nlen++] = (char)(0xF0 | (cp >> 18));
                    buf[nlen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    buf[nlen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[nlen++] = (char)(0x80 | (cp & 0x3F));
                }
                if (len + (size_t)nlen + 2 >= cap) {
                    cap = cap * 2 + 16;
                    char *nout = realloc(out, cap);
                    if (nout == NULL) {
                        free(out);
                        return NULL;
                    }
                    out = nout;
                }
                memcpy(out + len, buf, (size_t)nlen);
                len += (size_t)nlen;
            }
        }
        if (ay != ay1) {
            if (len + 2 >= cap) {
                cap = cap * 2 + 16;
                char *nout = realloc(out, cap);
                if (nout == NULL) {
                    free(out);
                    return NULL;
                }
                out = nout;
            }
            out[len++] = '\r';
            out[len++] = '\n';
        }
    }
    if (len == 0) {
        free(out);
        return NULL;
    }
    out[len] = '\0';
    return out;
}

void term_word_bounds(const term *t, int64_t abs_y, int x,
                      int *out_x0, int *out_x1) {
    const trow *r = NULL;
    if (t->on_alt) {
        if (abs_y >= 0 && abs_y < t->rows) {
            r = &t->alt[abs_y];
        }
    } else if (abs_y < (int64_t)t->back_count) {
        r = back_at(t, (size_t)abs_y);
    } else if (abs_y < (int64_t)t->back_count + t->rows) {
        r = &t->grid[abs_y - (int64_t)t->back_count];
    }
    if (r == NULL || x < 0 || x >= r->cols) {
        *out_x0 = x;
        *out_x1 = x + 1;
        return;
    }
    /* word chars: alnum + _ + the URL set; spaces are the split */
    int x0 = x, x1 = x;
    while (x0 > 0) {
        const tcell *c = &r->cells[x0 - 1];
        if (c->cp == 0 || c->cp == ' ') {
            break;
        }
        x0--;
    }
    while (x1 < r->cols - 1) {
        const tcell *c = &r->cells[x1 + 1];
        if (c->cp == 0 || c->cp == ' ') {
            break;
        }
        x1++;
    }
    *out_x0 = x0;
    *out_x1 = x1 + 1;
}

/* ---- URLs -------------------------------------------------------------- */

static bool url_char(uint32_t cp) {
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9') ||
           cp == ':' || cp == '/' || cp == '?' || cp == '#' || cp == '=' ||
           cp == '&' || cp == '%' || cp == '+' || cp == '-' || cp == '_' ||
           cp == '.' || cp == '~' || cp == '@' || cp == '!' || cp == '$' ||
           cp == '\'' || cp == '*' || cp == '(' || cp == ')' || cp == ',' ||
           cp == ';';
}

bool term_url_at(const term *t, int64_t abs_y, int x,
                 int *out_x0, int *out_x1) {
    const trow *r = NULL;
    if (t->on_alt) {
        if (abs_y >= 0 && abs_y < t->rows) {
            r = &t->alt[abs_y];
        }
    } else if (abs_y < (int64_t)t->back_count) {
        r = back_at(t, (size_t)abs_y);
    } else if (abs_y < (int64_t)t->back_count + t->rows) {
        r = &t->grid[abs_y - (int64_t)t->back_count];
    }
    if (r == NULL || x < 0 || x >= r->cols) {
        return false;
    }
    /* the START must itself be a URL character: expanding from a
     * space only looks forward and would swallow the next word */
    if (!url_char(r->cells[x].cp)) {
        return false;
    }
    /* expand to the url_char run containing x, then require a
     * scheme prefix inside the run */
    int x0 = x, x1 = x;
    while (x0 > 0 && url_char(r->cells[x0 - 1].cp)) {
        x0--;
    }
    while (x1 < r->cols - 1 && url_char(r->cells[x1 + 1].cp)) {
        x1++;
    }
    /* find "://" or "mailto:"-like prefix within [x0..x1] */
    for (int s = x0; s <= x1 - 3; s++) {
        if (r->cells[s].cp == ':' && r->cells[s + 1].cp == '/' &&
            r->cells[s + 2].cp == '/') {
            /* the scheme is the [x0..s) alnum run */
            bool scheme_ok = s > x0;
            for (int k = x0; k < s; k++) {
                uint32_t cp = r->cells[k].cp;
                if (!((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                      (cp >= '0' && cp <= '9') || cp == '+' || cp == '-' ||
                      cp == '.')) {
                    scheme_ok = false;
                    break;
                }
            }
            if (scheme_ok) {
                *out_x0 = x0;
                *out_x1 = x1 + 1;
                return true;
            }
        }
    }
    return false;
}

/* ---- line text + search -------------------------------------------------- */

char *term_line_text(const term *t, int64_t abs_y) {
    const trow *r = NULL;
    if (t->on_alt) {
        if (abs_y >= 0 && abs_y < t->rows) {
            r = &t->alt[abs_y];
        }
    } else if (abs_y < (int64_t)t->back_count) {
        r = back_at(t, (size_t)abs_y);
    } else if (abs_y < (int64_t)t->back_count + t->rows) {
        r = &t->grid[abs_y - (int64_t)t->back_count];
    }
    if (r == NULL) {
        return NULL;
    }
    size_t cap = (size_t)r->cols * 4 + 8, len = 0;
    char *out = malloc(cap);
    if (out == NULL) {
        return NULL;
    }
    for (int x = 0; x < r->cols; x++) {
        const tcell *c = &r->cells[x];
        if (c->attr & TATTR_WIDE_CONT || c->cp == 0) {
            continue;
        }
        uint32_t cp = c->cp;
        if (len + 4 + 1 >= cap) {
            cap *= 2;
            char *n = realloc(out, cap);
            if (n == NULL) {
                free(out);
                return NULL;
            }
            out = n;
        }
        if (cp < 0x80) {
            out[len++] = (char)cp;
        } else if (cp < 0x800) {
            out[len++] = (char)(0xC0 | (cp >> 6));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[len++] = (char)(0xE0 | (cp >> 12));
            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[len++] = (char)(0xF0 | (cp >> 18));
            out[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[len++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[len] = '\0';
    return out;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int64_t term_search(const term *t, const char *needle, int64_t from,
                    int *out_col) {
    if (needle == NULL || needle[0] == '\0') {
        return -1;
    }
    int64_t total = t->on_alt ? t->rows
                              : (int64_t)t->back_count + t->rows;
    if (total == 0) {
        return -1;
    }
    /* case-fold the needle into a small buffer */
    char fold[128];
    size_t nlen = strlen(needle);
    if (nlen >= sizeof fold) {
        return -1;
    }
    for (size_t i = 0; i <= nlen; i++) {
        fold[i] = ascii_lower(needle[i]);
    }
    for (int64_t off = 0; off < total; off++) {
        int64_t ay = from + off;
        while (ay >= total) {
            ay -= total;
        }
        while (ay < 0) {
            ay += total;
        }
        char *line = term_line_text(t, ay);
        if (line == NULL) {
            continue;
        }
        for (char *p = line; *p != '\0'; p++) {
            /* match fold case-insensitively at p */
            size_t k = 0;
            while (k < nlen && p[k] != '\0' &&
                   ascii_lower(p[k]) == fold[k]) {
                k++;
            }
            if (k == nlen) {
                if (out_col != NULL) {
                    *out_col = 0; /* column mapping is the view's job */
                }
                free(line);
                return ay;
            }
        }
        free(line);
    }
    return -1;
}
