#define FDK_LOG_TAG "i18n"

/*
 * catalog.c — translation message catalogs (.fmo format)
 *
 * The parser follows the same discipline as the .fdk theme parser
 * (docs/security.md): strict grammar where unknown anything is an
 * error carrying its line number, every loop bounded by documented
 * caps (1 MiB input, 1024-byte lines, 1024-byte strings, 8192
 * entries), hand-rolled scanning (no sscanf/strtol), explicit
 * UTF-8 validation, allocations through fdk_alloc freed on every
 * error path, and no partial results — a failed parse leaves
 * *out untouched.
 *
 * The format is specified in docs/fdk-catalog-format.md. Anything
 * not in that document reaching this parser is a bug in one of the
 * two.
 *
 * Lookup is binary search over entries sorted by (context, msgid)
 * at parse time — O(log n), no hashing, and the sort doubles as
 * the duplicate check (duplicates are adjacent after it; the
 * committed entry set is rejected wholesale).
 *
 * Entry state machine (line-granular):
 *   0 idle      expecting msgctxt or msgid
 *   1 ctxt      expecting msgid
 *   2 id        expecting msgstr or msgid_plural
 *   3 plural    expecting one or more msgstr[category]
 * A singular entry commits the moment its msgstr parses; a plural
 * entry commits when a line that is NOT another msgstr[category]
 * arrives (or input ends) and at least one form was seen.
 */

#include "i18n_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- bounds (documented in the header and the format spec) ---- */

#define FMO_INPUT_MAX (1024u * 1024u)
#define FMO_LINE_MAX 1024u
#define FMO_STRING_MAX 1024u
#define FMO_ENTRIES_MAX 8192u

typedef struct fdk_catalog_entry {
    char *ctx;    /* NULL = no context */
    char *id;     /* never NULL, never empty */
    char *str;    /* never NULL (may be "") */
    char *plural; /* NULL = singular-only entry */
    char *forms[6]; /* plural forms by fdk_plural_category; NULL =
                     * category absent */
} fdk_catalog_entry;

struct fdk_catalog {
    fdk_catalog_entry *entries; /* sorted by (ctx, id) */
    size_t count;
};

/* ---- diagnostics ---- */

static fdk_result fail(int line, fdk_result code, const char *what) {
    FDK_ERROR("catalog:%d: %s (%s)", line, what,
              fdk_result_to_string(code));
    return code;
}

/* ---- scanner (line-based, mirrors the theme parser) ---- */

typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
    int line;
} scanner;

static int scanner_next(scanner *s, const char **out, size_t *out_len) {
    if (s->pos >= s->len) {
        return 0;
    }
    const char *start = s->buf + s->pos;
    size_t n = 0;
    while (s->pos + n < s->len) {
        char c = s->buf[s->pos + n];
        if (c == '\n' || c == '\r') {
            break;
        }
        n++;
    }
    if (n > FMO_LINE_MAX) {
        return -1;
    }
    *out = start;
    *out_len = n;
    s->line++;
    s->pos += n;
    if (s->pos < s->len) {
        if (s->buf[s->pos] == '\r') {
            s->pos++;
            if (s->pos < s->len && s->buf[s->pos] == '\n') {
                s->pos++;
            }
        } else {
            s->pos++;
        }
    }
    return 1;
}

/* ---- UTF-8 validation ----
 *
 * Rejects overlong encodings, surrogates, > U+10FFFF, and raw
 * control characters (tab excepted) — the same strictness class
 * as the theme parser's text values. */

