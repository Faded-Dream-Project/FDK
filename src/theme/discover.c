/*
 * discover.c — theme discovery: the search path, name lookup, and
 * the available-theme enumeration (1.4.13)
 *
 * The library half of the CLI story. Until 1.4.13 a theme was a path
 * an application already knew (fdk_theme_load) or a string it parsed
 * itself; there was no way to ASK the system what is installed, and
 * no way for a user to say "my desktop is Matrix now" once, for every
 * FDK application. This file owns the answer to both:
 *
 *   fdk_theme_find()          — "load me the theme named X, wherever
 *                                it properly lives" (the search path
 *                                is documented in fdk_theme.h)
 *   fdk_theme_available_*()   — "what themes exist?" (the engine
 *                                behind `fdk-theme list`)
 *
 * GTK-parity note: this is GTK's GtkSettings
 * gtk-theme-name + icon-theme-scan machinery in miniature, and the
 * font layer's discovery posture applied to themes: environment
 * override first, then the XDG data hierarchy, then the system
 * directories `make install` fills — never a compiled-in absolute
 * path that silently goes stale when the library is relocated.
 *
 * LOOKUP ORDER inside find() is two passes:
 *
 *   1. STEM PASS — for each directory in priority order, stat
 *      <dir>/<name>.fdk. First hit wins (directory priority, not
 *      alphabetical). The stem must match the [A-Za-z0-9_-]{1,64}
 *      grammar to take this pass at all: `name` is interpolated into
 *      a path, so anything containing '/', '..', or an empty byte
 *      must never reach the stat. A name outside the grammar falls
 *      through to pass 2, which builds paths only from directory
 *      ENTRIES it enumerated itself — user input never becomes a
 *      path there. This is the same two-sides-of-trust shape the
 *      prefs resolver uses for $FDK_PREFS_FILE.
 *
 *   2. INTERNAL-NAME PASS — themes whose file name differs from
 *      their quoted `name` key ("Daylight" in daylight.fdk is a
 *      matching stem, but a file ships/collides as theme-v2.fdk with
 *      name "Daylight"). Every *.fdk entry is loaded and its internal
 *      name compared exactly (case-sensitive, like every name in the
 *      FDK family). This pass is bounded and honest about failure: a
 *      file that fails to parse here is skipped with ONE warning — a
 *      broken file squatting in a scanned directory must not hide
 *      its healthy neighbors — while in the stem pass the SAME file,
 *      explicitly asked for by name, propagates its error (themes
 *      fail loud when they are chosen, fail quiet when they are
 *      merely present; both postures are deliberate, see
 *      fdk_theme.h).
 *
 * The enumeration cache is process-lifetime by design (fontscan's
 * g_font_path precedent): installed-theme sets do not change under a
 * running application, `fdk-theme` is a fresh process per invocation,
 * and a rescan-per-call API is an invitation to call it from a paint
 * loop. The cache is reachable from this file's statics, so an LSan
 * build sees it as live, not leaked; nothing here needs the
 * fontconfig-style bracket.
 *
 * Threads: like the rest of the theme module, discovery is meant to
 * run before the application goes multi-threaded (the lazy settings
 * boot guarantees it runs on the first paint, on the main thread).
 * The one-shot cache without a lock is the documented contract, same
 * as fdk_theme_set_default().
 */

#define FDK_LOG_TAG "theme"

#include "theme_internal.h"
#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the search path -------------------------------------------------- */

/* Hard cap on directories consulted. The real path is 1 (env) + 1
 * (XDG_DATA_HOME) + N (XDG_DATA_DIRS); XDG systems have two or three
 * data dirs. The cap exists so a pathological XDG_DATA_DIRS (a loop
 * writing colons, a quoting accident) degrades to "first 14 entries"
 * with a warning instead of unbounded work — the same bounded-honesty
 * rule every FDK scanner follows. */
#define THEME_MAX_DIRS 16
#define THEME_MAX_DIRS_FROM_XDG 14

/* Suffix of a theme file, with the dot. */
#define THEME_SUFFIX ".fdk"

/* Stem grammar bound (mirrors the prefs key-half bound). */
#define THEME_STEM_MAX 64

/* Collects the search path into `dirs` (fixed-size, no allocation to
 * free) and returns the count. Entries are pointers into the ENVIRON
 * and stack buffers copied into `storage` — the caller copies what it
 * needs before the storage goes out of scope. */
