/* 10_file_manager.c — FDK Files: a real small file manager (1.2.1).
 *
 * The "can FDK build an actual application?" proof, rewritten after
 * the 1.2.0 live report ("too simple to be the default") into
 * something that behaves like a file manager people recognize:
 *
 *   PLACES        Home, Desktop, Documents, Downloads, Pictures,
 *                 Music, Videos, Temp, Filesystem — jumps verified
 *                 with stat() before navigating
 *   TOOLBAR       Back / Forward (real history stacks) / Up /
 *                 Refresh / New Folder / Rename / Delete / Hidden
 *   LOCATION BAR  an EDITABLE Entry — type any path, Enter goes
 *                 (~/ expands); Enter inside a folder row works too
 *   FILTER BOX    an Entry that re-filters the listing as you type
 *                 (case-insensitive substring on names)
 *   SORTING       a combo (Name / Size / Modified) plus an
 *                 Ascending/Descending toggle; directories always
 *                 group first, like every FM that respects users
 *   FILE LIST     Name + Size + Modified columns, MULTIPLE selection
 *                 (click, ctrl+click, shift+click, shift+arrows),
 *                 double-click / Enter enters a folder and tries
 *                 xdg-open on a file (honest status when absent)
 *   FILE OPS      New Folder and Rename ask through the toolkit's
 *                 PROMPT dialogs (a real text box, prefilled,
 *                 selected); Delete confirms with a YES/NO dialog,
 *                 then unlinks files and removes EMPTY directories —
 *                 recursive delete is deliberately absent (the
 *                 roadmap's honesty clause)
 *   STATUS BAR    items (+hidden count), selection count and total
 *                 size, filesystem free space (statvfs), sort state
 *
 * Keyboard: Up/Down/Home/End/PageUp/PageDown (+shift extends) and
 * Enter live in the list; Backspace = Up, F5 = Refresh, Ctrl+H =
 * Hidden, Alt+Left/Right = history, Escape quits. The main loop
 * paints when the tree has damage — the missing piece that made the
 * 1.2.0 build feel dead between interactions.
 *
 * For the GUI test rigs the demo prints:
 *   RIG: places|filelist|btn-up|btn-refresh|btn-hidden|btn-back|
 *        btn-forward|btn-newfolder|btn-rename|btn-delete|
 *        entry-path|entry-filter|combo-sort <x> <y> <w> <h>
 *   PHASE: dir <path> <n>            — every navigation
 *   PHASE: sel <n> [first-path]      — every selection change
 *   PHASE: open <path>               — xdg-open attempts
 *   PHASE: mkdir <path>              — New Folder results
 *   PHASE: rename <old> <new>        — Rename results
 *   PHASE: delete <requested> <done> — Delete results
 *   PHASE: quit
 *
 * Close the window, Escape, or FDK_DEMO_FRAMES=N to exit. Fonts from
 * fdk_font_load_system_default(). The window background is the
 * TOOLKIT default (the theme's window-background token — 1.2.1);
 * panels that need to read as panels set their own.
 *
 * INIT-tier helper user (see example_window.h): the demo owns its
 * manual-bounds three-pane layout. The helper still provides the
 * uniform app_id (org.fdk.example10).
 */

#define _DEFAULT_SOURCE

#include "example_window.h"
#include "fdk/fdk_dialog.h"

#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KEY_F5 ((fdk_scancode)63) /* evdev, like FDK_KEY_* in fdk_event.h */
#define SCAN_H ((fdk_scancode)35) /* evdev KEY_H, for Ctrl+H */

/* ---- app state ---- */

enum { SORT_NAME = 0, SORT_SIZE = 1, SORT_MTIME = 2 };

typedef struct {
    char path[4096][128];
    size_t count;
} history_t;

static struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_font *font;
    bool quit;
    int frames_left;
    char dir[4096];         /* current directory (absolute)          */
    char filter[128];       /* live name filter ("" = show all)      */
    /* listing store (parallel arrays, qsort-ordered) */
    char *names;            /* 256 bytes per row                    */
    bool *is_dir;
    off_t *sizes;
    time_t *mtimes;
    size_t count;           /* visible rows                          */
    size_t hidden_count;    /* entries filtered as hidden            */
    bool show_hidden;
    bool loading;            /* reload guard: selection callbacks read
                              * STALE parallel arrays mid-reload otherwise
                              * (found by the sway rig: clear() fires the
                              * selection callback per removed row while
                              * app.dir already points at the NEW dir) */
    int sort_mode;          /* SORT_*                                */
    bool sort_desc;
    history_t back;
    history_t fwd;
    /* widgets */
    fdk_widget *places;
    fdk_widget *btn_back, *btn_fwd, *btn_up, *btn_refresh;
    fdk_widget *btn_newfolder, *btn_rename, *btn_delete;
    fdk_widget *btn_hidden;
    fdk_widget *entry_path, *entry_filter;
    fdk_widget *combo_sort, *toggle_desc;
    fdk_widget *columns;
    fdk_widget *file_list;
    fdk_widget *status;
} app;

/* rename payload handed to the prompt callback (one dialog at a
 * time; static is honest about that) */
static char g_rename_old[4096 + 256];