static bool utf8_valid(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        fdk_u8 c = (fdk_u8)s[i];
        if (c < 0x80) {
            /* Tab and newline are the only control chars that can be
             * here — a tab from the source, a newline only from a
             * decoded "\n" escape (raw newlines end lines before
             * this runs). Every other control byte is invalid. */
            if (c < 0x20 && c != '\t' && c != '\n') {
                return false;
            }
            i++;
            continue;
        }
        int extra;
        fdk_u32 cp;
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + (size_t)extra + 1 > n) {
            return false; /* truncated sequence */
        }
        for (int k = 1; k <= extra; k++) {
            fdk_u8 cc = (fdk_u8)s[i + (size_t)k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((extra == 1 && cp < 0x80) ||
            (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) ||
            cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += (size_t)extra + 1;
    }
    return true;
}

/* ---- string literal parsing ---- */

typedef struct {
    const char *line;
    size_t len;
    size_t pos;
    int line_no;
} line_cursor;

static fdk_result parse_string(line_cursor *c, char **out) {
    /* Skip the blanks between the keyword and the opening quote. */
    while (c->pos < c->len &&
           (c->line[c->pos] == ' ' || c->line[c->pos] == '\t')) {
        c->pos++;
    }
    if (c->pos >= c->len || c->line[c->pos] != '"') {
        return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                    "expected a quoted string");
    }
    c->pos++; /* opening quote */

    char *buf = fdk_alloc(FMO_STRING_MAX + 1);
    if (buf == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    size_t n = 0;

    while (c->pos < c->len) {
        char ch = c->line[c->pos];
        if (ch == '"') {
            c->pos++;
            if (!utf8_valid(buf, n)) {
                fdk_free(buf);
                return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                            "string is not valid UTF-8");
            }
            buf[n] = '\0';
            /* Shrink to the exact size (short strings dominate). */
            char *exact = fdk_alloc(n + 1);
            if (exact == NULL) {
                *out = buf; /* the working buffer is still valid */
                return FDK_OK;
            }
            memcpy(exact, buf, n + 1);
            fdk_free(buf);
            *out = exact;
            return FDK_OK;
        }
        if (ch == '\\') {
            c->pos++;
            if (c->pos >= c->len) {
                fdk_free(buf);
                return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                            "dangling escape at end of line");
            }
            char esc = c->line[c->pos];
            char decoded;
            switch (esc) {
            case '"': decoded = '"'; break;
            case '\\': decoded = '\\'; break;
            case 'n': decoded = '\n'; break;
            case 't': decoded = '\t'; break;
            default:
                fdk_free(buf);
                return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                            "unknown escape sequence");
            }
            if (n >= FMO_STRING_MAX) {
                fdk_free(buf);
                return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                            "string exceeds 1024 bytes");
            }
            buf[n++] = decoded;
            c->pos++;
            continue;
        }
        if (n >= FMO_STRING_MAX) {
            fdk_free(buf);
            return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                        "string exceeds 1024 bytes");
        }
        buf[n++] = ch;
        c->pos++;
    }
    fdk_free(buf);
    return fail(c->line_no, FDK_ERR_CATALOG_PARSE,
                "unterminated string");
}

/* ---- keyword parsing ----
 *
 * Scans one [a-z_]+ word (after any leading blanks — the caller
 * positions the cursor); returns:
 *   3 = msgctxt            (string expected next)
 *   4 = msgid              (string expected next)
 *   2 = msgid_plural       (string expected next)
 *   5 = msgstr             (string expected next)
 *   6 = msgstr[category]   (string expected next; *cat in 0..5)
 *  -1 = unknown keyword
 */
static int parse_keyword(line_cursor *c, int *cat) {
    *cat = -1;
    size_t start = c->pos;
    while (c->pos < c->len) {
        char ch = c->line[c->pos];
        if ((ch >= 'a' && ch <= 'z') || ch == '_') {
            c->pos++;
        } else {
            break;
        }
    }
    size_t n = c->pos - start;
    if (n == 0) {
        return -1;
    }
    if (n == 7 && memcmp(c->line + start, "msgctxt", 7) == 0) {
        return 3;
    }
    if (n == 5 && memcmp(c->line + start, "msgid", 5) == 0) {
        return 4;
    }
    if (n == 12 && memcmp(c->line + start, "msgid_plural", 12) == 0) {
        return 2;
    }
    if (n == 6 && memcmp(c->line + start, "msgstr", 6) == 0) {
        if (c->pos < c->len && c->line[c->pos] == '[') {
            c->pos++;
            size_t cat_start = c->pos;
            while (c->pos < c->len &&
                   c->line[c->pos] >= 'a' && c->line[c->pos] <= 'z') {
                c->pos++;
            }
            size_t cat_len = c->pos - cat_start;
            if (cat_len == 0 || c->pos >= c->len ||
                c->line[c->pos] != ']') {
                return -1;
            }
            c->pos++; /* ']' */
            static const char *const names[6] = {
                "zero", "one", "two", "few", "many", "other"};
            for (int k = 0; k < 6; k++) {
                if (cat_len == strlen(names[k]) &&
                    memcmp(c->line + cat_start, names[k], cat_len) ==
                        0) {
                    *cat = k;
                    return 6;
                }
            }
            return -1; /* unknown category name */
        }
        return 5; /* plain msgstr */
    }
    return -1;
}

