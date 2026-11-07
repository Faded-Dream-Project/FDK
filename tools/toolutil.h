/*
 * toolutil.h — tiny shared helpers for the FDK command-line tools
 * (1.4.13)
 *
 * Static-inline only: both tools compile this header straight in and
 * nothing about the tools leaks back into the library (they are
 * ordinary applications on the public API, and are built exactly the
 * way the docs tell third parties to build against FDK — the tools
 * double as the reference consumer).
 *
 * The one thing both tools need that no library should ever grow:
 * create-the-config-directory. fdk_prefs_save() deliberately does
 * NOT mkdir anything (a settings library that silently creates
 * directory trees is a settings library with surprising side
 * effects); a USER-facing tool that fails on a missing ~/.config
 * would be a tool that fails on first run, which is worse. The rule
 * that falls out: policy lives here, mechanism stays in the library.
 */

#ifndef FDK_TOOLUTIL_H
#define FDK_TOOLUTIL_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Creates every component of `path` as a directory (mkdir -p).
 * Returns 0 on success (including "already exists"), -1 with errno
 * set on the first failure. Trailing slashes are tolerated; the
 * empty string is a no-op success. Mode 0777 & ~umask — the same
 * choice every XDG-respecting tool makes for user config/data dirs.
 * Components are chopped in place in `buf` and restored, so the
 * caller's buffer is left unchanged on success. */
static inline int tu_mkdir_p(char *buf) {
    if (buf == NULL) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(buf);
    if (len == 0) {
        return 0;
    }
    /* Drop trailing slashes (but keep a leading "/"). */
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char saved = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
                buf[i] = saved;
                return -1;
            }
            buf[i] = saved;
        }
    }
    return 0;
}

/* Strips the final component of `path` into `out` (which may alias
 * `path`). "a/b.fdk" -> "a"; "/x" -> "/"; "x" (no slash) -> ".".
 * `outsize` must hold the result; truncation is defensive only
 * (every path the tools feed this lives in a PATH_MAX-ish buffer). */
static inline void tu_dirname(char *out, size_t outsize,
                              const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        snprintf(out, outsize, ".");
        return;
    }
    if (slash == path) {
        snprintf(out, outsize, "/");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n >= outsize) {
        n = outsize - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

#endif /* FDK_TOOLUTIL_H */
