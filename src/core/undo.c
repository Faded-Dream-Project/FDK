/*
 * undo.c — the generic undo/redo stack (1.3.3)
 *
 * See include/fdk/fdk_undo.h for the full contract. Implementation
 * notes:
 *
 *   - Ops live in ONE array; `pos` is the index of the newest op on
 *     the undo side (count == pos means no redo tail; pos < count
 *     means ops[pos..count) are the dropped-forward redo tail).
 *     One allocation, one linear order, both walks.
 *
 *   - The depth limit trims from the FRONT (oldest): memmove the
 *     survivors, destroying the trimmed ops. Trimming is bounded
 *     amortized work — every push moves at most `limit` rows, and a
 *     push that would trim moves exactly the overflow.
 *
 *   - The applying flag is the reentrancy guard: undo()/redo()/
 *     push()/clear() refuse while an application closure runs. It
 *     is set ONLY around the closure call, so a closure that (by
 *     design) destroys a DIFFERENT stack still can.
 *
 *   - Every destroy call happens exactly once: the op leaves the
 *     array by exactly one path (trim, redo-tail drop, clear,
 *     stack destroy), and each path removes the row before calling
 *     destroy — a destroy closure may itself call stack functions
 *     (it must not touch THIS stack's state mid-mutation, and the
 *     remove-first order means the array is always consistent from
 *     inside destroy).
 */

#include "fdk/fdk_undo.h"

#define FDK_LOG_TAG "undo"
#include "core/log_internal.h"
#include "core/alloc_internal.h"
#include "core/undo_internal.h"

#include <string.h>

#define FDK_UNDO_DEFAULT_LIMIT 256

struct fdk_undo_stack {
    fdk_undo_op *ops;   /* rows [0, count)                       */
    size_t count;       /* total rows held (undo side + tail)    */
    size_t cap;
    size_t pos;         /* ops[0..pos) undoable; ops[pos..count) redoable */
    size_t limit;       /* max rows RETAINED (applied to undo side) */
    bool applying;      /* reentrancy guard                      */
};

