/* term_test.c — the fdk-term unit rig: the VT parser + grid model
 * under test, headless (no display needed). Covers the brief's
 * contract points: escape coverage, wide chars, combining marks,
 * scrollback flow, selection extraction, URL detection, search,
 * resize, and the reply queue.
 *
 * Build (the test-apps Makefile wires it as build/fdk-term-test):
 *   gcc ... term.c vt.c wide.c colors.c term_test.c -lfdk ...
 * Run: build/fdk-term-test */
#include "term.h"
#include "vt.h"
#include "wide.h"

#include "fdk/fdk_event.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static term t;
static vt *v;

static void feed(const char *s) {
    vt_feed(v, s, strlen(s), NULL);
}

static void setup(void) {
    term_init(&t, 10, 4, 100);
    v = vt_create(&t);
    assert(v != NULL);
}

static void teardown(void) {
    vt_destroy(v);
    term_free(&t);
}

static void drain_replies(void) {
    size_t n = 0;
    vt_reply_consume(v, vt_reply_peek(v, &n) != NULL ? n : 0);
}

/* ---- tests ---------------------------------------------------------------- */

static void test_plain_text_and_wrap(void) {
    setup();
    feed("hello");
    assert(t.grid[0].cells[0].cp == 'h');
    assert(t.grid[0].cells[4].cp == 'o');
    assert(t.cx == 5);
    assert(t.cy == 0);
    /* fill to the wrap */
    feed("wwwww");
    assert(t.cx == 10); /* parked at the margin */
    feed("X");
    assert(t.cx == 1); /* wrapped + placed */
    assert(t.cy == 1);
    assert(t.grid[0].wrapped == true);
    teardown();
    printf("[ok] plain text + autowrap\n");
}

static void test_controls(void) {
    setup();
    feed("ab\rX\nY");
    assert(t.grid[0].cells[0].cp == 'X');
    assert(t.cy == 1);
    /* LF moves down WITHOUT a carriage return (xterm: the tty's
     * ONLCR does the CR; raw \n keeps the column) — Y lands at
     * row 1 col 1, right of the pre-LF column. */
    assert(t.grid[1].cells[1].cp == 'Y');
    feed("\033[2;3H");
    assert(t.cy == 1 && t.cx == 2);
    feed("\b");
    assert(t.cx == 1);
    feed("\t");
    assert(t.cx == 8);
    teardown();
    printf("[ok] CR/LF/CUP/backspace/tab\n");
}

static void test_sgr_colors(void) {
    setup();
    feed("\033[31mR\033[1;44mB");
    assert(t.grid[0].cells[0].fg.index == 1);
    assert((t.grid[0].cells[0].attr & TATTR_BOLD) == 0);
    assert(t.grid[0].cells[1].attr & TATTR_BOLD);
    assert(t.grid[0].cells[1].bg.index == 4);
    /* 256-color */
    feed("\033[38;5;196mX");
    assert(t.grid[0].cells[2].fg.index == 196);
    /* truecolor */
    feed("\033[48;2;10;20;30mY");
    assert(t.grid[0].cells[3].bg.index == -1);
    assert(t.grid[0].cells[3].bg.r == 10);
    assert(t.grid[0].cells[3].bg.g == 20);
    assert(t.grid[0].cells[3].bg.b == 30);
    /* reset */
    feed("\033[0mZ");
    assert(t.grid[0].cells[4].fg.index == -1);
    assert(t.grid[0].cells[4].attr == 0);
    teardown();
    printf("[ok] SGR: named/256/truecolor/bold/reset\n");
}

static void test_erase_and_scroll_region(void) {
    setup();
    feed("aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc\r\ndddddddddd");
    /* EL to end of line on row 1 */
    feed("\033[2;4H\033[K");
    assert(t.grid[1].cells[2].cp == 'b');
    assert(t.grid[1].cells[3].cp == 0);
    /* DECSTBM rows 2..3, scroll within */
    feed("\033[2;3r");
    assert(t.scroll_top == 1 && t.scroll_bot == 3);
    feed("\033[2;1H\n");
    /* LF inside the region at its TOP: the cursor moves down within
     * the region (rows 2..3, 1-based -> indices 1..2); no scroll
     * yet (only the bottom margin scrolls). */
    assert(t.cy == 2);
    assert(t.grid[1].cells[0].cp == 'b');
    /* now LF AT the bottom margin: the region scrolls up */
    feed("\n");
    assert(t.grid[1].cells[0].cp == 'c');
    assert(t.grid[2].cells[0].cp == 0);
    /* row 0 (outside the region) never moved */
    assert(t.grid[0].cells[0].cp == 'a');
    teardown();
    printf("[ok] EL + DECSTBM scroll region\n");
}

