/*
 * icon_theme.c — the freedesktop icon-theme engine (internal)
 *
 * The toolkit's shared icon-resolution infrastructure: the file
 * dialogs (and any future internal surface that wants themed
 * icons) resolve raster icons through HERE, never by duplicating
 * lookup logic per consumer.
 *
 * What it implements, per the freedesktop Icon Theme Specification
 * (version 0.13) — to the extent FDK's renderer honestly supports:
 *
 *   - the XDG search path: $HOME/.icons, $XDG_DATA_HOME/icons,
 *     each $XDG_DATA_DIRS/icons entry, with /usr/share/pixmaps as
 *     the unthemed last resort;
 *   - index.theme parsing (the INI subset the spec defines):
 *     [Icon Theme] Name/Inherits/Hidden/Directories plus every
 *     per-directory section's Size/Type/Scale/MinSize/MaxSize/
 *     Threshold/Context;
 *   - theme INHERITANCE: the chain theme -> Inherits -> ... ->
 *     hicolor, cycle-guarded and depth-capped;
 *   - size matching per directory type (Fixed / Scalable /
 *     Threshold) with a strict pass (distance 0 only) and a
 *     documented relaxed fallback pass (closest nominal size) so
 *     sparse themes still serve something usable;
 *   - the user's selected theme, detected WITHOUT any bus or
 *     desktop daemon (the toolkit's no-D-Bus policy): $FDK_ICON_THEME
 *     (explicit override — also the hermetic test seam), the
 *     classic $XDG_CONFIG_HOME/icon-theme file, GTK3/GTK4
 *     settings.ini's gtk-icon-theme-name, KDE's kdeglobals
 *     [Icons] Theme, and the system default theme (the icon search
 *     path's own <base>/default/index.theme Inherits= line — the
 *     channel stock GNOME/XFCE systems actually set); hicolor is the
 *     honest default when the desktop says nothing;
 *   - caching at three layers (parsed index.theme data, name+size
 *     path lookups including negative results, decoded surfaces)
 *     with fdk__icon_theme_reset() as the single invalidation
 *     point — the file dialog calls it on every dialog creation,
 *     so a changed icon theme is picked up by the next dialog
 *     rather than served stale forever.
 *
 * Honest limitations, stated once here and repeated in the docs
 * rather than papered over:
 *
 *   - SVG icons are rasterized through the vendored nanosvg
 *     (third_party/nanosvg/): the icon-shaped subset of SVG — paths,
 *     transforms, groups, gradients, opacity, inline presentation
 *     attributes. SVGs that need features nanosvg deliberately does
 *     not implement (CSS <style> classes, <text>, filters, masks)
 *     produce no drawable coverage and are treated as MISSES — the
 *     chain keeps searching, and the final fallback stays the
 *     toolkit's own procedural glyph set, never a blank cell. XPM
 *     remains an honest miss (no decoder).
 *   - A same-directory probe prefers PNG (exact pixels, zero decode
 *     risk) and falls back to SVG in that directory; a Scalable
 *     directory's SVG rasterizes at the REQUESTED size, which is the
 *     spec's intent for scalable assets.
 *   - Scale>1 directories (the @2x HiDPI scheme) are skipped: the
 *     window layer composites the logical widget tree with exact
 *     integer scaling, so a logical-size icon behaves exactly like
 *     every procedural glyph already does.
 *   - Symbolic icons: a candidate name ending in "-symbolic" is
 *     tinted through the alpha mask with the current theme's text
 *     ink at decode time — the renderer-correct interpretation of
 *     the spec's symbolic intent, instead of shipping someone
 *     else's hardcoded black.
 *   - Path safety: icon names, theme names, and directory names
 *     are validated (no '/', no "..", bounded length) BEFORE they
 *     are ever glued into a filesystem path — a hostile settings
 *     file cannot steer the toolkit outside the icon search path.
 *
 * Memory discipline: every cache entry is fdk_alloc'd and freed on
 * reset; lookup results handed to callers are fdk_alloc'd copies
 * the caller frees; decoded surfaces are owned HERE (borrowed, and
 * invalidated only by reset — the reset contract is documented on
 * the function).
 */

#define FDK_LOG_TAG "theme"

#include "theme_internal.h"
#include "fdk/fdk_surface.h"
#include "fdk/fdk_theme.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <unistd.h>

/* ---- caps ------------------------------------------------------------ */

#define IT_NAME_MAX 96        /* icon/theme/dir name bytes + terminator */
#define IT_PATH_MAX 4096      /* PATH_MAX-safe scratch                    */
#define IT_LINE_MAX 65536     /* index.theme line cap (hicolor's real
                              * Directories= line is 11 KiB — the cap is a
                              * hostile-input bound, not a theme bound) */
#define IT_INDEX_MAX (2u * 1024u * 1024u) /* index.theme file size cap     */
#define IT_MAX_DIRS 2048      /* hostile-input bound on Directories
                              * entries parsed per theme (real themes
                              * reach ~1000; the array is heap-owned) */
#define IT_MAX_INHERITS 4     /* Inherits= names honored per theme        */
#define IT_CHAIN_DEPTH 16     /* theme chain walk cap (cycles + bombs)    */
#define IT_LOOKUP_CACHE 512   /* name+size results (positive and negative) */
#define IT_THEME_CACHE 16     /* parsed index.theme structs               */
#define IT_SURFACE_CACHE 256  /* decoded icon surfaces                    */
#define IT_REFUSED_MAX 32     /* paths that exist but will not raster —
                              * the decode-refusal set (shadowing guard:
                              * a corrupt PNG or an SVG full of features
                              * nanosvg cannot draw must not hide the
                              * next candidate in the chain)             */
#define IT_PIXMAPS_FALLBACK 1 /* /usr/share/pixmaps, flat                 */

/* Directory type values. */
#define IT_DIR_FIXED 0
#define IT_DIR_SCALABLE 1
#define IT_DIR_THRESHOLD 2

/* ---- validated-name helper ------------------------------------------ */

/* Icon/theme/directory names are untrusted strings (they come from
 * settings files and theme metadata). A usable name is non-empty,
 * fits IT_NAME_MAX, and contains no '/' and no "." or ".." segment
 * — precisely enough that gluing it into a search path can never
 * escape the search path. */
static bool it_name_ok(const char *s, size_t max) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = 0;
    while (s[n] != '\0') {
        if (s[n] == '/') {
            return false;
        }
        n++;
        if (n >= max) {
            return false;
        }
    }
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
        return false;
    }
    return true;
}

/* Directory names inside a theme's index.theme are RELATIVE
 * SUBPATHS — "16x16/places" is the spec's own vocabulary, so the
 * plain name rule (no slashes) does not apply. What must never pass:
 * absolute paths, empty segments, and "."/".." segments (traversal).
 * Found live against the REAL Adwaita and hicolor indexes. */
