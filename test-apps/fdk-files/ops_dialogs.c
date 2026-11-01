/* ops_dialogs.c — see ops_dialogs.h.
 *
 * The stock message/prompt dialogs cover confirm/ask; the custom
 * windows (progress, properties, app-chooser, trash) are plain FDK
 * windows on the same context, driven by the same pump. */
#include "ops_dialogs.h"
#include "mime.h"
#include "model.h"
#include "panes.h"
#include "teardown.h"
#include "trash.h"

#include "fdk/fdk_dialog.h"
#include "fdk/fdk_layout.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void props_apply(fdk_widget *btn, void *user);

/* ---- the conflict question (YES_NO_CANCEL shaped) ----------------- */

typedef struct conflict_ctx {
    fdk_dialog_response resp;
    bool answered;
} conflict_ctx;

static void conflict_cb(fdk_dialog_response response, void *user) {
    conflict_ctx *c = user;
    c->resp = response;
    c->answered = true;
}

conflict_answer ops_ask_conflict(const char *dst, bool multiple,
                                 bool *apply_all) {
    char msg[FI_PATH_MAX + 160];
    const char *base = strrchr(dst, '/');
    base = (base != NULL) ? base + 1 : dst;
    snprintf(msg, sizeof msg,
             "\"%.150s\" already exists.", base);

    conflict_ctx c = {FDK_DIALOG_CANCEL, false};
    fdk_dialog_show_message(
        fapp.base.ctx,
        &(fdk_dialog_options){
            .title = "File conflict",
            .text = msg,
            .buttons = (multiple && apply_all != NULL)
                           ? FDK_DIALOG_BUTTONS_YES_NO_CANCEL
                           : FDK_DIALOG_BUTTONS_YES_NO,
            .modal = true,
            .parent = fapp.base.window,
        },
        conflict_cb, &c, NULL);

    while (!c.answered && !fapp.base.quit) {
        if (fdk_pump_events(fapp.base.ctx, 15) < 0) {
            break;
        }
        if (fdk_widget_tree_has_damage(fapp.base.root)) {
            (void)fdk_window_paint(fapp.base.window);
        }
    }
    /* YES = Replace, NO = Skip, CANCEL = Abort (the multi-rename
     * "both" answer is the YES_NO_CANCEL third leg; documented in
     * the README as the mapping we chose) */
    switch (c.resp) {
    case FDK_DIALOG_YES:
        return CONFLICT_REPLACE;
    case FDK_DIALOG_NO:
        return CONFLICT_SKIP;
    case FDK_DIALOG_CANCEL:
    default:
        return CONFLICT_ABORT;
    }
}

/* ---- the delete confirm -------------------------------------------- */

typedef struct confirm_ctx {
    fdk_dialog_response resp;
    bool answered;
} confirm_ctx;

static void confirm_cb(fdk_dialog_response response, void *user) {
    confirm_ctx *c = user;
    c->resp = response;
    c->answered = true;
}

bool ops_confirm_delete(size_t count) {
    char msg[160];
    snprintf(msg, sizeof msg,
             "Permanently delete %zu item%s? This cannot be undone.",
             count, count == 1 ? "" : "s");
    confirm_ctx c = {FDK_DIALOG_CANCEL, false};
    fdk_dialog_show_message(
        fapp.base.ctx,
        &(fdk_dialog_options){
            .title = "Delete permanently",
            .text = msg,
            .buttons = FDK_DIALOG_BUTTONS_OK_CANCEL,
            .modal = true,
            .parent = fapp.base.window,
        },
        confirm_cb, &c, NULL);
    while (!c.answered && !fapp.base.quit) {
        if (fdk_pump_events(fapp.base.ctx, 15) < 0) {
            break;
        }
        if (fdk_widget_tree_has_damage(fapp.base.root)) {
            (void)fdk_window_paint(fapp.base.window);
        }
    }
    return c.resp == FDK_DIALOG_OK;
}

