/* Defined BEFORE every include: asprintf() and realpath() hide
 * behind _DEFAULT_SOURCE on strict feature-test builds. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#define FDK_LOG_TAG "widgets"

/*
 * file_dialog.c — file / folder selection dialogs (1.2.0)
 *
 * A real FDK window built from the stock catalog, following the
 * message-dialog lifecycle exactly (dialog.c): toolkit-owned,
 * auto-painted, one callback, self-destroying. The difference is the
 * content — a small file browser:
 *
 *   [ Up ]  [ x Hidden ]        <- toolbar row
 *   /current/path               <- path label
 *   +------------------------+
 *   | Documents/             |   <- the list (SINGLE or MULTIPLE
 *   | downloads/             |      selection per kind; FOLDER kinds
 *   | notes.txt              |      list directories only)
 *   | render.c               |
 *   +------------------------+
 *   status line                  <- why Open refused / what happened
 *              [ Cancel ] [ Open ]
 *
 * The scan is a real opendir/readdir pass (directories first, then
 * alphabetical; hidden entries behind the toggle). Accepting runs a
 * stat() re-validation on every selected path — the listing is a
 * snapshot, the filesystem is not. The result model is explicit:
 * ACCEPTED with paths[], CANCELLED, or ERROR (could not browse even
 * the fallback chain start_dir -> $HOME -> /).
 *
 * Double-click / Enter on a directory descends into it (the same
 * row-activation callback the file manager example uses); on a file
 * it accepts the current selection. "Up" climbs toward /.
 *
 * Modal grabs follow the message-dialog contract (X11 grab; Wayland
 * has no toplevel-grab protocol — accepted and ignored, documented
 * rather than faked).
 */

#include "widgets_internal.h"
#include "../theme/theme_internal.h"
#include "../window/window_internal.h"
#include "fdk/fdk_dialog.h"

#include "core/alloc_internal.h"
#include "core/log_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FD_PAD 12          /* outer padding                        */
#define FD_GAP 8           /* vertical gap between rows            */
#define FD_TOPBAR_H 34     /* Up + Hidden toggle row               */
#define FD_STATUS_H 22     /* status line                          */
#define FD_BTN_MIN_W 90    /* minimum button width                 */
#define FD_BTN_PAD 32      /* button text padding (both sides)     */
#define FD_LIST_W 460      /* dialog width                         */
#define FD_LIST_H 300      /* list viewport height                 */
#define FD_MAX_ENTRIES 8192 /* scan cap: pathological dirs degrade,
                               they do not OOM the dialog           */

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

/* Scans `dir`. Returns 0 on success (even an empty listing is a
 * success); -1 when the directory cannot be opened. `dirs_only`
 * filters to directories (the FOLDER kinds); `show_hidden` includes
 * dot entries. The result is fully owned by the caller
 * (fd_entries_free). Sorting: dirs first, then alphabetical. */
int fdk__file_dialog_scan(const char *dir, bool dirs_only,
                          bool show_hidden, fdk_fd_entries *out) {
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
            if (asprintf(&full, "%s/%s", dir, de->d_name) < 0) {
                continue;
            }
            struct stat st;
            is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        }
        if (dirs_only && !is_dir) {
            free(full);
            continue;
        }
        if (out->count == cap) {
            if (out->count >= FD_MAX_ENTRIES) {
                free(full);
                continue; /* cap reached: extra entries are skipped,
                             a warning, not a failure */
            }
            fdk_fd_entry *grown =
                fdk_realloc(out->v, cap * 2 * sizeof(fdk_fd_entry));
            if (grown == NULL) {
                free(full);
                break;
            }
            out->v = grown;
            cap *= 2;
        }
        out->v[out->count].name = fdk__strdup(de->d_name);
        if (out->v[out->count].name == NULL) {
            free(full);
            break;
        }
        out->v[out->count].dir = is_dir;
        out->v[out->count].hidden = hidden;
        out->count++;
        free(full);
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

/* ---- the dialog ---- */

typedef struct fdk_file_dialog {
    fdk_window *window;      /* owned lifecycle (self-destroying) */
    fdk_widget *body;        /* the arrange-hooked content widget */
    fdk_widget *up_btn;
    fdk_widget *hidden_toggle;
    fdk_widget *path_label;
    fdk_widget *list;
    fdk_widget *status;
    fdk_widget *accept_btn;
    fdk_widget *cancel_btn;
    fdk_font *font;          /* owned (system default we loaded)   */
    fdk_file_dialog_kind kind;
    bool show_hidden;
    char *dir;               /* owned; the browsed directory      */
    fdk_fd_entries entries;      /* the current listing               */
    fdk_file_dialog_done_fn on_done;
    void *on_done_user;
    fdk_file_dialog_result pending; /* built during accept        */
    bool answered;
} fdk_file_dialog;

typedef struct fdk_file_dialog_body {
    fdk_widget base;
    fdk_file_dialog *dialog; /* owned */
} fdk_file_dialog_body;

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

