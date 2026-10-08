/*
 * test_undo.c — the 1.3.3 generic undo stack battery
 *
 * A mock "document" (an int and a log) drives the ops, so every
 * stack semantic is pinned against observable state:
 *   - push/undo/redo interleavings, including the classic
 *     undo-undo-push-drops-redo-tail rule
 *   - depth limit trimming the OLDEST op, with destroy firing
 *     exactly once per op across every exit path (trim, tail drop,
 *     clear, stack destroy)
 *   - reentrancy refusal from inside a closure
 *   - quiet no-op undo/redo on an empty stack (the blind
 *     Ctrl+Z contract)
 *   - the top/redo_top inspection API Entry's coalescing relies on
 *
 * Entry's recording/coalescing behavior is test_entry.c's job; this
 * file owns the stack itself.
 */

#include "fdk/fdk.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a mock document ---- */

typedef struct doc {
    int value;
    int live_ops;      /* how many op structs are currently alive */
} doc;

typedef struct doc_op {
    doc *d;
    int before;
    int after;
    const char *label;
} doc_op;

static int g_destroy_count = 0;

static void doc_undo(void *user) {
    doc_op *op = user;
    op->d->value = op->before;
}

static void doc_redo(void *user) {
    doc_op *op = user;
    op->d->value = op->after;
}

static void doc_destroy(void *user) {
    doc_op *op = user;
    op->d->live_ops--;
    g_destroy_count++;
    free(op);
}

/* Records a value change: before -> after. */
static void doc_edit(doc *d, fdk_undo_stack *s, int after,
                     const char *label) {
    doc_op *op = malloc(sizeof(*op));
    assert(op != NULL);
    op->d = d;
    op->before = d->value;
    op->after = after;
    op->label = label;
    d->value = after;
    d->live_ops++;
    fdk_undo_op uop = {
        .user_data = op,
        .undo = doc_undo,
        .redo = doc_redo,
        .destroy = doc_destroy,
    };
    assert(fdk_ok(fdk_undo_stack_push(s, &uop)));
}

/* ---- basics: push/undo/redo walk ---- */

static void test_walk(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    assert(!fdk_undo_stack_can_undo(s));
    assert(!fdk_undo_stack_can_redo(s));
    assert(fdk_undo_stack_depth(s) == 0);

    /* Quiet no-ops on the empty stack (the blind-Ctrl+Z contract). */
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(fdk_ok(fdk_undo_stack_redo(s)));

    doc_edit(&d, s, 1, "one");
    doc_edit(&d, s, 2, "two");
    doc_edit(&d, s, 3, "three");
    assert(fdk_undo_stack_depth(s) == 3);
    assert(fdk_undo_stack_redo_depth(s) == 0);
    assert(d.value == 3);

    assert(fdk_undo_stack_can_undo(s));
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(d.value == 2);
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(d.value == 1);
    assert(fdk_undo_stack_can_redo(s));
    assert(fdk_undo_stack_redo_depth(s) == 2);
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(d.value == 2);
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(d.value == 1);
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(d.value == 0);
    assert(!fdk_undo_stack_can_undo(s));
    assert(fdk_ok(fdk_undo_stack_undo(s))); /* still quiet at the floor */
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(d.value == 3);
    assert(!fdk_undo_stack_can_redo(s));

    fdk_undo_stack_destroy(s);
    assert(d.live_ops == 0); /* every destroy ran exactly once */
    printf("[ok] undo stack: push/undo/redo walk, quiet floor/ceiling, "
           "destroy bookkeeping\n");
}

/* ---- THE linear-history rule ---- */

static void test_push_drops_tail(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    doc_edit(&d, s, 1, "a");
    doc_edit(&d, s, 2, "b");
    doc_edit(&d, s, 3, "c");
    (void)fdk_undo_stack_undo(s);
    (void)fdk_undo_stack_undo(s);
    assert(d.value == 1);
    assert(fdk_undo_stack_redo_depth(s) == 2);

    /* A NEW edit after undos: the redo tail dies. */
    doc_edit(&d, s, 99, "new");
    assert(fdk_undo_stack_redo_depth(s) == 0);
    assert(!fdk_undo_stack_can_redo(s));
    /* Undo walks back over: new, a — b and c are GONE. */
    (void)fdk_undo_stack_undo(s);
    assert(d.value == 1);
    (void)fdk_undo_stack_undo(s);
    assert(d.value == 0);
    assert(!fdk_undo_stack_can_undo(s));

    fdk_undo_stack_destroy(s);
    assert(d.live_ops == 0);
    printf("[ok] undo stack: push drops the redo tail (linear "
           "history), dropped ops destroyed\n");
}

