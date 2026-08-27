/*
 * parse.c — the `.fdk` theme file parser
 *
 * Written to the rules in docs/security.md: strict grammar (unknown
 * anything is an error with a line number), every loop bounded by the
 * documented caps (1 MiB input, 1024-byte lines, 128-byte strings),
 * hand-rolled numeric parsing (no strtol/sscanf/locale surprises),
 * allocations through fdk_alloc freed on every error path, and no
 * partial results — a failed parse returns an error and the caller
 * destroys the whole working object.
 *
 * The grammar is specified in docs/fdk-theme-format.md. Anything not
 * in that document reaching this parser is a bug in one of the two.
 */

#define FDK_LOG_TAG "theme"

#include "theme_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ---- scanner ----
 *
 * Works on (buf, len) — never on NUL-terminated strings, so embedded
 * NULs cannot truncate a value (they are simply invalid characters and
 * fail the current construct with a line number). */

#define THEME_LINE_MAX 1024u

typedef struct {
    const char *buf;
    size_t len;
    size_t pos;
    int line; /* 1-based; incremented per yielded line */
} scanner;

/* Yields the next line segment (terminator excluded). Returns:
 *   1  = segment stored via out and out_len
 *   0  = end of input
 *  -1  = line exceeds THEME_LINE_MAX (cap violation) */
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
    if (n > THEME_LINE_MAX) {
        return -1;
    }
    *out = start;
    *out_len = n;
    s->line++;

    /* Consume the terminator: \n, \r\n, or a lone \r. */
    s->pos += n;
    if (s->pos < s->len) {
        if (s->buf[s->pos] == '\r') {
            s->pos++;
            if (s->pos < s->len && s->buf[s->pos] == '\n') {
                s->pos++;
            }
        } else {
            s->pos++; /* '\n' */
        }
    }
    return 1;
}

/* ---- small char classes (ASCII only, byte-wise) ---- */

static bool is_space(char c) {
    return c == ' ' || c == '\t';
}

static bool is_key_start(char c) {
    return (c >= 'a' && c <= 'z') || c == '_';
}

static bool is_key_char(char c) {
    return is_key_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* ---- diagnostics ---- */

static fdk_result fail(int line, fdk_result code, const char *what) {
    FDK_ERROR("theme:%d: %s (%s)", line, what,
              fdk_result_to_string(code));
    return code;
}

/* ---- value parsers ----
 *
 * Each consumes its value from (v, len) at *p, leaving *p just past
 * the value. The caller enforces "only whitespace to end of line"
 * afterwards, so a value parser never needs to look at what follows.
 */

static fdk_result parse_hex_color(const char *v, size_t len, size_t *p,
                                  fdk_color *out, int line) {
    if (*p >= len || v[*p] != '#') {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "color value must start with '#'");
    }
    (*p)++;

    unsigned digits[8];
    size_t n = 0;
    while (*p + n < len && n < 8 && hex_value(v[*p + n]) >= 0) {
        digits[n] = (unsigned)hex_value(v[*p + n]);
        n++;
    }
    if (*p + n < len && n == 8 && hex_value(v[*p + n]) >= 0) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "color has more than 8 hex digits");
    }
    if (n != 6 && n != 8) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "color must be #RRGGBB or #RRGGBBAA");
    }
    *p += n;

    unsigned rgb[3];
    for (size_t i = 0; i < 3; i++) {
        rgb[i] = digits[i * 2] * 16u + digits[i * 2 + 1];
    }
    unsigned a = 255u;
    if (n == 8) {
        a = digits[6] * 16u + digits[7];
    }
    *out = (fdk_color){
        (fdk_f32)rgb[0] / 255.0f,
        (fdk_f32)rgb[1] / 255.0f,
        (fdk_f32)rgb[2] / 255.0f,
        (fdk_f32)a / 255.0f,
    };
    return FDK_OK;
}

/* Non-negative or negative decimal integer, at most 10 digits, no
 * leading zeros. Returns the value in *out on FDK_OK. */