/* ---- small helpers ---- */

static void setf(fdk_widget *label, const char *fmt, ...) {
    char buf[4600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)fdk_label_set_text(label, buf);
}

static void fmt_size(off_t bytes, char *buf, size_t cap) {
    double v = (double)bytes;
    if (bytes < 1024) {
        snprintf(buf, cap, "%d B", (int)bytes);
    } else if (bytes < 1024LL * 1024) {
        snprintf(buf, cap, "%.1f KB", v / 1024.0);
    } else if (bytes < 1024LL * 1024 * 1024) {
        snprintf(buf, cap, "%.1f MB", v / (1024.0 * 1024.0));
    } else if (bytes < 1024LL * 1024 * 1024 * 1024) {
        snprintf(buf, cap, "%.1f GB", v / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(buf, cap, "%.1f TB",
                 v / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
}

static void fmt_mtime(time_t t, char *buf, size_t cap) {
    struct tm tm_buf;
    if (localtime_r(&t, &tm_buf) == NULL) {
        snprintf(buf, cap, "?");
        return;
    }
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tm_buf);
}

/* Truncates `name` to `max` characters, appending the UTF-8 ellipsis
 * when something was cut (the row text is DISPLAY ONLY — the store
 * keeps the full name). */
static void trunc_name(const char *name, size_t max, char *out,
                       size_t cap) {
    size_t len = strlen(name);
    if (len <= max) {
        snprintf(out, cap, "%s", name);
        return;
    }
    if (max < 1) {
        snprintf(out, cap, "%s", "\xE2\x80\xA6");
        return;
    }
    snprintf(out, cap, "%.*s\xE2\x80\xA6", (int)(max - 1), name);
}

void report_selection(void);

/* ------------------------------------------------------------------ */
/* history                                                            */
/* ------------------------------------------------------------------ */

static void history_push(history_t *h, const char *path) {
    if (h->count == 128) { /* drop the OLDEST, keep moving */
        memmove(h->path[0], h->path[1], sizeof(h->path[0]) * 127);
        h->count--;
    }
    snprintf(h->path[h->count], sizeof(h->path[0]), "%s", path);
    h->count++;
}

static bool history_pop(history_t *h, char *out, size_t cap) {
    if (h->count == 0) {
        return false;
    }
    h->count--;
    snprintf(out, cap, "%s", h->path[h->count]);
    return true;
}

/* ------------------------------------------------------------------ */
/* listing                                                            */
/* ------------------------------------------------------------------ */

static int entry_cmp(const void *a, const void *b) {
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    if (app.is_dir[ia] != app.is_dir[ib]) {
        return app.is_dir[ia] ? -1 : 1; /* directories always first */
    }
    int cmp = 0;
    switch (app.sort_mode) {
    case SORT_SIZE:
        cmp = (app.sizes[ia] < app.sizes[ib]) ? -1
              : (app.sizes[ia] > app.sizes[ib]) ? 1 : 0;
        if (cmp == 0) {
            cmp = strcasecmp(app.names + ia * 256, app.names + ib * 256);
        }
        break;
    case SORT_MTIME:
        cmp = (app.mtimes[ia] < app.mtimes[ib]) ? -1
              : (app.mtimes[ia] > app.mtimes[ib]) ? 1 : 0;
        if (cmp == 0) {
            cmp = strcasecmp(app.names + ia * 256, app.names + ib * 256);
        }
        break;
    case SORT_NAME:
    default:
        cmp = strcasecmp(app.names + ia * 256, app.names + ib * 256);
        break;
    }
    return app.sort_desc ? -cmp : cmp;
}

static void list_free(void) {
    free(app.names);
    free(app.is_dir);
    free(app.sizes);
    free(app.mtimes);
    app.names = NULL;
    app.is_dir = NULL;
    app.sizes = NULL;
    app.mtimes = NULL;
    app.count = 0;
}

static int lower_ascii(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Case-insensitive ASCII substring match (the honest scope for a
 * demo filter; UTF-8 case folding is a non-goal here). */
static bool name_matches_filter(const char *name) {
    if (app.filter[0] == '\0') {
        return true;
    }
    size_t fl = strlen(app.filter);
    for (const char *scan = name; *scan != '\0'; scan++) {
        size_t i = 0;
        while (i < fl && scan[i] != '\0' &&
               lower_ascii((unsigned char)scan[i]) ==
                   lower_ascii((unsigned char)app.filter[i])) {
            i++;
        }
        if (i == fl) {
            return true;
        }
    }
    return false;
}

static void load_dir(void) {
    app.loading = true; /* selection callbacks: not now (see struct) */
    fdk_list_clear(app.file_list);
    list_free();
    app.hidden_count = 0;

    DIR *d = opendir(app.dir);
    if (d == NULL) {
        app.loading = false;
        setf(app.status, "Cannot open %s", app.dir);
        (void)fdk_entry_set_text(app.entry_path, app.dir);
        printf("PHASE: dir %s 0\n", app.dir);
        fflush(stdout);
        return;
    }

    size_t cap = 128;
    app.names = malloc(cap * 256);
    app.is_dir = malloc(cap * sizeof(bool));
    app.sizes = malloc(cap * sizeof(off_t));
    app.mtimes = malloc(cap * sizeof(time_t));
    size_t *order = malloc(cap * sizeof(size_t));
    if (app.names == NULL || app.is_dir == NULL || app.sizes == NULL ||
        app.mtimes == NULL || order == NULL) {
        closedir(d);
        free(order);
        list_free();
        app.loading = false;
        return;
    }

    struct dirent *de;
    size_t n = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (de->d_name[0] == '.') {
            if (!app.show_hidden) {
                app.hidden_count++;
                continue;
            }
        }
        if (strlen(de->d_name) >= 256) {
            continue; /* pathological name: skipped, not crashed */
        }
        if (!name_matches_filter(de->d_name)) {
            continue;
        }
        if (n == cap) {
            char *g_names = realloc(app.names, cap * 2 * 256);
            bool *g_dirs = realloc(app.is_dir, cap * 2 * sizeof(bool));
            off_t *g_sizes = realloc(app.sizes, cap * 2 * sizeof(off_t));
            time_t *g_times = realloc(app.mtimes,
                                      cap * 2 * sizeof(time_t));
            size_t *g_order = realloc(order, cap * 2 * sizeof(size_t));
            if (g_names == NULL || g_dirs == NULL || g_sizes == NULL ||
                g_times == NULL || g_order == NULL) {
                break;
            }
            app.names = g_names;
            app.is_dir = g_dirs;
            app.sizes = g_sizes;
            app.mtimes = g_times;
            order = g_order;
            cap *= 2;
        }
        char full[4096 + 256];
        snprintf(full, sizeof(full), "%s/%s", app.dir, de->d_name);
        struct stat st;
        bool have_stat = stat(full, &st) == 0;
        strcpy(app.names + n * 256, de->d_name);
        app.is_dir[n] = have_stat && S_ISDIR(st.st_mode);
        app.sizes[n] = have_stat ? st.st_size : 0;
        app.mtimes[n] = have_stat ? st.st_mtime : 0;
        order[n] = n;
        n++;
    }
    closedir(d);

    qsort(order, n, sizeof(size_t), entry_cmp);

    /* Permute the STORE into sorted order, so the list's display row
     * i IS store index i — every row -> name/size/dir lookup (the
     * selection report, activation, rename, delete) then indexes the
     * store directly. The rig caught the 1.2.1 draft reporting the
     * READDIR-order name for a clicked row (readdir order is hash
     * order, nothing like the sorted listing). */
    {
        char *sorted_names = malloc(cap * 256);
        bool *sorted_dirs = malloc(cap * sizeof(bool));
        off_t *sorted_sizes = malloc(cap * sizeof(off_t));
        time_t *sorted_times = malloc(cap * sizeof(time_t));
        if (sorted_names != NULL && sorted_dirs != NULL &&
            sorted_sizes != NULL && sorted_times != NULL) {
            for (size_t i = 0; i < n; i++) {
                size_t idx = order[i];
                memcpy(sorted_names + i * 256, app.names + idx * 256,
                       256);
                sorted_dirs[i] = app.is_dir[idx];
                sorted_sizes[i] = app.sizes[idx];
                sorted_times[i] = app.mtimes[idx];
            }
            free(app.names);
            free(app.is_dir);
            free(app.sizes);
            free(app.mtimes);
            app.names = sorted_names;
            app.is_dir = sorted_dirs;
            app.sizes = sorted_sizes;
            app.mtimes = sorted_times;
        } else {
            /* OOM on the permutation: keep readdir order (the rows
             * still list sorted; only the lookups stay scrambled —
             * degraded, not crashed). */
            free(sorted_names);
            free(sorted_dirs);
            free(sorted_sizes);
            free(sorted_times);
        }
    }

    for (size_t i = 0; i < n; i++) {
        const char *name = app.names + i * 256;
        char shown[64];
        char row[128];
        if (app.is_dir[i]) {
            trunc_name(name, 30, shown, sizeof(shown));
            size_t sl = strlen(shown);
            if (sl + 1 < sizeof(shown)) {
                shown[sl] = '/';
                shown[sl + 1] = '\0';
            }
            snprintf(row, sizeof(row), "%-32s %10s  (folder)", shown,
                     "");
        } else {
            char sz[16];
            fmt_size(app.sizes[i], sz, sizeof(sz));
            char dt[24];
            fmt_mtime(app.mtimes[i], dt, sizeof(dt));
            trunc_name(name, 30, shown, sizeof(shown));
            snprintf(row, sizeof(row), "%-32s %10s  %s", shown, sz, dt);
        }
        (void)fdk_list_append(app.file_list, row, NULL);
    }
    free(order);
    app.count = n;
    app.loading = false;

    (void)fdk_entry_set_text(app.entry_path, app.dir);
    report_selection();
    printf("PHASE: dir %s %zu\n", app.dir, n);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* navigation                                                         */
/* ------------------------------------------------------------------ */

static void sync_status_selection(void);

static void chdir_abs(const char *dir, bool remember) {
    if (remember && strcmp(dir, app.dir) != 0) {
        history_push(&app.back, app.dir);
        app.fwd.count = 0; /* a new branch invalidates forward */
    }
    snprintf(app.dir, sizeof(app.dir), "%s", dir);
    load_dir();
}

static void chdir_rel(const char *sub) {
    char next[4096];
    if (strcmp(sub, "..") == 0) {
        char *slash = strrchr(app.dir, '/');
        if (slash != NULL && slash != app.dir) {
            *slash = '\0';
        } else {
            strcpy(app.dir, "/");
        }
        snprintf(next, sizeof(next), "%s", app.dir);
        if (slash != NULL && slash != app.dir) {
            *slash = '/';
        }
    } else {
        size_t dl = strlen(app.dir), sl = strlen(sub);
        size_t room = sizeof(next) - 1;
        size_t take = dl > room ? room : dl;
        memcpy(next, app.dir, take);
        size_t used = take;
        if (used < room && app.dir[dl - 1] != '/') {
            next[used++] = '/';
        }
        size_t take_sub = sl > room - used ? room - used : sl;
        memcpy(next + used, sub, take_sub);
        used += take_sub;
        next[used] = '\0';
    }
    chdir_abs(next, true);
}

static void go_back(void) {
    char prev[4096];
    if (!history_pop(&app.back, prev, sizeof(prev))) {
        return;
    }
    history_push(&app.fwd, app.dir);
    snprintf(app.dir, sizeof(app.dir), "%s", prev);
    load_dir();
}

static void go_forward(void) {
    char next[4096];
    if (!history_pop(&app.fwd, next, sizeof(next))) {
        return;
    }
    history_push(&app.back, app.dir);
    snprintf(app.dir, sizeof(app.dir), "%s", next);
    load_dir();
}

/* Expands a leading ~ to $HOME, then navigates if the target is a
 * readable directory (stat-verified; the honest error otherwise). */
static void path_entry_activated(fdk_widget *entry, void *user) {
    (void)entry; (void)user;
    const char *typed = fdk_entry_get_text(app.entry_path);
    char expanded[4096];
    if (typed != NULL && typed[0] == '~') {
        const char *home = getenv("HOME");
        snprintf(expanded, sizeof(expanded), "%s%s",
                 home != NULL ? home : "", typed + 1);
    } else {
        snprintf(expanded, sizeof(expanded), "%s",
                 typed != NULL ? typed : "");
    }
    if (expanded[0] == '\0') {
        (void)fdk_entry_set_text(app.entry_path, app.dir);
        return;
    }
    struct stat st;
    if (stat(expanded, &st) == 0 && S_ISDIR(st.st_mode)) {
        chdir_abs(expanded, true);
    } else {
        setf(app.status, "Not a directory: %s", expanded);
        (void)fdk_entry_set_text(app.entry_path, app.dir);
    }
}

/* ------------------------------------------------------------------ */
/* selection reporting (the example is also a functional test)        */
/* ------------------------------------------------------------------ */

void report_selection(void) {
    size_t n = fdk_list_selected_count(app.file_list);
    if (n == 0) {
        sync_status_selection();
        printf("PHASE: sel 0\n");
    } else if (n == 1) {
        size_t row = 0;
        (void)fdk_list_selected_at(app.file_list, 0, &row);
        char full[4096 + 300];
        snprintf(full, sizeof(full), "%s/%s", app.dir,
                 app.names + row * 256);
        setf(app.status, "Selected %s: %s",
             app.is_dir[row] ? "folder" : "file", full);
        printf("PHASE: sel 1 %s\n", full);
    } else {
        off_t total = 0;
        for (size_t i = 0; i < n; i++) {
            size_t row = 0;
            if (fdk_ok(fdk_list_selected_at(app.file_list, i, &row)) &&
                row < app.count) {
                total += app.sizes[row];
            }
        }
        char sz[16];
        fmt_size(total, sz, sizeof(sz));
        setf(app.status, "%zu items — %zu selected (%s total)",
             app.count, n, sz);
        printf("PHASE: sel %zu\n", n);
    }
    fflush(stdout);
}

/* The no-selection variant of the status line: items, hidden count,
 * free space, sort state. */
static void sync_status_selection(void) {
    char free_buf[16] = "";
    struct statvfs vfs;
    if (statvfs(app.dir, &vfs) == 0) {
        fmt_size((off_t)vfs.f_bavail * (off_t)vfs.f_frsize, free_buf,
                 sizeof(free_buf));
    }
    const char *sort_name = app.sort_mode == SORT_SIZE ? "size"
                            : app.sort_mode == SORT_MTIME ? "modified"
                                                          : "name";
    setf(app.status, "%zu items%s — 0 selected — %s free — sorted by %s %s",
         app.count, app.hidden_count > 0 ? " (+hidden)" : "", free_buf,
         sort_name, app.sort_desc ? "\xE2\x86\x93" : "\xE2\x86\x91");
}

/* ------------------------------------------------------------------ */
/* external open                                                      */
/* ------------------------------------------------------------------ */

static void open_external(const char *path) {
    printf("PHASE: open %s\n", path);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        setf(app.status, "Could not launch a handler (fork failed)");
        return;
    }
    if (pid == 0) {
        /* child: quiet, detached handler attempt */
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execlp("xdg-open", "xdg-open", path, (char *)NULL);
        _exit(127); /* no xdg-open: the parent reports it honestly */
    }
    setf(app.status, "Opening %s (xdg-open)...", path);
}

/* ------------------------------------------------------------------ */
/* widget callbacks                                                   */
/* ------------------------------------------------------------------ */

static void on_selection(fdk_widget *list, void *user) {
    (void)list; (void)user;
    if (app.loading) {
        return; /* mid-reload: the parallel arrays are stale */
    }
    report_selection();
}

static void on_row_activated(fdk_widget *list, size_t row, void *user) {
    (void)list; (void)user;
    if (row >= app.count) {
        return;
    }
    if (app.is_dir[row]) {
        chdir_rel(app.names + row * 256);
        return;
    }
    char full[4096 + 256];
    snprintf(full, sizeof(full), "%s/%s", app.dir, app.names + row * 256);
    open_external(full);
}

static void back_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    go_back();
}
static void fwd_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    go_forward();
}
static void up_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    chdir_rel("..");
}
static void refresh_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    load_dir();
}

