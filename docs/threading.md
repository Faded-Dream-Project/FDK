# FDK Threading Model

## The rule

**All FDK objects (`fdk_context` and everything reachable from it —
windows, widgets, timers, once they exist) must only be accessed from
the thread that called `fdk_init()`.** Conventionally this is the
application's main thread; FDK refers to it as the "UI thread."

This is the same model GTK, Qt, and most native GUI toolkits use, for
the same reasons: window systems (X11, Wayland) are themselves
single-connection/single-thread-affine at the protocol level in
practice, and a lock-everything approach would cripple both
performance and API ergonomics for no real benefit to typical GUI
applications.

## Worker threads

Applications may freely use additional threads for background work
(networking, computation, file I/O) — FDK does not restrict what an
application does off the UI thread. What a worker thread must **not**
do is call any `fdk_*` function on an object owned by the UI thread's
context.

## Getting work back onto the UI thread

**Implemented** (§25/G-2, `include/fdk/fdk_thread.h`):

```c
/* Any thread -> the UI thread, on the next pump step. */
fdk_result fdk_post(fdk_context *ctx, void (*fn)(void *user),
                    void *user);
```

A worker thread calls this to hand a callback + data pointer to FDK;
FDK queues it (a mutex-guarded list) and wakes the pump's `poll()`
through a dedicated eventfd (a pipe as fallback — backend-independent,
identical on X11 and Wayland). The callback runs on the UI thread from
`fdk_pump_events()` / `fdk_run()`, with no lock held, in
per-posting-thread FIFO order. `fdk_shutdown()` drops queued posts
without running them; posting to a shut-down context returns
`FDK_ERR_NOT_INITIALIZED` instead of crashing a dying worker.

For whole jobs rather than one-shot handoffs, the worker facility:

```c
fdk_worker *pool;
fdk_worker_pool_create(ctx, 2, &pool);       /* 1..16 threads */

fdk_job_spec spec = {
    .job       = my_off_thread_fn,  /* void* return: the result     */
    .progress  = my_progress_fn,   /* UI thread, per report()      */
    .done      = my_done_fn,       /* UI thread, once              */
    .user_data = my_state,         /* job-owned until done         */
};
fdk_job *job;
fdk_worker_submit(pool, &spec, &job);
/* ... */
fdk_job_cancel(job);        /* stops delivery; the job polls
                              fdk_job_cancelled() cooperatively */
fdk_worker_pool_destroy(pool); /* cancels + joins everything */
```

The job function runs on a worker thread; every `report()` and the
single `done()` are delivered through `fdk_post` on the UI thread.
Cancellation is cooperative: `fdk_job_cancel` stops **delivery**
promptly, and a job that polls `fdk_job_cancelled()` stops its work;
a job that never checks simply runs to completion unheard.

## Enforcement (debug builds)

With `FDK_DEBUG_THREAD_CHECKS` (defined in every debug build — the
default `make`), a main-thread-only call made from another thread is
**refused** with `FDK_ERR_WRONG_THREAD` and one log line per call
site per thread, instead of racing toward the crash it would
eventually cause. Currently guarded: `fdk_window_create`,
`fdk_timer_add` (returns NULL). Release builds keep the historical
undefined-if-you-race behavior — the guard is a debugging aid, not
a lock; the rule itself is unchanged and total.

## Testing

`tests/test_threads.c` pins the contracts under ASan (it runs as
part of `make test-x11`) and under ThreadSanitizer
(`make test-threads-tsan`, a dedicated TSan object lane for the
whole library + the suite): stress (256 posts from 4 threads),
per-thread FIFO ordering (1024 posts), the blocked-poll wakeup
latency, worker progress/result delivery, cooperative cancel,
shutdown with jobs in flight, and the wrong-thread refusal.


## Per-function documentation

As each public function is added, its doc comment states explicitly
whether it's UI-thread-only (the default assumption unless stated
otherwise) or safe to call from any thread. The complete list of
any-thread-safe functions is in `include/fdk/fdk_thread.h`:
`fdk_post`, the worker facility (`fdk_worker_pool_create`,
`fdk_worker_submit`, `fdk_job_cancel`, `fdk_job_cancelled`,
`fdk_job_status`, `fdk_worker_pool_destroy`), `fdk_log` and its
sinks (see `fdk_log.h` — the installed sink is any-thread-safe;
installing/changing it is not), and the pure version/error helpers.
