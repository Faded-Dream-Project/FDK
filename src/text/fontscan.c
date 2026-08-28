/*
 * fontscan.c — system default font discovery for
 * fdk_font_load_system_default() (post-1.0.1 rework).
 *
 * The original probe was a fixed list of exact file paths, which broke
 * on any layout it did not enumerate — most visibly Arch Linux, whose
 * noto-fonts package ships variable fonts named "NotoSans[wdth,wght]
 * .ttf" rather than "NotoSans-Regular.ttf". This module replaces it
 * with the resolution order GTK and QT use on Linux, minus their hard
 * build-time dependency:
 *
 *   1. $FDK_FONT_FILE        — explicit user override (one font file)
 *   2. $FDK_FONT_DIRS scan   — user-prioritized directories (':' list),
 *                              filename-ranked recursive scan
 *   3. fontconfig            — the system font policy, resolved at RUN
 *                              TIME via dlopen("libfontconfig.so.1"):
 *                              no new build-time or link-time
 *                              dependency, and stripped containers
 *                              simply fall through
 *   4. known exact paths     — the historical Debian/Ubuntu/Fedora
 *                              candidate list (cheap, deterministic)
 *   5. standard roots scan   — /usr/share/fonts, /usr/local/share/
 *                              fonts, $XDG_DATA_HOME/fonts (default
 *                              ~/.local/share/fonts), ~/.fonts — the
 *                              same ranked filename scan as (2)
 *
 * Stages 3-5 accept any TrueType-flavored sfnt ('true' glyf outlines
 * or a 'ttcf' collection face); CFF-flavored OpenType ('OTTO') is
 * rejected because the bundled stb_truetype cannot rasterize it.
 * Variable fonts are fine: stb parses them and renders the default
 * instance (verified against NotoSansSC[wght].ttf).
 *
 * Single-threaded like the rest of FDK's object model. The resolved
 * path (including the negative result) is cached for the process
 * lifetime; the test suite resets it via
 * fdk_text_font_discovery_reset_for_tests().
 *
 * The fontconfig handle, once dlopened, is never dlclose'd: FDK never
 * calls FcFini, and unloading a library whose global state (font
 * caches) we initialized is riskier than keeping it mapped. GTK and
 * QT keep it mapped for the process lifetime too.
 */

#define FDK_LOG_TAG "text"

#include "text_internal.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- LeakSanitizer scoping ------------------------------------------- */
/*
 * fontconfig's one-time init (FcInit + the first FcFontSort) allocates
 * a few small pools it holds for the process lifetime. FDK must NOT
 * FcFini them away: the host application may itself be using
 * fontconfig (HarfBuzz/Pango/Qt stacks do), and tearing down shared
 * global state from inside a toolkit is exactly the kind of
 * side effect FDK forbids itself. GTK and QT keep fontconfig
 * initialized for the process lifetime too.
 *
 * Consequence: an ASan+LSan build of an FDK-only application would
 * report those allocations as "leaks" even though they are
 * reachable-by-design. The __lsan_disable/__lsan_enable bracket
 * below scopes them out of the report — the FDK code paths in
 * between stay fully instrumented. On toolchains without the
 * sanitizer the bracket compiles to nothing.
 */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FDK_HAVE_LSAN 1
#endif
#elif defined(__SANITIZE_ADDRESS__)
#define FDK_HAVE_LSAN 1
#endif

#ifdef FDK_HAVE_LSAN
/* Provided by the LeakSanitizer runtime; declared here because no
 * installed header exposes them under strict build flags. */
void __lsan_disable(void);
void __lsan_enable(void);
static void fs_lsan_off(void) {
    __lsan_disable();
}
static void fs_lsan_on(void) {
    __lsan_enable();
}
#else
static void fs_lsan_off(void) {}
static void fs_lsan_on(void) {}
#endif

/* ---- tiny local helpers --------------------------------------------- */

/* Reject-list membership (the list itself lives with the resolver
 * state at the bottom of this file; the tag gate needs it). */
static bool fs_is_rejected(const char *path);

