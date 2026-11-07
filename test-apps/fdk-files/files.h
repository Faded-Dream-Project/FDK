/* files.h — fdk-files: the shared state.
 *
 * A real file manager written only against FDK's public API, to
 * find what FDK is missing before 1.0.0. Modules:
 *
 *   files.h / files.c      the app state, wiring, main()
 *   fsops.[ch]             directory listing, copy/move/delete,
 *                          recursive size, mount table, inotify
 *   mime.[ch]              shared-mime-info globs+magic lookup,
 *                          icon name per type, category
 *   trash.[ch]             the freedesktop trash spec (.trashinfo,
 *                          restore, empty; sources in README)
 *   model.[ch]             the listing MODEL + sort/filter proxies
 *   panes.[ch]             panes/tabs/breadcrumb/sidebar UI
 *   ops_dialogs.[ch]       progress, conflict, properties,
 *                          permissions, app-chooser
 *   menus.[ch]             menu bar, context menus, shortcuts
 *
 * Design notes (what pushes FDK):
 *  - the listing is a fdk_model + sort proxy + filter proxy, bound
 *    to a fdk_list per pane (virtualized rows at 50k entries);
 *  - file operations are ASYNC-LOOKING via fdk timers (chatty
 *    progress) but synchronous inside one pump step — FDK has no
 *    thread API, which is one of the things this app reports;
 *  - clipboard/DnD use the real text/uri-list surfaces;
 *  - undo/redo covers move/rename/trash through fdk_undo.
 */
#ifndef FILES_APP_H
#define FILES_APP_H

#include "../common/app.h"
#include "../common/icons.h"

#include "fdk/fdk_clipboard.h"
#include "fdk/fdk_dnd.h"
#include "fdk/fdk_model.h"
#include "fdk/fdk_notify.h"
#include "fdk/fdk_tray.h"
#include "fdk/fdk_undo.h"

#include <stdio.h>
#include <sys/types.h>
#include <time.h>

/* forward decls (the pane/tab types live in panes.h, which needs
 * this header first — the classic two-file cycle) */
struct pane;
struct tab;

#define FI_NAME_MAX 256
#define FI_PATH_MAX 4096
/* buffers that HOLD a path-plus-component join (path + "/" + name +
 * a suffix like ".trashinfo"): one path plus one name plus slack,
 * so the join can never truncate */
#define FI_JOIN_MAX (FI_PATH_MAX + FI_NAME_MAX + 64)

/* ---- one directory entry, the model's row payload --------------- */

typedef struct file_info {
    char name[FI_NAME_MAX];   /* display name (the basename)      */
    char path[FI_PATH_MAX];   /* absolute                          */
    char mime[96];            /* "text/plain" or "inode/directory"*/
    char icon[48];            /* icon name for the theme           */
    off_t size;               /* st_size                           */
    time_t mtime;             /* st_mtime                          */
    uid_t uid;
    gid_t gid;
    mode_t mode;
    bool is_dir;
    bool is_link;
    bool is_hidden;
    bool is_executable;
    bool thumb_pending;       /* thumbnail requested, not ready    */
} file_info;

/* The app singleton. */
typedef struct files_app {
    ta_app base;              /* must be first (the common shell)  */

    /* panes (one in single view, two in split) */
    struct pane *panes[2];
    int pane_count;           /* 1 or 2                             */
    int active_pane;          /* index of the focused pane         */

    /* global features */
    fdk_widget *sidebar;
    fdk_widget *toolbar;
    fdk_undo_stack *undo;
    fdk_notify *notify;
    fdk_tray_item *tray;
    char clipboard_op;        /* 'c' copy, 'm' cut, 0 none        */
    char clipboard_dir[FI_PATH_MAX];
    char clipboard_names[64][FI_NAME_MAX];
    size_t clipboard_count;

    /* the clipboard-hold indicator & hidden toggle are global      */
    bool show_hidden;

    /* the recursive search's live state (owned by the active pane) */
    bool search_active;

    /* scripted-input hook for the smoke rig */
    char script_path[FI_PATH_MAX];

    /* perf markers printed for the README */
    bool perf_log;
} files_app;

extern files_app fapp;

/* A truncation-safe path join: snprintf("%s/%s") into a fixed
 * buffer makes GCC's -Wformat-truncation fire on every join of two
 * PATH_MAX sources; this wrapper states the bound once. */
static inline size_t fi_join(char *out, size_t cap, const char *a,
                             const char *b) {
    size_t n = (size_t)snprintf(out, cap, "%s/%s", a, b);
    return (n < cap) ? n : cap - 1;
}
/* bounded multi-part joins (the fixed-size-buffer discipline the
 * -Wformat-truncation pass demanded: bounds stated once, here) */
static inline void fi_join2(char *out, size_t cap, const char *a,
                            const char *b, const char *c) {
    snprintf(out, cap, "%.2000s/%.100s/%.200s", a, b, c);
}
static inline void fi_join3(char *out, size_t cap, const char *a,
                            const char *b, const char *c,
                            const char *d) {
    snprintf(out, cap, "%.2000s/%.100s/%.200s%.12s", a, b, c, d);
}
static inline size_t fi_copy(char *out, size_t cap, const char *a) {
    size_t n = (size_t)snprintf(out, cap, "%s", a);
    return (n < cap) ? n : cap - 1;
}

/* helpers shared across modules */
const char *fi_home(void);
void fi_fmt_size(off_t bytes, char *buf, size_t cap);
void fi_fmt_mtime(time_t t, char *buf, size_t cap);
void fi_set_status(const char *fmt, ...);

/* pane operations (panes.c) */
void pane_reload(struct pane *p);
#define FI_USES_PANES 1
void pane_navigate(struct pane *p, const char *abs_path);
void pane_open_selected(struct pane *p);
struct pane *active_pane(void);

/* operations (fsops.c) — each is one pump-step chunk; long ones
 * report progress through the dialog in ops_dialogs.c */
typedef void (*fi_op_done)(bool ok, const char *detail, void *user);

void fi_begin_copy(const char *src_dir, const char *const *names,
                   size_t count, const char *dst_dir);
void fi_begin_move(const char *src_dir, const char *const *names,
                   size_t count, const char *dst_dir);
void fi_trash_selection(struct pane *p);
void fi_delete_selection(struct pane *p, bool to_trash);
void fi_new_folder(struct pane *p, const char *name);
void fi_rename(struct pane *p, size_t row, const char *new_name);

/* model.c */

/* main.c's ops surfaces shared with menus/panes */
void ops_clipboard_set(char op);
void ops_clipboard_paste(struct pane *p);
void ops_clipboard_real(void);
void ops_open_prefs(void);
void ops_open_about(void);
void ops_focus_search(void);
void ops_prompt_new_folder(struct pane *p);
void ops_prompt_rename(struct pane *p);
const char *ta_pref_str_or(const char *key);

/* the undo entry (main.c) */
typedef struct fi_undo_entry {
    char from[FI_PATH_MAX];
    char to[FI_PATH_MAX];
    bool restore;
    char trash_name[FI_NAME_MAX];
} fi_undo_entry;

/* menus.c */
void menus_build(files_app *a);
void menus_popup_context(struct pane *p, int for_what, fdk_i32 x,
                         fdk_i32 y);
/* context menu flavors */
enum {
    CTX_FILE = 0,
    CTX_FOLDER,
    CTX_MULTI,
    CTX_EMPTY,
    CTX_SIDEBAR_PLACE,
};

#endif /* FILES_APP_H */
