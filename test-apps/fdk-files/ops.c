/* ops.c — see ops.h. The engine over the G-2 worker facility. */
#include "ops.h"
#include "../common/ta_buf.h"
#include "teardown.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static fdk_worker *g_pool;
static ops_job *g_running; /* the single in-flight op            */

void ops_init(void) {
    (void)fdk_worker_pool_create(fapp.base.ctx, 2, &g_pool);
}

void ops_shutdown(void) {
    if (g_pool != NULL) {
        fdk_worker_pool_destroy(g_pool);
        g_pool = NULL;
    }
    /* g_running's record was freed by its own completion path; the
     * done() that never fired after a cancel is the documented
     * drop */
    g_running = NULL;
}

bool ops_busy(void) {
    return g_running != NULL;
}

void ops_cancel(void) {
    if (g_running != NULL) {
        g_running->cancel = true;
        fdk_job_cancel(g_running->job);
    }
}

/* ---- the shared walk -----------------------------------------------
 *
 * A depth-first directory walk with a bounded payload slice per
 * cancel-check (the cooperative contract): copy/delete/size are the
 * same walk with different visit verbs. */

typedef enum walk_verb { WALK_COPY, WALK_DELETE, WALK_SIZE } walk_verb;

typedef struct walk_state {
    walk_verb verb;
    ops_job *op;
    /* progress accounting */
    off_t done_bytes, total_bytes;
    size_t done_files;
    char cur[FI_PATH_MAX];
} walk_state;

/* A pre-pass totals the bytes so progress is a fraction, not a
 * spinner. Cheap relative to the copy itself. */
static off_t walk_total(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return st.st_size;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return 0;
    }
    off_t total = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (strcmp(de->d_name, ".") == 0 ||
             strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        char sub[TA_JOIN_MAX];
        if (ta_join(sub, sizeof sub, path, de->d_name) == 0) {
            continue;
        }
        total += walk_total(sub);
    }
    closedir(d);
    return total;
}

static bool walk_dir(walk_state *w, const char *src, const char *dst);
static void walk_report(walk_state *w);

static bool walk_one(walk_state *w, const char *src, const char *dst) {
    if (w->op->cancel) {
        return false;
    }
    struct stat st;
    if (stat(src, &st) != 0) {
        return true; /* vanished mid-walk: skip, not fail */
    }
    if (S_ISDIR(st.st_mode)) {
        return walk_dir(w, src, dst);
    }
    snprintf(w->cur, sizeof w->cur, "%.4000s", src);
    walk_report(w);

    if (w->verb == WALK_SIZE) {
        w->done_bytes += st.st_size;
        w->done_files++;
        return true;
    }
    if (w->verb == WALK_DELETE) {
        if (unlink(src) == 0) {
            w->done_bytes += st.st_size;
            w->done_files++;
        }
        return true;
    }
    /* WALK_COPY */
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return true; /* skip unreadable */
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
    if (out < 0) {
        close(in);
        return true; /* exists or unwritable: skip (conflicts were
                      * resolved up front by the dialog) */
    }
    char buf[262144];
    for (;;) {
        if (w->op->cancel) {
            close(in);
            close(out);
            unlink(dst);
            return false;
        }
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        ssize_t off = 0;
        while (off < r) {
            ssize_t wr = write(out, buf + off, (size_t)(r - off));
            if (wr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(in);
                close(out);
                return true; /* skip on mid-file failure */
            }
            off += wr;
        }
        w->done_bytes += r;
    }
    close(in);
    close(out);
    w->done_files++;
    if (w->op->job == NULL ? false : fdk_job_cancelled(w->op->job)) {
        return false;
    }
    return true;
}

