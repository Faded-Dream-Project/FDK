/*
 * fdk_thread.h — Faded Dream ToolKit: cross-thread posting + workers
 *
 * The G-2 completion of the threading model (docs/threading.md's
 * "Getting work back onto the UI thread" placeholder, finally
 * real). Two problems, two primitives:
 *
 *   fdk_post      any thread -> a callback on the UI thread
 *   fdk_worker    a job pool that runs work off-thread and delivers
 *                 progress + result on the UI thread
 *
 * THE THREADING RULE (unchanged, now enforceable):
 *
 *   Every fdk_* object (context, windows, widgets, models, timers,
 *   catalogs...) belongs to the thread that created it — the UI
 *   thread. The ONLY functions legal from ANY thread are the ones
 *   in this header:
 *
 *     fdk_post / fdk_post_free
 *     fdk_worker_pool_create / fdk_worker_submit / fdk_job_cancel /
 *     fdk_worker_pool_destroy / fdk_job_status
 *     fdk_log and the log sinks (fdk_log.h's own rule)
 *     fdk_version / fdk_result_to_string (pure)
 *
 *   With FDK_DEBUG_THREAD_CHECKS defined (the debug build's default,
 *   see docs/build.md), a main-thread-only call made from another
 *   thread is REFUSED with FDK_ERR_WRONG_THREAD and logs one line —
 *   the crash you would have raced toward becomes a returned error.
 *   Release builds keep the old behavior (undefined if you race);
 *   the refusal is a debugging aid, not a lock.
 *
 * Why an eventfd/pipe wake: fdk_pump_events() sleeps in poll() on
 * the backend fd. A post from another thread must interrupt that
 * sleep PROMPTLY (a worker finishing must repaint within the frame,
 * not whenever the next X event happens). The wake fd is polled as
 * part of the ordinary pump set — one poll() call, one ordering
 * story, both backends (Wayland's wl_display fd and X11's XConnectionNumber
 * sit in the same set; the wake fd is backend-independent).
 *
 * Ordering: per-POSTING-THREAD FIFO. Threads interleave freely (the
 * queue is a mutex-guarded linked list), but one thread's posts run
 * in the order it made them. Posts made FROM the UI thread are also
 * queued (not run inline): a post's callback always runs from the
 * pump, never re-entrantly from inside fdk_post.
 *
 * Reentrancy: a callback may post, submit jobs, create/destroy
 * widgets, quit the loop. It runs with no lock held.
 *
 * Shutdown: fdk_shutdown() drops every queued post (without running
 * it) and closes the wake fd. Posting to a shut-down context is
 * FDK_ERR_NOT_INITIALIZED, safe to call from a dying worker.
 */

#ifndef FDK_THREAD_H
#define FDK_THREAD_H

#include "fdk_error.h"
#include "fdk_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The main-thread-only violation, refused under
 * FDK_DEBUG_THREAD_CHECKS. Distinct from every other error so a
 * worker's error handling can route it to "programming error on my
 * side", not "the platform failed". */
/* (part of the fdk_result family — see fdk_error.h; the value is
 * allocated there as FDK_ERR_WRONG_THREAD) */

/* ---- Posting: any thread -> UI thread -------------------------------- */

/* Runs `fn(user)` on the UI thread during a subsequent
 * fdk_pump_events()/fdk_run() batch. Safe from ANY thread,
 * including the UI thread itself (queued, never inline).
 *
 * `fn` runs with no locks held and may call any FDK function (it is
 * ON the UI thread). `user` is never dereferenced by FDK.
 *
 * Ordering: per-posting-thread FIFO.
 *
 * Delivery: exactly once if the context outlives the pump step that
 * drains it; DROPPED (never run) when fdk_shutdown() happens first.
 * There is no "post failed to deliver" signal — if you need one,
 * own it in the callback's first line.
 *
 * Can fail with FDK_ERR_INVALID_ARGUMENT (NULL ctx or fn),
 * FDK_ERR_NOT_INITIALIZED (ctx already shut down),
 * FDK_ERR_OUT_OF_MEMORY. */
fdk_result fdk_post(fdk_context *ctx, void (*fn)(void *user),
                    void *user);

