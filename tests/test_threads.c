/* test_threads.c — the G-2 cross-thread posting suite (§25).
 *
 * A SEPARATE binary from test_x11_integration.c on purpose: it needs
 * a real context + pump (so it runs under test-x11's throwaway Xvfb,
 * not plain hermetic `make test`), and it is the ONE suite also built
 * with ThreadSanitizer (`make test-threads-tsan`) — the thread
 * sanitizer needs every participant compiled with -fsanitize=thread,
 * which would distort the timing-sensitive expectations of the main
 * X11 suite.
 *
 * What it pins (each is a documented G-2 contract):
 *
 *   posting       fdk_post from N worker threads, M posts each, all
 *                 M*N callbacks arrive, none lost, none doubled
 *   ordering      per-thread FIFO order is preserved (the queue is a
 *                 linked list under a mutex; the wakeup is the only
 *                 racy part and it cannot reorder)
 *   wake          a post made while the main thread is BLOCKED inside
 *                 poll() wakes the pump within a bounded time (the
 *                 eventfd/pipe write) — the reason G-2 exists
 *   main thread   every callback runs ON the main thread (verified
 *                 by capturing pthread_self in the callback)
 *   UI-thread rule a worker calling a main-thread-only function
 *                 (fdk_window_create) gets FDK_ERR_WRONG_THREAD
 *                 when FDK_DEBUG_THREAD_CHECKS is compiled in, and
 *                 the call is refused
 *   workers       fdk_worker_submit: job runs off-thread, result +
 *                 progress land on the main thread, cancel stops
 *                 delivery, destroy with jobs in flight leaks
 *                 nothing (LSan)
 *   shutdown      fdk_shutdown with posts queued and workers running
 *                 is clean (posts are dropped, workers joined, no
 *                 UAF, no leak)
 *
 * Failure protocol: written RED first (the functions do not exist),
 * then the implementation, then this suite runs green under ASan
 * (make test-x11) and TSan (make test-threads-tsan).
 */

#include "hermetic_env.h"
#include "fdk/fdk.h"
#include "fdk/fdk_thread.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void nap_ms(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- helpers --------------------------------------------------------- */

static fdk_context *g_ctx;
static pthread_t g_main_thread;
static double g_wake_t0;
static int g_illegal_result;
static atomic_int g_cancel_requested;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* The pump-with-budget: pumps until `done` flips or ms elapses. */
static bool pump_until(atomic_int *done, double budget_ms) {
    double t0 = now_ms();
    while (atomic_load(done) == 0) {
        if (now_ms() - t0 > budget_ms) {
            return false;
        }
        fdk_pump_events(g_ctx, 20);
    }
    return true;
}

/* ---- test: basic posting + main-thread delivery -------------------- */

static atomic_int g_delivered;
static pthread_t g_seen_thread[512];
static void count_cb(void *user) {
    (void)user;
    g_seen_thread[atomic_load(&g_delivered) % 512] = pthread_self();
    atomic_fetch_add(&g_delivered, 1);
}

static void *poster_thread(void *user) {
    int n = (int)(intptr_t)user;
    for (int i = 0; i < n; i++) {
        assert(fdk_ok(fdk_post(g_ctx, count_cb, NULL)));
    }
    return NULL;
}

