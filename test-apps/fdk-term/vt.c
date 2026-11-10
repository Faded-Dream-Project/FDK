/* vt.c — the xterm-compatible state machine (see vt.h). */
#include "vt.h"
#include "wide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FDK scancode values (fdk_event.h) the key path needs. */
#include "fdk/fdk_event.h"

typedef enum {
    ST_GROUND = 0,
    ST_ESCAPE,
    ST_CSI,
    ST_OSC,
    ST_OSC_ESC,   /* OSC terminated by ESC (ST is ESC \)        */
    ST_DCS,
    ST_DCS_ESC,
    ST_CHARSET,   /* ESC ( or ESC ) waiting for the designator  */
} vstate;

struct vt {
    term *t;
    vstate state;
    uint8_t interp;       /* the ESC-time intermediate (e.g. '(')  */

    /* CSI accumulator */
    int params[16];
    bool pset[16];
    bool psub[16];  /* slot was introduced by ':' (ITU sub-param) */
    bool ppriv;           /* the '?' private marker                */
    int nparams;
    int cur_param;        /* in-progress param (sub-params by ';') */
    bool colon_pending;

    /* OSC/DCS string accumulator */
    char *str;
    size_t str_len, str_cap;

    /* G0/G1 charset designation: 0 = ASCII, 1 = DEC graphics. */
    bool g_is_graphics[2];
    int gl;               /* which of G0/G1 is active (0/1)        */

    /* UTF-8 decoder */
    uint32_t utf8_acc;
    int utf8_need, utf8_seen;

    /* base64 state for OSC 52 */
    int b64_acc, b64_bits;

    /* the accumulated output batch */
    vt_out out;

    /* reply queue */
    char *reply;
    size_t reply_len, reply_cap;
};

/* ---- reply queue ------------------------------------------------------ */

static void reply_put(vt *v, const char *s, size_t n) {
    if (v->reply_len + n + 1 > v->reply_cap) {
        size_t ncap = (v->reply_cap + n) * 2 + 64;
        char *nr = realloc(v->reply, ncap);
        if (nr == NULL) {
            return;
        }
        v->reply = nr;
        v->reply_cap = ncap;
    }
    memcpy(v->reply + v->reply_len, s, n);
    v->reply_len += n;
    v->reply[v->reply_len] = '\0'; /* strlen-friendly; length stays truth */
}

static void reply_str(vt *v, const char *s) {
    reply_put(v, s, strlen(s));
}

const char *vt_reply_peek(const vt *v, size_t *out_len) {
    if (out_len != NULL) {
        *out_len = v->reply_len;
    }
    return v->reply;
}

void vt_reply_consume(vt *parser, size_t n) {
    if (n > parser->reply_len) {
        n = parser->reply_len;
    }
    memmove(parser->reply, parser->reply + n, parser->reply_len - n);
    parser->reply_len -= n;
}

/* ---- string accumulator ------------------------------------------------ */

static void str_reset(vt *v) {
    v->str_len = 0;
    if (v->str != NULL) {
        v->str[0] = '\0';
    }
}

static void str_put(vt *v, char c) {
    if (v->str_len + 2 > v->str_cap) {
        size_t ncap = (v->str_cap + 32) * 2;
        char *ns = realloc(v->str, ncap);
        if (ns == NULL) {
            return;
        }
        v->str = ns;
        v->str_cap = ncap;
    }
    v->str[v->str_len++] = c;
    v->str[v->str_len] = '\0';
}

/* ---- grid operations (the parser's term mutations) --------------------- */

static void clamp_cursor(term *t) {
    if (t->cx < 0) {
        t->cx = 0;
    }
    if (t->cx >= t->cols) {
        t->cx = t->cols - 1;
    }
    if (t->cy < 0) {
        t->cy = 0;
    }
    if (t->cy >= t->rows) {
        t->cy = t->rows - 1;
    }
}

static void scroll_up(term *t, int n) {
    if (n <= 0) {
        return;
    }
    if (n > t->scroll_bot - t->scroll_top) {
        n = t->scroll_bot - t->scroll_top;
    }
    /* rows [top, bot-n) <- [top+n, bot); the tail clears */
    for (int y = t->scroll_top; y < t->scroll_bot - n; y++) {
        trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
        trow *src = t->on_alt ? &t->alt[y + n] : &t->grid[y + n];
        memcpy(dst->cells, src->cells, (size_t)t->cols * sizeof(tcell));
        dst->wrapped = src->wrapped;
    }
    for (int y = t->scroll_bot - n; y < t->scroll_bot; y++) {
        trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
        memset(dst->cells, 0, (size_t)t->cols * sizeof(tcell));
        for (int x = 0; x < t->cols; x++) {
            dst->cells[x].fg = TCOLOR_UNSET;
            dst->cells[x].bg = TCOLOR_UNSET;
        }
        dst->wrapped = false;
    }
}

static void scroll_down(term *t, int n) {
    if (n <= 0) {
        return;
    }
    if (n > t->scroll_bot - t->scroll_top) {
        n = t->scroll_bot - t->scroll_top;
    }
    for (int y = t->scroll_bot - 1; y >= t->scroll_top + n; y--) {
        trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
        trow *src = t->on_alt ? &t->alt[y - n] : &t->grid[y - n];
        memcpy(dst->cells, src->cells, (size_t)t->cols * sizeof(tcell));
        dst->wrapped = src->wrapped;
    }
    for (int y = t->scroll_top; y < t->scroll_top + n; y++) {
        trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
        memset(dst->cells, 0, (size_t)t->cols * sizeof(tcell));
        for (int x = 0; x < t->cols; x++) {
            dst->cells[x].fg = TCOLOR_UNSET;
            dst->cells[x].bg = TCOLOR_UNSET;
        }
        dst->wrapped = false;
    }
}

/* The bottom-of-screen newline: pushes the top grid row into
 * scrollback (primary grid only, and only when the cursor sits in
 * the bottom margin). */
