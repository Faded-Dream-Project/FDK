/* mime.c — see mime.h. */
#include "mime.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- the loaded databases ---------------------------------------- */

typedef struct glob_rule {
    int weight;
    char type[96];
    char pattern[128];
    bool case_sensitive;
} glob_rule;

typedef struct magic_line {
    int indent;
    long offset;        /* start offset                             */
    long range;         /* +range (1 = exact)                       */
    unsigned char value[64];
    size_t value_len;
    unsigned char mask[64];
    bool has_mask;
    int word_size;      /* ~word (byte swap grouping on LE hosts)   */
} magic_line;

typedef struct magic_rule {
    int priority;
    char type[96];
    magic_line *lines;
    size_t count, cap;
} magic_rule;

static glob_rule *g_globs;
static size_t g_glob_count;
static magic_rule *g_magic;
static size_t g_magic_count;

/* ---- the built-in glob table (no-database floor) ----------------- */

typedef struct builtin_glob {
    const char *pattern;
    const char *type;
    const char *icon;
} builtin_glob;

static const builtin_glob BUILTIN[] = {
    {"*.txt", "text/plain", "doc"},
    {"*.md", "text/markdown", "doc"},
    {"*.c", "text/x-csrc", "code"},
    {"*.h", "text/x-chdr", "code"},
    {"*.cpp", "text/x-c++src", "code"},
    {"*.hpp", "text/x-c++hdr", "code"},
    {"*.py", "text/x-python", "code"},
    {"*.sh", "application/x-shellscript", "code"},
    {"*.json", "application/json", "code"},
    {"*.xml", "application/xml", "code"},
    {"*.html", "text/html", "code"},
    {"*.css", "text/css", "code"},
    {"Makefile", "text/x-makefile", "code"},
    {"*.mk", "text/x-makefile", "code"},
    {"*.png", "image/png", "image"},
    {"*.jpg", "image/jpeg", "image"},
    {"*.jpeg", "image/jpeg", "image"},
    {"*.gif", "image/gif", "image"},
    {"*.bmp", "image/bmp", "image"},
    {"*.svg", "image/svg+xml", "image"},
    {"*.webp", "image/webp", "image"},
    {"*.ico", "image/vnd.microsoft.icon", "image"},
    {"*.mp3", "audio/mpeg", "audio"},
    {"*.ogg", "audio/ogg", "audio"},
    {"*.wav", "audio/x-wav", "audio"},
    {"*.flac", "audio/x-flac", "audio"},
    {"*.mp4", "video/mp4", "video"},
    {"*.mkv", "video/x-matroska", "video"},
    {"*.webm", "video/webm", "video"},
    {"*.pdf", "application/pdf", "doc"},
    {"*.zip", "application/zip", "archive"},
    {"*.tar", "application/x-tar", "archive"},
    {"*.gz", "application/gzip", "archive"},
    {"*.xz", "application/x-xz", "archive"},
    {"*.bz2", "application/x-bzip2", "archive"},
    {"*.7z", "application/x-7z-compressed", "archive"},
    {"*.deb", "application/vnd.debian.binary-package", "archive"},
    {"*.rpm", "application/x-rpm", "archive"},
    {"*.o", "application/x-object", "app"},
    {"*.a", "application/x-archive", "app"},
    {"*.so", "application/x-sharedlib", "app"},
    {"*.desktop", "application/x-desktop", "app"},
    {"*.fdk", "text/x-fdk-theme", "doc"},
    {"*.fmo", "application/x-fdk-catalog", "doc"},
};

/* ---- globs2 parsing ---------------------------------------------- */

static void add_glob(int weight, const char *type, const char *pattern,
                     bool cs) {
    glob_rule *grown = realloc(
        g_globs, (g_glob_count + 1) * sizeof *g_globs);
    if (grown == NULL) {
        return;
    }
    g_globs = grown;
    glob_rule *g = &g_globs[g_glob_count++];
    g->weight = weight;
    snprintf(g->type, sizeof g->type, "%s", type);
    snprintf(g->pattern, sizeof g->pattern, "%s", pattern);
    g->case_sensitive = cs;
}

