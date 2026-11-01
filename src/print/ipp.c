/*
 * ipp.c — the native IPP client (§23/B2f; fdk_print.h).
 *
 * Printer discovery and job submission over raw TCP with the
 * protocol's own bytes: RFC 8010's encoding (the 8-byte header,
 * delimiter-tagged attribute groups, value-tagged attributes,
 * end-of-attributes) carried in an HTTP/1.1 POST with
 * Content-Type: application/ipp, speaking RFC 8011's operations
 * (CUPS-Get-Printers, Get-Printer-Attributes, Print-Job). No
 * libcups, no D-Bus — the same "own wire or nothing" posture as
 * the D-Bus client.
 *
 * The server: $CUPS_SERVER (host[:port]) or localhost:631 — the
 * every-distro default. Timeouts are honored with poll(2); every
 * failure is a quiet empty result (a desktop without a print
 * server is a normal desktop).
 */

#define FDK_LOG_TAG "print"

#include "fdk/fdk_print.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- the server target -------------------------------------------------- */

static void ipp_server(char *host, size_t host_cap, int *out_port) {
    const char *env = getenv("CUPS_SERVER");
    if (env != NULL && env[0] != '\0' && host_cap > 0) {
        snprintf(host, host_cap, "%s", env);
        char *colon = strrchr(host, ':');
        if (colon != NULL && colon[1] != '\0') {
            *colon = '\0';
            *out_port = atoi(colon + 1);
            if (*out_port <= 0) {
                *out_port = 631;
            }
            return;
        }
    }
    snprintf(host, host_cap, "localhost");
    *out_port = 631;
}

/* A connect-with-timeout TCP socket, or -1. */
static int tcp_connect(const char *host, int port, int timeout_ms) {
    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        /* Non-blocking connect + poll: the whole discovery must
         * honor the caller's timeout, not the kernel's. */
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) {
            (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        int cr = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, timeout_ms) > 0) {
                int err = 0;
                socklen_t el = sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err,
                               &el) == 0 &&
                    err == 0) {
                    break; /* connected */
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Buffered send-all / recv-with-timeout. */
static bool send_all(int fd, const unsigned char *data, size_t len,
                     int timeout_ms) {
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) {
            return false;
        }
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

static ssize_t recv_some(int fd, unsigned char *buf, size_t cap,
                         int timeout_ms) {
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
        return -1;
    }
    return recv(fd, buf, cap, 0);
}

/* ---- the IPP message builder ------------------------------------------------ */

/* The encoder: a growable byte buffer with the RFC 8010 helpers.
 * The header is written at FLUSH time (the request needs its
 * length only for the HTTP layer, which we also build). */
typedef struct {
    unsigned char *buf;
    size_t len, cap;
    bool oom;
} ipp_buf;

static void ib_bytes(ipp_buf *b, const void *data, size_t n) {
    if (b->oom) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 512;
        while (cap < b->len + n) {
            cap *= 2;
        }
        unsigned char *grown = fdk_realloc(b->buf, cap);
        if (grown == NULL) {
            b->oom = true;
            return;
        }
        b->buf = grown;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
}

static void ib_u16(ipp_buf *b, unsigned v) {
    unsigned char t[2] = {(unsigned char)(v >> 8),
                          (unsigned char)(v & 0xFF)};
    ib_bytes(b, t, 2);
}
static void ib_u32(ipp_buf *b, unsigned v) {
    unsigned char t[4] = {(unsigned char)(v >> 24),
                          (unsigned char)((v >> 16) & 0xFF),
                          (unsigned char)((v >> 8) & 0xFF),
                          (unsigned char)(v & 0xFF)};
    ib_bytes(b, t, 4);
}

/* An attribute: value-tag, name-length, name, value-length, value
 * (RFC 8010 §3.1.4). */
static void ib_attr(ipp_buf *b, unsigned char tag, const char *name,
                    const char *value) {
    size_t nl = strlen(name);
    size_t vl = strlen(value);
    ib_bytes(b, &tag, 1);
    ib_u16(b, (unsigned)nl);
    ib_bytes(b, name, nl);
    ib_u16(b, (unsigned)vl);
    ib_bytes(b, value, vl);
}

/* Builds a complete IPP request body (version 1.1). */
static bool ipp_build(ipp_buf *b, unsigned operation,
                      const char *printer_uri) {
    ib_u16(b, 0x0101);      /* version-number 1.1                 */
    ib_u16(b, operation);   /* operation-id                       */
    ib_u32(b, 1);           /* request-id                         */
    ib_bytes(b, "\x01", 1); /* operation-attributes-tag           */
    ib_attr(b, 0x47, "attributes-charset", "utf-8");
    ib_attr(b, 0x48, "attributes-natural-language", "en");
    if (printer_uri != NULL) {
        ib_attr(b, 0x45, "printer-uri", printer_uri);
    }
    ib_attr(b, 0x42, "requesting-user-name", "fdk");
    ib_bytes(b, "\x03", 1); /* end-of-attributes-tag              */
    return !b->oom;
}