static void linefeed_scroll(term *t) {
    if (t->cy == t->rows - 1 || t->cy == t->scroll_bot - 1) {
        if (!t->on_alt && t->scroll_bot == t->rows) {
            /* scroll the whole grid; row 0 goes to scrollback */
            trow *r = calloc(1, sizeof(trow));
            if (r != NULL) {
                r->cols = t->cols;
                r->cells = malloc((size_t)t->cols * sizeof(tcell));
                if (r->cells != NULL) {
                    memcpy(r->cells, t->grid[0].cells,
                           (size_t)t->cols * sizeof(tcell));
                    r->wrapped = t->grid[0].wrapped;
                } else {
                    r->cols = 0;
                }
            }
            for (int y = 0; y < t->rows - 1; y++) {
                trow *dst = &t->grid[y];
                trow *src = &t->grid[y + 1];
                memcpy(dst->cells, src->cells,
                       (size_t)t->cols * sizeof(tcell));
                dst->wrapped = src->wrapped;
            }
            trow *last = &t->grid[t->rows - 1];
            memset(last->cells, 0, (size_t)t->cols * sizeof(tcell));
            for (int x = 0; x < t->cols; x++) {
                last->cells[x].fg = TCOLOR_UNSET;
                last->cells[x].bg = TCOLOR_UNSET;
            }
            last->wrapped = false;
            /* hand the detached copy to the ring */
            extern void term__back_push(term *t, trow *r);
            term__back_push(t, r);
        } else {
            scroll_up(t, 1);
        }
    } else if (t->cy < t->rows - 1) {
        t->cy++;
    }
}

/* Places a codepoint at the cursor with the current pen, advancing;
 * handles wide chars (the trail cell), combining marks (append to
 * the previous cell's cluster), and the wrap-pending state xterm
 * implements via "cursor parked at the right margin". */
static void put_glyph(vt *v, uint32_t cp) {
    term *t = v->t;

    int w = tw_width(cp);
    if (w == 0 && cp >= 0x20) {
        /* combining mark: attach to the cell behind the cursor */
        int px = t->cx - 1;
        int py = t->cy;
        if (px < 0 && py > 0) {
            py--;
            px = t->cols - 1;
        }
        trow *r = t->on_alt ? &t->alt[py] : &t->grid[py];
        if (px >= 0 && px < t->cols && r->cells[px].ncluster < 2) {
            r->cells[px].cluster[r->cells[px].ncluster++] = cp;
        }
        return;
    }

    if (t->insert_mode && w > 0) {
        /* ICH-lite: shift the tail right */
        trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
        for (int x = t->cols - 1; x > t->cx; x--) {
            r->cells[x] = r->cells[x - 1];
        }
    }

    /* the DEC graphics translation happens at placement */
    uint32_t out_cp = cp;
    if (v->g_is_graphics[v->gl] && cp >= 0x60 && cp < 0x7B) {
        static const uint32_t decgr[27] = {
            0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
            0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514,
            0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C,
            0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265,
        };
        out_cp = decgr[cp - 0x60];
    }

    trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
    if (t->cx >= t->cols) {
        /* wrap-pending overflow: wrap unless disabled */
        if (t->autowrap) {
            r->wrapped = true;
            t->cx = 0;
            if (t->cy + 1 >= t->rows || t->cy + 1 >= t->scroll_bot) {
                linefeed_scroll(t);
            } else {
                t->cy++;
            }
            r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
        } else {
            t->cx = t->cols - 1;
        }
    }

    /* wide char at the last column: wraps (or degrades to a space) */
    if (w == 2 && t->cx == t->cols - 1) {
        if (t->autowrap) {
            r->wrapped = true;
            t->cx = 0;
            if (t->cy + 1 >= t->rows || t->cy + 1 >= t->scroll_bot) {
                linefeed_scroll(t);
            } else {
                t->cy++;
            }
            r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
        } else {
            w = 1;
            out_cp = ' ';
        }
    }

    tcell *c = &r->cells[t->cx];
    memset(c, 0, sizeof *c);
    c->cp = out_cp;
    c->fg = t->pen.fg;
    c->bg = t->pen.bg;
    c->attr = t->pen.attr;
    if (t->cx > 0) {
        c->hyperlink = r->cells[t->cx - 1].hyperlink;
    }
    t->cx++;
    if (w == 2 && t->cx < t->cols) {
        tcell *tc = &r->cells[t->cx];
        memset(tc, 0, sizeof *tc);
        tc->attr = TATTR_WIDE_CONT | (t->pen.attr & ~TATTR_BOLD);
        tc->fg = t->pen.fg;
        tc->bg = t->pen.bg;
        tc->hyperlink = c->hyperlink;
        t->cx++;
    }
    v->out.dirty = true;
}