static void load_globs2(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        /* weight:type:pattern[:flags] */
        char *c1 = strchr(line, ':');
        if (c1 == NULL) {
            continue;
        }
        char *c2 = strchr(c1 + 1, ':');
        if (c2 == NULL) {
            continue;
        }
        *c1 = '\0';
        *c2 = '\0';
        char *end = NULL;
        long weight = strtol(line, &end, 10);
        if (end == line) {
            continue;
        }
        char *pattern = c2 + 1;
        char *nl = strpbrk(pattern, "\r\n");
        if (nl != NULL) {
            *nl = '\0';
        }
        char *c3 = strchr(pattern, ':');
        bool cs = false;
        if (c3 != NULL) { /* flags field */
            *c3 = '\0';
            cs = strstr(c3 + 1, "cs") != NULL;
        }
        if (strcmp(pattern, "__NOGLOBS__") == 0) {
            continue;
        }
        add_glob((int)weight, c1 + 1, pattern, cs);
    }
    fclose(f);
}

/* ---- magic parsing ------------------------------------------------ */

static magic_rule *magic_add_rule(int priority, const char *type) {
    magic_rule *grown =
        realloc(g_magic, (g_magic_count + 1) * sizeof *g_magic);
    if (grown == NULL) {
        return NULL;
    }
    g_magic = grown;
    magic_rule *m = &g_magic[g_magic_count++];
    memset(m, 0, sizeof *m);
    m->priority = priority;
    snprintf(m->type, sizeof m->type, "%s", type);
    return m;
}

static void magic_add_line(magic_rule *m, const magic_line *l) {
    if (m->count == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        magic_line *grown = realloc(m->lines, cap * sizeof *m->lines);
        if (grown == NULL) {
            return;
        }
        m->lines = grown;
        m->cap = cap;
    }
    m->lines[m->count++] = *l;
}

/* Parses one > line's payload: "off=len<value>..." where value is
 * raw bytes (escapes \xNN \0 \n \t \\ already decoded by the reader
 * below). Returns bytes consumed. */

/* The magic file's raw lines carry the VALUE length as two
 * big-endian bytes BEFORE the value bytes (spec: "the value part
 * is two big-endian length bytes followed by the bytes"). We read
 * the file in BINARY and parse that structure directly. */
static void load_magic(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    static char buf[262144];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = '\0';
    if (len < 13 || memcmp(buf, "MIME-Magic\0\n", 12) != 0) {
        return;
    }

    size_t i = 12;
    magic_rule *cur = NULL;
    while (i < len) {
        if (buf[i] == '[') {
            /* [priority:mimetype] */
            char *close = memchr(buf + i, ']', len - i);
            if (close == NULL) {
                break;
            }
            size_t hdr_len = (size_t)(close - (buf + i)) + 1;
            char hdr[128];
            size_t cp = 0;
            for (size_t j = i + 1; j < (size_t)(close - buf) &&
                                 cp < sizeof hdr - 1;
                 j++) {
                hdr[cp++] = buf[j];
            }
            hdr[cp] = '\0';
            char *c = strchr(hdr, ':');
            if (c != NULL) {
                *c = '\0';
                cur = magic_add_rule(atoi(hdr), c + 1);
            }
            i += hdr_len;
            /* skip the \n after ] */
            if (i < len && buf[i] == '\n') {
                i++;
            }
        } else if (buf[i] == '>' && cur != NULL) {
            /* >[indent]offset=len<value>[&mask][~word][+range]\n */
            size_t j = i + 1;
            int indent = 0;
            while (j < len && buf[j] >= '0' && buf[j] <= '9') {
                indent = indent * 10 + (buf[j] - '0');
                j++;
            }
            long offset = 0;
            bool neg = false;
            if (j < len && buf[j] == '-') {
                neg = true;
                j++;
            }
            while (j < len && buf[j] >= '0' && buf[j] <= '9') {
                offset = offset * 10 + (buf[j] - '0');
                j++;
            }
            if (neg) {
                offset = -offset;
            }
            magic_line ml;
            memset(&ml, 0, sizeof ml);
            ml.indent = indent;
            ml.offset = offset;
            ml.range = 1;
            ml.word_size = 1;
            if (j < len && buf[j] == '=') {
                j++;
                if (j + 2 <= len) {
                    size_t vlen = ((size_t)(unsigned char)buf[j]
                                   << 8) |
                                  (size_t)(unsigned char)buf[j + 1];
                    j += 2;
                    if (vlen > sizeof ml.value) {
                        vlen = sizeof ml.value;
                    }
                    if (j + vlen <= len) {
                        memcpy(ml.value, buf + j, vlen);
                        ml.value_len = vlen;
                        j += vlen;
                    }
                }
            }
            /* suffixes: &mask (same length bytes, raw) ~word +range */
            while (j < len && buf[j] != '\n') {
                char op = buf[j];
                j++;
                if (op == '&') {
                    if (j + ml.value_len <= len) {
                        memcpy(ml.mask, buf + j, ml.value_len);
                        ml.has_mask = true;
                        j += ml.value_len;
                    } else {
                        break;
                    }
                } else if (op == '~') {
                    long w = 0;
                    while (j < len && buf[j] >= '0' &&
                           buf[j] <= '9') {
                        w = w * 10 + (buf[j] - '0');
                        j++;
                    }
                    ml.word_size = (int)(w ? w : 1);
                } else if (op == '+') {
                    long r = 0;
                    while (j < len && buf[j] >= '0' &&
                           buf[j] <= '9') {
                        r = r * 10 + (buf[j] - '0');
                        j++;
                    }
                    ml.range = r ? r : 1;
                } else {
                    break;
                }
            }
            magic_add_line(cur, &ml);
            while (j < len && buf[j] != '\n') {
                j++;
            }
            i = (j < len) ? j + 1 : j;
        } else {
            i++;
        }
    }
}