static fdk_result parse_integer(const char *v, size_t len, size_t *p,
                                long long *out, int line) {
    bool negative = false;
    if (*p < len && v[*p] == '-') {
        negative = true;
        (*p)++;
    }
    if (*p >= len || !is_digit(v[*p])) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "expected an integer");
    }
    size_t n = 0;
    long long value = 0;
    while (*p + n < len && n < 10 && is_digit(v[*p + n])) {
        value = value * 10 + (v[*p + n] - '0');
        n++;
    }
    if (*p + n < len && is_digit(v[*p + n])) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "integer has more than 10 digits");
    }
    if (n > 1 && v[*p] == '0') {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "integer has a leading zero");
    }
    *p += n;
    *out = negative ? -value : value;
    return FDK_OK;
}

/* Double-quoted string, at most FDK_THEME_STRING_MAX content bytes,
 * escapes \" and \\ only, closed on the same line, no control bytes.
 * Returns an fdk_alloc'd copy in *out (owned by the caller). */
static fdk_result parse_quoted_string(const char *v, size_t len, size_t *p,
                                      char **out, int line) {
    if (*p >= len || v[*p] != '"') {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "string value must start with '\"'");
    }
    (*p)++;

    char tmp[FDK_THEME_STRING_MAX + 1];
    size_t n = 0;
    bool closed = false;
    while (*p < len) {
        char c = v[*p];
        if (c == '"') {
            (*p)++;
            closed = true;
            break;
        }
        if (c == '\\') {
            if (*p + 1 >= len) {
                break; /* dangling backslash at line end */
            }
            char esc = v[*p + 1];
            if (esc != '"' && esc != '\\') {
                return fail(line, FDK_ERR_THEME_PARSE,
                            "only \\\" and \\\\ escapes exist");
            }
            if (n >= FDK_THEME_STRING_MAX) {
                return fail(line, FDK_ERR_THEME_PARSE,
                            "string longer than 128 bytes");
            }
            tmp[n++] = esc;
            *p += 2;
            continue;
        }
        if ((unsigned char)c < 0x20u) {
            return fail(line, FDK_ERR_THEME_PARSE,
                        "control character in string");
        }
        if (n >= FDK_THEME_STRING_MAX) {
            return fail(line, FDK_ERR_THEME_PARSE,
                        "string longer than 128 bytes");
        }
        tmp[n++] = c;
        (*p)++;
    }
    if (!closed) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "unterminated string");
    }
    tmp[n] = '\0';
    *out = fdk__theme_strdup(tmp);
    if (*out == NULL) {
        return fail(line, FDK_ERR_OUT_OF_MEMORY,
                    "cannot allocate string");
    }
    return FDK_OK;
}

/* ---- key tables ---- */

typedef struct {
    const char *key;
    int id;
} key_entry;

enum {
    TK_VERSION = 0,
    TK_NAME = 1,
    TK_AUTHOR = 2,
};

static const key_entry k_theme_keys[] = {
    {"version", TK_VERSION},
    {"name", TK_NAME},
    {"author", TK_AUTHOR},
};

static const key_entry k_color_keys[] = {
    {"window_background", FDK_TK_WINDOW_BACKGROUND},
    {"text", FDK_TK_TEXT},
    {"text_disabled", FDK_TK_TEXT_DISABLED},
    {"control_background", FDK_TK_CONTROL_BACKGROUND},
    {"control_background_hover", FDK_TK_CONTROL_BACKGROUND_HOVER},
    {"control_background_pressed", FDK_TK_CONTROL_BACKGROUND_PRESSED},
    {"control_background_disabled", FDK_TK_CONTROL_BACKGROUND_DISABLED},
    {"control_border", FDK_TK_CONTROL_BORDER},
    {"accent", FDK_TK_ACCENT},
    {"track", FDK_TK_TRACK},
};

static const key_entry k_metric_keys[] = {
    {"button_corner_radius", FDK_TM_BUTTON_CORNER_RADIUS},
    {"separator_thickness", FDK_TM_SEPARATOR_THICKNESS},
    {"title_bar_height", FDK_TM_TITLE_BAR_HEIGHT},
};

static const struct {
    fdk_i32 lo, hi;
} k_metric_ranges[FDK_TM_COUNT] = {
    [FDK_TM_BUTTON_CORNER_RADIUS] = {0, 32},
    [FDK_TM_SEPARATOR_THICKNESS] = {1, 8},
    [FDK_TM_TITLE_BAR_HEIGHT] = {12, 64},
};