/* ---- OSC handlers ------------------------------------------------------ */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static char *b64_decode_range(const char *s, size_t n, size_t *out_len) {
    char *buf = malloc(n + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t olen = 0;
    int acc = 0, bits = 0;
    for (size_t i = 0; i < n; i++) {
        int val = b64_val(s[i]);
        if (val < 0) {
            continue; /* skip padding/whitespace */
        }
        acc = (acc << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[olen++] = (char)((acc >> bits) & 0xFF);
        }
    }
    buf[olen] = '\0';
    *out_len = olen;
    return buf;
}

static bool vt_trace_on(void) {
    static int on = -1;
    if (on < 0) {
        on = getenv("FDK_VT_TRACE") != NULL;
    }
    return on;
}

static void osc_dispatch(vt *v) {
    term *t = v->t;
    const char *s = v->str != NULL ? v->str : "";
    /* the number prefix */
    int num = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        num = num * 10 + (s[i] - '0');
        i++;
    }
    if (vt_trace_on()) {
        fprintf(stderr, "[vt] OSC %d len=%zu\n", num, v->str_len);
    }
    const char *rest = s + i;
    if (*rest == ';') {
        rest++;
    }
    switch (num) {
    case 0:
    case 2: {
        free(v->out.title);
        v->out.title = strdup(rest);
        v->out.title_changed = true;
        (void)t;
        break;
    }
    case 7: {
        free(v->out.cwd);
        v->out.cwd = strdup(rest);
        break;
    }
    case 8: {
        /* OSC 8 ; params ; URI ST — mark the hyperlink range. The
         * range ENDS at the next OSC 8 with an empty URI. We mark
         * per-cell as glyphs are placed (put_glyph copies the flag
         * forward); toggling here arms/disarms the pen. */
        /* params may be empty; find the URI after the second ';' */
        const char *uri = strchr(rest, ';');
        if (uri != NULL) {
            uri++;
            bool on = uri[0] != '\0';
            /* mark the CURRENT cell onward by setting a flag the
             * put_glyph chain reads (we reuse pen attr bit 9). */
            if (on) {
                v->t->pen.attr |= (1u << 9); /* hyperlink pen */
            } else {
                v->t->pen.attr &= ~(1u << 9);
            }
        }
        break;
    }
    case 52: {
        /* OSC 52 ; clipboard(;selections) ; base64 — the payload's
         * ';' splits vary by client; accept [;c;data] and [;;data]. */
        const char *data = rest;
        const char *semi = strchr(rest, ';');
        if (semi != NULL) {
            data = semi + 1;
        }
        if (data[0] == '\0') {
            v->out.request_clipboard_read = true;
        } else {
            size_t dlen = strlen(data);
            size_t olen = 0;
            char *txt = b64_decode_range(data, dlen, &olen);
            if (txt != NULL && olen > 0) {
                free(v->out.clipboard);
                v->out.clipboard = txt;
                v->out.clipboard_set = true;
            } else {
                free(txt);
            }
        }
        break;
    }
    case 133: /* shell integration markers: consumed, no-op */
    case 777: /* same family via a different prefix */
    default:
        break;
    }
}

/* ---- CSI handlers ------------------------------------------------------- */

static void sgr(vt *v) {
    term *t = v->t;
    tcell *pen = &t->pen;
    int i = 0;
    if (!v->pset[0]) {
        /* lone SGR: reset */
        pen->attr = 0;
        pen->fg = TCOLOR_UNSET;
        pen->bg = TCOLOR_UNSET;
        return;
    }
    while (i < v->nparams && i < 16) {
        int p = v->params[i];
        switch (p) {
        case 0:
            pen->attr = 0;
            pen->fg = TCOLOR_UNSET;
            pen->bg = TCOLOR_UNSET;
            break;
        case 1: pen->attr |= TATTR_BOLD; break;
        case 2: pen->attr |= TATTR_FAINT; break;
        case 3: pen->attr |= TATTR_ITALIC; break;
        case 4: pen->attr |= TATTR_UNDERLINE; break;
        case 5: pen->attr |= TATTR_BLINK; break;
        case 7: pen->attr |= TATTR_INVERSE; break;
        case 8: pen->attr |= TATTR_INVISIBLE; break;
        case 9: pen->attr |= TATTR_STRIKE; break;
        case 21: pen->attr &= ~TATTR_BOLD; break;
        case 22: pen->attr &= ~(TATTR_BOLD | TATTR_FAINT); break;
        case 23: pen->attr &= ~TATTR_ITALIC; break;
        case 24: pen->attr &= ~TATTR_UNDERLINE; break;
        case 25: pen->attr &= ~TATTR_BLINK; break;
        case 27: pen->attr &= ~TATTR_INVERSE; break;
        case 28: pen->attr &= ~TATTR_INVISIBLE; break;
        case 29: pen->attr &= ~TATTR_STRIKE; break;
        case 39: pen->fg = TCOLOR_UNSET; break;
        case 49: pen->bg = TCOLOR_UNSET; break;
        case 38:
        case 48: {
            tcolor tc = TCOLOR_UNSET;
            if (i + 1 < 16) {
                int mode = v->pset[i + 1] ? v->params[i + 1] : -1;
                if (mode == 5 && i + 2 < 16 && v->pset[i + 2]) {
                    tc.index = v->params[i + 2] & 0xFF;
                    i += 2;
                } else if (mode == 2) {
                    int j = i + 2;
                    if (v->psub[i + 1]) {
                        /* colon form: 38:2:R:G:B (ncurses),
                         * 38:2::R:G:B (empty ITU colorspace) and
                         * 38:2:CS:R:G:B all land here — with four
                         * or more sub-slots the first one is the
                         * colorspace, not red */
                        int k = j;
                        while (k < 16 && k < v->nparams &&
                               v->psub[k]) {
                            k++;
                        }
                        if (k - j >= 4) {
                            j++;
                        }
                    }
                    if (j + 2 < 16 && v->pset[j] &&
                        v->pset[j + 1] && v->pset[j + 2]) {
                        tc.index = TCOLOR_TRUECOLOR;
                        tc.r = (uint8_t)v->params[j];
                        tc.g = (uint8_t)v->params[j + 1];
                        tc.b = (uint8_t)v->params[j + 2];
                        i = j + 2;
                    }
                }
            }
            if (p == 38) {
                pen->fg = tc;
            } else {
                pen->bg = tc;
            }
            break;
        }
        default:
            if (p >= 30 && p <= 37) {
                pen->fg.index = p - 30;
            } else if (p >= 90 && p <= 97) {
                pen->fg.index = p - 90 + 8;
            } else if (p >= 40 && p <= 47) {
                pen->bg.index = p - 40;
            } else if (p >= 100 && p <= 107) {
                pen->bg.index = p - 100 + 8;
            }
            break;
        }
        i++;
    }
}

static void erase_cells(term *t, int y, int x0, int x1) {
    trow *r = t->on_alt ? &t->alt[y] : &t->grid[y];
    for (int x = x0; x < x1 && x < t->cols; x++) {
        tcell *c = &r->cells[x];
        memset(c, 0, sizeof *c);
        c->fg = TCOLOR_UNSET;
        c->bg = TCOLOR_UNSET;
    }
}