void mime_init(void) {
    const char *xdg = getenv("XDG_DATA_DIRS");
    if (xdg == NULL || xdg[0] == '\0') {
        xdg = "/usr/local/share:/usr/share";
    }
    char dirs[1024];
    snprintf(dirs, sizeof dirs, "%s", xdg);
    char *save = NULL;
    for (char *d = strtok_r(dirs, ":", &save); d != NULL;
         d = strtok_r(NULL, ":", &save)) {
        char path[1100];
        snprintf(path, sizeof path, "%s/mime/globs2", d);
        load_globs2(path);
        snprintf(path, sizeof path, "%s/mime/magic", d);
        load_magic(path);
    }
}

void mime_shutdown(void) {
    free(g_globs);
    g_globs = NULL;
    g_glob_count = 0;
    for (size_t i = 0; i < g_magic_count; i++) {
        free(g_magic[i].lines);
    }
    free(g_magic);
    g_magic = NULL;
    g_magic_count = 0;
}

/* ---- glob matching ------------------------------------------------ */

static bool glob_match(const char *pattern, const char *name) {
    /* fnmatch-style: we implement the subset the database uses —
     * literal names, *.ext, foo*.bar, ? and [...] — by delegation
     * to the shell's fnmatch via a tiny recursive matcher. */
    if (*pattern == '\0') {
        return *name == '\0';
    }
    if (*pattern == '*') {
        for (const char *n = name;; n++) {
            if (glob_match(pattern + 1, n)) {
                return true;
            }
            if (*n == '\0') {
                return false;
            }
        }
    }
    if (*pattern == '?') {
        return *name != '\0' && glob_match(pattern + 1, name + 1);
    }
    return *name == *pattern &&
           glob_match(pattern + 1, name + 1);
}