/* ---- the open-with chain (.desktop associations) -------------------
 *
 * Reads the XDG data dirs application .desktop files,
 * parses Type=Application entries with a matching MimeType=, and
 * offers the Name/Icon/Exec rows; launching forks /bin/sh -c with
 * the field codes expanded (%f -> the path, %u/%F/%U likewise —
 * the deprecated %d/%D/%n/%N/%v/%m dropped per the spec). */

typedef struct desktop_app {
    char name[128];
    char exec[FI_PATH_MAX];
    char icon[64];
    char path[FI_PATH_MAX]; /* the .desktop file                */
} desktop_app;

static size_t load_associations(const char *mime,
                                desktop_app **out) {
    *out = NULL;
    desktop_app *v = NULL;
    size_t n = 0, cap = 0;

    const char *dirs[8];
    size_t nd = 0;
    const char *xh = getenv("XDG_DATA_HOME");
    if (xh != NULL && xh[0] != '/') {
        xh = NULL;
    }
    if (xh == NULL) {
        static char def[FI_PATH_MAX];
        snprintf(def, sizeof def, "%s/.local/share", fi_home());
        xh = def;
    }
    dirs[nd++] = xh;
    const char *xd = getenv("XDG_DATA_DIRS");
    if (xd == NULL || xd[0] == '\0') {
        xd = "/usr/local/share:/usr/share";
    }
    char dup[1024];
    snprintf(dup, sizeof dup, "%s", xd);
    char *save = NULL;
    for (char *d = strtok_r(dup, ":", &save);
         d != NULL && nd < 8; d = strtok_r(NULL, ":", &save)) {
        dirs[nd++] = d;
    }

    for (size_t di = 0; di < nd; di++) {
        char appdir[FI_PATH_MAX];
        snprintf(appdir, sizeof appdir, "%s/applications", dirs[di]);
        DIR *d = opendir(appdir);
        if (d == NULL) {
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t nl = strlen(de->d_name);
            if (nl < 9 || strcmp(de->d_name + nl - 8, ".desktop") != 0) {
                continue;
            }
            char fp[FI_PATH_MAX];
            snprintf(fp, sizeof fp, "%.3800s/%.180s", appdir, de->d_name);
            FILE *f = fopen(fp, "r");
            if (f == NULL) {
                continue;
            }
            char line[2048];
            char name[128] = "", exec[FI_PATH_MAX] = "",
                 icon[64] = "", type[32] = "", mimes[2048] = "";
            bool in_group = false;
            while (fgets(line, sizeof line, f) != NULL) {
                if (line[0] == '[') {
                    in_group = strncmp(line, "[Desktop Entry]", 15) == 0;
                    continue;
                }
                if (!in_group) {
                    continue;
                }
                if (strncmp(line, "Name=", 5) == 0) {
                    sscanf(line + 5, "%127[^\n]", name);
                } else if (strncmp(line, "Exec=", 5) == 0) {
                    sscanf(line + 5, "%4095[^\n]", exec);
                } else if (strncmp(line, "Icon=", 5) == 0) {
                    sscanf(line + 5, "%63[^\n]", icon);
                } else if (strncmp(line, "Type=", 5) == 0) {
                    sscanf(line + 5, "%31[^\n]", type);
                } else if (strncmp(line, "MimeType=", 9) == 0) {
                    sscanf(line + 9, "%2047[^\n]", mimes);
                }
            }
            fclose(f);
            if (strcmp(type, "Application") != 0 || name[0] == '\0' ||
                exec[0] == '\0') {
                continue;
            }
            /* mime match: the semicolon-separated list */
            bool matches = false;
            char *save2 = NULL;
            char mdup[2048];
            snprintf(mdup, sizeof mdup, "%s", mimes);
            for (char *m = strtok_r(mdup, ";", &save2); m != NULL;
                 m = strtok_r(NULL, ";", &save2)) {
                if (strcmp(m, mime) == 0) {
                    matches = true;
                    break;
                }
            }
            if (!matches) {
                continue;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                desktop_app *g = realloc(v, cap * sizeof *g);
                if (g == NULL) {
                    break;
                }
                v = g;
            }
            snprintf(v[n].name, sizeof v[n].name, "%s", name);
            snprintf(v[n].exec, sizeof v[n].exec, "%s", exec);
            snprintf(v[n].icon, sizeof v[n].icon, "%s", icon);
            snprintf(v[n].path, sizeof v[n].path, "%s", fp);
            n++;
        }
        closedir(d);
    }
    *out = v;
    return n;
}