static size_t theme_dirs(char dirs[THEME_MAX_DIRS][1024]) {
    size_t n = 0;

    const char *env = getenv("FDK_THEME_DIR");
    if (env != NULL && env[0] == '/') {
        snprintf(dirs[n], 1024, "%s", env);
        n++;
    } else if (env != NULL && env[0] != '\0') {
        FDK_WARN("theme: $FDK_THEME_DIR is not absolute and is ignored");
    }

    const char *xdg_home = getenv("XDG_DATA_HOME");
    if (xdg_home != NULL && xdg_home[0] == '/') {
        snprintf(dirs[n], 1024, "%s/fdk/themes", xdg_home);
        n++;
    } else {
        /* XDG default when unset OR non-absolute (spec rule: a
         * non-absolute XDG var is ignored, not used relatively). */
        const char *home = getenv("HOME");
        if (home != NULL && home[0] == '/') {
            snprintf(dirs[n], 1024, "%s/.local/share/fdk/themes", home);
            n++;
        }
    }

    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    if (xdg_dirs == NULL || xdg_dirs[0] == '\0') {
        xdg_dirs = "/usr/local/share:/usr/share"; /* XDG default */
    }

    /* Walk the colon list. Empty and non-absolute entries are
     * skipped (XDG rules; also what makes a trailing/double colon
     * harmless). */
    const char *p = xdg_dirs;
    size_t taken = 0;
    while (*p != '\0' && taken < THEME_MAX_DIRS_FROM_XDG) {
        const char *colon = strchr(p, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && p[0] == '/' && n < THEME_MAX_DIRS) {
            snprintf(dirs[n], 1024, "%.*s/fdk/themes", (int)len, p);
            n++;
            taken++;
        } else if (len > 0 && p[0] != '/') {
            FDK_WARN("theme: non-absolute $XDG_DATA_DIRS entry ignored");
        }
        if (colon == NULL) {
            break;
        }
        p = colon + 1;
    }
    if (*p != '\0' && taken == THEME_MAX_DIRS_FROM_XDG) {
        FDK_WARN("theme: $XDG_DATA_DIRS truncated to %d entries",
                 (int)THEME_MAX_DIRS_FROM_XDG);
    }

    return n;
}

/* True when `name` is a usable file stem: 1..64 chars of
 * [A-Za-z0-9_-]. This is the gate for the direct (path-building)
 * lookup pass — see the header comment for why it must be strict. */
static bool theme_stem_ok(const char *name) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name;
         *p != '\0'; p++, n++) {
        bool ok = (*p >= 'A' && *p <= 'Z') ||
                  (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') ||
                  *p == '_' || *p == '-';
        if (!ok) {
            return false;
        }
    }
    return n >= 1 && n <= THEME_STEM_MAX;
}

/* True when `path` names a REGULAR file (a directory named foo.fdk
 * is not a theme, and d_type is DT_UNKNOWN on some filesystems, so
 * this goes through stat rather than trusting readdir). */
static bool theme_is_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

/* ---- fdk_theme_find --------------------------------------------------- */

fdk_theme *fdk_theme_find(const char *name, fdk_result *out_error) {
    if (out_error != NULL) {
        *out_error = FDK_ERR_INVALID_ARGUMENT;
    }
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    char dirs[THEME_MAX_DIRS][1024];
    size_t ndirs = theme_dirs(dirs);

    /* Pass 1: the stem, in directory-priority order. */
    if (theme_stem_ok(name)) {
        for (size_t i = 0; i < ndirs; i++) {
            char path[1024 + THEME_STEM_MAX + 8];
            snprintf(path, sizeof path, "%s/%s%s", dirs[i], name,
                     THEME_SUFFIX);
            if (!theme_is_file(path)) {
                continue;
            }
            /* The asked-for file exists: hand its parse result to the
             * caller untouched — loud, per the theme posture. */
            fdk_result r = FDK_OK;
            fdk_theme *t = fdk_theme_load(path, &r);
            if (out_error != NULL) {
                *out_error = r;
            }
            return t; /* NULL only on load failure, r carries why */
        }
    }

    /* Pass 2: the internal name. Paths here are built only from
     * entries readdir handed us; `name` is used solely as a
     * comparison string, never interpolated into a path. */
    for (size_t i = 0; i < ndirs; i++) {
        DIR *d = opendir(dirs[i]);
        if (d == NULL) {
            continue; /* unreadable/unopened directory: not an error,
                       * the search path is best-effort by design */
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *fn = ent->d_name;
            size_t flen = strlen(fn);
            if (flen <= strlen(THEME_SUFFIX) || fn[0] == '.' ||
                strcmp(fn + flen - strlen(THEME_SUFFIX), THEME_SUFFIX) != 0) {
                continue;
            }
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dirs[i], fn);
            if (!theme_is_file(path)) {
                continue;
            }
            /* Probing, not asking: a parse failure here is a skip
             * with ONE warning of our own, not the loader's full
             * ERROR diagnostics — those belong to the file somebody
             * explicitly requested (the stem pass). The bracket
             * silences exactly this load (log_internal.h). */
            fdk__log_level_push(FDK_LOG_NONE);
            fdk_result r = FDK_OK;
            fdk_theme *t = fdk_theme_load(path, &r);
            fdk__log_level_pop();
            if (t == NULL) {
                /* Present but broken: one warning, keep walking. A
                 * dead file must not bury the living ones. */
                FDK_WARN("theme: %s exists but failed to load; skipped",
                         path);
                continue;
            }
            if (strcmp(fdk_theme_name(t), name) == 0) {
                closedir(d);
                if (out_error != NULL) {
                    *out_error = FDK_OK;
                }
                return t;
            }
            fdk_theme_destroy(t);
        }
        closedir(d);
    }

    if (out_error != NULL) {
        *out_error = FDK_ERR_NOT_FOUND;
    }
    return NULL;
}