static void hidden_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w; (void)user;
    app.show_hidden = checked;
    load_dir();
}

static void filter_changed(fdk_widget *entry, void *user) {
    (void)entry; (void)user;
    const char *text = fdk_entry_get_text(app.entry_filter);
    snprintf(app.filter, sizeof(app.filter), "%s",
             text != NULL ? text : "");
    load_dir();
}

static void sort_changed(fdk_widget *combo, size_t index, void *user) {
    (void)combo; (void)user;
    app.sort_mode = (index <= SORT_MTIME) ? (int)index : SORT_NAME;
    load_dir();
}

static void desc_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w; (void)user;
    app.sort_desc = checked;
    load_dir();
}

/* ---- places ---- */

static void on_place(fdk_widget *list, void *user) {
    (void)user;
    fdk_i64 sel = fdk_list_get_selected(list);
    const char *home = getenv("HOME");
    static const struct {
        const char *label;
        const char *suffix; /* NULL = filesystem root          */
    } places[] = {
        {"Home", ""},        {"Desktop", "/Desktop"},
        {"Documents", "/Documents"}, {"Downloads", "/Downloads"},
        {"Pictures", "/Pictures"},  {"Music", "/Music"},
        {"Videos", "/Videos"},      {"Temp", NULL},
        {"Filesystem", "/"},
    };
    if (sel < 0 || (size_t)sel >= sizeof(places) / sizeof(places[0])) {
        return;
    }
    char path[4096];
    if (places[sel].suffix == NULL) {
        snprintf(path, sizeof(path), "%s", "/tmp");
    } else if (strcmp(places[sel].suffix, "/") == 0) {
        snprintf(path, sizeof(path), "%s", "/");
    } else if (places[sel].suffix[0] == '\0') {
        snprintf(path, sizeof(path), "%s",
                 (home != NULL && home[0]) ? home : "/");
    } else {
        snprintf(path, sizeof(path), "%s%s",
                 (home != NULL && home[0]) ? home : "", places[sel].suffix);
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        setf(app.status, "Place not present on this machine: %s",
             places[sel].label);
        return;
    }
    chdir_abs(path, true);
}

