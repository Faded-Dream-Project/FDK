/* ops_dialogs.h — the operation dialogs of fdk-files: progress
 * with cancel, conflict resolution, properties (recursive size +
 * permissions), the app-chooser (open-with), and the trash window.
 */
#ifndef FILES_OPS_DIALOGS_H
#define FILES_OPS_DIALOGS_H

#include "files.h"
#include "fsops.h"

/* Opens the selection with the .desktop association chain (reads
 * XDG data dirs' applications .desktop files, MimeType= matching; the
 * app-chooser dialog when several/none match). */
void ops_open_with(struct pane *p, const file_info *fi);

/* The app-chooser dialog itself (list of matching apps + a plain
 * "run a custom command" entry). */
void ops_choose_app(struct pane *p, const file_info *fi);

/* Starts a copy/move of `paths' into `dst_dir` with the progress
 * dialog (conflicts resolved per the dialog's answer). */
void ops_begin_paste_into(struct pane *p, const char *const *paths,
                          size_t count, const char *dst_dir,
                          fi_op_kind kind);

/* The properties dialog of one entry (size recursive, permissions
 * octal + checkboxes, owner, times; chmod on apply). */
void ops_properties(struct pane *p, const file_info *fi);

/* The trash window (list, restore, delete, empty). */
void ops_open_trash(void);

/* The conflict dialog's question: replace / skip / rename-both /
 * abort — for MULTI-file pastes also "apply to all". */
typedef enum conflict_answer {
    CONFLICT_ABORT = 0,
    CONFLICT_SKIP,
    CONFLICT_REPLACE,
    CONFLICT_RENAME,
} conflict_answer;

conflict_answer ops_ask_conflict(const char *dst, bool multiple,
                                 bool *apply_all);

/* The confirm dialog for permanent delete. */
bool ops_confirm_delete(size_t count);

#endif /* FILES_OPS_DIALOGS_H */
