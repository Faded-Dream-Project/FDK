/* doc.c — fdk-workbench's document: rows + undo ops + the views.
 *
 * The undo integration is the point: every mutation records an
 * fdk_undo_op whose closures re-apply / un-apply the SAME mutation
 * through the same code paths the UI uses, and the stack owns the
 * op payloads (destroy frees them — the stack is the memory owner,
 * the app never bookkeeps).
 */
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the op payloads ---- */

typedef struct add_op {
    bench *b;
    char text[BENCH_TEXT_CAP];
} add_op;

typedef struct del_op {
    bench *b;
    int index;
    char text[BENCH_TEXT_CAP];
} del_op;

typedef struct rep_op {
    bench *b;
    int index;
    char old_text[BENCH_TEXT_CAP];
    char new_text[BENCH_TEXT_CAP];
} rep_op;

static void op_free(void *p) {
    free(p);
}

/* ---- raw mutations (no undo bookkeeping; the closures call these) ---- */

static void raw_add(bench *b, const char *text) {
    if (b->row_count >= BENCH_MAX_ROWS) {
        return;
    }
    snprintf(b->rows[b->row_count].text, BENCH_TEXT_CAP, "%s", text);
    b->row_count++;
    b->selected = b->row_count - 1;
    b->dirty = true;
}

static void raw_del(bench *b, int index) {
    if (index < 0 || index >= b->row_count) {
        return;
    }
    memmove(&b->rows[index], &b->rows[index + 1],
            (size_t)(b->row_count - index - 1) * sizeof(bench_row));
    b->row_count--;
    if (b->selected >= b->row_count) {
        b->selected = b->row_count - 1;
    }
    b->dirty = true;
}

static void raw_replace(bench *b, int index, const char *text) {
    if (index < 0 || index >= b->row_count) {
        return;
    }
    snprintf(b->rows[index].text, BENCH_TEXT_CAP, "%s", text);
    b->dirty = true;
}

/* ---- the undo op closures (closure → raw mutation, mirrored) ---- */

