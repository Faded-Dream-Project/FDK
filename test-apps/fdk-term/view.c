/* view.c — the canvas renderer + input plumbing (see view.h). */
#include "view.h"
#include "wide.h"
#include "term_app.h"      /* the shared test-app shell (ta_app)        */
#include "app.h"
#include "profile.h"

#include "fdk/fdk_extend.h"
#include "fdk/fdk_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define LIVE 0

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- painting ---------------------------------------------------------- */

static void cell_rect(const tview *v, int x, int y, fdk_rect *out) {
    out->x = x * v->cell_w;
    out->y = y * v->cell_h;
    out->width = v->cell_w;
    out->height = v->cell_h;
}

static void paint(tview *v, fdk_surface *s, fdk_rect bounds) {
    term *t = &v->t;
    /* FDK paint hooks receive the widget's ABSOLUTE bounds and the
     * surface is NOT translated (only clipped — widget.c's paint_rec
     * pushes the abs rect as the clip). Every other widget paints
     * with bounds.x/y; the terminal used to paint at local (0,0),
     * which shifted the whole grid up by the menubar+tabbar height —
     * the prompt landed inside the clipped chrome strip and never
     * showed (the 1.4.20 "can't type" report: the keys were reaching
     * the pty all along, the echo was drawn off the viewport). */
    const fdk_i32 ox = bounds.x, oy = bounds.y;

    /* background */
    fdk_color bg = tcolor_default_bg();
    fdk_surface_fill_rect(s, (fdk_rect){ox, oy, t->cols * v->cell_w,
                                        t->rows * v->cell_h},
                          bg);

    /* the visible rows: scroll_off is rows ABOVE the grid top */
    int64_t back_count = t->on_alt ? 0 : (int64_t)t->back_count;
    int64_t total = back_count + t->rows;
    int64_t max_off = t->on_alt ? 0 : (total - t->rows);
    if (v->scroll_off > max_off) {
        v->scroll_off = max_off;
    }
    if (v->scroll_off < 0) {
        v->scroll_off = 0;
    }

    /* selection normalization */
    int sx0 = v->t.sel_x0, sx1 = v->t.sel_x1;
    int64_t sy0 = v->t.sel_y0, sy1 = v->t.sel_y1;
    if (v->t.sel_active && (sy0 > sy1 || (sy0 == sy1 && sx0 > sx1))) {
        int tx = sx0;
        int64_t ty = sy0;
        sx0 = sx1;
        sy0 = sy1;
        sx1 = tx;
        sy1 = ty;
    }

    for (int gy = 0; gy < t->rows; gy++) {
        int64_t abs_y = back_count - v->scroll_off + gy;
        const trow *r = NULL;
        if (abs_y >= 0 && abs_y < back_count && !t->on_alt) {
            /* scrollback row */
            r = NULL; /* fetched below via term helpers */
        }
        /* fetch through the grid/scrollback shim */
        char *line_txt = NULL;
        (void)line_txt;
        const trow *row = NULL;
        if (t->on_alt) {
            row = &t->alt[gy];
        } else if (v->scroll_off == 0) {
            row = &t->grid[gy];
        } else {
            int64_t src = abs_y;
            if (src < 0 || src >= total) {
                continue;
            }
            if (src < back_count) {
                row = term_back_row(t, (size_t)src);
            } else {
                row = &t->grid[src - back_count];
            }
        }
        if (row == NULL) {
            continue;
        }
        r = row;

        for (int x = 0; x < t->cols; x++) {
            const tcell *c = &r->cells[x];
            if (c->attr & TATTR_WIDE_CONT) {
                continue;
            }

            /* fg/bg resolution (deferred colors) */
            fdk_color cfg = tcolor_resolve(c->fg, false);
            fdk_color cbg = tcolor_resolve(c->bg, true);
            if (c->attr & TATTR_INVERSE) {
                fdk_color tmp = cfg;
                cfg = cbg;
                cbg = tmp;
            }

            /* the cell's own background — explicit bgs only, so the
             * common all-default cell skips the fill. This used to
             * be missing entirely: every 48;2/48;5 background (prompt
             * segments, fastfetch blocks, \e[K-painted runs) fell
             * through to the base fill and vanished. The bg fill
             * must also happen for cp==0 cells: a segment paints its
             * background by erasing cells with the pen bg set. */
            if (c->bg.index != -1) {
                fdk_rect bgr;
                cell_rect(v, x, gy, &bgr);
                bgr.x += ox;
                bgr.y += oy;
                fdk_surface_fill_rect(s, bgr, cbg);
            }

            if (c->attr & TATTR_INVISIBLE) {
                continue;
            }
            if (c->cp == 0) {
                continue;
            }

            /* selection highlight */
            int cell_abs_y = abs_y > INT_MAX ? INT_MAX : ((int)((abs_y) & 0x7FFFFFFF));
            bool selected = false;
            if (t->sel_active) {
                if (t->sel_rect) {
                    int rx0 = sx0 < sx1 ? sx0 : sx1;
                    int rx1 = sx0 < sx1 ? sx1 : sx0;
                    int64_t ry0 = sy0 < sy1 ? sy0 : sy1;
                    int64_t ry1 = sy0 < sy1 ? sy1 : sy0;
                    selected = cell_abs_y >= ry0 && cell_abs_y <= ry1 &&
                               x >= rx0 && x <= rx1;
                } else {
                    bool after_start = cell_abs_y > sy0 ||
                                       (cell_abs_y == sy0 && x >= sx0);
                    bool before_end = cell_abs_y < sy1 ||
                                      (cell_abs_y == sy1 && x <= sx1);
                    selected = after_start && before_end;
                }
            }
            if (selected) {
                fdk_color sel_bg = fdk_theme_get_color(NULL, FDK_TK_ACCENT);
                sel_bg.a = 0.45f;
                fdk_rect cr;
                cell_rect(v, x, gy, &cr);
                cr.x += ox;
                cr.y += oy;
                fdk_surface_fill_rect(s, cr, sel_bg);
            }

            /* the glyph cluster as UTF-8 */
            char buf[16];
            int blen = 0;
            uint32_t cps[3] = {c->cp,
                               c->ncluster > 0 ? c->cluster[0] : 0,
                               c->ncluster > 1 ? c->cluster[1] : 0};
            for (int k = 0; k < 3; k++) {
                uint32_t cp = cps[k];
                if (k > 0 && cp == 0) {
                    break;
                }
                if (cp < 0x80) {
                    buf[blen++] = (char)cp;
                } else if (cp < 0x800) {
                    buf[blen++] = (char)(0xC0 | (cp >> 6));
                    buf[blen++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    buf[blen++] = (char)(0xE0 | (cp >> 12));
                    buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[blen++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    buf[blen++] = (char)(0xF0 | (cp >> 18));
                    buf[blen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    buf[blen++] = (char)(0x80 | (cp & 0x3F));
                }
            }

            fdk_rect cr;
            cell_rect(v, x, gy, &cr);
            cr.x += ox;
            cr.y += oy;

            /* wide cells: the ink spans two columns */
            int span = 1;
            if (x + 1 < t->cols &&
                (r->cells[x + 1].attr & TATTR_WIDE_CONT)) {
                span = 2;
            }

            /* underline/strikethrough bars */
            if (c->attr & TATTR_UNDERLINE) {
                fdk_surface_fill_rect(
                    s, (fdk_rect){cr.x, cr.y + v->ascent + 2,
                                  cr.width * span, 1},
                    cfg);
            }
            if (c->attr & TATTR_STRIKE) {
                fdk_surface_fill_rect(
                    s, (fdk_rect){cr.x, cr.y + v->ascent - v->cell_h / 4,
                                  cr.width * span, 1},
                    cfg);
            }

            /* synthetic styles */
            fdk_font *font = v->font;
            unsigned style = 0;
            if (c->attr & TATTR_BOLD) {
                style |= FDK_FONT_STYLE_BOLD;
            }
            if (c->attr & TATTR_ITALIC) {
                style |= FDK_FONT_STYLE_ITALIC;
            }
            fdk_font_set_style(font, style);
            fdk_surface_draw_utf8(s, font, buf, (size_t)blen, cr.x,
                                  cr.y + v->ascent, cfg);
            fdk_font_set_style(font, 0);

            /* hyperlink affordance: dotted underline in the theme's
             * link color */
            if (c->hyperlink) {
                fdk_color link = fdk_theme_get_color(NULL, FDK_TK_LINK);
                for (int d = 0; d < cr.width * span; d += 3) {
                    fdk_surface_fill_rect(
                        s, (fdk_rect){cr.x + d, cr.y + v->cell_h - 2, 2, 1},
                        link);
                }
            }
        }
    }

    /* the cursor (only in the live view + visible) */
    if (t->cursor_visible && v->scroll_off == 0) {
        bool blink_show = !t->cursor_blink || v->blink_on;
        if (blink_show) {
            fdk_rect cr;
            cell_rect(v, t->cx, t->cy, &cr);
            cr.x += ox;
            cr.y += oy;
            /* wide cursor over wide cells */
            if (t->cx + 1 < t->cols) {
                const trow *r = t->on_alt ? &t->alt[t->cy]
                                          : &t->grid[t->cy];
                if (r->cells[t->cx + 1].attr & TATTR_WIDE_CONT) {
                    cr.width *= 2;
                }
            }
            fdk_color caret = tcolor_default_fg();
            caret.a = 0.85f;
            fdk_surface_fill_rect(s, cr, caret);
            /* the glyph under the cursor, re-drawn in bg color */
            const trow *r = t->on_alt ? &t->alt[t->cy] : &t->grid[t->cy];
            const tcell *c = &r->cells[t->cx];
            if (c->cp != 0) {
                char buf[8];
                int blen = 0;
                if (c->cp < 0x80) {
                    buf[blen++] = (char)c->cp;
                } else if (c->cp < 0x800) {
                    buf[blen++] = (char)(0xC0 | (c->cp >> 6));
                    buf[blen++] = (char)(0x80 | (c->cp & 0x3F));
                } else if (c->cp < 0x10000) {
                    buf[blen++] = (char)(0xE0 | (c->cp >> 12));
                    buf[blen++] = (char)(0x80 | ((c->cp >> 6) & 0x3F));
                    buf[blen++] = (char)(0x80 | (c->cp & 0x3F));
                } else {
                    buf[blen++] = (char)(0xF0 | (c->cp >> 18));
                    buf[blen++] = (char)(0x80 | ((c->cp >> 12) & 0x3F));
                    buf[blen++] = (char)(0x80 | ((c->cp >> 6) & 0x3F));
                    buf[blen++] = (char)(0x80 | (c->cp & 0x3F));
                }
                fdk_font_set_style(v->font, 0);
                fdk_surface_draw_utf8(s, v->font, buf, (size_t)blen,
                                      cr.x, cr.y + v->ascent,
                                      tcolor_default_bg());
            }
        }
    }

    /* the visual-bell flash (a border band) */
    if (v->flash_until_ms > now_ms()) {
        fdk_color warn = fdk_theme_get_color(NULL, FDK_TK_WARNING);
        warn.a = 0.55f;
        fdk_i32 w = t->cols * v->cell_w, h = t->rows * v->cell_h;
        fdk_surface_fill_rect(s, (fdk_rect){ox, oy, w, 3}, warn);
        fdk_surface_fill_rect(s, (fdk_rect){ox, oy + h - 3, w, 3}, warn);
        fdk_surface_fill_rect(s, (fdk_rect){ox, oy, 3, h}, warn);
        fdk_surface_fill_rect(s, (fdk_rect){ox + w - 3, oy, 3, h}, warn);
    }

    /* the scroll indicator: a thin bar on the right edge while
     * scrolled into history */
    if (v->scroll_off > 0) {
        fdk_i32 w = t->cols * v->cell_w, h = t->rows * v->cell_h;
        double frac = max_off > 0 ? (double)v->scroll_off / (double)max_off
                                  : 0.0;
        fdk_i32 bar_h = 40;
        fdk_i32 bar_y = (fdk_i32)((h - bar_h) * frac);
        fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);
        ink.a = 0.4f;
        fdk_surface_fill_rect(s, (fdk_rect){ox + w - 4, oy + bar_y, 3, bar_h},
                              ink);
    }
}

/* ---- the PTY bridge ------------------------------------------------------ */

/* the context-menu handlers (defined at the file's tail) */
static void ctx_copy(fdk_menu_item *item, void *user);
static void ctx_paste(fdk_menu_item *item, void *user);
static void ctx_select_all(fdk_menu_item *item, void *user);
static void ctx_split_h(fdk_menu_item *item, void *user);
static void ctx_split_v(fdk_menu_item *item, void *user);
void tview_blink_tick(fdk_timer *timer, void *user);
void tview_regrid(tview *v);
void tview_dump_grid(const tview *v, FILE *out);

static void drain_pty(tview *v) {
    char buf[16384];
    int n;
    while ((n = pty_read(&v->pt, buf, sizeof buf)) > 0) {
        vt_out out;
        vt_feed(v->parser, buf, (size_t)n, &out);
        if (out.title != NULL) {
            tview_set_tab_title(v, out.title);
        }
        if (out.cwd != NULL) {
            free(v->pt.cwd);
            v->pt.cwd = strdup(out.cwd);
        }
        if (out.clipboard_set && out.clipboard != NULL) {
            (void)fdk_clipboard_set_text(((tapp *)v->app)->ctx,
                                         out.clipboard);
        }
        if (out.bell) {
            tview_bell(v);
        }
        if (out.dirty) {
            tview_invalidate(v);
        }
        if (vt_reply_peek(v->parser, NULL) != NULL) {
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            (void)pty_write(&v->pt, rp, rl);
            vt_reply_consume(v->parser, rl);
        }
    }
    if (n < 0) {
        /* child gone: mark the pane dead */
        tview_child_died(v);
    }
    /* keep following the live bottom */
    if (v->scroll_off == LIVE) {
        tview_invalidate(v);
    }
}

static void pty_watch_cb(fdk_ext_fd_source *src, uint32_t revents,
                         void *user) {
    (void)src;
    tview *v = user;
    if (revents & (FDK_EXT_FD_IN | FDK_EXT_FD_HUP | FDK_EXT_FD_ERR)) {
        drain_pty(v);
    }
}

/* ---- the widget event surface --------------------------------------------- */

static void view_paint_cb(fdk_widget *w, fdk_surface *s, fdk_rect bounds,
                          fdk_rect clip, void *user) {
    (void)w;
    (void)clip;
    paint(user, s, bounds);
}

static void feed_key(tview *v, const fdk_key_event *key) {
    /* the parser's key mapping first */
    if (vt_key(v->parser, key->scancode, key->codepoint,
               key->modifiers)) {
        size_t rl = 0;
        const char *rp = vt_reply_peek(v->parser, &rl);
        if (rp != NULL && rl > 0) {
            (void)pty_write(&v->pt, rp, rl);
            fprintf(stderr, "[term] key sc=%d cp=%d mod=%d -> pty "
                    "(%zu bytes)\n", key->scancode, key->codepoint,
                    key->modifiers, rl);
            vt_reply_consume(v->parser, rl);
        }
        return;
    }
    if (key->codepoint != 0) {
        char buf[8];
        int blen = 0;
        uint32_t cp = key->codepoint;
        if (cp < 0x80) {
            buf[blen++] = (char)cp;
        } else if (cp < 0x800) {
            buf[blen++] = (char)(0xC0 | (cp >> 6));
            buf[blen++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[blen++] = (char)(0xE0 | (cp >> 12));
            buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[blen++] = (char)(0x80 | (cp & 0x3F));
        } else {
            buf[blen++] = (char)(0xF0 | (cp >> 18));
            buf[blen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            buf[blen++] = (char)(0x80 | (cp & 0x3F));
        }
        if (key->modifiers & 4) { /* Alt prefixes ESC */
            (void)pty_write(&v->pt, "\033", 1);
        }
        (void)pty_write(&v->pt, buf, (size_t)blen);
        fprintf(stderr, "[term] key cp=%d mod=%d -> pty (%d bytes)\n",
                key->codepoint, key->modifiers, blen);
        v->t.last_input_ms = now_ms();
    }
}

static bool view_event_cb(fdk_widget *w, const fdk_widget_event *ev,
                          void *user) {
    (void)w;
    tview *v = user;
    switch (ev->type) {
    case FDK_WIDGET_KEY_DOWN:
        feed_key(v, &ev->key);
        /* any keypress returns to live */
        if (v->scroll_off != LIVE) {
            v->scroll_off = LIVE;
            tview_invalidate(v);
        }
        return true;
    case FDK_WIDGET_IME_COMMIT: {
        const char *text = ev->ime.text;
        (void)pty_write(&v->pt, text, strlen(text));
        v->t.last_input_ms = now_ms();
        return true;
    }
    case FDK_WIDGET_POINTER_DOWN: {
        int x = (int)(ev->pointer.position.x / (fdk_f32)v->cell_w);
        int gy = (int)(ev->pointer.position.y / (fdk_f32)v->cell_h);
        if (x < 0 || x >= v->t.cols || gy < 0 || gy >= v->t.rows) {
            return true;
        }
        fdk_widget_focus(v->canvas);

        int64_t abs_y = tview_abs_y(v, gy);

        if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
            /* Ctrl+click: URL open */
            if (ev->pointer.modifiers & 2) {
                int ux0, ux1;
                if (term_url_at(&v->t, abs_y, x, &ux0, &ux1)) {
                    char *line = term_line_text(&v->t, abs_y);
                    if (line != NULL) {
                        char url[1024];
                        int ulen = 0;
                        for (int cx = ux0; cx < ux1 && ulen < 1023; cx++) {
                            /* extract the url cell range */
                            (void)cx;
                        }
                        /* simpler: re-scan the plain text by columns */
                        ulen = 0;
                        int col = 0;
                        for (const char *p = line; *p != '\0'; p++) {
                            if (col >= ux0 && col < ux1 && ulen < 1023) {
                                url[ulen++] = *p;
                            }
                            col++;
                        }
                        url[ulen] = '\0';
                        free(line);
                        (void)fdk_desktop_open_uri(url);
                        return true;
                    }
                }
            }
            /* plain press: start a selection */
            v->t.sel_active = true;
            v->t.sel_mode = TSEL_CHAR;
            v->t.sel_rect = (ev->pointer.modifiers & 4) != 0; /* Alt */
            v->t.sel_x0 = v->t.sel_x1 = x;
            v->t.sel_y0 = v->t.sel_y1 = abs_y;
            v->selecting = true;
            v->sel_anchor_x = x;
            v->sel_anchor_y = abs_y; /* int64 field */
        } else if (ev->pointer.button == FDK_POINTER_BUTTON_MIDDLE) {
            /* middle-click: PRIMARY paste */
            char *txt = fdk_clipboard_get_primary_text(
                ((tapp *)v->app)->ctx);
            if (txt != NULL) {
                vt_paste(v->parser, txt, strlen(txt));
                free(txt);
                size_t rl = 0;
                const char *rp = vt_reply_peek(v->parser, &rl);
                if (rp != NULL && rl > 0) {
                    (void)pty_write(&v->pt, rp, rl);
                    vt_reply_consume(v->parser, rl);
                }
            }
        } else if (ev->pointer.button == FDK_POINTER_BUTTON_RIGHT) {
            tview_open_context_menu(v, x, gy);
        }

        /* mouse press report */
        if (v->t.mouse_mode != 0) {
            int b = 0;
            if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
                b = 0;
            } else if (ev->pointer.button == FDK_POINTER_BUTTON_MIDDLE) {
                b = 1;
            } else if (ev->pointer.button == FDK_POINTER_BUTTON_RIGHT) {
                b = 2;
            }
            vt_mouse(v->parser, 0, b, x, gy,
                     (ev->pointer.modifiers & 1) != 0,
                     (ev->pointer.modifiers & 4) != 0,
                     (ev->pointer.modifiers & 2) != 0);
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_UP: {
        int x = (int)(ev->pointer.position.x / (fdk_f32)v->cell_w);
        int gy = (int)(ev->pointer.position.y / (fdk_f32)v->cell_h);
        if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT && v->selecting) {
            v->selecting = false;
            int64_t abs_y = tview_abs_y(v, gy);
            v->t.sel_x1 = x;
            v->t.sel_y1 = abs_y;
            /* copy-on-select (the profile's rule) */
            if (profile_copy_on_select(v->app)) {
                char *txt = term_selection_text(&v->t);
                if (txt != NULL) {
                    (void)fdk_clipboard_set_primary_text(
                        ((tapp *)v->app)->ctx, txt);
                    free(txt);
                }
            }
            tview_invalidate(v);
        }
        /* mouse release report */
        if (v->t.mouse_mode != 0) {
            int b = 0;
            if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
                b = 0;
            } else if (ev->pointer.button == FDK_POINTER_BUTTON_MIDDLE) {
                b = 1;
            } else if (ev->pointer.button == FDK_POINTER_BUTTON_RIGHT) {
                b = 2;
            }
            vt_mouse(v->parser, 1, b, x, gy, false, false, false);
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
        }
        return true;
    }
    case FDK_WIDGET_POINTER_MOTION: {
        int x = (int)(ev->pointer.position.x / (fdk_f32)v->cell_w);
        int gy = (int)(ev->pointer.position.y / (fdk_f32)v->cell_h);
        if (x == v->last_mx && gy == v->last_my) {
            return true;
        }
        v->last_mx = x;
        v->last_my = gy;

        /* selection drag */
        if (v->selecting) {
            int64_t abs_y = tview_abs_y(v, gy);
            v->t.sel_x1 = x;
            v->t.sel_y1 = abs_y;
            tview_invalidate(v);
        }

        /* motion report (1002/1003) */
        if (v->t.mouse_mode == 1002 || v->t.mouse_mode == 1003) {
            bool held = (ev->pointer.position.x >= 0); /* approximated
                            below via the last button state */
            (void)held;
            vt_mouse(v->parser, 2, v->last_mx >= 0 ? 0 : -1, x, gy,
                     false, false, false);
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
        }

        /* URL hover cursor */
        if (ev->pointer.modifiers & 2) {
            fdk_window_set_cursor(tview_owner_window(v), FDK_CURSOR_POINTER);
        }
        return true;
    }
    case FDK_WIDGET_SCROLL: {
        int x = (int)(ev->scroll.position.x / (fdk_f32)v->cell_w);
        int gy = (int)(ev->scroll.position.y / (fdk_f32)v->cell_h);
        /* alt-screen wheel: cursor keys (the vim/less contract) */
        if (v->t.on_alt && v->t.mouse_mode == 0) {
            for (int i = 0; i < 3; i++) {
                if (ev->scroll.delta_y > 0) {
                    vt_key(v->parser, FDK_KEY_DOWN, 0, 0);
                } else if (ev->scroll.delta_y < 0) {
                    vt_key(v->parser, FDK_KEY_UP, 0, 0);
                }
            }
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
            return true;
        }
        if (v->t.mouse_mode != 0) {
            vt_mouse(v->parser, ev->scroll.delta_y > 0 ? 4 : 3, 0, x, gy,
                     false, false, false);
            size_t rl = 0;
            const char *rp = vt_reply_peek(v->parser, &rl);
            if (rp != NULL && rl > 0) {
                (void)pty_write(&v->pt, rp, rl);
                vt_reply_consume(v->parser, rl);
            }
            return true;
        }
        tview_scroll_by(v, (int64_t)(ev->scroll.delta_y > 0 ? 3 : -3));
        return true;
    }
    default:
        break;
    }
    return false;
}

/* ---- lifecycle ------------------------------------------------------------ */

static int g_view_ids = 1;

bool tview_create(tview *v, tapp *app, twindow *owner, fdk_widget *parent,
                  const char *cwd) {
    memset(v, 0, sizeof *v);
    v->app = app;
    v->owner = owner;
    v->view_id = g_view_ids++;
    v->font = app->font;
    v->scroll_off = LIVE;

    /* metrics from the font */
    fdk_font_metrics m;
    fdk_font_get_metrics(v->font, &m);
    fdk_text_metrics tm;
    fdk_font_measure_utf8(v->font, "M", 1, &tm);
    v->cell_w = tm.advance_width > 0 ? tm.advance_width : 8;
    v->cell_h = m.line_height > 0 ? m.line_height : v->font ? 17 : 17;
    v->ascent = m.ascent;

    /* the grid size from the owner's window geometry */
    int cols = 80, rows = 24;
    if (owner != NULL && owner->cols > 0) {
        cols = owner->cols;
        rows = owner->rows;
    }

    term_init(&v->t, cols, rows, (size_t)app->active.scrollback);
    v->parser = vt_create(&v->t);
    if (v->parser == NULL) {
        term_free(&v->t);
        return false;
    }

    if (!pty_spawn(&v->pt, app->active.shell[0] ? app->active.shell : NULL,
                   cwd != NULL ? cwd
                               : (app->active.workdir[0]
                                      ? app->active.workdir
                                      : NULL),
                   app->active.login_shell, cols, rows)) {
        vt_destroy(v->parser);
        term_free(&v->t);
        return false;
    }

    if (!fdk_ok(fdk_canvas_create(parent, view_paint_cb, v, &v->canvas))) {
        pty_shutdown(&v->pt);
        vt_destroy(v->parser);
        term_free(&v->t);
        return false;
    }
    fdk_widget_set_natural_size(v->canvas, cols * v->cell_w,
                                rows * v->cell_h);
    fdk_widget_set_can_focus(v->canvas, true);
    fdk_widget_set_event_callback(v->canvas, view_event_cb, v);
    fdk_widget_set_accessible_name(v->canvas, "terminal");

    /* the PTY watch: readable -> drain */
    v->watch = fdk_ext_loop_add_fd(app->ctx, v->pt.master,
                                   FDK_EXT_FD_IN, pty_watch_cb, v);

    /* the blink clock */
    if (app->active.cursor_blink_ms > 0) {
        v->blink_timer = fdk_timer_add(
            app->ctx, (fdk_u32)app->active.cursor_blink_ms, true,
            tview_blink_tick, v);
    }
    return true;
}

void tview_destroy(tview *v) {
    if (v->watch != NULL) {
        fdk_ext_loop_remove_fd(v->watch);
        v->watch = NULL;
    }
    if (v->blink_timer != NULL) {
        fdk_timer_remove(v->blink_timer);
        v->blink_timer = NULL;
    }
    /* the canvas belongs to the widget tree (the parent destroys it) */
    pty_shutdown(&v->pt);
    if (v->parser != NULL) {
        vt_destroy(v->parser);
        v->parser = NULL;
    }
    term_free(&v->t);
}

void tview_refont(tview *v, fdk_font *font) {
    v->font = font;
    fdk_font_metrics m;
    fdk_font_get_metrics(font, &m);
    fdk_text_metrics tm;
    fdk_font_measure_utf8(font, "M", 1, &tm);
    v->cell_w = tm.advance_width > 0 ? tm.advance_width : 8;
    v->cell_h = m.line_height;
    v->ascent = m.ascent;
    /* re-grid to the same pixel size */
    tview_regrid(v);
    tview_invalidate(v);
}

void tview_regrid(tview *v) {
    if (v->owner == NULL || v->canvas == NULL) {
        return;
    }
    fdk_rect b = fdk_widget_get_bounds(v->canvas);
    int cols = b.width / (v->cell_w > 0 ? v->cell_w : 8);
    int rows = b.height / (v->cell_h > 0 ? v->cell_h : 17);
    if (cols < 2) {
        cols = 2;
    }
    if (rows < 2) {
        rows = 2;
    }
    if (cols != v->t.cols || rows != v->t.rows) {
        term_resize(&v->t, cols, rows);
        pty_resize(&v->pt, cols, rows);
    }
}

void tview_invalidate(tview *v) {
    if (v->canvas != NULL) {
        fdk_canvas_invalidate(v->canvas);
    }
}

void tview_scroll_by(tview *v, int64_t lines) {
    term *t = &v->t;
    int64_t total = t->on_alt ? t->rows : (int64_t)t->back_count + t->rows;
    int64_t max_off = t->on_alt ? 0 : total - t->rows;
    int64_t off = v->scroll_off + lines;
    if (off < 0) {
        off = 0;
    }
    if (off > max_off) {
        off = max_off;
    }
    v->scroll_off = off;
    tview_invalidate(v);
}

void tview_scroll_to(tview *v, int64_t off) {
    v->scroll_off = off;
    tview_scroll_by(v, 0); /* clamps */
}

bool tview_at_live(tview *v) {
    return v->scroll_off == 0;
}

void tview_focus(tview *v) {
    if (v->canvas != NULL) {
        fdk_widget_focus(v->canvas);
    }
}

void tview_metrics(const tview *v, int *w, int *h) {
    *w = v->cell_w;
    *h = v->cell_h;
}

int64_t tview_abs_y(const tview *v, int grid_y) {
    const term *t = &v->t;
    if (t->on_alt) {
        return grid_y;
    }
    return (int64_t)t->back_count - v->scroll_off + grid_y;
}

/* ---- the app-facing helpers ---------------------------------------------- */

void tview_blink_tick(fdk_timer *timer, void *user) {
    (void)timer;
    tview *v = user;
    if (v->t.cursor_visible && v->scroll_off == 0) {
        v->blink_on = !v->blink_on;
        fdk_rect cr;
        cell_rect(v, v->t.cx, v->t.cy, &cr);
        /* canvas-local damage: the canvas's own invalidate covers it */
        tview_invalidate(v);
        (void)cr;
    }
}

fdk_window *tview_owner_window(tview *v) {
    return v->owner != NULL ? v->owner->window : NULL;
}

void tview_set_tab_title(tview *v, const char *title) {
    if (v->owner == NULL || title == NULL || title[0] == '\0') {
        return;
    }
    ttab *tab = NULL;
    for (int i = 0; i < v->owner->ntabs; i++) {
        for (int k = 0; k < v->owner->tabs[i]->nviews; k++) {
            if (v->owner->tabs[i]->views[k] == v) {
                tab = v->owner->tabs[i];
                break;
            }
        }
    }
    if (tab == NULL) {
        return;
    }
    snprintf(tab->title, sizeof tab->title, "%s", title);
    /* window title = current tab's title */
    size_t idx = fdk_notebook_page_index(v->owner->notebook, tab->page);
    (void)fdk_notebook_set_page_label(v->owner->notebook, idx,
                                      tab->title);
    if (idx == fdk_notebook_get_current_page(v->owner->notebook)) {
        fdk_window_set_title(v->owner->window, title);
    }
}

void tview_bell(tview *v) {
    tapp *app = v->app;
    if (app->active.bell_audible) {
        (void)fdk_window_bell(tview_owner_window(v), 0);
    }
    if (app->active.bell_visual) {
        v->flash_until_ms = now_ms() + 160;
        tview_invalidate(v);
    }
    /* a bell in a non-focused tab marks it unread (focus tracking
     * rides the window FOCUS events the app layer records) */
    if (v->owner != NULL && !v->owner->focused) {
        v->has_unread = true;
    }
}

void tview_child_died(tview *v) {
    /* keep the view (the user reads the output); mark it dead in
     * the status bar and stop the watch */
    if (v->watch != NULL) {
        fdk_ext_loop_remove_fd(v->watch);
        v->watch = NULL;
    }
    if (v->owner != NULL) {
        tapp_update_status(v->owner);
    }
}

void tview_open_context_menu(tview *v, int x, int y) {
    tapp *app = v->app;
    fdk_menu *m = NULL;
    (void)fdk_menu_create(app->font_ui, &m);
    fdk_menu_item *it;

    (void)fdk_menu_append(m, "Copy", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+C");
    fdk_menu_item_set_on_activate(it, ctx_copy, v);

    (void)fdk_menu_append(m, "Paste", &it);
    fdk_menu_item_set_shortcut(it, "Ctrl+Shift+V");
    fdk_menu_item_set_on_activate(it, ctx_paste, v);

    (void)fdk_menu_append_separator(m);

    (void)fdk_menu_append(m, "Select All", &it);
    fdk_menu_item_set_on_activate(it, ctx_select_all, v);

    (void)fdk_menu_append_separator(m);

    (void)fdk_menu_append(m, "Split Horizontal", &it);
    fdk_menu_item_set_on_activate(it, ctx_split_h, v);

    (void)fdk_menu_append(m, "Split Vertical", &it);
    fdk_menu_item_set_on_activate(it, ctx_split_v, v);

    (void)fdk_menu_popup_at(m, v->canvas, x * v->cell_w, y * v->cell_h);
}

static void ctx_copy(fdk_menu_item *item, void *user) {
    (void)item;
    tview *v = user;
    char *txt = term_selection_text(&v->t);
    if (txt != NULL) {
        (void)fdk_clipboard_set_text(((tapp *)v->app)->ctx, txt);
        free(txt);
    }
}

static void ctx_paste(fdk_menu_item *item, void *user) {
    (void)item;
    tview *v = user;
    char *txt = fdk_clipboard_get_text(((tapp *)v->app)->ctx);
    if (txt != NULL) {
        vt_paste(v->parser, txt, strlen(txt));
        free(txt);
        size_t rl = 0;
        const char *rp = vt_reply_peek(v->parser, &rl);
        if (rp != NULL && rl > 0) {
            (void)pty_write(&v->pt, rp, rl);
            vt_reply_consume(v->parser, rl);
        }
    }
}

static void ctx_select_all(fdk_menu_item *item, void *user) {
    (void)item;
    tview *v = user;
    v->t.sel_active = true;
    v->t.sel_x0 = 0;
    v->t.sel_y0 = 0;
    v->t.sel_x1 = v->t.cols - 1;
    v->t.sel_y1 = (int)tview_abs_y(v, v->t.rows - 1) /* rows fit int */;
    tview_invalidate(v);
}

static void ctx_split_h(fdk_menu_item *item, void *user) {
    (void)item;
    tview *v = user;
    /* find the owning tab and split it along a horizontal line */
    if (v->owner != NULL) {
        for (int i = 0; i < v->owner->ntabs; i++) {
            ttab *tab = v->owner->tabs[i];
            for (int k = 0; k < tab->nviews; k++) {
                if (tab->views[k] == v) {
                    tapp_split(v->owner, tab, true);
                    return;
                }
            }
        }
    }
}

static void ctx_split_v(fdk_menu_item *item, void *user) {
    (void)item;
    tview *v = user;
    if (v->owner != NULL) {
        for (int i = 0; i < v->owner->ntabs; i++) {
            ttab *tab = v->owner->tabs[i];
            for (int k = 0; k < tab->nviews; k++) {
                if (tab->views[k] == v) {
                    tapp_split(v->owner, tab, false);
                    return;
                }
            }
        }
    }
}

/* The rig hook: the visible grid as text (one line per row). */
void tview_dump_grid(const tview *v, FILE *out) {
    const term *t = &v->t;
    for (int y = 0; y < t->rows; y++) {
        const trow *r = t->on_alt ? &t->alt[y] : &t->grid[y];
        for (int x = 0; x < t->cols; x++) {
            const tcell *c = &r->cells[x];
            if (c->attr & TATTR_WIDE_CONT || c->cp == 0) {
                continue;
            }
            char buf[8];
            int n = 0;
            uint32_t cp = c->cp;
            if (cp < 0x80) buf[n++] = (char)cp;
            else if (cp < 0x800) {
                buf[n++] = (char)(0xC0 | (cp >> 6));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                buf[n++] = (char)(0xE0 | (cp >> 12));
                buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                buf[n++] = (char)(0xF0 | (cp >> 18));
                buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            }
            fwrite(buf, 1, (size_t)n, out);
        }
        fputc('\n', out);
    }
}
