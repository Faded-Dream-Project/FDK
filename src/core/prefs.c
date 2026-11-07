#define FDK_LOG_TAG "prefs"

/*
 * prefs.c — application preferences (1.3.7)
 *
 * A tiny strict key-value store: one ordered array of
 * (section, name, value) rows, an ordered section list for layout,
 * a parser for the file dialect, and an atomic writer. No hash
 * table — settings files are tens of keys, and linear lookup at
 * that size is faster than a hash's cache behavior (and infinitely
 * easier to keep correct).
 *
 * The resilience rule (see fdk_prefs.h's header comment for why
 * prefs fail soft where themes fail loud): the parser validates
 * EVERYTHING and collects the first error with its line number —
 * and open() turns a rejected file into an empty store with one
 * warning, not into a failure. The strictness is not weakened:
 * the same grammar that would reject the file also decides that
 * it cannot be trusted, and an untrusted settings file yields
 * defaults rather than half-parsed values.
 */

#include "fdk/fdk_prefs.h"

#include "fdk/fdk_error.h"
#include "fdk/fdk_log.h"

#include "prefs_internal.h"
#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- bounds (the family constants; see docs/fdk-prefs-format.md) ---- */

#define PREFS_MAX_FILE (1024u * 1024u) /* 1 MiB */
#define PREFS_MAX_LINE 1024
#define PREFS_MAX_KEYS 8192
#define PREFS_MAX_VALUE 256
#define PREFS_MAX_PART 64 /* section / name length */

typedef struct {
    char *key;   /* owned, "section.name" */
    char *value; /* owned, never NULL (empty allowed) */
    char *file_value; /* owned; the exact bytes the file carried,
                       * kept so a save() of an untouched store can
                       * skip the disk (the dirty flag is exact, not
                       * approximate: hand-formatting survives). */
    int bad_warned; /* set once a typed getter has logged this row's
                     * unparsable value — the header's "one DEBUG
                     * line on first read" contract, not one per call
                     * (a getter on a render-loop path must not
                     * spam). Log bookkeeping, not data. */
} pref_row;

struct fdk_prefs {
    pref_row *rows;
    size_t count;
    size_t cap;
    char *path;  /* owned; NULL for a memory-only store */
    char *app_id; /* owned; for the generated header comment */
    fdk_prefs_origin source;
    /* The exact dirty flag (the header's contract: save() on an
     * unchanged store touches no disk). NOT monotonic — it is
     * recomputed at every mutation as "any row whose value differs
     * from its file_value, or any row not in the file (added), or
     * a removal happened since the last save". `removed` is the
     * sticky half: a removed row is GONE, so no row-walk can see
     * it — only save() clears the tombstone. Together they make
     * "set away, set back, save" leave no disk trace, exactly as
     * documented. */
    int dirty;
    int removed; /* a remove() happened since the last save */
};

/* Recomputes dirty from the rows + the removal tombstone. O(n);
 * settings files are tens of keys (the 8192-bound test's O(n^2)
 * set pattern is the deliberate worst case, still sub-second). */
static int store_recompute(fdk_prefs *p) {
    if (p->removed) {
        return 1;
    }
    for (size_t i = 0; i < p->count; i++) {
        if (p->rows[i].file_value == NULL ||
            strcmp(p->rows[i].value, p->rows[i].file_value) != 0) {
            return 1;
        }
    }
    return 0;
}

/* Core-layer strdup (fdk__strdup lives in the widget layer; core
 * does not reach up). */
static char *prefs_strdup(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *out = fdk_alloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

/* ---- key grammar ------------------------------------------------------ */

/* A part: 1..64 chars of [A-Za-z0-9_-]. Returns the length, or 0
 * when the part is empty, too long, or holds a foreign character. */
static size_t part_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        char c = s[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return 0;
        }
        n++;
        if (n > PREFS_MAX_PART) {
            return 0;
        }
    }
    return (n >= 1) ? n : 0;
}