/* ---- the HTTP wrapper -------------------------------------------------------- */

/* POSTs a body, returns the response (headers skipped) in a
 * caller-freed buffer. */
static unsigned char *http_post_ipp(int fd, const char *host,
                                    const char *resource,
                                    const unsigned char *body,
                                    size_t body_len, size_t *out_len,
                                    int timeout_ms) {
    char head[512];
    int hn = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/ipp\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      resource, host, body_len);
    if (hn <= 0 || !send_all(fd, (const unsigned char *)head,
                             (size_t)hn, timeout_ms) ||
        !send_all(fd, body, body_len, timeout_ms)) {
        return NULL;
    }

    /* Read to EOF (Connection: close) into one buffer. */
    size_t cap = 8192, len = 0;
    unsigned char *resp = fdk_alloc(cap);
    if (resp == NULL) {
        return NULL;
    }
    for (;;) {
        if (len == cap) {
            size_t cap2 = cap * 2;
            unsigned char *grown = fdk_realloc(resp, cap2);
            if (grown == NULL) {
                fdk_free(resp);
                return NULL;
            }
            resp = grown;
            cap = cap2;
        }
        ssize_t n = recv_some(fd, resp + len, cap - len, timeout_ms);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }

    /* Skip the HTTP headers: the blank line. */
    unsigned char *body_at = NULL;
    size_t body_len_out = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' &&
            resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            body_at = resp + i + 4;
            body_len_out = len - (i + 4);
            break;
        }
    }
    if (body_at == NULL) {
        fdk_free(resp);
        return NULL;
    }
    /* Move the body to a tight buffer. */
    unsigned char *out = fdk_alloc(body_len_out);
    if (out == NULL) {
        fdk_free(resp);
        return NULL;
    }
    memcpy(out, body_at, body_len_out);
    fdk_free(resp);
    *out_len = body_len_out;
    return out;
}

/* ---- the response parser ------------------------------------------------------- */

/* Collects printer-name + printer-state per printer in a
 * CUPS-Get-Printers response: walks the attributes, starts a new
 * printer at each printer-name. Returns the count. */
static size_t ipp_parse_printers(const unsigned char *resp,
                                 size_t len, fdk_printer_info *out,
                                 size_t cap) {
    if (resp == NULL || len < 9) {
        return 0;
    }
    size_t count = 0;
    fdk_printer_info cur;
    memset(&cur, 0, sizeof cur);
    bool have_cur = false;

    size_t i = 8;
    char namebuf[128] = "";
    char valbuf[256] = "";
    while (i + 3 <= len && count < (out ? cap : (size_t)-1)) {
        unsigned char tag = resp[i];
        if (tag == 0x03) {
            break;
        }
        if (tag >= 0x10 && i + 3 <= len) {
            size_t nl = ((size_t)resp[i + 1] << 8) | resp[i + 2];
            size_t at = i + 3;
            if (at + nl + 2 > len) {
                break;
            }
            size_t vl = ((size_t)resp[at + nl] << 8) |
                        resp[at + nl + 1];
            size_t vat = at + nl + 2;
            if (vat + vl > len) {
                break;
            }
            size_t ncopy = nl < sizeof namebuf - 1
                               ? nl
                               : sizeof namebuf - 1;
            memcpy(namebuf, resp + at, ncopy);
            namebuf[ncopy] = '\0';
            size_t vcopy = vl < sizeof valbuf - 1 ? vl
                                                  : sizeof valbuf - 1;
            memcpy(valbuf, resp + vat, vcopy);
            valbuf[vcopy] = '\0';

            if (strcmp(namebuf, "printer-name") == 0) {
                if (have_cur && out != NULL && count < cap) {
                    out[count++] = cur;
                } else if (have_cur) {
                    count++;
                }
                memset(&cur, 0, sizeof cur);
                snprintf(cur.name, sizeof cur.name, "%.127s",
                         valbuf);
                cur.accepts_pdf = true; /* the common case; the
                                         * attributes refine it  */
                have_cur = true;
            } else if (strcmp(namebuf, "printer-state") == 0) {
                snprintf(cur.state, sizeof cur.state, "%.31s",
                         valbuf);
            } else if (strcmp(namebuf,
                              "printer-uri-supported") == 0) {
                snprintf(cur.uri, sizeof cur.uri, "%.255s",
                         valbuf);
            }
            i = vat + vl;
        } else {
            i++;
        }
    }
    if (have_cur) {
        if (out != NULL && count < cap) {
            out[count++] = cur;
        } else {
            count++;
        }
    }
    return count;
}

