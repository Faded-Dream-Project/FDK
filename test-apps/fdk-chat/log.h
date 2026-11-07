/* log.h — the chat log view (fdk-chat).
 *
 * One read-only TextView per conversation: messages arrive as
 * styled appends (timestamps, colored nicks, mIRC-decoded bodies,
 * link ranges), the view sticks to the tail unless the reader
 * scrolled up, and Ctrl+click on a link opens it through
 * fdk_desktop_open_uri. Built on the streaming-log surface
 * (fdk_textview_append/erase/add_attr) so an all-day log is O(1)
 * per message, never a whole-document reset.
 */
#ifndef FDKCHAT_LOG_H
#define FDKCHAT_LOG_H

#include "fdk/fdk_widgets.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct chat_log chat_log;

typedef enum chat_log_kind {
    CHAT_MSG_PRIVMSG = 0, /* <nick> body                        */
    CHAT_MSG_ACTION,      /* * nick body (the /me form)         */
    CHAT_MSG_JOIN,
    CHAT_MSG_PART,
    CHAT_MSG_QUIT,
    CHAT_MSG_NICK,
    CHAT_MSG_NOTICE,
    CHAT_MSG_STATUS,      /* connection + system chatter        */
    CHAT_MSG_HIGHLIGHT,   /* a PRIVMSG that mentions us         */
} chat_log_kind;

typedef void (*chat_log_open_link_fn)(void *user, const char *href);

chat_log *chat_log_create(fdk_widget *parent, fdk_font *font,
                          chat_log_open_link_fn on_open, void *user);
void chat_log_destroy(chat_log *l);

/* Appends one message. `nick` may be NULL for system lines. The
 * body is decoded through irc_decode_mirc (colors/bold/links)
 * before it lands. */
void chat_log_append(chat_log *l, chat_log_kind kind,
                     const char *nick, const char *body);

/* A plain system line (already decoded; no mIRC pass). */
void chat_log_sysline(chat_log *l, const char *text);

/* The scrollback cap: trims from the head so the document stays
 * under `max_bytes` (the textview's own cap is 256 KB; trimming
 * keeps a day-long log alive). */
void chat_log_trim(chat_log *l, size_t max_bytes);

/* Ctrl+click: the href under the point, or false. */
bool chat_log_link_at(chat_log *l, fdk_f32 x, fdk_f32 y,
                      char *out, size_t cap);

/* The view's document size (status bar / tests). */
size_t chat_log_bytes(const chat_log *l);

/* The view widget (reparenting, focus, tests). */
fdk_widget *chat_log_widget(chat_log *l);

/* The document's text (valid until the next append). */
const char *chat_log_text(const chat_log *l);

/* Clears the whole log (the Conversation menu). */
void chat_log_clear(chat_log *l);

#endif /* FDKCHAT_LOG_H */