static void launch_desktop(const desktop_app *app, const char *path) {
    /* expand the field codes we support; deprecated ones stripped */
    char cmd[FI_PATH_MAX + 256];
    const char *s = app->exec;
    char *o = cmd;
    while (*s != '\0' && (size_t)(o - cmd) < sizeof cmd - 8) {
        if (*s == '%') {
            s++;
            switch (*s) {
            case 'f':
            case 'u':
                o += snprintf(o, 8 + (size_t)(cmd + sizeof cmd - o),
                              "'%s'", path);
                break;
            case 'F':
            case 'U':
                o += snprintf(o, 8 + (size_t)(cmd + sizeof cmd - o),
                              "'%s'", path);
                break;
            case 'i':
            case 'c':
            case 'k':
                /* icon/name/location — menus use these; a launcher
                 * drops them (documented) */
                break;
            case '%':
                *o++ = '%';
                break;
            default:
                break; /* %d %D %n %N %v %m: deprecated, dropped */
            }
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';

    char full[FI_PATH_MAX + 512];
    snprintf(full, sizeof full, "%.4000s &", cmd);
    int rc = system(full);
    if (rc == -1) {
        fi_set_status("Could not launch %.150s", app->name);
    } else {
        fi_set_status("Opened with %.150s", app->name);
    }
}

/* ---- the app chooser window ---------------------------------------- */

typedef struct chooser_ctx {
    fdk_window *win;
    const file_info *fi;
    desktop_app *apps;
    size_t count;
    fdk_widget *list;
} chooser_ctx;

static void chooser_row(fdk_widget *list, size_t row, void *user) {
    chooser_ctx *c = user;
    if (row < c->count) {
        launch_desktop(&c->apps[row], c->fi->path);
    }
    fdk_window_destroy(c->win);
    td_untrack(c);
    free(c->apps);
    free(c);
    (void)list;
}

static void chooser_close(fdk_window *w, const fdk_event_data *ev,
                          void *user) {
    chooser_ctx *c = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        td_untrack(c);
        free(c->apps);
        free(c);
        fdk_window_destroy(w);
    }
}

void ops_choose_app(pane *p, const file_info *fi) {
    (void)p;
    desktop_app *apps = NULL;
    size_t n = load_associations(fi->mime, &apps);
    if (n == 0) {
        free(apps);
        fi_set_status("No application is associated with %s",
                      fi->mime);
        return;
    }
    chooser_ctx *c = calloc(1, sizeof *c);
    c->fi = fi;
    c->apps = apps;
    c->count = n;
    td_track(c, TD_RAW);

    fdk_window_options wo = {0};
    wo.title = "Open with";
    wo.width = 420;
    wo.height = 360;
    (void)fdk_window_create(fapp.base.ctx, &wo, &c->win);
    (void)fdk_window_set_decorated(c->win, true);
    fdk_window_set_event_callback(c->win, chooser_close, c);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(c->win, &root);
    fdk_widget *box = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 12);
    fdk_box_set_spacing(box, 8);
    fdk_window_set_content(c->win, box);

    fdk_widget *label = NULL;
    char text[256];
    snprintf(text, sizeof text,
             "Choose an application for \"%.200s\"", fi->name);
    (void)fdk_label_create(box, fapp.base.font_ui, text, &label);

    (void)fdk_list_create(box, fapp.base.font_ui, &c->list);
    fdk_widget_set_expand(c->list, true, true);
    for (size_t i = 0; i < n; i++) {
        size_t idx = 0;
        (void)fdk_list_append(c->list, apps[i].name, &idx);
        (void)fdk_list_row_set_icon(c->list, idx, FDK_ROW_ICON_FILE);
    }
    fdk_list_set_on_row_activate(c->list, chooser_row, c);
    fdk_widget_set_accessible_name(c->list, "Applications");
    fdk_window_show(c->win);
}

