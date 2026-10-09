/* Defined BEFORE every include: asprintf() and realpath() hide
 * behind _DEFAULT_SOURCE on strict feature-test builds. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#define FDK_LOG_TAG "widgets"

/*
 * file_dialog.c — file / folder selection dialogs (1.2.0; 1.2.3 the
 * release-quality browser)
 *
 * A real FDK window built from the stock catalog, following the
 * message-dialog lifecycle exactly (dialog.c): toolkit-owned,
 * auto-painted, one callback, self-destroying. The content is a
 * small file browser in the native-desktop shape:
 *
 *   [ Up ] [ Home ] [ x Hidden ]     [ *.png v ]   <- toolbar
 *   /current/path                                 <- path bar (Entry:
 *                                                   type a path, Enter)
 *   +----------+  +----------------------------+
 *   | Places   |  | Documents/                 |   <- file list
 *   | Home     |  | downloads/                 |      (SINGLE for
 *   | Desktop  |  | notes.txt                  |       OPEN_FILE/SAVE,
 *   | ...      |  | render.c                   |       MULTIPLE for
 *   | mounts   |  +----------------------------+       the plurals)
 *   +----------+  Name: [ filename.txt ]          <- SAVE only
 *   status line                  [ Cancel ] [ Open/Save ]
 *
 * The places sidebar comes from pure-POSIX filesystem discovery
 * (fdk__fs_discover_places below): $HOME, the conventional XDG
 * user directories when they exist, /, real filesystem mounts from
 * /proc/self/mounts, and one level of /media and /mnt — no udev,
 * no D-Bus, per the toolkit's no-bus policy. Every place is
 * stat()-verified to exist at discovery time and deduplicated by
 * canonical path.
 *
 * The scan is a real opendir/readdir pass (directories first, then
 * alphabetical; hidden entries behind the toggle). Name filters
 * (options.filters, ";"-separated globs) hide non-matching FILES —
 * directories are never filtered, navigation always works — with
 * case-insensitive matching (the GTK convention). Accepting an
 * OPEN kind runs a stat() re-validation on every selected path —
 * the listing is a snapshot, the filesystem is not. The result
 * model is explicit: ACCEPTED with paths[], CANCELLED, or ERROR
 * (could not browse even the fallback chain start_dir -> $HOME
 * -> /).
 *
 * SAVE_FILE adds the Name row: the entry holds the target name
 * (seeded from options.start_name, filled by clicking a listed
 * file), Save validates it honestly (non-empty, no '/', not "." or
 * "..", <= 255 bytes), refuses directories and non-regular files,
 * and asks before overwriting (a nested Yes/No message dialog —
 * declining returns to the dialog; it does not cancel). The
 * accepted path canonicalizes the parent but keeps the leaf
 * EXACTLY as typed.
 *
 * Double-click / Enter on a directory descends into it; on a file
 * it accepts the current selection (SAVE: after filling the Name
 * row). "Up" climbs toward /. The path bar is an Entry: Enter
 * browses (absolute, "~"-expanded, or relative to the current
 * folder); a failed browse never abandons the dialog's working
 * directory — the new location is probed first and swapped only on
 * success, with the reason in the status line.
 *
 * Modal grabs follow the message-dialog contract (X11 grab; Wayland
 * has no toplevel-grab protocol — accepted and ignored, documented
 * rather than faked). The nested overwrite confirmation takes the
 * grab while it is up (X11); when it closes without accepting, the
 * file dialog re-establishes its own — the grab is re-taken, not
 * assumed.
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_dialog.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FD_PAD 12          /* outer padding                        */
#define FD_GAP 8           /* vertical gap between rows            */
#define FD_TOPBAR_H 34     /* Up + Home + toggle + filter row      */
#define FD_STATUS_H 22     /* status line                          */
#define FD_BTN_MIN_W 90    /* minimum button width                 */
#define FD_BTN_PAD 32      /* button text padding (both sides)     */
#define FD_PLACES_W 148    /* places sidebar width                 */
#define FD_LIST_W 456      /* file list width                      */
#define FD_LIST_H 300      /* list viewport height                 */
#define FD_ROW_H 28        /* path bar / name row height           */
#define FD_COMBO_W 170     /* filter combo width                   */
#define FD_MAX_ENTRIES 8192 /* scan cap: pathological dirs degrade,
                               they do not OOM the dialog           */
#define FD_MAX_PLACES 24   /* sidebar cap (mounts + media + mnt)    */
#define FD_MAX_MOUTS 8     /* mounts shown                         */
#define FD_MAX_MEDIA 8     /* /media + /mnt entries shown (each)    */
#define FD_NAME_MAX 255    /* NAME_MAX-safe cap for typed names     */
#define FD_PATH_BUF 4096   /* PATH_MAX-safe scratch for canonical   */
#define FD_KEY_L_EVDEV ((fdk_scancode)38) /* evdev KEY_L — the same
                                             value shortcut.c's table
                                             assigns the letter "L" */

/* ---- entry model ----
 *
 * The entry types live in widgets_internal.h (the headless test
 * seam); this file owns the scan and the sort. */

static int fd_entry_cmp(const void *a, const void *b) {
    const fdk_fd_entry *ea = a, *eb = b;
    /* directories first, then name order (plain byte compare — the
     * dialog's user-visible order matches `ls` closely enough that
     * locale collation differences are not worth the dependency). */
    if (ea->dir != eb->dir) {
        return ea->dir ? -1 : 1;
    }
    return strcmp(ea->name, eb->name);
}

static bool fd_name_hidden(const char *name) {
    return name[0] == '.';
}

/* ---- glob matching (1.2.3) ---- */

static char fd_ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

/* Iterative wildcard match: '*' spans any run (including empty),
 * '?' one character, everything else compares literally after
 * ASCII case folding — byte-wise and locale-independent, so the
 * dialog's filtering is identical on every machine. */
bool fdk__file_dialog_glob_match(const char *pattern, const char *name) {
    if (pattern == NULL || name == NULL) {
        return false;
    }
    const char *p = pattern, *n = name;
    const char *star_p = NULL, *star_n = NULL;
    while (*n != '\0') {
        if (*p == '*') {
            star_p = p++;
            star_n = n;
        } else if (*p == '?' ||
                   fd_ascii_lower(*p) == fd_ascii_lower(*n)) {
            p++;
            n++;
        } else if (star_p != NULL) {
            p = star_p + 1;
            n = ++star_n;
        } else {
            return false;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

size_t fdk__file_dialog_parse_filters(const char *filters, char ***out) {
    *out = NULL;
    if (filters == NULL) {
        return 0;
    }
    /* Count separators first so the array is allocated once. */
    size_t n = 1;
    for (const char *c = filters; *c != '\0'; c++) {
        if (*c == ';') {
            n++;
        }
    }
    char **v = fdk_alloc_array(n, sizeof(char *));
    if (v == NULL) {
        return 0;
    }
    size_t count = 0;
    const char *start = filters;
    for (;;) {
        const char *end = strchr(start, ';');
        size_t len = (end != NULL) ? (size_t)(end - start) : strlen(start);
        /* Trim surrounding whitespace; skip empty fragments. */
        while (len > 0 && (*start == ' ' || *start == '\t')) {
            start++;
            len--;
        }
        while (len > 0 &&
               (start[len - 1] == ' ' || start[len - 1] == '\t')) {
            len--;
        }
        if (len > 0) {
            v[count] = fdk_alloc(len + 1);
            if (v[count] == NULL) {
                fdk__file_dialog_free_filters(v, count);
                return 0;
            }
            memcpy(v[count], start, len);
            v[count][len] = '\0';
            count++;
        }
        if (end == NULL) {
            break;
        }
        start = end + 1;
    }
    if (count == 0) {
        fdk_free(v);
        return 0;
    }
    *out = v;
    return count;
}

void fdk__file_dialog_free_filters(char **patterns, size_t count) {
    if (patterns == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_free(patterns[i]);
    }
    fdk_free(patterns);
}

/* Does `name` match any pattern? Directories are the caller's
 * decision (never filtered). */
static bool fd_name_matches(char **patterns, size_t pattern_count,
                            const char *name) {
    if (pattern_count == 0 || patterns == NULL) {
        return true;
    }
    for (size_t i = 0; i < pattern_count; i++) {
        if (fdk__file_dialog_glob_match(patterns[i], name)) {
            return true;
        }
    }
    return false;
}

/* ---- path helpers (1.2.3) ---- */

char *fdk__path_join(const char *dir, const char *name) {
    if (dir == NULL || name == NULL) {
        return NULL;
    }
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    bool slash = (dl > 0 && dir[dl - 1] == '/');
    char *out = fdk_alloc(dl + (slash ? 0 : 1) + nl + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, dir, dl);
    size_t o = dl;
    if (!slash) {
        out[o++] = '/';
    }
    memcpy(out + o, name, nl + 1);
    return out;
}

char *fdk__path_normalize_dir(const char *dir) {
    if (dir == NULL || dir[0] == '\0') {
        return NULL;
    }
    size_t len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/') {
        len--;
    }
    char *out = fdk_alloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, dir, len);
    out[len] = '\0';
    return out;
}

int fdk__save_name_validate(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 1;
    }
    if (strlen(name) > FD_NAME_MAX) {
        return 4;
    }
    if (strchr(name, '/') != NULL) {
        return 2;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 3;
    }
    bool visible = false;
    for (const char *c = name; *c != '\0'; c++) {
        if (!isspace((unsigned char)*c)) {
            visible = true;
            break;
        }
    }
    if (!visible) {
        return 5;
    }
    return 0;
}

char *fdk__path_expand_tilde(const char *path) {
    if (path == NULL) {
        return NULL;
    }
    if (path[0] != '~') {
        return fdk__strdup(path);
    }
    const char *rest = path + 1;
    if (rest[0] != '\0' && rest[0] != '/') {
        /* "~user" — no name-service lookup in the toolkit; the
         * honest answer is "not expanded" (it will fail the stat
         * and the status line will say so). */
        return fdk__strdup(path);
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        return fdk__strdup(path);
    }
    if (rest[0] == '\0') {
        return fdk__strdup(home);
    }
    return fdk__path_join(home, rest + 1);
}

/* ---- the scan (with filters) ---- */

/* Scans `dir`. Returns 0 on success (even an empty listing is a
 * success); -1 when the directory cannot be opened. `dirs_only`
 * filters to directories (the FOLDER kinds); `show_hidden`
 * includes dot entries; `patterns` (count > 0) case-insensitively
 * filters FILES. The result is fully owned by the caller
 * (fd_entries_free). Sorting: dirs first, then alphabetical. */
