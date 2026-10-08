/*
 * undo_internal.h — internal seams of the undo stack that widget
 * code needs beyond the public API. Not installed, not public.
 */

#ifndef FDK_UNDO_INTERNAL_H
#define FDK_UNDO_INTERNAL_H

#include "fdk/fdk_undo.h"

/* Atomically REPLACES the op on top of the undo side with `op`
 * (destroying the old one). The Entry widget's gesture grouping
 * uses this: a selection-delete followed in the SAME keystroke by
 * an insert composes into one "replace" op, so undo steps match
 * user gestures instead of internal splice order.
 *
 * FDK_ERR_INVALID_STATE when the undo side is empty or an apply is
 * in flight; FDK_ERR_INVALID_ARGUMENT on NULLs. */
fdk_result fdk__undo_stack_replace_top(fdk_undo_stack *stack,
                                       const fdk_undo_op *op);

#endif /* FDK_UNDO_INTERNAL_H */
