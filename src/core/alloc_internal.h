/*
 * alloc_internal.h — internal allocation helpers
 *
 * Centralizes heap allocation so that:
 *   1. Out-of-memory is handled in one place (log + return NULL,
 *      never abort() — the application decides how to react).
 *   2. A future debug build can swap in allocation tracking/leak
 *      detection without touching call sites (see docs/memory.md).
 *
 * Not part of the public API.
 */

#ifndef FDK_ALLOC_INTERNAL_H
#define FDK_ALLOC_INTERNAL_H

#include <stddef.h>

/* Zero-initialized allocation of `size` bytes. Returns NULL on failure
 * (already logged); never invokes abort()/exit(). size == 0 returns
 * NULL, matching fdk_free(NULL)'s no-op contract. */
void *fdk_alloc(size_t size);

/* fdk_alloc() sized for an array of `count` elements of `elem_size`
 * bytes, with overflow-checked multiplication (returns NULL rather
 * than wrapping and under-allocating). */
void *fdk_alloc_array(size_t count, size_t elem_size);

/* Frees a pointer returned by fdk_alloc()/fdk_alloc_array()/
 * fdk_realloc(). `ptr == NULL` is a safe no-op. */
void fdk_free(void *ptr);

/* Resizes a previous fdk_alloc*() allocation. On failure returns NULL
 * and the original block pointed to by `ptr` is left untouched (it is
 * NOT freed) — this matches realloc()'s standard contract. */
void *fdk_realloc(void *ptr, size_t new_size);

#endif /* FDK_ALLOC_INTERNAL_H */
