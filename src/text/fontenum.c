#define FDK_LOG_TAG "text"

/*
 * fontenum.c — system font enumeration (1.4.3)
 *
 * The font-scan surface exposed as an API: every LOADABLE face under
 * the discovery chain's user-prioritized and standard roots, one
 * fdk_font_info per FACE (a .ttc contributes one entry per face).
 * Only TrueType-flavored sfnts are reported — the rasterizer's
 * honest limit (the same gate fdk_font_load_system_default applies);
 * CFF ('OTTO') files are skipped silently.
 *
 * Family and style come from each face's own 'name' table: the
 * typographic pair (IDs 16/17) when present, else the legacy pair
 * (IDs 1/2). Windows (platform 3) strings are UTF-16BE; Mac (platform
 * 1) strings are Roman. A face whose name table yields no family
 * name is skipped — a nameless entry helps nobody pick a font.
 *
 * The scan walks the filesystem on every call (a chooser calls it
 * once per dialog); caching would trade correctness (fonts install
 * and vanish while a process runs) for speed nobody needs here.
 */

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* fontscan.c keeps its own fs_strdup for the same reason: the
 * widget layer's fdk__strdup is not shared down here. */
static char *enum_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *out = fdk_alloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

/* ---- big-endian readers (the whole sfnt format is BE) ------------- */

static fdk_u32 rd_u32(const fdk_u8 *p) {
    return ((fdk_u32)p[0] << 24) | ((fdk_u32)p[1] << 16) |
           ((fdk_u32)p[2] << 8) | (fdk_u32)p[3];
}

static fdk_u16 rd_u16(const fdk_u8 *p) {
    return (fdk_u16)(((fdk_u32)p[0] << 8) | (fdk_u32)p[1]);
}

/* ---- name-table extraction ---------------------------------------- */

/* Decodes UTF-16BE (Windows platform 3) into a fresh UTF-8 heap
 * string; returns NULL on allocation failure (the BMP-only face-name
 * world keeps this simple — no surrogate pairs in practice). */