static fdk_u32 fs_be32(const unsigned char *p) {
    return ((fdk_u32)p[0] << 24) | ((fdk_u32)p[1] << 16) |
           ((fdk_u32)p[2] << 8) | (fdk_u32)p[3];
}

/* fdk_alloc'd copy (there is no project-wide fdk_strdup). */
static char *fs_strdup(const char *s) {
    size_t n = strlen(s);
    if (n == SIZE_MAX) {
        return NULL;
    }
    char *d = fdk_alloc(n + 1);
    if (d != NULL) {
        memcpy(d, s, n + 1);
    }
    return d;
}

/* Tag-level gate: is `path` a TrueType-flavored sfnt, or a 'ttcf'
 * collection containing face `face`? Reads only the header bytes; the
 * full table-directory validation happens later inside
 * fdk_text_font_load_face() (text.c). On success *out_face receives
 * the face to parse (clamped to 0 for negative input). */
static bool fs_file_has_face(const char *path, long face, long *out_face) {
    if (face < 0) {
        face = 0;
    }
    if (fs_is_rejected(path)) {
        return false; /* failed the loader's full validation before */
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = false;
    unsigned char hdr[12];
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        fdk_u32 tag = fs_be32(hdr);
        if (tag == 0x00010000u && face == 0) {
            ok = true; /* plain sfnt, glyf outlines */
        } else if (tag == 0x74746366u) { /* 'ttcf' — collection */
            fdk_u32 num = fs_be32(hdr + 8);
            if (num != 0 && (fdk_u32)face < num) {
                unsigned char offb[4];
                if (fseek(f, (long)(12u + 4u * (fdk_u32)face),
                          SEEK_SET) == 0 &&
                    fread(offb, 1, 4, f) == 4) {
                    fdk_u32 off = fs_be32(offb);
                    unsigned char sub[4];
                    if (off >= 12u && fseek(f, (long)off, SEEK_SET) == 0 &&
                        fread(sub, 1, 4, f) == 4 &&
                        fs_be32(sub) == 0x00010000u) {
                        ok = true; /* that face is TrueType-flavored */
                    }
                }
            }
        }
    }
    fclose(f);
    if (ok && out_face != NULL) {
        *out_face = face;
    }
    return ok;
}

/* ---- stage 1: $FDK_FONT_FILE ---------------------------------------- */

static bool fs_try_env_file(char **out_path, long *out_face) {
    const char *v = getenv("FDK_FONT_FILE");
    if (v == NULL || v[0] == '\0') {
        return false;
    }
    long face = 0;
    if (!fs_file_has_face(v, 0, &face)) {
        FDK_WARN("FDK_FONT_FILE=%s is not a usable TrueType font — "
                 "ignoring the override and falling back to system "
                 "discovery",
                 v);
        return false;
    }
    char *dup = fs_strdup(v);
    if (dup == NULL) {
        return false;
    }
    *out_path = dup;
    *out_face = face;
    return true;
}

/* ---- stage 3: fontconfig, resolved at run time ----------------------- */
/*
 * fontconfig's public headers are deliberately NOT included: that
 * would make fontconfig a build-time dependency, which this module
 * exists to avoid. The types below are FDK's own minimal view of the
 * ABI-stable subset used here (fs_-prefixed so an accidental later
 * #include <fontconfig.h> in this TU fails to compile instead of
 * silently mixing declarations). The values match fontconfig.h:
 * FcBool is int, FcMatchPattern == 0, FcResultMatch == 0, and the
 * property names are the well-known strings.
 */
typedef int fs_FcBool;
typedef unsigned char fs_FcChar8;
typedef struct _FcPattern fs_FcPattern;
typedef struct _FcConfig fs_FcConfig;
typedef struct _FcFontSet fs_FcFontSet;

enum { FS_FC_MATCH_PATTERN = 0 };
enum { FS_FC_RESULT_MATCH = 0 };

/* FcFontSet layout view (stable since fontconfig 2.0): only nfont and
 * the fonts array are read; sfont (allocated size) is carried to keep
 * the layout identical. */
typedef struct {
    int nfont;
    int sfont;
    fs_FcPattern **fonts;
} fs_FcFontSetView;

