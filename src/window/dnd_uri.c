#define FDK_LOG_TAG "dnd"

/*
 * dnd_uri.c — the text/uri-list codec (1.2.0)
 *
 * One implementation of the file-drag wire format, shared by the X11
 * (XDND) and Wayland (wl_data_device) backends and unit-tested
 * headlessly (tests/test_dnd_logic.c). RFC 2483 shape: one URI per
 * line, CRLF separators tolerated on parse and produced on build,
 * '#' comment lines skipped, trailing partial lines still parsed.
 *
 * Decode policy (the public FDK contract, fdk_event.h's drag event):
 * file:// URIs become POSIX paths — the authority component is
 * dropped except an empty authority ("file:///path" — the canonical
 * local form) and a "localhost" authority; other hosts keep the full
 * URI verbatim (an app that knows what to do with smb:// can have
 * it). Percent-escapes are decoded on file URIs only.
 *
 * Encode policy: entries containing "://" pass through verbatim;
 * anything else is a path — realpath-normalized when resolvable
 * (falling back to the raw string for paths that vanished mid-drag),
 * made absolute against the CWD when relative, then escaped and
 * file://-prefixed.
 */

/* Defined BEFORE every include: realpath() hides behind
 * _DEFAULT_SOURCE on strict feature-test builds. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "window_internal.h"

#include "core/alloc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- decode ---- */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* In-place percent-decoding of [in, in+len) into out (cap bytes incl.
 * NUL). Returns the decoded length, or -1 when out is too small /
 * a malformed escape appears (kept verbatim on malformed, per the
 * containment policy — hostile input round-trips, it never crashes). */
static size_t percent_decode(const char *in, size_t len, char *out,
                             size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (o + 1 >= cap) {
            return (size_t)-1;
        }
        if (in[i] == '%' && i + 2 < len) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[o++] = in[i];
    }
    out[o] = '\0';
    return o;
}

/* file:// URI -> POSIX path, or NULL when the URI names another host
 * (kept verbatim by the caller instead). */
static char *path_from_file_uri(const char *uri, size_t len) {
    /* "file:" + optional "//authority" + "/"path. The local forms
     * are file:///path (empty authority) and file://localhost/path;
     * anything else is remote — hand the app the full URI. */
    const char *rest = uri + 5; /* after "file:" */
    size_t rest_len = len - 5;
    if (rest_len >= 2 && rest[0] == '/' && rest[1] == '/') {
        const char *auth = rest + 2;
        size_t auth_len = 0;
        while (auth_len < rest_len - 2 && auth[auth_len] != '/') {
            auth_len++;
        }
        bool local = (auth_len == 0) ||
                     (auth_len == 9 &&
                      strncmp(auth, "localhost", 9) == 0);
        if (!local) {
            return NULL;
        }
        rest = auth + auth_len; /* at the '/' that starts the path */
        rest_len -= 2 + auth_len;
    }
    /* rest now starts with '/' (the path) — or the URI is malformed
     * ("file:" alone); treat as empty. */
    char *out = fdk_alloc(rest_len + 1);
    if (out == NULL) {
        return NULL;
    }
    if (percent_decode(rest, rest_len, out, rest_len + 1) == (size_t)-1) {
        fdk_free(out);
        return NULL;
    }
    return out;
}

static bool is_file_uri(const char *s, size_t len) {
    return len >= 5 && strncmp(s, "file:", 5) == 0;
}