static bool walk_dir(walk_state *w, const char *src, const char *dst) {
    struct stat st;
    if (stat(src, &st) != 0) {
        return true;
    }
    if (w->verb == WALK_COPY && mkdir(dst, st.st_mode & 07777) != 0 &&
        errno != EEXIST) {
        return true; /* skip unwritable destinations */
    }
    DIR *d = opendir(src);
    if (d == NULL) {
        return true;
    }
    /* snapshot the names: the verb (delete) mutates the dir */
    char (*names)[FI_NAME_MAX] = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' &&
            (strcmp(de->d_name, ".") == 0 ||
             strcmp(de->d_name, "..") == 0)) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            char (*g)[FI_NAME_MAX] =
                realloc(names, cap * sizeof *names);
            if (g == NULL) {
                break;
            }
            names = g;
        }
        snprintf(names[n], FI_NAME_MAX, "%.250s", de->d_name);
        n++;
    }
    closedir(d);

    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        char s2[TA_JOIN_MAX];
        if (ta_join(s2, sizeof s2, src, names[i]) == 0) {
            continue;
        }
        char d2[TA_JOIN_MAX];
        if (w->verb == WALK_COPY) {
            if (ta_join(d2, sizeof d2, dst, names[i]) == 0) {
                continue;
            }
        } else {
            d2[0] = '\0';
        }
        ok = walk_one(w, s2, d2);
    }
    free(names);
    if (ok && w->verb == WALK_DELETE) {
        rmdir(src);
    }
    return ok;
}

/* ---- the progress delivery (UI thread) ----------------------------- */

typedef struct progress_msg {
    double fraction;
    char current[FI_NAME_MAX];
    ops_progress_fn fn;
    void *user;
} progress_msg;

static void progress_deliver(void *p) {
    progress_msg *m = p;
    if (m->fn != NULL) {
        m->fn(m->fraction, m->current, m->user);
    }
    free(m);
}

typedef struct done_msg {
    bool ok;
    char err[256];
    ops_done_fn fn;
    void *user;
    ops_job *op;
    off_t size; /* SIZE jobs: the walked total; else 0           */
    void (*size_fn)(bool ok, off_t size, void *user); /* SIZE     */
} done_msg;

static void done_deliver(void *p) {
    done_msg *m = p;
    if (m->size_fn != NULL) {
        m->size_fn(m->ok, m->size, m->user);
    } else if (m->fn != NULL) {
        m->fn(m->ok, m->err, m->user);
    }
    ops_job *op = m->op;
    free(m);
    free(op);
    if (g_running == op) {
        g_running = NULL; /* engine free for the next operation */
    }
}

/* the worker-side progress post: called from walk_one at each file
 * boundary (and paced by bytes between). Runs ON the worker. */
static void walk_report(walk_state *w) {
    if (w->op == NULL || w->op->cancel ||
        w->op->on_progress == NULL) {
        return;
    }
    progress_msg *m = malloc(sizeof *m);
    if (m == NULL) {
        return;
    }
    m->fraction = (w->total_bytes > 0)
                      ? (double)w->done_bytes / (double)w->total_bytes
                      : 0.0;
    const char *slash = strrchr(w->cur, '/');
    snprintf(m->current, sizeof m->current, "%.250s",
             (slash != NULL) ? slash + 1 : w->cur);
    m->fn = w->op->on_progress;
    m->user = w->op->user;
    if (!fdk_ok(fdk_post(fapp.base.ctx, progress_deliver, m))) {
        free(m);
    }
}

/* ---- the job bodies -------------------------------------------------- */

static void *job_copy(void *user, void (*report)(double, void *),
                      void *report_user) {
    ops_job *op = user;
    walk_state w = {0};
    w.verb = (op->kind == FI_OP_MOVE) ? WALK_COPY : WALK_COPY;
    w.op = op;
    w.total_bytes = walk_total(op->src);
    bool ok = walk_dir(&w, op->src, op->dst);
    if (ok && op->kind == FI_OP_MOVE) {
        /* the copy succeeded: remove the source tree */
        walk_state d = {0};
        d.verb = WALK_DELETE;
        d.op = op;
        d.total_bytes = w.total_bytes;
        walk_dir(&d, op->src, "");
    }

    done_msg *m = calloc(1, sizeof *m);
    if (m != NULL) {
        m->ok = ok && !op->cancel;
        snprintf(m->err, sizeof m->err, "%s",
                 op->cancel ? "cancelled" : "");
        m->fn = op->on_done;
        m->user = op->user;
        m->op = op;
        if (!fdk_ok(fdk_post(fapp.base.ctx, done_deliver, m))) {
            free(m);
            free(op);
            g_running = NULL;
        }
    } else {
        free(op);
        g_running = NULL;
    }
    (void)report;
    (void)report_user;
    return NULL;
}