/* ---- file operations (through the toolkit's dialogs) ---- */

static void newfolder_done(fdk_dialog_response response,
                           const char *text, void *user);
static void rename_done(fdk_dialog_response response, const char *text,
                        void *user);

static void newfolder_prompt(fdk_widget *w, void *user) {
    (void)w; (void)user;
    fdk_prompt_dialog_options opts = {0};
    opts.title = "New Folder";
    opts.text = "Create a folder in the current directory:";
    opts.value = "New Folder";
    opts.parent = app.window;
    fdk_result r = fdk_dialog_show_prompt(app.ctx, &opts, newfolder_done,
                                          NULL, NULL);
    if (!fdk_ok(r)) {
        setf(app.status, "Could not open the prompt (%s)",
             fdk_result_to_string(r));
    }
}

static void newfolder_done(fdk_dialog_response response,
                           const char *text, void *user) {
    (void)user;
    if (response != FDK_DIALOG_OK || text == NULL || text[0] == '\0' ||
        strchr(text, '/') != NULL) {
        return;
    }
    /* Find a free name: "New Folder", "New Folder 2", ... 99 */
    char full[4096 + 300];
    char name[256];
    snprintf(name, sizeof(name), "%s", text);
    struct stat st;
    snprintf(full, sizeof(full), "%s/%s", app.dir, name);
    if (stat(full, &st) == 0) {
        bool placed = false;
        for (int i = 2; i < 100; i++) {
            snprintf(name, sizeof(name), "%s %d", text, i);
            snprintf(full, sizeof(full), "%s/%s", app.dir, name);
            if (stat(full, &st) != 0) {
                placed = true;
                break;
            }
        }
        if (!placed) {
            setf(app.status, "Could not find a free variant of \"%s\"",
                 text);
            return;
        }
    }
    if (mkdir(full, 0755) == 0) {
        setf(app.status, "Created %s", full);
        printf("PHASE: mkdir %s\n", full);
        fflush(stdout);
        load_dir();
    } else {
        setf(app.status, "Could not create \"%s\" in %s", name, app.dir);
    }
}