int fdk__file_dialog_scan(const char *dir, bool dirs_only,
                          bool show_hidden,
                          char **patterns, size_t pattern_count,
                          fdk_fd_entries *out) {
    memset(out, 0, sizeof(*out));
    DIR *d = opendir(dir);
    if (d == NULL) {
        return -1;
    }
    size_t cap = 64;
    out->v = fdk_alloc_array(cap, sizeof(fdk_fd_entry));
    if (out->v == NULL) {
        closedir(d);
        return -1;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue; /* Up handles ..; . is ourselves */
        }
        bool hidden = fd_name_hidden(de->d_name);
        if (hidden && !show_hidden) {
            continue;
        }
        /* d_type is a hint on most filesystems; DT_UNKNOWN forces
         * the stat fallback. Classification MUST be the stat's say,
         * not the hint, for the accept-time contract to hold. */
        bool is_dir = false;
        char *full = NULL;
        if (de->d_type == DT_DIR) {
            is_dir = true;
        } else {
            full = fdk__path_join(dir, de->d_name);
            if (full == NULL) {
                continue;
            }
            struct stat st;
            is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        }
        if (dirs_only && !is_dir) {
            fdk_free(full);
            continue;
        }
        /* Filters apply to FILES only — directories stay
         * navigable under every filter (the native rule). */
        if (!is_dir && pattern_count > 0 &&
            !fd_name_matches(patterns, pattern_count, de->d_name)) {
            fdk_free(full);
            continue;
        }
        if (out->count == cap) {
            if (out->count >= FD_MAX_ENTRIES) {
                fdk_free(full);
                continue; /* cap reached: extra entries are skipped,
                             a warning, not a failure */
            }
            fdk_fd_entry *grown =
                fdk_realloc(out->v, cap * 2 * sizeof(fdk_fd_entry));
            if (grown == NULL) {
                fdk_free(full);
                break;
            }
            out->v = grown;
            cap *= 2;
        }
        out->v[out->count].name = fdk__strdup(de->d_name);
        if (out->v[out->count].name == NULL) {
            fdk_free(full);
            break;
        }
        out->v[out->count].dir = is_dir;
        out->v[out->count].hidden = hidden;
        out->count++;
        fdk_free(full);
    }
    closedir(d);
    qsort(out->v, out->count, sizeof(fdk_fd_entry), fd_entry_cmp);
    return 0;
}

void fdk__file_dialog_entries_free(fdk_fd_entries *entries) {
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < entries->count; i++) {
        fdk_free(entries->v[i].name);
    }
    fdk_free(entries->v);
    memset(entries, 0, sizeof(*entries));
}

/* ---- filesystem discovery — the Places sidebar's data (1.2.3) ----
 *
 * Pure POSIX, in priority order, all stat()-verified and deduped
 * by canonical path (realpath): $HOME; the conventional XDG user
 * directories when they exist; / ; real-filesystem mounts from
 * /proc/self/mounts; one level of /media and /mnt (the removable
 * -media convention). Real filesystems means the whitelist below
 * — pseudo filesystems (proc, sysfs, tmpfs, cgroup, overlay,
 * squashfs, ...) are noise a file dialog should not show. */

/* The place's symbolic glyph (1.4.1): the house for HOME, the drive
 * slab for the root and real-filesystem mounts, the folder for
 * user directories. Label-based for Home (the one place whose path
 * is $HOME, whatever that is), path-based for the rest. */
static fdk_row_icon fdlg_place_icon(const char *path, const char *label) {
    if (label != NULL && strcmp(label, "Home") == 0) {
        return FDK_ROW_ICON_HOME;
    }
    if (path != NULL && strcmp(path, "/") == 0) {
        return FDK_ROW_ICON_DRIVE;
    }
    if (path != NULL && (strncmp(path, "/media/", 7) == 0 ||
                         strncmp(path, "/mnt/", 5) == 0)) {
        return FDK_ROW_ICON_DRIVE;
    }
    return FDK_ROW_ICON_FOLDER;
}