static char *decode_utf16be(const fdk_u8 *s, size_t bytes) {
    if ((bytes & 1u) != 0) {
        bytes--; /* a trailing odd byte is corrupt; drop it */
    }
    /* Worst case: every BMP unit becomes 3 UTF-8 bytes. */
    size_t cap = bytes / 2 * 3 + 1;
    char *out = fdk_alloc(cap);
    if (out == NULL) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i + 2 <= bytes; i += 2) {
        fdk_u32 cp = (fdk_u32)rd_u16(s + i);
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

/* Mac Roman is byte-per-char for the ASCII range every real family
 * name lives in; the handful of high bytes map approximately (the
 * classic dingbats positions), which is fine for a picker row. */
static char *decode_roman(const fdk_u8 *s, size_t bytes) {
    char *out = fdk_alloc(bytes + 1);
    if (out == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < bytes; i++) {
        out[i] = (char)s[i];
    }
    out[bytes] = '\0';
    return out;
}

/* Reads one 'name' record's string for `want_id`, preferring
 * platform 3 (UTF-16BE) over platform 1 (Roman). Returns a heap
 * string or NULL. `name` points at the whole table; `len` bounds it. */
static char *name_table_string(const fdk_u8 *name, size_t len,
                               fdk_u16 want_id) {
    if (len < 6) {
        return NULL;
    }
    fdk_u16 count = rd_u16(name + 2);
    fdk_u16 str_off = rd_u16(name + 4);
    if ((size_t)6 + (size_t)count * 12u > len) {
        return NULL; /* records overrun the table: corrupt */
    }
    char *best_utf16 = NULL;
    char *best_roman = NULL;
    for (size_t i = 0; i < count; i++) {
        const fdk_u8 *rec = name + 6 + i * 12;
        fdk_u16 plat = rd_u16(rec + 0);
        fdk_u16 enc = rd_u16(rec + 2);
        fdk_u16 name_id = rd_u16(rec + 6);
        fdk_u16 slen = rd_u16(rec + 8);
        fdk_u16 soff = rd_u16(rec + 10);
        if (name_id != want_id) {
            continue;
        }
        size_t abs_off = (size_t)str_off + (size_t)soff;
        if (abs_off + (size_t)slen > len) {
            continue; /* string overruns: corrupt record, skip */
        }
        const fdk_u8 *str = name + abs_off;
        if (plat == 3 && (enc == 0 || enc == 1) && best_utf16 == NULL) {
            best_utf16 = decode_utf16be(str, slen);
        } else if (plat == 1 && enc == 0 && best_roman == NULL) {
            best_roman = decode_roman(str, slen);
        }
        if (best_utf16 != NULL) {
            break; /* the preferred flavor won */
        }
    }
    if (best_utf16 != NULL) {
        fdk_free(best_roman);
        return best_utf16;
    }
    return best_roman;
}

/* ---- face probing --------------------------------------------------- */

/* Reads the whole 'name' table of the face at `sfnt` (a buffer of
 * `size` bytes starting at the face's sfnt header) into a heap
 * buffer; *out_len receives its length. Returns NULL when the table
 * is absent/unreadable. */
static fdk_u8 *read_name_table(FILE *f, long sfnt_off, long size,
                                size_t *out_len) {
    *out_len = 0;
    if (size < 12) {
        return NULL;
    }
    fdk_u8 head[12];
    if (fseek(f, sfnt_off, SEEK_SET) != 0 ||
        fread(head, 1, 12, f) != 12) {
        return NULL;
    }
    fdk_u16 num_tables = rd_u16(head + 4);
    /* Find the 'name' table record. */
    for (fdk_u16 i = 0; i < num_tables; i++) {
        fdk_u8 rec[16];
        if (fseek(f, sfnt_off + 12 + (long)i * 16, SEEK_SET) != 0 ||
            fread(rec, 1, 16, f) != 16) {
            return NULL;
        }
        if (memcmp(rec, "name", 4) != 0) {
            continue;
        }
        fdk_u32 off = rd_u32(rec + 8);
        fdk_u32 tlen = rd_u32(rec + 12);
        if ((long)off >= size || tlen == 0 || tlen > (1u << 20)) {
            return NULL; /* insane offsets: corrupt */
        }
        fdk_u8 *buf = fdk_alloc(tlen);
        if (buf == NULL) {
            return NULL;
        }
        if (fseek(f, (long)off, SEEK_SET) != 0 ||
            fread(buf, 1, tlen, f) != tlen) {
            fdk_free(buf);
            return NULL;
        }
        *out_len = (size_t)tlen;
        return buf;
    }
    return NULL;
}

/* One discovered face, pre-sort. */
typedef struct enum_face {
    char *family;
    char *style;
    char *path;
    fdk_i32 face_index;
} enum_face;

typedef struct enum_faces {
    enum_face *v;
    size_t n;
    size_t cap;
} enum_faces;

static int enum_face_cmp(const void *a, const void *b) {
    const enum_face *fa = a;
    const enum_face *fb = b;
    int c = strcmp(fa->family, fb->family);
    if (c != 0) {
        return c;
    }
    c = strcmp(fa->style, fb->style);
    if (c != 0) {
        return c;
    }
    c = strcmp(fa->path, fb->path);
    if (c != 0) {
        return c;
    }
    return (fa->face_index < fb->face_index)
        ? -1
        : (fa->face_index > fb->face_index ? 1 : 0);
}

static void enum_face_free(enum_face *f) {
    fdk_free(f->family);
    fdk_free(f->style);
    fdk_free(f->path);
    f->family = NULL;
    f->style = NULL;
    f->path = NULL;
}

/* Probes one file: every loadable face inside it becomes an entry.
 * A face with no family name is skipped (its style alone cannot
 * label a row honestly). */
static void enum_probe_file(enum_faces *ef, const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return;
    }
    /* File size, for sanity bounds. */
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 12) {
        fclose(f);
        return;
    }

    fdk_u8 top[12];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(top, 1, 12, f) != 12) {
        fclose(f);
        return;
    }
    fdk_u32 tag = rd_u32(top);

    /* The face offsets to probe: a plain sfnt is one face at 0; a
     * collection is every table-directory offset in its header. */
    long face_offs[64];
    fdk_i32 face_count = 0;
    if (tag == 0x74746366u) { /* 'ttcf' */
        if (fseek(f, 8, SEEK_SET) != 0) {
            fclose(f);
            return;
        }
        fdk_u8 cnt[4];
        if (fread(cnt, 1, 4, f) != 4) {
            fclose(f);
            return;
        }
        fdk_u32 num_fonts = rd_u32(cnt);
        if (num_fonts == 0 || num_fonts > 64) {
            fclose(f);
            return; /* absurd collections are corrupt */
        }
        for (fdk_u32 i = 0; i < num_fonts; i++) {
            fdk_u8 off[4];
            if (fread(off, 1, 4, f) != 4) {
                fclose(f);
                return;
            }
            fdk_u32 o = rd_u32(off);
            if ((long)o + 12 > size) {
                continue; /* corrupt offset: skip that face */
            }
            face_offs[face_count++] = (long)o;
        }
    } else if (tag == 0x00010000u || tag == 0x74727565u) {
        /* TrueType glyf flavor ('true' is the legacy Apple tag). */
        face_offs[face_count++] = 0;
    } else {
        /* 'OTTO' (CFF) or garbage: the rasterizer cannot draw it. */
        fclose(f);
        return;
    }

    for (fdk_i32 fi = 0; fi < face_count; fi++) {
        size_t table_len = 0;
        fdk_u8 *table = read_name_table(f, face_offs[fi], size,
                                        &table_len);
        if (table == NULL) {
            continue;
        }
        /* Typographic pair first, legacy pair as fallback. */
        char *family = name_table_string(table, table_len, 16);
        char *style = name_table_string(table, table_len, 17);
        if (family == NULL) {
            family = name_table_string(table, table_len, 1);
        }
        if (style == NULL) {
            style = name_table_string(table, table_len, 2);
        }
        fdk_free(table);
        if (family == NULL || family[0] == '\0') {
            fdk_free(family);
            fdk_free(style);
            continue;
        }
        if (style == NULL || style[0] == '\0') {
            fdk_free(style);
            style = enum_strdup("Regular");
            if (style == NULL) {
                fdk_free(family);
                continue;
            }
        }
        char *path_copy = enum_strdup(path);
        if (path_copy == NULL) {
            fdk_free(family);
            fdk_free(style);
            continue;
        }
        if (ef->n == ef->cap) {
            size_t ncap = (ef->cap == 0) ? 64 : ef->cap * 2;
            enum_face *grown =
                fdk_realloc(ef->v, ncap * sizeof(*grown));
            if (grown == NULL) {
                fdk_free(family);
                fdk_free(style);
                fdk_free(path_copy);
                continue; /* this face is lost, the walk survives */
            }
            ef->v = grown;
            ef->cap = ncap;
        }
        ef->v[ef->n].family = family;
        ef->v[ef->n].style = style;
        ef->v[ef->n].path = path_copy;
        ef->v[ef->n].face_index = fi;
        ef->n++;
    }
    fclose(f);
}