typedef struct {
    fs_FcBool (*FcInit)(void);
    fs_FcPattern *(*FcPatternCreate)(void);
    void (*FcPatternDestroy)(fs_FcPattern *p);
    fs_FcBool (*FcPatternAddString)(fs_FcPattern *p, const char *object,
                                    const fs_FcChar8 *value);
    fs_FcBool (*FcPatternAddBool)(fs_FcPattern *p, const char *object,
                                  fs_FcBool value);
    fs_FcBool (*FcConfigSubstitute)(fs_FcConfig *config,
                                    fs_FcPattern *p, int kind);
    void (*FcDefaultSubstitute)(fs_FcPattern *p);
    fs_FcFontSet *(*FcFontSort)(fs_FcConfig *config, fs_FcPattern *p,
                                fs_FcBool trim, void *csp, int *result);
    void (*FcFontSetDestroy)(fs_FcFontSet *s);
    fs_FcBool (*FcPatternGetString)(fs_FcPattern *p, const char *object,
                                    int id, fs_FcChar8 **out);
    fs_FcBool (*FcPatternGetInteger)(fs_FcPattern *p, const char *object,
                                     int id, int *out);
} fs_fc_api;

static fs_fc_api g_fc;     /* zeroed until resolved */
static void *g_fc_lib;     /* dlopen handle, kept for the process */
static bool g_fc_tried;

/* dlopen + dlsym the API table. Any missing symbol disables the whole
 * stage (an ancient or exotic fontconfig is treated as absent). The
 * void*-to-function-pointer conversion goes through memcpy — the only
 * warning-clean way under -Wpedantic. */
static bool fs_fc_load(void) {
    if (g_fc_tried) {
        return g_fc_lib != NULL;
    }
    g_fc_tried = true;

    static const char *const names[] = {"libfontconfig.so.1",
                                        "libfontconfig.so", NULL};
    for (int i = 0; names[i] != NULL && g_fc_lib == NULL; i++) {
        g_fc_lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (g_fc_lib == NULL) {
        return false;
    }

    static const struct {
        const char *name;
        void **slot;
    } syms[] = {
        {"FcInit", (void **)&g_fc.FcInit},
        {"FcPatternCreate", (void **)&g_fc.FcPatternCreate},
        {"FcPatternDestroy", (void **)&g_fc.FcPatternDestroy},
        {"FcPatternAddString", (void **)&g_fc.FcPatternAddString},
        {"FcPatternAddBool", (void **)&g_fc.FcPatternAddBool},
        {"FcConfigSubstitute", (void **)&g_fc.FcConfigSubstitute},
        {"FcDefaultSubstitute", (void **)&g_fc.FcDefaultSubstitute},
        {"FcFontSort", (void **)&g_fc.FcFontSort},
        {"FcFontSetDestroy", (void **)&g_fc.FcFontSetDestroy},
        {"FcPatternGetString", (void **)&g_fc.FcPatternGetString},
        {"FcPatternGetInteger", (void **)&g_fc.FcPatternGetInteger},
    };
    for (size_t i = 0; i < sizeof(syms) / sizeof(syms[0]); i++) {
        void *sym = dlsym(g_fc_lib, syms[i].name);
        if (sym == NULL) {
            /* Incomplete fontconfig: disable the stage permanently. */
            memset(&g_fc, 0, sizeof(g_fc));
            /* Not dlclose()-ing: see the file header. */
            return false;
        }
        memcpy(syms[i].slot, &sym, sizeof(sym));
    }
    return true;
}

/* Walks fontconfig's best-match list for the generic sans-serif
 * family under the user's own fontconfig policy, and returns the
 * first candidate that passes the TrueType-flavor gate. CFF faces
 * (Cantarell on GNOME, URW fonts from ghostscript, ...) are skipped
 * by that gate, which is exactly why the walk does not stop at the
 * first entry. The walk is capped so a pathological sort result
 * cannot pin the process. */
static bool fs_try_fontconfig(char **out_path, long *out_face);
static bool fs_fontconfig_walk(char **out_path, long *out_face);

static bool fs_try_fontconfig(char **out_path, long *out_face) {
    if (!fs_fc_load()) {
        return false;
    }

    /* See the LeakSanitizer note above: everything fontconfig
     * allocates in here is process-lifetime state by design. */
    fs_lsan_off();
    bool found = false;
    if (g_fc.FcInit()) {
        fs_lsan_on();
        found = fs_fontconfig_walk(out_path, out_face);
    } else {
        fs_lsan_on();
    }
    return found;
}

/* The pattern/sort walk, split out so the LSan bracket above stays
 * minimal and readable. */
static bool fs_fontconfig_walk(char **out_path, long *out_face) {
    fs_FcPattern *pat = g_fc.FcPatternCreate();
    if (pat == NULL) {
        return false;
    }
    (void)g_fc.FcPatternAddString(pat, "family",
                                  (const fs_FcChar8 *)"sans-serif");
    (void)g_fc.FcPatternAddBool(pat, "scalable", 1);
    (void)g_fc.FcConfigSubstitute(NULL, pat, FS_FC_MATCH_PATTERN);
    g_fc.FcDefaultSubstitute(pat);

    int result = 0;
    fs_FcFontSet *set = g_fc.FcFontSort(NULL, pat, 1, NULL, &result);
    g_fc.FcPatternDestroy(pat);
    if (set == NULL) {
        return false;
    }

    const fs_FcFontSetView *view = (const fs_FcFontSetView *)set;
    int n = view->nfont;
    if (n < 0) {
        n = 0;
    }
    if (n > 32) {
        n = 32;
    }

    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        fs_FcChar8 *file = NULL;
        int index = 0;
        if (g_fc.FcPatternGetString(view->fonts[i], "file", 0,
                                    &file) != FS_FC_RESULT_MATCH ||
            file == NULL) {
            continue;
        }
        (void)g_fc.FcPatternGetInteger(view->fonts[i], "index", 0,
                                       &index);
        long face = 0;
        if (fs_file_has_face((const char *)file, (long)index, &face)) {
            char *dup = fs_strdup((const char *)file);
            if (dup != NULL) {
                *out_path = dup;
                *out_face = face;
                found = true;
            }
        }
    }
    g_fc.FcFontSetDestroy(set);
    return found;
}