/* ---- The worker facility ---------------------------------------------
 *
 * A pool of OS threads owned by the context's UI thread, plus the
 * delivery plumbing. The SHAPE a real application needs:
 *
 *   UI thread                      worker thread
 *   ----------                     -------------
 *   fdk_worker_submit(pool,spec) -> job.job(user, report, report_user)
 *       returns a job handle          |
 *   <........ progress(frac) .........|  (each report() lands as a
 *       (a fdk_post)                  |   UI-thread callback)
 *   <........ done(result) ...........|  (once; result_user flows
 *                                      |   from job to done)
 *
 *   fdk_job_cancel(job)  -> sets a flag; report()/done() delivery
 *                           stops; the job function should poll
 *                           fdk_job_cancelled(job) cooperatively
 *                           (cancel is prompt-delivery-cancel, not
 *                           thread-kill — a job that never checks
 *                           still runs to completion, it just goes
 *                           silent).
 *
 * Everything the job touches (its buffers, its state) belongs to the
 * JOB while it runs; the UI thread must not touch it between submit
 * and done/cancel-confirmed. `result_user` crosses the boundary
 * exactly once.
 */

typedef struct fdk_worker fdk_worker;
typedef struct fdk_job fdk_job;

/* The job function. Returns the RESULT pointer handed to done()
 * (NULL when the job has nothing to say). `report` is never NULL —
 * a spec without a progress callback gets the pool's no-op, so the
 * job may call it unconditionally. */
typedef void *(*fdk_worker_job_fn)(void *user_data,
                                   void (*report)(double fraction,
                                                  void *report_user),
                                   void *report_user);

/* Progress delivery (UI thread, from the pump). */
typedef void (*fdk_worker_progress_fn)(double fraction, void *user);

/* Completion (UI thread). `result` is the job function's return
 * value; `user` is the spec's done_user. */
typedef void (*fdk_worker_done_fn)(void *result, void *user);

typedef struct fdk_job_spec {
    fdk_worker_job_fn job;        /* REQUIRED: the off-thread work  */
    fdk_worker_progress_fn progress; /* NULL = no progress reports  */
    fdk_worker_done_fn done;      /* NULL = fire-and-forget        */
    void *user_data;              /* flows into job()              */
    void *done_user;              /* flows into done()             */
} fdk_job_spec;

/* Creates a pool with `threads` worker threads (clamped to
 * [1, 16]). The pool — and every job still running in it — is torn
 * down by fdk_worker_pool_destroy. Fails with
 * FDK_ERR_INVALID_ARGUMENT / FDK_ERR_OUT_OF_MEMORY /
 * FDK_ERR_NOT_INITIALIZED (ctx mid-shutdown). */
fdk_result fdk_worker_pool_create(fdk_context *ctx, int threads,
                                  fdk_worker **out_pool);

/* Submits a job. `out_job` may be NULL (no handle: fire-and-forget
 * with no cancel). The handle is valid until done() fires or the
 * pool is destroyed. */
fdk_result fdk_worker_submit(fdk_worker *pool,
                             const fdk_job_spec *spec,
                             fdk_job **out_job);

/* Cancel: stops progress/done DELIVERY promptly. The job function
 * should poll fdk_job_cancelled() to stop its actual work. Returns
 * FDK_ERR_INVALID_ARGUMENT on NULL. */
fdk_result fdk_job_cancel(fdk_job *job);

/* True when the job is cancelled (safe to call from the job
 * function itself; false on NULL). */
bool fdk_job_cancelled(const fdk_job *job);

/* Destroys the pool: cancels every job, JOINS every thread, drops
 * every undelivered progress/done post. May block until in-flight
 * job functions return (a job that ignores cancellation delays this
 * by its own runtime — the honest cost of cooperative cancel). */
void fdk_worker_pool_destroy(fdk_worker *pool);

/* The job's status: 0 queued, 1 running, 2 done-delivered,
 * 3 cancelled. (Diagnostics; races with reality by design.) */
int fdk_job_status(const fdk_job *job);

/* ---- The wake fd (internal plumbing, exposed for the pump) ---------- */

/* The fd poll() watches for posts (eventfd on Linux, a pipe
 * elsewhere). -1 before fdk_init. INTERNAL — not for applications. */
int fdk__thread_wake_fd(const fdk_context *ctx);

/* Drains queued posts (the pump's readable-path). INTERNAL. Returns
 * the number of callbacks run. */
int fdk__thread_drain(fdk_context *ctx);

/* Init/teardown hooks the context calls. INTERNAL. */
fdk_result fdk__thread_init(fdk_context *ctx);
void fdk__thread_shutdown(fdk_context *ctx);

/* The main-thread check (compiled to a constant-true under release
 * builds). INTERNAL — the wrong-thread refusal lives here. */
bool fdk__on_main_thread(const fdk_context *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FDK_THREAD_H */