void ops_open_with(pane *p, const file_info *fi) {
    desktop_app *apps = NULL;
    size_t n = load_associations(fi->mime, &apps);
    if (n == 1) {
        launch_desktop(&apps[0], fi->path);
        free(apps);
        return;
    }
    free(apps);
    ops_choose_app(p, fi);
}

/* ---- the progress dialog + the sliced engine drive ----------------- */

typedef struct progress_ctx {
    fdk_window *win;
    fdk_widget *label_file;
    fdk_widget *bar;
    fdk_widget *label_stats;
    fdk_widget *cancel;
    fi_op *op;
    fdk_timer *timer;
    pane *pane;
    char dst_dir[FI_PATH_MAX];
    conflict_answer policy;
    bool policy_set;
    bool finished;
} progress_ctx;

static progress_ctx *g_progress;

static void progress_tick(fdk_timer *timer, void *user) {
    progress_ctx *pc = user;
    if (pc->finished) {
        return;
    }
    /* a slice of the operation */
    bool more = fi_op_step(pc->op);
    char cur[FI_NAME_MAX];
    const char *slash = strrchr(pc->op->cur_file, '/');
    snprintf(cur, sizeof cur, "%.250s",
             (slash != NULL) ? slash + 1 : pc->op->cur_file);
    (void)fdk_label_set_text(pc->label_file, cur);
    char stats[128];
    char done_s[48], total_s[48];
    fi_fmt_size(pc->op->done_bytes, done_s, sizeof done_s);
    fi_fmt_size(pc->op->total_bytes, total_s, sizeof total_s);
    snprintf(stats, sizeof stats, "%.30s of %.30s - %zu files",
             done_s, total_s, pc->op->done_files);
    (void)fdk_label_set_text(pc->label_stats, stats);
    fdk_f32 frac = pc->op->total_bytes > 0
                       ? (fdk_f32)((double)pc->op->done_bytes /
                                   (double)pc->op->total_bytes)
                       : 0.0f;
    (void)fdk_progress_set_fraction(pc->bar, frac);

    if (!more || pc->op->failed || pc->op->cancelled) {
        pc->finished = true;
        fdk_timer_remove(timer);
        if (pc->op->failed) {
            fi_set_status("Operation failed: %s", pc->op->err);
        } else {
            fi_set_status("Operation finished");
        }
        /* a notification when it was long (> 2 MB) */
        if (pc->op->total_bytes > 2 * 1024 * 1024) {
            fdk_notify_request req = {0};
            req.summary = "File operation finished";
            req.body = pc->op->failed ? pc->op->err
                                      : "The operation completed.";
            req.urgency = FDK_NOTIFY_URGENCY_LOW;
            (void)fdk_notify_send(fapp.notify, &req, NULL);
        }
        pane_reload(pc->pane);
        fdk_window_destroy(pc->win);
        fi_op_free(pc->op);
        free(pc);
        g_progress = NULL;
    }
}

static void progress_cancel(fdk_widget *btn, void *user) {
    (void)btn;
    progress_ctx *pc = user;
    pc->op->cancelled = true; /* the next tick unwinds */
}

static void progress_window_event(fdk_window *w,
                                  const fdk_event_data *ev,
                                  void *user) {
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(w);
    }
}