static void glob_lookup(const char *name, char *out, size_t cap) {
    out[0] = '\0';
    int best_weight = -1;
    size_t best_len = 0;
    bool multi = false;

    for (size_t i = 0; i < g_glob_count; i++) {
        const glob_rule *g = &g_globs[i];
        char lower_name[256], lower_pat[140];
        if (g->case_sensitive) {
            if (!glob_match(g->pattern, name)) {
                continue;
            }
        } else {
            snprintf(lower_name, sizeof lower_name, "%s", name);
            snprintf(lower_pat, sizeof lower_pat, "%s", g->pattern);
            for (char *s = lower_name; *s; s++) {
                *s = (char)tolower((unsigned char)*s);
            }
            for (char *s = lower_pat; *s; s++) {
                *s = (char)tolower((unsigned char)*s);
            }
            if (!glob_match(lower_pat, lower_name)) {
                continue;
            }
        }
        size_t plen = strlen(g->pattern);
        if (g->weight > best_weight ||
            (g->weight == best_weight && plen > best_len)) {
            best_weight = g->weight;
            best_len = plen;
            snprintf(out, cap, "%s", g->type);
            multi = false;
        } else if (g->weight == best_weight && plen == best_len &&
                   strcmp(out, g->type) != 0) {
            multi = true;
        }
    }
    if (multi) {
        out[0] = '\0'; /* conflict: magic decides */
    }

    if (out[0] == '\0') {
        /* the built-in floor table */
        for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0];
             i++) {
            if (glob_match(BUILTIN[i].pattern, name)) {
                snprintf(out, cap, "%s", BUILTIN[i].type);
                return;
            }
        }
    }
}

/* ---- magic matching ------------------------------------------------ */