/* ---- public ----------------------------------------------------------------- */

size_t fdk_print_enumerate_printers(fdk_printer_info *out,
                                    size_t cap, int timeout_ms) {
    char host[128];
    int port = 631;
    ipp_server(host, sizeof host, &port);

    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) {
        return 0; /* no print server: a normal desktop */
    }

    ipp_buf b = {NULL, 0, 0, false};
    if (!ipp_build(&b, 0x4002 /* CUPS-Get-Printers */, NULL)) {
        fdk_free(b.buf);
        close(fd);
        return 0;
    }
    size_t resp_len = 0;
    unsigned char *resp = http_post_ipp(fd, host, "/", b.buf, b.len,
                                        &resp_len, timeout_ms);
    fdk_free(b.buf);
    close(fd);
    if (resp == NULL) {
        return 0;
    }

    size_t n = ipp_parse_printers(resp, resp_len, out, cap);
    fdk_free(resp);

    /* The URIs the dialog submits against: ipp://host:port/
     * printers/NAME (the response's printer-uri-supported usually
     * carries it; synthesize when it did not). */
    if (out != NULL) {
        for (size_t i = 0; i < n && i < cap; i++) {
            if (out[i].uri[0] == '\0') {
                snprintf(out[i].uri, sizeof out[i].uri,
                         "ipp://%s:%d/printers/%s", host, port,
                         out[i].name);
            }
        }
    }
    FDK_DEBUG("ipp: %zu printer(s) from %s:%d", n, host, port);
    return n;
}

fdk_result fdk_print_submit(const fdk_printer_info *printer,
                            const unsigned char *pdf, size_t len,
                            const char *job_name, int timeout_ms) {
    if (printer == NULL || pdf == NULL || len == 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }

    /* Parse the URI back to host/port/resource. */
    char host[128];
    int port = 631;
    char resource[256] = "/ipp";
    if (strncmp(printer->uri, "ipp://", 6) == 0) {
        const char *at = printer->uri + 6;
        const char *slash = strchr(at, '/');
        size_t hlen = slash != NULL ? (size_t)(slash - at)
                                    : strlen(at);
        if (hlen >= sizeof host) {
            hlen = sizeof host - 1;
        }
        memcpy(host, at, hlen);
        host[hlen] = '\0';
        char *colon = strrchr(host, ':');
        if (colon != NULL) {
            *colon = '\0';
            port = atoi(colon + 1);
            if (port <= 0) {
                port = 631;
            }
        }
        if (slash != NULL) {
            snprintf(resource, sizeof resource, "%.200s", slash);
        }
    } else {
        snprintf(host, sizeof host, "localhost");
    }

    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) {
        return FDK_ERR_PLATFORM;
    }

    /* The Print-Job request (0x0002): operation attributes with
     * the printer-uri and job-name, then the document bytes. */
    ipp_buf b = {NULL, 0, 0, false};
    ib_u16(&b, 0x0101);
    ib_u16(&b, 0x0002); /* Print-Job */
    ib_u32(&b, 2);
    ib_bytes(&b, "\x01", 1);
    ib_attr(&b, 0x47, "attributes-charset", "utf-8");
    ib_attr(&b, 0x48, "attributes-natural-language", "en");
    ib_attr(&b, 0x45, "printer-uri", printer->uri);
    ib_attr(&b, 0x42, "requesting-user-name", "fdk");
    ib_attr(&b, 0x49, "job-name",
            job_name != NULL ? job_name : "FDK print job");
    ib_attr(&b, 0x49, "document-format", "application/pdf");
    ib_bytes(&b, "\x03", 1);
    ib_bytes(&b, pdf, len);
    if (b.oom) {
        fdk_free(b.buf);
        close(fd);
        return FDK_ERR_OUT_OF_MEMORY;
    }

    size_t resp_len = 0;
    unsigned char *resp = http_post_ipp(fd, host, resource, b.buf,
                                        b.len, &resp_len, timeout_ms);
    fdk_free(b.buf);
    close(fd);
    if (resp == NULL) {
        return FDK_ERR_PLATFORM;
    }

    /* The status-code (bytes 2-3 of the IPP body): 0x0000 =
     * successful-ok. */
    fdk_result r = FDK_ERR_PLATFORM;
    if (resp_len >= 4) {
        unsigned status = ((unsigned)resp[2] << 8) | resp[3];
        if (status == 0x0000) {
            r = FDK_OK;
        } else {
            FDK_WARN("ipp: Print-Job refused (status 0x%04X)",
                     status);
        }
    }
    fdk_free(resp);
    return r;
}