/* True when the cursor is at end-of-line after blanks. */
static bool at_eol(line_cursor *c) {
    while (c->pos < c->len &&
           (c->line[c->pos] == ' ' || c->line[c->pos] == '\t')) {
        c->pos++;
    }
    return c->pos == c->len;
}

/* ---- entry assembly ---- */

typedef struct {
    char *ctx;
    char *id;
    char *plural;
    char *str;
    char *forms[6];
} entry_build;

static void entry_free_strings(entry_build *e) {
    fdk_free(e->ctx);
    fdk_free(e->id);
    fdk_free(e->plural);
    fdk_free(e->str);
    for (int k = 0; k < 6; k++) {
        fdk_free(e->forms[k]);
    }
    memset(e, 0, sizeof(*e));
}

/* Sort order: (ctx with NULL first, id) — shared by qsort and the
 * binary search. */
static int entry_cmp(const void *a, const void *b) {
    const fdk_catalog_entry *ea = a;
    const fdk_catalog_entry *eb = b;
    if (ea->ctx == NULL && eb->ctx != NULL) {
        return -1;
    }
    if (ea->ctx != NULL && eb->ctx == NULL) {
        return 1;
    }
    if (ea->ctx != NULL && eb->ctx != NULL) {
        int r = strcmp(ea->ctx, eb->ctx);
        if (r != 0) {
            return r;
        }
    }
    return strcmp(ea->id, eb->id);
}

static int entry_vs_key(const fdk_catalog_entry *e, const char *ctx,
                        const char *id) {
    if (e->ctx == NULL && ctx != NULL) {
        return -1;
    }
    if (e->ctx != NULL && ctx == NULL) {
        return 1;
    }
    if (e->ctx != NULL && ctx != NULL) {
        int r = strcmp(e->ctx, ctx);
        if (r != 0) {
            return r;
        }
    }
    return strcmp(e->id, id);
}

/* ---- the parser ---- */