fdk_result fdk_undo_stack_create(size_t limit,
                                 fdk_undo_stack **out_stack) {
    if (out_stack == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (limit == 0) {
        limit = FDK_UNDO_DEFAULT_LIMIT;
    }
    /* Clamp to something an allocation can express: the array alone
     * must not be able to request more bytes than size_t counts. */
    if (limit > SIZE_MAX / sizeof(fdk_undo_op)) {
        limit = SIZE_MAX / sizeof(fdk_undo_op);
    }
    fdk_undo_stack *s = fdk_alloc(sizeof(*s));
    if (s == NULL) {
        return FDK_ERR_OUT_OF_MEMORY;
    }
    s->ops = NULL;
    s->count = 0;
    s->cap = 0;
    s->pos = 0;
    s->limit = limit;
    s->applying = false;
    *out_stack = s;
    return FDK_OK;
}

void fdk_undo_stack_destroy(fdk_undo_stack *stack) {
    if (stack == NULL) {
        return;
    }
    for (size_t i = 0; i < stack->count; i++) {
        fdk_undo_op op = stack->ops[i];
        if (op.destroy != NULL) {
            op.destroy(op.user_data);
        }
    }
    fdk_free(stack->ops);
    fdk_free(stack);
}

static void undo_drop_tail(fdk_undo_stack *s) {
    /* Destroy the redo tail rows [pos, count). Remove-first per the
     * file comment: the closure sees a consistent array. */
    while (s->count > s->pos) {
        fdk_undo_op op = s->ops[--s->count];
        if (op.destroy != NULL) {
            op.destroy(op.user_data);
        }
    }
}

fdk_result fdk_undo_stack_push(fdk_undo_stack *stack,
                               const fdk_undo_op *op) {
    if (stack == NULL || op == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->applying) {
        return FDK_ERR_INVALID_STATE;
    }
    undo_drop_tail(stack);

    if (stack->count == stack->cap) {
        size_t ncap = stack->cap * 2 + 8;
        if (ncap < stack->cap ||
            ncap > SIZE_MAX / sizeof(fdk_undo_op)) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        fdk_undo_op *rows = fdk_realloc(stack->ops,
                                        ncap * sizeof(fdk_undo_op));
        if (rows == NULL) {
            return FDK_ERR_OUT_OF_MEMORY;
        }
        stack->ops = rows;
        stack->cap = ncap;
    }
    stack->ops[stack->count++] = *op;
    stack->pos = stack->count;

    /* Depth limit: trim OLDEST rows beyond `limit`. The invariant
     * "count <= limit after every push" makes this at most ONE row
     * (count was <= limit before the push) — a fixed copy, not an
     * allocation, keeps push O(1) worst-case. */
    if (stack->count > stack->limit) {
        fdk_undo_op dropped = stack->ops[0];
        memmove(stack->ops, stack->ops + 1,
                (stack->count - 1) * sizeof(fdk_undo_op));
        stack->count--;
        stack->pos--;
        /* Destroy AFTER the array is consistent. */
        if (dropped.destroy != NULL) {
            dropped.destroy(dropped.user_data);
        }
    }
    return FDK_OK;
}

fdk_result fdk_undo_stack_undo(fdk_undo_stack *stack) {
    if (stack == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->applying) {
        return FDK_ERR_INVALID_STATE;
    }
    /* Nothing to undo, or the walk is stopped by an op with no undo
     * closure: a quiet no-op (the Qt/GTK convention) — keyboard
     * handlers call undo blind, and can_undo exists for the callers
     * who care. A NULL closure mid-stack logs once: it is a model
     * wiring mistake worth hearing about. */
    if (stack->pos == 0) {
        return FDK_OK;
    }
    fdk_undo_op *op = &stack->ops[stack->pos - 1];
    if (op->undo == NULL) {
        FDK_WARN("undo: op %zu has no undo closure; walk stops",
                 stack->pos - 1);
        return FDK_OK;
    }
    stack->pos--;
    stack->applying = true;
    op->undo(op->user_data);
    stack->applying = false;
    return FDK_OK;
}

fdk_result fdk_undo_stack_redo(fdk_undo_stack *stack) {
    if (stack == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->applying) {
        return FDK_ERR_INVALID_STATE;
    }
    /* Same quiet-no-op convention as undo(). */
    if (stack->pos >= stack->count) {
        return FDK_OK;
    }
    fdk_undo_op *op = &stack->ops[stack->pos];
    if (op->redo == NULL) {
        FDK_WARN("undo: op %zu has no redo closure; walk stops",
                 stack->pos);
        return FDK_OK;
    }
    stack->pos++;
    stack->applying = true;
    op->redo(op->user_data);
    stack->applying = false;
    return FDK_OK;
}

bool fdk_undo_stack_can_undo(const fdk_undo_stack *stack) {
    return stack != NULL && stack->pos > 0 &&
           stack->ops[stack->pos - 1].undo != NULL;
}

bool fdk_undo_stack_can_redo(const fdk_undo_stack *stack) {
    return stack != NULL && stack->pos < stack->count &&
           stack->ops[stack->pos].redo != NULL;
}

size_t fdk_undo_stack_depth(const fdk_undo_stack *stack) {
    return (stack != NULL) ? stack->pos : 0;
}

size_t fdk_undo_stack_redo_depth(const fdk_undo_stack *stack) {
    return (stack != NULL) ? stack->count - stack->pos : 0;
}

fdk_result fdk_undo_stack_top(const fdk_undo_stack *stack,
                              const fdk_undo_op **out_op) {
    if (stack == NULL || out_op == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->pos == 0) {
        return FDK_ERR_NOT_FOUND;
    }
    *out_op = &stack->ops[stack->pos - 1];
    return FDK_OK;
}

fdk_result fdk_undo_stack_redo_top(const fdk_undo_stack *stack,
                                   const fdk_undo_op **out_op) {
    if (stack == NULL || out_op == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->pos >= stack->count) {
        return FDK_ERR_NOT_FOUND;
    }
    *out_op = &stack->ops[stack->pos];
    return FDK_OK;
}

fdk_result fdk__undo_stack_replace_top(fdk_undo_stack *stack,
                                       const fdk_undo_op *op) {
    if (stack == NULL || op == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->applying || stack->pos == 0) {
        return FDK_ERR_INVALID_STATE;
    }
    /* Swap first, destroy after — the array stays consistent from
     * inside the old op's destroy closure, exactly like every other
     * removal path in this file. */
    fdk_undo_op old = stack->ops[stack->pos - 1];
    stack->ops[stack->pos - 1] = *op;
    if (old.destroy != NULL) {
        old.destroy(old.user_data);
    }
    return FDK_OK;
}

fdk_result fdk_undo_stack_clear(fdk_undo_stack *stack) {
    if (stack == NULL) {
        return FDK_ERR_INVALID_ARGUMENT;
    }
    if (stack->applying) {
        return FDK_ERR_INVALID_STATE;
    }
    undo_drop_tail(stack);
    while (stack->count > 0) {
        fdk_undo_op op = stack->ops[--stack->count];
        if (op.destroy != NULL) {
            op.destroy(op.user_data);
        }
    }
    stack->pos = 0;
    return FDK_OK;
}

size_t fdk_undo_stack_limit(const fdk_undo_stack *stack) {
    return (stack != NULL) ? stack->limit : 0;
}