static bool it_dirname_ok(const char *s, size_t max) {
    if (s == NULL || s[0] == '\0' || s[0] == '/') {
        return false;
    }
    size_t n = 0;
    size_t seg = 0; /* current segment length */
    char prev = '\0';
    for (;;) {
        char c = s[n];
        if (c == '\0') {
            break;
        }
        n++;
        if (n >= max) {
            return false;
        }
        if (c == '/') {
            if (seg == 0) {
                return false; /* empty segment ("//" or trailing soon) */
            }
            if (seg == 1 && (prev == '.')) {
                return false; /* "." segment */
            }
            if (seg == 2 && prev == '.' && s[n - 2] == '.') {
                return false; /* ".." segment */
            }
            seg = 0;
            prev = '\0';
            continue;
        }
        seg++;
        prev = c;
    }
    if (seg == 0) {
        return false; /* trailing slash */
    }
    if (seg == 1 && prev == '.') {
        return false; /* trailing "." */
    }
    if (seg == 2 && prev == '.' && n >= 2 && s[n - 2] == '.') {
        return false; /* trailing ".." */
    }
    return true;
}

/* Copies at most max-1 bytes + terminator; returns false (and
 * writes an empty string) when the source is unusable. */
static bool it_name_copy(char *dst, size_t max, const char *src) {
    if (!it_name_ok(src, max)) {
        dst[0] = '\0';
        return false;
    }
    strcpy(dst, src);
    return true;
}

/* ---- the base-directory search path --------------------------------- */

/* Collects the icon base directories in XDG priority order into
 * out[] (each a full directory path); returns the count. The last
 * entry is the pixmaps fallback (flagged flat=1 by the caller's
 * convention: it is searched WITHOUT theme subdirectories). */
typedef struct {
    char path[IT_PATH_MAX];
    bool flat; /* /usr/share/pixmaps: unthemed icons only */
} it_base;

static size_t it_collect_bases(it_base *out, size_t max) {
    size_t n = 0;
    const char *home = getenv("HOME");

    /* 1. $HOME/.icons (the legacy user dir, spec-first). */
    if (home != NULL && home[0] != '\0' &&
        n < max && snprintf(out[n].path, sizeof(out[n].path),
                            "%s/.icons", home) < (int)sizeof(out[n].path)) {
        out[n].flat = false;
        n++;
    }

    /* 2. $XDG_DATA_HOME/icons (default ~/.local/share/icons). */
    const char *xdh = getenv("XDG_DATA_HOME");
    char def_xdh[IT_PATH_MAX];
    if (xdh == NULL || xdh[0] == '\0') {
        if (home != NULL && home[0] != '\0') {
            snprintf(def_xdh, sizeof(def_xdh), "%s/.local/share", home);
            xdh = def_xdh;
        }
    }
    if (xdh != NULL && xdh[0] != '\0' &&
        snprintf(out[n].path, sizeof(out[n].path), "%s/icons", xdh) <
            (int)sizeof(out[n].path)) {
        out[n].flat = false;
        n++;
    }

    /* 3. Each $XDG_DATA_DIRS/icons (default /usr/local/share:/usr/share). */
    const char *xdds = getenv("XDG_DATA_DIRS");
    if (xdds == NULL || xdds[0] == '\0') {
        xdds = "/usr/local/share:/usr/share";
    }
    char dirs[IT_PATH_MAX];
    snprintf(dirs, sizeof(dirs), "%s", xdds);
    char *save = NULL;
    for (char *tok = strtok_r(dirs, ":", &save); tok != NULL;
         tok = strtok_r(NULL, ":", &save)) {
        if (tok[0] == '\0' || n >= max - 1) {
            continue; /* reserve one slot for pixmaps below */
        }
        if (snprintf(out[n].path, sizeof(out[n].path), "%s/icons", tok) <
            (int)sizeof(out[n].path)) {
            out[n].flat = false;
            n++;
        }
    }

    /* 4. The unthemed pixmaps fallback, always last: the LAST data
     * dir's pixmaps/ — /usr/share/pixmaps on every real desktop
     * (the spec's literal path), hermetically overridable through
     * XDG_DATA_DIRS for the test rig. */
    if (n < max) {
        const char *last = "/usr/share";
        const char *xdds2 = getenv("XDG_DATA_DIRS");
        if (xdds2 != NULL && xdds2[0] != '\0') {
            /* The final non-empty entry. */
            const char *p = xdds2;
            const char *last_start = NULL;
            while (*p != '\0') {
                while (*p == ':') {
                    p++;
                }
                if (*p == '\0') {
                    break;
                }
                last_start = p;
                while (*p != '\0' && *p != ':') {
                    p++;
                }
            }
            if (last_start != NULL) {
                last = last_start; /* borrowed; copied below */
            }
        }
        if (snprintf(out[n].path, sizeof(out[n].path), "%s/pixmaps",
                     last) < (int)sizeof(out[n].path)) {
            out[n].flat = true;
            n++;
        }
    }
    return n;
}

/* ---- index.theme parsing -------------------------------------------- */

typedef struct {
    char name[192]; /* a relative subpath ("16x16/places"), validated */
    fdk_i32 size;
    fdk_i32 min_size;
    fdk_i32 max_size;
    fdk_i32 threshold;
    fdk_i32 scale;
    int type;
} it_dir;

typedef struct {
    char name[IT_NAME_MAX];
    char inherits[IT_MAX_INHERITS][IT_NAME_MAX];
    size_t inherit_count;
    it_dir *dirs;      /* heap-owned (real themes carry hundreds of
                        * directories — Papirus ~1000; a fixed cap
                        * would silently drop most of them) */
    size_t dir_count;
    bool valid; /* parsed an [Icon Theme] section with Directories */
} it_theme;

/* Reads the whole (bounded) file. Returns an fdk_alloc'd buffer with
 * a NUL terminator, or NULL (missing/oversized/unreadable). */
static char *it_read_index(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }
    if (st.st_size <= 0 ||
        (unsigned long long)st.st_size > IT_INDEX_MAX) {
        return NULL; /* empty is useless, huge is hostile */
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char *buf = fdk_alloc(len + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, len, f) != len) {
        fdk_free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

/* Trims leading/trailing ASCII whitespace in place; returns the
 * trimmed start. */
static char *it_trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Parses a signed i32 (clamped into [0, 1<<20]; negative/invalid
 * values clamp to 0 — sizes are always sane or defaulted). */
static fdk_i32 it_parse_i32(const char *v) {
    if (v == NULL || *v == '\0') {
        return 0;
    }
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) {
        return 0;
    }
    if (n < 0) {
        return 0;
    }
    if (n > (1L << 20)) {
        return (fdk_i32)(1L << 20);
    }
    return (fdk_i32)n;
}

/* The per-directory defaults and validation per the spec. */
static void it_dir_init(it_dir *d, const char *name) {
    memset(d, 0, sizeof(*d));
    if (it_dirname_ok(name, sizeof(d->name))) {
        snprintf(d->name, sizeof(d->name), "%s", name);
    } else {
        d->name[0] = '\0';
    }
    d->type = IT_DIR_THRESHOLD; /* the spec's default */
    d->threshold = 2;           /* the spec's default */
    d->scale = 1;               /* the spec's default */
}

static void it_dir_finish(it_dir *d) {
    if (d->size <= 0) {
        d->size = 16; /* Size= is required; a broken entry still
                        needs a sane nominal size to serve lookups */
    }
    if (d->type == IT_DIR_SCALABLE) {
        if (d->min_size <= 0) {
            d->min_size = d->size;
        }
        if (d->max_size <= 0 || d->max_size < d->min_size) {
            d->max_size = d->size < d->min_size ? d->min_size : d->size;
        }
    }
    if (d->threshold < 0) {
        d->threshold = 2;
    }
    if (d->scale <= 0) {
        d->scale = 1;
    }
}

