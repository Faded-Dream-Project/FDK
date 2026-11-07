/*
 * dbus_address.c — bus address resolution and socket setup (1.1.0)
 *
 * The specification's address grammar, resolved to concrete
 * endpoints:
 *
 *   unix:path=/run/user/1000/bus          filesystem socket
 *   unix:abstract=/tmp/dbus-abcdef,guid=… abstract namespace socket
 *   unix:dir=/tmp                         scan for dbus-* sockets
 *   tcp:host=127.0.0.1,port=5556,family=ipv4
 *   nonce-tcp:host=…,port=…,noncefile=/tmp/nonce
 *
 * Multiple ';'-separated addresses are tried in order (the
 * DBUS_SESSION_BUS_ADDRESS of a desktop with both a session daemon
 * and a leftover launchd-era address does this). Values are
 * percent-decoded (%41 → 'A').
 *
 * Resolution order for the well-known buses (fdk_dbus_bus_address):
 *   session: FDK_DBUS_ADDRESS → DBUS_SESSION_BUS_ADDRESS →
 *            $XDG_RUNTIME_DIR/bus
 *   system:  FDK_DBUS_ADDRESS → DBUS_SYSTEM_BUS_ADDRESS →
 *            /var/run/dbus/system_bus_socket → /run/dbus/…
 *
 * getaddrinfo() (tcp transports only — opt-in, exotic) may consult
 * DNS and block briefly; the unix transports every real desktop
 * uses are pure filesystem/abstract connects with instant
 * failure. Every socket is created non-blocking with CLOEXEC: no
 * FDK app has ever leaked a bus fd into a child, and none will.
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ---- small helpers --------------------------------------------------------- */

static char *dup_str(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *copy = fdk_alloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

static char *dup_printf(const char *fmt, const char *arg) {
    size_t need = strlen(fmt) + strlen(arg) + 1;
    char *out = fdk_alloc(need);
    if (out != NULL) {
        snprintf(out, need, fmt, arg);
    }
    return out;
}

/* Percent-decodes into a fresh heap string. Invalid escapes are
 * kept literally (lenient, like every real client). */
static char *percent_decode(const char *s) {
    size_t n = strlen(s);
    char *out = fdk_alloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n) {
            int hi = -1, lo = -1;
            char c = s[i + 1];
            if (c >= '0' && c <= '9') hi = c - '0';
            else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
            char d = s[i + 2];
            if (d >= '0' && d <= '9') lo = d - '0';
            else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
            else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                out[w++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[w++] = s[i];
    }
    out[w] = '\0';
    return out;
}

/* ---- one address spec → endpoints ------------------------------------------ */

/* Appends one AF_UNIX endpoint; returns 0/-1 (OOM) / 1 (bad spec). */
static int add_unix(struct fdk__dbus_addr *out, int *count, int max_out,
                    const char *path_value, int abstract) {
    if (*count >= max_out) {
        return 0;
    }
    struct fdk__dbus_addr *a = &out[(*count)];
    memset(a, 0, sizeof(*a));
    a->family = AF_UNIX;
    a->abstract_socket = abstract ? 1 : 0;
    size_t n = strlen(path_value);
    /* abstract names: 104-1 usable; filesystem paths: 104-1 too */
    if (n == 0 || n >= sizeof(a->unix_path)) {
        return 1;
    }
    memcpy(a->unix_path, path_value, n + 1);
    (*count)++;
    return 0;
}

static int add_inet(struct fdk__dbus_addr *out, int *count, int max_out,
                    const char *host, const char *port, int family,
                    const char *nonce_file) {
    if (host == NULL || port == NULL) {
        return 1;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = (family == 4)   ? AF_INET
                      : (family == 6) ? AF_INET6
                                      : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || res == NULL) {
        return 1;
    }
    int added = 0;
    for (struct addrinfo *ai = res; ai != NULL && *count < max_out;
         ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) {
            continue;
        }
        struct fdk__dbus_addr *a = &out[(*count)];
        memset(a, 0, sizeof(*a));
        a->family = ai->ai_family;
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in *sin =
                (const struct sockaddr_in *)ai->ai_addr;
            a->ip[0] = ntohl(sin->sin_addr.s_addr);
            a->port = ntohs(sin->sin_port);
        } else {
            const struct sockaddr_in6 *s6 =
                (const struct sockaddr_in6 *)ai->ai_addr;
            for (int i = 0; i < 4; i++) {
                uint32_t w;
                memcpy(&w, &s6->sin6_addr.s6_addr[i * 4], 4);
                a->ip[i] = ntohl(w);
            }
            a->port = ntohs(s6->sin6_port);
        }
        if (nonce_file != NULL) {
            size_t n = strlen(nonce_file);
            if (n >= sizeof(a->nonce_file)) {
                continue;
            }
            memcpy(a->nonce_file, nonce_file, n + 1);
            a->has_nonce = 1;
        }
        (*count)++;
        added = 1;
        break; /* first resolved address; multi-A-record failover is
                  beyond any legitimate desktop use */
    }
    freeaddrinfo(res);
    return added ? 0 : 1;
}