static bool fd_fs_is_real(const char *fstype) {
    static const char *const real_fs[] = {
        "ext2", "ext3", "ext4", "btrfs",  "xfs",  "f2fs", "vfat",
        "exfat", "ntfs", "ntfs3", "udf", "iso9660", "zfs", "jfs",
        "reiserfs", "erofs", "fuseblk",
    };
    for (size_t i = 0; i < sizeof(real_fs) / sizeof(real_fs[0]);
         i++) {
        if (strcmp(fstype, real_fs[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Adds {label, path} after canonical-path dedup + stat check.
 * Returns 0 on success (added OR skipped), -1 on OOM. */
static int fd_place_add(const char *label, const char *path,
                        char **seen, size_t *seen_count,
                        fdk_fs_place **places, size_t *count) {
    if (*count >= FD_MAX_PLACES) {
        return 0;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0; /* does not exist right now: not a place */
    }
    char canonical[FD_PATH_BUF];
    const char *key = path;
    if (realpath(path, canonical) != NULL) {
        key = canonical;
    }
    for (size_t i = 0; i < *seen_count; i++) {
        if (strcmp(seen[i], key) == 0) {
            return 0; /* duplicate canonical path */
        }
    }
    char *path_copy = fdk__strdup(path);
    char *label_copy = fdk__strdup(label);
    char *key_copy = fdk__strdup(key);
    if (path_copy == NULL || label_copy == NULL || key_copy == NULL) {
        fdk_free(path_copy);
        fdk_free(label_copy);
        fdk_free(key_copy);
        return -1;
    }
    (*places)[*count].label = label_copy;
    (*places)[*count].path = path_copy;
    (*count)++;
    seen[*seen_count] = key_copy;
    (*seen_count)++;
    return 0;
}

/* Undoes the octal escapes /proc/self/mounts uses in mount points
 * (\040 space, \011 tab, \134 backslash, \012 newline). Mount
 * points with spaces are real (USB sticks love them). */
static void fd_unescape_mount(char *s) {
    char *w = s;
    for (char *r = s; *r != '\0';) {
        if (r[0] == '\\' && r[1] == '0' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            char oct[4] = {r[1], r[2], r[3], '\0'};
            *w++ = (char)strtol(oct, NULL, 8);
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

int fdk__fs_discover_places(fdk_fs_place **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    fdk_fs_place *places = fdk_alloc_array(FD_MAX_PLACES,
                                           sizeof(fdk_fs_place));
    char **seen = fdk_alloc_array(FD_MAX_PLACES, sizeof(char *));
    if (places == NULL || seen == NULL) {
        fdk_free(places);
        fdk_free(seen);
        return -1;
    }
    size_t count = 0, seen_count = 0;

    /* 1. Home — always the anchor. */
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        if (fd_place_add("Home", home, seen, &seen_count,
                         &places, &count) != 0) {
            goto oom;
        }
        /* 2. Conventional XDG user directories, when they exist.
         * ~/.config/user-dirs.dirs can localize these; reading it
         * is a config-format parser for six strings — the English
         * fallbacks are what GTK itself uses when that file is
         * absent, and every one is stat()-verified anyway. */
        static const char *const xdg[] = {
            "Desktop", "Documents", "Downloads",
            "Pictures", "Music", "Videos",
        };
        for (size_t i = 0;
             i < sizeof(xdg) / sizeof(xdg[0]) && count < FD_MAX_PLACES;
             i++) {
            char *p = fdk__path_join(home, xdg[i]);
            if (p == NULL) {
                goto oom;
            }
            int rc = fd_place_add(xdg[i], p, seen, &seen_count,
                                  &places, &count);
            fdk_free(p);
            if (rc != 0) {
                goto oom;
            }
        }
    }

    /* 3. The root filesystem. */
    if (fd_place_add("Filesystem", "/", seen, &seen_count,
                     &places, &count) != 0) {
        goto oom;
    }

    /* 4. Real-filesystem mounts from /proc/self/mounts. */
    FILE *mf = fopen("/proc/self/mounts", "r");
    if (mf != NULL) {
        char line[1024];
        size_t mounts = 0;
        while (mounts < FD_MAX_MOUTS &&
               count < FD_MAX_PLACES &&
               fgets(line, sizeof(line), mf) != NULL) {
            char dev[256], mnt[512], fstype[64];
            if (sscanf(line, "%255s %511s %63s", dev, mnt,
                       fstype) != 3) {
                continue;
            }
            if (!fd_fs_is_real(fstype)) {
                continue;
            }
            fd_unescape_mount(mnt);
            const char *label = (strcmp(mnt, "/") == 0)
                ? "Filesystem"
                : (strrchr(mnt, '/') != NULL ? strrchr(mnt, '/') + 1
                                             : mnt);
            if (fd_place_add(label, mnt, seen, &seen_count,
                             &places, &count) != 0) {
                fclose(mf);
                goto oom;
            }
            mounts++;
        }
        fclose(mf);
    }

    /* 5. Removable-media convention: one level of /media and
     * /mnt (usb sticks, cameras, bind mounts the admin made). */
    static const char *const media_dirs[] = {"/media", "/mnt"};
    for (size_t d = 0; d < 2; d++) {
        DIR *dir = opendir(media_dirs[d]);
        if (dir == NULL) {
            continue;
        }
        size_t added = 0;
        struct dirent *de;
        while (added < FD_MAX_MEDIA && count < FD_MAX_PLACES &&
               (de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') {
                continue;
            }
            char *p = fdk__path_join(media_dirs[d], de->d_name);
            if (p == NULL) {
                closedir(dir);
                goto oom;
            }
            int rc = fd_place_add(de->d_name, p, seen, &seen_count,
                                  &places, &count);
            fdk_free(p);
            if (rc != 0) {
                closedir(dir);
                goto oom;
            }
            added++;
        }
        closedir(dir);
    }

    if (count == 0) {
        /* Impossible on any POSIX system (/ always exists), but the
         * contract promises count >= 1 — enforce it honestly. */
        if (fd_place_add("Filesystem", "/", seen, &seen_count,
                         &places, &count) != 0 || count == 0) {
            goto oom;
        }
    }
    for (size_t i = 0; i < seen_count; i++) {
        fdk_free(seen[i]);
    }
    fdk_free(seen);
    *out = places;
    *out_count = count;
    return 0;

oom:
    fdk__fs_places_free(places, count);
    for (size_t i = 0; i < seen_count; i++) {
        fdk_free(seen[i]);
    }
    fdk_free(seen);
    return -1;
}

void fdk__fs_places_free(fdk_fs_place *places, size_t count) {
    if (places == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        fdk_free(places[i].label);
        fdk_free(places[i].path);
    }
    fdk_free(places);
}

/* ---- the dialog ---- */

typedef struct fdk_file_dialog {
    fdk_context *ctx;        /* borrowed; for nested dialogs       */
    fdk_window *window;      /* owned lifecycle (self-destroying) */
    fdk_widget *body;        /* the arrange-hooked content widget */
    fdk_widget *up_btn;
    fdk_widget *home_btn;
    fdk_widget *hidden_toggle;
    fdk_widget *filter_combo;
    fdk_widget *path_bar;    /* the breadcrumb row (1.4.0)          */
    fdk_widget *path_entry;  /* the Ctrl+L location entry           */
    bool location_mode;      /* true = entry shown, bar hidden      */
    fdk_widget *places_list;
    fdk_widget *list;
    fdk_widget *name_label;  /* SAVE only                          */
    fdk_widget *name_entry;  /* SAVE only                          */
    fdk_widget *status;
    fdk_widget *accept_btn;
    fdk_widget *cancel_btn;
    fdk_font *font;          /* owned (system default we loaded)   */
    fdk_file_dialog_kind kind;
    bool show_hidden;
    char *dir;               /* owned; the browsed directory      */
    fdk_fd_entries entries;      /* the current listing           */
    fdk_fs_place *places;        /* owned; sidebar data            */
    size_t place_count;
    char **patterns;             /* owned; parsed filters          */
    size_t pattern_count;
    size_t active_filter;        /* index into patterns,
                                    pattern_count = "All files"    */
    fdk_file_dialog_done_fn on_done;
    void *on_done_user;
    fdk_file_dialog_result pending; /* built during accept         */
    bool answered;
    /* Construction-succeeded marker (1.3.0): the fail: paths destroy
     * the window BEFORE it was ever shown; without this flag the
     * destroy-notify answered the app with CANCELLED while the
     * constructor simultaneously returned an error — the
     * double-signal every caller had to defensively ignore. */
    bool surfaced;
    bool was_modal;          /* re-grab after the overwrite ask    */
    bool tearing_down;       /* body destroy in progress           */
    fdk_window *confirm_win; /* nested overwrite dialog while up   */
} fdk_file_dialog;

typedef struct fdk_file_dialog_body {
    fdk_widget base;
    fdk_file_dialog *dialog; /* owned */
    /* The regions body_paint fills with the sidebar surface (the
     * places panel + the path row share the sidebar tone; everything
     * else is the window surface). Body-relative, set by arrange. */
    fdk_rect sidebar_rect;
} fdk_file_dialog_body;

/* Heap context handed to the nested overwrite confirmation (the
 * message dialog's user_data) — it must outlive `dlg` by exactly
 * the callback, and frees itself there. */
typedef struct fdk_save_confirm_ctx {
    fdk_file_dialog *dlg;
    char *target;            /* owned */
} fdk_save_confirm_ctx;

static fdk_file_dialog_body *fbody_of(fdk_widget *w) {
    return (fdk_file_dialog_body *)(void *)w;
}
static fdk_file_dialog *fdlg_of_body(fdk_widget *w) {
    return (w != NULL) ? fbody_of(w)->dialog : NULL;
}

/* ---- helpers ---- */

static const char *fdlg_accept_label(fdk_file_dialog_kind kind) {
    switch (kind) {
    case FDK_FILE_DIALOG_OPEN_FILES:
        return "Select Files";
    case FDK_FILE_DIALOG_OPEN_FOLDER:
        return "Select Folder";
    case FDK_FILE_DIALOG_OPEN_FOLDERS:
        return "Select Folders";
    case FDK_FILE_DIALOG_SAVE_FILE:
        return "Save";
    case FDK_FILE_DIALOG_OPEN_FILE:
    default:
        return "Open";
    }
}

static const char *fdlg_default_title(fdk_file_dialog_kind kind) {
    switch (kind) {
    case FDK_FILE_DIALOG_OPEN_FILES:
        return "Select Files";
    case FDK_FILE_DIALOG_OPEN_FOLDER:
        return "Select Folder";
    case FDK_FILE_DIALOG_OPEN_FOLDERS:
        return "Select Folders";
    case FDK_FILE_DIALOG_SAVE_FILE:
        return "Save File";
    case FDK_FILE_DIALOG_OPEN_FILE:
    default:
        return "Open File";
    }
}

static bool fdlg_kind_multi(fdk_file_dialog_kind kind) {
    return kind == FDK_FILE_DIALOG_OPEN_FILES ||
           kind == FDK_FILE_DIALOG_OPEN_FOLDERS;
}

static bool fdlg_kind_folders(fdk_file_dialog_kind kind) {
    return kind == FDK_FILE_DIALOG_OPEN_FOLDER ||
           kind == FDK_FILE_DIALOG_OPEN_FOLDERS;
}

static bool fdlg_kind_save(fdk_file_dialog_kind kind) {
    return kind == FDK_FILE_DIALOG_SAVE_FILE;
}

static void fdlg_set_status(fdk_file_dialog *d, const char *text) {
    if (d->status != NULL) {
        (void)fdk_label_set_text(d->status, text);
    }
}

/* The patterns the current scan should filter by: the ACTIVE combo
 * row only (one pattern), or none when "All files" is active. */
static char **fdlg_active_patterns(fdk_file_dialog *d, size_t *count) {
    if (d->active_filter < d->pattern_count) {
        *count = 1;
        return &d->patterns[d->active_filter];
    }
    *count = 0;
    return NULL;
}

/* ---- browsing ---- */

/* Rebuilds the file list rows from d->entries (the list widget is
 * cleared and re-appended; the selection resets — a snapshot list
 * cannot keep stale selections alive). The whole rebuild runs in ONE
 * list batch: a directory with 8192 entries is 8192 appends that
 * settle in a single relayout instead of re-placing the list per
 * append (the O(N^2) shape the 1.3.0 audit measured). */
static void fdlg_fill_list(fdk_file_dialog *d) {
    fdk_list_begin_batch(d->list);
    fdk_list_clear(d->list);
    for (size_t i = 0; i < d->entries.count; i++) {
        char row[512];
        if (d->entries.v[i].dir) {
            snprintf(row, sizeof(row), "%s/", d->entries.v[i].name);
        } else {
            snprintf(row, sizeof(row), "%s", d->entries.v[i].name);
        }
        (void)fdk_list_append(d->list, row, NULL);
    }
    fdk_list_end_batch(d->list);
}

/* ---- the breadcrumb path bar (1.4.0) ----
 *
 * The modern path display: the current directory split into clickable
 * segments separated by small vector chevrons — "Home  >  user  >
 * docs" — replacing the always-editable raw path Entry at rest. The
 * Entry survives behind Ctrl+L (the GTK location-toggle: type a path
 * by hand when the breadcrumbs cannot express it); Enter in the entry
 * browses and returns to breadcrumbs, Esc there abandons the edit and
 * returns too. The bar paints the flat-entry chrome (field fill +
 * border, rounded at the entry radius), the CURRENT segment in the
 * accent color, hover pills under the clickable segments, and an
 * ellipsis segment when too many crumbs precede the tail to fit
 * (clicking it jumps to the first collapsed crumb).
 *
 * Segment data: one entry per path component plus the leading "/"
 * (the root crumb; a relative dir — cannot happen for our normalized
 * dirs, but parsed honestly anyway — gets no leading slash crumb). */

#define FD_PB_MAX_SEGS 64   /* path components cap (deep trees collapse) */
#define FD_PB_PAD_X 10      /* inner left/right padding                  */
#define FD_PB_SEG_GAP 8     /* text-to-chevron breathing room            */
#define FD_PB_CHEV_W 14     /* the chevron slot between segments         */

typedef struct fdk_path_bar {
    fdk_widget base;
    fdk_file_dialog *dialog; /* borrowed (the body outlives the bar) */
    fdk_font *font;          /* borrowed from the dialog              */
    /* Parsed segments: display label + the absolute prefix clicking
     * the segment browses to. seg_count == 0 only before the first
     * set_path. */
    char *seg_text[FD_PB_MAX_SEGS];
    char *seg_path[FD_PB_MAX_SEGS];
    size_t seg_count;
    /* Visual layout, rebuilt at arrange-time for the current width:
     * first_visible = the leading segments replaced by the ellipsis
     * crumb (0 = none collapsed), x/w per VISIBLE crumb, and the
     * chevron x between crumb i and i+1. Local coordinates. */
    size_t first_visible;
    fdk_i32 crumb_x[FD_PB_MAX_SEGS + 1]; /* +1 = the ellipsis crumb   */
    fdk_i32 crumb_w[FD_PB_MAX_SEGS + 1];
    fdk_i32 chev_x[FD_PB_MAX_SEGS];
    int hover_crumb; /* -1 none; index into the VISIBLE crumb table */
    bool pressed;
} fdk_path_bar;

static fdk_path_bar *pbar_of(fdk_widget *w) {
    return (fdk_path_bar *)(void *)w;
}

/* Frees the parsed segments (the visual table is indices, owned by
 * nothing). Called from set_path and the destroy hook. */
static void pbar_clear_segments(fdk_path_bar *pb) {
    for (size_t i = 0; i < pb->seg_count; i++) {
        fdk_free(pb->seg_text[i]);
        fdk_free(pb->seg_path[i]);
    }
    pb->seg_count = 0;
    pb->hover_crumb = -1;
}

/* Forward: set_path relayouts the new crumbs for the current width;
 * the layout pass is defined below it. */
static void pbar_relayout(fdk_path_bar *pb, fdk_i32 width);

/* Splits `dir` into crumbs. Each label is one path component; the
 * root crumb ("/") leads absolute paths. Allocation failure keeps
 * the previous segments (a stale breadcrumb beats a blank bar). */
static void pbar_set_path(fdk_path_bar *pb, const char *dir) {
    if (dir == NULL || dir[0] == '\0') {
        pbar_clear_segments(pb);
        fdk_widget_invalidate(&pb->base);
        return;
    }
    char *texts[FD_PB_MAX_SEGS];
    char *paths[FD_PB_MAX_SEGS];
    size_t n = 0;
    size_t consumed = 0;
    bool absolute = dir[0] == '/';
    if (absolute) {
        texts[0] = fdk__strdup("/");
        paths[0] = fdk__strdup("/");
        if (texts[0] == NULL || paths[0] == NULL) {
            fdk_free(texts[0]);
            fdk_free(paths[0]);
            return; /* OOM: keep the old crumbs */
        }
        n = 1;
        consumed = 1;
    }
    while (n < FD_PB_MAX_SEGS && dir[consumed] != '\0') {
        size_t end = consumed;
        while (dir[end] != '\0' && dir[end] != '/') {
            end++;
        }
        if (end > consumed) { /* skip empty components (//, trailing) */
            /* The prefix includes everything up to (not past) this
             * component; absolute prefixes get their slash back. */
            size_t prefix_len = end + (absolute && consumed > 1 ? 1 : 0);
            char *text = fdk_alloc(end - consumed + 1);
            char *path = fdk_alloc(prefix_len + 1);
            if (text == NULL || path == NULL) {
                fdk_free(text);
                fdk_free(path);
                break; /* keep the crumbs that fit */
            }
            memcpy(text, dir + consumed, end - consumed);
            text[end - consumed] = '\0';
            memcpy(path, dir, prefix_len);
            path[prefix_len] = '\0';
            texts[n] = text;
            paths[n] = path;
            n++;
        }
        /* Advance past this component's separator — but never past
         * the NUL: when `end` sits ON the terminator (last component,
         * no trailing slash), park there so the loop condition reads
         * dir[strlen] ('\0') and exits instead of walking one byte
         * off the end of the string. */
        consumed = (dir[end] == '/') ? end + 1 : end;
    }
    pbar_clear_segments(pb);
    for (size_t i = 0; i < n; i++) {
        pb->seg_text[i] = texts[i];
        pb->seg_path[i] = paths[i];
    }
    pb->seg_count = n;
    pb->first_visible = 0;
    /* New crumbs need a visual layout for the CURRENT width. The
     * arrange hook relayouts on size changes only — it cannot see
     * content changes (and at first arrange the segments were not
     * parsed yet: construction arranges BEFORE the first reload).
     * Width 0 (never placed) skips: the arrange will lay them out. */
    if (pb->base.bounds.width > 0) {
        pbar_relayout(pb, pb->base.bounds.width);
    }
    fdk_widget_invalidate(&pb->base);
}

/* Rebuilds the visual layout for `width`. Collapses leading crumbs
 * (never the last two: the ellipsis plus the current folder must
 * always show) when the full run does not fit. */
static void pbar_relayout(fdk_path_bar *pb, fdk_i32 width) {
    pb->first_visible = 0;
    if (pb->font == NULL || pb->seg_count == 0) {
        return;
    }
    fdk_i32 avail = width - FD_PB_PAD_X * 2;
    if (avail < 0) {
        avail = 0;
    }

    /* Total width of every crumb + the separators between them. */
    fdk_i32 seg_w[FD_PB_MAX_SEGS];
    fdk_i32 total = 0;
    for (size_t i = 0; i < pb->seg_count; i++) {
        fdk_i32 tw = 0, th = 0;
        fdk__text_extent(pb->font, pb->seg_text[i], &tw, &th);
        seg_w[i] = tw;
        total += tw;
        if (i + 1 < pb->seg_count) {
            total += FD_PB_CHEV_W + FD_PB_SEG_GAP * 2;
        }
    }

    if (total <= avail) {
        fdk_i32 x = FD_PB_PAD_X;
        for (size_t i = 0; i < pb->seg_count; i++) {
            pb->crumb_x[i] = x;
            pb->crumb_w[i] = seg_w[i];
            x += seg_w[i] + FD_PB_SEG_GAP;
            if (i + 1 < pb->seg_count) {
                pb->chev_x[i] = x;
                x += FD_PB_CHEV_W - FD_PB_SEG_GAP * 0;
                x += FD_PB_SEG_GAP;
            }
        }
        return;
    }

    /* Overflow: measure the ellipsis crumb, then keep dropping
     * leading crumbs (after the root, which the ellipsis replaces
     * for absolute paths anyway) until the run fits. Never collapse
     * into the last crumb. */
    fdk_i32 ell_w = 0, ell_h = 0;
    fdk__text_extent(pb->font, "...", &ell_w, &ell_h);
    fdk_i32 ell_slot = ell_w + FD_PB_CHEV_W + FD_PB_SEG_GAP * 2;
    size_t first = 1; /* the root is subsumed by "..." when collapsing */
    while (first + 1 < pb->seg_count) {
        fdk_i32 run = ell_slot;
        for (size_t i = first; i < pb->seg_count; i++) {
            run += seg_w[i];
            if (i + 1 < pb->seg_count) {
                run += FD_PB_CHEV_W + FD_PB_SEG_GAP * 2;
            }
        }
        if (run <= avail) {
            break;
        }
        first++;
    }
    pb->first_visible = first;

    fdk_i32 x = FD_PB_PAD_X;
    /* The ellipsis crumb sits at index FD_PB_MAX_SEGS in the tables. */
    pb->crumb_x[FD_PB_MAX_SEGS] = x;
    pb->crumb_w[FD_PB_MAX_SEGS] = ell_w;
    x += ell_w + FD_PB_SEG_GAP;
    pb->chev_x[0] = x;
    x += FD_PB_CHEV_W + FD_PB_SEG_GAP;
    for (size_t i = first; i < pb->seg_count; i++) {
        pb->crumb_x[i] = x;
        pb->crumb_w[i] = seg_w[i];
        x += seg_w[i] + FD_PB_SEG_GAP;
        if (i + 1 < pb->seg_count) {
            pb->chev_x[i] = x;
            x += FD_PB_CHEV_W + FD_PB_SEG_GAP;
        }
    }
}

/* The VISIBLE crumb count: the tail segments plus (when collapsed)
 * the ellipsis crumb. Visible index v maps: FD_PB_MAX_SEGS = "..."
 * (browses to the first collapsed crumb), else segment v. */
static size_t pbar_visible_count(const fdk_path_bar *pb) {
    if (pb->seg_count == 0) {
        return 0;
    }
    return pb->seg_count - pb->first_visible +
           (pb->first_visible > 0 ? 1 : 0);
}

static void pbar_paint(fdk_widget *w, fdk_surface *surface,
                       fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    fdk_path_bar *pb = pbar_of(w);
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    /* Field chrome: the flat-entry pair, rounded at the entry radius. */
    fdk_i32 radius =
        fdk_theme_get_metric(NULL, FDK_TM_ENTRY_CORNER_RADIUS);
    fdk_surface_fill_rounded_rect(surface, bounds, radius,
                                  fdk__pal_entry());
    fdk_surface_draw_rounded_rect(surface, bounds, radius,
                                  fdk__pal_entry_border());

    if (pb->font == NULL || pb->seg_count == 0) {
        return;
    }
    fdk_i32 baseline =
        fdk__center_baseline(pb->font, bounds.y, bounds.height);
    fdk_i32 cy = bounds.y + bounds.height / 2;
    size_t visible = pbar_visible_count(pb);
    fdk_color chev = fdk__pal_border();

    for (size_t v = 0; v < visible; v++) {
        bool is_ell = (v == 0 && pb->first_visible > 0);
        size_t seg = is_ell ? 0 : pb->first_visible + v -
                                      (pb->first_visible > 0 ? 1 : 0);
        const char *text = is_ell ? "..." : pb->seg_text[seg];
        fdk_i32 x = pb->crumb_x[v];
        fdk_i32 cw = pb->crumb_w[v];

        /* Hover pill under the clickable crumb (rounded, inset). */
        if ((fdk_i32)v == pb->hover_crumb || pb->pressed) {
            fdk_rect pill = {bounds.x + x - 4, bounds.y + 2,
                             cw + 8, bounds.height - 4};
            if (pill.width > 0 && pill.height > 0) {
                fdk_surface_fill_rounded_rect(
                    surface, pill, radius > 2 ? radius - 2 : 2,
                    fdk__pal_row_hover());
            }
        }

        /* The CURRENT directory crumb reads in the accent color —
         * the one you are "in"; earlier crumbs are plain text. */
        bool current = (seg + 1 == pb->seg_count) && !is_ell;
        fdk__draw_text(surface, pb->font, text,
                       current ? fdk__pal_accent() : fdk__pal_text(),
                       bounds.x + x, baseline);

        /* The chevron AFTER this crumb (not after the last). */
        if (v + 1 < visible) {
            fdk_i32 chx = bounds.x + pb->chev_x[is_ell ? 0 : seg];
            fdk_surface_draw_line(surface, chx - 3, cy - 2, chx, cy + 1,
                                  chev);
            fdk_surface_draw_line(surface, chx, cy + 1, chx + 3, cy - 2,
                                  chev);
        }
    }
}

/* Local-x -> visible-crumb index (-1 = gap/chrome). */
static int pbar_hit(const fdk_path_bar *pb, fdk_f32 x) {
    if (x < (fdk_f32)FD_PB_PAD_X) {
        return -1;
    }
    size_t visible = pbar_visible_count(pb);
    for (size_t v = 0; v < visible; v++) {
        fdk_i32 x0 = pb->crumb_x[v];
        fdk_i32 x1 = x0 + pb->crumb_w[v];
        if (x >= (fdk_f32)x0 && x <= (fdk_f32)x1) {
            return (int)v;
        }
    }
    return -1;
}

/* fdlg_browse is defined below (the browsing section); the bar
 * navigates through it. */
static void fdlg_browse(fdk_file_dialog *d, const char *dir);

static bool pbar_handle_event(fdk_widget *w,
                              const fdk_widget_event *ev) {
    fdk_path_bar *pb = pbar_of(w);
    switch (ev->type) {
    case FDK_WIDGET_POINTER_MOTION:
    case FDK_WIDGET_POINTER_ENTER: {
        int hit = pbar_hit(pb, ev->pointer.position.x);
        if (hit != pb->hover_crumb) {
            pb->hover_crumb = hit;
            fdk_widget_invalidate(w);
        }
        return false; /* hover only; the bar eats nothing */
    }
    case FDK_WIDGET_POINTER_LEAVE:
        if (pb->hover_crumb != -1) {
            pb->hover_crumb = -1;
            fdk_widget_invalidate(w);
        }
        return false;
    case FDK_WIDGET_POINTER_DOWN:
        if (ev->pointer.button == FDK_POINTER_BUTTON_LEFT) {
            pb->pressed = true;
            fdk_widget_invalidate(w);
            return true;
        }
        return false;
    case FDK_WIDGET_POINTER_UP: {
        bool was = pb->pressed;
        pb->pressed = false;
        fdk_widget_invalidate(w);
        if (!was || ev->pointer.button != FDK_POINTER_BUTTON_LEFT) {
            return true;
        }
        /* Release inside the bar on a crumb = navigate there. */
        if (ev->pointer.position.x >= 0.0f &&
            ev->pointer.position.y >= 0.0f &&
            ev->pointer.position.x < (fdk_f32)w->bounds.width &&
            ev->pointer.position.y < (fdk_f32)w->bounds.height) {
            int v = pbar_hit(pb, ev->pointer.position.x);
            if (v >= 0 && pb->dialog != NULL) {
                bool is_ell = (v == 0 && pb->first_visible > 0);
                size_t seg = is_ell
                    ? 1 /* the first collapsed crumb */
                    : pb->first_visible + (size_t)v -
                          (pb->first_visible > 0 ? 1 : 0);
                if (seg < pb->seg_count) {
                    fdlg_browse(pb->dialog, pb->seg_path[seg]);
                }
            }
        }
        return true;
    }
    default:
        return false;
    }
}

static void pbar_measure(fdk_widget *w, fdk_size *out) {
    fdk_path_bar *pb = pbar_of(w);
    out->width = FD_PB_PAD_X * 2 + 120; /* the row is flexible-width */
    fdk_i32 th = 20;
    if (pb->font != NULL) {
        fdk_font_metrics m;
        fdk_font_get_metrics(pb->font, &m);
        th = m.ascent + m.descent + 8;
    }
    out->height = th > FD_ROW_H ? th : FD_ROW_H;
}

static void pbar_arrange(fdk_widget *w, fdk_rect assigned) {
    fdk_widget_set_bounds(w, assigned);
    pbar_relayout(pbar_of(w), assigned.width);
}

static void pbar_destroy(fdk_widget *w) {
    pbar_clear_segments(pbar_of(w));
}

static const fdk_widget_class fdk_path_bar_class = {
    .size = sizeof(fdk_path_bar),
    .name = "path-bar",
    .handle_event = pbar_handle_event,
    .paint = pbar_paint,
    .measure = pbar_measure,
    .arrange = pbar_arrange,
    .destroy = pbar_destroy,
};

/* Syncs the path bar to d->dir — unless the user is mid-typing in
 * it (focus check): never clobber a path being entered. */
static void fdlg_sync_path_bar(fdk_file_dialog *d) {
    if (d->path_entry != NULL &&
        !fdk_widget_has_focus(d->path_entry)) {
        (void)fdk_entry_set_text(d->path_entry, d->dir);
    }
    if (d->path_bar != NULL) {
        pbar_set_path(pbar_of(d->path_bar), d->dir);
    }
}

/* 1.4.0 — Ctrl+L: swap the breadcrumb bar for the raw location Entry
 * (and back). Entering location mode seeds the entry with the current
 * directory, shows it, hides the bar, and focuses the entry with the
 * whole path selected (type-over replaces; arrows edit). Leaving
 * returns focus to the file list (the keyboard browsing home) and
 * restores the breadcrumbs — the entry's text survives in case the
 * user toggles back (GTK parity: nothing is lost by peeking). */
static void fdlg_set_location_mode(fdk_file_dialog *d, bool on) {
    if (d->path_bar == NULL || d->path_entry == NULL ||
        d->location_mode == on) {
        return;
    }
    d->location_mode = on;
    if (on) {
        (void)fdk_entry_set_text(d->path_entry, d->dir);
        fdk_entry_select_all(d->path_entry);
        fdk_widget_set_visible(d->path_bar, false);
        fdk_widget_set_visible(d->path_entry, true);
        fdk_widget_focus(d->path_entry);
    } else {
        fdk_widget_set_visible(d->path_entry, false);
        fdk_widget_set_visible(d->path_bar, true);
        fdk_widget_focus(d->list);
    }
    /* The row swaps in place; re-run the body's placement (the body
     * is the window content — nothing above it re-arranges on a
     * visibility flip, so the dialog drives its own placement). */
    fdk_widget_arrange(d->body, fdk_widget_get_bounds(d->body));
}

/* Re-scans the CURRENT directory (hidden toggle flips, filter
 * changes). On failure the previous listing stays (stale but
 * harmless) and the status line says why — the working directory
 * is never abandoned by a reload. */
static void fdlg_reload(fdk_file_dialog *d) {
    size_t np = 0;
    char **pats = fdlg_active_patterns(d, &np);
    fdk_fd_entries fresh;
    if (fdk__file_dialog_scan(d->dir, fdlg_kind_folders(d->kind),
                              d->show_hidden, pats, np, &fresh) != 0) {
        fdk__file_dialog_entries_free(&fresh);
        fdlg_set_status(d, "Cannot open this directory");
        fdlg_sync_path_bar(d);
        return;
    }
    fdk__file_dialog_entries_free(&d->entries);
    d->entries = fresh;
    fdlg_fill_list(d);
    fdlg_sync_path_bar(d);
    char status[96];
    snprintf(status, sizeof(status), "%zu item%s", d->entries.count,
             d->entries.count == 1 ? "" : "s");
    fdlg_set_status(d, status);
}

/* Navigates to `dir`. The target is probed FIRST: only a directory
 * that actually opens replaces d->dir — a failed browse leaves the
 * dialog browsing where it was, with the reason in the status. */
static void fdlg_browse(fdk_file_dialog *d, const char *dir) {
    char *norm = fdk__path_normalize_dir(dir);
    if (norm == NULL) {
        fdlg_set_status(d, "Cannot open that path");
        return;
    }
    size_t np = 0;
    char **pats = fdlg_active_patterns(d, &np);
    fdk_fd_entries probe;
    if (fdk__file_dialog_scan(norm, fdlg_kind_folders(d->kind),
                              d->show_hidden, pats, np, &probe) != 0) {
        fdk__file_dialog_entries_free(&probe);
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot open %s", norm);
        fdlg_set_status(d, msg);
        fdk_free(norm);
        return;
    }
    fdk_free(d->dir);
    d->dir = norm;
    fdk__file_dialog_entries_free(&d->entries);
    d->entries = probe;
    fdlg_fill_list(d);
    fdlg_sync_path_bar(d);
    char status[96];
    snprintf(status, sizeof(status), "%zu item%s", d->entries.count,
             d->entries.count == 1 ? "" : "s");
    fdlg_set_status(d, status);
}

static void fdlg_up(fdk_file_dialog *d) {
    size_t len = strlen(d->dir);
    if (len <= 1) {
        return; /* already at / */
    }
    char *parent = fdk__strdup(d->dir);
    if (parent == NULL) {
        return;
    }
    char *slash = strrchr(parent + 1, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        parent[0] = '/';
        parent[1] = '\0';
    }
    fdlg_browse(d, parent);
    fdk_free(parent);
}

static void fdlg_home(fdk_file_dialog *d) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        home = "/";
    }
    fdlg_browse(d, home);
}

/* ---- response path ---- */

static void fdlg_respond(fdk_file_dialog *d,
                         const fdk_file_dialog_result *result) {
    if (d->answered) {
        return;
    }
    d->answered = true;
    if (d->on_done != NULL) {
        d->on_done(result, d->on_done_user);
    }
    /* The pending result's strings are freed by the body destroy
     * hook — the callback has already copied what it needs. */
    if (d->window != NULL) {
        fdk_window *win = d->window;
        d->window = NULL;
        fdk_window_destroy(win); /* releases the modal grab too */
    }
}

static void fdlg_cancelled(fdk_file_dialog *d) {
    fdk_file_dialog_result r = {
        .outcome = FDK_FILE_DIALOG_CANCELLED,
        .paths = NULL,
        .count = 0,
    };
    fdlg_respond(d, &r);
}

static void fdlg_save_accept(fdk_file_dialog *d);

/* Builds absolute, canonical paths for the selected rows, re-validates
 * each against the kind's contract with stat(), and accepts. */
static void fdlg_try_accept(fdk_file_dialog *d) {
    if (d->answered) {
        return;
    }
    if (fdlg_kind_save(d->kind)) {
        fdlg_save_accept(d);
        return;
    }
    size_t sel_count = fdk_list_selected_count(d->list);
    if (sel_count == 0) {
        fdlg_set_status(d, "Nothing selected");
        return;
    }

    /* Collect selected entries; a lone directory means "descend" in
     * the FILE kinds (the standard file-dialog affordance). In the
     * FOLDER kinds Open means SELECT: the selected directory is the
     * answer (double-click still descends — activation, not Open). */
    size_t *rows = fdk_alloc_array(sel_count, sizeof(size_t));
    if (rows == NULL) {
        return;
    }
    size_t n = 0;
    for (size_t p = 0; p < sel_count; p++) {
        size_t row = 0;
        if (fdk_list_selected_at(d->list, p, &row) == FDK_OK &&
            row < d->entries.count) {
            rows[n++] = row;
        }
    }
    if (n == 1 && d->entries.v[rows[0]].dir &&
        !fdlg_kind_folders(d->kind)) {
        char *path = fdk__path_join(d->dir, d->entries.v[rows[0]].name);
        fdk_free(rows);
        if (path != NULL) {
            fdlg_browse(d, path);
            fdk_free(path);
        }
        return;
    }

    /* Multi kinds with a single selection answer one path; single
     * kinds with multiple (impossible via the UI, possible via the
     * programmatic API) take the first — honest and bounded. */
    size_t take = fdlg_kind_multi(d->kind) ? n : (n > 0 ? 1 : 0);
    char **paths = fdk_alloc_array(take, sizeof(char *));
    if (paths == NULL) {
        fdk_free(rows);
        return;
    }
    size_t filled = 0;
    for (size_t i = 0; i < take; i++) {
        char *full = fdk__path_join(d->dir, d->entries.v[rows[i]].name);
        if (full == NULL) {
            goto fail;
        }
        char resolved[FD_PATH_BUF];
        const char *use = full;
        if (realpath(full, resolved) != NULL) {
            use = resolved;
        }
        struct stat st;
        if (stat(use, &st) != 0) {
            fdlg_set_status(d, "Selection vanished — reselect");
            fdk_free(full);
            goto fail;
        }
        bool ok = fdlg_kind_folders(d->kind) ? S_ISDIR(st.st_mode)
                                             : S_ISREG(st.st_mode);
        if (!ok) {
            fdlg_set_status(d, fdlg_kind_folders(d->kind)
                                   ? "Not a folder"
                                   : "Not a regular file");
            fdk_free(full);
            goto fail;
        }
        paths[filled] = fdk__strdup(use);
        fdk_free(full);
        if (paths[filled] == NULL) {
            goto fail;
        }
        filled++;
    }
    fdk_free(rows);

    fdk_file_dialog_result r = {
        .outcome = FDK_FILE_DIALOG_ACCEPTED,
        .paths = paths,
        .count = filled,
    };

    /* Ownership moves into d->pending (freed by the body destroy
     * hook after the callback returns). */
    d->pending.paths = paths;
    d->pending.count = filled;
    d->pending.outcome = FDK_FILE_DIALOG_ACCEPTED;
    fdlg_respond(d, &r);
    return;

fail:
    for (size_t i = 0; i < filled; i++) {
        fdk_free(paths[i]);
    }
    fdk_free(paths);
    fdk_free(rows);
}

/* ---- SAVE acceptance ---- */

/* Accepts one path: moves a copy into the pending result and
 * answers ACCEPTED. */
static void fdlg_accept_path(fdk_file_dialog *d, const char *path) {
    char **paths = fdk_alloc_array(1, sizeof(char *));
    if (paths == NULL) {
        return;
    }
    paths[0] = fdk__strdup(path);
    if (paths[0] == NULL) {
        fdk_free(paths);
        return;
    }
    d->pending.paths = paths;
    d->pending.count = 1;
    d->pending.outcome = FDK_FILE_DIALOG_ACCEPTED;
    fdk_file_dialog_result r = {
        .outcome = FDK_FILE_DIALOG_ACCEPTED,
        .paths = paths,
        .count = 1,
    };
    fdlg_respond(d, &r);
}

/* Canonicalizes the PARENT of `target` (realpath) while keeping the
 * leaf exactly as typed, then accepts. The parent is the dialog's
 * own working directory, so a relative-path trick cannot smuggle
 * the write outside what the user sees. */
static void fdlg_accept_canonical(fdk_file_dialog *d, const char *target) {
    const char *leaf = strrchr(target, '/');
    leaf = (leaf != NULL) ? leaf + 1 : target;
    char parent[FD_PATH_BUF];
    size_t plen = (size_t)(leaf - target); /* includes the slash */
    if (plen == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen >= sizeof(parent)) {
            return; /* cannot happen (dir came from normalized) */
        }
        memcpy(parent, target, plen - 1);
        parent[plen - 1] = '\0';
    }
    char resolved[FD_PATH_BUF];
    const char *pdir = parent;
    if (realpath(parent, resolved) != NULL) {
        pdir = resolved;
    }
    char *full = fdk__path_join(pdir, leaf);
    if (full == NULL) {
        return;
    }
    fdlg_accept_path(d, full);
    fdk_free(full);
}

/* The nested overwrite dialog's answer. Runs once, from dispatch;
 * frees its heap context on every path. */
static void fdlg_overwrite_response(fdk_dialog_response response,
                                    void *user) {
    fdk_save_confirm_ctx *ctx = user;
    fdk_file_dialog *d = ctx->dlg;
    if (d->tearing_down) {
        /* The file dialog is mid-destroy; `d` is being freed —
         * touch nothing but this context. */
        fdk_free(ctx->target);
        fdk_free(ctx);
        return;
    }
    d->confirm_win = NULL;
    if (response == FDK_DIALOG_YES) {
        fdlg_accept_canonical(d, ctx->target);
        /* NOTE: accept may have destroyed `d` (body destroy runs
         * inside fdlg_respond) — only the context is touched
         * below, never `d`. */
    } else {
        fdlg_set_status(d, "Overwrite declined");
        if (d->was_modal && d->window != NULL) {
            /* X11: the confirm held the grab; take it back so the
             * file dialog stays modal until it answers. (Wayland:
             * no-op by design — non-modal there.) */
            (void)fdk__window_set_modal(d->window, true);
        }
    }
    fdk_free(ctx->target);
    fdk_free(ctx);
}

static void fdlg_save_accept(fdk_file_dialog *d);

/* The Save button / Enter path. Every failure is a status message
 * that keeps the dialog up — a save dialog never guesses. */
static void fdlg_save_accept(fdk_file_dialog *d) {
    const char *name = fdk_entry_get_text(d->name_entry);
    switch (fdk__save_name_validate(name)) {
    case 1:
    case 5:
        fdlg_set_status(d, "Enter a file name");
        return;
    case 2:
        fdlg_set_status(d, "The name cannot contain '/'");
        return;
    case 3:
        fdlg_set_status(d, "Choose a real file name");
        return;
    case 4:
        fdlg_set_status(d, "Name is too long (max 255 bytes)");
        return;
    default:
        break;
    }
    struct stat st;
    if (stat(d->dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fdlg_set_status(d, "The current folder no longer exists");
        return;
    }
    char *target = fdk__path_join(d->dir, name);
    if (target == NULL) {
        return;
    }
    if (stat(target, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            fdlg_set_status(d, "A folder with that name exists");
            fdk_free(target);
            return;
        }
        if (!S_ISREG(st.st_mode)) {
            fdlg_set_status(d, "Not a regular file");
            fdk_free(target);
            return;
        }
        /* Existing regular file: ask before overwriting. */
        fdk_save_confirm_ctx *ctx = fdk_alloc(sizeof(*ctx));
        if (ctx == NULL) {
            fdk_free(target);
            return;
        }
        ctx->dlg = d;
        ctx->target = target;
        char text[600];
        snprintf(text, sizeof(text),
                 "The file \"%s\" already exists.\nOverwrite it?", name);
        fdk_dialog_options opts = {
            .title = "Overwrite file?",
            .text = text,
            .buttons = FDK_DIALOG_BUTTONS_YES_NO,
            .modal = d->was_modal,
            .font = NULL,
            .parent = d->window,
        };
        fdk_result r = fdk_dialog_show_message(
            d->ctx, &opts, fdlg_overwrite_response, ctx,
            &d->confirm_win);
        if (!fdk_ok(r)) {
            d->confirm_win = NULL;
            fdk_free(ctx->target);
            fdk_free(ctx);
            fdlg_set_status(d, "Could not ask about overwriting");
        }
        return;
    }
    if (errno != ENOENT) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Cannot check %s", target);
        fdlg_set_status(d, msg);
        fdk_free(target);
        return;
    }
    /* Fresh file: accept with a canonical parent. */
    fdlg_accept_canonical(d, target);
    fdk_free(target);
}

/* ---- widget callbacks ---- */

static void fdlg_up_clicked(fdk_widget *w, void *user) {
    (void)w;
    fdlg_up(user);
}

static void fdlg_home_clicked(fdk_widget *w, void *user) {
    (void)w;
    fdlg_home(user);
}

static void fdlg_hidden_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w;
    fdk_file_dialog *d = user;
    d->show_hidden = checked;
    fdlg_reload(d);
}

static void fdlg_filter_changed(fdk_widget *combo, size_t index,
                                void *user) {
    (void)combo;
    fdk_file_dialog *d = user;
    d->active_filter = index;
    fdlg_reload(d);
}

/* Path-bar Enter: browse what was typed ("~"-expanded; relative
 * paths resolve against the CURRENT directory — the mental model a
 * path bar showing an absolute path implies). */
static void fdlg_path_activated(fdk_widget *entry, void *user) {
    (void)entry;
    fdk_file_dialog *d = user;
    const char *text = fdk_entry_get_text(d->path_entry);
    char *expanded = fdk__path_expand_tilde(text);
    if (expanded == NULL) {
        return;
    }
    char *target = NULL;
    if (expanded[0] == '/') {
        target = fdk__path_normalize_dir(expanded);
    } else if (expanded[0] != '\0') {
        char *joined = fdk__path_join(d->dir, expanded);
        target = (joined != NULL) ? fdk__path_normalize_dir(joined)
                                  : NULL;
        fdk_free(joined);
    }
    fdk_free(expanded);
    if (target == NULL) {
        fdlg_set_status(d, "Enter a folder path");
        return;
    }
    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Not a folder: %s", target);
        fdlg_set_status(d, msg);
        fdk_free(target);
        return;
    }
    fdlg_browse(d, target);
    fdk_free(target);
    /* A typed location is a one-shot: browse, then back to the
     * breadcrumbs (the GTK rhythm — the entry is for getting
     * somewhere the crumbs cannot reach, not a mode to live in). */
    fdlg_set_location_mode(d, false);
}

static void fdlg_place_activated(fdk_widget *list, size_t row,
                                 void *user) {
    (void)list;
    fdk_file_dialog *d = user;
    if (row >= d->place_count) {
        return;
    }
    fdlg_browse(d, d->places[row].path);
}

/* Single-click selection in SAVE mode: picking a file copies its
 * name into the Name row (the native "save over that one" flow);
 * picking a directory does not (Open means navigate there). */
static void fdlg_selection_changed(fdk_widget *list, void *user) {
    (void)list;
    fdk_file_dialog *d = user;
    if (!fdlg_kind_save(d->kind) || d->name_entry == NULL) {
        return;
    }
    fdk_i64 sel = fdk_list_get_selected(d->list);
    if (sel >= 0 && (size_t)sel < d->entries.count &&
        !d->entries.v[sel].dir) {
        (void)fdk_entry_set_text(d->name_entry, d->entries.v[sel].name);
    }
}

static void fdlg_row_activated(fdk_widget *list, size_t row, void *user) {
    (void)list;
    fdk_file_dialog *d = user;
    if (row >= d->entries.count) {
        return;
    }
    if (d->entries.v[row].dir) {
        char *path = fdk__path_join(d->dir, d->entries.v[row].name);
        if (path != NULL) {
            fdlg_browse(d, path);
            fdk_free(path);
        }
    } else if (fdlg_kind_save(d->kind)) {
        /* Double-click on a file in Save: fill the name, then try
         * to accept (an existing file goes through the overwrite
         * ask) — the efficient "save over that one" gesture. */
        (void)fdk_entry_set_text(d->name_entry, d->entries.v[row].name);
        fdlg_save_accept(d);
    } else {
        fdlg_try_accept(d);
    }
}

static void fdlg_accept_clicked(fdk_widget *w, void *user) {
    (void)w;
    fdlg_try_accept(user);
}

static void fdlg_cancel_clicked(fdk_widget *w, void *user) {
    (void)w;
    fdlg_cancelled(user);
}

/* The dialog window's event callback: window-level semantics only. */
static void fdlg_window_event(fdk_window *window,
                              const fdk_event_data *ev, void *user) {
    (void)window;
    fdk_file_dialog *d = user;
    if (d->confirm_win != NULL) {
        /* The overwrite ask is up: input belongs to IT. (X11 keeps
         * this honest with the grab; Wayland/dialogs cannot grab —
         * this guard is the backstop, not the mechanism.) */
        return;
    }
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdlg_cancelled(d);
        return;
    }
    if (ev->type == FDK_EVENT_KEY_DOWN) {
        /* Ctrl+L toggles the location entry (1.4.0, the GTK file
         * chooser's binding). Matched on the PHYSICAL L (evdev 38)
         * with exactly Ctrl held — the shortcut-scancode discipline:
         * layout-stable, and no dependence on what codepoint Ctrl+L
         * resolves to on any backend. */
        if ((ev->key.modifiers & FDK_MOD_CTRL) != 0 &&
            (ev->key.modifiers & ~(fdk_u32)FDK_MOD_CTRL) == 0 &&
            ev->key.scancode == FD_KEY_L_EVDEV) {
            fdlg_set_location_mode(d, !d->location_mode);
            return;
        }
        if (ev->key.scancode == FDK_KEY_ESC) {
            if (d->location_mode) {
                /* Esc in the location entry abandons the edit —
                 * back to breadcrumbs, NOT a dialog cancel (the
                 * GTK behavior; the entry does not consume Esc
                 * itself, so the window hook sees it). */
                fdlg_set_location_mode(d, false);
            } else {
                fdlg_cancelled(d);
            }
        }
    }
}

/* Destroy-notify: died without answering (app teardown paths). The
 * documented contract: that answers CANCELLED (the dialog never
 * claimed a selection). */
static void fdlg_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_file_dialog *d = user;
    if (d->answered || !d->surfaced) {
        return; /* already answered, or failed construction: silent */
    }
    d->answered = true;
    if (d->on_done != NULL) {
        fdk_file_dialog_result r = {
            .outcome = FDK_FILE_DIALOG_CANCELLED,
            .paths = NULL,
            .count = 0,
        };
        d->on_done(&r, d->on_done_user);
    }
}

