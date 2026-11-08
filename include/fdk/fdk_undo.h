/*
 * fdk_undo.h — FDK's generic undo/redo stack (1.3.3)
 *
 * One small, composable service every editable thing in an
 * application can share: the Entry widget ships an
 * fdk_undo_stack-based history (see fdk_entry_undo), and application
 * models — canvas documents, list edits, form state — get the same
 * mechanics without growing their own.
 *
 * THE MODEL — an op is three closures over one user pointer:
 *
 *   undo(user)   restores the state that existed before the op
 *   redo(user)   re-applies the op (NOT "call undo again" — the
 *                application defines both directions explicitly, so
 *                an op is free to restore ancillary state like a
 *                caret or selection in either direction)
 *   destroy(user) frees user (or does nothing for static storage);
 *                called exactly once, whether the op is undone,
 *                redone, trimmed off the tail, or still live at
 *                stack destroy
 *
 * push() records the LATEST change. undo() walks back one; redo()
 * walks forward one. A push while a redo tail exists drops the tail
 * (the classic linear-history rule: after you undo three and do
 * something new, those three are gone). An optional depth limit
 * (default 256) trims the OLDEST ops when exceeded — bounded memory
 * by construction, not by app discipline.
 *
 * REENTRANCY: undo()/redo() refuse (FDK_ERR_INVALID_STATE) to run
 * while an application's undo/redo closure is executing on the same
 * stack — nested application of history is a bug, not a feature.
 * push() during a closure is likewise refused. destroy() and clear()
 * are unconditional.
 *
 * The stack takes no context and allocates through FDK's allocator
 * (fdk_alloc — see fdk_core.h). Single-threaded, like the rest of
 * FDK (docs/threading.md).
 */

#ifndef FDK_UNDO_H
#define FDK_UNDO_H

#include "fdk_error.h"
#include "fdk_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fdk_undo_stack fdk_undo_stack;

/* One undoable operation. All three functions may be NULL: an op
 * with a NULL undo (or redo) simply stops the walk there — can_undo
 * / can_redo report false past it. NULL destroy = nothing to free.
 * The struct is COPIED on push; the application never needs to keep
 * it alive. */
typedef struct fdk_undo_op {
    void *user_data;
    void (*undo)(void *user_data);
    void (*redo)(void *user_data);
    void (*destroy)(void *user_data);
} fdk_undo_op;

/* Creates a stack holding at most `limit` ops (0 = the default, 256;
 * SIZE_MAX-ish values clamp to something the allocator can express).
 * The stack is empty: nothing to undo, nothing to redo. */
fdk_result fdk_undo_stack_create(size_t limit,
                                 fdk_undo_stack **out_stack);

/* Destroys the stack, calling op.destroy for every op still in it
 * (past AND future). Safe on NULL. */
void fdk_undo_stack_destroy(fdk_undo_stack *stack);

/* Records `op` as the newest change. Drops any redo tail (calling
 * destroy on each dropped op), appends, and trims the oldest ops
 * beyond the limit. Refused (FDK_ERR_INVALID_STATE) while this
 * stack is applying an op. */
fdk_result fdk_undo_stack_push(fdk_undo_stack *stack,
                               const fdk_undo_op *op);

/* Applies one step back. A QUIET NO-OP (FDK_OK) when there is
 * nothing to undo or the walk is stopped by an op with no undo
 * closure — keyboard handlers call undo blind; can_undo exists for
 * the callers who care. FDK_ERR_INVALID_STATE only for reentrancy
 * misuse (called from inside this stack's own closure). */
fdk_result fdk_undo_stack_undo(fdk_undo_stack *stack);

/* Applies one step forward. Same no-op and refusal rules. */
fdk_result fdk_undo_stack_redo(fdk_undo_stack *stack);

bool fdk_undo_stack_can_undo(const fdk_undo_stack *stack);
bool fdk_undo_stack_can_redo(const fdk_undo_stack *stack);

/* Ops from the oldest still-recorded to the newest (the undo side's
 * depth; the redo tail is extra). */
size_t fdk_undo_stack_depth(const fdk_undo_stack *stack);
/* Ops on the redo tail (0 right after a push). */
size_t fdk_undo_stack_redo_depth(const fdk_undo_stack *stack);

/* Inspection: the op undo() would apply next, and the op redo()
 * would apply next. *out_op points into the stack (read-only view —
 * mutating the op's user state through it is the application's own
 * bookkeeping, as Entry's coalescing does). FDK_ERR_NOT_FOUND when
 * there is nothing on that side. */
fdk_result fdk_undo_stack_top(const fdk_undo_stack *stack,
                              const fdk_undo_op **out_op);
fdk_result fdk_undo_stack_redo_top(const fdk_undo_stack *stack,
                                   const fdk_undo_op **out_op);

/* Drops everything on both sides (destroy called per op). Refused
 * while an apply is in flight — teardown code that must clear
 * unconditionally should destroy the stack instead. */
fdk_result fdk_undo_stack_clear(fdk_undo_stack *stack);

/* Reads back the limit (create's argument, post-clamp). */
size_t fdk_undo_stack_limit(const fdk_undo_stack *stack);

#ifdef __cplusplus
}
#endif

#endif /* FDK_UNDO_H */