static void *job_delete(void *user, void (*report)(double, void *),
                        void *report_user) {
    ops_job *op = user;
    walk_state w = {0};
    w.verb = WALK_DELETE;
    w.op = op;
    w.total_bytes = walk_total(op->src);
    bool ok = walk_dir(&w, op->src, "");

    done_msg *m = calloc(1, sizeof *m);
    if (m != NULL) {
        m->ok = ok && !op->cancel;
        snprintf(m->err, sizeof m->err, "%s",
                 op->cancel ? "cancelled" : "");
        m->fn = op->on_done;
        m->user = op->user;
        m->op = op;
        if (!fdk_ok(fdk_post(fapp.base.ctx, done_deliver, m))) {
            free(m);
            free(op);
            g_running = NULL;
        }
    } else {
        free(op);
        g_running = NULL;
    }
    (void)report;
    (void)report_user;
    return NULL;
}

static void *job_size(void *user, void (*report)(double, void *),
                      void *report_user) {
    ops_job *op = user;
    walk_state w = {0};
    w.verb = WALK_SIZE;
    w.op = op;
    w.total_bytes = walk_total(op->src);
    bool ok = walk_dir(&w, op->src, "");
    (void)ok;

    done_msg *m = calloc(1, sizeof *m);
    if (m != NULL) {
        m->ok = !op->cancel;
        snprintf(m->err, sizeof m->err, "%s",
                 op->cancel ? "cancelled" : "");
        m->fn = NULL;
        m->size = w.done_bytes;
        m->size_fn = op->size_done;
        m->user = op->size_user;
        m->op = op;
        if (!fdk_ok(fdk_post(fapp.base.ctx, done_deliver, m))) {
            free(m);
            free(op);
            g_running = NULL;
        }
    } else {
        free(op);
        g_running = NULL;
    }
    (void)report;
    (void)report_user;
    return NULL;
}

/* ---- the public starts ---------------------------------------------- */

static bool start_job(ops_job *op, fdk_worker_job_fn body) {
    if (g_pool == NULL || g_running != NULL) {
        return false;
    }
    g_running = op;
    fdk_job_spec spec = {0};
    spec.job = body;
    spec.progress = NULL; /* the walk posts its own progress msgs
                           * (the payload carries the file name) */
    spec.done = NULL;    /* handled inside the body via done_msg   */
    spec.user_data = op;
    if (!fdk_ok(fdk_worker_submit(g_pool, &spec, &op->job))) {
        g_running = NULL;
        return false;
    }
    return true;
}

bool ops_start_copy(const char *src, const char *dst, bool move,
                    ops_progress_fn progress, ops_done_fn done,
                    void *user) {
    ops_job *op = calloc(1, sizeof *op);
    if (op == NULL) {
        return false;
    }
    op->kind = move ? FI_OP_MOVE : FI_OP_COPY;
    snprintf(op->src, sizeof op->src, "%.4000s", src);
    snprintf(op->dst, sizeof op->dst, "%.4000s", dst);
    op->on_progress = progress;
    op->on_done = done;
    op->user = user;
    if (!start_job(op, job_copy)) {
        free(op);
        return false;
    }
    return true;
}

bool ops_start_delete(const char *path, ops_progress_fn progress,
                      ops_done_fn done, void *user) {
    ops_job *op = calloc(1, sizeof *op);
    if (op == NULL) {
        return false;
    }
    op->kind = FI_OP_DELETE;
    snprintf(op->src, sizeof op->src, "%.4000s", path);
    op->on_progress = progress;
    op->on_done = done;
    op->user = user;
    if (!start_job(op, job_delete)) {
        free(op);
        return false;
    }
    return true;
}

bool ops_start_size(const char *path, ops_progress_fn progress,
                    void (*done)(bool ok, off_t size, void *user),
                    void *user) {
    ops_job *op = calloc(1, sizeof *op);
    if (op == NULL) {
        return false;
    }
    op->kind = FI_OP_DELETE; /* walk-only marker; the verb is SIZE */
    snprintf(op->src, sizeof op->src, "%.4000s", path);
    op->on_progress = progress;
    op->size_done = done;
    op->size_user = user;
    if (!start_job(op, job_size)) {
        free(op);
        return false;
    }
    return true;
}