static void csi_dispatch(vt *v, uint8_t final_byte) {
    term *t = v->t;
    int n0 = v->pset[0] ? v->params[0] : 0;
    int n1 = v->pset[1] ? v->params[1] : 0;
    if (n0 == 0) {
        n0 = 1; /* the default-1 crowd applies it below per-case */
    }
    (void)n1;

    if (v->ppriv) {
        /* DEC private modes */
        bool set = final_byte == 'h';
        bool reset = final_byte == 'l';
        for (int i = 0; i < v->nparams && i < 16; i++) {
            int m = v->params[i];
            switch (m) {
            case 1: t->app_cursor_keys = set; break;
            case 3:
                /* DECCOLM: we do not re-size on demand; clear home */
                if (set || reset) {
                    for (int y = 0; y < t->rows; y++) {
                        erase_cells(t, y, 0, t->cols);
                    }
                    t->cx = 0;
                    t->cy = 0;
                }
                break;
            case 4: t->insert_mode = set; break;
            case 5: /* DECSCNM (reverse video): paint-side, ignored */ break;
            case 6: t->origin_mode = set; break;
            case 7: t->autowrap = set; break;
            case 9:
                t->mouse_mode = set ? 9 : 0;
                break;
            case 12: t->cursor_blink = set; break;
            case 25: t->cursor_visible = set; break;
            case 47:
            case 1047:
                /* alt screen without cursor save */
                if (set && !t->on_alt) {
                    t->on_alt = true;
                    t->cx = 0;
                    t->cy = 0;
                } else if (reset && t->on_alt) {
                    t->on_alt = false;
                    t->cx = 0;
                    t->cy = 0;
                }
                v->out.dirty = true;
                break;
            case 1000: t->mouse_mode = set ? 1000 : 0; break;
            case 1002: t->mouse_mode = set ? 1002 : 0; break;
            case 1003: t->mouse_mode = set ? 1003 : 0; break;
            case 1005: t->mouse_utf8 = set; break;
            case 1006: t->mouse_sgr = set; break;
            case 1034: /* 8-bit meta key: N/A here */ break;
            case 2004: t->bracketed_paste = set; break;
            case 1048:
                if (set) {
                    t->saved.cx = t->cx;
                    t->saved.cy = t->cy;
                    t->saved.pen = t->pen;
                } else {
                    t->cx = t->saved.cx;
                    t->cy = t->saved.cy;
                    t->pen = t->saved.pen;
                }
                clamp_cursor(t);
                break;
            case 1049:
                if (set) {
                    /* save cursor, switch to a CLEARED alt screen */
                    t->saved.cx = t->cx;
                    t->saved.cy = t->cy;
                    t->saved.pen = t->pen;
                    t->on_alt = true;
                    for (int y = 0; y < t->rows; y++) {
                        erase_cells(t, y, 0, t->cols);
                    }
                    t->cx = 0;
                    t->cy = 0;
                } else {
                    t->on_alt = false;
                    t->cx = t->saved.cx;
                    t->cy = t->saved.cy;
                    t->pen = t->saved.pen;
                    clamp_cursor(t);
                }
                v->out.dirty = true;
                break;
            default:
                break;
            }
        }
        return;
    }

    switch (final_byte) {
    case '@': { /* ICH */
        int n = n0;
        trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
        for (int x = t->cols - 1; x >= t->cx + n; x--) {
            r->cells[x] = r->cells[x - n];
        }
        for (int x = t->cx; x < t->cx + n && x < t->cols; x++) {
            memset(&r->cells[x], 0, sizeof(tcell));
            r->cells[x].fg = TCOLOR_UNSET;
            r->cells[x].bg = TCOLOR_UNSET;
        }
        break;
    }
    case 'A': t->cy -= n0; clamp_cursor(t); break;
    case 'B': t->cy += n0; clamp_cursor(t); break;
    case 'C': t->cx += n0; clamp_cursor(t); break;
    case 'D': t->cx -= n0; clamp_cursor(t); break;
    case 'E': t->cy += n0; t->cx = 0; clamp_cursor(t); break;
    case 'F': t->cy -= n0; t->cx = 0; clamp_cursor(t); break;
    case 'G': t->cx = (v->pset[0] ? v->params[0] : 1) - 1; clamp_cursor(t); break;
    case 'H':
    case 'f': {
        int row = v->pset[0] && v->params[0] > 0 ? v->params[0] : 1;
        int col = v->pset[1] && v->params[1] > 0 ? v->params[1] : 1;
        if (t->origin_mode) {
            t->cy = t->scroll_top + row - 1;
        } else {
            t->cy = row - 1;
        }
        t->cx = col - 1;
        clamp_cursor(t);
        break;
    }
    case 'I': /* CHT: tab forward n */
        for (int k = 0; k < n0; k++) {
            t->cx = ((t->cx / 8) + 1) * 8;
            if (t->cx >= t->cols) {
                t->cx = t->cols - 1;
            }
        }
        break;
    case 'J': { /* ED */
        int mode = v->pset[0] ? v->params[0] : 0;
        if (mode == 0) {
            erase_cells(t, t->cy, t->cx, t->cols);
            for (int y = t->cy + 1; y < t->rows; y++) {
                erase_cells(t, y, 0, t->cols);
            }
        } else if (mode == 1) {
            for (int y = 0; y < t->cy; y++) {
                erase_cells(t, y, 0, t->cols);
            }
            erase_cells(t, t->cy, 0, t->cx + 1);
        } else {
            for (int y = 0; y < t->rows; y++) {
                erase_cells(t, y, 0, t->cols);
            }
        }
        v->out.dirty = true;
        break;
    }
    case 'K': { /* EL */
        int mode = v->pset[0] ? v->params[0] : 0;
        if (mode == 0) {
            erase_cells(t, t->cy, t->cx, t->cols);
        } else if (mode == 1) {
            erase_cells(t, t->cy, 0, t->cx + 1);
        } else {
            erase_cells(t, t->cy, 0, t->cols);
        }
        v->out.dirty = true;
        break;
    }
    case 'L': { /* IL */
        int n = n0;
        trow tmp;
        for (int k = 0; k < n; k++) {
            for (int y = t->scroll_bot - 1; y > t->cy; y--) {
                trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
                trow *src = t->on_alt ? &t->alt[y - 1] : &t->grid[y - 1];
                tmp = *dst;
                *dst = *src;
                *src = tmp;
            }
            trow *first = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
            memset(first->cells, 0, (size_t)t->cols * sizeof(tcell));
            for (int x = 0; x < t->cols; x++) {
                first->cells[x].fg = TCOLOR_UNSET;
                first->cells[x].bg = TCOLOR_UNSET;
            }
        }
        v->out.dirty = true;
        break;
    }
    case 'M': { /* DL */
        int n = n0;
        trow tmp;
        for (int k = 0; k < n; k++) {
            for (int y = t->cy; y < t->scroll_bot - 1; y++) {
                trow *dst = t->on_alt ? &t->alt[y] : &t->grid[y];
                trow *src = t->on_alt ? &t->alt[y + 1] : &t->grid[y + 1];
                tmp = *dst;
                *dst = *src;
                *src = tmp;
            }
            trow *last = t->on_alt ? &t->alt[t->scroll_bot - 1]
                                   : &t->grid[t->scroll_bot - 1];
            memset(last->cells, 0, (size_t)t->cols * sizeof(tcell));
            for (int x = 0; x < t->cols; x++) {
                last->cells[x].fg = TCOLOR_UNSET;
                last->cells[x].bg = TCOLOR_UNSET;
            }
        }
        v->out.dirty = true;
        break;
    }
    case 'P': { /* DCH */
        int n = n0;
        trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
        for (int x = t->cx; x + n < t->cols; x++) {
            r->cells[x] = r->cells[x + n];
        }
        for (int x = t->cols - n; x < t->cols; x++) {
            memset(&r->cells[x], 0, sizeof(tcell));
            r->cells[x].fg = TCOLOR_UNSET;
            r->cells[x].bg = TCOLOR_UNSET;
        }
        v->out.dirty = true;
        break;
    }
    case 'S': scroll_up(t, n0); v->out.dirty = true; break;
    case 'T': scroll_down(t, n0); v->out.dirty = true; break;
    case 'X': { /* ECH */
        int n = n0;
        erase_cells(t, t->cy, t->cx, t->cx + n);
        v->out.dirty = true;
        break;
    }
    case 'd': { /* VPA */
        t->cy = (v->pset[0] && v->params[0] > 0 ? v->params[0] : 1) - 1;
        clamp_cursor(t);
        break;
    }
    case 'r': { /* DECSTBM */
        int top = v->pset[0] && v->params[0] > 0 ? v->params[0] - 1 : 0;
        int bot = v->pset[1] && v->params[1] > 0 ? v->params[1] : t->rows;
        if (top < bot && bot <= t->rows) {
            t->scroll_top = top;
            t->scroll_bot = bot;
        }
        t->cx = 0;
        t->cy = t->origin_mode ? t->scroll_top : 0;
        break;
    }
    case 'm': sgr(v); v->out.dirty = true; break;
    case 'h': /* ANSI modes: IRM (4) handled in ppriv? No — ANSI SM/RM */
        for (int i = 0; i < v->nparams && i < 16; i++) {
            if (v->params[i] == 4) {
                t->insert_mode = true;
            } else if (v->params[i] == 20) {
                t->autowrap = true; /* LNM: newline does CR too */
            }
        }
        break;
    case 'l':
        for (int i = 0; i < v->nparams && i < 16; i++) {
            if (v->params[i] == 4) {
                t->insert_mode = false;
            } else if (v->params[i] == 20) {
                t->autowrap = false;
            }
        }
        break;
    case 'c': /* DA1: we are a VT102-ish */
        reply_str(v, "\033[?6c");
        break;
    case 'n':
        if (v->pset[0] && v->params[0] == 5) {
            reply_str(v, "\033[0n"); /* status: OK */
        } else if (v->pset[0] && v->params[0] == 6) {
            char buf[32];
            snprintf(buf, sizeof buf, "\033[%d;%dR", t->cy + 1,
                     t->cx + 1);
            reply_str(v, buf);
        }
        break;
    case 't':
        /* window ops: report size (18), pixels (14) — the app layer
         * fills these; the parser reports cell size via DA-style
         * replies only for 18. */
        if (v->pset[0] && v->params[0] == 18) {
            char buf[48];
            snprintf(buf, sizeof buf, "\033[8;%d;%dt", t->rows, t->cols);
            reply_str(v, buf);
        }
        break;
    case 'u': /* restore cursor (SCO) */
        t->cx = t->saved.cx;
        t->cy = t->saved.cy;
        clamp_cursor(t);
        break;
    case 's': /* save cursor (SCO) */
        t->saved.cx = t->cx;
        t->saved.cy = t->cy;
        break;
    default:
        break;
    }
}