fdk_result fdk_catalog_parse(const void *data, size_t size,
                             fdk_catalog **out) {
    if (out == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (data == NULL && size != 0) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (size > FMO_INPUT_MAX) {
        return fail(0, FDK_ERR_CATALOG_PARSE,
                    "input exceeds the 1 MiB cap");
    }

    fdk_catalog *cat = fdk_alloc(sizeof(fdk_catalog));
    if (cat == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    cat->entries = NULL;
    cat->count = 0;

    size_t cap = 16;
    if (size > 0) {
        cat->entries = fdk_alloc(cap * sizeof(*cat->entries));
        if (cat->entries == NULL) {
            fdk_free(cat);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }

    scanner sc = {(const char *)data, size, 0, 0};
    entry_build cur;
    memset(&cur, 0, sizeof(cur));
    int state = 0; /* 0 idle, 1 ctxt, 2 id, 3 plural */
    fdk_result r = FDK_OK;

    /* Commits cur into cat (growing it); cur is cleared either way.
     * `complete` was verified by the caller. */
#define COMMIT()                                                          \
    do {                                                                  \
        if (cat->count >= FMO_ENTRIES_MAX) {                              \
            entry_free_strings(&cur);                                     \
            fdk_catalog_destroy(cat);                                     \
            return fail(sc.line, FDK_ERR_CATALOG_PARSE,                   \
                        "catalog exceeds 8192 entries");                  \
        }                                                                 \
        if (cat->count >= cap) {                                          \
            size_t grown_cap = cap * 2;                                   \
            fdk_catalog_entry *grown = fdk_realloc(                       \
                cat->entries, grown_cap * sizeof(*cat->entries));         \
            if (grown == NULL) {                                          \
                entry_free_strings(&cur);                                 \
                fdk_catalog_destroy(cat);                                 \
                return FDK_ERR_OUT_OF_MEMORY;                             \
            }                                                             \
            cat->entries = grown;                                         \
            cap = grown_cap;                                              \
        }                                                                 \
        cat->entries[cat->count].ctx = cur.ctx;                           \
        cat->entries[cat->count].id = cur.id;                             \
        cat->entries[cat->count].str = cur.str;                           \
        cat->entries[cat->count].plural = cur.plural;                     \
        for (int k = 0; k < 6; k++) {                                     \
            cat->entries[cat->count].forms[k] = cur.forms[k];             \
        }                                                                 \
        cat->count++;                                                     \
        memset(&cur, 0, sizeof(cur));                                     \
        state = 0;                                                        \
    } while (0)

    const char *line;
    size_t line_len;
    int rc;
    while ((rc = scanner_next(&sc, &line, &line_len)) == 1) {
        /* Classify: blank / comment / content. */
        line_cursor c = {line, line_len, 0, sc.line};
        bool blank = at_eol(&c);
        bool comment = false;
        if (!blank) {
            char ch = line[c.pos];
            comment = (ch == '#') ||
                      (c.len - c.pos >= 2 && ch == '/' &&
                       line[c.pos + 1] == '/');
        }

        if (blank || comment) {
            if (state == 1 || state == 2) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "incomplete entry (missing msgstr)");
            }
            if (state == 3) {
                bool any = false;
                for (int k = 0; k < 6; k++) {
                    if (cur.forms[k] != NULL) {
                        any = true;
                    }
                }
                if (!any) {
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "plural entry has no msgstr[category]");
                }
                COMMIT();
            }
            continue;
        }

        /* Content line. */
        c.pos = 0;
        (void)at_eol(&c); /* skip leading blanks */
        int cat_idx = -1;
        int kw = parse_keyword(&c, &cat_idx);
        if (kw < 0) {
            entry_free_strings(&cur);
            fdk_catalog_destroy(cat);
            return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                        "unknown keyword");
        }

        if (kw == 3) { /* msgctxt */
            if (state != 0) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "msgctxt must start an entry");
            }
            char *ctx = NULL;
            r = parse_string(&c, &ctx);
            if (!fdk_ok(r)) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return r;
            }
            if (!at_eol(&c)) {
                fdk_free(ctx);
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "trailing text after msgctxt");
            }
            cur.ctx = ctx;
            state = 1;
            continue;
        }

        if (kw == 2) { /* msgid_plural */
            if (state != 2 || cur.plural != NULL) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "unexpected msgid_plural");
            }
            char *pl = NULL;
            r = parse_string(&c, &pl);
            if (!fdk_ok(r)) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return r;
            }
            if (!at_eol(&c)) {
                fdk_free(pl);
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "trailing text after msgid_plural");
            }
            cur.plural = pl;
            state = 3;
            continue;
        }

        if (kw == 4) { /* msgid */
            if (state == 3) {
                /* A new entry begins without a separator line:
                 * commit the plural one first. */
                bool any = false;
                for (int k = 0; k < 6; k++) {
                    if (cur.forms[k] != NULL) {
                        any = true;
                    }
                }
                if (!any) {
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "plural entry has no "
                                "msgstr[category]");
                }
                COMMIT();
            }
            if (state == 2) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "msgid without msgstr");
            }
            /* state 0 (fresh) or 1 (after msgctxt) */
            {
                char *id = NULL;
                r = parse_string(&c, &id);
                if (!fdk_ok(r)) {
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return r;
                }
                if (!at_eol(&c)) {
                    fdk_free(id);
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "trailing text after msgid");
                }
                if (id[0] == '\0') {
                    fdk_free(id);
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "empty msgid is not allowed");
                }
                cur.id = id;
                state = 2;
                continue;
            }
        }

        if (kw == 5 || kw == 6) { /* msgstr / msgstr[cat] */
            if (kw == 5) {
                /* Plain msgstr: singular entry completes now. */
                if (state != 2) {
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "unexpected msgstr (plural entries "
                                "use msgstr[category])");
                }
                char *s = NULL;
                r = parse_string(&c, &s);
                if (!fdk_ok(r)) {
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return r;
                }
                if (!at_eol(&c)) {
                    fdk_free(s);
                    entry_free_strings(&cur);
                    fdk_catalog_destroy(cat);
                    return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                                "trailing text after msgstr");
                }
                cur.str = s;
                COMMIT();
                continue;
            }
            /* msgstr[category]: plural form line. */
            if (state != 3) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "msgstr[category] needs msgid_plural "
                            "first");
            }
            if (cur.forms[cat_idx] != NULL) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "duplicate plural category");
            }
            char *s = NULL;
            r = parse_string(&c, &s);
            if (!fdk_ok(r)) {
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return r;
            }
            if (!at_eol(&c)) {
                fdk_free(s);
                entry_free_strings(&cur);
                fdk_catalog_destroy(cat);
                return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                            "trailing text after msgstr[...]");
            }
            cur.forms[cat_idx] = s;
            continue;
        }

        /* msgctxt (3) and msgid_plural (2) handled and continue'd
         * above; anything else is unreachable. */
        entry_free_strings(&cur);
        fdk_catalog_destroy(cat);
        return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                    "unexpected keyword inside entry");
    }
    if (rc == -1) {
        entry_free_strings(&cur);
        fdk_catalog_destroy(cat);
        return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                    "line exceeds 1024 bytes");
    }

    /* End of input: settle any pending entry. */
    if (state == 1 || state == 2) {
        entry_free_strings(&cur);
        fdk_catalog_destroy(cat);
        return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                    "truncated entry at end of input");
    }
    if (state == 3) {
        bool any = false;
        for (int k = 0; k < 6; k++) {
            if (cur.forms[k] != NULL) {
                any = true;
            }
        }
        if (!any) {
            entry_free_strings(&cur);
            fdk_catalog_destroy(cat);
            return fail(sc.line, FDK_ERR_CATALOG_PARSE,
                        "plural entry has no msgstr[category]");
        }
        COMMIT();
    }

    /* Sort by (ctx, id); reject duplicates (adjacent post-sort). */
    if (cat->count > 1) {
        qsort(cat->entries, cat->count, sizeof(*cat->entries),
              entry_cmp);
        for (size_t i = 1; i < cat->count; i++) {
            if (entry_cmp(&cat->entries[i - 1],
                          &cat->entries[i]) == 0) {
                fdk_catalog_destroy(cat);
                return fail(0, FDK_ERR_CATALOG_PARSE,
                            "duplicate msgid in the same context");
            }
        }
    }

    *out = cat;
    return FDK_OK;