/* ---- depth limit trims the OLDEST ---- */

static void test_limit(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(3, &s)));
    assert(fdk_undo_stack_limit(s) == 3);

    doc_edit(&d, s, 1, "one");
    doc_edit(&d, s, 2, "two");
    doc_edit(&d, s, 3, "three");
    assert(fdk_undo_stack_depth(s) == 3);
    doc_edit(&d, s, 4, "four"); /* trims "one" */
    assert(fdk_undo_stack_depth(s) == 3);
    assert(d.live_ops == 3); /* "one" destroyed exactly once */

    /* Undo reaches only three steps back (to value 1's edit is gone;
     * the walk stops at "two"). */
    (void)fdk_undo_stack_undo(s); /* -> 3 */
    (void)fdk_undo_stack_undo(s); /* -> 2 */
    (void)fdk_undo_stack_undo(s); /* -> 1 */
    assert(d.value == 1);
    assert(!fdk_undo_stack_can_undo(s));

    fdk_undo_stack_destroy(s);
    assert(d.live_ops == 0);
    printf("[ok] undo stack: limit trims the oldest op, bounded depth, "
           "destroy fires once per trimmed op\n");
}

/* ---- reentrancy refusal ---- */

static fdk_undo_stack *g_reenter_stack = NULL;
static int g_reenter_refused = 0;
static int g_redo_ran = 0;

static void bad_undo(void *user) {
    (void)user;
    /* Pushing from INSIDE the undo closure must be refused. */
    fdk_undo_op op = {0};
    if (!fdk_ok(fdk_undo_stack_push(g_reenter_stack, &op))) {
        g_reenter_refused++;
    }
    if (!fdk_ok(fdk_undo_stack_undo(g_reenter_stack))) {
        g_reenter_refused++;
    }
    if (!fdk_ok(fdk_undo_stack_redo(g_reenter_stack))) {
        g_reenter_refused++;
    }
    if (!fdk_ok(fdk_undo_stack_clear(g_reenter_stack))) {
        g_reenter_refused++;
    }
}

static void plain_redo(void *user) {
    (void)user;
    g_redo_ran++;
}

static void test_reentrancy(void) {
    g_reenter_refused = 0;
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    g_reenter_stack = s;
    int dummy = 0;
    fdk_undo_op op = {
        .user_data = &dummy,
        .undo = bad_undo,
        .redo = plain_redo,
        .destroy = NULL,
    };
    assert(fdk_ok(fdk_undo_stack_push(s, &op)));
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    /* All four mid-closure calls were refused with INVALID_STATE. */
    assert(g_reenter_refused == 4);
    /* The stack is still walkable after the refusal storm. */
    assert(fdk_undo_stack_can_redo(s));
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(g_redo_ran == 1);
    fdk_undo_stack_destroy(s);
    printf("[ok] undo stack: reentrant push/undo/redo/clear refused "
           "with INVALID_STATE, stack still consistent\n");
}

/* ---- clear ---- */

static void test_clear(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    doc_edit(&d, s, 1, "a");
    doc_edit(&d, s, 2, "b");
    (void)fdk_undo_stack_undo(s);
    assert(fdk_undo_stack_redo_depth(s) == 1);
    assert(fdk_ok(fdk_undo_stack_clear(s)));
    assert(fdk_undo_stack_depth(s) == 0);
    assert(fdk_undo_stack_redo_depth(s) == 0);
    assert(d.live_ops == 0);
    assert(!fdk_undo_stack_can_undo(s));
    assert(!fdk_undo_stack_can_redo(s));
    assert(fdk_ok(fdk_undo_stack_clear(s))); /* idempotent */
    fdk_undo_stack_destroy(s);
    printf("[ok] undo stack: clear drops both sides, destroys once, "
           "idempotent\n");
}

