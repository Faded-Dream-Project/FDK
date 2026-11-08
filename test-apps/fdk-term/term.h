/* term.h — the terminal grid model: cells, scrollback, selection,
 * and the screen state the VT parser drives.
 *
 * Two grids per terminal: the PRIMARY grid (scrollback flows off its
 * top into a ring buffer) and the ALTERNATE grid (vim/htop/less
 * switch to it; no scrollback, cursor state saved/restored). Cells
 * carry the DEFERRED color pair (colors.h) so a scheme switch never
 * rewrites the grid.
 *
 * Wide characters occupy a cell pair: the lead cell carries the
 * codepoint, the trail cell is a continuation marker (never painted
 * as itself, hit as part of the lead). Combining marks (width 0)
 * append to the previous cell's cluster.
 *
 * The model is UI-free: vt.c feeds it, view.c paints it, the unit
 * tests drive it directly. */
#ifndef FDKTERM_TERM_H
#define FDKTERM_TERM_H

#include "colors.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TATTR_BOLD      (1u << 0)
#define TATTR_ITALIC    (1u << 1)
#define TATTR_UNDERLINE (1u << 2)
#define TATTR_BLINK     (1u << 3)
#define TATTR_INVERSE   (1u << 4)
#define TATTR_INVISIBLE (1u << 5)
#define TATTR_STRIKE    (1u << 6)
#define TATTR_FAINT     (1u << 7)

/* A continuation cell: the right half of a wide char. */
#define TATTR_WIDE_CONT (1u << 8)

typedef struct tcell {
    uint32_t cp;      /* lead codepoint (clusters store the first) */
    uint32_t cluster[2]; /* up to 2 extra combining codepoints     */
    uint8_t ncluster; /* extra codepoints in cluster (0..2)        */
    tcolor fg, bg;    /* deferred colors (index or truecolor)      */
    uint32_t attr;    /* TATTR_* bits (a private pen bit 9 too)    */
    bool hyperlink;   /* cell belongs to a URL range (row-local)   */
} tcell;

/* A scrollback line: wrapped lines keep their continuation flag so
 * the viewport can draw the soft-wrap indicator and reflow honesty. */
typedef struct trow {
    tcell *cells;
    int cols;
    bool wrapped;   /* the line's tail continues on the next row   */
} trow;

typedef struct trange {
    int x0, y0, x1, y1; /* inclusive start, exclusive-ish end grid
                         * coordinates (y in absolute scrollback
                         * space: row index + scrollback offset)   */
} trange;

typedef enum tsel_mode {
    TSEL_CHAR = 0,  /* character-wise (the default)               */
    TSEL_WORD,      /* word-wise (double-click)                    */
    TSEL_LINE,      /* line-wise (triple-click)                    */
    TSEL_BLOCK,     /* rectangular (Alt+drag)                      */
} tsel_mode;

typedef struct term {
    int cols, rows;         /* current grid size                    */
    int scroll_top, scroll_bot; /* the DEC scroll region (inclusive,
                                 * bottom-exclusive at rows)         */
    int cx, cy;             /* cursor (col in cells, row in grid)   */

    trow *grid;             /* the primary grid: rows[]             */
    int ngrid;
    /* the alt array's own row count: term_resize's primary path
     * regrids the main screen without touching alt, so ngrid and
     * rows both drift away from the alt allocation — freeing it by
     * t->rows overflowed the heap (ASan, the 1.0.0 close report) */
    int nalt;              /* == rows                              */

    trow *alt;              /* the alternate grid                   */
    int alt_cursor_x, alt_cursor_y;
    bool on_alt;            /* currently displaying the alt grid    */

    trow **back;            /* scrollback ring: back[0] is the      */
    size_t back_head, back_count, back_cap;

    tcell pen;              /* the current SGR pen (cp unused)      */

    bool cursor_visible;
    bool cursor_blink;      /* app-mode preference from CSI ? 12    */
    bool app_cursor_keys;   /* DECCKM                               */
    bool app_keypad;        /* DECPNM/DECPAM                        */
    bool bracketed_paste;   /* CSI ? 2004                           */
    bool autowrap;          /* DECAWM (default on)                  */
    bool origin_mode;       /* DECOM                                */
    bool insert_mode;       /* CSI 4 h (IRM)                        */

    /* Mouse reporting: 0 off; 9 = X10; 1000 = normal; 1002 = button-
     * event; 1003 = any-event; 1005/1006 = the encoding (SGR). */
    int mouse_mode;
    bool mouse_sgr;
    bool mouse_utf8; /* 1005 */

    /* Selection state (absolute row = grid row + scrollback). */
    tsel_mode sel_mode;
    bool sel_active;
    int sel_x0, sel_x1;      /* columns                       */
    int64_t sel_y0, sel_y1;  /* absolute rows (scrollback space) */
    bool sel_rect; /* block mode */

    /* Bell + dirty tracking */
    bool bell_pending;
    int64_t last_output_ms; /* for the bg-finish notification       */
    int64_t last_input_ms;

    /* The saved cursor state (DECSC/DECRC + alt-screen switch). */
    struct {
        int cx, cy;
        tcell pen;
        bool origin_mode;
    } saved;

    /* The UTF-8 output queue the view drains into the PTY (paste,
     * key replies): owned, grown as needed. */
    char *reply;
    size_t reply_len, reply_cap;
} term;

/* Lifecycle + resize. resize keeps content where xterm keeps it
 * (rows gained: scrollback top reflows in; rows lost: bottom lines
 * flow out; cols grown: wrapped lines rejoin per the wrapped flag). */
void term_init(term *t, int cols, int rows, size_t scrollback_lines);
void term_free(term *t);
void term_resize(term *t, int cols, int rows);

/* The row the absolute index refers to (alt-grid aware). */
trow *term_row(term *t, int grid_y);

/* The ring push the VT parser's scroll path uses. */
void term__back_push(term *t, trow *r);

/* Scrollback row by absolute index (primary grid only; NULL when
 * out of range or the alt grid is active). */
const trow *term_back_row(const term *t, size_t abs_index);

/* Absolute (scrollback-space) row count. */
int64_t term_abs_rows(term *t);

/* Cell access with allocation safety. */
tcell *term_cell(term *t, int y, int x);

/* Emits the string of a selection (rect-aware, alt-aware) into a
 * malloc'd UTF-8 buffer; NULL when the selection is empty. */
char *term_selection_text(const term *t);

/* Word/line boundaries for the selection modes (cluster-aware). */
void term_word_bounds(const term *t, int64_t abs_y, int x,
                      int *out_x0, int *out_x1);

/* URL ranges: scans the (absolute) line for a URL containing (x,
 * abs_y); fills the [x0, x1) column range. Returns true when found
 * (the caller opens term_line_ptr(t, abs_y) cells [x0..x1)). */
bool term_url_at(const term *t, int64_t abs_y, int x,
                 int *out_x0, int *out_x1);

/* Search in scrollback: case-insensitive substring over plain text;
 * returns the absolute row of the first match at-or-after `from`
 * (wrapping), -1 when none. *out_col gets the match column. */
int64_t term_search(const term *t, const char *needle, int64_t from,
                    int *out_col);

/* A line's plain UTF-8 text (malloc'd; the caller frees). */
char *term_line_text(const term *t, int64_t abs_y);

#endif /* FDKTERM_TERM_H */