void ops_begin_paste_into(pane *p, const char *const *paths,
                          size_t count, const char *dst_dir,
                          fi_op_kind kind) {
    if (g_progress != NULL) {
        fi_set_status("An operation is already running");
        return;
    }
    progress_ctx *pc = calloc(1, sizeof *pc);
    pc->pane = p;
    snprintf(pc->dst_dir, sizeof pc->dst_dir, "%s", dst_dir);

    fdk_window_options wo = {0};
    wo.title = (kind == FI_OP_COPY) ? "Copying" : "Moving";
    wo.width = 420;
    wo.height = 140;
    (void)fdk_window_create(fapp.base.ctx, &wo, &pc->win);
    (void)fdk_window_set_decorated(pc->win, true);
    fdk_window_set_event_callback(pc->win, progress_window_event,
                                  pc);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(pc->win, &root);
    fdk_widget *box = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 12);
    fdk_box_set_spacing(box, 8);
    fdk_window_set_content(pc->win, box);
    (void)fdk_label_create(box, fapp.base.font_ui, "", &pc->label_file);
    (void)fdk_label_set_mode(pc->label_file, FDK_LABEL_ELLIPSIZE);
    (void)fdk_progress_create(box, &pc->bar);
    fdk_widget_set_natural_size(pc->bar, 0, 14);
    (void)fdk_label_create(box, fapp.base.font_ui, "", &pc->label_stats);
    (void)fdk_button_create(box, fapp.base.font_ui, "Cancel",
                            &pc->cancel);
    fdk_button_set_on_activate(pc->cancel, progress_cancel, pc);

    /* the first (and typically only) item: per-item conflict ask */
    pc->op = fi_op_start(kind, paths[0], NULL);
    /* the destination path: dst_dir/basename */
    const char *base = strrchr(paths[0], '/');
    base = (base != NULL) ? base + 1 : paths[0];
    char dst[FI_PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s", dst_dir, base);

    bool dst_dir_f = false;
    if (fi_conflict(dst, &dst_dir_f)) {
        conflict_answer a =
            ops_ask_conflict(dst, count > 1, NULL);
        if (a == CONFLICT_SKIP || a == CONFLICT_ABORT) {
            fdk_window_destroy(pc->win);
            fi_op_free(pc->op);
            free(pc);
            if (a == CONFLICT_ABORT) {
                return;
            }
            /* skip: continue with the remaining items */
            if (count > 1) {
                ops_begin_paste_into(p, paths + 1, count - 1,
                                     dst_dir, kind);
            }
            return;
        }
        if (a == CONFLICT_REPLACE) {
            /* unlink the destination, then run */
            struct stat st;
            if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
                fi_set_status(
                    "Replacing a directory is refused — rename it "
                    "first (honest limit, see README)");
                fdk_window_destroy(pc->win);
                fi_op_free(pc->op);
                free(pc);
                return;
            }
            unlink(dst);
        } else {
            char unique[FI_PATH_MAX];
            fi_unique_sibling(dst, unique, sizeof unique);
            snprintf(dst, sizeof dst, "%s", unique);
        }
        /* rebuild the op with the (possibly new) destination */
        fi_op_free(pc->op);
        pc->op = fi_op_start(kind, paths[0], dst);
    } else {
        fi_op_free(pc->op);
        pc->op = fi_op_start(kind, paths[0], dst);
    }

    g_progress = pc;
    pc->timer = fdk_timer_add(fapp.base.ctx, 16, true,
                              progress_tick, pc);
    fdk_window_show(pc->win);
}

/* ---- the properties dialog ------------------------------------------ */

typedef struct props_ctx {
    fdk_window *win;
    const file_info *fi;
    bool *done_flag;
} props_ctx;

static void props_close(fdk_window *w, const fdk_event_data *ev,
                        void *user) {
    props_ctx *pc = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(w);
        *pc->done_flag = true;
        td_untrack(pc);
        free(pc);
    }
}

