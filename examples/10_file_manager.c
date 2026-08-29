/* 10_file_manager.c — a small but real file manager (1.2.0).
 *
 * Not a toy: this is the "can FDK build an actual application?"
 * proof. It browses the REAL filesystem (opendir/readdir through the
 * same scan the file dialog uses internally — reimplemented here on
 * purpose, because an APPLICATION should own its listing logic) and
 * wires the whole stock catalog together:
 *
 *   PLACES       a List of the classic roots (Home / Desktop /
 *                Documents / Downloads / Filesystem) — click to jump
 *   TOOLBAR      Up / Refresh / Hidden toggle
 *   PATH BAR     the current directory, ellipsized from the left by
 *                the window's width
 *   FILE LIST    directories first (with a trailing /), MULTIPLE
 *                selection: click, ctrl+click, shift+click, drag,
 *                shift+arrows — double-click or Enter enters a
 *                directory (the List's row-activation callback)
 *   STATUS BAR   "N items" + the selection report: one name, "Selected
 *                4 items", or the folder's full path — the example is
 *                also a functional test, so selection is ALWAYS
 *                visibly reported
 *
 * Keyboard: the List owns Up/Down/Home/End/PageUp/PageDown (and
 * shift-extends), Enter activates; Escape quits.
 *
 * For the GUI test rigs the demo prints:
 *   RIG: places <x> <y> <w> <h>          — the places list
 *   RIG: filelist <x> <y> <w> <h>        — the file list
 *   RIG: btn-up|btn-refresh|btn-hidden <x> <y> <w> <h>
 *   PHASE: dir <path> <n>                — every navigation
 *   PHASE: sel <n> [first-path]          — every selection change
 *   PHASE: quit
 *
 * Close the window, Escape, or FDK_DEMO_FRAMES=N to exit. Fonts from
 * fdk_font_load_system_default(). This is deliberately NOT Nautilus:
 * no previews, no search, no file operations — the roadmap's honesty
 * clause (docs/roadmap.md 1.2.0).
 */

#define _DEFAULT_SOURCE

#include "fdk/fdk.h"

#include <stdarg.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ---- app state ---- */

static struct {
    fdk_context *ctx;
    fdk_window *window;
    fdk_font *font;
    bool quit;
    int frames_left;
    char dir[4096];         /* current directory (absolute)          */
    char *names;            /* flat store of row basenames (256 cap) */
    size_t name_cap;
    bool *is_dir;           /* per row                               */
    size_t count;
    bool show_hidden;
    /* widgets */
    fdk_widget *places;
    fdk_widget *btn_up;
    fdk_widget *btn_refresh;
    fdk_widget *btn_hidden;
    fdk_widget *path_label;
    fdk_widget *file_list;
    fdk_widget *status;
} app;

/* printf-shaped label set (fdk_label_set_text takes plain text). */
static void setf(fdk_widget *label, const char *fmt, ...) {
    char buf[4600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)fdk_label_set_text(label, buf);
}

void report_selection(void);

/* ------------------------------------------------------------------ */
/* listing (the app's own scan — see the header note)                  */
/* ------------------------------------------------------------------ */

static int entry_cmp(const void *a, const void *b) {
    /* qsort over indexes into parallel arrays; compare dir-ness then
     * name through app-owned storage (the qsort comparator receives
     * pointers INTO a local index array). */
    size_t ia = *(const size_t *)a, ib = *(const size_t *)b;
    if (app.is_dir[ia] != app.is_dir[ib]) {
        return app.is_dir[ia] ? -1 : 1;
    }
    return strcmp(app.names + ia * 256, app.names + ib * 256);
}

static void list_free(void) {
    free(app.names);
    free(app.is_dir);
    app.names = NULL;
    app.is_dir = NULL;
    app.name_cap = 0;
    app.count = 0;
}