/* ---- body hooks ---- */

/* The body's own paint: the dialog's two surfaces. Everything is the
 * window surface; the places sidebar sits on the sidebar surface (the
 * half-step-off panel tone). Painting this here — instead of setting
 * a per-widget background snapshot — keeps both surfaces resolving
 * through the CURRENT theme at paint time, so a re-themed dialog
 * actually changes color (the set_background snapshot would not).
 * Children paint on top of these fills (the normal tree walk). */
static void fdlg_body_paint(fdk_widget *w, fdk_surface *surface,
                            fdk_rect bounds, fdk_rect clip) {
    (void)clip;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }
    fdk_surface_fill_rect(surface, bounds,
                          fdk_theme_get_color(NULL,
                                              FDK_TK_WINDOW_BACKGROUND));
    fdk_rect sb = fbody_of(w)->sidebar_rect;
    if (sb.width > 0 && sb.height > 0) {
        fdk_surface_fill_rect(surface, sb, fdk__pal_sidebar());
    }
}

static void fdlg_body_arrange(fdk_widget *w, fdk_rect a) {
    fdk_widget_set_bounds(w, a);
    fdk_file_dialog *d = fdlg_of_body(w);
    if (d == NULL) {
        return;
    }
    fdk_i32 x = a.x + FD_PAD;
    fdk_i32 y = a.y + FD_PAD;
    fdk_i32 iw = a.width - FD_PAD * 2;

    /* Toolbar: Up + Home + toggle on the left, sized by measure;
     * the filter combo right-aligned. */
    fdk_size up_n = {0, 0};
    fdk_widget_measure(d->up_btn, &up_n);
    fdk_widget_set_bounds(
        d->up_btn, (fdk_rect){x, y, up_n.width, FD_TOPBAR_H});
    fdk_i32 tx = x + up_n.width + FD_GAP;
    fdk_size home_n = {0, 0};
    fdk_widget_measure(d->home_btn, &home_n);
    fdk_widget_set_bounds(
        d->home_btn, (fdk_rect){tx, y, home_n.width, FD_TOPBAR_H});
    tx += home_n.width + FD_GAP;
    fdk_size hid_n = {0, 0};
    fdk_widget_measure(d->hidden_toggle, &hid_n);
    fdk_widget_set_bounds(d->hidden_toggle,
                          (fdk_rect){tx, y, hid_n.width, FD_TOPBAR_H});
    fdk_i32 combo_h = FD_TOPBAR_H - 6;
    fdk_widget_set_bounds(
        d->filter_combo,
        (fdk_rect){a.x + a.width - FD_PAD - FD_COMBO_W,
                   y + (FD_TOPBAR_H - combo_h) / 2, FD_COMBO_W,
                   combo_h});
    y += FD_TOPBAR_H + FD_GAP / 2;

    /* Path row: the breadcrumb bar at rest, the location Entry in
     * Ctrl+L mode — one row, one occupant, same slot either way.
     * The bar needs its crumb layout rebuilt for the placed width
     * (set_bounds does not run the arrange hook, and the body owns
     * placement for every child — same rule as the rows below). */
    fdk_widget *path_row = d->location_mode ? d->path_entry
                                             : d->path_bar;
    fdk_size path_n = {0, 0};
    fdk_widget_measure(path_row, &path_n);
    fdk_i32 path_h = path_n.height > 0 ? path_n.height : FD_ROW_H;
    fdk_widget_set_bounds(path_row, (fdk_rect){x, y, iw, path_h});
    if (path_row == d->path_bar) {
        pbar_relayout(pbar_of(d->path_bar), iw);
    }
    y += path_h + FD_GAP / 2;

    /* Buttons: bottom-right; status line bottom-left. */
    fdk_size acc_n = {0, 0}, can_n = {0, 0};
    fdk_widget_measure(d->accept_btn, &acc_n);
    fdk_widget_measure(d->cancel_btn, &can_n);
    fdk_i32 btn_h = (acc_n.height > can_n.height) ? acc_n.height
                                                  : can_n.height;
    fdk_i32 status_y = a.y + a.height - FD_PAD - FD_STATUS_H;
    fdk_i32 btn_y = status_y - FD_GAP - btn_h;

    /* Name row (SAVE only), above the status area. */
    fdk_i32 list_bottom = btn_y - FD_GAP;
    if (fdlg_kind_save(d->kind) && d->name_entry != NULL) {
        fdk_size name_n = {0, 0};
        fdk_widget_measure(d->name_entry, &name_n);
        fdk_i32 name_h =
            name_n.height > 0 ? name_n.height : FD_ROW_H;
        fdk_i32 name_y = list_bottom - name_h;
        fdk_size lbl_n = {0, 0};
        fdk_widget_measure(d->name_label, &lbl_n);
        fdk_i32 lbl_w = lbl_n.width > 0 ? lbl_n.width : 44;
        fdk_widget_set_bounds(
            d->name_label,
            (fdk_rect){x, name_y, lbl_w, name_h});
        fdk_widget_set_bounds(
            d->name_entry,
            (fdk_rect){x + lbl_w + FD_GAP, name_y,
                       iw - lbl_w - FD_GAP, name_h});
        list_bottom = name_y - FD_GAP / 2;
    }

    /* Middle: places sidebar + file list. The sidebar rect is
     * remembered for body_paint's sidebar-surface fill. */
    fdk_i32 list_h = list_bottom - y;
    if (list_h < 40) {
        list_h = 40;
    }
    fdk_widget_set_bounds(
        d->places_list, (fdk_rect){x, y, FD_PLACES_W, list_h});
    fbody_of(w)->sidebar_rect =
        (fdk_rect){x, y, FD_PLACES_W, list_h};
    fdk_widget_set_bounds(
        d->list,
        (fdk_rect){x + FD_PLACES_W + FD_GAP, y,
                   iw - FD_PLACES_W - FD_GAP, list_h});

    fdk_widget_set_bounds(d->status,
                          (fdk_rect){x, status_y,
                                     iw - acc_n.width - can_n.width -
                                         FD_GAP * 2 - 120,
                                     FD_STATUS_H});
    fdk_widget_set_bounds(
        d->cancel_btn,
        (fdk_rect){a.x + a.width - FD_PAD - can_n.width, btn_y,
                   can_n.width, btn_h});
    fdk_widget_set_bounds(
        d->accept_btn,
        (fdk_rect){a.x + a.width - FD_PAD - can_n.width - FD_GAP -
                       acc_n.width,
                   btn_y, acc_n.width, btn_h});
}