/* ---- the state machine --------------------------------------------------- */

vt *vt_create(term *t) {
    vt *v = calloc(1, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->t = t;
    v->state = ST_GROUND;
    return v;
}

void vt_destroy(vt *parser) {
    if (parser != NULL) {
        free(parser->str);
        free(parser->reply);
        free(parser->out.title);
        free(parser->out.cwd);
        free(parser->out.clipboard);
        free(parser);
    }
}

bool vt_busy(const vt *parser) {
    return parser->state != ST_GROUND || parser->utf8_need > 0;
}

static void feed_codepoint(vt *v, uint32_t cp) {
    term *t = v->t;
    switch (v->state) {
    case ST_GROUND:
        if (cp < 0x20) {
            switch (cp) {
            case 0x07: v->out.bell = true; t->bell_pending = true; break;
            case 0x08:
                if (t->cx > 0) {
                    t->cx--;
                    /* step back over the wide-trail marker */
                    trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
                    if (t->cx > 0 &&
                        (r->cells[t->cx].attr & TATTR_WIDE_CONT)) {
                        t->cx--;
                    }
                }
                break;
            case 0x09: {
                int next = ((t->cx / 8) + 1) * 8;
                t->cx = next >= t->cols ? t->cols - 1 : next;
                break;
            }
            case 0x0A:
            case 0x0B:
            case 0x0C:
                linefeed_scroll(t);
                break;
            case 0x0D: t->cx = 0; break;
            case 0x0E: v->gl = 1; break; /* SO: G1 */
            case 0x0F: v->gl = 0; break; /* SI: G0 */
            case 0x1B:
                v->state = ST_ESCAPE;
                break;
            default:
                break;
            }
        } else if (cp >= 0x80 && cp < 0xA0) {
            /* 8-bit C1: treat as the ESC-equivalent */
            if (cp == 0x9B) {
                v->state = ST_CSI;
                v->nparams = 0;
                v->ppriv = false;
                memset(v->pset, 0, sizeof v->pset);
                memset(v->psub, 0, sizeof v->psub);
            } else if (cp == 0x90) {
                v->state = ST_DCS;
                str_reset(v);
            } else if (cp == 0x9D) {
                v->state = ST_OSC;
                str_reset(v);
            }
        } else {
            put_glyph(v, cp);
        }
        break;
    case ST_ESCAPE:
        switch (cp) {
        case '[':
            v->state = ST_CSI;
            v->nparams = 0;
            v->ppriv = false;
            v->cur_param = 0;
            memset(v->pset, 0, sizeof v->pset);
            memset(v->psub, 0, sizeof v->psub);
            break;
        case ']':
            v->state = ST_OSC;
            str_reset(v);
            break;
        case 'P':
            v->state = ST_DCS;
            str_reset(v);
            break;
        case '(':
        case ')':
            v->state = ST_CHARSET;
            v->interp = (uint8_t)cp;
            break;
        case '7': /* DECSC */
            t->saved.cx = t->cx;
            t->saved.cy = t->cy;
            t->saved.pen = t->pen;
            t->saved.origin_mode = t->origin_mode;
            v->state = ST_GROUND;
            break;
        case '8': /* DECRC */
            t->cx = t->saved.cx;
            t->cy = t->saved.cy;
            t->pen = t->saved.pen;
            t->origin_mode = t->saved.origin_mode;
            clamp_cursor(t);
            v->state = ST_GROUND;
            break;
        case 'M': /* RI: reverse linefeed */
            if (t->cy == t->scroll_top) {
                scroll_down(t, 1);
            } else if (t->cy > 0) {
                t->cy--;
            }
            v->state = ST_GROUND;
            v->out.dirty = true;
            break;
        case 'D': /* IND */
            if (t->cy == t->scroll_bot - 1) {
                scroll_up(t, 1);
            } else if (t->cy < t->rows - 1) {
                t->cy++;
            }
            v->state = ST_GROUND;
            v->out.dirty = true;
            break;
        case 'E': /* NEL */
            t->cx = 0;
            if (t->cy == t->scroll_bot - 1) {
                scroll_up(t, 1);
            } else if (t->cy < t->rows - 1) {
                t->cy++;
            }
            v->state = ST_GROUND;
            v->out.dirty = true;
            break;
        case 'c': /* RIS: full reset */
            for (int y = 0; y < t->rows; y++) {
                erase_cells(t, y, 0, t->cols);
            }
            t->cx = 0;
            t->cy = 0;
            t->scroll_top = 0;
            t->scroll_bot = t->rows;
            t->pen.attr = 0;
            t->pen.fg = TCOLOR_UNSET;
            t->pen.bg = TCOLOR_UNSET;
            t->on_alt = false;
            v->state = ST_GROUND;
            v->out.dirty = true;
            break;
        case '=': t->app_keypad = true; v->state = ST_GROUND; break;
        case '>': t->app_keypad = false; v->state = ST_GROUND; break;
        case '\\': /* ST: terminates a dangling OSC/DCS */
            v->state = ST_GROUND;
            break;
        default:
            v->state = ST_GROUND;
            break;
        }
        break;
    case ST_CSI:
        if (cp >= '0' && cp <= '9') {
            v->cur_param = v->cur_param * 10 + (int)(cp - '0');
            if (v->nparams < 16) {
                v->params[v->nparams] = v->cur_param;
                v->pset[v->nparams] = true;
            }
        } else if (cp == ';') {
            if (v->nparams < 15) {
                v->nparams++;
            }
            v->cur_param = 0;
        } else if (cp == ':') {
            /* sub-parameter: a separate slot that the SGR 38/48
             * handler must reassemble (the ITU 38:2::R:G:B form) */
            if (v->nparams < 15) {
                v->nparams++;
                v->psub[v->nparams] = true;
            }
            v->cur_param = 0;
        } else if (cp == '?') {
            v->ppriv = true;
        } else if (cp == '>' || cp == '<' || cp == '=') {
            /* other private markers: ignored */
        } else if (cp == 0x1B) {
            v->state = ST_ESCAPE;
        } else if (cp >= 0x40 && cp <= 0x7E) {
            /* finalize: params seen = separators + a trailing one
             * when any digits accumulated at this slot */
            if (v->nparams < 16 && v->pset[v->nparams]) {
                v->nparams++;
            } else if (v->nparams == 0 && !v->pset[0]) {
                /* no params at all: the default-1 crowd */
            }
            if (vt_trace_on()) {
                fprintf(stderr,
                        "[vt] CSI final='%c' priv=%d nparams=%d"
                        " p=[%d %d %d %d %d %d %d %d]"
                        " sub=[%d%d%d%d%d%d%d%d]\n",
                        cp, v->ppriv, v->nparams,
                        v->pset[0] ? v->params[0] : -1,
                        v->pset[1] ? v->params[1] : -1,
                        v->pset[2] ? v->params[2] : -1,
                        v->pset[3] ? v->params[3] : -1,
                        v->pset[4] ? v->params[4] : -1,
                        v->pset[5] ? v->params[5] : -1,
                        v->pset[6] ? v->params[6] : -1,
                        v->pset[7] ? v->params[7] : -1,
                        v->psub[0], v->psub[1], v->psub[2],
                        v->psub[3], v->psub[4], v->psub[5],
                        v->psub[6], v->psub[7]);
            }
            csi_dispatch(v, (uint8_t)cp);
            v->state = ST_GROUND;
        } else if (cp == 0x18 || cp == 0x1A) {
            v->state = ST_GROUND; /* CAN/SUB abort */
        } else {
            /* ignore other intermediates (space through /) */
        }
        break;
    case ST_OSC:
        if (cp == 0x07) {
            osc_dispatch(v);
            v->state = ST_GROUND;
        } else if (cp == 0x1B) {
            v->state = ST_OSC_ESC;
        } else if (cp == 0x9C) {
            osc_dispatch(v);
            v->state = ST_GROUND;
        } else {
            str_put(v, (char)cp);
        }
        break;
    case ST_OSC_ESC:
        if (cp == '\\') {
            osc_dispatch(v);
        }
        v->state = ST_GROUND;
        break;
    case ST_DCS:
        if (cp == 0x1B) {
            v->state = ST_DCS_ESC;
        } else if (cp == 0x9C || cp == 0x07) {
            v->state = ST_GROUND;
        } else {
            str_put(v, (char)cp); /* consumed; no DCS replies yet */
        }
        break;
    case ST_DCS_ESC:
        v->state = ST_GROUND;
        break;
    case ST_CHARSET:
        if (cp == '0') {
            v->g_is_graphics[v->interp == ')' ? 1 : 0] = true;
        } else if (cp == 'B' || cp == 'U') {
            v->g_is_graphics[v->interp == ')' ? 1 : 0] = false;
        }
        v->state = ST_GROUND;
        break;
    default:
        v->state = ST_GROUND;
        break;
    }
}

void vt_feed(vt *v, const char *bytes, size_t n, vt_out *out) {
    memset(&v->out, 0, sizeof v->out);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)bytes[i];
        if (v->utf8_need > 0) {
            if ((b & 0xC0) == 0x80) {
                v->utf8_acc =
                    (v->utf8_acc << 6) | (uint32_t)(b & 0x3F);
                v->utf8_seen++;
                if (v->utf8_seen >= v->utf8_need) {
                    feed_codepoint(v, v->utf8_acc);
                    v->utf8_need = 0;
                }
            } else {
                /* broken sequence: emit replacement, reprocess */
                feed_codepoint(v, 0xFFFD);
                v->utf8_need = 0;
                i--; /* reprocess this byte */
            }
            continue;
        }
        if (b < 0x80) {
            feed_codepoint(v, b);
        } else if ((b & 0xE0) == 0xC0) {
            v->utf8_acc = (uint32_t)(b & 0x1F);
            v->utf8_need = 1;
            v->utf8_seen = 0;
        } else if ((b & 0xF0) == 0xE0) {
            v->utf8_acc = (uint32_t)(b & 0x0F);
            v->utf8_need = 2;
            v->utf8_seen = 0;
        } else if ((b & 0xF8) == 0xF0) {
            v->utf8_acc = (uint32_t)(b & 0x07);
            v->utf8_need = 3;
            v->utf8_seen = 0;
        } else {
            feed_codepoint(v, 0xFFFD);
        }
    }
    if (out != NULL) {
        *out = v->out;
    }
    v->t->last_output_ms = -1; /* caller stamps with a real clock */
}