static void rename_prompt(fdk_widget *w, void *user) {
    (void)w; (void)user;
    size_t n = fdk_list_selected_count(app.file_list);
    if (n != 1) {
        setf(app.status, "Rename needs exactly one selection (have %zu)",
             n);
        return;
    }
    size_t row = 0;
    (void)fdk_list_selected_at(app.file_list, 0, &row);
    if (row >= app.count) {
        return;
    }
    snprintf(g_rename_old, sizeof(g_rename_old), "%s/%s", app.dir,
             app.names + row * 256);

    fdk_prompt_dialog_options opts = {0};
    opts.title = "Rename";
    opts.text = "New name (in the same directory):";
    opts.value = app.names + row * 256;
    opts.parent = app.window;
    fdk_result r = fdk_dialog_show_prompt(app.ctx, &opts, rename_done,
                                          NULL, NULL);
    if (!fdk_ok(r)) {
        setf(app.status, "Could not open the prompt (%s)",
             fdk_result_to_string(r));
    }
}

static void rename_done(fdk_dialog_response response, const char *text,
                        void *user) {
    (void)user;
    if (response != FDK_DIALOG_OK || text == NULL) {
        return;
    }
    if (text[0] == '\0' || strchr(text, '/') != NULL) {
        setf(app.status, "Invalid name: \"%s\"", text);
        return;
    }
    char new_full[4096 + 300];
    snprintf(new_full, sizeof(new_full), "%s/%s", app.dir, text);
    struct stat st;
    if (stat(new_full, &st) == 0) {
        setf(app.status, "Already exists: %s", new_full);
        return;
    }
    if (rename(g_rename_old, new_full) == 0) {
        setf(app.status, "Renamed to %s", new_full);
        printf("PHASE: rename %s %s\n", g_rename_old, new_full);
        fflush(stdout);
        load_dir();
    } else {
        setf(app.status, "Could not rename to \"%s\"", text);
    }
}