static void fdlg_body_destroy(fdk_widget *w) {
    fdk_file_dialog *d = fdlg_of_body(w);
    if (d == NULL) {
        return;
    }
    fbody_of(w)->dialog = NULL;
    d->tearing_down = true;
    if (d->confirm_win != NULL) {
        /* A live overwrite ask holds a context pointing at `d`:
         * destroy it first — its callback sees tearing_down, frees
         * its context, touches nothing else. */
        fdk_window *cw = d->confirm_win;
        d->confirm_win = NULL;
        fdk_window_destroy(cw);
    }
    if (d->font != NULL) {
        fdk_font_destroy(d->font);
        d->font = NULL;
    }
    fdk__file_dialog_entries_free(&d->entries);
    fdk__fs_places_free(d->places, d->place_count);
    d->places = NULL;
    d->place_count = 0;
    fdk__file_dialog_free_filters(d->patterns, d->pattern_count);
    d->patterns = NULL;
    d->pattern_count = 0;
    if (d->pending.paths != NULL) {
        for (size_t i = 0; i < d->pending.count; i++) {
            fdk_free(d->pending.paths[i]);
        }
        fdk_free(d->pending.paths);
    }
    fdk_free(d->dir);
    fdk_free(d);
}

static const fdk_widget_class fdk_file_dialog_body_class = {
    .size = sizeof(fdk_file_dialog_body),
    .name = "file-dialog-body",
    .handle_event = NULL,
    .paint = fdlg_body_paint, /* window + sidebar surfaces (1.4.0) */
    .measure = NULL,
    .arrange = fdlg_body_arrange,
    .destroy = fdlg_body_destroy,
};