static void fdlg_set_status(fdk_file_dialog *d, const char *text) {
    if (d->status != NULL) {
        (void)fdk_label_set_text(d->status, text);
    }
}

/* ---- browsing ---- */

static void fdlg_reload(fdk_file_dialog *d) {
    fdk_list_clear(d->list);
    fdk__file_dialog_entries_free(&d->entries);
    if (fdk__file_dialog_scan(d->dir, fdlg_kind_folders(d->kind),
                              d->show_hidden, &d->entries) != 0) {
        fdlg_set_status(d, "Cannot open this directory");
        (void)fdk_label_set_text(d->path_label, d->dir);
        return;
    }
    for (size_t i = 0; i < d->entries.count; i++) {
        char row[512];
        if (d->entries.v[i].dir) {
            snprintf(row, sizeof(row), "%s/", d->entries.v[i].name);
        } else {
            snprintf(row, sizeof(row), "%s", d->entries.v[i].name);
        }
        (void)fdk_list_append(d->list, row, NULL);
    }
    (void)fdk_label_set_text(d->path_label, d->dir);
    char status[64];
    snprintf(status, sizeof(status), "%zu item%s", d->entries.count,
             d->entries.count == 1 ? "" : "s");
    fdlg_set_status(d, status);
}

static void fdlg_browse(fdk_file_dialog *d, const char *dir) {
    char *copy = fdk__strdup(dir);
    if (copy == NULL) {
        return;
    }
    fdk_free(d->dir);
    d->dir = copy;
    fdlg_reload(d);
}

static void fdlg_up(fdk_file_dialog *d) {
    size_t len = strlen(d->dir);
    if (len <= 1) {
        return; /* already at / */
    }
    char *slash = strrchr(d->dir, '/');
    if (slash == d->dir) {
        fdlg_browse(d, "/");
    } else {
        /* truncate in place, then hand a copy to browse (it owns) */
        *slash = '\0';
        fdlg_browse(d, d->dir);
    }
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

/* Builds absolute, canonical paths for the selected rows, re-validates
 * each against the kind's contract with stat(), and accepts. */
static void fdlg_try_accept(fdk_file_dialog *d) {
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
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", d->dir,
                 d->entries.v[rows[0]].name);
        fdk_free(rows);
        fdlg_browse(d, path);
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
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", d->dir,
                 d->entries.v[rows[i]].name);
        char resolved[4096];
        const char *use = full;
        if (realpath(full, resolved) != NULL) {
            use = resolved;
        }
        struct stat st;
        if (stat(use, &st) != 0) {
            fdlg_set_status(d, "Selection vanished — reselect");
            goto fail;
        }
        bool ok = fdlg_kind_folders(d->kind) ? S_ISDIR(st.st_mode)
                                             : S_ISREG(st.st_mode);
        if (!ok) {
            fdlg_set_status(d, fdlg_kind_folders(d->kind)
                                   ? "Not a folder"
                                   : "Not a regular file");
            goto fail;
        }
        paths[filled] = fdk__strdup(use);
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

/* ---- widget callbacks ---- */

static void fdlg_up_clicked(fdk_widget *w, void *user) {
    (void)w;
    fdk_file_dialog *d = user;
    fdlg_up(d);
}

static void fdlg_hidden_toggled(fdk_widget *w, bool checked, void *user) {
    (void)w;
    fdk_file_dialog *d = user;
    d->show_hidden = checked;
    fdlg_reload(d);
}

static void fdlg_row_activated(fdk_widget *list, size_t row, void *user) {
    (void)list;
    fdk_file_dialog *d = user;
    if (row >= d->entries.count) {
        return;
    }
    if (d->entries.v[row].dir) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", d->dir,
                 d->entries.v[row].name);
        fdlg_browse(d, path);
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
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdlg_cancelled(d);
        return;
    }
    if (ev->type == FDK_EVENT_KEY_DOWN &&
        ev->key.scancode == FDK_KEY_ESC) {
        fdlg_cancelled(d);
    }
}

/* Destroy-notify: died without answering (app teardown paths). The
 * documented contract: that answers CANCELLED (the dialog never
 * claimed a selection). */
