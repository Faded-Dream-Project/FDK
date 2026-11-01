/* ops.h — the worker-backed operation engine of fdk-files (§25/4b).
 *
 * Everything §24 timer-sliced on the MAIN loop (copy/move/delete,
 * recursive sizes, the search walk, thumbnail generation) runs on
 * the G-2 worker facility now: fdk_worker threads do the work,
 * progress + completion arrive on the UI thread through fdk_post,
 * cancel is cooperative, and the UI stays responsive by
 * construction instead of by slicing.
 *
 * One engine, one job at a time (a file manager's operations are
 * user-serialized in practice; queuing a second while the first
 * runs would race conflict-resolution). The PROGRESS dialog in
 * ops_dialogs.c drives nothing — it OBSERVES.
 */
#ifndef FILES_OPS_H
#define FILES_OPS_H

#include "files.h"
#include "fsops.h"

#include "fdk/fdk_thread.h"

/* Starts the engine (one pool, 2 threads). Call after ta_open. */
void ops_init(void);

/* Stops the engine: cancels + joins (fdk_worker_pool_destroy).
 * Call before the window dies at app teardown. */
void ops_shutdown(void);

/* True while an engine operation runs (the toolbar's paste/delete
 * guard). */
bool ops_busy(void);

/* A worker-side progress report (fraction 0..1 + the current file
 * for the dialog). Delivered on the UI thread. */
typedef void (*ops_progress_fn)(double fraction,
                                const char *current_file,
                                void *user);
typedef void (*ops_done_fn)(bool ok, const char *err, void *user);

/* The engine's single in-flight operation. */
typedef struct ops_job {
    fi_op_kind kind;
    char src[FI_PATH_MAX];
    char dst[FI_PATH_MAX]; /* DELETE/TRASH: unused                */
    ops_progress_fn on_progress;
    ops_done_fn on_done;
    void *user;
    /* SIZE jobs carry their own done shape (the walked total). */
    void (*size_done)(bool ok, off_t size, void *user);
    void *size_user;

    /* engine-private */
    fdk_job *job;
    volatile bool cancel;
} ops_job;

/* Copy or move src -> dst (recursive). Returns false when the
 * engine is busy (the caller surfaces that). */
bool ops_start_copy(const char *src, const char *dst, bool move,
                    ops_progress_fn progress, ops_done_fn done,
                    void *user);

/* Recursive delete. */
bool ops_start_delete(const char *path, ops_progress_fn progress,
                      ops_done_fn done, void *user);

/* Recursive size (the properties dialog). The done callback gets
 * the size through `user`'s own state; err stays NULL on success. */
bool ops_start_size(const char *path, ops_progress_fn progress,
                    void (*done)(bool ok, off_t size, void *user),
                    void *user);

/* Cancels the in-flight operation (cooperative: the worker checks
 * between payload slices). */
void ops_cancel(void);

#endif /* FILES_OPS_H */
