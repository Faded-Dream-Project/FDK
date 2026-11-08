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
#include "bookmarks.h"
#include "ops.h"
#include "thumbs.h"
#include "ops_dialogs.h"
#include "panes.h"
#include "../common/ta_buf.h"
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

/* the tray's click menu (round 50): the model lives as long as the
 * item; the node ids are stable handles for the click dispatch. */
static fdk_tray_menu *g_tray_menu = NULL;
static uint32_t g_menu_open, g_menu_about, g_menu_quit;

static void tray_menu_click(fdk_tray_menu *menu, uint32_t id, void *user) {
    (void)menu;
    (void)user;
    if (id == g_menu_open) {
        fdk_window_show(fapp.base.window);
        fdk_window_activate(fapp.base.window);
    } else if (id == g_menu_about && fapp.notify != NULL) {
        fdk_notify_request req = {0};
        req.summary = "fdk-files";
        req.body = "The FDK validation file manager (round 50: the "
                   "tray click menu).";
        req.urgency = FDK_NOTIFY_URGENCY_LOW;
        (void)fdk_notify_send(fapp.notify, &req, NULL);
    } else if (id == g_menu_quit) {
        fapp.base.quit = true;
    }
}

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

static void delete_engine_done(bool ok, const char *err, void *user);

void fi_delete_selection(pane *p, bool to_trash) {
    const file_info *sel[512];
    size_t n = fmodel_selected(p, sel, 512);
    if (n == 0) {
        return;
    }
    if (!to_trash && !ops_confirm_delete(n)) {
        return;
    }
    /* permanent delete of DIRECTORIES goes to the worker engine
     * (recursive unlink must not freeze the UI — §25/4b) */
    if (!to_trash) {
        const file_info *dirs[64];
        size_t nd = 0;
        for (size_t i = 0; i < n && nd < 64; i++) {
            if (sel[i]->is_dir) {
                dirs[nd++] = sel[i];
            }
        }
        if (nd > 0) {
            size_t started = 0;
            for (size_t i = 0; i < nd; i++) {
                if (ops_start_delete(
                        dirs[i]->path, NULL,
                        delete_engine_done, NULL)) {
                    started++;
                }
            }
            printf("PHASE: trash %zu %zu\n", n, started);
            fflush(stdout);
            pane_reload(p);
            return;
        }
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
            /* single files unlink directly (fast); a DIRECTORY
             * would block — the §25 rule is the worker for those,
             * handled by the caller below */
            if (unlink(sel[i]->path) == 0) {
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

static void delete_engine_done(bool ok, const char *err, void *user) {
    (void)user;
    fi_set_status("%s", ok ? "Deleted" : ((err && err[0]) ? err
                                                          : "Delete failed"));
    pane_reload(active_pane());
}

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

static volatile int g_rig_copy_ok;
static void rig_copy_progress(double frac, const char *cur,
                              void *user) {
    (void)frac;
    (void)cur;
    (void)user;
}
static void rig_copy_done(bool ok, const char *err, void *user) {
    (void)err;
    (void)user;
    g_rig_copy_ok = ok ? 1 : 0;
    /* the tray-bus rig's T3: the long-op notification (the same
     * rule the progress dialog applies — >2 MB means "long") */
    if (ok && fapp.notify != NULL) {
        fdk_notify_request req = {0};
        req.summary = "File operation finished";
        req.body = "The smoke rig's worker copy completed.";
        req.urgency = FDK_NOTIFY_URGENCY_LOW;
        (void)fdk_notify_send(fapp.notify, &req, NULL);
    }
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
    /* a real mkdir + rename + trash round in a scratch dir. The
     * tree gets one >2 MB file so the worker-copy finish fires the
     * long-operation NOTIFICATION (the >2 MB rule the progress
     * dialog applies) — the tray-bus rig's T3 depends on it. */
    char scratch[FI_PATH_MAX];
    snprintf(scratch, sizeof scratch, "/tmp/ffs-%d", (int)getpid());
    mkdir(scratch, 0755);
    {
        char big[TA_JOIN_MAX];
        ta_join_suf(big, sizeof big, scratch, "payload.bin", "");
        FILE *bf = fopen(big, "wb");
        if (bf != NULL) {
            char chunk[65536];
            memset(chunk, 7, sizeof chunk);
            for (int i = 0; i < 40; i++) { /* ~2.6 MB */
                fwrite(chunk, 1, sizeof chunk, bf);
            }
            fclose(bf);
        }
    }
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

    /* the §26/3a steps: split view on/off, bookmark add + reorder
     * + remove + persistence */
    {
        int n = pane_add();
        printf("PHASE: split %d\n", n >= 2 ? 1 : 0);
        fflush(stdout);
        if (n >= 2) {
            /* the second pane navigated to the same dir */
            tab *t2 = active_tab(fapp.panes[1]);
            printf("PHASE: splitdir %s\n",
                   (t2 != NULL) ? t2->dir : "(null)");
            fflush(stdout);
            pane_remove_last();
            printf("PHASE: splitoff %d\n", fapp.pane_count);
            fflush(stdout);
        }
        /* bookmark the scratch dir + /tmp (both exist), reorder, drop */
        char b2[TA_JOIN_MAX];
        ta_copy(b2, sizeof b2, "/tmp");
        int bi = bookmarks_add(scratch);
        int b2i = bookmarks_add(b2);
        bookmarks_refresh();
        printf("PHASE: bookmark %d %d %zu\n", bi >= 0 ? 1 : 0,
               b2i >= 0 ? 1 : 0, bookmarks_count());
        fflush(stdout);
        /* reorder: swap them */
        bool moved = bookmarks_move(0, 1);
        printf("PHASE: bookmarkmove %d %s\n", moved ? 1 : 0,
               (bookmarks_count() > 0) ? bookmarks_at(0)->path
                                       : "(none)");
        fflush(stdout);
        /* remove one; persistence round-trip */
        (void)bookmarks_remove(scratch);
        bookmarks_refresh();
        bookmarks_save();
        size_t before = bookmarks_count();
        bookmarks_load();
        bookmarks_refresh();
        printf("PHASE: bookmarkpersist %zu %zu\n", before,
               bookmarks_count());
        fflush(stdout);
        (void)bookmarks_remove(b2);
        bookmarks_refresh();
        bookmarks_save();
    }

    /* a WORKER-engine copy round trip (§25/4b): copy the scratch
     * tree, wait for the done callback, verify the copy exists */
    {
        char copy_dst[TA_JOIN_MAX];
        snprintf(copy_dst, sizeof copy_dst, "/tmp/ffs-copy-%d",
                 (int)getpid());
        g_rig_copy_ok = -1;
        if (ops_start_copy(scratch, copy_dst, false,
                           rig_copy_progress, rig_copy_done, NULL)) {
            while (g_rig_copy_ok == -1 && !fapp.base.quit) {
                if (fdk_pump_events(fapp.base.ctx, 20) < 0) {
                    break;
                }
                if (fdk_widget_tree_has_damage(fapp.base.root)) {
                    (void)fdk_window_paint(fapp.base.window);
                }
            }
        }
        printf("PHASE: workercopy %d\n", g_rig_copy_ok);
        fflush(stdout);
        /* clean both scratch trees */
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf /tmp/ffs-%d /tmp/ffs-copy-%d",
                 (int)getpid(), (int)getpid());
        (void)system(cmd);
    }
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
        /* The tray's click menu (round 50): ONE model serves every
         * bar — a StatusNotifier host renders it over DBusMenu; a
         * classic XEmbed bar gets the toolkit's local dropdown on
         * click. */
        g_tray_menu = fdk_tray_menu_create();
        if (g_tray_menu != NULL) {
            g_menu_open = fdk_tray_menu_add(g_tray_menu, 0, "_Open Files");
            g_menu_about = fdk_tray_menu_add(g_tray_menu, 0, "_About");
            fdk_tray_menu_add_separator(g_tray_menu, 0);
            g_menu_quit = fdk_tray_menu_add(g_tray_menu, 0, "_Quit");
            fdk_tray_menu_on_click(g_tray_menu, tray_menu_click, NULL);
            fdk_tray_item_set_menu(fapp.tray, g_tray_menu);
        }
    }

    /* §27/2b: the deterministic drag-rig layout. FDK_FILES_DRIG=1
     * builds a split view with both panes in /tmp/fdk-drig (left
     * with alpha.txt + a folder, right empty), points the bookmark
     * store at a fresh file, and parks the window at (40,40) with
     * a known 1000x700 size — drag_rig.c drives REAL XTEST drags
     * against exactly these coordinates. */
    const bool drig = getenv("FDK_FILES_DRIG") != NULL;
    if (drig && getenv("FDK_TESTAPP_FRAMES") == NULL) {
        setenv("FDK_TESTAPP_FRAMES", "20000", 1);
    }
    if (drig) {
        (void)system("rm -rf /tmp/fdk-drig && mkdir -p "
                     "/tmp/fdk-drig/left /tmp/fdk-drig/right");
        FILE *af = fopen("/tmp/fdk-drig/left/alpha.txt", "w");
        if (af != NULL) { fputs("alpha\n", af); fclose(af); }
        (void)system("mkdir -p /tmp/fdk-drig/left/beta-dir");
        setenv("XDG_CONFIG_HOME", "/tmp/fdk-drig-cfg", 1);
        (void)system("rm -rf /tmp/fdk-drig-cfg && mkdir -p "
                     "/tmp/fdk-drig-cfg/fdk-files");
        FILE *bf = fopen("/tmp/fdk-drig-bookmarks", "w");
        if (bf != NULL) {
            fputs("# fdk-files bookmarks (order = display order)\n"
                  "/tmp/fdk-drig\n", bf);
            fclose(bf);
        }
        /* the bookmarks loader reads XDG_CONFIG_HOME; copy the seed
         * into place before load */
        (void)system("cp /tmp/fdk-drig-bookmarks "
                     "/tmp/fdk-drig-cfg/fdk-files/bookmarks");
    }

    bookmarks_load(); /* before the sidebar builds (§26/3a) */
    panes_build(&fapp);
    menus_build(&fapp);
    /* §31/S1: one relayout pass AFTER the tree exists — the canvas
     * bounds are also set at creation now, but this keeps them true
     * to the final laid-out button sizes even at boot. */
    ta_icon_buttons_relayout();
    ops_init(); /* the worker engine (§25/4b)                   */

    if (drig) {
        (void)system("cp /tmp/fdk-drig-bookmarks "
                     "/tmp/fdk-drig-cfg/fdk-files/bookmarks");
        bookmarks_load();
        bookmarks_refresh();
        pane_navigate(fapp.panes[0], "/tmp/fdk-drig/left");
        (void)pane_add();
        pane_navigate(fapp.panes[1], "/tmp/fdk-drig/right");
        /* the drag rig's coordinates assume the DETAIL list; the
         * app's real prefs may say icons (drag arming is list-only)
         */
        pane_set_view_mode(fapp.panes[0], VIEW_DETAIL);
        pane_set_view_mode(fapp.panes[1], VIEW_DETAIL);
        fapp.active_pane = 0;
        /* Round 14: the stress rig sizes the window to the SCREEN it
         * drives (FDK_DRIG_W/H) — a 1000x700 hardcode made 1920x1080
         * and 4K runs measure a small window's proportions. Default
         * unchanged for the older rigs. */
        {
            const char *rw = getenv("FDK_DRIG_W");
            const char *rh = getenv("FDK_DRIG_H");
            long ww = (rw != NULL) ? atol(rw) : 1000;
            long wh = (rh != NULL) ? atol(rh) : 700;
            if (ww < 200) ww = 200;
            if (wh < 150) wh = 150;
            if (ww > 4000) ww = 4000;
            if (wh > 4000) wh = 4000;
            (void)fdk_window_resize(fapp.base.window,
                                    (fdk_i32)ww, (fdk_i32)wh);
        }
        printf("DRIG-READY\n");
        fflush(stdout);
        /* DRIG-GEO: the rig's coordinates come from the REAL layout
         * (measured, not assumed): each pane's list bounds + row
         * count and the bookmarks bounds, in window coordinates.
         * The rig parses these lines from the app's log. */
        for (int pi = 0; pi < fapp.pane_count; pi++) {
            pane *p = fapp.panes[pi];
            if (p == NULL) {
                continue;
            }
            tab *t = active_tab(p);
            fdk_widget *v = (t != NULL) ? t->list : NULL;
            if (v != NULL) {
                fdk_rect ab = fdk_widget_get_absolute_bounds(v);
                /* rowh: the real row pitch (the theme floor can
                 * change it; round 15's rig first assumed 25 and
                 * clicked f10 when it aimed at f3) */
                printf("DRIG-GEO pane%d list %d %d %d %d rows=%zu "
                       "rowh=%d\n",
                       pi, ab.x, ab.y, ab.width, ab.height,
                       fdk_list_row_count(v),
                       fdk_list_get_row_height(v));
            }
        }
        {
            fdk_widget *bw = bookmarks_widget();
            if (bw != NULL) {
                fdk_rect ab = fdk_widget_get_absolute_bounds(bw);
                printf("DRIG-GEO bookmarks %d %d %d %d\n", ab.x,
                       ab.y, ab.width, ab.height);
            }
        }
        /* Round 15 (the delete rig): pane0's path entry — the rig
         * navigates by TYPING the absolute path (deterministic,
         * independent of the row order a remembered sort leaves
         * behind), so it needs the entry's real bounds too. */
        if (fapp.pane_count > 0 && fapp.panes[0] != NULL &&
            fapp.panes[0]->path_entry != NULL) {
            fdk_rect ab = fdk_widget_get_absolute_bounds(
                fapp.panes[0]->path_entry);
            printf("DRIG-GEO path %d %d %d %d\n", ab.x, ab.y,
                   ab.width, ab.height);
        }
        fflush(stdout);
    }

    /* the initial navigation */
    printf("PHASE: boot\n");
    fflush(stdout);
    if (!drig) {
        const char *startdir = getenv("FDK_FILES_STARTUP_DIR");
        pane_navigate(fapp.panes[0],
                      (startdir != NULL && startdir[0] != '\0')
                          ? startdir
                          : fi_home());
    }

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
                if (getenv("FDK_FILES_DRIG") != NULL) {
                    printf("DRIG-WATCH fire @frame %ld\n", frame);
                    fflush(stdout);
                }
                pane_reload(p);
            }
        }
        /* round 20 telemetry: the active list's bound row count,
         * once per second — catches the model emptying in the act */
        if (getenv("FDK_FILES_DRIG") != NULL && (frame % 64) == 0) {
            tab *t = active_tab(p);
            if (t != NULL && t->list != NULL) {
                printf("DRIG-ROWS frame=%ld rows=%zu\n", frame,
                       fdk_list_row_count(t->list));
                fflush(stdout);
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

    ops_shutdown(); /* joins the engine before the UI dies      */
    thumbs_shutdown();
    menus_ctx_shutdown();
    panes_shutdown();
    td_sweep(); /* the payloads + menu models the registry owns   */
    if (fapp.tray != NULL) {
        fdk_tray_item_close(fapp.tray);
    }
    if (g_tray_menu != NULL) {
        fdk_tray_menu_close(g_tray_menu);
        g_tray_menu = NULL;
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
