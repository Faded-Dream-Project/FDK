/* main.c — fdk-files: the wiring, the pump, the smoke rig.
 *
 * Smoke rig protocol (the --smoke path every test-app follows):
 *   PHASE: boot <cwd-entries>
 *   PHASE: dir <path> <n>          every navigation
 *   PHASE: sel <n> [first]         every selection change
 *   PHASE: open <path>             open-with attempts
 *   PHASE: mkdir <path>            New Folder results
 *   PHASE: rename <old> <new>      Rename results
 *   PHASE: trash <requested> <ok>  trash results
 *   PHASE: quit
 * plus Screenshot: <path> lines when FDK_TESTAPP_SCREENSHOT is set.
 */
#include "files.h"
#include "fsops.h"
#include "menus.h"
#include "model.h"
#include "ops_dialogs.h"
#include "panes.h"
#include "teardown.h"
#include "trash.h"

#include "fdk/fdk_dialog.h"
#include "fdk/fdk_i18n.h"
#include "fdk/fdk_layout.h"
#include "fdk/fdk_theme.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

files_app fapp;

static void fi_undo_apply(void *state);
static void fi_undo_redo(void *state);
static void fi_undo_free(void *state);
static void dialog_ok_cb(fdk_dialog_response response, void *user);
static void rename_cb(fdk_dialog_response response,
                      const char *text, void *user);
static void newfolder_cb(fdk_dialog_response response,
                         const char *text, void *user);

/* ---- the ops that live nowhere better ------------------------------ */

void fi_new_folder(pane *p, const char *name) {
    tab *t = active_tab(p);
    char path[FI_PATH_MAX];
    snprintf(path, sizeof path, "%.3700s/%.190s", t->dir, name);
    if (mkdir(path, 0755) == 0) {
        printf("PHASE: mkdir %s\n", path);
        fflush(stdout);
        pane_reload(p);
    } else {
        fi_set_status("mkdir failed: %s", strerror(errno));
    }
}

void fi_rename(pane *p, size_t row, const char *new_name) {
    (void)row;
    const file_info *sel[1];
    if (fmodel_selected(p, sel, 1) != 1) {
        return;
    }
    tab *t = active_tab(p);
    char new_path[FI_PATH_MAX];
    snprintf(new_path, sizeof new_path, "%.3800s/%.200s", t->dir, new_name);
    if (rename(sel[0]->path, new_path) == 0) {
        printf("PHASE: rename %s %s\n", sel[0]->name, new_name);
        fflush(stdout);
        /* undoable */
        fi_undo_entry *e = calloc(1, sizeof *e);
        snprintf(e->from, sizeof e->from, "%.4000s", new_path);
        snprintf(e->to, sizeof e->to, "%.4000s", sel[0]->path);
        fdk_undo_stack_push(
            fapp.undo,
            &(fdk_undo_op){.user_data = e,
                           .undo = fi_undo_apply,
                           .redo = fi_undo_redo,
                           .destroy = fi_undo_free});
        pane_reload(p);
    } else {
        fi_set_status("rename failed: %s", strerror(errno));
    }
}