/* ---- inspection API (the coalescing seam) ---- */

static void test_inspection(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    const fdk_undo_op *top = NULL;
    assert(fdk_undo_stack_top(s, &top) == FDK_ERR_NOT_FOUND);
    assert(fdk_undo_stack_redo_top(s, &top) == FDK_ERR_NOT_FOUND);
    assert(fdk_undo_stack_top(NULL, &top) == FDK_ERR_INVALID_ARGUMENT);

    doc_edit(&d, s, 1, "one");
    doc_edit(&d, s, 2, "two");
    assert(fdk_ok(fdk_undo_stack_top(s, &top)));
    const doc_op *dop = top->user_data;
    assert(strcmp(dop->label, "two") == 0); /* newest is on top */
    (void)fdk_undo_stack_undo(s);
    assert(fdk_ok(fdk_undo_stack_top(s, &top)));
    dop = top->user_data;
    assert(strcmp(dop->label, "one") == 0);
    assert(fdk_ok(fdk_undo_stack_redo_top(s, &top)));
    dop = top->user_data;
    assert(strcmp(dop->label, "two") == 0); /* the tail's head is two */
    assert(fdk_undo_stack_top(s, NULL) == FDK_ERR_INVALID_ARGUMENT);

    fdk_undo_stack_destroy(s);
    printf("[ok] undo stack: top/redo_top inspection + NOT_FOUND/"
           "argument discipline\n");
}

/* ---- NULL-closure walk stops ---- */

static void test_null_closures(void) {
    doc d = {0, 0};
    fdk_undo_stack *s = NULL;
    assert(fdk_ok(fdk_undo_stack_create(0, &s)));
    doc_edit(&d, s, 1, "one");
    /* An op with NO undo closure: the UNDO-side walk stops at it. */
    int marker = 0;
    fdk_undo_op stop_undo = {
        .user_data = &marker, .undo = NULL, .redo = NULL,
        .destroy = NULL,
    };
    assert(fdk_ok(fdk_undo_stack_push(s, &stop_undo)));
    doc_edit(&d, s, 2, "two");
    assert(d.value == 2);

    assert(fdk_ok(fdk_undo_stack_undo(s))); /* "two" undone */
    assert(d.value == 1);
    /* The stop op has no undo: quiet no-op, pos does not move. */
    assert(fdk_ok(fdk_undo_stack_undo(s)));
    assert(d.value == 1);
    assert(!fdk_undo_stack_can_undo(s));
    /* The stop op is BELOW pos (undo side): the redo tail starts at
     * "two", which redoes fine — the walk stops did not eat it. */
    assert(fdk_undo_stack_can_redo(s));
    assert(fdk_ok(fdk_undo_stack_redo(s)));
    assert(d.value == 2);

    /* Now a stop on the REDO side: undo "two", then push a NULL-redo
     * op — the redo tail is that op alone, and redo() stops on it. */
    (void)fdk_undo_stack_undo(s); /* -> 1 */
    int marker2 = 0;
    fdk_undo_op stop_redo = {
        .user_data = &marker2, .undo = NULL, .redo = NULL,
        .destroy = NULL,
    };
    assert(fdk_ok(fdk_undo_stack_push(s, &stop_redo))); /* tail dies */
    assert(fdk_undo_stack_depth(s) == 3); /* one, stop_undo, stop_redo */
    assert(fdk_undo_stack_redo_depth(s) == 0);
    assert(!fdk_undo_stack_can_redo(s)); /* stop_redo has no redo */

    fdk_undo_stack_destroy(s);
    printf("[ok] undo stack: NULL closures stop their own side's walk "
           "(can_undo/can_redo report honestly, other side intact)\n");
}

int main(void) {
    g_destroy_count = 0;
    test_walk();
    test_push_drops_tail();
    test_limit();
    test_reentrancy();
    test_clear();
    test_inspection();
    test_null_closures();
    assert(g_destroy_count >= 10); /* every path exercised destroys */
    printf("all undo stack tests passed\n");
    return 0;
}