static bool entry_has_scheme(const char *s, size_t len) {
    /* scheme = ALPHA *( ALPHA / DIGIT / + / - / . ) followed by ':' */
    if (len == 0 || !((s[0] >= 'a' && s[0] <= 'z') ||
                      (s[0] >= 'A' && s[0] <= 'Z'))) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (c == ':') {
            return true;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return false;
}

fdk_result fdk__dnd_parse_uri_list(const char *payload, size_t len,
                                   char ***out_list, size_t *out_count) {
    *out_list = NULL;
    *out_count = 0;
    if (payload == NULL || len == 0) {
        return FDK_OK;
    }

    size_t cap = 8;
    char **list = fdk_alloc_array(cap, sizeof(char *));
    if (list == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;

    size_t i = 0;
    while (i < len) {
        /* Find the end of this line (LF or CRLF). */
        size_t start = i;
        while (i < len && payload[i] != '\n' && payload[i] != '\r') {
            i++;
        }
        size_t line_len = i - start;
        /* Skip the separators (any run of CR/LF). */
        while (i < len && (payload[i] == '\n' || payload[i] == '\r')) {
            i++;
        }
        if (line_len == 0 || payload[start] == '#') {
            continue; /* blank / comment line */
        }
        const char *line = payload + start;
        char *entry;
        if (is_file_uri(line, line_len)) {
            entry = path_from_file_uri(line, line_len);
            if (entry == NULL) {
                /* remote file URI: keep verbatim */
                entry = fdk_alloc(line_len + 1);
                if (entry != NULL) {
                    memcpy(entry, line, line_len);
                    entry[line_len] = '\0';
                }
            }
        } else {
            entry = fdk_alloc(line_len + 1);
            if (entry != NULL) {
                memcpy(entry, line, line_len);
                entry[line_len] = '\0';
            }
        }
        if (entry == NULL) {
            fdk__dnd_free_uri_list(list, n);
            return FDK_ERR_OUT_OF_MEMORY;
        }
        if (n == cap) {
            char **grown = fdk_realloc(list, cap * 2 * sizeof(char *));
            if (grown == NULL) {
                fdk_free(entry);
                fdk__dnd_free_uri_list(list, n);
                return FDK_ERR_OUT_OF_MEMORY;
            }
            list = grown;
            cap *= 2;
        }
        list[n++] = entry;
    }

    if (n == 0) {
        fdk_free(list);
        return FDK_OK;
    }
    *out_list = list;
    *out_count = n;
    return FDK_OK;
}

void fdk__dnd_free_uri_list(char **list, size_t count) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_free(list[i]);
    }
    fdk_free(list);
}

/* ---- encode ---- */

static bool uri_needs_escape(unsigned char c) {
    /* Unreserved + path-safe set; everything else percent-encodes. */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return false;
    }
    return !(c == '-' || c == '_' || c == '.' || c == '~' || c == '/' ||
             c == '!' || c == '$' || c == '&' || c == '\'' ||
             c == '(' || c == ')' || c == '*' || c == '+' || c == ',' ||
             c == ';' || c == '=' || c == ':' || c == '@');
}

/* Escapes `path` into a file:// URI appended to `out` (grown by
 * exact-need realloc). Buffer discipline: (out, cap, len) in/out
 * params — everything through the pointers, `(*len)++` (the bare
 * `*len++` increments the POINTER, an ASan-caught bug this comment
 * exists to keep fixed). */
static bool append_escaped_uri(const char *path, char **out,
                               size_t *cap, size_t *len) {
    static const char hex[] = "0123456789ABCDEF";
    const char *prefix = "file://";
    size_t plen = strlen(prefix);
    size_t need = *len + plen;
    for (const char *p = path; *p != '\0'; p++) {
        need += uri_needs_escape((unsigned char)*p) ? 3 : 1;
    }
    need += 1; /* NUL margin */
    if (need > *cap) {
        char *grown = fdk_realloc(*out, need);
        if (grown == NULL) {
            return false;
        }
        *out = grown;
        *cap = need;
    }
    memcpy(*out + *len, prefix, plen);
    *len += plen;
    for (const char *p = path; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (uri_needs_escape(c)) {
            (*out)[(*len)++] = '%';
            (*out)[(*len)++] = hex[c >> 4];
            (*out)[(*len)++] = hex[c & 0x0F];
        } else {
            (*out)[(*len)++] = (char)c;
        }
    }
    return true;
}

char *fdk__dnd_build_uri_list(const char *const *uris, size_t count) {
    if (uris == NULL || count == 0) {
        return NULL;
    }
    size_t cap = 256;
    size_t len = 0;
    char *out = fdk_alloc(cap);
    if (out == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        const char *u = uris[i] != NULL ? uris[i] : "";
        size_t ulen = strlen(u);
        if (len > 0) {
            if (len + 3 > cap) {
                char *grown = fdk_realloc(out, cap * 2);
                if (grown == NULL) {
                    fdk_free(out);
                    return NULL;
                }
                out = grown;
                cap *= 2;
            }
            out[len++] = '\r';
            out[len++] = '\n';
        }
        if (entry_has_scheme(u, ulen)) {
            /* Already a URI: pass through verbatim. */
            while (len + ulen + 1 > cap) {
                char *grown = fdk_realloc(out, cap * 2);
                if (grown == NULL) {
                    fdk_free(out);
                    return NULL;
                }
                out = grown;
                cap *= 2;
            }
            memcpy(out + len, u, ulen);
            len += ulen;
        } else {
            /* A path: normalize + absolutize, then encode. */
            char resolved[4096];
            const char *use = u;
            if (u[0] != '\0' && realpath(u, resolved) != NULL) {
                use = resolved;
            }
            if (!append_escaped_uri(use, &out, &cap, &len)) {
                fdk_free(out);
                return NULL;
            }
        }
    }
    out[len] = '\0';
    return out;
}