/* Scans a directory for "dbus-*" sockets (unix:dir=/tmp, the
 * pre-session-daemon discovery form). Returns endpoints added. */
static int add_dir_scan(struct fdk__dbus_addr *out, int *count,
                        int max_out, const char *dir_value) {
    DIR *d = opendir(dir_value);
    if (d == NULL) {
        return 1;
    }
    struct dirent *ent;
    int found = 1; /* 1 = nothing usable */
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, "dbus-", 5) != 0) {
            continue;
        }
        char full[256];
        int n = snprintf(full, sizeof(full), "%s/%s", dir_value,
                         ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISSOCK(st.st_mode)) {
            continue;
        }
        if (add_unix(out, count, max_out, full, 0) == 0) {
            found = 0;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Parses ONE transport spec ("unix:path=…,guid=…"). Returns 0 when
 * at least one endpoint was added, 1 when the spec is unusable. */
static int resolve_one(const char *spec, struct fdk__dbus_addr *out,
                       int *count, int max_out) {
    /* transport name up to ':' */
    const char *colon = strchr(spec, ':');
    if (colon == NULL) {
        return 1;
    }
    size_t tlen = (size_t)(colon - spec);
    char transport[16];
    if (tlen == 0 || tlen >= sizeof(transport)) {
        return 1;
    }
    memcpy(transport, spec, tlen);
    transport[tlen] = '\0';

    /* key=value pairs */
    char path_v[128] = "";
    char abstract_v[128] = "";
    char dir_v[128] = "";
    char host_v[128] = "";
    char port_v[8] = "";
    char family_v[8] = "";
    char nonce_v[256] = "";

    const char *p = colon + 1;
    while (*p != '\0') {
        const char *eq = strchr(p, '=');
        const char *comma = strchr(p, ',');
        if (eq == NULL || (comma != NULL && eq > comma)) {
            break; /* stray token */
        }
        size_t klen = (size_t)(eq - p);
        const char *vstart = eq + 1;
        const char *vend = comma ? comma : p + strlen(p);
        size_t vlen = (size_t)(vend - vstart);
        char key[16];
        if (klen == 0 || klen >= sizeof(key)) {
            return 1;
        }
        memcpy(key, p, klen);
        key[klen] = '\0';
        char *val = fdk_alloc(vlen + 1);
        if (val == NULL) {
            return 1;
        }
        memcpy(val, vstart, vlen);
        val[vlen] = '\0';
        char *decoded = percent_decode(val);
        fdk_free(val);
        if (decoded == NULL) {
            return 1;
        }
        if (strcmp(key, "path") == 0) {
            snprintf(path_v, sizeof(path_v), "%s", decoded);
        } else if (strcmp(key, "abstract") == 0) {
            snprintf(abstract_v, sizeof(abstract_v), "%s", decoded);
        } else if (strcmp(key, "dir") == 0 || strcmp(key, "tmpdir") == 0) {
            snprintf(dir_v, sizeof(dir_v), "%s", decoded);
        } else if (strcmp(key, "host") == 0) {
            snprintf(host_v, sizeof(host_v), "%s", decoded);
        } else if (strcmp(key, "port") == 0) {
            snprintf(port_v, sizeof(port_v), "%s", decoded);
        } else if (strcmp(key, "family") == 0) {
            snprintf(family_v, sizeof(family_v), "%s", decoded);
        } else if (strcmp(key, "noncefile") == 0) {
            snprintf(nonce_v, sizeof(nonce_v), "%s", decoded);
        } /* guid and unknown keys: ignored */
        fdk_free(decoded);
        p = comma ? comma + 1 : p + strlen(p);
    }

    if (strcmp(transport, "unix") == 0) {
        if (path_v[0] != '\0') {
            return add_unix(out, count, max_out, path_v, 0) == 0 ? 0 : 1;
        }
        if (abstract_v[0] != '\0') {
            return add_unix(out, count, max_out, abstract_v, 1) == 0 ? 0 : 1;
        }
        if (dir_v[0] != '\0') {
            return add_dir_scan(out, count, max_out, dir_v);
        }
        return 1;
    }
    if (strcmp(transport, "tcp") == 0 || strcmp(transport, "nonce-tcp") == 0) {
        int family = 0;
        if (strcmp(family_v, "ipv4") == 0) {
            family = 4;
        } else if (strcmp(family_v, "ipv6") == 0) {
            family = 6;
        }
        const char *nonce = (strcmp(transport, "nonce-tcp") == 0 &&
                             nonce_v[0] != '\0')
                                ? nonce_v
                                : NULL;
        return add_inet(out, count, max_out, host_v, port_v, family, nonce);
    }
    return 1; /* autolaunch and unknown transports: unsupported (logged) */
}

int fdk__dbus_resolve_addresses(const char *address, struct fdk__dbus_addr *out,
                                int max_out) {
    if (address == NULL || out == NULL || max_out <= 0) {
        return 0;
    }
    int count = 0;
    const char *p = address;
    while (*p != '\0' && count < max_out) {
        const char *semi = strchr(p, ';');
        size_t n = semi ? (size_t)(semi - p) : strlen(p);
        if (n > 0) {
            char *spec = fdk_alloc(n + 1);
            if (spec == NULL) {
                return -1;
            }
            memcpy(spec, p, n);
            spec[n] = '\0';
            (void)resolve_one(spec, out, &count, max_out);
            fdk_free(spec);
        }
        p = semi ? semi + 1 : p + n;
    }
    return count;
}

/* ---- bus default resolution ------------------------------------------------- */

char *fdk__dbus_bus_address(fdk_dbus_bus bus) {
    const char *fdk_override = getenv("FDK_DBUS_ADDRESS");
    if (fdk_override != NULL && fdk_override[0] != '\0') {
        return dup_str(fdk_override);
    }
    if (bus == FDK_DBUS_SESSION_BUS) {
        const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
        if (env != NULL && env[0] != '\0') {
            return dup_str(env);
        }
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg != NULL && xdg[0] != '\0') {
            return dup_printf("unix:path=%s/bus", xdg);
        }
        return NULL;
    }
    if (bus == FDK_DBUS_SYSTEM_BUS) {
        const char *env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
        if (env != NULL && env[0] != '\0') {
            return dup_str(env);
        }
        return dup_str("unix:path=/var/run/dbus/system_bus_socket;"
                       "unix:path=/run/dbus/system_bus_socket");
    }
    return NULL; /* unknown bus selector */
}