/* "section.name" — validates and returns true/false. */
static bool key_valid(const char *key) {
    if (key == NULL) {
        return false;
    }
    const char *dot = strchr(key, '.');
    if (dot == NULL || strchr(dot + 1, '.') != NULL) {
        return false; /* exactly one dot, not zero, not two */
    }
    size_t sec = (size_t)(dot - key);
    if (sec < 1 || sec > PREFS_MAX_PART) {
        return false;
    }
    /* Full validation of both halves by walking the whole key. */
    size_t n = 0;
    while (key[n] != '\0') {
        char c = key[n];
        if (c == '.') {
            if (n != sec) {
                return false;
            }
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        n++;
    }
    size_t name = n - sec - 1;
    if (name < 1 || name > PREFS_MAX_PART) {
        return false;
    }
    return true;
}

/* ---- row storage ------------------------------------------------------ */

static pref_row *find_row(fdk_prefs *p, const char *key) {
    for (size_t i = 0; i < p->count; i++) {
        if (strcmp(p->rows[i].key, key) == 0) {
            return &p->rows[i];
        }
    }
    return NULL;
}

static const pref_row *find_row_const(const fdk_prefs *p,
                                      const char *key) {
    for (size_t i = 0; i < p->count; i++) {
        if (strcmp(p->rows[i].key, key) == 0) {
            return &p->rows[i];
        }
    }
    return NULL;
}

static fdk_result row_grow(fdk_prefs *p) {
    if (p->count == p->cap) {
        if (p->count >= PREFS_MAX_KEYS) {
            return FDK_ERR_LIMIT;
        }
        size_t ncap = (p->cap == 0) ? 16 : p->cap * 2;
        if (ncap > PREFS_MAX_KEYS) {
            ncap = PREFS_MAX_KEYS;
        }
        pref_row *grown = fdk_realloc(p->rows, ncap * sizeof(pref_row));
        if (grown == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        p->rows = grown;
        p->cap = ncap;
    }
    return FDK_OK;
}

/* Inserts (append order) or replaces; takes ownership of neither
 * argument (copies both). The store's dirty flag is RECOMPUTED on
 * the way out: a replace that lands back on the file's own value
 * leaves the store exactly as clean as it was (see the struct
 * comment — the flag is exact, not monotonic). */
static fdk_result row_put(fdk_prefs *p, const char *key,
                          const char *value, const char *file_value) {
    pref_row *existing = find_row(p, key);
    if (existing != NULL) {
        char *copy = prefs_strdup(value);
        if (copy == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_free(existing->value);
        existing->value = copy;
        p->dirty = store_recompute(p);
        return FDK_OK;
    }
    fdk_result r = row_grow(p);
    if (!fdk_ok(r)) {
        return r;
    }
    pref_row *row = &p->rows[p->count];
    row->key = prefs_strdup(key);
    row->value = prefs_strdup(value);
    row->file_value = (file_value != NULL) ? prefs_strdup(file_value)
                                           : NULL;
    row->bad_warned = 0; /* realloc'd memory is untyped — reset */
    if (row->key == NULL || row->value == NULL ||
        (file_value != NULL && row->file_value == NULL)) {
        fdk_free(row->key);
        fdk_free(row->value);
        fdk_free(row->file_value);
        row->key = NULL;
        row->value = NULL;
        row->file_value = NULL;
        return FDK_ERR_OUT_OF_MEMORY;
    }
    p->count++;
    p->dirty = store_recompute(p);
    return FDK_OK;
}

/* ---- the parser -------------------------------------------------------
 *
 * Line-based like the theme/catalog grammars. The walk keeps the
 * current section (a file MAY interleave sections; rows land in
 * file order and keep their full "section.name" keys, so iteration
 * and save() reproduce the file's own organization). Every failure
 * path names the 1-based line number — the warning a rejected file
 * produces must be actionable. */

typedef struct {
    const char *data;
    size_t size;
    size_t pos;
    int line;
} pref_scan;

/* Reads one line (without its terminator) into a caller buffer;
 * returns false at EOF. Handles LF and CRLF; a lone CR inside a
 * line is a foreign character (strict: rejected by the value
 * grammar, not silently swallowed). */
static bool scan_line(pref_scan *sc, char *out, size_t out_cap) {
    if (sc->pos >= sc->size) {
        return false;
    }
    size_t w = 0;
    while (sc->pos < sc->size && sc->data[sc->pos] != '\n') {
        if (w + 1 >= out_cap) {
            return false; /* over-long line: the caller errors */
        }
        out[w++] = sc->data[sc->pos++];
    }
    if (sc->pos < sc->size) {
        sc->pos++; /* the newline */
    }
    sc->line++;
    if (w > 0 && out[w - 1] == '\r') {
        w--; /* CRLF */
    }
    out[w] = '\0';
    return true;
}

/* Parses the whole file into the store. Returns the 1-based line of
 * the first problem, or 0 on success. Errors are counted, not
 * accumulated — the first one wins, matching the family's
 * "reject first" posture. */
static int prefs_parse(fdk_prefs *p, const char *data, size_t size) {
    pref_scan sc = {data, size, 0, 0};
    char line[PREFS_MAX_LINE + 1];
    char section[PREFS_MAX_PART + 1] = "";
    char *trim = NULL;

    while (scan_line(&sc, line, sizeof line)) {
        /* Blank / comment lines. */
        trim = line;
        while (*trim == ' ' || *trim == '\t') {
            trim++;
        }
        if (trim[0] == '\0' || trim[0] == '#' ||
            (trim[0] == '/' && trim[1] == '/')) {
            continue;
        }

        /* Section header: [name] — nothing else on the line. */
        if (trim[0] == '[') {
            char *close = strchr(trim, ']');
            if (close == NULL || close[1] != '\0') {
                return sc.line;
            }
            size_t n = (size_t)(close - trim - 1);
            if (n < 1 || n > PREFS_MAX_PART) {
                return sc.line;
            }
            memcpy(section, trim + 1, n);
            section[n] = '\0';
            if (part_len(section) == 0) {
                return sc.line;
            }
            continue;
        }

        /* Key line: name = value. Whitespace on BOTH sides of the
         * '=' is ignored (a name is [A-Za-z0-9_-], so any run of
         * spaces/tabs before the '=' can only be separator —
         * `width = 880`, `width= 880`, and `width  =880` are the
         * same line; the writer's canonical form is `name = value`).
         * The value may be empty and may contain leading/trailing
         * spaces (trimmed like every sibling format trims); it may
         * not contain control characters (strict UTF-8 discipline:
         * bytes >= 0x20, plus a well-formedness walk). */
        char *eq = strchr(trim, '=');
        if (eq == NULL) {
            return sc.line;
        }
        char *name_end = eq;
        while (name_end > trim &&
               (name_end[-1] == ' ' || name_end[-1] == '\t')) {
            name_end--;
        }
        size_t name_len = (size_t)(name_end - trim);
        if (name_len < 1 || name_len > PREFS_MAX_PART) {
            return sc.line;
        }
        char name[PREFS_MAX_PART + 1];
        memcpy(name, trim, name_len);
        name[name_len] = '\0';
        if (part_len(name) == 0) {
            return sc.line;
        }
        char *value = eq + 1;
        /* Trim spaces/tabs around the value (the theme format's
         * rule, kept identical here). */
        char *vend = value + strlen(value);
        while (vend > value &&
               (vend[-1] == ' ' || vend[-1] == '\t')) {
            vend--;
        }
        *vend = '\0';
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        if (strlen(value) > PREFS_MAX_VALUE) {
            return sc.line;
        }
        for (const char *v = value; *v != '\0'; v++) {
            if ((unsigned char)*v < 0x20u) {
                return sc.line; /* control characters: rejected */
            }
        }
        /* UTF-8 well-formedness (the catalog's walk: reject
         * truncated/overlong sequences rather than serving mojibake
         * back on save). */
        for (const char *v = value; *v != '\0';) {
            unsigned char c0 = (unsigned char)*v;
            size_t len = 1;
            if ((c0 & 0xE0u) == 0xC0u) {
                len = 2;
            } else if ((c0 & 0xF0u) == 0xE0u) {
                len = 3;
            } else if ((c0 & 0xF8u) == 0xF0u) {
                len = 4;
            }
            for (size_t k = 1; k < len; k++) {
                if (((unsigned char)v[k] & 0xC0u) != 0x80u) {
                    return sc.line;
                }
            }
            v += len;
        }

        if (section[0] == '\0') {
            return sc.line; /* key before any section */
        }

        char full[2 * PREFS_MAX_PART + 2];
        (void)snprintf(full, sizeof full, "%s.%s", section, name);
        if (find_row_const(p, full) != NULL) {
            return sc.line; /* duplicate key: the family's hard rule */
        }
        fdk_result r = row_put(p, full, value, value);
        if (!fdk_ok(r)) {
            return sc.line; /* limit/OOM: refuse the file wholesale */
        }
    }
    /* Over-long lines: scan_line returned false before EOF. */
    if (sc.pos < sc.size) {
        return sc.line + 1;
    }
    return 0;
}

/* ---- path resolution -------------------------------------------------- */

/* $FDK_PREFS_FILE, else $XDG_CONFIG_HOME/<app>.prefs, else
 * $HOME/.config/<app>.prefs, else NULL (memory-only). The result
 * is owned (fdk_alloc'd). */
char *fdk__prefs_resolve_path(const char *app_id) {
    const char *override = getenv("FDK_PREFS_FILE");
    if (override != NULL && override[0] == '/') {
        return prefs_strdup(override);
    }
    char buf[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != NULL && xdg[0] == '/') {
        if (snprintf(buf, sizeof buf, "%s/%s.prefs", xdg, app_id) <
            (int)sizeof buf) {
            return prefs_strdup(buf);
        }
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] == '/') {
        if (snprintf(buf, sizeof buf, "%s/.config/%s.prefs", home,
                     app_id) <
            (int)sizeof buf) {
            return prefs_strdup(buf);
        }
    }
    return NULL;
}

/* Reads the whole file (bounded); returns NULL with *err set on
 * failure. A missing file is NOT an error: *missing is set and
 * NULL with FDK_OK comes back. */
static char *prefs_read_file(const char *path, fdk_result *err,
                             bool *missing) {
    *err = FDK_OK;
    *missing = false;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        if (errno == ENOENT) {
            *missing = true;
            return NULL;
        }
        FDK_WARN("prefs: cannot open %s (%s)", path, strerror(errno));
        *err = FDK_ERR_IO;
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0) {
        FDK_WARN("prefs: cannot stat %s (%s)", path, strerror(errno));
        fclose(f);
        *err = FDK_ERR_IO;
        return NULL;
    }
    if ((unsigned long long)st.st_size > PREFS_MAX_FILE) {
        FDK_WARN("prefs: %s exceeds the %u-byte limit", path,
                 (unsigned)PREFS_MAX_FILE);
        fclose(f);
        *err = FDK_ERR_LIMIT;
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *data = fdk_alloc(size + 1);
    if (data == NULL) {
        fclose(f);
        *err = FDK_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    if (size > 0 && fread(data, 1, size, f) != size) {
        FDK_WARN("prefs: short read on %s", path);
        fdk_free(data);
        fclose(f);
        *err = FDK_ERR_IO;
        return NULL;
    }
    fclose(f);
    data[size] = '\0';
    return data;
}

/* ---- lifecycle -------------------------------------------------------- */

fdk_result fdk_prefs_open(const char *app_id, fdk_prefs **out_prefs) {
    if (out_prefs == NULL || app_id == NULL || app_id[0] == '\0') {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_prefs = NULL;

    fdk_prefs *p = fdk_alloc(sizeof *p);
    if (p == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(p, 0, sizeof *p);
    p->app_id = prefs_strdup(app_id);
    if (p->app_id == NULL) {
        fdk_free(p);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    p->path = fdk__prefs_resolve_path(app_id);

    if (p->path == NULL) {
        /* Memory-only: no directory to read or write. */
        p->source = FDK_PREFS_SOURCE_FRESH;
        *out_prefs = p;
        return FDK_OK;
    }

    bool missing = false;
    fdk_result err = FDK_OK;
    char *data = prefs_read_file(p->path, &err, &missing);
    if (missing) {
        p->source = FDK_PREFS_SOURCE_FRESH;
        *out_prefs = p;
        return FDK_OK; /* first run: clean and quiet */
    }
    if (!fdk_ok(err)) {
        /* Unreadable: same resilience as unparsable — defaults, one
         * warning, the app keeps running. */
        p->source = FDK_PREFS_SOURCE_RECOVERED;
        *out_prefs = p;
        return FDK_OK;
    }
    size_t size = strlen(data);
    if (size == 0) {
        /* An empty file is a first run with a residue file (an app
         * that crashed before its first save). Treat as fresh. */
        p->source = FDK_PREFS_SOURCE_FRESH;
        fdk_free(data);
        *out_prefs = p;
        return FDK_OK;
    }
    int bad_line = prefs_parse(p, data, size);
    fdk_free(data);
    if (bad_line != 0) {
        /* The resilience rule: reject wholesale, serve defaults,
         * say exactly where the file went wrong. */
        for (size_t i = 0; i < p->count; i++) {
            fdk_free(p->rows[i].key);
            fdk_free(p->rows[i].value);
            fdk_free(p->rows[i].file_value);
        }
        p->count = 0;
        p->dirty = 0;
        p->removed = 0;
        FDK_WARN("prefs: %s rejected at line %d — serving defaults; "
                 "the next save() rewrites it",
                 p->path, bad_line);
        p->source = FDK_PREFS_SOURCE_RECOVERED;
    } else {
        p->source = FDK_PREFS_SOURCE_FILE;
    }
    *out_prefs = p;
    return FDK_OK;
}

void fdk_prefs_destroy(fdk_prefs *prefs) {
    if (prefs == NULL) {
        return;
    }
    for (size_t i = 0; i < prefs->count; i++) {
        fdk_free(prefs->rows[i].key);
        fdk_free(prefs->rows[i].value);
        fdk_free(prefs->rows[i].file_value);
    }
    fdk_free(prefs->rows);
    fdk_free(prefs->path);
    fdk_free(prefs->app_id);
    fdk_free(prefs);
}

fdk_prefs_origin fdk_prefs_source(const fdk_prefs *prefs) {
    return (prefs != NULL) ? prefs->source : FDK_PREFS_SOURCE_FRESH;
}

const char *fdk_prefs_path(const fdk_prefs *prefs) {
    return (prefs != NULL) ? prefs->path : NULL;
}

/* ---- typed access ----------------------------------------------------- */

const char *fdk_prefs_get(const fdk_prefs *prefs, const char *key,
                          const char *def) {
    if (prefs == NULL || !key_valid(key)) {
        return def;
    }
    const pref_row *row = find_row_const(prefs, key);
    return (row != NULL) ? row->value : def;
}

/* The typed getters' "log one DEBUG line the first time a present
 * value fails to parse" hook. The getters take a const store (the
 * public contract — a settings READ must not look like a mutation),
 * so the warn-once flag is set through a const cast: it is log
 * bookkeeping, not observable store state (get/save/iterate results
 * are bit-identical with and without it). */
static void warn_bad_value(const fdk_prefs *p, const pref_row *row,
                            const char *kind) {
    /* -Wcast-qual-clean: hop through uintptr_t (integer types carry
     * no qualifiers) — the roundabout IS the honest spelling of
     * "this cast is deliberate; the flag is not store state". */
    pref_row *mutable_row =
        (pref_row *)(uintptr_t)row;
    (void)p;
    if (!mutable_row->bad_warned) {
        mutable_row->bad_warned = 1;
        FDK_DEBUG("prefs: %s does not parse as %s — serving the "
                  "default",
                  row->key, kind);
    }
}

long fdk_prefs_get_int(const fdk_prefs *prefs, const char *key,
                       long def) {
    if (prefs == NULL || !key_valid(key)) {
        return def;
    }
    const pref_row *row = find_row_const(prefs, key);
    if (row == NULL) {
        return def;
    }
    const char *v = row->value;
    if (v[0] == '\0') {
        return def;
    }
    char *end = NULL;
    errno = 0;
    long parsed = strtol(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE) {
        warn_bad_value(prefs, row, "an int");
        return def;
    }
    return parsed;
}

bool fdk_prefs_get_bool(const fdk_prefs *prefs, const char *key,
                        bool def) {
    if (prefs == NULL || !key_valid(key)) {
        return def;
    }
    const pref_row *row = find_row_const(prefs, key);
    if (row == NULL) {
        return def;
    }
    const char *v = row->value;
    if (strcmp(v, "true") == 0 || strcmp(v, "1") == 0) {
        return true;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "0") == 0) {
        return false;
    }
    warn_bad_value(prefs, row, "a bool");
    return def;
}

double fdk_prefs_get_double(const fdk_prefs *prefs, const char *key,
                            double def) {
    if (prefs == NULL || !key_valid(key)) {
        return def;
    }
    const pref_row *row = find_row_const(prefs, key);
    if (row == NULL) {
        return def;
    }
    const char *v = row->value;
    if (v[0] == '\0') {
        return def;
    }
    char *end = NULL;
    errno = 0;
    double parsed = strtod(v, &end);
    if (end == v || *end != '\0') {
        warn_bad_value(prefs, row, "a double");
        return def;
    }
    return parsed;
}

fdk_result fdk_prefs_set(fdk_prefs *prefs, const char *key,
                         const char *value) {
    if (prefs == NULL || !key_valid(key)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (value == NULL) {
        value = "";
    }
    if (strlen(value) > PREFS_MAX_VALUE) {
        return FDK_ERR_LIMIT;
    }
    return row_put(prefs, key, value, NULL);
}

fdk_result fdk_prefs_set_int(fdk_prefs *prefs, const char *key,
                             long value) {
    char buf[32];
    (void)snprintf(buf, sizeof buf, "%ld", value);
    return fdk_prefs_set(prefs, key, buf);
}

fdk_result fdk_prefs_set_bool(fdk_prefs *prefs, const char *key,
                              bool value) {
    return fdk_prefs_set(prefs, key, value ? "true" : "false");
}

fdk_result fdk_prefs_set_double(fdk_prefs *prefs, const char *key,
                                double value) {
    /* %.17g round-trips doubles bit-exactly; %g keeps the file
     * human-friendly for the values people actually store (0.5,
     * 1.25) while remaining lossless. */
    char buf[64];
    (void)snprintf(buf, sizeof buf, "%.17g", value);
    return fdk_prefs_set(prefs, key, buf);
}

fdk_result fdk_prefs_remove(fdk_prefs *prefs, const char *key) {
    if (prefs == NULL || !key_valid(key)) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < prefs->count; i++) {
        if (strcmp(prefs->rows[i].key, key) == 0) {
            fdk_free(prefs->rows[i].key);
            fdk_free(prefs->rows[i].value);
            fdk_free(prefs->rows[i].file_value);
            memmove(&prefs->rows[i], &prefs->rows[i + 1],
                    (prefs->count - i - 1) * sizeof(pref_row));
            prefs->count--;
            /* The tombstone: the row is gone, no row-walk can see
             * it — only save() clears this. */
            prefs->removed = 1;
            prefs->dirty = 1;
            return FDK_OK;
        }
    }
    return FDK_OK; /* absent key: quiet no-op */
}

/* ---- save ------------------------------------------------------------- */

/* Writes section-by-section in row order (rows keep their file
 * order; a re-saved file reproduces the same organization). */
fdk_result fdk_prefs_save(fdk_prefs *prefs) {
    if (prefs == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (prefs->path == NULL) {
        return FDK_ERR_UNSUPPORTED; /* memory-only store */
    }
    if (!prefs->dirty) {
        return FDK_OK; /* unchanged: no disk touch (documented) */
    }

    /* The temp file lives in the SAME directory so the rename is
     * atomic (same filesystem — the whole point). */
    size_t plen = strlen(prefs->path);
    char *tmp = fdk_alloc(plen + 11);
    if (tmp == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memcpy(tmp, prefs->path, plen);
    memcpy(tmp + plen, ".tmpXXXXXX", 11);

    int fd = mkstemp(tmp);
    if (fd < 0) {
        FDK_WARN("prefs: mkstemp for %s failed (%s)", prefs->path,
                 strerror(errno));
        fdk_free(tmp);
        return FDK_ERR_IO;
    }
    FILE *f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd);
        unlink(tmp);
        fdk_free(tmp);
        return FDK_ERR_IO;
    }

    int failed = 0;
    if (fprintf(f, "# FDK preferences v1 — %s\n",
                prefs->app_id) < 0) {
        failed = 1;
    }
    if (!failed && fprintf(f, "# format: docs/fdk-prefs-format.md "
                              "(human-editable)\n\n") < 0) {
        failed = 1;
    }
    char cur_section[PREFS_MAX_PART + 1] = "";
    for (size_t i = 0; !failed && i < prefs->count; i++) {
        const char *dot = strchr(prefs->rows[i].key, '.');
        size_t sec_len = (size_t)(dot - prefs->rows[i].key);
        if (strlen(cur_section) != sec_len ||
            strncmp(cur_section, prefs->rows[i].key, sec_len) != 0) {
            memcpy(cur_section, prefs->rows[i].key, sec_len);
            cur_section[sec_len] = '\0';
            if (fprintf(f, "[%s]\n", cur_section) < 0) {
                failed = 1;
                break;
            }
        }
        if (fprintf(f, "%s = %s\n", dot + 1, prefs->rows[i].value) <
            0) {
            failed = 1;
            break;
        }
    }
    if (!failed && fflush(f) != 0) {
        failed = 1;
    }
    if (!failed && fsync(fd) != 0 && errno != EINVAL) {
        failed = 1; /* EINVAL: some filesystems refuse fsync */
    }
    fclose(f);

    if (failed) {
        FDK_WARN("prefs: write to %s failed (%s)", tmp,
                 strerror(errno));
        unlink(tmp);
        fdk_free(tmp);
        return FDK_ERR_IO;
    }
    if (rename(tmp, prefs->path) != 0) {
        FDK_WARN("prefs: rename %s -> %s failed (%s)", tmp,
                 prefs->path, strerror(errno));
        unlink(tmp);
        fdk_free(tmp);
        return FDK_ERR_IO;
    }
    fdk_free(tmp);

    /* The saved state is now the file state. */
    for (size_t i = 0; i < prefs->count; i++) {
        fdk_free(prefs->rows[i].file_value);
        prefs->rows[i].file_value = prefs_strdup(prefs->rows[i].value);
    }
    prefs->dirty = 0;
    prefs->removed = 0;
    return FDK_OK;
}

/* ---- iteration -------------------------------------------------------- */

size_t fdk_prefs_count(const fdk_prefs *prefs) {
    return (prefs != NULL) ? prefs->count : 0;
}

const char *fdk_prefs_key_at(const fdk_prefs *prefs, size_t index) {
    return (prefs != NULL && index < prefs->count)
               ? prefs->rows[index].key
               : NULL;
}

const char *fdk_prefs_value_at(const fdk_prefs *prefs, size_t index) {
    return (prefs != NULL && index < prefs->count)
               ? prefs->rows[index].value
               : NULL;
}