/* ---- stage 4: known exact paths -------------------------------------- */
/* The historical candidate list. Kept because it is cheap and fully
 * deterministic on the layouts it covers (Debian, Ubuntu, and the
 * Arch/conda-style /usr/share/fonts/TTF). Everything else is the
 * scanner's job. */
static const char *const k_known_paths[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    NULL,
};

static bool fs_try_known_paths(char **out_path, long *out_face) {
    for (int i = 0; k_known_paths[i] != NULL; i++) {
        long face = 0;
        if (fs_file_has_face(k_known_paths[i], 0, &face)) {
            char *dup = fs_strdup(k_known_paths[i]);
            if (dup != NULL) {
                *out_path = dup;
                *out_face = face;
                return true;
            }
        }
    }
    return false;
}

/* ---- stages 2 & 5: ranked directory scan ----------------------------- */

#define FS_SCAN_MAX_DEPTH 4
#define FS_SCAN_MAX_CANDIDATES 8
#define FS_SCAN_NAME_MAX 256

typedef struct {
    char *path;   /* fdk_alloc'd */
    int score;    /* lower is better */
} fs_candidate;

typedef struct {
    fs_candidate cand[FS_SCAN_MAX_CANDIDATES];
    int count;
} fs_candidates;

/* Inserts (path, score) into the ranked set, keeping at most
 * FS_SCAN_MAX_CANDIDATES entries ordered by (score, strcmp). The cap
 * bounds memory on huge font trees (LibreOffice ships hundreds of
 * faces) while leaving enough depth that a tree whose best-looking
 * files fail the TrueType gate still resolves. */