/* Key-table lookup against a key segment (exact bytes). */
static const key_entry *lookup_key(const key_entry *table, size_t count,
                                   const char *seg, size_t seg_len) {
    for (size_t i = 0; i < count; i++) {
        if (strlen(table[i].key) == seg_len &&
            memcmp(table[i].key, seg, seg_len) == 0) {
            return &table[i];
        }
    }
    return NULL;
}

/* ---- section state ---- */

enum {
    SEC_THEME,
    SEC_COLORS,
    SEC_METRICS,
};

/* Entries before any section header belong to an IMPLICIT [theme]
 * section (the documented example starts with a bare `name = ...`
 * line) - so parsing starts already "in" [theme]. An explicit
 * [theme] header later is still fine (and still subject to the
 * duplicate-header rule).
 *
 * Section-HEADER dedup and KEY dedup are separate bitmasks on
 * purpose: sharing one field made the header's "section seen" bit
 * collide with the first key's bit (1 << 0) and reject the first
 * entry of every section as a duplicate - a bug the adversarial
 * test matrix caught the moment the implicit-section fix let it
 * run that far. */
typedef struct {
    fdk_theme *theme;
    int section;
    bool implicit_theme;   /* still before any section header */
    unsigned sections_seen; /* bit 0=[theme] 1=[colors] 2=[metrics] */
    unsigned seen_theme;   /* bit per k_theme_keys entry        */
    unsigned seen_colors;  /* bit per token                     */
    unsigned seen_metrics; /* bit per metric                    */
} parse_state;

/* ---- one line ---- */

static fdk_result handle_section(parse_state *st, const char *v,
                                 size_t len, size_t *p, int line) {
    (*p)++; /* consume '[' */
    while (*p < len && is_space(v[*p])) {
        (*p)++;
    }
    const char *ident = v + *p;
    size_t n = 0;
    while (*p + n < len && is_key_char(v[*p + n])) {
        n++;
    }
    if (n == 0) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "empty section name");
    }
    *p += n;
    while (*p < len && is_space(v[*p])) {
        (*p)++;
    }
    if (*p >= len || v[*p] != ']') {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "section header must end with ']'");
    }
    (*p)++;
    while (*p < len && is_space(v[*p])) {
        (*p)++;
    }
    if (*p != len) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "content after section header");
    }

    int section;
    unsigned header_bit;
    if (n == 5 && memcmp(ident, "theme", 5) == 0) {
        section = SEC_THEME;
        header_bit = 1u;
    } else if (n == 6 && memcmp(ident, "colors", 6) == 0) {
        section = SEC_COLORS;
        header_bit = 2u;
    } else if (n == 7 && memcmp(ident, "metrics", 7) == 0) {
        section = SEC_METRICS;
        header_bit = 4u;
    } else {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "unknown section (want theme/colors/metrics)");
    }
    if (st->sections_seen & header_bit) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "duplicate section");
    }
    st->sections_seen |= header_bit;
    st->section = section;
    st->implicit_theme = false;
    return FDK_OK;
}

static fdk_result handle_theme_entry(parse_state *st, const char *key,
                                     size_t key_len, const char *v,
                                     size_t len, size_t *p, int line) {
    const key_entry *e = lookup_key(
        k_theme_keys, sizeof k_theme_keys / sizeof k_theme_keys[0],
        key, key_len);
    if (e == NULL) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    st->implicit_theme
                        ? "unknown key before any section header "
                          "(only [theme] keys may appear there)"
                        : "unknown key in [theme]");
    }
    unsigned bit = 1u << e->id;
    if (st->seen_theme & bit) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "duplicate key in [theme]");
    }

    if (e->id == TK_VERSION) {
        long long value = 0;
        fdk_result r = parse_integer(v, len, p, &value, line);
        if (!fdk_ok(r)) {
            return r;
        }
        if (value != 1) {
            return fail(line, FDK_ERR_THEME_VERSION,
                        "unsupported format version (this build "
                        "understands version 1)");
        }
    } else {
        char *s = NULL;
        fdk_result r = parse_quoted_string(v, len, p, &s, line);
        if (!fdk_ok(r)) {
            return r;
        }
        if (e->id == TK_NAME) {
            fdk_free(st->theme->name);
            st->theme->name = s;
        } else {
            fdk_free(st->theme->author);
            st->theme->author = s;
        }
    }
    st->seen_theme |= bit;
    return FDK_OK;
}

