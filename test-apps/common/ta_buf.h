/* ta_buf.h — the ONE bounded-string helper the test-apps share.
 *
 * The §24 round lost most of its budget to GCC -Wformat-truncation
 * loops: every snprintf("%s/%s") of two PATH_MAX-sized sources into
 * a fixed buffer re-litigated the same warning. This header states
 * the bounds ONCE, in one place, with a truncation REPORT the
 * caller can surface (a status line "path too long" beats a silent
 * cut path handed to stat()).
 *
 * Rules of use (test-apps-wide):
 *  - NO direct snprintf of a path join; call ta_join / ta_join3.
 *  - NO direct "%s" copy of a path; call ta_copy.
 *  - Runtime checks use ta_ok(buf) once, after the batch.
 *  - Buffers stay FI_JOIN_MAX-class sizes (see ta_buf_cap).
 *
 * This is a header-only helper (static inline), like the common
 * shell's other pieces — not a framework.
 */
#ifndef TESTAPPS_TA_BUF_H
#define TESTAPPS_TA_BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* A thread-local "last op truncated" flag so a caller can check
 * one bit after a batch of joins instead of every return. */
#if defined(__GNUC__)
static __thread int ta_buf_truncated_;
#else
static int ta_buf_truncated_;
#endif

/* The canonical capacity for a path that will hold joins:
 * one full path + one name + a small suffix ("(3)", ".trashinfo"). */
#define TA_JOIN_MAX 4416

/* Copies `s` into `out` (cap bytes). Returns the length written.
 * Sets the truncation flag when it did not fit. PRECISION-LIMITED
 * so GCC's -Wformat-truncation can prove the bound. */
static inline size_t ta_copy(char *out, size_t cap, const char *s) {
    if (out == NULL || cap == 0) {
        ta_buf_truncated_ = 1;
        return 0;
    }
    if (s == NULL) {
        out[0] = '\0';
        return 0;
    }
    size_t n = strlen(s);
    if (n >= cap) {
        memcpy(out, s, cap - 1);
        out[cap - 1] = '\0';
        ta_buf_truncated_ = 1;
        return cap - 1;
    }
    memcpy(out, s, n + 1);
    return n;
}

/* Joins a + "/" + b into out. Truncation-flagged, never overrun. */
static inline size_t ta_join(char *out, size_t cap, const char *a,
                             const char *b) {
    if (out == NULL || cap == 0) {
        ta_buf_truncated_ = 1;
        return 0;
    }
    size_t n = (size_t)snprintf(out, cap, "%.4000s/%.200s",
                                a != NULL ? a : "", b != NULL ? b : "");
    if (n >= cap) {
        ta_buf_truncated_ = 1;
        return cap - 1;
    }
    return n;
}

/* Joins a + "/" + b + suffix (suffix first char is NOT a slash:
 * ".trashinfo", " (3)"...). */
static inline size_t ta_join_suf(char *out, size_t cap, const char *a,
                                 const char *b, const char *suffix) {
    if (out == NULL || cap == 0) {
        ta_buf_truncated_ = 1;
        return 0;
    }
    size_t n = (size_t)snprintf(out, cap, "%.3900s/%.200s%.16s",
                                a != NULL ? a : "", b != NULL ? b : "",
                                suffix != NULL ? suffix : "");
    if (n >= cap) {
        ta_buf_truncated_ = 1;
        return cap - 1;
    }
    return n;
}

/* A general bounded sprintf for NON-path strings (labels, status
 * text, error messages): same truncation accounting. */
static inline size_t ta_fmt(char *out, size_t cap, const char *fmt,
                            ...) __attribute__((format(printf, 3, 4)));
static inline size_t ta_fmt(char *out, size_t cap, const char *fmt,
                            ...) {
    if (out == NULL || cap == 0) {
        ta_buf_truncated_ = 1;
        return 0;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        ta_buf_truncated_ = 1;
        return cap - 1;
    }
    return (size_t)n;
}

/* Reads the truncation flag (and clears it). True = something was
 * cut since the last check; the caller owes the user a message. */
static inline bool ta_truncated(void) {
    bool t = ta_buf_truncated_ != 0;
    ta_buf_truncated_ = 0;
    return t;
}

#endif /* TESTAPPS_TA_BUF_H */
