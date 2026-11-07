/*
 * dbus_auth.c — the SASL client handshake (1.1.0)
 *
 * EXTERNAL first (the uid the kernel already proved to the server
 * through SO_PEERCRED on the unix socket — no secret travels),
 * ANONYMOUS as a fallback exactly where the bus allows it. The
 * byte sequences are the specification's, verified against the
 * reference implementation's own socket writes (the wirepeek rig):
 *
 *     client:  \0 AUTH EXTERNAL 31303031\r\n     (hex of "1001")
 *     server:  OK <32 hex chars>\r\n
 *     client:  BEGIN\r\n
 *
 * and the fallback ladder:
 *     server:  REJECTED EXTERNAL ANONYMOUS\r\n   → try ANONYMOUS
 *     server:  ERROR <text>\r\n                  → fail (no retry
 *     storm — one attempt per mechanism, then the connection dies
 *     with a single log line)
 *
 * Unix-fd passing is deliberately NOT negotiated (v1 posture; see
 * docs/DECISIONS.md) — the field is parsed and ignored on receipt.
 *
 * The state machine is pure over server LINES so the unit tests can
 * drive every branch without a socket (tests/test_dbus.c).
 */

#define FDK_LOG_TAG "dbus"

#include "dbus/dbus_internal.h"

#include <stdio.h>
#include <string.h>

/* Auth states. */
enum {
    AUTH_FRESH = 0,     /* nothing sent yet                        */
    AUTH_AWAIT,         /* sent a mechanism, awaiting a verdict    */
    AUTH_DONE,          /* OK + BEGIN sent: message stream begins  */
    AUTH_FAILED,
};

char *fdk__dbus_auth_start(uint32_t uid, size_t *out_len) {
    if (out_len == NULL) {
        return NULL;
    }
    *out_len = 0;
    /* "\0AUTH EXTERNAL <hex(uid)>\r\n" */
    char uid_dec[32];
    int n = snprintf(uid_dec, sizeof(uid_dec), "%u", uid);
    if (n <= 0 || (size_t)n >= sizeof(uid_dec)) {
        return NULL;
    }
    size_t hex_n = (size_t)n * 2;
    size_t total = 1 + 4 + 1 + 8 + 1 + hex_n + 2; /* \0AUTH EXTERNAL hex \r\n */
    char *buf = fdk_alloc(total + 1);
    if (buf == NULL) {
        return NULL;
    }
    static const char hexd[] = "0123456789abcdef";
    size_t w = 0;
    buf[w++] = '\0';
    memcpy(buf + w, "AUTH EXTERNAL ", 14);
    w += 14;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)uid_dec[i];
        buf[w++] = hexd[c >> 4];
        buf[w++] = hexd[c & 0xF];
    }
    buf[w++] = '\r';
    buf[w++] = '\n';
    buf[w] = '\0';
    *out_len = w;
    return buf;
}

/* Case-insensitive substring containment, bounded. */
static int contains_mech(const char *line, const char *mech) {
    size_t mlen = strlen(mech);
    for (const char *p = line; *p != '\0'; p++) {
        size_t k = 0;
        while (k < mlen && p[k] != '\0') {
            if ((p[k] | 0x20) != (mech[k] | 0x20)) {
                break;
            }
            k++;
        }
        if (k == mlen) {
            /* must be delimited (start/space/end) to not match a
             * mechanism whose name merely prefixes another. */
            char before = (p == line) ? ' ' : p[-1];
            char after = p[mlen];
            if (before == ' ' && (after == '\0' || after == ' ')) {
                return 1;
            }
        }
    }
    return 0;
}

static char *auth_line_alloc(const char *cmd, size_t *out_len) {
    size_t n = strlen(cmd);
    char *buf = fdk_alloc(n + 1);
    if (buf == NULL) {
        return NULL;
    }
    memcpy(buf, cmd, n);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

char *fdk__dbus_auth_step(fdk__dbus_auth *a, const char *line,
                          size_t *out_len) {
    if (a == NULL || line == NULL || out_len == NULL) {
        return NULL;
    }
    *out_len = 0;
    if (a->done || a->failed) {
        return NULL;
    }

    /* Strip a trailing \r\n if the driver passed it. */
    size_t ll = strlen(line);
    while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r')) {
        ll--;
    }
    char clean[FDK_DBUS_AUTH_LINE_MAX];
    if (ll >= sizeof(clean)) {
        a->failed = 1; /* absurd line: protocol violation */
        return NULL;
    }
    memcpy(clean, line, ll);
    clean[ll] = '\0';

    if (strncmp(clean, "OK", 2) == 0 && (clean[2] == ' ' || clean[2] == '\0')) {
        /* OK <guid> */
        const char *guid = clean + 2;
        while (*guid == ' ') {
            guid++;
        }
        size_t gl = strlen(guid);
        if (gl > 32) {
            gl = 32;
        }
        memcpy(a->guid, guid, gl);
        a->guid[gl] = '\0';
        a->state = AUTH_DONE;
        a->done = 1;
        return auth_line_alloc("BEGIN\r\n", out_len);
    }
    if (strncmp(clean, "REJECTED", 8) == 0) {
        if (!a->tried_anonymous && contains_mech(clean, "ANONYMOUS")) {
            a->tried_anonymous = 1;
            return auth_line_alloc("AUTH ANONYMOUS\r\n", out_len);
        }
        a->failed = 1;
        return NULL;
    }
    if (strncmp(clean, "ERROR", 5) == 0) {
        if (!a->tried_anonymous) {
            /* A server that errored our EXTERNAL may still accept
             * ANONYMOUS; try once, then give up. */
            a->tried_anonymous = 1;
            return auth_line_alloc("AUTH ANONYMOUS\r\n", out_len);
        }
        a->failed = 1;
        return NULL;
    }
    if (strncmp(clean, "DATA", 4) == 0) {
        /* A challenge. Neither EXTERNAL (the creds came with the
         * socket) nor ANONYMOUS (no state to exchange) needs one in
         * any real deployment; answering is optional and failing is
         * safe — documented posture. */
        a->failed = 1;
        return NULL;
    }
    if (strncmp(clean, "AGREE_UNIX_FD", 13) == 0) {
        /* We never negotiate; receiving the agreement anyway is
         * harmless — ignore and keep awaiting OK. */
        return NULL;
    }
    /* Unknown command: protocol violation. */
    a->failed = 1;
    return NULL;
}