static void fs_add_candidate(fs_candidates *cs, const char *path,
                             int score) {
    int slot;
    if (cs->count < FS_SCAN_MAX_CANDIDATES) {
        slot = cs->count;
        cs->count++;
    } else {
        int worst = cs->count - 1;
        if (score > cs->cand[worst].score ||
            (score == cs->cand[worst].score &&
             strcmp(path, cs->cand[worst].path) > 0)) {
            return; /* worse than everything held */
        }
        fdk_free(cs->cand[worst].path);
        slot = worst;
    }
    while (slot > 0 &&
           (cs->cand[slot - 1].score > score ||
            (cs->cand[slot - 1].score == score &&
             strcmp(cs->cand[slot - 1].path, path) > 0))) {
        cs->cand[slot] = cs->cand[slot - 1];
        slot--;
    }
    cs->cand[slot].path = fs_strdup(path);
    cs->cand[slot].score = score;
    if (cs->cand[slot].path == NULL) {
        /* Allocation failure: drop the slot and compact. */
        memmove(&cs->cand[slot], &cs->cand[slot + 1],
                (size_t)(cs->count - slot - 1) * sizeof(fs_candidate));
        cs->count--;
    }
}

/* Ranks a font FILENAME (lowercased) for suitability as the default
 * UI face. Returns the score (lower is better) or -1 when the name
 * does not name one of the known default-UI families.
 *
 * Tier 0 are the common Latin desktop sans faces (the same set the
 * old path list probed, plus the GNOME and Ubuntu defaults). Tier 1
 * is Noto Sans SC / Ubuntu under their legacy names — full Latin
 * coverage, kept behind the dedicated Latin faces. Tier 2 is the
 * CJK last resort. A family prefix only matches at a name boundary
 * ('-', '.', '[', or end) so e.g. "NotoSansDisplay" does not count
 * as Noto Sans.
 *
 * Style suffix handling prefers the regular/default instance: an
 * exact family file, a variable-font bracket name (the default
 * instance IS regular), and "-regular"/"-book" rank best; anything
 * mentioning a bold/black/light/condensed/... variant ranks worst,
 * because FDK synthesizes those styles itself. */
static int fs_score_name(const char *lower) {
    static const struct {
        const char *prefix;
        int tier;
    } families[] = {
        {"dejavusans", 0},     {"notosans", 0},
        {"liberationsans", 0}, {"freesans", 0},
        {"cantarell", 0},      {"adwaitasans", 0},
        {"ubuntusans", 0},
        {"notosanssc", 1},     {"ubuntu", 1},
        {"wenquanyi", 2},      {"lxgw", 2},
    };
    static const char *const style_penalized[] = {
        "bold",  "black", "heavy", "light", "thin", "italic",
        "oblique", "semibold", "extralight", "condensed", "expanded",
    };

    for (size_t i = 0; i < sizeof(families) / sizeof(families[0]);
         i++) {
        size_t n = strlen(families[i].prefix);
        if (strncmp(lower, families[i].prefix, n) != 0) {
            continue;
        }
        char c = lower[n];
        if (c != '\0' && c != '-' && c != '.' && c != '[') {
            continue; /* longer family name (Display, Mono, ...) */
        }
        int style;
        if (c == '\0' || c == '.') {
            style = 0; /* exact family file */
        } else if (c == '[') {
            style = 0; /* variable font, default instance = regular */
        } else {
            const char *rest = lower + n;
            if (strncmp(rest, "-regular", 8) == 0 ||
                strncmp(rest, "-book", 5) == 0) {
                style = 0;
            } else {
                style = 2;
                for (size_t j = 0;
                     j < sizeof(style_penalized) /
                             sizeof(style_penalized[0]);
                     j++) {
                    if (strstr(rest, style_penalized[j]) != NULL) {
                        style = 4;
                        break;
                    }
                }
            }
        }
        return families[i].tier * 8 + style;
    }
    return -1;
}

static bool fs_name_is_ttf(const char *lower, size_t n) {
    if (n < 4) {
        return false;
    }
    const char *ext = lower + n - 4;
    return strcmp(ext, ".ttf") == 0 || strcmp(ext, ".ttc") == 0;
}