/* ---- mouse + paste + keys ---------------------------------------------- */

void vt_mouse(vt *v, int bstate, int button, int x, int y,
              bool shift, bool alt, bool ctrl) {
    term *t = v->t;
    if (t->mouse_mode == 0 || t->mouse_mode == 9) {
        if (t->mouse_mode == 9 && bstate == 0) {
            /* X10: press-only reports, no coords beyond 223 */
            if (x + 33 > 223 || y + 33 > 223) {
                return;
            }
            char buf[8];
            snprintf(buf, sizeof buf, "\033[M%c%c%c", (char)(32 + button),
                     (char)(32 + x + 1), (char)(32 + y + 1));
            reply_str(v, buf);
        }
        return;
    }
    int mods = (shift ? 4 : 0) + (alt ? 8 : 0) + (ctrl ? 16 : 0);
    bool motion_only = (bstate == 2);
    if (t->mouse_mode == 1002 && !motion_only && bstate != 0 &&
        bstate != 1 && bstate != 3 && bstate != 4) {
        /* button-event mode: motion WITH a button only */
        if (button < 0) {
            return;
        }
    }
    if (t->mouse_mode == 1000 && motion_only) {
        return; /* normal mode: no motion reports */
    }
    int benc;
    switch (bstate) {
    case 0: benc = button; break;              /* press                    */
    case 1: benc = button; break;              /* release: SAME code + 'm' */
    case 2: benc = (button < 0 ? 3 : button) + 32; break; /* motion       */
    case 3: benc = 64; break;                  /* wheel up                 */
    case 4: benc = 65; break;                  /* wheel down               */
    default: return;
    }
    benc |= mods;
    if (t->mouse_sgr) {
        /* the SGR (1006) encoding: CSI < button ; col ; row M/m — the
         * release rides the lowercase terminator, the button code is
         * unchanged (the +3 confusion is the LEGACY scheme). */
        char buf[64];
        snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", benc, x + 1, y + 1,
                 (bstate == 1) ? 'm' : 'M');
        reply_str(v, buf);
    } else {
        /* the legacy (X10) encoding: CSI M Cb Cx Cy with Cb = 32 +
         * button (+ motion bit 32 on release) — coords capped 223. */
        int cb = button + (bstate == 1 ? 32 + 3 : 0) + mods;
        if (bstate == 2) {
            cb = (button < 0 ? 3 : button) + 32 + mods;
        }
        if (bstate == 3) {
            cb = 64 + mods;
        }
        if (bstate == 4) {
            cb = 65 + mods;
        }
        if (x + 33 > 255 || y + 33 > 255 || cb + 32 > 255) {
            return; /* out of legacy range: dropped, as xterm does */
        }
        char buf[8];
        buf[0] = '\033';
        buf[1] = '[';
        buf[2] = 'M';
        buf[3] = (char)(32 + cb);
        buf[4] = (char)(32 + x + 1);
        buf[5] = (char)(32 + y + 1);
        reply_put(v, buf, 6);
    }
}

