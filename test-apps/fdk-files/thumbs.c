/* thumbs.c — see thumbs.h. */
#include "thumbs.h"
#include "mime.h"
#include "ops.h"

#include "fdk/fdk_thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the in-memory LRU ---------------------------------------------- */

#define LRU_CAP 128

typedef struct lru_entry {
    char key[FI_PATH_MAX * 2];
    fdk_surface *surface;
    struct lru_entry *prev, *next;
} lru_entry;

static lru_entry *g_lru_head, *g_lru_tail;
static size_t g_lru_count;

static void lru_push_front(lru_entry *e) {
    e->prev = NULL;
    e->next = g_lru_head;
    if (g_lru_head != NULL) {
        g_lru_head->prev = e;
    }
    g_lru_head = e;
    if (g_lru_tail == NULL) {
        g_lru_tail = e;
    }
}

static void lru_unlink(lru_entry *e) {
    if (e->prev != NULL) {
        e->prev->next = e->next;
    } else {
        g_lru_head = e->next;
    }
    if (e->next != NULL) {
        e->next->prev = e->prev;
    } else {
        g_lru_tail = e->prev;
    }
}

/* the thumbnails' own 1-thread pool: parallel with the ops engine
 * (a slow folder copy must not starve icon painting) */
static fdk_worker *g_thumb_pool;

static fdk_worker *thumb_pool(void) {
    if (g_thumb_pool == NULL) {
        (void)fdk_worker_pool_create(fapp.base.ctx, 1, &g_thumb_pool);
    }
    return g_thumb_pool;
}

static fdk_surface *lru_get(const char *key) {
    for (lru_entry *e = g_lru_head; e != NULL; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            lru_unlink(e);
            lru_push_front(e);
            return e->surface;
        }
    }
    return NULL;
}

static void lru_put(const char *key, fdk_surface *s) {
    if (g_lru_count == LRU_CAP) {
        lru_entry *old = g_lru_tail;
        if (old != NULL) {
            lru_unlink(old);
            fdk_surface_destroy(old->surface);
            free(old);
            g_lru_count--;
        }
    }
    lru_entry *e = calloc(1, sizeof *e);
    if (e == NULL) {
        return;
    }
    snprintf(e->key, sizeof e->key, "%.4000s", key);
    e->surface = s;
    lru_push_front(e);
    g_lru_count++;
}

/* ---- the cache ------------------------------------------------------- */

