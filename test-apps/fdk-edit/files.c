/* files.c — reading and writing documents: UTF-8 (with/without BOM),
 * UTF-16LE/BE, CRLF/LF normalization both ways, recent files, and
 * the session (the open tabs, the caret, the wrap/highlight prefs). */
#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- decode ------------------------------------------------------- */

bool ed_read_file(const char *path, char **out, size_t *out_len,
                  char *encoding, size_t enc_cap, bool *crlf,
                  bool *bom) {
    *out = NULL;
    *out_len = 0;
    *crlf = false;
    *bom = false;
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return false;
    }
    unsigned char *raw = malloc((size_t)sz + 1u);
    if (raw == NULL) {
        fclose(f);
        return false;
    }
    size_t got = fread(raw, 1u, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(raw);
        return false;
    }

    char *text = NULL;
    size_t text_len = 0;

    if (sz >= 3 && raw[0] == 0xEF && raw[1] == 0xBB &&
        raw[2] == 0xBF) {
        /* UTF-8 with BOM: strip it, keep the bytes              */
        *bom = true;
        text_len = (size_t)sz - 3u;
        text = malloc(text_len + 1u);
        if (text == NULL) {
            free(raw);
            return false;
        }
        memcpy(text, raw + 3, text_len);
        text[text_len] = '\0';
        ta_copy(encoding, enc_cap, "UTF-8 BOM");
    } else if (sz >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        /* UTF-16LE: decode the BMP subset (surrogates joined)   */
        size_t n = (size_t)sz - 2u;
        text = malloc(n + n / 2u + 2u); /* worst case: 3/2×       */
        if (text == NULL) {
            free(raw);
            return false;
        }
        const unsigned char *p = raw + 2;
        size_t w = 0;
        for (size_t i = 0; i + 1u < n; i += 2u) {
            unsigned cp = p[i] | ((unsigned)p[i + 1] << 8);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3u < n) {
                unsigned lo = p[i + 2] | ((unsigned)p[i + 3] << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u +
                         ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2u;
                }
            }
            if (cp < 0x80) {
                text[w++] = (char)cp;
            } else if (cp < 0x800) {
                text[w++] = (char)(0xC0 | (cp >> 6));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                text[w++] = (char)(0xE0 | (cp >> 12));
                text[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            } else {
                text[w++] = (char)(0xF0 | (cp >> 18));
                text[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                text[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        text[w] = '\0';
        text_len = w;
        *bom = false;
        ta_copy(encoding, enc_cap, "UTF-16LE");
    } else if (sz >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        /* UTF-16BE: the same walk, big-endian pairs             */
        size_t n = (size_t)sz - 2u;
        text = malloc(n + n / 2u + 2u);
        if (text == NULL) {
            free(raw);
            return false;
        }
        const unsigned char *p = raw + 2;
        size_t w = 0;
        for (size_t i = 0; i + 1u < n; i += 2u) {
            unsigned cp = ((unsigned)p[i] << 8) | p[i + 1];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 3u < n) {
                unsigned lo = ((unsigned)p[i + 2] << 8) | p[i + 3];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u +
                         ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2u;
                }
            }
            if (cp < 0x80) {
                text[w++] = (char)cp;
            } else if (cp < 0x800) {
                text[w++] = (char)(0xC0 | (cp >> 6));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                text[w++] = (char)(0xE0 | (cp >> 12));
                text[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            } else {
                text[w++] = (char)(0xF0 | (cp >> 18));
                text[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                text[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                text[w++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        text[w] = '\0';
        text_len = w;
        ta_copy(encoding, enc_cap, "UTF-16BE");
    } else {
        /* plain (or BOM-less): assume UTF-8 (the Unix default)  */
        text_len = (size_t)sz;
        text = malloc(text_len + 1u);
        if (text == NULL) {
            free(raw);
            return false;
        }
        memcpy(text, raw, text_len);
        text[text_len] = '\0';
        ta_copy(encoding, enc_cap, "UTF-8");
    }
    free(raw);

    /* CRLF detection (not normalization — the editor preserves the
     * file's endings; only the SAVE converts back) */
    for (size_t i = 0; i + 1u < text_len; i++) {
        if (text[i] == '\r' && text[i + 1] == '\n') {
            *crlf = true;
            break;
        }
    }
    *out = text;
    *out_len = text_len;
    return true;
}

bool ed_write_file(const char *path, const char *text, size_t len,
                   bool crlf, bool bom) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = true;
    if (bom) {
        static const unsigned char bom3[3] = {0xEF, 0xBB, 0xBF};
        ok = fwrite(bom3, 1, 3, f) == 3;
    }
    if (ok) {
        if (crlf) {
            /* write \r\n for every bare \n (an \r already followed
             * by \n is left alone — idempotent saves) */
            for (size_t i = 0; ok && i < len; i++) {
                if (text[i] == '\n' &&
                    (i == 0 || text[i - 1] != '\r')) {
                    ok = fputc('\r', f) != EOF;
                }
                if (ok) {
                    ok = fputc(text[i], f) != EOF;
                }
            }
        } else {
            ok = fwrite(text, 1, len, f) == len;
        }
    }
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* ---- recent files ------------------------------------------------- */

void ed_recent_add(const char *path) {
    if (path == NULL || path[0] == '\0') {
        return;
    }
    /* dedupe: bump to front */
    size_t at = eapp.recent_count;
    for (size_t i = 0; i < eapp.recent_count; i++) {
        if (strcmp(eapp.recent[i], path) == 0) {
            at = i;
            break;
        }
    }
    for (size_t i = at; i > 0; i--) {
        ta_copy(eapp.recent[i], sizeof eapp.recent[i],
                eapp.recent[i - 1]);
    }
    ta_copy(eapp.recent[0], sizeof eapp.recent[0], path);
    if (eapp.recent_count < 8) {
        eapp.recent_count++;
    }
}

/* ---- the session ---------------------------------------------------
 * A prefs-backed list: the open tabs (paths, carets), the current
 * tab, and the three booleans. Restored on boot. */

void ed_session_save(void) {
    fdk_prefs *p = eapp.base.prefs;
    if (p == NULL) {
        return;
    }
    (void)fdk_prefs_set_int(p, "session.tabs", (int)eapp.doc_count);
    (void)fdk_prefs_set_int(p, "session.current", (int)eapp.current);
    (void)fdk_prefs_set_bool(p, "session.wrap", eapp.wrap);
    (void)fdk_prefs_set_bool(p, "session.highlight", eapp.highlight);
    (void)fdk_prefs_set_bool(p, "session.gutter", eapp.show_gutter);
    (void)fdk_prefs_set_int(p, "session.recent",
                            (int)eapp.recent_count);
    for (size_t i = 0; i < eapp.recent_count; i++) {
        char key[32];
        snprintf(key, sizeof key, "recent.%zu", i);
        (void)fdk_prefs_set(p, key, eapp.recent[i]);
    }
    for (size_t i = 0; i < eapp.doc_count; i++) {
        char key[48];
        ed_doc *d = &eapp.docs[i];
        snprintf(key, sizeof key, "tab.%zu.path", i);
        (void)fdk_prefs_set(p, key, d->path);
        size_t caret = 0;
        if (d->view != NULL) {
            (void)fdk_textview_get_caret(d->view, &caret);
        }
        snprintf(key, sizeof key, "tab.%zu.caret", i);
        (void)fdk_prefs_set_int(p, key,
                                caret > 0x7FFFFFFFu ? 0x7FFFFFFFu
                                                    : (int)caret);
    }
}

void ed_session_load(void) {
    fdk_prefs *p = eapp.base.prefs;
    if (p == NULL) {
        return;
    }
    eapp.wrap = ta_pref_bool(&eapp.base, "session.wrap", true);
    eapp.highlight =
        ta_pref_bool(&eapp.base, "session.highlight", true);
    eapp.show_gutter =
        ta_pref_bool(&eapp.base, "session.gutter", true);
    int nrec = ta_pref_int(&eapp.base, "session.recent", 0);
    if (nrec < 0) {
        nrec = 0;
    }
    if (nrec > 8) {
        nrec = 8;
    }
    for (int i = 0; i < nrec; i++) {
        char key[32];
        snprintf(key, sizeof key, "recent.%d", i);
        ta_pref_str(&eapp.base, key, "", eapp.recent[i],
                    sizeof eapp.recent[i]);
    }
    eapp.recent_count = (size_t)nrec;
    /* the TABS open after the UI exists (main.c) */
}