static void test_post_basic(void) {
    enum { N_THREADS = 4, PER_THREAD = 64 };
    atomic_store(&g_delivered, 0);
    pthread_t th[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        assert(pthread_create(&th[i], NULL, poster_thread,
                              (void *)(intptr_t)PER_THREAD) == 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    int expected = N_THREADS * PER_THREAD;
    if (!pump_until(&g_delivered, 5000.0)) {
        printf("[FAIL] post basic: only %d/%d delivered\n",
               atomic_load(&g_delivered), expected);
        assert(0);
    }
    assert(atomic_load(&g_delivered) == expected);
    /* every callback ran on the MAIN thread */
    for (int i = 0; i < 512 && i < expected; i++) {
        assert(pthread_equal(g_seen_thread[i], g_main_thread));
    }
    printf("[ok] threads: %d posts from 4 threads, all delivered on "
           "the main thread\n",
           expected);
}

/* ---- test: per-thread FIFO ordering -------------------------------- */

enum { ORDER_SLOTS = 256 };
static atomic_int g_order_pos;
static _Atomic int g_next_expected[4];
static void order_cb(void *user) {
    int thread = (int)(intptr_t)user >> 16;
    int seq = (int)((intptr_t)user & 0xffff);
    (void)atomic_fetch_add(&g_order_pos, 1);
    int want = atomic_load(&g_next_expected[thread]);
    assert(seq == want);
    atomic_store(&g_next_expected[thread], want + 1);
}

static void *order_thread(void *user) {
    int thread = (int)(intptr_t)user;
    for (int i = 0; i < ORDER_SLOTS; i++) {
        fdk_post(g_ctx, order_cb,
                 (void *)(intptr_t)((thread << 16) | (i & 0xffff)));
    }
    return NULL;
}

static void test_post_ordering(void) {
    atomic_store(&g_order_pos, 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++) {
        assert(pthread_create(&th[i], NULL, order_thread,
                              (void *)(intptr_t)i) == 0);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(th[i], NULL);
    }
    volatile int total = 0;
    while (atomic_load(&g_order_pos) < 4 * ORDER_SLOTS) {
        fdk_pump_events(g_ctx, 10);
        total++;
        if (total > 2000) {
            assert(0 && "ordering test stalled");
        }
    }
    printf("[ok] threads: per-thread FIFO order preserved across "
           "%d posts\n",
           4 * ORDER_SLOTS);
}

/* ---- test: the blocked-poll wakeup ---------------------------------- */

static atomic_int g_wake_seen;
static double g_wake_lag_ms;
static void wake_cb(void *user) {
    (void)user;
    g_wake_lag_ms = now_ms() - g_wake_t0;
    atomic_store(&g_wake_seen, 1);
}

static void *wake_thread(void *user) {
    (void)user;
    nap_ms(120); /* let the main thread reach poll() */
    g_wake_t0 = now_ms();
    fdk_post(g_ctx, wake_cb, NULL);
    return NULL;
}

static void test_post_wakes_blocked_pump(void) {
    atomic_store(&g_wake_seen, 0);
    pthread_t th;
    assert(pthread_create(&th, NULL, wake_thread, NULL) == 0);
    /* block in the pump with a LONG timeout; only the post's
     * eventfd/pipe write may bring us back */
    double t0 = now_ms();
    while (!atomic_load(&g_wake_seen)) {
        fdk_pump_events(g_ctx, 2000);
        if (now_ms() - t0 > 4000) {
            break;
        }
    }
    pthread_join(th, NULL);
    assert(atomic_load(&g_wake_seen) == 1);
    /* the wakeup must be prompt: the write happens before our
     * callback; budget generously for CI scheduling */
    assert(g_wake_lag_ms < 1000.0);
    printf("[ok] threads: a post woke a blocked poll in %.1f ms\n",
           g_wake_lag_ms);
}

/* ---- test: the UI-thread rule ---------------------------------------- */

static void *illegal_thread(void *user) {
    (void)user;
    fdk_window_options o = {0};
    fdk_window *w = NULL;
    fdk_result r = fdk_window_create(g_ctx, &o, &w);
    g_illegal_result = (int)r;
    if (fdk_ok(r)) {
        fdk_window_destroy(w); /* (not reached when checks are on) */
    }
    return NULL;
}

static void test_wrong_thread_refused(void) {
    pthread_t th;
    assert(pthread_create(&th, NULL, illegal_thread, NULL) == 0);
    pthread_join(th, NULL);
#ifdef FDK_DEBUG_THREAD_CHECKS
    assert(g_illegal_result == FDK_ERR_WRONG_THREAD);
    printf("[ok] threads: a worker's fdk_window_create refused with "
           "FDK_ERR_WRONG_THREAD\n");
#else
    /* The release build does not refuse; the test still documents
     * the expectation (and the debug suite enforces it). */
    printf("[skip] thread checks compiled out (release build)\n");
#endif
}

/* ---- test: the worker facility --------------------------------------- */

static atomic_int g_result_seen, g_progress_calls;
static int g_job_result;
static void *job_fn(void *user, void (*report)(double fraction,
                                             void *report_user),
                     void *report_user) {
    (void)user;
    for (int i = 1; i <= 4; i++) {
        report((double)i / 4.0, report_user);
    }
    g_job_result = 42;
    return (void *)(intptr_t)42;
}

static void job_progress(double frac, void *user) {
    (void)user;
    (void)frac;
    atomic_fetch_add(&g_progress_calls, 1);
    /* progress must ALSO be main-thread */
    assert(pthread_equal(pthread_self(), g_main_thread));
}

static void job_done(void *result, void *user) {
    (void)user;
    assert((int)(intptr_t)result == 42);
    atomic_store(&g_result_seen, 1);
}

static void test_worker_roundtrip(void) {
    atomic_store(&g_result_seen, 0);
    atomic_store(&g_progress_calls, 0);
    fdk_worker *w = NULL;
    assert(fdk_ok(fdk_worker_pool_create(g_ctx, 2, &w)));
    fdk_job_spec spec = {0};
    spec.job = job_fn;
    spec.progress = job_progress;
    spec.done = job_done;
    spec.user_data = NULL;
    fdk_job *job = NULL;
    assert(fdk_ok(fdk_worker_submit(w, &spec, &job)));
    /* progress + done arrive through the pump on the main thread */
    double t0 = now_ms();
    while (!atomic_load(&g_result_seen)) {
        fdk_pump_events(g_ctx, 20);
        if (now_ms() - t0 > 5000) {
            assert(0 && "worker result never arrived");
        }
    }
    assert(atomic_load(&g_progress_calls) == 4);
    fdk_worker_pool_destroy(w);
    printf("[ok] threads: worker job ran off-thread, 4 progress "
           "reports + result on the main thread\n");
}

/* ---- test: cancel ----------------------------------------------------- */

static atomic_int g_cancel_done_seen;
static void *cancel_job_fn(void *user,
                           void (*report)(double fraction,
                                          void *report_user),
                           void *report_user) {
    (void)user;
    for (int i = 0; i < 50; i++) {
        if (atomic_load_explicit(&g_cancel_requested,
                                 memory_order_relaxed)) {
            return NULL; /* cooperative */
        }
        report(0.0, report_user);
        nap_ms(20);
    }
    return NULL;
}

static atomic_int g_cancel_requested;

static void cancel_job_done(void *result, void *user) {
    (void)result;
    (void)user;
    atomic_store(&g_cancel_done_seen, 1);
}

static void test_worker_cancel(void) {
    g_cancel_requested = 0;
    atomic_store(&g_cancel_done_seen, 0);
    fdk_worker *w = NULL;
    assert(fdk_ok(fdk_worker_pool_create(g_ctx, 1, &w)));
    fdk_job_spec spec = {0};
    spec.job = cancel_job_fn;
    spec.done = cancel_job_done;
    fdk_job *job = NULL;
    assert(fdk_ok(fdk_worker_submit(w, &spec, &job)));
    fdk_pump_events(g_ctx, 50); /* let it start */
    fdk_job_cancel(job);
    atomic_store(&g_cancel_requested, 1); /* the app-side flag the job polls */
    fdk_pump_events(g_ctx, 50);
    fdk_worker_pool_destroy(w);
    /* cancelled: done() is NOT delivered after a successful cancel
     * (the contract: cancel wins the race, delivery stops) */
    assert(atomic_load(&g_cancel_done_seen) == 0);
    printf("[ok] threads: cancel stops the done delivery\n");
}

/* ---- test: shutdown with work in flight -------------------------------- */

static void *shutdown_job_fn(void *user,
                             void (*report)(double fraction,
                                            void *report_user),
                             void *report_user) {
    (void)report;
    (void)report_user;
    nap_ms(50);
    (void)user;
    return NULL;
}

static void test_shutdown_with_jobs(void) {
    fdk_worker *w = NULL;
    assert(fdk_ok(fdk_worker_pool_create(g_ctx, 2, &w)));
    for (int i = 0; i < 4; i++) {
        fdk_job_spec spec = {0};
        spec.job = shutdown_job_fn;
        assert(fdk_ok(fdk_worker_submit(w, &spec, NULL)));
    }
    /* posts queued but undelivered */
    assert(fdk_ok(fdk_post(g_ctx, count_cb, NULL)));
    /* destroy with everything in flight: joins workers, drops
     * undelivered posts, frees cleanly (LSan's job to check) */
    fdk_worker_pool_destroy(w);
    printf("[ok] threads: destroy with 4 jobs in flight + queued "
           "posts is clean\n");
}

/* ---- main -------------------------------------------------------------- */

int main(void) {
    setenv("FDK_PREFS_FILE", "/nonexistent-fdk-hermetic.prefs", 1);
    setenv("FDK_THEME", "", 1);

    fdk_init_options opts = {0};
    opts.app_id = "org.fdk.tests.threads";
    fdk_result r = fdk_init(&g_ctx, &opts);
    if (!fdk_ok(r)) {
        printf("[skip] no display: %s\n", fdk_result_to_string(r));
        return 0;
    }
    g_main_thread = pthread_self();

    test_post_basic();
    test_post_ordering();
    test_post_wakes_blocked_pump();
    test_wrong_thread_refused();
    test_worker_roundtrip();
    test_worker_cancel();
    test_shutdown_with_jobs();

    fdk_shutdown(g_ctx);
    printf("all thread tests passed\n");
    return 0;
}