void fi_delete_selection(pane *p, bool to_trash) {
    const file_info *sel[512];
    size_t n = fmodel_selected(p, sel, 512);
    if (n == 0) {
        return;
    }
    if (!to_trash && !ops_confirm_delete(n)) {
        return;
    }
    size_t ok = 0;
    char err[256];
    for (size_t i = 0; i < n; i++) {
        if (to_trash) {
            if (trash_file(sel[i]->path, err, sizeof err)) {
                ok++;
                fi_undo_entry *e = calloc(1, sizeof *e);
                e->restore = true;
                snprintf(e->trash_name, sizeof e->trash_name, "%.250s",
                         strrchr(sel[i]->path, '/') + 1);
                /* the trash stem: unique_stem may have suffixed it;
                 * the honest undo of trash is RESTORE by stem — we
                 * resolve it by listing (trash.c's stem rule) */
                trash_entry *all = NULL;
                size_t tn = trash_list(&all);
                for (size_t k = 0; k < tn; k++) {
                    if (strcmp(all[k].orig_path, sel[i]->path) == 0) {
                        snprintf(e->trash_name,
                                 sizeof e->trash_name, "%.250s",
                                 all[k].name);
                        break;
                    }
                }
                trash_list_free(all);
                fdk_undo_stack_push(
                    fapp.undo,
                    &(fdk_undo_op){.user_data = e,
                                   .undo = fi_undo_apply,
                                   .redo = fi_undo_redo,
                                   .destroy = fi_undo_free});
            }
        } else {
            if (unlink(sel[i]->path) == 0) {
                ok++;
            } else if (rmdir(sel[i]->path) == 0) {
                ok++;
            } else {
                snprintf(err, sizeof err, "%s", strerror(errno));
            }
        }
    }
    printf("PHASE: trash %zu %zu\n", n, ok);
    fflush(stdout);
    if (ok < n) {
        fi_set_status("Deleted %zu of %zu (%s)", ok, n, err);
    } else {
        fi_set_status("Deleted %zu item%s", ok, ok == 1 ? "" : "s");
    }
    pane_reload(p);
}

void fi_trash_selection(pane *p) {
    fi_delete_selection(p, true);
}

static void fi_undo_apply(void *state) {
    fi_undo_entry *e = state;
    char err[256];
    if (e->restore) {
        (void)trash_restore(e->trash_name, err, sizeof err);
    } else {
        (void)rename(e->from, e->to);
    }
    pane_reload(active_pane());
}

static void fi_undo_redo(void *state) {
    fi_undo_entry *e = state;
    char err[256];
    if (e->restore) {
        /* redo of a trash = re-trash: honest omission (README) */
        (void)err;
    } else {
        (void)rename(e->to, e->from);
    }
    pane_reload(active_pane());
}

static void fi_undo_free(void *state) {
    free(state);
}

/* ---- the clipboard ops ---------------------------------------------- */

void ops_clipboard_set(char op) {
    const file_info *sel[512];
    size_t n = fmodel_selected(active_pane(), sel, 512);
    if (n == 0) {
        return;
    }
    fapp.clipboard_op = op;
    snprintf(fapp.clipboard_dir, sizeof fapp.clipboard_dir, "%s",
             active_tab(active_pane())->dir);
    fapp.clipboard_count = 0;
    for (size_t i = 0; i < n && i < 64; i++) {
        snprintf(fapp.clipboard_names[fapp.clipboard_count],
                 FI_NAME_MAX, "%.250s", sel[i]->name);
        fapp.clipboard_count++;
    }
    fi_set_status("%zu item%s on the clipboard (%s)", n,
                  n == 1 ? "" : "s",
                  op == 'c' ? "copy" : "cut");
}

void ops_clipboard_paste(pane *p) {
    if (fapp.clipboard_op == 0 || fapp.clipboard_count == 0) {
        fi_set_status("The clipboard is empty");
        return;
    }
    static char paths[64][FI_PATH_MAX];
    const char *uris[64];
    size_t n = 0;
    for (size_t i = 0; i < fapp.clipboard_count; i++) {
        snprintf(paths[n], sizeof paths[n], "%.3700s/%.190s",
                 fapp.clipboard_dir, fapp.clipboard_names[i]);
        uris[n] = paths[n];
        n++;
    }
    ops_begin_paste_into(
        p, uris, n, active_tab(p)->dir,
        (fapp.clipboard_op == 'm') ? FI_OP_MOVE : FI_OP_COPY);
    if (fapp.clipboard_op == 'm') {
        fapp.clipboard_op = 0; /* a cut pastes once */
    }
}