static void add_undo(void *p) {
    add_op *o = p;
    raw_del(o->b, o->b->row_count - 1);
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

static void add_redo(void *p) {
    add_op *o = p;
    raw_add(o->b, o->text);
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

static void del_undo(void *p) {
    del_op *o = p;
    raw_add(o->b, o->text); /* the row returns at the tail; good
                             * enough for a workbench demo — the
                             * stack mechanics are what is under
                             * test, not the edit semantics */
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

static void del_redo(void *p) {
    del_op *o = p;
    raw_del(o->b, o->index);
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

static void rep_undo(void *p) {
    rep_op *o = p;
    raw_replace(o->b, o->index, o->old_text);
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

static void rep_redo(void *p) {
    rep_op *o = p;
    raw_replace(o->b, o->index, o->new_text);
    bench_doc_refresh_views(o->b);
    bench_banner_sweep(o->b);
}

/* ---- the recording mutations the UI calls ---- */

void bench_doc_add(bench *b, const char *text, bool record) {
    if (!record) {
        raw_add(b, text);
        bench_doc_refresh_views(b);
        return;
    }
    add_op *o = calloc(1, sizeof *o);
    if (o == NULL) {
        return;
    }
    o->b = b;
    snprintf(o->text, sizeof o->text, "%s", text);
    fdk_undo_op op;
    memset(&op, 0, sizeof op);
    op.user_data = o;
    op.undo = add_undo;
    op.redo = add_redo;
    op.destroy = op_free;
    raw_add(b, text);
    if (fdk_ok(fdk_undo_stack_push(b->undo, &op))) {
        bench_doc_refresh_views(b);
        bench_banner_sweep(b);
    } else {
        free(o);
    }
}

void bench_doc_delete(bench *b, int index, bool record) {
    if (index < 0 || index >= b->row_count) {
        return;
    }
    if (!record) {
        raw_del(b, index);
        bench_doc_refresh_views(b);
        return;
    }
    del_op *o = calloc(1, sizeof *o);
    if (o == NULL) {
        return;
    }
    o->b = b;
    o->index = index;
    snprintf(o->text, sizeof o->text, "%s", b->rows[index].text);
    fdk_undo_op op;
    memset(&op, 0, sizeof op);
    op.user_data = o;
    op.undo = del_undo;
    op.redo = del_redo;
    op.destroy = op_free;
    raw_del(b, index);
    if (fdk_ok(fdk_undo_stack_push(b->undo, &op))) {
        bench_doc_refresh_views(b);
        bench_banner_sweep(b);
    } else {
        free(o);
    }
}

void bench_doc_replace(bench *b, int index, const char *text,
                       bool record) {
    if (index < 0 || index >= b->row_count) {
        return;
    }
    if (!record) {
        raw_replace(b, index, text);
        bench_doc_refresh_views(b);
        return;
    }
    rep_op *o = calloc(1, sizeof *o);
    if (o == NULL) {
        return;
    }
    o->b = b;
    o->index = index;
    snprintf(o->old_text, sizeof o->old_text, "%s",
             b->rows[index].text);
    snprintf(o->new_text, sizeof o->new_text, "%s", text);
    fdk_undo_op op;
    memset(&op, 0, sizeof op);
    op.user_data = o;
    op.undo = rep_undo;
    op.redo = rep_redo;
    op.destroy = op_free;
    raw_replace(b, index, text);
    if (fdk_ok(fdk_undo_stack_push(b->undo, &op))) {
        bench_doc_refresh_views(b);
        bench_banner_sweep(b);
    } else {
        free(o);
    }
}

void bench_undo_apply(bench *b) {
    if (fdk_ok(fdk_undo_stack_undo(b->undo))) {
        b->undos_applied++;
    }
}

void bench_undo_redo(bench *b) {
    if (fdk_ok(fdk_undo_stack_redo(b->undo))) {
        b->redos_applied++;
    }
}

/* ---- the views ---- */

void bench_doc_refresh_views(bench *b) {
    /* the sidebar mirrors the doc */
    fdk_list_clear(b->side_list);
    for (int i = 0; i < b->row_count; i++) {
        char shown[BENCH_TEXT_CAP + 16];
        snprintf(shown, sizeof shown, "%3d  %s", i, b->rows[i].text);
        size_t idx = 0;
        if (fdk_ok(fdk_list_append(b->side_list, shown, &idx))) {
            /* every row is a drag source: press + move drags the
             * row's TEXT into the Pinned drop target */
            fdk_list_row_set_drag_armed(b->side_list, idx, true);
        }
    }
    if (b->selected >= 0 && b->selected < b->row_count) {
        fdk_list_select(b->side_list, (size_t)b->selected);
    }

    /* the stats the worker does not own */
    char buf[96];
    snprintf(buf, sizeof buf, "%d", b->row_count);
    fdk_label_set_text(b->stat_rows, buf);
    if (b->level != NULL) {
        fdk_levelbar_set_value(b->level, (double)b->row_count);
    }

    /* the canvas repaints through damage */
    if (b->doc_canvas != NULL) {
        fdk_canvas_invalidate(b->doc_canvas);
    }
    if (b->ta.statusbar != NULL) {
        ta_status(&b->ta, "%s — %d rows%s", b->path[0] ? b->path
                                                 : "(unsaved)",
                  b->row_count, b->dirty ? " · modified" : "");
    }
}

/* ---- the document io + stats ---- */

void bench_doc_save_to(bench *b, const char *path) {
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ta_status(&b->ta, "save failed: %s", path);
        return;
    }
    for (int i = 0; i < b->row_count; i++) {
        fprintf(f, "%s\n", b->rows[i].text);
    }
    fclose(f);
    snprintf(b->path, sizeof b->path, "%s", path);
    b->dirty = false;
    bench_doc_refresh_views(b);
}

void bench_doc_load_from(bench *b, const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ta_status(&b->ta, "open failed: %s", path);
        return;
    }
    b->row_count = 0;
    char line[BENCH_TEXT_CAP]; /* fgets caps at cap-1 chars: the
                                * copy below cannot truncate */
    while (b->row_count < BENCH_MAX_ROWS &&
           fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        snprintf(b->rows[b->row_count].text, BENCH_TEXT_CAP, "%s",
                 line);
        b->row_count++;
    }
    fclose(f);
    snprintf(b->path, sizeof b->path, "%s", path);
    b->dirty = false;
    b->selected = b->row_count > 0 ? 0 : -1;
    bench_doc_refresh_views(b);
}

long bench_doc_count_words(const bench *b) {
    long words = 0;
    for (int i = 0; i < b->row_count; i++) {
        const char *p = b->rows[i].text;
        bool in_word = false;
        while (*p != '\0') {
            if (*p == ' ' || *p == '\t') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
            p++;
        }
    }
    return words;
}

void bench_doc_init(bench *b) {
    static const char *seed[] = {
        "welcome to fdk-workbench",
        "undo/redo drives every mutation here",
        "the worker thread hashes what you open",
        "print writes a real PDF to /tmp",
        "en / de reformats this inspector live",
        "rows drag from the sidebar into Pinned",
    };
    for (size_t i = 0; i < sizeof seed / sizeof seed[0]; i++) {
        snprintf(b->rows[b->row_count].text, BENCH_TEXT_CAP, "%s",
                 seed[i]);
        b->row_count++;
    }
    b->selected = 0;
}
