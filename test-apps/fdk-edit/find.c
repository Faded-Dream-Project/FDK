/* find.c — find/replace over the document: plain substring
 * (case-sensitive/insensitive) and POSIX extended regex (the libc
 * regex.h already in every libc — no new dependency). */
#include "edit.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

/* plain, case-insensitive: ASCII lower, the honest scope (the same
 * rule fdk-files' live filter uses) */
static const char *find_plain_ci(const char *hay, size_t hay_len,
                                 const char *needle, size_t from) {
    size_t nl = strlen(needle);
    if (nl == 0 || from >= hay_len || hay_len - from < nl) {
        return NULL;
    }
    for (size_t i = from; i + nl <= hay_len; i++) {
        size_t j = 0;
        while (j < nl) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            j++;
        }
        if (j == nl) {
            return hay + i;
        }
    }
    return NULL;
}

size_t ed_find_first(const char *haystack, const char *needle,
                     bool regex, bool case_sensitive, size_t *out_len) {
    const char *m = ed_find_next(haystack, needle, regex,
                                 case_sensitive, 0, out_len);
    (void)m;
    return (m != NULL) ? (size_t)(m - haystack) : (size_t)-1;
}

const char *ed_find_next(const char *haystack, const char *needle,
                         bool regex, bool case_sensitive, size_t from,
                         size_t *out_len) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return NULL;
    }
    size_t hay_len = strlen(haystack);
    if (from > hay_len) {
        return NULL;
    }
    if (regex) {
        regex_t re;
        int flags = REG_EXTENDED;
        if (!case_sensitive) {
            flags |= REG_ICASE;
        }
        if (regcomp(&re, needle, flags) != 0) {
            return NULL; /* a bad pattern: no match (the caller's
                            entry border says why) */
        }
        regmatch_t m[1];
        const char *result = NULL;
        if (regexec(&re, haystack + from, 1, m, 0) == 0) {
            result = haystack + from + (size_t)m[0].rm_so;
            if (out_len != NULL) {
                *out_len = (size_t)(m[0].rm_eo - m[0].rm_so);
            }
            /* an empty match advances by one so the caller's loop
             * terminates (the classic $-anchor case) */
            if (m[0].rm_so == m[0].rm_eo) {
                /* keep the empty match, but at the NEXT position */
                if (out_len != NULL) {
                    *out_len = 0;
                }
            }
        }
        regfree(&re);
        return result;
    }
    const char *r = NULL;
    if (case_sensitive) {
        r = strstr(haystack + from, needle);
    } else {
        r = find_plain_ci(haystack, hay_len, needle, from);
    }
    if (r != NULL && out_len != NULL) {
        *out_len = strlen(needle);
    }
    return r;
}

/* replace-all: builds the new document once, then sets it (the ONE
 * programmatic set_text — not undoable as a group; the per-edit undo
 * granularity of N replaces is GTK's choice too, but a single atomic
 * document set is the honest scope for replace-ALL; undo rolls the
 * whole replacement back in one step). */
bool ed_replace_all(ed_doc *d, const char *find, const char *repl,
                    bool regex, bool case_sensitive, size_t *count) {
    if (d == NULL || d->text == NULL || find == NULL ||
        find[0] == '\0' || repl == NULL) {
        return false;
    }
    size_t fl = strlen(find);
    size_t rl = strlen(repl);
    size_t n = 0;
    /* pass 1: count + total length */
    size_t new_len = 0;
    size_t pos = 0;
    for (;;) {
        size_t ml = 0;
        const char *m = ed_find_next(d->text, find, regex,
                                     case_sensitive, pos, &ml);
        if (m == NULL) {
            break;
        }
        size_t at = (size_t)(m - d->text);
        new_len += (at - pos) + rl;
        pos = at + (ml > 0 ? ml : 1);
        n++;
        if (pos >= d->text_len) {
            break;
        }
    }
    if (n == 0) {
        if (count != NULL) {
            *count = 0;
        }
        return true;
    }
    new_len += d->text_len - pos;
    char *out = malloc(new_len + 1u);
    if (out == NULL) {
        return false;
    }
    /* pass 2: build */
    size_t w = 0, rpos = 0;
    for (;;) {
        size_t ml = 0;
        const char *m = ed_find_next(d->text, find, regex,
                                     case_sensitive, rpos, &ml);
        if (m == NULL) {
            break;
        }
        size_t at = (size_t)(m - d->text);
        memcpy(out + w, d->text + rpos, at - rpos);
        w += at - rpos;
        memcpy(out + w, repl, rl);
        w += rl;
        rpos = at + (ml > 0 ? ml : 1);
        if (rpos >= d->text_len) {
            break;
        }
    }
    memcpy(out + w, d->text + rpos, d->text_len - rpos);
    w += d->text_len - rpos;
    out[w] = '\0';

    free(d->text);
    d->text = out;
    d->text_len = w;
    if (d->view != NULL) {
        fdk_textview_set_text(d->view, out);
        ed_highlight(d);
        ed_doc_mark_modified(d);
    }
    if (count != NULL) {
        *count = n;
    }
    (void)fl;
    return true;
}
