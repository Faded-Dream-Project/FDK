/* vt.h — the xterm-compatible escape-sequence parser.
 *
 * A state machine over the PTY's byte stream: ground, escape, CSI,
 * OSC, DCS, charset selection, and the string queue the terminal
 * writes back (device replies, mouse reports, bracketed paste).
 * Everything mutates the `term` grid via the operations below; the
 * parser owns NO display state (view.c paints the term).
 *
 * Coverage (the brief's list): C0 controls, CSI (private ? modes,
 * SGR incl. 256/truecolor, cursor movement, erase/insert/delete,
 * scroll regions, DECSTBM), OSC 0/2 (title), OSC 7 (cwd), OSC 8
 * (hyperlinks), OSC 52 (clipboard), ESC sequences (RIS, DECSC/DECRC,
 * charsets incl. DEC special graphics), alternate screen (1049/47/
 * 1047/1048), bracketed paste (2004), mouse reporting (X10=9,
 * normal=1000, button-event=1002, any-event=1003, SGR=1006,
 * UTF-8=1005), application cursor keys (DECCKM), DEC line-drawing
 * (G0/G1 designation + SO/SI). */
#ifndef FDKTERM_VT_H
#define FDKTERM_VT_H

#include "term.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vt vt;

/* The parser's output requests: things the APP (not the grid) must
 * do. Queued per byte batch so one write() can ask for several. */
typedef struct vt_out {
    char *title;        /* OSC 0/2: strdup'd, app sets the tab label */
    char *cwd;          /* OSC 7: strdup'd                          */
    char *clipboard;    /* OSC 52: strdup'd base64-decoded text     */
    bool clipboard_set; /* the OSC 52 actually carried a payload    */
    bool bell;          /* BEL arrived                              */
    bool request_clipboard_read; /* OSC 52 with empty payload: the
                                  * app should SEND the clipboard    */
    bool dirty;         /* the grid changed at all (repaint)        */
    bool title_changed;
} vt_out;

/* Allocates the parser for `t`. */
vt *vt_create(term *t);
void vt_destroy(vt *vt);

/* Feeds bytes. `out` is zeroed by the call and accumulates requests
 * for THIS batch; the strings are owned by the parser and replaced
 * on the next feed (consume before the next call). */
void vt_feed(vt *vt, const char *bytes, size_t n, vt_out *out);

/* The reply queue: bytes the terminal owes the PTY (key replies,
 * device reports, mouse encodings). Drained by the app into the
 * master fd; cleared by vt_reply_take. */
const char *vt_reply_peek(const vt *vt, size_t *out_len);
void vt_reply_consume(vt *vt, size_t n);

/* Reports an event per the active mouse mode (no-op when mouse
 * reporting is off). bstate: 0=press,1=release,2=motion(with the
 * button encoded),3=wheel up,4=wheel down. */
void vt_mouse(vt *vt, int bstate, int button, int x, int y,
              bool shift, bool alt, bool ctrl);

/* Bracketed paste wrapping (the app calls with the pasted text). */
void vt_paste(vt *vt, const char *text, size_t len);

/* A key's escape sequence per the MODES (app cursor keys, keypad).
 * `key` is a FDK_KEY_* scancode; cp is the printable codepoint (0
 * when none). Writes to the reply queue. Returns false when the key
 * is not a recognized special (the caller inserts cp as text). */
bool vt_key(vt *vt, uint32_t scancode, uint32_t cp, uint32_t mods);

/* True when the parser is mid-sequence (the smoke tests use this to
 * wait for a quiescent parser before screenshotting). */
bool vt_busy(const vt *vt);

#endif /* FDKTERM_VT_H */