/* Recursive scan of one directory. Depth is capped; hidden entries,
 * sockets, and fifos are skipped. Classification is a plain stat()
 * per entry — deliberately NOT dirent.d_type, which is a
 * non-POSIX extension invisible under FDK's strict
 * -D_POSIX_C_SOURCE build flags, and which often reports
 * DT_UNKNOWN anyway. stat() follows symlinks: a link to a font file
 * counts (a common ~/.fonts setup), a link to a directory descends
 * (cycles are terminated by the depth cap). The whole scan runs
 * once per process, so a stat per entry is noise next to
 * fontconfig's own cache load. */
static void fs_scan_dir(const char *dir, int depth,
                        fs_candidates *out) {
    if (depth > FS_SCAN_MAX_DEPTH) {
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
        char full[PATH_MAX];
        int len = snprintf(full, sizeof(full), "%s/%s", dir,
                           e->d_name);
        if (len < 0 || (size_t)len >= sizeof(full)) {
            continue;
        }

        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            fs_scan_dir(full, depth + 1, out);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        char lower[FS_SCAN_NAME_MAX];
        size_t n = strlen(e->d_name);
        if (n >= sizeof(lower)) {
            continue;
        }
        for (size_t i = 0; i <= n; i++) {
            lower[i] = (char)tolower((unsigned char)e->d_name[i]);
        }
        if (!fs_name_is_ttf(lower, n)) {
            continue;
        }
        int score = fs_score_name(lower);
        if (score >= 0) {
            fs_add_candidate(out, full, score);
        }
    }
    closedir(d);
}

/* Scans every directory in the list, then validates the ranked
 * candidates in order until one passes the TrueType gate. */
static bool fs_scan_and_pick(const char *const *dirs, int ndirs,
                             char **out_path, long *out_face) {
    fs_candidates cs;
    memset(&cs, 0, sizeof(cs));
    for (int i = 0; i < ndirs; i++) {
        fs_scan_dir(dirs[i], 1, &cs);
    }
    bool found = false;
    for (int i = 0; i < cs.count && !found; i++) {
        long face = 0;
        if (fs_file_has_face(cs.cand[i].path, 0, &face)) {
            char *dup = fs_strdup(cs.cand[i].path);
            if (dup != NULL) {
                *out_path = dup;
                *out_face = face;
                found = true;
            }
        }
    }
    for (int i = 0; i < cs.count; i++) {
        fdk_free(cs.cand[i].path);
    }
    return found;
}

/* Stage 2: $FDK_FONT_DIRS — a ':'-separated list of directories the
 * user told FDK to prefer. Scanned before fontconfig because an
 * explicit environment override outranks system policy. */
static bool fs_try_user_dirs(char **out_path, long *out_face) {
    const char *v = getenv("FDK_FONT_DIRS");
    if (v == NULL || v[0] == '\0') {
        return false;
    }
    char *copy = fs_strdup(v);
    if (copy == NULL) {
        return false;
    }

    const char *dirs[32];
    int ndirs = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, ":", &save); tok != NULL;
         tok = strtok_r(NULL, ":", &save)) {
        if (tok[0] == '\0' || ndirs == 32) {
            continue;
        }
        dirs[ndirs++] = tok;
    }

    bool found = fs_scan_and_pick(dirs, ndirs, out_path, out_face);
    if (!found) {
        FDK_WARN("FDK_FONT_DIRS is set but yielded no usable "
                 "TrueType font — falling back to system discovery");
    }
    fdk_free(copy);
    return found;
}