static void test_scrollback_flow(void) {
    setup();
    /* 4 rows; write enough lines to push 2 into scrollback */
    for (int i = 0; i < 6; i++) {
        char line[16];
        snprintf(line, sizeof line, "L%d", i);
        feed(line);
        feed("\r\n");
    }
    /* 6 lines + trailing CRLF: L0..L2 flowed to scrollback (the
     * last LF scrolled once more), the grid holds L3..L5 + prompt */
    assert(t.back_count == 3);
    const trow *r = term_back_row(&t, 0);
    assert(r != NULL && r->cells[0].cp == 'L');
    assert(r->cells[1].cp == '0');
    r = term_back_row(&t, 1);
    assert(r != NULL && r->cells[1].cp == '1');
    r = term_back_row(&t, 2);
    assert(r != NULL && r->cells[1].cp == '2');
    /* the grid holds the last lines */
    assert(t.grid[0].cells[1].cp == '3');
    teardown();
    printf("[ok] scrollback ring flow\n");
}

static void test_alt_screen(void) {
    setup();
    feed("primary");
    feed("\033[?1049h");
    assert(t.on_alt);
    assert(t.grid[0].cells[0].cp == 'p'); /* primary preserved */
    feed("alt");
    assert(t.alt[0].cells[0].cp == 'a');
    feed("\033[?1049l");
    assert(!t.on_alt);
    assert(t.grid[0].cells[0].cp == 'p');
    teardown();
    printf("[ok] alternate screen save/restore\n");
}

static void test_wide_and_combining(void) {
    setup();
    feed("a\xe4\xb8\xad" "b"); /* a, U+4E2D (wide), b */
    assert(t.grid[0].cells[0].cp == 'a');
    assert(t.grid[0].cells[1].cp == 0x4E2D);
    assert(t.grid[0].cells[2].attr & TATTR_WIDE_CONT);
    assert(t.grid[0].cells[3].cp == 'b');
    assert(t.cx == 4);
    /* combining: e + U+0301 */
    feed("e\xcc\x81z");
    assert(t.grid[0].cells[4].cp == 'e');
    assert(t.grid[0].cells[4].cluster[0] == 0x301);
    assert(t.grid[0].cells[4].ncluster == 1);
    assert(t.grid[0].cells[5].cp == 'z');
    teardown();
    printf("[ok] wide chars (2 cells) + combining marks\n");
}

static void test_dec_graphics(void) {
    setup();
    feed("\033(0");
    feed("lqk");
    assert(t.grid[0].cells[0].cp == 0x250C); /* 'l' -> box top-left */
    assert(t.grid[0].cells[1].cp == 0x2500); /* 'q' -> horizontal  */
    assert(t.grid[0].cells[2].cp == 0x2510); /* 'k' -> top-right   */
    /* leaving DEC graphics: redesignate G0 to ASCII (ESC ( B).
     * SI alone would stay IN graphics here — G0 itself is the
     * graphics set after ESC ( 0; xterm's rule, not ours. */
    feed("\033(B");
    feed("q");
    assert(t.grid[0].cells[3].cp == 'q');
    teardown();
    printf("[ok] DEC special graphics + SO/SI\n");
}

static void test_osc(void) {
    setup();
    vt_out out;
    vt_feed(v, "\033]0;my title\007", strlen("\033]0;my title\007"), &out);
    assert(out.title != NULL && strcmp(out.title, "my title") == 0);
    vt_feed(v, "\033]7;file:///home/x\033\\",
            strlen("\033]7;file:///home/x\033\\"), &out);
    assert(out.cwd != NULL && strcmp(out.cwd, "file:///home/x") == 0);
    /* OSC 52 set: "hi" = aGk= */
    vt_feed(v, "\033]52;c;aGk=\007", strlen("\033]52;c;aGk=\007"), &out);
    assert(out.clipboard_set);
    assert(out.clipboard != NULL && strcmp(out.clipboard, "hi") == 0);
    teardown();
    printf("[ok] OSC 0 title / OSC 7 cwd / OSC 52 clipboard\n");
}

