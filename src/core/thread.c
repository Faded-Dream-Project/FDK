/*
 * thread.c — the G-2 threading core: posting + workers (§25)
 *
 * Two layers:
 *
 *   the post queue    a mutex-guarded singly-linked list of
 *                     {fn, user} nodes + one wake fd (eventfd when
 *                     available, a pipe otherwise). fdk_post appends
 *                     under the mutex and writes one byte/event to
 *                     the fd; the pump's poll() sees it readable and
 *                     calls fdk__thread_drain, which swaps the whole
 *                     list out under the mutex and runs every
 *                     callback with NO lock held (callbacks may post
 *                     back, submit jobs, destroy widgets, quit).
 *
 *   the worker pool   N pthreads + a job queue of fdk_job records.
 *                     submit enqueues under the pool mutex and
 *                     signals the condition; a worker pops, marks
 *                     running, calls spec->job(user, report,
 *                     report_user) where report() is the pool's own
 *                     trampoline (a cancel-checked fdk_post of the
 *                     progress callback), then posts done() once —
 *                     or drops it when cancelled.
 *
 * The wake discipline: ONE write per post batch would be the clever
 * drain-friendly shape, but a missed EAGAIN on a full eventfd would
 * DROP a wake. Instead every post writes; the counter semantics of
 * eventfd (and the pipe's byte stream) coalesce multiple writes
 * into "readable", and the drain's swap-under-mutex means a spurious
 * extra readable simply finds an empty list. Bounded: eventfd counts
 * to u64; the pipe fills at 64K pending posts and fdk_post drains it
 * (blocking write would deadlock a worker against the UI thread it
 * is trying to wake — so a full pipe is drained by the POSTER, which
 * is safe: reading our own pipe just clears the wake we are about to
 * re-raise).
 *
 * Shutdown order (fdk_shutdown):
 *   1. the wake fd is drained and closed first;
 *   2. queued posts are freed WITHOUT running (the documented drop);
 *   3. posting after that returns FDK_ERR_NOT_INITIALIZED.
 * Worker pools are application-owned: fdk_shutdown does NOT join
 * them (an app that leaks a pool leaks it — same rule as windows).
 *
 * Thread-safety of this file: every shared field is either
 *   - owned by the UI thread only (the list HEAD between drains),
 *   - guarded by ctx->post_mutex, or
 *   - atomic (the shutdown flag).
 */

#define FDK_LOG_TAG "core"

#include "fdk/fdk_thread.h"

#include "core/alloc_internal.h"
#include "core/context_internal.h"
#include "core/log_internal.h"
#include "core/thread_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* ---- the post queue -------------------------------------------------- */

typedef struct post_node {
    void (*fn)(void *user);
    void *user;
    struct post_node *next;
} post_node;

/* The per-context thread state. Allocated at fdk_init, freed at
 * fdk_shutdown. `dead` flips BEFORE the fd closes so a racing
 * poster either sees dead (and refuses) or holds a live fd. */
struct fdk_thread_state {
    pthread_mutex_t mutex;
    post_node *head;
    post_node *tail;
    int wake_fd;        /* eventfd or pipe[1]; -1 when dead       */
    int wake_read_fd;   /* pipe[0] when a pipe; else == wake_fd   */
    bool use_pipe;
    atomic_bool dead;
    pthread_t main_thread;
};

/* The context carries this by pointer (context_internal.h). */
static struct fdk_thread_state *ts_of(const fdk_context *ctx) {
    return ctx != NULL ? ctx->thread_state : NULL;
}

static void wake_write(struct fdk_thread_state *ts) {
    if (ts->use_pipe) {
        /* a pipe: write a byte; if the reader is behind (64K posts
         * pending), drain one batch ourselves so the write cannot
         * block — the drain is safe from any thread */
        char c = 1;
        ssize_t r = write(ts->wake_fd, &c, 1);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            char sink[64];
            while (read(ts->wake_read_fd, sink, sizeof sink) > 0) {
            }
            (void)write(ts->wake_fd, &c, 1);
        }
    } else {
        uint64_t one = 1;
        ssize_t r = write(ts->wake_fd, &one, sizeof one);
        if (r < 0 && errno == EAGAIN) {
            /* eventfd saturated at u64-1: it is readable already,
             * the wake is redundant — nothing to do */
        }
    }
}