static void load_dir(void) {
    fdk_list_clear(app.file_list);
    list_free();

    DIR *d = opendir(app.dir);
    if (d == NULL) {
        setf(app.status, "Cannot open %s", app.dir);
        (void)fdk_label_set_text(app.path_label, app.dir);
        return;
    }

    size_t cap = 128;
    app.names = malloc(cap * 256);
    app.is_dir = malloc(cap * sizeof(bool));
    size_t *order = malloc(cap * sizeof(size_t));
    if (app.names == NULL || app.is_dir == NULL || order == NULL) {
        closedir(d);
        free(order);
        list_free();
        return;
    }

    struct dirent *de;
    size_t n = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) {
            continue;
        }
        if (!app.show_hidden && de->d_name[0] == '.') {
            continue;
        }
        if (strlen(de->d_name) >= 256) {
            continue; /* pathological name: skipped, not crashed */
        }
        if (n == cap) {
            char *grown_names = realloc(app.names, cap * 2 * 256);
            bool *grown_dirs = realloc(app.is_dir,
                                       cap * 2 * sizeof(bool));
            size_t *grown_order = realloc(order,
                                          cap * 2 * sizeof(size_t));
            if (grown_names == NULL || grown_dirs == NULL ||
                grown_order == NULL) {
                break;
            }
            app.names = grown_names;
            app.is_dir = grown_dirs;
            order = grown_order;
            cap *= 2;
        }
        struct stat st;
        char full[4096 + 256];
        snprintf(full, sizeof(full), "%s/%s", app.dir, de->d_name);
        bool dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        strcpy(app.names + n * 256, de->d_name);
        app.is_dir[n] = dir;
        order[n] = n;
        n++;
    }
    closedir(d);

    qsort(order, n, sizeof(size_t), entry_cmp);
    for (size_t i = 0; i < n; i++) {
        size_t idx = order[i];
        char row[300];
        if (app.is_dir[idx]) {
            snprintf(row, sizeof(row), "%s/", app.names + idx * 256);
        } else {
            snprintf(row, sizeof(row), "%s", app.names + idx * 256);
        }
        (void)fdk_list_append(app.file_list, row, NULL);
    }
    free(order);
    app.count = n;
    app.name_cap = cap;

    (void)fdk_label_set_text(app.path_label, app.dir);
    setf(app.status, "%zu item%s — 0 selected", n, n == 1 ? "" : "s");
    printf("PHASE: dir %s %zu\n", app.dir, n);
    fflush(stdout);
    report_selection();
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
        /* Manual join: snprintf's provable-truncation warning on a
         * 4096+1+name join is exactly the (silent, bounded) behavior
         * wanted, spelled out instead. */
        size_t dl = strlen(app.dir), sl = strlen(sub);
        size_t room = sizeof(next) - 1;
        size_t take = dl;
        if (take > room) {
            take = room;
        }
        memcpy(next, app.dir, take);
        size_t used = take;
        if (used < room) {
            next[used++] = '/';
        }
        size_t take_sub = sl;
        if (take_sub > room - used) {
            take_sub = room - used;
        }
        memcpy(next + used, sub, take_sub);
        used += take_sub;
        next[used] = '\0';
    }
    snprintf(app.dir, sizeof(app.dir), "%s", next);
    load_dir();
}

static void chdir_abs(const char *dir) {
    snprintf(app.dir, sizeof(app.dir), "%s", dir);
    load_dir();
}

/* ------------------------------------------------------------------ */
/* selection reporting (the example is also a functional test)        */
/* ------------------------------------------------------------------ */