void vt_paste(vt *v, const char *text, size_t len) {
    term *t = v->t;
    if (t->bracketed_paste) {
        reply_str(v, "\033[200~");
        reply_put(v, text, len);
        reply_str(v, "\033[201~");
    } else {
        reply_put(v, text, len);
    }
}

bool vt_key(vt *v, uint32_t scancode, uint32_t cp, uint32_t mods) {
    term *t = v->t;
    bool ctrl = (mods & 2u) != 0;
    bool alt = (mods & 4u) != 0;
    bool shift = (mods & 1u) != 0;

    /* The ctrl+letter family: the control code itself. */
    if (ctrl && cp >= 'a' && cp <= 'z') {
        char c = (char)(cp - 'a' + 1);
        if (alt) {
            reply_put(v, "\033", 1);
        }
        reply_put(v, &c, 1);
        return true;
    }
    if (ctrl && (cp == '@' || cp == '[' || cp == '\\' || cp == ']' ||
                 cp == '^' || cp == '_' || cp == ' ')) {
        char c;
        switch (cp) {
        case '@': c = 0; break;
        case '[': c = 27; break;
        case '\\': c = 28; break;
        case ']': c = 29; break;
        case '^': c = 30; break;
        case '_': c = 31; break;
        default: c = 0; break;
        }
        if (alt) {
            reply_put(v, "\033", 1);
        }
        reply_put(v, &c, 1);
        return true;
    }

    /* Enter/Tab/Backspace/Escape: the plain codes (Enter is CR —
     * the LF-vs-CR translation is the tty's job). */
    if (scancode == FDK_KEY_ENTER) {
        reply_put(v, alt ? "\033\r" : "\r", alt ? 2 : 1);
        return true;
    }
    if (scancode == FDK_KEY_TAB) {
        reply_put(v, "\t", 1);
        return true;
    }
    if (scancode == FDK_KEY_BACKSPACE) {
        reply_put(v, alt ? "\033\177" : "\177", alt ? 2 : 1);
        return true;
    }
    if (scancode == FDK_KEY_ESC) {
        reply_put(v, "\033", 1);
        return true;
    }

    /* The arrow/home/end family: SS3 in app mode, CSI in normal. */
    const char *code = NULL;
    switch (scancode) {
    case FDK_KEY_UP: code = "A"; break;
    case FDK_KEY_DOWN: code = "B"; break;
    case FDK_KEY_RIGHT: code = "C"; break;
    case FDK_KEY_LEFT: code = "D"; break;
    case FDK_KEY_HOME: code = "H"; break;
    case FDK_KEY_END: code = "F"; break;
    case FDK_KEY_PAGE_UP: code = "5~"; break;
    case FDK_KEY_PAGE_DOWN: code = "6~"; break;
    case FDK_KEY_DELETE: code = "3~"; break;
    case 110: code = "2~"; break; /* Insert */
    default: break;
    }
    if (code != NULL) {
        bool one = code[1] == '\0';
        if (one && !ctrl) {
            /* plain single-letter (arrows, home/end): SS3 vs CSI */
            if (alt) {
                reply_put(v, "\033", 1);
            }
            reply_str(v, t->app_cursor_keys ? "\033O" : "\033[");
            reply_put(v, code, 1);
            return true;
        }
        /* modified or multi-letter: the CSI 1;modX form */
        char buf[24];
        int modnum = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
        if (one) {
            snprintf(buf, sizeof buf, "\033[1;%d%c", modnum, code[0]);
        } else {
            if (modnum > 1) {
                snprintf(buf, sizeof buf, "\033[1;%d%s", modnum, code);
            } else {
                snprintf(buf, sizeof buf, "\033[%s", code);
            }
        }
        reply_str(v, buf);
        return true;
    }

    /* F1..F12 (the xterm sequences). */
    static const struct {
        uint32_t sc;
        const char *seq;
        const char *app;
    } fkeys[] = {
        {59, "\033[11~", "\033OP"},
        {60, "\033[12~", "\033OQ"},
        {61, "\033[13~", "\033OR"},
        {62, "\033[14~", "\033OS"},
        {63, "\033[15~", NULL},
        {64, "\033[17~", NULL},
        {65, "\033[18~", NULL},
        {66, "\033[19~", NULL},
        {67, "\033[20~", NULL},
        {68, "\033[21~", NULL},
        {69, "\033[23~", NULL},
        {70, "\033[24~", NULL},
    };
    for (size_t i = 0; i < sizeof fkeys / sizeof *fkeys; i++) {
        if (fkeys[i].sc == scancode) {
            if (t->app_keypad && fkeys[i].app != NULL) {
                reply_str(v, fkeys[i].app);
            } else {
                reply_str(v, fkeys[i].seq);
            }
            return true;
        }
    }

    return false;
}

/* The scrollback ring push the linefeed path uses (exposed through
 * a non-static name to keep term.c the owner of the ring). */
void term__back_push(term *t, trow *r);