static void delete_confirm(fdk_dialog_response response, void *user);
static size_t delete_selection_paths(char paths[][4096 + 300],
                                     size_t cap);

static void delete_clicked(fdk_widget *w, void *user) {
    (void)w; (void)user;
    size_t n = fdk_list_selected_count(app.file_list);
    if (n == 0) {
        setf(app.status, "Nothing selected to delete");
        return;
    }
    char text[512];
    if (n == 1) {
        size_t row = 0;
        (void)fdk_list_selected_at(app.file_list, 0, &row);
        snprintf(text, sizeof(text), "Delete \"%s\"?",
                 row < app.count ? app.names + row * 256 : "?");
    } else {
        snprintf(text, sizeof(text), "Delete %zu items?", n);
    }
    fdk_dialog_options opts = {0};
    opts.title = "Delete";
    opts.text = text;
    opts.buttons = FDK_DIALOG_BUTTONS_YES_NO;
    opts.parent = app.window;
    (void)fdk_dialog_show_message(app.ctx, &opts, delete_confirm, NULL,
                                  NULL);
}

static void delete_confirm(fdk_dialog_response response, void *user) {
    (void)user;
    if (response != FDK_DIALOG_YES) {
        return;
    }
    static char paths[512][4096 + 300];
    size_t n = delete_selection_paths(paths, 512);
    size_t done = 0;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) {
            continue;
        }
        bool ok = S_ISDIR(st.st_mode) ? rmdir(paths[i]) == 0
                                      : unlink(paths[i]) == 0;
        if (ok) {
            done++;
        }
    }
    printf("PHASE: delete %zu %zu\n", n, done);
    fflush(stdout);
    if (done == n) {
        setf(app.status, "Deleted %zu item%s", done,
             done == 1 ? "" : "s");
    } else {
        setf(app.status,
             "Deleted %zu of %zu — non-empty folders are kept", done,
             n);
    }
    load_dir();
}

