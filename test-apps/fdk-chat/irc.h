/* irc.h — the IRC protocol engine (fdk-chat).
 *
 * RFC 1459/2812 message parsing, mIRC color decode, channel state —
 * UI-independent and unit-tested headless (chat_test.c). The UI
 * maps the engine's outputs onto FDK widgets; nothing here includes
 * an FDK header, so the protocol layer is testable without a
 * display.
 *
 * Sources: RFC 1459 (May 1993), RFC 2812 (April 2000) — the ABNF
 * and the numeric replies; the mIRC color codes are the de-facto
 * control-code convention every client speaks (0x02 bold, 0x1D
 * italic, 0x1F underline, 0x1E strikethrough, 0x11 monospace,
 * 0x16 reverse, 0x0F reset, 0x03 <fg>[,<bg>] 0-15 + 16-98 256-color,
 * 99/default, 0x29 reset colors).
 */
#ifndef FDKCHAT_IRC_H
#define FDKCHAT_IRC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- limits ------------------------------------------------------- */
#define IRC_LINE_MAX 4096  /* RFC says 512; be liberal in what we
                            * accept, strict in what we send */
#define IRC_NICK_MAX 64
#define IRC_CHAN_MAX 128
#define IRC_TOPIC_MAX 512
#define IRC_PARAMS_MAX 15

/* ---- the parsed line ----------------------------------------------
 * The engine PARSES IN PLACE: nick/user/host/command/params/trailing
 * point into the caller's line buffer (NULs replace the separators),
 * so a parsed message is valid exactly as long as the line buffer
 * is. The caller's line must stay writable (the socket read path
 * hands the assembled line straight in). */
typedef struct irc_msg {
    const char *raw_tags; /* the @tags blob (no leading @), NULL */
    const char *nick;     /* prefix parts; NULL when no prefix   */
    const char *user;
    const char *host;
    const char *command;  /* verbatim case as sent                */
    int numeric;          /* 100..599 when the command is a code  */
    const char *params[IRC_PARAMS_MAX];
    size_t n_params;
    const char *trailing; /* the last, colon-prefixed param; NULL
                           * when the message has none            */
} irc_msg;

/* Parses one line (without CRLF). Returns false when the line is
 * empty after tag/prefix stripping (heartbeats, comment lines) —
 * not an error, just nothing to dispatch. */
bool irc_parse(char *line, irc_msg *out);

/* ---- mIRC-styled text ---------------------------------------------
 * A segment is a styled run over a decoded copy of the input. The
 * decoded text is written to `out_text` (cap >= len+1; control
 * codes never expand); the segments' text pointers reference it.
 * `segs` cap-bounded; overflow keeps the LAST segment and returns
 * false (the UI renders the tail unstyled rather than losing
 * text). */
#define IRC_SEG_MAX 32

typedef struct irc_seg {
    const char *text; /* into out_text; NOT NUL-terminated —
                       * the (text,len) pair is the slice (later
                       * text overwrites the flush NULs)       */
    size_t len;
    bool bold, italic, underline, strike, mono, reverse;
    uint8_t fg, bg;   /* 0..98 (16 = the 256-color cube's first),
                       * 99 = default/none                        */
    bool fg_set, bg_set;
} irc_seg;

bool irc_decode_mirc(const char *src, size_t len, char *out_text,
                     size_t out_cap, irc_seg *segs, size_t seg_cap,
                     size_t *out_n);

/* ---- urls ----------------------------------------------------------
 * Word-boundary spans over plain text: http/https/www. — the caller
 * maps [start,end) to its own rendering. Cap-bounded; the scan is a
 * single pass. */
#define IRC_URLS_MAX 8

typedef struct irc_url {
    size_t start, end; /* byte offsets into the text            */
} irc_url;

size_t irc_find_urls(const char *text, size_t len, irc_url *out,
                     size_t cap);

/* true when `text` (case-insensitive, word-boundary) mentions
 * `nick` — the highlight/notification rule. */
bool irc_mentions(const char *nick, const char *text);

/* ---- channel state ------------------------------------------------- */

typedef struct irc_user {
    char nick[IRC_NICK_MAX];
    char mode; /* '~' '&' '@' '%' '+' or 0                        */
} irc_user;

typedef struct irc_chan {
    char name[IRC_CHAN_MAX];
    char topic[IRC_TOPIC_MAX];
    irc_user *users;
    size_t n_users, cap_users;
    bool names_done; /* after 366: the list is authoritative */
} irc_chan;

typedef struct irc_state {
    char nick[IRC_NICK_MAX];
    irc_chan *chans;
    size_t n_chans, cap_chans;
} irc_state;

void irc_state_init(irc_state *st);
void irc_state_destroy(irc_state *st);

/* Channel names are case-insensitive (RFC 2812 casemapping: ASCII
 * tolower plus {}| -> []\). irc_chan_find normalizes internally. */
irc_chan *irc_chan_find(irc_state *st, const char *name);
irc_chan *irc_chan_join(irc_state *st, const char *name);
bool irc_chan_part(irc_state *st, const char *name);

/* Adds one user (with mode prefix stripped) to the channel. */
bool irc_chan_user_add(irc_chan *ch, const char *nick);
bool irc_chan_user_remove(irc_chan *ch, const char *nick);
irc_user *irc_chan_user_find(irc_chan *ch, const char *nick);
bool irc_chan_user_rename(irc_chan *ch, const char *from,
                          const char *to);
/* A 353 NAMES body: "@nick1 +nick2 nick3 ..." — modes stripped. */
void irc_chan_names_add(irc_chan *ch, const char *names_line);

/* true when the name is a channel (# or &). */
bool irc_is_channel(const char *name);

#endif /* FDKCHAT_IRC_H */