static void test_modes_and_replies(void) {
    setup();
    feed("\033[?1h");  /* DECCKM */
    assert(t.app_cursor_keys);
    feed("\033[?2004h");
    assert(t.bracketed_paste);
    feed("\033[?1000;1006h");
    assert(t.mouse_mode == 1000 && t.mouse_sgr);
    /* DA1 */
    feed("\033[c");
    size_t rl = 0;
    const char *rp = vt_reply_peek(v, &rl);
    assert(rp != NULL && rl == 5 && memcmp(rp, "\033[?6c", 5) == 0);
    drain_replies();
    /* CPR: place at 3;5 then ask */
    feed("\033[3;5H\033[6n");
    rp = vt_reply_peek(v, &rl);
    assert(rp != NULL && rl >= 6);
    assert(rp[0] == '\033' && rp[1] == '[');
    /* cursor at row 3 col 5 -> [3;5R */
    assert(strstr(rp, "3;5R") != NULL);
    drain_replies();
    teardown();
    printf("[ok] DECCKM/bracketed/mouse modes + DA1/CPR replies\n");
}

static void test_bracketed_paste(void) {
    setup();
    feed("\033[?2004h");
    vt_paste(v, "hi", 2);
    size_t rl = 0;
    const char *rp = vt_reply_peek(v, &rl);
    assert(rp != NULL);
    /* ESC[200~ (6) + "hi" (2) + ESC[201~ (6) = 14 bytes */
    assert(rl == 14);
    assert(memcmp(rp, "\033[200~hi\033[201~", 14) == 0);
    drain_replies();
    /* with paste off: raw */
    feed("\033[?2004l");
    vt_paste(v, "hi", 2);
    rp = vt_reply_peek(v, &rl);
    assert(rl == 2 && memcmp(rp, "hi", 2) == 0);
    teardown();
    printf("[ok] bracketed paste wrap\n");
}

static void test_selection_text(void) {
    setup(); /* 10 cols: keep the fixture lines under one row */
    feed("hello wor\r\nsecond li");
    /* select "lo wor" on row 0: cols 3..8 */
    t.sel_active = true;
    t.sel_x0 = 3;
    t.sel_y0 = 0;
    t.sel_x1 = 8;
    t.sel_y1 = 0;
    char *txt = term_selection_text(&t);
    assert(txt != NULL && strcmp(txt, "lo wor") == 0);
    free(txt);
    /* two-row selection: row 0 cols 6..9 + row 1 cols 0..3 */
    t.sel_x0 = 6;
    t.sel_x1 = 3; /* end col on row 1 */
    t.sel_y1 = 1;
    txt = term_selection_text(&t);
    /* through the END corner inclusive: "wor" + CRLF + "seco" */
    assert(txt != NULL && strcmp(txt, "wor\r\nseco") == 0);
    free(txt);
    teardown();
    printf("[ok] selection extraction (char mode)\n");
}

static void test_url_detection(void) {
    setup();
    term_resize(&t, 40, 4); /* the URL must live on one row */
    feed("go https://example.org/x now");
    int x0, x1;
    /* pointer on the 'e' of example (col 14) */
    assert(term_url_at(&t, 0, 14, &x0, &x1));
    assert(x0 == 3);
    assert(x1 == 24); /* 'https://example.org/x': cols 3..23 */
    /* pointer outside the URL: no match — including the space
     * right after it (the start-must-be-a-url-char rule) */
    assert(!term_url_at(&t, 0, 1, &x0, &x1));
    assert(!term_url_at(&t, 0, 24, &x0, &x1));
    teardown();
    printf("[ok] URL detection + Ctrl+click ranges\n");
}

static void test_search(void) {
    setup();
    for (int i = 0; i < 5; i++) {
        char line[32];
        snprintf(line, sizeof line, "line %d alpha", i);
        feed(line);
        feed("\r\n");
    }
    feed("omega");
    int col = 0;
    int64_t row = term_search(&t, "LINE 3", 0, &col);
    /* "line 3" wrote when the grid was 4 rows; row math:
     * lines 0..4 + omega; line 3 lives 2 rows above omega's row */
    assert(row >= 0);
    char *txt = term_line_text(&t, row);
    assert(txt != NULL && strncmp(txt, "line 3", 6) == 0);
    free(txt);
    /* wrap-around search */
    int64_t again = term_search(&t, "line 0", row, &col);
    assert(again >= 0);
    /* no match */
    assert(term_search(&t, "zebra", 0, &col) < 0);
    teardown();
    printf("[ok] scrollback search (case-insensitive, wrapping)\n");
}