static size_t delete_selection_paths(char paths[][4096 + 300],
                                     size_t cap) {
    size_t n = fdk_list_selected_count(app.file_list);
    if (n > cap) {
        n = cap;
    }
    for (size_t i = 0; i < n; i++) {
        size_t row = 0;
        if (fdk_ok(fdk_list_selected_at(app.file_list, i, &row)) &&
            row < app.count) {
            snprintf(paths[i], sizeof(paths[i]), "%s/%s", app.dir,
                     app.names + row * 256);
        } else {
            paths[i][0] = '\0';
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* window events                                                      */
/* ------------------------------------------------------------------ */

static void relayout(void);

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user) {
    (void)window; (void)user;
    switch (event->type) {
    case FDK_EVENT_WINDOW_CLOSE_REQUEST:
        app.quit = true;
        return;
    case FDK_EVENT_WINDOW_CONFIGURE:
        relayout();
        return;
    case FDK_EVENT_KEY_DOWN:
        break;
    default:
        return;
    }
    /* Keyboard shortcuts that survive widget focus (an Entry eats
     * its own keys before these can ever arrive). */
    if (event->key.scancode == FDK_KEY_ESC) {
        app.quit = true;
    } else if (event->key.scancode == KEY_F5) {
        load_dir();
    } else if (event->key.scancode == FDK_KEY_BACKSPACE) {
        chdir_rel("..");
    } else if (event->key.scancode == FDK_KEY_LEFT &&
               (event->key.modifiers & FDK_MOD_ALT) != 0) {
        go_back();
    } else if (event->key.scancode == FDK_KEY_RIGHT &&
               (event->key.modifiers & FDK_MOD_ALT) != 0) {
        go_forward();
    } else if (event->key.scancode == SCAN_H &&
               (event->key.modifiers & FDK_MOD_CTRL) != 0) {
        /* Flip through the toggle so its own on_change does the
         * reload (one path, one truth). */
        fdk_toggle_set_checked(app.btn_hidden, !app.show_hidden);
    }
}

/* ------------------------------------------------------------------ */
/* layout                                                             */
/* ------------------------------------------------------------------ */

static void place_row(fdk_widget *w, fdk_i32 x, fdk_i32 y, fdk_i32 h) {
    fdk_size n = {0, 0};
    fdk_widget_measure(w, &n);
    fdk_widget_set_bounds(w, (fdk_rect){x, y, n.width, h});
}

static void relayout(void) {
    fdk_size ws;
    (void)fdk_window_get_size(app.window, &ws);
    fdk_i32 pad = 8;
    fdk_i32 x = pad, y = pad;
    fdk_i32 iw = ws.width - pad * 2;
    if (iw < 200) {
        iw = 200;
    }

    /* Toolbar row 1: navigation + operations + hidden. */
    place_row(app.btn_back, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_back).width + 4;
    place_row(app.btn_fwd, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_fwd).width + 4;
    place_row(app.btn_up, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_up).width + 4;
    place_row(app.btn_refresh, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_refresh).width + 10;
    place_row(app.btn_newfolder, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_newfolder).width + 4;
    place_row(app.btn_rename, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_rename).width + 4;
    place_row(app.btn_delete, x, y, 28);
    x += fdk_widget_get_bounds(app.btn_delete).width + 10;
    place_row(app.btn_hidden, x, y, 28);
    y += 34;

    /* Toolbar row 2: location bar + filter + sort. */
    fdk_i32 sort_w = 128;
    fdk_i32 desc_w = 96;
    fdk_i32 filter_w = 170;
    fdk_i32 path_w = iw - (sort_w + desc_w + filter_w + 3 * 6);
    if (path_w < 160) {
        path_w = 160;
    }
    fdk_widget_set_bounds(app.entry_path,
                          (fdk_rect){pad, y, path_w, 30});
    fdk_i32 rx = pad + path_w + 6;
    fdk_widget_set_bounds(app.entry_filter,
                          (fdk_rect){rx, y, filter_w, 30});
    rx += filter_w + 6;
    fdk_widget_set_bounds(app.combo_sort, (fdk_rect){rx, y, sort_w, 30});
    rx += sort_w + 6;
    fdk_widget_set_bounds(app.toggle_desc, (fdk_rect){rx, y, desc_w, 30});
    y += 36;

    /* Column header above the file list. */
    fdk_i32 places_w = 160;
    fdk_i32 list_x = pad + places_w + pad;
    fdk_i32 list_w = iw - places_w - pad;
    if (list_w < 200) {
        list_w = 200;
    }
    fdk_widget_set_bounds(app.columns,
                          (fdk_rect){list_x, y, list_w, 20});
    y += 22;

    /* Body: places + file list. */
    fdk_i32 status_h = 22;
    fdk_i32 body_h = ws.height - pad - status_h - 6 - y;
    if (body_h < 120) {
        body_h = 120;
    }
    fdk_widget_set_bounds(app.places, (fdk_rect){pad, y, places_w, body_h});
    fdk_widget_set_bounds(app.file_list,
                          (fdk_rect){list_x, y, list_w, body_h});

    /* Status bar. */
    fdk_widget_set_bounds(app.status,
                          (fdk_rect){pad, ws.height - pad - status_h,
                                     iw, status_h});
}

static void rig_announce(const char *name, fdk_widget *w) {
    fdk_rect r = fdk_widget_get_bounds(w);
    printf("RIG: %s %d %d %d %d\n", name, r.x, r.y, r.width, r.height);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */

int main(void) {
    memset(&app, 0, sizeof(app));
    /* xdg-open children must not become zombies (no wait loop). */
    signal(SIGCHLD, SIG_IGN);

    if (!fdk_example_init(&app.ctx, "10")) {
        return 1;
    }
    app.font = fdk_font_load_system_default(14);
    if (app.font == NULL) {
        fprintf(stderr, "10_file_manager: no system font\n");
        fdk_shutdown(app.ctx);
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK Files",
        .width = 880,
        .height = 540,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fdk_font_destroy(app.font);
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);

    /* Toolbar row 1. */
    (void)fdk_button_create(root, app.font, "Back", &app.btn_back);
    fdk_button_set_on_activate(app.btn_back, back_clicked, NULL);
    (void)fdk_button_create(root, app.font, "Forward", &app.btn_fwd);
    fdk_button_set_on_activate(app.btn_fwd, fwd_clicked, NULL);
    (void)fdk_button_create(root, app.font, "Up", &app.btn_up);
    fdk_button_set_on_activate(app.btn_up, up_clicked, NULL);
    (void)fdk_button_create(root, app.font, "Refresh",
                            &app.btn_refresh);
    fdk_button_set_on_activate(app.btn_refresh, refresh_clicked, NULL);
    (void)fdk_button_create(root, app.font, "New Folder",
                            &app.btn_newfolder);
    fdk_button_set_on_activate(app.btn_newfolder, newfolder_prompt, NULL);
    (void)fdk_button_create(root, app.font, "Rename", &app.btn_rename);
    fdk_button_set_on_activate(app.btn_rename, rename_prompt, NULL);
    (void)fdk_button_create(root, app.font, "Delete", &app.btn_delete);
    fdk_button_set_on_activate(app.btn_delete, delete_clicked, NULL);
    (void)fdk_toggle_create(root, app.font, "Hidden", &app.btn_hidden);
    fdk_toggle_set_on_change(app.btn_hidden, hidden_toggled, NULL);

    /* Toolbar row 2. */
    (void)fdk_entry_create(root, app.font, NULL, &app.entry_path);
    fdk_entry_set_on_activate(app.entry_path, path_entry_activated,
                              NULL);
    (void)fdk_entry_create(root, app.font, NULL, &app.entry_filter);
    fdk_entry_set_on_changed(app.entry_filter, filter_changed, NULL);
    (void)fdk_combo_create(root, app.font, &app.combo_sort);
    (void)fdk_combo_append(app.combo_sort, "Name", NULL);
    (void)fdk_combo_append(app.combo_sort, "Size", NULL);
    (void)fdk_combo_append(app.combo_sort, "Modified", NULL);
    (void)fdk_combo_set_active(app.combo_sort, 0);
    fdk_combo_set_on_changed(app.combo_sort, sort_changed, NULL);
    (void)fdk_toggle_create(root, app.font, "Descending",
                            &app.toggle_desc);
    fdk_toggle_set_on_change(app.toggle_desc, desc_toggled, NULL);

    /* Columns hint + body. */
    (void)fdk_label_create(root, app.font,
                           "Name                     Size  Modified",
                           &app.columns);
    (void)fdk_list_create(root, app.font, &app.places);
    fdk_list_set_selection_mode(app.places, FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_selection_changed(app.places, on_place, NULL);
    (void)fdk_list_append(app.places, "Home", NULL);
    (void)fdk_list_append(app.places, "Desktop", NULL);
    (void)fdk_list_append(app.places, "Documents", NULL);
    (void)fdk_list_append(app.places, "Downloads", NULL);
    (void)fdk_list_append(app.places, "Pictures", NULL);
    (void)fdk_list_append(app.places, "Music", NULL);
    (void)fdk_list_append(app.places, "Videos", NULL);
    (void)fdk_list_append(app.places, "Temp", NULL);
    (void)fdk_list_append(app.places, "Filesystem", NULL);

    (void)fdk_list_create(root, app.font, &app.file_list);
    fdk_list_set_selection_mode(app.file_list,
                                FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_selection_changed(app.file_list, on_selection, NULL);
    fdk_list_set_on_row_activate(app.file_list, on_row_activated, NULL);

    (void)fdk_label_create(root, app.font, "", &app.status);
    fdk_label_set_mode(app.status, FDK_LABEL_ELLIPSIZE);

    const char *home = getenv("HOME");
    snprintf(app.dir, sizeof(app.dir), "%s",
             (home != NULL && home[0]) ? home : "/");
    relayout();
    load_dir();
    fdk_window_show(app.window);
    fdk_widget_focus(app.file_list); /* keyboard works immediately */

    /* Announce geometry for the rigs (post-layout). */
    rig_announce("places", app.places);
    rig_announce("filelist", app.file_list);
    rig_announce("btn-up", app.btn_up);
    rig_announce("btn-refresh", app.btn_refresh);
    rig_announce("btn-hidden", app.btn_hidden);
    rig_announce("btn-back", app.btn_back);
    rig_announce("btn-forward", app.btn_fwd);
    rig_announce("btn-newfolder", app.btn_newfolder);
    rig_announce("btn-rename", app.btn_rename);
    rig_announce("btn-delete", app.btn_delete);
    rig_announce("entry-path", app.entry_path);
    rig_announce("entry-filter", app.entry_filter);
    rig_announce("combo-sort", app.combo_sort);

    const char *frames = getenv("FDK_DEMO_FRAMES");
    app.frames_left = frames != NULL ? atoi(frames) : -1;

    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
        /* The 1.2.0 build never painted here: after the first frame
         * the window only updated on resizes, and the damage that
         * HAD accumulated repainted as new text stamped straight
         * over the old frame (the ghosting report). Paint whenever
         * the tree says something changed — the documented app
         * pattern (see 04_widgets.c). */
        if (fdk_widget_tree_has_damage(app.file_list)) {
            (void)fdk_window_paint(app.window);
        }
        if (app.frames_left > 0 && --app.frames_left == 0) {
            break;
        }
    }

    printf("PHASE: quit\n");
    fflush(stdout);
    list_free();
    fdk_font_destroy(app.font);
    fdk_shutdown(app.ctx);
    return 0;
}