static void fdlg_destroyed(fdk_window *window, void *user) {
    (void)window;
    fdk_file_dialog *d = user;
    if (d->answered) {
        return;
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

static void fdlg_body_arrange(fdk_widget *w, fdk_rect a) {
    fdk_widget_set_bounds(w, a);
    fdk_file_dialog *d = fdlg_of_body(w);
    if (d == NULL) {
        return;
    }
    fdk_i32 x = a.x + FD_PAD;
    fdk_i32 y = a.y + FD_PAD;
    fdk_i32 iw = a.width - FD_PAD * 2;

    /* Toolbar: Up + toggle left, sized by measure. */
    fdk_size up_n = {0, 0};
    fdk_widget_measure(d->up_btn, &up_n);
    fdk_widget_set_bounds(
        d->up_btn, (fdk_rect){x, y, up_n.width, FD_TOPBAR_H});
    fdk_size hid_n = {0, 0};
    fdk_widget_measure(d->hidden_toggle, &hid_n);
    fdk_widget_set_bounds(d->hidden_toggle,
                          (fdk_rect){x + up_n.width + FD_GAP, y,
                                     hid_n.width, FD_TOPBAR_H});
    y += FD_TOPBAR_H + FD_GAP / 2;

    /* Path line. */
    fdk_size path_n = {0, 0};
    fdk_widget_measure(d->path_label, &path_n);
    fdk_i32 path_h = path_n.height > 0 ? path_n.height : 18;
    fdk_widget_set_bounds(
        d->path_label, (fdk_rect){x, y, iw, path_h});
    y += path_h + FD_GAP / 2;

    /* Buttons: bottom-right. */
    fdk_size acc_n = {0, 0}, can_n = {0, 0};
    fdk_widget_measure(d->accept_btn, &acc_n);
    fdk_widget_measure(d->cancel_btn, &can_n);
    fdk_i32 btn_h = (acc_n.height > can_n.height) ? acc_n.height
                                                  : can_n.height;
    fdk_i32 status_y = a.y + a.height - FD_PAD - FD_STATUS_H;
    fdk_i32 btn_y = status_y - FD_GAP - btn_h;

    /* List fills the middle. */
    fdk_i32 list_h = btn_y - FD_GAP - y;
    if (list_h < 40) {
        list_h = 40;
    }
    fdk_widget_set_bounds(d->list, (fdk_rect){x, y, iw, list_h});

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
    if (d->font != NULL) {
        fdk_font_destroy(d->font);
        d->font = NULL;
    }
    fdk__file_dialog_entries_free(&d->entries);
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
    .paint = NULL, /* base paint: the window_background fill */
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

fdk_result fdk_dialog_open_file(fdk_context *ctx,
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
        kind > FDK_FILE_DIALOG_OPEN_FOLDERS) {
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
    d->kind = kind;
    d->show_hidden = (options != NULL) && options->show_hidden;
    d->on_done = on_done;
    d->on_done_user = user_data;

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
                                       d->show_hidden, &probe);
        if (opened == 0) {
            d->dir = fdk__strdup(fallbacks[i]);
            fdk__file_dialog_entries_free(&probe);
        }
    }
    if (opened != 0 || d->dir == NULL) {
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

    /* Window + body, per the message-dialog pattern. */
    fdk_i32 width = FD_LIST_W + FD_PAD * 2;
    fdk_i32 height = FD_PAD * 2 + FD_TOPBAR_H + 20 + FD_LIST_H +
                     FD_STATUS_H + 48;
    fdk_window_options wopts = {
        .title = title,
        .width = width,
        .height = height,
    };
    fdk_window *win = NULL;
    fdk_result r = fdk_window_create(ctx, &wopts, &win);
    if (!fdk_ok(r)) {
        fdk_free(d->dir);
        fdk_free(d);
        return r;
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
    d->body = body;
    fdk_widget_set_accessible_name(body, title);
    fdk_widget_set_background(
        body, fdk_theme_get_color(NULL, FDK_TK_WINDOW_BACKGROUND));

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

    r = fdk_toggle_create(body, d->font, "Hidden", &d->hidden_toggle);
    if (!fdk_ok(r)) {
        goto fail;
    }
    if (d->show_hidden) {
        fdk_toggle_set_checked(d->hidden_toggle, true);
    }
    fdk_toggle_set_on_change(d->hidden_toggle, fdlg_hidden_toggled, d);

    r = fdk_label_create(body, d->font, d->dir, &d->path_label);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_label_set_mode(d->path_label, FDK_LABEL_ELLIPSIZE);

    r = fdk_list_create(body, d->font, &d->list);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_list_set_selection_mode(
        d->list, fdlg_kind_multi(kind)
                    ? FDK_LIST_SELECTION_MULTIPLE
                    : FDK_LIST_SELECTION_SINGLE);
    fdk_list_set_on_row_activate(d->list, fdlg_row_activated, d);

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
    r = fdk_button_create(body, d->font, "Cancel", &d->cancel_btn);
    if (!fdk_ok(r)) {
        goto fail;
    }
    fdk_button_set_on_activate(d->cancel_btn, fdlg_cancel_clicked, d);

    fdk_window_set_content(win, body);
    fdlg_reload(d);

    fdk_window_show(win);
    if (modal) {
        (void)fdk__window_set_modal(win, true);
    }
    /* The list takes the initial focus: keyboard browsing works from
     * the first keypress. */
    fdk_widget_focus(d->list);

    if (out_window != NULL) {
        *out_window = win;
    }
    return FDK_OK;

fail:
    if (d->body == NULL) {
        /* The body destroy hook owns d once the body exists. */
        if (d->font != NULL) {
            fdk_font_destroy(d->font);
        }
        fdk__file_dialog_entries_free(&d->entries);
        if (d->pending.paths != NULL) {
            fdk_file_dialog_result_free(&d->pending);
        }
        fdk_free(d->dir);
        fdk_free(d);
    }
    fdk_window_destroy(win);
    return r;
}