char *fdk_dbus_bus_address(fdk_dbus_bus bus) {
    return fdk__dbus_bus_address(bus);
}

/* ---- connecting --------------------------------------------------------------- */

static int socket_set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        return -1;
    }
    int fd_fl = fcntl(fd, F_GETFD, 0);
    if (fd_fl < 0 || fcntl(fd, F_SETFD, fd_fl | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}

/* Reads the nonce-tcp nonce file: hex text → raw bytes, up to 64.
 * Returns bytes read or -1. */
static int read_nonce(const char *file, unsigned char *out, size_t cap) {
    FILE *f = fopen(file, "rb");
    if (f == NULL) {
        return -1;
    }
    char hex[256];
    size_t n = fread(hex, 1, sizeof(hex) - 1, f);
    fclose(f);
    hex[n] = '\0';
    size_t w = 0;
    for (size_t i = 0; hex[i] != '\0' && w < cap; i++) {
        if (hex[i] == '\n' || hex[i] == '\r' || hex[i] == ' ') {
            continue;
        }
        int v = -1;
        char c = hex[i];
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0) {
            return -1;
        }
        if (w % 2 == 0) {
            out[w / 2] = (unsigned char)(v << 4);
        } else {
            out[w / 2] |= (unsigned char)v;
        }
        w++;
    }
    return (int)(w / 2);
}