/* also put the selection on the REAL clipboard as uri-list */
void ops_clipboard_real(void) {
    const file_info *sel[512];
    size_t n = fmodel_selected(active_pane(), sel, 512);
    if (n == 0) {
        return;
    }
    static const char *uris[512];
    static char bufs[512][FI_PATH_MAX];
    for (size_t i = 0; i < n; i++) {
        snprintf(bufs[i], sizeof bufs[i], "%.4000s", sel[i]->path);
        uris[i] = bufs[i];
    }
    (void)fdk_clipboard_set_uris(fapp.base.ctx, uris, n);
}

/* ---- prefs / about / search / prompts -------------------------------- */

void ops_open_prefs(void) {
    /* the honest v1 prefs surface: a message dialog listing the
     * keys + the filter box (a full prefs window is parked; the
     * prefs themselves ARE persisted through fdk_prefs) */
    fdk_prefs *pr = fapp.base.prefs;
    char body[512];
    snprintf(body, sizeof body,
             "Preferences are stored per-key by the app (fdk_prefs "
             "store: %s).\nCurrent: show-hidden=%s, view=%d, "
             "language=%s",
             pr != NULL ? fdk_prefs_path(pr) : "(in-memory)",
             fapp.show_hidden ? "on" : "off",
             active_tab(active_pane())->view_mode,
             ta_pref_str_or("ui.language"));
    fdk_dialog_response resp = FDK_DIALOG_CANCEL;
    static fdk_dialog_response r;
    (void)fdk_dialog_show_message(
        fapp.base.ctx,
        &(fdk_dialog_options){
            .title = "Preferences",
            .text = body,
            .buttons = FDK_DIALOG_BUTTONS_CLOSE,
            .parent = fapp.base.window,
        },
        dialog_ok_cb, &r, NULL);
    (void)resp;
}

static void dialog_ok_cb(fdk_dialog_response response, void *user) {
    (void)response;
    (void)user;
}

const char *ta_pref_str_or(const char *key) {
    static char buf[32];
    ta_pref_str(&fapp.base, key, "en", buf, sizeof buf);
    return buf;
}

void ops_open_about(void) {
    (void)fdk_dialog_show_about(
        fapp.base.ctx,
        &(fdk_about_dialog_options){
            .program_name = "fdk-files",
            .version = "in development",
            .comments = "The FDK validation file manager. Written "
                        "against FDK's public API only.",
            .website = "https://github.com/Faded-Dream-Project/FDK",
            .license = "Same as the FDK project.",
            .modal = false,
            .parent = fapp.base.window,
        },
        NULL, NULL, NULL, NULL);
}

void ops_focus_search(void) {
    pane *p = active_pane();
    if (p->filter_entry != NULL) {
        (void)fdk_widget_focus(p->filter_entry);
    }
}

typedef struct prompt_ctx {
    fdk_dialog_response resp;
    char value[FI_NAME_MAX];
    bool answered;
} prompt_ctx;

/* the new-folder prompt (the stock prompt dialog, async) */
typedef struct newfolder_ctx {
    pane *pane;
} newfolder_ctx;

static void newfolder_cb(fdk_dialog_response response,
                         const char *text, void *user) {
    newfolder_ctx *nc = user;
    if (response == FDK_DIALOG_OK && text != NULL) {
        fi_new_folder(nc->pane, text);
    }
    free(nc);
}

static char g_prompt_answer[FI_NAME_MAX];

void ops_prompt_new_folder(pane *p) {
    newfolder_ctx *nc = calloc(1, sizeof *nc);
    nc->pane = p;
    (void)fdk_dialog_show_prompt(
        fapp.base.ctx,
        &(fdk_prompt_dialog_options){
            .title = "New folder",
            .text = "Folder name:",
            .value = "New folder",
            .modal = true,
            .parent = fapp.base.window,
        },
        newfolder_cb, nc, NULL);
}