/* ---- the available-theme enumeration ---------------------------------- */

typedef struct {
    char *name; /* the file stem, owned */
    char *path; /* the full path, owned  */
} theme_entry;

/* The process-lifetime cache. Reachable from this static, so it is
 * root-reachable memory under LSan (unlike fontconfig's internals,
 * no leak bracket is needed). */
static theme_entry *g_entries;
static size_t g_entry_count;
static bool g_scanned;

static int entry_cmp(const void *a, const void *b) {
    const theme_entry *ea = a;
    const theme_entry *eb = b;
    return strcmp(ea->name, eb->name);
}

/* The one-time scan. Failure posture: unreadable directories are
 * skipped, unreadable/unparseable files are skipped with one warning
 * (the enumeration must list what it CAN, never die on what it
 * can't — same resilience rule as find()'s pass 2). */
static void scan_available(void) {
    g_scanned = true;

    char dirs[THEME_MAX_DIRS][1024];
    size_t ndirs = theme_dirs(dirs);

    size_t cap = 16;
    theme_entry *entries = fdk_alloc(cap * sizeof *entries);
    if (entries == NULL) {
        FDK_WARN("theme: out of memory scanning for themes");
        return;
    }
    size_t count = 0;

    for (size_t i = 0; i < ndirs; i++) {
        DIR *d = opendir(dirs[i]);
        if (d == NULL) {
            continue;
        }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            const char *fn = ent->d_name;
            size_t flen = strlen(fn);
            if (flen <= strlen(THEME_SUFFIX) || fn[0] == '.' ||
                strcmp(fn + flen - strlen(THEME_SUFFIX), THEME_SUFFIX) != 0) {
                continue;
            }
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dirs[i], fn);
            if (!theme_is_file(path)) {
                continue;
            }

            /* The stem is the listing's identity; the file only has
             * to EXIST to be listed (a broken theme still shows up,
             * flagged by its load failure when something tries to
             * use it — listing is inventory, not validation). */
            size_t stem_len = flen - strlen(THEME_SUFFIX);
            char *stem = fdk_alloc(stem_len + 1);
            if (stem == NULL) {
                continue;
            }
            memcpy(stem, fn, stem_len);
            stem[stem_len] = '\0';

            /* Shadowing: same stem in a later (lower-priority)
             * directory is dropped, matching find()'s pass 1. */
            bool shadowed = false;
            for (size_t k = 0; k < count; k++) {
                if (strcmp(entries[k].name, stem) == 0) {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed) {
                fdk_free(stem);
                continue;
            }

            if (count == cap) {
                size_t ncap = cap * 2;
                theme_entry *grown =
                    fdk_realloc(entries, ncap * sizeof *entries);
                if (grown == NULL) {
                    fdk_free(stem);
                    FDK_WARN("theme: out of memory scanning for themes");
                    closedir(d);
                    goto done;
                }
                entries = grown;
                cap = ncap;
            }
            char *path_copy = fdk__theme_strdup(path);
            if (path_copy == NULL) {
                fdk_free(stem);
                continue;
            }
            entries[count].name = stem;
            entries[count].path = path_copy;
            count++;
        }
        closedir(d);
    }

done:
    /* Stable, diffable output: byte-order sort by stem. */
    if (count > 1) {
        qsort(entries, count, sizeof *entries, entry_cmp);
    }
    g_entries = entries;
    g_entry_count = count;
}

size_t fdk_theme_available_count(void) {
    if (!g_scanned) {
        scan_available();
    }
    return g_entry_count;
}

const char *fdk_theme_available_name(size_t index) {
    if (!g_scanned) {
        scan_available();
    }
    return (index < g_entry_count) ? g_entries[index].name : NULL;
}

const char *fdk_theme_available_path(size_t index) {
    if (!g_scanned) {
        scan_available();
    }
    return (index < g_entry_count) ? g_entries[index].path : NULL;
}

/* Internal, TEST-only (see theme_internal.h): forget the cached scan.
 * Frees the entries and clears the flag so the next available_* call
 * rescans under whatever environment the test has staged. Nothing
 * about the public API suggests this exists — the cache is
 * process-lifetime by design. */
void fdk__theme_scan_reset_for_tests(void) {
    for (size_t i = 0; i < g_entry_count; i++) {
        fdk_free(g_entries[i].name);
        fdk_free(g_entries[i].path);
    }
    fdk_free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
    g_scanned = false;
}