#undef COMMIT
}

/* ---- load from file (the theme-load discipline) ---- */

fdk_result fdk_catalog_load(const char *path, fdk_catalog **out) {
    if (out == NULL || path == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        FDK_ERROR("fdk_catalog_load: cannot open %s", path);
        return FDK_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return FDK_ERR_IO;
    }
    long length = ftell(f);
    if (length < 0) {
        fclose(f);
        return FDK_ERR_IO;
    }
    if ((unsigned long)length > FMO_INPUT_MAX) {
        fclose(f);
        FDK_ERROR("fdk_catalog_load: %s exceeds the 1 MiB cap", path);
        return FDK_ERR_CATALOG_PARSE;
    }
    rewind(f);
    size_t size = (size_t)length;
    if (size == 0) {
        fclose(f);
        FDK_ERROR("fdk_catalog_load: %s is empty", path);
        return FDK_ERR_CATALOG_PARSE;
    }
    char *data = fdk_alloc(size);
    if (data == NULL) {
        fclose(f);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    if (fread(data, 1, size, f) != size) {
        fdk_free(data);
        fclose(f);
        return FDK_ERR_IO;
    }
    fclose(f);

    fdk_catalog *cat = NULL;
    fdk_result r = fdk_catalog_parse(data, size, &cat);
    fdk_free(data);
    if (!fdk_ok(r)) {
        FDK_ERROR("fdk_catalog_load: %s rejected (%s)", path,
                  fdk_result_to_string(r));
        return r;
    }
    *out = cat;
    return FDK_OK;
}

void fdk_catalog_destroy(fdk_catalog *catalog) {
    if (catalog == NULL) {
        return;
    }
    for (size_t i = 0; i < catalog->count; i++) {
        fdk_free(catalog->entries[i].ctx);
        fdk_free(catalog->entries[i].id);
        fdk_free(catalog->entries[i].str);
        fdk_free(catalog->entries[i].plural);
        for (int k = 0; k < 6; k++) {
            fdk_free(catalog->entries[i].forms[k]);
        }
    }
    fdk_free(catalog->entries);
    fdk_free(catalog);
}

/* ---- lookup ---- */

static const fdk_catalog_entry *find_entry(const fdk_catalog *catalog,
                                           const char *ctx,
                                           const char *id) {
    if (catalog == NULL || id == NULL) {
        return NULL;
    }
    size_t lo = 0;
    size_t hi = catalog->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const fdk_catalog_entry *e = &catalog->entries[mid];
        int cmp = entry_vs_key(e, ctx, id);
        if (cmp == 0) {
            return e;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

const char *fdk_catalog_get(const fdk_catalog *catalog,
                            const char *msgid) {
    const fdk_catalog_entry *e = find_entry(catalog, NULL, msgid);
    return (e != NULL) ? e->str : NULL;
}

const char *fdk_catalog_get_in_context(const fdk_catalog *catalog,
                                       const char *msgctxt,
                                       const char *msgid) {
    const fdk_catalog_entry *e = find_entry(catalog, msgctxt, msgid);
    return (e != NULL) ? e->str : NULL;
}

bool fdk_catalog_has(const fdk_catalog *catalog, const char *msgid) {
    return find_entry(catalog, NULL, msgid) != NULL;
}

size_t fdk_catalog_entry_count(const fdk_catalog *catalog) {
    return (catalog != NULL) ? catalog->count : 0;
}

const char *fdk_catalog_get_plural(const fdk_catalog *catalog,
                                   const fdk_locale *loc,
                                   const char *msgid, fdk_i64 n) {
    return fdk_catalog_get_plural_in_context(catalog, loc, NULL,
                                             msgid, n);
}

const char *fdk_catalog_get_plural_in_context(
    const fdk_catalog *catalog, const fdk_locale *loc,
    const char *msgctxt, const char *msgid, fdk_i64 n) {
    const fdk_catalog_entry *e = find_entry(catalog, msgctxt, msgid);
    if (e == NULL) {
        return NULL;
    }
    if (e->plural == NULL) {
        /* A singular entry queried through the plural API: return
         * its one stored form (the translator said the message does
         * not vary — honor it). */
        return e->str;
    }
    fdk_plural_category cat = fdk_plural_category_int(loc, n);
    if (e->forms[cat] != NULL) {
        return e->forms[cat];
    }
    if (e->forms[FDK_PLURAL_OTHER] != NULL) {
        return e->forms[FDK_PLURAL_OTHER];
    }
    /* Deterministic last resort: the first stored form. */
    for (int k = 0; k < 6; k++) {
        if (e->forms[k] != NULL) {
            return e->forms[k];
        }
    }
    return NULL; /* unreachable: commit requires >= 1 form */
}

const char *fdk_translate(const fdk_catalog *catalog,
                          const char *msgid) {
    const char *s = fdk_catalog_get(catalog, msgid);
    return (s != NULL) ? s : msgid;
}

const char *fdk_translate_plural(const fdk_catalog *catalog,
                                 const fdk_locale *loc,
                                 const char *msgid,
                                 const char *msgid_plural, fdk_i64 n) {
    const char *s = fdk_catalog_get_plural(catalog, loc, msgid, n);
    if (s != NULL) {
        return s;
    }
    /* Source-language fallback: English one/other on the SOURCE
     * strings (a missing catalog means English, never target-locale
     * plural guessing). */
    return (n == 1) ? msgid : msgid_plural;
}