static void test_resize(void) {
    setup();
    feed("one two three four five");
    /* 10 cols: rows 0..2 hold "one two t/ree four /ive" + an empty
     * tail row; the cursor ends on row 2. */
    term_resize(&t, 30, 2);
    assert(t.cols == 30 && t.rows == 2);
    /* the shrink keeps the BOTTOM rows (the cursor's view); the top
     * flows into scrollback — xterm's rule. */
    assert(t.grid[0].cells[0].cp == 'i');
    assert(t.grid[0].cells[1].cp == 'v');
    assert(t.grid[0].cells[2].cp == 'e');
    assert(t.back_count == 2); /* "one two t" + "ree four " */
    const trow *r = term_back_row(&t, 0);
    assert(r != NULL && r->cells[0].cp == 'o');
    /* grow back: scrollback reflows in, the tail returns */
    term_resize(&t, 10, 4);
    assert(t.cols == 10 && t.rows == 4);
    assert(t.back_count == 0);
    assert(t.grid[0].cells[0].cp == 'o');
    assert(t.grid[2].cells[0].cp == 'i');
    teardown();
    printf("[ok] grid resize keeps content (bottom rows; top to scrollback)\n");
}

static void test_keys(void) {
    setup();
    /* plain arrows in normal mode: CSI D */
    assert(vt_key(v, FDK_KEY_LEFT, 0, 0));
    size_t rl = 0;
    const char *rp = vt_reply_peek(v, &rl);
    assert(rl == 3 && memcmp(rp, "\033[D", 3) == 0);
    drain_replies();
    /* app mode: SS3 D */
    feed("\033[?1h");
    assert(vt_key(v, FDK_KEY_LEFT, 0, 0));
    rp = vt_reply_peek(v, &rl);
    assert(rl == 3 && memcmp(rp, "\033OD", 3) == 0);
    drain_replies();
    /* ctrl+shift+left: CSI 1;6D */
    assert(vt_key(v, FDK_KEY_LEFT, 0, 1 | 2));
    rp = vt_reply_peek(v, &rl);
    assert(strstr(rp, "1;6D") != NULL);
    drain_replies();
    /* ctrl+a -> 0x01 */
    assert(vt_key(v, 30, 'a', 2));
    rp = vt_reply_peek(v, &rl);
    assert(rl == 1 && rp[0] == 1);
    drain_replies();
    /* F1 in normal mode */
    assert(vt_key(v, 59, 0, 0));
    rp = vt_reply_peek(v, &rl);
    assert(rl == 5 && memcmp(rp, "\033[11~", 5) == 0);
    drain_replies();
    teardown();
    printf("[ok] key encoding: arrows/app-mode/modifiers/F-keys/ctrl\n");
}

static void test_mouse_reports(void) {
    setup();
    feed("\033[?1000;1006h"); /* normal + SGR */
    vt_mouse(v, 0, 0, 2, 3, false, false, false); /* press btn1 at 3;4 */
    size_t rl = 0;
    const char *rp = vt_reply_peek(v, &rl);
    if (!(rp != NULL && strcmp(rp, "\033[<0;3;4M") == 0)) {
        fprintf(stderr, "press reply len=%zu [", rl);
        for (size_t i = 0; i < rl; i++) {
            fprintf(stderr, "%s%d", i ? " " : "", (int)(unsigned char)rp[i]);
        }
        fprintf(stderr, "]\n");
    }
    assert(rp != NULL && strcmp(rp, "\033[<0;3;4M") == 0);
    drain_replies();
    vt_mouse(v, 1, 0, 2, 3, false, false, false); /* release */
    rp = vt_reply_peek(v, &rl);
    assert(rp != NULL && strcmp(rp, "\033[<0;3;4m") == 0);
    drain_replies();
    /* wheel */
    vt_mouse(v, 3, 0, 0, 0, false, false, false);
    rp = vt_reply_peek(v, &rl);
    assert(rp != NULL && strcmp(rp, "\033[<64;1;1M") == 0);
    drain_replies();
    teardown();
    printf("[ok] SGR mouse press/release/wheel reports\n");
}

static void test_wide_widths(void) {
    /* the width tables themselves */
    assert(tw_width('a') == 1);
    assert(tw_width(0x4E2D) == 2);    /* CJK */
    assert(tw_width(0x1F600) == 2);   /* emoji */
    assert(tw_width(0x0301) == 0);    /* combining acute */
    assert(tw_width(0x200B) == 0);    /* zero-width space */
    assert(tw_width(0x2500) == 1);    /* box drawing */
    printf("[ok] wcwidth tables\n");
}

int main(void) {
    test_wide_widths();
    test_plain_text_and_wrap();
    test_controls();
    test_sgr_colors();
    test_erase_and_scroll_region();
    test_scrollback_flow();
    test_alt_screen();
    test_wide_and_combining();
    test_dec_graphics();
    test_osc();
    test_modes_and_replies();
    test_bracketed_paste();
    test_selection_text();
    test_url_detection();
    test_search();
    test_resize();
    test_keys();
    test_mouse_reports();
    printf("all fdk-term unit tests passed\n");
    return 0;
}
