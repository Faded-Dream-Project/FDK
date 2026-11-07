/* net.c — the fdk-chat connection layer (see net.h). */
#include "net.h"

#include "fdk/fdk_extend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define CHAT_OUT_MAX 8192
#define CHAT_IN_MAX (IRC_LINE_MAX_LOCAL * 2)

/* keep the line cap local so net.h stays protocol-free */
#define IRC_LINE_MAX_LOCAL 4096

typedef struct chat_conn {
    int fd;
    fdk_ext_fd_source *src;
    fdk_context *ctx;
    chat_line_fn on_line;
    chat_state_fn on_state;
    void *user;
    bool connected;
    bool closed;
    char host[128];
    int port;
    /* line assembly across partial reads */
    char in[CHAT_IN_MAX];
    size_t in_len;
    /* pending output (POLLIN + POLLOUT-while-nonempty) */
    char out[CHAT_OUT_MAX];
    size_t out_len;
} chat_conn;

static void chat_report(chat_conn *c, chat_state st,
                        const char *detail) {
    if (c->on_state != NULL) {
        c->on_state(c->user, st, detail);
    }
}

static void chat_close(chat_conn *c, const char *why) {
    if (c->closed) {
        return;
    }
    c->closed = true;
    c->connected = false;
    if (c->src != NULL) {
        fdk_ext_loop_remove_fd(c->src); /* frees the source */
        c->src = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    chat_report(c, CHAT_CLOSED, why);
}

/* Writes what it can; on EAGAIN the remainder stays queued and the
 * caller re-arms POLLOUT. */
static void chat_flush(chat_conn *c) {
    while (c->out_len > 0) {
        ssize_t n = write(c->fd, c->out, c->out_len);
        if (n > 0) {
            memmove(c->out, c->out + n, c->out_len - (size_t)n);
            c->out_len -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; /* the loop re-arms us when writable */
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        chat_close(c, "write failed");
        return;
    }
}

/* Consumes complete lines from c->in; the remainder memmoves down. */
static void chat_deliver_lines(chat_conn *c) {
    size_t start = 0;
    for (size_t i = 0; i < c->in_len; i++) {
        if (c->in[i] != '\n') {
            continue;
        }
        size_t end = i;
        if (end > start && c->in[end - 1] == '\r') {
            end--;
        }
        c->in[end] = '\0'; /* in-place: the engine parses this */
        if (end > start || c->in[start] != '\0') {
            c->on_line(c->user, c->in + start);
        }
        start = i + 1;
        if (c->closed) {
            return; /* a handler disconnected us */
        }
    }
    if (start > 0) {
        memmove(c->in, c->in + start, c->in_len - start);
        c->in_len -= start;
    }
}

static void chat_on_fd(fdk_ext_fd_source *src, uint32_t revents,
                       void *user) {
    (void)src;
    chat_conn *c = user;
    if (c->closed) {
        return;
    }
    if ((revents & (FDK_EXT_FD_ERR | FDK_EXT_FD_HUP)) != 0 &&
        (revents & FDK_EXT_FD_IN) == 0) {
        chat_close(c, c->connected ? "connection closed"
                                   : "connect failed");
        return;
    }
    if (!c->connected && (revents & FDK_EXT_FD_OUT) != 0) {
        int err = 0;
        socklen_t elen = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &elen) !=
                0 ||
            err != 0) {
            chat_close(c, strerror(err != 0 ? err : errno));
            return;
        }
        c->connected = true;
        chat_report(c, CHAT_CONNECTED, c->host);
    }
    if ((revents & FDK_EXT_FD_IN) != 0) {
        for (;;) {
            if (c->in_len == sizeof c->in) {
                /* one line longer than the buffer: drop the bytes,
                 * keep the stream coherent (split at the next \n) */
                c->in_len = 0;
            }
            ssize_t n = read(c->fd, c->in + c->in_len,
                             sizeof c->in - c->in_len);
            if (n > 0) {
                c->in_len += (size_t)n;
                chat_deliver_lines(c);
                if (c->closed) {
                    return;
                }
                if (c->in_len == sizeof c->in) {
                    continue; /* keep draining */
                }
                if (n < (ssize_t)(sizeof c->in)) {
                    break; /* likely drained; the loop re-wakes */
                }
                continue;
            }
            if (n == 0) {
                chat_close(c, "server closed the connection");
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            chat_close(c, "read failed");
            return;
        }
    }
    if (!c->closed && c->out_len > 0) {
        chat_flush(c);
    }
    if (!c->closed && c->src != NULL) {
        uint32_t want = FDK_EXT_FD_IN;
        if (!c->connected || c->out_len > 0) {
            want |= FDK_EXT_FD_OUT;
        }
        (void)fdk_ext_loop_update_fd(c->src, want);
    }
}

static bool chat_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

chat_conn *chat_connect(fdk_context *ctx, const char *host, int port,
                        chat_line_fn on_line, chat_state_fn on_state,
                        void *user) {
    if (ctx == NULL || host == NULL || *host == '\0' ||
        port <= 0 || port > 65535 || on_line == NULL) {
        return NULL;
    }
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, service, &hints, &res) != 0 ||
        res == NULL) {
        return NULL;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (!chat_set_nonblock(fd)) {
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) {
            break; /* this one is in flight */
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return NULL;
    }
    chat_conn *c = calloc(1, sizeof *c);
    if (c == NULL) {
        close(fd);
        return NULL;
    }
    c->fd = fd;
    c->ctx = ctx;
    c->on_line = on_line;
    c->on_state = on_state;
    c->user = user;
    snprintf(c->host, sizeof c->host, "%s", host);
    c->port = port;

    c->src = fdk_ext_loop_add_fd(ctx, fd,
                                 FDK_EXT_FD_IN | FDK_EXT_FD_OUT,
                                 chat_on_fd, c);
    if (c->src == NULL) {
        close(fd);
        free(c);
        return NULL;
    }
    chat_report(c, CHAT_CONNECTING, host);
    return c;
}

bool chat_send(chat_conn *c, const char *line) {
    if (c == NULL || c->closed || line == NULL) {
        return false;
    }
    size_t len = strlen(line);
    if (len > 510) {
        len = 510; /* RFC: 512 including CRLF; refuse silently long */
    }
    if (c->out_len + len + 2 > sizeof c->out) {
        chat_flush(c);
        if (c->out_len + len + 2 > sizeof c->out) {
            return false; /* genuinely full; the caller decides */
        }
    }
    memcpy(c->out + c->out_len, line, len);
    c->out_len += len;
    c->out[c->out_len++] = '\r';
    c->out[c->out_len++] = '\n';
    chat_flush(c);
    if (!c->closed && c->src != NULL && c->out_len > 0) {
        (void)fdk_ext_loop_update_fd(c->src,
                                     FDK_EXT_FD_IN | FDK_EXT_FD_OUT);
    }
    return !c->closed;
}

void chat_disconnect(chat_conn *c) {
    if (c == NULL) {
        return;
    }
    c->closed = true; /* skip the CHAT_CLOSED report on teardown */
    if (c->src != NULL) {
        fdk_ext_loop_remove_fd(c->src);
        c->src = NULL;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    free(c);
}

bool chat_is_connected(const chat_conn *c) {
    return c != NULL && c->connected && !c->closed;
}

size_t chat_pending_out(const chat_conn *c) {
    return (c != NULL) ? c->out_len : 0;
}