/* Stage 5: the standard font roots, including the per-user ones. */
static bool fs_try_standard_dirs(char **out_path, long *out_face) {
    const char *dirs[6];
    int ndirs = 0;
    dirs[ndirs++] = "/usr/share/fonts";
    dirs[ndirs++] = "/usr/local/share/fonts";

    const char *home = getenv("HOME");
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        static char xdg_fonts[PATH_MAX];
        if (snprintf(xdg_fonts, sizeof(xdg_fonts), "%s/fonts", xdg) <
            (int)sizeof(xdg_fonts)) {
            dirs[ndirs++] = xdg_fonts;
        }
    } else if (home != NULL && home[0] != '\0') {
        static char home_fonts[PATH_MAX];
        if (snprintf(home_fonts, sizeof(home_fonts),
                     "%s/.local/share/fonts", home) <
            (int)sizeof(home_fonts)) {
            dirs[ndirs++] = home_fonts;
        }
    }
    if (home != NULL && home[0] != '\0') {
        static char dot_fonts[PATH_MAX];
        if (snprintf(dot_fonts, sizeof(dot_fonts), "%s/.fonts",
                     home) < (int)sizeof(dot_fonts)) {
            dirs[ndirs++] = dot_fonts;
        }
    }

    return fs_scan_and_pick(dirs, ndirs, out_path, out_face);
}

/* ---- cached entry point ---------------------------------------------- */

static char *g_font_path; /* fdk_alloc'd; NULL = not (yet) found */
static long g_font_face;
static bool g_resolved; /* true once probed, hit OR miss */

/* Paths whose full load failed (truncated repacks and corrupt files
 * can pass the tag-level gate but not the loader's complete
 * container validation — this container ships exactly such a
 * "variable" Noto whose directory claims 17 MB inside a 5 MB file).
 * Remembered so re-resolution skips them and the next-best candidate
 * gets the slot; bounded so a pathological environment cannot grow
 * it forever. */
#define FS_REJECT_MAX 8
static char *g_rejected[FS_REJECT_MAX];
static int g_reject_count;

static bool fs_is_rejected(const char *path) {
    for (int i = 0; i < g_reject_count; i++) {
        if (strcmp(g_rejected[i], path) == 0) {
            return true;
        }
    }
    return false;
}

bool fdk_text_resolve_system_font(const char **out_path,
                                  long *out_face) {
    if (out_path == NULL || out_face == NULL) {
        return false;
    }
    if (!g_resolved) {
        /* A previous winner may have been rejected since (see
         * fdk_text_font_discovery_reject) — its cached string is
         * still ours to free. */
        fdk_free(g_font_path);
        g_font_path = NULL;
        g_font_face = 0;
        char *path = NULL;
        long face = 0;
        const char *source = NULL;
        if (fs_try_env_file(&path, &face)) {
            source = "FDK_FONT_FILE";
        } else if (fs_try_user_dirs(&path, &face)) {
            source = "FDK_FONT_DIRS scan";
        } else if (fs_try_fontconfig(&path, &face)) {
            source = "fontconfig";
        } else if (fs_try_known_paths(&path, &face)) {
            source = "known path";
        } else if (fs_try_standard_dirs(&path, &face)) {
            source = "font scan";
        }
        if (path == NULL) {
            FDK_WARN("fdk_font_load_system_default: no usable system "
                     "font found — fontconfig had no usable face and "
                     "no candidate matched under the probed paths and "
                     "font directories. FDK bundles no font by "
                     "design: install a face such as DejaVu Sans or "
                     "Noto Sans, or point FDK_FONT_FILE at a "
                     ".ttf/.ttc file");
        } else {
            FDK_INFO("system font: %s (via %s)", path, source);
        }
        g_font_path = path;
        g_font_face = face;
        g_resolved = true;
    }
    *out_path = g_font_path;
    *out_face = g_font_face;
    return g_font_path != NULL;
}

void fdk_text_font_discovery_reject(const char *path) {
    if (path == NULL) {
        return;
    }
    if (!fs_is_rejected(path) && g_reject_count < FS_REJECT_MAX) {
        char *dup = fs_strdup(path);
        if (dup != NULL) {
            g_rejected[g_reject_count++] = dup;
        }
    }
    /* Force re-resolution: the stages will skip the rejected path and
     * surface the next-best candidate. */
    g_resolved = false;
}

void fdk_text_font_discovery_reset_for_tests(void) {
    fdk_free(g_font_path);
    g_font_path = NULL;
    g_font_face = 0;
    g_resolved = false;
    for (int i = 0; i < g_reject_count; i++) {
        fdk_free(g_rejected[i]);
        g_rejected[i] = NULL;
    }
    g_reject_count = 0;
}
