/* fsops.h — the filesystem layer of fdk-files.
 *
 * libc/kernel only: listing, recursive size, the copy/move/delete
 * engines (async-LOOKING via FDK timers — the honest single-thread
 * posture this app reports), the mount table (/proc/self mountinfo + /run/media), and the inotify watch.
 */
#ifndef FILES_FSOPS_H
#define FILES_FSOPS_H

#include "files.h"

#include <dirent.h>
#include <sys/types.h>

/* ---- listing ------------------------------------------------------ */

/* Reads `dir` into a caller-growable array. Skips "." and "..".
 * Returns the entry count (0 on error — check errno for the
 * reason). `hidden` includes dotfiles. The array is the caller's
 * (free with free()); stat()ed lazily per row by the model (see
 * files.h — the parallel-arrays approach the example used is
 * replaced by file_info rows). */
/* round 20: *unreadable distinguishes opendir() failure from a
 * genuinely empty directory — the model layer must not wipe the
 * view for a transient read failure. */
file_info *fs_list_dir(const char *dir, bool hidden, size_t *count,
                       bool *unreadable);

/* stat()s one entry into fi (name/path prefilled by the caller). */
void fs_stat_entry(file_info *fi);

/* Recursive size (du-style, st_size for files). */
off_t fs_tree_size(const char *path);

/* ---- mounts -------------------------------------------------------- */

typedef struct mount_info {
    char dir[FI_PATH_MAX];  /* the mount point                    */
    char dev[128];          /* the source (device/label)          */
    char fstype[32];
    off_t total, free_bytes;
} mount_info;

/* Parses /proc/self/mountinfo (fallback: /proc/mounts) + adds
 * /run/media/USER entries when present. Calls statvfs for usage. */
size_t fs_mounts(mount_info **out);

void fs_mounts_free(mount_info *m);

/* ---- inotify ------------------------------------------------------- */

/* Starts watching `dir` (one watch for the whole app; recursive
 * watching is OUT — recorded honestly). Events arrive via the
 * returned fd; poll it from an fdk_ext_loop_update_fd source or a
 * timer. Returns -1 when inotify is unavailable. */
int fs_watch_start(const char *dir);

/* Reads pending events; returns the number of DIR-CONTENT changes
 * (create/delete/move within the watched dir — the reload
 * triggers), 0 when none, -1 on error (watch dead). */
int fs_watch_poll(int fd);

void fs_watch_stop(int fd);

/* ---- async-shaped operations ---------------------------------------
 *
 * FDK is single-threaded (no thread API — a FINDINGS entry). The
 * "background" copy/move/delete runs in timer-sliced chunks on the
 * main loop with a progress dialog; each pump step does ~250k of
 * payload, so the UI stays live. */

typedef enum fi_op_kind {
    FI_OP_NONE = 0,
    FI_OP_COPY,
    FI_OP_MOVE,
    FI_OP_DELETE,
    FI_OP_TRASH,
} fi_op_kind;

/* A running operation's state (owned by ops_dialogs.c's progress
 * dialog). */
typedef struct fi_op {
    fi_op_kind kind;
    char src[FI_PATH_MAX], dst[FI_PATH_MAX];
    char cur_file[FI_PATH_MAX]; /* display for the dialog          */
    char err[256];
    off_t done_bytes, total_bytes;
    size_t done_files, total_files;
    bool failed;
    bool cancelled;
    void *dialog; /* the progress dialog (ops_dialogs.c)          */
    void *stack;  /* the internal work stack                      */
} fi_op;

/* Starts the sliced engine for `kind` over one (src,dst) pair
 * (COPY/MOVE) or one tree (DELETE). TRASH is one synchronous
 * rename per file (fast; no engine). */
fi_op *fi_op_start(fi_op_kind kind, const char *src, const char *dst);

/* One slice; returns false when the operation FINISHED (check
 * op->failed/op->err). Called from the progress dialog's timer. */
bool fi_op_step(fi_op *op);

void fi_op_free(fi_op *op);

/* The conflict question: does dst exist? (also returns the
 * destination's is_dir through *dst_dir) */
bool fi_conflict(const char *dst, bool *dst_dir);

/* A unique "name (n)" sibling at the destination. */
void fi_unique_sibling(const char *dst, char *out, size_t cap);

#endif /* FILES_FSOPS_H */