void report_selection(void) {
    size_t n = fdk_list_selected_count(app.file_list);
    if (n == 0) {
        setf(app.status, "%zu items — 0 selected", app.count);
        printf("PHASE: sel 0\n");
    } else if (n == 1) {
        size_t row = 0;
        (void)fdk_list_selected_at(app.file_list, 0, &row);
        char full[4096 + 300];
        snprintf(full, sizeof(full), "%s/%s", app.dir,
                 app.names + row * 256);
        const char *kind = app.is_dir[row] ? "Selected folder"
                                           : "Selected";
        setf(app.status, "%s: %s", kind, full);
        printf("PHASE: sel 1 %s\n", full);
    } else {
        setf(app.status, "%zu items — Selected %zu items",
             app.count, n);
        printf("PHASE: sel %zu\n", n);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* widget callbacks                                                    */
/* ------------------------------------------------------------------ */

static void on_selection(fdk_widget *list, void *user) {
    (void)list; (void)user;
    report_selection();
}

static void on_row_activated(fdk_widget *list, size_t row, void *user) {
    (void)list; (void)user;
    if (row < app.count && app.is_dir[row]) {
        chdir_rel(app.names + row * 256);
    }
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

static void on_place(fdk_widget *list, void *user) {
    (void)user;
    fdk_i64 sel = fdk_list_get_selected(list);
    switch (sel) {
    case 0: {
        const char *home = getenv("HOME");
        chdir_abs(home != NULL ? home : "/");
        break;
    }
    case 1: {
        char p[4096];
        const char *home = getenv("HOME");
        snprintf(p, sizeof(p), "%s/Desktop",
                 home != NULL ? home : "");
        chdir_abs(p);
        break;
    }
    case 2: {
        char p[4096];
        const char *home = getenv("HOME");
        snprintf(p, sizeof(p), "%s/Documents",
                 home != NULL ? home : "");
        chdir_abs(p);
        break;
    }
    case 3: {
        char p[4096];
        const char *home = getenv("HOME");
        snprintf(p, sizeof(p), "%s/Downloads",
                 home != NULL ? home : "");
        chdir_abs(p);
        break;
    }
    case 4:
        chdir_abs("/");
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* window events                                                       */
/* ------------------------------------------------------------------ */

static void window_event(fdk_window *window, const fdk_event_data *event,
                         void *user) {
    (void)window; (void)user;
    if (event->type == FDK_EVENT_WINDOW_CLOSE_REQUEST ||
        (event->type == FDK_EVENT_KEY_DOWN &&
         event->key.scancode == FDK_KEY_ESC)) {
        app.quit = true;
    }
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

static void relayout(void) {
    fdk_size ws;
    (void)fdk_window_get_size(app.window, &ws);
    fdk_i32 pad = 8;
    fdk_i32 x = pad, y = pad;
    fdk_i32 iw = ws.width - pad * 2;

    /* Toolbar. */
    fdk_size n = {0, 0};
    fdk_i32 tx = x;
    fdk_widget_measure(app.btn_up, &n);
    fdk_widget_set_bounds(app.btn_up, (fdk_rect){tx, y, n.width, 28});
    tx += n.width + 6;
    fdk_widget_measure(app.btn_refresh, &n);
    fdk_widget_set_bounds(app.btn_refresh,
                          (fdk_rect){tx, y, n.width, 28});
    tx += n.width + 6;
    fdk_widget_measure(app.btn_hidden, &n);
    fdk_widget_set_bounds(app.btn_hidden,
                          (fdk_rect){tx, y, n.width, 28});
    y += 34;

    /* Path bar. */
    fdk_widget_set_bounds(app.path_label, (fdk_rect){x, y, iw, 20});
    y += 24;

    /* Places + file list share the remaining height. */
    fdk_i32 status_h = 22;
    fdk_i32 body_h = ws.height - pad - status_h - 6 - y;
    if (body_h < 100) {
        body_h = 100;
    }
    fdk_i32 places_w = 150;
    fdk_widget_set_bounds(app.places,
                          (fdk_rect){x, y, places_w, body_h});
    fdk_widget_set_bounds(
        app.file_list,
        (fdk_rect){x + places_w + pad, y,
                   iw - places_w - pad, body_h});

    /* Status bar. */
    fdk_widget_set_bounds(app.status,
                          (fdk_rect){x, ws.height - pad - status_h,
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

    fdk_init_options init = {0};
    init.app_id = "10_file_manager";
    if (!fdk_ok(fdk_init(&app.ctx, &init))) {
        fprintf(stderr, "10_file_manager: no display — see docs\n");
        return 1;
    }
    app.font = fdk_font_load_system_default(14);
    if (app.font == NULL) {
        fprintf(stderr, "10_file_manager: no system font\n");
        fdk_shutdown(app.ctx);
        return 1;
    }

    fdk_window_options wopts = {
        .title = "FDK File Manager",
        .width = 760,
        .height = 480,
    };
    if (!fdk_ok(fdk_window_create(app.ctx, &wopts, &app.window))) {
        fdk_font_destroy(app.font);
        fdk_shutdown(app.ctx);
        return 1;
    }
    fdk_window_set_event_callback(app.window, window_event, NULL);

    fdk_widget *root = NULL;
    (void)fdk_window_get_root(app.window, &root);

    (void)fdk_button_create(root, app.font, "Up", &app.btn_up);
    fdk_button_set_on_activate(app.btn_up, up_clicked, NULL);
    (void)fdk_button_create(root, app.font, "Refresh",
                            &app.btn_refresh);
    fdk_button_set_on_activate(app.btn_refresh, refresh_clicked, NULL);
    (void)fdk_toggle_create(root, app.font, "Hidden", &app.btn_hidden);
    fdk_toggle_set_on_change(app.btn_hidden, hidden_toggled, NULL);

    (void)fdk_label_create(root, app.font, "/", &app.path_label);
    fdk_label_set_mode(app.path_label, FDK_LABEL_ELLIPSIZE);

    (void)fdk_list_create(root, app.font, &app.places);
    fdk_list_set_selection_mode(app.places,
                                FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_selection_changed(app.places, on_place, NULL);
    (void)fdk_list_append(app.places, "Home", NULL);
    (void)fdk_list_append(app.places, "Desktop", NULL);
    (void)fdk_list_append(app.places, "Documents", NULL);
    (void)fdk_list_append(app.places, "Downloads", NULL);
    (void)fdk_list_append(app.places, "Filesystem", NULL);

    (void)fdk_list_create(root, app.font, &app.file_list);
    fdk_list_set_selection_mode(app.file_list,
                                FDK_LIST_SELECTION_MULTIPLE);
    fdk_list_set_on_selection_changed(app.file_list, on_selection,
                                      NULL);
    fdk_list_set_on_row_activate(app.file_list, on_row_activated, NULL);

    (void)fdk_label_create(root, app.font, "", &app.status);

    const char *home = getenv("HOME");
    snprintf(app.dir, sizeof(app.dir), "%s",
             (home != NULL && home[0]) ? home : "/");
    relayout();
    load_dir();
    fdk_window_set_content(app.window, root);
    fdk_window_show(app.window);

    /* Announce geometry for the rigs (post-layout). */
    rig_announce("places", app.places);
    rig_announce("filelist", app.file_list);
    rig_announce("btn-up", app.btn_up);
    rig_announce("btn-refresh", app.btn_refresh);
    rig_announce("btn-hidden", app.btn_hidden);

    const char *frames = getenv("FDK_DEMO_FRAMES");
    app.frames_left = frames != NULL ? atoi(frames) : -1;

    while (!app.quit) {
        (void)fdk_pump_events(app.ctx, 15);
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