/* ---- the directory walk (fontscan's roots, unranked) --------------- */

#define ENUM_MAX_DEPTH 6
#define ENUM_NAME_MAX 512

static bool enum_name_is_font(const char *lower, size_t n) {
    return n > 4 &&
           (memcmp(lower + n - 4, ".ttf", 4) == 0 ||
            memcmp(lower + n - 4, ".ttc", 4) == 0);
}

static void enum_scan_dir(enum_faces *ef, const char *dir, int depth) {
    if (depth > ENUM_MAX_DEPTH) {
        return;
    }
    DIR *d = opendir(dir);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        char full[ENUM_NAME_MAX];
        int len = snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (len < 0 || (size_t)len >= sizeof(full)) {
            continue;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            enum_scan_dir(ef, full, depth + 1);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        size_t n = strlen(e->d_name);
        if (n >= ENUM_NAME_MAX) {
            continue;
        }
        char lower[ENUM_NAME_MAX];
        for (size_t i = 0; i <= n; i++) {
            lower[i] = (char)tolower((unsigned char)e->d_name[i]);
        }
        if (enum_name_is_font(lower, n)) {
            enum_probe_file(ef, full);
        }
    }
    closedir(d);
}

/* ---- public API ------------------------------------------------------ */

fdk_result fdk_font_enumerate(fdk_font_info **out_infos,
                              size_t *out_count) {
    if (out_infos == NULL || out_count == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    *out_infos = NULL;
    *out_count = 0;

    enum_faces ef;
    memset(&ef, 0, sizeof(ef));

    /* $FDK_FONT_DIRS first (the user's explicit priority), then the
     * standard roots — fontscan.c's stage order, unranked because a
     * picker wants EVERYTHING, not a winner. */
    const char *user = getenv("FDK_FONT_DIRS");
    if (user != NULL && user[0] != '\0') {
        char *copy = enum_strdup(user);
        if (copy != NULL) {
            char *save = NULL;
            for (char *tok = strtok_r(copy, ":", &save); tok != NULL;
                 tok = strtok_r(NULL, ":", &save)) {
                if (tok[0] != '\0') {
                    enum_scan_dir(&ef, tok, 1);
                }
            }
            fdk_free(copy);
        }
    }
    enum_scan_dir(&ef, "/usr/share/fonts", 1);
    enum_scan_dir(&ef, "/usr/local/share/fonts", 1);
    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        char buf[ENUM_NAME_MAX];
        if (snprintf(buf, sizeof(buf), "%s/fonts", xdg) <
            (int)sizeof(buf)) {
            enum_scan_dir(&ef, buf, 1);
        }
    } else if (home != NULL && home[0] != '\0') {
        char buf[ENUM_NAME_MAX];
        if (snprintf(buf, sizeof(buf), "%s/.local/share/fonts", home) <
            (int)sizeof(buf)) {
            enum_scan_dir(&ef, buf, 1);
        }
    }
    if (home != NULL && home[0] != '\0') {
        char buf[ENUM_NAME_MAX];
        if (snprintf(buf, sizeof(buf), "%s/.fonts", home) <
            (int)sizeof(buf)) {
            enum_scan_dir(&ef, buf, 1);
        }
    }

    if (ef.n == 0) {
        FDK_INFO("fdk_font_enumerate: no loadable faces found under "
                 "the probed roots");
        return FDK_OK; /* an empty list is an honest answer */
    }

    qsort(ef.v, ef.n, sizeof(*ef.v), enum_face_cmp);

    /* Move into the public shape (array of structs with owned
     * strings — one block, caller frees via the list_free API). */
    fdk_font_info *infos = fdk_alloc_array(ef.n, sizeof(*infos));
    if (infos == NULL) {
        for (size_t i = 0; i < ef.n; i++) {
            enum_face_free(&ef.v[i]);
        }
        fdk_free(ef.v);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    for (size_t i = 0; i < ef.n; i++) {
        infos[i].family = ef.v[i].family;
        infos[i].style = ef.v[i].style;
        infos[i].path = ef.v[i].path;
        infos[i].face_index = ef.v[i].face_index;
    }
    fdk_free(ef.v);
    *out_infos = infos;
    *out_count = ef.n;
    return FDK_OK;
}

void fdk_font_info_list_free(fdk_font_info *infos, size_t count) {
    if (infos == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_free(infos[i].family);
        fdk_free(infos[i].style);
        fdk_free(infos[i].path);
    }
    fdk_free(infos);
}