typedef struct rename_ctx {
    pane *pane;
    char old[FI_NAME_MAX];
} rename_ctx;

void ops_prompt_rename(pane *p) {
    const file_info *sel[1];
    if (fmodel_selected(p, sel, 1) != 1) {
        return;
    }
    rename_ctx *rc = calloc(1, sizeof *rc);
    rc->pane = p;
    snprintf(rc->old, sizeof rc->old, "%s", sel[0]->name);
    (void)fdk_dialog_show_prompt(
        fapp.base.ctx,
        &(fdk_prompt_dialog_options){
            .title = "Rename",
            .text = "New name:",
            .value = sel[0]->name,
            .modal = true,
            .parent = fapp.base.window,
        },
        rename_cb, rc, NULL);
}

void rename_cb(fdk_dialog_response response,
              const char *text, void *user) {
    rename_ctx *rc = user;
    snprintf(g_prompt_answer, sizeof g_prompt_answer, "%s",
             text != NULL ? text : "");
    if (response == FDK_DIALOG_OK && g_prompt_answer[0] != '\0') {
        pane *p = rc->pane;
        /* find the row by name (the prompt is not row-bound) */
        tab *t = active_tab(p);
        size_t n = fdk_list_row_count(t->list);
        for (size_t i = 0; i < n; i++) {
            fdk_u64 id = fdk_list_bound_row_id(t->list, i);
            const file_info *fi = fmodel_info_of(id);
            if (fi != NULL && strcmp(fi->name, rc->old) == 0) {
                fi_rename(p, i, g_prompt_answer);
                break;
            }
        }
    }
    free(rc);
}

/* the prompt answer capture: FDK hands the text INSIDE the
 * callback; our prompt wrappers copy it to g_prompt_answer first */
static void dialog_ok_cb(fdk_dialog_response response, void *user);

/* ---- the i18n catalogs ----------------------------------------------- */

static const char *CATALOG_EN = "en";

static void i18n_setup(void) {
    char dir[FI_PATH_MAX];
    /* po/ next to the executable's source tree */
    const char *self = "/proc/self/exe";
    char exe[FI_PATH_MAX];
    ssize_t n = readlink(self, exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
    } else {
        snprintf(exe, sizeof exe, ".");
    }
    (void)dir;
    (void)CATALOG_EN;
    /* the catalogs load from test-apps/fdk-files/po (RO only; EN
     * is the msgid language) */
    (void)ta_i18n_load(&fapp.base, PO_DIR, "ui.language");
}

/* ---- the window-configure relay -------------------------------------- */

static void app_window_event(fdk_window *w, const fdk_event_data *ev,
                             void *user) {
    (void)w;
    (void)user;
    if (ev->type == FDK_EVENT_WINDOW_CONFIGURE) {
        ta_icon_buttons_relayout();
    }
    if (ev->type == FDK_EVENT_WINDOW_CLOSE_REQUEST) {
        fapp.base.quit = true;
    }
}

/* ---- the smoke rig ---------------------------------------------------- */

static void rig_screenshot(int frame) {
    const char *path = getenv("FDK_TESTAPP_SCREENSHOT");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_window_get_surface(fapp.base.window, &s))) {
        if (fdk_ok(fdk_surface_save_png(s, path))) {
            printf("Screenshot: %s (frame %d)\n", path, frame);
            fflush(stdout);
        }
    }
}