/* ---- public API ---- */

void fdk_file_dialog_result_free(fdk_file_dialog_result *result) {
    if (result == NULL) {
        return;
    }
    if (result->paths != NULL) {
        for (size_t i = 0; i < result->count; i++) {
            fdk_free(result->paths[i]);
        }
        fdk_free(result->paths);
    }
    result->paths = NULL;
    result->count = 0;
}

/* The shared constructor behind both entry points. */
static fdk_result fdk_dialog_show_impl(fdk_context *ctx,
                                       const fdk_file_dialog_options *options,
                                       fdk_file_dialog_done_fn on_done,
                                       void *user_data,
                                       fdk_window **out_window);

fdk_result fdk_dialog_open_file(fdk_context *ctx,
                                const fdk_file_dialog_options *options,
                                fdk_file_dialog_done_fn on_done,
                                void *user_data,
                                fdk_window **out_window) {
    return fdk_dialog_show_impl(ctx, options, on_done, user_data,
                                out_window);
}

fdk_result fdk_dialog_save_file(fdk_context *ctx,
                                const fdk_file_dialog_options *options,
                                fdk_file_dialog_done_fn on_done,
                                void *user_data,
                                fdk_window **out_window) {
    fdk_file_dialog_options forced = {0};
    if (options != NULL) {
        forced = *options;
    }
    forced.kind = FDK_FILE_DIALOG_SAVE_FILE;
    return fdk_dialog_show_impl(ctx, &forced, on_done, user_data,
                                out_window);
}