static fdk_result handle_color_entry(parse_state *st, const char *key,
                                     size_t key_len, const char *v,
                                     size_t len, size_t *p, int line) {
    const key_entry *e = lookup_key(
        k_color_keys, sizeof k_color_keys / sizeof k_color_keys[0],
        key, key_len);
    if (e == NULL) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "unknown color token");
    }
    unsigned bit = 1u << e->id;
    if (st->seen_colors & bit) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "duplicate color token");
    }
    fdk_color c;
    fdk_result r = parse_hex_color(v, len, p, &c, line);
    if (!fdk_ok(r)) {
        return r;
    }
    st->theme->colors[e->id] = c;
    st->seen_colors |= bit;
    return FDK_OK;
}

static fdk_result handle_metric_entry(parse_state *st, const char *key,
                                      size_t key_len, const char *v,
                                      size_t len, size_t *p, int line) {
    const key_entry *e = lookup_key(
        k_metric_keys, sizeof k_metric_keys / sizeof k_metric_keys[0],
        key, key_len);
    if (e == NULL) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "unknown metric");
    }
    unsigned bit = 1u << e->id;
    if (st->seen_metrics & bit) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "duplicate metric");
    }
    long long value = 0;
    fdk_result r = parse_integer(v, len, p, &value, line);
    if (!fdk_ok(r)) {
        return r;
    }
    fdk_i32 lo = k_metric_ranges[e->id].lo;
    fdk_i32 hi = k_metric_ranges[e->id].hi;
    if (value < (long long)lo || value > (long long)hi) {
        FDK_ERROR("theme:%d: metric value %lld out of range %d..%d",
                  line, value, lo, hi);
        return FDK_ERR_THEME_PARSE;
    }
    st->theme->metrics[e->id] = (fdk_i32)value;
    st->seen_metrics |= bit;
    return FDK_OK;
}

static fdk_result handle_line(parse_state *st, const char *v, size_t len,
                              int line) {
    size_t p = 0;
    while (p < len && is_space(v[p])) {
        p++;
    }
    if (p == len || v[p] == '#') {
        return FDK_OK; /* blank or comment */
    }
    if (v[p] == '[') {
        return handle_section(st, v, len, &p, line);
    }

    /* Entry: key = value (st->section is never SEC_NONE here - see
     * the implicit [theme] note in parse_state). */
    const char *key = v + p;
    size_t key_len = 0;
    if (!is_key_start(v[p])) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "key must start with a-z or _");
    }
    while (p + key_len < len && is_key_char(v[p + key_len])) {
        key_len++;
    }
    p += key_len;
    while (p < len && is_space(v[p])) {
        p++;
    }
    if (p >= len || v[p] != '=') {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "expected '=' after key");
    }
    p++;
    while (p < len && is_space(v[p])) {
        p++;
    }

    fdk_result r;
    switch (st->section) {
    case SEC_THEME:
        r = handle_theme_entry(st, key, key_len, v, len, &p, line);
        break;
    case SEC_COLORS:
        r = handle_color_entry(st, key, key_len, v, len, &p, line);
        break;
    case SEC_METRICS:
        r = handle_metric_entry(st, key, key_len, v, len, &p, line);
        break;
    }
    if (!fdk_ok(r)) {
        return r;
    }

    /* Only trailing whitespace may follow a value. */
    while (p < len && is_space(v[p])) {
        p++;
    }
    if (p != len) {
        return fail(line, FDK_ERR_THEME_PARSE,
                    "trailing content after value");
    }
    return FDK_OK;
}

/* ---- entry points ---- */

fdk_result fdk__theme_parse_into(fdk_theme *t, const char *text,
                                 size_t length) {
    scanner sc = {.buf = text, .len = length, .pos = 0, .line = 0};

    /* Tolerate exactly one UTF-8 BOM at the very start. */
    if (sc.len >= 3 && (unsigned char)sc.buf[0] == 0xEFu &&
        (unsigned char)sc.buf[1] == 0xBBu &&
        (unsigned char)sc.buf[2] == 0xBFu) {
        sc.pos = 3;
    }

    parse_state st = {.theme = t, .section = SEC_THEME,
                    .implicit_theme = true, 0u, 0u, 0u, 0u};

    for (;;) {
        const char *line = NULL;
        size_t line_len = 0;
        int got = scanner_next(&sc, &line, &line_len);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            return fail(sc.line + 1, FDK_ERR_THEME_PARSE,
                        "line longer than 1024 bytes");
        }
        fdk_result r = handle_line(&st, line, line_len, sc.line);
        if (!fdk_ok(r)) {
            return r;
        }
    }
    return FDK_OK;
}