/* scripts a few interactions so the smoke run exercises paths */
static void rig_script(void) {
    /* navigate to /tmp (small, always present) */
    pane_navigate(fapp.panes[0], "/tmp");
    /* select the first row if any */
    tab *t = active_tab(fapp.panes[0]);
    if (t->list != NULL && fdk_list_row_count(t->list) > 0) {
        (void)fdk_list_select(t->list, 0);
        printf("PHASE: sel 1 %s\n",
               fdk_list_row_text(t->list, 0));
        fflush(stdout);
    }
    /* a real mkdir + rename + trash round in a scratch dir */
    char scratch[FI_PATH_MAX];
    snprintf(scratch, sizeof scratch, "/tmp/ffs-%d", (int)getpid());
    mkdir(scratch, 0755);
    fi_new_folder(fapp.panes[0], scratch);
    pane_navigate(fapp.panes[0], scratch);
    mkdir("/tmp/ffs-inner", 0755);
    fi_new_folder(fapp.panes[0], "/tmp/fdk-files-smoke-inner");
    /* trash it */
    char target[FI_PATH_MAX];
    snprintf(target, sizeof target, "%.4000s/ffs-inner", scratch);
    char err[256];
    printf("PHASE: trash 1 %d\n",
           trash_file(target, err, sizeof err) ? 1 : 0);
    fflush(stdout);
    /* the undo of it */
    fdk_undo_stack_undo(fapp.undo);
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv) {
    (void)argc;
    bool smoke = (argc > 1 && strcmp(argv[1], "--smoke") == 0);

    if (!ta_init(&fapp.base, "files")) {
        return 1;
    }
    fapp.show_hidden =
        ta_pref_bool(&fapp.base, "view.show-hidden", false);
    (void)ta_pref_int(&fapp.base, "view.mode", VIEW_DETAIL);

    if (!ta_open(&fapp.base, "fdk-files — FDK Files", 1080, 700)) {
        ta_close(&fapp.base);
        return 1;
    }
    fdk_window_set_event_callback(fapp.base.window,
                                  app_window_event, NULL);

    model_init();
    i18n_setup();

    fdk_undo_stack_create(128, &fapp.undo);
    fapp.notify = fdk_notify_open(fapp.base.ctx, NULL);

    /* the tray item (states, not errors, on a bare Xvfb) */
    fdk_tray_item_options topts = {0};
    topts.id = "org.fdk.testapps.files";
    topts.title = "fdk-files";
    fapp.tray = fdk_tray_item_open(fapp.base.ctx, &topts, NULL,
                                   NULL);
    if (fapp.tray != NULL) {
        fdk_tray_item_set_icon_name(fapp.tray, "folder");
        fdk_tray_item_set_tooltip(fapp.tray, "folder", "fdk-files",
                                  "The FDK validation file manager");
    }

    panes_build(&fapp);
    menus_build(&fapp);

    /* the initial navigation */
    printf("PHASE: boot\n");
    fflush(stdout);
    pane_navigate(fapp.panes[0], fi_home());

    if (smoke) {
        rig_script();
    }

    long frame = 0;
    while (ta_pump(&fapp.base)) {
        frame++;
        if (frame == 30) {
            rig_screenshot((int)frame);
        }
        /* the inotify poll rides the loop */
        pane *p = active_pane();
        if (p->watch_fd >= 0 && (frame % 8) == 0) {
            if (fs_watch_poll(p->watch_fd) > 0) {
                pane_reload(p);
            }
        }
    }

    /* persist prefs */
    if (fapp.base.prefs != NULL) {
        (void)fdk_prefs_set_bool(fapp.base.prefs, "view.show-hidden",
                                 fapp.show_hidden);
        (void)fdk_prefs_set_int(
            fapp.base.prefs, "view.mode",
            active_tab(active_pane())->view_mode);
    }

    printf("PHASE: quit\n");
    fflush(stdout);

    menus_ctx_shutdown();
    panes_shutdown();
    td_sweep(); /* the payloads + menu models the registry owns   */
    model_shutdown();
    if (fapp.tray != NULL) {
        fdk_tray_item_close(fapp.tray);
    }
    if (fapp.notify != NULL) {
        fdk_notify_close(fapp.notify);
    }
    if (fapp.undo != NULL) {
        fdk_undo_stack_destroy(fapp.undo);
    }
    ta_close(&fapp.base);
    return 0;
}