fdk_result fdk_post(fdk_context *ctx, void (*fn)(void *user),
                    void *user) {
    struct fdk_thread_state *ts = ts_of(ctx);
    if (ctx == NULL || fn == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (ts == NULL || atomic_load(&ts->dead)) {
        return FDK_ERR_NOT_INITIALIZED;
    }

    post_node *n = fdk_alloc(sizeof *n);
    if (n == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    n->fn = fn;
    n->user = user;
    n->next = NULL;

    int rc = pthread_mutex_lock(&ts->mutex);
    if (rc != 0) {
        fdk_free(n);
        return FDK_ERR_UNKNOWN;
    }
    if (atomic_load(&ts->dead)) { /* re-check under the lock */
        pthread_mutex_unlock(&ts->mutex);
        fdk_free(n);
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (ts->tail != NULL) {
        ts->tail->next = n;
    } else {
        ts->head = n;
    }
    ts->tail = n;
    pthread_mutex_unlock(&ts->mutex);

    wake_write(ts);
    return FDK_OK;
}

int fdk__thread_drain(fdk_context *ctx) {
    struct fdk_thread_state *ts = ts_of(ctx);
    if (ts == NULL) {
        return 0;
    }
    /* consume the wake token first (both shapes) */
    if (ts->use_pipe) {
        char sink[64];
        while (read(ts->wake_read_fd, sink, sizeof sink) > 0) {
        }
    } else {
        uint64_t count = 0;
        (void)read(ts->wake_fd, &count, sizeof count);
    }

    /* swap the whole list out under the lock, run it free */
    post_node *batch = NULL;
    int rc = pthread_mutex_lock(&ts->mutex);
    if (rc != 0) {
        return 0;
    }
    batch = ts->head;
    ts->head = NULL;
    ts->tail = NULL;
    pthread_mutex_unlock(&ts->mutex);

    int ran = 0;
    while (batch != NULL) {
        post_node *next = batch->next;
        batch->fn(batch->user); /* no lock held; may post back */
        fdk_free(batch);
        ran++;
        batch = next;
    }
    return ran;
}

int fdk__thread_wake_fd(const fdk_context *ctx) {
    struct fdk_thread_state *ts = ts_of(ctx);
    if (ts == NULL || atomic_load(&ts->dead)) {
        return -1;
    }
    return ts->wake_read_fd;
}

fdk_result fdk__thread_init(fdk_context *ctx) {
    if (ctx == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    struct fdk_thread_state *ts = fdk_alloc(sizeof *ts);
    if (ts == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(ts, 0, sizeof *ts);
    pthread_mutex_init(&ts->mutex, NULL);
    atomic_init(&ts->dead, false);
    ts->main_thread = pthread_self();

    ts->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ts->wake_fd >= 0) {
        ts->use_pipe = false;
        ts->wake_read_fd = ts->wake_fd;
    } else {
        int fds[2];
        if (pipe(fds) != 0) {
            pthread_mutex_destroy(&ts->mutex);
            fdk_free(ts);
            return FDK_ERR_PLATFORM_INIT;
        }
        (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
        ts->wake_fd = fds[1];
        ts->wake_read_fd = fds[0];
        ts->use_pipe = true;
    }
    ctx->thread_state = ts;
    return FDK_OK;
}

void fdk__thread_shutdown(fdk_context *ctx) {
    struct fdk_thread_state *ts = ts_of(ctx);
    if (ts == NULL) {
        return;
    }
    pthread_mutex_lock(&ts->mutex);
    atomic_store(&ts->dead, true);
    /* free the undelivered batch WITHOUT running it (documented) */
    post_node *n = ts->head;
    while (n != NULL) {
        post_node *next = n->next;
        fdk_free(n);
        n = next;
    }
    ts->head = NULL;
    ts->tail = NULL;
    pthread_mutex_unlock(&ts->mutex);

    if (ts->use_pipe) {
        close(ts->wake_read_fd);
    }
    close(ts->wake_fd);
    ts->wake_fd = -1;
    ts->wake_read_fd = -1;
    pthread_mutex_destroy(&ts->mutex);
    fdk_free(ts);
    ctx->thread_state = NULL;
}

bool fdk__on_main_thread(const fdk_context *ctx) {
    struct fdk_thread_state *ts = ts_of(ctx);
    if (ts == NULL) {
        return true; /* no state: no rule to break (headless tests) */
    }
    return pthread_equal(pthread_self(), ts->main_thread) != 0;
}

/* The refusal, shared by every guarded entry point (§25/G-2). One
 * log line per call site per process would still spam a hot loop;
 * the dedupe is per-callsite static — a misbehaving worker hears
 * about it once, loudly, and then gets clean error returns. */
fdk_result fdk__wrong_thread_refused(const char *what) {
    static _Thread_local int said[8];
    static const char *said_what[8];
    for (int i = 0; i < 8; i++) {
        if (said[i] && said_what[i] == what) {
            return FDK_ERR_WRONG_THREAD;
        }
        if (!said[i]) {
            said[i] = 1;
            said_what[i] = what;
            FDK_WARN("%s called from a non-main thread — refused "
                     "(the threading rule: docs/threading.md)",
                     what != NULL ? what : "an FDK call");
            return FDK_ERR_WRONG_THREAD;
        }
    }
    return FDK_ERR_WRONG_THREAD;
}

/* ---- the worker facility ---------------------------------------------- */

struct fdk_job {
    fdk_worker *pool;
    fdk_job_spec spec;
    atomic_int state; /* 0 queued, 1 running, 2 delivered, 3 cancelled */
    struct fdk_job *next;
};

struct fdk_worker {
    fdk_context *ctx;
    pthread_t *threads;
    int thread_count;
    pthread_mutex_t mutex;
    pthread_cond_t work_avail;
    fdk_job *head, *tail;
    bool shutdown;
};

/* The UI-thread delivery payloads. Both are plain nodes the post
 * callback frees; the SPEC travels by value so the job record can
 * die as soon as the worker finishes. */
typedef struct progress_node {
    fdk_job_spec spec;
    double fraction;
} progress_node;

typedef struct done_node {
    fdk_job_spec spec;
    void *result_user;
} done_node;

static void worker_progress_cb(void *user) {
    progress_node *pn = user;
    if (pn->spec.progress != NULL) {
        pn->spec.progress(pn->fraction, pn->spec.done_user);
    }
    free(pn);
}

static void worker_done_cb(void *user) {
    done_node *dn = user;
    if (dn->spec.done != NULL) {
        dn->spec.done(dn->result_user, dn->spec.done_user);
    }
    free(dn);
}

/* The progress trampoline handed to the job function. Runs ON the
 * worker thread; its whole job is a cancel-checked post. */
static void worker_report(double fraction, void *report_user) {
    fdk_job *job = report_user;
    if (atomic_load(&job->state) == 3) {
        return; /* cancelled: stay silent */
    }
    progress_node *pn = malloc(sizeof *pn);
    if (pn == NULL) {
        return;
    }
    pn->spec = job->spec;
    pn->fraction = fraction;
    if (!fdk_ok(fdk_post(job->pool->ctx, worker_progress_cb, pn))) {
        free(pn);
    }
}

/* the worker thread main */
static void *worker_main(void *arg) {
    fdk_worker *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (pool->head == NULL && !pool->shutdown) {
            pthread_cond_wait(&pool->work_avail, &pool->mutex);
        }
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        fdk_job *job = pool->head;
        pool->head = job->next;
        if (pool->head == NULL) {
            pool->tail = NULL;
        }
        pthread_mutex_unlock(&pool->mutex);

        atomic_store(&job->state, 1);
        void *result = job->spec.job(job->spec.user_data, worker_report, job);
        int final = atomic_load(&job->state);

        if (final == 3) {
            /* cancelled: no delivery; the handle is dead and nobody
             * else frees the record (cancel only flips the flag) */
            free(job);
        } else {
            atomic_store(&job->state, 2);
            done_node *dn = malloc(sizeof *dn);
            if (dn != NULL) {
                dn->spec = job->spec;
                dn->result_user = result;
                if (!fdk_ok(fdk_post(pool->ctx, worker_done_cb, dn))) {
                    free(dn);
                }
            }
            free(job);
        }
    }
}

fdk_result fdk_worker_pool_create(fdk_context *ctx, int threads,
                                  fdk_worker **out_pool) {
    if (ctx == NULL || out_pool == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (threads < 1) {
        threads = 1;
    }
    if (threads > 16) {
        threads = 16;
    }
    fdk_worker *pool = fdk_alloc(sizeof *pool);
    if (pool == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    memset(pool, 0, sizeof *pool);
    pool->ctx = ctx;
    pool->thread_count = threads;
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->work_avail, NULL);

    pool->threads = fdk_alloc(
        (size_t)threads * sizeof *pool->threads);
    if (pool->threads == NULL) {
        pthread_mutex_destroy(&pool->mutex);
        pthread_cond_destroy(&pool->work_avail);
        fdk_free(pool);
        return FDK_ERR_OUT_OF_MEMORY;
    }
    for (int i = 0; i < threads; i++) {
        int rc = pthread_create(&pool->threads[i], NULL, worker_main,
                                pool);
        if (rc != 0) {
            /* join the ones that started, then fail */
            pthread_mutex_lock(&pool->mutex);
            pool->shutdown = true;
            pthread_cond_broadcast(&pool->work_avail);
            pthread_mutex_unlock(&pool->mutex);
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            fdk_free(pool->threads);
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->work_avail);
            fdk_free(pool);
            return FDK_ERR_OUT_OF_MEMORY;
        }
    }
    *out_pool = pool;
    return FDK_OK;
}

fdk_result fdk_worker_submit(fdk_worker *pool,
                             const fdk_job_spec *spec,
                             fdk_job **out_job) {
    if (pool == NULL || spec == NULL || spec->job == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    fdk_job *job = fdk_alloc(sizeof *job);
    if (job == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    job->pool = pool;
    job->spec = *spec;
    atomic_init(&job->state, 0);
    job->next = NULL;

    pthread_mutex_lock(&pool->mutex);
    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->mutex);
        fdk_free(job);
        return FDK_ERR_NOT_INITIALIZED;
    }
    if (pool->tail != NULL) {
        pool->tail->next = job;
    } else {
        pool->head = job;
    }
    pool->tail = job;
    pthread_cond_signal(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    if (out_job != NULL) {
        *out_job = job; /* the handle: valid until done/cancel */
    }
    return FDK_OK;
}

fdk_result fdk_job_cancel(fdk_job *job) {
    if (job == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    atomic_store(&job->state, 3);
    return FDK_OK;
}

bool fdk_job_cancelled(const fdk_job *job) {
    return job != NULL && atomic_load(&job->state) == 3;
}

int fdk_job_status(const fdk_job *job) {
    return job != NULL ? atomic_load(&job->state) : -1;
}

void fdk_worker_pool_destroy(fdk_worker *pool) {
    if (pool == NULL) {
        return;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    /* cancel + free everything still QUEUED; running jobs finish on
     * their own (cooperative cancel) and their records are freed by
     * their own completion path */
    fdk_job *j = pool->head;
    while (j != NULL) {
        fdk_job *next = j->next;
        atomic_store(&j->state, 3);
        fdk_free(j);
        j = next;
    }
    pool->head = NULL;
    pool->tail = NULL;
    pthread_cond_broadcast(&pool->work_avail);
    pthread_mutex_unlock(&pool->mutex);

    for (int i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    fdk_free(pool->threads);
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->work_avail);
    fdk_free(pool);
}