fdk_theme *fdk_theme_parse(const char *text, size_t length,
                           fdk_result *out_error) {
    if (out_error != NULL) {
        *out_error = FDK_OK;
    }
    if (text == NULL || length == 0) {
        if (out_error != NULL) {
            *out_error = FDK_ERR_INVALID_ARGUMENT;
        }
        FDK_ERROR("fdk_theme_parse: %s",
                  text == NULL ? "text is NULL" : "length is 0");
        return NULL;
    }
    if (length > FDK_THEME_INPUT_MAX) {
        if (out_error != NULL) {
            *out_error = FDK_ERR_INVALID_ARGUMENT;
        }
        FDK_ERROR("fdk_theme_parse: input exceeds the 1 MiB cap "
                  "(%zu bytes)",
                  length);
        return NULL;
    }

    fdk_theme *t = fdk_theme_create_default();
    if (t == NULL) {
        if (out_error != NULL) {
            *out_error = FDK_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }
    fdk_result r = fdk__theme_parse_into(t, text, length);
    if (!fdk_ok(r)) {
        fdk_theme_destroy(t);
        if (out_error != NULL) {
            *out_error = r;
        }
        return NULL;
    }
    return t;
}

fdk_theme *fdk_theme_load(const char *path, fdk_result *out_error) {
    if (out_error != NULL) {
        *out_error = FDK_OK;
    }
    if (path == NULL) {
        if (out_error != NULL) {
            *out_error = FDK_ERR_INVALID_ARGUMENT;
        }
        FDK_ERROR("fdk_theme_load: path is NULL");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (out_error != NULL) {
            *out_error = FDK_ERR_THEME_IO;
        }
        FDK_ERROR("fdk_theme_load: cannot open %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (out_error != NULL) {
            *out_error = FDK_ERR_THEME_IO;
        }
        FDK_ERROR("fdk_theme_load: cannot seek %s", path);
        return NULL;
    }
    long length = ftell(f);
    if (length < 0) {
        fclose(f);
        if (out_error != NULL) {
            *out_error = FDK_ERR_THEME_IO;
        }
        FDK_ERROR("fdk_theme_load: cannot size %s", path);
        return NULL;
    }
    if ((unsigned long)length > (unsigned long)FDK_THEME_INPUT_MAX) {
        fclose(f);
        if (out_error != NULL) {
            *out_error = FDK_ERR_THEME_IO;
        }
        FDK_ERROR("fdk_theme_load: %s exceeds the 1 MiB cap", path);
        return NULL;
    }
    rewind(f);

    size_t size = (size_t)length;
    if (size == 0) {
        /* A zero-byte file is not a theme — almost certainly a wrong
         * path or a failed download; reject rather than silently
         * equalling the defaults. (A file with only comments IS a
         * valid all-defaults theme — the grammar ran, it overrode
         * nothing.) */
        fclose(f);
        if (out_error != NULL) {
            *out_error = FDK_ERR_THEME_PARSE;
        }
        FDK_ERROR("fdk_theme_load: %s is empty (zero bytes)", path);
        return NULL;
    }
    char *data = NULL;
    {
        data = fdk_alloc(size);
        if (data == NULL) {
            fclose(f);
            if (out_error != NULL) {
                *out_error = FDK_ERR_OUT_OF_MEMORY;
            }
            FDK_ERROR("fdk_theme_load: out of memory reading %s", path);
            return NULL;
        }
        if (fread(data, 1, size, f) != size) {
            fdk_free(data);
            fclose(f);
            if (out_error != NULL) {
                *out_error = FDK_ERR_THEME_IO;
            }
            FDK_ERROR("fdk_theme_load: short read on %s", path);
            return NULL;
        }
    }
    fclose(f);

    fdk_theme *t = fdk_theme_parse(data, size, out_error);
    fdk_free(data);
    if (t == NULL) {
        FDK_ERROR("fdk_theme_load: %s rejected (%s)", path,
                  fdk_result_to_string(
                      out_error != NULL ? *out_error : FDK_ERR_UNKNOWN));
    }
    return t;
}