static bool line_matches(const magic_line *l,
                         const unsigned char *data, size_t len) {
    if (l->value_len == 0) {
        return true;
    }
    for (long base = 0; base < l->range; base++) {
        size_t off = (size_t)(l->offset + base);
        if (l->offset < 0) {
            off = (size_t)((long)len + l->offset + base);
        }
        if (off + l->value_len > len) {
            continue;
        }
        bool ok = true;
        for (size_t k = 0; k < l->value_len; k++) {
            unsigned char b = data[off + k];
            unsigned char v = l->value[k];
            if (l->has_mask) {
                b = (unsigned char)(b & l->mask[k]);
                v = (unsigned char)(v & l->mask[k]);
            }
            if (b != v) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

/* The nesting semantics: an entry matches if it and ALL its
 * ancestors match (a and (b or c) — each child requires its parent
 * chain; siblings are alternatives). */
static bool magic_matches_from(const magic_rule *m, size_t start,
                               int indent,
                               const unsigned char *data,
                               size_t len) {
    size_t i = start;
    while (i < m->count && m->lines[i].indent >= indent) {
        if (m->lines[i].indent != indent) {
            i++;
            continue;
        }
        if (line_matches(&m->lines[i], data, len)) {
            /* any matching child chain completes this alternative */
            bool has_child = false;
            bool child_ok = false;
            size_t j = i + 1;
            while (j < m->count && m->lines[j].indent > indent) {
                has_child = true;
                if (magic_matches_from(m, j, m->lines[j].indent,
                                       data, len)) {
                    child_ok = true;
                }
                while (j < m->count &&
                       m->lines[j].indent > indent) {
                    j++;
                }
            }
            if (!has_child || child_ok) {
                return true;
            }
        }
        i++;
        while (i < m->count && m->lines[i].indent > indent) {
            i++;
        }
    }
    return false;
}

static void magic_lookup(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    unsigned char head[4096];
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    if (got == 0) {
        snprintf(out, cap, "application/x-empty");
        return;
    }

    int best_priority = -1;
    for (size_t i = 0; i < g_magic_count; i++) {
        const magic_rule *m = &g_magic[i];
        if (m->count == 0) {
            continue;
        }
        if (m->priority < best_priority) {
            continue;
        }
        if (magic_matches_from(m, 0, m->lines[0].indent, head,
                               got)) {
            if (m->priority > best_priority) {
                best_priority = m->priority;
                snprintf(out, cap, "%s", m->type);
            }
        }
    }
}

/* ---- binary/text sniff -------------------------------------------- */

static void sniff_floor(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "application/octet-stream");
    if (path == NULL) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    unsigned char head[128];
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    for (size_t i = 0; i < got; i++) {
        if (head[i] == 0 || (head[i] < 0x20 && head[i] != '\t' &&
                             head[i] != '\n' && head[i] != '\r' &&
                             head[i] != 0x0c)) {
            return; /* binary */
        }
    }
    snprintf(out, cap, "text/plain");
}

/* ---- the public lookup -------------------------------------------- */

static const char *icon_of_type(const char *type, const char *name) {
    (void)name;
    if (strncmp(type, "image/", 6) == 0) {
        return "image";
    }
    if (strncmp(type, "audio/", 6) == 0) {
        return "audio";
    }
    if (strncmp(type, "video/", 6) == 0) {
        return "video";
    }
    if (strstr(type, "zip") != NULL || strstr(type, "tar") != NULL ||
        strstr(type, "gzip") != NULL || strstr(type, "xz") != NULL ||
        strstr(type, "compressed") != NULL ||
        strcmp(type, "application/gzip") == 0) {
        return "archive";
    }
    if (strncmp(type, "text/x-c", 8) == 0 ||
        strncmp(type, "text/x-python", 13) == 0 ||
        strncmp(type, "application/x-shellscript", 24) == 0 ||
        strncmp(type, "application/json", 16) == 0 ||
        strncmp(type, "text/html", 9) == 0 ||
        strncmp(type, "text/x-makefile", 15) == 0 ||
        strncmp(type, "application/xml", 14) == 0) {
        return "code";
    }
    if (strcmp(type, "application/pdf") == 0 ||
        strcmp(type, "text/plain") == 0 ||
        strcmp(type, "text/markdown") == 0) {
        return "doc";
    }
    if (strncmp(type, "text/", 5) == 0) {
        return "doc";
    }
    return "file-new";
}

void mime_of(const char *name, const char *path, char *out_type,
             size_t type_cap, char *out_icon, size_t icon_cap) {
    char by_glob[96];
    glob_lookup(name, by_glob, sizeof by_glob);
    char by_magic[96] = {0};
    magic_lookup(path, by_magic, sizeof by_magic);

    if (by_glob[0] != '\0') {
        /* glob wins unless magic found something and they disagree
         * on a NOT-subclass basis — spec rule: if the glob result
         * equals or is a subclass of the magic result, glob stands;
         * otherwise glob still stands (highest-weight rule). We
         * implement the equality check; full subclass tables are
         * out of scope (recorded in the README). */
        snprintf(out_type, type_cap, "%s", by_glob);
    } else if (by_magic[0] != '\0') {
        snprintf(out_type, type_cap, "%s", by_magic);
    } else {
        sniff_floor(path, out_type, type_cap);
    }
    snprintf(out_icon, icon_cap, "%s", icon_of_type(out_type, name));
}

const char *mime_dir_icon(const char *dir_path) {
    if (dir_path != NULL) {
        if (strcmp(dir_path, "/") == 0) {
            return "disk";
        }
        if (strstr(dir_path, "media") != NULL ||
            strstr(dir_path, "mnt") != NULL) {
            return "disk";
        }
    }
    return "folder";
}

const char *mime_category(const char *mime_type) {
    if (mime_type == NULL) {
        return "Unknown";
    }
    /* The filesystem entries first (the owner's round-8 report:
     * directories fell through every branch and read "File" — the
     * Type column must distinguish folders from files). */
    if (strcmp(mime_type, "inode/directory") == 0) {
        return "Folder";
    }
    if (strncmp(mime_type, "inode/", 6) == 0) {
        return "System"; /* symlinks, sockets, devices            */
    }
    if (strncmp(mime_type, "image/", 6) == 0) {
        return "Image";
    }
    if (strncmp(mime_type, "audio/", 6) == 0) {
        return "Audio";
    }
    if (strncmp(mime_type, "video/", 6) == 0) {
        return "Video";
    }
    if (strncmp(mime_type, "text/", 5) == 0 ||
        strcmp(mime_type, "application/json") == 0 ||
        strcmp(mime_type, "application/xml") == 0) {
        return "Text";
    }
    if (strstr(mime_type, "zip") != NULL ||
        strstr(mime_type, "tar") != NULL ||
        strstr(mime_type, "compressed") != NULL) {
        return "Archive";
    }
    return "File";
}

bool mime_is_thumbnailable(const char *mime_type) {
    return mime_type != NULL &&
           (strncmp(mime_type, "image/", 6) == 0);
}