void ops_properties(pane *p, const file_info *fi) {
    (void)p;
    bool done = false;
    props_ctx *pc = calloc(1, sizeof *pc);
    td_track(pc, TD_RAW);
    pc->fi = fi;
    pc->done_flag = &done;

    fdk_window_options wo = {0};
    wo.title = "Properties";
    wo.width = 420;
    wo.height = 460;
    (void)fdk_window_create(fapp.base.ctx, &wo, &pc->win);
    (void)fdk_window_set_decorated(pc->win, true);
    fdk_window_set_event_callback(pc->win, props_close, pc);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(pc->win, &root);
    fdk_widget *box = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 14);
    fdk_box_set_spacing(box, 6);
    fdk_window_set_content(pc->win, box);

    char buf[FI_PATH_MAX + 64];
    fdk_widget *l = NULL;

    (void)fdk_label_create(box, fapp.base.font_ui, fi->name, &l);
    (void)fdk_label_create(box, fapp.base.font_ui, fi->mime, &l);

    off_t total = fi->is_dir ? fs_tree_size(fi->path) : fi->size;
    fi_fmt_size(total, buf, sizeof buf);
    (void)fdk_label_create(box, fapp.base.font_ui, buf, &l);

    snprintf(buf, sizeof buf, "Location: %.200s", fi->path);
    (void)fdk_label_create(box, fapp.base.font_ui, buf, &l);

    char mb[64];
    fi_fmt_mtime(fi->mtime, mb, sizeof mb);
    snprintf(buf, sizeof buf, "Modified: %s", mb);
    (void)fdk_label_create(box, fapp.base.font_ui, buf, &l);

    snprintf(buf, sizeof buf, "Permissions: %04o (uid %u, gid %u)",
             (unsigned)(fi->mode & 07777), (unsigned)fi->uid,
             (unsigned)fi->gid);
    (void)fdk_label_create(box, fapp.base.font_ui, buf, &l);

    /* the permission checkboxes + Apply */
    fdk_widget *rw = NULL;
    (void)fdk_checkbox_create(box, fapp.base.font_ui,
                              "Owner read", &rw);
    (void)fdk_checkbox_set_checked(rw, (fi->mode & 0400) != 0);
    fdk_widget *ww = NULL;
    (void)fdk_checkbox_create(box, fapp.base.font_ui,
                              "Owner write", &ww);
    (void)fdk_checkbox_set_checked(ww, (fi->mode & 0200) != 0);
    fdk_widget *ex = NULL;
    (void)fdk_checkbox_create(box, fapp.base.font_ui,
                              "Owner execute", &ex);
    (void)fdk_checkbox_set_checked(ex, (fi->mode & 0100) != 0);

    fdk_widget *apply = NULL;
    (void)fdk_button_create(box, fapp.base.font_ui, "Apply", &apply);
    struct {
        fdk_widget *r, *w, *x;
        const file_info *fi;
    } *perm = calloc(1, sizeof *perm);
    td_track(perm, TD_RAW);
    perm->r = rw;
    perm->w = ww;
    perm->x = ex;
    perm->fi = fi;
    fdk_button_set_on_activate(
        apply,
        (fdk_button_activate_fn)props_apply, perm);

    fdk_window_show(pc->win);
    while (!done && !fapp.base.quit) {
        if (fdk_pump_events(fapp.base.ctx, 15) < 0) {
            break;
        }
        if (fdk_widget_tree_has_damage(fapp.base.root)) {
            (void)fdk_window_paint(fapp.base.window);
        }
    }
}

static void props_apply(fdk_widget *btn, void *user) {
    struct {
        fdk_widget *r, *w, *x;
        const file_info *fi;
    } *perm = user;
    mode_t m = 0;
    if (fdk_checkbox_is_checked(perm->r)) {
        m |= 0400;
    }
    if (fdk_checkbox_is_checked(perm->w)) {
        m |= 0200;
    }
    if (fdk_checkbox_is_checked(perm->x)) {
        m |= 0100;
    }
    mode_t full = (perm->fi->mode & ~(mode_t)0700) | m;
    if (chmod(perm->fi->path, full) == 0) {
        fi_set_status("Permissions applied to %s", perm->fi->name);
    } else {
        fi_set_status("chmod failed: %s", strerror(errno));
    }
    (void)btn;
}

/* ---- the trash window ------------------------------------------------ */

typedef struct trash_ctx {
    fdk_window *win;
    fdk_widget *list;
    trash_entry *entries;
    size_t count;
} trash_ctx;

static void trash_refresh(trash_ctx *tc);