bool thumbs_dir(char *out, size_t cap) {
    const char *xc = getenv("XDG_CACHE_HOME");
    if (xc != NULL && xc[0] == '/') {
        snprintf(out, cap, "%.2000s/fdk-files/thumbs/%d", xc,
                 THUMB_SIZE);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL) {
            home = ".";
        }
        snprintf(out, cap, "%.2000s/.cache/fdk-files/thumbs/%d",
                 home, THUMB_SIZE);
    }
    /* create the chain */
    char step[FI_PATH_MAX];
    snprintf(step, sizeof step, "%s", out);
    for (char *slash = step + 1; *slash != '\0'; slash++) {
        if (*slash == '/') {
            *slash = '\0';
            if (mkdir(step, 0700) != 0 && errno != EEXIST) {
                return false;
            }
            *slash = '/';
        }
    }
    if (mkdir(step, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    while (*s != '\0') {
        h = h * 33 + (unsigned char)*s++;
    }
    return h;
}

void thumbs_cache_path(const file_info *fi, char *out, size_t cap) {
    char dir[FI_PATH_MAX];
    if (!thumbs_dir(dir, sizeof dir)) {
        out[0] = '\0';
        return;
    }
    char key[FI_PATH_MAX + 64];
    snprintf(key, sizeof key, "%s:%lld:%lld", fi->path,
             (long long)fi->mtime, (long long)fi->size);
    snprintf(out, cap, "%.3800s/%016lx.png", dir,
             hash_str(key));
}

bool thumbs_wanted(const file_info *fi) {
    return !fi->is_dir && mime_is_thumbnailable(fi->mime);
}

/* ---- the worker pipeline ---------------------------------------------
 *
 * Decode with FDK's own image loader, down-scale to THUMB_SIZE with
 * nearest-neighbor (a thumbnail, not art), write the PNG through the
 * surface encoder, then hand the SURFACE back through fdk_post.
 * The worker cannot create fdk_surfaces on its own thread safely?
 * It can: fdk_surface_create* is documented pure memory work (no
 * context, no window) — the threading rule covers CONTEXT objects;
 * offscreen surfaces are standalone like fonts/themes. */

typedef struct thumb_job {
    char path[FI_PATH_MAX];
    char cache_path[FI_PATH_MAX];
    thumb_ready_fn on_ready;
    void *user;
} thumb_job;

/* The pixels a worker produces: raw RGBA + the letterbox math done
 * off-thread (nearest-neighbor into a THUMB_SIZE buffer). The UI
 * thread's deliver wraps it into an fdk_surface — the threading
 * rule's honest shape (surfaces are UI-only; PIXELS are free). */
typedef struct thumb_pixels {
    unsigned char *rgba;   /* THUMB_SIZE * THUMB_SIZE * 4          */
    char path[FI_PATH_MAX];
    char cache_path[FI_PATH_MAX];
    thumb_ready_fn on_ready;
    void *user;
} thumb_pixels;

static void thumb_deliver(void *p) {
    thumb_pixels *tp = p;
    /* wrap on the UI thread */
    fdk_surface *s = NULL;
    if (fdk_ok(fdk_surface_create(THUMB_SIZE, THUMB_SIZE, &s)) &&
        s != NULL) {
        fdk_surface_info info;
        if (fdk_ok(fdk_surface_get_info(s, &info)) &&
            info.pixels != NULL) {
            for (fdk_i32 y = 0; y < THUMB_SIZE; y++) {
                memcpy(info.pixels + (size_t)y * (size_t)info.stride,
                       tp->rgba + (size_t)y * (size_t)(THUMB_SIZE * 4),
                       (size_t)THUMB_SIZE * 4);
            }
        }
        /* cache write (best effort, UI thread: the encoder is a
         * surface API) */
        if (tp->cache_path[0] != '\0') {
            (void)fdk_surface_save_png(s, tp->cache_path);
        }
        lru_put(tp->cache_path, s);
        if (tp->on_ready != NULL) {
            tp->on_ready(tp->path, s, tp->user);
        }
    }
    free(tp->rgba);
    free(tp);
}

static void *thumb_job_fn(void *user, void (*report)(double, void *),
                          void *report_user) {
    thumb_job *j = user;
    (void)report;
    (void)report_user;

    /* ANY-THREAD decode (G-4): no fdk_surface on the worker */
    fdk_image_pixels px = {0};
    if (!fdk_ok(fdk_image_decode_pixels(j->path, &px))) {
        free(j);
        return NULL;
    }

    /* letterbox down-scale, nearest-neighbor, into the out buffer */
    unsigned char *out = calloc(1, (size_t)THUMB_SIZE * THUMB_SIZE * 4);
    if (out == NULL) {
        fdk_image_pixels_free(&px);
        free(j);
        return NULL;
    }
    double scale = 1.0;
    if (px.width > px.height) {
        scale = (double)THUMB_SIZE / (double)px.width;
    } else {
        scale = (double)THUMB_SIZE / (double)px.height;
    }
    if (scale > 1.0) {
        scale = 1.0; /* never upscale */
    }
    fdk_i32 dw = (fdk_i32)((double)px.width * scale + 0.5);
    fdk_i32 dh = (fdk_i32)((double)px.height * scale + 0.5);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }
    fdk_i32 dx = (THUMB_SIZE - dw) / 2;
    fdk_i32 dy = (THUMB_SIZE - dh) / 2;
    for (fdk_i32 y = 0; y < dh; y++) {
        fdk_i32 sy = (fdk_i32)((double)y / scale);
        if (sy >= px.height) {
            sy = px.height - 1;
        }
        for (fdk_i32 x = 0; x < dw; x++) {
            fdk_i32 sx = (fdk_i32)((double)x / scale);
            if (sx >= px.width) {
                sx = px.width - 1;
            }
            const unsigned char *src =
                px.pixels + (size_t)sy * px.stride + (size_t)sx * 4;
            unsigned char *dst =
                out + (size_t)(y + dy) * (size_t)(THUMB_SIZE * 4) +
                (size_t)(x + dx) * 4;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
    fdk_image_pixels_free(&px);

    thumb_pixels *tp = calloc(1, sizeof *tp);
    if (tp != NULL) {
        tp->rgba = out;
        snprintf(tp->path, sizeof tp->path, "%.4000s", j->path);
        snprintf(tp->cache_path, sizeof tp->cache_path, "%.4000s",
                 j->cache_path);
        tp->on_ready = j->on_ready;
        tp->user = j->user;
        if (!fdk_ok(fdk_post(fapp.base.ctx, thumb_deliver, tp))) {
            free(tp->rgba);
            free(tp);
        }
    } else {
        free(out);
    }
    free(j);
    return NULL;
}

bool thumbs_get(const file_info *fi, fdk_surface **out_surface,
                thumb_ready_fn on_ready, void *user,
                bool *out_submitted) {
    *out_surface = NULL;
    if (out_submitted != NULL) {
        *out_submitted = false;
    }
    if (!thumbs_wanted(fi)) {
        return false;
    }
    char cache[FI_PATH_MAX];
    thumbs_cache_path(fi, cache, sizeof cache);
    if (cache[0] == '\0') {
        return false;
    }

    /* memory hit? */
    fdk_surface *hit = lru_get(cache);
    if (hit != NULL) {
        *out_surface = hit;
        return true;
    }

    /* disk hit: load + memoize (fast enough for the UI thread: one
     * PNG decode of a 96px asset) */
    struct stat st;
    if (stat(cache, &st) == 0) {
        fdk_surface *s = NULL;
        if (fdk_ok(fdk_surface_create_from_image(cache, &s)) &&
            s != NULL) {
            lru_put(cache, s);
            *out_surface = s;
            return true;
        }
    }

    /* miss: the worker */
    thumb_job *j = calloc(1, sizeof *j);
    if (j == NULL) {
        return false;
    }
    snprintf(j->path, sizeof j->path, "%.4000s", fi->path);
    snprintf(j->cache_path, sizeof j->cache_path, "%.4000s", cache);
    j->on_ready = on_ready;
    j->user = user;
    fdk_job_spec spec = {0};
    spec.job = thumb_job_fn;
    spec.user_data = j;
    if (!fdk_ok(fdk_worker_submit(thumb_pool(), &spec, NULL))) {
        free(j);
        return false; /* refused: on_ready will NOT fire */
    }
    if (out_submitted != NULL) {
        *out_submitted = true;
    }
    return false; /* not ready yet — the row keeps its icon */
}

void thumbs_invalidate(void) {
    lru_entry *e = g_lru_head;
    while (e != NULL) {
        lru_entry *next = e->next;
        fdk_surface_destroy(e->surface);
        free(e);
        e = next;
    }
    g_lru_head = NULL;
    g_lru_tail = NULL;
    g_lru_count = 0;
}

void thumbs_shutdown(void) {
    if (g_thumb_pool != NULL) {
        fdk_worker_pool_destroy(g_thumb_pool);
        g_thumb_pool = NULL;
    }
    thumbs_invalidate();
}