/* Parses the index.theme at `path` into `out`. Returns false when
 * the file is missing, malformed beyond repair, or lacks the
 * [Icon Theme] section — in which case out->valid is false and the
 * theme contributes nothing but its name. */
static bool it_parse_index(const char *path, it_theme *out) {
    memset(out, 0, sizeof(*out));
    char *buf = it_read_index(path);
    if (buf == NULL) {
        return false;
    }

    /* Pre-seed a SCRATCH array of directory slots with the spec
     * defaults so the parse walk only has to override what the file
     * actually says; the finalize step moves the named subset into
     * the heap-owned array. */
    /* HEAP scratch (the hostile-input bound makes the arrays
     * ~800 KB — not stack food for a UI-thread parse). Freed on
     * every path below. */
    it_dir *scratch = fdk_alloc_array(IT_MAX_DIRS, sizeof(it_dir));
    char (*dir_names)[192] = fdk_alloc_array(IT_MAX_DIRS, 192u);
    if (scratch == NULL || dir_names == NULL) {
        fdk_free(scratch);
        fdk_free(dir_names);
        return false;
    }
    for (size_t i = 0; i < IT_MAX_DIRS; i++) {
        it_dir_init(&scratch[i], "");
    }

    /* The Directories= list is needed before the per-directory
     * sections make sense; collect it while walking. */
    size_t dir_name_count = 0;
    bool have_icon_theme_section = false;
    bool have_directories = false;

    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    char section[IT_NAME_MAX] = "";
    while (line != NULL) {
        char *s = it_trim(line);
        if (s[0] == '#' || s[0] == ';' || s[0] == '\0') {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        if (strlen(s) >= IT_LINE_MAX) {
            line = strtok_r(NULL, "\n", &save);
            continue; /* a hostile line is skipped, not obeyed */
        }
        if (s[0] == '[') {
            char *close = strchr(s, ']');
            if (close != NULL) {
                *close = '\0';
                /* Section names are directory names (slashes legal,
                 * traversal not) — except [Icon Theme] itself. */
                if (it_dirname_ok(s + 1, sizeof(section))) {
                    snprintf(section, sizeof(section), "%s", s + 1);
                } else {
                    section[0] = '\0';
                }
                if (strcmp(section, "Icon Theme") == 0) {
                    have_icon_theme_section = true;
                }
            }
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL) {
            line = strtok_r(NULL, "\n", &save);
            continue;
        }
        *eq = '\0';
        char *key = it_trim(s);
        char *val = it_trim(eq + 1);
        if (strcmp(section, "Icon Theme") == 0) {
            if (strcmp(key, "Inherits") == 0) {
                char *isave = NULL;
                for (char *tok = strtok_r(val, ",", &isave); tok != NULL;
                     tok = strtok_r(NULL, ",", &isave)) {
                    char *t = it_trim(tok);
                    if (out->inherit_count >= IT_MAX_INHERITS) {
                        break;
                    }
                    if (it_name_copy(out->inherits[out->inherit_count],
                                     IT_NAME_MAX, t)) {
                        out->inherit_count++;
                    }
                }
            } else if (strcmp(key, "Directories") == 0 ||
                       strcmp(key, "ScaledDirectories") == 0) {
                /* ScaledDirectories: @2x entries — parsed for
                 * completeness, but scale!=1 dirs never match. */
                if (strcmp(key, "Directories") == 0) {
                    have_directories = true;
                }
                char *dsave = NULL;
                for (char *tok = strtok_r(val, ",", &dsave); tok != NULL;
                     tok = strtok_r(NULL, ",", &dsave)) {
                    char *t = it_trim(tok);
                    if (dir_name_count >= IT_MAX_DIRS) {
                        break;
                    }
                    if (it_dirname_ok(t, IT_PATH_MAX)) {
                        snprintf(dir_names[dir_name_count],
                                 sizeof(dir_names[dir_name_count]), "%s",
                                 t);
                        dir_name_count++;
                    }
                }
            }
            /* Name/Comment/Hidden/Example carry no lookup behavior. */
        } else if (section[0] != '\0') {
            /* A per-directory section: fill any directory whose name
             * matches (first match wins; spec files list each once). */
            for (size_t i = 0; i < dir_name_count; i++) {
                if (strcmp(section, dir_names[i]) != 0) {
                    continue;
                }
                it_dir *d = &scratch[i];
                if (strcmp(key, "Size") == 0) {
                    d->size = it_parse_i32(val);
                } else if (strcmp(key, "Type") == 0) {
                    if (strcmp(val, "Fixed") == 0) {
                        d->type = IT_DIR_FIXED;
                    } else if (strcmp(val, "Scalable") == 0) {
                        d->type = IT_DIR_SCALABLE;
                    } else {
                        d->type = IT_DIR_THRESHOLD;
                    }
                } else if (strcmp(key, "MinSize") == 0) {
                    d->min_size = it_parse_i32(val);
                } else if (strcmp(key, "MaxSize") == 0) {
                    d->max_size = it_parse_i32(val);
                } else if (strcmp(key, "Threshold") == 0) {
                    d->threshold = it_parse_i32(val);
                } else if (strcmp(key, "Scale") == 0) {
                    d->scale = it_parse_i32(val);
                }
                /* Context/MaxDepth: metadata, no lookup effect. */
                break;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    fdk_free(buf);

    /* The walk wrote per-directory fields into out->dirs[i]
     * positionally; now bind the names and apply validation. */
    out->dir_count = 0;
    out->dirs = NULL;
    if (dir_name_count > 0) {
        out->dirs = fdk_alloc_array(dir_name_count, sizeof(it_dir));
        if (out->dirs == NULL) {
            fdk_free(scratch);
            fdk_free(dir_names);
            return false; /* OOM: the theme is unusable, honestly */
        }
        for (size_t i = 0; i < dir_name_count; i++) {
            /* Copy the parsed fields + bind the name (the walk wrote
             * positionally into scratch). */
            out->dirs[i] = scratch[i];
            snprintf(out->dirs[i].name, sizeof(out->dirs[i].name), "%s",
                     dir_names[i]);
            it_dir_finish(&out->dirs[i]);
        }
        out->dir_count = dir_name_count;
    }
    fdk_free(scratch);
    fdk_free(dir_names);

    out->valid = have_icon_theme_section && have_directories &&
                 out->dir_count > 0;
    return out->valid;
}

/* Frees a parsed theme's heap (the cache entries call this on
 * eviction and reset). A zeroed struct is a safe no-op. */
static void it_theme_release(it_theme *t) {
    if (t == NULL) {
        return;
    }
    fdk_free(t->dirs);
    t->dirs = NULL;
    t->dir_count = 0;
}

/* ---- the caches ------------------------------------------------------ */

/* Parsed themes, keyed by their base path + theme name. */
typedef struct {
    bool used;
    char base[IT_PATH_MAX];
    char theme[IT_NAME_MAX];
    it_theme data;
} it_theme_cache_entry;
static it_theme_cache_entry g_theme_cache[IT_THEME_CACHE];

/* Lookup results (positive AND negative — a negative cached once
 * keeps repeated misses off the filesystem), keyed (name, size). */
typedef struct {
    bool used;
    char name[IT_NAME_MAX];
    fdk_i32 size;
    char *path; /* fdk_alloc'd copy, NULL = known miss */
} it_lookup_entry;
static it_lookup_entry g_lookup_cache[IT_LOOKUP_CACHE];

/* Decoded surfaces, keyed (name, size) — the requested size, not
 * the decoded one. */
typedef struct {
    bool used;
    char name[IT_NAME_MAX];
    fdk_i32 size;
    fdk_surface *surf; /* owned; destroyed only by reset */
} it_surface_entry;
static it_surface_entry g_surface_cache[IT_SURFACE_CACHE];

/* The detected current theme name (empty until first read). */
static char g_current[IT_NAME_MAX];
static bool g_current_read = false;

static size_t g_surface_refused = 0; /* logged-once latch when full */

/* Paths that exist on disk but failed to decode — the decode-refusal
 * set. Probes skip them, so the chain walk falls through to the NEXT
 * candidate (another size, another theme, the procedural floor)
 * instead of letting one broken asset shadow everything behind it.
 * Bounded: a hostile theme cannot make FDK loop; the cap simply turns
 * further breakage into the honest miss. Cleared by reset. */
static char *g_refused[IT_REFUSED_MAX];
static size_t g_refused_count = 0;

static bool it_path_refused(const char *path) {
    for (size_t i = 0; i < g_refused_count; i++) {
        if (strcmp(g_refused[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* Returns false when the set is full (caller treats the candidate as
 * terminal: no progress possible, the honest miss). */
static bool it_refuse_add(const char *path) {
    if (g_refused_count >= IT_REFUSED_MAX) {
        return false;
    }
    char *copy = fdk__theme_strdup(path);
    if (copy == NULL) {
        return false; /* OOM: treat as terminal, same as full */
    }
    g_refused[g_refused_count++] = copy;
    return true;
}

/* ---- parsed-theme lookup (through the cache) ------------------------ */

/* Finds (parsing and caching on first use) the index.theme data for
 * `theme` rooted at the FIRST base directory that has one. Returns
 * NULL when no base carries the theme or none parses; on success
 * *out_base receives the index of the base it lives in (probes must
 * use the same base). The parsed copy in the cache is owned here. */
static const it_theme *it_theme_load(const it_base *bases, size_t base_count,
                                     const char *theme, size_t *out_base) {
    for (size_t b = 0; b < base_count; b++) {
        if (bases[b].flat) {
            continue; /* pixmaps has no theme structure */
        }
        char index_path[IT_PATH_MAX];
        if (snprintf(index_path, sizeof(index_path), "%s/%s/index.theme",
                     bases[b].path, theme) >= (int)sizeof(index_path)) {
            continue;
        }
        struct stat st;
        if (stat(index_path, &st) != 0) {
            continue; /* theme not in this base */
        }
        if (out_base != NULL) {
            *out_base = b;
        }
        /* Cache hit? */
        for (size_t i = 0; i < IT_THEME_CACHE; i++) {
            it_theme_cache_entry *e = &g_theme_cache[i];
            if (e->used && strcmp(e->base, bases[b].path) == 0 &&
                strcmp(e->theme, theme) == 0) {
                return e->data.valid ? &e->data : NULL;
            }
        }
        /* Parse + insert. A full cache recycles slot 0 — its parsed
         * directory array is heap-owned and MUST be released (the
         * borrow contract: parsed themes live only in the cache). */
        it_theme_cache_entry *slot = &g_theme_cache[0];
        for (size_t i = 0; i < IT_THEME_CACHE; i++) {
            if (!g_theme_cache[i].used) {
                slot = &g_theme_cache[i];
                break;
            }
        }
        if (slot->used) {
            it_theme_release(&slot->data);
        }
        slot->used = true;
        snprintf(slot->base, sizeof(slot->base), "%s", bases[b].path);
        snprintf(slot->theme, sizeof(slot->theme), "%s", theme);
        (void)it_parse_index(index_path, &slot->data);
        if (!slot->data.valid) {
            FDK_INFO("icon theme '%s' has an unusable index.theme "
                     "(missing [Icon Theme] or Directories); skipped",
                     theme);
            return NULL;
        }
        return &slot->data;
    }
    return NULL;
}

/* ---- size matching --------------------------------------------------- */

/* Distance from `want` to what the directory serves. 0 = a true
 * match (Fixed exact / Scalable in range / Threshold within
 * threshold). >0 = how far outside (the relaxed pass picks the
 * minimum). -1 = unusable (scale != 1: the HiDPI @2x scheme the
 * window layer's integer compositing makes pointless). */
static fdk_i32 it_dir_distance(const it_dir *d, fdk_i32 want) {
    if (d->scale != 1) {
        return -1;
    }
    fdk_i32 diff = want - d->size;
    if (diff < 0) {
        diff = -diff;
    }
    switch (d->type) {
    case IT_DIR_FIXED:
        return (want == d->size) ? 0 : diff;
    case IT_DIR_SCALABLE:
        if (want >= d->min_size && want <= d->max_size) {
            return 0;
        }
        if (want < d->min_size) {
            return d->min_size - want;
        }
        return want - d->max_size;
    default: /* IT_DIR_THRESHOLD */
        return (diff <= d->threshold) ? 0 : diff - d->threshold;
    }
}

/* ---- the probe ------------------------------------------------------- */

/* Does <base>/<theme>/<dir>/<icon>.<ext> exist as a regular file,
 * AND is it not on the decode-refusal set? PNG first (exact pixels,
 * the decode-safe path), then SVG (raster at the requested size via
 * the vendored nanosvg — see icon_svg.c for the policy layer and its
 * honest-miss rules). XPM is not probed: no decoder, never offered.
 * The refusal check here is what makes a broken asset TRANSPARENT to
 * the chain: the walk simply proceeds to the next candidate. */
static bool it_probe_file(const it_base *base, const char *theme,
                          const it_dir *dir, const char *icon,
                          const char *ext, char *out, size_t out_len) {
    char path[IT_PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s/%s/%s.%s", base->path,
                 theme, dir->name, icon, ext) >= (int)sizeof(path)) {
        return false;
    }
    if (it_path_refused(path)) {
        return false; /* exists but will not raster: skip */
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    snprintf(out, out_len, "%s", path);
    return true;
}

/* PNG-then-SVG probe in one directory. */
static bool it_probe_icon(const it_base *base, const char *theme,
                          const it_dir *dir, const char *icon,
                          char *out, size_t out_len) {
    if (it_probe_file(base, theme, dir, icon, "png", out, out_len)) {
        return true;
    }
    return it_probe_file(base, theme, dir, icon, "svg", out, out_len);
}

/* ---- the lookup ------------------------------------------------------ */

static const char *it_lookup_cache_get(const char *name, fdk_i32 size) {
    for (size_t i = 0; i < IT_LOOKUP_CACHE; i++) {
        if (g_lookup_cache[i].used &&
            g_lookup_cache[i].size == size &&
            strcmp(g_lookup_cache[i].name, name) == 0) {
            return g_lookup_cache[i].path;
        }
    }
    return NULL; /* not cached (caller distinguishes via a probe flag) */
}

static bool it_lookup_cached(const char *name, fdk_i32 size) {
    for (size_t i = 0; i < IT_LOOKUP_CACHE; i++) {
        if (g_lookup_cache[i].used &&
            g_lookup_cache[i].size == size &&
            strcmp(g_lookup_cache[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void it_lookup_cache_put(const char *name, fdk_i32 size,
                                const char *path) {
    it_lookup_entry *slot = NULL;
    for (size_t i = 0; i < IT_LOOKUP_CACHE; i++) {
        if (!g_lookup_cache[i].used) {
            slot = &g_lookup_cache[i];
            break;
        }
    }
    if (slot == NULL) {
        /* Full: recycle the first entry (path freed; the picker's
         * working set is far below the cap, this is belt+braces). */
        slot = &g_lookup_cache[0];
        fdk_free(slot->path);
        slot->used = false;
    }
    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->size = size;
    if (path != NULL) {
        slot->path = fdk__theme_strdup(path);
        if (slot->path == NULL) {
            slot->used = false; /* OOM: leave uncached, not broken */
        }
    } else {
        slot->path = NULL;
    }
}

/* Drops ONE (name,size) entry so the next lookup re-walks the chain
 * — the decode-refusal retry loop's lever: after a path is refused,
 * the cached positive that pointed at it must not be served again. */
static void it_lookup_cache_invalidate(const char *name, fdk_i32 size) {
    for (size_t i = 0; i < IT_LOOKUP_CACHE; i++) {
        if (g_lookup_cache[i].used &&
            g_lookup_cache[i].size == size &&
            strcmp(g_lookup_cache[i].name, name) == 0) {
            fdk_free(g_lookup_cache[i].path);
            g_lookup_cache[i].path = NULL;
            g_lookup_cache[i].used = false;
            return;
        }
    }
}

/* The chain walk. Returns an fdk_alloc'd path (caller frees) or
 * NULL when every candidate misses. */
static char *it_lookup(const char *icon_name, fdk_i32 size) {
    it_base bases[16];
    size_t base_count = it_collect_bases(bases, 16);

    /* The chain: current -> inherits* -> hicolor (the spec's
     * terminal), cycle-guarded and depth-capped. */
    const char *chain[IT_CHAIN_DEPTH];
    size_t chain_len = 0;
    char current[IT_NAME_MAX];
    snprintf(current, sizeof(current), "%s",
             fdk__icon_theme_current_name());
    chain[chain_len++] = current; /* borrowed static, copied below */

    /* Breadth-first over the chain AS IT GROWS (depth < chain_len,
     * re-evaluated every iteration — the earlier `depth < IT_CHAIN_
     * DEPTH` form read UNINITIALIZED chain slots past chain_len and
     * fed garbage theme names to the loader: caught by the test
     * suite's repeated-run sweep). */
    for (size_t depth = 0; depth < chain_len; depth++) {
        const it_theme *t =
            it_theme_load(bases, base_count, chain[depth], NULL);
        if (t == NULL) {
            continue; /* a broken link is skipped; siblings still
                       * expand — inheritance is a DAG, not a list */
        }
        for (size_t i = 0; i < t->inherit_count && chain_len < IT_CHAIN_DEPTH;
             i++) {
            bool dup = false;
            for (size_t j = 0; j < chain_len; j++) {
                if (strcmp(chain[j], t->inherits[i]) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                chain[chain_len++] = t->inherits[i];
            }
        }
    }
    /* hicolor terminates every chain (when not already there). */
    bool have_hicolor = false;
    for (size_t j = 0; j < chain_len; j++) {
        if (strcmp(chain[j], "hicolor") == 0) {
            have_hicolor = true;
            break;
        }
    }
    if (!have_hicolor && chain_len < IT_CHAIN_DEPTH) {
        chain[chain_len++] = "hicolor";
    }

    /* Pass 1 (strict): a distance-0 directory match. Pass 2
     * (relaxed): the closest nominal size — sparse themes and
     * fixture tests still serve usable icons instead of nothing. */
    for (int pass = 0; pass < 2; pass++) {
        for (size_t c = 0; c < chain_len; c++) {
            size_t theme_base = 0;
            const it_theme *t =
                it_theme_load(bases, base_count, chain[c], &theme_base);
            if (t == NULL) {
                continue;
            }
            fdk_i32 best_dist = -1;
            char best_path[IT_PATH_MAX];
            best_path[0] = '\0';
            for (size_t d = 0; d < t->dir_count; d++) {
                fdk_i32 dist = it_dir_distance(&t->dirs[d], size);
                if (dist < 0) {
                    continue;
                }
                if (pass == 0 && dist != 0) {
                    continue;
                }
                if (pass == 1 && best_dist >= 0 && dist >= best_dist) {
                    continue;
                }
                char probe[IT_PATH_MAX];
                if (it_probe_icon(&bases[theme_base], chain[c], &t->dirs[d],
                                  icon_name, probe, sizeof(probe))) {
                    best_dist = dist;
                    snprintf(best_path, sizeof(best_path), "%s", probe);
                    if (pass == 0) {
                        break; /* strict pass: first true match wins */
                    }
                }
            }
            if (best_path[0] != '\0') {
                return fdk__theme_strdup(best_path);
            }
        }
    }

    /* The unthemed pixmaps last resort. */
    for (size_t b = 0; b < base_count; b++) {
        if (!bases[b].flat) {
            continue;
        }
        static const char *const exts[] = { "png", "svg" };
        for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
            char path[IT_PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s.%s", bases[b].path,
                         icon_name, exts[e]) < (int)sizeof(path)) {
                if (it_path_refused(path)) {
                    continue;
                }
                struct stat st;
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    return fdk__theme_strdup(path);
                }
            }
        }
    }
    return NULL;
}

/* ---- the public-internal API ----------------------------------------- */

char *fdk__icon_theme_lookup(const char *icon_name, fdk_i32 size) {
    if (!it_name_ok(icon_name, IT_NAME_MAX) || size <= 0 || size > 4096) {
        return NULL;
    }
    if (it_lookup_cached(icon_name, size)) {
        const char *cached = it_lookup_cache_get(icon_name, size);
        return cached == NULL ? NULL : fdk__theme_strdup(cached);
    }
    char *path = it_lookup(icon_name, size);
    it_lookup_cache_put(icon_name, size, path);
    return path;
}

const char *fdk__icon_theme_current_name(void) {
    if (g_current_read) {
        return g_current;
    }
    g_current_read = true;
    g_current[0] = '\0';

    /* 1. The explicit override — power users and the hermetic test
     *    seam (fixtures point XDG_* at a scratch tree and name the
     *    fixture theme here). */
    const char *env = getenv("FDK_ICON_THEME");
    if (env != NULL && it_name_copy(g_current, sizeof(g_current), env)) {
        return g_current;
    }

    /* The config-root scratch: XDG_CONFIG_HOME or ~/.config. */
    const char *home = getenv("HOME");
    char cfg[IT_PATH_MAX];
    const char *xdch = getenv("XDG_CONFIG_HOME");
    if (xdch != NULL && xdch[0] != '\0') {
        snprintf(cfg, sizeof(cfg), "%s", xdch);
    } else if (home != NULL && home[0] != '\0') {
        snprintf(cfg, sizeof(cfg), "%s/.config", home);
    } else {
        cfg[0] = '\0';
    }

    /* 2. The classic single-line icon-theme file (GNOME 2 era,
     *    still honored by several desktops). */
    if (cfg[0] != '\0') {
        char path[IT_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/icon-theme", cfg) <
            (int)sizeof(path)) {
            char *buf = it_read_index(path);
            if (buf != NULL) {
                char *name = it_trim(strtok(buf, "\n"));
                if (it_name_copy(g_current, sizeof(g_current), name)) {
                    fdk_free(buf);
                    return g_current;
                }
                fdk_free(buf);
            }
        }
    }

    /* 3/4. GTK3/GTK4 settings.ini: [Settings] gtk-icon-theme-name.
     * No bus, no daemon — the file IS the desktop's statement. */
    static const char *const gtk_confs[] = {
        "gtk-3.0/settings.ini",
        "gtk-4.0/settings.ini",
    };
    for (size_t i = 0; i < sizeof(gtk_confs) / sizeof(gtk_confs[0]);
         i++) {
        if (cfg[0] == '\0') {
            break;
        }
        char path[IT_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", cfg, gtk_confs[i]) >=
            (int)sizeof(path)) {
            continue;
        }
        char *buf = it_read_index(path);
        if (buf == NULL) {
            continue;
        }
        char *save = NULL;
        char *line = strtok_r(buf, "\n", &save);
        bool in_settings = false;
        while (line != NULL) {
            char *s = it_trim(line);
            if (s[0] == '[') {
                in_settings = strncmp(s, "[Settings", 9) == 0;
            } else if (in_settings) {
                char *eq = strchr(s, '=');
                if (eq != NULL) {
                    *eq = '\0';
                    char *key = it_trim(s);
                    if (strcmp(key, "gtk-icon-theme-name") == 0) {
                        char *val = it_trim(eq + 1);
                        /* GTK tolerates quoted values. */
                        size_t vl = strlen(val);
                        if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
                            val[vl - 1] = '\0';
                            val++;
                        }
                        if (it_name_copy(g_current, sizeof(g_current),
                                         val)) {
                            fdk_free(buf);
                            return g_current;
                        }
                    }
                }
            }
            line = strtok_r(NULL, "\n", &save);
        }
        fdk_free(buf);
    }

    /* 5. KDE: kdeglobals [Icons] Theme=. */
    if (cfg[0] != '\0') {
        char path[IT_PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/kdeglobals", cfg) <
            (int)sizeof(path)) {
            char *buf = it_read_index(path);
            if (buf != NULL) {
                char *save = NULL;
                char *line = strtok_r(buf, "\n", &save);
                bool in_icons = false;
                while (line != NULL) {
                    char *s = it_trim(line);
                    if (s[0] == '[') {
                        in_icons = strncmp(s, "[Icons", 6) == 0;
                    } else if (in_icons) {
                        char *eq = strchr(s, '=');
                        if (eq != NULL) {
                            *eq = '\0';
                            char *key = it_trim(s);
                            if (strcmp(key, "Theme") == 0 &&
                                it_name_copy(g_current, sizeof(g_current),
                                              it_trim(eq + 1))) {
                                fdk_free(buf);
                                return g_current;
                            }
                        }
                    }
                    line = strtok_r(NULL, "\n", &save);
                }
                fdk_free(buf);
            }
        }
    }

    /* 6. The system default theme: the icon search path's own
     *    <base>/default/index.theme, [Icon Theme] Inherits=. This is
     *    the channel stock GNOME/XFCE systems actually publish (set
     *    by the distro or update-alternatives; gsettings itself is
     *    D-Bus and out of policy reach). First base in XDG priority
     *    order wins — a user-level ~/.icons/default outranks the
     *    system one, mirroring every other base-resolved setting. */
    {
        it_base bases[16];
        size_t base_count = it_collect_bases(bases, 16);
        for (size_t b = 0; b < base_count; b++) {
            if (bases[b].flat) {
                continue;
            }
            char path[IT_PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/default/index.theme",
                         bases[b].path) >= (int)sizeof(path)) {
                continue;
            }
            char *buf = it_read_index(path);
            if (buf == NULL) {
                continue;
            }
            char *save = NULL;
            char *line = strtok_r(buf, "\n", &save);
            bool in_icon_theme = false;
            while (line != NULL) {
                char *s = it_trim(line);
                if (s[0] == '[') {
                    in_icon_theme = strncmp(s, "[Icon Theme", 11) == 0;
                } else if (in_icon_theme) {
                    char *eq = strchr(s, '=');
                    if (eq != NULL) {
                        *eq = '\0';
                        char *key = it_trim(s);
                        if (strcmp(key, "Inherits") == 0) {
                            /* Comma-separated list; the first entry is
                             * the default theme. */
                            char *val = it_trim(eq + 1);
                            char *comma = strchr(val, ',');
                            if (comma != NULL) {
                                *comma = '\0';
                            }
                            if (it_name_copy(g_current, sizeof(g_current),
                                             it_trim(val))) {
                                fdk_free(buf);
                                return g_current;
                            }
                        }
                    }
                }
                line = strtok_r(NULL, "\n", &save);
            }
            fdk_free(buf);
        }
    }

    /* 7. The spec's universal default. */
    snprintf(g_current, sizeof(g_current), "hicolor");
    return g_current;
}

/* Symbolic tint: the alpha mask kept, the color replaced with the
 * current FDK theme's text ink — the renderer-correct reading of
 * the spec's symbolic intent. */
static void it_tint_symbolic(fdk_surface *surf) {
    fdk_surface_info info;
    if (fdk_surface_get_info(surf, &info) != FDK_OK) {
        return;
    }
    fdk_color ink = fdk_theme_get_color(NULL, FDK_TK_TEXT);
    fdk_u32 r = (fdk_u32)(ink.r * 255.0f + 0.5f);
    fdk_u32 g = (fdk_u32)(ink.g * 255.0f + 0.5f);
    fdk_u32 b = (fdk_u32)(ink.b * 255.0f + 0.5f);
    if (r > 255u) { r = 255u; }
    if (g > 255u) { g = 255u; }
    if (b > 255u) { b = 255u; }
    fdk_u32 rgb = (r << 16) | (g << 8) | b;
    for (fdk_i32 y = 0; y < info.height; y++) {
        fdk_u32 *row = info.pixels + (size_t)y * (size_t)info.stride;
        for (fdk_i32 x = 0; x < info.width; x++) {
            fdk_u32 px = row[x];
            if ((px & 0xFF000000u) != 0u) {
                row[x] = (px & 0xFF000000u) | rgb;
            }
        }
    }
}

fdk_surface *fdk__icon_theme_surface(const char *icon_name, fdk_i32 size) {
    if (!it_name_ok(icon_name, IT_NAME_MAX) || size <= 0 || size > 4096) {
        return NULL;
    }
    for (size_t i = 0; i < IT_SURFACE_CACHE; i++) {
        if (g_surface_cache[i].used &&
            g_surface_cache[i].size == size &&
            strcmp(g_surface_cache[i].name, icon_name) == 0) {
            return g_surface_cache[i].surf; /* NULL = known miss */
        }
    }
    /* Find (or refuse) a slot BEFORE decoding: a full cache must
     * not decode-then-leak. */
    it_surface_entry *slot = NULL;
    for (size_t i = 0; i < IT_SURFACE_CACHE; i++) {
        if (!g_surface_cache[i].used) {
            slot = &g_surface_cache[i];
            break;
        }
    }
    if (slot == NULL) {
        if (g_surface_refused == 0) {
            FDK_WARN("icon surface cache full (%d distinct icons); "
                     "further icons fall back to the procedural glyphs",
                     IT_SURFACE_CACHE);
            g_surface_refused = 1;
        }
        return NULL;
    }
    slot->used = true;
    snprintf(slot->name, sizeof(slot->name), "%s", icon_name);
    slot->size = size;
    slot->surf = NULL;

    /* The decode-refusal retry loop. A path that exists but will not
     * raster (corrupt PNG; an SVG built from features nanosvg cannot
     * draw) is REFUSED — added to the refusal set, its (name,size)
     * lookup cache entry dropped — and the search restarts, now
     * skipping that path. Without this, one broken asset in the
     * highest-priority theme would shadow every valid icon behind it
     * (the parent theme's PNG, hicolor, ...) and pin the procedural
     * glyph instead. The loop is bounded: every iteration either
     * decodes, refuses one DISTINCT path (proven progress), or hits
     * the refusal-set cap / a true miss — at most IT_REFUSED_MAX + 1
     * lookups for one (name,size), ever. */
    for (size_t attempt = 0; attempt <= IT_REFUSED_MAX; attempt++) {
        it_lookup_cache_invalidate(icon_name, size);
        char *path = fdk__icon_theme_lookup(icon_name, size);
        if (path == NULL) {
            break; /* every candidate probed and missed: honest miss */
        }
        fdk_surface *surf = NULL;
        size_t pl = strlen(path);
        bool is_svg = pl >= 4 && strcmp(path + pl - 4, ".svg") == 0;
        if (is_svg) {
            /* SVG rasterizes at the REQUESTED size — the size passed
             * to this function, not any size declared inside the
             * file. */
            (void)fdk__icon_svg_raster(path, size, &surf);
        } else {
            (void)fdk_surface_create_from_image(path, &surf);
        }
        if (surf != NULL) {
            fdk_free(path);
            size_t nl = strlen(icon_name);
            if (nl > 9 && strcmp(icon_name + nl - 9, "-symbolic") == 0) {
                it_tint_symbolic(surf);
            }
            slot->surf = surf;
            return surf;
        }
        /* Undecodable candidate: refuse it and let the chain walk
         * fall through to the next one. */
        FDK_INFO("icon '%s' at %d px exists but will not raster; "
                 "refusing '%s' and continuing the chain",
                 icon_name, size, path);
        bool added = it_refuse_add(path);
        fdk_free(path);
        if (!added) {
            break; /* refusal set full: no progress possible */
        }
    }
    return NULL; /* total miss, negative result now cached */
}

fdk_surface *fdk__icon_theme_surface_chain(const char *const *names,
                                           fdk_i32 size) {
    if (names == NULL) {
        return NULL;
    }
    for (size_t i = 0; names[i] != NULL; i++) {
        fdk_surface *s = fdk__icon_theme_surface(names[i], size);
        if (s != NULL) {
            return s;
        }
    }
    return NULL;
}

void fdk__icon_theme_reset(void) {
    for (size_t i = 0; i < IT_LOOKUP_CACHE; i++) {
        fdk_free(g_lookup_cache[i].path);
        g_lookup_cache[i].path = NULL;
        g_lookup_cache[i].used = false;
    }
    for (size_t i = 0; i < IT_THEME_CACHE; i++) {
        if (g_theme_cache[i].used) {
            it_theme_release(&g_theme_cache[i].data);
        }
        g_theme_cache[i].used = false;
    }
    for (size_t i = 0; i < IT_SURFACE_CACHE; i++) {
        if (g_surface_cache[i].used && g_surface_cache[i].surf != NULL) {
            fdk_surface_destroy(g_surface_cache[i].surf);
        }
        g_surface_cache[i].used = false;
        g_surface_cache[i].surf = NULL;
    }
    for (size_t i = 0; i < g_refused_count; i++) {
        fdk_free(g_refused[i]);
        g_refused[i] = NULL;
    }
    g_refused_count = 0;
    g_current[0] = '\0';
    g_current_read = false;
    g_surface_refused = 0;
}

/* ---- file-type classification ---------------------------------------- */

/* The candidate chains, in preference order, every one ending in a
 * generic that virtually every real theme carries. The LAST resort
 * after the chain misses entirely is the toolkit's own procedural
 * glyph (the caller's fallback), never a wrong-type icon.
 *
 * Names follow the freedesktop icon naming spec + the names GTK's
 * shared-mime-info conventionally maps to; chains exist precisely
 * because themes disagree about which specific names they ship. */
static const char *const k_chain_folder[] = { "folder", NULL };
static const char *const k_chain_image[] = { "image-x-generic", NULL };
static const char *const k_chain_audio[] = { "audio-x-generic", NULL };
static const char *const k_chain_video[] = { "video-x-generic", NULL };
static const char *const k_chain_zip[] = {
    "application-zip", "application-x-archive", "package-x-generic", NULL };
static const char *const k_chain_tar[] = {
    "application-x-tar", "application-x-archive", "package-x-generic",
    NULL };
static const char *const k_chain_compressed[] = {
    "application-x-compressed-tar", "application-x-archive",
    "package-x-generic", NULL };
static const char *const k_chain_executable[] = {
    "application-x-executable", "application-x-generic", NULL };
static const char *const k_chain_text[] = { "text-x-generic", NULL };
static const char *const k_chain_markdown[] = {
    "text-x-markdown", "text-x-generic", NULL };
static const char *const k_chain_pdf[] = {
    "application-pdf", "application-x-generic", NULL };
static const char *const k_chain_html[] = {
    "text-html", "text-x-generic", NULL };
static const char *const k_chain_font[] = { "font-x-generic", NULL };
static const char *const k_chain_desktop[] = {
    "application-x-desktop", NULL };
static const char *const k_chain_sharedlib[] = {
    "application-x-sharedlib", "application-x-executable",
    "application-x-generic", NULL };
static const char *const k_chain_script_generic[] = {
    "text-x-script", "text-x-generic", NULL };
static const char *const k_chain_c[] = {
    "text-x-csrc", "text-x-script", "text-x-generic", NULL };
static const char *const k_chain_c_header[] = {
    "text-x-chdr", "text-x-csrc", "text-x-script", "text-x-generic",
    NULL };
static const char *const k_chain_cpp[] = {
    "text-x-c++src", "text-x-csrc", "text-x-script", "text-x-generic",
    NULL };
static const char *const k_chain_python[] = {
    "text-x-python", "text-x-script", "text-x-generic", NULL };
static const char *const k_chain_shell[] = {
    "application-x-shellscript", "text-x-script", "text-x-generic",
    NULL };
static const char *const k_chain_java[] = {
    "text-x-java", "text-x-script", "text-x-generic", NULL };
static const char *const k_chain_ruby[] = {
    "text-x-ruby", "text-x-script", "text-x-generic", NULL };
static const char *const k_chain_rust[] = {
    "text-x-rust", "text-x-rust-src", "text-x-script", "text-x-generic",
    NULL };
static const char *const k_chain_makefile[] = {
    "text-x-makefile", "text-x-script", "text-x-generic", NULL };

/* The places sidebar's chains (symbolic first — the desktop norm;
 * the engine tints any "-symbolic" hit through the alpha mask). */
static const char *const k_chain_place_home[] = {
    "user-home-symbolic", "user-home", "folder", NULL };
static const char *const k_chain_place_recent[] = {
    "document-open-recent-symbolic", "document-open-recent", "folder",
    NULL };
static const char *const k_chain_place_drive[] = {
    "drive-harddisk-symbolic", "drive-harddisk", "folder", NULL };
static const char *const k_chain_place_folder[] = {
    "folder-symbolic", "folder", NULL };

/* The extension table. Grouped by chain; one strcmp per entry, the
 * whole table is a few dozen compares per row — nothing worth a
 * hash for dialog-sized listings. Each extension INCLUDES the dot
 * so a case-insensitive suffix compare is the whole match. */
typedef struct {
    const char *ext;
    const char *const *chain;
} it_ext_entry;

static const it_ext_entry k_extensions[] = {
    /* images */
    { ".png", k_chain_image },  { ".jpg", k_chain_image },
    { ".jpeg", k_chain_image }, { ".gif", k_chain_image },
    { ".bmp", k_chain_image },  { ".webp", k_chain_image },
    { ".svg", k_chain_image },  { ".svgz", k_chain_image },
    { ".tiff", k_chain_image }, { ".tif", k_chain_image },
    { ".ico", k_chain_image },  { ".xpm", k_chain_image },
    { ".avif", k_chain_image }, { ".heic", k_chain_image },
    { ".pnm", k_chain_image },  { ".ppm", k_chain_image },
    /* audio */
    { ".mp3", k_chain_audio },  { ".ogg", k_chain_audio },
    { ".oga", k_chain_audio },  { ".opus", k_chain_audio },
    { ".flac", k_chain_audio }, { ".wav", k_chain_audio },
    { ".aac", k_chain_audio },  { ".m4a", k_chain_audio },
    { ".wma", k_chain_audio },  { ".mid", k_chain_audio },
    { ".midi", k_chain_audio },
    /* video */
    { ".mp4", k_chain_video },  { ".mkv", k_chain_video },
    { ".webm", k_chain_video }, { ".avi", k_chain_video },
    { ".mov", k_chain_video },  { ".wmv", k_chain_video },
    { ".flv", k_chain_video },  { ".m4v", k_chain_video },
    { ".mpg", k_chain_video },  { ".mpeg", k_chain_video },
    { ".ts", k_chain_video },
    /* archives */
    { ".zip", k_chain_zip },    { ".jar", k_chain_zip },
    { ".7z", k_chain_zip },     { ".rar", k_chain_zip },
    { ".cab", k_chain_zip },    { ".arj", k_chain_zip },
    { ".lz4", k_chain_zip },    { ".iso", k_chain_zip },
    { ".tar", k_chain_tar },
    { ".gz", k_chain_compressed }, { ".bz2", k_chain_compressed },
    { ".xz", k_chain_compressed }, { ".zst", k_chain_compressed },
    { ".tgz", k_chain_compressed }, { ".tbz", k_chain_compressed },
    { ".txz", k_chain_compressed }, { ".tzst", k_chain_compressed },
    /* documents */
    { ".pdf", k_chain_pdf },
    { ".html", k_chain_html },  { ".htm", k_chain_html },
    { ".md", k_chain_markdown }, { ".markdown", k_chain_markdown },
    /* fonts */
    { ".ttf", k_chain_font },   { ".otf", k_chain_font },
    { ".woff", k_chain_font },  { ".woff2", k_chain_font },
    /* source code */
    { ".c", k_chain_c },        { ".h", k_chain_c_header },
    { ".cpp", k_chain_cpp },    { ".cc", k_chain_cpp },
    { ".cxx", k_chain_cpp },    { ".hpp", k_chain_cpp },
    { ".py", k_chain_python },  { ".pyw", k_chain_python },
    { ".sh", k_chain_shell },   { ".bash", k_chain_shell },
    { ".zsh", k_chain_shell },  { ".fish", k_chain_shell },
    { ".java", k_chain_java },  { ".rb", k_chain_ruby },
    { ".rs", k_chain_rust },    { ".go", k_chain_script_generic },
    { ".js", k_chain_script_generic },  { ".ts", k_chain_script_generic },
    { ".lua", k_chain_script_generic }, { ".pl", k_chain_script_generic },
    { ".php", k_chain_script_generic }, { ".cs", k_chain_script_generic },
    { ".swift", k_chain_script_generic },
    { ".hs", k_chain_script_generic },  { ".ml", k_chain_script_generic },
    { ".scala", k_chain_script_generic },
    { ".sql", k_chain_script_generic }, { ".r", k_chain_script_generic },
    { ".vim", k_chain_script_generic },
    { ".mk", k_chain_makefile },
    /* system files */
    { ".desktop", k_chain_desktop }, { ".so", k_chain_sharedlib },
    { ".o", k_chain_sharedlib },     { ".a", k_chain_sharedlib },
    { ".fdk", k_chain_text },
};

/* Case-insensitive ASCII suffix match (extensions are compared the
 * GTK way: photo.PNG is an image). */
static bool it_ext_match(const char *name, const char *ext) {
    size_t nl = strlen(name);
    size_t el = strlen(ext);
    if (nl <= el) {
        return false; /* no stem, or equal: ".png" alone is not a
                         .png file in any file manager's book */
    }
    for (size_t i = 0; i < el; i++) {
        char a = name[nl - el + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

const char *const *fdk__icon_chain_for_name(const char *name) {
    if (name == NULL) {
        return k_chain_text;
    }
    for (size_t i = 0;
         i < sizeof(k_extensions) / sizeof(k_extensions[0]); i++) {
        if (it_ext_match(name, k_extensions[i].ext)) {
            return k_extensions[i].chain;
        }
    }
    return NULL; /* unknown: caller decides (exec-bit stat) */
}

const char *const *fdk__icon_chain_for_entry(const char *dir,
                                             const char *name,
                                             bool is_dir) {
    if (is_dir) {
        return k_chain_folder;
    }
    const char *const *chain = fdk__icon_chain_for_name(name);
    if (chain != NULL) {
        return chain;
    }
    /* No known extension: the honest split is the executable bit.
     * A stat ONLY on this path — extension-known files (the vast
     * majority) never pay for one. The join is local: the theme
     * layer never reaches into the widget layer's helpers. */
    if (dir == NULL || name == NULL) {
        return k_chain_text;
    }
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    bool slash = (dl > 0 && dir[dl - 1] == '/');
    size_t need = dl + (slash ? 0u : 1u) + nl + 1u;
    char *full = fdk_alloc(need);
    if (full == NULL) {
        return k_chain_text;
    }
    memcpy(full, dir, dl);
    size_t o = dl;
    if (!slash) {
        full[o++] = '/';
    }
    memcpy(full + o, name, nl + 1);
    struct stat st;
    const char *const *out = k_chain_text;
    if (stat(full, &st) == 0 && S_ISREG(st.st_mode) &&
        (st.st_mode & S_IXUSR) != 0) {
        out = k_chain_executable;
    }
    fdk_free(full);
    return out;
}

const char *const *fdk__icon_chain_place(fdk_icon_place_kind kind) {
    switch (kind) {
    case FDK_ICON_PLACE_HOME:
        return k_chain_place_home;
    case FDK_ICON_PLACE_RECENT:
        return k_chain_place_recent;
    case FDK_ICON_PLACE_DRIVE:
        return k_chain_place_drive;
    case FDK_ICON_PLACE_FOLDER:
    default:
        return k_chain_place_folder;
    }
}