static fdk_result fdk_dialog_show_impl(fdk_context *ctx,
                                       const fdk_file_dialog_options *options,
                                       fdk_file_dialog_done_fn on_done,
                                       void *user_data,
                                       fdk_window **out_window) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_file_dialog_kind kind = (options != NULL)
                                    ? options->kind
                                    : FDK_FILE_DIALOG_OPEN_FILE;
    if (kind < FDK_FILE_DIALOG_OPEN_FILE ||
        kind > FDK_FILE_DIALOG_SAVE_FILE) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    const char *title = (options != NULL && options->title != NULL)
                            ? options->title
                            : fdlg_default_title(kind);
    bool modal = (options != NULL) && options->modal;

    fdk_file_dialog *d = fdk_alloc(sizeof(*d));
    if (d == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(d, 0, sizeof(*d));
    d->ctx = ctx;
    d->kind = kind;
    d->show_hidden = (options != NULL) && options->show_hidden;
    d->on_done = on_done;
    d->on_done_user = user_data;
    d->was_modal = modal;
    d->active_filter = 0; /* adjusted once patterns are parsed */

    /* Filters: parse + the "All files" fallback row. */
    if (options != NULL && options->filters != NULL) {
        d->pattern_count = fdk__file_dialog_parse_filters(
            options->filters, &d->patterns);
    }
    d->active_filter =
        (d->pattern_count > 0) ? 0 : d->pattern_count /* all */;

    /* start_dir -> $HOME -> / ; if even / fails to open, the honest
     * answer is the ERROR outcome (no fake browsing). */
    const char *fallbacks[3];
    size_t nf = 0;
    if (options != NULL && options->start_dir != NULL &&
        options->start_dir[0] != '\0') {
        fallbacks[nf++] = options->start_dir;
    }
    const char *home = getenv("HOME");
    if (home != NULL && home[0] != '\0') {
        fallbacks[nf++] = home;
    }
    fallbacks[nf++] = "/";
    int opened = -1;
    for (size_t i = 0; i < nf && opened != 0; i++) {
        fdk_fd_entries probe;
        opened = fdk__file_dialog_scan(fallbacks[i],
                                       fdlg_kind_folders(kind),
                                       d->show_hidden, NULL, 0, &probe);
        if (opened == 0) {
            d->dir = fdk__strdup(fallbacks[i]);
            fdk__file_dialog_entries_free(&probe);
        }
    }
    if (opened != 0 || d->dir == NULL) {
        fdk__file_dialog_free_filters(d->patterns, d->pattern_count);
        fdk_free(d);
        fdk_file_dialog_result r = {
            .outcome = FDK_FILE_DIALOG_ERROR,
            .paths = NULL,
            .count = 0,
        };
        if (on_done != NULL) {
            on_done(&r, user_data);
        }
        return FDK_ERR_PLATFORM;
    }

    /* Places: the sidebar's data (always at least Home or /). */
    if (fdk__fs_discover_places(&d->places, &d->place_count) != 0) {
        d->places = NULL; /* OOM: the sidebar shows empty, not dead */
        d->place_count = 0;
    }

    /* Window + body, per the message-dialog pattern. */
    fdk_i32 width = FD_PAD * 2 + FD_PLACES_W + FD_GAP + FD_LIST_W;
    fdk_i32 height = FD_PAD * 2 + FD_TOPBAR_H + FD_ROW_H +
                     FD_LIST_H + FD_STATUS_H + 64;
    if (fdlg_kind_save(kind)) {
        height += FD_ROW_H + FD_GAP;
    }
    fdk_window_options wopts = {
        .title = title,
        .width = width,
        .height = height,
    };
    fdk_window *win = NULL;
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        goto fail_before_window;
    }
    d->window = win;
    fdk__window_set_auto_paint(win, true);
    fdk__window_set_destroy_notify(win, fdlg_destroyed, d);
    fdk_window_set_event_callback(win, fdlg_window_event, d);

    fdk_widget *root = NULL;
    r = fdk_window_get_root(win, &root);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_widget *body = NULL;
    r = fdk_widget_create(root, &fdk_file_dialog_body_class,
                          (fdk_rect){0, 0, width, height}, &body);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fbody_of(body)->dialog = d;
    fbody_of(body)->sidebar_rect = (fdk_rect){0, 0, 0, 0};
    d->body = body;
    fdk_widget_set_accessible_name(body, title);
    /* No set_background snapshot: fdlg_body_paint fills both surfaces
     * through the live theme (1.4.0). */

    /* The dialog's font: the system default (dialogs are toolkit
     * chrome; there is no options.font to borrow). */
    d->font = fdk_font_load_system_default(14);
    if (d->font == NULL) {
        r = FDK_ERR_PLATFORM;
        goto fail;
    }

    r = fdk_button_create(body, d->font, "Up", &d->up_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(d->up_btn, fdlg_up_clicked, d);

    r = fdk_button_create(body, d->font, "Home", &d->home_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(d->home_btn, fdlg_home_clicked, d);

    r = fdk_toggle_create(body, d->font, "Hidden", &d->hidden_toggle);
    if (!fdk_ok(r)) {
        goto fail;
    }
    if (d->show_hidden) {
        fdk_toggle_set_checked(d->hidden_toggle, true);
    }
    fdk_toggle_set_on_changed(d->hidden_toggle, fdlg_hidden_toggled, d);

    /* Filter combo: one row per pattern + "All files"; the first
     * pattern starts active when filters were given. */
    r = fdk_combo_create(body, d->font, &d->filter_combo);
    if (!fdk_ok(r)) {
        goto fail;
    }
    for (size_t i = 0; i < d->pattern_count; i++) {
        (void)fdk_combo_append(d->filter_combo, d->patterns[i], NULL);
    }
    (void)fdk_combo_append(d->filter_combo, "All files", NULL);
    fdk_combo_set_active(d->filter_combo,
                         (fdk_i64)d->active_filter);
    fdk_combo_set_on_changed(d->filter_combo, fdlg_filter_changed, d);

    /* Path row (1.4.0): the breadcrumb bar at rest, the location
     * Entry behind Ctrl+L. Both are created; the entry starts
     * hidden (location_mode = false) — the swap flips visibility,
     * placement follows in fdlg_body_arrange. */
    r = fdk_widget_create(body, &fdk_path_bar_class,
                          (fdk_rect){0, 0, 0, 0}, &d->path_bar);
    if (!fdk_ok(r)) {
        goto fail;
    }
    pbar_of(d->path_bar)->dialog = d;
    pbar_of(d->path_bar)->font = d->font;

    r = fdk_entry_create(body, d->font, d->dir, &d->path_entry);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_entry_set_on_activate(d->path_entry, fdlg_path_activated, d);
    fdk_widget_set_visible(d->path_entry, false);

    /* Places sidebar. */
    r = fdk_list_create(body, d->font, &d->places_list);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_list_set_selection_mode(d->places_list,
                                FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_row_activate(d->places_list,
                                 fdlg_place_activated, d);
    for (size_t i = 0; i < d->place_count; i++) {
        (void)fdk_list_append(d->places_list, d->places[i].label,
                              NULL);
        /* 1.4.1: the symbolic place glyphs — Home gets the house,
         * the root and real mounts get the drive slab, everything
         * else (XDG user dirs) the folder. */
        (void)fdk_list_row_set_icon(
            d->places_list, i,
            fdlg_place_icon(d->places[i].path, d->places[i].label));
    }

    /* The file list. */
    r = fdk_list_create(body, d->font, &d->list);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_list_set_selection_mode(
        d->list, fdlg_kind_multi(kind)
                    ? FDK_LIST_SELECTION_MULTIPLE
                    : FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_row_activate(d->list, fdlg_row_activated, d);
    fdk_list_set_on_selection_changed(d->list,
                                      fdlg_selection_changed, d);

    /* The Name row (SAVE only): label + entry, seeded from
     * start_name and selected so typing replaces it (the
     * rename-everywhere convention). */
    if (fdlg_kind_save(kind)) {
        r = fdk_label_create(body, d->font, "Name:", &d->name_label);
        if (!fdk_ok(r)) {
            goto fail;
        }
        const char *start_name = (options != NULL)
                                     ? options->start_name
                                     : NULL;
        if (start_name == NULL) {
            start_name = "";
        }
        r = fdk_entry_create(body, d->font, start_name, &d->name_entry);
        if (!fdk_ok(r)) {
            goto fail;
        }
        fdk_entry_set_on_activate(d->name_entry, fdlg_accept_clicked,
                                  d);
        if (start_name[0] != '\0') {
            fdk_entry_select_all(d->name_entry);
        }
    }

    r = fdk_label_create(body, d->font, "", &d->status);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_label_set_mode(d->status, FDK_LABEL_ELLIPSIZE);

    r = fdk_button_create(body, d->font, fdlg_accept_label(kind),
                          &d->accept_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(d->accept_btn, fdlg_accept_clicked, d);
    /* The accept button carries the dialog: suggested-role (accent-
     * filled) — the modern dialog's one obviously-primary action. */
    fdk_button_set_role(d->accept_btn, FDK_BUTTON_ROLE_SUGGESTED);
    r = fdk_button_create(body, d->font, "Cancel", &d->cancel_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(d->cancel_btn, fdlg_cancel_clicked, d);

    fdk_window_set_content(win, body);
    fdlg_reload(d);

    d->surfaced = true; /* construction succeeded: answers are honest */
    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }
    /* Keyboard browsing from the first keypress: the list for the
     * OPEN kinds, the Name row for SAVE (type the name at once). */
    if (fdlg_kind_save(kind) && d->name_entry != NULL) {
        fdk_widget_focus(d->name_entry);
    } else {
        fdk_widget_focus(d->list);
    }

    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail_before_window:
    fdk__file_dialog_free_filters(d->patterns, d->pattern_count);
    fdk__fs_places_free(d->places, d->place_count);
    fdk_free(d->dir);
    fdk_free(d);
    return r;

fail:
    if (d->body == NULL) {
        /* The body destroy hook owns d once the body exists. */
        if (d->font != NULL) {
            fdk_font_destroy(d->font);
        }
        fdk__file_dialog_entries_free(&d->entries);
        fdk__fs_places_free(d->places, d->place_count);
        fdk__file_dialog_free_filters(d->patterns, d->pattern_count);
        if (d->pending.paths != NULL) {
            fdk_file_dialog_result_free(&d->pending);
        }
        fdk_free(d->dir);
        fdk_free(d);
    }
    fdk_window_destroy(win);
    return r;
}