static void trash_close(fdk_window *w, const fdk_event_data *ev,
                        void *user) {
    trash_ctx *tc = user;
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fdk_window_destroy(w);
        td_untrack(tc);
        trash_list_free(tc->entries);
        free(tc);
    }
}

static void trash_restore_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    trash_ctx *tc = user;
    fdk_i64 sel = fdk_list_get_selected(tc->list);
    if (sel < 0 || (size_t)sel >= tc->count) {
        return;
    }
    char err[256];
    if (trash_restore(tc->entries[sel].name, err, sizeof err)) {
        fi_set_status("Restored %s", tc->entries[sel].orig_path);
        trash_refresh(tc);
    } else {
        fi_set_status("Restore failed: %s", err);
    }
}

static void trash_delete_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    trash_ctx *tc = user;
    fdk_i64 sel = fdk_list_get_selected(tc->list);
    if (sel < 0 || (size_t)sel >= tc->count) {
        return;
    }
    char err[256];
    (void)trash_delete(tc->entries[sel].name, err, sizeof err);
    trash_refresh(tc);
}

static void trash_empty_clicked(fdk_widget *btn, void *user) {
    (void)btn;
    trash_ctx *tc = user;
    char err[256];
    if (trash_empty(err, sizeof err)) {
        fi_set_status("Trash emptied");
        trash_refresh(tc);
    }
}

static void trash_refresh(trash_ctx *tc) {
    fdk_list_clear(tc->list);
    trash_list_free(tc->entries);
    tc->count = trash_list(&tc->entries);
    for (size_t i = 0; i < tc->count; i++) {
        char label[FI_PATH_MAX + 64];
        snprintf(label, sizeof label, "%s (deleted %s)",
                 tc->entries[i].orig_path, tc->entries[i].deleted);
        size_t idx = 0;
        (void)fdk_list_append(tc->list, label, &idx);
        (void)fdk_list_row_set_icon(tc->list, idx,
                                    FDK_ROW_ICON_FILE);
    }
}

void ops_open_trash(void) {
    trash_ctx *tc = calloc(1, sizeof *tc);
    td_track(tc, TD_RAW);
    fdk_window_options wo = {0};
    wo.title = "Trash";
    wo.width = 560;
    wo.height = 380;
    (void)fdk_window_create(fapp.base.ctx, &wo, &tc->win);
    (void)fdk_window_set_decorated(tc->win, true);
    fdk_window_set_event_callback(tc->win, trash_close, tc);
    fdk_widget *root = NULL;
    (void)fdk_window_get_root(tc->win, &root);
    fdk_widget *box = NULL;
    (void)fdk_box_create(root, FDK_VERTICAL, &box);
    fdk_box_set_padding(box, 10);
    fdk_box_set_spacing(box, 6);
    fdk_window_set_content(tc->win, box);

    (void)fdk_list_create(box, fapp.base.font_ui, &tc->list);
    fdk_list_set_selection_mode(tc->list,
                                FDK_LIST_SELECTION_SINGLE);
    fdk_widget_set_expand(tc->list, true, true);
    fdk_widget_set_accessible_name(tc->list, "Trashed items");

    fdk_widget *row = NULL;
    (void)fdk_box_create(box, FDK_HORIZONTAL, &row);
    fdk_box_set_spacing(row, 6);
    fdk_widget *b1 = NULL, *b2 = NULL, *b3 = NULL;
    (void)fdk_button_create(row, fapp.base.font_ui, "Restore", &b1);
    fdk_button_set_on_activate(b1, trash_restore_clicked, tc);
    (void)fdk_button_create(row, fapp.base.font_ui, "Delete", &b2);
    fdk_button_set_on_activate(b2, trash_delete_clicked, tc);
    (void)fdk_button_create(row, fapp.base.font_ui, "Empty trash",
                            &b3);
    fdk_button_set_role(b3, FDK_BUTTON_ROLE_DESTRUCTIVE);
    fdk_button_set_on_activate(b3, trash_empty_clicked, tc);

    trash_refresh(tc);
    fdk_window_show(tc->win);
}