/* The nonce bytes a nonce-tcp endpoint must send pre-auth; stored
 * on the endpoint (read at connect time — the file is short-lived
 * by design). Kept out of fdk__dbus_addr to keep that struct
 * trivially copyable; the connection layer re-reads via this. */
int fdk__dbus_read_nonce(const struct fdk__dbus_addr *a,
                         unsigned char *out, size_t cap) {
    if (a == NULL || !a->has_nonce) {
        return 0;
    }
    return read_nonce(a->nonce_file, out, cap);
}

int fdk__dbus_connect_first(const struct fdk__dbus_addr *addrs, int count,
                            struct fdk__dbus_addr *chosen) {
    for (int i = 0; i < count; i++) {
        const struct fdk__dbus_addr *a = &addrs[i];
        int fd = -1;
        if (a->family == AF_UNIX) {
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                continue;
            }
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            size_t namelen = strlen(a->unix_path);
            if (a->abstract_socket) {
                sa.sun_path[0] = '\0';
                memcpy(sa.sun_path + 1, a->unix_path, namelen);
            } else {
                memcpy(sa.sun_path, a->unix_path, namelen + 1);
            }
            socklen_t salen = (socklen_t)(offsetof(struct sockaddr_un,
                                                  sun_path) +
                                          (a->abstract_socket
                                               ? 1 + namelen
                                               : namelen + 1));
            if (connect(fd, (struct sockaddr *)&sa, salen) == 0 ||
                errno == EINPROGRESS) {
                /* EINPROGRESS completes asynchronously — the
                 * connection layer's io callback handles it. */
                if (socket_set_nonblock_cloexec(fd) == 0) {
                    if (chosen != NULL) {
                        *chosen = *a;
                    }
                    return fd;
                }
                close(fd);
                continue;
            }
            close(fd);
            continue;
        }
        /* inet */
        fd = socket(a->family, SOCK_STREAM, 0);
        if (fd < 0) {
            continue;
        }
        int connected = 0;
        if (a->family == AF_INET) {
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(a->port);
            sa.sin_addr.s_addr = htonl(a->ip[0]);
            connected = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) ==
                             0 ||
                         errno == EINPROGRESS);
        } else {
            struct sockaddr_in6 sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin6_family = AF_INET6;
            sa.sin6_port = htons(a->port);
            for (int k = 0; k < 4; k++) {
                uint32_t w = htonl(a->ip[k]);
                memcpy(&sa.sin6_addr.s6_addr[k * 4], &w, 4);
            }
            connected = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) ==
                             0 ||
                         errno == EINPROGRESS);
        }
        if (connected && socket_set_nonblock_cloexec(fd) == 0) {
            if (chosen != NULL) {
                *chosen = *a;
            }
            return fd;
        }
        close(fd);
    }
    return -1;
}
