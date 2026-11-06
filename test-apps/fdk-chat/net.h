/* net.h — the fdk-chat connection layer.
 *
 * A real TCP IRC client on FDK's extension fd loop: non-blocking
 * connect (EINPROGRESS + the writable edge), line assembly across
 * partial reads, a bounded output buffer with POLLIN/POLLOUT
 * re-arming (the documented anti-busy-wake rule), and honest state
 * reporting. UI-independent; the tests drive it against a local
 * socket server.
 */
#ifndef FDKCHAT_NET_H
#define FDKCHAT_NET_H

#include <stdbool.h>
#include <stddef.h>

struct fdk_context;
typedef struct fdk_context fdk_context;

typedef struct chat_conn chat_conn;

typedef enum chat_state {
    CHAT_CONNECTING = 0,
    CHAT_CONNECTED,
    CHAT_CLOSED, /* refused, reset, or ended — detail says which */
} chat_state;

/* Lines arrive WITHOUT CRLF, NUL-terminated, valid until the
 * callback returns (the engine parses in place). */
typedef void (*chat_line_fn)(void *user, const char *line);
typedef void (*chat_state_fn)(void *user, chat_state state,
                              const char *detail);

/* Opens a non-blocking connection to host:port and watches it on
 * ctx's loop. Never blocks the caller: CHAT_CONNECTING is reported
 * immediately, CHAT_CONNECTED when the socket completes, CHAT_CLOSED
 * with the reason when it never will. NULL on argument/alloc/socket
 * failure (no callbacks fired). */
chat_conn *chat_connect(fdk_context *ctx, const char *host, int port,
                        chat_line_fn on_line, chat_state_fn on_state,
                        void *user);

/* Queues a line (CRLF added); returns false when closed or the
 * queue would overflow. The line is copied. */
bool chat_send(chat_conn *c, const char *line);

/* Teardown: removes the fd source, closes the socket, frees. */
void chat_disconnect(chat_conn *c);

/* Diagnostics for the status bar / tests. */
bool chat_is_connected(const chat_conn *c);
size_t chat_pending_out(const chat_conn *c);

#endif /* FDKCHAT_NET_H */
